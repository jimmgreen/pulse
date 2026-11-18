#include "../ipc/picker_session.h"
#include "picker_host_mode.h"
#include "picker_appearance_ui.h"
#include "../common/localization.h"
#include "../ui/typography.h"
#include "../ui/file_save_model.h"
#include "../common/dialog_auto_registration.h"
#include "picker_bridge_model.h"
#include "../ipc/picker_transport.h"

#include <atomic>
#include <thread>

namespace pulse::app {
namespace {
namespace wire = ipc::picker;
wire::Message Failure(const wire::Message& request, DWORD error, wire::ErrorSource source) {
    wire::Response response;
    if (const auto* body = std::get_if<wire::Request>(&request.body)) response.operation = body->operation;
    response.status = wire::Status::Failed;
    response.error_source = source;
    response.error_code = error;
    return {request.request_id, std::move(response)};
}
struct ReaderGuard {
    HANDLE stop;
    std::thread thread;
    void Stop() {
        SetEvent(stop);
        if (thread.joinable()) thread.join();
    }
    ~ReaderGuard() { Stop(); }
};
}

namespace { int Serve(wire::Channel& channel, HANDLE parent_process, DWORD pid, uint64_t owner_value); }


int RunPickerHostMode(const wchar_t* pipe_name, const wchar_t* parent_pid, const wchar_t* owner_window) {
    if (!pipe_name || !parent_pid || !*parent_pid) return 2;
    uint64_t pid = 0;
    for (const wchar_t* p = parent_pid; *p; ++p) {
        if (*p < L'0' || *p > L'9' || pid > MAXDWORD / 10ull) return 2;
        pid = pid * 10 + static_cast<uint64_t>(*p - L'0');
        if (pid > MAXDWORD) return 2;
    }
    if (!pid) return 2;
    uint64_t owner_value = 0;
    if (owner_window) {
        if (!*owner_window) return 2;
        for (const wchar_t* p = owner_window; *p; ++p) {
            if (*p < L'0' || *p > L'9' || owner_value > (UINT64_MAX - 9) / 10ull) return 2;
            owner_value = owner_value * 10 + static_cast<uint64_t>(*p - L'0');
        }
    }
    wire::PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
    if (!parent) return 3;
    wire::Channel channel;
    if (!wire::Connect(pipe_name, parent.Get(), channel)) return 3;
    return Serve(channel, parent.Get(), static_cast<DWORD>(pid), owner_value);
}

namespace {
// One request on a connected channel: the cold host and the standby share it.
int Serve(wire::Channel& channel, HANDLE parent_process, DWORD pid, uint64_t owner_value) {
    wire::Message request;
    if (!wire::ReceivePickerSessionRequest(channel, request, {nullptr, 10000})) return 3;

    // A standby has no command line: the caller sends its window in-band.
    if (const auto* body = std::get_if<wire::Request>(&request.body); body && (body->required_capabilities & wire::CallerWindow))
        owner_value = body->caller_window;
    auto preparation = PreparePickerBridge(request);
    if (const auto* rejected = std::get_if<wire::Message>(&preparation))
        return channel.Send(*rejected, {nullptr, 5000}) ? 0 : 4;

    wire::PipeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wire::PipeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!cancel || !stop) {
        channel.Send(Failure(request, GetLastError(), wire::ErrorSource::Win32), {nullptr, 5000});
        return 3;
    }
    auto spec = std::get<ui::FolderPickerSpec>(std::move(preparation));
    if (spec.validate_local_save_access_v1) {
        // As the in-process provider: a local save whose starting target does
        // not resolve is declined before any UI, so the caller may stay native.
        ui::SaveTargetRequest target;
        target.directory = spec.initial_path; target.filename = spec.filename.empty() ? L"pulse-validation" : spec.filename;
        target.filters = spec.filters; target.filter_index = spec.filter_index; target.default_extension = spec.default_extension;
        target.native_save_defaults = spec.native_save_defaults;
        target.no_test_file_create = spec.no_test_file_create;
        target.require_existing_save_parent = spec.require_existing_save_parent;
        if (ui::ResolveSaveTarget(target).state != ui::SaveTargetState::Resolved) {
            wire::Response unsupported;
            unsupported.operation = wire::Operation::SaveFile;
            unsupported.status = wire::Status::Unsupported;
            unsupported.error_source = wire::ErrorSource::Win32;
            unsupported.error_code = ERROR_NOT_SUPPORTED;
            return channel.Send({request.request_id, std::move(unsupported)}, {nullptr, 5000}) ? 0 : 4;
        }
    }
    spec.cancel_event = cancel.Get();
    const auto appearance = ReadUserPickerAppearanceReadOnly().snapshot;
    ApplyPickerAppearanceToSpec(appearance, spec);
    l10n::Initialize(GetModuleHandleW(nullptr), appearance.language);
    ui::typography::SetUiFontScale(appearance.ui_font_scale);
    ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(appearance.text_render));
    const auto colors = ResolveSystemPickerAppearanceColors(appearance);
    if (WaitForSingleObject(parent_process, 0) != WAIT_TIMEOUT) return 4;
    std::atomic<DWORD> reader_error{ERROR_SUCCESS};
    std::atomic<bool> protocol_error{false};
    std::atomic<bool> requested_cancel{false};
    ReaderGuard reader{stop.Get(), {}};
    try {
        reader.thread = std::thread([&] {
            wire::Message command;
            const auto received = channel.Receive(command, {stop.Get(), INFINITE});
            if (!received) {
                if (received.error == ERROR_CANCELLED && WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) return;
                reader_error = received.error;
                protocol_error = received.codec_error != wire::CodecError::None;
            } else if (command.request_id != request.request_id || !std::holds_alternative<wire::Cancel>(command.body)) {
                reader_error = ERROR_INVALID_DATA;
                protocol_error = true;
            } else requested_cancel = true;
            SetEvent(cancel.Get());
        });
    } catch (const std::system_error&) {
        channel.Send(Failure(request, ERROR_NOT_ENOUGH_MEMORY, wire::ErrorSource::Win32), {nullptr, 5000});
        return 3;
    }
    // The caller owns its own modality. This process never disables an alien HWND.
    // The caller's window, verified to belong to the parent, only places the
    // dialog: over that window, above it in z-order, with no taskbar button of
    // its own. The caller's PickerOwnerGuard (which a second guard here could
    // not share) keeps disabling it and cancels us if it goes away.
    HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner_value));
    DWORD owner_pid = 0;
    if (owner && (!IsWindow(owner) || !GetWindowThreadProcessId(owner, &owner_pid) || owner_pid != static_cast<DWORD>(pid))) owner = nullptr;
    spec.placement_owner = owner;
    if (WaitForSingleObject(parent_process, 0) != WAIT_TIMEOUT) return 4;

    auto outcome = ui::ShowFilePickerDetailed(nullptr, spec, colors.dark, ui::HexColor(colors.accent_rgb));
    reader.Stop(); // All outstanding overlapped reads finish before response/write/destruction.
    wire::Message response;
    if (reader_error != ERROR_SUCCESS) {
        response = Failure(request, reader_error.load(), protocol_error ? wire::ErrorSource::Protocol : wire::ErrorSource::Win32);
    } else {
        if (requested_cancel) outcome = {ui::FilePickerStatus::Cancelled, ERROR_SUCCESS, {}};
        response = CompletePickerBridge(request, outcome);
    }
    return channel.Send(response, {nullptr, 5000}) ? 0 : 4;
}

