#include "AgentClient.h"

#include "rpc.h"
#include "session.h"
#include "../platform/windows/ipc_pipe.h"

#include <windows.h>
#include <sodium.h>
#include <stdexcept>

// Launch zima-agent.exe alongside this binary (fallback when it isn't running).
static bool spawnAgent() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"zima-agent.exe");

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path, nullptr, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static QString errorMessage(const std::vector<std::byte>& frame) {
    auto payload = frame_payload(frame);
    if (payload.is_map()) {
        const auto* mv = cbor::map_get(payload.as_map(), "message");
        if (mv && mv->is_text()) return QString::fromStdString(mv->as_text());
        const auto* rv = cbor::map_get(payload.as_map(), "reason");
        if (rv && rv->is_text()) return QString::fromStdString(rv->as_text());
    }
    return QStringLiteral("unknown error");
}

AgentClient::AgentClient(QObject* parent) : QObject(parent) {
    // 0 = ok, 1 = already initialised, -1 = failed. Either non-negative is fine.
    if (sodium_init() < 0) { /* crypto unavailable; ensure() will surface errors */ }
}

AgentClient::~AgentClient() {
    delete client_;
}

bool AgentClient::ensure() {
    if (client_) return true;
    const std::string pipe = agent_socket_path();
    for (int attempt = 0; attempt < 2 && !client_; ++attempt) {
        try {
            client_ = new WindowsIpcClient(pipe);
        } catch (...) {
            if (attempt == 0) {
                spawnAgent();
                for (int i = 0; i < 30 && !client_; ++i) {
                    Sleep(100);
                    try { client_ = new WindowsIpcClient(pipe); } catch (...) {}
                }
            }
        }
    }
    if (!client_) {
        emit disconnected(QStringLiteral(
            "Cannot reach zima-agent. Is it running alongside the app?"));
        return false;
    }
    try {
        SecureBuffer tok = load_session_token();
        client_->send_frame(msg_authenticate(tok.span()));
        auto reply = client_->recv_frame();
        if (frame_type(reply) != MsgType::AuthOk) {
            delete client_; client_ = nullptr;
            emit disconnected(QStringLiteral("Agent authentication failed."));
            return false;
        }
        client_->enable_encryption(derive_ipc_key(tok.span()));
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
        return false;
    }
    emit connected();
    return true;
}

void AgentClient::sendPrompt(const QString& prompt, const QString& model) {
    if (!ensure()) return;
    try {
        client_->send_frame(msg_complete_stream(prompt.toStdString(), model.toStdString()));
        for (;;) {
            auto frame = client_->recv_frame();
            MsgType t = frame_type(frame);
            if (t == MsgType::CompleteChunk) {
                auto payload = frame_payload(frame);
                if (!payload.is_map()) continue;
                const auto* dv = cbor::map_get(payload.as_map(), "delta");
                if (dv && dv->is_text() && !dv->as_text().empty())
                    emit chunk(QString::fromStdString(dv->as_text()));
            } else if (t == MsgType::CompleteDone) {
                emit turnFinished();
                break;
            } else if (t == MsgType::Error) {
                emit errorOccurred(errorMessage(frame));
                break;
            }
        }
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
    }
}

void AgentClient::requestComplete(const QString& prompt, const QString& model) {
    if (!ensure()) return;
    try {
        client_->send_frame(msg_complete(prompt.toStdString(), model.toStdString()));
        auto reply = client_->recv_frame();
        MsgType t = frame_type(reply);
        if (t == MsgType::CompleteReply) {
            auto payload = frame_payload(reply);
            QString content;
            if (payload.is_map()) {
                const auto* cv = cbor::map_get(payload.as_map(), "content");
                if (cv && cv->is_text()) content = QString::fromStdString(cv->as_text());
            }
            emit completeReply(content);
        } else if (t == MsgType::Error) {
            emit errorOccurred(errorMessage(reply));
        } else {
            emit errorOccurred(QStringLiteral("Unexpected reply from agent."));
        }
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
    }
}

void AgentClient::requestTitle(const QString& prompt, const QString& model) {
    // Best-effort: a failed title never disrupts the chat (a provisional title
    // is already in place), so errors here are swallowed.
    if (!ensure()) return;
    try {
        client_->send_frame(msg_complete(prompt.toStdString(), model.toStdString()));
        auto reply = client_->recv_frame();
        if (frame_type(reply) == MsgType::CompleteReply) {
            auto payload = frame_payload(reply);
            if (payload.is_map()) {
                const auto* cv = cbor::map_get(payload.as_map(), "content");
                if (cv && cv->is_text())
                    emit titleReady(QString::fromStdString(cv->as_text()));
            }
        }
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
    }
}

void AgentClient::configureCredential(const QString& apiKey) {
    if (!ensure()) return;
    try {
        const std::string key = apiKey.toStdString();
        SecureBuffer buf(key.size());
        if (!key.empty()) memcpy(buf.data(), key.data(), key.size());
        client_->send_frame(msg_configure_credential(buf.span()));
        auto reply = client_->recv_frame();
        if (frame_type(reply) == MsgType::AuthOk) emit loginSucceeded();
        else emit errorOccurred(QStringLiteral("Login failed: ") + errorMessage(reply));
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
    }
}

void AgentClient::requestStatus() {
    if (!ensure()) return;
    try {
        client_->send_frame(msg_status());
        auto reply = client_->recv_frame();
        auto payload = frame_payload(reply);
        bool locked = true; QString model = QStringLiteral("?");
        if (payload.is_map()) {
            const auto& m = payload.as_map();
            const auto* lv = cbor::map_get(m, "locked");
            const auto* mv = cbor::map_get(m, "model");
            if (lv && lv->is_bool()) locked = lv->as_bool();
            if (mv && mv->is_text()) model = QString::fromStdString(mv->as_text());
        }
        emit statusReceived(locked, model);
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
    }
}

void AgentClient::requestModels() {
    if (!ensure()) return;
    try {
        client_->send_frame(msg_models());
        auto reply = client_->recv_frame();
        if (frame_type(reply) == MsgType::ModelsReply) {
            auto payload = frame_payload(reply);
            if (payload.is_map()) {
                const auto* jv = cbor::map_get(payload.as_map(), "json");
                if (jv && jv->is_text())
                    emit modelsReceived(QString::fromStdString(jv->as_text()));
            }
        } else if (frame_type(reply) == MsgType::Error) {
            emit errorOccurred(errorMessage(reply));
        }
    } catch (const std::exception& e) {
        delete client_; client_ = nullptr;
        emit disconnected(QString::fromUtf8(e.what()));
    }
}
