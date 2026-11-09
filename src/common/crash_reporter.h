#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::crash {

enum class ProcessRole : uint8_t {
    App,
    IndexService,
    IndexHelper,
    NetworkAgent,
    ContentAgent,
    Preview,
    Shell,
    Test,
};

struct Config {
    ProcessRole role = ProcessRole::App;
    bool machine_scope = false;
    std::wstring data_root;
};

bool Initialize(const Config& config) noexcept;
void Shutdown() noexcept;
void AddBreadcrumb(uint32_t category, uint32_t action, int32_t result = 0) noexcept;
LONG ReportFatal(EXCEPTION_POINTERS* exception, const char* context) noexcept;
LONG ReportRecoverable(EXCEPTION_POINTERS* exception, const char* context) noexcept;
[[noreturn]] void ReportTerminate(const char* context = "terminate") noexcept;
bool SafeModeRequested() noexcept;
const std::wstring& DiagnosticsRoot() noexcept;
const wchar_t* RoleName(ProcessRole role) noexcept;

// Called from the crash path for a fatal report with the faulting address.
// Returns the id of the extension whose image contains it, or nullptr. It
// runs inside the exception filter: no allocation, no locks, no throwing.
using FaultAttribution = const char* (*)(const void* address) noexcept;
void SetFaultAttribution(FaultAttribution attribution) noexcept;

} // namespace pulse::crash
