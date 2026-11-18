#include "extension_store_ui.h"
#include "extension_store_review.h"
#include "extension_store_management_ui.h"
#include "app_state.h"
#include "extensions/runtime.h"
#include "extensions/store/catalog_service.h"
#include "extensions/store/install_service.h"
#include "../ui/form_dialog.h"
#include "../common/localization.h"
#include "../common/json_value.h"
#include "../../sdk/native/include/pulse/abi.h"
#include "pulse_version.h"
#include <filesystem>

namespace pulse {
namespace {
using Id = l10n::StringId;
namespace store = extensions::store;
using Phase = store::InstallServicePhase;
std::wstring Text(Id id) { return l10n::Get(id); }
std::wstring Localized(const store::LocalizedText& value) {
    const auto language = l10n::effective_language();
    const auto& selected = language == l10n::Language::EnUS ? value.en : language == l10n::Language::ZhTW ? value.tw : value.zh;
    return json::Wide(selected.empty() ? value.en : selected);
}
bool Busy(Phase phase) { return phase == Phase::Preparing || phase == Phase::Committing; }
extensions::UiNode Label(const char* id, const std::wstring& text) {
    extensions::UiNode node; node.type = extensions::UiNodeType::Text; node.id = id; node.text = json::Utf8(text); return node;
}
extensions::UiNode Check(const char* key, Id label) {
    extensions::UiNode node; node.type = extensions::UiNodeType::Checkbox; node.id = key; node.state = key; node.text = json::Utf8(Text(label)); return node;
}
}
// Catalog icon names map onto Segoe Fluent glyphs; an unknown name falls back to the package mark.
static std::wstring IconGlyph(const std::string& name) {
    static const std::pair<const char*, const wchar_t*> kGlyphs[] = {
        {"archive", L"çB8"}, {"compress", L"çB8"}, {"film", L"ç14"}, {"image", L"ë9F"}, {"doc", L"èA5"},
        {"find", L"ç21"}, {"ocr", L"èD2"}, {"git", L"èC8"}, {"rename", L"èAC"}, {"duplicate", L"èC8"},
        {"cloud", L"ç53"}, {"puzzle", L"ê86"}};
    for (const auto& [key, glyph] : kGlyphs) if (name == key) return glyph;
    return L"çB8";
}
static uint32_t IconColor(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') return 0x527D70;
    uint32_t value = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = hex[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return 0x527D70;
        value = value * 16 + static_cast<uint32_t>(digit);
    }
    return value;
}
static std::wstring SizeText(uint64_t bytes) {
    wchar_t text[32]{};
    if (bytes >= 1024ull * 1024) swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else swprintf_s(text, L"%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
    return text;
}
namespace app {
class ExtensionStoreController : public std::enable_shared_from_this<ExtensionStoreController> {
public:
    explicit ExtensionStoreController(bool persist) {
        root = extensions::PackageStoreRoot();
        if (root.empty() && persist && extensions::PackageCount() == 0) {
            wchar_t base[32768]{};
            const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", base, 32768);
            if (count && count < 32768) root = std::filesystem::path(base) / L"Pulse" / L"extensions";
        }
        if (!root.empty() && !root.is_absolute()) root.clear();
        if (!root.empty()) {
            cache = root.parent_path() / L"extension-catalog.cache";
            installer = std::make_unique<store::InstallService>(store::InstallServiceConfig{root, PULSE_VERSION_STRING_A, PULSE_SDK_PREVIEW});
            management = std::make_unique<ExtensionManagementUi>(root);
        }
        catalog.Refresh(cache);
    }
    void Stop() { dialog.Destroy(); catalog.Stop(); if (installer) installer->Stop(); if (management) management->Stop(); }
    ~ExtensionStoreController() { Stop(); } // AppCore teardown, after the window loop.
    store::CatalogService catalog;
    std::unique_ptr<store::InstallService> installer;
    std::unique_ptr<ExtensionManagementUi> management;
    uint64_t refreshed_installation = 0;
    std::filesystem::path root, cache;
    ui::FormDialog dialog;
    HWND prompt_owner = nullptr;
    const AppState* prompt_state = nullptr; // Identity only; cleared by WM_DESTROY, never dereferenced.
    uint64_t prompted_generation = 0;
    Phase prompted_phase = Phase::Idle;
    bool consent_missing = false;
    uint64_t draft_generation = 0;
    std::vector<extensions::UiStateEntry> draft_states;
    void Prompt(AppState& state) {
        if (!installer || &state != prompt_state || state.hwnd != prompt_owner || dialog.IsOpen() || !IsWindowEnabled(state.hwnd)) return;
        const auto snapshot = installer->GetSnapshot();
        if (snapshot.phase != Phase::AwaitingPublisherTrust && snapshot.phase != Phase::AwaitingInstall) return;
        if (prompted_generation == snapshot.generation && prompted_phase == snapshot.phase) return;
        if (!snapshot.package) return;
        prompted_generation = snapshot.generation; prompted_phase = snapshot.phase;
        const bool trust = snapshot.phase == Phase::AwaitingPublisherTrust;
        // A package signed by the pinned official publisher comes from the store itself: nothing to ask.
        // Trust, the native-code/permission consent and enabling are granted in one step.
        if (snapshot.package->PublisherId() == store::kOfficialCatalogPublisher) {
            if (trust) installer->ConfirmPublisherTrust(snapshot.generation, true);
            else installer->ConfirmInstall(snapshot.generation, store::InstallConsent{true, true, true, true});
            return;
        }
        ui::FormDialogSpec spec;
        spec.title = Text(trust ? Id::ExtensionStoreTrustTitle : Id::ExtensionStoreInstallTitle);
        spec.message = Text(trust ? Id::ExtensionStoreTrustMessage : Id::ExtensionStoreInstallMessage);
        if (!trust && consent_missing) spec.message += L"\n" + Text(Id::ExtensionStoreConsentRequired);
        spec.buttons = {{Text(trust ? Id::ExtensionStoreTrustAccept : Id::ExtensionStoreInstallAccept), ui::FormDialogButtonKind::Primary}, {Text(Id::Cancel)}};
        spec.default_button = -1; spec.cancel_button = 1; spec.width_dip = 620;
        extensions::UiTree tree; tree.root.id = "review"; tree.root.gap = 8;
        tree.root.children.push_back(Label("package", json::Wide(snapshot.package->Declaration().name)));
        tree.root.children.push_back(Label("publisher", Text(Id::Publisher) + L": " + json::Wide(snapshot.package->PublisherId())));
        extensions::UiNode scroll; scroll.type = extensions::UiNodeType::ScrollView; scroll.id = "details"; scroll.min_height = 96; scroll.max_height = 144;
        std::wstring details = root.wstring();
        if (!trust && snapshot.install_review) for (const auto& permission : snapshot.install_review->added_resources) details += L"\n" + json::Wide(permission);
        scroll.children.push_back(Label("permissions", details)); tree.root.children.push_back(std::move(scroll));
        bool native = false, resources = false, is_new = false;
        if (!trust && snapshot.install_review) {
            native = snapshot.install_review->native_code; resources = !snapshot.install_review->added_resources.empty(); is_new = snapshot.install_review->active_revision.empty();
            if (native) tree.root.children.push_back(Check("native", Id::ExtensionStoreNativeConsent));
            if (resources) tree.root.children.push_back(Check("resources", Id::ExtensionStoreResourcesConsent));
            if (is_new) tree.root.children.push_back(Check("enable", Id::ExtensionStoreEnableConsent));
            tree.state = draft_generation == snapshot.generation ? draft_states : std::vector<extensions::UiStateEntry>{{"native", "false"}, {"resources", "false"}, {"enable", "false"}};
        }
        spec.form = std::move(tree); spec.form_height_dip = trust ? 232.0f : 376.0f;
        const auto weak = weak_from_this(); const auto generation = snapshot.generation;
        spec.on_closed = [weak, generation, trust](int button, std::vector<extensions::UiStateEntry> states) {
            if (const auto self = weak.lock()) {
                if (!self->installer) return;
                const auto decision = EvaluateStoreReview(self->installer->GetSnapshot(), generation, trust, button, states);
                using Kind = StoreReviewDecision::Kind;
                switch (decision.kind) {
                case Kind::Trust: self->installer->ConfirmPublisherTrust(generation, true); break;
                case Kind::Decline:
                    if (trust) self->installer->ConfirmPublisherTrust(generation, false);
                    else self->installer->Cancel();
                    break;
                case Kind::MissingConsent:
                    self->draft_states = std::move(states); self->draft_generation = generation;
                    self->consent_missing = true; self->prompted_generation = 0; break;
                case Kind::Install: self->installer->ConfirmInstall(generation, decision.consent); break;
                case Kind::Ignore: break;
                }
            }
        };
        if (!dialog.Show(state.hwnd, std::move(spec), state.darkMode, state.accentColor) && installer->GetSnapshot().generation == generation) installer->Cancel();
    }
};
}
namespace {
std::shared_ptr<app::ExtensionStoreController> Controller(AppState& s) {
    if (!s.core.extension_store) s.core.extension_store = std::make_shared<app::ExtensionStoreController>(s.appPrefs.persist);
    return s.core.extension_store;
}
}
void FillExtensionStoreView(AppState& s, ui::WindowViewModel& vm) {
    vm.settings_store_browse = s.settings_store_browse;
    vm.settings_store_installed = s.settings_store_installed;
    const auto controller = Controller(s);
    const auto catalog = controller->catalog.GetSnapshot();
    s.store_view_generation = catalog.generation;
    auto& view = vm.settings_store_view;
    view = {};
    using State = ui::ExtensionStoreView::State;
    switch (catalog.state) {
    case store::CatalogServiceState::Loading: view.state = State::Loading; break;
    case store::CatalogServiceState::Online: view.state = State::Online; break;
    case store::CatalogServiceState::Offline: view.state = State::Offline; break;
    case store::CatalogServiceState::Failed: view.state = State::Failed; break;
    default: view.state = State::Idle; break;
    }
    store::InstallServiceSnapshot installation;
    if (controller->installer) installation = controller->installer->GetSnapshot();
    view.pending_restart = installation.pending_restart;
    view.busy = catalog.state == store::CatalogServiceState::Loading || Busy(installation.phase) || installation.phase == Phase::AwaitingInstall || installation.phase == Phase::AwaitingPublisherTrust;
    if (controller->management) {
        controller->management->Fill(s, view, Busy(installation.phase) || installation.phase == Phase::AwaitingInstall || installation.phase == Phase::AwaitingPublisherTrust);
        view.busy = view.busy || controller->management->Active();
    } else view.installed_error = Text(Id::ExtensionStoreUnavailable);
    if (installation.phase == Phase::Failed) view.notice = Text(Id::StoreFailed) + L"\n" + json::Wide(installation.error);
    else if (installation.phase == Phase::Incompatible) view.notice = Text(Id::ExtensionStoreIncompatible);
    else if (Busy(installation.phase)) view.notice = Text(Id::ExtensionStorePreparing);
    else if (installation.phase == Phase::Installed) view.notice = Text(Id::ExtensionStoreInstalledRestart);
    // One view item per catalog extension: the item index is the action index (StoreItemBase + i).
    if (catalog.result) for (const auto& extension : catalog.result->catalog.extensions) {
        ui::ExtensionStoreItem item;
        item.name = Localized(extension.name); item.summary = Localized(extension.summary);
        item.icon_glyph = IconGlyph(extension.icon_glyph); item.icon_color = IconColor(extension.icon_color);
        for (const auto& publisher : catalog.result->catalog.publishers)
            if (publisher.id == extension.publisher) { item.publisher = json::Wide(publisher.name); item.official = publisher.official; break; }
        if (extension.versions.empty()) { view.items.push_back(std::move(item)); continue; }
        const auto& release = extension.versions.front();
        item.version = json::Wide(release.version);
        item.size = SizeText(release.package.size);
        item.button_label = Text(Id::DownloadAndInstall);
        const bool compatible = store::IsReleaseCompatible(release, PULSE_VERSION_STRING_A, PULSE_SDK_PREVIEW);
        item.action_enabled = controller->installer && !view.busy && compatible;
        if (!compatible) item.status = Text(Id::ExtensionStoreIncompatible);
        else if (!controller->installer) item.status = Text(Id::ExtensionStoreUnavailable);
        for (const auto& installed : view.installed) {
            if (installed.id != json::Wide(extension.id)) continue;
            item.status = Text(Id::Installed) + L" · " + installed.status;
            // The catalog's newest release is what is installed: nothing to download.
            if (installed.revision == release.manifest_sha256) { item.button_label = Text(Id::Installed); item.action_enabled = false; }
            else item.button_label = Text(Id::StoreUpdateItem);
            break;
        }
        view.items.push_back(std::move(item));
    }
}
bool ExtensionStoreAction(AppState& s, int action) {
    using Action = ui::PackAction;
    if (action == static_cast<int>(Action::StoreBrowse) || action == static_cast<int>(Action::StoreManage) || action == static_cast<int>(Action::StoreInstalled)) {
        s.settings_store_browse = action == static_cast<int>(Action::StoreBrowse);
        s.settings_store_installed = action == static_cast<int>(Action::StoreInstalled);
        s.settings.SetScroll(0, 0); InvalidateRect(s.hwnd, nullptr, FALSE); return true;
    }
    if (action != static_cast<int>(Action::StoreRefresh) && action != static_cast<int>(Action::StoreCancel) && action != static_cast<int>(Action::InstalledRefresh) && action < static_cast<int>(Action::StoreItemBase)) return false;
    const auto controller = Controller(s);
    const auto install_phase = controller->installer ? controller->installer->GetSnapshot().phase : Phase::Idle;
    const bool installing = Busy(install_phase) || install_phase == Phase::AwaitingInstall || install_phase == Phase::AwaitingPublisherTrust;
    if (controller->management && controller->management->Action(s, action, installing)) { InvalidateRect(s.hwnd, nullptr, FALSE); return true; }
    if (action == static_cast<int>(Action::StoreRefresh)) controller->catalog.Refresh(controller->cache);
    else if (action == static_cast<int>(Action::StoreCancel)) {
        controller->dialog.Destroy();
        if (controller->installer) controller->installer->Cancel();
        if (controller->management) controller->management->Cancel();
        if (controller->catalog.GetSnapshot().state == store::CatalogServiceState::Loading) controller->catalog.Cancel();
    } else if (controller->installer) {
        if (installing || (controller->management && controller->management->Active())) return true;
        const auto catalog = controller->catalog.GetSnapshot();
        const auto index = static_cast<size_t>(action - static_cast<int>(Action::StoreItemBase));
        if (catalog.generation != s.store_view_generation || !catalog.result || index >= catalog.result->catalog.extensions.size()) return true;
        const auto& extension = catalog.result->catalog.extensions[index];
        if (!extension.versions.empty()) {
            controller->prompt_owner = s.hwnd; controller->prompt_state = &s;
            controller->consent_missing = false; controller->draft_states.clear(); controller->draft_generation = 0;
            controller->installer->Prepare(catalog.result, extension.id, extension.versions.front().version);
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE); return true;
}
void TickExtensionStoreUi(AppState& s) {
    if (!s.core.extension_store) return;
    const auto& controller = s.core.extension_store;
    const auto catalog = controller->catalog.GetSnapshot();
    const auto installation = controller->installer ? controller->installer->GetSnapshot() : store::InstallServiceSnapshot{};
    const uint64_t token = catalog.generation * 131 + static_cast<unsigned>(catalog.state) * 17 + installation.generation * 8191 +
        static_cast<unsigned>(installation.phase) * 256 + (installation.pending_restart ? 1 : 0);
    if (token != s.store_ui_token) { s.store_ui_token = token; InvalidateRect(s.hwnd, nullptr, FALSE); }
    if (controller->management) {
        if (installation.phase == Phase::Installed && controller->refreshed_installation != installation.generation && !controller->management->Active()) {
            controller->refreshed_installation = installation.generation;
            controller->management->Refresh();
        }
        controller->management->Tick(s);
    }
    controller->Prompt(s);
}
void ReleaseExtensionStoreOwner(AppState& s) {
    const auto& controller = s.core.extension_store;
    if (controller && controller->management) controller->management->ReleaseOwner(s);
    if (!controller || controller->prompt_state != &s || controller->prompt_owner != s.hwnd) return;
    controller->dialog.Destroy(); controller->prompt_owner = nullptr; controller->prompt_state = nullptr;
    if (controller->installer) {
        const auto phase = controller->installer->GetSnapshot().phase;
        if (Busy(phase) || phase == Phase::AwaitingInstall || phase == Phase::AwaitingPublisherTrust) controller->installer->Cancel();
    }
}
void StopExtensionStore(AppCore& core) { if (core.extension_store) core.extension_store->Stop(); }
}
