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

namespace pulse {
struct DialogManagementUiState {
    std::shared_ptr<app::DialogManagementController> controller;
    app::DialogManagementResult snapshot;
    uint64_t generation = 0;
    DWORD error = ERROR_SUCCESS;
    bool busy = false, refreshing = false, scope_expanded = false, restoring = false;
    bool restore_other_ok = true, retry_disable = false;
    std::future<DWORD> restore_tags;
    std::wstring notice;
    ULONGLONG refreshed = 0;
};
namespace {
using I=l10n::StringId;
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
        if(operation==app::DialogManagementOperation::DisableAuto) {
            result.configuration=current->snapshot.configuration;
            result.bundle_verified=current->snapshot.bundle_verified;
            result.bundle_error=current->snapshot.bundle_error;
        }
        const DWORD previous_error=current->error;
        if(operation!=app::DialogManagementOperation::Refresh || result.error) current->error=result.error;
        const bool redraw=current->busy || current->restoring || operation!=app::DialogManagementOperation::Refresh ||
            current->snapshot.auto_path_mismatch!=result.auto_path_mismatch || current->snapshot.auto_enabled!=result.auto_enabled || current->snapshot.auto_conflict!=result.auto_conflict ||
            current->snapshot.auto_error!=result.auto_error || current->snapshot.bundle_verified!=result.bundle_verified ||
            current->snapshot.bundle_error!=result.bundle_error || previous_error!=current->error;
        current->snapshot=std::move(result);
        current->busy=false;current->refreshing=false;current->refreshed=GetTickCount64();
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
    const auto current=s.core.dialogManagement;
    if(!current) return;
    if(current->busy || current->refreshing) DialogManagementReady(s);
    if(current->restoring && current->restore_tags.valid() && current->restore_tags.wait_for(std::chrono::seconds(0))==std::future_status::ready)
        FinishRestore(s,*current,current->restore_tags.get());
}
void FillDialogManagementVm(AppState& s,ui::WindowViewModel& vm) {
    vm.settings_dialog_available=!s.isolatedTest && IsWindow(s.core.host.Hwnd());
    vm.settings_dialog_status=l10n::Get(I::StartupDialogsChecking);
    if(!s.core.dialogManagement && vm.settings_page==11 && vm.settings_dialog_available) {
        auto fresh=std::make_shared<DialogManagementUiState>();
        fresh->controller=std::make_shared<app::DialogManagementController>(s.core.host.Hwnd(),WM_DIALOG_MANAGEMENT_RESULT,std::wstring{});
        fresh->generation=fresh->controller->Refresh();fresh->busy=true;
        s.core.dialogManagement=std::move(fresh);
    }
    const auto current=s.core.dialogManagement;
    if(!current) return;
    if(vm.settings_page==11 && vm.settings_dialog_available && current->controller && !current->busy && !current->refreshing && !current->restoring && GetTickCount64()-current->refreshed>15000) {
        // Keep the last verified state visible during periodic background checks.
        current->generation=current->controller->Refresh();current->refreshing=true;
    }
    const auto& snapshot=current->snapshot;
    vm.settings_dialog_busy=current->busy;
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
    if(!current || !current->controller || current->busy || current->restoring) return;
    if(action!=10 && action!=11 && action!=12 && action!=15) return;
    const bool disabling=action==15 || (!current->snapshot.auto_path_mismatch && (action==11 ? current->retry_disable : current->snapshot.auto_enabled));
    if((action==15 || ((action==10 || action==11) && !disabling)) && !Confirm(s,action==15)) return;
    // Confirmation pumps messages: recheck shared state before submitting.
    if(current->busy || current->restoring) return;
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
    fixture->snapshot.bundle_verified=true;
    fixture->snapshot.auto_enabled=true;
    fixture->refreshed=GetTickCount64();
    s.core.dialogManagement=fixture;
    bool passed=true;
    for(int variant=0;variant<4;++variant) {
        fixture->snapshot.auto_conflict=variant==1;
        fixture->snapshot.auto_path_mismatch=variant==3;
        fixture->error=variant==2 ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
        fixture->refreshing=false;
        ui::WindowViewModel before{},during{};
        FillDialogManagementVm(s,before);
        fixture->refreshing=true;
        FillDialogManagementVm(s,during);
        if (variant == 3) passed=passed && !during.settings_dialog_enabled && during.settings_dialog_failed &&
            during.settings_dialog_status==l10n::Get(l10n::StringId::StartupDialogsPathMismatch);
        passed=passed && before.settings_dialog_busy==during.settings_dialog_busy &&
            !during.settings_dialog_busy &&
            before.settings_dialog_status==during.settings_dialog_status &&
            before.settings_dialog_failed==during.settings_dialog_failed &&
            before.settings_dialog_enabled==during.settings_dialog_enabled;
    }
    s.core.dialogManagement=saved;
    return passed;
}
}
