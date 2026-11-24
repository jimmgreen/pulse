// Included inside pulse::ops: independent execution state, shared admission/history.
namespace {
bool InteractiveRequestShape(const OpRequest& req) {
    if (req.sources.empty() || IsShellTransferRequest(req) || req.is_undo || req.undo_group ||
        req.generated_output || req.generated_directory_replay || req.duplicate_cleanup || req.lock_retry ||
        req.only_if_empty || !req.close_first.empty() || req.rename_authorized || req.rename_completed ||
        req.authorized_rename_identity || !req.authorized_rename_identities.empty() ||
        (!req.template_file.empty() && (req.type != OpType::CreateTextFile || !req.template_data.empty())) ||
        req.template_data.size() > 1024 * 1024 ||
        !req.new_names.empty()) return false;
    if (req.collision_policy != CollisionPolicy::System &&
        !(req.collision_policy == CollisionPolicy::KeepBoth && (req.type == OpType::CreateFolder ||
          req.type == OpType::CreateTextFile || req.type == OpType::Rename || req.type == OpType::Copy ||
          req.type == OpType::Move))) return false;
    switch (req.type) {
    case OpType::CreateFolder: case OpType::CreateTextFile: case OpType::Rename:
    case OpType::Copy: case OpType::Move: case OpType::RealDelete: case OpType::RecycleDelete: return true;
    default: return false;
    }
}

bool InteractiveKnownShape(const OpRequest& req) {
    if (req.sources.empty() || IsShellTransferRequest(req) || req.generated_output || req.generated_directory_replay ||
        req.is_undo || req.duplicate_cleanup || req.authorized_rename_identity || !req.authorized_rename_identities.empty())
        return false;
    switch (req.type) {
    case OpType::Rename: return req.sources.size() == 1 && IsRenameFilename(req.new_name);
    case OpType::Copy: case OpType::Move:
        return !req.dest_dir.empty() && (req.new_name.empty() ||
            (req.type == OpType::Move && req.sources.size() == 1 && IsRenameFilename(req.new_name)));
    case OpType::CreateFolder: case OpType::CreateTextFile: case OpType::RecycleDelete: case OpType::RealDelete: return true;
    default: return false;
    }
}
std::vector<interactive_names::Root> InteractiveRoots(const OpRequest& req) {
    using interactive_names::Family;
    std::vector<interactive_names::Root> roots;
    if (!InteractiveKnownShape(req)) return roots;
    for (const auto& source : req.sources) {
        auto original = interactive_names::Make(source);
        if (original.parent.empty() || original.leaf.empty()) return {};
        const bool keep = req.collision_policy == CollisionPolicy::KeepBoth;
        if (keep && req.type == OpType::CreateFolder) original.family = Family::NumberedDirectory;
        if (keep && req.type == OpType::CreateTextFile) original.family = Family::NumberedFile;
        if (req.type == OpType::Copy || req.type == OpType::Move)
            roots.push_back(interactive_names::Make(JoinPath(req.dest_dir,
                req.new_name.empty() ? original.leaf : req.new_name), Family::Transfer));
        else if (req.type == OpType::Rename)
            roots.push_back({original.parent, req.new_name, keep ? Family::NumberedUnknown : Family::Exact});
        // A copy only reads its source; a move, rename or delete changes it.
        original.read = req.type == OpType::Copy;
        roots.push_back(std::move(original));
    }
    if (!req.template_file.empty()) {
        auto tmpl = interactive_names::Make(req.template_file);
        tmpl.read = true;
        roots.push_back(std::move(tmpl));
    }
    // Recycles on one volume share its Recycle Bin (payload names, index, quota):
    // a footprint on the bin keeps them in order with each other.
    if (req.type == OpType::RecycleDelete) {
        std::set<std::wstring> bins;
        for (const auto& source : req.sources) {
            const auto plain = pulse::path::StripExtendedPathPrefix(source);
            if (plain.size() >= 3 && plain[1] == L':') bins.insert(std::wstring(1, static_cast<wchar_t>(std::towupper(plain[0]))) + L":\\$Recycle.Bin");
        }
        for (const auto& bin : bins) roots.push_back(interactive_names::Make(bin));
    }
    return roots;
}
using InteractiveFootprint = std::vector<interactive_names::Root>;
wchar_t InteractiveDrive(const InteractiveFootprint& roots) {
    wchar_t letter = 0;
    for (const auto& root : roots) {
        if (root.parent.size() < 3 || root.parent[1] != L':' || root.parent[2] != L'\\') return 0;
        const auto current = static_cast<wchar_t>(std::towupper(root.parent[0]));
        if (letter && current != letter) return 0;
        letter = current;
    }
    return letter;
}
bool InteractiveRequestsConflict(const InteractiveFootprint& left, wchar_t left_drive,
                                const InteractiveFootprint& right, wchar_t right_drive) {
    if (left.empty() || right.empty()) return true;
    const auto overlap = [](const auto& a, const auto& b) {
        for (const auto& x : a) for (const auto& y : b)
            if (!(x.read && y.read) && interactive_names::Overlaps(x, y)) return true;
        return false;
    };
    if (left_drive && left_drive == right_drive) return overlap(left, right);
    auto a = left, b = right;
    if (!interactive_names::NormalizeVolumes(a, b, [](wchar_t key, std::wstring& result) {
        const wchar_t mount[]{key, L':', L'\\', L'\0'};
        wchar_t volume[MAX_PATH]{};
        if (!GetVolumeNameForVolumeMountPointW(mount, volume, MAX_PATH)) return false;
        result = volume; return true;
    })) return true;
    return overlap(a, b);
}

// Admission checks run only on the scheduler, never on the window thread.
// Reject namespace aliases and reparse ancestors rather than guessing identities.
struct InteractivePathProof {
    std::set<std::wstring> volumes, plain_directories;
};
bool InteractivePlainPath(const std::wstring& input, bool may_be_missing, InteractivePathProof& proof) {
    const auto value = pulse::path::StripExtendedPathPrefix(input);
    if (value.size() < 3 || value[1] != L':' || value[2] != L'\\' ||
        value.find(L'~') != std::wstring::npos || value.find(L'/') != std::wstring::npos ||
        value.find(L':', 2) != std::wstring::npos || value.find(L"\\\\") != std::wstring::npos) return false;
    const std::filesystem::path path(value);
    for (const auto& component : path) {
        const auto part = component.wstring();
        if (component == L"." || component == L".." || (!part.empty() && (part.back() == L'.' || part.back() == L' '))) return false;
    }
    const auto root = path.root_path().wstring();
    if (!proof.volumes.contains(root)) {
        // Local disks, USB sticks included; a share's names cannot be proven to be its only names.
        const UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return false;
        wchar_t volume[MAX_PATH]{};
        if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) return false;
        proof.volumes.insert(root);
    }
    auto current = path;
    bool first = true;
    for (;;) {
        const auto name = current.wstring();
        if (proof.plain_directories.contains(name)) break;
        const DWORD attrs = GetFileAttributesW(LongPathForApi(name).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (!first || !may_be_missing || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) return false;
        } else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        else if (attrs & FILE_ATTRIBUTE_DIRECTORY) proof.plain_directories.insert(name);
        else if (!first) return false;
        if (current == current.root_path()) break;
        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) return false;
        current = parent;
        first = false;
    }
    return true;
}

