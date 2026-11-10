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
#include "../common/menu_icon.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::app {

struct StaticVerb {
    std::wstring verb;      // registry verb key ("print", "edit", "openas")
    std::wstring display;   // menu row text
    std::wstring app_path;  // non-empty => open-with entry: launch this exe
    std::wstring command;   // raw HKCR command template; CommandStore children need this
    std::vector<StaticVerb> children; // cascade flyout (SubCommands / ExtendedSubCommandsKey)
    wchar_t mnemonic = 0;   // access key from the registry label ("&Edit"), 0 = none
    // Explorer's row icon: the verb key's Icon value ("file,index"), or the
    // program itself for open-with entries. Not part of the machine cache;
    // the live pass fills it.
    std::wstring icon;
    common::MenuIconRef icon_pixels;  // ResolveStaticVerbIcons' result for `icon`
};

// 以管理员身份运行 / 以其他用户身份运行: the registry command beside these verbs
// (batfile: cmd.exe /C "%1") runs unelevated when started as a command line.
// Only ShellExecuteEx with the verb itself asks Windows for the elevation.
// Install and print also require Shell association handling (including DDE).
inline bool ShellOwnsStaticVerb(std::wstring_view verb) noexcept {
    const auto same = [verb](std::wstring_view name) {  // registry keys: ASCII, any case
        if (verb.size() != name.size()) return false;
        for (size_t i = 0; i < name.size(); ++i) {
            wchar_t c = verb[i];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            if (c != name[i]) return false;
        }
        return true;
    };
    return same(L"runas") || same(L"runasuser") || same(L"install") || same(L"print");
}

// ext is ".dwg" (file), or a location key ":folder" / ":drive" / ":bg".
// File keys return progid/OpenWith verbs plus "打开方式…". Location keys
// return Directory/Folder/Drive/Background shell verbs, including cascades.
std::vector<StaticVerb> EnumerateStaticVerbs(const std::wstring& ext);

// Loads StaticVerb::icon (and children's) into icon_pixels at the system
// DPI's 16 DIP, through a process-wide cache keyed by the icon reference.
// Icon extraction touches the program files: call it off the UI thread, right
// after EnumerateStaticVerbs.
void ResolveStaticVerbIcons(std::vector<StaticVerb>& verbs);

// Pure helper (self-tested): dedupes by display text, drops entries matching
// any built-in row text, caps the list.
std::vector<StaticVerb> DedupeStaticVerbs(std::vector<StaticVerb> verbs,
                                          const std::vector<std::wstring>& builtin_texts,
                                          size_t cap);

// The shell's small icon for a file type (".docx"), as the 新建 menu shows
// it. Process-wide cache by extension; misses are remembered.
common::MenuIconRef FileTypeMenuIcon(const std::wstring& ext);

// A program's display name: Applications\<exe>\FriendlyAppName, else the
// version resource's FileDescription, else the file name. Reads the file.
std::wstring ProgramFriendlyName(const std::wstring& exe_path);
// Icon pixels for "file[,index]" at the menu's 16 DIP, through the same
// process-wide cache as the open-with rows. Touches the file: worker threads.
common::MenuIconRef ProgramMenuIcon(const std::wstring& reference);

std::wstring MachineStaticVerbCachePath();
bool LoadMachineStaticVerbCache(std::unordered_map<std::wstring, std::vector<StaticVerb>>& out);
bool SaveMachineStaticVerbCache(
    const std::unordered_map<std::wstring, std::vector<StaticVerb>>& cache);
// Walks HKCR file extensions and writes ProgramData\Pulse\shell-verbs.bin.
// Safe to run from the installer (elevated) or Pulse.exe --seed-shell-verbs.
bool SeedMachineStaticVerbCache();

} // namespace pulse::app
