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
