#include "audit_log.h"
#include "audit_persistence.h"
#include <windows.h>
#include <deque>

namespace pulse::extensions {
namespace {
struct Event { CallerBinding caller; uint64_t sequence, time; std::string operation, outcome; };
std::deque<Event> events;
uint64_t next_sequence=0;
}
void RecordExtensionCall(const CallerBinding& caller,std::string_view operation,std::string_view outcome) {
    if (operation != "files.deleteCurrentSelection" && operation != "files.transfer" &&
        operation != "files.rename.commit") return;
    PersistExtensionAudit(caller.Package().id, operation, outcome);
    events.push_back({caller,++next_sequence,GetTickCount64(),std::string(operation.substr(0,128)),std::string(outcome.substr(0,128))});
    while(events.size()>1024) events.pop_front();
}
json::Value ReadExtensionAudit(const CallerBinding& caller,size_t limit) {
    auto result=json::Value::Array();
    if (!ValidateCallerBinding(caller)) return result;
    for(auto at=events.rbegin();at!=events.rend() && result.size()<limit && result.size()<128;++at) {
        if (at->caller.Package()!=caller.Package() || at->caller.Owner()!=caller.Owner() || !ValidateCallerBinding(at->caller)) continue;
        auto item=json::Value::Object(); item.set("sequence",std::to_string(at->sequence));
        item.set("monotonicMs",std::to_string(at->time));item.set("operation",at->operation);item.set("outcome",at->outcome);
        result.push(std::move(item));
    }
    return result;
}
}
