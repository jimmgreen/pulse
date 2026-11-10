// Pulse setup bootstrapper: wizard (setup_ui) or silent install engine.
//
// The process starts unelevated (asInvoker). A machine install relaunches
// itself elevated and waits. The installer asks the interactive desktop to
// restart Pulse, so neither a low-integrity parent nor the admin token leaks
// into the GUI (--restore-update-session after /PULSEUPDATE=1).

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "setup_common.h"
#include "setup_engine.h"
#include "setup_log.h"
#include "setup_options.h"
#include "setup_payload.h"
#include "setup_strings.h"
#include "setup_ui.h"
#include "setup_uninstall.h"
#include "setup_desktop_launch.h"
#include "../common/os_version.h"

using namespace pulse::setup;

namespace {

std::wstring SelfPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) { path.resize(n); return path; }
        path.resize(path.size() * 2);
    }
}

std::wstring TempPath() {
    wchar_t buffer[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, buffer);
    std::wstring p(buffer, n);
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

std::wstring DefaultLogPath() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"\\Pulse-Setup-%04u%02u%02u-%02u%02u%02u.log", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond);
    return TempPath() + name;
}

void ShowError(const Options& o, const std::wstring& text) {
    if (o.very_silent || o.suppress_msgboxes) return;
    MessageBoxW(nullptr, text.c_str(), L"Pulse Setup", MB_OK | MB_ICONERROR);
}

void LaunchPulse(const std::wstring& app, const wchar_t* args) {
    const std::wstring exe = app + L"\\pulse.exe";
    const HRESULT result = LaunchFromDesktop(exe, args, app);
    if (SUCCEEDED(result)) {
        Log(L"Requested desktop-user launch: " + Quote(exe) + L" " + args);
    } else {
        Log(L"Desktop-user launch failed: " + std::to_wstring(static_cast<unsigned long>(result)));
    }
}

// Runs this setup elevated with the same arguments and returns its exit code.
int RunElevated(const Options& o, const std::wstring& self, std::wstring& app_dir, bool& launch) {
    const std::wstring result_file = TempPath() + L"\\Pulse-Setup-" + std::to_wstring(GetCurrentProcessId()) + L".result";
    DeleteFileW(result_file.c_str());
    std::wstring args = o.raw + L" /PULSE-ELEVATED /LOG=" + Quote(LogPath()) + L" /RESULTFILE=" + Quote(result_file);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        Log(L"Elevation failed: " + Win32Error(e));
        return e == ERROR_CANCELLED ? kExitCancelledBefore : kExitPrepareFailed;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = kExitPrepareFailed;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    std::string data;
    if (ReadWholeFile(result_file, data)) {
        // "<app dir>\n<launch|>"
        const auto nl = data.find('\n');
        app_dir = Utf8ToWide(data.substr(0, nl));
        launch = nl != std::string::npos && data.compare(nl + 1, 6, "launch") == 0;
    }
    DeleteFileW(result_file.c_str());
    return static_cast<int>(code);
}

int RunUninstaller(const Options& o, const std::wstring& self) {
    const Lang lang = DetectLanguage(o.lang.c_str());
    UninstallRequest request;
    request.options = o;
    request.app_dir = ParentDir(self);
    // InitializeUninstall: silent removal (the upgrade path) keeps user data.
    request.cleanup_data = !o.silent;
    if (!IsPulseInstallDir(request.app_dir)) {
        Log(L"Not a Pulse installation: " + request.app_dir);
        ShowError(o, L"Pulse was not found in " + request.app_dir + L".");
        return kExitPrepareFailed;
    }
    if (!o.test && !IsProcessElevated() && !o.elevated_child) {
        std::wstring unused;
        bool launch = false;
        const int code = RunElevated(o, self, unused, launch);
        Log(L"Elevated uninstaller exited with " + std::to_wstring(code));
        return code;
    }
    // Keep {app} free of our working directory so it can be removed.
    SetCurrentDirectoryW(TempPath().c_str());
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = kExitOk;
    bool self_delete = false;
    if (!o.silent) {
        const WizardOutcome w = RunUninstallWizard(self, request, lang);
        code = w.exit_code;
        self_delete = w.self_delete && code == kExitOk;
    } else {
        const UninstallResult r = RunUninstall(request, {});
        code = r.exit_code;
        self_delete = r.schedule_self_delete && code == kExitOk;
        if (code != kExitOk) ShowError(o, r.error);
        else if (r.restore_result > 0 && !o.suppress_msgboxes && !o.very_silent)
            MessageBoxW(nullptr, Text(lang, r.restore_result == 1 ? Str::IntegrationIncomplete : Str::IntegrationFailed),
                        L"Pulse", MB_OK | MB_ICONWARNING);
    }
    CoUninitialize();
    if (self_delete) ScheduleSelfDelete(self, request.app_dir);
    Log(L"Exit code " + std::to_wstring(code));
    return code;
}

