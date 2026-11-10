#include "../shell_host/context_working_directory.h"
#include <cstdio>

int main() {
    struct Case { const wchar_t* path; bool background; const wchar_t* expected; const char* label; };
    const Case cases[] = {
        {L"C:\\setup.inf", false, L"C:\\", "drive-root file keeps root separator"},
        {L"C:\\folder\\setup.inf", false, L"C:\\folder", "nested file uses parent"},
        {L"C:\\", false, L"C:\\", "selected drive root remains root"},
        {L"C:\\folder", true, L"C:\\folder", "background uses current folder"},
        {L"C:\\", true, L"C:\\", "background root remains root"},
        {L"\\\\server\\share\\setup.inf", false, L"\\\\server\\share", "UNC file uses share parent"},
        {L"\\\\?\\C:\\setup.inf", false, L"C:\\", "extended drive root is normalized"},
        {L"\\\\?\\UNC\\server\\share\\setup.inf", false, L"\\\\server\\share", "extended UNC is normalized"},
        {L"::{fixture}\\item", false, L"", "namespace item has no working directory"},
        {L"::pidl:fixture", false, L"", "PIDL item has no working directory"},
        {L"", false, L"", "empty path remains empty"},
    };
    int failures = 0;
    for (const auto& item : cases) {
        const bool ok = pulse::shell::ContextWorkingDirectory(item.path, item.background) == item.expected;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", item.label);
        if (!ok) ++failures;
    }
    std::printf("Checks=%zu Failures=%d\n", sizeof(cases) / sizeof(cases[0]), failures);
    return failures ? 1 : 0;
}
