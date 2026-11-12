#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulse::dialog_diagnostics {
enum class ModuleStatus { Complete, Partial, AccessDenied, Unavailable };
struct ProcessSnapshot {
    DWORD pid = 0, process_query_error = 0, elevation_error = 0, module_error = 0;
    unsigned process_arch = 0;
    std::optional<bool> elevated;
    std::optional<uint64_t> creation_time, app_image_id;
    uint64_t sample_tick_ms = 0;
    ModuleStatus module_status = ModuleStatus::Unavailable;
    std::array<bool, 3> loaded_components{};
    bool identity_verified = false;
};
// Local-only module paths, never serialized/exported by Serialize.
struct RestartProcessSnapshot {
    ProcessSnapshot process;
    std::wstring image_path;
    std::vector<std::wstring> module_paths;
    DWORD scope_error = 0;
    bool same_user_session = false;
};
RestartProcessSnapshot CaptureRestartProcess(DWORD pid);
// Read-only metadata: no injection, process memory reads, window messages or input.
ProcessSnapshot CaptureProcess(DWORD pid);
std::string Serialize(const ProcessSnapshot& snapshot);
std::string CaptureVisibleProcesses(const std::function<bool()>& cancelled = {});
}
