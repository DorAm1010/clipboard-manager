#include "ClipboardManager.h"

#include <thread>
#include <chrono>
#include <iostream>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

ClipboardManager::ClipboardManager(size_t maxHistory, unsigned int pollIntervalMs)
    : m_maxHistory(maxHistory)
    , m_pollIntervalMs(pollIntervalMs)
    , m_running(false)
{
    m_history.reserve(maxHistory);
}

ClipboardManager::~ClipboardManager()
{
    stop();
}

// ─── Public API ───────────────────────────────────────────────────────────────

void ClipboardManager::onNewEntry(NewEntryCallback cb)
{
    m_callback = std::move(cb);
}

void ClipboardManager::start()
{
    m_running = true;

    std::cout << "[ClipboardManager] Started. Polling every "
              << m_pollIntervalMs << "ms.\n";

    while (m_running) {
        std::string current = readClipboard();

        // Only act when the clipboard content has actually changed.
        if (!current.empty() && current != m_lastSeen) {
            m_lastSeen = current;

            ClipboardEntry entry(current);

            // Maintain the history size limit (drop the oldest entry).
            if (m_history.size() >= m_maxHistory) {
                m_history.erase(m_history.begin());
            }
            m_history.push_back(entry);

            // Fire the user-registered callback if one was set.
            if (m_callback) {
                m_callback(entry);
            }
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_pollIntervalMs));
    }

    std::cout << "[ClipboardManager] Stopped.\n";
}

void ClipboardManager::stop()
{
    m_running = false;
}

std::vector<ClipboardEntry> ClipboardManager::history() const
{
    return m_history;   // returns a copy; safe for the caller to iterate
}

void ClipboardManager::clearHistory()
{
    m_history.clear();
    m_lastSeen.clear();
}
