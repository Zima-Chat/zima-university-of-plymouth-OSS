#include "zima_client.h"
#include "json_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <sodium.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/evp.h>

// ---- Tool system prompts -------------------------------------------------
//
// The create_file rule is shared. The top-level (ask) prompt also offers
// spawn_agents for parallel multi-agent work; the agent orchestrates that
// server-side. Helper agents and the synthesis step deliberately lack
// spawn_agents so a delegation cannot recurse forever.

static const char* kCreateFileRule =
    "create_file writes a file to the user's computer. Whenever the user asks you to "
    "create, write, generate, build, make, or save a file, program, script, or any "
    "piece of code, you MUST use it: reply with ONLY this JSON and nothing else (no "
    "prose, no markdown, no code fences):\n"
    "{\"tool\":\"create_file\",\"path\":\"<relative path>\",\"content\":\"<full file contents>\"}\n"
    "Pick a sensible relative filename yourself. Put the COMPLETE contents in "
    "\"content\" as a JSON string (escape newlines as \\n and double quotes as \\\"), "
    "and keep \"content\" last.\n";

// Core-agent prompt for `ask`: an iterative loop where the agent can delegate
// to parallel helpers across multiple rounds (seeing results before deciding to
// delegate again or finish), then answer in text or via create_file.
static const std::string kAgentLoopPrompt = std::string(
    "You are a coding assistant in a command-line tool. You can do work yourself or "
    "delegate independent subtasks to parallel helper agents, and you may delegate "
    "MORE THAN ONCE as you learn from earlier results.\n"
    "TOOLS:\n"
    "- spawn_agents: run 2-5 INDEPENDENT subtasks in parallel. Reply with ONLY:\n"
    "  {\"tool\":\"spawn_agents\",\"tasks\":[\"<self-contained subtask>\",\"<...>\"]}\n"
    "  You will then receive the helpers' results and can delegate again or finish.\n"
    "- create_file: ") + kCreateFileRule +
    "  Using create_file finishes the task.\n"
    "Each spawn_agents task MUST be fully self-contained - helpers do not see this "
    "conversation. Delegate only when parallel work genuinely helps; for a simple "
    "request just answer directly. In any tool JSON, escape newlines as \\n and double "
    "quotes as \\\".\n"
    "When you have what you need, give the user your final answer as plain text (or a "
    "create_file). Do not mention the helper agents or that you delegated.";

// Streaming prompt for `chat`: create_file only (delegation can't stream).
static const std::string kStreamSystemPrompt =
    std::string("You are a coding assistant in a command-line tool.\nTOOL ") +
    kCreateFileRule +
    "For a plain question or explanation, answer in text.";

// Helper-agent prompt: focused worker, no tools.
static const char* kWorkerSystemPrompt =
    "You are a focused helper agent assigned ONE subtask that is part of a larger job. "
    "Complete the subtask as concretely and completely as possible and return only your "
    "result. Do not ask questions. If you write code, include it inline. Do not use any "
    "tools or JSON wrappers - reply in plain text.";

// Synthesis prompt: combine helper results; may itself emit a create_file.
static const std::string kSynthesisSystemPrompt = std::string(
    "You are combining the results of several helper agents into one final answer for "
    "the user's original request. Produce the best unified result and do not mention "
    "the helpers or the delegation. If the original request was to produce a file, use "
    "create_file: ") + kCreateFileRule +
    "Otherwise answer in plain text.";

// Zima API endpoints.
static constexpr const char* kChatCompletionsUrl = "https://zimalabs.io/api/v1/chat/completions";
static constexpr const char* kModelsUrl          = "https://zimalabs.io/api/v1/models";
static constexpr const char* kUsageUrl           = "https://zimalabs.io/api/v1/usage";

// SPKI pins (§4.4, §7.3). We pin the issuing CA's public keys, not the leaf:
// the connection is accepted only if one of these SubjectPublicKeyInfo SHA-256
// hashes appears in the validated certificate chain. Pinning the Let's Encrypt
// intermediate + ISRG root means routine ~90-day leaf renewals do NOT break the
// pin (the old leaf-key pin did), while still constraining trust to the
// ISRG/Let's Encrypt hierarchy rather than every CA in the system store.
//
// Values are base64(SHA256(DER SubjectPublicKeyInfo)). Refresh only if Zima
// moves off Let's Encrypt or LE rotates these CAs (years). Regenerate per cert:
//   openssl x509 -in ca.pem -pubkey -noout | openssl pkey -pubin -outform der \
//     | openssl dgst -sha256 -binary | openssl base64
static const char* const kPinnedSpki[] = {
    "nWN7PSep5XDQdge5zK24CnCRXHr3KvzhKEGxsdqCX9E=", // Let's Encrypt intermediate (CN=YR2)
    "fk6IOKit1ild5647BH06ujSIq5XbCgqlbYl6ANhhi88=", // ISRG root (CN=Root YR)
};

