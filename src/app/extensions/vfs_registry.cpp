#include "vfs_registry.h"

#include <chrono>
#include <condition_variable>

namespace pulse::extensions::vfs {

namespace {
constexpr size_t kMaxVfsEntriesPerListing = 50000;
} // namespace

struct VfsRegistry::MountRecord {
    PulseVfsMountHandle handle = 0;
    std::string package_id;
    std::string mount_id;
    std::wstring display_name;
    std::wstring icon_glyph;
    uint32_t capabilities = 0;
    PulseVfsListDirFn list_dir = nullptr;
    PulseVfsExtractFileFn extract_file = nullptr;
    PulseVfsStatFn stat = nullptr;
    PulseVfsExtractItemsFn extract_items = nullptr;
    PulseVfsModifyFn modify = nullptr;
    void* user = nullptr;
    std::shared_ptr<std::atomic<uint32_t>> session_active_calls;
    std::function<void()> on_call_finished;

    std::mutex state_mutex;
    std::condition_variable drain_cv;
    bool active = true;
    std::atomic<bool> cancelled{false};
    uint32_t in_flight = 0;
};

struct VfsRegistry::ActiveCallGuard {
    std::shared_ptr<MountRecord> record;
    bool entered = false;

    bool TryEnter() noexcept {
        if (!record) return false;
        std::lock_guard lock(record->state_mutex);
        if (!record->active || record->cancelled.load(std::memory_order_acquire)) {
            return false;
        }
        ++record->in_flight;
        if (record->session_active_calls) {
            record->session_active_calls->fetch_add(1, std::memory_order_acq_rel);
        }
        entered = true;
        return true;
    }

    ~ActiveCallGuard() {
        if (!entered || !record) return;
        std::function<void()> wake;
        bool wake_session = false;
        {
            std::lock_guard lock(record->state_mutex);
            if (record->in_flight > 0) --record->in_flight;
            if (record->in_flight == 0) {
                record->drain_cv.notify_all();
            }
            if (record->session_active_calls) {
                if (record->session_active_calls->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    wake_session = true;
                    wake = record->on_call_finished;
                }
            }
        }
        if (wake_session && wake) {
            try { wake(); } catch (...) {}
        }
    }
};

namespace {

struct CancelBridge {
    const std::atomic<bool>* mount_cancelled = nullptr;
    const std::function<bool()>* caller_cancelled = nullptr;

    static uint32_t PULSE_CALL IsCancelled(void* ctx) noexcept {
        if (!ctx) return 0u;
        const auto* self = static_cast<const CancelBridge*>(ctx);
        if (self->mount_cancelled && self->mount_cancelled->load(std::memory_order_acquire)) {
            return 1u;
        }
        if (self->caller_cancelled && *self->caller_cancelled) {
            try {
                return (*self->caller_cancelled)() ? 1u : 0u;
            } catch (...) {
                return 1u;
            }
        }
        return 0u;
    }

    PulseVfsCancelToken Token() noexcept {
        return PulseVfsCancelToken{this, &CancelBridge::IsCancelled};
    }
};

struct EmitCollector {
    std::vector<VfsEntryData>* out = nullptr;
    CancelBridge* cancel = nullptr;
    size_t max_entries = kMaxVfsEntriesPerListing;
    bool overflow = false;

