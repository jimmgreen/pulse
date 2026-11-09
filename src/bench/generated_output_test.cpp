#include "../ops/ops_manager.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
using namespace pulse::ops;
namespace fs = std::filesystem;
namespace pulse::ops {
struct OpsManagerAuditAccess {
    static void UnresolvedDeletion(OpsManager& ops, uint64_t task, const std::wstring& path) {
        std::lock_guard lock(ops.mutex_);
        for (auto& entry : ops.undo_) if (entry.task_id == task)
            entry.generated_recycle_payloads.insert_or_assign(path, std::wstring{});
    }
    static bool ReplayIdle(OpsManager& ops) {
        std::lock_guard lock(ops.mutex_);
        return !ops.history_replay_ && ops.queue_.empty() && !ops.active_item_;
    }
};
}

namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
void Put(const fs::path& path, const char* value) { fs::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary) << value; }
std::string Read(const fs::path& path) { std::ifstream f(path, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
bool Wait(OpsManager& ops, uint64_t task, ConflictChoice choice = ConflictChoice::Replace) {
    const auto end = GetTickCount64() + 20000;
    while (GetTickCount64() < end) {
        if (auto conflict = ops.PendingConflict()) ops.ResolveConflict(conflict->token, choice, true);
        const auto status = ops.QueryTask(task);
        if (status && !status->active && status->phase != OpPhase::Queued) return true;
        Sleep(5);
    }
    return false;
}
bool Replay(OpsManager& ops, bool redo) {
    const auto before = ops.UndoHistory().completed_sequence;
    if (redo) { if (!ops.Redo()) return false; } else ops.Undo();
    const auto end = GetTickCount64() + 20000;
    while (GetTickCount64() < end) {
        const auto h = ops.UndoHistory();
        if (h.completed_sequence != before) return h.completed_success;
        Sleep(5);
    }
    return false;
}
uint64_t Publish(OpsManager& ops, const fs::path& source, const fs::path& target, bool generated = true) {
    OpRequest req; req.type = OpType::Move; req.generated_output = generated;
    req.sources = {source.wstring()}; req.dest_dir = target.wstring();
    req.collision_policy = CollisionPolicy::System;
    return ops.Submit(std::move(req));
}
}
int main() {
    wchar_t temp[32768]{}; GetTempPathW(32768, temp);
    const auto root = fs::path(temp) / (L"PulseGeneratedTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto stage = root / L"stage", target = root / L"target";
    fs::create_directories(stage); fs::create_directory(target);
    OpsManager ops; ops.Start([] {});
    Put(stage / L"replace.txt", "first generation");
    auto first_generation = Publish(ops, stage / L"replace.txt", target);
    Check(Wait(ops, first_generation) && Replay(ops, false) && Replay(ops, true) && Replay(ops, false),
        "earlier publication leaves a distinct recycled generation at the same destination");
    Put(stage / L"replace.txt", "generated"); Put(target / L"replace.txt", "original");
    auto task = Publish(ops, stage / L"replace.txt", target);
    Check(Wait(ops, task) && Read(target / L"replace.txt") == "generated", "generated Replace publishes output");
    fs::remove_all(stage);
    Check(ops.UndoToJson().find(L"replace.txt") == std::wstring::npos, "session-only generated history is not serialized");
    Check(Replay(ops, false) && Read(target / L"replace.txt") == "original", "undo restores replaced original after staging cleanup");
    Check(Replay(ops, true) && Read(target / L"replace.txt") == "generated" && !fs::exists(stage), "redo restores generated output without recreating staging");
    Put(stage / L"fresh.txt", "fresh");
    task = Publish(ops, stage / L"fresh.txt", target);
    Check(Wait(ops, task), "same-volume fresh publication finishes");
    fs::remove_all(stage);
    Check(Replay(ops, false) && !fs::exists(target / L"fresh.txt"), "fresh undo removes output");
    Check(Replay(ops, true) && Read(target / L"fresh.txt") == "fresh", "fresh redo restores output from recycle");
    Check(Replay(ops, false), "prepare occupied fresh redo regression");
    Put(target / L"fresh.txt", "user replacement");
    Check(!Replay(ops, true) && Read(target / L"fresh.txt") == "user replacement" &&
        !fs::exists(target / L"fresh (2).txt"), "generated redo rejects occupied destination instead of keeping both");
    fs::remove(target / L"fresh.txt");
    Check(Replay(ops, true) && Read(target / L"fresh.txt") == "fresh", "blocked redo retains recycle payload and resumes at same request");
    Check(Replay(ops, false) && !fs::exists(target / L"fresh.txt"), "later undo still targets the actual generated destination");
    Put(stage / L"merge" / L"replace.txt", "merged-new");
    Put(stage / L"merge" / L"new-dir" / L"new.txt", "new child");
    Put(target / L"merge" / L"replace.txt", "merged-old"); Put(target / L"merge" / L"untouched.txt", "keep");
    task = Publish(ops, stage / L"merge", target);
    Check(Wait(ops, task) && Read(target / L"merge" / L"replace.txt") == "merged-new", "directory merge publishes only generated entries");
    fs::remove_all(stage);
    Check(Replay(ops, false) && Read(target / L"merge" / L"replace.txt") == "merged-old" &&
        Read(target / L"merge" / L"untouched.txt") == "keep" && !fs::exists(target / L"merge" / L"new-dir"), "merge undo preserves old content and removes new subtree");
    Check(Replay(ops, true) && Read(target / L"merge" / L"new-dir" / L"new.txt") == "new child", "merge redo preserves replay ordering");
    Put(target / L"merge" / L"new-dir" / L"user-added.txt", "user data");
    Check(Replay(ops, false) && Read(target / L"merge" / L"new-dir" / L"user-added.txt") == "user data" &&
        !fs::exists(target / L"merge" / L"new-dir" / L"new.txt"), "undo keeps new container when user added a file");
    Check(Replay(ops, true) && Read(target / L"merge" / L"new-dir" / L"user-added.txt") == "user data" &&
        Read(target / L"merge" / L"new-dir" / L"new.txt") == "new child", "redo skips retained same directory and restores generated child");
    Check(Replay(ops, false), "prepare retained directory replacement regression");
    fs::rename(target / L"merge" / L"new-dir", target / L"held-original-directory");
    Put(target / L"merge" / L"new-dir" / L"foreign.txt", "foreign data");
    Check(!Replay(ops, true) && Read(target / L"merge" / L"new-dir" / L"foreign.txt") == "foreign data" &&
        !fs::exists(target / L"merge" / L"new-dir" / L"new.txt"), "redo rejects replacement directory identity without restoring children into it");
    Put(stage / L"partial" / L"new.txt", "new"); Put(stage / L"partial" / L"skip.txt", "incoming");
    Put(target / L"partial" / L"skip.txt", "keep skip");
    task = Publish(ops, stage / L"partial", target);
    Check(Wait(ops, task, ConflictChoice::Skip) && Read(target / L"partial" / L"skip.txt") == "keep skip", "partial skip preserves existing item");
    fs::remove_all(stage);
    Check(Replay(ops, false) && !fs::exists(target / L"partial" / L"new.txt") && Read(target / L"partial" / L"skip.txt") == "keep skip", "partial undo records only completed output");
    Put(stage / L"cancel-first.txt", "first"); Put(stage / L"cancel-last.txt", "incoming");
    Put(target / L"cancel-last.txt", "untouched cancellation");
    OpRequest cancel; cancel.type = OpType::Move; cancel.generated_output = true;
    cancel.sources = {(stage / L"cancel-first.txt").wstring(), (stage / L"cancel-last.txt").wstring()};
    cancel.dest_dir = target.wstring();
    task = ops.Submit(std::move(cancel));
    Check(Wait(ops, task, ConflictChoice::Cancel) && Read(target / L"cancel-first.txt") == "first" &&
        Read(target / L"cancel-last.txt") == "untouched cancellation", "cancel preserves completed publication and untouched conflict");
    fs::remove_all(stage);
    Check(Replay(ops, false) && !fs::exists(target / L"cancel-first.txt") &&
        Read(target / L"cancel-last.txt") == "untouched cancellation", "cancelled publication undo touches only completed output");
    Put(stage / L"keep-both.txt", "incoming"); Put(target / L"keep-both.txt", "existing");
    task = Publish(ops, stage / L"keep-both.txt", target);
    Check(Wait(ops, task, ConflictChoice::KeepBoth) && Read(target / L"keep-both.txt") == "existing", "keep-both preserves existing name");
    fs::remove_all(stage);
    Check(Replay(ops, false) && Read(target / L"keep-both.txt") == "existing", "keep-both undo leaves original untouched");
    Put(stage / L"ordinary.txt", "ordinary");
    task = Publish(ops, stage / L"ordinary.txt", target, false);
    Check(Wait(ops, task) && Replay(ops, false) && Read(stage / L"ordinary.txt") == "ordinary", "ordinary Move keeps original move-back semantics");
    Put(stage / L"uncertain.txt", "generated");
    task = Publish(ops, stage / L"uncertain.txt", target);
    Check(Wait(ops, task), "prepare unresolved recycle receipt fixture");
    // Model the post-delete state where no unique new recycle receipt was found.
    fs::remove(target / L"uncertain.txt");
    OpsManagerAuditAccess::UnresolvedDeletion(ops, task, (target / L"uncertain.txt").wstring());
    Put(target / L"uncertain.txt", "user recreated file");
    Check(!ops.UndoTask(task) && !ops.UndoTo(task) && OpsManagerAuditAccess::ReplayIdle(ops) &&
        Read(target / L"uncertain.txt") == "user recreated file",
        "unresolved completed deletion blocks retries without queuing work or deleting recreated file");
    ops.Stop(); fs::remove_all(root);
    return failures ? 1 : 0;
}
