/**
 * @file hotkey_linux.cpp
 * @brief Linux (X11) global hotkey listener using XGrabKey.
 *
 * Compiled only on Linux (this file is only added to CORE_SOURCES under
 * `elseif(UNIX)` in CMakeLists.txt).
 *
 * Why XGrabKey and not something higher-level:
 *   XGrabKey asks the X server itself to route one specific key+modifier
 *   combination to this client, system-wide, for as long as the grab is held
 *   — the direct X11 analogue of macOS's Carbon RegisterEventHotKey (see
 *   hotkey_mac.mm), and for the same reason: no permission prompt, no
 *   Accessibility-style dialog. X11's security model is far more permissive
 *   than macOS's TCC by default, so this works the moment the process opens a
 *   display connection.
 *
 * The NumLock/CapsLock trap (a well-known X11 gotcha, easy to miss):
 *   XGrabKey matches the modifier state EXACTLY. If we grab only Mod4Mask
 *   (Super), the grab silently never fires while NumLock or CapsLock happens
 *   to be toggled on, because the X server reports those as extra bits in the
 *   event's modifier state. The fix every X11 hotkey utility uses is to grab
 *   the same combination multiple times, once for each combination of the
 *   "ignored" lock modifiers being on or off (see ignoredMasks below).
 *
 * Wayland caveat (see the Linux implementation plan discussed with the user):
 *   This only works on X11 (or an XWayland-hosted session where the
 *   compositor still runs an X server for compatibility) — Wayland's security
 *   model deliberately blocks arbitrary clients from grabbing global keys.
 *   XOpenDisplay() failing is the signal we use to detect "no X11 available"
 *   and log a pointer to the Wayland workaround (a compositor-level custom
 *   shortcut invoking this binary) rather than crash.
 *
 * Threading: runEventLoop() blocks the calling thread pumping X11 events
 * until requestStop() (from any other thread) sets the atomic flag it polls.
 * There is no threading requirement tying the grab to a specific thread the
 * way Carbon/CoreFoundation does on macOS — X11 event delivery is tied to the
 * Display connection, not the OS thread — but we still run it on main()'s
 * daemon thread for consistency with the macOS design and to keep this
 * project's threading model uniform across platforms.
 *
 * Configurable hotkey: same spec-string format as macOS ("cmd+shift+v"),
 * read via paths.h's getConfiguredHotkeySpec() so the config FILE format
 * doesn't need to differ per platform — only the parsing of modifier names
 * into platform masks differs. "cmd"/"command" maps to Mod4Mask (the Super /
 * Windows / Meta key), the closest Linux equivalent of a Command key; "super"
 * / "meta" / "win" are also accepted as more natural Linux spellings of the
 * same modifier. Supported keys: a-z, 0-9, and space, via X11's
 * XStringToKeysym() — a general-purpose name-to-keysym lookup, so (unlike
 * Carbon's kVK_* constants) no hand-written per-character table is needed.
 *
 * GTK integration: unlike a typical GTK application, this file does not call
 * gtk_main(). It owns the single event loop for the whole daemon: a select()
 * call that watches BOTH the raw X11 connection used for the hotkey grab
 * above AND GDK's own X11 connection (opened by gtk_init_check() below), so
 * it wakes up promptly for either a hotkey press or a popup-window UI event
 * (click, keystroke), then pumps gtk_main_iteration() to let GTK actually
 * process whatever's pending — see the loop in runEventLoop() below. This
 * avoids pulling in GLib's g_io_add_watch main-loop machinery while still
 * interleaving both event sources correctly on one thread.
 *
 * gtk_init_check() (not plain gtk_init()) is used deliberately: gtk_init()
 * aborts the whole process if it can't open a display connection, which
 * would break the graceful degradation this file already provides when no
 * X11 is available at all (see the Wayland caveat above) — a real regression
 * risk introduced by adding GTK. If GTK fails to initialize, gtkReady stays
 * false, the hotkey grab and daemon keep working exactly as in STEP 1 (the
 * handler just logs), and PopupWindow::toggle() is simply never called.
 */

