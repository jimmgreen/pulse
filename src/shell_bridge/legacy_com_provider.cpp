#include "legacy_com_provider.h"
#include "legacy_dialog_error.h"
#include "legacy_hook_host.h"
#include <wrl/client.h>
#include <shlobj.h>
#include <cderr.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>
#include <type_traits>
#include <dlgs.h>
namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
constexpr DWORD supported = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
    OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_ENABLESIZING |
    OFN_EXTENSIONDIFFERENT | OFN_OVERWRITEPROMPT | OFN_NOREADONLYRETURN | OFN_NODEREFERENCELINKS | OFN_FORCESHOWHIDDEN |
    OFN_NOTESTFILECREATE | OFN_LONGNAMES | OFN_NOLONGNAMES | OFN_ENABLEHOOK |
    OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE;
std::wstring Wide(const wchar_t* text) { return text ? text : L""; }
std::wstring Wide(const char* text) {
    if (!text) return {};
    const int n = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (!n) throw E_INVALIDARG;
    std::wstring output(n, 0);
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, -1, output.data(), n)) throw E_INVALIDARG;
    output.pop_back();
    return output;
}
template<class C> std::basic_string<C> Encode(const std::wstring& text) {
    if constexpr (std::is_same_v<C, wchar_t>) return text;
    else {
        const bool utf8 = GetACP() == CP_UTF8;
        BOOL lost = FALSE;
        const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        const int n = WideCharToMultiByte(CP_ACP, flags, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, utf8 ? nullptr : &lost);
        if ((!n && !text.empty()) || lost) throw E_INVALIDARG;
        std::string output(n, 0);
        if (n && !WideCharToMultiByte(CP_ACP, flags, text.data(), static_cast<int>(text.size()), output.data(), n, nullptr, utf8 ? nullptr : &lost)) throw E_INVALIDARG;
        if (lost) throw E_INVALIDARG;
        return output;
    }
}
// Legacy Explorer Open mutates the process directory while browsing, restores it
// on cancellation, but retains the selected folder even on output-buffer failure.
struct LegacyOpenDirectory {
    bool enabled = false, keep = false;
    std::wstring original;
    explicit LegacyOpenDirectory(bool active) : enabled(active) {}
    HRESULT Capture() {
        if (!enabled) return S_OK;
        const DWORD count = GetCurrentDirectoryW(0, nullptr);
        if (!count) return HRESULT_FROM_WIN32(GetLastError());
        original.resize(count);
        const DWORD copied = GetCurrentDirectoryW(count, original.data());
        if (!copied || copied >= count) return HRESULT_FROM_WIN32(copied ? ERROR_RETRY : GetLastError());
        original.resize(copied); return S_OK;
    }
    ~LegacyOpenDirectory() {
        if (enabled && !keep && !original.empty()) {
            const DWORD error = GetLastError(); SetCurrentDirectoryW(original.c_str()); SetLastError(error);
        }
    }
};
class LegacyDirectoryEvents final : public IFileDialogEvents {
    LONG references_ = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *value = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = InterlockedDecrement(&references_); if (!count) delete this; return static_cast<ULONG>(count); }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        ComPtr<IShellItem> folder; PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetFolder(&folder)) && folder && SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            if (path) SetCurrentDirectoryW(path); CoTaskMemFree(path);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* result) override { *result = FDESVR_DEFAULT; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* result) override { *result = FDEOR_DEFAULT; return S_OK; }
};
template<class O> class LegacyHookEvents final : public IFileDialogEvents {
    LONG references_ = 1;
    O* input_;
    LegacyHookHost host_;
    bool initialized_ = false;
    DWORD terminal_error_ = 0;
    LRESULT ParentMessage(IFileDialog* dialog, UINT message, WPARAM capacity, LPARAM buffer) {
        using C=std::remove_pointer_t<decltype(input_->lpstrFile)>;
        if (message == CDM_GETSPEC || message == CDM_GETFILEPATH || message == CDM_GETFOLDERPATH) {
            std::wstring value;
            if (message != CDM_GETFOLDERPATH) {
                PWSTR text=nullptr; const HRESULT hr=dialog->GetFileName(&text);
                if(FAILED(hr)) return -1;
                if(text) value=text; CoTaskMemFree(text);
            }
            if (message != CDM_GETSPEC) {
                ComPtr<IShellItem> folder; PWSTR text=nullptr;
                if(FAILED(dialog->GetFolder(&folder)) || FAILED(folder->GetDisplayName(SIGDN_FILESYSPATH,&text))) return -1;
                std::wstring directory=text?text:L"";CoTaskMemFree(text);
                if(message==CDM_GETFOLDERPATH) value=std::move(directory);
                else if(!value.empty() && !(value.size()>1 && value[1]==L':') && !value.starts_with(L"\\")) {
                    if(!directory.empty() && directory.back()!=L'\\') directory+=L'\\';
                    value=directory+value;
                }
            }
            const auto encoded=Encode<C>(value);
            const size_t required=encoded.size()+1;
            if(buffer && capacity>=required) {
                auto* output=reinterpret_cast<C*>(buffer);
                std::copy(encoded.begin(),encoded.end(),output);output[encoded.size()]=0;
            }
            return static_cast<LRESULT>(required);
        }
        if(message==CDM_GETFOLDERIDLIST) {
            ComPtr<IShellItem> folder; PIDLIST_ABSOLUTE pidl=nullptr;
            if(FAILED(dialog->GetFolder(&folder)) || FAILED(SHGetIDListFromObject(folder.Get(),&pidl))) return -1;
            const UINT bytes=ILGetSize(pidl);
            if(buffer && capacity>=bytes) std::memcpy(reinterpret_cast<void*>(buffer),pidl,bytes);
            CoTaskMemFree(pidl); return bytes;
        }
        if(message==CDM_SETDEFEXT) {
            return SUCCEEDED(dialog->SetDefaultExtension(Wide(reinterpret_cast<const C*>(buffer)).c_str()))?0:-1;
        }
        if(message==CDM_SETCONTROLTEXT) {
            const auto text=Wide(reinterpret_cast<const C*>(buffer));
            if(capacity==edt1 || capacity==cmb13) return SUCCEEDED(dialog->SetFileName(text.c_str()))?0:-1;
            if(capacity==IDOK) return SUCCEEDED(dialog->SetOkButtonLabel(text.c_str()))?0:-1;
            const HWND control=GetDlgItem(host_.Window(),static_cast<int>(capacity));
            return control && SetWindowTextW(control,text.c_str())?0:-1;
        }
        if(message==CDM_HIDECONTROL) {
            const HWND control=GetDlgItem(host_.Window(),static_cast<int>(capacity));
            if(!control) return -1; ShowWindow(control,SW_HIDE);return 0;
        }
        return -1;
    }
    HRESULT Ensure(IFileDialog* dialog) {
        if (initialized_) return host_.Window() ? S_OK : E_FAIL;
        ComPtr<IOleWindow> window; HWND parent = nullptr;
        HRESULT hr = dialog->QueryInterface(IID_PPV_ARGS(&window));
        if (FAILED(hr) || FAILED(hr = window->GetWindow(&parent))) return hr;
        if (FAILED(hr = host_.Create(parent, [this, dialog](UINT message, WPARAM wp, LPARAM lp) { return ParentMessage(dialog, message, wp, lp); }))) { dialog->Close(hr); return hr; }
        initialized_ = true; host_.Notify(CDN_INITDONE); return S_OK;
    }
public:
    explicit LegacyHookEvents(O* input) : input_(input), host_(input) {}
    DWORD TerminalError() const noexcept { return terminal_error_; }
    HRESULT Prepare() { return host_.Prepare(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *out = static_cast<IFileDialogEvents*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=InterlockedDecrement(&references_); if(!n) delete this; return static_cast<ULONG>(n); }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        const HRESULT hr=Ensure(dialog); if(SUCCEEDED(hr)) host_.Notify(CDN_FOLDERCHANGE); return hr;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* dialog) override {
        const HRESULT hr=Ensure(dialog); if(SUCCEEDED(hr)) host_.Notify(CDN_SELCHANGE); return hr;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override {
        const HRESULT hr=Ensure(dialog); if(FAILED(hr)) return hr;
        UINT index=0; if(SUCCEEDED(dialog->GetFileTypeIndex(&index))) input_->nFilterIndex=index;
        host_.Notify(CDN_TYPECHANGE); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        try {
            const HRESULT ready=Ensure(dialog); if(FAILED(ready)) return ready;
            ComPtr<IShellItem> item; HRESULT hr=dialog->GetResult(&item); if(FAILED(hr)) return hr;
            PWSTR raw=nullptr; hr=item->GetDisplayName(SIGDN_FILESYSPATH,&raw); if(FAILED(hr)) return hr;
            std::wstring path(raw); CoTaskMemFree(raw);
            using C=std::remove_pointer_t<decltype(input_->lpstrFile)>;
            const auto encoded=Encode<C>(path);
            if(encoded.size()+1>input_->nMaxFile) {
                const size_t required=encoded.size()+1;
                if(required<=MAXWORD) {const WORD count=static_cast<WORD>(required);std::memcpy(input_->lpstrFile,&count,sizeof(count));}
                terminal_error_=FNERR_BUFFERTOOSMALL;
                dialog->Close(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
                return S_FALSE;
            }
            const auto slash=path.find_last_of(L"\\/");
            const auto dot=path.find_last_of(L'.');
            const auto offset=Encode<C>(path.substr(0,slash==path.npos?0:slash+1)).size();
            const auto extension=dot!=path.npos && (slash==path.npos || dot>slash) ? Encode<C>(path.substr(0,dot+1)).size():0;
            if(offset>MAXWORD || extension>MAXWORD) return E_INVALIDARG;
            std::copy(encoded.begin(),encoded.end(),input_->lpstrFile); input_->lpstrFile[encoded.size()]=0;
            input_->nFileOffset=static_cast<WORD>(offset);input_->nFileExtension=static_cast<WORD>(extension);
            UINT index=0; if(SUCCEEDED(dialog->GetFileTypeIndex(&index))) input_->nFilterIndex=index;
            return host_.FileOk()?S_FALSE:S_OK;
        } catch (...) { return E_UNEXPECTED; }
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* response) override { *response=FDESVR_DEFAULT;return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* response) override { *response=FDEOR_DEFAULT;return S_OK; }
};
struct LegacySubscription {
    IFileDialog* dialog = nullptr;
    DWORD cookie = 0;
    ~LegacySubscription() { if (dialog) dialog->Unadvise(cookie); }
};
template<class O> bool Run(O* input, bool save, const LegacyDialogFactory& create, BOOL& result, const LegacyDiagnostic& diagnostic, bool verified_local_hook_host) {
    using C = std::remove_pointer_t<decltype(input->lpstrFile)>;
    LegacyComDiagnostic details;
    auto decline = [&](const char* reason, HRESULT error = E_INVALIDARG) {
        const DWORD saved_error = GetLastError();
        details.reason = reason; details.error = error;
        try { if (diagnostic) diagnostic(details); } catch (...) {}
        SetLastError(saved_error);
        return false;
    };
    if (!input) return decline("legacy-decline-null-input");
    details.structure_size = input->lStructSize;
    if (input->lStructSize != sizeof(O)) return decline("legacy-decline-structure-size");
    details.flags = input->Flags; details.flags_ex = input->FlagsEx;
    details.max_file = input->nMaxFile; details.max_title = input->nMaxFileTitle;
    details.max_custom_filter = input->nMaxCustFilter; details.filter_index = input->nFilterIndex;
    details.presence = (input->lpstrFile ? 1u : 0u) | (input->lpstrFileTitle ? 2u : 0u) |
        (input->lpstrFilter ? 4u : 0u) | (input->lpstrCustomFilter ? 8u : 0u) |
        (input->lpstrInitialDir ? 16u : 0u) | (input->lpstrTitle ? 32u : 0u) |
        (input->lpstrDefExt ? 64u : 0u) | (input->lpfnHook ? 128u : 0u) |
        (input->lpTemplateName ? 256u : 0u) | (input->pvReserved ? 512u : 0u) |
        (input->dwReserved ? 1024u : 0u) | (input->lCustData ? 2048u : 0u);
    if (!input->lpstrFile) return decline("legacy-decline-file-buffer");
    if (input->nMaxFile < 2 || input->nMaxFile > 1024 * 1024) return decline("legacy-decline-file-capacity");
    if (input->nMaxFileTitle > 1024 * 1024 || (input->lpstrFileTitle && !input->nMaxFileTitle)) return decline("legacy-decline-title-capacity");
    // OPENFILENAME ignores these pointers when their enabling flags are clear.
    // Never inspect a disabled hook/template pointer, including integer resources.
    if ((input->Flags & OFN_ENABLEHOOK) && (!verified_local_hook_host || !input->lpfnHook)) return decline("legacy-decline-hook");
    if ((input->Flags & OFN_ENABLEHOOK) && (input->Flags & OFN_ALLOWMULTISELECT)) return decline("legacy-decline-hook-multiselect");
    const bool customization = (input->Flags & (OFN_ENABLEHOOK | OFN_ENABLETEMPLATE | OFN_ENABLETEMPLATEHANDLE)) != 0;
    if (customization && !verified_local_hook_host) return decline("legacy-decline-customization-host");
    if (customization && (input->Flags & OFN_ALLOWMULTISELECT)) return decline("legacy-decline-customization-multiselect");
    if (input->lpstrCustomFilter || input->nMaxCustFilter) return decline("legacy-decline-custom-filter");
    if (input->pvReserved || input->dwReserved) return decline("legacy-decline-reserved");
    if (input->FlagsEx) return decline("legacy-decline-flags-ex");
    // Enabled templates are bounded and validated before Show; disabled hook
    // pointers and lCustData remain untouched. Explorer LONGNAMES/NOLONGNAMES
    // are ignored by the native dialog.
    if (input->Flags & ~supported) return decline("legacy-decline-unsupported-flags");
    if (!(input->Flags & OFN_EXPLORER)) return decline("legacy-decline-missing-explorer");
    if (!(input->Flags & OFN_HIDEREADONLY)) return decline("legacy-decline-missing-hidereadonly");
    if (save && !(input->Flags & OFN_NOCHANGEDIR)) return decline("legacy-decline-missing-nochangedir");
    if (save && (input->Flags & OFN_ALLOWMULTISELECT)) return decline("legacy-decline-save-multiselect");
    const char* stage = "legacy-decline-convert-input";
    bool shown = false;
    result = FALSE;
    try {
        LegacyOpenDirectory cwd(!save && !(input->Flags & OFN_NOCHANGEDIR));
        const HRESULT captured_cwd = cwd.Capture();
        if (FAILED(captured_cwd)) return decline("legacy-decline-capture-cwd", captured_cwd);
        if (std::find(input->lpstrFile, input->lpstrFile + input->nMaxFile, C{}) == input->lpstrFile + input->nMaxFile) return decline("legacy-decline-unterminated-file");
        auto initial = Wide(input->lpstrFile);
        auto directory = Wide(input->lpstrInitialDir);
        const auto initial_slash = initial.find_last_of(L"\\/");
        if (initial_slash != std::wstring::npos) {
            wchar_t absolute[32768]{};
            const DWORD count = GetFullPathNameW(initial.c_str(), 32768, absolute, nullptr);
            if (!count || count >= 32768) return decline("legacy-decline-initial-fullpath", HRESULT_FROM_WIN32(count ? ERROR_FILENAME_EXCED_RANGE : GetLastError()));
            initial = absolute;
            const auto split = initial.find_last_of(L"\\/");
            directory = initial.substr(0, split + 1);
            initial.erase(0, split + 1);
        }
        if ((input->Flags & OFN_ENABLEHOOK) && !initial.empty() &&
            ((directory.size() >= 3 && directory[1] == L':' && (directory[2] == L'\\' || directory[2] == L'/')) || directory.starts_with(L"\\\\"))) {
            auto candidate = directory;
            if (!candidate.empty() && candidate.back() != L'\\' && candidate.back() != L'/') candidate += L'\\';
            candidate += initial;
            if (Encode<C>(candidate).size() + 1 > input->nMaxFile)
                return decline("legacy-decline-hook-initial-buffer", HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
        }
        const auto title = Wide(input->lpstrTitle);
        const auto extension = Wide(input->lpstrDefExt);
        // Explorer A/W native dialogs preserve long defaults (for example html)
        // even though the legacy structure documentation describes three units.
        // Keep the complete setter value and explicit filename; never truncate.
        if (extension.size() > 255) return decline("legacy-decline-extension-length");
        stage = "legacy-decline-convert-filter";
        std::vector<std::wstring> filter_strings;
        if (input->lpstrFilter) {
            const C* at = input->lpstrFilter;
            while (*at && filter_strings.size() < 128) {
                filter_strings.push_back(Wide(at)); at += std::char_traits<C>::length(at) + 1;
                if (!*at) return decline("legacy-decline-filter-pair");
                filter_strings.push_back(Wide(at)); at += std::char_traits<C>::length(at) + 1;
            }
            if (*at) return decline("legacy-decline-filter-count");
        }
        // The adapter owns a balanced STA initialization when the caller has none.
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return decline("legacy-decline-apartment", initialized);
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
        ComPtr<IFileDialog> dialog;
        stage = "legacy-decline-factory-exception";
        HRESULT setup = create(save, &dialog);
        if (FAILED(setup) || !dialog) return decline("legacy-decline-factory", FAILED(setup) ? setup : E_POINTER);
        DWORD options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        const std::pair<DWORD, DWORD> mapped[]{
            {OFN_FILEMUSTEXIST,FOS_FILEMUSTEXIST}, {OFN_PATHMUSTEXIST,FOS_PATHMUSTEXIST},
            {OFN_ALLOWMULTISELECT,FOS_ALLOWMULTISELECT}, {OFN_OVERWRITEPROMPT,FOS_OVERWRITEPROMPT},
            {OFN_NOREADONLYRETURN,FOS_NOREADONLYRETURN}, {OFN_NODEREFERENCELINKS,FOS_NODEREFERENCELINKS},
            {OFN_DONTADDTORECENT,FOS_DONTADDTORECENT}, {OFN_FORCESHOWHIDDEN,FOS_FORCESHOWHIDDEN},
            {OFN_NOTESTFILECREATE,FOS_NOTESTFILECREATE}};
        for (const auto& [ofn, fos] : mapped) if (input->Flags & ofn) options |= fos;
        if (FAILED(setup = dialog->SetOptions(options))) return decline("legacy-decline-set-options", setup);
        std::vector<COMDLG_FILTERSPEC> filters;
        for (size_t i = 0; i < filter_strings.size(); i += 2) filters.push_back({filter_strings[i].c_str(), filter_strings[i + 1].c_str()});
        if (!filters.empty()) {
            if (FAILED(setup = dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data()))) return decline("legacy-decline-set-types", setup);
            if (FAILED(setup = dialog->SetFileTypeIndex(input->nFilterIndex ? input->nFilterIndex : 1))) return decline("legacy-decline-set-type-index", setup);
        }
        if (!directory.empty()) {
            ComPtr<IShellItem> folder;
            if (FAILED(setup = SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&folder)))) return decline("legacy-decline-folder-item", setup);
            if (FAILED(setup = dialog->SetFolder(folder.Get()))) return decline("legacy-decline-set-folder", setup);
        }
        if (!initial.empty() && FAILED(setup = dialog->SetFileName(initial.c_str()))) return decline("legacy-decline-set-filename", setup);
        if (!title.empty() && FAILED(setup = dialog->SetTitle(title.c_str()))) return decline("legacy-decline-set-title", setup);
        if (!extension.empty() && FAILED(setup = dialog->SetDefaultExtension(extension.c_str()))) return decline("legacy-decline-set-extension", setup);
        LegacySubscription subscription;
        ComPtr<IFileDialogEvents> directory_events;
        if (cwd.enabled) {
            directory_events.Attach(new LegacyDirectoryEvents());
            if (FAILED(setup = dialog->Advise(directory_events.Get(), &subscription.cookie))) return decline("legacy-decline-cwd-events", setup);
            subscription.dialog = dialog.Get();
        }
        LegacySubscription hook_subscription;
        ComPtr<IFileDialogEvents> hook_events;
        LegacyHookEvents<O>* hook_state = nullptr;
        if (customization) {
            hook_state = new LegacyHookEvents<O>(input);
            hook_events.Attach(hook_state);
            if (FAILED(setup = hook_state->Prepare())) return decline("legacy-decline-template-format", setup);
            if (FAILED(setup = dialog->Advise(hook_events.Get(), &hook_subscription.cookie))) return decline("legacy-decline-hook-events", setup);
            hook_subscription.dialog = dialog.Get();
        }
        shown = true;
        const HRESULT hr = dialog->Show(input->hwndOwner);
        if (FAILED(hr)) {
            legacy_dialog_error::SetBridge(hook_state && hook_state->TerminalError() ? hook_state->TerminalError() : hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? 0 : CDERR_DIALOGFAILURE);
            return true;
        }
        cwd.keep = cwd.enabled;
        std::vector<std::wstring> paths;
        auto append = [&](IShellItem* item) {
            PWSTR path = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) throw E_FAIL;
            try { paths.emplace_back(path); } catch (...) { CoTaskMemFree(path); throw; }
            CoTaskMemFree(path);
        };
        if (!save && (input->Flags & OFN_ALLOWMULTISELECT)) {
            ComPtr<IFileOpenDialog> open;
            ComPtr<IShellItemArray> items;
            if (FAILED(dialog.As(&open)) || FAILED(open->GetResults(&items))) throw E_FAIL;
            DWORD count = 0;
            if (FAILED(items->GetCount(&count)) || !count || count > 1024) throw E_FAIL;
            for (DWORD i = 0; i < count; ++i) { ComPtr<IShellItem> item; if (FAILED(items->GetItemAt(i, &item))) throw E_FAIL; append(item.Get()); }
        } else { ComPtr<IShellItem> item; if (FAILED(dialog->GetResult(&item))) throw E_FAIL; append(item.Get()); }
        const auto slash = paths.front().find_last_of(L"\\/");
        if (slash == std::wstring::npos) throw E_FAIL;
        if (cwd.enabled) {
            const auto selected_directory = paths.front().substr(0, slash == 2 ? 3 : slash);
            SetCurrentDirectoryW(selected_directory.c_str());
        }
        std::wstring packed;
        size_t file_offset = 0, extension_offset = 0;
        const auto leaf = paths.front().substr(slash + 1);
        if (paths.size() == 1) {
            packed = paths.front(); file_offset = slash + 1;
            const auto dot = packed.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > slash) extension_offset = dot + 1;
            packed.push_back(0);
            // Explorer's multiselect buffer is a complete double-NUL list even
            // for one full path. Leaving caller bytes after the first NUL can
            // make a host consume phantom files from its previous buffer.
            if (input->Flags & OFN_ALLOWMULTISELECT) packed.push_back(0);
        } else {
            const auto parent = paths.front().substr(0, slash);
            packed = parent; if (slash == 2) packed.push_back(L'\\'); packed.push_back(0); file_offset = packed.size();
            for (const auto& path : paths) {
                const auto split = path.find_last_of(L"\\/");
                if (split == std::wstring::npos || path.substr(0, split) != parent) throw E_FAIL;
                packed += path.substr(split + 1); packed.push_back(0);
            }
            packed.push_back(0);
        }
        const auto encoded = Encode<C>(packed);
        const auto encoded_title = Encode<C>(leaf);
        const auto offset = Encode<C>(packed.substr(0, file_offset)).size();
        const auto ext_offset = extension_offset ? Encode<C>(packed.substr(0, extension_offset)).size() : 0;
        UINT filter_index = input->nFilterIndex;
        if (!filters.empty() && FAILED(dialog->GetFileTypeIndex(&filter_index))) throw E_FAIL;
        if (encoded.size() > input->nMaxFile) {
            if (encoded.size() <= MAXWORD) { const WORD count = static_cast<WORD>(encoded.size()); std::memcpy(input->lpstrFile, &count, sizeof(count)); }
            legacy_dialog_error::SetBridge(FNERR_BUFFERTOOSMALL); return true;
        }
        if (offset > MAXWORD || ext_offset > MAXWORD) throw E_FAIL;
        std::copy(encoded.begin(), encoded.end(), input->lpstrFile);
        if (input->lpstrFileTitle) {
            const size_t count = (std::min)(encoded_title.size(), size_t(input->nMaxFileTitle - 1));
            std::copy_n(encoded_title.data(), count, input->lpstrFileTitle); input->lpstrFileTitle[count] = 0;
        }
        input->nFileOffset = static_cast<WORD>(offset); input->nFileExtension = static_cast<WORD>(ext_offset);
        input->nFilterIndex = filter_index;
        input->Flags &= ~OFN_EXTENSIONDIFFERENT;
        if (!extension.empty() && (!extension_offset || _wcsicmp(paths.front().c_str() + extension_offset, extension.c_str()))) input->Flags |= OFN_EXTENSIONDIFFERENT;
        legacy_dialog_error::SetBridge(0); result = TRUE; return true;
    } catch (...) {
        if (!shown) return decline(stage, E_UNEXPECTED);
        legacy_dialog_error::SetBridge(CDERR_DIALOGFAILURE); return true;
    }
}
}
HRESULT CreateSystemLegacyDialog(bool save, IUnknown** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    using Entry = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    static const Entry entry = []() -> Entry {
        wchar_t directory[32768]{};
        const UINT count = GetSystemDirectoryW(directory, 32768);
        if (!count || count >= 32768) return nullptr;
        // Retain the system module for the process lifetime, matching runtime hooks.
        const auto module = LoadLibraryExW((std::wstring(directory, count) + L"\\comdlg32.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<Entry>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    }();
    if (!entry) return CLASS_E_CLASSNOTAVAILABLE;
    Microsoft::WRL::ComPtr<IClassFactory> factory;
    HRESULT hr = entry(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, IID_PPV_ARGS(&factory));
    return FAILED(hr) ? hr : factory->CreateInstance(nullptr, IID_IUnknown, reinterpret_cast<void**>(output));
}
bool RunLegacyComDialog(OPENFILENAMEW* input, bool save, const LegacyDialogFactory& create, BOOL& result, const LegacyDiagnostic& diagnostic, bool verified_local_hook_host) { return Run(input, save, create, result, diagnostic, verified_local_hook_host); }
bool RunLegacyComDialog(OPENFILENAMEA* input, bool save, const LegacyDialogFactory& create, BOOL& result, const LegacyDiagnostic& diagnostic, bool verified_local_hook_host) { return Run(input, save, create, result, diagnostic, verified_local_hook_host); }
}
