#pragma once
#include "runtime.h"
namespace pulse::extensions {
// UI thread: bounded session records plus a nonblocking host-only persistence mirror.
// ReadExtensionAudit remains scoped to the current caller/owner session.
// No file paths, request payloads or secrets are logged.
void RecordExtensionCall(const CallerBinding& caller, std::string_view operation, std::string_view outcome);
json::Value ReadExtensionAudit(const CallerBinding& caller, size_t limit);
}
