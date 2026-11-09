#include "../ui/folder_picker_operations.h"
#include "../common/localization.h"
#include "../app/context_menu.h"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string_view>

namespace {
int failures = 0;
bool create_only = false;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int RunFilePickerOperationsTest() {
    using namespace pulse;
    setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef PULSE_FILE_PICKER_OPERATIONS_STANDALONE
    if (create_only) {
        const auto desktop_name = L"PulsePickerOperations_" + std::to_wstring(GetCurrentProcessId());
        const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) return 2;
    }
#endif
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"picker-operations-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root / L"destination");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    std::ofstream(root / L"source.txt") << "picker fixture";
    std::ofstream(root / L"occupied.txt") << "do not overwrite";
    const HWND owner = CreateWindowExW(0, L"STATIC", L"picker operation test", WS_OVERLAPPED,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "isolated owner window");
    if (!owner) return 1;
    ui::PickerOperationSession session;
    const auto run = [&](ops::OpRequest request) {
        auto result = session.Run(owner, std::move(request), false, D2D1::ColorF(0x0078D4));
        Check(IsWindowEnabled(owner), "operation restores owner after worker settles");
        return result;
    };
    const auto creation_checks = [&] {
        for (const bool folder : {true, false}) {
            const auto base = root / (folder ? L"New folder" : L"New text document.txt");
            if (folder) std::filesystem::create_directory(base);
            else std::ofstream(base) << "existing content";
            ops::OpRequest create;
            create.type = folder ? ops::OpType::CreateFolder : ops::OpType::CreateTextFile;
            create.sources = {base.wstring()};
            const auto unique = session.Run(owner, std::move(create), false, D2D1::ColorF(0x0078D4), true);
            std::wstring actual;
            for (const auto& completion : unique.completed)
                if (!completion.refresh_only && !completion.destinations.empty()) actual = completion.destinations.front();
            const auto expected = root / (folder ? L"New folder (2)" : L"New text document (2).txt");
            Check(unique.error.empty() && actual == expected.wstring() && std::filesystem::exists(expected) &&
                  (folder ? std::filesystem::is_directory(base) : Read(base) == "existing content"),
                  folder ? "picker uses shared unique folder naming and returns actual destination"
                         : "picker uses shared unique file naming without overwriting existing contents");
            Check(IsWindowEnabled(owner), "unique-name preparation restores owner after worker completes");
        }
        int cancel_checks = 0;
        Check(app::UniqueChildName(root.wstring(), L"New folder", L"", [&] { return ++cancel_checks >= 2; }).empty(),
            "shared unique-name search observes cancellation after a filesystem probe");
    };
    if (create_only) {
        creation_checks();
        DestroyWindow(owner);
        OleUninitialize();
        return failures ? 1 : 0;
    }
    ops::OpsManager main_queue;
    main_queue.Start({});
    const auto create_through_main = [&](const wchar_t* name) {
        const auto before = main_queue.Status().completed_ops;
        ops::OpRequest create;
        create.type = ops::OpType::CreateTextFile;
        create.sources = {(root / name).wstring()};
        main_queue.Submit(std::move(create));
        const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline && main_queue.Status().completed_ops == before) Sleep(10);
        return main_queue.Status().completed_ops > before &&
            main_queue.Status().last_error.empty() && std::filesystem::exists(root / name);
    };
    Check(create_through_main(L"before.txt"), "main queue shell callback works before picker operations");
    ops::OpRequest rename;
    rename.type = ops::OpType::Rename;
    rename.sources = {(root / L"source.txt").wstring()};
    rename.new_name = L"renamed.txt";
    auto result = run(rename);
    Check(result.error.empty() && !std::filesystem::exists(root / L"source.txt") &&
          Read(root / L"renamed.txt") == "picker fixture", "picker rename preserves contents");
    Check(result.completed.size() == 1 && result.completed.front().destinations ==
          std::vector<std::wstring>{(root / L"renamed.txt").wstring()}, "rename reports destination for reselection");
    rename.sources = {(root / L"renamed.txt").wstring()};
    rename.new_name = L"occupied.txt";
    result = run(rename);
    Check(!result.error.empty() && result.completed.empty() && Read(root / L"occupied.txt") == "do not overwrite" &&
          Read(root / L"renamed.txt") == "picker fixture", "rename collision returns error without overwriting");
    rename.new_name = L"..\\outside.txt";
    result = run(rename);
    Check(!result.error.empty() && Read(root / L"renamed.txt") == "picker fixture", "rename rejects path traversal");
    ops::OpRequest copy;
    copy.type = ops::OpType::Copy;
    copy.sources = {(root / L"renamed.txt").wstring(), (root / L"occupied.txt").wstring()};
    copy.dest_dir = (root / L"destination").wstring();
    result = run(copy);
    Check(result.error.empty() && Read(root / L"destination/renamed.txt") == "picker fixture" &&
          Read(root / L"destination/occupied.txt") == "do not overwrite" &&
          Read(root / L"renamed.txt") == "picker fixture", "picker multi-file copy preserves sources and contents");
    Check(session.CanUndo(), "session preserves undo across consecutive operations");
    result = session.Undo(owner, false, D2D1::ColorF(0x0078D4));
    Check(result.error.empty() && !std::filesystem::exists(root / L"destination/renamed.txt") &&
          !std::filesystem::exists(root / L"destination/occupied.txt"), "copy undo settles all produced files");
    result = session.Undo(owner, false, D2D1::ColorF(0x0078D4));
    Check(result.error.empty() && Read(root / L"source.txt") == "picker fixture" &&
          !std::filesystem::exists(root / L"renamed.txt"), "second undo reaches earlier rename across failed requests");
    // Restore only this test's copied items so fixture cleanup also removes them from the bin.
    ops::OpRequest restore;
    restore.type = ops::OpType::RestoreRecycle;
    restore.sources = {(root / L"destination/renamed.txt").wstring(), (root / L"destination/occupied.txt").wstring()};
    result = run(restore);
    Check(result.error.empty(), "restore isolated copied fixtures after undo");
    rename.sources = {(root / L"source.txt").wstring()};
    rename.new_name = L"renamed.txt";
    result = run(rename);
    Check(result.error.empty(), "session remains usable after undo");
    copy.collision_policy = ops::CollisionPolicy::KeepBoth;
    result = run(copy);
    bool kept = result.error.empty() && !result.completed.empty();
    size_t kept_count = 0;
    for (const auto& completed : result.completed) {
        if (completed.refresh_only) continue;
        for (const auto& destination : completed.destinations) {
            kept = kept && std::filesystem::exists(destination) &&
                destination != (root / L"destination/renamed.txt").wstring() &&
                destination != (root / L"destination/occupied.txt").wstring();
            ++kept_count;
        }
    }
    Check(kept && kept_count == 2, "keep-both copy returns actual new destination names");
    std::filesystem::create_directory(root / L"moved");
    copy.type = ops::OpType::Move;
    copy.dest_dir = (root / L"moved").wstring();
    result = run(copy);
    Check(result.error.empty() && Read(root / L"moved/renamed.txt") == "picker fixture" &&
          Read(root / L"moved/occupied.txt") == "do not overwrite" &&
          !std::filesystem::exists(root / L"renamed.txt") && !std::filesystem::exists(root / L"occupied.txt"),
          "picker multi-file move removes only transferred sources");
    size_t moved = 0;
    for (const auto& completed : result.completed)
        if (!completed.refresh_only && completed.type == ops::OpType::Move) moved += completed.sources.size();
    Check(moved == 2, "move completion identifies sources for cut clipboard completion");
    result = session.Undo(owner, false, D2D1::ColorF(0x0078D4));
    Check(result.error.empty() && Read(root / L"renamed.txt") == "picker fixture" &&
          Read(root / L"occupied.txt") == "do not overwrite" &&
          !std::filesystem::exists(root / L"moved/renamed.txt") &&
          !std::filesystem::exists(root / L"moved/occupied.txt"), "multi-file move undo waits for every queued inverse");
    ops::OpRequest remove;
    remove.type = ops::OpType::RecycleDelete;
    remove.sources = {(root / L"renamed.txt").wstring()};
    result = run(remove);
    Check(result.error.empty() && !std::filesystem::exists(root / L"renamed.txt") && session.CanUndo(),
          "picker recycle delete retains undo");
    result = session.Undo(owner, false, D2D1::ColorF(0x0078D4));
    Check(result.error.empty() && Read(root / L"renamed.txt") == "picker fixture", "picker delete undo restores contents");
    ops::OpRequest cancelled_copy;
    cancelled_copy.type = ops::OpType::Copy;
    cancelled_copy.sources = {(root / L"renamed.txt").wstring()};
    cancelled_copy.dest_dir = (root / L"destination").wstring();
    // The existing destination forces a decision if the worker wins the cancellation race.
    PostMessageW(owner, WM_CLOSE, 0, 0);
    result = run(cancelled_copy);
    Check(!result.error.empty() && IsWindow(owner) && Read(root / L"renamed.txt") == "picker fixture",
          "owner close cancels queued operation without destroying owner or hanging");
    ops::OpRequest create_folder;
    create_folder.type = ops::OpType::CreateFolder;
    create_folder.sources = {(root / L"created").wstring()};
    result = run(create_folder);
    Check(result.error.empty() && std::filesystem::is_directory(root / L"created"),
          "session accepts new folder after cancellation");
    result = session.Undo(owner, false, D2D1::ColorF(0x0078D4));
    Check(result.error.empty() && !std::filesystem::exists(root / L"created"), "new folder undo removes created folder");
    restore.sources = create_folder.sources;
    result = run(restore);
    Check(result.error.empty(), "restore isolated created folder for cleanup");
    Check(create_through_main(L"after.txt"), "picker leaves main queue shell callbacks and connection intact");
    main_queue.Stop();
    creation_checks();
    ops::OpRequest unsupported;
    unsupported.type = ops::OpType::EmptyRecycle;
    unsupported.sources = {(root / L"renamed.txt").wstring()};
    result = run(unsupported);
    Check(!result.error.empty() && Read(root / L"renamed.txt") == "picker fixture",
          "isolated picker queue rejects unsupported shell operations");
    DestroyWindow(owner);
    OleUninitialize();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

#if defined(PULSE_FILE_PICKER_OPERATIONS_STANDALONE)
int main(int argc, char** argv) {
    create_only = argc == 2 && std::string_view(argv[1]) == "--create-only";
    return RunFilePickerOperationsTest();
}
#endif
