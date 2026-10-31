#include "../common/picker_user_storage.h"
#include "../picker_inprocess/picker_ui_api_v2.h"
#include <cstdio>
#include <filesystem>
#include <objbase.h>
#include <vector>
using namespace pulse::picker_user_storage;
int wmain(int argc, wchar_t** argv) {
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* name) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); };
    Path path;
    check(Parse(L"\\\\?\\UNC\\server\\share\\dir\\file.txt", path) && path.unc && path.root == L"\\\\server\\share\\", "extended UNC has share root");
    std::vector<std::wstring> observed;
    unsigned drive_calls = 0;
    auto drive = [&](const std::wstring&) { ++drive_calls; return UINT{DRIVE_REMOVABLE}; };
    auto attributes = [&](const std::wstring& current, DWORD& flags) {
        observed.push_back(current); flags = current == path.value ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY; return DWORD{ERROR_SUCCESS};
    };
    check(CheckAncestors(path, true, drive, attributes, [] { return false; }) == ERROR_SUCCESS && drive_calls == 0,
        "UNC uses worker metadata rather than local drive classification");
    check(observed == std::vector<std::wstring>{L"\\\\server\\share", L"\\\\server\\share\\dir", L"\\\\server\\share\\dir\\file.txt"}, "UNC checks share and each ancestor once");
    for (const auto* invalid : {L"relative", L"C:relative", L"\\\\.\\pipe\\x", L"\\\\?\\GLOBALROOT\\Device\\x", L"\\\\server", L"\\\\server\\", L"\\\\server\\share\\..\\file", L"C:\\file:stream", L"C:\\a\\\\b"})
        check(!Parse(invalid, path), "device relative ADS and ambiguous paths rejected");
    check(Parse(L"\\\\?\\E:\\dir\\file.txt", path) && path.root == L"E:\\", "extended drive path normalized");
    for (UINT kind : {UINT{DRIVE_FIXED}, UINT{DRIVE_REMOVABLE}, UINT{DRIVE_REMOTE}})
        check(CheckAncestors(path, true, [=](const std::wstring&) { return kind; }, attributes, [] { return false; }) == ERROR_SUCCESS, "fixed removable mapped-network ordinary objects admitted");
    for (UINT kind : {UINT{DRIVE_CDROM}, UINT{DRIVE_RAMDISK}})
        check(CheckAncestors(path, true, [=](const std::wstring&) { return kind; }, attributes, [] { return false; }) == ERROR_NOT_SUPPORTED, "unimplemented storage classes remain unsupported");
    check(CheckAncestors(path, true, [](const std::wstring&) { return UINT{DRIVE_NO_ROOT_DIR}; }, attributes, [] { return false; }) == ERROR_PATH_NOT_FOUND, "removed drive root is unavailable");
    for (DWORD error : {DWORD{ERROR_ACCESS_DENIED}, DWORD{ERROR_NOT_READY}, DWORD{ERROR_BAD_NETPATH}, DWORD{ERROR_NETWORK_UNREACHABLE}})
        check(CheckAncestors(path, true, drive, [=](const std::wstring&, DWORD&) { return error; }, [] { return false; }) == error, "permission and disconnected-media errors preserved");
    for (DWORD flags : {DWORD{FILE_ATTRIBUTE_REPARSE_POINT}, DWORD{FILE_ATTRIBUTE_OFFLINE}, DWORD{0x00040000u}, DWORD{0x00400000u}})
        check(CheckAncestors(path, true, drive, [=](const std::wstring&, DWORD& result) { result = FILE_ATTRIBUTE_DIRECTORY | flags; return DWORD{ERROR_SUCCESS}; }, [] { return false; }) == ERROR_NOT_SUPPORTED, "links and cloud hydration states remain unsupported");
    observed.clear();
    check(CheckAncestors(path, true, drive, attributes, [] { return true; }) == ERROR_CANCELLED && observed.empty(), "cancel before callbacks");
    bool cancelled = false;
    check(CheckAncestors(path, true, drive, [&](const std::wstring&, DWORD& flags) { flags = FILE_ATTRIBUTE_DIRECTORY; cancelled = true; return DWORD{ERROR_ACCESS_DENIED}; }, [&] { return cancelled; }) == ERROR_CANCELLED, "stale generation wins after blocking metadata callback");
    check(SamePath(L"\\\\server\\share\\dir\\file.txt", L"\\\\?\\UNC\\SERVER\\SHARE\\dir\\file.txt"), "UNC final handle spelling compares canonically");
    check(!SamePath(L"\\\\server\\share\\file.txt", L"\\\\?\\UNC\\other\\share\\file.txt"), "changed network identity rejected");
    check(!SamePath(L"E:\\dir\\file.txt", L"E:\\elsewhere\\file.txt"), "redirected final object rejected");
    check(Parse(L"\\\\server\\share\\file.txt", path), "reconnect fixture parses");
    bool connected = false;
    auto reconnect = [&](const std::wstring& current, DWORD& flags) { if (!connected) return DWORD{ERROR_BAD_NETPATH}; flags = current == path.value ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY; return DWORD{ERROR_SUCCESS}; };
    check(CheckAncestors(path, true, drive, reconnect, [] { return false; }) == ERROR_BAD_NETPATH, "disconnected share fails without cached success");
    connected = true;
    check(CheckAncestors(path, true, drive, reconnect, [] { return false; }) == ERROR_SUCCESS, "reconnected share is probed again");
    if (argc == 2) {
        check(std::filesystem::path(argv[1]).is_absolute(), "explicit absolute test provider");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HMODULE module = std::filesystem::path(argv[1]).is_absolute() ? LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) : nullptr;
        using GetApi = HRESULT (WINAPI*)(uint32_t, uint32_t, PulsePickerUiV2Api*);
        auto get = module ? reinterpret_cast<GetApi>(GetProcAddress(module, "PulsePickerUiGetApiV2")) : nullptr;
        PulsePickerUiV2Api api{};
        check(get && get(2, sizeof(api), &api) == S_OK, "load real provider config validator");
        if (api.Initialize && api.ValidateConfig && api.Shutdown) {
            PulsePickerUiV2Initialize init{}; init.size = sizeof(init); init.version = 2;
            PulsePickerUiV2Session session = 0;
            const HRESULT initialized = api.Initialize(&init, &session);
            check(initialized == S_OK, "initialize real provider without UI");
            if (initialized == S_OK) {
                const std::wstring folder = L"\\\\server\\share";
                PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = 2;
                config.initial_folder = {folder.data(), static_cast<uint32_t>(folder.size())};
                config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                check(api.ValidateConfig(session, &config) == S_OK, "Open UNC syntax admitted without network I/O");
                config.access_policy = PULSE_PICKER_UI_V2_ACCESS_FOLDER_V1;
                check(api.ValidateConfig(session, &config) == S_OK, "Folder UNC syntax admitted without network I/O");
                config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_SAVE_V1;
                config.save_options = PULSE_PICKER_UI_V2_SAVE_OVERWRITE_PROMPT;
                check(api.ValidateConfig(session, &config) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Save UNC remains rejected before Show");
                const std::wstring local = L"C:\\", label = L"Share";
                config.initial_folder = {local.data(), static_cast<uint32_t>(local.size())};
                PulsePickerUiV2Place place{}; place.size = sizeof(place);
                place.label = {label.data(), static_cast<uint32_t>(label.size())}; place.path = {folder.data(), static_cast<uint32_t>(folder.size())};
                config.places = &place; config.place_count = 1;
                check(api.ValidateConfig(session, &config) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "Save UNC places remain rejected before Show");
                check(api.Shutdown(session) == S_OK, "shutdown real config validator");
            }
        }
        if (module) FreeLibrary(module);
        if (SUCCEEDED(com)) CoUninitialize();
    }
    std::printf("Checks=%u failures=%u; injected storage only, no real UNC or USB certification\n", checks, failures);
    return failures ? 1 : 0;
}
