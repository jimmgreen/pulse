#pragma once
#include "system_ctx_registry.h"
#include "../shell_host/packaged_ctx_handlers.h"
#include <algorithm>
#include <cwctype>
#include <unordered_set>
namespace pulse::app::sysctx {
inline bool SystemItemToggleable(const Item& item) {
    return item.source == Source::Registry &&
        (item.kind == Kind::Verb || item.kind == Kind::ShellEx || item.kind == Kind::DragDrop) &&
        !item.is_protected && !item.trusted_installer && item.root != Root::File;
}
inline std::vector<std::wstring> PackagedSceneTypes(const Scene& scene) {
    if (scene.mode != SceneMode::Menu) return {};
    if (scene.id == L"dirbg" || scene.id == L"desk") return {L"directory\\background"};
    if (scene.id == L"folder") return {L"directory", L"folder", L"allfilesystemobjects"};
    if (scene.id == L"drive") return {L"drive", L"folder", L"allfilesystemobjects"};
    if (scene.id == L"file") return {L"*", L"allfilesystemobjects"};
    if (scene.id == L"allobj") return {L"allfilesystemobjects"};
    if (scene.id == L"lnk") return {L"*", L"allfilesystemobjects", L".lnk"};
    if (scene.id == L"exe") return {L"*", L"allfilesystemobjects", L".exe"};
    if (scene.id.starts_with(L"ext:")) return {L"*", L"allfilesystemobjects", scene.id.substr(4)};
    return {};
}
inline void AppendPackagedItems(const Scene& scene, const std::vector<shell::PackagedCtxVerb>& verbs,
                               std::vector<Item>& items) {
    const auto types = PackagedSceneTypes(scene);
    std::unordered_set<std::wstring> seen;
    const auto lower = [](std::wstring value) {
        for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
        return value;
    };
    for (const auto& item : items) if (!item.clsid.empty()) seen.insert(lower(item.clsid));
    for (const auto& verb : verbs) {
        if (verb.clsid_text.empty() || !shell::PackagedVerbMatches(verb, types) ||
            !seen.insert(lower(verb.clsid_text)).second) continue;
        Item item; item.source = Source::Packaged; item.kind = Kind::ShellEx;
        item.scene = scene.id; item.clsid = verb.clsid_text;
        item.key_name = verb.verb_id; item.key_path = L"packaged:" + lower(verb.clsid_text);
        item.display = verb.name.empty() ? verb.verb_id : verb.name;
        if (item.display.empty()) item.display = verb.clsid_text;
        items.push_back(std::move(item));
    }
}
}
