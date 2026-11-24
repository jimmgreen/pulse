#include <windows.h>
#include "settings.h"
#include "command_preferences.h"
#include "association.h"
#include "takeover_record.h"
#include "engine.h"
#include "text.h"
#include <pulse/client.hpp>
#include <pulse/ui_events.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using pulse::sdk::View;
using T = text::Id;
namespace {
constexpr auto kKey = pulse::seven_zip::kPreferencesKey;
constexpr const char* kHandlerId = "archive-open";

std::string Copy(PulseStringView value) { return value.length ? std::string(value.data, value.length) : std::string{}; }

bool LocalAbsolute(std::string_view path) {
    if (path.find_first_of("\r\n") != std::string_view::npos || path.find('\0') != std::string_view::npos) return false;
    if (path.starts_with("\\\\?\\")) path.remove_prefix(4);
    return path.size() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

bool Digit(const std::string& value, char max) { return value.size() == 1 && value[0] >= '0' && value[0] <= max; }
constexpr int kCommandCount = static_cast<int>(pulse::seven_zip::kMenuCommands.size());
constexpr int kAllCommands = static_cast<int>(pulse::seven_zip::kAllMenuCommands);
int HexDigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
std::string Hex2(int value) {
    constexpr char kDigits[] = "0123456789abcdef";
    return {kDigits[(value >> 4) & 15], kDigits[value & 15]};
}
std::string Hex3(unsigned value) {
    constexpr char kDigits[] = "0123456789abcdef";
    return {kDigits[(value >> 8) & 15], kDigits[(value >> 4) & 15], kDigits[value & 15]};
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(std::string_view value) {
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}

struct FormatLists {
    std::vector<std::string> rw;
    std::vector<std::string> ro;
    std::string rw_csv;
    std::string ro_csv;
};

FormatLists CollectSupportedFormats() {
    FormatLists out;
    for (const auto& line : engine::SupportedFormats()) {
        const auto name = line.substr(0, line.find(L':'));
        std::string utf8 = Utf8(name);
        for (auto& c : utf8) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (utf8.empty()) continue;
        const bool writable = line.size() >= 3 && line.compare(line.size() - 3, 3, L"(+)") == 0;
        auto& list = writable ? out.rw : out.ro;
        auto& csv = writable ? out.rw_csv : out.ro_csv;
        list.push_back(utf8);
        csv += (csv.empty() ? "" : ",") + utf8;
    }
    return out;
}
} // namespace

struct Settings::State {
    pulse::sdk::SettingsPagesClient pages;
    std::optional<pulse::sdk::AssociationsClient> assoc_client;
    pulse::sdk::AssociationsClient::Request assoc_request;
    std::vector<pulse::sdk::AssociationStatusData> last_sdk_items;
    PulseUiModelV1 model{};
    PulseUiEventsV1 events{};
    PulseUiModelV1 formats_model{};
    // 接管压缩包: its own settings tab (own state budget), drawn by the host from SDK components only.
    PulseUiModelV1 take_model{};
    PulseUiEventsV1 take_events{};
    uint64_t take_page = 0;
    pulse::sdk::Subscription take_edits;
    PulseResourcesV1 resources{};
    PulseStagingV1 staging{};
    std::function<void()> changed;
    uint64_t page = 0, formats_page = 0, version = 0;
    PulseResourcesRequestId request = 0;
    // Saved values, as the page shows them.
    std::map<std::string, std::string> values{{"mode", "0"}, {"location", "0"}, {"folder", ""}, {"format", "0"},
                                              {"level", "3"}, {"remember", "true"}, {"placement", "0"},
                                              {"commands", Hex2(kAllCommands)},
                                              {"take", "false"}, {"dbl", "0"}, {"inc", Hex3(association::kAllGroups)}};
    bool loading = false, saving = false, loaded = false, dirty = false;
    pulse::sdk::Subscription browse, edits;
    // Legacy direct-registry worker OR SDK Default Apps watch poller.
    pulse::sdk::DispatchQueue* dispatch = nullptr;
    std::array<pulse::sdk::Subscription, association::kGroupCount + 2> take_commands;
    std::vector<association::Status> statuses = std::vector<association::Status>(association::kGroupCount);
    std::thread worker;
    std::atomic<bool> stop{false};
    unsigned job = 0;
    bool busy = false;
    unsigned chain_before = 0;
    int chain = -1; // 逐个确认: the group whose chooser was just shown

    ~State() {
        StopWorker();
        assoc_request.Cancel();
        if (request) resources.cancel_request(resources.context, request);
        browse.Reset();
        edits.Reset();
        for (auto& command : take_commands) command.Reset();
        take_edits.Reset();
        if (take_page) pages.Remove(take_page);
        if (formats_page) pages.Remove(formats_page);
        if (page) pages.Remove(page);
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
    int Commands() const {
        const auto& hex = values.at("commands");
        return hex.size() == 2 && HexDigit(hex[0]) >= 0 && HexDigit(hex[1]) >= 0
            ? (HexDigit(hex[0]) << 4) | HexDigit(hex[1])
            : kAllCommands;
    }

    enum class Job { Query, Refresh, Apply, Remove };

    void Run(Job kind, bool watch = false) {
        if (!dispatch) return;
        StopWorker();
        const unsigned groups = Groups(), serial = ++job;
        const bool extract = values.at("dbl") != "1", taking = Taking();
        if (kind == Job::Apply || kind == Job::Remove) {
            busy = true;
            DeriveTake();
            PublishHeaderStatus(PULSE_SETTINGS_STATUS_SAVING, text::Get(T::AutoSaved));
        }

        if (assoc_client && assoc_client->Available()) {
            RunSdk(kind, watch, groups, extract, taking, serial);
            return;
        }

        worker = std::thread([this, kind, watch, groups, extract, taking, serial] {
            bool ok = true;
            if (kind == Job::Apply || kind == Job::Refresh) ok = association::Apply(groups, extract);
            else if (kind == Job::Remove) ok = association::Remove();
            auto result = association::Query(taking ? groups : 0);
            auto posted = result;
            dispatch->Post([this, serial, kind, ok, posted = std::move(posted)]() mutable {
                Finished(serial, kind, ok, std::move(posted));
            });
            for (int tick = 0; watch && tick < 300 && !stop; ++tick) {
                Sleep(200);
                if (tick % 10 != 9 || stop) continue;
                auto again = association::Query(taking ? groups : 0);
                bool same = again.size() == result.size();
                for (size_t i = 0; same && i < again.size(); ++i) {
                    same = again[i].owner == result[i].owner && again[i].who == result[i].who &&
                           again[i].pulse_count == result[i].pulse_count && again[i].windows == result[i].windows;
                }
                if (same) continue;
                result = again;
                dispatch->Post([this, serial, again = std::move(again)]() mutable {
                    Finished(serial, Job::Query, true, std::move(again));
                });
            }
        });
    }

    std::vector<association::Status> AggregateSdkStatuses(const std::vector<pulse::sdk::AssociationStatusData>& items,
                                                          unsigned active_groups) const {
        std::vector<association::Status> result(association::kGroupCount);
        std::vector<bool> free_group(association::kGroupCount, false);
        std::vector<bool> unknown_group(association::kGroupCount, false);
        const auto specs = association::AllExtensions();
        std::map<std::string, int> ext_to_group;
        for (const auto& s : specs) ext_to_group[s.extension] = s.group;

        for (const auto& item : items) {
            const auto git = ext_to_group.find(item.extension);
            if (git == ext_to_group.end()) continue;
            const int group = git->second;
            if (group < 0 || group >= association::kGroupCount) continue;
            auto& out = result[static_cast<size_t>(group)];
            switch (item.owner) {
            case PULSE_ASSOC_OWNER_PULSE:
                ++out.pulse_count;
                break;
            case PULSE_ASSOC_OWNER_FREE:
            case PULSE_ASSOC_OWNER_UNREGISTERED:
                free_group[static_cast<size_t>(group)] = true;
                break;
            case PULSE_ASSOC_OWNER_UNKNOWN:
                unknown_group[static_cast<size_t>(group)] = true;
                if ((item.flags & PULSE_ASSOC_STATUS_FLAG_WINDOWS_DEFAULT) != 0) out.windows = true;
                break;
            case PULSE_ASSOC_OWNER_OTHER:
                if (out.who.empty()) {
                    out.who = item.current_app.empty() ? L"Other" : Wide(item.current_app);
                    out.windows = (item.flags & PULSE_ASSOC_STATUS_FLAG_WINDOWS_DEFAULT) != 0;
                }
                break;
            default:
                unknown_group[static_cast<size_t>(group)] = true;
                break;
            }
        }
        for (int group = 0; group < association::kGroupCount; ++group) {
            auto& out = result[static_cast<size_t>(group)];
            if (!((active_groups >> group) & 1u)) out.owner = association::Owner::Off;
            else if (unknown_group[static_cast<size_t>(group)]) out.owner = association::Owner::Unknown;
            else if (!out.who.empty()) out.owner = association::Owner::Other;
            else if (free_group[static_cast<size_t>(group)]) out.owner = association::Owner::Free;
            else out.owner = association::Owner::Pulse;
        }
        return result;
    }

    void QuerySdk(unsigned serial, Job origin_kind, bool origin_ok, unsigned active_groups, bool start_watch) {
        if (!assoc_client) return;
        std::vector<std::string> all_exts;
        const auto specs = association::AllExtensions();
        all_exts.reserve(specs.size());
        for (const auto& spec : specs) all_exts.push_back(spec.extension);

        const PulseStatus qst = assoc_client->Query(all_exts, assoc_request,
            [this, serial, origin_kind, origin_ok, active_groups, start_watch](
                PulseStatus st, const std::vector<pulse::sdk::AssociationStatusData>& items) {
                if (serial != job) return;
                if (st == PULSE_OK) last_sdk_items = items;
                auto aggregated = st == PULSE_OK
                    ? AggregateSdkStatuses(items, active_groups)
                    : std::vector<association::Status>(association::kGroupCount);
                Finished(serial, origin_kind, origin_ok && st == PULSE_OK, std::move(aggregated));
                if (start_watch && !stop && dispatch) {
                    StartSdkWatch(serial, active_groups);
                }
            });
        if (qst != PULSE_OK) {
            Finished(serial, origin_kind, false, std::vector<association::Status>(association::kGroupCount));
        }
    }

    void StartSdkWatch(unsigned serial, unsigned active_groups) {
        StopWorker();
        worker = std::thread([this, serial, active_groups] {
            for (int tick = 0; tick < 30 && !stop; ++tick) {
                for (int step = 0; step < 10 && !stop; ++step) Sleep(200);
                if (stop) break;
                dispatch->Post([this, serial, active_groups] {
                    if (serial != job || busy || !assoc_client) return;
                    std::vector<std::string> all_exts;
                    const auto specs = association::AllExtensions();
                    all_exts.reserve(specs.size());
                    for (const auto& s : specs) all_exts.push_back(s.extension);
                    (void)assoc_client->Query(all_exts, assoc_request,
                        [this, serial, active_groups](PulseStatus st,
                                                      const std::vector<pulse::sdk::AssociationStatusData>& items) {
                            if (serial != job || st != PULSE_OK) return;
                            last_sdk_items = items;
                            Finished(serial, Job::Query, true, AggregateSdkStatuses(items, active_groups));
                        });
                });
            }
        });
    }

    void RunSdk(Job kind, bool watch, unsigned groups, bool extract, bool taking, unsigned serial) {
        const unsigned active_groups = taking ? groups : 0u;
        if (kind == Job::Query) {
            QuerySdk(serial, Job::Query, true, active_groups, watch);
            return;
        }
        // Migrate legacy direct-HKCU takeover records cleanly before host associations manage HKCU.
        if (takeover::Recorded()) takeover::RemoveAll();

        if (kind == Job::Remove || !taking || groups == 0u) {
            const PulseStatus st = assoc_client->RemoveAll(assoc_request,
                [this, serial, kind, active_groups, watch](PulseStatus result_st) {
                    if (serial != job) return;
                    QuerySdk(serial, kind, result_st == PULSE_OK, active_groups, watch);
                });
            if (st != PULSE_OK) {
                QuerySdk(serial, kind, false, active_groups, false);
            }
            return;
        }

        const auto specs = association::AllExtensions();
        const std::string verb = text::Get(extract ? T::CmdSmart : T::VerbPreview);
        std::vector<pulse::sdk::AssociationEntrySpec> entries;
        entries.reserve(specs.size());
        for (const auto& spec : specs) {
            if (((groups >> spec.group) & 1u) == 0u) continue;
            pulse::sdk::AssociationEntrySpec entry;
            entry.action = PULSE_ASSOC_ACTION_COMMAND;
            entry.handler_id = kHandlerId;
            entry.extension = spec.extension;
            entry.display_name = spec.display_name;
            entry.verb_label = verb;
            entry.icon_resource = "pulse_7zip.dll,-" + std::to_string(spec.icon);
            entries.push_back(std::move(entry));
        }
        const PulseStatus st = assoc_client->Apply(entries, assoc_request,
            [this, serial, kind, active_groups, watch](PulseStatus result_st) {
                if (serial != job) return;
                QuerySdk(serial, kind, result_st == PULSE_OK, active_groups, watch);
            });
        if (st != PULSE_OK) {
            QuerySdk(serial, kind, false, active_groups, false);
        }
    }

    void Finished(unsigned serial, Job kind, bool ok, std::vector<association::Status> result) {
        if (serial != job) return;
        busy = false;
        statuses = std::move(result);
        DeriveTake();
        if ((kind == Job::Apply || kind == Job::Refresh || kind == Job::Remove) && !ok) {
            Status(text::Get(T::TakeFailed));
        } else if (kind == Job::Apply || kind == Job::Refresh || kind == Job::Remove) {
            Status(text::Get(T::SettingsSaved));
        }
        // 逐个确认: once a format became Pulse's, Windows' chooser comes up for the next one.
        if (chain >= 0) {
            const int done = chain;
            chain = -1;
            if (statuses[static_cast<size_t>(done)].pulse_count > chain_before) {
                const int next = NextAsk();
                if (next >= 0) dispatch->Post([this, next] { Choose(next, true); });
            }
        }
    }

    int NextAsk() const {
        if (!Taking()) return -1;
        for (int i = 0; i < association::kGroupCount; ++i)
            if (statuses[static_cast<size_t>(i)].owner != association::Owner::Pulse &&
                statuses[static_cast<size_t>(i)].owner != association::Owner::Off) {
                return i;
            }
        return -1;
    }

    std::string FirstAskableExtension(int group) const {
        const auto specs = association::AllExtensions();
        std::string fallback;
        for (const auto& spec : specs) {
            if (spec.group != group) continue;
            if (fallback.empty()) fallback = spec.extension;
            bool pulse_owned = false;
            for (const auto& item : last_sdk_items) {
                if (item.extension == spec.extension && item.owner == PULSE_ASSOC_OWNER_PULSE) {
                    pulse_owned = true;
                    break;
                }
            }
            if (!pulse_owned) return spec.extension;
        }
        return fallback;
    }

    void Choose(int group, bool chained) {
        if (busy || !Taking() || group < 0 || group >= association::kGroupCount) return;
        chain_before = statuses[static_cast<size_t>(group)].pulse_count;
        if (assoc_client && assoc_client->Available()) {
            const std::string ext = FirstAskableExtension(group);
            if (!ext.empty()) (void)assoc_client->OpenChooser(ext);
        } else {
            association::Choose(GetActiveWindow(), group);
        }
        chain = chained ? group : -1;
        Run(Job::Query);
    }

    void SetTake(const std::string& key, const std::string& value) {
        if (!take_page || !take_model.set_states) return;
        const PulseUiState update{View(key), View(value)};
        take_model.set_states(take_model.context, take_page, &update, 1);
    }

    // 接管压缩包: who opens each format now, as rows of the plugin's own tab. The plugin owns every
    // string and state here; the host only renders the SDK components (no 7-Zip-specific host code).
    void DeriveTake() {
        if (!take_page) return;
        const bool taking = Taking(), extract = values.at("dbl") != "1";
        const unsigned groups = Groups();
        int ours = 0, asks = 0;
        for (int i = 0; i < association::kGroupCount; ++i) {
            const auto& status = statuses[static_cast<size_t>(i)];
            const auto owner = taking ? status.owner : association::Owner::Off;
            const std::string who = Utf8(status.who).substr(0, 60);
            std::string line;
            switch (owner) {
            case association::Owner::Pulse: line = text::Get(extract ? T::TakeStatusExtract : T::TakeStatusOpen); ++ours; break;
            case association::Owner::Other: line = text::Format(T::TakeStatusOther, who); ++asks; break;
            case association::Owner::Free: line = text::Get(T::TakeStatusFree); ++asks; break;
            case association::Owner::Unknown: line = text::Get(T::TakeStatusUnknown); ++asks; break;
            default:
                line = !taking ? (who.empty() ? text::Get(T::TakeStatusOff) : text::Format(T::TakeStatusOffWho, who))
                               : (who.empty() ? text::Get(T::TakeStatusExcluded) : text::Format(T::TakeStatusExcludedWho, who));
                break;
            }
            const bool ask = taking && !busy && (owner == association::Owner::Other || owner == association::Owner::Free ||
                                                 owner == association::Owner::Unknown);
            const bool set_label = status.windows || owner == association::Owner::Free || owner == association::Owner::Unknown;
            const auto n = std::to_string(i);
            SetTake("st" + n, std::string(association::GroupExtensions(i)) + " · " + line);
            SetTake("inc" + n, (groups >> i) & 1u ? "true" : "false");
            SetTake("askw" + n, ask && set_label ? "true" : "false");
            SetTake("askc" + n, ask && !set_label ? "true" : "false");
        }
        const bool ask_any = taking && !busy && asks > 0;
        SetTake("take", taking ? "true" : "false");
        SetTake("editable", taking && !busy ? "true" : "false");
        SetTake("idle", busy ? "false" : "true");
        SetTake("askany", ask_any ? "true" : "false");
        SetTake("askmany", ask_any && asks > 2 ? "true" : "false");
        SetTake("dbl0", extract ? "true" : "false");
        SetTake("dbl1", extract ? "false" : "true");
        SetTake("takehead", busy ? text::Get(T::TakeBusy)
            : !taking ? text::Get(T::TakeOffHint)
            : asks ? text::Format(T::TakeSummaryAsk, std::to_string(asks), std::to_string(ours))
                   : text::Format(T::TakeSummaryDone, {}, std::to_string(ours)));
    }

    // One edit of the 接管压缩包 tab: the switch, a format's checkbox or the double-click choice.
    void ApplyTake(bool take, const std::string& dbl, const std::string& inc) {
        std::optional<Job> takeover;
        if (take != Taking()) takeover = take ? Job::Apply : Job::Remove;
        else if (take && (dbl != values["dbl"] || inc != values["inc"])) takeover = Job::Apply;
        const bool changed_value = take != Taking() || dbl != values["dbl"] || inc != values["inc"];
        values["take"] = take ? "true" : "false";
        values["dbl"] = dbl;
        values["inc"] = inc;
        if (takeover) Run(*takeover);
        else DeriveTake();
        if (changed_value) {
            Save();
            if (changed) changed();
        }
    }

    void TakeEdited(const PulseUiStateBatch& batch) {
        if (busy || !loaded) { DeriveTake(); return; }
        bool take = Taking();
        std::string dbl = values["dbl"];
        unsigned groups = Groups();
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto key = Copy(batch.changes[i].key), value = Copy(batch.changes[i].value);
            const bool flag = value == "true";
            if (value != "true" && value != "false") continue;
            if (key == "take") take = flag;
            else if ((key == "dbl0" || key == "dbl1") && flag) dbl.assign(1, key.back());
            else if (key.size() >= 4 && key.size() <= 5 && key.compare(0, 3, "inc") == 0) {
                const int group = std::atoi(key.c_str() + 3);
                if (group >= 0 && group < association::kGroupCount && std::to_string(group) == key.substr(3))
                    groups = flag ? groups | (1u << group) : groups & ~(1u << group);
            }
        }
        ApplyTake(take, dbl, Hex3(groups & association::kAllGroups));
    }

    void PublishHeaderStatus(uint32_t kind, const std::string& text) {
        if (pages.HasV2() && page) (void)pages.SetStatus(page, kind, text);
    }

    void Set(const std::string& key, const std::string& value) {
        if (!page || !model.set_states) return;
        const PulseUiState update{View(key), View(value)};
        model.set_states(model.context, page, &update, 1);
    }

    void Status(const std::string& value) {
        Set("status", value);
        uint32_t kind = PULSE_SETTINGS_STATUS_SAVED;
        if (value == text::Get(T::TakeFailed) || value == text::Get(T::SettingsFailed) ||
            value == text::Get(T::SettingsBadPath)) {
            kind = PULSE_SETTINGS_STATUS_WARNING;
        } else if (saving || busy) {
            kind = PULSE_SETTINGS_STATUS_SAVING;
        } else if (value.empty()) {
            kind = PULSE_SETTINGS_STATUS_NONE;
        }
        PublishHeaderStatus(kind, value);
    }

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
            const auto prefs = pulse::seven_zip::ParseCommandPreferences(value);
            if (!prefs) continue;
            values["mode"] = std::to_string(prefs->mode);
            values["location"] = std::to_string(prefs->location);
            values["format"] = std::to_string(prefs->format);
            values["level"] = std::to_string(prefs->level);
            values["remember"] = prefs->remember ? "true" : "false";
            values["placement"] = prefs->submenu ? "1" : "0";
            values["commands"] = Hex2(static_cast<int>(prefs->commands));
            values["take"] = prefs->takeover ? "true" : "false";
            values["dbl"] = prefs->preview_on_open ? "1" : "0";
            values["inc"] = prefs->groups;
            values["folder"] = prefs->folder;
        }
        version = settings.version;
    }

