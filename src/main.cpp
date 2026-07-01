#include "ClipboardManager.h"
#include "platform/paths.h"
#include "ansi.h"
#include "platform/service.hpp"

// Global hotkey listener for the popup window. Only implemented on macOS so
// far (hotkey_mac.mm); every use of Hotkey:: below is therefore guarded with
// #if defined(__APPLE__) so Windows/Linux builds never need to link symbols
// that don't exist yet on those platforms.
#if defined(__APPLE__)
#include "platform/hotkey.h"
#endif

#include <CLI/CLI.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <thread>
#include <atomic>
#include <algorithm>
#include <string>

// For closing stdin from the signal handler (see handleSignal).
#ifdef _WIN32
#include <io.h> // _close
#else
#include <unistd.h> // close, STDIN_FILENO
#endif

// ─── Signal handling ──────────────────────────────────────────────────────────

/**
 * @brief Global pointer used to reach the ClipboardManager from the signal handler.
 *
 * Signal handlers are C-style callbacks: they receive only an int (the signal
 * number) and have no access to local variables or `this`. A file-scope
 * pointer is the standard way to bridge this gap. It is set once in main()
 * before any signals can arrive, so no synchronisation is needed.
 */
static ClipboardManager *g_manager = nullptr;

/**
 * @brief Signal handler for SIGINT (Ctrl+C) and SIGTERM.
 *
 * Three things have to happen for a clean shutdown:
 *   1. Stop the polling loop  — g_manager->stop() flips the running flag so the
 *      background monitor thread exits.
 *   2. Unblock the main thread — in interactive mode the main thread is parked
 *      inside std::getline(std::cin, ...), which the signal alone does NOT
 *      interrupt (libc++/libstdc++ retry the read on EINTR). Closing stdin
 *      forces getline to hit end-of-file and return false, so the command loop
 *      breaks and the program shuts down just as if the user had typed `q`.
 *   3. (macOS daemon mode only) Unblock the hotkey run loop — Hotkey::runEventLoop()
 *      parks the main thread in CFRunLoopRun(); without this call that loop
 *      would never notice the shutdown and the daemon would hang exactly like
 *      the start/stop race fixed earlier in this project.
 *
 * In daemon mode stdin is already redirected to /dev/null, so closing it is
 * harmless there.
 *
 * Note: close() is async-signal-safe. (The std::cout below technically is not,
 * but the original handler already used it; for this learning project that is
 * an acceptable simplification.)
 *
 * @param signum  The signal number delivered by the OS (unused; we handle
 *                SIGINT and SIGTERM identically).
 */
void handleSignal(int /*signum*/)
{
    std::cout << "\n[Signal] Shutting down...\n";
    if (g_manager)
    {
        g_manager->stop();
    }

#if defined(__APPLE__)
    Hotkey::requestStop();
#endif

    // Unblock a main thread that is waiting in std::getline by closing stdin.
#ifdef _WIN32
    _close(0);
#else
    close(STDIN_FILENO);
#endif
}

// ─── Helper functions ─────────────────────────────────────────────────────────

/**
 * @brief Format a system_clock time_point as a local-time "HH:MM:SS" string.
 *
 * Steps:
 *   1. Convert the time_point to a time_t (seconds since Unix epoch).
 *   2. Explode it into a struct tm (year, month, day, hour, …) in local time.
 *   3. Format it with std::put_time using the "%H:%M:%S" format specifier.
 *
 * localtime_r (POSIX) and localtime_s (Windows) are thread-safe variants of
 * the standard localtime(). We pick the right one at compile time with #ifdef.
 *
 * @param tp  The time_point to format.
 * @return    A string such as "14:03:57".
 */
std::string formatTime(const std::chrono::system_clock::time_point &tp)
{
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};

#ifdef _WIN32
    localtime_s(&tm, &t); // Windows thread-safe variant (parameters reversed)
#else
    localtime_r(&t, &tm); // POSIX thread-safe variant
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

