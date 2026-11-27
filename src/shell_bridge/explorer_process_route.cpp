#include "../common/native_browse_launch.h"
#include "explorer_process_route.h"
#include "../common/explorer_launch_request.h"
#include "../common/dialog_bundle.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bridge_paths.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <bcrypt.h>
namespace pulse::shell_bridge::explorer_process {
namespace {
struct State {
    std::wstring bridge;
    Gate gate = nullptr;
    dialog_bundle::BundleLease lease;
    HANDLE file = INVALID_HANDLE_VALUE;
    ~State() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
std::atomic<std::shared_ptr<const State>> state;
std::atomic<Gate> armed_gate{nullptr};
std::atomic<DWORD> configuration_error{ERROR_SUCCESS};
std::mutex publication;
uint64_t generation = 0;
uint64_t BeginConfiguration(Gate gate) {
    std::lock_guard lock(publication);
    armed_gate.store(gate, std::memory_order_release);
    configuration_error.store(ERROR_IO_PENDING, std::memory_order_release);
    return ++generation;
}
DWORD Publish(std::shared_ptr<State> candidate, uint64_t ticket) {
    std::lock_guard lock(publication);
    if (ticket != generation) return ERROR_CANCELLED;
    state.store(std::move(candidate), std::memory_order_release);
    configuration_error.store(ERROR_SUCCESS, std::memory_order_release);
    return ERROR_SUCCESS;
}
DWORD ConfigurationFailed(uint64_t ticket, DWORD error, bool revoke = false) {
    std::lock_guard lock(publication);
    if (ticket == generation) {
        if (revoke) state.store({}, std::memory_order_release);
        configuration_error.store(error, std::memory_order_release);
    }
    return error;
}
thread_local bool entering = false;
struct EnteringGuard { EnteringGuard() { entering = true; } ~EnteringGuard() { entering = false; } };
std::wstring BrowseCommand(const std::wstring& bridge, const browse::Request& request) {
    return explorer_launch::Quote(bridge) + L" --browse-v1 " +
        (request.intent == browse::Intent::Reveal ? L"select " : L"folder ") + explorer_launch::Quote(request.path);
}

#ifdef PULSE_EXPLORER_PROCESS_TEST
bool fail_classification = false;
#endif
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == path.npos ? L"" : path.substr(0, slash);
}
DWORD HoldBridge(State& value) {
    value.file = CreateFileW(value.bridge.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (value.file == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(value.file, &info)) return GetLastError();
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
    return ERROR_SUCCESS;
}
DWORD VerifyHeldBridge(State& value, const std::string& expected) {
    LARGE_INTEGER size{}, zero{};
    if (expected.size() != 64) return ERROR_INVALID_DATA;
    if (!GetFileSizeEx(value.file, &size)) return GetLastError();
    if (size.QuadPart < 256 || size.QuadPart > 64 * 1024 * 1024) return ERROR_BAD_EXE_FORMAT;
    std::vector<UCHAR> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!SetFilePointerEx(value.file, zero, nullptr, FILE_BEGIN) ||
        !ReadFile(value.file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return GetLastError();
    if (read != bytes.size()) return ERROR_READ_FAULT;
    IMAGE_DOS_HEADER dos{}; memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        static_cast<size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > bytes.size()) return ERROR_BAD_EXE_FORMAT;
    DWORD signature = 0; IMAGE_FILE_HEADER header{};
    memcpy(&signature, bytes.data() + dos.e_lfanew, sizeof(signature));
    memcpy(&header, bytes.data() + dos.e_lfanew + sizeof(signature), sizeof(header));
#ifdef _WIN64
    constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
    constexpr WORD machine = IMAGE_FILE_MACHINE_I386;
#endif
    if (signature != IMAGE_NT_SIGNATURE || header.Machine != machine ||
        !(header.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) || (header.Characteristics & IMAGE_FILE_DLL)) return ERROR_BAD_EXE_FORMAT;
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } hash;
    UCHAR digest[32]{};
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(hash.algorithm, &hash.hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash.hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash.hash, digest, sizeof(digest), 0) < 0) return ERROR_FUNCTION_FAILED;
    std::string actual; actual.reserve(64);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : digest) { actual.push_back(hex[byte >> 4]); actual.push_back(hex[byte & 15]); }
    return actual == expected ? ERROR_SUCCESS : ERROR_CRC;
}
}
void Disable() noexcept {
    std::lock_guard lock(publication);
    ++generation; state.store({}, std::memory_order_release);
    armed_gate.store(nullptr, std::memory_order_release);
    configuration_error.store(ERROR_SUCCESS, std::memory_order_release);
}
DWORD Configure(HMODULE owner, Gate gate) noexcept {
    if (!owner || !gate) return ERROR_INVALID_PARAMETER;
    const auto ticket = BeginConfiguration(gate);
    const auto fail = [ticket](DWORD error) { return ConfigurationFailed(ticket, error); };
    try {
        std::vector<std::wstring> roots;
        const auto common = dialog_bridge_paths::CommonDialogBridgeRoot();
        if (!common.empty() && !dialog_admin::VerifyProtectedPath(common, true) &&
            !dialog_admin::VerifyProtectedPath(common + L"\\active-bridge.json", false)) {
            dialog_bridge_paths::ActiveBridgePointer pointer;
            const DWORD pointer_error = dialog_bridge_paths::ReadActiveBridgePointer(common, pointer);
            if (pointer_error) return fail(pointer_error);
            if (pointer.disabled) return ConfigurationFailed(ticket, ERROR_ACCESS_DISABLED_BY_POLICY, true);
            {
#ifdef _WIN64
                const auto& directory = pointer.core_dir_x64;
                const auto& digest = pointer.explorer_bridge_sha256_x64;
                constexpr WORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
                const auto& directory = pointer.core_dir_x86;
                const auto& digest = pointer.explorer_bridge_sha256_x86;
                constexpr WORD machine = IMAGE_FILE_MACHINE_I386;
#endif
                if (!digest.empty()) {
                    // A declared independent helper is authoritative: corruption
                    // must not silently select a different AppRuntime generation.
                    DWORD error = dialog_admin::VerifyProtectedBridgeLocation(common, directory, machine);
                    if (error) return fail(error);
                    auto candidate = std::make_shared<State>();
                    candidate->bridge = directory + L"\\" + dialog_bridge_paths::kExplorerBridgeExeName;
                    if ((error = dialog_admin::VerifyProtectedPath(candidate->bridge, false)) ||
                        (error = HoldBridge(*candidate)) || (error = VerifyHeldBridge(*candidate, digest))) return fail(error);
                    candidate->gate = gate;
                    return Publish(std::move(candidate), ticket);
                }
                if (!pointer.install_root.empty()) roots.push_back(pointer.install_root);
            }
        }
        wchar_t path[32768]{};
        const DWORD length = GetModuleFileNameW(owner, path, ARRAYSIZE(path));
        if (!length || length >= ARRAYSIZE(path)) return fail(ERROR_BAD_PATHNAME);
        auto parent = Parent(std::wstring(path, length));
        for (unsigned i = 0; i < 4 && !parent.empty(); ++i, parent = Parent(parent)) roots.push_back(parent);
        DWORD error = ERROR_FILE_NOT_FOUND;
        for (const auto& root : roots) {
            if (dialog_admin::VerifyProtectedPath(root, true) ||
                dialog_admin::VerifyProtectedPath(root + L"\\app-current.json", false)) continue;
            auto candidate = std::make_shared<State>();
            error = dialog_bundle::ResolveInstalledAppRuntime(root, candidate->lease);
            if (error) continue;
            error = dialog_admin::VerifyProtectedTree(candidate->lease.Directory());
            if (error) continue;
            candidate->bridge = candidate->lease.Directory() + L"\\pulse_explorer_bridge.exe";
            if ((error = dialog_admin::VerifyProtectedPath(candidate->bridge, false)) != ERROR_SUCCESS ||
                (error = HoldBridge(*candidate)) != ERROR_SUCCESS) continue;
            candidate->gate = gate;
            return Publish(std::move(candidate), ticket);
        }
        return fail(error);
    } catch (...) { return fail(ERROR_NOT_ENOUGH_MEMORY); }
}
#ifdef PULSE_EXPLORER_PROCESS_TEST
void SetClassificationFailureForTest(bool enabled) noexcept { fail_classification = enabled; }
void RevokeForTest(DWORD error) noexcept {
    const auto gate = armed_gate.load(std::memory_order_acquire);
    const auto ticket = BeginConfiguration(gate);
    (void)ConfigurationFailed(ticket, error, true);
}
DWORD VerifyBridgeForTest(const std::wstring& bridge, const std::string& digest) noexcept {
    try { State candidate; candidate.bridge = bridge;
        const DWORD error = HoldBridge(candidate); return error ? error : VerifyHeldBridge(candidate, digest);
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD ConfigureForTest(const std::wstring& bridge, Gate gate) noexcept {
    try {
        const auto ticket = BeginConfiguration(gate);
        auto candidate = std::make_shared<State>();
        candidate->bridge = bridge; candidate->gate = gate;
        const DWORD error = HoldBridge(*candidate);
        if (error) return ConfigurationFailed(ticket, error);
        return Publish(std::move(candidate), ticket);
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
#endif
Status QueryStatus() noexcept {
    if (!armed_gate.load(std::memory_order_acquire)) return {Readiness::Disabled, ERROR_SUCCESS};
    const DWORD error = configuration_error.load(std::memory_order_acquire);
    if (state.load(std::memory_order_acquire)) return {Readiness::Ready, error};
    return {error == ERROR_IO_PENDING ? Readiness::Configuring : Readiness::Unavailable, error};
}
bool IsReady() noexcept { return QueryStatus().readiness == Readiness::Ready; }
DirectRevealResult SubmitBrowse(const browse::Request& request, DWORD timeout_ms) noexcept {
    if (entering || NativeFallbackActive()) { SetLastError(ERROR_BUSY); return DirectRevealResult::Rejected; }
    EnteringGuard guard;
    try {
        const auto config = state.load(std::memory_order_acquire);
        const auto gate = config ? config->gate : armed_gate.load(std::memory_order_acquire);
        if (!gate || !gate()) return DirectRevealResult::Rejected;
        if (!config) { SetLastError(ERROR_NOT_READY); return DirectRevealResult::Rejected; }
        if (request.path.empty() || (request.intent != browse::Intent::OpenFolder && request.intent != browse::Intent::Reveal)) {
            SetLastError(ERROR_NOT_SUPPORTED); return DirectRevealResult::Unsupported;
        }
        if (direct_reveal_detail::FindPrimaryEndpoint({})) {
            return request.intent == browse::Intent::Reveal ? TryDirectReveal(request.path, timeout_ms)
                : TryDirectOpenFolder(request.path, timeout_ms);
        }
        auto command = BrowseCommand(config->bridge, request);
        if (command.size() >= 32767) { SetLastError(ERROR_FILENAME_EXCED_RANGE); return DirectRevealResult::Rejected; }
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        if (!CreateProcessW(config->bridge.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child))
            return DirectRevealResult::Rejected;
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        return DirectRevealResult::Accepted;
    } catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return DirectRevealResult::Rejected; }
}
BOOL FallbackCreateProcess(Native original, LPCWSTR app, LPWSTR command,
    LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
    LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup,
    LPPROCESS_INFORMATION information) noexcept {
    const DWORD incoming = GetLastError();
    browse::Request request;
    bool identified = false;
    try {
        // Flags rejected by routing do not change the target of the original
        // command. Classification here never rewrites those flags or arguments.
        identified = explorer_launch::Classify(app, command, 0, directory, request) == browse::Match::Supported;
    } catch (...) {}
    if (!identified || NativeFallbackActive()) {
        SetLastError(incoming);
        return original(app, command, pa, ta, inherit, flags, environment, directory, startup, information);
    }
    native_browse::Launch launch;
    // An explicit destination desktop is outside this thread's ledger. Do not
    // publish a misleading exemption for windows on the current desktop.
    if (!startup || !startup->lpDesktop || !*startup->lpDesktop) launch.Prepare(request);
    SetLastError(incoming);
    const BOOL result = original(app, command, pa, ta, inherit, flags, environment, directory, startup, information);
    const DWORD error = GetLastError();
    launch.Complete(result != FALSE);
    SetLastError(error);
    return result;
}
bool TryRoute(Native native, LPCWSTR app, LPWSTR command, LPSECURITY_ATTRIBUTES pa,
    LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information, BOOL& result) noexcept {
    const DWORD incoming = GetLastError();
    DWORD outgoing = incoming;
    struct ErrorRestore { DWORD& value; ~ErrorRestore() { SetLastError(value); } } restore{outgoing};
    if (entering || NativeFallbackActive() || !native || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
    const auto config = state.load(std::memory_order_acquire);
    const auto gate = config ? config->gate : armed_gate.load(std::memory_order_acquire);
    if (!gate || !gate()) return false;
    EnteringGuard guard;
    // A helper's own final Explorer launch must not route back into itself,
    // even if the installed Core was loaded into that helper process.
    if (config) {
        wchar_t image[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, image, ARRAYSIZE(image));
        if (length && length < ARRAYSIZE(image) && !_wcsicmp(image, config->bridge.c_str())) return false;
    }
    try {
        browse::Request request;
#ifdef PULSE_EXPLORER_PROCESS_TEST
        if (fail_classification) throw std::bad_alloc();
#endif
        const auto match = explorer_launch::Classify(app, command, flags, directory, request);
        if (match != browse::Match::Supported) return false;
        if (!startup || !information || startup->cb != sizeof(STARTUPINFOW)) return false;
        if (startup->lpDesktop && *startup->lpDesktop) return false;
        if (!config) return false;
        // Preserve ordinary security/handle/CRT startup contracts on the real
        // child; bInheritHandles=FALSE is not a reason to launch native Explorer.
        auto rewritten = BrowseCommand(config->bridge, request);
        if (rewritten.size() >= 32767) return false;
        SetLastError(incoming);
        result = native(config->bridge.c_str(), rewritten.data(), pa, ta, inherit, flags,
            environment, directory, startup, information);
        if (!result) return false; // Original caller retries its untouched Explorer call once.
        outgoing = GetLastError();
        return true;
    } catch (...) {} // Preserve the original call when redirect preparation fails.
    return false;
}
}