bool InteractivePlainRequest(const OpRequest& req, InteractivePathProof& proof) {
    if (!InteractiveKnownShape(req)) return false;
    bool batched = false;
    if (req.sources.size() >= SourceListing::kMinSourcesPerFolder) {
        const auto parent = ParentOf(req.sources.front());
        const auto same_parent = [&](const std::wstring& p) {
            const auto slash = p.rfind(L'\\');
            const auto length = !parent.empty() && parent.back() == L'\\' ? parent.size() - 1 : parent.size();
            return slash == length && !p.compare(0, parent.size(), parent);
        };
        if (!parent.empty() && std::all_of(req.sources.begin(), req.sources.end(), same_parent)) {
            if (!InteractivePlainPath(parent, false, proof)) return false;
            std::vector<std::wstring> leaves;
            leaves.reserve(req.sources.size());
            for (const auto& path : req.sources) leaves.push_back(FileName(path));
            InteractiveSiblingListing siblings;
            if (siblings.Read(parent, leaves, GetTickCount64() + 100)) {
                for (const auto& name : leaves)
                    if (siblings.Get(name) == InteractiveSiblingListing::Kind::Unsafe) return false;
                batched = true;
            }
        }
    }
    if (!batched) for (const auto& source : req.sources) if (!InteractivePlainPath(source, true, proof)) return false;
    return (req.dest_dir.empty() || InteractivePlainPath(req.dest_dir, false, proof)) &&
        (req.template_file.empty() || InteractivePlainPath(req.template_file, false, proof));
}
bool InteractivePlainRequest(const OpRequest& req) {
    InteractivePathProof proof; // Never carry a positive reparse proof across admissions.
    return InteractivePlainRequest(req, proof);
}

