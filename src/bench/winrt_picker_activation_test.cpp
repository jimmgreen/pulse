#include "../shell_bridge/winrt_picker_activation.h"
#include <shobjidl.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <cstdio>
#include <atomic>
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
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; };
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        const HWND owner = CreateWindowExW(0, L"STATIC", L"Owned WinRT activation test", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        struct Cleanup { HWND hwnd; ~Cleanup() { DestroyWindow(hwnd); } } cleanup{owner};
        std::atomic<unsigned> calls{0};
        api::Services bridge{[&](const wire::Message& request, HWND hwnd, HANDLE cancel) {
            if (hwnd != owner || !cancel) winrt::throw_hresult(E_FAIL);
            ++calls;
            wire::Response response; response.operation = std::get<wire::Request>(request.body).operation;
            response.status = wire::Status::Cancelled;
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
        auto folder = factory(L"Windows.Storage.Pickers.FolderPicker").ActivateInstance<pick::FolderPicker>();
        folder.FileTypeFilter().Append(L"*"); folder.SuggestedStartLocation(pick::PickerLocationId::Desktop);
        winrt::check_hresult(folder.as<IInitializeWithWindow>()->Initialize(owner));
        check(!Wait(folder.PickSingleFolderAsync()) && calls == 3, "native FolderPicker activation routes async cancel");
        printf("INFO stage=save-activate\n");
        auto save = factory(L"Windows.Storage.Pickers.FileSavePicker").ActivateInstance<pick::FileSavePicker>();
        printf("INFO stage=save-choices\n");
        save.FileTypeChoices().Insert(L"Text", winrt::single_threaded_vector<winrt::hstring>({L".txt"}));
        printf("INFO stage=save-location\n");
        save.SuggestedStartLocation(pick::PickerLocationId::Desktop);
        printf("INFO stage=save-initialize\n");
        winrt::check_hresult(save.as<IInitializeWithWindow>()->Initialize(owner));
        printf("INFO save-settings=%u\n", save.SettingsIdentifier().size());
        printf("INFO save-commit=%u\n", save.CommitButtonText().size());
        printf("INFO save-choices=%u\n", save.FileTypeChoices().Size());
        printf("INFO save-filename=%u\n", save.SuggestedFileName().size());
        printf("INFO save-extension=%u\n", save.DefaultFileExtension().size());
        printf("INFO stage=save-pick\n");
        check(!Wait(save.PickSaveFileAsync()) && calls == 4, "native FileSavePicker activation routes async cancel");
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
