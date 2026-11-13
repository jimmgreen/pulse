#include "../ops/archive_open_fallback.h"
#include <cstdio>

int main() {
    using namespace pulse::ops::archive_open;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    const auto check = [&](bool okay, const char* name) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", name);
        if (!okay) ++failures;
    };
    check(Supported(L"C:\\Downloads\\file.ZIP") && Supported(L"\\\\server\\share\\a.tar.gz"),
        "ordinary archives include case insensitive and UNC paths");
    for (const auto* path : {L"a.msi", L"a.exe", L"a.apk", L"a.jar", L"a.iso", L"a.vhd", L"a.001", L"a.zip.exe"})
        check(!Supported(path), "installers images arbitrary volumes and executable suffixes stay native");
    check(ShouldExtract(HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION), {}), "missing default uses archive fallback");
    check(ShouldExtract(S_OK, L"CompressedFolder"), "Windows ZIP folder default uses smart extract");
    check(!ShouldExtract(S_OK, L"WinRAR.ZIP") && !ShouldExtract(S_OK, L"7-Zip.zip") &&
        !ShouldExtract(S_OK, L"AppXCustomArchive"), "third party and delegated defaults are preserved");
    check(!ShouldExtract(E_ACCESSDENIED, {}) && !ShouldExtract(E_FAIL, {}), "unknown association errors never take over");
    check(ShouldExtract(S_OK, L"ArchiveFolder"), "Windows archive folder default also uses smart extract");
    std::wstring progid;
    const auto result = CurrentDefault(L".pulse-unregistered-archive-fixture-45cc980e", progid);
    std::printf("UNREGISTERED_HRESULT=0x%08lX\n", static_cast<unsigned long>(result));
    check(MissingAssociation(result), "real unregistered extension is recognized without changing registration");
    check(!NeedsFallback(L"C:\\pulse-nonexistent-archive-fixture-45cc980e.zip"), "missing archive never queues extraction");
    CoUninitialize();
    return failures ? 1 : 0;
}
