#include "resource_service.h"
#include "rename_service.h"
#include "resource_contract.h"
#include "resource_settings.h"
#include "resource_reference_store.h"
#include "audit_log.h"
#include "context.h"
#include "view_mode_mapping.h"
#include "transfer_status.h"
#include "../app_runtime.h"
#include "../session.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace pulse::extensions {
namespace {
ResourceReferenceStore& References() {
    static ResourceReferenceStore store(HostAuthorizationRegistry());
    return store;
}
std::string ReferenceFor(const CallerBinding& caller, const app::Tab& tab, size_t index, bool cursor) {
    return References().Issue(caller, tab.snapshot, tab.current_path, index, cursor);
}
std::optional<size_t> Resolve(const std::string& id, const CallerBinding& caller, const app::Tab& tab, bool cursor) {
    return References().Resolve(id, caller, tab.snapshot, tab.current_path, cursor);
}
json::Value Entry(const fs::DirEntry& entry) {
    auto result=json::Value::Object();
    result.set("name",json::Utf8(entry.name)); result.set("sizeBytes",std::to_string(entry.size));
    result.set("directory",entry.is_dir);result.set("reparsePoint",entry.is_reparse);
    result.set("attributes",static_cast<int64_t>(entry.attrs));
    const uint64_t modified=(static_cast<uint64_t>(entry.mtime.dwHighDateTime)<<32)|entry.mtime.dwLowDateTime;
    result.set("modifiedFileTime",std::to_string(modified));
    return result;
}
bool Directory(const app::Tab* tab) {
    return tab && !tab->loading && tab->snapshot && !tab->search_entries && !tab->content_results &&
        tab->current_path==tab->snapshot_path && !tab->current_path.empty() &&
        ((tab->current_path.size()>2 && tab->current_path[1]==L':') || tab->current_path.starts_with(L"\\\\"));
}
struct Pending {
    CallerBinding caller;
    HWND owner=nullptr,window=nullptr;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::optional<json::Value> result;
    ResourceService::Completion complete;
    uint64_t deadline=0;
    explicit Pending(CallerBinding value):caller(std::move(value)){}
};
std::unordered_map<HWND,std::shared_ptr<Pending>> pending;
void Retire(const std::shared_ptr<Pending>& job) {
    job->cancelled=true;
    const HWND window=std::exchange(job->window,nullptr);
    if(window){pending.erase(window);DestroyWindow(window);}
}
LRESULT CALLBACK PendingProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_TIMER) {
        const auto found=pending.find(window);
        if(found==pending.end())return 0;
        const auto job=found->second;
        if(job->cancelled || !IsWindow(job->owner) || !ValidateCallerBinding(job->caller)) {
            Retire(job);return 0;
        }
        std::optional<json::Value> result;
        {std::lock_guard lock(job->mutex);result.swap(job->result);}
        if(!result && GetTickCount64()>=job->deadline)result=ResourceFailure("timeout");
        if(result) {
            auto callback=std::move(job->complete);Retire(job);
            if(callback)callback(std::move(*result));
        }
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
std::optional<size_t> ResolveResourceEntry(const CallerBinding& caller, const std::string& entry, AppState& owner) {
    if (!ValidateCallerBinding(caller) || caller.Owner().key != reinterpret_cast<uintptr_t>(owner.hwnd))
        return std::nullopt;
    const auto* tab = ActiveTab(owner);
    if (!Directory(tab) || tab->pending_generation) return std::nullopt;
    return Resolve(entry, caller, *tab, false);
}
ResourceService::Cancel ResourceService::Call(AppState& owner,const CallerLease& lease,const json::Value& input,Completion complete) {
    if(!complete)return {};
    if(!ValidateCaller(lease)){complete(ResourceFailure("invalid_caller"));return {};}
    if(!owner.hwnd || lease.owner.key!=reinterpret_cast<uintptr_t>(owner.hwnd)) {complete(ResourceFailure("wrong_owner"));return {};}
    const auto binding=CaptureCallerBinding(lease.package.slot,lease.owner.key);
    if(!binding || binding->Package()!=lease.package || binding->Owner()!=lease.owner) {complete(ResourceFailure("invalid_caller"));return {};}
    const auto finish=[&](json::Value result) {complete(std::move(result));};
    const auto request=ParseResourceRequest(input);
    if(!request){finish(ResourceFailure("invalid_request"));return {};}
    if (request->operation == ResourceOperation::RenamePrepare || request->operation == ResourceOperation::RenameCommit) {
        return request->operation == ResourceOperation::RenamePrepare
            ? RenameService::Prepare(owner, lease, request->rename_items, std::move(complete))
            : RenameService::Commit(owner, lease, request->plan_id, std::move(complete));
    }
    if(request->operation==ResourceOperation::Context) {
        const auto context=CapturePanelContext(owner);auto data=json::Value::Object();
        data.set("directory",json::Utf8(context.directory));data.set("selectionCount",static_cast<int64_t>(context.selection_count));
        data.set("selectionName",json::Utf8(context.selection_name));
        const auto* tab = ActiveTab(owner);
        const auto mode = tab ? SdkViewModeName(tab->view_mode) : std::nullopt;
        data.set("viewMode", mode ? json::Value(std::string(*mode)) : json::Value{});
        data.set("loading",ActiveTab(owner)&&ActiveTab(owner)->loading);finish(ResourceSuccess(std::move(data)));return {};
    }
    if(request->operation==ResourceOperation::List || request->operation==ResourceOperation::Stat) {
        const auto* tab=ActiveTab(owner);
        if(!Directory(tab)){finish(ResourceFailure(tab&&tab->loading?"loading":"unsupported_context"));return {};}
        size_t offset=0;
        if(request->operation==ResourceOperation::Stat || !request->cursor.empty()) {
            const auto ref=Resolve(request->operation==ResourceOperation::Stat?request->entry:request->cursor,*binding,*tab,request->operation==ResourceOperation::List);
            if(!ref){finish(ResourceFailure("stale_reference"));return {};}
            offset=*ref;
        }
        if(request->operation==ResourceOperation::Stat) {
            if(offset>=tab->snapshot->size()){finish(ResourceFailure("stale_reference"));return {};}
            finish(ResourceSuccess(Entry((*tab->snapshot)[offset])));return {};
        }
        if(!References().HasCapacity(request->limit+1)){finish(ResourceFailure("reference_budget"));return {};}
        auto data=json::Value::Object(),items=json::Value::Array();
        const auto end=(std::min)(tab->snapshot->size(),offset+request->limit);
        for(size_t index=offset;index<end;++index) {
            auto value=Entry((*tab->snapshot)[index]);const auto reference=ReferenceFor(*binding,*tab,index,false);
            if(reference.empty()){finish(ResourceFailure("reference_budget"));return {};}
            value.set("entry",reference);items.push(std::move(value));
        }
        std::string next;
        if(end<tab->snapshot->size()) {
            next=ReferenceFor(*binding,*tab,end,true);
            if(next.empty()){finish(ResourceFailure("reference_budget"));return {};}
        }
        data.set("entries",std::move(items));data.set("nextCursor",next);data.set("total",static_cast<int64_t>(tab->snapshot->size()));
        finish(ResourceSuccess(std::move(data)));return {};
    }
    if(request->operation==ResourceOperation::TaskGet || request->operation==ResourceOperation::TaskCancel) {
        if (auto rename = RenameService::Task(owner, lease, request->task, request->operation == ResourceOperation::TaskCancel)) {
            finish(std::move(*rename)); return {};
        }
        auto task=owner.ops.QueryControlled(request->task,*binding);
        if(task.error!=transfer_plan::Error::None){finish(ResourceFailure("not_found"));return {};}
        if(request->operation==ResourceOperation::TaskCancel) {
            const bool accepted=owner.ops.CancelControlled(request->task,*binding);
            auto data=json::Value::Object();data.set("accepted",accepted);data.set("taskId",std::to_string(request->task));
            finish(ResourceSuccess(std::move(data)));return {};
        }
        auto data=json::Value::Object();data.set("taskId",std::to_string(request->task));
        data.set("cancelRequested",task.cancel_requested);data.set("terminal",task.result.has_value());
        data.set("completed",task.result&&task.result->complete);
        data.set("transferredBytes",std::to_string(task.progress.transferred_bytes));data.set("totalBytes",std::to_string(task.progress.total_bytes));
        auto items=json::Value::Array();
        if(task.result)for(const auto& item:task.result->items) {
            auto value=json::Value::Object();value.set("source",json::Utf8(item.source));value.set("destination",json::Utf8(item.destination));
            value.set("state",static_cast<int>(item.state));value.set("error",static_cast<int64_t>(item.error));
            value.set("cleanupError",static_cast<int64_t>(item.cleanup_error));items.push(std::move(value));
        }
        data.set("items",std::move(items));finish(ResourceSuccess(std::move(data)));return {};
    }
    if(request->operation==ResourceOperation::Audit) {finish(ResourceSuccess(ReadExtensionAudit(*binding,request->limit)));return {};}
    std::wstring isolated_root;
    if(owner.isolatedTest) {
        wchar_t buffer[32768]{};
        const auto length=GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",buffer,32768);
        if(!length||length>=32768||!std::filesystem::path(buffer).is_absolute()) {finish(ResourceFailure("invalid_test_profile"));return {};}
        isolated_root.assign(buffer,length);
    }
    if(pending.size()>=64){finish(ResourceFailure("request_budget"));return {};}
    auto job=std::make_shared<Pending>(*binding);job->owner=owner.hwnd;job->deadline=GetTickCount64()+30000;
    WNDCLASSW cls{};cls.lpfnWndProc=PendingProcedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"PulseExtensionResourceRequest";
    if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){finish(ResourceFailure("unavailable"));return {};}
    job->window=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,nullptr);
    if(!job->window){finish(ResourceFailure("unavailable"));return {};}
    pending.emplace(job->window,job);
    if(!SetTimer(job->window,1,30,nullptr)){Retire(job);finish(ResourceFailure("unavailable"));return {};}
    job->complete=std::move(complete);
    owner.worker.EnqueueIo([job,request=*request,isolated_root=std::move(isolated_root)] {
        const auto authorized=[&]{return !job->cancelled && ValidateCallerBinding(job->caller);};
        json::Value result;
        try {
            if(!authorized())return;
            const auto root=isolated_root.empty()?app::GetPulseDataDir():isolated_root;
            result=request.operation==ResourceOperation::SettingsGet
                ? ReadOwnSettings(root,job->caller.Package().id)
                : WriteOwnSetting(root,job->caller.Package().id,request.key,request.value,request.version,authorized);
        } catch(...) {result=ResourceFailure("io_error");}
        std::lock_guard lock(job->mutex);if(!job->cancelled)job->result=std::move(result);
    });
    return [job]{job->cancelled=true;};
}
void ResourceService::ReleaseOwner(uintptr_t owner) {
    RenameService::ReleaseOwner(owner);
    std::vector<std::shared_ptr<Pending>> retired;
    for(const auto& [window,job]:pending)if(job->caller.Owner().key==owner)retired.push_back(job);
    for(const auto& job:retired)Retire(job);
    References().ReleaseOwner(owner);
}
}
