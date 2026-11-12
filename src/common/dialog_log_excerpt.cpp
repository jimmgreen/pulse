#include "dialog_log_excerpt.h"
#include "json_value.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
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
    {"dialog_show_begin", "request dialog show_sequence process_arch entry_family launch_route"},
    {"dialog_show_terminal", "request dialog show_sequence process_arch hr native_fallback provider_started ui_shown_known ui_shown entry_family launch_route terminal_status reason_name fallback_reason"}
};
bool Enum(std::string_view key, std::string_view value) {
    if (key == "warning") return Word(value, "existing_target scan_incomplete none");
    if (key == "existing_target_managed") return value == "unknown";
    if (key == "stage") return Word(value, "initialize activation resolve validation");
    if (key == "elevation_type_name") return Word(value, "default full limited unknown");
    if (key == "token_type_name") return Word(value, "primary impersonation unknown");
    if (key == "entry_family") return Word(value, "COM OFN_W OFN_A SHBROWSE_W SHBROWSE_A");
    if (key == "launch_route") return Word(value, "ordinary managed");
    if (key == "terminal_status") return Word(value, "pending native accepted cancelled failed closed");
    if (key == "fallback_reason") return value.empty() || Word(value, "none unknown resolve-not-ready provider-unavailable initial-folder-unavailable contract provider-prepare-failed provider-failed-before-ui ENTRY_DISABLED_OR_UNAVAILABLE INITIAL_FOLDER_UNREPRESENTABLE PROVIDER_START_FAILED_BEFORE_UI NATIVE_CONTRACT_FALLBACK");
    if (key != "reason_name") return false;
    return Word(value, "TOKEN_ACCEPTED PROCESS_TOKEN_OBSERVED THREAD_IMPERSONATION THREAD_TOKEN_QUERY_FAILED PROCESS_TOKEN_QUERY_FAILED ELEVATION_QUERY_FAILED ELEVATED_BUNDLE_UNTRUSTED RESTRICTED_TOKEN APPCONTAINER_QUERY_FAILED APPCONTAINER INTEGRITY_QUERY_FAILED INVALID_INTEGRITY_SID BELOW_MEDIUM_INTEGRITY ELIGIBLE AUTOMATIC_DISABLED REGISTRATION_QUERY_FAILED REGISTRATION_CONFLICT REGISTERED_MODULE_MISMATCH REGISTRATION_CHANGED_OR_NOT_CURRENT PROVIDER_UNAVAILABLE CONTRACT_NOT_CERTIFIED TRUSTED None SetterFailed UnrecognizedSuccess UnknownSetting UnsupportedOptions FilesystemNotForced RecentItemsPolicy MissingValidationOptions DefaultDirectoryPolicy NonFilesystemFolder UnsupportedFilename UnsupportedExtension UnsupportedFilter UnsupportedSettingOrder FolderFileSettings SaveContract Events Customization Labels Places Persistence SaveMetadata ShellFilter InvalidData Limit Unknown NONE SETTER_FAILED UNRECOGNIZED_SUCCESS UNKNOWN_SETTING UNSUPPORTED_OPTIONS FILESYSTEM_NOT_FORCED RECENT_ITEMS_POLICY MISSING_VALIDATION_OPTIONS DEFAULT_DIRECTORY_POLICY NON_FILESYSTEM_FOLDER UNSUPPORTED_FILENAME UNSUPPORTED_EXTENSION UNSUPPORTED_FILTER UNSUPPORTED_SETTING_ORDER FOLDER_FILE_SETTINGS SAVE_CONTRACT EVENTS CUSTOMIZATION LABELS PLACES PERSISTENCE SAVE_METADATA SHELL_FILTER INVALID_DATA LIMIT UNKNOWN");
}
bool Guid(std::string_view value) {
    if (value.size() != 38 || value.front() != '{' || value.back() != '}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (value[i] != '-') return false; }
        else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
}
std::string Filter(std::string_view line, bool managed, bool launcher, Summary& s, bool local) {
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
            } else if(local && key=="dialog_kind" && parsed && parsed->is_string()) {
                if(Word(parsed->as_string(),"open save unknown")) accepted=Quote(parsed->as_string());
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
            } else if (!Word(key, "warning existing_target_managed reason_name stage elevation_type_name token_type_name entry_family launch_route terminal_status fallback_reason") &&
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
bool Name(const std::wstring& name, bool& managed, bool& launcher, bool /*local*/) {
    launcher = name == L"launcher-preflight.jsonl";
    if (launcher) return true;
    managed = name == L"runtime-dialog-shows.jsonl";
    if (managed) return true;
    const std::wstring_view prefix = name.starts_with(L"dialog_admin_auto-") ? L"dialog_admin_auto-" : name.starts_with(L"dialog_runtime-") ? L"dialog_runtime-" : L"dialog_auto-";
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
std::string Finish(std::string output, const Summary& s) {
    auto value = json::Value::Object();
    value.set("schema", 1);
    value.set("event", "dialog_log_collection");
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

static std::string CollectExcerpt(const std::wstring& directory, const std::function<bool()>& cancelled, bool local) {
    Summary s;
    std::string output;
    try {
        if (Cancel(cancelled,s)) return Finish({},s);
        if (directory.empty() || directory.size() > 32760 || directory.find(L'\0') != directory.npos || !std::filesystem::path(directory).is_absolute() || directory.starts_with(L"\\\\.\\")) {
            s.directory_error = ERROR_INVALID_NAME; return Finish({},s);
        }
        constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        Handle root{CreateFileW(directory.c_str(),FILE_READ_ATTRIBUTES,share,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        BY_HANDLE_FILE_INFORMATION root_info{};
        if (root.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(root.value,&root_info)) { s.directory_error=GetLastError();return Finish({},s); }
        if (!(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {s.directory_error=ERROR_ACCESS_DENIED;return Finish({},s);}
        struct Candidate {std::wstring name;uint64_t modified;bool managed;bool launcher;};
        std::vector<Candidate> selected;
        WIN32_FIND_DATAW found{};
        const auto pattern=(std::filesystem::path(directory)/L"*").wstring();
        HANDLE search=FindFirstFileExW(pattern.c_str(),FindExInfoBasic,&found,FindExSearchNameMatch,nullptr,0);
        if(search==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND)s.enumeration_error=error;return Finish({},s);}
        do {
            if(Cancel(cancelled,s))break;
            if(++s.entries>kDirectoryEntries){s.enumeration_limited=true;break;}
            bool managed=false,launcher=false;
            if(!Name(found.cFileName,managed,launcher,local))continue;
            if(found.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)){++s.rejected_files;continue;}
            const uint64_t size=(static_cast<uint64_t>(found.nFileSizeHigh)<<32)|found.nFileSizeLow;
            if(size>kFileSize){++s.oversized_files;continue;}
            ++s.eligible;
            selected.push_back({found.cFileName,(static_cast<uint64_t>(found.ftLastWriteTime.dwHighDateTime)<<32)|found.ftLastWriteTime.dwLowDateTime,managed,launcher});
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
                auto record=Filter(line,file.managed,file.launcher,s,local);
                if(record.empty())continue;
                if(s.records>=kRecords||output.size()+record_bytes+record.size()>kOutput-4096){s.output_limited=true;stop=true;break;}
                record_bytes+=record.size();records.push_back(std::move(record));++s.records;
            }
            // Selection favors recent records; presentation preserves the file's
            // original sequence so downstream last-record decisions stay valid.
            for(auto record=records.rbegin();record!=records.rend();++record)output+=*record;
        }
        return Finish(std::move(output),s);
    }catch(...){s.read_errors++;s.last_read_error=ERROR_INVALID_DATA;return Finish(std::move(output),s);}
}
std::string CollectDialogLogExcerpt(const std::wstring& directory,const std::function<bool()>& cancelled) {
    return CollectExcerpt(directory,cancelled,false);
}
std::string CollectLocalTakeoverExcerpt(const std::wstring& directory,const std::function<bool()>& cancelled) {
    return CollectExcerpt(directory,cancelled,true);
}

}
