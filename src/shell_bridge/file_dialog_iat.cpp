#include "file_dialog_iat.h"

#include <atomic>
#include <cstring>
#include <cstdint>
#include <memory>
#include <new>

namespace pulse::shell_bridge {
namespace {
// SDK aliases follow the compiling process width; Machine additionally rules
// out other instruction sets with the same pointer width.
using NativeNtHeaders = IMAGE_NT_HEADERS;
using NativeOptionalHeader = IMAGE_OPTIONAL_HEADER;
using NativeThunk = IMAGE_THUNK_DATA;
#if defined(_M_X64)
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_IX86)
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#else
constexpr WORD kMachine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
static_assert(sizeof(NativeThunk) == sizeof(void*));
constexpr size_t kMaxImage = 512 * 1024 * 1024;
HRESULT InvalidImage() { return HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT); }
template<class Read> HRESULT Inspect(Read read, size_t* slot_rva) {
    if (!slot_rva) return E_POINTER;
    *slot_rva = 0;
    if (kMachine == IMAGE_FILE_MACHINE_UNKNOWN) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    IMAGE_DOS_HEADER dos{};
    if (!read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 1024 * 1024) return InvalidImage();
    NativeNtHeaders nt{};
    if (!read(static_cast<size_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != kMachine || nt.FileHeader.SizeOfOptionalHeader != sizeof(NativeOptionalHeader) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IAT ||
        nt.OptionalHeader.SizeOfImage > kMaxImage || nt.OptionalHeader.SizeOfImage < sizeof(nt) ||
        static_cast<size_t>(dos.e_lfanew) > nt.OptionalHeader.SizeOfImage - sizeof(nt)) return InvalidImage();
    const size_t size = nt.OptionalHeader.SizeOfImage;
    auto bounded = [&](size_t offset, void* target, size_t count) {
        return offset <= size && count <= size - offset && read(offset, target, count);
    };
    auto name = [&](size_t offset, const char* expected, bool insensitive) {
        // Validate the terminator even for names which do not match.
        bool matches = true; size_t expected_size = std::strlen(expected);
        for (size_t i = 0; i < 256; ++i) {
            char value = 0;
            if (!bounded(offset + i, &value, 1)) return -1;
            if (!value) return matches && i == expected_size ? 1 : 0;
            char comparison = i < expected_size ? expected[i] : 0;
            if (insensitive && value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
            if (value != comparison) matches = false;
        }
        return -1;
    };
    const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        directory.VirtualAddress > size || directory.Size > size - directory.VirtualAddress) return InvalidImage();
    const auto iat = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT];
    if (!iat.VirtualAddress || iat.Size < sizeof(NativeThunk) || iat.VirtualAddress > size ||
        iat.Size > size - iat.VirtualAddress) return InvalidImage();
    bool ended = false; size_t found = 0;
    for (size_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= directory.Size; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        if (!bounded(directory.VirtualAddress + offset, &descriptor, sizeof(descriptor))) return InvalidImage();
        if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk && !descriptor.TimeDateStamp && !descriptor.ForwarderChain) {
            ended = true; break;
        }
        if (!descriptor.Name || !descriptor.FirstThunk || !descriptor.OriginalFirstThunk) return InvalidImage();
        const int ole = name(descriptor.Name, "ole32.dll", true);
        const int combase = name(descriptor.Name, "combase.dll", true);
        const int api = name(descriptor.Name, "api-ms-win-core-com-l1-1-0.dll", true);
        if (ole < 0 || combase < 0 || api < 0) return InvalidImage();
        bool thunk_end = false;
        for (size_t index = 0; index < size / sizeof(NativeThunk); ++index) {
            NativeThunk lookup{}, address{};
            const size_t delta = index * sizeof(lookup);
            if (!bounded(static_cast<size_t>(descriptor.OriginalFirstThunk) + delta, &lookup, sizeof(lookup)) ||
                !bounded(static_cast<size_t>(descriptor.FirstThunk) + delta, &address, sizeof(address))) return InvalidImage();
            if (!lookup.u1.AddressOfData) { if (address.u1.Function) return InvalidImage(); thunk_end = true; break; }
            if (!address.u1.Function) return InvalidImage();
            if (IMAGE_SNAP_BY_ORDINAL(lookup.u1.Ordinal)) continue;
            if (lookup.u1.AddressOfData > size || size - lookup.u1.AddressOfData < sizeof(WORD)) return InvalidImage();
            const int match = name(static_cast<size_t>(lookup.u1.AddressOfData) + sizeof(WORD), "CoCreateInstance", false);
            if (match < 0) return InvalidImage();
            if (match && (ole || combase || api)) {
                if (found) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                found = static_cast<size_t>(descriptor.FirstThunk) + delta;
                if (found % alignof(void*) != 0 || found < iat.VirtualAddress ||
                    found - iat.VirtualAddress > iat.Size - sizeof(void*)) return InvalidImage();
            }
        }
        if (!thunk_end) return InvalidImage();
    }
    if (!ended) return InvalidImage();
    if (!found) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    *slot_rva = found; return S_OK;
}
struct State {
    std::atomic<CoCreateInstanceFunction> original{nullptr};
    std::atomic<std::shared_ptr<const FileDialogActivation>> routing;
    void* volatile* slot = nullptr;
    DWORD saved_protection = 0;
    bool protection_pending = false;
    bool installed = false;
    bool owner_pinned = false;
};
constinit std::atomic<State*> published_state{nullptr};
SRWLOCK administration = SRWLOCK_INIT;
struct AdministrationLock {
    AdministrationLock() { AcquireSRWLockExclusive(&administration); }
    ~AdministrationLock() { ReleaseSRWLockExclusive(&administration); }
};
// Published state is process-lifetime. Before publication a local unique_ptr
// owns the candidate, so a pre-PIN rejection leaves no abandoned allocation.
HRESULT WINAPI Dispatcher(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* output) {
    auto* state = published_state.load();
    const auto original = state ? state->original.load() : nullptr;
    if (!original) { if (output) *output = nullptr; return E_UNEXPECTED; }
    const auto routing = state->routing.load();
    if (routing) return ActivateFileDialog(*routing, clsid, outer, context, iid, output);
    return original(clsid, outer, context, iid, output);
}
FileDialogIatResult Result(const State& state, HRESULT hr) { return {hr, state.installed, !state.protection_pending, state.owner_pinned}; }
// No allocation or dynamic lock initialization on metadata/error queries.
FileDialogIatResult Snapshot(HRESULT hr) {
    AdministrationLock lock;
    const auto* state = published_state.load();
    return state ? Result(*state, hr) : FileDialogIatResult{hr, false, true, false};
}
bool RestoreProtection(State& state) {
    if (!state.protection_pending) return true;
    DWORD ignored = 0;
    if (!VirtualProtect(const_cast<void**>(state.slot), sizeof(void*), state.saved_protection, &ignored)) return false;
    state.protection_pending = false; return true;
}
HRESULT Exchange(State& state, void* expected, void* replacement) {
    if (!RestoreProtection(state)) return HRESULT_FROM_WIN32(GetLastError());
    DWORD previous = 0;
    if (!VirtualProtect(const_cast<void**>(state.slot), sizeof(void*), PAGE_READWRITE, &previous)) return HRESULT_FROM_WIN32(GetLastError());
    state.saved_protection = previous; state.protection_pending = true;
    const auto observed = InterlockedCompareExchangePointer(state.slot, replacement, expected);
    state.installed = (observed == expected ? replacement : observed) == reinterpret_cast<void*>(&Dispatcher);
    if (!RestoreProtection(state)) {
        const DWORD error = GetLastError();
        // The page remains writable. Undo only our own successful install.
        if (observed == expected && replacement == reinterpret_cast<void*>(&Dispatcher)) {
            const auto reverted = InterlockedCompareExchangePointer(state.slot, expected, replacement);
            state.installed = false;
            (void)reverted; // A competing foreign value is never overwritten.
        }
        RestoreProtection(state);
        return HRESULT_FROM_WIN32(error);
    }
    return observed == expected ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
}
}

