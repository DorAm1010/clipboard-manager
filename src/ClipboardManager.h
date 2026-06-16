#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>

// ─── ClipboardEntry ───────────────────────────────────────────────────────────
// Represents a single item stored in clipboard history.
struct ClipboardEntry {
    std::string content;
    std::chrono::system_clock::time_point timestamp;

    ClipboardEntry(std::string text)
        : content(std::move(text))
        , timestamp(std::chrono::system_clock::now())
    {}
};

// ─── ClipboardManager ─────────────────────────────────────────────────────────
// Monitors the system clipboard and maintains a history of copied text.
//
// Usage:
//   ClipboardManager mgr(50);           // keep last 50 entries
//   mgr.onNewEntry([](auto& e) { ... }); // optional callback
//   mgr.start();                         // begins polling (blocks until stop())
//
class ClipboardManager {
public:
    // Callback type: called whenever new text appears on the clipboard.
    using NewEntryCallback = std::function<void(const ClipboardEntry&)>;

    // maxHistory  – maximum number of entries to keep in memory.
    // pollInterval – how often (ms) to check the clipboard for changes.
    explicit ClipboardManager(size_t maxHistory = 100,
                               unsigned int pollIntervalMs = 500);
    ~ClipboardManager();

    // Register a callback that fires each time a new entry is added.
    void onNewEntry(NewEntryCallback cb);

    // Start the polling loop. Blocks until stop() is called from another thread.
    void start();

    // Signal the polling loop to exit.
    void stop();

    // Returns a snapshot of the history (newest entry last).
    std::vector<ClipboardEntry> history() const;

    // Clears all stored history entries.
    void clearHistory();

private:
    // Platform-specific: read the current clipboard text.
    // Returns an empty string if the clipboard holds non-text data or is empty.
    std::string readClipboard();

    size_t              m_maxHistory;
    unsigned int        m_pollIntervalMs;
    bool                m_running;
    std::string         m_lastSeen;
    std::vector<ClipboardEntry> m_history;
    NewEntryCallback    m_callback;
};
