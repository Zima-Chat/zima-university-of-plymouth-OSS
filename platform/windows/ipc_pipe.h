#pragma once
#include "../interfaces.h"
#include "../../agent/secure_buffer.h"
#include <string>
#include <vector>

// Windows named pipe IPC (§5.1).
// Pipe path: \\.\pipe\assistant-agent-<SID>
// Created with PIPE_REJECT_REMOTE_CLIENTS and a security descriptor that
// grants GENERIC_READ|GENERIC_WRITE only to the creator's SID.
class WindowsIpcServer {
public:
    explicit WindowsIpcServer(std::string pipe_path);
    ~WindowsIpcServer();

    void accept();
    void send_frame(const std::vector<std::byte>& frame);
    std::vector<std::byte> recv_frame();
    void close_client();
    void shutdown();
    const std::string& path() const noexcept { return path_; }

    // Switch the channel to authenticated encryption for all subsequent frames.
    void enable_encryption(SecureBuffer key);

private:
    std::string path_;
    void* listen_handle_ = nullptr; // HANDLE
    void* client_handle_ = nullptr;
    bool         encrypted_ = false;
    SecureBuffer enc_key_;
};

class WindowsIpcClient {
public:
    explicit WindowsIpcClient(std::string pipe_path);
    ~WindowsIpcClient();

    void send_frame(const std::vector<std::byte>& frame);
    std::vector<std::byte> recv_frame();
    void close();

    // Switch the channel to authenticated encryption for all subsequent frames.
    void enable_encryption(SecureBuffer key);

private:
    void* handle_ = nullptr;
    bool         encrypted_ = false;
    SecureBuffer enc_key_;
};
