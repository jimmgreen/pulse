// ai_mcp_test - Pulse.AI.exe --mcp and pulse.exe --mcp-stdio, driven over
// real pipes the way an MCP client drives them.
//
//   * Pulse.AI.exe against a ToolBroker in this process (which is then the
//     "pulse.exe" the host must find on the pipe): legacy initialize, modern
//     server/discover and _meta, tools/list, tools/call with argument
//     checks, progress while Pulse asks, cancellation without an answer,
//     out-of-order answers, the call limit, JSON-RPC errors.
//   * Refusals: no Pulse, a pipe served by another program, a peer the broker
//     rejects (pack_outdated), a client the user denied.
//   * The launcher: the bundled host of this build directory, a copy of
//     pulse.exe without a pack (exit 3), and the same copy with the real
//     Pulse.AI.exe installed through InstallPack (Ready: held and started);
//     killing the launcher kills the host (job object).
// LOCALAPPDATA points into a scratch directory; the real packs are untouched.
#include "../app/ai_broker.h"
#include "../app/extensions/developer_tools.h"
#include "../app/ai_launcher.h"
#include "../app/pack_installer.h"
#include "../common/json_value.h"
#include "../common/preview_packs.h"
#include "../ipc/ai_tool_protocol.h"
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::app;
using namespace pulse::app::ai;
namespace json = pulse::json;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_passed : ++g_failed;
}

std::wstring ModulePath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}
std::wstring ModuleDirectory() {
    const std::wstring path = ModulePath();
    return path.substr(0, path.find_last_of(L'\\'));
}

std::vector<uint8_t> ReadAll(const std::wstring& p) {
    std::vector<uint8_t> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") || !f) return out;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

DWORD Run(std::wstring command) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return 0xFFFFFFFF;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0xFFFFFFFF;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// A child on three pipes, like an MCP client's server process.
class Child {
public:
    bool Start(std::wstring command) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE in_r = nullptr, out_w = nullptr, err_w = nullptr;
        if (!CreatePipe(&in_r, &in_w_, &sa, 0) || !CreatePipe(&out_r_, &out_w, &sa, 0) || !CreatePipe(&err_r_, &err_w, &sa, 0))
            return false;
        SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_r_, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = err_w;
        PROCESS_INFORMATION pi{};
        const bool ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si, &pi) != FALSE;
        CloseHandle(in_r);
        CloseHandle(out_w);
        CloseHandle(err_w);
        if (!ok) return false;
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;
        return true;
    }
    ~Child() {
        CloseInput();
        if (process_) {
            if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0) TerminateProcess(process_, 1);
            CloseHandle(process_);
        }
        if (out_r_) CloseHandle(out_r_);
        if (err_r_) CloseHandle(err_r_);
    }
    DWORD pid() const { return pid_; }
    HANDLE process() const { return process_; }

    bool SendLine(const std::string& line) {
        const std::string data = line + "\n";
        DWORD written = 0;
        return WriteFile(in_w_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    }
    bool Send(const json::Value& m) { return SendLine(json::Serialize(m)); }
    void CloseInput() {
        if (in_w_) CloseHandle(in_w_);
        in_w_ = nullptr;
    }

    // The next message on stdout within `timeout_ms`.
    bool Next(json::Value& m, DWORD timeout_ms = 8000) {
        const ULONGLONG end = GetTickCount64() + timeout_ms;
        for (;;) {
            const size_t newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                const std::string line = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1);
                lines_.push_back(line);
                return json::Parse(line, m);
            }
            DWORD available = 0;
            if (!PeekNamedPipe(out_r_, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available) {
                std::string chunk(available, '\0');
                DWORD n = 0;
                if (!ReadFile(out_r_, chunk.data(), available, &n, nullptr)) return false;
                buffer_.append(chunk.data(), n);
                continue;
            }
            if (GetTickCount64() >= end) return false;
            Sleep(5);
        }
    }
    // The response with this id; notifications seen meanwhile go to `notes`.
    bool Response(const json::Value& id, json::Value& m, std::vector<json::Value>* notes = nullptr, DWORD timeout_ms = 8000) {
        const std::string want = json::Serialize(id);
        const ULONGLONG end = GetTickCount64() + timeout_ms;
        while (GetTickCount64() < end) {
            json::Value next;
            if (!Next(next, static_cast<DWORD>(end - GetTickCount64()))) return false;
            const json::Value* got = next.find("id");
            if (got && json::Serialize(*got) == want && !next.find("method")) {
                m = std::move(next);
                return true;
            }
            if (notes) notes->push_back(std::move(next));
            else others_.push_back(std::move(next));
        }
        return false;
    }
    const std::vector<json::Value>& others() const { return others_; }
    std::string ReadErr() {
        std::string out;
        DWORD available = 0;
        while (PeekNamedPipe(err_r_, nullptr, 0, nullptr, &available, nullptr) && available) {
            std::string chunk(available, '\0');
            DWORD n = 0;
            if (!ReadFile(err_r_, chunk.data(), available, &n, nullptr)) break;
            out.append(chunk.data(), n);
        }
        return out;
    }
    DWORD WaitExit(DWORD timeout_ms) {
        if (WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) return 0xFFFFFFFF;
        DWORD code = 0xFFFFFFFF;
        GetExitCodeProcess(process_, &code);
        return code;
    }

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HANDLE in_w_ = nullptr, out_r_ = nullptr, err_r_ = nullptr;
    std::string buffer_;
    std::vector<std::string> lines_;
    std::vector<json::Value> others_;
};

json::Value Request(const json::Value& id, const char* method, json::Value params = json::Value::Object()) {
    json::Value m = json::Value::Object();
    m.set("jsonrpc", "2.0");
    m.set("id", id);
    m.set("method", method);
    m.set("params", std::move(params));
    return m;
}

