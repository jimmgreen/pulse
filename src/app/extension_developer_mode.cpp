// extension_developer_mode.cpp — 开发者模式 (devmode_plan.md M1/M2), see the header.
#include "extension_developer_mode.h"
#include "about_info.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_state.h"
#include "settings_transfer_ui.h"
#include "extensions/developer_mode.h"
#include "extensions/manifest.h"
#include "extensions/native_image_verify.h"
#include "extensions/package_revision.h"
#include "extensions/runtime.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "../ui/extension_store_view.h"
#include "../ui/fluent_menu.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <set>

namespace pulse::app {
namespace {
namespace fs = std::filesystem;
namespace devmode = extensions::devmode;
using Id = l10n::StringId;
std::wstring Text(Id id) { return l10n::Get(id); }
std::wstring Replace(std::wstring text, std::wstring_view token, std::wstring_view value) {
    if (const auto at = text.find(token); at != std::wstring::npos) text.replace(at, token.size(), value);
    return text;
}

struct Cache {
    std::wstring root;
    bool setting_loaded = false, setting = false;
    bool entries_loaded = false;
    std::vector<devmode::DevPackageEntry> entries;
    std::set<std::string> changed;   // ids added, reloaded or removed by this process
    std::string error_id, error;     // last failed action on a row
    std::wstring error_detail;
};
Cache& State(const fs::path& root) {
    static Cache cache;
    if (cache.root != root.native()) { cache = {}; cache.root = root.native(); }
    return cache;
}
const std::vector<devmode::DevPackageEntry>& Entries(const fs::path& root) {
    auto& state = State(root);
    if (!state.entries_loaded) { state.entries = devmode::ReadRegistry(root); state.entries_loaded = true; }
    return state.entries;
}

bool WriteAtomic(const fs::path& path, std::string_view bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const auto temp = path.native() + L".tmp";
    {
        std::ofstream out(fs::path(temp), std::ios::binary | std::ios::trunc);
        if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !out.flush()) {
            error = "dev_write_failed"; return false;
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "dev_write_failed:" + std::to_string(GetLastError());
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}
bool WriteRegistry(const fs::path& root, const std::vector<devmode::DevPackageEntry>& entries, std::string& error) {
    auto packages = json::Value::Array();
    for (const auto& entry : entries) {
        auto item = json::Value::Object();
        item.set("id", entry.id);
        item.set("source", json::Utf8(entry.source));
        item.set("revision", entry.revision);
        packages.push(std::move(item));
    }
    auto value = json::Value::Object();
    value.set("formatVersion", 1);
    value.set("packages", std::move(packages));
    if (!WriteAtomic(devmode::DeveloperRoot(root) / L"dev-packages.json", json::Serialize(value) + "\n", error)) return false;
    auto& state = State(root);
    state.entries = entries;
    state.entries_loaded = true;
    return true;
}
bool LocalFolder(const fs::path& path) {
    const auto drive = path.root_name().wstring();
    if (!path.is_absolute() || drive.size() != 2 || drive[1] != L':') return false;
    const UINT type = GetDriveTypeW(path.root_path().c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
}
bool ReadPayload(const fs::path& path, std::string& bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto size = in.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(extensions::kMaxNativeImageBytes)) return false;
    bytes.assign(static_cast<size_t>(size), '\0');
    in.seekg(0);
    return static_cast<bool>(in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())));
}
std::wstring StartupError(const std::string& id) {
    for (const auto& [failed, error] : extensions::DeveloperFolderLoadErrors())
        if (failed == id) return LocalizedDeveloperError(error);
    return {};
}
void ShowMessage(AppState& s, const std::wstring& title, const std::wstring& message) {
    ui::ConfirmDialogSpec spec;
    spec.title = title;
    spec.message = message;
    spec.confirm_text = Text(Id::Close);
    spec.single_button = true;
    spec.tone = ui::ConfirmTone::Warning;
    ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
}
int Menu(AppState& s, std::vector<ui::FluentMenuItem> items) {
    if (!EnsureMenu(s)) return 0;
    s.menu->SetTheme(s.darkMode, s.accentColor);
    POINT anchor{};
    GetCursorPos(&anchor);
    return s.menu->TrackPopup(anchor, std::move(items));
}
// Load from folder: pick, confirm, copy, then the quick restart picks it up.
bool LoadFolder(AppState& s, const fs::path& root, const std::vector<std::string>& taken_ids) {
    std::wstring folder;
    if (!PickFolder(s, folder, Text(Id::StoreDeveloperLoadFolder).c_str(), L"extension_dev_folder") || folder.empty()) return true;
    ui::ConfirmDialogSpec spec;
    spec.title = Text(Id::StoreDevLoadTitle);
    spec.message = Text(Id::StoreDevLoadPrompt);
    spec.items = {folder};
    spec.item_glyph = L"\xE8B7";  // Folder
    spec.confirm_text = Text(Id::StoreDevLoadConfirm);
    spec.tone = ui::ConfirmTone::Warning;
    spec.default_choice = ui::ConfirmChoice::Cancel;
    if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return true;
    const auto result = AddDeveloperFolder(root, folder, taken_ids);
    if (!result.error.empty()) {
        ShowMessage(s, Text(Id::StoreDevLoadTitle), LocalizedDeveloperError(result.error, result.detail));
        return true;
    }
    RestartPulse(s);
    return true;
}
}

