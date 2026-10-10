#pragma once
#include <string>
namespace pulse {
bool ExportAppDiagnostics(const std::wstring& destination, bool include_service, bool include_dumps, std::wstring& error);
}
