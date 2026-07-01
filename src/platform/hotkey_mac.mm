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
 */

#include "hotkey.h"
#include "window.h"

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <atomic>
#include <iostream>

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

    // cmdKey/shiftKey are the classic Carbon Events.h modifier masks used
    // specifically with RegisterEventHotKey — NOT the same constants as
    // NSEvent's modifierFlags. kVK_ANSI_V is the virtual key code for 'V' on a
    // US keyboard layout (Cmd+Shift+V, matching the project's chosen default).
    OSStatus regStatus = RegisterEventHotKey(
        kVK_ANSI_V, cmdKey | shiftKey, hotKeyID,
        GetApplicationEventTarget(), 0, &hotKeyRef);

    if (regStatus != noErr)
    {
        std::cerr << "[Hotkey] Failed to register global hotkey (OSStatus "
                  << regStatus << "). The popup hotkey will not work; the "
                  << "daemon continues running without it.\n";
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
                std::cout << "[Hotkey] Cmd+Shift+V pressed! Toggling popup...\n"
                          << std::flush;
                PopupWindow::toggle(*g_hotkeyManager);
            }
            return noErr;
        },
        1, &eventType, nullptr, nullptr);

    std::cout << "[Hotkey] Registered Cmd+Shift+V. Listening...\n"
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
