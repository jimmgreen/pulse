#include "dialog_takeover_history.h"
#include "dialog_log_excerpt.h"
#include "json_value.h"
#include "current_user_security.h"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <cstdio>
namespace pulse::dialog_history {
namespace {
constexpr size_t kRecords=128,kCursors=256,kIdentities=256,kBytes=512*1024;
using json::Value;
uint64_t Number(const Value& v,std::string_view name) {
    const auto* p=v.find(name);if(!p)return 0;
    if(p->is_string()) {const auto& s=p->as_string();uint64_t n=0;auto r=std::from_chars(s.data(),s.data()+s.size(),n);return !s.empty()&&r.ec==std::errc{}&&r.ptr==s.data()+s.size()?n:0;}
    int64_t n=0;return p->get_int(n,0,0xffffffff)?static_cast<uint64_t>(n):0;
}
bool Basename(const std::wstring& s) {
    return s.size()>4&&s.size()<=255&&_wcsicmp(s.c_str()+s.size()-4,L".exe")==0&&s.find_first_of(L"\\/:<>\"|?*")==s.npos&&
        std::none_of(s.begin(),s.end(),[](wchar_t c){return c<32;});
}
bool Utc(const std::wstring& s) {
    if(s.size()!=24||s[4]!=L'-'||s[7]!=L'-'||s[10]!=L'T'||s[13]!=L':'||s[16]!=L':'||s[19]!=L'.'||s[23]!=L'Z')return false;
    for(size_t i=0;i<s.size();++i)if(i!=4&&i!=7&&i!=10&&i!=13&&i!=16&&i!=19&&i!=23&&(s[i]<L'0'||s[i]>L'9'))return false;
    auto n=[&](size_t i,size_t count){WORD v=0;while(count--)v=static_cast<WORD>(v*10+s[i++]-L'0');return v;};
    SYSTEMTIME st{};st.wYear=n(0,4);st.wMonth=n(5,2);st.wDay=n(8,2);st.wHour=n(11,2);st.wMinute=n(14,2);st.wSecond=n(17,2);st.wMilliseconds=n(20,3);FILETIME ft{};
    return SystemTimeToFileTime(&st,&ft)!=FALSE;
}
std::wstring UtcFiletime(uint64_t t) {
    FILETIME ft{static_cast<DWORD>(t),static_cast<DWORD>(t>>32)};SYSTEMTIME st{};
    if(!t||!FileTimeToSystemTime(&ft,&st))return {};
    wchar_t out[32]{};swprintf_s(out,L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds);return out;
}
bool Token(std::string_view s) {return !s.empty()&&s.size()<=128&&s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-:|")==s.npos;}
struct Event {Value data;std::string name,source,session,base,producer;uint32_t pid=0;uint64_t sequence=0;std::wstring utc;};
std::string Base(const Identity& i){return i.source+"|"+std::to_string(i.pid)+"|"+i.session;}
Identity ParseIdentity(const Event& e) {
    Identity i;if(Number(e.data,"identity_revision")!=2)return i;
    i.creation_time=Number(e.data,"creation_time_filetime");i.image_id=Number(e.data,"app_instance_id");i.basename=json::Wide(e.data.get_string("app_basename"));
    if(!i.creation_time||!i.image_id||!Basename(i.basename))return {};
    i.pid=e.pid;i.source=e.source;i.session=e.session;i.producer=e.base+"|"+std::to_string(i.creation_time);return i;
}
void Trim(History& h) {
    while(h.records.size()>kRecords) {auto it=std::min_element(h.records.begin(),h.records.end(),[](const Record& a,const Record& b){return a.last_utc<b.last_utc;});h.records.erase(it);h.partial=h.retention_limited=true;}
    while(h.cursors.size()>kCursors) {auto it=std::min_element(h.cursors.begin(),h.cursors.end(),[](const Cursor& a,const Cursor& b){return a.last_utc<b.last_utc;});h.ignore_before_utc=(std::max)(h.ignore_before_utc,it->last_utc);h.cursors.erase(it);h.partial=h.retention_limited=true;}
    if(h.identities.size()>kIdentities) {h.identities.erase(h.identities.begin(),h.identities.begin()+static_cast<ptrdiff_t>(h.identities.size()-kIdentities));h.partial=h.retention_limited=true;}
}
bool Consume(History& h,const std::string& producer,uint64_t seq,const std::wstring& utc) {
    if(!seq||utc<=h.ignore_before_utc)return false;
    auto it=std::find_if(h.cursors.begin(),h.cursors.end(),[&](const Cursor& c){return c.producer==producer;});
    if(it!=h.cursors.end()) {if(seq<=it->sequence)return false;it->sequence=seq;it->last_utc=(std::max)(it->last_utc,utc);}
    else h.cursors.push_back({producer,seq,utc});
    Trim(h);return true;
}
Action Advice(Reason r) {
    switch(r) {
    case Reason::Succeeded:case Reason::ShownClosed:return Action::None;
    case Reason::ComponentMismatch:return Action::RestartApplication;
    case Reason::AutomaticDisabled:case Reason::RegistrationConflict:case Reason::AdminAutoEntryFailed:return Action::CheckIntegrationSettings;
    case Reason::ProviderUnavailable:case Reason::Timeout:return Action::RetryDialog;
    case Reason::UnsupportedRequest:case Reason::InitialFolderUnsupported:case Reason::NativeFallback:return Action::UseNativeDialog;
    default:return Action::ExportDiagnostics;
    }
}
bool Classify(const Event& e,State& state,Reason& reason) {
    if (e.name == "admin_auto_target_result") {
        if (!Number(e.data, "error")) return false; // A ready loader is not a displayed dialog.
        state = State::Failure; reason = Reason::AdminAutoEntryFailed; return true;
    }
    const auto terminal=e.data.get_string("terminal_status"),r=e.data.get_string("reason_name"),fallback=e.data.get_string("fallback_reason");
    const auto hr=Number(e.data,"hr");
    if(e.name=="dialog_show_terminal") {
        if(!e.data.get_bool("native_fallback")&&(terminal=="cancelled"||terminal=="closed"||hr==0x800704c7u)) {
            if ((terminal=="cancelled"||terminal=="closed") && e.data.get_bool("ui_shown_known") && e.data.get_bool("ui_shown")) {
                state=State::Success;reason=Reason::ShownClosed;return true;
            }
            return false;
        }
        if(terminal=="accepted"&&e.data.get_bool("ui_shown_known")&&e.data.get_bool("ui_shown")&&!e.data.get_bool("native_fallback")) {state=State::Success;reason=Reason::Succeeded;return true;}
        state=e.data.get_bool("native_fallback")?State::Fallback:terminal=="failed"?State::Failure:State::Unknown;
    } else if(e.name=="token_rejected"||e.name=="activation_gate"||e.name=="resolve_gate"||e.name=="validation_gate") {
        if(r.empty()||r=="ELIGIBLE"||r=="TRUSTED"||r=="TOKEN_ACCEPTED")return false;
        state=State::Unknown;
    } else return false;
    if(hr==0x800705b4u||hr==0x8001011fu||Number(e.data,"error")==ERROR_TIMEOUT)reason=Reason::Timeout;
    else if(r=="REGISTERED_MODULE_MISMATCH"||r=="REGISTRATION_CHANGED_OR_NOT_CURRENT")reason=Reason::ComponentMismatch;
    else if(r=="AUTOMATIC_DISABLED")reason=Reason::AutomaticDisabled;
    else if(r=="REGISTRATION_CONFLICT")reason=Reason::RegistrationConflict;
    else if(r=="PROCESS_TOKEN_QUERY_FAILED"||r=="THREAD_TOKEN_QUERY_FAILED"||r=="ELEVATION_QUERY_FAILED"||r=="INTEGRITY_QUERY_FAILED")reason=Reason::PermissionUndiagnosable;
    else if(r=="ELEVATED_BUNDLE_UNTRUSTED"||r=="RESTRICTED_TOKEN"||r=="APPCONTAINER"||r=="BELOW_MEDIUM_INTEGRITY"||r=="THREAD_IMPERSONATION")reason=Reason::PermissionDenied;
    else if(r=="PROVIDER_UNAVAILABLE"||fallback=="provider-unavailable"||fallback=="resolve-not-ready")reason=Reason::ProviderUnavailable;
    else if(fallback=="initial-folder-unavailable"||fallback=="INITIAL_FOLDER_UNREPRESENTABLE")reason=Reason::InitialFolderUnsupported;
    else if(fallback=="provider-prepare-failed"||fallback=="provider-failed-before-ui"||fallback=="PROVIDER_START_FAILED_BEFORE_UI")reason=Reason::ProviderFailure;
    else if(r=="CONTRACT_NOT_CERTIFIED"||fallback=="contract"||fallback=="NATIVE_CONTRACT_FALLBACK"||r.starts_with("Unsupported")||r.starts_with("UNSUPPORTED_"))reason=Reason::UnsupportedRequest;
    else if(state==State::Fallback)reason=Reason::NativeFallback;
    else reason=Reason::Unknown;
    return true;
}
struct Handle {HANDLE h=INVALID_HANDLE_VALUE;~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
DWORD Directory(const std::wstring& dir,std::wstring& canonical,Handle& handle) {
    const std::filesystem::path path(dir);if(!path.is_absolute())return ERROR_BAD_PATHNAME;
    auto part=path.root_path();for(const auto& component:path.relative_path()){part/=component;const DWORD a=GetFileAttributesW(part.c_str());if(a==INVALID_FILE_ATTRIBUTES)return GetLastError();if(a&FILE_ATTRIBUTE_REPARSE_POINT)return ERROR_REPARSE_TAG_INVALID;}
    handle.h=CreateFileW(dir.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle.h==INVALID_HANDLE_VALUE)return GetLastError();BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle.h,&info))return GetLastError();if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return ERROR_ACCESS_DENIED;
    canonical.resize(32768);const DWORD n=GetFinalPathNameByHandleW(handle.h,canonical.data(),static_cast<DWORD>(canonical.size()),FILE_NAME_NORMALIZED);if(!n||n>=canonical.size())return ERROR_BAD_PATHNAME;canonical.resize(n);return 0;
}
DWORD Read(const std::wstring& path,std::string& bytes) {
    Handle f{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    if(f.h==INVALID_HANDLE_VALUE)return GetLastError();BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(f.h,&info)||!GetFileSizeEx(f.h,&size))return GetLastError();
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))||info.nNumberOfLinks!=1)return ERROR_ACCESS_DENIED;
    if(size.QuadPart<0||size.QuadPart>kBytes)return ERROR_FILE_TOO_LARGE;bytes.resize(static_cast<size_t>(size.QuadPart));DWORD n=0;
    if(!ReadFile(f.h,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr))return GetLastError();return n==bytes.size()?0:ERROR_HANDLE_EOF;
}
void Text(Value& v,const char* name,const std::wstring& text){v.set(name,json::Utf8(text));}
void Num(Value& v,const char* name,uint64_t n){v.set(name,std::to_string(n));}
Value Encode(const History& h) {
    auto v=Value::Object();v.set("schema",1);Num(v,"unattributed",h.unattributed_events);Text(v,"cutoff",h.ignore_before_utc);v.set("partial",h.partial);v.set("retention",h.retention_limited);
    auto rows=Value::Array();for(const auto& r:h.records){auto x=Value::Object();Text(x,"app",r.app_basename);Num(x,"image",r.app_image_id);Num(x,"count",r.count);x.set("state",static_cast<int>(r.state));x.set("reason",static_cast<int>(r.reason));x.set("action",static_cast<int>(r.action));x.set("certainty",static_cast<int>(r.certainty));x.set("operation",static_cast<int>(r.operation));Text(x,"first",r.first_utc);Text(x,"last",r.last_utc);x.set("resolved",r.resolved_by_success);x.set("later",r.later_success);x.set("producer",r.last_producer);Num(x,"seq",r.last_sequence);rows.push(std::move(x));}v.set("records",std::move(rows));
    auto cursors=Value::Array();for(const auto& c:h.cursors){auto x=Value::Object();x.set("producer",c.producer);Num(x,"seq",c.sequence);Text(x,"utc",c.last_utc);cursors.push(std::move(x));}v.set("cursors",std::move(cursors));
    auto ids=Value::Array();for(const auto& i:h.identities){auto x=Value::Object();x.set("producer",i.producer);x.set("source",i.source);x.set("session",i.session);Num(x,"pid",i.pid);Num(x,"creation",i.creation_time);Num(x,"image",i.image_id);Text(x,"app",i.basename);x.set("ambiguous",i.ambiguous);ids.push(std::move(x));}v.set("identities",std::move(ids));return v;
}
bool Keys(const Value& v,std::initializer_list<std::string_view> keys){if(!v.is_object()||v.size()!=keys.size())return false;for(const auto key:keys)if(!v.find(key))return false;return true;}
bool EnumValue(const Value& v,const char* key,int maximum){const auto* p=v.find(key);int64_t n=0;return p&&p->get_int(n,0,maximum);}
bool Decode(const Value& v,History& h) {
    if(!Keys(v,{"schema","unattributed","cutoff","partial","retention","records","cursors","identities"})||Number(v,"schema")!=1)return false;
    const auto* unattributed=v.find("unattributed");uint64_t parsed=0; if(!unattributed->is_string())return false; const auto& raw=unattributed->as_string();const auto conversion=std::from_chars(raw.data(),raw.data()+raw.size(),parsed);if(raw.empty()||conversion.ec!=std::errc{}||conversion.ptr!=raw.data()+raw.size())return false;h.unattributed_events=parsed;h.ignore_before_utc=json::Wide(v.get_string("cutoff"));if(!h.ignore_before_utc.empty()&&!Utc(h.ignore_before_utc))return false;
    if(!v.find("partial")->is_bool()||!v.find("retention")->is_bool())return false;h.partial=v.get_bool("partial");h.retention_limited=v.get_bool("retention");
    const auto* rows=v.find("records"),*cursors=v.find("cursors"),*ids=v.find("identities");
    if(!rows->is_array()||rows->size()>kRecords||!cursors->is_array()||cursors->size()>kCursors||!ids->is_array()||ids->size()>kIdentities)return false;
    for(const auto& x:rows->items()) {
        if(!Keys(x,{"app","image","count","state","reason","action","certainty","operation","first","last","resolved","later","producer","seq"})||!EnumValue(x,"state",3)||!EnumValue(x,"reason",14)||!EnumValue(x,"action",5)||!EnumValue(x,"certainty",1)||!EnumValue(x,"operation",2)||!x.find("resolved")->is_bool()||!x.find("later")->is_bool())return false;
        Record r;r.app_basename=json::Wide(x.get_string("app"));r.app_image_id=Number(x,"image");r.count=Number(x,"count");r.state=static_cast<State>(Number(x,"state"));r.reason=static_cast<Reason>(Number(x,"reason"));r.action=static_cast<Action>(Number(x,"action"));r.certainty=static_cast<Certainty>(Number(x,"certainty"));r.operation=static_cast<Operation>(Number(x,"operation"));r.first_utc=json::Wide(x.get_string("first"));r.last_utc=json::Wide(x.get_string("last"));r.resolved_by_success=x.get_bool("resolved");r.later_success=x.get_bool("later");r.last_producer=x.get_string("producer");r.last_sequence=Number(x,"seq");
        if(!Basename(r.app_basename)||!r.app_image_id||!r.count||!Utc(r.first_utc)||!Utc(r.last_utc)||r.first_utc>r.last_utc||!Token(r.last_producer)||!r.last_sequence||r.action!=Advice(r.reason))return false;if(std::any_of(h.records.begin(),h.records.end(),[&](const Record& old){return old.app_image_id==r.app_image_id&&old.reason==r.reason;}))return false;h.records.push_back(std::move(r));
    }
    for(const auto& x:cursors->items()){if(!Keys(x,{"producer","seq","utc"}))return false;Cursor c{x.get_string("producer"),Number(x,"seq"),json::Wide(x.get_string("utc"))};if(!Token(c.producer)||!c.sequence||!Utc(c.last_utc)||std::any_of(h.cursors.begin(),h.cursors.end(),[&](const Cursor& old){return old.producer==c.producer;}))return false;h.cursors.push_back(std::move(c));}
    for(const auto& x:ids->items()){if(!Keys(x,{"producer","source","session","pid","creation","image","app","ambiguous"}))return false;Identity i;i.producer=x.get_string("producer");i.source=x.get_string("source");i.session=x.get_string("session");const auto pid=Number(x,"pid");if(!pid||pid>0xffffffff)return false;i.pid=static_cast<uint32_t>(pid);i.creation_time=Number(x,"creation");i.image_id=Number(x,"image");i.basename=json::Wide(x.get_string("app"));if(!x.find("ambiguous")->is_bool())return false;i.ambiguous=x.get_bool("ambiguous");if(!i.creation_time||!i.image_id||!Basename(i.basename)||(i.source!="dialog_auto"&&i.source!="managed_runtime"&&i.source!="dialog_admin_auto")||!Token(i.session)||i.producer!=Base(i)+"|"+std::to_string(i.creation_time))return false;if(std::any_of(h.identities.begin(),h.identities.end(),[&](const Identity& old){return old.producer==i.producer;}))return false;h.identities.push_back(std::move(i));}
    return true;
}
}
static CollectionResult ApplyInto(History& h,std::string_view text) {
    h.partial=h.retention_limited||h.recovered_backup;
    CollectionResult result;if(text.size()>768*1024){result.error=ERROR_FILE_TOO_LARGE;return result;}
    std::vector<Event> events;size_t lines=0;
    while(!text.empty()&&++lines<=2048) {
        const auto end=text.find('\n');if(end==text.npos){result.partial=true;break;}auto line=text.substr(0,end);text.remove_prefix(end+1);if(line.empty())continue;
        Value v;if(!json::Parse(line,v,nullptr,{8192,8})||!v.is_object()){++result.invalid;result.partial=true;continue;}
        if(v.get_string("event")=="dialog_log_collection") {
            const auto directory_error=Number(v,"directory_error");
            const bool absent=directory_error==ERROR_FILE_NOT_FOUND||directory_error==ERROR_PATH_NOT_FOUND;
            for(const auto* key:{"directory_error","enumeration_error","last_read_error"}) {
                const auto error=Number(v,key);if(error&&!(std::string_view(key)=="directory_error"&&absent))result.error=static_cast<DWORD>(error);
            }
            result.cancelled=result.cancelled||v.get_bool("cancelled");
            result.partial=result.partial||(v.get_bool("incomplete")&&!absent);
            continue;
        }
        if(v.get_string("kind")!="dialog_log_excerpt")continue;const auto* data=v.find("data");if(!data||!data->is_object())continue;
        Event e;e.data=*data;e.name=v.get_string("event");e.source=v.get_string("source");e.session=v.get_string("session");const auto pid=Number(v,"pid");if(!pid||pid>0xffffffff||!Token(e.session)) {++result.invalid;continue;}e.pid=static_cast<uint32_t>(pid);
        e.sequence=Number(v,"seq");e.utc=json::Wide(v.get_string("utc"));
        if(e.source=="dialog_runtime") {e.source="managed_runtime";const auto session=Number(e.data,"source_session");e.sequence=Number(e.data,"source_event_sequence");if(session)e.session=std::to_string(e.pid)+"-"+std::to_string(session);}
        if(e.source=="managed_runtime") {if(!e.sequence)e.sequence=Number(e.data,"event_sequence");if(!Utc(e.utc))e.utc=UtcFiletime(Number(e.data,"event_time_filetime"));}
        if (e.source == "dialog_admin_auto") {
            const auto target = Number(e.data, "target_pid"), created = Number(e.data, "target_creation_time_filetime");
            if (e.name != "admin_auto_target_result" || !target || target > MAXDWORD || !created) { ++result.invalid; continue; }
            e.session = std::to_string(e.pid) + "-" + e.session + "-" + std::to_string(created);
            e.pid = static_cast<uint32_t>(target);
            e.data.set("identity_revision", 2); e.data.set("creation_time_filetime", std::to_string(created));
        }
        if(e.source!="dialog_auto"&&e.source!="managed_runtime"&&e.source!="dialog_admin_auto")continue;
        if(!Utc(e.utc)){++result.invalid;result.partial=true;continue;}e.base=e.source+"|"+std::to_string(e.pid)+"|"+e.session;
        auto identity=ParseIdentity(e);if(!identity.producer.empty()) {
            auto existing=std::find_if(h.identities.begin(),h.identities.end(),[&](const Identity& i){return i.producer==identity.producer;});
            if(existing==h.identities.end())h.identities.push_back(std::move(identity));
            else if(existing->image_id!=identity.image_id||existing->basename!=identity.basename){existing->ambiguous=true;result.partial=true;}
        }
        events.push_back(std::move(e));
    }
    if(!text.empty())result.partial=true;
    auto request=[](const Event& e) {const auto value=e.data.get_string("request");return !value.empty()?value:std::to_string(Number(e.data,"request"));};
    for(auto& e:events) if(e.name=="dialog_show_terminal"&&e.data.get_string("dialog_kind").empty()) {
        for(const auto& begin:events) if(begin.name=="dialog_show_begin"&&begin.base==e.base&&request(begin)==request(e)&&request(e)!="0"&&begin.sequence<e.sequence) {
            const auto kind=begin.data.get_string("dialog_kind");if(kind=="open"||kind=="save")e.data.set("dialog_kind",kind);
        }
    }
    std::sort(events.begin(),events.end(),[](const Event& a,const Event& b){return a.utc!=b.utc?a.utc<b.utc:a.base!=b.base?a.base<b.base:a.sequence<b.sequence;});
    Trim(h);
    for(const auto& e:events) {
        if(e.name=="dialog_identity"||e.name=="dialog_show_begin")continue;
        State state=State::Unknown;Reason reason=Reason::Unknown;const bool issue=Classify(e,state,reason);
        if(!issue&&e.name!="dialog_show_terminal")continue;
        const Identity* identity=nullptr;bool conflict=false;for(const auto& i:h.identities)if(Base(i)==e.base){if(identity)conflict=true;identity=&i;}
        const auto unknown="unknown|"+e.base;
        if(conflict||!identity||!identity->image_id||identity->ambiguous) {if(issue&&Consume(h,unknown,e.sequence,e.utc)&&h.unattributed_events!=(std::numeric_limits<uint64_t>::max)())++h.unattributed_events;if(!e.sequence)result.partial=true;continue;}
        const auto prior_unknown=std::find_if(h.cursors.begin(),h.cursors.end(),[&](const Cursor& c){return c.producer==unknown;});
        if(prior_unknown!=h.cursors.end()&&e.sequence<=prior_unknown->sequence)continue;
        if(!Consume(h,identity->producer,e.sequence,e.utc))continue;
        if(!issue)continue;
        if(state==State::Success)for(auto& r:h.records)if(r.app_image_id==identity->image_id&&r.state!=State::Success&&e.utc>=r.last_utc){if(r.last_producer==identity->producer&&e.sequence>r.last_sequence)r.resolved_by_success=true;else if(e.utc>r.last_utc)r.later_success=true;}
        const auto kind=e.data.get_string("dialog_kind");const auto op=e.name!="dialog_show_terminal"?Operation::Unknown:kind=="open"?Operation::Open:kind=="save"?Operation::Save:Operation::Unknown;
        auto it=std::find_if(h.records.begin(),h.records.end(),[&](const Record& r){return r.app_image_id==identity->image_id&&r.reason==reason;});
        if(it==h.records.end()){Record r;r.app_basename=identity->basename;r.app_image_id=identity->image_id;r.reason=reason;r.action=Advice(reason);r.first_utc=e.utc;r.last_utc=e.utc;r.operation=op;h.records.push_back(std::move(r));it=h.records.end()-1;}
        if(it->count!=(std::numeric_limits<uint64_t>::max)())++it->count;
        it->first_utc=(std::min)(it->first_utc,e.utc);if(it->operation!=op)it->operation=Operation::Unknown;
        if(e.utc>=it->last_utc){it->last_utc=e.utc;it->state=state;it->certainty=reason==Reason::Unknown?Certainty::Incomplete:Certainty::Observed;it->last_producer=identity->producer;it->last_sequence=e.sequence;it->resolved_by_success=false;it->later_success=false;}
        ++result.added;
    }
    Trim(h);h.partial=h.partial||result.partial;result.partial=h.partial;return result;
}
CollectionResult ApplyLocalExcerpt(History& history,std::string_view text) {
    History next=history;auto result=ApplyInto(next,text);
    if(!result.cancelled)history=std::move(next);else result.added=0;
    return result;
}
CollectionResult CollectTakeoverHistory(const std::wstring& runtime,const std::vector<std::wstring>& managed,History& h,const std::function<bool()>& cancelled) {
    try {
    if(cancelled&&cancelled()){CollectionResult r;r.cancelled=true;return r;}
    std::string excerpt=dialog_diagnostics::CollectLocalTakeoverExcerpt(runtime,cancelled);size_t count=0;
    for(const auto& directory:managed){if(++count>4)break;if(cancelled&&cancelled()){CollectionResult r;r.cancelled=true;return r;}excerpt+=dialog_diagnostics::CollectLocalTakeoverExcerpt(directory,cancelled);}
    if(cancelled&&cancelled()){CollectionResult r;r.cancelled=true;return r;}
    auto result=ApplyLocalExcerpt(h,excerpt);if(managed.size()>4&&!result.cancelled)result.partial=h.partial=true;return result;
    }catch(...){CollectionResult r;r.cancelled=true;r.error=ERROR_GEN_FAILURE;return r;}
}
DWORD LoadHistory(const std::wstring& directory,History& out) noexcept {
    out={};try{
        std::wstring root;Handle handle;DWORD error=Directory(directory,root,handle);if(error)return error;
        std::string bytes;error=Read(root+L"\\takeover-history.json",bytes);
        auto parse=[&](const std::string& input,History& h){Value v;return json::Parse(input,v,nullptr,{kBytes,8})&&Decode(v,h);};
        History next;if(!error&&parse(bytes,next)){out=std::move(next);return 0;}
        if(error&&error!=ERROR_FILE_NOT_FOUND)return error;
        const DWORD original=error?error:ERROR_INVALID_DATA;
        std::string backup;const DWORD backup_error=Read(root+L"\\takeover-history.bak",backup);
        if(backup_error==ERROR_FILE_NOT_FOUND)return original==ERROR_FILE_NOT_FOUND?0:original;
        if(backup_error)return backup_error;History recovered;if(!parse(backup,recovered))return ERROR_INVALID_DATA;
        recovered.partial=true;recovered.recovered_backup=true;out=std::move(recovered);return 0;
    }catch(...){return ERROR_NOT_ENOUGH_MEMORY;}
}
DWORD SaveHistory(const std::wstring& directory,const History& h) noexcept {
    try {
        if(h.records.size()>kRecords||h.cursors.size()>kCursors||h.identities.size()>kIdentities)return ERROR_INVALID_DATA;
        const auto bytes=json::Serialize(Encode(h));if(bytes.size()>kBytes)return ERROR_FILE_TOO_LARGE;History checked;Value parsed;if(!json::Parse(bytes,parsed,nullptr,{kBytes,8})||!Decode(parsed,checked))return ERROR_INVALID_DATA;
        std::wstring root;Handle folder;DWORD error=Directory(directory,root,folder);if(error)return error;
        const auto current=root+L"\\takeover-history.json",backup=root+L"\\takeover-history.bak",temporary=root+L"\\takeover-history.tmp";
        std::string old;error=Read(current,old);if(!error&&old==bytes)return 0;if(error&&error!=ERROR_FILE_NOT_FOUND)return error;const bool existed=!error;
        std::string ignored;const DWORD backup_error=Read(backup,ignored);if(backup_error&&backup_error!=ERROR_FILE_NOT_FOUND)return backup_error;
        CurrentUserSecurityAttributes security;if(!security)return ERROR_ACCESS_DENIED;
        Handle file{CreateFileW(temporary.c_str(),GENERIC_READ|GENERIC_WRITE,0,security.get(),OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
        if(file.h==INVALID_HANDLE_VALUE)return GetLastError();BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file.h,&info))return GetLastError();if(info.nNumberOfLinks!=1||(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)))return ERROR_ACCESS_DENIED;
        LARGE_INTEGER start{};if(!SetFilePointerEx(file.h,start,nullptr,FILE_BEGIN)||!SetEndOfFile(file.h))return GetLastError();DWORD written=0;if(!WriteFile(file.h,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.h))return GetLastError()?GetLastError():ERROR_WRITE_FAULT;
        CloseHandle(file.h);file.h=INVALID_HANDLE_VALUE;
        if(existed) {
            if(!ReplaceFileW(current.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr))return GetLastError();
        } else if(!MoveFileExW(temporary.c_str(),current.c_str(),MOVEFILE_WRITE_THROUGH))return GetLastError();
        return 0;
    }catch(...){return ERROR_NOT_ENOUGH_MEMORY;}
}
}