bool DeveloperModeSetting(const fs::path& store_root) {
    auto& state = State(store_root);
    if (!state.setting_loaded) { state.setting = devmode::Enabled(store_root); state.setting_loaded = true; }
    return state.setting;
}
bool SaveDeveloperModeSetting(const fs::path& store_root, bool enabled, std::string& error) {
    if (store_root.empty()) { error = "dev_no_store"; return false; }
    auto value = json::Value::Object();
    value.set("formatVersion", 1);
    value.set("enabled", enabled);
    if (!WriteAtomic(devmode::DeveloperRoot(store_root) / L"developer-mode.json", json::Serialize(value) + "\n", error)) return false;
    auto& state = State(store_root);
    state.setting = enabled;
    state.setting_loaded = true;
    return true;
}

DeveloperFolderResult AddDeveloperFolder(const fs::path& store_root, const fs::path& folder,
    const std::vector<std::string>& taken_ids) {
    DeveloperFolderResult result;
    try {
        const auto source = folder.lexically_normal();
        if (store_root.empty() || !LocalFolder(source)) { result.error = "dev_not_local"; return result; }
        std::string manifest_bytes;
        if (!devmode::ReadSmallFile(source / L"manifest.json", manifest_bytes)) { result.error = "dev_manifest_missing"; return result; }
        auto parsed = extensions::ParseManifest(manifest_bytes);
        if (!parsed) { result.error = "dev_manifest_invalid"; result.detail = json::Wide(parsed.error); return result; }
        const auto manifest = std::move(*parsed.manifest);
        result.id = manifest.id;
        if (!manifest.native) { result.error = "dev_no_native"; return result; }
        if (std::find(taken_ids.begin(), taken_ids.end(), manifest.id) != taken_ids.end()) { result.error = "dev_id_conflict"; return result; }
        auto entries = devmode::ReadRegistry(store_root);
        const bool replacing = std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return e.id == manifest.id; });
        if (!replacing && entries.size() >= devmode::kMaxDevPackages) { result.error = "dev_full"; return result; }

        // The files the manifest declares, read once: hashed and copied from the same bytes.
        const auto& native = *manifest.native;
        std::vector<std::pair<std::string, std::string>> files;  // name, bytes
        for (size_t i = 0; i <= native.files.size(); ++i) {
            const auto& name = i == 0 ? native.library : native.files[i - 1].path;
            std::string bytes;
            if (!ReadPayload(source / json::Wide(name), bytes)) { result.error = "dev_file_missing"; result.detail = json::Wide(name); return result; }
            files.emplace_back(name, std::move(bytes));
        }
        json::Value root;
        if (!json::Parse(manifest_bytes, root, nullptr, {extensions::kMaxManifestBytes, 16}) || !root.find("native")) {
            result.error = "dev_manifest_invalid"; return result;
        }
        json::Value native_value = *root.find("native");
        native_value.set("sha256", extensions::PackageRevision(files[0].second));
        if (const auto* list = native_value.find("files"); list && list->is_array()) {
            auto patched = json::Value::Array();
            for (size_t i = 0; i < list->size() && i + 1 < files.size(); ++i) {
                json::Value item = list->at(i);
                item.set("sha256", extensions::PackageRevision(files[i + 1].second));
                patched.push(std::move(item));
            }
            native_value.set("files", std::move(patched));
        }
        root.set("native", std::move(native_value));
        const auto shadow_bytes = json::Serialize(root);
        const auto reparsed = extensions::ParseManifest(shadow_bytes);
        if (!reparsed || reparsed.manifest->id != manifest.id) {
            result.error = "dev_manifest_invalid"; result.detail = reparsed ? std::wstring{} : json::Wide(reparsed.error); return result;
        }
        const auto revision = extensions::PackageRevision(shadow_bytes);
        if (!devmode::HexRevision(revision)) { result.error = "dev_copy_failed"; return result; }

        const auto package_dir = devmode::DeveloperRoot(store_root) / json::Wide(manifest.id);
        const auto target = package_dir / json::Wide(revision);
        std::error_code ec;
        if (!fs::exists(target / L"manifest.json", ec)) {
            const auto staging = package_dir / (L".staging-" + std::to_wstring(GetTickCount64()));
            fs::create_directories(staging, ec);
            if (ec) { result.error = "dev_copy_failed"; result.detail = json::Wide(ec.message()); return result; }
            std::string error;
            bool ok = WriteAtomic(staging / L"manifest.json", shadow_bytes, error);
            for (size_t i = 0; ok && i < files.size(); ++i) ok = WriteAtomic(staging / json::Wide(files[i].first), files[i].second, error);
            // Symbols for debugging, never verified or loaded by the host.
            const auto symbols = (source / json::Wide(native.library)).replace_extension(L".pdb");
            if (ok && fs::is_regular_file(symbols, ec)) fs::copy_file(symbols, staging / symbols.filename(), ec);
            if (ok) {
                fs::remove_all(target, ec);
                fs::rename(staging, target, ec);
                ok = !ec;
                if (!ok) error = ec.message();
            }
            if (!ok) {
                fs::remove_all(staging, ec);
                result.error = "dev_copy_failed"; result.detail = json::Wide(error); return result;
            }
        }
        std::string error;
        const devmode::DevPackageEntry entry{manifest.id, source.wstring(), revision};
        if (replacing) {
            for (auto& existing : entries) if (existing.id == manifest.id) existing = entry;
        } else entries.push_back(entry);
        if (!WriteRegistry(store_root, entries, error)) { result.error = error; return result; }
        // Older copies of this id: best effort (the running one stays locked until restart).
        for (const auto& child : fs::directory_iterator(package_dir, ec))
            if (child.path().filename() != target.filename()) fs::remove_all(child.path(), ec);
        auto& state = State(store_root);
        state.changed.insert(manifest.id);
        if (state.error_id == manifest.id) { state.error_id.clear(); state.error.clear(); state.error_detail.clear(); }
    } catch (const std::exception& error) {
        result.error = "dev_copy_failed";
        result.detail = json::Wide(error.what());
    }
    return result;
}

