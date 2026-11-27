#include "extension_store_ui.h"
#include "extension_store_item_state.h"
#include "extension_store_permissions.h"
#include "extension_store_review.h"
#include "extension_store_management_ui.h"
#include "settings_transfer_ui.h"
#include "app_state.h"
#include "extensions/runtime.h"
#include "extensions/icon_registry.h"
#include "extensions/native/command_contributions.h"
#include "extensions/store/catalog_service.h"
#include "extensions/store/install_service.h"
#include "extensions/native/settings_pages_host.h"
#include "extension_store_quick.h"
#include "extension_store_actions.h"
#include "extension_store_edits.h"
#include "extension_developer_mode.h"
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
// Catalog icon/category fallback for the official archive extension (not a UI surface).
constexpr char kSevenZipExtensionId[] = "pulse.7zip";
using Id = l10n::StringId;
namespace store = extensions::store;
using Phase = store::InstallServicePhase;
std::wstring Text(Id id) { return l10n::Get(id); }
std::wstring Localized(const store::LocalizedText& value) {
    const auto language = l10n::effective_language();
    const auto& selected = language == l10n::Language::EnUS ? value.en : language == l10n::Language::ZhTW ? value.tw : value.zh;
    return json::Wide(selected.empty() ? value.en : selected);
}
std::wstring LocalizedManifest(const extensions::ManifestLocalizedText& value) {
    const auto language = l10n::effective_language();
    if (language == l10n::Language::EnUS) return json::Wide(value.en.empty() ? value.zh : value.en);
    if (language == l10n::Language::ZhTW) {
        if (!value.tw.empty()) return json::Wide(value.tw);
        if (!value.zh.empty()) return l10n::HantText(json::Wide(value.zh));
        return json::Wide(value.en);
    }
    return json::Wide(value.zh.empty() ? value.en : value.zh);
}
bool Busy(Phase phase) { return phase == Phase::Preparing || phase == Phase::Committing; }
}
// Review dialog: a built-in icon name for each resource permission row.
static const char* ResourceGlyph(std::string_view resource) {
    if (resource.starts_with("context.")) return "eye";
    if (resource == "files.renameCurrentDirectory") return "rename";
    if (resource == "files.deleteCurrentSelection") return "trash";
    if (resource == "files.open") return "open";
    if (resource == "files.watchDirectory") return "refresh";
    if (resource.starts_with("files.transfer")) return "copy";
    if (resource.starts_with("files.")) return "folder";
    if (resource == "operations.own") return "lines";
    if (resource == "settings.own") return "gear";
    if (resource == "audit.own") return "list-view";
    return "shield";
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
    if (const auto builtin = extensions::icons::ResolveBuiltinIconName(name)) {
        return ui::StoreIconFromId(*builtin);
    }
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
    uint64_t logged_install_generation = 0;
    Phase logged_install_phase = Phase::Idle;
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
        // Review card: heading, the extension as a tile with its trust badge, warnings as info
        // bars, what it will be able to do as a card of rows, and each consent as a toggle card.
        const auto& decl = snapshot.package->Declaration();
        const std::string& pub_id = snapshot.package->PublisherId();
        const auto* review = trust ? nullptr : snapshot.install_review.get();
        const bool is_update = review && !review->active_revision.empty();
        ui::FormDialogSpec spec;
        spec.title = Text(trust ? Id::ExtensionStoreTrustTitle : Id::ExtensionStoreInstallTitle);
        spec.heading = Text(trust ? Id::StoreReviewTrustHeading : is_update ? Id::StoreReviewUpdateHeading : Id::StoreReviewInstallHeading);
        spec.icon = trust ? ui::FormDialogIcon::Key : pub_id.empty() ? ui::FormDialogIcon::Warning : ui::FormDialogIcon::Lock;
        spec.buttons = {{Text(trust ? Id::ExtensionStoreTrustAccept : Id::ExtensionStoreInstallAccept), ui::FormDialogButtonKind::Primary}, {Text(Id::Cancel)}};
        spec.default_button = -1; spec.cancel_button = 1; spec.width_dip = 580;
        if (!trust) { spec.footnote = Text(Id::StoreReviewManageNote); spec.footnote_icon = ui::FormDialogIcon::Info; }

        // The subject card: tile from the catalog entry when there is one.
        spec.subject_kind = ui::FormDialogSubject::Extension;
        spec.subject = json::Wide(decl.name);
        ui::StoreIcon tile = ui::StoreIcon::Puzzle;
        uint32_t tile_color = 0x5B6B8C;
        std::wstring pub_name;
        if (const auto cat = catalog.GetSnapshot().result) {
            for (const auto& ext : cat->catalog.extensions) {
                if (ext.id != decl.id) continue;
                tile = IconGlyph(ext.icon_glyph); tile_color = IconColor(ext.icon_color);
                break;
            }
            for (const auto& p : cat->catalog.publishers) {
                if (p.id == pub_id) { pub_name = json::Wide(p.name); break; }
            }
        }
        if (decl.id == kSevenZipExtensionId) { tile = ui::StoreIcon::Box; tile_color = 0x527D70; }
        else if (decl.id == "pulse.desktop") { tile = ui::StoreIcon::Pc; tile_color = 0x3F6AD8; }
        spec.subject_tile_icon = static_cast<int>(tile);
        spec.subject_tile_color = tile_color;
        std::wstring detail_line;
        if (!snapshot.version.empty()) detail_line = ReplaceStoreToken(Text(Id::StoreReviewVersion), L"{v}", json::Wide(snapshot.version));
        const auto add_detail = [&](const std::wstring& part) {
            if (part.empty()) return;
            if (!detail_line.empty()) detail_line += L" \u00B7 ";
            detail_line += part;
        };
        add_detail(Text(snapshot.is_local_package ? Id::StoreSourceLocalFile : Id::StoreSourceCatalog));
        if (pub_id.empty()) {
            spec.subject_badge = Text(Id::StoreReviewBadgeUnsigned); spec.subject_badge_kind = 3;
        } else if (pub_id == store::kOfficialCatalogPublisher) {
            spec.subject_badge = Text(Id::StoreOfficial); spec.subject_badge_kind = 2;
        } else {
            add_detail(pub_name);
            spec.subject_badge = Text(trust ? Id::StoreReviewBadgeNewKey : Id::StoreReviewBadgeVerified);
            spec.subject_badge_kind = trust ? 1 : 2;
        }
        spec.subject_detail = detail_line;

        extensions::UiTree tree; tree.root.id = "review"; tree.root.gap = 10;
        const auto info = [&](const char* id, const char* kind, std::wstring title, std::wstring message) {
            extensions::UiNode node; node.type = extensions::UiNodeType::InfoBar; node.id = id; node.info_kind = kind;
            node.text = json::Utf8(title); node.message = json::Utf8(message);
            tree.root.children.push_back(std::move(node));
        };
        if (!trust && consent_missing) info("consent_missing", "error", Text(Id::ExtensionStoreConsentRequired), {});
        if (trust) {
            const std::wstring key = ReplaceStoreToken(Text(Id::StorePublisherNewKey), L"{pin}", AbbreviateHexDigest(pub_id));
            info("trust", "informational", Text(Id::ExtensionStoreTrustMessage), key + L"\n" + Text(Id::StoreTrustKeyHint));
        }
        if (!trust && pub_id.empty() && !(review && review->replaces_signed)) info("unsigned_warning", "warning", Text(Id::StoreReviewUnsignedTitle), Text(Id::StoreReviewUnsignedBody));
        if (review && review->replaces_signed) info("replaces_signed", "warning", Text(Id::StoreReviewReplacesTitle), Text(Id::StoreUnsignedReplacesSigned));
        if (review && review->publisher_rotated) {
            info("publisher_rotation", "warning", Text(Id::StoreReviewRotatedTitle),
                 ReplaceStoreToken(Text(Id::ExtensionStorePublisherRotated), L"{old}", AbbreviateHexDigest(review->previous_publisher_id)));
        }

        bool native = false, resources = false, is_new = false;
        if (review) {
            native = review->native_code; resources = !review->added_resources.empty(); is_new = !is_update;
            // What it can do: one row per capability, in a card.
            extensions::UiNode card; card.type = extensions::UiNodeType::Group; card.id = "capabilities"; card.bordered = true; card.gap = 4;
            card.text = json::Utf8(Text(is_update ? Id::StoreReviewNewPermissions : Id::StoreReviewCanDo));
            const auto row = [&](const char* glyph, const std::wstring& text, const std::wstring& detail) {
                extensions::UiNode r; r.type = extensions::UiNodeType::SettingsRow; r.glyph = std::string("icon:") + glyph; r.bordered = false;
                r.text = json::Utf8(text); r.detail = json::Utf8(detail);
                card.children.push_back(std::move(r));
            };
            if (native) row("cube", Text(Id::StoreReviewNativeRow), Text(Id::StoreReviewNativeDetail));
            for (const auto& permission : review->added_resources) {
                row(ResourceGlyph(permission), LocalizedResourcePermission(permission), {});
            }
            if (!native && !resources) row("check", Text(is_update ? Id::StorePermissionsUnchanged : Id::StorePermissionsNone), {});
            tree.root.children.push_back(std::move(card));

            if (native || resources || is_new) {
                extensions::UiNode heading; heading.type = extensions::UiNodeType::Text; heading.text_role = "section";
                heading.text = json::Utf8(Text(Id::StoreReviewConfirm));
                tree.root.children.push_back(std::move(heading));
            }
            const auto consent = [&](const char* key, Id label, const std::wstring& detail) {
                extensions::UiNode node; node.type = extensions::UiNodeType::OptionCard; node.option_kind = "toggle";
                node.id = key; node.state = key; node.text = json::Utf8(Text(label)); node.detail = json::Utf8(detail);
                tree.root.children.push_back(std::move(node));
            };
            if (native) consent("native", Id::ExtensionStoreNativeConsent, Text(Id::StoreReviewNativeConsentDetail));
            if (resources) consent("resources", Id::ExtensionStoreResourcesConsent, Text(Id::StoreReviewResourcesConsentDetail));
            if (is_new) consent("enable", Id::ExtensionStoreEnableConsent, Text(Id::StoreReviewEnableConsentDetail));
            tree.state = draft_generation == snapshot.generation ? draft_states : std::vector<extensions::UiStateEntry>{{"native", "false"}, {"resources", "false"}, {"enable", "false"}};
        }
        extensions::UiNode id_line; id_line.type = extensions::UiNodeType::Text; id_line.text_role = "caption"; id_line.id = "ext_id";
        std::wstring id_text = ReplaceStoreToken(Text(Id::StoreExtensionIdLabel), L"{id}", json::Wide(decl.id));
        if (!trust && !pub_id.empty() && pub_id != store::kOfficialCatalogPublisher)
            id_text += L"\n" + ReplaceStoreToken(Text(Id::StorePublisherVerified), L"{pin}", AbbreviateHexDigest(pub_id));
        id_line.text = json::Utf8(id_text);
        tree.root.children.push_back(std::move(id_line));
        spec.form = std::move(tree); spec.form_height_dip = trust ? 260.0f : 600.0f;
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
const extensions::Manifest* PackageManifestById(const std::string& id) {
    for (size_t p = 0; p < extensions::PackageCount(); ++p)
        if (const auto* manifest = extensions::PackageManifest(p); manifest && manifest->id == id)
            return manifest;
    return nullptr;
}
void EnrichItemFromManifest(AppState& s, ui::ExtensionStoreItem& item) {
    if (item.settings_package >= 0) {
        item.has_menu_tab = !extensions::native::KnownPackageCommands(s, static_cast<size_t>(item.settings_package)).empty();
        item.plugin_tabs = extensions::native::PackageSettingsExtraTabTitles(s.hwnd, static_cast<size_t>(item.settings_package));
    }
    const auto* manifest = PackageManifestById(item.id);
    if (!manifest) return;
    if (item.name.empty() && !manifest->name.empty()) item.name = json::Wide(manifest->name);
    if (item.permissions.empty() && !manifest->resources.empty()) {
        for (const auto& perm : manifest->resources) {
            item.permissions.push_back(app::LocalizedResourcePermission(perm));
        }
    }
    if (!manifest->showcase) return;
    const auto& sc = *manifest->showcase;
    if (item.version.empty() && !sc.version.empty()) item.version = json::Wide(sc.version);
    if (item.summary.empty()) item.summary = LocalizedManifest(sc.summary);
    if (item.description.empty()) item.description = LocalizedManifest(sc.description);
    if (item.category.empty() && !sc.category.empty()) item.category = json::Wide(sc.category);
    if (item.publisher.empty() && !sc.publisher.empty()) item.publisher = json::Wide(sc.publisher);
    if (item.engine.empty()) item.engine = LocalizedManifest(sc.engine);
    if (item.license.empty() && !sc.license.empty()) item.license = json::Wide(sc.license);
    if (item.homepage.empty() && !sc.homepage.empty()) item.homepage = json::Wide(sc.homepage);
    if (item.source.empty() && !sc.source.empty()) item.source = json::Wide(sc.source);
    if (item.tags.empty()) {
        for (const auto& t : sc.tags) item.tags.push_back(json::Wide(t));
    }
    if (item.highlights.empty()) {
        for (const auto& h : sc.highlights) {
            if (auto text = LocalizedManifest(h); !text.empty()) item.highlights.push_back(std::move(text));
        }
    }
    const bool first_party_product_tile =
        sc.icon_name.empty() && (item.id == kSevenZipExtensionId || item.id == "pulse.desktop");
    if (sc.has_icon_color && !first_party_product_tile) item.icon_color = sc.icon_color;
    if (!sc.icon_name.empty()) {
        if (const auto custom = extensions::icons::GlobalRegistry().ResolveName(manifest->id, sc.icon_name);
            custom && ui::IsCustomExtensionIcon(*custom)) {
            item.custom_icon = *custom;
        }
    } else if (sc.builtin_icon != 0 && item.custom_icon == 0 && !first_party_product_tile) {
        item.icon = sc.builtin_icon == PULSE_ICON_ARCHIVE
            ? ui::StoreIcon::Box
            : ui::StoreIconFromId(sc.builtin_icon);
    }
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
    const bool installing = Busy(installation.phase) || installation.phase == Phase::AwaitingInstall ||
        installation.phase == Phase::AwaitingPublisherTrust;
    view.busy = catalog.state == store::CatalogServiceState::Loading || installing;
    view.installed_busy = installing;
    if (controller->management) {
        controller->management->Fill(s, view, installing);
        view.busy = view.busy || controller->management->Active();
    } else view.installed_error = Text(Id::ExtensionStoreUnavailable);
    // Developer folder rows follow the registry entries (their indices stay action indices).
    app::FillDeveloperRows(s, controller->root, view);
    using Notice = ui::ExtensionStoreView::NoticeKind;
    if (installation.phase == Phase::Failed) {
        view.notice = Text(Id::StoreInstallFailed) + L"\n" + app::LocalizedStoreInstallError(installation.error);
        view.notice_kind = Notice::Error;
    } else if (installation.phase == Phase::Incompatible) {
        view.notice = Text(Id::ExtensionStoreIncompatible);
        view.notice_kind = Notice::Warning;
    } else if (Busy(installation.phase)) {
        view.notice = Text(Id::ExtensionStorePreparing);
    } else if (installation.phase == Phase::Installed) {
        view.install_completed = true;
        if (!view.pending_restart) {
            view.notice = Text(Id::Installed);
            view.notice_kind = Notice::Success;
        }
    } else if (catalog.result && (catalog.result->expired || catalog.result->stale_offline)) {
        view.notice = Text(catalog.result->expired ? Id::StoreCatalogExpired : Id::StoreCatalogStaleOffline);
        view.notice_kind = Notice::Warning;
    }
    // Match immutable manifest identity, never assume the latest release is installed. A package the
    // catalog does not list (installed from a file) has no version of its own: it says so, with its revision.
    const auto local_build = [](const std::string& revision) {
        return Text(Id::StoreLocalBuild) + L" · " + json::Wide(revision.substr(0, 7));
    };
    for (auto& installed : view.installed) {
        bool listed = false;
        if (catalog.result) for (const auto& extension : catalog.result->catalog.extensions) {
            if (installed.id != json::Wide(extension.id)) continue;
            for (const auto& release : extension.versions)
                if (installed.revision == release.manifest_sha256) { installed.version = json::Wide(release.version); listed = true; break; }
            break;
        }
        if (!listed && !installed.revision.empty() && installed.version.empty()) installed.version = local_build(installed.revision);
    }
    // One view item per catalog extension: the item index is the action index (StoreItemBase + i).
    if (catalog.result) for (const auto& extension : catalog.result->catalog.extensions) {
        ui::ExtensionStoreItem item;
        item.name = Localized(extension.name); item.summary = Localized(extension.summary);
        item.description = Localized(extension.description);
        item.id = extension.id; item.category = json::Wide(extension.category);
        item.license = json::Wide(extension.license);
        item.homepage = json::Wide(extension.homepage);
        item.source = json::Wide(extension.source.empty() ? extension.homepage : extension.source);
        for (const auto& t : extension.tags) item.tags.push_back(json::Wide(t));
        for (const auto& release : extension.versions) {
            item.history.push_back(json::Wide(release.version) + (release.released.empty() ? std::wstring{} : L" · " + json::Wide(release.released)));
            item.releases.push_back({json::Wide(release.version),json::Wide(release.released),Localized(release.notes),json::Wide(release.min_pulse)});
        }
        if (!extension.versions.empty()) {
            item.released = json::Wide(extension.versions.front().released);
            for (const auto& perm : extension.versions.front().resources) {
                item.permissions.push_back(app::LocalizedResourcePermission(perm));
            }
        }
        item.icon = IconGlyph(extension.icon_glyph); item.icon_color = IconColor(extension.icon_color);
        if (extension.id == kSevenZipExtensionId) { item.icon = ui::StoreIcon::Box; item.icon_color = 0x527D70; }
        // 桌面整理: a desktop (monitor), not the generic extension piece the catalog entry carries.
        if (extension.id == "pulse.desktop") { item.icon = ui::StoreIcon::Pc; item.icon_color = 0x3F6AD8; }
        for (const auto& publisher : catalog.result->catalog.publishers)
            if (publisher.id == extension.publisher) { item.publisher = json::Wide(publisher.name); item.official = publisher.official; break; }
        if (extension.versions.empty()) { EnrichItemFromManifest(s, item); view.items.push_back(std::move(item)); continue; }
        const auto& release = extension.versions.front();
        item.version = json::Wide(release.version);
        item.size = SizeText(release.package.size);
        item.button_label = Text(Id::DownloadAndInstall);
        const bool compatible = store::IsReleaseCompatible(release, PULSE_VERSION_STRING_A, PULSE_SDK_PREVIEW);
        item.action_enabled = controller->installer && !view.busy && compatible && view.state==State::Online;
        if (!compatible) item.status = Text(Id::ExtensionStoreIncompatible);
        else if (!controller->installer) item.status = Text(Id::ExtensionStoreUnavailable);
        for (size_t idx = 0; idx < view.installed.size(); ++idx) {
            const auto& installed = view.installed[idx];
            if (installed.id != json::Wide(extension.id)) continue;
            // What is installed names itself (a package can be renamed before the catalog is republished).
            if (!installed.name.empty()) item.name = installed.name;
            item.status = Text(Id::Installed) + L" · " + installed.status;
            item.enabled = installed.enabled;
            item.installed = true;
            item.installed_index = static_cast<int>(idx);
            // Its loaded, enabled package may provide a settings page (opened from 设置).
            if (installed.enabled) item.settings_package = SettingsPackage(extension.id);
            // The catalog's newest release is what is installed: nothing to download.
            const bool known_revision = std::any_of(extension.versions.begin(), extension.versions.end(),
                [&](const auto& candidate) { return candidate.manifest_sha256 == installed.revision; });
            app::ApplyInstalledStoreItem(item, known_revision,
                installed.revision == release.manifest_sha256, installed.version);
            break;
        }
        EnrichItemFromManifest(s, item);
        view.items.push_back(std::move(item));
    }
    // Installed settings do not depend on a network catalog. This local row has
    // no package/download action and never supplies install authorization data.
    for (size_t idx = 0; idx < view.installed.size(); ++idx) {
        const auto& installed = view.installed[idx];
        if (installed.development_index >= 0) continue;  // developer folders live on 已安装 only
        const auto id = json::Utf8(installed.id);
        if (std::any_of(view.items.begin(), view.items.end(), [&](const auto& item) { return item.id == id; })) continue;
        ui::ExtensionStoreItem item;
        item.id=id;item.name=installed.name;item.version=installed.version;
        item.publisher=installed.publisher;
        item.enabled=installed.enabled;item.status=installed.status;
        item.installed = true;
        item.installed_index = static_cast<int>(idx);
        item.official=installed.publisher==Text(Id::StoreOfficial);
        item.icon = ui::StoreIcon::Puzzle;
        if (id == kSevenZipExtensionId) { item.category=L"archive";item.icon=ui::StoreIcon::Box; }
        else if (id == "pulse.desktop") { item.icon=ui::StoreIcon::Pc;item.icon_color=0x3F6AD8; }
        if (installed.enabled) item.settings_package=SettingsPackage(id);
        EnrichItemFromManifest(s, item);
        if (!item.official && item.summary.empty()) item.summary = installed.publisher;
        view.items.push_back(std::move(item));
    }
    view.category = s.store_category;
    view.search = s.store_search;
    view.detail = s.store_detail;
    view.detail_tab = s.store_detail_tab;
    view.saved = s.store_saved_at && GetTickCount64() - s.store_saved_at < 1600;
    // Every package, 7-Zip included, uses the manifest quick settings card and the
    // shared detail/settings pages; the store has no per-extension surfaces.
    FillGenericQuickCards(s, view);
    if (view.detail) {
        if (!s.store_detail_id.empty()) {
            for (size_t i = 0; i < view.items.size(); ++i) {
                if (view.items[i].id == s.store_detail_id) {
                    view.detail_item = static_cast<int>(i);
                    break;
                }
            }
        }
        // The detail page needs a backing extension item; otherwise show browse again.
        if (view.detail_item < 0) view.detail = false;
    }
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
    // The file list hands over long-path forms (\\?\C:\...); the local package reader takes
    // plain drive paths only, so opening a .pulse-ext from a folder failed (local_package_path_invalid).
    if (path.size() > 7 && path.starts_with(L"\\\\?\\") && path[5] == L':' && (path[6] == L'\\' || path[6] == L'/'))
        path.erase(0, 4);
    s.store_detail = false;
    s.store_detail_id.clear();
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
std::filesystem::path ExtensionStoreRoot(AppState& s) { return Controller(s)->root; }
std::vector<std::string> DeveloperTakenIds(AppState& s) {
    std::vector<std::string> ids;
    ui::WindowViewModel vm;
    FillExtensionStoreView(s, vm);
    for (const auto& installed : vm.settings_store_view.installed)
        if (installed.development_index < 0) ids.push_back(json::Utf8(installed.id));
    for (size_t p = 0; p < extensions::PackageCount() && p < 10000; ++p)
        if (const auto* manifest = extensions::PackageManifest(p); manifest && !extensions::PackageFromDeveloperFolder(p))
            ids.push_back(manifest->id);
    return ids;
}
bool ExtensionStoreAction(AppState& s, int action) {
    using Action = ui::PackAction;
    if (action == static_cast<int>(Action::StoreRestartNow)) {
        const auto controller = Controller(s);
        const auto phase = controller->installer ? controller->installer->GetSnapshot().phase : Phase::Idle;
        if (Busy(phase) || phase == Phase::AwaitingInstall || phase == Phase::AwaitingPublisherTrust ||
            (controller->management && controller->management->Active())) return true;
        RestartPulse(s);
        return true;
    }
    if (action == static_cast<int>(Action::StoreDeveloper)) {
        const auto controller = Controller(s);
        const auto phase = controller->installer ? controller->installer->GetSnapshot().phase : Phase::Idle;
        if (Busy(phase) || phase == Phase::AwaitingInstall || phase == Phase::AwaitingPublisherTrust ||
            (controller->management && controller->management->Active())) return true;
        return app::DeveloperButtonAction(s, controller->root, DeveloperTakenIds(s));
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
    // Before the detail handler: it claims every action from StoreChipBase up.
    if (action >= static_cast<int>(Action::NativeTabBase) && action <= static_cast<int>(Action::NativeTabBase) + 3) {
        const int tab = action - static_cast<int>(Action::NativeTabBase);
        if (tab <= 1) {
            if (tab == 0) extensions::native::SetFocusedSettingsPluginTab(s.hwnd, 0);
            extensions::native::SetFocusedSettingsMode(s.hwnd, tab);
        } else {
            const auto focused = extensions::native::FocusedSettings(s.hwnd);
            if (!focused.package_id.empty()) {
                s.store_detail = true;
                s.store_detail_id = focused.package_id;
                s.store_detail_tab = tab == 3 ? 1 : 0;
                s.settings_store_browse = true;
                s.settings_store_installed = false;
                OpenSettingsTab(s, 8);
                s.settings.SetScroll(0, 0);
                InvalidateRect(s.hwnd, nullptr, FALSE);
            }
        }
        return true;
    }
    if (action >= static_cast<int>(Action::NativeCustomTabBase) && action < static_cast<int>(Action::NativeCustomTabBase) + 6) {
        const int custom_index = action - static_cast<int>(Action::NativeCustomTabBase);
        extensions::native::SetFocusedSettingsPluginTab(s.hwnd, 1 + custom_index);
        extensions::native::SetFocusedSettingsMode(s.hwnd, 0);
        return true;
    }
    if (ExtensionStoreDetailAction(s, action)) return true;
    if (action >= static_cast<int>(Action::StoreItemSettingsBase) && action < static_cast<int>(Action::StoreItemSettingsBase) + 10000) {
        // 设置 on a card: the plugin settings page, opened on that package. The view
        // as last built names the package; a stale index just opens the page.
        const auto index = static_cast<size_t>(action - static_cast<int>(Action::StoreItemSettingsBase));
        const auto catalog = Controller(s)->catalog.GetSnapshot();
        if (catalog.revision == s.store_view_generation && catalog.result && index < catalog.result->catalog.extensions.size()) {
            const int package = SettingsPackage(catalog.result->catalog.extensions[index].id);
            if (package >= 0) extensions::native::RequestSettingsPagesPackage(s.hwnd, static_cast<size_t>(package));
        }
        const size_t catalog_count=catalog.result ? catalog.result->catalog.extensions.size() : 0;
        if(catalog.revision==s.store_view_generation && index>=catalog_count) {
            ui::WindowViewModel current;
            FillExtensionStoreView(s,current);
            if(index<current.settings_store_view.items.size()) {
                const auto& item=current.settings_store_view.items[index];
                if(item.settings_package>=0)
                    extensions::native::RequestSettingsPagesPackage(s.hwnd,static_cast<size_t>(item.settings_package));
            }
        }
        OpenSettingsTab(s, ui::kSettingsPageNativeExtensions);
        return true;
    }
    if (action == static_cast<int>(Action::StoreBrowse) || action == static_cast<int>(Action::StoreManage) || action == static_cast<int>(Action::StoreInstalled)) {
        s.store_detail = false;
        s.store_detail_id.clear();
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
    if (controller->installer &&
        installation.generation != 0 &&
        (controller->logged_install_generation != installation.generation ||
         controller->logged_install_phase != installation.phase)) {
        controller->logged_install_generation = installation.generation;
        controller->logged_install_phase = installation.phase;
        const bool log_phase = installation.phase == Phase::Preparing ||
            installation.phase == Phase::Committing ||
            installation.phase == Phase::Installed ||
            installation.phase == Phase::Failed ||
            installation.phase == Phase::Incompatible;
        if (log_phase) {
            const auto id_w = json::Wide(installation.id);
            const auto version_w = json::Wide(installation.version);
            const auto error_w = json::Wide(installation.error);
            const auto level = installation.phase == Phase::Failed
                ? diagnostics::runtime::Level::Error
                : installation.phase == Phase::Incompatible
                ? diagnostics::runtime::Level::Warning
                : diagnostics::runtime::Level::Info;
            const std::initializer_list<diagnostics::runtime::Field> fields = {
                {"generation", installation.generation},
                {"phase", static_cast<uint64_t>(installation.phase)},
                {"local", installation.is_local_package ? 1ULL : 0ULL},
                {"pending_restart", installation.pending_restart ? 1ULL : 0ULL}
            };
            if (error_w.empty()) {
                diagnostics::runtime::TextEvent("extension_install_status",
                    {{"id", id_w}, {"version", version_w}}, fields, level);
            } else {
                diagnostics::runtime::TextEvent("extension_install_status",
                    {{"id", id_w}, {"version", version_w}, {"error", error_w}}, fields, level);
            }
        }
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