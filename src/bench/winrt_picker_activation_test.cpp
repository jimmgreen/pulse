#include "../shell_bridge/winrt_picker_activation.h"
#include <shobjidl.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <cstdio>
#include <atomic>
#include <filesystem>
#include <string>
namespace api = pulse::shell_bridge::winrt_picker;
namespace wire = pulse::ipc::picker;
namespace pick = winrt::Windows::Storage::Pickers;
using winrt::Windows::Foundation::AsyncStatus;
template<class Operation> auto Wait(const Operation& operation) {
    const auto start = GetTickCount64();
    while (operation.Status() == AsyncStatus::Started) {
        if (GetTickCount64() - start > 10000) { operation.Cancel(); winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return operation.GetResults();
}
static uint64_t TestHash(std::wstring_view text) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t ch : text) {
        const uint16_t unit = static_cast<uint16_t>(ch);
        hash = (hash ^ (unit & 0xffu)) * 1099511628211ull;
        hash = (hash ^ (unit >> 8)) * 1099511628211ull;
    }
    return hash;
}
static std::wstring TestSettingsRegValue(std::wstring_view settings_id) {
    wchar_t module_path[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
    std::wstring_view exe(module_path, len && len < ARRAYSIZE(module_path) ? len : 0);
    const auto slash = exe.find_last_of(L"\\/");
    if (slash != exe.npos) exe.remove_prefix(slash + 1);
    std::wstring lowered(exe);
    for (auto& ch : lowered) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    wchar_t name[64]{};
    std::swprintf(name, ARRAYSIZE(name), L"%016llx-%016llx",
        static_cast<unsigned long long>(TestHash(lowered)),
        static_cast<unsigned long long>(TestHash(settings_id)));
    return name;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    const auto temp_dir = std::filesystem::temp_directory_path() / (L"pulse-winrt-act-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code fs_ec;
    std::filesystem::create_directories(temp_dir, fs_ec);
    const std::wstring settings_id = L"pulse-test-settings-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring reg_value_name = TestSettingsRegValue(settings_id);
    struct TempCleanup {
        std::filesystem::path dir;
        std::wstring reg_val;
        ~TempCleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Pulse\\WinRtPickerState", 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
                RegDeleteValueW(key, reg_val.c_str());
                RegCloseKey(key);
            }
        }
    } temp_cleanup{temp_dir, reg_value_name};
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        const HWND owner = CreateWindowExW(0, L"STATIC", L"Owned WinRT activation test", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        struct Cleanup { HWND hwnd; ~Cleanup() { DestroyWindow(hwnd); } } cleanup{owner};
        std::atomic<unsigned> calls{0};
        std::u16string last_initial_dir;
        bool accept_temp_folder = false;
        api::Services bridge{[&](const wire::Message& request, HWND hwnd, HANDLE cancel) {
            if (hwnd != owner || !cancel) winrt::throw_hresult(E_FAIL);
            ++calls;
            const auto& body = std::get<wire::Request>(request.body);
            last_initial_dir = body.initial_directory;
            wire::Response response; response.operation = body.operation;
            if (accept_temp_folder && body.operation == wire::Operation::PickFolder) {
                response.status = wire::Status::Accepted;
                response.paths.push_back(temp_dir.u16string());
            } else {
                response.status = wire::Status::Cancelled;
            }
            return wire::Message{request.request_id, response};
        }};
        auto factory = [&](const wchar_t* name) {
            const winrt::hstring type(name);
            winrt::Windows::Foundation::IActivationFactory result{nullptr};
            winrt::check_hresult(api::GetActivationFactory(static_cast<HSTRING>(winrt::get_abi(type)),
                winrt::guid_of<winrt::Windows::Foundation::IActivationFactory>(), winrt::put_abi(result), bridge));
            return result;
        };
        auto open = factory(L"Windows.Storage.Pickers.FileOpenPicker").ActivateInstance<pick::FileOpenPicker>();
        open.FileTypeFilter().Append(L".txt");
        open.SuggestedStartLocation(pick::PickerLocationId::Desktop);
        winrt::check_hresult(open.as<IInitializeWithWindow>()->Initialize(owner));
        check(open.as<pick::IFileOpenPicker2>().as<IUnknown>() == open.as<IInitializeWithWindow>().as<IUnknown>(), "public interfaces preserve controlling IUnknown identity");
        check(!Wait(open.PickSingleFileAsync()) && calls == 1, "native FileOpenPicker activation routes async single cancel through adapter");
        check(Wait(open.PickMultipleFilesAsync()).Size() == 0 && calls == 2, "native FileOpenPicker multi cancel returns empty collection");
        open.SuggestedStartLocation(pick::PickerLocationId::ComputerFolder);
        check(!Wait(open.PickSingleFileAsync()) && calls == 3 && last_initial_dir == u"pulse:computer",
            "ComputerFolder maps to pulse:computer on Open");
        open.SuggestedStartLocation(pick::PickerLocationId::Objects3D);
        check(!Wait(open.PickSingleFileAsync()) && calls == 4 && !last_initial_dir.empty(),
            "Objects3D start location routes through Pulse");
        auto folder = factory(L"Windows.Storage.Pickers.FolderPicker").ActivateInstance<pick::FolderPicker>();
        folder.FileTypeFilter().Append(L"*"); folder.SuggestedStartLocation(pick::PickerLocationId::ComputerFolder);
        winrt::check_hresult(folder.as<IInitializeWithWindow>()->Initialize(owner));
        check(!Wait(folder.PickSingleFolderAsync()) && calls == 5 && last_initial_dir == u"pulse:computer",
            "native FolderPicker ComputerFolder routes through Pulse");
        folder.SuggestedStartLocation(pick::PickerLocationId::Desktop);
        folder.SettingsIdentifier(settings_id);
        accept_temp_folder = true;
        auto picked = Wait(folder.PickSingleFolderAsync());
        accept_temp_folder = false;
        check(static_cast<bool>(picked) && calls == 6,
            "FolderPicker with SettingsIdentifier routes through Pulse and accepts folder");
        last_initial_dir.clear();
        check(!Wait(folder.PickSingleFolderAsync()) && calls == 7 && last_initial_dir == temp_dir.u16string(),
            "SettingsIdentifier remembers previous accepted directory across calls");
        printf("INFO stage=save-activate\n");
        auto save = factory(L"Windows.Storage.Pickers.FileSavePicker").ActivateInstance<pick::FileSavePicker>();
        printf("INFO stage=save-choices\n");
        save.FileTypeChoices().Insert(L"Text", winrt::single_threaded_vector<winrt::hstring>({L".txt"}));
        printf("INFO stage=save-location\n");
        save.SuggestedStartLocation(pick::PickerLocationId::ComputerFolder);
        printf("INFO stage=save-initialize\n");
        winrt::check_hresult(save.as<IInitializeWithWindow>()->Initialize(owner));
        printf("INFO save-settings=%u\n", save.SettingsIdentifier().size());
        printf("INFO save-commit=%u\n", save.CommitButtonText().size());
        printf("INFO save-choices=%u\n", save.FileTypeChoices().Size());
        printf("INFO save-filename=%u\n", save.SuggestedFileName().size());
        printf("INFO save-extension=%u\n", save.DefaultFileExtension().size());
        printf("INFO stage=save-pick\n");
        check(!Wait(save.PickSaveFileAsync()) && calls == 8 && last_initial_dir != u"pulse:computer" && !last_initial_dir.empty(),
            "native FileSavePicker ComputerFolder maps to physical directory and routes through Pulse");
        const winrt::hstring type(L"Windows.Storage.Pickers.FileOpenPicker");
        pick::IFileOpenPickerStatics statics{nullptr};
        winrt::check_hresult(api::GetActivationFactory(static_cast<HSTRING>(winrt::get_abi(type)), winrt::guid_of<pick::IFileOpenPickerStatics>(), winrt::put_abi(statics), bridge));
        check(static_cast<bool>(statics), "non-default static factory interface remains native");
        api::Services waiting{[](const wire::Message& request, HWND, HANDLE cancel) {
            if (WaitForSingleObject(cancel, 5000) != WAIT_OBJECT_0) winrt::throw_hresult(E_FAIL);
            wire::Response response; response.operation = std::get<wire::Request>(request.body).operation; response.status = wire::Status::Cancelled;
            return wire::Message{request.request_id, response};
        }};
        auto pending = api::PickFileAsync(wire::Request{}, owner, waiting);
        pending.Cancel();
        bool canceled = false; try { (void)Wait(pending); } catch (const winrt::hresult_error&) { canceled = true; }
        check(canceled && pending.Status() == AsyncStatus::Canceled, "IAsyncInfo Cancel reaches canceled state");
    } catch (const winrt::hresult_error& e) { printf("[FAIL] activation hr=0x%08X\n", static_cast<unsigned>(e.code().value)); ++failures; }
    printf("failures=%d storage_grant_verified=0\n", failures);
    return failures ? 1 : 0;
}
