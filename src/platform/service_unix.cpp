#if !defined(_WIN32)

#include "service.hpp"
#include "ClipboardManager.h"

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <filesystem>
#include <cstring>  // strlen
#include <cstdio>   // std::perror, std::remove
#include <cstdlib>  // EXIT_SUCCESS / EXIT_FAILURE

// MSG_NOSIGNAL prevents send() from raising SIGPIPE if the peer has closed the
// connection. Linux defines it in <sys/socket.h> (included above), but macOS
// does not — so we only fall back to 0 when the platform header didn't define
// it. This MUST come after <sys/socket.h> so we never redefine the real value.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace fs = std::filesystem;

int Service::daemonize()
{
    openlog("clipboard_manager_daemon", LOG_PID, LOG_DAEMON);

    // 1. First fork
    pid_t pid = fork();
    if (pid < 0)
        return EXIT_FAILURE;
    if (pid > 0)
    {
        // --- Write the Child PID to file ---
        std::ofstream pid_file(Service::PID_FILE_PATH);
        if (pid_file.is_open())
        {
            pid_file << pid; // Safely grabs the current execution PID
            pid_file.close();
        }
        else
        {
            syslog(LOG_ERR, "Failed to write PID file.");
            return EXIT_FAILURE;
        }
        _exit(0); // Exit the parent process
    }

    // 2. Create new session
    if (setsid() < 0)
        return EXIT_FAILURE;

    // 4. Set file-creation mask and working directory.
    //
    // umask(077) clears the group/other permission bits on every file the
    // daemon subsequently creates (history file, PID file, socket), so they
    // default to owner-only (0600 / 0700). The previous umask(0) made them
    // world-readable/writable — a privacy hole given the history can contain
    // passwords. We do NOT use umask(0): there is no file here that needs to be
    // group- or world-accessible.
    umask(077);
    if (chdir("/") < 0)
        return EXIT_FAILURE;

    // 5. Production-Grade File Descriptor Redirection
    int dev_null = open("/dev/null", O_RDWR);
    if (dev_null == -1)
    {
        syslog(LOG_ERR, "Failed to open /dev/null");
        return EXIT_FAILURE;
    }

    // Redirect standard streams to /dev/null safely
    dup2(dev_null, STDIN_FILENO);  // 0
    dup2(dev_null, STDOUT_FILENO); // 1
    dup2(dev_null, STDERR_FILENO); // 2

    // Now it's safe to close our original fd reference
    if (dev_null > 2)
    {
        close(dev_null);
    }

    syslog(LOG_INFO, "Daemon detached and securely bound to /dev/null.");

    closelog();
    return EXIT_SUCCESS;
}

int Service::stop()
{
    std::ifstream pid_file(Service::PID_FILE_PATH);
    if (!pid_file.is_open())
    {
        std::cerr << "Error: Daemon does not appear to be running (No PID file found).\n";
        return EXIT_FAILURE;
    }

    pid_t pid;
    pid_file >> pid;
    pid_file.close();

    std::cout << "Sending SIGTERM to daemon process " << pid << "...\n";

    // send SIGTERM signal to target PID
    if (kill(pid, SIGTERM) == 0)
    {
        std::cout << "Stop signal sent successfully.\n";
        std::remove(Service::PID_FILE_PATH.c_str());  // Clean up PID file
        std::remove(Service::SOCKET_PATH.c_str());    // Clean up stale socket file
        return EXIT_SUCCESS;
    }
    else
    {
        std::cerr << "Failed to terminate process. It might have already exited.\n";
        std::remove(Service::PID_FILE_PATH.c_str());
        std::remove(Service::SOCKET_PATH.c_str());
        return EXIT_FAILURE;
    }
}

void Service::createHistoryRequestSocket(ClipboardManager &manager)
{
    // 1. Clean up any stale socket files from a previous crashed run.
    // In production, failing to do this causes the bind() call to fail.
    if (fs::exists(Service::SOCKET_PATH))
    {
        fs::remove(Service::SOCKET_PATH);
    }

    // 2. Create the socket file descriptor
    // AF_UNIX = Unix Domain, SOCK_STREAM = Connection-oriented byte stream (TCP-like)
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        std::perror("Failed to create socket");
        return;
    }

    // 3. Set up the address structure
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    // Ensure the path fits within the operating system limits (usually 108 bytes)
    if (Service::SOCKET_PATH.length() >= sizeof(addr.sun_path))
    {
        std::cerr << "Socket path is too long.\n";
        close(server_fd);
        return;
    }
    std::strncpy(addr.sun_path, Service::SOCKET_PATH.data(), sizeof(addr.sun_path) - 1);

    // 4. Bind the socket to the filesystem path
    if (bind(server_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == -1)
    {
        std::perror("Failed to bind socket");
        close(server_fd);
        return;
    }

    // 4b. Restrict the socket to the owner. On Linux, connecting to a Unix
    // domain socket requires write permission on the socket file, so 0600
    // prevents other local users from connecting and dumping the clipboard
    // history. (On macOS socket-file permissions are not enforced for connect,
    // but setting them is harmless and documents intent; the data dir is also
    // already 0700, which blocks traversal there.) Best-effort: a failure here
    // should not take the daemon down, but do log it.
    if (chmod(Service::SOCKET_PATH.c_str(), S_IRUSR | S_IWUSR) == -1)
    {
        std::perror("Warning: failed to chmod socket to 0600");
    }

    // 5. Start listening for incoming client connections
    if (listen(server_fd, 10) == -1)
    {
        std::perror("Failed to listen on socket");
        close(server_fd);
        return;
    }

    // 6. Accept loop
    //
    // We must NOT block indefinitely in accept(), otherwise this thread can
    // never notice that the daemon is shutting down (isRunning() == false) and
    // the join() in runAsDaemon() would hang forever. Note that SO_RCVTIMEO
    // does NOT make accept() time out on macOS/BSD — it only affects recv() on
    // connected sockets. The portable solution is poll(): wait up to 1 second
    // for an incoming connection, then loop back and re-check isRunning().
    while (manager.isRunning())
    {
        struct pollfd pfd{};
        pfd.fd = server_fd;
        pfd.events = POLLIN;

        int ready = poll(&pfd, 1, 1000); // 1000 ms timeout
        if (ready <= 0)
        {
            // 0  → timed out (no connection): loop back and re-check isRunning()
            // -1 → error (e.g., EINTR from a signal): also just retry
            continue;
        }

        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd == -1)
        {
            continue;
        }

        // Handle client connection (read/write data)
        char buffer[128];
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0)
        {
            close(client_fd);
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
            send(client_fd, serialized_history.data(), serialized_history.length(), MSG_NOSIGNAL);
        }

        close(client_fd); // RAII wraps this in full systems
    }

    // Clean up
    close(server_fd);
    fs::remove(Service::SOCKET_PATH);
}

void Service::requestHistory()
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1)
    {
        std::perror("Failed to create socket");
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, Service::SOCKET_PATH.data(), sizeof(addr.sun_path) - 1);

    if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == -1)
    {
        std::perror("Failed to connect to daemon socket");
        close(sock);
        return;
    }

    const char *request = "history\n";
    send(sock, request, strlen(request), MSG_NOSIGNAL);

    char buffer[1024];
    ssize_t bytes_received;
    while ((bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0)
    {
        buffer[bytes_received] = '\0';
        std::cout << buffer;
    }

    close(sock);
}

#endif // !defined(_WIN32)