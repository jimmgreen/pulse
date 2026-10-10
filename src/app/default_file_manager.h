// default_file_manager.h — "设为默认文件管理器" (B站 #1): one switch over the
// folder/drive, Win+E and This PC takeovers, plus the This PC open verb.
#pragma once

#include <string>
#include <string_view>

namespace pulse::app {

struct AppPrefs;

// Shell parsing name of This PC; also the argument the This PC verb passes.
inline constexpr wchar_t kThisPcParsingName[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
// Shell parsing name of the Recycle Bin; the argument its open verb passes.
inline constexpr wchar_t kRecycleBinParsingName[] = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

enum class DefaultManagerState { Off, Partial, Full };

// From the four takeover flags in prefs (registry is their source of truth).
DefaultManagerState DefaultFileManagerState(const AppPrefs& prefs);

// Settings text: the full description, or which parts File Explorer still opens.
std::wstring DefaultFileManagerSummary(const AppPrefs& prefs);

// Turns every takeover on (filling in the missing ones) or off (restoring
// File Explorer; other programs' registrations stay untouched).
bool ApplyDefaultFileManager(AppPrefs& prefs, bool on);

// HKCU CLSID\{20D04FE0-...}\shell\open: double-clicking This PC opens Pulse.
bool ReadThisPcOpen(const std::wstring& exe);
bool ApplyThisPcOpen(AppPrefs& prefs, bool on);

// HKCU CLSID\{645FF040-...}\shell\open: double-clicking the desktop Recycle
// Bin opens it in Pulse (pulse:recycle).
bool ReadRecycleBinOpen(const std::wstring& exe);
bool ApplyRecycleBinOpen(AppPrefs& prefs, bool on);

// Launch arguments that mean This PC: its parsing name (with or without the
// "shell:" prefix) and shell:MyComputerFolder. Quotes and case are ignored.
bool IsThisPcArgument(std::wstring_view raw);
// Same rules for the Recycle Bin: its parsing name and shell:RecycleBinFolder.
bool IsRecycleBinArgument(std::wstring_view raw);

} // namespace pulse::app
