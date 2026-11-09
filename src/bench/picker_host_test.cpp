#include "../ipc/picker_transport.h"
#include "../ipc/picker_client.h"
#include "../shell_bridge/file_dialog_wrapper.h"
#include "../shell_bridge/picker_shell_results.h"
#include "../shell_bridge/legacy_adapter.h"
#include "../shell_bridge/legacy_ansi_adapter.h"
#include "../shell_bridge/file_dialog_iat.h"
#include "../shell_bridge/file_dialog_activation_ex.h"
#include "../shell_bridge/file_dialog_class_factory.h"
#include "../shell_bridge/file_dialog_probe_abi.h"

#include <filesystem>
#include <iostream>
#include <thread>
#include <shobjidl.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <utility>
#include <atomic>
#include <memory>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")

using namespace pulse::ipc::picker;
namespace {
int checks = 0, failures = 0;
thread_local int* legacy_native_calls = nullptr;
BOOL WINAPI RejectLegacyNative(OPENFILENAMEW*) {
    if (legacy_native_calls) ++*legacy_native_calls;
    return FALSE;
}
BOOL WINAPI RejectLegacyAnsiNative(OPENFILENAMEA*) {
    if (legacy_native_calls) ++*legacy_native_calls;
    return FALSE;
}
bool StrictAcp(const std::wstring& wide, std::string& bytes) {
    if (wide.empty()) { bytes.clear(); return true; }
    const UINT cp = GetACP();
    BOOL replaced = FALSE;
    BOOL* used = cp == CP_UTF8 ? nullptr : &replaced;
    const DWORD flags = cp == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const int count = WideCharToMultiByte(cp, flags, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, used);
    if (count <= 0 || replaced) return false;
    bytes.resize(static_cast<size_t>(count));
    if (WideCharToMultiByte(cp, flags, wide.data(), static_cast<int>(wide.size()), bytes.data(), count, nullptr, used) != count || replaced)
        return false;
    const int units = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), count, nullptr, 0);
    if (units != static_cast<int>(wide.size())) return false;
    std::wstring roundtrip(static_cast<size_t>(units), 0);
    return MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, bytes.data(), count, roundtrip.data(), units) == units && roundtrip == wide;
}
DWORD WINAPI RejectLegacyError() { return CDERR_DIALOGFAILURE; }
__declspec(noinline) HRESULT DirectCreateDialog(REFCLSID clsid, REFIID iid, void** output) {
    return ::CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, output);
}
__declspec(noinline) pulse::shell_bridge::CoCreateInstanceFunction ImportedCreateAddress() {
    return &::CoCreateInstance;
}
void* OwnCreateSlot() {
    auto* base = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{}; SIZE_T read = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), base, &dos, sizeof(dos), &read) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
        return nullptr;
    IMAGE_NT_HEADERS nt{};
    if (!ReadProcessMemory(GetCurrentProcess(), base + dos.e_lfanew, &nt, sizeof(nt), &read) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > 256 * 1024 * 1024) return nullptr;
    std::vector<std::byte> image(nt.OptionalHeader.SizeOfImage);
    for (size_t offset = 0; offset < image.size();) {
        MEMORY_BASIC_INFORMATION page{};
        if (!VirtualQuery(base + offset, &page, sizeof(page))) return nullptr;
        const size_t available = static_cast<size_t>(page.RegionSize -
            (reinterpret_cast<uintptr_t>(base + offset) - reinterpret_cast<uintptr_t>(page.BaseAddress)));
        const auto count = (std::min)(available, image.size() - offset);
        if (!count) return nullptr;
        // Unreadable image gaps stay zero, as in the dedicated IAT parser test.
        ReadProcessMemory(GetCurrentProcess(), base + offset, image.data() + offset, count, &read);
        offset += count;
    }
    size_t rva = 0;
    if (FAILED(pulse::shell_bridge::detail::InspectFileDialogIatImage(image, &rva))) return nullptr;
    return base + rva;
}
void* ReadCreateSlot(void* slot) {
    void* value = nullptr; SIZE_T read = 0;
    return slot && ReadProcessMemory(GetCurrentProcess(), slot, &value, sizeof(value), &read) && read == sizeof(value) ? value : nullptr;
}
struct BridgeEvidence {
    Request expected;
    std::atomic<int> verified{0}, native{0}, client_calls{0};
};
bool IsLinkedWrapper(IFileDialog* dialog) {
    if (!dialog) return false;
    const void* vtable = *reinterpret_cast<const void* const*>(dialog);
    HMODULE implementation = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(vtable), &implementation) && implementation == GetModuleHandleW(nullptr);
}
#ifdef _WIN64
constexpr uint32_t kProbeMachine = IMAGE_FILE_MACHINE_AMD64;
#else
constexpr uint32_t kProbeMachine = IMAGE_FILE_MACHINE_I386;
#endif
struct ProbeFixtureSession {
    HMODULE module = nullptr;
    PulseBridgeProbeApi api{};
    PulseBridgeProbeFixtureApi fixture_api{};
    PulseBridgeProbeState before{}, after{};
    PulseBridgeProbeFixtureState fixture{};
    void* slot = nullptr;
    void* original = nullptr;
    HRESULT status = E_UNEXPECTED;
    bool installed = false, restored = false, pinned_after_free = false;
    HRESULT ReadState(PulseBridgeProbeState& value) {
        value = {}; value.size = sizeof(value); value.version = PULSE_BRIDGE_PROBE_ABI_VERSION; value.machine = kProbeMachine;
        return api.get_state ? api.get_state(&value) : E_NOINTERFACE;
    }
    HRESULT ReadFixture() {
        fixture = {}; fixture.size = sizeof(fixture); fixture.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION; fixture.machine = kProbeMachine;
        return fixture_api.get_state ? fixture_api.get_state(&fixture) : E_NOINTERFACE;
    }
    bool Step(HRESULT hr, const char* name) {
        status = hr;
        std::cout << "[DIAG] probe " << name << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << std::endl;
        return SUCCEEDED(hr);
    }
    bool Begin(const std::wstring& path, const std::wstring& host, const Message& request) {
        module = LoadLibraryW(path.c_str());
        if (!module) return Step(HRESULT_FROM_WIN32(GetLastError()), "LoadLibrary");
        const auto get_api = reinterpret_cast<PulseBridgeProbeGetApiFn>(GetProcAddress(module, "PulseBridgeProbeGetApi"));
        const auto get_fixture = reinterpret_cast<PulseBridgeProbeGetFixtureApiFn>(GetProcAddress(module, "PulseBridgeProbeGetFixtureApi"));
        if (!get_api || !get_fixture) return Step(E_NOINTERFACE, "exports");
        if (!Step(get_api(PULSE_BRIDGE_PROBE_ABI_VERSION, sizeof(api), &api), "GetApi") ||
            api.size != sizeof(api) || api.version != PULSE_BRIDGE_PROBE_ABI_VERSION || api.machine != kProbeMachine || api.reserved ||
            !api.install || !api.deactivate || !api.get_state) return false;
        if (!Step(get_fixture(PULSE_BRIDGE_PROBE_FIXTURE_VERSION, sizeof(fixture_api), &fixture_api), "GetFixtureApi") ||
            fixture_api.size != sizeof(fixture_api) || fixture_api.version != PULSE_BRIDGE_PROBE_FIXTURE_VERSION ||
            fixture_api.machine != kProbeMachine || fixture_api.reserved || !fixture_api.set_fixture ||
            !fixture_api.get_state || !fixture_api.clear_fixture) return false;
        slot = OwnCreateSlot(); original = ReadCreateSlot(slot);
        if (!original) return Step(E_UNEXPECTED, "locate original slot");
        PulseBridgeProbeConfig config{};
        config.size = sizeof(config); config.version = PULSE_BRIDGE_PROBE_ABI_VERSION; config.machine = kProbeMachine;
        config.target_exe = GetModuleHandleW(nullptr); config.host_path = host.c_str();
        config.host_path_units = static_cast<uint32_t>(host.size());
        const HRESULT disabled = api.install(&config);
        const HRESULT observed = ReadState(before);
        std::cout << "[DIAG] probe disabled-install flags=" << before.flags << std::endl;
        if (!Step(disabled, "disabled Install") || FAILED(observed) || !(before.flags & PULSE_BRIDGE_PROBE_PINNED) ||
            !(before.flags & PULSE_BRIDGE_PROBE_INSTALLED) || (before.flags & PULSE_BRIDGE_PROBE_ENABLED)) return false;
        std::vector<uint8_t> wire;
        if (Encode(request, wire) != CodecError::None) return Step(E_INVALIDARG, "Encode fixture");
        PulseBridgeProbeFixture profile{};
        profile.size = sizeof(profile); profile.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION; profile.machine = kProbeMachine;
        profile.bytes = wire.data(); profile.byte_count = static_cast<uint32_t>(wire.size());
        if (!Step(fixture_api.set_fixture(&profile), "SetFixture") || !Step(ReadFixture(), "initial fixture state")) return false;
        if (!Step(api.deactivate(), "disabled Deactivate") || ReadCreateSlot(slot) != original) return false;
        config.flags = PULSE_BRIDGE_PROBE_ENABLE;
        const HRESULT enabled = api.install(&config);
        const HRESULT sampled = ReadState(before);
        std::cout << "[DIAG] probe enabled-install flags=" << before.flags << std::endl;
        installed = SUCCEEDED(enabled) && SUCCEEDED(sampled) && (before.flags & PULSE_BRIDGE_PROBE_PINNED) &&
            (before.flags & PULSE_BRIDGE_PROBE_INSTALLED) && (before.flags & PULSE_BRIDGE_PROBE_ENABLED) &&
            (before.flags & PULSE_BRIDGE_PROBE_PROTECTION_RESTORED) && ReadCreateSlot(slot) != original;
        Step(enabled, "enabled Install");
        return installed;
    }
    bool Owns(IFileDialog* dialog) const {
        if (!dialog || !module) return false;
        const void* vtable = *reinterpret_cast<const void* const*>(dialog);
        HMODULE owner = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vtable), &owner) && owner == module;
    }
    bool Replace(const Message& request) {
        std::vector<uint8_t> bytes;
        if (Encode(request, bytes) != CodecError::None) return false;
        PulseBridgeProbeFixture profile{};
        profile.size = sizeof(profile); profile.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION; profile.machine = kProbeMachine;
        profile.bytes = bytes.data(); profile.byte_count = static_cast<uint32_t>(bytes.size());
        return Step(fixture_api.set_fixture(&profile), "replace selected fixture") && SUCCEEDED(ReadFixture());
    }
    bool Finish() {
        const bool sampled = SUCCEEDED(ReadState(after)) && SUCCEEDED(ReadFixture());
        std::cout << "[DIAG] probe fixture generation=" << fixture.generation << " validations=" << fixture.validation_calls
                  << " matches=" << fixture.validation_matches << " hosts=" << fixture.bridge_calls
                  << " active=" << fixture.active_bridge_calls << " terminal=" << fixture.last_terminal_status
                  << " native_before=" << before.native_show_calls << " native_after=" << after.native_show_calls << std::endl;
        restored = api.deactivate && SUCCEEDED(api.deactivate()) && original && ReadCreateSlot(slot) == original;
        PulseBridgeProbeState stopped{};
        const bool pinned = SUCCEEDED(ReadState(stopped)) && (stopped.flags & PULSE_BRIDGE_PROBE_PINNED) &&
            !(stopped.flags & PULSE_BRIDGE_PROBE_INSTALLED);
        if (fixture_api.clear_fixture) fixture_api.clear_fixture();
        const HMODULE released = module;
        const bool freed = module && FreeLibrary(module);
        module = nullptr;
        HMODULE resident = nullptr;
        pinned_after_free = pinned && freed && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(released), &resident) && resident == released;
        return sampled;
    }
    ~ProbeFixtureSession() {
        if (module) {
            if (api.deactivate) api.deactivate();
            FreeLibrary(module);
        }
    }
};
void Check(bool ok, const char* name, DWORD error = 0) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name;
    if (!ok) std::cout << " error=" << error;
    std::cout << std::endl;
}
struct Desktop {
    std::wstring name = L"PulsePickerHostTest." + std::to_wstring(GetCurrentProcessId());
    HDESK handle = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    ~Desktop() { if (handle) CloseDesktop(handle); }
};
struct Child {
    PipeHandle process;
    DWORD pid = 0;
    bool Start(const std::wstring& executable, const std::wstring& pipe, const std::wstring& desktop) {
        std::wstring command = L"\"" + executable + L"\" --picker-host " + pipe + L" " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.lpDesktop = const_cast<wchar_t*>(desktop.c_str());
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return false;
        CloseHandle(info.hThread); process.Reset(info.hProcess); pid = info.dwProcessId;
        return true;
    }
    bool Finish(DWORD expected) {
        if (!process || WaitForSingleObject(process.Get(), 15000) != WAIT_OBJECT_0) return false;
        DWORD code = MAXDWORD;
        GetExitCodeProcess(process.Get(), &code);
        if (code != expected) std::cout << "[DIAG] host exit=" << code << " expected=" << expected << std::endl;
        return code == expected;
    }
    ~Child() {
        if (process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.Get(), 99);
            WaitForSingleObject(process.Get(), 5000);
        }
    }
};
struct WindowSearch { DWORD pid; HWND window = nullptr; };
BOOL CALLBACK FindPicker(HWND window, LPARAM param) {
    auto& search = *reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    wchar_t name[128]{}; GetClassNameW(window, name, 128);
    // WM_CREATE creates enabled EDITs before Show initializes its loader and
    // disables input for Navigate. Only a presented window is actionable.
    if ((!search.pid || pid == search.pid) && wcscmp(name, L"PulseFolderPickerWindow") == 0 && IsWindowVisible(window)) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}