#include "hotkey.h"
#include "window.h"
#include "platform/paths.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <sys/select.h>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Set by requestStop() (e.g. from the signal handler's thread) and polled
    // by the select()-with-timeout loop in runEventLoop(). Same "atomic flag,
    // polled on a short timer" idiom already used for the IPC accept loop in
    // service_unix.cpp and for hotkey_mac.mm's CFRunLoopTimer.
    std::atomic<bool> g_stopRequested{false};

    // Set by the temporary X error handler installed around the XGrabKey
    // calls, so a failed grab (e.g. another client already owns this exact
    // combination) can be detected and reported instead of the default Xlib
    // error handler tearing down the whole process.
    std::atomic<bool> g_grabError{false};

    int hotkeyErrorHandler(Display *, XErrorEvent *)
    {
        g_grabError.store(true);
        return 0;
    }

    // Converts a single lowercase letter/digit/space to its X11 KeySym via
    // XStringToKeysym(), a general-purpose name lookup — unlike Carbon's
    // virtual key codes, X11 keysym names for a-z/0-9 are just the character
    // itself, so no hand-transcribed table is needed.
    KeySym keysymForChar(char c)
    {
        if (c == ' ')
        {
            return XStringToKeysym("space");
        }
        const std::string name(1, c);
        return XStringToKeysym(name.c_str());
    }

    struct HotkeySpec
    {
        unsigned int modifiers = 0;
        KeySym keysym = NoSymbol;
    };

    // Parses a spec string like "cmd+shift+v" into an X11 modifier mask + a
    // KeySym. Case-insensitive; the last recognized single-character token is
    // treated as the key. Returns keysym == NoSymbol if the spec has no
    // recognizable key, so the caller can detect and fall back to a default.
    HotkeySpec parseHotkeySpec(const std::string &spec)
    {
        HotkeySpec result;

        std::string lower = spec;
        for (char &c : lower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        std::istringstream iss(lower);
        std::vector<std::string> tokens;
        std::string token;
        while (std::getline(iss, token, '+'))
        {
            tokens.push_back(token);
        }

        for (const auto &t : tokens)
        {
            if (t == "cmd" || t == "command" || t == "super" || t == "meta" || t == "win")
                result.modifiers |= Mod4Mask;
            else if (t == "shift")
                result.modifiers |= ShiftMask;
            else if (t == "option" || t == "alt")
                result.modifiers |= Mod1Mask;
            else if (t == "ctrl" || t == "control")
                result.modifiers |= ControlMask;
            else if (t.size() == 1)
            {
                KeySym ks = keysymForChar(t[0]);
                if (ks != NoSymbol)
                {
                    result.keysym = ks;
                }
            }
            // Unrecognized multi-character tokens (typos, unsupported key
            // names) are silently skipped; an invalid overall spec is caught
            // by the caller checking keysym/modifiers afterward.
        }

        return result;
    }

    // Finds which modifier mask bit X11 has assigned to NumLock on this
    // keyboard, so runEventLoop() can grab the hotkey with and without it.
    // Returns 0 (no extra bit to account for) if NumLock isn't mapped to any
    // modifier, which is a valid and harmless outcome.
    unsigned int numLockMask(Display *display)
    {
        unsigned int mask = 0;
        XModifierKeymap *modmap = XGetModifierMapping(display);
        if (!modmap)
        {
            return 0;
        }

        KeyCode numLockCode = XKeysymToKeycode(display, XK_Num_Lock);
        if (numLockCode != 0)
        {
            for (int mod = 0; mod < 8; ++mod)
            {
                for (int k = 0; k < modmap->max_keypermod; ++k)
                {
                    if (modmap->modifiermap[mod * modmap->max_keypermod + k] == numLockCode)
                    {
                        mask = (1u << mod);
                    }
                }
            }
        }

        XFreeModifiermap(modmap);
        return mask;
    }
}

