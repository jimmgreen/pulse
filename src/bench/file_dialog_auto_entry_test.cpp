#define PULSE_RUNTIME_ENTRY_TEST
#include "../shell_bridge/file_dialog_runtime.cpp"

int main() {
    unsigned failures = 0;
    const auto check = [&](bool value, const char* name) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); if (!value) ++failures;
    };
    auto request = std::make_unique<config::PulseRuntimeAutoStartV1>();
    wcscpy_s(request->nonce, L"{9CFDE88A-65C7-44C5-BF5C-CA02456683F2}");
    wcscpy_s(request->host, L"C:\\missing-auto-fixture\\host.exe");
    wcscpy_s(request->control, L"C:\\missing-auto-fixture\\control");
    check(PulseFileDialogRuntimeAutoStart(nullptr, sizeof(*request)) == E_INVALIDARG, "null input rejected");
    check(PulseFileDialogRuntimeAutoStart(request.get(), sizeof(*request) - 1) == E_INVALIDARG, "size mismatch rejected");
    request->version = 2;
    check(!ValidAutoRequest(request.get(), sizeof(*request)), "unknown version rejected"); request->version = 1;
    request->nonce[39] = L'x';
    check(!ValidAutoRequest(request.get(), sizeof(*request)), "nonce trailing data rejected"); request->nonce[39] = 0;
    request->host[config::kPathUnits - 1] = L'x';
    check(!ValidAutoRequest(request.get(), sizeof(*request)), "unterminated host rejected"); request->host[config::kPathUnits - 1] = 0;
    check(ValidAutoRequest(request.get(), sizeof(*request)), "bounded explicit request accepted structurally");
    bool deferred = true;
    check(Enter(0, CLSID_FileOpenDialog, deferred) == nullptr && !deferred,
        "partially installed automatic imports remain native before provider publication");
    auto_started = true;
    check(PulseFileDialogRuntimeAutoStart(request.get(), sizeof(*request)) == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), "duplicate start rejected before scheduling");
    auto_started = false;
    if (!pulse::dialog_admin::IsElevated()) {
        check(PulseFileDialogRuntimeAutoStart(request.get(), sizeof(*request)) == E_ACCESSDENIED, "ordinary token rejected before scheduling");
        check(!auto_started.load(), "rejected request does not consume start slot");
    }
    Initialize(request.release());
    check(FAILED(initialization.load()) && current.load() == nullptr && install_result.load() == ERROR_NOT_READY,
        "failed validation neither installs hooks nor publishes provider");
    PulseFileDialogRuntimeStatus status{};
    check(SUCCEEDED(PulseFileDialogRuntimeQuery(&status, sizeof(status))) && !status.ready && !status.installed_entries,
        "query preserves native state after failure");
    config::PulseRuntimeAutoCoverageV1 coverage;
    check(SUCCEEDED(PulseFileDialogRuntimeAutoQuery(&coverage, sizeof(coverage))) && !coverage.api_mask && !coverage.patched_slots,
        "failed initialization reports no patched import coverage");
    std::printf("failures=%u\n", failures);
    return failures ? 1 : 0;
}
