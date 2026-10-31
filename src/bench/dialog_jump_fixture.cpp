// Isolated native common-dialog fixture for scripts/test_dialog_jump.py.
#include <cstdio>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <windows.h>

struct Events : IFileDialogEvents {
    ULONG refs = 1;
    std::wstring status;
    explicit Events(const wchar_t *path) : status(path) {
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs;
        if (!n)
            delete this;
        return n;
    }
    void Record(IFileDialog *dialog) {
        IShellItem *item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetFolder(&item))) {
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                HANDLE file = CreateFileW(status.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          CREATE_ALWAYS, 0, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD wrote;
                    WriteFile(file, path, static_cast<DWORD>(wcslen(path) * sizeof(wchar_t)), &wrote, nullptr);
                    CloseHandle(file);
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *dialog) override {
        Record(dialog);
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *dialog) override {
        Record(dialog);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *) override {
        return E_NOTIMPL;
    }
};
int wmain(int argc, wchar_t **argv) {
    if (argc < 4)
        return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileDialog *dialog = nullptr;
    HRESULT hr = CoCreateInstance(wcscmp(argv[1], L"save") == 0 ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        return 3;
    IShellItem *folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(argv[2], nullptr, IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
    }
    dialog->SetFileName(L"keep-name.txt");
    dialog->SetTitle(L"Pulse isolated navigation test");
    FILEOPENDIALOGOPTIONS flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
    auto *events = new Events(argv[3]);
    DWORD cookie = 0;
    dialog->Advise(events, &cookie);
    hr = dialog->Show(nullptr);
    dialog->Unadvise(cookie);
    events->Release();
    dialog->Release();
    CoUninitialize();
    return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : 1;
}
