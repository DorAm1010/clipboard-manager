/**
 * @file window_mac.mm
 * @brief macOS popup window shown/hidden by the global hotkey.
 *
 * Compiled only on macOS (added to CORE_SOURCES only under `if(APPLE)` in
 * CMakeLists.txt).
 *
 * STEP 6 SCOPE: a search field at the top of the popup, filtering live as you
 * type (via ClipboardManager::search() — case-insensitive substring match).
 *
 * The TABLE, not the search field, is the default focus target when the
 * popup opens — arrows/Enter/click all work directly on it immediately, the
 * same as before there was a search field at all. Pressing "s" (no
 * modifiers, checked by actual character so it works across keyboard
 * layouts) while the table has focus switches focus to the search field to
 * start a query; its placeholder text ("click 's' to search for an entry")
 * hints at this. Once the search field itself has focus, "s" is just a
 * normal character like any other.
 *
 * Even with the search field focused, Up/Down (move selection), Return
 * (paste selected), and Escape (dismiss) still work exactly as when the
 * table has focus — see CBMSearchFieldDelegate's
 * control:textView:doCommandBySelector: below, the standard AppKit mechanism
 * for "a text field that also drives a list." Every other key (regular
 * typing, Left/Right/Backspace for cursor movement) behaves as normal text
 * editing.
 *
 * While a search query is active, pagination is suspended: all matches are
 * shown in one scrollable list (no paging), since narrowing a search further
 * is just typing more, not clicking through pages. Left/Right therefore only
 * switch pages when the TABLE has keyboard focus (the default state) and
 * have no special meaning while actively searching, where they do normal
 * cursor movement. Both the search field and the current page reset on every
 * fresh open, so you never reopen the popup and see a stale query or page.
 *
 * Row activation is now content-based (activateRow/pasteEntryByContent), not
 * index-based: search results have no direct position in the full history,
 * so both the paginated view and the search view populate a parallel
 * "rawContents" array the table's click/Enter handler looks up by row and
 * hands to ClipboardManager::pasteEntryByContent(). This also incidentally
 * removes the earlier "stale index" race noted in Steps 4/5 — a row now
 * always maps to "the entry with this exact text," which stays correct even
 * if the background poller changes history while the popup is open.
 *
 * Selecting a row copies that entry to the system clipboard, promotes it to
 * the front of history (see ClipboardManager::pasteEntryByContent — "most
 * recently used" applies to selecting/pasting, not just to fresh copies),
 * closes the popup, and — if this app has been granted Accessibility
 * permission — synthesizes a Cmd+V keystroke so it's actually pasted into
 * whatever app was frontmost, with no extra keystroke from the user.
 *
 * Why Accessibility permission is required for auto-paste (unlike the hotkey
 * registration in hotkey_mac.mm, which needs none): injecting a keystroke
 * that another app receives as if the user typed it is a more sensitive
 * operation than registering to receive our OWN hotkey, and macOS gates it
 * behind Accessibility. AXIsProcessTrustedWithOptions (called once, at daemon
 * startup — see ensureAccessibilityPermission()) triggers the system's
 * one-time "clipboard-manager would like to control this computer" dialog if
 * not already granted. If permission is NOT granted, we degrade gracefully:
 * the entry is still written to the clipboard (so the user can paste it
 * manually), we just skip the synthesized keystroke.
 *
 * Click outside to dismiss: implemented via NSWindowDidResignKeyNotification
 * (see getOrCreatePanel()) rather than requiring Escape or a second hotkey
 * press.
 *
 * Implementation notes:
 *   - NSPanel with NSWindowStyleMaskNonactivatingPanel: the panel can become
 *     the *key* window (so it receives keyboard events) WITHOUT making our
 *     (invisible, accessory-policy) app become the system's *active*
 *     application. This is the same "doesn't interrupt whatever you were
 *     doing" behavior Spotlight and similar quick-palette tools use — it's
 *     also why there is no "restore focus to the previous app" step here:
 *     that app was never displaced from being frontmost/active in the first
 *     place, so there's nothing to hand back.
 *   - NSFloatingWindowLevel keeps the panel above ordinary application
 *     windows, so it's never accidentally hidden behind something else.
 *   - A tiny NSPanel subclass overrides cancelOperation: — AppKit routes the
 *     Escape key there automatically when nothing else claims it first (used
 *     when the TABLE has focus; the search field's Escape is handled by its
 *     own delegate instead, since a first responder's doCommandBySelector:
 *     is consulted before the window's cancelOperation: fallback).
 *   - A tiny NSTableView subclass overrides keyDown: to activate the selected
 *     row on Return/Enter and switch pages on Left/Right when the table
 *     itself has focus; every other key falls through to NSTableView's own
 *     default handling (arrow-key row navigation), no extra code needed.
 */

