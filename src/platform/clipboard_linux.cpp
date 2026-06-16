// clipboard_linux.cpp  –  Linux implementation using xclip via popen()
//
// This approach uses xclip (a common CLI tool) rather than linking directly
// against X11/Xlib, keeping the code simple and readable for learning.
//
// Prerequisites (Debian / Ubuntu):
//   sudo apt install xclip
//
// For a production app you'd use XFixesSelectSelectionInput() from libxfixes
// to receive events instead of polling, but popen+xclip is much easier to
// understand when you're starting out.

#if defined(__linux__)

#include "ClipboardManager.h"
#include <cstdio>       // popen, pclose, fread
#include <array>

std::string ClipboardManager::readClipboard()
{
    // popen() runs a shell command and gives us a FILE* to read its stdout.
    // "xclip -selection clipboard -o" prints the current clipboard content.
    FILE* pipe = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!pipe) {
        return {};
    }

    std::string result;
    std::array<char, 256> buffer{};

    // Read the command's output in 256-byte chunks until EOF.
    while (fread(buffer.data(), 1, buffer.size(), pipe) > 0) {
        result.append(buffer.data(), buffer.size());
    }

    pclose(pipe);
    return result;
}

#endif // __linux__
