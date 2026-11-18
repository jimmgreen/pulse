#include "../app/context_menu.h"
#include <future>
#include "folder_picker_operations.h"
#include "file_operation_dialog.h"
#include "../common/localization.h"

namespace pulse::ui {

PickerOperationSession::PickerOperationSession() { manager_.Start({}, false); }
PickerOperationSession::~PickerOperationSession() { manager_.Stop(); }

bool PickerOperationSession::CanUndo() const { return !executing_ && manager_.CanUndo(); }
void PickerOperationSession::Cancel() { cancelling_ = true; manager_.CancelCurrent(); }

PickerOperationResult PickerOperationSession::Run(HWND owner, ops::OpRequest request,
                                                 bool dark, D2D1_COLOR_F accent, bool unique_create) {
    if (unique_create && request.sources.size() == 1 &&
        (request.type == ops::OpType::CreateFolder || request.type == ops::OpType::CreateTextFile)) {
        std::future<ops::OpRequest> prepared;
        auto prepare = [this, request = std::move(request)]() mutable {
            const auto slash = request.sources.front().find_last_of(L"\\/");
            const auto parent = request.sources.front().substr(0, slash);
            auto base = request.sources.front().substr(slash + 1);
            std::wstring extension;
            if (request.type == ops::OpType::CreateTextFile) {
                const auto dot = base.find_last_of(L'.');
                if (dot != std::wstring::npos) { extension = base.substr(dot); base.resize(dot); }
            }
            const auto name = app::UniqueChildName(parent, base, extension, [&] { return cancelling_.load(); });
            if (name.empty()) request.sources.clear();
            else request.sources.front() = parent + L"\\" + name;
            return request;
        };
        return Execute(owner, [&] {
            try { return !cancelling_ && manager_.Submit(prepared.get()) != 0; }
            catch (...) { return false; }
        }, dark, accent, [&] {
            if (!prepared.valid()) {
                try { prepared = std::async(std::launch::async, std::move(prepare)); }
                catch (...) { return true; }
            }
            return prepared.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        });
    }
    return Execute(owner, [&] { return manager_.Submit(std::move(request)) != 0; }, dark, accent);
}

PickerOperationResult PickerOperationSession::Undo(HWND owner, bool dark, D2D1_COLOR_F accent) {
    if (!CanUndo()) return {};
    return Execute(owner, [&] { manager_.Undo(); return true; }, dark, accent);
}

PickerOperationResult PickerOperationSession::Execute(HWND owner, const std::function<bool()>& submit,
                                                     bool dark, D2D1_COLOR_F accent, const std::function<bool()>& ready) {
    PickerOperationResult result;
    if (executing_) return result;
    executing_ = true;
    cancelling_ = false;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{executing_};
    auto& manager = manager_;
    FileOperationWindow progress;
    FileOperationCallbacks callbacks;
    callbacks.cancel = [&] { Cancel(); };
    callbacks.skip_current = [&](uint64_t task, uint64_t token) { manager.SkipCurrentItem(task, token); };
    callbacks.pause = [&] { manager.PauseCurrent(); };
    callbacks.resume = [&] { manager.ResumeCurrent(); };
    callbacks.retry_authorization = [&](uint64_t id) { manager.ResolveAuthorization(id, true); };
    callbacks.skip_authorization = [&](uint64_t id) { manager.ResolveAuthorization(id, false); };
    if (!progress.Create(owner, std::move(callbacks))) {
        result.error = l10n::Get(l10n::StringId::CouldNotOpenTheFileOperationWindow).c_str();
        return result;
    }
    progress.SetTheme(dark, accent);
    const bool enabled = owner && IsWindowEnabled(owner);
    if (enabled) EnableWindow(owner, FALSE);
    manager.SetUiWindow(owner);
    bool preparing = ready && !ready();
    if (!preparing && !submit()) {
        progress.Destroy();
        if (enabled) EnableWindow(owner, TRUE);
        result.error = l10n::Get(l10n::StringId::ThisFileOperationIsNotSupported).c_str();
        return result;
    }
    const ULONGLONG started = GetTickCount64();
    bool quit = false;
    int exit_code = 0;
    uint64_t conflict_token = 0;
    for (;;) {
        if (preparing && ready()) {
            preparing = false;
            if (!submit()) { result.error = l10n::Get(l10n::StringId::Canceled); break; }
        }
        const auto status = manager.Status();
        progress.Update(status);
        if (!preparing && !manager.HasPendingOperations()) {
            result.error = manager.Status().last_error;
            break;
        }
        if ((status.active || preparing) && (GetTickCount64() - started >= 250 ||
                              status.authorization != ops::AuthorizationState::None)) {
            if (!progress.IsVisible()) progress.Show();
        }
        if (quit || cancelling_ || (owner && !IsWindow(owner))) Cancel();
        MSG message{};
        // A continuously posting source must not starve worker completion checks.
        for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) {
                quit = true;
                exit_code = static_cast<int>(message.wParam);
                Cancel();
            } else if (message.hwnd == owner && message.message == WM_CLOSE) {
                Cancel();
            } else {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (const auto conflict = manager.PendingConflict();
            !quit && !cancelling_ && conflict && conflict->token != conflict_token) {
            conflict_token = conflict->token;
            progress.Hide();
            const auto choice = ShowFileConflictDialog(owner, *conflict, dark, accent);
            // Nested dialogs may enable their owner as they close.
            if (enabled) EnableWindow(owner, FALSE);
            manager.ResolveConflict(conflict->token, choice.choice, choice.apply_to_all);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    result.completed = manager.DrainCompletions();
    for (const auto& task : manager.DrainFinishedTasks()) {
        if (!task.success && result.error.empty()) result.error = l10n::Get(l10n::StringId::OpFailed);
    }
    progress.Destroy();
    if (enabled && IsWindow(owner)) { EnableWindow(owner, TRUE); SetActiveWindow(owner); }
    if (quit) PostQuitMessage(exit_code);
    return result;
}

PickerOperationResult RunPickerOperation(HWND owner, ops::OpRequest request,
                                        bool dark, D2D1_COLOR_F accent) {
    PickerOperationSession session;
    return session.Run(owner, std::move(request), dark, accent);
}

} // namespace pulse::ui