#include "window.h"

#import <Cocoa/Cocoa.h>
#include <ApplicationServices/ApplicationServices.h> // AXIsProcessTrustedWithOptions, CGEventPost
#include <Carbon/Carbon.h>                           // kVK_Return / kVK_ANSI_KeypadEnter / kVK_ANSI_V / kVK_LeftArrow / kVK_RightArrow
#include <algorithm>
#include <string>

// Forward-declared here so the C++ helpers below (which reference these
// types) can see them; all classes' @implementation blocks appear further
// down, after the C++ helpers they call — Objective-C only needs the
// @interface visible at the point of use, not the @implementation.
@interface CBMPopupPanel : NSPanel
@end

@interface CBMHistoryTableView : NSTableView
@end

namespace
{
    CBMPopupPanel *g_panel = nil;
    CBMHistoryTableView *g_tableView = nil;
    NSTextField *g_statusLabel = nil; // "Page X of Y" or "N matches for ..."
    NSSearchField *g_searchField = nil;
    id g_dataSource = nil; // CBMHistoryDataSource*; see below

    // The manager the currently-displayed history came from. Set each time
    // PopupWindow::toggle() runs; read later, asynchronously, when the user
    // clicks a row, presses Enter, switches pages, or types a search query
    // (AppKit invokes those callbacks well after toggle() has returned, so a
    // stored pointer — the same pattern used for g_hotkeyManager in
    // hotkey_mac.mm — is required here too).
    ClipboardManager *g_manager = nullptr;

    // Entries shown per page, and which page (0-based) is currently displayed.
    // Matches the project's agreed pagination spec: 10/page, 5 pages, 50 max
    // (the 50 cap comes from ClipboardManager(50, 500) in main.cpp). Only
    // meaningful when g_searchQuery is empty — see refreshHistory().
    constexpr size_t kPageSize = 10;
    size_t g_currentPage = 0;

    // Current search field text, kept in sync via CBMSearchFieldDelegate's
    // controlTextDidChange:. Empty means "not searching" — show the
    // paginated full history instead of search results.
    std::string g_searchQuery;

    size_t totalPages(size_t totalEntries)
    {
        // Ceiling division, clamped to at least 1 so an empty history still
        // shows "Page 1 of 1" rather than "Page 1 of 0".
        return std::max<size_t>(1, (totalEntries + kPageSize - 1) / kPageSize);
    }

    // Format one history entry for a table row. AppKit truncates long text
    // itself via the column's line-break mode, so — unlike the CLI's
    // truncate() — no manual byte-truncation is needed here. Embedded
    // newlines are still collapsed so one entry never visually spans
    // multiple rows.
    NSString *formatEntry(const ClipboardEntry &entry)
    {
        std::string collapsed;
        collapsed.reserve(entry.content.size());
        for (char c : entry.content)
        {
            collapsed += (c == '\n' || c == '\r') ? ' ' : c;
        }

        return [NSString stringWithUTF8String:collapsed.c_str()];
    }

    // Forward declaration; defined after getOrCreatePanel() below, but
    // referenced by the ObjC classes' @implementation blocks (which appear
    // before it in this file) via changePage()/the search delegate.
    void refreshHistory(ClipboardManager &manager);

    // Sends the system-wide "Cmd+V" keystroke via CGEventPost, as if the user
    // had physically pressed it. This only actually affects other apps if
    // this process has been granted Accessibility permission (checked by the
    // caller before calling this).
    void synthesizePasteKeystroke()
    {
        CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

        CGEventRef keyDown = CGEventCreateKeyboardEvent(source, kVK_ANSI_V, true);
        CGEventSetFlags(keyDown, kCGEventFlagMaskCommand);
        CGEventRef keyUp = CGEventCreateKeyboardEvent(source, kVK_ANSI_V, false);
        CGEventSetFlags(keyUp, kCGEventFlagMaskCommand);

        CGEventPost(kCGHIDEventTap, keyDown);
        CGEventPost(kCGHIDEventTap, keyUp);

        CFRelease(keyDown);
        CFRelease(keyUp);
        CFRelease(source);
    }

