#include "../app/single_instance_coordinator.h"
#include "../app/shell_tag_menu.h"
#include "../app/open_request_pipe.h"
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <string>

namespace {
using Coordinator = pulse::app::SingleInstanceCoordinator;
Coordinator* primary = nullptr;
std::wstring received;
int opens = 0, tags = 0, copydata_calls = 0;
constexpr UINT kQueueWake = WM_APP + 127;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kQueueWake && primary) {
        auto request = primary->TakeQueuedOpenRequest();
        if (request) { ++opens; received = request->path; Coordinator::CompleteDelivery(*request, true); }
        return 0;
    }
    if (message == WM_COPYDATA) {
        ++copydata_calls;
        if (!primary || !primary->OwnsEndpoint(hwnd)) return FALSE;
        auto* data = reinterpret_cast<COPYDATASTRUCT*>(lparam);
        Coordinator::OpenRequest request;
        if (Coordinator::DecodeOpenRequest(data, request)) {
            const auto accepted = primary->AcceptOpenRequest(request, GetTickCount64());
            if (accepted == Coordinator::OpenAcceptance::Invalid) return FALSE;
            if (accepted == Coordinator::OpenAcceptance::New) { ++opens; received = request.path; }
            return TRUE;
        }
        pulse::app::ShellTagRequest tag;
        if (pulse::app::DecodeShellTagRequest(data, tag)) {
            ++tags; received = tag.path;
            return tag.action == pulse::app::ShellTagAction::Remove && !tag.operation_id.empty();
        }
        return FALSE;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
HWND Window() {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = Coordinator::WindowClassName();
    cls.lpfnWndProc = WindowProc;
    RegisterClassW(&cls);
    return CreateWindowW(cls.lpszClassName, L"Private endpoint fixture", 0, 0, 0, 1, 1,
        nullptr, nullptr, cls.hInstance, nullptr);
}
HANDLE Spawn(const std::wstring& arguments, const std::wstring& cwd) {
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    auto command = L"\"" + std::wstring(exe) + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, cwd.c_str(), &startup, &process)) return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
}
bool WaitChild(HANDLE process) {
    if (!process) return false;
    const auto deadline = GetTickCount64() + 10000;
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(2);
    }
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    if (code == STILL_ACTIVE) { TerminateProcess(process, 2); WaitForSingleObject(process, 2000); }
    CloseHandle(process);
    return code == 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--decoy") {
        HWND window = Window();
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(argv[2]) + L".Ready").c_str());
        HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (std::wstring(argv[2]) + L".Stop").c_str());
        if (!window || !ready || !stop) return 2;
        SetEvent(ready);
        while (WaitForSingleObject(stop, 5) == WAIT_TIMEOUT) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        }
        DestroyWindow(window); CloseHandle(ready); CloseHandle(stop); return 0;
    }
    if (argc >= 5 && std::wstring_view(argv[1]) == L"--sender") {
        Coordinator sender;
        if (sender.Acquire(argv[2]) != Coordinator::AcquireResult::Existing) return 2;
        if (std::wstring_view(argv[3]) == L"tag") {
            wchar_t* command[]{argv[0], const_cast<wchar_t*>(L"--tag-remove"),
                const_cast<wchar_t*>(L"private-tag"), argv[4]};
            auto request = pulse::app::ParseShellTagArgs(4, command);
            return request && pulse::app::ForwardShellTagRequest(*request, argv[2]) ? 0 : 3;
        }
        return sender.ForwardOpenPath(argv[4], 5000) ? 0 : 4;
    }
    namespace fs = std::filesystem;
    int failed = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failed += !ok; };
    const auto original = fs::current_path();
    const auto parent = original / L"bench_data";
    fs::create_directories(parent);
    const auto root = parent / (L"singleton-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"A"); fs::create_directories(root / L"B");
    const auto name = L"Local\\Pulse.PrivateEndpointTest-" + root.filename().wstring();
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (name + L".Ready").c_str());
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, (name + L".Stop").c_str());
    Coordinator coordinator;
    primary = &coordinator;
    check(coordinator.Acquire(name) == Coordinator::AcquireResult::Primary, "private primary owns namespace");
    HANDLE decoy = Spawn(L"--decoy \"" + name + L"\"", root.wstring());
    check(decoy && WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "same-class isolated process creates its window first");
    check(!Coordinator::FindPrimaryWindow(name), "unpublished primary does not discover isolated same-class window");
    Coordinator legacy_secondary;
    check(legacy_secondary.Acquire(name) == Coordinator::AcquireResult::Existing &&
        !legacy_secondary.ForwardOpenPath(L".", 80),
        "legacy-style mutex owner without endpoint causes explicit timeout, not another primary or class fallback");
    HANDLE early_sender = Spawn(L"--sender \"" + name + L"\" open \".\"", (root / L"B").wstring());
    HWND window = Window();
    check(window && coordinator.PublishEndpoint(window), "only mutex owner publishes endpoint");
    check(WaitChild(early_sender) && opens == 1 && received == (root / L"B").wstring(),
        "requester waits through primary initialization instead of delivering to decoy");
    HWND later_decoy = Window();
    check(Coordinator::FindPrimaryWindow(name) == window, "later same-class window does not replace published endpoint");
    Coordinator isolated;
    check(!isolated.PublishEndpoint(window), "unowned coordinator cannot publish");
    check(Coordinator::FindPrimaryWindow(name) == window, "discovery verifies published process and nonce");
    fs::current_path(root / L"A");
    for (const auto& path : {L".", L"..\\B\\file with spaces.txt", L"", L"pulse:starred", L"MyComputerFolder"}) {
        received = L"not received";
        const int before = opens;
        HANDLE sender = Spawn(L"--sender \"" + name + L"\" open \"" + path + L"\"", (root / L"B").wstring());
        check(WaitChild(sender) && opens == before + 1, "separate requester forwards only to primary");
        std::wstring expected;
        fs::current_path(root / L"B");
        Coordinator::NormalizeLaunchPath(path, expected);
        fs::current_path(root / L"A");
        check(received == expected, "received launch path retains requester CWD or virtual/empty semantics");
    }
    HANDLE sender = Spawn(L"--sender \"" + name + L"\" tag \".\\tag file.txt\"", (root / L"B").wstring());
    check(WaitChild(sender) && tags == 1 && received == (root / L"B" / L"tag file.txt").wstring(),
        "tag action and operation ID use verified endpoint and requester CWD");
    Coordinator::OpenRequest request;
    request.id[0] = 1; request.deadline = GetTickCount64() + 1000; request.path = L"";
    check(coordinator.AcceptOpenRequest(request, GetTickCount64()) == Coordinator::OpenAcceptance::New &&
        coordinator.AcceptOpenRequest(request, GetTickCount64()) == Coordinator::OpenAcceptance::Duplicate &&
        coordinator.AcceptOpenRequest(request, request.deadline) == Coordinator::OpenAcceptance::Invalid,
        "request ID deduplication and deadline remain enforced");
    std::wstring normalized;
    check(Coordinator::NormalizeLaunchPath(L"\\\\server\\share\\folder", normalized) &&
        normalized == L"\\\\server\\share\\folder", "UNC normalization is lexical without network access");
    check(!Coordinator::NormalizeLaunchPath(std::wstring(L"a\0b", 3), normalized),
        "invalid embedded NUL is rejected rather than converted to empty wakeup");
    pulse::app::ShellTagRequest tag{L"tag", L"C:\\file", pulse::app::ShellTagAction::Remove, L"operation"};
    const auto encoded = pulse::app::EncodeShellTagRequest(tag);
    COPYDATASTRUCT tag_data{pulse::app::ShellTagMessageId(),
        static_cast<DWORD>((encoded.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(encoded.c_str())};
    pulse::app::ShellTagRequest decoded;
    check(pulse::app::DecodeShellTagRequest(&tag_data, decoded) &&
        decoded.action == tag.action && decoded.operation_id == tag.operation_id,
        "tag wire roundtrip retains explicit action and retry ID");
    std::wstring legacy = L"tag\nC:\\file";
    tag_data.cbData = static_cast<DWORD>((legacy.size() + 1) * sizeof(wchar_t)); tag_data.lpData = legacy.data();
    check(pulse::app::DecodeShellTagRequest(&tag_data, decoded) &&
        decoded.action == pulse::app::ShellTagAction::Add && decoded.operation_id.empty(),
        "legacy tag request defaults to add without synthesizing retry identity");
    coordinator.Release();
    check(!Coordinator::FindPrimaryWindow(name), "released endpoint is not replaced by same-class decoy");
    check(coordinator.Acquire(name) == Coordinator::AcquireResult::Primary && coordinator.PublishEndpoint(window) &&
        Coordinator::FindPrimaryWindow(name) == window, "primary restart publishes a fresh endpoint");
    coordinator.Release();
    check(coordinator.Acquire(name) == Coordinator::AcquireResult::Primary, "RawV2 primary reacquires private namespace");
    const int raw_before = opens, copy_before = copydata_calls;
    HANDLE raw_sender = Spawn(L"--sender \"" + name + L"\" open \"pulse:win-e\"", root.wstring());
    check(coordinator.PublishEndpoint(window, kQueueWake), "RawV2 publication starts receiver before advertising capability");
    const bool accepted_without_pump = raw_sender && WaitForSingleObject(raw_sender, 6000) == WAIT_OBJECT_0;
    check(accepted_without_pump && opens == raw_before, "cold requester is accepted while primary UI does not pump");
    check(WaitChild(raw_sender), "RawV2 requester exits with acknowledged acceptance");
    auto pump = [] { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message); };
    pump();
    check(opens == raw_before + 1 && received == L"pulse:win-e" && copydata_calls == copy_before,
        "resumed UI dispatches unchanged WinE marker exactly once without COPYDATA");
    const auto endpoint = Coordinator::FindPrimaryEndpoint(name);
    check(endpoint && !endpoint->pipe_name.empty(), "RawV2 sidecar yields verified pipe identity");
    if (endpoint) {
        using Pipe = pulse::app::OpenRequestPipe;
        Coordinator::OpenRequest queued;
        queued.id[0] = 73; queued.deadline = GetTickCount64() + 5000; queued.path = L"pulse:new-tab";
        auto bytes = Coordinator::EncodeOpenRequest(queued);
        const Pipe::ServerIdentity identity{endpoint->pid, endpoint->created};
        // Exchange deliberately performs one connection attempt. A previous
        // client's ACK may still be draining; retry only explicit admission
        // Retry or a proven pre-send busy result, never an ambiguous send here.
        auto exchange = [&](const char* label) {
            unsigned attempts = 0;
            for (;;) {
                ++attempts;
                auto result = Pipe::Exchange(endpoint->pipe_name, identity, bytes, queued.deadline);
                const auto observed_at = GetTickCount64();
                printf("EXCHANGE %s attempt=%u reply=%d error=%lu stage=%u may_sent=%u remaining=%llu\n",
                    label, attempts, result.reply ? static_cast<int>(*result.reply) : -1,
                    result.error, static_cast<unsigned>(result.stage), result.request_may_have_been_sent ? 1u : 0u,
                    static_cast<unsigned long long>(queued.deadline > observed_at ? queued.deadline - observed_at : 0));
                fflush(stdout);
                const bool retry = result.reply == Pipe::Reply::Retry ||
                    (!result.reply && result.error == ERROR_PIPE_BUSY && !result.request_may_have_been_sent && result.stage == Pipe::Stage::None);
                const auto now = GetTickCount64();
                if (!retry || now >= queued.deadline) return result;
                Sleep(static_cast<DWORD>((std::min)(uint64_t{10}, queued.deadline - now)));
            }
        };
        const int before_duplicate = opens;
        const auto first = exchange("first");
        const auto duplicate = exchange("duplicate");
        check(first.reply == Pipe::Reply::Accepted && duplicate.reply == Pipe::Reply::Duplicate && opens == before_duplicate,
            "same ID has one committed queue entry before UI dispatch");
        pump();
        printf("DISPATCH before=%d after=%d expected_path_match=%u\n", before_duplicate, opens, received == queued.path ? 1u : 0u);
        check(opens == before_duplicate + 1 && received == queued.path, "duplicate pipe retry executes only once");
        const auto sidecar_name = name + L".Endpoint.RawV2." + endpoint->nonce;
        HANDLE sidecar = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, sidecar_name.c_str());
        auto* sidecar_bytes = sidecar ? static_cast<unsigned char*>(MapViewOfFile(sidecar, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 128)) : nullptr;
        check(sidecar_bytes != nullptr, "fixture can inspect its own 128-byte capability sidecar");
        if (sidecar_bytes) {
            DWORD magic = 0; std::memcpy(&magic, sidecar_bytes + 4, sizeof(magic));
            const DWORD bad_magic = 0; std::memcpy(sidecar_bytes + 4, &bad_magic, sizeof(bad_magic));
            const int before_invalid = copydata_calls;
            check(!Coordinator::FindPrimaryEndpoint(name) && !legacy_secondary.ForwardOpenPath(L"pulse:win-e", 80) && copydata_calls == before_invalid,
                "present malformed capability fails closed without legacy COPYDATA fallback");
            std::memcpy(sidecar_bytes + 4, &magic, sizeof(magic));
            UnmapViewOfFile(sidecar_bytes);
        }
        if (sidecar) CloseHandle(sidecar);
        queued.id[0] = 74; queued.deadline = GetTickCount64() + 5000;
        bytes = Coordinator::EncodeOpenRequest(queued);
        const auto pending = exchange("shutdown-pending");
        check(pending.reply == Pipe::Reply::Accepted, "shutdown fixture has accepted but undispatched work");
        const int before_release = opens;
        coordinator.Release();
        pump();
        check(!coordinator.TakeQueuedOpenRequest() && opens == before_release && !Coordinator::FindPrimaryEndpoint(name),
            "release drains receiver and cancels queued work before stale wake can dispatch");
        const auto stopped = Pipe::Exchange(endpoint->pipe_name, identity, bytes, GetTickCount64() + 100);
        check(!stopped.reply, "released endpoint no longer acknowledges requests");
    }
    coordinator.Release(); DestroyWindow(window); DestroyWindow(later_decoy); primary = nullptr;
    SetEvent(stop); check(WaitChild(decoy), "isolated helper exits cleanly");
    CloseHandle(ready); CloseHandle(stop);
    fs::current_path(original);
    std::error_code error;
    if (root.parent_path() == parent && root.filename().wstring().starts_with(L"singleton-"))
        fs::remove_all(root, error);
    check(!error && !fs::exists(root), "private fixture cleanup");
    return failed ? 1 : 0;
}
