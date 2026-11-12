#include "../common/dialog_process_diagnostics.h"
#include <cstdio>
#include <cwchar>
#include <string_view>

namespace diag = pulse::dialog_diagnostics;
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--pid") {
        wchar_t* end = nullptr; const auto pid = wcstoul(argv[2], &end, 10);
        if (!pid || !end || *end) return 2;
        const auto snapshot = diag::CaptureProcess(pid);
        std::printf("%s", diag::Serialize(snapshot).c_str());
        return snapshot.process_query_error ? 2 : 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--visible") {
        std::printf("%s", diag::CaptureVisibleProcesses().c_str()); return 0;
    }
    if (argc != 1) return 2;
    int checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* name) {
        ++checks; failures += !pass; std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    };
    diag::ProcessSnapshot value; value.pid = 42; value.creation_time = 123; value.identity_verified = true;
    value.module_status = diag::ModuleStatus::Complete; value.loaded_components[0] = true;
    auto text = diag::Serialize(value);
    check(text.find("\"module_status\":\"complete\"") != std::string::npos && text.find("[\"auto\"]") != std::string::npos,
        "complete same-lifetime observation preserves present component");
    value.module_status = diag::ModuleStatus::AccessDenied; value.module_error = ERROR_ACCESS_DENIED;
    text = diag::Serialize(value);
    check(text.find("\"module_status\":\"access_denied\"") != std::string::npos && text.find("\"module_error\":5") != std::string::npos && text.find("\"loaded_components\":[]") != std::string::npos,
        "access denied cannot become an absent or present component claim");
    value.module_status = diag::ModuleStatus::Partial;
    text = diag::Serialize(value);
    check(text.find("\"module_status\":\"partial\"") != std::string::npos && text.find("[\"auto\"]") != std::string::npos,
        "partial observation keeps only positive observations");
    value.module_status = diag::ModuleStatus::Complete;
    check(diag::Serialize(value).find("\"module_status\":\"partial\"") != std::string::npos,
        "enumeration error never claims complete inventory");
    value.module_error = 0; value.identity_verified = false;
    text = diag::Serialize(value);
    check(text.find("\"module_status\":\"unavailable\"") != std::string::npos && text.find("\"loaded_components\":[]") != std::string::npos,
        "process lifetime mismatch discards stale module observations");
    value.identity_verified = true; value.creation_time.reset();
    check(diag::Serialize(value).find("\"module_status\":\"unavailable\"") != std::string::npos,
        "missing creation time cannot prove a complete inventory");
    check(diag::Serialize(value).find("\"elevated\":null") != std::string::npos,
        "unknown elevation remains null rather than unelevated");
    const auto self = diag::CaptureProcess(GetCurrentProcessId());
    check(self.process_query_error == 0 && self.creation_time && self.app_image_id && self.identity_verified,
        "read-only self probe records stable process identity");
    check(self.module_status == diag::ModuleStatus::Complete && self.module_error == 0,
        "self module observation completes without process memory reads");
    text = diag::Serialize(self);
    check(text.find("C:") == std::string::npos && text.find(".exe") == std::string::npos && text.find("logger_session") == std::string::npos,
        "snapshot excludes paths image names and guessed logger sessions");
    const auto cancelled = diag::CaptureVisibleProcesses([] { return true; });
    check(cancelled.find("\"counts_as_requests\":false") != std::string::npos && cancelled.find("\"snapshot_count\":0") != std::string::npos,
        "cancelled collection has no request denominator or process samples");
    std::printf("PROCESS_DIAGNOSTICS checks=%d failures=%d process_writes=0 ui_input=0\n", checks, failures);
    return failures ? 1 : 0;
}
