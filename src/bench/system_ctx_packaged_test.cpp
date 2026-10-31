#include "../app/system_ctx_packaged.h"
#include "../ipc/ctx_menu_util.h"
#include <cstdio>
using namespace pulse;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    std::vector<shell::PackagedCtxVerb> verbs;
    for (const auto* type : {L"*", L"allfilesystemobjects", L"folder", L"drive", L"directory\\background", L".png"}) {
        shell::PackagedCtxVerb verb; verb.clsid_text = std::wstring(L"{") + type + L"}";
        verb.name = type; verb.verb_id = type; verb.item_types.push_back(type); verbs.push_back(verb);
    }
    const auto count = [&](const wchar_t* id) {
        app::sysctx::Scene scene;
        for (const auto& builtin : app::sysctx::BuiltinScenes()) if (builtin.id == id) scene = builtin;
        if (std::wstring(id).starts_with(L"ext:")) scene.id = id;
        std::vector<app::sysctx::Item> items;
        app::sysctx::AppendPackagedItems(scene, verbs, items); return items.size();
    };
    check(count(L"file") == 2 && count(L"allobj") == 1, "file and all-object scopes match host categories");
    check(count(L"folder") == 2 && count(L"drive") == 3, "folders and drives include shared folder/all-object commands");
    check(count(L"dirbg") == 1 && count(L"desk") == 1, "background scopes do not inherit file wildcards");
    check(count(L"ext:.png") == 3 && count(L"ext:.jpg") == 2, "extension scopes include exact and generic file commands");
    check(count(L"pc") == 0 && count(L"sendto") == 0, "unrelated scopes do not receive packaged file commands");
    app::sysctx::Scene scene; scene.id = L"file";
    std::vector<app::sysctx::Item> items;
    app::sysctx::AppendPackagedItems(scene, verbs, items);
    app::sysctx::AppendPackagedItems(scene, verbs, items);
    check(items.size() == 2, "repeated packaged declarations deduplicate by CLSID");
    for (const auto& item : items) {
        check(item.source == app::sysctx::Source::Packaged && !app::sysctx::SystemItemToggleable(item),
              "packaged item cannot enter system toggle delete or move operations");
        check(!ipc::HandlerCatalogKey(item.clsid).empty(), "packaged item retains existing Pulse handler visibility key");
    }
    items[0].source = app::sysctx::Source::Registry;
    check(app::sysctx::SystemItemToggleable(items[0]), "classic handler remains system-manageable");
    app::sysctx::AppendPackagedItems(scene, verbs, items);
    check(items.size() == 2 && items[0].source == app::sysctx::Source::Registry, "classic registration wins duplicate CLSID");
    app::sysctx::Scene drag; drag.id = L"file"; drag.mode = app::sysctx::SceneMode::DragDrop;
    std::vector<app::sysctx::Item> none; app::sysctx::AppendPackagedItems(drag, verbs, none);
    check(none.empty(), "drag/drop mode excludes regular packaged context commands");
    return failures ? 1 : 0;
}
