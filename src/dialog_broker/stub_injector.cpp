#include "stub_injector.h"

#include <psapi.h>
#include <cstring>
#include <vector>

namespace pulse::dialog_broker {
namespace {

struct HandleGuard {
    HANDLE h = nullptr;
    ~HandleGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

struct RemoteMemGuard {
    HANDLE proc = nullptr;
    void* mem = nullptr;
    ~RemoteMemGuard() {
        if (proc && mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    }
};

HMODULE FindRemoteModuleBase(HANDLE proc, const std::wstring& dll_basename) noexcept {
    HMODULE mods[1024]{};
    DWORD bytes = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &bytes, LIST_MODULES_ALL)) return nullptr;
    const DWORD count = (bytes / sizeof(HMODULE)) < ARRAYSIZE(mods) ? (bytes / sizeof(HMODULE)) : ARRAYSIZE(mods);
    wchar_t name[MAX_PATH]{};
    for (DWORD i = 0; i < count; ++i) {
        const DWORD len = GetModuleBaseNameW(proc, mods[i], name, ARRAYSIZE(name));
        if (len > 0 && CompareStringOrdinal(name, static_cast<int>(len),
                                            dll_basename.data(), static_cast<int>(dll_basename.size()), TRUE) == CSTR_EQUAL) {
            return mods[i];
        }
    }
    return nullptr;
}

std::wstring BasenameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

} // namespace

DWORD FindExportRva(const std::wstring& dll_path, const char* export_name, DWORD& rva_out) noexcept {
    rva_out = 0;
    if (!export_name || !*export_name) return ERROR_INVALID_PARAMETER;
    HandleGuard file{CreateFileW(dll_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.h == INVALID_HANDLE_VALUE) return GetLastError();

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.h, &size) || size.QuadPart < sizeof(IMAGE_DOS_HEADER) || size.QuadPart > 64 * 1024 * 1024) {
        return ERROR_BAD_EXE_FORMAT;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
        return GetLastError();
    }

    const auto* base = bytes.data();
    const size_t total = bytes.size();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > total) {
        return ERROR_BAD_EXE_FORMAT;
    }

    const size_t nt_off = static_cast<size_t>(dos->e_lfanew);
    DWORD sig = 0;
    std::memcpy(&sig, base + nt_off, sizeof(sig));
    if (sig != IMAGE_NT_SIGNATURE) return ERROR_BAD_EXE_FORMAT;

    const auto* fh = reinterpret_cast<const IMAGE_FILE_HEADER*>(base + nt_off + sizeof(DWORD));
    const size_t opt_off = nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (opt_off + fh->SizeOfOptionalHeader > total) return ERROR_BAD_EXE_FORMAT;

    WORD opt_magic = 0;
    std::memcpy(&opt_magic, base + opt_off, sizeof(opt_magic));
    IMAGE_DATA_DIRECTORY exp_dir{};
    if (opt_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fh->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        const auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(base + opt_off);
        exp_dir = opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (opt_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && fh->SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        const auto* opt = reinterpret_cast<const IMAGE_OPTIONAL_HEADER32*>(base + opt_off);
        exp_dir = opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return ERROR_BAD_EXE_FORMAT;
    }

    if (exp_dir.VirtualAddress == 0 || exp_dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        return ERROR_PROC_NOT_FOUND;
    }

    const size_t sec_off = opt_off + fh->SizeOfOptionalHeader;
    if (sec_off + static_cast<size_t>(fh->NumberOfSections) * sizeof(IMAGE_SECTION_HEADER) > total) {
        return ERROR_BAD_EXE_FORMAT;
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + sec_off);

    auto rva_to_file = [&](DWORD rva, size_t need) -> const uint8_t* {
        for (WORD i = 0; i < fh->NumberOfSections; ++i) {
            const DWORD start = sections[i].VirtualAddress;
            const DWORD span = sections[i].SizeOfRawData;
            if (rva >= start && rva - start + need <= span) {
                const size_t off = static_cast<size_t>(sections[i].PointerToRawData) + (rva - start);
                if (off + need <= total) return base + off;
            }
        }
        return nullptr;
    };

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        rva_to_file(exp_dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)));
    if (!exports || exports->NumberOfNames == 0) return ERROR_PROC_NOT_FOUND;

    const auto* names = reinterpret_cast<const DWORD*>(
        rva_to_file(exports->AddressOfNames, static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD)));
    const auto* ordinals = reinterpret_cast<const WORD*>(
        rva_to_file(exports->AddressOfNameOrdinals, static_cast<size_t>(exports->NumberOfNames) * sizeof(WORD)));
    const auto* funcs = reinterpret_cast<const DWORD*>(
        rva_to_file(exports->AddressOfFunctions, static_cast<size_t>(exports->NumberOfFunctions) * sizeof(DWORD)));
    if (!names || !ordinals || !funcs) return ERROR_BAD_EXE_FORMAT;

    const size_t target_len = std::strlen(export_name);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        const auto* name_ptr = reinterpret_cast<const char*>(rva_to_file(names[i], target_len + 1));
        if (!name_ptr) continue;
        if (std::strcmp(name_ptr, export_name) == 0) {
            const WORD ord = ordinals[i];
            if (ord >= exports->NumberOfFunctions) return ERROR_BAD_EXE_FORMAT;
            const DWORD fn_rva = funcs[ord];
            // Reject forwarded exports.
            if (fn_rva >= exp_dir.VirtualAddress && fn_rva < exp_dir.VirtualAddress + exp_dir.Size) {
                return ERROR_BAD_EXE_FORMAT;
            }
            rva_out = fn_rva;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_PROC_NOT_FOUND;
}

