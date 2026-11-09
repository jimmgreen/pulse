#include "archive_settings.h"
#include <pulse/client.hpp>
#include <array>
#include <stdexcept>
#include <string_view>

using pulse::sdk::View;
namespace {
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string{}; }
bool LocalAbsoluteDestination(std::string_view path) {
    if (path.empty()) return true;
    if (path.find_first_of("\r\n") != std::string_view::npos || path.find('\0') != std::string_view::npos) return false;
    if (path.starts_with("\\\\?\\")) path.remove_prefix(4);
    return path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

}
struct ArchiveSettings::State {
    PulseSettingsPagesV1 pages{};
    PulseUiModelV1 model{};
    PulseResourcesV1 resources{};
    PulseStagingV1 staging{};
    uint64_t page = 0, version = 0;
    PulseResourcesRequestId request = 0;
    std::string destination, format = "0", level = "1", pending;
    bool loading = false, saving = false, loaded = false, conflict_reload = false;
    std::array<pulse::sdk::Subscription, 3> subscriptions;
    ~State() {
        if (request) resources.cancel_request(resources.context, request);
        for (auto& subscription : subscriptions) subscription.Reset();
        if (page) pages.remove(pages.context, page);
    }
    void Set(const char* key, const std::string& value) {
        const PulseUiState update{View(key), View(value)};
        model.set_states(model.context, page, &update, 1);
    }
    void Status(const std::string& text) {
        Set("status", text); Set("ready", loaded && !loading && !saving ? "true" : "false");
        Set("idle", !loading && !saving ? "true" : "false");
    }
    std::string Read(const char* key) {
        uint32_t needed = 0;
        if (model.get_state(model.context, page, View(key), nullptr, 0, &needed) != PULSE_OK || needed > 1024)
            throw std::runtime_error("Cannot read settings input");
        std::string value(needed + 1, '\0');
        if (model.get_state(model.context, page, View(key), value.data(), static_cast<uint32_t>(value.size()), &needed) != PULSE_OK)
            throw std::runtime_error("Cannot read settings input");
        value.resize(needed); return value;
    }
    void Decode(const PulseSettings& values) {
        destination.clear(); format = "0"; level = "1";
        for (uint32_t i = 0; i < values.count; ++i) if (Copy(values.values[i].key) == "archive.preferences") {
            const auto value = Copy(values.values[i].value);
            if (value.size() < 6 || value.substr(0, 2) != "1\n" || value[3] != '\n' || value[5] != '\n' ||
                (value[2] != '0' && value[2] != '1') || value[4] < '0' || value[4] > '3')
                throw std::runtime_error("Saved preferences have an unsupported format. Reload after repairing the stored value.");
            format.assign(1, value[2]); level.assign(1, value[4]); destination = value.substr(6);
        }
        version = values.version;
    }
    void Load(bool preserve = false) {
        if (request) return;
        loading = true; conflict_reload = preserve;
        Status(preserve ? "Settings changed elsewhere. Reloading the current revision; your draft is retained." : "Loading settings...");
        if (resources.settings_get(resources.context, Completed, this, &request) != PULSE_OK) {
            request = 0; loading = false; Status("Settings load was rejected. Use Reload to retry.");
        }
    }
    void Save() {
        if (!loaded || request) return;
        const auto path = Read("destination"), chosen_format = Read("format"), chosen_level = Read("level");
        if (!LocalAbsoluteDestination(path)) {
            Status("Use an empty destination or a local absolute drive path. Relative paths, UNC paths and line breaks are not supported. Draft retained.");
            return;
        }
        if ((chosen_format != "0" && chosen_format != "1") || chosen_level.size() != 1 || chosen_level[0] < '0' || chosen_level[0] > '3') {
            Status("Check the destination, format and level before saving."); return;
        }
        pending = "1\n" + chosen_format + "\n" + chosen_level + "\n" + path;
        if (pending.size() > 1024) { Status("Destination is too long to store in these preferences."); return; }
        saving = true; Status("Saving settings...");
        if (resources.settings_set(resources.context, View("archive.preferences"), View(pending), 0, version,
            Completed, this, &request) != PULSE_OK) {
            request = 0; saving = false; Status("Save was rejected. Your draft is retained; retry Save.");
        }
    }
    static void PULSE_CALL Completed(void* value, const PulseResourcesReply* reply) noexcept {
        auto& self = *static_cast<State*>(value);
        try {
            self.request = 0;
            const bool save = self.saving;
            self.loading = self.saving = false;
            if (reply->status != PULSE_OK) {
                if (save && Copy(reply->error) == "version_conflict") { self.Load(true); return; }
                self.Status("Settings request failed: " + Copy(reply->error) + ". Draft retained; Reload or Save to retry."); return;
            }
            self.Decode(reply->settings); self.loaded = true;
            if (!self.conflict_reload) {
                self.Set("destination", self.destination); self.Set("format", self.format); self.Set("level", self.level);
            }
            const bool conflict = self.conflict_reload; self.conflict_reload = false;
            self.Status(conflict ? "New revision loaded. Draft retained. Review it and press Save to replace the current preferences."
                : (save ? "Saved settings revision " : "Loaded settings revision ") + std::to_string(self.version));
        } catch (...) {
            self.loaded = false; self.loading = self.saving = false;
            try { self.Status("Settings reply could not be read. Use Reload to retry."); } catch (...) {}
        }
    }
    struct Choice { std::string path; bool accepted = false; };
    static void PULSE_CALL Chosen(void* value, PulseStatus status, PulseStringView path) noexcept {
        auto& choice = *static_cast<Choice*>(value);
        try { if (status == PULSE_OK) { choice.path = Copy(path); choice.accepted = !choice.path.empty(); } } catch (...) {}
    }
    static void PULSE_CALL Command(void* value, uint64_t, PulseStringView command) noexcept {
        auto& self = *static_cast<State*>(value);
        try {
            const auto id = Copy(command);
            if (id == "save") self.Save();
            else if (id == "reload") self.Load();
            else if (id == "browse" && self.loaded && !self.request) {
                Choice choice; const auto initial = self.Read("destination");
                if (self.staging.choose_destination(self.staging.context, View(initial), Chosen, &choice) == PULSE_OK && choice.accepted) {
                    self.Set("destination", choice.path); self.Status("Destination selected. Press Save to persist it.");
                }
            }
        } catch (...) { try { self.Status("Settings action failed. Your saved preferences are unchanged."); } catch (...) {} }
    }
    PulseStatus Create() {
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;
        auto path = ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "destination-input")
            .String(&N::text, "Default extraction location")
            .String(&N::state, "destination").String(&N::placeholder, "Empty uses the archive directory")
            .String(&N::enabled_binding, "state.ready");
        auto browse = ModelNode(PULSE_UI_MODEL_BUTTON, "browse").String(&N::text, "Browse...")
            .String(&N::command, "browse").String(&N::enabled_binding, "state.ready");
        auto format_node = ModelNode(PULSE_UI_MODEL_SELECT, "format-input").String(&N::state, "format")
            .String(&N::text, "Compression format")
            .Options({"7z", "zip"}).String(&N::enabled_binding, "state.ready");
        auto level_node = ModelNode(PULSE_UI_MODEL_SELECT, "level-input").String(&N::state, "level")
            .String(&N::text, "Compression level")
            .Options({"Store", "Fast", "Normal", "Maximum"}).String(&N::enabled_binding, "state.ready");
        auto save = ModelNode(PULSE_UI_MODEL_BUTTON, "save").String(&N::text, "Save")
            .String(&N::command, "save").String(&N::enabled_binding, "state.ready");
        auto reload = ModelNode(PULSE_UI_MODEL_BUTTON, "reload").String(&N::text, "Reload")
            .String(&N::command, "reload").String(&N::enabled_binding, "state.idle");
        auto actions = ModelNode(PULSE_UI_MODEL_STACK, "settings-actions").String(&N::orientation, "horizontal")
            .Int(&N::gap, 8).Children({save, reload});
        auto content = ModelNode(PULSE_UI_MODEL_STACK, "archive-settings-content").Int(&N::gap, 10).Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "destination-label").String(&N::text, "Default extraction location"), path, browse,
            ModelNode(PULSE_UI_MODEL_TEXT, "format-label").String(&N::text, "Compression format"), format_node,
            ModelNode(PULSE_UI_MODEL_TEXT, "level-label").String(&N::text, "Compression level"), level_node,
            ModelNode(PULSE_UI_MODEL_TEXT, "compression-note").String(&N::text,
                "New archives use this format and level. Destination is chosen for each compression task; publication uses host confirmation and undo."), actions,
            ModelNode(PULSE_UI_MODEL_TEXT, "settings-status").String(&N::binding, "state.status")});
        auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "archive-settings-root").Children({content}))
            .State("destination", "").State("format", "0").State("level", "1")
            .State("status", "Loading settings...").State("ready", "false").State("idle", "false")
            .Command("browse").Command("save").Command("reload");
        auto result = pages.create(pages.context, View("Archive preferences"), &tree.Build(), &page);
        if (result != PULSE_OK) return result;
        result = pages.query(pages.context, page, View(PULSE_UI_MODEL_INTERFACE), PULSE_UI_MODEL_VERSION, &model, sizeof(model));
        if (result != PULSE_OK) return result;
        const char* commands[]{"browse", "save", "reload"};
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            result = subscriptions[i].Command(model, page, commands[i],
                [this](uint64_t panel, PulseStringView command) { Command(this, panel, command); });
            if (result != PULSE_OK) return result;
        }
        Load(); return PULSE_OK;
    }
};
ArchiveSettings::ArchiveSettings() : state_(std::make_unique<State>()) {}
ArchiveSettings::~ArchiveSettings() = default;
PulseStatus ArchiveSettings::Initialize(const pulse::sdk::Host& host, PulseStagingV1 staging) {
    state_->staging = staging;
    auto result = host.Query(PULSE_SETTINGS_PAGES_INTERFACE_NAME, PULSE_SETTINGS_PAGES_VERSION_1, state_->pages);
    if (result != PULSE_OK) return result;
    result = host.Query(PULSE_RESOURCES_INTERFACE_NAME, PULSE_RESOURCES_VERSION_1, state_->resources);
    return result == PULSE_OK ? state_->Create() : result;
}
const std::string& ArchiveSettings::Destination() const { return state_->destination; }
bool ArchiveSettings::Ready() const { return state_->loaded && !state_->loading && !state_->saving; }
bool ArchiveSettings::Zip() const { return state_->format == "1"; }
unsigned ArchiveSettings::Level() const {
    constexpr unsigned levels[]{0, 1, 5, 9};
    return levels[static_cast<unsigned>(state_->level[0] - '0')];
}
