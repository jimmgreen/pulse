#include "pickers_adapter_host.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../picker_appearance.h"
#include "../../../ui/folder_picker_dialog.h"
#include "../../../ui/quick_pick_dialog.h"
#include <deque>

namespace pulse::extensions::native {
namespace {

constexpr wchar_t kPickerDispatchClass[] = L"PulseExtensionPickerDispatcher";
constexpr UINT kPickerDispatchMessage = WM_APP + 0x57;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

struct PendingPickerJob {
    PickerLaunchSpec spec;
    std::function<void(PickerBackendCompletion)> complete;
    bool cancelled = false;
};

struct PickerHostDispatcher : std::enable_shared_from_this<PickerHostDispatcher> {
    HWND owner = nullptr;
    std::function<HWND()> active_modal_owner;
    HWND message_hwnd = nullptr;
    std::deque<PendingPickerJob> queue;

    ~PickerHostDispatcher() {
        if (message_hwnd && IsWindow(message_hwnd)) {
            SetWindowLongPtrW(message_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(message_hwnd);
        }
        message_hwnd = nullptr;
    }

    bool EnsureWindow() {
        if (message_hwnd && IsWindow(message_hwnd)) return true;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kPickerDispatchClass;
        if (!GetClassInfoExW(wc.hInstance, kPickerDispatchClass, &wc)) {
            RegisterClassExW(&wc);
        }
        message_hwnd = CreateWindowExW(0, kPickerDispatchClass, L"", 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, wc.hInstance, this);
        return message_hwnd != nullptr;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PickerHostDispatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<PickerHostDispatcher*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && msg == kPickerDispatchMessage) {
            self->RunNext();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void RunNext() {
        auto keep = shared_from_this();
        if (queue.empty()) return;
        PendingPickerJob job = std::move(queue.front());
        queue.pop_front();

        PickerBackendCompletion result{};
        const HANDLE cancel_ev = job.spec.cancel_event ? job.spec.cancel_event->handle : nullptr;
        if (job.cancelled || (cancel_ev && WaitForSingleObject(cancel_ev, 0) == WAIT_OBJECT_0)) {
            result.outcome = PULSE_PICKER_CANCELLED;
            if (job.complete) job.complete(std::move(result));
            return;
        }

        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) {
            result.outcome = PULSE_PICKER_CANCELLED;
            if (job.complete) job.complete(std::move(result));
            return;
        }

        const HWND modal_dialog = active_modal_owner ? active_modal_owner() : nullptr;
        const HWND target_owner = (modal_dialog && IsWindow(modal_dialog)) ? modal_dialog : owner;

        if (job.spec.kind == PickerRequestKind::QuickPick) {
            ui::QuickPickDialogSpec qp;
            qp.title = std::move(job.spec.title);
            qp.placeholder = std::move(job.spec.placeholder);
            qp.initial_query = std::move(job.spec.initial_query);
            qp.cancel_event = cancel_ev;
            qp.items.reserve(job.spec.items.size());
            for (auto& src : job.spec.items) {
                ui::QuickPickDialogItem item;
                item.id = std::move(src.id);
                item.title = std::move(src.title);
                item.detail = std::move(src.detail);
                item.glyph = std::move(src.glyph);
                item.shortcut = std::move(src.shortcut);
                item.selected = src.selected;
                item.disabled = src.disabled;
                qp.items.push_back(std::move(item));
            }
            const auto outcome = ui::ShowQuickPickDialog(target_owner, qp, app->darkMode, app->accentColor);
            if (outcome.status == ui::QuickPickDialogStatus::Accepted) {
                result.outcome = PULSE_PICKER_ACCEPTED;
                result.selected_id = outcome.selected_id;
            } else if (outcome.status == ui::QuickPickDialogStatus::Cancelled) {
                result.outcome = PULSE_PICKER_CANCELLED;
            } else {
                result.outcome = PULSE_PICKER_FAILED;
                result.error = "quick_pick presentation failed (Win32 error " + std::to_string(outcome.error) + ")";
            }
            if (job.complete) job.complete(std::move(result));
            return;
        }

        ui::FolderPickerSpec picker;
        app::ApplyPickerAppearanceToSpec(app::PickerAppearanceFromPrefs(app->appPrefs), picker);
        picker.mode = job.spec.kind == PickerRequestKind::SaveFile ? ui::PickerMode::Save
            : job.spec.kind == PickerRequestKind::PickFolders ? ui::PickerMode::Folder
            : ui::PickerMode::File;
        picker.allow_multiselect = (job.spec.flags & PULSE_PICKER_FLAG_MULTISELECT) != 0;
        picker.show_hidden = ((job.spec.flags & PULSE_PICKER_FLAG_SHOW_HIDDEN) != 0) || app->appPrefs.show_hidden_files;
        picker.prompt_overwrite = (job.spec.flags & PULSE_PICKER_FLAG_NO_OVERWRITE_PROMPT) == 0;
        if (!job.spec.title.empty()) {
            picker.title = std::move(job.spec.title);
            picker.explicit_title = true;
        }
        if (!job.spec.ok_label.empty()) {
            picker.ok_button_label = std::move(job.spec.ok_label);
            picker.has_ok_button_label = true;
        }
        picker.initial_path = std::move(job.spec.initial_path);
        picker.filename = std::move(job.spec.default_name);
        picker.default_extension = std::move(job.spec.default_extension);
        picker.filters.reserve(job.spec.filters.size());
        for (auto& f : job.spec.filters) {
            picker.filters.push_back({std::move(f.label), std::move(f.pattern)});
        }
        picker.filter_index = job.spec.default_filter_index;
        picker.cancel_event = cancel_ev;

        const auto outcome = ui::ShowFilePickerDetailed(target_owner, picker, app->darkMode, app->accentColor);
        if (outcome.status == ui::FilePickerStatus::Accepted) {
            result.outcome = PULSE_PICKER_ACCEPTED;
            result.filter_index = static_cast<uint32_t>(outcome.result.filter_index);
            result.paths.reserve(outcome.result.paths.size());
            for (const auto& p : outcome.result.paths) {
                result.paths.push_back(Utf8(p));
            }
        } else if (outcome.status == ui::FilePickerStatus::Cancelled) {
            result.outcome = PULSE_PICKER_CANCELLED;
        } else {
            result.outcome = PULSE_PICKER_FAILED;
            result.error = "file picker failed (Win32 error " + std::to_string(outcome.error) + ")";
        }
        if (job.complete) job.complete(std::move(result));
    }
};

} // namespace

PickersBackend MakePickersBackend(HWND owner, std::function<HWND()> active_modal_owner) {
    auto host = std::make_shared<PickerHostDispatcher>();
    host->owner = owner;
    host->active_modal_owner = std::move(active_modal_owner);

    PickersBackend backend;
    backend.valid_owner = [host] { return IsWindow(host->owner) != FALSE; };
    backend.launch = [host](PickerLaunchSpec spec, std::function<void(PickerBackendCompletion)> complete) -> bool {
        if (!host->EnsureWindow()) return false;
        host->queue.push_back(PendingPickerJob{std::move(spec), std::move(complete), false});
        if (!PostMessageW(host->message_hwnd, kPickerDispatchMessage, 0, 0)) {
            host->queue.pop_back();
            return false;
        }
        return true;
    };
    backend.cancel = [host](uint64_t request_id) {
        for (auto& job : host->queue) {
            if (job.spec.request_id == request_id) {
                job.cancelled = true;
            }
        }
    };
    return backend;
}

PickersAdapter::PickersAdapter(HWND owner, std::function<HWND()> active_modal_owner, std::function<bool()> alive,
                               std::shared_ptr<DiagnosticsSink> diagnostics)
    : PickersAdapter(MakePickersBackend(owner, std::move(active_modal_owner)), std::move(alive), std::move(diagnostics)) {}

} // namespace pulse::extensions::native
