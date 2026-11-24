#include "app_internal.h"
#include "dialog_management_ui.h"
#include "dialog_api_probe.h"
#include "dialog_management_controller.h"
#include "../common/json_value.h"
#include "app_messages.h"
#include "app_commands.h"
#include "session.h"
#include "../ui/ui_renderer.h"
#include "shell_tag_registry.h"
#include "../ui/confirm_dialog.h"
#include "../ui/fluent_menu.h"
#include <initializer_list>
#include <future>
#include <thread>
#include <algorithm>
#include <shlobj.h>

namespace pulse {
struct DialogManagementUiState {
    std::shared_ptr<app::DialogManagementController> controller;
    app::DialogManagementResult snapshot;
    uint64_t generation = 0;
    DWORD error = ERROR_SUCCESS;
    bool busy = false, refreshing = false, scope_expanded = false, restoring = false, modal = false;
    bool restore_other_ok = true, retry_disable = false;
    std::future<DWORD> restore_tags;
    std::wstring notice;
    HANDLE api_probe = nullptr;
    dialog_diagnostics::ReproductionWindow feedback;
    bool exporting_feedback=false;
    ULONGLONG refreshed = 0, history_refreshed = 0;
    ~DialogManagementUiState() { if(api_probe) CloseHandle(api_probe); }
};
namespace {
using I=l10n::StringId;
uint64_t NowFileTime() {
    FILETIME now{};GetSystemTimeAsFileTime(&now);
    return (static_cast<uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
}
int DialogMenu(AppState& s,std::initializer_list<std::pair<int,I>> choices) {
    if(!EnsureMenu(s)) return 0;
    std::vector<ui::FluentMenuItem> items;
    for(const auto& [command,title]:choices) {
        ui::FluentMenuItem item;item.command=command;item.text=l10n::Get(title);items.push_back(std::move(item));
    }
    s.menu->SetTheme(s.darkMode,s.accentColor);
    POINT point{};GetCursorPos(&point);
    return s.menu->TrackPopup(point,std::move(items));
}
// "2026-11-11T12:00:00.000Z" -> UTC FILETIME; 0 when malformed.
uint64_t UtcFileTime(const std::wstring& iso) {
    SYSTEMTIME time{};
    unsigned year=0,month=0,day=0,hour=0,minute=0,second=0;
    if(swscanf_s(iso.c_str(),L"%4u-%2u-%2uT%2u:%2u:%2u",&year,&month,&day,&hour,&minute,&second)!=6) return 0;
    time.wYear=static_cast<WORD>(year);time.wMonth=static_cast<WORD>(month);time.wDay=static_cast<WORD>(day);
    time.wHour=static_cast<WORD>(hour);time.wMinute=static_cast<WORD>(minute);time.wSecond=static_cast<WORD>(second);
    FILETIME file{};
    if(!SystemTimeToFileTime(&time,&file)) return 0;
    return (static_cast<uint64_t>(file.dwHighDateTime)<<32)|file.dwLowDateTime;
}
// The program's friendly name and icon when the worker found it, else the file name.
void FillApplication(const app::DialogManagementResult& result,const std::wstring& basename,uint64_t image_id,
                     ui::WindowViewModel::DialogTakeoverRow& row) {
    row.application=basename;row.file=basename;
    if(const auto found=result.record_apps.find(image_id);found!=result.record_apps.end()) {
        if(!found->second.name.empty()) row.application=found->second.name;
        row.icon=found->second.icon;
    }
}
void FillHistoryVm(const app::DialogManagementResult& result,ui::WindowViewModel& vm) {
    vm.settings_dialog_records.clear();
    vm.settings_dialog_history_note=l10n::Get(result.history.records.empty() ? I::DialogHistoryEmpty : I::DialogHistoryNote);
    if(result.history.unattributed_events) vm.settings_dialog_history_note+=L"\n"+std::wstring(l10n::Get(I::DialogHistoryUnattributed));
    if(result.history.partial) vm.settings_dialog_history_note+=L"\n"+std::wstring(l10n::Get(I::DialogHistoryPartial));
    if(result.history_error) vm.settings_dialog_history_note+=L"\n"+std::wstring(l10n::Get(I::DialogHistoryStorageError));
    bool restart_group=false;
    if(result.restart_checked) {
        vm.settings_dialog_history_note+=L"\n"+std::wstring(l10n::Get(result.restart_inventory.records.empty() ? I::DialogRestartNone : I::DialogRestartNote));
        if(!result.restart_inventory.scan_complete) vm.settings_dialog_history_note+=L"\n"+std::wstring(l10n::Get(I::DialogRestartPartial));
        for(const auto& record:result.restart_inventory.records) {
            if(vm.settings_dialog_records.size()>=12) break;
            ui::WindowViewModel::DialogTakeoverRow row;
            FillApplication(result,record.app_basename,record.app_image_id,row);
            row.detail=l10n::Get(I::DialogRestartRequired);row.attention=true;
            row.meta=l10n::Get(I::DialogRecordRunningOld);
            row.status=l10n::Get(I::DialogRecordStatusRestart);row.tone=2;row.group=1;
            vm.settings_dialog_records.push_back(std::move(row));restart_group=true;
        }
    }
    // Programs the circuit breaker paused: system dialogs until they are retried.
    bool paused_group=false;
    FILETIME now_ft{};GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now=(static_cast<uint64_t>(now_ft.dwHighDateTime)<<32)|now_ft.dwLowDateTime;
    for(const auto& target:result.paused) {
        if(vm.settings_dialog_records.size()>=12) break;
        const std::wstring basename=json::Wide(target.basename);
        if(basename.empty()) continue;
        // A history record of the same program supplies its name and icon.
        uint64_t image_id=0;
        for(const auto& record:result.history.records)
            if(CompareStringOrdinal(record.app_basename.c_str(),-1,basename.c_str(),-1,TRUE)==CSTR_EQUAL) {image_id=record.app_image_id;break;}
        ui::WindowViewModel::DialogTakeoverRow row;
        FillApplication(result,basename,image_id,row);
        constexpr uint64_t day=24ull*60*60*10000000;
        const uint64_t days=target.retry_ft>now ? (std::max<uint64_t>)(1,(target.retry_ft-now+day-1)/day) : 1;
        auto reason=l10n::Get(I::DialogPausedReason);
        if(const auto slot=reason.find(L"{count}");slot!=std::wstring::npos) reason.replace(slot,7,std::to_wstring(target.failures));
        if(const auto slot=reason.find(L"{days}");slot!=std::wstring::npos) reason.replace(slot,6,std::to_wstring(days));
        row.detail=reason+L"\n"+l10n::Get(I::DialogPausedNext);row.attention=true;
        row.last_event=target.paused_ft;
        row.status=l10n::Get(I::DialogRecordStatusPaused);row.tone=2;row.group=3;row.retry=true;
        vm.settings_dialog_records.push_back(std::move(row));paused_group=true;
    }
    const I reasons[]={I::DialogHistoryReasonUnknown,I::DialogHistoryUnsupported,I::DialogHistoryProviderUnavailable,
        I::DialogHistoryTimeout,I::DialogHistoryPermissionDenied,I::DialogHistoryPermissionUnknown,I::DialogHistoryMismatch,
        I::DialogHistoryDisabled,I::DialogHistoryConflict,I::DialogHistoryInitialFolder,I::DialogHistoryProviderFailure,
        I::DialogHistoryNative,I::DialogHistorySucceeded,I::DialogHistoryShownClosed,I::DialogHistoryAdminAutoFailed,
        I::DialogHistoryShownThenFailed};
    const I actions[]={I::DialogHistoryNoAction,I::DialogHistoryRestart,I::DialogHistorySettings,
        I::DialogHistoryRetry,I::DialogHistoryUseNative,I::DialogHistoryExport};
    std::vector<const dialog_history::Record*> recent;
    recent.reserve(result.history.records.size());
    for(const auto& record:result.history.records) recent.push_back(&record);
    std::stable_sort(recent.begin(),recent.end(),[](const auto* a,const auto* b){return a->last_utc>b->last_utc;});
    for(const auto* record:recent) {
        if(vm.settings_dialog_records.size()>=12) break;
        ui::WindowViewModel::DialogTakeoverRow row;
        FillApplication(result,record->app_basename,record->app_image_id,row);
        const bool failed=record->state==dialog_history::State::Failure,fallback=record->state==dialog_history::State::Fallback;
        row.attention=failed || fallback;
        row.status=l10n::Get(record->state==dialog_history::State::Success ? I::DialogRecordStatusSuccess :
            fallback ? I::DialogRecordStatusNative : failed ? I::DialogRecordStatusFailed : I::DialogRecordStatusUnknown);
        row.tone=record->state==dialog_history::State::Success ? 1 : fallback ? 2 : failed ? 3 : 0;
        // Only a record that needs something says why and what to do.
        if(row.attention) {
            const auto reason=static_cast<size_t>(record->reason),action=static_cast<size_t>(record->action);
            row.detail=l10n::Get(reasons[reason<std::size(reasons)?reason:0]);
            row.detail+=L"\n"+std::wstring(l10n::Get(actions[action<std::size(actions)?action:5]));
        }
        row.meta=l10n::Get(record->operation==dialog_history::Operation::Open ? I::Open :
            record->operation==dialog_history::Operation::Save ? I::Save : I::DialogHistoryOperationUnknown);
        auto count=l10n::Get(record->count>1 ? I::DialogRecordCountMany : I::DialogRecordCountOne);
        if(const auto slot=count.find(L"{count}");slot!=std::wstring::npos) count.replace(slot,7,std::to_wstring(record->count));
        row.meta+=L" · "+count;
        row.last_event=UtcFileTime(record->last_utc);
        if(record->resolved_by_success || record->later_success) row.later=l10n::Get(I::DialogHistoryLaterSuccess);
        row.group=restart_group || paused_group ? 2 : 0;
        row.removable=true;
        vm.settings_dialog_records.push_back(std::move(row));
    }
}
void Redraw(AppState& s) {
    s.core.windows.ForEach([](HWND hwnd,AppState&) {InvalidateRect(hwnd,nullptr,FALSE);});
}
void GoTo(AppState& s,int page,I target) {
    OpenSettingsTab(s,page);
    const auto vm=BuildVm(s,false);
    const float w=static_cast<float>(s.compositor.Width()),h=static_cast<float>(s.compositor.Height());
    s.settings.SetScroll(s.renderer.SettingsDestinationOffset(vm,static_cast<int>(target),w,h),s.renderer.SettingsMaxScroll(vm,w,h));
    InvalidateRect(s.hwnd,nullptr,FALSE);
}
bool Confirm(AppState& s,bool restore) {
    ui::ConfirmDialogSpec spec;
    spec.title=l10n::Get(restore ? I::StartupRestoreConfirm : I::StartupDialogsEnableTitle);
    spec.message=l10n::Get(restore ? I::StartupRestoreDesc : I::StartupDialogsScopeBody);
    spec.confirm_text=l10n::Get(restore ? I::StartupRestoreAction : I::StartupDialogsEnableConfirm);
    spec.cancel_text=l10n::Get(I::Cancel);
    spec.default_choice=ui::ConfirmChoice::Cancel;
    return ui::ShowConfirmDialog(s.hwnd,spec,s.darkMode,s.accentColor);
}
void FinishRestore(AppState& s,DialogManagementUiState& current,DWORD tags_error) {
    current.restoring=false;
    current.error=current.error ? current.error : tags_error ? tags_error : current.restore_other_ok ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    current.notice=l10n::Get(!current.error && current.restore_other_ok ? I::StartupRestoreDone : I::StartupRestorePartial);
    Redraw(s);
}
void RestoreOthers(AppState& s,DialogManagementUiState& current) {
    // Reuse the same ownership-aware restoration as the folder master switch.
    s.settings.IntegrationAction(6);
    s.appPrefs.take_over_explorer_windows=false;
    s.appPrefs.integration_folders_compat=false;
    s.appPrefs.shell_tag_menu=false;
    current.restore_other_ok=!s.settings.IntegrationCanRetry() && s.settings.IntegrationState()==0;
    current.restore_other_ok=s.appPrefs.Save() && current.restore_other_ok;
    auto completion=std::make_shared<std::promise<DWORD>>();
    current.restore_tags=completion->get_future();
    std::thread([completion] {
        DWORD error=ERROR_GEN_FAILURE;
        try {error=app::RemoveTagMenu(app::GetPulseDataDir()+L"\\tagicons") ? ERROR_SUCCESS : ERROR_WRITE_FAULT;} catch(...) {}
        completion->set_value(error);
    }).detach();
}
}
void DialogManagementReady(AppState& s) {
    const auto current=s.core.dialogManagement;
    if(!current || !current->controller) return;
    for(auto& result:current->controller->Drain()) {
        if(result.generation!=current->generation) continue;
        const auto operation=result.operation;
        if(operation==app::DialogManagementOperation::PollHistory || operation==app::DialogManagementOperation::ClearHistory ||
           operation==app::DialogManagementOperation::ResumePaused) {
            if(!result.error) {
                current->snapshot.history=std::move(result.history);
                current->snapshot.paused=std::move(result.paused);
                current->snapshot.record_apps=std::move(result.record_apps);
                current->snapshot.restart_inventory=std::move(result.restart_inventory);
                current->snapshot.restart_checked=result.restart_checked;
            }
            current->snapshot.history_error=result.history_error ? result.history_error : result.error;
            current->busy=false;current->refreshing=false;current->history_refreshed=GetTickCount64();
            Redraw(s);
            continue;
        }
        if(operation==app::DialogManagementOperation::DisableAuto) {
            result.configuration=current->snapshot.configuration;
            result.bundle_verified=current->snapshot.bundle_verified;
            result.bundle_error=current->snapshot.bundle_error;
        }
        const DWORD previous_error=current->error;
        if(operation!=app::DialogManagementOperation::Refresh || result.error) current->error=result.error;
        const bool redraw=result.restart_checked || current->busy || current->restoring || operation!=app::DialogManagementOperation::Refresh ||
            current->snapshot.auto_path_mismatch!=result.auto_path_mismatch || current->snapshot.auto_enabled!=result.auto_enabled || current->snapshot.auto_conflict!=result.auto_conflict ||
            current->snapshot.auto_error!=result.auto_error || current->snapshot.bundle_verified!=result.bundle_verified ||
            current->snapshot.auto_arch!=result.auto_arch ||
            current->snapshot.application_valid!=result.application_valid || current->snapshot.application_machine!=result.application_machine ||
            current->snapshot.launch_bundle_verified!=result.launch_bundle_verified || current->snapshot.launch_bundle_error!=result.launch_bundle_error ||
            current->snapshot.admin_launch_ready!=result.admin_launch_ready || current->snapshot.admin_launch_error!=result.admin_launch_error ||
            current->snapshot.configuration.application!=result.configuration.application ||
            current->snapshot.bundle_error!=result.bundle_error || previous_error!=current->error;
        // Only history polls resolve programs: keep the found names and icons.
        if(result.record_apps.empty()) result.record_apps=std::move(current->snapshot.record_apps);
        current->snapshot=std::move(result);
        current->busy=false;current->refreshing=false;current->refreshed=GetTickCount64();
        if(operation==app::DialogManagementOperation::Refresh || operation==app::DialogManagementOperation::ExportDiagnostics) current->history_refreshed=current->refreshed;
        const bool feedback_result=operation==app::DialogManagementOperation::ExportDiagnostics && current->exporting_feedback;
        if(feedback_result) {
            current->exporting_feedback=false;
            if(!current->error) current->feedback={};
            else {
                current->notice=l10n::Get(I::DiagnosticsExportFailed)+L" ("+std::to_wstring(current->error)+L")";
                current->error=previous_error;
            }
        }
        if(operation==app::DialogManagementOperation::ExportDiagnostics && !current->snapshot.error) {
            current->notice=l10n::Get(I::StartupDiagnosticsSaved)+current->snapshot.diagnostics_path;
            if(feedback_result) current->notice+=L"\n"+l10n::Get(I::DialogFeedbackSaved);
            if(!current->snapshot.diagnostics_raw_files)
                current->notice+=L"\n"+l10n::Get(I::StartupDiagnosticsNoRawLogs);
            else if(current->snapshot.diagnostics_raw_incomplete)
                current->notice+=L"\n"+l10n::Get(I::StartupDiagnosticsPartialRawLogs);
        }
        if(operation==app::DialogManagementOperation::EnableAuto && !current->error)
            current->notice=l10n::Get(I::StartupDialogsEnabledHint);
        if(operation==app::DialogManagementOperation::DisableAuto && current->restoring) {
            try {RestoreOthers(s,*current);} catch(...) {current->restore_other_ok=false;FinishRestore(s,*current,ERROR_NOT_ENOUGH_MEMORY);}
        }
        if(redraw) Redraw(s);
    }
}
void TickDialogManagement(AppState& s) {
    if(s.isolatedTest || !IsWindow(s.core.host.Hwnd())) return;
    if(!s.core.dialogManagement) {
        auto fresh=std::make_shared<DialogManagementUiState>();
        fresh->controller=std::make_shared<app::DialogManagementController>(s.core.host.Hwnd(),WM_DIALOG_MANAGEMENT_RESULT,std::wstring{},
            [](const std::vector<app::DialogRecordAppQuery>& queries){return app::ResolveDialogRecordApps(queries,app::DialogRecordIconEdge());});
        fresh->generation=fresh->controller->PollHistory();fresh->refreshing=true;
        s.core.dialogManagement=std::move(fresh);
    }
    const auto current=s.core.dialogManagement;
    if(current->feedback.start_filetime && !current->exporting_feedback && !current->modal && !current->api_probe) {
        const HWND foreground=GetForegroundWindow();
        DWORD pid=0;
        if(foreground && foreground!=GetShellWindow()) GetWindowThreadProcessId(foreground,&pid);
        if(pid && pid!=GetCurrentProcessId()) current->feedback.foreground_pid=pid;
    }
    if(current->api_probe && WaitForSingleObject(current->api_probe,0)!=WAIT_TIMEOUT) {
        CloseHandle(current->api_probe);current->api_probe=nullptr;
        Redraw(s);
    }
    if(current->busy || current->refreshing) DialogManagementReady(s);
    if(current->restoring && current->restore_tags.valid() && current->restore_tags.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        FinishRestore(s,*current,current->restore_tags.get());
    if(current->controller && !current->busy && !current->refreshing && !current->restoring && !current->modal && GetTickCount64()-current->history_refreshed>15000) {
        current->generation=current->controller->PollHistory();current->refreshing=true;
    }
}
void FillDialogManagementVm(AppState& s,ui::WindowViewModel& vm) {
    vm.settings_dialog_available=!s.isolatedTest && IsWindow(s.core.host.Hwnd());
    vm.settings_dialog_status=l10n::Get(I::StartupDialogsChecking);
    if(!s.core.dialogManagement && vm.settings_page==11 && vm.settings_dialog_available) {
        auto fresh=std::make_shared<DialogManagementUiState>();
        fresh->controller=std::make_shared<app::DialogManagementController>(s.core.host.Hwnd(),WM_DIALOG_MANAGEMENT_RESULT,std::wstring{},
            [](const std::vector<app::DialogRecordAppQuery>& queries){return app::ResolveDialogRecordApps(queries,app::DialogRecordIconEdge());});
        fresh->generation=fresh->controller->Refresh();fresh->busy=true;
        s.core.dialogManagement=std::move(fresh);
    }
    const auto current=s.core.dialogManagement;
    if(!current) return;
    if(vm.settings_page==11 && vm.settings_dialog_available && current->controller && !current->busy && !current->refreshing && !current->restoring && !current->modal && (!current->snapshot.generation || GetTickCount64()-current->refreshed>15000)) {
        // Keep the last verified state visible during periodic background checks.
        current->generation=current->controller->Refresh();current->refreshing=true;
    }
    const auto& snapshot=current->snapshot;
    vm.settings_dialog_api_test_running=current->api_probe!=nullptr;
    vm.settings_dialog_feedback_recording=current->feedback.start_filetime!=0;
    vm.settings_dialog_busy=current->busy || current->modal;
    FillHistoryVm(snapshot,vm);
    vm.settings_dialog_can_launch=snapshot.launch_bundle_verified && snapshot.application_valid;
    vm.settings_dialog_can_launch_admin=snapshot.admin_launch_ready && snapshot.application_valid;
    vm.settings_integration_restore_busy=current->restoring;
    vm.settings_dialog_enabled=snapshot.auto_enabled && !snapshot.auto_path_mismatch;
    s.core.userDialogAuto.Update(!s.isolatedTest && !s.shot.active && s.appPrefs.dialog_adaptive_legacy && vm.settings_dialog_enabled, app::GetPulseDataDir());
    vm.settings_dialog_conflict=snapshot.auto_conflict;
    vm.settings_dialog_failed=snapshot.auto_path_mismatch || current->error || snapshot.auto_error || (!snapshot.bundle_verified && !snapshot.auto_enabled && !current->busy);
    vm.settings_dialog_scope_expanded=current->scope_expanded;
    vm.settings_dialog_can_enable_auto=snapshot.bundle_verified;
    if(current->busy) vm.settings_dialog_status=l10n::Get(I::StartupDialogsChecking);
    else if(snapshot.auto_conflict) vm.settings_dialog_status=l10n::Get(I::StartupDialogsConflict);
    else if(current->error || snapshot.auto_error) vm.settings_dialog_status=l10n::Get(I::StartupDialogsFailed);
    else if(snapshot.auto_path_mismatch) vm.settings_dialog_status=l10n::Get(I::StartupDialogsPathMismatch);
    else if(!snapshot.bundle_verified && !snapshot.auto_enabled) vm.settings_dialog_status=l10n::Get(I::StartupDialogsMissing);
    else vm.settings_dialog_status=l10n::Get(snapshot.auto_enabled ? I::StartupDialogsOn : I::StartupDialogsOff);
    const DWORD error=current->error ? current->error : snapshot.auto_error ? snapshot.auto_error : (!snapshot.bundle_verified ? snapshot.bundle_error : 0);
    if(error && !current->busy) vm.settings_dialog_status+=L" ("+std::to_wstring(error)+L")";
    if(!current->notice.empty()) vm.settings_dialog_status+=L"\n"+current->notice;
}
void DialogManagementAction(AppState& s,int action) {
    if(action==14) {GoTo(s,11,I::DialogJumpBar);return;}
    if(action>=20 && action<=22) {
        const I targets[]={I::SettingsIntegration,I::StartupDialogsTitle,I::SettingsSecStartupTray};GoTo(s,11,targets[action-20]);return;
    }
    if(action==13) {
        if(!s.core.dialogManagement) s.core.dialogManagement=std::make_shared<DialogManagementUiState>();
        s.core.dialogManagement->scope_expanded=!s.core.dialogManagement->scope_expanded;Redraw(s);return;
    }
    if(s.isolatedTest || !IsWindow(s.core.host.Hwnd())) return;
    auto current=s.core.dialogManagement;
    if(!current || !current->controller || current->busy || current->restoring || current->modal) return;
    if(action==17) {
        if(current->api_probe || current->feedback.start_filetime) return;
        const auto choice=DialogMenu(s,{{100,I::DialogApiTestOpen},{101,I::DialogApiTestSave},{102,I::DialogApiTestFolder},{103,I::DialogApiTestLegacy}});
        if(choice<100 || choice>103) return;
        DWORD error=ERROR_SUCCESS;
        try {error=app::LaunchDialogApiProbe(current->api_probe,static_cast<app::DialogApiScenario>(choice),s.darkMode);}
        catch(...) {error=ERROR_NOT_ENOUGH_MEMORY;}
        if(error) current->notice=l10n::Get(I::DialogApiTestLaunchFailed)+L" ("+std::to_wstring(error)+L")";
        Redraw(s);return;
    }
    if(action==18) {
        if(current->api_probe) return;
        if(!current->feedback.start_filetime) {
            const int choice=DialogMenu(s,{{1,I::DialogFeedbackOpen},{2,I::DialogFeedbackSave},{3,I::DialogFeedbackFolder},{4,I::DialogFeedbackOther}});
            if(choice<1 || choice>4) return;
            current->feedback={NowFileTime(),0,0,static_cast<uint32_t>(choice==4 ? 0 : choice)};
            current->notice.clear();Redraw(s);return;
        }
        auto window=current->feedback;window.end_filetime=(std::max)(NowFileTime(),window.start_filetime);
        ui::ConfirmDialogSpec spec;
        spec.title=l10n::Get(I::DialogFeedbackFinish);
        spec.message=l10n::Get(I::DialogFeedbackReview);
        spec.confirm_text=l10n::Get(I::DialogFeedbackExport);
        spec.secondary_text=l10n::Get(I::DialogFeedbackDiscard);
        spec.cancel_text=l10n::Get(I::Cancel);
        spec.default_choice=ui::ConfirmChoice::Cancel;
        current->modal=true;
        struct EndModal {bool& flag;~EndModal(){flag=false;}};
        ui::ConfirmChoice answer;
        {EndModal end{current->modal};answer=ui::ShowConfirmDialogEx(s.hwnd,spec,s.darkMode,s.accentColor);}
        if(answer==ui::ConfirmChoice::Cancel) return;
        if(answer==ui::ConfirmChoice::Secondary) {current->feedback={};Redraw(s);return;}
        current->refreshing=false;current->notice.clear();current->error=0;
        try {
            current->generation=current->controller->ExportFeedback(window);
            current->busy=current->exporting_feedback=current->generation!=0;
            if(!current->busy) current->error=ERROR_NOT_READY;
        } catch(...) {current->error=ERROR_NOT_ENOUGH_MEMORY;}
        Redraw(s);return;
    }
    if(action==4) {
        current->refreshing=false;current->notice.clear();
        try {current->generation=current->controller->PollHistory(true);current->busy=current->generation!=0;}
        catch(...) {current->snapshot.history_error=ERROR_NOT_ENOUGH_MEMORY;}
        Redraw(s);return;
    }
    if(action==16 || (action>=100 && action<100+12)) {
        // 16 clears every record; 100+i deletes the i-th listed row, or resumes a paused program.
        bool all=action==16;uint64_t id=0;uint32_t reason=0;
        if(!all) {
            // The rows as drawn: restart rows, paused programs, then records newest first.
            ui::WindowViewModel listed;FillHistoryVm(current->snapshot,listed);
            const auto index=static_cast<size_t>(action-100);
            if(index<listed.settings_dialog_records.size() && listed.settings_dialog_records[index].retry) {
                current->refreshing=false;current->notice.clear();
                try {current->generation=current->controller->ResumePaused(listed.settings_dialog_records[index].file);current->busy=current->generation!=0;}
                catch(...) {current->snapshot.history_error=ERROR_NOT_ENOUGH_MEMORY;}
                Redraw(s);return;
            }
            if(index>=listed.settings_dialog_records.size() || !listed.settings_dialog_records[index].removable) return;
            size_t restart=0;for(const auto& row:listed.settings_dialog_records) restart+=row.removable ? 0 : 1;
            std::vector<const dialog_history::Record*> recent;
            for(const auto& record:current->snapshot.history.records) recent.push_back(&record);
            std::stable_sort(recent.begin(),recent.end(),[](const auto* a,const auto* b){return a->last_utc>b->last_utc;});
            if(index<restart || index-restart>=recent.size()) return;
            id=recent[index-restart]->app_image_id;reason=static_cast<uint32_t>(recent[index-restart]->reason);
        }
        current->refreshing=false;current->notice.clear();
        try {current->generation=current->controller->ClearHistory(all,id,reason);current->busy=current->generation!=0;}
        catch(...) {current->snapshot.history_error=ERROR_NOT_ENOUGH_MEMORY;}
        Redraw(s);return;
    }
    if(action!=10 && action!=11 && action!=12 && action!=15) return;
    const bool disabling=action==15 || (!current->snapshot.auto_path_mismatch && (action==11 ? current->retry_disable : current->snapshot.auto_enabled));
    if((action==15 || ((action==10 || action==11) && !disabling)) && !Confirm(s,action==15)) return;
    // Confirmation pumps messages: recheck shared state before submitting.
    if(current->busy || current->restoring || current->modal) return;
    std::wstring destination;
    if(action==12) {
        std::wstring desktop;
        PWSTR known=nullptr;
        if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,KF_FLAG_DEFAULT,nullptr,&known))) {
            desktop=known;CoTaskMemFree(known);
        }
        const HWND owner=s.hwnd;
        bool chosen=false;
        {
            current->modal=true;
            struct EndModal {bool& flag;~EndModal(){flag=false;}} end{current->modal};
            chosen=PickFolder(s,destination,l10n::Get(I::StartupDiagnosticsSaveLocation).c_str(),{},desktop);
        }
        if(!chosen || destination.empty() || !IsWindow(owner))return;
    }
    current->refreshing=false;
    current->notice.clear();current->error=0;
    try {
        if(action==12) current->generation=current->controller->ExportDiagnostics(std::move(destination));
        else if(action==15) {current->restoring=true;current->retry_disable=true;current->generation=current->controller->DisableAuto();}
        else {current->retry_disable=disabling;current->generation=disabling ? current->controller->DisableAuto() : current->controller->EnableAuto();}
        current->busy=current->generation!=0;
        if(!current->busy) {current->error=ERROR_NOT_READY;current->restoring=false;}
    } catch(...) {current->error=ERROR_NOT_ENOUGH_MEMORY;current->busy=false;current->restoring=false;}
    Redraw(s);
}
}

