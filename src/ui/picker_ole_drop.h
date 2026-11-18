#pragma once

#include "drag_drop.h"
#include <ole2.h>

namespace pulse::ui {

// OLE initialization belongs to the dialog's STA, never the shell host. Keep
// it alive through a nested drag loop even when that loop closes the window.
class PickerOleDrop {
public:
    PickerOleDrop() = default;
    PickerOleDrop(const PickerOleDrop&) = delete;
    PickerOleDrop& operator=(const PickerOleDrop&) = delete;
    ~PickerOleDrop() {
        Revoke();
        if (initialized_) OleUninitialize();
    }
    void Register(HWND window, DropTargetCallbacks callbacks) {
        if (window_ || target_) return;
        if (!initialized_) initialized_ = SUCCEEDED(OleInitialize(nullptr));
        if (!initialized_) return;
        target_ = new WindowDropTarget(window, std::move(callbacks));
        if (SUCCEEDED(RegisterDragDrop(window, target_))) window_ = window;
        else { target_->Release(); target_ = nullptr; }
    }
    void Revoke() {
        if (window_) { RevokeDragDrop(window_); window_ = nullptr; }
        if (target_) { target_->Release(); target_ = nullptr; }
    }
    bool Ready() const { return initialized_; }
#ifdef PULSE_WITH_TEST_HOOKS
    IDropTarget* TestTarget() const { return target_; }
#endif

private:
    bool initialized_ = false;
    HWND window_ = nullptr;
    WindowDropTarget* target_ = nullptr;
};

} // namespace pulse::ui
