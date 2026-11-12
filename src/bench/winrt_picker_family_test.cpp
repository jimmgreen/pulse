#include <windows.h>
#include <shobjidl_core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cstdio>
#include <string>
#include <string_view>

namespace {
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::FileIO;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::Pickers::FileOpenPicker;

template<class Async> AsyncStatus Pump(const Async& operation, DWORD timeout_ms, DWORD cancel_after_ms = 0) {
    const ULONGLONG begin = GetTickCount64();
    bool cancelled = false;
    while (operation.Status() == AsyncStatus::Started) {
        const auto elapsed = GetTickCount64() - begin;
        if (cancel_after_ms && !cancelled && elapsed >= cancel_after_ms) {
            operation.Cancel(); cancelled = true;
            std::printf("INFO cancel_requested=1\n"); std::fflush(stdout);
        }
        if (elapsed >= timeout_ms) {
            operation.Cancel();
            winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    return operation.Status();
}

bool TokenInfo() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{}; DWORD count = 0, container = 0;
    const bool okay = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &count) &&
        GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &count);
    CloseHandle(token);
    if (okay) std::printf("INFO pid=%lu process_arch=%zu elevated=%u appcontainer=%lu\n", GetCurrentProcessId(), sizeof(void*) * 8,
        elevation.TokenIsElevated ? 1u : 0u, static_cast<unsigned long>(container));
    return okay;
}

struct Owner {
    HWND window = CreateWindowExW(0, L"STATIC", L"Pulse WinRT picker family probe", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Owner() { if (window) DestroyWindow(window); }
};

bool CancelPicker() {
    Owner owner;
    if (!owner.window) winrt::throw_last_error();
    FileOpenPicker picker;
    picker.FileTypeFilter().Append(L".txt");
    winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(owner.window));
    ShowWindow(owner.window, SW_SHOWNORMAL);
    const auto operation = picker.PickSingleFileAsync();
    std::printf("INFO family=WINRT api=PickSingleFileAsync expected_calls=1 ui_shown=unknown\n");
    std::fflush(stdout);
    const auto status = Pump(operation, 15000, 750);
    std::printf("INFO async_status=%u async_error_hr=0x%08lX\n", static_cast<unsigned>(status), static_cast<unsigned long>(operation.ErrorCode().value));
    if (status == AsyncStatus::Canceled) {
        std::printf("[PASS] WinRT cancellation status=Canceled result_present=0 host_consumption_verified=0\n");
        return true;
    }
    const auto file = operation.GetResults();
    const bool okay = status == AsyncStatus::Completed && !file;
    std::printf("[%s] WinRT cancellation status=%u result_present=%u host_consumption_verified=0\n",
        okay ? "PASS" : "FAIL", static_cast<unsigned>(status), file ? 1u : 0u);
    return okay;
}

bool ReadStorageFixture(const wchar_t* base) {
    const std::wstring directory = std::wstring(base) + L"\\winrt-picker-" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(directory.c_str(), nullptr)) winrt::throw_last_error();
    struct Cleanup {
        std::wstring directory, file;
        ~Cleanup() { if (!file.empty()) DeleteFileW(file.c_str()); RemoveDirectoryW(directory.c_str()); }
    } cleanup{directory, directory + L"\\owned.txt"};
    const HANDLE fixture = CreateFileW(cleanup.file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fixture == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    constexpr char contents[] = "pulse-winrt-owned-fixture";
    DWORD written = 0;
    const bool created = WriteFile(fixture, contents, sizeof(contents) - 1, &written, nullptr) && written == sizeof(contents) - 1;
    CloseHandle(fixture);
    if (!created) winrt::throw_hresult(E_FAIL);
    auto operation = StorageFile::GetFileFromPathAsync(cleanup.file);
    Pump(operation, 10000);
    auto file = operation.GetResults();
    auto read = FileIO::ReadTextAsync(file);
    Pump(read, 10000);
    const bool okay = read.GetResults() == L"pulse-winrt-owned-fixture";
    file = nullptr;
    std::printf("[%s] StorageFile owned_fixture_read=%u picker_grant_verified=0 appcontainer_grant_verified=0\n", okay ? "PASS" : "FAIL", okay ? 1u : 0u);
    if (!DeleteFileW(cleanup.file.c_str())) winrt::throw_last_error();
    bool missing_rejected = false;
    try {
        auto missing = StorageFile::GetFileFromPathAsync(cleanup.file);
        Pump(missing, 10000);
        (void)missing.GetResults();
    } catch (const winrt::hresult_error& error) {
        missing_rejected = error.code() == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || error.code() == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        std::printf("INFO missing_fixture_hr=0x%08lX\n", static_cast<unsigned long>(error.code().value));
    }
    std::printf("[%s] deleted StorageFile cannot become stale success\n", missing_rejected ? "PASS" : "FAIL");
    return okay && missing_rejected;
}
}

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (!TokenInfo()) winrt::throw_last_error();
        bool passed = false;
        if (argc == 2 && std::wstring_view(argv[1]) == L"--cancel") passed = CancelPicker();
        else if (argc == 3 && std::wstring_view(argv[1]) == L"--storage-fixture") passed = ReadStorageFixture(argv[2]);
        else { std::printf("Usage: --cancel | --storage-fixture EXISTING_OWNED_DIRECTORY\n"); return 2; }
        std::printf("RESULT family=WINRT pulse_adapter=not_implemented reason=WINRT_STORAGE_GRANT_NOT_IMPLEMENTED test_passed=%u\n", passed ? 1u : 0u);
        return passed ? 0 : 1;
    } catch (const winrt::hresult_error& error) {
        std::printf("[FAIL] WinRT family probe hr=0x%08lX host_consumption_verified=0\n", static_cast<unsigned long>(error.code().value));
        return 1;
    } catch (...) {
        std::printf("[FAIL] WinRT family probe exception host_consumption_verified=0\n");
        return 1;
    }
}
