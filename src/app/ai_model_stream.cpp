// ai_model_stream.cpp - Streamed chat completions over WinHTTP (ai_model_stream.h).
#include "ai_model_stream.h"

#include "../common/json_value.h"

#include <winhttp.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace pulse::app::ai {

struct ModelStream::Shared {
    std::mutex mu;
    std::vector<ModelEvent> events;
    HINTERNET request = nullptr;   // under mu; closed by whoever clears it
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
};

namespace {

struct Handle {
    HINTERNET value = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET h) : value(h) {}
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

void Push(ModelStream::Shared& sh, ModelEvent ev) {
    std::lock_guard<std::mutex> lock(sh.mu);
    // Merge consecutive deltas of one kind: the UI drains every ~30 ms.
    if ((ev.kind == ModelEvent::Kind::Reasoning || ev.kind == ModelEvent::Kind::Content) && !sh.events.empty() &&
        sh.events.back().kind == ev.kind) {
        sh.events.back().text += ev.text;
        return;
    }
    sh.events.push_back(std::move(ev));
}

ModelEvent Failure(int status, DWORD error, std::string type, std::string message) {
    ModelEvent ev;
    ev.kind = ModelEvent::Kind::Failed;
    ev.http_status = status;
    ev.win32_error = error;
    ev.error_type = std::move(type);
    ev.error_message = std::move(message);
    return ev;
}

struct Url {
    std::wstring host;
    std::wstring path;   // base path without a trailing slash
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

bool Crack(const std::wstring& base, Url& url) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(base.c_str(), static_cast<DWORD>(base.size()), 0, &parts)) return false;
    url.host.assign(host, parts.dwHostNameLength);
    url.path.assign(path, parts.dwUrlPathLength);
    while (!url.path.empty() && url.path.back() == L'/') url.path.pop_back();
    url.port = parts.nPort;
    url.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return !url.host.empty();
}

// One tool call while its pieces stream in.
struct ToolAcc {
    std::string id, name, arguments;
};

void AddCitationUnique(std::vector<ModelCitation>& out, std::string url, std::string title) {
    if (url.empty() || out.size() >= 20) return;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;
    for (ModelCitation& existing : out) {
        if (existing.url == url) {
            if (existing.title.empty() && !title.empty()) existing.title = std::move(title);
            return;
        }
    }
    out.push_back({std::move(title), std::move(url)});
}

void ExtractCitationsFromObject(const json::Value& v, std::vector<ModelCitation>& out) {
    // Top-level citations: ["https://...", ...] or [{"url": ..., "title": ...}]
    if (const json::Value* cits = v.find("citations"); cits && cits->is_array()) {
        for (const json::Value& item : cits->items()) {
            if (item.is_string()) {
                AddCitationUnique(out, item.as_string(), {});
            } else if (item.is_object()) {
                std::string url = item.get_string("url");
                if (url.empty()) url = item.get_string("link");
                AddCitationUnique(out, std::move(url), item.get_string("title"));
            }
        }
    }
    // Zhipu: top-level web_search: [{"title": ..., "link": ...}]
    if (const json::Value* ws = v.find("web_search"); ws && ws->is_array()) {
        for (const json::Value& item : ws->items()) {
            if (!item.is_object()) continue;
            std::string url = item.get_string("link");
            if (url.empty()) url = item.get_string("url");
            AddCitationUnique(out, std::move(url), item.get_string("title"));
        }
    }
    // Qwen DashScope: search_info.search_results: [{"title": ..., "url": ...}]
    if (const json::Value* si = v.find("search_info"); si && si->is_object()) {
        if (const json::Value* res = si->find("search_results"); res && res->is_array()) {
            for (const json::Value& item : res->items()) {
                if (!item.is_object()) continue;
                AddCitationUnique(out, item.get_string("url"), item.get_string("title"));
            }
        }
    }
    // OpenAI / OpenRouter: choices[0].{delta|message}.annotations: [{"type":"url_citation","url_citation":{"url","title"}}]
    auto scan_annotations = [&](const json::Value* container) {
        if (!container || !container->is_object()) return;
        const json::Value* anns = container->find("annotations");
        if (!anns || !anns->is_array()) return;
        for (const json::Value& ann : anns->items()) {
            if (!ann.is_object()) continue;
            const json::Value* uc = ann.find("url_citation");
            if (uc && uc->is_object()) {
                AddCitationUnique(out, uc->get_string("url"), uc->get_string("title"));
            } else {
                AddCitationUnique(out, ann.get_string("url"), ann.get_string("title"));
            }
        }
    };
    if (const json::Value* choices = v.find("choices"); choices && choices->is_array() && choices->size() > 0) {
        const json::Value& choice = choices->at(0);
        scan_annotations(choice.find("delta"));
        scan_annotations(choice.find("message"));
    }
}

