#include "target_preflight.h"
#include <tlhelp32.h>
#include <vector>
#include <sstream>
#include <iomanip>

namespace pulse::dialog_launcher {
namespace {
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring_view ImageSpelling(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return path; // Do not equate ambiguous UNC spellings.
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    return path;
}
bool SameImage(std::wstring_view a, std::wstring_view b) {
    a = ImageSpelling(a); b = ImageSpelling(b);
    return !a.empty() && a.size() == b.size() && a.size() <= 32767 &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
TargetObservation QueryImage(DWORD pid, DWORD session) {
    DWORD candidate_session = 0;
    if (!ProcessIdToSessionId(pid, &candidate_session)) {
        const DWORD error = GetLastError();
        return {error == ERROR_INVALID_PARAMETER ? TargetObservationState::Vanished : TargetObservationState::QueryError, {}, error};
    }
    if (candidate_session != session) return {TargetObservationState::OtherSession, {}, 0};
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
    if (!process.value) {
        const DWORD error = GetLastError();
        return {error == ERROR_INVALID_PARAMETER ? TargetObservationState::Vanished : TargetObservationState::QueryError, {}, error};
    }
    if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) return {TargetObservationState::Vanished, {}, 0};
    std::wstring image(32768, L'\0');
    DWORD count = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.value, 0, image.data(), &count))
        return {TargetObservationState::QueryError, {}, GetLastError()};
    image.resize(count);
    if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) return {TargetObservationState::Vanished, {}, 0};
    return {TargetObservationState::Image, std::move(image), 0};
}
std::string JsonString(std::wstring_view value) {
    std::ostringstream out;
    out << '"';
    for (wchar_t c : value) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') out << static_cast<char>(c);
        else out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c);
    }
    out << '"';
    return out.str();
}
}
TargetPreflight ScanTargetSnapshot(std::wstring_view target, std::span<const DWORD> pids,
    const std::function<TargetObservation(DWORD)>& query, const std::function<bool()>& cancelled) {
    TargetPreflight result;
    if (target.empty() || !query) { result.query_errors = 1; return result; }
    for (DWORD pid : pids) {
        if (cancelled && cancelled()) { result.cancelled = true; break; }
        ++result.scanned_processes;
        const auto observation = query(pid);
        switch (observation.state) {
        case TargetObservationState::Image:
            ++result.same_session_processes;
            if (observation.image.empty()) ++result.query_errors;
            else if (SameImage(target, observation.image)) ++result.existing_target_count;
            break;
        case TargetObservationState::OtherSession: break;
        case TargetObservationState::Vanished: ++result.vanished_processes; break;
        case TargetObservationState::QueryError: ++result.query_errors; break;
        }
    }
    result.scan_complete = !result.cancelled && !result.query_errors && !result.vanished_processes;
    return result;
}
TargetPreflight ScanExistingTarget(std::wstring_view target, const std::function<bool()>& cancelled) {
    TargetPreflight failed;
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) { failed.query_errors = 1; return failed; }
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) { failed.query_errors = 1; return failed; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::vector<DWORD> pids;
    BOOL found = Process32FirstW(snapshot.value, &entry);
    while (found) {
        if (entry.th32ProcessID && entry.th32ProcessID != GetCurrentProcessId()) pids.push_back(entry.th32ProcessID);
        found = Process32NextW(snapshot.value, &entry);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) { failed.query_errors = 1; return failed; }
    const auto deadline = GetTickCount64() + 2000;
    bool timed_out = false;
    auto result = ScanTargetSnapshot(target, pids, [session](DWORD pid) { return QueryImage(pid, session); }, [&] {
        if (cancelled && cancelled()) return true;
        timed_out = GetTickCount64() >= deadline;
        return timed_out;
    });
    result.timed_out = timed_out;
    if (timed_out) result.cancelled = false;
    return result;
}
std::string TargetPreflightJson(const TargetPreflight& r, std::wstring_view session, DWORD launcher_pid) {
    const char* warning = r.existing_target_count ? "existing_target" : r.scan_complete ? "none" : "scan_incomplete";
    std::ostringstream out;
    out << "{\"schema\":1,\"session\":" << JsonString(session) << ",\"pid\":" << launcher_pid
        << ",\"event\":\"target_preflight\",\"data\":{\"existing_target_count\":" << r.existing_target_count
        << ",\"scan_complete\":" << (r.scan_complete ? "true" : "false") << ",\"query_errors\":" << r.query_errors
        << ",\"vanished_processes\":" << r.vanished_processes << ",\"cancelled\":" << (r.cancelled ? "true" : "false")
        << ",\"timed_out\":" << (r.timed_out ? "true" : "false") << ",\"scanned_processes\":" << r.scanned_processes
        << ",\"same_session_processes\":" << r.same_session_processes << ",\"warning\":\"" << warning
        << "\",\"existing_target_managed\":\"unknown\",\"certified\":0}}";
    return out.str();
}
}
