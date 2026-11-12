#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <propsys.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
void Result(const char* name, HRESULT hr) { std::printf("%s hr=%08lX\n", name, static_cast<unsigned long>(hr)); }
void ProbeFolder(IFileDialog* dialog, const char* phase) {
    ComPtr<IShellItem> item; PWSTR name = nullptr;
    const HRESULT hr = dialog->GetFolder(&item);
    if (item) item->GetDisplayName(SIGDN_FILESYSPATH, &name);
    std::printf("folder phase=%s hr=%08lX path=%ls\n", phase, static_cast<unsigned long>(hr), name ? name : L"<none>"); CoTaskMemFree(name);
}
void Properties(IFileDialog* dialog, const char* phase) {
    ComPtr<IFileSaveDialog> save; dialog->QueryInterface(IID_PPV_ARGS(&save));
    if (!save) return;
    ComPtr<IPropertyStore> store; const HRESULT hr = save->GetProperties(&store);
    PROPVARIANT value{}; const HRESULT got = store ? store->GetValue(PKEY_Title, &value) : E_POINTER;
    std::printf("properties phase=%s hr=%08lX get=%08lX vt=%u title=%ls\n", phase, static_cast<unsigned long>(hr),
        static_cast<unsigned long>(got), value.vt, value.vt == VT_LPWSTR ? value.pwszVal : L"<none>"); PropVariantClear(&value);
}
BOOL CALLBACK DumpControl(HWND window, LPARAM) {
    wchar_t name[128]{}, text[256]{}; GetClassNameW(window, name, 128); GetWindowTextW(window, text, 256);
    if (wcscmp(name, TOOLBARCLASSNAMEW) == 0) {
        const auto count = SendMessageW(window, TB_BUTTONCOUNT, 0, 0);
        for (LRESULT i = 0; i < count && i < 32; ++i) {
            TBBUTTON button{};
            if (SendMessageW(window, TB_GETBUTTON, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&button)))
                std::printf("toolbar id=%d command=%d state=%u style=%u\n", GetDlgCtrlID(window), button.idCommand, button.fsState, button.fsStyle);
        }
    }
    if (wcscmp(name, WC_TREEVIEWW) != 0) return TRUE;
    std::printf("navigation-control class=%ls\n", name);
    auto item = TreeView_GetRoot(window);
    for (unsigned count = 0; item && count < 64; ++count) {
        TVITEMW value{}; value.mask = TVIF_TEXT; value.hItem = item; value.pszText = text; value.cchTextMax = 256;
        if (SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&value))) { char utf8[1024]{}; WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, 1024, nullptr, nullptr); std::printf("navigation-root-item=%s\n", utf8); }
        item = TreeView_GetNextSibling(window, item);
    }
    return TRUE;
}
void CALLBACK Cancel(HWND window, UINT, UINT_PTR id, DWORD) {
    KillTimer(window, id); EnumChildWindows(window, DumpControl, 0); PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
}
struct Events final : IFileDialogEvents {
    ULONG refs = 1; bool root_case = false; unsigned changed = 0; ComPtr<IShellItem> outside;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE; *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        ++changed; ProbeFolder(dialog, "OnFolderChange");
        ComPtr<IOleWindow> window; HWND hwnd = nullptr; dialog->QueryInterface(IID_PPV_ARGS(&window)); if (window) window->GetWindow(&hwnd);
        if (changed == 1) {
            if (root_case) {
                Result("SetFolder-outside-live", dialog->SetFolder(outside.Get()));
                ProbeFolder(dialog, "after-outside-live");
                if (hwnd) SetTimer(hwnd, 0xA91, 700, Cancel);
            } else {
                Properties(dialog, "live");
                Result("SetFileName-live", dialog->SetFileName(L"new-output.txt"));
                if (hwnd) PostMessageW(hwnd, WM_COMMAND, IDOK, 0);
            }
        }
        return S_OK;
    }
        HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        PWSTR current = nullptr; Result("FileOk-GetFileName", dialog->GetFileName(&current));
        Result("FileOk-repeat-name", dialog->SetFileName(current)); CoTaskMemFree(current);
        Result("FileOk-repeat-extension", dialog->SetDefaultExtension(L"txt"));
        Properties(dialog, "FileOk"); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { std::puts("event=FolderChanging"); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* out) override { *out = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* out) override { *out = FDEOR_DEFAULT; return S_OK; }
};
ComPtr<IShellItem> Item(const std::filesystem::path& path) { ComPtr<IShellItem> item; Result("ShellItem", SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))); return item; }
}
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    const auto base = std::filesystem::absolute(L"bench_data") / (L"advanced-native-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(base / L"root" / L"child"); std::filesystem::create_directories(base / L"outside");
    { std::ofstream file(base / L"root" / L"source.txt"); file << "owned"; }
    auto root = Item(base / L"root"), child = Item(base / L"root" / L"child"), outside = Item(base / L"outside"), source = Item(base / L"root" / L"source.txt");
    int failures = 0;
    for (int mode = 0; mode < 4; ++mode) {
        std::printf("scenario=%d\n", mode);
        ComPtr<IFileDialog> dialog; Result("Create", CoCreateInstance((mode == 0 || mode == 3) ? CLSID_FileOpenDialog : CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)));
        if (!dialog) { ++failures; continue; }
        GUID guid{}; CoCreateGuid(&guid); dialog->SetClientGuid(guid);
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR);
        Events events; events.root_case = mode == 0 || mode == 3; events.outside = mode == 3 ? root : outside;
        if (mode == 0 || mode == 3) {
            dialog->SetFolder(child.Get()); ComPtr<IFileDialog2> dialog2; dialog.As(&dialog2);
            Result("SetNavigationRoot", dialog2->SetNavigationRoot(root.Get()));
            Result("SetFolder-outside-before-show", dialog->SetFolder(mode == 3 ? root.Get() : outside.Get())); ProbeFolder(dialog.Get(), "before-show");
        } else {
            ComPtr<IFileSaveDialog> save; dialog.As(&save);
            ComPtr<IPropertyStore> store; PSCreateMemoryPropertyStore(IID_PPV_ARGS(&store));
            PROPVARIANT title{}; InitPropVariantFromString(L"original title", &title); store->SetValue(PKEY_Title, title); PropVariantClear(&title);
            ComPtr<IPropertyDescriptionList> list; PSGetPropertyDescriptionListFromString(L"prop:System.Title;System.Rating", IID_PPV_ARGS(&list));
            if (mode == 1) Result("SetSaveAsItem-before-metadata", save->SetSaveAsItem(source.Get()));
            Result("SetProperties", save->SetProperties(store.Get()));
            Result("SetCollectedProperties", save->SetCollectedProperties(list.Get(), FALSE));
            if (mode == 2) Result("SetSaveAsItem-after-metadata", save->SetSaveAsItem(source.Get()));
            Result("InitialDefaultExtension", dialog->SetDefaultExtension(L"txt")); Properties(dialog.Get(), "before-show");
        }
        DWORD cookie = 0; dialog->Advise(&events, &cookie);
        const HRESULT shown = dialog->Show(nullptr); Result("Show", shown); ProbeFolder(dialog.Get(), "after-show"); Properties(dialog.Get(), "after-show");
        if (shown != ((mode == 0 || mode == 3) ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : S_OK)) ++failures;
        dialog->Unadvise(cookie);
    }
    source.Reset(); outside.Reset(); child.Reset(); root.Reset();
    std::filesystem::remove_all(base);
    CoUninitialize(); std::printf("failures=%d\n", failures); return failures ? 1 : 0;
}
