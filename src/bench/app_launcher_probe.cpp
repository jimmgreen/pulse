#include "../common/app_runtime_paths.h"
#include "../common/json_value.h"
#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    std::filesystem::path record;
    int exit_code = 0, delay = 0; bool echo = false;
    wchar_t shell_record[32768]{};
    if (argc == 3 && wcscmp(argv[1], L"--shell-open") == 0 &&
        GetEnvironmentVariableW(L"PULSE_LAUNCHER_PROBE_RECORD", shell_record, ARRAYSIZE(shell_record))) {
        record = shell_record;
        exit_code = 37;
    }
    // Record arguments before interpreting probe-only switches.
    std::wstring args;
    for (int i = 1; i < argc; ++i) { args += L"[" + std::wstring(argv[i]) + L"]"; }
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--record") == 0 && i + 1 < argc) record = argv[++i];
        else if (wcscmp(argv[i], L"--exit-code") == 0 && i + 1 < argc) exit_code = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"--delay") == 0 && i + 1 < argc) delay = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"--echo") == 0) echo = true;
    }
    LocalFree(argv);
    const auto runtime = pulse::app_runtime_paths::RuntimeDirectory();
    const auto stable = pulse::app_runtime_paths::StableEntryPath();
    const auto root = pulse::app_runtime_paths::InstallRoot();
    std::string text = "arguments=" + pulse::json::Utf8(args) + "\nruntime=" + pulse::json::Utf8(runtime) +
        "\nstable=" + pulse::json::Utf8(stable) + "\nroot=" + pulse::json::Utf8(root) +
        "\ncwd=" + pulse::json::Utf8(std::filesystem::current_path().wstring()) + "\n";
    if (!record.empty()) { std::ofstream out(record, std::ios::binary); out << text; if (!out) return 3; }
    if (echo) {
        char bytes[4096]; DWORD count = 0, written = 0;
        if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), bytes, sizeof(bytes), &count, nullptr)) return 4;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes, count, &written, nullptr) || written != count) return 5;
        const char error[] = "probe stderr\n";
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), error, sizeof(error) - 1, &written, nullptr);
    }
    if (delay > 0 && delay <= 10000) Sleep(static_cast<DWORD>(delay));
    return exit_code;
}
