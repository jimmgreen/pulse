#include "../ops/elevated_transfer_client.h"
#include "../common/path_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
std::string Read(const std::filesystem::path& path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}}; }
void Write(const std::filesystem::path& path, const char* text) { std::ofstream out(path); out << text; }
}
int RunReviewTransferTests();
int RunReviewTemplateTests();
int RunReviewRoutingTests(bool invoke_only = false);
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--invoke-feedback") return RunReviewRoutingTests(true);
    if (argc == 2 && std::wstring(argv[1]) == L"--review-routing") return RunReviewRoutingTests();
    if (argc == 2 && std::wstring(argv[1]) == L"--review-template") return RunReviewTemplateTests();
    if (argc >= 3 && std::wstring(argv[1]) == L"--template-child") {
        // Isolated argv recorder only: never open the selected path or launch a helper.
        std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary);
        const uint32_t count = static_cast<uint32_t>(argc - 3);
        output.write(reinterpret_cast<const char*>(&count), sizeof(count));
        for (int i = 3; i < argc; ++i) {
            const std::wstring value(argv[i]); const uint32_t size = static_cast<uint32_t>(value.size());
            output.write(reinterpret_cast<const char*>(&size), sizeof(size));
            output.write(reinterpret_cast<const char*>(value.data()), size * sizeof(wchar_t));
        }
        return output ? 0 : 2;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--review-transfer") return RunReviewTransferTests();
    if (argc == 3 && std::wstring(argv[1]) == L"--orphan-child") {
        const std::filesystem::path fixture(argv[2]);
        const auto source = fixture / L"orphan_source", target = fixture / L"orphan_target";
        std::filesystem::create_directories(source); std::filesystem::create_directories(target);
        Write(source / L"item.txt", "orphan fixture");
        std::atomic<bool> cancel{false};
        const auto result = pulse::ops::TransferWithElevatedHelper({(source / L"item.txt").wstring()}, target.wstring(), false, nullptr, cancel);
        if (FAILED(result.hr)) return 3;
        { std::ofstream output(fixture / L"helper.pid"); output << pulse::ops::ElevatedHelperProcessIdForTesting(); }
        const auto started = GetTickCount64();
        while (!std::filesystem::exists(fixture / L"exit_parent") && GetTickCount64() - started < 15000) Sleep(25);
        ExitProcess(0); // Deliberately bypass explicit client/helper shutdown.
    }
    using namespace pulse::ops;
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n'; if (!ok) ++failed; };
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    const auto root = std::filesystem::path(temp) / (L"pulse_session_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    std::atomic<bool> cancel{false};
    if (argc == 2 && std::wstring(argv[1]) == L"--drive-eject") {
        pulse::storage::VirtualDiskIdentity identity;
        identity.volume_name = L"\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}\\";
        identity.image_path = L"C:\\nonexistent-pulse-fixture.vhdx";
        cancel = true;
        auto result = EjectWithElevatedHelper(identity, nullptr, cancel);
        check(result.cancelled && ElevatedHelperProcessIdForTesting() == 0, "eject cancelled before authorization starts no helper");
        cancel = false;
        auto invalid = identity; invalid.volume_name = L"C:\\";
        result = EjectWithElevatedHelper(invalid, nullptr, cancel);
        check(FAILED(result.hr) && ElevatedHelperProcessIdForTesting() == 0, "eject rejects drive letters before helper launch");
        result = EjectWithElevatedHelper(identity, nullptr, cancel);
        check(FAILED(result.hr) && !result.mutated && ElevatedHelperProcessIdForTesting() != 0,
              "helper returns identity lookup failure without mutation or replay");
        ShutdownElevatedTransferHelper();
        return failed ? 1 : 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--create-folder") {
        std::filesystem::create_directories(root);
        const auto folder = root / L"新建文件夹 授权测试";
        ElevatedTransferCallbacks callbacks;
        std::vector<bool> authorization;
        callbacks.authorization = [&](bool value) { authorization.push_back(value); };
        cancel = true;
        auto result = CreateFolderWithElevatedHelper(folder.wstring(), nullptr, cancel, callbacks);
        check(result.cancelled && ElevatedHelperProcessIdForTesting() == 0 && !std::filesystem::exists(folder),
              "cancel before authorization creates nothing and launches no helper");
        cancel = false;
        result = CreateFolderWithElevatedHelper(L"relative\\folder", nullptr, cancel, callbacks);
        check(FAILED(result.hr) && ElevatedHelperProcessIdForTesting() == 0, "relative path rejected before helper launch");
        result = CreateFolderWithElevatedHelper(folder.wstring(), nullptr, cancel, callbacks);
        check(SUCCEEDED(result.hr) && result.destinations.size() == 1 &&
              std::filesystem::is_directory(folder), "authenticated helper creates directory and reports completion");
        check(authorization == std::vector<bool>({true, false}), "one authorization lifecycle for creation");
        const auto helper = ElevatedHelperProcessIdForTesting();
        result = CreateFolderWithElevatedHelper(folder.wstring(), nullptr, cancel, callbacks);
        check(FAILED(result.hr) && result.destinations.empty(), "existing directory fails without false completion or native conflict dialog");
        result = CreateFolderWithElevatedHelper((root / L"missing" / L"child").wstring(), nullptr, cancel, callbacks);
        check(FAILED(result.hr) && !std::filesystem::exists(root / L"missing"), "missing parent fails without creating ancestors");
        result = CreateFolderWithElevatedHelper((root / L"second").wstring(), nullptr, cancel, callbacks);
        check(SUCCEEDED(result.hr) && helper == ElevatedHelperProcessIdForTesting() && authorization.size() == 2,
              "subsequent creation reuses authorized helper without another prompt");
        ShutdownElevatedTransferHelper();
        std::filesystem::remove_all(root);
        return failed ? 1 : 0;
    }
    try {
        const auto source = root / L"source", destination = root / L"destination", moved = root / L"moved";
        std::filesystem::create_directories(source); std::filesystem::create_directories(destination); std::filesystem::create_directories(moved);
        Write(source / L"a.txt", "original");
        ElevatedTransferCallbacks callbacks;
        callbacks.verify_copies = true;
        std::vector<bool> authorization_events;
        callbacks.authorization = [&](bool awaiting) { authorization_events.push_back(awaiting); };
        auto invalid = TransferWithElevatedHelper(std::vector<std::wstring>(4097, (source / L"a.txt").wstring()), destination.wstring(), false, nullptr, cancel);
        check(FAILED(invalid.hr) && !invalid.error.empty() && ElevatedHelperProcessIdForTesting() == 0, "oversized selection rejected before helper launch");
        auto result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.size() == 1 && Read(destination / L"a.txt") == "original", "authenticated helper copy reports exact completion");
        check(authorization_events == std::vector<bool>({true, false}), "new authorization lifecycle reported exactly once");
        const DWORD helper = ElevatedHelperProcessIdForTesting();
        unsigned conflicts = 0;
        callbacks.conflict = [&](const ConflictItemInfo& item) {
            ++conflicts;
            check(pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(item.source), (source / L"a.txt").wstring()) &&
                  pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(item.destination), (destination / L"a.txt").wstring()), "conflict names forwarded to Pulse");
            return ElevatedConflictAnswer{ConflictChoice::Skip, false};
        };
        Write(source / L"a.txt", "updated");
        result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.empty() && conflicts == 1 && Read(destination / L"a.txt") == "original", "skip stays skipped with no false completion");
        check(helper != 0 && helper == ElevatedHelperProcessIdForTesting(), "same helper reused across requests");
        callbacks.conflict = [&](const ConflictItemInfo&) { return ElevatedConflictAnswer{ConflictChoice::Replace, false}; };
        result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && Read(destination / L"a.txt") == "updated", "Pulse replace choice applied in existing engine");
        result = TransferWithElevatedHelper({(source / L"a.txt").wstring()}, moved.wstring(), true, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.size() == 1 && !std::filesystem::exists(source / L"a.txt") && Read(moved / L"a.txt") == "updated", "helper move reports actual paths");
        std::filesystem::create_directories(source / L"tree"); std::filesystem::create_directories(destination / L"tree");
        Write(source / L"tree" / L"same.txt", "new"); Write(source / L"tree" / L"fresh.txt", "fresh");
        Write(destination / L"tree" / L"same.txt", "old");
        conflicts = 0;
        callbacks.conflict = [&](const ConflictItemInfo&) { ++conflicts; return ElevatedConflictAnswer{ConflictChoice::KeepBoth, false}; };
        result = TransferWithElevatedHelper({(source / L"tree").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && conflicts == 1 && Read(destination / L"tree" / L"same.txt") == "old" && Read(destination / L"tree" / L"fresh.txt") == "fresh", "nested merge delegates collision without batch replay");
        bool renamed_new = false;
        for (size_t i = 0; i < result.sources.size(); ++i) {
            if (pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(result.sources[i]), (source / L"tree" / L"same.txt").wstring()) &&
                !pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(result.destinations[i]), (destination / L"tree" / L"same.txt").wstring()) &&
                Read(result.destinations[i]) == "new") renamed_new = true;
        }
        check(renamed_new, "nested keep-both returns actual renamed file containing new content");
        bool want_pause = false, saw_pause = false;
        ULONGLONG paused_at = 0;
        const auto pause_test_started = GetTickCount64();
        callbacks.conflict = [&](const ConflictItemInfo&) { want_pause = true; return ElevatedConflictAnswer{ConflictChoice::Replace, false}; };
        callbacks.paused = [&] { return want_pause && GetTickCount64() - pause_test_started < 5000 && (!paused_at || GetTickCount64() - paused_at < 200); };
        callbacks.progress = [&](const OpStatus& status) {
            if (status.phase == OpPhase::Paused) { saw_pause = true; if (!paused_at) paused_at = GetTickCount64(); }
        };
        result = TransferWithElevatedHelper({(source / L"tree" / L"same.txt").wstring()}, (destination / L"tree").wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(SUCCEEDED(result.hr) && saw_pause && Read(destination / L"tree" / L"same.txt") == "new", "pause and resume propagate through authorized session");
        callbacks.paused = {}; callbacks.progress = {};

        callbacks.conflict = [&](const ConflictItemInfo&) { return ElevatedConflictAnswer{ConflictChoice::Cancel, false}; };
        result = TransferWithElevatedHelper({(source / L"tree").wstring()}, destination.wstring(), false, nullptr, cancel, ShellCollisionPolicy::System, callbacks);
        check(result.cancelled && FAILED(result.hr), "cancel choice round-trips without blocking");
        check(helper == ElevatedHelperProcessIdForTesting(), "cancelled request retains authorized session");
        cancel = true;
        result = TransferWithElevatedHelper({(source / L"tree").wstring()}, destination.wstring(), false, nullptr, cancel);
        check(result.cancelled && helper == ElevatedHelperProcessIdForTesting(), "pre-cancel launches no replacement helper");
        cancel = false;
        Write(source / L"delete.txt", "deletion fixture");
        result = DeleteWithElevatedHelper({(source / L"delete.txt").wstring()}, true, nullptr, cancel, callbacks);
        check(SUCCEEDED(result.hr) && result.sources.size() == 1 && result.destinations.empty() && !std::filesystem::exists(source / L"delete.txt"), "permanent delete opcode reports actual deleted source");
        check(helper == ElevatedHelperProcessIdForTesting() && authorization_events == std::vector<bool>({true, false}), "cached helper deletes without repeated authorization state");
        result = DeleteWithElevatedHelper({L"\\\\localhost\\nonexistent_pulse_recycle_test_share\\file.txt"}, false, nullptr, cancel, callbacks);
        check(FAILED(result.hr) && !result.cancelled && !result.mutated && result.sources.empty(), "recycle opcode refuses unsupported location without permanent fallback");
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, helper);
        ShutdownElevatedTransferHelper();
        check(process && WaitForSingleObject(process, 10000) == WAIT_OBJECT_0, "explicit shutdown ends helper");
        if (process) CloseHandle(process);
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --orphan-child \"" + root.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        const bool launched = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
        bool orphan_closed = false;
        if (launched) {
            CloseHandle(child.hThread);
            const auto started = GetTickCount64();
            DWORD orphan_pid = 0;
            while (!orphan_pid && GetTickCount64() - started < 15000 && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) {
                { std::ifstream input(root / L"helper.pid"); input >> orphan_pid; }
                if (!orphan_pid) Sleep(25);
            }
            HANDLE orphan = orphan_pid ? OpenProcess(SYNCHRONIZE, FALSE, orphan_pid) : nullptr;
            Write(root / L"exit_parent", "exit");
            orphan_closed = WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0 && orphan && WaitForSingleObject(orphan, 10000) == WAIT_OBJECT_0;
            if (orphan) CloseHandle(orphan);
            CloseHandle(child.hProcess);
        }
        check(orphan_closed, "parent process exit ends its retained helper");
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n'; ++failed;
        ShutdownElevatedTransferHelper();
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
    }
    return failed ? 1 : 0;
}
