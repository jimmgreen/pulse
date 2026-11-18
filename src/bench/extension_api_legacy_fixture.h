#pragma once
#include <string_view>
// Frozen public metadata from the host before structured capability discovery.
inline constexpr std::string_view kLegacyExtensionApi = R"LEGACY({
  "apiVersion": 1,
  "developerFeatures": [
    "isolatedDevReload",
    "installedDeclarativePackages",
    "toolbarContributions",
    "panelLayoutPresets",
    "sidebarPanels"
  ],
  "panelPlacements": [
    "window",
    "sidebar"
  ],
  "panelLayouts": [
    "auto",
    "stack",
    "grid"
  ],
  "panelDensities": [
    "comfortable",
    "compact"
  ],
  "capabilities": [
    "navigation.back",
    "navigation.forward",
    "navigation.up",
    "view.refresh",
    "view.setMode"
  ],
  "bindings": [
    "context.directory",
    "context.selectionCount",
    "context.selectionName",
    "context.viewMode"
  ],
  "viewModes": [
    "extra_large_icons",
    "large_icons",
    "medium_icons",
    "small_icons",
    "list",
    "details",
    "tiles",
    "content"
  ],
  "limits": {
    "manifestBytes": 65536,
    "commands": 32,
    "idBytes": 64,
    "labelBytes": 128,
    "panelCommands": 8,
    "panelDescriptionBytes": 512,
    "panelFields": 3,
    "toolbarCommands": 8
  },
  "developmentNotes": [
    "Development preview: only the listed capabilities are public; internal host actions are not extension APIs.",
    "API version remains 1 and old manifests remain supported. Feature-detect capabilities, bindings and viewModes before generating commands for a host.",
    "view.setMode requires args containing exactly mode, selected from viewModes. Other capabilities reject args, even an empty object. context.viewMode displays the current host view mode read-only.",
    "view.setMode targets the focused pane of the active tab in the invoking window and uses normal host view preferences: changes may be saved for that folder. Disabling a package does not restore the previous view. context.viewMode is a localized display label, not a mode ID for command arguments.",
    "Declarative JSON only: no scripts, native libraries, arbitrary processes, network access or custom filesystem access.",
    "Unknown properties, duplicate JSON keys, duplicate command IDs and unsupported capabilities are rejected.",
    "String limits count decoded UTF-8 bytes. Names, titles and labels must be nonempty, not only spaces, and contain no ASCII controls or DEL.",
    "JSON Schema cannot enforce UTF-8 byte limits, unique command IDs, distinct field bindings or panel command references; validate with the host parser too.",
    "Panel buttons must reference 1-8 distinct existing command IDs. Fields are optional, limited to three distinct listed bindings.",
    "Toolbar contributions reference 1-8 distinct local command IDs in declared order, shown in the host toolbar More menu; they do not pin buttons or create a sidebar.",
    "Feature-detect toolbarContributions and panelLayoutPresets. Panel layout defaults to auto; stack is one column, grid prefers two columns with a narrow-width fallback. Density defaults to comfortable; compact uses host spacing presets. No custom CSS, colors or scripts are supported.",
    "Feature-detect sidebarPanels and panelPlacements before requesting panel placement sidebar. Placement defaults to window. Sidebar panels open explicitly in the right workspace and fall back to a floating window when narrow; no author-controlled coordinates or sizes are exposed.",
    "Context values are read-only and displayed inside the host; they are not sent to external code. selectionName is available only for one selected ordinary in-memory entry; otherwise the field uses its empty fallback.",
    "Validation accepts {manifest: string} containing JSON text and has no loading, enabling or execution side effects.",
    "After validation, save UTF-8 JSON and launch Pulse with --extension-manifest followed by its absolute path. --extension-check with an absolute path validates without opening the UI.",
    "Open contributed commands and panels in command mode of the command palette. Disabling the package prevents command execution and closes its panel."
  ]
})LEGACY";
