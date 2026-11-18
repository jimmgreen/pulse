#include <pulse/sdk.hpp>
#include <pulse/ui_model.h>
#include <pulse/files.h>
#include <pulse/resources.h>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string>
#include <vector>
namespace {
using pulse::sdk::View;
PulseUiModelString Text(std::string_view value) { return {1, View(value)}; }
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string(); }
const char* commands[] = {"analyze-button", "stat-button", "load-settings-button", "save-settings-button", "audit-button"};
struct Bucket { std::string extension; uint64_t count = 0, bytes = 0; };
struct Plugin {
    pulse::sdk::Host host;
    PulseUiModelV1 model{};
    PulseFilesV1 files{};
    PulseResourcesV1 resources{};
    uint64_t panel = 0, subscriptions[5]{}, settings_version = 0;
    PulseFilesRequestId file_request = 0;
    PulseResourcesRequestId resource_request = 0;
    bool active = false, settings_loaded = false;
    uint64_t files_seen = 0, entries_seen = 0;
    std::map<std::string, Bucket> buckets;
    std::vector<Bucket> result;
    std::string first_entry, status = "Ready. Analyze the current folder.", option = "bytes";
    explicit Plugin(const PulseHostV1& h) : host(h) {}
    void Message(const char* text) noexcept {
        if (!active || !panel) return;
        PulseUiState state{View("message"), View(text)};
        model.set_states(model.context, panel, &state, 1);
    }
    void Show() { Message(status.c_str()); }
    void Cancel() noexcept {
        if (file_request) { files.cancel_request(files.context, file_request); file_request = 0; }
        if (resource_request) { resources.cancel_request(resources.context, resource_request); resource_request = 0; }
    }
    bool ReadOption() {
        char buffer[1024]{}; uint32_t needed = 0;
        if (model.get_state(model.context, panel, View("option"), buffer, sizeof(buffer), &needed) != PULSE_OK) { Message("Cannot read the display option."); return false; }
        option.assign(buffer, needed);
        if (option != "bytes" && option != "count") { Message("Display option must be bytes or count."); return false; }
        return true;
    }
    PulseStatus Render(bool create) {
        std::vector<PulseUiModelNode> children;
        children.reserve(10);
        const auto node = [&](uint32_t type, const char* id, const char* label) -> PulseUiModelNode& {
            PulseUiModelNode n{}; n.size = sizeof(n); n.type = type; n.id = View(id); n.text = Text(label); children.push_back(n); return children.back();
        };
        auto& input = node(PULSE_UI_MODEL_TEXT_INPUT, "option-input", "Sort totals by bytes or count");
        input.state = Text("option"); input.placeholder = Text("bytes or count");
        const char* labels[] = {"Analyze directory", "Stat first file", "Load my settings", "Save my settings", "Read my audit"};
        for (size_t i = 0; i < 5; ++i) node(PULSE_UI_MODEL_BUTTON, commands[i], labels[i]).command = Text(commands[i]);
        node(PULSE_UI_MODEL_TEXT, "message-text", "").binding = Text("state.message");
        std::vector<std::array<std::string, 3>> values;
        for (const auto& item : result) values.push_back({item.extension, std::to_string(item.count), std::to_string(item.bytes)});
        std::vector<std::array<PulseStringView, 3>> cells;
        for (const auto& value : values) cells.push_back({View(value[0]), View(value[1]), View(value[2])});
        std::vector<std::string> ids;
        for (size_t i = 0; i < result.size(); ++i) ids.push_back("extension-" + std::to_string(i));
        std::vector<PulseUiModelRow> rows;
        for (size_t i = 0; i < result.size(); ++i) {
            PulseUiModelRow row{}; row.size = sizeof(row); row.id = View(ids[i]); row.text = Text(values[i][0]);
            row.cells = {1, cells[i].data(), 3}; rows.push_back(row);
        }
        PulseUiModelColumn columns[3]{};
        const char* column_ids[] = {"extension", "count", "bytes"};
        const char* titles[] = {"Extension", "Files", "Bytes"};
        for (size_t i = 0; i < 3; ++i) { columns[i].size = sizeof(columns[i]); columns[i].id = View(column_ids[i]); columns[i].text = View(titles[i]); columns[i].width = {1, i == 0 ? 180 : 110}; }
        auto& table = node(PULSE_UI_MODEL_DATA_TABLE, "result-table", "Extension totals");
        table.state = Text("selected"); table.rows = {1, rows.data(), static_cast<uint32_t>(rows.size())};
        table.data_columns = {1, columns, 3}; table.min_height = {1, 220};
        PulseUiModelNode stack{}; stack.size = sizeof(stack); stack.type = PULSE_UI_MODEL_STACK; stack.id = View("inspector-content");
        stack.gap = {1, 10}; stack.children = {1, children.data(), static_cast<uint32_t>(children.size())};
        PulseUiModelNode root{}; root.size = sizeof(root); root.type = PULSE_UI_MODEL_SCROLL_VIEW; root.id = View("inspector-root"); root.children = {1, &stack, 1};
        PulseUiState states[] = {{View("option"), View(option)}, {View("message"), View(status)}, {View("selected"), View("")}};
        PulseStringView command_views[5]; for (size_t i = 0; i < 5; ++i) command_views[i] = View(commands[i]);
        PulseUiModelTree tree{sizeof(tree), &root, states, 3, nullptr, 0, command_views, 5};
        if (!create) { const PulseUiState selection{View("selected"), View("")}; model.set_states(model.context, panel, &selection, 1); }
        auto code = create ? model.create(model.context, View("Developer directory inspector"), &tree, &panel) : model.update(model.context, panel, &tree);
        if (code == PULSE_OK && !create) Show();
        return code;
    }
    void FileAccepted(PulseStatus code) { if (code != PULSE_OK) { file_request = 0; status = "Directory request rejected."; } Show(); }
    void ResourceAccepted(PulseStatus code) { if (code != PULSE_OK) { resource_request = 0; status = "Resource request rejected."; } Show(); }
    void Analyze() {
        if (!ReadOption()) return;
        Cancel(); buckets.clear(); result.clear(); first_entry.clear(); files_seen = entries_seen = 0;
        status = "Analyzing directory...";
        FileAccepted(files.list(files.context, View(""), 256, FileReply, this, &file_request));
    }
    void Page(const PulseFilesReply& reply) {
        if (!active || reply.request_id != file_request) return;
        file_request = 0;
        if (reply.status != PULSE_OK) { status = "Analysis failed: " + Copy(reply.error); Show(); return; }
        entries_seen += reply.page.count;
        if (entries_seen > 4096) { status = "Analysis failed: example entry budget exceeded."; Show(); return; }
        for (uint32_t i = 0; i < reply.page.count; ++i) {
            const auto& file = reply.page.entries[i];
            if (file.flags & (PULSE_FILE_DIRECTORY | PULSE_FILE_REPARSE_POINT)) continue;
            if (first_entry.empty()) first_entry = Copy(file.entry);
            const auto name = Copy(file.name); const auto dot = name.find_last_of('.');
            std::string extension = dot == std::string::npos || dot == 0 ? "(no extension)" : name.substr(dot);
            for (char& c : extension) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            auto& bucket = buckets[extension]; bucket.extension = extension;
            if (file.size_bytes > (std::numeric_limits<uint64_t>::max)() - bucket.bytes) { status = "Analysis failed: byte total overflow."; Show(); return; }
            ++bucket.count; bucket.bytes += file.size_bytes; ++files_seen;
        }
        const auto cursor = Copy(reply.page.next_cursor);
        if (!cursor.empty()) { FileAccepted(files.list(files.context, View(cursor), 256, FileReply, this, &file_request)); return; }
        for (const auto& pair : buckets) result.push_back(pair.second);
        std::sort(result.begin(), result.end(), [&](const Bucket& a, const Bucket& b) {
            const auto av = option == "bytes" ? a.bytes : a.count, bv = option == "bytes" ? b.bytes : b.count;
            return av == bv ? a.extension < b.extension : av > bv;
        });
        status = "Analysis complete: " + std::to_string(files_seen) + " files, " + std::to_string(result.size()) + " extensions.";
        if (Render(false) != PULSE_OK) Message("Analysis computed, but the host rejected the result table.");
    }
    void Resource(const PulseResourcesReply& reply) {
        if (!active || reply.request_id != resource_request) return;
        resource_request = 0;
        if (reply.status != PULSE_OK) {
            const auto error = Copy(reply.error);
            if (error == "version_conflict") { settings_loaded = false; status = "Settings conflict: reload before saving; nothing overwritten."; }
            else status = "Resource failed: " + error;
            Show(); return;
        }
        if (reply.operation == PULSE_RESOURCES_STAT) status = "Stat complete: " + std::to_string(reply.file_stat.size_bytes) + " bytes.";
        else if (reply.operation == PULSE_RESOURCES_AUDIT_LIST) status = "Audit complete: " + std::to_string(reply.audit.count) + " own records.";
        else if (reply.operation == PULSE_RESOURCES_SETTINGS_GET || reply.operation == PULSE_RESOURCES_SETTINGS_SET) {
            settings_version = reply.settings.version; settings_loaded = true;
            option = "bytes";
            for (uint32_t i = 0; i < reply.settings.count; ++i) if (Copy(reply.settings.values[i].key) == "display.sort") option = Copy(reply.settings.values[i].value);
            const PulseUiState state{View("option"), View(option)};
            if (model.set_states(model.context, panel, &state, 1) != PULSE_OK) { Message("Settings returned, but updating the option field failed."); return; }
            status = reply.operation == PULSE_RESOURCES_SETTINGS_GET ? "Settings loaded: version " : "Settings saved: version ";
            status += std::to_string(settings_version) + ".";
        }
        Show();
    }
    static void PULSE_CALL FileReply(void* user, const PulseFilesReply* reply) noexcept {
        auto* self = static_cast<Plugin*>(user); try { if (reply) self->Page(*reply); } catch (...) { self->Message("Analysis failed inside the DLL."); }
    }
    static void PULSE_CALL ResourceReply(void* user, const PulseResourcesReply* reply) noexcept {
        auto* self = static_cast<Plugin*>(user); try { if (reply) self->Resource(*reply); } catch (...) { self->Message("Resource reply processing failed inside the DLL."); }
    }
    static void PULSE_CALL Click(void* user, uint64_t panel_id, PulseStringView command) noexcept {
        auto* self = static_cast<Plugin*>(user); if (!self->active || self->panel != panel_id) return;
        try {
            const auto id = Copy(command);
            if (id == "analyze-button") { self->Analyze(); return; }
            self->Cancel();
            if (id == "stat-button") {
                if (self->first_entry.empty()) { self->Message("Analyze first to obtain an opaque file entry."); return; }
                self->status = "Reading first file details...";
                self->ResourceAccepted(self->resources.stat(self->resources.context, View(self->first_entry), ResourceReply, self, &self->resource_request));
            } else if (id == "load-settings-button") {
                self->status = "Loading own settings...";
                self->ResourceAccepted(self->resources.settings_get(self->resources.context, ResourceReply, self, &self->resource_request));
            } else if (id == "save-settings-button") {
                if (!self->settings_loaded) { self->Message("Load settings before saving an expected version."); return; }
                if (!self->ReadOption()) return;
                self->status = "Saving own settings...";
                self->ResourceAccepted(self->resources.settings_set(self->resources.context, View("display.sort"), View(self->option), 0, self->settings_version, ResourceReply, self, &self->resource_request));
            } else if (id == "audit-button") {
                self->status = "Reading own audit...";
                self->ResourceAccepted(self->resources.audit_list(self->resources.context, 128, ResourceReply, self, &self->resource_request));
            }
        } catch (...) { self->Message("Action failed inside the DLL."); }
    }
    PulseStatus Start() {
        auto code = host.Query(PULSE_UI_MODEL_INTERFACE, 1, model); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_FILES_INTERFACE_NAME, 1, files); if (code != PULSE_OK) return code;
        code = host.Query(PULSE_RESOURCES_INTERFACE_NAME, 1, resources); if (code != PULSE_OK) return code;
        active = true; code = Render(true); if (code != PULSE_OK) return code;
        for (size_t i = 0; i < 5; ++i) { code = model.subscribe_command(model.context, panel, View(commands[i]), Click, this, &subscriptions[i]); if (code != PULSE_OK) return code; }
        return PULSE_OK;
    }
    void Stop() noexcept {
        active = false; Cancel();
        for (auto& id : subscriptions) if (id) { model.unsubscribe(model.context, id); id = 0; }
        if (panel) { model.close(model.context, panel); panel = 0; }
    }
};
PulseStatus PULSE_CALL Activate(void* instance) noexcept { try { return static_cast<Plugin*>(instance)->Start(); } catch (...) { return PULSE_ERROR_INITIALIZATION; } }
void PULSE_CALL Deactivate(void* instance) noexcept { static_cast<Plugin*>(instance)->Stop(); }
void PULSE_CALL Destroy(void* instance) noexcept { auto* p = static_cast<Plugin*>(instance); p->Stop(); delete p; }
}
PULSE_PLUGIN_SDK_PREVIEW()
extern "C" __declspec(dllexport) PulseStatus PULSE_CALL PulsePluginEntryV1(const PulseHostV1* host, PulsePluginV1* out) noexcept {
    if (!host || !out || host->size < sizeof(*host) || out->size < sizeof(*out) || host->version != 1 || out->version != 1) return PULSE_ERROR_ABI;
    try { *out = {sizeof(*out), 1, new Plugin(*host), Activate, Deactivate, Destroy}; return PULSE_OK; } catch (...) { return PULSE_ERROR_INITIALIZATION; }
}
