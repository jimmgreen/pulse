#include <pulse/sdk.hpp>
#include <pulse/context.h>
#include <pulse/directories.h>
#include <pulse/ui_data.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using pulse::sdk::View;
PulseUiModelString Text(std::string_view value) { return {1, View(value)}; }
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
std::string Clip(std::string value, size_t limit = 1000) {
    if (value.size() <= limit) return value;
    while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
    value.resize(limit); return value;
}
constexpr uint32_t kExhausted = (std::numeric_limits<uint32_t>::max)();
constexpr uint32_t kUiRowLimit = 4096;
const char* commands[] = {"open-directory", "refresh", "next", "close-directory", "open-item"};
struct Entry { uint64_t directory, snapshot, entry; };

struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 model{};
    PulseUiDataV1 data{};
    PulseDirectoriesV1 directories{};
    PulseContextV1 context{};
    uint64_t panel = 0, provider = 0, ui_request = 0, epoch = 0;
    PulseDirectoryId directory = 0;
    PulseDirectoryWatchId watch = 0;
    PulseDirectoryRequestId opening = 0, operation = 0, opening_entry = 0;
    PulseContextSubscriptionId context_subscription = 0;
    std::array<uint64_t, 5> subscriptions{};
    uint64_t snapshot = 0, total = 0;
    uint32_t operation_offset = 0;
    bool active = false, dirty = false;
    std::string initial_path, context_text, selection_text;
    std::unordered_map<std::string, Entry> entries;

    explicit Plugin(const PulseHostV1& h) : host(h) {}
    void Set(std::string_view key, std::string_view value) noexcept {
        if (!active || !panel) return;
        const PulseUiState state{View(key), View(value)}; model.set_states(model.context, panel, &state, 1);
    }
    void Message(std::string value) { Set("message", Clip(std::move(value))); }
    std::string Read(std::string_view key) {
        uint32_t needed = 0;
        if (model.get_state(model.context, panel, View(key), nullptr, 0, &needed) != PULSE_OK || needed > 32768) return {};
        std::vector<char> text(static_cast<size_t>(needed) + 1);
        if (model.get_state(model.context, panel, View(key), text.data(), static_cast<uint32_t>(text.size()), &needed) != PULSE_OK) return {};
        return std::string(text.data(), needed);
    }
    void Status(uint32_t status, std::string error = {}) {
        if (!ui_request) return;
        error = Clip(std::move(error));
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = status;
        page.progress = status == PULSE_UI_DATA_LOADING ? 10u : 0u; page.error = View(error);
        data.publish(data.context, ui_request, &page);
        if (status == PULSE_UI_DATA_ERROR) Message(directory
            ? "Directory request failed. Choose Refresh to retry."
            : "Directory open failed. Check the path and choose Open directory to retry.");
    }
    void Empty() {
        if (!ui_request) return;
        const std::string revision = "closed-" + std::to_string(epoch);
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = PULSE_UI_DATA_READY;
        page.progress = 100; page.revision = View(revision); data.publish(data.context, ui_request, &page);
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
        snapshot = total = 0; dirty = false; entries.clear(); ++epoch;
    }
    PulseStatus Bind() {
        const PulseUiDataBinding binding{sizeof(binding), 256, View("filter"), View("sort"), View("status"), View("error"), View("progress")};
        return data.bind(data.context, panel, View("items"), provider, &binding);
    }
    void CloseDirectory(bool clear) {
        data.request(data.context, panel, View("items"), PULSE_UI_DATA_CANCEL);
        ReleaseDirectory(); Set("selected", ""); Set("path", "Directory: closed");
        data.unbind(data.context, panel, View("items"));
        if (Bind() != PULSE_OK) { Message("Could not rebind the cleared directory table."); return; }
        Set("status", "cancelled"); Set("error", ""); Set("progress", "0");
        if (clear) Message("Directory closed; requests and watch canceled.");
    }
    PulseStatus List(uint32_t offset) {
        operation_offset = offset;
        return directories.list(directories.context, directory, snapshot, offset, 256, Completed, this, &operation);
    }
    void RequestRefresh() {
        if (!active || !directory) return;
        if (operation || opening) { dirty = true; return; }
        dirty = false;
        const auto code = data.request(data.context, panel, View("items"), PULSE_UI_DATA_REFRESH);
        if (code != PULSE_OK) Message("The UI data controller rejected refresh.");
    }
    void Finish() {
        if (dirty && active && directory) RequestRefresh();
    }
    void OpenDirectory() {
        const auto path = Read("directory");
        if (path.empty()) { Message("Enter an absolute directory path."); return; }
        CloseDirectory(false);
        Set("path", Clip("Opening: " + path)); Message("Opening directory on the host worker...");
        const auto code = directories.open(directories.context, View(path), Completed, this, &opening);
        data.request(data.context, panel, View("items"), PULSE_UI_DATA_REFRESH);
        if (code != PULSE_OK) { opening = 0; Status(PULSE_UI_DATA_ERROR, "Directory open request rejected."); }
    }
    void OpenItem() {
        const auto selected = Read("selected");
        const auto found = entries.find(selected);
        if (found == entries.end() || found->second.directory != directory || found->second.snapshot != snapshot) {
            Message("Select a row from the current snapshot before opening it."); return;
        }
        if (opening_entry) { directories.cancel(directories.context, opening_entry); opening_entry = 0; }
        const auto token = found->second;
        const auto code = directories.open_entry(directories.context, token.directory, token.snapshot, token.entry, Completed, this, &opening_entry);
        if (code != PULSE_OK) { opening_entry = 0; Message("Open entry request rejected; Refresh and select again."); }
        else Message("Opening the selected snapshot entry...");
    }
    void Page(const PulseDirectoryReply& reply) {
        if (!ui_request) return;
        if (reply.snapshot_id != snapshot || reply.directory_id != directory) { Status(PULSE_UI_DATA_ERROR, "Directory page belongs to an obsolete snapshot."); return; }
        std::vector<std::string> ids;
        std::vector<std::array<std::string, 3>> values;
        size_t bytes = 0;
        for (uint32_t i = 0; i < reply.count && i < 256 && operation_offset + i < kUiRowLimit; ++i) {
            const auto& row = reply.rows[i];
            const auto id = "s" + std::to_string(snapshot) + "-e" + std::to_string(row.entry_id);
            const auto name = Copy(row.name);
            const auto size = row.flags & PULSE_DIRECTORY_ENTRY_FOLDER ? "" : std::to_string(row.size_bytes);
            const std::string type = row.flags & PULSE_DIRECTORY_ENTRY_FOLDER ? "Folder" : row.flags & PULSE_DIRECTORY_ENTRY_REPARSE ? "Link" : "File";
            const auto next_bytes = id.size() + name.size() * 2 + size.size() + type.size();
            if (bytes + next_bytes > 60000) break;
            bytes += next_bytes; ids.push_back(id); values.push_back({name, size, type});
        }
        if (reply.count && ids.empty()) { Status(PULSE_UI_DATA_ERROR, "One directory row exceeds the UI page byte budget."); return; }
        const auto consumed = static_cast<uint32_t>(ids.size());
        auto next = consumed < reply.count ? operation_offset + consumed : reply.next_offset;
        if (next >= kUiRowLimit) next = kExhausted;
        const std::string revision = "snapshot-" + std::to_string(snapshot);
        const std::string cursor = next != kExhausted ? std::to_string(snapshot) + "@" + std::to_string(next) : "";
        std::vector<std::array<PulseStringView, 3>> cells;
        for (const auto& value : values) cells.push_back({View(value[0]), View(value[1]), View(value[2])});
        std::vector<PulseUiModelRow> rows;
        for (size_t i = 0; i < ids.size(); ++i) {
            PulseUiModelRow row{}; row.size = sizeof(row); row.id = View(ids[i]); row.text = Text(values[i][0]);
            row.cells = {1, cells[i].data(), 3}; rows.push_back(row);
        }
        PulseUiDataPage page{}; page.size = sizeof(page); page.status = PULSE_UI_DATA_READY; page.progress = 100;
        page.revision = View(revision); page.next_cursor = View(cursor); page.rows = rows.data(); page.row_count = consumed;
        if (data.publish(data.context, ui_request, &page) == PULSE_OK) {
            for (uint32_t i = 0; i < consumed; ++i) entries[ids[i]] = {directory, snapshot, reply.rows[i].entry_id};
            Message("Snapshot " + std::to_string(snapshot) + ": " + std::to_string(total) + " entries. " +
                (total > kUiRowLimit ? "UI shows up to 4096; the service snapshot is complete." : "Next appends an immutable snapshot page."));
        }
    }
    void Complete(const PulseDirectoryReply& reply) {
        if (!active) return;
        if (reply.request_id == opening_entry) {
            opening_entry = 0;
            Message(reply.status == PULSE_OK ? "Open dispatch accepted; application launch completion is not reported." : "Open entry failed: " + Copy(reply.error)); return;
        }
        if (reply.request_id == opening) {
            opening = 0;
            if (reply.status != PULSE_OK) { Status(PULSE_UI_DATA_ERROR, "Open failed: " + Copy(reply.error)); Set("path", "Directory: closed (open failed)"); return; }
            directory = reply.directory_id; snapshot = reply.snapshot_id; total = reply.total;
            Set("path", Clip("Directory: " + Copy(reply.path)));
            const auto code = directories.watch(directories.context, directory, Changed, this, &watch);
            if (code != PULSE_OK) { watch = 0; Message("Watch unavailable; manual Refresh still works."); }
            RequestRefresh(); return;
        }
        if (reply.request_id != operation || reply.directory_id != directory) return;
        operation = 0;
        if (reply.status != PULSE_OK) { Status(PULSE_UI_DATA_ERROR, "Directory operation failed: " + Copy(reply.error)); Finish(); return; }
        if (reply.operation == PULSE_DIRECTORY_REFRESH) {
            snapshot = reply.snapshot_id; total = reply.total; entries.clear(); Set("selected", "");
            const auto code = List(0);
            if (code != PULSE_OK) { operation = 0; Status(PULSE_UI_DATA_ERROR, "Snapshot list request rejected."); Finish(); }
        } else if (reply.operation == PULSE_DIRECTORY_LIST) { Page(reply); Finish(); }
    }
    static void PULSE_CALL Completed(void* user, const PulseDirectoryReply* reply) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        try { if (reply) self.Complete(*reply); }
        catch (...) { try { self.Status(PULSE_UI_DATA_ERROR, "Directory callback failed inside the DLL."); } catch (...) {} }
    }
    static void PULSE_CALL Changed(void* user, const PulseDirectoryChange* change) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!self.active || !change || change->watch_id != self.watch || change->directory_id != self.directory) return;
        try {
            if (change->kind == PULSE_DIRECTORY_WATCH_READY || change->kind == PULSE_DIRECTORY_INVALIDATED) self.RequestRefresh();
            else if (change->kind == PULSE_DIRECTORY_WATCH_ERROR) {
                const auto error = Clip("Watch failed; use manual Refresh: " + Copy(change->error)); self.Set("error", error); self.Message(error);
            }
        } catch (...) { self.Set("message", "Directory watch callback failed."); }
    }
    static void PULSE_CALL Requested(void* user, const PulseUiDataRequest* request) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!self.active || !request) return;
        try {
            self.ui_request = request->request;
            if (!self.directory) { if (self.opening) self.Status(PULSE_UI_DATA_LOADING); else self.Empty(); return; }
            self.Status(PULSE_UI_DATA_LOADING);
            const auto cursor = Copy(request->cursor);
            if (!cursor.empty()) {
                const auto separator = cursor.find('@'); uint64_t snapshot_id = 0; uint32_t offset = 0;
                if (separator == std::string::npos) { self.Status(PULSE_UI_DATA_ERROR, "Malformed page cursor."); return; }
                const auto a = std::from_chars(cursor.data(), cursor.data() + separator, snapshot_id);
                const auto b = std::from_chars(cursor.data() + separator + 1, cursor.data() + cursor.size(), offset);
                if (a.ec != std::errc{} || a.ptr != cursor.data() + separator || b.ec != std::errc{} || b.ptr != cursor.data() + cursor.size() || snapshot_id != self.snapshot || offset >= kUiRowLimit) {
                    self.Status(PULSE_UI_DATA_ERROR, "Stale page cursor. Refresh before paging again."); return;
                }
                if (self.List(offset) != PULSE_OK) { self.operation = 0; self.Status(PULSE_UI_DATA_ERROR, "Snapshot page request rejected."); }
            } else {
                self.dirty = false;
                if (self.directories.refresh(self.directories.context, self.directory, Completed, &self, &self.operation) != PULSE_OK) {
                    self.operation = 0; self.Status(PULSE_UI_DATA_ERROR, "Directory refresh request rejected.");
                }
            }
        } catch (...) { try { self.Status(PULSE_UI_DATA_ERROR, "Directory data request failed inside the DLL."); } catch (...) {} }
    }
    static void PULSE_CALL Canceled(void* user, uint64_t request) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (self.ui_request != request) return;
        self.ui_request = 0; self.CancelOperation();
    }
    static void PULSE_CALL ContextChanged(void* user, const PulseContextSnapshot* snapshot) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!snapshot) return;
        try {
            const auto path = Copy(snapshot->directory);
            if (!self.active && path.size() <= 1024) self.initial_path = path;
            self.context_text = Clip("Pulse: " + path + " | " + Copy(snapshot->view_mode) + (snapshot->loading ? " | loading" : " | ready") + " | revision " + std::to_string(snapshot->navigation_revision));
            self.selection_text = Clip("Pulse selection: " + std::to_string(snapshot->selection_count) + " | " + Copy(snapshot->selection_name) + " | revision " + std::to_string(snapshot->selection_revision));
            self.Set("context", self.context_text); self.Set("selection", self.selection_text);
        } catch (...) { self.Set("message", "Context update failed inside the DLL."); }
    }
    static void PULSE_CALL Click(void* user, uint64_t panel, PulseStringView command) noexcept {
        auto& self = *static_cast<Plugin*>(user);
        if (!self.active || panel != self.panel) return;
        try {
            const auto id = Copy(command);
            if (id == "open-directory") self.OpenDirectory();
            else if (id == "refresh") self.RequestRefresh();
            else if (id == "close-directory") self.CloseDirectory(true);
            else if (id == "open-item") self.OpenItem();
            else if (id == "next" && self.data.request(self.data.context, self.panel, View("items"), PULSE_UI_DATA_NEXT_PAGE) != PULSE_OK) self.Message("No next page. Refresh or open a directory first.");
        } catch (...) { self.Set("message", "Directory action failed inside the DLL."); }
    }
    PulseStatus Render() {
        PulseUiModelNode input{}; input.size = sizeof(input); input.type = PULSE_UI_MODEL_TEXT_INPUT;
        input.id = View("directory-input"); input.text = Text("Absolute directory path"); input.state = Text("directory"); input.placeholder = Text("C:\\folder or \\\\server\\share");
        std::array<PulseUiModelNode, 5> buttons{}; std::array<PulseStringView, 5> declared{};
        const char* titles[] = {"Open directory", "Refresh", "Next 256", "Close directory", "Open selected"};
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto& button = buttons[i]; button.size = sizeof(button); button.type = PULSE_UI_MODEL_BUTTON;
            button.id = View(commands[i]); button.text = Text(titles[i]); button.command = Text(commands[i]); declared[i] = View(commands[i]);
        }
        PulseUiModelNode controls{}; controls.size = sizeof(controls); controls.type = PULSE_UI_MODEL_GRID;
        controls.id = View("controls"); controls.columns = {1, 3}; controls.gap = {1, 6}; controls.children = {1, buttons.data(), 5};
        std::array<PulseUiModelNode, 6> labels{};
        const char* label_ids[] = {"status-label", "error-label", "path-label", "context-label", "selection-label", "message-label"};
        const char* bindings[] = {"state.status", "state.error", "state.path", "state.context", "state.selection", "state.message"};
        for (size_t i = 0; i < labels.size(); ++i) {
            labels[i].size = sizeof(PulseUiModelNode); labels[i].type = PULSE_UI_MODEL_TEXT; labels[i].id = View(label_ids[i]); labels[i].binding = Text(bindings[i]);
        }
        std::array<PulseUiModelColumn, 3> columns{};
        const char* column_ids[] = {"name", "size", "type"}; const char* column_titles[] = {"Name", "Size", "Type"}; const int64_t widths[] = {210, 80, 80};
        for (size_t i = 0; i < columns.size(); ++i) { columns[i].size = sizeof(PulseUiModelColumn); columns[i].id = View(column_ids[i]); columns[i].text = View(column_titles[i]); columns[i].width = {1, widths[i]}; }
        PulseUiModelNode table{}; table.size = sizeof(table); table.type = PULSE_UI_MODEL_DATA_TABLE;
        table.id = View("items"); table.state = Text("selected"); table.rows = {1, nullptr, 0}; table.data_columns = {1, columns.data(), 3}; table.min_height = {1, 240};
        const PulseUiModelNode children[] = {input, controls, labels[0], labels[1], labels[2], labels[3], labels[4], labels[5], table};
        PulseUiModelNode stack{}; stack.size = sizeof(stack); stack.type = PULSE_UI_MODEL_STACK; stack.id = View("content"); stack.gap = {1, 6}; stack.children = {1, children, 9};
        PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_SCROLL_VIEW; root.id = View("root"); root.children = {1, &stack, 1};
        const PulseUiState states[] = {
            {View("directory"), View(initial_path)}, {View("selected"), View("")}, {View("filter"), View("")}, {View("sort"), View("nameAsc")},
            {View("status"), View("")}, {View("error"), View("")}, {View("progress"), View("0")}, {View("path"), View("Directory: closed")},
            {View("context"), View(context_text)}, {View("selection"), View(selection_text)}, {View("message"), View("Open a directory to browse a host-owned immutable snapshot.")}};
        const PulseUiModelTree tree{sizeof(tree), &root, states, 11, nullptr, 0, declared.data(), 5};
        return model.create(model.context, View("Native directory browser"), &tree, &panel);
    }
    PulseStatus Start() {
        auto code = host.Query(PULSE_UI_MODEL_INTERFACE, 1, model); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_UI_DATA_INTERFACE, 1, data); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_DIRECTORIES_INTERFACE_NAME, 1, directories); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_CONTEXT_INTERFACE_NAME, 1, context); if (code != PULSE_OK) return code;
        code = context.get(context.context, ContextChanged, this); if (code != PULSE_OK) return code;
        code = Render(); if (code != PULSE_OK) return code;
        active = true;
        code = context.subscribe(context.context, ContextChanged, this, &context_subscription); if (code != PULSE_OK) return code;
        code = data.register_provider(data.context, Requested, Canceled, this, &provider); if (code != PULSE_OK) return code;
        code = Bind(); if (code != PULSE_OK) return code;
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            code = model.subscribe_command(model.context, panel, View(commands[i]), Click, this, &subscriptions[i]); if (code != PULSE_OK) return code;
        }
        if (!initial_path.empty()) OpenDirectory();
        else data.request(data.context, panel, View("items"), PULSE_UI_DATA_REFRESH);
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false;
        if (context_subscription) { context.unsubscribe(context.context, context_subscription); context_subscription = 0; }
        if (provider) { data.request(data.context, panel, View("items"), PULSE_UI_DATA_CANCEL); ReleaseDirectory(); data.unbind(data.context, panel, View("items")); data.unregister_provider(data.context, provider); provider = 0; }
        else if (directory || opening || operation || opening_entry || watch) ReleaseDirectory();
        for (auto& subscription : subscriptions) if (subscription) { model.unsubscribe(model.context, subscription); subscription = 0; }
        if (panel) { model.close(model.context, panel); panel = 0; }
        ui_request = 0;
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
