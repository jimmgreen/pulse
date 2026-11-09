// ai_assistant.h - Pulse 助手, the in-app assistant panel: open/close and
// docking, the
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
struct AiVoiceUi; // 语音输入: listening, menu, connection card (ai_assistant_voice.h)
struct AsstPageUi; // Settings > AI 助手: stage, fields, 连接 (ai_assistant_settings.h)
struct AiHistoryUi; // 历史对话: the saved list, its filter and row actions (ai_chat_history.cpp)

struct AiAssistantState {
    bool open = false;
    bool floating = false;
    bool prefs_loaded = false;
    bool enabled = true;
    int model = 0;                 // index into the model list
    int think = 4;                 // the preferred level, 0 off .. 6 max (ai_providers.h ThinkLevel)
    bool menu_open = false;
    uint64_t menu_closed_at = 0;   // the menu's fade-out still covers the input
    // The context chip was removed for this folder.
    std::wstring context_removed_for;
    // Pointer.
    ui::AiPanelHit hover;
    ui::AiPanelHit pressed;
    int hover_turn = -1;           // user block whose turn is under the pointer (its 复制 · 编辑)
    POINT hover_point{};
    uint64_t hover_since = 0;
    bool captured = false;
    // Text selection in the conversation: clicks are counted (2 a word, 3 a
    // paragraph) and a drag past its edge scrolls.
    bool selecting = false;
    int sel_clicks = 0;
    uint64_t sel_click_at = 0;
    POINT sel_click_pt{};
    POINT sel_drag{};
    // 引用: the words quoted above the composer; the next message starts
    // with them as a Markdown block quote.
    std::wstring quote;
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
    // Editing a message in place (55 P2): its block id (0: none), the EDIT
    // over the box the panel draws, its words and wrapped lines.
    uint32_t edit_id = 0;
    HWND turn_edit = nullptr;
    bool turn_edit_visible = false;
    bool turn_edit_focus = false;  // focus it once it is placed
    int turn_edit_lines = 1;
    std::wstring turn_edit_text;
    // Conversation; created on first use. Opaque here so the driver can
    // change without rebuilding everything that sees AppState.
    std::shared_ptr<AiChat> chat;
    // 语音输入; created on the first frame with the panel.
    std::shared_ptr<AiVoiceUi> voice;
    // Settings > AI 助手; created when the page first shows.
    std::shared_ptr<AsstPageUi> page;
    // 55 P3 历史对话: the layer over the panel, its list, the search EDIT
    // and the EDIT over a renamed title (one line each, like turn_edit).
    bool hist_open = false;
    std::shared_ptr<AiHistoryUi> hist;
    HWND hist_search = nullptr;
    HWND hist_rename = nullptr;
    bool hist_search_visible = false, hist_rename_visible = false;
    bool hist_search_focus = false, hist_rename_focus = false;
    std::wstring hist_cue;          // the search EDIT's placeholder (its Pulse.EditCue)
};

// The status bar chip: opens or closes the panel (an outside client's open
// question still comes forward first).
void AiAssistantToggle(AppState& s);
// The keymap's Pulse 助手 key (Ctrl+J): open + focus, focus, or collapse.
void AiAssistantHotkey(AppState& s);
void AiAssistantOpen(AppState& s, bool animate = true);
void AiAssistantClose(AppState& s, bool animate = true);
bool AiAssistantIsOpen(const AppState& s);
// Right-click on the status bar's ✦ AI: open / collapse, AI 助手设置….
void AiChipContextMenu(AppState& s, POINT screen_pt);

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
// A tilt (horizontal) or Shift + wheel scrolls the code block or table under it.
bool AiAssistantWheel(AppState& s, int x, int y, int delta, bool horizontal = false);
// Right button released over the conversation: copy / quote / search, or a
// link's or path chip's menu; true when one was shown.
bool AiAssistantContextMenu(AppState& s, int x, int y);
// Ctrl+C (Ctrl+Insert) and Ctrl+A with text selected in the conversation.
bool AiAssistantSelectionKey(AppState& s, WPARAM vk);
void AiAssistantMouseLeave(AppState& s);
bool AiAssistantSetCursor(AppState& s, int x, int y);
// Esc with the main window focused: closes the menu, else stops a running
// turn.
bool AiAssistantEscape(AppState& s);
// Ctrl+H: 历史对话 (opens the panel when it is closed; closes the layer).
void AiAssistantHistoryKey(AppState& s);
// ←/→ (↑/↓), Home/End and 1–9 move the open 思考强度 slider; true when taken.
bool AiAssistantMenuKey(AppState& s, WPARAM vk);

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
