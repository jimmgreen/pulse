#include "diagnostics_report.h"
#include "diagnostics_exporter.h"
#include "json_utils.h"
#include "utf8_file.h"
#include "pulse_version.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace pulse::diagnostics {
namespace {
namespace fs = std::filesystem;
constexpr size_t kReadLimit = 64 * 1024 * 1024;
std::string Narrow(std::wstring_view s) {
    std::string out; out.reserve(s.size());
    for (wchar_t c : s) out += static_cast<char>(c);
    return out;
}
bool Token(std::wstring_view s, size_t limit = 80) {
    return !s.empty() && s.size() <= limit && std::all_of(s.begin(), s.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L'.' || c == L':' || c == L'+';
    });
}
std::wstring Wide(std::string_view bytes) {
    std::wstring result;
    DecodeUtf8Bytes(std::vector<uint8_t>(bytes.begin(), bytes.end()), result);
    return result;
}
std::string Number(const std::wstring& object, const wchar_t* key, std::string fallback = "null") {
    size_t p = json::ValuePosition(object, key);
    if (p == std::wstring::npos) return fallback;
    size_t end = p;
    if (end < object.size() && object[end] == L'-') ++end;
    const size_t digits = end;
    while (end < object.size() && object[end] >= L'0' && object[end] <= L'9') ++end;
    if (end == digits || end - digits > 20 || (end < object.size() && object[end] != L',' && object[end] != L'}' && !iswspace(object[end]))) return fallback;
    return Narrow(std::wstring_view(object).substr(p, end - p));
}
std::string NumericObject(const std::wstring& object) {
    std::string out = "{";
    size_t p = 1;
    while (p < object.size()) {
        json::SkipWhitespace(object, p);
        if (object[p] == L'}') break;
        if (object[p++] != L'"') break;
        std::wstring key;
        if (!json::DecodeString(object, p, key)) break;
        json::SkipWhitespace(object, p);
        if (p == object.size() || object[p++] != L':') break;
        json::SkipWhitespace(object, p);
        const size_t begin = p;
        int depth = 0; bool quoted = false, escape = false;
        for (; p < object.size(); ++p) {
            const wchar_t c = object[p];
            if (quoted) { if (escape) escape = false; else if (c == L'\\') escape = true; else if (c == L'"') quoted = false; continue; }
            if (c == L'"') quoted = true;
            else if (c == L'{' || c == L'[') ++depth;
            else if (c == L'}' || c == L']') { if (!depth) break; --depth; }
            else if (c == L',' && !depth) break;
        }
        std::wstring value = object.substr(begin, p - begin);
        while (!value.empty() && iswspace(value.back())) value.pop_back();
        std::string numeric = Number(L"{\"v\":" + value + L"}", L"v");
        if (value == L"true" || value == L"false") numeric = Narrow(value);
        if (Token(key, 48) && numeric != "null") {
            if (out.size() > 1) out += ',';
            out += "\"" + Narrow(key) + "\":" + numeric;
        }
        if (p == object.size() || object[p] == L'}') break;
        ++p;
    }
    return out + '}';
}
bool ReadPlain(const fs::path& path, std::string& out, DWORD& error) {
    out.clear(); error = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    bool ok = GetFileInformationByHandle(file, &info) && GetFileSizeEx(file, &size);
    if (!ok) error = GetLastError();
    else if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || size.QuadPart < 0 || size.QuadPart > kReadLimit) { ok = false; error = ERROR_INVALID_DATA; }
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart)); DWORD got = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
        if (!ok) error = GetLastError() ? GetLastError() : ERROR_READ_FAULT;
    }
    CloseHandle(file); return ok;
}
bool WriteNew(const fs::path& path, std::string_view value) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0; const bool ok = WriteFile(f, value.data(), static_cast<DWORD>(value.size()), &n, nullptr) && n == value.size();
    CloseHandle(f); return ok;
}
std::string Hash(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    if (ok) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    const char* hex = "0123456789abcdef"; std::string result;
    for (auto v : digest) { result += hex[v >> 4]; result += hex[v & 15]; }
    return result;
}
std::string Version(const fs::path& path) {
    DWORD unused = 0, size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!size || size > 1024 * 1024) return {};
    std::vector<BYTE> bytes(size); VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data()) ||
        !VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) || length < sizeof(*info)) return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + '.' + std::to_string(LOWORD(info->dwFileVersionMS)) + '.' + std::to_string(HIWORD(info->dwFileVersionLS)) + '.' + std::to_string(LOWORD(info->dwFileVersionLS));
}
std::string Service() {
    DWORD error = 0; SERVICE_STATUS_PROCESS status{}; DWORD got = 0;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE service = manager ? OpenServiceW(manager, L"PulseIndex", SERVICE_QUERY_STATUS) : nullptr;
    if (!service || !QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &got)) error = GetLastError();
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    return "{\"query_error\":" + std::to_string(error) + ",\"state\":" + (error ? "null" : std::to_string(status.dwCurrentState)) + ",\"pid\":" + (error ? "null" : std::to_string(status.dwProcessId)) + ",\"exit_code\":" + (error ? "null" : std::to_string(status.dwWin32ExitCode)) + '}';
}
std::string Drives() {
    std::string out = "["; const DWORD mask = GetLogicalDrives();
    for (unsigned i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = L"A:\\"; root[0] += static_cast<wchar_t>(i);
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER available{}, total{}, free{}; wchar_t fs_name[32]{};
        const bool space_ok = GetDiskFreeSpaceExW(root, &available, &total, &free) != FALSE;
        const DWORD space_error = space_ok ? 0 : GetLastError();
        const bool fs_ok = GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs_name, 32) != FALSE;
        if (out.size() > 1) out += ',';
        out += "{\"drive\":" + std::to_string(root[0]) + ",\"space_error\":" + std::to_string(space_error) +
            ",\"total_bytes\":" + (space_ok ? std::to_string(total.QuadPart) : "null") + ",\"free_bytes\":" + (space_ok ? std::to_string(free.QuadPart) : "null") +
            ",\"ntfs\":" + (fs_ok ? (wcscmp(fs_name, L"NTFS") == 0 ? std::string("true") : std::string("false")) : std::string("null")) + '}';
    }
    return out + ']';
}
}