struct Assembler {
    std::string content, reasoning, finish;
    std::map<int64_t, ToolAcc> tools;
    std::vector<ModelCitation> citations;
    int64_t prompt_tokens = 0, completion_tokens = 0;

    // One "data:" payload. False when it is not JSON.
    bool Feed(ModelStream::Shared& sh, std::string_view payload) {
        json::Value v;
        if (!json::Parse(payload, v) || !v.is_object()) return false;
        ExtractCitationsFromObject(v, citations);
        if (const json::Value* usage = v.find("usage"); usage && usage->is_object()) {
            if (const json::Value* p = usage->find("prompt_tokens")) p->get_int(prompt_tokens);
            if (const json::Value* c = usage->find("completion_tokens")) c->get_int(completion_tokens);
        }
        const json::Value* choices = v.find("choices");
        if (!choices || !choices->is_array() || choices->size() == 0) return true;
        const json::Value& choice = choices->at(0);
        if (const json::Value* fr = choice.find("finish_reason"); fr && fr->is_string()) finish = fr->as_string();
        const json::Value* delta = choice.find("delta");
        if (!delta || !delta->is_object()) return true;
        if (const json::Value* r = delta->find("reasoning_content"); r && r->is_string() && !r->as_string().empty()) {
            reasoning += r->as_string();
            ModelEvent ev;
            ev.kind = ModelEvent::Kind::Reasoning;
            ev.text = r->as_string();
            Push(sh, std::move(ev));
        }
        if (const json::Value* c = delta->find("content"); c && c->is_string() && !c->as_string().empty()) {
            content += c->as_string();
            ModelEvent ev;
            ev.kind = ModelEvent::Kind::Content;
            ev.text = c->as_string();
            Push(sh, std::move(ev));
        }
        if (const json::Value* calls = delta->find("tool_calls"); calls && calls->is_array()) {
            for (const json::Value& call : calls->items()) {
                int64_t index = static_cast<int64_t>(tools.size());
                if (const json::Value* i = call.find("index")) i->get_int(index, 0, 127);
                ToolAcc& acc = tools[index];
                if (const json::Value* id = call.find("id"); id && id->is_string() && !id->as_string().empty())
                    acc.id = id->as_string();
                if (const json::Value* fn = call.find("function"); fn && fn->is_object()) {
                    if (const json::Value* n = fn->find("name"); n && n->is_string()) acc.name += n->as_string();
                    if (const json::Value* a = fn->find("arguments"); a && a->is_string()) acc.arguments += a->as_string();
                }
            }
        }
        return true;
    }

    ModelEvent Finished() const {
        ModelEvent ev;
        ev.kind = ModelEvent::Kind::Finished;
        ev.finish_reason = finish;
        ev.content = content;
        ev.reasoning = reasoning;
        for (const auto& [index, acc] : tools) {
            if (acc.name.empty()) continue;
            ev.tool_calls.push_back({acc.id.empty() ? "call_" + std::to_string(index) : acc.id, acc.name,
                                     acc.arguments.empty() ? "{}" : acc.arguments});
        }
        ev.citations = citations;
        ev.prompt_tokens = prompt_tokens;
        ev.completion_tokens = completion_tokens;
        return ev;
    }
};

