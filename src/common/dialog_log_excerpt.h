#pragma once
#include <functional>
#include <string>

namespace pulse::dialog_diagnostics {
// Reads only immediate, fixed-name log files. Exports a bounded privacy whitelist,
// followed by a dialog_log_collection summary; absence never proves no entry.
// No process/snapshot correlation, recursion, registry writes or application calls.
// Local UI/history only: includes validated executable basenames and producer
// identity. Never use this variant for diagnostics export or uploads.
std::string CollectLocalTakeoverExcerpt(const std::wstring& runtime_log_dir,
    const std::function<bool()>& cancelled = {});
std::string CollectDialogLogExcerpt(const std::wstring& runtime_log_dir,
    const std::function<bool()>& cancelled = {});
}
