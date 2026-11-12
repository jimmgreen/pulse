#include "runtime_auto_imports.h"
#include "runtime_config.h"
#include <tlhelp32.h>
#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace pulse::shell_bridge::runtime {
namespace {
using auto_import_detail::ImportSlot;
constexpr size_t kMaxImage = 512 * 1024 * 1024, kMaxSlots = 16384;
constexpr const char* kNames[]{"CoCreateInstance", "CoCreateInstanceEx", "CoGetClassObject", "RoGetActivationFactory",
    "GetOpenFileNameW", "GetSaveFileNameW", "GetOpenFileNameA", "GetSaveFileNameA", "CommDlgExtendedError", "SHBrowseForFolderW", "SHBrowseForFolderA"};
constexpr uint32_t kMasks[]{kCoCreateInstance, kCoCreateInstanceEx, kCoGetClassObject, kRoGetActivationFactory,
    kGetOpenFileNameW, kGetSaveFileNameW, kGetOpenFileNameA, kGetSaveFileNameA, kCommDlgExtendedError, kSHBrowseForFolderW, kSHBrowseForFolderA};
#if defined(_M_X64)
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#else
constexpr WORD kMachine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
HRESULT BadImage() noexcept { return HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT); }
template<class Read> HRESULT Inspect(Read read, std::vector<ImportSlot>& output) {
    std::vector<ImportSlot> slots;
    IMAGE_DOS_HEADER dos{};
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024) return BadImage();
    IMAGE_NT_HEADERS nt{};
    if (!read(static_cast<size_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        kMachine == IMAGE_FILE_MACHINE_UNKNOWN || nt.FileHeader.Machine != kMachine ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC || nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.SizeOfImage < sizeof(nt) || nt.OptionalHeader.SizeOfImage > kMaxImage ||
        static_cast<size_t>(dos.e_lfanew) > nt.OptionalHeader.SizeOfImage - sizeof(nt)) return BadImage();
    const size_t size = nt.OptionalHeader.SizeOfImage;
    auto bounded = [&](size_t rva, void* buffer, size_t count) {
        return rva <= size && count <= size - rva && read(rva, buffer, count);
    };
    std::array<IMAGE_SECTION_HEADER, 96> sections{};
    if (!bounded(static_cast<size_t>(dos.e_lfanew) + sizeof(nt), sections.data(),
        static_cast<size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER))) return BadImage();
    auto data_slot = [&](size_t rva) {
        for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            const size_t length = section.Misc.VirtualSize;
            if (rva >= section.VirtualAddress && rva - section.VirtualAddress <= length &&
                sizeof(void*) <= length - (rva - section.VirtualAddress))
                return (section.Characteristics & IMAGE_SCN_MEM_READ) && !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE);
        }
        return false;
    };
    auto read_name = [&](size_t rva, std::array<char, 256>& name) {
        for (size_t i = 0; i < name.size(); ++i) {
            if (!bounded(rva + i, &name[i], 1)) return false;
            if (!name[i]) return true;
        }
        return false;
    };
    if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) { output.clear(); return S_OK; }
    const auto imports = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress && !imports.Size) { output.clear(); return S_OK; }
    if (!imports.VirtualAddress || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) || imports.VirtualAddress > size ||
        imports.Size > size - imports.VirtualAddress || imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) > 4096 ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IAT) return BadImage();
    const auto iat = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    bool ended = false;
    size_t total_thunks = 0;
    for (size_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        if (!bounded(imports.VirtualAddress + offset, &descriptor, sizeof(descriptor))) return BadImage();
        if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk && !descriptor.TimeDateStamp && !descriptor.ForwarderChain) { ended = true; break; }
        if (!descriptor.Name || !descriptor.FirstThunk) return BadImage();
        // An import without a name lookup table is not guessed from live addresses.
        if (!descriptor.OriginalFirstThunk) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (descriptor.FirstThunk > size || descriptor.OriginalFirstThunk > size) return BadImage();
        std::array<char, 256> name{};
        if (!read_name(descriptor.Name, name)) return BadImage();
        bool thunk_end = false;
        for (size_t index = 0; index < 65536; ++index) {
            if (++total_thunks > 131072) return BadImage();
            IMAGE_THUNK_DATA lookup{}, address{};
            const size_t delta = index * sizeof(lookup);
            if (delta > size - descriptor.OriginalFirstThunk || delta > size - descriptor.FirstThunk ||
                !bounded(static_cast<size_t>(descriptor.OriginalFirstThunk) + delta, &lookup, sizeof(lookup)) ||
                !bounded(static_cast<size_t>(descriptor.FirstThunk) + delta, &address, sizeof(address))) return BadImage();
            if (!lookup.u1.AddressOfData) { if (address.u1.Function) return BadImage(); thunk_end = true; break; }
            if (!address.u1.Function) return BadImage();
            if (IMAGE_SNAP_BY_ORDINAL(lookup.u1.Ordinal)) continue;
            if (lookup.u1.AddressOfData > size || size - static_cast<size_t>(lookup.u1.AddressOfData) < sizeof(WORD) ||
                !read_name(static_cast<size_t>(lookup.u1.AddressOfData) + sizeof(WORD), name)) return BadImage();
            for (uint32_t api = 0; api < std::size(kNames); ++api) if (std::strcmp(name.data(), kNames[api]) == 0) {
                const size_t rva = static_cast<size_t>(descriptor.FirstThunk) + delta;
                if (rva % alignof(void*) || !data_slot(rva) || !iat.VirtualAddress || iat.Size < sizeof(void*) ||
                    iat.VirtualAddress > size || iat.Size > size - iat.VirtualAddress ||
                    rva < iat.VirtualAddress || rva - iat.VirtualAddress > iat.Size - sizeof(void*) || slots.size() >= kMaxSlots) return BadImage();
                for (const auto& existing : slots) if (existing.rva == rva) return BadImage();
                slots.push_back({rva, api});
            }
        }
        if (!thunk_end) return BadImage();
    }
    if (!ended) return BadImage();
    output = std::move(slots);
    return S_OK;
}
bool ReadImage(HMODULE module, size_t rva, void* output, size_t count) noexcept {
    const auto base = reinterpret_cast<uintptr_t>(module);
    if (rva > kMaxImage || count > kMaxImage - rva || base > UINTPTR_MAX - rva - count) return false;
    size_t checked = 0;
    while (checked < count) {
        MEMORY_BASIC_INFORMATION memory{};
        const auto address = base + rva + checked;
        if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) || memory.AllocationBase != module ||
            memory.Type != MEM_IMAGE || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const size_t remaining = memory.RegionSize - (address - reinterpret_cast<uintptr_t>(memory.BaseAddress));
        if (!remaining) return false;
        checked += remaining < count - checked ? remaining : count - checked;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + rva), output, count, &copied) && copied == count;
}
std::wstring CanonicalPath(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const DWORD length = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::wstring canonical(length ? static_cast<size_t>(length) + 1 : 0, 0);
    const DWORD written = length ? GetFinalPathNameByHandleW(file, canonical.data(), static_cast<DWORD>(canonical.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    CloseHandle(file);
    if (!written || written >= canonical.size()) return {};
    canonical.resize(written);
    return canonical;
}
std::wstring ModulePath(HMODULE module) {
    std::wstring path(32768, 0);
    const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count);
    return CanonicalPath(path);
}
bool Under(const std::wstring& path, const std::wstring& directory) noexcept {
    return path.size() >= directory.size() && directory.size() <= INT_MAX &&
        CompareStringOrdinal(path.data(), static_cast<int>(directory.size()), directory.data(), static_cast<int>(directory.size()), TRUE) == CSTR_EQUAL;
}
}
namespace auto_import_detail {
// A foreign IAT writer may revoke write access after VirtualProtect succeeds.
// Module references prevent unload, but cannot serialize another tool's page
// protections. Treat this particular access race as a failed transaction.
static bool TryCompareExchange(void* volatile* slot, void* replacement, void* expected, void*& observed) noexcept {
    __try {
        observed = InterlockedCompareExchangePointer(slot, replacement, expected);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
HRESULT InspectImage(std::span<const std::byte> image, std::vector<ImportSlot>& slots) noexcept {
    slots.clear();
    try { return Inspect([&](size_t rva, void* output, size_t count) {
        if (rva > image.size() || count > image.size() - rva) return false;
        std::memcpy(output, image.data() + rva, count); return true;
    }, slots); } catch (...) { slots.clear(); return E_OUTOFMEMORY; }
}
HRESULT ChangeSlot(SlotChange& change, bool install, Protect protect) noexcept {
    if (!change.slot || reinterpret_cast<uintptr_t>(change.slot) % alignof(void*) || !protect) return E_INVALIDARG;
    auto restore = [&]() {
        if (!change.protection_pending) return true;
        DWORD ignored = 0;
        if (!protect(const_cast<void**>(change.slot), sizeof(void*), change.saved_protection, &ignored)) return false;
        change.protection_pending = false; return true;
    };
    if (!restore()) return HRESULT_FROM_WIN32(GetLastError());
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(const_cast<void**>(change.slot), &page, sizeof(page)) || page.State != MEM_COMMIT ||
        (page.Protect != PAGE_READONLY && page.Protect != PAGE_READWRITE && page.Protect != PAGE_WRITECOPY)) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    DWORD previous = 0;
    if (!protect(const_cast<void**>(change.slot), sizeof(void*), PAGE_READWRITE, &previous)) return HRESULT_FROM_WIN32(GetLastError());
    change.saved_protection = previous; change.protection_pending = true;
    if (previous != PAGE_READONLY && previous != PAGE_READWRITE && previous != PAGE_WRITECOPY) {
        restore(); return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    void* expected = install ? change.original : change.replacement;
    void* replacement = install ? change.replacement : change.original;
    void* observed = nullptr;
    if (!TryCompareExchange(change.slot, replacement, expected, observed)) {
        restore();
        return HRESULT_FROM_WIN32(ERROR_NOACCESS);
    }
    if (install) change.owned = observed == expected;
    else change.owned = false; // Foreign values are never overwritten or claimed.
    if (!restore()) {
        const DWORD error = GetLastError();
        if (install && observed == expected) {
            void* reverted = nullptr;
            if (TryCompareExchange(change.slot, change.original, change.replacement, reverted)) change.owned = false;
        }
        restore();
        return HRESULT_FROM_WIN32(error);
    }
    return observed == expected ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
}
}
struct AutoImportPatch::Impl {
    struct Slot { auto_import_detail::SlotChange change; uint32_t mask; HMODULE module; std::wstring path; };
    std::vector<HMODULE> modules;
    std::vector<Slot> slots;
    AutoImportResult result;
    bool committed = false;
    ~Impl() { for (auto module : modules) FreeLibrary(module); }
};
AutoImportPatch::AutoImportPatch() noexcept = default;
AutoImportPatch::~AutoImportPatch() { if (impl_ && !impl_->committed) Rollback(); }
AutoImportResult AutoImportPatch::Install(HMODULE owner, std::span<const AutoImportHook> hooks, Permit permit, void* context) noexcept {
    if (impl_) return {HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED)};
    try {
        impl_ = std::make_unique<Impl>();
        auto& state = *impl_;
        const auto fail = [&](HRESULT error) { state.result.status = error; return Rollback(); };
        if (!owner || (hooks.size() != 3 && hooks.size() != 4 && hooks.size() != std::size(kNames))) return fail(E_INVALIDARG);
        for (size_t index = 0; index < hooks.size(); ++index) {
            const auto& hook = hooks[index];
            if (index == 3 && hooks.size() == std::size(kNames) && !hook.original && !hook.replacement) continue;
            if (!hook.original || !hook.replacement || hook.original == hook.replacement) return fail(E_INVALIDARG);
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(hooks[0].replacement), &pinned) || pinned != owner) return fail(E_ACCESSDENIED);
        for (const auto& hook : hooks) {
            if (!hook.original) continue;
            HMODULE actual = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(hook.replacement), &actual) || actual != owner) return fail(E_ACCESSDENIED);
        }
        const auto own_path = ModulePath(owner);
        if (own_path.empty()) return fail(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        const auto own_directory = own_path.substr(0, own_path.find_last_of(L'\\') + 1);
        std::wstring windows(32768, 0);
        const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
        if (!length || length >= windows.size()) return fail(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        windows.resize(length); windows = CanonicalPath(windows);
        if (windows.empty()) return fail(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        windows += L"\\";
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) return fail(HRESULT_FROM_WIN32(GetLastError()));
        struct Snapshot { HANDLE value; ~Snapshot() { CloseHandle(value); } } close{snapshot};
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot, &entry)) return fail(HRESULT_FROM_WIN32(GetLastError()));
        size_t module_count = 0;
        do {
            if (++module_count > 4096) return fail(HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY));
            HMODULE retained = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(entry.modBaseAddr), &retained)) { ++state.result.skipped_modules; continue; }
            try { state.modules.push_back(retained); } catch (...) { FreeLibrary(retained); throw; }
            const auto path = ModulePath(retained);
            if (retained == owner || path.empty() || Under(path, own_directory) || Under(path, windows)) { ++state.result.skipped_modules; continue; }
            std::vector<ImportSlot> candidates;
            const HRESULT parsed = Inspect([&](size_t rva, void* buffer, size_t count) { return ReadImage(retained, rva, buffer, count); }, candidates);
            if (parsed == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) { ++state.result.skipped_modules; continue; }
            if (FAILED(parsed)) return fail(parsed);
            for (const auto& candidate : candidates) {
                if (candidate.api_index >= hooks.size() || !hooks[candidate.api_index].original) continue;
                void* current = nullptr;
                if (!ReadImage(retained, candidate.rva, &current, sizeof(current))) return fail(HRESULT_FROM_WIN32(ERROR_READ_FAULT));
                const auto& hook = hooks[candidate.api_index];
                if (current == hook.replacement) continue;
                if (current != hook.original) { ++state.result.foreign_slots; continue; }
                if (state.slots.size() >= kMaxSlots) return fail(E_OUTOFMEMORY);
                auto* slot = reinterpret_cast<void* volatile*>(reinterpret_cast<uintptr_t>(retained) + candidate.rva);
                state.slots.push_back({{slot, hook.original, hook.replacement}, kMasks[candidate.api_index], retained, path});
            }
        } while (Module32NextW(snapshot, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES) return fail(HRESULT_FROM_WIN32(GetLastError()));
        if (state.slots.empty()) return fail(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
        // All candidates/module references are collected before the first write.
        for (auto& slot : state.slots) {
            if (permit && !permit(context)) return fail(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED));
            const HRESULT changed = auto_import_detail::ChangeSlot(slot.change, true);
            if (FAILED(changed)) return fail(changed);
            state.result.api_mask |= slot.mask;
            ++state.result.patched_slots;
        }
        state.result.status = S_OK;
        return state.result;
    } catch (...) { if (impl_) { impl_->result.status = E_OUTOFMEMORY; return Rollback(); } return {E_OUTOFMEMORY}; }
}
AutoImportResult AutoImportPatch::Rollback() noexcept {
    if (!impl_) return {S_FALSE};
    auto& state = *impl_;
    if (state.committed) return state.result;
    for (auto it = state.slots.rbegin(); it != state.slots.rend(); ++it) {
        if (!it->change.owned && !it->change.protection_pending) continue;
        const HRESULT undone = auto_import_detail::ChangeSlot(it->change, false);
        if (FAILED(undone) && undone != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) state.result.status = undone;
    }
    state.result.api_mask = state.result.patched_slots = 0;
    state.result.protection_restored = true;
    for (const auto& slot : state.slots) {
        if (slot.change.owned) { state.result.api_mask |= slot.mask; ++state.result.patched_slots; }
        if (slot.change.protection_pending) state.result.protection_restored = false;
    }
    return state.result;
}
void AutoImportPatch::Commit() noexcept {
    if (impl_) {
        impl_->committed = true;
        for (auto module : impl_->modules) FreeLibrary(module);
        impl_->modules.clear();
    }
}
AutoImportResult AutoImportPatch::RestoreCommitted() noexcept {
    if (!impl_ || !impl_->committed) return Rollback();
    auto& state = *impl_;
    state.result.status = S_OK;
    for (auto it = state.slots.rbegin(); it != state.slots.rend(); ++it) {
        if (!it->change.owned && !it->change.protection_pending) continue;
        HMODULE retained = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(it->change.slot)), &retained)) { it->change.owned = false; continue; }
        bool matching = false;
        try { matching = retained == it->module && ModulePath(retained) == it->path; } catch (...) {}
        if (matching) {
            const HRESULT restored = auto_import_detail::ChangeSlot(it->change, false);
            if (FAILED(restored) && restored != HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) state.result.status = restored;
        } else it->change.owned = false;
        FreeLibrary(retained);
    }
    state.result.api_mask = state.result.patched_slots = 0;
    state.result.protection_restored = true;
    for (const auto& slot : state.slots) {
        if (slot.change.owned) { state.result.api_mask |= slot.mask; ++state.result.patched_slots; }
        if (slot.change.protection_pending) state.result.protection_restored = false;
    }
    return state.result;
}
AutoImportResult AutoImportPatch::CurrentCoverage() noexcept {
    if (!impl_) return {S_FALSE};
    auto result = impl_->result; result.api_mask = result.patched_slots = 0;
    for (auto& slot : impl_->slots) {
        if (!slot.change.owned) continue;
        HMODULE retained = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(slot.change.slot)), &retained)) {
            slot.change.owned = false; continue;
        }
        bool matching = false; void* value = nullptr;
        try {
            matching = retained == slot.module && ModulePath(retained) == slot.path &&
                ReadImage(retained, reinterpret_cast<uintptr_t>(slot.change.slot) - reinterpret_cast<uintptr_t>(retained), &value, sizeof(value)) &&
                value == slot.change.replacement;
        } catch (...) {}
        FreeLibrary(retained);
        if (matching) { result.api_mask |= slot.mask; ++result.patched_slots; }
        else slot.change.owned = false;
    }
    return result;
}
}
