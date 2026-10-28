// ai_assistant_settings.h - Settings > AI 助手 (kSettingsPageAssistant): what
// the page shows about Pulse's own assistant (from ai.json and whether keys
// are stored, never the keys) and its buttons, which open the assistant
// panel's connection and voice cards where the real settings live.
#pragma once
#include "../common/windows_compat.h"

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }

// BuildVm: the page on page 14, the sidebar's 新 everywhere.
void FillAssistantSettingsVm(AppState& s, ui::WindowViewModel& vm);
// A click on HitTestResult::SettingsAssistant (AssistantHit).
void AssistantSettingsClick(AppState& s, int target);
// A connection check of the assistant's model service finished somewhere
// (this page or the panel's 管理连接 card): 上次连接, for this run only.
void NoteAssistantConnection(bool ok);

} // namespace pulse