// The kernel's build, not RtlGetVersion's: a compatibility layer on this file
// (属性 > 兼容性, or one inherited through __COMPAT_LAYER) made Windows 11 24H2
// report an old build and the Windows 10 gate refuse it. Unknown = no gate.
DWORD WindowsBuild() {
    const uint32_t reported = pulse::os::ReportedWindowsBuild();
    const uint32_t build = pulse::os::TrueWindowsBuild();
    if (reported != build) {
        wchar_t layer[512]{};
        const DWORD n = GetEnvironmentVariableW(L"__COMPAT_LAYER", layer, ARRAYSIZE(layer));
        Log(L"Compatibility layer: reported build " + std::to_wstring(reported) + L", real build " +
            std::to_wstring(build) + L", __COMPAT_LAYER=" + (n > 0 && n < ARRAYSIZE(layer) ? layer : L"(none)"));
    }
    return build ? build : 0xFFFFFFFF;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const Options o = ParseOptions(GetCommandLineW());
    LogOpen(o.log.empty() ? DefaultLogPath() : o.log);
    Log(L"Pulse setup: " + o.raw);
    const std::wstring self = SelfPath();

    // UI fixtures use an empty payload and never enter installation or uninstall dispatch.
    if (o.test && GetEnvironmentVariableW(L"PULSE_SETUP_UI_PROBE", nullptr, 0) != 0) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) return kExitPrepareFailed;
        Payload fixture_payload;
        InstallRequest fixture;
        fixture.options = o;
        fixture.app_dir = o.dir.empty() ? ParentDir(self) + L"\\probe-app" : o.dir;
        fixture.index_path = fixture.app_dir + L"\\index";
        const WizardOutcome outcome = RunWizard(fixture_payload, self, fixture, DetectLanguage(o.lang.c_str()));
        CoUninitialize();
        return outcome.exit_code;
    }

    // {app}\uninstall.exe is this bootstrapper without a payload.
    const size_t slash = self.find_last_of(L'\\');
    const std::wstring self_name = slash == std::wstring::npos ? self : self.substr(slash + 1);
    if (o.uninstall || EqualsNoCase(self_name, L"uninstall.exe")) return RunUninstaller(o, self);

    Payload payload;
    std::wstring error;
    if (!payload.Open(self, error)) {
        Log(L"Payload: " + error);
        ShowError(o, L"The setup file is damaged or incomplete. Please download it again.\n\n" + error);
        return kExitPrepareFailed;
    }
    // Same gate as [Setup] MinVersion in PulseSetup.iss (10.0 / 6.3).
    const DWORD build = WindowsBuild();
    if (payload.Info().min_windows_build && build < payload.Info().min_windows_build) {
        Log(L"Windows build " + std::to_wstring(build) + L" < required " + std::to_wstring(payload.Info().min_windows_build));
        ShowError(o, payload.Info().min_windows_build >= 10240
                         ? L"This version of Pulse requires Windows 10 or later.\n\nOn Windows 8.1, please download the Windows 8.1 x64 edition."
                         : L"This version of Pulse requires Windows 8.1 x64 or later.");
        return kExitPrepareFailed;
    }

    const bool needs_elevation = !o.test && !IsProcessElevated();
    if (needs_elevation && !o.elevated_child) {
        std::wstring app_dir;
        bool launch = false;
        const int code = RunElevated(o, self, app_dir, launch);
        Log(L"Elevated setup exited with " + std::to_wstring(code));
        // The installer child asks the desktop to launch. This parent may itself
        // be low-integrity (for example a downloaded installer opened by a host).
        return code;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const InstallRequest request = ResolveDefaults(o);
    InstallResult result;
    bool launch = false;
    if (!o.silent) {
        // The wizard reports its own errors.
        const WizardOutcome w = RunWizard(payload, self, request, DetectLanguage(o.lang.c_str()));
        result.exit_code = w.exit_code;
        result.app_dir = w.app_dir;
        launch = w.launch;
        if (launch && o.test) { Log(L"Test install: not starting Pulse"); launch = false; }
    } else {
        result = RunInstall(payload, self, request, {});
        if (result.exit_code != kExitOk && result.exit_code != kExitCancelledBefore) ShowError(o, result.error);
    }
    if (!o.result_file.empty() && result.exit_code == kExitOk) {
        const std::string data = WideToUtf8(result.app_dir) + (launch ? "\nlaunch" : "\n");
        WriteWholeFile(o.result_file, data.data(), data.size());
    }
    // Use the desktop token from the installer child as well as direct installs.
    // Never start the GUI with either a low-integrity parent or the admin token.
    if (!o.test && result.exit_code == kExitOk) {
        if (o.pulse_update) LaunchPulse(result.app_dir, L"--restore-update-session");
        else if (launch) LaunchPulse(result.app_dir, L"");
    }
    CoUninitialize();
    Log(L"Exit code " + std::to_wstring(result.exit_code));
    return result.exit_code;
}
