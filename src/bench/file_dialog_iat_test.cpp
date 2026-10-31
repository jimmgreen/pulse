#include "../shell_bridge/file_dialog_iat.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#include <wrl/client.h>

using namespace pulse::shell_bridge;
using Microsoft::WRL::ComPtr;
namespace {
using NativeNtHeaders = IMAGE_NT_HEADERS;
using NativeOptionalHeader = IMAGE_OPTIONAL_HEADER;
using NativeThunk = IMAGE_THUNK_DATA;
#if defined(_M_X64)
constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
constexpr WORD kOtherMachine = IMAGE_FILE_MACHINE_I386;
constexpr WORD kOtherMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
using OtherNtHeaders = IMAGE_NT_HEADERS32;
using OtherOptionalHeader = IMAGE_OPTIONAL_HEADER32;
using OtherThunk = IMAGE_THUNK_DATA32;
#else
constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
constexpr WORD kOtherMachine = IMAGE_FILE_MACHINE_AMD64;
constexpr WORD kOtherMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
using OtherNtHeaders = IMAGE_NT_HEADERS64;
using OtherOptionalHeader = IMAGE_OPTIONAL_HEADER64;
using OtherThunk = IMAGE_THUNK_DATA64;
#endif
int checks = 0, failures = 0;
void Check(bool ok, const char* name, HRESULT hr = S_OK) {
    ++checks; if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " HRESULT=" << std::hex << static_cast<unsigned long>(hr) << std::dec;
    std::cout << '\n';
}
// Deliberately a real compiler-generated static import call, not the adapter.
__declspec(noinline) HRESULT DirectCreate(REFCLSID clsid, REFIID iid, void** output) {
    return ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, output);
}
struct Evidence {
    std::atomic<int> copies{0}, shows{0};
    std::atomic<bool> block{false}, recurse{false};
    std::atomic<int> nested{0}, throw_copy{0};
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Evidence() { if (entered) CloseHandle(entered); if (resume) CloseHandle(resume); }
};
struct ShowProbe {
    std::shared_ptr<Evidence> evidence;
    explicit ShowProbe(std::shared_ptr<Evidence> value) : evidence(std::move(value)) {}
    ShowProbe(const ShowProbe& other) : evidence(other.evidence) {
        if (evidence->throw_copy == 1) throw std::bad_alloc();
        if (evidence->throw_copy == 2) throw std::runtime_error("copy fixture");
        ++evidence->copies;
        if (evidence->recurse.load()) {
            ComPtr<IFileOpenDialog> nested;
            if (SUCCEEDED(DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&nested)))) ++evidence->nested;
        }
        if (evidence->block.load()) {
            SetEvent(evidence->entered);
            if (WaitForSingleObject(evidence->resume, 10000) != WAIT_OBJECT_0) throw std::bad_alloc();
        }
    }
    HRESULT operator()(IFileDialog*, HWND) const { ++evidence->shows; return E_ABORT; }
};
template<class T> void Put(std::vector<std::byte>& image, size_t offset, const T& value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}
std::vector<std::byte> Fixture() {
    std::vector<std::byte> image(4096);
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128; Put(image, 0, dos);
    NativeNtHeaders nt{};
    nt.Signature = IMAGE_NT_SIGNATURE; nt.FileHeader.Machine = kMachine;
    nt.FileHeader.SizeOfOptionalHeader = sizeof(NativeOptionalHeader);
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
    nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {512, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {1056, 2 * sizeof(NativeThunk)};
    Put(image, 128, nt);
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    descriptor.Name = 768; descriptor.OriginalFirstThunk = 1024; descriptor.FirstThunk = 1056;
    Put(image, 512, descriptor);
    std::memcpy(image.data() + 768, "ole32.dll", 10);
    NativeThunk lookup{}, address{}; lookup.u1.AddressOfData = 1100; address.u1.Function = 0x12345678;
    Put(image, 1024, lookup); Put(image, 1056, address);
    std::memcpy(image.data() + 1102, "CoCreateInstance", 17);
    return image;
}
void ParserTests() {
    auto image = Fixture(); size_t slot = 0;
    Check(detail::InspectFileDialogIatImage(image, &slot) == S_OK && slot == 1056, "bounded parser locates current-architecture static named import");
    auto ordinal_image = image;
    NativeThunk ordinal{}, named{}, first_address{}, named_address{};
    ordinal.u1.Ordinal = IMAGE_ORDINAL_FLAG | 7;
    named.u1.AddressOfData = 1100;
    first_address.u1.Function = 0x76543210; named_address.u1.Function = 0x12345678;
    Put(ordinal_image, 1024, ordinal); Put(ordinal_image, 1024 + sizeof(NativeThunk), named);
    Put(ordinal_image, 1056, first_address); Put(ordinal_image, 1056 + sizeof(NativeThunk), named_address);
    IMAGE_DATA_DIRECTORY larger_iat{1056, 3 * sizeof(NativeThunk)};
    Put(ordinal_image, 128 + offsetof(NativeNtHeaders, OptionalHeader) + offsetof(NativeOptionalHeader, DataDirectory) +
        IMAGE_DIRECTORY_ENTRY_IAT * sizeof(IMAGE_DATA_DIRECTORY), larger_iat);
    Check(detail::InspectFileDialogIatImage(ordinal_image, &slot) == S_OK && slot == 1056 + sizeof(NativeThunk),
        "native ordinal high bit and thunk stride skip ordinal before named import");
    bool truncations = true;
    for (size_t size = 0; size < 1119; ++size) {
        slot = 99;
        if (SUCCEEDED(detail::InspectFileDialogIatImage(std::span(image.data(), size), &slot)) || slot != 0) { truncations = false; break; }
    }
    Check(truncations, "all required-header and import-name truncations rejected with zero slot");
    const auto reject = [](std::vector<std::byte> broken, const char* name) {
        size_t offset = 99;
        Check(FAILED(detail::InspectFileDialogIatImage(broken, &offset)) && !offset, name);
    };
    auto broken = image; DWORD rva = MAXDWORD; Put(broken, 512 + offsetof(IMAGE_IMPORT_DESCRIPTOR, OriginalFirstThunk), rva);
    reject(std::move(broken), "out-of-image lookup table rejected");
    broken = image; rva = 1057; Put(broken, 512 + offsetof(IMAGE_IMPORT_DESCRIPTOR, FirstThunk), rva);
    reject(std::move(broken), "unaligned target slot rejected");
    broken = image; rva = 0; Put(broken, 512 + offsetof(IMAGE_IMPORT_DESCRIPTOR, OriginalFirstThunk), rva);
    reject(std::move(broken), "missing original lookup table rejected rather than guessed");
    broken = image; WORD machine = kOtherMachine; Put(broken, 128 + offsetof(NativeNtHeaders, FileHeader), machine);
    reject(std::move(broken), "mismatched architecture Machine rejected independently");
    broken = image; WORD magic = kOtherMagic; Put(broken, 128 + offsetof(NativeNtHeaders, OptionalHeader), magic);
    reject(std::move(broken), "mismatched optional-header Magic rejected independently");
    broken = image;
    std::memset(broken.data() + 128, 0, sizeof(IMAGE_NT_HEADERS64));
    OtherNtHeaders opposite{};
    opposite.Signature = IMAGE_NT_SIGNATURE; opposite.FileHeader.Machine = kOtherMachine;
    opposite.FileHeader.SizeOfOptionalHeader = sizeof(OtherOptionalHeader);
    opposite.OptionalHeader.Magic = kOtherMagic; opposite.OptionalHeader.SizeOfImage = static_cast<DWORD>(broken.size());
    opposite.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    opposite.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {512, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
    opposite.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = {1056, 2 * sizeof(OtherThunk)};
    Put(broken, 128, opposite);
    std::memset(broken.data() + 1024, 0, 64);
    OtherThunk opposite_lookup{}, opposite_address{};
    opposite_lookup.u1.AddressOfData = 1100; opposite_address.u1.Function = 0x12345678;
    Put(broken, 1024, opposite_lookup); Put(broken, 1056, opposite_address);
    reject(std::move(broken), "complete opposite-bitness header and thunk layout rejected");
    broken = image; std::memset(broken.data() + 1102, 'x', 256);
    reject(std::move(broken), "unterminated import name rejected");
    broken = image; NativeThunk bad{};
    // Keep the current architecture's ordinal bit clear.
    bad.u1.AddressOfData = IMAGE_ORDINAL_FLAG - 1; Put(broken, 1024, bad);
    reject(std::move(broken), "oversized named import RVA rejected");
    Check(detail::InspectFileDialogIatImage(image, nullptr) == E_POINTER, "null parser output rejected");
}
// Obtain a bounded live-image copy for locating the same slot in this test.
// Unreadable gaps are zero-filled; the production installer validates all reads.
void* volatile* OwnSlot() {
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{}; SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base, &dos, sizeof(dos), &read)) return nullptr;
    NativeNtHeaders nt{};
    if (!ReadProcessMemory(GetCurrentProcess(), base + dos.e_lfanew, &nt, sizeof(nt), &read)) return nullptr;
    std::vector<std::byte> image(nt.OptionalHeader.SizeOfImage);
    for (size_t offset = 0; offset < image.size();) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(base + offset, &info, sizeof(info))) return nullptr;
        size_t count = info.RegionSize - (reinterpret_cast<uintptr_t>(base + offset) - reinterpret_cast<uintptr_t>(info.BaseAddress));
        if (count > image.size() - offset) count = image.size() - offset;
        ReadProcessMemory(GetCurrentProcess(), base + offset, image.data() + offset, count, &read);
        offset += count;
    }
    size_t rva = 0;
    if (FAILED(detail::InspectFileDialogIatImage(image, &rva))) return nullptr;
    return reinterpret_cast<void* volatile*>(base + rva);
}
void* ReadSlot(void* volatile* slot) {
    void* result = nullptr; SIZE_T read = 0;
    ReadProcessMemory(GetCurrentProcess(), const_cast<void**>(slot), &result, sizeof(result), &read);
    return result;
}
bool TestExchange(void* volatile* slot, void* expected, void* value) {
    DWORD previous = 0, ignored = 0;
    if (!VirtualProtect(const_cast<void**>(slot), sizeof(void*), PAGE_READWRITE, &previous)) return false;
    const bool changed = InterlockedCompareExchangePointer(slot, value, expected) == expected;
    const bool restored = VirtualProtect(const_cast<void**>(slot), sizeof(void*), previous, &ignored) != FALSE;
    return changed && restored;
}
HRESULT WINAPI Foreign(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID* output) {
    if (output) *output = nullptr; return E_ACCESSDENIED;
}
}
int main() {
    std::cout << std::unitbuf;
    ParserTests();
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const auto module = GetModuleHandleW(nullptr);
    auto* slot = OwnSlot();
    Check(slot != nullptr, "locate actual compiler-generated EXE CoCreateInstance import");
    if (!slot) { CoUninitialize(); return 2; }
    const auto original_pointer = ReadSlot(slot);
    const auto original = reinterpret_cast<CoCreateInstanceFunction>(original_pointer);
    MEMORY_BASIC_INFORMATION before{}; VirtualQuery(const_cast<void**>(slot), &before, sizeof(before));
    auto evidence = std::make_shared<Evidence>();
    FileDialogBridgeServices services; services.show_native = ShowProbe(evidence);
    const auto initial_state = QueryFileDialogIatState();
    Check(initial_state.status == S_OK && !initial_state.installed && !initial_state.owner_pinned && initial_state.protection_restored,
        "metadata query before first installation returns empty state");
    const auto initial_stop = UninstallSelfFileDialogIat();
    Check(initial_stop.status == S_FALSE && !initial_stop.installed && !initial_stop.owner_pinned && initial_stop.protection_restored,
        "uninitialized uninstall is a no-op without creating process-lifetime state");
    const auto wrong_owner = InstallPinnedModuleFileDialogIat(module, services, true);
    Check(wrong_owner.status == E_ACCESSDENIED && !wrong_owner.owner_pinned && ReadSlot(slot) == original_pointer,
        "DLL-only pin entry rejects an EXE-owned dispatcher without changing import");
    Check(!QueryFileDialogIatState().owner_pinned, "ordinary EXE experiment has no DLL pin state");
    auto installed = InstallSelfFileDialogIat(GetModuleHandleW(L"ole32.dll"), services, true);
    Check(FAILED(installed.status) && ReadSlot(slot) == original_pointer, "foreign module rejected without modifying own import");
    installed = InstallSelfFileDialogIat(module, services, true);
    Check(SUCCEEDED(installed.status) && installed.installed && installed.protection_restored && ReadSlot(slot) != original_pointer,
        "explicit own-EXE installation atomically changes actual IAT slot", installed.status);
    if (FAILED(installed.status)) { UninstallSelfFileDialogIat(); CoUninitialize(); return 2; }
    const auto dispatcher = reinterpret_cast<CoCreateInstanceFunction>(ReadSlot(slot));
    {
        auto failing_evidence = std::make_shared<Evidence>();
        FileDialogBridgeServices failing_services; failing_services.show_native = ShowProbe(failing_evidence);
        failing_evidence->throw_copy = 1;
        const auto failed_copy = InstallSelfFileDialogIat(module, failing_services, true);
        Check(failed_copy.status == E_OUTOFMEMORY && failed_copy.installed && failed_copy.protection_restored &&
            ReadSlot(slot) == reinterpret_cast<void*>(dispatcher), "copy allocation failure reports existing global installation unchanged");
        failing_evidence->throw_copy = 2;
        const auto failed_other = InstallSelfFileDialogIat(module, failing_services, true);
        Check(failed_other.status == E_FAIL && failed_other.installed && failed_other.protection_restored &&
            ReadSlot(slot) == reinterpret_cast<void*>(dispatcher), "other copy exception reports existing global installation unchanged");
        const auto rejected = InstallSelfFileDialogIat(GetModuleHandleW(L"ole32.dll"), failing_services, true);
        Check(rejected.status == E_ACCESSDENIED && rejected.installed && rejected.protection_restored &&
            ReadSlot(slot) == reinterpret_cast<void*>(dispatcher), "foreign module rejection reports existing global installation without copying services");
        const int copied = evidence->copies.load(), shown = evidence->shows.load();
        ComPtr<IFileOpenDialog> still_active;
        const auto hr = DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&still_active));
        Check(SUCCEEDED(hr) && still_active && evidence->copies > copied && still_active->Show(nullptr) == E_ABORT &&
            evidence->shows == shown + 1 && failing_evidence->shows == 0,
            "old configuration remains callable after rejected replacement and copy exceptions", hr);
    }

    MEMORY_BASIC_INFORMATION after{}; VirtualQuery(const_cast<void**>(slot), &after, sizeof(after));
    Check(before.Protect == after.Protect, "installation restores original page protection");
    Check(InstallSelfFileDialogIat(module, services, true).status == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), "duplicate installation refused without replacing active configuration");
    {
        const int copied = evidence->copies.load(), shown = evidence->shows.load();
        ComPtr<IFileOpenDialog> dialog;
        const auto hr = DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&dialog));
        Check(SUCCEEDED(hr) && dialog && evidence->copies > copied, "direct CoCreateInstance import enters activation and wraps native object", hr);
        Check(dialog && evidence->copies > copied && dialog->Show(nullptr) == E_ABORT && evidence->shows == shown + 1, "direct-created wrapper retains closed validation gate and native Show seam");
    }
    {
        const int copied = evidence->copies.load();
        ComPtr<IUnknown> link;
        const auto hr = DirectCreate(CLSID_ShellLink, IID_PPV_ARGS(&link));
        Check(SUCCEEDED(hr) && link && evidence->copies == copied, "unrelated CLSID passes through without wrapper configuration");
        void* output = reinterpret_cast<void*>(1);
        const auto failed = DirectCreate(CLSID_FileOpenDialog, IID_IClassFactory, &output);
        void* expected = nullptr;
        const auto native_failed = original(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IClassFactory, &expected);
        Check(FAILED(failed) && failed == native_failed && !output && !expected, "original creation failure and zero output preserved through real import");
        ComPtr<IFileOpenDialog> dynamic;
        const auto dynamic_original = reinterpret_cast<CoCreateInstanceFunction>(GetProcAddress(GetModuleHandleW(L"ole32.dll"), "CoCreateInstance"));
        Check(SUCCEEDED(dynamic_original(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dynamic))) && evidence->copies == copied,
            "dynamic GetProcAddress call explicitly remains outside IAT coverage");
    }
    {
        const int copied = evidence->copies.load();
        evidence->recurse = true;
        ComPtr<IFileOpenDialog> outer;
        const auto hr = DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&outer));
        evidence->recurse = false;
        Check(SUCCEEDED(hr) && outer && evidence->nested == 1 && evidence->copies == copied + 1,
            "real import reentry during wrapper construction uses activation TLS bypass");
    }
    auto removed = UninstallSelfFileDialogIat();
    Check(SUCCEEDED(removed.status) && !removed.installed && removed.protection_restored && ReadSlot(slot) == original_pointer, "uninstall restores exact original slot");
    Check(UninstallSelfFileDialogIat().status == S_FALSE, "repeated uninstall is safe and reports already removed");
    {
        const int copied = evidence->copies.load();
        ComPtr<IFileOpenDialog> plain, delayed;
        Check(SUCCEEDED(DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&plain))) && evidence->copies == copied, "direct import after uninstall is original native creation");
        Check(SUCCEEDED(dispatcher(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&delayed))) && evidence->copies == copied,
            "previously loaded dispatcher remains safe after uninstall and routes original");
    }
    installed = InstallSelfFileDialogIat(module, services, false);
    {
        const int copied = evidence->copies.load();
        ComPtr<IFileOpenDialog> plain;
        Check(SUCCEEDED(installed.status) && SUCCEEDED(DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&plain))) && evidence->copies == copied,
            "installed but explicitly disabled adapter bypasses wrapping");
    }
    Check(SUCCEEDED(UninstallSelfFileDialogIat().status), "disabled installation uninstalls cleanly");
    const auto foreign = reinterpret_cast<void*>(&Foreign);
    Check(TestExchange(slot, original_pointer, foreign), "install isolated foreign-slot fixture");
    installed = InstallSelfFileDialogIat(module, services, true);
    Check(FAILED(installed.status) && ReadSlot(slot) == foreign, "installer refuses premodified slot without overwriting foreign owner");
    Check(TestExchange(slot, foreign, original_pointer), "restore foreign-slot fixture");
    installed = InstallSelfFileDialogIat(module, services, true);
    Check(SUCCEEDED(installed.status), "reinstall after conflict fixture cleanup");
    Check(TestExchange(slot, reinterpret_cast<void*>(dispatcher), foreign), "simulate foreign rewrite after installation");
    removed = UninstallSelfFileDialogIat();
    Check(FAILED(removed.status) && ReadSlot(slot) == foreign && removed.protection_restored, "uninstaller refuses overwriting a changed slot");
    Check(TestExchange(slot, foreign, original_pointer), "restore post-install conflict fixture");
    {
        auto active_evidence = std::make_shared<Evidence>();
        FileDialogBridgeServices active_services; active_services.show_native = ShowProbe(active_evidence);
        installed = InstallSelfFileDialogIat(module, active_services, true);
        Check(SUCCEEDED(installed.status) && active_evidence->entered && active_evidence->resume, "install active-call lifetime fixture");
        active_evidence->block = true;
        HRESULT created = E_UNEXPECTED, shown = E_UNEXPECTED;
        const int copies_before_call = active_evidence->copies.load();
        std::thread caller([&] {
            const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hr)) {
                { ComPtr<IFileOpenDialog> dialog;
                  created = DirectCreate(CLSID_FileOpenDialog, IID_PPV_ARGS(&dialog));
                  if (dialog && active_evidence->copies > copies_before_call) shown = dialog->Show(nullptr); }
                CoUninitialize();
            } else created = hr;
        });
        const bool entered = WaitForSingleObject(active_evidence->entered, 10000) == WAIT_OBJECT_0;
        removed = UninstallSelfFileDialogIat();
        active_services = {};
        Check(entered && SUCCEEDED(removed.status) && ReadSlot(slot) == original_pointer, "uninstall restores import while an activation holds its configuration");
        active_evidence->block = false; SetEvent(active_evidence->resume); caller.join();
        Check(SUCCEEDED(created) && shown == E_ABORT && active_evidence->shows == 1, "in-flight activation completes safely after uninstall without lost state", created);
    }
    VirtualQuery(const_cast<void**>(slot), &after, sizeof(after));
    Check(ReadSlot(slot) == original_pointer && before.Protect == after.Protect, "all fixtures leave original import and page protection restored");
    UninstallSelfFileDialogIat(); CoUninitialize();
    std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
    return failures ? 1 : 0;
}
