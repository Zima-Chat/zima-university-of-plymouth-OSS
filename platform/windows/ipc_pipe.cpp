#include "ipc_pipe.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#include <stdexcept>
#include <string>
#include <cstring>
#include <vector>
#include <sodium.h>

static constexpr DWORD kMaxFrame = 16u * 1024u * 1024u;
static constexpr DWORD kBufSize  = 65536;

static void write_all(HANDLE h, const void* buf, DWORD n) {
    const char* p = static_cast<const char*>(buf);
    while (n > 0) {
        DWORD written = 0;
        if (!WriteFile(h, p, n, &written, nullptr) || written == 0)
            throw std::runtime_error("IPC pipe write error");
        p += written; n -= written;
    }
}
static void read_all(HANDLE h, void* buf, DWORD n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        DWORD read_bytes = 0;
        if (!ReadFile(h, p, n, &read_bytes, nullptr) || read_bytes == 0)
            throw std::runtime_error("IPC pipe read error or disconnect");
        p += read_bytes; n -= read_bytes;
    }
}

// ---------- Encrypted frame codec ----------
// Encrypted wire frame: [4-byte BE total][24-byte nonce][ciphertext+16-byte MAC]
// where total = 24 + plaintext_len + 16. The plaintext is the frame "body"
// (the [type][CBOR] portion); the unencrypted 4-byte length prefix that
// make_frame() prepends is stripped before encryption and replaced by this
// transport framing. crypto_secretbox (XSalsa20-Poly1305) gives confidentiality
// and authentication; a fresh random nonce per frame is safe with its 192-bit
// nonce space.
static void send_encrypted(HANDLE h, const SecureBuffer& key,
                           const std::byte* body, size_t body_len) {
    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    randombytes_buf(nonce, sizeof nonce);

    std::vector<unsigned char> ct(body_len + crypto_secretbox_MACBYTES);
    crypto_secretbox_easy(
        ct.data(),
        reinterpret_cast<const unsigned char*>(body), body_len,
        nonce, static_cast<const unsigned char*>(key.data()));

    uint32_t total   = static_cast<uint32_t>(sizeof nonce + ct.size());
    uint32_t net_len = htonl(total);
    write_all(h, &net_len, 4);
    write_all(h, nonce, sizeof nonce);
    write_all(h, ct.data(), static_cast<DWORD>(ct.size()));
}

static std::vector<std::byte> recv_encrypted(HANDLE h, const SecureBuffer& key) {
    uint32_t net_len;
    read_all(h, &net_len, 4);
    uint32_t total = ntohl(net_len);
    if (total > kMaxFrame ||
        total < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES)
        throw std::runtime_error("IPC: bad encrypted frame size");

    unsigned char nonce[crypto_secretbox_NONCEBYTES];
    read_all(h, nonce, sizeof nonce);

    size_t ct_len = total - sizeof nonce;
    std::vector<unsigned char> ct(ct_len);
    read_all(h, ct.data(), static_cast<DWORD>(ct_len));

    std::vector<std::byte> body(ct_len - crypto_secretbox_MACBYTES);
    if (crypto_secretbox_open_easy(
            reinterpret_cast<unsigned char*>(body.data()),
            ct.data(), ct_len, nonce,
            static_cast<const unsigned char*>(key.data())) != 0)
        throw std::runtime_error("IPC: frame authentication failed");
    return body;
}

// Resolve the current process user's SID as an SDDL string (e.g. "S-1-5-21-...").
static std::string current_user_sid_string() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        throw std::runtime_error("IPC: OpenProcessToken failed");

    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len); // query size
    std::vector<unsigned char> buf(len ? len : 1);
    if (!GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
        CloseHandle(token);
        throw std::runtime_error("IPC: GetTokenInformation failed");
    }
    CloseHandle(token);

    auto* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
    LPSTR sid_str = nullptr;
    if (!ConvertSidToStringSidA(tu->User.Sid, &sid_str))
        throw std::runtime_error("IPC: ConvertSidToStringSid failed");
    std::string s(sid_str);
    LocalFree(sid_str);
    return s;
}

// Build a SECURITY_DESCRIPTOR that grants Read+Write to the current user only.
// A DACL with a single allow ACE implicitly denies everyone else, so no deny
// ACEs are needed. (The previous CREATOR OWNER + deny-Everyone form actually
// denied the owner, since CREATOR OWNER is not substituted for a pipe's own
// DACL — it only resolves through inheritance.)
static SECURITY_ATTRIBUTES make_pipe_sa() {
    const std::string sddl = "D:(A;;GRGW;;;" + current_user_sid_string() + ")";
    PSECURITY_DESCRIPTOR psd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            sddl.c_str(), SDDL_REVISION_1, &psd, nullptr))
        throw std::runtime_error("IPC: failed to build security descriptor");
    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = psd;
    sa.bInheritHandle       = FALSE;
    return sa;
}

// ---------- server ----------

WindowsIpcServer::WindowsIpcServer(std::string pipe_path)
    : path_(std::move(pipe_path))
{
    SECURITY_ATTRIBUTES sa = make_pipe_sa();
    HANDLE h = CreateNamedPipeA(
        path_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, kBufSize, kBufSize, 0, &sa);
    LocalFree(sa.lpSecurityDescriptor);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("IPC: CreateNamedPipe failed");
    listen_handle_ = h;
}

