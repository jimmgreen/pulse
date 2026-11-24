#include "mcp_server.h"
#include "broker_client.h"
#include "mcp_tools.h"
#include "../common/json_value.h"
#include "../common/mcp_update_gate.h"
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace pulse::ai_host {
namespace {

// Newest first. Modern versions take per-request _meta; legacy ones use initialize.
constexpr const char* kModernVersions[] = {"2026-07-28"};
constexpr const char* kLegacyVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

constexpr char kMetaVersion[] = "io.modelcontextprotocol/protocolVersion";
constexpr char kMetaClientInfo[] = "io.modelcontextprotocol/clientInfo";
constexpr char kMetaServerInfo[] = "io.modelcontextprotocol/serverInfo";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kUnsupportedVersion = -32022;

constexpr char kInstructions[] =
    "Pulse is the file manager on this PC. These tools find, list and describe files and folders, show them to "
    "the user in Pulse, pack them into a zip archive (the user confirms in Pulse) and undo what an AI client "
    "changed. They return names, paths and properties only; file contents are never read.";

bool KnownVersion(std::string_view v) {
    for (const char* known : kModernVersions) if (v == known) return true;
    for (const char* known : kLegacyVersions) if (v == known) return true;
    return false;
}

bool IsLegacyVersion(std::string_view v) {
    for (const char* known : kLegacyVersions) if (v == known) return true;
    return false;
}

json::Value SupportedVersions() {
    json::Value list = json::Value::Array();
    for (const char* v : kModernVersions) list.push(v);
    for (const char* v : kLegacyVersions) list.push(v);
    return list;
}

// What the broker accepts as a client name: 1-128 UTF-16 units, no controls.
std::string CleanClientName(const std::string& utf8) {
    std::wstring name = json::Wide(utf8);
    std::wstring out;
    for (wchar_t c : name)
        if (c >= 0x20 && c != 0x7F) out += c;
    if (out.size() > 64) out.resize(64);
    if (!out.empty() && out.back() >= 0xD800 && out.back() <= 0xDBFF) out.pop_back();   // split pair
    std::string clean = json::Utf8(out);
    return clean.empty() ? std::string("MCP client") : clean;
}

json::Value TextContent(std::string text) {
    json::Value item = json::Value::Object();
    item.set("type", "text");
    item.set("text", std::move(text));
    json::Value content = json::Value::Array();
    content.push(std::move(item));
    return content;
}

json::Value ToolSuccess(const json::Value& data) {
    json::Value result = json::Value::Object();
    result.set("content", TextContent(json::Serialize(data)));
    if (data.is_object()) result.set("structuredContent", data);
    result.set("isError", false);
    return result;
}

json::Value ToolError(const std::string& code, const std::string& message, bool retryable) {
    std::string text = code;
    if (!message.empty()) text += ": " + message;
    if (retryable) text += " (retryable)";
    json::Value result = json::Value::Object();
    result.set("content", TextContent(std::move(text)));
    result.set("isError", true);
    return result;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

class Server {
public:
    explicit Server(const McpServerOptions& options)
        : options_(options), broker_(options.pipe_name, options.server_image, options.version) {}

    int Run() {
        const auto gate = mcp_update::GatePath(mcp_update::InstallRootForHost(options_.server_image));
        if (mcp_update::IsGated(gate)) {
            Diagnostic(mcp_update::kRetryMessage);
            return static_cast<int>(mcp_update::kRetryAfterUpdate);
        }
        std::atomic<bool> update{false};
        HANDLE reader = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &reader, 0, FALSE, DUPLICATE_SAME_ACCESS);
        const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::jthread watcher;
        if (reader && done) watcher = std::jthread([&] {
            do {
                if (update || mcp_update::IsGated(gate)) {
                    update = true;
                    // Repeat to cover the race between checking update in the
                    // main loop and entering the client's synchronous ReadFile.
                    CancelSynchronousIo(reader);
                }
            } while (WaitForSingleObject(done, update ? 25 : 200) == WAIT_TIMEOUT);
        });
        Diagnostic("Pulse.AI: MCP server ready");
        std::string buffer;
        std::string chunk(64 * 1024, '\0');
        bool discarding = false;   // inside a line that was too long
        while (!update) {
            DWORD n = 0;
            if (!ReadFile(options_.input, chunk.data(), static_cast<DWORD>(chunk.size()), &n, nullptr) || n == 0) break;
            buffer.append(chunk.data(), n);
            size_t start = 0;
            for (;;) {
                const size_t newline = buffer.find('\n', start);
                if (newline == std::string::npos) break;
                if (!discarding) HandleLine(std::string_view(buffer).substr(start, newline - start));
                discarding = false;
                start = newline + 1;
            }
            buffer.erase(0, start);
            if (buffer.size() > kMaxLineBytes) {
                if (!discarding) RespondError(nullptr, kParseError, "Message too large");
                discarding = true;
                buffer.clear();
            }
            Reap(false);
            if (output_broken_) break;
        }
        if (!update && !discarding && !output_broken_) HandleLine(buffer);   // a last line without newline
        Reap(true);
        if (update) Diagnostic(mcp_update::kRetryMessage);
        else Diagnostic("Pulse.AI: input closed");
        if (watcher.joinable()) { SetEvent(done); watcher.join(); }
        if (done) CloseHandle(done);
        if (reader) CloseHandle(reader);
        return update ? static_cast<int>(mcp_update::kRetryAfterUpdate) : 0;
    }

private:
    struct Call {
        json::Value id;
        std::string key;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> done{false};
        std::thread thread;
    };

    void Write(const json::Value& message) {
        std::string line = json::Serialize(message);
        line += '\n';
        std::lock_guard lock(write_mu_);
        if (output_broken_) return;
        const char* data = line.data();
        size_t left = line.size();
        while (left) {
            DWORD written = 0;
            if (!WriteFile(options_.output, data, static_cast<DWORD>(left), &written, nullptr) || written == 0) {
                output_broken_ = true;
                return;
            }
            data += written;
            left -= written;
        }
    }

    void Diagnostic(const char* text) {
        if (!options_.diagnostics || options_.diagnostics == INVALID_HANDLE_VALUE) return;
        std::string line = std::string(text) + "\n";
        DWORD written = 0;
        WriteFile(options_.diagnostics, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    json::Value ServerInfo() const {
        json::Value info = json::Value::Object();
        info.set("name", "pulse");
        info.set("title", "Pulse");
        info.set("version", options_.version);
        return info;
    }

    void Respond(const json::Value& id, json::Value result, bool modern) {
        if (modern) {
            result.set("resultType", "complete");
            json::Value meta = json::Value::Object();
            meta.set(kMetaServerInfo, ServerInfo());
            result.set("_meta", std::move(meta));
        }
        json::Value m = json::Value::Object();
        m.set("jsonrpc", "2.0");
        m.set("id", id);
        m.set("result", std::move(result));
        Write(m);
    }

    void RespondError(const json::Value& id, int code, std::string message, json::Value data = {}) {
        json::Value error = json::Value::Object();
        error.set("code", code);
        error.set("message", std::move(message));
        if (!data.is_null()) error.set("data", std::move(data));
        json::Value m = json::Value::Object();
        m.set("jsonrpc", "2.0");
        m.set("id", id);
        m.set("error", std::move(error));
        Write(m);
    }

    void NameClient(const json::Value* info) {
        if (client_named_ || !info || !info->is_object()) return;
        const std::string name = info->get_string("name");
        if (name.empty()) return;
        client_named_ = true;
        broker_.SetClient(CleanClientName(name), info->get_string("version").substr(0, 64));
    }

    void HandleLine(std::string_view raw) {
        const std::string_view line = Trim(raw);
        if (line.empty()) return;
        json::Value m;
        json::ParseLimits limits;
        limits.max_bytes = kMaxLineBytes;
        if (!json::Parse(line, m, nullptr, limits)) {
            RespondError(nullptr, kParseError, "Parse error");
            return;
        }
        if (m.is_array()) {
            RespondError(nullptr, kInvalidRequest, "Batches are not supported");
            return;
        }
        if (!m.is_object()) {
            RespondError(nullptr, kInvalidRequest, "Invalid request");
            return;
        }
        const json::Value* id = m.find("id");
        const json::Value* method = m.find("method");
        if (!method) return;   // a response: this server sends no requests
        int64_t numeric = 0;
        const bool valid_id = id && (id->is_string() || id->get_int(numeric));
        if (m.get_string("jsonrpc") != "2.0" || !method->is_string()) {
            if (id) RespondError(valid_id ? *id : json::Value(), kInvalidRequest, "Invalid request");
            return;
        }
        const json::Value* params_member = m.find("params");
        const json::Value params = params_member ? *params_member : json::Value::Object();
        if (!params.is_object()) {
            if (id) RespondError(valid_id ? *id : json::Value(), kInvalidParams, "params must be an object");
            return;
        }
        if (!id) {
            HandleNotification(method->as_string(), params);
            return;
        }
        if (!valid_id) {
            RespondError(nullptr, kInvalidRequest, "Invalid request id");
            return;
        }
        HandleRequest(*id, method->as_string(), params);
    }

    void HandleNotification(const std::string& method, const json::Value& params) {
        if (method != "notifications/cancelled") return;   // initialized and the rest need nothing
        const json::Value* request = params.find("requestId");
        if (!request) return;
        const std::string key = json::Serialize(*request);
        std::lock_guard lock(calls_mu_);
        for (auto& call : calls_)
            if (call->key == key) call->cancelled = true;
    }

    void HandleRequest(const json::Value& id, const std::string& method, const json::Value& params) {
        const json::Value* meta = params.find("_meta");
        if (meta && !meta->is_object()) meta = nullptr;
        const json::Value* version = meta ? meta->find(kMetaVersion) : nullptr;
        const bool modern = version != nullptr;
        if (modern) {
            if (!version->is_string() || !KnownVersion(version->as_string())) {
                json::Value data = json::Value::Object();
                data.set("supported", SupportedVersions());
                data.set("requested", *version);
                RespondError(id, kUnsupportedVersion, "Unsupported protocol version", std::move(data));
                return;
            }
            NameClient(meta->find(kMetaClientInfo));
        }

        if (method == "initialize") {
            const std::string requested = params.get_string("protocolVersion");
            NameClient(params.find("clientInfo"));
            json::Value tools = json::Value::Object();
            tools.set("listChanged", false);
            json::Value capabilities = json::Value::Object();
            capabilities.set("tools", std::move(tools));
            json::Value result = json::Value::Object();
            result.set("protocolVersion", IsLegacyVersion(requested) ? requested : std::string(kLegacyVersions[0]));
            result.set("capabilities", std::move(capabilities));
            result.set("serverInfo", ServerInfo());
            result.set("instructions", kInstructions);
            Respond(id, std::move(result), false);
        } else if (method == "server/discover") {
            json::Value tools = json::Value::Object();
            tools.set("listChanged", false);
            json::Value capabilities = json::Value::Object();
            capabilities.set("tools", std::move(tools));
            json::Value result = json::Value::Object();
            result.set("supportedVersions", SupportedVersions());
            result.set("capabilities", std::move(capabilities));
            result.set("instructions", kInstructions);
            result.set("ttlMs", 3600000);
            result.set("cacheScope", "private");
            Respond(id, std::move(result), true);
        } else if (method == "ping") {
            Respond(id, json::Value::Object(), modern);
        } else if (method == "tools/list") {
            json::Value result = ToolsList();
            if (modern) {
                result.set("ttlMs", 3600000);
                result.set("cacheScope", "private");
            }
            Respond(id, std::move(result), modern);
        } else if (method == "tools/call") {
            CallTool(id, params, meta, modern);
        } else {
            RespondError(id, kMethodNotFound, "Method not found");
        }
    }

    void CallTool(const json::Value& id, const json::Value& params, const json::Value* meta, bool modern) {
        const std::string name = params.get_string("name");
        const ToolDef* tool = FindTool(name);
        if (!tool) {
            RespondError(id, kInvalidParams, "Unknown tool: " + name.substr(0, 64));
            return;
        }
        const json::Value* args_member = params.find("arguments");
        json::Value args = args_member ? *args_member : json::Value::Object();
        if (!args.is_object()) {
            RespondError(id, kInvalidParams, "arguments must be an object");
            return;
        }
        const std::string reason = ValidateArguments(*tool, args);
        if (!reason.empty()) {
            Respond(id, ToolError("invalid_args", reason, false), modern);
            return;
        }
        json::Value token;
        if (meta)
            if (const json::Value* t = meta->find("progressToken"); t && (t->is_string() || t->is_number())) token = *t;

        auto call = std::make_shared<Call>();
        call->id = id;
        call->key = json::Serialize(id);
        {
            std::lock_guard lock(calls_mu_);
            size_t running = 0;
            for (const auto& other : calls_) {
                if (other->done) continue;
                if (other->key == call->key) {
                    RespondError(id, kInvalidRequest, "Request id already in use");
                    return;
                }
                ++running;
            }
            if (running >= kMaxCalls) {
                Respond(id, ToolError("busy", "Too many calls at once; try again.", true), modern);
                return;
            }
            calls_.push_back(call);
        }
        call->thread = std::thread([this, call, broker_tool = tool->broker_name(), args = std::move(args),
                                    token = std::move(token), modern] {
            auto waiting = [&] {
                if (token.is_null() || call->cancelled) return;
                json::Value progress = json::Value::Object();
                progress.set("progressToken", token);
                progress.set("progress", 0);
                progress.set("message", "Waiting for the user to confirm in Pulse");
                json::Value m = json::Value::Object();
                m.set("jsonrpc", "2.0");
                m.set("method", "notifications/progress");
                m.set("params", std::move(progress));
                Write(m);
            };
            const BrokerReply reply = broker_.Call(broker_tool, args, call->cancelled, waiting);
            // A cancelled request gets no answer at all.
            if (!call->cancelled)
                Respond(call->id, reply.ok ? ToolSuccess(reply.data) : ToolError(reply.code, reply.message, reply.retryable),
                        modern);
            call->done = true;
        });
    }

    // Joins finished calls; `all`: cancels and joins every call (shutting down).
    void Reap(bool all) {
        std::list<std::shared_ptr<Call>> finished;
        {
            std::lock_guard lock(calls_mu_);
            if (all)
                for (auto& call : calls_) call->cancelled = true;
            for (auto it = calls_.begin(); it != calls_.end();) {
                if (all || (*it)->done) {
                    finished.push_back(*it);
                    it = calls_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (all) broker_.Shutdown();
        for (auto& call : finished)
            if (call->thread.joinable()) call->thread.join();
    }

    const McpServerOptions& options_;
    BrokerClient broker_;
    std::mutex write_mu_;
    std::atomic<bool> output_broken_{false};
    std::mutex calls_mu_;
    std::list<std::shared_ptr<Call>> calls_;
    bool client_named_ = false;
};

} // namespace

int RunMcpServer(const McpServerOptions& options) {
    Server server(options);
    return server.Run();
}

} // namespace pulse::ai_host