namespace
{
    // Number of UTF-8 code points in a string ≈ its display width in columns.
    // We count every byte that is NOT a UTF-8 continuation byte (0b10xxxxxx).
    // Caveat: true double-width glyphs (CJK, emoji) are counted as width 1, so
    // those would still misalign — handling them needs a wcwidth-style table.
    // For ASCII plus single-width symbols (⏎, ─, accents) this is exact.
    int displayWidth(const std::string &s)
    {
        int w = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) // not a continuation byte → start of a code point
                ++w;
        return w;
    }

    // Length in bytes of the UTF-8 code point starting at byte `c`.
    size_t utf8CharLen(unsigned char c)
    {
        if ((c & 0x80) == 0x00)
            return 1; // 0xxxxxxx
        if ((c & 0xE0) == 0xC0)
            return 2; // 110xxxxx
        if ((c & 0xF0) == 0xE0)
            return 3; // 1110xxxx
        if ((c & 0xF8) == 0xF0)
            return 4; // 11110xxx
        return 1;     // invalid byte: treat as 1 so we always make progress
    }
}

/**
 * @brief Collapse a string to a single line and shorten it for terminal display.
 *
 * Embedded newlines / carriage returns become a "⏎" marker so a multi-line entry
 * renders on one line. The result is then truncated to at most maxLen display
 * columns (UTF-8 aware — it never slices a character in half), appending "..."
 * if it was cut.
 *
 * @param s       The string to collapse and truncate.
 * @param maxLen  Maximum display width including the "..." suffix.
 * @return        A single-line copy, truncated with "..." if it exceeded maxLen.
 */
std::string truncate(const std::string &s, size_t maxLen = 72)
{
    // First flatten any line breaks to a visible marker.
    std::string oneLine;
    oneLine.reserve(s.size());
    for (char c : s)
    {
        if (c == '\n' || c == '\r')
            oneLine += "⏎"; // ⏎ (return symbol)
        else
            oneLine += c;
    }

    if (static_cast<size_t>(displayWidth(oneLine)) <= maxLen)
        return oneLine;

    // Walk whole code points until we've kept (maxLen - 3) columns, then add "...".
    const size_t targetCols = maxLen - 3;
    size_t cols = 0;
    size_t i = 0;
    while (i < oneLine.size() && cols < targetCols)
    {
        i += utf8CharLen(static_cast<unsigned char>(oneLine[i]));
        ++cols;
    }
    return oneLine.substr(0, i) + "...";
}

// ─── Table rendering helpers ───────────────────────────────────────────────────
//
// These build a box-drawn table using only the standard library — no third-party
// dependency. Note: padding is computed on byte length, so columns may misalign
// slightly for content with multi-byte UTF-8 characters (accents, emoji, the ⏎
// marker). For mostly-ASCII clipboard text this looks clean.

namespace
{
    // Pad a string on the right with spaces to a given display width (UTF-8 aware).
    std::string padRight(const std::string &s, int width)
    {
        int len = displayWidth(s);
        return len >= width ? s : s + std::string(width - len, ' ');
    }

    // Pad a string on the left with spaces (used for right-aligned numbers).
    std::string padLeft(const std::string &s, int width)
    {
        int len = displayWidth(s);
        return len >= width ? s : std::string(width - len, ' ') + s;
    }

    // Build a horizontal rule like "├────┼──────┼────┤". Each column reserves
    // width+2 dashes to account for the single space of padding on each side.
    std::string hrule(const char *left, const char *mid, const char *right,
                      int w1, int w2, int w3, int w4)
    {
        auto seg = [](int w)
        {
            std::string s;
            for (int i = 0; i < w + 2; ++i)
                s += "─";
            return s;
        };
        return std::string(left) + seg(w1) + mid + seg(w2) + mid + seg(w3) + mid + seg(w4) + right;
    }
}

/**
 * @brief Render a list of entries as a box-drawn table: #, Time, Content, Count.
 *
 * @param heading  A short title printed above the table.
 * @param entries  The entries to display (already a stable snapshot).
 */
