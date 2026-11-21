// batch_rename_presets.h - Saved step lists for the 批量重命名 dialog.
//
// %LOCALAPPDATA%\Pulse\rename_presets.json keeps the user's presets and the
// steps of the last rename that went through ("上次使用"). Built-in presets
// live in the dialog (their template tokens follow the UI language).
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

struct PresetStore {
    std::vector<Preset> presets;   // the user's, in the order saved
    bool has_last = false;
    Preset last;                   // steps of the last applied rename
};

// Missing or unreadable file: an empty store.
PresetStore LoadPresets();
// Writes the whole store (temp file + replace). False when it cannot.
bool SavePresets(const PresetStore& store);

// Compares step lists: equal keys = the same steps and options.
std::string StepsKey(const std::vector<Step>& steps, const Options& opts);

// Selects the preset used when the dialog opens without an explicit override.
// Invalid or empty history falls back to the built-in default.
Preset InitialPreset(const PresetStore& store, const Preset& fallback);

// The JSON text, for tests.
std::string PresetsToJson(const PresetStore& store);
bool PresetsFromJson(const std::string& utf8, PresetStore& out);

} // namespace pulse::app::rename
