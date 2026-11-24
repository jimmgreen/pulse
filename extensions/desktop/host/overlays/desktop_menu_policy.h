#pragma once
#include "../../../../src/common/json_utils.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::desktop::overlays {

struct DesktopMenuPolicy {
    enum class Placement { Default, Flat, Submenu, Hidden };
    Placement placement = Placement::Default;
    std::wstring language = L"system";
    std::vector<std::string> hidden;

    static bool Parse(const std::wstring& text, DesktopMenuPolicy& out) {
        if (!json::ValidConfigObject(text)) return false;
        DesktopMenuPolicy parsed;
        const auto extensions = json::ExtractObject(text, L"extensions");
        const auto packages = json::ExtractObject(extensions, L"placement");
        const auto mode = json::ExtractString(json::ExtractObject(packages, L"pulse.desktop"), L"mode");
        if (mode == L"flat") parsed.placement = Placement::Flat;
        else if (mode == L"submenu") parsed.placement = Placement::Submenu;
        else if (mode == L"hidden") parsed.placement = Placement::Hidden;
        json::ForEachMember(json::ExtractObject(extensions, L"commands"), [&](const auto& key, const auto& block) {
            constexpr std::wstring_view prefix = L"native:pulse.desktop:";
            if (key.starts_with(prefix) && !json::ExtractBool(block, L"visible", true)) {
                const auto id = key.substr(prefix.size());
                std::string ascii;
                for (const wchar_t ch : id) {
                    if (ch <= 0x20 || ch > 0x7e) return;
                    ascii.push_back(static_cast<char>(ch));
                }
                parsed.hidden.push_back(std::move(ascii));
            }
        });
        out = std::move(parsed);
        return true;
    }

    bool Visible(std::string_view command) const {
        const std::string_view id = command == "ai-organize" ? "desktop-ai-organize" :
            command == "health-check" ? "desktop-health" :
            command == "beautify" ? "desktop-beautify" :
            command == "toggle-icons" ? "desktop-toggle-icons" : command;
        return std::find(hidden.begin(), hidden.end(), id) == hidden.end();
    }

    static bool Managed(std::string_view id) {
        return id == "ai-organize" || id == "auto-organize" || id == "health-check" ||
            id == "new-group" || id == "new-mapped-group" || id == "sub:add-tile" ||
            id == "tiles-edit" || id == "sub:scene" || id == "beautify";
    }

    template<class Row> void Apply(std::vector<Row>& rows, const std::wstring& title) const {
        const auto filter = [&](auto&& self, std::vector<Row>& items) -> void {
            std::erase_if(items, [&](const Row& row) {
                return !Visible(row.command_id) || (placement == Placement::Hidden &&
                    (Managed(row.command_id) || row.command_id == "toggle-icons"));
            });
            for (auto& row : items) self(self, row.submenu_rows);
        };
        filter(filter, rows);
        if (placement == Placement::Hidden) return;
        Row group;
        group.command_id = "sub:desktop-organize";
        group.label = title;
        for (auto& row : rows) {
            if (row.command_id != "sub:view") continue;
            std::erase_if(row.submenu_rows, [&](Row& child) {
                if (child.command_id != "toggle-icons") return false;
                group.submenu_rows.push_back(std::move(child));
                return true;
            });
        }
        if (placement == Placement::Flat) {
            size_t at = rows.size();
            for (size_t i = 0; i < rows.size(); ++i)
                if (Managed(rows[i].command_id)) at = i + 1;
            for (auto& row : group.submenu_rows) rows.insert(rows.begin() + at++, std::move(row));
            return;
        }
        size_t at = rows.size();
        std::vector<Row> kept;
        for (auto& row : rows) {
            if (Managed(row.command_id)) {
                if (at == rows.size()) at = kept.size();
                group.submenu_rows.push_back(std::move(row));
            } else kept.push_back(std::move(row));
        }
        if (!group.submenu_rows.empty()) kept.insert(kept.begin() + std::min(at, kept.size()), std::move(group));
        rows = std::move(kept);
    }
};

// The desktop owns no second preference file. Read Pulse's persisted menu
// policy off the UI thread, retaining the last valid snapshot during writes.
class DesktopMenuPolicyCache {
public:
    ~DesktopMenuPolicyCache();
    void Start(std::wstring path = {});
    void Stop();
    DesktopMenuPolicy Snapshot() const;
private:
    mutable std::mutex mutex_;
    DesktopMenuPolicy policy_;
    std::jthread worker_;
};

} // namespace pulse::desktop::overlays
