#pragma once
#include <windows.h>
#include "../common/direct_reveal_bootstrap_diagnostic.h"
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include "../common/direct_reveal_client.h"
namespace pulse::shell_bridge::runtime {
enum class DirectShellAction { None, Reveal, OpenFolder, Unsupported };
// Parsers resolve supported local browsing shapes; dispatchers distinguish unrelated
// operations from recognized-but-unsupported browsing before choosing any native path.
bool DirectRevealPath(PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY children,
    DWORD flags, std::wstring& path) noexcept;
bool DirectExplorerSelection(const wchar_t* file, const wchar_t* parameters, std::wstring& path) noexcept;
bool ResolveDirectShellTarget(const wchar_t* verb, const wchar_t* file, const wchar_t* parameters,
    const wchar_t* directory, DirectShellAction& action, std::wstring& path) noexcept;
bool ResolveDirectShellExecuteExTarget(const SHELLEXECUTEINFOW& info,
    DirectShellAction& action, std::wstring& path) noexcept;
bool ResolveDirectShellExecuteExATarget(const SHELLEXECUTEINFOA& info,
    DirectShellAction& action, std::wstring& path) noexcept;
bool ResolveDirectShellExecuteATarget(const char* verb, const char* file, const char* parameters,
    const char* directory, DirectShellAction& action, std::wstring& path) noexcept;
using DirectRevealGate = bool (*)() noexcept;
// Worker only, never DllMain. The pinned hook/trampoline lives to process exit;
// the gate is checked on every call, including after session revocation.
using DirectRevealObserver = void (*)(const char* entry, unsigned result) noexcept;
using DirectRevealSender = pulse::DirectRevealResult (*)(const std::wstring&, DWORD) noexcept;
pulse::DirectRevealResult DefaultDirectRevealSend(const std::wstring& path, DWORD timeout) noexcept;
pulse::DirectRevealResult DefaultDirectOpenFolderSend(const std::wstring& path, DWORD timeout) noexcept;
inline DWORD DirectRevealFailureError(pulse::DirectRevealResult result) noexcept {
    if (result == pulse::DirectRevealResult::Indeterminate) return ERROR_TIMEOUT;
    if (result == pulse::DirectRevealResult::Rejected) return ERROR_NOT_READY;
    return ERROR_NOT_SUPPORTED;
}
inline HRESULT CompleteDirectReveal(pulse::DirectRevealResult result) noexcept {
    if (result == pulse::DirectRevealResult::Accepted) return S_OK;
    const DWORD error = DirectRevealFailureError(result);
    SetLastError(error);
    return HRESULT_FROM_WIN32(error);
}
inline HINSTANCE CompleteDirectShellExecute(pulse::DirectRevealResult result) noexcept {
    if (result == pulse::DirectRevealResult::Accepted) return reinterpret_cast<HINSTANCE>(33);
    const INT_PTR shell_error = result == pulse::DirectRevealResult::Indeterminate ? SE_ERR_DDETIMEOUT :
        result == pulse::DirectRevealResult::Rejected ? SE_ERR_DDEFAIL : SE_ERR_NOASSOC;
    SetLastError(DirectRevealFailureError(result));
    return reinterpret_cast<HINSTANCE>(shell_error);
}
template<class Info> BOOL CompleteDirectShellExecuteEx(Info* info, pulse::DirectRevealResult result) noexcept {
    info->hProcess = nullptr;
    info->hInstApp = CompleteDirectShellExecute(result);
    return result == pulse::DirectRevealResult::Accepted ? TRUE : FALSE;
}
// Shared dispatchers usable by both Detours (PulseDialogAuto) and IAT/Detours (PulseFileDialogAutoRuntime).
pulse::DirectRevealResult DispatchDirectRevealPidl(PCIDLIST_ABSOLUTE folder, UINT count,
    PCUITEMID_CHILD_ARRAY children, DWORD flags, DirectRevealSender reveal_sender = &DefaultDirectRevealSend) noexcept;
pulse::DirectRevealResult DispatchDirectShellExecuteW(LPCWSTR verb, LPCWSTR file, LPCWSTR parameters,
    LPCWSTR directory, INT show, DirectRevealSender reveal_sender = &DefaultDirectRevealSend,
    DirectRevealSender open_sender = &DefaultDirectOpenFolderSend) noexcept;
pulse::DirectRevealResult DispatchDirectShellExecuteExW(const SHELLEXECUTEINFOW* info,
    DirectRevealSender reveal_sender = &DefaultDirectRevealSend,
    DirectRevealSender open_sender = &DefaultDirectOpenFolderSend) noexcept;
pulse::DirectRevealResult DispatchDirectShellExecuteA(LPCSTR verb, LPCSTR file, LPCSTR parameters,
    LPCSTR directory, INT show, DirectRevealSender reveal_sender = &DefaultDirectRevealSend,
    DirectRevealSender open_sender = &DefaultDirectOpenFolderSend) noexcept;
pulse::DirectRevealResult DispatchDirectShellExecuteExA(const SHELLEXECUTEINFOA* info,
    DirectRevealSender reveal_sender = &DefaultDirectRevealSend,
    DirectRevealSender open_sender = &DefaultDirectOpenFolderSend) noexcept;
// Non-Accepted known browsing uses the system Explorer image rather than the
// Directory association. Unrelated/bypass requests and unrepresentable contracts
// retain their original call. ANSI conversion must round-trip without substitution.
HRESULT FallbackDirectRevealPidl(decltype(&::SHOpenFolderAndSelectItems) original,
    PCIDLIST_ABSOLUTE folder, UINT count, PCUITEMID_CHILD_ARRAY children, DWORD flags) noexcept;
HINSTANCE FallbackShellExecuteW(pulse::DirectRevealResult result, decltype(&::ShellExecuteW) original,
    HWND window, LPCWSTR verb, LPCWSTR file, LPCWSTR parameters, LPCWSTR directory, INT show) noexcept;
HINSTANCE FallbackShellExecuteA(pulse::DirectRevealResult result, decltype(&::ShellExecuteA) original,
    HWND window, LPCSTR verb, LPCSTR file, LPCSTR parameters, LPCSTR directory, INT show) noexcept;
BOOL FallbackShellExecuteExW(pulse::DirectRevealResult result, decltype(&::ShellExecuteExW) original,
    SHELLEXECUTEINFOW* info) noexcept;
BOOL FallbackShellExecuteExA(pulse::DirectRevealResult result, decltype(&::ShellExecuteExA) original,
    SHELLEXECUTEINFOA* info) noexcept;
bool IsDirectRevealHookInstalled() noexcept;
bool IsExplorerProcessHookInstalled() noexcept;
LONG InstallDirectRevealHook(HMODULE owner, DirectRevealGate gate, DirectRevealObserver observer = nullptr,
    DirectRevealSender sender = &DefaultDirectRevealSend,
    pulse::DirectRevealInstallDiagnostic* diagnostic = nullptr,
    DirectRevealSender open_sender = nullptr) noexcept;
}
