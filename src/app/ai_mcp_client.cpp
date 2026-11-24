// ai_mcp_client.cpp - External MCP JSON-RPC 2.0 client (stdio & HTTP).
#include "ai_mcp_client.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace pulse::app::ai {
namespace {

std::string SanitizeSlug(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "tool";
    if (out.size() > 28) out.resize(28);
    return out;
}

std::wstring QuoteWinArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t bs = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++bs; ++i; }
        if (i == arg.size()) {
            out.append(bs * 2, L'\\');
            break;
        } else if (arg[i] == L'"') {
            out.append(bs * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(bs, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

json::Value MakeRpcRequest(int id, const char* method, json::Value params) {
    json::Value req = json::Value::Object();
    req.set("jsonrpc", "2.0");
    if (id > 0) req.set("id", id);
    req.set("method", method);
    if (!params.is_null()) req.set("params", std::move(params));
    return req;
}

json::Value MakeInitParams() {
    json::Value p = json::Value::Object();
    p.set("protocolVersion", "2024-11-05");
    p.set("capabilities", json::Value::Object());
    json::Value ci = json::Value::Object();
    ci.set("name", "Pulse");
    ci.set("version", "1.0");
    p.set("clientInfo", std::move(ci));
    return p;
}

struct StdioSession {
    HANDLE child_proc = nullptr;
    HANDLE child_thread = nullptr;
    HANDLE stdin_wr = nullptr;
    HANDLE stdout_rd = nullptr;
    std::string pending;

    ~StdioSession() { Close(); }

    void Close() {
        if (stdin_wr) { CloseHandle(stdin_wr); stdin_wr = nullptr; }
        if (stdout_rd) { CloseHandle(stdout_rd); stdout_rd = nullptr; }
        if (child_proc) {
            if (WaitForSingleObject(child_proc, 200) == WAIT_TIMEOUT) {
                TerminateProcess(child_proc, 1);
                WaitForSingleObject(child_proc, 500);
            }
            CloseHandle(child_proc);
            child_proc = nullptr;
        }
        if (child_thread) { CloseHandle(child_thread); child_thread = nullptr; }
    }

    bool Start(const ExternalMcpServer& srv, std::string& err) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE stdin_rd = nullptr;
        HANDLE stdout_wr = nullptr;
        if (!CreatePipe(&stdin_rd, &stdin_wr, &sa, 0) || !CreatePipe(&stdout_rd, &stdout_wr, &sa, 0)) {
            err = "Failed to create stdio pipes.";
            if (stdin_rd) CloseHandle(stdin_rd);
            if (stdout_wr) CloseHandle(stdout_wr);
            Close();
            return false;
        }
        SetHandleInformation(stdin_wr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdout_rd, HANDLE_FLAG_INHERIT, 0);

        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);

        std::wstring cmd = QuoteWinArg(json::Wide(srv.command));
        for (const std::string& a : srv.args) {
            cmd.push_back(L' ');
            cmd += QuoteWinArg(json::Wide(a));
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdin_rd;
        si.hStdOutput = stdout_wr;
        si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : stdout_wr;

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si, &pi);
        CloseHandle(stdin_rd);
        CloseHandle(stdout_wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            err = "Could not launch MCP server process (Win32 " + std::to_string(GetLastError()) + ").";
            Close();
            return false;
        }
        child_proc = pi.hProcess;
        child_thread = pi.hThread;
        return true;
    }

    bool WriteLine(const json::Value& msg) {
        if (!stdin_wr) return false;
        std::string line = json::Serialize(msg) + "\n";
        size_t sent = 0;
        while (sent < line.size()) {
            DWORD w = 0;
            if (!WriteFile(stdin_wr, line.data() + sent, static_cast<DWORD>(line.size() - sent), &w, nullptr) || w == 0)
                return false;
            sent += w;
        }
        return true;
    }

    bool ReadResponseForId(int expected_id, uint32_t timeout_ms, const std::atomic<bool>& cancelled,
                           json::Value& out, std::string& err) {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        char buf[4096];
        for (;;) {
            // Check already buffered lines first.
            size_t nl = std::string::npos;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                json::Value parsed;
                if (!json::Parse(line, parsed) || !parsed.is_object()) continue;
                int64_t id_val = -1;
                if (const json::Value* idv = parsed.find("id"); idv && idv->get_int(id_val) && id_val == expected_id) {
                    out = std::move(parsed);
                    return true;
                }
            }
            if (cancelled.load()) {
                err = "Cancelled.";
                return false;
            }
            if (GetTickCount64() >= deadline) {
                err = "Timed out waiting for MCP server response.";
                return false;
            }
            DWORD avail = 0;
            if (!PeekNamedPipe(stdout_rd, nullptr, 0, nullptr, &avail, nullptr)) {
                err = "MCP server closed stdout.";
                return false;
            }
            if (avail == 0) {
                if (WaitForSingleObject(child_proc, 0) == WAIT_OBJECT_0) {
                    err = "MCP server exited before responding.";
                    return false;
                }
                Sleep(10);
                continue;
            }
            DWORD got = 0;
            const DWORD want = (std::min<DWORD>)(avail, static_cast<DWORD>(sizeof(buf)));
            if (!ReadFile(stdout_rd, buf, want, &got, nullptr) || got == 0) {
                err = "MCP server pipe read failed.";
                return false;
            }
            if (pending.size() + got > 4 * 1024 * 1024) {
                err = "MCP response exceeded 4 MB limit.";
                return false;
            }
            pending.append(buf, got);
        }
    }
};

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
};

