// association.h - 接管压缩包: Pulse's own file types for archive extensions, so a
// double-click in File Explorer extracts with this extension and each format shows
// its icon (icons/*.ico, resources 101-115). Windows protects the default each user
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
int GroupIcon(int group);                // its first extension's icon resource (101-115)
std::wstring IconLibrary();              // this extension's DLL, which holds the icons

enum class Owner { Off, Pulse, Free, Other };
struct Status {
    Owner owner = Owner::Off;
    std::wstring who;      // the program that opens it now, when not Pulse
    bool windows = false;  // that program is Windows' own archive folder
};
// How Windows opens each group now. `groups`: bit i = the group is taken over. Blocking (COM, registry).
std::vector<Status> Query(unsigned groups);
// Registers the file types of the included groups (removing excluded ones) and makes Pulse the
// default where nothing opens an extension yet. `extract`: a double-click extracts; otherwise it
// shows the archive in Pulse with its contents previewed. Blocking; false if the registry refused.
bool Apply(unsigned groups, bool extract);
// Removes everything Apply wrote and gives the extensions back to what opened them before.
void Remove();
// Windows' own "open with" chooser for the group's first extension (UI thread, modal).
void Choose(HWND owner, int group);
// Windows Settings › Apps › Default apps › Pulse, where all of them can be switched at once.
void OpenDefaultApps();
} // namespace association