namespace detail {
HRESULT InspectFileDialogIatImage(std::span<const std::byte> image, size_t* slot_rva) noexcept {
    return Inspect([&](size_t offset, void* target, size_t count) {
        if (offset > image.size() || count > image.size() - offset) return false;
        std::memcpy(target, image.data() + offset, count); return true;
    }, slot_rva);
}
}

static FileDialogIatResult InstallFileDialogIat(HMODULE module, const FileDialogBridgeServices& services, bool enabled, bool pin_module) noexcept {
#if !defined(_M_X64) && !defined(_M_IX86)
    (void)module; (void)services; (void)enabled; (void)pin_module;
    return Snapshot(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
#else
    try {
        HMODULE self = nullptr;
        if (!module || module != GetModuleHandleW(nullptr) ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&Dispatcher), &self) || (pin_module ? self == module : self != module)) return Snapshot(E_ACCESSDENIED);
        // Copy user-owned callable state before taking the installation lock.
        auto routing = std::make_shared<FileDialogActivation>();
        routing->enabled = enabled; routing->services = services;
        std::unique_ptr<State> candidate;
        if (!published_state.load()) candidate = std::make_unique<State>();
        AdministrationLock lock;
        auto* existing = published_state.load();
        auto& state = existing ? *existing : *candidate;
        if (!RestoreProtection(state)) return Result(state, HRESULT_FROM_WIN32(GetLastError()));
        if (state.installed) return Result(state, HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS));
        const auto base = reinterpret_cast<uintptr_t>(module);
        size_t offset = 0;
        const HRESULT inspected = Inspect([&](size_t rva, void* target, size_t count) {
            if (rva > kMaxImage || count > kMaxImage - rva || base > UINTPTR_MAX - rva - count) return false;
            size_t checked = 0;
            while (checked < count) {
                MEMORY_BASIC_INFORMATION info{};
                const auto address = base + rva + checked;
                if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.AllocationBase != module ||
                    info.Type != MEM_IMAGE || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
                const size_t remaining = info.RegionSize - (address - reinterpret_cast<uintptr_t>(info.BaseAddress));
                checked += (remaining < count - checked ? remaining : count - checked);
            }
            SIZE_T copied = 0;
            return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + rva), target, count, &copied) && copied == count;
        }, &offset);
        if (FAILED(inspected)) return Result(state, inspected);
        const auto original = reinterpret_cast<CoCreateInstanceFunction>(GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance"));
        if (!original) return Result(state, HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
        auto* slot = reinterpret_cast<void* volatile*>(base + offset);
        MEMORY_BASIC_INFORMATION slot_page{};
        if (!VirtualQuery(const_cast<void**>(slot), &slot_page, sizeof(slot_page)) ||
            (slot_page.Protect != PAGE_READONLY && slot_page.Protect != PAGE_READWRITE && slot_page.Protect != PAGE_WRITECOPY))
            return Result(state, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
        if (state.slot && state.slot != slot) return Result(state, HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        // ReadProcessMemory avoids writing to a read-only IAT merely to inspect it.
        void* current = nullptr; SIZE_T copied = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), const_cast<void**>(slot), &current, sizeof(current), &copied) || copied != sizeof(current))
            return Result(state, HRESULT_FROM_WIN32(ERROR_READ_FAULT));
        if (current != reinterpret_cast<void*>(original)) return Result(state, HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        routing->original = original;
        state.slot = slot; state.original = original;
        if (pin_module && !state.owner_pinned) {
            HMODULE pinned = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&Dispatcher), &pinned)) return Result(state, HRESULT_FROM_WIN32(GetLastError()));
            state.owner_pinned = true;
            // Nothing which can throw may intervene between PIN success and
            // publication. Even a later failure must retain the pinned state.
            if (!existing) { published_state.store(candidate.release()); existing = &state; }
            if (pinned != self) return Result(state, E_UNEXPECTED);
        }
        // The EXE owner is inherently process-lifetime. Publish its candidate
        // too before the IAT can expose Dispatcher to another thread.
        if (!existing) published_state.store(candidate.release());
        const HRESULT patched = Exchange(state, reinterpret_cast<void*>(original), reinterpret_cast<void*>(&Dispatcher));
        if (SUCCEEDED(patched)) state.routing.store(std::move(routing));
        return Result(state, patched);
    } catch (const std::bad_alloc&) { return Snapshot(E_OUTOFMEMORY); }
    catch (...) { return Snapshot(E_FAIL); }
#endif
}

FileDialogIatResult InstallSelfFileDialogIat(HMODULE module, const FileDialogBridgeServices& services, bool enabled) noexcept {
    return InstallFileDialogIat(module, services, enabled, false);
}
FileDialogIatResult InstallPinnedModuleFileDialogIat(HMODULE module, const FileDialogBridgeServices& services, bool enabled) noexcept {
    return InstallFileDialogIat(module, services, enabled, true);
}
FileDialogIatResult QueryFileDialogIatState() noexcept { return Snapshot(S_OK); }

FileDialogIatResult UninstallSelfFileDialogIat() noexcept {
    try {
        std::shared_ptr<const FileDialogActivation> retired;
        AdministrationLock lock;
        auto* existing = published_state.load();
        if (!existing) return {S_FALSE, false, true, false};
        auto& state = *existing;
        retired = state.routing.exchange(nullptr);
        if (!RestoreProtection(state)) return Result(state, HRESULT_FROM_WIN32(GetLastError()));
        if (!state.installed) return Result(state, S_FALSE);
        return Result(state, Exchange(state, reinterpret_cast<void*>(&Dispatcher), reinterpret_cast<void*>(state.original.load())));
    } catch (...) { return Snapshot(E_FAIL); }
}
}
