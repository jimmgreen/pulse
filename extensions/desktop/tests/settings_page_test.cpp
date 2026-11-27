#include "plugin/settings_page.h"
#include "../../src/app/extensions/native/ui_model.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <map>
#include <set>

namespace {
std::function<void(const PulseUiModelTree*)> model_capture;
PulseStringView Sv(const std::string& s) { return {s.data(), static_cast<uint32_t>(s.size())}; }
std::string Str(PulseStringView v) { return {v.data, v.length}; }
// Every tree the page hands the host, run through Pulse's own validator (what the settings host does).
std::vector<std::string> g_validator_errors;
size_t g_validated_trees = 0;
pulse::extensions::UiTree g_last_ui_tree;  // the last accepted tree (settings_render_test paints it)
void Validate(const PulseUiModelTree* tree) {
    pulse::extensions::UiTree out;
    std::set<std::string> commands;
    std::string error;
    ++g_validated_trees;
    if (pulse::extensions::native::ConvertUiModel(tree, out, commands, error) != PULSE_OK) g_validator_errors.push_back(error);
    else g_last_ui_tree = std::move(out);
}
// Canvas drawing scripts have their own option budget in the SDK (4096 bytes per entry, ui_tree.cpp).
size_t g_max_canvas_opt = 0;
void WalkNode(const PulseUiModelNode* n, size_t depth, size_t& nodes, size_t& max_depth,
              size_t& max_id, size_t& max_opt, size_t& max_text) {
    if (!n) return;
    ++nodes;
    max_depth = std::max(max_depth, depth);
    max_id = std::max<size_t>(max_id, n->id.length);
    if (n->text.present) max_text = std::max<size_t>(max_text, n->text.value.length);
    if (n->detail.present) max_text = std::max<size_t>(max_text, n->detail.value.length);
    if (n->message.present) max_text = std::max<size_t>(max_text, n->message.value.length);
    if (n->options.present && n->options.data) {
        for (uint32_t i = 0; i < n->options.count; ++i) {
            if (n->type == PULSE_UI_MODEL_CANVAS) g_max_canvas_opt = std::max<size_t>(g_max_canvas_opt, n->options.data[i].length);
            else max_opt = std::max<size_t>(max_opt, n->options.data[i].length);
        }
    }
    if (n->children.present && n->children.data) {
        for (uint32_t i = 0; i < n->children.count; ++i) {
            WalkNode(&n->children.data[i], depth + 1, nodes, max_depth, max_id, max_opt, max_text);
        }
    }
}
struct Host {
    std::map<std::string,std::string> all_states;
    std::map<std::string,std::pair<PulseUiClickFn,void*>> command_callbacks;
    unsigned saves=0;
    unsigned rejected_values=0;
    bool enforce_value_limit=false;
    bool reject_submission=false;
    bool synchronous_failure=false;
    bool synchronous_conflict=false;
    bool synchronous_load=false;
    std::vector<std::string> label_options;
    std::string ui_label;
    PulseUiStateChangedFn state = nullptr;
    void* state_user = nullptr;
    PulseResourcesCompletionFn get = nullptr, set = nullptr;
    void* get_user = nullptr;
    void* set_user = nullptr;
    std::string written, stored, inflight_value, ui_material, initial_ui_material, initial_ui_opacity;
    size_t updates = 0, update_max_nodes = 0, update_max_depth = 0, update_max_states = 0;
    size_t tree_nodes = 0, tree_max_depth = 0, tree_states = 0, tree_max_id = 0, tree_max_opt = 0, tree_max_text = 0;
    uint64_t expected_version = 0;
    uint64_t next = 1;
    static PulseStatus PULSE_CALL Query(void* context, PulseStringView name, uint32_t, void* out, uint32_t size) {
        auto* self = static_cast<Host*>(context);
        if (Str(name) == PULSE_RESOURCES_INTERFACE_NAME && size == sizeof(PulseResourcesV1)) {
            PulseResourcesV1 api{}; api.context = self;
            api.settings_get = [](void* p, PulseResourcesCompletionFn cb, void* u, uint64_t* id) -> PulseStatus {
                auto& h = *static_cast<Host*>(p); h.get = cb; h.get_user = u; *id = h.next++;
                if(h.synchronous_load){const auto saved=h.stored;h.Load(saved.c_str(),h.expected_version+1);}
                return PULSE_OK;
            };
            api.settings_set = [](void* p, PulseStringView, PulseStringView value, uint32_t, uint64_t version,
                                  PulseResourcesCompletionFn cb, void* u, uint64_t* id) -> PulseStatus {
                auto& h = *static_cast<Host*>(p); ++h.saves; h.written = Str(value);
                if (h.enforce_value_limit && value.length > 1024) {
                    ++h.rejected_values; return PULSE_ERROR_ARGUMENT;
                }
                if (h.reject_submission) return PULSE_ERROR_UNAVAILABLE;
                if (h.synchronous_failure || h.synchronous_conflict) {
                    PulseResourcesReply reply{}; reply.status=PULSE_ERROR_STATE;
                    const std::string error=h.synchronous_conflict?"version_conflict":"fixture_io_failure"; reply.error=Sv(error);
                    cb(u,&reply); return PULSE_OK;
                }
                h.set = cb; h.set_user = u;
                h.inflight_value=h.written;
                h.expected_version = version;
                *id = h.next++; return PULSE_OK;
            };
            std::memcpy(out, &api, size); return PULSE_OK;
        }
        if (Str(name) == PULSE_SETTINGS_PAGES_INTERFACE_NAME && size == sizeof(PulseSettingsPagesV1)) {
            PulseSettingsPagesV1 api{}; api.context = self;
            api.create = [](void* p, PulseStringView, const PulseUiModelTree* tree, uint64_t* id) -> PulseStatus {
                auto& h = *static_cast<Host*>(p);
                if (tree) {
                    Validate(tree);
                    if (model_capture) model_capture(tree);
                    h.tree_nodes = 0;
                    h.tree_max_depth = 0;
                    h.tree_states = tree->state_count;
                    h.tree_max_id = 0;
                    h.tree_max_opt = 0;
                    h.tree_max_text = 0;
                    WalkNode(tree->root, 0, h.tree_nodes, h.tree_max_depth,
                             h.tree_max_id, h.tree_max_opt, h.tree_max_text);
                    const auto collect = [&](auto&& self, const PulseUiModelNode* node) -> void {
                        if (!node) return;
                        if (Str(node->id)=="label-seg" && node->options.present)
                            for(uint32_t i=0;i<node->options.count;++i)h.label_options.push_back(Str(node->options.data[i]));
                        if(node->children.present)for(uint32_t i=0;i<node->children.count;++i)self(self,&node->children.data[i]);
                    };
                    collect(collect,tree->root);
                    for (uint32_t i = 0; i < tree->state_count; ++i) {
                        h.all_states[Str(tree->states[i].key)] = Str(tree->states[i].value);
                        if (Str(tree->states[i].key) == "material") h.initial_ui_material = Str(tree->states[i].value);
                        if (Str(tree->states[i].key) == "opacity") h.initial_ui_opacity = Str(tree->states[i].value);
                    }
                }
                *id = 1; return PULSE_OK;
            };
            api.query = [](void* p, uint64_t, PulseStringView n, uint32_t v, void* o, uint32_t s) -> PulseStatus { return Query(p, n, v, o, s); };
            api.remove = [](void*, uint64_t) -> PulseStatus { return PULSE_OK; };
            std::memcpy(out, &api, size); return PULSE_OK;
        }
        if (Str(name) == PULSE_UI_EVENTS_INTERFACE_NAME && size == sizeof(PulseUiEventsV1)) {
            PulseUiEventsV1 api{}; api.context = self;
            api.subscribe_state = [](void* p, uint64_t, PulseStringView, uint32_t, PulseUiStateChangedFn cb, void* u, uint64_t* token) -> PulseStatus {
                auto& h = *static_cast<Host*>(p); h.state = cb; h.state_user = u; *token = 1; return PULSE_OK;
            };
            std::memcpy(out, &api, size); return PULSE_OK;
        }
        if (Str(name) == PULSE_UI_MODEL_INTERFACE && size == sizeof(PulseUiModelV1)) {
            PulseUiModelV1 api{}; api.context = self;
            api.subscribe_command=[](void* p,uint64_t,PulseStringView id,PulseUiClickFn cb,void* user,uint64_t* token)->PulseStatus {
                auto& h=*static_cast<Host*>(p);h.command_callbacks[Str(id)]={cb,user};*token=h.next++;return PULSE_OK;
            };
            api.update = [](void* p, uint64_t, const PulseUiModelTree* tree) -> PulseStatus {
                auto& h = *static_cast<Host*>(p);
                if (!tree || !tree->root) return PULSE_ERROR_ARGUMENT;
                Validate(tree);
                size_t nodes = 0, depth = 0, id = 0, opt = 0, text = 0;
                WalkNode(tree->root, 0, nodes, depth, id, opt, text);
                // The SDK rejects it (sdk/native/include/pulse/ui_model.h: nodes 384/depth 12, states 128).
                if (nodes > 384 || depth > 12 || tree->state_count > 128 || g_max_canvas_opt > 4096) return PULSE_ERROR_ARGUMENT;
                ++h.updates;
                h.update_max_nodes = std::max(h.update_max_nodes, nodes);
                h.update_max_depth = std::max(h.update_max_depth, depth);
                h.update_max_states = std::max<size_t>(h.update_max_states, tree->state_count);
                return PULSE_OK;
            };
            api.set_states = [](void* p, uint64_t, const PulseUiState* values, uint32_t count) -> PulseStatus {
                auto& h = *static_cast<Host*>(p);
                for (uint32_t i = 0; i < count; ++i) h.all_states[Str(values[i].key)] = Str(values[i].value);
                for (uint32_t i = 0; i < count; ++i) if (Str(values[i].key) == "material") h.ui_material = Str(values[i].value);
                for (uint32_t i = 0; i < count; ++i) if (Str(values[i].key) == "label_style") h.ui_label = Str(values[i].value);
                return PULSE_OK;
            };
            std::memcpy(out, &api, size); return PULSE_OK;
        }
        return PULSE_ERROR_UNAVAILABLE;
    }
    void SelectKey(const char* k, const char* value) {
        PulseUiStateChange change{};
        const std::string key = k, val = value;
        change.key = Sv(key); change.value = Sv(val); change.source_mask = PULSE_UI_STATE_USER;
        PulseUiStateBatch batch{}; batch.changes = &change; batch.count = 1;
        state(state_user, 1, &batch);
    }
    void Command(const std::string& id) { auto at=command_callbacks.find(id);if(at!=command_callbacks.end())at->second.first(at->second.second,1,Sv(id)); }
    void Select(const char* value) { SelectKey("material", value); }
    void Load(const char* json, uint64_t version) {
        stored=json;
        auto cb = get; auto* user = get_user; get = nullptr;
        const std::string key = "desktop.preferences", value = json;
        PulseSetting setting{Sv(key), Sv(value)};
        PulseResourcesReply reply{}; reply.status = PULSE_OK; reply.settings = {version, &setting, 1, 0};
        cb(user, &reply);
    }
    void SaveReply(bool conflict, uint64_t version) {
        if (!set) return;
        if(!conflict)stored=inflight_value;
        auto cb = set; auto* user = set_user; set = nullptr;
        const std::string error = "version_conflict";
        PulseResourcesReply reply{}; reply.status = conflict ? PULSE_ERROR_STATE : PULSE_OK;
        if (conflict) reply.error = Sv(error);
        reply.settings.version = version; cb(user, &reply);
    }
};
}
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--ai-visibility") == 0) {
        Host storage;
        PulseHostV1 api{sizeof(PulseHostV1), 1, &storage, Host::Query};
        pulse::desktop::plugin::DesktopSettingsPage page;
        std::vector<std::string> invoked;
        std::string ai_binding;
        int failed = 0;
        const auto check = [&](bool ok, const char* name) {
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
            failed += !ok;
        };
        model_capture = [&](const PulseUiModelTree* tree) {
            const auto walk = [&](auto&& visit, const PulseUiModelNode& node) -> void {
                if (Str(node.id) == "ai-row" && node.visible_binding.present)
                    ai_binding = Str(node.visible_binding.value);
                if (node.children.present)
                    for (uint32_t i = 0; i < node.children.count; ++i) visit(visit, node.children.data[i]);
            };
            walk(walk, *tree->root);
        };
        check(page.Open(&api, {}, [&](std::string_view cmd) { invoked.emplace_back(cmd); }, false) == PULSE_OK,
            "open desktop settings while AI assistant is off");
        model_capture = nullptr;
        check(ai_binding == "state.ai_organize_enabled" && storage.all_states["ai_organize_enabled"] == "false",
            "row containing title, description and button is hidden in initial SDK model");
        storage.Command("cmd.ai_organize");
        storage.Command("reorganize-desktop");
        check(invoked == std::vector<std::string>{"reorganize-desktop"}, "reject stale AI click but retain rule action");
        page.SetAiOrganizerEnabled(true);
        storage.Command("cmd.ai_organize");
        check(storage.all_states["ai_organize_enabled"] == "true" && invoked.size() == 2 &&
              invoked.back() == "cmd.ai_organize", "enable restores the row and AI action on the same page");
        page.SetAiOrganizerEnabled(false);
        storage.Command("cmd.ai_organize");
        check(storage.all_states["ai_organize_enabled"] == "false" && invoked.size() == 2,
            "disable hides the live row and rejects queued clicks");
        return failed != 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--glass-persistence") == 0) {
        using namespace pulse::desktop;
        using plugin::DesktopPrefs;
        int failed=0;
        const auto check=[&](bool ok,const char* name){std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);failed+=!ok;};
        Host storage; storage.enforce_value_limit=true;
        PulseHostV1 api{sizeof(PulseHostV1),1,&storage,Host::Query};
        plugin::DesktopSettingsPage settings;
        check(settings.Open(&api,{}, {})==PULSE_OK,"open isolated persistence controller");
        storage.Load("{\"material\":5,\"glass_preset\":2,\"glass_thickness\":44,\"glass_ior\":188,\"glass_disp_gain\":22,\"glass_glare\":108,\"glass_angle\":-38,\"glass_frost\":0}",269);
        check(settings.Prefs().glass_preset==2 && settings.Prefs().glass_glare==108,"legacy preset is loaded without silently replacing user choice");
        settings.UpdateFromHostProcess(settings.Prefs());
        storage.SaveReply(false,270);
        storage.Command("glass-reference-reset");
        std::printf("RESET_PAYLOAD_BYTES=%zu\n",storage.written.size());
        check(storage.written.size()<=1024 && storage.set && storage.rejected_values==0,"reset passes real SDK 1024-byte single-value limit");
        const auto reset=DesktopPrefs::FromJson(storage.written);
        const DesktopPrefs fresh;
        check(reset.glass_options==fresh.glass_options && reset.glass_reference && reset.glass_preset==5 && reset.glass_frost==fresh.glass_frost,"persisted reset equals fresh optical defaults including legacy frost");
        storage.SaveReply(false,271);
        const auto persisted=storage.stored;
        settings.Close();
        Host reopened; reopened.enforce_value_limit=true;
        PulseHostV1 api2{sizeof(PulseHostV1),1,&reopened,Host::Query};
        settings.Open(&api2,{},{}); reopened.Load(persisted.c_str(),271);
        check(settings.Prefs().glass_options==fresh.glass_options && settings.Prefs().glass_reference && settings.Prefs().glass_preset==5,"restart loads reset instead of stale thick-prism preset");
        auto custom=settings.Prefs(); custom.glass_preset=4; custom.glass_reference=true;
        for(const auto& field:kGlassOpticalFields) custom.glass_options.*(field.member)=field.min+(field.max-field.min)*0.371234567891234;
        custom.glass_options.blur_edge=false;
        for(int i=0;i<10;++i)custom.weather_city+=L"中\"\\\n";
        settings.UpdateFromHostProcess(custom);
        std::printf("CUSTOM_PAYLOAD_BYTES=%zu\n",reopened.written.size());
        const auto roundtrip=DesktopPrefs::FromJson(reopened.written);
        check(reopened.written.size()<=1024 && reopened.set && reopened.rejected_values==0,"all custom fractional optical fields plus escaped city fit SDK limit");
        check(roundtrip==custom,"custom optics and non-optical preferences roundtrip exactly");
        reopened.SaveReply(false,272);
        reopened.reject_submission=true;
        settings.UpdateFromHostProcess(custom);
        reopened.reject_submission=false;
        settings.UpdateFromHostProcess(custom);
        check(reopened.set!=nullptr && reopened.expected_version==272,"immediate API rejection does not wedge later save");
        reopened.SaveReply(false,273);
        reopened.synchronous_failure=true;
        const auto before=reopened.saves;
        settings.UpdateFromHostProcess(custom);
        check(reopened.saves==before+1 && reopened.set==nullptr,"synchronous failure has no recursive retry loop");
        reopened.synchronous_failure=false;
        settings.UpdateFromHostProcess(custom);
        check(reopened.set!=nullptr,"save remains retryable after synchronous failure");
        reopened.SaveReply(false,274);
        auto newer=custom;newer.glass_options.glare_factor=35.125;newer.glass_preset=4;
        settings.UpdateFromHostProcess(newer);reopened.SaveReply(true,275);
        if(reopened.get)reopened.Load(persisted.c_str(),275);
        check(DesktopPrefs::FromJson(reopened.written).glass_options==newer.glass_options && reopened.expected_version==275,"version-conflict reload keeps latest custom edit and retries");
        reopened.SaveReply(false,276);
        auto extreme=newer;
        extreme.weather_city=std::wstring(40,L'\x01');
        for(const auto& field:kGlassOpticalFields)extreme.glass_options.*(field.member)=field.min==0?1.234567890123456e-200:field.max;
        settings.UpdateFromHostProcess(extreme);
        std::printf("EXTREME_PAYLOAD_BYTES=%zu\n",reopened.written.size());
        check(reopened.written.size()<=1024 && DesktopPrefs::FromJson(reopened.written)==extreme,"exponent values and forty fully escaped characters fit and roundtrip");
        reopened.SaveReply(false,277);
        auto preserved=extreme;
        preserved.ApplyJson("{\"prefs_schema\":99,\"values\":[],\"optics\":[]}");
        check(preserved==extreme,"unknown persistence schema leaves existing custom state unchanged");
        preserved.ApplyJson("{\"prefs_schema\":1,\"values\":[1],\"optics\":[]}");
        check(preserved==extreme,"truncated persistence arrays leave existing custom state unchanged");
        reopened.synchronous_conflict=true;reopened.synchronous_load=true;
        const auto conflict_start=reopened.saves;
        settings.UpdateFromHostProcess(newer);
        check(reopened.saves>conflict_start && reopened.saves<=conflict_start+4,"inline conflict and reload loop has bounded work without recursion");
        reopened.synchronous_conflict=false;reopened.synchronous_load=false;
        settings.UpdateFromHostProcess(newer);
        check(reopened.set!=nullptr && DesktopPrefs::FromJson(reopened.written).glass_options==newer.glass_options,"bounded conflicts retain edits for next explicit retry");
        reopened.SaveReply(false,280);
        settings.Close();
        std::printf("GLASS_PERSISTENCE_RESULT failed=%d\n",failed);
        return failed?1:0;
    }
    Host h; PulseHostV1 host{sizeof(PulseHostV1), 1, &h, Host::Query};
    pulse::desktop::plugin::DesktopSettingsPage page;
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failed += !ok; };
    check(page.Open(&host, {}, {}) == PULSE_OK && h.state && h.get, "real settings controller starts asynchronous load");
    check(h.initial_ui_material == "1" && h.initial_ui_opacity == "3" &&
          page.Prefs().material == 1 && page.Prefs().opacity_percent == 60,
          "default appearance is Acrylic (1) at 60% opacity (slider stop 3)");
    h.Select("4"); h.Load("{\"material\":3}", 5);
    check(page.Prefs().material == 4 && h.ui_material == "4" && h.expected_version == 5 && pulse::desktop::plugin::DesktopPrefs::FromJson(h.written).material == 4,
          "None selection survives a delayed initial persisted MicaAlt reply");
    h.SaveReply(false, 6);
    h.Select("2"); h.SaveReply(true, 7);
    h.Select("4"); h.Load("{\"material\":3}", 7);
    check(page.Prefs().material == 4 && h.ui_material == "4" && h.expected_version == 7 && pulse::desktop::plugin::DesktopPrefs::FromJson(h.written).material == 4,
          "None survives version-conflict reload and is retried with the current version");
    h.SaveReply(false, 8);
    page.Close();
    Host clean; PulseHostV1 clean_host{sizeof(PulseHostV1), 1, &clean, Host::Query};
    check(page.Open(&clean_host, {}, {}) == PULSE_OK, "reopen settings controller");
    clean.Load("{\"material\":4}", 8);
    check(page.Prefs().material == 4 && clean.ui_material == "4", "persisted None loads when no newer edits exist");
    clean.Select("1"); clean.Select("4"); clean.SaveReply(false, 9);
    check(page.Prefs().material == 4 && clean.expected_version == 9 && pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).material == 4,
          "rapid Acrylic to None edits during an in-flight save keep the last choice");
    clean.SaveReply(false, 10);

    // LiquidGlass (5), dispersion/live-rate roundtrip, unknown material fallback, and SDK ui_tree.cpp budget check
    std::printf("[INFO] settings_tree nodes=%zu/384 depth=%zu/12 states=%zu/128 id=%zu/64 opt=%zu/128 canvas=%zu/4096 text=%zu/1024\n",
                clean.tree_nodes, clean.tree_max_depth, clean.tree_states,
                clean.tree_max_id, clean.tree_max_opt, g_max_canvas_opt, clean.tree_max_text);
    check(clean.tree_nodes > 0 && clean.tree_nodes <= 384 &&
          clean.tree_max_depth <= 12 && clean.tree_states <= 128 &&
          clean.tree_max_id <= 64 && clean.tree_max_opt <= 128 && g_max_canvas_opt <= 4096 && clean.tree_max_text <= 1024,
          "settings UI tree stays within Pulse SDK ui_tree.cpp node, depth, state, and string limits");

    clean.Select("5");
    clean.SaveReply(false, 11);
    check(clean.all_states["is_liquid_glass"] == "true",
          "selecting LiquidGlass reveals curated glass mode and rate controls");
    clean.SelectKey("glass_mode", "3"); // 流光棱镜 -> preset 2
    clean.SaveReply(false, 12);
    check(page.Prefs().glass_preset == 2 && clean.all_states["glass_mode"] == "3",
          "glass_mode=3 selects Prism (preset 2) and syncs UI state");
    clean.SelectKey("glass_mode", "2"); // 柔雾凝脂 -> preset 3
    clean.SaveReply(false, 13);
    check(page.Prefs().glass_preset == 3 && clean.all_states["glass_mode"] == "2",
          "glass_mode=2 selects Frosted (preset 3) and syncs UI state");
    clean.SelectKey("glass_mode", "0"); // 清透水滴 -> preset 5
    clean.SaveReply(false, 14);
    check(page.Prefs().glass_preset == 5 && clean.all_states["glass_mode"] == "0",
          "glass_mode=0 selects Clear Droplet (preset 5) and syncs UI state");
    clean.SelectKey("glass_preset", "2"); // DeepPrism -> thick=44, ior=188, disp=22, glare=108, angle=-38, frost=0
    clean.SaveReply(false, 15);
    check(page.Prefs().material == 5 && page.Prefs().glass_preset == 2 &&
          page.Prefs().glass_thickness == 44 && page.Prefs().glass_ior_x100 == 188 &&
          page.Prefs().glass_dispersion == 22 && page.Prefs().glass_frost == 0 &&
          pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).glass_preset == 2 &&
          pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).glass_ior_x100 == 188 &&
          pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).glass_dispersion == 22,
          "LiquidGlass DeepPrism preset applies full Studio optical fields and serializes them");
    clean.SelectKey("glass_frost", "3"); // 60% stop -> switches glass_preset to Custom (4)
    clean.SaveReply(false, 16);
    clean.SelectKey("glass_rate", "0");
    check(page.Prefs().glass_preset == 4 && page.Prefs().glass_frost == 60 &&
          page.Prefs().glass_live_rate == 0 &&
          pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).glass_preset == 4 &&
          pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).glass_frost == 60 &&
          pulse::desktop::plugin::DesktopPrefs::FromJson(clean.written).glass_live_rate == 0,
          "Glass frost slider switches preset to Custom (4) and serializes cleanly");
    clean.SaveReply(false, 17);
    clean.Select("1");
    clean.SaveReply(false, 18);
    check(clean.all_states["is_liquid_glass"] == "false",
          "switching away from LiquidGlass hides curated glass mode controls");
    page.Close();

    Host compat; PulseHostV1 compat_host{sizeof(PulseHostV1), 1, &compat, Host::Query};
    check(page.Open(&compat_host, {}, {}) == PULSE_OK, "open settings controller for forward-compat check");
    compat.Load("{\"material\":99,\"glass_disp\":9,\"glass_rate\":7,\"glass_ior\":999,\"glass_angle\":-250}", 15);
    check(page.Prefs().material == 1 && compat.ui_material == "1" &&
          page.Prefs().glass_dispersion == 20 && page.Prefs().glass_live_rate == 2 &&
          page.Prefs().glass_ior_x100 == 400 && page.Prefs().glass_glare_angle == -180,
          "unknown future material downgrades safely, migrates legacy glass_disp, and clamps optical ranges");
    page.Close();
    for (int style=0;style<4;++style) {
        Host labels; PulseHostV1 labels_host{sizeof(PulseHostV1),1,&labels,Host::Query};
        check(page.Open(&labels_host,{}, {})==PULSE_OK,"open isolated label settings fixture");
        labels.Load("{}",20);
        const auto value=std::to_string(style);
        labels.SelectKey("label_style",value.c_str());
        const auto restored=pulse::desktop::plugin::DesktopPrefs::FromJson(page.Prefs().ToJson());
        check(page.Prefs().label_style==style&&restored.label_style==style,
              "all four label values retain numeric identity through selection and JSON");
        check(labels.label_options.size()==4&&!labels.label_options[3].empty(),
              "settings tree exposes four options with catalog-backed Normal text");
        page.Close();
    }
    {
        // 小组件: every widget's options fit the SDK's page limits, and the options survive save and reload.
        Host w; PulseHostV1 w_host{sizeof(PulseHostV1), 1, &w, Host::Query};
        w.enforce_value_limit = true;
        check(page.Open(&w_host, {}, {}) == PULSE_OK, "open widget settings fixture");
        w.Load("{}", 30);
        const size_t updates_before_tabs = w.updates;
        for (int tab = 1; tab < 8; ++tab) {
            const auto t = std::to_string(tab);
            w.SelectKey("w_tab", t.c_str());
        }
        std::printf("[INFO] widget tabs: updates=%zu max nodes=%zu/384 depth=%zu/12 states=%zu/128\n",
                    w.updates - updates_before_tabs, w.update_max_nodes, w.update_max_depth, w.update_max_states);
        check(w.updates - updates_before_tabs == 7 && w.update_max_nodes <= 384 && w.update_max_states <= 128,
              "every 小组件 tab (including 照片) rebuilds the page within the SDK node and state limits");
        w.SelectKey("w_scheme_3", "true");
        check(w.all_states["w_scheme_3"] == "true" && w.all_states["w_scheme_0"] == "false",
              "配色 option cards act as radios (the page unchecks the others)");
        w.Command("w-tint-2");
        check(w.update_max_nodes > 0, "a swatch click redraws the page");
        w.SelectKey("w_rotate", "false");
        w.SelectKey("w_seconds", "false");
        w.Command("w-city-del-3");      // 纽约 out (defaults 北京 东京 伦敦 纽约)
        w.Command("w-city-add-16");     // 悉尼 in
        w.Command("w-city-add-16");     // a city already shown is not added twice
        w.SelectKey("w_sys_cpu", "false");
        w.SelectKey("w_photo_int", "3"); // 60s
        w.SelectKey("w_photo_shuf", "true");
        w.SelectKey("w_photo_cap", "false");
        w.SelectKey("w_focus", "4");    // 45
        w.SelectKey("w_low", "3");      // 30%
        w.SelectKey("dock_enabled", "true");
        w.SelectKey("dock_pos", "1");   // 左侧
        w.SelectKey("dock_hide", "2");  // 自动隐藏
        w.SelectKey("dock_minfx", "1"); // 缩放 (2)
        w.SelectKey("dock_size", "2");  // 大 (56)
        w.SelectKey("dock_mag", "false");
        w.SelectKey("dock_sys", "false");
        w.SelectKey("wp_kind", "1");
        w.SelectKey("video_fill", "1");
        page.SetVideoWallpaperStatus("海边日落.mp4", "正在播放");
        const auto& wp = page.Prefs().widgets;
        const auto& dp = page.Prefs().dock;
        check(page.Prefs().wallpaper_kind == 1 && page.Prefs().video_fill == 1 &&
              w.all_states["is_video_wallpaper"] == "true" &&
              w.all_states["video_status"].find("海边日落.mp4") != std::string::npos,
              "video wallpaper settings state, fill mode, and status sync cleanly");
        check(wp.scheme == 3 && !wp.tint_auto && wp.tint_argb == 0xFF5E5CE6u && !wp.stack_rotate &&
              !wp.clock_seconds && wp.world_cities.size() == 4 && wp.world_cities[3] == L"悉尼" &&
              wp.system_items == 0x1E && wp.photo_interval_s == 60 && wp.photo_shuffle && !wp.photo_caption &&
              wp.focus_min == 45 && wp.battery_low_pct == 30,
              "小组件 selections (including 叠放自动轮换 & 照片 tab) land in the prefs");
        check(dp.enabled && dp.position == 1 && dp.hide_mode == 2 && dp.minimize_effect == 2 &&
              dp.icon_size == 56 && !dp.magnification && !dp.system_area,
              "程序坞 Dock settings selections land in DesktopPrefs::dock");
        const auto restored = pulse::desktop::plugin::DesktopPrefs::FromJson(page.Prefs().ToPersistedJson());
        check(restored.widgets == wp && restored.dock == dp && w.all_states["w_is_tint"] == "true" &&
              restored.wallpaper_kind == 1 && restored.video_fill == 1,
              "小组件, Dock, and video wallpaper prefs survive the persisted form and reveal the tint row");
        w.Command("w-tint-0");
        for (int i = 0; i < 5; ++i) w.Command("w-city-del-0");
        check(page.Prefs().widgets.tint_auto && page.Prefs().widgets.world_cities.size() == 1 &&
              page.Prefs().widgets.world_cities[0] == L"悉尼",
              "取自壁纸 brings back the wallpaper tint; removing chips keeps the last city");
        w.Command("w-city-add-1");      // 上海
        check(page.Prefs().widgets.world_cities.size() == 2 && page.Prefs().widgets.world_cities[1] == L"上海",
              "添加城市 appends after the cities already shown");
        for (const char* key : {"w_sys_mem", "w_sys_vram", "w_sys_disk", "w_sys_net"}) w.SelectKey(key, "false");
        check(page.Prefs().widgets.system_items != 0, "turning every 系统 item off keeps one");
        pulse::desktop::plugin::DesktopPrefs big = page.Prefs();
        big.weather_city = std::wstring(40, L'\u6D4B');
        big.widgets.world_cities = {L"奥克兰", L"温哥华", L"莫斯科", L"新加坡"};
        const auto persisted = big.ToPersistedJson();
        std::printf("[INFO] largest persisted prefs: %zu/1024 bytes\n", persisted.size());
        check(persisted.size() <= 1024, "the largest persisted prefs stay within the SDK's 1024-byte value limit");
        page.Close();
    }
    for (const auto& e : g_validator_errors) std::printf("[INFO] validator: %s\n", e.c_str());
    check(g_validated_trees > 0 && g_validator_errors.empty(), "every settings tree passes Pulse's ConvertUiModel/ui_tree validation");
    std::printf("SETTINGS_RESULT failed=%d\n", failed);
    return failed ? 1 : 0;
}

