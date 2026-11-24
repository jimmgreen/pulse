// explorer_launch_origin.h — which application launched a reveal through
// `explorer.exe /select,<path>`.
//
// The launcher Explorer process lives only until it hands the request to the
// Shell, before the File Explorer window appears, so a periodic process scan
// records each launcher it sees (its selected path and parent application).
// The takeover then asks who asked for the path it is about to show.
#pragma once
#include <windows.h>
#include <tlhelp32.h>

#include <string>
#include <string_view>

namespace pulse::app::explorer_launch_origin {

// Called for every process of a periodic snapshot (any thread).
void Observe(const PROCESSENTRY32W& entry) noexcept;
// Inspects one process as a launcher (its /select command line and parent);
// Observe applies it to explorer.exe entries only. Exposed for tests.
void Inspect(DWORD pid, DWORD parent_pid) noexcept;
// Image path of the application that launched Explorer to select
// `selected_path` within the last `window_ms`; empty if unknown or ambiguous.
std::wstring FindLauncherParent(std::wstring_view selected_path, ULONGLONG window_ms) noexcept;
// The path after `/select,` in an Explorer command line; empty if absent.
std::wstring SelectTarget(std::wstring_view command_line);
// Test seam: records an observation as Observe would after inspecting a process.
void RecordForTest(std::wstring selected_path, std::wstring parent_image, ULONGLONG tick);

} // namespace pulse::app::explorer_launch_origin
