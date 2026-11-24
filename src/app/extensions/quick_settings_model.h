#pragma once

#include "manifest.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulse::extensions {

inline constexpr int kStoreQuickControlActionBase = 100000;
inline constexpr int kStoreQuickControlStride = 16;
inline constexpr size_t kMaxStoreQuickCards = 8;
inline constexpr size_t kMaxStoreQuickControlsPerCard = 8;

inline int EncodeQuickControlAction(size_t card_idx, size_t ctrl_idx) noexcept {
    if (card_idx >= kMaxStoreQuickCards || ctrl_idx >= kMaxStoreQuickControlsPerCard) return -1;
    return kStoreQuickControlActionBase +
           static_cast<int>(card_idx * static_cast<size_t>(kStoreQuickControlStride) + ctrl_idx);
}

inline bool DecodeQuickControlAction(int action, size_t& out_card, size_t& out_ctrl) noexcept {
    if (action < kStoreQuickControlActionBase) return false;
    const int offset = action - kStoreQuickControlActionBase;
    const size_t card = static_cast<size_t>(offset / kStoreQuickControlStride);
    const size_t ctrl = static_cast<size_t>(offset % kStoreQuickControlStride);
    if (card >= kMaxStoreQuickCards || ctrl >= kMaxStoreQuickControlsPerCard) return false;
    out_card = card;
    out_ctrl = ctrl;
    return true;
}

struct ResolvedQuickControl {
    QuickSettingControlKind kind = QuickSettingControlKind::Switch;
    size_t flat_index = 0;
    std::wstring label;
    bool checked = false;
    bool disabled = false;
    uint32_t width_dip = 0;
    std::vector<std::wstring> option_labels;
    int selected_option = 0;
};

struct ResolvedQuickRow {
    std::wstring title;
    std::wstring description;
    std::vector<ResolvedQuickControl> controls;
};

struct ResolvedQuickCard {
    std::wstring title;
    bool available = false;
    std::wstring status;
    std::vector<ResolvedQuickRow> rows;
    bool has_menu_preview = false;
    std::wstring preview_caption;
    bool preview_submenu = false;
};

struct QuickControlEdit {
    bool is_click = false;
    std::string key_or_command;
    std::string value;
    bool operator==(const QuickControlEdit&) const = default;
};

namespace detail {
inline std::wstring Utf8ToWide(std::string_view utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        const uint8_t b0 = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = 0xFFFDu;
        if (b0 < 0x80u) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0u) == 0xC0u && i + 1 < utf8.size()) {
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
            i += 2;
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < utf8.size()) {
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            }
            i += 3;
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < utf8.size()) {
            const uint8_t b1 = static_cast<uint8_t>(utf8[i + 1]);
            const uint8_t b2 = static_cast<uint8_t>(utf8[i + 2]);
            const uint8_t b3 = static_cast<uint8_t>(utf8[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
                cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
            }
            i += 4;
        } else {
            i += 1;
        }
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<wchar_t>(cp));
        } else if (cp <= 0x10FFFFu) {
            cp -= 0x10000u;
            out.push_back(static_cast<wchar_t>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu)));
        }
    }
    return out;
}

inline bool IsDisabledByCondition(const ManifestQuickSettingCondition& cond,
                                  const std::function<std::optional<std::string>(const std::string&)>& read_state) {
    if (!cond.present) return false;
    const auto val = read_state ? read_state(cond.state) : std::nullopt;
    return val.value_or("") == cond.equals;
}

inline const ManifestQuickSettingControl* FindFlatControl(const ManifestQuickSettings& decl, size_t target_idx) noexcept {
    size_t idx = 0;
    for (const auto& row : decl.rows) {
        for (const auto& ctrl : row.controls) {
            if (idx == target_idx) return &ctrl;
            ++idx;
        }
    }
    return nullptr;
}
} // namespace detail

