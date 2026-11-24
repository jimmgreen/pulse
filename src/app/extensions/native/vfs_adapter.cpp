#include "vfs_adapter.h"
#include "diagnostics_adapter.h"
#include "../vfs_path.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

namespace pulse::extensions::native {
namespace {

constexpr size_t kMaxMountsPerSession = 32;

std::string ViewString(PulseStringView view) {
    if (!view.data || view.length == 0) return {};
    return std::string(view.data, view.length);
}

bool ValidUtf8WideBounded(PulseStringView view,
                          size_t max_bytes,
                          bool required,
                          std::wstring& out) {
    out.clear();
    if (view.length == 0) return !required;
    if (!view.data || view.length > max_bytes) return false;
    const std::string_view utf8(view.data, view.length);
    for (const unsigned char ch : utf8) {
        if (ch < 0x20u) return false;
    }
    return vfs::Utf8ToWideStrict(utf8, out);
}

} // namespace

struct VfsAdapter::State : std::enable_shared_from_this<State> {
    DWORD thread = GetCurrentThreadId();
    std::string package_id;
    VfsBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    std::mutex wake_mutex;
    std::function<void()> wake;
    vfs::VfsRegistry* registry = nullptr;
    std::shared_ptr<std::atomic<uint32_t>> active_calls = std::make_shared<std::atomic<uint32_t>>(0);
    bool closed = false;
    std::set<PulseVfsMountHandle> owned_handles;

    State(std::string pkg,
          VfsBackend impl,
          std::function<bool()> live,
          std::shared_ptr<DiagnosticsSink> sink,
          std::function<void()> wake_fn,
          vfs::VfsRegistry* reg)
        : package_id(std::move(pkg)),
          backend(std::move(impl)),
          alive(std::move(live)),
          diagnostics(std::move(sink)),
          wake(std::move(wake_fn)),
          registry(reg ? reg : &vfs::VfsRegistry::Instance()) {}

    bool Valid() const noexcept {
        return GetCurrentThreadId() == thread && !closed &&
               (!backend.valid_owner || backend.valid_owner()) &&
               (!alive || alive());
    }

