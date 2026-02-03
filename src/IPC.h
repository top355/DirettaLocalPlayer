#pragma once
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

// Simple IPC Implementation using Unix Domain Sockets
// Protocol: 
// Client connects, sends "COMMAND arg1 arg2...", waits for response, closes.
// Server accepts, parses, executes callback, sends response, closes.

#define IPC_SOCKET_PATH "/tmp/diretta_player.sock"

class IPCServer {
public:
    using CommandHandler = std::function<std::string(const std::vector<std::string>&)>;

    IPCServer();
    ~IPCServer();

    bool start(CommandHandler handler);
    void stop();

private:
    void acceptLoop();
    void handleClient(int clientSocket);

    std::string m_socketPath;
    int m_serverSocket = -1;
    std::atomic<bool> m_running {false};
    std::thread m_thread;
    CommandHandler m_handler;
};

class IPCClient {
public:
    static std::string sendCommand(const std::vector<std::string>& args);
};
