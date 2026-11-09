#include <pulse/sdk.hpp>
#include <pulse/model_builder.hpp>
#include <pulse/fileops.h>
#include <new>
#include <string>
#include <vector>

namespace {
using pulse::sdk::View;
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
    PulseFileOpsV1 fileops{};
    uint64_t panel = 0;
    uint64_t subscriptions[5]{};
    PulseFileOperationRequestId request = 0;
    bool active = false;
    std::string status = "idle", task = "0", progress = "Select files in the Pulse window, then choose an operation.";
    explicit Plugin(const PulseHostV1& value) : host(value) {}

    PulseStatus Render(bool create) {
        using pulse::sdk::ModelNode;
        using N = PulseUiModelNode;
        std::vector<ModelNode> content;
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "description").String(&N::text, "Pulse owns confirmation and file execution. This DLL chooses actions and displays its own request status."));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "copy-selection").String(&N::text, "Copy selection to other pane").String(&N::command, "copy-selection").Bool(&N::enabled, !(request != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "move-selection").String(&N::text, "Move selection to other pane").String(&N::command, "move-selection").Bool(&N::enabled, !(request != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "recycle-selection").String(&N::text, "Move selection to Recycle Bin").String(&N::command, "recycle-selection").Bool(&N::enabled, !(request != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "refresh-status").String(&N::text, "Refresh own request status").String(&N::command, "refresh-status").Bool(&N::enabled, !(request == 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "cancel-request").String(&N::text, "Cancel own request").String(&N::command, "cancel-request").Bool(&N::enabled, !(request == 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "status-label").String(&N::binding, "state.status"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "task-label").String(&N::binding, "state.task"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "progress-label").String(&N::binding, "state.progress"));
        pulse::sdk::ModelTree tree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "fileops-root").Children({ModelNode(PULSE_UI_MODEL_STACK, "fileops-content").Int(&N::gap, 12).Children(std::move(content))}));
        tree.Command("copy-selection");
        tree.Command("move-selection");
        tree.Command("recycle-selection");
        tree.Command("refresh-status");
        tree.Command("cancel-request");
        PulseUiState states[] = {{View("status"), View(status)}, {View("task"), View(task)}, {View("progress"), View(progress)}};
        for (const auto& state : states) tree.State(std::string(state.key.data, state.key.length), std::string(state.value.data, state.value.length));
        const auto result = create ? ui.create(ui.context, View("Native file operations"), &tree.Build(), &panel)
                                   : ui.update(ui.context, panel, &tree.Build());
        if (result != PULSE_OK || create) return result;
        return ui.set_states(ui.context, panel, states, 3);
    }
    void Submit(uint32_t operation) {
        if (request) return;
        const auto result = fileops.submit(fileops.context, operation, &request);
        status = result == PULSE_OK ? "preparing" : "rejected";
        task = "0";
        progress = result == PULSE_OK ? "Accepted for host preparation; no file completion is implied."
                                    : "Check selection, permissions and operation availability.";
        Render(false);
    }
    void Refresh() {
        if (!request) return;
        PulseFileOperation snapshot{}; snapshot.size = sizeof(snapshot);
        const auto result = fileops.get(fileops.context, request, &snapshot);
        if (result != PULSE_OK) { status = "unavailable"; progress = "Host no longer has this request."; request = 0; }
        else {
            status = StateName(snapshot.state);
            task = std::to_string(snapshot.task_id);
            progress = std::to_string(snapshot.completed_items) + " / " + std::to_string(snapshot.total_items) + " items";
            if (snapshot.terminal) request = 0;
        }
        Render(false);
    }
    static void PULSE_CALL Click(void* user, uint64_t owner_panel, PulseStringView id) noexcept {
        auto* self = static_cast<Plugin*>(user);
        if (!self->active || owner_panel != self->panel) return;
        try {
            const std::string name(id.data, id.length);
            if (name == "copy-selection") self->Submit(PULSE_FILEOPS_COPY_SELECTION_TO_OTHER_PANE);
            else if (name == "move-selection") self->Submit(PULSE_FILEOPS_MOVE_SELECTION_TO_OTHER_PANE);
            else if (name == "recycle-selection") self->Submit(PULSE_FILEOPS_DELETE_SELECTION_TO_RECYCLE_BIN);
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
        result = host.Query(PULSE_FILEOPS_INTERFACE_NAME, PULSE_FILEOPS_VERSION_1, fileops);
        if (result != PULSE_OK) return result;
        active = true;
        result = Render(true);
        if (result != PULSE_OK) return result;
        const char* ids[] = {"copy-selection", "move-selection", "recycle-selection", "refresh-status", "cancel-request"};
        for (size_t index = 0; index < 5; ++index) {
            result = ui.subscribe_command(ui.context, panel, View(ids[index]), Click, this, &subscriptions[index]);
            if (result != PULSE_OK) return result;
        }
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false;
        if (request) { fileops.cancel(fileops.context, request); request = 0; }
        for (auto& subscription : subscriptions) if (subscription) { ui.unsubscribe(ui.context, subscription); subscription = 0; }
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
