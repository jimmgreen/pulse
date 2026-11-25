// association.h - 接管压缩包: Pulse's own file types for archive extensions, so a
// double-click in File Explorer extracts with this extension and each format shows
// its icon (icons/*.ico; one resource block per icon style, see plugin.rc). Windows protects the default each user
// chose (UserChoice): an extension that already opens with a program is only offered
// to Pulse through Windows' own chooser, never changed silently. takeover_record.h
// says what is written and how it is undone.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace association {
// One row of the settings list; the legacy formats share one.
inline constexpr int kGroupCount = 12;
inline constexpr unsigned kAllGroups = (1u << kGroupCount) - 1;
const char* GroupLabel(int group);       // "ZIP", "LZH · ARJ · CPIO"
const char* GroupExtensions(int group);  // ".gz .tgz"
// Icon styles the settings offer; each is a block of resources kIconStyleStride above the previous one.
enum IconStyle : int { kIconDocument = 0, kIconFolder = 1, kIconColor = 2 };
inline constexpr int kIconStyleCount = 3;
inline constexpr int kIconStyleStride = 20;
// The resource id of a 「文档」 icon (101-115) in `style`; an unknown style falls back to 「文档」.
inline int StyledIcon(int document_icon, int style) {
    return document_icon + (style > 0 && style < kIconStyleCount ? style * kIconStyleStride : 0);
}
int GroupIcon(int group);                // its first extension's 「文档」 icon resource (101-115)
std::wstring IconLibrary();              // this extension's DLL, which holds the icons

struct ExtensionSpec {
    std::string extension;               // lowercase dotted extension, e.g. ".zip"
    int group = 0;                       // 0..kGroupCount-1
    int icon = 0;                        // 「文档」 resource id in pulse_7zip.dll (101..115); see StyledIcon
    std::string display_name;            // localized file type display name (UTF-8)
};
std::vector<ExtensionSpec> AllExtensions();

enum class Owner { Off, Pulse, Free, Other, Unknown };
struct Status {
    Owner owner = Owner::Off;
    std::wstring who;      // the program that opens it now, when not Pulse
    unsigned pulse_count = 0; // effective extensions in this group, for chooser progress
    bool windows = false;  // that program is Windows' own archive folder
};
// How Windows opens each group now. `groups`: bit i = the group is taken over. Blocking (COM, registry).
std::vector<Status> Query(unsigned groups);
// Registers the file types of the included groups (removing excluded ones) and makes Pulse the
// default where nothing opens an extension yet. `extract`: a double-click extracts; otherwise it
// shows the archive in Pulse with its contents previewed. `icon_style`: which icon block DefaultIcon
// points at (IconStyle). Blocking; false if the registry refused.
bool Apply(unsigned groups, bool extract, int icon_style = kIconDocument);
// Removes everything Apply wrote and gives the extensions back to what opened them before.
bool Remove();
// Windows' own "open with" chooser for the first extension not yet owned by Pulse (UI thread, modal).
void Choose(HWND owner, int group);
// Windows Settings › Apps › Default apps › Pulse, where all of them can be switched at once.
void OpenDefaultApps();
} // namespace association
