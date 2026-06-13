#pragma once
// IPC frame encoding/decoding and RPC message types (§5.3, §5.4).
// Wire format: [4-byte BE length (type+payload)] [4-byte type] [CBOR payload]

#include "secure_buffer.h"
#include "cbor.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// ---------- Message type identifiers (§5.4) ----------
enum class MsgType : uint32_t {
    Authenticate       = 1,
    AuthOk             = 2,
    AuthFail           = 3,
    ConfigureCredential= 4,
    Status             = 5,
    StatusReply        = 6,
    Complete           = 7,
    CompleteReply      = 8,
    CompleteStream     = 9,
    CompleteChunk      = 10,
    CompleteDone       = 11,
    Lock               = 12,
    Logout             = 13,
    Error              = 14,
    Models             = 15,
    ModelsReply        = 16,
    Usage              = 17,
    UsageReply         = 18,
};

// ---------- Frame building ----------

// Encode a complete wire frame from a type and a pre-encoded CBOR payload.
// Returns: [4-byte BE(len)] [4-byte type] [payload]
std::vector<std::byte> make_frame(MsgType type, const std::vector<uint8_t>& payload);
std::vector<std::byte> make_frame(MsgType type); // empty payload

// Decode the type from the first 4 bytes of the body portion of a frame
// (i.e. what recv_frame() returns, which excludes the length prefix).
MsgType frame_type(const std::vector<std::byte>& frame);

// Decode the CBOR payload from a frame body.
cbor::Value frame_payload(const std::vector<std::byte>& frame);

// ---------- Typed message helpers ----------

// Client → agent: first frame after connect.
std::vector<std::byte> msg_authenticate(std::span<const std::byte> token);

// Agent → client: authentication result.
std::vector<std::byte> msg_auth_ok();
std::vector<std::byte> msg_auth_fail(std::string_view reason);

// Client → agent: store a new API key.
std::vector<std::byte> msg_configure_credential(std::span<const std::byte> api_key);

// Client → agent: query agent state.
std::vector<std::byte> msg_status();
// Agent → client: agent state reply.
std::vector<std::byte> msg_status_reply(bool locked, std::string_view model);

// Client → agent: single-turn completion.
std::vector<std::byte> msg_complete(std::string_view prompt, std::string_view model);
// Agent → client: completion result.
std::vector<std::byte> msg_complete_reply(std::string_view content,
                                          uint64_t prompt_tokens,
                                          uint64_t completion_tokens);

// Client → agent: streaming completion.
std::vector<std::byte> msg_complete_stream(std::string_view prompt, std::string_view model);
// Agent → client: one delta chunk.
std::vector<std::byte> msg_complete_chunk(std::string_view delta);
// Agent → client: end of stream.
std::vector<std::byte> msg_complete_done();

// Client → agent: lock (zero in-memory key).
std::vector<std::byte> msg_lock();
// Client → agent: logout (zero key + delete from secret store).
std::vector<std::byte> msg_logout();

// Client → agent: fetch available models.
std::vector<std::byte> msg_models();
// Agent → client: raw models JSON payload.
std::vector<std::byte> msg_models_reply(std::string_view json);

// Client → agent: fetch usage/billing information.
std::vector<std::byte> msg_usage();
// Agent → client: raw usage JSON payload.
std::vector<std::byte> msg_usage_reply(std::string_view json);

// Agent → client: generic error.
std::vector<std::byte> msg_error(std::string_view message);
