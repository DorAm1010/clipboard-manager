#!/usr/bin/env bash
#
# run-linux-tests.sh — headless integration tests for the clipboard manager on Linux.
#
# WHY THIS EXISTS
#   The clipboard manager's Linux backend shells out to `xclip`, which needs a
#   running X11 display. Inside a container (or any headless box) there is no
#   real display, so we spin up a *virtual* one with Xvfb. This script then
#   drives the app the way a user would — copying text, asking for history,
#   pasting — and checks the output automatically, printing PASS/FAIL.
#
# HOW TO RUN (inside the Docker container, from /app):
#   bash docker/run-linux-tests.sh
#
# EXIT CODE: 0 if every test passed, 1 if any failed (so CI can detect failure).

# 'set -u' makes the script error out if we ever use an undefined variable —
# a cheap way to catch typos. We deliberately DO NOT use 'set -e' (exit on any
# error), because we want the suite to keep running and report every failure
# rather than stopping at the first one.
set -u

# ─── Configuration ──────────────────────────────────────────────────────────
BIN="./build/clipboard-manager" # the binary, relative to /app
HIST="$HOME/.local/share/clipboard-manager/history.txt"

# Timing knobs. The app polls the clipboard every 500 ms, so we sleep longer
# than that to be sure a change is noticed. If tests flake on a slow machine,
# bump these up.
WARMUP=1.5    # time to let the app start and "seed" the (empty) clipboard
POLL_WAIT=1.5 # time to let the poll loop notice a copy we just made

# ─── Virtual display (Xvfb) ─────────────────────────────────────────────────
# Start ONE virtual X server on display :99 for the whole suite. "&" runs it in
# the background; we save its PID so we can shut it down cleanly at the end.
Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
XVFB_PID=$!
export DISPLAY=:99 # tells xclip (and the app) which display to talk to

# 'trap ... EXIT' registers cleanup that runs whenever the script exits, for any
# reason — so we never leave a stray Xvfb process behind.
trap 'kill "$XVFB_PID" 2>/dev/null' EXIT

sleep 1 # give Xvfb a moment to come up before anything uses it

# Start from a clean slate so test assertions are deterministic.
rm -f "$HIST"

# Sanity check: bail early with a clear message if the app wasn't built.
if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not found. Build the project first (cmake --build build)."
    exit 1
fi

# ─── Test helpers ───────────────────────────────────────────────────────────
PASS_COUNT=0
FAIL_COUNT=0

