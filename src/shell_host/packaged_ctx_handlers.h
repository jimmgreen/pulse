#pragma once

// File Explorer context-menu verbs contributed by MSIX / sparse packages
// (the windows.fileExplorerContextMenus extension used by WinRAR 7, NanaZip,
// VS Code, Bandizip, Windows Terminal, ...). They are not listed under
// shellex\ContextMenuHandlers and implement IExplorerCommand, not IContextMenu.

#include <windows.h>
#include <objidl.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace pulse::shell {

struct PackagedCtxVerb {
    CLSID clsid{};
    std::wstring clsid_text;               // lower-case {guid}
    std::wstring name;                     // package display name (settings, slow reports)
    std::wstring verb_id;
    std::vector<std::wstring> item_types;  // lower-case: "*", "directory", "directory\background", ".txt"
};

// Verbs of every installed package, cached until HKCR\PackagedCom\Package changes.
std::vector<PackagedCtxVerb> PackagedContextMenuVerbs();

// Manifest item types that a right-click on `path` (or its background) matches.
std::vector<std::wstring> PackagedItemTypesFor(bool background, const std::wstring& path);
bool PackagedVerbMatches(const PackagedCtxVerb& verb, const std::vector<std::wstring>& types);

// Parses the context-menu verbs of one AppxManifest.xml (pure; self-tested).
// `identity` and `display_name` receive the package Identity Name and the raw
// Properties/DisplayName (possibly an ms-resource: reference).
std::vector<PackagedCtxVerb> ParsePackagedContextMenus(IStream* manifest, std::wstring* identity,
                                                       std::wstring* display_name);

// True when at least half of a packaged verb's invokable row texts already
// came from classic handlers: apps such as Bandizip register both a classic
// handler and its Windows 11 twin, and Explorer only ever shows one of them.
bool PackagedRowsDuplicate(const std::vector<std::wstring>& packaged_rows,
                           const std::vector<std::wstring>& classic_rows);

// Hosts an IExplorerCommand in the IContextMenu pipeline: QueryContextMenu
// inserts its title (sub-commands become a submenu) and InvokeCommand calls
// IExplorerCommand::Invoke with `items`. A `site` is handed to the command and
// its sub-commands through IObjectWithSite, as Explorer does, and taken back
// when the menu is released.
HRESULT CreateExplorerCommandMenu(IExplorerCommand* command, IShellItemArray* items,
                                  IContextMenu** out, IUnknown* site = nullptr);
HRESULT CreateExplorerCommandMenuMulti(const std::vector<IExplorerCommand*>& commands,
                                       IShellItemArray* items, IContextMenu** out,
                                       IUnknown* site = nullptr);

// Creates a read-only IPropertyBag backed by `root\subkey` for initializing
// registered IExplorerCommand / IExecuteCommand handlers.
HRESULT CreateRegistryPropertyBag(HKEY root, const std::wstring& subkey,
                                  IPropertyBag** out);

// The part of Explorer's view site that verbs read their folder from:
// IServiceProvider::QueryService(SID_SFolderView) gives an IFolderView whose
// GetFolder returns `folder` (the folder shown, for a background click the
// folder itself), IOleWindow::GetWindow gives `owner`, and when `selection` is
// given IFolderView::ItemCount / Items answer SVGIO_SELECTION.
HRESULT CreateFolderViewSite(PCIDLIST_ABSOLUTE folder, IUnknown** out,
                             HWND owner = nullptr, IShellItemArray* selection = nullptr);

} // namespace pulse::shell