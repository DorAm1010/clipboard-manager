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
}