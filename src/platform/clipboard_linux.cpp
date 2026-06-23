/**
 * @file clipboard_linux.cpp
 * @brief Linux clipboard reader using xclip via popen().
 *
 * Compiled only on Linux (guarded by `#if defined(__linux__)`).
 *
 * There are two common approaches to reading the X11 clipboard from C++:
 *
 *   A. Link against libX11 and call XGetSelectionOwner / XConvertSelection
 *      directly. This is event-driven and efficient but requires 100+ lines
 *      of X11 boilerplate and an event loop.
 *
 *   B. Shell out to `xclip`, a small CLI tool that wraps the X11 protocol.
 *      This is a single popen() call — trivial to read and understand.
 *
 * We chose approach B for clarity. For a production daemon you would
 * prefer approach A (or use XFixesSelectSelectionInput() to receive
 * change events instead of polling), but B is the right starting point
 * when you are learning.
 *
 * Prerequisites:
 *   sudo apt install xclip    # Debian / Ubuntu
 *   sudo pacman -S xclip      # Arch
 */

#if defined(__linux__)

#include "ClipboardManager.h"
#include <cstdio> // popen(), pclose(), fread()
#include <array>  // std::array for the read buffer

/**
 * @brief Read the current plain-text content of the X11 clipboard.
 *
 * popen() forks a child process, runs the given shell command, and returns
 * a FILE* connected to the child's stdout. We drain that pipe in fixed-size
 * chunks and accumulate the result in a std::string.
 *
 * The shell command:
 *   xclip -selection clipboard -o
 *     -selection clipboard  → read the CLIPBOARD selection (Ctrl+C), not
 *                             the PRIMARY selection (mouse highlight)
 *     -o                    → output mode (print clipboard to stdout)
 *     2>/dev/null           → suppress xclip's error messages if the
 *                             clipboard is empty or holds non-text data
 *
 * popen()/pclose() is a convenience wrapper around fork()/exec()/waitpid().
 * We call pclose() at the end to reap the child process and avoid a zombie.
 *
 * @return The clipboard text as a std::string, or an empty string if xclip
 *         is not installed, the clipboard is empty, or it holds non-text.
 */
std::string ClipboardManager::readClipboard()
{
    // Launch xclip and open a read pipe to its stdout.
    // The "r" mode means we can only read from the pipe (as opposed to "w"
    // which would write to the child's stdin).
    FILE *pipe = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!pipe)
    {
        // popen() failed (e.g., xclip not found, fork() failed).
        return {};
    }

    std::string result;

    // Use a fixed-size stack buffer for reading. 256 bytes per read is a
    // reasonable chunk size — small enough to be stack-safe, large enough
    // that most clipboard texts are read in one or two passes.
    // The {} initializes all bytes to zero, which is good practice even
    // though fread() only writes to bytes it fills.
    std::array<char, 256> buffer{};

    // fread() reads up to buffer.size() bytes from the pipe into buffer.data()
    // and returns the number of bytes actually read. A return value of 0
    // means EOF (xclip finished writing) or an error.
    while (fread(buffer.data(), 1, buffer.size(), pipe) > 0)
    {
        // append() copies exactly as many bytes as fread returned into result.
        // Accumulating in a std::string is safe for arbitrary-length output.
        result.append(buffer.data(), buffer.size());
    }

    // pclose() sends a wait() for the child process and releases the FILE*.
    // Without it the child becomes a zombie (a finished process whose exit
    // status has not been collected by the parent).
    pclose(pipe);
    return result;
}

void ClipboardManager::writeClipboard(const std::string &text)
{
    // Platform-specific implementation to write text to the clipboard.
    // This is a placeholder and should be implemented according to the
    // target operating system (e.g., using WinAPI on Windows, pbcopy on macOS, etc.).
    FILE *pipe = popen("xclip -selection clipboard -i", "w");
    if (!pipe)
    {
        // popen() failed (e.g., xclip not found, fork() failed).
        return {};
    }
    fwrite(text.data(), 1, text.size(), pipe);
    pclose(pipe);
}

#endif // __linux__
