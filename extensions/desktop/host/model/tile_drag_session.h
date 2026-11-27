#pragma once
#include "tile_reorder.h"
#include <algorithm>
#include <cstdlib>
#include <optional>

namespace pulse::desktop::model {

enum class TileDragPhase { Reorder, Approach, StackPending, StackArmed };

// Reference-aligned tile drag session:
// - During drag, the dragged tile follows the pointer smoothly 1:1 while all other tiles remain calm.
// - Target ghost snaps to the nearest non-overlapping free grid cell (avoiding other tiles & groups).
// - Holding still over a stackable tile's core for 600 ms arms stacking onto that tile.
// - Commit places the dragged tile at the snapped target (caller launches spring snap-back).
class TileDragSession {
public:
    bool Begin(const std::vector<DesktopTile>& tiles, size_t index, RectI work, RectI board,
               std::span<const RectI> groups) {
        Reset();
        if (index >= tiles.size()) return false;
        original_ = preview_ = reorder_preview_ = tiles;
        index_ = index;
        work_ = work;
        board_ = board;
        groups_.assign(groups.begin(), groups.end());
        origin_rect_ = target_ = reorder_target_ = proposal_rect_ = ResolveTileRect(tiles[index], board);
        active_ = valid_ = reorder_valid_ = true;
        return true;
    }

    bool Update(PointI proposed, PointI pointer, uint64_t now) {
        if (!active_) return false;
        const auto prior_phase = phase_;
        const auto prior_candidate = candidate_;
        const auto prior_target = target_;
        const auto prior_proposal = proposal_rect_;
        const bool prior_valid = valid_;

        proposal_rect_ = origin_rect_;
        proposal_rect_.x = proposed.x;
        proposal_rect_.y = proposed.y;
        if (!work_.empty()) {
            proposal_rect_.x = std::clamp(proposal_rect_.x, work_.x, std::max(work_.x, work_.right() - proposal_rect_.w));
            proposal_rect_.y = std::clamp(proposal_rect_.y, work_.y, std::max(work_.y, work_.bottom() - proposal_rect_.h));
        }

        const bool planned = PlanNearestFree({proposal_rect_.x, proposal_rect_.y});
        const auto candidate = FindCandidate({proposal_rect_.x, proposal_rect_.y}, pointer);
        stack_target_.clear();
        if (candidate.empty()) {
            candidate_.clear();
            candidate_rect_ = {};
            phase_ = TileDragPhase::Reorder;
        } else {
            candidate_ = candidate;
            for (const auto& tile : original_) {
                if (tile.id == candidate_) candidate_rect_ = ResolveTileRect(tile, board_);
            }
            if (!Core(candidate_rect_).contains(pointer.x, pointer.y)) {
                phase_ = TileDragPhase::Approach;
            } else {
                if (candidate_ != prior_candidate ||
                    (prior_phase != TileDragPhase::StackPending && prior_phase != TileDragPhase::StackArmed) ||
                    Distance(pointer.x, dwell_anchor_.x) > 6 || Distance(pointer.y, dwell_anchor_.y) > 6) {
                    dwell_anchor_ = pointer;
                    dwell_start_ = now;
                }
                phase_ = now >= dwell_start_ && now - dwell_start_ >= 600
                    ? TileDragPhase::StackArmed : TileDragPhase::StackPending;
                if (phase_ == TileDragPhase::StackArmed) stack_target_ = candidate_;
            }
        }

        // Visual preview during drag: non-dragged tiles stay completely stationary while the
        // dragged tile follows the pointer smoothly 1:1 (matching reference widget_demo.cpp).
        preview_ = original_;
        auto& moving = preview_[index_];
        moving.free_placed = true;
        moving.x = proposal_rect_.x;
        moving.y = proposal_rect_.y;
        moving.w = proposal_rect_.w;
        moving.h = proposal_rect_.h;
        moving.rest_origin.reset();

        target_ = phase_ == TileDragPhase::StackArmed ? candidate_rect_ : reorder_target_;
        valid_ = phase_ == TileDragPhase::StackArmed || reorder_valid_;
        return planned || phase_ != prior_phase || candidate_ != prior_candidate ||
               target_ != prior_target || valid_ != prior_valid || proposal_rect_ != prior_proposal;
    }