bool RemoveDeveloperFolder(const fs::path& store_root, const std::string& id, std::string& error) {
    try {
        auto entries = devmode::ReadRegistry(store_root);
        std::erase_if(entries, [&](const auto& entry) { return entry.id == id; });
        if (!WriteRegistry(store_root, entries, error)) return false;
        std::error_code ec;
        fs::remove_all(devmode::DeveloperRoot(store_root) / json::Wide(id), ec);  // best effort while loaded
        State(store_root).changed.insert(id);
        return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

std::wstring LocalizedDeveloperError(const std::string& error, const std::wstring& detail) {
    if (error == "dev_id_conflict") return Text(Id::StoreDevErrConflict);
    if (error == "dev_no_native") return Text(Id::StoreDevErrNoNative);
    if (error == "dev_manifest_missing") return Text(Id::StoreDevErrNoManifest);
    if (error == "dev_file_missing") return Replace(Text(Id::StoreDevErrFile), L"{name}", detail);
    if (error == "dev_shadow_missing" || error == "dev_shadow_changed" || error == "dev_id_mismatch") return Text(Id::StoreDevErrShadow);
    if (error == "dev_not_local") return Text(Id::StoreDevErrLocal);
    if (error == "dev_manifest_invalid") return Replace(Text(Id::StoreDevErrInvalid), L"{error}", detail);
    if (error == "dev_full" || error == "store_full") return Text(Id::StoreDevErrFull);
    if (error.starts_with("dev_copy_failed") || error.starts_with("dev_write_failed"))
        return Replace(Text(Id::StoreDevErrCopy), L"{error}", detail.empty() ? json::Wide(error) : detail);
    return json::Wide(error) + (detail.empty() ? std::wstring{} : L" (" + detail + L")");
}

void FillDeveloperRows(AppState&, const fs::path& store_root, ui::ExtensionStoreView& view) {
    if (store_root.empty()) return;
    auto& state = State(store_root);
    view.developer_mode = DeveloperModeSetting(store_root);
    if (view.developer_mode != extensions::DeveloperModeAtStartup() || !state.changed.empty()) view.pending_restart = true;
    const auto& entries = Entries(store_root);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        ui::InstalledExtensionItem item;
        item.id = json::Wide(entry.id);
        item.name = item.id;
        bool loaded = false;
        for (size_t p = 0; p < extensions::PackageCount() && p < 10000; ++p) {
            const auto* manifest = extensions::PackageManifest(p);
            if (!manifest || manifest->id != entry.id || !extensions::PackageFromDeveloperFolder(p)) continue;
            loaded = true;
            if (!manifest->name.empty()) item.name = json::Wide(manifest->name);
            break;
        }
        item.enabled = loaded;
        item.native_code = true;
        item.revision = entry.revision;
        item.version = Text(Id::StoreLocalBuild) + L" · " + json::Wide(entry.revision.substr(0, 7));
        item.publisher = entry.source;
        const bool changed = state.changed.count(entry.id) != 0;
        item.status = loaded ? Text(Id::StoreDevLoaded) : Text(Id::StoreDevPendingRestart);
        if (loaded && changed) item.status += L" · " + Text(Id::StoreDevPendingRestart);
        if (!view.developer_mode) item.notice = Text(Id::StoreDevModeOff);
        else if (state.error_id == entry.id) item.notice = LocalizedDeveloperError(state.error, state.error_detail);
        else if (!changed) item.notice = StartupError(entry.id);
        item.can_uninstall = !view.installed_busy && !view.installed_loading;
        item.development_index = static_cast<int>(i);
        view.installed.push_back(std::move(item));
    }
}

bool DeveloperButtonAction(AppState& s, const fs::path& store_root, const std::vector<std::string>& taken_ids) {
    if (store_root.empty()) return true;
    const bool enabled = DeveloperModeSetting(store_root);
    ui::FluentMenuItem toggle, load, open;
    toggle.command = 1; toggle.text = Text(Id::StoreDeveloperMode); toggle.toggle = true; toggle.checked = enabled;
    toggle.separator_after = true;
    load.command = 2; load.text = Text(Id::StoreDeveloperLoadFolder); load.enabled = enabled; load.glyph = L"\xE8DA";  // OpenLocal
    open.command = 3; open.text = Text(Id::StoreDeveloperOpenRoot); open.glyph = L"\xE838";  // FolderOpen
    const int choice = Menu(s, {toggle, load, open});
    if (choice == 1) {
        if (!enabled) {
            ui::ConfirmDialogSpec spec;
            spec.title = Text(Id::StoreDeveloperEnableTitle);
            spec.message = Text(Id::StoreDeveloperEnablePrompt);
            spec.confirm_text = Text(Id::StoreDeveloperEnableConfirm);
            spec.tone = ui::ConfirmTone::Warning;
            spec.default_choice = ui::ConfirmChoice::Cancel;
            if (ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor) != ui::ConfirmChoice::Confirm) return true;
        }
        std::string error;
        if (!SaveDeveloperModeSetting(store_root, !enabled, error))
            ShowMessage(s, Text(Id::StoreDeveloperMode), LocalizedDeveloperError(error));
    } else if (choice == 2) {
        return LoadFolder(s, store_root, taken_ids);
    } else if (choice == 3) {
        const auto root = devmode::DeveloperRoot(store_root);
        std::error_code ec;
        fs::create_directories(root, ec);
        NavigateTo(s, root.wstring());
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool DeveloperRowMenu(AppState& s, const fs::path& store_root, int development_index,
    const std::vector<std::string>& taken_ids) {
    const auto entries = Entries(store_root);
    if (development_index < 0 || static_cast<size_t>(development_index) >= entries.size()) return true;
    const auto entry = entries[static_cast<size_t>(development_index)];
    auto& state = State(store_root);
    std::wstring last_error = state.error_id == entry.id ? LocalizedDeveloperError(state.error, state.error_detail) : StartupError(entry.id);
    ui::FluentMenuItem reload, open, copy, remove;
    reload.command = 1; reload.text = Text(Id::StoreDevReload); reload.glyph = L"\xE72C";  // Refresh
    reload.enabled = DeveloperModeSetting(store_root);
    open.command = 2; open.text = Text(Id::StoreDevOpenFolder); open.glyph = L"\xE838";
    copy.command = 3; copy.text = Text(Id::StoreDevCopyError); copy.glyph = L"\xE8C8";  // Copy
    copy.enabled = !last_error.empty(); copy.separator_after = true;
    remove.command = 4; remove.text = Text(Id::StoreDevRemove); remove.vector_icon = ui::StoreIcon::Trash;
    const int choice = Menu(s, {reload, open, copy, remove});
    if (choice == 1) {
        const auto result = AddDeveloperFolder(store_root, entry.source, taken_ids);
        if (!result.error.empty()) {
            state.error_id = entry.id; state.error = result.error; state.error_detail = result.detail;
            ShowMessage(s, Text(Id::StoreDevReload), LocalizedDeveloperError(result.error, result.detail));
        } else {
            RestartPulse(s);
            return true;
        }
    } else if (choice == 2) {
        NavigateTo(s, entry.source);
    } else if (choice == 3) {
        CopyTextToClipboard(s.hwnd, json::Wide(entry.id) + L": " + last_error);
    } else if (choice == 4) {
        std::string error;
        if (!RemoveDeveloperFolder(store_root, entry.id, error))
            ShowMessage(s, Text(Id::StoreDevRemove), LocalizedDeveloperError(error));
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}
}
