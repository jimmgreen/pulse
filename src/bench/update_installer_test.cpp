#include "../app/update_installer.h"
#include "../app/update_transport.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    HWND window = CreateWindowExW(0, L"STATIC", L"Update test", 0, 0, 0, 0, 0, HWND_MESSAGE,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (argc == 4 && std::wstring_view(argv[1]) == L"--download") {
        UpdateResult release;
        release.update_available = true;
        release.download_page = argv[2];
        release.installer_sha256 = argv[3];
        UpdateInstaller installer;
        check(installer.Start(release, window, WM_APP + 1), "live installer download starts");
        const auto deadline = GetTickCount64() + 180000;
        DWORD error = ERROR_TIMEOUT;
        while (GetTickCount64() < deadline && !installer.TakeResult(error)) Sleep(20);
        check(error == ERROR_SUCCESS, "live HTTPS installer downloaded and hash verified without execution");
        std::cout << "diagnostic=" << error << '\n';
        installer.Stop();
        DestroyWindow(window);
        return failures ? 1 : 0;
    }
    namespace fs = std::filesystem;
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"update-test-" + std::to_wstring(GetCurrentProcessId())));
    fs::create_directories(root);
    const auto file = root / L"安装包.exe";
    std::ofstream(file, std::ios::binary) << "abc";
    constexpr wchar_t hash[] = L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    HANDLE guard = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(VerifyUpdateInstaller(guard, hash), "signed SHA256 matches exact installer bytes");
    check(!VerifyUpdateInstaller(guard, std::wstring(64, L'0')), "changed installer hash rejected");
    check(!VerifyUpdateInstaller(guard, L"short"), "malformed installer hash rejected");
    HANDLE writer = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(writer == INVALID_HANDLE_VALUE, "verified installer is locked against replacement before launch");
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    CloseHandle(guard);
    std::ofstream(file, std::ios::binary | std::ios::trunc) << "changed";
    guard = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(!VerifyUpdateInstaller(guard, hash), "tampered installer bytes rejected");
    CloseHandle(guard);
    UpdateError category = UpdateError::None;
    DWORD error = 0;
    std::atomic<bool> cancelled{false};
    auto consume = [](const void*, DWORD) { return true; };
    check(!ReadUpdateResponse(L"http://example.test/update.exe", 100, cancelled, consume, category, error) &&
        category == UpdateError::InsecureUrl, "HTTP installer URL rejected before network access");
    check(!ReadUpdateResponse(L"https://user:password@example.test/update.exe", 100, cancelled, consume, category, error) &&
        category == UpdateError::InsecureUrl, "credential-bearing URL rejected");
    cancelled = true;
    check(!ReadUpdateResponse(L"https://example.test/update.exe", 100, cancelled, consume, category, error) &&
        error == ERROR_CANCELLED, "cancelled request does not access the network");
    UpdateInstaller installer;
    UpdateResult invalid;
    check(UpdateInstallErrorFromExitCode(0) == ERROR_SUCCESS, "completed setup succeeds");
    check(UpdateInstallErrorFromExitCode(2) == ERROR_CANCELLED &&
        UpdateInstallErrorFromExitCode(5) == ERROR_CANCELLED, "setup cancellation is reported");
    for (DWORD code : {1ul, 3ul, 4ul, 6ul, 7ul, 8ul, 999ul}) {
        check(UpdateInstallErrorFromExitCode(code) == ERROR_INSTALL_FAILURE,
            "failed or unknown setup exit is not mistaken for success");
    }
    check(!installer.TakeInstallResult(error), "no completion before installer launch");
    check(!installer.Start(invalid, window, WM_APP + 1), "unverified update cannot start installer download");
    const auto before = GetTickCount64();
    installer.Stop();
    check(GetTickCount64() - before < 100, "shutdown does not wait for network activity");
    check(!installer.Launch(window, error), "installer cannot launch before successful verification");
    fs::remove_all(root);
    DestroyWindow(window);
    return failures ? 1 : 0;
}