json::Value Notification(const char* method, json::Value params = json::Value::Object()) {
    json::Value m = json::Value::Object();
    m.set("jsonrpc", "2.0");
    m.set("method", method);
    m.set("params", std::move(params));
    return m;
}

json::Value CallParams(const char* tool, json::Value args = json::Value::Object()) {
    json::Value p = json::Value::Object();
    p.set("name", tool);
    p.set("arguments", std::move(args));
    return p;
}

json::Value Args(std::initializer_list<std::pair<const char*, json::Value>> members) {
    json::Value a = json::Value::Object();
    for (const auto& [k, v] : members) a.set(k, v);
    return a;
}

json::Value Strings(std::initializer_list<const char*> items) {
    json::Value a = json::Value::Array();
    for (const char* s : items) a.push(s);
    return a;
}

json::Value ModernMeta(const char* version = "2026-07-28") {
    json::Value info = json::Value::Object();
    info.set("name", "modern-client");
    info.set("version", "2.0");
    json::Value meta = json::Value::Object();
    meta.set("io.modelcontextprotocol/protocolVersion", version);
    meta.set("io.modelcontextprotocol/clientInfo", std::move(info));
    meta.set("io.modelcontextprotocol/clientCapabilities", json::Value::Object());
    return meta;
}

// The text of a tools/call result, and whether it is an error.
std::string ToolText(const json::Value& response, bool* is_error = nullptr) {
    const json::Value* result = response.find("result");
    if (!result) return {};
    if (is_error) *is_error = result->get_bool("isError");
    const json::Value* content = result->find("content");
    if (!content || !content->is_array() || !content->size()) return {};
    return content->at(0).get_string("text");
}
bool ToolErrorIs(const json::Value& response, const std::string& code) {
    bool error = false;
    const std::string text = ToolText(response, &error);
    const bool match = error && text.rfind(code + ":", 0) == 0;
    if (!match) std::printf("  expected %s, got: %s\n", code.c_str(), json::Serialize(response).substr(0, 300).c_str());
    return match;
}
int ErrorCode(const json::Value& response) {
    const json::Value* error = response.find("error");
    int64_t code = 0;
    return error && error->find("code") && error->find("code")->get_int(code) ? static_cast<int>(code) : 0;
}

void WaitCancel(const ToolCall& call, DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (!call.cancelled && GetTickCount64() < end) Sleep(10);
}

std::vector<DWORD> ChildrenOf(DWORD parent, const wchar_t* exe) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ParentProcessID == parent && _wcsicmp(e.szExeFile, exe) == 0) out.push_back(e.th32ProcessID);
    CloseHandle(snap);
    return out;
}

std::wstring ImageOf(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = MAX_PATH * 2;
    std::wstring out = QueryFullProcessImageNameW(p, 0, path, &size) ? std::wstring(path, size) : std::wstring{};
    CloseHandle(p);
    return out;
}

bool ProcessGone(DWORD pid, DWORD timeout_ms) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return true;
    const bool gone = WaitForSingleObject(p, timeout_ms) == WAIT_OBJECT_0;
    CloseHandle(p);
    return gone;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