bool InteractiveTargetTreeKnown(const OpRequest& req, InteractiveTreeWalkBudget& budget) {
    if (req.type != OpType::Copy && req.type != OpType::Move) return true;
    std::vector<std::wstring> pending;
    bool batched = false;
    if (req.sources.size() >= SourceListing::kMinSourcesPerFolder) {
        std::vector<std::wstring> leaves;
        leaves.reserve(req.sources.size());
        for (const auto& source : req.sources) leaves.push_back(FileName(source));
        InteractiveSiblingListing siblings;
        if (siblings.Read(req.dest_dir, leaves, budget.deadline)) {
            for (const auto& name : leaves) {
                if (!budget.Visit()) return false;
                const auto kind = siblings.Get(name);
                if (kind == InteractiveSiblingListing::Kind::Unsafe) return false;
                if (kind == InteractiveSiblingListing::Kind::Directory) pending.push_back(JoinPath(req.dest_dir, name));
            }
            batched = true;
        }
    }
    if (!batched) for (const auto& source : req.sources)
        pending.push_back(JoinPath(req.dest_dir, req.type == OpType::Move && req.sources.size() == 1 && !req.new_name.empty()
            ? req.new_name : FileName(source)));
    return WalkPlainInteractiveTargets(pending, budget, LongPathForApi, JoinPath);
}