inline ResolvedQuickCard ResolveQuickSettingsCard(
    const ManifestQuickSettings& decl,
    const std::function<std::optional<std::string>(const std::string&)>& read_state,
    const std::function<std::wstring(const ManifestLocalizedText&)>& localize,
    bool page_present) {
    ResolvedQuickCard out;
    out.title = localize(decl.title);
    if (decl.menu_preview) {
        out.has_menu_preview = true;
        out.preview_caption = localize(decl.menu_preview->caption);
    }
    if (!page_present) return out;
    if (!decl.ready_state.empty()) {
        const auto ready = read_state ? read_state(decl.ready_state) : std::nullopt;
        if (!ready || *ready != "true") return out;
    }
    out.available = true;
    if (!decl.status_state.empty() && read_state) {
        if (const auto st = read_state(decl.status_state); st && !st->empty()) {
            out.status = detail::Utf8ToWide(*st);
        }
    }
    if (decl.menu_preview && !decl.menu_preview->submenu_state.empty() && read_state) {
        const auto sub = read_state(decl.menu_preview->submenu_state);
        out.preview_submenu = sub && *sub == "true";
    }

    size_t flat_idx = 0;
    out.rows.reserve(decl.rows.size());
    for (const auto& rdecl : decl.rows) {
        ResolvedQuickRow row;
        row.title = localize(rdecl.title);
        row.description = localize(rdecl.description);
        row.controls.reserve(rdecl.controls.size());
        for (const auto& ctrl_decl : rdecl.controls) {
            ResolvedQuickControl ctrl;
            ctrl.kind = ctrl_decl.kind;
            ctrl.flat_index = flat_idx++;
            ctrl.disabled = detail::IsDisabledByCondition(ctrl_decl.disabled_when, read_state);
            ctrl.width_dip = ctrl_decl.width_dip;
            if (ctrl_decl.kind == QuickSettingControlKind::Switch) {
                const auto val = read_state ? read_state(ctrl_decl.state) : std::nullopt;
                ctrl.checked = val && *val == "true";
            } else if (ctrl_decl.kind == QuickSettingControlKind::Select) {
                const auto val = read_state ? read_state(ctrl_decl.state) : std::nullopt;
                ctrl.option_labels.reserve(ctrl_decl.options.size());
                ctrl.selected_option = 0;
                for (size_t i = 0; i < ctrl_decl.options.size(); ++i) {
                    ctrl.option_labels.push_back(localize(ctrl_decl.options[i].label));
                    if (val && *val == ctrl_decl.options[i].value) {
                        ctrl.selected_option = static_cast<int>(i);
                    }
                }
                const std::wstring disabled_lbl = localize(ctrl_decl.disabled_label);
                if (ctrl.disabled && !disabled_lbl.empty()) {
                    ctrl.label = disabled_lbl;
                } else if (!ctrl.option_labels.empty()) {
                    ctrl.label = ctrl.option_labels[static_cast<size_t>(ctrl.selected_option)];
                }
            } else {
                ctrl.label = localize(ctrl_decl.label);
            }
            row.controls.push_back(std::move(ctrl));
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

inline std::optional<QuickControlEdit> ComputeQuickControlEdit(
    const ManifestQuickSettings& decl,
    size_t flat_ctrl_idx,
    const std::function<std::optional<std::string>(const std::string&)>& read_state,
    int chosen_option = -1) {
    const auto* ctrl = detail::FindFlatControl(decl, flat_ctrl_idx);
    if (!ctrl || detail::IsDisabledByCondition(ctrl->disabled_when, read_state)) return std::nullopt;
    QuickControlEdit edit;
    if (ctrl->kind == QuickSettingControlKind::Switch) {
        const auto current = read_state ? read_state(ctrl->state) : std::nullopt;
        edit.is_click = false;
        edit.key_or_command = ctrl->state;
        edit.value = (current && *current == "true") ? "false" : "true";
        return edit;
    }
    if (ctrl->kind == QuickSettingControlKind::Select) {
        if (chosen_option < 0 || static_cast<size_t>(chosen_option) >= ctrl->options.size()) return std::nullopt;
        edit.is_click = false;
        edit.key_or_command = ctrl->state;
        edit.value = ctrl->options[static_cast<size_t>(chosen_option)].value;
        return edit;
    }
    edit.is_click = true;
    edit.key_or_command = ctrl->command;
    return edit;
}

} // namespace pulse::extensions