std::string SanitizeRuntime(std::string_view text, uint64_t& rejected) {
    std::string out; size_t p = 0;
    while (p < text.size()) {
        const size_t end = text.find('\n', p); const size_t stop = end == text.npos ? text.size() : end;
        const auto line = text.substr(p, stop - p); p = end == text.npos ? text.size() : end + 1;
        if (line.empty()) continue;
        if (line.size() > 32768) { ++rejected; continue; }
        const auto w = Wide(line);
        const auto event = json::ExtractString(w, L"event");
        if (!json::ConfigSyntax(w, false).Object() || !Token(event)) { ++rejected; continue; }
        std::string record = "{\"event\":\"" + Narrow(event) + "\"";
        for (const wchar_t* key : {L"utc", L"component", L"version", L"build", L"level", L"evidence"}) {
            const auto value = json::ExtractString(w, key);
            if (Token(value)) record += ",\"" + Narrow(key) + "\":\"" + Narrow(value) + "\"";
        }
        for (const wchar_t* key : {L"schema", L"tick_ms", L"session", L"pid", L"tid", L"seq", L"occurrences", L"severity"}) {
            const auto value = Number(w, key);
            if (value != "null") record += ",\"" + Narrow(key) + "\":" + value;
        }
        const auto data = json::ExtractObject(w, L"data");
        record += ",\"data\":" + (data.empty() ? "{}" : NumericObject(data)) + "}\n";
        out += record;
    }
    return out;
}

