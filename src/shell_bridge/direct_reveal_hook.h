#pragma once
#include <windows.h>
#include "../common/direct_reveal_bootstrap_diagnostic.h"
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include "../common/direct_reveal_client.h"
namespace pulse::shell_bridge::runtime {
enum class DirectShellAction { None, Reveal, OpenFolder };
// Only ordinary filesystem selection / local folder-open shapes; unsupported semantics stay native.
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
inline pulse::DirectRevealResult DefaultDirectRevealSend(const std::wstring& path, DWORD timeout) noexcept {
    return pulse::TryDirectReveal(path, timeout);
}
inline pulse::DirectRevealResult DefaultDirectOpenFolderSend(const std::wstring& path, DWORD timeout) noexcept {
    return pulse::TryDirectOpenFolder(path, timeout);
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
bool IsDirectRevealHookInstalled() noexcept;
LONG InstallDirectRevealHook(HMODULE owner, DirectRevealGate gate, DirectRevealObserver observer = nullptr,
    DirectRevealSender sender = &DefaultDirectRevealSend,
    pulse::DirectRevealInstallDiagnostic* diagnostic = nullptr,
    DirectRevealSender open_sender = nullptr) noexcept;
}
