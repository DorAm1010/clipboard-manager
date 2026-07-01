/**
 * @file window_linux.cpp
 * @brief Linux popup window shown/hidden by the global hotkey, built with GTK3.
 *
 * Compiled only on Linux (added to CORE_SOURCES only under `elseif(UNIX)` in
 * CMakeLists.txt).
 *
 * Why GTK3 and not raw Xlib (unlike the "native per-OS, no new dependency"
 * approach used for macOS/window_mac.mm and planned for Windows):
 *   Raw Xlib has no built-in widgets at all — no list view, no text field, no
 *   scrollbar, not even proper anti-aliased text rendering. Cocoa/Win32 give
 *   the other two ports a table view and search field essentially for free;
 *   X11 at the Xlib level gives nothing comparable, so a from-scratch
 *   implementation would mean hand-rolling an entire mini widget toolkit
 *   (layout, focus, redraw, text shaping) for something every other Linux
 *   desktop app already gets from a toolkit. GTK3 is effectively a system
 *   library on most Linux desktops already (not a heavy new dependency the
 *   way Qt or Electron would be) and is the toolkit native GNOME/most Linux
 *   apps use, so this popup looks at home the same way NSPanel does on
 *   macOS. This tradeoff was discussed with and approved by the project
 *   owner before implementation.
 *
 * Feature parity with window_mac.mm — same behavior, translated to GTK:
 *   - GtkListBox (not GtkTreeView) as the list — single click activates a row
 *     by default (GtkListBox's activate-on-single-click defaults to TRUE),
 *     matching the "quick palette" single-click paste behavior. Enter/Space
 *     on a focused row also activates it via GtkListBoxRow's own default key
 *     bindings — no extra code needed for that, mirroring how NSTableView's
 *     arrow-key navigation needed no extra code on macOS either.
 *   - Row activation is content-based (activateRow/pasteEntryByContent), not
 *     index-based, for the same reason as macOS: search results have no
 *     direct position in the full history. g_rowRawContents is the parallel
 *     array of exact original text, indexed the same as the visible rows.
 *   - Left/Right change pages when the LIST has keyboard focus (the default
 *     state); Up/Down/Return/Escape are intercepted by the search entry's own
 *     key-press-event handler so they still work identically even while
 *     typing a query, exactly mirroring
 *     CBMSearchFieldDelegate::control:textView:doCommandBySelector: — every
 *     other key (typing, Left/Right/Backspace for cursor movement) falls
 *     through to GtkEntry's normal text editing.
 *   - "s" (no Ctrl/Alt/Super — Shift is allowed, same as macOS) switches
 *     focus to the search entry when the list has focus; once the search
 *     entry itself has focus, "s" is just a normal character.
 *   - Selecting a row promotes it to the front of history via
 *     ClipboardManager::pasteEntryByContent(), hides the window, and — if the
 *     X11 XTest extension is available — synthesizes a Ctrl+V keystroke so it
 *     lands in whatever window was focused before the popup opened.
 *   - Click outside to dismiss: the window's "focus-out-event" signal, the
 *     GTK/X11 analogue of NSWindowDidResignKeyNotification.
 *   - Both the search query and the current page reset on every fresh open,
 *     and the list (not the search entry) is the default focus target.
 *
 * Why no permission gate is needed for auto-paste (unlike macOS's
 * Accessibility prompt in window_mac.mm): XTestFakeKeyEvent has no
 * counterpart to macOS's TCC/Accessibility permission system — any X11
 * client can synthesize input for the local X session by default.
 * PopupWindow::ensureAccessibilityPermission() is therefore a genuine no-op
 * here, kept only so main.cpp/hotkey_linux.cpp can call it unconditionally
 * across platforms without an #ifdef at the call site.
 *
 * Wayland caveat: XTestFakeKeyEvent only reaches X11/XWayland clients, the
 * same limitation noted in hotkey_linux.cpp for the hotkey grab itself — this
 * file inherits that constraint rather than introducing a new one. If GTK
 * itself fails to initialize (e.g. no display at all, a headless server),
 * PopupWindow::toggle() is simply never called — see hotkey_linux.cpp's
 * gtk_init_check() guard — so the daemon and the raw hotkey grab keep working
 * even with no GUI available.
 *
 * Threading and event loop integration: unlike a typical GTK application,
 * this process does not call gtk_main(). hotkey_linux.cpp owns the single
 * event loop (a select() call watching both the raw X11 hotkey connection
 * and GDK's own X11 connection) and pumps pending GTK events with
 * gtk_main_iteration() each time it wakes — see that file for the full
 * design note. Every function in this file therefore runs on that same
 * (main) thread; there is no cross-thread GTK access to guard against.
 */

