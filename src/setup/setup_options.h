#pragma once
// Command line accepted by the Pulse setup. Inno Setup switches used by the
// in-app updater (src/app/update_installer.cpp) are accepted unchanged:
//   /SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /PULSEUPDATE=1 /LOG /DIR="..."
// plus /SILENT, /LOG="file", /TASKS="a,!b", /MERGETASKS="..", /LANG=..
// Pulse additions: /PULSETEST (isolated test install), /INDEXPATH="..",
// /UNINSTALL, /PULSEUPGRADE=1 (uninstall during an upgrade, keeps data).

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

struct Options {
    bool silent = false;           // /SILENT or /VERYSILENT: no wizard
    bool very_silent = false;
    bool suppress_msgboxes = false;
    bool pulse_update = false;     // /PULSEUPDATE=1
    bool pulse_upgrade = false;    // /PULSEUPGRADE=1 (uninstaller)
    bool uninstall = false;
    bool test = false;             // /PULSETEST
    bool elevated_child = false;   // internal: /PULSE-ELEVATED
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

}  // namespace pulse::setup
