// download_source_probe.h - Settings > 增强包 > 测速: one small download (the
// update manifest) through each fixed source, timed. Results live for this
// run only; nothing is saved.
#pragma once
#include "../common/windows_compat.h"
#include <string>

namespace pulse::app {

inline constexpr int kSourceProbeCount = 3;      // mirror 1, mirror 2, GitHub (the 测速 rows)
inline constexpr int kSourceProbeNotRun = -1;
inline constexpr int kSourceProbeRunning = -2;
inline constexpr int kSourceProbeFailed = -3;

// Starts a probe of every source (a running one is left alone); `notify`
// gets WM_SOURCE_PROBE_DONE as each one finishes.
void StartSourceProbe(HWND notify);
bool SourceProbeRunning() noexcept;
// Milliseconds, or kSourceProbeNotRun / Running / Failed, per row.
int SourceProbeResult(int row) noexcept;
// The system (IE) proxy GitHub goes through, as host:port; empty when none.
// Read when a probe starts.
std::wstring SourceProbeProxy();

} // namespace pulse::app