void printTable(const std::string &heading, const std::vector<ClipboardEntry> &entries)
{
    // Column content widths (the visible cell width, excluding the 1-space pad
    // on each side that the borders account for).
    const int idxW = std::max<int>(1, static_cast<int>(std::to_string(entries.size()).size()));
    const int timeW = 8; // "HH:MM:SS"
    const int contentW = 50;

    int maxCount = 1;
    for (const auto &e : entries)
        maxCount = std::max(maxCount, e.copyCount);
    const int countW = std::max<int>(5, static_cast<int>(std::to_string(maxCount).size())); // "Count" is 5 wide

    std::cout << "\n"
              << heading << "\n";
    std::cout << hrule("┌", "┬", "┐", idxW, timeW, contentW, countW) << "\n";
    std::cout << "│ " << padRight("#", idxW)
              << " │ " << padRight("Time", timeW)
              << " │ " << padRight("Content", contentW)
              << " │ " << padRight("Count", countW) << " │\n";
    std::cout << hrule("├", "┼", "┤", idxW, timeW, contentW, countW) << "\n";

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const std::string idx = std::to_string(i + 1);
        const std::string tm = formatTime(entries[i].timestamp);
        const std::string content = truncate(entries[i].content, contentW);
        const std::string cnt = std::to_string(entries[i].copyCount);

        // Color is applied around the already-padded content so the (invisible)
        // escape codes don't throw off the column alignment.
        std::cout << "│ " << padLeft(idx, idxW)
                  << " │ " << padRight(tm, timeW)
                  << " │ " << ansi::entryColor(i) << padRight(content, contentW) << ansi::reset()
                  << " │ " << padLeft(cnt, countW) << " │\n";
    }

    std::cout << hrule("└", "┴", "┘", idxW, timeW, contentW, countW) << "\n\n";
}

/**
 * @brief Print the full clipboard history as a table.
 *
 * Calls mgr.history() which returns a copy, so this safely iterates a stable
 * snapshot even if the background thread is concurrently adding entries.
 */
void printHistory(const ClipboardManager &mgr)
{
    auto entries = mgr.history();
    if (entries.empty())
    {
        std::cout << "(history is empty)\n";
        return;
    }
    // history() returns a deque; copy into a vector for the table renderer.
    std::vector<ClipboardEntry> rows(entries.begin(), entries.end());
    printTable("Clipboard History (" + std::to_string(rows.size()) + " entries)", rows);
}

void printSearchResults(const std::vector<ClipboardEntry> &results, const std::string &keyword)
{
    if (results.empty())
    {
        std::cout << "No entries found containing \"" << keyword << "\".\n";
        return;
    }
    printTable("Search Results for \"" + keyword + "\" (" + std::to_string(results.size()) + " entries)", results);
}

void pasteEntry(ClipboardManager &mgr, const std::string &numberStr)
{
    size_t index = 0;
    try
    {
        index = std::stoi(numberStr);
    }
    catch (const std::invalid_argument &)
    {
        std::cout << "Invalid index. Please enter a valid number.\n";
        return;
    }
    catch (const std::out_of_range &)
    {
        std::cout << "Index out of range. Please enter a smaller number.\n";
        return;
    }
    std::string content;
    if (index == 0 || !mgr.pasteEntry(index - 1, content))
    {
        std::cout << "Invalid entry number.\n";
        return;
    }
    // "Paste" the entry into the terminal by printing its full content, and
    // (via pasteEntry) it has also been placed on the system clipboard.
    std::cout << content << "\n";
}

// make this banner into a --help flag in the future
void printPlatformInfo()
{
    std::cout << "╔══════════════════════════════════╗\n";
    std::cout << "║      Clipboard Manager v0.1      ║\n";
    std::cout << "╚══════════════════════════════════╝\n\n";
    std::cout << "Commands (type and press Enter):\n";
    std::cout << "  h  – show history\n";
    std::cout << "  c  – clear history\n";
    std::cout << "  s  – search history\n";
    std::cout << "  p  – paste from history\n";
    std::cout << "  q  – quit\n\n";
}

