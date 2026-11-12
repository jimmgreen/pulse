#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "../../app/extensions/authorization.h"
#include "../../app/extensions/icon_registry.h"
#include "../../app/extensions/manifest.h"
#include "../../app/extensions/native/commands_adapter.h"
#include "../../app/extensions/native/diagnostics_adapter.h"
#include "../../app/extensions/native/dialogs_adapter.h"
#include "../../app/extensions/native/fileops_adapter.h"
#include "../../app/extensions/native/icons_adapter.h"
#include "../../app/extensions/native/module.h"
#include "../../app/extensions/native/pickers_adapter.h"
#include "../../app/extensions/native/tasks_adapter.h"
#include "../../app/extensions/native/ui_model.h"
#include "../../app/extensions/native/workbench_adapter.h"
#include "../../app/extensions/ui_tree.h"
#include "../../common/json_value.h"

#include <pulse/abi.h>
#include <pulse/commands.h>
#include <pulse/context.h>
#include <pulse/diagnostics.h>
#include <pulse/dialogs.h>
#include <pulse/directories.h>
#include <pulse/dispatch.h>
#include <pulse/fileops.h>
#include <pulse/files.h>
#include <pulse/icons.h>
#include <pulse/pickers.h>
#include <pulse/preview.h>
#include <pulse/resources.h>
#include <pulse/settings_pages.h>
#include <pulse/staging.h>
#include <pulse/tasks.h>
#include <pulse/ui_data.h>
#include <pulse/ui_events.h>
#include <pulse/ui_model.h>
#include <pulse/ui_mount.h>
#include <pulse/workbench.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

using pulse::json::Value;

PulseStringView AsView(std::string_view text) noexcept {
    return PulseStringView{text.empty() ? "" : text.data(), static_cast<uint32_t>(text.size())};
}

std::string CopyView(PulseStringView view) {
    if (!view.data || view.length == 0) {
        return {};
    }
    return std::string(view.data, view.length);
}

const char* StatusName(PulseStatus status) noexcept {
    switch (status) {
    case PULSE_OK: return "PULSE_OK";
    case PULSE_ERROR_ARGUMENT: return "PULSE_ERROR_ARGUMENT";
    case PULSE_ERROR_ABI: return "PULSE_ERROR_ABI";
    case PULSE_ERROR_UNAVAILABLE: return "PULSE_ERROR_UNAVAILABLE";
    case PULSE_ERROR_INITIALIZATION: return "PULSE_ERROR_INITIALIZATION";
    case PULSE_ERROR_STATE: return "PULSE_ERROR_STATE";
    case PULSE_ERROR_LOAD: return "PULSE_ERROR_LOAD";
    case PULSE_ERROR_ENTRY: return "PULSE_ERROR_ENTRY";
    case PULSE_ERROR_PERMISSION: return "PULSE_ERROR_PERMISSION";
    case PULSE_ERROR_SDK_PREVIEW: return "PULSE_ERROR_SDK_PREVIEW";
    default: return "PULSE_ERROR_UNKNOWN";
    }
}

const char* TaskStateName(PulseTaskState state) noexcept {
    switch (state) {
    case PULSE_TASK_RUNNING: return "running";
    case PULSE_TASK_CANCELLING: return "cancelling";
    case PULSE_TASK_COMPLETED: return "completed";
    case PULSE_TASK_PARTIAL: return "partial";
    case PULSE_TASK_FAILED: return "failed";
    case PULSE_TASK_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

const char* InterfaceStabilityTier(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 12> kFrozen = {
        "pulse.files",
        "pulse.resources",
        "pulse.dispatch",
        "pulse.directories",
        "pulse.context",
        "pulse.fileops",
        "pulse.commands",
        "pulse.preview",
        "pulse.tasks",
        "pulse.staging",
        "pulse.settings.pages",
        "pulse.dialogs",
    };
    static constexpr std::array<std::string_view, 8> kExperimental = {
        "pulse.ui.model",
        "pulse.ui.events",
        "pulse.ui.data",
        "pulse.ui.mount",
        "pulse.icons",
        "pulse.diagnostics",
        "pulse.pickers",
        "pulse.workbench",
    };
    if (std::find(kFrozen.begin(), kFrozen.end(), name) != kFrozen.end()) {
        return "frozen";
    }
    if (std::find(kExperimental.begin(), kExperimental.end(), name) != kExperimental.end()) {
        return "experimental";
    }
    return "unknown";
}

bool ReadFileBytes(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const std::streamsize size = in.tellg();
    if (size < 0 || size > (64 * 1024 * 1024)) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(out.data(), size)) {
        return false;
    }
    return true;
}

std::string Sha256FileHex(const fs::path& path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::array<uint8_t, 65536> buffer{};
            DWORD read_bytes = 0;
            bool ok = true;
            while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read_bytes, nullptr)) {
                if (read_bytes == 0) {
                    break;
                }
                if (BCryptHashData(hash, buffer.data(), read_bytes, 0) != 0) {
                    ok = false;
                    break;
                }
            }
            std::array<uint8_t, 32> digest{};
            if (ok && BCryptFinishHash(hash, digest.data(), static_cast< ULONG >(digest.size()), 0) == 0) {
                static constexpr char kHex[] = "0123456789abcdef";
                hex.reserve(64);
                for (uint8_t byte : digest) {
                    hex.push_back(kHex[(byte >> 4) & 0x0Fu]);
                    hex.push_back(kHex[byte & 0x0Fu]);
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }

    CloseHandle(file);
    return hex;
}

