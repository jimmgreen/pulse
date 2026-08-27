#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pulse;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const wchar_t* name) {
    if (condition) {
        ++g_pass;
        wprintf(L"[PASS] %s\n", name);
    } else {
        ++g_fail;
        wprintf(L"[FAIL] %s\n", name);
    }
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::wstring SelfPath() {
    std::wstring value(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, value.data(),
                                          static_cast<DWORD>(value.size()));
    value.resize(size);
    return value;
}

bool RunChild(const std::wstring& root, const wchar_t* mode) {
    std::wstring command = Quote(SelfPath()) + L" --child " + Quote(root) + L" " + mode;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return false;
    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    DWORD code = 0;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code != 0;
}

size_t CountExtension(const std::filesystem::path& path, const wchar_t* extension) {
    size_t count = 0;
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(path, error)) {
        if (item.is_regular_file() && item.path().extension() == extension) ++count;
    }
    return count;
}

std::string FirstJson(const std::filesystem::path& path) {
    std::error_code error;
    for (const auto& item : std::filesystem::directory_iterator(path, error)) {
        if (item.path().extension() != L".json") continue;
        std::ifstream file(item.path(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    }
    return {};
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && wcscmp(argv[1], L"--child") == 0) {
        crash::Initialize({crash::ProcessRole::Test, false, argv[2]});
        crash::AddBreadcrumb(7, 11, 13);
        if (wcscmp(argv[3], L"terminate") == 0) crash::ReportTerminate("test-terminate");
        RaiseException(0xE0424242u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 0;
    }

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::filesystem::path root = std::filesystem::path(temp) /
        (L"PulseCrashTest-" + std::to_wstring(GetCurrentProcessId()));
    const std::filesystem::path crashes = root / L"Diagnostics" / L"Crashes";
    const std::filesystem::path exported = root / L"Export";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);

    Check(RunChild(root.wstring(), L"seh"), L"unhandled SEH terminates child");
    Check(RunChild(root.wstring(), L"terminate"), L"terminate handler terminates child");
    Check(CountExtension(crashes, L".dmp") == 2, L"crashes create minidumps");
    Check(CountExtension(crashes, L".json") == 2, L"crashes create JSON metadata");
    const std::string json = FirstJson(crashes);
    Check(json.find("\"schema\":1") != std::string::npos &&
          json.find("\"build_id\"") != std::string::npos &&
          json.find("\"breadcrumbs\"") != std::string::npos,
          L"metadata contains stable schema and breadcrumbs");

    for (int i = 0; i < 11; ++i) RunChild(root.wstring(), L"seh");
    crash::Initialize({crash::ProcessRole::Test, false, root.wstring()});
    Check(CountExtension(crashes, L".json") <= 10 &&
          CountExtension(crashes, L".dmp") <= 10,
          L"startup retention keeps at most ten event pairs");

    std::filesystem::create_directories(exported, error);
    diagnostics::ExportOptions options;
    options.source_root = root.wstring();
    options.destination = exported.wstring();
    std::wstring export_error;
    Check(diagnostics::Export(options, &export_error), L"diagnostics export succeeds");
    Check(std::filesystem::exists(exported / L"diagnostics-manifest.json"),
          L"diagnostics export writes manifest");
    Check(CountExtension(exported / L"Crashes", L".dmp") <= 10,
          L"diagnostics export includes retained dumps");

    crash::Shutdown();
    std::filesystem::remove_all(root, error);
    wprintf(L"%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