// base64(SHA256(SPKI-DER)) of a cert, tested against the pinned set.
static bool cert_spki_pinned(X509* cert) {
    unsigned char* der = nullptr;
    int der_len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(cert), &der);
    if (der_len <= 0 || !der) return false;

    unsigned char hash[32];
    unsigned int  hash_len = 0;
    EVP_Digest(der, static_cast<size_t>(der_len), hash, &hash_len, EVP_sha256(), nullptr);
    OPENSSL_free(der);
    if (hash_len != 32) return false;

    char b64[64];
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64), hash, 32);
    if (n <= 0) return false;
    b64[n] = '\0';

    for (const char* pin : kPinnedSpki)
        if (std::strcmp(b64, pin) == 0) return true;
    return false;
}

// OpenSSL verify callback: keep the standard chain/expiry validation, then
// additionally require a pinned SPKI somewhere in the chain. Evaluated once,
// at the leaf (depth 0), where the chain is fully built.
static int pin_verify_cb(int preverify_ok, X509_STORE_CTX* ctx) {
    if (!preverify_ok) return 0; // standard validation (incl. CA trust) must pass
    if (X509_STORE_CTX_get_error_depth(ctx) != 0) return 1;

    STACK_OF(X509)* chain = X509_STORE_CTX_get0_chain(ctx);
    if (chain) {
        for (int i = 0; i < sk_X509_num(chain); ++i)
            if (cert_spki_pinned(sk_X509_value(chain, i))) return 1;
    }
    X509_STORE_CTX_set_error(ctx, X509_V_ERR_CERT_REJECTED);
    return 0; // chain validated but no pinned CA key present → reject
}

// Installed via CURLOPT_SSL_CTX_FUNCTION so our verify callback runs on top of
// curl's verification (VERIFYPEER + native CA store remain in force).
static CURLcode ssl_ctx_cb(CURL*, void* ssl_ctx, void*) {
    SSL_CTX_set_verify(static_cast<SSL_CTX*>(ssl_ctx), SSL_VERIFY_PEER, pin_verify_cb);
    return CURLE_OK;
}

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
    // OpenSSL build ships no CA bundle). The SSL_CTX callback then enforces our
    // CA SPKI pin on top of standard validation.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,      CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          timeout_secs);

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

std::string ZimaClient::build_request_body(const std::string& system_prompt,
                                            const std::string& prompt,
                                            const std::string& model,
                                            bool stream) {
    // max_tokens must be generous: without it the API's default can truncate a
    // longer file mid-generation, leaving an unparseable tool-call JSON.
    std::string body = "{\"model\":\"" + model + "\","
        "\"messages\":["
            "{\"role\":\"system\",\"content\":\"" + jsonutil::escape(system_prompt) + "\"},"
            "{\"role\":\"user\",\"content\":\""   + jsonutil::escape(prompt)        + "\"}],"
        "\"max_tokens\":4096,"
        "\"stream\":" + (stream ? "true" : "false") + "}";
    return body;
}

std::string ZimaClient::perform_completion(std::span<const std::byte> api_key,
                                           const std::string& system_prompt,
                                           const std::string& user_prompt,
                                           const std::string& model) {
    std::string body = build_request_body(system_prompt, user_prompt, model, false);
    std::string resp = perform_json_request(api_key, kChatCompletionsUrl, "POST",
                                            body.c_str(), 60L);
    return parse_response(resp).content;
}

std::string ZimaClient::perform_chat(std::span<const std::byte> api_key,
                                     const std::vector<ChatMessage>& messages,
                                     const std::string& model) {
    std::string body = "{\"model\":\"" + model + "\",\"messages\":[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i) body += ",";
        body += "{\"role\":\"" + messages[i].first + "\",\"content\":\""
              + jsonutil::escape(messages[i].second) + "\"}";
    }
    body += "],\"max_tokens\":4096,\"stream\":false}";
    std::string resp = perform_json_request(api_key, kChatCompletionsUrl, "POST",
                                            body.c_str(), 60L);
    return parse_response(resp).content;
}

