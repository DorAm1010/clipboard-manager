// race_regression_test.cpp
//
// Regression test for the start/stop data race that could hang shutdown.
//
// Background: start() used to do `m_running.store(true)` at the top. If a
// caller invoked stop() (which stores false) *before* the freshly-spawned
// polling thread reached that store, the store(true) would clobber the stop()
// and the poll loop would never exit — so join() blocked forever and the
// program hung. The fix makes stop() the single writer (m_running is
// initialised true at construction and start() never writes it).
//
// How this test catches a regression: it runs the "launch start() on a thread,
// then immediately stop()" sequence many times. With the bug present, some
// iteration's join() hangs; the test process never finishes and CTest's
// per-test TIMEOUT (set in CMakeLists.txt) marks it as failed. With the bug
// fixed, every cycle completes and the process exits 0.
//
// Note: this is a stress/timing test, not a formal proof. A ThreadSanitizer
// build would catch the race deterministically and is a worthwhile future
// addition; the hammer-loop approach is what reproduced the bug originally.

#include "ClipboardManager.h"

#include <chrono>
#include <iostream>
#include <ostream>
#include <streambuf>
#include <thread>

int main()
{
    // The race window is at startup, so many short cycles exercise it well.
    // The original bug reproduced at roughly 1-in-3; a few hundred iterations
    // make a surviving regression virtually certain to hang.
    constexpr int kIterations = 300;

    // The manager prints "Started/Stopped" lines on every cycle. Silence its
    // stdout for the duration so the test output stays readable; restore it
    // afterwards for the final summary line.
    std::streambuf *const realCout = std::cout.rdbuf();
    class NullBuffer : public std::streambuf
    {
        int overflow(int c) override { return c; } // discard everything
    } nullBuffer;
    std::cout.rdbuf(&nullBuffer);

    for (int i = 0; i < kIterations; ++i)
    {
        // Small poll interval so a (correctly) running loop wakes quickly; the
        // history bound is irrelevant here.
        ClipboardManager mgr(/*maxHistory=*/16, /*pollIntervalMs=*/5);

        std::thread worker([&mgr]() { mgr.start(); });

        // Immediately request a stop. This is the exact ordering that used to
        // race: stop() may execute before the worker thread runs start()'s body.
        mgr.stop();

        // If the race regressed, this join() blocks forever and CTest's TIMEOUT
        // fails the test. With the fix in place it returns promptly every time.
        worker.join();
    }

    std::cout.rdbuf(realCout); // restore stdout
    std::cout << "race_regression: " << kIterations
              << " start/stop cycles completed without hanging\n";
    return 0;
}