// The service's {"error": {"message", "type", "code"}} body.
void ReadError(const std::string& body, std::string& type, std::string& message) {
    json::Value v;
    if (!json::Parse(body, v) || !v.is_object()) return;
    const json::Value* err = v.find("error");
    if (!err || !err->is_object()) return;
    message = err->get_string("message");
    type = err->get_string("type");
    if (type.empty()) type = err->get_string("code");
}

void Run(std::shared_ptr<ModelStream::Shared> sh, ModelEndpoint endpoint, std::string body) {
    struct DoneGuard {
        ModelStream::Shared& s;
        ~DoneGuard() { s.done.store(true); }
    } guard{*sh};
    Url url;
    if (!Crack(endpoint.base_url, url)) {
        Push(*sh, Failure(0, ERROR_INVALID_PARAMETER, "bad_url", "The service address is not a valid URL."));
        return;
    }
    Handle session(WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value)   // Windows before 8.1: no automatic proxy
        session.value = WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        Push(*sh, Failure(0, GetLastError(), "network", {}));
        return;
    }
    // Thinking can go quiet for a while between chunks; the service sends
    // keep-alive comments, so a long receive timeout only catches dead links.
    WinHttpSetTimeouts(session.value, 10000, 15000, 30000, 180000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    Handle connection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
    if (!connection.value) {
        Push(*sh, Failure(0, GetLastError(), "network", {}));
        return;
    }
    if (endpoint.anthropic) {
        body = BuildAnthropicRequestBody(body);
    }
    const std::wstring path = endpoint.anthropic ? (url.path + L"/messages") : (url.path + L"/chat/completions");
    HINTERNET request = WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        Push(*sh, Failure(0, GetLastError(), "network", {}));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sh->mu);
        if (sh->cancelled.load()) {
            WinHttpCloseHandle(request);
            return;
        }
        sh->request = request;
    }
    // Closes the request unless Cancel already did.
    struct RequestGuard {
        ModelStream::Shared& s;
        ~RequestGuard() {
            std::lock_guard<std::mutex> lock(s.mu);
            if (s.request) WinHttpCloseHandle(s.request);
            s.request = nullptr;
        }
    } request_guard{*sh};

    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (endpoint.anthropic) {
        headers += L"x-api-key: " + endpoint.key + L"\r\nanthropic-version: 2023-06-01\r\n";
    } else {
        headers += L"Authorization: Bearer " + endpoint.key + L"\r\n";
    }
    SecureZeroMemory(endpoint.key.data(), endpoint.key.size() * sizeof(wchar_t));
    const BOOL sent = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), body.data(),
                                         static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        const DWORD error = GetLastError();
        if (!sh->cancelled.load()) Push(*sh, Failure(0, error, "network", {}));
        return;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);

    std::string pending;   // bytes after the last complete line
    std::string error_body;
    Assembler acc;
    AnthropicSseState anth;
    bool finished = false;
    std::vector<char> buffer(16 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            const DWORD error = GetLastError();
            if (!sh->cancelled.load()) Push(*sh, Failure(0, error, "network", {}));
            return;
        }
        if (read == 0) break;
        if (status != 200) {
            if (error_body.size() < 64 * 1024) error_body.append(buffer.data(), read);
            continue;
        }
        pending.append(buffer.data(), read);
        size_t start = 0;
        for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string_view line(pending.data() + start, nl - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.size() < 5 || line.compare(0, 5, "data:") != 0) continue;   // comments, event:, blank
            line.remove_prefix(5);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (line == "[DONE]") {
                finished = true;
                break;
            }
            if (endpoint.anthropic) {
                std::vector<ModelEvent> deltas;
                anth.Feed(line, deltas);
                for (ModelEvent& ev : deltas) Push(*sh, std::move(ev));
                if (anth.message_stopped) {
                    finished = true;
                    break;
                }
            } else {
                acc.Feed(*sh, line);
            }
        }
        pending.erase(0, start);
        if (finished || sh->cancelled.load()) break;
    }
    if (sh->cancelled.load()) return;
    if (status != 200) {
        std::string type, message;
        ReadError(error_body, type, message);
        Push(*sh, Failure(static_cast<int>(status), 0, std::move(type), std::move(message)));
        return;
    }
    if (endpoint.anthropic) {
        if (!finished && anth.finish_reason.empty()) {
            Push(*sh, Failure(0, ERROR_HANDLE_EOF, "incomplete", "The answer ended early."));
            return;
        }
        Push(*sh, anth.BuildFinished());
        return;
    }
    if (!finished && acc.finish.empty()) {
        Push(*sh, Failure(0, ERROR_HANDLE_EOF, "incomplete", "The answer ended early."));
        return;
    }
    Push(*sh, acc.Finished());
}

} // namespace