    PulseStatus Fail(PulseStatus status,
                     std::string_view op,
                     std::string_view subject,
                     std::string_view field,
                     std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_VFS_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template <class F>
    static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            if (GetCurrentThreadId() != self->thread) {
                return self->Fail(PULSE_ERROR_STATE, "", "", "thread",
                                  "pulse.vfs must be called on the owner UI thread");
            }
            if (!self->Valid()) return PULSE_ERROR_STATE;
            return action(*self);
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    void NotifyWake() noexcept {
        std::function<void()> fn;
        {
            std::lock_guard lock(wake_mutex);
            fn = wake;
        }
        if (fn) {
            try { fn(); } catch (...) {}
        }
    }

    static PulseStatus PULSE_CALL RegisterMount(void* context,
                                                const PulseVfsMountDesc* desc,
                                                PulseVfsMountHandle* out_handle) noexcept {
        if (out_handle) *out_handle = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!desc || !out_handle) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", "", "desc",
                                 "desc and out_handle must be non-null");
            }
            if (desc->size < PULSE_VFS_MOUNT_DESC_V1_SIZE) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", "", "size",
                                 "desc->size must be >= PULSE_VFS_MOUNT_DESC_V1_SIZE");
            }
            // Fields appended after v1 exist only in descriptors that are large enough.
            const bool v2 = desc->size >= PULSE_VFS_MOUNT_DESC_V2_SIZE;
            const PulseVfsExtractItemsFn extract_items = v2 ? desc->extract_items : nullptr;
            const PulseVfsModifyFn modify = v2 ? desc->modify : nullptr;
            if (!vfs::IsValidVfsIdentifier(self.package_id)) {
                return self.Fail(PULSE_ERROR_STATE, "register_mount", "", "package_id",
                                 "invalid package identity for VFS mount");
            }
            if (self.owned_handles.size() >= kMaxMountsPerSession) {
                return self.Fail(PULSE_ERROR_STATE, "register_mount", "", "mounts",
                                 "maximum 32 VFS mounts per session");
            }
            if ((desc->capabilities & ~PULSE_VFS_ALL_CAPS) != 0u ||
                (desc->capabilities & PULSE_VFS_CAP_LIST_DIR) == 0u) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", "", "capabilities",
                                 "capabilities must include PULSE_VFS_CAP_LIST_DIR and only documented flags");
            }
            const std::string mount_id = ViewString(desc->mount_id);
            if (!vfs::IsValidVfsIdentifier(mount_id)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "mount_id",
                                 "mount_id must be 1..64 ASCII [A-Za-z0-9._-] bytes without '..'");
            }
            std::wstring display_name;
            if (!ValidUtf8WideBounded(desc->display_name, 128, true, display_name)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "display_name",
                                 "display_name must be 1..128 valid UTF-8 bytes without control characters");
            }
            std::wstring icon_glyph;
            if (!ValidUtf8WideBounded(desc->icon_glyph, 16, false, icon_glyph)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "icon_glyph",
                                 "icon_glyph must be <=16 valid UTF-8 bytes without control characters");
            }
            if (!desc->list_dir) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "list_dir",
                                 "list_dir callback is required");
            }
            if ((desc->capabilities & PULSE_VFS_CAP_EXTRACT_FILE) != 0u && !desc->extract_file) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "extract_file",
                                 "extract_file callback is required when PULSE_VFS_CAP_EXTRACT_FILE is set");
            }
            if ((desc->capabilities & PULSE_VFS_CAP_STAT) != 0u && !desc->stat) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "stat",
                                 "stat callback is required when PULSE_VFS_CAP_STAT is set");
            }
            if ((desc->capabilities & PULSE_VFS_CAP_EXTRACT_ITEMS) != 0u && !extract_items) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "extract_items",
                                 "extract_items callback (v2 descriptor) is required when PULSE_VFS_CAP_EXTRACT_ITEMS is set");
            }
            if ((desc->capabilities & PULSE_VFS_CAP_MODIFY) != 0u &&
                (!modify || (desc->capabilities & PULSE_VFS_CAP_READONLY) != 0u)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "register_mount", mount_id, "modify",
                                 "PULSE_VFS_CAP_MODIFY needs a modify callback (v2 descriptor) and excludes PULSE_VFS_CAP_READONLY");
            }

            vfs::VfsMountRegistration reg;
            reg.package_id = self.package_id;
            reg.mount_id = mount_id;
            reg.display_name = std::move(display_name);
            reg.icon_glyph = std::move(icon_glyph);
            reg.capabilities = desc->capabilities;
            reg.list_dir = desc->list_dir;
            reg.extract_file = desc->extract_file;
            reg.stat = desc->stat;
            reg.extract_items = (desc->capabilities & PULSE_VFS_CAP_EXTRACT_ITEMS) != 0u ? extract_items : nullptr;
            reg.modify = (desc->capabilities & PULSE_VFS_CAP_MODIFY) != 0u ? modify : nullptr;
            reg.user = desc->user;
            reg.session_active_calls = self.active_calls;
            std::weak_ptr<State> weak_self = self.shared_from_this();
            reg.on_call_finished = [weak_self] {
                if (const auto locked = weak_self.lock()) locked->NotifyWake();
            };

            PulseVfsMountHandle handle = 0;
            const PulseStatus st = self.registry->RegisterMount(std::move(reg), handle);
            if (st != PULSE_OK) {
                return self.Fail(st, "register_mount", mount_id, "mount_id",
                                 st == PULSE_ERROR_STATE ? "duplicate active VFS mount_id for package"
                                                         : "invalid VFS mount registration");
            }
            self.owned_handles.insert(handle);
            *out_handle = handle;
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL UnregisterMount(void* context, PulseVfsMountHandle handle) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (handle == 0 || !self.owned_handles.contains(handle)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "unregister_mount", "", "handle",
                                 "unknown or unowned VFS mount handle");
            }
            self.owned_handles.erase(handle);
            self.registry->UnregisterMount(handle);
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL InvalidatePath(void* context,
                                                 PulseVfsMountHandle handle,
                                                 PulseStringView rel_path) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (handle == 0 || !self.owned_handles.contains(handle)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "invalidate_path", "", "handle",
                                 "unknown or unowned VFS mount handle");
            }
            std::string pkg, mount;
            if (!self.registry->ResolveHandle(handle, pkg, mount)) {
                return self.Fail(PULSE_ERROR_STATE, "invalidate_path", "", "handle",
                                 "mount handle is no longer active");
            }
            if (rel_path.length > 0 && !rel_path.data) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "invalidate_path", mount, "rel_path",
                                 "non-empty rel_path requires valid UTF-8 pointer");
            }
            std::string norm_rel;
            if (!vfs::NormalizeRelPathUtf8(ViewString(rel_path), norm_rel)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "invalidate_path", mount, "rel_path",
                                 "rel_path contains invalid VFS segments or traversal");
            }
            if (self.backend.invalidate_path) {
                self.backend.invalidate_path(pkg, mount, norm_rel);
            }
            return PULSE_OK;
        });
    }

    static PulseStatus PULSE_CALL OpenPath(void* context,
                                           PulseVfsMountHandle handle,
                                           PulseStringView rel_path,
                                           uint32_t mode) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (handle == 0 || !self.owned_handles.contains(handle)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_path", "", "handle",
                                 "unknown or unowned VFS mount handle");
            }
            if (mode != PULSE_VFS_OPEN_CURRENT_TAB && mode != PULSE_VFS_OPEN_NEW_TAB) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_path", "", "mode",
                                 "unsupported VFS open mode");
            }
            std::string pkg, mount;
            if (!self.registry->ResolveHandle(handle, pkg, mount)) {
                return self.Fail(PULSE_ERROR_STATE, "open_path", "", "handle",
                                 "mount handle is no longer active");
            }
            if (rel_path.length > 0 && !rel_path.data) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_path", mount, "rel_path",
                                 "non-empty rel_path requires valid UTF-8 pointer");
            }
            std::string norm_rel;
            if (!vfs::NormalizeRelPathUtf8(ViewString(rel_path), norm_rel)) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_path", mount, "rel_path",
                                 "rel_path contains invalid VFS segments or traversal");
            }
            const std::wstring uri = vfs::FormatVfsPath(pkg, mount, norm_rel);
            if (uri.empty()) {
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_path", mount, "rel_path",
                                 "unable to format VFS URI");
            }
            if (!self.backend.open_path) {
                return self.Fail(PULSE_ERROR_UNAVAILABLE, "open_path", mount, "",
                                 "VFS navigation backend is unavailable");
            }
            const PulseStatus st = self.backend.open_path(uri, mode);
            if (st != PULSE_OK) {
                return self.Fail(st, "open_path", mount, "rel_path",
                                 "host rejected VFS path navigation");
            }
            return PULSE_OK;
        });
    }
};

