#include "frame_perf_environment.h"
#include "../common/app_runtime_paths.h"
#include "app_diagnostics.h"
#include "../common/runtime_log.h"
#include "../common/localization.h"
#include "session.h"
#include "../common/diagnostics_exporter.h"
#include "../common/diagnostics_package.h"
#include "../index/index_client.h"
#include "../index/index_config.h"
#include <windows.h>

namespace pulse {
bool ExportAppDiagnostics(const std::wstring& destination, bool include_service, bool include_dumps, std::wstring& error) {
    diagnostics::runtime::Event("diagnostics_export_start", {{"service", include_service}, {"dumps", include_dumps}});
    try {
        const bool environment_saved = WriteFramePerfEnvironment(app::GetPulseDataDir() +
            L"\\Diagnostics\\Perf\\perf-environment-" + std::to_wstring(GetCurrentProcessId()) + L".json");
        diagnostics::runtime::Event("frame_environment_status", {{"saved", environment_saved}});
        diagnostics::ExportOptions options;
        options.source_root = app::GetPulseDataDir();
        options.include_dumps = include_dumps;
        options.support_report = options.collect_environment = true;
        options.configuration_file = index::MachineConfigPath();
        options.install_directory = app_runtime_paths::InstallRoot();
        diagnostics::PackageResult result;
        return diagnostics::ExportSupportPackage(std::move(options), destination, include_service,
            [](const std::wstring& folder, bool dumps) {
                return index::IndexClient::ExportDiagnosticsElevated(folder, dumps);
            }, result, error);
    } catch (...) {
        diagnostics::runtime::Event("diagnostics_export_failed", {{"stage", 0}, {"error", ERROR_GEN_FAILURE}},
            diagnostics::runtime::Level::Error);
        error = l10n::Get(l10n::StringId::DiagnosticsExportFailed);
        return false;
    }
}
}
