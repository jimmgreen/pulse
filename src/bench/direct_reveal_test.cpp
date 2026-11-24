#include "../app/single_instance_coordinator.h"
#include "../common/direct_reveal_client.h"
#include <cstdio>
#include <cstring>
using C = pulse::app::SingleInstanceCoordinator;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    C::OpenRequest request; request.id[0] = 17; request.deadline = GetTickCount64() + 5000;
    request.path = L"C:\\fixture\\selected.txt";
    auto decode = [&](C::OpenRequest& out) {
        auto bytes = C::EncodeOpenRequest(request);
        COPYDATASTRUCT data{C::OpenRequestMessageId(), static_cast<DWORD>(bytes.size()), bytes.data()};
        return C::DecodeOpenRequest(&data, out);
    };
    C::OpenRequest decoded;
    check(decode(decoded) && !decoded.reveal && !decoded.wire_reveal, "v2 retains ordinary-open semantics");
    request.reveal = true;
    check(decode(decoded) && decoded.reveal && decoded.wire_reveal && decoded.path == request.path, "v3 roundtrip preserves explicit reveal and item path");
    request.reveal = false; request.wire_reveal = false; request.wire_direct_open = true;
    check(decode(decoded) && !decoded.reveal && !decoded.wire_reveal && decoded.wire_direct_open && decoded.path == request.path,
        "v4 roundtrip preserves direct folder-open intent without reveal");
    request.wire_direct_open = false; request.reveal = true;
    decode(decoded);
    HWND host = CreateWindowExW(0, L"STATIC", L"direct reveal fixture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(host != nullptr, "private receiver window created");
    if (!host) return 1;
    C coordinator;
    auto submit = [&] { return coordinator.SubmitOpenRequest(decoded, GetTickCount64(), host, WM_APP + 946); };
    check(submit() == C::OpenSubmission::Invalid, "wire reveal rejected when folder integration is disabled");
    coordinator.SetRevealEnabled(true);
    check(submit() == C::OpenSubmission::Accepted && submit() == C::OpenSubmission::Duplicate, "enabled receiver accepts once and deduplicates same ID");
    coordinator.SetRevealEnabled(false);
    check(submit() == C::OpenSubmission::Duplicate, "already accepted retry remains duplicate after integration disabled");
    auto queued = coordinator.TakeQueuedOpenRequest();
    check(queued && queued->reveal && queued->path == request.path && !coordinator.TakeQueuedOpenRequest(), "receiver queues exactly one selected item without shell resolution");
    decoded.path += L"changed";
    check(submit() == C::OpenSubmission::Invalid, "same ID with changed path rejected");
    coordinator.SetRevealEnabled(false);
    decoded.wire_reveal = false; decoded.id[0] = 18;
    check(submit() == C::OpenSubmission::Accepted, "internal quick-search reveal remains independent of integration");
    for (const auto* invalid : {L"shell:Downloads", L"relative.txt", L"\\\\server\\share\\file.txt", L"\\\\?\\C:\\file.txt"}) {
        request.path = invalid;
        check(!decode(decoded), "v3 rejects unsupported path forms");
        check(pulse::TryDirectReveal(invalid) == pulse::DirectRevealResult::Rejected, "sender rejects unsupported path before transport");
    }
    {
        C pipe_receiver;
        const auto endpoint_name = L"Local\\Pulse.DirectRevealTest." + std::to_wstring(GetCurrentProcessId());
        check(pipe_receiver.Acquire(endpoint_name) == C::AcquireResult::Primary &&
            pipe_receiver.PublishEndpoint(host, WM_APP + 947), "isolated authenticated pipe endpoint published");
        const auto endpoint = pulse::direct_reveal_detail::FindPrimaryEndpoint(endpoint_name);
        check(endpoint && endpoint->window == host && !endpoint->pipe_name.empty(), "lightweight endpoint reader matches coordinator layout and identity");
        pulse::DirectRevealDiagnostics diagnostics;
        auto send = [&] {
            const auto result = pulse::TryDirectReveal(L"C:\\fixture\\item.txt", 1000, endpoint_name, &diagnostics);
            std::printf("[TRANSPORT] result=%u attempts=%u stage=%u error=%lu reply=%d may_sent=%d\n",
                static_cast<unsigned>(result), diagnostics.attempts, static_cast<unsigned>(diagnostics.transport.stage),
                diagnostics.transport.error, diagnostics.transport.reply ? static_cast<int>(*diagnostics.transport.reply) : -1,
                diagnostics.transport.request_may_have_been_sent ? 1 : 0);
            return result;
        };
        check(send() == pulse::DirectRevealResult::Rejected && diagnostics.transport.reply == pulse::app::OpenRequestPipe::Reply::Invalid,
            "real pipe returns explicit rejection with integration disabled");
        pipe_receiver.SetRevealEnabled(true);
        check(send() == pulse::DirectRevealResult::Accepted,
            "real pipe accepts lightweight v3 sender");
        auto actual = pipe_receiver.TakeQueuedOpenRequest();
        check(actual && actual->wire_reveal && actual->reveal && actual->path == L"C:\\fixture\\item.txt",
            "real pipe queues exact item selection intent");
        bool consecutive = true;
        for (unsigned i = 0; i != 12; ++i) {
            const auto result = send();
            auto item = pipe_receiver.TakeQueuedOpenRequest();
            consecutive = consecutive && result == pulse::DirectRevealResult::Accepted && item && item->wire_reveal;
        }
        check(consecutive, "back-to-back sender requests survive pipe ACK/disconnect/reconnect window");
        pipe_receiver.SetRevealEnabled(false);
        check(pulse::TryDirectOpenFolder(L"C:\\fixture\\folder", 1000, endpoint_name, &diagnostics) == pulse::DirectRevealResult::Rejected &&
            diagnostics.transport.reply == pulse::app::OpenRequestPipe::Reply::Invalid,
            "v4 direct folder open rejected when folder integration is disabled");
        pipe_receiver.SetRevealEnabled(true);
        check(pulse::TryDirectOpenFolder(L"C:\\fixture\\folder", 1000, endpoint_name, &diagnostics) == pulse::DirectRevealResult::Accepted,
            "real pipe accepts v4 direct folder open when enabled");
        auto actual_dir = pipe_receiver.TakeQueuedOpenRequest();
        check(actual_dir && actual_dir->wire_direct_open && !actual_dir->reveal && actual_dir->path == L"C:\\fixture\\folder",
            "real pipe queues exact folder open without reveal");
        pipe_receiver.Release();
    }
    DestroyWindow(host);
    return failures ? 1 : 0;
}