// Serialize log lines so concurrent worker output stays readable.
static std::mutex g_log_mutex;
static void agent_log(const std::string& line) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::fprintf(stderr, "[%lld] %s\n", static_cast<long long>(ms), line.c_str());
    std::fflush(stderr);
}

std::vector<std::string> ZimaClient::run_agents_parallel(
    std::span<const std::byte> api_key,
    const std::string& model,
    const std::vector<std::string>& tasks) {

    std::vector<std::string> results(tasks.size());
    std::atomic<size_t> next{0};

    // Bounded pool: cap concurrent HTTPS calls so we don't open an unreasonable
    // number of sockets for a large task list. Each worker thread uses its own
    // CURL handle (perform_completion is self-contained), so this is data-race
    // free; results[i] is written by exactly one thread.
    const size_t pool = std::min<size_t>(tasks.size(), 6);

    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= tasks.size()) return;

            std::ostringstream tid; tid << std::this_thread::get_id();
            auto t0 = std::chrono::steady_clock::now();
            std::string preview = tasks[i].substr(0, 64);
            agent_log("  sub-agent " + std::to_string(i + 1) + " START  thread=" + tid.str() +
                      "  task=\"" + preview + (tasks[i].size() > 64 ? "..." : "") + "\"");

            try {
                results[i] = perform_completion(api_key, kWorkerSystemPrompt, tasks[i], model);
            } catch (const std::exception& e) {
                results[i] = std::string("[subtask failed: ") + e.what() + "]";
            }

            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            agent_log("  sub-agent " + std::to_string(i + 1) + " DONE   thread=" + tid.str() +
                      "  " + std::to_string(dt) + "ms  " +
                      std::to_string(results[i].size()) + " chars returned");
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(pool);
    for (size_t t = 0; t < pool; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();
    return results;
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
    const std::string resolved_model = resolve_model(api_key, model);
    const int kMaxRounds = 4; // bound on delegation rounds (depth-1 helpers)

    // The core agent's running conversation. It can call spawn_agents across
    // multiple rounds, seeing each round's results before deciding what's next.
    std::vector<ChatMessage> msgs = {
        { "system", kAgentLoopPrompt },
        { "user",   prompt },
    };

    std::vector<std::string> last_tasks, last_results;

    for (int round = 0; round < kMaxRounds; ++round) {
        std::string content = perform_chat(api_key, msgs, resolved_model);

        std::string_view obj = jsonutil::find_object(content);
        std::string tool;
        const bool is_delegation =
            !obj.empty() && jsonutil::get_string(obj, "tool", tool) && tool == "spawn_agents";

        if (!is_delegation) {
            // Terminal: a plain-text answer or a create_file for the CLI.
            CompletionResult r;
            r.content = content;
            return r;
        }

        std::vector<std::string> tasks;
        if (!jsonutil::get_string_array(obj, "tasks", tasks) || tasks.empty()) {
            CompletionResult r; r.content = content; return r; // malformed → pass through
        }

        agent_log("spawn_agents (round " + std::to_string(round + 1) + "): dispatching " +
                  std::to_string(tasks.size()) + " helper agents to separate threads");

        std::vector<std::string> results = run_agents_parallel(api_key, resolved_model, tasks);

        agent_log("spawn_agents (round " + std::to_string(round + 1) + "): all " +
                  std::to_string(tasks.size()) + " helpers joined");
        last_tasks = tasks; last_results = results;

        // Feed the delegation and its results back into the conversation.
        msgs.push_back({ "assistant", content });
        std::string fb = "Results from the helper agents you spawned:\n";
        for (size_t i = 0; i < tasks.size(); ++i) {
            fb += "\n--- Agent " + std::to_string(i + 1) + " (task: " + tasks[i] + ") ---\n";
            fb += results[i] + "\n";
        }
        fb += "\nNow either delegate more work with spawn_agents, or give your final "
              "answer (plain text or create_file).";
        msgs.push_back({ "user", fb });
    }

    // Delegation budget exhausted while still delegating: force a clean synthesis.
    std::string syn = "Original request:\n" + prompt + "\n\nHelper agent results:\n";
    for (size_t i = 0; i < last_tasks.size(); ++i) {
        syn += "\n=== " + last_tasks[i] + " ===\n" + last_results[i] + "\n";
    }
    CompletionResult r;
    r.content = perform_completion(api_key, kSynthesisSystemPrompt, syn, resolved_model);
    return r;
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
    std::string body = build_request_body(kStreamSystemPrompt, prompt, resolved_model, true);
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
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,   1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,   2L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,      CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          300L);

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
