#include "zima_client.h"
#include "json_util.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include <curl/curl.h>
#include <sodium.h>

// System prompt that advertises the CLI's local tools to the model. When the
// user asks to create a file, the model is told to answer with a single JSON
// tool-call object, which the CLI parses and executes (§ assistant tools).
static const char* kToolSystemPrompt =
    "You are a coding assistant running inside a command-line tool that has a "
    "create_file tool which writes a file to the user's computer.\n"
    "Whenever the user asks you to create, write, generate, build, make, or save a "
    "file, program, script, or any piece of code in any language, you MUST use the "
    "tool. Do NOT paste code into a normal reply in that case. Using the tool means "
    "replying with ONLY a single JSON object and nothing else - no prose, no "
    "explanation, no markdown, no code fences - in exactly this shape:\n"
    "{\"tool\":\"create_file\",\"path\":\"<relative path>\",\"content\":\"<full file contents>\"}\n"
    "Pick a sensible relative filename yourself (the user does not need to name it or "
    "say the word 'tool'). Put the COMPLETE file contents in \"content\" as a JSON "
    "string (escape newlines as \\n and double quotes as \\\"), and keep \"content\" as "
    "the last key.\n"
    "Only when the request is clearly NOT about producing a file or code - for example "
    "a question, a definition, or an explanation - should you answer normally in plain "
    "text.";

// Zima API endpoints.
static constexpr const char* kChatCompletionsUrl = "https://zimalabs.io/api/v1/chat/completions";
static constexpr const char* kModelsUrl          = "https://zimalabs.io/api/v1/models";
static constexpr const char* kUsageUrl           = "https://zimalabs.io/api/v1/usage";

// SHA-256 SPKI pin for the Zima leaf/intermediate certificate (§4.4, §7.3).
// Format expected by CURLOPT_PINNEDPUBLICKEY: "sha256//base64=="
static constexpr const char* kSpkiPin = "sha256//NPL8oxNeaLVDIjqJ7pn74k2BDokQ9zOONgNygOAjhQg=";

// ---------- curl write callback helpers ----------

struct WriteCtx {
    SecureBuffer* buf;
    size_t        used = 0;
};

static size_t write_cb(char* ptr, size_t /*size*/, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    size_t needed = ctx->used + nmemb;
    if (needed > ctx->buf->size()) {
        // Grow in 64 KiB increments (§4.4): allocate-copy-zero-free, no realloc.
        size_t new_size = ((needed + 65535) / 65536) * 65536;
        ctx->buf->resize(new_size);
    }
    std::memcpy(static_cast<char*>(ctx->buf->data()) + ctx->used, ptr, nmemb);
    ctx->used += nmemb;
    return nmemb;
}

struct StreamCtx {
    std::function<void(std::string_view)>* delta_cb;
    CompletionResult*                      result;
    std::string                            line_buf; // pending partial SSE line
};

// Called per-chunk by curl during streaming; reassembles SSE lines.
static size_t stream_write_cb(char* ptr, size_t /*size*/, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    std::string_view chunk(ptr, nmemb);
    while (!chunk.empty()) {
        auto nl = chunk.find('\n');
        if (nl == std::string_view::npos) {
            ctx->line_buf.append(chunk);
            break;
        }
        ctx->line_buf.append(chunk.substr(0, nl));
        // Strip trailing \r if present (SSE uses \r\n or \n).
        std::string_view line(ctx->line_buf);
        if (!line.empty() && line.back() == '\r')
            line = line.substr(0, line.size()-1);
        ZimaClient::parse_sse_line(line, *ctx->delta_cb, *ctx->result);
        ctx->line_buf.clear();
        chunk = chunk.substr(nl + 1);
    }
    return nmemb;
}

// ---------- Authorization header allocation ----------

// Write "Bearer <key>" into sodium_malloc memory, add to curl header list,
// zero immediately after the request, and free.
static struct curl_slist* build_auth_header(std::span<const std::byte> api_key) {
    const size_t prefix_len = 7; // "Bearer "
    size_t total = prefix_len + api_key.size() + 1; // +1 for null terminator
    char* auth = static_cast<char*>(sodium_malloc(total));
    if (!auth) throw std::bad_alloc();
    std::memcpy(auth, "Bearer ", prefix_len);
    std::memcpy(auth + prefix_len, api_key.data(), api_key.size());
    auth[total-1] = '\0';
    struct curl_slist* headers = nullptr;
    // curl_slist_append copies the string internally.
    headers = curl_slist_append(nullptr, "Content-Type: application/json");
    char header_buf[4096];
    if (total - 1 + 16 < sizeof(header_buf)) {
        std::memcpy(header_buf, "Authorization: ", 15);
        std::memcpy(header_buf + 15, auth, total);
        headers = curl_slist_append(headers, header_buf);
        sodium_memzero(header_buf, sizeof(header_buf));
    }
    sodium_memzero(auth, total);
    sodium_free(auth);
    return headers;
}

// ---------- ZimaClient ----------

ZimaClient::ZimaClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ZimaClient::~ZimaClient() {
    curl_global_cleanup();
}

std::string ZimaClient::extract_first_model_id(std::string_view s) {
    size_t pos = 0;
    while (true) {
        pos = s.find("\"id\"", pos);
        if (pos == std::string_view::npos) return {};
        pos += 4;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) ++pos;
        if (pos >= s.size() || s[pos] != ':') continue;
        ++pos;
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) ++pos;
        if (pos >= s.size() || s[pos] != '"') continue;
        ++pos;

        std::string id;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                ++pos;
                id += s[pos];
            } else {
                id += s[pos];
            }
            ++pos;
        }
        if (!id.empty()) return id;
    }
}

