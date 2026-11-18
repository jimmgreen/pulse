#include <pulse/context.h>
#include <pulse/directories.h>
#include <pulse/ui_data.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1*, PulsePluginV1*) noexcept;
namespace {
constexpr uint32_t kEnd = (std::numeric_limits<uint32_t>::max)();
PulseStringView View(std::string_view value) { return {value.data(), static_cast<uint32_t>(value.size())}; }
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
struct Pending {
    uint32_t kind = 0, offset = 0, limit = 0;
    uint64_t directory = 0, snapshot = 0, entry = 0;
    std::string path;
    PulseDirectoryCompletionFn callback = nullptr;
    void* user = nullptr;
    bool fail = false;
};
struct Directory { std::string path; uint64_t snapshot = 0; uint32_t count = 0; };
struct Watch { uint64_t directory; PulseDirectoryChangeFn callback; void* user; };
struct Row { std::string id, name; };
struct Host {
    std::map<std::string, std::string> states;
    std::map<std::string, std::pair<PulseUiClickFn, void*>> commands;
    std::map<uint64_t, Pending> pending;
    std::map<uint64_t, Directory> directories;
    std::map<uint64_t, Watch> watches;
    std::deque<std::function<void()>> events;
    std::vector<Row> displayed;
    std::set<std::string> nodes;
    PulseUiDataRequestFn requested = nullptr;
    PulseUiDataCancelFn canceled = nullptr;
    void* provider_user = nullptr;
    PulseContextChangeFn context_callback = nullptr;
    void* context_user = nullptr;
    uint64_t serial = 10, directory_serial = 100, snapshot_serial = 1000, watch_serial = 10000;
    uint64_t ui_serial = 20000, ui_request = 0;
    uint64_t selection_revision = 1, navigation_revision = 1;
    std::string context_path = "C:\\A", context_selection = "initial.txt", revision, cursor;
    uint64_t context_selection_count = 1;
    uint32_t refreshes = 0, list_calls = 0, entry_calls = 0, canceled_calls = 0, callbacks = 0, peak_pending = 0, publish_errors = 0, reentered = 0;
    uint64_t last_entry_directory = 0, last_entry_snapshot = 0, last_entry = 0;
    bool in_provider = false, is_next = false, registered = false, bound = false, panel_closed = false, fail_next_refresh = false, fail_next_list = false;
    bool empty_table = false;
    uint32_t page_size = 0, column_width = 0;
    static Host& Self(void* context) { return *static_cast<Host*>(context); }
    static PulseStatus PULSE_CALL Query(void* context, PulseStringView name, uint32_t version, void* out, uint32_t size) {
        if (version != 1) return PULSE_ERROR_ABI;
        const auto key = Copy(name);
        if (key == PULSE_UI_MODEL_INTERFACE && size == sizeof(PulseUiModelV1)) {
            const PulseUiModelV1 table{sizeof(table), 1, context, Create, nullptr, SubscribeCommand, SetStates, GetState, UnsubscribeClick, ClosePanel};
            std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
        }
        if (key == PULSE_UI_DATA_INTERFACE && size == sizeof(PulseUiDataV1)) {
            const PulseUiDataV1 table{sizeof(table), 1, context, Register, Unregister, Bind, Unbind, DataRequest, Publish};
            std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
        }
        if (key == PULSE_DIRECTORIES_INTERFACE_NAME && size == sizeof(PulseDirectoriesV1)) {
            const PulseDirectoriesV1 table{sizeof(table), 1, context, Open, Refresh, List, OpenEntry, WatchDirectory, Unwatch, CloseDirectory, Cancel};
            std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
        }
        if (key == PULSE_CONTEXT_INTERFACE_NAME && size == sizeof(PulseContextV1)) {
            const PulseContextV1 table{sizeof(table), 1, context, ContextGet, ContextSubscribe, ContextUnsubscribe};
            std::memcpy(out, &table, sizeof(table)); return PULSE_OK;
        }
        return PULSE_ERROR_UNAVAILABLE;
    }
    void Inspect(const PulseUiModelNode& node) {
        nodes.insert(Copy(node.id));
        if (node.type == PULSE_UI_MODEL_DATA_TABLE) {
            empty_table = node.rows.present && node.rows.count == 0;
            for (uint32_t i = 0; i < node.data_columns.count; ++i) column_width += static_cast<uint32_t>(node.data_columns.data[i].width.value);
        }
        for (uint32_t i = 0; i < node.children.count; ++i) Inspect(node.children.data[i]);
    }
    static PulseStatus PULSE_CALL Create(void* context, PulseStringView, const PulseUiModelTree* tree, uint64_t* panel) {
        auto& self = Self(context); self.Inspect(*tree->root);
        for (uint32_t i = 0; i < tree->state_count; ++i) self.states[Copy(tree->states[i].key)] = Copy(tree->states[i].value);
        *panel = 1; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL SetStates(void* context, uint64_t, const PulseUiState* states, uint32_t count) {
        auto& self = Self(context);
        for (uint32_t i = 0; i < count; ++i) self.states[Copy(states[i].key)] = Copy(states[i].value);
        return PULSE_OK;
    }
    static PulseStatus PULSE_CALL GetState(void* context, uint64_t, PulseStringView key, char* buffer, uint32_t capacity, uint32_t* needed) {
        auto& self = Self(context); const auto found = self.states.find(Copy(key)); if (found == self.states.end()) return PULSE_ERROR_ARGUMENT;
        *needed = static_cast<uint32_t>(found->second.size());
        if (!buffer && !capacity) return PULSE_OK;
        if (capacity <= found->second.size()) return PULSE_ERROR_ARGUMENT;
        std::memcpy(buffer, found->second.data(), found->second.size()); buffer[found->second.size()] = 0; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL SubscribeCommand(void* context, uint64_t, PulseStringView id, PulseUiClickFn callback, void* user, uint64_t* subscription) {
        auto& self = Self(context); self.commands[Copy(id)] = {callback, user}; *subscription = self.commands.size(); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL UnsubscribeClick(void*, uint64_t) { return PULSE_OK; }
    static PulseStatus PULSE_CALL ClosePanel(void* context, uint64_t) { Self(context).panel_closed = true; return PULSE_OK; }
    static PulseStatus PULSE_CALL Register(void* context, PulseUiDataRequestFn requested, PulseUiDataCancelFn canceled, void* user, uint64_t* provider) {
        auto& self = Self(context); self.requested = requested; self.canceled = canceled; self.provider_user = user; self.registered = true; *provider = 1; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unregister(void* context, uint64_t) { auto& self = Self(context); DataRequest(context, 1, View("items"), PULSE_UI_DATA_CANCEL); self.registered = false; return PULSE_OK; }
    static PulseStatus PULSE_CALL Bind(void* context, uint64_t, PulseStringView, uint64_t, const PulseUiDataBinding* binding) {
        auto& self = Self(context); self.bound = true; self.page_size = binding->page_size; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unbind(void* context, uint64_t, PulseStringView) { auto& self = Self(context); DataRequest(context, 1, View("items"), PULSE_UI_DATA_CANCEL); self.bound = false; self.displayed.clear(); return PULSE_OK; }
    static PulseStatus PULSE_CALL DataRequest(void* context, uint64_t, PulseStringView, uint32_t operation) {
        auto& self = Self(context);
        if (self.in_provider) { ++self.reentered; return PULSE_ERROR_STATE; }
        if (operation == PULSE_UI_DATA_NEXT_PAGE && self.cursor.empty()) return PULSE_ERROR_STATE;
        const auto old = self.ui_request; self.ui_request = 0;
        if (old) { self.in_provider = true; self.canceled(self.provider_user, old); self.in_provider = false; }
        if (operation == PULSE_UI_DATA_CANCEL) return PULSE_OK;
        self.ui_request = ++self.ui_serial; self.is_next = operation == PULSE_UI_DATA_NEXT_PAGE;
        const std::string cursor = self.is_next ? self.cursor : "";
        const PulseUiDataRequest request{sizeof(request), 256, self.ui_request, 1, 1, View("items"), View(""), View("nameAsc"), View(cursor)};
        self.in_provider = true; self.requested(self.provider_user, &request); self.in_provider = false; return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Publish(void* context, uint64_t request, const PulseUiDataPage* page) {
        auto& self = Self(context);
        if (!self.registered || request != self.ui_request) return PULSE_ERROR_STATE;
        if (page->row_count > 256 || (page->status == PULSE_UI_DATA_READY && page->revision.length == 0)) { ++self.publish_errors; return PULSE_ERROR_ARGUMENT; }
        self.states["status"] = page->status == PULSE_UI_DATA_LOADING ? "loading" : page->status == PULSE_UI_DATA_READY ? "ready" : "error";
        self.states["error"] = Copy(page->error);
        if (page->status == PULSE_UI_DATA_READY) {
            if (self.is_next && self.revision != Copy(page->revision)) { ++self.publish_errors; return PULSE_ERROR_STATE; }
            if (!self.is_next) self.displayed.clear();
            for (uint32_t i = 0; i < page->row_count; ++i) self.displayed.push_back({Copy(page->rows[i].id), Copy(page->rows[i].text.value)});
            if (self.displayed.size() > 4096) { ++self.publish_errors; return PULSE_ERROR_ARGUMENT; }
            self.revision = Copy(page->revision); self.cursor = Copy(page->next_cursor);
        }
        return PULSE_OK;
    }
    PulseStatus Add(Pending request, PulseDirectoryRequestId* id) {
        *id = ++serial; pending.emplace(*id, std::move(request)); peak_pending = (std::max)(peak_pending, static_cast<uint32_t>(pending.size()));
        const auto request_id = *id; events.push_back([this, request_id] { Deliver(request_id); }); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Open(void* context, PulseStringView path, PulseDirectoryCompletionFn callback, void* user, PulseDirectoryRequestId* id) {
        Pending request{}; request.kind = PULSE_DIRECTORY_OPEN; request.path = Copy(path); request.callback = callback; request.user = user;
        return Self(context).Add(std::move(request), id);
    }
    static PulseStatus PULSE_CALL Refresh(void* context, uint64_t directory, PulseDirectoryCompletionFn callback, void* user, PulseDirectoryRequestId* id) {
        auto& self = Self(context); if (!self.directories.contains(directory)) return PULSE_ERROR_STATE;
        ++self.refreshes; Pending request{}; request.kind = PULSE_DIRECTORY_REFRESH; request.directory = directory; request.callback = callback; request.user = user;
        request.fail = self.fail_next_refresh; self.fail_next_refresh = false; return self.Add(std::move(request), id);
    }
    static PulseStatus PULSE_CALL List(void* context, uint64_t directory, uint64_t snapshot, uint32_t offset, uint32_t limit, PulseDirectoryCompletionFn callback, void* user, PulseDirectoryRequestId* id) {
        auto& self = Self(context); if (!self.directories.contains(directory) || limit == 0 || limit > 256) return PULSE_ERROR_ARGUMENT;
        ++self.list_calls; Pending request{}; request.kind = PULSE_DIRECTORY_LIST; request.directory = directory; request.snapshot = snapshot; request.offset = offset; request.limit = limit; request.callback = callback; request.user = user;
        request.fail = self.fail_next_list; self.fail_next_list = false; return self.Add(std::move(request), id);
    }
    static PulseStatus PULSE_CALL OpenEntry(void* context, uint64_t directory, uint64_t snapshot, uint64_t entry, PulseDirectoryCompletionFn callback, void* user, PulseDirectoryRequestId* id) {
        auto& self = Self(context); ++self.entry_calls; self.last_entry_directory = directory; self.last_entry_snapshot = snapshot; self.last_entry = entry;
        Pending request{}; request.kind = PULSE_DIRECTORY_OPEN_ENTRY; request.directory = directory; request.snapshot = snapshot; request.entry = entry; request.callback = callback; request.user = user;
        return self.Add(std::move(request), id);
    }
    static PulseStatus PULSE_CALL WatchDirectory(void* context, uint64_t directory, PulseDirectoryChangeFn callback, void* user, PulseDirectoryWatchId* watch) {
        auto& self = Self(context); *watch = ++self.watch_serial; self.watches.emplace(*watch, Watch{directory, callback, user});
        const auto id = *watch; self.events.push_back([&self, id] { self.Change(id, PULSE_DIRECTORY_WATCH_READY); }); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL Unwatch(void* context, uint64_t watch) { Self(context).watches.erase(watch); return PULSE_OK; }
    static PulseStatus PULSE_CALL Cancel(void* context, uint64_t request) { auto& self = Self(context); ++self.canceled_calls; self.pending.erase(request); return PULSE_OK; }
    static PulseStatus PULSE_CALL CloseDirectory(void* context, uint64_t directory) {
        auto& self = Self(context); self.directories.erase(directory);
        std::erase_if(self.pending, [&](const auto& value) { return value.second.directory == directory; });
        std::erase_if(self.watches, [&](const auto& value) { return value.second.directory == directory; }); return PULSE_OK;
    }
    void Context(PulseContextChangeFn callback, void* user, uint32_t changes) {
        const PulseContextSnapshot value{sizeof(value), changes, navigation_revision + selection_revision, navigation_revision, selection_revision, View(context_path), View(context_selection), View("details"), context_selection_count, 0, 0};
        ++callbacks; callback(user, &value);
    }
    static PulseStatus PULSE_CALL ContextGet(void* context, PulseContextChangeFn callback, void* user) { auto& self = Self(context); self.Context(callback, user, PULSE_CONTEXT_INITIAL); return PULSE_OK; }
    static PulseStatus PULSE_CALL ContextSubscribe(void* context, PulseContextChangeFn callback, void* user, PulseContextSubscriptionId* id) {
        auto& self = Self(context); self.context_callback = callback; self.context_user = user; *id = 1;
        self.events.push_back([&self] { if (self.context_callback) self.Context(self.context_callback, self.context_user, PULSE_CONTEXT_INITIAL); }); return PULSE_OK;
    }
    static PulseStatus PULSE_CALL ContextUnsubscribe(void* context, uint64_t) { Self(context).context_callback = nullptr; Self(context).context_user = nullptr; return PULSE_OK; }
    void Deliver(uint64_t request_id) {
        const auto found = pending.find(request_id); if (found == pending.end()) return;
        const Pending request = found->second; pending.erase(found);
        PulseDirectoryReply reply{}; reply.size = sizeof(reply); reply.operation = request.kind; reply.request_id = request_id; reply.directory_id = request.directory;
        reply.status = PULSE_OK; reply.next_offset = kEnd;
        std::string path, error; std::vector<std::string> names; std::vector<PulseDirectoryRow> rows;
        if (request.kind == PULSE_DIRECTORY_OPEN) {
            if (request.path == "C:\\missing") { reply.status = PULSE_ERROR_STATE; error = "fixture missing"; }
            else {
                reply.directory_id = ++directory_serial;
                const uint32_t count = request.path == "C:\\empty" ? 0 : request.path == "C:\\B" ? 3 : request.path == "C:\\large" ? 5000 : 600;
                directories.emplace(reply.directory_id, Directory{request.path, ++snapshot_serial, count});
            }
        }
        if (reply.status == PULSE_OK) {
            const auto directory = directories.find(reply.directory_id);
            if (directory == directories.end()) { reply.status = PULSE_ERROR_STATE; error = "closed directory"; }
            else {
                auto& value = directory->second; path = value.path;
                if (request.kind == PULSE_DIRECTORY_REFRESH) {
                    if (request.fail) { reply.status = PULSE_ERROR_STATE; error = "fixture refresh failure"; }
                    else value.snapshot = ++snapshot_serial;
                }
                reply.snapshot_id = value.snapshot; reply.total = value.count;
                if (request.kind == PULSE_DIRECTORY_LIST && request.fail) { reply.status = PULSE_ERROR_STATE; error = "fixture list failure"; }
                if ((request.kind == PULSE_DIRECTORY_LIST || request.kind == PULSE_DIRECTORY_OPEN_ENTRY) && request.snapshot != value.snapshot) { reply.status = PULSE_ERROR_STATE; error = "stale snapshot"; }
                if (reply.status == PULSE_OK && request.kind == PULSE_DIRECTORY_LIST) {
                    const auto end = (std::min)(request.offset + request.limit, value.count);
                    for (uint32_t i = request.offset; i < end; ++i) names.push_back("entry-" + std::to_string(i + 1) + ".txt");
                    for (uint32_t i = 0; i < names.size(); ++i) rows.push_back({value.snapshot * 100000 + request.offset + i + 1, View(names[i]), 1024 + request.offset + i, 0, 0, request.offset + i == 0 ? PULSE_DIRECTORY_ENTRY_FOLDER : 0u});
                    reply.rows = rows.data(); reply.count = static_cast<uint32_t>(rows.size()); reply.next_offset = end < value.count ? end : kEnd;
                }
                if (request.kind == PULSE_DIRECTORY_OPEN_ENTRY && (request.entry <= value.snapshot * 100000 || request.entry > value.snapshot * 100000 + value.count)) { reply.status = PULSE_ERROR_STATE; error = "stale entry"; }
            }
        }
        reply.path = View(path); reply.error = View(error); ++callbacks; request.callback(request.user, &reply);
    }
    void Change(uint64_t id, uint32_t kind, uint32_t overflow = 0) {
        const auto found = watches.find(id); if (found == watches.end()) return;
        const auto watch = found->second; const PulseDirectoryChange change{sizeof(change), kind, id, watch.directory, 1, PULSE_OK, overflow, View("")};
        ++callbacks; watch.callback(watch.user, &change);
    }
    void Pump() { unsigned budget = 10000; while (!events.empty() && budget--) { auto event = std::move(events.front()); events.pop_front(); event(); } }
    void Click(std::string_view command) { const auto found = commands.find(std::string(command)); if (found != commands.end()) found->second.first(found->second.second, 1, View(command)); }
    void OpenPath(std::string path) { states["directory"] = std::move(path); Click("open-directory"); }
    uint64_t WatchId() const { return watches.empty() ? 0 : watches.begin()->first; }
};
bool Check(bool result, const char* name) { std::cout << (result ? "[PASS] " : "[FAIL] ") << name << '\n'; return result; }
}
int main() {
    Host fake; const PulseHostV1 host{sizeof(host), 1, &fake, Host::Query}; PulsePluginV1 plugin{}; plugin.size = sizeof(plugin); plugin.version = 1;
    bool ok = Check(PulsePluginEntryV1(&host, &plugin) == PULSE_OK && plugin.activate(plugin.instance) == PULSE_OK && fake.states["directory"] == "C:\\A" && fake.directories.empty() && fake.pending.size() == 1 && fake.empty_table && fake.page_size == 256 && fake.column_width <= 370 && fake.nodes.contains("selection-label"), "public-only activation uses current context and deferred directory open");
    fake.Pump();
    ok &= Check(fake.states["status"] == "ready" && fake.displayed.size() == 256 && fake.refreshes == 2 && fake.watches.size() == 1 && !fake.cursor.empty(), "watch READY causes a second refresh after the arming window");
    const auto first_revision = fake.revision; fake.Click("next"); fake.Pump();
    ok &= Check(fake.displayed.size() == 512 && fake.revision == first_revision && fake.displayed[256].name == "entry-257.txt", "next appends 256 entries from the same immutable snapshot");
    fake.Click("next"); fake.Pump();
    ok &= Check(fake.displayed.size() == 600 && fake.cursor.empty() && fake.displayed.back().name == "entry-600.txt", "final page is complete and exhausts its cursor");
    const auto selected = fake.displayed.front().id; fake.states["selected"] = selected; fake.Click("open-item"); fake.Pump();
    ok &= Check(fake.entry_calls == 1 && fake.last_entry_directory == fake.directories.begin()->first && fake.last_entry_snapshot == fake.directories.begin()->second.snapshot && fake.last_entry == fake.last_entry_snapshot * 100000 + 1 && fake.states["message"].find("dispatch accepted") != std::string::npos, "selected UI row resolves directory/snapshot/entry before open_entry");
    fake.Click("refresh"); fake.Pump(); fake.states["selected"] = selected; fake.Click("open-item");
    ok &= Check(fake.entry_calls == 1 && fake.states["message"].find("current snapshot") != std::string::npos, "obsolete row ID never reuses an entry token after refresh");
    const auto before_burst = fake.refreshes;
    for (unsigned i = 0; i < 100; ++i) fake.Change(fake.WatchId(), PULSE_DIRECTORY_INVALIDATED, i == 99 ? 1u : 0u);
    fake.Pump();
    ok &= Check(fake.refreshes == before_burst + 2 && fake.peak_pending <= 2 && fake.displayed.size() == 256, "100 invalidations and overflow coalesce into at most one queued refresh");
    const auto old_watch_id = fake.WatchId(); const auto old_watch = fake.watches.begin()->second;
    fake.Click("refresh"); fake.OpenPath("C:\\B");
    const PulseDirectoryChange stale{sizeof(stale), PULSE_DIRECTORY_INVALIDATED, old_watch_id, old_watch.directory, 2, PULSE_OK, 0, View("")};
    old_watch.callback(old_watch.user, &stale); fake.Pump();
    ok &= Check(fake.directories.size() == 1 && fake.directories.begin()->second.path == "C:\\B" && fake.displayed.size() == 3 && fake.watches.size() == 1 && fake.canceled_calls > 0, "A-to-B switching cancels old work and ignores stale watch tokens");
    fake.OpenPath("C:\\missing"); fake.Pump();
    ok &= Check(fake.states["status"] == "error" && fake.states["error"].find("fixture missing") != std::string::npos && fake.directories.empty() && fake.watches.empty() && fake.states["message"] == "Directory open failed. Check the path and choose Open directory to retry.", "asynchronous open failure closes the previous directory with retry guidance");
    fake.OpenPath("C:\\empty"); fake.Pump(); const bool empty = fake.states["status"] == "ready" && fake.displayed.empty() && fake.cursor.empty();
    fake.OpenPath("C:\\B"); fake.Pump();
    ok &= Check(empty && fake.displayed.size() == 3 && fake.states["status"] == "ready", "empty snapshot and reopening recover after a failed open");
    const auto preserved_snapshot = fake.directories.begin()->second.snapshot; fake.fail_next_refresh = true; fake.Click("refresh"); fake.Pump();
    const bool preserved = fake.states["status"] == "error" && fake.directories.begin()->second.snapshot == preserved_snapshot && fake.displayed.size() == 3 && fake.states["message"] == "Directory request failed. Choose Refresh to retry.";
    fake.Click("refresh"); fake.Pump();
    const bool recovered_refresh = fake.states["status"] == "ready" && fake.displayed.size() == 3;
    fake.fail_next_list = true; fake.Click("refresh"); fake.Pump();
    const bool failed_list = fake.states["status"] == "error" && fake.states["error"].find("fixture list failure") != std::string::npos && fake.states["message"] == "Directory request failed. Choose Refresh to retry.";
    fake.Click("refresh"); fake.Pump();
    ok &= Check(preserved && recovered_refresh && failed_list && fake.states["status"] == "ready" && fake.displayed.size() == 3, "refresh/list failures show retry guidance and manual retry recovers");
    fake.context_path = "C:\\other"; fake.context_selection = "selected.txt"; fake.context_selection_count = 2; ++fake.navigation_revision; ++fake.selection_revision;
    fake.Context(fake.context_callback, fake.context_user, PULSE_CONTEXT_NAVIGATION | PULSE_CONTEXT_SELECTION);
    const auto old_selection = fake.states["selection"]; ++fake.selection_revision; fake.Context(fake.context_callback, fake.context_user, PULSE_CONTEXT_SELECTION);
    ok &= Check(fake.states["context"].find("C:\\other") != std::string::npos && fake.states["selection"].find("selected.txt") != std::string::npos && fake.states["selection"] != old_selection && fake.states["directory"] == "C:\\B", "context navigation and selection revisions update without overwriting typed path");
    fake.OpenPath("C:\\large"); fake.Pump(); unsigned pages = 0;
    while (!fake.cursor.empty() && pages++ < 20) { fake.Click("next"); fake.Pump(); }
    ok &= Check(fake.displayed.size() == 4096 && fake.cursor.empty() && fake.states["message"].find("snapshot is complete") != std::string::npos, "UI row budget is explicit while directory service snapshot stays complete");
    fake.states["selected"] = fake.displayed.front().id; fake.Click("open-item"); fake.Click("refresh"); fake.Click("close-directory"); fake.Pump();
    ok &= Check(fake.pending.empty() && fake.directories.empty() && fake.watches.empty() && fake.displayed.empty() && fake.states["path"] == "Directory: closed" && fake.states["status"] == "cancelled", "close cancels entry/list work and watch before releasing the directory");
    fake.OpenPath("C:\\A");
    plugin.deactivate(plugin.instance); plugin.destroy(plugin.instance);
    const auto completed_callbacks = fake.callbacks; fake.Pump();
    ok &= Check(fake.callbacks == completed_callbacks && fake.pending.empty() && fake.watches.empty() && fake.directories.empty() && !fake.context_callback && !fake.registered && fake.panel_closed && fake.reentered == 0 && fake.publish_errors == 0, "destroy suppresses deferred directory/context callbacks without UI-data reentry");
    return ok ? 0 : 1;
}
