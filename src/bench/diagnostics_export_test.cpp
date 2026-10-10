#include "../common/diagnostics_exporter.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
int failures = 0;

void Check(bool value, const char* name) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}

void Write(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file) throw std::runtime_error("Could not create test fixture");
}

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool Export(const fs::path& source, const fs::path& destination, std::wstring* error = nullptr) {
    fs::create_directories(destination);
    pulse::diagnostics::ExportOptions options;
    options.source_root = source.wstring();
    options.destination = destination.wstring();
    return pulse::diagnostics::Export(options, error);
}

bool Symlink(const fs::path& link, const fs::path& target, bool directory) {
    DWORD flags = (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | 2;
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
        flags &= ~2u;
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), flags)) return true;
        error = GetLastError();
    }
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED) {
        std::printf("[SKIP] Symlink creation unavailable (Win32 %lu)\n", error);
    } else {
        std::printf("[FAIL] Symlink fixture creation (Win32 %lu)\n", error);
        ++failures;
    }
    return false;
}

void Run(const fs::path& root) {
    const auto source = root / L"source";
    const auto runtime = source / L"Diagnostics" / L"Runtime";
    fs::create_directories(runtime);
    Write(runtime / L"app-101.jsonl", "current\n");
    Write(runtime / L"app-101.jsonl.1", "rotated\n");
    Write(runtime / L"index-102.jsonl", "index\n");
    Write(runtime / L"index-102.jsonl.1", "index-old\n");
    Write(runtime / L"index-service-102.jsonl", "service\n");
    Write(runtime / L"index-helper-102.jsonl.1", "helper\n");
    Write(runtime / L"private.jsonl", "private");
    Write(runtime / L"app-not-a-pid.jsonl", "private");
    Write(runtime / L"app-101.jsonl.2", "private");
    Write(source / L"preferences.json", "private");
    Write(source / L"index.db", "private");
    Write(source / L"pulse_shell_host.log", "legacy\n");
    fs::create_directories(source / L"Diagnostics" / L"Crashes");
    Write(source / L"Diagnostics" / L"Crashes" / L"report.json", "{}");
    Write(source / L"Diagnostics" / L"Crashes" / L"report.dmp", "dump");

    const auto good = root / L"good";
    Check(Export(source, good), "export current, rotated, index, legacy and crash logs");
    Check(Read(good / L"Runtime" / L"app-101.jsonl") == "current\n" &&
          Read(good / L"Runtime" / L"app-101.jsonl.1") == "rotated\n" &&
          Read(good / L"Runtime" / L"index-102.jsonl.1") == "index-old\n",
          "rotation contents preserved");
    Check(Read(good / L"Runtime" / L"index-service-102.jsonl") == "service\n" &&
          Read(good / L"Runtime" / L"index-helper-102.jsonl.1") == "helper\n",
          "service and helper component names included");
    Check(!fs::exists(good / L"Runtime" / L"private.jsonl") &&
          !fs::exists(good / L"Runtime" / L"app-not-a-pid.jsonl") &&
          !fs::exists(good / L"Runtime" / L"app-101.jsonl.2") &&
          !fs::exists(good / L"preferences.json") && !fs::exists(good / L"index.db"),
          "allowlist excludes unrelated files");
    const auto manifest = Read(good / L"diagnostics-manifest.json");
    Check(manifest.find("\"schema\":2") != std::string::npos &&
          manifest.find("\"version\":") != std::string::npos &&
          manifest.find("\"build_id\":") != std::string::npos &&
          manifest.find("\"complete\":true") != std::string::npos &&
          manifest.find("\"name\":\"Runtime/app-101.jsonl\",\"bytes\":8,\"error_code\":0") != std::string::npos,
          "manifest lists filenames, actual byte counts, version and status");
    Check(manifest.find("\"status\":\"missing\"") != std::string::npos,
          "missing optional legacy logs recorded");
    Check(fs::exists(good / L"Crashes" / L"report.dmp"),
          "existing default dump inclusion preserved");
    std::wstring error;
    Check(!Export(source, good, &error) && !error.empty() &&
          Read(good / L"diagnostics-manifest.json") == manifest,
          "nonempty destination rejected without modifying previous export");

    const auto empty = root / L"empty-source";
    fs::create_directories(empty);
    Check(Export(empty, root / L"empty-export") &&
          Read(root / L"empty-export" / L"diagnostics-manifest.json").find("\"complete\":true") != std::string::npos,
          "no diagnostics is valid and produces a manifest");

    const auto live = runtime / L"app-103.jsonl";
    HANDLE writer = CreateFileW(live.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "open active writer fixture");
    if (writer != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(writer, "live\n", 5, &written, nullptr);
        FlushFileBuffers(writer);
        Check(Export(source, root / L"live-export") &&
              Read(root / L"live-export" / L"Runtime" / L"app-103.jsonl") == "live\n",
              "shared-read copy exports log held open by writer");
        CloseHandle(writer);
    }

    HANDLE blocked = CreateFileW((source / L"pulse_shell_host.log").c_str(), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(blocked != INVALID_HANDLE_VALUE, "open exclusive file fixture");
    if (blocked != INVALID_HANDLE_VALUE) {
        const auto partial = root / L"partial";
        Check(!Export(source, partial, &error), "copy failure reports unsuccessful export");
        const auto partial_manifest = Read(partial / L"diagnostics-manifest.json");
        Check(partial_manifest.find("\"name\":\"pulse_shell_host.log\",\"bytes\":0,\"error_code\":32") != std::string::npos &&
              partial_manifest.find("\"complete\":false") != std::string::npos &&
              fs::exists(partial / L"Runtime" / L"app-101.jsonl") && !error.empty(),
              "partial failure preserves manifest error and continues other files");
        CloseHandle(blocked);
    }

    const auto large = runtime / L"app-104.jsonl";
    HANDLE oversized = CreateFileW(large.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(oversized != INVALID_HANDLE_VALUE, "open oversized fixture");
    if (oversized != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        size.QuadPart = 64ll * 1024 * 1024 + 1;
        const bool resized = SetFilePointerEx(oversized, size, nullptr, FILE_BEGIN) && SetEndOfFile(oversized);
        CloseHandle(oversized);
        Check(resized, "create oversized log");
        Check(!Export(source, root / L"oversized") &&
              !fs::exists(root / L"oversized" / L"Runtime" / L"app-104.jsonl") &&
              Read(root / L"oversized" / L"diagnostics-manifest.json").find("\"error_code\":223") != std::string::npos,
              "file size limit rejects file and records error");
        fs::remove(large);
    }

    const auto symlink_file = runtime / L"app-105.jsonl";
    if (Symlink(symlink_file, source / L"preferences.json", false)) {
        Check(!Export(source, root / L"linked-file") &&
              !fs::exists(root / L"linked-file" / L"Runtime" / L"app-105.jsonl"),
              "reparse log file cannot export target contents");
        fs::remove(symlink_file);
    }
    const auto linked_source = root / L"linked-source";
    fs::create_directories(linked_source / L"Diagnostics");
    if (Symlink(linked_source / L"Diagnostics" / L"Runtime", runtime, true)) {
        Check(!Export(linked_source, root / L"linked-directory") &&
              !fs::exists(root / L"linked-directory" / L"Runtime"),
              "reparse runtime directory rejected");
        fs::remove(linked_source / L"Diagnostics" / L"Runtime");
    }
    const auto linked_parent = root / L"linked-parent";
    if (Symlink(linked_parent, source, true)) {
        Check(!Export(linked_parent, root / L"linked-root"), "reparse source root rejected");
        fs::remove(linked_parent);
    }

    const auto prefixed = fs::path(L"\\\\?\\" + source.wstring());
    Check(Export(prefixed, root / L"extended-path"), "extended-length path prefix supported");

    const auto many = root / L"many-source" / L"Diagnostics" / L"Runtime";
    fs::create_directories(many);
    for (int i = 0; i < 260; ++i) Write(many / (L"app-" + std::to_wstring(i) + L".jsonl"), "x");
    Check(!Export(root / L"many-source", root / L"many-export") &&
          Read(root / L"many-export" / L"diagnostics-manifest.json").find("\"error_code\":4") != std::string::npos,
          "file-count cap is explicit in manifest");

    const auto support_source = root / L"support-source";
    const auto support_runtime = support_source / L"Diagnostics" / L"Runtime";
    fs::create_directories(support_runtime);
    auto record = [](const char* utc, const char* component, const char* event, int error, int severity,
                     const char* extra = "") {
        return std::string("{\"schema\":1,\"utc\":\"") + utc + "\",\"tick_ms\":1,\"session\":1,\"pid\":7,\"tid\":1,\"seq\":1,"
            "\"component\":\"" + component + "\",\"version\":\"1\",\"build\":\"fixture\",\"event\":\"" + event +
            "\",\"severity\":" + std::to_string(severity) + extra + ",\"data\":{\"error\":" + std::to_string(error) + "}}\n";
    };
    std::string noisy;
    for (int i = 0; i < 300; ++i)
        noisy += record("2026-10-09T04:00:00.000Z", "index-service", "index_write_invalid_hierarchy", 13, 2);
    Write(support_runtime / L"index-service-7.jsonl", noisy);
    Write(support_runtime / L"index-service-7.critical.jsonl",
        record("2026-10-09T03:00:00.000Z", "index-service", "index_write_invalid_hierarchy", 13, 2,
               ",\"evidence\":\"first\",\"occurrences\":900") +
        record("2026-10-09T04:00:00.000Z", "index-service", "index_write_invalid_hierarchy", 13, 2,
               ",\"evidence\":\"last\",\"occurrences\":900"));
    Write(support_runtime / L"app-8.jsonl",
        record("2026-10-09T05:00:00.000Z", "app", "preview_client_failure", 109, 2) +
        record("2026-10-09T05:30:00.000Z", "app", "navigation_end", 0, 0) +
        record("2026-10-09T05:40:00.000Z", "app", "preview_client_cancelled", 1223, 0));
    const auto support = root / L"support-export";
    fs::create_directories(support);
    pulse::diagnostics::ExportOptions support_options;
    support_options.source_root = support_source.wstring();
    support_options.destination = support.wstring();
    support_options.support_report = true;
    support_options.include_dumps = false;
    std::wstring support_error;
    Check(pulse::diagnostics::Export(support_options, &support_error), "support export with runtime failures succeeds");
    const auto report = Read(support / L"support-report.json");
    const auto newest = report.find("\"event\":\"preview_client_failure\"");
    const auto repeated = report.find("\"event\":\"index_write_invalid_hierarchy\"");
    Check(newest != std::string::npos && repeated != std::string::npos && newest < repeated,
          "support report lists failure patterns newest first");
    Check(repeated != std::string::npos &&
          report.find("\"event\":\"index_write_invalid_hierarchy\"", repeated + 1) == std::string::npos,
          "a repeating failure collapses into one pattern");
    Check(report.find("{\"occurrences\":900,\"last\":") != std::string::npos &&
          report.find("{\"occurrences\":1,\"last\":") != std::string::npos,
          "pattern counts use aggregated critical occurrences");
    Check(report.find("\"failure_patterns\":2") != std::string::npos &&
          report.find("navigation_end") == std::string::npos && report.find("preview_client_cancelled") == std::string::npos,
          "successful and cancelled events are not failure patterns");
    Check(report.find("\"schema\":2") == 1, "support report schema is versioned for the pattern format");
    const auto summary = Read(support / L"summary.txt");
    Check(summary.find("Failure patterns: 2") != std::string::npos &&
          summary.find("app/preview_client_failure  error=109  x1") != std::string::npos &&
          summary.find("index-service/index_write_invalid_hierarchy  error=13  x900") != std::string::npos,
          "summary names the newest failure patterns");
}
} // namespace

int wmain() {
    const auto workspace = std::filesystem::canonical(std::filesystem::current_path());
    const auto fixtures = workspace / L"bench_data";
    std::filesystem::create_directories(fixtures);
    if (std::filesystem::canonical(fixtures).parent_path() != workspace) {
        std::printf("[FAIL] Fixture folder resolves outside the workspace\n");
        return 1;
    }
    const auto root = fixtures /
        (L"diagnostics-export-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) {
        std::printf("[FAIL] Fixture directory already exists\n");
        return 1;
    }
    try {
        Run(root);
    } catch (const std::exception& error) {
        std::printf("[FAIL] Exception: %s\n", error.what());
        ++failures;
    }
    std::error_code cleanup_error;
    if (std::filesystem::canonical(root).parent_path() == std::filesystem::canonical(fixtures))
        std::filesystem::remove_all(root, cleanup_error);
    else
        cleanup_error = std::make_error_code(std::errc::permission_denied);
    Check(!cleanup_error, "isolated fixture cleanup");
    std::printf("Diagnostics export tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
