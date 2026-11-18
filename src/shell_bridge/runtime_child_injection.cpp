#include "runtime_child_injection.h"
#include <detours.h>
#include <vector>
#include <cstdint>
#include <algorithm>
namespace pulse::shell_bridge::runtime {
namespace {
struct Protection { BYTE* address; SIZE_T size; DWORD value; };
struct Snapshot {
    BYTE* image = nullptr;
    std::vector<BYTE> bytes, verification;
    std::vector<Protection> protections;
};
bool Read(HANDLE process, const void* address, void* output, SIZE_T size) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, address, output, size, &read) && read == size;
}
bool Capture(HANDLE process, Snapshot& snapshot) {
    uintptr_t cursor = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (VirtualQueryEx(process, reinterpret_cast<void*>(cursor), &region, sizeof(region))) {
        const auto next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
        if (region.Type != MEM_IMAGE || region.State != MEM_COMMIT || region.BaseAddress != region.AllocationBase) continue;
        IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS headers{};
        auto* base = static_cast<BYTE*>(region.AllocationBase);
        if (!Read(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024 ||
            !Read(process, base + dos.e_lfanew, &headers, sizeof(headers)) || headers.Signature != IMAGE_NT_SIGNATURE) return false;
        if (headers.FileHeader.Characteristics & IMAGE_FILE_DLL) continue;
        if (snapshot.image) return false;
#ifdef _WIN64
        constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
        constexpr WORD machine = IMAGE_FILE_MACHINE_I386;
#endif
        if (headers.FileHeader.Machine != machine || headers.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
            headers.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR ||
            headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress ||
            headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].Size ||
            headers.OptionalHeader.SizeOfHeaders > 1024 * 1024 ||
            headers.OptionalHeader.SizeOfHeaders < static_cast<DWORD>(dos.e_lfanew) + sizeof(headers)) return false;
        snapshot.image = base;
        snapshot.bytes.resize(headers.OptionalHeader.SizeOfHeaders);
        snapshot.verification.resize(snapshot.bytes.size());
        if (!Read(process, base, snapshot.bytes.data(), snapshot.bytes.size())) return false;
        SIZE_T offset = 0;
        while (offset < snapshot.bytes.size()) {
            MEMORY_BASIC_INFORMATION part{};
            if (!VirtualQueryEx(process, base + offset, &part, sizeof(part)) || part.State != MEM_COMMIT ||
                part.AllocationBase != base || (part.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
            const SIZE_T remaining = static_cast<BYTE*>(part.BaseAddress) + part.RegionSize - (base + offset);
            const SIZE_T length = (std::min<SIZE_T>)(remaining, static_cast<SIZE_T>(snapshot.bytes.size()) - offset);
            if (!length) return false;
            snapshot.protections.push_back({base + offset, length, part.Protect});
            offset += length;
        }
    }
    return snapshot.image != nullptr;
}
bool Restore(HANDLE process, Snapshot& snapshot) {
    bool success = true;
    // Detours v4.0.1 uimports.cpp writes new import/IAT data only into newly
    // allocated memory. Existing-image writes are DOS/NT headers; protection
    // changes cover SizeOfHeaders. CLR writes are excluded before injection.
    for (const auto& part : snapshot.protections) {
        DWORD ignored = 0;
        if (!VirtualProtectEx(process, part.address, part.size, PAGE_READWRITE, &ignored)) success = false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, snapshot.image, snapshot.bytes.data(), snapshot.bytes.size(), &written) ||
        written != snapshot.bytes.size()) success = false;
    for (const auto& part : snapshot.protections) {
        DWORD ignored = 0;
        if (!VirtualProtectEx(process, part.address, part.size, part.value, &ignored)) success = false;
    }
    if (!Read(process, snapshot.image, snapshot.verification.data(), snapshot.verification.size()) ||
        snapshot.verification != snapshot.bytes) success = false;
    for (const auto& part : snapshot.protections) {
        MEMORY_BASIC_INFORMATION actual_region{};
        if (!VirtualQueryEx(process, part.address, &actual_region, sizeof(actual_region)) ||
            actual_region.Protect != part.value) success = false;
    }
    return success;
}
}
ChildInjectionResult InjectChildRuntime(HANDLE process, const char* dll, bool fail_after_update_for_test) noexcept {
    ChildInjectionResult result;
    try {
        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic_code{};
        if (!GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature)) ||
            !GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dynamic_code, sizeof(dynamic_code))) {
            result.error = GetLastError(); return result;
        }
        if (signature.MicrosoftSignedOnly || signature.StoreSignedOnly || signature.MitigationOptIn ||
            dynamic_code.ProhibitDynamicCode) { result.error = ERROR_NOT_SUPPORTED; return result; }
        Snapshot snapshot;
        if (!process || !dll || !*dll || !Capture(process, snapshot)) {
            result.error = ERROR_NOT_SUPPORTED; return result;
        }
        const char* libraries[]{dll};
        const BOOL updated = DetourUpdateProcessWithDll(process, libraries, 1);
        result.error = updated ? ERROR_SUCCESS : GetLastError();
        if (updated && !fail_after_update_for_test) { result.injected = true; return result; }
        if (updated || !result.error) result.error = ERROR_GEN_FAILURE;
        result.restored = Restore(process, snapshot);
        result.rollback_failed = !result.restored;
        // Unreferenced remote allocations may remain until process exit. Do not
        // free guessed regions or describe recovery as zero-residue injection.
        return result;
    } catch (...) { result.error = ERROR_NOT_ENOUGH_MEMORY; return result; }
}
}