// Whether a request may run in a lane beside other operations. Any size,
// folders and several items are fine; what must hold is that every path is a
// plain local path (InteractivePlainRequest) and the items are not links. A
// taken target is fine too: the lane asks about the conflict itself, and the
// scheduler walks the target tree for links before admitting the request.
bool InteractiveEligibleFile(const OpRequest& req) {
    constexpr size_t kMaxLaneSources = 20000;  // beyond this, the checks below cost more than queueing
    InteractivePathProof proof;
    if (!InteractiveRequestShape(req) || req.sources.size() > kMaxLaneSources || !InteractivePlainRequest(req, proof))
        return false;
    const bool create = req.type == OpType::CreateFolder || req.type == OpType::CreateTextFile;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!req.template_file.empty() && (!GetFileAttributesExW(LongPathForApi(req.template_file).c_str(),
        GetFileExInfoStandard, &data) || (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        data.nFileSizeHigh || data.nFileSizeLow > 1024 * 1024)) return false;
    if (create) {
        return req.collision_policy == CollisionPolicy::KeepBoth ||
            GetFileAttributesW(LongPathForApi(req.sources.front()).c_str()) == INVALID_FILE_ATTRIBUTES;
    }
    for (const auto& source : req.sources) {
        if (!GetFileAttributesExW(LongPathForApi(source).c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (req.type == OpType::Copy || req.type == OpType::Move) {
            const auto target = JoinPath(req.dest_dir, req.new_name.empty() ? FileName(source) : req.new_name);
            if (!InteractivePlainPath(target, true, proof)) return false;
        } else if (req.type == OpType::Rename) {
            const auto target = JoinPath(ParentOf(source), req.new_name);
            if (!InteractivePlainPath(target, true, proof)) return false;
            // A KeepBoth rename only tries the reserved numbered names without replacing anything.
            if (GetFileAttributesW(LongPathForApi(target).c_str()) != INVALID_FILE_ATTRIBUTES &&
                req.collision_policy != CollisionPolicy::KeepBoth) return false;
        }
    }
    return true;
}
}

std::shared_ptr<OpsManager> OpsManager::InteractiveExecutor(uint64_t task_id) const {
    if (!task_id) return nullptr;
    std::lock_guard lock(mutex_);
    for (const auto& lane : lanes_)
        if (lane.item && lane.item->seq == task_id) return lane.executor;
    return nullptr;
}

bool OpsManager::AnyLaneBusyLocked() const {
    return std::any_of(lanes_.begin(), lanes_.end(), [](const Lane& lane) { return lane.item.has_value(); });
}

std::shared_ptr<OpsManager> OpsManager::PresentedInteractiveExecutor() const {
    std::lock_guard lock(mutex_);
    std::shared_ptr<OpsManager> first;
    for (const auto& lane : lanes_) {
        if (!lane.item) continue;
        const auto child = lane.executor->Status();
        if (child.authorization != AuthorizationState::None || child.phase == OpPhase::WaitingForConflict)
            return lane.executor;
        if (!first) first = lane.executor;
    }
    return active_item_ ? nullptr : first;
}

std::vector<OpStatus> OpsManager::ActiveStatuses(bool include_queued) const {
    std::vector<OpStatus> result;
    std::vector<std::shared_ptr<OpsManager>> busy;
    {
        std::lock_guard lock(mutex_);
        if (active_item_ && status_.active && status_.task_id == active_item_->seq) result.push_back(StatusLocked());
        for (const auto& lane : lanes_) if (lane.item) busy.push_back(lane.executor);
    }
    for (const auto& executor : busy) {
        auto status = executor->Status();
        if (status.active) result.push_back(std::move(status));
    }
    if (include_queued) {
        std::lock_guard lock(mutex_);
        const bool lanes_full = std::all_of(lanes_.begin(), lanes_.end(), [](const Lane& lane) { return lane.item.has_value(); });
        for (const auto& item : queue_) {
            const auto type = item.req.type;
            if (item.cancel_requested || item.controlled || !item.open_path.empty() || !item.open_paths.empty() ||
                (type != OpType::Copy && type != OpType::Move && type != OpType::RecycleDelete && type != OpType::RealDelete))
                continue;
            // A lane merged back or the main worker took it between the two locks.
            if (std::any_of(result.begin(), result.end(), [&](const OpStatus& s) { return s.task_id == item.seq; })) continue;
            OpStatus queued;
            queued.active = true;
            queued.type = type;
            queued.phase = OpPhase::Queued;
            queued.task_id = item.seq;
            queued.total_items = item.req.sources.size();
            queued.source_label = item.req.sources.empty() ? std::wstring() : FileName(item.req.sources.front());
            queued.destination_label = FileName(item.req.dest_dir);
            queued.wait_reason = item.wait_reason != OpWaitReason::None ? item.wait_reason
                : lanes_full ? OpWaitReason::LanesFull : OpWaitReason::None;
            result.push_back(std::move(queued));
        }
    }
    std::sort(result.begin(), result.end(), [](const OpStatus& a, const OpStatus& b) { return a.task_id < b.task_id; });
    return result;
}

bool OpsManager::InteractiveBlocksLocked(const QueueItem&) const {
    // The main worker starts the next queued item only when no lane runs: a
    // lane's paths were checked against what ran at its admission, not against
    // whatever the main worker would pick up next. The scheduler places queued
    // work beside running lanes instead.
    return AnyLaneBusyLocked();
}

// The scheduler: merges lanes that finished, and starts queued work in a free
// lane when it cannot touch anything running (the main worker's operation or
// any lane's) or anything queued ahead of it. Whatever it cannot prove safe
// stays in the queue in order; a path really shared with a running operation
// is the one reason to wait.
void OpsManager::InteractiveThread() {
    InteractiveTreeWatch target_watch;
    uint64_t watched_primary = 0;
    bool cached_tree = false, watch_next_walk = false;
    std::vector<uint64_t> last_running;
    const auto merge_lane = [&](size_t index) {
        auto& lane = lanes_[index];
        auto& executor = *lane.executor;
        std::scoped_lock lock(mutex_, executor.mutex_);
        if (!lane.item || executor.active_item_ || !executor.queue_.empty()) return false;
        for (auto& entry : executor.undo_) {
            ClearRedoLocked();
            entry.redo_cleared = std::exchange(redo_cleared_pending_, false) || entry.redo_cleared;
            InsertUndoLocked(std::move(entry));
            ++undo_json_revision_;
        }
        executor.undo_.clear();
        TrimUndoHistory(undo_);
        for (auto& completed : executor.completions_) completions_.push_back(std::move(completed));
        executor.completions_.clear();
        for (auto& finished : executor.finished_tasks_) finished_tasks_.push_back(std::move(finished));
        executor.finished_tasks_.clear();
        for (auto& retry : executor.failed_retries_) failed_retries_.push_back(std::move(retry));
        executor.failed_retries_.clear();
        while (failed_retries_.size() > 8) failed_retries_.pop_front();
        if (executor.lock_retry_) lock_retry_ = std::move(executor.lock_retry_);
        executor.lock_retry_.reset();
        for (auto& final : executor.final_statuses_) {
            final.terminal_sequence = ++terminal_sequence_;
            final.completed_ops = ++status_.completed_ops;
            if (retained_task_ids_.contains(final.task_id)) retained_final_statuses_.insert_or_assign(final.task_id, final);
            final_statuses_.push_back(final);
            if (!active_item_) status_ = final;
        }
        executor.final_statuses_.clear();
        while (final_statuses_.size() > 16) final_statuses_.pop_front();
        lane.item.reset();
        BumpUiRevision();
        return true;
    };
    const auto lane_finished_locked = [&](size_t index) {
        const auto& lane = lanes_[index];
        if (!lane.item) return false;
        std::lock_guard child_lock(lane.executor->mutex_);
        return !lane.executor->active_item_ && lane.executor->queue_.empty();
    };
    for (;;) {
        QueueItem candidate;
        std::vector<QueueItem> running, predecessors;
        size_t free_lane = lanes_.size();
        bool merge = false;
        bool has_primary = false;  // running.front() is the main worker's operation
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                if (!running_ || stopping_) return true;
                for (size_t i = 0; i < lanes_.size(); ++i) if (lane_finished_locked(i)) { merge = true; return true; }
                // What runs changed: what waited for it gets another look.
                std::vector<uint64_t> now;
                if (active_item_) now.push_back(active_item_->seq);
                for (const auto& lane : lanes_) if (lane.item) now.push_back(lane.item->seq);
                if (now != last_running) {
                    last_running = now;
                    interactive_serial_only_.clear();
                }
                if (now.empty() || history_replay_ || recovery_cleanup_active_ || !recovery_cleanup_roots_.empty())
                    return false;
                if (active_item_ && (active_item_->controlled || active_item_->cancel_requested ||
                    active_item_->transfer_cancelled)) return false;
                free_lane = lanes_.size();
                for (size_t i = 0; i < lanes_.size(); ++i) if (!lanes_[i].item) { free_lane = i; break; }
                if (free_lane == lanes_.size()) return false;
                for (const auto& item : queue_) {
                    if (item.controlled || item.req.is_undo) return false;  // these keep their place
                    if (!item.cancel_requested && !interactive_serial_only_.contains(item.seq) &&
                        InteractiveRequestShape(item.req)) return true;
                }
                return false;
            });
            if (!running_ || stopping_) break;
            if (!merge) {
                auto it = queue_.begin();
                for (; it != queue_.end(); ++it)
                    if (!it->cancel_requested && !interactive_serial_only_.contains(it->seq) &&
                        InteractiveRequestShape(it->req)) break;
                if (it == queue_.end()) continue;
                candidate = *it;
                predecessors.assign(queue_.begin(), it);
                has_primary = active_item_.has_value();
                if (active_item_) running.push_back(*active_item_);
                for (const auto& lane : lanes_) if (lane.item) running.push_back(*lane.item);
            }
        }
        if (merge) {
            bool merged = false;
            for (size_t i = 0; i < lanes_.size(); ++i) merged = merge_lane(i) || merged;
            if (merged) {
                cv_.notify_all();
                NotifyStatusChanged();
                PersistJournal();
            }
            continue;
        }

        const auto started = GetTickCount64();
        uint32_t reject = 0;
        const auto roots = InteractiveRoots(candidate.req);
        const auto drive = InteractiveDrive(roots);
        for (const auto& other : running) {
            if (reject) break;
            const auto other_roots = InteractiveRoots(other.req);
            if (InteractiveRequestsConflict(roots, drive, other_roots, InteractiveDrive(other_roots))) reject = 1u;
            else if (!InteractivePlainRequest(other.req)) reject = 4u;
        }
        if (!reject && !InteractiveEligibleFile(candidate.req)) reject = 2u;
        for (const auto& prior : predecessors) {
            if (reject) break;
            const auto prior_roots = InteractiveRoots(prior.req);
            if (InteractiveRequestsConflict(roots, drive, prior_roots, InteractiveDrive(prior_roots)) ||
                !InteractivePlainRequest(prior.req)) reject = 8u;
        }
        // Walks are bounded by a deadline the scheduler extends only while the
        // candidate keeps its place in the queue.
        InteractiveTreeWalkBudget budget{stopping_, [&] {
            std::lock_guard lock(mutex_);
            if (!running_ || stopping_ || history_replay_ || recovery_cleanup_active_ ||
                !recovery_cleanup_roots_.empty() || queue_.size() <= predecessors.size()) return false;
            for (size_t i = 0; i < predecessors.size(); ++i)
                if (queue_[i].seq != predecessors[i].seq) return false;
            const auto& item = queue_[predecessors.size()];
            return item.seq == candidate.seq && !item.cancel_requested;
        }, GetTickCount64() + 250};
        bool reused_tree = false;
        uint32_t target_proof = 0;
        // No running operation may write through a link inside its target tree:
        // the names compared above would then not be all the places it writes.
        for (const auto& other : running) {
            if (reject) break;
            const bool primary = has_primary && &other == &running.front();
            PrimaryTargetTreeState tree_state;
            if (primary) {
                std::lock_guard lock(mutex_);
                if (primary_target_tree_.task_id == other.seq && primary_target_tree_.ready) tree_state = primary_target_tree_;
            }
            if (tree_state.ready) {
                if (tree_state.unsafe) { target_proof = 3u; reject = 16u; }
                else if (tree_state.safe_without_walk) { target_proof = 2u; reused_tree = true; }
                else {
                    target_proof = 1u;
                    auto pending = tree_state.preexisting_dirs;
                    if (!WalkPlainInteractiveTargets(pending, budget, LongPathForApi, JoinPath)) reject = 16u;
                }
                continue;
            }
            // A single concrete destination of the main worker is watched after an
            // expensive walk, so the many small requests beside one big merge do
            // not walk it again; never reused across changes or task ids.
            const bool single_transfer = primary && other.req.sources.size() == 1 &&
                (other.req.type == OpType::Copy || other.req.type == OpType::Move);
            if (watched_primary != other.seq) {
                watched_primary = primary ? other.seq : watched_primary;
                cached_tree = watch_next_walk = false;
                target_watch.Reset();
            }
            std::wstring path;
            if (single_transfer && (cached_tree || watch_next_walk))
                path = LongPathForApi(JoinPath(other.req.dest_dir, other.req.new_name.empty()
                    ? FileName(other.req.sources.front()) : other.req.new_name));
            reused_tree = single_transfer && cached_tree && !path.empty() && target_watch.Unchanged(other.seq, path);
            if (reused_tree) continue;
            if (single_transfer) {
                cached_tree = false;
                if (!path.empty()) target_watch.Arm(other.seq, path);
            }
            if (!InteractiveTargetTreeKnown(other.req, budget)) reject = 16u;
            if (single_transfer) {
                cached_tree = !reject && !path.empty() && target_watch.Unchanged(other.seq, path);
                watch_next_walk = budget.visited >= 64;
                if (!cached_tree) target_watch.Reset();
            }
        }
        // The candidate's own target, now that a taken one is allowed (a merge).
        if (!reject && !InteractiveTargetTreeKnown(candidate.req, budget)) reject = 64u;
        for (const auto& prior : predecessors)
            if (!reject && !InteractiveTargetTreeKnown(prior.req, budget)) reject = 32u;
        if (budget.interrupted) continue;  // a superseded walk is not a negative proof
        diagnostics::runtime::Event("ops_interactive_admission", {
            {"candidate_type", static_cast<uint32_t>(candidate.req.type)},
            {"running", static_cast<uint32_t>(running.size())},
            {"lane", static_cast<uint32_t>(free_lane)},
            {"predecessors", static_cast<uint32_t>(predecessors.size())},
            {"reject", reject},
            {"target_proof", target_proof},
            {"visited", static_cast<uint32_t>(budget.visited)}, {"reused_tree", reused_tree ? 1u : 0u},
            {"walk_continuations", static_cast<uint32_t>(budget.continuations)},
            {"walk_ms", static_cast<uint32_t>(GetTickCount64() - started)}
        });
        {
            std::unique_lock lock(mutex_);
            if (!running_ || stopping_) break;
            auto it = std::find_if(queue_.begin(), queue_.end(), [&](const auto& item) { return item.seq == candidate.seq; });
            if (it == queue_.end() || it->cancel_requested) continue;
            if (history_replay_ || recovery_cleanup_active_ || !recovery_cleanup_roots_.empty() ||
                static_cast<size_t>(std::distance(queue_.begin(), it)) != predecessors.size()) continue;
            bool unchanged = true;
            for (size_t i = 0; i < predecessors.size(); ++i) if (queue_[i].seq != predecessors[i].seq) unchanged = false;
            // Everything running now must have been checked: a newly started
            // operation (the main worker took the next item) means checking again.
            const auto checked = [&](uint64_t seq) {
                return std::any_of(running.begin(), running.end(), [&](const QueueItem& item) { return item.seq == seq; });
            };
            if (active_item_ && !checked(active_item_->seq)) unchanged = false;
            if (active_item_ && (active_item_->controlled || active_item_->cancel_requested || active_item_->transfer_cancelled))
                unchanged = false;
            for (const auto& lane : lanes_) if (lane.item && !checked(lane.item->seq)) unchanged = false;
            if (!unchanged) continue;
            if (reject) {
                interactive_serial_only_.insert(candidate.seq);
                // A path shared with what runs or waits ahead; anything else
                // (links, network, an unreadable target) only waits its turn.
                it->wait_reason = (reject & (1u | 8u)) ? OpWaitReason::SamePlace : OpWaitReason::None;
                BumpUiRevision();
                continue;
            }
            if (lanes_[free_lane].item) continue;
            candidate = *it;
            lanes_[free_lane].item = candidate;
            last_running.push_back(candidate.seq);
            BumpUiRevision();
            queue_.erase(it);
            auto& executor = *lanes_[free_lane].executor;
            executor.SetUiWindow(UiWindow());
            executor.SetVerifyCopies(VerifyCopies());
            {
                std::lock_guard child_lock(executor.mutex_);
                executor.next_seq_ = candidate.seq + 1;
                executor.queue_.push_back(candidate);
            }
            executor.cv_.notify_all();
        }
        PersistJournal();
        cv_.notify_all();
        NotifyStatusChanged();
    }
    // Shutting down: stop what still runs in lanes and keep its outcome.
    for (size_t i = 0; i < lanes_.size(); ++i) {
        bool busy = false;
        { std::lock_guard lock(mutex_); busy = lanes_[i].item.has_value(); }
        if (!busy) continue;
        lanes_[i].executor->Stop();
        merge_lane(i);
    }
}
