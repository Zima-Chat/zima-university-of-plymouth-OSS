#pragma once
#include "../interfaces.h"
#include <string>

// Unix domain socket IPC server endpoint (§5.1, §5.3).
// Frame wire format: [4-byte big-endian length][4-byte type][CBOR payload]
// Length includes the type field. Max frame = 16 MiB.
//
// The socket is created with umask 0077 so it has 0600 permissions.
// The parent directory is checked for user ownership and no world-write bit.
class UnixIpcServer {
public:
    // Bind and listen. Fails with std::runtime_error if the directory or
    // permission checks fail (§5.1).
    explicit UnixIpcServer(std::string socket_path);
    ~UnixIpcServer();

    // Block until a client connects; after return, send/recv operate on
    // that client fd. Throws std::runtime_error on fatal error.
    void accept();

    // Send a complete wire frame to the current client.
    void send_frame(const std::vector<std::byte>& frame);

    // Receive one complete wire frame from the current client.
    // Throws std::runtime_error on disconnect or oversized frame.
    std::vector<std::byte> recv_frame();

    void close_client();
    void shutdown();

    const std::string& path() const { return path_; }

private:
    std::string path_;
    int listen_fd_  = -1;
    int client_fd_  = -1;

    static void write_all(int fd, const void* buf, size_t n);
    static void read_all (int fd, void* buf, size_t n);
};

// Unix domain socket IPC client endpoint — used by the CLI and GUI host.
class UnixIpcClient {
public:
    // Connect to the agent socket. Throws std::runtime_error if unavailable.
    explicit UnixIpcClient(std::string socket_path);
    ~UnixIpcClient();

    void send_frame(const std::vector<std::byte>& frame);
    std::vector<std::byte> recv_frame();
    void close();

private:
    int fd_ = -1;

    static void write_all(int fd, const void* buf, size_t n);
    static void read_all (int fd, void* buf, size_t n);
};
