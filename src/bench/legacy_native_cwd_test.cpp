#include "../shell_bridge/legacy_com_provider.h"
#include "../shell_bridge/legacy_dialog_error.h"
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <cstdio>
#include <string>
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool adapter = argc > 4 && std::string(argv[4]) == "adapter";
    bool consumed = true; std::atomic<bool> child_observed = false;
    const pulse::shell_bridge::LegacyDialogFactory factory = [](bool save,IFileDialog** out){IUnknown* object=nullptr;HRESULT hr=pulse::shell_bridge::CreateSystemLegacyDialog(save,&object);if(SUCCEEDED(hr)){hr=object->QueryInterface(IID_PPV_ARGS(out));object->Release();}return hr;};
    const bool ansi = argc > 1 && std::string(argv[1]) == "ansi";
    const bool nochange = argc > 2 && std::string(argv[2]) == "nochange";
    const bool full = argc > 3 && std::string(argv[3]) == "full";
    const bool small_buffer = argc > 3 && std::string(argv[3]) == "small";
    const bool navigate = argc > 3 && std::string(argv[3]) == "navigate";
    const bool cancel = navigate || (argc > 3 && std::string(argv[3]) == "cancel");
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") / (L"legacy-cwd-" + std::to_wstring(GetCurrentProcessId())));
    const auto before = root / L"before", selected = root / L"selected", file = selected / L"probe.txt";
    std::filesystem::create_directories(before); std::filesystem::create_directory(selected);
    HANDLE fixture = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fixture == INVALID_HANDLE_VALUE) return 2;
    CloseHandle(fixture);
    const auto child = selected / L"navigated"; std::filesystem::create_directory(child);
    const auto original = std::filesystem::current_path(); std::filesystem::current_path(before);
    const DWORD thread = GetCurrentThreadId(); std::atomic<bool> done = false;
    std::thread click([&] {
        for (unsigned i = 0; i < 120 && !done; ++i) {
            Sleep(100);
            if(i==5 || i==20)std::printf("DURING cwd_before=%u cwd_selected=%u\n", std::filesystem::current_path()==before, std::filesystem::current_path()==selected);
            if (i < 25) continue;
            struct Action {bool cancel; bool query;};
            Action action{(cancel && (!navigate || i > 40)) || i > 100, navigate && i == 35};
            EnumThreadWindows(thread, [](HWND hwnd, LPARAM context) -> BOOL {
                auto& action = *reinterpret_cast<Action*>(context);
                wchar_t name[32]{}; GetClassNameW(hwnd, name, 32);
                if (std::wstring(name) == L"#32770" && IsWindowVisible(hwnd)) {
                    if(action.query) {wchar_t path[32768]{};const auto result=SendMessageW(hwnd,CDM_GETFOLDERPATH,32768,reinterpret_cast<LPARAM>(path));std::printf("NAVIGATION result=%lld path=%ls\n",static_cast<long long>(result),path);}
                    else PostMessageW(hwnd, WM_COMMAND, action.cancel ? IDCANCEL : IDOK, 0);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&action));
            if(navigate && i==35)child_observed=std::filesystem::current_path()==child;
            if(navigate && i==35)std::printf("NAVIGATED_CWD before=%u child=%u\n",std::filesystem::current_path()==before,std::filesystem::current_path()==child);
        }
    });
    const DWORD flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT | (nochange ? OFN_NOCHANGEDIR : 0);
    BOOL result = FALSE;
    if (ansi) {
        char buffer[1024] = "probe.txt";if(navigate)strcpy_s(buffer,"navigated");if(full)strcpy_s(buffer,file.string().c_str());
        const auto directory = (full?before:selected).string();
        OPENFILENAMEA ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = buffer; ofn.nMaxFile = small_buffer ? 12 : 1024;
        ofn.lpstrInitialDir = directory.c_str(); ofn.lpstrFilter = "Text\0*.txt\0\0"; ofn.Flags = flags;
        if(adapter)consumed=pulse::shell_bridge::RunLegacyComDialog(&ofn,false,factory,result);else result = GetOpenFileNameA(&ofn);
    } else {
        wchar_t buffer[1024] = L"probe.txt";if(navigate)wcscpy_s(buffer,L"navigated");if(full)wcscpy_s(buffer,file.c_str());
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.lpstrFile = buffer; ofn.nMaxFile = small_buffer ? 12 : 1024;
        ofn.lpstrInitialDir = full?before.c_str():selected.c_str(); ofn.lpstrFilter = L"Text\0*.txt\0\0"; ofn.Flags = flags;
        if(adapter)consumed=pulse::shell_bridge::RunLegacyComDialog(&ofn,false,factory,result);else result = GetOpenFileNameW(&ofn);
    }
    const DWORD error = adapter ? pulse::shell_bridge::legacy_dialog_error::Query(&CommDlgExtendedError) : CommDlgExtendedError(); const auto after = std::filesystem::current_path();
    done = true; click.join(); std::filesystem::current_path(original);
    std::printf("adapter=%u consumed=%u %s nochange=%u accepted=%u error=%lu cwd_before=%u cwd_selected=%u\n", adapter,consumed,ansi ? "A" : "W", nochange, result, error, after == before, after == selected);
    DeleteFileW(file.c_str()); RemoveDirectoryW(child.c_str()); RemoveDirectoryW(selected.c_str()); RemoveDirectoryW(before.c_str()); RemoveDirectoryW(root.c_str());
    const bool cwd_ok = (cancel || nochange) ? after == before : after == selected;
    return consumed && cwd_ok && (!navigate || child_observed) && (small_buffer ? !result && error == FNERR_BUFFERTOOSMALL : cancel ? !result && !error : result != FALSE) ? 0 : 1;
}
