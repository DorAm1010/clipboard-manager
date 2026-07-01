#pragma once

#include "platform/paths.h"
#include "ClipboardManager.h"

#include <string>

namespace Service
{
    // Path where the PID file will be saved
    const std::string PID_FILE_PATH = getDataDir() + "clipboard-manager.pid"; // For Windows, we will adapt this dynamically
    const std::string SOCKET_PATH = getSocketPath();

    int daemonize();
    int stop();
    void createHistoryRequestSocket(ClipboardManager &manager);
    void requestHistory();

    /**
     * @brief Check whether a clipboard-manager daemon is already running.
     *
     * Reads the PID file and verifies the process it names is still alive
     * (kill(pid, 0) on Unix / OpenProcess+GetExitCodeProcess on Windows — the
     * same liveness check --kill already relies on). A stale PID file (the
     * process crashed or the machine rebooted without a clean --kill) is
     * treated as "not running" and does not block a fresh start.
     *
     * This can't fully distinguish "our own daemon" from an unrelated process
     * that happens to have been assigned the same PID since ours exited (PID
     * reuse) — a rare enough collision in practice that this project accepts
     * the same level of rigor --kill already uses, rather than adding
     * per-platform executable-path verification for a single-user tool.
     *
     * @return The PID of the running daemon if one is found, or 0 if none is.
     */
    long isAlreadyRunning();
}