bool CheckExclusiveFileUnlock(const fs::path& path, DWORD wait_budget_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_budget_ms);
    for (;;) {
        HANDLE handle = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
            handle = CreateFileW(
                path.c_str(),
                GENERIC_READ,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
        }
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

struct QueryAuditEntry {
    std::string name;
    uint32_t version = 0;
    uint32_t out_size = 0;
    PulseStatus status = PULSE_OK;
    std::string stability_tier;
};

struct PreviewProviderRecord {
    uint64_t id = 0;
    std::string extension;
    PulsePreviewFn callback = nullptr;
    void* user = nullptr;
};

struct PanelRecord {
    uint64_t id = 0;
    std::string title;
    uint32_t placement = PULSE_UI_MOUNT_WINDOW;
    std::map<std::string, std::string> states;
    bool closed = false;
    bool is_settings_page = false;
};

struct SelectionItem {
    std::string path;
    std::string name;
    uint32_t flags = 0;
};

class HeadlessHostEnvironment {
public:
    HeadlessHostEnvironment(std::string directory_utf8, std::vector<SelectionItem> selection_items, bool omit_experimental_ui)
        : directory_(std::move(directory_utf8)),
          selection_(std::move(selection_items)),
          omit_experimental_ui_(omit_experimental_ui),
          owner_thread_(GetCurrentThreadId()) {}

    bool Live() const noexcept {
        return live_.load(std::memory_order_acquire);
    }

    void Close() noexcept {
        live_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(dispatch_mutex_);
            dispatch_closed_ = true;
            dispatch_queue_.clear();
        }
        deferred_ui_callbacks_.clear();
    }

    void AttachAdapters(
        pulse::extensions::native::CommandsAdapter* commands,
        pulse::extensions::native::IconsAdapter* icons,
        pulse::extensions::native::TasksAdapter* tasks,
        pulse::extensions::native::FileOpsAdapter* fileops,
        pulse::extensions::native::DiagnosticsAdapter* diagnostics,
        pulse::extensions::native::DialogsAdapter* dialogs,
        pulse::extensions::native::PickersAdapter* pickers,
        pulse::extensions::native::WorkbenchAdapter* workbench,
        std::shared_ptr<pulse::extensions::native::DiagnosticsSink> diagnostics_sink) noexcept {
        commands_ = commands;
        icons_ = icons;
        tasks_ = tasks;
        fileops_ = fileops;
        diagnostics_ = diagnostics;
        dialogs_ = dialogs;
        pickers_ = pickers;
        workbench_ = workbench;
        diagnostics_sink_ = std::move(diagnostics_sink);
    }

    void DeferUiCallback(std::function<void()> fn) {
        if (fn && live_.load(std::memory_order_acquire)) {
            deferred_ui_callbacks_.push_back(std::move(fn));
        }
    }

    PulseContextV1 MakeContextTable() noexcept {
        return PulseContextV1{
            sizeof(PulseContextV1),
            PULSE_CONTEXT_VERSION_1,
            this,
            &ContextGet,
            &ContextSubscribe,
            &ContextUnsubscribe,
            &ContextGetSelection,
        };
    }

    PulseHostV1 MakeHostTable() noexcept {
        return PulseHostV1{
            sizeof(PulseHostV1),
            PULSE_ABI_VERSION_1,
            this,
            &QueryInterfaceStatic,
        };
    }

    bool HasPendingDeferred() const {
        std::lock_guard lock(dispatch_mutex_);
        return !dispatch_queue_.empty() || !deferred_ui_callbacks_.empty();
    }

    size_t PumpDeferred() {
        size_t executed = 0;
        for (size_t pass = 0; pass < 16; ++pass) {
            std::vector<DispatchItem> batch;
            {
                std::lock_guard lock(dispatch_mutex_);
                if (!dispatch_closed_ && !dispatch_queue_.empty()) {
                    batch.assign(dispatch_queue_.begin(), dispatch_queue_.end());
                    dispatch_queue_.clear();
                }
            }
            std::vector<std::function<void()>> local_deferred;
            if (!deferred_ui_callbacks_.empty()) {
                local_deferred.swap(deferred_ui_callbacks_);
            }
            if (batch.empty() && local_deferred.empty()) {
                break;
            }
            for (const auto& item : batch) {
                if (live_.load(std::memory_order_acquire) && item.callback) {
                    item.callback(item.user);
                    ++executed;
                }
            }
            for (auto& fn : local_deferred) {
                if (live_.load(std::memory_order_acquire) && fn) {
                    fn();
                    ++executed;
                }
            }
        }
        return executed;
    }

    const std::vector<QueryAuditEntry>& Queries() const noexcept { return queries_; }
    const std::map<uint64_t, PreviewProviderRecord>& Previews() const noexcept { return previews_; }
    const std::map<uint64_t, PanelRecord>& Panels() const noexcept { return panels_; }

    size_t OpenUserPanelCount() const noexcept {
        size_t count = 0;
        for (const auto& [id, panel] : panels_) {
            (void)id;
            if (!panel.closed && !panel.is_settings_page) {
                ++count;
            }
        }
        return count;
    }

    size_t OpenSettingsPageCount() const noexcept {
        return settings_page_id_ != 0 ? 1u : 0u;
    }

    size_t UnreleasedStageCount() const noexcept {
        return active_stages_.size();
    }

private:
    struct DispatchItem {
        uint64_t token = 0;
        PulseDispatchCallback callback = nullptr;
        void* user = nullptr;
    };

    bool OnOwnerThread() const noexcept {
        return GetCurrentThreadId() == owner_thread_ && live_.load(std::memory_order_acquire);
    }

    template <typename TableT>
    static PulseStatus WriteTable(const TableT& table, void* out_table, uint32_t out_size) noexcept {
        if (!out_table || out_size < sizeof(TableT)) {
            return PULSE_ERROR_ARGUMENT;
        }
        std::memcpy(out_table, &table, sizeof(TableT));
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL QueryInterfaceStatic(
        void* context,
        PulseStringView name,
        uint32_t version,
        void* out_table,
        uint32_t out_size) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(context);
        if (!self) {
            return PULSE_ERROR_STATE;
        }
        return self->QueryInterface(name, version, out_table, out_size);
    }

    PulseStatus QueryInterface(
        PulseStringView name_view,
        uint32_t version,
        void* out_table,
        uint32_t out_size) noexcept {
        std::string name;
        try {
            name = CopyView(name_view);
        } catch (...) {
            return PULSE_ERROR_ARGUMENT;
        }
        const std::string tier = InterfaceStabilityTier(name);
        const PulseStatus status = ResolveInterface(name, version, out_table, out_size);
        try {
            queries_.push_back(QueryAuditEntry{
                std::move(name),
                version,
                out_size,
                status,
                tier,
            });
        } catch (...) {
        }
        return status;
    }

    PulseStatus ResolveInterface(
        std::string_view name,
        uint32_t version,
        void* out_table,
        uint32_t out_size) noexcept {
        if (!OnOwnerThread()) {
            return PULSE_ERROR_STATE;
        }
        if (name.empty() || !out_table) {
            return PULSE_ERROR_ARGUMENT;
        }
        if (omit_experimental_ui_ && std::string_view(InterfaceStabilityTier(name)) == "experimental") {
            return PULSE_ERROR_UNAVAILABLE;
        }

        if (name == PULSE_COMMANDS_INTERFACE_NAME) {
            return commands_ ? commands_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_ICONS_INTERFACE_NAME) {
            return icons_ ? icons_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_DIAGNOSTICS_INTERFACE_NAME) {
            return diagnostics_ ? diagnostics_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_PICKERS_INTERFACE_NAME) {
            return pickers_ ? pickers_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_WORKBENCH_INTERFACE_NAME) {
            return workbench_ ? workbench_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_TASKS_INTERFACE_NAME) {
            return tasks_ ? tasks_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_FILEOPS_INTERFACE_NAME) {
            return fileops_ ? fileops_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_CONTEXT_INTERFACE_NAME) {
            if (version != PULSE_CONTEXT_VERSION_1) return PULSE_ERROR_ABI;
            return WriteTable(MakeContextTable(), out_table, out_size);
        }
        if (name == PULSE_DISPATCH_INTERFACE) {
            if (version != PULSE_DISPATCH_VERSION) return PULSE_ERROR_ABI;
            const PulseDispatchV1 table{sizeof(PulseDispatchV1), PULSE_DISPATCH_VERSION, this, &DispatchPost, &DispatchCancel};
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_PREVIEW_INTERFACE_NAME) {
            if (version != PULSE_PREVIEW_VERSION_1) return PULSE_ERROR_ABI;
            const PulsePreviewV1 table{sizeof(PulsePreviewV1), PULSE_PREVIEW_VERSION_1, this, &PreviewRegister, &PreviewUnregister};
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_SETTINGS_PAGES_INTERFACE_NAME) {
            if (version != PULSE_SETTINGS_PAGES_VERSION_1) return PULSE_ERROR_ABI;
            const PulseSettingsPagesV1 table{
                sizeof(PulseSettingsPagesV1),
                PULSE_SETTINGS_PAGES_VERSION_1,
                this,
                &SettingsPageCreate,
                &SettingsPageQuery,
                &SettingsPageRemove,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_DIALOGS_INTERFACE_NAME) {
            return dialogs_ ? dialogs_->Query(version, out_table, out_size) : PULSE_ERROR_UNAVAILABLE;
        }
        if (name == PULSE_STAGING_INTERFACE_NAME) {
            if (version != PULSE_STAGING_VERSION_1) return PULSE_ERROR_ABI;
            const PulseStagingV1 table{
                sizeof(PulseStagingV1),
                PULSE_STAGING_VERSION_1,
                this,
                &StagingCreate,
                &StagingCommit,
                &StagingGet,
                &StagingCancel,
                &StagingRelease,
                &StagingChooseDestination,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_FILES_INTERFACE_NAME) {
            if (version != PULSE_FILES_VERSION_1) return PULSE_ERROR_ABI;
            const PulseFilesV1 table{
                sizeof(PulseFilesV1),
                PULSE_FILES_VERSION_1,
                this,
                &FilesCurrentDirectory,
                &FilesList,
                &FilesPrepareRename,
                &FilesCommitRename,
                &FilesTaskGet,
                &FilesTaskCancel,
                &FilesCancelRequest,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_RESOURCES_INTERFACE_NAME) {
            if (version != PULSE_RESOURCES_VERSION_1) return PULSE_ERROR_ABI;
            const PulseResourcesV1 table{
                sizeof(PulseResourcesV1),
                PULSE_RESOURCES_VERSION_1,
                this,
                &ResourcesStat,
                &ResourcesSettingsGet,
                &ResourcesSettingsSet,
                &ResourcesAuditList,
                &ResourcesCancelRequest,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_DIRECTORIES_INTERFACE_NAME) {
            if (version != PULSE_DIRECTORIES_VERSION_1) return PULSE_ERROR_ABI;
            const PulseDirectoriesV1 table{
                sizeof(PulseDirectoriesV1),
                PULSE_DIRECTORIES_VERSION_1,
                this,
                &DirectoriesOpen,
                &DirectoriesRefresh,
                &DirectoriesList,
                &DirectoriesOpenEntry,
                &DirectoriesWatch,
                &DirectoriesUnwatch,
                &DirectoriesClose,
                &DirectoriesCancel,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_UI_MODEL_INTERFACE) {
            if (version != PULSE_UI_MODEL_VERSION) return PULSE_ERROR_ABI;
            const PulseUiModelV1 table{
                sizeof(PulseUiModelV1),
                PULSE_UI_MODEL_VERSION,
                this,
                &UiModelCreate,
                &UiModelUpdate,
                &UiModelSubscribeCommand,
                &UiModelSetStates,
                &UiModelGetState,
                &UiModelUnsubscribe,
                &UiModelClose,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_UI_EVENTS_INTERFACE_NAME) {
            if (version != PULSE_UI_EVENTS_VERSION_1) return PULSE_ERROR_ABI;
            const PulseUiEventsV1 table{
                sizeof(PulseUiEventsV1),
                PULSE_UI_EVENTS_VERSION_1,
                this,
                &UiEventsSubscribeState,
                &UiEventsUnsubscribe,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_UI_DATA_INTERFACE) {
            if (version != PULSE_UI_DATA_VERSION) return PULSE_ERROR_ABI;
            const PulseUiDataV1 table{
                sizeof(PulseUiDataV1),
                PULSE_UI_DATA_VERSION,
                this,
                &UiDataRegisterProvider,
                &UiDataUnregisterProvider,
                &UiDataBind,
                &UiDataUnbind,
                &UiDataRequest,
                &UiDataPublish,
            };
            return WriteTable(table, out_table, out_size);
        }
        if (name == PULSE_UI_MOUNT_INTERFACE) {
            if (version != PULSE_UI_MOUNT_VERSION_1) return PULSE_ERROR_ABI;
            const PulseUiMountV1 table{
                sizeof(PulseUiMountV1),
                PULSE_UI_MOUNT_VERSION_1,
                this,
                &UiMountSet,
                &UiMountGet,
                &UiMountSubscribe,
                &UiMountUnsubscribe,
            };
            return WriteTable(table, out_table, out_size);
        }

        return PULSE_ERROR_UNAVAILABLE;
    }

    // --- pulse.context ---
    static PulseStatus PULSE_CALL ContextGet(void* ctx, PulseContextChangeFn callback, void* user) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !callback) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const std::string first_name = self->selection_.empty() ? std::string{} : self->selection_.front().name;
        const std::string view_mode = "details";
        const PulseContextSnapshot snap{
            sizeof(PulseContextSnapshot),
            PULSE_CONTEXT_INITIAL,
            1,
            1,
            1,
            AsView(self->directory_),
            AsView(first_name),
            AsView(view_mode),
            static_cast<uint64_t>(self->selection_.size()),
            0,
            0,
        };
        callback(user, &snap);
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL ContextSubscribe(
        void* ctx,
        PulseContextChangeFn callback,
        void* user,
        PulseContextSubscriptionId* out_id) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (out_id) *out_id = 0;
        if (!self || !callback || !out_id) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        if (self->context_subs_.size() >= 16) return PULSE_ERROR_UNAVAILABLE;
        const uint64_t id = self->next_handle_++;
        try {
            self->context_subs_.insert(id);
            self->deferred_ui_callbacks_.push_back([self, id, callback, user]() {
                if (self->context_subs_.contains(id)) {
                    ContextGet(self, callback, user);
                }
            });
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
        *out_id = id;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL ContextUnsubscribe(void* ctx, PulseContextSubscriptionId id) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || id == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->context_subs_.erase(id) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL ContextGetSelection(void* ctx, PulseContextSelectionFn callback, void* user) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !callback) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        try {
            std::vector<PulseContextSelectionEntry> entries;
            entries.reserve(self->selection_.size());
            for (const auto& item : self->selection_) {
                entries.push_back(PulseContextSelectionEntry{
                    sizeof(PulseContextSelectionEntry),
                    item.flags,
                    AsView(item.path),
                    AsView(item.name),
                });
            }
            const PulseContextSelectionSnapshot snap{
                sizeof(PulseContextSelectionSnapshot),
                1,
                AsView(self->directory_),
                entries.empty() ? nullptr : entries.data(),
                static_cast<uint32_t>(entries.size()),
            };
            callback(user, &snap);
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    // --- pulse.dispatch (any-thread post/cancel) ---
    static PulseStatus PULSE_CALL DispatchPost(
        void* ctx,
        PulseDispatchCallback callback,
        void* user,
        uint64_t* out_token) noexcept {
        if (out_token) *out_token = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !callback) return PULSE_ERROR_ARGUMENT;
        try {
            std::lock_guard lock(self->dispatch_mutex_);
            if (self->dispatch_closed_ || self->dispatch_queue_.size() >= PULSE_DISPATCH_MAX_PENDING) {
                return PULSE_ERROR_STATE;
            }
            const uint64_t token = self->next_dispatch_token_++;
            self->dispatch_queue_.push_back(DispatchItem{token, callback, user});
            if (out_token) *out_token = token;
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    static PulseStatus PULSE_CALL DispatchCancel(void* ctx, uint64_t token) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || token == 0) return PULSE_ERROR_ARGUMENT;
        std::lock_guard lock(self->dispatch_mutex_);
        const auto it = std::find_if(
            self->dispatch_queue_.begin(),
            self->dispatch_queue_.end(),
            [token](const DispatchItem& item) { return item.token == token; });
        if (it == self->dispatch_queue_.end()) {
            return PULSE_ERROR_STATE;
        }
        self->dispatch_queue_.erase(it);
        return PULSE_OK;
    }

    // --- pulse.preview ---
    static PulseStatus PULSE_CALL PreviewRegister(
        void* ctx,
        const PulsePreviewProvider* provider,
        PulsePreviewProviderId* out_id) noexcept {
        if (out_id) *out_id = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !provider || !out_id || provider->size < sizeof(PulsePreviewProvider) || !provider->callback) {
            return PULSE_ERROR_ARGUMENT;
        }
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        if (self->previews_.size() >= 16) return PULSE_ERROR_UNAVAILABLE;
        try {
            const std::string ext = CopyView(provider->extension);
            if (ext.size() < 2 || ext.size() > 32 || ext[0] != '.') return PULSE_ERROR_ARGUMENT;
            for (size_t i = 1; i < ext.size(); ++i) {
                const char c = ext[i];
                const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                if (!valid) return PULSE_ERROR_ARGUMENT;
            }
            for (const auto& [existing_id, rec] : self->previews_) {
                (void)existing_id;
                if (rec.extension == ext) return PULSE_ERROR_STATE;
            }
            const uint64_t id = self->next_handle_++;
            self->previews_.emplace(id, PreviewProviderRecord{id, ext, provider->callback, provider->user});
            *out_id = id;
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_ARGUMENT;
        }
    }

    static PulseStatus PULSE_CALL PreviewUnregister(void* ctx, PulsePreviewProviderId id) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || id == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->previews_.erase(id) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    // --- pulse.settings.pages ---
    static PulseStatus PULSE_CALL SettingsPageCreate(
        void* ctx,
        PulseStringView title,
        const PulseUiModelTree* model,
        uint64_t* out_page) noexcept {
        if (out_page) *out_page = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !model || !out_page || title.length == 0 || title.length > 128) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        if (self->settings_page_id_ != 0) return PULSE_ERROR_STATE;
        try {
            const uint64_t id = self->next_handle_++;
            PanelRecord rec;
            rec.id = id;
            rec.title = CopyView(title);
            rec.is_settings_page = true;
            if (model->states && model->state_count > 0) {
                for (uint32_t i = 0; i < model->state_count; ++i) {
                    rec.states[CopyView(model->states[i].key)] = CopyView(model->states[i].value);
                }
            }
            self->panels_.emplace(id, std::move(rec));
            self->settings_page_id_ = id;
            *out_page = id;
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_ARGUMENT;
        }
    }

    static PulseStatus PULSE_CALL SettingsPageQuery(
        void* ctx,
        uint64_t page,
        PulseStringView interface_name,
        uint32_t version,
        void* out,
        uint32_t size) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !out || page == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread() || page != self->settings_page_id_) return PULSE_ERROR_STATE;
        const std::string iface = CopyView(interface_name);
        if (iface != PULSE_UI_MODEL_INTERFACE && iface != PULSE_UI_EVENTS_INTERFACE_NAME) {
            return PULSE_ERROR_UNAVAILABLE;
        }
        return self->ResolveInterface(iface, version, out, size);
    }

    static PulseStatus PULSE_CALL SettingsPageRemove(void* ctx, uint64_t page) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || page == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread() || page != self->settings_page_id_) return PULSE_ERROR_STATE;
        if (const auto it = self->panels_.find(page); it != self->panels_.end()) {
            it->second.closed = true;
        }
        self->settings_page_id_ = 0;
        return PULSE_OK;
    }

    // --- pulse.staging ---
    static PulseStatus PULSE_CALL StagingCreate(
        void* ctx,
        PulseStringView destination,
        PulseStageCreateFn callback,
        void*) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !callback || destination.length == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->Live() || self->active_stages_.size() >= 16) return PULSE_ERROR_STATE;
        return PULSE_ERROR_UNAVAILABLE;
    }

    static PulseStatus PULSE_CALL StagingCommit(void*, PulseStageId) noexcept { return PULSE_ERROR_STATE; }
    static PulseStatus PULSE_CALL StagingGet(void*, PulseStageId, PulseStageSnapshotFn, void*) noexcept { return PULSE_ERROR_STATE; }
    static PulseStatus PULSE_CALL StagingCancel(void*, PulseStageId) noexcept { return PULSE_ERROR_STATE; }
    static PulseStatus PULSE_CALL StagingRelease(void*, PulseStageId) noexcept { return PULSE_ERROR_STATE; }
    static PulseStatus PULSE_CALL StagingChooseDestination(void* ctx, PulseStringView initial_path, PulseStageChooseFn cb, void* user) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !cb) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        cb(user, PULSE_OK, initial_path.length ? initial_path : AsView(self->directory_));
        return PULSE_OK;
    }

    // --- pulse.files ---
    static PulseStatus PULSE_CALL FilesCurrentDirectory(
        void* ctx,
        PulseFilesCompletionFn completion,
        void* user,
        PulseFilesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        *out_req = req;
        try {
            self->pending_requests_.insert(req);
            self->deferred_ui_callbacks_.push_back([self, req, completion, user]() {
                if (!self->pending_requests_.erase(req)) return;
                PulseFilesReply reply{};
                reply.size = sizeof(PulseFilesReply);
                reply.operation = PULSE_FILES_CONTEXT;
                reply.request_id = req;
                reply.status = PULSE_OK;
                reply.directory.directory = AsView(self->directory_);
                reply.directory.selection_count = self->selection_.size();
                completion(user, &reply);
            });
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL FilesList(
        void* ctx,
        PulseStringView,
        uint32_t limit,
        PulseFilesCompletionFn completion,
        void* user,
        PulseFilesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req || limit == 0 || limit > 256) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        *out_req = req;
        try {
            self->pending_requests_.insert(req);
            self->deferred_ui_callbacks_.push_back([self, req, completion, user]() {
                if (!self->pending_requests_.erase(req)) return;
                PulseFilesReply reply{};
                reply.size = sizeof(PulseFilesReply);
                reply.operation = PULSE_FILES_LIST;
                reply.request_id = req;
                reply.status = PULSE_OK;
                completion(user, &reply);
            });
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL FilesPrepareRename(
        void*,
        const PulseRenameInput*,
        uint32_t,
        PulseFilesCompletionFn,
        void*,
        PulseFilesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        return PULSE_ERROR_UNAVAILABLE;
    }

    static PulseStatus PULSE_CALL FilesCommitRename(
        void*,
        PulseStringView,
        PulseFilesCompletionFn,
        void*,
        PulseFilesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        return PULSE_ERROR_UNAVAILABLE;
    }

    static PulseStatus PULSE_CALL FilesTaskGet(
        void*,
        uint64_t,
        PulseFilesCompletionFn,
        void*,
        PulseFilesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        return PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL FilesTaskCancel(
        void*,
        uint64_t,
        PulseFilesCompletionFn,
        void*,
        PulseFilesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        return PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL FilesCancelRequest(void* ctx, PulseFilesRequestId req) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || req == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->pending_requests_.erase(req) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    // --- pulse.resources ---
    static PulseStatus PULSE_CALL ResourcesStat(
        void*,
        PulseStringView,
        PulseResourcesCompletionFn,
        void*,
        PulseResourcesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        return PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL ResourcesSettingsGet(
        void* ctx,
        PulseResourcesCompletionFn completion,
        void* user,
        PulseResourcesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        *out_req = req;
        try {
            self->pending_requests_.insert(req);
            self->deferred_ui_callbacks_.push_back([self, req, completion, user]() {
                if (!self->pending_requests_.erase(req)) return;
                std::vector<PulseSetting> pairs;
                pairs.reserve(self->kv_settings_.size());
                for (const auto& [k, v] : self->kv_settings_) {
                    pairs.push_back(PulseSetting{AsView(k), AsView(v)});
                }
                PulseResourcesReply reply{};
                reply.size = sizeof(PulseResourcesReply);
                reply.operation = PULSE_RESOURCES_SETTINGS_GET;
                reply.request_id = req;
                reply.status = PULSE_OK;
                reply.settings.version = self->kv_version_;
                reply.settings.values = pairs.empty() ? nullptr : pairs.data();
                reply.settings.count = static_cast<uint32_t>(pairs.size());
                completion(user, &reply);
            });
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL ResourcesSettingsSet(
        void* ctx,
        PulseStringView key_view,
        PulseStringView value_view,
        uint32_t delete_value,
        uint64_t expected_version,
        PulseResourcesCompletionFn completion,
        void* user,
        PulseResourcesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req || key_view.length == 0 || key_view.length > 64 || value_view.length > 1024) {
            return PULSE_ERROR_ARGUMENT;
        }
        if (delete_value > 1 || (delete_value == 1 && value_view.length != 0)) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        *out_req = req;
        try {
            const std::string key = CopyView(key_view);
            const std::string val = CopyView(value_view);
            self->pending_requests_.insert(req);
            self->deferred_ui_callbacks_.push_back([self, req, key, val, delete_value, expected_version, completion, user]() {
                if (!self->pending_requests_.erase(req)) return;
                PulseResourcesReply reply{};
                reply.size = sizeof(PulseResourcesReply);
                reply.operation = PULSE_RESOURCES_SETTINGS_SET;
                reply.request_id = req;
                if (expected_version != self->kv_version_) {
                    static constexpr std::string_view kConflict = "version_conflict";
                    reply.status = PULSE_ERROR_STATE;
                    reply.error = AsView(kConflict);
                    completion(user, &reply);
                    return;
                }
                if (delete_value == 1) {
                    self->kv_settings_.erase(key);
                } else {
                    self->kv_settings_[key] = val;
                }
                ++self->kv_version_;
                std::vector<PulseSetting> pairs;
                pairs.reserve(self->kv_settings_.size());
                for (const auto& [k, v] : self->kv_settings_) {
                    pairs.push_back(PulseSetting{AsView(k), AsView(v)});
                }
                reply.status = PULSE_OK;
                reply.settings.version = self->kv_version_;
                reply.settings.values = pairs.empty() ? nullptr : pairs.data();
                reply.settings.count = static_cast<uint32_t>(pairs.size());
                completion(user, &reply);
            });
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL ResourcesAuditList(
        void* ctx,
        uint32_t limit,
        PulseResourcesCompletionFn completion,
        void* user,
        PulseResourcesRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req || limit == 0 || limit > 128) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        *out_req = req;
        try {
            self->pending_requests_.insert(req);
            self->deferred_ui_callbacks_.push_back([self, req, completion, user]() {
                if (!self->pending_requests_.erase(req)) return;
                PulseResourcesReply reply{};
                reply.size = sizeof(PulseResourcesReply);
                reply.operation = PULSE_RESOURCES_AUDIT_LIST;
                reply.request_id = req;
                reply.status = PULSE_OK;
                completion(user, &reply);
            });
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL ResourcesCancelRequest(void* ctx, PulseResourcesRequestId req) noexcept {
        return FilesCancelRequest(ctx, req);
    }

    // --- pulse.directories ---
    static PulseStatus PULSE_CALL DirectoriesOpen(
        void* ctx,
        PulseStringView path_view,
        PulseDirectoryCompletionFn completion,
        void* user,
        PulseDirectoryRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req || path_view.length == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        const uint64_t dir_id = self->next_handle_++;
        *out_req = req;
        try {
            const std::string p = CopyView(path_view);
            self->open_directories_[dir_id] = p;
            self->pending_requests_.insert(req);
            self->deferred_ui_callbacks_.push_back([self, req, dir_id, p, completion, user]() {
                if (!self->pending_requests_.erase(req)) return;
                PulseDirectoryReply reply{};
                reply.size = sizeof(PulseDirectoryReply);
                reply.operation = PULSE_DIRECTORY_OPEN;
                reply.request_id = req;
                reply.status = PULSE_OK;
                reply.directory_id = dir_id;
                reply.snapshot_id = 1;
                reply.path = AsView(p);
                reply.next_offset = UINT32_MAX;
                completion(user, &reply);
            });
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_UNAVAILABLE;
        }
    }

    static PulseStatus PULSE_CALL DirectoriesRefresh(
        void* ctx,
        PulseDirectoryId dir_id,
        PulseDirectoryCompletionFn completion,
        void* user,
        PulseDirectoryRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req || !self->open_directories_.contains(dir_id)) return PULSE_ERROR_STATE;
        const std::string p = self->open_directories_[dir_id];
        return DirectoriesOpen(ctx, AsView(p), completion, user, out_req);
    }

    static PulseStatus PULSE_CALL DirectoriesList(
        void* ctx,
        PulseDirectoryId dir_id,
        uint64_t,
        uint32_t,
        uint32_t limit,
        PulseDirectoryCompletionFn completion,
        void* user,
        PulseDirectoryRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !completion || !out_req || limit == 0 || limit > 256) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread() || !self->open_directories_.contains(dir_id)) return PULSE_ERROR_STATE;
        const uint64_t req = self->next_handle_++;
        *out_req = req;
        const std::string p = self->open_directories_[dir_id];
        self->pending_requests_.insert(req);
        self->deferred_ui_callbacks_.push_back([self, req, dir_id, p, completion, user]() {
            if (!self->pending_requests_.erase(req)) return;
            PulseDirectoryReply reply{};
            reply.size = sizeof(PulseDirectoryReply);
            reply.operation = PULSE_DIRECTORY_LIST;
            reply.request_id = req;
            reply.status = PULSE_OK;
            reply.directory_id = dir_id;
            reply.snapshot_id = 1;
            reply.path = AsView(p);
            reply.next_offset = UINT32_MAX;
            completion(user, &reply);
        });
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL DirectoriesOpenEntry(
        void*,
        PulseDirectoryId,
        uint64_t,
        uint64_t,
        PulseDirectoryCompletionFn,
        void*,
        PulseDirectoryRequestId* out_req) noexcept {
        if (out_req) *out_req = 0;
        return PULSE_ERROR_UNAVAILABLE;
    }

    static PulseStatus PULSE_CALL DirectoriesWatch(
        void* ctx,
        PulseDirectoryId dir_id,
        PulseDirectoryChangeFn cb,
        void*,
        PulseDirectoryWatchId* out_watch) noexcept {
        if (out_watch) *out_watch = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !cb || !out_watch) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread() || !self->open_directories_.contains(dir_id)) return PULSE_ERROR_STATE;
        const uint64_t wid = self->next_handle_++;
        self->directory_watches_.insert(wid);
        *out_watch = wid;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL DirectoriesUnwatch(void* ctx, PulseDirectoryWatchId wid) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || wid == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->directory_watches_.erase(wid) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL DirectoriesClose(void* ctx, PulseDirectoryId dir_id) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || dir_id == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->open_directories_.erase(dir_id) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL DirectoriesCancel(void* ctx, PulseDirectoryRequestId req) noexcept {
        return FilesCancelRequest(ctx, req);
    }

    // --- pulse.ui.model / events / data / mount ---
    static PulseStatus PULSE_CALL UiModelCreate(
        void* ctx,
        PulseStringView title,
        const PulseUiModelTree* model,
        uint64_t* out_panel) noexcept {
        if (out_panel) *out_panel = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !model || !out_panel || title.length == 0 || title.length > 128) {
            if (self && self->diagnostics_sink_) {
                self->diagnostics_sink_->Record(
                    PULSE_ERROR_ARGUMENT, "pulse.ui.model", "create", {}, "title", "Invalid title, model, or out_panel");
            }
            return PULSE_ERROR_ARGUMENT;
        }
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        try {
            pulse::extensions::UiTree parsed;
            std::set<std::string> commands;
            std::string error;
            const PulseStatus st = pulse::extensions::native::ConvertUiModel(model, parsed, commands, error);
            if (st != PULSE_OK) {
                if (self->diagnostics_sink_) {
                    self->diagnostics_sink_->RecordParsed(st, "pulse.ui.model", "create", error);
                }
                return st;
            }
            const uint64_t id = self->next_handle_++;
            PanelRecord rec;
            rec.id = id;
            rec.title = CopyView(title);
            for (const auto& state : parsed.state) {
                rec.states[state.key] = state.value;
            }
            self->panels_.emplace(id, std::move(rec));
            *out_panel = id;
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_ARGUMENT;
        }
    }

    static PulseStatus PULSE_CALL UiModelUpdate(void* ctx, uint64_t panel, const PulseUiModelTree* model) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !model || panel == 0) {
            if (self && self->diagnostics_sink_) {
                self->diagnostics_sink_->Record(
                    PULSE_ERROR_ARGUMENT, "pulse.ui.model", "update", {}, "model", "Invalid panel or model pointer");
            }
            return PULSE_ERROR_ARGUMENT;
        }
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed) return PULSE_ERROR_STATE;
        try {
            pulse::extensions::UiTree parsed;
            std::set<std::string> commands;
            std::string error;
            const PulseStatus st = pulse::extensions::native::ConvertUiModel(model, parsed, commands, error);
            if (st != PULSE_OK) {
                if (self->diagnostics_sink_) {
                    self->diagnostics_sink_->RecordParsed(st, "pulse.ui.model", "update", error);
                }
                return st;
            }
            for (const auto& state : parsed.state) {
                if (!it->second.states.contains(state.key)) {
                    it->second.states[state.key] = state.value;
                }
            }
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_ARGUMENT;
        }
    }

    static PulseStatus PULSE_CALL UiModelSubscribeCommand(
        void* ctx,
        uint64_t panel,
        PulseStringView command,
        PulseUiClickFn cb,
        void*,
        uint64_t* out_sub) noexcept {
        if (out_sub) *out_sub = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !cb || !out_sub || command.length == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed) return PULSE_ERROR_STATE;
        const uint64_t sub = self->next_handle_++;
        self->ui_subscriptions_.insert(sub);
        *out_sub = sub;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiModelSetStates(
        void* ctx,
        uint64_t panel,
        const PulseUiState* states,
        uint32_t count) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || panel == 0 || (count > 0 && !states)) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed) return PULSE_ERROR_STATE;
        try {
            for (uint32_t i = 0; i < count; ++i) {
                it->second.states[CopyView(states[i].key)] = CopyView(states[i].value);
            }
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_ARGUMENT;
        }
    }

    static PulseStatus PULSE_CALL UiModelGetState(
        void* ctx,
        uint64_t panel,
        PulseStringView key,
        char* buffer,
        uint32_t capacity,
        uint32_t* needed) noexcept {
        if (needed) *needed = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || panel == 0 || key.length == 0 || !needed) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed) return PULSE_ERROR_STATE;
        const auto sit = it->second.states.find(CopyView(key));
        if (sit == it->second.states.end()) return PULSE_ERROR_ARGUMENT;
        const uint32_t len = static_cast<uint32_t>(sit->second.size());
        *needed = len;
        if (!buffer || capacity == 0) return PULSE_OK;
        if (capacity < len) return PULSE_ERROR_ARGUMENT;
        if (len > 0) std::memcpy(buffer, sit->second.data(), len);
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiModelUnsubscribe(void* ctx, uint64_t sub) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || sub == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->ui_subscriptions_.erase(sub) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL UiModelClose(void* ctx, uint64_t panel) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || panel == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed || it->second.is_settings_page) return PULSE_ERROR_STATE;
        it->second.closed = true;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiEventsSubscribeState(
        void* ctx,
        uint64_t panel,
        PulseStringView,
        uint32_t source_mask,
        PulseUiStateChangedFn cb,
        void*,
        uint64_t* out_token) noexcept {
        if (out_token) *out_token = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !cb || !out_token || source_mask == 0 || (source_mask & ~3u) != 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed) return PULSE_ERROR_STATE;
        const uint64_t tok = self->next_handle_++;
        self->ui_subscriptions_.insert(tok);
        *out_token = tok;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiEventsUnsubscribe(void* ctx, uint64_t token) noexcept {
        return UiModelUnsubscribe(ctx, token);
    }

    static PulseStatus PULSE_CALL UiDataRegisterProvider(
        void* ctx,
        PulseUiDataRequestFn req_fn,
        PulseUiDataCancelFn,
        void*,
        uint64_t* out_provider) noexcept {
        if (out_provider) *out_provider = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !req_fn || !out_provider) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const uint64_t id = self->next_handle_++;
        self->data_providers_.insert(id);
        *out_provider = id;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiDataUnregisterProvider(void* ctx, uint64_t provider) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || provider == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        return self->data_providers_.erase(provider) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL UiDataBind(void* ctx, uint64_t panel, PulseStringView node, uint64_t provider, const PulseUiDataBinding* b) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !b || node.length == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread() || !self->data_providers_.contains(provider)) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        return (it != self->panels_.end() && !it->second.closed) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL UiDataUnbind(void* ctx, uint64_t panel, PulseStringView node) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || node.length == 0) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        return (it != self->panels_.end() && !it->second.closed) ? PULSE_OK : PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL UiDataRequest(void*, uint64_t, PulseStringView, uint32_t) noexcept {
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiDataPublish(void*, uint64_t, const PulseUiDataPage*) noexcept {
        return PULSE_ERROR_STATE;
    }

    static PulseStatus PULSE_CALL UiMountSet(void* ctx, uint64_t panel, uint32_t placement) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || placement > PULSE_UI_MOUNT_WORKSPACE) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed || it->second.is_settings_page) return PULSE_ERROR_STATE;
        it->second.placement = placement;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiMountGet(void* ctx, uint64_t panel, PulseUiMountInfo* info) noexcept {
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !info || info->size != sizeof(PulseUiMountInfo)) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed || it->second.is_settings_page) return PULSE_ERROR_STATE;
        info->placement = it->second.placement;
        info->width_dip = 640;
        info->height_dip = 420;
        info->dpi = 96;
        info->dark = 1;
        info->focused = 1;
        info->reserved = 0;
        info->revision = 1;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiMountSubscribe(
        void* ctx,
        uint64_t panel,
        PulseUiMountChangedFn cb,
        void*,
        uint64_t* out_token) noexcept {
        if (out_token) *out_token = 0;
        auto* self = static_cast<HeadlessHostEnvironment*>(ctx);
        if (!self || !cb || !out_token) return PULSE_ERROR_ARGUMENT;
        if (!self->OnOwnerThread()) return PULSE_ERROR_STATE;
        const auto it = self->panels_.find(panel);
        if (it == self->panels_.end() || it->second.closed || it->second.is_settings_page) return PULSE_ERROR_STATE;
        const uint64_t tok = self->next_handle_++;
        self->ui_subscriptions_.insert(tok);
        *out_token = tok;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL UiMountUnsubscribe(void* ctx, uint64_t token) noexcept {
        return UiModelUnsubscribe(ctx, token);
    }

    std::string directory_;
    std::vector<SelectionItem> selection_;
    bool omit_experimental_ui_ = false;
    DWORD owner_thread_ = 0;
    std::atomic<bool> live_{true};

    pulse::extensions::native::CommandsAdapter* commands_ = nullptr;
    pulse::extensions::native::IconsAdapter* icons_ = nullptr;
    pulse::extensions::native::TasksAdapter* tasks_ = nullptr;
    pulse::extensions::native::FileOpsAdapter* fileops_ = nullptr;
    pulse::extensions::native::DiagnosticsAdapter* diagnostics_ = nullptr;
    pulse::extensions::native::DialogsAdapter* dialogs_ = nullptr;
    pulse::extensions::native::PickersAdapter* pickers_ = nullptr;
    pulse::extensions::native::WorkbenchAdapter* workbench_ = nullptr;
    std::shared_ptr<pulse::extensions::native::DiagnosticsSink> diagnostics_sink_;

    std::vector<QueryAuditEntry> queries_;
    uint64_t next_handle_ = 1;
    std::set<uint64_t> context_subs_;
    std::map<uint64_t, PreviewProviderRecord> previews_;
    std::map<uint64_t, PanelRecord> panels_;
    uint64_t settings_page_id_ = 0;
    std::set<uint64_t> active_stages_;
    std::set<uint64_t> pending_requests_;
    std::map<std::string, std::string> kv_settings_;
    uint64_t kv_version_ = 0;
    std::map<uint64_t, std::string> open_directories_;
    std::set<uint64_t> directory_watches_;
    std::set<uint64_t> ui_subscriptions_;
    std::set<uint64_t> data_providers_;
    std::vector<std::function<void()>> deferred_ui_callbacks_;

    mutable std::mutex dispatch_mutex_;
    bool dispatch_closed_ = false;
    uint64_t next_dispatch_token_ = 1;
    std::deque<DispatchItem> dispatch_queue_;
};