    void Load() {
        if (request) return;
        loading = true;
        if (resources.settings_get(resources.context, Completed, this, &request) != PULSE_OK) {
            request = 0;
            loading = false;
            Status(text::Get(T::SettingsFailed));
            if (!loaded && changed) changed();
        }
    }

    void Save() {
        if (!loaded) return;
        if (request) { dirty = true; return; }
        dirty = false;
        saving = true;
        PublishHeaderStatus(PULSE_SETTINGS_STATUS_SAVING, text::Get(T::AutoSaved));
        const auto data = Encoded();
        if (resources.settings_set(resources.context, View(kKey), View(data), 0, version, Completed, this, &request) != PULSE_OK) {
            request = 0;
            saving = false;
            Status(text::Get(T::SettingsFailed));
        }
    }

    static void PULSE_CALL Completed(void* user, const PulseResourcesReply* reply) noexcept {
        auto& self = *static_cast<State*>(user);
        try {
            self.request = 0;
            const bool save = self.saving;
            self.loading = self.saving = false;
            if (reply->status != PULSE_OK) {
                if (save && Copy(reply->error) == "version_conflict") {
                    self.dirty = true;
                    self.Load();
                    return;
                }
                self.Status(text::Get(T::SettingsFailed));
                if (!self.loaded && self.changed) self.changed();
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
                self.PublishHeaderStatus(PULSE_SETTINGS_STATUS_SAVED, text::Get(T::AutoSaved));
                if (first) self.Run(self.Taking() ? Job::Refresh : Job::Query);
            }
            if (self.dirty) self.Save();
            if (self.changed) self.changed();
        } catch (...) {
            self.loading = self.saving = false;
            self.Status(text::Get(T::SettingsFailed));
            if (!self.loaded && self.changed) self.changed();
        }
    }

