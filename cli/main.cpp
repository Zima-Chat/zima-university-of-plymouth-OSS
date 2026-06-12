// CLI client entry point (§7).
// Thin, stateless: parses args → connects to agent IPC → presents session
// token → issues one RPC → streams result to stdout → exits.
// Holds no secrets, opens no network sockets, persists no state.

#include "terminal_input.h"
#include "../agent/rpc.h"
#include "../agent/session.h"
#include "../agent/json_util.h"

#if defined(__APPLE__) || defined(__linux__)
#include "../platform/posix/ipc_unix.h"
using IpcClientImpl = UnixIpcClient;
#elif defined(_WIN32)
#include "../platform/windows/ipc_pipe.h"
#include <windows.h>
using IpcClientImpl = WindowsIpcClient;
#endif

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include <sodium.h>
#if !defined(_WIN32)
#include <unistd.h>  // getpid, fork, execv
#endif

// ---------- helpers ----------

struct ModelInfo {
    std::string id;
    std::string provider;
    std::string access;
};

static std::string extract_json_string_field(std::string_view obj, std::string_view field) {
    std::string key = std::string("\"") + std::string(field) + "\"";
    size_t pos = obj.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.size();
    while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' || obj[pos] == '\r' || obj[pos] == '\n')) ++pos;
    if (pos >= obj.size() || obj[pos] != ':') return {};
    ++pos;
    while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' || obj[pos] == '\r' || obj[pos] == '\n')) ++pos;
    if (pos >= obj.size() || obj[pos] != '"') return {};
    ++pos;

    std::string out;
    while (pos < obj.size() && obj[pos] != '"') {
        if (obj[pos] == '\\' && pos + 1 < obj.size()) {
            ++pos;
            out += obj[pos];
        } else {
            out += obj[pos];
        }
        ++pos;
    }
    return out;
}

static std::string extract_json_bool_field(std::string_view obj, std::string_view field) {
    std::string key = std::string("\"") + std::string(field) + "\"";
    size_t pos = obj.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.size();
    while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' || obj[pos] == '\r' || obj[pos] == '\n')) ++pos;
    if (pos >= obj.size() || obj[pos] != ':') return {};
    ++pos;
    while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t' || obj[pos] == '\r' || obj[pos] == '\n')) ++pos;
    if (obj.substr(pos, 4) == "true") return "true";
    if (obj.substr(pos, 5) == "false") return "false";
    return {};
}

static std::vector<ModelInfo> parse_models(std::string_view json) {
    std::vector<ModelInfo> models;
    size_t pos = 0;
    while (true) {
        pos = json.find("\"id\"", pos);
        if (pos == std::string_view::npos) break;

        size_t obj_start = json.rfind('{', pos);
        if (obj_start == std::string_view::npos) { pos += 4; continue; }

        int depth = 0;
        size_t obj_end = obj_start;
        for (; obj_end < json.size(); ++obj_end) {
            if (json[obj_end] == '{') ++depth;
            else if (json[obj_end] == '}') {
                --depth;
                if (depth == 0) break;
            }
        }
        if (obj_end >= json.size()) break;

        std::string_view obj = json.substr(obj_start, obj_end - obj_start + 1);
        ModelInfo m;
        m.id = extract_json_string_field(obj, "id");
        if (m.id.empty()) { pos = obj_end + 1; continue; }
        m.provider = extract_json_string_field(obj, "provider");
        if (m.provider.empty()) m.provider = "-";

        m.access = extract_json_string_field(obj, "access_requirements");
        if (m.access.empty()) m.access = extract_json_string_field(obj, "access");
        if (m.access.empty()) m.access = extract_json_string_field(obj, "tier");
        if (m.access.empty()) {
            std::string sub = extract_json_bool_field(obj, "requires_subscription");
            if (!sub.empty()) m.access = (sub == "true") ? "subscription required" : "no subscription required";
        }
        if (m.access.empty()) m.access = "-";

        models.push_back(std::move(m));
        pos = obj_end + 1;
    }
    return models;
}

static IpcClientImpl connect_or_spawn() {
    std::string sock = agent_socket_path();

    // Try to connect first.
    try {
        return IpcClientImpl(sock);
    } catch (...) {}

    // Spawn the agent (§3.3 step 1) and retry once.
#if !defined(_WIN32)
    pid_t pid = fork();
    if (pid == 0) {
        // Child: exec the agent binary (expected alongside this binary).
        const char* argv[] = {"zima-agent", nullptr};
        execvp("zima-agent", const_cast<char* const*>(argv));
        _exit(1);
    }
    // Wait briefly for the agent to start.
    for (int i = 0; i < 20; ++i) {
        usleep(100000); // 100 ms
        try { return IpcClientImpl(sock); } catch (...) {}
    }
#endif
    throw std::runtime_error("Cannot connect to agent. Is zima-agent installed?");
}

