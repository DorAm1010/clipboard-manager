/**
 * @file hotkey_mac.mm
 * @brief macOS global hotkey listener using the Carbon Event Manager.
 *
 * Compiled only on macOS (this file is only added to CORE_SOURCES under
 * `if(APPLE)` in CMakeLists.txt).
 *
 * Why Carbon's RegisterEventHotKey and not the more "modern" NSEvent global
 * monitor (addGlobalMonitorForEventsMatchingMask)?
 *   RegisterEventHotKey requires NO special permission — it works the moment
 *   the process starts. The NSEvent global-monitor approach requires the user
 *   to grant this app Accessibility (or Input Monitoring) permission in System
 *   Settings first, which is a much heavier first-run experience for a
 *   background daemon with no visible UI to explain why it needs that. Carbon
 *   is technically part of the legacy (mostly deprecated) Carbon framework,
 *   but RegisterEventHotKey specifically has no replacement and is still the
 *   standard mechanism every macOS hotkey utility uses for exactly this
 *   permission-free reason (e.g. the open-source MASShortcut/HotKey libraries
 *   wrap this same call).
 *
 * Why NSApplication is required (this was a real bug in the first version of
 * this file — leaving the note since it's a well-known, easy-to-miss trap):
 *   RegisterEventHotKey succeeding (OSStatus noErr) only means the Carbon
 *   Event Manager accepted the registration IN THIS PROCESS. The actual
 *   system-wide key routing goes through the WindowServer, which only talks to
 *   processes that have established a real GUI connection — which nothing
 *   does automatically for a bare command-line binary with no app bundle. The
 *   fix is the same one every menu-bar-style utility (Alfred, Rectangle, etc.)
 *   uses: create an NSApplication and set its activation policy to
 *   "Accessory" — no Dock icon, no app menu bar, but a full WindowServer
 *   connection, which is exactly the "light UI" this project wants.
 *
 * The handler (below) calls PopupWindow::toggle() to show/hide the popup and
 * render its current history — see window_mac.mm.
 *
 * Threading: runEventLoop() must be called on the thread that owns the
 * NSApplication / hotkey event delivery — on macOS that means the process's
 * main thread. requestStop() may be called from any other thread (e.g. the
 * SIGTERM signal handler) to unblock it.
 *
 * Configurable hotkey: the actual combination is read from the user config
 * file (see paths.h's getConfiguredHotkeySpec()) as a spec string like
 * "cmd+shift+v" — the default if unconfigured. parseHotkeySpec() below turns
 * that string into Carbon's modifier mask + virtual key code. Supported
 * modifiers: cmd/command, shift, option/alt, ctrl/control. Supported keys:
 * a-z, 0-9, and space — a deliberately bounded set (not the full keyboard)
 * that covers realistic hotkey choices without needing an exhaustive
 * virtual-keycode table. An unparseable or missing spec falls back to the
 * hardcoded default with a logged warning.
 */

