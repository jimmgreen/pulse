// settings_layout_store.h - geometry of the 扩展商店 browse page and of the generic
// extension detail page. Included by settings_layout_sections.h
// after pack_text / store_text; shared by drawing (ui_settings_store.cpp) and hit tests.
#pragma once

namespace store_view {
inline bool Has(D2D1_RECT_F r) { return r.right>r.left && r.bottom>r.top; }
inline bool HasSettings(const ExtensionStoreView& view, const ExtensionStoreItem& item) {
    (void)view;
    return item.settings_package>=0;
}
// Chip of a catalog category: 1 archives, 2 preview, 3 search, 4 AI, 0 none of these.
inline int CategoryChip(std::wstring_view category) {
    if (category == L"archive" || category == L"compress" || category == L"archives" || category == L"fileops") return 1;
    if (category == L"preview" || category == L"image" || category == L"media" || category == L"images") return 2;
    if (category == L"search" || category == L"find" || category == L"ocr") return 3;
    if (category == L"ai") return 4;
    return 0;
}
inline bool Contains(std::wstring_view text, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    const auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; };
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(text[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}
inline bool Shown(const ExtensionStoreView& view, int chip, std::wstring_view name, std::wstring_view summary) {
    if (view.category != 0 && view.category != chip) return false;
    return Contains(name, view.search) || Contains(summary, view.search);
}
inline const std::wstring& Chip(int index) {
    using I = l10n::StringId;
    static constexpr I ids[] = {I::StoreCatAll, I::StoreCatArchive, I::StoreCatPreview, I::StoreCatSearch, I::StoreCatAi};
    return l10n::Get(ids[index < 0 || index > 4 ? 0 : index]);
}
inline const std::wstring& Tab(int index) {
    using I = l10n::StringId;
    return l10n::Get(index == 0 ? I::StoreTabDiscover : index == 1 ? I::StoreTabInstalled : I::StoreManage);
}

// An official pack as a store card, from the same model and actions as 管理增强包.
struct PackCard {
    std::wstring name, summary, meta, state, button, secondary;
    PackAction action = PackAction::Install, secondary_action = PackAction::StoreManage;
    bool action_enabled = false, installed = false;
    float progress = -1.0f; // 0..1 while downloading
    int chip = 2;
    uint32_t color = 0x527D70;
    StoreIcon icon = StoreIcon::None;
};
inline PackCard Pack(const WindowViewModel& vm, int pack) {
    using I = l10n::StringId;
    PackCard c;
    const auto installed_text = [](const std::wstring& version) {
        std::wstring text = l10n::Get(I::Installed);
        return version.empty() ? text : text + L" " + version;
    };
    // Images, RAW and FFmpeg share one shape: download, update, or done (管理 opens the packs tab).
    const auto file_pack = [&](bool available, bool installed, bool installing, float progress, const std::wstring& version,
                               const std::wstring& update, PackAction install, PackAction update_action) {
        c.installed = installed;
        if (installing) {
            c.state = pack_text::Downloading(progress); c.progress = progress;
            c.button = l10n::Get(I::Cancel); c.action = install; c.action_enabled = true;
        } else if (installed && !update.empty()) {
            c.state = installed_text(version);
            c.button = l10n::Get(I::StoreUpdateItem); c.action = update_action; c.action_enabled = true;
        } else if (installed) {
            c.state = installed_text(version);
            c.secondary = l10n::Get(I::StorePackManage); c.secondary_action = PackAction::StoreManage;
        } else if (available) {
            c.button = l10n::Get(I::StorePackInstall); c.action = install; c.action_enabled = true;
        } else c.state = l10n::Get(I::NotPublished);
    };
    switch (pack) {
    case 0:
        c.name = pack_text::ImagesTitle(); c.summary = pack_text::ImagesDesc(); c.meta = pack_text::ImagesMeta();
        c.color = 0x7A64C4; c.icon = StoreIcon::Image;
        file_pack(vm.settings_pack_images_available, vm.settings_pack_images_installed, vm.settings_pack_images_installing,
                  vm.settings_pack_images_progress, vm.settings_pack_images_version, vm.settings_pack_images_update_version,
                  PackAction::ImagesInstall, PackAction::ImagesUpdate);
        break;
    case 1:
        c.name = pack_text::RawTitle(); c.summary = pack_text::RawDesc(); c.meta = pack_text::RawMeta();
        c.color = 0xB2733A; c.icon = StoreIcon::Camera;
        file_pack(vm.settings_pack_raw_available, vm.settings_pack_raw_installed, vm.settings_pack_raw_installing,
                  vm.settings_pack_raw_progress, vm.settings_pack_raw_version, vm.settings_pack_raw_update_version,
                  PackAction::RawInstall, PackAction::RawUpdate);
        break;
    case 2:
        c.name = pack_text::MediaTitle(); c.summary = pack_text::MediaDesc(); c.meta = pack_text::MediaMeta();
        c.color = 0x3A7E9B; c.icon = StoreIcon::Film;
        if (pack_text::MediaLocalInUse(vm) && !vm.settings_pack_media_installed) {
            c.state = l10n::Get(I::StoreLocalFfmpeg);
            c.secondary = l10n::Get(I::StorePackManage);
        } else file_pack(vm.settings_pack_media_available, vm.settings_pack_media_installed, vm.settings_pack_installing,
                         vm.settings_pack_progress, vm.settings_pack_version, vm.settings_pack_update_version,
                         PackAction::Install, PackAction::Update);
        break;
    case 3:
        c.name = l10n::Get(I::ContentPackTitle); c.summary = l10n::Get(I::ContentPackDesc); c.meta = l10n::Get(I::ContentPackMeta);
        c.color = 0x4E6FA8; c.icon = StoreIcon::TextSearch; c.chip = 3;
        c.installed = vm.settings_content_pack_state == 0 || vm.settings_content_pack_state == 1;
        if (c.installed) {
            c.state = pack_text::ContentBadge(vm).text;
            c.secondary = l10n::Get(I::StorePackManage); c.secondary_action = PackAction::ContentManage;
        } else if (vm.settings_content_pack_state == 3 || vm.settings_content_pack_state == 4) {
            c.state = pack_text::ContentBadge(vm).text;
            c.button = l10n::Get(I::StorePackRepair); c.action = PackAction::ContentInstall; c.action_enabled = true;
        } else if (vm.settings_content_pack_state == 2) {
            c.button = l10n::Get(I::StorePackInstall); c.action = PackAction::ContentInstall; c.action_enabled = true;
        } else c.state = l10n::Get(I::NotPublished);
        break;
    default:
        c.name = pack_text::AiTitle(); c.summary = pack_text::AiDesc(); c.meta = pack_text::AiMeta();
        c.color = 0x8D63C2; c.icon = StoreIcon::Sparkle; c.chip = 4;
        c.installed = pack_text::AiUsable(vm);
        if (vm.settings_ai_pack_installing) {
            c.state = pack_text::Downloading(vm.settings_ai_pack_progress); c.progress = vm.settings_ai_pack_progress;
            c.button = l10n::Get(I::Cancel); c.action = PackAction::AiCancel; c.action_enabled = true;
        } else if (c.installed) {
            c.state = pack_text::AiBadge(vm).text;
            c.secondary = l10n::Get(I::StorePackManage); c.secondary_action = PackAction::AiManage;
        } else if (pack_text::AiAvailable(vm)) {
            c.state = vm.settings_ai_pack_state == 3 || vm.settings_ai_pack_state == 4 ? pack_text::AiBadge(vm).text : std::wstring{};
            c.button = l10n::Get(c.state.empty() ? I::StorePackInstall : I::StorePackRepair);
            c.action = PackAction::AiInstall; c.action_enabled = true;
        } else c.state = l10n::Get(I::NotPublished);
        break;
    }
    return c;
}
inline constexpr int kPackCount = 5;

inline std::wstring Fill(std::wstring text, std::wstring_view key, std::wstring_view value) {
    const auto at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}
inline std::wstring InstalledVersion(const ExtensionStoreView& view,const ExtensionStoreItem& item) {
    for(const auto& installed:view.installed)
        if(installed.id==std::wstring(item.id.begin(),item.id.end()))return installed.version;
    return {};
}
inline std::wstring AboutValue(const ExtensionStoreView& view,const ExtensionStoreItem& item,int row) {
    using I=l10n::StringId;
    if(row==0)return item.official?l10n::Get(I::StoreOfficial):item.publisher;
    if(row==2)return item.engine;
    if(row==3)return item.license;
    if(row==4) {
        if(!item.permissions.empty()) {
            std::wstring joined;
            for(size_t i=0;i<item.permissions.size();++i) {
                if(i)joined+=L" · ";
                joined+=item.permissions[i];
            }
            return joined;
        }
        return l10n::Get(I::StorePermissionsNone);
    }
    if(row==5)return item.source;
    std::wstring version=InstalledVersion(view,item);
    if(version.empty()) {
        for(const auto& installed:view.installed)
            if(installed.id==std::wstring(item.id.begin(),item.id.end()))return l10n::Get(I::OpUnknown);
        version=item.version;
    }
    for(const auto& release:item.releases)if(release.version==version) {
        if(!release.released.empty())version+=L" · "+release.released;
        if(!release.min_pulse.empty())version+=L" · "+Fill(l10n::Get(I::SzRequiresPulse),L"{version}",release.min_pulse);
        return version;
    }
    if(version==item.version&&!item.released.empty())version+=L" · "+item.released;
    return version;
}
inline size_t HistoryCount(const ExtensionStoreItem& item) {return item.releases.empty()?item.history.size():item.releases.size();}
inline std::wstring HistoryVersion(const ExtensionStoreItem& item,size_t index) {
    if(!item.releases.empty())return item.releases[index].version;
    const auto& text=item.history[index];const auto end=text.find(L" · ");
    return text.substr(0,end);
}
inline std::wstring HistoryNote(const ExtensionStoreItem& item,size_t index) {
    if(!item.releases.empty()) {
        const auto& release=item.releases[index];
        return release.released+(release.released.empty()||release.notes.empty()?L"":L" · ")+release.notes;
    }
    const auto& text=item.history[index];const auto start=text.find(L" · ");
    return start==std::wstring::npos?std::wstring{}:text.substr(start+3);
}
// The context menu preview is the host right-click menu around the plugin's rows
// (StorePainter::Menu draws it with the same steps): quick actions, Open, the
// plugin group (caption + rows, or its submenu item with the rows on a rail and
// Settings), Properties. A detail line makes a row 40 DIP instead of 28.
inline float MenuPreviewGroupHeight(const std::vector<SevenZipMenuRow>& rows, bool submenu) {
    float h = submenu ? 30.0f : 26.0f;
    for (const auto& row : rows) h += (row.detail.empty() ? 28.0f : 40.0f) + 2.0f;
    return h + (submenu ? 9.0f + 28.0f : 0.0f) + 4.0f;
}
inline float MenuPreviewHeight(const std::vector<SevenZipMenuRow>& rows, bool submenu, float s) {
    return (8 + 32 + 28 + 9 + MenuPreviewGroupHeight(rows, submenu) + 9 + 28 + 6) * s;
}
inline int GridColumns(float width,float scale) {
    return (std::clamp)(static_cast<int>((width+12*scale)/(272*scale)),1,3);
}
inline void LayoutSearch(SettingsLayout& l,float s,float font,float& y) {
    const float left=l.content.left+20*s,right=l.content.right-20*s;
    const float sw=(std::min)(right-left,260*s);
    const float h=34*s*font;
    if(Has(l.page_heading) && right-left>=sw+280*s*font+24*s) {
        const float top=(l.page_heading.top+l.page_heading.bottom-h)/2;
        l.store_search=D2D1::RectF(right-sw,top,right,top+h);
        l.page_heading.right=l.store_search.left-24*s;
    } else {
        l.store_search=D2D1::RectF(left,y,right,y+h);
        y=l.store_search.bottom+14*s;
    }
}

inline void LayoutStatusBanner(D2D1_RECT_F& surface, D2D1_RECT_F& text, D2D1_RECT_F* restart,
    std::wstring_view message, float left, float right, float s, float font, float& y,
    const fluent::Painter* painter) {
    if (message.empty() || right - left < 68*s) return;
    const float text_left = left + 36*s; // space for the standard InfoBar glyph
    const float text_right = right - 12*s;
    const float available = text_right - text_left;
    const float action_height = 32*s*font;
    const auto& label = l10n::Get(l10n::StringId::StoreRestartNow);
    const float action_width = restart ? (std::min)(available, painter ? painter->MeasureButtonWidth(label) : 144*s*font) : 0.0f;
    const bool inline_action = restart && available - action_width - 12*s >= 180*s;
    const float end = inline_action ? text_right - action_width - 12*s : text_right;
    const float message_height = painter ? painter->MeasureWrappedCaptionHeight(message, end - text_left) : 40*s*font;
    const float row_height = (std::max)(16*s, inline_action ? (std::max)(message_height, action_height) : message_height);
    const float top = y + 8*s;
    const float text_top = top + (row_height - message_height) * 0.5f;
    text = D2D1::RectF(text_left, text_top, end, text_top + message_height);
    float bottom = top + row_height + 8*s;
    if (restart) {
        const float button_top = inline_action ? top + (row_height - action_height) * 0.5f : bottom;
        const float button_left = inline_action ? text_right - action_width : text_left;
        *restart = D2D1::RectF(button_left, button_top, button_left + action_width, button_top + action_height);
        bottom = (std::max)(bottom, restart->bottom + 8*s);
    }
    surface = D2D1::RectF(left, y, right, bottom);
    y = bottom + 12*s;
}

inline void LayoutStatusBanners(SettingsLayout& l, const ExtensionStoreView& view, bool browse, bool installed,
    bool package_needs_restart, float left, float right, float s, float font, float& y, const fluent::Painter* painter) {
    LayoutStatusBanner(l.store_notice_surface, l.store_notice, nullptr, store_text::Notice(view, browse, installed).text,
        left, right, s, font, y, painter);
    if (view.pending_restart || package_needs_restart) {
        LayoutStatusBanner(l.store_restart_surface, l.store_restart, &l.store_restart_button,
            store_text::RestartNotice(view).text, left, right, s, font, y, painter);
    }
}

inline int ActiveDetailItem(const ExtensionStoreView& view) {
    if (!view.detail) return -1;
    const int index = view.detail_item;
    return index >= 0 && index < static_cast<int>(view.items.size()) ? index : -1;
}
} // namespace store_view

// Browse: the store home.
float LayoutSettingsStore(SettingsLayout& l, const WindowViewModel& vm, float s,
                          float y, const fluent::Painter* painter) {
    using I = l10n::StringId;
    const auto& view = vm.settings_store_view;
    const float left = l.content.left + 20*s, right = l.content.right - 20*s;
    const float font = static_cast<float>(vm.settings_ui_font_scale)/100;
    auto height = [&](std::wstring_view value, float w) {
        return value.empty() ? 0.0f : painter ? painter->MeasureWrappedCaptionHeight(value, (std::max)(1.0f, w)) : 40*s*font;
    };
    auto caption = [&](std::wstring_view value, float inset = 0.0f) {
        const auto r = D2D1::RectF(left+inset, y, right-inset, y+height(value, right-left-2*inset));
        y = r.bottom+12*s; return r;
    };
    auto width = [&](std::wstring_view value) {
        return (std::min)(right-left, painter ? painter->MeasureButtonWidth(value) : 144*s*font);
    };
    const float bh = 32*s*font;
    store_view::LayoutSearch(l,s,font,y);
    const bool loading = view.state == ExtensionStoreView::State::Loading;
    const bool failed = view.state == ExtensionStoreView::State::Failed;
    const bool listed = view.state == ExtensionStoreView::State::Online || view.state == ExtensionStoreView::State::Offline;
    store_view::LayoutStatusBanners(l, view, true, false, false, left, right, s, font, y, painter);
    if (failed) {
        // The catalog failure has its own retry/recovery actions, separate from installation notices.
        const float rw = width(l10n::Get(I::RecoveryRetry)), sw2 = width(l10n::Get(I::StoreDownloadSources));
        const float fw = width(l10n::Get(I::StoreInstallFromFile));
        const float max_bw = (std::max)(rw, (std::max)(sw2, fw));
        const bool stacked = right-left < 600*s;
        const float text_r = stacked ? right-16*s : right - 16*s - max_bw - 20*s;
        const float top = y;
        float cy = top + 18*s;
        const float tx = left + 16*s + 44*s + 16*s;
        l.store_state = D2D1::RectF(tx, cy, text_r, cy + 22*s*font); cy = l.store_state.bottom + 4*s;
        const float desc_h = height(l10n::Get(I::StoreFailedDesc), text_r - tx);
        cy += desc_h + 6*s;
        l.store_failed = D2D1::RectF(left, top, right, (std::max)(cy + 14*s, top + 18*s + 3*bh + 16*s + 18*s));
        const float bx = right - 16*s - max_bw;
        l.store_refresh = D2D1::RectF(bx, top + 18*s, right - 16*s, top + 18*s + bh);
        l.store_failed_sources = D2D1::RectF(bx, l.store_refresh.bottom + 8*s, right - 16*s, l.store_refresh.bottom + 8*s + bh);
        l.store_install_file = D2D1::RectF(bx, l.store_failed_sources.bottom + 8*s, right - 16*s, l.store_failed_sources.bottom + 8*s + bh);
        if (stacked) {
            l.store_refresh = D2D1::RectF(left+16*s, cy+8*s, (std::min)(right-16*s, left+16*s+rw), cy+8*s+bh);
            l.store_failed_sources = D2D1::RectF(left+16*s, l.store_refresh.bottom+8*s, (std::min)(right-16*s, left+16*s+sw2), l.store_refresh.bottom+8*s+bh);
            l.store_install_file = D2D1::RectF(left+16*s, l.store_failed_sources.bottom+8*s, (std::min)(right-16*s, left+16*s+fw), l.store_failed_sources.bottom+8*s+bh);
            l.store_failed.bottom = l.store_install_file.bottom+16*s;
        }
        y = l.store_failed.bottom + 24*s;
    }
    l.store_items.assign(view.items.size(), {});
    auto layout_hero_header = [&](SettingsLayout::StoreCard& c, const ExtensionStoreItem& item,
                                  bool has_settings, float& out_body) {
        c.feature = true;
        const float pad = 22*s, tile = 72*s;
        const float top = y;
        c.icon = D2D1::RectF(left + pad, top + pad, left + pad + tile, top + pad + tile);
        const auto& all = l10n::Get(I::StoreAllSettings);
        const float aw = (std::min)(right - left - 2*pad, width(all) + 14*s);
        float actions_left = right - pad;
        if (has_settings) {
            c.settings = D2D1::RectF(right - pad - aw, top + pad, right - pad, top + pad + bh);
            actions_left = c.settings.left - 8*s;
        }
        if (!item.button_label.empty() && (item.action_enabled || !has_settings)) {
            const float bw = (std::min)(right - left - 2*pad, width(item.button_label));
            c.button = D2D1::RectF(actions_left - bw, top + pad, actions_left, top + pad + bh);
            actions_left = c.button.left - 8*s;
        }
        const float tx = c.icon.right + 18*s;
        const float text_r = actions_left - 12*s > tx + 160*s ? actions_left - 12*s : right - pad;
        const float name_h = painter ? painter->MeasureWrappedSectionHeight(item.name, (std::max)(1.0f, text_r - tx)) : 28*s*font;
        c.name = D2D1::RectF(tx, top + pad - 2*s, text_r, top + pad - 2*s + (std::max)(26*s*font, name_h));
        float publisher_top = c.name.bottom + 3*s;
        if (item.enabled) {
            const float bw = (std::min)(text_r - tx, (painter ? painter->MeasureBadgeWidth(l10n::Get(I::StoreExtensionEnabled)) : 100*s*font) + 12*s);
            const float nw = painter ? (painter->MeasureButtonWidth(item.name) - 16*s) * 1.5f + 8*s : 160*s*font;
            if (tx + nw + 8*s + bw <= text_r) {
                c.name.right = tx + nw;
                const float badge_h = 22*s*font;
                const float badge_y = (c.name.top + c.name.bottom - badge_h) / 2;
                c.enabled_badge = D2D1::RectF(c.name.right + 8*s, badge_y, c.name.right + 8*s + bw, badge_y + badge_h);
            } else {
                c.enabled_badge = D2D1::RectF(tx, publisher_top, tx + bw, publisher_top + 22*s*font);
                publisher_top = c.enabled_badge.bottom + 4*s;
            }
        }
        c.publisher = D2D1::RectF(tx, publisher_top, text_r, publisher_top + 20*s*font);
        c.summary = D2D1::RectF(tx, c.publisher.bottom + 6*s, text_r, c.publisher.bottom + 6*s + height(item.summary, text_r - tx));
        float body = (std::max)(c.icon.bottom, c.summary.bottom) + 18*s;
        if (text_r > actions_left - 12*s && (store_view::Has(c.settings) || store_view::Has(c.button))) {
            if (store_view::Has(c.settings)) {
                c.settings = D2D1::RectF(left + pad, body, left + pad + aw, body + bh);
                if (store_view::Has(c.button)) {
                    const float bw = c.button.right - c.button.left;
                    const bool wrap = c.settings.right + 8*s + bw > right - pad;
                    const float bx = wrap ? left + pad : c.settings.right + 8*s;
                    const float by = wrap ? c.settings.bottom + 8*s : body;
                    c.button = D2D1::RectF(bx, by, bx + bw, by + bh);
                }
                body = (std::max)(c.settings.bottom, c.button.bottom) + 18*s;
            } else if (store_view::Has(c.button)) {
                const float bw = c.button.right - c.button.left;
                c.button = D2D1::RectF(left + pad, body, left + pad + bw, body + bh);
                body = c.button.bottom + 18*s;
            }
        }
        out_body = body;
        c.bounds = D2D1::RectF(left, top, right, body);
    };
    l.store_generic_quick.clear();
    for (size_t qidx = 0; qidx < view.quick_cards.size(); ++qidx) {
        const auto& qc = view.quick_cards[qidx];
        if (qc.item_index < 0 || static_cast<size_t>(qc.item_index) >= view.items.size()) continue;
        const auto& item = view.items[static_cast<size_t>(qc.item_index)];
        if (!store_view::Shown(view, store_view::CategoryChip(item.category), item.name, item.summary)) continue;

        SettingsLayout::GenericQuickCard gq;
        gq.item_index = qc.item_index;
        gq.quick_index = static_cast<int>(qidx);
        const float pad = 22*s;
        const float top = y;
        float body = y;
        layout_hero_header(gq.card, item, store_view::HasSettings(view, item), body);

        gq.section = D2D1::RectF(left, body, right, body);
        const bool split = qc.has_menu_preview && (right - left >= 680*s);
        // The preview stage sits in the card (16 DIP from its edges), 20 DIP after the settings column.
        const float preview_w = split ? (std::clamp)((right - left) * 0.36f, 300*s, 360*s) : 0.0f;
        const float ql = left + pad, qr = split ? right - 16*s - preview_w - 20*s : right - pad;
        float qy = body + 16*s;
        gq.title = D2D1::RectF(ql, qy, qr, qy + 22*s*font);
        const auto& status_text = qc.status.empty() ? l10n::Get(view.saved ? I::StoreSaved : I::StoreAutoSaved) : qc.status;
        const float saved_w = (std::min)(qr - ql, (painter ? painter->MeasureBadgeWidth(status_text) : 138*s*font) + 22*s);
        gq.saved = D2D1::RectF((std::max)(ql, qr - saved_w), qy, qr, qy + 22*s*font);
        const bool stacked_quick = qr - ql < 340*s*font;
        if (stacked_quick) {
            gq.saved = D2D1::RectF(ql, gq.title.bottom + 4*s, (std::min)(qr, ql + saved_w), gq.title.bottom + 4*s + 22*s*font);
        } else {
            gq.title.right = gq.saved.left - 8*s;
        }
        qy = gq.saved.bottom + 6*s;

        // Settings rows: label and description on the left, the controls right-aligned and vertically
        // centred on the same line. Only a row whose label would get under 150 DIP puts its controls
        // underneath (one row at a time, not the whole card).
        const size_t row_count = qc.available ? qc.rows.size() : (std::max)(size_t{1}, qc.rows.size());
        const float ch = 32*s*font;
        const auto control_width = [&](const ExtensionQuickControlView& ctrl) {
            if (ctrl.kind == 1) return (std::min)(qr - ql, (ctrl.width_dip ? static_cast<float>(ctrl.width_dip) : 140.0f) * s);
            if (ctrl.kind == 2) return (std::min)(qr - ql, ctrl.width_dip ? static_cast<float>(ctrl.width_dip) * s : width(ctrl.label));
            return 40*s;
        };
        gq.rows.reserve(row_count);
        for (size_t ri = 0; ri < row_count; ++ri) {
            const ExtensionQuickRowView* row_view_ptr = qc.available && ri < qc.rows.size() ? &qc.rows[ri] : nullptr;
            float controls_w = 0.0f;
            if (row_view_ptr) {
                for (const auto& ctrl : row_view_ptr->controls) controls_w += control_width(ctrl) + 8*s;
            }
            const bool stacked_row = controls_w > 0.0f && (qr - ql) - controls_w - 8*s < 150*s*font;
            const float row_h = stacked_row ? 56*s*font + ch + 8*s : 56*s*font;
            const D2D1_RECT_F row_rect = D2D1::RectF(ql, qy, qr, qy + row_h);
            gq.rows.push_back(row_rect);
            qy += row_h;
            if (!row_view_ptr) continue;
            const auto& row_view = *row_view_ptr;
            float right_edge = qr;
            const float cy = stacked_row ? row_rect.bottom - ch - 8*s : (row_rect.top + row_rect.bottom - ch) / 2;
            for (size_t ci_rev = row_view.controls.size(); ci_rev > 0; --ci_rev) {
                const auto& ctrl = row_view.controls[ci_rev - 1];
                const float cw = control_width(ctrl);
                const float cl = (std::max)(ql, right_edge - cw);
                D2D1_RECT_F cr = D2D1::RectF(cl, cy, right_edge, cy + ch);
                if (ctrl.kind == 0) {
                    cr.top += (ch - 20*s) / 2;
                    cr.bottom = cr.top + 20*s;
                }
                gq.controls.push_back({cr, ctrl.action, static_cast<int>(ri), ctrl.kind, ctrl.disabled});
                right_edge = cl - 8*s;
            }
        }

        float qbottom = qy + 12*s;
        if (qc.available && qc.has_menu_preview) {
            const float menu_h = (std::max)(84*s, store_view::MenuPreviewHeight(qc.preview_rows, qc.preview_submenu, s));
            if (split) {
                gq.preview = D2D1::RectF(right - 16*s - preview_w, body + 14*s, right - 16*s, body + 14*s);
            } else {
                gq.preview = D2D1::RectF(left + pad, qbottom, right - pad, qbottom);
            }
            gq.preview_caption = D2D1::RectF(gq.preview.left + 14*s, gq.preview.top + 12*s,
                                             gq.preview.right - 14*s, gq.preview.top + 12*s + 20*s*font);
            const float menu_w = (std::min)(gq.preview.right - gq.preview.left - 28*s, 340*s);
            gq.preview_menu = D2D1::RectF(gq.preview.left + 14*s, gq.preview_caption.bottom + 10*s,
                                          gq.preview.left + 14*s + menu_w, gq.preview_caption.bottom + 10*s + menu_h);
            gq.preview.bottom = gq.preview_menu.bottom + 18*s;
            qbottom = split ? (std::max)(qbottom, gq.preview.bottom + 16*s) : gq.preview.bottom + 16*s;
        }
        gq.section.bottom = qbottom;
        body = qbottom;
        gq.card.bounds = D2D1::RectF(left, top, right, body);
        y = gq.card.bounds.bottom + 18*s;
        l.store_generic_quick.push_back(std::move(gq));
    }
    // Chips, then the catalog state and 刷新 at the end of the same row.
    {
        float cx = left;
        const float chip_h = 30*s*font;
        for (int i = 0; i < 5; ++i) {
            const float cw = (painter ? painter->MeasureBadgeWidth(store_view::Chip(i)) : 64*s) + (i == 0 ? 20*s : 34*s);
            if (cx > left && cx + cw > right) { cx = left; y += chip_h + 8*s; }
            l.store_chips[i] = D2D1::RectF(cx, y, cx + cw, y + chip_h);
            cx += cw + 6*s;
        }
        if (!failed) {
            const float rw = width(l10n::Get(I::Refresh));
            const float fw = width(l10n::Get(I::StoreInstallFromFile));
            float row_top = y;
            if (cx + 12*s + fw + 8*s + rw + 90*s > right) { row_top = y + chip_h + 8*s; }
            l.store_refresh = D2D1::RectF(right - rw, row_top + (chip_h - bh)/2, right, row_top + (chip_h - bh)/2 + bh);
            if (l.store_refresh.left - 8*s - fw >= left) {
                l.store_install_file = D2D1::RectF(l.store_refresh.left - 8*s - fw, l.store_refresh.top, l.store_refresh.left - 8*s, l.store_refresh.bottom);
            } else {
                l.store_install_file = D2D1::RectF((std::max)(left, right - fw), l.store_refresh.bottom + 8*s, right, l.store_refresh.bottom + 8*s + bh);
            }
            float buttons_left = (l.store_install_file.top == l.store_refresh.top ? l.store_install_file.left : l.store_refresh.left);
            float buttons_bottom = (std::max)(l.store_refresh.bottom, l.store_install_file.bottom);
            if (view.busy) {
                const float cw = width(l10n::Get(I::Cancel));
                if (l.store_install_file.top == l.store_refresh.top && buttons_left - 8*s - cw >= left) {
                    l.store_cancel = D2D1::RectF(buttons_left - 8*s - cw, l.store_refresh.top, buttons_left - 8*s, l.store_refresh.bottom);
                    buttons_left = l.store_cancel.left;
                } else if (l.store_install_file.top > l.store_refresh.top && l.store_install_file.left - 8*s - cw >= left) {
                    l.store_cancel = D2D1::RectF(l.store_install_file.left - 8*s - cw, l.store_install_file.top, l.store_install_file.left - 8*s, l.store_install_file.bottom);
                } else {
                    l.store_cancel = D2D1::RectF((std::max)(left, right - cw), buttons_bottom + 8*s, right, buttons_bottom + 8*s + bh);
                }
                buttons_bottom = (std::max)(buttons_bottom, l.store_cancel.bottom);
            }
            const float state_right = buttons_left - 12*s;
            const float state_left = row_top>y ? left : cx;
            if (state_right-state_left < 90*s) {
                l.store_state = D2D1::RectF(left,buttons_bottom+8*s,right,buttons_bottom+8*s+chip_h);
            } else l.store_state = D2D1::RectF(state_left,row_top,state_right,row_top+chip_h);
            y = (std::max)(y + chip_h, (std::max)(buttons_bottom, l.store_state.bottom)) + 16*s;
        } else y += chip_h + 16*s;
    }
    if (loading && view.items.empty()) {
        // Skeleton cards stand in for the catalog while it loads.
        const int columns = right-left >= 660*s*font ? 3 : 1;
        const float gap = 12*s, w = (right-left-gap*(columns-1))/columns;
        l.store_skeleton[0] = D2D1::RectF(left, y, right, y + 150*s);
        y = l.store_skeleton[0].bottom + 18*s;
        for (int i = 0; i < 3; ++i) {
            const int col = columns == 1 ? 0 : i, row = columns == 1 ? i : 0;
            l.store_skeleton[1+i] = D2D1::RectF(left + col*(w+gap), y + row*(140*s+gap), left + col*(w+gap) + w, y + row*(140*s+gap) + 140*s);
        }
        y = l.store_skeleton[3].bottom + 24*s;
    }
    // Card grid: catalog items of a section, then (official) the packs.
    const int columns = store_view::GridColumns(right-left,s);
    const float gap = 12*s, w = (right-left-gap*(columns-1))/columns;
    float row_bottom = y;
    size_t placed = 0;
    auto place = [&](float card_h) {
        if (placed && placed%columns==0) y = row_bottom + gap;
        else if (!placed) y = row_bottom;
        const float x = left + static_cast<float>(placed%columns)*(w+gap);
        ++placed;
        const auto r = D2D1::RectF(x, y, x + w, y + card_h);
        row_bottom = (std::max)(row_bottom, r.bottom);
        return r;
    };
    auto card = [&](SettingsLayout::StoreCard& c, std::wstring_view name, std::wstring_view publisher, std::wstring_view summary,
                    std::wstring_view status, std::wstring_view button, bool settings) {
        // Measure first (fixed two-line summary), then place in the grid.
        const float pad = 16*s, tile = 44*s;
        const float tw = w - 2*pad;
        const float name_h = painter ? painter->MeasureWrappedSectionHeight(name, (std::max)(1.0f, tw - tile - 12*s)) : 24*s;
        const float summary_limit=painter ? painter->MeasureWrappedCaptionHeight(L"Ag\nAg",tw) : 40*s*font;
        const float sum_h = (std::min)(height(summary, tw), summary_limit);
        const float status_h = status.empty() ? 0.0f : height(status, tw) + 8*s;
        const float head = (std::max)(tile, name_h + 2*s + 20*s*font);
        const bool stacked_actions = settings && !button.empty() && width(button)+width(l10n::Get(I::Settings))+8*s > tw;
        const float card_h = pad + head + 12*s + sum_h + 10*s + status_h + bh + pad + (stacked_actions ? bh+8*s : 0);
        const auto r = place(card_h);
        const float tl = r.left + pad, tr = r.right - pad;
        c.bounds = r;
        c.icon = D2D1::RectF(tl, r.top + pad, tl + tile, r.top + pad + tile);
        c.name = D2D1::RectF(c.icon.right + 12*s, r.top + pad, tr, r.top + pad + name_h);
        c.publisher = D2D1::RectF(c.name.left, c.name.bottom + 2*s, tr, c.name.bottom + 2*s + 20*s*font);
        (void)publisher;
        float cy = r.top + pad + head + 12*s;
        c.summary = D2D1::RectF(tl, cy, tr, cy + sum_h); cy = c.summary.bottom + 10*s;
        if (!status.empty()) { c.status = D2D1::RectF(tl, cy, tr, cy + status_h - 8*s); cy = c.status.bottom + 8*s; }
        c.version = D2D1::RectF(tl, cy, tr, cy + bh);
        float actions = tr;
        if (!button.empty()) {
            const float bw = (std::min)(tw, width(button));
            c.button = D2D1::RectF(actions - bw, cy, actions, cy + bh);
            actions = c.button.left - 8*s;
        }
        if (settings) {
            const float sw = (std::min)(tw, width(l10n::Get(I::Settings)));
            if (stacked_actions) { cy+=bh+8*s; actions=tr; }
            c.settings = D2D1::RectF(actions - sw, cy, actions, cy + bh);
            actions = c.settings.left - 8*s;
        }
        c.version.right = (std::max)(c.version.left, actions - 4*s);
    };
    // Pulse 官方: catalog items, then the packs.
    bool official_any = false;
    const auto section = [&](bool official) {
        y = row_bottom;
        if (official) {
            l.store_section = D2D1::RectF(left, y, right, y + 24*s*font);
            if(right-left<600*s*font) {
                l.store_official_hint=D2D1::RectF(left,l.store_section.bottom+4*s,right,l.store_section.bottom+4*s+24*s*font);
                y=l.store_official_hint.top;
            } else {
                l.store_section.right=left+200*s*font;
                l.store_official_hint=D2D1::RectF(l.store_section.right+12*s,y,right,y+24*s*font);
            }
        } else l.store_community_section = D2D1::RectF(left, y, right, y + 24*s*font);
        y += 24*s*font + 10*s;
        row_bottom = y; placed = 0;
    };
    for (size_t i = 0; i < view.items.size(); ++i) {
        const auto& item = view.items[i];
        if (!item.official) continue;
        if (!store_view::Shown(view, store_view::CategoryChip(item.category), item.name, item.summary)) continue;
        if (!official_any) { section(true); official_any = true; }
        card(l.store_items[i], item.name, item.publisher, item.summary, item.status, item.button_label, store_view::HasSettings(view,item));
    }
    l.store_packs.clear();
    for (int p = 0; p < store_view::kPackCount; ++p) {
        const auto info = store_view::Pack(vm, p);
        if (!store_view::Shown(view, info.chip, info.name, info.summary)) continue;
        if (!official_any) { section(true); official_any = true; }
        SettingsLayout::StoreCard c;
        card(c, info.name, L"", info.summary, info.progress >= 0 ? std::wstring{} : std::wstring{}, info.button, false);
        SettingsLayout::StorePack pack;
        pack.pack = p; pack.bounds = c.bounds; pack.icon = c.icon; pack.name = c.name; pack.publisher = c.publisher;
        pack.summary = c.summary; pack.meta = c.version; pack.button = c.button;
        if (!info.secondary.empty()) {
            const float sw = width(info.secondary);
            const float right_edge = store_view::Has(pack.button) ? pack.button.left - 8*s : c.version.right + 4*s + (store_view::Has(c.button) ? 0.0f : 0.0f);
            const float edge = store_view::Has(pack.button) ? right_edge : c.bounds.right - 16*s;
            pack.secondary = D2D1::RectF(edge - sw, c.version.top, edge, c.version.bottom);
            pack.meta.right = (std::max)(pack.meta.left, pack.secondary.left - 8*s);
        }
        if (!info.state.empty()) { pack.state = pack.meta; pack.meta = {}; }
        if (info.progress >= 0 && store_view::Has(pack.state)) {
            pack.progress = D2D1::RectF(pack.state.left,pack.state.bottom-3*s,pack.state.right,pack.state.bottom);
            pack.state.bottom = pack.progress.top-4*s;
        }
        l.store_packs.push_back(pack);
    }
    if (official_any) row_bottom += 28*s;
    // 社区增强包: its cards, or the empty card.
    bool community_any = false;
    for (size_t i = 0; i < view.items.size(); ++i) {
        const auto& item = view.items[i];
        if (item.official) continue;
        if (!store_view::Shown(view, store_view::CategoryChip(item.category), item.name, item.summary)) continue;
        if (!community_any) { section(false); community_any = true; }
        card(l.store_items[i], item.name, item.publisher, item.summary, item.status, item.button_label, store_view::HasSettings(view,item));
    }
    if (community_any) row_bottom += 28*s;
    else if (listed) {
        section(false);
        const bool filtered = view.category != 0 || !view.search.empty();
        const auto& title = l10n::Get(view.items.empty() ? I::StoreEmpty : filtered ? I::StoreNoMatch : I::StoreCommunityEmpty);
        const float tx = left + 20*s + 44*s + 16*s;
        const float desc_h = height(l10n::Get(I::StoreCommunityEmptyDesc), right - 20*s - tx);
        l.store_community_card = D2D1::RectF(left, y, right, y + (std::max)(84*s, 24*s + 22*s*font + 4*s + desc_h + 24*s));
        l.store_empty = D2D1::RectF(tx, l.store_community_card.top + 24*s, right - 20*s, l.store_community_card.top + 24*s + 22*s*font);
        (void)title;
        row_bottom = l.store_community_card.bottom + 28*s;
    }
    // Footnotes: the trust note behind a shield, then the official-pack link.
    y = row_bottom;
    l.store_trust = caption(l10n::Get(I::StoreTrust), 30*s);
    y -= 4*s;
    l.store_manage = caption(l10n::Get(I::StoreOfficialManage), 12*s);
    return y + 12*s;
}

std::wstring ExtensionDetailSubtitle(bool official, const std::wstring& summary) {
    std::wstring text = official ? l10n::Get(l10n::StringId::StoreOfficial) : std::wstring{};
    if (!summary.empty()) text += (text.empty() ? L"" : L" · ") + summary;
    return text;
}
// One template owns the geometry of every package detail header.
float LayoutExtensionDetailHeader(SettingsLayout::DetailHeader& d, bool enabled, bool official,
                                 const std::wstring& version, const std::wstring& summary,
                                 float left, float right, float y, float s, float font,
                                 const fluent::Painter* painter,
                                 bool installed = true, std::wstring_view action_label = {},
                                 std::wstring_view status_text = {}) {
    using I = l10n::StringId;
    d.on = true;
    d.install_button = {};
    d.enable = {};
    d.enable_label = {};
    d.more = {};
    const float bh = 32*s*font;
    auto height = [&](std::wstring_view value, float w) {
        return painter ? painter->MeasureWrappedCaptionHeight(value, (std::max)(1.0f, w)) : 40*s*font;
    };
    auto width = [&](std::wstring_view value) {
        return (std::min)(right-left, painter ? painter->MeasureButtonWidth(value) : 144*s*font);
    };
    // Crumb: ‹ 扩展商店 › name.
    const float crumb_w = width(l10n::Get(I::StoreTitle)) + 8*s;
    d.crumb = D2D1::RectF(left - 6*s, y, left + crumb_w, y + 28*s*font);
    d.crumb_name = D2D1::RectF(d.crumb.right + 4*s, y, right, y + 28*s*font);
    y = d.crumb.bottom + 16*s;
    // Header: the tile, name, the publisher line and badges; optional action button, switch and ⋯ at the right.
    d.icon = D2D1::RectF(left, y, left + 72*s, y + 72*s);
    const float more_w = 36*s;
    const float label_w = 64*s*font;
    const float action_w = action_label.empty() ? 0.0f : width(action_label);
    float controls_left = right;
    if (installed) {
        d.more = D2D1::RectF(right - more_w, y + 18*s, right, y + 18*s + bh);
        d.enable = D2D1::RectF(d.more.left - 12*s - 40*s, y + 18*s + (bh - 20*s)/2, d.more.left - 12*s, y + 18*s + (bh + 20*s)/2);
        d.enable_label = D2D1::RectF(d.enable.left - 10*s - label_w, y + 18*s, d.enable.left - 10*s, y + 18*s + bh);
        controls_left = d.enable_label.left;
        if (!action_label.empty()) {
            d.install_button = D2D1::RectF(controls_left - 12*s - action_w, y + 18*s, controls_left - 12*s, y + 18*s + bh);
            controls_left = d.install_button.left;
        }
    } else if (!action_label.empty()) {
        d.install_button = D2D1::RectF(right - action_w, y + 18*s, right, y + 18*s + bh);
        controls_left = d.install_button.left;
    }
    const float tx = d.icon.right + 18*s;
    const bool narrow_head = (controls_left < right) && (controls_left - tx < 260*s);
    const float tr = narrow_head || controls_left >= right ? right : controls_left - 16*s;
    d.name = D2D1::RectF(tx, y - 2*s, tr, y + 34*s*font);
    d.sub = D2D1::RectF(tx, d.name.bottom + 2*s, tr, d.name.bottom + 2*s + height(ExtensionDetailSubtitle(official, summary), tr - tx - (official ? 20*s : 0)));
    float bx = tx;
    float by = d.sub.bottom + 8*s;
    const std::wstring badges[3] = {
        l10n::Get(!installed ? I::StoreDetailNotInstalled : enabled ? I::StoreExtensionEnabled : I::StoreExtensionDisabled),
        std::wstring{},
        version};
    for (int i = 0; i < 3; ++i) {
        if (badges[i].empty()) continue;
        const float bw = (std::min)(right-tx, (painter ? painter->MeasureBadgeWidth(badges[i]) : 120*s) + (i == 0 && installed ? 14*s : 0.0f));
        if (bx>tx && bx+bw>right) { bx=tx; by+=24*s*font+6*s; }
        d.badges[i] = D2D1::RectF(bx, by, bx + bw, by + 24*s*font);
        bx = d.badges[i].right + 6*s;
    }
    d.status_pill = {};
    if (!status_text.empty()) {
        const float sbw = (std::min)(right-tx, painter ? painter->MeasureBadgeWidth(status_text) + 12*s : 112*s);
        if (bx>tx && bx+sbw>right) { bx=tx; by+=24*s*font+6*s; }
        d.status_pill = D2D1::RectF(bx, by, bx + sbw, by + 24*s*font);
        bx = d.status_pill.right + 6*s;
    }
    y = (std::max)(d.icon.bottom, by+24*s*font) + 18*s;
    if (narrow_head) {
        float ax = left;
        if (!action_label.empty()) {
            d.install_button = D2D1::RectF(ax, y, ax + action_w, y + bh);
            ax = d.install_button.right + 12*s;
        }
        if (installed) {
            const float switch_group_w = label_w + 10*s + 40*s + 12*s + more_w;
            if (ax > left && ax + switch_group_w > right) {
                ax = left;
                y += bh + 8*s;
            }
            d.enable_label = D2D1::RectF(ax, y, ax + label_w, y + bh);
            d.enable = D2D1::RectF(d.enable_label.right + 10*s, y + (bh - 20*s)/2, d.enable_label.right + 50*s, y + (bh + 20*s)/2);
            d.more = D2D1::RectF(d.enable.right + 12*s, y, d.enable.right + 12*s + more_w, y + bh);
        }
        y += bh + 14*s;
    }
    return y;
}
inline float LayoutExtensionDetailTabsCustom(SettingsLayout::DetailHeader& d,
                                             const std::wstring* titles, const int* actions, int count,
                                             float left, float right, float y, float s, float font,
                                             const fluent::Painter* painter) {
    count = (std::clamp)(count, 0, SettingsLayout::DetailHeader::kMaxTabs);
    d.tab_count = count;
    float x = left;
    const float height = 38*s*font;
    for (int i = 0; i < count; ++i) {
        d.tab_titles[i] = titles[i];
        d.tab_actions[i] = actions[i];
        const float width = (std::min)(right-left, painter ? painter->MeasureButtonWidth(titles[i]) : 96*s);
        if (x > left && x + width > right) { x = left; y += height + 6*s; }
        d.tabs[i] = D2D1::RectF(x, y, x + width, y + height);
        x += width + 4*s;
    }
    for (int i = count; i < SettingsLayout::DetailHeader::kMaxTabs; ++i) {
        d.tabs[i] = {};
        d.tab_titles[i].clear();
        d.tab_actions[i] = -1;
    }
    return y + height + 20*s;
}
inline float LayoutExtensionDetailTabsSpec(SettingsLayout::DetailHeader& d,
                                           const l10n::StringId* labels, const int* actions, int count,
                                           float left, float right, float y, float s, float font,
                                           const fluent::Painter* painter) {
    count = (std::clamp)(count, 0, SettingsLayout::DetailHeader::kMaxTabs);
    std::wstring titles[SettingsLayout::DetailHeader::kMaxTabs];
    for (int i = 0; i < count; ++i) {
        d.tab_labels[i] = labels[i];
        titles[i] = l10n::Get(labels[i]);
    }
    return LayoutExtensionDetailTabsCustom(d, titles, actions, count, left, right, y, s, font, painter);
}
float LayoutExtensionDetailTabs(SettingsLayout::DetailHeader& d, int count, float left, float right,
                               float y, float s, float font, const fluent::Painter* painter) {
    using I = l10n::StringId;
    static constexpr I ids[] = {I::SzTabSettings, I::SzTabMenu, I::SzTabFormats, I::SzTabAbout};
    int actions[4]{};
    const int base = static_cast<int>(PackAction::StoreDetailTabBase);
    for (int i = 0; i < 4; ++i) actions[i] = base + i;
    return LayoutExtensionDetailTabsSpec(d, ids, actions, count, left, right, y, s, font, painter);
}

// Generic store detail / showcase page for any catalog or local extension.
float LayoutExtensionDetail(SettingsLayout& l, const WindowViewModel& vm, float s,
                            float y, const fluent::Painter* painter) {
    using I = l10n::StringId;
    const auto& view = vm.settings_store_view;
    const int item_index = store_view::ActiveDetailItem(view);
    if (item_index < 0 || item_index >= static_cast<int>(view.items.size())) return y;
    const auto& item = view.items[static_cast<size_t>(item_index)];
    auto& d = l.ext_detail;
    const float left = l.content.left + 20*s, right = l.content.right - 20*s;
    const float font = static_cast<float>(vm.settings_ui_font_scale)/100;
    auto height = [&](std::wstring_view value, float w) {
        return value.empty() ? 0.0f : painter ? painter->MeasureWrappedCaptionHeight(value, (std::max)(1.0f, w)) : 40*s*font;
    };
    const std::wstring_view action_label = (!item.button_label.empty() && (!item.installed || item.action_enabled))
        ? std::wstring_view{item.button_label} : std::wstring_view{};
    const std::wstring ver = item.version + (item.size.empty() ? std::wstring{} : L" · " + item.size);
    y = LayoutExtensionDetailHeader(d, item.enabled, item.official, ver, item.summary,
        left, right, y, s, font, painter, item.installed, action_label);
    store_view::LayoutStatusBanners(l, view, false, false, false, left, right, s, font, y, painter);

    l10n::StringId tab_labels[4]{};
    int tab_actions[4]{};
    int tab_count = 0;
    if (item.installed && item.settings_package >= 0) {
        tab_labels[tab_count] = I::SzTabSettings;
        tab_actions[tab_count++] = static_cast<int>(PackAction::StoreDetailOpenSettings);
        if (item.has_menu_tab) {
            tab_labels[tab_count] = I::SzTabMenu;
            tab_actions[tab_count++] = static_cast<int>(PackAction::StoreDetailOpenMenu);
        }
    }
    tab_labels[tab_count] = I::StoreDetailTabOverview;
    tab_actions[tab_count++] = static_cast<int>(PackAction::StoreDetailTabBase);
    tab_labels[tab_count] = I::SzTabAbout;
    tab_actions[tab_count++] = static_cast<int>(PackAction::StoreDetailTabBase) + 1;
    y = LayoutExtensionDetailTabsSpec(d, tab_labels, tab_actions, tab_count, left, right, y, s, font, painter);

    if (view.detail_tab == 1) {
        const float ml = left, mr = right;
        d.body = D2D1::RectF(ml, y, mr, y);
        // Details as a grid of key-over-value cells (3, 2 or 1 per row by width), then the release
        // history as a timeline: a rail with one dot per release, the version, the current badge, its note.
        const float g0 = y;
        const int cols = mr - ml >= 760*s*font ? 3 : mr - ml >= 460*s*font ? 2 : 1;
        const float il = ml + 20*s, ir = mr - 20*s, col_gap = 24*s;
        const float cw = (std::max)(1.0f, (ir - il - col_gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
        float cy = y + 18*s;
        for (int i = 0; i < 6; i += cols) {
            float row_h = 0.0f;
            for (int c = 0; c < cols && i + c < 6; ++c) {
                const int k = i + c;
                auto value = store_view::AboutValue(view, item, k);
                if (k == 5 && value.empty() && !item.homepage.empty()) value = item.homepage;
                const float x = il + static_cast<float>(c) * (cw + col_gap);
                const float vw = (std::max)(1.0f, cw - (k == 0 && item.official ? 19*s : 0.0f));
                const float vh = (std::max)(22*s*font, height(value, vw));
                d.about_keys[k] = {x, cy, x + cw, cy + 18*s*font};
                d.about_values[k] = {x, d.about_keys[k].bottom + 4*s, x + cw, d.about_keys[k].bottom + 4*s + vh};
                d.about_rows[k] = {x, cy, x + cw, d.about_values[k].bottom};
                row_h = (std::max)(row_h, d.about_rows[k].bottom - cy);
            }
            cy += row_h + 18*s;
        }
        if (item.source.starts_with(L"https://") || item.source.starts_with(L"http://"))
            d.about_source = d.about_values[5];
        else if (item.source.empty() && (item.homepage.starts_with(L"https://") || item.homepage.starts_with(L"http://")))
            d.about_homepage = d.about_values[5];
        y = cy;
        d.groups[0] = D2D1::RectF(ml, g0, mr, y);
        if (store_view::HistoryCount(item) > 0) {
            y += 24*s;
            d.history_label = D2D1::RectF(ml + 2*s, y, mr, y + 22*s*font);
            y = d.history_label.bottom + 8*s;
            const float g1 = y;
            y += 8*s;
            const auto installed_version = store_view::InstalledVersion(view, item);
            const float vx = ml + 48*s;
            for (size_t i = 0; i < store_view::HistoryCount(item) && i < 8; ++i) {
                const bool current = !installed_version.empty() && store_view::HistoryVersion(item, i) == installed_version;
                const auto note = store_view::HistoryNote(item, i);
                const float version_w = (std::min)(mr - vx - 20*s, 120*s*font);
                const D2D1_RECT_F version{vx, y + 10*s, vx + version_w, y + 10*s + 22*s*font};
                const float badge_w = current ? (painter ? painter->MeasureBadgeWidth(l10n::Get(I::SzAboutCurrent)) : 64*s) : 0.0f;
                const float nh = note.empty() ? 0.0f : (std::max)(20*s*font, height(note, (std::max)(1.0f, mr - 20*s - vx)));
                const D2D1_RECT_F note_rect{vx, version.bottom + 2*s, mr - 20*s, version.bottom + 2*s + nh};
                const float rh = note_rect.bottom - y + 10*s;
                d.history_rows.push_back({ml, y, mr, y + rh});
                d.history_versions.push_back(version);
                d.history_notes.push_back(note_rect);
                d.history_current.push_back(current && version.right + 8*s + badge_w <= mr - 20*s
                    ? D2D1::RectF(version.right + 8*s, version.top, version.right + 8*s + badge_w, version.bottom)
                    : D2D1::RectF());
                y += rh;
            }
            y += 8*s;
            d.history_card = {ml, g1, mr, y};
        }
        y += 24*s;
        d.body.bottom = y;
        return y;
    }

    const bool split = right - left >= 680*s;
    const float side_w = split ? (right - left >= 780*s ? 286*s : 264*s) : 0.0f;
    const float ml = left, mr = split ? right - side_w - 20*s : right;
    d.body = D2D1::RectF(ml, y, mr, y);

    // The introduction and what the plugin adds to the right-click menu share one card: the text on
    // the left, the menu (from its manifest quickSettings preview) on a tinted stage at the right; the
    // stage moves under the text when the column is narrow.
    d.showcase_quick = -1;
    for (size_t qi = 0; qi < view.quick_cards.size(); ++qi) {
        const auto& qc = view.quick_cards[qi];
        if (qc.item_index == item_index && qc.available && qc.has_menu_preview && !qc.preview_rows.empty()) {
            d.showcase_quick = static_cast<int>(qi);
            break;
        }
    }
    const bool has_menu = d.showcase_quick >= 0;
    const bool beside = has_menu && mr - ml >= 640*s;
    const float stage_w = beside ? (std::clamp)((mr - ml) * 0.44f, 300*s, 360*s) : mr - ml - 32*s;
    const float card_top = y;
    const float text_l = ml + 20*s, text_r = beside ? mr - 16*s - stage_w - 24*s : mr - 20*s;
    d.desc_label = D2D1::RectF(text_l, card_top + 18*s, text_r, card_top + 18*s + 22*s*font);
    const std::wstring_view desc_str = !item.description.empty() ? std::wstring_view{item.description}
        : !item.summary.empty() ? std::wstring_view{item.summary}
        : std::wstring_view{l10n::Get(I::StoreDetailNoDescription)};
    const float desc_w = (std::max)(1.0f, text_r - text_l);
    const float desc_h = painter ? painter->MeasureWrappedBodyHeight(desc_str, desc_w) : 60*s*font;
    d.desc_text = D2D1::RectF(text_l, d.desc_label.bottom + 8*s, text_r, d.desc_label.bottom + 8*s + (std::max)(22*s*font, desc_h));
    float card_bottom = d.desc_text.bottom + 20*s;
    d.showcase_label = {};
    if (has_menu) {
        const auto& qc = view.quick_cards[static_cast<size_t>(d.showcase_quick)];
        const float menu_h = (std::max)(84*s, store_view::MenuPreviewHeight(qc.preview_rows, qc.preview_submenu, s));
        const float stage_l = beside ? mr - 16*s - stage_w : ml + 16*s;
        const float stage_t = beside ? card_top + 16*s : d.desc_text.bottom + 16*s;
        const float menu_w = (std::min)(stage_w - 28*s, 340*s);
        d.showcase_caption = D2D1::RectF(stage_l + 14*s, stage_t + 12*s, stage_l + stage_w - 14*s, stage_t + 12*s + 20*s*font);
        const float mx = stage_l + (stage_w - menu_w) / 2;
        d.showcase_menu = D2D1::RectF(mx, d.showcase_caption.bottom + 10*s, mx + menu_w, d.showcase_caption.bottom + 10*s + menu_h);
        d.showcase_card = D2D1::RectF(stage_l, stage_t, stage_l + stage_w, d.showcase_menu.bottom + 16*s);
        card_bottom = (std::max)(card_bottom, d.showcase_card.bottom + 16*s);
    }
    d.desc_card = D2D1::RectF(ml, card_top, mr, card_bottom);
    y = d.desc_card.bottom + 20*s;

    // Highlights as tiles: a tinted check badge and the text, two per row when there is room.
    if (!item.highlights.empty()) {
        d.highlights_label = D2D1::RectF(ml + 2*s, y, mr, y + 22*s*font);
        y = d.highlights_label.bottom + 8*s;
        const float hcard_top = y;
        const int hcols = mr - ml >= 560*s ? 2 : 1;
        const float hgap = 12*s;
        const float tile_w = (mr - ml - hgap * static_cast<float>(hcols - 1)) / static_cast<float>(hcols);
        for (size_t i = 0; i < item.highlights.size(); i += static_cast<size_t>(hcols)) {
            float row_h = 0.0f;
            for (int c = 0; c < hcols && i + static_cast<size_t>(c) < item.highlights.size(); ++c) {
                const float th = (std::max)(20*s*font, height(item.highlights[i + static_cast<size_t>(c)], (std::max)(1.0f, tile_w - 72*s)));
                row_h = (std::max)(row_h, (std::max)(64*s, th + 36*s));
            }
            for (int c = 0; c < hcols && i + static_cast<size_t>(c) < item.highlights.size(); ++c) {
                const float x = ml + static_cast<float>(c) * (tile_w + hgap);
                d.highlight_rows.push_back(D2D1::RectF(x, y, x + tile_w, y + row_h));
            }
            y += row_h + hgap;
        }
        d.highlights_card = D2D1::RectF(ml, hcard_top, mr, y - hgap);
        y += 20*s - hgap;
    }

    if (!item.tags.empty()) {
        d.tags_label = D2D1::RectF(ml + 2*s, y, mr, y + 22*s*font);
        y = d.tags_label.bottom + 8*s;
        const float tcard_top = y;
        float tx = ml + 16*s, ty = y + 14*s;
        const float ch = 26*s*font;
        for (const auto& tag : item.tags) {
            const float cw = (std::min)(mr - ml - 32*s,
                painter ? painter->MeasureBadgeWidth(tag) + 8*s : (24.0f + 10.0f * static_cast<float>(tag.size())) * s * font);
            if (tx > ml + 16*s && tx + cw > mr - 16*s) { tx = ml + 16*s; ty += ch + 8*s; }
            d.tag_chips.push_back(D2D1::RectF(tx, ty, tx + cw, ty + ch));
            tx += cw + 8*s;
        }
        d.tags_card = D2D1::RectF(ml, tcard_top, mr, ty + ch + 14*s);
        y = d.tags_card.bottom + 20*s;
    }
    d.body.bottom = y;

    float sy = split ? d.body.top : y;
    const float sl = split ? right - side_w : left, sr = right;
    d.side = D2D1::RectF(sl, sy, sr, sy);
    {
        struct Fact { l10n::StringId key; std::wstring value; int link_kind; };
        std::vector<Fact> facts;
        const auto pub = store_view::AboutValue(view, item, 0);
        if (!pub.empty()) facts.push_back({I::Publisher, pub, 0});
        const auto ver_fact = store_view::AboutValue(view, item, 1);
        if (!ver_fact.empty()) facts.push_back({I::SzAboutVersion, ver_fact, 0});
        if (!item.engine.empty()) facts.push_back({I::SzAboutEngine, item.engine, 0});
        if (!item.license.empty()) facts.push_back({I::SzAboutLicense, item.license, 0});
        if (!item.homepage.empty()) facts.push_back({I::StoreDetailHomepage, item.homepage, 1});
        if (!item.source.empty() && item.source != item.homepage) facts.push_back({I::SzAboutSource, item.source, 2});

        const float fcard_top = sy;
        d.facts_title = D2D1::RectF(sl + 14*s, sy + 12*s, sr - 14*s, sy + 12*s + 22*s*font);
        float fy = d.facts_title.bottom + 10*s;
        const bool side_stacked = (sr - sl - 28*s) < 220*s*font;
        const float key_w = 72*s*font;
        for (const auto& f : facts) {
            const float vl = side_stacked ? sl + 14*s : sl + 14*s + key_w + 10*s;
            const float vy = side_stacked ? fy + 20*s*font : fy;
            const float vh = (std::max)(20*s*font, height(f.value, (std::max)(1.0f, sr - 14*s - vl)));
            d.fact_keys.push_back({sl + 14*s, fy, side_stacked ? sr - 14*s : vl - 10*s, fy + 20*s*font});
            const D2D1_RECT_F vr{vl, vy, sr - 14*s, vy + vh};
            d.fact_values.push_back(vr);
            d.fact_rows.push_back({sl + 14*s, fy, sr - 14*s, vr.bottom});
            if (f.link_kind == 1 && (f.value.starts_with(L"https://") || f.value.starts_with(L"http://")))
                d.fact_homepage = vr;
            if (f.link_kind == 2 && (f.value.starts_with(L"https://") || f.value.starts_with(L"http://")))
                d.fact_source = vr;
            fy = vr.bottom + 8*s;
        }
        d.facts_card = D2D1::RectF(sl, fcard_top, sr, fy + 6*s);
        sy = d.facts_card.bottom + 12*s;
    }
    {
        const float pcard_top = sy;
        d.perms_title = D2D1::RectF(sl + 14*s, sy + 12*s, sr - 14*s, sy + 12*s + 22*s*font);
        sy = d.perms_title.bottom + 10*s;
        const float perm_w = (std::max)(1.0f, sr - sl - 28*s - 18*s);
        if (item.permissions.empty()) {
            const float ph = (std::max)(20*s*font, height(l10n::Get(I::StorePermissionsNone), (std::max)(1.0f, sr - sl - 28*s)));
            d.perm_rows.push_back(D2D1::RectF(sl + 14*s, sy, sr - 14*s, sy + ph));
            sy += ph + 8*s;
        } else {
            for (const auto& perm : item.permissions) {
                const float ph = (std::max)(20*s*font, height(perm, perm_w));
                d.perm_rows.push_back(D2D1::RectF(sl + 14*s, sy, sr - 14*s, sy + ph));
                sy += ph + 8*s;
            }
        }
        d.perms_card = D2D1::RectF(sl, pcard_top, sr, sy + 6*s);
    }
    d.side.bottom = d.perms_card.bottom;
    return split ? (std::max)(y, d.side.bottom + 24*s) : d.side.bottom + 24*s;
}