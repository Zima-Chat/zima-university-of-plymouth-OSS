#pragma once
// Zima HTTPS client (§4.4).
// Wraps libcurl: one CURL* per request, TLS 1.3, pinned SPKI, streaming SSE.
// The Authorization header is written into sodium_malloc'd memory and zeroed
// immediately after curl_easy_perform returns, regardless of outcome.

#include "secure_buffer.h"
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct CompletionResult {
    std::string content;
    uint64_t    prompt_tokens     = 0;
    uint64_t    completion_tokens = 0;
};

class ZimaClient {
public:
    ZimaClient();
    ~ZimaClient();

    // Single-turn completion (non-streaming).
    // api_key must remain valid for the duration of the call.
    CompletionResult complete(
        std::span<const std::byte> api_key,
        const std::string& prompt,
        const std::string& model);

    // Streaming completion: delta_cb is called for each SSE delta token.
    // Returns total usage. api_key must remain valid for the duration.
    CompletionResult complete_stream(
        std::span<const std::byte> api_key,
        const std::string& prompt,
        const std::string& model,
        std::function<void(std::string_view delta)> delta_cb);

    // GET /api/v1/models
    std::string list_models_json(std::span<const std::byte> api_key);

    // GET /api/v1/usage
    std::string usage_json(std::span<const std::byte> api_key);

    // Parse one SSE "data:" line and call delta_cb if it contains a delta.
    // Public so the static curl write callback in the .cpp can reach it.
    static bool parse_sse_line(std::string_view line,
                               std::function<void(std::string_view)>& delta_cb,
                               CompletionResult& result);

private:
    // Build the JSON request body (OpenAI-compatible §4.4) with the given
    // system prompt prepended.
    static std::string build_request_body(const std::string& system_prompt,
                                          const std::string& prompt,
                                          const std::string& model,
                                          bool stream);

    // One non-streaming completion round-trip with an explicit system prompt;
    // returns the assistant content. Thread-safe (no shared mutable state).
    static std::string perform_completion(std::span<const std::byte> api_key,
                                          const std::string& system_prompt,
                                          const std::string& user_prompt,
                                          const std::string& model);

    // A role/content message in a multi-turn conversation.
    using ChatMessage = std::pair<std::string, std::string>; // {role, content}

    // One non-streaming round-trip over a full conversation; returns the
    // assistant content. Powers the core agent's delegation loop.
    static std::string perform_chat(std::span<const std::byte> api_key,
                                    const std::vector<ChatMessage>& messages,
                                    const std::string& model);

    // Run subtasks as concurrent helper agents (bounded thread pool) and return
    // their results in task order. Powers the spawn_agents tool.
    static std::vector<std::string> run_agents_parallel(
        std::span<const std::byte> api_key,
        const std::string& model,
        const std::vector<std::string>& tasks);

    // Parse a complete non-streaming response body.
    // Takes only the bytes actually received (not the full pre-allocated buffer).
    static CompletionResult parse_response(std::string_view received);

    static std::string perform_json_request(std::span<const std::byte> api_key,
                                            const char* url,
                                            const char* method,
                                            const char* body,
                                            long timeout_secs);
    static std::string resolve_model(std::span<const std::byte> api_key,
                                     const std::string& requested_model);
    static std::string extract_first_model_id(std::string_view models_json);
};
