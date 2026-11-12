#include "app_internal.h"
#include "dialog_management_ui.h"
#include "dialog_management_controller.h"
#include "app_messages.h"
#include "session.h"
#include "../ui/ui_renderer.h"
#include "shell_tag_registry.h"
#include "../ui/confirm_dialog.h"
#include <future>
#include <thread>
#include <algorithm>

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
    ULONGLONG refreshed = 0, history_refreshed = 0;
};
namespace {
using I=l10n::StringId;
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
    const I reasons[]={I::DialogHistoryReasonUnknown,I::DialogHistoryUnsupported,I::DialogHistoryProviderUnavailable,
        I::DialogHistoryTimeout,I::DialogHistoryPermissionDenied,I::DialogHistoryPermissionUnknown,I::DialogHistoryMismatch,
        I::DialogHistoryDisabled,I::DialogHistoryConflict,I::DialogHistoryInitialFolder,I::DialogHistoryProviderFailure,
        I::DialogHistoryNative,I::DialogHistorySucceeded,I::DialogHistoryShownClosed,I::DialogHistoryAdminAutoFailed};
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
        row.group=restart_group ? 2 : 0;
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
        if(operation==app::DialogManagementOperation::PollHistory) {
            if(!result.error) {
                current->snapshot.history=std::move(result.history);
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
        if(operation==app::DialogManagementOperation::ExportDiagnostics && !current->error)
            current->notice=l10n::Get(I::StartupDiagnosticsSaved)+current->snapshot.diagnostics_path;
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
    vm.settings_dialog_busy=current->busy || current->modal;
    FillHistoryVm(snapshot,vm);
    vm.settings_dialog_can_launch=snapshot.launch_bundle_verified && snapshot.application_valid;
    vm.settings_dialog_can_launch_admin=snapshot.admin_launch_ready && snapshot.application_valid;
    vm.settings_integration_restore_busy=current->restoring;
    vm.settings_dialog_enabled=snapshot.auto_enabled && !snapshot.auto_path_mismatch;
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
    if(action==4) {
        current->refreshing=false;current->notice.clear();
        try {current->generation=current->controller->PollHistory(true);current->busy=current->generation!=0;}
        catch(...) {current->snapshot.history_error=ERROR_NOT_ENOUGH_MEMORY;}
        Redraw(s);return;
    }
    if(action!=10 && action!=11 && action!=12 && action!=15) return;
    const bool disabling=action==15 || (!current->snapshot.auto_path_mismatch && (action==11 ? current->retry_disable : current->snapshot.auto_enabled));
    if((action==15 || ((action==10 || action==11) && !disabling)) && !Confirm(s,action==15)) return;
    // Confirmation pumps messages: recheck shared state before submitting.
    if(current->busy || current->restoring || current->modal) return;
    current->refreshing=false;
    current->notice.clear();current->error=0;
    try {
        if(action==12) current->generation=current->controller->ExportDiagnostics();
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
    s.core.dialogManagement=saved;return passed;
}
}
