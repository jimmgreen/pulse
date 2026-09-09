// shell_verbs.h — Registry-backed static context-menu verbs (优化.md §7.2.1).
//
// HKCR\<progid>\shell verbs (打印/编辑/…) and the OpenWithList MRU are plain
// registry data, so they go into the context menu's first frame without
// waiting for pulse_shell's COM session. Invocation stays on the ops worker
// (ShellExecuteEx verb / explicit application launch).
//
// EnumerateStaticVerbs does registry reads plus version-info lookups for
// friendly app names: call it off the UI thread and cache per extension.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

struct StaticVerb {
    std::wstring verb;      // registry verb key ("print", "edit", "openas")
    std::wstring display;   // menu row text
    std::wstring app_path;  // non-empty => open-with entry: launch this exe
    std::wstring command;   // raw HKCR command template; CommandStore children need this
    std::vector<StaticVerb> children; // cascade flyout (SubCommands / ExtendedSubCommandsKey)
};

// ext is ".dwg" (file), or a location key ":folder" / ":drive" / ":bg".
// File keys return progid/OpenWith verbs plus "打开方式…". Location keys
// return Directory/Folder/Drive/Background shell verbs, including cascades.
std::vector<StaticVerb> EnumerateStaticVerbs(const std::wstring& ext);

// Pure helper (self-tested): dedupes by display text, drops entries matching
// any built-in row text, caps the list.
std::vector<StaticVerb> DedupeStaticVerbs(std::vector<StaticVerb> verbs,
                                          const std::vector<std::wstring>& builtin_texts,
                                          size_t cap);

std::wstring MachineStaticVerbCachePath();
bool LoadMachineStaticVerbCache(std::unordered_map<std::wstring, std::vector<StaticVerb>>& out);
bool SaveMachineStaticVerbCache(
    const std::unordered_map<std::wstring, std::vector<StaticVerb>>& cache);
// Walks HKCR file extensions and writes ProgramData\Pulse\shell-verbs.bin.
// Safe to run from the installer (elevated) or Pulse.exe --seed-shell-verbs.
bool SeedMachineStaticVerbCache();

} // namespace pulse::app
