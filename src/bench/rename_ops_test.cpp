#include "../ops/ops_manager.h"
#include "../ipc/shell_client.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <atomic>
using namespace pulse::ops;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
bool Wait(OpsManager& ops, uint64_t before) {
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (ops.Status().completed_ops > before && !ops.Status().active) return true;
        Sleep(5);
    }
    return false;
}
bool Exact(const std::filesystem::path& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(path.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find); return path.filename().wstring() == data.cFileName;
}
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"rename-ops-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root / L"child");
    auto make = [&](const wchar_t* name) { std::ofstream(root / name) << "fixture"; };
    if (argc > 1 && std::string_view(argv[1]) == "--shell-roundtrip") {
        OpsManager roundtrip;
        roundtrip.Start([] {});
        OpRequest create; create.type = OpType::CreateTextFile;
        create.sources = {(root / L"shell-created.txt").wstring()};
        roundtrip.Submit(create);
        const bool received = Wait(roundtrip, 0);
        const auto create_status = roundtrip.Status();
        std::printf("[INFO] create received=%d phase=%d exists=%d error=", received,
            static_cast<int>(create_status.phase), Exact(root / L"shell-created.txt"));
        for (const auto c : create_status.last_error) std::printf("%04x ", static_cast<unsigned>(c));
        std::printf("\n");
        Check(received && create_status.phase == OpPhase::Completed &&
            Exact(root / L"shell-created.txt"), "normal shell CreateNewFile receives Done");
        Check(pulse::ipc::ShellClient::Instance().Ping(), "normal shell ping submitted on proven connection");
        const auto after_ping = roundtrip.Status().completed_ops;
        create.sources = {(root / L"shell-after-ping.txt").wstring()};
        roundtrip.Submit(create);
        Check(Wait(roundtrip, after_ping) && roundtrip.Status().phase == OpPhase::Completed &&
            Exact(root / L"shell-after-ping.txt"), "shell response stream continues after Ping");
        roundtrip.Stop();
        std::filesystem::remove_all(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--startup-stop") {
        for (int i = 0; i < 20; ++i) {
            make(L"startup.txt");
            OpsManager immediate;
            HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            immediate.Start([&] { if (immediate.Status().completed_ops) SetEvent(done); });
            OpRequest request; request.type = OpType::Rename;
            request.sources = {(root / L"startup.txt").wstring()}; request.new_name = L"startup-renamed.txt";
            immediate.Submit(request);
            Check(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0 && Exact(root / L"startup-renamed.txt"),
                "immediate startup rename reaches disk");
            const auto started = GetTickCount64();
            std::printf("[INFO] startup iteration=%d pid=%lu stopping\n", i, GetCurrentProcessId());
            immediate.Stop();
            std::printf("[INFO] startup stop elapsed=%llu ms\n", GetTickCount64() - started);
            Check(GetTickCount64() - started < 3000, "immediate startup stop finishes promptly");
            CloseHandle(done);
            DeleteFileW((root / L"startup-renamed.txt").c_str());
        }
        std::filesystem::remove_all(root);
        return failures ? 1 : 0;
    }
    OpsManager ops;
    std::atomic<bool> cancel_batch{false};
    ops.Start([&] {
        const auto status = ops.Status();
        if (status.type == OpType::BatchRename && status.active && status.completed_items == 1 && cancel_batch.exchange(false))
            ops.CancelCurrent();
    });
    auto rename = [&](const wchar_t* from, const wchar_t* to) {
        const auto before = ops.Status().completed_ops;
        OpRequest request; request.type = OpType::Rename; request.sources = {(root / from).wstring()}; request.new_name = to;
        ops.Submit(std::move(request)); Check(Wait(ops, before), "rename completes within deadline");
    };
    auto undo = [&] { const auto before = ops.Status().completed_ops; ops.Undo(); Check(Wait(ops, before), "undo completes within deadline"); };
    make(L"Report.txt"); rename(L"Report.txt", L"report.txt");
    Check(Exact(root / L"report.txt"), "case-only filename changes on disk");
    undo(); Check(Exact(root / L"Report.txt"), "case-only undo restores exact spelling");
    make(L"ordinary.txt"); rename(L"ordinary.txt", L"changed.txt");
    Check(Exact(root / L"changed.txt") && !std::filesystem::exists(root / L"ordinary.txt"), "ordinary file rename");
    undo(); Check(Exact(root / L"ordinary.txt"), "ordinary file undo");
    std::filesystem::create_directory(root / L"Folder"); rename(L"Folder", L"folder");
    Check(Exact(root / L"folder"), "case-only directory rename");
    undo(); Check(Exact(root / L"Folder"), "directory undo");
    rename(L"Folder", L"RenamedFolder");
    Check(Exact(root / L"RenamedFolder") && !std::filesystem::exists(root / L"Folder"), "ordinary directory rename");
    undo(); Check(Exact(root / L"Folder"), "ordinary directory undo");
    make(L"stay.txt"); rename(L"stay.txt", L"child\\moved.txt");
    Check(std::filesystem::exists(root / L"stay.txt") && !std::filesystem::exists(root / L"child" / L"moved.txt") &&
        ops.Status().phase == OpPhase::Failed, "path component rejected without moving file");
    for (const auto name : {L"child/name.txt", L"..\\escape.txt", L"C:\\escape.txt", L"bad:name", L"bad?name",
        L"CON.txt", L"LPT1", L"trailing.", L"trailing ", L".", L"..", L""}) {
        rename(L"stay.txt", name);
        Check(Exact(root / L"stay.txt") && ops.Status().phase == OpPhase::Failed, "invalid filename rejected");
    }
    make(L"first.txt"); make(L"second.txt"); make(L"third.txt");
    OpRequest batch; batch.type = OpType::BatchRename;
    batch.sources = {(root / L"first.txt").wstring(), (root / L"second.txt").wstring(), (root / L"third.txt").wstring()};
    batch.new_names = {L"first-new.txt", L"second-new.txt", L"third-new.txt"};
    cancel_batch = true;
    const auto before = ops.Status().completed_ops; ops.Submit(batch); Check(Wait(ops, before), "batch terminates");
    Check(Exact(root / L"first-new.txt") && Exact(root / L"second.txt") && Exact(root / L"third.txt") &&
        ops.Status().phase == OpPhase::Failed && ops.Status().completed_items == 1, "cancel after first item leaves remaining files unchanged");
    undo(); Check(Exact(root / L"first.txt"), "cancelled batch retains completed item undo");
    Check(Exact(root / L"second.txt") && Exact(root / L"third.txt"), "partial undo leaves unprocessed items unchanged");
    OpRequest invalid_count; invalid_count.type = OpType::Rename;
    invalid_count.sources = {(root / L"second.txt").wstring(), (root / L"third.txt").wstring()};
    invalid_count.new_name = L"multi.txt";
    const auto count_before = ops.Status().completed_ops; ops.Submit(invalid_count);
    Check(Wait(ops, count_before) && ops.Status().phase == OpPhase::Failed &&
        Exact(root / L"second.txt") && Exact(root / L"third.txt") && !std::filesystem::exists(root / L"multi.txt"),
        "single rename rejects multiple sources without changing files");
    make(L"conflict-source.txt"); make(L"conflict-target.txt");
    // A collision must never enter the Shell host's silent replacement path.
    rename(L"conflict-source.txt", L"conflict-target.txt");
    std::printf("[INFO] conflict result source=%d target=%d phase=%d\n", Exact(root / L"conflict-source.txt"),
        Exact(root / L"conflict-target.txt"), static_cast<int>(ops.Status().phase));
    Check(Exact(root / L"conflict-source.txt") && Exact(root / L"conflict-target.txt") &&
        ops.Status().phase == OpPhase::Failed && ops.Status().last_error != L"操作层未启动",
        "existing target rejected without replacing either file");
    make(L"locked.txt");
    HANDLE locked = CreateFileW((root / L"locked.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold isolated file against deletion");
    rename(L"locked.txt", L"locked-renamed.txt");
    Check(Exact(root / L"locked.txt") && !std::filesystem::exists(root / L"locked-renamed.txt") &&
        ops.Status().phase == OpPhase::Failed && ops.Status().last_error != L"操作层未启动",
        "sharing violation reports failure without changing file");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    ops.Stop();
    // This absolute directory was created by this process under bench_data.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
