#include "../app/selection_target.h"
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(const std::wstring& actual, const std::wstring& expected, const char* name) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

} // namespace

int main() {
    using pulse::app::SelectionTargetDirectory;

    Check(SelectionTargetDirectory(L"C:\\parent", L"C:\\parent\\child", true),
          L"C:\\parent\\child", "selected folder is the paste/new-item destination");
    Check(SelectionTargetDirectory(L"C:\\parent", L"C:\\parent\\child\\file.txt", false),
          L"C:\\parent\\child", "selected file targets its parent");
    Check(SelectionTargetDirectory(L"C:\\parent", L"C:\\file.txt", false),
          L"C:\\", "file directly under a drive targets the drive root");
    Check(SelectionTargetDirectory(L"C:\\parent", L"\\\\server\\share\\file.txt", false),
          L"\\\\server\\share", "UNC file targets its share directory");
    Check(SelectionTargetDirectory(L"pulse:device:0123456789ab\\storage",
                                   L"pulse:device:0123456789ab\\storage\\photo.jpg", false),
          L"pulse:device:0123456789ab\\storage", "device file targets its parent");
    Check(SelectionTargetDirectory(L"C:\\search", L"C:\\real\\folder\\file.txt", false),
          L"C:\\real\\folder", "virtual view file targets its real parent");
    Check(SelectionTargetDirectory(L"C:\\parent", L"", false),
          L"C:\\parent", "no resolved selection falls back to the current directory");
    return failures ? 1 : 0;
}
