#include "setup_options.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <vector>

#include "setup_common.h"

namespace pulse::setup {
namespace {

// Splits "/NAME=value" into name and value (value may be quoted).
void SplitSwitch(const std::wstring& arg, std::wstring& name, std::wstring& value, bool& has_value) {
    const auto eq = arg.find(L'=');
    has_value = eq != std::wstring::npos;
    name = has_value ? arg.substr(0, eq) : arg;
    value = has_value ? arg.substr(eq + 1) : std::wstring();
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size() - 2);
}

std::vector<std::wstring> SplitList(const std::wstring& list) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : list) {
        if (c == L',') { out.push_back(cur); cur.clear(); }
        else if (c != L' ') cur += c;
    }
    out.push_back(cur);
    return out;
}

}  // namespace

Options ParseOptions(const wchar_t* command_line) {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command_line, &argc);
    if (!argv) return o;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (!o.raw.empty()) o.raw += L' ';
        // Re-quote so the elevated relaunch receives identical arguments.
        const auto eq = arg.find(L'=');
        if (arg.find(L' ') != std::wstring::npos && eq != std::wstring::npos)
            o.raw += arg.substr(0, eq + 1) + L"\"" + arg.substr(eq + 1) + L"\"";
        else if (arg.find(L' ') != std::wstring::npos)
            o.raw += L"\"" + arg + L"\"";
        else
            o.raw += arg;
        if (arg.empty() || (arg[0] != L'/' && arg[0] != L'-')) continue;
        std::wstring name, value;
        bool has_value = false;
        SplitSwitch(arg, name, value, has_value);
        name[0] = L'/';
        if (EqualsNoCase(name, L"/SP-") || EqualsNoCase(name, L"/NORESTART") || EqualsNoCase(name, L"/NOCANCEL")) {}
        else if (EqualsNoCase(name, L"/SILENT")) o.silent = true;
        else if (EqualsNoCase(name, L"/VERYSILENT")) o.silent = o.very_silent = true;
        else if (EqualsNoCase(name, L"/SUPPRESSMSGBOXES")) o.suppress_msgboxes = true;
        else if (EqualsNoCase(name, L"/PULSEUPDATE")) o.pulse_update = value == L"1";
        else if (EqualsNoCase(name, L"/PULSEUPGRADE")) o.pulse_upgrade = value == L"1";
        else if (EqualsNoCase(name, L"/UNINSTALL")) o.uninstall = true;
        else if (EqualsNoCase(name, L"/PULSETEST")) o.test = true;
        else if (EqualsNoCase(name, L"/PULSE-ELEVATED")) o.elevated_child = true;
        else if (EqualsNoCase(name, L"/CLEANUPVERSIONS")) o.cleanup_versions = true;
        else if (EqualsNoCase(name, L"/DIR")) o.dir = value;
        else if (EqualsNoCase(name, L"/LOG")) { o.log_requested = true; o.log = value; }
        else if (EqualsNoCase(name, L"/LANG")) o.lang = value;
        else if (EqualsNoCase(name, L"/INDEXPATH")) o.index_path = value;
        else if (EqualsNoCase(name, L"/RESULTFILE")) o.result_file = value;
        else if (EqualsNoCase(name, L"/TASKS")) o.tasks = value;
        else if (EqualsNoCase(name, L"/MERGETASKS")) o.merge_tasks = value;
    }
    LocalFree(argv);
    return o;
}

void ApplyTaskList(const std::wstring& list, Tasks& tasks, bool replace) {
    if (replace) tasks = Tasks{false, false, false, false, false};
    for (auto item : SplitList(list)) {
        bool on = true;
        if (!item.empty() && item[0] == L'!') { on = false; item.erase(0, 1); }
        if (EqualsNoCase(item, L"indexservice")) tasks.index_service = on;
        else if (EqualsNoCase(item, L"globalsearch")) tasks.global_search = on;
        else if (EqualsNoCase(item, L"aiassistant")) tasks.ai_assistant = on;
        else if (EqualsNoCase(item, L"startup")) tasks.startup = on;
        else if (EqualsNoCase(item, L"desktopicon")) tasks.desktop_icon = on;
    }
}

void ApplyRememberedTaskList(const std::wstring& list, Tasks& tasks) {
    ApplyTaskList(list, tasks, true);
    bool feature_tokens = false;
    for (auto item : SplitList(list)) {
        if (!item.empty() && item[0] == L'!') item.erase(0, 1);
        feature_tokens |= EqualsNoCase(item, L"globalsearch") || EqualsNoCase(item, L"aiassistant");
    }
    if (!feature_tokens) tasks.global_search = tasks.ai_assistant = true;
}

std::wstring FormatTaskList(const Tasks& tasks) {
    std::wstring out;
    auto add = [&](bool on, const wchar_t* name) { if (on) { if (!out.empty()) out += L','; out += name; } };
    add(tasks.index_service, L"indexservice");
    add(true, tasks.global_search ? L"globalsearch" : L"!globalsearch");
    add(true, tasks.ai_assistant ? L"aiassistant" : L"!aiassistant");
    add(tasks.startup, L"startup");
    add(tasks.desktop_icon, L"desktopicon");
    return out;
}

const char* UiModeKey(UiMode mode) {
    switch (mode) {
        case UiMode::Zen: return "zen";
        case UiMode::Pro: return "pro";
        case UiMode::Standard: break;
    }
    return "standard";
}

std::string SetupDefaultsIni(const Tasks& tasks, const UiChoice& ui) {
    std::string out = std::string("[features]\r\nglobal_search=") + (tasks.global_search ? "1" : "0") +
        "\r\nai_assistant=" + (tasks.ai_assistant ? "1" : "0") + "\r\n";
    static const char* const kThemes[] = {"system", "light", "dark"};
    out += std::string("[ui]\r\nmode=") + UiModeKey(ui.mode) + "\r\ntheme=" +
        kThemes[ui.theme >= 0 && ui.theme <= 2 ? ui.theme : 0] + "\r\n";
    if (ui.has_accent) {
        char hex[8]{};
        std::snprintf(hex, sizeof(hex), "%06X", static_cast<unsigned>(ui.accent & 0xFFFFFFu));
        out += std::string("accent=") + hex + "\r\n";
    }
    return out;
}

bool WriteUiChoice(const std::wstring& ini_path, const UiChoice& ui) {
    static const wchar_t* const kThemes[] = {L"system", L"light", L"dark"};
    std::wstring mode;
    for (const char* p = UiModeKey(ui.mode); *p; ++p) mode += static_cast<wchar_t>(*p);
    wchar_t hex[8]{};
    swprintf_s(hex, L"%06X", static_cast<unsigned>(ui.accent & 0xFFFFFFu));
    const wchar_t* file = ini_path.c_str();
    bool ok = WritePrivateProfileStringW(L"ui", L"mode", mode.c_str(), file) != FALSE;
    ok = WritePrivateProfileStringW(L"ui", L"theme", kThemes[ui.theme >= 0 && ui.theme <= 2 ? ui.theme : 0], file) != FALSE && ok;
    // A null value deletes the key: no accent keeps the theme default.
    ok = WritePrivateProfileStringW(L"ui", L"accent", ui.has_accent ? hex : nullptr, file) != FALSE && ok;
    return ok;
}

}  // namespace pulse::setup