// Authenticate with the agent using the on-disk session token (§5.2), then
// bring up the encrypted channel. The channel key is derived from that same
// session token, so the CLI needs no other secret.
static void authenticate(IpcClientImpl& client) {
    SecureBuffer tok = load_session_token();
    client.send_frame(msg_authenticate(tok.span()));
    auto reply = client.recv_frame();           // plaintext AuthOk
    if (frame_type(reply) != MsgType::AuthOk)
        throw std::runtime_error("Agent authentication failed.");
    client.enable_encryption(derive_ipc_key(tok.span()));
}

// ---------- subcommands ----------

static int cmd_login() {
    SecureBuffer key = read_secret("Enter Zima API key: ");
    if (key.empty()) { std::cerr << "No key entered.\n"; return 1; }

    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_configure_credential(key.span()));
    auto reply = client.recv_frame();
    if (frame_type(reply) == MsgType::AuthOk) {
        std::cout << "Logged in successfully.\n";
        return 0;
    }
    auto payload = frame_payload(reply);
    std::string reason;
    if (payload.is_map()) {
        const auto* rv = cbor::map_get(payload.as_map(), "reason");
        if (rv && rv->is_text()) reason = rv->as_text();
        const auto* mv = cbor::map_get(payload.as_map(), "message");
        if (mv && mv->is_text()) reason = mv->as_text();
    }
    std::cerr << "Login failed: " << reason << "\n";
    return 1;
}

static int cmd_logout() {
    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_logout());
    client.recv_frame(); // consume reply
    std::cout << "Logged out.\n";
    return 0;
}

static int cmd_status() {
    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_status());
    auto reply = client.recv_frame();
    auto payload = frame_payload(reply);
    if (payload.is_map()) {
        const auto& m = payload.as_map();
        const auto* locked_v = cbor::map_get(m, "locked");
        const auto* model_v  = cbor::map_get(m, "model");
        bool locked = locked_v && locked_v->is_bool() && locked_v->as_bool();
        std::string model = (model_v && model_v->is_text()) ? model_v->as_text() : "?";
        std::cout << "status: " << (locked ? "locked" : "unlocked")
                  << "  model: " << model << "\n";
    }
    return 0;
}

static int cmd_models() {
    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_models());
    auto reply = client.recv_frame();
    MsgType t = frame_type(reply);
    if (t == MsgType::ModelsReply) {
        auto payload = frame_payload(reply);
        if (payload.is_map()) {
            const auto* jv = cbor::map_get(payload.as_map(), "json");
            if (jv && jv->is_text()) {
                const std::string& json = jv->as_text();
                auto models = parse_models(json);
                if (models.empty()) {
                    std::cout << json << "\n";
                    return 0;
                }

                size_t id_w = 8;
                size_t provider_w = 8;
                for (const auto& m : models) {
                    if (m.id.size() > id_w) id_w = m.id.size();
                    if (m.provider.size() > provider_w) provider_w = m.provider.size();
                }

                std::cout << std::left
                          << std::setw(static_cast<int>(id_w + 2)) << "MODEL"
                          << std::setw(static_cast<int>(provider_w + 2)) << "PROVIDER"
                          << "ACCESS" << "\n";
                for (const auto& m : models) {
                    std::cout << std::left
                              << std::setw(static_cast<int>(id_w + 2)) << m.id
                              << std::setw(static_cast<int>(provider_w + 2)) << m.provider
                              << m.access << "\n";
                }
                return 0;
            }
        }
        std::cerr << "Malformed models reply.\n";
        return 1;
    }
    if (t == MsgType::Error) {
        auto payload = frame_payload(reply);
        std::string msg;
        if (payload.is_map()) {
            const auto* mv = cbor::map_get(payload.as_map(), "message");
            if (mv && mv->is_text()) msg = mv->as_text();
        }
        std::cerr << "Error: " << msg << "\n";
        return 1;
    }
    std::cerr << "Unexpected reply type.\n";
    return 1;
}