std::shared_ptr<ModelStream> ModelStream::Start(ModelEndpoint endpoint, std::string body) {
    std::shared_ptr<ModelStream> stream(new ModelStream());
    stream->shared_ = std::make_shared<Shared>();
    std::thread(Run, stream->shared_, std::move(endpoint), std::move(body)).detach();
    return stream;
}

ModelStream::~ModelStream() { Cancel(); }

void ModelStream::Cancel() {
    if (!shared_) return;
    shared_->cancelled.store(true);
    std::lock_guard<std::mutex> lock(shared_->mu);
    if (shared_->request) {
        WinHttpCloseHandle(shared_->request);   // a blocked send or read fails at once
        shared_->request = nullptr;
    }
}

void ModelStream::Drain(std::vector<ModelEvent>& out) {
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mu);
    for (ModelEvent& ev : shared_->events) out.push_back(std::move(ev));
    shared_->events.clear();
}

bool ModelStream::Ended() const {
    if (!shared_) return true;
    if (!shared_->done.load()) return false;
    std::lock_guard<std::mutex> lock(shared_->mu);
    return shared_->events.empty();
}

ModelProbe ProbeModelService(ModelEndpoint endpoint) {
    ModelProbe probe;
    const ULONGLONG started = GetTickCount64();
    struct Wipe {
        std::wstring& key;
        ~Wipe() { SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t)); }
    } wipe{endpoint.key};
    Url url;
    if (!Crack(endpoint.base_url, url)) {
        probe.win32_error = ERROR_INVALID_PARAMETER;
        probe.error_type = "bad_url";
        return probe;
    }
    Handle session(WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value)
        session.value = WinHttpOpen(L"Pulse", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        probe.win32_error = GetLastError();
        return probe;
    }
    WinHttpSetTimeouts(session.value, 8000, 10000, 10000, 15000);
    Handle connection(WinHttpConnect(session.value, url.host.c_str(), url.port, 0));
    const std::wstring path = url.path + L"/models";
    Handle request(connection.value ? WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr,
                                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                         url.secure ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr);
    if (!request.value) {
        probe.win32_error = GetLastError();
        return probe;
    }
    std::wstring headers = L"Accept: application/json\r\nAuthorization: Bearer " + endpoint.key + L"\r\n";
    // Anthropic lists its models only to its own key header (chat takes the bearer).
    const std::wstring anthropic = L"api.anthropic.com";
    if (url.host.size() >= anthropic.size() &&
        url.host.compare(url.host.size() - anthropic.size(), anthropic.size(), anthropic) == 0)
        headers += L"x-api-key: " + endpoint.key + L"\r\nanthropic-version: 2023-06-01\r\n";
    const BOOL sent = WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()),
                                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    SecureZeroMemory(headers.data(), headers.size() * sizeof(wchar_t));
    if (!sent || !WinHttpReceiveResponse(request.value, nullptr)) {
        probe.win32_error = GetLastError();
        return probe;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string body;
    std::vector<char> buffer(8192);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read) || read == 0) break;
        // OpenRouter's list (descriptions, prices …) runs to megabytes.
        if (body.size() < 16 * 1024 * 1024) body.append(buffer.data(), read);
    }
    probe.http_status = static_cast<int>(status);
    probe.ms = static_cast<uint32_t>(GetTickCount64() - started);
    if (status == 200) {
        json::Value v;
        if (json::Parse(body, v) && v.is_object())
            if (const json::Value* data = v.find("data"); data && data->is_array())
                for (const json::Value& m : data->items()) {
                    std::string id = m.get_string("id");
                    if (id.empty()) continue;
                    // OpenRouter: supported_parameters without "tools" (仅聊天).
                    if (const json::Value* params = m.find("supported_parameters"); params && params->is_array()) {
                        bool tools = false;
                        for (const json::Value& p : params->items())
                            if (p.is_string() && p.as_string() == "tools") tools = true;
                        if (!tools) probe.chat_only.push_back(id);
                    }
                    probe.models.push_back(std::move(id));
                }
    } else {
        ReadError(body, probe.error_type, probe.error_message);
    }
    return probe;
}

