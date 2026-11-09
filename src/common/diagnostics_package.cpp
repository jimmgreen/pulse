#include "diagnostics_package.h"
#include "diagnostics_report.h"
#include "localization.h"
#include "runtime_log.h"
#include "utf8_file.h"
#include <windows.h>
#include <filesystem>
#include <fstream>

namespace pulse::diagnostics {
namespace {
enum Stage : unsigned { Destination = 1, CreatePackage, CollectUser, CollectService, Summary, Archive };
void Failure(Stage stage, DWORD code) {
    runtime::Event("diagnostics_export_failed", {{"stage", stage}, {"error", code}}, runtime::Level::Error);
}
}
bool ExportSupportPackage(ExportOptions options, const std::wstring& parent,
    bool include_service, const std::function<bool(const std::wstring&, bool)>& service_export,
    PackageResult& result, std::wstring& error) {
    namespace fs = std::filesystem;
    result = {};
    error.clear();
    Stage stage = Destination;
    const auto fail = [&](DWORD code) {
        Failure(stage, code);
        if (error.empty()) error = l10n::Get(result.directory.empty() ? l10n::StringId::DiagnosticsExportFailed
            : l10n::StringId::TheDiagnosticPackageIsIncompleteExportedFilesRemainInTheSele);
        wchar_t system_error[512]{};
        if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            code, 0, system_error, static_cast<DWORD>(std::size(system_error)), nullptr)) {
            std::wstring detail(system_error);
            while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n')) detail.pop_back();
            error += L"\n" + detail;
        }
        error += L" (" + std::to_wstring(static_cast<unsigned>(stage)) + L" / " + std::to_wstring(code) + L")";
        return false;
    };
    try {
        const DWORD attrs = GetFileAttributesW(parent.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return fail(GetLastError());
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return fail(ERROR_DIRECTORY);
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return fail(ERROR_REPARSE_TAG_INVALID);
        SYSTEMTIME now{}; GetLocalTime(&now);
        wchar_t stem[96]{};
        swprintf_s(stem, L"Pulse-diagnostics-%04u%02u%02u-%02u%02u%02u-%lu",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
        stage = CreatePackage;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = fs::path(parent) / (std::wstring(stem) + L"-" + std::to_wstring(attempt));
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { result.directory = candidate.wstring(); break; }
            const DWORD code = GetLastError();
            if (code != ERROR_ALREADY_EXISTS) return fail(code);
        }
        if (result.directory.empty()) return fail(ERROR_ALREADY_EXISTS);
        options.destination = (fs::path(result.directory) / L"User").wstring();
        if (!CreateDirectoryW(options.destination.c_str(), nullptr)) return fail(GetLastError());
        stage = CollectUser;
        result.user_complete = Export(options, &error);
        const std::wstring user_error = error;
        runtime::Event("diagnostics_export_user", {{"complete", result.user_complete}});
        stage = CollectService;
        if (include_service) {
            const auto directory = fs::path(result.directory) / L"IndexService";
            if (CreateDirectoryW(directory.c_str(), nullptr) && service_export) {
                try { result.service_complete = service_export(directory.wstring(), options.include_dumps); }
                catch (...) { Failure(CollectService, ERROR_GEN_FAILURE); }
            }
        }
        stage = Summary;
        std::ofstream summary(fs::path(result.directory) / L"feedback-summary.txt", std::ios::binary);
        summary << "Pulse diagnostic feedback package\r\n\r\nUser logs: "
            << (result.user_complete ? "exported" : "partial or unavailable")
            << "\r\nIndex service logs: " << (!include_service ? "not requested" :
                result.service_complete ? "exported" : "unavailable or administrator permission declined")
            << "\r\nCrash dumps: " << (options.include_dumps ? "explicitly included; may contain private information" : "excluded")
            << "\r\n\r\nSend Pulse-diagnostics.zip after reviewing it.\r\n"
               "User/summary.txt and User/support-report.json, when available, describe failures and missing evidence.\r\n"
               "An exported file set does not guarantee every process or event was captured.\r\n";
        if (!result.user_complete) {
            std::vector<uint8_t> text;
            if (EncodeUtf8Bytes(user_error, text)) {
                summary << "User export error: ";
                summary.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size()));
                summary << "\r\n";
            }
        }
        summary.close();
        if (!summary) { error.clear(); return fail(ERROR_WRITE_FAULT); }
        runtime::Event("diagnostics_export_collected", {{"user_ok", result.user_complete}, {"service_ok", result.service_complete}});
        stage = Archive;
        DWORD archive_error = ERROR_SUCCESS;
        const bool archived = CreateSupportArchive(result.directory,
            (fs::path(result.directory) / L"Pulse-diagnostics.zip").wstring(), &error, &archive_error);
        runtime::Event("diagnostics_export_archive", {{"ok", archived}});
        if (!archived) return fail(archive_error);
        if (!result.user_complete) {
            error = user_error.empty() ? l10n::Get(l10n::StringId::TheDiagnosticPackageIsIncompleteExportedFilesRemainInTheSele)
                                       : user_error;
            error += L"\n" + result.directory;
            return false;
        }
        error.clear();
        return true;
    } catch (const fs::filesystem_error& exception) {
        error.clear();
        return fail(static_cast<DWORD>(exception.code().value()));
    } catch (const std::bad_alloc&) {
        error.clear();
        return fail(ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        error.clear();
        return fail(ERROR_GEN_FAILURE);
    }
}
}
