#include "picker_host_mode.h"
#include "picker_appearance_ui.h"
#include "../common/localization.h"
#include "../ui/typography.h"
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

int RunPickerHostMode(const wchar_t* pipe_name, const wchar_t* parent_pid) {
    if (!pipe_name || !parent_pid || !*parent_pid) return 2;
    uint64_t pid = 0;
    for (const wchar_t* p = parent_pid; *p; ++p) {
        if (*p < L'0' || *p > L'9' || pid > MAXDWORD / 10ull) return 2;
        pid = pid * 10 + static_cast<uint64_t>(*p - L'0');
        if (pid > MAXDWORD) return 2;
    }
    if (!pid) return 2;
    wire::PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
    if (!parent) return 3;
    wire::Channel channel;
    if (!wire::Connect(pipe_name, parent.Get(), channel)) return 3;
    wire::Message request;
    if (!channel.Receive(request, {nullptr, 10000})) return 3;
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
    spec.cancel_event = cancel.Get();
    const auto appearance = ReadUserPickerAppearanceReadOnly().snapshot;
    ApplyPickerAppearanceToSpec(appearance, spec);
    l10n::Initialize(GetModuleHandleW(nullptr), appearance.language);
    ui::typography::SetUiFontScale(appearance.ui_font_scale);
    ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(appearance.text_render));
    const auto colors = ResolveSystemPickerAppearanceColors(appearance);
    if (WaitForSingleObject(parent.Get(), 0) != WAIT_TIMEOUT) return 4;
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
    if (WaitForSingleObject(parent.Get(), 0) != WAIT_TIMEOUT) return 4;
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
} // namespace pulse::app
