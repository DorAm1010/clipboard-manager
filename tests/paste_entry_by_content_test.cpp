// paste_entry_by_content_test.cpp
//
// Regression test for ClipboardManager::pasteEntryByContent() — added when
// the GUI popup's search feature needed content-based (not index-based)
// entry activation, since search results have no direct position in the
// full history. Verifies:
//   1. Pasting an entry not at the front promotes it to the front (MRU),
//      refreshes its timestamp, and does NOT bump copyCount (pasting an
//      existing entry isn't a fresh copy event).
//   2. Pasting the same (now-front) entry again is a harmless no-op reorder.
//   3. Pasting content that doesn't exist in history fails cleanly and
//      leaves history completely unchanged.
//
// loadHistory() seeds the deque directly from a temp file — exactly what a
// real history.txt on disk would produce — so none of this needs the
// background poller thread or a running daemon at all.
//
// NOTE: pasteEntryByContent() calls writeClipboard() internally, so running
// this test DOES overwrite whatever is currently on the real system
// clipboard (there is no dependency-injection seam to mock it out). Harmless
// on CI (ephemeral runners) and in practice for a local run, but worth
// knowing before wondering why your clipboard changed after `ctest`.

#include "ClipboardManager.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    int g_failures = 0;

    void check(bool condition, const std::string &description)
    {
        // Deliberately NOT using assert(): assert() is compiled out under
        // NDEBUG, which CMAKE_BUILD_TYPE=Release defines — exactly the
        // configuration this project's CI actually builds with. An assert()
        // here would silently vanish and the test would "pass" no matter
        // what. Plain if-checks always run, regardless of build type.
        if (!condition)
        {
            std::cerr << "FAIL: " << description << "\n";
            ++g_failures;
        }
    }

    void writeTempHistoryFile(const std::string &path)
    {
        std::ofstream out(path);
        // TIMESTAMP|content|copyCount, one entry per line, front (newest)
        // first — matches the on-disk format documented in ClipboardManager.cpp.
        out << "1700000003|charlie|1\n";
        out << "1700000002|bravo|2\n";
        out << "1700000001|alpha|1\n";
    }
}

int main()
{
    const std::string tempPath = "/tmp/paste_entry_by_content_test_history.txt";
    writeTempHistoryFile(tempPath);

    ClipboardManager manager(/*maxHistory=*/50, /*pollIntervalMs=*/500);
    manager.loadHistory(tempPath);

    // Sanity-check the seed loaded in the expected order before testing
    // anything else against it.
    {
        auto entries = manager.history();
        check(entries.size() == 3, "seed loads exactly 3 entries");
        if (entries.size() == 3)
        {
            check(entries[0].content == "charlie", "seed[0] is charlie (newest)");
            check(entries[1].content == "bravo", "seed[1] is bravo");
            check(entries[2].content == "alpha", "seed[2] is alpha (oldest)");
        }
    }

    // Pasting an entry NOT at the front should promote it to the front,
    // refresh its timestamp, and leave copyCount unchanged.
    {
        bool ok = manager.pasteEntryByContent("alpha");
        check(ok, "pasteEntryByContent(\"alpha\") succeeds");

        auto entries = manager.history();
        check(entries.size() == 3, "history still has 3 entries after promoting alpha");
        if (entries.size() == 3)
        {
            check(entries[0].content == "alpha", "alpha promoted to front");
            check(entries[0].copyCount == 1, "alpha's copyCount unchanged by paste (was 1)");
            check(entries[1].content == "charlie", "charlie now second");
            check(entries[2].content == "bravo", "bravo now third");
        }
    }

    // Pasting the same (now-front) entry again should be a harmless no-op —
    // still at the front, copyCount still untouched.
    {
        bool ok = manager.pasteEntryByContent("alpha");
        check(ok, "pasting the already-front entry again still succeeds");

        auto entries = manager.history();
        if (!entries.empty())
        {
            check(entries[0].content == "alpha", "alpha stays at front on repeat paste");
            check(entries[0].copyCount == 1, "repeat paste still doesn't bump copyCount");
        }
    }

    // Pasting content that doesn't exist in history must fail cleanly and
    // must not modify history at all.
    {
        bool ok = manager.pasteEntryByContent("does-not-exist");
        check(!ok, "pasting nonexistent content fails (returns false)");

        auto entries = manager.history();
        check(entries.size() == 3, "history unchanged after a failed paste");
        if (entries.size() == 3)
        {
            check(entries[0].content == "alpha", "order unchanged after failed paste");
            check(entries[1].content == "charlie", "order unchanged after failed paste");
            check(entries[2].content == "bravo", "order unchanged after failed paste");
        }
    }

    std::remove(tempPath.c_str());

    if (g_failures == 0)
    {
        std::cout << "paste_entry_by_content_test: all checks passed\n";
        return 0;
    }
    std::cerr << "paste_entry_by_content_test: " << g_failures << " check(s) failed\n";
    return 1;
}
