#include "ClipboardManager.h"

#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

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
    // ASCII-lowercase a copy of the string. Used to make search case-insensitive.
    // (This folds A-Z only; full Unicode case folding is out of scope here.)
    std::string toLower(const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) // unsigned char: std::tolower is UB on negative values
            out += static_cast<char>(std::tolower(c));
        return out;
    }

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

    outFile.close();

    // Clipboard history routinely contains secrets (passwords, tokens), so the
    // file must be readable only by its owner (POSIX 0600). std::filesystem is
    // used so this stays cross-platform; on Windows owner-only ACLs are managed
    // differently and this call is a best-effort no-op (errors are swallowed).
    std::error_code ec;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec);
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
        // Format: TIMESTAMP|content|COUNT. The content may itself contain '|'
        // characters, so we cannot naively split on every '|'. Instead we split
        // on the FIRST and LAST '|': timestamp is before the first, count is
        // after the last, and everything in between is the (escaped) content.
        // timestamp and count are always plain integers, so they never contain
        // a '|' — this makes the first/last delimiters unambiguous.
        size_t firstPipe = line.find('|');
        size_t lastPipe = line.rfind('|');

        // Need at least two distinct '|' for a well-formed record.
        if (firstPipe == std::string::npos || lastPipe == firstPipe)
            continue;

        std::string timestampStr = line.substr(0, firstPipe);
        std::string content = line.substr(firstPipe + 1, lastPipe - firstPipe - 1);
        std::string copyCountStr = line.substr(lastPipe + 1);

        // A corrupt or partially-written line should be skipped, not crash the
        // whole program — std::stoll/std::stoi throw on non-numeric input.
        try
        {
            std::time_t timestamp = std::stoll(timestampStr);
            int copyCount = std::stoi(copyCountStr);
            newHistory.emplace_back(unescapeNewlines(content));
            newHistory.back().timestamp = std::chrono::system_clock::from_time_t(timestamp);
            newHistory.back().copyCount = copyCount;
        }
        catch (const std::exception &)
        {
            continue; // skip malformed line
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

    // Case-insensitive match: compare lowercased copies of both sides.
    const std::string loweredKeyword = toLower(keyword);

    // Lock the mutex to safely access m_history from this thread.
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto &entry : m_history)
    {
        if (toLower(entry.content).find(loweredKeyword) != std::string::npos)
        {
            results.push_back(entry);
        }
    }

    return results;
}

void ClipboardManager::start()
{
    // NOTE: we deliberately do NOT set m_running = true here. It is initialised
    // to true at construction and only stop() ever writes it (to false). See the
    // m_running declaration in the header for why — setting it here races with a
    // fast stop() and can hang the poll loop forever.

    // Seed m_lastSeen with whatever is already on the clipboard so that
    // pre-existing content is treated as "already seen" and is NOT captured
    // as a new entry. The manager only records copies made after it starts
    // watching.
    //
    // m_lastSeen is shared with pasteEntry()/clearHistory() (which run on other
    // threads and write it under m_mutex), so EVERY access to it — including
    // this seed and the comparison below — must hold m_mutex. The clipboard read
    // itself is done OUTSIDE the lock because it can be slow (a platform call).
    {
        std::string seed = readClipboard();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastSeen = std::move(seed);
    }

    std::cout << "[ClipboardManager] Started. Polling every "
              << m_pollIntervalMs << "ms.\n";

    while (m_running.load())
    {
        // Read the clipboard without holding the lock (platform call may block).
        std::string current = readClipboard();

        // If we capture a genuinely new entry, we deliver it to the callback
        // AFTER releasing the lock. Firing the callback while holding m_mutex
        // would deadlock if the callback ever called back into the manager
        // (history(), search(), pasteEntry(), …) since m_mutex is not recursive.
        bool haveNewEntry = false;
        ClipboardEntry newEntry{std::string{}};

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // No change since last poll → nothing to do this round.
            // Empty reads (image/file/empty clipboard) are recorded as "seen"
            // so we don't keep re-evaluating them, but never stored as entries.
            if (current != m_lastSeen)
            {
                m_lastSeen = current; // Remember what we acted on.

                if (!current.empty())
                {
                    // History is kept in most-recently-used order: index 0 is newest.
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
                        // Genuinely new content. Enforce the size limit by evicting
                        // the least-recently-used entry (the back), then insert at
                        // the front so the newest entry is always m_history[0].
                        if (m_history.size() >= m_maxHistory)
                        {
                            m_history.pop_back();
                        }
                        m_history.push_front(ClipboardEntry(current));

                        // Stage the new entry for the callback (fired below, after
                        // the lock is released). std::function is truthy when set.
                        if (m_callback)
                        {
                            haveNewEntry = true;
                            newEntry = m_history.front();
                        }
                    }
                }
            }
        } // m_mutex released here

        // Fire the callback outside the lock — see haveNewEntry comment above.
        if (haveNewEntry && m_callback)
        {
            m_callback(newEntry);
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
