#include "ipc_unix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// SOCK_CLOEXEC is Linux-specific; use fcntl on other POSIX systems.
static int socket_cloexec(int domain, int type, int proto) {
#if defined(SOCK_CLOEXEC)
    return socket(domain, type | SOCK_CLOEXEC, proto);
#else
    int fd = socket(domain, type, proto);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

static constexpr size_t kMaxFrame = 16u * 1024u * 1024u; // 16 MiB

// ---------- helpers ----------

void UnixIpcServer::write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) throw std::runtime_error("IPC write error");
        p += r; n -= static_cast<size_t>(r);
    }
}
void UnixIpcServer::read_all(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) throw std::runtime_error("IPC peer disconnected");
        if (r < 0)  throw std::runtime_error("IPC read error");
        p += r; n -= static_cast<size_t>(r);
    }
}
void UnixIpcClient::write_all(int fd, const void* buf, size_t n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) throw std::runtime_error("IPC write error");
        p += r; n -= static_cast<size_t>(r);
    }
}
void UnixIpcClient::read_all(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) throw std::runtime_error("IPC peer disconnected");
        if (r < 0)  throw std::runtime_error("IPC read error");
        p += r; n -= static_cast<size_t>(r);
    }
}

// ---------- server ----------

UnixIpcServer::UnixIpcServer(std::string socket_path)
    : path_(std::move(socket_path))
{
    // Verify parent directory: owned by current user, not world-writable (§5.1).
    std::string dir = path_.substr(0, path_.rfind('/'));
    struct stat ds{};
    if (stat(dir.empty() ? "." : dir.c_str(), &ds) != 0)
        throw std::runtime_error("IPC: cannot stat parent dir");
    if (ds.st_uid != geteuid())
        throw std::runtime_error("IPC: parent dir not owned by current user");
    if (ds.st_mode & S_IWOTH)
        throw std::runtime_error("IPC: parent dir is world-writable");

    // Remove stale socket file if present.
    unlink(path_.c_str());

    listen_fd_ = socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("IPC: socket() failed");

    // Set umask 0077 so the socket file gets 0600 permissions (§5.1).
    mode_t old_mask = umask(0077);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        umask(old_mask);
        throw std::runtime_error("IPC: bind() failed: " + std::string(strerror(errno)));
    }
    umask(old_mask);

    // Verify the socket file has 0600 permissions (§5.1).
    struct stat ss{};
    if (fstat(listen_fd_, &ss) == 0) {
        if ((ss.st_mode & 0777) != 0600) {
            // Some platforms don't honour umask on sockets; force it.
            chmod(path_.c_str(), 0600);
        }
    }

    if (listen(listen_fd_, 5) != 0)
        throw std::runtime_error("IPC: listen() failed");
}

UnixIpcServer::~UnixIpcServer() { shutdown(); }

void UnixIpcServer::accept() {
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
    client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd_ < 0) throw std::runtime_error("IPC: accept() failed");
}

void UnixIpcServer::send_frame(const std::vector<std::byte>& frame) {
    write_all(client_fd_, frame.data(), frame.size());
}

std::vector<std::byte> UnixIpcServer::recv_frame() {
    // Read 4-byte big-endian length.
    uint32_t net_len;
    read_all(client_fd_, &net_len, 4);
    uint32_t len = ntohl(net_len);
    if (len > kMaxFrame)
        throw std::runtime_error("IPC: oversized frame (" + std::to_string(len) + ")");
    // Length includes the 4-byte type field + payload.
    std::vector<std::byte> frame(len);
    read_all(client_fd_, frame.data(), len);
    return frame;
}

void UnixIpcServer::close_client() {
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
}

void UnixIpcServer::shutdown() {
    close_client();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    unlink(path_.c_str());
}

// ---------- client ----------

UnixIpcClient::UnixIpcClient(std::string socket_path) {
    fd_ = socket_cloexec(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("IPC client: socket() failed");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        throw std::runtime_error("IPC client: connect() failed — is the agent running?");
}

UnixIpcClient::~UnixIpcClient() { close(); }

void UnixIpcClient::send_frame(const std::vector<std::byte>& frame) {
    write_all(fd_, frame.data(), frame.size());
}

std::vector<std::byte> UnixIpcClient::recv_frame() {
    uint32_t net_len;
    read_all(fd_, &net_len, 4);
    uint32_t len = ntohl(net_len);
    if (len > kMaxFrame)
        throw std::runtime_error("IPC: oversized frame");
    std::vector<std::byte> frame(len);
    read_all(fd_, frame.data(), len);
    return frame;
}

void UnixIpcClient::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}
