#pragma once
#include "diagnostics_exporter.h"
#include <functional>

namespace pulse::diagnostics {
struct PackageResult {
    std::wstring directory;
    bool user_complete = false;
    bool service_complete = false;
};
// Each request gets a new child of the selected directory, including retries.
// A declined service export still leaves a usable, explicitly partial package.
bool ExportSupportPackage(ExportOptions options, const std::wstring& parent,
    bool include_service, const std::function<bool(const std::wstring&, bool)>& service_export,
    PackageResult& result, std::wstring& error);
}
