// ai_assistant_voice.h - 语音输入 (third batch, flow V): the composer's
// microphone. A click checks the connection, the microphone and Windows'
// privacy switch, then records (ai_voice_capture.h); each sentence is
// uploaded once to the speech service (ai_voice_service.h) and its words are
// appended to the composer, in order. Nothing is ever sent by itself: Enter
// sends. The chevron's menu (microphone, pause, 语音设置…) is here too; the
// service itself is set up in Settings > AI 助手 (ai_assistant_settings.cpp).
#pragma once

#include "../ui/ai_panel_view.h"
#include <windows.h>
#include <string>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }

// BuildVm: p.voice, and the chip while listening.
void AiVoiceFill(AppState& s, ui::WindowViewModel& vm);
// A click on the mic, its chevron, the menu or the card; false: not ours.
bool AiVoiceActivate(AppState& s, const ui::AiPanelHit& hit);
// An error card button of the voice cards (VoiceAdd ...); false: not ours.
bool AiVoiceErrAct(AppState& s, ui::AiErrAct act, int index);
// The composer EDIT steps aside: the menu or the card covers it, or the
// panel draws the recording in its place.
bool AiVoiceCovers(const AppState& s);
// Listening, or a recorded sentence is still being recognized.
bool AiVoiceBusy(const AppState& s);
// The frame pump's share: while motion runs (the waveform) its posted frames
// outrank WM_TIMER, so the poll and the armed test also run from there.
void AiVoiceRunDue(AppState& s);
bool AiVoiceMenuOpen(const AppState& s);
void AiVoiceCloseMenu(AppState& s);
// Esc: the menu, else stops listening. True when it did something.
bool AiVoiceEscape(AppState& s);
// Stops listening (the panel closes, 发送): sentences already recorded are
// still recognized and appended.
void AiVoiceStop(AppState& s);
// After every frame: repaints while the menu fades.
void AiVoiceLayout(AppState& s);
bool AiVoiceTip(const AppState& s, ui::AiPanelPart part, std::wstring& text);
bool AiVoiceHasTip(ui::AiPanelPart part);
void AiVoiceDestroy(AppState& s);
// 设置 › AI 助手 saved the voice connection: read ai.json again (not mid-recording).
void AiVoicePrefsChanged(AppState& s);

} // namespace pulse
