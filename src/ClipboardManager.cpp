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
    // Pre-allocate the backing array so the first maxHistory push_back calls
    // never trigger a heap reallocation. reserve() sets capacity without
    // changing size — the vector is still logically empty after this call.
    m_history.reserve(maxHistory);
}

ClipboardManager::~ClipboardManager()
{
    // Defensively stop the loop in case the caller forgot to call stop()
    // before destroying the object. The caller is still responsible for
    // joining any thread that is running start().
    stop();
}

// ─── Public API ───────────────────────────────────────────────────────────────

void ClipboardManager::onNewEntry(NewEntryCallback cb)
{
    // Move the callable into m_callback rather than copying it. For a lambda
    // that captures variables by value, this transfers ownership of those
    // captures without duplicating them.
    m_callback = std::move(cb);
}

void ClipboardManager::start()
{
    m_running = true;

    std::cout << "[ClipboardManager] Started. Polling every "
              << m_pollIntervalMs << "ms.\n";

    while (m_running) {
        std::string current = readClipboard();

        // Guard 1: ignore empty reads — the clipboard may hold an image,
        //          a file reference, or simply be empty right now.
        // Guard 2: ignore unchanged content — the clipboard text is still
        //          the same as last time we checked; nothing to record.
        if (!current.empty() && current != m_lastSeen) {
            m_lastSeen = current;

            // Capture the text and timestamp together in one entry object.
            ClipboardEntry entry(current);

            // Enforce the sliding-window size limit: if we are at capacity,
            // drop the oldest (front) entry before adding the new one.
            // erase(begin()) on a vector is O(n) because all subsequent
            // elements shift left by one. For small histories (≤100 entries)
            // this is negligible. See ROADMAP Task 1.3 for a deque upgrade.
            if (m_history.size() >= m_maxHistory) {
                m_history.erase(m_history.begin());
            }
            m_history.push_back(entry);

            // Fire the registered callback if the caller provided one.
            // std::function is truthy when it holds a callable, falsy when empty.
            if (m_callback) {
                m_callback(entry);
            }
        }

        // Sleep before the next poll. sleep_for() suspends only this thread,
        // leaving other threads (e.g., the main/command thread) free to run.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_pollIntervalMs));
    }

    std::cout << "[ClipboardManager] Stopped.\n";
}

void ClipboardManager::stop()
{
    // Setting m_running to false causes the while-loop in start() to exit
    // after its current sleep completes (at most one more pollIntervalMs delay).
    m_running = false;
}

std::vector<ClipboardEntry> ClipboardManager::history() const
{
    // Return by value — the caller receives an independent copy of the vector.
    // This means the caller can safely iterate it even while the background
    // thread continues modifying m_history (though a mutex would be needed
    // to make this fully race-free — see ROADMAP Task 1.2).
    return m_history;
}

void ClipboardManager::clearHistory()
{
    m_history.clear();

    // Also reset m_lastSeen so the next clipboard read is treated as new,
    // even if the clipboard content has not changed since the last poll.
    m_lastSeen.clear();
}
