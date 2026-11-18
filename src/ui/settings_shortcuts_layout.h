// settings_shortcuts_layout.h — Settings > 快捷键 (page 7): texts, geometry
// and hit testing. Included by ui_renderer_internal.h inside its anonymous
// namespace after settings_sysctx_layout.h (whose measuring helpers it
// reuses), so no includes here. Drawing: ui_settings_shortcuts.cpp; clicks:
// HitTestResult::SettingsShortcuts, handled in app/shortcut_page.cpp; data:
// app::ShortcutSettings.
//
// Layout follows the approved mockup: the
// tools sit on the title row, then the conflict strip, the filter bar and the
// group cards in up to three masonry columns. Popovers hang under the row
// they edit; the undo toast stays at the bottom of the viewport.

namespace shortcuts_text {
inline const wchar_t* Title() { return l10n::Get(l10n::StringId::KeyboardShortcuts).c_str(); }
inline const wchar_t* Lead() { return l10n::Get(l10n::StringId::ClickAnyKeyToChangeItItWorksAtOnceShortcutHelpAndButtonTipsS).c_str(); }
inline const wchar_t* SearchHint() { return l10n::Get(l10n::StringId::SearchActionsOrKeys).c_str(); }
inline const wchar_t* Find() { return l10n::Get(l10n::StringId::FindByKey).c_str(); }
inline const wchar_t* FindWaiting() { return l10n::Get(l10n::StringId::PressTheKeysToLookUp).c_str(); }
inline const wchar_t* Tester() { return l10n::Get(l10n::StringId::KeyTester).c_str(); }
inline const wchar_t* Filter(int f) {
    return f == 1 ? l10n::Get(l10n::StringId::Problems).c_str() : f == 2 ? l10n::Get(l10n::StringId::Changed_2).c_str()
                  : l10n::Get(l10n::StringId::FilterAll).c_str();
}
inline std::wstring FilterChip(const ShortcutsView& v, int f) {
    return std::wstring(Filter(f)) + L"  " + std::to_wstring(v.counts[f]);
}
inline const wchar_t* ResetAll() { return l10n::Get(l10n::StringId::ResetAll).c_str(); }
inline const wchar_t* Recheck() { return l10n::Get(l10n::StringId::CheckAgain).c_str(); }
inline std::wstring StripTitle(int n) {
    return std::to_wstring(n) + l10n::Pick(L" 个按键可能不起作用", n == 1 ? L" key may not work" : L" keys may not work");
}
inline const wchar_t* Probing() { return l10n::Get(l10n::StringId::CheckingWhichKeysOtherProgramsHold).c_str(); }
inline const wchar_t* AllGood() { return l10n::Get(l10n::StringId::EveryKeyWorks).c_str(); }
inline const wchar_t* Empty() { return l10n::Get(l10n::StringId::NoMatchingActions).c_str(); }
inline const wchar_t* Unassigned() { return l10n::Get(l10n::StringId::None).c_str(); }
inline const wchar_t* GlobalTag() { return l10n::Get(l10n::StringId::SystemWide).c_str(); }
inline const wchar_t* GlobalOff() { return l10n::Get(l10n::StringId::IntegrationOff).c_str(); }
inline const wchar_t* Cancel() { return l10n::Get(l10n::StringId::Cancel).c_str(); }
inline const wchar_t* Remove() { return l10n::Get(l10n::StringId::RemoveKey).c_str(); }
inline const wchar_t* Reset() { return l10n::Get(l10n::StringId::Reset).c_str(); }
inline const wchar_t* Ignore() { return l10n::Get(l10n::StringId::TurnedOffStopWarning).c_str(); }
inline const wchar_t* Record() { return l10n::Get(l10n::StringId::RecordMyOwn).c_str(); }
inline const wchar_t* Suggest() { return l10n::Get(l10n::StringId::TryInstead).c_str(); }
inline const wchar_t* PressKeys() { return l10n::Get(l10n::StringId::PressKeys).c_str(); }
inline const wchar_t* Undo() { return l10n::Get(l10n::StringId::SnackUndo).c_str(); }
} // namespace shortcuts_text

inline constexpr float kScRowDip = 38.0f;
inline constexpr float kScHeadDip = 40.0f;
inline constexpr float kScCardMinDip = 380.0f;
inline constexpr float kScCapDip = 22.0f;
inline constexpr float kScGapDip = 12.0f;

