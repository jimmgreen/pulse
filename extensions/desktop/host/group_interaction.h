#pragma once
#include "host/model/group_tabs.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace pulse::desktop::interaction {

class GroupHoverSession {
public:
    const std::string& Expanded() const noexcept { return expanded_; }
    bool Pending() const noexcept { return !candidate_.empty() || !expanded_.empty(); }
    void Reset() { expanded_.clear(); candidate_.clear(); enter_at_ = leave_at_ = 0; }
    bool Update(std::string_view under_pointer, bool collapsed, bool keep_open, uint64_t now) {
        const auto previous = expanded_;
        if (!expanded_.empty()) {
            if (under_pointer == expanded_ || keep_open) leave_at_ = 0;
            else if (!leave_at_) leave_at_ = now + 400;
            else if (now >= leave_at_) { expanded_.clear(); leave_at_ = 0; }
        }
        if (collapsed && !under_pointer.empty() && under_pointer != expanded_) {
            if (candidate_ != under_pointer) { candidate_ = under_pointer; enter_at_ = now + 250; }
            else if (now >= enter_at_) {
                expanded_ = candidate_; candidate_.clear(); enter_at_ = leave_at_ = 0;
            }
        } else { candidate_.clear(); enter_at_ = 0; }
        return previous != expanded_;
    }
private:
    std::string expanded_, candidate_;
    uint64_t enter_at_ = 0, leave_at_ = 0;
};

