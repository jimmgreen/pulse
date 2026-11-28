#include "direct_reveal_bootstrap.h"
#include "direct_reveal_targets.h"
#include "explorer_launch_origin.h"
#include "../common/direct_reveal_bootstrap_diagnostic.h"
#include "../common/current_user_security.h"
#include "../common/dialog_adaptive_attach.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_bridge_paths.h"
#include "../common/dialog_admin_trust.h"
#include "../common/process_token_restriction.h"
#include "../common/runtime_log.h"
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <map>
#include <cstdio>
#include <vector>
#include <utility>
#include <cstring>
namespace pulse::app {
namespace direct_reveal_bootstrap_detail {
// Applications whose "show in folder" runs in-process through explorer.exe
// /select or SHOpenFolderAndSelectItems: built-in names plus applications
// learned from reveals the Explorer takeover caught. Any local install location
// is accepted: loading the bridge grants a same-user target nothing it could
// not already send to Pulse, and every unsupported or failed reveal stays native.
bool RevealTargetName(std::wstring_view name) noexcept {
    return direct_reveal_targets::IsTarget(name);
}
bool AllowedImage(std::wstring_view path) noexcept {
    const bool drive = path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
    if (!drive || path.find(L"\\..\\") != path.npos || path.find(L"\\.\\") != path.npos ||
        path.find(L'/') != path.npos || path.find(L'\0') != path.npos) return false;
    return RevealTargetName(path.substr(path.find_last_of(L'\\') + 1));
}
bool EligibleIntegrity(DWORD current_il, DWORD target_il) noexcept {
    return current_il >= SECURITY_MANDATORY_MEDIUM_RID && target_il == current_il;
}
bool GeneralRevealCandidateImage(std::wstring_view path, std::wstring_view windows_dir, std::wstring_view install_root) noexcept {
    const bool drive = path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
    if (!drive || path.find(L"\\..\\") != path.npos || path.find(L"\\.\\") != path.npos ||
        path.find(L'/') != path.npos || path.find(L'\0') != path.npos) return false;
    return !pulse::dialog_adaptive::IsExcludedExecutable(std::wstring(path), std::wstring(windows_dir), std::wstring(install_root));
}
std::wstring InstallRootFromExecutable(std::wstring_view executable) {
    const std::filesystem::path image_directory = std::filesystem::path(executable).parent_path();
    const auto leaf = image_directory.filename().wstring();
    const bool digest_directory = leaf.size() == 64 && leaf.find_first_not_of(L"0123456789abcdefABCDEF") == leaf.npos;
    if (digest_directory) {
        const auto family = image_directory.parent_path().filename().wstring();
        if (_wcsicmp(family.c_str(), L"DialogBridge") == 0 ||
            _wcsicmp(family.c_str(), dialog_bundle::kAppRuntimeDirectory) == 0) {
            return image_directory.parent_path().parent_path().wstring();
        }
    }
    return image_directory.wstring();
}
}
namespace {
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (*this) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept { if (*this) CloseHandle(value); value = std::exchange(other.value, nullptr); return *this; }
    explicit operator bool() const noexcept { return value && value != INVALID_HANDLE_VALUE; }
};
DWORD Last() noexcept { const DWORD error = GetLastError(); return error ? error : ERROR_GEN_FAILURE; }
void Log(const char* stage, DWORD pid, DWORD error) noexcept {
    diagnostics::runtime::Event(stage, {{"target_pid", pid}, {"error", error}});
}
std::wstring Image(HANDLE process) {
    std::wstring path(32768, 0); DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size); return path;
}
std::wstring ProgramFiles() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &path))) return {};
    std::wstring result(path); CoTaskMemFree(path); return result;
}
bool SamePath(const std::wstring& a, const std::wstring& b) {
    const wchar_t* left = a.starts_with(L"\\\\?\\") ? a.c_str() + 4 : a.c_str();
    const wchar_t* right = b.starts_with(L"\\\\?\\") ? b.c_str() + 4 : b.c_str();
    return _wcsicmp(left, right) == 0;
}
DWORD Integrity(HANDLE process, DWORD& rid) {
    Handle token; if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return Last();
    if (const DWORD error = security::CheckUnrestrictedToken(token.value)) return error;
    DWORD container = 0, size = 0;
    if (!GetTokenInformation(token.value, TokenIsAppContainer, &container, sizeof(container), &size)) return Last();
    if (container) return ERROR_ACCESS_DENIED;
    alignas(TOKEN_MANDATORY_LABEL) BYTE data[1024]{};
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, data, sizeof(data), &size)) return Last();
    const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data)->Label.Sid;
    if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) return ERROR_INVALID_SID;
    rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1); return 0;
}
DWORD Eligible(HANDLE process, DWORD pid) {
    DWORD ours = 0, theirs = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ours) || !ProcessIdToSessionId(pid, &theirs)) return Last();
    if (ours != theirs) return ERROR_ACCESS_DENIED;
    const auto sid = CurrentUserSidString();
    if (sid.empty() || sid != ProcessUserSidString(process)) return ERROR_ACCESS_DENIED;
    DWORD current_il = 0, target_il = 0;
    if (DWORD error = Integrity(GetCurrentProcess(), current_il)) return error;
    if (DWORD error = Integrity(process, target_il)) return error;
    if (!direct_reveal_bootstrap_detail::EligibleIntegrity(current_il, target_il)) return ERROR_ACCESS_DENIED;
    using MachineFn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto machine = reinterpret_cast<MachineFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (!machine) return ERROR_NOT_SUPPORTED;
    USHORT target = 0, native = 0;
    if (!machine(process, &target, &native)) return Last();
    if ((target ? target : native) != IMAGE_FILE_MACHINE_AMD64) return ERROR_EXE_MACHINE_TYPE_MISMATCH;
    // GetProcessInformation is Windows 8+ and must not be a static import (Windows 8.1 startup guard).
    const auto query_protection = reinterpret_cast<decltype(&GetProcessInformation)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessInformation"));
    if (!query_protection) return ERROR_NOT_SUPPORTED;
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (!query_protection(process, ProcessProtectionLevelInfo, &protection, sizeof(protection))) return Last();
    return protection.ProtectionLevel == PROTECTION_LEVEL_NONE ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
