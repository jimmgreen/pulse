#include "app_diagnostics.h"
#include "session.h"
#include "../common/diagnostics_exporter.h"
#include "../common/diagnostics_report.h"
#include "../common/utf8_file.h"
#include "../common/localization.h"
#include "../index/index_client.h"
#include "../index/index_config.h"
#include <windows.h>
#include <filesystem>
#include <fstream>

namespace pulse {
bool ExportAppDiagnostics(const std::wstring& destination, bool include_service, bool include_dumps, std::wstring& error) {
    namespace fs = std::filesystem;
    try {
        if (!fs::is_directory(destination) || !fs::is_empty(destination) ||
            (GetFileAttributesW(destination.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = l10n::Pick(L"请选择空文件夹保存诊断包。", L"Choose an empty folder for the diagnostics package.");
            return false;
        }
        diagnostics::ExportOptions options;
        options.source_root = app::GetPulseDataDir();
        options.destination = (fs::path(destination) / L"User").wstring();
        options.include_dumps = include_dumps;
        options.support_report = options.collect_environment = true;
        options.configuration_file = index::MachineConfigPath();
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        executable.resize(length);
        options.install_directory = fs::path(executable).parent_path().wstring();
        if (!fs::create_directory(options.destination)) return false;
        const bool user_ok = diagnostics::Export(options, &error);
        bool service_ok = false;
        if (include_service) {
            const auto service_dir = fs::path(destination) / L"IndexService";
            service_ok = fs::create_directory(service_dir) && index::IndexClient::ExportDiagnosticsElevated(service_dir.wstring(), include_dumps);
        }
        std::ofstream summary(fs::path(destination) / L"feedback-summary.txt", std::ios::binary);
        summary << "Pulse diagnostic feedback package\r\n\r\nUser logs: " << (user_ok ? "exported" : "partial or unavailable")
            << "\r\nIndex service logs: " << (!include_service ? "not requested" : service_ok ? "exported" : "unavailable or administrator permission declined")
            << "\r\nCrash dumps: " << (include_dumps ? "explicitly included; may contain private information" : "excluded")
            << "\r\n\r\nSend Pulse-diagnostics.zip after reviewing it.\r\nUser/summary.txt and User/support-report.json describe recent failures, binaries and missing evidence.\r\n"
            << "An exported file set does not guarantee every process or event was captured.\r\n";
        summary.close();
        if (!summary) { error = L"Could not write feedback summary."; return false; }
        // Preserve a usable partial package when service access is declined.
        const bool archived = diagnostics::CreateSupportArchive(destination, (fs::path(destination) / L"Pulse-diagnostics.zip").wstring(), &error);
        if (archived) error.clear();
        return archived;
    } catch (...) {
        error = l10n::Pick(L"诊断包未完整生成，已导出的文件仍保留在所选文件夹。", L"The diagnostic package is incomplete. Exported files remain in the selected folder.");
        return false;
    }
}
}
