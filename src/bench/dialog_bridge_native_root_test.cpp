#include "../common/dialog_bridge_paths.h"
#include <shlobj.h>
#include <cstdio>
#include <string>

namespace {
std::wstring QueryKnownFolder(const KNOWNFOLDERID& id, HRESULT& status) {
    PWSTR raw = nullptr;
    status = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    const std::wstring result = SUCCEEDED(status) && raw ? raw : L"";
    if (raw) CoTaskMemFree(raw);
    return result;
}
std::wstring NativeMachineSetting() {
    wchar_t path[32768]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", L"CommonFilesDir",
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, path, &size) != ERROR_SUCCESS) return {};
    return path;
}
}
int wmain() {
    namespace paths = pulse::dialog_bridge_paths;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks; if (!ok) ++failures;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    HRESULT native_status = E_FAIL, process_status = E_FAIL;
    const auto native = QueryKnownFolder(FOLDERID_ProgramFilesCommonX64, native_status);
    const auto process = QueryKnownFolder(FOLDERID_ProgramFilesCommon, process_status);
    BOOL wow64 = FALSE;
    const BOOL detected = IsWow64Process(GetCurrentProcess(), &wow64);
    check(detected != FALSE && SUCCEEDED(process_status) && !process.empty(), "process architecture and Common Files API available");
#ifdef _WIN64
    const bool native64 = true;
#else
    const bool native64 = wow64 != FALSE;
#endif
    const auto machine = NativeMachineSetting();
    check(!native64 || !machine.empty(), "native Common Files machine setting is available for independent comparison");
    const auto expected = (native64 ? machine : process) + L"\\Pulse\\DialogBridge";
    check(native.empty() || native == machine, "available CommonX64 API agrees with native machine configuration");
    const auto actual = paths::CommonDialogBridgeRoot();
    check(actual == expected, "default bridge root selects the native Common Files directory");
    check(paths::IsUnderProgramFiles(actual), "native bridge root passes Program Files classification in this process architecture");
    check(paths::CommonDialogBridgeRoot(L"C:\\PrivateBridge\\") == L"C:\\PrivateBridge", "explicit isolated override remains unchanged");
    std::wprintf(L"arch=%u wow64=%d native_hr=0x%08lx process_common=%ls native_common=%ls actual=%ls\n",
        static_cast<unsigned>(sizeof(void*) * 8), wow64, static_cast<unsigned long>(native_status),
        process.c_str(), native.c_str(), actual.c_str());
    std::printf("RESULT checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
