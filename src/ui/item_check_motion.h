#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace pulse::ui {
class ItemCheckMotion {
public:
    struct Sample { float progress; bool active; };
    void Begin(const std::wstring& path, uint64_t frame) {
        if (path_ != path) {
            path_ = path;
            rows_.clear();
        }
        std::erase_if(rows_, [frame](const auto& row) { return frame > row.second.seen + 2; });
    }
    Sample Update(const std::wstring& path, bool selected, uint64_t frame, uint64_t now, bool animate) {
        auto [it, added] = rows_.try_emplace(path, Transition{selected, 0, frame});
        auto& value = it->second;
        if (!added && value.selected != selected) {
            value.selected = selected;
            value.started = animate && selected ? now : 0;
        }
        value.seen = frame;
        if (!animate) value.started = 0;
        const float progress = value.started
            ? std::clamp(static_cast<float>(now - value.started) / 140.0f, 0.0f, 1.0f) : 1.0f;
        const bool active = value.started && progress < 1.0f;
        if (!active) value.started = 0;
        return {progress, active};
    }
private:
    struct Transition { bool selected; uint64_t started; uint64_t seen; };
    std::wstring path_;
    std::unordered_map<std::wstring, Transition> rows_;
};
} // namespace pulse::ui