#include "hotkey.h"
#include "window.h"
#include "platform/paths.h"

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <atomic>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Set by requestStop() (e.g. from the signal handler's thread) and polled
    // by a CFRunLoopTimer running on the main thread (see runEventLoop()).
    //
    // Design note: requestStop() deliberately does ONLY this atomic write —
    // it does not call into AppKit ([NSApp stop:...]) directly. AppKit calls
    // are not documented as safe to make from a signal handler's calling
    // context (signals can interrupt malloc/Objective-C runtime machinery
    // at an arbitrary point), and this project's signal handler already
    // favors "safe, simple flag write" over "do the real work here" for
    // exactly that reason. The CFRunLoopTimer below does the actual AppKit
    // work later, during normal (non-signal) execution on the main run loop.
    std::atomic<bool> g_stopRequested{false};

    // The Carbon hotkey callback below is a plain (capture-less) C function
    // so it can convert to the EventHandlerProcPtr the API requires — it
    // cannot capture anything. This file-scope pointer is how it reaches the
    // manager, the same pattern main.cpp already uses for its signal handler.
    ClipboardManager *g_hotkeyManager = nullptr;

    // A human-readable version of the currently-registered hotkey, purely for
    // the startup log line — set once by parseHotkeySpec()'s caller.
    std::string g_hotkeyDisplayName;

    // Maps a single lowercase letter/digit/space to its Carbon virtual key
    // code, using the SYMBOLIC kVK_* constants (not hand-transcribed hex
    // values, which would risk a silent transcription error the compiler
    // can't catch). Returns -1 for anything outside the supported set.
    int virtualKeyCodeForChar(char c)
    {
        switch (c)
        {
        case 'a': return kVK_ANSI_A;
        case 'b': return kVK_ANSI_B;
        case 'c': return kVK_ANSI_C;
        case 'd': return kVK_ANSI_D;
        case 'e': return kVK_ANSI_E;
        case 'f': return kVK_ANSI_F;
        case 'g': return kVK_ANSI_G;
        case 'h': return kVK_ANSI_H;
        case 'i': return kVK_ANSI_I;
        case 'j': return kVK_ANSI_J;
        case 'k': return kVK_ANSI_K;
        case 'l': return kVK_ANSI_L;
        case 'm': return kVK_ANSI_M;
        case 'n': return kVK_ANSI_N;
        case 'o': return kVK_ANSI_O;
        case 'p': return kVK_ANSI_P;
        case 'q': return kVK_ANSI_Q;
        case 'r': return kVK_ANSI_R;
        case 's': return kVK_ANSI_S;
        case 't': return kVK_ANSI_T;
        case 'u': return kVK_ANSI_U;
        case 'v': return kVK_ANSI_V;
        case 'w': return kVK_ANSI_W;
        case 'x': return kVK_ANSI_X;
        case 'y': return kVK_ANSI_Y;
        case 'z': return kVK_ANSI_Z;
        case '0': return kVK_ANSI_0;
        case '1': return kVK_ANSI_1;
        case '2': return kVK_ANSI_2;
        case '3': return kVK_ANSI_3;
        case '4': return kVK_ANSI_4;
        case '5': return kVK_ANSI_5;
        case '6': return kVK_ANSI_6;
        case '7': return kVK_ANSI_7;
        case '8': return kVK_ANSI_8;
        case '9': return kVK_ANSI_9;
        case ' ': return kVK_Space;
        default: return -1;
        }
    }

    struct HotkeySpec
    {
        UInt32 modifiers = 0;
        int keyCode = -1;
    };

    // Parses a spec string like "cmd+shift+v" into Carbon's modifier mask +
    // virtual key code. Case-insensitive; the last non-modifier token is
    // treated as the key. Returns keyCode == -1 (invalid) if the spec has no
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
            if (t == "cmd" || t == "command")
                result.modifiers |= cmdKey;
            else if (t == "shift")
                result.modifiers |= shiftKey;
            else if (t == "option" || t == "alt")
                result.modifiers |= optionKey;
            else if (t == "ctrl" || t == "control")
                result.modifiers |= controlKey;
            else if (t.size() == 1)
            {
                int vk = virtualKeyCodeForChar(t[0]);
                if (vk >= 0)
                {
                    result.keyCode = vk;
                }
            }
            // Unrecognized multi-character tokens (typos, unsupported key
            // names) are silently skipped; an invalid overall spec is caught
            // by the caller checking keyCode/modifiers afterward.
        }

        return result;
    }
}

