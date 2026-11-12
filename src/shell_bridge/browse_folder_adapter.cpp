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
template<class Input> bool Run(Input* input, const LegacyDialogFactory& create,
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
    // Roots constrain the entire namespace, not just the starting directory.
    // The picker provides both resizing and address/name editing for the two
    // ordinary modern styles. Their native W/A accept/cancel output contract is
    // covered by browse_folder_runtime_test --native-baseline. Callback/BFFM
    // messages and every other BIF behavior remain native.
    if (input->pidlRoot) { report("browse-decline-root"); return false; }
    if (input->lpfn) { report("browse-decline-callback"); return false; }
    if (input->ulFlags != BIF_RETURNONLYFSDIRS && input->ulFlags != (BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE) &&
        input->ulFlags != (BIF_RETURNONLYFSDIRS | BIF_USENEWUI)) { report("browse-decline-flags"); return false; }
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    if (FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
        (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA)) {
        report("browse-decline-apartment"); return false;
    }
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
        ComPtr<IFileDialog> dialog;
        HRESULT hr = create(false, &dialog);
        if (FAILED(hr) || !dialog) { report("browse-decline-create", FAILED(hr) ? hr : E_UNEXPECTED); return false; }
        constexpr DWORD options = FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
        hr = dialog->SetOptions(options);
        if (SUCCEEDED(hr) && input->lpszTitle) hr = dialog->SetTitle(title.c_str());
        if (FAILED(hr)) { report("browse-decline-configuration", hr); return false; }
        report("browse-show-started");
        shown = true;
        hr = dialog->Show(input->hwndOwner);
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
bool RunBrowseFolderDialog(BROWSEINFOW* input, const LegacyDialogFactory& create, PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer) {
    return Run(input, create, result, observer);
}
bool RunBrowseFolderDialog(BROWSEINFOA* input, const LegacyDialogFactory& create, PIDLIST_ABSOLUTE& result, const BrowseFolderObserver& observer) {
    return Run(input, create, result, observer);
}
}
