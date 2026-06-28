#include "ClipboardManager.h"

#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

// ─── File-format helpers ──────────────────────────────────────────────────────
//
// The on-disk / on-wire format is one entry per line: TIMESTAMP|content|COUNT.
// Because the line is split on '\n', any newline inside the copied text would
// corrupt the format (one entry would span several lines). To avoid that we
// escape newlines to the two-character sequence "\n" on the way out, and undo
// it on the way back in. A literal backslash is escaped first so the mapping
// is reversible.

namespace
{
    // Replace each '\\' with "\\\\" and each '\n' with "\\n".
    std::string escapeNewlines(const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s)
        {
            if (c == '\\')
                out += "\\\\";
            else if (c == '\n')
                out += "\\n";
            else
                out += c;
        }
        return out;
    }

    // Reverse escapeNewlines(): turn "\\n" back into '\n' and "\\\\" into '\\'.
    std::string unescapeNewlines(const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                char next = s[i + 1];
                if (next == 'n')
                {
                    out += '\n';
                    ++i;
                    continue;
                }
                if (next == '\\')
                {
                    out += '\\';
                    ++i;
                    continue;
                }
            }
            out += s[i];
        }
        return out;
    }
}

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
        outFile << std::chrono::system_clock::to_time_t(entry.timestamp)
                << "|" << escapeNewlines(entry.content)
                << "|" << entry.copyCount
                << "\n";
    }
}

std::string ClipboardManager::serializeHistory() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ostringstream oss;
    for (const auto &entry : m_history)
    {
        oss << std::chrono::system_clock::to_time_t(entry.timestamp)
            << "|" << escapeNewlines(entry.content)
            << "|" << entry.copyCount << "\n";
    }
    return oss.str();
}

void ClipboardManager::loadHistory(const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::ifstream inFile(path);
    if (!inFile)
        return;

    std::deque<ClipboardEntry> newHistory;
    std::string line;
    while (std::getline(inFile, line))
    {
        std::istringstream iss(line);
        std::string timestampStr, content, copyCountStr;
        if (std::getline(iss, timestampStr, '|') && std::getline(iss, content, '|') && std::getline(iss, copyCountStr))
        {
            std::time_t timestamp = std::stoll(timestampStr);
            int copyCount = std::stoi(copyCountStr);
            newHistory.emplace_back(unescapeNewlines(content));
            newHistory.back().timestamp = std::chrono::system_clock::from_time_t(timestamp);
            newHistory.back().copyCount = copyCount;
        }
    }
    m_history = std::move(newHistory);
    if (!m_history.empty())
    {
        // history[0] is the most-recently-used entry under MRU ordering.
        m_lastSeen = m_history.front().content;
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

    // Seed m_lastSeen with whatever is already on the clipboard so that
    // pre-existing content is treated as "already seen" and is NOT captured
    // as a new entry. The manager only records copies made after it starts
    // watching.
    m_lastSeen = readClipboard();

    std::cout << "[ClipboardManager] Started. Polling every "
              << m_pollIntervalMs << "ms.\n";

    while (m_running.load())
    {
        std::string current = readClipboard();
        if (current == m_lastSeen)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(m_pollIntervalMs));
            continue;
        }
        m_lastSeen = current; // Remember what we acted on, so we don't re-capture it.

        // Ignore empty reads — the clipboard may hold an image, a file
        // reference, or simply be empty right now.
        if (current.empty())
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(m_pollIntervalMs));
            continue;
        }

        // History is kept in most-recently-used order: index 0 is the newest.
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // Is this exact text already somewhere in the history?
            auto it = std::find_if(m_history.begin(), m_history.end(),
                                   [&current](const ClipboardEntry &e)
                                   { return e.content == current; });

            if (it != m_history.end())
            {
                // Already present: promote it to the front (most recent),
                // bump its copy count and refresh its timestamp.
                ClipboardEntry promoted = std::move(*it);
                promoted.copyCount++;
                promoted.timestamp = std::chrono::system_clock::now();
                m_history.erase(it);
                m_history.push_front(std::move(promoted));
            }
            else
            {
                // Genuinely new content. Enforce the size limit by evicting the
                // least-recently-used entry (the back), then insert at the front
                // so the newest entry is always m_history[0].
                if (m_history.size() >= m_maxHistory)
                {
                    m_history.pop_back();
                }
                m_history.push_front(ClipboardEntry(current));

                // Fire the registered callback for genuinely new entries only.
                // std::function is truthy when it holds a callable.
                if (m_callback)
                {
                    m_callback(m_history.front());
                }
            }
        }

        // Sleep before the next poll. sleep_for() suspends only this thread,
        // leaving other threads (e.g., the main/command thread) free to run.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(m_pollIntervalMs));
    }

    std::cout << "[ClipboardManager] Stopped.\n";
}

bool ClipboardManager::pasteEntry(size_t index, std::string &outContent)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_history.size())
        return false;
    outContent = m_history[index].content;
    writeClipboard(outContent);

    // Mark the text we just put on the clipboard as already-seen so the polling
    // loop does NOT re-capture it as a new copy (which would bump its copyCount).
    // Pasting from history should not count as a fresh clipboard event.
    m_lastSeen = outContent;
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
    // Read the current clipboard BEFORE locking so we don't hold the mutex
    // during the (potentially slow) platform clipboard call.
    std::string current = readClipboard();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_history.clear();

    // Mark the text currently on the clipboard as already-seen so that
    // 'clear' stays cleared: the content still physically on the clipboard
    // is NOT immediately re-captured on the next poll. Only a genuinely new
    // copy after this point will be recorded.
    m_lastSeen = current;
}