    // Copies the entry shown at on-screen `row` (looked up by its exact text
    // — see the file header for why this is content-based, not index-based)
    // to the system clipboard, closes the popup, and — if Accessibility
    // permission has been granted — auto-pastes it into whatever app was
    // frontmost.
    void activateRow(NSInteger row, NSArray<NSString *> *rawContents)
    {
        if (row < 0 || g_manager == nullptr || (NSUInteger)row >= rawContents.count)
        {
            // Includes the "(clipboard history is empty)" / "(no matches)"
            // placeholder rows, which have no corresponding rawContents entry.
            return;
        }

        std::string content([rawContents[(NSUInteger)row] UTF8String]);

        if (g_manager->pasteEntryByContent(content))
        {
            [g_panel orderOut:nil];

            // AXIsProcessTrusted() (no prompt argument) is a cheap, silent
            // check — the one-time PROMPTING check happens once at daemon
            // startup, see ensureAccessibilityPermission(). If permission
            // isn't granted, we've already written to the clipboard above, so
            // we degrade gracefully to "clipboard-only" rather than doing
            // nothing further.
            if (AXIsProcessTrusted())
            {
                // Give the WindowServer a brief moment to hand key/focus back
                // to the previously-frontmost app before sending the paste
                // keystroke — sending it in the same instant as orderOut: can
                // race the focus change and land in the wrong place (or
                // nowhere). 50ms is a pragmatic, commonly-used delay for
                // exactly this "hide overlay, then act on what's behind it"
                // pattern.
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.05 * NSEC_PER_SEC)),
                               dispatch_get_main_queue(), ^{
                                   synthesizePasteKeystroke();
                               });
            }
        }
        // pasteEntryByContent() returning false means the underlying entry
        // vanished between being shown and being activated (e.g. evicted by
        // the 50-entry cap) — an accepted, rare edge case. Leave the popup
        // open in that case.
    }

    // Convenience overload: looks up the currently-selected table row's
    // rawContents from the data source. Declared after CBMHistoryDataSource's
    // @interface further down is visible, so its body is defined later in
    // this file (see the second anonymous-namespace block).
    void activateSelectedRow();

    // Switches page by `delta` (+1 = next, -1 = previous), clamped to
    // [0, totalPages-1] — no wraparound, since jumping from the last page
    // straight back to the first (or vice versa) is more likely to confuse
    // than help. Only meaningful when not searching (see refreshHistory());
    // called while a search is active it still runs, but refreshHistory()
    // ignores g_currentPage in that case, so it has no visible effect.
    void changePage(ClipboardManager &manager, int delta)
    {
        auto entries = manager.history();
        size_t pages = totalPages(entries.size());

        // Signed arithmetic so a -1 delta from page 0 doesn't wrap around
        // through size_t's unsigned range.
        std::ptrdiff_t next = static_cast<std::ptrdiff_t>(g_currentPage) + delta;
        next = std::max<std::ptrdiff_t>(0, std::min(next, static_cast<std::ptrdiff_t>(pages) - 1));

        g_currentPage = static_cast<size_t>(next);
        refreshHistory(manager);
    }
}

