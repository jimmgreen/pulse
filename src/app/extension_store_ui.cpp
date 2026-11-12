#include "extension_store_ui.h"
#include "extension_store_permissions.h"
#include "extension_store_review.h"
#include "extension_store_management_ui.h"
#include "settings_transfer_ui.h"
#include "app_state.h"
#include "extensions/runtime.h"
#include "extensions/store/catalog_service.h"
#include "extensions/store/install_service.h"
#include "extensions/native/settings_pages_host.h"
#include "extension_store_seven_zip.h"
#include "extension_store_actions.h"
#include "extension_store_edits.h"
#include "app_navigation.h"
#include "../ui/settings_page_ids.h"
#include "../ui/ui_view_model.h"
#include "../ui/form_dialog.h"
#include "../common/localization.h"
#include "../common/json_value.h"
#include "../common/runtime_log.h"
#include "../../sdk/native/include/pulse/abi.h"
#include "pulse_version.h"
#include <shobjidl.h>
#include <algorithm>
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
// Catalog icon names use the store's vector icon contract.
static ui::StoreIcon IconGlyph(const std::string& name) {
    using Icon = ui::StoreIcon;
    static const std::pair<const char*, Icon> icons[] = {
        {"archive", Icon::Box}, {"compress", Icon::Compress}, {"film", Icon::Film},
        {"image", Icon::Image}, {"doc", Icon::File}, {"find", Icon::Search},
        {"ocr", Icon::TextSearch}, {"git", Icon::Copy}, {"rename", Icon::Rename},
        {"duplicate", Icon::Duplicate}, {"cloud", Icon::Download}, {"puzzle", Icon::Puzzle}};
    for (const auto& [key, icon] : icons) if (name == key) return icon;
    return Icon::Box;
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
        // A package signed by the pinned official publisher comes from the store itself: nothing to ask,
        // unless an explicit publisher key rotation requires user visibility.
        if (snapshot.package->PublisherId() == store::kOfficialCatalogPublisher &&
            (!snapshot.install_review || !snapshot.install_review->publisher_rotated)) {
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
        const auto& decl = snapshot.package->Declaration();
        tree.root.children.push_back(Label("package", json::Wide(decl.name)));
        tree.root.children.push_back(Label("source", Text(snapshot.is_local_package ? Id::StoreSourceLocalFile : Id::StoreSourceCatalog)));
        const std::string& pub_id = snapshot.package->PublisherId();
        std::wstring publisher_status;
        if (pub_id.empty()) {
            publisher_status = Text(Id::StorePublisherUnsigned);
        } else if (pub_id == store::kOfficialCatalogPublisher) {
            publisher_status = Text(Id::StoreOfficial);
        } else {
            std::wstring pub_name;
            if (const auto cat = catalog.GetSnapshot().result) {
                for (const auto& p : cat->catalog.publishers) {
                    if (p.id == pub_id) { pub_name = json::Wide(p.name); break; }
                }
            }
            const std::wstring key_label = ReplaceStoreToken(
                Text(trust ? Id::StorePublisherNewKey : Id::StorePublisherVerified),
                L"{id}", AbbreviateHexDigest(pub_id));
            publisher_status = pub_name.empty() ? key_label : (pub_name + L" · " + key_label);
        }
        tree.root.children.push_back(Label("publisher", Text(Id::Publisher) + L": " + publisher_status));
        if (!trust && pub_id.empty()) {
            tree.root.children.push_back(Label("unsigned_warning", Text(Id::StoreUnsignedLocalWarning)));
        }
        if (!trust && snapshot.install_review && snapshot.install_review->publisher_rotated) {
            std::wstring rotated = Text(Id::ExtensionStorePublisherRotated);
            const auto prev_short = AbbreviateHexDigest(snapshot.install_review->previous_publisher_id);
            rotated = ReplaceStoreToken(std::move(rotated), L"{old}", prev_short);
            tree.root.children.push_back(Label("publisher_rotation", rotated));
        }
        extensions::UiNode scroll; scroll.type = extensions::UiNodeType::ScrollView; scroll.id = "details"; scroll.min_height = 96; scroll.max_height = 144;
        std::wstring details = ReplaceStoreToken(Text(Id::StoreExtensionIdLabel), L"{id}", json::Wide(decl.id));
        if (trust) {
            details += L"\n" + Text(Id::StoreTrustKeyHint);
        } else if (snapshot.install_review) {
            if (!snapshot.install_review->added_resources.empty()) {
                details += L"\n" + Text(Id::StorePermissionsAddedHeading);
                for (const auto& permission : snapshot.install_review->added_resources) {
                    details += L"\n• " + LocalizedResourcePermission(permission);
                }
            } else {
                details += L"\n" + Text(snapshot.install_review->active_revision.empty()
                    ? Id::StorePermissionsNone
                    : Id::StorePermissionsUnchanged);
            }
        }
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
// The running, enabled package of a catalog extension, or -1.
int SettingsPackage(const std::string& id) {
    for (size_t p = 0; p < extensions::PackageCount() && p < 10000; ++p)
        if (const auto* manifest = extensions::PackageManifest(p); manifest && manifest->id == id && extensions::PackageEnabled(p))
            return static_cast<int>(p);
    return -1;
}
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
    s.store_view_generation = catalog.revision;
    auto& view = vm.settings_store_view;
    view = {};
    using State = ui::ExtensionStoreView::State;
    switch (catalog.state) {
    case store::CatalogServiceState::Loading: view.state = catalog.result ? State::Online : State::Loading; break;
    case store::CatalogServiceState::Online: view.state = State::Online; break;
    case store::CatalogServiceState::Offline: view.state = State::Offline; break;
    case store::CatalogServiceState::Failed: view.state = catalog.result ? State::Offline : State::Failed; break;
    case store::CatalogServiceState::Cancelled: view.state = catalog.result ? State::Online : State::Idle; break;
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
    if (installation.phase == Phase::Failed) view.notice = Text(Id::StoreInstallFailed) + L"\n" + app::LocalizedStoreInstallError(installation.error);
    else if (installation.phase == Phase::Incompatible) view.notice = Text(Id::ExtensionStoreIncompatible);
    else if (Busy(installation.phase)) view.notice = Text(Id::ExtensionStorePreparing);
    else if (installation.phase == Phase::Installed) view.notice = Text(Id::ExtensionStoreInstalledRestart);
    else if (catalog.result && catalog.result->expired) view.notice = Text(Id::StoreCatalogExpired);
    else if (catalog.result && catalog.result->stale_offline) view.notice = Text(Id::StoreCatalogStaleOffline);
    // Match immutable manifest identity, never assume the latest release is installed.
    if (catalog.result) for (auto& installed : view.installed) {
        for (const auto& extension : catalog.result->catalog.extensions) {
            if (installed.id != json::Wide(extension.id)) continue;
            for (const auto& release : extension.versions)
                if (installed.revision == release.manifest_sha256) { installed.version = json::Wide(release.version); break; }
            break;
        }
    }
    // One view item per catalog extension: the item index is the action index (StoreItemBase + i).
    if (catalog.result) for (const auto& extension : catalog.result->catalog.extensions) {
        ui::ExtensionStoreItem item;
        item.name = Localized(extension.name); item.summary = Localized(extension.summary);
        item.id = extension.id; item.category = json::Wide(extension.category);
        item.license = json::Wide(extension.license); item.source = json::Wide(extension.source.empty() ? extension.homepage : extension.source);
        for (const auto& release : extension.versions) {
            item.history.push_back(json::Wide(release.version) + (release.released.empty() ? std::wstring{} : L" · " + json::Wide(release.released)));
            item.releases.push_back({json::Wide(release.version),json::Wide(release.released),Localized(release.notes),json::Wide(release.min_pulse)});
        }
        if (!extension.versions.empty()) item.released = json::Wide(extension.versions.front().released);
        item.icon = IconGlyph(extension.icon_glyph); item.icon_color = IconColor(extension.icon_color);
        if (extension.id == kSevenZipExtensionId) { item.icon = ui::StoreIcon::Box; item.icon_color = 0x527D70; }
        for (const auto& publisher : catalog.result->catalog.publishers)
            if (publisher.id == extension.publisher) { item.publisher = json::Wide(publisher.name); item.official = publisher.official; break; }
        if (extension.versions.empty()) { view.items.push_back(std::move(item)); continue; }
        const auto& release = extension.versions.front();
        item.version = json::Wide(release.version);
        item.size = SizeText(release.package.size);
        item.button_label = Text(Id::DownloadAndInstall);
        const bool compatible = store::IsReleaseCompatible(release, PULSE_VERSION_STRING_A, PULSE_SDK_PREVIEW);
        item.action_enabled = controller->installer && !view.busy && compatible && view.state==State::Online;
        if (!compatible) item.status = Text(Id::ExtensionStoreIncompatible);
        else if (!controller->installer) item.status = Text(Id::ExtensionStoreUnavailable);
        for (const auto& installed : view.installed) {
            if (installed.id != json::Wide(extension.id)) continue;
            item.status = Text(Id::Installed) + L" · " + installed.status;
            item.enabled = installed.enabled;
            // Its loaded, enabled package may provide a settings page (opened from 设置).
            if (installed.enabled) item.settings_package = SettingsPackage(extension.id);
            // The catalog's newest release is what is installed: nothing to download.
            const bool known_revision = std::any_of(extension.versions.begin(), extension.versions.end(),
                [&](const auto& candidate) { return candidate.manifest_sha256 == installed.revision; });
            // An unlisted local build is not evidence of an older version.
            // Do not offer the public catalog's older package as its "update".
            if (!known_revision || installed.revision == release.manifest_sha256) { item.button_label = Text(Id::Installed); item.action_enabled = false; }
            else item.button_label = Text(Id::StoreUpdateItem);
            break;
        }
        view.items.push_back(std::move(item));
    }
    // Installed settings do not depend on a network catalog. This local row has
    // no package/download action and never supplies install authorization data.
    if(std::none_of(view.items.begin(),view.items.end(),[](const auto& item){return item.id==kSevenZipExtensionId;})) {
        for(const auto& installed:view.installed) if(installed.id==json::Wide(kSevenZipExtensionId)) {
            ui::ExtensionStoreItem item;
            item.id=kSevenZipExtensionId; item.name=installed.name; item.version=installed.version;
            item.publisher=installed.publisher; item.enabled=installed.enabled; item.status=installed.status;
            item.category=L"archive"; item.icon=ui::StoreIcon::Box;
            if(installed.native_code) item.summary=Text(Id::StoreNativeCode);
            if(installed.enabled) item.settings_package=SettingsPackage(kSevenZipExtensionId);
            view.items.push_back(std::move(item)); break;
        }
    }
    view.category = s.store_category;
    view.search = s.store_search;
    view.detail = s.store_detail;
    view.detail_tab = s.store_detail_tab;
    view.preview_kind = s.store_preview_kind;
    view.saved = s.store_saved_at && GetTickCount64() - s.store_saved_at < 1600;
    view.level_drag = s.store_level_drag;
    FillSevenZipView(s, view);
    // The detail page needs the extension; without it the store shows browse again.
    if (view.detail && view.seven_zip.item < 0) view.detail = false;
}
bool IsExtensionPackageFilePath(std::wstring_view path) {
    while (!path.empty() && path.front() == L'"') path.remove_prefix(1);
    while (!path.empty() && path.back() == L'"') path.remove_suffix(1);
    constexpr std::wstring_view kSuffix = L".pulse-ext";
    if (path.size() <= kSuffix.size()) return false;
    const auto tail = path.substr(path.size() - kSuffix.size());
    for (size_t i = 0; i < kSuffix.size(); ++i) {
        wchar_t c = tail[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c != kSuffix[i]) return false;
    }
    const wchar_t prev = path[path.size() - kSuffix.size() - 1];
    return prev != L'\\' && prev != L'/' && prev != L':';
}
bool OpenExtensionPackageInStore(AppState& s, const std::wstring& raw_path) {
    std::wstring path = raw_path;
    while (!path.empty() && path.front() == L'"') path.erase(path.begin());
    while (!path.empty() && path.back() == L'"') path.pop_back();
    if (!IsExtensionPackageFilePath(path)) return false;
    s.store_detail = false;
    if (!s.settings_store_browse && !s.settings_store_installed) {
        s.settings_store_browse = true;
    }
    if (s.settings.page() != 8) OpenSettingsTab(s, 8);
    s.settings.SetScroll(0, 0);
    const auto controller = Controller(s);
    if (!controller->installer) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    const auto phase = controller->installer->GetSnapshot().phase;
    const bool installing = Busy(phase) || phase == Phase::AwaitingInstall || phase == Phase::AwaitingPublisherTrust;
    if (installing || (controller->management && controller->management->Active())) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    }
    const auto catalog = controller->catalog.GetSnapshot();
    controller->prompt_owner = s.hwnd;
    controller->prompt_state = &s;
    controller->consent_missing = false;
    controller->draft_states.clear();
    controller->draft_generation = 0;
    controller->installer->PrepareLocal(std::filesystem::path(path), {}, catalog.result);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
namespace {
std::wstring PickExtensionPackageFile(HWND owner) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = SUCCEEDED(co);
    std::wstring chosen;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
        }
        const std::wstring title = Text(Id::StoreInstallFileDialogTitle);
        const std::wstring filter_label = Text(Id::StoreInstallFileFilter);
        const COMDLG_FILTERSPEC filter{filter_label.c_str(), L"*.pulse-ext"};
        dlg->SetTitle(title.c_str());
        dlg->SetFileTypes(1, &filter);
        dlg->SetFileTypeIndex(1);
        dlg->SetDefaultExtension(L"pulse-ext");
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                    chosen = psz;
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (uninit) CoUninitialize();
    return chosen;
}
}
bool ExtensionStoreAction(AppState& s, int action) {
    using Action = ui::PackAction;
    if (action == static_cast<int>(Action::StoreRestartNow)) {
        RestartPulse(s);
        return true;
    }
    if (action == static_cast<int>(Action::StoreInstallFromFile)) {
        const auto controller = Controller(s);
        const auto phase = controller->installer ? controller->installer->GetSnapshot().phase : Phase::Idle;
        const bool installing = Busy(phase) || phase == Phase::AwaitingInstall || phase == Phase::AwaitingPublisherTrust;
        if (installing || (controller->management && controller->management->Active())) return true;
        const std::wstring chosen = PickExtensionPackageFile(s.hwnd);
        if (!chosen.empty()) OpenExtensionPackageInStore(s, chosen);
        return true;
    }
    if (ExtensionStoreDetailAction(s, action)) return true;
    if (action >= static_cast<int>(Action::StoreItemSettingsBase) && action < static_cast<int>(Action::StoreItemSettingsBase) + 10000) {
        // 设置 on a card: the plugin settings page, opened on that package. The view
        // as last built names the package; a stale index just opens the page.
        const auto index = static_cast<size_t>(action - static_cast<int>(Action::StoreItemSettingsBase));
        const auto catalog = Controller(s)->catalog.GetSnapshot();
        if (catalog.revision == s.store_view_generation && catalog.result && index < catalog.result->catalog.extensions.size()) {
            if (catalog.result->catalog.extensions[index].id == kSevenZipExtensionId) {
                s.store_detail = true; s.store_detail_tab = 0;
                s.settings_store_browse = true; s.settings_store_installed = false;
                if (s.settings.page() != 8) OpenSettingsTab(s, 8);
                s.settings.SetScroll(0, 0); InvalidateRect(s.hwnd, nullptr, FALSE);
                return true;
            }
            const int package = SettingsPackage(catalog.result->catalog.extensions[index].id);
            if (package >= 0) extensions::native::RequestSettingsPagesPackage(s.hwnd, static_cast<size_t>(package));
        }
        const size_t catalog_count=catalog.result ? catalog.result->catalog.extensions.size() : 0;
        if(catalog.revision==s.store_view_generation && index==catalog_count) {
            ui::WindowViewModel current;
            FillExtensionStoreView(s,current);
            if(index<current.settings_store_view.items.size() && current.settings_store_view.items[index].id==kSevenZipExtensionId) {
                s.store_detail=true; s.store_detail_tab=0;
                s.settings_store_browse=true; s.settings_store_installed=false;
                if(s.settings.page()!=8) OpenSettingsTab(s,8);
                s.settings.SetScroll(0,0); InvalidateRect(s.hwnd,nullptr,FALSE);
                return true;
            }
        }
        OpenSettingsTab(s, ui::kSettingsPageNativeExtensions);
        return true;
    }
    if (action == static_cast<int>(Action::StoreBrowse) || action == static_cast<int>(Action::StoreManage) || action == static_cast<int>(Action::StoreInstalled)) {
        s.store_detail = false;
        s.settings_store_browse = action == static_cast<int>(Action::StoreBrowse);
        s.settings_store_installed = action == static_cast<int>(Action::StoreInstalled);
        // From the plugin settings page (its "‹ 扩展商店"), go back to the store itself.
        if (s.settings.page() != 8) OpenSettingsTab(s, 8);
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
        if (catalog.revision != s.store_view_generation || !catalog.result || index >= catalog.result->catalog.extensions.size()) return true;
        ui::WindowViewModel current;
        FillExtensionStoreView(s, current);
        if (catalog.revision != s.store_view_generation || index >= current.settings_store_view.items.size() ||
            !current.settings_store_view.items[index].action_enabled) return true;
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
    if (s.store_saved_at && GetTickCount64() - s.store_saved_at >= 1600) {
        s.store_saved_at = 0; InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    if (!s.core.extension_store) return;
    const auto& controller = s.core.extension_store;
    const auto catalog = controller->catalog.GetSnapshot();
    const auto installation = controller->installer ? controller->installer->GetSnapshot() : store::InstallServiceSnapshot{};
    const uint64_t token = catalog.revision * 131 + static_cast<unsigned>(catalog.state) * 17 + installation.generation * 8191 +
        static_cast<unsigned>(installation.phase) * 256 + (installation.pending_restart ? 1 : 0);
    if (token != s.store_ui_token) {
        s.store_ui_token = token;
        const auto diagnostic=catalog.state!=store::CatalogServiceState::Cancelled && !catalog.error.empty() ? catalog.error :
            catalog.result ? catalog.result->diagnostic : std::string{};
        if(!diagnostic.empty()) diagnostics::runtime::TextEvent("extension_catalog_status",
            {{"diagnostic",json::Wide(diagnostic)}},{{"generation",catalog.generation},{"revision",catalog.revision},
            {"state",static_cast<uint64_t>(catalog.state)}},diagnostics::runtime::Level::Warning);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
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
    DestroyStoreEdits(s);
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
