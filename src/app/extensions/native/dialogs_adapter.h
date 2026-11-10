#pragma once
#include "../../../../sdk/native/include/pulse/dialogs.h"
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <memory>

namespace pulse::extensions::native {
// pulse.dialogs V1 for one package session (see sdk/native/include/pulse/dialogs.h).
class DialogsAdapter {
public:
    DialogsAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, std::function<bool()> alive);
    ~DialogsAdapter();
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void SetAppearance(bool dark, D2D1_COLOR_F accent);
    // Session stop: the open dialog closes without its callback.
    void Close();
    bool CallbackActive() const;
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