namespace pulse {
bool TestDialogManagementBackgroundVm(AppState& s) {
    const auto saved=s.core.dialogManagement;
    auto fixture=std::make_shared<DialogManagementUiState>();
    fixture->snapshot.bundle_verified=true;fixture->snapshot.auto_enabled=true;
    fixture->refreshed=GetTickCount64();s.core.dialogManagement=fixture;
    ui::WindowViewModel empty{};FillDialogManagementVm(s,empty);
    bool passed=empty.settings_dialog_records.empty() && empty.settings_dialog_history_note==l10n::Get(l10n::StringId::DialogHistoryEmpty);
    dialog_history::Record record;record.app_basename=L"Example.exe";record.last_utc=L"2026-11-11T12:00:00.000Z";record.count=2;
    record.state=dialog_history::State::Fallback;record.reason=dialog_history::Reason::ComponentMismatch;
    record.action=dialog_history::Action::RestartApplication;record.operation=dialog_history::Operation::Open;
    fixture->snapshot.history.records.push_back(record);
    ui::WindowViewModel before{};FillDialogManagementVm(s,before);
    fixture->refreshing=true;
    ui::WindowViewModel during{};FillDialogManagementVm(s,during);
    passed=passed && !during.settings_dialog_busy && before.settings_dialog_status==during.settings_dialog_status &&
        during.settings_dialog_records.size()==1 && during.settings_dialog_records[0].application==L"Example.exe" &&
        during.settings_dialog_records[0].detail.find(l10n::Get(l10n::StringId::DialogHistoryRestart))!=std::wstring::npos;
    fixture->snapshot.history.unattributed_events=1;fixture->snapshot.history_error=ERROR_READ_FAULT;
    ui::WindowViewModel partial{};FillDialogManagementVm(s,partial);
    passed=passed && partial.settings_dialog_records.size()==1 &&
        partial.settings_dialog_history_note.find(l10n::Get(l10n::StringId::DialogHistoryUnattributed))!=std::wstring::npos &&
        partial.settings_dialog_history_note.find(l10n::Get(l10n::StringId::DialogHistoryStorageError))!=std::wstring::npos;
    fixture->snapshot.restart_checked=true;
    dialog_restart::Record running;running.app_basename=L"OldComponent.exe";
    fixture->snapshot.restart_inventory.records.push_back(running);
    ui::WindowViewModel restart{};FillDialogManagementVm(s,restart);
    passed=passed && restart.settings_dialog_records.size()==2 &&
        restart.settings_dialog_records[0].application==L"OldComponent.exe" &&
        restart.settings_dialog_records[0].detail==l10n::Get(I::DialogRestartRequired);
    fixture->snapshot.restart_inventory.records.clear();
    fixture->snapshot.restart_inventory.scan_complete=false;
    ui::WindowViewModel rescanned{};FillDialogManagementVm(s,rescanned);
    passed=passed && rescanned.settings_dialog_records.size()==1 &&
        rescanned.settings_dialog_history_note.find(l10n::Get(I::DialogRestartPartial))!=std::wstring::npos;
    // A paused program: listed before the records with a retry button, not deletable,
    // and the history caption follows it.
    FILETIME now_ft{};GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now=(static_cast<uint64_t>(now_ft.dwHighDateTime)<<32)|now_ft.dwLowDateTime;
    dialog_adaptive::PausedTarget paused;paused.fingerprint="v2-fixture";paused.basename="Example.exe";paused.failures=2;
    paused.paused_ft=now;paused.retry_ft=now+dialog_adaptive::AdaptiveTargetStore::kBlockedRetryAfter;
    fixture->snapshot.paused.push_back(paused);
    ui::WindowViewModel withpaused{};FillDialogManagementVm(s,withpaused);
    const auto& rows=withpaused.settings_dialog_records;
    passed=passed && rows.size()==2 && rows[0].retry && !rows[0].removable && rows[0].group==3 && rows[0].file==L"Example.exe" &&
        rows[0].status==l10n::Get(I::DialogRecordStatusPaused) && rows[0].detail.find(L'{')==std::wstring::npos &&
        rows[0].detail.find(L"7")!=std::wstring::npos && rows[0].last_event==now &&
        !rows[1].retry && rows[1].removable && rows[1].group==2;
    fixture->snapshot.paused.clear();
    s.core.dialogManagement=saved;return passed;
}
}