void PopupWindow::ensureAccessibilityPermission()
{
    // kAXTrustedCheckOptionPrompt: true makes macOS show its one-time system
    // dialog ("clipboard-manager would like to control this computer") and
    // adds this app (unchecked) to System Settings > Privacy & Security >
    // Accessibility if it isn't listed yet. Called once at daemon startup so
    // the user sees this immediately rather than being surprised mid-click.
    NSDictionary *options = @{(__bridge id)kAXTrustedCheckOptionPrompt : @YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

@implementation CBMPopupPanel
- (void)cancelOperation:(id)sender
{
    (void)sender;
    [self orderOut:nil];
}
@end

@implementation CBMHistoryTableView
- (void)keyDown:(NSEvent *)event
{
    if (event.keyCode == kVK_Return || event.keyCode == kVK_ANSI_KeypadEnter)
    {
        activateSelectedRow();
        return;
    }
    if (event.keyCode == kVK_LeftArrow)
    {
        if (g_manager != nullptr)
        {
            changePage(*g_manager, -1);
        }
        return;
    }
    if (event.keyCode == kVK_RightArrow)
    {
        if (g_manager != nullptr)
        {
            changePage(*g_manager, +1);
        }
        return;
    }
    // "s" (no modifiers, so it doesn't steal e.g. Cmd+S) switches focus to
    // the search field to start a query. Checked via the actual character
    // produced (charactersIgnoringModifiers), not a specific virtual key
    // code, so it works correctly across keyboard layouts.
    NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    BOOL noModifiers = (mods & ~NSEventModifierFlagShift) == 0;
    if (noModifiers && ([event.charactersIgnoringModifiers isEqualToString:@"s"] ||
                        [event.charactersIgnoringModifiers isEqualToString:@"S"]))
    {
        [self.window makeFirstResponder:g_searchField];
        return;
    }
    // Every other key (notably Up/Down) falls through to NSTableView's own
    // default handling, which already implements arrow-key row navigation
    // with no extra code needed.
    [super keyDown:event];
}
@end

// Data source backing the table view. Holds pre-formatted display strings
// (rows) AND the corresponding unformatted original text (rawContents, same
// indices) — see the file header for why activation is content-based.
// Also serves as the table's click target: a single click on a row is
// treated the same as pressing Enter on it (see the file header's
// paste-behavior note for why this is a "quick palette" style single-click
// activation rather than requiring a double-click).
@interface CBMHistoryDataSource : NSObject <NSTableViewDataSource>
@property(nonatomic, strong) NSArray<NSString *> *rows;
@property(nonatomic, strong) NSArray<NSString *> *rawContents;
- (void)rowActivated:(id)sender;
@end

@implementation CBMHistoryDataSource

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tableView
{
    (void)tableView;
    return (NSInteger)self.rows.count;
}

- (id)tableView:(NSTableView *)tableView
    objectValueForTableColumn:(NSTableColumn *)tableColumn
                          row:(NSInteger)row
{
    (void)tableView;
    (void)tableColumn;
    if (row < 0 || (NSUInteger)row >= self.rows.count)
    {
        return @"";
    }
    return self.rows[(NSUInteger)row];
}

- (void)rowActivated:(id)sender
{
    activateRow([sender clickedRow], self.rawContents);
}

@end

namespace
{
    // Defined here, now that CBMHistoryDataSource's @interface is visible —
    // see the forward declaration and comment further up.
    void activateSelectedRow()
    {
        CBMHistoryDataSource *ds = g_dataSource;
        activateRow([g_tableView selectedRow], ds.rawContents);
    }
}

// Delegate for the search field. Implements the standard AppKit pattern for
// "a text field that also drives a list": controlTextDidChange: re-filters
// on every keystroke, and control:textView:doCommandBySelector: intercepts
// just the four navigation commands the popup needs (Up/Down/Return/Escape)
// while letting every other key (typing, Left/Right/Backspace for cursor
// movement) fall through to normal text editing.
@interface CBMSearchFieldDelegate : NSObject <NSSearchFieldDelegate>
@end

@implementation CBMSearchFieldDelegate

- (void)controlTextDidChange:(NSNotification *)notification
{
    NSTextField *field = notification.object;
    g_searchQuery = std::string([[field stringValue] UTF8String]);
    if (g_manager != nullptr)
    {
        refreshHistory(*g_manager);
    }
}

- (BOOL)control:(NSControl *)control
                textView:(NSTextView *)textView
    doCommandBySelector:(SEL)commandSelector
{
    (void)control;
    (void)textView;

    if (commandSelector == @selector(moveDown:))
    {
        NSInteger row = [g_tableView selectedRow];
        NSInteger next = std::min(row + 1, [g_tableView numberOfRows] - 1);
        if (next >= 0)
        {
            [g_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)next]
                      byExtendingSelection:NO];
            [g_tableView scrollRowToVisible:next];
        }
        return YES;
    }
    if (commandSelector == @selector(moveUp:))
    {
        NSInteger row = [g_tableView selectedRow];
        NSInteger prev = std::max<NSInteger>(row - 1, 0);
        [g_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)prev]
                  byExtendingSelection:NO];
        [g_tableView scrollRowToVisible:prev];
        return YES;
    }
    if (commandSelector == @selector(insertNewline:))
    {
        activateSelectedRow();
        return YES;
    }
    if (commandSelector == @selector(cancelOperation:))
    {
        [g_panel orderOut:nil];
        return YES;
    }

    // Every other command (typing, moveLeft:/moveRight: for cursor movement,
    // deleteBackward:, etc.) falls through to the field's normal text editing.
    return NO;
}

