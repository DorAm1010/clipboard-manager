#pragma once

#include <string>

/**
 * @brief Return the OS-standard directory for storing application data.
 *
 * Each platform has a conventional location:
 *   - macOS:   ~/Library/Application Support/ClipboardManager/
 *   - Linux:   $XDG_DATA_HOME/clipboard-manager/  (fallback: ~/.local/share/clipboard-manager/)
 *   - Windows: %APPDATA%\ClipboardManager\
 *
 * The directory is created if it does not already exist.
 *
 * @return Absolute path to the data directory (with trailing separator).
 */
std::string getDataDir();

/**
 * @brief Return the full path to the history file.
 *
 * Convenience wrapper: getDataDir() + "history.txt"
 */
std::string getHistoryFilePath();

/**
 * @brief Return the full path to the socket file (Unix) or named pipe (Windows).
 *
 * Convenience wrapper: getDataDir() + "clipboard-manager.sock" (Unix)
 * or getDataDir() + "clipboard-manager.pipe" (Windows)
 */
std::string getSocketPath();

/**
 * @brief Return the full path to the user config file (currently just the
 *        popup hotkey setting; extensible to more options later).
 *
 * Convenience wrapper: getDataDir() + "config.txt"
 */
std::string getConfigFilePath();

/**
 * @brief Read the configured global hotkey spec from the config file, e.g.
 *        a line reading "hotkey=cmd+shift+v".
 *
 * This only reads the raw text of the "hotkey" key — it has no idea what a
 * *valid* spec looks like, or how to turn it into a platform hotkey
 * registration. Each platform's hotkey_*.{mm,cpp} file owns that parsing,
 * since the modifier/keycode representations are entirely platform-specific
 * (Carbon's cmdKey/kVK_* vs. Win32's MOD_CONTROL vs. X11's masks). This
 * function is shared because the raw config-file format itself is the same
 * everywhere.
 *
 * @param defaultSpec  Returned as-is if the config file doesn't exist, can't
 *                     be read, or has no "hotkey" line.
 */
std::string getConfiguredHotkeySpec(const std::string &defaultSpec);
