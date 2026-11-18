#include "../common/app_identity.h"
#include "../common/string_ids.h"
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    int failed = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        failed += !passed;
    };
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    check(SUCCEEDED(pulse::app_identity::SetProcess()), "process accepts stable application identity");
    PWSTR id = nullptr;
    check(SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id)) && id &&
        std::wstring(id) == pulse::app_identity::kId, "process identity matches the launcher and shortcut constant");
    CoTaskMemFree(id);
    const std::wstring stable = L"C:\\Program Files\\Pulse\\pulse.exe";
    for (int i = 0; i < 2; ++i) {
        HWND window = CreateWindowExW(0, L"STATIC", L"Pulse identity fixture", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        check(window && SUCCEEDED(pulse::app_identity::SetWindow(window, stable)),
            "main and additional hidden windows accept the same taskbar identity");
        IPropertyStore* properties = nullptr;
        if (!window || FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties)))) {
            if (window) DestroyWindow(window);
            ++failed; continue;
        }
        auto equals = [&](REFPROPERTYKEY key, const std::wstring& expected) {
            PROPVARIANT value{};
            const HRESULT result = properties->GetValue(key, &value);
            const bool same = SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal && expected == value.pwszVal;
            PropVariantClear(&value);
            return same;
        };
        check(equals(PKEY_AppUserModel_ID, pulse::app_identity::kId), "window uses version-independent app identity");
        check(equals(PKEY_AppUserModel_RelaunchCommand, L"\"" + stable + L"\" --activate"),
            "pin relaunches the quoted stable launcher without creating an explicit new window");
        check(equals(PKEY_AppUserModel_RelaunchIconResource, stable + L",0"), "pinned icon uses stable launcher resource");
        check(equals(PKEY_AppUserModel_RelaunchDisplayNameResource,
            L"@" + stable + L",-" + std::to_wstring(IDS_INTEGRATION_HANDLER_PULSE)), "pin name uses catalog resource");
        pulse::app_identity::ClearWindow(window);
        PROPVARIANT cleared{};
        check(SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &cleared)) && cleared.vt == VT_EMPTY,
            "window properties are released before destruction");
        PropVariantClear(&cleared);
        properties->Release();
        DestroyWindow(window);
    }
    if (argc == 2) {
        const std::wstring resource = std::wstring(L"@") + argv[1] + L",-" + std::to_wstring(IDS_INTEGRATION_HANDLER_PULSE);
        wchar_t title[128]{};
        check(SUCCEEDED(SHLoadIndirectString(resource.c_str(), title, ARRAYSIZE(title), nullptr)) &&
            std::wstring(title) == L"Pulse", "built launcher supplies the relaunch display-name resource");
    }
    CoUninitialize();
    return failed ? 1 : 0;
}