DWORD FindRemoteExportRva(HANDLE process, HMODULE remote_mod, const char* export_name, DWORD& rva_out) noexcept {
    rva_out = 0;
    if (!process || !remote_mod || !export_name || !*export_name) return ERROR_INVALID_PARAMETER;

    const auto* base = reinterpret_cast<const uint8_t*>(remote_mod);
    auto read_exact = [&](size_t offset, void* dst, size_t size) -> bool {
        SIZE_T got = 0;
        return ReadProcessMemory(process, base + offset, dst, size, &got) && got == size;
    };

    IMAGE_DOS_HEADER dos{};
    if (!read_exact(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 4 * 1024 * 1024) {
        return ERROR_BAD_EXE_FORMAT;
    }

    const size_t nt_off = static_cast<size_t>(dos.e_lfanew);
    DWORD sig = 0;
    IMAGE_FILE_HEADER fh{};
    if (!read_exact(nt_off, &sig, sizeof(sig)) || sig != IMAGE_NT_SIGNATURE) return ERROR_BAD_EXE_FORMAT;
    if (!read_exact(nt_off + sizeof(DWORD), &fh, sizeof(fh))) return ERROR_BAD_EXE_FORMAT;

    const size_t opt_off = nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    WORD opt_magic = 0;
    if (!read_exact(opt_off, &opt_magic, sizeof(opt_magic))) return ERROR_BAD_EXE_FORMAT;

    IMAGE_DATA_DIRECTORY exp_dir{};
    DWORD size_of_image = 0;
    if (opt_magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && fh.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)) {
        IMAGE_OPTIONAL_HEADER64 opt{};
        if (!read_exact(opt_off, &opt, sizeof(opt))) return ERROR_BAD_EXE_FORMAT;
        exp_dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        size_of_image = opt.SizeOfImage;
    } else if (opt_magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && fh.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)) {
        IMAGE_OPTIONAL_HEADER32 opt{};
        if (!read_exact(opt_off, &opt, sizeof(opt))) return ERROR_BAD_EXE_FORMAT;
        exp_dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        size_of_image = opt.SizeOfImage;
    } else {
        return ERROR_BAD_EXE_FORMAT;
    }

    if (exp_dir.VirtualAddress == 0 || exp_dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        exp_dir.VirtualAddress >= size_of_image ||
        exp_dir.Size > size_of_image - exp_dir.VirtualAddress) {
        return ERROR_PROC_NOT_FOUND;
    }

    IMAGE_EXPORT_DIRECTORY exports{};
    if (!read_exact(exp_dir.VirtualAddress, &exports, sizeof(exports)) ||
        exports.NumberOfNames == 0 || exports.NumberOfNames > 65536 ||
        exports.NumberOfFunctions == 0 || exports.NumberOfFunctions > 65536) {
        return ERROR_PROC_NOT_FOUND;
    }

    const size_t names_bytes = static_cast<size_t>(exports.NumberOfNames) * sizeof(DWORD);
    const size_t ords_bytes = static_cast<size_t>(exports.NumberOfNames) * sizeof(WORD);
    const size_t funcs_bytes = static_cast<size_t>(exports.NumberOfFunctions) * sizeof(DWORD);
    if (exports.AddressOfNames >= size_of_image || names_bytes > size_of_image - exports.AddressOfNames ||
        exports.AddressOfNameOrdinals >= size_of_image || ords_bytes > size_of_image - exports.AddressOfNameOrdinals ||
        exports.AddressOfFunctions >= size_of_image || funcs_bytes > size_of_image - exports.AddressOfFunctions) {
        return ERROR_BAD_EXE_FORMAT;
    }

    std::vector<DWORD> names(exports.NumberOfNames);
    std::vector<WORD> ordinals(exports.NumberOfNames);
    std::vector<DWORD> funcs(exports.NumberOfFunctions);
    if (!read_exact(exports.AddressOfNames, names.data(), names_bytes) ||
        !read_exact(exports.AddressOfNameOrdinals, ordinals.data(), ords_bytes) ||
        !read_exact(exports.AddressOfFunctions, funcs.data(), funcs_bytes)) {
        return ERROR_BAD_EXE_FORMAT;
    }

    const size_t target_len = std::strlen(export_name);
    if (target_len >= 256) return ERROR_INVALID_PARAMETER;
    char name_buf[257]{};
    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        const DWORD name_rva = names[i];
        if (name_rva == 0 || name_rva >= size_of_image || target_len + 1 > size_of_image - name_rva) continue;
        if (!read_exact(name_rva, name_buf, target_len + 1)) continue;
        if (std::memcmp(name_buf, export_name, target_len + 1) == 0) {
            const WORD ord = ordinals[i];
            if (ord >= exports.NumberOfFunctions) return ERROR_BAD_EXE_FORMAT;
            const DWORD fn_rva = funcs[ord];
            if (fn_rva >= exp_dir.VirtualAddress && fn_rva < exp_dir.VirtualAddress + exp_dir.Size) {
                return ERROR_BAD_EXE_FORMAT;
            }
            if (fn_rva == 0 || fn_rva >= size_of_image) return ERROR_BAD_EXE_FORMAT;
            rva_out = fn_rva;
            return ERROR_SUCCESS;
        }
    }
    return ERROR_PROC_NOT_FOUND;
}