    void Edited(const PulseUiStateBatch& batch) {
        bool save = false;
        for (uint32_t i = 0; i < batch.count; ++i) {
            const auto key = Copy(batch.changes[i].key), value = Copy(batch.changes[i].value);
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
                       (key == "placement" && Digit(value, '1')) ||
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

    void Derive() {
        const int mode = values["mode"][0] - '0', format = values["format"][0] - '0';
        constexpr const char* kFormatNames[] = {"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
        const bool fixed = values["location"] == "1";
        Set("modedesc", text::Get(mode == 0 ? T::ModeSmartDesc : mode == 1 ? T::ModeFolderDesc : T::ModeHereDesc));
        Set("quickdesc", text::Format(T::QuickFormatDesc, kFormatNames[format]));
        Set("fixed", fixed ? "true" : "false");
        const auto first = fixed && !values["folder"].empty() ? T::CmdFixed
            : mode == 1 ? T::CmdFolder : mode == 2 ? T::CmdHere : T::CmdSmart;
        Set("menushows", text::Format(T::MenuShows, text::Get(first)));
        for (const auto& [group, count] : {std::pair<const char*, int>{"mode", 3}, {"location", 2}, {"placement", 2}})
            for (int i = 0; i < count; ++i)
                Set(group + std::to_string(i), values[group][0] - '0' == i ? "true" : "false");
        const int shown = Commands();
        for (int i = 0; i < kCommandCount; ++i) Set("cmd" + std::to_string(i), (shown >> i) & 1 ? "true" : "false");
        UpdateSidePreview();
    }

    pulse::sdk::ModelTree BuildSidePreviewTree() const {
        using pulse::sdk::ModelMenuItem;
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;

        const int mode = values.at("mode")[0] - '0';
        const int format = values.at("format")[0] - '0';
        const bool fixed = values.at("location") == "1";
        const auto smart_label = fixed && !values.at("folder").empty() ? T::CmdFixed
            : mode == 1 ? T::CmdFolder : mode == 2 ? T::CmdHere : T::CmdSmart;
        constexpr const char* kFormatNames[] = {"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
        const std::string quick_label = text::Format(T::CmdQuick, kFormatNames[format]);

        const int shown = kAllCommands;  // which commands show is set in the host's 右键菜单 tab
        const int level = values.at("level")[0] - '0';
        constexpr T kLevels[] = {T::Level0, T::Level1, T::Level3, T::Level5, T::Level7, T::Level9};
        const std::string extract_detail = fixed && !values.at("folder").empty()
            ? text::Format(T::MenuDetailFixed, values.at("folder"))
            : text::Get(mode == 1 ? T::MenuDetailFolder : mode == 2 ? T::MenuDetailHere : T::MenuDetailSmart);
        const bool submenu = values.at("placement") == "1";
        // archive=true: what right-clicking an archive shows; false: an ordinary file, where only the
        // compress and checksum commands apply. Item ids carry the prefix so both menus coexist.
        auto build_menu = [&](const std::string& p, bool archive) {
            std::vector<ModelMenuItem> entries;
            if (archive && ((shown >> 0) & 1)) entries.push_back(ModelMenuItem(p + "extract", text::Get(smart_label)).Glyph("icon:extract"));
            if (archive && ((shown >> 1) & 1)) entries.push_back(ModelMenuItem(p + "to", text::Get(T::CmdTo)).Glyph("icon:extract-to"));
            if (archive && ((shown >> 2) & 1)) entries.push_back(ModelMenuItem(p + "test", text::Get(T::CmdTest)).Glyph("icon:test"));
            if ((shown >> 3) & 1) entries.push_back(ModelMenuItem(p + "quick", quick_label).Glyph("icon:archive"));
            if ((shown >> 4) & 1) entries.push_back(ModelMenuItem(p + "add", text::Get(T::CmdAdd)).Glyph("icon:add-archive"));
            if ((shown >> 5) & 1) entries.push_back(ModelMenuItem(p + "hash", text::Get(T::CmdHash)).Glyph("icon:hash"));
            // The first command is the group's main action: semibold, with what it will do underneath.
            if (!entries.empty()) {
                entries.front().Primary();
                if (archive && (shown & 1)) entries.front().Detail(extract_detail);
                else if (((shown >> 3) & 1) && (!archive || (shown & 0x7) == 0))
                    entries.front().Detail(text::Format(T::MenuDetailQuick, kFormatNames[format],
                                                        text::Get(kLevels[std::clamp(level, 0, 5)])));
            }
            std::vector<ModelMenuItem> root_menu;
            if (entries.empty()) {
                root_menu.push_back(ModelMenuItem(p + "none", text::Get(T::MenuSection)).Enabled(false));
            } else if (submenu) {
                entries.push_back(ModelMenuItem::Separator(p + "sub-sep"));
                entries.push_back(ModelMenuItem(p + "sub-settings", text::Get(T::MenuSettings)).Glyph("icon:gear"));
                root_menu.push_back(ModelMenuItem(p + "group", "7-Zip").Glyph("icon:box").Group("7-Zip").Children(std::move(entries)));
            } else {
                for (auto& entry : entries) entry.Group("7-Zip");
                root_menu = std::move(entries);
            }
            return root_menu;
        };

        auto stack = ModelNode(PULSE_UI_MODEL_STACK, "side-preview-stack").Int(&N::gap, 10).Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "side-preview-title").String(&N::text, text::Get(T::SidePreviewTitle)).String(&N::role, "section"),
            ModelNode(PULSE_UI_MODEL_TEXT, "side-preview-detail").String(&N::text, text::Get(T::SidePreviewDetail)).String(&N::role, "caption"),
            ModelNode(PULSE_UI_MODEL_TEXT, "side-preview-archive").String(&N::text, text::Get(T::PreviewArchive)).String(&N::role, "bodyStrong"),
            ModelNode::MenuPreview("side-preview-menu", {}, build_menu("prev-", true)).Kind("context"),
            ModelNode(PULSE_UI_MODEL_TEXT, "side-preview-file").String(&N::text, text::Get(T::PreviewFile)).String(&N::role, "bodyStrong"),
            ModelNode::MenuPreview("side-preview-file-menu", {}, build_menu("prevf-", false)).Kind("context"),
        });
        return ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "side-preview-root").Children({std::move(stack)}));
    }

    void UpdateSidePreview() {
        if (!pages.HasV2() || !page) return;
        auto preview = BuildSidePreviewTree();
        (void)pages.SetSidePreview(page, &preview.Build(), 300);
    }

    pulse::sdk::ModelTree BuildFormatsTree(const FormatLists& fmts) const {
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;

        const auto make_chips_card = [&](const std::string& prefix, const std::string& title,
                                         const std::vector<std::string>& items, std::string color) {
            std::vector<ModelNode> rows;
            constexpr size_t kPerRow = 6;
            const size_t limit = std::min<size_t>(items.size(), 64);
            for (size_t start = 0; start < limit; start += kPerRow) {
                std::vector<ModelNode> line;
                const size_t end = std::min<size_t>(limit, start + kPerRow);
                for (size_t i = start; i < end; ++i) {
                    ModelNode chip(PULSE_UI_MODEL_TAG, prefix + "-chip-" + std::to_string(i));
                    chip.String(&N::text, items[i]).Bool(&N::bordered, true);
                    if (!color.empty()) chip.String(&N::color, color);
                    line.push_back(std::move(chip));
                }
                rows.push_back(ModelNode(PULSE_UI_MODEL_STACK, prefix + "-row-" + std::to_string(start))
                                   .String(&N::orientation, "horizontal")
                                   .Int(&N::gap, 6)
                                   .Children(std::move(line)));
            }
            if (rows.empty()) {
                rows.push_back(ModelNode(PULSE_UI_MODEL_TEXT, prefix + "-empty").String(&N::text, "-").String(&N::role, "caption"));
            }
            return ModelNode(PULSE_UI_MODEL_GROUP, prefix + "-group")
                .String(&N::text, title)
                .Bool(&N::bordered, true)
                .Int(&N::gap, 8)
                .Children(std::move(rows));
        };

        auto content = ModelNode(PULSE_UI_MODEL_STACK, "formats-content").Int(&N::gap, 12).Children({
            make_chips_card("rw", text::Get(T::FormatsRwTitle), fmts.rw, "accent"),
            make_chips_card("ro", text::Get(T::FormatsRoTitle), fmts.ro, {}),
        });
        return ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "formats-root").Children({std::move(content)}));
    }

    pulse::sdk::ModelTree BuildTree(const FormatLists& fmts) const {
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;
        const auto caption = [](const char* id, const std::string& value) {
            return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "caption");
        };
        const auto section = [](const char* id, const std::string& value) {
            return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "section");
        };
        const auto card = [](const char* id, std::vector<ModelNode> children) {
            return ModelNode(PULSE_UI_MODEL_GROUP, id).Bool(&N::bordered, true).Int(&N::gap, 12).Children(std::move(children));
        };
        const auto ready = [](ModelNode node) { return node.String(&N::enabled_binding, "state.ready"); };

        std::vector<std::string> levels;
        for (const auto id : {T::Level0, T::Level1, T::Level3, T::Level5, T::Level7, T::Level9}) levels.push_back(text::Get(id));

        auto hero = card("hero", {
            section("hero-title", text::Get(T::HeroTitle)),
            caption("hero-detail", text::Format(T::HeroDetail, std::to_string(fmts.rw.size() + fmts.ro.size()))),
            ModelNode(PULSE_UI_MODEL_TEXT, "settings-status").String(&N::binding, "state.status").String(&N::role, "caption"),
        }).Int(&N::gap, 2);

        // Extract mode as three illustrated tiles: the archive, an arrow, and what lands on disk.
        const auto art = [](const char* id, int kind) {
            pulse::sdk::CanvasBuilder canvas(id);
            canvas.ViewBox(150, 56).MinHeight(64)
                .Icon("archive", 6, 13, 30, "secondary")
                .Path("none", "accent", 1.6, "M42 28h18m-5-5 5 5-5 5")
                .Rect(66, 6, 78, 44, 5, "card", "border", 1);
            if (kind == 0) {
                canvas.Icon("file", 73, 11, 16, "secondary").Rect(94, 17, 40, 4, 2, "border")
                    .Icon("folder", 73, 30, 16, "accent").Rect(94, 36, 32, 4, 2, "border");
            } else if (kind == 1) {
                canvas.Icon("folder", 71, 9, 22, "accent").Rect(97, 18, 36, 4, 2, "border")
                    .Path("none", "border", 1, "M81 33v9h8")
                    .Icon("file", 90, 35, 12, "secondary").Icon("image", 104, 35, 12, "secondary")
                    .Icon("file", 118, 35, 12, "secondary");
            } else {
                canvas.Icon("folder", 72, 12, 18, "secondary").Icon("file", 95, 12, 18, "secondary")
                    .Icon("image", 118, 12, 18, "secondary").Icon("file", 84, 31, 16, "secondary")
                    .Icon("file", 106, 31, 16, "secondary");
            }
            return canvas.BuildNode();
        };
        const auto tile = [&](const char* id, T title, T detail, int kind, const char* badge) {
            auto node = ModelNode::OptionCard(id, text::Get(title), text::Get(detail), id, "radio", {}, badge ? badge : "");
            node.Int(&N::min_width, 136).Children({art((std::string(id) + "-art").c_str(), kind)});
            return ready(std::move(node));
        };
        auto mode_choices = ModelNode(PULSE_UI_MODEL_STACK, "mode-choices").Int(&N::gap, 8).Children({
            ModelNode(PULSE_UI_MODEL_TEXT, "mode-title").String(&N::text, text::Get(T::ModeTitle)),
            caption("mode-desc", text::Get(T::ModeDesc)),
            ModelNode(PULSE_UI_MODEL_GRID, "mode-tiles").Int(&N::columns, 3).Int(&N::gap, 10).Children({
                tile("mode0", T::ModeSmart, T::ModeSmartDesc, 0, text::Get(T::Recommended)),
                tile("mode1", T::ModeFolder, T::ModeFolderDesc, 1, nullptr),
                tile("mode2", T::ModeHere, T::ModeHereDesc, 2, nullptr),
            }),
        });

        auto extract = card("extract-card", {
            std::move(mode_choices),
            ModelNode(PULSE_UI_MODEL_TEXT, "menu-shows").String(&N::binding, "state.menushows").String(&N::role, "caption"),
            ModelNode(PULSE_UI_MODEL_DIVIDER, "extract-divider"),
            ModelNode::SettingsRow("location", text::Get(T::LocationTitle), {},
                ready(ModelNode(PULSE_UI_MODEL_SEGMENTED, "location-input").String(&N::state, "location")
                    .Options({text::Get(T::LocationBeside), text::Get(T::LocationFixed)}).Int(&N::min_width, 220).Int(&N::max_width, 280)),
                "icon:path", false),
            ModelNode(PULSE_UI_MODEL_STACK, "folder-row").String(&N::orientation, "horizontal").Int(&N::gap, 8)
                .String(&N::visible_binding, "state.fixed").Children({
                    ready(ModelNode(PULSE_UI_MODEL_TEXT_INPUT, "folder-input").String(&N::text, text::Get(T::LocationFixed))
                        .String(&N::state, "folder").String(&N::placeholder, text::Get(T::SettingsDestinationHint)).Int(&N::flex, 1)),
                    ready(ModelNode(PULSE_UI_MODEL_BUTTON, "browse").String(&N::text, text::Get(T::SettingsBrowse)).String(&N::command, "browse")),
                }),
            ModelNode::SettingsRow("remember", text::Get(T::RememberTitle), text::Get(T::RememberDesc),
                ready(ModelNode(PULSE_UI_MODEL_SWITCH, "remember-input").String(&N::state, "remember").Int(&N::min_width, 48).Int(&N::max_width, 48)),
                "icon:lock", false),
        });

        auto compress = card("compress-card", {
            ModelNode::SettingsRow("format", text::Get(T::QuickFormatTitle), {},
                ready(ModelNode(PULSE_UI_MODEL_SEGMENTED, "format-input").String(&N::state, "format")
                    .Options({"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"}).Int(&N::min_width, 320).Int(&N::max_width, 340)),
                "icon:archive", false),
            ModelNode(PULSE_UI_MODEL_TEXT, "format-desc").String(&N::binding, "state.quickdesc").String(&N::role, "caption"),
            ModelNode::SettingsRow("level", text::Get(T::QuickLevelTitle), text::Get(T::QuickLevelDesc),
                ready(ModelNode(PULSE_UI_MODEL_SLIDER, "level-input").String(&N::state, "level").Int(&N::stops, 6).Options(levels)
                    .Int(&N::min_width, 300).Int(&N::max_width, 340)),
                "icon:gear", false),
            caption("compress-note", text::Get(T::SettingsCompressNote)),
        });



        auto content = ModelNode(PULSE_UI_MODEL_STACK, "settings-content").Int(&N::gap, 12).Children({
            hero, section("extract-title", text::Get(T::SettingsExtractSection)), extract,
            section("compress-title", text::Get(T::SettingsCompressSection)), compress,
            caption("rar-note", text::Get(T::RarNote)),
        });

        const int mode = values.at("mode")[0] - '0';
        const int format = values.at("format")[0] - '0';
        constexpr const char* kFormatNames[] = {"7z", "zip", "tar", "tar.gz", "tar.xz", "tar.bz2"};
        const bool fixed = values.at("location") == "1";
        const auto first_cmd = fixed && !values.at("folder").empty() ? T::CmdFixed
            : mode == 1 ? T::CmdFolder : mode == 2 ? T::CmdHere : T::CmdSmart;
        const int shown = Commands();

        auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "settings-root").Children({content}))
            .State("formats_rw", fmts.rw_csv).State("formats_ro", fmts.ro_csv)
            .State("mode", values.at("mode")).State("location", values.at("location"))
            .State("folder", values.at("folder")).State("format", values.at("format"))
            .State("level", values.at("level")).State("remember", values.at("remember"))
            .State("fixed", fixed ? "true" : "false")
            .State("status", text::Get(T::AutoSaved))
            .State("ready", loaded ? "true" : "false")
            .State("modedesc", text::Get(mode == 0 ? T::ModeSmartDesc : mode == 1 ? T::ModeFolderDesc : T::ModeHereDesc))
            .State("quickdesc", text::Format(T::QuickFormatDesc, kFormatNames[format]))
            .State("menushows", text::Format(T::MenuShows, text::Get(first_cmd)))
            .State("mode0", mode == 0 ? "true" : "false").State("mode1", mode == 1 ? "true" : "false").State("mode2", mode == 2 ? "true" : "false")
            .State("location0", values.at("location") == "0" ? "true" : "false").State("location1", fixed ? "true" : "false")
            .State("placement0", values.at("placement") == "0" ? "true" : "false").State("placement1", values.at("placement") == "1" ? "true" : "false")
            .State("placement", values.at("placement")).State("commands", values.at("commands"))
            .State("cmd0", (shown >> 0) & 1 ? "true" : "false").State("cmd1", (shown >> 1) & 1 ? "true" : "false")
            .State("cmd2", (shown >> 2) & 1 ? "true" : "false").State("cmd3", (shown >> 3) & 1 ? "true" : "false")
            .State("cmd4", (shown >> 4) & 1 ? "true" : "false").State("cmd5", (shown >> 5) & 1 ? "true" : "false")
            .Command("browse");
        return tree;
    }

    // 接管压缩包 tab: a switch, one SettingsRow per format (status in its detail line, Windows'
    // chooser button, include switch) and the double-click choice as option cards.
    pulse::sdk::ModelTree BuildTakeTree() const {
        using pulse::sdk::ModelNode;
        using pulse::sdk::ModelTree;
        using N = PulseUiModelNode;
        const auto caption = [](const char* id, const std::string& value) {
            return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "caption");
        };
        const auto section = [](const char* id, const std::string& value) {
            return ModelNode(PULSE_UI_MODEL_TEXT, id).String(&N::text, value).String(&N::role, "section");
        };
        const auto card = [](const char* id, std::vector<ModelNode> children) {
            return ModelNode(PULSE_UI_MODEL_GROUP, id).Bool(&N::bordered, true).Int(&N::gap, 12).Children(std::move(children));
        };
        const auto toggle = [](const std::string& id, const std::string& state) {
            return ModelNode(PULSE_UI_MODEL_SWITCH, id).String(&N::state, state).Int(&N::min_width, 48).Int(&N::max_width, 48);
        };

        std::vector<ModelNode> rows{
            ModelNode::SettingsRow("take-row", text::Get(T::TakeTitle), text::Get(T::TakeDesc),
                toggle("take-input", "take").String(&N::enabled_binding, "state.idle"),
                "icon:box", false),
            ModelNode(PULSE_UI_MODEL_DIVIDER, "take-divider"),
            ModelNode(PULSE_UI_MODEL_STACK, "take-head-row").String(&N::orientation, "horizontal").Int(&N::gap, 8).Children({
                ModelNode(PULSE_UI_MODEL_TEXT, "take-head").String(&N::binding, "state.takehead").String(&N::role, "caption").Int(&N::flex, 1),
                ModelNode(PULSE_UI_MODEL_BUTTON, "take-winsettings").String(&N::text, text::Get(T::TakeWinSettings))
                    .String(&N::command, "winsettings").String(&N::visible_binding, "state.askmany"),
                ModelNode(PULSE_UI_MODEL_BUTTON, "take-next").String(&N::text, text::Get(T::TakeConfirmEach))
                    .String(&N::command, "asknext").String(&N::visible_binding, "state.askany"),
            }),
        };
        for (int i = 0; i < association::kGroupCount; ++i) {
            const auto n = std::to_string(i);
            ModelNode row(PULSE_UI_MODEL_SETTINGS_ROW, "grp" + n);
            row.String(&N::text, association::GroupLabel(i)).String(&N::detail, association::GroupExtensions(i))
                .String(&N::glyph, "icon:archive").Bool(&N::bordered, false).String(&N::binding, "state.st" + n);
            row.Children({
                ModelNode(PULSE_UI_MODEL_BUTTON, "askw" + n).String(&N::text, text::Get(T::TakeAskSet))
                    .String(&N::command, "ask" + n).String(&N::visible_binding, "state.askw" + n),
                ModelNode(PULSE_UI_MODEL_BUTTON, "askc" + n).String(&N::text, text::Get(T::TakeAskChange))
                    .String(&N::command, "ask" + n).String(&N::visible_binding, "state.askc" + n),
                toggle("inc" + n + "-input", "inc" + n).String(&N::enabled_binding, "state.editable"),
            });
            rows.push_back(std::move(row));
        }

        auto dbl = card("dbl-card", {
            ModelNode(PULSE_UI_MODEL_TEXT, "dbl-title").String(&N::text, text::Get(T::DblTitle)),
            caption("dbl-desc", text::Get(T::DblDesc)),
            ModelNode::OptionCard("dbl0", text::Get(T::TakeStatusExtract), text::Get(T::ModeSmartDesc), "dbl0", "radio", "icon:sparkle")
                .String(&N::enabled_binding, "state.editable"),
            ModelNode::OptionCard("dbl1", text::Get(T::DblOpen), text::Get(T::DblOpenDesc), "dbl1", "radio", "icon:folder")
                .String(&N::enabled_binding, "state.editable"),
        });

        auto content = ModelNode(PULSE_UI_MODEL_STACK, "take-content").Int(&N::gap, 12).Children({
            section("take-title", text::Get(T::TakeSection)), card("take-card", std::move(rows)),
            section("dbl-section", text::Get(T::DblTitle)), std::move(dbl),
        });
        auto tree = ModelTree(ModelNode(PULSE_UI_MODEL_SCROLL_VIEW, "take-root").Children({std::move(content)}))
            .State("take", Taking() ? "true" : "false").State("editable", "false").State("idle", "true")
            .State("askany", "false").State("askmany", "false")
            .State("dbl0", values.at("dbl") != "1" ? "true" : "false").State("dbl1", values.at("dbl") == "1" ? "true" : "false")
            .State("takehead", text::Get(T::TakeOffHint))
            .Command("asknext").Command("winsettings");
        const unsigned groups = Groups();
        for (int i = 0; i < association::kGroupCount; ++i) {
            const auto n = std::to_string(i);
            tree.State("st" + n, association::GroupExtensions(i)).State("inc" + n, (groups >> i) & 1u ? "true" : "false")
                .State("askw" + n, "false").State("askc" + n, "false").Command("ask" + n);
        }
        return tree;
    }

    void EnsureTakeTab() {
        if (!pages.HasV2()) return;
        for (auto& command : take_commands) command.Reset();
        take_edits.Reset();
        if (take_page) {
            (void)pages.Remove(take_page);
            take_page = 0;
            take_model = {};
            take_events = {};
        }
        auto tree = BuildTakeTree();
        uint64_t created = 0;
        if (pages.CreateTab("takeover", text::Get(T::TakeSection), tree.Build(), created, "icon:archive", 5) != PULSE_OK) return;
        PulseUiModelV1 created_model{};
        PulseUiEventsV1 created_events{};
        bool ok = pages.QueryModel(created, created_model) == PULSE_OK && pages.QueryEvents(created, created_events) == PULSE_OK;
        ok = ok && take_commands[0].Command(created_model, created, "asknext",
            [this](uint64_t, PulseStringView) { try { Choose(NextAsk(), true); } catch (...) {} }) == PULSE_OK;
        ok = ok && take_commands[1].Command(created_model, created, "winsettings", [this](uint64_t, PulseStringView) {
            try {
                if (Taking() && !busy) {
                    if (assoc_client && assoc_client->Available()) (void)assoc_client->OpenDefaultApps();
                    else association::OpenDefaultApps();
                    Run(Job::Query, true);
                }
            } catch (...) {}
        }) == PULSE_OK;
        for (int i = 0; ok && i < association::kGroupCount; ++i) {
            ok = take_commands[static_cast<size_t>(i) + 2].Command(created_model, created, "ask" + std::to_string(i),
                [this, i](uint64_t, PulseStringView) { try { Choose(i, false); } catch (...) {} }) == PULSE_OK;
        }
        ok = ok && take_edits.State(created_model, created_events, created, {}, PULSE_UI_STATE_USER,
            [this](uint64_t, const PulseUiStateBatch& batch) { try { TakeEdited(batch); } catch (...) {} }) == PULSE_OK;
        if (!ok) {
            for (auto& command : take_commands) command.Reset();
            take_edits.Reset();
            (void)pages.Remove(created);
            return;
        }
        take_page = created;
        take_model = created_model;
        take_events = created_events;
        DeriveTake();
    }

    void EnsureFormatsTab(const FormatLists& fmts) {
        if (!pages.HasV2()) return;
        if (formats_page) {
            (void)pages.Remove(formats_page);
            formats_page = 0;
            formats_model = {};
        }
        auto formats_tree = BuildFormatsTree(fmts);
        if (pages.CreateTab("formats", text::Get(T::TabFormats), formats_tree.Build(), formats_page, "icon:box", 10) == PULSE_OK) {
            (void)pages.QueryModel(formats_page, formats_model);
        } else {
            formats_page = 0;
        }
    }

    PulseStatus Create() {
        const auto fmts = CollectSupportedFormats();
        auto tree = BuildTree(fmts);
        auto result = pages.Create(text::Get(T::SettingsTitle), tree.Build(), page);
        if (result != PULSE_OK) return result;
        result = pages.QueryModel(page, model);
        if (result != PULSE_OK) return result;
        result = pages.QueryEvents(page, events);
        if (result != PULSE_OK) return result;
        result = browse.Command(model, page, "browse", [this](uint64_t, PulseStringView) { try { Browse(); } catch (...) {} });
        if (result != PULSE_OK) return result;
        result = edits.State(model, events, page, {}, PULSE_UI_STATE_USER,
                             [this](uint64_t, const PulseUiStateBatch& batch) { try { Edited(batch); } catch (...) {} });
        if (result != PULSE_OK) return result;

        UpdateSidePreview();
        EnsureTakeTab();
        EnsureFormatsTab(fmts);
        Load();
        return PULSE_OK;
    }

    void RefreshLanguage() {
        if (!page || !model.update) return;
        const auto fmts = CollectSupportedFormats();
        auto tree = BuildTree(fmts);
        if (model.update(model.context, page, &tree.Build()) == PULSE_OK) {
            Show();
            Status(text::Get(T::AutoSaved));
            EnsureTakeTab();
            EnsureFormatsTab(fmts);
        }
    }
};