bool HttpJsonRpc(const std::string& url_utf8, const json::Value& req, uint32_t timeout_ms,
                 const std::atomic<bool>& cancelled, json::Value& out, std::string& err) {
    if (cancelled.load()) { err = "Cancelled."; return false; }
    const std::wstring wurl = json::Wide(url_utf8);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts)) {
        err = "Invalid MCP HTTP URL.";
        return false;
    }
    WinHttpHandle ses(WinHttpOpen(L"Pulse-MCP", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h) { err = "WinHttpOpen failed."; return false; }
    const int t = static_cast<int>(std::clamp<uint32_t>(timeout_ms, 2000, 60000));
    WinHttpSetTimeouts(ses.h, t, t, t, t);
    WinHttpHandle conn(WinHttpConnect(ses.h, host, parts.nPort, 0));
    if (!conn.h) { err = "WinHttpConnect failed."; return false; }
    const std::wstring req_path = parts.dwUrlPathLength ? std::wstring(path, parts.dwUrlPathLength) : L"/";
    WinHttpHandle rq(WinHttpOpenRequest(conn.h, L"POST", req_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!rq.h) { err = "WinHttpOpenRequest failed."; return false; }

    std::string body = json::Serialize(req);
    const wchar_t* hdrs = L"Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n";
    if (!WinHttpSendRequest(rq.h, hdrs, static_cast<DWORD>(wcslen(hdrs)), body.data(), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(rq.h, nullptr)) {
        err = "MCP HTTP request failed (Win32 " + std::to_string(GetLastError()) + ").";
        return false;
    }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(rq.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    std::string resp;
    char buf[8192];
    for (;;) {
        if (cancelled.load()) { err = "Cancelled."; return false; }
        DWORD got = 0;
        if (!WinHttpReadData(rq.h, buf, sizeof(buf), &got) || got == 0) break;
        if (resp.size() + got > 4 * 1024 * 1024) break;
        resp.append(buf, got);
    }
    if (status < 200 || status >= 300) {
        err = "MCP HTTP returned status " + std::to_string(status);
        return false;
    }
    // Accept plain JSON or single-event SSE (`data: {...}`).
    if (json::Parse(resp, out) && out.is_object()) return true;
    size_t pos = 0;
    while (pos < resp.size()) {
        const size_t nl = resp.find('\n', pos);
        std::string_view line = nl == std::string::npos ? std::string_view(resp).substr(pos)
                                                        : std::string_view(resp).substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.rfind("data:", 0) == 0) {
            line.remove_prefix(5);
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            if (json::Parse(line, out) && out.is_object()) return true;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    err = "MCP HTTP response was not valid JSON-RPC.";
    return false;
}

bool RunMcpRpc(const ExternalMcpServer& srv, const char* method, json::Value params, uint32_t timeout_ms,
               const std::atomic<bool>& cancelled, json::Value& out_result, std::string& err) {
    json::Value rpc_resp;
    if (srv.transport == "http") {
        // Initialize (best effort) then invoke method.
        json::Value init_resp;
        std::string init_err;
        (void)HttpJsonRpc(srv.url, MakeRpcRequest(1, "initialize", MakeInitParams()), timeout_ms, cancelled, init_resp,
                          init_err);
        if (!HttpJsonRpc(srv.url, MakeRpcRequest(2, method, std::move(params)), timeout_ms, cancelled, rpc_resp, err))
            return false;
    } else {
        StdioSession ses;
        if (!ses.Start(srv, err)) return false;
        if (!ses.WriteLine(MakeRpcRequest(1, "initialize", MakeInitParams()))) {
            err = "Failed to send MCP initialize.";
            return false;
        }
        json::Value init_resp;
        if (!ses.ReadResponseForId(1, timeout_ms, cancelled, init_resp, err)) return false;
        (void)ses.WriteLine(MakeRpcRequest(0, "notifications/initialized", json::Value::Object()));
        if (!ses.WriteLine(MakeRpcRequest(2, method, std::move(params)))) {
            err = "Failed to send MCP request.";
            return false;
        }
        if (!ses.ReadResponseForId(2, timeout_ms, cancelled, rpc_resp, err)) return false;
    }

    if (const json::Value* e = rpc_resp.find("error"); e && e->is_object()) {
        err = e->get_string("message", "MCP JSON-RPC error");
        return false;
    }
    if (const json::Value* res = rpc_resp.find("result")) {
        out_result = *res;
        return true;
    }
    err = "MCP response missing result.";
    return false;
}

} // namespace

std::string MakeMcpToolId(std::string_view server_name, std::string_view tool_name) {
    return "mcp__" + SanitizeSlug(server_name) + "__" + SanitizeSlug(tool_name);
}

McpListResult ListMcpServerTools(const ExternalMcpServer& server, uint32_t timeout_ms,
                                 const std::atomic<bool>& cancelled) {
    McpListResult out;
    json::Value res;
    if (!RunMcpRpc(server, "tools/list", json::Value::Object(), timeout_ms, cancelled, res, out.error_message))
        return out;
    const json::Value* tools = res.find("tools");
    if (!tools || !tools->is_array()) {
        out.ok = true;
        return out;
    }
    for (const json::Value& t : tools->items()) {
        if (!t.is_object() || out.tools.size() >= 32) continue;
        const std::string name = t.get_string("name");
        if (name.empty()) continue;
        McpToolDef def;
        def.server_name = server.name;
        def.tool_name = name;
        def.full_id = MakeMcpToolId(server.name, name);
        def.description = t.get_string("description");
        if (const json::Value* sc = t.find("inputSchema"); sc && sc->is_object()) {
            def.input_schema = *sc;
        } else {
            def.input_schema = json::Value::Object();
            def.input_schema.set("type", "object");
            def.input_schema.set("properties", json::Value::Object());
        }
        out.tools.push_back(std::move(def));
    }
    out.ok = true;
    return out;
}

McpCallResult CallMcpServerTool(const ExternalMcpServer& server, const std::string& tool_name,
                                const json::Value& arguments, uint32_t timeout_ms,
                                const std::atomic<bool>& cancelled) {
    McpCallResult out;
    json::Value params = json::Value::Object();
    params.set("name", tool_name);
    params.set("arguments", arguments.is_object() ? arguments : json::Value::Object());
    json::Value res;
    if (!RunMcpRpc(server, "tools/call", std::move(params), timeout_ms, cancelled, res, out.error_message))
        return out;
    out.ok = true;
    out.raw = res;
    out.is_error = res.get_bool("isError", false);
    if (const json::Value* cnt = res.find("content"); cnt && cnt->is_array()) {
        for (const json::Value& item : cnt->items()) {
            if (!item.is_object()) continue;
            if (item.get_string("type") == "text") {
                if (!out.text.empty()) out.text += "\n";
                out.text += item.get_string("text");
            }
        }
    }
    if (out.text.empty()) out.text = json::Serialize(res);
    return out;
}

} // namespace pulse::app::ai
