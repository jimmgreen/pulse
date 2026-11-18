// ai_broker_test - the tool broker end to end over its named pipe, with fake
// tools: hello first, results and errors, permission classes (Create asks,
// always-allow only for Create, Change refused), confirmation denied / timed
// out, tool timeout, cancel, the per-connection call limit, malformed and
// oversized frames, impostor clients (this test process is not Pulse.AI.exe),
// undo_last limited to AI undo groups, plus ai.json and the activity log
// (summaries only, no arguments verbatim). LOCALAPPDATA is redirected to a
// scratch folder.
#include "../app/ai_broker.h"
#include "../app/ai_pack.h"
#include "../app/ai_settings.h"
#include "../app/ai_status.h"
#include "../app/pack_installer.h"
#include "../common/current_user_security.h"
#include "../common/utf8_file.h"
#include "../ipc/ai_tool_protocol.h"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pulse;
using namespace pulse::app::ai;
namespace proto = pulse::ipc::ai;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_passed : ++g_failed;
}

auto Never = [] { return false; };

class Client {
public:
    explicit Client(const std::wstring& name) {
        for (int i = 0; i < 50 && pipe_ == INVALID_HANDLE_VALUE; ++i) {
            pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe_ == INVALID_HANDLE_VALUE) WaitNamedPipeW(name.c_str(), 100);
        }
    }
    ~Client() { if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_); }
    bool ok() const { return pipe_ != INVALID_HANDLE_VALUE; }
    bool Send(const json::Value& m) { return proto::WriteFrame(pipe_, m, GetTickCount64() + 3000, Never); }
    bool SendRaw(const std::string& bytes) {
        std::string copy = bytes;
        return DeadlinePipeIoWrite(copy);
    }
    bool Hello(const char* name = "TestClient") { return Send(proto::MakeHello(name, "1.0", "1.0.57")); }
    // The next frame within `ms`; Closed when the broker hung up.
    proto::FrameStatus Read(json::Value& m, DWORD ms = 5000) {
        return proto::ReadFrame(pipe_, m, GetTickCount64() + ms, Never);
    }
    // Calls and waits for the result (skipping progress frames, counted).
    json::Value Call(int64_t id, const char* tool, json::Value args = json::Value::Object(), int* progress = nullptr) {
        if (!Send(proto::MakeCall(id, tool, std::move(args)))) return {};
        for (;;) {
            json::Value m;
            if (Read(m) != proto::FrameStatus::Ok) return {};
            if (m.get_string("type") == proto::kProgress) { if (progress) ++*progress; continue; }
            return m;
        }
    }

private:
    bool DeadlinePipeIoWrite(std::string& bytes) {
        return ipc::DeadlinePipeIo(pipe_, reinterpret_cast<uint8_t*>(bytes.data()), static_cast<DWORD>(bytes.size()),
                                   true, GetTickCount64() + 3000, Never);
    }
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

std::string Code(const json::Value& result) {
    const json::Value* error = result.find("error");
    return error ? error->get_string("code") : (result.get_bool("ok") ? "ok" : "<none>");
}

std::string FileText(const std::wstring& path) {
    std::wstring text;
    return ReadUtf8File(path, text) ? json::Utf8(text) : std::string{};
}

std::wstring PipeFor(const std::wstring& local, const wchar_t* suffix) {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return proto::ToolPipeName(session, CurrentUserSidString(), local + suffix);
}
} // namespace