VfsAdapter::VfsAdapter(std::string package_id,
                       VfsBackend backend,
                       std::function<bool()> alive,
                       std::shared_ptr<DiagnosticsSink> diagnostics,
                       std::function<void()> wake,
                       vfs::VfsRegistry* registry)
    : state_(std::make_shared<State>(std::move(package_id),
                                     std::move(backend),
                                     std::move(alive),
                                     std::move(diagnostics),
                                     std::move(wake),
                                     registry)) {}

VfsAdapter::~VfsAdapter() { Close(); }

void VfsAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

void VfsAdapter::SetWake(std::function<void()> wake) {
    if (!state_) return;
    std::lock_guard lock(state_->wake_mutex);
    state_->wake = std::move(wake);
}

PulseStatus VfsAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_VFS_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulseVfsV1)) return PULSE_ERROR_ARGUMENT;
        const PulseVfsV1 table{
            sizeof(PulseVfsV1),
            PULSE_VFS_VERSION_1,
            state_.get(),
            State::RegisterMount,
            State::UnregisterMount,
            State::InvalidatePath,
            State::OpenPath,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void VfsAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    const auto handles = std::move(state_->owned_handles);
    state_->owned_handles.clear();
    for (const auto handle : handles) {
        state_->registry->UnregisterMount(handle);
    }
}

bool VfsAdapter::CallbackActive() const noexcept {
    return state_ && state_->active_calls &&
           state_->active_calls->load(std::memory_order_acquire) > 0;
}

} // namespace pulse::extensions::native