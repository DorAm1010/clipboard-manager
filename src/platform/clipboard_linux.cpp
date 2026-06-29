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
    //
    // We wrap xclip in `timeout 2` so a wedged X server / unresponsive
    // selection owner can never block this call forever. Since this runs in
    // the 500 ms poll loop (and in a daemon), an unbounded hang here would
    // freeze the whole program; the timeout caps it at ~2 s and we just treat
    // a stuck read as "no text this round".
    FILE *pipe = popen("timeout 2 xclip -selection clipboard -o 2>/dev/null", "r");
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
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), pipe)) > 0)
    {
        // append() copies exactly as many bytes as fread returned into result.
        // Accumulating in a std::string is safe for arbitrary-length output.
        result.append(buffer.data(), n);
    }

    // pclose() sends a wait() for the child process and releases the FILE*.
    // Without it the child becomes a zombie (a finished process whose exit
    // status has not been collected by the parent).
    pclose(pipe);
    return result;
}

void ClipboardManager::writeClipboard(const std::string &text)
{
    // Write text to the CLIPBOARD selection via xclip. xclip forks a child that
    // keeps serving the selection after the parent exits; the `timeout 2` only
    // bounds the initial set (which is near-instant), so it won't kill the
    // long-lived server child in normal operation — it just prevents a hang if
    // the X server is unresponsive.
    FILE *pipe = popen("timeout 2 xclip -selection clipboard -i", "w");
    if (!pipe)
    {
        // popen() failed (e.g., xclip not found, fork() failed).
        return;
    }
    fwrite(text.data(), 1, text.size(), pipe);
    pclose(pipe);
}

#endif // __linux__
