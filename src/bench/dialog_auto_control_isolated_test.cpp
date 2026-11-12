// Exercise the unchanged native control entrypoint inside a process-private
// HKCU override. This is a test executable, never the production helper.
#define wmain PulseDialogAutoControlMain
#include "../dialog_launcher/auto_main.cpp"
#undef wmain
#include <cwctype>

int wmain(int argc, wchar_t** argv) {
    wchar_t token[64]{};
    const DWORD units = GetEnvironmentVariableW(L"PULSE_AUTO_TEST_HIVE", token, 64);
    if (units != 36) return 87;
    for (DWORD i = 0; i < units; ++i) if (!iswxdigit(token[i]) && token[i] != L'-') return 87;
#ifdef _WIN64
    constexpr wchar_t suffix[] = L"_x64";
#else
    constexpr wchar_t suffix[] = L"_x86";
#endif
    const std::wstring path = std::wstring(L"Software\\PulseDialogAutoControlIsolated_") + token + suffix;
    if (argc == 2 && wcscmp(argv[1], L"--test-identity") == 0) {
#ifdef _WIN64
        std::cout << "{\"fixture\":\"pulse-native-control-isolated-test\",\"architecture\":\"x64\"}\n";
#else
        std::cout << "{\"fixture\":\"pulse-native-control-isolated-test\",\"architecture\":\"x86\"}\n";
#endif
        return 0;
    }
    if (argc == 2 && wcscmp(argv[1], L"--test-cleanup") == 0) {
        const DWORD error = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        return error == ERROR_FILE_NOT_FOUND ? 0 : static_cast<int>(error);
    }
    HKEY root = nullptr;
    DWORD error = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr);
    if (!error) error = RegOverridePredefKey(HKEY_CURRENT_USER, root);
    if (error) { if (root) RegCloseKey(root); return static_cast<int>(error); }
    const int result = PulseDialogAutoControlMain(argc, argv);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(root);
    // Model a helper that mutates its view then fails before a trustworthy
    // reply reaches the controller; the transaction must undo the attempted view.
    wchar_t fail[2]{};
    const bool inject = GetEnvironmentVariableW(L"PULSE_AUTO_TEST_FAIL_ENABLE32", fail, 2) == 1 && fail[0] == L'1';
#ifndef _WIN64
    if (inject && !result && argc == 3 && wcscmp(argv[1], L"--enable-json") == 0) return 1;
#else
    (void)inject;
#endif
    return result;
}
