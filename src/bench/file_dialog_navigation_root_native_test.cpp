#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
using Factory = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
void Module(const void* address) {
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module))
        GetModuleFileNameW(module, path, ARRAYSIZE(path));
    std::printf("MODULE %ls\n", path);
}
void LogFolder(IFileDialog* dialog, const char* phase) {
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    const auto hr = dialog->GetFolder(&item);
    const auto name_hr = item ? item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path) : E_UNEXPECTED;
    std::printf("FOLDER phase=%s hr=%08lX name_hr=%08lX path=%ls\n", phase,
        static_cast<unsigned long>(hr), static_cast<unsigned long>(name_hr), path ? path : L"<null>");
    CoTaskMemFree(path);
}
void LogItem(IShellItem* item, const char* phase) {
    PWSTR path = nullptr;
    const auto hr = item ? item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &path) : E_POINTER;
    std::printf("ITEM phase=%s hr=%08lX path=%ls\n", phase,
        static_cast<unsigned long>(hr), path ? path : L"<null>");
    CoTaskMemFree(path);
}
class Events final : public IFileDialogEvents {
public:
    std::atomic<ULONG> refs{1};
    ComPtr<IShellItem> mutation_root;
    bool mutate = false;
    unsigned folder_changes = 0, file_ok = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *output = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --refs; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++file_ok;
        ComPtr<IShellItem> result;
        const auto hr = dialog->GetResult(&result);
        std::printf("EVENT FileOk count=%u result_hr=%08lX thread=%lu verdict=S_OK\n",
            file_ok, static_cast<unsigned long>(hr), GetCurrentThreadId());
        if (result) LogItem(result.Get(), "OnFileOk.GetResult");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem* folder) override {
        std::printf("EVENT FolderChanging thread=%lu verdict=S_OK\n", GetCurrentThreadId());
        LogItem(folder, "FolderChanging"); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        ++folder_changes;
        std::printf("EVENT FolderChange count=%u thread=%lu\n", folder_changes, GetCurrentThreadId());
        LogFolder(dialog, "FolderChange");
        if (mutate) {
            mutate = false;
            ComPtr<IFileDialog2> dialog2;
            HRESULT hr = dialog->QueryInterface(IID_PPV_ARGS(&dialog2));
            if (SUCCEEDED(hr)) hr = dialog2->SetNavigationRoot(mutation_root.Get());
            std::printf("ROOT phase=during-Show hr=%08lX\n", static_cast<unsigned long>(hr));
            LogFolder(dialog, "after-during-Show-root");
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* dialog) override {
        ComPtr<IShellItem> item;
        const auto hr = dialog->GetCurrentSelection(&item);
        std::printf("EVENT SelectionChange hr=%08lX thread=%lu\n", static_cast<unsigned long>(hr), GetCurrentThreadId());
        if (item) LogItem(item.Get(), "GetCurrentSelection");
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { std::puts("EVENT TypeChange"); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* value) override {
        if (!value) return E_POINTER; *value = FDESVR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem* item, FDE_OVERWRITE_RESPONSE* value) override {
        if (!value) return E_POINTER;
        std::puts("EVENT Overwrite response=DEFAULT"); LogItem(item, "Overwrite"); *value = FDEOR_DEFAULT; return S_OK;
    }
};
int ManualShow(Factory factory, const std::filesystem::path& base, const std::wstring& scenario, bool save) {
    if (scenario != L"pane" && scenario != L"initial-outside" && scenario != L"setter-outside" &&
        scenario != L"mutation" && scenario != L"repeat") return 6;
    ComPtr<IClassFactory> cf;
    ComPtr<IFileDialog2> dialog;
    auto hr = factory ? factory(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&cf)) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
    std::printf("NATIVE manual=1 kind=%s bits=%zu create=%08lX thread=%lu\n", save ? "Save" : "Open",
        sizeof(void*) * 8, static_cast<unsigned long>(hr), GetCurrentThreadId());
    if (FAILED(hr) || !dialog) return 7;
    Module((*reinterpret_cast<void***>(dialog.Get()))[28]);
    ComPtr<IShellItem> root, child, outside;
    if (FAILED(SHCreateItemFromParsingName((base / L"root").c_str(), nullptr, IID_PPV_ARGS(&root))) ||
        FAILED(SHCreateItemFromParsingName((base / L"root" / L"child").c_str(), nullptr, IID_PPV_ARGS(&child))) ||
        FAILED(SHCreateItemFromParsingName((base / L"sibling").c_str(), nullptr, IID_PPV_ARGS(&outside)))) return 8;
    GUID guid{}; CoCreateGuid(&guid);
    std::printf("CONFIG ClientGuid=%08lX-%04X-%04X (unique, no ClearClientData)\n",
        static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3);
    hr = dialog->SetClientGuid(guid);
    std::printf("CONFIG SetClientGuid hr=%08lX\n", static_cast<unsigned long>(hr));
    const auto options = static_cast<FILEOPENDIALOGOPTIONS>(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        FOS_NOCHANGEDIR | FOS_DONTADDTORECENT | (save ? FOS_NOREADONLYRETURN | FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    hr = dialog->SetOptions(options);
    std::printf("CONFIG SetOptions value=%08lX hr=%08lX\n", static_cast<unsigned long>(options), static_cast<unsigned long>(hr));
    hr = dialog->SetFolder(scenario == L"initial-outside" ? outside.Get() : child.Get());
    std::printf("CONFIG SetFolder-before-root hr=%08lX\n", static_cast<unsigned long>(hr));
    hr = dialog->SetNavigationRoot(root.Get());
    std::printf("ROOT phase=before-Show hr=%08lX\n", static_cast<unsigned long>(hr));
    if (hr != S_OK) return 9;
    if (scenario == L"setter-outside") {
        hr = dialog->SetFolder(outside.Get());
        std::printf("CONFIG SetFolder-after-root-outside hr=%08lX\n", static_cast<unsigned long>(hr));
    }
    hr = dialog->SetFileName(save ? L"new-save.txt" : L"owned.txt");
    std::printf("CONFIG SetFileName hr=%08lX\n", static_cast<unsigned long>(hr));
    LogFolder(dialog.Get(), "before-Show");
    auto* sink = new Events;
    sink->mutate = scenario == L"mutation"; sink->mutation_root = outside;
    DWORD cookie = 0;
    hr = dialog->Advise(sink, &cookie);
    if (FAILED(hr)) { sink->Release(); return 10; }
    std::printf("FIXTURE root=%ls child=%ls outside=%ls outside_file=%ls\n", (base / L"root").c_str(),
        (base / L"root" / L"child").c_str(), (base / L"sibling").c_str(), (base / L"sibling" / L"outside.txt").c_str());
    std::puts("MANUAL_ONLY no timer, input, click, navigation or automatic close; operator controls native UI; host never writes selected result");
    for (unsigned show = 1; show <= (scenario == L"repeat" ? 2u : 1u); ++show) {
        const auto title = L"Pulse.Root.Native." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + scenario + L"." + (save ? L"Save" : L"Open") + L"." + std::to_wstring(show);
        dialog->SetTitle(title.c_str());
        std::printf("SHOW_BEGIN sequence=%u title=%ls\n", show, title.c_str());
        const auto shown = dialog->Show(nullptr);
        std::printf("SHOW_END sequence=%u hr=%08lX file_ok=%u changes=%u\n", show,
            static_cast<unsigned long>(shown), sink->file_ok, sink->folder_changes);
        LogFolder(dialog.Get(), "after-Show");
        ComPtr<IShellItem> result;
        const auto result_hr = dialog->GetResult(&result);
        std::printf("RESULT sequence=%u hr=%08lX item=%u\n", show, static_cast<unsigned long>(result_hr), result ? 1u : 0u);
        if (result) LogItem(result.Get(), "final.GetResult");
    }
    const auto unadvised = dialog->Unadvise(cookie);
    std::printf("CLEANUP Unadvise hr=%08lX\n", static_cast<unsigned long>(unadvised));
    sink->Release();
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool manual = argc == 5 && std::wstring(argv[1]) == L"--show";
    if (argc != 2 && !manual) return 2;
    if (manual && std::wstring(argv[4]) != L"open" && std::wstring(argv[4]) != L"save") return 2;
    const std::filesystem::path base = std::filesystem::absolute(argv[manual ? 3 : 1]);
    if (std::filesystem::exists(base)) return 3;
    std::filesystem::create_directories(base / L"root" / L"child");
    std::filesystem::create_directory(base / L"sibling");
    { std::ofstream file(base / L"root" / L"owned.txt"); file << "owned-root-oracle"; }
    for (const auto& relative : { L"root\\child\\owned.txt", L"sibling\\owned.txt", L"sibling\\outside.txt" }) {
        std::ofstream file(base / relative); file << "owned-root-oracle";
    }
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 4;
    unsigned failures = 0;
    {
        wchar_t system[32768]{};
        GetSystemDirectoryW(system, ARRAYSIZE(system));
        const auto module = LoadLibraryExW((std::wstring(system) + L"\\comdlg32.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto factory = module ? reinterpret_cast<Factory>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
        Module(reinterpret_cast<const void*>(factory));
        if (manual) {
            const int result = ManualShow(factory, base, argv[2], std::wstring(argv[4]) == L"save");
            CoUninitialize(); return result;
        }
        ComPtr<IShellItem> root, child, sibling, file, desktop;
        const auto item = [&](const wchar_t* relative, ComPtr<IShellItem>& value) {
            auto path = base / relative; path.make_preferred();
            const auto hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&value));
            std::printf("ITEM path=%ls hr=%08lX\n", path.c_str(), static_cast<unsigned long>(hr));
            return hr;
        };
        if (FAILED(item(L"root", root)) || FAILED(item(L"root/child", child)) ||
            FAILED(item(L"sibling", sibling)) || FAILED(item(L"root/owned.txt", file)) ||
            FAILED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&desktop)))) return 5;
        for (unsigned kind = 0; kind != 2; ++kind) {
            ComPtr<IClassFactory> cf;
            ComPtr<IFileDialog2> dialog;
            HRESULT hr = factory ? factory(kind ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&cf)) : E_NOINTERFACE;
            if (SUCCEEDED(hr)) hr = cf->CreateInstance(nullptr, IID_PPV_ARGS(&dialog));
            std::printf("NATIVE kind=%s bits=%zu create=%08lX no_show=1\n", kind ? "Save" : "Open", sizeof(void*) * 8, static_cast<unsigned long>(hr));
            if (FAILED(hr) || !dialog) { ++failures; continue; }
            Module((*reinterpret_cast<void***>(dialog.Get()))[28]);
            GUID guid{}; CoCreateGuid(&guid);
            dialog->SetClientGuid(guid);
            const auto set = [&](const char* phase, IShellItem* value) {
                const auto result = dialog->SetNavigationRoot(value);
                std::printf("ROOT phase=%s hr=%08lX\n", phase, static_cast<unsigned long>(result));
                LogFolder(dialog.Get(), phase);
                return result;
            };
            set("null-before", nullptr);
            hr = dialog->SetFolder(sibling.Get());
            std::printf("SET_FOLDER phase=sibling-before hr=%08lX\n", static_cast<unsigned long>(hr));
            LogFolder(dialog.Get(), "sibling-before");
            if (set("root", root.Get()) != S_OK) ++failures;
            hr = dialog->SetFolder(child.Get());
            std::printf("SET_FOLDER phase=child-after-root hr=%08lX\n", static_cast<unsigned long>(hr));
            LogFolder(dialog.Get(), "child-after-root");
            hr = dialog->SetFolder(sibling.Get());
            std::printf("SET_FOLDER phase=sibling-after-root hr=%08lX\n", static_cast<unsigned long>(hr));
            LogFolder(dialog.Get(), "sibling-after-root");
            hr = dialog->SetDefaultFolder(sibling.Get());
            std::printf("SET_DEFAULT phase=sibling-after-root hr=%08lX\n", static_cast<unsigned long>(hr));
            LogFolder(dialog.Get(), "default-after-root");
            set("replace-sibling", sibling.Get());
            set("null-after", nullptr);
            set("file-root", file.Get());
            set("computer-root", desktop.Get());
            ComPtr<IShellItem> result;
            hr = dialog->GetResult(&result);
            std::printf("RESULT no_show=1 hr=%08lX item=%u\n", static_cast<unsigned long>(hr), result ? 1u : 0u);
        }
    }
    CoUninitialize();
    std::printf("NATIVE_ROOT failures=%u no_show=1\n", failures);
    return failures ? 1 : 0;
}




