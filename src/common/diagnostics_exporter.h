#pragma once

#include <string>

namespace pulse::diagnostics {

struct ExportOptions {
    std::wstring source_root;
    std::wstring destination;
    bool include_dumps = true;
    bool require_empty_destination = true;
    // Ordinary support export: numeric runtime evidence, no legacy/path logs.
    bool support_report = false;
    bool collect_environment = false;
    std::wstring install_directory;
    std::wstring configuration_file;
};

bool Export(const ExportOptions& options, std::wstring* error = nullptr);
bool ClearCrashReports(const std::wstring& source_root, std::wstring* error = nullptr);

} // namespace pulse::diagnostics
