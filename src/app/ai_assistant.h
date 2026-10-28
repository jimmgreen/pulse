// ai_assistant.h - Pulse 助手, the in-app assistant panel
// (plans/ai/pulse_ai_assistant_v2.html): open/close and docking, the
// composer's EDIT, pointer routing for the panel, ai.json "assistant" prefs
// and the view model ui::AiPanelView draws.
#pragma once

#include "../ui/ai_panel_view.h"
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }
struct AiChat;   // the conversation and its driver (ai_assistant_chat.h)
struct AiConnUi; // 管理连接 card: fields, test, key (ai_assistant_conn.h)
struct AiVoiceUi; // 语音输入: listening, menu, connection card (ai_assistant_voice.h)
struct AsstPageUi; // Settings > AI 助手: stage, fields, 连接 (ai_assistant_settings.h)

struct AiAssistantState {
    bool open = false;
    bool floating = false;
    bool prefs_loaded = false;
    int model = 0;                 // index into the model list
    int effort = 2;                // 0 off .. 3 high
    bool menu_open = false;
    uint64_t menu_closed_at = 0;   // the menu's fade-out still covers the input
    // The context chip was removed for this folder.
    std::wstring context_removed_for;
    // Pointer.
    ui::AiPanelHit hover;
    ui::AiPanelHit pressed;
    POINT hover_point{};
    uint64_t hover_since = 0;
    bool captured = false;
    // Composer EDIT.
    HWND edit = nullptr;
    HFONT font = nullptr;
    float font_scale = 0.0f;
    HBRUSH brush = nullptr;
    COLORREF brush_color = 0;
    // Card / text colours the composer was last presented with (both paths).
    COLORREF edit_back = CLR_INVALID;
    COLORREF edit_fore = CLR_INVALID;
    std::wstring edit_cue;   // placeholder text the EDIT's "Pulse.EditCue" points at
    bool edit_visible = false;
    bool focus_pending = false;
    // A click on the composer text the panel drew (the glass panel hides the
    // EDIT at rest): the caret goes there once the EDIT takes focus.
    bool focus_click_set = false;
    POINT focus_click{};
    bool focused = false;
    int lines = 1;
    int text_len = 0;
    int line_px = 0;
    // Conversation; created on first use. Opaque here so the driver can
    // change without rebuilding everything that sees AppState.
    std::shared_ptr<AiChat> chat;
    // 管理连接 card; created when it first opens.
    std::shared_ptr<AiConnUi> conn;
    // 语音输入; created on the first frame with the panel.
    std::shared_ptr<AiVoiceUi> voice;
    // Settings > AI 助手; created when the page first shows.
    std::shared_ptr<AsstPageUi> page;
};

// The status bar chip: opens or closes the panel (an outside client's open
// question still comes forward first).
void AiAssistantToggle(AppState& s);
// The keymap's Pulse 助手 key (Ctrl+J): open + focus, focus, or collapse.
void AiAssistantHotkey(AppState& s);
void AiAssistantOpen(AppState& s, bool animate = true);
void AiAssistantClose(AppState& s, bool animate = true);
bool AiAssistantIsOpen(const AppState& s);

// BuildVm: the panel and the chip's open/idle look.
void FillAiAssistantVm(AppState& s, ui::WindowViewModel& vm);
// After every frame: the composer EDIT follows the drawn input box.
void AiAssistantLayout(AppState& s);
// Settings changed the model or 思考 elsewhere (设置 › AI 助手): read them again.
void AiAssistantPrefsChanged(AppState& s);

// Pointer messages in client pixels; true when the panel took them.
bool AiAssistantMouseMove(AppState& s, int x, int y);
bool AiAssistantMouseDown(AppState& s, int x, int y);
bool AiAssistantMouseUp(AppState& s, int x, int y);
bool AiAssistantMouseOther(AppState& s, int x, int y);   // dblclick, right button
// Wheel over the panel scrolls the conversation; true when the panel took it.
bool AiAssistantWheel(AppState& s, int x, int y, int delta);
void AiAssistantMouseLeave(AppState& s);
bool AiAssistantSetCursor(AppState& s, int x, int y);
// Esc with the main window focused: closes the menu, else stops a running
// turn.
bool AiAssistantEscape(AppState& s);

// WM_CTLCOLOREDIT for the composer; nullptr for any other EDIT.
HBRUSH AiAssistantCtlColor(AppState& s, HDC hdc, HWND edit);
void AiAssistantDpiChanged(AppState& s);
void AiAssistantDestroy(AppState& s);
// The assistant's multi-step task submitted this operation: when it fails the
// task pauses and asks in the panel, so the lock prompt and failure window
// stay closed.
bool AiAssistantOwnsTask(uint64_t task_id);

// The assistant's 查看 tabs (pulse:ai-list) list files by path and have no
// folder watch: reload every visible one after files moved or came back,
// whichever pane it is in and however the operation was started (Ctrl+Z).
void AiAssistantRefreshLists(AppState& s);

} // namespace pulse
