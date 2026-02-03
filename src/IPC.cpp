#include "IPC.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>

IPCServer::IPCServer() : m_socketPath(IPC_SOCKET_PATH) {}

IPCServer::~IPCServer() {
    stop();
}

bool IPCServer::start(CommandHandler handler) {
    m_handler = handler;
    
    // Create socket
    m_serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_serverSocket == -1) {
        perror("Socket create failed");
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    unlink(m_socketPath.c_str()); // Remove existing socket file

    if (bind(m_serverSocket, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("Socket bind failed");
        return false;
    }

    if (listen(m_serverSocket, 5) == -1) {
        perror("Socket listen failed");
        return false;
    }

    m_running = true;
    m_thread = std::thread(&IPCServer::acceptLoop, this);
    return true;
}

void IPCServer::stop() {
    m_running = false;
    if (m_serverSocket != -1) {
        shutdown(m_serverSocket, SHUT_RDWR);
        close(m_serverSocket);
        m_serverSocket = -1;
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    unlink(m_socketPath.c_str());
}

void IPCServer::acceptLoop() {
    while (m_running) {
        int clientSock = accept(m_serverSocket, nullptr, nullptr);
        if (clientSock == -1) {
            if (m_running) perror("Accept failed");
            continue;
        }
        handleClient(clientSock);
    }
}

void IPCServer::handleClient(int clientSocket) {
    char buffer[4096];
    std::string request;
    ssize_t bytesRead;

    // Read request
    while ((bytesRead = read(clientSocket, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        request += buffer;
        if (bytesRead < (ssize_t)sizeof(buffer) - 1) break; // Assume end of message for simplicity
    }

    // Parse args
    std::vector<std::string> args;
    std::stringstream ss(request);
    std::string arg;
    while (ss >> arg) {
        args.push_back(arg);
    }

    // Execute handler
    std::string response = "Error: Internal Handling Failed";
    if (m_handler) {
        response = m_handler(args);
    }

    // Send response
    write(clientSocket, response.c_str(), response.length());
    close(clientSocket);
}

std::string IPCClient::sendCommand(const std::vector<std::string>& args) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) return "Error: Socket creation failed";

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sock);
        return "Error: Connection failed (Daemon not running?)";
    }

    // Construct command string
    std::string cmd;
    for (const auto& a : args) {
        if (!cmd.empty()) cmd += " ";
        cmd += a;
    }

    if (write(sock, cmd.c_str(), cmd.length()) == -1) {
        close(sock);
        return "Error: Write failed";
    }
    
    // Shutdown write to signal EOF if needed, but here server reads until done.
    shutdown(sock, SHUT_WR);

    // Read response
    std::string response;
    char buffer[4096];
    ssize_t bytesRead;
    while ((bytesRead = read(sock, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
    }

    close(sock);
    return response;
}
