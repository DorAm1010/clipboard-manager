#include "ClipboardManager.h"

#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

ClipboardManager::ClipboardManager(size_t maxHistory, unsigned int pollIntervalMs)
    : m_maxHistory(maxHistory), m_pollIntervalMs(pollIntervalMs) {}

ClipboardManager::~ClipboardManager()
{
    // Defensively stop the loop in case the caller forgot to call stop()
    // before destroying the object. The caller is still responsible for
    // joining any thread that is running start().
    stop();
}

// ─── Public API ───────────────────────────────────────────────────────────────

void ClipboardManager::saveHistory(const std::string &path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ofstream outFile(path);

    if (!outFile)
    {
        return;
    }

    for (const auto &entry : m_history)
    {
        outFile << std::chrono::system_clock::to_time_t(entry.timestamp) << "|" << entry.content << "\n";
    }
}

void ClipboardManager::loadHistory(const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ifstream inFile(path);
    if (!inFile)
    {
        std::cerr << "Error: Could not open file for reading: " << path << "\n";
        return;
    }
    std::deque<ClipboardEntry> newHistory;
    std::string line;
    while (std::getline(inFile, line))
    {
        std::istringstream iss(line);
        std::string timestampStr, content;
        if (std::getline(iss, timestampStr, '|') && std::getline(iss, content))
        {
            std::time_t timestamp = std::stoll(timestampStr);
            newHistory.emplace_back(content);
            newHistory.back().timestamp = std::chrono::system_clock::from_time_t(timestamp);
        }
    }
    m_history = std::move(newHistory);
    if (!m_history.empty())
    {
        m_lastSeen = m_history.back().content;
    }
}

void ClipboardManager::onNewEntry(NewEntryCallback cb)
{
    // Move the callable into m_callback rather than copying it. For a lambda
    // that captures variables by value, this transfers ownership of those
    // captures without duplicating them.
    m_callback = std::move(cb);
}

std::vector<ClipboardEntry> ClipboardManager::search(const std::string &keyword) const
{
    std::vector<ClipboardEntry> results;

    // Lock the mutex to safely access m_history from this thread.
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto &entry : m_history)
    {
        if (entry.content.find(keyword) != std::string::npos)
        {
            results.push_back(entry);
        }
    }

    return results;
}

void ClipboardManager::start()
{
    m_running.store(true);

    std::cout << "[ClipboardManager] Started. Polling every "
              << m_pollIntervalMs << "ms.\n";

    while (m_running.load())
    {
        std::string current = readClipboard();

        // Guard 1: ignore empty reads — the clipboard may hold an image,
        //          a file reference, or simply be empty right now.
        // Guard 2: ignore unchanged content — the clipboard text is still
        //          the same as last time we checked; nothing to record.
        if (!current.empty() && current != m_lastSeen)
        {
            m_lastSeen = current;

            // Capture the text and timestamp together in one entry object.
            ClipboardEntry entry(current);

            // Enforce the sliding-window size limit: if we are at capacity,
            // drop the oldest (front) entry before adding the new one.
            // erase(begin()) on a vector is O(n) because all subsequent
            // elements shift left by one. For small histories (≤100 entries)
            // this is negligible. See ROADMAP Task 1.3 for a deque upgrade.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_history.size() >= m_maxHistory)
            {
                m_history.pop_front();
            }
            m_history.push_back(entry);

            // Fire the registered callback if the caller provided one.
            // std::function is truthy when it holds a callable, falsy when empty.
            if (m_callback)
            {
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

bool ClipboardManager::pasteEntry(size_t index)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_history.size())
        return false;
    writeClipboard(m_history[index].content);
    return true;
}

void ClipboardManager::stop()
{
    // Setting m_running to false causes the while-loop in start() to exit
    // after its current sleep completes (at most one more pollIntervalMs delay).
    m_running.store(false);
}

std::deque<ClipboardEntry> ClipboardManager::history() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_history;
}

void ClipboardManager::clearHistory()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_history.clear();

    // Also reset m_lastSeen so the next clipboard read is treated as new,
    // even if the clipboard content has not changed since the last poll.
    m_lastSeen.clear();
}
