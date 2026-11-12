#include "../shell_bridge/file_dialog_remote_provider.cpp"
#include <cstdio>

int wmain() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label, HRESULT result = S_OK) {
        std::printf("[%s] %s hr=0x%08lx\n", ok ? "PASS" : "FAIL", label, static_cast<unsigned long>(result));
        if (!ok) ++failures;
    };
    {
        using namespace pulse::shell_bridge;
        auto context = std::make_shared<Context>();
        context->initialized = true;
        context->showing = true;
        context->callback = true;
        context->io = true;
        context->show_id = 1;
        context->accepted.show_id = 1;
        active = context;
        const auto token = context->token;
        const auto sequence = context->sequence;
        const HRESULT wrong = Close(token, 2, E_ABORT);
        check(wrong == HRESULT_FROM_WIN32(ERROR_RETRY) && !context->close_requested,
            "wrong show rejects without recording Close", wrong);
        const HRESULT closed = Close(token, 1, E_ABORT);
        check(closed == S_OK, "Close during callback RPC message pump is accepted", closed);
        check(context->close_requested && context->close_result == E_ABORT,
            "Close intent and exact HRESULT are retained");
        check(context->accepted.show_id == 0, "Close invalidates previously accepted selection");
        const HRESULT duplicate = Close(token, 1, E_ACCESSDENIED);
        check(duplicate == S_FALSE && context->close_result == E_ABORT,
            "duplicate Close preserves the first reason", duplicate);
        PulsePickerUiV2Snapshot snapshot{};
        snapshot.size = sizeof(snapshot);
        const HRESULT queried = Query(token, 1, 0, &snapshot);
        check(queried == RPC_E_CALL_REJECTED, "other callback RPCs remain rejected", queried);
        unsigned operations = 0;
        const HRESULT nested = context->Io([&](HANDLE) { ++operations; return TransportResult{}; });
        check(nested == RPC_E_CALL_REJECTED && operations == 0, "nested I/O never invokes transport", nested);
        check(context->sequence == sequence && context->pending_close == 0 && !context->channel &&
            !context->dead && context->io, "Close performs no channel I/O or sequence mutation");
        active.reset();
    }
    CoUninitialize();
    std::printf("reentrant-close failures=%d\n", failures);
    return failures ? 1 : 0;
}
