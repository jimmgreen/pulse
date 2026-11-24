#include "dialog_log_excerpt.h"
#include "dialog_diagnostic_values.h"
#include "json_value.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

namespace pulse::dialog_diagnostics {
namespace {
constexpr size_t kFiles = 8, kDirectoryEntries = 512, kTail = 256 * 1024, kRead = 1024 * 1024;
constexpr size_t kLine = 8192, kLines = 4096, kRecords = 256, kOutput = 128 * 1024, kFileSize = 64 * 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Summary {
    ExcerptCounts evidence;
    uint64_t entries = 0, eligible = 0, files = 0, bytes = 0, lines = 0, records = 0, ignored = 0, invalid = 0;
    uint64_t omitted = 0, partial = 0, oversized_lines = 0, oversized_files = 0, rejected_files = 0, tails = 0, read_errors = 0;
    DWORD directory_error = 0, enumeration_error = 0, last_read_error = 0;
    bool enumeration_limited = false, files_limited = false, read_limited = false, output_limited = false;
    bool line_limited = false, cancelled = false, callback_error = false;
};
bool Cancel(const std::function<bool()>& callback, Summary& s) {
    try { if (callback && callback()) s.cancelled = true; }
    catch (...) { s.cancelled = s.callback_error = true; }
    return s.cancelled;
}
std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
// The strict existing DOM parser validates the complete JSON first. This scanner
// retrieves only canonical literal member names and their original value spans.
// In particular, uint64 session IDs must never pass through DOM's double storage.
using Members = std::vector<std::pair<std::string_view, std::string_view>>;
Members Fields(std::string_view object) {
    Members result;
    size_t p = 1;
    while (p < object.size()) {
        while (p < object.size() && (object[p] == ',' || object[p] == ' ' || object[p] == '\r' || object[p] == '\n' || object[p] == '\t')) ++p;
        if (p >= object.size() || object[p] != '"') break;
        const size_t key = ++p;
        while (p < object.size() && object[p] != '"') { if (object[p] == '\\') ++p; ++p; }
        if (p >= object.size()) break;
        const auto name = object.substr(key, p - key);
        ++p;
        while (p < object.size() && object[p] != ':') ++p;
        const size_t begin = ++p;
        unsigned depth = 0; bool quoted = false;
        for (; p < object.size(); ++p) {
            const char c = object[p];
            if (quoted) { if (c == '\\') ++p; else if (c == '"') quoted = false; continue; }
            if (c == '"') quoted = true;
            else if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (!depth) break; --depth; }
            else if (c == ',' && !depth) break;
        }
        result.emplace_back(name, Trim(object.substr(begin, p - begin)));
    }
    return result;
}
std::string_view Get(const Members& fields, std::string_view key) {
    for (const auto& field : fields) if (field.first == key) return field.second;
    return {};
}
bool Unsigned(std::string_view text, uint64_t maximum = (std::numeric_limits<uint64_t>::max)()) {
    if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != text.npos) return false;
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= maximum;
}
bool Pair(std::string_view text, char separator) {
    const auto position = text.find(separator);
    return position != text.npos && Unsigned(text.substr(0, position)) && Unsigned(text.substr(position + 1));
}
bool Word(std::string_view word, std::string_view list) {
    while (!list.empty()) {
        const auto end = list.find(' ');
        if (word == list.substr(0, end)) return true;
        if (end == list.npos) break;
        list.remove_prefix(end + 1);
    }
    return false;
}
std::string Quote(std::string_view value) { return json::Serialize(json::Value(value)); }
std::string FilterDecision(std::string_view line, Summary& s) {
    const auto token = [&line](std::string_view key) {
        if (!line.starts_with(key)) return std::string_view{};
        line.remove_prefix(key.size());
        const auto end = line.find(' ');
        const auto value = line.substr(0, end);
        line.remove_prefix(end == line.npos ? line.size() : end + 1);
        return value;
    };
    const auto entry = token("interface=");
    const auto tick = token("tick=");
    const auto reason = token("reason=");
    const auto options = token("options=0x");
    const auto category = token("category=");
    if (!line.empty() || !Word(entry,
        "runtime auto-runtime CoCreateInstance CoCreateInstanceEx CoGetClassObject GetOpenFileNameW GetSaveFileNameW "
        "GetOpenFileNameA GetSaveFileNameA SHBrowseForFolderW SHBrowseForFolderA") ||
        !Unsigned(tick) || !Unsigned(category, 0xffffffffu) || options.size() != 8 ||
        options.find_first_not_of("0123456789ABCDEFabcdef") != options.npos) {
        ++s.invalid;
        return {};
    }
    // Free-form traces and unknown values are not part of the report's schema.
    if (!Word(reason,
        "activation-adapter-entered activation-session-disabled resolve-disabled-native resolve-ready resolve-not-ready-native "
        "show-session-disabled-native native-contract-or-validation-fallback thin-companion-remote-provider "
        "inprocess-ui-proof-load legacy-adapter-declined browse-family-entered browse-session-or-token-native "
        "browse-result-hresult browse-service-construction-native adapter-return service-construction-failed-native "
        "validated-session-provider-published static-iat-slots-late-delay-dynamic-imports-native "
        "single-native-aggregation single-native-context single-native-class single-native-requested-interface "
        "single-native-apartment single-original-failed single-wrapper-published single-native-wrapper-creation "
        "class-original-failed class-native-context class-factory-wrapper-published "
        "ex-missing-original ex-native-disabled ex-native-nested ex-native-aggregation ex-native-server-info "
        "ex-native-context ex-native-result-array ex-native-class ex-native-apartment-unavailable ex-native-apartment "
        "ex-native-input-iid ex-native-input-pointer ex-native-requested-interface ex-original-failed "
        "ex-native-aggregate-result ex-native-output-state ex-native-identity-query ex-native-mixed-identities "
        "ex-native-wrapper-creation ex-native-wrapper-interface ex-wrapper-published ex-native-wrapper-exception "
        "early-wrapper-created initialization-revoked-marker-failed initialization-revoked-marker-written") &&
        !KnownDiagnosticValue("reason_name", reason) && !KnownDiagnosticValue("fallback_reason", reason)) {
        ++s.ignored;
        return {};
    }
    if (reason.empty()) { ++s.invalid; return {}; }
    return "{\"schema\":1,\"kind\":\"dialog_log_excerpt\",\"source\":\"managed_runtime_decisions\","
           "\"event\":\"runtime_decision\",\"tick_ms\":" + std::string(tick) + ",\"data\":{\"interface\":" + Quote(entry) +
           ",\"reason\":" + Quote(reason) + ",\"options_hex\":" + Quote(options) +
           ",\"category\":" + std::string(category) + "}}\n";
}
bool Utc(std::string_view value) {
    if (value.size() != 24) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char fixed = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : i == 19 ? '.' : i == 23 ? 'Z' : 0;
        if (fixed ? value[i] != fixed : value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}
constexpr std::string_view kSecurity = "reason error reason_name stage process_arch_bits process_elevated_known process_elevated process_elevation_type process_token_type elevation_type_error token_type_error admin_trust_checked admin_trust_error admin_trust_accepted elevation_type_name token_type_name";
struct Policy { std::string_view event, fields; };
constexpr Policy kPolicies[]{
    {"admin_auto_target_result", "target_pid error ready attach_stage"},
    {"target_preflight", "existing_target_count scan_complete query_errors vanished_processes cancelled timed_out scanned_processes same_session_processes warning existing_target_managed certified"},
    {"dialog_identity", "app_image_id process_arch diagnostics_revision"},
    {"token_rejected", kSecurity}, {"token_accepted", kSecurity}, {"caller_security_context", kSecurity},
    {"activation_gate", "token_safe registered conflict error path_match path_case_match reason_name stage"},
    {"resolve_gate", "dialog token_safe enabled error hr reason_name stage"},
    {"validation_gate", "token_safe enabled accepted reason_name stage"},
    {"elevated_bundle_trust", "error checked accepted reason_name stage"},
    {"activation", "enabled save diagnostics_revision"},
    {"registration_snapshot", "dialog enabled conflict error path_match"},
    {"route", "prefer_bridge remote_host"}, {"native", "reason request dialog"},
    {"provider_load", ""}, {"provider_cache", "reused"}, {"provider", "hr deferred remote_fallback"},
    {"remote_provider_load", ""}, {"remote_provider_cache", "reused"}, {"remote_provider", "hr features"},
    {"contract_decision", "reason_name request dialog hr options_known call"},
    {"observe_contract", "reason disposition call options native_options native shown subscriptions"},
    {"contract", "accepted category"}, {"pulse_show", "request"}, {"pulse_result", "request status hr flags count"},
    {"native_result", "hr request"},
    {"dialog_show_begin", "request dialog show_sequence process_arch entry_family launch_route dialog_kind"},
    {"dialog_show_terminal", "request dialog show_sequence process_arch hr native_fallback provider_started ui_shown_known ui_shown entry_family launch_route dialog_kind terminal_status reason_name fallback_reason"}
};
bool Enum(std::string_view key, std::string_view value) {
    if (key == "warning") return Word(value, "existing_target scan_incomplete none");
    if (key == "existing_target_managed") return value == "unknown";
    if (key == "stage") return Word(value, "initialize activation resolve validation");
    if (key == "elevation_type_name") return Word(value, "default full limited unknown");
    if (key == "token_type_name") return Word(value, "primary impersonation unknown");
    return KnownDiagnosticValue(key, value);
}
bool Guid(std::string_view value) {
    if (value.size() != 38 || value.front() != '{' || value.back() != '}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (value[i] != '-') return false; }
        else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
}
uint64_t EventFileTime(std::string_view utc, std::string_view event_filetime) {
    uint64_t precise = 0;
    if (!event_filetime.empty()) {
        const auto parsed = std::from_chars(event_filetime.data(),event_filetime.data()+event_filetime.size(),precise);
        if (parsed.ec == std::errc{} && parsed.ptr == event_filetime.data()+event_filetime.size() && precise) return precise;
    }
    if (!Utc(utc)) return 0;
    unsigned year=0,month=0,day=0,hour=0,minute=0,second=0,millis=0;
    const std::string text(utc);
    if (sscanf_s(text.c_str(),"%4u-%2u-%2uT%2u:%2u:%2u.%3u",&year,&month,&day,&hour,&minute,&second,&millis)!=7) return 0;
    SYSTEMTIME time{static_cast<WORD>(year),static_cast<WORD>(month),0,static_cast<WORD>(day),
        static_cast<WORD>(hour),static_cast<WORD>(minute),static_cast<WORD>(second),static_cast<WORD>(millis)};
    FILETIME file{};
    if(!SystemTimeToFileTime(&time,&file)) return 0;
    return (static_cast<uint64_t>(file.dwHighDateTime)<<32)|file.dwLowDateTime;
}
std::string Filter(std::string_view line, bool managed, bool launcher, Summary& s, bool local,
                   const ReproductionWindow* reproduction, bool& foreground) {
    json::Value value;
    if (!json::Parse(line, value, nullptr, {kLine, 8}) || !value.is_object()) { ++s.invalid; return {}; }
    const auto event = value.get_string("event");
    const Policy* policy = nullptr;
    for (const auto& item : kPolicies) if (item.event == event) { policy = &item; break; }
    if (!policy) { ++s.ignored; return {}; }
    const auto fields = Fields(Trim(line));
    const auto* data = value.find("data");
    const auto pid = Get(fields, "pid"), session = Get(fields, "session");
    const auto schema = Get(fields, "schema");
    if (!data || !data->is_object() || !Unsigned(pid, 0xffffffffu) || pid == "0" ||
        (!managed && !launcher && (schema != "1" || (value.get_string("component") != "dialog_auto" && value.get_string("component") != "dialog_runtime" && value.get_string("component") != "dialog_admin_auto") || !Unsigned(session) || session == "0" || !Unsigned(Get(fields, "seq")) || !Utc(value.get_string("utc")))) ||
        (launcher && (schema != "1" || event != "target_preflight" || !Guid(value.get_string("session")))) ||
        (!launcher && event == "target_preflight") ||
        (managed && ((!schema.empty() && schema != "1") || !Pair(value.get_string("session"), '-') || !Word(event, "dialog_show_begin dialog_show_terminal")))) {
        ++s.invalid; return {};
    }
    if (managed) {
        const auto id = value.get_string("session");
        if (id.substr(0, id.find('-')) != pid) { ++s.invalid; return {}; }
    }
    if (reproduction && Word(event, "dialog_show_begin dialog_show_terminal")) {
        const auto precise=Get(Fields(Get(fields,"data")),"event_time_filetime");
        const auto when=EventFileTime(value.get_string("utc"),precise);
        if (!when || when<reproduction->start_filetime || when>reproduction->end_filetime) return {};
        uint32_t process=0;
        std::from_chars(pid.data(),pid.data()+pid.size(),process);
        foreground=process==reproduction->foreground_pid;
    }
    std::string result = "{\"schema\":1,\"kind\":\"dialog_log_excerpt\",\"source\":" + Quote(launcher ? "managed_launcher" : managed ? "managed_runtime" : value.get_string("component")=="dialog_admin_auto" ? "dialog_admin_auto" : value.get_string("component")=="dialog_runtime" ? "dialog_runtime" : "dialog_auto") +
        ",\"event\":" + Quote(event) + ",\"pid\":" + std::string(pid) + ",\"session\":" +
        ((managed || launcher) ? Quote(value.get_string("session")) : local ? Quote(session) : std::string(session));
    if (!managed && !launcher) result += ",\"utc\":" + Quote(value.get_string("utc")) + ",\"seq\":" + (local ? Quote(Get(fields,"seq")) : std::string(Get(fields, "seq")));
    for (const auto key : {"tid", "tick_ms", "severity"}) {
        const auto raw = Get(fields, key);
        if (Unsigned(raw, std::string_view(key) == "severity" ? 2 : std::string_view(key) == "tid" ? 0xffffffffu : (std::numeric_limits<uint64_t>::max)())) result += "," + Quote(key) + ":" + std::string(raw);
    }
    result += ",\"data\":{";
    const auto raw_data = Fields(Get(fields, "data"));
    bool first = true;
    for (const auto& field : raw_data) {
        const auto key = field.first;
        std::string accepted;
        if (Word(key, policy->fields) || (local && Word(key,"app_basename target_creation_time_filetime creation_time_filetime app_instance_id identity_revision event_time_filetime event_sequence source_session source_event_sequence dialog_kind"))) {
            const auto* parsed = data->find(key);
            if(local && key=="app_basename" && parsed && parsed->is_string()) {
                const auto wide=json::Wide(parsed->as_string());
                const auto name=parsed->as_string();
                const bool clean=!wide.empty() && wide.size()<=255 && wide.find_first_of(L"\\/:<>\"|?*")==wide.npos &&
                    std::none_of(wide.begin(),wide.end(),[](wchar_t c){return c<32;}) && wide.size()>4 && _wcsicmp(wide.c_str()+wide.size()-4,L".exe")==0;
                if(clean) accepted=Quote(name);
            } else if(local && Word(key,"target_creation_time_filetime creation_time_filetime app_instance_id event_time_filetime event_sequence source_session source_event_sequence app_image_id") && Unsigned(field.second)) {
                accepted=Quote(field.second);
            } else if (key == "request") {
                if (Unsigned(field.second)) accepted = field.second;
                else if (parsed && parsed->is_string() && Pair(parsed->as_string(), ':')) accepted = Quote(parsed->as_string());
            } else if (key == "certified") {
                if (field.second == "false" || field.second == "0") accepted = "false";
            } else if (Word(key, "scan_complete cancelled timed_out token_safe registered conflict path_match path_case_match enabled accepted checked save process_elevated_known process_elevated admin_trust_checked admin_trust_accepted prefer_bridge remote_host reused deferred remote_fallback options_known native shown native_fallback provider_started ui_shown_known ui_shown")) {
                if (field.second == "true" || field.second == "1") accepted = "true";
                if (field.second == "false" || field.second == "0") accepted = "false";
            } else if (parsed && parsed->is_string()) {
                if (Enum(key, parsed->as_string())) accepted = Quote(parsed->as_string());
            } else if (!Word(key, "warning existing_target_managed reason_name stage elevation_type_name token_type_name entry_family launch_route dialog_kind terminal_status fallback_reason") &&
                Unsigned(field.second, Word(key, "dialog show_sequence call app_image_id") ? (std::numeric_limits<uint64_t>::max)() : 0xffffffffu)) {
                if (!Word(key, "process_arch process_arch_bits") || field.second == "32" || field.second == "64") accepted = field.second;
            }
        }
        if (accepted.empty()) { ++s.omitted; continue; }
        if (!first) result += ',';
        first = false; result += Quote(key) + ":" + accepted;
    }
    return result + "}}\n";
}
std::string FilterExplorer(std::string_view line, Summary& s, const ReproductionWindow* reproduction) {
    static constexpr Policy policies[]{
        {"explorer_takeover_config", "enabled integration takeover compat this_pc isolated folders_requested folders_direct_desired folders_direct_active"},
        {"explorer_watcher_start", "phase hr baseline_windows window_hook"},
        {"explorer_watcher_ready", "attempt"},
        {"explorer_watcher_retry", "attempt delay_ms"},
        {"explorer_watcher_scan", "complete"},
        {"explorer_window_shown", "window source_pid already_known event_tick"},
        {"explorer_candidate", "window source_pid candidate_id reason age_ms view_age_ms stable_age_ms view_ready supported selection_read selected tabs source_exists scoped_view window_identity_matches"},
        {"explorer_candidate_link", "window source_pid candidate_id request_id"},
        {"explorer_handoff", "request_id phase code detail elapsed_ms"},
        {"explorer_source_close_unconfirmed", "request_id source_exists marker_matches source_pid"},
        {"explorer_presentation_wake", "request_id posted"},
        {"external_open", "id_high id_low phase code entry handoff_id"},
    };
    json::Value value;
    if (!json::Parse(line, value, nullptr, {kLine, 8}) || !value.is_object()) { ++s.invalid; return {}; }
    const auto event = value.get_string("event");
    const auto policy = std::find_if(std::begin(policies), std::end(policies),
        [&](const Policy& item) { return item.event == event; });
    if (policy == std::end(policies) || value.get_string("component") != "app") { ++s.ignored; return {}; }
    const auto fields = Fields(Trim(line));
    const auto pid = Get(fields, "pid"), seq = Get(fields, "seq"), session = Get(fields, "session");
    const auto utc = value.get_string("utc");
    const auto* data = value.find("data");
    if (Get(fields, "schema") != "1" || !Unsigned(pid, 0xffffffffu) || pid == "0" ||
        !Unsigned(seq) || !Unsigned(session) || session == "0" || !Utc(utc) ||
        !data || !data->is_object()) { ++s.invalid; return {}; }
    const auto raw = Fields(Get(fields, "data"));
    if (event == "external_open" && Get(raw, "entry") != "1" && Get(raw, "entry") != "2") {
        ++s.ignored;
        return {};
    }
    if (reproduction) {
        const auto when = EventFileTime(utc, {});
        const bool state = Word(event, "explorer_takeover_config explorer_watcher_start explorer_watcher_ready explorer_watcher_retry explorer_watcher_scan");
        const auto max = (std::max)(reproduction->start_filetime, reproduction->end_filetime);
        if (!when || when / 10000 > max / 10000 || (!state && when / 10000 < reproduction->start_filetime / 10000))
            return {};
    }
    std::string result = "{\"schema\":1,\"kind\":\"explorer_log_excerpt\",\"source\":\"app\",\"event\":" +
        Quote(event) + ",\"pid\":" + std::string(pid) + ",\"session\":" + Quote(session) +
        ",\"utc\":" + Quote(utc) + ",\"seq\":" + Quote(seq) + ",\"data\":{";
    bool first = true;
    for (const auto& [key, literal] : raw) {
        if (!Word(key, policy->fields) || (!Unsigned(literal) && literal != "true" && literal != "false")) {
            ++s.omitted;
            continue;
        }
        if (!first) result += ',';
        first = false;
        result += Quote(key) + ":" + std::string(literal);
    }
    return result + "}}\n";
}
bool Name(const std::wstring& name, bool& managed, bool& launcher, bool /*local*/, bool explorer = false) {
    if (!explorer) {
        launcher = name == L"launcher-preflight.jsonl";
        if (launcher) return true;
        managed = name == L"runtime-dialog-shows.jsonl";
        if (managed) return true;
    }
    const std::wstring_view prefix = explorer ? L"app-" : name.starts_with(L"dialog_admin_auto-") ? L"dialog_admin_auto-" : name.starts_with(L"dialog_runtime-") ? L"dialog_runtime-" : L"dialog_auto-";
    if (!name.starts_with(prefix)) return false;
    size_t p = prefix.size();
    const size_t first = p;
    while (p < name.size() && name[p] >= L'0' && name[p] <= L'9') ++p;
    if (p == first || p - first > 10) return false;
    const auto tail = std::wstring_view(name).substr(p);
    return tail == L".jsonl" || tail == L".critical.jsonl" || tail == L".jsonl.1" || tail == L".critical.jsonl.1";
}
std::wstring FinalName(HANDLE file) {
    std::wstring value(32768, 0);
    const DWORD size = GetFinalPathNameByHandleW(file, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED);
    if (!size || size >= value.size()) return {};
    value.resize(size); return value;
}
std::string Finish(std::string output, const Summary& s, bool explorer = false) {
    auto value = json::Value::Object();
    value.set("schema", 1);
    value.set("event", explorer ? "explorer_log_collection" : "dialog_log_collection");
    value.set("scope", "direct_whitelisted_log_excerpt");
    value.set("no_entry_inference", true);
    value.set("process_snapshot_joined", false);
    value.set("record_order", "newest_files_first_original_order_within_file");
    const bool incomplete = s.directory_error || s.enumeration_error || s.read_errors || s.invalid || s.partial || s.oversized_lines || s.oversized_files || s.rejected_files || s.tails || s.enumeration_limited || s.files_limited || s.read_limited || s.output_limited || s.line_limited || s.cancelled;
    value.set("incomplete", incomplete);
    value.set("cancelled", s.cancelled);
    value.set("callback_error", s.callback_error);
    value.set("directory_error", static_cast<uint64_t>(s.directory_error));
    value.set("enumeration_error", static_cast<uint64_t>(s.enumeration_error));
    value.set("last_read_error", static_cast<uint64_t>(s.last_read_error));
    value.set("show_begin_events", static_cast<uint64_t>(s.evidence.shows_started));
    value.set("show_terminal_events", static_cast<uint64_t>(s.evidence.shows_finished));
    value.set("runtime_decision_events", static_cast<uint64_t>(s.evidence.runtime_decisions));
    for (const auto& item : std::initializer_list<std::pair<const char*, uint64_t>>{{"entries_scanned",s.entries},{"eligible_files",s.eligible},{"files_read",s.files},{"bytes_read",s.bytes},{"lines_scanned",s.lines},{"records_emitted",s.records},{"non_diagnostic_lines",s.ignored},{"invalid_lines",s.invalid},{"fields_omitted",s.omitted},{"partial_lines_discarded",s.partial},{"oversized_lines",s.oversized_lines},{"oversized_files",s.oversized_files},{"rejected_files",s.rejected_files},{"tail_files",s.tails}}) value.set(item.first,item.second);
    value.set("enumeration_limited",s.enumeration_limited);
    value.set("files_limited",s.files_limited);
    value.set("read_limited",s.read_limited);
    value.set("output_limited",s.output_limited);
    value.set("line_limited",s.line_limited);
    value.set("max_files",static_cast<uint64_t>(kFiles));
    value.set("max_tail_bytes",static_cast<uint64_t>(kTail));
    value.set("max_read_bytes",static_cast<uint64_t>(kRead));
    value.set("max_line_bytes",static_cast<uint64_t>(kLine));
    value.set("max_records",static_cast<uint64_t>(kRecords));
    value.set("max_output_bytes",static_cast<uint64_t>(kOutput));
    return output + json::Serialize(value) + "\n";
}
}

static std::string CollectExcerpt(const std::wstring& directory, const std::function<bool()>& cancelled,
                                  bool local, ExcerptCounts* counts = nullptr, bool verified_managed = false,
                                  const ReproductionWindow* reproduction = nullptr, bool explorer = false) {
    Summary s;
    const auto finish = [&](std::string output) { return Finish(std::move(output), s, explorer); };
    struct Publish { ExcerptCounts* out; const Summary& summary;
        ~Publish() { if (out) *out = summary.evidence; }
    } publish{counts, s};
    std::string output;
    try {
        if (Cancel(cancelled,s)) return finish({});
        if (directory.empty() || directory.size() > 32760 || directory.find(L'\0') != directory.npos || !std::filesystem::path(directory).is_absolute() || directory.starts_with(L"\\\\.\\")) {
            s.directory_error = ERROR_INVALID_NAME; return finish({});
        }
        constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        Handle root{CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,share,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        BY_HANDLE_FILE_INFORMATION root_info{};
        if (root.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(root.value,&root_info)) { s.directory_error=GetLastError();return finish({}); }
        if (!(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {s.directory_error=ERROR_ACCESS_DENIED;return finish({});}
        struct Candidate {std::wstring name;uint64_t modified;bool managed;bool launcher;bool decision;};
        std::vector<Candidate> selected;
        WIN32_FIND_DATAW found{};
        const auto pattern=(std::filesystem::path(directory)/L"*").wstring();
        HANDLE search=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&found,FindExSearchNameMatch,nullptr,0);
        if(search==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)s.enumeration_error=error;return finish({});}
        do {
            if(Cancel(cancelled,s))break;
            if(++s.entries>kDirectoryEntries){s.enumeration_limited=true;break;}
            bool managed=false,launcher=false;
            const bool decision=verified_managed && std::wstring_view(found.cFileName)==L"runtime-decisions.tsv";
            if(!decision && !Name(found.cFileName,managed,launcher,local,explorer))continue;
            if(found.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)){++s.rejected_files;continue;}
            const uint64_t size=(static_cast<uint64_t>(found.nFileSizeHigh)<<32)|found.nFileSizeLow;
            if(size>kFileSize){++s.oversized_files;continue;}
            ++s.eligible;
            selected.push_back({found.cFileName,(static_cast<uint64_t>(found.ftLastWriteTime.dwHighDateTime)<<32)|found.ftLastWriteTime.dwLowDateTime,managed,launcher,decision});
            std::sort(selected.begin(),selected.end(),[](const Candidate& a,const Candidate& b){return a.modified!=b.modified?a.modified>b.modified:a.name>b.name;});
            if(selected.size()>kFiles){selected.pop_back();s.files_limited=true;}
        }while(FindNextFileW(search,&found));
        const auto search_error=GetLastError();FindClose(search);
        if(!s.cancelled&&!s.enumeration_limited&&search_error!=ERROR_NO_MORE_FILES)s.enumeration_error=search_error;
        bool stop=false;
        for(const auto& file:selected){
            if(stop||Cancel(cancelled,s))break;
            if(s.bytes>=kRead){s.read_limited=true;break;}
            const auto path=(std::filesystem::path(directory)/file.name).wstring();
            Handle input{CreateFileW(path.c_str(),GENERIC_READ,share,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
            BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
            if(input.value==INVALID_HANDLE_VALUE||!GetFileInformationByHandle(input.value,&info)||!GetFileSizeEx(input.value,&size)){++s.read_errors;s.last_read_error=GetLastError();continue;}
            const auto parent=std::filesystem::path(FinalName(input.value)).parent_path().wstring();
            const auto root_name=FinalName(root.value);
            if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1||parent.empty()||root_name.empty()||_wcsicmp(parent.c_str(),root_name.c_str())){++s.rejected_files;continue;}
            if(size.QuadPart<0||static_cast<uint64_t>(size.QuadPart)>kFileSize){++s.oversized_files;continue;}
            const auto want=static_cast<DWORD>((std::min)({static_cast<uint64_t>(size.QuadPart),static_cast<uint64_t>(kTail),static_cast<uint64_t>(kRead-s.bytes)}));
            const LONGLONG offset=size.QuadPart-want;
            LARGE_INTEGER position{};position.QuadPart=offset;
            if(!SetFilePointerEx(input.value,position,nullptr,FILE_BEGIN)){++s.read_errors;s.last_read_error=GetLastError();continue;}
            std::string buffer(want,0);DWORD read=0;
            if(want&&!ReadFile(input.value,buffer.data(),want,&read,nullptr)){++s.read_errors;s.last_read_error=GetLastError();continue;}
            ++s.files;s.bytes+=read;buffer.resize(read);
            if(read!=want){++s.read_errors;s.last_read_error=ERROR_HANDLE_EOF;}
            size_t begin=0,end=buffer.size();
            if(offset){++s.tails;++s.partial;const auto first=buffer.find('\n');if(first==buffer.npos)continue;begin=first+1;}
            if(end&&buffer.back()!='\n'){++s.partial;const auto last=buffer.rfind('\n');if(last==buffer.npos)continue;end=last+1;}
            std::vector<std::string> records;
            size_t record_bytes=0;
            while(end>begin){
                if(Cancel(cancelled,s)){stop=true;break;}
                if(s.lines>=kLines){s.line_limited=true;stop=true;break;}
                ++s.lines;
                const auto newline=end-1;
                const auto previous=newline?buffer.rfind('\n',newline-1):buffer.npos;
                const size_t start=previous==buffer.npos?begin:(std::max)(begin,previous+1);
                auto line=Trim(std::string_view(buffer).substr(start,newline-start));end=start;
                if(line.size()>kLine){++s.oversized_lines;continue;}
                if(line.empty())continue;
                bool foreground=false;
                auto record=explorer ? FilterExplorer(line,s,reproduction) :
                    file.decision ? FilterDecision(line,s) : Filter(line,file.managed,file.launcher,s,local,reproduction,foreground);
                if(record.empty())continue;
                if(s.records>=kRecords||output.size()+record_bytes+record.size()>kOutput-4096){s.output_limited=true;stop=true;break;}
                if(file.decision)++s.evidence.runtime_decisions;
                else if(record.find("\"event\":\"dialog_show_begin\"")!=record.npos) {
                    ++s.evidence.shows_started;if(foreground)++s.evidence.foreground_shows_started;
                } else if(record.find("\"event\":\"dialog_show_terminal\"")!=record.npos) {
                    ++s.evidence.shows_finished;if(foreground)++s.evidence.foreground_shows_finished;
                }
                record_bytes+=record.size();records.push_back(std::move(record));++s.records;
            }
            // Selection favors recent records; presentation preserves the file's
            // original sequence so downstream last-record decisions stay valid.
            for(auto record=records.rbegin();record!=records.rend();++record)output+=*record;
        }
        return finish(std::move(output));
    }catch(...){s.read_errors++;s.last_read_error=ERROR_INVALID_DATA;return finish(std::move(output));}
}
std::string CollectDialogLogExcerpt(const std::wstring& directory,const std::function<bool()>& cancelled,
                                   ExcerptCounts* counts,bool verified_managed_directory,const ReproductionWindow* reproduction) {
    return CollectExcerpt(directory,cancelled,false,counts,verified_managed_directory,reproduction);
}
std::string CollectLocalTakeoverExcerpt(const std::wstring& directory,const std::function<bool()>& cancelled) {
    return CollectExcerpt(directory,cancelled,true);
}
std::string CollectExplorerLogExcerpt(const std::wstring& directory,const std::function<bool()>& cancelled,
                                      const ReproductionWindow* reproduction) {
    return CollectExcerpt(directory,cancelled,false,nullptr,false,reproduction,true);
}

void CopyDialogRawLogs(const std::wstring& source,const std::wstring& destination,bool managed,
    RawLogCopy& result,const std::function<bool()>& cancelled) {
    namespace fs=std::filesystem;
    constexpr DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
    constexpr uint64_t max_file=16ull*1024*1024, max_total=64ull*1024*1024;
    constexpr unsigned max_files=64, max_entries=512;
    const auto failed=[&](DWORD error){++result.read_errors;result.last_error=error;};
    const auto stop=[&]{return cancelled && cancelled();};
    const auto eligible=[&](const std::wstring& name){
        if(managed) return name==L"launcher.log" || name==L"launcher-preflight.jsonl" ||
            name==L"runtime-dialog-shows.jsonl" || name==L"runtime-decisions.tsv";
        bool session=false,launcher=false;
        if(Name(name,session,launcher,false)) return !session && !launcher;
        for(const std::wstring_view prefix:{L"dialog_auto_control-",L"picker_ui-"}) {
            if(!name.starts_with(prefix)) continue;
            auto tail=std::wstring_view(name).substr(prefix.size());
            const auto dot=tail.find(L'.');
            if(dot==tail.npos || dot==0 || dot>10 || tail.substr(0,dot).find_first_not_of(L"0123456789")!=tail.npos) return false;
            tail.remove_prefix(dot);
            return tail==L".jsonl" || tail==L".jsonl.1" || tail==L".critical.jsonl" || tail==L".critical.jsonl.1";
        }
        return false;
    };
    if(stop()) return;
    if(source.empty() || !fs::path(source).is_absolute() || source.starts_with(L"\\\\.\\")) {
        failed(ERROR_INVALID_NAME);return;
    }
    Handle root{CreateFileW(source.c_str(),FILE_READ_ATTRIBUTES,share,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    BY_HANDLE_FILE_INFORMATION root_info{};
    if(root.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(root.value,&root_info)) {
        failed(GetLastError());return;
    }
    const auto root_name=FinalName(root.value);
    if(root_name.empty() || !(root_info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
       (root_info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {failed(ERROR_INVALID_NAME);return;}
    struct Candidate {std::wstring name;uint64_t modified;};
    std::vector<Candidate> files;
    std::error_code ec;
    unsigned entries=0;
    for(fs::directory_iterator it(source,ec),end; !ec && it!=end;it.increment(ec)) {
        if(stop()) return;
        if(++entries>max_entries) {result.limited=true;break;}
        const auto name=it->path().filename().wstring();
        if(!eligible(name)) continue;
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if(!GetFileAttributesExW(it->path().c_str(),GetFileExInfoStandard,&attributes)) {failed(GetLastError());continue;}
        if(attributes.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) {++result.skipped;continue;}
        files.push_back({name,(static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime)<<32)|attributes.ftLastWriteTime.dwLowDateTime});
    }
    if(ec) failed(static_cast<DWORD>(ec.value()));
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){
        return a.modified!=b.modified ? a.modified>b.modified : a.name<b.name;
    });
    std::vector<char> buffer(64*1024);
    for(const auto& file:files) {
        if(stop()) return;
        if(result.files>=max_files) {++result.skipped;result.limited=true;continue;}
        const auto input_path=fs::path(source)/file.name;
        Handle input{CreateFileW(input_path.c_str(),GENERIC_READ,share,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
        BY_HANDLE_FILE_INFORMATION info{};
        if(input.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(input.value,&info)) {failed(GetLastError());continue;}
        const auto parent=fs::path(FinalName(input.value)).parent_path().wstring();
        if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks!=1 || parent.empty() || _wcsicmp(parent.c_str(),root_name.c_str())!=0) {++result.skipped;continue;}
        const uint64_t size=(static_cast<uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        if(size>max_file || size>max_total-result.bytes) {++result.skipped;result.limited=true;continue;}
        const auto output_path=fs::path(destination)/file.name;
        DWORD error=ERROR_SUCCESS;
        {
            Handle output{CreateFileW(output_path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
            if(output.value==INVALID_HANDLE_VALUE) {failed(GetLastError());continue;}
            uint64_t left=size;
            while(left && !error) {
                if(stop()) {error=ERROR_CANCELLED;break;}
                const auto chunk=static_cast<DWORD>((std::min)(left,static_cast<uint64_t>(buffer.size())));
                DWORD read=0,written=0;
                if(!ReadFile(input.value,buffer.data(),chunk,&read,nullptr)) error=GetLastError();
                else if(read!=chunk) error=ERROR_HANDLE_EOF;
                else if(!WriteFile(output.value,buffer.data(),read,&written,nullptr)) error=GetLastError();
                else if(written!=read) error=ERROR_WRITE_FAULT;
                else left-=written;
            }
        }
        if(error) {
            DeleteFileW(output_path.c_str());
            if(error==ERROR_CANCELLED) return;
            failed(error);
        } else {
            ++result.files;result.bytes+=size;
        }
    }
}

}