std::wstring ModelHost(const std::wstring& base_url) {
    Url url;
    return Crack(base_url, url) ? url.host : base_url;
}

void CollectModelCitations(std::string_view sse_json, std::vector<ModelCitation>& out) {
    json::Value v;
    if (!json::Parse(sse_json, v) || !v.is_object()) return;
    ExtractCitationsFromObject(v, out);
}

std::string BuildAnthropicRequestBody(std::string_view openai_body) {
    json::Value src;
    if (!json::Parse(openai_body, src) || !src.is_object()) return std::string(openai_body);

    json::Value out = json::Value::Object();
    out.set("model", src.get_string("model"));
    out.set("max_tokens", 8192);
    out.set("stream", src.get_bool("stream", true));

    // Extract system prompt into a cache_control block for Anthropic Prompt Caching.
    std::string sys_text;
    json::Value out_msgs = json::Value::Array();
    if (const json::Value* msgs = src.find("messages"); msgs && msgs->is_array()) {
        auto append_user_block = [&](json::Value block) {
            if (out_msgs.size() > 0 && out_msgs.at(out_msgs.size() - 1).get_string("role") == "user") {
                // Coalesce consecutive user / tool_result messages as Anthropic requires.
                json::Value prev = out_msgs.at(out_msgs.size() - 1);
                json::Value arr = json::Value::Array();
                if (const json::Value* pc = prev.find("content")) {
                    if (pc->is_array()) {
                        for (const json::Value& it : pc->items()) arr.push(it);
                    } else if (pc->is_string() && !pc->as_string().empty()) {
                        json::Value tb = json::Value::Object();
                        tb.set("type", "text");
                        tb.set("text", pc->as_string());
                        arr.push(std::move(tb));
                    }
                }
                arr.push(std::move(block));
                prev.set("content", std::move(arr));
                // Replace last element cleanly.
                json::Value rebuilt = json::Value::Array();
                for (size_t i = 0; i + 1 < out_msgs.size(); ++i) rebuilt.push(out_msgs.at(i));
                rebuilt.push(std::move(prev));
                out_msgs = std::move(rebuilt);
            } else {
                json::Value m = json::Value::Object();
                m.set("role", "user");
                json::Value arr = json::Value::Array();
                arr.push(std::move(block));
                m.set("content", std::move(arr));
                out_msgs.push(std::move(m));
            }
        };

        for (const json::Value& m : msgs->items()) {
            const std::string role = m.get_string("role");
            if (role == "system") {
                const std::string t = m.get_string("content");
                if (!t.empty()) {
                    if (!sys_text.empty()) sys_text += "\n\n";
                    sys_text += t;
                }
            } else if (role == "user") {
                if (const json::Value* cnt = m.find("content")) {
                    if (cnt->is_string()) {
                        json::Value tb = json::Value::Object();
                        tb.set("type", "text");
                        tb.set("text", cnt->as_string());
                        append_user_block(std::move(tb));
                    } else if (cnt->is_array()) {
                        for (const json::Value& part : cnt->items()) {
                            const std::string t = part.get_string("type");
                            if (t == "text") {
                                json::Value tb = json::Value::Object();
                                tb.set("type", "text");
                                tb.set("text", part.get_string("text"));
                                append_user_block(std::move(tb));
                            } else if (t == "image_url") {
                                std::string url;
                                if (const json::Value* iu = part.find("image_url"); iu && iu->is_object()) {
                                    url = iu->get_string("url");
                                }
                                // Parse data:<mime>;base64,<payload>
                                if (url.rfind("data:", 0) == 0) {
                                    const size_t semi = url.find(";base64,");
                                    if (semi != std::string::npos && semi > 5) {
                                        json::Value ib = json::Value::Object();
                                        ib.set("type", "image");
                                        json::Value src_obj = json::Value::Object();
                                        src_obj.set("type", "base64");
                                        src_obj.set("media_type", url.substr(5, semi - 5));
                                        src_obj.set("data", url.substr(semi + 8));
                                        ib.set("source", std::move(src_obj));
                                        append_user_block(std::move(ib));
                                    }
                                }
                            }
                        }
                    }
                }
            } else if (role == "tool") {
                json::Value tr = json::Value::Object();
                tr.set("type", "tool_result");
                tr.set("tool_use_id", m.get_string("tool_call_id"));
                tr.set("content", m.get_string("content"));
                append_user_block(std::move(tr));
            } else if (role == "assistant") {
                json::Value am = json::Value::Object();
                am.set("role", "assistant");
                json::Value blocks = json::Value::Array();
                const std::string text = m.get_string("content");
                if (!text.empty()) {
                    json::Value tb = json::Value::Object();
                    tb.set("type", "text");
                    tb.set("text", text);
                    blocks.push(std::move(tb));
                }
                if (const json::Value* tcs = m.find("tool_calls"); tcs && tcs->is_array()) {
                    for (const json::Value& tc : tcs->items()) {
                        const json::Value* fn = tc.find("function");
                        if (!fn || !fn->is_object()) continue;
                        json::Value tu = json::Value::Object();
                        tu.set("type", "tool_use");
                        tu.set("id", tc.get_string("id"));
                        tu.set("name", fn->get_string("name"));
                        json::Value input = json::Value::Object();
                        const std::string args_str = fn->get_string("arguments");
                        if (!args_str.empty()) {
                            json::Value parsed;
                            if (json::Parse(args_str, parsed) && parsed.is_object()) input = std::move(parsed);
                        }
                        tu.set("input", std::move(input));
                        blocks.push(std::move(tu));
                    }
                }
                if (blocks.size() == 0) {
                    json::Value tb = json::Value::Object();
                    tb.set("type", "text");
                    tb.set("text", " ");
                    blocks.push(std::move(tb));
                }
                am.set("content", std::move(blocks));
                out_msgs.push(std::move(am));
            }
        }
    }
    if (!sys_text.empty()) {
        json::Value sys_arr = json::Value::Array();
        json::Value sb = json::Value::Object();
        sb.set("type", "text");
        sb.set("text", std::move(sys_text));
        json::Value cc = json::Value::Object();
        cc.set("type", "ephemeral");
        sb.set("cache_control", std::move(cc));
        sys_arr.push(std::move(sb));
        out.set("system", std::move(sys_arr));
    }
    out.set("messages", std::move(out_msgs));

    // Translate OpenAI tools -> Anthropic tools, adding cache_control on the last tool.
    if (const json::Value* tools = src.find("tools"); tools && tools->is_array() && tools->size() > 0) {
        json::Value out_tools = json::Value::Array();
        for (const json::Value& t : tools->items()) {
            const json::Value* fn = t.find("function");
            if (!fn || !fn->is_object()) continue;
            json::Value at = json::Value::Object();
            at.set("name", fn->get_string("name"));
            at.set("description", fn->get_string("description"));
            if (const json::Value* params = fn->find("parameters"); params && params->is_object()) {
                at.set("input_schema", *params);
            } else {
                json::Value empty_schema = json::Value::Object();
                empty_schema.set("type", "object");
                empty_schema.set("properties", json::Value::Object());
                at.set("input_schema", std::move(empty_schema));
            }
            out_tools.push(std::move(at));
        }
        if (out_tools.size() > 0) {
            json::Value last = out_tools.at(out_tools.size() - 1);
            json::Value cc = json::Value::Object();
            cc.set("type", "ephemeral");
            last.set("cache_control", std::move(cc));
            json::Value rebuilt = json::Value::Array();
            for (size_t i = 0; i + 1 < out_tools.size(); ++i) rebuilt.push(out_tools.at(i));
            rebuilt.push(std::move(last));
            out.set("tools", std::move(rebuilt));
        }
    }
    return json::Serialize(out);
}