InjectionResult InjectSameArchStub(DWORD pid, const std::wstring& stub_dll_path,
                                   uint32_t activate_flags, DWORD timeout_ms) noexcept {
    InjectionResult out{};
    if (pid <= 4 || stub_dll_path.empty()) {
        out.error = ERROR_INVALID_PARAMETER;
        return out;
    }

    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HandleGuard proc{OpenProcess(access, FALSE, pid)};
    if (!proc.h) {
        out.error = GetLastError();
        return out;
    }

    const std::wstring base_name = BasenameOf(stub_dll_path);
    HMODULE remote_mod = FindRemoteModuleBase(proc.h, base_name);
    if (remote_mod) {
        out.already_loaded = true;
    } else {
        const size_t bytes = (stub_dll_path.size() + 1) * sizeof(wchar_t);
        RemoteMemGuard remote_str{proc.h, VirtualAllocEx(proc.h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)};
        if (!remote_str.mem) {
            out.error = GetLastError();
            return out;
        }
        SIZE_T written = 0;
        if (!WriteProcessMemory(proc.h, remote_str.mem, stub_dll_path.c_str(), bytes, &written) || written != bytes) {
            out.error = GetLastError();
            return out;
        }

        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto load_lib = k32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW")) : nullptr;
        if (!load_lib) {
            out.error = ERROR_PROC_NOT_FOUND;
            return out;
        }

        HandleGuard loader{CreateRemoteThread(proc.h, nullptr, 0, load_lib, remote_str.mem, 0, nullptr)};
        if (!loader.h) {
            out.error = GetLastError();
            return out;
        }
        const DWORD wait = WaitForSingleObject(loader.h, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            out.error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
            return out;
        }
        remote_mod = FindRemoteModuleBase(proc.h, base_name);
        if (!remote_mod) {
            out.error = ERROR_MOD_NOT_FOUND;
            return out;
        }
    }

    // Resolve PulseStubActivate directly from the remote process's mapped image so
    // an upgraded/renamed Stub (.old.<tick>) still invokes its exact in-memory export.
    DWORD activate_rva = 0;
    DWORD rva_err = FindRemoteExportRva(proc.h, remote_mod, "PulseStubActivate", activate_rva);
    if (rva_err != ERROR_SUCCESS && !out.already_loaded) {
        rva_err = FindExportRva(stub_dll_path, "PulseStubActivate", activate_rva);
    }
    if (rva_err != ERROR_SUCCESS) {
        out.error = rva_err;
        return out;
    }

    // Call PulseStubActivate(activate_flags) inside the remote process.
    const auto entry_addr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<uint8_t*>(remote_mod) + activate_rva);
    void* arg = reinterpret_cast<void*>(static_cast<uintptr_t>(activate_flags));
    HandleGuard act_thread{CreateRemoteThread(proc.h, nullptr, 0, entry_addr, arg, 0, nullptr)};
    if (!act_thread.h) {
        out.error = GetLastError();
        return out;
    }
    const DWORD wait = WaitForSingleObject(act_thread.h, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        out.error = (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : GetLastError();
        return out;
    }
    DWORD exit_code = 0;
    if (GetExitCodeThread(act_thread.h, &exit_code)) {
        out.activate_result = exit_code;
    }
    out.error = ERROR_SUCCESS;
    return out;
}

InjectionResult InjectViaHelper32(const std::wstring& helper32_exe, DWORD pid,
                                  const std::wstring& stub32_dll_path,
                                  uint32_t activate_flags, DWORD timeout_ms) noexcept {
    InjectionResult out{};
    if (helper32_exe.empty() || stub32_dll_path.empty() || pid <= 4) {
        out.error = ERROR_INVALID_PARAMETER;
        return out;
    }

    std::wstring cmd = L"\"" + helper32_exe + L"\" --pid " + std::to_wstring(pid) +
                       L" --dll \"" + stub32_dll_path + L"\" --flags " + std::to_wstring(activate_flags);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helper32_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        out.error = GetLastError();
        return out;
    }
    HandleGuard proc{pi.hProcess};
    HandleGuard thr{pi.hThread};
    const DWORD wait = WaitForSingleObject(proc.h, timeout_ms);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(proc.h, ERROR_TIMEOUT);
        out.error = ERROR_TIMEOUT;
        return out;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(proc.h, &code)) {
        out.error = code;
    }
    return out;
}

} // namespace pulse::dialog_broker