class GroupDragSession {
public:
    bool Pending() const noexcept { return !original_.empty(); }
    bool Active() const noexcept { return active_; }
    bool Resizing() const noexcept { return resizing_; }
    const std::vector<model::DesktopGroup>& Original() const noexcept { return original_; }
    const std::string& MergeTarget() const noexcept { return target_; }
    bool MergeReady(uint64_t now) const noexcept { return !target_.empty() && now >= merge_at_; }
    const model::RectI& TargetRect() const noexcept { return target_rect_; }
    std::string DraggingGroupId() const noexcept {
        return Pending() && index_ < original_.size() ? original_[index_].id : std::string{};
    }
    void Begin(const std::vector<model::DesktopGroup>& groups,
               const std::vector<model::GroupComputedView>& views, size_t index,
               model::PointI press, bool resize, model::RectI work,
               model::DesktopIconSize icons, std::vector<model::RectI> obstacles) {
        Reset();
        if (index >= groups.size()) return;
        original_ = groups; original_views_ = views; index_ = index;
        members_ = model::TabMembers(original_, original_[index_].id);
        start_ = press; last_ = press; resizing_ = resize; work_ = work; icons_ = icons;
        obstacles_ = std::move(obstacles);
        target_rect_ = (index_ < original_views_.size() && !original_views_[index_].frame.empty())
            ? original_views_[index_].frame
            : model::ComputeGroupFrame(original_[index_], icons_);
    }
    bool Update(std::vector<model::DesktopGroup>& groups, model::PointI point, uint64_t now) {
        if (!Pending()) return false;
        if (!active_ && std::abs(point.x - start_.x) < 6 && std::abs(point.y - start_.y) < 6) return false;
        active_ = true;
        last_ = point;
        groups = original_; // Always replay from press, never accumulate collision pushes.
        const auto& source = original_[index_];
        if (resizing_) {
            const auto cell = model::IconCellMetrics(icons_);
            const int cols = std::clamp(source.cols + static_cast<int>(std::lround(
                static_cast<double>(point.x - start_.x) / cell.x)), model::kGroupMinCols, model::kGroupMaxCols);
            const int rows = std::clamp(source.visible_rows + static_cast<int>(std::lround(
                static_cast<double>(point.y - start_.y) / cell.y)), model::kGroupMinRows, model::kGroupMaxRows);
            for (const size_t member : members_) {
                groups[member].cols = cols;
                groups[member].visible_rows = rows;
            }
            target_.clear();
            target_rect_ = {};
        } else {
            const std::string next = MergeTargetAt(point);
            if (next != target_) { target_ = next; merge_at_ = now + 600; }
            // Dragged file group follows the pointer smoothly 1:1 while all stationary groups remain calm.
            const auto frame = EffectiveFrame();
            const int x = std::clamp(source.x + point.x - start_.x, work_.x, std::max(work_.x, work_.right() - frame.w));
            const int y = std::clamp(source.y + point.y - start_.y, work_.y, std::max(work_.y, work_.bottom() - frame.h));
            for (const size_t member : members_) { groups[member].x = x; groups[member].y = y; }
            UpdateDropTargetRect(x, y, now);
        }
        return true;
    }
    // Refreshes ghost target rectangle when 600ms dwell timer arms tab-merge while pointer is still.
    bool RefreshTargetRect(uint64_t now) {
        if (!Pending() || !active_ || resizing_) return false;
        const model::RectI prev = target_rect_;
        const auto& source = original_[index_];
        const auto frame = EffectiveFrame();
        const int x = std::clamp(source.x + last_.x - start_.x, work_.x, std::max(work_.x, work_.right() - frame.w));
        const int y = std::clamp(source.y + last_.y - start_.y, work_.y, std::max(work_.y, work_.bottom() - frame.h));
        UpdateDropTargetRect(x, y, now);
        return target_rect_ != prev;
    }
    bool Commit(std::vector<model::DesktopGroup>& groups, uint64_t now, bool& merged) {
        merged = false;
        if (!Pending()) return false;
        if (active_ && MergeReady(now)) {
            auto merged_groups = original_;
            if (model::MergeGroupTabs(merged_groups, original_[index_].id, target_)) {
                groups = std::move(merged_groups); merged = true;
            } else {
                groups = original_; Reset(); return false;
            }
        } else if (active_) {
            const int cols = groups[index_].cols, rows = groups[index_].visible_rows;
            groups = original_;
            const auto& source = original_[index_];
            if (resizing_) {
                model::ResizeGroupColumns(groups, index_, cols, rows, work_, {}, icons_, obstacles_);
            } else {
                const int px = source.x + last_.x - start_.x;
                const int py = source.y + last_.y - start_.y;
                if (std::abs(last_.x - start_.x) <= 6 && std::abs(last_.y - start_.y) <= 6) {
                    // Returned to start: leave original layout untouched.
                } else if (auto spot = model::FindNearestFreeGroupSpot(
                               original_, index_, px, py, work_, {}, icons_, obstacles_)) {
                    for (const size_t member : members_) {
                        groups[member].x = spot->x;
                        groups[member].y = spot->y;
                        groups[member].rest_origin.reset();
                    }
                } else {
                    model::MoveGroupInteractive(groups, index_, px, py, work_, {}, icons_, obstacles_);
                }
            }
        }
        bool changed = merged;
        if (active_ && !changed && groups.size() == original_.size()) {
            for (size_t i = 0; i < groups.size(); ++i) {
                const auto& a = groups[i]; const auto& b = original_[i];
                changed = changed || a.x != b.x || a.y != b.y || a.cols != b.cols || a.visible_rows != b.visible_rows;
            }
        }
        if (changed) model::CommitGroupRestOrigins(groups);
        else groups = original_;
        Reset();
        return changed;
    }
    void Cancel(std::vector<model::DesktopGroup>& groups) { if (Pending()) groups = original_; Reset(); }
    void Reset() {
        original_.clear(); original_views_.clear(); members_.clear(); obstacles_.clear(); target_.clear();
        target_rect_ = {};
        active_ = resizing_ = false; merge_at_ = 0;
    }
private:
    model::RectI EffectiveFrame() const {
        if (index_ < original_views_.size() && !original_views_[index_].frame.empty()) {
            return original_views_[index_].frame;
        }
        return model::ComputeGroupFrame(original_[index_], icons_);
    }
    void UpdateDropTargetRect(int proposed_x, int proposed_y, uint64_t now) {
        if (MergeReady(now)) {
            for (size_t i = 0; i < original_.size() && i < original_views_.size(); ++i) {
                if (original_[i].id == target_ && !original_views_[i].frame.empty()) {
                    target_rect_ = original_views_[i].frame;
                    return;
                }
            }
        }
        const auto& source = original_[index_];
        const auto frame = EffectiveFrame();
        if (std::abs(last_.x - start_.x) <= 6 && std::abs(last_.y - start_.y) <= 6) {
            target_rect_ = {source.x, source.y, frame.w, frame.h};
            return;
        }
        if (auto spot = model::FindNearestFreeGroupSpot(
                original_, index_, proposed_x, proposed_y, work_, {}, icons_, obstacles_)) {
            target_rect_ = {spot->x, spot->y, frame.w, frame.h};
            return;
        }
        auto copy = original_;
        model::MoveGroupInteractive(copy, index_, proposed_x, proposed_y, work_, {}, icons_, obstacles_);
        target_rect_ = {copy[index_].x, copy[index_].y, frame.w, frame.h};
    }
    std::string MergeTargetAt(model::PointI point) const {
        // Hit-test the press snapshot, not solver-displaced or preview geometry.
        for (size_t i = 0; i < original_views_.size() && i < original_.size(); ++i) {
            if (std::find(members_.begin(), members_.end(), i) != members_.end()) continue;
            const auto& frame = original_views_[i].frame;
            if (frame.empty()) continue;
            const model::RectI header{frame.x, frame.y, frame.w, model::kGroupHeaderHeight};
            const model::RectI center{frame.x + frame.w / 4, frame.y + frame.h / 4, frame.w / 2, frame.h / 2};
            if (header.contains(point.x, point.y) || center.contains(point.x, point.y)) return original_[i].id;
        }
        return {};
    }

    std::vector<model::DesktopGroup> original_;
    std::vector<model::GroupComputedView> original_views_;
    std::vector<size_t> members_;
    std::vector<model::RectI> obstacles_;
    size_t index_ = 0;
    model::PointI start_{}, last_{};
    model::RectI work_{};
    model::RectI target_rect_{};
    model::DesktopIconSize icons_ = model::DesktopIconSize::Medium;
    bool active_ = false, resizing_ = false;
    std::string target_;
    uint64_t merge_at_ = 0;
};
}