// Width of one keycap / one chord drawn as keycaps.
inline float ShortcutCapWidth(const fluent::Painter* painter, std::wstring_view text, float s) {
    const float w = painter ? painter->MeasureBadgeWidth(text) - 4.0f * s : static_cast<float>(text.size()) * 8.0f * s + 10.0f * s;
    return (std::max)(kScCapDip * s, w);
}
inline float ShortcutCapsWidth(const fluent::Painter* painter, const std::vector<std::wstring>& caps, float s) {
    float w = 0.0f;
    for (const auto& cap : caps) w += ShortcutCapWidth(painter, cap, s);
    if (caps.size() > 1) w += 3.0f * s * static_cast<float>(caps.size() - 1);
    return w;
}
inline std::vector<std::wstring> ShortcutRangeCaps(const ShortcutRowView& row) {
    std::vector<std::wstring> caps = row.bindings.empty() ? std::vector<std::wstring>{} : row.bindings.front().caps;
    caps.push_back(row.range_label);
    return caps;
}

inline void OffsetShortcutRect(D2D1_RECT_F& r, float dy) {
    if (r.right <= r.left) return;
    r.top += dy;
    r.bottom += dy;
}

// Lays the popover out from `top`; returns its bottom.
inline float LayoutShortcutPop(ShortcutsLayout& o, const ShortcutPopView& p, float left, float top, float width,
                               const fluent::Painter* painter, float s) {
    const float pad = 14.0f * s;
    const float x0 = left + pad;
    const float x1 = left + width - pad;
    o.pop_steps.clear(); o.pop_suggest.clear(); o.pop_digits.clear(); o.pop_more.clear();
    o.pop_box = o.pop_verdict = o.pop_detail = o.pop_save = o.pop_cancel = o.pop_remove = o.pop_reset = {};
    o.pop_ignore = o.pop_record = o.pop_suggest_label = o.pop_sub = {};
    for (auto& r : o.pop_mods) r = {};
    float y = top + pad;
    o.pop_title = D2D1::RectF(x0, y, x1 - 30.0f * s, y + 20.0f * s);
    o.pop_close = D2D1::RectF(x1 - 24.0f * s, y - 2.0f * s, x1, y + 22.0f * s);
    y += 24.0f * s;
    if (!p.subtitle.empty()) {
        const float h = SysCtxWrappedHeight(painter, p.subtitle, x1 - x0, s);
        o.pop_sub = D2D1::RectF(x0, y, x1, y + h);
        y += h + 6.0f * s;
    }
    if (p.kind == ShortcutPopView::More) {
        for (size_t i = 0; i < p.more_items.size(); ++i) {
            o.pop_more.push_back(D2D1::RectF(left + 6.0f * s, y, left + width - 6.0f * s, y + 34.0f * s));
            y += 34.0f * s;
        }
        return y + 8.0f * s;
    }
    if (p.kind == ShortcutPopView::Record || p.kind == ShortcutPopView::Tester) {
        o.pop_box = D2D1::RectF(x0, y + 4.0f * s, x1, y + 60.0f * s);
        y += 70.0f * s;
    }
    if (p.kind == ShortcutPopView::Range) {
        float mx = x0;
        for (int i = 0; i < 3; ++i) {
            o.pop_mods[i] = D2D1::RectF(mx, y + 4.0f * s, mx + 70.0f * s, y + 36.0f * s);
            mx += 76.0f * s;
        }
        y += 46.0f * s;
        const size_t n = p.digit_labels.size();
        if (n) {
            const float gap = 4.0f * s;
            const float cell = (std::min)(34.0f * s, (x1 - x0 - gap * static_cast<float>(n - 1)) / static_cast<float>(n));
            for (size_t i = 0; i < n; ++i) {
                const float cx = x0 + static_cast<float>(i) * (cell + gap);
                o.pop_digits.push_back(D2D1::RectF(cx, y, cx + cell, y + 26.0f * s));
            }
            y += 36.0f * s;
        }
    }
    if (!p.verdict.empty()) {
        const float text_w = x1 - x0 - 34.0f * s;
        const float detail_h = p.detail.empty() ? 0.0f : SysCtxWrappedHeight(painter, p.detail, text_w, s) + 2.0f * s;
        o.pop_verdict = D2D1::RectF(x0, y, x1, y + 10.0f * s + 20.0f * s + detail_h + 10.0f * s);
        if (detail_h > 0.0f)
            o.pop_detail = D2D1::RectF(x0 + 30.0f * s, y + 32.0f * s, x1 - 4.0f * s, y + 32.0f * s + detail_h);
        y = o.pop_verdict.bottom + 8.0f * s;
    }
    for (const auto& step : p.steps) {
        const float h = SysCtxWrappedHeight(painter, step, x1 - x0 - 24.0f * s, s);
        o.pop_steps.push_back(D2D1::RectF(x0, y, x1, y + h));
        y += h + 6.0f * s;
    }
    if (!p.steps.empty()) y += 2.0f * s;
    if (!p.suggestions.empty()) {
        o.pop_suggest_label = D2D1::RectF(x0, y, x1, y + 18.0f * s);
        y += 22.0f * s;
        float cx = x0;
        for (const auto& text : p.suggestions) {
            const float w = SysCtxTextWidth(painter, text, s) + 12.0f * s;
            if (cx > x0 && cx + w > x1) { cx = x0; y += 34.0f * s; }
            o.pop_suggest.push_back(D2D1::RectF(cx, y, cx + w, y + 28.0f * s));
            cx += w + 6.0f * s;
        }
        y += 38.0f * s;
    }
    // Secondary actions: a row of text buttons above the main ones.
    float lx = x0 - 8.0f * s;
    bool links = false;
    auto link = [&](D2D1_RECT_F& r, bool on, std::wstring_view text) {
        if (!on) return;
        const float w = painter ? painter->MeasureButtonWidth(text) : 96.0f * s;
        if (lx > x0 && lx + w > x1) { lx = x0 - 8.0f * s; y += 34.0f * s; }
        r = D2D1::RectF(lx, y, lx + w, y + 30.0f * s);
        lx += w + 2.0f * s;
        links = true;
    };
    link(o.pop_ignore, p.can_ignore, shortcuts_text::Ignore());
    link(o.pop_record, p.can_record, shortcuts_text::Record());
    link(o.pop_reset, p.can_reset, shortcuts_text::Reset());
    link(o.pop_remove, p.can_remove, shortcuts_text::Remove());
    if (links) y += 36.0f * s;
    if (!p.save_label.empty()) {
        const float save_w = painter ? painter->MeasureButtonWidth(p.save_label) : 96.0f * s;
        const float cancel_w = painter ? painter->MeasureButtonWidth(shortcuts_text::Cancel()) : 72.0f * s;
        o.pop_save = D2D1::RectF(x1 - save_w, y + 2.0f * s, x1, y + 34.0f * s);
        o.pop_cancel = D2D1::RectF(o.pop_save.left - 8.0f * s - cancel_w, y + 2.0f * s, o.pop_save.left - 8.0f * s, y + 34.0f * s);
        y += 40.0f * s;
    }
    return y + pad - 6.0f * s;
}