// ─── Entry point ──────────────────────────────────────────────────────────────

void run()
{
    printPlatformInfo();
    // Keep last 50 entries; check the clipboard every 500 ms.
    ClipboardManager manager(50, 500);

    manager.loadHistory(getHistoryFilePath());
    // Expose the manager to the signal handler before registering signals,
    // so the handler is never called with a null pointer.
    g_manager = &manager;

    // Replace the default SIGINT / SIGTERM handlers with our clean-shutdown handler.
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Register an inline callback (lambda) that fires each time the clipboard
    // changes. The lambda captures nothing — it only uses its parameter.
    // std::flush forces the "> " prompt to appear immediately after the
    // notification line, even though std::cout is line-buffered by default.
    std::string newEntryMessage = "[new] ";
    if (ansi::supportsColor())
    {
        newEntryMessage = std::string(ansi::entryColor(0)) + newEntryMessage + ansi::reset();
    }
    manager.onNewEntry([newEntryMessage](const ClipboardEntry &entry)
                       { std::cout << newEntryMessage
                                   << truncate(entry.content, 60)
                                   << "\n> " << std::flush; });

    // Spin up a background thread that runs the blocking poll loop.
    // The lambda captures manager by reference ([&manager]) — the reference
    // stays valid because manager lives in main()'s stack frame for the
    // entire duration of monitorThread.
    std::thread monitorThread([&manager]()
                              { manager.start(); });

    // ── Command loop ──────────────────────────────────────────────────────
    // std::getline blocks until the user presses Enter, then writes the
    // typed line (without the newline) into `line`. It returns false when
    // stdin is closed (e.g., the user presses Ctrl+D on Unix).
    std::string line;
    std::cout << "> " << std::flush;

    while (std::getline(std::cin, line))
    {
        if (line == "q" || line == "quit")
        {
            manager.stop(); // signal the background thread to exit
            break;
        }
        else if (line == "h" || line == "history")
        {
            printHistory(manager);
        }
        else if (line == "c" || line == "clear")
        {
            manager.clearHistory();
            std::cout << "History cleared.\n";
        }
        else if (line == "s" || line == "search")
        {
            std::cout << "Enter search keyword: ";
            std::string keyword;
            std::getline(std::cin, keyword);
            auto results = manager.search(keyword);
            printSearchResults(results, keyword);
        }
        else if (line.substr(0, 2) == "p " || line.substr(0, 6) == "paste ")
        {
            std::string numberStr = (line[0] == 'p' && line[1] == ' ')
                                        ? line.substr(2)
                                        : line.substr(6);
            pasteEntry(manager, numberStr);
        }
        else if (line.empty())
        {
            printHistory(manager);
        }
        else if (!line.empty())
        {
            std::cout << "Unknown command. Use h, c, or q.\n";
        }
        std::cout << "> " << std::flush;
    }

    // join() blocks the main thread until monitorThread has returned from
    // manager.start(). This is mandatory: destroying a std::thread object
    // that is still joinable calls std::terminate(), crashing the program.
    if (monitorThread.joinable())
    {
        monitorThread.join();
    }
    manager.saveHistory(getHistoryFilePath());
    std::cout << "Goodbye!\n";
}

