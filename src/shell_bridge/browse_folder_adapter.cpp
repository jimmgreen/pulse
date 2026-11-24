#include "browse_folder_adapter.h"
#include <wrl/client.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>

namespace pulse::shell_bridge {
namespace {
using Microsoft::WRL::ComPtr;
struct ShellMemory {
    void* value = nullptr;
    ~ShellMemory() { CoTaskMemFree(value); }
};
std::string Ansi(const std::wstring& text) {
    const bool utf8 = GetACP() == CP_UTF8;
    const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL lost = FALSE;
    const int count = WideCharToMultiByte(CP_ACP, flags, text.c_str(), -1, nullptr, 0, nullptr, utf8 ? nullptr : &lost);
    if (!count || lost) throw E_INVALIDARG;
    std::string result(count, 0);
    if (!WideCharToMultiByte(CP_ACP, flags, text.c_str(), -1, result.data(), count, nullptr, utf8 ? nullptr : &lost) || lost) throw E_INVALIDARG;
    result.pop_back();
    return result;
}
std::wstring Wide(const wchar_t* value) { return value ? value : L""; }
std::wstring Wide(const char* value) {
    if (!value) return {};
    const int count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (!count) throw E_INVALIDARG;
    std::wstring result(count, 0);
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value, -1, result.data(), count)) throw E_INVALIDARG;
    result.pop_back();
    if (Ansi(result) != value) throw E_INVALIDARG;
    return result;
}
template<class Input> bool Run(Input* input, const BrowseFolderFactory& create,
    PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer) {
    BrowseFolderDiagnostic detail;
    const auto report = [&](const char* reason, HRESULT error = S_OK) noexcept {
        detail.reason = reason; detail.error = error;
        try { if (observer) observer(detail); } catch (...) {}
    };
    if (!input) { report("browse-decline-null-input"); return false; }
    detail.flags = input->ulFlags;
    detail.presence = (input->hwndOwner ? 1u : 0u) | (input->pidlRoot ? 2u : 0u) |
        (input->pszDisplayName ? 4u : 0u) | (input->lpszTitle ? 8u : 0u) |
        (input->lpfn ? 16u : 0u) | (input->lParam ? 32u : 0u);
    constexpr UINT supported = BIF_RETURNONLYFSDIRS | BIF_USENEWUI | BIF_NONEWFOLDERBUTTON |
        BIF_STATUSTEXT | BIF_DONTGOBELOWDOMAIN | BIF_RETURNFSANCESTORS | BIF_NOTRANSLATETARGETS | BIF_UAHINT;
    const bool fs_or_modern = input->ulFlags == 0 || (input->ulFlags & (BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE)) != 0;
    if (!fs_or_modern || (input->ulFlags & ~supported) ||
        ((input->ulFlags & BIF_NONEWFOLDERBUTTON) && !(input->ulFlags & BIF_NEWDIALOGSTYLE)) ||
        ((input->ulFlags & BIF_STATUSTEXT) && !(input->ulFlags & BIF_NEWDIALOGSTYLE)) ||
        ((input->ulFlags & BIF_UAHINT) && !(input->ulFlags & BIF_NEWDIALOGSTYLE))) {
        report("browse-decline-flags"); return false;
    }
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    const HRESULT apartment_hr = CoGetApartmentType(&apartment, &qualifier);
    bool own_apartment = false;
    if (apartment_hr == CO_E_NOTINITIALIZED ||
        (SUCCEEDED(apartment_hr) && apartment == APTTYPE_MTA && qualifier == APTTYPEQUALIFIER_IMPLICIT_MTA)) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            report("browse-decline-apartment"); return false;
        }
        own_apartment = true;
    } else if (FAILED(apartment_hr) || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA)) {
        report("browse-decline-apartment"); return false;
    }
    struct ApartmentGuard {
        bool owned = false;
        ~ApartmentGuard() { if (owned) CoUninitialize(); }
    } apartment_guard{own_apartment};
    if (input->hwndOwner) {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(input->hwndOwner, &process);
        if (thread != GetCurrentThreadId() || process != GetCurrentProcessId()) {
            report("browse-decline-owner"); return false;
        }
    }
    bool shown = false;
    result = nullptr;
    try {
        const auto title = Wide(input->lpszTitle);
        ComPtr<IShellItem> root;
        if (input->pidlRoot && !ILIsEmpty(input->pidlRoot)) {
            SFGAOF attrs = 0;
            if (SHCreateItemFromIDList(input->pidlRoot, IID_PPV_ARGS(&root)) != S_OK ||
                root->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attrs) != S_OK ||
                attrs != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) {
                report("browse-decline-root"); return false;
            }
        }
        bool use_original = false;
        const BrowseFolderPolicy policy{input->lpfn, input->lParam, !!root,
            !!(input->ulFlags & BIF_NONEWFOLDERBUTTON), &use_original};
        ComPtr<IFileDialog> dialog;
        HRESULT hr = create(policy, &dialog);
        if (FAILED(hr) || !dialog) { report("browse-decline-create", FAILED(hr) ? hr : E_UNEXPECTED); return false; }
        DWORD options = FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
        if (input->ulFlags & BIF_NOTRANSLATETARGETS) options |= FOS_NODEREFERENCELINKS;
        hr = dialog->SetOptions(options);
        if (SUCCEEDED(hr) && input->lpszTitle) hr = dialog->SetTitle(title.c_str());
        if (SUCCEEDED(hr) && root) {
            ComPtr<IFileDialog2> rooted;
            hr = dialog.As(&rooted);
            if (SUCCEEDED(hr)) hr = rooted->SetNavigationRoot(root.Get());
            if (hr == S_OK) hr = dialog->SetFolder(root.Get());
            if (hr != S_OK) { report("browse-decline-root", hr); return false; }
        }
        if (FAILED(hr)) { report("browse-decline-configuration", hr); return false; }
        report("browse-show-started");
        shown = true;
        hr = dialog->Show(input->hwndOwner);
        if (use_original) { report("browse-decline-exact-contract"); return false; }
        if (FAILED(hr)) { report(hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? "browse-cancelled" : "browse-show-failed", hr); return true; }
        ComPtr<IShellItem> item;
        hr = dialog->GetResult(&item);
        SFGAOF attributes = 0;
        if (SUCCEEDED(hr) && item) hr = item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
        if (FAILED(hr) || !item || (attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) {
            report("browse-result-not-filesystem-folder", FAILED(hr) ? hr : E_UNEXPECTED); return true;
        }
        ShellMemory pidl;
        hr = SHGetIDListFromObject(item.Get(), reinterpret_cast<PIDLIST_ABSOLUTE*>(&pidl.value));
        if (FAILED(hr) || !pidl.value) { report("browse-result-pidl-failed", FAILED(hr) ? hr : E_UNEXPECTED); return true; }
        if (root) {
            const auto selected = static_cast<PCIDLIST_ABSOLUTE>(pidl.value);
            if (!ILIsEqual(input->pidlRoot, selected) && !ILIsParent(input->pidlRoot, selected, FALSE)) {
                report("browse-result-outside-root", E_INVALIDARG); return true;
            }
        }
        ShellMemory name;
        hr = item->GetDisplayName(SIGDN_NORMALDISPLAY, reinterpret_cast<PWSTR*>(&name.value));
        if (FAILED(hr) || !name.value) { report("browse-result-name-failed", FAILED(hr) ? hr : E_UNEXPECTED); return true; }
        using C = std::remove_pointer_t<decltype(input->pszDisplayName)>;
        std::basic_string<C> output;
        if (input->pszDisplayName) {
            if constexpr (std::is_same_v<C, wchar_t>) output = static_cast<PCWSTR>(name.value);
            else output = Ansi(static_cast<PCWSTR>(name.value));
        }
        if (output.size() >= MAX_PATH) { report("browse-result-name-too-long", HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)); return true; }
        SHFILEINFOW information{};
        if (!SHGetFileInfoW(static_cast<PCWSTR>(pidl.value), 0, &information, sizeof(information), SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON)) {
            report("browse-result-icon-failed", E_FAIL); return true;
        }
        if (input->pszDisplayName) std::copy(output.c_str(), output.c_str() + output.size() + 1, input->pszDisplayName);
        input->iImage = information.iIcon;
        result = static_cast<PIDLIST_ABSOLUTE>(pidl.value); pidl.value = nullptr;
        report("browse-accepted-pidl");
        return true;
    } catch (HRESULT error) { report(shown ? "browse-result-encoding-failed" : "browse-decline-encoding", error); }
    catch (...) { report(shown ? "browse-terminal-exception" : "browse-decline-exception", E_FAIL); }
    return shown;
}
}
bool RunBrowseFolderDialog(BROWSEINFOW* input, const BrowseFolderFactory& create, PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer) {
    return Run(input, create, result, observer);
}
bool RunBrowseFolderDialog(BROWSEINFOA* input, const BrowseFolderFactory& create, PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer) {
    return Run(input, create, result, observer);
}
}
