#include "../shell_bridge/file_dialog_probe_abi.h"
#include "../ipc/picker_protocol.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using CreateFunction = HRESULT (WINAPI*)(REFCLSID, IUnknown*, DWORD, REFIID, void**);
#ifdef _WIN64
constexpr uint32_t kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr uint32_t kMachine = IMAGE_FILE_MACHINE_I386;
#endif
int checks = 0, failures = 0;
void Check(bool good, const char* label, HRESULT status = S_OK) {
    ++checks; if (!good) ++failures;
    std::cout << (good ? "[PASS] " : "[FAIL] ") << label;
    if (!good) std::cout << " status=0x" << std::hex << static_cast<unsigned long>(status) << std::dec;
    std::cout << std::endl;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
bool Read(const void* source, void* destination, size_t bytes) {
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), source, destination, bytes, &copied) && copied == bytes;
}
void* ReadSlot(void* slot) { void* value = nullptr; return slot && Read(slot, &value, sizeof(value)) ? value : nullptr; }
// Read only this EXE's ordinary named imports. No bridge/parser implementation
// is linked into this loader (only the wire codec), and function-address expressions are not compared.
void* FindCreateSlot() {
    auto* base = reinterpret_cast<const std::byte*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    if (!Read(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return nullptr;
    IMAGE_NT_HEADERS nt{};
    if (!Read(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) return nullptr;
    const size_t size = nt.OptionalHeader.SizeOfImage;
    const auto read_rva = [&](size_t rva, void* output, size_t bytes) {
        return rva <= size && bytes <= size - rva && Read(base + rva, output, bytes);
    };
    const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    void* found = nullptr;
    for (size_t index = 0; index < directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++index) {
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        if (!read_rva(static_cast<size_t>(directory.VirtualAddress) + index * sizeof(descriptor), &descriptor, sizeof(descriptor))) return nullptr;
        if (!descriptor.Name) break;
        if (!descriptor.OriginalFirstThunk || !descriptor.FirstThunk) continue;
        for (size_t item = 0; item < size / sizeof(IMAGE_THUNK_DATA); ++item) {
            IMAGE_THUNK_DATA thunk{};
            if (!read_rva(static_cast<size_t>(descriptor.OriginalFirstThunk) + item * sizeof(thunk), &thunk, sizeof(thunk))) return nullptr;
            if (!thunk.u1.AddressOfData) break;
            if (IMAGE_SNAP_BY_ORDINAL(thunk.u1.Ordinal)) continue;
            std::array<char, 17> name{};
            if (!read_rva(static_cast<size_t>(thunk.u1.AddressOfData) + sizeof(WORD), name.data(), name.size())) return nullptr;
            if (std::memcmp(name.data(), "CoCreateInstance", 17) != 0) continue;
            const size_t rva = static_cast<size_t>(descriptor.FirstThunk) + item * sizeof(thunk);
            if (found || rva > size || sizeof(void*) > size - rva) return nullptr;
            found = const_cast<std::byte*>(base + rva);
        }
    }
    return found;
}
__declspec(noinline) HRESULT StaticCreate(IFileDialog** dialog) {
    return ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog));
}
HMODULE ObjectModule(IUnknown* object) {
    if (!object) return nullptr;
    const void* vtable = nullptr;
    if (!Read(object, &vtable, sizeof(vtable))) return nullptr;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(vtable), &module);
    return module;
}
PulseBridgeProbeState State(const PulseBridgeProbeApi& api) {
    PulseBridgeProbeState state{};
    state.size = sizeof(state); state.version = PULSE_BRIDGE_PROBE_ABI_VERSION; state.machine = kMachine;
    const HRESULT hr = api.get_state(&state);
    Check(SUCCEEDED(hr), "read C ABI state", hr);
    std::cout << "[STATE] flags=0x" << std::hex << state.flags << " last_status=0x"
              << static_cast<unsigned long>(state.last_status) << std::dec << " shows=" << state.native_show_calls
              << " active=" << state.active_native_shows << " barrier_error=" << state.last_probe_error << std::endl;
    return state;
}
bool Pinned(const PulseBridgeProbeState& state) { return (state.flags & PULSE_BRIDGE_PROBE_PINNED) != 0; }