std::vector<std::string> AuditUndeclaredResources(
    const std::vector<QueryAuditEntry>& queries,
    const std::vector<std::string>& declared_resources) {
    const std::set<std::string> declared(declared_resources.begin(), declared_resources.end());
    const auto has_any = [&declared](std::initializer_list<const char*> candidates) {
        for (const char* item : candidates) {
            if (declared.contains(item)) {
                return true;
            }
        }
        return false;
    };

    std::set<std::string> seen_interfaces;
    std::vector<std::string> warnings;
    for (const auto& q : queries) {
        if (q.status != PULSE_OK || !seen_interfaces.insert(q.name).second) {
            continue;
        }
        if (q.name == PULSE_CONTEXT_INTERFACE_NAME && !has_any({"context.read"})) {
            warnings.push_back("Queried pulse.context without declaring resource 'context.read'");
        } else if (q.name == PULSE_DIRECTORIES_INTERFACE_NAME && !has_any({"files.readDirectory"})) {
            warnings.push_back("Queried pulse.directories without declaring resource 'files.readDirectory'");
        } else if (q.name == PULSE_FILEOPS_INTERFACE_NAME && !has_any({"operations.own"})) {
            warnings.push_back("Queried pulse.fileops without declaring resource 'operations.own'");
        } else if (q.name == PULSE_FILES_INTERFACE_NAME &&
                   !has_any({"context.read", "files.readCurrentDirectory", "files.renameCurrentDirectory", "operations.own"})) {
            warnings.push_back("Queried pulse.files without declaring a matching files.* or context.read resource");
        } else if (q.name == PULSE_RESOURCES_INTERFACE_NAME &&
                   !has_any({"files.readCurrentDirectory", "settings.own", "audit.own"})) {
            warnings.push_back("Queried pulse.resources without declaring settings.own, audit.own, or files.readCurrentDirectory");
        }
    }
    return warnings;
}

