#ifndef REDIS_SERVER_H
#define REDIS_SERVER_H

#include <atomic>
#include <mutex>
#include <set>
#include <string>

class RedisServer{
public:
    // saveIntervalSeconds <= 0 disables the periodic background save
    RedisServer(int port, int saveIntervalSeconds = 300);

    // Blocks serving clients until shutdown() is called. Returns false if the socket could not be set up.
    bool run();

    // Async-signal-safe: only flips a flag and shuts the listening socket down so accept() returns.
    void shutdown();

private:
    void handleClient(int clientSocket);
    void persistenceLoop();
    void setupSignalHandler();

    int port;
    int saveInterval;
    std::atomic<int> server_socket;
    std::atomic<bool> running;
    std::atomic<int> activeClients;

    std::set<int> clientSockets;
    std::mutex clientMutex;
};

#endif