# assert_contains <name> <output> <needle>
#   Passes if <output> contains the literal string <needle>.
#   grep -F = fixed string (so characters like '|' and '⏎' are matched literally,
#   not as regex); -q = quiet (we only care about the exit status).
assert_contains() {
    local name="$1" haystack="$2" needle="$3"
    if printf '%s' "$haystack" | grep -qF -- "$needle"; then
        echo "  PASS: $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $name"
        echo "        expected to find: [$needle]"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# assert_equals <name> <actual> <expected>
assert_equals() {
    local name="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "  PASS: $name"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $name (expected [$expected], got [$actual])"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# drive_manager <clipboard_text> <commands>
#   The core trick. A subshell feeds the app's STDIN on a timer:
#     1. wait for the app to start (and seed the empty clipboard),
#     2. put <clipboard_text> on the clipboard WHILE the app is watching
#        (this matters: the app only captures copies made AFTER it starts),
#     3. wait for the poll loop to catch it,
#     4. send <commands> (e.g. 'h\nq\n') to the app's command prompt.
#   The whole thing is piped into the app; we capture stdout+stderr and return it.
#   '%b' makes printf interpret the \n escapes in <commands>.
drive_manager() {
    local clip_text="$1" commands="$2"
    (
        sleep "$WARMUP"
        printf '%s' "$clip_text" | xclip -selection clipboard
        sleep "$POLL_WAIT"
        printf '%b' "$commands"
    ) | timeout 10 "$BIN" 2>&1
}

# drive_manager_seq <commands> <clip1> <clip2> ...
#   Like drive_manager, but copies SEVERAL values in order (one poll-wait apart)
#   before sending <commands>. Used by the paste tests: by capturing two
#   different items and then pasting the OLDER one, the clipboard is forced to
#   actually change — so the test proves the *app* did the paste, not the
#   harness's own xclip. (With a single value, the clipboard already holds it,
#   making "did paste work?" impossible to tell apart from "nothing happened".)
drive_manager_seq() {
    local commands="$1"
    shift
    (
        sleep "$WARMUP"
        local c
        for c in "$@"; do
            printf '%s' "$c" | xclip -selection clipboard
            sleep "$POLL_WAIT"
        done
        printf '%b' "$commands"
    ) | timeout 10 "$BIN" 2>&1
}

echo "=== Linux integration tests ==="

# ─── Test 1: capture (xclip read) ───────────────────────────────────────────
# Copy text while the app is running; it should appear in the history.
out=$(drive_manager "hello linux" 'h\nq\n')
assert_contains "Test 1: captures text copied while running" "$out" "hello linux"

# ─── Test 2: large content round-trip (verifies the fread byte-count fix) ────
# The read buffer is 256 bytes. A 1000-byte payload forces several reads plus a
# partial final one. A buggy reader would pad to a 256-byte boundary.
#   We capture the big payload, THEN a short sentinel "x" (so the clipboard now
#   holds "x", not the big text). Pasting entry 2 (the big one) must change the
#   clipboard back to exactly 1000 bytes — which only works if the app both read
#   the full 1000 bytes AND wrote them back.
# 'head -c 1000 /dev/zero | tr' builds 1000 'A's with no external dependency.
big=$(head -c 1000 /dev/zero | tr '\0' 'A')
drive_manager_seq 'p 2\nq\n' "$big" "x" >/dev/null
sleep 0.3
count=$(xclip -o -selection clipboard 2>/dev/null | wc -c | tr -d ' ')
assert_equals "Test 2: 1000-byte content round-trips exactly" "$count" "1000"

# ─── Test 3: paste (xclip write) ────────────────────────────────────────────
# Capture "first" then "second" (clipboard now holds "second"). Pasting entry 2
# ("first") must change the clipboard to "first" — proving the app pulled an
# older entry from history and wrote it, not that the clipboard happened to
# already hold the value.
drive_manager_seq 'p 2\nq\n' "first" "second" >/dev/null
sleep 0.3
pasted=$(xclip -o -selection clipboard 2>/dev/null)
assert_equals "Test 3: paste writes an older entry to the clipboard" "$pasted" "first"

# ─── Test 4: multi-line content survives save/load as ONE entry ─────────────
# Copy text with real newlines, quit (which saves), then relaunch and show
# history. It must come back as a single row with ⏎ markers, not split apart.
drive_manager $'l1\nl2\nl3' 'q\n' >/dev/null
relaunch=$(timeout 10 "$BIN" <<<$'h\nq' 2>&1)
assert_contains "Test 4: multi-line entry persists as one row" "$relaunch" "l1⏎l2⏎l3"

# ─── Test 5: data file is at the XDG path ───────────────────────────────────
if [ -f "$HIST" ]; then
    echo "  PASS: Test 5: history file exists at XDG path"
    PASS_COUNT=$((PASS_COUNT + 1))
else
    echo "  FAIL: Test 5: history file missing ($HIST)"
    FAIL_COUNT=$((FAIL_COUNT + 1))
fi

# ─── Test 6: persistence across restarts ────────────────────────────────────
# A brand-new process should load the saved history (non-empty table header).
persist=$(timeout 10 "$BIN" <<<$'h\nq' 2>&1)
assert_contains "Test 6: history persists across restarts" "$persist" "Clipboard History"

# ─── Test 7: content containing '|' loads without crashing ──────────────────
# Regression test for the loadHistory parser: a pipe in the content used to make
# stoi throw and abort the program on startup.
drive_manager "a|b|c|d" 'q\n' >/dev/null
timeout 10 "$BIN" <<<$'q' >/dev/null 2>&1
code=$?
assert_equals "Test 7: pipe-in-content loads without crashing (exit 0)" "$code" "0"

# ─── Summary ────────────────────────────────────────────────────────────────
echo
echo "==================================="
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
echo "==================================="

# The final command's exit status becomes the script's exit code: 0 when no
# failures, 1 otherwise. CI uses this to mark the job green or red.
[ "$FAIL_COUNT" -eq 0 ]
