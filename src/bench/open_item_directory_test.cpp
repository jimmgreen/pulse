#include "../ops/open_item_directory.h"
#include <cstdio>

namespace {
int calls = 0, failures = 0;
DWORD returned_attrs = FILE_ATTRIBUTE_DIRECTORY;
DWORD WINAPI Attributes(LPCWSTR) { ++calls; return returned_attrs; }
bool Remote(std::wstring_view path) { return pulse::fs::DriveLetterOf(path) == L'Z'; }
void Check(const wchar_t* path, const wchar_t* expected, int expected_calls) {
    calls = 0;
    const auto result = pulse::ops::OpenItemWorkingDirectory(path, Attributes, Remote);
    const bool ok = result == expected && calls == expected_calls;
    std::printf("[%s] working directory; remote attribute probes=%d expected=%d\n",
        ok ? "PASS" : "FAIL", calls, expected_calls);
    if (!ok) ++failures;
}
}
int main() {
    Check(L"\\\\nas\\share\\video.mp4", L"\\\\nas\\share", 0);
    Check(L"\\\\?\\UNC\\nas\\share\\folder\\video.mkv", L"\\\\nas\\share\\folder", 0);
    Check(L"Z:\\video.mp4", L"Z:\\", 0);
    Check(L"\\\\?\\Z:\\folder\\video.mp4", L"Z:\\folder", 0);
    Check(L"Z:\\folder with spaces\\视频.mp4", L"Z:\\folder with spaces", 0);
    Check(L"C:\\video.mp4", L"C:\\", 1);
    Check(L"\\\\?\\C:\\folder\\video.mp4", L"C:\\folder", 1);
    returned_attrs = INVALID_FILE_ATTRIBUTES;
    Check(L"C:\\missing\\video.mp4", L"", 1);
    // Remote resolution does not depend on a successful metadata read.
    Check(L"\\\\nas\\offline\\video.mp4", L"\\\\nas\\offline", 0);
    returned_attrs = FILE_ATTRIBUTE_NORMAL;
    Check(L"C:\\not-a-folder\\video.mp4", L"", 1);
    Check(L"::{namespace}", L"", 0);
    Check(L"video.mp4", L"", 0);
    return failures ? 1 : 0;
}