struct CliOptions {
    fs::path package_dir;
    fs::path manifest_path;
    fs::path library_path;
    fs::path directory_path;
    std::vector<fs::path> selection_paths;
    std::vector<std::string> invoke_commands;
    bool smoke_commands = false;
    bool omit_experimental_ui = false;
    bool skip_hash_verify = false;
    bool force_verify = false;
    bool strict_resources = false;
    bool help = false;
    DWORD timeout_ms = 5000;
    std::string parse_error;
};

CliOptions ParseArgs(int argc, wchar_t** argv) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i] ? argv[i] : L"";
        auto require_next = [&](const char* flag) -> std::optional<std::wstring> {
            if (i + 1 >= argc || !argv[i + 1]) {
                opts.parse_error = std::string("Missing value for ") + flag;
                return std::nullopt;
            }
            return std::wstring(argv[++i]);
        };

        if (arg == L"--help" || arg == L"-h") {
            opts.help = true;
            return opts;
        }
        if (arg == L"--package") {
            if (auto v = require_next("--package")) opts.package_dir = *v;
        } else if (arg == L"--manifest") {
            if (auto v = require_next("--manifest")) opts.manifest_path = *v;
        } else if (arg == L"--library") {
            if (auto v = require_next("--library")) opts.library_path = *v;
        } else if (arg == L"--directory") {
            if (auto v = require_next("--directory")) opts.directory_path = *v;
        } else if (arg == L"--selection") {
            if (auto v = require_next("--selection")) opts.selection_paths.emplace_back(*v);
        } else if (arg == L"--invoke-command") {
            if (auto v = require_next("--invoke-command")) opts.invoke_commands.push_back(pulse::json::Utf8(*v));
        } else if (arg == L"--smoke-commands") {
            opts.smoke_commands = true;
        } else if (arg == L"--omit-experimental-ui") {
            opts.omit_experimental_ui = true;
        } else if (arg == L"--skip-hash-verify") {
            opts.skip_hash_verify = true;
        } else if (arg == L"--verify") {
            opts.force_verify = true;
        } else if (arg == L"--strict-resources") {
            opts.strict_resources = true;
        } else if (arg == L"--timeout-ms") {
            if (auto v = require_next("--timeout-ms")) {
                const long parsed = std::wcstol(v->c_str(), nullptr, 10);
                if (parsed < 100 || parsed > 60000) {
                    opts.parse_error = "--timeout-ms must be between 100 and 60000";
                } else {
                    opts.timeout_ms = static_cast<DWORD>(parsed);
                }
            }
        } else {
            opts.parse_error = "Unknown argument: " + pulse::json::Utf8(arg);
        }
        if (!opts.parse_error.empty()) {
            break;
        }
    }
    return opts;
}

