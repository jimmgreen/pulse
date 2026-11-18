#include "../../app/extensions/native/crash_guard.h"
#include "../../app/extensions/native/module.h"
#include "../../app/extensions/runtime.h"
#include "../../common/crash_reporter.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>

namespace fs = std::filesystem;
namespace guard = pulse::extensions::native::crash_guard;
namespace {
pulse::extensions::Manifest manifest;
bool enabled = true;
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
DWORD Run(const fs::path& exe, const fs::path& root, const fs::path& dll, const wchar_t* mode) {
    std::wstring command = L"\"" + exe.wstring() + L"\" \"" + root.wstring() +
        L"\" \"" + dll.wstring() + L"\" " + mode;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 10;
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD code = 11;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else { TerminateProcess(process.hProcess, 11); WaitForSingleObject(process.hProcess, 5000); }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}
using Click = void (PULSE_CALL*)();
Click click = nullptr;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && LOWORD(wparam) == 1 && HIWORD(wparam) == BN_CLICKED) {
        click();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

// Replace only package discovery in this component host; the module loader,
// crash reporter, crash guard and restart persistence are production code.
namespace pulse::extensions {
size_t PackageCount() { return 1; }
const Manifest* PackageManifest(size_t index) { return index == 0 ? &manifest : nullptr; }
void SetPackageEnabled(size_t index, bool value) { if (index == 0) enabled = value; }
bool DevelopmentMode() { return false; }
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 4) {
        const fs::path root = argv[1];
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", root.c_str());
        manifest.id = "test.native-crash";
        manifest.name = "Native crash fixture";
        manifest.native.emplace();
        manifest.native->sha256 = std::string(64, 'a');
        const std::wstring mode = argv[3];
        const auto notices = guard::ApplyAtStartup(true);
        if (mode == L"recover") return !enabled && notices.size() == 1 &&
            notices[0] == L"Native crash fixture" ? 0 : 12;
        if (mode == L"repeat") return !enabled && notices.empty() ? 0 : 13;
        if (mode == L"reenable") {
            guard::ForgetPackageCrash(0);
            return fs::exists(root / "extensions" / "crash-disabled.txt") ? 14 : 0;
        }
        if (!enabled || !notices.empty()) return 15;
        if (!pulse::crash::Initialize({pulse::crash::ProcessRole::Test, false, root.wstring()})) return 16;
        pulse::extensions::native::Module module;
        const PulseHostV1 host{sizeof(host), PULSE_ABI_VERSION_1, nullptr,
            [](void*, PulseStringView, uint32_t, void*, uint32_t) -> PulseStatus { return PULSE_ERROR_UNAVAILABLE; }};
        {
            guard::ActivationScope activation(0);
            if (module.Load(fs::absolute(argv[2]).wstring(), host) != PULSE_OK) return 17;
            if (!guard::TrackModule(0, module.Library())) return 18;
            if (module.Activate() != PULSE_OK) return 19;
        }
        if (mode == L"healthy") {
            guard::ForgetModule(0);
            module.Close();
            pulse::crash::Shutdown();
            return fs::exists(root / "extensions" / "native-activating.txt") ? 20 : 0;
        }
        click = reinterpret_cast<Click>(GetProcAddress(module.Library(), "PulseCrashClick"));
        if (!click) return 21;
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseCrashGuardTest";
        if (!RegisterClassW(&wc)) return 22;
        HWND window = CreateWindowW(wc.lpszClassName, L"Crash callback test", 0,
            0, 0, 200, 100, nullptr, nullptr, wc.hInstance, nullptr);
        HWND button = CreateWindowW(L"BUTTON", L"Crash", WS_CHILD, 0, 0, 80, 30,
            window, reinterpret_cast<HMENU>(1), wc.hInstance, nullptr);
        if (!window || !button) return 23;
        SendMessageW(button, BM_CLICK, 0, 0);
        return 24;
    }
    if (argc != 2) return 2;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    const auto root = fs::absolute(fs::path("bench_data") /
        ("native-crash-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    const auto binaries = fs::absolute(argv[1]);
    for (const auto* mode : {L"activate", L"callback"}) {
        const auto profile = root / mode;
        fs::create_directories(profile / "extensions");
        const auto dll = binaries / (std::wstring(mode) == L"activate" ? L"native_crash_activate.dll" : L"native_crash_callback.dll");
        const DWORD code = Run(executable, profile, dll, mode);
        std::printf("case=%ls exit=0x%08lx\n", mode, code);
        Check(code == EXCEPTION_ACCESS_VIOLATION, "real plugin fault terminates child process");
        Check(Read(profile / "extensions" / "native-crashed.txt").starts_with("test.native-crash\t"),
            "fault address attributes crash to plugin and persists identity");
        Check(fs::exists(profile / "extensions" / "native-activating.txt") == (std::wstring(mode) == L"activate"),
            "activation marker only survives activation crash");
        bool attributed = false;
        for (const auto& entry : fs::directory_iterator(profile / "Diagnostics" / "Crashes"))
            if (entry.path().extension() == L".json" && Read(entry.path()).find("\"extension\":\"test.native-crash\"") != std::string::npos)
                attributed = true;
        Check(attributed, "production crash JSON identifies plugin");
        HANDLE locked = CreateFileW((profile / "extensions" / "crash-disabled.txt").c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "lock disabled list to simulate persistence failure");
        Check(Run(executable, profile, dll, L"recover") == 0,
            "persistence failure still disables plugin in current process");
        Check(fs::exists(profile / "extensions" / "native-crashed.txt"),
            "persistence failure retains crash marker for subsequent restart");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        Check(Run(executable, profile, dll, L"recover") == 0, "fresh process disables plugin and emits one notice");
        Check(Run(executable, profile, dll, L"repeat") == 0, "following restart stays disabled without repeated notice");
        Check(Run(executable, profile, dll, L"reenable") == 0, "explicit re-enable clears crash record");
        Check(Run(executable, profile, binaries / L"native_crash_callback.dll", L"healthy") == 0,
            "healthy activation removes marker and permits clean unload");
    }
    std::printf("Fixture: %ls\n", root.c_str());
    return failures ? 1 : 0;
}
