#include "pickers_adapter.h"
#include "diagnostics_adapter.h"
#include <algorithm>
#include <cstring>
#include <set>

namespace pulse::extensions::native {
namespace {

bool ValidUtf8Wide(PulseStringView view, size_t max_bytes, bool required, std::wstring& out) {
    out.clear();
    if (!view.data) return !required && view.length == 0;
    if (view.length > max_bytes || (required && view.length == 0)) return false;
    if (std::memchr(view.data, 0, view.length) != nullptr) return false;
    if (view.length == 0) return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                                          static_cast<int>(view.length), nullptr, 0);
    if (count <= 0) return false;
    out.resize(static_cast<size_t>(count));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, view.data,
                        static_cast<int>(view.length), out.data(), count);
    return true;
}

bool ValidQuickPickId(PulseStringView view, std::string& out) {
    out.clear();
    if (!view.data || view.length == 0 || view.length > 128) return false;
    out.assign(view.data, view.length);
    return std::all_of(out.begin(), out.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == ':' || c == '/' || c == '-';
    });
}

bool ValidFileName(const std::wstring& name) {
    if (name.empty()) return true;
    if (name == L"." || name == L"..") return false;
    for (const wchar_t c : name) {
        if (c < 0x20 || c == L'/' || c == L'\\' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            return false;
    }
    return true;
}

bool ValidExtension(const std::wstring& ext) {
    if (ext.empty()) return true;
    if (ext.front() == L'.') return false;
    for (const wchar_t c : ext) {
        if (c <= 0x20 || c == L'/' || c == L'\\' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            return false;
    }
    return true;
}

PulseStringView View(const std::string& s) noexcept {
    return {s.empty() ? "" : s.data(), static_cast<uint32_t>(s.size())};
}

} // namespace

struct PickersAdapter::State : std::enable_shared_from_this<State> {
    struct ActiveRequest {
        uint64_t id = 0;
        PulsePickerResultFn callback = nullptr;
        void* user = nullptr;
        std::shared_ptr<SharedEventHandle> cancel_event;
        bool cancel_requested = false;
    };

    DWORD thread = GetCurrentThreadId();
    PickersBackend backend;
    std::function<bool()> alive;
    std::shared_ptr<DiagnosticsSink> diagnostics;
    bool closed = false;
    int callbacks = 0;
    uint64_t next_id = 1;
    ActiveRequest active;

    State(PickersBackend impl, std::function<bool()> live, std::shared_ptr<DiagnosticsSink> sink)
        : backend(std::move(impl)), alive(std::move(live)), diagnostics(std::move(sink)) {}

    bool Valid() const noexcept {
        return GetCurrentThreadId() == thread && !closed &&
               (!backend.valid_owner || backend.valid_owner()) &&
               (!alive || alive());
    }

    PulseStatus Fail(PulseStatus status, std::string_view op, std::string_view subject,
                     std::string_view field, std::string_view msg) const {
        if (diagnostics) diagnostics->Record(status, PULSE_PICKERS_INTERFACE_NAME, op, subject, field, msg);
        return status;
    }

    template<class F> static PulseStatus Guard(void* context, F action) noexcept {
        if (!context) return PULSE_ERROR_ARGUMENT;
        try {
            auto self = static_cast<State*>(context)->shared_from_this();
            return self->Valid() ? action(*self) : PULSE_ERROR_STATE;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }

    bool ParseFilters(std::string_view op, const PulsePickerFilter* filters, uint32_t count,
                      uint32_t default_index, std::vector<PickerFilterSpec>& out) const {
        out.clear();
        if (count > 32 || (count && !filters)) {
            Fail(PULSE_ERROR_ARGUMENT, op, "", "filters", "filter_count must be 0..32 with a valid filters pointer");
            return false;
        }
        if (count > 0 && default_index >= count) {
            Fail(PULSE_ERROR_ARGUMENT, op, "", "default_filter_index", "default_filter_index is out of range");
            return false;
        }
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto& f = filters[i];
            const std::string sub = "filters[" + std::to_string(i) + "]";
            if (f.size < sizeof(PulsePickerFilter) || f.reserved != 0) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "size", "invalid PulsePickerFilter size or reserved field");
                return false;
            }
            PickerFilterSpec item;
            if (!ValidUtf8Wide(f.label, 128, true, item.label)) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "label", "filter label must be 1..128 valid UTF-8 bytes");
                return false;
            }
            if (!ValidUtf8Wide(f.pattern, 256, true, item.pattern) ||
                item.pattern.find_first_of(L"/\\") != std::wstring::npos) {
                Fail(PULSE_ERROR_ARGUMENT, op, sub, "pattern",
                     "filter pattern must be 1..256 valid UTF-8 bytes without path separators");
                return false;
            }
            out.push_back(std::move(item));
        }
        return true;
    }

    void Complete(uint64_t request_id, PickerBackendCompletion result) noexcept {
        if (GetCurrentThreadId() != thread || active.id != request_id) return;
        const auto finished = std::move(active);
        active = ActiveRequest{};
        if (!Valid() || !finished.callback) return;

        if (finished.cancel_requested && result.outcome != PULSE_PICKER_ACCEPTED) {
            result.outcome = PULSE_PICKER_CANCELLED;
        }
        if (finished.cancel_requested && result.outcome == PULSE_PICKER_ACCEPTED) {
            result.outcome = PULSE_PICKER_CANCELLED;
            result.paths.clear();
            result.selected_id.clear();
        }
        if (result.outcome == PULSE_PICKER_FAILED && diagnostics && !result.error.empty()) {
            diagnostics->Record(PULSE_ERROR_INITIALIZATION, PULSE_PICKERS_INTERFACE_NAME,
                                "complete", "", "", result.error);
        }

        std::vector<PulseStringView> path_views;
        if (result.outcome == PULSE_PICKER_ACCEPTED && !result.paths.empty()) {
            path_views.reserve(result.paths.size());
            for (const auto& p : result.paths) path_views.push_back(View(p));
        }
        PulsePickerResult payload{};
        payload.size = sizeof(payload);
        payload.outcome = result.outcome;
        payload.request_id = request_id;
        payload.filter_index = result.outcome == PULSE_PICKER_ACCEPTED ? result.filter_index : 0u;
        payload.path_count = static_cast<uint32_t>(path_views.size());
        payload.paths = path_views.empty() ? nullptr : path_views.data();
        payload.selected_id = result.outcome == PULSE_PICKER_ACCEPTED ? View(result.selected_id) : PulseStringView{"", 0};
        payload.error = result.outcome == PULSE_PICKER_FAILED ? View(result.error) : PulseStringView{"", 0};

        ++callbacks;
        try { finished.callback(finished.user, &payload); } catch (...) {}
        --callbacks;
    }

    PulseStatus Launch(std::string_view op, PickerLaunchSpec spec, PulsePickerResultFn callback,
                       void* user, PulsePickerRequestId* out_id) {
        if (active.id != 0)
            return Fail(PULSE_ERROR_STATE, op, "", "",
                        "another picker or quick_pick request is already active for this session");
        if (!backend.launch)
            return Fail(PULSE_ERROR_UNAVAILABLE, op, "", "", "picker host backend is unavailable");

        HANDLE raw_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!raw_event)
            return Fail(PULSE_ERROR_INITIALIZATION, op, "", "", "failed to allocate picker cancellation event");

        const uint64_t id = next_id++;
        auto cancel_handle = std::make_shared<SharedEventHandle>(raw_event);
        spec.request_id = id;
        spec.cancel_event = cancel_handle;
        active = ActiveRequest{id, callback, user, cancel_handle, false};

        std::weak_ptr<State> weak = shared_from_this();
        const bool launched = backend.launch(std::move(spec), [weak, id](PickerBackendCompletion completion) {
            if (auto self = weak.lock()) self->Complete(id, std::move(completion));
        });
        if (!launched) {
            if (active.id == id) active = ActiveRequest{};
            return Fail(PULSE_ERROR_INITIALIZATION, op, "", "", "failed to schedule picker presentation");
        }
        *out_id = id;
        return PULSE_OK;
    }

    static PulseStatus PULSE_CALL OpenFiles(void* context, const PulseOpenFilesRequest* request,
                                            PulsePickerResultFn callback, void* user,
                                            PulsePickerRequestId* request_id) noexcept {
        if (request_id) *request_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || !callback || !request_id || request->size < sizeof(PulseOpenFilesRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_files", "", "size",
                                 "null request/callback/request_id or invalid PulseOpenFilesRequest size");
            constexpr uint32_t kAllowed = PULSE_PICKER_FLAG_MULTISELECT | PULSE_PICKER_FLAG_SHOW_HIDDEN;
            if ((request->flags & ~kAllowed) != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_files", "", "flags",
                                 "unsupported flag for open_files");
            PickerLaunchSpec spec;
            spec.kind = PickerRequestKind::OpenFiles;
            spec.flags = request->flags;
            spec.default_filter_index = request->default_filter_index;
            if (!ValidUtf8Wide(request->title, 128, false, spec.title) ||
                !ValidUtf8Wide(request->ok_label, 64, false, spec.ok_label) ||
                !ValidUtf8Wide(request->initial_path, 32768, false, spec.initial_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "open_files", "", "title",
                                 "invalid UTF-8 or length in title, ok_label, or initial_path");
            if (!self.ParseFilters("open_files", request->filters, request->filter_count,
                                   request->default_filter_index, spec.filters))
                return PULSE_ERROR_ARGUMENT;
            return self.Launch("open_files", std::move(spec), callback, user, request_id);
        });
    }

    static PulseStatus PULSE_CALL SaveFile(void* context, const PulseSaveFileRequest* request,
                                           PulsePickerResultFn callback, void* user,
                                           PulsePickerRequestId* request_id) noexcept {
        if (request_id) *request_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || !callback || !request_id || request->size < sizeof(PulseSaveFileRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "save_file", "", "size",
                                 "null request/callback/request_id or invalid PulseSaveFileRequest size");
            constexpr uint32_t kAllowed = PULSE_PICKER_FLAG_SHOW_HIDDEN | PULSE_PICKER_FLAG_NO_OVERWRITE_PROMPT;
            if ((request->flags & ~kAllowed) != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "save_file", "", "flags",
                                 "unsupported flag for save_file (multiselect is not allowed)");
            PickerLaunchSpec spec;
            spec.kind = PickerRequestKind::SaveFile;
            spec.flags = request->flags;
            spec.default_filter_index = request->default_filter_index;
            if (!ValidUtf8Wide(request->title, 128, false, spec.title) ||
                !ValidUtf8Wide(request->ok_label, 64, false, spec.ok_label) ||
                !ValidUtf8Wide(request->initial_path, 32768, false, spec.initial_path) ||
                !ValidUtf8Wide(request->default_name, 255, false, spec.default_name) ||
                !ValidUtf8Wide(request->default_extension, 32, false, spec.default_extension))
                return self.Fail(PULSE_ERROR_ARGUMENT, "save_file", "", "default_name",
                                 "invalid UTF-8 or length in save_file strings");
            if (!ValidFileName(spec.default_name))
                return self.Fail(PULSE_ERROR_ARGUMENT, "save_file", "", "default_name",
                                 "default_name contains invalid path or filename characters");
            if (!ValidExtension(spec.default_extension))
                return self.Fail(PULSE_ERROR_ARGUMENT, "save_file", "", "default_extension",
                                 "default_extension must not start with '.' or contain path characters");
            if (!self.ParseFilters("save_file", request->filters, request->filter_count,
                                   request->default_filter_index, spec.filters))
                return PULSE_ERROR_ARGUMENT;
            return self.Launch("save_file", std::move(spec), callback, user, request_id);
        });
    }

    static PulseStatus PULSE_CALL PickFolders(void* context, const PulsePickFoldersRequest* request,
                                              PulsePickerResultFn callback, void* user,
                                              PulsePickerRequestId* request_id) noexcept {
        if (request_id) *request_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || !callback || !request_id || request->size < sizeof(PulsePickFoldersRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "pick_folders", "", "size",
                                 "null request/callback/request_id or invalid PulsePickFoldersRequest size");
            constexpr uint32_t kAllowed = PULSE_PICKER_FLAG_MULTISELECT | PULSE_PICKER_FLAG_SHOW_HIDDEN;
            if ((request->flags & ~kAllowed) != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "pick_folders", "", "flags",
                                 "unsupported flag for pick_folders");
            PickerLaunchSpec spec;
            spec.kind = PickerRequestKind::PickFolders;
            spec.flags = request->flags;
            if (!ValidUtf8Wide(request->title, 128, false, spec.title) ||
                !ValidUtf8Wide(request->ok_label, 64, false, spec.ok_label) ||
                !ValidUtf8Wide(request->initial_path, 32768, false, spec.initial_path))
                return self.Fail(PULSE_ERROR_ARGUMENT, "pick_folders", "", "title",
                                 "invalid UTF-8 or length in title, ok_label, or initial_path");
            return self.Launch("pick_folders", std::move(spec), callback, user, request_id);
        });
    }

    static PulseStatus PULSE_CALL QuickPick(void* context, const PulseQuickPickRequest* request,
                                            PulsePickerResultFn callback, void* user,
                                            PulsePickerRequestId* request_id) noexcept {
        if (request_id) *request_id = 0;
        return Guard(context, [&](State& self) -> PulseStatus {
            if (!request || !callback || !request_id || request->size < sizeof(PulseQuickPickRequest))
                return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", "", "size",
                                 "null request/callback/request_id or invalid PulseQuickPickRequest size");
            if (request->flags != 0 || request->reserved != 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", "", "flags",
                                 "PulseQuickPickRequest flags and reserved must be zero");
            if (request->item_count < 1 || request->item_count > 512 || !request->items)
                return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", "", "items",
                                 "item_count must be 1..512 with a non-null items array");
            PickerLaunchSpec spec;
            spec.kind = PickerRequestKind::QuickPick;
            if (!ValidUtf8Wide(request->title, 128, false, spec.title) ||
                !ValidUtf8Wide(request->placeholder, 128, false, spec.placeholder) ||
                !ValidUtf8Wide(request->initial_query, 128, false, spec.initial_query))
                return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", "", "title",
                                 "invalid UTF-8 or length in quick_pick header fields");
            spec.items.reserve(request->item_count);
            std::set<std::string> unique_ids;
            size_t enabled_count = 0;
            for (uint32_t i = 0; i < request->item_count; ++i) {
                const auto& src = request->items[i];
                const std::string sub = "items[" + std::to_string(i) + "]";
                if (src.size < sizeof(PulseQuickPickItem) ||
                    (src.flags & ~static_cast<uint32_t>(PULSE_QUICK_PICK_ITEM_ALL_FLAGS)) != 0)
                    return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", sub, "flags",
                                     "invalid PulseQuickPickItem size or flags");
                QuickPickItemSpec item;
                if (!ValidQuickPickId(src.id, item.id))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", sub, "id",
                                     "item id must be 1..128 ASCII [A-Za-z0-9._:/-] bytes");
                if (!unique_ids.insert(item.id).second)
                    return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", sub, "id",
                                     "duplicate quick_pick item id");
                if (!ValidUtf8Wide(src.title, 256, true, item.title) ||
                    !ValidUtf8Wide(src.detail, 256, false, item.detail) ||
                    !ValidUtf8Wide(src.glyph, 128, false, item.glyph) ||
                    !ValidUtf8Wide(src.shortcut, 64, false, item.shortcut))
                    return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", sub, "title",
                                     "invalid UTF-8 or length in quick_pick item text fields");
                item.selected = (src.flags & PULSE_QUICK_PICK_ITEM_SELECTED) != 0;
                item.disabled = (src.flags & PULSE_QUICK_PICK_ITEM_DISABLED) != 0;
                if (!item.disabled) ++enabled_count;
                spec.items.push_back(std::move(item));
            }
            if (enabled_count == 0)
                return self.Fail(PULSE_ERROR_ARGUMENT, "quick_pick", "", "items",
                                 "quick_pick requires at least one enabled item");
            return self.Launch("quick_pick", std::move(spec), callback, user, request_id);
        });
    }

    static PulseStatus PULSE_CALL Cancel(void* context, PulsePickerRequestId request_id) noexcept {
        return Guard(context, [&](State& self) -> PulseStatus {
            if (request_id == 0 || self.active.id != request_id)
                return self.Fail(PULSE_ERROR_STATE, "cancel", "", "request_id",
                                 "picker request_id is not active");
            self.active.cancel_requested = true;
            if (self.active.cancel_event && self.active.cancel_event->handle) {
                SetEvent(self.active.cancel_event->handle);
            }
            if (self.backend.cancel) self.backend.cancel(request_id);
            return PULSE_OK;
        });
    }
};

