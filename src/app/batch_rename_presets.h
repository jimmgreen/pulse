// batch_rename_presets.h - Saved step lists for the 批量重命名 dialog.
//
// %LOCALAPPDATA%\Pulse\rename_presets.json keeps the user's presets, the
// last selected preset identity, and the separate "上次使用" step list.
#pragma once
#include "batch_rename_plan.h"

#include <string>
#include <vector>

namespace pulse::app::rename {

struct Preset {
    std::wstring name;
    std::vector<Step> steps;
    Options opts;
};

struct PresetSelection {
    enum class Kind { None, Builtin, User, Last };
    Kind kind = Kind::None;
    std::wstring key;             // stable built-in ID or saved user name
    bool operator==(const PresetSelection&) const = default;
};

struct PresetStore {
    std::vector<Preset> presets;   // the user's, in the order saved
    bool has_last = false;
    Preset last;                   // steps of the last applied rename
    PresetSelection selection;
};

// Missing or unreadable file: an empty store.
PresetStore LoadPresets();
// Writes the whole store (temp file + replace). False when it cannot.
bool SavePresets(const PresetStore& store);

// Compares step lists: equal keys = the same steps and options.
std::string StepsKey(const std::vector<Step>& steps, const Options& opts);

// The JSON text, for tests.
std::string PresetsToJson(const PresetStore& store);
bool PresetsFromJson(const std::string& utf8, PresetStore& out);

} // namespace pulse::app::rename
