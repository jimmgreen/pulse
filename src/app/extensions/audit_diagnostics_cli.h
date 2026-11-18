#pragma once
#include <optional>
namespace pulse::extensions {
// Host-local process startup only. Never dispatch this through MCP or a package caller.
std::optional<int> HandleAuditDiagnosticsCli(int argc, wchar_t** argv);
}
