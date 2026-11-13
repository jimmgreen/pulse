#include <windows.h>
#include "settings.h"
#include "association.h"
#include "engine.h"
#include "text.h"
#include <pulse/client.hpp>
#include <pulse/ui_events.h>
#include <array>
#include <atomic>
#include <map>
#include <stdexcept>
#include <optional>
#include <string_view>
#include <thread>

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
constexpr int kCommandCount = 6;  // plugin.cpp Command order
constexpr int kAllCommands = (1 << kCommandCount) - 1;
int HexDigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
std::string Hex2(int value) {
    constexpr char kDigits[] = "0123456789abcdef";
    return {kDigits[(value >> 4) & 15], kDigits[value & 15]};
}
std::string Hex3(unsigned value) {
    constexpr char kDigits[] = "0123456789abcdef";
    return {kDigits[(value >> 8) & 15], kDigits[(value >> 4) & 15], kDigits[value & 15]};
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
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
                                              {"level", "3"}, {"remember", "true"}, {"placement", "0"},
                                              {"commands", Hex2(kAllCommands)},
                                              {"take", "false"}, {"dbl", "0"}, {"inc", Hex3(association::kAllGroups)}};
    bool loading = false, saving = false, loaded = false, dirty = false;
    pulse::sdk::Subscription browse, edits;
    // 接管压缩包: registry and COM work runs on `worker`, one job at a time; results come back through `dispatch`.
    pulse::sdk::DispatchQueue* dispatch = nullptr;
    std::array<pulse::sdk::Subscription, association::kGroupCount + 2> take_commands;
    std::vector<association::Status> statuses = std::vector<association::Status>(association::kGroupCount);
    std::thread worker;
    std::atomic<bool> stop{false};
    unsigned job = 0;
    bool busy = false;
    int chain = -1;  // 逐个确认: the group whose chooser was just shown
    ~State() {
        StopWorker();
        if (request) resources.cancel_request(resources.context, request);
        browse.Reset();
        edits.Reset();
        for (auto& command : take_commands) command.Reset();
        if (page) pages.remove(pages.context, page);
    }
    void StopWorker() {
        stop = true;
        if (worker.joinable()) worker.join();
        stop = false;
    }
    unsigned Groups() const {
        const auto& hex = values.at("inc");
        unsigned value = 0;
        for (const char c : hex) {
            const int digit = HexDigit(c);
            if (hex.size() != 3 || digit < 0) return association::kAllGroups;
            value = (value << 4) | static_cast<unsigned>(digit);
        }
        return value & association::kAllGroups;
    }
    bool Taking() const { return values.at("take") == "true"; }
    enum class Job { Query, Refresh, Apply, Remove };
    // Writes or removes the associations as the values say, then reads back how Windows opens each format.
    // `watch`: keep re-reading for a minute (the user is changing them in Windows Settings).
    void Run(Job kind, bool watch = false) {
        if (!dispatch) return;
        StopWorker();
        const unsigned groups = Groups(), serial = ++job;
        const bool extract = values.at("dbl") != "1", taking = Taking();
        if (kind == Job::Apply || kind == Job::Remove) { busy = true; DeriveTake(); }
        worker = std::thread([this, kind, watch, groups, extract, taking, serial] {
            bool ok = true;
            if (kind == Job::Apply || kind == Job::Refresh) ok = association::Apply(groups, extract);
            else if (kind == Job::Remove) association::Remove();
            auto result = association::Query(taking ? groups : 0);
            auto posted = result;
            dispatch->Post([this, serial, kind, ok, posted = std::move(posted)]() mutable { Finished(serial, kind, ok, std::move(posted)); });
            for (int tick = 0; watch && tick < 300 && !stop; ++tick) {
                Sleep(200);
                if (tick % 10 != 9 || stop) continue;
                auto again = association::Query(taking ? groups : 0);
                bool same = again.size() == result.size();
                for (size_t i = 0; same && i < again.size(); ++i) same = again[i].owner == result[i].owner && again[i].who == result[i].who;
                if (same) continue;
                result = again;
                dispatch->Post([this, serial, again = std::move(again)]() mutable { Finished(serial, Job::Query, true, std::move(again)); });
            }
        });
    }
    void Finished(unsigned serial, Job kind, bool ok, std::vector<association::Status> result) {
        if (serial != job) return;
        busy = false;
        statuses = std::move(result);
        DeriveTake();
        if ((kind == Job::Apply || kind == Job::Refresh) && !ok) Status(text::Get(T::TakeFailed));
        // 逐个确认: once a format became Pulse's, Windows' chooser comes up for the next one.
        if (chain >= 0) {
            const int done = chain;
            chain = -1;
            if (statuses[static_cast<size_t>(done)].owner == association::Owner::Pulse) {
                const int next = NextAsk();
                if (next >= 0) dispatch->Post([this, next] { Choose(next, true); });
            }
        }
    }
    int NextAsk() const {
        if (!Taking()) return -1;
        for (int i = 0; i < association::kGroupCount; ++i)
            if (statuses[static_cast<size_t>(i)].owner == association::Owner::Other) return i;
        return -1;
    }
    void Choose(int group, bool chained) {
        if (busy || !Taking() || group < 0) return;
        association::Choose(GetActiveWindow(), group);
        chain = chained ? group : -1;
        Run(Job::Query);
    }
    // 接管压缩包 as Pulse's 7-Zip page reads it (extension_store_seven_zip.cpp); a page holds at most 32
    // states, so the whole section is three: takecfg "<take 0-1>|<double-click 0-1>|<formats, 3 hex digits>",
    // takeview "<busy 0-1>" then per format "\n<owner p/o/f/x><w if Windows' own folder, else ->who",
    // takefmt "<icon library>" then per format "\n<label>|<extensions>|<icon resource>".
    std::string Config() const {
        return std::string(Taking() ? "1" : "0") + "|" + values.at("dbl") + "|" + values.at("inc");
    }
    void DeriveTake() {
        const bool taking = Taking();
        std::string view = busy ? "1" : "0";
        for (const auto& status : statuses) {
            const auto owner = taking ? status.owner : association::Owner::Off;
            view += owner == association::Owner::Pulse ? "\np" : owner == association::Owner::Other ? "\no"
                  : owner == association::Owner::Free ? "\nf" : "\nx";
            view += status.windows ? "w" : "-";
            view += Utf8(status.who).substr(0, 60);
        }
        Set("takecfg", Config());
        Set("takeview", view);
    }
    void Set(const std::string& key, const std::string& value) {
        const PulseUiState update{View(key), View(value)};
        model.set_states(model.context, page, &update, 1);
    }
    void Status(const std::string& value) { Set("status", value); }
    void Show() {
        for (const auto& [key, value] : values)
            if (key != "take" && key != "dbl" && key != "inc") Set(key, value);
        Derive();
        Set("ready", loaded ? "true" : "false");
        DeriveTake();
    }
    std::string Encoded() const {
        const auto& v = values;
        return "5\n" + v.at("mode") + "\n" + v.at("location") + "\n" + v.at("format") + "\n" + v.at("level") + "\n" +
               (v.at("remember") == "false" ? "0" : "1") + "\n" + v.at("placement") + "\n" + v.at("commands") + "\n" +
               (v.at("take") == "true" ? "1" : "0") + "\n" + v.at("dbl") + "\n" + v.at("inc") + "\n" + v.at("folder");
    }
    void Decode(const PulseSettings& settings) {
        for (uint32_t i = 0; i < settings.count; ++i) {
            if (Copy(settings.values[i].key) != kKey) continue;
            const auto value = Copy(settings.values[i].value);
            // v3: "3\n<mode 0-2>\n<location 0-1>\n<format 0-5>\n<level 0-5>\n<remember 0-1>\n<folder>"
            // v4: the same, then "<placement 0-1>\n<commands, 2 hex digits>\n" before <folder>.
            // v5: v4, then "<take 0-1>\n<double-click 0-1>\n<formats taken, 3 hex digits>\n" before <folder>.
            const bool v3 = value.compare(0, 2, "3\n") == 0, v4 = value.compare(0, 2, "4\n") == 0, v5 = value.compare(0, 2, "5\n") == 0;
            if (value.size() >= 12 && (v3 || v4 || v5) && value[3] == '\n' && value[5] == '\n' && value[7] == '\n' &&
                value[9] == '\n' && value[11] == '\n' && value[2] >= '0' && value[2] <= '2' && (value[4] == '0' || value[4] == '1') &&
                value[6] >= '0' && value[6] <= '5' && value[8] >= '0' && value[8] <= '5' && (value[10] == '0' || value[10] == '1')) {
                size_t folder = 12;
                if (v4 || v5) {
                    if (value.size() < 17 || (value[12] != '0' && value[12] != '1') || value[13] != '\n' || value[16] != '\n') continue;
                    const int high = HexDigit(value[14]), low = HexDigit(value[15]);
                    if (high < 0 || low < 0 || ((high << 4) | low) > kAllCommands) continue;
                    folder = 17;
                }
                if (v5) {
                    if (value.size() < 25 || (value[17] != '0' && value[17] != '1') || value[18] != '\n' ||
                        (value[19] != '0' && value[19] != '1') || value[20] != '\n' || value[24] != '\n' ||
                        HexDigit(value[21]) < 0 || HexDigit(value[22]) < 0 || HexDigit(value[23]) < 0) continue;
                    values["take"] = value[17] == '1' ? "true" : "false";
                    values["dbl"].assign(1, value[19]);
                    values["inc"] = value.substr(21, 3);
                    folder = 25;
                }
                if (v4 || v5) {
                    values["placement"].assign(1, value[12]);
                    values["commands"] = value.substr(14, 2);
                }
                values["mode"].assign(1, value[2]);
                values["location"].assign(1, value[4]);
                values["format"].assign(1, value[6]);
                values["level"].assign(1, value[8]);
                values["remember"] = value[10] == '1' ? "true" : "false";
                values["folder"] = value.substr(folder);
            }
        }
        version = settings.version;
    }
    int Commands() const {
        const auto& hex = values.at("commands");
        return hex.size() == 2 && HexDigit(hex[0]) >= 0 && HexDigit(hex[1]) >= 0 ? (HexDigit(hex[0]) << 4) | HexDigit(hex[1]) : kAllCommands;
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
                const bool first = !self.loaded;
                self.Decode(reply->settings);
                if (self.dirty) self.values = mine;
                self.loaded = true;
                self.Show();
                // The extension's files may have moved (an update): keep the icons and command pointing at them.
                if (first) self.Run(self.Taking() ? Job::Refresh : Job::Query);
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
        std::optional<Job> takeover;
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto key = Copy(batch.changes[i].key), value = Copy(batch.changes[i].value);
            if (key != "takecfg") continue;
            // "<take>|<double-click>|<formats>" from Pulse's 7-Zip page; one change at a time while idle.
            if (busy || value.size() != 7 || (value[0] != '0' && value[0] != '1') || value[1] != '|' ||
                (value[2] != '0' && value[2] != '1') || value[3] != '|' ||
                HexDigit(value[4]) < 0 || HexDigit(value[5]) < 0 || HexDigit(value[6]) < 0) {
                DeriveTake();
                continue;
            }
            const bool take = value[0] == '1';
            const std::string dbl(1, value[2]), inc = value.substr(4, 3);
            if (take != Taking()) takeover = take ? Job::Apply : Job::Remove;
            else if (take && (dbl != values["dbl"] || inc != values["inc"])) takeover = Job::Apply;
            values["take"] = take ? "true" : "false";
            values["dbl"] = dbl;
            values["inc"] = inc;
            save = true;
        }
        if (takeover) Run(*takeover);
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto key = Copy(batch.changes[i].key), value = Copy(batch.changes[i].value);
            // Radios report "true" for the one picked: mode0..2, location0..1, placement0..1.
            const auto radio = [&](const char* group, char max) {
                const std::string prefix(group);
                if (key.size() != prefix.size() + 1 || key.compare(0, prefix.size(), prefix) != 0 || value != "true" ||
                    key.back() < '0' || key.back() > max) return false;
                values[prefix].assign(1, key.back());
                return true;
            };
            if (radio("mode", '2') || radio("location", '1') || radio("placement", '1')) {
                save = true;
                Derive();
            } else if (key.size() == 4 && key.compare(0, 3, "cmd") == 0 && key[3] >= '0' && key[3] < '0' + kCommandCount &&
                       (value == "true" || value == "false")) {
                const int bit = 1 << (key[3] - '0');
                values["commands"] = Hex2(value == "true" ? Commands() | bit : Commands() & ~bit);
                save = true;
                Derive();
            } else if ((key == "mode" && Digit(value, '2')) || (key == "location" && Digit(value, '1')) ||
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
        const auto first = fixed && !values["folder"].empty() ? T::CmdFixed : mode == 1 ? T::CmdFolder : mode == 2 ? T::CmdHere : T::CmdSmart;
        Set("menushows", text::Format(T::MenuShows, text::Get(first)));
        // One radio per choice: exactly the saved one is on.
        for (const auto& [group, count] : {std::pair<const char*, int>{"mode", 3}, {"location", 2}, {"placement", 2}})
            for (int i = 0; i < count; ++i)
                Set(group + std::to_string(i), values[group][0] - '0' == i ? "true" : "false");
        const int shown = Commands();
        for (int i = 0; i < kCommandCount; ++i) Set("cmd" + std::to_string(i), (shown >> i) & 1 ? "true" : "false");
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
        std::vector<std::string> levels;
        for (const auto id : {T::Level0, T::Level1, T::Level3, T::Level5, T::Level7, T::Level9}) levels.push_back(text::Get(id));

        auto hero = card("hero", {
            section("hero-title", text::Get(T::HeroTitle)),
            caption("hero-detail", text::Format(T::HeroDetail, std::to_string(engine::SupportedFormats().size()))),
            ModelNode(PULSE_UI_MODEL_TEXT, "settings-status").String(&N::binding, "state.status").String(&N::role, "caption")}).Int(&N::gap, 2);
        // A titled choice: the title over its description, then one radio per option, each with a caption.
        const auto choices = [&](const std::string& id, const std::string& title, const std::string& description,
                                 std::vector<std::pair<std::string, std::string>> options) {
            std::vector<ModelNode> children{ModelNode(PULSE_UI_MODEL_TEXT, (id + "-title").c_str()).String(&N::text, title)};
            if (!description.empty()) children.push_back(caption((id + "-desc").c_str(), description));
            for (size_t i = 0; i < options.size(); ++i) {
                const auto key = id + std::to_string(i);
                std::vector<ModelNode> option{
                    ready(ModelNode(PULSE_UI_MODEL_RADIO, (key + "-input").c_str()).String(&N::text, options[i].first).String(&N::state, key))};
                if (!options[i].second.empty()) option.push_back(caption((key + "-caption").c_str(), options[i].second));
                children.push_back(ModelNode(PULSE_UI_MODEL_STACK, (key + "-option").c_str()).Int(&N::gap, 0).Children(std::move(option)));
            }
            return ModelNode(PULSE_UI_MODEL_STACK, (id + "-choices").c_str()).Int(&N::gap, 8).Children(std::move(children));
        };
        auto extract = card("extract-card", {
            choices("mode", text::Get(T::ModeTitle), text::Get(T::ModeDesc), {
                {text::Get(T::ModeSmart), text::Get(T::ModeSmartDesc)},
                {text::Get(T::ModeFolder), text::Get(T::ModeFolderDesc)},
                {text::Get(T::ModeHere), text::Get(T::ModeHereDesc)}}),
            ModelNode(PULSE_UI_MODEL_TEXT, "menu-shows").String(&N::binding, "state.menushows").String(&N::role, "caption"),
            ModelNode(PULSE_UI_MODEL_DIVIDER, "extract-divider"),
            choices("location", text::Get(T::LocationTitle), {}, {
                {text::Get(T::LocationBeside), {}},
                {text::Get(T::LocationFixed), text::Get(T::LocationFixedDesc)}}),
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
        std::vector<ModelNode> menu_rows{
            choices("placement", text::Get(T::PlacementTitle), text::Get(T::PlacementDesc), {
                {text::Get(T::PlacementTop), text::Get(T::PlacementTopDesc)},
                {text::Get(T::PlacementSub), text::Get(T::PlacementSubDesc)}}),
            ModelNode(PULSE_UI_MODEL_DIVIDER, "menu-divider"),
            ModelNode(PULSE_UI_MODEL_TEXT, "commands-title").String(&N::text, text::Get(T::CommandsTitle)),
            caption("commands-desc", text::Get(T::CommandsDesc))};
        const T kCommandTitles[kCommandCount] = {T::CmdExtractSwitch, T::CmdTo, T::CmdTest, T::CmdQuickSwitch, T::CmdAdd, T::CmdHash};
        for (int i = 0; i < kCommandCount; ++i) {
            const auto key = "cmd" + std::to_string(i);
            menu_rows.push_back(row(key, text::Get(kCommandTitles[i]), {}, nullptr,
                ready(ModelNode(PULSE_UI_MODEL_SWITCH, (key + "-input").c_str()).String(&N::state, key).Int(&N::min_width, 48).Int(&N::max_width, 48))));
        }
        auto menu = card("menu-card", std::move(menu_rows));
        auto content = ModelNode(PULSE_UI_MODEL_STACK, "settings-content").Int(&N::gap, 12).Children({
            hero, section("extract-title", text::Get(T::SettingsExtractSection)), extract,
            section("compress-title", text::Get(T::SettingsCompressSection)), compress,
            section("menu-title", text::Get(T::MenuSection)), menu,
            section("password-title", text::Get(T::SettingsPasswordSection)), passwords,
            caption("rar-note", text::Get(T::RarNote))});
        // The engine's formats for the store's 支持格式 view: writable ones carry "(+)".
        std::string formats_rw, formats_ro;
        for (const auto& line : engine::SupportedFormats()) {
            const auto name = line.substr(0, line.find(L':'));
            std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr)), '\0');
            if (!utf8.empty()) WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            for (auto& c : utf8) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            auto& target = line.size() >= 3 && line.compare(line.size() - 3, 3, L"(+)") == 0 ? formats_rw : formats_ro;
            if (!utf8.empty()) target += (target.empty() ? "" : ",") + utf8;
        }
        auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "settings-root").Children({content}))
            .State("formats_rw", formats_rw).State("formats_ro", formats_ro)
            .State("mode", "0").State("location", "0").State("folder", "").State("format", "0").State("level", "3")
            .State("remember", "true").State("fixed", "false").State("status", text::Get(T::AutoSaved)).State("ready", "false")
            .State("modedesc", text::Get(T::ModeSmartDesc)).State("quickdesc", "")
            .State("menushows", text::Format(T::MenuShows, text::Get(T::CmdSmart)))
            .State("mode0", "true").State("mode1", "false").State("mode2", "false")
            .State("location0", "true").State("location1", "false")
            .State("placement0", "true").State("placement1", "false")
            .State("placement", "0").State("commands", Hex2(kAllCommands))
            .State("cmd0", "true").State("cmd1", "true").State("cmd2", "true").State("cmd3", "true").State("cmd4", "true").State("cmd5", "true")
            .Command("browse").Command("asknext").Command("winsettings");
        // 接管压缩包 lives on Pulse's own 7-Zip page; this page only carries its data (DeriveTake).
        std::string formats = Utf8(association::IconLibrary());
        for (int i = 0; i < association::kGroupCount; ++i) {
            formats += std::string("\n") + association::GroupLabel(i) + "|" + association::GroupExtensions(i) + "|" +
                       std::to_string(association::GroupIcon(i));
            tree.Command("ask" + std::to_string(i));
        }
        tree.State("takecfg", Config()).State("takeview", "0").State("takefmt", formats);
        auto result = pages.create(pages.context, View(text::Get(T::SettingsTitle)), &tree.Build(), &page);
        if (result != PULSE_OK) return result;
        result = pages.query(pages.context, page, View(PULSE_UI_MODEL_INTERFACE), PULSE_UI_MODEL_VERSION, &model, sizeof(model));
        if (result != PULSE_OK) return result;
        result = pages.query(pages.context, page, View(PULSE_UI_EVENTS_INTERFACE_NAME), PULSE_UI_EVENTS_VERSION_1, &events, sizeof(events));
        if (result != PULSE_OK) return result;
        result = browse.Command(model, page, "browse", [this](uint64_t, PulseStringView) { try { Browse(); } catch (...) {} });
        if (result != PULSE_OK) return result;
        result = take_commands[0].Command(model, page, "asknext", [this](uint64_t, PulseStringView) { try { Choose(NextAsk(), true); } catch (...) {} });
        if (result != PULSE_OK) return result;
        result = take_commands[1].Command(model, page, "winsettings", [this](uint64_t, PulseStringView) {
            try { if (Taking() && !busy) { association::OpenDefaultApps(); Run(Job::Query, true); } } catch (...) {}
        });
        if (result != PULSE_OK) return result;
        for (int i = 0; i < association::kGroupCount; ++i) {
            result = take_commands[static_cast<size_t>(i) + 2].Command(model, page, "ask" + std::to_string(i),
                [this, i](uint64_t, PulseStringView) { try { Choose(i, false); } catch (...) {} });
            if (result != PULSE_OK) return result;
        }
        result = edits.State(model, events, page, {}, PULSE_UI_STATE_USER,
                             [this](uint64_t, const PulseUiStateBatch& batch) { try { Edited(batch); } catch (...) {} });
        if (result != PULSE_OK) return result;
        Load();
        return PULSE_OK;
    }
};

Settings::Settings() : state_(std::make_unique<State>()) {}
Settings::~Settings() = default;
PulseStatus Settings::Initialize(const pulse::sdk::Host& host, PulseStagingV1 staging, pulse::sdk::DispatchQueue& dispatch,
                                 std::function<void()> changed) {
    state_->staging = staging;
    state_->dispatch = &dispatch;
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
bool Settings::MenuSubmenu() const { return state_->values.at("placement") == "1"; }
bool Settings::CommandShown(int index) const { return index >= 0 && index < kCommandCount && ((state_->Commands() >> index) & 1); }
bool Settings::TakeOver() const { return state_->Taking(); }
