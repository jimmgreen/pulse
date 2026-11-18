#include "../ops/elevated_transfer_client.h"
#include "../common/path_utils.h"
#include "../ipc/shell_client.h"
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <aclapi.h>

namespace pulse::ops {
std::function<void(OpsManager&, const std::wstring&)> review_scan_observer;
std::function<void(OpsManager&, uint64_t)> review_start_observer;
void ObserveTransferStartForReview(OpsManager& manager, uint64_t task) {
    if (review_start_observer) review_start_observer(manager, task);
}
void ObserveTransferScanForReview(OpsManager& manager, const std::wstring& path) {
    if (review_scan_observer) review_scan_observer(manager, path);
}
std::function<void(const ipc::ShellClient::Callbacks&)> review_shell_callbacks;
void ObserveShellCallbacksForReview(const ipc::ShellClient::Callbacks& cb) {
    if (review_shell_callbacks) review_shell_callbacks(cb);
}
struct OpsManagerReviewTestAccess {
    static void CompleteInvoke(OpsManager& manager, HRESULT hr, bool cancelled, std::wstring error = {}) {
        manager.ctx_invoke_ids_.insert(991);
        manager.RecordShellDone(991, static_cast<uint32_t>(hr), cancelled, std::move(error));
    }
    static void ConfigureRouting(OpsManager& manager) {
        manager.own_shell_client_ = false;
        manager.ConfigureShellCallbacks();
    }
    static void Query(OpsManager& manager, uint32_t token) {
        OpsManager::MenuJob job; job.kind = OpsManager::MenuJob::Kind::Query;
        job.token = token; job.paths = {L"review-only-argument"};
        manager.DispatchMenuJob(job);
    }
    static void Invoke(OpsManager& manager, uint32_t token) {
        OpsManager::MenuJob job; job.kind = OpsManager::MenuJob::Kind::Invoke;
        job.token = token; job.item_id = 1; job.verb = L"review-noop";
        manager.DispatchMenuJob(job);
    }
    static bool RoutesEmpty(OpsManager& manager) {
        std::lock_guard lock(manager.menu_mutex_);
        return manager.menu_session_by_token_.empty() && manager.menu_token_by_session_.empty() && manager.ctx_invoke_ids_.empty();
    }
    static void RunMockFile(OpsManager& manager) {
        OpRequest req; req.type = OpType::CreateTextFile;
        req.sources = {L"review-only-argument-never-created.txt"};
        manager.RunShellOp(req, 12345);
    }
    static void RescueWait(OpsManager& manager) {
        { std::lock_guard lock(manager.done_mutex_); manager.stopping_ = true; }
        manager.done_cv_.notify_all();
    }
    static void PrepareRunningProgress(OpsManager& manager) {
        // RunShellOp establishes this state before accepting a file request.
        manager.SetStatus([](OpStatus& status) {
            status.active = true; status.phase = OpPhase::Running;
            status.type = OpType::CreateTextFile; status.percent = 0.0f;
        });
    }
    static bool ResultsEmpty(OpsManager& manager) {
        std::lock_guard lock(manager.done_mutex_);
        return manager.shell_request_ids_.empty() && manager.done_results_.empty();
    }
    static void Close(OpsManager& manager, uint32_t token) {
        OpsManager::MenuJob job; job.kind = OpsManager::MenuJob::Kind::Close;
        job.token = token; manager.DispatchMenuJob(job);
    }
    static void StopRoutingFixture(OpsManager& manager) {
        // Exercise post-join bookkeeping with no OS workers or helper started.
        manager.running_ = true; manager.Stop();
    }
    static void Register(OpsManager& manager, uint32_t id) { manager.RegisterShellRequest(id); }
    static bool Wait(OpsManager& manager, uint32_t id, uint32_t& hr, std::wstring& error) {
        bool cancelled = false;
        return manager.WaitShellDone(id, hr, cancelled, error) && !cancelled;
    }

    static std::wstring ExpandOnly(OpsManager& manager, const std::wstring& command, const std::wstring& path) {
        manager.open_running_ = true; // Deliberately never create OpenThread.
        manager.ExecuteCommand(command, path);
        std::lock_guard lock(manager.open_mutex_);
        std::wstring result = manager.open_queue_.empty() ? L"" : manager.open_queue_.front().open_file;
        manager.open_queue_.clear(); manager.open_running_ = false;
        return result;
    }
    static OpRequest QueuedRequest(OpsManager& manager) {
        std::lock_guard lock(manager.mutex_);
        return manager.queue_.empty() ? OpRequest{} : manager.queue_.front().req;
    }
    static void Persist(OpsManager& manager) { manager.PersistJournal(); }
    static bool RunQueuedAuthorized(OpsManager& manager, unsigned& conflicts) {
        OpRequest request;
        {
            std::lock_guard lock(manager.mutex_);
            if (manager.queue_.size() != 1) return false;
            request = manager.queue_.front().req;
            manager.queue_.pop_front();
        }
        Run(manager, request, ConflictChoice::Cancel, conflicts);
        return true;
    }
    static void RunLocal(OpsManager& manager, const OpRequest& request,
                         std::function<void(OpsManager&, const std::wstring&)> observer = {},
                         std::function<void()> notify = {}) {
        review_scan_observer = std::move(observer);
        manager.notify_ = std::move(notify);
        manager.RunTransfer(request, 2);
        manager.notify_ = {};
        review_scan_observer = {};
    }
    static void Run(OpsManager& manager, const OpRequest& request,
                    ConflictChoice choice, unsigned& conflicts) {
        manager.transfer_cancel_ = false;
        manager.transfer_pause_ = false;
        manager.transfer_active_ = true;
        manager.notify_ = [&] {
            const auto conflict = manager.PendingConflict();
            if (!conflict) return;
            ++conflicts;
            // The production callback turns this answer into transfer_cancel_,
            // unlike a test that only returns Cancel directly to the client.
            manager.ResolveConflict(conflict->token, choice, false);
        };
        manager.RunAuthorizedTransfer(request, 1);
        manager.notify_ = {};
    }
};
}

namespace {
namespace fs = std::filesystem;
class ReviewEvent {
public:
    ReviewEvent() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ReviewEvent() { if (handle_) CloseHandle(handle_); }
    void Set() const { SetEvent(handle_); }
    bool Wait(DWORD milliseconds = 10000) const {
        return handle_ && WaitForSingleObject(handle_, milliseconds) == WAIT_OBJECT_0;
    }
private:
    HANDLE handle_ = nullptr;
};
// Protected fixture ACL grants only the selected rights to the current SID;
// this tests omitted grants rather than relying on a broad deny ACE.
class FixtureAccess {
public:
    FixtureAccess(const fs::path& path, DWORD access) : path_(path) {
        if (GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &original_, nullptr, &descriptor_) != ERROR_SUCCESS) return;
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        if (!GetSecurityDescriptorControl(descriptor_, &control, &revision)) return;
        restore_flags_ = DACL_SECURITY_INFORMATION | ((control & SE_DACL_PROTECTED)
            ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
        DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> data(size);
        const BOOL obtained = GetTokenInformation(token, TokenUser, data.data(), size, &size);
        CloseHandle(token);
        if (!obtained) return;
        EXPLICIT_ACCESSW grant{}; grant.grfAccessPermissions = access;
        grant.grfAccessMode = SET_ACCESS; grant.grfInheritance = NO_INHERITANCE;
        grant.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        grant.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &grant, nullptr, &acl) != ERROR_SUCCESS) return;
        applied_ = SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
        LocalFree(acl);
    }
    ~FixtureAccess() { Restore(); if (descriptor_) LocalFree(descriptor_); }
    void Relocate(const fs::path& path) { path_ = path; }
    bool valid() const { return applied_; }
    bool Restore() {
        if (!applied_) return true;
        if (SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            restore_flags_, nullptr, nullptr, original_, nullptr) != ERROR_SUCCESS) return false;
        applied_ = false; return true;
    }
