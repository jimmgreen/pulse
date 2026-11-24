#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace pulse::dialog_diagnostics {
struct ExcerptCounts {
    unsigned shows_started = 0, shows_finished = 0, runtime_decisions = 0;
    unsigned foreground_shows_started = 0, foreground_shows_finished = 0;
};
struct ReproductionWindow {
    uint64_t start_filetime = 0, end_filetime = 0;
    uint32_t foreground_pid = 0, reported_operation = 0;
};
struct RawLogCopy {
    uint64_t bytes = 0;
    unsigned files = 0, skipped = 0, read_errors = 0;
    unsigned long last_error = 0;
    bool limited = false;
};
// Reads only immediate, fixed-name log files. Exports a bounded privacy whitelist,
// followed by a dialog_log_collection summary; absence never proves no entry.
// No process/snapshot correlation, recursion, registry writes or application calls.
// Local UI/history only: includes validated executable basenames and producer
// identity. Never use this variant for diagnostics export or uploads.
std::string CollectLocalTakeoverExcerpt(const std::wstring& runtime_log_dir,
    const std::function<bool()>& cancelled = {});
std::string CollectDialogLogExcerpt(const std::wstring& runtime_log_dir,
    const std::function<bool()>& cancelled = {}, ExcerptCounts* counts = nullptr,
    bool verified_managed_directory = false, const ReproductionWindow* reproduction = nullptr);
// Explorer handoff is an app-side path, not a dialog API call. Export only
// its structured fields, never the host's raw logs or file names.
std::string CollectExplorerLogExcerpt(const std::wstring& runtime_log_dir,
    const std::function<bool()>& cancelled = {}, const ReproductionWindow* reproduction = nullptr);
// Byte-for-byte, immediate files only. The destination is a fresh private staging directory.
// The caller verifies managed sessions and reuses one budget across all sources.
void CopyDialogRawLogs(const std::wstring& source, const std::wstring& destination,
    bool verified_managed_directory, RawLogCopy& result, const std::function<bool()>& cancelled = {});
}