std::string ZimaClient::resolve_model(std::span<const std::byte> api_key,
                                      const std::string& requested_model) {
    if (!requested_model.empty()) return requested_model;
    std::string models = perform_json_request(api_key, kModelsUrl, "GET", nullptr, 10L);
    std::string model = extract_first_model_id(models);
    if (model.empty())
        throw std::runtime_error("Zima: no accessible model found for this API key");
    return model;
}

std::string ZimaClient::perform_json_request(std::span<const std::byte> api_key,
                                             const char* url,
                                             const char* method,
                                             const char* body,
                                             long timeout_secs) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    SecureBuffer resp_buf(65536);
    WriteCtx wctx{&resp_buf, 0};
    struct curl_slist* headers = build_auth_header(api_key);

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &wctx);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION,      CURL_SSLVERSION_TLSv1_3);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  2L);
    // Trust roots come from the Windows system certificate store (the static
    // OpenSSL build ships no CA bundle). Pinning below is an extra check on top.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,     CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, kSpkiPin);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         timeout_secs);

    if (std::strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (std::strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body ? std::strlen(body) : 0));
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    CURLcode rc = curl_easy_perform(curl);

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));

    std::string_view received(static_cast<const char*>(resp_buf.data()), wctx.used);
    if (http_status < 200 || http_status >= 300) {
        throw std::runtime_error("Zima HTTP " + std::to_string(http_status)
                                 + ": " + std::string(received.substr(0, 512)));
    }

    return std::string(received);
}

std::string ZimaClient::build_request_body(const std::string& prompt,
                                            const std::string& model,
                                            bool stream) {
    // max_tokens must be generous: without it the API's default can truncate a
    // longer file mid-generation, leaving an unparseable tool-call JSON.
    std::string body = "{\"model\":\"" + model + "\","
        "\"messages\":["
            "{\"role\":\"system\",\"content\":\"" + jsonutil::escape(kToolSystemPrompt) + "\"},"
            "{\"role\":\"user\",\"content\":\""   + jsonutil::escape(prompt)            + "\"}],"
        "\"max_tokens\":4096,"
        "\"stream\":" + (stream ? "true" : "false") + "}";
    return body;
}

// Parse only the bytes actually received (passed as string_view, not the full buffer).
CompletionResult ZimaClient::parse_response(std::string_view s) {
    // Surface API-level errors before attempting content extraction.
    // Zima/OpenAI error body: {"error":{"message":"...","type":"..."},...}
    auto err_pos = s.find("\"error\":");
    if (err_pos != std::string_view::npos) {
        std::string errmsg;
        if (!jsonutil::get_string(s.substr(err_pos), "message", errmsg))
            errmsg = "API error";
        throw std::runtime_error("Zima: " + errmsg);
    }

    // Assistant content: choices[0].message.content. get_string decodes JSON
    // escapes properly (so newlines survive, and an embedded tool-call object
    // comes back as valid JSON for the CLI to parse).
    CompletionResult r;
    if (!jsonutil::get_string(s, "content", r.content) || r.content.empty())
        throw std::runtime_error("Zima: response contained no content field. Body: "
                                 + std::string(s.substr(0, 512)));
    return r;
}

bool ZimaClient::parse_sse_line(std::string_view line,
                                 std::function<void(std::string_view)>& delta_cb,
                                 CompletionResult& /*result*/) {
    if (line.empty() || line == "data: [DONE]") return false;
    if (line.substr(0, 6) != "data: ") return false;
    std::string_view json = line.substr(6);
    // Extract and properly decode the delta content from:
    //   ...{"delta":{"content":"..."}}...
    // Proper JSON decoding matters so escapes like < ("<") and \n survive.
    std::string delta;
    if (!jsonutil::get_string(json, "content", delta)) return false;
    if (!delta.empty()) delta_cb(delta);
    return true;
}

CompletionResult ZimaClient::complete(std::span<const std::byte> api_key,
                                       const std::string& prompt,
                                       const std::string& model) {
    std::string resolved_model = resolve_model(api_key, model);
    std::string body = build_request_body(prompt, resolved_model, false);
    std::string received = perform_json_request(api_key,
                                                kChatCompletionsUrl,
                                                "POST",
                                                body.c_str(),
                                                60L);
    return parse_response(received);
}

CompletionResult ZimaClient::complete_stream(
    std::span<const std::byte> api_key,
    const std::string& prompt,
    const std::string& model,
    std::function<void(std::string_view)> delta_cb)
{
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string resolved_model = resolve_model(api_key, model);
    std::string body = build_request_body(prompt, resolved_model, true);
    CompletionResult result;
    StreamCtx sctx{&delta_cb, &result, {}};

    struct curl_slist* headers = build_auth_header(api_key);

    curl_easy_setopt(curl, CURLOPT_URL,            kChatCompletionsUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,      headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,      body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,   static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   stream_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &sctx);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION,      CURL_SSLVERSION_TLSv1_3);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  2L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,     CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, kSpkiPin);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,         300L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("curl stream: ") + curl_easy_strerror(rc));

    return result;
}

std::string ZimaClient::list_models_json(std::span<const std::byte> api_key) {
    return perform_json_request(api_key, kModelsUrl, "GET", nullptr, 10L);
}

std::string ZimaClient::usage_json(std::span<const std::byte> api_key) {
    return perform_json_request(api_key, kUsageUrl, "GET", nullptr, 10L);
}
