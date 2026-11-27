#pragma once

#include "../ops/ops_manager.h"
#include <d2d1.h>
#include <atomic>

namespace pulse::ui {

struct PickerOperationResult {
    std::wstring error;
    std::vector<ops::CompletedOperation> completed;
};

class PickerOperationSession {
public:
    PickerOperationSession();
    ~PickerOperationSession();
    PickerOperationSession(const PickerOperationSession&) = delete;
    PickerOperationSession& operator=(const PickerOperationSession&) = delete;

    PickerOperationResult Run(HWND owner, ops::OpRequest request, bool dark, D2D1_COLOR_F accent, bool unique_create = false);
    // Runs a single non-queue transfer while retaining the same modal pumping and
    // cancellation lifetime. The worker returns an empty string only on success.
    PickerOperationResult RunBackground(HWND owner,
        std::function<std::wstring(const std::atomic_bool&)> worker, bool dark, D2D1_COLOR_F accent);
    PickerOperationResult Undo(HWND owner, bool dark, D2D1_COLOR_F accent);
    bool CanUndo() const;
    void Cancel();

private:
    void EnsureStarted();
    PickerOperationResult Execute(HWND owner, const std::function<bool()>& submit,
                                  bool dark, D2D1_COLOR_F accent, const std::function<bool()>& ready = {},
                                  const ops::OpStatus* background_status = nullptr);
    ops::OpsManager manager_;
    bool started_ = false;
    bool executing_ = false;
    std::atomic_bool cancelling_{false};
};

// Runs the existing worker queue while pumping a modal progress window.
// The picker stays disabled until cancellation or completion has settled.
PickerOperationResult RunPickerOperation(HWND owner, ops::OpRequest request,
                                        bool dark, D2D1_COLOR_F accent);

} // namespace pulse::ui
