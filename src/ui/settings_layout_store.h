// settings_layout_store.h - geometry of the 扩展商店 browse page and of the 7-Zip
// detail page (archive_store_mockup.html). Included by settings_layout_sections.h
// after pack_text / store_text; shared by drawing (ui_settings_store.cpp) and hit tests.
#pragma once

namespace store_view {
inline bool Has(D2D1_RECT_F r) { return r.right>r.left && r.bottom>r.top; }
inline bool HasSettings(const ExtensionStoreView& view, const ExtensionStoreItem& item) {
    const int seven_zip=view.seven_zip.item;
    return item.settings_package>=0 || (view.seven_zip.installed && seven_zip>=0 &&
        seven_zip<static_cast<int>(view.items.size()) && &item==&view.items[static_cast<size_t>(seven_zip)]);
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

// The 7-Zip texts the host draws (catalog.tsv), by setting value.
inline const std::wstring& ModeTitle(int mode) {
    using I = l10n::StringId;
    return l10n::Get(mode == 1 ? I::SzModeFolder : mode == 2 ? I::SzModeHere : I::SzModeSmart);
}
inline const std::wstring& ModeDesc(int mode) {
    using I = l10n::StringId;
    return l10n::Get(mode == 1 ? I::SzModeFolderDesc : mode == 2 ? I::SzModeHereDesc : I::SzModeSmartDesc);
}
inline const wchar_t* FormatName(int format) {
    static constexpr const wchar_t* names[] = {L"7z", L"zip", L"tar", L"tar.gz", L"tar.xz", L"tar.bz2"};
    return names[format < 0 || format > 5 ? 0 : format];
}
inline const std::wstring& FormatDesc(int format) {
    using I = l10n::StringId;
    static constexpr I ids[] = {I::SzFmt7z, I::SzFmtZip, I::SzFmtTar, I::SzFmtTarGz, I::SzFmtTarXz, I::SzFmtTarBz2};
    return l10n::Get(ids[format < 0 || format > 5 ? 0 : format]);
}
inline const std::wstring& LevelName(int level) {
    using I = l10n::StringId;
    static constexpr I ids[] = {I::SzLevel0, I::SzLevel1, I::SzLevel2, I::SzLevel3, I::SzLevel4, I::SzLevel5};
    return l10n::Get(ids[level < 0 || level > 5 ? 3 : level]);
}
inline const std::wstring& CommandName(int index) {
    using I = l10n::StringId;
    static constexpr I ids[] = {I::SzCmdExtract, I::SzCmdTo, I::SzCmdTest, I::SzCmdQuick, I::SzCmdAdd, I::SzCmdHash};
    return l10n::Get(ids[index < 0 || index > 5 ? 0 : index]);
}
inline std::wstring CommandTag(int index) {
    using I = l10n::StringId;
    if (index <= 2) return l10n::Get(I::SzForArchives) + (index == 0 ? L" · " + l10n::Get(I::SzWithResult) : std::wstring{});
    if (index <= 4) return l10n::Get(I::SzForFiles) + (index == 3 ? L" · " + l10n::Get(I::SzWithResult) : std::wstring{});
    return l10n::Get(I::SzForFile);
}
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
    if(row==2)return item.id=="pulse.7zip"?L"7-Zip 26.00 (Igor Pavlov)":std::wstring{};
    if(row==3)return item.license;
    if(row==4)return l10n::Get(I::SzAboutPermissionsValue);
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
// Rows of the context menu preview: a caption "7-Zip" then the commands (a detail line adds height).
inline float MenuPreviewHeight(const std::vector<SevenZipMenuRow>& rows, bool submenu, float s) {
    float h = (8 + (submenu ? 30 : 26)) * s;
    for (const auto& row : rows) h += (row.detail.empty() ? 30.0f : 42.0f) * s;
    return h + (6 + (submenu ? 37 : 0)) * s;
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
} // namespace store_view

// Browse: the store home.
float LayoutSettingsStore(SettingsLayout& l, const WindowViewModel& vm, float s,
                          float y, const fluent::Painter* painter) {
    using I = l10n::StringId;
    const auto& view = vm.settings_store_view;
    const auto& z = view.seven_zip;
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
    // Banners: the last action's notice and the pending restart, each its own tinted line.
    if ((!view.notice.empty() || view.state==ExtensionStoreView::State::Offline) && !failed) { l.store_notice = caption(view.state==ExtensionStoreView::State::Offline ? l10n::Get(I::StoreOfflineDesc) : view.notice, 14*s); y += 10*s; }
    if (view.pending_restart) {
        const float rbw = (std::min)(right - left - 28*s, width(l10n::Get(I::StoreRestartNow)));
        const bool inline_btn = right - left - 28*s - rbw - 12*s >= 180*s;
        if (inline_btn) {
            const float text_w = right - left - 28*s - rbw - 12*s;
            const float th = height(l10n::Get(I::StoreRestart), text_w);
            const float row_h = (std::max)(th, bh);
            const float ty = y + (row_h - th) / 2;
            const float by = y + (row_h - bh) / 2;
            l.store_restart = D2D1::RectF(left + 14*s, ty, right - 14*s - rbw - 12*s, ty + th);
            l.store_restart_button = D2D1::RectF(right - 14*s - rbw, by, right - 14*s, by + bh);
            y += row_h + 22*s;
        } else {
            l.store_restart = caption(l10n::Get(I::StoreRestart), 14*s);
            l.store_restart_button = D2D1::RectF(left + 14*s, y - 4*s, (std::min)(right - 14*s, left + 14*s + rbw), y - 4*s + bh);
            y = l.store_restart_button.bottom + 18*s;
        }
    }
    if (failed) {
        // The failed card: what happened, what to try, the diagnostic, retry, and local file install.
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
        if (!view.notice.empty()) { l.store_notice = D2D1::RectF(tx, cy, text_r, cy + height(view.notice, text_r - tx)); cy = l.store_notice.bottom + 4*s; }
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
    // The 7-Zip card: full width, its quick settings and the menu it makes.
    const bool hero_has_settings = z.item >= 0 && z.item < static_cast<int>(view.items.size()) &&
        store_view::HasSettings(view, view.items[static_cast<size_t>(z.item)]);
    const int hero = z.item >= 0 && z.item < static_cast<int>(view.items.size()) && (hero_has_settings || listed) &&
        store_view::Shown(view, 1, view.items[static_cast<size_t>(z.item)].name, view.items[static_cast<size_t>(z.item)].summary) ? z.item : -1;
    l.store_items.assign(view.items.size(), {});
    if (hero >= 0) {
        const auto& item = view.items[static_cast<size_t>(hero)];
        SettingsLayout::StoreCard c;
        c.feature = true;
        const float pad = 22*s, tile = 72*s;
        const float top = y;
        c.icon = D2D1::RectF(left + pad, top + pad, left + pad + tile, top + pad + tile);
        // Actions at the top right: 全部设置 › (when installed), and the catalog action (下载并安装 / 更新).
        const auto& all = l10n::Get(I::StoreAllSettings);
        const float aw = (std::min)(right - left - 2*pad, width(all) + 14*s);
        float actions_left = right - pad;
        if (hero_has_settings) {
            c.settings = D2D1::RectF(right - pad - aw, top + pad, right - pad, top + pad + bh);
            actions_left = c.settings.left - 8*s;
        }
        if (!item.button_label.empty() && (item.action_enabled || !hero_has_settings)) {
            const float bw = (std::min)(right - left - 2*pad, width(item.button_label));
            c.button = D2D1::RectF(actions_left - bw, top + pad, actions_left, top + pad + bh);
            actions_left = c.button.left - 8*s;
        }
        const float tx = c.icon.right + 18*s;
        const float text_r = actions_left - 12*s > tx + 160*s ? actions_left - 12*s : right - pad;
        const float name_h = painter ? painter->MeasureWrappedSectionHeight(item.name, (std::max)(1.0f, text_r - tx)) : 28*s*font;
        c.name = D2D1::RectF(tx, top + pad - 2*s, text_r, top + pad - 2*s + (std::max)(26*s*font, name_h));
        float publisher_top=c.name.bottom+3*s;
        if(item.enabled) {
            const float bw=(std::min)(text_r-tx,(painter ? painter->MeasureBadgeWidth(l10n::Get(I::StoreExtensionEnabled)) : 100*s*font) + 12*s);
            // The title draws at 20 dip semibold; the button measure is 14 dip body text plus
            // 16 dip padding, so scale its text part (with a little slack for the weight).
            const float nw=painter ? (painter->MeasureButtonWidth(item.name) - 16*s) * 1.5f + 8*s : 160*s*font;
            if(tx+nw+8*s+bw<=text_r) {
                c.name.right=tx+nw;
                const float badge_h = 22*s*font;
                const float badge_y = (c.name.top + c.name.bottom - badge_h) / 2;
                c.enabled_badge=D2D1::RectF(c.name.right+8*s,badge_y,c.name.right+8*s+bw,badge_y+badge_h);
            } else {
                c.enabled_badge=D2D1::RectF(tx,publisher_top,tx+bw,publisher_top+22*s*font);
                publisher_top=c.enabled_badge.bottom+4*s;
            }
        }
        c.publisher = D2D1::RectF(tx, publisher_top, text_r, publisher_top + 20*s*font);
        c.summary = D2D1::RectF(tx, c.publisher.bottom + 6*s, text_r, c.publisher.bottom + 6*s + height(item.summary, text_r - tx));
        float body = (std::max)(c.icon.bottom, c.summary.bottom) + 18*s;
        if (text_r > actions_left - 12*s && (store_view::Has(c.settings) || store_view::Has(c.button))) {
            // Narrow: the actions move under the text.
            if (store_view::Has(c.settings)) {
                c.settings = D2D1::RectF(left+pad, body, left+pad+aw, body+bh);
                if (store_view::Has(c.button)) {
                    const float bw = c.button.right-c.button.left;
                    const bool wrap = c.settings.right+8*s+bw > right-pad;
                    const float bx = wrap ? left+pad : c.settings.right+8*s;
                    const float by = wrap ? c.settings.bottom+8*s : body;
                    c.button = D2D1::RectF(bx,by,bx+bw,by+bh);
                }
                body = (std::max)(c.settings.bottom,c.button.bottom)+18*s;
            } else if (store_view::Has(c.button)) {
                const float bw = c.button.right-c.button.left;
                c.button = D2D1::RectF(left+pad, body, left+pad+bw, body+bh);
                body = c.button.bottom + 18*s;
            }
        }
        if (hero_has_settings) {
            auto& q = l.store_quick;
            q.on = true;
            q.section = D2D1::RectF(left, body, right, body);
            const bool split = right - left >= 680*s;
            const float preview_w = split ? 256*s : 0.0f;
            const float ql = left + pad, qr = right - preview_w - pad;
            float qy = body + 16*s;
            q.title = D2D1::RectF(ql, qy, qr, qy + 22*s*font);
            const auto& saved_str = z.status.empty() ? l10n::Get(view.saved ? I::StoreSaved : I::StoreAutoSaved) : z.status;
            const float saved_w = (std::min)(qr - ql, (painter ? painter->MeasureBadgeWidth(saved_str) : 138*s*font) + 22*s);
            q.saved = D2D1::RectF((std::max)(ql, qr - saved_w), qy, qr, qy + 22*s*font);
            const bool stacked_quick = qr-ql < 520*s*font;
            if (stacked_quick) { q.saved = D2D1::RectF(ql, q.title.bottom+4*s, (std::min)(qr, ql+saved_w), q.title.bottom+4*s+22*s*font); }
            else q.title.right=q.saved.left-8*s;
            qy = q.saved.bottom + 6*s;
            const float row_h = (stacked_quick ? 92 : 54)*s*font;
            for (int i = 0; i < 3; ++i) { q.rows[i] = D2D1::RectF(ql, qy, qr, qy + row_h); qy += row_h; }
            const float ch = 32*s*font;
            auto control = [&](int row, float w, float right_edge) {
                const float cy = stacked_quick ? q.rows[row].bottom-ch-6*s : (q.rows[row].top + q.rows[row].bottom - ch)/2;
                return D2D1::RectF(right_edge - w, cy, right_edge, cy + ch);
            };
            q.mode = control(0, 196*s, qr);
            q.level = control(1, 104*s, qr);
            q.format = control(1, 104*s, q.level.left - 8*s);
            q.remember = control(2, 40*s, qr);
            q.remember.top += (ch - 20*s)/2; q.remember.bottom = q.remember.top + 20*s;
            float qbottom = qy + 12*s;
            // The menu preview: right column, or below the rows when narrow.
            const float menu_h = store_view::MenuPreviewHeight(z.archive_rows, z.submenu, s);
            if (split) {
                q.preview = D2D1::RectF(right - preview_w, body, right, body);
                q.preview_caption = D2D1::RectF(q.preview.left + 16*s, body + 16*s, right - 16*s, body + 16*s + 20*s);
                q.preview_menu = D2D1::RectF(q.preview.left + 14*s, q.preview_caption.bottom + 8*s, right - 14*s, q.preview_caption.bottom + 8*s + menu_h);
                qbottom = (std::max)(qbottom, q.preview_menu.bottom + 16*s);
                q.preview.bottom = qbottom;
            } else {
                q.preview = D2D1::RectF(left, qbottom, right, qbottom);
                q.preview_caption = D2D1::RectF(left + pad, qbottom + 12*s, right - pad, qbottom + 32*s);
                q.preview_menu = D2D1::RectF(left + pad, q.preview_caption.bottom + 8*s, right - pad, q.preview_caption.bottom + 8*s + menu_h);
                qbottom = q.preview_menu.bottom + 16*s;
                q.preview.bottom = qbottom;
            }
            q.section.bottom = qbottom;
            body = qbottom;
        }
        c.bounds = D2D1::RectF(left, top, right, body);
        l.store_feature = c;
        l.store_feature_item=hero;
        y = c.bounds.bottom + 18*s;
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

// The 7-Zip detail page: header, four tabs, and the menu preview beside them.
float LayoutSevenZipDetail(SettingsLayout& l, const WindowViewModel& vm, float s,
                           float y, const fluent::Painter* painter) {
    using I = l10n::StringId;
    const auto& view = vm.settings_store_view;
    const auto& z = view.seven_zip;
    auto& d = l.sz;
    d.on = true;
    const float left = l.content.left + 20*s, right = l.content.right - 20*s;
    const float font = static_cast<float>(vm.settings_ui_font_scale)/100;
    const float bh = 32*s*font;
    auto height = [&](std::wstring_view value, float w) {
        return value.empty() ? 0.0f : painter ? painter->MeasureWrappedCaptionHeight(value, (std::max)(1.0f, w)) : 40*s*font;
    };
    auto width = [&](std::wstring_view value) {
        return (std::min)(right-left, painter ? painter->MeasureButtonWidth(value) : 144*s*font);
    };
    const auto& item = view.items[static_cast<size_t>(z.item)];
    // Crumb: ‹ 扩展商店 › name.
    const float crumb_w = width(l10n::Get(I::StoreTitle)) + 8*s;
    d.crumb = D2D1::RectF(left - 6*s, y, left + crumb_w, y + 28*s*font);
    d.crumb_name = D2D1::RectF(d.crumb.right + 4*s, y, right, y + 28*s*font);
    y = d.crumb.bottom + 16*s;
    // Header: the tile, name, the publisher line and badges; the switch and ⋯ at the right.
    d.icon = D2D1::RectF(left, y, left + 72*s, y + 72*s);
    const float more_w = 36*s;
    d.more = D2D1::RectF(right - more_w, y + 18*s, right, y + 18*s + bh);
    d.enable = D2D1::RectF(d.more.left - 12*s - 40*s, y + 18*s + (bh - 20*s)/2, d.more.left - 12*s, y + 18*s + (bh + 20*s)/2);
    const float label_w = 64*s*font;
    d.enable_label = D2D1::RectF(d.enable.left - 10*s - label_w, y + 18*s, d.enable.left - 10*s, y + 18*s + bh);
    const float tx = d.icon.right + 18*s;
    const bool narrow_head = d.enable_label.left - tx < 260*s;
    const float tr = narrow_head ? right : d.enable_label.left - 16*s;
    d.name = D2D1::RectF(tx, y - 2*s, tr, y + 34*s*font);
    d.sub = D2D1::RectF(tx, d.name.bottom + 2*s, tr, d.name.bottom + 2*s + height(L"x", tr - tx));
    float bx = tx;
    float by = d.sub.bottom + 8*s;
    const std::wstring badges[3] = {
        l10n::Get(item.enabled ? I::StoreExtensionEnabled : I::StoreExtensionDisabled),
        l10n::Get(I::StoreNativeCode),
        item.version.empty() ? std::wstring{} : item.version + (item.size.empty() ? std::wstring{} : L" · " + item.size)};
    for (int i = 0; i < 3; ++i) {
        if (badges[i].empty()) continue;
        const float bw = (std::min)(right-tx, (painter ? painter->MeasureBadgeWidth(badges[i]) : 120*s) + (i == 0 ? 14*s : 0.0f));
        if (bx>tx && bx+bw>right) { bx=tx; by+=24*s*font+6*s; }
        d.badges[i] = D2D1::RectF(bx, by, bx + bw, by + 24*s*font);
        bx = d.badges[i].right + 6*s;
    }
    y = (std::max)(d.icon.bottom, by+24*s*font) + 18*s;
    if (narrow_head) {
        // Narrow: the switch row goes under the badges.
        d.enable_label = D2D1::RectF(left, y, left + label_w, y + bh);
        d.enable = D2D1::RectF(d.enable_label.right + 10*s, y + (bh - 20*s)/2, d.enable_label.right + 50*s, y + (bh + 20*s)/2);
        d.more = D2D1::RectF(d.enable.right + 12*s, y, d.enable.right + 12*s + more_w, y + bh);
        y += bh + 14*s;
    }
    if (!item.enabled || z.needs_restart || view.pending_restart) {
        const bool show_restart_btn = item.enabled && (z.needs_restart || view.pending_restart);
        const auto banner_text = l10n::Get(!item.enabled ? I::SzDisabledNote : I::StoreRestart);
        if (show_restart_btn) {
            const float rbw = (std::min)(right - left - 28*s, width(l10n::Get(I::StoreRestartNow)));
            const bool inline_btn = right - left - 28*s - rbw - 12*s >= 180*s;
            if (inline_btn) {
                const float text_w = right - left - 28*s - rbw - 12*s;
                const float th = height(banner_text, text_w);
                const float row_h = (std::max)(th, bh);
                d.disabled_banner = D2D1::RectF(left + 14*s, y + (row_h - th)/2, right - 14*s - rbw - 12*s, y + (row_h + th)/2);
                l.store_restart_button = D2D1::RectF(right - 14*s - rbw, y + (row_h - bh)/2, right - 14*s, y + (row_h + bh)/2);
                y += row_h + 20*s;
            } else {
                d.disabled_banner = D2D1::RectF(left + 14*s, y, right - 14*s, y + height(banner_text, right - left - 28*s));
                l.store_restart_button = D2D1::RectF(left + 14*s, d.disabled_banner.bottom + 8*s, left + 14*s + rbw, d.disabled_banner.bottom + 8*s + bh);
                y = l.store_restart_button.bottom + 20*s;
            }
        } else {
            d.disabled_banner = D2D1::RectF(left + 14*s, y, right - 14*s, y + height(banner_text, right - left - 28*s));
            y = d.disabled_banner.bottom + 20*s;
        }
    }
    // Tabs (pivot).
    float tx2 = left;
    const float tab_h = 38*s*font;
    for (int i = 0; i < 4; ++i) {
        static constexpr I ids[] = {I::SzTabSettings, I::SzTabMenu, I::SzTabFormats, I::SzTabAbout};
        const float tw = (painter ? painter->MeasureButtonWidth(l10n::Get(ids[i])) : 96*s);
        if (tx2 > left && tx2 + tw > right) { tx2 = left; y += tab_h + 6*s; }
        d.tabs[i] = D2D1::RectF(tx2, y, tx2 + tw, y + tab_h);
        tx2 += tw + 4*s;
    }
    y = d.tabs[3].bottom + 20*s;
    // Body and the preview column.
    const bool split = right - left >= 680*s;
    const float side_w = split ? (right - left >= 780*s ? 286*s : 264*s) : 0.0f;
    const float ml = left, mr = split ? right - side_w - 20*s : right;
    d.body = D2D1::RectF(ml, y, mr, y);
    const float opt_gap = 8*s;
    const float option_left = ml + (mr-ml < 440*s ? 16 : 62)*s;
    auto group_begin = [&](int index, std::wstring_view label) {
        if (index >= 0 && index < 3) { d.section_label[index] = D2D1::RectF(ml + 2*s, y, mr, y + 22*s*font); }
        (void)label;
        y += 22*s*font + 8*s;
        return y;
    };
    // A row: 32 dip icon tile + title over description; returns the row and moves y.
    auto head_row = [&](std::wstring_view desc, float control_w) {
        const float textw = mr - ml - 32*s - 46*s - control_w;
        const float h = 16*s + 22*s*font + height(desc, textw) + 14*s;
        const auto r = D2D1::RectF(ml, y, mr, y + (std::max)(64*s, h));
        y = r.bottom;
        return r;
    };
    if (z.TabAvailable(view.detail_tab) || view.detail_tab == 3) {
        if (view.detail_tab == 0) {
            // 接管压缩包 (archive_icons_mockup.html): the switch, one row per format, what a double-click does.
            if (z.take_available) {
                d.take_label = D2D1::RectF(ml + 2*s, y, mr, y + 22*s*font);
                y += 22*s*font + 8*s;
                const float gt = y;
                d.take_row = head_row(l10n::Get(I::SzTakeDesc), 60*s);
                const float mid = (d.take_row.top + d.take_row.bottom) / 2;
                d.take_switch = D2D1::RectF(mr - 16*s - 40*s, mid - 10*s, mr - 16*s, mid + 10*s);
                const float ll = ml + 16*s, lr = mr - 16*s;
                y += 6*s;
                // The summary line, with 逐个确认 and (for several) 在 Windows 设置里一起改 at its right.
                float buttons_left = lr;
                if (!z.take_busy && z.ask_any) {
                    const float nw = width(l10n::Get(I::SzTakeConfirmEach));
                    d.take_next = D2D1::RectF(lr - nw, y, lr, y + bh);
                    buttons_left = d.take_next.left - 8*s;
                    if (z.ask_many) {
                        const float ww = width(l10n::Get(I::SzTakeWinSettings));
                        if (buttons_left - ww - ll >= 160*s) {
                            d.take_winsettings = D2D1::RectF(buttons_left - ww, y, buttons_left, y + bh);
                            buttons_left = d.take_winsettings.left - 8*s;
                        }
                    }
                }
                const auto& head = z.take_busy ? l10n::Get(I::SzTakeBusy) : z.take_head;
                const float head_h = (std::max)(bh, height(head, buttons_left - ll - 8*s));
                d.take_head = D2D1::RectF(ll + 8*s, y, buttons_left - 8*s, y + head_h);
                y += head_h + 6*s;
                if (!z.take_busy) {
                    for (const auto& row : z.take_rows) {
                        SettingsLayout::SevenZipDetail::TakeRow r;
                        const float ask_w = row.ask ? width(l10n::Get(row.ask_windows ? I::SzTakeAskSet : I::SzTakeAskChange)) : 0.0f;
                        const float name_w = (std::clamp)((lr - ll) * 0.3f, 128*s, 220*s);
                        const float status_x = ll + 8*s + 18*s + 12*s + 24*s + 10*s + name_w + 8*s;
                        const float status_r = row.ask ? lr - 8*s - ask_w - 8*s : lr - 8*s;
                        // Narrow: the status goes under the name.
                        const bool stacked = status_r - status_x < 120*s;
                        const float row_h = (stacked ? 62 : 46)*s*font;
                        r.bounds = D2D1::RectF(ll, y, lr, y + row_h);
                        const float cy = stacked ? y + 23*s*font : y + row_h / 2;
                        r.check = D2D1::RectF(ll + 8*s, cy - 9*s, ll + 26*s, cy + 9*s);
                        r.icon = D2D1::RectF(r.check.right + 12*s, cy - 12*s, r.check.right + 36*s, cy + 12*s);
                        r.name = D2D1::RectF(r.icon.right + 10*s, y + 4*s, stacked ? lr - 8*s - ask_w - 8*s : r.icon.right + 10*s + name_w, y + 42*s*font);
                        r.status = stacked ? D2D1::RectF(r.name.left, r.name.bottom, lr - 8*s, r.bounds.bottom - 4*s)
                                           : D2D1::RectF(r.name.right + 8*s, y, status_r, r.bounds.bottom);
                        if (row.ask) r.ask = D2D1::RectF(lr - 8*s - ask_w, cy - bh / 2, lr - 8*s, cy + bh / 2);
                        d.take_rows.push_back(r);
                        y = r.bounds.bottom;
                    }
                }
                y += 10*s;
                d.double_click_row = head_row(l10n::Get(I::SzDblDesc), 0);
                const bool two = mr - ml - 78*s >= 440*s;
                for (int i = 0; i < 2; ++i) {
                    const float ow = two ? (mr - 16*s - option_left - opt_gap) / 2 : mr - 16*s - option_left;
                    const auto& desc = l10n::Get(i ? I::SzDblOpenDesc : I::SzModeSmartDesc);
                    const float oh = 14*s + 20*s*font + height(desc, ow - 46*s) + 12*s;
                    const float ox = option_left + (two ? i * (ow + opt_gap) : 0.0f);
                    const float oy = two || i == 0 ? y : d.double_click_opts[0].bottom + opt_gap;
                    d.double_click_opts[i] = D2D1::RectF(ox, oy, ox + ow, oy + (std::max)(62*s*font, oh));
                }
                if (two) {
                    const float tallest = (std::max)(d.double_click_opts[0].bottom, d.double_click_opts[1].bottom);
                    d.double_click_opts[0].bottom = d.double_click_opts[1].bottom = tallest;
                }
                y = d.double_click_opts[1].bottom + opt_gap + 10*s;
                d.take_group = D2D1::RectF(ml, gt, mr, y);
                y += 22*s;
            }
            const float g0 = group_begin(0, l10n::Get(I::SzSectionExtract));
            d.mode_row = head_row(l10n::Get(I::SzModeDesc), 0);
            for (int i = 0; i < 3; ++i) {
                const float h = 14*s + 20*s*font + height(store_view::ModeDesc(i), mr - ml - 46*s - 56*s) + 12*s;
                d.mode_opts[i] = D2D1::RectF(option_left, y, mr - 16*s, y + h);
                y = d.mode_opts[i].bottom + opt_gap;
            }
            y += 8*s;
            d.location_row = head_row(l10n::Get(I::SzLocationDesc), 0);
            const bool two = mr - ml - 78*s >= 440*s;
            for (int i = 0; i < 2; ++i) {
                const float ow = two ? (mr - 16*s - (option_left) - opt_gap) / 2 : mr - 16*s - (option_left);
                const float ox = option_left + (two ? i * (ow + opt_gap) : 0.0f);
                const float oy = two ? y : y + i * (62*s*font + opt_gap);
                d.location_opts[i] = D2D1::RectF(ox, oy, ox + ow, oy + 62*s*font);
            }
            y = d.location_opts[1].bottom + opt_gap;
            if (z.location == 1) {
                const float bw = width(l10n::Get(I::Browse));
                d.browse = D2D1::RectF(mr - 16*s - bw, y + 2*s, mr - 16*s, y + 2*s + bh);
                d.folder = D2D1::RectF(option_left, y + 2*s, d.browse.left - 8*s, y + 2*s + bh);
                if (d.folder.right-d.folder.left < 140*s) {
                    d.folder.right=mr-16*s;
                    d.browse.top=d.folder.bottom+8*s; d.browse.bottom=d.browse.top+bh;
                }
                y = d.browse.bottom + opt_gap;
            }
            y += 10*s;
            d.groups[0] = D2D1::RectF(ml, g0, mr, y);
            y += 22*s;
            const float g1 = group_begin(1, l10n::Get(I::SzSectionCompress));
            d.format_row = head_row(store_view::Fill(l10n::Get(I::SzFormatDesc), L"{name}", store_view::FormatName(z.format)), 0);
            const int cols = mr-option_left-16*s >= 520*s ? 3 : 2;
            const float tw = (mr - 16*s - (option_left) - opt_gap * (cols - 1)) / cols;
            float tile_bottom = y;
            for (int i = 0; i < 6; ++i) {
                const float h = 14*s + 20*s*font + height(store_view::FormatDesc(i), tw - 46*s) + 12*s;
                const int col = i % cols, row = i / cols;
                (void)row;
                if (col == 0 && i) y = tile_bottom + opt_gap;
                d.format_opts[i] = D2D1::RectF(option_left + col * (tw + opt_gap), y, option_left + col * (tw + opt_gap) + tw, y + h);
                tile_bottom = (std::max)(tile_bottom, d.format_opts[i].bottom);
            }
            // Tiles of one row share the tallest height.
            for (int i = 0; i < 6; i += cols) {
                float tallest = 0;
                for (int k = i; k < i + cols && k < 6; ++k) tallest = (std::max)(tallest, d.format_opts[k].bottom - d.format_opts[k].top);
                for (int k = i; k < i + cols && k < 6; ++k) d.format_opts[k].bottom = d.format_opts[k].top + tallest;
            }
            y = tile_bottom + 14*s;
            d.level_row = head_row(l10n::Get(I::SzLevelDesc), 0);
            d.level_value = D2D1::RectF(option_left,y,mr-16*s,y+22*s*font);
            y=d.level_value.bottom+8*s;
            d.level_track = D2D1::RectF(option_left, y, mr - 16*s, y + fluent::StepSliderHeight(s));
            for (int i = 0; i < 6; ++i) {
                const float cx=fluent::StepSliderStopX(d.level_track,6,static_cast<float>(i),s);
                const float hit_left=i==0 ? d.level_track.left : (cx+fluent::StepSliderStopX(d.level_track,6,static_cast<float>(i-1),s))/2;
                const float hit_right=i==5 ? d.level_track.right : (cx+fluent::StepSliderStopX(d.level_track,6,static_cast<float>(i+1),s))/2;
                d.level_stops[i] = D2D1::RectF(hit_left,y,hit_right,d.level_track.bottom+26*s*font);
            }
            y = d.level_track.bottom + 26*s*font;
            d.groups[1] = D2D1::RectF(ml, g1, mr, y);
            y += 12*s;
            d.compress_note = D2D1::RectF(ml + 24*s, y, mr, y + height(l10n::Get(I::SzCompressNote), mr - ml - 24*s));
            y = d.compress_note.bottom + 22*s;
            const float g2 = group_begin(2, l10n::Get(I::SzSectionPassword));
            d.remember_row = head_row(l10n::Get(I::SzRememberDesc), 60*s);
            d.remember = D2D1::RectF(mr - 16*s - 40*s, (d.remember_row.top + d.remember_row.bottom)/2 - 10*s, mr - 16*s, (d.remember_row.top + d.remember_row.bottom)/2 + 10*s);
            d.groups[2] = D2D1::RectF(ml, g2, mr, y);
            y += 12*s;
            d.rar_note = D2D1::RectF(ml + 24*s, y, mr, y + height(l10n::Get(I::SzRarNote), mr - ml - 24*s));
            y = d.rar_note.bottom + 24*s;
        } else if (view.detail_tab == 1) {
            const float g0 = group_begin(0, l10n::Get(I::SzPlacementTitle));
            y += 14*s;
            const bool two = mr - ml - 32*s >= 440*s;
            for (int i = 0; i < 2; ++i) {
                const float ow = two ? (mr - ml - 32*s - opt_gap) / 2 : mr - ml - 32*s;
                const float ox = ml + 16*s + (two ? i * (ow + opt_gap) : 0.0f);
                const float oy = two ? y : y + i * (62*s*font + opt_gap);
                d.placement_opts[i] = D2D1::RectF(ox, oy, ox + ow, oy + 62*s*font);
            }
            y = d.placement_opts[1].bottom + 16*s;
            d.groups[0] = D2D1::RectF(ml, g0, mr, y);
            y += 22*s;
            d.commands_label = D2D1::RectF(ml + 2*s, y, mr, y + 22*s*font);
            y += 22*s*font + 8*s;
            const float g1 = y;
            for (int i = 0; i < 6; ++i) {
                d.command_rows[i] = D2D1::RectF(ml, y, mr, y + 58*s*font);
                d.command_switch[i] = D2D1::RectF(mr - 16*s - 40*s, (d.command_rows[i].top + d.command_rows[i].bottom)/2 - 10*s,
                                                  mr - 16*s, (d.command_rows[i].top + d.command_rows[i].bottom)/2 + 10*s);
                y = d.command_rows[i].bottom;
            }
            d.groups[1] = D2D1::RectF(ml, g1, mr, y);
            y += 24*s;
        } else if (view.detail_tab == 2) {
            const float g0 = y;
            y += 16*s;
            auto chips = [&](D2D1_RECT_F& label, std::vector<D2D1_RECT_F>& out, const std::vector<std::wstring>& names) {
                label = D2D1::RectF(ml + 16*s, y, mr - 16*s, y + 22*s*font);
                y = label.bottom + 8*s;
                float cx = ml + 16*s;
                const float ch = 24*s*font;
                out.clear();
                for (const auto& name : names) {
                    const float cw = painter ? painter->MeasureMonospaceBadgeWidth(name) : (28.0f+12.0f*static_cast<float>(name.size()))*s*font;
                    if (cx > ml + 16*s && cx + cw > mr - 16*s) { cx = ml + 16*s; y += ch + 6*s; }
                    out.push_back(D2D1::RectF(cx, y, cx + cw, y + ch));
                    cx += cw + 6*s;
                }
                y += ch + 16*s;
            };
            chips(d.formats_rw_label, d.chips_rw, z.formats_rw);
            chips(d.formats_ro_label, d.chips_ro, z.formats_ro);
            d.groups[0] = D2D1::RectF(ml, g0, mr, y);
            y += 12*s;
            d.formats_note = D2D1::RectF(ml + 24*s, y, mr, y + height(l10n::Get(I::SzFormatsNote), mr - ml - 24*s));
            y = d.formats_note.bottom + 24*s;
        } else {
            const float g0 = y;
            y += 10*s;
            const bool stacked=mr-ml<400*s*font;
            for(int i=0;i<6;++i) {
                const auto value=store_view::AboutValue(view,item,i);
                const float key_width=120*s*font;
                const float vl=stacked?ml+16*s:ml+16*s+key_width+16*s;
                const float vy=stacked?y+22*s*font:y;
                const float value_height=(std::max)(22*s*font,height(value,mr-16*s-vl-(i==0&&item.official?18*s:0)));
                d.about_keys[i]={ml+16*s,y,stacked?mr-16*s:vl-16*s,y+22*s*font};
                d.about_values[i]={vl,vy,mr-16*s,vy+value_height};
                d.about_rows[i]={ml+16*s,y,mr-16*s,d.about_values[i].bottom};
                y=d.about_rows[i].bottom+8*s;
            }
            if(item.source.starts_with(L"https://")||item.source.starts_with(L"http://"))d.about_source=d.about_values[5];
            y += 8*s;
            d.groups[0] = D2D1::RectF(ml, g0, mr, y);
            if(store_view::HistoryCount(item)>0) {
                y += 22*s;
                d.history_label = D2D1::RectF(ml+2*s,y,mr,y+22*s*font);
                y=d.history_label.bottom+8*s;
                const float g1=y;
                const auto installed_version=store_view::InstalledVersion(view,item);
                for(size_t i=0;i<store_view::HistoryCount(item)&&i<8;++i) {
                    const bool current=!installed_version.empty()&&store_view::HistoryVersion(item,i)==installed_version;
                    const auto note=store_view::HistoryNote(item,i);
                    const float badge_width=current?(painter?painter->MeasureBadgeWidth(l10n::Get(I::SzAboutCurrent)):64*s):0;
                    const float version_width=(std::min)(100*s*font,(mr-ml-32*s)*.35f);
                    const bool wrap=mr-ml<440*s*font;
                    const float nl=wrap?ml+16*s:ml+16*s+version_width+12*s;
                    const float nr=mr-16*s-(current&&!wrap?badge_width+12*s:0);
                    const float ny=y+(wrap?36*s*font:10*s);
                    const float nh=note.empty()?0:(std::max)(22*s*font,height(note,nr-nl));
                    const float rh=(std::max)(48*s*font,ny-y+nh+10*s);
                    d.history_rows.push_back({ml,y,mr,y+rh});
                    d.history_versions.push_back({ml+16*s,y+10*s,ml+16*s+version_width,y+34*s*font});
                    d.history_notes.push_back({nl,ny,nr,ny+nh});
                    d.history_current.push_back(current?D2D1::RectF(mr-16*s-badge_width,y+12*s,mr-16*s,y+36*s):D2D1::RectF());
                    // Registry management knows the rollback target; catalog order alone does not.
                    d.history_rollback.push_back({});
                    y+=rh;
                }
                d.history_card={ml,g1,mr,y};
            }
            y += 24*s;
        }
    } else {
        // Not running yet (or off): one line, and the generic plugin page stays reachable.
        const auto& note=l10n::Get(z.available?I::SzExtensionUpdateRequired:item.enabled?I::SzLoading:I::SzUnavailable);
        d.groups[0] = D2D1::RectF(ml, y, mr, y + (std::max)(72*s,40*s+height(note,mr-ml-40*s)));
        y = d.groups[0].bottom + 24*s;
    }
    d.body.bottom = y;
    // The preview card and the result note.
    float sy = split ? d.body.top : y;
    const float sl = split ? right - side_w : left, sr = right;
    if (z.available) {
        const auto& rows = view.preview_kind == 0 ? z.archive_rows : z.file_rows;
        d.side = D2D1::RectF(sl, sy, sr, sy);
        d.preview_card = D2D1::RectF(sl, sy, sr, sy);
        const float kinds_w = 2 * 56*s*font;
        const bool inline_kinds = (sr - sl - 28*s - kinds_w - 10*s) >= 112*s*font;
        if (inline_kinds) {
            d.preview_kinds[0] = D2D1::RectF(sr - 14*s - kinds_w, sy + 12*s, sr - 14*s - kinds_w/2, sy + 12*s + 26*s*font);
            d.preview_kinds[1] = D2D1::RectF(d.preview_kinds[0].right, sy + 12*s, sr - 14*s, sy + 12*s + 26*s*font);
            d.preview_title = D2D1::RectF(sl + 14*s, sy + 12*s, d.preview_kinds[0].left - 8*s, sy + 12*s + 26*s*font);
        } else {
            d.preview_title = D2D1::RectF(sl + 14*s, sy + 14*s, sr - 14*s, sy + 14*s + 22*s*font);
            d.preview_kinds[0] = D2D1::RectF(sr - 14*s - kinds_w, d.preview_title.bottom + 6*s, sr - 14*s - kinds_w/2, d.preview_title.bottom + 6*s + 26*s*font);
            d.preview_kinds[1] = D2D1::RectF(d.preview_kinds[0].right, d.preview_kinds[0].top, sr - 14*s, d.preview_kinds[0].bottom);
        }
        const float menu_top = d.preview_kinds[0].bottom + 12*s;
        const float menu_h = store_view::MenuPreviewHeight(rows, z.submenu, s) + (view.preview_kind==0 ? 227 : 171)*s;
        d.preview_menu = D2D1::RectF(sl + 14*s, menu_top, sr - 14*s, menu_top + menu_h);
        d.preview_card.bottom = d.preview_menu.bottom + 14*s;
        const auto& result = l10n::Get(z.mode == 1 ? I::SzResultFolder : z.mode == 2 ? I::SzResultHere : I::SzResultSmart);
        d.result_card = D2D1::RectF(sl, d.preview_card.bottom + 12*s, sr, d.preview_card.bottom + 12*s + 28*s + height(result, sr - sl - 56*s));
        d.side.bottom = d.result_card.bottom;
        y = split ? (std::max)(y, d.side.bottom + 24*s) : d.side.bottom + 24*s;
    }
    return y;
}