void Hotkey::runEventLoop(ClipboardManager &manager)
{
    g_hotkeyManager = &manager;

    // Creates the NSApplication singleton and establishes the WindowServer
    // connection that global hotkey delivery depends on (see file header).
    [NSApplication sharedApplication];

    // Accessory: no Dock icon, no app menu bar. Right fit for a background
    // daemon that only ever shows a small popup on demand.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    // Trigger the one-time Accessibility permission prompt now (if not
    // already granted) so the user sees it right when the daemon starts,
    // rather than being surprised the first time they select a history
    // entry. See window_mac.mm for what this permission is used for.
    PopupWindow::ensureAccessibilityPermission();

    EventHotKeyRef hotKeyRef{};
    EventHotKeyID hotKeyID{};
    hotKeyID.signature = 'CBMH'; // arbitrary 4-char tag, unique to this app
    hotKeyID.id = 1;

    // Read the user's configured hotkey (see paths.h), falling back to the
    // hardcoded default if unconfigured, unparseable, or invalid.
    const std::string kDefaultSpec = "cmd+shift+v";
    std::string spec = getConfiguredHotkeySpec(kDefaultSpec);
    HotkeySpec parsed = parseHotkeySpec(spec);

    if (parsed.keyCode < 0 || parsed.modifiers == 0)
    {
        std::cerr << "[Hotkey] Invalid or unparseable hotkey config \"" << spec
                  << "\" — falling back to the default (" << kDefaultSpec << ").\n";
        spec = kDefaultSpec;
        parsed = parseHotkeySpec(spec);
    }
    g_hotkeyDisplayName = spec;

    OSStatus regStatus = RegisterEventHotKey(
        parsed.keyCode, parsed.modifiers, hotKeyID,
        GetApplicationEventTarget(), 0, &hotKeyRef);

    if (regStatus != noErr)
    {
        std::cerr << "[Hotkey] Failed to register global hotkey \"" << spec
                  << "\" (OSStatus " << regStatus << "). The popup hotkey will "
                  << "not work; the daemon continues running without it.\n";
        return;
    }

    EventTypeSpec eventType{};
    eventType.eventClass = kEventClassKeyboard;
    eventType.eventKind = kEventHotKeyPressed;
    InstallApplicationEventHandler(
        [](EventHandlerCallRef, EventRef theEvent, void *) -> OSStatus
        {
            EventHotKeyID hkID{};
            GetEventParameter(theEvent, kEventParamDirectObject, typeEventHotKeyID,
                               nullptr, sizeof(hkID), nullptr, &hkID);
            if (hkID.id == 1 && g_hotkeyManager != nullptr)
            {
                std::cout << "[Hotkey] " << g_hotkeyDisplayName << " pressed! Toggling popup...\n"
                          << std::flush;
                PopupWindow::toggle(*g_hotkeyManager);
            }
            return noErr;
        },
        1, &eventType, nullptr, nullptr);

    std::cout << "[Hotkey] Registered " << spec << ". Listening...\n"
              << std::flush;

    // If a stop was already requested before we got here, skip straight to
    // cleanup rather than entering [NSApp run] at all.
    if (g_stopRequested.load())
    {
        UnregisterEventHotKey(hotKeyRef);
        return;
    }

    // Poll g_stopRequested on the main run loop (same "check a flag on a
    // short timer" idiom already used for the IPC accept loop's shutdown
    // check in service_unix.cpp/service_win.cpp). When it flips true, ask
    // NSApplication to stop, then post a no-op event to actually wake up
    // [NSApp run] — NSApp stop: only takes effect after the run loop
    // processes one more event, a well-known NSApplication quirk.
    CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.2, 0.2, 0, 0,
        ^(CFRunLoopTimerRef timerRef)
        {
            if (g_stopRequested.load())
            {
                [NSApp stop:nil];
                NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                    location:NSZeroPoint
                                               modifierFlags:0
                                                   timestamp:0
                                                windowNumber:0
                                                     context:nil
                                                     subtype:0
                                                       data1:0
                                                       data2:0];
                [NSApp postEvent:wake atStart:YES];
                CFRunLoopTimerInvalidate(timerRef);
            }
        });
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopCommonModes);

    [NSApp run]; // blocks until requestStop() flips the flag the timer polls

    CFRelease(timer);
    UnregisterEventHotKey(hotKeyRef);
}

void Hotkey::requestStop()
{
    g_stopRequested.store(true);
}