#include "window.h"

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    GtkWidget *g_window = nullptr;
    GtkWidget *g_listBox = nullptr;
    GtkWidget *g_searchEntry = nullptr;
    GtkWidget *g_statusLabel = nullptr;
    GtkWidget *g_scrolledWindow = nullptr;
    bool g_windowBuilt = false;

    // Whether the X11 server this GDK connection talks to supports the XTest
    // extension. Checked once, lazily, the first time the window is built.
    // If false, entries are still written to the clipboard (via
    // pasteEntryByContent) — we just skip the synthesized keystroke, the same
    // "degrade to clipboard-only" fallback macOS uses when Accessibility
    // permission hasn't been granted.
    bool g_xtestAvailable = false;

    // The manager the currently-displayed history came from. Set each time
    // PopupWindow::toggle() runs; read later, asynchronously, from GTK signal
    // callbacks well after toggle() has returned — the same pattern
    // g_hotkeyManager uses in hotkey_mac.mm/hotkey_linux.cpp.
    ClipboardManager *g_manager = nullptr;

    // Entries shown per page, and which page (0-based) is currently
    // displayed. Matches the project's agreed pagination spec: 10/page, 5
    // pages, 50 max (the cap comes from ClipboardManager(50, 500) in
    // main.cpp). Only meaningful when g_searchQuery is empty — see
    // refreshHistory().
    constexpr size_t kPageSize = 10;
    size_t g_currentPage = 0;

    // Current search entry text, kept in sync via on_search_changed(). Empty
    // means "not searching" — show the paginated full history instead.
    std::string g_searchQuery;

    // Parallel array to the visible GtkListBox rows: g_rowRawContents[i] is
    // the exact original text of the entry shown at row i, used to activate
    // by content rather than position (see file header). A placeholder row
    // ("(clipboard history is empty)" / "(no matches)") has no corresponding
    // entry here, so activateRow() on it is naturally a no-op (index out of
    // range).
    std::vector<std::string> g_rowRawContents;

    size_t totalPages(size_t totalEntries)
    {
        // Ceiling division, clamped to at least 1 so an empty history still
        // shows "Page 1 of 1" rather than "Page 1 of 0".
        return std::max<size_t>(1, (totalEntries + kPageSize - 1) / kPageSize);
    }

    // Format one history entry for a list row. Embedded newlines are
    // collapsed to a space so one entry never visually spans multiple rows;
    // GtkLabel's ellipsize handles overly long single-line text.
    std::string formatEntry(const ClipboardEntry &entry)
    {
        std::string collapsed;
        collapsed.reserve(entry.content.size());
        for (char c : entry.content)
        {
            collapsed += (c == '\n' || c == '\r') ? ' ' : c;
        }
        return collapsed;
    }

    // Sends a system-wide Ctrl+V via XTestFakeKeyEvent, as if the user had
    // physically pressed it. Only called when g_xtestAvailable is true.
    void synthesizePasteKeystroke()
    {
        GdkDisplay *gdkDisplay = gdk_display_get_default();
        if (gdkDisplay == nullptr)
        {
            return;
        }
        Display *display = GDK_DISPLAY_XDISPLAY(gdkDisplay);

        KeyCode ctrlKey = XKeysymToKeycode(display, XK_Control_L);
        KeyCode vKey = XKeysymToKeycode(display, XK_v);
        if (ctrlKey == 0 || vKey == 0)
        {
            return;
        }

        XTestFakeKeyEvent(display, ctrlKey, True, 0);
        XTestFakeKeyEvent(display, vKey, True, 0);
        XTestFakeKeyEvent(display, vKey, False, 0);
        XTestFakeKeyEvent(display, ctrlKey, False, 0);
        XFlush(display);
    }

    gboolean synthesizePasteTimeoutCallback(gpointer)
    {
        synthesizePasteKeystroke();
        return G_SOURCE_REMOVE;
    }

    // Copies the entry shown at row `index` (looked up by its exact text —
    // see the file header for why this is content-based, not index-based) to
    // the system clipboard, hides the popup, and — if the XTest extension is
    // available — auto-pastes it into whatever window was focused before the
    // popup opened.
    void activateRow(int index)
    {
        if (index < 0 || g_manager == nullptr ||
            static_cast<size_t>(index) >= g_rowRawContents.size())
        {
            // Includes the "(clipboard history is empty)" / "(no matches)"
            // placeholder row, which has no corresponding rawContents entry.
            return;
        }

        const std::string content = g_rowRawContents[static_cast<size_t>(index)];

        if (g_manager->pasteEntryByContent(content))
        {
            gtk_widget_hide(g_window);

            if (g_xtestAvailable)
            {
                // Give the window manager a brief moment to hand focus back
                // to the previously-focused window before sending the paste
                // keystroke — sending it in the same instant as hide() can
                // race the focus change. 50ms matches the delay
                // window_mac.mm uses for the same "hide overlay, then act on
                // what's behind it" pattern.
                g_timeout_add(50, synthesizePasteTimeoutCallback, nullptr);
            }
        }
        // pasteEntryByContent() returning false means the underlying entry
        // vanished between being shown and being activated (e.g. evicted by
        // the 50-entry cap) — an accepted, rare edge case. Leave the popup open.
    }

    // Forward declaration; refreshHistory() is defined after the GTK signal
    // handlers below but referenced by changePage(), which those handlers call.
    void refreshHistory(ClipboardManager &manager);

    // Grabs GTK keyboard focus on the currently-selected ROW widget, not the
    // GtkListBox container. gtk_widget_grab_focus() on the container itself
    // does not reliably move keyboard focus away from whatever GTK's
    // default focus-on-map picked (typically the first focusable widget in
    // the window, i.e. the search entry) — GtkListBoxRow is the actual
    // focusable unit here, so it must be targeted directly. Also needed
    // after every refreshHistory() call made while the list has focus:
    // refreshHistory() destroys and recreates all row widgets, which would
    // otherwise silently drop keyboard focus (the previously-focused row no
    // longer exists) and break further list-driven key handling.
    void focusSelectedRow()
    {
        GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_listBox));
        if (selected != nullptr)
        {
            gtk_widget_grab_focus(GTK_WIDGET(selected));
        }
        else
        {
            gtk_widget_grab_focus(g_listBox);
        }
    }

    // Switches page by `delta` (+1 = next, -1 = previous), clamped to
    // [0, totalPages-1] — no wraparound. Only meaningful when not searching
    // (see refreshHistory()); called while a search is active it still runs,
    // but refreshHistory() ignores g_currentPage in that case, so it has no
    // visible effect. Only ever invoked from on_listbox_key_press(), i.e.
    // only while the list has keyboard focus, so re-focusing the (freshly
    // recreated) selected row after refreshHistory() is always correct here.
    void changePage(ClipboardManager &manager, int delta)
    {
        auto entries = manager.history();
        size_t pages = totalPages(entries.size());

        std::ptrdiff_t next = static_cast<std::ptrdiff_t>(g_currentPage) + delta;
        next = std::max<std::ptrdiff_t>(0, std::min(next, static_cast<std::ptrdiff_t>(pages) - 1));

        g_currentPage = static_cast<size_t>(next);
        refreshHistory(manager);
        focusSelectedRow();
    }

    // Moves the list's selection by `delta` without moving GTK keyboard
    // focus off the search entry — mirrors CBMSearchFieldDelegate's
    // moveUp:/moveDown: handling, which likewise moves the table's selection
    // while keeping the search field itself focused.
    void moveSelection(int delta)
    {
        GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_listBox));
        int current = selected ? gtk_list_box_row_get_index(selected) : -1;
        int next = std::max(0, current + delta);

        GtkListBoxRow *nextRow = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_listBox), next);
        if (nextRow == nullptr)
        {
            // Past the last row (or the list is empty) — stay on the current
            // row rather than losing selection entirely.
            nextRow = selected;
        }
        if (nextRow == nullptr)
        {
            return;
        }

        gtk_list_box_select_row(GTK_LIST_BOX(g_listBox), nextRow);

        GtkAllocation alloc;
        gtk_widget_get_allocation(GTK_WIDGET(nextRow), &alloc);
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_scrolledWindow));
        gtk_adjustment_clamp_page(vadj, alloc.y, alloc.y + alloc.height);
    }

    // Grabs GTK keyboard focus on the currently-selected ROW widget, not the
    void addRow(const std::string &label)
    {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_start(lbl, 4);
        gtk_widget_set_margin_end(lbl, 4);
        gtk_widget_set_margin_top(lbl, 2);
        gtk_widget_set_margin_bottom(lbl, 2);
        gtk_container_add(GTK_CONTAINER(row), lbl);
        gtk_list_box_insert(GTK_LIST_BOX(g_listBox), row, -1);
    }

    // --- GTK signal handlers ------------------------------------------------

    void on_listbox_row_activated(GtkListBox *, GtkListBoxRow *row, gpointer)
    {
        if (row != nullptr)
        {
            activateRow(gtk_list_box_row_get_index(row));
        }
    }

    // Handles Escape/Left/Right/"s" when the LIST has keyboard focus. Every
    // other key (notably Up/Down/Enter/Space) returns FALSE so GtkListBox's
    // own default key bindings handle it (row navigation, row activation) —
    // no extra code needed, mirroring how unhandled keys in
    // CBMHistoryTableView::keyDown: fall through to [super keyDown:].
    gboolean on_listbox_key_press(GtkWidget *, GdkEventKey *event, gpointer)
    {
        if (event->keyval == GDK_KEY_Escape)
        {
            gtk_widget_hide(g_window);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Left)
        {
            if (g_manager != nullptr)
            {
                changePage(*g_manager, -1);
            }
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Right)
        {
            if (g_manager != nullptr)
            {
                changePage(*g_manager, +1);
            }
            return TRUE;
        }

        // "s"/"S" (Shift allowed, Ctrl/Alt/Super not) switches focus to the
        // search entry, same modifier policy as window_mac.mm's keyDown:.
        const guint blockingMods = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK);
        if (blockingMods == 0 && (event->keyval == GDK_KEY_s || event->keyval == GDK_KEY_S))
        {
            gtk_widget_grab_focus(g_searchEntry);
            return TRUE;
        }

        return FALSE;
    }

    void on_search_changed(GtkEditable *, gpointer)
    {
        const gchar *text = gtk_entry_get_text(GTK_ENTRY(g_searchEntry));
        g_searchQuery = (text != nullptr) ? text : "";
        if (g_manager != nullptr)
        {
            refreshHistory(*g_manager);
        }
    }

    // Intercepts Up/Down/Return/Escape while the search entry has focus, the
    // same four commands CBMSearchFieldDelegate's
    // control:textView:doCommandBySelector: intercepts on macOS. Every other
    // key (typing, Left/Right/Backspace for cursor movement) returns FALSE
    // and falls through to GtkEntry's normal text editing.
    gboolean on_search_key_press(GtkWidget *, GdkEventKey *event, gpointer)
    {
        if (event->keyval == GDK_KEY_Escape)
        {
            gtk_widget_hide(g_window);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
        {
            GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_listBox));
            if (selected != nullptr)
            {
                activateRow(gtk_list_box_row_get_index(selected));
            }
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Up)
        {
            moveSelection(-1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Down)
        {
            moveSelection(+1);
            return TRUE;
        }
        return FALSE;
    }

    // Dismiss when the user clicks outside the popup — the GTK/X11 analogue
    // of NSWindowDidResignKeyNotification. Fires when focus moves to another
    // window (another app, the desktop, etc.), mediated by the window
    // manager; it does not fire just from showing the window in the first
    // place (that is a focus-IN event, not a resign).
    gboolean on_window_focus_out(GtkWidget *, GdkEvent *, gpointer)
    {
        gtk_widget_hide(g_window);
        return FALSE;
    }

    // Re-reads history/search results from the manager and refreshes the
    // list, updates the status label, and pre-selects the first row so arrow
    // keys/Enter work immediately without requiring an initial click. Called
    // whenever the displayed content should change: a fresh open, after
    // Left/Right switches pages, or after the search query changes.
    //
    // When g_searchQuery is non-empty, shows ALL matches (via
    // ClipboardManager::search()) in one scrollable list — no pagination.
    // When empty, shows the current page of the full history.
    void refreshHistory(ClipboardManager &manager)
    {
        // Clear any existing rows before rebuilding.
        GList *children = gtk_container_get_children(GTK_CONTAINER(g_listBox));
        for (GList *iter = children; iter != nullptr; iter = iter->next)
        {
            gtk_widget_destroy(GTK_WIDGET(iter->data));
        }
        g_list_free(children);
        g_rowRawContents.clear();

        std::string statusText;

        if (!g_searchQuery.empty())
        {
            auto results = manager.search(g_searchQuery); // unordered by position, no cap

            if (results.empty())
            {
                addRow("(no matches)");
            }
            else
            {
                for (const auto &entry : results)
                {
                    addRow(formatEntry(entry));
                    g_rowRawContents.push_back(entry.content);
                }
            }

            statusText = std::to_string(results.size()) + (results.size() == 1 ? " match" : " matches");
        }
        else
        {
            auto entries = manager.history(); // newest first (MRU)
            size_t pages = totalPages(entries.size());

            if (entries.empty())
            {
                addRow("(clipboard history is empty)");
            }
            else
            {
                size_t offset = g_currentPage * kPageSize;
                size_t end = std::min(offset + kPageSize, entries.size());
                for (size_t i = offset; i < end; ++i)
                {
                    addRow(formatEntry(entries[i]));
                    g_rowRawContents.push_back(entries[i].content);
                }
            }

            statusText = "Page " + std::to_string(g_currentPage + 1) + " of " +
                         std::to_string(pages) + "  (← / →)";
        }

        gtk_widget_show_all(g_listBox);
        gtk_label_set_text(GTK_LABEL(g_statusLabel), statusText.c_str());

        GtkListBoxRow *firstRow = gtk_list_box_get_row_at_index(GTK_LIST_BOX(g_listBox), 0);
        if (firstRow != nullptr)
        {
            gtk_list_box_select_row(GTK_LIST_BOX(g_listBox), firstRow);
        }
    }

    // Lazily builds the window (search entry + list + status label) the
    // first time it's needed. GTK itself must already be initialized by this
    // point — hotkey_linux.cpp calls gtk_init_check() before the hotkey can
    // ever fire — so this only ever constructs widgets, never the toolkit.
    void buildWindow()
    {
        GdkDisplay *gdkDisplay = gdk_display_get_default();
        if (gdkDisplay != nullptr)
        {
            int eventBase = 0, errorBase = 0, majorVersion = 0, minorVersion = 0;
            g_xtestAvailable = XTestQueryExtension(GDK_DISPLAY_XDISPLAY(gdkDisplay),
                                                   &eventBase, &errorBase,
                                                   &majorVersion, &minorVersion);
        }

        GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "Clipboard History");
        gtk_window_set_default_size(GTK_WINDOW(window), 420, 320);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
        gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);

        g_signal_connect(window, "focus-out-event", G_CALLBACK(on_window_focus_out), nullptr);
        // If the window manager gives this window a close button, hide
        // rather than destroy it — we reuse this single instance for the
        // life of the daemon, the same as g_panel on macOS.
        g_signal_connect(window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), nullptr);

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 4);

        GtkWidget *searchEntry = gtk_search_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(searchEntry), "click 's' to search for an entry");
        g_signal_connect(searchEntry, "changed", G_CALLBACK(on_search_changed), nullptr);
        g_signal_connect(searchEntry, "key-press-event", G_CALLBACK(on_search_key_press), nullptr);

        GtkWidget *scrolled = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scrolled, TRUE);

        GtkWidget *listBox = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(listBox), GTK_SELECTION_SINGLE);
        g_signal_connect(listBox, "row-activated", G_CALLBACK(on_listbox_row_activated), nullptr);
        g_signal_connect(listBox, "key-press-event", G_CALLBACK(on_listbox_key_press), nullptr);
        gtk_container_add(GTK_CONTAINER(scrolled), listBox);

        GtkWidget *statusLabel = gtk_label_new("");
        gtk_widget_set_halign(statusLabel, GTK_ALIGN_CENTER);

        gtk_box_pack_start(GTK_BOX(vbox), searchEntry, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), statusLabel, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(window), vbox);

        g_window = window;
        g_listBox = listBox;
        g_searchEntry = searchEntry;
        g_statusLabel = statusLabel;
        g_scrolledWindow = scrolled;
        g_windowBuilt = true;
    }
}

