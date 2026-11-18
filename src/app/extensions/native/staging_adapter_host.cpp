#include "staging_adapter_host.h"
#include "../audit_log.h"
#include "../../app_runtime.h"
#include "../../app_ops_ui.h"
#include "../../arrival_select.h"
#include "../../picker_appearance.h"
#include "../../../ui/confirm_dialog.h"
#include "../../../ui/confirm_dialog_view.h"
#include "../../../ui/folder_picker_dialog.h"
#include <algorithm>
#include <filesystem>
#include <set>

namespace pulse::extensions::native {
namespace {
std::function<void(HWND)> confirm_observer, picker_observer;
std::string Utf8(const std::wstring& text) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring Wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
void Replace(std::wstring& text, const wchar_t* key, const std::wstring& value) {
    if (const auto at = text.find(key); at != text.npos) text.replace(at, wcslen(key), value);
}
}
void SetStagingConfirmObserver(std::function<void(HWND)> value) { confirm_observer = std::move(value); }
void SetStagingPickerObserver(std::function<void(HWND)> value) { picker_observer = std::move(value); }
StagingBackend MakeStagingBackend(HWND owner, CallerBinding caller) {
    StagingBackend backend;
    auto cancelled = std::make_shared<std::set<uint64_t>>();
    auto reported = std::make_shared<std::set<uint64_t>>();
    backend.submit = [owner, caller](const StageSubmission& stage) -> StageSubmitResult {
        auto* app = GetAppState(owner);
        if (!app || !ValidateCallerBinding(caller)) return {};
        using I = l10n::StringId;
        const std::wstring package = Wide(caller.Package().id);
        const auto& trusted = app->appPrefs.extension_publish_trusted;
        const bool always = std::find(trusted.begin(), trusted.end(), package) != trusted.end();
        ui::ConfirmDialogSpec spec;
        spec.title = l10n::Get(I::ExtensionPublishTitle);
        spec.message = l10n::Get(I::ExtensionPublishMessage);
        Replace(spec.message, L"{name}", Wide(caller.Package().id));
        Replace(spec.message, L"{path}", stage.destination);
        spec.confirm_text = l10n::Get(I::ConfirmContinue);
        spec.cancel_text = l10n::Get(I::Cancel);
        spec.secondary_text = l10n::Get(I::ExtensionPublishAlways);
        spec.note = l10n::Get(I::ExtensionPublishNote);
        spec.items_total = stage.sources.size();
        for (const auto& path : stage.sources) {
            if (spec.items.size() == ui::kConfirmMaxItemRows) break;
            spec.items.push_back(std::filesystem::path(path).filename().wstring());
            // A staged folder shows as a folder, not the document mark.
            const DWORD attrs = GetFileAttributesW(path.c_str());
            spec.item_glyphs.push_back(
                attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) ? L"\xE8B7" : L"");
        }
        spec.cancel_event = stage.cancel_event;
        const bool isolated = app->isolatedTest;
        spec.on_open = [isolated](HWND window) { if (window && isolated && confirm_observer) confirm_observer(window); };
        // Generated output is undoable and goes through conflict resolution, so a
        // package the user chose to always allow publishes without this question.
        RecordExtensionCall(caller, "staging.commit", always ? "trusted" : "confirmation");
        const auto choice = always ? ui::ConfirmChoice::Confirm
                                   : ui::ShowConfirmDialogEx(owner, spec, app->darkMode, app->accentColor);
        app = GetAppState(owner);
        if (app && choice == ui::ConfirmChoice::Secondary) {
            auto& list = app->appPrefs.extension_publish_trusted;
            if (std::find(list.begin(), list.end(), package) == list.end()) list.push_back(package);
            if (!app->isolatedTest) app->appPrefs.Save();
        }
        if ((choice != ui::ConfirmChoice::Confirm && choice != ui::ConfirmChoice::Secondary) || !app ||
            !ValidateCallerBinding(caller) ||
            (stage.cancel_event && WaitForSingleObject(stage.cancel_event, 0) == WAIT_OBJECT_0)) {
            RecordExtensionCall(caller, "staging.commit", "cancelled");
            return {PULSE_ERROR_STATE, 0};
        }
        ops::OpRequest request;
        request.type = ops::OpType::Move;
        request.generated_output = true;
        request.sources = stage.sources; request.dest_dir = stage.destination;
        request.display_name = Wide(caller.Package().id);
        request.undo_label = spec.title + L" · " + Wide(caller.Package().id);
        request.refresh_directories.push_back(stage.destination);
        uint64_t task = 0;
        if (!SubmitWithConflictResolution(*app, std::move(request), &task)) return {};
        // Publication may target another pane (or a folder no longer shown).
        // Never arm the archive's source folder for output written elsewhere.
        const auto destination = fs::NormalizePath(stage.destination);
        const app::Pane* target = nullptr;
        ForEachPane(*app, [&](app::Pane& pane) {
            const auto* tab = pane.ActiveTab();
            if (tab && _wcsicmp(fs::NormalizePath(tab->current_path).c_str(), destination.c_str()) == 0 &&
                (!target || &pane == app->pane)) target = &pane;
        });
        if (target) ArmArrivalSelection(*app, target, task, 0);
        RecordExtensionCall(caller, "staging.commit", "submitted:" + std::to_string(task));
        return {PULSE_OK, task};
    };
    backend.query = [owner, caller, cancelled, reported](uint64_t task) {
        StageOperation result;
        auto* app = GetAppState(owner);
        const auto status = app ? app->ops.QueryTask(task) : std::nullopt;
        if (!status) { result.state = PULSE_STAGE_FAILED; result.error = "Publication task is unavailable"; return result; }
        result.percent = status->percent < 0 ? UINT32_MAX : static_cast<uint32_t>(std::clamp(status->percent, 0.0f, 100.0f));
        result.completed_items = status->completed_items; result.total_items = status->total_items;
        result.completed_bytes = status->transferred_bytes; result.total_bytes = status->total_bytes;
        result.error = Utf8(status->last_error);
        if (!status->active) {
            if (status->phase == ops::OpPhase::Completed && !status->failed_items && status->last_error.empty()) result.state = PULSE_STAGE_COMPLETED;
            else if (cancelled->contains(task) || status->last_error == l10n::Get(l10n::StringId::Canceled)) result.state = PULSE_STAGE_CANCELLED;
            else result.state = status->completed_items ? PULSE_STAGE_PARTIAL : PULSE_STAGE_FAILED;
            if (reported->insert(task).second) RecordExtensionCall(caller, "staging.finished", std::to_string(task) + ":" + std::to_string(result.state));
        }
        return result;
    };
    backend.cancel = [owner, cancelled](uint64_t task) {
        cancelled->insert(task);
        if (auto* app = GetAppState(owner)) app->ops.CancelTask(task);
    };
    backend.choose_destination = [owner, caller](const std::wstring& initial) -> std::pair<PulseStatus, std::wstring> {
        auto* app = GetAppState(owner);
        if (!app || !ValidateCallerBinding(caller)) return {PULSE_ERROR_STATE, {}};
        ui::FolderPickerSpec spec;
        app::ApplyPickerAppearanceToSpec(app::PickerAppearanceFromPrefs(app->appPrefs), spec);
        spec.show_hidden = app->appPrefs.show_hidden_files;
        spec.initial_path = initial;
        const bool isolated = app->isolatedTest;
        spec.on_dialog_event = [isolated](HWND window, ui::PickerDialogEvent event, const ui::PickerDialogSnapshot&, const std::wstring&) {
            if (isolated && event == ui::PickerDialogEvent::Ready && picker_observer) picker_observer(window);
            return S_OK;
        };
        std::wstring path;
        const bool chosen = ui::ShowFolderPicker(owner, spec, app->darkMode, app->accentColor, path);
        return chosen && GetAppState(owner) && ValidateCallerBinding(caller)
            ? std::make_pair(PULSE_OK, std::move(path)) : std::make_pair(PULSE_ERROR_STATE, std::wstring{});
    };
    return backend;
}
}