namespace picker = pulse::ipc::picker;
PulseBridgeProbeFixtureState FixtureStateHeader() {
    PulseBridgeProbeFixtureState value{};
    value.size = sizeof(value); value.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION; value.machine = kMachine;
    return value;
}
bool FixtureApi(HMODULE module, PulseBridgeProbeFixtureApi& api) {
    const auto get = reinterpret_cast<PulseBridgeProbeGetFixtureApiFn>(GetProcAddress(module, "PulseBridgeProbeGetFixtureApi"));
    Check(get != nullptr, "independent undecorated fixture export exists");
    if (!get) return false;
    std::memset(&api, 0x5a, sizeof(api)); const auto unchanged = api;
    Check(FAILED(get(PULSE_BRIDGE_PROBE_FIXTURE_VERSION + 1, sizeof(api), &api)) &&
        std::memcmp(&api, &unchanged, sizeof(api)) == 0, "fixture API unknown version leaves output untouched");
    Check(FAILED(get(PULSE_BRIDGE_PROBE_FIXTURE_VERSION, sizeof(api) - 1, &api)) &&
        std::memcmp(&api, &unchanged, sizeof(api)) == 0, "fixture API short table leaves output untouched");
    Check(FAILED(get(PULSE_BRIDGE_PROBE_FIXTURE_VERSION, sizeof(api), nullptr)), "fixture API rejects null output");
    const HRESULT hr = get(PULSE_BRIDGE_PROBE_FIXTURE_VERSION, sizeof(api), &api);
    const bool valid = SUCCEEDED(hr) && api.size == sizeof(api) && api.version == PULSE_BRIDGE_PROBE_FIXTURE_VERSION &&
        api.machine == kMachine && !api.reserved && api.set_fixture && api.clear_fixture && api.get_state;
    Check(valid, "independent fixture function table matches version size and architecture", hr);
    return valid;
}
std::vector<uint8_t> FixtureBytes(picker::Operation operation, uint8_t id) {
    picker::Message message; message.request_id[0] = id;
    picker::Request request; request.operation = operation;
    request.required_capabilities = operation == picker::Operation::OpenFiles ? picker::Open :
        operation == picker::Operation::SaveFile ? picker::Save : picker::Folder;
    request.filename = u"Q"; message.body = request;
    std::vector<uint8_t> bytes;
    Check(picker::Encode(message, bytes) == picker::CodecError::None, "encode representative fixture Request with existing codec");
    return bytes;
}
PulseBridgeProbeFixture FixtureHeader(const std::vector<uint8_t>& bytes) {
    PulseBridgeProbeFixture value{}; value.size = sizeof(value); value.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION;
    value.machine = kMachine; value.bytes = bytes.data(); value.byte_count = static_cast<uint32_t>(bytes.size());
    return value;
}
void FixtureGuard(const PulseBridgeProbeFixtureApi& api) {
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto* allocation = static_cast<std::byte*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(allocation != nullptr, "allocate fixture size-first guard pages");
    if (!allocation) return;
    auto* size = reinterpret_cast<uint32_t*>(allocation + page - sizeof(uint32_t)); *size = sizeof(uint32_t);
    DWORD previous = 0;
    const bool protected_page = VirtualProtect(allocation + page, page, PAGE_NOACCESS, &previous) != FALSE;
    Check(protected_page, "protect fixture trailing page");
    if (protected_page) {
        Check(api.set_fixture(reinterpret_cast<const PulseBridgeProbeFixture*>(size)) == E_INVALIDARG && *size == sizeof(uint32_t),
            "SetFixture checks size before reading inaccessible version");
        Check(api.get_state(reinterpret_cast<PulseBridgeProbeFixtureState*>(size)) == E_INVALIDARG && *size == sizeof(uint32_t),
            "fixture query checks size before reading inaccessible version");
    }
    Check(VirtualFree(allocation, 0, MEM_RELEASE) != FALSE, "release fixture guard allocation");
}
void PrePinFixture(HMODULE module) {
    PulseBridgeProbeFixtureApi api{}; if (!FixtureApi(module, api)) return;
    FixtureGuard(api);
    const auto zero = FixtureStateHeader(); auto state = zero;
    Check(SUCCEEDED(api.get_state(&state)) && std::memcmp(&state, &zero, sizeof(state)) == 0,
        "pre-PIN fixture query has zero flags generations counters and IDs");
    Check(api.clear_fixture() == S_FALSE, "pre-PIN empty fixture clear returns S_FALSE");
    auto bytes = FixtureBytes(picker::Operation::OpenFiles, 71); auto fixture = FixtureHeader(bytes);
    DWORD before = 0, after = 0;
    const bool counted_before = GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE;
    const HRESULT set = api.set_fixture(&fixture);
    const bool counted_after = GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE;
    Check(set == E_UNEXPECTED && counted_before && counted_after && before == after,
        "pre-PIN valid fixture rejected without persistent handles", set);
    state = zero;
    Check(SUCCEEDED(api.get_state(&state)) && std::memcmp(&state, &zero, sizeof(state)) == 0 && api.clear_fixture() == S_FALSE,
        "rejected pre-PIN fixture leaves no selected profile");
}
void PinnedFixture(HMODULE module, const PulseBridgeProbeApi& base) {
    PulseBridgeProbeFixtureApi api{}; if (!FixtureApi(module, api)) return;
    FixtureGuard(api);
    const auto query = [&] { auto state = FixtureStateHeader(); Check(SUCCEEDED(api.get_state(&state)), "query selected fixture generation"); return state; };
    const auto zero = FixtureStateHeader();
    auto initial = query();
    Check(std::memcmp(&initial, &zero, sizeof(zero)) == 0 && api.clear_fixture() == S_FALSE, "PIN alone selects no fixture");
    auto input = FixtureBytes(picker::Operation::OpenFiles, 72); auto fixture = FixtureHeader(input);
    SYSTEM_INFO buffer_system{}; GetSystemInfo(&buffer_system);
    const size_t buffer_page = buffer_system.dwPageSize;
    auto* guarded_bytes = static_cast<uint8_t*>(VirtualAlloc(nullptr, buffer_page * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(guarded_bytes != nullptr && input.size() <= buffer_page, "allocate bounded wire-copy guard fixture");
    if (!guarded_bytes || input.size() > buffer_page) {
        if (guarded_bytes) VirtualFree(guarded_bytes, 0, MEM_RELEASE);
        return;
    }
    uint8_t* bounded = guarded_bytes + buffer_page - input.size();
    std::memcpy(bounded, input.data(), input.size());
    DWORD buffer_previous = 0;
    const bool buffer_guarded = VirtualProtect(guarded_bytes + buffer_page, buffer_page, PAGE_NOACCESS, &buffer_previous) != FALSE;
    Check(buffer_guarded, "wire ends immediately before inaccessible page");
    fixture.bytes = bounded;
    const HRESULT registration = buffer_guarded ? api.set_fixture(&fixture) : E_FAIL;
    Check(SUCCEEDED(registration), "PIN permits exact bounded Open wire without trailing read", registration);
    Check(VirtualFree(guarded_bytes, 0, MEM_RELEASE) != FALSE, "release caller wire storage immediately after SetFixture");
    const auto selected = query();
    auto expected = zero; expected.flags = PULSE_BRIDGE_PROBE_FIXTURE_PRESENT; expected.generation = selected.generation;
    expected.fixture_request_id[0] = 72;
    Check(selected.generation != 0 && std::memcmp(&selected, &expected, sizeof(expected)) == 0,
        "Open registration starts one generation with zero activity and exact request ID");
    std::fill(input.begin(), input.end(), uint8_t{0xff}); input.clear(); input.shrink_to_fit();
    auto after_source_release = query();
    Check(std::memcmp(&after_source_release, &selected, sizeof(selected)) == 0, "fixture state survives caller byte-buffer overwrite and release");
    auto valid_bytes = FixtureBytes(picker::Operation::OpenFiles, 73); fixture = FixtureHeader(valid_bytes);
    const auto reject = [&](const PulseBridgeProbeFixture& bad, const char* label) {
        const HRESULT hr = api.set_fixture(&bad); const auto after = query();
        Check(FAILED(hr) && std::memcmp(&after, &selected, sizeof(selected)) == 0, label, hr);
    };
    auto bad = fixture; --bad.size; reject(bad, "fixture wrong size preserves selected generation");
    bad = fixture; ++bad.version; reject(bad, "fixture wrong version preserves selected generation");
    bad = fixture; bad.machine = kMachine == IMAGE_FILE_MACHINE_I386 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    reject(bad, "fixture wrong machine preserves selected generation");
    bad = fixture; bad.flags = 1; reject(bad, "fixture nonzero flags preserve selected generation");
    bad = fixture; bad.reserved[2] = 1; reject(bad, "fixture reserved field preserves selected generation");
    bad = fixture; bad.byte_count = PULSE_BRIDGE_PROBE_MAX_FIXTURE_BYTES + 1;
    reject(bad, "fixture excessive length rejected before reading caller buffer");
    bad = fixture; bad.bytes = nullptr; reject(bad, "fixture null payload rejected atomically");
    bad = fixture; bad.byte_count = 0; reject(bad, "fixture empty payload rejected atomically");
    bad = fixture; --bad.byte_count; reject(bad, "fixture truncated wire rejected atomically");
    auto corrupt = valid_bytes; corrupt[0] ^= 1; bad = FixtureHeader(corrupt); reject(bad, "fixture bad magic rejected atomically");
    // In this encoded minimal Request, locate the unique UTF-16 Q, not a copied codec implementation.
    const std::array<uint8_t, 2> marker{0x51, 0};
    auto unit = std::search(corrupt.begin(), corrupt.end(), marker.begin(), marker.end());
    Check(unit != corrupt.end(), "find encoded UTF-16 fixture marker");
    if (unit != corrupt.end()) {
        corrupt = valid_bytes; unit = std::search(corrupt.begin(), corrupt.end(), marker.begin(), marker.end());
        *unit = 0; *(unit + 1) = 0xd8; bad = FixtureHeader(corrupt);
        reject(bad, "fixture isolated UTF-16 surrogate rejected atomically");
    }
    auto folder = FixtureBytes(picker::Operation::PickFolder, 74); bad = FixtureHeader(folder);
    reject(bad, "valid PickFolder wire is outside fixture API contract");
    picker::Message wrong_kind; wrong_kind.request_id[0] = 75; wrong_kind.body = picker::Cancel{};
    std::vector<uint8_t> other;
    Check(picker::Encode(wrong_kind, other) == picker::CodecError::None, "encode wrong-kind Cancel using codec");
    bad = FixtureHeader(other); reject(bad, "valid Cancel wire cannot replace Request fixture");
    wrong_kind.body = picker::Response{};
    Check(picker::Encode(wrong_kind, other) == picker::CodecError::None, "encode wrong-kind Response using codec");
    bad = FixtureHeader(other); reject(bad, "valid Response wire cannot replace Request fixture");
    for (unsigned field = 0; field < 4; ++field) {
        auto invalid = FixtureStateHeader();
        if (field == 0) ++invalid.version;
        if (field == 1) invalid.machine ^= 1;
        if (field == 2) invalid.reserved[3] = 1;
        if (field == 3) invalid.reserved0 = 1;
        const auto untouched = invalid;
        Check(FAILED(api.get_state(&invalid)) && std::memcmp(&invalid, &untouched, sizeof(invalid)) == 0,
            "invalid fixture query header is rejected without partial output");
    }
    auto save = FixtureBytes(picker::Operation::SaveFile, 76); fixture = FixtureHeader(save);
    Check(SUCCEEDED(api.set_fixture(&fixture)), "PIN permits Save Request replacement");
    auto replacement = query(); expected = zero; expected.flags = PULSE_BRIDGE_PROBE_FIXTURE_PRESENT;
    expected.generation = replacement.generation; expected.fixture_request_id[0] = 76;
    Check(replacement.generation != selected.generation && replacement.generation != 0 &&
        std::memcmp(&replacement, &expected, sizeof(expected)) == 0, "query returns only current replacement generation with reset counters");
    PulseBridgeProbeConfig no_host{}; no_host.size = sizeof(no_host); no_host.version = PULSE_BRIDGE_PROBE_ABI_VERSION;
    no_host.machine = kMachine; no_host.flags = PULSE_BRIDGE_PROBE_ENABLE; no_host.target_exe = GetModuleHandleW(nullptr);
    Check(FAILED(base.install(&no_host)), "selected fixture cannot enable bridge without absolute host path");
    no_host.host_path = L"pulse.exe"; no_host.host_path_units = 9;
    Check(FAILED(base.install(&no_host)), "selected fixture cannot enable bridge with relative host path");
    const auto no_dispatch = query();
    Check(std::memcmp(&no_dispatch, &replacement, sizeof(replacement)) == 0, "rejected installation causes no validation or host dispatch");
    Check(api.clear_fixture() == S_OK, "clear selected fixture succeeds");
    const auto cleared = query();
    Check(std::memcmp(&cleared, &zero, sizeof(zero)) == 0 && api.clear_fixture() == S_FALSE,
        "clear restores fully zero no-profile state and repeated clear is S_FALSE");
}

bool PrePinReload(const std::filesystem::path& path, HMODULE& module,
                  PulseBridgeProbeGetApiFn& get_api, PulseBridgeProbeApi& api) {
    const auto initial = State(api);
    const uint32_t live_flags = PULSE_BRIDGE_PROBE_INSTALLED | PULSE_BRIDGE_PROBE_PINNED | PULSE_BRIDGE_PROBE_ENABLED;
    Check(!(initial.flags & live_flags) && !initial.native_show_calls && !initial.active_native_shows && !initial.last_probe_error,
        "fresh pre-PIN load has no live routing or probe activity");
    wchar_t empty_path[2] = {L'!', L'!'};
    uint32_t required = 0;
    Check(SUCCEEDED(api.copy_host_path(empty_path, 2, &required)) && required == 1 && empty_path[0] == 0,
        "pre-PIN read-only path query returns empty state");
    PulseBridgeProbeConfig rejected{};
    rejected.size = sizeof(rejected); rejected.version = PULSE_BRIDGE_PROBE_ABI_VERSION;
    rejected.machine = kMachine; rejected.target_exe = GetModuleHandleW(nullptr); rejected.flags = 2;
    const HRESULT rejection = api.install(&rejected);
    Check(FAILED(rejection), "pre-PIN invalid install records rejection without installation", rejection);
    {
        Handle entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, resume{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        const bool events_valid = entered.value && resume.value;
        Check(events_valid, "create caller-owned events for pre-PIN rejection");
        if (events_valid) {
            DWORD before = 0, after = 0;
            const bool counted_before = GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE;
            const HRESULT barrier = api.set_show_barrier(entered.value, resume.value, 100);
            const bool counted_after = GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE;
            Check(barrier == E_UNEXPECTED, "nonempty pre-PIN barrier returns E_UNEXPECTED", barrier);
            std::cout << "[DIAG] pre-PIN rejection process handles before=" << before << " after=" << after << std::endl;
            Check(counted_before && counted_after && before == after,
                "barrier rejection adds no process handles within this call");
            Check(SetEvent(entered.value) && SetEvent(resume.value) &&
                WaitForSingleObject(entered.value, 0) == WAIT_OBJECT_0 && WaitForSingleObject(resume.value, 0) == WAIT_OBJECT_0,
                "rejected barrier leaves caller event handles usable");
        }
    }
    Check(api.deactivate() == S_FALSE, "uninitialized Deactivate returns S_FALSE");
    Check(SUCCEEDED(api.set_show_barrier(nullptr, nullptr, 0)), "empty pre-PIN barrier clear succeeds");
    PrePinFixture(module);
    const auto after_calls = State(api);
    Check(!(after_calls.flags & live_flags) && !after_calls.native_show_calls && !after_calls.active_native_shows,
        "read reject deactivate and clear leave DLL unpinned and uninstalled");
    const bool freed = FreeLibrary(module) != FALSE;
    module = nullptr;
    api = {}; get_api = nullptr; // Never call or retain the old API table after unload.
    Check(freed, "release pre-PIN explicit load reference");
    HMODULE observed = nullptr;
    const bool resident = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.c_str(), &observed) != FALSE;
    Check(!resident, "pre-PIN DLL actually unloads after read-only and rejected calls");
    if (!freed || resident) return false;
    module = LoadLibraryW(path.c_str());
    get_api = module ? reinterpret_cast<PulseBridgeProbeGetApiFn>(GetProcAddress(module, "PulseBridgeProbeGetApi")) : nullptr;
    const HRESULT acquired = get_api ? get_api(PULSE_BRIDGE_PROBE_ABI_VERSION, sizeof(api), &api) : E_FAIL;
    const bool valid = SUCCEEDED(acquired) && api.size == sizeof(api) && api.version == PULSE_BRIDGE_PROBE_ABI_VERSION &&
        api.machine == kMachine && !api.reserved && api.install && api.deactivate && api.get_state && api.copy_host_path && api.set_show_barrier;
    Check(valid, "reload DLL and acquire fresh C ABI table", acquired);
    if (!valid) return false;
    const auto reloaded = State(api);
    Check(reloaded.flags == initial.flags && reloaded.last_status == initial.last_status &&
        !reloaded.last_probe_error && !reloaded.native_show_calls && !reloaded.active_native_shows,
        "reload starts with clean initial status and counters");
    empty_path[0] = L'!'; required = 0;
    Check(SUCCEEDED(api.copy_host_path(empty_path, 2, &required)) && required == 1 && empty_path[0] == 0,
        "reload has no retained host string");
    std::cout << "[LIMIT] Pre-PIN checks establish unload lifecycle and call-local handle stability, not comprehensive heap leak coverage." << std::endl;
    return true;
}

void ShortHeaderGuardTest(const PulseBridgeProbeApi& api) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t page_size = static_cast<size_t>(info.dwPageSize);
    auto* memory = static_cast<std::byte*>(VirtualAlloc(nullptr, page_size * 2,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(memory != nullptr, "allocate process-owned short-header guard fixture", HRESULT_FROM_WIN32(GetLastError()));
    if (!memory) return;
    auto* size = reinterpret_cast<DWORD*>(memory + page_size - sizeof(DWORD));
    *size = sizeof(DWORD);
    DWORD previous = 0;
    const bool guarded = VirtualProtect(memory + page_size, page_size, PAGE_NOACCESS, &previous) != FALSE;
    Check(guarded, "protect page immediately after readable size DWORD", HRESULT_FROM_WIN32(GetLastError()));
    if (guarded) {
        // The only supplied readable field is size. Any eager version/machine
        // read crosses into NOACCESS; this case runs only against the fixed DLL.
        const HRESULT install = api.install(reinterpret_cast<const PulseBridgeProbeConfig*>(size));
        Check(install == E_INVALIDARG && *size == sizeof(DWORD),
            "Install rejects four-byte header without reading next page or writing size", install);
        const HRESULT state = api.get_state(reinterpret_cast<PulseBridgeProbeState*>(size));
        Check(state == E_INVALIDARG && *size == sizeof(DWORD),
            "GetState rejects four-byte header without reading next page or writing size", state);
    }
    const bool released = VirtualFree(memory, 0, MEM_RELEASE) != FALSE;
    Check(released, "release process-owned short-header guard fixture", HRESULT_FROM_WIN32(GetLastError()));
}

int Run(const std::filesystem::path& dll_path, const std::filesystem::path& incompatible) {
    HMODULE module = LoadLibraryW(dll_path.c_str());
    Check(module != nullptr, "load explicitly named matching DLL", HRESULT_FROM_WIN32(GetLastError()));
    if (!module) return 2;
    auto get_api = reinterpret_cast<PulseBridgeProbeGetApiFn>(GetProcAddress(module, "PulseBridgeProbeGetApi"));
    Check(get_api != nullptr, "stable undecorated C export exists");
    if (!get_api) { FreeLibrary(module); return 2; }
    PulseBridgeProbeApi api{};
    std::memset(&api, 0x5a, sizeof(api));
    const auto untouched_api = api;
    Check(FAILED(get_api(PULSE_BRIDGE_PROBE_ABI_VERSION + 1, sizeof(api), &api)) &&
        std::memcmp(&api, &untouched_api, sizeof(api)) == 0, "unknown API version rejected without output writes");
    Check(FAILED(get_api(PULSE_BRIDGE_PROBE_ABI_VERSION, sizeof(api) - 1, &api)) &&
        std::memcmp(&api, &untouched_api, sizeof(api)) == 0, "wrong API size rejected without output writes");
    const HRESULT acquired = get_api(PULSE_BRIDGE_PROBE_ABI_VERSION, sizeof(api), &api);
    const bool valid_api = SUCCEEDED(acquired) && api.size == sizeof(api) && api.version == PULSE_BRIDGE_PROBE_ABI_VERSION &&
        api.machine == kMachine && api.reserved == 0 && api.install && api.deactivate && api.get_state && api.copy_host_path && api.set_show_barrier;
    Check(valid_api, "matching C ABI table returned", acquired);
    if (!valid_api) { FreeLibrary(module); return 2; }
    if (!PrePinReload(dll_path, module, get_api, api)) { if (module) FreeLibrary(module); return 1; }
    ShortHeaderGuardTest(api);
    PulseBridgeProbeState invalid_state{};
    invalid_state.size = sizeof(invalid_state); invalid_state.version = PULSE_BRIDGE_PROBE_ABI_VERSION;
    invalid_state.machine = kMachine; invalid_state.reserved[0] = 1;
    Check(FAILED(api.get_state(&invalid_state)), "GetState rejects reserved input fields");
    invalid_state.reserved[0] = 0; --invalid_state.size;
    Check(FAILED(api.get_state(&invalid_state)), "GetState rejects wrong input size");
    invalid_state.size = sizeof(invalid_state); ++invalid_state.version;
    Check(FAILED(api.get_state(&invalid_state)), "GetState rejects wrong input version");
    invalid_state.version = PULSE_BRIDGE_PROBE_ABI_VERSION;
    invalid_state.machine = kMachine == IMAGE_FILE_MACHINE_I386 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    Check(FAILED(api.get_state(&invalid_state)), "GetState rejects wrong input machine");
    Check(FAILED(api.set_show_barrier(nullptr, nullptr, 1)), "incomplete barrier configuration rejected");
    void* slot = FindCreateSlot();
    void* original = ReadSlot(slot);
    Check(slot && original, "locate actual EXE static CoCreateInstance slot by PE imports");
    if (!slot || !original) { FreeLibrary(module); return 2; }
    PulseBridgeProbeConfig config{};
    config.size = sizeof(config); config.version = PULSE_BRIDGE_PROBE_ABI_VERSION; config.machine = kMachine;
    config.target_exe = GetModuleHandleW(nullptr);
    const auto reject = [&](PulseBridgeProbeConfig bad, const char* label) {
        const HRESULT hr = api.install(&bad);
        Check(FAILED(hr) && ReadSlot(slot) == original, label, hr);
        State(api); // A failure must not be misreported as proof of non-pinning.
    };
    auto bad = config; --bad.size; reject(bad, "config size rejected before patching");
    bad = config; ++bad.version; reject(bad, "config version rejected before patching");
    bad = config; bad.machine = kMachine == IMAGE_FILE_MACHINE_I386 ? IMAGE_FILE_MACHINE_AMD64 : IMAGE_FILE_MACHINE_I386;
    reject(bad, "wrong machine rejected before patching");
    bad = config; bad.reserved[4] = 1; reject(bad, "reserved config data rejected");
    bad = config; bad.flags = 2; reject(bad, "unknown flags rejected");
    bad = config; bad.host_path = L"x"; bad.host_path_units = PULSE_BRIDGE_PROBE_MAX_PATH_UNITS + 1;
    reject(bad, "oversized bounded string rejected");
    bad = config; bad.host_path_units = 1; reject(bad, "null bounded string rejected");
    const wchar_t embedded[] = {L'a', 0, L'b'};
    bad = config; bad.host_path = embedded; bad.host_path_units = 3; reject(bad, "embedded NUL rejected");
    bad = config; bad.target_exe = module; reject(bad, "DLL target rejected instead of patching foreign module");

    std::wstring host = L"C:\\PulseProbeFixture\\host-\x6D4B\x8BD5.exe";
    const auto expected_host = host;
    config.host_path = host.data(); config.host_path_units = static_cast<uint32_t>(host.size());
    HRESULT hr = api.install(&config);
    auto state = State(api);
    const bool installed_disabled = SUCCEEDED(hr) && (state.flags & PULSE_BRIDGE_PROBE_INSTALLED) &&
        !(state.flags & PULSE_BRIDGE_PROBE_ENABLED) && Pinned(state) && (state.flags & PULSE_BRIDGE_PROBE_PROTECTION_RESTORED);
    Check(installed_disabled, "default disabled install succeeds with truthful pinned state", hr);
    if (!installed_disabled) { api.deactivate(); FreeLibrary(module); return 1; }
    void* disabled_slot = ReadSlot(slot);
    bad = config; bad.flags = 2;
    const HRESULT rejected_while_installed = api.install(&bad);
    const auto after_rejected = State(api);
    Check(FAILED(rejected_while_installed) && after_rejected.flags == state.flags && ReadSlot(slot) == disabled_slot,
        "rejected live config preserves installation pin and slot, while last_status may change", rejected_while_installed);
    host.assign(host.size(), L'Z');
    uint32_t required = 0;
    hr = api.copy_host_path(nullptr, 0, &required);
    Check(hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && required == expected_host.size() + 1,
        "copy reports required caller capacity including NUL", hr);
    if (required != expected_host.size() + 1) { api.deactivate(); FreeLibrary(module); return 1; }
    std::vector<wchar_t> copied(required, L'!');
    const auto before_short = copied;
    Check(api.copy_host_path(copied.data(), required - 1, &required) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
        copied == before_short, "insufficient caller capacity makes no partial writes");
    Check(SUCCEEDED(api.copy_host_path(copied.data(), static_cast<uint32_t>(copied.size()), &required)) &&
        copied.back() == 0 && std::wstring(copied.data()) == expected_host, "DLL owns independent UTF-16 host string copy");
    {
        ComPtr<IFileDialog> native;
        hr = StaticCreate(&native);
        Check(SUCCEEDED(hr) && native && ObjectModule(native.Get()) != module, "disabled static activation stays native without Show", hr);
    }
    Check(FAILED(api.install(&config)), "duplicate installation rejected");
    Check(SUCCEEDED(api.deactivate()) && ReadSlot(slot) == original, "disabled deactivation restores exact original IAT");
    Check(SUCCEEDED(api.deactivate()), "repeated deactivation is safe");
    Check(SUCCEEDED(api.copy_host_path(copied.data(), static_cast<uint32_t>(copied.size()), &required)) &&
        copied.back() == 0 && std::wstring(copied.data()) == expected_host, "last successful copied host remains after deactivation");

    config.flags = PULSE_BRIDGE_PROBE_ENABLE;
    config.host_path = expected_host.data(); config.host_path_units = static_cast<uint32_t>(expected_host.size());
    hr = api.install(&config); state = State(api);
    const bool enabled = SUCCEEDED(hr) && (state.flags & PULSE_BRIDGE_PROBE_INSTALLED) && (state.flags & PULSE_BRIDGE_PROBE_ENABLED);
    Check(enabled && ReadSlot(slot) != original, "enabled installation changes actual import slot", hr);
    if (!enabled) { api.deactivate(); FreeLibrary(module); return 1; }
    const auto delayed = reinterpret_cast<CreateFunction>(ReadSlot(slot));
    ComPtr<IFileDialog> old_wrapper;
    hr = StaticCreate(&old_wrapper);
    const bool wrapped = SUCCEEDED(hr) && ObjectModule(old_wrapper.Get()) == module;
    Check(wrapped, "real static API returns DLL wrapper before allowing Show", hr);
    if (wrapped) Check(old_wrapper->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED), "verified DLL wrapper Show uses fixed cancellation");
    auto after_show = State(api);
    Check(after_show.native_show_calls == state.native_show_calls + 1 && after_show.active_native_shows == 0,
        "one completed native-seam call counted without real UI");

    Handle entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, resume{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle entered_observer, resume_control;
    const bool handles = entered.value && resume.value &&
        DuplicateHandle(GetCurrentProcess(), entered.value, GetCurrentProcess(), &entered_observer.value, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
        DuplicateHandle(GetCurrentProcess(), resume.value, GetCurrentProcess(), &resume_control.value, 0, FALSE, DUPLICATE_SAME_ACCESS);
    Check(handles, "create probe barrier and independent test handles");
    const HRESULT barrier_hr = handles ? api.set_show_barrier(entered.value, resume.value, 10000) : E_FAIL;
    if (SUCCEEDED(barrier_hr)) {
        CloseHandle(entered.value); entered.value = nullptr;
        CloseHandle(resume.value); resume.value = nullptr;
        std::atomic<HRESULT> worker_result{E_PENDING};
        std::atomic<bool> worker_wrapped{false}, worker_finished{false};
        std::thread worker([&] {
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(initialized)) {
                { ComPtr<IFileDialog> dialog;
                  HRESULT created = StaticCreate(&dialog);
                  const bool is_wrapper = SUCCEEDED(created) && ObjectModule(dialog.Get()) == module;
                  worker_wrapped = is_wrapper;
                  worker_result = is_wrapper ? dialog->Show(nullptr) : E_UNEXPECTED;
                }
                CoUninitialize();
            } else worker_result = initialized;
            worker_finished = true;
        });
        const bool signaled = WaitForSingleObject(entered_observer.value, 5000) == WAIT_OBJECT_0;
        Check(signaled, "duplicated entered handle signals in-flight verified Show");
        const auto in_flight = State(api);
        Check(signaled && in_flight.active_native_shows == 1, "state exposes active barrier Show");
        Check(SUCCEEDED(api.deactivate()) && ReadSlot(slot) == original, "deactivate restores import while Show remains in flight");
        Check(SUCCEEDED(api.set_show_barrier(nullptr, nullptr, 0)), "clear configured barrier without changing running snapshot");
        Check(!worker_finished.load(), "deactivation and barrier replacement do not drain existing Show");
        SetEvent(resume_control.value);
        worker.join();
        Check(worker_wrapped && worker_result == HRESULT_FROM_WIN32(ERROR_CANCELLED), "in-flight Show finishes safely after deactivation");
    } else { Check(false, "install finite duplicated-handle barrier", barrier_hr); api.deactivate(); }
    const auto final_state = State(api);
    Check(Pinned(final_state) && !(final_state.flags & PULSE_BRIDGE_PROBE_INSTALLED) && !final_state.active_native_shows &&
        !final_state.last_probe_error, "deactivated DLL remains pinned with no active shows or barrier errors");
    if (Pinned(final_state)) {
        PinnedFixture(module, api);
        const auto after_fixture = State(api);
        Check(after_fixture.native_show_calls == final_state.native_show_calls && !after_fixture.active_native_shows &&
            !(after_fixture.flags & PULSE_BRIDGE_PROBE_INSTALLED), "fixture API tests never invoke Show or install routing");
    }
    if (!Pinned(final_state)) old_wrapper.Reset();
    Check(FreeLibrary(module) != FALSE, "release explicit DLL load reference");
    HMODULE resident = nullptr;
    const bool resident_pinned = Pinned(final_state) && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(get_api), &resident) && resident == module;
    Check(resident_pinned, "PIN retains code after FreeLibrary until process exit");
    if (resident_pinned) {
        if (wrapped && ObjectModule(old_wrapper.Get()) == module)
            Check(old_wrapper->Show(nullptr) == HRESULT_FROM_WIN32(ERROR_CANCELLED), "old wrapper remains callable after FreeLibrary");
        ComPtr<IFileDialog> from_old_thunk;
        hr = delayed(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&from_old_thunk));
        Check(SUCCEEDED(hr) && from_old_thunk && ObjectModule(from_old_thunk.Get()) != module,
            "previously read thunk remains safe and routes native creation after FreeLibrary", hr);
        const auto retained_state = State(api);
        Check(retained_state.native_show_calls == final_state.native_show_calls + 1 && !retained_state.active_native_shows,
            "post-FreeLibrary old wrapper contributes exactly one completed probe Show");
    }
    // A broken PIN contract must not lead the test to Release through unmapped
    // code. The failing process exits immediately after preserving diagnostics.
    if (resident_pinned) old_wrapper.Reset(); else (void)old_wrapper.Detach();
    if (!incompatible.empty()) {
        HMODULE wrong = LoadLibraryW(incompatible.c_str());
        const DWORD error = GetLastError();
        Check(!wrong && error == ERROR_BAD_EXE_FORMAT, "optional wrong-bitness DLL load fails with bad image format", HRESULT_FROM_WIN32(error));
        if (wrong) FreeLibrary(wrong);
    } else std::cout << "[SKIP] no opposite-bitness DLL path supplied" << std::endl;
    return failures ? 1 : 0;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3 || !std::filesystem::path(argv[1]).is_absolute() ||
        (argc == 3 && !std::filesystem::path(argv[2]).is_absolute())) {
        std::cerr << "Usage: pulse_file_dialog_probe_load_test <absolute matching DLL> [absolute wrong-bitness DLL]\n";
        return 2;
    }
    Handle finished{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!finished.value) return 2;
    std::thread watchdog([&] {
        if (WaitForSingleObject(finished.value, 60000) != WAIT_OBJECT_0) {
            std::cerr << "[FAIL] 60-second self-test watchdog; ending only this test process" << std::endl;
            TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 2;
    if (SUCCEEDED(initialized)) {
        result = Run(argv[1], argc == 3 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        CoUninitialize();
    }
    SetEvent(finished.value); watchdog.join();
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return result;
}
