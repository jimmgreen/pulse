// tag_rules.h — 自动规则 (pulse_tag_redesign P2): "when a file appears in
// [folder] and [its name contains / its extension is] → [ask / tag it]
// [tag]". Asking is the default (a snack offers 加上); rules only run while
// Pulse does. Saved in <data dir>\tag_rules.json.
#pragma once
#include "app_runtime.h"

#include <vector>

namespace pulse {
// (Re)loads the rules and watches their folders; hits go to `target`.
void StartTagRules(HWND target);
void StopTagRules();
int RunTagRuleReuseTest();
// The window that handles hits (the last active one).
void RetargetTagRules(HWND target);
// WM_TAG_RULE_HIT: files appeared — tag them or offer to.
void HandleTagRuleHits(AppState& s);
// WM_TAG_RULE_ACCEPT: the snack's 加上.
void AcceptTagRuleSuggestion(AppState& s);
// The tag menu's 自动规则 ▸ (its rules, click removes one; 新建规则…).
// Commands: new_command, and rule_base + n for the menu's n-th rule.
void AppendTagRulesMenu(const app::ColorTag& tag, std::vector<ui::FluentMenuItem>& items,
                        int new_command, int rule_base);
// Runs a command from that submenu; false when it is not one.
bool HandleTagRulesCommand(AppState& s, const std::wstring& tag_id, int command, int new_command,
                           int rule_base, POINT screen_pt);
}
