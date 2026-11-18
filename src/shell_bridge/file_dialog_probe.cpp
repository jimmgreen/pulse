#include "file_dialog_probe_abi.h"

// The cancellation seam and exports exist exclusively in the explicitly
// named test-probe DLL target, never as a production configuration option.
#if defined(PULSE_BRIDGE_PROBE_DLL)
#include "file_dialog_iat.h"
#include "../ipc/picker_client.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>
#include <memory>
#include <new>
#include <string>

namespace {
using namespace pulse::shell_bridge;
namespace wire = pulse::ipc::picker;
#if defined(_M_IX86)
constexpr uint32_t kMachine = IMAGE_FILE_MACHINE_I386;
static_assert(sizeof(PulseBridgeProbeConfig) == 48 && sizeof(PulseBridgeProbeApi) == 36);
static_assert(sizeof(PulseBridgeProbeFixture) == 36 && sizeof(PulseBridgeProbeFixtureApi) == 28);
#elif defined(_M_X64)
constexpr uint32_t kMachine = IMAGE_FILE_MACHINE_AMD64;
static_assert(sizeof(PulseBridgeProbeConfig) == 56 && sizeof(PulseBridgeProbeApi) == 56);
static_assert(sizeof(PulseBridgeProbeFixture) == 40 && sizeof(PulseBridgeProbeFixtureApi) == 40);
#else
#error The bridge probe supports only x86 and x64.
#endif
static_assert(sizeof(PulseBridgeProbeState) == 56 && sizeof(WCHAR) == 2);
static_assert(sizeof(PulseBridgeProbeFixtureState) == 160);
static_assert(PULSE_BRIDGE_PROBE_MAX_FIXTURE_BYTES == wire::kMaxMessageBytes);
struct ShowBarrier {
    HANDLE entered = nullptr, resume = nullptr;
    DWORD timeout = 0;
    ~ShowBarrier() { if (entered) CloseHandle(entered); if (resume) CloseHandle(resume); }
};
struct FixtureProfile {
    wire::Request request;
    wire::RequestId fixture_id{};
    uint64_t generation = 0;
    std::atomic<uint64_t> validations{0}, matches{0}, bridges{0}, active{0};
    std::atomic<uint64_t> accepted{0}, cancelled{0}, unsupported{0}, failed{0}, rejected{0};
    SRWLOCK terminal_lock = SRWLOCK_INIT;
    uint32_t terminal_status = 0, error_source = 0, error_code = 0;
    wire::RequestId last_request_id{};
};
struct RecordLock {
    SRWLOCK* lock;
    explicit RecordLock(SRWLOCK& value) : lock(&value) { AcquireSRWLockExclusive(lock); }
    ~RecordLock() { ReleaseSRWLockExclusive(lock); }
};
struct ProbeState {
    std::wstring host_path;
    bool enabled = false;
    uint64_t fixture_generation = 0;
    std::shared_ptr<FixtureProfile> selected_fixture;
    std::atomic<std::shared_ptr<const ShowBarrier>> barrier;
};
// Constant-initialized metadata never allocates or pins the module. Only a
// confirmed PIN promotes a locally owned candidate to process-lifetime state.
constinit std::atomic<ProbeState*> published_state{nullptr};
constinit std::atomic<HRESULT> last_status{S_OK};
constinit std::atomic<DWORD> last_probe_error{ERROR_SUCCESS};
constinit std::atomic<uint64_t> native_shows{0}, active_shows{0};
SRWLOCK administration = SRWLOCK_INIT;
struct AdministrationLock {
    AdministrationLock() { AcquireSRWLockExclusive(&administration); }
    ~AdministrationLock() { ReleaseSRWLockExclusive(&administration); }
};
HRESULT Remember(HRESULT hr) noexcept { last_status = hr; return hr; }
template<size_t N> bool Zero(const uint32_t (&values)[N]) {
    for (auto value : values) if (value) return false;
    return true;
}
HRESULT Header(uint32_t version, uint32_t machine, uint32_t expected_version = PULSE_BRIDGE_PROBE_ABI_VERSION) {
    if (version != expected_version) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (machine != kMachine) return HRESULT_FROM_WIN32(ERROR_EXE_MACHINE_TYPE_MISMATCH);
    return S_OK;
}
HRESULT CopyPath(const PulseBridgeProbeConfig& config, std::wstring& path) {
    if (config.host_path_units > PULSE_BRIDGE_PROBE_MAX_PATH_UNITS || (config.host_path_units && !config.host_path)) return E_INVALIDARG;
    path.resize(config.host_path_units);
    if (config.host_path_units) {
        SIZE_T copied = 0;
        const size_t bytes = path.size() * sizeof(WCHAR);
        if (!ReadProcessMemory(GetCurrentProcess(), config.host_path, path.data(), bytes, &copied) || copied != bytes)
            return HRESULT_FROM_WIN32(ERROR_NOACCESS);
    }
    for (size_t i = 0; i < path.size(); ++i) {
        const auto value = static_cast<uint16_t>(path[i]);
        if (!value || (value >= 0xdc00 && value <= 0xdfff)) return E_INVALIDARG;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (++i == path.size()) return E_INVALIDARG;
            const auto trail = static_cast<uint16_t>(path[i]);
            if (trail < 0xdc00 || trail > 0xdfff) return E_INVALIDARG;
        }
    }
    return S_OK;
}
HRESULT CancelNativeShow(IFileDialog*, HWND) noexcept {
    try {
        ++native_shows; ++active_shows;
        struct Active { ~Active() { --active_shows; } } active;
        // An IAT caller may enter between core publication and bootstrap
        // publication. Fixed cancellation needs no allocated bootstrap state.
        auto* state = published_state.load();
        const auto barrier = state ? state->barrier.load() : std::shared_ptr<const ShowBarrier>{};
        if (barrier) {
            if (!SetEvent(barrier->entered)) last_probe_error = GetLastError();
            else {
                const DWORD waited = WaitForSingleObject(barrier->resume, barrier->timeout);
                if (waited != WAIT_OBJECT_0) last_probe_error = waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
            }
        }
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    } catch (...) { return E_FAIL; }
}
bool Match(const FixtureProfile& profile, const wire::Message& message) {
    const auto* request = std::get_if<wire::Request>(&message.body);
    return request && *request == profile.request;
}
wire::Message Failure(const FixtureProfile& profile, const wire::Message& request, wire::ErrorSource source, uint32_t code) {
    wire::Response response;
    const auto* body = std::get_if<wire::Request>(&request.body);
    response.operation = body ? body->operation : profile.request.operation;
    response.status = wire::Status::Failed; response.error_source = source; response.error_code = code;
    return {request.request_id, std::move(response)};
}
void RecordTerminal(FixtureProfile& profile, const wire::Message& response) {
    const auto& body = std::get<wire::Response>(response.body);
    switch (body.status) {
    case wire::Status::Accepted: ++profile.accepted; break;
    case wire::Status::Cancelled: ++profile.cancelled; break;
    case wire::Status::Unsupported: ++profile.unsupported; break;
    case wire::Status::Failed: ++profile.failed; break;
    }
    RecordLock lock(profile.terminal_lock);
    profile.terminal_status = static_cast<uint32_t>(body.status);
    profile.error_source = static_cast<uint32_t>(body.error_source); profile.error_code = body.error_code;
    profile.last_request_id = response.request_id;
}
wire::Message RunFixture(const std::shared_ptr<FixtureProfile>& profile, const std::wstring& path,
                         const wire::Message& request, HWND owner, HANDLE cancel) {
    if (!Match(*profile, request)) {
        ++profile->rejected;
        return Failure(*profile, request, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
    }
    ++profile->bridges; ++profile->active;
    struct Active { FixtureProfile& profile; ~Active() { --profile.active; } } active{*profile};
    wire::Message response;
    try {
        response = wire::RunPickerClient(path, request, owner, cancel);
        if (!wire::ValidateResponse(request, response)) response = Failure(*profile, request, wire::ErrorSource::Protocol, ERROR_INVALID_DATA);
    } catch (const std::bad_alloc&) {
        response = Failure(*profile, request, wire::ErrorSource::Win32, ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) { response = Failure(*profile, request, wire::ErrorSource::Win32, ERROR_GEN_FAILURE); }
    RecordTerminal(*profile, response);
    return response;
}
HRESULT WINAPI Install(const PulseBridgeProbeConfig* config) noexcept {
    if (!config) return Remember(E_POINTER);
    if (config->size != sizeof(*config)) return Remember(E_INVALIDARG);
    const auto header = Header(config->version, config->machine);
    if (FAILED(header)) return Remember(header);
    if (!Zero(config->reserved) || (config->flags & ~PULSE_BRIDGE_PROBE_ENABLE)) return Remember(E_INVALIDARG);
    if (!config->target_exe || config->target_exe != GetModuleHandleW(nullptr)) return Remember(E_ACCESSDENIED);
    try {
        std::wstring path;
        const auto copied = CopyPath(*config, path);
        if (FAILED(copied)) return Remember(copied);
        FileDialogBridgeServices services;
        services.executable = path;
        services.show_native = CancelNativeShow; // Probe only, not a configurable policy.
        std::shared_ptr<FixtureProfile> profile;
        {
            AdministrationLock lock;
            const auto* state = published_state.load();
            if (state) profile = state->selected_fixture;
        }
        const bool enabled = (config->flags & PULSE_BRIDGE_PROBE_ENABLE) != 0;
        if (enabled && profile && (path.empty() || !std::filesystem::path(path).is_absolute())) return Remember(E_INVALIDARG);
        if (profile) {
            services.validation_verified = [profile](const ContractDecision& decision) {
                ++profile->validations;
                const bool matched = decision.request && Match(*profile, *decision.request);
                if (matched) ++profile->matches;
                return matched;
            };
            services.run_bridge = [profile, executable = path](const wire::Message& request, HWND owner, HANDLE cancel) {
                return RunFixture(profile, executable, request, owner, cancel);
            };
        }
        // Absent fixture leaves validation and bridge callbacks absent.
        std::unique_ptr<ProbeState> candidate;
        if (!published_state.load()) candidate = std::make_unique<ProbeState>();
        AdministrationLock lock;
        auto* state = published_state.load();
        const auto result = InstallPinnedModuleFileDialogIat(config->target_exe, services, enabled);
        // No allocation or throwing operation after observing successful PIN.
        // Preserve the candidate even when the later IAT write failed.
        if (!state && result.owner_pinned) {
            state = candidate.release(); published_state.store(state);
        }
        if (SUCCEEDED(result.status) && state) { state->host_path.swap(path); state->enabled = enabled; }
        return Remember(result.status);
    } catch (const std::bad_alloc&) { return Remember(E_OUTOFMEMORY); }
    catch (...) { return Remember(E_FAIL); }
}
HRESULT WINAPI Deactivate() noexcept {
    try {
        AdministrationLock lock;
        auto* state = published_state.load();
        if (!state) return Remember(S_FALSE);
        const auto result = UninstallSelfFileDialogIat();
        state->enabled = false;
        return Remember(result.status);
    } catch (const std::bad_alloc&) { return Remember(E_OUTOFMEMORY); }
    catch (...) { return Remember(E_FAIL); }
}
HRESULT WINAPI GetState(PulseBridgeProbeState* output) noexcept {
    if (!output) return E_POINTER;
    if (output->size != sizeof(*output)) return E_INVALIDARG;
    const auto header = Header(output->version, output->machine);
    if (FAILED(header)) return header;
    if (!Zero(output->reserved)) return E_INVALIDARG;
    try {
        AdministrationLock lock;
        const auto* state = published_state.load();
        const auto iat = QueryFileDialogIatState();
        PulseBridgeProbeState result{};
        result.size = sizeof(result); result.version = PULSE_BRIDGE_PROBE_ABI_VERSION; result.machine = kMachine;
        if (iat.installed) result.flags |= PULSE_BRIDGE_PROBE_INSTALLED;
        if (iat.owner_pinned) result.flags |= PULSE_BRIDGE_PROBE_PINNED;
        if (iat.protection_restored) result.flags |= PULSE_BRIDGE_PROBE_PROTECTION_RESTORED;
        if (state && state->enabled && iat.installed) result.flags |= PULSE_BRIDGE_PROBE_ENABLED;
        result.last_status = last_status.load();
        result.last_probe_error = last_probe_error.load();
        result.native_show_calls = native_shows.load(); result.active_native_shows = active_shows.load();
        *output = result; return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT WINAPI CopyHostPath(WCHAR* buffer, uint32_t capacity, uint32_t* required) noexcept {
    if (!required) return E_POINTER;
    try {
        AdministrationLock lock;
        const auto* state = published_state.load();
        *required = state ? static_cast<uint32_t>(state->host_path.size() + 1) : 1;
        if (capacity < *required) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        if (!buffer) return E_POINTER;
        std::memcpy(buffer, state ? state->host_path.c_str() : L"", *required * sizeof(WCHAR)); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT WINAPI SetShowBarrier(HANDLE entered, HANDLE resume, uint32_t timeout) noexcept {
    if ((!entered && !resume && timeout) || ((!entered || !resume) && (entered || resume)) ||
        ((entered && resume) && (!timeout || timeout > 30000))) return E_INVALIDARG;
    try {
        std::shared_ptr<ShowBarrier> barrier;
        std::shared_ptr<const ShowBarrier> retired;
        AdministrationLock lock;
        auto* state = published_state.load();
        if (!state) {
            if (entered) return E_UNEXPECTED;
            last_probe_error = ERROR_SUCCESS; return S_OK;
        }
        if (entered) {
            barrier = std::make_shared<ShowBarrier>(); barrier->timeout = timeout;
            if (!DuplicateHandle(GetCurrentProcess(), entered, GetCurrentProcess(), &barrier->entered, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
                !DuplicateHandle(GetCurrentProcess(), resume, GetCurrentProcess(), &barrier->resume, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return HRESULT_FROM_WIN32(GetLastError());
        }
        retired = state->barrier.exchange(std::move(barrier));
        last_probe_error = ERROR_SUCCESS;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT WINAPI SetFixture(const PulseBridgeProbeFixture* fixture) noexcept {
    if (!fixture) return E_POINTER;
    if (fixture->size != sizeof(*fixture)) return E_INVALIDARG;
    const auto header = Header(fixture->version, fixture->machine, PULSE_BRIDGE_PROBE_FIXTURE_VERSION);
    if (FAILED(header)) return header;
    if (fixture->flags || !Zero(fixture->reserved)) return E_INVALIDARG;
    // Publication is permanent and only follows a confirmed DLL PIN.
    if (!published_state.load()) return E_UNEXPECTED;
    if (!fixture->bytes || !fixture->byte_count || fixture->byte_count > wire::kMaxMessageBytes) return E_INVALIDARG;
    try {
        std::vector<uint8_t> bytes(fixture->byte_count);
        SIZE_T copied = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), fixture->bytes, bytes.data(), bytes.size(), &copied) || copied != bytes.size())
            return HRESULT_FROM_WIN32(ERROR_NOACCESS);
        wire::Message message;
        const auto decoded = wire::Decode(bytes, message);
        if (decoded == wire::CodecError::UnsupportedVersion) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
        if (decoded == wire::CodecError::UnsupportedCapability) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (decoded != wire::CodecError::None) return E_INVALIDARG;
        const auto* request = std::get_if<wire::Request>(&message.body);
        if (!request || (request->operation != wire::Operation::OpenFiles && request->operation != wire::Operation::SaveFile))
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        auto profile = std::make_shared<FixtureProfile>();
        profile->request = *request; profile->fixture_id = message.request_id;
        std::shared_ptr<FixtureProfile> retired;
        AdministrationLock lock;
        auto* state = published_state.load();
        if (state->fixture_generation == std::numeric_limits<uint64_t>::max()) return HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW);
        profile->generation = ++state->fixture_generation;
        retired = std::move(state->selected_fixture);
        state->selected_fixture = std::move(profile);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT WINAPI ClearFixture() noexcept {
    std::shared_ptr<FixtureProfile> retired;
    AdministrationLock lock;
    auto* state = published_state.load();
    if (!state || !state->selected_fixture) return S_FALSE;
    retired = std::move(state->selected_fixture);
    return S_OK;
}
HRESULT WINAPI GetFixtureState(PulseBridgeProbeFixtureState* output) noexcept {
    if (!output) return E_POINTER;
    if (output->size != sizeof(*output)) return E_INVALIDARG;
    const auto header = Header(output->version, output->machine, PULSE_BRIDGE_PROBE_FIXTURE_VERSION);
    if (FAILED(header)) return header;
    if (output->reserved0 || !Zero(output->reserved)) return E_INVALIDARG;
    std::shared_ptr<FixtureProfile> profile;
    {
        AdministrationLock lock;
        const auto* state = published_state.load();
        if (state) profile = state->selected_fixture;
    }
    PulseBridgeProbeFixtureState result{};
    result.size = sizeof(result); result.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION; result.machine = kMachine;
    if (profile) {
        result.flags = PULSE_BRIDGE_PROBE_FIXTURE_PRESENT; result.generation = profile->generation;
        result.validation_calls = profile->validations.load(); result.validation_matches = profile->matches.load();
        result.bridge_calls = profile->bridges.load(); result.active_bridge_calls = profile->active.load();
        result.accepted = profile->accepted.load(); result.cancelled = profile->cancelled.load();
        result.unsupported = profile->unsupported.load(); result.failed = profile->failed.load();
        result.rejected_dispatches = profile->rejected.load();
        std::memcpy(result.fixture_request_id, profile->fixture_id.data(), profile->fixture_id.size());
        RecordLock lock(profile->terminal_lock);
        result.last_terminal_status = profile->terminal_status;
        result.last_error_source = profile->error_source; result.last_error_code = profile->error_code;
        std::memcpy(result.last_request_id, profile->last_request_id.data(), profile->last_request_id.size());
    }
    *output = result; return S_OK;
}
}

extern "C" HRESULT WINAPI PulseBridgeProbeGetApi(uint32_t version, uint32_t api_size, PulseBridgeProbeApi* output) noexcept {
    if (!output) return E_POINTER;
    if (version != PULSE_BRIDGE_PROBE_ABI_VERSION) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (api_size != sizeof(*output)) return E_INVALIDARG;
    PulseBridgeProbeApi api{};
    api.size = sizeof(api); api.version = PULSE_BRIDGE_PROBE_ABI_VERSION; api.machine = kMachine;
    api.install = Install; api.deactivate = Deactivate; api.get_state = GetState;
    api.copy_host_path = CopyHostPath; api.set_show_barrier = SetShowBarrier;
    *output = api; return S_OK;
}
extern "C" HRESULT WINAPI PulseBridgeProbeGetFixtureApi(uint32_t version, uint32_t api_size, PulseBridgeProbeFixtureApi* output) noexcept {
    if (!output) return E_POINTER;
    if (version != PULSE_BRIDGE_PROBE_FIXTURE_VERSION) return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
    if (api_size != sizeof(*output)) return E_INVALIDARG;
    PulseBridgeProbeFixtureApi api{};
    api.size = sizeof(api); api.version = PULSE_BRIDGE_PROBE_FIXTURE_VERSION; api.machine = kMachine;
    api.set_fixture = SetFixture; api.clear_fixture = ClearFixture; api.get_state = GetFixtureState;
    *output = api; return S_OK;
}

#endif
