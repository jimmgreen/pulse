#include <pulse/sdk.hpp>
#include <pulse/model_builder.hpp>
#include <pulse/files.h>
#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace {
using pulse::sdk::View;
std::string Copy(PulseStringView text) { return text.length ? std::string(text.data, text.length) : std::string(); }
std::string Display(std::string text) {
    if (text.size() <= 900) return text;
    size_t end = 897;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text.resize(end); text += "..."; return text;
}
struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 ui{};
    PulseFilesV1 files{};
    uint64_t panel = 0;
    uint64_t subscriptions[4]{};
    PulseFilesRequestId pending = 0;
    uint64_t task = 0;
    bool active = false;
    std::string prefix, plan, output, status = "Enter a prefix, then preview the current folder.";
    std::vector<std::string> rows;
    explicit Plugin(const PulseHostV1& value) : host(value) {}

    PulseStatus Render(bool create) {
        using pulse::sdk::ModelNode;
        using N = PulseUiModelNode;
        std::vector<ModelNode> content;
#ifdef PULSE_SAMPLE_UPPER_CASE
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "native-rule").String(&N::text, "DLL algorithm: prefix + number + ASCII UPPERCASE filename"));
#else
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "native-rule").String(&N::text, "DLL algorithm: prefix + number + ASCII lowercase filename"));
#endif
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "prefix-input").String(&N::text, "Filename prefix").String(&N::state, "prefix").String(&N::placeholder, "e.g. reviewed-"));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "preview-button").String(&N::text, "Preview current folder").String(&N::command, "preview-button"));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "commit-button").String(&N::text, "Review and rename").String(&N::command, "commit-button").Bool(&N::enabled, !(plan.empty() || pending != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "status-button").String(&N::text, "Refresh task status").String(&N::command, "status-button").Bool(&N::enabled, !(task == 0 || pending != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_BUTTON, "native-cancel").String(&N::text, "Cancel own task").String(&N::command, "native-cancel").Bool(&N::enabled, !(task == 0 || pending != 0)));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "message-text").String(&N::binding, "state.status"));
        content.push_back(ModelNode(PULSE_UI_MODEL_TEXT, "output-text").String(&N::binding, "state.output"));
        if (!rows.empty()) content.push_back(ModelNode(PULSE_UI_MODEL_SELECT, "native-plan").String(&N::text, "Rename preview").String(&N::state, "selected").Options(rows).Int(&N::min_height, 120));
        pulse::sdk::ModelTree tree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "native-root").Children({ModelNode(PULSE_UI_MODEL_STACK, "native-content").Int(&N::gap, 12).Children(std::move(content))}));
        tree.Command("preview-button");
        tree.Command("commit-button");
        tree.Command("status-button");
        tree.Command("native-cancel");
        PulseUiState states[] = {{View("prefix"), View("")}, {View("status"), View(status)}, {View("selected"), View("0")}, {View("output"), View(output)}};
        for (const auto& state : states) tree.State(std::string(state.key.data, state.key.length), std::string(state.value.data, state.value.length));
        const auto result = create ? ui.create(ui.context, View("Developer rename example"), &tree.Build(), &panel) : ui.update(ui.context, panel, &tree.Build());
        if (result == PULSE_OK && !create) {
            // update preserves existing states; reset selection for changed rows.
            PulseUiState updates[] = {{View("status"), View(status)}, {View("selected"), View("0")}, {View("output"), View(output)}};
            return ui.set_states(ui.context, panel, updates, 3);
        }
        return result;
    }
    void Fail(const char* text) noexcept {
        if (!active || !panel) return;
        const PulseUiState value{View("status"), View(text)};
        ui.set_states(ui.context, panel, &value, 1);
    }
    void CancelPending() noexcept {
        if (pending) { files.cancel_request(files.context, pending); pending = 0; }
    }
    void Accepted(PulseStatus result) {
        if (result != PULSE_OK) { pending = 0; status = "The host rejected this request. Check permissions and folder availability."; }
        Render(false);
    }
    void Preview() {
        CancelPending(); plan.clear(); rows.clear(); output.clear();
        char text[1024]{}; uint32_t length = 0;
        if (ui.get_state(ui.context, panel, View("prefix"), text, sizeof(text), &length) != PULSE_OK) { Fail("Cannot read the prefix."); return; }
        prefix.assign(text, length);
        status = "Reading current folder...";
        Accepted(files.list(files.context, View(""), 64, Complete, this, &pending));
    }
    void List(const PulseFilePage& page) {
        if (page.next_cursor.length) { status = "This example handles one page of at most 64 entries. Use a smaller folder."; Render(false); return; }
        std::vector<std::string> entries, names;
        for (uint32_t i = 0; i < page.count; ++i) {
            if (page.entries[i].flags & (PULSE_FILE_DIRECTORY | PULSE_FILE_REPARSE_POINT)) continue;
            entries.push_back(Copy(page.entries[i].entry));
            std::string name = Copy(page.entries[i].name);
            for (char& c : name) {
#ifdef PULSE_SAMPLE_UPPER_CASE
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
#else
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
            }
            names.push_back(prefix + std::to_string(names.size() + 1) + "-" + name);
        }
        if (entries.empty()) { status = "No regular files on this page."; Render(false); return; }
        std::vector<PulseRenameInput> inputs;
        for (size_t i = 0; i < entries.size(); ++i) inputs.push_back({View(entries[i]), View(names[i])});
        status = "Host is validating the DLL-generated names...";
        Accepted(files.prepare_rename(files.context, inputs.data(), static_cast<uint32_t>(inputs.size()), Complete, this, &pending));
    }
    void Reply(const PulseFilesReply& reply) {
        if (!active || reply.request_id != pending) return;
        pending = 0;
        if (reply.status != PULSE_OK) {
            status = "Host request failed: " + Copy(reply.error); plan.clear(); Render(false); return;
        }
        if (reply.operation == PULSE_FILES_LIST) { List(reply.page); return; }
        if (reply.operation == PULSE_FILES_PREPARE_RENAME) {
            plan = Copy(reply.plan.plan_id); rows.clear();
            for (uint32_t i = 0; i < reply.plan.count; ++i) rows.push_back(Display(Copy(reply.plan.rows[i].original_name) + " -> " + Copy(reply.plan.rows[i].new_leaf_name)));
            output = rows.empty() ? "" : rows.front();
            status = "Preview ready. Review and rename opens the host confirmation; plans expire after 60 seconds.";
        } else if (reply.operation == PULSE_FILES_COMMIT_RENAME) {
            task = reply.task.task_id; plan.clear(); status = "Task submitted. Refresh status to check actual completion.";
        } else if (reply.operation == PULSE_FILES_TASK_GET || reply.operation == PULSE_FILES_TASK_CANCEL) {
            status = reply.task.terminal ? (reply.task.completed ? "Task completed." : "Task ended without full completion.") : "Task is still running.";
            if (reply.operation == PULSE_FILES_TASK_CANCEL && reply.cancel_accepted) status = "Cancellation requested. Refresh status for the final result.";
        }
        Render(false);
    }
    static void PULSE_CALL Complete(void* user, const PulseFilesReply* reply) noexcept {
        auto* self = static_cast<Plugin*>(user);
        try { if (reply) self->Reply(*reply); } catch (...) { self->Fail("The DLL could not process the host reply."); }
    }
    static void PULSE_CALL Click(void* user, uint64_t owner_panel, PulseStringView id) noexcept {
        auto* self = static_cast<Plugin*>(user);
        if (!self->active || owner_panel != self->panel) return;
        try {
            const auto name = Copy(id);
            if (name == "preview-button") self->Preview();
            else if (name == "commit-button" && !self->pending && !self->plan.empty()) {
                self->status = "Waiting for native host confirmation...";
                self->Accepted(self->files.commit_rename(self->files.context, View(self->plan), Complete, self, &self->pending));
            } else if (name == "status-button" && !self->pending && self->task) {
                self->Accepted(self->files.task_get(self->files.context, self->task, Complete, self, &self->pending));
            } else if (name == "native-cancel" && !self->pending && self->task) {
                self->Accepted(self->files.task_cancel(self->files.context, self->task, Complete, self, &self->pending));
            }
        } catch (...) { self->Fail("The DLL could not process this action."); }
    }
    PulseStatus Start() {
        auto result = host.Query(PULSE_UI_MODEL_INTERFACE, PULSE_UI_MODEL_VERSION, ui);
        if (result != PULSE_OK) return result;
        result = host.Query(PULSE_FILES_INTERFACE_NAME, PULSE_FILES_VERSION_1, files);
        if (result != PULSE_OK) return result;
        active = true;
        result = Render(true);
        if (result != PULSE_OK) return result;
        const char* ids[] = {"preview-button", "commit-button", "status-button", "native-cancel"};
        for (size_t i = 0; i < 4; ++i) {
            result = ui.subscribe_command(ui.context, panel, View(ids[i]), Click, this, &subscriptions[i]);
            if (result != PULSE_OK) return result;
        }
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false; CancelPending();
        for (auto& subscription : subscriptions) if (subscription) { ui.unsubscribe(ui.context, subscription); subscription = 0; }
        if (panel) { ui.close(ui.context, panel); panel = 0; }
    }
};
PulseStatus PULSE_CALL Activate(void* p) noexcept { try { return static_cast<Plugin*>(p)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* p) noexcept { static_cast<Plugin*>(p)->Stop(); }
void PULSE_CALL Destroy(void* p) noexcept { auto* self = static_cast<Plugin*>(p); self->Stop(); delete self; }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) || host->version != 1 || out->version != 1) return PULSE_ERROR_ABI;
    try {
        auto* instance = new Plugin(*host);
        *out = {sizeof(*out), 1, instance, Activate, Deactivate, Destroy};
        return PULSE_OK;
    } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