static int cmd_usage_info() {
    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_usage());
    auto reply = client.recv_frame();
    MsgType t = frame_type(reply);
    if (t == MsgType::UsageReply) {
        auto payload = frame_payload(reply);
        if (payload.is_map()) {
            const auto* jv = cbor::map_get(payload.as_map(), "json");
            if (jv && jv->is_text()) {
                std::cout << jv->as_text() << "\n";
                return 0;
            }
        }
        std::cerr << "Malformed usage reply.\n";
        return 1;
    }
    if (t == MsgType::Error) {
        auto payload = frame_payload(reply);
        std::string msg;
        if (payload.is_map()) {
            const auto* mv = cbor::map_get(payload.as_map(), "message");
            if (mv && mv->is_text()) msg = mv->as_text();
        }
        std::cerr << "Error: " << msg << "\n";
        return 1;
    }
    std::cerr << "Unexpected reply type.\n";
    return 1;
}

// ---------- assistant tools ----------

// Create a file requested by the assistant. Writes relative to the user's
// current directory; refuses absolute paths and any path that escapes the CWD
// via "..". Prompts before overwriting an existing file.
static void tool_create_file(const std::string& rel_path, const std::string& content) {
    namespace fs = std::filesystem;
    if (rel_path.empty()) { std::cerr << "create_file: empty path.\n"; return; }

    fs::path p(rel_path);
    if (p.is_absolute()) {
        std::cerr << "create_file: refusing absolute path '" << rel_path << "'.\n";
        return;
    }
    for (const auto& part : p) {
        if (part == fs::path("..")) {
            std::cerr << "create_file: refusing path containing '..': '" << rel_path << "'.\n";
            return;
        }
    }

    std::error_code ec;
    if (fs::exists(p, ec)) {
        std::cout << "File '" << rel_path << "' already exists. Overwrite? [y/N] ";
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") { std::cout << "Skipped.\n"; return; }
    }
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "create_file: cannot open '" << rel_path << "' for writing.\n";
        return;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    std::cout << "Created " << rel_path << " (" << content.size() << " bytes)\n";
}

// If the assistant's reply is a tool-call JSON object we understand, execute it
// and return true. Otherwise return false so the caller prints the reply text.
static bool handle_tool_call(const std::string& content) {
    std::string_view obj = jsonutil::find_object(content);
    if (obj.empty()) return false;

    std::string tool;
    if (!jsonutil::get_string(obj, "tool", tool)) return false;

    if (tool == "create_file") {
        std::string path, file_content;
        if (!jsonutil::get_string(obj, "path", path)) {
            std::cerr << "Assistant requested create_file without a path.\n";
            return true;
        }
        jsonutil::get_string(obj, "content", file_content); // empty content allowed
        tool_create_file(path, file_content);
        return true;
    }
    return false; // unknown tool — let the caller print the raw text
}

static int cmd_ask(const std::string& prompt, const std::string& model) {
    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_complete(prompt, model));
    auto reply = client.recv_frame();
    MsgType t = frame_type(reply);
    if (t == MsgType::CompleteReply) {
        auto payload = frame_payload(reply);
        if (payload.is_map()) {
            const auto* cv = cbor::map_get(payload.as_map(), "content");
            if (cv && cv->is_text()) {
                std::string content = cv->as_text();
                // If the model returned a tool call (e.g. create_file), run it;
                // otherwise just print the answer.
                if (!handle_tool_call(content))
                    std::cout << content << "\n";
            }
        }
        return 0;
    }
    if (t == MsgType::Error) {
        auto payload = frame_payload(reply);
        std::string msg;
        if (payload.is_map()) {
            const auto* mv = cbor::map_get(payload.as_map(), "message");
            if (mv && mv->is_text()) msg = mv->as_text();
        }
        std::cerr << "Error: " << msg << "\n";
        return 1;
    }
    std::cerr << "Unexpected reply type.\n";
    return 1;
}

