#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <cstdio>
#include <initializer_list>

using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Record(const char* name, HRESULT result) {
    std::printf("%s hr=0x%08lX\n", name, static_cast<unsigned long>(result));
    if (result != S_OK) ++failures;
}
struct Events final : IFileDialogEvents {
    ULONG refs = 1;
    bool observed = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        if (observed) return S_OK;
        observed = true;
        std::puts("callback=OnFolderChange");
        Record("SetTitle", dialog->SetTitle(L"Dynamic oracle title"));
        Record("SetOkButtonLabel", dialog->SetOkButtonLabel(L"Choose dynamically"));
        Record("SetFileNameLabel", dialog->SetFileNameLabel(L"Dynamic name:"));
        Record("SetFileName", dialog->SetFileName(L"dynamic-probe.txt"));
        Record("SetDefaultExtension", dialog->SetDefaultExtension(L"log"));
        PWSTR name = nullptr;
        Record("GetFileName", dialog->GetFileName(&name));
        if (!name || wcscmp(name, L"dynamic-probe.txt")) ++failures;
        CoTaskMemFree(name);
        ComPtr<IFileDialogCustomize> custom;
        Record("Customize", dialog->QueryInterface(IID_PPV_ARGS(&custom)));
        if (custom) {
            Record("AddControlItem", custom->AddControlItem(42, 3, L"Third"));
            Record("SetSelectedControlItem", custom->SetSelectedControlItem(42, 3));
            Record("RemoveControlItem", custom->RemoveControlItem(42, 2));
            const HRESULT clear = custom->RemoveAllControlItems(42);
            std::printf("RemoveAllControlItems hr=0x%08lX\n", static_cast<unsigned long>(clear));
            if (clear != E_NOTIMPL) ++failures;
            Record("AddControlItemAfterClear", custom->AddControlItem(42, 4, L"Fourth"));
        }
        Record("Close", dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED)));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* out) override { *out = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* out) override { *out = FDEOR_DEFAULT; return S_OK; }
};
}
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    for (bool save : {false, true}) {
        std::printf("mode=%s\n", save ? "save" : "open");
        ComPtr<IFileDialog> dialog;
        Record("Create", CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)));
        if (!dialog) continue;
        ComPtr<IFileDialogCustomize> custom;
        dialog.As(&custom);
        Record("AddComboBox", custom->AddComboBox(42));
        Record("InitialItem1", custom->AddControlItem(42, 1, L"First"));
        Record("InitialItem2", custom->AddControlItem(42, 2, L"Second"));
        Events events; DWORD cookie = 0;
        Record("Advise", dialog->Advise(&events, &cookie));
        const HRESULT shown = dialog->Show(nullptr);
        std::printf("Show hr=0x%08lX observed=%u\n", static_cast<unsigned long>(shown), events.observed ? 1u : 0u);
        if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED) || !events.observed) ++failures;
        dialog->Unadvise(cookie);
    }
    CoUninitialize();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