    static uint32_t PULSE_CALL Emit(void* ctx, const PulseVfsEntry* entry) noexcept {
        auto* self = static_cast<EmitCollector*>(ctx);
        if (!self || !self->out || !entry) return 0u;
        if (self->cancel && CancelBridge::IsCancelled(self->cancel) != 0u) return 0u;
        if (self->out->size() >= self->max_entries) {
            self->overflow = true;
            return 0u;
        }
        if (entry->size < sizeof(PulseVfsEntry)) return 0u;
        if ((entry->flags & ~PULSE_VFS_ENTRY_ALL_FLAGS) != 0u) return 0u;
        if (!entry->name.data || entry->name.length == 0 || entry->name.length > 255u) return 0u;
        const std::string_view name_utf8(entry->name.data, entry->name.length);
        if (!IsValidVfsEntryName(name_utf8)) return 0u;

        std::wstring wide_name;
        if (!Utf8ToWideStrict(name_utf8, wide_name)) return 0u;

        std::wstring wide_type;
        if (entry->type_text.length > 0) {
            if (!entry->type_text.data || entry->type_text.length > 128u) return 0u;
            const std::string_view type_utf8(entry->type_text.data, entry->type_text.length);
            if (!Utf8ToWideStrict(type_utf8, wide_type)) return 0u;
        }

        try {
            VfsEntryData row;
            row.name = std::move(wide_name);
            row.size_bytes = entry->size_bytes;
            row.modified_unix_ms = entry->modified_unix_ms;
            row.flags = entry->flags;
            row.type_text = std::move(wide_type);
            self->out->push_back(std::move(row));
            return 1u;
        } catch (...) {
            return 0u;
        }
    }
};

} // namespace

FILETIME UnixMsToFileTime(int64_t unix_ms) noexcept {
    FILETIME ft{};
    if (unix_ms <= 0) return ft;
    constexpr uint64_t kEpochDiff100Ns = 116444736000000000ULL;
    const uint64_t ticks = kEpochDiff100Ns + static_cast<uint64_t>(unix_ms) * 10000ULL;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
}

VfsRegistry& VfsRegistry::Instance() {
    static VfsRegistry* instance = new VfsRegistry();
    return *instance;
}

PulseStatus VfsRegistry::RegisterMount(VfsMountRegistration reg, PulseVfsMountHandle& out_handle) {
    out_handle = 0;
    if (!IsValidVfsIdentifier(reg.package_id) || !IsValidVfsIdentifier(reg.mount_id) ||
        reg.display_name.empty() || !reg.list_dir ||
        (reg.capabilities & PULSE_VFS_CAP_LIST_DIR) == 0u ||
        (reg.capabilities & ~PULSE_VFS_ALL_CAPS) != 0u) {
        return PULSE_ERROR_ARGUMENT;
    }
    if ((reg.capabilities & PULSE_VFS_CAP_EXTRACT_FILE) != 0u && !reg.extract_file) {
        return PULSE_ERROR_ARGUMENT;
    }
    if ((reg.capabilities & PULSE_VFS_CAP_STAT) != 0u && !reg.stat) {
        return PULSE_ERROR_ARGUMENT;
    }
    if ((reg.capabilities & PULSE_VFS_CAP_EXTRACT_ITEMS) != 0u && !reg.extract_items) {
        return PULSE_ERROR_ARGUMENT;
    }
    if ((reg.capabilities & PULSE_VFS_CAP_MODIFY) != 0u &&
        (!reg.modify || (reg.capabilities & PULSE_VFS_CAP_READONLY) != 0u)) {
        return PULSE_ERROR_ARGUMENT;
    }

    std::lock_guard lock(mutex_);
    const std::pair<std::string, std::string> key{reg.package_id, reg.mount_id};
    if (by_key_.contains(key)) {
        return PULSE_ERROR_STATE;
    }

    auto rec = std::make_shared<MountRecord>();
    rec->handle = next_handle_++;
    rec->package_id = std::move(reg.package_id);
    rec->mount_id = std::move(reg.mount_id);
    rec->display_name = std::move(reg.display_name);
    rec->icon_glyph = std::move(reg.icon_glyph);
    rec->capabilities = reg.capabilities;
    rec->list_dir = reg.list_dir;
    rec->extract_file = reg.extract_file;
    rec->stat = reg.stat;
    rec->extract_items = reg.extract_items;
    rec->modify = reg.modify;
    rec->user = reg.user;
    rec->session_active_calls = std::move(reg.session_active_calls);
    rec->on_call_finished = std::move(reg.on_call_finished);

    out_handle = rec->handle;
    by_key_.emplace(key, rec->handle);
    by_handle_.emplace(rec->handle, std::move(rec));
    return PULSE_OK;
}

bool VfsRegistry::UnregisterMount(PulseVfsMountHandle handle) {
    std::shared_ptr<MountRecord> rec;
    {
        std::lock_guard lock(mutex_);
        const auto it = by_handle_.find(handle);
        if (it == by_handle_.end()) return false;
        rec = std::move(it->second);
        by_handle_.erase(it);
        by_key_.erase({rec->package_id, rec->mount_id});
    }
    if (rec) {
        std::lock_guard lock(rec->state_mutex);
        rec->active = false;
        rec->cancelled.store(true, std::memory_order_release);
    }
    return true;
}

bool VfsRegistry::ResolveHandle(PulseVfsMountHandle handle,
                                std::string& out_package_id,
                                std::string& out_mount_id) const {
    out_package_id.clear();
    out_mount_id.clear();
    std::lock_guard lock(mutex_);
    const auto it = by_handle_.find(handle);
    if (it == by_handle_.end()) return false;
    out_package_id = it->second->package_id;
    out_mount_id = it->second->mount_id;
    return true;
}

std::shared_ptr<VfsRegistry::MountRecord> VfsRegistry::FindRecord(std::string_view package_id,
                                                                  std::string_view mount_id) const {
    std::lock_guard lock(mutex_);
    const auto key_it = by_key_.find({std::string(package_id), std::string(mount_id)});
    if (key_it == by_key_.end()) return {};
    const auto it = by_handle_.find(key_it->second);
    return it == by_handle_.end() ? std::shared_ptr<MountRecord>{} : it->second;
}

bool VfsRegistry::DescribeMount(std::string_view package_id,
                                std::string_view mount_id,
                                VfsMountInfo& out_info) const {
    out_info = {};
    const auto rec = FindRecord(package_id, mount_id);
    if (!rec) return false;
    out_info.package_id = rec->package_id;
    out_info.mount_id = rec->mount_id;
    out_info.display_name = rec->display_name;
    out_info.icon_glyph = rec->icon_glyph;
    out_info.capabilities = rec->capabilities;
    return true;
}

std::vector<VfsMountInfo> VfsRegistry::ListMounts() const {
    std::vector<VfsMountInfo> out;
    std::lock_guard lock(mutex_);
    out.reserve(by_handle_.size());
    for (const auto& [h, rec] : by_handle_) {
        (void)h;
        VfsMountInfo info;
        info.package_id = rec->package_id;
        info.mount_id = rec->mount_id;
        info.display_name = rec->display_name;
        info.icon_glyph = rec->icon_glyph;
        info.capabilities = rec->capabilities;
        out.push_back(std::move(info));
    }
    return out;
}

PulseStatus VfsRegistry::ListDirectory(std::wstring_view vfs_path,
                                       std::vector<VfsEntryData>& out_entries,
                                       const std::function<bool()>& is_cancelled) {
    out_entries.clear();
    const auto parsed = ParseVfsPath(vfs_path);
    if (!parsed) return PULSE_ERROR_ARGUMENT;

    const auto rec = FindRecord(parsed->package_id, parsed->mount_id);
    if (!rec) return PULSE_ERROR_UNAVAILABLE;

    ActiveCallGuard guard{rec};
    if (!guard.TryEnter()) return PULSE_ERROR_UNAVAILABLE;
    if (!rec->list_dir) return PULSE_ERROR_UNAVAILABLE;

    CancelBridge cancel{&rec->cancelled, is_cancelled ? &is_cancelled : nullptr};
    if (CancelBridge::IsCancelled(&cancel) != 0u) return PULSE_ERROR_STATE;

    const PulseVfsCancelToken token = cancel.Token();
    const PulseStringView mount_view{rec->mount_id.data(), static_cast<uint32_t>(rec->mount_id.size())};
    const PulseStringView rel_view{
        parsed->rel_path_utf8.empty() ? "" : parsed->rel_path_utf8.data(),
        static_cast<uint32_t>(parsed->rel_path_utf8.size())};

    EmitCollector collector{&out_entries, &cancel, kMaxVfsEntriesPerListing, false};
    const PulseStatus status = rec->list_dir(
        rec->user,
        mount_view,
        rel_view,
        &EmitCollector::Emit,
        &collector,
        &token);
    if (CancelBridge::IsCancelled(&cancel) != 0u) {
        out_entries.clear();
        return PULSE_ERROR_STATE;
    }
    return status;
}

PulseStatus VfsRegistry::ExtractFile(std::wstring_view vfs_path,
                                     std::wstring_view dest_path,
                                     const std::function<bool()>& is_cancelled) {
    const auto parsed = ParseVfsPath(vfs_path);
    if (!parsed || parsed->rel_path_utf8.empty() || dest_path.empty()) return PULSE_ERROR_ARGUMENT;

    std::string dest_utf8;
    if (!WideToUtf8Strict(dest_path, dest_utf8)) return PULSE_ERROR_ARGUMENT;

    const auto rec = FindRecord(parsed->package_id, parsed->mount_id);
    if (!rec) return PULSE_ERROR_UNAVAILABLE;

    ActiveCallGuard guard{rec};
    if (!guard.TryEnter()) return PULSE_ERROR_UNAVAILABLE;
    if ((rec->capabilities & PULSE_VFS_CAP_EXTRACT_FILE) == 0u || !rec->extract_file) {
        return PULSE_ERROR_UNAVAILABLE;
    }

    CancelBridge cancel{&rec->cancelled, is_cancelled ? &is_cancelled : nullptr};
    if (CancelBridge::IsCancelled(&cancel) != 0u) return PULSE_ERROR_STATE;

    const PulseVfsCancelToken token = cancel.Token();
    const PulseStringView mount_view{rec->mount_id.data(), static_cast<uint32_t>(rec->mount_id.size())};
    const PulseStringView rel_view{
        parsed->rel_path_utf8.data(),
        static_cast<uint32_t>(parsed->rel_path_utf8.size())};
    const PulseStringView dest_view{
        dest_utf8.data(),
        static_cast<uint32_t>(dest_utf8.size())};

    const PulseStatus status = rec->extract_file(
        rec->user,
        mount_view,
        rel_view,
        dest_view,
        &token);
    if (CancelBridge::IsCancelled(&cancel) != 0u && status == PULSE_OK) {
        return PULSE_ERROR_STATE;
    }
    return status;
}

PulseStatus VfsRegistry::Stat(std::wstring_view vfs_path,
                              VfsEntryData& out_entry,
                              const std::function<bool()>& is_cancelled) {
    out_entry = {};
    const auto parsed = ParseVfsPath(vfs_path);
    if (!parsed) return PULSE_ERROR_ARGUMENT;

    const auto rec = FindRecord(parsed->package_id, parsed->mount_id);
    if (!rec) return PULSE_ERROR_UNAVAILABLE;

    ActiveCallGuard guard{rec};
    if (!guard.TryEnter()) return PULSE_ERROR_UNAVAILABLE;
    if ((rec->capabilities & PULSE_VFS_CAP_STAT) == 0u || !rec->stat) {
        return PULSE_ERROR_UNAVAILABLE;
    }

    CancelBridge cancel{&rec->cancelled, is_cancelled ? &is_cancelled : nullptr};
    if (CancelBridge::IsCancelled(&cancel) != 0u) return PULSE_ERROR_STATE;

    std::vector<VfsEntryData> single;
    EmitCollector collector{&single, &cancel, 1, false};
    const PulseVfsCancelToken token = cancel.Token();
    const PulseStringView mount_view{rec->mount_id.data(), static_cast<uint32_t>(rec->mount_id.size())};
    const PulseStringView rel_view{
        parsed->rel_path_utf8.empty() ? "" : parsed->rel_path_utf8.data(),
        static_cast<uint32_t>(parsed->rel_path_utf8.size())};

    const PulseStatus status = rec->stat(
        rec->user,
        mount_view,
        rel_view,
        &EmitCollector::Emit,
        &collector,
        &token);
    if (status != PULSE_OK) return status;
    if (single.empty()) return PULSE_ERROR_UNAVAILABLE;
    out_entry = std::move(single.front());
    return PULSE_OK;
}

namespace {

struct OperationBridge {
    CancelBridge* cancel = nullptr;
    VfsOperationHooks* hooks = nullptr;

