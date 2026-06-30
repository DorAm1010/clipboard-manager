#include "paths.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

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

    // This directory holds the clipboard history (which routinely contains
    // secrets) and the IPC socket, so restrict it to the owner only (POSIX
    // 0700). Done with std::filesystem to stay cross-platform; on Windows
    // owner-only access is governed by ACLs and this is a best-effort no-op.
    // Errors are intentionally ignored — a failure here must not stop startup.
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);

    return dir.string() + std::string(1, fs::path::preferred_separator);
}

std::string getHistoryFilePath()
{
    return getDataDir() + "history.txt";
}

std::string getSocketPath()
{
    return getDataDir() + "clipboard-manager.sock";
}