int ExtensionToolsOnly() {
    const std::wstring pipe = L"\\\\.\\pipe\\Pulse.Tools.extensions." + std::to_wstring(GetCurrentProcessId());
    std::atomic<DWORD> host_pid{0};
    BrokerOptions options;
    options.pipe_name = pipe;
    options.verify_peer = [&](HANDLE p) {
        ULONG pid = 0;
        return GetNamedPipeClientProcessId(p, &pid) && pid == host_pid.load();
    };
    ToolBroker broker(options);
    broker.Register({"pulse.describe_extension_api", PermissionClass::Read, extensions::DescribeExtensionApiTool});
    broker.Register({"pulse.validate_extension_manifest", PermissionClass::Read, extensions::ValidateExtensionManifestTool});
    broker.Register({"pulse.get_extension_template", PermissionClass::Read, extensions::GetExtensionTemplateTool});
    DWORD error = 0;
    if (!broker.Start(error)) { Check(false, "extension broker starts"); return 1; }
    {
        Child c;
        const auto host = ModuleDirectory() + L"\\Pulse.AI.exe";
        if (!c.Start(L"\"" + host + L"\" --mcp --server \"" + ModulePath() + L"\" --pipe " + pipe)) {
            Check(false, "MCP host starts"); return 1;
        }
        host_pid = c.pid();
        json::Value init = json::Value::Object(), r;
        init.set("protocolVersion", "2025-06-18");
        init.set("capabilities", json::Value::Object());
        init.set("clientInfo", Args({{"name", "extension-developer-test"}, {"version", "1"}}));
        c.Send(Request(1, "initialize", init));
        Check(c.Response(1, r) && r.find("result"), "extension MCP handshake");
        c.Send(Notification("notifications/initialized"));
        c.Send(Request(2, "tools/list"));
        bool listed = c.Response(2, r) && r.find("result") && r.find("result")->find("tools");
        const auto* list = listed ? r.find("result")->find("tools") : nullptr;
        Check(list && list->size() == 12 && list->at(9).get_string("name") == "describe_extension_api" &&
              list->at(10).get_string("name") == "validate_extension_manifest" &&
              list->at(11).get_string("name") == "get_extension_template", "extension tools discoverable over MCP");
        c.Send(Request(3, "tools/call", CallParams("describe_extension_api")));
        bool failed = true;
        const bool described = c.Response(3, r) && !ToolText(r, &failed).empty() && !failed;
        const auto* data = described ? r.find("result")->find("structuredContent") : nullptr;
        Check(data && data->find("manifestSchema") && data->find("capabilities") &&
              data->find("capabilities")->size() == 8 && data->find("workspaceLayouts") &&
              data->find("workspaceLayouts")->size() == 5 && data->find("viewModes") &&
              data->find("viewModes")->size() == 8, "production contract returned through MCP broker");
        const std::atomic<bool> describe_cancelled{false};
        const auto shared_description = extensions::DescribeExtensionApiTool(
            {L"extension discovery parity test", json::Value::Object(), describe_cancelled});
        Check(data && shared_description.ok && data->find("capabilityDetails") && data->find("bindingDetails") &&
              json::Serialize(*data) == json::Serialize(shared_description.data),
              "real MCP discovery equals the complete shared built-in assistant adapter result");
        const std::string manifest = R"({"id":"test","name":"Test","apiVersion":1,"commands":[]})";
        c.Send(Request(4, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", manifest}}))));
        const bool validated = c.Response(4, r) && !ToolText(r, &failed).empty() && !failed;
        data = validated ? r.find("result")->find("structuredContent") : nullptr;
        Check(data && data->get_bool("valid") && data->get_string("error").empty(), "valid manifest accepted through production adapter");
        c.Send(Request(5, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", "{"}}))));
        const bool invalid = c.Response(5, r) && !ToolText(r, &failed).empty() && !failed;
        data = invalid ? r.find("result")->find("structuredContent") : nullptr;
        Check(data && !data->get_bool("valid", true) && !data->get_string("error").empty(), "invalid manifest is a validation result, not transport failure");
        c.Send(Request(6, "tools/call", CallParams("validate_extension_manifest", Args({{"path", "C:\\missing.json"}}))));
        Check(c.Response(6, r) && ToolErrorIs(r, "invalid_args"), "file paths are not accepted as manifest input");
        c.Send(Request(7, "tools/call", CallParams("describe_extension_api", Args({{"extra", true}}))));
        Check(c.Response(7, r) && ToolErrorIs(r, "invalid_args"), "discovery rejects unknown arguments");
        c.Send(Request(8, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", 42}}))));
        Check(c.Response(8, r) && ToolErrorIs(r, "invalid_args"), "validation rejects non-string input");
        std::string boundary = manifest;
        boundary.resize(65536, ' ');
        c.Send(Request(9, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", boundary}}))));
        const bool boundary_ok = c.Response(9, r) && !ToolText(r, &failed).empty() && !failed;
        data = boundary_ok ? r.find("result")->find("structuredContent") : nullptr;
        Check(data && data->get_bool("valid"), "64 KiB manifest accepted over real MCP transport");
        std::string multibyte;
        for (int i = 0; i < 22000; ++i) multibyte += "\xe4\xb8\xad";
        c.Send(Request(10, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", multibyte}}))));
        Check(c.Response(10, r) && ToolErrorIs(r, "invalid_args"), "MCP enforces UTF-8 byte limit for multibyte text");
        const std::string view_manifest = R"({"apiVersion":1,"id":"view","name":"View","commands":[{"id":"details","title":"Details","capability":"view.setMode","args":{"mode":"details"}}]})";
        c.Send(Request(11, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", view_manifest}}))));
        const bool view_ok = c.Response(11, r) && !ToolText(r, &failed).empty() && !failed;
        data = view_ok ? r.find("result")->find("structuredContent") : nullptr;
        Check(data && data->get_bool("valid"), "typed view command accepted over MCP");
        auto invalid_view = view_manifest;
        invalid_view.replace(invalid_view.find("\"mode\":\"details\""), 16, "\"mode\":\"bogus\"");
        c.Send(Request(12, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", invalid_view}}))));
        const bool view_invalid = c.Response(12, r) && !ToolText(r, &failed).empty() && !failed;
        data = view_invalid ? r.find("result")->find("structuredContent") : nullptr;
        Check(data && !data->get_bool("valid", true), "unknown view mode rejected over MCP");
        c.Send(Request(20, "tools/call", CallParams("get_extension_template", Args({{"template", "transfer-panel"}}))));
        const bool template_ok = c.Response(20, r) && !ToolText(r, &failed).empty() && !failed;
        data = template_ok ? r.find("result")->find("structuredContent") : nullptr;
        const auto template_text = data ? data->get_string("manifest") : std::string{};
        Check(data && data->get_bool("validated") && !data->get_bool("executed", true) && !template_text.empty(),
            "real MCP returns a validated template without execution");
        const auto shared_template = extensions::GetExtensionTemplateTool(
            {L"template parity test", Args({{"template", "transfer-panel"}}), describe_cancelled});
        Check(data && shared_template.ok && json::Serialize(*data) == json::Serialize(shared_template.data),
            "MCP template equals shared assistant adapter output");
        c.Send(Request(21, "tools/call", CallParams("validate_extension_manifest", Args({{"manifest", template_text}}))));
        Check(c.Response(21, r) && r.find("result") && r.find("result")->find("structuredContent") &&
            r.find("result")->find("structuredContent")->get_bool("valid"), "retrieved template completes MCP validate round trip");
        c.Send(Request(22, "tools/call", CallParams("get_extension_template", Args({{"template", "../outside.json"}}))));
        Check(c.Response(22, r) && ToolErrorIs(r, "invalid_args"), "MCP rejects template path traversal at its schema boundary");
        c.CloseInput();
        Check(c.WaitExit(5000) == 0, "MCP host exits cleanly");
    }
    broker.Stop();
    std::printf("extension_mcp_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef PULSE_EXTENSION_MCP_ONLY
    if (argc == 1) return ExtensionToolsOnly();
#endif
    if (argc == 2 && std::string(argv[1]) == "--extensions-only") return ExtensionToolsOnly();
    const std::wstring build = ModuleDirectory();
    const std::wstring host = build + L"\\Pulse.AI.exe";
    const std::wstring self = ModulePath();
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"pulse_ai_mcp_test_" + std::to_wstring(GetCurrentProcessId());
    packs::CreateDirectoryChain(root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root + L"\\local0").c_str());

    // --- command line.
    Check(Run(L"\"" + host + L"\" --mcp") == ERROR_INVALID_PARAMETER, "--mcp without --server is refused");
    Check(Run(L"\"" + host + L"\" --mcp --server \"" + self + L"\" --pipe \\\\.\\pipe\\Other") == ERROR_INVALID_PARAMETER,
          "a pipe outside Pulse.Tools.* is refused");
    Check(Run(L"\"" + host + L"\" --version") == 0, "--version still works");

    // --- a broker in this process; Pulse.AI.exe must find this executable on it.
    const std::wstring pipe = L"\\\\.\\pipe\\Pulse.Tools.test." + std::to_wstring(GetCurrentProcessId());
    std::atomic<DWORD> host_pid{0};
    std::atomic<bool> search_cancelled{false};
    std::mutex names_mu;
    std::vector<std::wstring> client_names;
    BrokerOptions options;
    options.pipe_name = pipe;
    options.verify_peer = [&](HANDLE p) {
        ULONG pid = 0;
        return GetNamedPipeClientProcessId(p, &pid) && pid == host_pid.load();
    };
    options.approve_client = [&](const std::wstring& name) {
        std::lock_guard lock(names_mu);
        client_names.push_back(name);
        return name != L"blocked-client";
    };
    options.confirm = [](const ConfirmRequest&, const std::atomic<bool>&, DWORD) {
        Sleep(300);
        return Decision::Allow;
    };
    options.always_allowed = [](const std::wstring&, const std::string&, const std::wstring&) { return false; };
    ToolBroker broker(options);
    broker.Register({"pulse.list_folder", PermissionClass::Read, [](const ToolCall& call) {
        ToolOutcome o;
        o.data = json::Value::Object();
        o.data.set("echo_path", call.args.get_string("path"));
        o.data.set("items", json::Value::Array());
        return o;
    }});
    broker.Register({"pulse.search", PermissionClass::Read, [&](const ToolCall& call) {
        WaitCancel(call, 10000);
        if (call.cancelled) search_cancelled = true;
        return ToolOutcome{};
    }});
    broker.Register({"pulse.get_properties", PermissionClass::Read, [](const ToolCall&) -> ToolOutcome { throw 1; }});
    ToolSpec zip{"pulse.create_zip", PermissionClass::Create, [](const ToolCall&) {
        ToolOutcome o;
        o.data = json::Value::Object();
        o.data.set("path", "C:\\Data\\a.zip");
        return o;
    }};
    zip.target_folder = [](const json::Value&) { return std::wstring(L"C:\\Data"); };
    broker.Register(zip);
    DWORD error = 0;
    Check(broker.Start(error), "test broker starts");

    {
        Child c;
        Check(c.Start(L"\"" + host + L"\" --mcp --server \"" + self + L"\" --pipe " + pipe), "Pulse.AI.exe --mcp starts on pipes");
        host_pid = c.pid();
        json::Value r;

        // Legacy handshake.
        json::Value init = json::Value::Object();
        init.set("protocolVersion", "2025-06-18");
        init.set("capabilities", json::Value::Object());
        init.set("clientInfo", Args({{"name", "test-client"}, {"version", "1.0"}}));
        c.Send(Request(1, "initialize", init));
        Check(c.Response(1, r) && r.find("result") && r.find("result")->get_string("protocolVersion") == "2025-06-18" &&
                  r.find("result")->find("serverInfo")->get_string("name") == "pulse" &&
                  r.find("result")->find("capabilities")->find("tools") && !r.find("result")->find("resultType"),
              "initialize (2025-06-18): that version, serverInfo pulse, tools capability, legacy shape");
        init.set("protocolVersion", "1999-01-01");
        c.Send(Request(2, "initialize", init));
        Check(c.Response(2, r) && r.find("result")->get_string("protocolVersion") == "2025-11-25",
              "initialize with an unknown version gets the newest legacy one");
        c.Send(Notification("notifications/initialized"));
        c.Send(Request("p1", "ping"));
        Check(c.Response("p1", r) && r.find("result") && r.find("result")->is_object() && c.others().empty(),
              "notifications get no answer; ping with a string id");

        // tools/list.
        c.Send(Request(3, "tools/list"));
        bool shape = c.Response(3, r) && r.find("result") && r.find("result")->find("tools");
        const json::Value* tools = shape ? r.find("result")->find("tools") : nullptr;
        shape = shape && tools->size() == 12 && tools->at(0).get_string("name") == "get_context";
        std::string names;
        for (size_t i = 0; shape && i < tools->size(); ++i) {
            const json::Value& t = tools->at(i);
            const std::string name = t.get_string("name");
            names += name + " ";
            for (char ch : name) shape &= (ch >= 'a' && ch <= 'z') || ch == '_';
            const json::Value* schema = t.find("inputSchema");
            shape &= schema && schema->get_string("type") == "object" && schema->find("additionalProperties") &&
                     !schema->find("additionalProperties")->as_bool(true) && !t.get_string("description").empty() &&
                     t.find("annotations") != nullptr;
        }
        Check(shape && names == "get_context list_folder search get_properties reveal create_zip find_actions run_actions "
                                "undo_last describe_extension_api validate_extension_manifest get_extension_template ",
              "tools/list: the 12 tools in a fixed order, plain names, strict "
              "object schemas");
        bool zip_hints = false;
        for (size_t i = 0; tools && i < tools->size(); ++i)
            if (tools->at(i).get_string("name") == "create_zip")
                zip_hints = !tools->at(i).find("annotations")->get_bool("readOnlyHint", true) &&
                            !tools->at(i).find("annotations")->get_bool("openWorldHint", true);
        Check(zip_hints, "create_zip is marked as not read-only");

        // tools/call through the broker.
        c.Send(Request(4, "tools/call", CallParams("list_folder", Args({{"path", "C:\\Data"}}))));
        bool is_error = true;
        const bool listed = c.Response(4, r) && !ToolText(r, &is_error).empty() && !is_error &&
                            r.find("result")->find("structuredContent") &&
                            r.find("result")->find("structuredContent")->get_string("echo_path") == "C:\\Data";
        Check(listed, "list_folder reaches the broker; structuredContent and text carry its data");
        {
            std::lock_guard lock(names_mu);
            Check(!client_names.empty() && client_names.front() == L"test-client",
                  "hello carries the MCP client's name from initialize");
        }
        c.Send(Request(5, "tools/call", CallParams("list_folder")));
        Check(c.Response(5, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("\"path\"") != std::string::npos,
              "a missing required argument: invalid_args naming it (a tool error, not a protocol error)");
        c.Send(Request(6, "tools/call", CallParams("list_folder", Args({{"path", "C:\\x"}, {"extra", 1}}))));
        Check(c.Response(6, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("extra") != std::string::npos,
              "unknown arguments are refused");
        c.Send(Request(7, "tools/call", CallParams("search", Args({{"query", "a"}, {"type", "bogus"}}))));
        Check(c.Response(7, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("one of") != std::string::npos,
              "an enum argument outside its values is refused");
        c.Send(Request(8, "tools/call", CallParams("search", Args({{"query", "a"}, {"modified_after", "2026-13-01"}}))));
        Check(c.Response(8, r) && ToolErrorIs(r, "invalid_args"), "a malformed date is refused");
        c.Send(Request(9, "tools/call", CallParams("list_folder", Args({{"path", "C:\\x"}, {"limit", 501}}))));
        Check(c.Response(9, r) && ToolErrorIs(r, "invalid_args"), "an integer above its maximum is refused");
        c.Send(Request(10, "tools/call", CallParams("get_properties", Args({{"paths", json::Value::Array()}}))));
        Check(c.Response(10, r) && ToolErrorIs(r, "invalid_args"), "an empty paths array is refused");
        c.Send(Request(11, "tools/call", CallParams("reveal", Args({{"paths", Strings({"C:\\a.txt"})}}))));
        Check(c.Response(11, r) && ToolErrorIs(r, "unsupported"), "a tool Pulse does not run yet: unsupported from the broker");
        c.Send(Request(12, "tools/call", CallParams("get_properties", Args({{"paths", Strings({"C:\\a.txt"})}}))));
        Check(c.Response(12, r) && ToolErrorIs(r, "internal"), "a failing tool: internal");
        c.Send(Request(13, "tools/call", CallParams("no_such_tool")));
        Check(c.Response(13, r) && ErrorCode(r) == -32602, "an unknown tool is a protocol error (-32602)");
        json::Value bad_args = json::Value::Object();
        bad_args.set("name", "list_folder");
        bad_args.set("arguments", "C:\\x");
        c.Send(Request(14, "tools/call", bad_args));
        Check(c.Response(14, r) && ErrorCode(r) == -32602, "arguments that are not an object: -32602");

        // run_actions (P5): Pulse.AI.exe checks the steps' shape before the broker sees them.
        auto step = [](const char* action) {
            json::Value v = json::Value::Object();
            v.set("action", action);
            return v;
        };
        auto steps_args = [](json::Value steps) {
            json::Value a = json::Value::Object();
            a.set("steps", std::move(steps));
            return a;
        };
        c.Send(Request(16, "tools/call", CallParams("run_actions", steps_args(json::Value::Array()))));
        Check(c.Response(16, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("empty") != std::string::npos,
              "run_actions: an empty steps array is refused");
        {
            json::Value steps = json::Value::Array();
            json::Value no_action = json::Value::Object();
            no_action.set("args", json::Value::Object());
            steps.push(std::move(no_action));
            c.Send(Request(17, "tools/call", CallParams("run_actions", steps_args(std::move(steps)))));
        }
        Check(c.Response(17, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("steps[0].action") != std::string::npos &&
              ToolText(r).find("action identifier of 1..64 bytes") != std::string::npos,
              "run_actions: a step without action is refused, naming it");
        {
            json::Value steps = json::Value::Array();
            json::Value odd = step("view.details");
            odd.set("path", "C:\\x");
            steps.push(std::move(odd));
            c.Send(Request(18, "tools/call", CallParams("run_actions", steps_args(std::move(steps)))));
        }
        Check(c.Response(18, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("steps[0]: unknown member path") != std::string::npos,
              "run_actions: a stray member in a step is refused, naming its path");
        // Keep the shared action envelope's authority boundary covered over real MCP pipes.
        auto invalid_step = [&](int id, json::Value value, const char* reason) {
            json::Value steps = json::Value::Array();
            steps.push(std::move(value));
            c.Send(Request(id, "tools/call", CallParams("run_actions", steps_args(std::move(steps)))));
            const bool rejected = c.Response(id, r) && ToolErrorIs(r, "invalid_args") &&
                                  ToolText(r).find(reason) != std::string::npos;
            if (!rejected) std::printf("  action envelope response: %s\n", json::Serialize(r).c_str());
            return rejected;
        };
        bool invalid_ids = true;
        int boundary_id = 100;
        for (const json::Value& action : {json::Value(nullptr), json::Value(""), json::Value(std::string(65, 'x'))}) {
            json::Value value = json::Value::Object();
            value.set("action", action);
            invalid_ids = invalid_step(boundary_id++, std::move(value), "steps[0].action") && invalid_ids;
        }
        Check(invalid_ids, "run_actions: null, empty and oversized action identifiers are refused at their path");
        {
            json::Value value = step("nav.back");
            value.set("args", "not an object");
            Check(invalid_step(boundary_id++, std::move(value), "steps[0].args must be an object"),
                  "run_actions: non-object step args are refused at their path");
        }
        bool authority_rejected = true;
        for (const char* claim : {"principal", "caller", "owner", "package", "ticket", "capability", "client"}) {
            json::Value value = step("nav.back");
            value.set("args", Args({{claim, "claimed-authority"}}));
            authority_rejected = invalid_step(boundary_id++, std::move(value),
                                              "steps[0].args cannot supply caller identity or capability authority") &&
                                 authority_rejected;
        }
        Check(authority_rejected, "run_actions: step arguments cannot claim caller identity or capability authority");
        bool package_rejected = true;
        for (const char* action : {"workflow.run", "extensions.list", "resources.read"})
            package_rejected = invalid_step(boundary_id++, step(action),
                                            "steps[0]: package-bound workflows and resources are not AI actions") &&
                               package_rejected;
        Check(package_rejected, "run_actions: package-bound workflows and resources cannot bypass the action envelope");
        {
            json::Value steps = json::Value::Array();
            for (int i = 0; i < 13; ++i) steps.push(step("nav.back"));
            c.Send(Request(19, "tools/call", CallParams("run_actions", steps_args(std::move(steps)))));
        }
        Check(c.Response(19, r) && ToolErrorIs(r, "invalid_args") && ToolText(r).find("at most 12") != std::string::npos,
              "run_actions: more than 12 steps are refused");
        {
            json::Value steps = json::Value::Array();
            json::Value waits = step("nav.go");
            waits.set("wait", "forever");
            steps.push(std::move(waits));
            c.Send(Request(90, "tools/call", CallParams("run_actions", steps_args(std::move(steps)))));
        }
        Check(c.Response(90, r) && ToolErrorIs(r, "invalid_args"), "run_actions: a wait outside its values is refused");
        {
            json::Value steps = json::Value::Array();
            steps.push(step("view.details"));
            c.Send(Request(91, "tools/call", CallParams("run_actions", steps_args(std::move(steps)))));
        }
        Check(c.Response(91, r) && ToolErrorIs(r, "unsupported"),
              "run_actions: well-formed steps reach the broker (the test broker has no such tool)");

        // Progress while Pulse asks the user.
        json::Value zip_params = CallParams("create_zip", Args({{"paths", Strings({"C:\\Data\\a.txt"})}}));
        json::Value meta = json::Value::Object();
        meta.set("progressToken", "tok-1");
        zip_params.set("_meta", meta);
        c.Send(Request(15, "tools/call", zip_params));
        std::vector<json::Value> notes;
        const bool zipped = c.Response(15, r, &notes) && ToolText(r, &is_error).find("a.zip") != std::string::npos && !is_error;
        bool progressed = false;
        for (const auto& n : notes)
            progressed |= n.get_string("method") == "notifications/progress" &&
                          n.find("params")->get_string("progressToken") == "tok-1";
        Check(zipped && progressed, "a confirmed Create: progress notification with the token, then the result");

        // Out of order, cancellation without an answer.
        c.Send(Request(20, "tools/call", CallParams("search", Args({{"query", "slow"}}))));
        c.Send(Request(21, "tools/call", CallParams("list_folder", Args({{"path", "C:\\y"}}))));
        Check(c.Response(21, r) && ToolText(r).find("C:\\\\y") != std::string::npos,
              "a later call answers while an earlier one still runs");
        c.Send(Notification("notifications/cancelled", Args({{"requestId", 20}, {"reason", "user"}})));
        Sleep(300);
        c.Send(Request(22, "ping"));
        notes.clear();
        Check(c.Response(22, r, &notes), "ping after the cancel");
        Sleep(700);
        json::Value late;
        bool answered20 = false;
        for (const auto& n : notes) answered20 |= n.find("id") && json::Serialize(*n.find("id")) == "20";
        while (c.Next(late, 200)) answered20 |= late.find("id") && json::Serialize(*late.find("id")) == "20";
        Check(!answered20 && search_cancelled, "a cancelled call gets no answer and the tool sees the cancel");

        // The call limit.
        for (int id = 30; id < 34; ++id) c.Send(Request(id, "tools/call", CallParams("search", Args({{"query", "x"}}))));
        Sleep(200);
        c.Send(Request(34, "tools/call", CallParams("list_folder", Args({{"path", "C:\\z"}}))));
        Check(c.Response(34, r) && ToolErrorIs(r, "busy") && ToolText(r).find("retryable") != std::string::npos,
              "a fifth concurrent call: busy (retryable)");
        c.Send(Request(30, "tools/call", CallParams("list_folder", Args({{"path", "C:\\z"}}))));
        Check(c.Response(30, r) && ErrorCode(r) == -32600, "a request id still running cannot be reused");
        for (int id = 30; id < 34; ++id) c.Send(Notification("notifications/cancelled", Args({{"requestId", id}})));
        Sleep(500);

        // JSON-RPC errors.
        c.SendLine("{bad json");
        Check(c.Response(nullptr, r) && ErrorCode(r) == -32700, "malformed JSON: -32700 with id null");
        c.SendLine("[{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"ping\"}]");
        Check(c.Response(nullptr, r) && ErrorCode(r) == -32600, "a batch: -32600");
        c.Send(Request(41, "foo/bar"));
        Check(c.Response(41, r) && ErrorCode(r) == -32601, "an unknown method: -32601");
        c.SendLine("{\"jsonrpc\":\"1.0\",\"id\":42,\"method\":\"ping\"}");
        Check(c.Response(42, r) && ErrorCode(r) == -32600, "jsonrpc other than 2.0: -32600");
        c.SendLine("{\"jsonrpc\":\"2.0\",\"id\":43,\"method\":\"ping\",\"params\":[1]}");
        Check(c.Response(43, r) && ErrorCode(r) == -32602, "params that are not an object: -32602");
        c.SendLine("{\"jsonrpc\":\"2.0\",\"id\":44,\"method\":\"ping\",\"id\":45}");
        Check(c.Response(nullptr, r) && ErrorCode(r) == -32700, "duplicate keys are malformed JSON");
        c.SendLine("   ");
        c.Send(Request(46, "ping"));
        Check(c.Response(46, r) && r.find("result"), "blank lines are ignored");

        // Modern requests.
        json::Value discover = json::Value::Object();
        discover.set("_meta", ModernMeta());
        c.Send(Request("d1", "server/discover", discover));
        bool modern = c.Response("d1", r) && r.find("result");
        if (modern) {
            const json::Value& res = *r.find("result");
            std::string versions;
            for (const auto& v : res.find("supportedVersions")->items()) versions += v.as_string() + " ";
            modern = res.get_string("resultType") == "complete" && versions.find("2026-07-28") != std::string::npos &&
                     versions.find("2025-11-25") != std::string::npos && res.find("capabilities")->find("tools") &&
                     res.find("_meta")->find("io.modelcontextprotocol/serverInfo")->get_string("name") == "pulse" &&
                     res.find("ttlMs") && res.get_string("cacheScope") == "private";
        }
        Check(modern, "server/discover: resultType complete, modern and legacy versions, tools, serverInfo, cache hints");
        json::Value list_modern = json::Value::Object();
        list_modern.set("_meta", ModernMeta());
        c.Send(Request(50, "tools/list", list_modern));
        Check(c.Response(50, r) && r.find("result")->get_string("resultType") == "complete" &&
                  r.find("result")->find("tools")->size() == 12 && r.find("result")->find("ttlMs"),
              "modern tools/list: resultType and ttlMs");
        json::Value call_modern = CallParams("list_folder", Args({{"path", "C:\\m"}}));
        call_modern.set("_meta", ModernMeta());
        c.Send(Request(51, "tools/call", call_modern));
        Check(c.Response(51, r) && r.find("result")->get_string("resultType") == "complete" && ToolText(r).find("C:\\\\m") != std::string::npos,
              "modern tools/call: resultType complete");
        json::Value old_version = json::Value::Object();
        old_version.set("_meta", ModernMeta("1900-01-01"));
        c.Send(Request(52, "tools/list", old_version));
        const json::Value* data = c.Response(52, r) && r.find("error") ? r.find("error")->find("data") : nullptr;
        Check(ErrorCode(r) == -32022 && data && data->find("supported") && data->find("supported")->size() >= 2 &&
                  data->get_string("requested") == "1900-01-01",
              "an unsupported _meta version: -32022 with supported and requested");

        c.CloseInput();
        Check(c.WaitExit(5000) == 0, "Pulse.AI.exe exits 0 when stdin closes");
        Check(c.ReadErr().find("Pulse.AI:") != std::string::npos, "stderr has diagnostics, stdout only protocol");
    }

    auto quick_call = [&](Child& c, const char* client, json::Value& r) {
        json::Value init = json::Value::Object();
        init.set("protocolVersion", "2025-11-25");
        init.set("clientInfo", Args({{"name", client}, {"version", "1"}}));
        c.Send(Request(1, "initialize", init));
        if (!c.Response(1, r)) return false;
        c.Send(Request(2, "tools/list"));
        if (!c.Response(2, r) || r.find("result")->find("tools")->size() != 12) return false;
        c.Send(Request(3, "tools/call", CallParams("list_folder", Args({{"path", "C:\\Data"}}))));
        return c.Response(3, r, nullptr, 15000);
    };

    // --- refusals.
    {
        Child c;
        c.Start(L"\"" + host + L"\" --mcp --server \"" + self + L"\" --pipe \\\\.\\pipe\\Pulse.Tools.none." +
                std::to_wstring(GetCurrentProcessId()));
        json::Value r;
        Check(quick_call(c, "test-client", r) && ToolErrorIs(r, "pulse_not_running") &&
                  ToolText(r).find("retryable") != std::string::npos,
              "Pulse not running: initialize and tools/list work, calls get pulse_not_running (retryable)");
    }
    {
        Child c;
        c.Start(L"\"" + host + L"\" --mcp --server \"" + self + L"\"");
        json::Value r;
        Check(quick_call(c, "test-client", r) && ToolErrorIs(r, "pulse_not_running"), "no pipe given: pulse_not_running");
    }
    {
        Child c;
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        c.Start(L"\"" + host + L"\" --mcp --server \"" + std::wstring(system) + L"\\cmd.exe\" --pipe " + pipe);
        host_pid = c.pid();
        json::Value r;
        Check(quick_call(c, "test-client", r) && ToolErrorIs(r, "access_denied"),
              "a pipe served by another program than --server: access_denied, nothing sent");
    }
    {
        Child c;
        c.Start(L"\"" + host + L"\" --mcp --server \"" + self + L"\" --pipe " + pipe);
        host_pid = c.pid();
        json::Value r;
        Check(quick_call(c, "blocked-client", r) && ToolErrorIs(r, "client_not_approved"),
              "a client the user denied: client_not_approved");
    }
    {
        Child c;
        c.Start(L"\"" + host + L"\" --mcp --server \"" + self + L"\" --pipe " + pipe);
        host_pid = 1;   // the broker's verify_peer now rejects this host
        json::Value r;
        Check(quick_call(c, "test-client", r) && ToolErrorIs(r, "pack_outdated"),
              "a host the broker rejects: pack_outdated with a hint");
    }
    broker.Stop();

    // --- the launcher.
    const std::wstring pulse_exe = build + L"\\pulse.exe";
    const bool have_pulse = GetFileAttributesW(pulse_exe.c_str()) != INVALID_FILE_ATTRIBUTES;
    Check(have_pulse, "pulse.exe is built beside the test");
    if (have_pulse) {
        // Bundled (Pulse.AI.exe beside pulse.exe).
        SetEnvironmentVariableW(L"LOCALAPPDATA", (root + L"\\local1").c_str());
        packs::CreateDirectoryChain(root + L"\\local1");
        {
            Child c;
            Check(c.Start(L"\"" + pulse_exe + L"\" --mcp-stdio"), "pulse.exe --mcp-stdio starts");
            json::Value r;
            Check(quick_call(c, "launcher-client", r) && ToolErrorIs(r, "pulse_not_running"),
                  "through the launcher (bundled host): initialize, tools/list, calls report pulse_not_running");
            const auto hosts = ChildrenOf(c.pid(), L"Pulse.AI.exe");
            Check(hosts.size() == 1 && _wcsicmp(ImageOf(hosts.front()).c_str(), host.c_str()) == 0,
                  "the launcher started the Pulse.AI.exe beside it");
            c.CloseInput();
            Check(c.WaitExit(5000) == 0, "closing stdin ends host and launcher (exit 0)");
        }
        {
            Child c;
            c.Start(L"\"" + pulse_exe + L"\" --mcp-stdio");
            json::Value r;
            c.Send(Request(1, "ping"));
            c.Response(1, r);
            const auto hosts = ChildrenOf(c.pid(), L"Pulse.AI.exe");
            TerminateProcess(c.process(), 9);
            Check(hosts.size() == 1 && ProcessGone(hosts.front(), 5000), "killing the launcher kills the host (job object)");
        }

        // A copy of pulse.exe without a sibling host: the installed pack decides.
        const std::wstring lone_dir = root + L"\\lone", lone = lone_dir + L"\\pulse.exe";
        packs::CreateDirectoryChain(lone_dir);
        Check(CopyFileW(pulse_exe.c_str(), lone.c_str(), FALSE) != FALSE, "copy pulse.exe away from its host");
        SetEnvironmentVariableW(L"LOCALAPPDATA", (root + L"\\local2").c_str());
        packs::CreateDirectoryChain(root + L"\\local2");
        {
            Child c;
            c.Start(L"\"" + lone + L"\" --mcp-stdio");
            const DWORD code = c.WaitExit(10000);
            const std::string err = c.ReadErr();
            Check(code == kLauncherPackUnusable && err.rfind("Pulse: ", 0) == 0 && err.find("AI") != std::string::npos,
                  "no pack installed: exit 3 and one line on stderr");
        }

        // The real Pulse.AI.exe installed as the pack of this build.
        std::string catalog;
        {
            const auto h = ReadAll(build + L"\\generated\\ai_pack_catalog.h");
            catalog.assign(h.begin(), h.end());
        }
        auto quoted_after = [&](const std::string& marker, size_t from) {
            const size_t at = catalog.find(marker, from);
            if (at == std::string::npos) return std::string{};
            const size_t open = catalog.find("L\"", at + marker.size());
            const size_t close = open == std::string::npos ? open : catalog.find('"', open + 2);
            return close == std::string::npos ? std::string{} : catalog.substr(open + 2, close - open - 2);
        };
        const std::string version = quoted_after("kAiPackRelease{L\"ai\",", 0);
        const size_t row = catalog.find("{L\"Pulse.AI.exe\"");
        const std::string sha = quoted_after("ull, ", row);
        const std::string packed_sha = quoted_after("ull, ", catalog.find("ull, ", row) + 5);
        // The asset name carries the release channel (ai-windows-..., ai-win81-...).
        const std::string asset_name = packed_sha.empty() ? std::string{} : quoted_after(packed_sha + "\",", row);
        const std::wstring asset(asset_name.begin(), asset_name.end());
        const auto packed = ReadAll(build + L"\\ai-pack\\" + asset);
        const auto plain = ReadAll(host);
        Check(!version.empty() && sha.size() == 64 && packed_sha.size() == 64 && !asset.empty() &&
              !packed.empty() && !plain.empty(),
              "this build's catalog row and asset are readable");
        const std::wstring version_w(version.begin(), version.end()), sha_w(sha.begin(), sha.end()),
            packed_sha_w(packed_sha.begin(), packed_sha.end());
        const std::wstring base_url = L"https://packs.example.invalid/";
        const PackFile file{L"Pulse.AI.exe", plain.size(), sha_w.c_str(), packed.size(), packed_sha_w.c_str(), asset.c_str()};
        const PackRelease release{L"ai", version_w.c_str(), base_url.c_str(), &file, 1};
        const UpdateResponseReader reader = [&](std::wstring_view url, uint64_t maximum, const std::atomic<bool>&,
                                                const std::function<bool(const void*, DWORD)>& consume,
                                                UpdateError& category, DWORD& code) {
            if (url != base_url + asset) { category = UpdateError::HttpStatus; code = 404; return false; }
            if (packed.size() > maximum) { category = UpdateError::ResponseTooLarge; code = ERROR_FILE_TOO_LARGE; return false; }
            return consume(packed.data(), static_cast<DWORD>(packed.size()));
        };
        SetEnvironmentVariableW(L"LOCALAPPDATA", (root + L"\\local3").c_str());
        const std::wstring packs_root = root + L"\\local3\\Pulse\\packs";
        packs::CreateDirectoryChain(packs_root);
        PackInstallProgress progress;
        DWORD install_error = 0;
        Check(InstallPack(release, packs_root, progress, install_error, reader) == PackInstallOutcome::Installed,
              "the real Pulse.AI.exe installs as this build's AI pack");
        {
            Child c;
            c.Start(L"\"" + lone + L"\" --mcp-stdio");
            json::Value r;
            Check(quick_call(c, "launcher-client", r) && ToolErrorIs(r, "pulse_not_running"),
                  "Ready pack: the launcher verifies, holds and starts it; the MCP server answers");
            const auto hosts = ChildrenOf(c.pid(), L"Pulse.AI.exe");
            const std::string image = hosts.size() == 1 ? json::Utf8(ImageOf(hosts.front())) : std::string{};
            Check(Lower(image).find("\\local3\\pulse\\packs\\ai\\") != std::string::npos,
                  "the host runs from packs\\ai\\<version>");
            c.CloseInput();
            Check(c.WaitExit(5000) == 0, "and exits with it");
        }
    }

    SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    std::printf("ai_mcp_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
