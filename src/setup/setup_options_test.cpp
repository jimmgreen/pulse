#include "setup_options.h"
#include "setup_common.h"
#include <shlobj.h>
#include <windows.h>
#include <cstdio>
int main() {
    using namespace pulse::setup;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    Tasks t;
    check(t.global_search && t.ai_assistant, "feature defaults");
    ApplyTaskList(L"globalsearch,!aiassistant,startup", t, true);
    check(t.global_search && !t.ai_assistant && t.startup && !t.index_service && !t.desktop_icon, "replace tasks");
    ApplyTaskList(L"!globalsearch,AIASSISTANT", t, false);
    check(!t.global_search && t.ai_assistant && t.startup, "merge tasks");
    Tasks roundtrip;
    ApplyTaskList(FormatTaskList(t), roundtrip, true);
    check(FormatTaskList(t) == FormatTaskList(roundtrip), "round trip");
    const auto options = ParseOptions(L"setup.exe /TASKS=globalsearch /MERGETASKS=!globalsearch,aiassistant");
    Tasks parsed;
    ApplyTaskList(*options.tasks, parsed, true);
    ApplyTaskList(*options.merge_tasks, parsed, false);
    check(!parsed.global_search && parsed.ai_assistant && !parsed.index_service, "command line");
    ApplyRememberedTaskList(L"indexservice,startup", t);
    check(t.global_search && t.ai_assistant && t.startup, "legacy remembered tasks");
    ApplyRememberedTaskList(L"indexservice,!globalsearch,!aiassistant", t);
    check(!t.global_search && !t.ai_assistant, "remember explicit feature opt out");
    ApplyTaskList(L"indexservice", t, true);
    check(!t.global_search && !t.ai_assistant, "explicit old task list replaces all");
    {
        const UiChoice ui;
        check(ui.mode == UiMode::Standard && ui.theme == 0 && !ui.has_accent, "ui choice defaults to standard");
        check(SetupDefaultsIni(Tasks{}, ui) ==
                  "[features]\r\nglobal_search=1\r\nai_assistant=1\r\n[ui]\r\nmode=standard\r\ntheme=system\r\n",
              "defaults ini records the standard layout");
        Tasks off;
        off.global_search = false;
        const UiChoice zen{UiMode::Zen, 2, true, 0x0067C0};
        check(SetupDefaultsIni(off, zen) ==
                  "[features]\r\nglobal_search=0\r\nai_assistant=1\r\n[ui]\r\nmode=zen\r\ntheme=dark\r\naccent=0067C0\r\n",
              "defaults ini records mode, theme and accent");
        check(std::string(UiModeKey(UiMode::Pro)) == "pro", "pro mode key");
    }
    {
        wchar_t dir[MAX_PATH + 1]{};
        GetTempPathW(MAX_PATH + 1, dir);
        const std::wstring ini = std::wstring(dir) + L"pulse_setup_ui_choice_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
        const std::string seed = SetupDefaultsIni(Tasks{}, UiChoice{UiMode::Standard, 0, true, 0x5B63D9});
        WriteWholeFile(ini, seed.data(), seed.size());
        const bool written = WriteUiChoice(ini, UiChoice{UiMode::Pro, 1, false, 0});
        wchar_t mode[16]{}, theme[16]{}, accent[16]{};
        GetPrivateProfileStringW(L"ui", L"mode", L"", mode, 16, ini.c_str());
        GetPrivateProfileStringW(L"ui", L"theme", L"", theme, 16, ini.c_str());
        GetPrivateProfileStringW(L"ui", L"accent", L"none", accent, 16, ini.c_str());
        check(written && std::wstring(mode) == L"pro" && std::wstring(theme) == L"light" && std::wstring(accent) == L"none",
              "final pick rewrites [ui] and drops the accent");
        check(GetPrivateProfileIntW(L"features", L"global_search", 0, ini.c_str()) == 1, "final pick keeps [features]");
        DeleteFileW(ini.c_str());
    }

    const std::wstring default_app = NormalizeDir(KnownFolder(FOLDERID_ProgramFiles) + L"\\Pulse");
    const std::wstring progdata_idx = NormalizeDir(KnownFolder(FOLDERID_ProgramData) + L"\\Pulse\\Index");
    check(SameDirectory(DefaultIndexPathForApp(default_app, false), progdata_idx), "default app dir keeps ProgramData index");
    check(DefaultIndexPathForApp(L"F:\\Y-App\\Pulse", false) == L"F:\\Y-App\\Pulse\\Index", "custom app dir follows with Index subfolder");
    check(NormalizeIndexSelection(L"F:\\", L"F:\\Y-App\\Pulse", {}, true) == L"F:\\Index", "drive root normalizes to Index");
    check(NormalizeIndexSelection(L"F:\\Y-App\\Pulse", L"F:\\Y-App\\Pulse", {}, true) == L"F:\\Y-App\\Pulse\\Index", "app dir selection appends Index");
    check(NormalizeIndexSelection(L"F:\\Y-App", L"F:\\Y-App\\Pulse", {}, false) == L"F:\\Y-App\\Index", "app parent selection appends Index");

    wchar_t tmp_buf[MAX_PATH + 1]{};
    const DWORD tmp_len = GetTempPathW(MAX_PATH + 1, tmp_buf);
    if (tmp_len > 0 && tmp_len <= MAX_PATH) {
        std::wstring base = NormalizeDir(tmp_buf) + L"\\pulse_setup_opts_test_" + std::to_wstring(GetCurrentProcessId());
        RemoveTree(base);
        CreateDirs(base + L"\\Y-App");
        WriteWholeFile(base + L"\\Y-App\\dummy.txt", "x", 1);
        CreateDirs(base + L"\\MyIdx");
        check(NormalizeIndexSelection(base + L"\\Y-App", L"C:\\Program Files\\Pulse", {}, true) == base + L"\\Y-App\\Index",
              "browse non-empty non-index dir appends Index");
        check(NormalizeIndexSelection(base + L"\\MyIdx", L"C:\\Program Files\\Pulse", {}, true) == base + L"\\MyIdx",
              "browse empty custom dir preserves chosen path");

        CreateDirs(base + L"\\TestIdx\\v9\\0123456789abcdef");
        WriteWholeFile(base + L"\\TestIdx\\pulse-index.bin", "idx", 3);
        WriteWholeFile(base + L"\\TestIdx\\v9\\0123456789abcdef\\base-a.bin", "shard", 5);
        check(DirectoryHasPulseIndexArtifacts(base + L"\\TestIdx"), "detects pulse index artifacts");
        DeletePulseIndexDirectoryArtifacts(base + L"\\TestIdx");
        check(GetFileAttributesW((base + L"\\TestIdx").c_str()) == INVALID_FILE_ATTRIBUTES, "removes empty index dir after shard cleanup");

        CreateDirs(base + L"\\KeepIdx");
        WriteWholeFile(base + L"\\KeepIdx\\pulse-index.bin", "idx", 3);
        WriteWholeFile(base + L"\\KeepIdx\\notes.txt", "keep", 4);
        DeletePulseIndexDirectoryArtifacts(base + L"\\KeepIdx");
        check(GetFileAttributesW((base + L"\\KeepIdx\\pulse-index.bin").c_str()) == INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW((base + L"\\KeepIdx\\notes.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
              "cleanup deletes artifacts while preserving unrelated user files");
        RemoveTree(base);
    }
    return failures ? 1 : 0;
}