WindowsIpcServer::~WindowsIpcServer() { shutdown(); }

void WindowsIpcServer::accept() {
    close_client();
    if (!ConnectNamedPipe(static_cast<HANDLE>(listen_handle_), nullptr))
        if (GetLastError() != ERROR_PIPE_CONNECTED)
            throw std::runtime_error("IPC: ConnectNamedPipe failed");
    client_handle_ = listen_handle_; // single-instance pipe
}

void WindowsIpcServer::send_frame(const std::vector<std::byte>& frame) {
    HANDLE h = static_cast<HANDLE>(client_handle_);
    if (encrypted_) {
        // Strip make_frame()'s 4-byte length prefix; encrypt the body.
        if (frame.size() < 4) throw std::runtime_error("IPC: short frame");
        send_encrypted(h, enc_key_, frame.data() + 4, frame.size() - 4);
    } else {
        write_all(h, frame.data(), static_cast<DWORD>(frame.size()));
    }
}

std::vector<std::byte> WindowsIpcServer::recv_frame() {
    HANDLE h = static_cast<HANDLE>(client_handle_);
    if (encrypted_) return recv_encrypted(h, enc_key_);
    uint32_t net_len;
    read_all(h, &net_len, 4);
    uint32_t len = ntohl(net_len);
    if (len > kMaxFrame) throw std::runtime_error("IPC: oversized frame");
    std::vector<std::byte> frame(len);
    read_all(h, frame.data(), len);
    return frame;
}

void WindowsIpcServer::enable_encryption(SecureBuffer key) {
    enc_key_   = std::move(key);
    encrypted_ = true;
}

void WindowsIpcServer::close_client() {
    if (client_handle_ && client_handle_ != listen_handle_)
        CloseHandle(static_cast<HANDLE>(client_handle_));
    else if (listen_handle_) {
        // Re-arm the single pipe instance for the next client.
        DisconnectNamedPipe(static_cast<HANDLE>(listen_handle_));
    }
    client_handle_ = nullptr;

    // Reset the per-connection encrypted channel. The server object is reused
    // across clients, so the next client must start with a plaintext handshake
    // and derive its own channel key.
    encrypted_ = false;
    enc_key_   = SecureBuffer{};
}

void WindowsIpcServer::shutdown() {
    close_client();
    if (listen_handle_) {
        CloseHandle(static_cast<HANDLE>(listen_handle_));
        listen_handle_ = nullptr;
    }
}

// ---------- client ----------

WindowsIpcClient::WindowsIpcClient(std::string pipe_path) {
    HANDLE h = CreateFileA(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("IPC client: cannot open pipe — is the agent running?");
    handle_ = h;
}

WindowsIpcClient::~WindowsIpcClient() { close(); }

void WindowsIpcClient::send_frame(const std::vector<std::byte>& frame) {
    HANDLE h = static_cast<HANDLE>(handle_);
    if (encrypted_) {
        if (frame.size() < 4) throw std::runtime_error("IPC: short frame");
        send_encrypted(h, enc_key_, frame.data() + 4, frame.size() - 4);
    } else {
        write_all(h, frame.data(), static_cast<DWORD>(frame.size()));
    }
}

std::vector<std::byte> WindowsIpcClient::recv_frame() {
    HANDLE h = static_cast<HANDLE>(handle_);
    if (encrypted_) return recv_encrypted(h, enc_key_);
    uint32_t net_len;
    read_all(h, &net_len, 4);
    uint32_t len = ntohl(net_len);
    if (len > kMaxFrame) throw std::runtime_error("IPC: oversized frame");
    std::vector<std::byte> frame(len);
    read_all(h, frame.data(), len);
    return frame;
}

void WindowsIpcClient::close() {
    if (handle_) { CloseHandle(static_cast<HANDLE>(handle_)); handle_ = nullptr; }
}

void WindowsIpcClient::enable_encryption(SecureBuffer key) {
    enc_key_   = std::move(key);
    encrypted_ = true;
}

#else
// Non-Windows stubs
WindowsIpcServer::WindowsIpcServer(std::string p) : path_(std::move(p)) {}
WindowsIpcServer::~WindowsIpcServer() {}
void WindowsIpcServer::accept() {}
void WindowsIpcServer::send_frame(const std::vector<std::byte>&) {}
std::vector<std::byte> WindowsIpcServer::recv_frame() { return {}; }
void WindowsIpcServer::close_client() {}
void WindowsIpcServer::shutdown() {}
void WindowsIpcServer::enable_encryption(SecureBuffer) {}

WindowsIpcClient::WindowsIpcClient(std::string) {}
WindowsIpcClient::~WindowsIpcClient() {}
void WindowsIpcClient::send_frame(const std::vector<std::byte>&) {}
std::vector<std::byte> WindowsIpcClient::recv_frame() { return {}; }
void WindowsIpcClient::close() {}
void WindowsIpcClient::enable_encryption(SecureBuffer) {}
#endif