static int cmd_chat(const std::string& model) {
    auto client = connect_or_spawn();
    authenticate(client);
    std::cout << "Zima chat (Ctrl-D to exit)\n";
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        client.send_frame(msg_complete_stream(line, model));

        // A tool call must be parsed whole, but normal prose should still stream
        // live. So we buffer until the first non-whitespace character: if it's
        // '{' or a code fence the reply may be a tool call -> keep buffering and
        // decide at the end; otherwise flush and stream the rest live.
        std::string buf;
        bool streaming = false;

        while (true) {
            auto frame = client.recv_frame();
            MsgType t  = frame_type(frame);

            if (t == MsgType::CompleteChunk) {
                auto payload = frame_payload(frame);
                if (!payload.is_map()) continue;
                const auto* dv = cbor::map_get(payload.as_map(), "delta");
                if (!dv || !dv->is_text()) continue;
                const std::string& delta = dv->as_text();

                if (streaming) {
                    std::cout << delta << std::flush;
                    continue;
                }
                buf += delta;
                size_t i = 0;
                while (i < buf.size() &&
                       (buf[i]==' '||buf[i]=='\t'||buf[i]=='\n'||buf[i]=='\r')) ++i;
                if (i < buf.size() && buf[i] != '{' && buf[i] != '`') {
                    // Plain text: commit to live streaming.
                    streaming = true;
                    std::cout << buf << std::flush;
                    buf.clear();
                }
            } else if (t == MsgType::CompleteDone) {
                if (!streaming) {
                    // Whole reply buffered: run it as a tool call, else print it.
                    if (!handle_tool_call(buf))
                        std::cout << buf;
                }
                std::cout << "\n";
                break;
            } else if (t == MsgType::Error) {
                auto payload = frame_payload(frame);
                std::string msg;
                if (payload.is_map()) {
                    const auto* mv = cbor::map_get(payload.as_map(), "message");
                    if (mv && mv->is_text()) msg = mv->as_text();
                }
                std::cerr << (streaming ? "\n" : "") << "Error: " << msg << "\n";
                break;
            }
        }
    }
    return 0;
}

static int cmd_edit(const std::string& filepath, const std::string& model) {
    // Read file contents.
    std::ifstream f(filepath);
    if (!f.is_open()) { std::cerr << "Cannot open " << filepath << "\n"; return 1; }
    std::string contents((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    std::string prompt = "Please suggest improvements to the following code as a unified diff:\n\n" + contents;
    auto client = connect_or_spawn();
    authenticate(client);
    client.send_frame(msg_complete(prompt, model));
    auto reply = client.recv_frame();
    if (frame_type(reply) != MsgType::CompleteReply) {
        std::cerr << "Request failed.\n"; return 1;
    }
    auto payload = frame_payload(reply);
    std::string diff;
    if (payload.is_map()) {
        const auto* cv = cbor::map_get(payload.as_map(), "content");
        if (cv && cv->is_text()) diff = cv->as_text();
    }

    // Present diff for confirmation.
    std::cout << diff << "\n\nApply this diff? [y/N] ";
    std::string ans;
    std::getline(std::cin, ans);
    if (ans != "y" && ans != "Y") { std::cout << "Aborted.\n"; return 0; }

    // Apply via patch(1).
    std::string cmd = "echo " + std::string("'") + diff + "' | patch " + filepath;
    return std::system(cmd.c_str());
}

// ---------- main ----------

static void usage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " login                  — store API key\n"
              << "  " << prog << " logout                 — remove API key\n"
              << "  " << prog << " status                 — agent status\n"
              << "  " << prog << " models                 — list available models\n"
              << "  " << prog << " usage                  — account usage and billing\n"
              << "  " << prog << " ask <prompt> [--model <id>] — single-turn completion\n"
              << "  " << prog << " chat [--model <id>]    — interactive REPL\n"
              << "  " << prog << " edit <file> [--model <id>] — request and apply a diff\n";
}

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Source strings and API/LLM responses are UTF-8; make the console render
    // and accept them as UTF-8 instead of the legacy OEM code page (which turns
    // an em-dash into "ÔÇö").
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    if (sodium_init() < 0) { std::cerr << "sodium_init failed\n"; return 1; }

    if (argc < 2) { usage(argv[0]); return 1; }
    std::string cmd = argv[1];
    std::string model;
    std::vector<std::string> positional;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--model") {
            if (i + 1 >= argc) {
                std::cerr << "--model requires a value\n";
                return 1;
            }
            model = argv[++i];
            continue;
        }
        positional.push_back(arg);
    }

    try {
        if (cmd == "login")  return cmd_login();
        if (cmd == "logout") return cmd_logout();
        if (cmd == "status") return cmd_status();
        if (cmd == "models") return cmd_models();
        if (cmd == "usage")  return cmd_usage_info();
        if (cmd == "chat")   return cmd_chat(model);

        if (cmd == "ask") {
            if (positional.empty()) { std::cerr << "ask requires a prompt argument\n"; return 1; }
            std::string prompt;
            for (size_t i = 0; i < positional.size(); ++i) {
                if (i != 0) prompt += ' ';
                prompt += positional[i];
            }
            return cmd_ask(prompt, model);
        }

        if (cmd == "edit") {
            if (positional.empty()) { std::cerr << "edit requires a file argument\n"; return 1; }
            return cmd_edit(positional[0], model);
        }

        usage(argv[0]); return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
