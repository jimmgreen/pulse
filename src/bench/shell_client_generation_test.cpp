#define PULSE_SHELL_CLIENT_TEST
#include "../ipc/shell_client.cpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <chrono>

namespace pulse::l10n {
const wchar_t* Pick(const wchar_t*, const wchar_t* english) noexcept { return english; }
}
namespace pulse::ipc {
struct ShellClientTestAccess {
    static std::unique_ptr<ShellClient, void(*)(ShellClient*)> Make() {
        return {new ShellClient, [](ShellClient* client) { delete client; }};
    }
    static uint64_t Generation(ShellClient& client) {
        std::lock_guard lock(client.send_mutex_);
        return client.connection_ ? client.connection_->generation : 0;
    }
    static DWORD Process(ShellClient& client) {
        std::lock_guard lock(client.send_mutex_); return client.child_.dwProcessId;
    }
    static bool ChildCleared(ShellClient& client) {
        std::lock_guard lock(client.send_mutex_);
        return !client.child_started_ && !client.child_.hProcess && !client.child_.hThread;
    }
    static std::pair<std::weak_ptr<void>, HANDLE> Connection(ShellClient& client) {
        std::lock_guard lock(client.send_mutex_);
        if (!client.connection_) return {};
        return {client.connection_, client.connection_->pipe};
    }
    static void KillServer(ShellClient& client) {
        std::lock_guard lock(client.send_mutex_);
        if (client.child_.hProcess) TerminateProcess(client.child_.hProcess, 19);
    }
};
}
namespace {
using namespace pulse::ipc;
int failures = 0;
void Check(bool value, const char* name) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", name); fflush(stdout); if (!value) ++failures;
}
template<class F> bool Await(F predicate, DWORD limit = 5000) {
    const auto end = GetTickCount64() + limit;
    do { if (predicate()) return true; Sleep(10); } while (GetTickCount64() < end);
    return predicate();
}
bool Io(HANDLE pipe, void* data, DWORD size, bool write) {
    return DeadlinePipeIo(pipe, static_cast<uint8_t*>(data), size, write, GetTickCount64() + 10000, [] { return false; });
}
int Stub(const std::wstring& mode, const std::filesystem::path& marker, DWORD parent) {
    const HANDLE first_file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool first = first_file != INVALID_HANDLE_VALUE;
    if (first) CloseHandle(first_file);
    if (mode == L"early" && first) return 17;
    const HANDLE pipe = CreateNamedPipeW(PipeNameFor(parent).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    OVERLAPPED connected{}; connected.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = ConnectNamedPipe(pipe, &connected) != FALSE;
    if (!ok) {
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) ok = true;
        else if (error == ERROR_IO_PENDING) {
            DWORD bytes = 0;
            if (WaitForSingleObject(connected.hEvent, 10000) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(pipe, &connected, &bytes, FALSE) != FALSE;
            else { CancelIoEx(pipe, &connected); GetOverlappedResult(pipe, &connected, &bytes, TRUE); }
        }
    }
    CloseHandle(connected.hEvent);
    if (!ok) { CloseHandle(pipe); return 3; }
    if (mode == L"partial") {
        MsgHeader header{}; header.type = RSP_DONE;
        Io(pipe, &header, 3, true);
        std::ofstream(marker.wstring() + L".partial") << "ready";
        Sleep(10000); CloseHandle(pipe); return 0;
    }
    for (;;) {
        MsgHeader header{};
        if (!Io(pipe, &header, sizeof(header), false) || header.magic != kMagic || header.payload_size > kMaxPayload) break;
        std::vector<uint8_t> payload(header.payload_size);
        if (!payload.empty() && !Io(pipe, payload.data(), header.payload_size, false)) break;
        if (first && (mode == L"abort" || mode == L"sendfail")) continue;
        PayloadWriter writer;
        if (header.type == REQ_CTX_QUERY) {
            writer.PutU32(header.request_id); writer.PutU32(0); writer.PutU32(0); writer.PutStringArray({});
            header.type = RSP_CTX_ITEMS;
        } else { writer.PutU32(0); writer.PutU32(0); writer.PutString(L""); header.type = RSP_DONE; }
        header.payload_size = static_cast<uint32_t>(writer.data().size());
        if (!Io(pipe, &header, sizeof(header), true) ||
            !Io(pipe, const_cast<uint8_t*>(writer.data().data()), header.payload_size, true)) break;
    }
    CloseHandle(pipe); return 0;
}
struct Results {
    std::mutex mutex;
    std::map<uint32_t, std::vector<uint32_t>> done;
    std::set<uint32_t> reserved;
    bool order = true;
    ShellClient::Callbacks Callbacks() {
        ShellClient::Callbacks cb;
        cb.done = [this](uint32_t id, uint32_t hr, bool, auto) {
            std::lock_guard lock(mutex); order &= reserved.contains(id); done[id].push_back(hr);
        };
        cb.ctx_items = [this](uint32_t id, auto, bool, auto) {
            std::lock_guard lock(mutex); order &= reserved.contains(id); done[id].push_back(0);
        };
        return cb;
    }
    auto Reserve() { return [this](uint32_t id) { std::lock_guard lock(mutex); reserved.insert(id); }; }
    bool Completed(uint32_t id) { std::lock_guard lock(mutex); return done.contains(id); }
    bool Once(uint32_t id, uint32_t hr = 0) {
        std::lock_guard lock(mutex); return done[id].size() == 1 && done[id].front() == hr && order;
    }
};
void Command(const std::wstring& exe, const wchar_t* mode, const std::filesystem::path& marker) {
    shell_test_child_command = L"\"" + exe + L"\" --stub " + mode + L" \"" + marker.wstring() + L"\"";
}
void Reconnect(const std::wstring& exe, const std::filesystem::path& root, bool send_failure) {
    Command(exe, send_failure ? L"sendfail" : L"abort", root / (send_failure ? L"sendfail" : L"abort"));
    auto client = ShellClientTestAccess::Make(); Results results;
    HANDLE paused = CreateEventW(nullptr, TRUE, FALSE, nullptr), release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> once = false;
    shell_test_before_disconnect = [&] {
        if (!once.exchange(true)) { SetEvent(paused); WaitForSingleObject(release, 7000); }
    };
    client->Start(results.Callbacks());
    const auto old_id = client->CreateFolder(L"old", results.Reserve());
    const auto other_id = client->CreateFolder(L"other-old", results.Reserve());
    const auto old_generation = ShellClientTestAccess::Generation(*client);
    const auto old = ShellClientTestAccess::Connection(*client);
    Check(old_generation != 0, "isolated real child connected before reconnect race");
    Sleep(1600); // Exercise normal production spawn backoff, not a test override.
    if (send_failure) ShellClientTestAccess::KillServer(*client); else client->Abort(old_id);
    Check(WaitForSingleObject(paused, 3000) == WAIT_OBJECT_0, "old reader paused after real pipe failure before cleanup");
    const auto new_id = client->CreateFolder(L"new", results.Reserve());
    const auto new_generation = ShellClientTestAccess::Generation(*client);
    const auto new_pid = ShellClientTestAccess::Process(*client);
    DWORD flags = 0;
    Check(!old.first.expired() && GetHandleInformation(old.second, &flags), "old pipe stays owned until its reader has drained cancellation");
    Check(new_generation > old_generation && new_pid != 0, "new connection established while old reader cleanup is delayed");
    SetEvent(release);
    Check(Await([&] { return results.Completed(new_id) && results.Completed(other_id) && results.Completed(old_id); }),
        "new and retried old requests reach terminal responses after stale cleanup");
    Check(results.Once(new_id) && results.Once(other_id) &&
        results.Once(old_id, send_failure ? 0 : HRESULT_FROM_WIN32(ERROR_TIMEOUT)),
        "new request survives; old requests complete exactly once with correct abort status");
    Check(ShellClientTestAccess::Generation(*client) == new_generation && ShellClientTestAccess::Process(*client) == new_pid,
        "stale reader cannot kill or replace the new child connection");
    Check(Await([&] { return old.first.expired(); }), "retired pipe ownership released after old IO and reconciliation finish");
    client->Stop(); shell_test_before_disconnect = {};
    CloseHandle(paused); CloseHandle(release);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring_view(argv[1]) == L"--stub")
        return Stub(argv[2], argv[3], wcstoul(argv[4], nullptr, 10));
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto root = std::filesystem::path(module).parent_path() / L"shell-client-fixtures" / std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(root);
    Command(module, L"early", root / L"early");
    {
        auto client = ShellClientTestAccess::Make(); Results results;
        client->Start(results.Callbacks());
        Check(Await([&] { return std::filesystem::exists(root / L"early") && ShellClientTestAccess::ChildCleared(*client); }),
            "host exit before first pipe clears child process and thread handles");
        Check(Await([&] { return ShellClientTestAccess::Generation(*client) != 0; }), "same client automatically recovers after early exit without Stop/Start");
        const auto id = client->QueryContextMenu({}, 0, false, false, {}, results.Reserve());
        Check(Await([&] { return results.Completed(id); }) && results.Once(id), "fast menu response observes pre-registered request id");
        const auto gen_before_suspend = ShellClientTestAccess::Generation(*client);
        Check(gen_before_suspend != 0 && !client->SuspendIfIdle(60000),
            "SuspendIfIdle honors min_idle_ms before inactivity threshold elapses");
        Check(client->SuspendIfIdle(0), "SuspendIfIdle(0) disconnects idle shell host");
        Check(Await([&] { return ShellClientTestAccess::ChildCleared(*client) && ShellClientTestAccess::Generation(*client) == 0; }),
            "idle suspend retires pipe and terminates shell child cleanly");
        Sleep(150);
        Check(ShellClientTestAccess::ChildCleared(*client), "suspended client does not auto-respawn while idle");
        const auto woke_id = client->CreateFolder(L"after-suspend", results.Reserve());
        Check(Await([&] { return results.Completed(woke_id); }) && results.Once(woke_id) &&
            ShellClientTestAccess::Generation(*client) > gen_before_suspend,
            "new request after SuspendIfIdle immediately wakes host with fresh generation");
        client->Stop();
    }
    Reconnect(module, root, false);
    Reconnect(module, root, true);
    Command(module, L"partial", root / L"partial");
    {
        auto client = ShellClientTestAccess::Make(); Results results; client->Start(results.Callbacks());
        Check(Await([&] { return std::filesystem::exists(root / L"partial.partial"); }), "real child writes partial frame before Stop");
        const auto start = GetTickCount64(); client->Stop();
        Check(GetTickCount64() - start < 2000, "Stop cancels and drains outstanding partial-frame read without handle reuse");
    }
    shell_test_child_command = L"\"" + (root / L"missing.exe").wstring() + L"\"";
    {
        auto client = ShellClientTestAccess::Make(); Results results; client->Start(results.Callbacks());
        const auto id = client->InvokeContextMenu(1, 1, L"", L"", results.Reserve());
        Check(results.Completed(id) && results.Once(id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED)),
            "synchronous unreachable completion before Submit returns observes reserved id");
        client->Stop();
    }
    printf("shell-client failures=%d fixtures=%ls\n", failures, root.c_str());
    return failures ? 1 : 0;
}
