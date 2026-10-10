#pragma once
#include <string>
#include <string_view>
#include "runtime_log.h"

namespace pulse::diagnostics {
struct ExportOptions;
std::string SanitizeRuntime(std::string_view text, uint64_t& rejected);
bool WriteSupportReport(const ExportOptions& options, bool copied, bool flushed,
    const runtime::Health& health, std::wstring* error);
bool CreateSupportArchive(const std::wstring& directory, const std::wstring& path, std::wstring* error);
}