void Hotkey::runEventLoop(ClipboardManager &manager)
{
    Display *display = XOpenDisplay(nullptr);
    if (!display)
    {
        std::cerr << "[Hotkey] Could not open an X11 display — the popup hotkey will "
                     "not work. If you're on a Wayland session without XWayland, "
                     "global hotkeys need a compositor-level custom shortcut instead "
                     "(see the project docs). The daemon continues running without it.\n";
        return;
    }

    // gtk_init_check() opens GDK's own (separate) X11 connection and returns
    // false instead of aborting if it can't — see the file header for why
    // that matters here. A failure only disables the popup window; the raw
    // hotkey grab below is unaffected.
    int gtkArgc = 0;
    bool gtkReady = gtk_init_check(&gtkArgc, nullptr);
    if (!gtkReady)
    {
        std::cerr << "[Hotkey] GTK failed to initialize — the popup window will not be "
                     "shown, but the hotkey will still be registered and logged. The "
                     "daemon continues running.\n";
    }
    else
    {
        PopupWindow::ensureAccessibilityPermission();
    }

    const std::string kDefaultSpec = "cmd+shift+v";
    std::string spec = getConfiguredHotkeySpec(kDefaultSpec);
    HotkeySpec parsed = parseHotkeySpec(spec);

    if (parsed.keysym == NoSymbol || parsed.modifiers == 0)
    {
        std::cerr << "[Hotkey] Invalid or unparseable hotkey config \"" << spec
                  << "\" — falling back to the default (" << kDefaultSpec << ").\n";
        spec = kDefaultSpec;
        parsed = parseHotkeySpec(spec);
    }

    KeyCode keycode = XKeysymToKeycode(display, parsed.keysym);
    if (keycode == 0)
    {
        std::cerr << "[Hotkey] Could not map hotkey \"" << spec << "\" to a keycode on "
                     "this keyboard layout. The popup hotkey will not work; the daemon "
                     "continues running without it.\n";
        XCloseDisplay(display);
        return;
    }

    Window root = DefaultRootWindow(display);
    const unsigned int numLock = numLockMask(display);

    // Grab the combination once per combination of the ignorable lock
    // modifiers (see numLockMask()'s header comment) so the hotkey still
    // fires regardless of NumLock/CapsLock state.
    const unsigned int ignoredMasks[] = {0, LockMask, numLock, numLock | LockMask};

    g_grabError.store(false);
    XErrorHandler previousHandler = XSetErrorHandler(hotkeyErrorHandler);
    for (unsigned int ignored : ignoredMasks)
    {
        XGrabKey(display, keycode, parsed.modifiers | ignored, root,
                 False, GrabModeAsync, GrabModeAsync);
    }
    // XGrabKey is asynchronous — a rejected grab arrives as an X error event,
    // not a return value. XSync forces the server to flush any pending errors
    // to our handler before we check g_grabError.
    XSync(display, False);
    XSetErrorHandler(previousHandler);

    if (g_grabError.load())
    {
        std::cerr << "[Hotkey] Failed to register global hotkey \"" << spec
                  << "\" — it may already be grabbed by another application "
                     "(window manager, another instance, etc.). The daemon "
                     "continues running without it.\n";
        XCloseDisplay(display);
        return;
    }

    std::cout << "[Hotkey] Registered " << spec << ". Listening...\n"
              << std::flush;

    const int xfd = ConnectionNumber(display);
    // GDK's own X11 connection (separate from the raw one above), watched
    // too so popup-window UI events (clicks, keystrokes) get serviced
    // promptly rather than waiting for the periodic timeout below.
    const int gdkFd = gtkReady ? ConnectionNumber(GDK_DISPLAY_XDISPLAY(gdk_display_get_default())) : -1;

    while (!g_stopRequested.load())
    {
        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == KeyPress)
            {
                // Mask off the lock modifiers we deliberately grabbed on top
                // of, so the comparison below only looks at the modifiers the
                // user actually configured.
                const unsigned int relevantMods = event.xkey.state & ~(LockMask | numLock);
                if (event.xkey.keycode == keycode && relevantMods == parsed.modifiers)
                {
                    std::cout << "[Hotkey] " << spec << " pressed! Toggling popup...\n"
                              << std::flush;
                    if (gtkReady)
                    {
                        PopupWindow::toggle(manager);
                    }
                }
            }
        }

        if (gtkReady)
        {
            while (gtk_events_pending())
            {
                gtk_main_iteration();
            }
        }

        if (g_stopRequested.load())
        {
            break;
        }

        // Block on the X11 connection file descriptor(s) with a timeout, so
        // this loop stays idle (no busy-polling) but still wakes up
        // periodically to re-check g_stopRequested even with no events
        // arriving — same "poll a flag on a short timer" idiom used
        // elsewhere in this project (service_unix.cpp's IPC accept loop,
        // hotkey_mac.mm's CFRunLoopTimer).
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        int maxfd = xfd;
        if (gdkFd >= 0)
        {
            FD_SET(gdkFd, &fds);
            maxfd = std::max(maxfd, gdkFd);
        }
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        select(maxfd + 1, &fds, nullptr, nullptr, &tv);
    }

    for (unsigned int ignored : ignoredMasks)
    {
        XUngrabKey(display, keycode, parsed.modifiers | ignored, root);
    }
    XCloseDisplay(display);
}

void Hotkey::requestStop()
{
    g_stopRequested.store(true);
}
