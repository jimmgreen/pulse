#include "process_admission.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <algorithm>
#include <array>
#include <cwctype>

namespace pulse::dialog_broker {
namespace {

struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

constexpr std::array<std::wstring_view, 65> kBuiltInExcluded = {
    // Windows critical / core session processes
    L"system", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
    L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
    L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe", L"sihost.exe",
    L"taskhostw.exe", L"runtimebroker.exe", L"ctfmon.exe", L"conhost.exe",
    L"dllhost.exe", L"audiodg.exe", L"spoolsv.exe", L"searchindexer.exe",
    L"wmiprvse.exe", L"werfault.exe", L"werfaultsecure.exe", L"tiworker.exe",
    L"trustedinstaller.exe", L"msiexec.exe", L"consent.exe", L"logonui.exe",
    // Security / AV / EDR
    L"msmpeng.exe", L"mpdefendercoreservice.exe", L"nissrv.exe", L"securityhealthservice.exe",
    L"hipsdaemon.exe", L"hipstray.exe", L"usysdiag.exe",
    L"360tray.exe", L"360sd.exe", L"zhudongfangyu.exe",
    L"qqpctray.exe", L"qqpcrtp.exe", L"avp.exe", L"ekrn.exe", L"bdagent.exe",
    // Kernel anti-cheat engines
    L"beservice.exe", L"bedaisy.exe", L"easyanticheat.exe", L"easyanticheat_eos.exe",
    L"vgc.exe", L"vgtray.exe", L"ace-guard.exe", L"sguard64.exe", L"sguardsvc64.exe",
    // Pulse's own binaries (never self-inject)
    L"pulse.exe", L"pulse_shell.exe", L"pulse_elevated.exe", L"pulse.index.exe",
    L"pulse.preview.exe", L"pulse_7zip_worker.exe", L"pulse_hotkey_broker.exe",
    L"pulsedialogbroker.exe", L"pulsedialogoverlay.exe", L"pulsedialoglauncher.exe",
    L"pulsedialoginject32.exe", L"pulsedialogadminauto.exe",
};

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ExtractBasename(std::wstring_view full_path) {
    const auto slash = full_path.find_last_of(L"\\/");
    return std::wstring(slash == std::wstring_view::npos ? full_path : full_path.substr(slash + 1));
}

bool QueryIsProtectedProcess(HANDLE process) noexcept {
    using NtQueryInfoFn = NTSTATUS (NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto nt_query = []() -> NtQueryInfoFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<NtQueryInfoFn>(GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
    }();
    if (!nt_query) return false;

    // ProcessProtectionInformation = 61; PS_PROTECTION is a single BYTE (Type:3, Audit:1, Signer:4).
    constexpr ULONG kProcessProtectionInformation = 61;
    BYTE protection = 0;
    ULONG returned = 0;
    const NTSTATUS st = nt_query(process, kProcessProtectionInformation, &protection, sizeof(protection), &returned);
    return st == 0 && (protection & 0x07u) != 0;
}

} // namespace

bool IsBuiltInExcludedProcess(std::wstring_view basename) noexcept {
    if (basename.empty()) return true;
    for (const auto item : kBuiltInExcluded) {
        if (EqualsIgnoreCase(basename, item)) return true;
    }
    return false;
}

bool ReadPeImageHeader(const std::wstring& image_path, WORD& machine_out, WORD& subsystem_out) noexcept {
    machine_out = 0;
    subsystem_out = 0;
    HandleGuard file{CreateFileW(image_path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.h == INVALID_HANDLE_VALUE) return false;

    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    if (!ReadFile(file.h, &dos, sizeof(dos), &read, nullptr) || read != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 4 * 1024 * 1024) {
        return false;
    }

    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    if (!SetFilePointerEx(file.h, offset, nullptr, FILE_BEGIN)) return false;

    DWORD pe_sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (!ReadFile(file.h, &pe_sig, sizeof(pe_sig), &read, nullptr) || read != sizeof(pe_sig) ||
        pe_sig != IMAGE_NT_SIGNATURE) {
        return false;
    }
    if (!ReadFile(file.h, &fh, sizeof(fh), &read, nullptr) || read != sizeof(fh)) return false;

    WORD opt_magic = 0;
    if (!ReadFile(file.h, &opt_magic, sizeof(opt_magic), &read, nullptr) || read != sizeof(opt_magic)) return false;

    // Subsystem sits at offset 68 inside both IMAGE_OPTIONAL_HEADER32 and IMAGE_OPTIONAL_HEADER64.
    LARGE_INTEGER sub_offset{};
    sub_offset.QuadPart = static_cast<LONGLONG>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + 68;
    if (!SetFilePointerEx(file.h, sub_offset, nullptr, FILE_BEGIN)) return false;
    WORD subsystem = 0;
    if (!ReadFile(file.h, &subsystem, sizeof(subsystem), &read, nullptr) || read != sizeof(subsystem)) return false;

    if ((fh.Machine == IMAGE_FILE_MACHINE_AMD64 && opt_magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) ||
        (fh.Machine == IMAGE_FILE_MACHINE_I386 && opt_magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)) {
        return false;
    }
    machine_out = fh.Machine;
    subsystem_out = subsystem;
    return true;
}

ProcessCharacteristics InspectLiveProcess(DWORD pid) noexcept {
    ProcessCharacteristics info{};
    info.pid = pid;
    if (pid <= 4 || pid == GetCurrentProcessId()) return info;

    if (!ProcessIdToSessionId(pid, &info.session_id)) {
        info.session_id = 0;
        return info;
    }

    HandleGuard proc{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!proc.h) return info;

    FILETIME create_ft{}, exit_ft{}, kernel_ft{}, user_ft{};
    if (GetProcessTimes(proc.h, &create_ft, &exit_ft, &kernel_ft, &user_ft)) {
        ULARGE_INTEGER uli{};
        uli.LowPart = create_ft.dwLowDateTime;
        uli.HighPart = create_ft.dwHighDateTime;
        info.creation_time = uli.QuadPart;
    }

    HandleGuard token{};
    if (OpenProcessToken(proc.h, TOKEN_QUERY, &token.h) && token.h) {
        TOKEN_ELEVATION elev{};
        DWORD elev_bytes = 0;
        if (GetTokenInformation(token.h, TokenElevation, &elev, sizeof(elev), &elev_bytes)) {
            info.is_elevated = (elev.TokenIsElevated != 0);
        }
    }

    wchar_t path_buf[32768]{};
    DWORD len = ARRAYSIZE(path_buf);
    if (QueryFullProcessImageNameW(proc.h, 0, path_buf, &len) && len > 0) {
        info.image_path.assign(path_buf, len);
        info.basename = ExtractBasename(info.image_path);
    }

    info.is_protected = QueryIsProtectedProcess(proc.h);

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig{};
    if (GetProcessMitigationPolicy(proc.h, ProcessSignaturePolicy, &sig, sizeof(sig))) {
        info.has_cig_ms_only = (sig.MicrosoftSignedOnly != 0);
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
    if (GetProcessMitigationPolicy(proc.h, ProcessDynamicCodePolicy, &dyn, sizeof(dyn))) {
        info.has_acg_no_dyn_code = (dyn.ProhibitDynamicCode != 0);
    }

    using IsWow64Process2Fn = BOOL (WINAPI*)(HANDLE, USHORT*, USHORT*);
    static auto fn_wow64_2 = []() -> IsWow64Process2Fn {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return k32 ? reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(k32, "IsWow64Process2")) : nullptr;
    }();
    USHORT proc_machine = 0;
    USHORT native_machine = 0;
    if (fn_wow64_2 && fn_wow64_2(proc.h, &proc_machine, &native_machine)) {
        info.machine = (proc_machine == IMAGE_FILE_MACHINE_UNKNOWN) ? native_machine : proc_machine;
    } else {
        BOOL wow64 = FALSE;
        if (IsWow64Process(proc.h, &wow64)) {
            info.machine = wow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
        }
    }

    WORD pe_machine = 0;
    WORD pe_subsystem = 0;
    if (!info.image_path.empty() && ReadPeImageHeader(info.image_path, pe_machine, pe_subsystem)) {
        if (!info.machine) info.machine = pe_machine;
        info.pe_subsystem = pe_subsystem;
    }

    // Best-effort check whether user32/comdlg32 or our Stub is already mapped (requires PROCESS_VM_READ).
    HandleGuard mod_proc{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid)};
    if (mod_proc.h) {
        HMODULE mods[512]{};
        DWORD bytes_needed = 0;
        if (EnumProcessModulesEx(mod_proc.h, mods, sizeof(mods), &bytes_needed, LIST_MODULES_ALL)) {
            const DWORD count = std::min<DWORD>(bytes_needed / sizeof(HMODULE), ARRAYSIZE(mods));
            wchar_t mod_name[MAX_PATH]{};
            for (DWORD i = 0; i < count; ++i) {
                const DWORD n = GetModuleBaseNameW(mod_proc.h, mods[i], mod_name, ARRAYSIZE(mod_name));
                if (!n) continue;
                std::wstring_view name(mod_name, n);
                if (EqualsIgnoreCase(name, L"PulseDialogStub64.dll") ||
                    EqualsIgnoreCase(name, L"PulseDialogStub32.dll")) {
                    info.already_has_stub = true;
                } else if (EqualsIgnoreCase(name, L"user32.dll") ||
                           EqualsIgnoreCase(name, L"comdlg32.dll") ||
                           EqualsIgnoreCase(name, L"shell32.dll")) {
                    info.has_gui_dll_loaded = true;
                }
            }
        }
    }

    return info;
}

bool QueryProcessCreationTime(DWORD pid, uint64_t& creation_time_out) noexcept {
    creation_time_out = 0;
    if (pid <= 4) return false;
    HandleGuard proc{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!proc.h) return false;
    FILETIME create_ft{}, exit_ft{}, kernel_ft{}, user_ft{};
    if (!GetProcessTimes(proc.h, &create_ft, &exit_ft, &kernel_ft, &user_ft)) return false;
    ULARGE_INTEGER uli{};
    uli.LowPart = create_ft.dwLowDateTime;
    uli.HighPart = create_ft.dwHighDateTime;
    creation_time_out = uli.QuadPart;
    return creation_time_out != 0;
}

std::vector<DWORD> EnumerateInteractiveProcessIds() noexcept {
    std::vector<DWORD> pids;
    try {
        HandleGuard snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
        if (!snap.h || snap.h == INVALID_HANDLE_VALUE) return pids;
        const DWORD self_pid = GetCurrentProcessId();
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snap.h, &entry)) return pids;
        do {
            const DWORD pid = entry.th32ProcessID;
            if (pid <= 4 || pid == self_pid) continue;
            DWORD session_id = 0;
            if (!ProcessIdToSessionId(pid, &session_id) || session_id == 0) continue;
            pids.push_back(pid);
        } while (Process32NextW(snap.h, &entry));
    } catch (...) {
        pids.clear();
    }
    return pids;
}

