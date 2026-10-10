// folder_open_com_test.cpp — drives the folder-open delegate through real COM
// activation.
//
// This covers the half of the feature that cannot be checked by reading the
// registry: that the CLSID registered in Folder\shell\open\command\DelegateExecute
// actually constructs, that the interface set the shell was measured to use is
// exposed, and that what the shell passes (folder / verb / empty property bag)
// becomes a queued request for the window.
//
// It registers a private sandbox CLSID rather than the shipped one, so it never
// collides with an installed Pulse.

#include "../app/folder_open_com.h"

#include <windows.h>
#include <shobjidl.h>
#include <objidl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

// {2B7C4E19-58A3-4F6D-9E11-7C4A2D6B8F30}
const CLSID sandbox_class{0x2b7c4e19, 0x58a3, 0x4f6d, {0x9e, 0x11, 0x7c, 0x4a, 0x2d, 0x6b, 0x8f, 0x30}};

const wchar_t* kFixtureDir = L"C:\\pulse\\diag\\__folder_open_com_fixture__";

std::wstring RequestText(const pulse::app::folder_open::Request& request) {
    return request.folder + L" | " + request.target + L" | " + request.verb;
}

} // namespace

int wmain() {
    using namespace pulse::app::folder_open;

    CreateDirectoryW(kFixtureDir, nullptr);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::printf("FAIL: CoInitializeEx\n");
        return 2;
    }

    Check(SUCCEEDED(RegisterCommandServer(&sandbox_class)), "delegate: sandbox class registers");
    Check(CommandServerBusy() == false, "delegate: idle server reports not busy");

    // --- the shell's measured call sequence, through real activation ---
    IExecuteCommand* executable = nullptr;
    const HRESULT activated = CoCreateInstance(sandbox_class, nullptr, CLSCTX_LOCAL_SERVER,
                                               IID_IExecuteCommand, reinterpret_cast<void**>(&executable));
    Check(SUCCEEDED(activated) && executable != nullptr,
          "delegate: CoCreateInstance(IExecuteCommand) succeeds");

    IObjectWithSelection* selection = nullptr;
    IInitializeCommand* initialize = nullptr;
    if (executable) {
        executable->QueryInterface(IID_IObjectWithSelection, reinterpret_cast<void**>(&selection));
        executable->QueryInterface(IID_IInitializeCommand, reinterpret_cast<void**>(&initialize));
    }
    Check(selection != nullptr, "delegate: exposes IObjectWithSelection (the shell queries it)");
    Check(initialize != nullptr, "delegate: exposes IInitializeCommand (the shell queries it)");

    if (executable) {
        Check(SUCCEEDED(executable->SetShowWindow(1)), "delegate: SetShowWindow accepted");
        Check(SUCCEEDED(executable->SetNoShowUI(FALSE)), "delegate: SetNoShowUI accepted");
        Check(SUCCEEDED(executable->SetKeyState(0)), "delegate: SetKeyState accepted");
        Check(SUCCEEDED(executable->SetPosition(POINT{0, 0})), "delegate: SetPosition accepted");
        // The shell leaves the property bag empty and names the verb.
        if (initialize) Check(SUCCEEDED(initialize->Initialize(L"open", nullptr)),
                              "delegate: Initialize(open, null bag) accepted");
        // Measured: the shell passes the folder in SetDirectory.
        Check(SUCCEEDED(executable->SetDirectory(kFixtureDir)),
              "delegate: SetDirectory accepts the folder");
    }

    Check(TakeRequests().empty(), "delegate: nothing is queued before Execute");

    if (executable) {
        Check(SUCCEEDED(executable->Execute()), "delegate: Execute succeeds");
        // A second Execute must not produce a second request.
        Check(SUCCEEDED(executable->Execute()), "delegate: repeated Execute is ignored");
    }

    const auto requests = TakeRequests();
    Check(requests.size() == 1, "delegate: exactly one request is queued");
    if (requests.size() == 1) {
        std::wprintf(L"    queued: %ls\n", RequestText(requests.front()).c_str());
        Check(_wcsicmp(requests.front().folder.c_str(), kFixtureDir) == 0,
              "delegate: the queued folder is the one SetDirectory named");
        Check(requests.front().verb == L"open", "delegate: the verb is carried through");
        Check(requests.front().target.empty(),
              "delegate: no target, because the shell never delivers the file");
    }
    Check(TakeRequests().empty(), "delegate: draining the queue empties it");

    // --- a delegate that never learned a folder must not queue anything ---
    IExecuteCommand* bare = nullptr;
    CoCreateInstance(sandbox_class, nullptr, CLSCTX_LOCAL_SERVER, IID_IExecuteCommand,
                     reinterpret_cast<void**>(&bare));
    if (bare) {
        const HRESULT empty = bare->Execute();
        Check(FAILED(empty), "delegate: Execute without a folder is refused");
        bare->Release();
    }
    Check(TakeRequests().empty(), "delegate: the refused call queued nothing");

    if (initialize) initialize->Release();
    if (selection) selection->Release();
    if (executable) executable->Release();

    // --- the newest-entry fallback ---
    // The shell never delivers the file, so the window resolves the target from
    // the folder. Timestamps are set explicitly so this never depends on
    // filesystem write-order resolution.
    {
        const std::wstring older = std::wstring(kFixtureDir) + L"\\older.txt";
        const std::wstring newer = std::wstring(kFixtureDir) + L"\\newer.txt";
        const std::wstring hidden = std::wstring(kFixtureDir) + L"\\hidden.txt";
        for (const auto* path : {older.c_str(), newer.c_str(), hidden.c_str()}) {
            HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        }
        SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN);

        const auto stamp = [](const std::wstring& path, int year) {
            HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            SYSTEMTIME time{};
            time.wYear = static_cast<WORD>(year); time.wMonth = 1; time.wDay = 1;
            FILETIME local{};
            SystemTimeToFileTime(&time, &local);
            FILETIME utc{};
            LocalFileTimeToFileTime(&local, &utc);
            SetFileTime(file, nullptr, nullptr, &utc);
            CloseHandle(file);
        };
        stamp(older, 2020);
        stamp(newer, 2024);
        stamp(hidden, 2026); // newer than everything, but hidden

        const std::wstring newest = NewestEntryPath(kFixtureDir);
        std::wprintf(L"    newest: %ls\n", newest.c_str());
        Check(newest.find(L"newer.txt") != std::wstring::npos,
              "delegate: the newest visible entry is the fallback target");
        Check(newest.find(L"hidden.txt") == std::wstring::npos,
              "delegate: a hidden entry is never chosen as the fallback target");
        Check(NewestEntryPath(L"").empty(), "delegate: an empty folder argument yields no target");
        Check(NewestEntryPath(L"C:\\pulse\\diag\\__no_such_folder__").empty(),
              "delegate: a missing folder yields no target");

        for (const auto* path : {older.c_str(), newer.c_str()}) DeleteFileW(path);
        SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(hidden.c_str());
    }

    // --- revocation releases the class object ---
    RevokeCommandServer();
    IExecuteCommand* after_revoke = nullptr;
    const HRESULT revoked = CoCreateInstance(sandbox_class, nullptr, CLSCTX_LOCAL_SERVER,
                                             IID_IExecuteCommand, reinterpret_cast<void**>(&after_revoke));
    if (after_revoke) after_revoke->Release();
    Check(FAILED(revoked), "delegate: the class is gone after revoke");

    CoUninitialize();
    RemoveDirectoryW(kFixtureDir);

    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