inline float LayoutSettingsShortcuts(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                                     const fluent::Painter* painter) {
    ShortcutsLayout& o = l.shortcuts;
    o = {};
    const ShortcutsView* v = vm.settings_shortcuts.get();
    const float pad = 20.0f * s;
    const float left = l.content.left + pad;
    const float right = l.content.right - pad;
    // Tools on the title row, right-aligned: search, 按键查找, 按键测试, ⋯.
    {
        const float top = l.content_origin + pad + 2.0f * s;
        float x = right;
        o.more = D2D1::RectF(x - 34.0f * s, top, x, top + 32.0f * s);
        x = o.more.left - 6.0f * s;
        const float tester_w = painter ? painter->MeasureButtonWidth(shortcuts_text::Tester(), L"\xE765") : 110.0f * s;
        o.tester = D2D1::RectF(x - tester_w, top, x, top + 32.0f * s);
        x = o.tester.left - 6.0f * s;
        const float find_w = painter ? painter->MeasureButtonWidth(shortcuts_text::Find(), L"\xE7C8") : 110.0f * s;
        o.find = D2D1::RectF(x - find_w, top, x, top + 32.0f * s);
        x = o.find.left - 8.0f * s;
        // The title needs ~120 DIP; the search box gives way first.
        const float search_w = (std::min)(260.0f * s, x - (left + 130.0f * s));
        if (search_w >= 140.0f * s) {
            o.search = D2D1::RectF(x - search_w, top, x, top + 32.0f * s);
            if (v && (!v->query.empty() || !v->find_caps.empty() || v->finding))
                o.search_clear = D2D1::RectF(o.search.right - 30.0f * s, top + 4.0f * s, o.search.right - 4.0f * s, top + 28.0f * s);
        } else {
            // Narrow: the search box takes its own row under the title.
            o.search = D2D1::RectF(left, y, right, y + 32.0f * s);
            if (v && (!v->query.empty() || !v->find_caps.empty() || v->finding))
                o.search_clear = D2D1::RectF(right - 30.0f * s, y + 4.0f * s, right - 4.0f * s, y + 28.0f * s);
            y += 40.0f * s;
        }
    }
    const float lead_h = SysCtxWrappedHeight(painter, shortcuts_text::Lead(), right - left, s);
    o.lead = D2D1::RectF(left, y - 4.0f * s, right, y - 4.0f * s + lead_h);
    y = o.lead.bottom + 12.0f * s;
    if (!v) return y + 40.0f * s;

    // Conflict strip: count, chips, 重新检测.
    {
        const float top = y;
        const float recheck_w = SysCtxHeaderButtonWidth(painter, shortcuts_text::Recheck(), s);
        o.recheck = D2D1::RectF(right - 10.0f * s - recheck_w, 0, right - 10.0f * s, 0);
        const int issues = v->red + v->amber;
        const std::wstring title = issues ? shortcuts_text::StripTitle(issues)
                                          : std::wstring(v->probing ? shortcuts_text::Probing() : shortcuts_text::AllGood());
        const float title_w = SysCtxTextWidth(painter, title, s) + 8.0f * s;
        o.strip_text = D2D1::RectF(left + 36.0f * s, top, left + 36.0f * s + title_w, top + 44.0f * s);
        float cx = o.strip_text.right + 6.0f * s;
        float cy = top + 9.0f * s;
        const float limit = o.recheck.left - 8.0f * s;
        for (const auto& chip : v->chips) {
            const float w = SysCtxTextWidth(painter, chip.text, s) + 14.0f * s;
            if (cx + w > limit && cx > o.strip_text.right + 6.0f * s) { cx = left + 36.0f * s; cy += 32.0f * s; }
            o.chips.push_back(D2D1::RectF(cx, cy, (std::min)(cx + w, limit), cy + 26.0f * s));
            cx += w + 6.0f * s;
        }
        const float bottom = (std::max)(top + 44.0f * s, cy + 26.0f * s + 9.0f * s);
        o.strip = D2D1::RectF(left, top, right, bottom);
        const float mid = (top + (std::min)(bottom, top + 44.0f * s)) * 0.5f;
        o.recheck.top = mid - kSysCtxHeaderButtonDip * s * 0.5f;
        o.recheck.bottom = o.recheck.top + kSysCtxHeaderButtonDip * s;
        y = bottom + 12.0f * s;
    }
    // Filter bar: 全部 / 有问题 / 已修改, then 全部恢复默认 on the right.
    {
        float cx = left;
        for (int i = 0; i < 3; ++i) {
            const float w = SysCtxTextWidth(painter, shortcuts_text::FilterChip(*v, i), s) + 16.0f * s;
            o.seg[i] = D2D1::RectF(cx, y, cx + w, y + 30.0f * s);
            cx += w + 2.0f * s;
        }
        const float reset_w = painter ? painter->MeasureButtonWidth(shortcuts_text::ResetAll(), L"\xE777") : 120.0f * s;
        o.reset_all = D2D1::RectF(right - reset_w, y - 1.0f * s, right, y + 31.0f * s);
        y += 30.0f * s + 14.0f * s;
    }
    // Cards: up to three masonry columns, each card into the shortest one.
    const float gap = kScGapDip * s;
    const float avail = right - left;
    const int columns = (std::max)(1, (std::min)(3, static_cast<int>((avail + gap) / (kScCardMinDip * s + gap))));
    const float col_w = (avail - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    float col_y[3] = {y, y, y};
    if (v->groups.empty()) {
        o.empty = D2D1::RectF(left, y, right, y + 80.0f * s);
        y += 96.0f * s;
    }
    for (size_t g = 0; g < v->groups.size(); ++g) {
        const auto& group = v->groups[g];
        int col = 0;
        for (int c = 1; c < columns; ++c) if (col_y[c] < col_y[col] - 1.0f) col = c;
        const float cl = left + static_cast<float>(col) * (col_w + gap);
        const float cr = cl + col_w;
        ShortcutsCardLayout card;
        card.group = static_cast<int>(g);
        float cy = col_y[col];
        card.head = D2D1::RectF(cl, cy, cr, cy + kScHeadDip * s);
        cy += kScHeadDip * s;
        for (size_t r = 0; r < group.rows.size(); ++r) {
            const auto& row = group.rows[r];
            ShortcutsRowLayout rl;
            rl.action = row.action;
            rl.group = static_cast<int>(g);
            rl.row = static_cast<int>(r);
            rl.rect = D2D1::RectF(cl + 6.0f * s, cy, cr - 6.0f * s, cy + kScRowDip * s);
            const float kt = cy + (kScRowDip - kScCapDip) * 0.5f * s - 3.0f * s;
            const float kb = kt + kScCapDip * s + 6.0f * s;
            float kx = rl.rect.right - 8.0f * s;
            // The + slot is kept on every row so the keys of a card line up.
            if (row.can_add) rl.add = D2D1::RectF(kx - 24.0f * s, kt + 1.0f * s, kx, kb - 1.0f * s);
            kx -= 28.0f * s;
            if (row.range) {
                const float w = ShortcutCapsWidth(painter, ShortcutRangeCaps(row), s) + 8.0f * s;
                rl.keys.push_back(D2D1::RectF(kx - w, kt, kx, kb));
                kx -= w;
            } else {
                std::vector<D2D1_RECT_F> keys;
                for (size_t b = row.bindings.size(); b-- > 0;) {
                    const float w = ShortcutCapsWidth(painter, row.bindings[b].caps, s) + 8.0f * s;
                    keys.push_back(D2D1::RectF(kx - w, kt, kx, kb));
                    kx -= w + (b ? 12.0f * s : 0.0f);  // room for the "/" between keys
                }
                rl.keys.assign(keys.rbegin(), keys.rend());
                if (row.bindings.empty()) {
                    const float w = SysCtxTextWidth(painter, shortcuts_text::Unassigned(), s);
                    kx -= w;
                }
            }
            if (row.fixed) kx -= 20.0f * s;  // the lock
            rl.name_right = kx - 8.0f * s;
            card.rows.push_back(std::move(rl));
            cy += kScRowDip * s;
        }
        cy += 6.0f * s;
        card.rect = D2D1::RectF(cl, card.head.top, cr, cy);
        col_y[col] = cy + gap;
        o.cards.push_back(std::move(card));
    }
    for (int c = 0; c < columns; ++c) y = (std::max)(y, col_y[c]);

    // Popover: under its row (or above it when the viewport ends first).
    const auto& p = v->pop;
    if (p.kind != ShortcutPopView::None) {
        D2D1_RECT_F anchor{};
        if (p.kind == ShortcutPopView::Tester) anchor = o.tester;
        else if (p.kind == ShortcutPopView::More) anchor = o.more;
        else
            for (const auto& card : o.cards)
                for (const auto& row : card.rows)
                    if (row.action == p.action) {
                        anchor = row.rect;
                        if (p.slot >= 0 && p.slot < static_cast<int>(row.keys.size())) {
                            anchor.left = row.keys[static_cast<size_t>(p.slot)].left;
                            anchor.right = (std::max)(row.keys[static_cast<size_t>(p.slot)].right, anchor.left + 1.0f);
                        }
                    }
        if (anchor.right > anchor.left) {
            const float width = (std::min)((p.kind == ShortcutPopView::More ? 240.0f : 360.0f) * s, right - left);
            // Right edges line up with the key (or row) being edited.
            const float pl = (std::max)(left, (std::min)(anchor.right, right) - width);
            float top = anchor.bottom + 4.0f * s;
            float bottom = LayoutShortcutPop(o, p, pl, top, width, painter, s);
            if (bottom > l.content.bottom - 8.0f * s && anchor.top - (bottom - top) - 4.0f * s >= l.content.top + 8.0f * s) {
                const float h = bottom - top;
                top = anchor.top - 4.0f * s - h;
                bottom = LayoutShortcutPop(o, p, pl, top, width, painter, s);
            }
            o.pop = D2D1::RectF(pl, top, pl + width, bottom);
            o.pop_anchor = anchor;
            y = (std::max)(y, bottom + 16.0f * s);
        }
    }
    // Toast: fixed at the bottom of the viewport.
    if (!v->toast.empty()) {
        const float text_w = SysCtxTextWidth(painter, v->toast, s) + 8.0f * s;
        const float undo_w = v->toast_undo ? (painter ? painter->MeasureButtonWidth(shortcuts_text::Undo()) : 64.0f * s) : 0.0f;
        const float w = (std::min)(right - left, text_w + undo_w + 36.0f * s);
        const float cx = (l.content.left + l.content.right) * 0.5f;
        o.toast = D2D1::RectF(cx - w * 0.5f, l.content.bottom - 60.0f * s, cx + w * 0.5f, l.content.bottom - 16.0f * s);
        o.toast_text = D2D1::RectF(o.toast.left + 16.0f * s, o.toast.top, o.toast.right - undo_w - 14.0f * s, o.toast.bottom);
        if (v->toast_undo)
            o.toast_undo = D2D1::RectF(o.toast.right - 8.0f * s - undo_w, o.toast.top + 6.0f * s, o.toast.right - 8.0f * s,
                                       o.toast.bottom - 6.0f * s);
    }
    return y + 8.0f * s;
}

inline int HitTestSettingsShortcuts(const SettingsLayout& l, const WindowViewModel& vm, float x, float y) {
    const ShortcutsLayout& o = l.shortcuts;
    const ShortcutsView* v = vm.settings_shortcuts.get();
    auto in = [&](const D2D1_RECT_F& r) { return r.right > r.left && SysCtxContains(r, x, y); };
    if (!v) return -1;
    if (in(o.toast_undo)) return kScToastUndo;
    if (v->pop.kind != ShortcutPopView::None && o.pop.right > o.pop.left) {
        if (!in(o.pop)) return kScBackdrop;
        if (in(o.pop_close) || in(o.pop_cancel)) return kScPopClose;
        if (in(o.pop_save) && v->pop.can_save) return kScPopSave;
        if (in(o.pop_remove)) return kScPopRemove;
        if (in(o.pop_reset)) return kScPopReset;
        if (in(o.pop_ignore)) return kScPopIgnore;
        if (in(o.pop_record)) return kScPopRecord;
        for (int i = 0; i < 3; ++i) if (in(o.pop_mods[i])) return kScPopModBase + i;
        for (size_t i = 0; i < o.pop_suggest.size(); ++i) if (in(o.pop_suggest[i])) return kScPopSuggestBase + static_cast<int>(i);
        for (size_t i = 0; i < o.pop_more.size(); ++i) if (in(o.pop_more[i])) return kScMoreBase + static_cast<int>(i);
        return kScPopBody;  // inside the popover, not on a control: swallow the click
    }
    if (in(o.search_clear)) return kScSearchClear;
    if (in(o.search)) return kScSearch;
    if (in(o.find)) return kScFind;
    if (in(o.tester)) return kScTester;
    if (in(o.more)) return kScMore;
    if (in(o.recheck)) return kScRecheck;
    for (size_t i = 0; i < o.chips.size(); ++i) if (in(o.chips[i])) return kScChipBase + static_cast<int>(i);
    for (int i = 0; i < 3; ++i) if (in(o.seg[i])) return kScFilterBase + i;
    if (in(o.reset_all) && v->any_modified) return kScResetAll;
    for (const auto& card : o.cards) {
        if (!in(card.rect)) continue;
        for (const auto& row : card.rows) {
            if (!in(row.rect)) continue;
            const auto& rv = v->groups[static_cast<size_t>(row.group)].rows[static_cast<size_t>(row.row)];
            if (rv.fixed) return -1;
            if (in(row.add)) return kScKeyBase + row.action * 4 + 3;
            for (size_t k = 0; k < row.keys.size(); ++k)
                if (in(row.keys[k])) return kScKeyBase + row.action * 4 + static_cast<int>(k);
            return kScRowBase + row.action;
        }
    }
    return -1;
}