void runAsDaemon()
{
    // Keep last 50 entries; check the clipboard every 500 ms.
    ClipboardManager manager(50, 500);

    manager.loadHistory(getHistoryFilePath());
    // Expose the manager to the signal handler before registering signals,
    // so the handler is never called with a null pointer.
    g_manager = &manager;

    // Replace the default SIGINT / SIGTERM handlers with our clean-shutdown handler.
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Spin up a background thread that runs the blocking poll loop.
    // The lambda captures manager by reference ([&manager]) — the reference
    // stays valid because manager lives in main()'s stack frame for the
    // entire duration of monitorThread.
    std::thread monitorThread([&manager]()
                              { manager.start(); });
    std::thread socketThread([&manager]()
                             { Service::createHistoryRequestSocket(manager); });

#if defined(__APPLE__)
    // STEP 1 of the popup-window work: register the global hotkey and block
    // THIS (main) thread pumping its event loop, since Carbon/CoreFoundation
    // hotkey delivery is tied to the thread that registered it. monitorThread
    // and socketThread keep running exactly as before on their own threads.
    // handleSignal() calls Hotkey::requestStop() to unblock this on shutdown.
    // There is no popup window yet — runEventLoop()'s handler just logs.
    Hotkey::runEventLoop(manager);
#endif

    // Wait for the background thread to finish (it will run until stop() is called).
    if (monitorThread.joinable())
    {
        monitorThread.join();
    }
    if (socketThread.joinable())
    {
        socketThread.join();
    }
    manager.saveHistory(getHistoryFilePath());
}

/**
 * @brief Program entry point.
 *
 * Orchestrates three cooperating components:
 *   1. A ClipboardManager that polls the OS clipboard on a background thread.
 *   2. An OS signal handler that cleanly stops the manager on Ctrl+C / SIGTERM.
 *   3. A command loop on the main thread that reads user input and dispatches
 *      commands (show history, clear history, quit).
 *
 * Threading model:
 *   - Main thread:       reads stdin, prints prompts, dispatches commands.
 *   - monitorThread:     runs ClipboardManager::start(), blocks on sleep(),
 *                        exits when m_running becomes false.
 *   - Signal handler:    not a thread — it interrupts whatever thread happens
 *                        to be executing when the signal arrives.
 *
 * @return 0 on normal exit.
 */
int main(int argc, char *argv[])
{
    CLI::App app{"Clipboard Manager App"};
    bool daemon_mode{false};
    bool stop_mode{false};
    bool history_mode{false};
    bool foreground_mode{false};

    auto d_flag = app.add_flag("-d,--daemon", daemon_mode, "Run app as a background daemon");
    auto s_flag = app.add_flag("-k,--kill", stop_mode, "kill the running daemon process");
    auto history_flag = app.add_flag("-H,--history", history_mode, "Show the clipboard history and exit");
    // Debug aid: run the daemon's logic (polling + IPC + hotkey) WITHOUT
    // forking/detaching, so stdout/stderr stay attached to the terminal.
    // Real `--daemon` redirects them to /dev/null (see Service::daemonize()),
    // which makes it impossible to see log output like "[Hotkey] ... pressed!"
    // while developing/testing. Only meaningful together with --daemon.
    app.add_flag("--foreground", foreground_mode,
                 "With --daemon: don't detach, keep logs on this terminal (for debugging)");

    // Production constraint: these three modes are mutually exclusive — you
    // cannot daemonize, kill, and dump history in the same invocation. CLI11's
    // excludes() is symmetric, so one call per pair is enough; we wire every
    // pair so any two-flag combination is rejected with a clear error.
    d_flag->excludes(s_flag);
    d_flag->excludes(history_flag);
    s_flag->excludes(history_flag);

    // Enforce parsing constraints rules cleanly
    CLI11_PARSE(app, argc, argv);

    // If --kill was requested, call stop and exit early
    if (stop_mode)
    {
        return Service::stop();
    }
    // After CLI11_PARSE:
    if (history_mode)
    {
        Service::requestHistory();
        return 0;
    }
    if (daemon_mode)
    {
        if (foreground_mode)
        {
            std::cout << "[Foreground] Running daemon logic without detaching "
                          "(Ctrl+C to stop; --kill will not find this process "
                          "since no PID file is written in this mode).\n";
        }
        else if (Service::daemonize() != 0)
        {
            std::cerr << "Failed to enter background mode.\n";
            return 1;
        }
        runAsDaemon();
    }
    else
    {
        run();
    }
    return 0;
}
