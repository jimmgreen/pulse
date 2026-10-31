#pragma once
#include <dwrite.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace pulse::ui {
class PlainHighlightLines {
public:
    void Reset() { std::vector<Line>().swap(lines_); attempted_ = false; }
    bool Range(IDWriteTextLayout* layout, float top, float bottom,
               uint32_t& begin, uint32_t& end) {
        if (!attempted_) {
            attempted_ = true;
            UINT32 count = 0;
            layout->GetLineMetrics(nullptr, 0, &count);
            if (!count || count > 65536) return false;
            std::vector<DWRITE_LINE_METRICS> metrics(count);
            if (FAILED(layout->GetLineMetrics(metrics.data(), count, &count))) return false;
            std::vector<Line> lines;
            lines.reserve(count);
            uint32_t offset = 0;
            DWRITE_TEXT_METRICS text_metrics{};
            if (FAILED(layout->GetMetrics(&text_metrics))) return false;
            float y = text_metrics.top;
            for (const auto& metric : metrics) {
                offset += metric.length;
                y += metric.height;
                lines.push_back({offset, y});
            }
            lines_ = std::move(lines);
        }
        if (lines_.empty()) return false;
        // Retain touching lines too: fractional rasterization must match the
        // original clipped drawing at both viewport edges.
        auto first = std::lower_bound(lines_.begin(), lines_.end(), top,
            [](const Line& line, float value) { return line.bottom < value; });
        begin = first == lines_.begin() ? 0 : (first - 1)->end;
        auto last = std::upper_bound(first, lines_.end(), bottom,
            [](float value, const Line& line) { return value < line.bottom; });
        end = last == lines_.end() ? lines_.back().end : last->end;
        return true;
    }
private:
    struct Line { uint32_t end; float bottom; };
    std::vector<Line> lines_;
    bool attempted_ = false;
};
}