bool WriteSupportReport(const ExportOptions& options, bool copied, bool flushed, const runtime::Health& health, std::wstring* error) {
    try {
        std::string report = "{\"schema\":2,\"version\":\"" PULSE_VERSION_STRING_A "\",\"copy_complete\":" + std::string(copied ? "true" : "false") +
            ",\"live_logger_scope\":\"exporting_process_only\",\"flush_completed\":" + (flushed ? "true" : "false") +
            ",\"logger\":{\"enabled\":" + (health.enabled ? "true" : "false") + ",\"initialization_error\":" + std::to_string(health.initialization_error) +
            ",\"write_errors\":" + std::to_string(health.write_errors) + ",\"last_write_error\":" + std::to_string(health.last_write_error) +
            ",\"dropped_events\":" + std::to_string(health.dropped_events) + ",\"pending_events\":" + std::to_string(health.pending_events) + "}";
        std::string contents; DWORD read_error = 0;
        const bool config_ok = !options.configuration_file.empty() && ReadPlain(options.configuration_file, contents, read_error);
        const auto config = config_ok ? Wide(contents) : std::wstring{};
        const bool config_valid = config_ok && json::ConfigSyntax(config, false).Object();
        report += ",\"configuration\":{\"read_error\":" + std::to_string(read_error) + ",\"available\":" + (config_valid ? "true" : "false");
        if (config_valid) {
            report += ",\"version\":" + Number(config, L"version") + ",\"generation\":" + Number(config, L"generation");
            report += ",\"excluded_volume_count\":" + std::to_string(json::ExtractStringArray(config, L"excluded_volume_ids").size());
            report += ",\"excluded_path_count\":" + std::to_string(json::ExtractStringArray(config, L"excluded_paths").size());
            const auto path = json::ExtractString(config, L"index_path");
            report += ",\"index_path_chars\":" + std::to_string(path.size()) + ",\"index_drive\":" + (path.size() > 2 && path[1] == L':' ? std::to_string(towupper(path[0])) : "null");
        }
        report += "},\"service\":" + (options.collect_environment ? Service() : "{\"not_collected\":true}");
        report += ",\"fixed_drives\":" + (options.collect_environment ? Drives() : "[]");
        report += ",\"binaries\":["; bool first = true;
        if (!options.install_directory.empty()) for (const wchar_t* name : {L"pulse.exe", L"Pulse.Index.exe", L"Pulse.Preview.exe", L"Pulse.Document.exe", L"pulse_shell.exe"}) {
            const auto path = fs::path(options.install_directory) / name;
            const bool got = ReadPlain(path, contents, read_error);
            if (!first) report += ','; first = false;
            report += "{\"name\":\"" + Narrow(name) + "\",\"read_error\":" + std::to_string(read_error) + ",\"sha256\":\"" + (got ? Hash(contents) : "") + "\",\"file_version\":\"" + (got ? Version(path) : "") + "\"}";
        }
        report += "],\"recent_failures\":[";
        // One entry per failure pattern (component, event and failure-category
        // fields), newest first. Critical sidecars contribute their aggregated
        // counts, so one repeating error cannot crowd out other modules.
        struct FailurePattern { std::string utc, line, component, event, error; uint64_t logged = 0, aggregated = 0; };
        std::vector<FailurePattern> patterns;
        std::unordered_map<std::string, size_t> pattern_index;
        size_t parsed = 0;
        const auto runtime_dir = fs::path(options.destination) / L"Runtime";
        if (fs::is_directory(runtime_dir)) for (const auto& file : fs::directory_iterator(runtime_dir)) {
            if (!ReadPlain(file.path(), contents, read_error)) continue;
            std::unordered_map<size_t, uint64_t> file_occurrences;
            std::istringstream lines(contents);
            for (std::string line; std::getline(lines, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto w = Wide(line); if (!json::ConfigSyntax(w, false).Object()) continue;
                ++parsed; const auto data = json::ExtractObject(w, L"data");
                const auto level = json::ExtractString(w, L"level");
                bool failed = level == L"error" || Number(w, L"severity", "0") == "2";
                for (const auto* key : {L"error", L"last_error", L"hresult"}) {
                    const auto value = Number(data, key, "0");
                    failed |= value != "0" && value != "995" && value != "1223";
                }
                if (!failed) continue;
                const auto component = Narrow(json::ExtractString(w, L"component"));
                const auto event = Narrow(json::ExtractString(w, L"event"));
                std::string key = component + '|' + event;
                for (const auto* field : {L"error", L"code", L"hr", L"hresult", L"win32_error", L"reason"})
                    key += '|' + Number(data, field, "");
                const auto utc = Narrow(json::ExtractString(w, L"utc"));
                auto [slot, inserted] = pattern_index.try_emplace(key, patterns.size());
                if (inserted) {
                    std::string failure_code = Number(data, L"error", "");
                    if (failure_code.empty()) failure_code = Number(data, L"hresult", "");
                    patterns.push_back({utc, line, component, event, failure_code});
                }
                auto& pattern = patterns[slot->second];
                if (json::ExtractString(w, L"evidence").empty()) ++pattern.logged;
                else {
                    const auto occurrences = std::strtoull(Number(w, L"occurrences", "1").c_str(), nullptr, 10);
                    auto& seen = file_occurrences[slot->second];
                    seen = (std::max)(seen, static_cast<uint64_t>(occurrences));
                }
                if (utc > pattern.utc) { pattern.utc = utc; pattern.line = line; }
            }
            for (const auto& [index, occurrences] : file_occurrences) patterns[index].aggregated += occurrences;
        }
        const size_t pattern_count = patterns.size();
        std::stable_sort(patterns.begin(), patterns.end(),
            [](const FailurePattern& a, const FailurePattern& b) { return a.utc > b.utc; });
        if (patterns.size() > 128) patterns.resize(128);
        auto occurrences_of = [](const FailurePattern& p) { return (std::max)(p.logged, p.aggregated); };
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (i) report += ',';
            report += "{\"occurrences\":" + std::to_string(occurrences_of(patterns[i])) + ",\"last\":" + patterns[i].line + "}";
        }
        report += "],\"failure_patterns\":" + std::to_string(pattern_count);
        report += ",\"runtime_records\":" + std::to_string(parsed) + ",\"evidence_available\":" + (parsed ? "true" : "false") +
            ",\"limitations\":[\"Missing events do not establish success.\",\"Other processes are not synchronously flushed.\",\"Events are bounded by retention and may be incomplete.\"]}\n";
        std::string summary = "Pulse support report\r\n\r\nFiles copied: " + std::string(copied ? "yes" : "partial") +
            "\r\nRuntime records: " + std::to_string(parsed) + "\r\nFailure patterns: " + std::to_string(pattern_count) +
            "\r\nExporter logger enabled: " + (health.enabled ? "yes" : "no") + "\r\nExporter flush completed: " + (flushed ? "yes" : "no") +
            "\r\nDropped events: " + std::to_string(health.dropped_events) + "\r\nWrite errors: " + std::to_string(health.write_errors) +
            "\r\n\r\nSee support-report.json and diagnostics-manifest.json for phases, error codes, binary hashes and missing files.\r\nA successful copy does not mean complete diagnostic coverage. Other processes may have pending events.\r\n";
        if (!parsed) summary += "WARNING: no readable runtime events were exported. Reproduce after checking logging availability.\r\n";
        if (!patterns.empty()) {
            summary += "\r\nNewest failure patterns (UTC, component/event, error, occurrences):\r\n";
            for (size_t i = 0; i < patterns.size() && i < 10; ++i) {
                const auto& p = patterns[i];
                summary += "  " + (p.utc.empty() ? std::string("?") : p.utc) + "  " + p.component + "/" + p.event +
                    "  error=" + (p.error.empty() ? std::string("-") : p.error) + "  x" + std::to_string(occurrences_of(p)) + "\r\n";
            }
        }
        const bool ok = WriteNew(fs::path(options.destination) / L"support-report.json", report) && WriteNew(fs::path(options.destination) / L"summary.txt", summary);
        if (!ok && error) *error = L"Could not write support report.";
        return ok;
    } catch (...) { if (error) *error = L"Could not collect support report."; return false; }
}
}