    static void PULSE_CALL Progress(void* ctx, const PulseVfsProgress* p) noexcept {
        auto* self = static_cast<OperationBridge*>(ctx);
        if (!self || !self->hooks || !self->hooks->progress || !p || p->size < sizeof(PulseVfsProgress)) return;
        try {
            VfsProgressInfo info;
            info.completed_bytes = p->completed_bytes;
            info.total_bytes = p->total_bytes;
            info.completed_items = p->completed_items;
            info.total_items = p->total_items;
            if (p->current.data && p->current.length > 0 && p->current.length <= 4096u)
                (void)Utf8ToWideStrict(std::string_view(p->current.data, p->current.length), info.current);
            self->hooks->progress(info);
        } catch (...) {}
    }

    static void PULSE_CALL Message(void* ctx, PulseStringView text) noexcept {
        auto* self = static_cast<OperationBridge*>(ctx);
        if (!self || !self->hooks || !text.data || text.length == 0 || text.length > 1024u) return;
        try {
            std::wstring wide;
            if (Utf8ToWideStrict(std::string_view(text.data, text.length), wide)) {
                for (auto& ch : wide) if (ch < 0x20) ch = L' ';
                self->hooks->message = std::move(wide);
            }
        } catch (...) {}
    }

    PulseVfsOperation Make(const PulseVfsCancelToken* token) noexcept {
        PulseVfsOperation op{};
        op.size = sizeof(op);
        op.context = this;
        op.cancel = token;
        op.progress = &OperationBridge::Progress;
        op.message = &OperationBridge::Message;
        return op;
    }
};

PulseStringView ViewOf(const std::string& text) noexcept {
    return PulseStringView{text.empty() ? "" : text.data(), static_cast<uint32_t>(text.size())};
}

} // namespace

uint32_t VfsRegistry::CapabilitiesOf(std::wstring_view vfs_path) const {
    const auto parsed = ParseVfsPath(vfs_path);
    if (!parsed) return 0u;
    const auto rec = FindRecord(parsed->package_id, parsed->mount_id);
    return rec ? rec->capabilities : 0u;
}

PulseStatus VfsRegistry::ExtractItems(const std::vector<std::wstring>& vfs_paths,
                                      std::wstring_view dest_dir,
                                      VfsOperationHooks& hooks) {
    hooks.message.clear();
    if (vfs_paths.empty() || vfs_paths.size() > 100000u || dest_dir.empty()) return PULSE_ERROR_ARGUMENT;
    std::string dest_utf8;
    if (!WideToUtf8Strict(dest_dir, dest_utf8)) return PULSE_ERROR_ARGUMENT;

    std::vector<std::string> rels;
    rels.reserve(vfs_paths.size());
    std::string package_id, mount_id;
    for (const auto& path : vfs_paths) {
        const auto parsed = ParseVfsPath(path);
        if (!parsed || parsed->rel_path_utf8.empty()) return PULSE_ERROR_ARGUMENT;
        if (rels.empty()) {
            package_id = parsed->package_id;
            mount_id = parsed->mount_id;
        } else if (parsed->package_id != package_id || parsed->mount_id != mount_id) {
            return PULSE_ERROR_ARGUMENT;
        }
        rels.push_back(parsed->rel_path_utf8);
    }

    const auto rec = FindRecord(package_id, mount_id);
    if (!rec) return PULSE_ERROR_UNAVAILABLE;
    ActiveCallGuard guard{rec};
    if (!guard.TryEnter()) return PULSE_ERROR_UNAVAILABLE;
    if ((rec->capabilities & PULSE_VFS_CAP_EXTRACT_ITEMS) == 0u || !rec->extract_items) return PULSE_ERROR_UNAVAILABLE;

    CancelBridge cancel{&rec->cancelled, hooks.is_cancelled ? &hooks.is_cancelled : nullptr};
    if (CancelBridge::IsCancelled(&cancel) != 0u) return PULSE_ERROR_STATE;
    const PulseVfsCancelToken token = cancel.Token();
    OperationBridge bridge{&cancel, &hooks};
    const PulseVfsOperation op = bridge.Make(&token);

    std::vector<PulseStringView> views;
    views.reserve(rels.size());
    for (const auto& rel : rels) views.push_back(ViewOf(rel));
    const PulseStatus status = rec->extract_items(rec->user, ViewOf(rec->mount_id), views.data(),
                                                  static_cast<uint32_t>(views.size()), ViewOf(dest_utf8), &op);
    if (CancelBridge::IsCancelled(&cancel) != 0u && status == PULSE_OK) return PULSE_ERROR_STATE;
    return status;
}

PulseStatus VfsRegistry::Modify(const std::vector<VfsChangeRequest>& changes, VfsOperationHooks& hooks) {
    hooks.message.clear();
    if (changes.empty() || changes.size() > 100000u) return PULSE_ERROR_ARGUMENT;

    struct Encoded {
        uint32_t kind = 0;
        std::string rel, source, target;
    };
    std::vector<Encoded> encoded;
    encoded.reserve(changes.size());
    std::string package_id, mount_id;
    for (const auto& change : changes) {
        if (change.kind > PULSE_VFS_CHANGE_RESTORE) return PULSE_ERROR_ARGUMENT;
        if (change.kind == PULSE_VFS_CHANGE_RESTORE && changes.size() != 1u) return PULSE_ERROR_ARGUMENT;
        const auto parsed = ParseVfsPath(change.vfs_path);
        if (!parsed) return PULSE_ERROR_ARGUMENT;
        if (encoded.empty()) {
            package_id = parsed->package_id;
            mount_id = parsed->mount_id;
        } else if (parsed->package_id != package_id || parsed->mount_id != mount_id) {
            return PULSE_ERROR_ARGUMENT;
        }
        Encoded item;
        item.kind = change.kind;
        item.rel = parsed->rel_path_utf8;
        if (change.kind != PULSE_VFS_CHANGE_RESTORE && item.rel.empty()) return PULSE_ERROR_ARGUMENT;
        if (change.kind == PULSE_VFS_CHANGE_PUT &&
            (change.source_path.empty() || !WideToUtf8Strict(change.source_path, item.source))) {
            return PULSE_ERROR_ARGUMENT;
        }
        if (change.kind == PULSE_VFS_CHANGE_RENAME) {
            const auto target = ParseVfsPath(change.new_vfs_path);
            if (!target || target->package_id != package_id || target->mount_id != mount_id ||
                target->rel_path_utf8.empty()) {
                return PULSE_ERROR_ARGUMENT;
            }
            item.target = target->rel_path_utf8;
        }
        encoded.push_back(std::move(item));
    }

    const auto rec = FindRecord(package_id, mount_id);
    if (!rec) return PULSE_ERROR_UNAVAILABLE;
    ActiveCallGuard guard{rec};
    if (!guard.TryEnter()) return PULSE_ERROR_UNAVAILABLE;
    if ((rec->capabilities & PULSE_VFS_CAP_MODIFY) == 0u || !rec->modify) return PULSE_ERROR_UNAVAILABLE;

    CancelBridge cancel{&rec->cancelled, hooks.is_cancelled ? &hooks.is_cancelled : nullptr};
    if (CancelBridge::IsCancelled(&cancel) != 0u) return PULSE_ERROR_STATE;
    const PulseVfsCancelToken token = cancel.Token();
    OperationBridge bridge{&cancel, &hooks};
    const PulseVfsOperation op = bridge.Make(&token);

    std::vector<PulseVfsChange> raw(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        raw[i].size = sizeof(PulseVfsChange);
        raw[i].kind = encoded[i].kind;
        raw[i].rel_path = ViewOf(encoded[i].rel);
        raw[i].source_path_utf8 = ViewOf(encoded[i].source);
        raw[i].new_rel_path = ViewOf(encoded[i].target);
    }
    // Writes are not abandoned half way by mount cancellation: the provider decides when it is safe to stop.
    return rec->modify(rec->user, ViewOf(rec->mount_id), raw.data(), static_cast<uint32_t>(raw.size()), &op);
}

bool VfsRegistry::WaitUntilIdle(PulseVfsMountHandle handle, uint32_t timeout_ms) {
    std::shared_ptr<MountRecord> rec;
    {
        std::lock_guard lock(mutex_);
        const auto it = by_handle_.find(handle);
        if (it == by_handle_.end()) return true;
        rec = it->second;
    }
    std::unique_lock lock(rec->state_mutex);
    if (rec->in_flight == 0) return true;
    return rec->drain_cv.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [&] { return rec->in_flight == 0; });
}

} // namespace pulse::extensions::vfs