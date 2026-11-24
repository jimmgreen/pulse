// Include the implementation to exercise its private validator without registry IO.
#include "../common/picker_layout_settings.cpp"
#include "../ui/picker_custom_layout.h"
#include <cstdio>

namespace {
bool BoxesOverlap(const pulse::ui::PickerFlowBox& a, const pulse::ui::PickerFlowBox& b) {
    if (!a.visible || !b.visible) return false;
    return a.left < b.right - 0.1f && b.left < a.right - 0.1f &&
           a.top < b.bottom - 0.1f && b.top < a.bottom - 0.1f;
}
}

int main() {
    using namespace pulse::picker_layout;
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](bool okay, const char* label) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
        if (!okay) ++failures;
    };
    State state;
    check(Valid(state), "default layout validates without registry IO");
    state.collapsed_mask = 1u << 8;
    check(Valid(state), "network ninth collapse bit is accepted");
    state.collapsed_mask = 511;
    check(Valid(state), "all nine sidebar collapse bits are accepted");
    state.collapsed_mask = 1u << 9;
    check(!Valid(state), "unknown tenth collapse bit is rejected");
    state.collapsed_mask = 0xffffffffu;
    check(!Valid(state), "unknown high collapse bits are rejected");

    // Mnemonic display formatting for IFileDialogCustomize captions.
    check(FormatCustomControlCaption(L"ICC \x914D\x7F6E\x6587\x4EF6(&C): sRGB IEC61966-2.1") == L"ICC \x914D\x7F6E\x6587\x4EF6: sRGB IEC61966-2.1",
          "strips CJK (&C) mnemonic cleanly");
    check(FormatCustomControlCaption(L"\x7F29\x89C8\x56FE(&T)") == L"\x7F29\x89C8\x56FE",
          "strips trailing (&T) mnemonic cleanly");
    check(FormatCustomControlCaption(L"&Save Copy...") == L"Save Copy...",
          "strips bare English & mnemonic cleanly");
    check(FormatCustomControlCaption(L"Black && White") == L"Black & White",
          "unescapes literal && ampersand");
    check(IsInlineColonCaption(L"\x989C\x8272:") && IsInlineColonCaption(L"\x5176\x5B83\xFF1A") && !IsInlineColonCaption(L"\x5B58\x50A8\x9009\x9879"),
          "distinguishes inline colon labels from section headings");

    // Photoshop 2022 Save As layout: section header + two inline groups + separator + push button.
    {
        std::vector<PickerCustomFlowItem> ps;
        ps.push_back({1, 10, true, false, false, 0, true, false, 60.0f, {}});   // 存储选项
        ps.push_back({10, 11, true, false, true, 10, true, true, 36.0f, {}});   // 颜色:
        ps.push_back({7, 12, true, false, true, 10, true, false, 240.0f, {}});  // ICC 配置文件: sRGB...
        ps.push_back({kPickerCustomSeparator, 13, true, false, true, 10, false, false, 0.0f, {}});
        ps.push_back({10, 14, true, false, true, 10, true, true, 36.0f, {}});   // 其它:
        ps.push_back({7, 15, true, false, true, 10, true, false, 84.0f, {}});   // 缩览图
        ps.push_back({2, 16, true, false, true, 10, true, false, 108.0f, {}});  // 存储副本...

        const auto wide = LayoutPickerCustomFlow(1000.0f, 220.0f, ps);
        check(wide.viewport_height <= 80.0f && !wide.has_scrollbar,
              "PS Save As packs into compact <=80 DIP panel at 1000 DIP width");
        check(wide.boxes[3].visible && wide.boxes[3].vertical_separator,
              "PS separator turns into vertical divider when inline groups fit on one row");
        check(wide.boxes[6].right - wide.boxes[6].left <= 120.0f && wide.boxes[6].top == wide.boxes[2].top,
              "PS Save Copy button keeps natural width on the same row instead of full-width stretch");
        bool any_overlap = false;
        for (size_t i = 0; i < wide.boxes.size(); ++i)
            for (size_t j = i + 1; j < wide.boxes.size(); ++j)
                any_overlap |= BoxesOverlap(wide.boxes[i], wide.boxes[j]);
        check(!any_overlap, "PS wide layout has zero overlapping controls");

        const auto narrow = LayoutPickerCustomFlow(460.0f, 220.0f, ps);
        bool narrow_fits = true;
        bool narrow_overlap = false;
        for (size_t i = 0; i < narrow.boxes.size(); ++i) {
            if (narrow.boxes[i].visible && narrow.boxes[i].right > 460.0f - 15.5f) narrow_fits = false;
            for (size_t j = i + 1; j < narrow.boxes.size(); ++j)
                narrow_overlap |= BoxesOverlap(narrow.boxes[i], narrow.boxes[j]);
        }
        check(narrow_fits && !narrow_overlap && narrow.boxes[6].top > narrow.boxes[2].top,
              "PS narrow layout wraps cleanly with zero overflow or overlap");
    }

    // Heavy multi-parameter dialog (12 controls across combos, checkboxes, radios, edit, buttons + prominent).
    {
        std::vector<PickerCustomFlowItem> heavy;
        heavy.push_back({1, 1, true, false, false, 0, true, false, 80.0f, {}});
        heavy.push_back({10, 2, true, false, true, 1, true, true, 48.0f, {}});
        heavy.push_back({3, 3, true, false, true, 1, true, false, 160.0f, {}});
        heavy.push_back({10, 4, true, false, true, 1, true, true, 48.0f, {}});
        heavy.push_back({3, 5, true, false, true, 1, true, false, 150.0f, {}});
        heavy.push_back({10, 6, true, false, true, 1, true, true, 40.0f, {}});
        heavy.push_back({8, 7, true, false, true, 1, false, false, 140.0f, {}});
        heavy.push_back({kPickerCustomSeparator, 8, true, false, false, 0, false, false, 0.0f, {}});
        for (uint32_t k = 0; k < 5; ++k) {
            heavy.push_back({7, 10 + k, true, false, false, 0, true, false, 130.0f, {}});
        }
        heavy.push_back({6, 20, true, false, false, 0, false, false, 0.0f, {96.0f, 110.0f, 76.0f}, {78.0f, 92.0f, 58.0f}});
        heavy.push_back({2, 21, true, true, false, 0, true, false, 110.0f, {}});

        const auto wide = LayoutPickerCustomFlow(1000.0f, 220.0f, heavy);
        bool ok_bounds = true, any_overlap = false;
        const float max_right = 1000.0f - 16.0f - (wide.has_scrollbar ? 14.0f : 0.0f) + 0.5f;
        for (size_t i = 0; i < wide.boxes.size(); ++i) {
            if (wide.boxes[i].visible && wide.boxes[i].right > max_right) ok_bounds = false;
            for (size_t j = i + 1; j < wide.boxes.size(); ++j)
                any_overlap |= BoxesOverlap(wide.boxes[i], wide.boxes[j]);
        }
        check(ok_bounds && !any_overlap && wide.boxes.back().right >= 1000.0f - 31.0f,
              "heavy multi-parameter dialog wraps cleanly, aligns prominent action right, zero overlap");
        check(wide.boxes[13].segmented_radio &&
              wide.radio_boxes[13].size() == 3 &&
              wide.radio_boxes[13][0].top == wide.radio_boxes[13][2].top &&
              std::abs(wide.radio_boxes[13][0].right - wide.radio_boxes[13][1].left) < 0.1f &&
              std::abs(wide.radio_boxes[13][1].right - wide.radio_boxes[13][2].left) < 0.1f &&
              (wide.boxes[13].right - wide.boxes[13].left) < 235.0f,
              "multi-option radio group packs into a compact contiguous segmented control");

        std::vector<PickerCustomFlowItem> overflowing_radios;
        overflowing_radios.push_back({6, 30, true, false, false, 0, false, false, 0.0f,
            {150.0f, 160.0f, 170.0f, 160.0f}, {135.0f, 145.0f, 155.0f, 145.0f}});
        const auto narrow_rad = LayoutPickerCustomFlow(360.0f, 220.0f, overflowing_radios);
        check(!narrow_rad.boxes[0].segmented_radio &&
              narrow_rad.radio_boxes[0][3].top > narrow_rad.radio_boxes[0][0].top,
              "wide multi-option radio list falls back cleanly to wrapped radio buttons on narrow viewport");
    }
    return failures ? 1 : 0;
}
