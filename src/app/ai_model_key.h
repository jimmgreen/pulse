// ai_model_key.h - The model services' API keys (Pulse 助手) and the voice
// recognition service's key, kept only on this machine: encrypted with DPAPI
// for the current user in %LOCALAPPDATA%\Pulse\ai\model_key*.bin and
// voice_key.bin. Never written to ai.json, the activity log or the runtime log.
#pragma once

#include <string>
#include <string_view>

namespace pulse::app::ai {

// Small file, local disk: fine on the UI thread.
// The key of one model service (ai_providers.h id): DeepSeek's in
// model_key.bin as before, each other service's in model_key_<id>.bin.
bool SaveServiceKey(std::string_view service, std::wstring_view key);
bool LoadServiceKey(std::string_view service, std::wstring& key);   // false: none stored or unreadable
bool HasServiceKey(std::string_view service);
bool ClearServiceKey(std::string_view service);
// The same for the service in use (CurrentProvider()).
bool SaveModelKey(std::wstring_view key);
bool LoadModelKey(std::wstring& key);   // false: none stored or unreadable
bool HasModelKey();
bool ClearModelKey();
// 语音识别连接 (third batch, flow V): the same, in voice_key.bin.
bool SaveVoiceKey(std::wstring_view key);
bool LoadVoiceKey(std::wstring& key);
bool HasVoiceKey();
bool ClearVoiceKey();
// "sk-…a1b2" for the settings page; empty for an empty key.
std::wstring MaskModelKey(std::wstring_view key);

} // namespace pulse::app::ai
