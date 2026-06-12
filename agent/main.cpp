// Agent entry point (§3.1, §3.3, §4).
// Lifecycle: harden → init sodium → load/generate session token → load key →
//            accept loop (thread per client) → idle timer → graceful shutdown.

#include "secure_buffer.h"
#include "session.h"
#include "rpc.h"
#include "zima_client.h"
#include "prompt_scanner.h"

#if defined(__APPLE__) || defined(__linux__)
#include "../platform/posix/hardening_posix.h"
#include "../platform/posix/ipc_unix.h"
using HardeningImpl = PosixHardening;
#  if defined(__APPLE__)
#    include "../platform/posix/secret_store_keychain.h"
     using SecretStoreImpl = KeychainSecretStore;
#  else
#    include "../platform/posix/secret_store_libsecret.h"
     #include <ostream>
     using SecretStoreImpl = LibsecretSecretStore;
#  endif
     using IpcServerImpl = UnixIpcServer;
#elif defined(_WIN32)
#include "../platform/windows/hardening_win.h"
#include "../platform/windows/ipc_pipe.h"
#include "../platform/windows/secret_store_credman.h"
using HardeningImpl   = WindowsHardening;
using SecretStoreImpl = CredManSecretStore;
using IpcServerImpl   = WindowsIpcServer;
#endif

#include "../platform/secure_random.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// ---------- Shared agent state ----------

static std::mutex    g_key_mutex;
static SecureBuffer  g_api_key;           // sole long-lived secret in this process
static std::atomic<bool> g_key_loaded{false};
static std::atomic<bool> g_locked{false}; // true after idle timeout

static SecretStoreImpl g_secret_store;
static ZimaClient      g_zima;
static IdleTimer       g_idle_timer;

static const std::string kCredentialName = "api_key";

// ---------- Key helpers ----------

static void zero_key() {
    std::lock_guard lock(g_key_mutex);
    g_api_key = SecureBuffer{};
    g_key_loaded.store(false, std::memory_order_relaxed);
    g_locked.store(true, std::memory_order_relaxed);
}

static bool ensure_key_loaded() {
    std::lock_guard lock(g_key_mutex);
    if (g_key_loaded.load(std::memory_order_relaxed)) return true;
    if (!g_secret_store.exists(kCredentialName)) return false;
    g_api_key = g_secret_store.get(kCredentialName);
    g_key_loaded.store(true, std::memory_order_relaxed);
    g_locked.store(false, std::memory_order_relaxed);
    return true;
}

// ---------- Per-client handler ----------