AdmissionDecision EvaluateProcessPolicy(const ProcessCharacteristics& info,
                                        const dialog_bridge_state::StateSnapshot* state) noexcept {
    if (info.pid <= 4 || info.basename.empty()) {
        return {AdmissionVerdict::SkipInvalidProcess, 0, "invalid_or_inaccessible_process"};
    }
    if (info.session_id == 0) {
        return {AdmissionVerdict::SkipSessionZero, 0, "session_zero_service"};
    }
    if (info.is_protected) {
        return {AdmissionVerdict::SkipProtectedProcess, 0, "protected_process_ppl"};
    }
    if (info.has_cig_ms_only) {
        return {AdmissionVerdict::SkipCigSignedOnly, 0, "cig_microsoft_signed_only"};
    }
    if (info.has_acg_no_dyn_code) {
        return {AdmissionVerdict::SkipAcgNoDynamicCode, 0, "acg_prohibit_dynamic_code"};
    }
    if (IsBuiltInExcludedProcess(info.basename)) {
        return {AdmissionVerdict::SkipBuiltInExcluded, 0, "builtin_system_av_anticheat"};
    }
    if (state && state->IsProcessExcluded(info.basename)) {
        return {AdmissionVerdict::SkipUserExcluded, 0, "user_excluded"};
    }
    if (info.already_has_stub) {
        return {AdmissionVerdict::SkipAlreadyLoaded, info.machine, "stub_already_loaded"};
    }
    if (info.machine != IMAGE_FILE_MACHINE_AMD64 && info.machine != IMAGE_FILE_MACHINE_I386) {
        return {AdmissionVerdict::SkipUnsupportedArch, info.machine, "unsupported_machine"};
    }
    // Skip pure console-only utilities (cmd/git/compilers) unless they have mapped GUI DLLs.
    if (info.pe_subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI && !info.has_gui_dll_loaded) {
        return {AdmissionVerdict::SkipConsoleOnlySubsystem, info.machine, "cui_without_gui_dll"};
    }
    return {AdmissionVerdict::Eligible, info.machine, "eligible"};
}

} // namespace pulse::dialog_broker
