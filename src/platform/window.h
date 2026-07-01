#pragma once

#include "ClipboardManager.h"

// Cross-platform declaration for the popup window (macOS-only so far, see
// window_mac.mm).
namespace PopupWindow
{
    /**
     * @brief Show the popup window if it is hidden (refreshing its content
     *        from the manager's current history first), hide it if visible.
     *
     * Must be called on the main thread (the same thread NSApplication and
     * the hotkey run loop live on — see hotkey_mac.mm).
     */
    void toggle(ClipboardManager &manager);

    /**
     * @brief Trigger macOS's one-time Accessibility permission prompt if this
     *        app hasn't been granted it yet. Needed for auto-paste (see
     *        window_mac.mm); harmless to call if permission is already
     *        granted (a no-op check in that case). Call once at daemon
     *        startup, before any hotkey can fire.
     */
    void ensureAccessibilityPermission();
}
