#include "../app/direct_reveal_bootstrap.h"
#include "../app/direct_reveal_targets.h"
#include "../app/explorer_launch_origin.h"
#include <cstdio>
#include <string>
#include <filesystem>
using namespace pulse::app::direct_reveal_bootstrap_detail;
namespace targets = pulse::app::direct_reveal_targets;
namespace origin = pulse::app::explorer_launch_origin;
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--child") == 0) { Sleep(30000); return 0; }
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    // Isolated learned-target store: never the user's real list.
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const auto store = (std::filesystem::path(temp) / (L"pulse-reveal-targets-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".txt")).wstring();
    targets::SetStorePathForTest(store);
    check(AllowedImage(L"C:\\Program Files\\Tencent\\Weixin\\Weixin.exe"), "installed Weixin target");
    check(AllowedImage(L"c:\\program files\\tencent\\wechat\\4.2\\WeChat.exe"), "installed WeChat case and version subdirectory");
    check(AllowedImage(L"D:\\Apps\\Weixin\\Weixin.exe"), "Weixin outside Program Files accepted");
    check(AllowedImage(L"C:\\Users\\u\\AppData\\Local\\Programs\\Alma\\Alma.exe"), "per-user Alma install accepted");
    check(AllowedImage(L"E:\\Portable\\Alma\\alma.exe"), "portable Alma case-insensitive name accepted");
    check(!AllowedImage(L"C:\\Program Files\\Tencent\\Weixin\\other.exe"), "unrelated target rejected");
    check(!AllowedImage(L"C:\\Apps\\NotAlma.exe") && !AllowedImage(L"C:\\Apps\\Alma.exe.bak"), "name prefix/suffix spoof rejected");
    check(!AllowedImage(L"C:\\Program Files\\Tencent\\Weixin\\..\\Weixin.exe"), "parent traversal rejected");
    check(!AllowedImage(L"\\\\server\\share\\Alma\\Alma.exe") && !AllowedImage(L"Alma.exe") && !AllowedImage(L"C:/Apps/Alma.exe"),
        "UNC, relative and forward-slash images rejected");
    // Split AppRuntime / DialogBridge install-root resolution.
    const std::wstring hex64(64, L'a');
    check(InstallRootFromExecutable(L"C:\\Program Files\\Pulse\\AppRuntime\\" + hex64 + L"\\pulse.exe") == L"C:\\Program Files\\Pulse",
        "AppRuntime split bundle resolves to install root");
    check(InstallRootFromExecutable(L"C:\\Program Files\\Pulse\\DialogBridge\\" + hex64 + L"\\pulse.exe") == L"C:\\Program Files\\Pulse",
        "DialogBridge bundle resolves to install root");
    check(InstallRootFromExecutable(L"C:\\Program Files\\Pulse\\pulse.exe") == L"C:\\Program Files\\Pulse",
        "flat install executable resolves to its own directory");
    check(InstallRootFromExecutable(L"C:\\Program Files\\Pulse\\AppRuntime\\short\\pulse.exe") == L"C:\\Program Files\\Pulse\\AppRuntime\\short",
        "non-digest AppRuntime child does not climb to parent");
    // Same-IL >= Medium integrity gate (supports both standard Medium and UAC-off/elevated High sessions).
    check(EligibleIntegrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID), "Medium->Medium integrity accepted");
    check(EligibleIntegrity(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_HIGH_RID), "High->High integrity accepted for elevated sessions");
    check(!EligibleIntegrity(SECURITY_MANDATORY_LOW_RID, SECURITY_MANDATORY_LOW_RID), "Low integrity rejected");
    check(!EligibleIntegrity(SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID) &&
          !EligibleIntegrity(SECURITY_MANDATORY_HIGH_RID, SECURITY_MANDATORY_MEDIUM_RID), "cross-integrity mismatch rejected");
    // Non-whitelist general candidate filtering.
    const std::wstring win_dir = L"C:\\Windows";
    const std::wstring install_dir = L"C:\\Program Files\\Pulse";
    check(GeneralRevealCandidateImage(L"C:\\Program Files (x86)\\XZDesktop\\XZDesktop64.exe", win_dir, install_dir),
        "non-whitelisted desktop organizer accepted without hardcoded list");
    check(GeneralRevealCandidateImage(L"D:\\Tools\\AnyLauncher\\Launcher.exe", win_dir, install_dir),
        "arbitrary non-excluded local app accepted");
    check(!GeneralRevealCandidateImage(L"C:\\Windows\\explorer.exe", win_dir, install_dir) &&
          !GeneralRevealCandidateImage(L"C:\\Program Files\\Pulse\\pulse.exe", win_dir, install_dir) &&
          !GeneralRevealCandidateImage(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", win_dir, install_dir),
        "system, Pulse, and excluded browser images stay excluded");
    // Learned targets.
    wchar_t windows[MAX_PATH]{}; GetWindowsDirectoryW(windows, MAX_PATH);
    check(!targets::Learnable(std::wstring(windows) + L"\\System32\\notepad.exe"), "system image is never learned");
    check(!targets::Learnable(L"C:\\Program Files\\Pulse\\pulse.exe") && !targets::Learnable(L"D:\\x\\explorer.exe"),
        "Pulse and Explorer images are never learned");
    check(!targets::Learnable(L"\\\\server\\share\\tool.exe") && !targets::Learnable(L"C:\\Tools\\..\\tool.exe") &&
          !targets::Learnable(L"C:\\Tools\\readme.txt"), "UNC, traversal and non-executable rejected");
    check(!AllowedImage(L"D:\\Tools\\ShareX\\ShareX.exe"), "unlearned application is not a target");
    check(targets::Learn(L"D:\\Tools\\ShareX\\ShareX.exe"), "launching application learned");
    check(!targets::Learn(L"E:\\Other\\sharex.exe"), "same image name is learned once");
    check(!targets::Learn(L"C:\\Program Files\\Alma\\Alma.exe"), "built-in target is not duplicated in the learned list");
    check(AllowedImage(L"D:\\Tools\\ShareX\\ShareX.exe") && AllowedImage(L"C:\\Elsewhere\\sharex.EXE"),
        "learned name becomes a target in any local location");
    targets::SetStorePathForTest(store);   // forget memory, reload from disk
    const auto learned = targets::LearnedPaths();
    check(learned.size() == 1 && learned.front() == L"D:\\Tools\\ShareX\\ShareX.exe", "learned list persists across reload");
    DeleteFileW(store.c_str());
    targets::SetStorePathForTest(store);
    check(!AllowedImage(L"D:\\Tools\\ShareX\\ShareX.exe"), "removing the store forgets learned targets");
    // Launcher origin.
    check(origin::SelectTarget(L"\"C:\\WINDOWS\\explorer.exe\" /select,\"C:\\a b\\c, d.txt\"") == L"C:\\a b\\c, d.txt",
        "quoted /select target parsed");
    check(origin::SelectTarget(L"explorer.exe /SELECT, C:\\x\\y.txt ") == L"C:\\x\\y.txt", "unquoted /select target parsed");
    check(origin::SelectTarget(L"explorer.exe C:\\x").empty() && origin::SelectTarget(L"explorer.exe /select,\"C:\\x").empty(),
        "no or malformed /select yields nothing");
    const ULONGLONG now = GetTickCount64();
    origin::RecordForTest(L"C:\\Docs\\a.txt", L"D:\\Apps\\Caller.exe", now);
    check(origin::FindLauncherParent(L"c:/docs/A.TXT", 15000) == L"D:\\Apps\\Caller.exe", "origin found by selected path");
    check(origin::FindLauncherParent(L"C:\\Docs\\b.txt", 15000).empty(), "different selection has no origin");
    origin::RecordForTest(L"C:\\Docs\\old.txt", L"D:\\Apps\\Old.exe", now - 20000);
    check(origin::FindLauncherParent(L"C:\\Docs\\old.txt", 15000).empty(), "stale observation ignored");
    origin::RecordForTest(L"C:\\Docs\\a.txt", L"D:\\Apps\\Other.exe", now);
    check(origin::FindLauncherParent(L"C:\\Docs\\a.txt", 15000).empty(), "two applications asking for one path is ambiguous");
    {
        // Real inspection of an ordinary child carrying a /select command line
        // (no renamed images: the name filter is Observe's alone).
        wchar_t image[32768]{}; GetModuleFileNameW(nullptr, image, 32768);
        std::wstring command = L"\"" + std::wstring(image) + L"\" --child /select,\"C:\\Docs\\live.txt\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION child{};
        const BOOL started = CreateProcessW(image, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child);
        check(started != FALSE, "launcher-shaped child started");
        if (started) {
            origin::Inspect(child.dwProcessId, GetCurrentProcessId());
            const auto parent = origin::FindLauncherParent(L"C:\\Docs\\live.txt", 15000);
            check(_wcsicmp(parent.c_str(), image) == 0, "live launcher inspection names its parent application");
            TerminateProcess(child.hProcess, 0); WaitForSingleObject(child.hProcess, 5000);
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
        }
    }
    if (argc != 2 && !(argc == 3 && wcscmp(argv[2], L"--production-dll") == 0)) {
        std::puts("[FAIL] fixture DLL absolute path required"); return 1;
    }
    const DWORD expected_result = argc == 3 ? ERROR_SUCCESS : 1234;
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child";
    std::wstring child_image = self;
    if (argc == 3) {
        // Workspace executables can carry a low-integrity label. Exercise the
        // production token gate with a fresh, OS-hosted, noninteractive sleeper.
        wchar_t system[MAX_PATH]{};
        if (!GetSystemDirectoryW(system, MAX_PATH)) return 1;
        child_image = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        command = L"\"" + child_image + L"\" -NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 30\"";
    }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(child_image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    check(created != FALSE, "owned child fixture starts");
    if (created) {
        HANDLE token = nullptr;
        if (OpenProcessToken(process.hProcess, TOKEN_QUERY, &token)) {
            DWORD size = 0; TOKEN_ELEVATION elevation{};
            GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
            SetLastError(0); const BOOL restricted = IsTokenRestricted(token);
            std::printf("fixture token elevated=%lu restricted=%d query_error=%lu\n",
                elevation.TokenIsElevated, restricted, GetLastError());
            alignas(TOKEN_MANDATORY_LABEL) BYTE label[1024]{};
            if (GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(label), &size)) {
                const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
                std::printf("fixture integrity=%lu\n", *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
            }
            CloseHandle(token);
        }
        const DWORD result = AttachFixture(process.dwProcessId, argv[1]);
        std::printf("fixture pid=%lu export result=%lu\n", process.dwProcessId, result);
        Sleep(1500); // Flush asynchronous diagnostics before ending the owned child.
        check(result == expected_result, "remote LoadLibrary plus explicit export returned expected status");
        const auto directory = (std::filesystem::path(self).parent_path() /
            (L"pulse-reveal-generation-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()))).wstring();
        const bool made = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
        if (!made) std::printf("fixture directory error=%lu\n", GetLastError());
        check(made, "isolated alternate-generation fixture directory");
        if (made) {
            const auto alternate = (std::filesystem::path(directory) / std::filesystem::path(argv[1]).filename()).wstring();
            const bool copied = CopyFileW(argv[1], alternate.c_str(), TRUE) != FALSE;
            check(copied, "alternate-generation fixture copy");
            if (copied) {
                const DWORD collision = AttachFixture(process.dwProcessId, alternate.c_str());
                check(collision == ERROR_REVISION_MISMATCH, "already loaded different-generation DLL is rejected without unload");
                DeleteFileW(alternate.c_str());
            }
            RemoveDirectoryW(directory.c_str());
        }
        // Only this test-created child is ended; production never terminates targets.
        TerminateProcess(process.hProcess, 0); WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
    }
    return failures ? 1 : 0;
}
