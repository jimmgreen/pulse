#include <pulse/sdk.hpp>
#include <pulse/model_builder.hpp>
#include <pulse/directories.h>
#include <pulse/fileops.h>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using pulse::sdk::View;
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
std::string Clip(std::string value, size_t limit = 1000) {
    if (value.size() <= limit) return value;
    while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80) --limit;
    value.resize(limit); return value;
}
const char* StateName(uint32_t state) {
    switch (state) {
    case PULSE_FILEOPS_PREPARING: return "preparing";
    case PULSE_FILEOPS_WAITING_CONFIRMATION: return "waiting";
    case PULSE_FILEOPS_QUEUED: return "queued";
    case PULSE_FILEOPS_RUNNING: return "running";
    case PULSE_FILEOPS_CANCELLING: return "cancelling";
    case PULSE_FILEOPS_COMPLETED: return "completed";
    case PULSE_FILEOPS_PARTIAL: return "partial";
    case PULSE_FILEOPS_CANCELLED: return "cancelled";
    case PULSE_FILEOPS_FAILED: return "failed";
    default: return "unknown";
    }
}
struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 ui{};
    PulseDirectoriesV1 directories{};
    PulseFileOpsV4 fileops{};
    uint64_t panel = 0;
    std::array<uint64_t, 12> subscriptions{};
    PulseDirectoryId directory = 0, source_directory = 0;
    PulseDirectoryRequestId opening = 0, listing = 0;
    PulseFileOperationRequestId request = 0;
    uint64_t source_snapshot = 0;
    std::unordered_map<std::string, uint64_t> source_entries;
    std::unordered_map<std::string, uint64_t> source_folders;
    bool tree_request = false;
    bool opening_source = false;
    bool active = false;
    std::string status = "idle", directory_text = "Directory: closed", source_text = "Source: closed", task = "0";
    std::string progress = "Open source and target directories, list the source snapshot, then submit a file or directory entry.";
    explicit Plugin(const PulseHostV1& value) : host(value) {}

    PulseStatus Render(bool create) {
        using pulse::sdk::ModelNode;
        using N = PulseUiModelNode;
        std::vector<ModelNode> content;
        std::vector<ModelNode> actions;
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "description").String(&N::text, "Transfer current-snapshot file or directory entries through Pulse's confirmed file service."));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "source-input").String(&N::text, "Source directory").String(&N::state, "source").String(&N::placeholder, "Absolute source directory path"));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "open-source").String(&N::text, "Open and list source").String(&N::command, "open-source").Bool(&N::enabled, !(request != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "refresh-source").String(&N::text, "Refresh source snapshot").String(&N::command, "refresh-source").Bool(&N::enabled, !(!source_directory || request != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "close-source").String(&N::text, "Close source").String(&N::command, "close-source").Bool(&N::enabled, !(!source_directory && !opening_source)));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "source-label").String(&N::binding, "state.sourceLabel"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "entry-input").String(&N::text, "Snapshot entry name").String(&N::state, "entryName").String(&N::placeholder, "Exact file or directory name from current snapshot"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "destination-input").String(&N::text, "Target directory").String(&N::state, "destination").String(&N::placeholder, "Absolute target directory path"));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "open-target").String(&N::text, "Open target").String(&N::command, "open-target").Bool(&N::enabled, !(request != 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "close-target").String(&N::text, "Close target").String(&N::command, "close-target").Bool(&N::enabled, !(directory == 0 && opening == 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "copy-selection").String(&N::text, "Copy current Pulse selection").String(&N::command, "copy-selection").Bool(&N::enabled, !(directory == 0 || request != 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "move-selection").String(&N::text, "Move current Pulse selection").String(&N::command, "move-selection").Bool(&N::enabled, !(directory == 0 || request != 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "copy-entry").String(&N::text, "Copy snapshot entry").String(&N::command, "copy-entry").Bool(&N::enabled, !(!source_directory || !directory || request != 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "move-entry").String(&N::text, "Move snapshot entry").String(&N::command, "move-entry").Bool(&N::enabled, !(!source_directory || !directory || request != 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "copy-directory-tree").String(&N::text, "Copy directory tree").String(&N::command, "copy-directory-tree").Bool(&N::enabled, !(!source_directory || !directory || request != 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "refresh-status").String(&N::text, "Refresh status").String(&N::command, "refresh-status").Bool(&N::enabled, !(request == 0)));
        actions.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "cancel-request").String(&N::text, "Cancel request").String(&N::command, "cancel-request").Bool(&N::enabled, !(request == 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_GRID, "target-actions").Int(&N::columns, 2).Int(&N::gap, 8).Children(std::move(actions)));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "status-label").String(&N::binding, "state.status"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "directory-label").String(&N::binding, "state.directory"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "task-label").String(&N::binding, "state.task"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "progress-label").String(&N::binding, "state.progress"));
        pulse::sdk::ModelTree tree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "target-root").Children({ModelNode(PULSE_UI_MODEL_STACK, "target-content").Int(&N::gap, 12).Children(std::move(content))}));
        tree.Command("open-source");
        tree.Command("refresh-source");
        tree.Command("close-source");
        tree.Command("open-target");
        tree.Command("close-target");
        tree.Command("copy-selection");
        tree.Command("move-selection");
        tree.Command("copy-entry");
        tree.Command("move-entry");
        tree.Command("copy-directory-tree");
        tree.Command("refresh-status");
        tree.Command("cancel-request");
        PulseUiState states[] = {{View("source"), View("")}, {View("entryName"), View("")},
            {View("destination"), View("")}, {View("status"), View(status)},
            {View("directory"), View(directory_text)}, {View("sourceLabel"), View(source_text)},
            {View("task"), View(task)}, {View("progress"), View(progress)}};
        for (const auto& state : states) tree.State(std::string(state.key.data, state.key.length), std::string(state.value.data, state.value.length));
        const auto result = create ? ui.create(ui.context, View("Native directory transfer"), &tree.Build(), &panel)
                                   : ui.update(ui.context, panel, &tree.Build());
        if (result != PULSE_OK || create) return result;
        return ui.set_states(ui.context, panel, states + 3, 5);
    }
    std::string ReadState(std::string_view key) {
        uint32_t needed = 0;
        if (ui.get_state(ui.context, panel, View(key), nullptr, 0, &needed) != PULSE_OK || needed > 1024) return {};
        std::vector<char> text(static_cast<size_t>(needed) + 1);
        if (ui.get_state(ui.context, panel, View(key), text.data(), static_cast<uint32_t>(text.size()), &needed) != PULSE_OK) return {};
        return std::string(text.data(), needed);
    }
    std::string ReadDestination() {
        uint32_t needed = 0;
        if (ui.get_state(ui.context, panel, View("destination"), nullptr, 0, &needed) != PULSE_OK || needed > 1024) return {};
        std::vector<char> text(static_cast<size_t>(needed) + 1);
        if (ui.get_state(ui.context, panel, View("destination"), text.data(), static_cast<uint32_t>(text.size()), &needed) != PULSE_OK) return {};
        return std::string(text.data(), needed);
    }
    void ReleaseTarget() noexcept {
        if (opening) { const auto old = opening; opening = 0; directories.cancel(directories.context, old); }
        if (directory) { const auto old = directory; directory = 0; directories.close(directories.context, old); }
    }
    void ReleaseSource() noexcept {
        if (listing) { const auto old = listing; listing = 0; directories.cancel(directories.context, old); }
        if (opening) { const auto old = opening; opening = 0; directories.cancel(directories.context, old); }
        if (source_directory) { const auto old = source_directory; source_directory = 0; directories.close(directories.context, old); }
        source_snapshot = 0; source_entries.clear(); source_folders.clear();
    }
    void OpenSource() {
        if (request) return;
        const auto path = ReadState("source");
        ReleaseSource(); opening_source = true;
        if (path.empty()) { status = "source-error"; source_text = "Source: closed"; progress = "Enter an absolute source directory path."; Render(false); return; }
        status = "source-opening"; source_text = Clip("Opening: " + path);
        progress = "Opening source directory on the host worker...";
        const auto result = directories.open(directories.context, View(path), Opened, this, &opening);
        if (result != PULSE_OK) { opening = 0; status = "source-error"; source_text = "Source: closed"; }
        Render(false);
    }
    void RefreshSource() {
        if (!source_directory || request) return;
        listing = 0;
        status = "source-refreshing";
        const auto result = directories.refresh(directories.context, source_directory, Refreshed, this, &listing);
        if (result != PULSE_OK) { listing = 0; status = "source-error"; progress = "Source refresh was rejected."; }
        Render(false);
    }
    void CloseSource() {
        ReleaseSource(); source_text = "Source: closed";
        if (!request) { status = "source-closed"; progress = "Source directory and snapshot closed."; }
        Render(false);
    }
    void OpenTarget() {
        if (request) return;
        const auto path = ReadDestination();
        ReleaseTarget();
        directory_text = "Directory: closed";
        if (path.empty()) {
            status = "target-error"; progress = "Enter an absolute target directory path."; Render(false); return;
        }
        status = "target-opening"; directory_text = Clip("Opening: " + path);
        progress = "Opening the target on the host worker...";
        const auto result = directories.open(directories.context, View(path), Opened, this, &opening);
        if (result != PULSE_OK) {
            opening = 0; status = "target-error"; directory_text = "Directory: closed";
            progress = "Target open request rejected; check the path and permissions.";
        }
        Render(false);
    }
    void CloseTarget() {
        ReleaseTarget(); directory_text = "Directory: closed";
        if (!request) { status = "target-closed"; progress = "Target directory closed."; }
        else progress = "Target closed. Refresh or cancel the existing request; accepted tasks keep running.";
        Render(false);
    }
    void Submit(uint32_t operation) {
        if (request || !directory) return;
        const auto result = fileops.submit_to_directory(fileops.context, operation, directory, &request);
        status = result == PULSE_OK ? "preparing" : "rejected"; task = "0";
        progress = result == PULSE_OK ? "Accepted for host preparation; confirmation and file completion are still pending."
                                    : "Check selection, target, permissions and operation availability.";
        Render(false);
    }
    void SubmitEntry(uint32_t operation) {
        if (request || !source_directory || !directory || !source_snapshot) return;
        const auto name = ReadState("entryName");
        const auto found = source_entries.find(name);
        if (found == source_entries.end()) { status = "rejected"; progress = "Name is not an ordinary file in the current source snapshot."; Render(false); return; }
        const uint64_t entry = found->second;
        const auto result = fileops.submit_entries(fileops.context, operation, source_directory,
            source_snapshot, &entry, 1, directory, &request);
        status = result == PULSE_OK ? "preparing" : "rejected"; task = "0";
        progress = result == PULSE_OK ? "Snapshot entry accepted for preparation and native confirmation."
                                      : "Check source snapshot, destination, permissions and entry type.";
        Render(false);
    }
    void SubmitDirectoryTree() {
        if (request || !source_directory || !directory || !source_snapshot) return;
        const auto name = ReadState("entryName");
        const auto found = source_folders.find(name);
        if (found == source_folders.end()) { status = "rejected"; progress = "Name is not an ordinary directory in the current source snapshot."; Render(false); return; }
        const auto result = fileops.submit_directory_tree(fileops.context, source_directory,
            source_snapshot, found->second, directory, &request);
        tree_request = result == PULSE_OK;
        status = result == PULSE_OK ? "preparing" : "rejected"; task = "0";
        progress = result == PULSE_OK ? "Directory tree accepted for frozen preparation and native confirmation."
                                      : "Check the current snapshot, target, permissions and directory type.";
        Render(false);
    }
    void Refresh() {
        if (!request) return;
        PulseFileOperation snapshot{}; snapshot.size = sizeof(snapshot);
        const auto result = fileops.get(fileops.context, request, &snapshot);
        if (result != PULSE_OK) { status = "unavailable"; progress = "Host no longer has this request."; request = 0; }
        else {
            status = StateName(snapshot.state); task = std::to_string(snapshot.task_id);
            progress = std::to_string(snapshot.completed_items) + " / " + std::to_string(snapshot.total_items) + " items";
            if (snapshot.terminal && tree_request) {
                std::string details;
                for (uint32_t index = 0; index < snapshot.total_items; ++index) {
                    PulseFileOperationItem item{}; item.size = sizeof(item);
                    const auto sized = fileops.get_item(fileops.context, request, index, nullptr, 0, &item);
                    if (sized != PULSE_ERROR_ARGUMENT || !item.path_bytes || item.path_bytes > 32768) {
                        details += "\nresult unavailable at item " + std::to_string(index); break;
                    }
                    std::vector<char> path(item.path_bytes);
                    item.size = sizeof(item);
                    if (fileops.get_item(fileops.context, request, index, path.data(), static_cast<uint32_t>(path.size()), &item) != PULSE_OK) {
                        details += "\nresult unavailable at item " + std::to_string(index); break;
                    }
                    details += "\n" + std::string(path.data()) + ": " +
                        (item.state == PULSE_FILEOPS_ITEM_COMPLETED ? "completed" : "failed") +
                        " (" + std::to_string(item.transferred_bytes) + " bytes)";
                }
                progress += details;
                request = 0; tree_request = false;
            } else if (snapshot.terminal) request = 0;
        }
        Render(false);
    }
    static void PULSE_CALL Opened(void* user, const PulseDirectoryReply* reply) noexcept {
        auto* self = static_cast<Plugin*>(user);
        if (!self->active || !reply || reply->request_id != self->opening) return;
        self->opening = 0;
        try {
            if (reply->status == PULSE_OK && reply->directory_id) {
                if (self->opening_source) {
                    self->source_directory = reply->directory_id; self->source_snapshot = reply->snapshot_id;
                    self->source_text = Clip("Source: " + Copy(reply->path)); self->status = "source-listing";
                    self->listing = 0;
                    const auto listed = self->directories.list(self->directories.context, self->source_directory,
                        self->source_snapshot, 0, 64, Listed, self, &self->listing);
                    if (listed != PULSE_OK) { self->status = "source-error"; self->progress = "Could not request the source snapshot page."; }
                    else self->progress = "Reading the first 64 rows of the source snapshot...";
                } else {
                    self->directory = reply->directory_id; self->status = "target-ready";
                    self->directory_text = Clip("Directory: " + Copy(reply->path));
                    self->progress = "Target ready. Choose a file name from the source snapshot.";
                }
            } else {
                self->status = self->opening_source ? "source-error" : "target-error";
                if (self->opening_source) self->source_text = "Source: closed"; else self->directory_text = "Directory: closed";
                self->progress = Clip("Directory open failed: " + Copy(reply->error));
            }
            self->Render(false);
        } catch (...) {
            const PulseUiState failure{View("status"), View("error")};
            self->ui.set_states(self->ui.context, self->panel, &failure, 1);
        }
    }
    static void PULSE_CALL Listed(void* user, const PulseDirectoryReply* reply) noexcept {
        auto* self = static_cast<Plugin*>(user);
        if (!self->active || !reply || reply->request_id != self->listing) return;
        self->listing = 0;
        try {
            self->source_entries.clear();
            if (reply->status == PULSE_OK && reply->directory_id == self->source_directory &&
                reply->snapshot_id == self->source_snapshot && reply->next_offset == UINT32_MAX && reply->total <= 64) {
                for (uint32_t i = 0; i < reply->count; ++i) {
                    const auto& row = reply->rows[i];
                    if ((row.flags & PULSE_DIRECTORY_ENTRY_FOLDER) && !(row.flags & PULSE_DIRECTORY_ENTRY_REPARSE))
                        self->source_folders.emplace(Copy(row.name), row.entry_id);
                    else if (!(row.flags & PULSE_DIRECTORY_ENTRY_REPARSE))
                        self->source_entries.emplace(Copy(row.name), row.entry_id);
                }
                self->status = "source-ready";
                self->progress = "Current snapshot ready: " + std::to_string(self->source_entries.size()) + " ordinary files.";
            } else {
                self->status = "source-error";
                self->progress = "Snapshot must fit in one page of at most 64 entries; reopen or refresh the source.";
            }
            self->Render(false);
        } catch (...) { self->status = "source-error"; self->Render(false); }
    }
    static void PULSE_CALL Refreshed(void* user, const PulseDirectoryReply* reply) noexcept {
        auto* self = static_cast<Plugin*>(user);
        if (!self->active || !reply || reply->request_id != self->listing) return;
        self->listing = 0;
        self->status = reply->status == PULSE_OK ? "source-refreshed" : "source-error";
        self->progress = reply->status == PULSE_OK
            ? "The source now has a newer snapshot; reopen and list it before reusing entry IDs."
            : Clip("Source refresh failed: " + Copy(reply->error));
        self->Render(false);
    }
    static void PULSE_CALL Click(void* user, uint64_t owner_panel, PulseStringView id) noexcept {
        auto* self = static_cast<Plugin*>(user);
        if (!self->active || owner_panel != self->panel) return;
        try {
            const auto name = Copy(id);
            if (name == "open-source") self->OpenSource();
            else if (name == "refresh-source") self->RefreshSource();
            else if (name == "close-source") self->CloseSource();
            else if (name == "open-target") { self->opening_source = false; self->OpenTarget(); }
            else if (name == "close-target") self->CloseTarget();
            else if (name == "copy-selection") self->Submit(PULSE_FILEOPS_COPY_SELECTION_TO_DIRECTORY);
            else if (name == "move-selection") self->Submit(PULSE_FILEOPS_MOVE_SELECTION_TO_DIRECTORY);
            else if (name == "copy-entry") self->SubmitEntry(PULSE_FILEOPS_COPY_ENTRIES_TO_DIRECTORY);
            else if (name == "move-entry") self->SubmitEntry(PULSE_FILEOPS_MOVE_ENTRIES_TO_DIRECTORY);
            else if (name == "copy-directory-tree") self->SubmitDirectoryTree();
            else if (name == "refresh-status") self->Refresh();
            else if (name == "cancel-request" && self->request) {
                const auto result = self->fileops.cancel(self->fileops.context, self->request);
                self->progress = result == PULSE_OK ? "Cancellation requested; refresh for the terminal result."
                                                   : "Request already ended or cancellation is unavailable.";
                self->Render(false);
            }
        } catch (...) {
            const PulseUiState failure{View("status"), View("error")};
            self->ui.set_states(self->ui.context, self->panel, &failure, 1);
        }
    }
    PulseStatus Start() {
        auto result = host.Query(PULSE_UI_MODEL_INTERFACE, PULSE_UI_MODEL_VERSION, ui);
        if (result != PULSE_OK) return result;
        result = host.Query(PULSE_DIRECTORIES_INTERFACE_NAME, PULSE_DIRECTORIES_VERSION_1, directories);
        if (result != PULSE_OK) return result;
        result = host.Query(PULSE_FILEOPS_INTERFACE_NAME, PULSE_FILEOPS_VERSION_4, fileops);
        if (result != PULSE_OK) return result;
        active = true; result = Render(true);
        if (result != PULSE_OK) return result;
        const char* ids[] = {"open-source", "refresh-source", "close-source", "open-target", "close-target",
            "copy-selection", "move-selection", "copy-entry", "move-entry", "copy-directory-tree", "refresh-status", "cancel-request"};
        for (size_t index = 0; index < subscriptions.size(); ++index) {
            result = ui.subscribe_command(ui.context, panel, View(ids[index]), Click, this, &subscriptions[index]);
            if (result != PULSE_OK) return result;
        }
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false;
        for (auto& subscription : subscriptions) if (subscription) { ui.unsubscribe(ui.context, subscription); subscription = 0; }
        if (request) { const auto old = request; request = 0; fileops.cancel(fileops.context, old); }
        ReleaseTarget(); ReleaseSource();
        if (panel) { ui.close(ui.context, panel); panel = 0; }
    }
};
PulseStatus PULSE_CALL Activate(void* value) noexcept { try { return static_cast<Plugin*>(value)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* value) noexcept { static_cast<Plugin*>(value)->Stop(); }
void PULSE_CALL Destroy(void* value) noexcept { auto* plugin = static_cast<Plugin*>(value); plugin->Stop(); delete plugin; }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) ||
        host->version != PULSE_ABI_VERSION_1 || out->version != PULSE_ABI_VERSION_1) return PULSE_ERROR_ABI;
    try {
        auto* plugin = new Plugin(*host);
        *out = {sizeof(*out), PULSE_ABI_VERSION_1, plugin, Activate, Deactivate, Destroy};
        return PULSE_OK;
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
