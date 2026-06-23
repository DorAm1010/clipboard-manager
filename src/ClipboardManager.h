#pragma once

#include <atomic>
#include <string>
#include <functional>
#include <chrono>
#include <mutex>
#include <deque>
#include <vector>

// ─── ClipboardEntry ───────────────────────────────────────────────────────────

/**
 * @brief A single snapshot of clipboard text captured at a point in time.
 *
 * Every time the clipboard changes to new text, one ClipboardEntry is created
 * and appended to ClipboardManager's internal history deque.
 *
 * Design note: this is a plain data struct (no private members, no invariants
 * to protect), so we use `struct` rather than `class` as a signal to readers
 * that it is just a bundle of values.
 */
struct ClipboardEntry
{
    /// The raw text that was on the clipboard when this entry was captured.
    std::string content;

    /// Wall-clock time at which the entry was captured.
    /// Stored as a system_clock time_point so it can be formatted later
    /// without losing precision.
    std::chrono::system_clock::time_point timestamp;

    int copyCount = 1; // Number of times this entry has been copied to the clipboard

    /**
     * @brief Construct an entry from a text string, stamping the current time.
     *
     * @param text  The clipboard text. Taken by value so the caller can pass
     *              either an lvalue (copied once) or an rvalue/temporary
     *              (moved at zero cost). We then move it into `content` to
     *              avoid a second copy.
     */
    ClipboardEntry(std::string text)
        : content(std::move(text)), timestamp(std::chrono::system_clock::now())
    {
    }
};

// ─── ClipboardManager ─────────────────────────────────────────────────────────

/**
 * @brief Polls the system clipboard and maintains a bounded history of entries.
 *
 * The manager runs a tight poll-and-compare loop on a background thread:
 * every `pollIntervalMs` milliseconds it reads the clipboard, compares the
 * result with the last-seen value, and — if the text changed — appends a new
 * ClipboardEntry to the internal history and fires the optional user callback.
 *
 * The history is bounded: once it reaches `maxHistory` entries the oldest
 * entry is discarded before the new one is added (a sliding window).
 *
 * Typical usage (blocking call on a dedicated thread):
 * @code
 *   ClipboardManager mgr(50, 500);          // keep 50 entries, poll every 500 ms
 *   mgr.onNewEntry([](const ClipboardEntry& e) {
 *       std::cout << "new: " << e.content << "\n";
 *   });
 *   std::thread t([&]{ mgr.start(); });     // blocks inside start()
 *   // ... later, from any thread:
 *   mgr.stop();
 *   t.join();
 * @endcode
 */
class ClipboardManager
{
public:
    /**
     * @brief Type alias for the new-entry callback.
     *
     * std::function<> is a type-erased callable container — it can hold a
     * plain function pointer, a lambda, a functor, or a bound member function,
     * as long as the signature matches `void(const ClipboardEntry&)`.
     *
     * This is the Observer pattern: the manager does not know or care what
     * the caller does with a new entry — it just calls whatever was registered.
     */
    using NewEntryCallback = std::function<void(const ClipboardEntry &)>;

    /**
     * @brief Construct the manager with a history limit and poll interval.
     *
     * @param maxHistory      Maximum number of entries to keep. When the limit
     *                        is reached the oldest entry is erased before the
     *                        new one is pushed. Defaults to 100.
     * @param pollIntervalMs  How often (in milliseconds) to read the clipboard
     *                        and check for changes. Lower values feel more
     *                        responsive but consume more CPU. Defaults to 500.
     */
    explicit ClipboardManager(size_t maxHistory = 100,
                              unsigned int pollIntervalMs = 500);

    /**
     * @brief Destructor — ensures the polling loop is stopped before the
     *        object is destroyed.
     *
     * Calls `stop()` so that if the caller forgets, the background thread is
     * at least signalled to exit. The caller is still responsible for joining
     * the thread before the ClipboardManager object goes out of scope.
     */
    ~ClipboardManager();

    /**
     * @brief Register a callback invoked each time a new entry is captured.
     *
     * The callback is called from the polling thread (the same thread that
     * called `start()`), not from the thread that called `onNewEntry()`.
     * Keep the callback short and non-blocking to avoid delaying the poll loop.
     *
     * Passing a new callback replaces any previously registered one.
     * Pass an empty `std::function` (or call `onNewEntry({})`) to clear it.
     *
     * @param cb  A callable matching `void(const ClipboardEntry&)`.
     *            Moved into internal storage for efficiency.
     */
    void onNewEntry(NewEntryCallback cb);

