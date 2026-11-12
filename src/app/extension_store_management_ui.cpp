#include "extension_store_management_ui.h"
#include "extension_store_permissions.h"
#include "extension_store_review.h"
#include "app_state.h"
#include "extensions/package_signature.h"
#include "extensions/runtime.h"
#include "extensions/native/session.h"
#include "extensions/store/catalog.h"
#include "../ui/form_dialog.h"
#include "../common/json_value.h"
#include "../common/localization.h"

namespace pulse::app {
namespace {
namespace store = extensions::store;
using Phase = store::InstalledStorePhase;
using Id = l10n::StringId;
std::wstring Text(Id id) { return l10n::Get(id); }
bool ActivePhase(Phase phase) { return phase == Phase::Preparing || phase == Phase::AwaitingConfirmation || phase == Phase::Committing; }
Id ActionText(store::StoreChangeAction action) {
    switch (action) {
    case store::StoreChangeAction::Enable: return Id::ExtensionPackageEnable;
    case store::StoreChangeAction::Disable: return Id::ExtensionPackageDisable;
    case store::StoreChangeAction::Uninstall: return Id::StoreUninstall;
    case store::StoreChangeAction::Rollback: return Id::StoreRollback;
    }
    return Id::StoreChangeTitle;
}
extensions::UiNode Label(const char* id, const std::wstring& text) {
    extensions::UiNode n; n.type = extensions::UiNodeType::Text; n.id = id; n.text = json::Utf8(text); return n;
}
extensions::UiNode Check(const char* key, Id text) {
    extensions::UiNode n; n.type = extensions::UiNodeType::Checkbox; n.id = key; n.state = key; n.text = json::Utf8(Text(text)); return n;
}
}
struct ExtensionManagementUi::Impl : std::enable_shared_from_this<Impl> {
    explicit Impl(std::filesystem::path path) : root(std::move(path)) { service.Refresh(root); }
    ~Impl() { Stop(); }
    void Stop() { dialog.Destroy(); service.Stop(); }
    store::InstalledStoreService service;
    std::filesystem::path root;
    ui::FormDialog dialog;
    HWND owner = nullptr;
    const AppState* owner_state = nullptr; // Identity only, explicitly released at WM_DESTROY.
    uint64_t prompted = 0, draft_generation = 0, purged_generation = 0;
    bool missing_consent = false;
    std::vector<extensions::UiStateEntry> draft;
    void Prompt(AppState& s) {
        const auto snapshot = service.GetSnapshot();
        if (&s != owner_state || s.hwnd != owner || dialog.IsOpen() || !IsWindowEnabled(owner) ||
            snapshot.phase != Phase::AwaitingConfirmation || !snapshot.review || snapshot.generation == prompted) return;
        prompted = snapshot.generation;
        const auto& review = *snapshot.review;
        ui::FormDialogSpec spec;
        spec.title = Text(Id::StoreChangeTitle);
        spec.message = Text(Id::StoreChangeMessage);
        if (missing_consent) spec.message += L"\n" + Text(Id::ExtensionStoreConsentRequired);
        spec.buttons = {{Text(Id::StoreChangeApprove), ui::FormDialogButtonKind::Primary}, {Text(Id::Cancel)}};
        spec.default_button = -1; spec.cancel_button = 1; spec.width_dip = 620;
        extensions::UiTree tree; tree.root.id = "store_change"; tree.root.gap = 8;
        tree.root.children.push_back(Label("action", Text(ActionText(review.action)) + L" · " + json::Wide(review.id)));
        const std::wstring publisher_line = review.publisher_id.empty()
            ? Text(Id::StorePublisherUnsigned)
            : (review.publisher_id == store::kOfficialCatalogPublisher
                   ? Text(Id::StoreOfficial)
                   : ReplaceStoreToken(Text(Id::StorePublisherVerified), L"{id}", AbbreviateHexDigest(review.publisher_id)));
        tree.root.children.push_back(Label("publisher", Text(Id::Publisher) + L": " + publisher_line));
        extensions::UiNode details; details.id = "details"; details.type = extensions::UiNodeType::ScrollView;
        details.min_height = 96; details.max_height = 144;
        std::wstring description = ReplaceStoreToken(Text(Id::StoreExtensionIdLabel), L"{id}", json::Wide(review.id));
        if (review.action == store::StoreChangeAction::Rollback) {
            description += L"\n" + ReplaceStoreToken(Text(Id::StoreRollbackRevisionLabel), L"{rev}",
                AbbreviateHexDigest(review.active_revision) + L" → " + AbbreviateHexDigest(review.previous_revision));
        }
        if (!review.added_resources.empty()) {
            description += L"\n" + Text(Id::StorePermissionsAddedHeading);
            for (const auto& permission : review.added_resources) {
                description += L"\n• " + LocalizedResourcePermission(permission);
            }
        } else {
            description += L"\n" + Text(Id::StoreChangeNoAddedPermissions);
        }
        details.children.push_back(Label("permissions", description)); tree.root.children.push_back(std::move(details));
        const bool uninstall = review.action == store::StoreChangeAction::Uninstall;
        if (review.native_code) tree.root.children.push_back(Check("native", Id::ExtensionStoreNativeConsent));
        if (!review.added_resources.empty()) tree.root.children.push_back(Check("resources", Id::ExtensionStoreResourcesConsent));
        if (uninstall) tree.root.children.push_back(Check("purge_data", Id::StoreUninstallPurgeData));
        tree.state = draft_generation == snapshot.generation ? draft : std::vector<extensions::UiStateEntry>{{"native", "false"}, {"resources", "false"}, {"purge_data", "false"}};
        spec.form = std::move(tree);
        spec.form_height_dip = review.native_code || !review.added_resources.empty() ? 312.0f : (uninstall ? 272.0f : 208.0f);
        const auto weak = weak_from_this(); const auto generation = snapshot.generation;
        spec.on_closed = [weak, generation](int button, std::vector<extensions::UiStateEntry> states) {
            if (const auto self = weak.lock()) {
                const auto decision = EvaluateStoreChangeReview(self->service.GetSnapshot(), generation, button, states);
                using Kind = StoreChangeDecision::Kind;
                switch (decision.kind) {
                case Kind::Decline: self->service.Cancel(); break;
                case Kind::Approve: self->service.Confirm(generation, decision.consent); break;
                case Kind::MissingConsent:
                    self->draft = std::move(states); self->draft_generation = generation;
                    self->missing_consent = true; self->prompted = 0; break;
                case Kind::Ignore: break;
                }
            }
        };
        if (!dialog.Show(s.hwnd, std::move(spec), s.darkMode, s.accentColor) && service.GetSnapshot().generation == generation) service.Cancel();
    }
};
ExtensionManagementUi::ExtensionManagementUi(std::filesystem::path root) : impl_(std::make_shared<Impl>(std::move(root))) {}
ExtensionManagementUi::~ExtensionManagementUi() = default;
void ExtensionManagementUi::Stop() { impl_->Stop(); }
bool ExtensionManagementUi::Active() const { return ActivePhase(impl_->service.GetSnapshot().phase); }
void ExtensionManagementUi::Refresh() { impl_->service.Refresh(impl_->root); }
void ExtensionManagementUi::Cancel() { if (Active()) { impl_->dialog.Destroy(); impl_->service.Cancel(); } }
void ExtensionManagementUi::Fill(AppState& s, ui::ExtensionStoreView& view, bool installation_active) {
    const auto snapshot = impl_->service.GetSnapshot();
    s.store_manage_view_generation = snapshot.generation;
    view.installed_loading = snapshot.phase == Phase::Loading;
    view.installed_busy = installation_active || ActivePhase(snapshot.phase);
    view.pending_restart = view.pending_restart || snapshot.pending_restart;
    if (snapshot.phase == Phase::Failed) view.installed_error = Text(Id::StoreInstalledFailed) + L"\n" + json::Wide(snapshot.error);
    if (!snapshot.installed) return;
    for (const auto& entry : snapshot.installed->entries) {
        ui::InstalledExtensionItem item;
        item.id = json::Wide(entry.id); item.name = entry.name.empty() ? item.id : json::Wide(entry.name);
        // A registry revision is a manifest digest, not a product version.
        // FillExtensionStoreView resolves versions from matching signed releases.
        const bool unsigned_local = entry.publisher_id.empty() && entry.error == "store_open:2";
        item.publisher = unsigned_local
            ? Text(Id::StorePublisherUnsigned)
            : (entry.publisher_id == store::kOfficialCatalogPublisher ? Text(Id::StoreOfficial) : json::Wide(entry.publisher_id));
        item.enabled = entry.enabled; item.native_code = entry.native_code; item.revision = entry.revision;
        item.status = Text(entry.enabled ? Id::StoreExtensionEnabled : Id::StoreExtensionDisabled);
        for (size_t p = 0; p < extensions::PackageCount() && p < 10000; ++p) {
            const auto* manifest = extensions::PackageManifest(p);
            if (!manifest || manifest->id != entry.id) continue;
            if (unsigned_local) {
                item.native_code = item.native_code || manifest->native.has_value();
            }
            const auto runtime_state = extensions::native::QueryPackageRuntime(p);
            if (runtime_state.state == extensions::native::PackageRuntimeState::Draining)
                item.status += L" · " + Text(Id::StoreExtensionDraining);
            if (runtime_state.restart_required) {
                view.pending_restart = true;
                item.status += L" · " + Text(Id::StoreExtensionRestartRequired);
            }
            break;
        }
        item.notice = unsigned_local ? std::wstring{} : json::Wide(entry.error);
        const bool ready = !view.installed_busy && !view.installed_loading;
        item.can_enable = ready && !entry.enabled && entry.error.empty();
        item.can_disable = ready && entry.enabled;
        item.can_uninstall = ready;
        item.can_rollback = ready && !entry.previous_revision.empty() && entry.error.empty();
        view.installed.push_back(std::move(item));
    }
}
bool ExtensionManagementUi::Action(AppState& s, int action, bool installation_active) {
    using A = ui::PackAction;
    if (action == static_cast<int>(A::InstalledRefresh)) {
        if (!installation_active && !Active()) Refresh();
        return true;
    }
    int base = 0; auto change = store::StoreChangeAction::Disable;
    if (action >= static_cast<int>(A::InstalledEnableBase) && action < static_cast<int>(A::InstalledDisableBase)) { base = static_cast<int>(A::InstalledEnableBase); change = store::StoreChangeAction::Enable; }
    else if (action >= static_cast<int>(A::InstalledDisableBase) && action < static_cast<int>(A::InstalledUninstallBase)) { base = static_cast<int>(A::InstalledDisableBase); change = store::StoreChangeAction::Disable; }
    else if (action >= static_cast<int>(A::InstalledUninstallBase) && action < static_cast<int>(A::InstalledRollbackBase)) { base = static_cast<int>(A::InstalledUninstallBase); change = store::StoreChangeAction::Uninstall; }
    else if (action >= static_cast<int>(A::InstalledRollbackBase) && action < static_cast<int>(A::InstalledRollbackBase) + 10000) { base = static_cast<int>(A::InstalledRollbackBase); change = store::StoreChangeAction::Rollback; }
    if (!base) return false;
    const auto snapshot = impl_->service.GetSnapshot(); const auto index = static_cast<size_t>(action - base);
    if (installation_active || Active() || snapshot.phase == Phase::Loading || snapshot.generation != s.store_manage_view_generation || !snapshot.installed || index >= snapshot.installed->entries.size()) return true;
    const auto& entry = snapshot.installed->entries[index];
    if ((change == store::StoreChangeAction::Enable && (entry.enabled || !entry.error.empty())) ||
        (change == store::StoreChangeAction::Disable && !entry.enabled) ||
        (change == store::StoreChangeAction::Rollback && (entry.previous_revision.empty() || !entry.error.empty()))) return true;
    impl_->owner = s.hwnd; impl_->owner_state = &s; impl_->missing_consent = false;
    impl_->draft.clear(); impl_->draft_generation = 0;
    impl_->service.PrepareChange(impl_->root, entry.id, change);
    return true;
}
void ExtensionManagementUi::Tick(AppState& s) {
    const auto snapshot = impl_->service.GetSnapshot();
    if (snapshot.purged_package_id && snapshot.generation != impl_->purged_generation) {
        impl_->purged_generation = snapshot.generation;
        if (extensions::RevokePublishTrustForPackage(s.appPrefs.extension_publish_trusted, *snapshot.purged_package_id) > 0 && !s.isolatedTest)
            s.appPrefs.Save();
    }
    const auto token = snapshot.generation * 32 + static_cast<unsigned>(snapshot.phase) * 2 + (snapshot.pending_restart ? 1 : 0);
    if (token != s.store_manage_ui_token) { s.store_manage_ui_token = token; InvalidateRect(s.hwnd, nullptr, FALSE); }
    impl_->Prompt(s);
}
void ExtensionManagementUi::ReleaseOwner(AppState& s) {
    if (impl_->owner_state != &s || impl_->owner != s.hwnd) return;
    impl_->dialog.Destroy(); impl_->owner_state = nullptr; impl_->owner = nullptr;
    if (Active()) impl_->service.Cancel();
}
}