@end

namespace
{
    // Lazily creates the panel (search field + table + status label) the
    // first time it's needed. NSApplication must already exist by this point
    // — Hotkey::runEventLoop() creates it before any hotkey can fire — so
    // [NSApplication sharedApplication] here just returns the existing
    // singleton, it does not create a second one.
    CBMPopupPanel *getOrCreatePanel()
    {
        if (g_panel != nil)
        {
            return g_panel;
        }

        NSRect frame = NSMakeRect(0, 0, 420, 320);
        NSUInteger styleMask = NSWindowStyleMaskTitled |
                               NSWindowStyleMaskNonactivatingPanel;

        CBMPopupPanel *panel = [[CBMPopupPanel alloc] initWithContentRect:frame
                                                                 styleMask:styleMask
                                                                   backing:NSBackingStoreBuffered
                                                                     defer:NO];
        [panel setTitle:@"Clipboard History"];
        [panel setLevel:NSFloatingWindowLevel];
        [panel setReleasedWhenClosed:NO]; // we reuse this instance; don't let orderOut: free it
        [panel setBecomesKeyOnlyIfNeeded:NO];

        // Dismiss when the user clicks outside the popup. Clicking anywhere
        // else (another window, another app, the desktop) makes that other
        // window key instead, which fires "did resign key" on this panel —
        // the standard AppKit signal for "focus moved away from me". This
        // does NOT fire just from showing the panel itself: "resigned key"
        // only means something ELSE became key, which is a different event
        // from this panel becoming key in the first place.
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
                                                           object:panel
                                                            queue:[NSOperationQueue mainQueue]
                                                       usingBlock:^(NSNotification *note)
                                                       {
                                                           (void)note;
                                                           [panel orderOut:nil];
                                                       }];

        // Container content view: search field (top), table (middle), and
        // status label (bottom) are siblings within it. AppKit's default
        // (non-flipped) coordinate system puts Y=0 at the BOTTOM of the view.
        NSView *container = [[NSView alloc] initWithFrame:frame];

        const CGFloat searchHeight = 28;
        const CGFloat labelHeight = 24;

        NSRect searchFrame = NSMakeRect(4, frame.size.height - searchHeight - 2,
                                        frame.size.width - 8, searchHeight);
        NSRect labelFrame = NSMakeRect(0, 0, frame.size.width, labelHeight);
        NSRect scrollFrame = NSMakeRect(0, labelHeight, frame.size.width,
                                        frame.size.height - searchHeight - labelHeight - 2);

        NSSearchField *searchField = [[NSSearchField alloc] initWithFrame:searchFrame];
        [searchField setPlaceholderString:@"click 's' to search for an entry"];
        [searchField setAutoresizingMask:NSViewWidthSizable];
        CBMSearchFieldDelegate *searchDelegate = [[CBMSearchFieldDelegate alloc] init];
        [searchField setDelegate:searchDelegate];
        // Keep the delegate alive for the process lifetime — associate it
        // with the field via objc_setAssociatedObject-free approach: a
        // strong static keeps a single instance around exactly like the
        // panel/table/etc. below.
        static CBMSearchFieldDelegate *retainedSearchDelegate = searchDelegate;
        (void)retainedSearchDelegate;
        g_searchField = searchField;

        NSTextField *statusLabel = [[NSTextField alloc] initWithFrame:labelFrame];
        [statusLabel setEditable:NO];
        [statusLabel setSelectable:NO];
        [statusLabel setBezeled:NO];
        [statusLabel setDrawsBackground:NO];
        [statusLabel setAlignment:NSTextAlignmentCenter];
        [statusLabel setTextColor:[NSColor secondaryLabelColor]];
        [statusLabel setAutoresizingMask:NSViewWidthSizable];
        g_statusLabel = statusLabel;

        NSScrollView *scrollView = [[NSScrollView alloc] initWithFrame:scrollFrame];
        [scrollView setHasVerticalScroller:YES];
        [scrollView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];

