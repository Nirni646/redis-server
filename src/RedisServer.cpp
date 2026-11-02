#include "../include/RedisServer.h"
#include "../include/RedisCommandHandler.h"
#include "../include/RedisDatabase.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static RedisServer* globalServer = nullptr;

static void signalHandler(int){
    if(globalServer) globalServer->shutdown();
}

RedisServer::RedisServer(int port, int saveIntervalSeconds)
    : port(port), saveInterval(saveIntervalSeconds), server_socket(-1), running(true), activeClients(0){
    globalServer = this;
}

void RedisServer::setupSignalHandler(){
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);    // Ctrl+C
    sigaction(SIGTERM, &sa, nullptr);   // kill / docker stop
    signal(SIGPIPE, SIG_IGN);           // writing to a closed socket must not kill the server
}

// Only async-signal-safe calls here: an atomic store and shutdown(2).
// shutdown() on the listening socket makes the blocked accept() return.
void RedisServer::shutdown(){
    running = false;
    int fd = server_socket.load();
    if(fd != -1) ::shutdown(fd, SHUT_RDWR);
}

static bool sendAll(int fd, const std::string& data){
    size_t sent = 0;
    while(sent < data.size()){
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if(n < 0){
            if(errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// One thread per client. Keeps reading until the client disconnects, sends QUIT or the server stops.
void RedisServer::handleClient(int clientSocket){
    RedisCommandHandler handler;
    std::string buffer;
    char chunk[4096];
    bool closeConnection = false;

    while(running && !closeConnection){
        ssize_t n = recv(clientSocket, chunk, sizeof(chunk), 0);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0) break;                                // client closed the connection (or error)
        buffer.append(chunk, static_cast<size_t>(n));

        // A single recv() can contain several pipelined commands, or only part of one.
        std::string replies;
        while(true){
            RedisCommandHandler::Args args;
            std::string error;
            auto status = handler.parseCommand(buffer, args, error);
            if(status == RedisCommandHandler::ParseStatus::INCOMPLETE) break;
            if(status == RedisCommandHandler::ParseStatus::ERROR){
                replies += "-ERR Protocol error: " + error + "\r\n";
                closeConnection = true;
                break;
            }
            if(args.empty()) continue;
            replies += handler.processCommand(args, closeConnection);
            if(closeConnection) break;
        }
        if(!replies.empty() && !sendAll(clientSocket, replies)) break;
    }

    {
        std::lock_guard<std::mutex> lock(clientMutex);
        clientSockets.erase(clientSocket);
        close(clientSocket);
    }
    --activeClients;
}

// Background persistence: dump the database every saveInterval seconds (default 5 * 60).
// Sleeps in short slices so shutdown does not have to wait for a full interval.
void RedisServer::persistenceLoop(){
    auto lastSave = std::chrono::steady_clock::now();
    while(running){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(saveInterval <= 0) continue;
        auto now = std::chrono::steady_clock::now();
        if(now - lastSave >= std::chrono::seconds(saveInterval)){
            lastSave = now;
            if(RedisDatabase::getInstance().save()) std::cout << "Database dumped to disk\n";
            else std::cerr << "Background save failed\n";
        }
    }
}

bool RedisServer::run(){
    setupSignalHandler();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0){
        std::cerr << "Error Creating Server Socket\n";
        return false;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if(bind(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0){
        std::cerr << "Error Binding Server Socket (is port " << port << " already in use?)\n";
        close(sock);
        return false;
    }

    if(listen(sock, 128) < 0){
        std::cerr << "Error Listening on Server Socket\n";
        close(sock);
        return false;
    }

    server_socket = sock;
    std::cout << "RedisServer Listening on Port " << port << "\n" << std::flush;

    std::thread persistenceThread(&RedisServer::persistenceLoop, this);

    while(running){
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSocket = accept(sock, (struct sockaddr*)&clientAddr, &clientLen);
        if(clientSocket < 0){
            if(!running) break;                          // woken up by shutdown()
            if(errno == EINTR) continue;
            std::cerr << "Error Accepting Client Connection\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int nodelay = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        {
            std::lock_guard<std::mutex> lock(clientMutex);
            clientSockets.insert(clientSocket);
        }
        ++activeClients;
        std::thread(&RedisServer::handleClient, this, clientSocket).detach();
    }

    // ---- graceful shutdown
    std::cout << "\nShutting down...\n";
    server_socket = -1;
    close(sock);

    {   // wake every client thread that is blocked in recv()
        std::lock_guard<std::mutex> lock(clientMutex);
        for(int fd : clientSockets) ::shutdown(fd, SHUT_RDWR);
    }
    for(int i = 0; i < 500 && activeClients > 0; ++i){
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    persistenceThread.join();
    if(RedisDatabase::getInstance().save()) std::cout << "Final database dump written\n";
    else std::cerr << "Final database dump FAILED\n";

    std::cout << "Server Shutdown Complete\n";
    return true;
}