int main(int argc, char** argv) {
    const bool settings_only = argc == 2 && std::string_view(argv[1]) == "--settings-only";
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"pulse_ai_broker_test_" + std::to_wstring(GetCurrentProcessId());
    app::RemovePackTree(root);
    const std::wstring local = root + L"\\local";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(local.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());

    // --- pipe names.
    const std::wstring name = PipeFor(local, L"");
    if (!settings_only) {
    const size_t last_dot = name.find_last_of(L'.');
    Check(name.rfind(L"\\\\.\\pipe\\Pulse.Tools.", 0) == 0 && last_dot != std::wstring::npos &&
          name.size() - last_dot - 1 == 16 && name.find_first_not_of(L"0123456789abcdef", last_dot + 1) == std::wstring::npos,
          "pipe name is Pulse.Tools.<session>.<16 hex digits>");
    Check(proto::ToolPipeName(1, L"S-1-5-21-1", L"C:\\A") == proto::ToolPipeName(1, L"s-1-5-21-1", L"c:\\a") &&
          proto::ToolPipeName(1, L"S-1-5-21-1", L"C:\\A") != proto::ToolPipeName(1, L"S-1-5-21-1", L"C:\\B") &&
          proto::ToolPipeName(1, L"S-1-5-21-1", L"C:\\A") != proto::ToolPipeName(2, L"S-1-5-21-1", L"C:\\A") &&
          proto::ToolPipeName(1, L"S-1-5-21-1", L"C:\\A") != proto::ToolPipeName(1, L"S-1-5-21-2", L"C:\\A"),
          "pipe name depends on session, user and LOCALAPPDATA, not on case");

    }

    // --- ai.json.
    {
        wchar_t executable[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        std::wstring ini(executable, count);
        const size_t slash = ini.find_last_of(L"\\/");
        ini.resize(slash == std::wstring::npos ? 0 : slash + 1);
        ini += L"setup-defaults.ini";
        const bool expected_enabled = GetPrivateProfileIntW(L"features", L"ai_assistant", 1, ini.c_str()) != 0;
        bool missing_malformed = true;
        Check(LoadSettings(&missing_malformed).enabled == expected_enabled && !missing_malformed,
              "missing ai.json reads install assistant default (enabled when absent)");
        Settings s;
        s.mcp_enabled = true;
        s.log_days = 45;
        s.clients = {{L"Claude", ClientGrant::Allowed}, {L"Other", ClientGrant::Session}, {L"Bad", ClientGrant::Denied}};
        s.allow = {{L"Claude", "pulse.create_zip", L"C:/Work/../Work/"}};
        Check(SaveSettings(s), "SaveSettings writes ai.json");
        bool malformed = true;
        const Settings back = LoadSettings(&malformed);
        Check(!malformed && back.mcp_enabled && back.log_days == 45 && back.clients.size() == 3 &&
              back.clients[0].grant == ClientGrant::Allowed &&
              back.clients[1].grant == ClientGrant::Ask && back.clients[2].grant == ClientGrant::Denied,
              "ai.json round-trips; a session approval is not remembered");
        Check(back.allow.size() == 1 && back.allow[0].folder == L"C:\\Work", "allow folders are normalized");
        Check(IsAlwaysAllowed(back, PermissionClass::Create, L"claude", "pulse.create_zip", L"C:\\Work") &&
              IsAlwaysAllowed(back, PermissionClass::Create, L"Claude", "pulse.create_zip", L"c:\\work\\Sub\\Deeper"),
              "always-allow covers the folder and its subfolders");
        Check(!IsAlwaysAllowed(back, PermissionClass::Create, L"Claude", "pulse.create_zip", L"C:\\Workshop") &&
              !IsAlwaysAllowed(back, PermissionClass::Create, L"Claude", "pulse.create_zip", L"C:\\Work\\..\\Windows") &&
              !IsAlwaysAllowed(back, PermissionClass::Create, L"Claude", "pulse.other", L"C:\\Work") &&
              !IsAlwaysAllowed(back, PermissionClass::Create, L"Other", "pulse.create_zip", L"C:\\Work") &&
              !IsAlwaysAllowed(back, PermissionClass::Create, L"Claude", "pulse.create_zip", L"Work"),
              "always-allow refuses siblings, .. escapes, other tools, other clients, relative paths");
        Check(!IsAlwaysAllowed(back, PermissionClass::Change, L"Claude", "pulse.create_zip", L"C:\\Work") &&
              !IsAlwaysAllowed(back, PermissionClass::Read, L"Claude", "pulse.create_zip", L"C:\\Work"),
              "always-allow applies to Create only");
        Settings drive;
        drive.allow = {{L"Claude", "pulse.create_zip", L"D:\\"}};
        Check(IsAlwaysAllowed(ParseSettings(SerializeSettings(drive)), PermissionClass::Create, L"Claude",
                              "pulse.create_zip", L"D:\\x\\y"), "a drive-root rule covers the drive");
        WriteUtf8FileAtomic(SettingsPath(), L"{\"mcp_enabled\":true,}");
        const Settings broken = LoadSettings(&malformed);
        Check(malformed && !broken.mcp_enabled, "a malformed ai.json reads as defaults (MCP off)");
        Check(ParseSettings("{\"log_days\":9999}").log_days == 30, "log_days out of range keeps the default");
        // Legacy effort is migrated to thinking without adding service preferences.
        const std::string older =
            "{\"mcp_enabled\":false,\"log_days\":30,\"clients\":[],\"allow\":[],"
            "\"assistant\":{\"dock\":\"docked\",\"model\":\"deepseek-flash\",\"effort\":2}}\n";
        const std::string migrated =
            "{\"mcp_enabled\":false,\"log_days\":30,\"clients\":[],\"allow\":[],"
            "\"assistant\":{\"dock\":\"docked\",\"model\":\"deepseek-flash\",\"thinking\":\"high\",\"effort\":2}}\n";
        Check(SerializeSettings(ParseSettings(older)) == migrated,
              "legacy effort migrates to thinking while service preferences stay absent");
        Check(ParseSettings(older).assistant.service.empty(), "no service means DeepSeek");
        Check(ParseSettings(older).enabled, "legacy assistant settings remain enabled");
        Settings disabled = ParseSettings(older);
        disabled.enabled = false;
        disabled.assistant.service = "qwen";
        disabled.assistant.model = "saved-model";
        disabled.mcp_enabled = true;
        const Settings disabled_back = ParseSettings(SerializeSettings(disabled));
        Check(!disabled_back.enabled && disabled_back.mcp_enabled &&
              disabled_back.assistant.service == "qwen" && disabled_back.assistant.model == "saved-model",
              "disabling assistant preserves provider and independent MCP setting");
        disabled.enabled = true;
        Check(ParseSettings(SerializeSettings(disabled)).enabled, "assistant can be re-enabled");
        Settings svc;
        svc.assistant.service = "qwen";
        svc.assistant.model = "qwen-plus";
        svc.assistant.services = {{"deepseek", {"deepseek-flash", ""}}, {"custom", {"my-model", "http://192.168.1.20:8000/v1"}}};
        const Settings svc_back = ParseSettings(SerializeSettings(svc));
        Check(svc_back.assistant.service == "qwen" && svc_back.assistant.model == "qwen-plus" &&
              svc_back.assistant.services.size() == 2 && svc_back.assistant.services[0].first == "deepseek" &&
              svc_back.assistant.services[0].second.model == "deepseek-flash" &&
              svc_back.assistant.services[1].second.base_url == "http://192.168.1.20:8000/v1",
              "service and the other services' models and addresses round-trip");
        const Settings bad_svc = ParseSettings(
            "{\"assistant\":{\"service\":\"Qwen!\",\"services\":{\"../x\":{\"model\":\"m\"},"
            "\"kimi\":{\"base_url\":\"ftp://x\"}}}}");
        Check(bad_svc.assistant.service.empty() && bad_svc.assistant.services.size() == 1 &&
              bad_svc.assistant.services[0].first == "kimi" && bad_svc.assistant.services[0].second.base_url.empty(),
              "malformed service ids and addresses are dropped");
    }

    if (settings_only) {
        app::RemovePackTree(root);
        std::printf("ai_broker_test settings: %d passed, %d failed\n", g_passed, g_failed);
        return g_failed ? 1 : 0;
    }

    // --- the broker with fake tools.
    std::atomic<int> confirms{0};
    std::atomic<int> decision{0};   // 0 deny, 1 allow, 2 never answer
    Settings rules;
    rules.allow = {{L"TestClient", "t.zip", L"C:\\Work"}};
    const std::wstring activity = ActivityDirectory();
    BrokerOptions options;
    options.pipe_name = name;
    options.verify_peer = [](HANDLE) { return true; };
    options.activity_directory = activity;
    options.confirm_timeout_ms = 400;
    options.confirm = [&](const ConfirmRequest& request, const std::atomic<bool>& cancelled, DWORD timeout_ms) {
        ++confirms;
        if (request.tool != "t.zip" || request.client != L"TestClient") return Decision::Deny;
        if (decision == 1) return Decision::Allow;
        if (decision == 0) return Decision::Deny;
        const auto end = GetTickCount64() + timeout_ms;
        while (!cancelled && GetTickCount64() < end) Sleep(10);
        return Decision::Timeout;
    };
    options.always_allowed = [&](const std::wstring& client, const std::string& tool, const std::wstring& folder) {
        return IsAlwaysAllowed(rules, PermissionClass::Create, client, tool, folder);
    };
    ToolBroker broker(options);
    auto wait_cancel = [](const ToolCall& call, int ms) {
        for (int i = 0; i < ms / 10 && !call.cancelled; ++i) Sleep(10);
    };
    broker.Register({"t.echo", PermissionClass::Read, [](const ToolCall& call) {
        ToolOutcome o;
        o.data = call.args;
        return o;
    }});
    broker.Register({"t.wait", PermissionClass::Read, [&](const ToolCall& call) {
        wait_cancel(call, 5000);
        return ToolOutcome{};
    }});
    ToolSpec slow{"t.slow", PermissionClass::Read, [&](const ToolCall& call) {
        wait_cancel(call, 5000);
        return ToolOutcome{};
    }};
    slow.timeout_ms = 300;
    broker.Register(slow);
    // Ignores cancellation, runs ~1 s, then reports a change it made anyway.
    ToolSpec deaf{"t.deaf", PermissionClass::Read, [](const ToolCall&) {
        Sleep(1000);
        ToolOutcome o;
        o.changed_files = true;
        o.undo_group = 777;
        return o;
    }};
    deaf.timeout_ms = 200;
    broker.Register(deaf);
    ToolSpec deaf_long = deaf;
    deaf_long.name = "t.deaf_long";
    deaf_long.timeout_ms = 30000;
    broker.Register(deaf_long);
    ToolSpec zip{"t.zip", PermissionClass::Create, [](const ToolCall&) {
        ToolOutcome o;
        o.data.set("zip", "C:\\Work\\a.zip");
        o.changed_files = true;
        o.undo_group = 77;
        return o;
    }};
    zip.target_folder = [](const json::Value& args) { return json::Wide(args.get_string("dest_folder")); };
    broker.Register(zip);
    ToolSpec checked = zip;
    checked.name = "t.zip_checked";
    checked.check = [](const json::Value& args, ToolOutcome& error) {
        if (args.find("dest_folder")) return true;
        error = ToolOutcome::Fail("invalid_args", "dest_folder is missing");
        return false;
    };
    broker.Register(checked);
    broker.Register({"t.change", PermissionClass::Change, [](const ToolCall&) { return ToolOutcome{}; }});
    broker.Register({"t.undo", PermissionClass::Undo, [&](const ToolCall& call) {
        int64_t group = 0;
        const json::Value* g = call.args.find("group");
        if (!g || !g->get_int(group, 1) || !broker.IsAiUndoGroup(static_cast<uint64_t>(group)))
            return ToolOutcome::Fail("not_ai_operation", "The last operation was not made by an AI client.");
        broker.ForgetAiUndoGroup(static_cast<uint64_t>(group));
        return ToolOutcome{};
    }});
    broker.Register({"t.throw", PermissionClass::Read, [](const ToolCall&) -> ToolOutcome { throw 1; }});

    DWORD error = 0;
    Check(broker.Start(error), "broker starts on its pipe");
    ToolBroker squatter(options);
    Check(!squatter.Start(error) && (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY),
          "a second server cannot take the name (first instance is owned)");

    {
        Client c(name);
        Check(c.ok() && c.Hello(), "client connects and says hello");
        json::Value args = json::Value::Object();
        args.set("query", "SECRET-QUERY-123");
        args.set("paths", json::Value::Array());
        json::Value paths = json::Value::Array();
        paths.push("C:\\Work\\secret-name.txt");
        paths.push("C:\\Work\\other.txt");
        paths.push("D:\\Elsewhere\\x.pdf");
        args.set("paths", std::move(paths));
        const json::Value echo = c.Call(1, "t.echo", args);
        Check(Code(echo) == "ok" && echo.find("data") && echo.find("data")->get_string("query") == "SECRET-QUERY-123" &&
              echo.find("id")->as_number() == 1, "Read tool returns its data with the call id");
        Check(Code(c.Call(2, "t.nope")) == "unsupported", "unknown tool: unsupported");
        json::Value bad = proto::MakeCall(3, "t.echo", json::Value::Object());
        bad.set("args", json::Value::Array());
        c.Send(bad);
        json::Value m;
        Check(c.Read(m) == proto::FrameStatus::Ok && Code(m) == "invalid_args", "arguments must be an object");
        Check(Code(c.Call(4, "t.change")) == "access_denied", "Change class is refused");
        Check(Code(c.Call(5, "t.throw")) == "internal", "a throwing tool reports internal");
        Check(Code(c.Call(6, "t.slow")) == "timeout", "a tool past its timeout is cancelled: timeout");
        {
            ULONGLONG t0 = GetTickCount64();
            const bool timed_out = Code(c.Call(7, "t.deaf")) == "timeout";
            const ULONGLONG timeout_ms = GetTickCount64() - t0;
            Check(timed_out && timeout_ms < 700, "a tool that ignores cancel is still answered at its timeout");
            Check(Code(c.Call(7, "t.echo")) == "invalid_args", "its call id stays taken until the handler returns");
            Sleep(1100);
            Check(broker.IsAiUndoGroup(777), "a change made after the timeout stays undoable");
            broker.ForgetAiUndoGroup(777);
            c.Send(proto::MakeCall(8, "t.deaf_long", json::Value::Object()));
            Sleep(100);
            t0 = GetTickCount64();
            c.Send(proto::MakeCancel(8));
            const bool cancelled = c.Read(m) == proto::FrameStatus::Ok && Code(m) == "cancelled";
            Check(cancelled && GetTickCount64() - t0 < 500, "cancel is answered at once even if the tool ignores it");
            Sleep(1100);
            broker.ForgetAiUndoGroup(777);
        }

        // Create: confirmation, always-allow, undo ledger.
        json::Value outside = json::Value::Object();
        outside.set("dest_folder", "C:\\Other");
        int progress = 0;
        decision = 0;
        Check(Code(c.Call(10, "t.zip", outside, &progress)) == "user_denied" && progress == 1 && confirms == 1,
              "Create asks (progress waiting_user) and a refusal is user_denied");
        decision = 2;
        Check(Code(c.Call(11, "t.zip", outside)) == "user_timeout", "an unanswered confirmation is user_timeout");
        decision = 1;
        progress = 0;
        const json::Value made = c.Call(12, "t.zip", outside, &progress);
        Check(Code(made) == "ok" && progress == 1 && broker.IsAiUndoGroup(77), "an allowed Create runs; its undo group is an AI group");
        json::Value inside = json::Value::Object();
        inside.set("dest_folder", "C:\\Work\\Sub");
        const int before = confirms;
        progress = 0;
        Check(Code(c.Call(13, "t.zip", inside, &progress)) == "ok" && confirms == before && progress == 0,
              "an always-allow rule skips the confirmation");
        {
            const int asked = confirms;
            progress = 0;
            const json::Value refused = c.Call(30, "t.zip_checked", json::Value::Object(), &progress);
            Check(Code(refused) == "invalid_args" && confirms == asked && progress == 0,
                  "a Create whose check fails answers its error without asking");
        }
        json::Value undo_user = json::Value::Object();
        undo_user.set("group", 78);
        Check(Code(c.Call(14, "t.undo", undo_user)) == "not_ai_operation", "undo_last refuses a group the user made");
        json::Value undo_ai = json::Value::Object();
        undo_ai.set("group", 77);
        Check(Code(c.Call(15, "t.undo", undo_ai)) == "ok" && !broker.IsAiUndoGroup(77) &&
              Code(c.Call(16, "t.undo", undo_ai)) == "not_ai_operation", "undo_last undoes an AI group once");

        // Cancel, duplicates and the call limit.
        c.Send(proto::MakeCall(20, "t.wait", json::Value::Object()));
        c.Send(proto::MakeCall(20, "t.echo", json::Value::Object()));
        Check(c.Read(m) == proto::FrameStatus::Ok && Code(m) == "invalid_args" && m.find("id")->as_number() == 20,
              "a call id still running cannot be reused");
        c.Send(proto::MakeCancel(20));
        Check(c.Read(m) == proto::FrameStatus::Ok && Code(m) == "cancelled", "cancel ends a running call");
        for (int id = 30; id < 34; ++id) c.Send(proto::MakeCall(id, "t.wait", json::Value::Object()));
        Sleep(100);
        c.Send(proto::MakeCall(34, "t.echo", json::Value::Object()));
        Check(c.Read(m) == proto::FrameStatus::Ok && Code(m) == "busy" && m.find("id")->as_number() == 34 &&
                  m.find("error")->get_bool("retryable"),
              "a fifth concurrent call is busy (retryable)");
        for (int id = 30; id < 34; ++id) c.Send(proto::MakeCancel(id));
        int cancelled = 0;
        for (int i = 0; i < 4; ++i) {
            const auto status = c.Read(m);
            const bool ok = status == proto::FrameStatus::Ok && Code(m) == "cancelled";
            if (!ok) std::printf("  got status %d: %s\n", static_cast<int>(status), json::Serialize(m).c_str());
            cancelled += ok;
        }
        Check(cancelled == 4, "every cancelled call answers");
    }

    // --- protocol violations close the connection.
    {
        Client c(name);
        c.Send(proto::MakeCall(1, "t.echo", json::Value::Object()));
        json::Value m;
        Check(c.Read(m, 3000) != proto::FrameStatus::Ok, "a call before hello: connection closed, no answer");
    }
    {
        Client c(name);
        c.Hello();
        c.SendRaw(std::string("\x00\x00\x50\x00", 4));   // 5 MB announced
        json::Value m;
        Check(c.Read(m, 3000) != proto::FrameStatus::Ok, "an oversized frame closes the connection");
    }
    {
        Client c(name);
        c.Hello();
        const std::string body = "{\"type\":\"call\",";
        std::string frame(4, '\0');
        frame[0] = static_cast<char>(body.size());
        c.SendRaw(frame + body);
        json::Value m;
        Check(c.Read(m, 3000) != proto::FrameStatus::Ok, "invalid JSON closes the connection");
    }
    {
        Client c(name);
        c.Send(proto::MakeHello("", "1", "1"));
        json::Value m;
        Check(c.Read(m, 3000) != proto::FrameStatus::Ok, "a hello without a client name is refused");
    }

    // --- stopping with a call in flight.
    {
        Client c(name);
        c.Hello();
        c.Send(proto::MakeCall(1, "t.wait", json::Value::Object()));
        Sleep(100);
        const auto t0 = GetTickCount64();
        broker.Stop();
        Check(GetTickCount64() - t0 < 3000 && !broker.running(), "Stop cancels running calls and returns promptly");
    }

    // --- impostors: the app's verifier only admits the verified Pulse.AI.exe.
    {
        BrokerOptions strict = options;
        strict.pipe_name = PipeFor(local, L"\\strict");
        strict.verify_peer = [](HANDLE pipe) { return app::IsAiHostPipeClient(pipe); };
        ToolBroker guarded(strict);
        guarded.Register({"t.echo", PermissionClass::Read, [](const ToolCall&) { return ToolOutcome{}; }});
        Check(guarded.Start(error), "a broker with the app's verifier starts");
        Client c(strict.pipe_name);
        c.Hello();
        c.Send(proto::MakeCall(1, "t.echo", json::Value::Object()));
        json::Value m;
        Check(c.ok() && c.Read(m, 3000) != proto::FrameStatus::Ok,
              "this test process (not Pulse.AI.exe) is rejected before anything is read");
        Check(!app::IsAiHostProcess(GetCurrentProcessId()), "IsAiHostProcess: the test executable is not the host");
        guarded.Stop();
    }
    {
        BrokerOptions picky = options;
        picky.pipe_name = PipeFor(local, L"\\picky");
        picky.approve_client = [](const std::wstring& client) { return client != L"Blocked"; };
        ToolBroker gate(picky);
        gate.Register({"t.echo", PermissionClass::Read, [](const ToolCall&) { return ToolOutcome{}; }});
        gate.Start(error);
        Client c(picky.pipe_name);
        c.Hello("Blocked");
        json::Value m;
        Check(c.Read(m, 3000) == proto::FrameStatus::Ok && Code(m) == "client_not_approved" &&
              c.Read(m, 3000) != proto::FrameStatus::Ok, "an unapproved client gets client_not_approved, then closed");
        gate.Stop();
    }

    {
        // Events for the status bar: connect, calls (with the wait for the
        // user), and DisconnectClient closing the connection.
        std::mutex events_mu;
        std::vector<BrokerEvent> events;
        BrokerOptions watched = options;
        watched.pipe_name = PipeFor(local, L"\\events");
        watched.on_event = [&](const BrokerEvent& e) {
            std::lock_guard lock(events_mu);
            events.push_back(e);
        };
        ToolBroker observed(watched);
        observed.Register({"t.echo", PermissionClass::Read, [](const ToolCall& call) {
            ToolOutcome o;
            o.data = call.args;
            return o;
        }});
        observed.Register(zip);
        observed.Start(error);
        decision = 1;
        Client c(watched.pipe_name);
        c.Hello("Watcher");
        const json::Value echoed = c.Call(1, "t.echo");
        json::Value zip_args = json::Value::Object();
        zip_args.set("dest_folder", "D:\\Other");
        int progress = 0;
        c.Call(2, "t.nothing");
        auto snapshot = [&] {
            std::lock_guard lock(events_mu);
            return events;
        };
        // t.zip asks: the fake confirm only allows TestClient, so this one is denied.
        const json::Value denied = c.Call(3, "t.zip", zip_args, &progress);
        Check(Code(denied) == "user_denied" && progress == 1, "events: a Create call waits for the user, then is denied");
        observed.DisconnectClient(L"watcher");
        json::Value m;
        Check(c.Read(m, 3000) != proto::FrameStatus::Ok, "events: DisconnectClient closes the client's connection");
        for (int i = 0; i < 100 && (snapshot().empty() || snapshot().back().kind != BrokerEventKind::Disconnected); ++i)
            Sleep(20);
        const std::vector<BrokerEvent> seen = snapshot();
        std::vector<BrokerEventKind> kinds;
        for (const BrokerEvent& e : seen) kinds.push_back(e.kind);
        using K = BrokerEventKind;
        Check(kinds == std::vector<K>{K::Connected, K::CallStarted, K::CallFinished, K::CallStarted, K::CallFinished,
                                      K::CallStarted, K::WaitingUser, K::CallFinished, K::Disconnected},
              "events: connected, each call started and finished, the wait, disconnected - in order");
        bool consistent = !seen.empty();
        for (const BrokerEvent& e : seen) consistent &= e.client == L"Watcher" && e.connection == seen.front().connection;
        Check(consistent, "events: one connection id and the client's name throughout");
        Check(seen.size() == 9 && seen[1].tool == "t.echo" && seen[1].call == 1 && seen[2].ok && echoed.is_object() &&
                  !seen[4].ok && seen[4].code == "unsupported" && seen[7].tool == "t.zip" && seen[7].code == "user_denied",
              "events: tool, call id, outcome and error code");
        observed.Stop();
        decision = 0;
    }

    {
        // ai_status: what the status bar chip shows, fed by events.
        ResetAiStatusForTest();
        auto event = [](BrokerEventKind kind, uint64_t connection, const wchar_t* client, const char* tool = "",
                        int64_t call = 0, bool ok = true, const char* code = "") {
            BrokerEvent e;
            e.kind = kind;
            e.connection = connection;
            e.client = client;
            e.tool = tool;
            e.call = call;
            e.ok = ok;
            e.code = code;
            OnAiBrokerEvent(e);
        };
        Check(GetAiChip(GetTickCount64()).state == AiChipState::Hidden, "status: no chip while not listening");
        SetAiListening(true);
        Check(GetAiChip(GetTickCount64()).state == AiChipState::Idle && GetAiChip(GetTickCount64()).client.empty(),
              "status: listening without clients is an idle chip");
        event(BrokerEventKind::Connected, 1, L"Claude");
        event(BrokerEventKind::Connected, 2, L"Claude");
        event(BrokerEventKind::CallStarted, 1, L"Claude", "pulse.search", 5);
        AiChip chip = GetAiChip(GetTickCount64());
        Check(chip.state == AiChipState::Busy && chip.client == L"Claude" && chip.tool == "search" && chip.clients == 2,
              "status: a running call makes the chip busy, with client and short tool name");
        event(BrokerEventKind::CallStarted, 2, L"Claude", "pulse.create_zip", 6);
        event(BrokerEventKind::WaitingUser, 2, L"Claude", "pulse.create_zip", 6);
        chip = GetAiChip(GetTickCount64());
        Check(chip.state == AiChipState::Waiting && chip.tool == "create_zip", "status: waiting for the user wins over busy");
        event(BrokerEventKind::CallFinished, 2, L"Claude", "pulse.create_zip", 6, false, "user_denied");
        event(BrokerEventKind::CallFinished, 1, L"Claude", "pulse.search", 5);
        const uint64_t now = GetTickCount64();
        chip = GetAiChip(now);
        Check(chip.state == AiChipState::Done && chip.burst_calls == 2 && AiChipSettlesIn(now) > 0,
              "status: finished calls show as done for a while");
        Check(GetAiChip(now + kAiDoneHoldMs + 1).state == AiChipState::Idle && AiChipSettlesIn(now + kAiDoneHoldMs + 1) == 0,
              "status: then the chip is idle again");
        const AiStatusSnapshot snap = GetAiStatus();
        Check(snap.listening && snap.clients == std::vector<std::wstring>{L"Claude"} && snap.session_calls == 2 &&
                  snap.recent.size() == 2 && snap.recent[0].tool == "create_zip" && !snap.recent[0].ok &&
                  snap.recent[0].code == "user_denied" && snap.recent[1].ok && !snap.recent[1].running,
              "status: the snapshot lists clients once and calls newest first with outcomes");
        event(BrokerEventKind::CallStarted, 1, L"Claude", "pulse.list_folder", 7);
        SetAiListening(false);
        const AiStatusSnapshot stopped = GetAiStatus();
        Check(!stopped.listening && stopped.clients.empty() && !stopped.recent[0].running &&
                  stopped.recent[0].code == "cancelled" && GetAiChip(GetTickCount64()).state == AiChipState::Hidden,
              "status: stopping forgets connections and marks running calls cancelled");
        ResetAiStatusForTest();
    }

    // --- the activity log: summaries only.
    {
        int year = 0, month = 0;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        year = st.wYear;
        month = st.wMonth;
        wchar_t file[32];
        swprintf_s(file, L"\\%04d-%02d.jsonl", year, month);
        const std::string log = FileText(activity + file);
        Check(log.find("\"tool\":\"t.echo\"") != std::string::npos && log.find("\"outcome\":\"ok\"") != std::string::npos &&
              log.find("\"client\":\"TestClient\"") != std::string::npos, "calls are logged with client, tool and outcome");
        Check(log.find("\"paths\":3") != std::string::npos && log.find("C:\\\\Work") != std::string::npos &&
              log.find("D:\\\\Elsewhere") != std::string::npos, "the log has the path count and parent folders");
        Check(log.find("SECRET-QUERY") == std::string::npos && log.find("secret-name") == std::string::npos,
              "the log never contains argument values or file names");
        Check(log.find("\"outcome\":\"user_denied\"") != std::string::npos &&
              log.find("\"undo_group\":77") != std::string::npos && log.find("\"changed\":true") != std::string::npos,
              "refusals, changes and undo groups are logged");
        WriteUtf8FileAtomic(activity + L"\\2020-01.jsonl", L"{}\n");
        WriteUtf8FileAtomic(activity + L"\\notes.txt", L"keep\n");
        PruneActivity(activity, 30, UnixMillisecondsNow());
        Check(GetFileAttributesW((activity + L"\\2020-01.jsonl").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((activity + file).c_str()) != INVALID_FILE_ATTRIBUTES &&
              GetFileAttributesW((activity + L"\\notes.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
              "pruning drops expired months only");
    }

    app::RemovePackTree(root);
    std::printf("ai_broker_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
