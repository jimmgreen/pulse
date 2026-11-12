#include "../dialog_launcher/target_preflight.h"
#include <filesystem>
#include <cstdio>
#include <vector>
#include <array>
namespace fs = std::filesystem;
using namespace pulse::dialog_launcher;
namespace {
int failures = 0, checks = 0;
void Check(bool value, const char* text) {
    ++checks; if (!value) ++failures;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text);
}
struct Child {
    HANDLE stop = nullptr, process = nullptr;
    bool Start(const fs::path& image, const std::wstring& event) {
        stop = CreateEventW(nullptr, TRUE, FALSE, event.c_str());
        std::wstring command = L"\"" + image.wstring() + L"\" --child " + event;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION pi{};
        if (!stop || !CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, image.parent_path().c_str(), &startup, &pi)) return false;
        process = pi.hProcess; CloseHandle(pi.hThread); return true;
    }
    bool Finish() {
        if (stop) SetEvent(stop);
        return !process || WaitForSingleObject(process, 10000) == WAIT_OBJECT_0;
    }
    ~Child() { Finish(); if (process) CloseHandle(process); if (stop) CloseHandle(stop); }
};
void ModelCases() {
    const std::wstring target = L"\\\\?\\C:\\fixture\\target.exe";
    const std::array<DWORD, 1> pids{7};
    auto scan = [&](TargetObservation observation) {
        return ScanTargetSnapshot(target, pids, [observation](DWORD) { return observation; });
    };
    auto r = scan({TargetObservationState::Image, L"c:\\FIXTURE\\TARGET.exe", 0});
    Check(r.scan_complete && r.existing_target_count == 1, "full image comparison normalizes DOS prefix and case");
    r = scan({TargetObservationState::Image, L"C:\\other\\target.exe", 0});
    Check(r.scan_complete && !r.existing_target_count, "same basename in different directory is not a match");
    r = scan({TargetObservationState::OtherSession, target, 0});
    Check(r.scan_complete && !r.existing_target_count, "other session is not counted");
    r = scan({TargetObservationState::QueryError, {}, ERROR_ACCESS_DENIED});
    Check(!r.scan_complete && r.query_errors == 1 && !r.existing_target_count, "query denied remains unknown, not absence");
    Check(TargetPreflightJson(r, L"session", 1).find("\"warning\":\"scan_incomplete\"") != std::string::npos,
        "incomplete zero matches emits unknown warning");
    r = scan({TargetObservationState::Vanished, {}, ERROR_INVALID_PARAMETER});
    Check(!r.scan_complete && r.vanished_processes == 1, "vanished snapshot member marks partial scan");
    bool queried = false;
    r = ScanTargetSnapshot(target, pids, [&](DWORD) { queried = true; return TargetObservation{}; }, [] { return true; });
    Check(r.cancelled && !r.scan_complete && !queried && !r.scanned_processes, "cancel stops before process query");
    r = ScanTargetSnapshot(target, {}, {});
    Check(!r.scan_complete && r.query_errors == 1, "missing query implementation fails closed");
    r = ScanTargetSnapshot(target, {}, [](DWORD) { return TargetObservation{}; });
    Check(r.scan_complete && !r.existing_target_count, "complete empty snapshot is distinct from unknown");
    const auto none = TargetPreflightJson(r, L"{guid}", 77);
    Check(none.find("\"warning\":\"none\"") != std::string::npos && none.find("\"schema\":1") != std::string::npos &&
        none.find("\"certified\":0") != std::string::npos && none.find("\"existing_target_managed\":\"unknown\"") != std::string::npos,
        "structured warning remains not-certified and never infers management");
    const std::array<DWORD, 2> mixed{1, 2};
    r = ScanTargetSnapshot(target, mixed, [&](DWORD pid) { return pid == 1 ?
        TargetObservation{TargetObservationState::Image, target, 0} : TargetObservation{}; });
    Check(r.existing_target_count == 1 && !r.scan_complete && r.query_errors == 1 &&
        TargetPreflightJson(r, L"guid", 1).find("\"warning\":\"existing_target\"") != std::string::npos,
        "confirmed match remains lower bound when another query fails");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--child") {
        HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!stop) return 2;
        const DWORD wait = WaitForSingleObject(stop, 30000); CloseHandle(stop);
        return wait == WAIT_OBJECT_0 ? 0 : 3;
    }
    if (argc != 2) return 2;
    ModelCases();
    const fs::path root = fs::absolute(argv[1]);
    if (fs::exists(root)) return 3;
    fs::create_directories(root / L"a"); fs::create_directories(root / L"b");
    std::wstring self(32768, L'\0'); self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    const auto a = root / L"a" / L"fixture.exe", b = root / L"b" / L"fixture.exe";
    fs::copy_file(self, a); fs::copy_file(self, b);
    auto r = ScanExistingTarget(a.wstring());
    Check(!r.existing_target_count, "native snapshot has no fixture target before child creation");
    Child child;
    const auto event = L"Local\\Pulse.Preflight.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    Check(child.Start(a, event), "start owned native child fixture");
    r = ScanExistingTarget(a.wstring());
    Check(r.existing_target_count == 1, "native query detects pre-existing target child");
    std::printf("native-existing-json=%s\n", TargetPreflightJson(r, L"fixture", GetCurrentProcessId()).c_str());
    r = ScanExistingTarget(b.wstring());
    Check(!r.existing_target_count, "native same basename different executable path is excluded");
    r = ScanExistingTarget(a.wstring(), [] { return true; });
    Check(r.cancelled && !r.scan_complete, "native scan supports cancellation without changing child");
    Check(WaitForSingleObject(child.process, 0) == WAIT_TIMEOUT, "read-only scans leave old target running");
    Check(child.Finish(), "fixture child exits through its own event, not termination");
    r = ScanExistingTarget(a.wstring());
    Check(!r.existing_target_count, "exited fixture disappears from confirmed target count");
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    return failures ? 1 : 0;
}
