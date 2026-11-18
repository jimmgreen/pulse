#include <pulse/client.hpp>
#include <pulse/context.h>
#include <pulse/directories.h>
#include <pulse/resources.h>
#include <pulse/ui_data.h>
#include <pulse/ui_events.h>
#include <pulse/ui_mount.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using pulse::sdk::View;
constexpr uint32_t kExhausted = (std::numeric_limits<uint32_t>::max)();
constexpr size_t kEntryLimit = 4096;
constexpr size_t kCardCount = 8;
constexpr size_t kCardsPerType = 2;
const char* const kKinds[] = {"Folders", "Images", "Documents", "Other files"};
const char* const kCommands[] = {
    "open-directory", "refresh", "up", "use-context", "restore-path", "cancel-load", "next", "open-selected",
    "view-cards", "view-table", "mount-window", "mount-sidebar", "mount-workspace", "save-settings", "reload-settings",
    "card-0", "card-1", "card-2", "card-3", "card-4", "card-5", "card-6", "card-7", "advanced-toggle"
};
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
std::string Clip(std::string value, size_t limit = 1000) {
    if (value.size() <= limit) return value;
    while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
    value.resize(limit); return value;
}
std::string Fold(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
uint32_t Kind(std::string_view name, uint32_t flags) {
    if (flags & PULSE_DIRECTORY_ENTRY_FOLDER) return 0;
    const auto dot = name.find_last_of('.');
    const auto extension = dot == std::string_view::npos ? std::string() : Fold(std::string(name.substr(dot)));
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".gif" || extension == ".webp" || extension == ".bmp" || extension == ".svg" || extension == ".heic") return 1;
    if (extension == ".txt" || extension == ".md" || extension == ".pdf" || extension == ".doc" || extension == ".docx" || extension == ".csv" || extension == ".xlsx" || extension == ".pptx" || extension == ".json") return 2;
    return 3;
}
std::string ParentPath(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    while (path.size() > 3 && path.back() == '\\') path.pop_back();
    size_t root = 0;
    if (path.size() >= 3 && path[1] == ':' && path[2] == '\\') root = 3;
    else if (path.starts_with("\\\\")) {
        const auto server = path.find('\\', 2);
        if (server == std::string::npos) return path;
        const auto share = path.find('\\', server + 1);
        if (share == std::string::npos) return path;
        root = share;
    } else return {};
    const auto slash = path.find_last_of('\\');
    if (slash == std::string::npos || slash < root) return path.substr(0, root);
    return path.substr(0, slash == 2 ? 3 : slash);
}

struct Entry {
    uint64_t entry = 0, bytes = 0;
    uint32_t kind = 0, flags = 0;
    std::string name, folded, id;
};

struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 model{};
    PulseUiDataV1 data{};
    PulseUiEventsV1 events{};
    PulseUiMountV1 mounting{};
    PulseDirectoriesV1 directories{};
    PulseContextV1 context{};
    PulseResourcesV1 resources{};
    uint64_t panel = 0, provider = 0, ui_request = 0, mount_subscription = 0;
    std::array<pulse::sdk::Subscription, std::size(kCommands)> subscriptions;
    pulse::sdk::Subscription state_subscription;
    PulseDirectoryId directory = 0;
    PulseDirectoryWatchId watch = 0;
    PulseDirectoryRequestId opening = 0, operation = 0, opening_entry = 0;
    PulseContextSubscriptionId context_subscription = 0;
    PulseResourcesRequestId settings_request = 0;
    uint64_t snapshot = 0, total = 0, epoch = 0, settings_version = 0;
    bool active = false, loading = false, dirty = false, settings_loaded = false, saving = false, save_again = false;
    bool remember_directory = false;
    size_t save_index = 0;
    std::string context_path, loaded_path, saved_path, view = "cards", sort = "nameAsc", filter, failure, watch_warning, route_summary;
    std::array<std::string, 3> save_values;
    std::array<std::string, 3> persisted_values;
    std::vector<Entry> entries;
    std::vector<size_t> visible;
    std::array<std::string, kCardCount> card_ids{};

    explicit Plugin(const PulseHostV1& h) : host(h) {}
    void Set(std::string_view key, std::string_view value) noexcept {
        if (!active || !panel) return;
        const PulseUiState state{View(key), View(value)};
        model.set_states(model.context, panel, &state, 1);
    }
    void Route(std::string value) {
        route_summary = std::move(value);
        Set("route", Clip(route_summary, 450) + " | Pulse context: " + Clip(context_path, 500));
    }
    std::string Read(std::string_view key) {
        uint32_t needed = 0;
        if (model.get_state(model.context, panel, View(key), nullptr, 0, &needed) != PULSE_OK || needed > 1024) return {};
        std::array<char, 1025> buffer{};
        if (model.get_state(model.context, panel, View(key), buffer.data(), static_cast<uint32_t>(buffer.size()), &needed) != PULSE_OK) return {};
        return std::string(buffer.data(), needed);
    }
    void PublishStatus(uint32_t status, std::string error = {}) {
        if (!ui_request) { Set("status", status == PULSE_UI_DATA_LOADING ? "loading" : status == PULSE_UI_DATA_ERROR ? "error" : "ready"); Set("error", error); return; }
        error = Clip(std::move(error));
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = status;
        page.error = View(error); page.progress = status == PULSE_UI_DATA_LOADING ? 10u : 0u;
        data.publish(data.context, ui_request, &page);
    }
    void Fail(std::string text) {
        loading = false; failure = Clip(std::move(text));
        PublishStatus(PULSE_UI_DATA_ERROR, failure); Route(failure);
    }
    void CancelOperation() noexcept {
        const auto old = operation; operation = 0;
        if (old) directories.cancel(directories.context, old);
    }
    void ReleaseDirectory() noexcept {
        CancelOperation();
        if (opening) { const auto old = opening; opening = 0; directories.cancel(directories.context, old); }
        if (opening_entry) { const auto old = opening_entry; opening_entry = 0; directories.cancel(directories.context, old); }
        if (watch) { directories.unwatch(directories.context, watch); watch = 0; }
        if (directory) { directories.close(directories.context, directory); directory = 0; }
        snapshot = total = 0; loading = dirty = false; entries.clear(); visible.clear(); card_ids.fill({}); watch_warning.clear(); ++epoch;
    }
    PulseStatus Bind() {
        const PulseUiDataBinding binding{sizeof(binding), 256, View("filter"), View("sort"), View("status"), View("error"), View("")};
        return data.bind(data.context, panel, View("items"), provider, &binding);
    }
    void Arrange() {
        filter = Fold(Read("filter"));
        sort = Read("sort"); if (sort != "nameDesc") sort = "nameAsc";
        visible.clear();
        for (size_t i = 0; i < entries.size(); ++i) if (filter.empty() || entries[i].folded.find(filter) != std::string::npos) visible.push_back(i);
        std::sort(visible.begin(), visible.end(), [&](size_t left, size_t right) {
            const auto& a = entries[left]; const auto& b = entries[right];
            if (a.folded == b.folded) return a.entry < b.entry;
            return sort == "nameDesc" ? a.folded > b.folded : a.folded < b.folded;
        });
        std::array<size_t, 4> used{};
        card_ids.fill({});
        for (const auto index : visible) {
            const auto& item = entries[index]; auto& count = used[item.kind];
            if (count >= kCardsPerType) continue;
            const auto slot = item.kind * kCardsPerType + count++;
            card_ids[slot] = item.id;
            Set("card" + std::to_string(slot), Clip(item.name, 160) + "\n" + (item.kind == 0 ? "Folder · normal navigation" : std::to_string(item.bytes) + " bytes · " + kKinds[item.kind]));
        }
        size_t cards = 0;
        for (size_t i = 0; i < kCardCount; ++i) { Set("show" + std::to_string(i), card_ids[i].empty() ? "false" : "true"); if (!card_ids[i].empty()) ++cards; }
        Set("count", std::to_string(visible.size()) + " entries · " + std::to_string(cards) + " cards" +
            (total > kEntryLimit ? " · first 4096 of " + std::to_string(total) : ""));
    }
    void PublishRows(uint32_t offset) {
        if (!ui_request) return;
        if (loading) { PublishStatus(PULSE_UI_DATA_LOADING); return; }
        if (!failure.empty()) { PublishStatus(PULSE_UI_DATA_ERROR, failure); return; }
        std::vector<std::array<std::string, 3>> values;
        std::vector<pulse::sdk::ModelRow> owned_rows;
        std::vector<PulseUiModelRow> rows;
        values.reserve(256); owned_rows.reserve(256); rows.reserve(256);
        size_t bytes = 0;
        for (size_t i = offset; i < visible.size() && values.size() < 256; ++i) {
            const auto& entry = entries[visible[i]];
            std::array<std::string, 3> value{entry.name, entry.kind == 0 ? "—" : std::to_string(entry.bytes), entry.kind == 3 ? "Other" : kKinds[entry.kind]};
            const auto next_bytes = entry.id.size() + value[0].size() * 2 + value[1].size() + value[2].size();
            if (bytes + next_bytes > 58000) break;
            bytes += next_bytes; values.push_back(std::move(value));
        }
        if (offset < visible.size() && values.empty()) { Fail("A filename exceeds the data page byte budget."); return; }
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& entry = entries[visible[static_cast<size_t>(offset) + i]];
            owned_rows.push_back(pulse::sdk::ModelRow(entry.id, values[i][0]).Cells({values[i][0], values[i][1], values[i][2]}));
        }
        for (const auto& row : owned_rows) rows.push_back(row.Build());
        const auto revision = std::to_string(epoch) + "-" + std::to_string(snapshot);
        const auto next = static_cast<size_t>(offset) + rows.size();
        const auto cursor = next < visible.size() ? revision + "@" + std::to_string(next) : "";
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = PULSE_UI_DATA_READY; page.progress = 100;
        page.revision = View(revision); page.next_cursor = View(cursor); page.rows = rows.data(); page.row_count = static_cast<uint32_t>(rows.size());
        if (data.publish(data.context, ui_request, &page) != PULSE_OK) Route("The table rejected a stale data request; Refresh to retry.");
    }
    void RefreshTable() {
        if (data.request(data.context, panel, View("items"), PULSE_UI_DATA_REFRESH) != PULSE_OK) Route("The data controller rejected refresh.");
    }
    void ArmWatch() {
        if (!directory || watch) return;
        if (directories.watch(directories.context, directory, Changed, this, &watch) != PULSE_OK) { watch = 0; watch_warning = "Watch unavailable. Manual Refresh still works."; Route(watch_warning); }
        else watch_warning.clear();
    }
    void RefreshDirectory() {
        if (!directory) { OpenPath(Read("directory")); return; }
        if (loading || operation || opening) { dirty = true; return; }
        if (opening_entry) { const auto old = opening_entry; opening_entry = 0; directories.cancel(directories.context, old); }
        dirty = false; failure.clear(); loading = true; Set("selected", "");
        PublishStatus(PULSE_UI_DATA_LOADING); ArmWatch();
        if (directories.refresh(directories.context, directory, Completed, this, &operation) != PULSE_OK) { operation = 0; Fail("Directory refresh was rejected. Retry Refresh."); }
    }
    void OpenPath(std::string path, bool remember = true) {
        if (path.empty() || path.size() > 1024) { Route("Enter an absolute drive or UNC directory path (up to 1024 UTF-8 bytes)."); return; }
        ReleaseDirectory(); failure.clear(); loading = true; remember_directory = remember;
        Set("directory", path); Set("path", Clip("Opening: " + path)); Set("selected", ""); Arrange();
        RefreshTable(); Route("Opening a host-owned immutable directory snapshot…");
        if (directories.open(directories.context, View(path), Completed, this, &opening) != PULSE_OK) { opening = 0; Fail("Directory open was rejected. Check the absolute path."); }
    }
    void Load(uint32_t offset) {
        if (directories.list(directories.context, directory, snapshot, offset, 256, Completed, this, &operation) != PULSE_OK) { operation = 0; Fail("Snapshot page request was rejected."); }
    }
    void Complete(const PulseDirectoryReply& reply) {
        if (!active) return;
        if (reply.request_id == opening_entry) {
            opening_entry = 0;
            Route(reply.status == PULSE_OK ? "Open route accepted. Folders navigate through Pulse; for files, this does not report application launch or execution completion." : "Open route failed: " + Copy(reply.error)); return;
        }
        if (reply.request_id == opening) {
            opening = 0;
            if (reply.status != PULSE_OK) { Set("path", "No directory open"); Fail("Open failed: " + Copy(reply.error)); return; }
            directory = reply.directory_id; snapshot = reply.snapshot_id; total = reply.total;
            loaded_path = Copy(reply.path); Set("directory", loaded_path); Set("path", Clip(loaded_path));
            ArmWatch(); Load(0); return;
        }
        if (reply.request_id != operation || reply.directory_id != directory) return;
        operation = 0;
        if (reply.status != PULSE_OK) { Fail("Directory request failed: " + Copy(reply.error)); return; }
        if (reply.operation == PULSE_DIRECTORY_REFRESH) {
            snapshot = reply.snapshot_id; total = reply.total; entries.clear(); ++epoch; Set("selected", ""); Load(0); return;
        }
        if (reply.operation != PULSE_DIRECTORY_LIST || reply.snapshot_id != snapshot) return;
        for (uint32_t i = 0; i < reply.count && entries.size() < kEntryLimit; ++i) {
            const auto& row = reply.rows[i]; Entry entry;
            entry.entry = row.entry_id; entry.bytes = row.size_bytes; entry.flags = row.flags;
            entry.name = Copy(row.name); entry.folded = Fold(entry.name); entry.kind = Kind(entry.name, entry.flags);
            entry.id = "g" + std::to_string(epoch) + "e" + std::to_string(row.entry_id); entries.push_back(std::move(entry));
        }
        if (reply.next_offset != kExhausted && entries.size() < kEntryLimit) { Load(reply.next_offset); return; }
        loading = false; failure.clear(); Arrange(); RefreshTable();
        const std::string message = entries.empty() ? "This directory is empty. Open a path, go Up, or follow Pulse context." : "Snapshot ready. Cards group by type; table keeps the same filtered and sorted selection set.";
        Route(watch_warning.empty() ? message : message + " " + watch_warning);
        SaveSettings();
        if (dirty) { dirty = false; RefreshDirectory(); }
    }
    void OpenEntry(const std::string& id) {
        if (loading) { Route("Wait for the current snapshot before opening an item."); return; }
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.id == id; });
        if (found == entries.end() || !directory || !snapshot) { Route("Select a current table row, or use a visible card's Open button."); return; }
        Set("selected", id);
        if (opening_entry) { const auto old = opening_entry; opening_entry = 0; directories.cancel(directories.context, old); }
        if (directories.open_entry(directories.context, directory, snapshot, found->entry, Completed, this, &opening_entry) != PULSE_OK) { opening_entry = 0; Route("Open route rejected a stale or unavailable entry. Refresh and select again."); }
        else Route(found->kind == 0 ? "Folder navigation requested through Pulse's normal open route…" : "File open dispatch requested. Acceptance does not confirm application launch or execution completion…");
    }
    void SetView(std::string value) {
        view = value == "table" ? "table" : "cards";
        Set("view", view); Set("cards-visible", view == "cards" ? "true" : "false"); Set("table-visible", view == "table" ? "true" : "false");
    }
    void ShowMount(const PulseUiMountInfo& info) {
        const char* name = info.placement == PULSE_UI_MOUNT_SIDEBAR ? "sidebar" : info.placement == PULSE_UI_MOUNT_WORKSPACE ? "workspace" : "window";
        Set("mount", std::string(name) + " · " + std::to_string(info.width_dip) + " × " + std::to_string(info.height_dip) + " DIP · " + std::to_string(info.dpi) + " DPI · " + (info.dark ? "dark" : "light"));
    }
    void Move(uint32_t place) {
        const auto code = mounting.set(mounting.context, panel, place);
        if (code != PULSE_OK) { Route("Mount request rejected (the host slot may be occupied). Window remains available."); return; }
        PulseUiMountInfo info{}; info.size = sizeof(info);
        if (mounting.get(mounting.context, panel, &info) == PULSE_OK) ShowMount(info);
    }
    void LoadSettings() {
        if (settings_request) { Route("A settings request is still pending."); return; }
        saving = false; settings_loaded = false; Set("settings", "loading");
        if (resources.settings_get(resources.context, SettingsCompleted, this, &settings_request) != PULSE_OK) { settings_request = 0; Set("settings", "get rejected; local choices retained"); }
    }
    void SaveNext() {
        static const char* const keys[] = {"workbench.view", "workbench.sort", "workbench.path"};
        if (resources.settings_set(resources.context, View(keys[save_index]), View(save_values[save_index]), 0, settings_version, SettingsCompleted, this, &settings_request) != PULSE_OK) {
            settings_request = 0; saving = false; Set("settings", "save rejected; local choices retained");
        }
    }
    void SaveSettings() {
        if (!settings_loaded) return;
        if (settings_request) { save_again = true; return; }
        const auto path = directory && !loading && !loaded_path.empty() && (remember_directory || saved_path.empty()) ? loaded_path : saved_path;
        save_values = {view, sort, path};
        if (save_values == persisted_values) return;
        save_index = 0; saving = true;
        Set("settings", "saving with expected version " + std::to_string(settings_version)); SaveNext();
    }
    void Settings(const PulseResourcesReply& reply) {
        if (!active || reply.request_id != settings_request) return;
        settings_request = 0;
        if (reply.status != PULSE_OK) {
            saving = save_again = false; settings_loaded = false;
            Set("settings", Copy(reply.error) == "version_conflict" ? "conflict: no conflicting write committed; Reload settings before saving again" : Clip("settings error: " + Copy(reply.error))); return;
        }
        settings_version = reply.settings.version; settings_loaded = true;
        if (!saving) {
            std::string restored_view = "cards", restored_sort = "nameAsc"; saved_path.clear();
            for (uint32_t i = 0; i < reply.settings.count; ++i) {
                const auto key = Copy(reply.settings.values[i].key), value = Copy(reply.settings.values[i].value);
                if (key == "workbench.view") restored_view = value;
                else if (key == "workbench.sort" && (value == "nameAsc" || value == "nameDesc")) restored_sort = value;
                else if (key == "workbench.path") saved_path = value;
            }
            SetView(restored_view); sort = restored_sort; Set("sort", sort); Set("sort-index", sort == "nameDesc" ? "1" : "0"); Arrange();
            persisted_values = {view, sort, saved_path};
            Set("settings", "loaded version " + std::to_string(settings_version) + (saved_path.empty() ? " · no saved path" : " · Restore saved path is available"));
            if (!loading && directory) SaveSettings();
            return;
        }
        if (++save_index < save_values.size()) { SaveNext(); return; }
        saved_path = save_values[2]; persisted_values = save_values; saving = false;
        Set("settings", "saved version " + std::to_string(settings_version) + " · view/sort/path");
        if (save_again) { save_again = false; SaveSettings(); }
    }
    static void PULSE_CALL Completed(void* user, const PulseDirectoryReply* reply) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        try { if (reply) self.Complete(*reply); } catch (...) { try { self.Fail("Directory callback failed inside the workbench DLL."); } catch (...) {} }
    }
    static void PULSE_CALL Changed(void* user, const PulseDirectoryChange* change) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!self.active || !change || change->watch_id != self.watch || change->directory_id != self.directory) return;
        try {
            if (change->kind == PULSE_DIRECTORY_WATCH_READY || change->kind == PULSE_DIRECTORY_INVALIDATED) self.RefreshDirectory();
            else if (change->kind == PULSE_DIRECTORY_WATCH_ERROR) {
                const auto old = self.watch; self.watch = 0; self.directories.unwatch(self.directories.context, old);
                self.watch_warning = "Watch failed; use manual Refresh: " + Copy(change->error); self.Route(self.watch_warning);
            }
        } catch (...) { self.Set("route", "Watch callback failed."); }
    }
    static void PULSE_CALL Requested(void* user, const PulseUiDataRequest* request) noexcept {
        auto& self = *static_cast<Plugin*>(user); if (!self.active || !request) return;
        try {
            self.ui_request = request->request; self.Arrange();
            uint32_t offset = 0; const auto cursor = Copy(request->cursor);
            if (!cursor.empty()) {
                const auto revision = std::to_string(self.epoch) + "-" + std::to_string(self.snapshot) + "@";
                if (!cursor.starts_with(revision)) { self.PublishStatus(PULSE_UI_DATA_ERROR, "Stale table cursor; Refresh starts the current snapshot."); return; }
                const auto result = std::from_chars(cursor.data() + revision.size(), cursor.data() + cursor.size(), offset);
                if (result.ec != std::errc{} || result.ptr != cursor.data() + cursor.size() || offset >= self.visible.size()) { self.PublishStatus(PULSE_UI_DATA_ERROR, "Invalid table cursor; Refresh starts the current snapshot."); return; }
            }
            self.PublishRows(offset);
        } catch (...) { try { self.Fail("Data request failed inside the workbench DLL."); } catch (...) {} }
    }
    static void PULSE_CALL Canceled(void* user, uint64_t request) noexcept {
        auto& self = *static_cast<Plugin*>(user); if (self.ui_request == request) self.ui_request = 0;
    }
    static void PULSE_CALL ContextChanged(void* user, const PulseContextSnapshot* snapshot_value) noexcept {
        auto& self = *static_cast<Plugin*>(user); if (!snapshot_value) return;
        try {
            const auto path = Copy(snapshot_value->directory);
            self.context_path = path.size() <= 1024 ? path : "";
            self.Route(self.route_summary);
        } catch (...) { self.Set("route", "Context callback failed."); }
    }
    static void PULSE_CALL SettingsCompleted(void* user, const PulseResourcesReply* reply) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        try { if (reply) self.Settings(*reply); } catch (...) { self.Set("settings", "Settings callback failed; Reload settings before saving."); self.settings_loaded = false; }
    }
    static void PULSE_CALL StateChanged(void* user, uint64_t panel_id, const PulseUiStateBatch* batch) noexcept {
        auto& self = *static_cast<Plugin*>(user); if (!self.active || self.panel != panel_id || !batch) return;
        try {
            bool arrange = false, save = false;
            for (uint32_t i = 0; i < batch->count; ++i) {
                const auto key = Copy(batch->changes[i].key);
                if (key == "filter") arrange = true;
                else if (key == "sort-index") { self.Set("sort", self.Read("sort-index") == "1" ? "nameDesc" : "nameAsc"); arrange = true; save = true; }
            }
            if (arrange) self.Arrange();
            if (save) self.SaveSettings();
        } catch (...) { self.Set("route", "Control state callback failed."); }
    }
    static void PULSE_CALL Mounted(void* user, uint64_t panel_id, const PulseUiMountInfo* info) noexcept {
        auto& self = *static_cast<Plugin*>(user); if (!self.active || self.panel != panel_id || !info) return;
        try { self.ShowMount(*info); } catch (...) { self.Set("mount", "Mount event failed."); }
    }
    static void PULSE_CALL Click(void* user, uint64_t panel_id, PulseStringView command) noexcept {
        auto& self = *static_cast<Plugin*>(user); if (!self.active || self.panel != panel_id) return;
        try {
            const auto id = Copy(command);
            if (id == "open-directory") self.OpenPath(self.Read("directory"));
            else if (id == "advanced-toggle") self.Set("advanced-open", self.Read("advanced-open") == "true" ? "false" : "true");
            else if (id == "refresh") self.RefreshDirectory();
            else if (id == "up") { const auto path = ParentPath(self.loaded_path); if (!path.empty()) self.OpenPath(path); }
            else if (id == "use-context") self.OpenPath(self.context_path);
            else if (id == "restore-path") self.OpenPath(self.saved_path);
            else if (id == "open-selected") self.OpenEntry(self.Read("selected"));
            else if (id == "next") { if (self.data.request(self.data.context, self.panel, View("items"), PULSE_UI_DATA_NEXT_PAGE) != PULSE_OK) self.Route("No next page. The table has all matching loaded rows."); }
            else if (id == "cancel-load") {
                self.ReleaseDirectory(); self.failure.clear(); self.Set("selected", ""); self.Arrange();
                self.data.request(self.data.context, self.panel, View("items"), PULSE_UI_DATA_CANCEL);
                self.data.unbind(self.data.context, self.panel, View("items"));
                if (self.Bind() != PULSE_OK) { self.Route("Closing succeeded, but rebinding the table failed."); return; }
                self.Set("status", "cancelled"); self.Set("error", ""); self.Set("path", "Directory closed; pending requests and watch cancelled"); self.Route("Cancelled. Open or Refresh starts the current path again.");
            } else if (id == "view-cards" || id == "view-table") { self.SetView(id == "view-table" ? "table" : "cards"); self.SaveSettings(); }
            else if (id == "mount-window") self.Move(PULSE_UI_MOUNT_WINDOW);
            else if (id == "mount-sidebar") self.Move(PULSE_UI_MOUNT_SIDEBAR);
            else if (id == "mount-workspace") self.Move(PULSE_UI_MOUNT_WORKSPACE);
            else if (id == "save-settings") { if (self.settings_loaded) self.SaveSettings(); else self.Set("settings", "Reload settings first; no unknown version will be overwritten"); }
            else if (id == "reload-settings") self.LoadSettings();
            else for (size_t i = 0; i < kCardCount; ++i) if (id == "card-" + std::to_string(i)) { self.OpenEntry(self.card_ids[i]); break; }
        } catch (...) { self.Set("route", "Workbench action failed inside the DLL."); }
    }

    PulseStatus Render() {
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelColumn;
        using N = PulseUiModelNode;
        const auto button = [](std::string id, std::string text) {
            return ModelNode(PULSE_UI_MODEL_BUTTON, id).String(&N::text, std::move(text)).String(&N::command, std::move(id));
        };
        const auto label = [](std::string id, std::string key) {
            return ModelNode(PULSE_UI_MODEL_TEXT, std::move(id)).String(&N::binding, "state." + key);
        };
        const auto grid = [](std::string id, std::vector<ModelNode> children, int64_t columns) {
            return ModelNode(PULSE_UI_MODEL_GRID, std::move(id)).Int(&N::columns, columns)
                .Int(&N::gap, 6).Children(std::move(children));
        };
        const auto compact = [](std::string id, std::vector<ModelNode> children, int64_t minimum) {
            for (auto& child : children) child.Int(&N::min_width, minimum).Int(&N::flex, 1);
            if (id == "mount-controls") children.back().Int(&N::min_width, 88);
            return ModelNode(PULSE_UI_MODEL_STACK, std::move(id)).String(&N::orientation, "horizontal")
                .Int(&N::gap, 6).Children(std::move(children));
        };
        auto path = ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "path-input").String(&N::text, "Directory path")
            .String(&N::state, "directory").String(&N::placeholder, "Absolute drive or UNC directory path");
        auto filter_node = ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "filter-input").String(&N::text, "Filter loaded names")
            .String(&N::state, "filter").String(&N::placeholder, "Filter names");
        auto sort_node = ModelNode(PULSE_UI_MODEL_SELECT, "sort-input").String(&N::text, "Name order")
            .String(&N::state, "sort-index").Options({"Name A to Z", "Name Z to A"});
        std::vector<ModelNode> groups;
        for (size_t kind = 0; kind < std::size(kKinds); ++kind) {
            std::vector<ModelNode> cards;
            for (size_t position = 0; position < kCardsPerType; ++position) {
                const auto slot = kind * kCardsPerType + position;
                const auto suffix = std::to_string(slot);
                auto text = label("card-text-" + suffix, "card" + suffix).Int(&N::min_height, 48);
                cards.push_back(ModelNode(PULSE_UI_MODEL_STACK, "card-tile-" + suffix).Int(&N::gap, 4)
                    .String(&N::visible_binding, "state.show" + suffix)
                    .Children({std::move(text), button(kCommands[15 + slot], "Open")}));
            }
            const auto suffix = std::to_string(kind);
            auto heading = ModelNode(PULSE_UI_MODEL_TEXT, "kind-title-" + suffix)
                .String(&N::text, kKinds[kind]).String(&N::role, "section");
            groups.push_back(ModelNode(PULSE_UI_MODEL_STACK, "kind-group-" + suffix).Int(&N::gap, 6)
                .Children({std::move(heading), grid("kind-" + suffix, std::move(cards), 2)}));
        }
        auto cards_node = grid("cards", std::move(groups), 2).Int(&N::gap, 12)
            .String(&N::visible_binding, "state.cards-visible");
        auto table = ModelNode(PULSE_UI_MODEL_DATA_TABLE, "items").String(&N::state, "selected").Rows({})
            .DataColumns({ModelColumn("name", "Name", 112), ModelColumn("bytes", "Bytes", 56), ModelColumn("type", "Type", 72)})
            .Int(&N::min_height, 180).String(&N::visible_binding, "state.table-visible");
        auto table_group = ModelNode(PULSE_UI_MODEL_STACK, "table-view").Int(&N::gap, 6)
            .String(&N::visible_binding, "state.table-visible").Children({std::move(table),
                grid("table-actions", {button("open-selected", "Open selected"), button("next", "Next 256")}, 2)});
        auto advanced = ModelNode(PULSE_UI_MODEL_GROUP, "advanced-content").String(&N::visible_binding, "state.advanced-open")
            .Int(&N::gap, 8).Children({std::move(path),
                grid("navigation", {button("open-directory", "Open path"), button("refresh", "Refresh"),
                    button("up", "Up"), button("use-context", "Pulse context")}, 2),
                label("route", "route"), label("path", "path"), label("mount", "mount"), ModelNode(PULSE_UI_MODEL_TEXT, "context").String(&N::text, "Pulse context follows the main window; current path is shown above."),
                label("settings", "settings"), grid("settings-controls", {button("restore-path", "Restore saved path"),
                    button("save-settings", "Save choices"), button("reload-settings", "Reload settings"),
                    button("cancel-load", "Close / cancel")}, 2), label("view", "view")});
        auto stack = ModelNode(PULSE_UI_MODEL_STACK, "content").Int(&N::gap, 8).Children({
            compact("mount-controls", {button("mount-window", "Window"), button("mount-sidebar", "Sidebar"),
                button("mount-workspace", "Workspace")}, 72),
            compact("view-controls", {button("view-cards", "Cards"), button("view-table", "Table")}, 100),
            compact("filtering", {std::move(filter_node), std::move(sort_node)}, 112), label("count", "count"),
            std::move(cards_node), std::move(table_group), label("status", "status"), label("error", "error"),
            ModelNode(PULSE_UI_MODEL_BUTTON, "advanced").String(&N::text, "Directory and settings").String(&N::command, "advanced-toggle"), std::move(advanced)});
        pulse::sdk::ModelTree tree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "root").Children({std::move(stack)}));
        std::vector<std::pair<std::string, std::string>> initial = {
            {"directory", context_path}, {"filter", ""}, {"sort", "nameAsc"}, {"selected", ""}, {"status", "loading"}, {"error", ""}, {"sort-index", "0"},
            {"count", "Loading current Pulse directory…"}, {"view", "cards"}, {"mount", "window"}, {"settings", "loading"}, {"route", "Host workers enumerate and watch; this DLL groups and sorts."},
            {"path", "No directory open"}, {"cards-visible", "true"}, {"table-visible", "false"}, {"advanced-open", "false"}};
        for (size_t i = 0; i < kCardCount; ++i) { initial.emplace_back("card" + std::to_string(i), ""); initial.emplace_back("show" + std::to_string(i), "false"); }
        for (const auto& [key, value] : initial) tree.State(key, value);
        for (const auto* command : kCommands) tree.Command(command);
        return model.create(model.context, View("Directory workbench"), &tree.Build(), &panel);
    }
    PulseStatus Start() {
        auto code = host.Query(PULSE_UI_MODEL_INTERFACE, 1, model); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_UI_DATA_INTERFACE, 1, data); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_UI_EVENTS_INTERFACE_NAME, 1, events); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_UI_MOUNT_INTERFACE, 1, mounting); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_DIRECTORIES_INTERFACE_NAME, 1, directories); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_CONTEXT_INTERFACE_NAME, 1, context); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_RESOURCES_INTERFACE_NAME, 1, resources); if (code != PULSE_OK) return code;
        code = context.get(context.context, ContextChanged, this); if (code != PULSE_OK) return code;
        code = Render(); if (code != PULSE_OK) return code;
        active = true;
        for (size_t i = 0; i < subscriptions.size(); ++i) { code = subscriptions[i].Command(model, panel, kCommands[i],
            [this](uint64_t id, PulseStringView command) { Click(this, id, command); }); if (code != PULSE_OK) return code; }
        code = state_subscription.State(model, events, panel, "", PULSE_UI_STATE_USER,
            [this](uint64_t id, const PulseUiStateBatch& batch) { StateChanged(this, id, &batch); }); if (code != PULSE_OK) return code;
        code = mounting.subscribe(mounting.context, panel, Mounted, this, &mount_subscription); if (code != PULSE_OK) return code;
        code = context.subscribe(context.context, ContextChanged, this, &context_subscription); if (code != PULSE_OK) return code;
        code = data.register_provider(data.context, Requested, Canceled, this, &provider); if (code != PULSE_OK) return code;
        code = Bind(); if (code != PULSE_OK) return code;
        LoadSettings();
        if (!context_path.empty()) OpenPath(context_path, false);
        else { failure = "Pulse context has no supported directory. Enter an absolute path."; RefreshTable(); }
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false;
        if (context_subscription) { context.unsubscribe(context.context, context_subscription); context_subscription = 0; }
        state_subscription.Reset();
        if (mount_subscription) { mounting.unsubscribe(mounting.context, mount_subscription); mount_subscription = 0; }
        if (settings_request) { resources.cancel_request(resources.context, settings_request); settings_request = 0; }
        ReleaseDirectory();
        if (provider) { data.request(data.context, panel, View("items"), PULSE_UI_DATA_CANCEL); data.unbind(data.context, panel, View("items")); data.unregister_provider(data.context, provider); provider = 0; }
        for (auto& subscription : subscriptions) subscription.Reset();
        if (panel) { model.close(model.context, panel); panel = 0; }
        ui_request = 0; saving = save_again = settings_loaded = false;
    }
    ~Plugin() { Stop(); }
};
PulseStatus PULSE_CALL Activate(void* instance) noexcept { try { return static_cast<Plugin*>(instance)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* instance) noexcept { static_cast<Plugin*>(instance)->Stop(); }
void PULSE_CALL Destroy(void* instance) noexcept { delete static_cast<Plugin*>(instance); }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) || host->version != 1 || out->version != 1) return PULSE_ERROR_ABI;
    try { *out = {sizeof(*out), 1, new Plugin(*host), Activate, Deactivate, Destroy}; return PULSE_OK; } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
