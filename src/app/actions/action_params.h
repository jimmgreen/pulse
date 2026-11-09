// action_params.h - What each action takes when the assistant runs it, and
// the assistant's lookup of the action table.
//
// A spec is "name:kind" words: kinds are bool, string, path, folder,
// folders, paths, pane, setting, any, selector, or the values with | (for
// example "by:name|modified|type|size|created desc:bool pane:pane"). An
// action without a spec runs as its menu row does; one with choices takes
// "choice" (a choice's title or its position).
#pragma once

#include "action_registry.h"
#include "../../common/json_value.h"

#include <string_view>
#include <vector>

namespace pulse::app::actions {

// The spec of `id`, or "" when it takes no arguments.
std::string_view ArgSpec(std::string_view id) noexcept;
// {"by":"name|modified|type|size|created","desc":"bool"}.
json::Value ArgSpecJson(std::string_view spec);

// The best matches for `query` (words split on spaces; Chinese, pinyin or
// English ids), best first, at most `max`; runnable ones before the rest.
// Settings rows only match words that name them.
std::vector<const ActionView*> FindActions(const std::vector<ActionView>& views, std::wstring_view query,
                                           size_t max);

// One find_actions row: id (settings: "settings.set" and key), title, args,
// value, enabled / why, risk, shortcut.
json::Value ActionJson(const ActionView& view);

} // namespace pulse::app::actions
