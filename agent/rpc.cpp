#include "rpc.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// ---------- Frame building ----------

// Wire: [4-byte BE length = sizeof(type)+sizeof(payload)] [4-byte type] [payload]
std::vector<std::byte> make_frame(MsgType type, const std::vector<uint8_t>& payload) {
    uint32_t body_len = 4u + static_cast<uint32_t>(payload.size());
    uint32_t net_len  = htonl(body_len);
    uint32_t net_type = htonl(static_cast<uint32_t>(type));

    std::vector<std::byte> frame;
    frame.reserve(4 + 4 + payload.size());
    const std::byte* p;

    p = reinterpret_cast<const std::byte*>(&net_len);
    frame.insert(frame.end(), p, p+4);
    p = reinterpret_cast<const std::byte*>(&net_type);
    frame.insert(frame.end(), p, p+4);
    for (uint8_t b : payload) frame.push_back(static_cast<std::byte>(b));
    return frame;
}

std::vector<std::byte> make_frame(MsgType type) {
    return make_frame(type, {});
}

MsgType frame_type(const std::vector<std::byte>& frame) {
    if (frame.size() < 4) throw std::runtime_error("rpc: frame too short");
    uint32_t net_type;
    std::memcpy(&net_type, frame.data(), 4);
    return static_cast<MsgType>(ntohl(net_type));
}

cbor::Value frame_payload(const std::vector<std::byte>& frame) {
    if (frame.size() <= 4) return cbor::Value{std::monostate{}};
    return cbor::decode(std::span<const std::byte>(frame.data() + 4, frame.size() - 4));
}

// ---------- typed helpers ----------

std::vector<std::byte> msg_authenticate(std::span<const std::byte> token) {
    auto payload = cbor::MapBuilder{}
        .add_bytes("token",
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(token.data()), token.size()))
        .build();
    return make_frame(MsgType::Authenticate, payload);
}

std::vector<std::byte> msg_auth_ok() { return make_frame(MsgType::AuthOk); }

std::vector<std::byte> msg_auth_fail(std::string_view reason) {
    auto p = cbor::MapBuilder{}.add_text("reason", reason).build();
    return make_frame(MsgType::AuthFail, p);
}

std::vector<std::byte> msg_configure_credential(std::span<const std::byte> api_key) {
    auto payload = cbor::MapBuilder{}
        .add_bytes("api_key",
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(api_key.data()), api_key.size()))
        .build();
    return make_frame(MsgType::ConfigureCredential, payload);
}

std::vector<std::byte> msg_status() { return make_frame(MsgType::Status); }

std::vector<std::byte> msg_status_reply(bool locked, std::string_view model) {
    auto p = cbor::MapBuilder{}
        .add_bool("locked", locked)
        .add_text("model",  model)
        .build();
    return make_frame(MsgType::StatusReply, p);
}

std::vector<std::byte> msg_complete(std::string_view prompt, std::string_view model) {
    auto p = cbor::MapBuilder{}
        .add_text("prompt", prompt)
        .add_text("model",  model)
        .build();
    return make_frame(MsgType::Complete, p);
}

std::vector<std::byte> msg_complete_reply(std::string_view content,
                                          uint64_t prompt_tokens,
                                          uint64_t completion_tokens) {
    auto p = cbor::MapBuilder{}
        .add_text ("content",           content)
        .add_uint ("prompt_tokens",     prompt_tokens)
        .add_uint ("completion_tokens", completion_tokens)
        .build();
    return make_frame(MsgType::CompleteReply, p);
}

std::vector<std::byte> msg_complete_stream(std::string_view prompt, std::string_view model) {
    auto p = cbor::MapBuilder{}
        .add_text("prompt", prompt)
        .add_text("model",  model)
        .build();
    return make_frame(MsgType::CompleteStream, p);
}

std::vector<std::byte> msg_complete_chunk(std::string_view delta) {
    auto p = cbor::MapBuilder{}.add_text("delta", delta).build();
    return make_frame(MsgType::CompleteChunk, p);
}

std::vector<std::byte> msg_complete_done() { return make_frame(MsgType::CompleteDone); }

std::vector<std::byte> msg_lock()   { return make_frame(MsgType::Lock); }
std::vector<std::byte> msg_logout() { return make_frame(MsgType::Logout); }

std::vector<std::byte> msg_models() { return make_frame(MsgType::Models); }

std::vector<std::byte> msg_models_reply(std::string_view json) {
    auto p = cbor::MapBuilder{}.add_text("json", json).build();
    return make_frame(MsgType::ModelsReply, p);
}

std::vector<std::byte> msg_usage() { return make_frame(MsgType::Usage); }

std::vector<std::byte> msg_usage_reply(std::string_view json) {
    auto p = cbor::MapBuilder{}.add_text("json", json).build();
    return make_frame(MsgType::UsageReply, p);
}

std::vector<std::byte> msg_error(std::string_view message) {
    auto p = cbor::MapBuilder{}.add_text("message", message).build();
    return make_frame(MsgType::Error, p);
}