    /**
     * @brief Search for entries containing the specified keyword.
     *
     * @param keyword  The string to search for.
     * @return         A vector of matching ClipboardEntry objects.
     */
    std::vector<ClipboardEntry> search(const std::string &keyword) const;

    /**
     * @brief Save the current history to a file.
     *
     * @param path  The file path where the history should be saved.
     */
    void saveHistory(const std::string &path) const;

    /**
     * @brief Load history entries from a file, replacing the current history.
     *
     * @param path  The file path from which to load the history.
     */
    void loadHistory(const std::string &path);

    /**
     * @brief Start the clipboard polling loop. Blocks until `stop()` is called.
     *
     * This method is designed to run on a dedicated thread. It loops
     * indefinitely, sleeping for `m_pollIntervalMs` milliseconds between
     * each clipboard read, until `m_running` is set to false by `stop()`.
     *
     * Calling `start()` a second time while it is already running is
     * undefined — do not do it.
     */
    void start();

    /**
     * @brief Signal the polling loop to exit on its next iteration.
     *
     * Sets `m_running = false`. The loop will exit after at most one more
     * sleep interval. This method is safe to call from any thread, including
     * a signal handler (though a `std::atomic<bool>` would be more correct —
     * see ROADMAP_AND_TASKS.md Task 1.1).
     */
    void stop();

    /**
     * @brief Return a copy of the current history, oldest entry first.
     *
     * Returns by value (a full copy) so the caller can iterate the snapshot
     * without holding any lock, even if the background thread continues to
     * modify `m_history`. This is safe as long as the caller does not outlive
     * the manager.
     *
     * @return A deque of ClipboardEntry objects ordered from oldest to newest.
     */
    std::deque<ClipboardEntry> history() const;

    /**
     * @brief Erase all stored history entries and reset the last-seen value.
     *
     * After this call `history()` returns an empty deque and the manager
     * treats the next clipboard read as a genuinely new entry even if the
     * clipboard content has not changed.
     */
    void clearHistory();

    /**
     * @brief Write text to the system clipboard.
     *
     * @param text  The text to write to the clipboard.
     */
    bool pasteEntry(size_t index);

private:
    /**
     * @brief Read the current plain-text content of the system clipboard.
     *
     * This is a pure platform-specific function with no shared logic.
     * Three separate implementations are compiled depending on the target OS:
     *   - clipboard_mac.mm   (macOS)  — uses NSPasteboard via Objective-C++
     *   - clipboard_win.cpp  (Windows) — uses Win32 OpenClipboard / CF_UNICODETEXT
     *   - clipboard_linux.cpp (Linux)  — shells out to xclip via popen()
     *
     * @return The clipboard text, or an empty string if the clipboard holds
     *         non-text data, is empty, or cannot be read.
     */
    std::string readClipboard();

    /**
     * @brief Write text to the system clipboard.
     *
     * @param text  The text to write to the clipboard.
     */
    void writeClipboard(const std::string &text);

    /**
     * @brief Check if a text entry exists in the history.
     *
     * @param text  The text to search for.
     * @return The index of the text if it exists in the history, -1 otherwise.
     */
    int exists(const std::string &text);

    /// Mutex to protect access to the history and last-seen values.
    /// This is needed because `history()` and `clearHistory()` can be called
    /// from any thread, while `start()` modifies these values on the polling thread.
    mutable std::mutex m_mutex;

    /// Maximum number of history entries to keep before evicting the oldest.
    size_t m_maxHistory;

    /// Milliseconds to sleep between successive clipboard reads.
    unsigned int m_pollIntervalMs;

    /// Polling loop control flag. Set to true by start(), false by stop().
    /// NOTE: accessed from two threads without synchronisation — a known
    /// limitation to be fixed with std::atomic<bool> (Task 1.1 in roadmap).
    std::atomic<bool> m_running{false};

    /// The last clipboard text we acted on, used to detect changes.
    /// Storing this avoids firing the callback when the clipboard content
    /// is re-read but has not actually changed since the last poll.
    std::string m_lastSeen;

    /// Bounded history of captured clipboard entries, oldest first.
    std::deque<ClipboardEntry> m_history;

    /// Optional user-registered callback; empty (falsy) if not set.
    NewEntryCallback m_callback;
};