Settings::Settings() : state_(std::make_unique<State>()) {}
Settings::~Settings() = default;

PulseStatus Settings::Initialize(const pulse::sdk::Host& host, PulseStagingV1 staging, pulse::sdk::DispatchQueue& dispatch,
                                 std::function<void()> changed) {
    state_->staging = staging;
    state_->dispatch = &dispatch;
    state_->changed = std::move(changed);
    const auto pages_st = state_->pages.Attach(host);
    if (pages_st != PULSE_OK) return pages_st;
    state_->assoc_client = pulse::sdk::AssociationsClient::TryQuery(host);
    const auto res_st = host.Query(PULSE_RESOURCES_INTERFACE_NAME, PULSE_RESOURCES_VERSION_1, state_->resources);
    return res_st == PULSE_OK ? state_->Create() : res_st;
}

void Settings::RefreshLanguage() { if (state_) state_->RefreshLanguage(); }
bool Settings::Ready() const { return state_->loaded; }
void Settings::EnsureLoaded() { if (!state_->loaded) state_->Load(); }
Settings::ExtractMode Settings::Mode() const { return static_cast<ExtractMode>(state_->values.at("mode")[0] - '0'); }
std::string Settings::FixedFolder() const { return state_->values.at("location") == "1" ? state_->values.at("folder") : std::string{}; }
int Settings::Format() const { return state_->values.at("format")[0] - '0'; }
int Settings::Level() const { return state_->values.at("level")[0] - '0'; }
bool Settings::RememberPasswords() const { return state_->values.at("remember") != "false"; }
bool Settings::MenuSubmenu() const { return state_->values.at("placement") == "1"; }
// Every command is registered and runs; whether it shows in the menu is the host's 右键菜单 tab
// (the old "commands" bits stay stored only so the host can carry them over once).
bool Settings::CommandShown(int index) const { return index >= 0 && index < kCommandCount; }
bool Settings::TakeOver() const { return state_->Taking(); }