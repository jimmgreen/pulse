# Pulse

**A file manager for Windows that makes browsing, searching and organizing files feel effortless.**

Tabs and multi-pane layouts, whole-disk indexed search with pinyin matching, Space-bar Quick Look, a staging tray and a customizable look — all in one workspace.

[Download the latest release](https://github.com/jimmgreen/pulse/releases/latest) · [Release notes](https://github.com/jimmgreen/pulse/releases) · [Report an issue](https://github.com/jimmgreen/pulse/issues) · [简体中文](README.md)

![Pulse in dark theme with a wallpaper background](docs/images/appearance.png)

## Download and install

Pick the package for your system on the releases page:

| Edition | System | File |
| --- | --- | --- |
| Windows (64-bit) | Windows 10 / Windows 11 | `PulseSetup-<version>.exe` |
| Windows 8.1 compatible (64-bit) | Windows 8.1 | `PulseSetup-<version>-win81.exe` |
| Portable (64-bit) | Windows 10 / Windows 11 | `Pulse-<version>-portable-win-x64.zip` |

Run the installer and follow the wizard. You can enable the PulseIndex service during setup for whole-disk indexed search. The portable edition runs straight from the unpacked folder and still keeps its settings in your user app-data folder; the index service is only deployed by the installer.

The interface is available in Simplified Chinese, Traditional Chinese and English. It follows Windows by default and can be changed in Settings → Display language.

## Make the workspace yours

- Light, dark or system theme, switchable from the title bar; pick an accent color, a window effect (Acrylic, Mica), a wallpaper background, panel transparency and background blur.
- Text rendering offers Auto / Sharp / Smooth modes, and list rows come in compact, standard and roomy heights.
- Details view supports smart dates ("Today 14:32", "Yesterday 09:10"), zebra rows, colored extension badges, size bars and content-fitted column widths; right-click a column header to choose which columns to show.
- Seven colored tags mark files and folders and are summarized in the sidebar; you can also tag items straight from the Explorer context menu.
- Sidebar sections can be reordered and collapsed, and tabs can move into the sidebar as a vertical list (Ctrl+B to collapse / expand).

## Find files faster

- PulseIndex searches file names across whole drives with live-updating results; you can also limit a search to the current folder or a chosen folder.
- File-name search understands full pinyin, initials and polyphonic characters — typing `sh` finds 品牌设计, 城市夜景 and 山湖晨雾 — and highlights the matched characters.
- Content search covers text and code, PDF, Word, Excel, PowerPoint and more. Results stream in, and after opening a file's location you can go back to the same results.
- Advanced search (Ctrl+Shift+F) filters by type, date, size and more. Global quick search (Alt+Space) is available anywhere, and "Search in Pulse" opens the full result list in the main window.
- Type `cmd`, `powershell`, `pwsh` or `wt` in the address bar to open a terminal in the current folder.

![Pinyin search in Pulse](docs/images/search.png)

## Work with several folders at once

- Single, side-by-side, stacked, three-pane and four-pane layouts (Ctrl+1/2/3/4); every tab keeps its own layout and locations.
- Column view shows the parent, current and selected folders together; switch freely between tiles, icons, list, details and content views.
- Group lists by name, date, type or size; each folder remembers its own view, sort and grouping.
- Two-pane folder compare highlights items that differ, are missing or are newer on either side. Folder sizes are calculated in the background.
- The details panel shows a preview, basic info and tags; multi-selection opens a combined Properties window.

![Multi-pane layout and column view in Pulse](docs/images/panes.png)

## Quick Look with the Space bar

Select a file and press Space to preview it without opening another app:

- Markdown is rendered, code is syntax-highlighted, PDFs page through and folders list their contents.
- CSV / TSV / XLSX open as tables; JSON / XML as collapsible trees; Jupyter notebooks cell by cell.
- DOCX, EPUB and Office documents can be read without the original software; archives show a browsable, searchable tree.
- Videos play with seeking and frame stepping, animated WebP / APNG play back, and SVG, PSD, AI, DWG thumbnails, fonts and more are supported.

In video previews, Left / Right seek backward / forward by 5 seconds; hold Shift for 30 seconds or Ctrl for 60 seconds. PageUp / PageDown or Up / Down switch files. Space toggles playback, and comma / period retain frame stepping.

PE files such as EXE and DLL use tabs for properties/version, PE headers/sections, DLL dependencies, imports and exports; Ctrl+Tab switches tabs. Dependencies show candidate paths found by static search, with distinct states for missing modules, missing descendants and unresolved results. Inspection never executes the file. API sets, runtime DLL directories and SxS resolution cannot be determined by file searches alone, so these results do not guarantee that a program will start.

Rest the pointer on a file (0.5 s by default) to get a hover card with its thumbnail or a text excerpt, its type and key facts; press Space while the card shows to open the full preview. The card takes the content's shape (wider for photos and clips, taller for pages and portraits, info only for files without a preview); hold Ctrl to enlarge it. Settings → Quick Look → Preview trigger switches to Space only or changes the hover delay and card size (small / medium / large).

Settings → Supported formats lists every previewable format and checks whether the HEIF / HEVC / AV1 / WebP system extensions are installed.

![Markdown Quick Look in Pulse](docs/images/quicklook.png)

## Staging tray and text compare

Files you copy or cut (Ctrl+C / Ctrl+X) are collected in the staging tray at the bottom left. Gather them across folders and tabs, then drag them to the destination in one go — dropping copies by default, hold Shift to move.

With two files staged you can compare location, modified time, size and content. For text files, "View diff" opens a compare window with side-by-side / unified views, inline change highlighting, differences-only mode, ignore whitespace and case, and F7 / Shift+F7 navigation.

![Text compare from the Pulse staging tray](docs/images/diff.png)

## Fits into Windows

- Make Pulse the default file manager for opening folders, Win+E and the desktop This PC icon; "Open file location" selects the file in Pulse. Turn it off at any time to restore the original behavior.
- The context menu merges Explorer and Windows 11 modern menu items (such as VS Code, Windows Terminal and NanaZip), and Pulse's own entries can be hidden.
- Start with Windows into the tray, set a default location, or restore your last tabs on launch.
- When a file is in use, Pulse shows the locking process and can end it and retry; This PC shows drive types and usage bars.

![Pulse settings and Windows integration](docs/images/settings.png)

## Automatic updates

Pulse checks for a new version after it starts. When an update is available, a prompt appears in the window; click it to download the installer, which opens the setup wizard after it passes verification. You can also check manually in Settings → About & diagnostics, and turn off "Check for updates automatically" on the same page if you don't want reminders.

Downloads can be cancelled; confirm the administrator prompt from Windows during installation. The standard and Windows 8.1 editions each receive their own updates.

The update manifest and installer are fetched through the ghproxy.net and gh-proxy.com mirrors first, switching automatically on connection failure and falling back to GitHub — no setup needed. Every source must pass signature and installer verification.

**Version 1.0.2 and earlier must first install 1.0.3 or later manually; after that, automatic update prompts work.**

## Build from source

Requires Windows x64, the Visual Studio C++ tools, CMake 3.25+ and Ninja. Release builds also need PowerShell 7.2+ and Python 3 (to pack the installer).

Development build:

```powershell
.\build_release.bat
.\build\pulse.exe
```

Build the same installers as an official release and run the regression checks:

```powershell
pwsh ./scripts/build_release_ci.ps1 -Channel windows
pwsh ./scripts/build_release_ci.ps1 -Channel win81 -BuildDir build-ci-win81
```

The release scripts use Visual Studio 2022 v143 and verify the pinned LumaText static-runtime SDK in the repository. `cmake/lumatext-sdk.json` pins the SHA-256 of the SDK manifest, which covers the DLL, import library, headers, CMake exports and license. Development builds can point `LUMATEXT_SOURCE_DIR` at the source; without LumaText, DirectWrite is used.

The stack is C++20, Win32, Direct2D and DirectComposition. File system, indexing, preview and Shell work live in their own modules so they never block the UI.

More: [Automatic updates and releases](docs/automatic-updates.md) · [Windows compatibility](docs/windows-compatibility.md) · [Index storage and migration](docs/index-migration.md)

## License

Pulse source code is licensed under the [Apache License 2.0](LICENSE). Third-party components (such as the LumaText SDK) are covered by their own bundled licenses.
