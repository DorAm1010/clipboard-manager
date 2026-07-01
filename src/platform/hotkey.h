#pragma once

#include "ClipboardManager.h"

// Cross-platform declaration for the global-hotkey listener. Implemented on
// macOS (hotkey_mac.mm) and Linux (hotkey_linux.cpp) so far; Windows will get
// its own hotkey_win.cpp in a later step, and main.cpp only calls into this
// on the platforms where it's implemented (see the #if in main.cpp).
namespace Hotkey
{
    /**
     * @brief Register the global hotkey and block, pumping the platform event
     *        loop, until requestStop() is called (from any thread).
     *
     * This MUST run on the same thread that registered the hotkey — on macOS
     * that means the main thread, since Carbon/CoreFoundation event delivery
     * for a process-wide hotkey is tied to that thread's run loop.
     *
     * @param manager  The running ClipboardManager. A reference is kept for
     *                 the lifetime of the event loop so the hotkey handler can
     *                 show the popup window with the manager's current history.
     */
    void runEventLoop(ClipboardManager &manager);

    /**
     * @brief Unblock a runEventLoop() call on another thread. Safe to call
     *        from a signal handler or any other thread.
     */
    void requestStop();
}
