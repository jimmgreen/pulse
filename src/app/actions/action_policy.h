// action_policy.h - What the assistant may do with an action, and how.
// MCP (P5) reads the same answer.
//
//   Direct   Read / View / Organize / Create: runs at once; the card shows
//            old -> new and undoes the call as one group
//   Confirm  External (programs, scripts) and System: a confirm card first,
//            every time (never "always allow")
//   Plan     Move / Delete / PermanentDelete: the file steps of
//            action_files.h show the user a plan card first (P4c): the first
//            batch's for moves, the Recycle Bin card, the red two-click card
//            for a permanent delete; other moves (tray.send_to) stay with
//            the flow tools that have their own cards
//   Locked   UserOnly and the assistant's own limits: Pulse shows the user
//            why and where to change it themselves
#pragma once

#include "action_inventory.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pulse::app::actions {

enum class Gate : uint8_t { Direct, Confirm, Plan, Locked };

struct Policy {
    Gate gate = Gate::Locked;
    ActionRisk risk = ActionRisk::UserOnly;
};

// Programs and scripts: .exe .bat .cmd .ps1 .vbs .msi and the like, and
// shortcuts (a shortcut may start anything). Documents and media are not.
bool IsProgramPath(std::wstring_view path) noexcept;

// The policy for running `id` on `target` (nav.open: the file it opens).
// Unknown ids are Locked. settings.set is Direct: a setting that changes
// Windows still runs at once and its card says so (third batch); the locked
// settings are refused by the settings code itself.
Policy AiPolicy(std::string_view id, std::wstring_view target = {}) noexcept;

const char* RiskName(ActionRisk risk) noexcept;   // "view", "organize", ...

// The switch in 设置 › 增强包 › AI 与 MCP that lets AI apps (MCP) run an
// action: 查看与界面 (read and view),
// 整理 Pulse (organize and settings), 新建, 移动、重命名和删除 (the file
// steps), Pulse 之外的效果 (programs, Git, shell verbs, system); Locked
// actions are only the user's, whatever is switched on.
enum class McpGroup : uint8_t { View, Organize, Create, Files, External, Locked };
inline constexpr size_t kMcpGroupCount = 6;

McpGroup McpGroupOf(std::string_view id, std::wstring_view target = {}) noexcept;
McpGroup McpGroupOfRisk(ActionRisk risk) noexcept;
const char* McpGroupKey(McpGroup group) noexcept;   // "view", "organize", "create", "files", "external", "locked"
bool McpGroupFromKey(std::string_view key, McpGroup& group) noexcept;

} // namespace pulse::app::actions
