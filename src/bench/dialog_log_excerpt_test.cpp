#include "../common/dialog_log_excerpt.h"
#include "../common/json_value.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
unsigned checks = 0, failures = 0;
void Check(bool result, const char* name) {
    ++checks;
    if (!result) ++failures;
    std::cout << (result ? "[PASS] " : "[FAIL] ") << name << '\n';
}
struct Fixture {
    fs::path root;
    std::vector<fs::path> files;
    explicit Fixture(unsigned id) {
        root = fs::current_path() / (L"dialog-log-excerpt-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id));
        if (!CreateDirectoryW(root.c_str(), nullptr)) throw std::runtime_error("exclusive fixture directory failed");
    }
    ~Fixture() {
        for (auto file = files.rbegin(); file != files.rend(); ++file) DeleteFileW(file->c_str());
        RemoveDirectoryW(root.c_str());
    }
    fs::path Write(const wchar_t* name, const std::string& text) {
        const auto path = root / name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("exclusive fixture file failed");
        files.push_back(path);
        DWORD written = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        CloseHandle(file);
        if (!ok) throw std::runtime_error("fixture write failed");
        return path;
    }
    std::string Collect(const std::function<bool()>& cancelled = {}) const {
        return pulse::dialog_diagnostics::CollectDialogLogExcerpt(root.wstring(), cancelled);
    }
};
std::string Auto(std::string event = "token_rejected", std::string data = "\"reason\":5,\"reason_name\":\"ELEVATED_BUNDLE_UNTRUSTED\",\"stage\":\"activation\"", std::string session = "134388388389325565", unsigned seq = 1) {
    return "{\"schema\":1,\"utc\":\"2026-11-10T12:34:56.789Z\",\"session\":" + session +
        ",\"pid\":42,\"tid\":43,\"seq\":" + std::to_string(seq) + ",\"component\":\"dialog_auto\",\"event\":\"" + event + "\",\"data\":{" + data + "}}\n";
}
pulse::json::Value Summary(const std::string& text) {
    const auto end = text.size() - 1;
    const auto previous = end ? text.rfind('\n', end - 1) : std::string::npos;
    pulse::json::Value value;
    if (!pulse::json::Parse(std::string_view(text).substr(previous == std::string::npos ? 0 : previous + 1), value)) throw std::runtime_error("summary JSON invalid");
    return value;
}
bool Counter(const std::string& text, const char* name, uint64_t expected) {
    const auto summary = Summary(text);
    const auto* value = summary.find(name);
    return value && value->as_number(-1) == static_cast<double>(expected);
}
bool Flag(const std::string& text, const char* name) { return Summary(text).get_bool(name); }
bool Contains(const std::string& text, const char* value) { return text.find(value) != std::string::npos; }
void Privacy() {
    Fixture f(1);
    f.Write(L"dialog_auto-42.jsonl", Auto("token_rejected", "\"reason\":5,\"reason_name\":\"ELEVATED_BUNDLE_UNTRUSTED\",\"stage\":\"activation\",\"detail\":\"PRIVATE_SENTINEL\",\"path\":\"C:\\\\secret\",\"filename\":\"PRIVATE_SENTINEL\",\"commandline\":\"PRIVATE_SENTINEL\",\"unknown\":\"PRIVATE_SENTINEL\",\"process_arch_bits\":64,\"process_elevated\":1") +
        Auto("activation_gate", "\"token_safe\":0,\"reason_name\":\"PRIVATE_SENTINEL\",\"stage\":\"activation\"", "18446744073709551615", 2) +
        Auto("dialog_identity", "\"app_image_id\":18446744073709551614,\"process_arch\":64,\"diagnostics_revision\":51", "134388388389325565", 3));
    const auto out = f.Collect();
    Check(Counter(out, "records_emitted", 3), "recognized diagnostic events");
    Check(Contains(out, "\"app_image_id\":18446744073709551614"), "software identity raw uint64 exact without snapshot join");
    Check(Contains(out, "134388388389325565") && Contains(out, "18446744073709551615"), "uint64 session exact beyond double precision");
    Check(!Contains(out, "PRIVATE_SENTINEL") && !Contains(out, "secret") && !Contains(out, "commandline") && !Contains(out, "filename"), "private and unknown strings removed");
    Check(Contains(out, "ELEVATED_BUNDLE_UNTRUSTED") && Contains(out, "\"process_elevated\":true") && Contains(out, "\"token_safe\":false"), "fixed reason and numeric booleans retained");
    Check(out.find("\"seq\":1") < out.find("\"seq\":2"), "original order within each file");
    Check(Flag(out, "no_entry_inference") && !Flag(out, "process_snapshot_joined"), "summary forbids missing-entry and snapshot inference");
    Check(!Flag(out, "incomplete"), "field redaction alone is not collection failure");
}
void Filtering() {
    Fixture f(2);
    auto wrong_component = Auto();
    wrong_component.replace(wrong_component.find("dialog_auto"), 11, "other_stuff");
    auto escaped_session = Auto();
    escaped_session.replace(escaped_session.find("\"session\""), 9, "\"sess\\u0069on\"");
    f.Write(L"dialog_auto-42.jsonl", Auto("bridge_trace", "\"detail\":\"PRIVATE_SENTINEL\"") + wrong_component + Auto("token_rejected", "", "1e17") + Auto("token_rejected", "", "18446744073709551616") + escaped_session + "not JSON\n");
    f.Write(L"other.jsonl", Auto());
    f.Write(L"runtime-decisions.tsv", "PRIVATE_SENTINEL\n");
    const auto out = f.Collect();
    Check(Counter(out, "records_emitted", 0) && Counter(out, "invalid_lines", 5), "invalid session component and malformed records rejected");
    Check(Counter(out, "non_diagnostic_lines", 1) && Counter(out, "files_read", 1), "event and filename whitelist");
    Check(Flag(out, "incomplete") && !Contains(out, "PRIVATE_SENTINEL"), "invalid input reported without raw input");
}
void LaunchPreflight() {
    Fixture f(77);
    const std::string line = R"({"schema":1,"session":"{12345678-1234-1234-1234-123456789ABC}","pid":42,"event":"target_preflight","data":{"existing_target_count":1,"scan_complete":0,"query_errors":2,"vanished_processes":1,"cancelled":0,"warning":"existing_target","existing_target_managed":"unknown","certified":0,"path":"PRIVATE_SENTINEL","commandline":"PRIVATE_SENTINEL"}})";
    f.Write(L"launcher-preflight.jsonl", line + "\n");
    const auto out = f.Collect();
    Check(Counter(out, "records_emitted", 1), "launcher preflight retained in managed diagnostic export");
    Check(Contains(out, "managed_launcher") && Contains(out, "existing_target_count") && Contains(out, "scan_complete"), "launcher counters are separate from dialog shows");
    Check(Contains(out, "\"existing_target_managed\":\"unknown\"") && Contains(out, "\"certified\":false"), "launcher never certifies old process takeover");
    Check(!Contains(out, "PRIVATE_SENTINEL") && !Contains(out, "commandline"), "launcher private paths and commands stripped");
}
void Managed() {
    Fixture f(3);
    const std::string begin = "{\"session\":\"42-123456789\",\"pid\":42,\"event\":\"dialog_show_begin\",\"data\":{\"request\":\"7:1\",\"dialog\":7,\"show_sequence\":1,\"entry_family\":\"OFN_W\",\"launch_route\":\"managed\",\"process_arch\":64,\"detail\":\"PRIVATE_SENTINEL\"}}\n";
    const std::string terminal = "{\"session\":\"42-123456789\",\"pid\":42,\"event\":\"dialog_show_terminal\",\"data\":{\"request\":\"7:1\",\"terminal_status\":\"cancelled\",\"hr\":2147943623,\"provider_started\":true,\"ui_shown_known\":true,\"ui_shown\":false,\"reason_name\":\"None\",\"fallback_reason\":\"none\"}}\n";
    auto mismatch = begin;
    mismatch.replace(mismatch.find("42-"), 3, "99-");
    f.Write(L"runtime-dialog-shows.jsonl", begin + terminal + mismatch);
    const auto out = f.Collect();
    Check(Counter(out, "records_emitted", 2) && Counter(out, "invalid_lines", 1), "managed show records require own pid-session grammar");
    Check(Contains(out, "\"request\":\"7:1\"") && Contains(out, "\"terminal_status\":\"cancelled\"") && !Contains(out, "PRIVATE_SENTINEL"), "managed fixed request and terminal fields retained");
    Check(!Contains(out, "\"utc\""), "managed record does not invent UTC");
}
void Limits() {
    Fixture f(4);
    f.Write(L"dialog_auto-42.jsonl", std::string(270000, 'x') + "\n" + std::string(9000, 'x') + "\n" + Auto() + "{\"unfinished\":");
    const auto out = f.Collect();
    Check(Counter(out, "records_emitted", 1) && Counter(out, "tail_files", 1), "bounded tail retains recent complete record");
    Check(Counter(out, "partial_lines_discarded", 2) && Counter(out, "oversized_lines", 1) && Flag(out, "incomplete"), "tail first fragment last partial and long line explicit");
    Check(out.size() <= 128 * 1024 && Counter(out, "bytes_read", 256 * 1024), "tail and output byte limits");
    Fixture many(5);
    std::string lines;
    for (unsigned i = 0; i < 300; ++i) lines += Auto("token_rejected", "\"reason\":5", "134388388389325565", i + 1);
    many.Write(L"dialog_auto-42.jsonl", lines);
    const auto capped = many.Collect();
    Check(Counter(capped, "records_emitted", 256) && Flag(capped, "output_limited"), "record cap explicit");
    Check(!Contains(capped, "\"seq\":44,") && Contains(capped, "\"seq\":45,") && capped.find("\"seq\":45,") < capped.find("\"seq\":300,"), "latest records selected then original order restored");
    Fixture files(6);
    for (unsigned i = 1; i <= 10; ++i) {
        const auto path = files.Write((L"dialog_auto-" + std::to_wstring(i) + L".jsonl").c_str(), Auto("token_rejected", "\"reason\":5", std::to_string(i)));
        HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FILETIME time{i * 100000u, 30000000};
        const bool ok = handle != INVALID_HANDLE_VALUE && SetFileTime(handle, nullptr, nullptr, &time);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!ok) throw std::runtime_error("fixture timestamp failed");
    }
    const auto recent = files.Collect();
    Check(Counter(recent, "files_read", 8) && Flag(recent, "files_limited") && recent.find("\"session\":10,") < recent.find("\"session\":3,"), "newest eight files only");
}
void Errors() {
    Fixture f(7);
    f.Write(L"dialog_auto-42.jsonl", Auto());
    const auto early = f.Collect([] { return true; });
    Check(Flag(early, "cancelled") && Flag(early, "incomplete") && Counter(early, "files_read", 0), "immediate cancellation");
    const auto callback = f.Collect([]() -> bool { throw std::runtime_error("PRIVATE_SENTINEL"); });
    Check(Flag(callback, "callback_error") && !Contains(callback, "PRIVATE_SENTINEL"), "callback error redacted");
    const auto empty = pulse::dialog_diagnostics::CollectDialogLogExcerpt(L"");
    const auto relative = pulse::dialog_diagnostics::CollectDialogLogExcerpt(L".");
    const auto missing = pulse::dialog_diagnostics::CollectDialogLogExcerpt((f.root / L"absent").wstring());
    Check(Counter(empty, "directory_error", ERROR_INVALID_NAME) && Counter(relative, "directory_error", ERROR_INVALID_NAME), "empty and relative path never scan cwd");
    Check(Flag(missing, "incomplete") && Counter(missing, "records_emitted", 0), "missing directory error summary");
    const auto source = f.Write(L"source.jsonl", Auto());
    const auto link = f.root / L"dialog_auto-99.jsonl";
    const bool linked = CreateHardLinkW(link.c_str(), source.c_str(), nullptr) != FALSE;
    if (linked) f.files.push_back(link);
    Check(linked, "hardlink fixture created");
    if (linked) Check(Counter(f.Collect(), "rejected_files", 1), "multiply linked file rejected");
    const auto large = f.Write(L"dialog_auto-100.jsonl", "");
    HANDLE handle = CreateFileW(large.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{}; size.QuadPart = 65 * 1024 * 1024;
    const bool sized = handle != INVALID_HANDLE_VALUE && SetFilePointerEx(handle, size, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    Check(sized && Counter(f.Collect(), "oversized_files", 1), "oversized file excluded before read");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--preflight-only") { LaunchPreflight(); return failures ? 1 : 0; }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--collect") {
        std::cout << pulse::dialog_diagnostics::CollectDialogLogExcerpt(argv[2]);
        return 0;
    }
    if (argc != 1) return 2;
    try { Privacy(); Filtering(); Managed(); LaunchPreflight(); Limits(); Errors(); }
    catch (const std::exception& error) { ++failures; std::cout << "[FAIL] fixture exception: " << error.what() << '\n'; }
    std::cout << "DIALOG_LOG_EXCERPT " << checks << " checks " << failures << " failures\n";
    return failures ? 1 : 0;
}
