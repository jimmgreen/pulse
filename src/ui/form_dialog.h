// form_dialog.h - a host-drawn modal with an optional UI-tree form and 1-3
// footer buttons; the window behind pulse.dialogs. Show returns at once; the
// dialog then runs its own modal loop from the owner's queue, so callers see
// the answer asynchronously through on_closed.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../app/extensions/ui_tree.h"

namespace pulse::ui {

enum class FormDialogButtonKind { Standard, Primary, Danger };

struct FormDialogButton {
    std::wstring text;
    FormDialogButtonKind kind = FormDialogButtonKind::Standard;
};

struct FormDialogSpec {
    std::wstring title;
    std::wstring message;
    std::optional<extensions::UiTree> form;
    std::vector<FormDialogButton> buttons;  // 1-3, shown left to right
    int default_button = -1;                // Enter; -1: none
    int cancel_button = -1;                 // Esc / close; -1: dismiss without a button
    float width_dip = 480.0f;
    float form_height_dip = 0.0f;
    std::function<void(const std::vector<extensions::UiStateEntry>&)> on_states_changed;
    // button -1: dismissed with no cancel button. Runs once, after the owner is
    // enabled again; not run when Destroy() closes the dialog.
    std::function<void(int button, std::vector<extensions::UiStateEntry> states)> on_closed;
};

class FormDialog {
public:
    FormDialog();
    ~FormDialog();  // Destroy()
    FormDialog(const FormDialog&) = delete;
    FormDialog& operator=(const FormDialog&) = delete;
    bool Show(HWND owner, FormDialogSpec spec, bool dark, D2D1_COLOR_F accent);
    bool SetStates(const std::vector<extensions::UiStateEntry>& states);
    void Dismiss();   // as the cancel button (on_closed runs)
    void Destroy();   // close without on_closed
    bool IsOpen() const;
    HWND Hwnd() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulse::ui