PickersAdapter::PickersAdapter(PickersBackend backend, std::function<bool()> alive,
                               std::shared_ptr<DiagnosticsSink> diagnostics)
    : state_(std::make_shared<State>(std::move(backend), std::move(alive), std::move(diagnostics))) {}

PickersAdapter::~PickersAdapter() { Close(); }

void PickersAdapter::SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink) {
    if (state_) state_->diagnostics = std::move(sink);
}

PulseStatus PickersAdapter::Query(uint32_t version, void* out, uint32_t size) const noexcept {
    try {
        if (!state_->Valid()) return PULSE_ERROR_STATE;
        if (version != PULSE_PICKERS_VERSION_1) return PULSE_ERROR_ABI;
        if (!out || size < sizeof(PulsePickersV1)) return PULSE_ERROR_ARGUMENT;
        const PulsePickersV1 table{
            sizeof(PulsePickersV1),
            PULSE_PICKERS_VERSION_1,
            state_.get(),
            State::OpenFiles,
            State::SaveFile,
            State::PickFolders,
            State::QuickPick,
            State::Cancel,
        };
        std::memcpy(out, &table, sizeof(table));
        return PULSE_OK;
    } catch (...) {
        return PULSE_ERROR_STATE;
    }
}

void PickersAdapter::Close() noexcept {
    if (!state_ || GetCurrentThreadId() != state_->thread || state_->closed) return;
    state_->closed = true;
    const uint64_t active_id = state_->active.id;
    auto cancel_handle = state_->active.cancel_event;
    state_->active = State::ActiveRequest{};
    if (cancel_handle && cancel_handle->handle) SetEvent(cancel_handle->handle);
    if (active_id && state_->backend.cancel) state_->backend.cancel(active_id);
}

bool PickersAdapter::CallbackActive() const noexcept {
    return state_ && state_->callbacks > 0;
}

} // namespace pulse::extensions::native
