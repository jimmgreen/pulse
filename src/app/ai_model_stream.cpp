// ai_model_stream.cpp - Streamed chat completions over WinHTTP (ai_model_stream.h).
#include "ai_model_stream.h"

#include "../common/json_value.h"

#define LLM_RETRY_IMPLEMENTATION
#include "../../third_party/llm-retry/llm_retry.hpp"

#include <winhttp.h>

#include <algorithm>
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

struct Assembler {
    std::string content, reasoning, finish;
    std::map<int64_t, ToolAcc> tools;
    int64_t prompt_tokens = 0, completion_tokens = 0;

    // One "data:" payload. False when it is not JSON.
    bool Feed(ModelStream::Shared& sh, std::string_view payload) {
        json::Value v;
        if (!json::Parse(payload, v) || !v.is_object()) return false;
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

// Closes the request unless Cancel already did.
void CloseRequest(ModelStream::Shared& sh) {
    std::lock_guard<std::mutex> lock(sh.mu);
    if (sh.request) WinHttpCloseHandle(sh.request);
    sh.request = nullptr;
}

// The link dropped before any answer. Timeouts are not retried: the receive
// timeout is long, so a retry would multiply the wait.
bool TransientNetworkError(DWORD error) {
    return error == ERROR_WINHTTP_CANNOT_CONNECT || error == ERROR_WINHTTP_CONNECTION_ERROR;
}

struct Cancelled {};

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
    const std::wstring path = url.path + L"/chat/completions";
    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\nAuthorization: Bearer ";
    headers += endpoint.key;
    headers += L"\r\n";
    SecureZeroMemory(endpoint.key.data(), endpoint.key.size() * sizeof(wchar_t));
    struct Wipe {
        std::wstring& text;
        ~Wipe() { SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t)); }
    } wipe{headers};
    struct RequestGuard {
        ModelStream::Shared& s;
        ~RequestGuard() { CloseRequest(s); }
    } request_guard{*sh};

    // A dropped link or a busy service (5xx) before the answer starts is tried
    // again, up to 3 attempts with backoff. Once the stream starts nothing is
    // retried, so no delta reaches the UI twice; 429 stays with the user.
    llm::RetryConfig retry;
    retry.retry_on_http_codes = {500, 502, 503, 504};
    HINTERNET request = nullptr;
    DWORD status = 0;
    DWORD network_error = 0;
    std::string error_body;
    try {
        status = llm::with_retry<DWORD>([&]() -> DWORD {
            if (sh->cancelled.load()) throw Cancelled{};
            HINTERNET r = WinHttpOpenRequest(connection.value, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0);
            if (!r) {
                network_error = GetLastError();
                throw llm::LLMError{0, {}, false};
            }
            {
                std::lock_guard<std::mutex> lock(sh->mu);
                if (sh->cancelled.load()) {
                    WinHttpCloseHandle(r);
                    throw Cancelled{};
                }
                sh->request = r;
            }
            if (!WinHttpSendRequest(r, headers.c_str(), static_cast<DWORD>(headers.size()), body.data(),
                                    static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
                !WinHttpReceiveResponse(r, nullptr)) {
                network_error = GetLastError();
                CloseRequest(*sh);
                throw llm::LLMError{0, {}, !sh->cancelled.load() && TransientNetworkError(network_error)};
            }
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            error_body.clear();
            const auto& codes = retry.retry_on_http_codes;
            if (std::find(codes.begin(), codes.end(), static_cast<int>(code)) != codes.end()) {
                // Kept for the failure if this was the last attempt.
                std::vector<char> chunk(16 * 1024);
                DWORD read = 0;
                while (WinHttpReadData(r, chunk.data(), static_cast<DWORD>(chunk.size()), &read) && read > 0)
                    if (error_body.size() < 64 * 1024) error_body.append(chunk.data(), read);
                CloseRequest(*sh);
                throw llm::LLMError{static_cast<int>(code), {}, true};
            }
            request = r;
            return code;
        }, retry).value;
    } catch (const Cancelled&) {
        return;
    } catch (const llm::LLMError& err) {
        if (sh->cancelled.load()) return;
        if (err.http_code == 0) {
            Push(*sh, Failure(0, network_error, "network", {}));
            return;
        }
        std::string type, message;
        ReadError(error_body, type, message);
        Push(*sh, Failure(err.http_code, 0, std::move(type), std::move(message)));
        return;
    }

    std::string pending;   // bytes after the last complete line
    Assembler acc;
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
            acc.Feed(*sh, line);
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

} // namespace pulse::app::ai
