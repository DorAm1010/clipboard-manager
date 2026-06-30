/**
 * @file service_win.cpp
 * @brief Windows implementation of the daemon / IPC service.
 *
 * Compiled only on Windows (guarded by `#ifdef _WIN32`). This mirrors
 * service_unix.cpp but uses the Win32 + Winsock APIs:
 *
 *   - "Daemonizing" on Windows: there is no fork(). We detach from the
 *     console with FreeConsole() and write the PID to a file. (A truly
 *     robust background service would be a Windows Service via the SCM,
 *     but that is well beyond the scope of this learning project.)
 *   - Stopping: read the PID file and call TerminateProcess().
 *   - IPC: Windows 10 (1803+) supports AF_UNIX sockets via <afunix.h>,
 *     so the socket logic is almost identical to the POSIX version.
 *
 * Winsock requires WSAStartup()/WSACleanup() around all socket usage, uses
 * SOCKET/INVALID_SOCKET instead of int/-1, and closesocket() instead of close().
 */

#ifdef _WIN32

#include "service.hpp"
#include "ClipboardManager.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h> // must come before windows.h
#include <afunix.h>   // AF_UNIX support on Windows 10+
#include <windows.h>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <cstdio>
#include <cstring> // strlen
#include <cstdlib> // EXIT_SUCCESS / EXIT_FAILURE

// Link against the Winsock library.
#pragma comment(lib, "Ws2_32.lib")

int Service::daemonize()
{
    // Detach from the parent console so the process keeps running without a
    // visible terminal window. FreeConsole() returns nonzero on success; we
    // ignore the result because a console may legitimately not be attached.
    FreeConsole();

    // Record our PID so `--kill` can find us later.
    std::ofstream pid_file(Service::PID_FILE_PATH);
    if (pid_file.is_open())
    {
        pid_file << GetCurrentProcessId();
        pid_file.close();
    }

    return 0;
}

int Service::stop()
{
    std::ifstream pid_file(Service::PID_FILE_PATH);
    if (!pid_file.is_open())
    {
        std::cerr << "Error: Daemon does not appear to be running (No PID file found).\n";
        return EXIT_FAILURE;
    }

    DWORD pid;
    pid_file >> pid;
    pid_file.close();

    // Open the running process with termination rights.
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess == NULL)
    {
        std::cerr << "Failed to open process. It may have already exited.\n";
        std::remove(Service::PID_FILE_PATH.c_str());
        std::remove(Service::SOCKET_PATH.c_str());
        return EXIT_FAILURE;
    }

    // Windows has no SIGTERM; TerminateProcess is the closest equivalent.
    if (TerminateProcess(hProcess, 0))
    {
        std::cout << "Daemon stopped successfully.\n";
        CloseHandle(hProcess);
        std::remove(Service::PID_FILE_PATH.c_str()); // Clean up PID file
        std::remove(Service::SOCKET_PATH.c_str());   // Clean up socket file
        return EXIT_SUCCESS;
    }

    std::cerr << "Failed to terminate process.\n";
    CloseHandle(hProcess);
    return EXIT_FAILURE;
}

void Service::createHistoryRequestSocket(ClipboardManager &manager)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed.\n";
        return;
    }

    // Clean up any stale socket file from a previous crashed run, otherwise
    // bind() fails with "address already in use".
    std::remove(Service::SOCKET_PATH.c_str());

    SOCKET server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET)
    {
        std::cerr << "Failed to create socket.\n";
        WSACleanup();
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (Service::SOCKET_PATH.length() >= sizeof(addr.sun_path))
    {
        std::cerr << "Socket path is too long.\n";
        closesocket(server_fd);
        WSACleanup();
        return;
    }
    strncpy_s(addr.sun_path, sizeof(addr.sun_path), Service::SOCKET_PATH.data(), _TRUNCATE);

    if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "Failed to bind socket.\n";
        closesocket(server_fd);
        WSACleanup();
        return;
    }

    if (listen(server_fd, 10) == SOCKET_ERROR)
    {
        std::cerr << "Failed to listen on socket.\n";
        closesocket(server_fd);
        WSACleanup();
        return;
    }

    // Do not block indefinitely in accept(), otherwise this thread can never
    // notice the daemon is shutting down and the join() in runAsDaemon() would
    // hang forever. WSAPoll waits up to 1 second for an incoming connection,
    // then we loop back and re-check isRunning(). (This mirrors poll() in the
    // POSIX implementation.)
    while (manager.isRunning())
    {
        WSAPOLLFD pfd{};
        pfd.fd = server_fd;
        pfd.events = POLLIN;

        int ready = WSAPoll(&pfd, 1, 1000); // 1000 ms timeout
        if (ready <= 0)
        {
            continue; // timed out or error: re-check isRunning()
        }

        SOCKET client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd == INVALID_SOCKET)
        {
            continue;
        }

        char buffer[128];
        int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0)
        {
            closesocket(client_fd);
            continue;
        }

        // Trim trailing whitespace/newlines so the request matches whether the
        // client sent "history", "history\n", or "history\r\n". (A request split
        // across multiple reads is not handled here — fine for this 8-byte
        // command on a local stream socket.)
        std::string_view received_data(buffer, bytes_read);
        while (!received_data.empty() &&
               (received_data.back() == '\n' || received_data.back() == '\r' ||
                received_data.back() == ' ' || received_data.back() == '\t'))
        {
            received_data.remove_suffix(1);
        }
        if (received_data == "history")
        {
            std::string serialized_history = manager.serializeHistory();
            send(client_fd, serialized_history.data(),
                 static_cast<int>(serialized_history.length()), 0);
        }

        closesocket(client_fd);
    }

    closesocket(server_fd);
    std::remove(Service::SOCKET_PATH.c_str());
    WSACleanup();
}

void Service::requestHistory()
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed.\n";
        return;
    }

    SOCKET sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
    {
        std::cerr << "Failed to create socket.\n";
        WSACleanup();
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy_s(addr.sun_path, sizeof(addr.sun_path), Service::SOCKET_PATH.data(), _TRUNCATE);

    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "Failed to connect to daemon socket. Is the daemon running?\n";
        closesocket(sock);
        WSACleanup();
        return;
    }

    const char *request = "history\n";
    send(sock, request, static_cast<int>(strlen(request)), 0);

    char buffer[1024];
    int bytes_received;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0)
    {
        buffer[bytes_received] = '\0';
        std::cout << buffer;
    }

    closesocket(sock);
    WSACleanup();
}

#endif // _WIN32
