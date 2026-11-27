#pragma once

#include "../common/picker_tray_consume.h"
#include "folder_picker_model.h"
#include "../common/path_utils.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <memory>
#include <atomic>
#include <functional>
#include <thread>

namespace pulse::ui {
// Selection-time identities survive polling and shelf switches. Never re-resolve
// an accepted path against whichever shelf happens to be current at delivery.
struct PickerTrayPickup {
    picker_places::Snapshot endpoint;
    picker_places::TrayItem item;
};

inline std::function<void()> PickerTrayDelivery(std::vector<PickerTrayPickup> pickups,
                                              const std::vector<std::wstring>& accepted) {
    std::erase_if(pickups, [&](const auto& pickup) {
        return std::none_of(accepted.begin(), accepted.end(), [&](const auto& path) {
            return SamePickerPath(path::StripExtendedPathPrefix(path), path::StripExtendedPathPrefix(pickup.item.path));
        });
    });
    if (pickups.empty()) return {};
    auto sent = std::make_shared<std::atomic<bool>>(false);
    return [pickups = std::move(pickups), sent] {
        if (sent->exchange(true)) return;
        try {
            // The V2 UI DLL is pinned by initialization; HWNDs are checked by
            // the protocol on this worker. No SendMessage wait on the UI STA.
            std::thread([pickups] {
                try {
                    std::vector<bool> used(pickups.size());
                    for (size_t i = 0; i < pickups.size(); ++i) {
                        if (used[i]) continue;
                        const auto& endpoint = pickups[i].endpoint;
                        std::vector<picker_places::TrayItem> items;
                        for (size_t j = i; j < pickups.size(); ++j) {
                            const auto& other = pickups[j].endpoint;
                            if (used[j] || other.endpoint != endpoint.endpoint || other.endpoint_pid != endpoint.endpoint_pid ||
                                other.tray_source != endpoint.tray_source || other.tray_publisher_pid != endpoint.tray_publisher_pid ||
                                other.tray_shelf_id != endpoint.tray_shelf_id) continue;
                            used[j] = true;
                            items.push_back(pickups[j].item);
                        }
                        const auto result = picker_places::RequestConsumeTray(endpoint, items);
                        diagnostics::runtime::Event("picker_tray_consumed", {
                            {"accepted", result.accepted}, {"removed", result.removed}, {"retained_content", result.skipped_owned}});
                    }
                } catch (...) {
                    diagnostics::runtime::Event("picker_tray_consume_failed", {});
                }
            }).detach();
        } catch (...) {
            diagnostics::runtime::Event("picker_tray_consume_start_failed", {});
        }
    };
}
} // namespace pulse::ui
