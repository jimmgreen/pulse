#include "../common/rename_identity.h"
#include "../ops/ops_manager.h"
#include "../common/rename_identity.h"
#include "../ipc/shell_client.h"
#include "../common/localization.h"
#include "../app/batch_rename.h"
#include <filesystem>
#include <aclapi.h>
#include <sddl.h>
#include <vector>
#include <fstream>
#include <cstdio>
#include <atomic>
#include <climits>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
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
bool Has(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }
// Failure text is for people: a reason, never a bare error number.
bool Friendly(const std::wstring& text) { return !text.empty() && !Has(text, L"错误 ") && !Has(text, L"error "); }
bool Exact(const std::filesystem::path& path) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(path.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find); return path.filename().wstring() == data.cFileName;
}
}

// This opt-in test must run at medium integrity as pulse.exe beside the real
// pulse_elevated.exe. It never touches Program Files or modifies helper trust.
int AuthorizationRenameTest() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 2;
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    const bool queried = GetTokenInformation(token, TokenUser, user.data(), size, &size) != FALSE;
    CloseHandle(token);
    Check(queried && !elevation.TokenIsElevated, "authorization client is not elevated");
    if (!queried || elevation.TokenIsElevated) return 2;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return 2;
    const std::wstring sddl = L"D:P(A;;FA;;;BA)(A;;FRFX;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR restricted = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &restricted, nullptr)) return 2;
    PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorDacl(restricted, &present, &acl, &defaulted);
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto root = std::filesystem::path(temp) / (L"pulse-rename-auth-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    for (auto name : {L"source.txt", L"collision.txt", L"locked.txt", L"batch-a.txt", L"batch-b.txt"}) std::ofstream(root / name) << "fixture";
    std::filesystem::create_directory(root / L"Folder");
    struct Saved { HANDLE handle; PSECURITY_DESCRIPTOR descriptor; PACL acl; SECURITY_DESCRIPTOR_CONTROL control; };
    std::vector<Saved> saved;
    bool protected_ok = true;
    std::vector<std::filesystem::path> paths{root};
    for (const auto& entry : std::filesystem::directory_iterator(root)) paths.push_back(entry.path());
    for (const auto& path : paths) {
        HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        PSECURITY_DESCRIPTOR original = nullptr; PACL original_acl = nullptr;
        const DWORD read_error = handle == INVALID_HANDLE_VALUE ? GetLastError() :
            GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                nullptr, nullptr, &original_acl, nullptr, &original);
        if (read_error != ERROR_SUCCESS) {
            std::printf("[INFO] fixture ACL capture path=%ls error=%lu\n", path.c_str(), read_error);
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            protected_ok = false; break;
        }
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        GetSecurityDescriptorControl(original, &control, &revision);
        saved.push_back({handle, original, original_acl, control});
    }
    // Capture every handle and original DACL before restricting a parent:
    // directory ACL propagation can otherwise remove access to its children.
    // Protect children first so restricting the root cannot alter their ACLs.
    if (protected_ok) {
        for (size_t i = saved.size(); i > 0; --i) {
            const DWORD error = SetSecurityInfo(saved[i - 1].handle, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, acl, nullptr);
            if (error != ERROR_SUCCESS) {
                std::printf("[INFO] fixture ACL restrict path=%ls error=%lu\n", paths[i - 1].c_str(), error);
                protected_ok = false; break;
            }
        }
    }
    LocalFree(restricted);
    Check(protected_ok, "isolated fixture grants administrators full access and current user read/execute");
    if (protected_ok) {
        const BOOL direct = MoveFileW((root / L"source.txt").c_str(), (root / L"direct.txt").c_str());
        const DWORD error = GetLastError();
        Check(!direct && error == ERROR_ACCESS_DENIED, "medium token cannot rename protected fixture directly");
        OpsManager ops;
        std::atomic<bool> cancel_batch{false};
        ops.Start([&] { const auto status = ops.Status();
            if (status.type == OpType::BatchRename && status.completed_items == 1 && cancel_batch.exchange(false)) ops.CancelCurrent();
        });
        auto wait = [&](uint64_t before) {
            const auto deadline = GetTickCount64() + 120000;
            while (GetTickCount64() < deadline) {
                if (ops.Status().completed_ops > before && !ops.Status().active) return true;
                if (ops.Status().authorization == AuthorizationState::ActionRequired) { ops.CancelCurrent(); }
                Sleep(10);
            }
            ops.CancelCurrent(); return false;
        };
        auto rename = [&](const wchar_t* from, const wchar_t* to) {
            const auto before = ops.Status().completed_ops;
            OpRequest request; request.type = OpType::Rename; request.sources = {(root / from).wstring()}; request.new_name = to;
            ops.Submit(request); Check(wait(before), "authorized rename terminates");
        };
        rename(L"source.txt", L"renamed.txt");
        const bool success = Exact(root / L"renamed.txt") && !Exact(root / L"source.txt");
        Check(success, "production elevated helper renames protected file");
        if (success) {
            const auto before = ops.Status().completed_ops; ops.Undo();
            Check(wait(before) && Exact(root / L"source.txt") && !Exact(root / L"renamed.txt"), "authorized undo restores protected file");
        }
        rename(L"Folder", L"RenamedFolder");
        const bool folder_success = Exact(root / L"RenamedFolder") && !Exact(root / L"Folder");
        Check(folder_success, "production helper renames protected directory");
        if (folder_success) {
            const auto before = ops.Status().completed_ops; ops.Undo();
            Check(wait(before) && Exact(root / L"Folder") && !Exact(root / L"RenamedFolder"),
                "authorized undo restores protected directory");
        }
        rename(L"source.txt", L"collision.txt");
        Check(Exact(root / L"source.txt") && Exact(root / L"collision.txt") && ops.Status().phase == OpPhase::Failed,
            "authorized collision preserves both files");
        HANDLE locked = CreateFileW((root / L"locked.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "hold protected fixture without delete sharing");
        rename(L"locked.txt", L"locked-new.txt");
        Check(Exact(root / L"locked.txt") && !Exact(root / L"locked-new.txt") && ops.Status().phase == OpPhase::Failed,
            "authorized locked rename preserves file");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        OpRequest batch; batch.type = OpType::BatchRename;
        batch.sources = {(root / L"batch-a.txt").wstring(), (root / L"batch-b.txt").wstring()};
        batch.new_names = {L"batch-a-new.txt", L"batch-b-new.txt"};
        cancel_batch = true;
        const auto before = ops.Status().completed_ops; ops.Submit(batch);
        Check(wait(before) && Exact(root / L"batch-a-new.txt") && Exact(root / L"batch-b.txt") && !Exact(root / L"batch-b-new.txt"),
            "authorized batch cancel preserves unprocessed item");
        ops.Stop();
    }
    bool restored = true;
    for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        restored &= SetSecurityInfo(it->handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
            ((it->control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            nullptr, nullptr, it->acl, nullptr) == ERROR_SUCCESS;
        CloseHandle(it->handle); LocalFree(it->descriptor);
    }
    Check(restored, "restore original fixture DACLs");
    std::error_code ec; std::filesystem::remove_all(root, ec);
    Check(!ec && !std::filesystem::exists(root), "remove isolated authorization fixture");
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string_view(argv[1]) == "--rename-authorization") return AuthorizationRenameTest();
    // Assertions compare Simplified error text from the embedded string table.
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"rename-ops-" + std::to_wstring(GetCurrentProcessId())));
    std::filesystem::create_directories(root / L"child");
    auto make = [&](const wchar_t* name) { std::ofstream(root / name) << "fixture"; };
    if (argc > 1 && std::string_view(argv[1]) == "--rename-identity") {
        make(L"identity.txt");
        RenameIdentity identity;
        Check(CaptureRenameIdentity((root / L"identity.txt").wstring(), identity) == ERROR_SUCCESS,
            "capture source and parent identity");
        Check(MoveFileW((root / L"identity.txt").c_str(), (root / L"original.txt").c_str()) != FALSE,
            "retain original object while replacing source pathname");
        make(L"identity.txt");
        Check(RenameIdentified((root / L"identity.txt").wstring(), L"unexpected.txt", identity) == ERROR_FILE_INVALID &&
            Exact(root / L"identity.txt") && Exact(root / L"original.txt") && !Exact(root / L"unexpected.txt"),
            "replacement at authorized pathname is rejected without mutation");
        Check(CaptureRenameIdentity((root / L"identity.txt").wstring(), identity) == ERROR_SUCCESS &&
            RenameIdentified((root / L"identity.txt").wstring(), L"identified.txt", identity) == ERROR_SUCCESS &&
            Exact(root / L"identified.txt") && !Exact(root / L"identity.txt"),
            "matching identity renames through validated handle");
        make(L"occupied.txt");
        Check(CaptureRenameIdentity((root / L"identified.txt").wstring(), identity) == ERROR_SUCCESS &&
            RenameIdentified((root / L"identified.txt").wstring(), L"occupied.txt", identity) != ERROR_SUCCESS &&
            Exact(root / L"identified.txt") && Exact(root / L"occupied.txt"),
            "identified rename never replaces existing destination");
        std::filesystem::remove_all(root);
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--shell-roundtrip") {
        // New items are created in process now, so drive the Shell host's
        // request/Done stream directly.
        auto& client = pulse::ipc::ShellClient::Instance();
        std::mutex done_mutex;
        std::condition_variable done_cv;
        std::map<uint32_t, uint32_t> done;
        pulse::ipc::ShellClient::Callbacks callbacks;
        callbacks.done = [&](uint32_t id, uint32_t hr, bool, std::wstring) {
            std::lock_guard lock(done_mutex); done[id] = hr; done_cv.notify_all();
        };
        client.Start(std::move(callbacks));
        auto succeeded = [&](uint32_t id) {
            std::unique_lock lock(done_mutex);
            const bool received = id && done_cv.wait_for(lock, std::chrono::seconds(15), [&] { return done.contains(id); });
            std::printf("[INFO] shell request=%u received=%d hr=%08x\n", id, received, received ? done[id] : 0u);
            return received && SUCCEEDED(static_cast<HRESULT>(done[id]));
        };
        Check(succeeded(client.CreateNewFile((root / L"shell-created.txt").wstring())) &&
            Exact(root / L"shell-created.txt"), "normal shell CreateNewFile receives Done");
        Check(client.Ping(), "normal shell ping submitted on proven connection");
        Check(succeeded(client.CreateNewFile((root / L"shell-after-ping.txt").wstring())) &&
            Exact(root / L"shell-after-ping.txt"), "shell response stream continues after Ping");
        client.Stop();
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
    make(L"Report-two.txt");
    pulse::app::BatchRenameRule rule;
    rule.find = L"Report"; rule.replace = L"report";
    const auto preview = pulse::app::PreviewBatchRename(
        {(root / L"Report.txt").wstring(), (root / L"Report-two.txt").wstring()}, rule);
    Check(preview.size() == 2 && preview[0].status == pulse::app::BatchRenameStatus::Ok &&
        preview[1].status == pulse::app::BatchRenameStatus::Ok, "case-only batch preview enables both items");
    OpRequest case_batch; case_batch.type = OpType::BatchRename;
    for (const auto& item : preview) {
        if (item.status == pulse::app::BatchRenameStatus::Ok) {
            case_batch.sources.push_back(item.source_path);
            case_batch.new_names.push_back(item.new_name);
        }
    }
    const auto case_before = ops.Status().completed_ops;
    if (!case_batch.sources.empty()) ops.Submit(std::move(case_batch));
    Check(!preview.empty() && Wait(ops, case_before) && Exact(root / L"report.txt") &&
        Exact(root / L"report-two.txt"), "preview-selected batch changes exact disk spelling");
    if (ops.Status().completed_ops > case_before) undo();
    Check(Exact(root / L"Report.txt") && Exact(root / L"Report-two.txt"), "batch undo restores both spellings");
    rule = {};
    Check(pulse::app::PreviewBatchRename({(root / L"Report.txt").wstring()}, rule)[0].status ==
        pulse::app::BatchRenameStatus::Unchanged, "exactly identical preview stays unchanged");
    for (const auto reserved : {L"CON.txt", L"LPT1", L"COM\u00b9.log", L"NUL .txt"}) {
        rule.pattern = reserved;
        Check(pulse::app::PreviewBatchRename({(root / L"Report.txt").wstring()}, rule)[0].status ==
            pulse::app::BatchRenameStatus::Invalid, "reserved device names rejected in preview");
    }
    rule.pattern = L"{n:4294967296}";
    Check(pulse::app::ApplyBatchRenameRule(L"a.txt", 0, rule) == L"000000001",
        "large numeric width saturates without wrapping");
    rule.pattern = L"{n}"; rule.start = INT_MAX;
    Check(pulse::app::ApplyBatchRenameRule(L"a.txt", 1, rule) == L"2147483648",
        "batch numbering remains positive across 32-bit boundary");
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
    const struct { const wchar_t* name; const wchar_t* reason; } reasons[] = {
        {L"bad:name", L"不能包含以下字符"}, {L"bad?name", L"不能包含以下字符"}, {L"trailing.", L"空格或句点结尾"},
        {L"trailing ", L"空格或句点结尾"}, {L"CON.txt", L"Windows 保留的名称"}, {L"LPT1", L"Windows 保留的名称"},
        {L"", L"不能为空"}};
    for (const auto& item : reasons) {
        rename(L"stay.txt", item.name);
        Check(Exact(root / L"stay.txt") && Has(ops.Status().last_error, item.reason) &&
            Friendly(ops.Status().last_error), "invalid filename says why");
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
    Check(ops.Status().last_error == L"目标名称已存在", "existing target keeps the message the UI localizes");
    auto create = [&](OpType type, const std::filesystem::path& path) {
        const auto before = ops.Status().completed_ops;
        OpRequest request; request.type = type; request.sources = {path.wstring()};
        ops.Submit(std::move(request)); Check(Wait(ops, before), "create completes within deadline");
        return ops.Status();
    };
    {
        auto status = create(OpType::CreateFolder, root / L"新建文件夹");
        Check(status.phase == OpPhase::Completed && std::filesystem::is_directory(root / L"新建文件夹"),
            "new folder is created in process");
        status = create(OpType::CreateTextFile, root / L"新建文本文档.txt");
        Check(status.phase == OpPhase::Completed && std::filesystem::is_regular_file(root / L"新建文本文档.txt"),
            "new text file is created in process");
        status = create(OpType::CreateFolder, root / L"新建文件夹");
        Check(status.phase == OpPhase::Failed && status.last_error == L"已存在同名项目",
            "existing new-folder name fails with a reason");
        status = create(OpType::CreateFolder, root / L"missing-parent" / L"新建文件夹");
        Check(status.phase == OpPhase::Failed && Has(status.last_error, L"当前文件夹已不存在") &&
            Friendly(status.last_error), "new folder in a vanished folder says why");
    }
    make(L"locked.txt");
    HANDLE locked = CreateFileW((root / L"locked.txt").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold isolated file against deletion");
    rename(L"locked.txt", L"locked-renamed.txt");
    Check(Exact(root / L"locked.txt") && !std::filesystem::exists(root / L"locked-renamed.txt") &&
        ops.Status().phase == OpPhase::Failed && ops.Status().last_error != L"操作层未启动",
        "sharing violation reports failure without changing file");
    {
        const auto status = ops.Status();
        Check(Has(status.last_error, L"正被其他程序使用") && Friendly(status.last_error),
            "sharing violation says another program is using the file");
        bool self = false;
        for (const auto& owner : status.lock_owners) self = self || owner.pid == GetCurrentProcessId();
        std::printf("[INFO] locked rename owners=%zu path_matches=%d\n", status.lock_owners.size(),
            status.locked_path == (root / L"locked.txt").wstring());
        Check(self && status.locked_path == (root / L"locked.txt").wstring(),
            "locked rename names the program holding the file");
    }
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    ops.Stop();
    // This absolute directory was created by this process under bench_data.
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
