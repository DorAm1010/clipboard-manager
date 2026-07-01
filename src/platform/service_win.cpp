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
 *   - IPC: a TCP socket bound to the loopback interface (127.0.0.1).
 *
 * Why loopback TCP and not AF_UNIX?
 *   Windows 10 (1803+) does support AF_UNIX sockets, and we used to. But a
 *   Unix-domain socket leaves a *file* (a reparse point with tag
 *   IO_REPARSE_TAG_AF_UNIX) on disk. When the daemon is force-killed
 *   (TerminateProcess, used by `--kill`) or crashes, that socket file is
 *   orphaned — and an orphaned AF_UNIX socket file cannot be deleted from user
 *   space (every DeleteFile/remove returns ERROR_CANT_ACCESS_FILE / 1920).
 *   The next daemon start then fails to bind() with WSAEINVAL forever, leaving
 *   `--history` permanently broken until a reboot. A loopback TCP socket has no
 *   on-disk artifact: when the process dies the OS frees the port immediately,
 *   so the whole orphan problem disappears.
 *
 *   The price is that a bare TCP loopback socket has no access control beyond
 *   "can you reach 127.0.0.1" — unlike the AF_UNIX socket (chmod'd to 0600),
 *   ANY local process, even one running under a different Windows user account
 *   (Fast User Switching, RDP, etc.), could port-scan 127.0.0.1 and read the
 *   clipboard history with no other access required. We close that with a
 *   capability token: the daemon writes a random, hex-encoded token (see
 *   generateToken() below) alongside the port into a file in the data dir,
 *   and only a client that presents the matching token is served. Sized for
 *   this local, ephemeral use case, not as a cryptographic key — but it is
 *   the actual security boundary here: unguessable regardless of the port, so
 *   cross-account/network-level scanning is defeated outright.
 *
 *   What this does NOT protect against: another process running under the
 *   SAME Windows user account reading the port file directly. We deliberately
 *   do not harden that further (e.g. with a Windows ACL/DACL on the data dir —
 *   see paths.cpp, which is honest that fs::permissions() is a best-effort
 *   no-op on Windows). A same-user process already has a strictly easier path
 *   to the same data: it can call GetClipboardData() directly (the same API
 *   this app uses) for the live clipboard, or read history.txt for everything
 *   persisted — both with identical, unenforced Windows permissions. Adding a
 *   real ACL to just the port file would protect the smallest, most ephemeral
 *   piece of this data while leaving the larger, persistent one (history.txt)
 *   exactly as exposed, which isn't a coherent security posture. So: this is
 *   an intentional scope boundary, not an oversight.
 *
 * Winsock requires WSAStartup()/WSACleanup() around all socket usage, uses
 * SOCKET/INVALID_SOCKET instead of int/-1, and closesocket() instead of close().
 */

#ifdef _WIN32

#include "service.hpp"
#include "ClipboardManager.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h> // must come before windows.h
#include <ws2tcpip.h>
#include <windows.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <cstdio>
#include <cstring> // strlen
#include <cstdlib> // EXIT_SUCCESS / EXIT_FAILURE

// Link against the Winsock library. This pragma only takes effect under MSVC;
// other toolchains (MinGW/GCC, Clang) ignore it, so the CMake build also links
// ws2_32 explicitly for the Windows target.
#pragma comment(lib, "Ws2_32.lib")

namespace
{
    // Where the daemon advertises its loopback port + access token. Replaces
    // the AF_UNIX socket file used previously. The token itself (not this
    // file's permissions, which are not hardened on Windows — see the file
    // header comment) is what keeps other-account/network access out.
    std::string portFilePath()
    {
        return getDataDir() + "clipboard-manager.port";
    }

    // A 128-bit random token, hex-encoded. Used as a capability: a client must
    // present it to be served, which limits access to processes that can read the
    // owner-only port file.
    std::string generateToken()
    {
        std::random_device rd;
        std::mt19937_64 gen(
            (static_cast<uint64_t>(rd()) << 32) ^ rd() ^
            static_cast<uint64_t>(GetCurrentProcessId()));
        std::uniform_int_distribution<uint64_t> dist;

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        oss << std::setw(16) << dist(gen) << std::setw(16) << dist(gen);
        return oss.str();
    }
}

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
        std::remove(portFilePath().c_str());
        return EXIT_FAILURE;
    }

    // Windows has no SIGTERM; TerminateProcess is the closest equivalent. The
    // daemon leaves no socket file behind (loopback TCP), so the only cleanup
    // needed is the PID and port files — both are ordinary files that remove()
    // can delete.
    if (TerminateProcess(hProcess, 0))
    {
        std::cout << "Daemon stopped successfully.\n";
        CloseHandle(hProcess);
        std::remove(Service::PID_FILE_PATH.c_str()); // Clean up PID file
        std::remove(portFilePath().c_str());         // Clean up port/token file
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

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET)
    {
        std::cerr << "Failed to create socket. WSA error " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    // Bind to the loopback interface only (never the public network) on an
    // ephemeral port (port 0 → the OS picks a free one). Restricting to
    // 127.0.0.1 means the port is unreachable from other hosts.
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        std::cerr << "Failed to bind socket. WSA error " << err << "\n";
        closesocket(server_fd);
        WSACleanup();
        return;
    }

    // Discover the port the OS assigned so we can advertise it to clients.
    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    if (getsockname(server_fd, reinterpret_cast<sockaddr *>(&bound), &bound_len) == SOCKET_ERROR)
    {
        std::cerr << "Failed to read bound port. WSA error " << WSAGetLastError() << "\n";
        closesocket(server_fd);
        WSACleanup();
        return;
    }
    const unsigned short port = ntohs(bound.sin_port);

    if (listen(server_fd, 10) == SOCKET_ERROR)
    {
        std::cerr << "Failed to listen on socket. WSA error " << WSAGetLastError() << "\n";
        closesocket(server_fd);
        WSACleanup();
        return;
    }

    // Generate a per-run access token and advertise "<port>\n<token>" in the
    // owner-only data dir. Written only after the socket is ready to accept, so
    // a client that successfully reads the file can immediately connect.
    const std::string token = generateToken();
    {
        std::ofstream port_file(portFilePath(), std::ios::trunc);
        if (port_file.is_open())
        {
            port_file << port << "\n"
                      << token << "\n";
        }
        else
        {
            std::cerr << "Failed to write port file.\n";
        }
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

        char buffer[256];
        int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0)
        {
            closesocket(client_fd);
            continue;
        }

        // Trim trailing whitespace/newlines so the request matches whether the
        // client sent "history <tok>", "history <tok>\n", or "...\r\n". (A
        // request split across multiple reads is not handled here — fine for
        // this short command on a local stream socket.)
        std::string_view received_data(buffer, bytes_read);
        while (!received_data.empty() &&
               (received_data.back() == '\n' || received_data.back() == '\r' ||
                received_data.back() == ' ' || received_data.back() == '\t'))
        {
            received_data.remove_suffix(1);
        }

        // Expected request: "history <token>". Split on the first space.
        std::string_view command = received_data;
        std::string_view client_token;
        const auto space = received_data.find(' ');
        if (space != std::string_view::npos)
        {
            command = received_data.substr(0, space);
            client_token = received_data.substr(space + 1);
        }

        // Constant-ish comparison is unnecessary here — the token is high-entropy
        // and the channel is loopback-only — so a plain compare is fine.
        if (command == "history" && client_token == token)
        {
            std::string serialized_history = manager.serializeHistory();
            send(client_fd, serialized_history.data(),
                 static_cast<int>(serialized_history.length()), 0);
        }

        closesocket(client_fd);
    }

    closesocket(server_fd);
    std::remove(portFilePath().c_str());
    WSACleanup();
}

void Service::requestHistory()
{
    // Read the daemon's advertised loopback port and access token.
    unsigned short port = 0;
    std::string token;
    {
        std::ifstream port_file(portFilePath());
        if (!port_file.is_open() || !(port_file >> port) || !(port_file >> token) || port == 0)
        {
            std::cerr << "Failed to connect to daemon socket. Is the daemon running?\n";
            return;
        }
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "WSAStartup failed.\n";
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET)
    {
        std::cerr << "Failed to create socket.\n";
        WSACleanup();
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        std::cerr << "Failed to connect to daemon socket. Is the daemon running?\n";
        closesocket(sock);
        WSACleanup();
        return;
    }

    const std::string request = "history " + token + "\n";
    send(sock, request.data(), static_cast<int>(request.length()), 0);

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