private:
    fs::path path_;
    PACL original_ = nullptr;
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_INFORMATION restore_flags_ = 0;
    bool applied_ = false;
};
class DenyDirectoryListing {
public:
    explicit DenyDirectoryListing(const fs::path& path) : path_(path) {
        if (GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &original_, nullptr, &descriptor_) != ERROR_SUCCESS) return;
        BYTE world[SECURITY_MAX_SID_SIZE]{}; DWORD bytes = sizeof(world);
        if (!CreateWellKnownSid(WinWorldSid, nullptr, world, &bytes)) return;
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_LIST_DIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &deny, original_, &acl) != ERROR_SUCCESS) return;
        applied_ = SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
        LocalFree(acl);
    }
    ~DenyDirectoryListing() { Restore(); if (descriptor_) LocalFree(descriptor_); }
    bool valid() const { return applied_; }
    bool Restore() {
        if (!applied_) return true;
        const bool restored = SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, original_, nullptr) == ERROR_SUCCESS;
        if (restored) applied_ = false;
        return restored;
    }
private:
    fs::path path_;
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    PACL original_ = nullptr;
    bool applied_ = false;
};
void Write(const fs::path& file, const char* text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
bool EqualPath(const std::wstring& actual, const fs::path& expected) {
    return pulse::path::EqualInsensitive(pulse::path::StripExtendedPathPrefix(actual), expected.wstring());
}
}

