#include "../shell_bridge/file_dialog_generation.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    using namespace pulse;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    unsigned failures = 0;
    const auto check = [&](bool pass, const char* name) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name); failures += !pass;
    };
    const std::wstring root = L"C:\\generation-test\\DialogBridge\\";
    const auto path = [&](wchar_t digit) { return root + std::wstring(64, digit) + L"\\" + dialog_auto::DllName; };
    const auto own = path(L'a'), next = path(L'b');
    const auto caller = GetCurrentThreadId();
    std::atomic<unsigned> calls{0};
    std::atomic<bool> off_thread{false}, verified{true}, throws{false};
    shell_bridge::GenerationAdmission admission([&](const std::wstring& old, const std::wstring& current) -> std::shared_ptr<const void> {
        ++calls; off_thread = GetCurrentThreadId() != caller;
        if (throws) throw std::runtime_error("fixture");
        if (old != own || current.empty() || !verified) return {};
        return std::make_shared<int>(1);
    });
    dialog_auto::Status status{true, false, 0, own};
    check(admission.Enabled(own, status) && calls == 0, "same generation preserves immediate admission");
    status.dll_path = next;
    check(admission.Enabled(own, status) && calls == 1 && off_thread, "first changed generation waits for worker proof, no retry");
    check(admission.Enabled(own, status) && calls == 1, "cached generation does not hash again on resolve or validation");
    status.enabled = false;
    check(!admission.Enabled(own, status), "disabled overrides cached proof");
    status.enabled = true; status.conflict = true;
    check(!admission.Enabled(own, status), "registration conflict overrides cached proof");
    status.conflict = false; status.error = ERROR_ACCESS_DENIED;
    check(!admission.Enabled(own, status), "registration query error overrides cached proof");
    status.error = 0;
    status.dll_path = L"C:\\other-install\\DialogBridge\\" + std::wstring(64, L'b') + L"\\" + dialog_auto::DllName;
    check(!admission.Enabled(own, status) && calls == 1, "other installation cannot reuse authorization");
    status.dll_path = root + L"not-a-digest\\" + dialog_auto::DllName;
    check(!admission.Enabled(own, status) && calls == 1, "unaddressed deployment cannot reuse authorization");
    status.dll_path = path(L'c'); verified = false;
    check(!admission.Enabled(own, status), "failed closure verification remains native");
    verified = true;
    check(admission.Enabled(own, status), "transient verification failure may recover on next request");
    status.dll_path = path(L'd'); throws = true;
    check(!admission.Enabled(own, status), "worker exception fails closed and signals waiter");
    throws = false;
    check(admission.Enabled(own, status), "worker exception does not poison subsequent generation");
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> first_result{false};
    shell_bridge::GenerationAdmission concurrent([&](const std::wstring&, const std::wstring& current) -> std::shared_ptr<const void> {
        if (current == next) {
            SetEvent(started);
            if (WaitForSingleObject(release, 5000) != WAIT_OBJECT_0) return {};
        }
        return std::make_shared<int>(2);
    });
    std::thread first([&] {
        const HRESULT sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        first_result = SUCCEEDED(sta) && concurrent.Enabled(own, {true, false, 0, next});
        if (SUCCEEDED(sta)) CoUninitialize();
    });
    const bool waiting = WaitForSingleObject(started, 5000) == WAIT_OBJECT_0;
    const bool second = concurrent.Enabled(own, {true, false, 0, path(L'e')});
    SetEvent(release); first.join();
    check(waiting && second && first_result, "two STAs retain independent generation jobs with reverse completion");
    CloseHandle(started); CloseHandle(release);
    std::atomic<unsigned> shared_calls{0};
    shell_bridge::GenerationAdmission shared([&](const std::wstring&, const std::wstring&) -> std::shared_ptr<const void> {
        ++shared_calls; Sleep(100); return std::make_shared<int>(3);
    });
    std::thread same([&] {
        const HRESULT sta = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        first_result = SUCCEEDED(sta) && shared.Enabled(own, {true, false, 0, next});
        if (SUCCEEDED(sta)) CoUninitialize();
    });
    const bool same_result = shared.Enabled(own, {true, false, 0, next});
    same.join();
    check(first_result && same_result && shared_calls == 1, "concurrent same-generation callers share one verification");
    if (argc == 3) {
        shell_bridge::GenerationAdmission actual;
        dialog_auto::Status real{true, false, 0, argv[2]};
        check(actual.Enabled(argv[1], real), "real content-addressed manifests and both entry files verify");
        real.enabled = false;
        check(!actual.Enabled(argv[1], real), "real proof does not bypass later disable");
    }
    CoUninitialize();
    std::printf("GENERATION_POLICY failures=%u\n", failures);
    return failures ? 1 : 0;
}
