#include "ClipboardManager.h"
#include "platform/paths.h"
#include "ansi.h"
#include "platform/service.hpp"

#include <CLI/CLI.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <thread>
#include <atomic>

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
 * Tells the ClipboardManager's polling loop to exit on its next iteration,
 * allowing the program to shut down cleanly instead of being killed mid-write.
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

/**
 * @brief Shorten a string to at most maxLen characters for terminal display.
 *
 * If the string is already short enough it is returned unchanged. Otherwise
 * the last three characters of the truncated string are replaced with "..."
 * to signal that content was omitted.
 *
 * @param s       The string to (possibly) truncate.
 * @param maxLen  Maximum allowed length including the "..." suffix.
 * @return        The original string, or a truncated copy ending in "...".
 */
std::string truncate(const std::string &s, size_t maxLen = 72)
{
    if (s.size() <= maxLen)
        return s;
    return s.substr(0, maxLen - 3) + "...";
}

/**
 * @brief Print all history entries to stdout in a numbered, timestamped list.
 *
 * Calls mgr.history() which returns a copy of the internal vector, so this
 * function safely iterates a stable snapshot even if the background thread
 * is concurrently adding new entries.
 *
 * @param mgr  The ClipboardManager whose history to display.
 */
void printHistory(const ClipboardManager &mgr)
{
    auto entries = mgr.history();
    if (entries.empty())
    {
        std::cout << "(history is empty)\n";
        return;
    }
    std::cout << "\n── Clipboard History (" << entries.size() << " entries) ──\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        std::cout << ansi::entryColor(i)
                  << "  [" << (i + 1) << "] " << ansi::reset()
                  << formatTime(entries[i].timestamp)
                  << " | "
                  << truncate(entries[i].content)
                  << " | "
                  << "copied " << entries[i].copyCount << " times"
                  << "\n";
    }
    std::cout << "─────────────────────────────────────\n\n";
}

void printSearchResults(const std::vector<ClipboardEntry> &results, const std::string &keyword)
{
    if (results.empty())
    {
        std::cout << "No entries found containing \"" << keyword << "\".\n";
        return;
    }
    std::cout << "\n── Search Results for \"" << keyword << "\" (" << results.size() << " entries) ──\n";
    for (size_t i = 0; i < results.size(); ++i)
    {
        std::cout << ansi::entryColor(i)
                  << "  [" << (i + 1) << "] " << ansi::reset()
                  << formatTime(results[i].timestamp)
                  << " | "
                  << truncate(results[i].content)
                  << " | "
                  << "copied " << results[i].copyCount << " times"
                  << "\n";
    }
    std::cout << "─────────────────────────────────────\n\n";
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
    if (index == 0 || !mgr.pasteEntry(index - 1))
    {
        std::cout << "Invalid entry number.\n";
    }
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

    auto d_flag = app.add_flag("-d,--daemon", daemon_mode, "Run app as a background daemon");
    auto s_flag = app.add_flag("-k,--kill", stop_mode, "kill the running daemon process");
    auto history_flag = app.add_flag("-H,--history", history_mode, "Show the clipboard history and exit");

    // Production constraint: Exclude flags from being used together
    d_flag->excludes(s_flag);
    s_flag->excludes(d_flag);

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
        if (Service::daemonize() != 0)
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
