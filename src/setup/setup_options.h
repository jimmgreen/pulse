#pragma once
// Command line accepted by the Pulse setup. Inno Setup switches used by the
// in-app updater (src/app/update_installer.cpp) are accepted unchanged:
//   /SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /PULSEUPDATE=1 /LOG /DIR="..."
// plus /SILENT, /LOG="file", /TASKS="a,!b", /MERGETASKS="..", /LANG=..
// Pulse additions: /PULSETEST (isolated test install), /INDEXPATH="..",
// /UNINSTALL, /PULSEUPGRADE=1 (uninstall during an upgrade, keeps data).

#include <cstdint>
#include <optional>
#include <string>

namespace pulse::setup {

struct Tasks {
    bool index_service = true;
    bool global_search = true;
    bool ai_assistant = true;
    bool startup = false;
    bool desktop_icon = false;
};

// Starting layout picked in the wizard (界面起点). A fresh install records it
// in setup-defaults.ini [ui]; the app applies it on its first launch. An
// upgrade keeps the previous setup-defaults.ini, so the choice never
// overrides an existing user's layout.
enum class UiMode { Zen = 0, Standard = 1, Pro = 2 };

struct UiChoice {
    UiMode mode = UiMode::Standard;
    int theme = 0;            // app_prefs theme_mode: 0 follow system, 1 light, 2 dark
    bool has_accent = false;  // false keeps the theme's default accent
    uint32_t accent = 0;      // 0xRRGGBB, app_prefs accent_rgb
};

struct Options {
    bool silent = false;           // /SILENT or /VERYSILENT: no wizard
    bool very_silent = false;
    bool suppress_msgboxes = false;
    bool pulse_update = false;     // /PULSEUPDATE=1
    bool pulse_upgrade = false;    // /PULSEUPGRADE=1 (uninstaller)
    bool uninstall = false;
    bool test = false;             // /PULSETEST
    bool elevated_child = false;   // internal: /PULSE-ELEVATED
    bool cleanup_versions = false; // internal: /CLEANUPVERSIONS, run by the elevated dialog helper
    std::wstring dir;              // /DIR
    std::wstring log;              // /LOG[=file]; empty value means default path
    bool log_requested = false;
    std::wstring lang;
    std::wstring index_path;
    std::wstring result_file;      // internal: /RESULTFILE
    // Explicit /TASKS replaces the remembered selection; /MERGETASKS edits it.
    std::optional<std::wstring> tasks;
    std::optional<std::wstring> merge_tasks;
    std::wstring raw;              // original arguments (for the elevated relaunch)
};

Options ParseOptions(const wchar_t* command_line);

// Applies an Inno-style task list ("indexservice,!startup") onto `tasks`.
void ApplyTaskList(const std::wstring& list, Tasks& tasks, bool replace);
void ApplyRememberedTaskList(const std::wstring& list, Tasks& tasks);
std::wstring FormatTaskList(const Tasks& tasks);  // "indexservice,startup"

const char* UiModeKey(UiMode mode);  // "zen" / "standard" / "pro"
// setup-defaults.ini written by a fresh install: [features] and [ui].
std::string SetupDefaultsIni(const Tasks& tasks, const UiChoice& ui);
// Rewrites [ui] of an installed setup-defaults.ini (the wizard's final pick).
bool WriteUiChoice(const std::wstring& ini_path, const UiChoice& ui);

}  // namespace pulse::setup