        CBMHistoryTableView *tableView = [[CBMHistoryTableView alloc] initWithFrame:scrollFrame];
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"content"];
        [column setTitle:@"Content"];
        [column setWidth:scrollFrame.size.width - 20];
        [tableView addTableColumn:column];
        [tableView setHeaderView:nil]; // no header row — a light popup, not a spreadsheet

        CBMHistoryDataSource *dataSource = [[CBMHistoryDataSource alloc] init];
        dataSource.rows = @[];
        dataSource.rawContents = @[];
        [tableView setDataSource:dataSource];

        // Single click activates the row (see file header for why this is
        // "quick palette" single-click, not double-click).
        [tableView setTarget:dataSource];
        [tableView setAction:@selector(rowActivated:)];

        g_dataSource = dataSource;

        [scrollView setDocumentView:tableView];
        [container addSubview:searchField];
        [container addSubview:scrollView];
        [container addSubview:statusLabel];
        [panel setContentView:container];

        g_panel = panel;
        g_tableView = tableView;
        return g_panel;
    }

    // Re-reads history/search results from the manager and refreshes the
    // table, updates the status label, and pre-selects the first row so
    // arrow keys / Enter work immediately without requiring an initial
    // click. Called whenever the displayed content should change: a fresh
    // open, after Left/Right switches pages, or after the search query
    // changes.
    //
    // When g_searchQuery is non-empty, shows ALL matches (via
    // ClipboardManager::search()) in one scrollable list — no pagination.
    // When empty, shows the current page of the full history, exactly as in
    // the pre-search-bar behavior.
    void refreshHistory(ClipboardManager &manager)
    {
        NSMutableArray<NSString *> *rows = [NSMutableArray array];
        NSMutableArray<NSString *> *rawContents = [NSMutableArray array];
        NSString *statusText;

        if (!g_searchQuery.empty())
        {
            auto results = manager.search(g_searchQuery); // std::vector<ClipboardEntry>, unordered by position, no cap

            if (results.empty())
            {
                [rows addObject:@"(no matches)"];
            }
            else
            {
                for (const auto &entry : results)
                {
                    [rows addObject:formatEntry(entry)];
                    [rawContents addObject:[NSString stringWithUTF8String:entry.content.c_str()]];
                }
            }

            statusText = [NSString stringWithFormat:@"%zu match%s",
                                                     results.size(), results.size() == 1 ? "" : "es"];
        }
        else
        {
            auto entries = manager.history(); // std::deque<ClipboardEntry>, newest first (MRU)
            size_t pages = totalPages(entries.size());

            if (entries.empty())
            {
                [rows addObject:@"(clipboard history is empty)"];
            }
            else
            {
                size_t offset = g_currentPage * kPageSize;
                size_t end = std::min(offset + kPageSize, entries.size());
                for (size_t i = offset; i < end; ++i)
                {
                    [rows addObject:formatEntry(entries[i])];
                    [rawContents addObject:[NSString stringWithUTF8String:entries[i].content.c_str()]];
                }
            }

            statusText = [NSString stringWithFormat:@"Page %zu of %zu  (← / →)",
                                                     g_currentPage + 1, pages];
        }

        [g_dataSource setRows:rows];
        [g_dataSource setRawContents:rawContents];
        [g_tableView reloadData];
        [g_tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                 byExtendingSelection:NO];
        [g_statusLabel setStringValue:statusText];
    }
}

void PopupWindow::toggle(ClipboardManager &manager)
{
    g_manager = &manager;

    CBMPopupPanel *panel = getOrCreatePanel();

    if ([panel isVisible])
    {
        [panel orderOut:nil];
    }
    else
    {
        g_currentPage = 0;   // every fresh open starts on page 1
        g_searchQuery.clear(); // ...and with no active search query
        [g_searchField setStringValue:@""];

        refreshHistory(manager);
        [panel center]; // re-center each time it's shown, in case display config changed
        [panel makeKeyAndOrderFront:nil];
        // Table is the default focus (not the search field) — arrows/Enter/
        // click work directly on it immediately. Press "s" to switch focus
        // to the search field and start a query (see CBMHistoryTableView's
        // keyDown:).
        [panel makeFirstResponder:g_tableView];
    }
}
