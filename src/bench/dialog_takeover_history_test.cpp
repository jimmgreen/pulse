#include "../common/dialog_takeover_history.h"
#include "../common/dialog_log_excerpt.h"
#include "../common/json_value.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
using namespace pulse::dialog_history;
namespace fs=std::filesystem;
namespace {
int checks=0,failures=0;
void Check(bool yes,const char* name){++checks;if(!yes)++failures;std::printf("[%s] %s\n",yes?"PASS":"FAIL",name);}
pulse::json::Value Data(bool identity=true,uint64_t image=123,uint64_t creation=134000000000000001ull) {
    auto d=pulse::json::Value::Object();
    if(identity){d.set("identity_revision",2);d.set("creation_time_filetime",std::to_string(creation));d.set("app_instance_id",std::to_string(image));d.set("app_basename","example.exe");}
    return d;
}
std::string Line(uint64_t seq,std::string name,pulse::json::Value data,std::string session="100",std::string utc="2026-11-11T10:00:00.000Z") {
    auto v=pulse::json::Value::Object();v.set("schema",1);v.set("kind","dialog_log_excerpt");v.set("source","dialog_auto");v.set("pid",42);v.set("session",session);v.set("seq",std::to_string(seq));v.set("utc",utc);v.set("event",name);v.set("data",data);return pulse::json::Serialize(v)+"\n";
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==3&&wcscmp(argv[1],L"--collect")==0) {
        History actual;auto r=CollectTakeoverHistory(argv[2],{},actual);
        Check(!r.error&&!r.cancelled,"real global collector completes");
        Check(actual.records.size()==1&&actual.records[0].state==State::Unknown&&actual.records[0].count==6,"real accepted without UI proof stays unknown; six cancellations ignored");
        const auto again=CollectTakeoverHistory(argv[2],{},actual);
        Check(!again.error&&again.added==0&&actual.records.size()==1&&actual.records[0].count==6,"real global replay deduplicates");
        std::printf("Records: %zu unattributed: %llu partial: %d\n",actual.records.size(),actual.unattributed_events,actual.partial);
        std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
    }
    if(argc!=2)return 2;
    const fs::path root=fs::absolute(argv[1]);if(fs::exists(root))return 3;fs::create_directories(root);
    History h;auto d=Data();d.set("terminal_status","native");d.set("native_fallback",true);d.set("fallback_reason","contract");d.set("dialog_kind","open");
    const auto fallback=Line(2,"dialog_show_terminal",d);
    auto result=ApplyLocalExcerpt(h,fallback);
    Check(result.added==1&&h.records.size()==1&&h.records[0].state==State::Fallback,"observed terminal fallback creates one attributed record");
    ApplyLocalExcerpt(h,fallback);Check(h.records[0].count==1,"duplicate polling does not count twice");
    Check(SaveHistory(root.wstring(),h)==0,"bounded history saves");History restored;Check(LoadHistory(root.wstring(),restored)==0&&restored.records.size()==1,"bounded history reloads");ApplyLocalExcerpt(restored,fallback);Check(restored.records[0].count==1,"restart replay preserves dedup cursor");
    auto accepted=Data();accepted.set("terminal_status","accepted");accepted.set("ui_shown_known",true);accepted.set("ui_shown",true);
    ApplyLocalExcerpt(h,Line(3,"dialog_show_terminal",accepted));Check(h.records.size()==2&&h.records[0].resolved_by_success,"synthetic UI proof success resolves same-instance fallback");
    auto cancelled=Data();cancelled.set("terminal_status","cancelled");ApplyLocalExcerpt(h,Line(4,"dialog_show_terminal",cancelled));Check(h.records.size()==2&&h.records[1].count==1,"cancel only advances cursor");
    {
        History forget=h;const auto first=forget.records[0];
        Check(ForgetRecords(forget,false,first.app_image_id,first.reason)==1&&forget.records.size()==1&&forget.records[0].reason!=first.reason,"deleting one record keeps the others");
        Check(ForgetRecords(forget,false,first.app_image_id+1,first.reason)==0&&forget.records.size()==1,"an unknown record deletes nothing");
        ApplyLocalExcerpt(forget,fallback);Check(forget.records.size()==1,"a deleted record does not return from events already read");
        Check(ForgetRecords(forget,true)==1&&forget.records.empty()&&!forget.cursors.empty(),"clearing removes every record but keeps the read cursors");
    }
    History shown;ApplyLocalExcerpt(shown,fallback);
    auto shown_cancel=Data();shown_cancel.set("terminal_status","cancelled");shown_cancel.set("ui_shown_known",true);shown_cancel.set("ui_shown",true);
    ApplyLocalExcerpt(shown,Line(3,"dialog_show_terminal",shown_cancel));
    Check(shown.records.size()==2&&shown.records[0].resolved_by_success&&shown.records[1].state==State::Success&&static_cast<unsigned>(shown.records[1].reason)==13&&shown.records[1].action==Action::None,"shown cancellation resolves old failure without claiming file result");
    auto not_shown=shown_cancel;not_shown.set("ui_shown",false);History unseen;ApplyLocalExcerpt(unseen,Line(1,"dialog_show_terminal",not_shown));Check(unseen.records.empty(),"cancel before UI is not takeover success");
    auto shown_closed=shown_cancel;shown_closed.set("terminal_status","closed");History closed;ApplyLocalExcerpt(closed,Line(1,"dialog_show_terminal",shown_closed));Check(closed.records.size()==1&&static_cast<unsigned>(closed.records[0].reason)==13,"shown programmatic close is not file acceptance");
    auto shown_failed=shown_cancel;shown_failed.set("terminal_status","failed");History failed;ApplyLocalExcerpt(failed,Line(1,"dialog_show_terminal",shown_failed));Check(failed.records.size()==1&&failed.records[0].state==State::Failure,"failure after UI remains failure");
    Check(failed.records.size()==1&&failed.records[0].reason==Reason::ShownThenFailed&&failed.records[0].action==Action::RetryDialog&&failed.records[0].certainty==Certainty::Observed,"failure after visible UI gets its own reason, not unknown");
    {const auto sf_dir=root/L"shown_failed";fs::create_directories(sf_dir);History sf_loaded;Check(SaveHistory(sf_dir.wstring(),failed)==0&&LoadHistory(sf_dir.wstring(),sf_loaded)==0&&sf_loaded.records.size()==1&&sf_loaded.records[0].reason==Reason::ShownThenFailed,"shown-then-failed reason persists");}
    auto hidden_failed=shown_failed;hidden_failed.set("ui_shown",false);History hidden;ApplyLocalExcerpt(hidden,Line(1,"dialog_show_terminal",hidden_failed));Check(hidden.records.size()==1&&hidden.records[0].reason==Reason::Unknown,"failure without visible UI stays unknown");
    {
        // The provider accepted the contract, then Prepare failed before any UI.
        // The contract reason name is still attached; it must not turn this into
        // an unsupported request or a generic native fallback.
        auto prepare=Data();prepare.set("terminal_status","native");prepare.set("native_fallback",true);prepare.set("reason_name","UnsupportedOptions");prepare.set("fallback_reason","PROVIDER_PREPARE_FAILED_BEFORE_UI");prepare.set("hr","2147500037");
        History p;ApplyLocalExcerpt(p,Line(1,"dialog_show_terminal",prepare));
        Check(p.records.size()==1&&p.records[0].reason==Reason::ProviderFailure&&p.records[0].state==State::Fallback,"prepare failure before UI is a provider failure, not unsupported or plain fallback");
        auto start=prepare;start.set("fallback_reason","PROVIDER_START_FAILED_BEFORE_UI");start.set("reason_name","None");
        History q;ApplyLocalExcerpt(q,Line(1,"dialog_show_terminal",start));
        Check(q.records.size()==1&&q.records[0].reason==Reason::ProviderFailure,"start failure before UI stays a provider failure");
    }
    const auto shown_dir=root/L"shown-cancel";fs::create_directories(shown_dir);History shown_loaded;
    Check(SaveHistory(shown_dir.wstring(),shown)==0&&LoadHistory(shown_dir.wstring(),shown_loaded)==0&&shown_loaded.records.size()==2&&static_cast<unsigned>(shown_loaded.records[1].reason)==13,"shown cancellation persists with distinct reason");
    {
        auto helper_line = [](uint64_t sequence, uint32_t pid, uint64_t image, DWORD error, const char* stage = nullptr) {
            auto data = Data(true, image); data.set("target_pid", static_cast<uint64_t>(pid));
            data.set("target_creation_time_filetime", "134000000000000004"); data.set("error", static_cast<uint64_t>(error)); data.set("ready", error ? 0 : 1);
            if (stage) data.set("attach_stage_name", stage);
            auto line = Line(sequence, "admin_auto_target_result", data); pulse::json::Value event;
            pulse::json::Parse(line, event); event.set("source", "dialog_admin_auto");
            return pulse::json::Serialize(event) + "\n";
        };
        History admin;
        ApplyLocalExcerpt(admin, helper_line(1, 1234, 400, ERROR_ACCESS_DENIED));
        ApplyLocalExcerpt(admin, helper_line(2, 1235, 401, ERROR_TIMEOUT));
        Check(admin.records.empty(), "process startup failures do not create dialog history");
        ApplyLocalExcerpt(admin, helper_line(4, 1235, 401, 0x80070005u, "runtime_status"));
        ApplyLocalExcerpt(admin, helper_line(5, 1235, 401, ERROR_TIMEOUT, "runtime_status_unavailable"));
        ApplyLocalExcerpt(admin, helper_line(3, 1234, 400, 0));
        Check(admin.records.empty(), "loader status and readiness remain diagnostics only");
        History old; ApplyLocalExcerpt(old, fallback);
        auto obsolete = old.records.front();
        obsolete.app_image_id = 400; obsolete.reason = Reason::AdminAutoEntryFailed;
        obsolete.action = Action::CheckIntegrationSettings; obsolete.operation = Operation::Unknown;
        old.records.push_back(obsolete);
        const auto dir = root / L"admin-auto"; fs::create_directories(dir); History loaded;
        Check(SaveHistory(dir.wstring(), old) == 0 && LoadHistory(dir.wstring(), loaded) == 0 &&
            loaded.records.size() == 1 && loaded.records.front().reason == old.records.front().reason &&
            loaded.cursors.size() == old.cursors.size(),
            "migration removes old loader failures but preserves real dialog and cursors");

    }
    History nativecancel;auto nd=d;nd.set("terminal_status","cancelled");nd.set("hr",static_cast<uint64_t>(0x800704c7u));ApplyLocalExcerpt(nativecancel,Line(1,"dialog_show_terminal",nd));Check(nativecancel.records.size()==1&&nativecancel.records[0].state==State::Fallback,"native fallback followed by user cancel remains fallback never failure");
    History gate;auto gd=Data();gd.set("reason_name","REGISTERED_MODULE_MISMATCH");ApplyLocalExcerpt(gate,Line(1,"activation_gate",gd));Check(gate.records.empty(),"activation alone is not a dialog request");
    History legacy;auto missing=Data(false);missing.set("terminal_status","failed");ApplyLocalExcerpt(legacy,Line(1,"dialog_show_terminal",missing));ApplyLocalExcerpt(legacy,Line(1,"dialog_show_terminal",missing));Check(legacy.records.empty()&&legacy.unattributed_events==1,"legacy identity remains anonymous and deduplicated");
    History reused;ApplyLocalExcerpt(reused,fallback);auto other=Data(true,999,134000000000000002ull);other.set("terminal_status","failed");ApplyLocalExcerpt(reused,Line(1,"dialog_show_terminal",other,"101"));Check(reused.records.size()==2&&reused.records[1].app_image_id==999,"PID reuse different creation and session never borrows identity");
    History newprocess;ApplyLocalExcerpt(newprocess,fallback);auto later=accepted;later.set("creation_time_filetime","134000000000000003");ApplyLocalExcerpt(newprocess,Line(1,"dialog_show_terminal",later,"102","2026-11-11T10:01:00.000Z"));Check(newprocess.records.size()==2&&!newprocess.records[0].resolved_by_success&&newprocess.records[0].later_success,"new process success is not global repair proof");
    History privacy;auto bad=Data();bad.set("app_basename","C:\\secret\\example.exe");bad.set("terminal_status","failed");ApplyLocalExcerpt(privacy,Line(1,"dialog_show_terminal",bad));Check(privacy.records.empty()&&privacy.unattributed_events==1,"path masquerading as basename rejected");
    auto summary=pulse::json::Value::Object();summary.set("event","dialog_log_collection");summary.set("cancelled",true);summary.set("incomplete",true);
    History transaction;auto cr=ApplyLocalExcerpt(transaction,fallback+pulse::json::Serialize(summary)+"\n");Check(cr.cancelled&&transaction.records.empty()&&transaction.cursors.empty(),"top-level cancellation applies no events or cursors");
    summary.set("cancelled",false);summary.set("last_read_error",5);auto er=ApplyLocalExcerpt(transaction,pulse::json::Serialize(summary)+"\n");Check(er.error==5&&er.partial,"top-level collector read errors preserved");
    History absent;auto nr=CollectTakeoverHistory((root/L"absent").wstring(),{},absent);Check(!nr.error&&!nr.partial&&absent.records.empty(),"real missing log directory is normal empty history");
    auto stop=CollectTakeoverHistory(root.wstring(),{},absent,[]{return true;});Check(stop.cancelled&&absent.cursors.empty(),"real cancellation leaves state untouched");
    History fragment;auto fr=ApplyLocalExcerpt(fragment,fallback.substr(0,fallback.size()-1));Check(fr.partial&&fragment.records.empty(),"partial JSON line never becomes issue");
    History collision;ApplyLocalExcerpt(collision,fallback);auto collided=other;ApplyLocalExcerpt(collision,Line(3,"dialog_show_terminal",collided));Check(collision.records.size()==1&&collision.unattributed_events==1,"same PID session conflicting creation is not guessed");
    const auto stable=root/L"stable";fs::create_directories(stable);Check(SaveHistory(stable.wstring(),h)==0,"separate unchanged-write fixture saves");const auto stamp=fs::last_write_time(stable/L"takeover-history.json");Check(SaveHistory(stable.wstring(),h)==0&&fs::last_write_time(stable/L"takeover-history.json")==stamp,"unchanged poll skips persistence rewrite");
    const auto link=root/L"linked";fs::create_directories(link);const BOOL linked=CreateHardLinkW((link/L"takeover-history.json").c_str(),(stable/L"takeover-history.json").c_str(),nullptr);History unsafe;Check(linked&&LoadHistory(link.wstring(),unsafe)==ERROR_ACCESS_DENIED,"linked persistence input rejected");
    Check(SaveHistory(root.wstring(),h)==0,"atomic replace creates backup");
    fs::rename(root/L"takeover-history.json",root/L"retained-current.json");History recovered;Check(LoadHistory(root.wstring(),recovered)==0&&recovered.recovered_backup&&recovered.records.size()==1,"missing current recovers strictly validated backup");
    {std::ofstream out(root/L"takeover-history.bak",std::ios::binary|std::ios::trunc);out<<"broken";}
    History broken;Check(LoadHistory(root.wstring(),broken)==ERROR_INVALID_DATA,"damaged backup rejected");
    const auto logroot=root/L"logs";fs::create_directories(logroot);
    std::string raw="{\"schema\":1,\"utc\":\"2026-11-11T10:00:00.000Z\",\"session\":100,\"pid\":42,\"seq\":1,\"component\":\"dialog_runtime\",\"event\":\"dialog_identity\",\"data\":{\"identity_revision\":2,\"creation_time_filetime\":134000000000000001,\"app_instance_id\":123,\"app_basename\":\"example.exe\",\"selected_path\":\"private-document\"}}\n";
    {std::ofstream out(logroot/L"dialog_runtime-42.jsonl",std::ios::binary);out<<raw;}
    auto local=pulse::dialog_diagnostics::CollectLocalTakeoverExcerpt(logroot.wstring());auto exported=pulse::dialog_diagnostics::CollectDialogLogExcerpt(logroot.wstring());
    Check(local.find("example.exe")!=local.npos&&local.find("134000000000000001")!=local.npos,"local collector preserves basename and exact 64-bit creation");
    Check(exported.find("dialog_identity")!=exported.npos&&exported.find("example.exe")==exported.npos&&exported.find("private-document")==exported.npos&&local.find("private-document")==local.npos,"default global export removes basename and document path");
    HANDLE locked=CreateFileW((logroot/L"dialog_runtime-42.jsonl").c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);History unreadable;auto ur=CollectTakeoverHistory(logroot.wstring(),{},unreadable);Check(locked!=INVALID_HANDLE_VALUE&&ur.error==ERROR_SHARING_VIOLATION&&ur.partial,"real read sharing error becomes diagnostic warning");if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
    for(int i=0;i<10;++i){std::ofstream out(logroot/(L"dialog_auto-"+std::to_wstring(i)+L".jsonl"),std::ios::binary);out<<raw;}
    History limited;auto lr=CollectTakeoverHistory(logroot.wstring(),{},limited);Check(lr.partial,"real file budget marks collection partial");
    std::printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;
}