void PrintJsonReport(const Value& report) {
    std::string serialized = pulse::json::Serialize(report);
    serialized.push_back('\n');
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && out != nullptr) {
        DWORD written = 0;
        WriteFile(out, serialized.data(), static_cast<DWORD>(serialized.size()), &written, nullptr);
    } else {
        std::fwrite(serialized.data(), 1, serialized.size(), stdout);
        std::fflush(stdout);
    }
}

int EmitFailure(std::string_view code, std::string_view message) {
    Value report = Value::Object();
    report.set("ok", false);
    report.set("code", code);
    report.set("error", message);
    PrintJsonReport(report);
    return 2;
}

} // namespace

// Headless Low-Integrity thunks so commands_adapter.cpp never imports USER32.dll.
namespace headless_user32_thunks {
BOOL WINAPI StubIsWindow(HWND) noexcept {
    return FALSE;
}
BOOL WINAPI StubInvalidateRect(HWND, const RECT*, BOOL) noexcept {
    return FALSE;
}
} // namespace headless_user32_thunks

extern "C" void* __imp_IsWindow = reinterpret_cast<void*>(&headless_user32_thunks::StubIsWindow);
extern "C" void* __imp_InvalidateRect = reinterpret_cast<void*>(&headless_user32_thunks::StubInvalidateRect);

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto started_at = std::chrono::steady_clock::now();

    const CliOptions opts = ParseArgs(argc, argv);
    if (opts.help) {
        std::fprintf(
            stdout,
            "Usage: pulse_plugin_harness.exe (--package <dir> | --manifest <manifest.json> [--library <plugin.dll>])\n"
            "       [--invoke-command <id>] [--smoke-commands] [--selection <path> ...] [--directory <dir>]\n"
            "       [--omit-experimental-ui] [--skip-hash-verify] [--verify] [--strict-resources] [--timeout-ms <100..60000>]\n");
        return 0;
    }
    if (!opts.parse_error.empty()) {
        return EmitFailure("invalid_arguments", opts.parse_error);
    }

    fs::path manifest_path = opts.manifest_path;
    fs::path package_root;
    if (!opts.package_dir.empty()) {
        std::error_code ec;
        package_root = fs::absolute(opts.package_dir, ec);
        if (ec || !fs::is_directory(package_root, ec)) {
            return EmitFailure("invalid_package_dir", "Staged package directory does not exist");
        }
        if (manifest_path.empty()) {
            manifest_path = package_root / L"manifest.json";
        }
    }
    if (manifest_path.empty()) {
        return EmitFailure("missing_manifest", "Specify --package <dir> or --manifest <manifest.json>");
    }

    std::error_code ec;
    manifest_path = fs::absolute(manifest_path, ec);
    if (ec || !fs::is_regular_file(manifest_path, ec)) {
        return EmitFailure("manifest_not_found", "manifest.json does not exist");
    }
    if (package_root.empty()) {
        package_root = manifest_path.parent_path();
    }

    std::string manifest_bytes;
    if (!ReadFileBytes(manifest_path, manifest_bytes)) {
        return EmitFailure("manifest_read_failed", "Failed to read manifest.json");
    }

    const pulse::extensions::ManifestResult parsed = pulse::extensions::ParseManifest(manifest_bytes);
    if (!parsed) {
        return EmitFailure(parsed.error.empty() ? "invalid_manifest" : parsed.error, "Manifest failed contract validation");
    }
    const pulse::extensions::Manifest& manifest = *parsed.manifest;

    fs::path dll_path = opts.library_path;
    if (dll_path.empty()) {
        if (manifest.native && !manifest.native->library.empty()) {
            dll_path = package_root / pulse::json::Wide(manifest.native->library);
        } else {
            // Look for a single DLL inside package_root when manifest has not been stamped yet.
            std::vector<fs::path> dll_candidates;
            for (const auto& entry : fs::directory_iterator(package_root, ec)) {
                if (!ec && entry.is_regular_file(ec)) {
                    std::wstring ext = entry.path().extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
                    if (ext == L".dll") {
                        dll_candidates.push_back(entry.path());
                    }
                }
            }
            if (dll_candidates.size() == 1) {
                dll_path = dll_candidates.front();
            }
        }
    }
    if (dll_path.empty()) {
        return EmitFailure("missing_library", "No plugin DLL specified; pass --library <path> or stage native.library");
    }
    dll_path = fs::absolute(dll_path, ec);
    if (ec || !fs::is_regular_file(dll_path, ec)) {
        return EmitFailure("library_not_found", "Plugin DLL file does not exist");
    }

    // Prepare selection and directory context.
    std::vector<SelectionItem> selection_items;
    selection_items.reserve(opts.selection_paths.size());
    for (const auto& raw_sel : opts.selection_paths) {
        const fs::path abs_sel = fs::absolute(raw_sel, ec);
        if (ec) {
            return EmitFailure("invalid_selection_path", "Failed to resolve --selection path");
        }
        const bool is_dir = fs::is_directory(abs_sel, ec);
        SelectionItem item;
        item.path = pulse::json::Utf8(abs_sel.wstring());
        item.name = pulse::json::Utf8(abs_sel.filename().wstring());
        if (item.name.empty()) {
            item.name = item.path;
        }
        item.flags = is_dir ? PULSE_CONTEXT_ENTRY_DIRECTORY : 0u;
        selection_items.push_back(std::move(item));
    }

    fs::path active_dir = opts.directory_path;
    if (active_dir.empty()) {
        if (!opts.selection_paths.empty()) {
            active_dir = fs::absolute(opts.selection_paths.front(), ec).parent_path();
        } else {
            active_dir = fs::current_path(ec);
        }
    } else {
        active_dir = fs::absolute(active_dir, ec);
    }
    const std::string active_dir_utf8 = pulse::json::Utf8(active_dir.wstring());

    // Set up headless host environment and real Pulse adapters.
    const HWND synthetic_owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x50554C53u));
    HeadlessHostEnvironment env(active_dir_utf8, selection_items, opts.omit_experimental_ui);

    pulse::extensions::authorization::Registry auth_registry;
    const pulse::extensions::authorization::PackageIdentity pkg_identity{0, manifest.id, "harness-rev-1", 1};
    auth_registry.SyncPackages({{pkg_identity, true}});
    const auto owner_handle = auth_registry.RegisterOwner(reinterpret_cast<uintptr_t>(synthetic_owner));
    const auto binding = owner_handle ? auth_registry.CaptureBinding(pkg_identity, *owner_handle) : std::nullopt;
    if (!binding) {
        return EmitFailure("internal_binding_failed", "Failed to capture headless principal binding");
    }

    std::map<uint64_t, pulse::extensions::native::TaskSnapshot> task_records;
    std::vector<uint64_t> task_order;
    pulse::extensions::native::TasksBackend tasks_backend;
    tasks_backend.create = [&](const pulse::extensions::native::TaskSnapshot& snap) {
        task_records[snap.id] = snap;
        task_order.push_back(snap.id);
        return true;
    };
    tasks_backend.update = [&](const pulse::extensions::native::TaskSnapshot& snap) {
        task_records[snap.id] = snap;
    };
    tasks_backend.finish = [&](const pulse::extensions::native::TaskSnapshot& snap) {
        task_records[snap.id] = snap;
    };
    tasks_backend.cancelled = [](uint64_t) { return false; };
    tasks_backend.release = [](uint64_t) {};

    pulse::extensions::native::FileOpsBackend fileops_backend;
    fileops_backend.live = [&](const pulse::extensions::CallerBinding& b) {
        return env.Live() && auth_registry.ValidateBinding(b);
    };
    fileops_backend.submit = [](const pulse::extensions::CallerBinding&, uint32_t, PulseFileOperationRequestId* out_id) {
        if (out_id) *out_id = 0;
        return PULSE_ERROR_UNAVAILABLE;
    };
    fileops_backend.get = [](const pulse::extensions::CallerBinding&, PulseFileOperationRequestId, PulseFileOperation*) {
        return PULSE_ERROR_STATE;
    };
    fileops_backend.cancel = [](const pulse::extensions::CallerBinding&, PulseFileOperationRequestId) {
        return PULSE_ERROR_STATE;
    };
    fileops_backend.release = [](const pulse::extensions::CallerBinding&) {};

    pulse::extensions::SyncManifestIcons(manifest);
    auto diagnostics_sink = std::make_shared<pulse::extensions::native::DiagnosticsSink>();
    pulse::extensions::native::DiagnosticsAdapter diagnostics(
        diagnostics_sink,
        [&env]() { return env.Live(); });

    struct HeadlessDialogState {
        bool open = false;
        std::map<std::string, std::string> states;
        std::function<void(int, std::vector<pulse::extensions::UiStateEntry>)> on_closed;
    } headless_dialog;

    pulse::extensions::native::DialogsBackend dialogs_backend;
    dialogs_backend.valid_owner = [&env]() { return env.Live(); };
    dialogs_backend.show = [&](pulse::extensions::native::DialogPresentationSpec spec) {
        if (!env.Live() || headless_dialog.open) return false;
        headless_dialog.open = true;
        headless_dialog.states.clear();
        if (spec.form.has_value()) {
            for (const auto& entry : spec.form->state) {
                headless_dialog.states[entry.key] = entry.value;
            }
        }
        headless_dialog.on_closed = std::move(spec.on_closed);
        env.DeferUiCallback([&headless_dialog]() {
            if (!headless_dialog.open) return;
            headless_dialog.open = false;
            std::vector<pulse::extensions::UiStateEntry> final_states;
            for (const auto& [k, v] : headless_dialog.states) {
                final_states.push_back({k, v});
            }
            auto cb = std::move(headless_dialog.on_closed);
            headless_dialog.on_closed = nullptr;
            if (cb) {
                cb(-1, std::move(final_states));
            }
        });
        return true;
    };
    dialogs_backend.set_states = [&headless_dialog](const std::vector<pulse::extensions::UiStateEntry>& updates) {
        if (!headless_dialog.open) return false;
        for (const auto& item : updates) {
            headless_dialog.states[item.key] = item.value;
        }
        return true;
    };
    dialogs_backend.update_form = [&headless_dialog](const pulse::extensions::UiTree& form) {
        if (!headless_dialog.open) return false;
        for (const auto& item : form.state) {
            if (!headless_dialog.states.contains(item.key)) {
                headless_dialog.states[item.key] = item.value;
            }
        }
        return true;
    };
    dialogs_backend.get_state = [&headless_dialog](const std::string& key) -> std::optional<std::string> {
        if (!headless_dialog.open) return std::nullopt;
        const auto it = headless_dialog.states.find(key);
        return it != headless_dialog.states.end() ? std::optional<std::string>(it->second) : std::nullopt;
    };
    dialogs_backend.refresh_form = []() {};
    dialogs_backend.dismiss = [&headless_dialog]() {
        if (!headless_dialog.open) return;
        headless_dialog.open = false;
        std::vector<pulse::extensions::UiStateEntry> final_states;
        for (const auto& [k, v] : headless_dialog.states) {
            final_states.push_back({k, v});
        }
        auto cb = std::move(headless_dialog.on_closed);
        headless_dialog.on_closed = nullptr;
        if (cb) {
            cb(-1, std::move(final_states));
        }
    };
    dialogs_backend.destroy = [&headless_dialog]() {
        headless_dialog.open = false;
        headless_dialog.on_closed = nullptr;
        headless_dialog.states.clear();
    };
    dialogs_backend.is_open = [&headless_dialog]() { return headless_dialog.open; };
    dialogs_backend.active_hwnd = [&headless_dialog]() -> HWND {
        return headless_dialog.open ? reinterpret_cast<HWND>(static_cast<uintptr_t>(0xD1A106u)) : nullptr;
    };
    dialogs_backend.set_appearance = [](bool, D2D1_COLOR_F) {};

    pulse::extensions::native::DialogsAdapter dialogs(
        std::move(dialogs_backend),
        [&env]() { return env.Live(); },
        diagnostics_sink);

    pulse::extensions::native::PickersBackend pickers_backend;
    pickers_backend.valid_owner = [&env]() { return env.Live(); };
    pickers_backend.launch = [&env](
                                 pulse::extensions::native::PickerLaunchSpec,
                                 std::function<void(pulse::extensions::native::PickerBackendCompletion)> done) {
        if (!env.Live()) return false;
        env.DeferUiCallback([done = std::move(done)]() mutable {
            if (done) {
                pulse::extensions::native::PickerBackendCompletion res;
                res.outcome = PULSE_PICKER_CANCELLED;
                done(std::move(res));
            }
        });
        return true;
    };
    pickers_backend.cancel = [](uint64_t) {};

    pulse::extensions::native::PickersAdapter pickers(
        std::move(pickers_backend),
        [&env]() { return env.Live(); },
        diagnostics_sink);

    pulse::extensions::native::WorkbenchBackend workbench_backend;
    workbench_backend.valid_owner = [&env]() { return env.Live(); };
    workbench_backend.show_snack = [](pulse::extensions::native::SnackPresentationSpec) { return true; };
    workbench_backend.show_notice = [](pulse::extensions::native::NoticePresentationSpec) { return true; };
    workbench_backend.reveal_path = [](const std::wstring&, uint32_t) { return PULSE_OK; };
    workbench_backend.show_text_diff = [](const std::wstring&, const std::wstring&) { return PULSE_OK; };
    workbench_backend.set_clipboard_text = [](const std::wstring&) { return PULSE_OK; };

    pulse::extensions::native::WorkbenchAdapter workbench(
        std::move(workbench_backend),
        [&env]() { return env.Live(); },
        diagnostics_sink);

    pulse::extensions::native::IconsAdapter icons(
        manifest.id,
        [&env]() { return env.Live(); });
    pulse::extensions::native::CommandsAdapter commands(
        synthetic_owner,
        manifest.id,
        env.MakeContextTable(),
        [&env]() { return env.Live(); });
    pulse::extensions::native::TasksAdapter tasks(
        synthetic_owner,
        [&env]() { return env.Live(); },
        std::move(tasks_backend),
        {},
        manifest.id);
    pulse::extensions::native::FileOpsAdapter fileops(*binding, std::move(fileops_backend));

    env.AttachAdapters(
        &commands,
        &icons,
        &tasks,
        &fileops,
        &diagnostics,
        &dialogs,
        &pickers,
        &workbench,
        diagnostics_sink);

    // Decide verified vs unverified load.
    const bool strict_staged_load = (!opts.package_dir.empty() || opts.force_verify) && !opts.skip_hash_verify;
    bool use_verified_load = false;
    if (!opts.skip_hash_verify && manifest.native.has_value()) {
        if (strict_staged_load) {
            use_verified_load = true;
        } else {
            const std::string actual_sha = Sha256FileHex(dll_path);
            bool sidecars_present = true;
            for (const auto& sidecar : manifest.native->files) {
                const fs::path sidecar_path = dll_path.parent_path() / pulse::json::Wide(sidecar.path);
                if (!fs::is_regular_file(sidecar_path, ec) || Sha256FileHex(sidecar_path) != sidecar.sha256) {
                    sidecars_present = false;
                    break;
                }
            }
            use_verified_load = (!actual_sha.empty() && actual_sha == manifest.native->sha256 && sidecars_present);
        }
    } else if (strict_staged_load && !manifest.native.has_value()) {
        return EmitFailure("missing_native_runtime", "Staged package manifest.json is missing 'native' runtime block");
    }

    std::vector<std::string> errors;
    PulseStatus load_status = PULSE_OK;
    PulseStatus activate_status = PULSE_OK;
    PulseStatus deactivate_status = PULSE_OK;
    PulseStatus close_status = PULSE_OK;

    Value registered_commands_json = Value::Array();
    Value contributed_commands_json = Value::Array();
    Value command_invocations_json = Value::Array();

    {
        pulse::extensions::native::Module mod;
        mod.SetPackageId(manifest.id);
        const PulseHostV1 host_table = env.MakeHostTable();

        if (use_verified_load) {
            load_status = mod.LoadVerified(dll_path.wstring(), *manifest.native, host_table);
        } else {
            load_status = mod.Load(dll_path.wstring(), host_table);
        }

        if (load_status != PULSE_OK) {
            errors.push_back(std::string("Plugin load/entry failed with ") + StatusName(load_status));
        } else {
            env.PumpDeferred();
            tasks.Poll();

            activate_status = mod.Activate();
            if (activate_status != PULSE_OK) {
                errors.push_back(std::string("Plugin activate failed with ") + StatusName(activate_status));
            } else {
                env.PumpDeferred();
                tasks.Poll();

                // Snapshot registered commands.
                const auto entries = pulse::extensions::native::CommandsAdapter::Entries(synthetic_owner);
                std::set<std::string> runtime_identities;
                for (const auto& cmd : entries) {
                    runtime_identities.insert(cmd.identity);
                    const std::string prefix = "native:" + manifest.id + ":";
                    const std::string local_id = cmd.identity.starts_with(prefix)
                        ? cmd.identity.substr(prefix.size())
                        : cmd.identity;
                    Value item = Value::Object();
                    item.set("id", local_id);
                    item.set("identity", cmd.identity);
                    item.set("title", cmd.title);
                    item.set("group", cmd.group);
                    item.set("detail", cmd.detail);
                    item.set("enabledForSelection", cmd.enabled);
                    registered_commands_json.push(std::move(item));
                }

                // Audit static manifest.contributes.commands vs runtime registrations.
                for (const auto& contrib : manifest.command_contributions) {
                    const std::string expected_identity = "native:" + manifest.id + ":" + contrib.id;
                    const bool matched = runtime_identities.contains(expected_identity);
                    Value c = Value::Object();
                    c.set("id", contrib.id);
                    c.set("identity", expected_identity);
                    c.set("registeredAtRuntime", matched);
                    contributed_commands_json.push(std::move(c));
                    if (!matched) {
                        errors.push_back("Contributed static command '" + contrib.id + "' was not registered at runtime");
                    }
                }

                // Build command invocation list.
                std::vector<std::string> targets = opts.invoke_commands;
                if (opts.smoke_commands) {
                    const std::string prefix = "native:" + manifest.id + ":";
                    for (const auto& cmd : entries) {
                        if (cmd.enabled) {
                            const std::string local_id = cmd.identity.starts_with(prefix)
                                ? cmd.identity.substr(prefix.size())
                                : cmd.identity;
                            if (std::find(targets.begin(), targets.end(), local_id) == targets.end()) {
                                targets.push_back(local_id);
                            }
                        }
                    }
                }

                for (const auto& target_cmd : targets) {
                    const std::string identity = target_cmd.starts_with("native:")
                        ? target_cmd
                        : ("native:" + manifest.id + ":" + target_cmd);

                    const auto current_entries = pulse::extensions::native::CommandsAdapter::Entries(synthetic_owner);
                    const auto it = std::find_if(
                        current_entries.begin(),
                        current_entries.end(),
                        [&identity](const pulse::extensions::native::CommandInfo& info) {
                            return info.identity == identity;
                        });

                    Value inv = Value::Object();
                    inv.set("command", target_cmd);
                    inv.set("identity", identity);

                    if (it == current_entries.end()) {
                        inv.set("invoked", false);
                        inv.set("reason", "command_not_found");
                        errors.push_back("Requested command '" + target_cmd + "' is not registered");
                        command_invocations_json.push(std::move(inv));
                        continue;
                    }
                    if (!it->enabled) {
                        inv.set("invoked", false);
                        inv.set("reason", "selection_mismatch");
                        errors.push_back("Command '" + target_cmd + "' is disabled for the provided --selection");
                        command_invocations_json.push(std::move(inv));
                        continue;
                    }

                    const size_t tasks_before = task_order.size();
                    const bool invoke_ok = pulse::extensions::native::CommandsAdapter::InvokeIdentity(synthetic_owner, identity);
                    inv.set("invoked", invoke_ok);

                    // Drain tasks and deferred callbacks up to timeout_ms.
                    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts.timeout_ms);
                    bool timed_out = false;
                    for (;;) {
                        tasks.Poll();
                        env.PumpDeferred();
                        tasks.Poll();
                        if (tasks.ActiveWorkerCount() == 0 &&
                            tasks.QueuedCount() == 0 &&
                            !tasks.CallbackActive() &&
                            !env.HasPendingDeferred()) {
                            break;
                        }
                        if (std::chrono::steady_clock::now() >= drain_deadline) {
                            timed_out = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }

                    inv.set("timedOut", timed_out);
                    Value spawned = Value::Array();
                    for (size_t idx = tasks_before; idx < task_order.size(); ++idx) {
                        const uint64_t tid = task_order[idx];
                        const auto& snap = task_records[tid];
                        Value t = Value::Object();
                        t.set("id", snap.id);
                        t.set("title", snap.title);
                        t.set("type", snap.type);
                        t.set("state", TaskStateName(snap.state));
                        t.set("percent", static_cast<uint64_t>(snap.percent));
                        t.set("result", snap.result);
                        t.set("resultPath", snap.result_path);
                        t.set("error", snap.error);
                        t.set("detail", snap.detail);
                        t.set("log", snap.log);
                        spawned.push(std::move(t));
                        if (snap.state == PULSE_TASK_FAILED) {
                            errors.push_back("Task '" + snap.title + "' failed: " + (snap.error.empty() ? "unspecified error" : snap.error));
                        }
                    }
                    inv.set("tasks", std::move(spawned));
                    if (!invoke_ok) {
                        errors.push_back("Failed to invoke command '" + target_cmd + "'");
                    }
                    if (timed_out) {
                        errors.push_back("Timed out waiting for tasks spawned by command '" + target_cmd + "'");
                    }
                    command_invocations_json.push(std::move(inv));
                }
            }

            deactivate_status = mod.Deactivate();
            if (deactivate_status != PULSE_OK) {
                errors.push_back(std::string("Plugin deactivate failed with ") + StatusName(deactivate_status));
            }
            env.PumpDeferred();
            tasks.Poll();
        }

        // Stop host adapters before final module Close (mirrors host session teardown order).
        pickers.Close();
        dialogs.Close();
        workbench.Close();
        diagnostics.Close();
        commands.Close();
        icons.Close();
        tasks.Close();
        fileops.Close();
        env.Close();
        if (owner_handle) {
            auth_registry.UnregisterOwner(*owner_handle);
        }
        pulse::extensions::native::CommandsAdapter::ClearStaticOwner(synthetic_owner);
        pulse::extensions::icons::GlobalRegistry().ClearManifestIconsForMissingPackages({});

        close_status = mod.Close();
        if (close_status != PULSE_OK) {
            errors.push_back(std::string("Plugin close/unload failed with ") + StatusName(close_status));
        }
    }

    // Verify clean unload & zero lingering file locks.
    HMODULE lingering_module = nullptr;
    const BOOL still_mapped = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        dll_path.c_str(),
        &lingering_module);
    const bool module_unloaded = (!still_mapped && lingering_module == nullptr);
    if (!module_unloaded) {
        errors.push_back("Plugin DLL remained mapped in process address space after Module::Close()");
    }

    std::vector<std::string> locked_files;
    if (!CheckExclusiveFileUnlock(dll_path, 50)) {
        locked_files.push_back(pulse::json::Utf8(dll_path.filename().wstring()));
    }
    if (manifest.native.has_value()) {
        for (const auto& sidecar : manifest.native->files) {
            const fs::path sidecar_path = dll_path.parent_path() / pulse::json::Wide(sidecar.path);
            if (fs::is_regular_file(sidecar_path, ec) && !CheckExclusiveFileUnlock(sidecar_path, 100)) {
                locked_files.push_back(sidecar.path);
            }
        }
    }
    const bool file_locks_released = locked_files.empty();
    if (!file_locks_released) {
        errors.push_back("Lingering file lock detected after plugin unload (possible leaked handle or orphan worker process)");
    }

    const size_t active_workers = tasks.ActiveWorkerCount();
    const size_t queued_tasks = tasks.QueuedCount();
    const bool unload_clean = module_unloaded && file_locks_released && active_workers == 0 && queued_tasks == 0;

    // Audit queried interfaces, stability tiers, and resource disclosures.
    Value queries_json = Value::Array();
    std::set<std::string> frozen_used;
    std::set<std::string> experimental_used;
    for (const auto& q : env.Queries()) {
        Value item = Value::Object();
        item.set("name", q.name);
        item.set("version", static_cast<uint64_t>(q.version));
        item.set("outSize", static_cast<uint64_t>(q.out_size));
        item.set("status", StatusName(q.status));
        item.set("stabilityTier", q.stability_tier);
        queries_json.push(std::move(item));
        if (q.status == PULSE_OK) {
            if (q.stability_tier == "frozen") frozen_used.insert(q.name);
            if (q.stability_tier == "experimental") experimental_used.insert(q.name);
        }
    }

    const std::vector<std::string> resource_warnings = AuditUndeclaredResources(env.Queries(), manifest.resources);
    if (opts.strict_resources) {
        for (const auto& w : resource_warnings) {
            errors.push_back(w);
        }
    }

    Value previews_json = Value::Array();
    for (const auto& [pid, rec] : env.Previews()) {
        (void)pid;
        previews_json.push(rec.extension);
    }

    Value frozen_json = Value::Array();
    for (const auto& name : frozen_used) frozen_json.push(name);
    Value experimental_json = Value::Array();
    for (const auto& name : experimental_used) experimental_json.push(name);
    Value warnings_json = Value::Array();
    for (const auto& w : resource_warnings) warnings_json.push(w);
    Value locked_json = Value::Array();
    for (const auto& f : locked_files) locked_json.push(f);
    Value errors_json = Value::Array();
    for (const auto& e : errors) errors_json.push(e);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count();

    const bool ok = errors.empty() && load_status == PULSE_OK && activate_status == PULSE_OK &&
                    deactivate_status == PULSE_OK && close_status == PULSE_OK && unload_clean;

    Value lifecycle = Value::Object();
    lifecycle.set("verifiedLoad", use_verified_load);
    lifecycle.set("loadStatus", StatusName(load_status));
    lifecycle.set("activateStatus", StatusName(activate_status));
    lifecycle.set("deactivateStatus", StatusName(deactivate_status));
    lifecycle.set("closeStatus", StatusName(close_status));

    Value unload = Value::Object();
    unload.set("unloadClean", unload_clean);
    unload.set("moduleUnloaded", module_unloaded);
    unload.set("fileLocksReleased", file_locks_released);
    unload.set("lockedFiles", std::move(locked_json));
    unload.set("activeWorkers", static_cast<uint64_t>(active_workers));
    unload.set("queuedTasks", static_cast<uint64_t>(queued_tasks));

    Value tiers = Value::Object();
    tiers.set("frozen", std::move(frozen_json));
    tiers.set("experimental", std::move(experimental_json));
    tiers.set("experimentalUiOmitted", opts.omit_experimental_ui);

    Value report = Value::Object();
    report.set("ok", ok);
    report.set("id", manifest.id);
    report.set("name", manifest.name);
    report.set("sdkPreview", static_cast<uint64_t>(PULSE_SDK_PREVIEW));
    report.set("elapsedMs", static_cast<int64_t>(elapsed_ms));
    report.set("unloadClean", unload_clean);
    report.set("lifecycle", std::move(lifecycle));
    report.set("unload", std::move(unload));
    report.set("queriedInterfaces", std::move(queries_json));
    report.set("stabilityTiers", std::move(tiers));
    report.set("undeclaredResourceWarnings", std::move(warnings_json));
    report.set("registeredCommands", std::move(registered_commands_json));
    report.set("contributedCommands", std::move(contributed_commands_json));
    report.set("registeredPreviews", std::move(previews_json));
    report.set("openPanelsAtDeactivate", static_cast<uint64_t>(env.OpenUserPanelCount()));
    report.set("settingsPagesRegistered", static_cast<uint64_t>(env.OpenSettingsPageCount()));
    report.set("commandInvocations", std::move(command_invocations_json));
    report.set("errors", std::move(errors_json));

    PrintJsonReport(report);
    return ok ? 0 : 2;
}
