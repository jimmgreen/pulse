#include "../shell_bridge/runtime_auto_imports.h"
#include <objbase.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string_view>

using namespace pulse::shell_bridge::runtime;
namespace detail = pulse::shell_bridge::runtime::auto_import_detail;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); }
template<class T> void Put(std::vector<std::byte>& image, size_t offset, const T& value) { std::memcpy(image.data() + offset, &value, sizeof(value)); }
std::vector<std::byte> Fixture() {
    std::vector<std::byte> image(4096);
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128; Put(image, 0, dos);
    IMAGE_NT_HEADERS nt{}; nt.Signature = IMAGE_NT_SIGNATURE;
#ifdef _WIN64
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
#else
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
#endif
    nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER); nt.FileHeader.NumberOfSections = 1;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC; nt.OptionalHeader.SizeOfImage = 4096;
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {512, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {1056, 2 * sizeof(IMAGE_THUNK_DATA)};
    Put(image, 128, nt);
    IMAGE_SECTION_HEADER section{}; section.VirtualAddress = 512; section.Misc.VirtualSize = 3584; section.Characteristics = IMAGE_SCN_MEM_READ;
    Put(image, 128 + sizeof(nt), section);
    IMAGE_IMPORT_DESCRIPTOR descriptor{}; descriptor.Name = 768; descriptor.OriginalFirstThunk = 1024; descriptor.FirstThunk = 1056; Put(image, 512, descriptor);
    std::memcpy(image.data() + 768, "ole32.dll", 10);
    IMAGE_THUNK_DATA lookup{}, address{}; lookup.u1.AddressOfData = 1100; address.u1.Function = 0x12345678;
    Put(image, 1024, lookup); Put(image, 1056, address);
    std::memcpy(image.data() + 1102, "CoCreateInstance", 17);
    return image;
}
void Parser() {
    auto image = Fixture(); std::vector<detail::ImportSlot> slots;
    Check(detail::InspectImage(image, slots) == S_OK && slots.size() == 1 && slots[0].rva == 1056 && slots[0].api_index == 0, "bounded native PE import located");
    for (const auto& pair : std::array<std::pair<const char*, unsigned>, 3>{{{"CoCreateInstanceEx", 1}, {"CoGetClassObject", 2}, {"RoGetActivationFactory", 3}}}) {
        auto other = image; std::memset(other.data() + 1102, 0, 40); std::memcpy(other.data() + 1102, pair.first, std::strlen(pair.first) + 1);
        Check(detail::InspectImage(other, slots) == S_OK && slots.size() == 1 && slots[0].api_index == pair.second, "all requested named APIs mapped");
    }
    auto reject = [&](std::vector<std::byte> bad, const char* name) { Check(FAILED(detail::InspectImage(bad, slots)) && slots.empty(), name); };
    auto bad = image; DWORD rva = MAXDWORD; Put(bad, 512 + offsetof(IMAGE_IMPORT_DESCRIPTOR, OriginalFirstThunk), rva); reject(bad, "out-of-range thunk rejected");
    bad = image; rva = 1057; Put(bad, 512 + offsetof(IMAGE_IMPORT_DESCRIPTOR, FirstThunk), rva); reject(bad, "unaligned pointer slot rejected");
    bad = image; rva = 0; Put(bad, 512 + offsetof(IMAGE_IMPORT_DESCRIPTOR, OriginalFirstThunk), rva); reject(bad, "missing lookup table not guessed");
    bad = image; DWORD flags = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE; Put(bad, 128 + sizeof(IMAGE_NT_HEADERS) + offsetof(IMAGE_SECTION_HEADER, Characteristics), flags); reject(bad, "executable section excluded");
    bad = image; std::memset(bad.data() + 1102, 'x', 256); reject(bad, "unterminated import name rejected");
    bad = image; WORD machine = IMAGE_FILE_MACHINE_ARM64; Put(bad, 128 + offsetof(IMAGE_NT_HEADERS, FileHeader), machine); reject(bad, "foreign machine rejected");
    bool truncations = true;
    for (size_t size = 0; size < 1119; ++size) if (SUCCEEDED(detail::InspectImage(std::span(image.data(), size), slots)) || !slots.empty()) truncations = false;
    Check(truncations, "all required prefix truncations rejected");
    bad = image; IMAGE_DATA_DIRECTORY empty{}; Put(bad, 128 + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + offsetof(IMAGE_OPTIONAL_HEADER, DataDirectory) + IMAGE_DIRECTORY_ENTRY_IMPORT * sizeof(empty), empty);
    Check(detail::InspectImage(bad, slots) == S_OK && slots.empty(), "no static imports produces zero candidates");
}
unsigned protection_calls = 0, fail_call = 0;
BOOL WINAPI InjectProtect(LPVOID address, SIZE_T size, DWORD flags, PDWORD old) {
    if (++protection_calls == fail_call) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return VirtualProtect(address, size, flags, old);
}
BOOL WINAPI RevokeWriteProtect(LPVOID address, SIZE_T size, DWORD flags, PDWORD old) {
    const BOOL result = VirtualProtect(address, size, flags, old);
    if (result && flags == PAGE_READWRITE) { DWORD ignored = 0; VirtualProtect(address, size, PAGE_READONLY, &ignored); }
    return result;
}
void Slots() {
    auto* page = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(page != nullptr, "owned data page allocated"); if (!page) return;
    void* original = reinterpret_cast<void*>(uintptr_t{0x1000});
    void* replacement = reinterpret_cast<void*>(uintptr_t{0x2000});
    void* foreign = reinterpret_cast<void*>(uintptr_t{0x3000});
    *page = original; DWORD old = 0; VirtualProtect(page, 4096, PAGE_READONLY, &old);
    detail::SlotChange change{page, original, replacement};
    Check(detail::ChangeSlot(change, true) == S_OK && *page == replacement && change.owned, "CAS installs own slot");
    MEMORY_BASIC_INFORMATION info{}; VirtualQuery(page, &info, sizeof(info));
    Check(info.Protect == PAGE_READONLY && !change.protection_pending, "original read-only protection restored");
    Check(detail::ChangeSlot(change, false) == S_OK && *page == original, "CAS rollback restores original pointer");
    protection_calls = 0; fail_call = 2;
    Check(FAILED(detail::ChangeSlot(change, true, InjectProtect)) && *page == original && !change.owned && !change.protection_pending, "protection restore failure reverts own slot and retries protection");
    protection_calls = 0; fail_call = 1;
    Check(FAILED(detail::ChangeSlot(change, true, InjectProtect)) && *page == original, "initial protection failure writes nothing");
    Check(detail::ChangeSlot(change, true, RevokeWriteProtect) == HRESULT_FROM_WIN32(ERROR_NOACCESS) &&
        *page == original && !change.owned && !change.protection_pending,
        "foreign write-permission revocation fails native instead of crashing during CAS");
    VirtualProtect(page, 4096, PAGE_READWRITE, &old); *page = foreign; VirtualProtect(page, 4096, PAGE_READONLY, &old);
    Check(FAILED(detail::ChangeSlot(change, true)) && *page == foreign, "foreign existing hook never overwritten");
    VirtualProtect(page, 4096, PAGE_READWRITE, &old); *page = original; VirtualProtect(page, 4096, PAGE_READONLY, &old);
    Check(SUCCEEDED(detail::ChangeSlot(change, true)), "install before competing writer");
    VirtualProtect(page, 4096, PAGE_READWRITE, &old); InterlockedExchangePointer(page, foreign); VirtualProtect(page, 4096, PAGE_READONLY, &old);
    Check(FAILED(detail::ChangeSlot(change, false)) && *page == foreign && !change.owned, "rollback preserves competing writer");
    VirtualProtect(page, 4096, PAGE_READWRITE, &old); page[0] = original; page[1] = foreign;
    detail::SlotChange second{page + 1, original, replacement};
    const bool first_changed = SUCCEEDED(detail::ChangeSlot(change, true));
    const bool second_conflict = FAILED(detail::ChangeSlot(second, true));
    const bool first_reverted = SUCCEEDED(detail::ChangeSlot(change, false));
    Check(first_changed && second_conflict && first_reverted && page[0] == original && page[1] == foreign,
        "later slot conflict permits prior owned-slot rollback without foreign overwrite");
    VirtualProtect(page, 4096, PAGE_READWRITE, &old); *page = original;
    std::atomic<bool> done{false}; std::atomic<unsigned> invalid{0};
    std::thread reader([&] { while (!done.load()) { const auto value = InterlockedCompareExchangePointer(page, nullptr, nullptr); if (value != original && value != replacement) ++invalid; } });
    bool swapped = true;
    for (unsigned i = 0; i != 1000; ++i) {
        if (FAILED(detail::ChangeSlot(change, true)) || FAILED(detail::ChangeSlot(change, false))) swapped = false;
    }
    done = true; reader.join();
    Check(swapped && !invalid.load() && *page == original, "concurrent atomic reader sees only original or replacement through 1000 cycles");
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    Check(FAILED(detail::ChangeSlot(change, true)), "executable page never patched");
    VirtualFree(page, 0, MEM_RELEASE);
}
__declspec(noinline) HRESULT DirectCreate() { IUnknown* output = nullptr; const auto hr = CoCreateInstance(CLSID_NULL, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, reinterpret_cast<void**>(&output)); if (output) output->Release(); return hr; }
__declspec(noinline) HRESULT DirectEx() { MULTI_QI output{&IID_IUnknown, nullptr, E_FAIL}; const auto hr = CoCreateInstanceEx(CLSID_NULL, nullptr, CLSCTX_INPROC_SERVER, nullptr, 1, &output); if (output.pItf) output.pItf->Release(); return hr; }
__declspec(noinline) HRESULT DirectClass() { IUnknown* output = nullptr; const auto hr = CoGetClassObject(CLSID_NULL, CLSCTX_INPROC_SERVER, nullptr, IID_IUnknown, reinterpret_cast<void**>(&output)); if (output) output->Release(); return hr; }
void Live(const wchar_t* path) {
    HMODULE dll = LoadLibraryW(path); Check(dll != nullptr, "isolated helper DLL loaded"); if (!dll) return;
    const auto install = reinterpret_cast<HRESULT (WINAPI*)()>(GetProcAddress(dll, "ProbeInstall"));
    const auto rollback = reinterpret_cast<HRESULT (WINAPI*)()>(GetProcAddress(dll, "ProbeRollback"));
    const auto value = reinterpret_cast<unsigned (WINAPI*)(unsigned)>(GetProcAddress(dll, "ProbeValue"));
    Check(install && rollback && value, "probe ABI found"); if (!install || !rollback || !value) { FreeLibrary(dll); return; }
    const HRESULT baseline = DirectCreate(); Check(baseline != E_ABORT && FAILED(baseline), "baseline real COM import remains native");
    std::atomic<bool> stop{false}; std::atomic<unsigned> heartbeats{0}, unexpected{0};
    std::array<std::thread, 4> workers;
    for (auto& thread : workers) thread = std::thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop.load()) { const auto hr = DirectCreate(); if (hr != baseline && hr != E_ABORT) ++unexpected; ++heartbeats; }
        CoUninitialize();
    });
    const unsigned before = heartbeats.load();
    const HRESULT installed = install();
    Check(SUCCEEDED(installed), "real static IAT installation succeeds while workers call COM");
    std::printf("install_hr=%08lx mask=%u slots=%u\n", static_cast<unsigned long>(installed), value(3), value(4));
    Check(value(3) == 7 && value(4) >= 3, "actual API mask and patched slot count reported");
    Check(DirectCreate() == E_ABORT && DirectEx() == E_ABORT && DirectClass() == E_ABORT, "real compiler-generated COM3 imports route to helper");
    const auto dynamic_create = reinterpret_cast<decltype(&CoCreateInstance)>(GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance"));
    IUnknown* dynamic_output = nullptr;
    Check(dynamic_create && dynamic_create(CLSID_NULL, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
        reinterpret_cast<void**>(&dynamic_output)) == baseline, "GetProcAddress path remains native as declared coverage gap");
    if (dynamic_output) dynamic_output->Release();
    Check(value(0) && value(1) && value(2), "all three helper callbacks observed");
    Check(SUCCEEDED(rollback()), "real IAT rollback succeeds during concurrent COM calls");
    Check(DirectCreate() == baseline && DirectEx() != E_ABORT && DirectClass() != E_ABORT, "all static entries return native after rollback");
    stop = true; for (auto& thread : workers) thread.join();
    Check(heartbeats.load() > before && !unexpected.load(), "workers progress with only native or hooked call results");
    Check(value(3) == 0 && value(4) == 0 && value(5) == 1, "rollback reports no owned slots and restored protection");
    FreeLibrary(dll);
}
void ZeroSlots(const wchar_t* path) {
    HMODULE dll = LoadLibraryW(path); Check(dll != nullptr, "same-directory helper loaded"); if (!dll) return;
    const auto install = reinterpret_cast<HRESULT (WINAPI*)()>(GetProcAddress(dll, "ProbeInstall"));
    const auto value = reinterpret_cast<unsigned (WINAPI*)(unsigned)>(GetProcAddress(dll, "ProbeValue"));
    const HRESULT before = DirectCreate();
    Check(install && value && install() == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "excluded-bundle main EXE yields explicit zero-slot failure");
    Check(value && !value(3) && !value(4) && DirectCreate() == before, "zero-slot failure retains native calls and reports no coverage");
    FreeLibrary(dll);
}
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--parser-only") Parser();
    else if (argc == 3 && std::wstring_view(argv[2]) == L"--zero-slots") ZeroSlots(argv[1]);
    else { Parser(); Slots(); if (argc == 2) Live(argv[1]); else Check(false, "absolute helper path required"); }
    CoUninitialize();
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