    bool Commit(std::vector<DesktopTile>& tiles) {
        if (!active_ || !reorder_valid_ || !stack_target_.empty()) return false;
        tiles = reorder_preview_;
        CommitTileRestOrigins(tiles);
        Reset();
        return true;
    }
    void Cancel(std::vector<DesktopTile>& tiles) {
        if (active_) tiles = original_;
        Reset();
    }
    void Reset() { *this = TileDragSession{}; }
    bool Active() const noexcept { return active_; }
    bool Valid() const noexcept { return valid_; }
    bool HasStackCandidate() const noexcept { return !candidate_.empty(); }
    bool NeedsDwellTimer() const noexcept { return phase_ == TileDragPhase::StackPending; }
    TileDragPhase Phase() const noexcept { return phase_; }
    const std::string& PendingCandidate() const noexcept { return candidate_; }
    const RectI& CandidateRect() const noexcept { return candidate_rect_; }
    const RectI& ProposalRect() const noexcept { return proposal_rect_; }
    bool PlacementChanged() const noexcept {
        return reorder_valid_ && (reorder_target_ != origin_rect_);
    }
    const std::string& StackTarget() const noexcept { return stack_target_; }
    const RectI& Target() const noexcept { return target_; }
    const std::vector<DesktopTile>& Original() const noexcept { return original_; }
    const std::vector<DesktopTile>& Preview() const noexcept { return preview_; }
    size_t PlanningCount() const noexcept { return planning_count_; }

private:
    static int64_t Distance(int a, int b) noexcept { return std::abs(static_cast<int64_t>(a) - b); }
    static RectI Core(const RectI& r) noexcept {
        const int x = std::clamp(r.w / 5, 12, 32), y = std::clamp(r.h / 5, 12, 32);
        return {r.x + x, r.y + y, std::max(1, r.w - 2 * x), std::max(1, r.h - 2 * y)};
    }
    std::string FindCandidate(PointI proposed, PointI pointer) const {
        RectI dragged = origin_rect_;
        dragged.x = proposed.x;
        dragged.y = proposed.y;
        std::string approach;
        for (const auto& tile : original_) {
            if (tile.id == original_[index_].id || !CanStackTiles(original_, original_[index_].id, tile.id)) continue;
            const auto r = ResolveTileRect(tile, board_); // rest_origin is not visible geometry.
            if (Core(r).contains(pointer.x, pointer.y)) return tile.id;
            if (dragged.intersects(r)) {
                if (tile.id == candidate_) approach = tile.id;
                else if (approach.empty()) approach = tile.id;
            }
        }
        return approach;
    }
    bool PlanNearestFree(PointI proposed) {
        if (Distance(proposed.x, origin_rect_.x) <= 6 && Distance(proposed.y, origin_rect_.y) <= 6) {
            const bool changed = (reorder_target_ != origin_rect_) || !reorder_valid_ || cell_.has_value();
            reorder_preview_ = original_;
            reorder_target_ = origin_rect_;
            reorder_valid_ = true;
            cell_.reset();
            last_planned_proposal_ = proposed;
            return changed;
        }
        if (last_planned_proposal_ && *last_planned_proposal_ == proposed) {
            return false;
        }
        const auto result = FindNearestFreeTileCell(original_, index_, proposed, work_, board_, groups_);
        ++planning_count_;
        last_planned_proposal_ = proposed;
        const bool changed = (reorder_target_ != result.rect) || (reorder_valid_ != result.valid);
        cell_ = result.cell;
        reorder_target_ = result.rect;
        reorder_valid_ = result.valid;
        reorder_preview_ = original_;
        if (reorder_valid_) {
            auto& dst = reorder_preview_[index_];
            dst.free_placed = true;
            dst.x = reorder_target_.x;
            dst.y = reorder_target_.y;
            dst.w = reorder_target_.w;
            dst.h = reorder_target_.h;
            dst.rest_origin.reset();
        }
        return changed;
    }

    std::vector<DesktopTile> original_, preview_, reorder_preview_;
    std::vector<RectI> groups_;
    size_t index_ = 0, planning_count_ = 0;
    RectI work_, board_, origin_rect_, target_, reorder_target_, candidate_rect_, proposal_rect_;
    std::optional<PointI> cell_;
    std::optional<PointI> last_planned_proposal_;
    bool active_ = false, valid_ = false, reorder_valid_ = false;
    TileDragPhase phase_ = TileDragPhase::Reorder;
    std::string candidate_, stack_target_;
    PointI dwell_anchor_;
    uint64_t dwell_start_ = 0;
};

} // namespace pulse::desktop::model
