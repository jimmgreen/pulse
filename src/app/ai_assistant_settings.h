// ai_assistant_settings.h - Settings > AI 助手 (kSettingsPageAssistant): where
// Pulse 助手's model service is chosen and set up. Tiles to pick a service,
// its key and address with 连接 (which reads its models), then the default
// model, 思考, key, address and the other services whose keys are kept. The
// page hosts its own EDITs (key, address, typed model name); keys go straight
// from the field to Windows encryption (ai_model_key.h) and are never shown.
#pragma once
#include "../common/windows_compat.h"

#include <cstdint>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }

// BuildVm: the page on page 14, the sidebar's 新 everywhere.
void FillAssistantSettingsVm(AppState& s, ui::WindowViewModel& vm);
// A click on HitTestResult::SettingsAssistant (AssistantHit).
void AssistantSettingsClick(AppState& s, int target);
// 思考强度's knob, held from that click: follow the pointer, then keep the
// level under it on release (true when a drag ended).
bool AssistantThinkDragging(AppState& s);
void AssistantThinkDrag(AppState& s, int x);
bool AssistantThinkDragEnd(AppState& s);
// A connection check of the assistant's model service finished somewhere
// (this page or the panel): 上次连接, for this run only.
void NoteAssistantConnection(bool ok, uint32_t ms = 0);
// After every frame: the page's EDITs follow the drawn fields (or hide).
void AssistantSettingsLayout(AppState& s);
// WM_CTLCOLOREDIT for the page's EDITs; nullptr for any other.
HBRUSH AssistantSettingsCtlColor(AppState& s, HDC hdc, HWND edit);
void AssistantSettingsDpiChanged(AppState& s);
// The mic menu's 语音设置…: Settings > AI 助手, scrolled to 语音输入.
void ShowAssistantVoiceSettings(AppState& s);
void AssistantSettingsDestroy(AppState& s);

} // namespace pulse