void PopupWindow::ensureAccessibilityPermission()
{
    // No-op on Linux: XTestFakeKeyEvent (used for auto-paste, see
    // synthesizePasteKeystroke() above) has no permission-gate counterpart to
    // macOS's Accessibility/TCC system — any X11 client can synthesize input
    // for the local session by default. Kept as a real function (not an
    // #ifdef'd-out call at the site) purely so callers can invoke it
    // unconditionally across platforms.
}

void PopupWindow::toggle(ClipboardManager &manager)
{
    g_manager = &manager;

    if (!g_windowBuilt)
    {
        buildWindow();
    }

    if (gtk_widget_get_visible(g_window))
    {
        gtk_widget_hide(g_window);
    }
    else
    {
        g_currentPage = 0;    // every fresh open starts on page 1
        g_searchQuery.clear(); // ...and with no active search query
        gtk_entry_set_text(GTK_ENTRY(g_searchEntry), "");

        refreshHistory(manager);
        gtk_window_set_position(GTK_WINDOW(g_window), GTK_WIN_POS_CENTER);
        gtk_widget_show_all(g_window);
        gtk_window_present(GTK_WINDOW(g_window));
        // The list, not the search entry, is the default focus target —
        // arrows/Enter/click work directly on it immediately. Press "s" to
        // switch focus to the search entry and start a query (see
        // on_listbox_key_press()).
        focusSelectedRow();
    }
}