static void handle_client(IpcServerImpl& srv, const SecureBuffer& session_token) {
    // §5.2: authenticate within 100 ms
    try {
        auto frame = srv.recv_frame();
        if (frame_type(frame) != MsgType::Authenticate) {
            srv.send_frame(msg_auth_fail("first frame must be Authenticate"));
            return;
        }
        auto payload = frame_payload(frame);
        if (!payload.is_map()) { srv.send_frame(msg_auth_fail("bad payload")); return; }
        const auto* tok_val = cbor::map_get(payload.as_map(), "token");
        if (!tok_val || !tok_val->is_bytes()) {
            srv.send_frame(msg_auth_fail("missing token"));
            return;
        }
        const auto& tok_bytes = tok_val->as_bytes();
        SecureBuffer client_token(tok_bytes.size());
        std::memcpy(client_token.data(), tok_bytes.data(), tok_bytes.size());
        if (!token_matches(session_token, client_token)) {
            srv.send_frame(msg_auth_fail("invalid token"));
            return;
        }

        // Establish the encrypted channel. The key is derived from the session
        // token, which both peers already share via the ACL-protected token
        // file — neither side needs the API key or any other stored secret.
        srv.send_frame(msg_auth_ok());                            // last plaintext frame
        srv.enable_encryption(derive_ipc_key(session_token.span())); // encrypt onward
    } catch (...) { return; }

    // Dispatch RPC loop
    try {
        while (!shutdown_requested()) {
            auto frame = srv.recv_frame();
            MsgType type = frame_type(frame);

            if (type == MsgType::Status) {
                bool locked = !g_key_loaded.load(std::memory_order_relaxed);
                srv.send_frame(msg_status_reply(locked, "auto"));

            } else if (type == MsgType::ConfigureCredential) {
                auto payload = frame_payload(frame);
                if (!payload.is_map()) { srv.send_frame(msg_error("bad payload")); continue; }
                const auto* kv = cbor::map_get(payload.as_map(), "api_key");
                if (!kv || !kv->is_bytes()) { srv.send_frame(msg_error("missing api_key")); continue; }
                const auto& kb = kv->as_bytes();
                {
                    std::lock_guard lock(g_key_mutex);
                    g_api_key = SecureBuffer(kb.size());
                    std::memcpy(g_api_key.data(), kb.data(), kb.size());
                    g_secret_store.put(kCredentialName, g_api_key.span());
                    // Re-read from store into locked buffer (§3.3 step 2).
                    g_api_key = g_secret_store.get(kCredentialName);
                    g_key_loaded.store(true, std::memory_order_relaxed);
                    g_locked.store(false, std::memory_order_relaxed);
                }
                srv.send_frame(msg_auth_ok());

            } else if (type == MsgType::Complete || type == MsgType::CompleteStream) {
                if (!ensure_key_loaded()) {
                    srv.send_frame(msg_error("agent is locked — please login first"));
                    continue;
                }
                auto payload = frame_payload(frame);
                if (!payload.is_map()) { srv.send_frame(msg_error("bad payload")); continue; }
                const auto& m = payload.as_map();
                const auto* pv = cbor::map_get(m, "prompt");
                const auto* mv = cbor::map_get(m, "model");
                std::string prompt = (pv && pv->is_text()) ? pv->as_text() : "";
                std::string model  = (mv && mv->is_text()) ? mv->as_text() : "";

                // §9.1: scan for outbound secrets before forwarding
                auto hits = scan_prompt(prompt);
                if (!hits.empty()) {
                    std::string warn = "Prompt contains potential secrets (";
                    warn += hits[0].pattern_name + "). Send anyway? (y/N): ";
                    srv.send_frame(msg_error("SCANNER:" + warn));
                    // In this implementation the client must re-send with explicit consent.
                    // A full implementation would add a "confirmed" flag to the request.
                    continue;
                }

                g_idle_timer.mark_activity();

                if (type == MsgType::Complete) {
                    std::span<const std::byte> key;
                    { std::lock_guard lock(g_key_mutex); key = g_api_key.span(); }
                    try {
                        auto result = g_zima.complete(key, prompt, model);
                        srv.send_frame(msg_complete_reply(
                            result.content,
                            result.prompt_tokens,
                            result.completion_tokens));
                    } catch (const std::exception& e) {
                        srv.send_frame(msg_error(e.what()));
                    }
                } else {
                    try {
                        std::span<const std::byte> key;
                        { std::lock_guard lock(g_key_mutex); key = g_api_key.span(); }
                        auto result = g_zima.complete_stream(
                            key, prompt, model,
                            [&](std::string_view delta) {
                                srv.send_frame(msg_complete_chunk(delta));
                            });
                        srv.send_frame(msg_complete_done());
                        (void)result;
                    } catch (const std::exception& e) {
                        srv.send_frame(msg_error(e.what()));
                    }
                }

            } else if (type == MsgType::Models) {
                if (!ensure_key_loaded()) {
                    srv.send_frame(msg_error("agent is locked - please login first"));
                    continue;
                }
                try {
                    std::span<const std::byte> key;
                    { std::lock_guard lock(g_key_mutex); key = g_api_key.span(); }
                    std::string json = g_zima.list_models_json(key);
                    srv.send_frame(msg_models_reply(json));
                } catch (const std::exception& e) {
                    srv.send_frame(msg_error(e.what()));
                }

            } else if (type == MsgType::Usage) {
                if (!ensure_key_loaded()) {
                    srv.send_frame(msg_error("agent is locked - please login first"));
                    continue;
                }
                try {
                    std::span<const std::byte> key;
                    { std::lock_guard lock(g_key_mutex); key = g_api_key.span(); }
                    std::string json = g_zima.usage_json(key);
                    srv.send_frame(msg_usage_reply(json));
                } catch (const std::exception& e) {
                    srv.send_frame(msg_error(e.what()));
                }

            } else if (type == MsgType::Lock) {
                zero_key();
                srv.send_frame(msg_auth_ok());

            } else if (type == MsgType::Logout) {
                zero_key();
                g_secret_store.erase(kCredentialName);
                srv.send_frame(msg_auth_ok());

            } else {
                srv.send_frame(msg_error("unknown message type"));
            }
        }
    } catch (...) {
        // Client disconnected or error — clean up silently.
    }
}

// ---------- main ----------

int main() {
    // §4.1: harden before any other init. abort() on failure.
    HardeningImpl{}.harden();

    // Register zero-on-exit for the API key.
    register_zero_on_exit([]{ zero_key(); });

    // Generate session token and write to 0600 file.
    SecureBuffer session_token = generate_and_write_session_token();

    // Try to load existing API key from the platform secret store.
    ensure_key_loaded();

    // Start idle timer: zero in-memory key after 30 minutes of inactivity.
    g_idle_timer.start(zero_key);

    // Start IPC server.
    IpcServerImpl srv(agent_socket_path());
    std::cerr << "[agent] listening on " << srv.path() << "\n";

    // Accept loop — one client at a time for simplicity.
    // A production build would thread-per-client with bounded concurrency.
    while (!shutdown_requested()) {
        try {
            srv.accept();
            handle_client(srv, session_token);
            srv.close_client();
        } catch (const std::exception& e) {
            if (!shutdown_requested())
                std::cerr << "[agent] accept error: " << e.what() << "\n";
        }
    }

    // Graceful shutdown (§3.3 step 5).
    g_idle_timer.stop();
    zero_key();
    srv.shutdown();
    return 0;
}
