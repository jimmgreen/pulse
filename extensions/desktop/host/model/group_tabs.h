#pragma once
#include "desktop_model.h"

namespace pulse::desktop::model {
inline constexpr size_t kMaxGroupTabs = 8;
std::vector<size_t> TabMembers(const std::vector<DesktopGroup>& groups, std::string_view group_id);
void NormalizeGroupTabs(std::vector<DesktopGroup>& groups);
bool MergeGroupTabs(std::vector<DesktopGroup>& groups, std::string_view moving_id, std::string_view target_id);
bool SelectGroupTab(std::vector<DesktopGroup>& groups, std::string_view group_id);
bool DetachGroupTab(std::vector<DesktopGroup>& groups, std::string_view group_id,
                   const RectI& work_area, const RectI& board_rect,
                   DesktopIconSize icon_size = DesktopIconSize::Medium,
                   std::span<const RectI> extra_obstacles = {});
}
