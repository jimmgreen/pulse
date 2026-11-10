#include "settings.h"
#include "engine.h"
#include "text.h"
#include <pulse/client.hpp>
#include <pulse/ui_events.h>
#include <array>
#include <map>
#include <stdexcept>
#include <string_view>

using pulse::sdk::View;
using T = text::Id;
namespace {
constexpr const char* kKey = "7zip.preferences";
std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string{}; }
bool LocalAbsolute(std::string_view path) {
    if (path.find_first_of("\r\n") != std::string_view::npos || path.find('\0') != std::string_view::npos) return false;
    if (path.starts_with("\\\\?\\")) path.remove_prefix(4);
    return path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}
bool Digit(const std::string& value, char max) { return value.size() == 1 && value[0] >= '0' && value[0] <= max; }
} // namespace

struct Settings::State {
    PulseSettingsPagesV1 pages{};
    PulseUiModelV1 model{};
    PulseUiEventsV1 events{};
    PulseResourcesV1 resources{};
    PulseStagingV1 staging{};
    std::function<void()> changed;
    uint64_t page = 0, version = 0;
    PulseResourcesRequestId request = 0;
    // Saved values, as the page shows them.
    std::map<std::string, std::string> values{{"mode", "0"}, {"location", "0"}, {"folder", ""}, {"format", "0"},
                                              {"level", "3"}, {"remember", "true"}};
    bool loading = false, saving = false, loaded = false, dirty = false;
    pulse::sdk::Subscription browse, edits;
    ~State() {
        if (request) resources.cancel_request(resources.context, request);
        browse.Reset();
        edits.Reset();
        if (page) pages.remove(pages.context, page);
    }
    void Set(const std::string& key, const std::string& value) {
        const PulseUiState update{View(key), View(value)};
        model.set_states(model.context, page, &update, 1);
    }
    void Status(const std::string& value) { Set("status", value); }
    void Show() {
        for (const auto& [key, value] : values) Set(key, value);
        Derive();
        Set("ready", loaded ? "true" : "false");
    }
    std::string Encoded() const {
        const auto& v = values;
        return "3\n" + v.at("mode") + "\n" + v.at("location") + "\n" + v.at("format") + "\n" + v.at("level") + "\n" +
               (v.at("remember") == "false" ? "0" : "1") + "\n" + v.at("folder");
    }
    void Decode(const PulseSettings& settings) {
        for (uint32_t i = 0; i < settings.count; ++i) {
            if (Copy(settings.values[i].key) != kKey) continue;
            const auto value = Copy(settings.values[i].value);
            // "3\n<mode 0-2>\n<location 0-1>\n<format 0-5>\n<level 0-5>\n<remember 0-1>\n<folder>"
            if (value.size() >= 12 && value.compare(0, 2, "3\n") == 0 && value[3] == '\n' && value[5] == '\n' && value[7] == '\n' &&
                value[9] == '\n' && value[11] == '\n' && value[2] >= '0' && value[2] <= '2' && (value[4] == '0' || value[4] == '1') &&
                value[6] >= '0' && value[6] <= '5' && value[8] >= '0' && value[8] <= '5' && (value[10] == '0' || value[10] == '1')) {
                values["mode"].assign(1, value[2]);
                values["location"].assign(1, value[4]);
                values["format"].assign(1, value[6]);
                values["level"].assign(1, value[8]);
                values["remember"] = value[10] == '1' ? "true" : "false";
                values["folder"] = value.substr(12);
            }
        }
        version = settings.version;
    }
    void Load() {
        if (request) return;
        loading = true;
        if (resources.settings_get(resources.context, Completed, this, &request) != PULSE_OK) {
            request = 0; loading = false; Status(text::Get(T::SettingsFailed));
        }
    }
    void Save() {
        if (!loaded) return;
        if (request) { dirty = true; return; }  // one write at a time; the latest values follow
        dirty = false;
        saving = true;
        const auto data = Encoded();
        if (resources.settings_set(resources.context, View(kKey), View(data), 0, version, Completed, this, &request) != PULSE_OK) {
            request = 0; saving = false; Status(text::Get(T::SettingsFailed));
        }
    }
    static void PULSE_CALL Completed(void* user, const PulseResourcesReply* reply) noexcept {
        auto& self = *static_cast<State*>(user);
        try {
            self.request = 0;
            const bool save = self.saving;
            self.loading = self.saving = false;
            if (reply->status != PULSE_OK) {
                if (save && Copy(reply->error) == "version_conflict") {  // another window saved: keep ours, retry on its revision
                    self.dirty = true;
                    self.Load();
                    return;
                }
                self.Status(text::Get(T::SettingsFailed));
                return;
            }
            if (save) {
                self.version = reply->settings.version ? reply->settings.version : self.version + 1;
                self.Status(text::Get(T::SettingsSaved));
            } else {
                const auto mine = self.values;
                self.Decode(reply->settings);
                if (self.dirty) self.values = mine;
                self.loaded = true;
                self.Show();
            }
            if (self.dirty) self.Save();
            if (self.changed) self.changed();
        } catch (...) {
            self.loading = self.saving = false;
            try { self.Status(text::Get(T::SettingsFailed)); } catch (...) {}
        }
    }
    void Edited(const PulseUiStateBatch& batch) {
        bool save = false;
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto key = Copy(batch.changes[i].key), value = Copy(batch.changes[i].value);
            if ((key == "mode" && Digit(value, '2')) || (key == "location" && Digit(value, '1')) ||
                (key == "format" && Digit(value, '5')) || (key == "level" && Digit(value, '5')) ||
                (key == "remember" && (value == "true" || value == "false"))) {
                values[key] = value;
                save = true;
                Derive();
            } else if (key == "folder") {
                if (value.empty() || LocalAbsolute(value)) { values[key] = value; save = true; Status({}); }
                else Status(text::Get(T::SettingsBadPath));
            }
        }
        if (save) {
            Save();
            if (changed) changed();
        }
    }
    struct Choice { std::string path; bool accepted = false; };
    static void PULSE_CALL Chosen(void* user, PulseStatus status, PulseStringView path) noexcept {
        auto& choice = *static_cast<Choice*>(user);
        try { if (status == PULSE_OK) { choice.path = Copy(path); choice.accepted = !choice.path.empty(); } } catch (...) {}
    }
    void Browse() {
        Choice choice;
        if (staging.choose_destination(staging.context, View(values["folder"]), Chosen, &choice) == PULSE_OK && choice.accepted) {
            values["folder"] = choice.path;
            values["location"] = "1";
            Show();
            Save();
            if (changed) changed();
        }
    }
    // Texts that follow the choices: the mode's explanation, the menu's first command, the quick-compress hint.
    void Derive() {
        const int mode = values["mode"][0] - '0', format = values["format"][0] - '0';
        constexpr const char* kFormatNames[] = {"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
        const bool fixed = values["location"] == "1";
        Set("modedesc", text::Get(mode == 0 ? T::ModeSmartDesc : mode == 1 ? T::ModeFolderDesc : T::ModeHereDesc));
        Set("quickdesc", text::Format(T::QuickFormatDesc, kFormatNames[format]));
        Set("fixed", fixed ? "true" : "false");
    }
    PulseStatus Create() {
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;
        const auto caption = [](const char* id, const std::string& value) {
            return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "caption");
        };
        const auto section = [](const char* id, const std::string& value) {
            return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "section");
        };
        // Title and description on the left, the control on the right; the description may follow a state value.
        const auto row = [](const std::string& id, const std::string& title, const std::string& description,
                            const char* description_state, ModelNode control) {
            auto detail = ModelNode(PULSE_UI_MODEL_TEXT, (id + "-desc").c_str()).String(&N::text, description).String(&N::role, "caption");
            if (description_state) detail.String(&N::binding, description_state);
            return ModelNode(PULSE_UI_MODEL_STACK, (id + "-row").c_str()).String(&N::orientation, "horizontal").String(&N::alignment, "center")
                .Int(&N::gap, 16).Children({
                    ModelNode(PULSE_UI_MODEL_STACK, (id + "-text").c_str()).Int(&N::gap, 0).Int(&N::flex, 1).Children({
                        ModelNode(PULSE_UI_MODEL_TEXT, (id + "-title").c_str()).String(&N::text, title), std::move(detail)}),
                    std::move(control)});
        };
        const auto card = [](const char* id, std::vector<ModelNode> children) {
            return ModelNode(PULSE_UI_MODEL_GROUP, id).Bool(&N::bordered, true).Int(&N::gap, 14).Children(std::move(children));
        };
        const auto ready = [](ModelNode node) { return node.String(&N::enabled_binding, "state.ready"); };
        const auto select = [&](const char* id, const char* state, std::vector<std::string> options) {
            return ready(ModelNode(PULSE_UI_MODEL_SELECT, id).String(&N::state, state).Options(std::move(options))
                .Int(&N::min_width, 220).Int(&N::max_width, 220));
        };
        std::vector<std::string> levels;
        for (const auto id : {T::Level0, T::Level1, T::Level3, T::Level5, T::Level7, T::Level9}) levels.push_back(text::Get(id));

        auto hero = card("hero", {
            section("hero-title", text::Get(T::HeroTitle)),
            caption("hero-detail", text::Format(T::HeroDetail, std::to_string(engine::SupportedFormats().size()))),
            ModelNode(PULSE_UI_MODEL_TEXT, "settings-status").String(&N::binding, "state.status").String(&N::role, "caption")}).Int(&N::gap, 2);
        auto extract = card("extract-card", {
            row("mode", text::Get(T::ModeTitle), text::Get(T::ModeDesc), "state.modedesc",
                select("mode-input", "mode", {text::Get(T::ModeSmart), text::Get(T::ModeFolder), text::Get(T::ModeHere)})),
            row("location", text::Get(T::LocationTitle), text::Get(T::LocationFixedDesc), nullptr,
                select("location-input", "location", {text::Get(T::LocationBeside), text::Get(T::LocationFixed)})),
            ModelNode(PULSE_UI_MODEL_STACK, "folder-row").String(&N::orientation, "horizontal").Int(&N::gap, 8)
                .String(&N::visible_binding, "state.fixed").Children({
                    ready(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "folder-input").String(&N::text, text::Get(T::LocationFixed))
                        .String(&N::state, "folder").String(&N::placeholder, text::Get(T::SettingsDestinationHint)).Int(&N::flex, 1)),
                    ready(ModelNode(PULSE_UI_MODEL_BUTTON, "browse").String(&N::text, text::Get(T::SettingsBrowse)).String(&N::command, "browse"))})});
        auto compress = card("compress-card", {
            row("format", text::Get(T::QuickFormatTitle), {}, "state.quickdesc",
                ready(ModelNode(PULSE_UI_MODEL_SEGMENTED, "format-input").String(&N::state, "format")
                    .Options({"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"}).Int(&N::min_width, 340).Int(&N::max_width, 340))),
            row("level", text::Get(T::QuickLevelTitle), text::Get(T::QuickLevelDesc), nullptr,
                ready(ModelNode(PULSE_UI_MODEL_SLIDER, "level-input").String(&N::state, "level").Int(&N::stops, 6).Options(levels)
                    .Int(&N::min_width, 340).Int(&N::max_width, 340))),
            caption("compress-note", text::Get(T::SettingsCompressNote))});
        auto passwords = card("password-card", {
            row("remember", text::Get(T::RememberTitle), text::Get(T::RememberDesc), nullptr,
                ready(ModelNode(PULSE_UI_MODEL_SWITCH, "remember-input").String(&N::state, "remember").Int(&N::min_width, 48).Int(&N::max_width, 48)))});
        auto content = ModelNode(PULSE_UI_MODEL_STACK, "settings-content").Int(&N::gap, 12).Children({
            hero, section("extract-title", text::Get(T::SettingsExtractSection)), extract,
            section("compress-title", text::Get(T::SettingsCompressSection)), compress,
            section("password-title", text::Get(T::SettingsPasswordSection)), passwords,
            caption("rar-note", text::Get(T::RarNote))});
        auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "settings-root").Children({content}))
            .State("mode", "0").State("location", "0").State("folder", "").State("format", "0").State("level", "3")
            .State("remember", "true").State("fixed", "false").State("status", text::Get(T::AutoSaved)).State("ready", "false")
            .State("modedesc", text::Get(T::ModeSmartDesc)).State("quickdesc", "").Command("browse");
        auto result = pages.create(pages.context, View(text::Get(T::SettingsTitle)), &tree.Build(), &page);
        if (result != PULSE_OK) return result;
        result = pages.query(pages.context, page, View(PULSE_UI_MODEL_INTERFACE), PULSE_UI_MODEL_VERSION, &model, sizeof(model));
        if (result != PULSE_OK) return result;
        result = pages.query(pages.context, page, View(PULSE_UI_EVENTS_INTERFACE_NAME), PULSE_UI_EVENTS_VERSION_1, &events, sizeof(events));
        if (result != PULSE_OK) return result;
        result = browse.Command(model, page, "browse", [this](uint64_t, PulseStringView) { try { Browse(); } catch (...) {} });
        if (result != PULSE_OK) return result;
        result = edits.State(model, events, page, {}, PULSE_UI_STATE_USER,
                             [this](uint64_t, const PulseUiStateBatch& batch) { try { Edited(batch); } catch (...) {} });
        if (result != PULSE_OK) return result;
        Load();
        return PULSE_OK;
    }
};

Settings::Settings() : state_(std::make_unique<State>()) {}
Settings::~Settings() = default;
PulseStatus Settings::Initialize(const pulse::sdk::Host& host, PulseStagingV1 staging, std::function<void()> changed) {
    state_->staging = staging;
    state_->changed = std::move(changed);
    auto result = host.Query(PULSE_SETTINGS_PAGES_INTERFACE_NAME, PULSE_SETTINGS_PAGES_VERSION_1, state_->pages);
    if (result != PULSE_OK) return result;
    result = host.Query(PULSE_RESOURCES_INTERFACE_NAME, PULSE_RESOURCES_VERSION_1, state_->resources);
    return result == PULSE_OK ? state_->Create() : result;
}
bool Settings::Ready() const { return state_->loaded; }
Settings::ExtractMode Settings::Mode() const { return static_cast<ExtractMode>(state_->values.at("mode")[0] - '0'); }
std::string Settings::FixedFolder() const { return state_->values.at("location") == "1" ? state_->values.at("folder") : std::string{}; }
int Settings::Format() const { return state_->values.at("format")[0] - '0'; }
int Settings::Level() const { return state_->values.at("level")[0] - '0'; }
bool Settings::RememberPasswords() const { return state_->values.at("remember") != "false"; }
