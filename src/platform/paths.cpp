#include "paths.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

std::string getDataDir()
{
    fs::path dir;

#if defined(__APPLE__)
    // macOS: ~/Library/Application Support/ClipboardManager/
    const char *home = std::getenv("HOME");
    if (home)
    {
        dir = fs::path(home) / "Library" / "Application Support" / "ClipboardManager";
    }

#elif defined(_WIN32)
    // Windows: %APPDATA%\ClipboardManager\   (e.g. C:\Users\you\AppData\Roaming\)
    const char *appdata = std::getenv("APPDATA");
    if (appdata)
    {
        dir = fs::path(appdata) / "ClipboardManager";
    }

#elif defined(__linux__)
    // Linux: $XDG_DATA_HOME/clipboard-manager/ or ~/.local/share/clipboard-manager/
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0')
    {
        dir = fs::path(xdg) / "clipboard-manager";
    }
    else
    {
        const char *home = std::getenv("HOME");
        if (home)
        {
            dir = fs::path(home) / ".local" / "share" / "clipboard-manager";
        }
    }
#endif

    // Fallback: current working directory (should not happen in practice).
    if (dir.empty())
    {
        dir = fs::current_path() / ".clipboard-manager";
    }

    // Create the directory tree if it doesn't exist yet.
    fs::create_directories(dir);

    return dir.string() + std::string(1, fs::path::preferred_separator);
}

std::string getHistoryFilePath()
{
    return getDataDir() + "history.txt";
}