int RunReviewTransferTests() {
    using namespace pulse::ops;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    const auto root = fs::absolute(fs::path("bench_data") /
        ("review-transfer-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    DWORD retained_helper = 0;
    try {
        for (int scenario = 0; scenario < 4; ++scenario) {
            const bool move = scenario == 1 || scenario == 3;
            const bool merge = scenario == 3;
            const bool locked_failure = scenario == 2;
            const auto folder = root / std::to_string(scenario);
            const auto source = folder / "source", target = folder / "target";
            // Compose components individually so the strict comparison uses
            // native separators, including the merged-directory descendants.
            const auto source_items = merge ? source / "tree" : source;
            const auto target_items = merge ? target / "tree" : target;
            const auto a = source_items / "a-first.txt";
            const auto b = source_items / "z-stop.txt";
            const auto dest_a = target_items / "a-first.txt";
            const auto dest_b = target_items / "z-stop.txt";
            Write(a, "confirmed first item"); Write(b, "new second item"); Write(dest_b, "original destination");
            HANDLE locked = INVALID_HANDLE_VALUE;
            if (locked_failure) {
                locked = CreateFileW(dest_b.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                check(locked != INVALID_HANDLE_VALUE, "M04-013 locked second target fixture acquired");
            }
            OpsManager manager;
            OpRequest request;
            request.type = move ? OpType::Move : OpType::Copy;
            request.sources = merge ? std::vector<std::wstring>{(source / "tree").wstring()}
                                    : std::vector<std::wstring>{a.wstring(), b.wstring()};
            request.dest_dir = target.wstring();
            unsigned conflicts = 0;
            OpsManagerReviewTestAccess::Run(manager, request,
                locked_failure ? ConflictChoice::Replace : ConflictChoice::Cancel, conflicts);
            if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
            check(conflicts == 1 && manager.Status().phase == OpPhase::Failed && !manager.Status().active,
                  "M04-012/013 real OpsManager conflict callback reaches one failed/cancelled terminal state");
            check(Read(dest_a) == "confirmed first item" && Read(dest_b) == "original destination" &&
                  Read(b) == "new second item" && (move ? !fs::exists(a) : fs::exists(a)),
                  "M04-013 actual partial Copy/Move/merge preserves completed and uncompleted file contents");
            const auto completions = manager.DrainCompletions();
            size_t refreshes = 0, mappings = 0;
            bool exact = true;
            for (const auto& completion : completions) {
                if (completion.refresh_only) { ++refreshes; continue; }
                ++mappings;
                exact = exact && completion.type == request.type && completion.sources.size() == 1 &&
                    completion.destinations.size() == 1 && EqualPath(completion.sources.front(), a) &&
                    EqualPath(completion.destinations.front(), dest_a);
            }
            if (refreshes != 1 || mappings != 1 || !exact) {
                std::cout << "[DIAGNOSTIC] scenario=" << scenario << " refresh=" << refreshes << " mappings=" << mappings << std::endl;
                std::wcout << L"[DIAGNOSTIC] expected " << a.wstring() << L" -> " << dest_a.wstring() << std::endl;
                for (const auto& event : completions) {
                    if (event.refresh_only) continue;
                    for (size_t i = 0; i < event.sources.size(); ++i)
                        std::wcout << L"[DIAGNOSTIC] actual " << event.sources[i] << L" -> " <<
                            (i < event.destinations.size() ? event.destinations[i] : L"<missing>") << std::endl;
                }
            }
            check(refreshes == 1 && mappings == 1 && exact,
                  "M04-013 publishes exactly one verified mapping plus conservative refresh; never all requested sources");
            check(manager.DrainCompletions().empty() && !manager.CanUndo(),
                  "M04-013 completion is drained once and unsafe authorized undo remains disabled");
            const auto helper = ElevatedHelperProcessIdForTesting();
            if (!retained_helper) retained_helper = helper;
            check(helper != 0 && helper == retained_helper,
                  "M04-012 cancellation/error does not discard the retained helper session");
            Sleep(50); // Also exercise a result received before the host consumes any late frame.
            const auto follow = source / "follow-up.txt";
            Write(follow, "next authenticated request");
            std::atomic<bool> cancel{false};
            const auto result = TransferWithElevatedHelper({follow.wstring()}, target.wstring(), false, nullptr, cancel);
            check(SUCCEEDED(result.hr) && Read(target / "follow-up.txt") == "next authenticated request" &&
                  ElevatedHelperProcessIdForTesting() == retained_helper,
                  "M04-012 next real transfer succeeds on the same authenticated helper");
        }
        {
            const auto source = root / "zero" / "source", target = root / "zero" / "target";
            Write(source / "same.txt", "source"); Write(target / "same.txt", "destination");
            OpsManager manager; OpRequest request;
            request.sources = {(source / "same.txt").wstring()}; request.dest_dir = target.wstring();
            unsigned conflicts = 0;
            OpsManagerReviewTestAccess::Run(manager, request, ConflictChoice::Cancel, conflicts);
            const auto done = manager.DrainCompletions();
            bool no_mapping = true;
            for (const auto& item : done) no_mapping = no_mapping && item.refresh_only;
            check(conflicts == 1 && no_mapping && Read(source / "same.txt") == "source" &&
                  Read(target / "same.txt") == "destination" && !manager.CanUndo(),
                  "M04-013 cancellation before any completion never fabricates a path mapping");
        }
        for (const auto& invalid : {L"..\\escape", L"NUL.txt", L"C:\\escape", L"a:stream", L"a/b"}) {
            const auto folder = root / "invalid-named-move", source = folder / "source.txt", target = folder / "target";
            Write(source, "must not move"); fs::create_directories(target);
            OpsManager manager; OpRequest request;
            request.type = OpType::Move; request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            request.new_name = invalid;
            OpsManagerReviewTestAccess::RunLocal(manager, request);
            check(manager.Status().phase == OpPhase::Failed && Read(source) == "must not move" &&
                  fs::is_empty(target) && manager.DrainCompletions().empty(),
                  "M04-003 local named Move rejects an invalid leaf before filesystem mutation");
            std::atomic<bool> cancel{false};
            const DWORD before = ElevatedHelperProcessIdForTesting();
            const auto result = TransferWithElevatedHelper(request.sources, request.dest_dir, true, nullptr, cancel,
                ShellCollisionPolicy::System, {}, invalid);
            check(result.hr == E_INVALIDARG && !result.mutated && result.sources.empty() &&
                  before == ElevatedHelperProcessIdForTesting() && Read(source) == "must not move" && fs::is_empty(target),
                  "M04-003 client rejects an invalid named Move before submitting or replacing the helper");
        }
        for (const bool roundtrip : {false, true}) for (int scenario = 0; scenario < 9; ++scenario) {
            std::cout << "[SCENARIO] M04-003 kind=" << scenario << " json+journal=" << roundtrip << std::endl;
            const auto folder = root / (std::string("undo-") + (roundtrip ? "restored-" : "live-") + std::to_string(scenario));
            const auto source_root = folder / "source", target = folder / "target";
            const bool directory = scenario == 1, merge = scenario == 2, partial = scenario == 3;
            const auto source = directory ? source_root / "tree" : merge ? source_root / "tree" / "a.txt" : source_root / "a.txt";
            const auto conflict_target = (directory || merge) ? target / "tree" : target / "a.txt";
            if (directory) Write(source / "inside.txt", "undo payload");
            else Write(source, "undo payload");
            Write(merge ? conflict_target / "a.txt" : conflict_target, "original target must remain");
            OpRequest request; request.type = OpType::Move; request.dest_dir = target.wstring();
            request.sources = {(merge ? source.parent_path() : source).wstring()};
            if (partial) {
                Write(source_root / "z.txt", "uncompleted source"); Write(target / "z.txt", "uncompleted target");
                request.sources.push_back((source_root / "z.txt").wstring());
            }
            OpsManager manager;
            unsigned forward_conflicts = 0;
            OpsManagerReviewTestAccess::RunLocal(manager, request, {}, [&] {
                if (const auto conflict = manager.PendingConflict())
                    manager.ResolveConflict(conflict->token,
                        partial && ++forward_conflicts == 2 ? ConflictChoice::Cancel : ConflictChoice::KeepBoth, false);
            });
            const auto forward = manager.DrainCompletions();
            const bool fixture_ok = forward.size() == 1 && forward[0].sources.size() == 1 &&
                forward[0].destinations.size() == 1 && EqualPath(forward[0].sources[0], source) && manager.CanUndo();
            check(fixture_ok, "M04-003 real KeepBoth Move records one exact undo mapping including merged/partial work");
            if (!fixture_ok) continue;
            const fs::path moved(forward[0].destinations[0]);
            check(!fs::exists(source) && moved.filename() != source.filename() &&
                  Read(directory ? moved / "inside.txt" : moved) == "undo payload",
                  "M04-003 forward KeepBoth really changes the destination name");
            OpsManager restored, recovered;
            OpsManager& undo_manager = roundtrip ? restored : manager;
            check(!roundtrip || restored.UndoFromJson(manager.UndoToJson()), "M04-003 exact undo mapping survives session JSON round trip");
            if (scenario >= 4 && scenario <= 7) Write(source, "recreated source must be protected");
            const auto journal = folder / "undo-journal.json";
            if (roundtrip) undo_manager.SetJournalPath(journal.wstring());
            undo_manager.Undo();
            const auto inverse = OpsManagerReviewTestAccess::QueuedRequest(undo_manager);
            check(inverse.is_undo && inverse.type == OpType::Move && inverse.new_name == source.filename().wstring() &&
                  inverse.sources.size() == 1 && EqualPath(inverse.sources[0], moved) && EqualPath(inverse.dest_dir, source.parent_path()),
                  "M04-003 Undo enqueues the original leaf name, not the generated KeepBoth basename");
            if (roundtrip) {
                OpsManagerReviewTestAccess::Persist(undo_manager);
                recovered.SetJournalPath(journal.wstring());
                const auto recovery = recovered.PendingRecovery();
                check(recovery.entries.size() == 1 && recovery.entries[0].request.new_name == source.filename().wstring() &&
                      recovery.entries[0].request.is_undo && recovered.RetryRecovery(),
                      "M04-003 queued exact-name undo survives journal write/read/retry");
            }
            OpsManager& engine = roundtrip ? recovered : undo_manager;
            unsigned inverse_conflicts = 0;
            const auto choice = scenario == 4 ? ConflictChoice::Cancel : scenario == 5 ? ConflictChoice::Skip
                : scenario == 6 ? ConflictChoice::KeepBoth : scenario == 7 ? ConflictChoice::Replace : ConflictChoice::Cancel;
            const auto completed_before = engine.Status().completed_ops;
            if (scenario == 8) {
                check(OpsManagerReviewTestAccess::RunQueuedAuthorized(engine, inverse_conflicts),
                      "M04-003 real undo request traverses the authorized client/host named-transfer protocol");
            } else {
                ReviewEvent finished;
                engine.Start([&] {
                    if (const auto conflict = engine.PendingConflict()) {
                        ++inverse_conflicts; engine.ResolveConflict(conflict->token, choice, false);
                    }
                    const auto status = engine.Status();
                    if (!status.active && status.completed_ops == completed_before + 1) finished.Set();
                }, false);
                const bool ended = finished.Wait();
                engine.Stop();
                check(ended, "M04-003 exact-name undo reaches a terminal state on the real worker");
            }
            const auto done = engine.DrainCompletions();
            if (scenario == 4 || scenario == 5) {
                check(inverse_conflicts == 1 && Read(source) == "recreated source must be protected" &&
                      Read(moved) == "undo payload" && done.empty() &&
                      engine.Status().phase == (scenario == 4 ? OpPhase::Failed : OpPhase::Completed),
                      "M04-003 undo conflict Cancel/Skip never overwrites the recreated original path");
            } else if (scenario == 6) {
                check(inverse_conflicts == 1 && Read(source) == "recreated source must be protected" && !fs::exists(moved) &&
                      done.size() == 1 && done[0].destinations.size() == 1 && !EqualPath(done[0].destinations[0], source) &&
                      Read(fs::path(done[0].destinations[0])) == "undo payload",
                      "M04-003 explicit KeepBoth on undo preserves recreated content and publishes the chosen alternate path");
            } else {
                check(engine.Status().phase == OpPhase::Completed && !fs::exists(moved) &&
                      Read(directory ? source / "inside.txt" : source) == "undo payload" &&
                      done.size() == 1 && done[0].sources.size() == 1 && done[0].destinations.size() == 1 &&
                      EqualPath(done[0].sources[0], moved) && EqualPath(done[0].destinations[0], source) &&
                      inverse_conflicts == (scenario == 7 ? 1u : 0u),
                      "M04-003 undo restores the exact original absolute path and content, with exact completion mapping");
            }
            check(Read(merge ? conflict_target / "a.txt" : conflict_target) == "original target must remain" &&
                  (!partial || (Read(source_root / "z.txt") == "uncompleted source" && Read(target / "z.txt") == "uncompleted target")),
                  "M04-003 undo leaves the old target conflict and uncompleted items untouched");
        }
        {
            const auto folder = root / "undo-multi-leaf", source = folder / "source" / "tree", target = folder / "target";
            Write(source / "a.txt", "new a"); Write(source / "b.txt", "new b");
            Write(target / "tree" / "a.txt", "old a"); Write(target / "tree" / "b.txt", "old b");
            OpsManager manager; OpRequest request;
            request.type = OpType::Move; request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            request.collision_policy = CollisionPolicy::KeepBoth;
            OpsManagerReviewTestAccess::RunLocal(manager, request);
            const auto forward = manager.DrainCompletions();
            const bool fixture_ok = manager.Status().phase == OpPhase::Completed && forward.size() == 1 &&
                forward[0].sources.size() == 2 && forward[0].destinations.size() == 2 && !fs::exists(source);
            check(fixture_ok, "M04-003 merged multi-leaf KeepBoth records each moved leaf and removes the empty source tree");
            if (fixture_ok) {
                OpsManager restored, recovered;
                check(restored.UndoFromJson(manager.UndoToJson()), "M04-003 multi-leaf undo session JSON round trip");
                const auto journal = folder / "journal.json";
                restored.SetJournalPath(journal.wstring()); restored.Undo();
                OpsManagerReviewTestAccess::Persist(restored);
                recovered.SetJournalPath(journal.wstring());
                const auto pending = recovered.PendingRecovery();
                check(pending.entries.size() == 2 && pending.entries[0].request.new_name != pending.entries[1].request.new_name &&
                      recovered.RetryRecovery(), "M04-003 distinct original leaf names survive two queued journal entries");
                ReviewEvent done;
                recovered.Start([&] {
                    const auto status = recovered.Status();
                    if (!status.active && status.completed_ops == 2) done.Set();
                }, false);
                const bool ended = done.Wait(); recovered.Stop();
                check(ended && recovered.Status().phase == OpPhase::Completed && Read(source / "a.txt") == "new a" &&
                      Read(source / "b.txt") == "new b" && Read(target / "tree" / "a.txt") == "old a" &&
                      Read(target / "tree" / "b.txt") == "old b", "M04-003 both original paths and contents restored while old target conflicts remain");
                const auto completed = recovered.DrainCompletions();
                bool exact = completed.size() == 2;
                for (const auto& item : completed) {
                    bool matched = false;
                    if (item.sources.size() == 1 && item.destinations.size() == 1)
                        for (size_t i = 0; i < forward[0].sources.size(); ++i)
                            matched = matched || (EqualPath(item.sources[0], fs::path(forward[0].destinations[i])) &&
                                EqualPath(item.destinations[0], fs::path(forward[0].sources[i])));
                    exact = exact && matched;
                }
                check(exact, "M04-003 each recovered completion reverses its exact forward mapping");
            }
        }
        for (const bool move : {false, true}) for (int scenario = 0; scenario < 8; ++scenario) {
            std::cout << "[SCENARIO] M04-011 " << (move ? "Move" : "Copy") << " barrier=" << scenario << std::endl;
            const auto folder = root / (std::string("startup-") + (move ? "move-" : "copy-") + std::to_string(scenario));
            const auto source = folder / "first.txt", target = folder / "target";
            Write(source, "request-scoped control"); fs::create_directories(target);
            ReviewEvent entered, release_start, initialized, release_initialized, paused, finished, next_finished;
            std::atomic<bool> barrier_ok{true};
            OpsManager manager; OpRequest request;
            request.type = move ? OpType::Move : OpType::Copy;
            request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            const auto task = manager.Submit(request);
            // Queue-owned commands arrive before any worker exists.
            if (scenario == 0) { manager.CancelCurrent(); manager.PauseCurrent(); manager.ResumeCurrent(); }
            if (scenario == 1 || scenario == 5) manager.PauseCurrent();
            if (scenario == 5) manager.ResumeCurrent();
            review_start_observer = [&](OpsManager&, uint64_t id) {
                if (id != task) return;
                entered.Set();
                if (!release_start.Wait()) barrier_ok = false;
            };
            std::atomic<bool> scan_barrier_used{false};
            manager.Start([&] {
                const auto status = manager.Status();
                if (status.task_id == task && status.phase == OpPhase::Scanning && scenario >= 6 &&
                    !scan_barrier_used.exchange(true)) {
                    initialized.Set();
                    if (!release_initialized.Wait()) barrier_ok = false;
                }
                if (status.task_id == task && status.phase == OpPhase::Paused) paused.Set();
                if (!status.active && status.task_id == task && status.completed_ops == 1) finished.Set();
                if (!status.active && status.completed_ops == 2) next_finished.Set();
            }, false); // Real worker, no ShellClient/open/menu threads and no journal path.
            check(task != 0 && entered.Wait(), "M04-011 real worker held after Submit and before engine initialization");
            if (scenario == 2) manager.CancelCurrent();
            if (scenario == 3 || scenario == 4) manager.PauseCurrent();
            release_start.Set();
            if (scenario >= 6) {
                check(initialized.Wait(), "M04-011 initialized engine held before first filesystem work");
                if (scenario == 6) manager.CancelCurrent();
                else manager.PauseCurrent();
                release_initialized.Set();
            }
            if (scenario == 1 || scenario == 3 || scenario == 4 || scenario == 7) {
                check(paused.Wait() && !finished.Wait(150) && Read(source) == "request-scoped control" &&
                      !fs::exists(target / "first.txt"),
                      "M04-011 accepted pause survives startup and no filesystem mutation precedes Resume");
                if (scenario == 4) manager.CancelCurrent();
                else manager.ResumeCurrent();
            }
            const bool terminal = finished.Wait();
            const bool cancel = scenario == 0 || scenario == 2 || scenario == 4 || scenario == 6;
            const auto status = manager.Status();
            check(terminal && barrier_ok.load() && !status.active && status.task_id == task && status.completed_ops == 1 &&
                  status.phase == (cancel ? OpPhase::Failed : OpPhase::Completed),
                  "M04-011 request reaches exactly one terminal result with the intended control outcome");
            const auto done = manager.DrainCompletions();
            check(cancel ? (Read(source) == "request-scoped control" && !fs::exists(target / "first.txt") &&
                            done.empty() && !manager.CanUndo())
                         : (Read(target / "first.txt") == "request-scoped control" && (move ? !fs::exists(source) : fs::exists(source)) &&
                            done.size() == 1 && done[0].sources.size() == 1 && done[0].destinations.size() == 1 &&
                            EqualPath(done[0].sources[0], source) && EqualPath(done[0].destinations[0], target / "first.txt")),
                  "M04-011 cancelled request changes nothing; resumed request publishes exact paths");
            const auto next_source = folder / "next.txt";
            Write(next_source, "next request is independent"); request.sources = {next_source.wstring()};
            const auto next_task = manager.Submit(request);
            check(next_finished.Wait() && manager.Status().task_id == next_task &&
                  manager.Status().phase == OpPhase::Completed && Read(target / "next.txt") == "next request is independent",
                  "M04-011 following request inherits neither cancellation nor pause");
            manager.Stop();
            review_start_observer = {};
        }
        for (unsigned allowed = 0; allowed < 4; ++allowed) {
            const bool files = (allowed & 1) != 0, directories = (allowed & 2) != 0;
            const auto folder = root / ("acl-" + std::to_string(allowed));
            const auto target = folder / "target", file = folder / "file.txt", tree = folder / "empty";
            Write(file, "minimum access copy"); fs::create_directories(tree);
            fs::create_directories(target / "merged");
            fs::create_directories(folder / "merged");
            DWORD rights = FILE_ALL_ACCESS & ~(FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
            if (files) rights |= FILE_ADD_FILE;
            if (directories) rights |= FILE_ADD_SUBDIRECTORY;
            FixtureAccess acl(target, rights);
            check(acl.valid(), "M04-014 isolated ACL with independent file/directory creation grants");
            if (!acl.valid()) continue;
            check(NeedsShellTransfer({file.wstring()}, target.wstring(), false) == !files,
                  "M04-014 ordinary file probe requests only FILE_ADD_FILE");
            check(NeedsShellTransfer({tree.wstring()}, target.wstring(), false) == !directories,
                  "M04-014 new directory probe requests only FILE_ADD_SUBDIRECTORY");
            check(NeedsShellTransfer({file.wstring(), tree.wstring()}, target.wstring(), false) == !(files && directories),
                  "M04-014 mixed roots require both creation rights");
            check(NeedsShellTransfer({file.wstring()}, (target / "missing" / "nested").wstring(), false) == !directories,
                  "M04-014 missing ancestors require directory creation at the existing ancestor");
            check(!NeedsShellTransfer({(folder / "merged").wstring()}, target.wstring(), false),
                  "M04-014 existing directory merge does not require a new parent entry");
            if (files) {
                OpsManager manager; OpRequest request;
                request.type = OpType::Copy; request.sources = {file.wstring()}; request.dest_dir = target.wstring();
                OpsManagerReviewTestAccess::RunLocal(manager, request);
                check(manager.Status().phase == OpPhase::Completed && Read(target / "file.txt") == "minimum access copy",
                      "M04-014 actual current-token file copy succeeds without directory creation permission");
                Write(file, "replacement"); request.collision_policy = CollisionPolicy::Replace;
                OpsManagerReviewTestAccess::RunLocal(manager, request);
                check(manager.Status().phase == OpPhase::Completed && Read(target / "file.txt") == "replacement",
                      "M04-014 file replacement also works without directory creation permission");
            }
            if (directories) {
                OpsManager manager; OpRequest request;
                request.type = OpType::Copy; request.sources = {tree.wstring()}; request.dest_dir = target.wstring();
                OpsManagerReviewTestAccess::RunLocal(manager, request);
                check(manager.Status().phase == OpPhase::Completed && fs::is_directory(target / "empty"),
                      "M04-014 actual empty-directory copy succeeds without file creation permission");
            }
            check(acl.Restore(), "M04-014 target fixture ACL restored");
        }
        for (unsigned allowed = 0; allowed < 4; ++allowed) {
            const bool direct_delete = (allowed & 1) != 0, parent_delete = (allowed & 2) != 0;
            const auto folder = root / ("move-acl-" + std::to_string(allowed));
            const auto parent = folder / "source", target = folder / "target", file = parent / "file.txt";
            Write(file, "move by either delete permission"); fs::create_directories(target);
            FixtureAccess source_acl(file, direct_delete ? FILE_ALL_ACCESS : FILE_ALL_ACCESS & ~DELETE);
            FixtureAccess parent_acl(parent, parent_delete ? FILE_ALL_ACCESS : FILE_ALL_ACCESS & ~FILE_DELETE_CHILD);
            check(source_acl.valid() && parent_acl.valid(), "M04-014 isolated Move delete-rights matrix acquired");
            if (!source_acl.valid() || !parent_acl.valid()) continue;
            check(NeedsShellTransfer({file.wstring()}, target.wstring(), true) == !(direct_delete || parent_delete),
                  "M04-014 Move accepts DELETE or parent FILE_DELETE_CHILD, but rejects neither");
            if (direct_delete || parent_delete) {
                OpsManager manager; OpRequest request;
                request.type = OpType::Move; request.sources = {file.wstring()}; request.dest_dir = target.wstring();
                OpsManagerReviewTestAccess::RunLocal(manager, request);
                const bool moved = !fs::exists(file) && fs::exists(target / "file.txt");
                if (moved) source_acl.Relocate(target / "file.txt");
                check(manager.Status().phase == OpPhase::Completed && moved &&
                      Read(target / "file.txt") == "move by either delete permission",
                      "M04-014 actual current-token Move succeeds with either deletion grant");
            }
            check(source_acl.Restore() && parent_acl.Restore(), "M04-014 Move fixture ACLs restored");
        }
        for (const bool move : {false, true}) {
            const auto folder = root / (move ? "scan-move" : "scan-copy");
            const auto source = folder / "source" / "tree", target = folder / "target";
            const auto blocked = source / "blocked";
            Write(blocked / "secret.txt", "must remain at source");
            fs::create_directories(target / "tree"); // Move must exercise merge, not atomic root rename.
            DenyDirectoryListing deny(blocked);
            check(deny.valid(), "M04-002 isolated child directory denies enumeration");
            OpsManager manager; OpRequest request;
            request.type = move ? OpType::Move : OpType::Copy;
            request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            OpsManagerReviewTestAccess::RunLocal(manager, request);
            check(manager.Status().phase == OpPhase::Failed && !manager.Status().last_error.empty() &&
                  manager.DrainCompletions().empty() && !fs::exists(target / "tree" / "blocked"),
                  "M04-002 Copy/merged Move cannot report success after descendant enumeration failure");
            check(deny.Restore() && Read(blocked / "secret.txt") == "must remain at source",
                  "M04-002 failed scan preserves source payload and fixture ACL is restored");
        }
        {
            const auto folder = root / "cancel-single", target = folder / "target";
            const auto source = folder / "source.txt";
            Write(source, "do not move"); fs::create_directories(target);
            OpsManager manager; OpRequest request;
            request.type = OpType::Move; request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            OpsManagerReviewTestAccess::RunLocal(manager, request, {}, [&] {
                if (manager.Status().phase == OpPhase::Scanning) manager.CancelCurrent();
            });
            check(manager.Status().phase == OpPhase::Failed && fs::exists(source) &&
                  !fs::exists(target / "source.txt") && manager.DrainCompletions().empty(),
                  "M04-008 cancel observed at scanning prevents the single-root atomic move");
        }
        {
            const auto folder = root / "cancel-next", target = folder / "target";
            const auto first = folder / "first.txt", second = folder / "second.txt";
            Write(first, "already done"); Write(second, "must stay"); fs::create_directories(target);
            OpsManager manager; OpRequest request;
            request.type = OpType::Move; request.sources = {first.wstring(), second.wstring()}; request.dest_dir = target.wstring();
            OpsManagerReviewTestAccess::RunLocal(manager, request, [&](OpsManager& active, const auto& path) {
                if (EqualPath(path, second)) active.CancelCurrent();
            });
            const auto completed = manager.DrainCompletions();
            check(manager.Status().phase == OpPhase::Failed && !fs::exists(first) &&
                  Read(target / "first.txt") == "already done" && Read(second) == "must stay" &&
                  !fs::exists(target / "second.txt"),
                  "M04-008 cancellation after first root does not start a second atomic move");
            check(completed.size() == 1 && completed[0].sources.size() == 1 &&
                  EqualPath(completed[0].sources[0], first) && manager.CanUndo(),
                  "M04-008 already completed root remains in exact completion and undo records");
        }
        {
            const auto folder = root / "cancel-enumeration", source = folder / "tree", target = folder / "target";
            fs::create_directories(target);
            for (int i = 0; i < 100; ++i) Write(source / (std::to_string(i) + ".txt"), "untouched");
            OpsManager manager; OpRequest request;
            request.type = OpType::Copy; request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            unsigned observed = 0;
            OpsManagerReviewTestAccess::RunLocal(manager, request, [&](OpsManager& active, const auto&) {
                if (++observed == 3) active.CancelCurrent();
            });
            check(observed == 3 && manager.Status().phase == OpPhase::Failed &&
                  !fs::exists(target / "tree") && manager.DrainCompletions().empty(),
                  "M04-008 recursive enumeration stops at cancellation without creating a destination");
        }
        {
            const auto folder = root / "late-source", source = folder / "tree", target = folder / "target";
            Write(source / "file.txt", "moved content"); fs::create_directories(target / "tree");
            OpsManager manager; OpRequest request;
            request.type = OpType::Move; request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            bool injected = false;
            OpsManagerReviewTestAccess::RunLocal(manager, request, {}, [&] {
                const auto status = manager.Status();
                if (!injected && status.phase == OpPhase::Running && status.total_items &&
                    status.completed_items == status.total_items) {
                    injected = true; Write(source / "late.txt", "new source item");
                }
            });
            check(injected && manager.Status().phase == OpPhase::Failed && !manager.Status().last_error.empty() &&
                  Read(target / "tree" / "file.txt") == "moved content" && Read(source / "late.txt") == "new source item",
                  "M04-002 source-directory removal failure is reported, not silently completed");
            const auto done = manager.DrainCompletions();
            check(done.size() == 1 && done[0].sources.size() == 1 && EqualPath(done[0].sources[0], source / "file.txt"),
                  "M04-002 late source item is not included in completed mappings");
        }
        {
            const auto folder = root / "intentional-skip", source = folder / "tree", target = folder / "target";
            Write(source / "same.txt", "source kept"); Write(target / "tree" / "same.txt", "destination kept");
            OpsManager manager; OpRequest request;
            request.type = OpType::Move; request.sources = {source.wstring()}; request.dest_dir = target.wstring();
            OpsManagerReviewTestAccess::RunLocal(manager, request, {}, [&] {
                if (const auto conflict = manager.PendingConflict())
                    manager.ResolveConflict(conflict->token, ConflictChoice::Skip, false);
            });
            check(manager.Status().phase == OpPhase::Completed && Read(source / "same.txt") == "source kept" &&
                  Read(target / "tree" / "same.txt") == "destination kept",
                  "M04-002 explicitly skipped contents may keep a source directory without a false failure");
        }
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    HANDLE helper = retained_helper ? OpenProcess(SYNCHRONIZE, FALSE, retained_helper) : nullptr;
    ShutdownElevatedTransferHelper();
    check(helper && WaitForSingleObject(helper, 10000) == WAIT_OBJECT_0,
          "isolated non-elevated test helper shuts down after regression");
    if (helper) CloseHandle(helper);
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "only this test's unique fixture directory is cleaned up");
    return failures ? 1 : 0;
}
int RunReviewTemplateTests() {
    using namespace pulse::ops;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl; failures += !ok;
    };
    const auto root = fs::absolute(fs::path("bench_data") /
        ("review-template-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    try {
        fs::create_directories(root);
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
        const auto child = root / "argv recorder.exe";
        fs::copy_file(fs::path(module), child);
        const std::vector<std::wstring> values{
            L"C:\\docs\\%L report.txt", L"C:\\docs\\%1-%L-%l-%V-%v-%*.txt",
            L"C:\\资料 folder\\百分号%L.txt", L"C:\\", L"C:\\folder with spaces\\",
            L"\\\\server\\share\\", L"\\\\?\\C:\\资料 folder\\%v.txt",
            L"\\\\?\\UNC\\server\\share\\%L folder\\"};
        unsigned index = 0;
        for (const auto& path : values) {
            const auto expected = pulse::path::StripExtendedPathPrefix(path);
            OpsManager manager;
            // An independent expected command for the simple no-trailing-slash regression.
            if (index == 0) {
                const auto expanded = OpsManagerReviewTestAccess::ExpandOnly(manager, L"viewer.exe \"%1\"", path);
                check(expanded == L"viewer.exe \"C:\\docs\\%L report.txt\"",
                      "M04-010 inserted percent-placeholder text remains literal in the exact expanded command");
            }
            const auto marker = root / (std::to_string(index++) + ".bin");
            const auto command = L"\"" + child.wstring() + L"\" --template-child \"" + marker.wstring() +
                L"\" \"%1\" %L \"%l\" %V \"%v\" \"%*\" %1 %1 %Q";
            auto expanded = OpsManagerReviewTestAccess::ExpandOnly(manager, command, path);
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
            // Explicit image prevents broken quoting from selecting another executable.
            const bool started = CreateProcessW(child.c_str(), expanded.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process) != FALSE;
            bool ended = false; DWORD exit = 2;
            if (started) {
                ended = WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0;
                if (!ended) { TerminateProcess(process.hProcess, ERROR_TIMEOUT); WaitForSingleObject(process.hProcess, 5000); }
                GetExitCodeProcess(process.hProcess, &exit);
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
            }
            check(started && ended && exit == 0, "M04-010 isolated argv recorder launches and exits without opening the selected path");
            std::ifstream input(marker, std::ios::binary);
            uint32_t count = 0; input.read(reinterpret_cast<char*>(&count), sizeof(count));
            bool exact = input.good() && count == 9;
            for (uint32_t i = 0; exact && i < count; ++i) {
                uint32_t size = 0; input.read(reinterpret_cast<char*>(&size), sizeof(size));
                if (!input || size > 32768) { exact = false; break; }
                std::wstring value(size, L'\0');
                input.read(reinterpret_cast<char*>(value.data()), size * sizeof(wchar_t));
                exact = input.good() && value == (i == 8 ? L"%Q" : expected);
            }
            check(exact, "M04-010 real Windows argv preserves literal tokens, Unicode, spaces and trailing separators for every placeholder");
        }
        OpsManager manager;
        check(OpsManagerReviewTestAccess::ExpandOnly(manager, L"viewer.exe --literal %% %Q", L"C:\\unused.txt") ==
              L"viewer.exe --literal %% %Q", "M04-010 unrecognized percent text and templates without placeholders stay unchanged");
    } catch (const std::exception& error) { check(false, error.what()); }
    std::error_code error; fs::remove_all(root, error);
    check(!error && !fs::exists(root), "M04-010 only the isolated argv recorder and its output fixtures are cleaned up");
    return failures ? 1 : 0;
}
namespace pulse::ipc {
std::function<void(ShellClient&, uint32_t, uint32_t)> review_submit;
bool InterceptShellSubmitForReview(ShellClient& client, uint32_t type, uint32_t id) {
    if (!review_submit) return false;
    review_submit(client, type, id);
    return true;
}
struct ShellClientReviewTestAccess {
    static bool IsIsolated(ShellClient& client) {
        return !client.reader_.joinable() && !client.connection_ && !client.child_started_ && client.pending_.empty();
    }
    static void Configure(ShellClient& client, const ShellClient::Callbacks& cb) {
        client.cb_ = cb; client.running_ = true;
    }
    static void Reset(ShellClient& client) { client.running_ = false; client.cb_ = {}; }
    static void SetNextId(ShellClient& client, uint32_t id) { client.next_id_ = id; }
    static void Progress(ShellClient& client, uint32_t id, const std::wstring& item) {
        if (client.cb_.progress) client.cb_.progress(id, 50.0f, item, 1, 2);
    }
    static void NotRunning(ShellClient& client) { client.running_ = false; }
    static void Done(ShellClient& client, uint32_t id, HRESULT hr = S_OK, const std::wstring& error = {}) {
        client.FireDone(id, static_cast<uint32_t>(hr), false, error);
    }
    static void Items(ShellClient& client, uint32_t id, bool partial = false) {
        CtxMenuItem item; item.id = 1; item.text = L"Review no-op"; item.verb = L"review-noop";
        if (client.cb_.ctx_items) client.cb_.ctx_items(id, {item}, partial, {});
    }
};
}
namespace {
struct RoutingFixture {
    pulse::ops::OpsManager manager;
    pulse::ipc::ShellClient& client = pulse::ipc::ShellClient::Instance();
    RoutingFixture() {
        using namespace pulse;
        if (!ipc::ShellClientReviewTestAccess::IsIsolated(client)) ExitProcess(2);
        ops::review_shell_callbacks = [&](const ipc::ShellClient::Callbacks& cb) {
            ipc::ShellClientReviewTestAccess::Configure(client, cb);
        };
        ops::OpsManagerReviewTestAccess::ConfigureRouting(manager);
        ops::review_shell_callbacks = {};
    }
    ~RoutingFixture() {
        pulse::ipc::review_submit = {};
        pulse::ipc::ShellClientReviewTestAccess::Reset(client);
    }
};
}
int RunReviewRoutingTests(bool invoke_only = false) {
    using namespace pulse;
    using A = ops::OpsManagerReviewTestAccess;
    using C = ipc::ShellClientReviewTestAccess;
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << text << '\n';
        if (!ok) ++failures;
    };
    {
        ops::OpsManager manager;
        A::CompleteInvoke(manager, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), false);
        const auto message = manager.TakeShellVerbError();
        wchar_t expected[512]{};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            ERROR_ACCESS_DENIED, 0, expected, 512, nullptr);
        std::wstring detail(expected);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n')) detail.pop_back();
        check(!detail.empty() && message.find(detail) != std::wstring::npos, "invoke access denied reaches UI with decoded Win32 error");
        check(manager.TakeShellVerbError().empty() && manager.TakeCtxInvokeDone(), "invoke error consumed once and refresh retained");
        A::CompleteInvoke(manager, E_FAIL, true);
        check(manager.TakeShellVerbError().empty(), "cancelled invoke stays silent");
        A::CompleteInvoke(manager, S_OK, false);
        check(manager.TakeShellVerbError().empty(), "successful invoke stays silent");
        A::CompleteInvoke(manager, E_FAIL, false, L"provider failure");
        check(manager.TakeShellVerbError().find(L"0x80004005") != std::wstring::npos, "invoke failure exposes HRESULT even with generic provider text");
    }
    if (invoke_only) return failures ? 1 : 0;
    std::cout << "[INFO] production dispatch/Submit/callback/wait paths with deterministic in-process transport; no host or filesystem action\n";
    {
        RoutingFixture f;
        unsigned callbacks = 0; uint32_t received_token = 0;
        f.manager.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem> items, bool partial, ops::ShellMenuAnswer) {
            if (!partial && items.size() == 1) { ++callbacks; received_token = token; }
        });
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t type, uint32_t id) {
            if (type == ipc::REQ_CTX_QUERY) {
                // Force completion on another Windows thread before Submit returns.
                std::thread response([&] { C::Items(client, id); }); response.join();
            } else C::Done(client, id, HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED), L"injected send failure");
        };
        A::Query(f.manager, 101);
        check(callbacks == 1 && received_token == 101, "M04-001 immediate query callback is delivered to the correct token exactly once");
        A::Invoke(f.manager, 101);
        check(f.manager.TakeCtxInvokeDone(), "M04-001 synchronous invoke failure is routed as a menu completion");
        check(!f.manager.TakeCtxInvokeDone(), "M04-001 menu completion notification is consumed only once");
        check(A::RoutesEmpty(f.manager), "M04-001 completed invoke leaves no stale in-flight route");
        check(f.manager.TryPrepareForUpdate(), "M04-001 synchronous invoke failure does not leave update preparation permanently busy");
        f.manager.CancelUpdatePreparation();
        check(C::IsIsolated(f.client), "M04-001 routing tests never start a reader, pipe or helper");
    }
    {
        RoutingFixture f;
        unsigned terminal = 0; uint32_t token_seen = 0;
        f.manager.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem> items, bool partial, ops::ShellMenuAnswer) {
            if (!partial && items.empty()) { ++terminal; token_seen = token; }
        });
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t, uint32_t id) {
            C::Done(client, id, E_FAIL, L"injected query failure");
        };
        A::Query(f.manager, 102);
        check(terminal == 1 && token_seen == 102, "M04-001 query RSP_DONE failure terminates its UI token exactly once");
        check(A::RoutesEmpty(f.manager), "M04-001 failed query leaves no session mappings");
    }
    {
        RoutingFixture f;
        unsigned terminal = 0;
        f.manager.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem> items, bool partial, ops::ShellMenuAnswer) {
            if (token == 103 && !partial && items.empty()) ++terminal;
        });
        C::NotRunning(f.client);
        A::Query(f.manager, 103);
        check(terminal == 1 && A::RoutesEmpty(f.manager), "M04-001 rejected zero-id query ends locally without a stale route or callback wait");
    }
    {
        RoutingFixture f;
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t type, uint32_t id) {
            if (type == ipc::REQ_CTX_QUERY) C::Items(client, id);
            else if (type == ipc::REQ_NEW_FILE) {
                C::Done(client, id); // terminal file result first
                A::Invoke(f.manager, 104); // competing menu failure before file Submit returns
            } else C::Done(client, id, E_FAIL, L"injected later menu failure");
        };
        A::Query(f.manager, 104);
        auto operation = std::async(std::launch::async, [&] { A::RunMockFile(f.manager); });
        const bool timely = operation.wait_for(std::chrono::milliseconds(600)) == std::future_status::ready;
        check(timely, "M04-001 file DONE survives later menu DONE without waiting for the inactivity timeout");
        if (!timely) A::RescueWait(f.manager);
        if (operation.wait_for(std::chrono::seconds(3)) != std::future_status::ready) ExitProcess(2);
        operation.get();
        const auto status = f.manager.Status();
        check(status.phase == ops::OpPhase::Completed && status.completed_ops == 1,
              "M04-001 real RunShellOp consumes its own successful result exactly once");
        check(f.manager.TakeCtxInvokeDone() && A::RoutesEmpty(f.manager),
              "M04-001 competing menu failure remains independently completed and retired");
        check(C::IsIsolated(f.client), "M04-001 fake file request produces no actual helper or file operation");
    }
    {
        RoutingFixture f;
        A::Register(f.manager, 70001); A::Register(f.manager, 70002);
        C::Done(f.client, 70001, E_ACCESSDENIED, L"first owned result");
        C::Done(f.client, 70002, S_OK, L"second owned result");
        C::Done(f.client, 99999, E_FAIL, L"unowned result must be ignored");
        C::Done(f.client, 70001, S_OK, L"duplicate must not replace first terminal");
        uint32_t first_hr = 0; std::wstring first_error;
        auto first = std::async(std::launch::async, [&] { return A::Wait(f.manager, 70001, first_hr, first_error); });
        if (first.wait_for(std::chrono::milliseconds(600)) != std::future_status::ready) A::RescueWait(f.manager);
        if (first.wait_for(std::chrono::seconds(3)) != std::future_status::ready) ExitProcess(2);
        check(first.get() && first_hr == static_cast<uint32_t>(E_ACCESSDENIED) && first_error == L"first owned result",
              "M04-001 first owned terminal is not replaced by unknown or duplicate completions");
        uint32_t second_hr = static_cast<uint32_t>(E_FAIL); std::wstring second_error;
        auto second = std::async(std::launch::async, [&] { return A::Wait(f.manager, 70002, second_hr, second_error); });
        if (second.wait_for(std::chrono::milliseconds(600)) != std::future_status::ready) A::RescueWait(f.manager);
        if (second.wait_for(std::chrono::seconds(3)) != std::future_status::ready) ExitProcess(2);
        check(second.get() && second_hr == S_OK && second_error == L"second owned result",
              "M04-001 another owned request retains its independent terminal payload until consumed");
    }
    {
        RoutingFixture f;
        C::SetNextId(f.client, 0); // zero is reserved for rejection, including wraparound
        unsigned accepted = 0, transports = 0; uint32_t registered = 0;
        bool ordered = false;
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t, uint32_t id) {
            ++transports; ordered = accepted == 1 && registered == id;
            C::Done(client, id);
        };
        const auto on_accept = [&](uint32_t id) { ++accepted; registered = id; };
        const uint32_t id = f.client.CreateFolder(L"review-only", on_accept);
        check(id == 1 && registered == id && accepted == 1 && transports == 1,
              "M04-001 Submit skips reserved zero and accepts a nonzero request exactly once");
        check(ordered && A::ResultsEmpty(f.manager),
              "M04-001 acceptance precedes synchronous transport completion and unowned results are not retained");
        C::NotRunning(f.client);
        check(f.client.CreateFolder(L"review-only", on_accept) == 0 && accepted == 1 && transports == 1,
              "M04-001 stopped client neither accepts nor sends a rejected request");
    }
    {
        RoutingFixture f;
        unsigned partials = 0, finals = 0; uint32_t session = 0;
        f.manager.SetShellMenuCallback([&](uint32_t token, std::vector<ops::ShellMenuItem>, bool partial, ops::ShellMenuAnswer) {
            if (token == 201) { if (partial) ++partials; else ++finals; }
        });
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t, uint32_t id) {
            session = id; C::Items(client, id, true); C::Items(client, id);
        };
        A::Query(f.manager, 201);
        check(partials == 1 && finals == 1, "M04-001 partial and final query responses both preserve token ownership");
        A::Close(f.manager, 201);
        check(A::RoutesEmpty(f.manager), "M04-001 closing a completed query retires both session mappings");
        C::Items(f.client, session);
        check(partials == 1 && finals == 1, "M04-001 late callback after Close is not delivered to a retired token");
    }
    {
        RoutingFixture f;
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t, uint32_t id) { C::Items(client, id); };
        A::Query(f.manager, 202); C::NotRunning(f.client); A::Invoke(f.manager, 202);
        check(f.manager.TakeCtxInvokeDone() && !f.manager.TakeCtxInvokeDone(),
              "M04-001 zero-id invoke finishes its local notification exactly once");
        check(A::RoutesEmpty(f.manager) && f.manager.TryPrepareForUpdate(),
              "M04-001 zero-id invoke cannot strand update preparation");
        f.manager.CancelUpdatePreparation();
    }
    {
        RoutingFixture f;
        A::PrepareRunningProgress(f.manager);
        A::Register(f.manager, 71001);
        C::Progress(f.client, 99999, L"unowned");
        check(f.manager.Status().current_item.empty() && f.manager.Status().percent == 0.0f, "M04-001 unrelated progress cannot overwrite file-operation status");
        C::Progress(f.client, 71001, L"owned");
        check(f.manager.Status().active && f.manager.Status().phase == ops::OpPhase::Running &&
              f.manager.Status().current_item == L"owned" && f.manager.Status().percent == 50.0f,
              "M04-001 progress from the registered file request remains visible");
    }
    {
        RoutingFixture f;
        ipc::review_submit = [&](ipc::ShellClient& client, uint32_t type, uint32_t id) {
            if (type == ipc::REQ_CTX_QUERY) C::Items(client, id);
            // Deliberately retain the accepted invoke until Stop bookkeeping.
        };
        A::Query(f.manager, 203); A::Invoke(f.manager, 203); A::Query(f.manager, 204);
        A::Register(f.manager, 72001); C::Done(f.client, 72001);
        check(!A::RoutesEmpty(f.manager) && !A::ResultsEmpty(f.manager),
              "M04-001 stop fixture contains genuine pending routes and a retained owned result");
        A::StopRoutingFixture(f.manager);
        check(A::RoutesEmpty(f.manager) && A::ResultsEmpty(f.manager),
              "M04-001 post-join Stop cleanup retires menu routes and file-result ownership");
        C::Done(f.client, 72001);
        check(A::ResultsEmpty(f.manager) && f.manager.TryPrepareForUpdate(),
              "M04-001 late completion cannot repopulate stopped ownership or block update preparation");
        f.manager.CancelUpdatePreparation();
    }
    return failures ? 1 : 0;
}