HWND WaitPicker(HDESK desktop, const Child& child) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 10000 && WaitForSingleObject(child.process.Get(), 0) == WAIT_TIMEOUT) {
        WindowSearch search{child.pid};
        EnumDesktopWindows(desktop, FindPicker, reinterpret_cast<LPARAM>(&search));
        if (search.window) return search.window;
        Sleep(20);
    }
    return nullptr;
}
void Case(const std::wstring& executable, const Desktop& desktop, const std::filesystem::path& fixture, const char* mode) {
    Server server;
    auto result = Server::Create(server);
    Check(static_cast<bool>(result), "create one-shot host pipe", result.error);
    if (!result) return;
    Child child;
    const bool launched = child.Start(executable, server.Name(), desktop.name);
    Check(launched, "launch pulse picker-host on isolated desktop", GetLastError());
    if (!launched) return;
    Channel channel;
    result = server.Accept(child.process.Get(), channel);
    Check(static_cast<bool>(result), "host identity validated", result.error);
    if (!result) return;
    Message request;
    request.request_id[0] = 1;
    auto& body = std::get<Request>(request.body);
    const auto directory = fixture.parent_path().wstring();
    body.initial_directory.assign(directory.begin(), directory.end());
    const std::string kind(mode);
    const bool save = kind == "save-select" || kind == "save-cancel";
    if (kind == "unsupported") {
        // The wire supports filters, but the bridge intentionally rejects DOS
        // wildcard contracts it cannot reproduce exactly.
        body.required_capabilities |= Filters;
        body.filters = {{u"Unsupported wildcard", u"report?.txt"}};
    }
    if (save) {
        body.operation = Operation::SaveFile;
        body.required_capabilities = Save;
        body.options = OverwritePrompt;
        body.default_extension = u"txt";
    }
    const auto expected_path = save ? fixture.parent_path() / L"new-save-target.txt" : fixture;
    if (save) Check(GetFileAttributesW(expected_path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "Save target absent before picker");
    if (kind == "select" || save) {
        const auto filename = save ? std::wstring(L"new-save-target") : fixture.filename().wstring();
        body.filename.assign(filename.begin(), filename.end());
    }
    result = channel.Send(request, {nullptr, 5000});
    Check(static_cast<bool>(result), "host request sent", result.error);
    if (!result) return;
    if (kind != "unsupported") {
        const HWND picker = WaitPicker(desktop.handle, child);
        Check(picker != nullptr, "real picker window presented");
        if (!picker) return;
        if (kind == "disconnect") {
            channel = Channel{};
            Check(child.Finish(4), "disconnect closes picker and host exits without success reply");
            return;
        }
        if (kind == "select" || kind == "save-select") {
            HWND edit = nullptr;
            const auto start = GetTickCount64();
            while (GetTickCount64() - start < 5000) {
                edit = GetDlgItem(picker, 102);
                if (edit && IsWindowEnabled(edit)) break;
                Sleep(20);
            }
            Check(edit && IsWindowEnabled(edit), "filename input ready");
            if (!edit || !IsWindowEnabled(edit)) return;
            wchar_t actual_text[512]{};
            DWORD_PTR text_result = 0;
            const bool text_read = SendMessageTimeoutW(edit, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(actual_text),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &text_result) != 0;
            const std::wstring expected_name(body.filename.begin(), body.filename.end());
            std::wcout << L"[DIAG] initial filename actual=[" << actual_text << L"] expected=[" << expected_name << L"]" << std::endl;
            Check(text_read && actual_text == expected_name, "request filename preserved in real input", GetLastError());
            Check(PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0) != FALSE, "submit filename Enter queued", GetLastError());
        } else {
            Message cancel{request.request_id, Cancel{}};
            if (kind == "wrong-cancel") cancel.request_id[1] = 1;
            result = channel.Send(cancel, {nullptr, 5000});
            Check(static_cast<bool>(result), "cancel control message sent", result.error);
        }
    }
    Message response;
    result = channel.Receive(response, {nullptr, 15000});
    Check(static_cast<bool>(result), "host terminal response received", result.error);
    if (result) {
        Check(ValidateResponse(request, response), "host response bound to request");
        const auto* answer = std::get_if<Response>(&response.body);
        if (kind == "unsupported") Check(answer && answer->status == Status::Unsupported, "unsupported wildcard contract rejected before UI");
        else if (kind == "cancel" || kind == "save-cancel") Check(answer && answer->status == Status::Cancelled, "matching cancellation returned");
        else if (kind == "wrong-cancel") Check(answer && answer->status == Status::Failed && answer->error_source == ErrorSource::Protocol,
            "wrong cancel ID is protocol failure not user cancellation");
        else {
            const auto path = expected_path.wstring();
            Check(answer && answer->status == Status::Accepted && answer->paths == std::vector<std::u16string>{{path.begin(), path.end()}},
                save ? "new Save path with default extension returned" : "selected local fixture path returned");
        }
    }
    Check(child.Finish(0), "host exits cleanly after response");
    if (save) Check(GetFileAttributesW(expected_path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
        "Save picker did not create target file");
}
void ClientCase(const std::wstring& executable, const Desktop& desktop,
                const std::filesystem::path& fixture, bool save, bool cancel_ui, bool preserve_link = false,
                bool wrapper = false, bool legacy = false, bool iat = false, bool legacy_ansi = false,
                bool activation_ex = false, bool class_factory = false, const std::wstring& probe_path = {}, bool probe_snapshot = false,
                bool advisory_filters = false, bool skip_access_validation = false) {
    Message request;
    request.request_id[0] = save ? 12 : cancel_ui ? 13 : 11;
    auto& body = std::get<Request>(request.body);
    const auto directory = fixture.parent_path().wstring();
    body.initial_directory.assign(directory.begin(), directory.end());
    const auto filename = save ? std::wstring(L"client-save-target") : advisory_filters ? std::wstring(L"selection.txt") : fixture.filename().wstring();
    body.filename.assign(filename.begin(), filename.end());
    if (preserve_link) {
        body.options |= NoDereferenceLinks;
        body.required_capabilities |= PreserveLinks;
    }
    if (save) {
        body.operation = Operation::SaveFile;
        body.required_capabilities = Save;
        body.options = OverwritePrompt;
        body.default_extension = u"txt";
    }
    if (legacy) {
        body.required_capabilities |= Filters;
        body.filters = {{u"Text", u"*.txt"}};
    }
    if (advisory_filters) {
        body.required_capabilities |= Filters | AdvisoryFilters;
        body.options |= AdvisoryFileFilters;
        body.filters = {{u"Text", u"*.txt"}};
    }
    if (skip_access_validation) {
        body.options |= SkipAccessValidation;
        body.required_capabilities |= ExplicitAccessPolicy;
    }
    const auto expected = save ? fixture.parent_path() / L"client-save-target.txt" : fixture;
    std::string acp_directory, acp_filename, acp_expected, acp_title, acp_file_prefix, acp_extension_prefix;
    if (legacy_ansi) {
        const auto text = expected.wstring();
        const bool encodable = StrictAcp(directory, acp_directory) && StrictAcp(filename, acp_filename) &&
            StrictAcp(text, acp_expected) && StrictAcp(expected.filename().wstring(), acp_title) &&
            StrictAcp(text.substr(0, text.find_last_of(L'\\') + 1), acp_file_prefix) &&
            StrictAcp(text.substr(0, text.find_last_of(L'.') + 1), acp_extension_prefix);
        std::cout << "[DIAG] legacy ANSI ACP=" << GetACP() << " exact_roundtrip=" << encodable << std::endl;
        Check(encodable && acp_filename.size() < 1024 && acp_expected.size() + 1 < 1024 && acp_title.size() + 1 < 256,
            "ANSI fixture path is losslessly ACP encodable within byte capacities; otherwise case refused");
        if (!encodable || acp_filename.size() >= 1024 || acp_expected.size() + 1 >= 1024 || acp_title.size() + 1 >= 256) return;
    }
    if (save) Check(GetFileAttributesW(expected.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
                    "client Save target absent before real bridge call");
    struct Results {
        bool caller_desktop = false, controller_desktop = false, owner_created = false;
        bool picker_found = false, input_ready = false, filename_matches = false;
        bool filename_read = false, edit_visible = false, edit_enabled_after = false;
        DWORD filename_read_error = ERROR_SUCCESS, picker_pid = 0, edit_pid = 0;
        DWORD_PTR filename_read_length = 0;
        ULONGLONG readiness_ms = 0, filename_read_ms = 0;
        std::wstring initial_filename;
        bool owner_disabled = false, action_posted = false, owner_restored = false, timed_out = false;
        bool picker_owned = false, picker_over_owner_monitor = false;
        bool wrapper_configured = false, no_cancel_result = false;
        int verified_requests = 0, native_shows = 0;
        HRESULT wrapper_result = E_UNEXPECTED, shell_result = E_UNEXPECTED;
        UINT returned_filter_index = 0;
        HRESULT filter_index_result = E_PENDING;
        BOOL legacy_result = FALSE;
        DWORD legacy_error = CDERR_DIALOGFAILURE;
        bool legacy_contract = false;
        bool iat_installed = false, iat_restored = false, native_created_after = false, wrapper_module = false;
        bool ex_identity = false, ex_references = false;
        HRESULT ex_result = E_UNEXPECTED;
        HRESULT factory_result = E_UNEXPECTED, factory_create_result = E_UNEXPECTED;
        bool factory_identity = false, factory_released_dialog_usable = false;
        bool dll_installed = false, dll_sampled = false, dll_restored = false, dll_pinned = false;
        PulseBridgeProbeState dll_before{}, dll_after{};
        PulseBridgeProbeFixtureState dll_fixture{};
        bool snapshot_registered = false;
        uint64_t snapshot_generation = 0;
        int client_calls = 0;
        std::wstring shell_path;
        Message response;
    } result;
    std::thread caller([&] {
        result.caller_desktop = SetThreadDesktop(desktop.handle) != FALSE;
        if (!result.caller_desktop) return;
        const HRESULT initialized = wrapper ? CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) : S_OK;
        if (FAILED(initialized)) { result.wrapper_result = initialized; return; }
        struct ComScope { bool active; ~ComScope() { if (active) CoUninitialize(); } } com_scope{wrapper};
        HWND owner = CreateWindowW(L"STATIC", L"isolated picker client owner", WS_OVERLAPPED,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        result.owner_created = owner != nullptr;
        if (!owner) return;
        PipeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        PipeHandle finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop || !finished) { DestroyWindow(owner); return; }
        std::thread controller([&] {
            const auto cancel_case = [&] {
                SetEvent(stop.Get());
                // The wrapper deliberately uses its production default client,
                // not an injected bridge callback. Destroy only this test owner
                // to trigger that client's existing owner-loss cancellation.
                if (wrapper || legacy) PostMessageW(owner, WM_CLOSE, 0, 0);
            };
            result.controller_desktop = SetThreadDesktop(desktop.handle) != FALSE;
            if (!result.controller_desktop) { cancel_case(); return; }
            HWND picker = nullptr;
            const auto readiness_start = GetTickCount64();
            const auto deadline = readiness_start + 10000;
            while (GetTickCount64() < deadline && WaitForSingleObject(finished.Get(), 0) == WAIT_TIMEOUT) {
                WindowSearch search{0}; // Only this case's real host runs on this private desktop.
                EnumDesktopWindows(desktop.handle, FindPicker, reinterpret_cast<LPARAM>(&search));
                if (search.window) { picker = search.window; break; }
                Sleep(20);
            }
            result.picker_found = picker != nullptr;
            HWND edit = nullptr;
            while (picker && GetTickCount64() < deadline && WaitForSingleObject(finished.Get(), 0) == WAIT_TIMEOUT) {
                edit = GetDlgItem(picker, 102);
                if (edit && IsWindowEnabled(edit)) break;
                Sleep(20);
            }
            result.input_ready = edit && IsWindowEnabled(edit);
            result.readiness_ms = GetTickCount64() - readiness_start;
            if (picker) GetWindowThreadProcessId(picker, &result.picker_pid);
            if (edit) GetWindowThreadProcessId(edit, &result.edit_pid);
            result.edit_visible = edit && IsWindowVisible(edit);
            result.owner_disabled = !IsWindowEnabled(owner);
            // The host owns its dialog by the caller's window (z-order, no own
            // taskbar button) and opens it on the owner's monitor.
            result.picker_owned = picker && GetWindow(picker, GW_OWNER) == owner;
            result.picker_over_owner_monitor = picker &&
                MonitorFromWindow(picker, MONITOR_DEFAULTTONULL) == MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
            if (result.input_ready) {
                wchar_t text[512]{};
                DWORD_PTR length = 0;
                const auto read_start = GetTickCount64();
                SetLastError(ERROR_SUCCESS);
                result.filename_read = SendMessageTimeoutW(edit, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(text),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &length) != 0;
                result.filename_read_error = result.filename_read ? ERROR_SUCCESS : GetLastError();
                result.filename_read_ms = GetTickCount64() - read_start;
                result.filename_read_length = length;
                result.initial_filename = text;
                result.edit_enabled_after = IsWindowEnabled(edit) != FALSE;
                result.filename_matches = result.filename_read && text == filename;
                if (advisory_filters && result.filename_matches) {
                    const auto manual = fixture.filename().wstring();
                    DWORD_PTR sent = 0;
                    const bool typed = SendMessageTimeoutW(edit, EM_SETSEL, 0, -1, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &sent) &&
                        SendMessageTimeoutW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(manual.c_str()),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &sent);
                    text[0] = 0;
                    result.filename_matches = typed && SendMessageTimeoutW(edit, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(text),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &length) && text == manual;
                    if (!result.filename_matches) { cancel_case(); return; }
                }
                result.action_posted = cancel_ui ? PostMessageW(picker, WM_CLOSE, 0, 0) != FALSE :
                    PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0) != FALSE;
            }
            if (!result.action_posted) { cancel_case(); return; }
            if (WaitForSingleObject(finished.Get(), 15000) != WAIT_OBJECT_0) {
                result.timed_out = true;
                cancel_case(); // Bounded failure cleanup belongs to RunPickerClient's private job.
            }
        });
        if (legacy) {
            namespace shell = pulse::shell_bridge;
            namespace model = pulse::ipc::picker::legacy;
            shell::LegacyDialogServices services;
            services.executable = executable;
            services.open_native = RejectLegacyNative;
            services.save_native = RejectLegacyNative;
            services.native_error = RejectLegacyError;
            const DWORD flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
                (save ? OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOTESTFILECREATE : OFN_FILEMUSTEXIST);
            const uint32_t obligations = model::CallerDirectoryPolicy | model::LegacyFileValidation |
                (save ? model::SaveReadOnlyCompatibility : 0u);
            services.verified_abi_checks = [&](const model::Prepared& candidate) -> uint32_t {
                const auto* value = std::get_if<Request>(&candidate.request.body);
                const bool exact_fixture = value && *value == body && candidate.flags == flags &&
                    candidate.file_title_present && candidate.default_extension == body.default_extension &&
                    candidate.required_abi_checks == obligations;
                if (exact_fixture) ++result.verified_requests;
                return exact_fixture ? obligations : 0u; // This exact isolated fixture only, not production certification.
            };
            // run_bridge stays absent: adapter -> real RunPickerClient -> real host.
            if (legacy_ansi) {
                shell::LegacyAnsiDialogServices ansi_services;
                ansi_services.executable = executable;
                ansi_services.open_native = RejectLegacyAnsiNative;
                ansi_services.save_native = RejectLegacyAnsiNative;
                ansi_services.native_error = RejectLegacyError;
                ansi_services.verified_abi_checks = services.verified_abi_checks;
                shell::LegacyAnsiFileDialogAdapter adapter(std::move(ansi_services));
                std::array<char, 1024> file_buffer{};
                std::array<char, 256> title_buffer{};
                file_buffer.fill('!'); title_buffer.fill('?');
                std::copy(acp_filename.begin(), acp_filename.end(), file_buffer.begin());
                file_buffer[acp_filename.size()] = 0;
                const auto file_before = file_buffer;
                const auto title_before = title_buffer;
                OPENFILENAMEA ofn{};
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
                ofn.lpstrFile = file_buffer.data(); ofn.nMaxFile = static_cast<DWORD>(file_buffer.size());
                ofn.lpstrFileTitle = title_buffer.data(); ofn.nMaxFileTitle = static_cast<DWORD>(title_buffer.size());
                ofn.lpstrFilter = "Text\0*.txt\0\0"; ofn.nFilterIndex = 1;
                ofn.lpstrInitialDir = acp_directory.c_str(); ofn.lpstrDefExt = save ? "txt" : nullptr;
                ofn.Flags = flags; ofn.nFileOffset = 91; ofn.nFileExtension = 92;
                std::array<std::byte, sizeof(OPENFILENAMEA)> structure_before{};
                std::memcpy(structure_before.data(), &ofn, sizeof(ofn));
                legacy_native_calls = &result.native_shows;
                result.legacy_result = save ? adapter.Save(&ofn) : adapter.Open(&ofn);
                result.legacy_error = adapter.ExtendedError();
                legacy_native_calls = nullptr;
                result.legacy_contract = cancel_ui ? file_buffer == file_before && title_buffer == title_before &&
                    std::memcmp(structure_before.data(), &ofn, sizeof(ofn)) == 0 :
                    result.legacy_result == TRUE && std::string(file_buffer.data()) == acp_expected &&
                    std::string(title_buffer.data()) == acp_title && ofn.nFileOffset == acp_file_prefix.size() &&
                    ofn.nFileExtension == acp_extension_prefix.size() && ofn.nFilterIndex == 1 && ofn.Flags == flags &&
                    file_buffer[acp_expected.size() + 1] == file_before[acp_expected.size() + 1] &&
                    title_buffer[acp_title.size() + 1] == title_before[acp_title.size() + 1];
            } else {
                shell::LegacyFileDialogAdapter adapter(std::move(services));
                std::array<wchar_t, 1024> file_buffer{};
                std::array<wchar_t, 256> title_buffer{};
                file_buffer.fill(L'!'); title_buffer.fill(L'?');
                std::copy(filename.begin(), filename.end(), file_buffer.begin());
                file_buffer[filename.size()] = 0;
                const auto file_before = file_buffer;
                const auto title_before = title_buffer;
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = owner;
                ofn.lpstrFile = file_buffer.data(); ofn.nMaxFile = static_cast<DWORD>(file_buffer.size());
                ofn.lpstrFileTitle = title_buffer.data(); ofn.nMaxFileTitle = static_cast<DWORD>(title_buffer.size());
                ofn.lpstrFilter = L"Text\0*.txt\0\0"; ofn.nFilterIndex = 1;
                ofn.lpstrInitialDir = directory.c_str(); ofn.lpstrDefExt = save ? L"txt" : nullptr;
                ofn.Flags = flags; ofn.nFileOffset = 91; ofn.nFileExtension = 92;
                legacy_native_calls = &result.native_shows;
                result.legacy_result = save ? adapter.Save(&ofn) : adapter.Open(&ofn);
                result.legacy_error = adapter.ExtendedError();
                legacy_native_calls = nullptr;
                const auto expected_text = expected.wstring();
                result.legacy_contract = cancel_ui ?
                    file_buffer == file_before && title_buffer == title_before && ofn.nFileOffset == 91 &&
                        ofn.nFileExtension == 92 && ofn.nFilterIndex == 1 && ofn.Flags == flags :
                    result.legacy_result == TRUE && std::wstring(file_buffer.data()) == expected_text &&
                        std::wstring(title_buffer.data()) == expected.filename().wstring() &&
                        ofn.nFileOffset == expected_text.find_last_of(L'\\') + 1 &&
                        ofn.nFileExtension == expected_text.find_last_of(L'.') + 1 && ofn.nFilterIndex == 1 &&
                        ofn.Flags == flags && file_buffer[expected_text.size() + 1] == file_before[expected_text.size() + 1];
            }
        } else if (!wrapper) result.response = RunPickerClient(executable, request, owner, stop.Get());
        else {
            namespace shell = pulse::shell_bridge;
            using Microsoft::WRL::ComPtr;
            shell::FileDialogBridgeServices services;
            services.executable = executable;
            const auto evidence = std::make_shared<BridgeEvidence>();
            evidence->expected = body;
            services.validation_verified = [evidence, skip_access_validation](const shell::ContractDecision& candidate) {
                const auto* value = candidate.request ? std::get_if<Request>(&candidate.request->body) : nullptr;
                const bool exact_fixture = value && *value == evidence->expected && (!skip_access_validation ||
                    (candidate.effective_options && *candidate.effective_options == 0x02001948u &&
                     candidate.validation_category == shell::ValidationCategory::OpenSkipAccessValidation));
                if (exact_fixture) ++evidence->verified;
                return exact_fixture; // Test-only certification, never a production profile.
            };
            services.show_native = [evidence](IFileDialog*, HWND) { ++evidence->native; return E_UNEXPECTED; };
            // IAT configuration owns all captures. This thin counter invokes the
            // real client, while --wrapper keeps its absent production default.
            if (iat) services.run_bridge = [evidence, executable](const Message& candidate, HWND caller_owner, HANDLE cancel) {
                ++evidence->client_calls;
                return RunPickerClient(executable, candidate, caller_owner, cancel);
            };
            const auto factory = save ? shell::DialogFactory::Save : shell::DialogFactory::Open;
            ComPtr<IFileDialog> dialog;
            ProbeFixtureSession probe;
            // &CoCreateInstance is a language-level function address: optimized
            // comparisons may be constant-folded despite IAT mutation. Inspect
            // the actual slot through an explicit OS memory read instead.
            void* import_slot = iat ? OwnCreateSlot() : nullptr;
            void* original_import = ReadCreateSlot(import_slot);
            HRESULT hr = S_OK;
            if (!probe_path.empty()) {
                result.dll_installed = probe.Begin(probe_path, executable, request);
                hr = result.dll_installed ? DirectCreateDialog(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                    IID_PPV_ARGS(&dialog)) : E_FAIL;
                result.wrapper_module = probe.Owns(dialog.Get());
                if (!result.wrapper_module) hr = E_UNEXPECTED;
            } else if (iat) {
                const auto installed = shell::InstallSelfFileDialogIat(GetModuleHandleW(nullptr), services, true);
                void* current_import = ReadCreateSlot(import_slot);
                std::cout << "[DIAG] IAT install HRESULT=0x" << std::hex << static_cast<unsigned long>(installed.status)
                          << std::dec << " installed=" << installed.installed << " protection_restored=" << installed.protection_restored
                          << " slot=" << import_slot << " original=" << original_import << " current=" << current_import
                          << " source_address=" << reinterpret_cast<void*>(ImportedCreateAddress()) << std::endl;
                result.iat_installed = SUCCEEDED(installed.status) && installed.installed && installed.protection_restored &&
                    original_import && current_import && current_import != original_import;
                hr = result.iat_installed ? DirectCreateDialog(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                    IID_PPV_ARGS(&dialog)) : E_FAIL;
                if (SUCCEEDED(hr) && dialog) {
                    // Controlled MSVC test: the linked wrapper's vtable is
                    // in this EXE. Reject native activation rollback before any
                    // Show; probing unknown QI would itself force native mode.
                    result.wrapper_module = IsLinkedWrapper(dialog.Get());
                    if (!result.wrapper_module) hr = E_UNEXPECTED;
                }
            } else if (activation_ex) {
                shell::FileDialogActivationEx activation;
                activation.enabled = true;
                activation.services = services;
                // original remains the real CoCreateInstanceEx. No Ex IAT or
                // native Show seam is installed outside this explicit adapter.
                MULTI_QI interfaces[3] = {
                    {&IID_IUnknown, nullptr, S_OK},
                    {&IID_IFileDialog, nullptr, S_OK},
                    {save ? &IID_IFileSaveDialog : &IID_IFileOpenDialog, nullptr, S_OK}
                };
                APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
                const HRESULT apartment_hr = CoGetApartmentType(&apartment, &qualifier);
                std::cout << "[DIAG] Ex eligibility inputs enabled=" << activation.enabled
                          << " apartment_hr=0x" << std::hex << static_cast<unsigned long>(apartment_hr)
                          << std::dec << " apartment=" << apartment
                          << " local_INPROC_no_outer_server=1 known_IIDs=3 input_pItf_null_hr_zero=1" << std::endl;
                result.ex_result = shell::ActivateFileDialogEx(activation,
                    save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, nullptr, 3, interfaces);
                std::cout << "[DIAG] real Ex aggregate HRESULT=0x" << std::hex << static_cast<unsigned long>(result.ex_result)
                          << std::dec << std::endl;
                ComPtr<IUnknown> identities[3];
                bool all = result.ex_result == S_OK;
                for (size_t i = 0; i < 3; ++i) {
                    const bool valid = interfaces[i].hr == S_OK && interfaces[i].pItf &&
                        SUCCEEDED(interfaces[i].pItf->QueryInterface(IID_PPV_ARGS(&identities[i])));
                    std::cout << "[DIAG] Ex result[" << i << "] hr=0x" << std::hex << static_cast<unsigned long>(interfaces[i].hr)
                              << std::dec << " interface=" << static_cast<void*>(interfaces[i].pItf)
                              << " identity=" << static_cast<void*>(identities[i].Get()) << " valid=" << valid << std::endl;
                    all = all && valid;
                }
                result.ex_identity = all && identities[0].Get() == identities[1].Get() && identities[0].Get() == identities[2].Get();
                hr = result.ex_identity ? interfaces[1].pItf->QueryInterface(IID_PPV_ARGS(&dialog)) : E_UNEXPECTED;
                for (auto& identity : identities) identity.Reset();
                result.ex_references = SUCCEEDED(hr) && dialog;
                for (auto& item : interfaces) {
                    if (item.pItf) { item.pItf->Release(); item.pItf = nullptr; }
                    FILEOPENDIALOGOPTIONS remaining_options{};
                    const HRESULT options_hr = dialog ? dialog->GetOptions(&remaining_options) : E_POINTER;
                    std::cout << "[DIAG] Ex after Release[" << (&item - interfaces) << "] GetOptions=0x" << std::hex
                              << static_cast<unsigned long>(options_hr) << " options=0x" << remaining_options << std::dec << std::endl;
                    result.ex_references = result.ex_references && SUCCEEDED(options_hr);
                }
                result.wrapper_module = IsLinkedWrapper(dialog.Get());
                std::cout << "[DIAG] Ex same_identity=" << result.ex_identity << " references_survive=" << result.ex_references
                          << " wrapper_module=" << result.wrapper_module << std::endl;
                // An activation rollback must not accidentally open native UI.
                if (!result.ex_references || !result.wrapper_module) hr = E_UNEXPECTED;
            } else if (class_factory) {
                shell::FileDialogClassActivation activation;
                activation.enabled = true;
                activation.services = services;
                // Keep the real default CoGetClassObject. Only known factory
                // interfaces are queried; no factory registration or IAT hook.
                ComPtr<IClassFactory> obtained, roundtrip_factory;
                ComPtr<IUnknown> identity, roundtrip_identity;
                result.factory_result = shell::ActivateFileDialogClass(activation,
                    save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, CLSCTX_INPROC_SERVER,
                    nullptr, IID_PPV_ARGS(&obtained));
                result.factory_identity = result.factory_result == S_OK && obtained &&
                    SUCCEEDED(obtained.As(&identity)) && SUCCEEDED(identity.As(&roundtrip_factory)) &&
                    SUCCEEDED(roundtrip_factory.As(&roundtrip_identity)) && identity.Get() == roundtrip_identity.Get();
                result.factory_create_result = result.factory_identity ?
                    obtained->CreateInstance(nullptr, IID_PPV_ARGS(&dialog)) : E_UNEXPECTED;
                roundtrip_identity.Reset(); roundtrip_factory.Reset(); identity.Reset(); obtained.Reset();
                FILEOPENDIALOGOPTIONS retained_options{};
                const HRESULT retained = dialog ? dialog->GetOptions(&retained_options) : E_POINTER;
                result.factory_released_dialog_usable = result.factory_create_result == S_OK && SUCCEEDED(retained);
                result.wrapper_module = IsLinkedWrapper(dialog.Get());
                std::cout << "[DIAG] class factory activation=0x" << std::hex << static_cast<unsigned long>(result.factory_result)
                          << " CreateInstance=0x" << static_cast<unsigned long>(result.factory_create_result)
                          << " post-factory-release GetOptions=0x" << static_cast<unsigned long>(retained) << std::dec
                          << " same_factory_identity=" << result.factory_identity
                          << " wrapper_module=" << result.wrapper_module << std::endl;
                hr = result.factory_released_dialog_usable && result.wrapper_module ? S_OK : E_UNEXPECTED;
            } else hr = shell::CreatePulseFileDialog(factory, services, IID_PPV_ARGS(&dialog));
            ComPtr<IShellItem> initial_folder;
            if (SUCCEEDED(hr)) hr = shell::CreatePickerShellItem(Operation::PickFolder, directory, &initial_folder);
            auto dialog_options = shell::DefaultDialogOptions(factory) | FOS_FORCEFILESYSTEM | FOS_DONTADDTORECENT;
            if (preserve_link) dialog_options |= FOS_NODEREFERENCELINKS;
            if (skip_access_validation) dialog_options |= FOS_NOVALIDATE;
            if (SUCCEEDED(hr)) hr = dialog->SetOptions(dialog_options);
            if (SUCCEEDED(hr) && advisory_filters) {
                const COMDLG_FILTERSPEC filters[] = {{L"Text", L"*.txt"}};
                hr = dialog->SetFileTypes(1, filters);
                if (SUCCEEDED(hr)) hr = dialog->SetFileTypeIndex(1);
            }
            if (SUCCEEDED(hr)) hr = dialog->SetFolder(initial_folder.Get());
            if (SUCCEEDED(hr)) hr = dialog->SetFileName(filename.c_str());
            if (SUCCEEDED(hr) && save) hr = dialog->SetDefaultExtension(L"txt");
            if (SUCCEEDED(hr) && probe_snapshot) {
                const uint64_t previous_generation = probe.fixture.generation;
                Message replacement = request;
                replacement.request_id[1] = 0x5a;
                std::get<Request>(replacement.body).filename += u"-profile-B";
                // A is already captured by the configured old wrapper. B differs
                // in exactly one body field and must never replace that snapshot.
                result.snapshot_registered = probe.Replace(replacement) && probe.fixture.generation != previous_generation;
                result.snapshot_generation = probe.fixture.generation;
                if (!result.snapshot_registered) hr = E_FAIL;
            }
            result.wrapper_configured = SUCCEEDED(hr);
            result.wrapper_result = SUCCEEDED(hr) ? dialog->Show(owner) : hr;
            if (dialog) {
                if (advisory_filters && SUCCEEDED(result.wrapper_result))
                    result.filter_index_result = dialog->GetFileTypeIndex(&result.returned_filter_index);
                ComPtr<IShellItem> item;
                result.shell_result = dialog->GetResult(&item);
                result.no_cancel_result = FAILED(result.shell_result) && !item;
                if (SUCCEEDED(result.shell_result) && item) {
                    PWSTR path = nullptr;
                    result.shell_result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
                    if (SUCCEEDED(result.shell_result) && path) result.shell_path = path;
                    CoTaskMemFree(path);
                }
            }
            if (!probe_path.empty()) {
                dialog.Reset();
                result.dll_sampled = probe.Finish();
                result.dll_before = probe.before; result.dll_after = probe.after; result.dll_fixture = probe.fixture;
                result.dll_restored = probe.restored; result.dll_pinned = probe.pinned_after_free;
                result.verified_requests = static_cast<int>(probe.fixture.validation_matches);
                result.native_shows = static_cast<int>(probe.after.native_show_calls - probe.before.native_show_calls);
            } else if (iat) {
                dialog.Reset();
                const auto removed = shell::UninstallSelfFileDialogIat();
                void* restored_import = ReadCreateSlot(import_slot);
                std::cout << "[DIAG] IAT uninstall HRESULT=0x" << std::hex << static_cast<unsigned long>(removed.status)
                          << std::dec << " installed=" << removed.installed << " protection_restored=" << removed.protection_restored
                          << " original=" << original_import << " current=" << restored_import << std::endl;
                result.iat_restored = SUCCEEDED(removed.status) && !removed.installed && removed.protection_restored &&
                    original_import && restored_import == original_import;
                // Only create after verified restoration, never call native Show.
                if (result.iat_restored) {
                    ComPtr<IFileDialog> native;
                    ComPtr<IOleWindow> native_window;
                    result.native_created_after = SUCCEEDED(DirectCreateDialog(CLSID_FileOpenDialog, IID_PPV_ARGS(&native))) &&
                        SUCCEEDED(native.As(&native_window));
                }
            }
            if (probe_path.empty()) {
                result.verified_requests = evidence->verified.load();
                result.native_shows = evidence->native.load();
            }
            result.client_calls = evidence->client_calls.load();
        }
        SetEvent(finished.Get());
        controller.join();
        result.owner_restored = IsWindow(owner) && IsWindowEnabled(owner);
        DestroyWindow(owner);
    });
    caller.join();
    Check(result.caller_desktop && result.controller_desktop && result.owner_created,
          "real client caller and controller stay on isolated desktop");
    std::wcout << L"[DIAG] filename readiness save=" << save << L" cancel=" << cancel_ui
               << L" picker_found=" << result.picker_found << L" input_ready=" << result.input_ready
               << L" visible=" << result.edit_visible << L" enabled_after=" << result.edit_enabled_after
               << L" picker_pid=" << result.picker_pid << L" edit_pid=" << result.edit_pid
               << L" ready_ms=" << result.readiness_ms << L" read_ok=" << result.filename_read
               << L" read_error=" << result.filename_read_error << L" read_ms=" << result.filename_read_ms
               << L" read_length=" << result.filename_read_length << L" expected=[" << filename
               << L"] initial_actual=[" << result.initial_filename << L"] final_match=" << result.filename_matches << std::endl;
    if (const auto* reply = std::get_if<Response>(&result.response.body))
        std::wcout << L"[DIAG] client response status=" << static_cast<int>(reply->status) << L" source=" << static_cast<int>(reply->error_source)
                   << L" error=" << reply->error_code << std::endl;
    Check(result.picker_found && result.input_ready && result.filename_matches,
          "RunPickerClient launches real Pulse UI with ready requested filename", result.filename_read_error);
    Check(result.owner_disabled, "real client owner is modal while host UI is open");
    Check(result.picker_owned, "host dialog is owned by the caller window");
    Check(result.picker_over_owner_monitor, "host dialog opens on the caller window's monitor");
    Check(result.action_posted && !result.timed_out, "real UI action returns through client without watchdog cancellation");
    const auto* response = std::get_if<Response>(&result.response.body);
    const std::wstring expected_path = expected.wstring();
    if (legacy) {
        std::cout << "[DIAG] legacy " << (legacy_ansi ? "ANSI bytes" : "Unicode units") << " BOOL=" << result.legacy_result << " ExtendedError=" << result.legacy_error
                  << " gate=" << result.verified_requests << " native=" << result.native_shows << std::endl;
        Check(result.verified_requests == 1 && result.native_shows == 0,
            "legacy exact-fixture gate uses real client without native fallback");
        Check(result.legacy_error == 0 && (cancel_ui ? result.legacy_result == FALSE : result.legacy_result == TRUE),
            cancel_ui ? "legacy real UI Cancel returns FALSE and zero extended error" :
            save ? "legacy real Save returns TRUE and zero extended error" : "legacy real Open returns TRUE and zero extended error");
        Check(result.legacy_contract, cancel_ui ? "legacy Cancel preserves all output buffers and fields" :
            "legacy real response commits exact file title offsets index flags and preserves trailing buffer");
    } else if (wrapper) {
        if (!probe_path.empty()) {
            const auto& stats = result.dll_fixture;
            const auto expected_terminal = cancel_ui ? Status::Cancelled : Status::Accepted;
            Check(result.dll_installed && result.wrapper_module, "C ABI fixture install returns verified DLL wrapper before Show");
            if (probe_snapshot) {
                auto replacement_id = request.request_id; replacement_id[1] = 0x5a;
                const RequestId zero_id{};
                Check(result.snapshot_registered && result.dll_sampled && stats.flags == PULSE_BRIDGE_PROBE_FIXTURE_PRESENT &&
                    stats.generation == result.snapshot_generation &&
                    std::equal(replacement_id.begin(), replacement_id.end(), stats.fixture_request_id),
                    "replacement profile B remains selected throughout old wrapper Show");
                Check(!stats.validation_calls && !stats.validation_matches && !stats.bridge_calls && !stats.active_bridge_calls &&
                    !stats.accepted && !stats.cancelled && !stats.unsupported && !stats.failed && !stats.rejected_dispatches &&
                    !stats.last_terminal_status && !stats.last_error_source && !stats.last_error_code &&
                    std::equal(zero_id.begin(), zero_id.end(), stats.last_request_id) &&
                    result.dll_before.native_show_calls == result.dll_after.native_show_calls && !result.dll_after.active_native_shows &&
                    result.wrapper_result == S_OK && SUCCEEDED(result.shell_result) && result.shell_path == expected_path &&
                    result.filename_matches && result.action_posted && !result.timed_out,
                    "old wrapper returns actual A result without using selected B or fixed native seam");
                // Actual A filename, host interaction and Shell result are checked
                // by the shared driver below; no old-profile counters are exposed.
            } else {
                Check(result.dll_sampled && stats.flags == PULSE_BRIDGE_PROBE_FIXTURE_PRESENT && stats.validation_calls == 1 &&
                    stats.validation_matches == 1 && stats.bridge_calls == 1 && !stats.active_bridge_calls && !stats.rejected_dispatches &&
                    result.dll_before.native_show_calls == result.dll_after.native_show_calls && !result.dll_after.active_native_shows,
                    "DLL exact body matches once and runs one real host without fixed-native seam");
                Check(stats.last_terminal_status == static_cast<uint32_t>(expected_terminal) && !stats.last_error_source && !stats.last_error_code &&
                    stats.accepted == (cancel_ui ? 0u : 1u) && stats.cancelled == (cancel_ui ? 1u : 0u) && !stats.failed && !stats.unsupported,
                    "DLL terminal evidence matches actual accepted or cancelled result");
                const RequestId zero_id{};
                Check(std::equal(request.request_id.begin(), request.request_id.end(), stats.fixture_request_id) &&
                    !std::equal(request.request_id.begin(), request.request_id.end(), stats.last_request_id) &&
                    !std::equal(zero_id.begin(), zero_id.end(), stats.last_request_id),
                    "DLL matches body while retaining distinct fixture and real wrapper request IDs");
            }
            Check(result.dll_restored && result.dll_pinned, "DLL deactivation restores IAT and FreeLibrary truthfully retains permanent PIN");
        }
        if (class_factory) {
            Check(result.factory_result == S_OK && result.factory_identity,
                "real class activation returns factory with stable IUnknown identity", static_cast<DWORD>(result.factory_result));
            Check(result.factory_create_result == S_OK && result.factory_released_dialog_usable && result.wrapper_module,
                "factory CreateInstance yields linked wrapper usable after all factory references release",
                static_cast<DWORD>(result.factory_create_result));
        }
        if (activation_ex) {
            Check(result.ex_result == S_OK && result.ex_identity,
                "real CoCreateInstanceEx MULTI_QI returns three successful interfaces with one IUnknown identity",
                static_cast<DWORD>(result.ex_result));
            Check(result.ex_references && result.wrapper_module,
                "retained dialog survives each MULTI_QI Release and remains linked wrapper before Show");
        }
        if (iat) {
            Check(result.iat_installed, "own EXE static CoCreateInstance import installed before actual activation");
            Check(result.wrapper_module, "static activation returns linked wrapper before any Show is allowed");
            Check(result.client_calls == 1, "one IAT activation launches exactly one real client host operation");
            Check(result.iat_restored && result.native_created_after,
                "IAT uninstall restores original import and native-only creation without Show");
        }
        std::wcout << L"[DIAG] wrapper Show=" << std::hex << static_cast<unsigned long>(result.wrapper_result)
                   << L" Shell=" << static_cast<unsigned long>(result.shell_result) << std::dec
                   << L" gate=" << result.verified_requests << L" native=" << result.native_shows
                   << L" path=[" << result.shell_path << L"]" << std::endl;
        if (!probe_snapshot) Check(result.wrapper_configured && result.verified_requests == 1 && result.native_shows == 0,
            "wrapper fixture-only gate routes through real client without native Show", static_cast<DWORD>(result.wrapper_result));
        Check(cancel_ui ? result.wrapper_result == HRESULT_FROM_WIN32(ERROR_CANCELLED) && result.no_cancel_result :
              result.wrapper_result == S_OK && SUCCEEDED(result.shell_result) && result.shell_path == expected_path,
              cancel_ui ? "wrapper real UI Cancel returns cancellation with no Shell result" :
              save ? "wrapper real Save returns new target Shell item" : "wrapper real Open returns selected Shell item",
              static_cast<DWORD>(result.wrapper_result));
        if (advisory_filters && !cancel_ui) Check(result.filter_index_result == S_OK && result.returned_filter_index == 1,
            "manual outside-filter filename preserves original filter index without switching to All files");
    } else Check(ValidateResponse(request, result.response) && response &&
          (cancel_ui ? response->status == Status::Cancelled :
           response->status == Status::Accepted && response->paths ==
               std::vector<std::u16string>{{expected_path.begin(), expected_path.end()}}),
          cancel_ui ? "real UI cancel traverses client and host" :
          save ? "real client Save returns default-extension target" :
          preserve_link ? "real client NoDereferenceLinks returns shortcut itself rather than target" :
          "real client Open returns selected fixture");
    if (skip_access_validation && !cancel_ui && SUCCEEDED(result.wrapper_result)) {
        PipeHandle selected(CreateFileW(result.shell_path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        PipeHandle original(CreateFileW(fixture.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        BY_HANDLE_FILE_INFORMATION a{}, b{};
        Check(selected && original && GetFileInformationByHandle(selected.Get(), &a) && GetFileInformationByHandle(original.Get(), &b) &&
            a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow,
            "NOVALIDATE Shell result identifies the same owned file", GetLastError());
    }
    Check(result.owner_restored, "real client restores owner after terminal result");
    if (save) Check(GetFileAttributesW(expected.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND,
                    "real client Save does not create target file");
}

// This guard only receives a CREATE_NEW fixture in this run's exclusive directory.
class HostFixtureAcl {
public:
    explicit HostFixtureAcl(const std::filesystem::path& path) : path_(path) {}
    ~HostFixtureAcl() {
        Restore();
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        if (original_) LocalFree(original_);
    }
    bool Install() {
        file_ = CreateFileW(path_.c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (file_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1) return false;
        if (GetSecurityInfo(file_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
            &original_acl_, nullptr, &original_) != ERROR_SUCCESS) return false;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(original_, &control_, &revision)) return false;
        LPWSTR sddl = nullptr;
        if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(original_, SDDL_REVISION_1,
            DACL_SECURITY_INFORMATION, &sddl, nullptr)) return false;
        // Persist recovery evidence before changing the new file's DACL.
        const auto journal_path = path_.parent_path() / L"acl-recovery.txt";
        HANDLE journal = CreateFileW(journal_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        const std::wstring record = L"fixture=" + path_.wstring() + L"\r\noriginal_dacl=" + sddl + L"\r\n";
        LocalFree(sddl);
        DWORD written = 0;
        const DWORD bytes = static_cast<DWORD>(record.size() * sizeof(wchar_t));
        const bool persisted = journal != INVALID_HANDLE_VALUE && WriteFile(journal, record.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(journal);
        if (journal != INVALID_HANDLE_VALUE) CloseHandle(journal);
        if (!persisted) return false;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        std::vector<BYTE> user(needed);
        const bool token_ok = GetTokenInformation(token, TokenUser, user.data(), needed, &needed) != FALSE;
        CloseHandle(token);
        if (!token_ok) return false;
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_READ_ATTRIBUTES | SYNCHRONIZE | READ_CONTROL | WRITE_DAC | DELETE;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
        entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
        PACL restricted = nullptr;
        if (SetEntriesInAclW(1, &entry, nullptr, &restricted) != ERROR_SUCCESS) return false;
        const DWORD result = SetSecurityInfo(file_, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, restricted, nullptr);
        LocalFree(restricted);
        changed_ = result == ERROR_SUCCESS;
        std::printf("[FACT] ACL fixture=%ls SetSecurityInfo=%lu; current SID retains attributes/WRITE_DAC/DELETE, read-data not granted\n",
            path_.c_str(), result);
        std::fflush(stdout);
        return changed_;
    }
    bool Restore() {
        std::lock_guard lock(mutex_);
        if (!changed_) return true;
        const DWORD result = SetSecurityInfo(file_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION |
            ((control_ & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            nullptr, nullptr, original_acl_, nullptr);
        PSECURITY_DESCRIPTOR verified = nullptr;
        PACL acl = nullptr;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        const DWORD queried = GetSecurityInfo(file_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &acl, nullptr, &verified);
        const bool same_acl = queried == ERROR_SUCCESS &&
            ((!acl && !original_acl_) || (acl && original_acl_ && acl->AclSize == original_acl_->AclSize &&
                std::memcmp(acl, original_acl_, acl->AclSize) == 0));
        const bool ok = result == ERROR_SUCCESS && same_acl && GetSecurityDescriptorControl(verified, &control, &revision) &&
            (control & SE_DACL_PROTECTED) == (control_ & SE_DACL_PROTECTED);
        if (verified) LocalFree(verified);
        if (ok) changed_ = false;
        std::printf("[FACT] ACL restore fixture=%ls set=%lu query=%lu exact_dacl_and_protection=%d\n",
            path_.c_str(), result, queried, ok);
        std::fflush(stdout);
        return ok;
    }
private:
    std::filesystem::path path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR original_ = nullptr;
    PACL original_acl_ = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control_ = 0;
    bool changed_ = false;
    std::mutex mutex_;
};

bool CreateShortcutFixture(const std::filesystem::path& shortcut, const std::filesystem::path& target) {
    if (GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    IShellLinkW* link = nullptr;
    IPersistFile* persist = nullptr;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(result)) result = link->SetPath(target.c_str());
    if (SUCCEEDED(result)) result = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(result)) result = persist->Save(shortcut.c_str(), TRUE);
    if (persist) persist->Release();
    if (link) link->Release();
    CoUninitialize();
    return SUCCEEDED(result);
}

}
int wmain(int argc, wchar_t** argv) {
    const bool wrapper_only = argc == 3 && std::wstring(argv[2]) == L"--wrapper";
    const bool wrapper_cancel_only = argc == 3 && std::wstring(argv[2]) == L"--wrapper-cancel";
    const bool wrapper_links_only = argc == 3 && std::wstring(argv[2]) == L"--wrapper-links";
    const bool wrapper_filters_only = argc == 3 && std::wstring(argv[2]) == L"--wrapper-filters";
    const bool wrapper_novalidate = argc == 3 && std::wstring(argv[2]) == L"--wrapper-novalidate";
    const bool legacy_only = argc == 3 && std::wstring(argv[2]) == L"--legacy";
    const bool legacy_ansi_only = argc == 3 && std::wstring(argv[2]) == L"--legacy-ansi";
    const bool iat_only = argc == 3 && std::wstring(argv[2]) == L"--iat";
    const bool ex_only = argc == 3 && std::wstring(argv[2]) == L"--ex";
    const bool factory_only = argc == 3 && std::wstring(argv[2]) == L"--factory";
    const bool session_cold = argc == 3 && std::wstring(argv[2]) == L"--session-cold";
    const bool session_standby = argc == 3 && std::wstring(argv[2]) == L"--session-standby";
    const bool standby_only = session_standby || (argc == 3 && std::wstring(argv[2]) == L"--standby");
    const bool dll_only = argc == 4 && std::wstring(argv[2]) == L"--dll" && std::filesystem::path(argv[3]).is_absolute();
    const bool dll_filters_only = argc == 4 && std::wstring(argv[2]) == L"--dll-filters" && std::filesystem::path(argv[3]).is_absolute();
    const bool dll_novalidate = argc == 4 && std::wstring(argv[2]) == L"--dll-novalidate" && std::filesystem::path(argv[3]).is_absolute();
    const bool novalidate_only = wrapper_novalidate || dll_novalidate;
    const std::wstring probe_path = dll_only || dll_filters_only || dll_novalidate ? argv[3] : L"";
    if (argc != 2 && !session_cold && !wrapper_only && !wrapper_cancel_only && !wrapper_links_only && !wrapper_filters_only && !legacy_only && !iat_only && !legacy_ansi_only && !ex_only && !factory_only && !standby_only && !dll_only && !dll_filters_only && !wrapper_novalidate && !dll_novalidate) { std::cerr << "Usage: pulse_picker_host_test <pulse.exe> [--session-cold|--session-standby|--standby|--wrapper|--wrapper-cancel|--wrapper-links|--wrapper-filters|--wrapper-novalidate|--legacy|--legacy-ansi|--iat|--ex|--factory|--dll <absolute same-bitness DLL>|--dll-filters <absolute same-bitness DLL>|--dll-novalidate <absolute same-bitness DLL>]\n"; return 2; }
    Desktop desktop;
    Check(desktop.handle != nullptr, "isolated non-active desktop created", GetLastError());
    if (!desktop.handle) return 1;
    const auto directory = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"picker_host_fixture_" + std::to_wstring(GetCurrentProcessId()) +
         (novalidate_only ? L"_novalidate_" + std::to_wstring(GetTickCount64()) : L"")));
    std::error_code error;
    if (novalidate_only) {
        const bool new_directory = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
        Check(new_directory, "NOVALIDATE exclusive fixture directory created", GetLastError());
        if (!new_directory) return 1;
        std::wcout << L"[DIAG] NOVALIDATE fixture/recovery directory=" << directory << std::endl;
    } else std::filesystem::create_directories(directory, error);
    const auto fixture = directory / L"selection.txt";
    PipeHandle file(CreateFileW(fixture.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    Check(static_cast<bool>(file), "isolated local fixture created", GetLastError());
    if (!file) return 1;
    DWORD bytes = 0; WriteFile(file.Get(), "picker fixture", 14, &bytes, nullptr); file.Reset();
    const auto executable = std::filesystem::absolute(argv[1]).wstring();
    if (novalidate_only) {
        const auto run = [&](const std::filesystem::path& target, bool cancel = false) {
            ClientCase(executable, desktop, target, false, cancel, false, true, false, false, false,
                false, false, probe_path, false, true, true);
        };
        Check(bytes == 14, "NOVALIDATE fixture initialized");
        if (bytes != 14) { Check(DeleteFileW(fixture.c_str()) != FALSE, "partial fixture cleaned"); return 1; }
        std::cout << "[CASE] NOVALIDATE ordinary" << std::endl;
        run(fixture);
        {
            PipeHandle exclusive(CreateFileW(fixture.c_str(), FILE_READ_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            PipeHandle probe(CreateFileW(fixture.c_str(), FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            const DWORD denied = probe ? ERROR_SUCCESS : GetLastError();
            const bool ready = exclusive && !probe && denied == ERROR_SHARING_VIOLATION;
            Check(ready, "NOVALIDATE exclusive fixture independently denies read-data sharing", denied);
            probe.Reset();
            if (ready) { std::cout << "[CASE] NOVALIDATE exclusive lock" << std::endl; run(fixture); }
        }
        bool restored = true;
        {
            HostFixtureAcl acl(fixture);
            const bool installed = acl.Install();
            Check(installed, "NOVALIDATE owned ACL installed with recovery journal");
            if (installed) {
                PipeHandle attr(CreateFileW(fixture.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                PipeHandle data(CreateFileW(fixture.c_str(), FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                const DWORD denied = data ? ERROR_SUCCESS : GetLastError();
                const bool ready = attr && !data && denied == ERROR_ACCESS_DENIED && GetFileAttributesW(fixture.c_str()) != INVALID_FILE_ATTRIBUTES;
                Check(ready, "NOVALIDATE ACL attributes readable but file data denied", denied);
                attr.Reset(); data.Reset();
                if (ready) { std::cout << "[CASE] NOVALIDATE attributes-only ACL" << std::endl; run(fixture); }
            }
            restored = acl.Restore();
            Check(restored, "NOVALIDATE original DACL and protection exactly restored");
        }
        const auto manual = directory / L"manual.bin";
        PipeHandle output(CreateFileW(manual.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        const bool manual_created = static_cast<bool>(output);
        DWORD written = 0;
        const bool manual_ready = manual_created && WriteFile(output.Get(), "picker fixture", 14, &written, nullptr) && written == 14;
        output.Reset();
        Check(manual_ready, "NOVALIDATE outside-filter fixture exclusively created");
        if (manual_ready) { std::cout << "[CASE] NOVALIDATE manual outside filter" << std::endl; run(manual); }
        std::cout << "[CASE] NOVALIDATE explicit UI cancel" << std::endl;
        run(fixture, true);
        if (manual_created) Check(DeleteFileW(manual.c_str()) != FALSE, "NOVALIDATE manual fixture deleted");
        if (restored) {
            PipeHandle verify(CreateFileW(fixture.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            char content[15]{}; DWORD read = 0;
            Check(verify && ReadFile(verify.Get(), content, 15, &read, nullptr) && read == 14 && std::memcmp(content, "picker fixture", 14) == 0,
                "NOVALIDATE fixture bytes unchanged and exclusive verification handle available");
            verify.Reset();
            Check(DeleteFileW(fixture.c_str()) != FALSE, "NOVALIDATE original fixture deleted; recovery journal retained");
        }
        std::cout << "[LIMIT] Missing-target error notice is compositor-only; this mode does not claim missing-target UI verification.\n";
        std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
        return failures ? 1 : 0;
    }
    if (argc == 2) {
        for (const auto* mode : {"unsupported", "cancel", "select", "save-select", "save-cancel", "wrong-cancel", "disconnect"})
            Case(executable, desktop, fixture, mode);
        ClientCase(executable, desktop, fixture, false, false);
        ClientCase(executable, desktop, fixture, true, false);
        ClientCase(executable, desktop, fixture, false, true);
    }
    if (session_cold) {
        // A direct v1 request still reaches the same host before negotiated calls.
        Case(executable, desktop, fixture, "cancel");
        ClientCase(executable, desktop, fixture, false, false);
        ClientCase(executable, desktop, fixture, true, false);
        ClientCase(executable, desktop, fixture, false, true);
    }
    if (standby_only) {
        // Warm standby on this isolated desktop (a test-hook build honours
        // PULSE_PICKER_STANDBY_TEST instead of the user's interception
        // registration). A kill-on-close job ends it and every successor.
        PipeHandle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const bool job_ready = job && SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        SetEnvironmentVariableW(L"PULSE_PICKER_STANDBY_TEST", L"1");
        std::wstring command = L"\"" + executable + L"\" --picker-standby";
        std::wstring desktop_name = desktop.name;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop_name.data();
        PROCESS_INFORMATION info{};
        bool started = job_ready && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, nullptr, &startup, &info);
        if (started) started = AssignProcessToJobObject(job.Get(), info.hProcess) && ResumeThread(info.hThread) != static_cast<DWORD>(-1);
        if (info.hThread) CloseHandle(info.hThread);
        if (info.hProcess) CloseHandle(info.hProcess);
        Check(started, "standby started on the isolated desktop", GetLastError());
        const auto listening = [&](DWORD ms) {
            const auto name = StandbyPipeName(desktop.name);
            const auto start = GetTickCount64();
            while (GetTickCount64() - start < ms) {
                if (WaitNamedPipeW(name.c_str(), 50)) return true;
                Sleep(20);
            }
            return false;
        };
        Check(started && listening(10000), "standby listens before the first request");
        std::cout << "[CASE] standby warm open" << std::endl;
        ClientCase(executable, desktop, fixture, false, false);
        Check(listening(10000), "a successor standby listens after the first request");
        std::cout << "[CASE] standby warm save" << std::endl;
        ClientCase(executable, desktop, fixture, true, false);
        SetEnvironmentVariableW(L"PULSE_PICKER_STANDBY_TEST", nullptr);
    }
    if (session_cold || session_standby) {
        Check(DeleteFileW(fixture.c_str()) != FALSE && RemoveDirectoryW(directory.c_str()) != FALSE, "session fixture cleaned up");
        std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
        return failures ? 1 : 0;
    }
    if (legacy_only || legacy_ansi_only) {
        ClientCase(executable, desktop, fixture, false, false, false, false, true, false, legacy_ansi_only);
        ClientCase(executable, desktop, fixture, true, false, false, false, true, false, legacy_ansi_only);
        ClientCase(executable, desktop, fixture, false, true, false, false, true, false, legacy_ansi_only);
    } else if (!wrapper_links_only && !wrapper_filters_only && !dll_filters_only) {
        if (!wrapper_cancel_only) {
            ClientCase(executable, desktop, fixture, false, false, false, true, false, iat_only, false, ex_only, factory_only, probe_path);
            ClientCase(executable, desktop, fixture, true, false, false, true, false, iat_only, false, ex_only, factory_only, probe_path);
        }
        ClientCase(executable, desktop, fixture, false, true, false, true, false, iat_only, false, ex_only, factory_only, probe_path);
        if (dll_only) ClientCase(executable, desktop, fixture, false, false, false, true, false, false, false, false, false, probe_path, true);
    }
    const auto shortcut = directory / L"selection-shortcut.lnk";
    if (argc == 2 || wrapper_links_only) {
        const bool shortcut_created = CreateShortcutFixture(shortcut, fixture);
        Check(shortcut_created, "real Shell shortcut fixture points to selected text file");
        if (shortcut_created) {
            ClientCase(executable, desktop, shortcut, false, false, true, wrapper_links_only);
            if (wrapper_links_only) ClientCase(executable, desktop, shortcut, false, true, true, true);
        }
    }
    if (GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES)
        Check(DeleteFileW(shortcut.c_str()) != FALSE, "shortcut fixture cleaned up");
    if (wrapper_filters_only || dll_filters_only) {
        const auto manual = directory / L"manual.bin";
        PipeHandle output(CreateFileW(manual.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        const bool created = static_cast<bool>(output);
        const bool ready = created && WriteFile(output.Get(), "picker fixture", 14, &written, nullptr) && written == 14;
        output.Reset();
        Check(ready, "manual outside-filter fixture created");
        if (ready) {
            ClientCase(executable, desktop, manual, false, false, false, true, false, false, false, false, false, probe_path, false, true);
            ClientCase(executable, desktop, manual, false, true, false, true, false, false, false, false, false, probe_path, false, true);
        }
        if (created) Check(DeleteFileW(manual.c_str()) != FALSE, "manual outside-filter fixture cleaned up");
    }
    Check(DeleteFileW(fixture.c_str()) != FALSE && RemoveDirectoryW(directory.c_str()) != FALSE, "fixture cleaned up");
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
