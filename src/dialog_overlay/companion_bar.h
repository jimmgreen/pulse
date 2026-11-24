#pragma once

#include "dialog_classifier.h"
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace pulse::dialog_overlay {

struct QuickJumpEntry {
    std::wstring label;
    std::wstring path;
};

// Mode A (Default): Non-activating floating companion jump bar docked above a native
// file dialog. Uses WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST so keyboard
// focus in the target app is never stolen, and drives the native dialog's filename
// Edit control directly on click.
class CompanionBar {
public:
    using PickViaPulseFn = std::function<bool(const NativeDialogInfo& target, std::wstring& chosen_path)>;

    CompanionBar() = default;
    ~CompanionBar();
    CompanionBar(const CompanionBar&) = delete;
    CompanionBar& operator=(const CompanionBar&) = delete;

    bool Create(HINSTANCE instance) noexcept;
    void Destroy() noexcept;

    void AttachTo(const NativeDialogInfo& target, std::vector<QuickJumpEntry> entries,
                  PickViaPulseFn on_pick_via_pulse = {});
    void UpdatePosition() noexcept;
    void Detach() noexcept;

    HWND Hwnd() const noexcept { return hwnd_; }
    HWND TargetHwnd() const noexcept { return target_.dialog_hwnd; }
    bool Visible() const noexcept { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void RebuildButtons();
    void OnButtonClick(int command_id);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    NativeDialogInfo target_{};
    std::vector<QuickJumpEntry> entries_;
    std::vector<HWND> buttons_;
    PickViaPulseFn on_pick_via_pulse_;
};

// Mode B (Optional, pure standard dialogs only): Hides the native #32770 off-screen
// (alpha=0, preserving modality), invokes `pick_fn`, injects the chosen path into the
// native Edit(1148) + IDOK, and guarantees full restoration on cancel or error.
struct ModeBOutcome {
    bool handled = false;
    bool accepted = false;
    bool restored_native = false;
};

ModeBOutcome RunModeBProxy(const NativeDialogInfo& info,
                           const std::function<bool(std::wstring& out_path)>& pick_fn) noexcept;

} // namespace pulse::dialog_overlay