bool AnthropicSseState::Feed(std::string_view data_json, std::vector<ModelEvent>& out_deltas) {
    json::Value v;
    if (!json::Parse(data_json, v) || !v.is_object()) return false;
    const std::string type = v.get_string("type");
    if (type == "message_start") {
        if (const json::Value* msg = v.find("message"); msg && msg->is_object()) {
            if (const json::Value* u = msg->find("usage"); u && u->is_object()) {
                int64_t inp = 0, cr = 0, cc = 0;
                if (const json::Value* x = u->find("input_tokens")) x->get_int(inp);
                if (const json::Value* x = u->find("cache_read_input_tokens")) x->get_int(cr);
                if (const json::Value* x = u->find("cache_creation_input_tokens")) x->get_int(cc);
                prompt_tokens = inp + cr + cc;
            }
        }
    } else if (type == "content_block_start") {
        int64_t idx = static_cast<int64_t>(blocks.size());
        if (const json::Value* iv = v.find("index")) iv->get_int(idx, 0, 255);
        if (static_cast<size_t>(idx) >= blocks.size()) blocks.resize(static_cast<size_t>(idx) + 1);
        BlockAcc& b = blocks[static_cast<size_t>(idx)];
        if (const json::Value* cb = v.find("content_block"); cb && cb->is_object()) {
            b.kind = cb->get_string("type");
            if (b.kind == "tool_use") {
                b.id = cb->get_string("id");
                b.name = cb->get_string("name");
            }
        }
    } else if (type == "content_block_delta") {
        int64_t idx = 0;
        if (const json::Value* iv = v.find("index")) iv->get_int(idx, 0, 255);
        if (static_cast<size_t>(idx) >= blocks.size()) blocks.resize(static_cast<size_t>(idx) + 1);
        BlockAcc& b = blocks[static_cast<size_t>(idx)];
        if (const json::Value* d = v.find("delta"); d && d->is_object()) {
            const std::string dt = d->get_string("type");
            if (dt == "text_delta") {
                const std::string txt = d->get_string("text");
                if (!txt.empty()) {
                    content += txt;
                    ModelEvent ev;
                    ev.kind = ModelEvent::Kind::Content;
                    ev.text = txt;
                    out_deltas.push_back(std::move(ev));
                }
            } else if (dt == "thinking_delta") {
                const std::string th = d->get_string("thinking");
                if (!th.empty()) {
                    reasoning += th;
                    ModelEvent ev;
                    ev.kind = ModelEvent::Kind::Reasoning;
                    ev.text = th;
                    out_deltas.push_back(std::move(ev));
                }
            } else if (dt == "input_json_delta") {
                b.json_args += d->get_string("partial_json");
            }
        }
    } else if (type == "message_delta") {
        if (const json::Value* d = v.find("delta"); d && d->is_object()) {
            const std::string sr = d->get_string("stop_reason");
            if (!sr.empty()) finish_reason = sr;
        }
        if (const json::Value* u = v.find("usage"); u && u->is_object()) {
            if (const json::Value* x = u->find("output_tokens")) x->get_int(completion_tokens);
        }
    } else if (type == "message_stop") {
        message_stopped = true;
    }
    return true;
}

ModelEvent AnthropicSseState::BuildFinished() const {
    ModelEvent ev;
    ev.kind = ModelEvent::Kind::Finished;
    ev.finish_reason = finish_reason.empty() ? "end_turn" : finish_reason;
    ev.content = content;
    ev.reasoning = reasoning;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const BlockAcc& b = blocks[i];
        if (b.kind != "tool_use" || b.name.empty()) continue;
        ev.tool_calls.push_back({b.id.empty() ? "toolu_" + std::to_string(i) : b.id, b.name,
                                 b.json_args.empty() ? "{}" : b.json_args});
    }
    ev.prompt_tokens = prompt_tokens;
    ev.completion_tokens = completion_tokens;
    return ev;
}

} // namespace pulse::app::ai