std::wstring OwnExecutable() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size && size < path.size() ? size : 0);
    return path;
}
std::wstring DirectoryOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}
// The pulse.exe next to the registered interception DLL, when the pulse.exe
// route is on; empty otherwise. That is the executable clients will expect.
std::wstring RegisteredStandbyExecutable() {
    if (!dialog_auto::PreferOutOfProcess()) return {};
    const auto status = dialog_auto::Inspect();
    if (!status.enabled || status.conflict || status.error || status.dll_path.empty()) return {};
    return DirectoryOf(status.dll_path) + L"\\pulse.exe";
}
bool LaunchStandby(const std::wstring& executable) {
    if (executable.empty() || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring command = L"\"" + executable + L"\" --picker-standby";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return false;
    CloseHandle(info.hThread); CloseHandle(info.hProcess);
    return true;
}
} // namespace

int RunPickerStandbyMode() {
    const auto self = OwnExecutable();
    if (self.empty()) return 2;
    const auto current = [&] {
#ifdef PULSE_WITH_TEST_HOOKS
        // Isolated tests run a standby without registering interception in
        // the user's HKCU. Lifetime only: clients still verify this process.
        if (GetEnvironmentVariableW(L"PULSE_PICKER_STANDBY_TEST", nullptr, 0) > 0) return true;
#endif
        const auto registered = RegisteredStandbyExecutable();
        return !registered.empty() && CompareStringOrdinal(registered.c_str(), -1, self.c_str(), -1, TRUE) == CSTR_EQUAL;
    };
    if (!current()) return 0;
    // One idle standby per bundle; a second one leaves at once.
    wire::PipeHandle idle(CreateMutexW(nullptr, TRUE, wire::StandbyIdleMutexName(self).c_str()));
    if (!idle || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    // Warm-up while idle: the libraries the dialog's first frame needs.
    for (const wchar_t* library : {L"d2d1.dll", L"dwrite.dll", L"d3d11.dll", L"dxgi.dll", L"dcomp.dll", L"windowscodecs.dll", L"uxtheme.dll"})
        LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    LoadLibraryExW((DirectoryOf(self) + L"\\lumatext.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    for (;;) {
        wire::StandbyServer server;
        if (!wire::StandbyServer::Create(server)) return 3;
        wire::Channel channel;
        DWORD client_pid = 0;
        // Wake up every minute: an upgrade or a disabled route retires this
        // standby instead of keeping an old bundle's files open.
        const auto accepted = server.Accept(channel, client_pid, {nullptr, 60000});
        if (accepted.error == ERROR_TIMEOUT) { if (!current()) return 0; continue; }
        if (!accepted) continue; // A rejected client: keep waiting for a genuine one.
        wire::PipeHandle client(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, client_pid));
        if (!client) continue;
        // Hand the idle role to a successor before this dialog starts.
        ReleaseMutex(idle.Get());
        idle.Reset();
        if (current()) LaunchStandby(self);
        return Serve(channel, client.Get(), client_pid, 0);
    }
}

void SeedPickerStandbyAsync() {
    std::thread([] {
        const auto executable = RegisteredStandbyExecutable();
        if (executable.empty()) return;
        wire::PipeHandle existing(OpenMutexW(SYNCHRONIZE, FALSE, wire::StandbyIdleMutexName(executable).c_str()));
        if (!existing) LaunchStandby(executable);
    }).detach();
}
} // namespace pulse::app
