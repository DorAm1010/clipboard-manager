#include "ClipboardManager.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <csignal>
#include <thread>
#include <atomic>

// ─── Signal handling ──────────────────────────────────────────────────────────
// We want Ctrl+C to cleanly shut the manager down rather than abruptly kill it.

static ClipboardManager* g_manager = nullptr;

void handleSignal(int /*signum*/)
{
    std::cout << "\n[Signal] Shutting down...\n";
    if (g_manager) {
        g_manager->stop();
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Format a time_point as a human-readable "HH:MM:SS" string.
std::string formatTime(const std::chrono::system_clock::time_point& tp)
{
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};

#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

// Truncate long strings so they fit nicely on one terminal line.
std::string truncate(const std::string& s, size_t maxLen = 72)
{
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + "...";
}

// Print the current history to stdout.
void printHistory(const ClipboardManager& mgr)
{
    auto entries = mgr.history();
    if (entries.empty()) {
        std::cout << "(history is empty)\n";
        return;
    }
    std::cout << "\n── Clipboard History (" << entries.size() << " entries) ──\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] "
                  << formatTime(entries[i].timestamp)
                  << "  "
                  << truncate(entries[i].content)
                  << "\n";
    }
    std::cout << "─────────────────────────────────────\n\n";
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::cout << "╔══════════════════════════════════╗\n";
    std::cout << "║      Clipboard Manager v0.1      ║\n";
    std::cout << "╚══════════════════════════════════╝\n\n";
    std::cout << "Commands (type and press Enter):\n";
    std::cout << "  h  – show history\n";
    std::cout << "  c  – clear history\n";
    std::cout << "  q  – quit\n\n";

    // Create the manager: keep last 50 entries, poll every 500 ms.
    ClipboardManager manager(50, 500);
    g_manager = &manager;

    // Register Ctrl+C handler.
    std::signal(SIGINT,  handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Whenever a new entry is detected, print a short notification.
    manager.onNewEntry([](const ClipboardEntry& entry) {
        std::cout << "[" << "new" << "] "
                  << truncate(entry.content, 60)
                  << "\n> " << std::flush;
    });

    // Run the clipboard polling loop on a background thread so the main
    // thread stays free for reading keyboard commands.
    std::thread monitorThread([&manager]() {
        manager.start();
    });

    // ── Command loop ──────────────────────────────────────────────────────
    std::string line;
    std::cout << "> " << std::flush;

    while (std::getline(std::cin, line)) {
        if (line == "q" || line == "quit") {
            manager.stop();
            break;
        } else if (line == "h" || line == "history") {
            printHistory(manager);
        } else if (line == "c" || line == "clear") {
            manager.clearHistory();
            std::cout << "History cleared.\n";
        } else if (!line.empty()) {
            std::cout << "Unknown command. Use h, c, or q.\n";
        }
        std::cout << "> " << std::flush;
    }

    // Wait for the background thread to finish before exiting.
    if (monitorThread.joinable()) {
        monitorThread.join();
    }

    std::cout << "Goodbye!\n";
    return 0;
}