struct Module { uintptr_t base; DWORD size; std::wstring path; };
DWORD ModulesOnce(DWORD pid, std::vector<Module>& modules) {
    modules.clear();
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snapshot) return Last();
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.value, &entry)) return Last();
    do {
        if (modules.size() >= 2048) return ERROR_TOO_MANY_NAMES;
        modules.push_back({reinterpret_cast<uintptr_t>(entry.modBaseAddr), entry.modBaseSize, entry.szExePath});
    } while (Module32NextW(snapshot.value, &entry));
    return GetLastError() == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : Last();
}
bool EarlyModuleError(DWORD error) noexcept {
    return error == ERROR_NO_MORE_FILES || error == ERROR_BAD_LENGTH || error == ERROR_PARTIAL_COPY || error == ERROR_MOD_NOT_FOUND;
}
DWORD Modules(DWORD pid, std::vector<Module>& modules) {
    DWORD error = ERROR_NO_MORE_FILES;
    // A newly created process may not have a loader module list yet. Retry only
    // transient snapshot states; access/security failures remain hard failures.
    for (unsigned attempt = 0; attempt != 20; ++attempt) {
        error = ModulesOnce(pid, modules);
        if (!EarlyModuleError(error)) return error;
        if (attempt == 0) Log("reveal_bootstrap_module_list_pending", pid, error);
        Sleep(25);
    }
    return error;
}
DWORD LoaderAddress(DWORD pid, uintptr_t& address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadLibraryW), &module)) return Last();
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, 32768)) return Last();
    const auto offset = reinterpret_cast<uintptr_t>(&LoadLibraryW) - reinterpret_cast<uintptr_t>(module);
    for (unsigned attempt = 0; attempt != 20; ++attempt) {
        std::vector<Module> modules;
        if (const DWORD error = Modules(pid, modules)) return error;
        for (const auto& item : modules) if (SamePath(item.path, path) && offset < item.size) { address = item.base + offset; return 0; }
        if (attempt == 0) Log("reveal_bootstrap_loader_pending", pid, ERROR_MOD_NOT_FOUND);
        Sleep(25);
    }
    return ERROR_MOD_NOT_FOUND;
}
// Read the trusted image's export RVA without running DllMain in Pulse or
// polluting its executable module loader state with DONT_RESOLVE_DLL_REFERENCES.
DWORD ExportRva(const std::wstring& dll, DWORD& rva) {
    HMODULE view = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    if (!view) return Last();
    struct Release { HMODULE value; ~Release() { FreeLibrary(value); } } release{view};
    const auto* base = reinterpret_cast<const BYTE*>(reinterpret_cast<uintptr_t>(view) & ~uintptr_t(3));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024) return ERROR_BAD_EXE_FORMAT;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return ERROR_BAD_EXE_FORMAT;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const auto bound = [size](DWORD at, size_t length) { return at < size && length <= size - at; };
    if (!bound(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY))) return ERROR_BAD_EXE_FORMAT;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    if (!bound(exports->AddressOfNames, size_t(exports->NumberOfNames) * 4) ||
        !bound(exports->AddressOfNameOrdinals, size_t(exports->NumberOfNames) * 2) ||
        !bound(exports->AddressOfFunctions, size_t(exports->NumberOfFunctions) * 4)) return ERROR_BAD_EXE_FORMAT;
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    constexpr char name[] = "PulseInitializeDirectReveal";
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        if (!bound(names[i], sizeof(name)) || memcmp(base + names[i], name, sizeof(name))) continue;
        if (ordinals[i] >= exports->NumberOfFunctions) return ERROR_BAD_EXE_FORMAT;
        rva = functions[ordinals[i]];
        if (!bound(rva, 1) || (rva >= directory.VirtualAddress && rva - directory.VirtualAddress < directory.Size)) return ERROR_BAD_EXE_FORMAT;
        return 0;
    }
    return ERROR_PROC_NOT_FOUND;
}
struct Work {
    Handle process, thread; DWORD pid = 0, export_rva = 0;
    std::wstring dll; void* remote = nullptr; bool attaching = false, reported_timeout = false;
#ifdef PULSE_DIRECT_REVEAL_BOOTSTRAP_TEST
    pulse::DirectRevealBootstrapDiagnostic* diagnostic = nullptr;
#endif
    ULONGLONG deadline = 0;
    ~Work() {
        // A still-running LoadLibrary thread owns its argument. On Pulse exit
        // leave that small allocation alive rather than freeing under the target.
        if (remote && (!thread || WaitForSingleObject(thread.value, 0) == WAIT_OBJECT_0)) VirtualFreeEx(process.value, remote, 0, MEM_RELEASE);
    }
};
DWORD Begin(Work& work) {
    std::vector<Module> loaded;
    if (const DWORD error = Modules(work.pid, loaded)) {
        Log("reveal_bootstrap_preload_modules_failed", work.pid, error); return error;
    }
    const auto name = std::filesystem::path(work.dll).filename().wstring();
    for (const auto& item : loaded) {
        if (_wcsicmp(std::filesystem::path(item.path).filename().c_str(), name.c_str()) == 0 && !SamePath(item.path, work.dll)) {
            // Never unload or chain a second generation into a running target.
            Log("reveal_bootstrap_restart_target_required", work.pid, ERROR_REVISION_MISMATCH);
            return ERROR_REVISION_MISMATCH;
        }
    }
    uintptr_t loader = 0;
    if (const DWORD error = LoaderAddress(work.pid, loader)) {
        Log("reveal_bootstrap_loader_address_failed", work.pid, error); return error;
    }
    const size_t bytes = (work.dll.size() + 1) * sizeof(wchar_t);
    work.remote = VirtualAllocEx(work.process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!work.remote) return Last();
    SIZE_T written = 0;
    if (!WriteProcessMemory(work.process.value, work.remote, work.dll.c_str(), bytes, &written) || written != bytes) return Last();
    work.thread = Handle(CreateRemoteThread(work.process.value, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loader), work.remote, 0, nullptr));
    if (!work.thread) return Last();
    work.deadline = GetTickCount64() + 10000; return 0;
}
// Returns false while a remote thread still owns state. Never cancels or kills it.
bool Poll(Work& work, bool enabled, DWORD& result) {
    if (WaitForSingleObject(work.thread.value, 0) != WAIT_OBJECT_0) {
        if (!work.reported_timeout && GetTickCount64() >= work.deadline) {
            Log("reveal_bootstrap_timeout", work.pid, ERROR_TIMEOUT); work.reported_timeout = true;
        }
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeThread(work.thread.value, &code)) { result = Last(); return true; }
#ifdef PULSE_DIRECT_REVEAL_BOOTSTRAP_TEST
    if (work.attaching && work.remote && work.diagnostic) {
        SIZE_T bytes = 0;
        if (!ReadProcessMemory(work.process.value, work.remote, work.diagnostic, sizeof(*work.diagnostic), &bytes) || bytes != sizeof(*work.diagnostic))
            std::printf("fixture diagnostics read error=%lu\n", Last());
    }
#endif
    if (work.remote) { VirtualFreeEx(work.process.value, work.remote, 0, MEM_RELEASE); work.remote = nullptr; }
    if (work.attaching) { result = code; return true; }
    if (!enabled) { result = ERROR_CANCELLED; return true; }
    std::vector<Module> modules;
    if (const DWORD error = Modules(work.pid, modules)) { result = error; return true; }
    for (const auto& module : modules) if (SamePath(module.path, work.dll) && work.export_rva < module.size) {
#ifdef PULSE_DIRECT_REVEAL_BOOTSTRAP_TEST
        if (work.diagnostic) {
            work.remote = VirtualAllocEx(work.process.value, nullptr, sizeof(*work.diagnostic), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            SIZE_T bytes = 0;
            if (!work.remote || !WriteProcessMemory(work.process.value, work.remote, work.diagnostic, sizeof(*work.diagnostic), &bytes) || bytes != sizeof(*work.diagnostic)) {
                result = Last(); return true;
            }
        }
#endif
        work.thread = Handle(CreateRemoteThread(work.process.value, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(module.base + work.export_rva), work.remote, 0, nullptr));
        if (!work.thread) { result = Last(); return true; }
        work.attaching = true; work.reported_timeout = false; work.deadline = GetTickCount64() + 10000;
        return false;
    }
    result = ERROR_MOD_NOT_FOUND; return true;
}
bool StartsWithDir(std::wstring_view path, std::wstring_view root) noexcept {
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (root.starts_with(L"\\\\?\\")) root.remove_prefix(4);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.remove_suffix(1);
    if (root.empty() || path.size() <= root.size()) return false;
    return _wcsnicmp(path.data(), root.data(), root.size()) == 0 && (path[root.size()] == L'\\' || path[root.size()] == L'/');
}
bool IsShellOpenImportName(const char* name) noexcept {
    return name && (
        std::strcmp(name, "SHOpenFolderAndSelectItems") == 0 ||
        std::strcmp(name, "ShellExecuteW") == 0 ||
        std::strcmp(name, "ShellExecuteExW") == 0 ||
        std::strcmp(name, "ShellExecuteA") == 0 ||
        std::strcmp(name, "ShellExecuteExA") == 0);
}
bool ReadRemoteChecked(HANDLE process, uintptr_t base, size_t image_size, size_t rva, void* out, size_t count) noexcept {
    if (rva > image_size || count > image_size - rva || base > UINTPTR_MAX - rva - count) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + rva), out, count, &copied) && copied == count;
}
bool ReadRemoteAscii(HANDLE process, uintptr_t base, size_t image_size, size_t rva, std::array<char, 128>& out) noexcept {
    out.fill('\0');
    if (rva >= image_size) return false;
    const size_t max_len = (std::min)(out.size() - 1, image_size - rva);
    if (!max_len) return false;
    SIZE_T copied = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + rva), out.data(), max_len, &copied) || !copied) {
        for (size_t i = 0; i < max_len; ++i) {
            if (!ReadRemoteChecked(process, base, image_size, rva + i, &out[i], 1)) return false;
            if (!out[i]) return true;
        }
        return false;
    }
    for (size_t i = 0; i < copied; ++i) if (!out[i]) return true;
    return false;
}
bool ModuleImportsShellOpen(HANDLE process, uintptr_t base) noexcept {
    IMAGE_DOS_HEADER dos{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base), &dos, sizeof(dos), &read) || read != sizeof(dos)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(base + static_cast<size_t>(dos.e_lfanew)), &nt, sizeof(nt), &read) || read != sizeof(nt)) return false;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage < sizeof(nt) ||
        nt.OptionalHeader.SizeOfImage > 512u * 1024u * 1024u) return false;
    const size_t size = nt.OptionalHeader.SizeOfImage;
    size_t total_thunks = 0;
    std::array<char, 128> name{};
    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        const auto imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress && imports.Size >= sizeof(IMAGE_IMPORT_DESCRIPTOR) &&
            imports.VirtualAddress < size && imports.Size <= size - imports.VirtualAddress &&
            imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) <= 2048) {
            for (size_t off = 0; off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size; off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
                IMAGE_IMPORT_DESCRIPTOR desc{};
                if (!ReadRemoteChecked(process, base, size, imports.VirtualAddress + off, &desc, sizeof(desc))) break;
                if (!desc.Name && !desc.FirstThunk && !desc.OriginalFirstThunk) break;
                const size_t int_rva = desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk;
                if (!int_rva || int_rva >= size) continue;
                for (size_t idx = 0; idx < 32768; ++idx) {
                    if (++total_thunks > 65536) return false;
                    IMAGE_THUNK_DATA64 thunk{};
                    if (!ReadRemoteChecked(process, base, size, int_rva + idx * sizeof(thunk), &thunk, sizeof(thunk))) break;
                    if (!thunk.u1.AddressOfData) break;
                    if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) continue;
                    const size_t name_rva = static_cast<size_t>(thunk.u1.AddressOfData) + sizeof(WORD);
                    if (ReadRemoteAscii(process, base, size, name_rva, name) && IsShellOpenImportName(name.data())) return true;
                }
            }
        }
    }
    if (nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) {
        const auto delay = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (delay.VirtualAddress && delay.Size >= sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) &&
            delay.VirtualAddress < size && delay.Size <= size - delay.VirtualAddress &&
            delay.Size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= 1024) {
            for (size_t off = 0; off + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) <= delay.Size; off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
                IMAGE_DELAYLOAD_DESCRIPTOR desc{};
                if (!ReadRemoteChecked(process, base, size, delay.VirtualAddress + off, &desc, sizeof(desc))) break;
                if (!desc.DllNameRVA && !desc.ImportNameTableRVA && !desc.ImportAddressTableRVA) break;
                if (!desc.Attributes.RvaBased || !desc.ImportNameTableRVA || desc.ImportNameTableRVA >= size) continue;
                for (size_t idx = 0; idx < 32768; ++idx) {
                    if (++total_thunks > 65536) return false;
                    IMAGE_THUNK_DATA64 thunk{};
                    if (!ReadRemoteChecked(process, base, size, desc.ImportNameTableRVA + idx * sizeof(thunk), &thunk, sizeof(thunk))) break;
                    if (!thunk.u1.AddressOfData) break;
                    if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) continue;
                    const size_t name_rva = static_cast<size_t>(thunk.u1.AddressOfData) + sizeof(WORD);
                    if (ReadRemoteAscii(process, base, size, name_rva, name) && IsShellOpenImportName(name.data())) return true;
                }
            }
        }
    }
    return false;
}
bool HasShellOpenImport(HANDLE process, const std::vector<Module>& modules, const std::wstring& windows_dir, const std::wstring& install_root) noexcept {
    size_t scanned = 0;
    for (const auto& item : modules) {
        if (item.path.empty() || StartsWithDir(item.path, windows_dir) || StartsWithDir(item.path, install_root)) continue;
        if (++scanned > 256) break;
        if (ModuleImportsShellOpen(process, item.base)) return true;
    }
    return false;
}
struct State { std::atomic<bool> enabled{false}; Handle changed{CreateEventW(nullptr, FALSE, FALSE, nullptr)}; };
DWORD WINAPI Worker(void* context) noexcept {
    auto& state = *static_cast<State*>(context);
    try {
        wchar_t win_buf[32768]{};
        const UINT win_len = GetWindowsDirectoryW(win_buf, 32768);
        const std::wstring windows_dir = (win_len && win_len < 32768) ? std::wstring(win_buf, win_len) : std::wstring();
        const auto program_files = ProgramFiles();
        const auto own_image = Image(GetCurrentProcess());
        const auto root = direct_reveal_bootstrap_detail::InstallRootFromExecutable(own_image);
        const auto protected_prefix = program_files + L"\\";
        const bool in_program_files = !program_files.empty() && root.size() > protected_prefix.size() &&
            _wcsnicmp(root.c_str(), protected_prefix.c_str(), protected_prefix.size()) == 0;
        std::wstring dll;
        std::wstring common_root;
        dialog_bundle::BundleLease bundle;
        DWORD error = ERROR_SUCCESS;
        const bool has_legacy_dialog_pointer =
            GetFileAttributesW((root + L"\\dialog-current.json").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (in_program_files && has_legacy_dialog_pointer) {
            dialog_bundle::BundleLease startup_bundle;
            error = dialog_bundle::ResolveStartupBundle(root, startup_bundle);
            if (!error && startup_bundle.Machine() != IMAGE_FILE_MACHINE_AMD64) error = ERROR_EXE_MACHINE_TYPE_MISMATCH;
            // Bind the running host to the current pointer (AppRuntime when split, or
            // DialogBridge on legacy installs): an old immutable generation must not
            // silently inject a newer/different generation into applications.
            if (!error && !SamePath(startup_bundle.Host(), own_image)) error = ERROR_REVISION_MISMATCH;
            if (!error) error = dialog_bundle::ResolveInstalledBundle(root, bundle);
            if (!error && bundle.Machine() != IMAGE_FILE_MACHINE_AMD64) error = ERROR_EXE_MACHINE_TYPE_MISMATCH;
            if (!error) error = dialog_admin::VerifyProtectedPath(root, true);
            if (!error && !SamePath(startup_bundle.Directory(), bundle.Directory())) {
                error = dialog_admin::VerifyProtectedPath(root + L"\\app-current.json", false);
                if (!error) error = dialog_admin::VerifyProtectedTree(startup_bundle.Directory());
            }
            if (!error) error = dialog_admin::VerifyProtectedPath(root + L"\\dialog-current.json", false);
            if (!error) error = dialog_admin::VerifyProtectedTree(bundle.Directory());
            if (!error) dll = bundle.Directory() + L"\\PulseDialogAuto64.dll";
        } else {
            if (in_program_files) {
                dialog_bundle::BundleLease startup_bundle;
                error = dialog_bundle::ResolveStartupBundle(root, startup_bundle);
                if (!error && startup_bundle.Machine() != IMAGE_FILE_MACHINE_AMD64) error = ERROR_EXE_MACHINE_TYPE_MISMATCH;
                if (!error && !SamePath(startup_bundle.Host(), own_image)) error = ERROR_REVISION_MISMATCH;
                if (!error) error = dialog_admin::VerifyProtectedPath(root, true);
                if (!error) error = dialog_admin::VerifyProtectedPath(root + L"\\app-current.json", false);
                if (!error) error = dialog_admin::VerifyProtectedTree(startup_bundle.Directory());
                if (error) { Log("reveal_bootstrap_bundle_rejected", 0, error); return error; }
            }
            // Modern AppRuntime-only installs (and non-C: installs) store the protected
            // Stub+Core bridge under C:\Program Files\Common Files\Pulse\DialogBridge.
            common_root = dialog_bridge_paths::CommonDialogBridgeRoot();
            if (common_root.empty() || dialog_admin::VerifyProtectedPath(common_root, true) != ERROR_SUCCESS) {
                Log("reveal_bootstrap_location_rejected", 0, ERROR_ACCESS_DENIED);
                return ERROR_ACCESS_DENIED;
            }
            dialog_bridge_paths::ActiveBridgePointer pointer;
            if (dialog_bridge_paths::ReadActiveBridgePointer(common_root, pointer) == ERROR_SUCCESS &&
                !pointer.core_dir_x64.empty()) {
                error = dialog_admin::VerifyProtectedBridgeLocation(common_root, pointer.core_dir_x64, IMAGE_FILE_MACHINE_AMD64);
                if (!error) {
                    const auto stub_path = common_root + L"\\" + dialog_bridge_paths::kStubDllName64;
                    const auto auto_path = pointer.core_dir_x64 + L"\\PulseDialogAuto64.dll";
                    dll = GetFileAttributesW(stub_path.c_str()) != INVALID_FILE_ATTRIBUTES ? stub_path : auto_path;
                }
            } else {
                error = dialog_bundle::ResolveInstalledBundle(common_root, bundle);
                if (!error && bundle.Machine() != IMAGE_FILE_MACHINE_AMD64) error = ERROR_EXE_MACHINE_TYPE_MISMATCH;
                if (!error) error = dialog_admin::VerifyProtectedBridgeLocation(common_root, bundle.Directory(), IMAGE_FILE_MACHINE_AMD64);
                if (!error) dll = bundle.Directory() + L"\\PulseDialogAuto64.dll";
            }
        }
        DWORD export_rva = 0;
        if (!error) error = ExportRva(dll, export_rva);
        if (error) { Log("reveal_bootstrap_bundle_rejected", 0, error); return error; }
        using IdentityKey = std::pair<DWORD, ULONGLONG>;
        struct GeneralMiss { unsigned attempts = 0; ULONGLONG retry_at = 0; };
        std::map<IdentityKey, unsigned> attempted;
        std::map<IdentityKey, GeneralMiss> general_misses;
        std::vector<std::unique_ptr<Work>> active;
        ULONGLONG next_scan = 0;
        for (;;) {
            const bool enabled = state.enabled.load(std::memory_order_acquire);
            for (auto it = active.begin(); it != active.end();) {
                DWORD result = 0;
                if (Poll(**it, enabled, result)) { Log("reveal_bootstrap_complete", (*it)->pid, result); it = active.erase(it); }
                else ++it;
            }
            if (enabled && GetTickCount64() >= next_scan && active.size() < 8 && attempted.size() < 8192) {
                const ULONGLONG scan_now = GetTickCount64();
                next_scan = scan_now + 500;
                if (general_misses.size() > 8192) general_misses.clear();
                Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
                PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
                if (snapshot && Process32FirstW(snapshot.value, &entry)) do {
                    // Transient `explorer.exe /select` launchers reveal who asked.
                    explorer_launch_origin::Observe(entry);
                    if (!entry.th32ProcessID || entry.th32ProcessID == GetCurrentProcessId()) continue;
                    const bool named_target = direct_reveal_bootstrap_detail::RevealTargetName(entry.szExeFile);
                    Handle probe(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
                    if (!probe) continue;
                    FILETIME created{}, exited{}, kernel{}, user{};
                    if (!GetProcessTimes(probe.value, &created, &exited, &kernel, &user)) continue;
                    const IdentityKey identity{entry.th32ProcessID, (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime};
                    if (const auto done = attempted.find(identity); done != attempted.end() && done->second >= 6) continue;
                    if (!named_target) {
                        const auto miss = general_misses.find(identity);
                        if (miss != general_misses.end() && (miss->second.attempts >= 3 || scan_now < miss->second.retry_at)) continue;
                    }
                    const auto path = Image(probe.value);
                    if (named_target) {
                        if (!direct_reveal_bootstrap_detail::AllowedImage(path)) {
                            attempted[identity] = 6;
                            Log("reveal_bootstrap_image_rejected", entry.th32ProcessID, ERROR_ACCESS_DENIED);
                            continue;
                        }
                    } else {
                        if (!direct_reveal_bootstrap_detail::GeneralRevealCandidateImage(path, windows_dir, root)) {
                            attempted[identity] = 6;
                            continue;
                        }
                        if (GetGuiResources(probe.value, GR_USEROBJECTS) == 0) {
                            auto& miss = general_misses[identity];
                            ++miss.attempts;
                            miss.retry_at = scan_now + (miss.attempts == 1 ? 5000ull : miss.attempts == 2 ? 20000ull : 60000ull);
                            continue;
                        }
                    }
                    error = Eligible(probe.value, entry.th32ProcessID);
                    if (error) {
                        attempted[identity] = 6;
                        if (named_target) Log("reveal_bootstrap_target_rejected", entry.th32ProcessID, error);
                        continue;
                    }
                    std::vector<Module> loaded;
                    const DWORD mod_error = Modules(entry.th32ProcessID, loaded);
                    if (mod_error) {
                        if (!EarlyModuleError(mod_error)) attempted[identity] = 6;
                        continue;
                    }
                    bool already_loaded = false;
                    for (const auto& item : loaded) {
                        if (SamePath(item.path, dll)) { already_loaded = true; break; }
                        // PulseFileDialogAutoRuntime64.dll and Stub+Core already hook ShellExecute*
                        // and SHOpenFolderAndSelectItems; avoid redundant double-injection.
                        const auto mod_name = std::filesystem::path(item.path).filename().wstring();
                        if (_wcsicmp(mod_name.c_str(), L"PulseFileDialogAutoRuntime64.dll") == 0 ||
                            _wcsicmp(mod_name.c_str(), dialog_bridge_paths::kStubDllName64) == 0 ||
                            _wcsicmp(mod_name.c_str(), dialog_bridge_paths::kCoreDllName64) == 0) {
                            already_loaded = true;
                            break;
                        }
                    }
                    if (already_loaded) {
                        attempted[identity] = 6;
                        general_misses.erase(identity);
                        continue;
                    }
                    if (!named_target) {
                        // Scan remote PE import tables with read-only access so unrelated
                        // GUI processes never receive VM_WRITE / CREATE_THREAD open handles.
                        Handle reader(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, entry.th32ProcessID));
                        FILETIME read_created{};
                        if (!reader || !GetProcessTimes(reader.value, &read_created, &exited, &kernel, &user) ||
                            CompareFileTime(&read_created, &created) != 0) continue;
                        if (!HasShellOpenImport(reader.value, loaded, windows_dir, root)) {
                            auto& miss = general_misses[identity];
                            ++miss.attempts;
                            miss.retry_at = scan_now + (miss.attempts == 1 ? 5000ull : miss.attempts == 2 ? 20000ull : 60000ull);
                            continue;
                        }
                    }
                    auto work = std::make_unique<Work>(); work->pid = entry.th32ProcessID;
                    work->process = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION |
                        PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, work->pid));
                    if (!work->process) continue;
                    FILETIME reopened{};
                    if (!GetProcessTimes(work->process.value, &reopened, &exited, &kernel, &user) ||
                        CompareFileTime(&reopened, &created) != 0) continue;
                    general_misses.erase(identity);
                    auto& attempts = attempted[identity];
                    if (attempts >= 6) continue;
                    ++attempts;
                    error = bundle ? dialog_bundle::RecheckDirectory(bundle)
                                   : dialog_admin::VerifyProtectedPath(common_root, true);
                    if (error) { attempts = 6; Log("reveal_bootstrap_target_rejected", work->pid, error); continue; }
                    if (!state.enabled.load(std::memory_order_acquire)) break;
                    work->dll = dll; work->export_rva = export_rva;
                    error = Begin(*work);
                    // Repeat only pre-thread loader readiness failures. Once a
                    // remote thread starts, Poll exclusively owns that identity.
                    if (!EarlyModuleError(error)) attempts = 6;
                    Log("reveal_bootstrap_load", work->pid, error);
                    if (!error) active.push_back(std::move(work));
                    if (active.size() >= 8) break;
                } while (Process32NextW(snapshot.value, &entry));
            }
            // Remote phase completion wakes immediately; only discovery is polled.
            HANDLE wait_handles[9]{state.changed.value};
            DWORD wait_count = 1;
            for (const auto& work : active) wait_handles[wait_count++] = work->thread.value;
            const ULONGLONG now = GetTickCount64();
            const DWORD delay = !enabled && active.empty() ? INFINITE :
                enabled && next_scan > now ? static_cast<DWORD>(next_scan - now) : 500;
            WaitForMultipleObjects(wait_count, wait_handles, FALSE, delay);
        }
    } catch (...) { Log("reveal_bootstrap_exception", 0, ERROR_NOT_ENOUGH_MEMORY); return ERROR_NOT_ENOUGH_MEMORY; }
}
}
void ConfigureDirectRevealBootstrap(bool enabled) noexcept {
#if defined(_M_X64)
    // Process-lifetime worker/state: no UI-thread joins and no freeing a live
    // remote argument when settings change. The receiver gate stops delivery.
    static State* state = []() noexcept -> State* {
        try {
            auto* value = new State;
            if (!value->changed) { delete value; return nullptr; }
            HANDLE thread = CreateThread(nullptr, 0, Worker, value, 0, nullptr);
            if (!thread) { delete value; return nullptr; }
            CloseHandle(thread); return value;
        } catch (...) { return nullptr; }
    }();
    if (state) { state->enabled.store(enabled, std::memory_order_release); SetEvent(state->changed.value); }
#else
    (void)enabled; // No cross-bitness loading: the x86 bootstrap is not implemented.
#endif
}
#ifdef PULSE_DIRECT_REVEAL_BOOTSTRAP_TEST
namespace direct_reveal_bootstrap_detail {
DWORD AttachFixture(DWORD pid, const wchar_t* dll) noexcept {
    try {
        Work work; work.pid = pid; work.dll = dll;
        pulse::DirectRevealBootstrapDiagnostic diagnostic;
        work.diagnostic = &diagnostic;
        work.process = Handle(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
        if (!work.process) return Last();
        DWORD error = ExportRva(work.dll, work.export_rva);
        std::printf("fixture ExportRva status=%lu rva=%lu\n", error, work.export_rva);
        if (!error) error = Begin(work);
        std::printf("fixture Begin status=%lu\n", error);
        if (error) return error;
        const ULONGLONG deadline = GetTickCount64() + 15000;
        while (!Poll(work, true, error)) { if (GetTickCount64() >= deadline) return ERROR_TIMEOUT; Sleep(10); }
        std::printf("fixture diagnostic stage=%lu token_reason=%lu token_error=%lu folder_hr=%08lx log_error=%lu install_stage=%lu install_error=%lu install_tid=%lu\n",
            diagnostic.stage, diagnostic.token_reason, diagnostic.token_error, diagnostic.folder_status, diagnostic.log_error,
            diagnostic.install.stage, diagnostic.install.error, diagnostic.install.thread_id);
        return error;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
}
#endif
}
