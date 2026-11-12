#pragma once
#include "../../../../sdk/native/include/pulse/dialogs.h"
#include "../ui_tree.h"
#include <windows.h>
#include <d2d1.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulse::extensions::native {

class DiagnosticsSink;

struct DialogButtonSpec {
    std::wstring text;
    uint32_t kind = PULSE_DIALOG_BUTTON_STANDARD;
    std::string id;
};

struct DialogPresentationSpec {
    std::wstring title;
    std::wstring message;
    std::wstring heading;
    uint32_t legacy_icon = PULSE_DIALOG_ICON_NONE;
    std::optional<uint32_t> extension_icon;
    uint32_t subject_kind = PULSE_DIALOG_SUBJECT_NONE;
    std::wstring subject;
    std::wstring subject_detail;
    std::wstring footnote;
    uint32_t legacy_footnote_icon = PULSE_DIALOG_ICON_NONE;
    std::optional<uint32_t> extension_footnote_icon;
    std::optional<extensions::UiTree> form;
    std::vector<DialogButtonSpec> buttons;
    int default_button = -1;
    int cancel_button = -1;
    float width_dip = 480.0f;
    float form_height_dip = 0.0f;
    std::function<void(const std::vector<extensions::UiStateEntry>&)> on_states_changed;
    std::function<void(const std::string&)> on_command;
    std::function<bool(const std::string&)> command_enabled;
    std::function<void(int button, std::vector<extensions::UiStateEntry> states)> on_closed;
};

struct DialogsBackend {
    std::function<bool()> valid_owner;
    std::function<bool(DialogPresentationSpec)> show;
    std::function<bool(const std::vector<extensions::UiStateEntry>&)> set_states;
    std::function<bool(const extensions::UiTree&)> update_form;
    std::function<std::optional<std::string>(const std::string&)> get_state;
    std::function<void()> refresh_form;
    std::function<void()> dismiss;
    std::function<void()> destroy;
    std::function<bool()> is_open;
    std::function<HWND()> active_hwnd;
    std::function<void(bool dark, D2D1_COLOR_F accent)> set_appearance;
};

// pulse.dialogs V1 & V2 for one package session (see sdk/native/include/pulse/dialogs.h).
class DialogsAdapter {
public:
    DialogsAdapter(DialogsBackend backend, std::function<bool()> alive,
                   std::shared_ptr<DiagnosticsSink> diagnostics = nullptr);
    DialogsAdapter(HWND owner, bool dark, D2D1_COLOR_F accent, std::function<bool()> alive,
                   std::shared_ptr<DiagnosticsSink> diagnostics = nullptr);
    ~DialogsAdapter();
    DialogsAdapter(const DialogsAdapter&) = delete;
    DialogsAdapter& operator=(const DialogsAdapter&) = delete;

    void SetDiagnostics(std::shared_ptr<DiagnosticsSink> sink);
    PulseStatus Query(uint32_t version, void* out, uint32_t size) const noexcept;
    void SetAppearance(bool dark, D2D1_COLOR_F accent);
    // Session stop: the open dialog closes without its callback.
    void Close();
    bool CallbackActive() const;
    HWND ActiveModalWindow() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::extensions::native
