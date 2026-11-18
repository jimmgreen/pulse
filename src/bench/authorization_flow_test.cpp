#include "../ops/ops_manager.h"
#include <aclapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>

int main() {
    using namespace pulse::ops;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) return 2;
    const auto root = std::filesystem::path(temporary) /
        (L"pulse_authorization_flow_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    const auto target = root / L"protected";
    std::filesystem::create_directories(target);
    const auto first = root / L"first.txt", second = root / L"second.txt";
    std::ofstream(first) << "first";
    std::ofstream(second) << "second";
    const auto delete_first = target / L"delete-first.txt", delete_second = target / L"delete-second.txt";
    std::ofstream(delete_first) << "keep first";
    std::ofstream(delete_second) << "keep second";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL original = nullptr;
    if (GetNamedSecurityInfoW(target.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &original, nullptr, &descriptor) != ERROR_SUCCESS) return 2;
    BYTE world[SECURITY_MAX_SID_SIZE]{}; DWORD sid_size = sizeof(world);
    CreateWellKnownSid(WinWorldSid, nullptr, world, &sid_size);
    EXPLICIT_ACCESSW deny{};
    deny.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | DELETE | FILE_DELETE_CHILD;
    deny.grfAccessMode = DENY_ACCESS;
    deny.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world);
    PACL denied = nullptr;
    bool applied = SetEntriesInAclW(1, &deny, original, &denied) == ERROR_SUCCESS;
    if (applied) applied = SetNamedSecurityInfoW(const_cast<LPWSTR>(target.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr) == ERROR_SUCCESS;
    if (denied) LocalFree(denied);
    check(applied, "isolated destination denies writes");
    OpsManager manager;
    std::atomic<unsigned> prompts{0};
    manager.Start([&] {
        if (manager.Status().authorization == AuthorizationState::ActionRequired) ++prompts;
    });
    auto wait = [&](auto condition) {
        const auto start = GetTickCount64();
        while (!condition() && GetTickCount64() - start < 5000) Sleep(10);
        return condition();
    };
    auto finished_without_move = [&](uint64_t task) {
        std::vector<FinishedTask> finished;
        const bool arrived = wait([&] {
            if (!finished.empty()) return true;
            finished = manager.DrainFinishedTasks();
            return !finished.empty();
        });
        return arrived && finished.size() == 1 && finished.front().task_id == task &&
            finished.front().moved_sources.empty();
    };
    if (applied) {
        OpRequest request; request.sources = {first.wstring(), second.wstring()}; request.dest_dir = target.wstring();
        // Production rejects this test executable as an elevation client before
        // launching UAC, exercising the real zero-mutation failure path.
        const auto task = manager.Submit(request);
        check(wait([&] { return manager.Status().authorization == AuthorizationState::ActionRequired; }),
              "authorization failure remains in the active operation");
        const auto before = prompts.load();
        manager.ResolveAuthorization(task + 1, true);
        Sleep(30);
        check(prompts.load() == before && manager.Status().active, "stale authorization action is ignored");
        manager.ResolveAuthorization(task, true);
        check(wait([&] { return prompts.load() > before; }), "explicit retry returns to permission state without replaying files");
        manager.ResolveAuthorization(task, false);
        check(wait([&] { return manager.Status().current_item == second.wstring() &&
            manager.Status().authorization == AuthorizationState::ActionRequired; }), "skip advances only the current item");
        manager.CancelCurrent();
        check(wait([&] { return !manager.Status().active; }) && manager.Status().authorization == AuthorizationState::None,
              "cancel unblocks the permission wait");
        check(std::filesystem::exists(first) && std::filesystem::exists(second) &&
            !std::filesystem::exists(target / first.filename()) && !std::filesystem::exists(target / second.filename()) &&
            manager.DrainCompletions().empty(), "denied retry skip and cancel change no files or completion records");
        check(finished_without_move(task), "authorization cancel emits terminal identity without successful moved roots");
        const auto next = manager.Submit(request);
        check(wait([&] { return manager.Status().task_id == next && manager.Status().authorization == AuthorizationState::ActionRequired; }),
              "next task starts with fresh permission state");
        manager.ResolveAuthorization(next, false);
        wait([&] { return manager.Status().current_item == second.wstring(); });
        manager.ResolveAuthorization(next, false);
        check(wait([&] { return !manager.Status().active; }) && manager.Status().phase == OpPhase::Completed &&
            manager.Status().completed_items == 0, "skipping the selection does not fabricate completed items");
        check(finished_without_move(next), "authorization all-skip emits terminal identity without consuming sources");
        for (const auto type : {OpType::RealDelete, OpType::RecycleDelete}) {
            OpRequest denied_remove; denied_remove.type = type;
            denied_remove.sources = {delete_first.wstring(), delete_second.wstring()};
            const auto denied_task = manager.Submit(denied_remove);
            check(wait([&] { return manager.Status().task_id == denied_task && manager.Status().authorization == AuthorizationState::ActionRequired; }),
                  "protected deletion uses the existing permission state");
            manager.ResolveAuthorization(denied_task, false);
            check(wait([&] { return manager.Status().current_item == delete_second.wstring() &&
                manager.Status().authorization == AuthorizationState::ActionRequired; }), "deletion skip advances without deleting either item");
            manager.CancelCurrent();
            check(wait([&] { return !manager.Status().active; }) && std::filesystem::exists(delete_first) &&
                std::filesystem::exists(delete_second), "cancel preserves protected deletion sources");
            check(finished_without_move(denied_task), "authorization delete cancel publishes its own terminal identity");
        }
        OpRequest remove; remove.type = OpType::RealDelete; remove.sources = {first.wstring()};
        const auto deleted = manager.Submit(remove);
        check(wait([&] { return manager.Status().task_id == deleted && !manager.Status().active; }) &&
            manager.Status().phase == OpPhase::Completed && !std::filesystem::exists(first) && !manager.CanUndo(),
            "ordinary permanent deletion stays unelevated and non-undoable");
    }
    manager.Stop();
    const auto restored = SetNamedSecurityInfoW(const_cast<LPWSTR>(target.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, original, nullptr);
    LocalFree(descriptor);
    check(restored == ERROR_SUCCESS, "isolated destination permissions restored");
    std::error_code ignored;
    std::filesystem::remove(first, ignored); std::filesystem::remove(second, ignored);
    std::filesystem::remove(delete_first, ignored); std::filesystem::remove(delete_second, ignored);
    std::filesystem::remove(target, ignored); std::filesystem::remove(root, ignored);
    check(!std::filesystem::exists(root), "isolated authorization fixture removed");
    return failures ? 1 : 0;
}
