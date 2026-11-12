#pragma once
#include <windows.h>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace pulse::dialog_launcher {
enum class TargetObservationState { Image, OtherSession, Vanished, QueryError };
struct TargetObservation {
    TargetObservationState state = TargetObservationState::QueryError;
    std::wstring image;
    DWORD error = ERROR_GEN_FAILURE;
};
struct TargetPreflight {
    uint32_t existing_target_count = 0;
    uint32_t scanned_processes = 0;
    uint32_t same_session_processes = 0;
    uint32_t query_errors = 0;
    uint32_t vanished_processes = 0;
    bool scan_complete = false;
    bool cancelled = false;
    bool timed_out = false;
};
// A snapshot observation, not proof of absence or single-instance behavior.
// Matches full image paths, not basenames; hard-link aliases are not inferred.
TargetPreflight ScanTargetSnapshot(std::wstring_view target, std::span<const DWORD> pids,
    const std::function<TargetObservation(DWORD)>& query,
    const std::function<bool()>& cancelled = {});
TargetPreflight ScanExistingTarget(std::wstring_view target,
    const std::function<bool()>& cancelled = {});
std::string TargetPreflightJson(const TargetPreflight& result, std::wstring_view session, DWORD launcher_pid);
}
