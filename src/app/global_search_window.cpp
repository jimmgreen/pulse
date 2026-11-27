#include "global_search_window.h"
#include "global_search_foreground_policy.h"
#include "global_search_monitor.h"
#include "global_search_activation.h"
#include "recycle_search.h"
#include "../common/runtime_log.h"
#include "../common/navigation_known_folders.h"
#include "global_search_result_state.h"
#include "global_search_query.h"
#include "global_search_arrange.h"
#include "entry_group.h"
#include "live_folder_scope.h"
#include "app_catalog.h"
#include "folder_alias.h"
#include "../fs/fs_enum.h"
#include "jump_history.h"
#include "jump_match.h"
#include "../ui/FluentTokens.h"
#include "../ui/command_icons.h"
#include "resource.h"
#include "../ui/empty_state_layout.h"
#include "../ui/fluent_components.h"
#include "../ui/typography.h"
#include "../ui/svg_bitmap.h"
#include "../ui/edit_host.h"
#include "../ui/shell_icons.h"
#include "../ui/thumbnail_cache.h"
#include "../ui/quick_preview_window.h"
#include "../ui/name_highlight.h"
#include "../ui/icon_size_slider.h"
#include "../ui/fluent_menu.h"
#include "../ui/address_search_control.h"
#include "../ui/ui_motion.h"
#include "../common/path_utils.h"
#include <uxtheme.h>
#include "../common/localization.h"
#include "../index/index_client.h"
#include "../index/index_query.h"
#include "../index/content_search_client.h"
#include "content_scope_mode.h"
#include "content_pack_ui.h"
#include "../index/network_agent_client.h"
#include "../index/search_kinds.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <numeric>
#include <thread>
#include <tuple>

namespace pulse {
namespace {
using Microsoft::WRL::ComPtr;
using l10n::StringId;
constexpr UINT kIndexStatus = WM_APP + 181, kIndexResult = WM_APP + 182, kContentResult = WM_APP + 183;
constexpr UINT kNetworkResult = WM_APP + 184;
constexpr UINT kPreviewNavigate = WM_APP + 189, kPreviewOpen = WM_APP + 190;
constexpr UINT kAppsReady = WM_APP + 186;  // the Start menu app list is in
constexpr UINT kRecycleResult = WM_APP + 187;
constexpr UINT kDirectoryResult = WM_APP + 188;
constexpr UINT kLiveResult = WM_APP + 191;
constexpr UINT kActivationReconcile = WM_APP + 192;
constexpr UINT_PTR kDebounce = 1;
constexpr UINT_PTR kConnectTimeout = 2;
constexpr UINT_PTR kScopeTooltipTimer = 3;
constexpr UINT_PTR kAnimationTimer = 4;
constexpr uint64_t kSession = 0x50554c534547534full;
// Layout in DIPs.
constexpr float kHeader = 72, kFooter = 42, kRow = 52, kContentRow = 78, kGroupRow = 30;
constexpr float kSide = 216, kPreview = 318, kResizeBorder = 6;
constexpr float kFieldLeft = 62, kFieldTop = 14, kFieldHeight = 44, kFieldRight = 70;
constexpr float kMinWidth = app::kSearchMinWidth, kMinHeight = app::kSearchMinHeight;
constexpr float kDefaultWidth = app::kSearchDefaultWidth, kDefaultHeight = app::kSearchDefaultHeight;
constexpr size_t kMaximumResults = 200;
// Aliased folders listed ahead of the best matches.
constexpr size_t kMaximumAliases = 3;
constexpr size_t kJumpRows = 4;  // 跳转 group: known folders above the index results
constexpr size_t kAppRows = 4;   // 应用 group: Start menu programs after the best matches
constexpr size_t kMaximumPinnedScopes = 3, kMaximumNetworkScopes = 2;
const std::wstring& Text(StringId id) { return l10n::Get(id); }
std::wstring ParentPath(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash == 2 ? 3 : slash);
}
std::wstring LeafName(const std::wstring& path) {
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const auto slash = trimmed.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= trimmed.size() ? trimmed : trimmed.substr(slash + 1);
}
bool IsNetworkPath(const std::wstring& path) {
    const auto plain = path::StripExtendedPathPrefix(path);
    return plain.size() > 2 && plain[0] == L'\\' && plain[1] == L'\\';
}
bool UnderFolder(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty()) return true;
    const auto p = path::StripExtendedPathPrefix(path), f = path::StripExtendedPathPrefix(folder);
    if (p.size() < f.size()) return false;
    if (CompareStringOrdinal(p.data(), static_cast<int>(f.size()), f.data(), static_cast<int>(f.size()), TRUE) != CSTR_EQUAL) return false;
    return p.size() == f.size() || f.back() == L'\\' || p[f.size()] == L'\\';
}
// Shell providers may block. This worker owns all its data and outlives the popup safely.
void OpenResult(std::wstring path, bool location) {
    std::thread([path = std::move(path), location] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (app::IsAppPath(path)) {
            // Start menu programs (Store apps too) start through their shell item.
            PIDLIST_ABSOLUTE item = nullptr;
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
                SHELLEXECUTEINFOW info{sizeof(info)};
                info.fMask = SEE_MASK_IDLIST | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
                info.lpIDList = item; info.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&info);
                CoTaskMemFree(item);
            }
        } else if (location) {
            PIDLIST_ABSOLUTE item = nullptr;
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) {
                SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                CoTaskMemFree(item);
            }
        } else {
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&info);
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).detach();
}
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) { color.a = alpha; return color; }
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
}

struct GlobalSearchWindow::Impl {
    HWND hwnd = nullptr, edit = nullptr;
    app::SearchActivationState activation;
    HWND opening_foreground = nullptr;
    HFONT edit_font = nullptr;
    ComPtr<IDWriteTextFormat> edit_format;
    HBRUSH background = nullptr;
    bool dark = true, content_mode = false, current_only = false, composing = false, busy = false, truncated = false, search_pinyin = true;
    float scale = 1, width = kDefaultWidth, height = kDefaultHeight;
    // Extra header height (DIP) when the interface font size makes the query
    // line taller than the default field; everything below moves down by it.
    float grow = 0;
    std::wstring current_folder, current_folder_tip, query, error;
    std::wstring raw_query, last_search_query;
    app::GlobalSearchParsedQuery parsed_query;
    std::optional<app::GlobalSearchCalculation> calculation;
    double previous_answer = 0;
    std::vector<app::GlobalSearchCommand> commands;
    bool command_mode = false;
    struct Row {
        std::wstring name, path, snippet; bool directory = false;
        uint64_t size = 0, mtime = 0; bool network = false;
        bool app = false;  // a Start menu program (path: its AppsFolder item)
        std::wstring alias;  // a folder found by its alias, shown as 「alias · name」
        std::wstring recycle_path;
        app::GlobalSearchRowClass cls = app::GlobalSearchRowClass::File;
    };
    std::vector<Row> rows;
    // Search results as merged (relevance order). `rows` is their shown order:
    // sorted, grouped, collapsed groups left out (Arrange).
    std::vector<Row> merged_rows;
    bool arranged = false;
    app::GlobalSearchSort sort_order = app::GlobalSearchSort::Relevance;
    bool sort_desc = false;
    app::GlobalSearchFolders folder_order = app::GlobalSearchFolders::Top;
    app::GlobalSearchGroup group_by = app::GlobalSearchGroup::Match;
    std::vector<std::wstring> collapsed;  // group keys
    D2D1_RECT_F sort_button{}, group_button{};
    // Sidebar facets: counts from complete results only (the index has no
    // count API), kept while the query and scope stay the same.
    int size_filter = 0;
    std::wstring ext_filter;
    std::wstring facet_key;
    std::array<size_t, 8> kind_counts{};
    std::array<size_t, 5> size_counts{};
    bool kind_counts_known = false, size_counts_known = false, ext_counts_known = false;
    int ext_kind = 0;
    std::vector<std::pair<std::wstring, size_t>> ext_counts;
    float side_scroll = 0, side_content = 0;
    bool hot_clip = false;
    bool order_reset = false;  // next merged results start at the top
    float hot_clip_top = 0, hot_clip_bottom = 0;
    struct DirectoryRequest {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        std::vector<Row> rows;
        std::wstring error;
    };
    std::shared_ptr<DirectoryRequest> directory_request;
    std::wstring browse_folder;
    std::vector<std::wstring> browse_history, marked_paths;
    std::vector<std::wstring> breadcrumb_paths;
    bool grid_view = false, tray_open = false;
    size_t tray_offset = 0;
    D2D1_RECT_F tray_bounds{};
    bool row_drag_pending = false, row_dragging = false;
    POINT row_drag_start{};
    int timeline_first = -1, timeline_last = -1, timeline_anchor = -1;
    D2D1_RECT_F timeline_rect{};
    float tile_width = 150;
    D2D1_RECT_F tile_size_slider{};
    bool tile_size_dragging = false;
    int selected = 0, first = 0;
    uint32_t generation = 1;
    size_t total = 0;
    index::IndexClient filenames;
    index::NetworkAgentClient network;
    index::SearchResult local_result, network_result;
    index::SearchResult recycle_result;
    struct RecycleRequest {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        index::SearchResult result;
    };
    std::shared_ptr<RecycleRequest> recycle_request;
    bool recycle_ready = true;
    struct LiveRequest {
        std::atomic<bool> cancelled{false};
        std::mutex mutex;
        index::LiveNetworkMatches matches;
        index::Query query;
    };
    std::shared_ptr<LiveRequest> live_request;
    app::GlobalProviderStatus providers;
    index::ContentSearchClient contents;
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> write_factory;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1Bitmap> logo;
    ID2D1RenderTarget* logo_target = nullptr;
    ComPtr<ID2D1Bitmap> empty_art;
    ID2D1RenderTarget* empty_art_target = nullptr;
    float empty_art_scale = 0;
    ui::Compositor compositor;
    ui::fluent::Painter painter;
    ui::FluentMenu result_menu;
    bool result_menu_open = false;
    ui::WindowMaterial material;
    ui::ShellIconCache icons;
    ui::ThumbnailCache grid_thumbnails;
    ui::QuickPreviewWindow preview;
    bool preview_dark = true, preview_occluded = false;
    ui::QuickPreviewItem preview_requested;
    bool grid_cache_ready = false;
    ui::WindowEffect effect = ui::WindowEffect::None;
    std::wstring background_image;
    D2D1_COLOR_F accent_color = ui::GetAccentColor();
    bool backdrop = false;
    bool scope_tooltip_visible = false, tracking_mouse = false;
    GlobalSearchHandoffHandler handoff;
    GlobalSearchContext context;
    GlobalSearchEvents events;
    D2D1_RECT_F handoff_rect{};
    bool handoff_hover = false;
    float scope_pointer_x = 0, scope_pointer_y = 0;

    // --- redesign state -------------------------------------------------
    enum class ScopeKind : uint8_t { All, Current, Known, Pinned, Network };
    struct Scope {
        ScopeKind kind = ScopeKind::All;
        std::wstring label, path;
        const wchar_t* glyph = L"";
        size_t count = 0;
        bool counted = false;
        const KNOWNFOLDERID* known_id = nullptr;
    };
    std::vector<Scope> scopes;
    int scope = 0, kind_filter = 0, date_filter = 0;
    std::wstring counts_key;
    size_t best_count = 0, folder_count = 0, jump_count = 0;
    std::vector<Row> jump_rows;  // known folders matching the query (跳转)
    app::AppCatalog app_catalog;
    std::vector<app::AppEntry> apps;
    std::vector<Row> app_rows;   // Start menu programs matching the query (应用)
    size_t app_count = 0;
    wchar_t swallow_char = 0;
    bool stale = false, stale_content_mode = false;
    bool showing_recent = false, keyboard_nav = false, preview_open = false, animations = true;
    std::vector<float> row_top;
    float content_height = 0, scroll = 0, scroll_target = 0, recent_block = 0;
    uint64_t frame = 0, appear_start = 0, toast_until = 0, last_tick = 0;
    std::wstring toast;
    ui::motion::SpringRectMotion plate_motion, scope_motion;
    uint64_t layout_epoch = 1;
    enum Hot { HotNone, HotRow, HotScope, HotAddPlace, HotKind, HotDate, HotMode, HotEsc, HotHandoff, HotPreviewOpen,
               HotPreviewLocation, HotPreviewCopy, HotRecentQuery, HotClearFilters, HotSearchContent,
               HotView, HotBack, HotExitFolder, HotTray, HotStage, HotTrayItem, HotTrayRemove, HotTrayClear, HotTileSize, HotToken, HotCalculation, HotTimeline, HotAlternative, HotTrayPanel, HotBreadcrumb,
               HotSearchScope, HotSearchClear, HotSearchField, HotSort, HotGroupMenu, HotGroupHeader, HotGroupSelect,
               HotSize, HotExt };
    struct HotSpot { D2D1_RECT_F rect; Hot kind; int index; };
    std::vector<HotSpot> hot;
    Hot hover_kind = HotNone; int hover_index = -1;

    static constexpr index::SearchKind kKinds[] = {index::SearchKind::Any, index::SearchKind::Folder, index::SearchKind::Document,
        index::SearchKind::Image, index::SearchKind::Video, index::SearchKind::Audio, index::SearchKind::Archive, index::SearchKind::Code};
    static constexpr StringId kKindLabels[] = {StringId::FilterAll, StringId::KindFolder, StringId::KindDocument, StringId::KindImage,
        StringId::KindVideo, StringId::KindAudio, StringId::KindArchive, StringId::KindCode};
    static constexpr UINT32 kKindColors[] = {0, 0xf4c04e, 0x2b6fd6, 0x7b5ee0, 0xd0603a, 0x1f9d8a, 0x8a6d3b, 0x6b7385};
    static constexpr const wchar_t* kDateTokens[] = {L"", L" dm:today", L" dm:thisweek", L" dm:thismonth", L" dm:thisyear"};
    static constexpr StringId kDateLabels[] = {StringId::DateAny, StringId::DateToday, StringId::DateThisWeek, StringId::DateThisMonth, StringId::DateThisYear};
    // 大小 chips (files only): index 0 is no filter and has no chip.
    static constexpr const wchar_t* kSizeTokens[] = {L"", L" file: size:<1mb", L" file: size:>=1mb size:<128mb",
        L" file: size:>=128mb size:<1gb", L" file: size:>=1gb"};
    static constexpr StringId kSizeLabels[] = {StringId::DateAny, StringId::GlobalSearchSizeSmall, StringId::GlobalSearchSizeMedium,
        StringId::GlobalSearchSizeLarge, StringId::GlobalSearchSizeHuge};
    static int SizeBucket(uint64_t bytes) {
        constexpr uint64_t MB = 1024ull * 1024ull;
        return bytes < MB ? 1 : bytes < 128 * MB ? 2 : bytes < 1024 * MB ? 3 : 4;
    }
    int KindIndex(const Row& row) const {
        const auto kind = app::GlobalSearchKindOf(row.name, row.directory);
        for (int i = 1; i < 8; ++i) if (kKinds[i] == kind) return i;
        return 0;
    }
    UINT32 KindColor(index::SearchKind kind) const {
        for (int i = 1; i < 8; ++i) if (kKinds[i] == kind) return kKindColors[i];
        return 0;
    }

    ui::Theme Theme() const { return ui::IsHighContrast() ? ui::MakeHighContrastTheme() : ui::MakeTheme(dark, accent_color); }
    D2D1_COLOR_F EditBackground() const { return FieldColor(); }
    D2D1_COLOR_F FieldColor() const {
        const auto theme = Theme();
        return ui::AddressSearchHostedFill(theme, ui::IsHighContrast());
    }
    static UINT32 Rgb(D2D1_COLOR_F color) {
        return (static_cast<UINT32>(std::lround(color.r * 255)) << 16) |
            (static_cast<UINT32>(std::lround(color.g * 255)) << 8) | static_cast<UINT32>(std::lround(color.b * 255));
    }
    ui::WindowEffect EffectiveEffect() const {
        return !ui::IsHighContrast() && ui::SystemAllowsPopupAcrylic() ? ui::WindowEffect::Acrylic : ui::WindowEffect::None;
    }
    static constexpr UINT_PTR kDiagnosticTimer = 0x475344;
    uint64_t diagnostic_until = 0, diagnostic_id = 0;
    unsigned diagnostic_events = 0;
    HWND diagnostic_foreground = nullptr;
    void Trace(const char* event, uint64_t value = 0) {
        if (!diagnostic_until || GetTickCount64() > diagnostic_until || diagnostic_events++ >= 256) return;
        const HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        diagnostics::runtime::Event(event, {{"id", diagnostic_id}, {"value", value},
            {"foreground", reinterpret_cast<uintptr_t>(foreground)}, {"foreground_pid", pid},
            {"self_foreground", foreground == hwnd}, {"visible", hwnd && IsWindowVisible(hwnd)},
            {"focus", reinterpret_cast<uintptr_t>(GetFocus())}, {"active", reinterpret_cast<uintptr_t>(GetActiveWindow())},
            {"activation_state", static_cast<uint64_t>(activation.Visibility())},
            {"activation_generation", activation.Generation()}, {"trigger", static_cast<uint64_t>(activation.Trigger())}});
    }
    void ApplyAppearance() {
        Trace("search_appearance");
        if (!hwnd) return;
        backdrop = ui::ApplyWindowEffect(hwnd, EffectiveEffect(), dark);
        if (background) DeleteObject(background);
        background = CreateSolidBrush(Background());
        Invalidate();
        if (edit) InvalidateRect(edit, nullptr, TRUE);
    }

    int Px(float dip) const { return static_cast<int>(std::lround(dip * scale)); }
    COLORREF Background() const {
        const auto color = FieldColor();
        return RGB(static_cast<BYTE>(color.r * 255), static_cast<BYTE>(color.g * 255), static_cast<BYTE>(color.b * 255));
    }
    D2D1_COLOR_F Color(UINT32 value) const { return D2D1::ColorF(value); }
    void Invalidate() { SyncPreview(); if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    float Header() const { return kHeader + grow; }
    float ResultBodyTop() const { return Header() + 43 + (CategoryStrip() ? kCategoryStrip : 0.0f); }
    struct CalculationPanel {
        D2D1_RECT_F card{};
        std::array<D2D1_RECT_F, 5> alternatives{};
        size_t alternative_count = 0;
        float bottom = 0;
        bool compact = false;
    };
    CalculationPanel CalculationPanelLayout() const {
        CalculationPanel layout;
        layout.bottom = ResultBodyTop();
        if (!calculation || command_mode || content_mode) return layout;
        const float left = ListLeft() + 14, right = ListRight() - 14;
        const float available = ListBottom() - layout.bottom;
        layout.compact = available < 330 || right - left < 440;
        const float card_height = layout.compact ? 56.0f : 104.0f;
        layout.card = {left, layout.bottom + 6, right, layout.bottom + 6 + card_height};
        layout.alternative_count = std::min(calculation->alternatives.size(), layout.alternatives.size());
        layout.bottom = layout.card.bottom + 6;
        if (layout.alternative_count) {
            // Equal-width chips keep every conversion accessible without allowing
            // wrapped alternatives to consume the first file's viewport.
            const int columns = std::min(static_cast<int>(layout.alternative_count),
                std::max(1, static_cast<int>((right - left + 6) / 80)));
            const float step = (right - left + 6) / static_cast<float>(columns);
            const float chip_height = layout.compact ? 22.0f : 26.0f;
            const float row_step = chip_height + 4;
            for (size_t i = 0; i < layout.alternative_count; ++i) {
                const float x = left + static_cast<float>(i % columns) * step;
                const float y = layout.bottom + static_cast<float>(i / columns) * row_step;
                layout.alternatives[i] = {x, y, x + step - 6, y + chip_height};
            }
            layout.bottom += static_cast<float>((layout.alternative_count + columns - 1) / columns) * row_step + 2;
        }
        return layout;
    }
    float ListTop() const { return CalculationPanelLayout().bottom; }
    bool ResultSelectionActive() const {
        return selected >= 0 && selected < static_cast<int>(rows.size()) &&
            (keyboard_nav || (!showing_recent && !calculation));
    }
    // Narrow windows hide the sidebar; its type chips move to a strip under the result bar.
    static constexpr float kCategoryStrip = 38;
    bool CategoryStrip() const {
        return SideWidth() <= 0 && !content_mode && !command_mode && !showing_recent && browse_folder.empty();
    }
    const std::vector<Row>& AllRows() const { return arranged ? merged_rows : rows; }
    bool FileFilters() const { return kind_filter != 0 || size_filter != 0 || !ext_filter.empty(); }
    float ListBottom() const { return height - kFooter; }
    float SideWidth() const {
        // At compact widths use the space for the requested preview, keeping
        // the result list usable. Closing preview restores the scope sidebar.
        return width >= (preview_open ? 760.0f : 700.0f) ? kSide : 0.0f;
    }
    float PreviewWidth() const { return preview_open ? std::min(kPreview, width * 0.5f) : 0.0f; }
    float ListLeft() const { return SideWidth() + (SideWidth() > 0 ? 1.0f : 0.0f); }
    float ListRight() const { return width - PreviewWidth(); }
    float RowHeight() const { return content_mode && !showing_recent ? kContentRow : kRow; }
    bool IsGrid() const { return grid_view && !content_mode && !command_mode; }
    bool HasSearchQuery() const { return !query.empty() || (!content_mode && (FileFilters() || date_filter != 0 || timeline_first >= 0)); }
    int GridColumns() const { return std::max(1, static_cast<int>((ListRight() - ListLeft() - 16) / tile_width)); }
    float TileHeight() const { return tile_width * .75f + 54; }
    int PageSize() const { return std::max(1, static_cast<int>((ListBottom() - ListTop() - 12) / RowHeight())); }

    // ---- list layout -----------------------------------------------------
    struct Group {
        StringId title{};
        size_t start = 0, count = 0;  // shown rows; 0 while collapsed
        size_t total = 0;             // rows in the group, shown or not
        std::wstring label, detail, key;
        UINT32 dot = 0;
        bool header = true, foldable = false, app = false;
    };
    std::vector<Group> groups;
    std::vector<float> group_top;  // header top per group (content coordinates)
    std::vector<int> row_col;      // grouped grid: column of each tile
    bool grid_grouped = false;
    const std::vector<Group>& Groups() const { return groups; }
    std::wstring GroupTitle(const Group& group) const { return group.label.empty() ? Text(group.title) : group.label; }
    bool Collapsed(const std::wstring& key) const { return std::find(collapsed.begin(), collapsed.end(), key) != collapsed.end(); }
    std::vector<Group> LegacyGroups() const {
        std::vector<Group> out;
        const auto add = [&](StringId title, size_t start, size_t count) {
            Group group; group.title = title; group.start = start; group.count = group.total = count;
            out.push_back(std::move(group));
        };
        if (rows.empty()) return out;
        if (showing_recent) { add(StringId::ColumnRecentOpened, 0, rows.size()); return out; }
        if (content_mode) { add(StringId::GlobalSearchContentMatches, 0, rows.size()); return out; }
        const size_t j = (std::min)(jump_count, rows.size());
        if (j) add(StringId::GlobalSearchJump, 0, j);
        // 跳转, 最佳匹配, 应用, then the (long) folder and file lists.
        const size_t b = (std::min)(best_count, rows.size() - j);
        if (b) add(StringId::GlobalSearchBestMatch, j, b);
        const size_t a = (std::min)(app_count, rows.size() - j - b);
        if (a) add(StringId::GlobalSearchGroupApps, j + b, a);
        const size_t f = (std::min)(folder_count, rows.size() - j - b - a);
        if (f) add(StringId::Folder, j + b + a, f);
        const size_t files = rows.size() - j - b - a - f;
        if (files) add(StringId::FilterFiles, j + b + a + f, files);
        return out;
    }
    // ---- sort and group ----------------------------------------------------
    app::GlobalSearchArrangeOptions ArrangeOptions() const {
        app::GlobalSearchArrangeOptions options;
        options.sort = sort_order; options.descending = sort_desc; options.folders = folder_order;
        options.group = group_by; options.apps_first = context.apps_first;
        return options;
    }
    // merged_rows -> rows and groups in the chosen order.
    void Arrange() {
        std::vector<app::GlobalSearchArrangeItem> items;
        items.reserve(merged_rows.size());
        for (const auto& row : merged_rows) items.push_back({row.name, row.path, row.directory, row.size, row.mtime, row.cls});
        const auto clock = app::MakeGroupClock();
        const auto spans = app::ArrangeGlobalSearch(items, ArrangeOptions(), &clock);
        rows.clear(); groups.clear();
        rows.reserve(merged_rows.size());
        for (const auto& span : spans) {
            Group group;
            group.title = span.title; group.label = span.label; group.detail = span.detail; group.key = span.key;
            group.dot = span.kind != index::SearchKind::Any ? KindColor(span.kind) : 0;
            group.header = span.header; group.foldable = span.foldable; group.app = span.app;
            group.start = rows.size(); group.total = span.items.size();
            if (!(span.foldable && Collapsed(span.key)))
                for (const size_t i : span.items) rows.push_back(merged_rows[i]);
            group.count = rows.size() - group.start;
            groups.push_back(std::move(group));
        }
        arranged = true;
    }
    // Order or collapse changed without new results: keep the selected row.
    void Rearrange(bool from_top = false) {
        if (!arranged) return;
        std::wstring selected_path;
        if (selected >= 0 && selected < static_cast<int>(rows.size())) selected_path = rows[static_cast<size_t>(selected)].path;
        Arrange();
        ++layout_epoch;
        selected = rows.empty() ? -1 : 0;
        // A new order reads from the top; folding keeps the row in hand.
        if (from_top) scroll = scroll_target = 0;
        else for (size_t i = 0; i < rows.size(); ++i) if (!selected_path.empty() && rows[i].path == selected_path) { selected = static_cast<int>(i); break; }
        BuildLayout();
        ClampSelection();
        SyncPreview();
        Invalidate();
    }
    void BuildLayout() {
        if (!arranged) groups = LegacyGroups();
        row_top.assign(rows.size(), 0);
        row_col.assign(rows.size(), 0);
        group_top.assign(groups.size(), 0);
        if (IsGrid()) {
            recent_block = 0;
            const int columns = GridColumns();
            grid_grouped = std::any_of(groups.begin(), groups.end(), [](const Group& g) { return g.foldable; });
            if (!grid_grouped) {
                for (size_t i = 0; i < rows.size(); ++i) {
                    row_top[i] = 8 + static_cast<float>(i / columns) * TileHeight();
                    row_col[i] = static_cast<int>(i % columns);
                }
                content_height = 16 + static_cast<float>((rows.size() + columns - 1) / columns) * TileHeight();
                ClampScroll(); return;
            }
            float y = 6;
            for (size_t g = 0; g < groups.size(); ++g) {
                const auto& group = groups[g];
                if (group.header) { group_top[g] = y; y += kGroupRow + 4; }
                for (size_t k = 0; k < group.count; ++k) {
                    row_top[group.start + k] = y + static_cast<float>(k / columns) * TileHeight();
                    row_col[group.start + k] = static_cast<int>(k % columns);
                }
                y += static_cast<float>((group.count + columns - 1) / columns) * TileHeight() + 4;
            }
            content_height = y + 8;
            ClampScroll(); return;
        }
        float y = 6;
        recent_block = showing_recent && !context.recent_queries.empty() ? 70.0f : 0.0f;
        y += recent_block;
        for (size_t g = 0; g < groups.size(); ++g) {
            const auto& group = groups[g];
            if (group.header) { group_top[g] = y; y += kGroupRow; }
            for (size_t i = group.start; i < group.start + group.count; ++i) { row_top[i] = y; y += RowHeight(); }
            y += 4;
        }
        content_height = y + 8;
        ClampScroll();
    }
    float MaxScroll() const { return std::max(0.0f, content_height - (ListBottom() - ListTop())); }
    void ClampScroll() {
        scroll_target = std::clamp(scroll_target, 0.0f, MaxScroll());
        scroll = std::clamp(scroll, 0.0f, MaxScroll());
        UpdateFirst();
    }
    void UpdateFirst() {
        first = 0;
        for (size_t i = 0; i < row_top.size(); ++i) {
            if (row_top[i] + RowHeight() > scroll_target) { first = static_cast<int>(i); break; }
        }
    }
    void ClampSelection(bool reveal_selection = true) {
        if (rows.empty()) selected = -1;
        else if (selected >= 0) selected = std::min(selected, static_cast<int>(rows.size()) - 1);
        if (row_top.size() != rows.size()) BuildLayout();
        if (reveal_selection && selected >= 0 && selected < static_cast<int>(row_top.size())) {
            const float view = ListBottom() - ListTop();
            const float top = row_top[static_cast<size_t>(selected)], bottom = top + (IsGrid() ? TileHeight() : RowHeight());
            const bool first_in_group = top - kGroupRow <= 6 + recent_block + 1 ||
                (selected > 0 && row_top[static_cast<size_t>(selected) - 1] + RowHeight() + 1 < top);
            const float want_top = first_in_group ? top - kGroupRow - 4 : top - 4;
            if (want_top < scroll_target) scroll_target = want_top;
            else if (bottom + 8 > scroll_target + view) scroll_target = bottom + 8 - view;
        }
        ClampScroll();
        if (!animations) scroll = scroll_target;
    }
    void Cancel() {
        if (directory_request) directory_request->cancelled = true;
        directory_request.reset();
        if (recycle_request) recycle_request->cancelled = true;
        recycle_request.reset();
        recycle_ready = true;
        if (live_request) live_request->cancelled = true;
        live_request.reset();
        ++generation;
        filenames.CancelSession(kSession);
        contents.Suspend();
        busy = false;
        if (hwnd) { KillTimer(hwnd, kDebounce); KillTimer(hwnd, kConnectTimeout); }
    }
    void Hide() {
        Trace("search_hide");
        activation.Hide();
        ClosePreview();
        if (result_menu_open) result_menu.Dismiss();
        Cancel();
        HideScopeTooltip();
        handoff_hover = false;
        hover_kind = HotNone; hover_index = -1;
        if (hwnd) { KillTimer(hwnd, kAnimationTimer); ShowWindow(hwnd, SW_HIDE); }
        grid_thumbnails.TrimToBytes(0);
    }
    void FillRecent() {
        rows.clear();
        for (const auto& recent : context.recent_files) {
            Row row; row.path = recent.path; row.name = LeafName(recent.path); row.directory = recent.directory;
            row.network = IsNetworkPath(recent.path);
            if (!row.network) {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                if (GetFileAttributesExW(recent.path.c_str(), GetFileExInfoStandard, &data)) {
                    row.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    row.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                    row.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                } else continue;  // gone since it was opened
            }
            rows.push_back(std::move(row));
            if (rows.size() >= 6) break;
        }
        best_count = folder_count = jump_count = app_count = 0;
        total = rows.size();
        arranged = false; merged_rows.clear();
    }
    void Changed(bool reopening = false) {
        Cancel();
        if (!command_mode) last_search_query = raw_query;
        const int count = GetWindowTextLengthW(edit);
        query.resize(static_cast<size_t>(count) + 1);
        GetWindowTextW(edit, query.data(), count + 1);
        query.resize(static_cast<size_t>(count));
        raw_query = query;
        parsed_query = content_mode ? app::GlobalSearchParsedQuery{raw_query, {}} : app::ParseGlobalSearchQuery(raw_query);
        command_mode = !raw_query.empty() && raw_query.front() == L'>';
        calculation = !content_mode && !command_mode ? app::EvaluateGlobalSearchCalculation(raw_query, previous_answer) : std::nullopt;
        query = parsed_query.query;
        marked_paths.clear();
        keyboard_nav = false;
        commands = command_mode ? app::MatchGlobalSearchCommands(raw_query) : std::vector<app::GlobalSearchCommand>{};
        if (command_mode) {
            rows.clear(); showing_recent = stale = busy = false; selected = commands.empty() ? -1 : 0;
            for (const auto command : commands) { Row row; row.name = CommandLabel(command); rows.push_back(std::move(row)); }
            best_count = folder_count = jump_count = app_count = 0;
            arranged = false; merged_rows.clear();
            BuildLayout(); Invalidate(); return;
        }
        if (!browse_folder.empty() && !content_mode) { SearchDirectory(); return; }
        // Refining a search keeps the previous rows (dimmed) until the new
        // results land, so the list does not blank out on every keystroke.
        const bool keep = !reopening && !query.empty() && !showing_recent && !rows.empty() && content_mode == stale_content_mode;
        stale = keep;
        stale_content_mode = content_mode;
        if (!keep) {
            rows.clear(); merged_rows.clear(); arranged = false;
            selected = first = 0; best_count = folder_count = jump_count = app_count = 0; scroll = scroll_target = 0;
        }
        ComputeJump();
        ComputeApps();
        if (!keep && (!jump_rows.empty() || !app_rows.empty())) {
            // Known folders and programs answer at once; the index results
            // join around them.
            merged_rows = jump_rows; jump_count = merged_rows.size();
            merged_rows.insert(merged_rows.end(), app_rows.begin(), app_rows.end()); app_count = app_rows.size();
            Arrange();
            selected = 0;
        }
        total = 0; error.clear(); truncated = false;
        local_result = {}; network_result = {}; recycle_result = {}; providers = {};
        if (CountsKey() != counts_key) for (auto& item : scopes) item.counted = false;
        showing_recent = !HasSearchQuery();
        if (showing_recent) {
            FillRecent(); selected = rows.empty() ? -1 : 0;
            // A summon presents the refreshed recent list in its first frame.
            appear_start = reopening ? 0 : ui::motion::NowMs();
        }
        else {
            // Known folders and apps match in memory, so they show at once; the
            // index results join them.
            if (!keep && !rows.empty()) { selected = 0; appear_start = ui::motion::NowMs(); }
            total = jump_count + app_count;
        }
        ++layout_epoch;
        BuildLayout();
        if (HasSearchQuery() && !composing) { busy = true; SetTimer(hwnd, kDebounce, 140, nullptr); }
        SyncPreview();
        Invalidate();
    }
    std::wstring FilterTokens() const {
        if (content_mode) return {};
        std::wstring out;
        const auto kind = kKinds[kind_filter];
        if (kind == index::SearchKind::Folder) out += L" folder:";
        else if (kind != index::SearchKind::Any) out += std::wstring(L" type:") + index::KindId(kind);
        if (!ext_filter.empty()) out += L" ext:" + ext_filter;
        out += kSizeTokens[size_filter];
        out += kDateTokens[date_filter];
        if (timeline_first >= 0) {
            SYSTEMTIME local{}; GetLocalTime(&local);
            local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
            FILETIME stamp{}; SystemTimeToFileTime(&local, &stamp);
            const uint64_t today = (static_cast<uint64_t>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
            const auto date = [&](int days) {
                const uint64_t ticks = today - static_cast<uint64_t>(days) * 864000000000ull;
                FILETIME file{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
                SYSTEMTIME time{}; FileTimeToSystemTime(&file, &time);
                wchar_t text[16]{}; swprintf_s(text, L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
                return std::wstring(text);
            };
            out += L" dm:" + date((11 - timeline_first) * 7 + 6) + L".." + date((11 - timeline_last) * 7);
        }
        return out;
    }
    // The facet counts hold while everything but the type, extension and size chips stays.
    std::wstring FacetKey() const {
        return query + L"|" + ScopePath() + kDateTokens[date_filter] + L"|" + std::to_wstring(timeline_first) + L"-" +
            std::to_wstring(timeline_last) + (search_pinyin ? L"|p" : L"|n");
    }
    // Name, date and size orders come sorted from the index, so the first 200
    // are the right ones; type and path reorder the relevance results.
    void ApplyIndexOrder(index::Query& request) const {
        switch (sort_order) {
        case app::GlobalSearchSort::Name: request.sort = index::ResultSort::Name; break;
        case app::GlobalSearchSort::Modified: request.sort = index::ResultSort::Mtime; break;
        case app::GlobalSearchSort::Size: request.sort = index::ResultSort::Size; break;
        default: return;
        }
        request.rank = false;
        request.sort_desc = sort_desc;
    }
    static bool IndexSorted(app::GlobalSearchSort sort) {
        return sort == app::GlobalSearchSort::Name || sort == app::GlobalSearchSort::Modified || sort == app::GlobalSearchSort::Size;
    }
    std::wstring ScopePath() const {
        if (!browse_folder.empty()) return browse_folder;
        if (scope <= 0 || scope >= static_cast<int>(scopes.size())) return {};
        return scopes[static_cast<size_t>(scope)].path;
    }
    void Search() {
        KillTimer(hwnd, kDebounce);
        if (!IsWindowVisible(hwnd) || composing || !HasSearchQuery()) return;
        const auto folder = ScopePath();
        if (content_mode) {
            index::ContentSearchRequest request;
            request.session_id = kSession; request.generation = generation;
            request.needle = query; request.maximum_hits = kMaximumResults;
            request.indexed = true; request.task_scan = true;
            request.skip_system_locations = true;
            const auto config = contents.GetConfig();
            request.maximum_file_bytes = config.maximum_file_bytes;
            request.maximum_document_bytes = config.maximum_document_bytes;
            if (!folder.empty()) {
                request.root = folder;
                const auto scope_key = app::NormalizeLiveScopeKey(folder);
                const bool covered = std::any_of(config.roots.begin(), config.roots.end(), [&](const auto& root) {
                    const auto root_key = app::NormalizeLiveScopeKey(root.path);
                    return app::LiveScopePathUnder(scope_key, root_key) || app::LiveScopePathUnder(root_key, scope_key);
                });
                if (!covered) {
                    std::vector<index::VolumeInfo> volumes;
                    std::vector<std::wstring> excluded;
                    const bool scope_known = filenames.GetScope(volumes, excluded);
                    request.allow_uncovered_root = app::ShouldAllowUncoveredContentScope(
                        filenames.Ready(), scope_known, volumes, excluded, folder, false);
                }
            }
            else request.roots = app::DefaultContentSearchRoots(config.shared_scope);
            contents.SearchAsync(std::move(request));
        } else {
            if (live_request) live_request->cancelled = true;
            live_request.reset();
            if (recycle_request) recycle_request->cancelled = true;
            recycle_request.reset();
            providers = {};
            local_result = {};
            network_result = {};
            recycle_result = {};
            index::Query request;
            request.session_id = kSession; request.needle = query + FilterTokens(); request.limit = kMaximumResults;
            if (!search_pinyin) request.needle = L"nopinyin: " + request.needle;
            ApplyIndexOrder(request);
            request.path_prefix = folder;
            recycle_ready = !folder.empty();
            if (!recycle_ready) {
                auto job = std::make_shared<RecycleRequest>();
                recycle_request = job;
                std::thread([job, request, notify = hwnd, id = generation] {
                    auto result = app::SearchRecycleBin(request, [&] { return job->cancelled.load(); });
                    if (job->cancelled) return;
                    { std::lock_guard lock(job->mutex); job->result = std::move(result); }
                    if (!job->cancelled) PostMessageW(notify, kRecycleResult, id, 0);
                }).detach();
            }
            std::vector<index::VolumeInfo> volumes;
            std::vector<std::wstring> excluded;
            const bool scope_known = filenames.GetScope(volumes, excluded);
            const auto live_kind = app::ClassifyLiveFolderSearch(
                filenames.Ready(), scope_known, volumes, excluded, network.Roots(), folder, false);
            if (live_kind != app::LiveFolderSearchKind::None) {
                providers.Accept(false, 0);
                providers.ExpectNetwork(true);
                auto job = std::make_shared<LiveRequest>();
                job->query = request;
                live_request = job;
                std::thread([job, notify = hwnd, id = generation] {
                    index::LiveNetworkWalk(
                        job->query, job->matches, job->mutex,
                        [job] { return job->cancelled.load(); },
                        [job, notify, id] {
                            if (!job->cancelled.load()) PostMessageW(notify, kLiveResult, id, 0);
                        });
                    {
                        std::lock_guard lock(job->mutex);
                        job->matches.finished = true;
                        if (job->cancelled.load()) job->matches.error = ERROR_CANCELLED;
                    }
                    if (!job->cancelled.load()) PostMessageW(notify, kLiveResult, id, 0);
                }).detach();
            } else {
                filenames.SearchAsync(request, generation);
                const bool network_scope = folder.empty() || IsNetworkPath(folder);
                providers.ExpectNetwork(network_scope && !network.Roots().empty());
                if (network_scope) network.SearchAsync(request, generation);
                SetTimer(hwnd, kConnectTimeout, 10000, nullptr);
            }
        }
    }
    bool StartsWithQuery(const std::wstring& name) const {
        const auto begin = query.find_first_not_of(L" \t");
        if (begin == std::wstring::npos) return false;
        const auto end = query.find_first_of(L" \t", begin);
        const std::wstring word = query.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        return name.size() >= word.size() && CompareStringOrdinal(name.data(), static_cast<int>(word.size()),
            word.data(), static_cast<int>(word.size()), TRUE) == CSTR_EQUAL;
    }
    // 跳转: open tabs, Quick access and recent folders matching the query,
    // ranked by how they match (name, word start, pinyin, initials).
    void ComputeJump() {
        jump_rows.clear();
        if (content_mode || query.empty() || date_filter != 0 || size_filter != 0 || !ext_filter.empty()) return;
        const auto kind = kKinds[kind_filter];
        if (kind != index::SearchKind::Any && kind != index::SearchKind::Folder) return;
        const auto scope_path = ScopePath();
        std::vector<app::JumpCandidate> candidates;
        for (const auto& folder : context.jump_folders)
            if (UnderFolder(folder.path, scope_path)) candidates.push_back({folder.path, folder.label});
        for (const size_t i : app::RankJumpFolders(candidates, query, search_pinyin, kJumpRows)) {
            Row row; row.path = candidates[i].path; row.directory = true;
            row.name = candidates[i].label.empty() ? LeafName(row.path) : candidates[i].label;
            row.network = IsNetworkPath(row.path);
            row.cls = app::GlobalSearchRowClass::Jump;
            jump_rows.push_back(std::move(row));
        }
    }
    // 应用: Start menu programs whose name matches, the most started first
    // among equal matches. Only for a plain search everywhere.
    void ComputeApps() {
        app_rows.clear();
        if (!context.show_apps || content_mode || query.empty() || date_filter != 0 || FileFilters() || scope != 0 || apps.empty()) return;
        std::vector<size_t> order(apps.size());
        std::iota(order.begin(), order.end(), size_t{0});
        if (context.history) {
            std::vector<double> score(apps.size());
            for (size_t i = 0; i < apps.size(); ++i) score[i] = context.history->Score(apps[i].path, {});
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return score[a] > score[b]; });
        }
        std::vector<app::JumpCandidate> candidates;
        candidates.reserve(order.size());
        for (size_t i : order) candidates.push_back({apps[i].name, {}});
        for (const size_t k : app::RankJumpFolders(candidates, query, search_pinyin, kAppRows)) {
            Row row; row.name = apps[order[k]].name; row.path = apps[order[k]].path; row.app = true;
            row.cls = app::GlobalSearchRowClass::App;
            app_rows.push_back(std::move(row));
        }
    }
    // Aliased folders lead the best matches; up to three names starting with
    // the query follow.
    void GroupRows(std::vector<Row> aliases = {}) {
        std::vector<Row> best = std::move(aliases), folders, files;
        const size_t best_limit = best.size() + 3;
        for (auto& row : rows) {
            if (best.size() < best_limit && StartsWithQuery(row.name)) best.push_back(std::move(row));
            else if (row.directory) folders.push_back(std::move(row));
            else files.push_back(std::move(row));
        }
        best_count = best.size(); folder_count = folders.size();
        for (auto& row : best) row.cls = app::GlobalSearchRowClass::Best;
        for (auto& row : folders) row.cls = app::GlobalSearchRowClass::Folder;
        for (auto& row : files) row.cls = app::GlobalSearchRowClass::File;
        rows = std::move(best);
        for (auto& row : folders) rows.push_back(std::move(row));
        for (auto& row : files) rows.push_back(std::move(row));
    }
    std::wstring CountsKey() const { return (content_mode ? L"c|" : L"n|") + query + FilterTokens(); }
    void UpdateCounts() {
        const auto key = CountsKey();
        if (scope != 0) {
            if (key != counts_key) for (auto& item : scopes) item.counted = false;
            return;
        }
        counts_key = key;
        for (auto& item : scopes) {
            item.counted = !busy && (item.kind == ScopeKind::All || !truncated);
            item.count = item.kind == ScopeKind::All ? total : 0;
            if (item.kind == ScopeKind::All) continue;
            for (const auto& row : AllRows()) if (UnderFolder(row.path, item.path)) ++item.count;
        }
    }
    // Type, size and extension counts for the sidebar chips. Counted with no
    // file filter on (the extensions with the type chip on); numbers only
    // when the results are complete, the extension list either way.
    void UpdateFacets() {
        if (content_mode || busy || !error.empty()) return;
        const auto key = FacetKey();
        if (key != facet_key) {
            facet_key = key;
            kind_counts_known = size_counts_known = ext_counts_known = false;
            ext_counts.clear(); ext_kind = 0;
        }
        const auto& all = AllRows();
        if (!FileFilters()) {
            kind_counts = {}; size_counts = {};
            for (const auto& row : all) {
                ++kind_counts[0];  // 全部 shows apps too, so it matches the result count
                if (row.app) continue;
                if (const int kind = KindIndex(row)) ++kind_counts[static_cast<size_t>(kind)];  // 0: no type chip lists it
                if (!row.directory) ++size_counts[static_cast<size_t>(SizeBucket(row.size))];
            }
            kind_counts_known = size_counts_known = !truncated;
        }
        if (kind_filter >= 2 && size_filter == 0 && ext_filter.empty()) {
            std::vector<std::pair<std::wstring, size_t>> counts;
            for (const auto& row : all) {
                if (row.app || row.directory) continue;
                auto ext = app::GlobalSearchExtensionOf(row.name);
                if (ext.empty()) continue;
                auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == ext; });
                if (it == counts.end()) counts.push_back({std::move(ext), 1}); else ++it->second;
            }
            std::stable_sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            if (counts.size() > 8) counts.resize(8);
            ext_counts = std::move(counts); ext_kind = kind_filter; ext_counts_known = !truncated;
        }
    }
    // Folders whose alias (folder_alias.h) matches the query, as the omnibar
    // ranks them. Only the alias counts: the index finds folder names itself.
    // They obey the scope and the kind and date chips like any result.
    std::vector<Row> AliasRows() const {
        std::vector<Row> out;
        if (content_mode || showing_recent || query.empty() || size_filter != 0 || !ext_filter.empty()) return out;
        const auto kind = kKinds[kind_filter];
        if (kind != index::SearchKind::Any && kind != index::SearchKind::Folder) return out;
        const std::wstring scope_path = ScopePath();
        const index::CompiledQuery dates = index::ParseQuery(kDateTokens[date_filter]);
        const auto in_dates = [&](const std::wstring& path, const std::wstring& name, uint64_t mtime) {
            if (dates.groups.empty()) return true;
            return std::any_of(dates.groups.begin(), dates.groups.end(), [&](const auto& group) {
                return std::all_of(group.begin(), group.end(),
                                   [&](const index::Term& term) { return index::MatchTerm(path, name, true, 0, mtime, term); });
            });
        };
        for (auto& [rank, entry] : app::AliasStore::Instance().SuggestRanked(query, kMaximumAliases + 4)) {
            if (out.size() == kMaximumAliases) break;
            if (rank > 3 || (rank == 2 && !search_pinyin)) continue;  // 4: the folder's own name
            if (!UnderFolder(entry.path, scope_path)) continue;
            Row row{LeafName(entry.path), entry.path, {}, true};
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(entry.path.c_str(), GetFileExInfoStandard, &data))
                row.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            if (!in_dates(entry.path, row.name, row.mtime)) continue;
            row.alias = std::move(entry.alias);
            row.network = IsNetworkPath(entry.path);
            out.push_back(std::move(row));
        }
        return out;
    }
    void MergeFilenames() {
        const auto position = app::GlobalSearchPosition::Capture(rows, selected, first);
        const bool had_rows = !rows.empty();
        if (stale) { stale = false; scroll = scroll_target = 0; }
        rows.clear();
        index::Query request;
        request.needle = search_pinyin ? query + FilterTokens() : L"nopinyin: " + query + FilterTokens();
        request.limit = kMaximumResults;
        ApplyIndexOrder(request);
        auto candidates = local_result;
        candidates.total += recycle_result.total;
        candidates.hits.insert(candidates.hits.end(), recycle_result.hits.begin(), recycle_result.hits.end());
        const auto merged = index::MergeSearchResults(request, std::move(candidates), network_result, true);
        auto aliases = AliasRows();
        const auto same = [](const std::wstring& l, const std::wstring& r) {
            const auto a = path::StripExtendedPathPrefix(l), b = path::StripExtendedPathPrefix(r);
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        };
        ComputeJump();
        const auto listed = [&](const std::vector<Row>& list, const std::wstring& path) {
            return std::any_of(list.begin(), list.end(), [&](const Row& row) { return same(row.path, path); });
        };
        // A folder listed under 跳转 is not repeated under its alias.
        aliases.erase(std::remove_if(aliases.begin(), aliases.end(), [&](const Row& alias) { return listed(jump_rows, alias.path); }),
                      aliases.end());
        size_t duplicates = 0;
        for (const auto& hit : merged.hits) {
            if (hit.recycle_path.empty() && (listed(aliases, hit.path) || listed(jump_rows, hit.path))) {
                ++duplicates;  // already listed under its alias or 跳转
                continue;
            }
            Row row{hit.name, hit.path, {}, hit.is_dir};
            row.recycle_path = hit.recycle_path;
            row.size = hit.size; row.mtime = hit.mtime; row.network = IsNetworkPath(hit.path);
            rows.push_back(std::move(row));
        }
        const size_t alias_count = aliases.size();
        GroupRows(std::move(aliases));
        rows.insert(rows.begin(), jump_rows.begin(), jump_rows.end());
        jump_count = jump_rows.size();
        ComputeApps();
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(jump_count + best_count), app_rows.begin(), app_rows.end());
        app_count = app_rows.size();
        total = (std::max)(merged.total - (std::min)(merged.total, duplicates) + alias_count + jump_count + app_count, rows.size());
        truncated = total > rows.size(); busy = providers.Busy() || !recycle_ready;
        merged_rows = std::move(rows);
        Arrange();
        error = (providers.Error() || recycle_result.error) ? Text(merged_rows.empty() ? StringId::GlobalSearchFailed : StringId::SearchIncomplete) : std::wstring{};
        if (!busy) KillTimer(hwnd, kConnectTimeout);
        if (!had_rows && !rows.empty()) appear_start = ui::motion::NowMs();
        BuildLayout();
        // After a new index order the list starts at its first row.
        const int restored = order_reset ? (rows.empty() ? -1 : 0) : position.Selection(rows);
        order_reset = false;
        selected = restored;
        ClampSelection(false);
        if (restored < 0) selected = rows.empty() ? -1 : 0;
        UpdateCounts();
        UpdateFacets();
        SyncPreview();
        Invalidate();
    }
    void TimeoutFilenames() {
        providers.Timeout();
        if (!recycle_ready) {
            if (recycle_request) recycle_request->cancelled = true;
            recycle_ready = true;
            recycle_result.error = ERROR_TIMEOUT;
        }
        MergeFilenames();
    }
    void AcceptFilenames(bool from_network, uint32_t id, index::SearchResult result) {
        if (id != generation || content_mode) return;
        if (from_network && result.error && !providers.network_expected) return;
        providers.Accept(from_network, result.error);
        if (from_network) network_result = std::move(result);
        else local_result = std::move(result);
        MergeFilenames();
    }
    GlobalSearchHandoff MakeHandoff() const {
        GlobalSearchHandoff out;
        const auto begin = query.find_first_not_of(L" \t");
        if (begin != std::wstring::npos) out.query = query.substr(begin, query.find_last_not_of(L" \t") - begin + 1);
        out.content = content_mode;
        out.folder = ScopePath();
        out.filters = FilterTokens();
        return out;
    }
    void Open(bool location, void (*open_result)(std::wstring, bool) = OpenResult) {
        if (command_mode) { ExecuteCommand(); return; }
        if (stale || rows.empty() || selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto& row = rows[static_cast<size_t>(selected)];
        if (!row.recycle_path.empty()) {
            if (!handoff) return;
            auto request = MakeHandoff();
            request.folder = L"pulse:recycle";
            request.content = false;
            request.query = L"\"" + row.name + L"\"";
            request.filters.clear();
            const auto handler = handoff;
            Hide();
            handler(request);
            return;
        }
        auto path = row.path;
        if (row.app) {
            // An app has no location to reveal and stays out of the recent files.
            Hide(); open_result(std::move(path), false);
            return;
        }
        const bool directory = row.directory;
        const auto search = MakeHandoff();
        const auto opened = events.opened;
        const auto route = events.open_path;
        Hide();
        if (!route || !route(path, directory, location)) open_result(path, location);
        if (opened) opened(search, path, directory);
    }
    void Handoff() {
        if (!handoff) return;
        const auto request = MakeHandoff();
        const auto handler = handoff; // the handler may replace handoff
        Hide();
        handler(request);
    }
    bool InHandoff(float x, float y) const {
        return handoff && handoff_rect.right > handoff_rect.left &&
            x >= handoff_rect.left && x < handoff_rect.right && y >= handoff_rect.top && y < handoff_rect.bottom;
    }
    void HideScopeTooltip() {
        if (hwnd) KillTimer(hwnd, kScopeTooltipTimer);
        const bool redraw = scope_tooltip_visible;
        scope_tooltip_visible = false;
        if (redraw) Invalidate();
    }
    std::wstring TooltipText() const {
        if (hover_kind == HotSort) return Text(StringId::SortBy) + L"  Alt+S";
        if (hover_kind == HotGroupMenu) return Text(StringId::GroupBy) + L"  Alt+G";
        if (hover_kind == HotAlternative && calculation && hover_index >= 0 &&
            hover_index < static_cast<int>(calculation->alternatives.size()))
            return calculation->alternatives[static_cast<size_t>(hover_index)].text;
        if (hover_kind != HotScope || hover_index <= 0 || hover_index >= static_cast<int>(scopes.size())) return {};
        return path::StripExtendedPathPrefix(scopes[static_cast<size_t>(hover_index)].path);
    }
    void CopyPath() {
        if (command_mode) return;
        if (!marked_paths.empty()) {
            std::wstring text;
            for (const auto& path : marked_paths) { if (!text.empty()) text += L"\r\n"; text += path::StripExtendedPathPrefix(path); }
            CopyText(text); return;
        }
        if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto path = path::StripExtendedPathPrefix(rows[static_cast<size_t>(selected)].path);
        CopyText(path);
    }
    void CopyCalculation(bool expression) {
        if (!calculation) return;
        CopyText(expression ? raw_query : calculation->text);
        if (!expression && calculation->has_value) previous_answer = calculation->value;
    }
    const std::wstring& CommandLabel(app::GlobalSearchCommand command) const {
        switch (command) {
        case app::GlobalSearchCommand::ToggleTheme: return Text(StringId::TooltipToggleTheme);
        case app::GlobalSearchCommand::ToggleView: return Text(StringId::AiCtlStepView);
        case app::GlobalSearchCommand::OpenStaging: return Text(StringId::TrayShelfTitle);
        case app::GlobalSearchCommand::ClearStaging: return Text(StringId::TrayMenuClear);
        case app::GlobalSearchCommand::ContinueSearch: return Text(StringId::GlobalSearchHandoff);
        case app::GlobalSearchCommand::ClearHistory: return Text(StringId::SearchHistoryClear);
        }
        return Text(StringId::Open);
    }
    void ExecuteCommand() {
        if (selected < 0 || selected >= static_cast<int>(commands.size())) return;
        switch (commands[static_cast<size_t>(selected)]) {
        case app::GlobalSearchCommand::ToggleTheme: if (events.toggle_theme) events.toggle_theme(); break;
        case app::GlobalSearchCommand::ToggleView: grid_view = !grid_view; break;
        case app::GlobalSearchCommand::OpenStaging: tray_open = true; break;
        case app::GlobalSearchCommand::ClearStaging: if (events.clear_staging) events.clear_staging(); break;
        case app::GlobalSearchCommand::ContinueSearch:
            if (handoff) {
                auto request = MakeHandoff(); request.query = app::NormalizeGlobalSearchQuery(last_search_query);
                const auto handler = handoff; Hide(); handler(request);
            }
            return;
        case app::GlobalSearchCommand::ClearHistory:
            if (events.clear_search_history) events.clear_search_history();
            context.recent_queries.clear(); break;
        }
        SetWindowTextW(edit, L"");
    }
    void CopyText(const std::wstring& path) {
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard();
        const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* data = GlobalLock(memory)) {
                memcpy(data, path.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
            } else GlobalFree(memory);
        }
        CloseClipboard();
        ShowToast(Text(StringId::CopiedShort));
    }
    void ShowToast(std::wstring text) {
        toast = std::move(text);
        toast_until = ui::motion::NowMs() + 1500;
        Invalidate();
    }
    void SetScope(int index) {
        if (index < 0 || index >= static_cast<int>(scopes.size())) return;
        browse_folder.clear(); browse_history.clear();
        scope = scope == index && index != 0 ? 0 : index;
        Changed();
    }
    void SetMode(bool content) {
        if (content_mode == content) return;
        content_mode = content;
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(content_mode ? StringId::SearchContentHint : StringId::SearchNameHint).c_str()));
        Changed();
    }
    void SearchScopeMenu() {
        if (!EnsureTarget() || !result_menu.Create(hwnd, &compositor, scale)) return;
        result_menu.SetTheme(dark, accent_color);
        std::vector<ui::FluentMenuItem> items;
        for (size_t i = 0; i < scopes.size(); ++i) {
            ui::FluentMenuItem item;
            item.command = static_cast<int>(i) + 1; item.text = scopes[i].label; item.glyph = scopes[i].glyph;
            item.checked = browse_folder.empty() && scope == static_cast<int>(i);
            item.radio = item.checked; item.radio_group = true; items.push_back(std::move(item));
        }
        const auto bounds = SearchFieldLayout().scope;
        POINT point{Px(bounds.left) - ui::FluentMenu::kShadowMargin, Px(bounds.bottom + 4) - ui::FluentMenu::kShadowMargin};
        ClientToScreen(hwnd, &point);
        result_menu_open = true;
        const int chosen = result_menu.TrackPopup(point, std::move(items));
        result_menu_open = false;
        if (!IsWindowVisible(hwnd)) return;
        if (chosen > 0 && chosen <= static_cast<int>(scopes.size())) {
            browse_folder.clear(); browse_history.clear(); scope = chosen - 1; Changed();
        }
        SetFocus(edit);
    }
    // ---- sort / group menus ------------------------------------------------
    void ApplyContextOrder() {
        sort_order = app::GlobalSearchSortFromInt(context.sort);
        sort_desc = context.sort_desc;
        folder_order = app::GlobalSearchFoldersFromInt(context.folders);
        const auto group = app::GlobalSearchGroupFromInt(context.group);
        if (group != group_by) collapsed.clear();
        group_by = group;
    }
    void SaveOrder() {
        context.sort = static_cast<int>(sort_order); context.sort_desc = sort_desc;
        context.folders = static_cast<int>(folder_order); context.group = static_cast<int>(group_by);
        if (events.order_changed) events.order_changed(context.sort, sort_desc, context.folders, context.group, context.apps_first);
    }
    // New order: the index sorts name, date and size itself, so changing to
    // or between those searches again; the rest reorders what is shown.
    void ApplyOrder(app::GlobalSearchSort old_sort, bool old_desc) {
        SaveOrder();
        const bool research = !content_mode && arranged && HasSearchQuery() &&
            (IndexSorted(old_sort) || IndexSorted(sort_order)) && (old_sort != sort_order || old_desc != sort_desc);
        if (research) { order_reset = true; selected = 0; scroll = scroll_target = 0; Changed(); }
        else Rearrange(true);
        Invalidate();
    }
    std::wstring DirectionLabel(app::GlobalSearchSort sort, bool descending) const {
        switch (sort) {
        case app::GlobalSearchSort::Modified: return Text(descending ? StringId::SortNewOld : StringId::SortOldNew);
        case app::GlobalSearchSort::Size: return Text(descending ? StringId::SortLargeSmall : StringId::SortSmallLarge);
        default: return Text(descending ? StringId::SortDescending : StringId::SortAscending);
        }
    }
    static StringId SortLabel(app::GlobalSearchSort sort) {
        constexpr StringId labels[] = {StringId::GlobalSearchSortRelevance, StringId::ColumnName, StringId::ColumnModified,
            StringId::ColumnSize, StringId::ColumnType, StringId::ColumnPath};
        return labels[static_cast<int>(sort)];
    }
    StringId GroupLabel(app::GlobalSearchGroup group) const {
        constexpr StringId labels[] = {StringId::GlobalSearchGroupMatch, StringId::ColumnType, StringId::ColumnModified,
            StringId::GlobalSearchGroupFolder, StringId::ColumnSize, StringId::GroupNone};
        if (group == app::GlobalSearchGroup::Match && content_mode) return StringId::GlobalSearchContentMatches;
        return labels[static_cast<int>(group)];
    }
    POINT MenuPoint(D2D1_RECT_F anchor) const {
        POINT point{Px(anchor.left) - ui::FluentMenu::kShadowMargin, Px(anchor.bottom + 4) - ui::FluentMenu::kShadowMargin};
        ClientToScreen(hwnd, &point);
        return point;
    }
    void SortMenu() {
        if (command_mode || !EnsureTarget() || !result_menu.Create(hwnd, &compositor, scale)) return;
        result_menu.SetTheme(dark, accent_color);
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem heading;
        heading.heading = true; heading.text = Text(StringId::SortBy); heading.shortcut = L"Alt+S";
        items.push_back(std::move(heading));
        constexpr StringId details[] = {StringId::GlobalSearchSortRelevanceDesc, StringId::SortHintName, StringId::SortHintModified,
            StringId::SortHintSize, StringId::SortHintType, StringId::GlobalSearchSortPathDesc};
        for (int i = 0; i < app::kGlobalSearchSortCount; ++i) {
            const auto sort = static_cast<app::GlobalSearchSort>(i);
            ui::FluentMenuItem item;
            item.command = i + 1; item.text = Text(SortLabel(sort)); item.detail = Text(details[i]);
            item.radio_group = true; item.radio = item.checked = sort == sort_order;
            if (item.checked && sort != app::GlobalSearchSort::Relevance) item.shortcut = DirectionLabel(sort, sort_desc);
            items.push_back(std::move(item));
        }
        items.back().separator_after = true;
        const bool ordered = sort_order != app::GlobalSearchSort::Relevance;
        ui::FluentMenuItem direction;
        direction.text = Text(StringId::SortWheelDirection); direction.enabled = ordered;
        direction.shortcut = ordered ? DirectionLabel(sort_order, sort_desc) : std::wstring{};
        for (int i = 0; i < 2; ++i) {
            ui::FluentMenuItem item;
            item.command = 11 + i; item.text = DirectionLabel(sort_order, i == 1);
            item.radio_group = true; item.radio = item.checked = sort_desc == (i == 1);
            direction.children.push_back(std::move(item));
        }
        items.push_back(std::move(direction));
        ui::FluentMenuItem folders;
        constexpr StringId folder_labels[] = {StringId::SortWheelFolderTop, StringId::SortWheelFolderMixed, StringId::SortWheelFolderFollow};
        folders.text = Text(StringId::SortWheelFolders); folders.enabled = ordered;
        folders.shortcut = ordered ? Text(folder_labels[static_cast<int>(folder_order)]) : std::wstring{};
        for (int i = 0; i < app::kGlobalSearchFoldersCount; ++i) {
            ui::FluentMenuItem item;
            item.command = 21 + i; item.text = Text(folder_labels[i]);
            item.radio_group = true; item.radio = item.checked = static_cast<int>(folder_order) == i;
            folders.children.push_back(std::move(item));
        }
        items.push_back(std::move(folders));
        result_menu_open = true;
        const int chosen = result_menu.TrackPopup(MenuPoint(sort_button), std::move(items));
        result_menu_open = false;
        if (!IsWindowVisible(hwnd)) return;
        SetFocus(edit);
        const auto old_sort = sort_order; const bool old_desc = sort_desc;
        if (chosen >= 1 && chosen <= app::kGlobalSearchSortCount) {
            const auto sort = static_cast<app::GlobalSearchSort>(chosen - 1);
            // Choosing the current order again turns it around.
            if (sort == sort_order) { if (sort != app::GlobalSearchSort::Relevance) sort_desc = !sort_desc; }
            else { sort_order = sort; sort_desc = app::GlobalSearchSortStartsDescending(sort); }
        } else if (chosen == 11 || chosen == 12) {
            sort_desc = chosen == 12;
        } else if (chosen >= 21 && chosen < 21 + app::kGlobalSearchFoldersCount) {
            folder_order = static_cast<app::GlobalSearchFolders>(chosen - 21);
            SaveOrder(); Rearrange(true); return;
        } else return;
        ApplyOrder(old_sort, old_desc);
    }
    bool FoldableGroups() const {
        return std::count_if(groups.begin(), groups.end(), [](const Group& g) { return g.foldable; }) > 1;
    }
    void GroupMenu() {
        if (command_mode || !EnsureTarget() || !result_menu.Create(hwnd, &compositor, scale)) return;
        result_menu.SetTheme(dark, accent_color);
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem heading;
        heading.heading = true; heading.text = Text(StringId::GroupBy); heading.shortcut = L"Alt+G";
        items.push_back(std::move(heading));
        constexpr StringId details[] = {StringId::GlobalSearchGroupMatchDesc, StringId::GlobalSearchGroupTypeDesc,
            StringId::GlobalSearchGroupDateDesc, StringId::GlobalSearchGroupFolderDesc, StringId::GlobalSearchGroupSizeDesc,
            StringId::GlobalSearchGroupNoneDesc};
        for (int i = 0; i < app::kGlobalSearchGroupCount; ++i) {
            const auto group = static_cast<app::GlobalSearchGroup>(i);
            ui::FluentMenuItem item;
            item.command = 31 + i; item.text = Text(GroupLabel(group));
            item.detail = group == app::GlobalSearchGroup::Match && content_mode ? Text(StringId::GlobalSearchGroupNoneDesc) : Text(details[i]);
            item.radio_group = true; item.radio = item.checked = group == group_by;
            items.push_back(std::move(item));
        }
        if (context.show_apps && !content_mode) {
            items.back().separator_after = true;
            ui::FluentMenuItem apps_first;
            apps_first.command = 41; apps_first.text = Text(StringId::GlobalSearchAppsFirst);
            apps_first.toggle = true; apps_first.checked = context.apps_first;
            items.push_back(std::move(apps_first));
        }
        if (FoldableGroups()) {
            items.back().separator_after = true;
            const bool all_closed = std::all_of(groups.begin(), groups.end(), [&](const Group& g) { return !g.foldable || Collapsed(g.key); });
            ui::FluentMenuItem fold;
            fold.command = 40; fold.text = Text(all_closed ? StringId::GlobalSearchExpandAll : StringId::GlobalSearchCollapseAll);
            fold.shortcut = all_closed ? L"Ctrl+\x2192" : L"Ctrl+\x2190";
            items.push_back(std::move(fold));
        }
        result_menu_open = true;
        const int chosen = result_menu.TrackPopup(MenuPoint(group_button), std::move(items));
        result_menu_open = false;
        if (!IsWindowVisible(hwnd)) return;
        SetFocus(edit);
        if (chosen >= 31 && chosen < 31 + app::kGlobalSearchGroupCount) {
            const auto group = static_cast<app::GlobalSearchGroup>(chosen - 31);
            if (group == group_by) return;
            group_by = group; collapsed.clear(); scroll_target = 0;
            SaveOrder(); Rearrange(true);
        } else if (chosen == 40) {
            const bool all_closed = std::all_of(groups.begin(), groups.end(), [&](const Group& g) { return !g.foldable || Collapsed(g.key); });
            collapsed.clear();
            if (!all_closed) for (const auto& g : groups) if (g.foldable) collapsed.push_back(g.key);
            Rearrange(true);
        } else if (chosen == 41) {
            context.apps_first = !context.apps_first;
            SaveOrder(); Rearrange(true);
        }
    }
    int GroupOf(int row) const {
        for (size_t g = 0; g < groups.size(); ++g)
            if (row >= static_cast<int>(groups[g].start) && row < static_cast<int>(groups[g].start + groups[g].count)) return static_cast<int>(g);
        return -1;
    }
    void ToggleGroup(size_t index) {
        if (index >= groups.size() || !groups[index].foldable) return;
        const auto key = groups[index].key;
        if (Collapsed(key)) collapsed.erase(std::find(collapsed.begin(), collapsed.end(), key));
        else collapsed.push_back(key);
        Rearrange();
    }
    void MarkGroup(size_t index) {
        if (index >= groups.size()) return;
        const auto& group = groups[index];
        if (group.count == 0) ToggleGroup(index);
        const auto& shown = groups[index];
        if (shown.count == 0) return;
        MarkRange(static_cast<int>(shown.start), static_cast<int>(shown.start + shown.count) - 1);
        selected = static_cast<int>(shown.start); keyboard_nav = true;
        ClampSelection(); Invalidate();
    }
    // Ctrl+↑/↓: first row of the previous / next group with rows.
    void JumpGroup(bool down) {
        if (rows.empty()) return;
        if (calculation && !keyboard_nav) {
            selected = down ? 0 : static_cast<int>(rows.size()) - 1;
            keyboard_nav = true; ClampSelection(); Invalidate(); return;
        }
        const int current = std::max(0, GroupOf(std::max(0, selected)));
        int next_group = -1;
        if (down) {
            for (size_t g = static_cast<size_t>(current) + 1; g < groups.size(); ++g) if (groups[g].count) { next_group = static_cast<int>(g); break; }
        } else {
            const bool at_start = selected == static_cast<int>(groups[static_cast<size_t>(current)].start);
            for (int g = at_start ? current - 1 : current; g >= 0; --g) if (groups[static_cast<size_t>(g)].count) { next_group = g; break; }
        }
        if (next_group < 0) return;
        selected = static_cast<int>(groups[static_cast<size_t>(next_group)].start); keyboard_nav = true;
        ClampSelection(); Invalidate();
    }
    void TogglePreview() {
        preview_open = !preview_open;
        BuildLayout();
        SyncPreview();
        ClampSelection();
        Invalidate();
    }
    void SearchDirectory() {
        auto job = std::make_shared<DirectoryRequest>();
        directory_request = job;
        const auto folder = browse_folder;
        const auto compiled = index::ParseQuery(query + FilterTokens());
        rows.clear(); showing_recent = stale = false; busy = true; error.clear();
        best_count = folder_count = jump_count = app_count = 0;
        arranged = false; merged_rows.clear();
        selected = -1; scroll = scroll_target = 0;
        BuildLayout(); Invalidate();
        std::thread([job, folder, compiled, failure_label = Text(StringId::GlobalSearchFailed), notify = hwnd, id = generation] {
            std::vector<fs::DirEntry> entries;
            std::vector<Row> result;
            std::wstring failure;
            try {
                fs::EnumerationOptions options;
                options.cancelled = [job] { return job->cancelled.load(); };
                fs::EnumerateDirectory(folder, entries, options);
                for (const auto& entry : entries) {
                    if (job->cancelled) return;
                    const std::wstring full = folder + (folder.back() == L'\\' ? L"" : L"\\") + entry.name;
                    const uint64_t modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) | entry.mtime.dwLowDateTime;
                    bool matches = compiled.groups.empty();
                    for (const auto& group : compiled.groups) {
                        if (std::all_of(group.begin(), group.end(), [&](const index::Term& term) {
                            return index::MatchTerm(full, entry.name, entry.is_dir, entry.size, modified, term);
                        })) { matches = true; break; }
                    }
                    if (matches) result.push_back({entry.name, full, L"", entry.is_dir, entry.size, modified, IsNetworkPath(full)});
                }
                std::stable_sort(result.begin(), result.end(), [](const Row& a, const Row& b) {
                    return a.directory != b.directory ? a.directory : StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
                });
            } catch (...) { failure = failure_label; }
            if (job->cancelled) return;
            { std::lock_guard lock(job->mutex); job->rows = std::move(result); job->error = std::move(failure); }
            if (!job->cancelled) PostMessageW(notify, kDirectoryResult, id, 0);
        }).detach();
    }
    void EnterFolder() {
        if (stale || selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto row = rows[static_cast<size_t>(selected)];
        if (!row.directory || !row.recycle_path.empty()) return;
        browse_history.push_back(browse_folder);
        browse_folder = row.path;
        SetWindowTextW(edit, L"");
    }
    void BackFolder() {
        if (browse_folder.empty()) return;
        browse_folder = browse_history.empty() ? ParentPath(browse_folder) : browse_history.back();
        if (!browse_history.empty()) browse_history.pop_back();
        Changed();
    }
    void PickPlace() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
        DWORD options = 0; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(Text(StringId::GlobalSearchPickPlace).c_str());
        if (FAILED(dialog->Show(hwnd))) return;
        ComPtr<IShellItem> item; PWSTR raw = nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return;
        std::wstring path = raw; CoTaskMemFree(raw);
        if (events.pin_folder) events.pin_folder(path);
        auto& pinned = context.pinned_folders;
        if (std::find(pinned.begin(), pinned.end(), path) == pinned.end()) pinned.insert(pinned.begin(), path);
        BuildScopes();
        for (size_t i = 0; i < scopes.size(); ++i) if (scopes[i].path == path) scope = static_cast<int>(i);
        if (IsWindowVisible(hwnd)) { SetForegroundWindow(hwnd); SetFocus(edit); }
        Changed();
    }
    void BuildScopes() {
        const Scope selected = scope > 0 && scope < static_cast<int>(scopes.size())
            ? scopes[static_cast<size_t>(scope)] : Scope{};
        scopes.clear();
        scopes.push_back({ScopeKind::All, Text(StringId::GlobalSearchEverywhere), L"", L"\xE774"});
        if (!current_folder.empty()) scopes.push_back({ScopeKind::Current, Text(StringId::GlobalSearchCurrentFolder), current_folder, L"\xE8B7"});
        const struct { REFKNOWNFOLDERID id; StringId label; const wchar_t* glyph; } known[] = {
            {FOLDERID_Desktop, StringId::Desktop, L"\xE7F4"}, {FOLDERID_Documents, StringId::PickerDocuments, L"\xE8A5"},
            {FOLDERID_Downloads, StringId::Downloads, L"\xE896"}};
        std::vector<std::wstring> taken;
        for (const auto& folder : known) {
            auto path = known_folders::CurrentPath(folder.id);
            if (path.empty()) continue;
            taken.push_back(path);
            scopes.push_back({ScopeKind::Known, Text(folder.label), std::move(path), folder.glyph});
            scopes.back().known_id = &folder.id;
        }
        size_t pinned = 0;
        for (const auto& path : context.pinned_folders) {
            if (pinned >= kMaximumPinnedScopes) break;
            if (std::any_of(taken.begin(), taken.end(), [&](const std::wstring& t) { return path::EqualInsensitive(t, path); })) continue;
            taken.push_back(path); ++pinned;
            scopes.push_back({ScopeKind::Pinned, LeafName(path), path, L"\xE718"});
        }
        size_t shares = 0;
        for (const auto& root : network.Roots()) {
            if (shares >= kMaximumNetworkScopes) break;
            ++shares;
            scopes.push_back({ScopeKind::Network, LeafName(root.path), root.path, L"\xE8CE"});
        }
        scope = 0;
        if (selected.kind == ScopeKind::Current && !current_folder.empty()) scope = 1;
        else if (!selected.path.empty()) {
            for (size_t i = 1; i < scopes.size(); ++i) {
                if (selected.known_id ? scopes[i].known_id == selected.known_id :
                    scopes[i].kind == selected.kind && path::EqualInsensitive(scopes[i].path, selected.path)) {
                    scope = static_cast<int>(i);
                    break;
                }
            }
        }
        counts_key.clear();
    }
    bool Key(WPARAM key) {
        if (composing) return false;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (ctrl && key == 'A' && keyboard_nav && !command_mode) { MarkRange(0, static_cast<int>(rows.size()) - 1); swallow_char = 1; Invalidate(); return true; }
        if (ctrl && key == 'K') { swallow_char = 11; POINT point{Px(ListLeft() + 90), Px(ListTop() + 45)}; ClientToScreen(hwnd, &point); ResultMenu(point); return true; }
        if (alt && key == 'A') { StageSelection(); return true; }
        if (alt && (key == '1' || key == '2')) { grid_view = key == '2'; BuildLayout(); Invalidate(); return true; }
        if (alt && key == 'S') { SortMenu(); return true; }
        if (alt && key == 'G') { GroupMenu(); return true; }
        if (ctrl && (key == VK_UP || key == VK_DOWN) && !command_mode) { JumpGroup(key == VK_DOWN); return true; }
        if (ctrl && keyboard_nav && (key == VK_LEFT || key == VK_RIGHT) && arranged && FoldableGroups()) {
            collapsed.clear();
            if (key == VK_LEFT) for (const auto& g : groups) if (g.foldable) collapsed.push_back(g.key);
            Rearrange(true); return true;
        }
        if (ctrl && key == VK_SPACE) { ToggleMarked(); swallow_char = L' '; return true; }
        if (key == VK_RETURN && calculation && !keyboard_nav && !shift) { CopyCalculation(ctrl); return true; }
        if (key == VK_ESCAPE && tray_open) { tray_open = false; Invalidate(); return true; }
        if (key == VK_RIGHT && keyboard_nav && !IsGrid()) { EnterFolder(); return true; }
        if (IsGrid() && keyboard_nav && (key == VK_LEFT || key == VK_RIGHT)) {
            const int previous = selected;
            selected += key == VK_RIGHT ? 1 : -1; ClampSelection();
            if (shift) MarkRange(previous, selected);
            Invalidate(); return true;
        }
        if (key == VK_BACK && raw_query.empty() && !browse_folder.empty()) { BackFolder(); return true; }
        if (key == VK_ESCAPE) { if (preview_open) TogglePreview(); else Hide(); return true; }
        if (key == VK_RETURN) {
            if (shift) Handoff();
            else Open(ctrl);
            return true;
        }
        if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
            if (rows.empty()) return true;
            const int previous = selected;
            if (selected < 0 || (calculation && !keyboard_nav)) selected = key == VK_UP || key == VK_PRIOR ? static_cast<int>(rows.size()) - 1 : 0;
            else selected = std::max(0, selected + (key == VK_UP ? -(IsGrid() ? GridColumns() : 1) : key == VK_DOWN ? (IsGrid() ? GridColumns() : 1) : key == VK_PRIOR ? -PageSize() : PageSize()));
            keyboard_nav = true;
            ClampSelection();
            if (shift) MarkRange(previous, selected);
            Invalidate(); return true;
        }
        if (key == VK_TAB) { SetMode(!content_mode); return true; }
        if (key == VK_SPACE && (keyboard_nav || preview_open) && !ctrl && !rows.empty()) {
            swallow_char = L' '; TogglePreview(); return true;
        }
        if (ctrl && key == 'C' && edit) {
            DWORD start = 0, end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            if (start == end && selected >= 0 && (!calculation || keyboard_nav)) { swallow_char = 3; CopyPath(); return true; }
        }
        if (ctrl && key >= '1' && key <= '9') {
            const int index = static_cast<int>(key - '1');
            if (index < static_cast<int>(scopes.size())) { scope = index; Changed(); }
            return true;
        }
        return false;
    }
    static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<Impl*>(data);
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) self->Invalidate();
        if (message == WM_LBUTTONDOWN && self->calculation) {
            self->keyboard_nav = false;
            self->Invalidate();
        }
        if (message == WM_IME_STARTCOMPOSITION) { self->composing = true; self->Cancel(); }
        if (message == WM_IME_ENDCOMPOSITION) {
            self->composing = false;
            const auto result = self->DefaultEdit(window, message, wp, lp);
            self->Changed(); return result;
        }
        if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && self->Key(wp)) return 0;
        if (message == WM_SYSCHAR && (wp == L'a' || wp == L'A' || wp == L'1' || wp == L'2' || wp == L's' || wp == L'S' ||
            wp == L'g' || wp == L'G')) return 0;
        if (message == WM_CHAR && self->swallow_char) {
            const bool same = static_cast<wchar_t>(wp) == self->swallow_char;
            self->swallow_char = 0;
            if (same) return 0;
        }
        if (message == WM_CHAR && !self->composing && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
        LRESULT result = 0;
        if (ui::HandleChildEditMessage(self->compositor, self->edit_format.Get(), self->Theme().text,
            self->EditBackground(), self->background, window, message, wp, lp, result)) return result;
        return self->DefaultEdit(window, message, wp, lp);
    }
    LRESULT DefaultEdit(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        return ui::DefPresentedChildEditProc(compositor, edit_format.Get(), Theme().text,
            EditBackground(), window, message, wp, lp);
    }
    // The same active search field layout used beside the main address bar.
    D2D1_RECT_F FieldRect() const { return {kFieldLeft, kFieldTop, width - kFieldRight, kFieldTop + kFieldHeight + grow}; }
    ui::AddressSearchLayout SearchFieldLayout() const { return ui::LayoutAddressSearchControl(FieldRect(), 1, false); }
    D2D1_RECT_F ModeRect() const { return SearchFieldLayout().mode; }
    void Layout() {
        RECT rect{}; GetClientRect(hwnd, &rect);
        width = static_cast<float>(rect.right) / scale; height = static_cast<float>(rect.bottom) / scale;
        if (target) {
            if (target.Get() == compositor.Dc() &&
                (compositor.Width() != rect.right || compositor.Height() != rect.bottom))
                compositor.Resize(rect.right, rect.bottom);
            target->SetDpi(96 * scale, 96 * scale);
            ComPtr<ID2D1HwndRenderTarget> window_target;
            if (SUCCEEDED(target.As(&window_target))) window_target->Resize(D2D1::SizeU(static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)));
        }
        // Use the address-search hosted edit's UI family and 14-DIP sizing.
        if (HFONT font = ui::typography::CreateEditFont(scale)) {
            if (edit_font) DeleteObject(edit_font);
            edit_font = font;
        }
        if (EnsureTarget()) {
            edit_format.Reset();
            write_factory->CreateTextFormat(ui::typography::PreferredTextFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                static_cast<float>(ui::typography::EditFontPixels(scale)), l10n::LocaleName(), &edit_format);
        }
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
        // Use the main address-search editor's typography for input and hint.
        SetPropW(edit, L"Pulse.EditCueSize",
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(ui::typography::EditFontPixels(scale))));
        // One text line plus 4 DIP inside the 44 DIP field; taller fonts grow the header.
        const float line_dip = static_cast<float>(ui::EditLineHeight(edit, edit_font, 1 << 16)) / scale;
        grow = std::max(0.0f, std::ceil(line_dip + 14.0f - kFieldHeight));
        const float field_h = std::ceil(line_dip + 4.0f);
        const auto field = FieldRect();
        const auto input = SearchFieldLayout().input;
        const float edit_left = input.left;
        const float edit_right = input.right;
        const float edit_width = std::max(40.0f, edit_right - edit_left);
        const float edit_top = (field.top + field.bottom - field_h) / 2;
        MoveWindow(edit, Px(edit_left), Px(edit_top), Px(edit_width), Px(field_h), TRUE);
        SetWindowRgn(edit, nullptr, TRUE);
        if (edit_format)
            ui::PresentChildEdit(compositor, edit_format.Get(), Theme().text, EditBackground(), edit);
        BuildLayout();
        ClampSelection(false); Invalidate();
    }
    bool EnsureTarget() {
        if (target) return true;
        if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) return false;
        if (!write_factory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_factory.GetAddressOf())))) return false;
        RECT rect{}; GetClientRect(hwnd, &rect);
        if (compositor.NeedsRecovery()) {
            if (!compositor.Recover()) return false;
            material.Invalidate();
        } else if (!compositor.Dc() && !compositor.Init(hwnd)) return false;
        material.SetCompositor(&compositor);
        target = compositor.Dc();
        target->SetDpi(96 * scale, 96 * scale);
        painter.SetCompositor(&compositor);
        // This surface uses logical DIPs through its render-target DPI.
        painter.SetScale(1.0f);
        icons.SetDeviceContext(compositor.Dc());
        icons.SetScale(scale);
        icons.SetNotifyWindow(hwnd);
        return SUCCEEDED(target->CreateSolidColorBrush(Color(0xffffff), &brush));
    }
    void Fill(D2D1_RECT_F rect, UINT32 color, float radius = 0) { FillC(rect, Color(color), radius); }
    void FillC(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 0) {
        brush->SetColor(color);
        if (radius) target->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
        else target->FillRectangle(rect, brush.Get());
    }
    void StrokeC(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius, float stroke = 1) {
        brush->SetColor(color);
        const D2D1_RECT_F inner{rect.left + stroke / 2, rect.top + stroke / 2, rect.right - stroke / 2, rect.bottom - stroke / 2};
        target->DrawRoundedRectangle(D2D1::RoundedRect(inner, radius, radius), brush.Get(), stroke);
    }
    void CommandIcon(std::wstring_view glyph, D2D1_RECT_F rect, float size, D2D1_COLOR_F color) {
        brush->SetColor(color);
        ui::command_icons::DrawGlyph(target.Get(), brush.Get(), glyph, rect, size);
    }
    void Label(const std::wstring& text, D2D1_RECT_F rect, float size, UINT32 color, bool bold = false, const wchar_t* face = L"Segoe UI", bool highlight = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        LabelC(text, rect, size, Color(color), bold, face, highlight, alignment);
    }
    std::wstring name_highlight_query;
    bool name_highlight_pinyin = true;
    std::vector<index::Term> name_highlight_terms;
    std::vector<ui::NameMatchRange> FilenameHighlightRanges(std::wstring_view name) {
        if (name_highlight_query != query || name_highlight_pinyin != search_pinyin) {
            name_highlight_query = query;
            name_highlight_pinyin = search_pinyin;
            name_highlight_terms = ui::NameHighlightTerms({}, search_pinyin ? query : L"nopinyin: " + query);
        }
        return ui::NameMatchRanges(name, name_highlight_terms);
    }
    void LabelC(const std::wstring& text, D2D1_RECT_F rect, float size, D2D1_COLOR_F color, bool bold = false, const wchar_t* face = L"Segoe UI", bool highlight = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false) {
        if (text.empty() || rect.right <= rect.left) return;
        ComPtr<IDWriteTextFormat> format;
        // Glyph icons keep their slots; text follows Settings > Interface font size.
        const bool icon_face = std::wstring_view(face).find(L"Icons") != std::wstring_view::npos;
        const float text_size = icon_face ? size : size * ui::typography::UiFontScale();
        if (FAILED(write_factory->CreateTextFormat(face, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, text_size, l10n::LocaleName(), &format))) return;
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(write_factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis))) format->SetTrimming(&trim, ellipsis.Get());
        brush->SetColor(color);
        ComPtr<IDWriteTextLayout> layout;
        if (highlight && !query.empty() && SUCCEEDED(write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(),
            std::max(0.0f, rect.right - rect.left), std::max(0.0f, rect.bottom - rect.top), &layout))) {
            ComPtr<ID2D1SolidColorBrush> accent;
            if (!content_mode) {
                const auto matches = FilenameHighlightRanges(text);
                if (ui::IsHighContrast()) {
                    for (const auto& match : matches) {
                        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {match.start, match.length});
                        layout->SetUnderline(TRUE, {match.start, match.length});
                    }
                } else if (target.Get() == compositor.Dc()) {
                    // This surface uses DIPs; the target applies the monitor DPI.
                    ui::DrawNameHighlightBackground(&compositor, layout.Get(), {rect.left, rect.top},
                        rect, matches, Theme(), 1.0f);
                } else if (SUCCEEDED(target->CreateSolidColorBrush(MarkColor(), &accent))) {
                    for (const auto& match : matches)
                        layout->SetDrawingEffect(accent.Get(), {match.start, match.length});
                }
            } else if (SUCCEEDED(target->CreateSolidColorBrush(MarkColor(), &accent))) {
                // Every word of the query is marked where it occurs.
                size_t begin = 0;
                while ((begin = query.find_first_not_of(L" \t", begin)) != std::wstring::npos) {
                    const size_t end = std::min(query.find_first_of(L" \t", begin), query.size());
                    const std::wstring word = query.substr(begin, end - begin);
                    begin = end;
                    if (word.find(L':') != std::wstring::npos) continue;
                    for (size_t start = 0; start + word.size() <= text.size(); ++start) {
                        if (CompareStringOrdinal(text.data() + start, static_cast<int>(word.size()), word.data(), static_cast<int>(word.size()), TRUE) == CSTR_EQUAL) {
                            if (ui::IsHighContrast())
                                layout->SetUnderline(TRUE, {static_cast<UINT32>(start), static_cast<UINT32>(word.size())});
                            else
                                layout->SetDrawingEffect(accent.Get(), {static_cast<UINT32>(start), static_cast<UINT32>(word.size())});
                            layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, {static_cast<UINT32>(start), static_cast<UINT32>(word.size())});
                        }
                    }
                }
            }
            if (!icon_face && target.Get() == compositor.Dc())
                compositor.DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            else
                target->DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else if (!icon_face && target.Get() == compositor.Dc() && SUCCEEDED(write_factory->CreateTextLayout(text.data(),
            static_cast<UINT32>(text.size()), format.Get(), std::max(0.0f, rect.right - rect.left),
            std::max(0.0f, rect.bottom - rect.top), &layout))) {
            // Same LumaText glyphs as the main window; icon glyphs stay DirectWrite.
            compositor.DrawTextLayout({rect.left, rect.top}, layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format.Get(), rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    D2D1_COLOR_F MarkColor() const {
        const auto accent = Theme().accent;
        return dark ? Mix(accent, D2D1::ColorF(1, 1, 1, 1), 0.42f) : Mix(accent, D2D1::ColorF(0, 0, 0, 1), 0.18f);
    }
    void DrawLogo(D2D1_RECT_F rect) {
        if (logo_target != target.Get()) { logo.Reset(); logo_target = target.Get(); }
        if (!logo) {
            const auto module = GetModuleHandleW(nullptr);
            const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_GLOBAL_SEARCH_LOGO), RT_RCDATA);
            const auto data = resource ? LoadResource(module, resource) : nullptr;
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame_decode;
            ComPtr<IWICFormatConverter> converter;
            if (data && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)), SizeofResource(module, resource))) &&
                SUCCEEDED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame_decode)) && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame_decode.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
                target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &logo);
        }
        if (logo) target->DrawBitmap(logo.Get(), rect);
    }
    float TextWidth(const std::wstring& text, float size = 12, bool bold = false) {
        if (!write_factory) return 0;
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(write_factory->CreateTextFormat(L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * ui::typography::UiFontScale(),
            l10n::LocaleName(), &format)) &&
            SUCCEEDED(write_factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format.Get(), 4000, 40, &layout)))
            layout->GetMetrics(&metrics);
        return std::ceil(metrics.widthIncludingTrailingWhitespace);
    }
    // Keeps both ends of a long path readable: "C:\Users\…\2026\合同".
    std::wstring MiddleEllipsis(const std::wstring& text, float available, float size) {
        if (available <= 0 || TextWidth(text, size) <= available) return text;
        size_t keep = text.size();
        std::wstring out;
        while (keep > 6) {
            keep = keep * 4 / 5;
            const size_t head = keep * 2 / 5, tail = keep - head;
            out = text.substr(0, head) + L"\x2026" + text.substr(text.size() - tail);
            if (TextWidth(out, size) <= available) return out;
        }
        return out.empty() ? text : out;
    }
    float Keycap(const std::wstring& text, float right, float mid, D2D1_COLOR_F color, D2D1_COLOR_F fill) {
        const float w = std::max(22.0f, TextWidth(text, 11) + 12);
        const D2D1_RECT_F rect{right - w, mid - 10, right, mid + 10};
        FillC(rect, fill, 4);
        const bool symbols = std::any_of(text.begin(), text.end(), [](wchar_t c) { return c >= 0x2190 && c <= 0x21ff; });
        LabelC(text, rect, symbols ? 11.5f : 11, color, false, symbols ? L"Segoe UI Symbol" : L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        return rect.left;
    }
    // Draws "label [key]" pairs right-to-left; returns the left edge.
    float KeyHint(const std::wstring& key, const std::wstring& label, float right, float mid, D2D1_COLOR_F text, D2D1_COLOR_F fill) {
        if (!label.empty()) {
            const float lw = TextWidth(label, 12);
            LabelC(label, {right - lw, mid - 10, right, mid + 10}, 12, text);
            right -= lw + 6;
        }
        return Keycap(key, right, mid, text, fill);
    }
    std::wstring FormatTime(uint64_t filetime) const {
        if (!filetime) return {};
        FILETIME utc{static_cast<DWORD>(filetime), static_cast<DWORD>(filetime >> 32)};
        SYSTEMTIME system{}, local{}, now{};
        if (!FileTimeToSystemTime(&utc, &system) || !SystemTimeToTzSpecificLocalTime(nullptr, &system, &local)) return {};
        GetLocalTime(&now);
        auto day_number = [](const SYSTEMTIME& t) {
            FILETIME f{}; SYSTEMTIME d = t; d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
            SystemTimeToFileTime(&d, &f);
            return ((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 864000000000ull;
        };
        const auto days = static_cast<int64_t>(day_number(now)) - static_cast<int64_t>(day_number(local));
        wchar_t buffer[64]{};
        if (days == 0 || days == 1) {
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, buffer, 64);
            return Text(days == 0 ? StringId::ChangeToday : StringId::ChangeYesterday) + L" " + buffer;
        }
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, buffer, 64, nullptr);
        return buffer;
    }
    static std::wstring FormatSize(uint64_t bytes) {
        wchar_t buffer[32]{};
        if (FAILED(StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, buffer, 32))) return {};
        return buffer;
    }
    std::wstring TypeName(const Row& row) const {
        if (row.app) return Text(StringId::TypeApplication);
        SHFILEINFOW info{};
        if (SHGetFileInfoW(row.name.c_str(), row.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
            SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES)) return info.szTypeName;
        return {};
    }
    void ClearPreviewMessages() {
        if (!hwnd) return;
        MSG pending{};
        while (PeekMessageW(&pending, hwnd, kPreviewNavigate, kPreviewOpen, PM_REMOVE)) {}
        preview.TakeOpenPath();
    }
    void ClosePreview() {
        ClearPreviewMessages();
        preview.Close();
        preview_occluded = false;
        preview_requested = {};
    }
    D2D1_RECT_F PreviewContentBounds() const {
        const float top = Header() + 1;
        return {width - PreviewWidth() + 16, top + 16, width - 16,
            top + 16 + std::max(ListBottom() - top - 280, 70.0f)};
    }
    void SyncPreview() {
        if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || PreviewWidth() <= 0 ||
            command_mode || stale || selected < 0 || selected >= static_cast<int>(rows.size())) {
            ClosePreview();
            return;
        }
        const auto& row = rows[static_cast<size_t>(selected)];
        if (row.app || row.path.empty()) { ClosePreview(); return; }
        const auto box = PreviewContentBounds();
        RECT bounds{Px(box.left), Px(box.top), Px(box.right), Px(box.bottom)};
        if (box.bottom > ListBottom() - 94 || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
            ClosePreview(); return;
        }
        // No file-command channel: the search panel only exposes its existing open/path actions.
        if (!preview.InitializeEmbedded(hwnd, kPreviewNavigate, kPreviewOpen, 0)) return;
        ui::QuickPreviewItem item;
        item.path = row.recycle_path.empty() ? row.path : row.recycle_path;
        item.name = row.name;
        item.attrs = row.directory ? FILE_ATTRIBUTE_DIRECTORY : 0;
        item.modified = row.mtime; item.size = row.size;
        item.position = selected + 1; item.count = static_cast<int>(rows.size());
        preview.SetAccent(accent_color);
        // The host refreshes disk facts itself; compare the requested result identity.
        const auto& shown = preview_requested;
        if ((!preview.visible() && !preview_occluded) || preview_dark != dark || shown.path != item.path ||
            shown.modified != item.modified || shown.size != item.size || shown.attrs != item.attrs) {
            ClearPreviewMessages();
            preview_dark = dark;
            preview_requested = item;
            preview.ShowEmbedded(item, bounds, dark, false);
        } else preview.SetEmbeddedBounds(bounds);
        preview.SetSearchQuery(content_mode ? query : std::wstring{});
        // These overlays share the parent surface; child HWNDs otherwise cover them.
        // Hide only the surface so closing an overlay preserves decoding and scroll state.
        const bool occluded = (tray_open && events.staged_paths) ||
            (scope_tooltip_visible && !TooltipText().empty());
        if (occluded) {
            if (preview.visible()) {
                ClearPreviewMessages();
                if (GetFocus() == preview.hwnd() || IsChild(preview.hwnd(), GetFocus())) SetFocus(edit);
                ShowWindow(preview.hwnd(), SW_HIDE);
            }
        } else if (preview_occluded) ShowWindow(preview.hwnd(), SW_SHOWNOACTIVATE);
        preview_occluded = occluded;
    }
    std::wstring FooterStatus() const {
        if (!error.empty()) return rows.empty() ? L"" : error;
        return busy ? Text(StringId::GlobalSearchLoading)
                    : std::to_wstring(total) + Text(StringId::GlobalSearchResults);
    }
    void DrawScopeTooltip(const ui::Theme& theme) {
        const auto tip = TooltipText();
        if (!scope_tooltip_visible || tip.empty()) return;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(ui::typography::CreateTextFormat(compositor.DwriteFactory(),
            {ui::typography::FontRole::Text, 12 * scale}, &format))) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(write_factory->CreateTextLayout(tip.data(), static_cast<UINT32>(tip.size()), format.Get(),
            std::max(1.0f, (width - 32) * scale), 10000, &layout))) return;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) return;
        const float tooltip_width = std::min(ui::typography::MeasureLine(&compositor, format.Get(), tip) + 16 * scale, (width - 16) * scale);
        const float tooltip_height = std::ceil(metrics.height) + 16 * scale;
        const float x = std::round(std::clamp((scope_pointer_x + 12) * scale, 8 * scale, width * scale - tooltip_width - 8 * scale));
        const float y = std::round(std::clamp((scope_pointer_y + 18) * scale, 8 * scale, std::max(8 * scale, height * scale - tooltip_height - 8 * scale)));
        const D2D1_RECT_F bounds{x, y, x + tooltip_width, y + tooltip_height};
        // Painter/LumaText rasterize at physical pixel sizes on a 96-DPI target.
        target->SetDpi(96, 96);
        painter.SetScale(scale);
        if (painter.BeginFrame(theme, ui::IsHighContrast())) {
            painter.DrawTooltip(bounds, metrics.lineCount > 1 ? std::wstring_view{} : std::wstring_view{tip});
            if (metrics.lineCount > 1) {
                brush->SetColor(theme.text);
                if (target.Get() == compositor.Dc())
                    compositor.DrawTextLayout({x + 8 * scale, y + 8 * scale}, layout.Get(), brush.Get());
                else
                    target->DrawTextLayout({x + 8 * scale, y + 8 * scale}, layout.Get(), brush.Get());
            }
        }
        target->SetDpi(96 * scale, 96 * scale);
        painter.SetScale(1.0f);
    }

    struct Palette {
        D2D1_COLOR_F text, muted, faint, line, hover, sel, sel_stroke, chip, key, accent, accent_text, mark;
    };
    Palette MakePalette(const ui::Theme& theme) const {
        Palette p{};
        const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 1), black = D2D1::ColorF(0, 0, 0, 1);
        p.text = theme.text;
        p.muted = dark ? D2D1::ColorF(0xa6aab3) : D2D1::ColorF(0x50545e);
        p.faint = dark ? D2D1::ColorF(0x7c808a) : D2D1::ColorF(0x767a84);
        p.line = WithAlpha(dark ? white : black, 0.08f);
        p.hover = WithAlpha(dark ? white : black, dark ? 0.05f : 0.04f);
        p.sel = WithAlpha(theme.accent, dark ? 0.26f : 0.16f);
        p.sel_stroke = WithAlpha(dark ? Mix(theme.accent, white, 0.35f) : theme.accent, 0.34f);
        p.chip = WithAlpha(dark ? white : black, dark ? 0.06f : 0.05f);
        p.key = WithAlpha(dark ? white : black, dark ? 0.09f : 0.06f);
        p.accent = theme.accent;
        p.accent_text = dark ? Mix(theme.accent, white, 0.38f) : Mix(theme.accent, black, 0.12f);
        p.mark = MarkColor();
        if (ui::IsHighContrast()) {
            p.muted = p.faint = theme.text; p.line = theme.text; p.sel = theme.selection_bg; p.accent_text = theme.accent;
        }
        return p;
    }
    void AddHot(D2D1_RECT_F rect, Hot kind, int index = 0) {
        if (kind == HotRow) { rect.top = std::max(rect.top, ListTop()); rect.bottom = std::min(rect.bottom, ListBottom()); }
        if (hot_clip) { rect.top = std::max(rect.top, hot_clip_top); rect.bottom = std::min(rect.bottom, hot_clip_bottom); }
        if (rect.bottom > rect.top && rect.right > rect.left) hot.push_back({rect, kind, index});
    }
    bool Hovered(Hot kind, int index = 0) const { return hover_kind == kind && hover_index == index; }

    void DrawHeader(const Palette& p) {
        const float mid = (kFieldTop + kFieldTop + kFieldHeight + grow) / 2;
        DrawLogo({20, mid - 15, 50, mid + 15});
        const auto field = FieldRect();
        const auto layout = SearchFieldLayout();
        AddHot(field, HotSearchField);
        AddHot(layout.scope, HotSearchScope); AddHot(layout.mode, HotMode); AddHot(layout.close, HotSearchClear);
        ui::AddressSearchControlState control;
        control.focused = GetFocus() == edit; control.content = content_mode;
        control.current = !ScopePath().empty();
        control.scope_label = browse_folder.empty() && scope >= 0 && scope < static_cast<int>(scopes.size())
            ? scopes[static_cast<size_t>(scope)].label : LeafName(browse_folder);
        if (Hovered(HotSearchScope)) control.hover = ui::AddressSearchPart::Scope;
        else if (Hovered(HotMode)) control.hover = ui::AddressSearchPart::Mode;
        else if (Hovered(HotSearchClear)) control.hover = ui::AddressSearchPart::Close;
        else if (Hovered(HotSearchField)) control.hover = ui::AddressSearchPart::Field;
        painter.SetScale(1);
        const auto theme = Theme();
        if (painter.BeginFrame(theme, ui::IsHighContrast())) {
            ui::DrawAddressSearchControl(painter, compositor.Dc(), theme, field, layout, 1, control,
                [&](float x, float y, float w, float h, const wchar_t* glyph, D2D1_COLOR_F color, float ratio) {
                    CommandIcon(glyph, {x, y, x + w, y + h}, std::min(w, h) * ratio, color);
                },
                [&](const std::wstring& text, D2D1_RECT_F bounds, D2D1_COLOR_F color) { LabelC(text, bounds, 12, color); },
                [&](const std::wstring& text) { return TextWidth(text, 12); });
        }
        // Esc
        const D2D1_RECT_F esc{width - 56, mid - 11, width - 20, mid + 11};
        AddHot(esc, HotEsc);
        FillC(esc, Hovered(HotEsc) ? p.hover : p.key, 5);
        LabelC(L"Esc", esc, 11, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (busy) {
            // Thin sweep under the header while providers work.
            const float t = static_cast<float>(ui::motion::NowMs() % 1100) / 1100.0f;
            const float w = width * 0.28f, x = -w + (width + w) * t;
            FillC({std::max(0.0f, x), Header() - 1, std::min(width, x + w), Header() + 1}, WithAlpha(p.accent_text, 0.9f));
        }
        FillC({0, Header(), width, Header() + 1}, p.line);
    }
    void DrawChip(D2D1_RECT_F rect, const std::wstring& text, bool on, bool hovered, bool enabled, UINT32 dot, const Palette& p,
                  const std::wstring& count = {}, bool dim = false) {
        const D2D1_COLOR_F fill = on ? p.accent : hovered && enabled ? p.hover : p.chip;
        FillC(rect, enabled && !dim ? fill : WithAlpha(fill, fill.a * 0.5f), (rect.bottom - rect.top) / 2);
        float x = rect.left + 9;
        D2D1_COLOR_F color = on ? D2D1::ColorF(1, 1, 1, 1) : hovered ? p.text : p.muted;
        if (!enabled) color = WithAlpha(p.faint, 0.6f);
        else if (dim && !hovered) color = WithAlpha(p.muted, 0.55f);
        if (dot) {
            FillC({x, (rect.top + rect.bottom) / 2 - 4, x + 8, (rect.top + rect.bottom) / 2 + 4},
                  on ? D2D1::ColorF(1, 1, 1, 0.85f) : WithAlpha(Color(dot), enabled && !dim ? 1.0f : 0.5f), 2);
            x += 13;
        }
        LabelC(text, {x, rect.top, rect.right - 6, rect.bottom}, 12, color);
        if (!count.empty()) {
            const float cx = x + TextWidth(text, 12) + 5;
            LabelC(count, {cx, rect.top, rect.right - 4, rect.bottom}, 11, on ? D2D1::ColorF(1, 1, 1, 0.78f) : WithAlpha(p.faint, dim ? 0.6f : 1.0f));
        }
    }
    float ChipWidth(const std::wstring& text, bool dot, const std::wstring& count = {}) {
        return TextWidth(text, 12) + 18 + (dot ? 13 : 0) + (count.empty() ? 0 : TextWidth(count, 11) + 5);
    }
    std::wstring KindCount(int i) const { return kind_counts_known && !content_mode ? std::to_wstring(kind_counts[static_cast<size_t>(i)]) : std::wstring{}; }
    void DrawSide(const Palette& p) {
        const float side = SideWidth();
        if (side <= 0) return;
        const float top = Header() + 1, bottom = ListBottom();
        FillC({side, top, side + 1, bottom}, p.line);
        // The sidebar scrolls when its sections do not fit (short windows, many places).
        side_scroll = std::clamp(side_scroll, 0.0f, std::max(0.0f, side_content - (bottom - top)));
        const float off = side_scroll;
        target->PushAxisAlignedClip({0, top, side, bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        hot_clip = true; hot_clip_top = top; hot_clip_bottom = bottom;
        float y = top + 8 - off;
        LabelC(Text(StringId::AdvSearchLocation), {20, y, side - 12, y + 22}, 11.5f, p.faint);
        y += 24;
        D2D1_RECT_F plate_target{};
        for (size_t i = 0; i < scopes.size(); ++i) {
            const D2D1_RECT_F rect{10, y, side - 10, y + 30};
            if (static_cast<int>(i) == scope) plate_target = rect;
            AddHot(rect, HotScope, static_cast<int>(i));
            if (Hovered(HotScope, static_cast<int>(i)) && static_cast<int>(i) != scope) FillC(rect, p.hover, 6);
            y += 30;
        }
        if (plate_target.right > plate_target.left) {
            scope_motion.SetAnimationsEnabled(animations && !ui::IsHighContrast());
            const auto plate = scope_motion.Update(2, scope, plate_target, frame, ui::motion::NowMs(), ui::motion::spring::Snappy);
            FillC(plate, p.sel, 6);
            FillC({plate.left, plate.top + 8, plate.left + 3, plate.bottom - 8}, p.accent, 1.5f);
        }
        float row_y = top + 32 - off;
        for (size_t i = 0; i < scopes.size(); ++i) {
            const auto& item = scopes[i];
            const bool on = static_cast<int>(i) == scope;
            const bool hc_selected = on && ui::IsHighContrast();
            const auto selected_ink = hc_selected ? ui::ColorFromRef(GetSysColor(COLOR_HIGHLIGHTTEXT)) : p.text;
            const bool zero = item.counted && item.count == 0 && !query.empty() && !on;
            CommandIcon(item.glyph, {22, row_y + 7, 40, row_y + 23}, 14, hc_selected ? selected_ink : on ? p.accent_text : zero ? WithAlpha(p.muted, 0.55f) : p.muted);
            float count_left = side - 20;
            if (item.counted && !query.empty()) {
                const auto count = std::to_wstring(item.count);
                const float cw = TextWidth(count, 11.5f);
                LabelC(count, {side - 20 - cw, row_y, side - 20, row_y + 30}, 11.5f, hc_selected ? selected_ink : on ? p.accent_text : p.faint);
                count_left = side - 26 - cw;
            }
            const float label_w = std::min(TextWidth(item.label, 13), count_left - 46);
            LabelC(item.label, {46, row_y, 46 + std::max(0.0f, label_w) + 1, row_y + 30}, 13, hc_selected ? selected_ink : zero ? p.faint : p.text);
            if (item.kind == ScopeKind::Current && !current_folder.empty()) {
                const float sub_left = 46 + label_w + 6;
                LabelC(LeafName(current_folder), {sub_left, row_y + 1, count_left, row_y + 30}, 11, hc_selected ? selected_ink : p.faint);
            }
            row_y += 30;
        }
        {
            const D2D1_RECT_F rect{10, row_y, side - 10, row_y + 30};
            AddHot(rect, HotAddPlace);
            const bool hovered = Hovered(HotAddPlace);
            if (hovered) FillC(rect, p.hover, 6);
            CommandIcon(L"\xE710", {22, row_y + 8, 40, row_y + 22}, 12, hovered ? p.accent_text : p.faint);
            LabelC(Text(StringId::GlobalSearchAddPlace), {46, row_y, side - 14, row_y + 30}, 12.5f, hovered ? p.accent_text : p.faint);
            row_y += 30;
        }
        // Chip rows: label, dot colour, count (empty: unknown) and whether it reads as empty.
        auto chips = [&](float& cy, float left, int first_index, int count, auto label_of, auto dot_of, auto count_of, int current,
                         Hot kind, bool enabled) {
            float cx = left;
            for (int i = first_index; i < count; ++i) {
                const std::wstring label = label_of(i);
                const UINT32 dot = dot_of(i);
                const std::wstring number = count_of(i);
                const bool dim = number == L"0" && i != current;
                const float w = ChipWidth(label, dot != 0, number);
                if (cx + w > side - 10 && cx > left) { cx = left; cy += 31; }
                const D2D1_RECT_F rect{cx, cy, cx + w, cy + 26};
                if (enabled) AddHot(rect, kind, i);
                DrawChip(rect, label, enabled && i == current, Hovered(kind, i), enabled, dot, p, number, dim);
                cx += w + 5;
            }
            cy += 31;
        };
        row_y += 8;
        LabelC(Text(StringId::SearchChipType), {20, row_y, side - 12, row_y + 22}, 11.5f, p.faint);
        row_y += 26;
        chips(row_y, 16, 0, 8, [&](int i) { return Text(kKindLabels[i]); }, [&](int i) { return kKindColors[i]; },
              [&](int i) { return KindCount(i); }, kind_filter, HotKind, !content_mode);
        // 扩展名: the extensions found under the chosen type.
        if (!content_mode && kind_filter >= 2 && ext_kind == kind_filter && (ext_counts.size() > 1 || !ext_filter.empty())) {
            const float sub_top = row_y - 4;
            LabelC(Text(StringId::GlobalSearchExtension), {28, row_y - 2, side - 12, row_y + 16}, 10.5f, p.faint);
            row_y += 18;
            int current = -1;
            for (size_t i = 0; i < ext_counts.size(); ++i) if (ext_counts[i].first == ext_filter) current = static_cast<int>(i);
            chips(row_y, 28, 0, static_cast<int>(ext_counts.size()), [&](int i) { return L"." + ext_counts[static_cast<size_t>(i)].first; },
                  [](int) { return UINT32{0}; },
                  [&](int i) { return ext_counts_known ? std::to_wstring(ext_counts[static_cast<size_t>(i)].second) : std::wstring{}; },
                  current, HotExt, true);
            FillC({18, sub_top, 20, row_y - 9}, p.line, 1);
        }
        row_y += 4;
        LabelC(Text(StringId::SearchChipSize), {20, row_y, side - 12, row_y + 22}, 11.5f, p.faint);
        row_y += 26;
        chips(row_y, 16, 1, 5, [&](int i) { return Text(kSizeLabels[i]); }, [](int) { return UINT32{0}; },
              [&](int i) { return size_counts_known && !content_mode ? std::to_wstring(size_counts[static_cast<size_t>(i)]) : std::wstring{}; },
              size_filter, HotSize, !content_mode && kKinds[kind_filter] != index::SearchKind::Folder);
        row_y += 4;
        LabelC(Text(StringId::SearchChipDate), {20, row_y, side - 12, row_y + 22}, 11.5f, p.faint);
        row_y += 26;
        if (!content_mode) {
            timeline_rect = {18, row_y, side - 18, row_y + 48};
            std::array<size_t, 12> counts{};
            FILETIME time{}; GetSystemTimeAsFileTime(&time);
            const uint64_t now = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
            for (const auto& row : AllRows()) {
                if (!row.mtime || row.mtime > now) continue;
                const auto age = (now - row.mtime) / (7ull * 864000000000ull);
                if (age < 12) ++counts[11 - static_cast<size_t>(age)];
            }
            const size_t max_count = std::max<size_t>(1, *std::max_element(counts.begin(), counts.end()));
            const float bw = (timeline_rect.right - timeline_rect.left) / 12;
            for (int i = 0; i < 12; ++i) {
                const float x = timeline_rect.left + i * bw;
                AddHot({x, row_y, x + bw, row_y + 48}, HotTimeline, i);
                const float h = std::max(2.0f, 48 * static_cast<float>(counts[static_cast<size_t>(i)]) / static_cast<float>(max_count));
                FillC({x, row_y + 48 - h, x + bw - 3, row_y + 48}, timeline_first >= 0 && i >= timeline_first && i <= timeline_last ? p.accent : WithAlpha(p.faint, .45f), 2);
            }
            LabelC(Text(StringId::GlobalSearchTimelineResults), {18, row_y + 50, side - 18, row_y + 75}, 10.5f, p.faint);
            row_y += 78;
        }
        hot_clip = false;
        side_content = row_y + off - top + 8;
        const float view = bottom - top;
        if (side_content > view + 1) {
            const float thumb = std::max(24.0f, view * view / side_content);
            const float ty = top + (view - thumb) * off / std::max(1.0f, side_content - view);
            FillC({side - 5, ty + 2, side - 2, ty + thumb - 2}, WithAlpha(p.muted, 0.35f), 1.5f);
        }
        target->PopAxisAlignedClip();
    }
    void DrawRow(size_t i, float y, float left, float right, const Palette& p, float alpha) {
        const auto& row = rows[i];
        if (IsMarked(row.path)) FillC({left, y, right, y + RowHeight()}, p.sel, 8);
        const bool sel = static_cast<int>(i) == selected && ResultSelectionActive();
        const bool hc_selected = ui::IsHighContrast() && (sel || IsMarked(row.path));
        const auto text_ink = hc_selected ? ui::ColorFromRef(GetSysColor(COLOR_HIGHLIGHTTEXT)) : p.text;
        const auto muted_ink = hc_selected ? text_ink : p.muted;
        const auto faint_ink = hc_selected ? text_ink : p.faint;
        const float h = RowHeight();
        const bool content_row = content_mode && !showing_recent;
        if (alpha < 1.0f) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(), alpha), nullptr);
        const D2D1_RECT_F rect{left, y, right, y + h};
        if (Hovered(HotRow, static_cast<int>(i)) && !sel) FillC(rect, p.hover, 7);
        const float icon_top = content_row ? y + 10 : y + (h - 28) / 2;
        const D2D1_RECT_F icon_rect{left + 12, icon_top, left + 40, icon_top + 28};
        if (!icons.Draw(compositor.Dc(), icon_rect, row.recycle_path.empty() ? row.path : row.recycle_path, row.name, row.directory,
            row.directory ? FILE_ATTRIBUTE_DIRECTORY : 0))
            CommandIcon(row.directory ? L"\xE8B7" : L"\xE8A5", icon_rect, 22, faint_ink);
        const float text_left = left + 52;
        const bool show_hints = sel && right - left >= 460;
        const float hints_w = show_hints ? (row.app ? 90.0f : row.directory || preview_open ? 150.0f : 205.0f) : 0.0f;
        const float meta_w = sel || right - left < 360 ? 0.0f : 112.0f;
        const float text_right = right - 12 - std::max(hints_w, meta_w);
        const float name_top = content_row ? y + 8 : y + 7;
        LabelC(row.alias.empty() ? row.name : row.alias + L" \x00B7 " + row.name, {text_left, name_top, text_right, name_top + 20}, 14,
               text_ink, false, L"Segoe UI", !showing_recent);
        const float path_top = name_top + 20;
        std::wstring parent = row.app ? Text(StringId::TypeApplication) : path::StripExtendedPathPrefix(ParentPath(row.path));
        if (!row.recycle_path.empty()) parent = Text(StringId::RecycleBin) + L" · " + parent;
        float path_left = text_left;
        if (row.network) {
            CommandIcon(L"\xE8CE", {text_left, path_top + 2, text_left + 14, path_top + 15}, 10, faint_ink);
            path_left += 16;
        }
        LabelC(MiddleEllipsis(parent, text_right - path_left, 12), {path_left, path_top, text_right, path_top + 17}, 12, faint_ink);
        if (content_row && !row.snippet.empty())
            LabelC(row.snippet, {text_left, path_top + 19, right - 16, path_top + 41}, 12.5f, muted_ink, false, L"Segoe UI", true);
        if (!sel && !row.app && meta_w > 0) {
            const auto time = FormatTime(row.mtime);
            const auto size = row.directory ? Text(StringId::KindFolder) : row.size || row.mtime ? FormatSize(row.size) : std::wstring{};
            const float mt = content_row ? y + 9 : y + 8;
            LabelC(time, {right - 12 - meta_w, mt, right - 12, mt + 17}, 11.5f, faint_ink, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            LabelC(size, {right - 12 - meta_w, mt + 17, right - 12, mt + 34}, 11.5f, faint_ink, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (show_hints) {
            const float mid = content_row ? y + 20 : y + h / 2;
            float r = right - 12;
            if (!row.app) r = KeyHint(L"Ctrl \x21B5", Text(StringId::Location), r, mid, muted_ink, p.key) - 12;
            r = KeyHint(L"\x21B5", Text(StringId::Open), r, mid, muted_ink, p.key) - 12;
            if (!row.directory && !row.app && !preview_open) KeyHint(L"Space", Text(StringId::Preview), r, mid, muted_ink, p.key);
        }
        if (alpha < 1.0f) target->PopLayer();
    }
    void DrawEmpty(float left, float right, float top, float bottom, const Palette& p) {
        const float cx = (left + right) / 2;
        float y = (top + bottom) / 2 - 40;
        if (busy) {
            LabelC(Text(StringId::GlobalSearchLoading), {left, y, right, y + 40}, 14, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (!error.empty()) {
            LabelC(error, {left + 16, y, right - 16, y + 40}, 14, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (!HasSearchQuery() && browse_folder.empty()) {
            if (empty_art_target != target.Get() || empty_art_scale != scale) {
                empty_art.Reset(); empty_art_target = target.Get(); empty_art_scale = scale;
                ui::CreateSvgResourceBitmap(target.Get(), IDR_EMPTY_FOLDER_SVG,
                    D2D1::SizeU(static_cast<UINT32>(Px(200)), static_cast<UINT32>(Px(200 * 360.0f / 512))), D2D1::SizeF(512, 360), &empty_art);
            }
            if (empty_art) target->DrawBitmap(empty_art.Get(), {cx - 100, y - 110, cx + 100, y + 30}, 1.0f);
            LabelC(Text(StringId::GlobalSearchEmpty), {left, y + 36, right, y + 62}, 14, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        const bool filtered = scope != 0 || !parsed_query.tokens.empty() || (!content_mode && (FileFilters() || date_filter != 0 || timeline_first >= 0));
        const bool compact = calculation && bottom - top < 130;
        if (compact) y = top + 6;
        LabelC(Text(StringId::GlobalSearchNoResults), {left, y, right, y + 26}, 15, p.text, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (filtered) LabelC(Text(StringId::GlobalSearchNoFilteredResults), {left, y + 26, right, y + 48}, 12.5f, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        const auto& action = Text(filtered ? StringId::EmptyActClearFilters : content_mode ? StringId::SearchModeName : StringId::EmptyActContent);
        const float bw = TextWidth(action, 12.5f) + 28;
        const float button_top = y + (compact ? (filtered ? 52.0f : 32.0f) : 60.0f);
        const D2D1_RECT_F button{cx - bw / 2, button_top, cx + bw / 2, button_top + (compact ? 26.0f : 30.0f)};
        const Hot kind = filtered ? HotClearFilters : HotSearchContent;
        AddHot(button, kind);
        FillC(button, Hovered(kind) ? p.hover : p.chip, 7);
        LabelC(action, button, 12.5f, p.accent_text, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    bool OrderControls() const {
        return !showing_recent && browse_folder.empty() && !command_mode && HasSearchQuery();
    }
    // 排序 / 分组 buttons: icon, current choice (left out when narrow), direction, chevron.
    D2D1_RECT_F DrawOrderButton(float right, float y, Hot spot, const wchar_t* glyph, const std::wstring& label, bool active,
                                bool arrow, bool show_label, const Palette& p) {
        const float tw = show_label ? TextWidth(label, 12) : 0.0f;
        const float w = 8 + 15 + (show_label ? 6 + tw : 0.0f) + (arrow ? 14.0f : 0.0f) + 18;
        const D2D1_RECT_F rect{right - w, y, right, y + 28};
        AddHot(rect, spot);
        const bool hovered = Hovered(spot);
        FillC(rect, active ? p.sel : hovered ? p.hover : p.chip, 7);
        const auto ink = active ? p.accent_text : hovered ? p.text : p.muted;
        float x = rect.left + 8;
        CommandIcon(glyph, {x, y + 6.5f, x + 15, y + 21.5f}, 15, ink); x += 15;
        if (show_label) { x += 6; LabelC(label, {x, y, x + tw + 1, y + 28}, 12, ink); x += tw; }
        if (arrow) { CommandIcon(sort_desc ? L"\xE74B" : L"\xE74A", {x + 2, y + 8, x + 14, y + 20}, 11, ink); x += 14; }
        CommandIcon(L"\xE70D", {x + 5, y + 10, x + 14, y + 19}, 9, WithAlpha(ink, 0.7f));
        return rect;
    }
    void DrawCategoryStrip(const Palette& p) {
        if (!CategoryStrip()) return;
        const float top = Header() + 43, l = ListLeft() + 14, r = ListRight() - 14;
        FillC({ListLeft(), top - 1, ListRight(), top}, p.line);
        float x = l;
        const float y = top + 6;
        for (int i = 0; i < 8; ++i) {
            const auto number = KindCount(i);
            if (number == L"0" && i != kind_filter && i != 0) continue;  // empty types make room
            const auto label = Text(kKindLabels[i]);
            const float w = ChipWidth(label, kKindColors[i] != 0, number);
            if (x + w > r) break;
            const D2D1_RECT_F rect{x, y, x + w, y + 26};
            AddHot(rect, HotKind, i);
            DrawChip(rect, label, i == kind_filter, Hovered(HotKind, i), true, kKindColors[i], p, number);
            x += w + 5;
        }
    }
    void DrawResultBar(const Palette& p) {
        const float l = ListLeft() + 14, r = ListRight() - 14, y = Header() + 8;
        const float segment = std::min(140.0f, (r - l) * .5f);
        tile_size_slider = {};
        const float slider_width = std::min(ui::kIconSizeSliderWidth, r - segment - 10 - l);
        if (IsGrid() && slider_width >= 70) tile_size_slider = {r - segment - 10 - slider_width, y, r - segment - 10, y + 28};
        float content_end = tile_size_slider.right > tile_size_slider.left ? tile_size_slider.left - 8 : r - segment - 10;
        sort_button = group_button = {};
        if (OrderControls()) {
            const bool sorted = sort_order != app::GlobalSearchSort::Relevance;
            const bool grouped = group_by != app::GlobalSearchGroup::Match;
            const bool labels = content_end - l >= 300;
            group_button = DrawOrderButton(content_end, y, HotGroupMenu, L"\xF168", Text(GroupLabel(group_by)), grouped, false,
                                           labels || grouped, p);
            sort_button = DrawOrderButton(group_button.left - 6, y, HotSort, L"\xE8CB", Text(SortLabel(sort_order)), sorted, sorted,
                                          labels || sorted, p);
            content_end = sort_button.left - 10;
        }
        DrawCategoryStrip(p);
        FillC({ListLeft(), ResultBodyTop() - 1, ListRight(), ResultBodyTop()}, p.line);
        float x = l;
        if (!browse_folder.empty() && content_end - l > 55) {
            AddHot({x, y, x + 26, y + 28}, HotBack);
            LabelC(L"\x2190", {x, y, x + 26, y + 28}, 14, p.muted); x += 28;
            breadcrumb_paths.clear();
            for (auto path = browse_folder; !path.empty() && breadcrumb_paths.size() < 64;) {
                breadcrumb_paths.push_back(path);
                const auto parent = ParentPath(path);
                if (parent == path) break;
                path = parent;
            }
            std::reverse(breadcrumb_paths.begin(), breadcrumb_paths.end());
            const float end = content_end;
            AddHot({x, y, std::min(x + 22, end), y + 28}, HotExitFolder);
            LabelC(L"…", {x, y, std::min(x + 22, end), y + 28}, 12, p.muted); x += 24;
            size_t start = breadcrumb_paths.empty() ? 0 : breadcrumb_paths.size() - 1;
            float used = std::min(120.0f, TextWidth(LeafName(browse_folder), 12) + 12);
            while (start > 0) {
                const float needed = std::min(120.0f, TextWidth(LeafName(breadcrumb_paths[start - 1]), 12) + 12) + 10;
                if (used + needed > end - x) break;
                used += needed; --start;
            }
            for (size_t i = start; i < breadcrumb_paths.size() && x < end; ++i) {
                const auto label = LeafName(breadcrumb_paths[i]);
                const float w = std::min({120.0f, TextWidth(label, 12) + 12, end - x});
                AddHot({x, y, x + w, y + 28}, HotBreadcrumb, static_cast<int>(i));
                LabelC(label, {x + 3, y, x + w - 3, y + 28}, 12, i + 1 == breadcrumb_paths.size() ? p.text : p.muted); x += w;
                if (i + 1 < breadcrumb_paths.size()) { LabelC(L"›", {x, y, x + 10, y + 28}, 12, p.faint); x += 10; }
            }
        } else {
            if (parsed_query.tokens.empty()) {
                const auto count = std::to_wstring(AllRows().size());
                auto label = showing_recent ? Text(StringId::Recent) + L" · " + count : count + Text(StringId::GlobalSearchResults);
                if (OrderControls() && sort_order != app::GlobalSearchSort::Relevance) {
                    wchar_t sorted_by[96]{};
                    swprintf_s(sorted_by, Text(StringId::GlobalSearchSortedBy).c_str(), Text(SortLabel(sort_order)).c_str());
                    label += L" \x00b7 "; label += sorted_by;
                }
                LabelC(label, {x, y, content_end, y + 28}, 12, p.faint);
            }
            for (size_t i = 0; i < parsed_query.tokens.size(); ++i) {
                const auto& token = parsed_query.tokens[i];
                const float w = std::min(160.0f, TextWidth(token.label, 12) + 30);
                if (x + w > content_end) break;
                const D2D1_RECT_F rect{x, y + 2, x + w, y + 26};
                FillC(rect, p.sel, 12); AddHot(rect, HotToken, static_cast<int>(i));
                LabelC(token.label + L" \x00d7", {x + 8, y + 2, x + w - 5, y + 26}, 12, p.accent_text); x += w + 6;
            }
        }
        FillC({r - segment, y, r, y + 28}, p.chip, 7);
        for (int i = 0; i != 2; ++i) {
            D2D1_RECT_F rect{r - segment + i * segment / 2 + 3, y + 3, r - segment + (i + 1) * segment / 2 - 3, y + 25};
            AddHot(rect, HotView, i);
            if ((grid_view ? 1 : 0) == i) FillC(rect, p.key, 5);
            LabelC(Text(i ? StringId::GlobalSearchThumbnails : StringId::ViewList), rect, 12, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (tile_size_slider.right > tile_size_slider.left) {
            AddHot(tile_size_slider, HotTileSize);
            ui::DrawIconSizeSlider(compositor.Dc(), brush.Get(), tile_size_slider, tile_width - 24, 1, scale,
                Theme(), tile_size_dragging || Hovered(HotTileSize));
        }
    }
    void UpdateTileSize(float x) {
        const float value = ui::IconSizeSliderValueAt(tile_size_slider, x, 1, scale);
        if (value <= 0 || tile_width == value + 24) return;
        tile_width = value + 24;
        ++layout_epoch; BuildLayout(); ClampSelection(false); Invalidate();
    }
    bool IsMarked(const std::wstring& path) const {
        return std::find(marked_paths.begin(), marked_paths.end(), path) != marked_paths.end();
    }
    void ToggleMarked() {
        if (command_mode) return;
        if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto& row = rows[static_cast<size_t>(selected)];
        if (row.path.empty() || row.app || !row.recycle_path.empty()) return;
        if (calculation) keyboard_nav = true;
        const auto it = std::find(marked_paths.begin(), marked_paths.end(), row.path);
        if (it == marked_paths.end()) marked_paths.push_back(row.path); else marked_paths.erase(it);
        Invalidate();
    }
    void MarkRange(int from, int to) {
        if (rows.empty() || command_mode) return;
        const int first_index = std::clamp(std::min(from, to), 0, static_cast<int>(rows.size()) - 1);
        const int last_index = std::clamp(std::max(from, to), 0, static_cast<int>(rows.size()) - 1);
        for (int i = first_index; i <= last_index; ++i) {
            const auto& row = rows[static_cast<size_t>(i)];
            if (!row.app && row.recycle_path.empty() && !IsMarked(row.path)) marked_paths.push_back(row.path);
        }
    }
    void StageSelection() {
        if (!events.stage_paths || stale || command_mode) return;
        auto paths = marked_paths;
        if (paths.empty() && selected >= 0 && selected < static_cast<int>(rows.size())) {
            const auto& row = rows[static_cast<size_t>(selected)];
            if (!row.path.empty() && !row.app && row.recycle_path.empty()) paths.push_back(row.path);
        }
        if (!paths.empty()) { events.stage_paths(paths); ShowToast(Text(StringId::TrayStagedBadge)); }
        Invalidate();
    }
    void CopyFiles() {
        if (command_mode || stale) return;
        auto paths = marked_paths;
        if (paths.empty() && selected >= 0 && selected < static_cast<int>(rows.size())) {
            const auto& row = rows[static_cast<size_t>(selected)];
            if (!row.path.empty() && !row.app && row.recycle_path.empty()) paths.push_back(row.path);
        }
        if (paths.empty() || !OpenClipboard(hwnd)) return;
        size_t chars = 1;
        for (const auto& path : paths) chars += path.size() + 1;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
        if (memory) {
            if (auto* drop = static_cast<DROPFILES*>(GlobalLock(memory))) {
                drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
                auto* out = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES));
                for (const auto& path : paths) { memcpy(out, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); out += path.size() + 1; }
                GlobalUnlock(memory); EmptyClipboard();
                if (!SetClipboardData(CF_HDROP, memory)) GlobalFree(memory);
                else ShowToast(Text(StringId::CopiedShort));
            } else GlobalFree(memory);
        }
        CloseClipboard();
    }
    void ResultMenu(POINT point) {
        if (command_mode) return;
        if (selected < 0 || selected >= static_cast<int>(rows.size()) || stale) return;
        if (!EnsureTarget() || !result_menu.Create(hwnd, &compositor, scale)) return;
        result_menu.SetTheme(dark, accent_color);
        std::vector<ui::FluentMenuItem> items;
        const auto add = [&](int id, StringId text, const wchar_t* glyph, const wchar_t* shortcut = L"") {
            ui::FluentMenuItem item;
            item.command = id; item.text = Text(text); item.glyph = glyph; item.shortcut = shortcut;
            items.push_back(std::move(item));
        };
        add(1, StringId::Open, L"\xE8E5", L"Enter");
        if (rows[static_cast<size_t>(selected)].directory) add(2, StringId::GlobalSearchEnterFolder, L"\xE8B7", L"\x2192");
        add(3, StringId::HintActOpenPath, L"\xE838", L"Ctrl+Enter");
        items.back().separator_after = true;
        add(4, StringId::CopyPath, L"\xE71B", L"Ctrl+C");
        add(7, StringId::Copy, L"\xE8C8");
        if (events.stage_paths) add(5, StringId::CtxAddToTray, L"\xE8A1", L"Alt+A");
        if (handoff) add(6, StringId::GlobalSearchHandoff, L"\xE721", L"Shift+Enter");
        const auto menu_generation = generation;
        const auto menu_path = rows[static_cast<size_t>(selected)].path;
        point.x -= ui::FluentMenu::kShadowMargin;
        point.y -= ui::FluentMenu::kShadowMargin;
        result_menu_open = true;
        const int chosen = result_menu.TrackPopup(point, std::move(items));
        result_menu_open = false;
        if (!IsWindowVisible(hwnd) || generation != menu_generation || selected < 0 || selected >= static_cast<int>(rows.size()) ||
            rows[static_cast<size_t>(selected)].path != menu_path) return;
        SetFocus(edit);
        switch (chosen) {
        case 1: Open(false); break;
        case 2: EnterFolder(); break;
        case 3: Open(true); break;
        case 4: CopyPath(); break;
        case 5: StageSelection(); break;
        case 6: Handoff(); break;
        case 7: CopyFiles(); break;
        default: break;
        }
    }
    void DrawGrid(const Palette& p) {
        const float left = ListLeft(), right = ListRight(), top = ListTop(), bottom = ListBottom();
        target->PushAxisAlignedClip({left, top, right, bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!grid_cache_ready) {
            grid_thumbnails.SetParallelism(ui::ThumbnailCache::DefaultParallelism());
            grid_thumbnails.SetNotifyWindow(hwnd); grid_cache_ready = true;
        }
        grid_thumbnails.SetDeviceContext(compositor.Dc());
        grid_thumbnails.BeginFrame(); grid_thumbnails.AdvanceFrame(hwnd);
        const float cell = (right - left - 16) / static_cast<float>(GridColumns());
        if (grid_grouped)
            for (size_t g = 0; g < groups.size() && g < group_top.size(); ++g) {
                if (!groups[g].header) continue;
                const float gy = top + group_top[g] - scroll;
                if (gy + kGroupRow >= top && gy <= bottom) DrawGroupHeader(g, gy, left + 8, right - 8, top, bottom, p);
            }
        for (size_t i = 0; i < rows.size(); ++i) {
            const float y = top + row_top[i] - scroll;
            if (y + TileHeight() < top || y > bottom) continue;
            const auto& row = rows[i];
            const float x = left + 8 + static_cast<float>(i < row_col.size() ? row_col[i] : static_cast<int>(i % GridColumns())) * cell;
            const D2D1_RECT_F rect{x, y, x + cell - 6, y + TileHeight() - 6};
            AddHot(rect, HotRow, static_cast<int>(i));
            if ((static_cast<int>(i) == selected && ResultSelectionActive()) || IsMarked(row.path)) FillC(rect, p.sel, 9);
            else if (Hovered(HotRow, static_cast<int>(i))) FillC(rect, p.hover, 9);
            D2D1_RECT_F art{x + 8, y + 8, x + cell - 14, y + TileHeight() - 54};
            FillC(art, p.chip, 7);
            const DWORD attrs = row.directory ? FILE_ATTRIBUTE_DIRECTORY : 0;
            const long raw_px = std::clamp(static_cast<long>(Px(tile_width)), 32l, 512l);
            const uint32_t thumb_px = static_cast<uint32_t>(
                raw_px <= 64 ? 64l : raw_px <= 128 ? 128l : raw_px <= 256 ? 256l : 512l);
            const auto result = grid_thumbnails.DrawGridThumbnail(compositor.Dc(), art, row.path, attrs,
                thumb_px, generation, row.mtime, row.size, 1, false, nullptr);
            if (result != ui::PreviewDrawResult::Bitmap) {
                const float cx = (art.left + art.right) / 2, cy = (art.top + art.bottom) / 2;
                icons.Draw(compositor.Dc(), {cx - 24, cy - 24, cx + 24, cy + 24}, row.path, row.name, row.directory, attrs);
            }
            const bool hc_selected = ui::IsHighContrast() && ((static_cast<int>(i) == selected && ResultSelectionActive()) || IsMarked(row.path));
            const auto selected_ink = hc_selected ? ui::ColorFromRef(GetSysColor(COLOR_HIGHLIGHTTEXT)) : p.text;
            LabelC(row.name, {x + 8, art.bottom + 4, rect.right - 6, art.bottom + 25}, 13, selected_ink, false, L"Segoe UI", true);
            LabelC(row.directory ? Text(StringId::Folder) : FormatSize(row.size), {x + 8, art.bottom + 25, rect.right - 6, rect.bottom}, 11.5f, hc_selected ? selected_ink : p.faint);
            if (IsMarked(row.path)) LabelC(L"\x2713", {x + 12, y + 10, x + 34, y + 32}, 14, hc_selected ? selected_ink : p.accent_text);
        }
        if (rows.empty() && groups.empty()) DrawEmpty(left, right, top, bottom, p);
        grid_thumbnails.EndInterestFrame();
        target->PopAxisAlignedClip();
    }
    // Group title row. Foldable groups: chevron, colour dot (type groups),
    // where the folder is (folder groups), count; 选择本组 on hover.
    void DrawGroupHeader(size_t index, float gy, float lx, float rx, float clip_top, float clip_bottom, const Palette& p) {
        const auto& group = groups[index];
        const auto title = GroupTitle(group);
        const auto count = std::to_wstring(group.total);
        const float tw = TextWidth(title, 11.5f), cw = TextWidth(count, 11.5f);
        if (!group.foldable) {
            LabelC(title, {lx + 12, gy + 8, lx + 14 + tw, gy + kGroupRow}, 11.5f, p.faint);
            LabelC(count, {lx + 20 + tw, gy + 8, lx + 22 + tw + cw, gy + kGroupRow}, 11.5f, p.faint);
            FillC({lx + 30 + tw + cw, gy + 19, rx - 12, gy + 20}, p.line);
            return;
        }
        const bool closed = Collapsed(group.key);
        const D2D1_RECT_F row{lx, gy + 2, rx, gy + kGroupRow};
        const bool hovered = (Hovered(HotGroupHeader, static_cast<int>(index)) || Hovered(HotGroupSelect, static_cast<int>(index)));
        const auto ink = hovered ? p.muted : p.faint;
        if (row.bottom > clip_top && row.top < clip_bottom)
            AddHot({row.left, std::max(row.top, clip_top), row.right, std::min(row.bottom, clip_bottom)}, HotGroupHeader, static_cast<int>(index));
        float x = lx + 6;
        CommandIcon(closed ? L"\xE76C" : L"\xE70D", {x, gy + 11, x + 11, gy + 22}, 9, ink); x += 16;
        if (group.dot) { FillC({x, gy + 13, x + 8, gy + 21}, Color(group.dot), 2); x += 13; }
        LabelC(title, {x, gy + 8, x + tw + 1, gy + kGroupRow}, 11.5f, ink); x += tw + 6;
        const float select_w = TextWidth(Text(StringId::GroupSelect), 11.5f) + 12;
        const float limit = rx - 12 - (hovered && group.count ? select_w + 8 : 0.0f);
        if (!group.detail.empty() && x < limit - cw - 40) {
            const float dw = std::min(TextWidth(group.detail, 11), limit - cw - 30 - x);
            LabelC(MiddleEllipsis(path::StripExtendedPathPrefix(group.detail), dw, 11), {x, gy + 9, x + dw + 1, gy + kGroupRow}, 11, WithAlpha(p.faint, 0.8f));
            x += dw + 6;
        }
        LabelC(count, {x, gy + 8, x + cw + 1, gy + kGroupRow}, 11.5f, ink); x += cw + 8;
        if (x < limit) FillC({x, gy + 19, limit, gy + 20}, p.line);
        if (hovered && group.count) {
            const D2D1_RECT_F select{rx - 12 - select_w, gy + 6, rx - 12, gy + kGroupRow - 2};
            if (select.bottom > clip_top && select.top < clip_bottom) AddHot(select, HotGroupSelect, static_cast<int>(index));
            if (Hovered(HotGroupSelect, static_cast<int>(index))) FillC(select, p.hover, 5);
            LabelC(Text(StringId::GroupSelect), select, 11.5f, p.accent_text, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    void DrawCalculation(const Palette& p) {
        const auto layout = CalculationPanelLayout();
        const auto card = layout.card;
        FillC(card, Hovered(HotCalculation) ? p.hover : p.chip, 10);
        AddHot(card, HotCalculation);
        const float inset = layout.compact ? 12.0f : 20.0f;
        const float expression_top = card.top + (layout.compact ? 4 : 10);
        LabelC(raw_query + L" =", {card.left + inset, expression_top, card.right - inset, expression_top + 22}, 12.5f, p.muted);
        const float value_top = card.top + (layout.compact ? 25 : 34);
        const float value_right = card.right - (layout.compact ? 100 : inset);
        LabelC(calculation->text, {card.left + inset, value_top, value_right,
            card.top + (layout.compact ? 53 : 76)}, layout.compact ? 22.0f : 30.0f, p.text, true);
        if (!keyboard_nav) KeyHint(L"\x21B5", Text(StringId::Copy), card.right - inset, card.bottom - 16, p.muted, p.key);
        else LabelC(Text(StringId::Copy), {card.right - inset - 80, card.bottom - 28, card.right - inset, card.bottom - 4},
            12, p.muted, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        for (size_t i = 0; i < layout.alternative_count; ++i) {
            const auto rect = layout.alternatives[i];
            const auto& item = calculation->alternatives[i];
            FillC(rect, Hovered(HotAlternative, static_cast<int>(i)) ? p.hover : p.chip, 10);
            AddHot(rect, HotAlternative, static_cast<int>(i));
            // Keep the destination unit visible when the numeric value is
            // ellipsized in a narrow chip; the tooltip retains the full value.
            const auto separator = item.text.find_last_of(L' ');
            const auto label = separator != std::wstring::npos && separator + 1 < item.text.size()
                ? item.text.substr(separator + 1) + L" " + item.text.substr(0, separator) : item.text;
            LabelC(label, {rect.left + 8, rect.top, rect.right - 8, rect.bottom}, 12, p.muted);
        }
    }
    void DrawList(const Palette& p, uint64_t now) {
        DrawResultBar(p);
        if (command_mode) {
            const float l = ListLeft() + 10, r = ListRight() - 10;
            for (size_t i = 0; i < commands.size(); ++i) {
                const float y = ListTop() + 8 + static_cast<float>(i) * 48;
                const D2D1_RECT_F rect{l, y, r, y + 44};
                AddHot(rect, HotRow, static_cast<int>(i));
                if (selected == static_cast<int>(i)) FillC(rect, p.sel, 8);
                const bool hc_selected = ui::IsHighContrast() && selected == static_cast<int>(i);
                const auto selected_ink = hc_selected ? ui::ColorFromRef(GetSysColor(COLOR_HIGHLIGHTTEXT)) : p.text;
                LabelC(L">", {l + 12, y, l + 36, y + 44}, 18, hc_selected ? selected_ink : p.accent_text);
                LabelC(CommandLabel(commands[i]), {l + 44, y, r - 30, y + 44}, 14, selected_ink);
            }
            if (commands.empty()) DrawEmpty(ListLeft(), ListRight(), ListTop(), ListBottom(), p);
            return;
        }
        if (calculation) DrawCalculation(p);
        if (IsGrid()) { DrawGrid(p); return; }
        const float left = ListLeft(), right = ListRight(), top = ListTop(), bottom = ListBottom();
        target->PushAxisAlignedClip({left, top, right, bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float lx = left + 8, rx = right - 8;
        const float origin = top - scroll;
        if (showing_recent && recent_block > 0) {
            float y = origin + 8;
            LabelC(Text(StringId::SearchRecent), {lx + 12, y, rx, y + 20}, 11.5f, p.faint);
            y += 26;
            float x = lx + 12;
            for (size_t i = 0; i < context.recent_queries.size(); ++i) {
                const auto& text = context.recent_queries[i];
                const float w = std::min(220.0f, TextWidth(text, 12.5f) + 40);
                if (x + w > rx - 8) break;
                const D2D1_RECT_F chip{x, y, x + w, y + 28};
                AddHot(chip, HotRecentQuery, static_cast<int>(i));
                FillC(chip, Hovered(HotRecentQuery, static_cast<int>(i)) ? p.hover : p.chip, 14);
                CommandIcon(L"\xE81C", {x + 10, y + 7, x + 24, y + 21}, 11, p.faint);
                LabelC(text, {x + 28, y, x + w - 10, y + 28}, 12.5f, p.muted);
                x += w + 6;
            }
        }
        if (rows.empty() && groups.empty()) {
            DrawEmpty(left, right, top + recent_block, bottom, p);
        } else {
            // Selection plate under the rows.
            if (ResultSelectionActive()) {
                const float y = origin + row_top[static_cast<size_t>(selected)];
                const D2D1_RECT_F target_rect{lx, y, rx, y + RowHeight()};
                // The high-contrast foreground changes immediately with the selected row.
                plate_motion.SetAnimationsEnabled(animations && !ui::IsHighContrast());
                const auto plate = plate_motion.Update(layout_epoch, selected, target_rect, frame, now, ui::motion::spring::Plate);
                FillC(plate, p.sel, 7);
                StrokeC(plate, p.sel_stroke, 7);
                FillC({plate.left, plate.top + 14, plate.left + 3, plate.bottom - 14}, p.accent, 1.5f);
            } else plate_motion.Hide();
            for (size_t g = 0; g < groups.size(); ++g) {
                const auto& group = groups[g];
                if (group.header) {
                    const float gy = origin + group_top[g];
                    if (gy + kGroupRow >= top && gy <= bottom) DrawGroupHeader(g, gy, lx, rx, top, bottom, p);
                }
                for (size_t i = group.start; i < group.start + group.count; ++i) {
                    float y = origin + row_top[i];
                    if (y + RowHeight() < top || y > bottom) continue;
                    float alpha = 1.0f;
                    // Search results are actionable as soon as they arrive.
                    AddHot({lx, std::max(top, y), rx, std::min(bottom, y + RowHeight())}, HotRow, static_cast<int>(i));
                    if (stale) alpha *= 0.45f;
                    if (alpha > 0.01f) DrawRow(i, y, lx, rx, p, alpha);
                }
            }
        }
        target->PopAxisAlignedClip();
        if (MaxScroll() > 0) {
            const float track = bottom - top - 12;
            const float thumb = std::max(24.0f, track * (bottom - top) / content_height);
            const float y = top + 6 + (track - thumb) * scroll / MaxScroll();
            FillC({right - 5, y, right - 2, y + thumb}, WithAlpha(p.muted, 0.45f), 1.5f);
        }
        // toast
        if (!toast.empty() && now < toast_until) {
            const float tw = TextWidth(toast, 12.5f) + 44;
            const float cx = (left + right) / 2;
            const D2D1_RECT_F pill{cx - tw / 2, bottom - 46, cx + tw / 2, bottom - 14};
            FillC(pill, D2D1::ColorF(0x15161b, 0.94f), 16);
            CommandIcon(L"\xE73E", {pill.left + 14, pill.top + 9, pill.left + 30, pill.bottom - 9}, 12, D2D1::ColorF(0x7fd6a0));
            LabelC(toast, {pill.left + 34, pill.top, pill.right - 10, pill.bottom}, 12.5f, D2D1::ColorF(1, 1, 1, 1));
        }
    }
    void DrawPreview(const Palette& p) {
        const float pw = PreviewWidth();
        if (pw <= 0) return;
        const float x0 = width - pw, top = Header() + 1, bottom = ListBottom();
        FillC({x0, top, x0 + 1, bottom}, p.line);
        if (command_mode) return;
        if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto& row = rows[static_cast<size_t>(selected)];
        const float l = x0 + 16, r = width - 16;
        const auto thumb_box = PreviewContentBounds();
        target->PushAxisAlignedClip({x0, top, width, bottom - 94}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (row.app) {
            FillC(thumb_box, p.chip, 9);
            const float cx = (thumb_box.left + thumb_box.right) / 2, cy = (thumb_box.top + thumb_box.bottom) / 2;
            icons.Draw(compositor.Dc(), {cx - 24, cy - 24, cx + 24, cy + 24}, row.path, row.name, false, 0);
        }
        float y = thumb_box.bottom + 14;
        LabelC(row.name, {l, y, r, y + 44}, 15, p.text, true, L"Segoe UI", !showing_recent, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        y += TextWidth(row.name, 15, true) > r - l ? 46 : 24;
        if (!row.app) {
            LabelC(path::StripExtendedPathPrefix(row.path), {l, y, r, y + 36}, 12, p.faint, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            y += 44;
        } else {
            y += 8;
        }
        auto meta = [&](StringId label, const std::wstring& value) {
            if (value.empty()) return;
            LabelC(Text(label), {l, y, l + 72, y + 20}, 12, p.faint);
            LabelC(value, {l + 76, y, r, y + 20}, 12, p.muted);
            y += 22;
        };
        meta(StringId::ColumnType, row.app ? Text(StringId::GlobalSearchGroupApps) : TypeName(row));
        if (!row.directory && (row.size || row.mtime)) meta(StringId::ColumnSize, FormatSize(row.size));
        meta(StringId::ModifiedTime, FormatTime(row.mtime));
        target->PopAxisAlignedClip();
        // actions
        const float b = bottom - 16;
        const D2D1_RECT_F location{l, b - 32, (l + r) / 2 - 3, b};
        const D2D1_RECT_F copy{(l + r) / 2 + 3, b - 32, r, b};
        // A program has no location or path to copy: just 打开.
        const D2D1_RECT_F open = row.app ? D2D1_RECT_F{l, b - 32, r, b} : D2D1_RECT_F{l, b - 70, r, b - 38};
        AddHot(open, HotPreviewOpen);
        FillC(open, Hovered(HotPreviewOpen) ? Theme().accent_hover : p.accent, 7);
        LabelC(Text(row.recycle_path.empty() ? StringId::Open : StringId::RecycleBin), open, 13, D2D1::ColorF(1, 1, 1, 1), false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (row.app) return;
        AddHot(location, HotPreviewLocation); AddHot(copy, HotPreviewCopy);
        for (auto [rect, label, kind] : {std::tuple{location, StringId::HintActOpenPath, HotPreviewLocation}, std::tuple{copy, StringId::CopyPath, HotPreviewCopy}}) {
            FillC(rect, Hovered(kind) ? p.hover : p.chip, 7);
            StrokeC(rect, p.line, 7);
            LabelC(Text(label), rect, 12.5f, p.text, false, L"Segoe UI", false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    void DrawFooter(const Palette& p) {
        const float bottom = ListBottom(), mid = bottom + kFooter / 2;
        FillC({0, bottom, width, bottom + 1}, p.line);
        float x = 20;
        const bool waiting = busy || (providers.network_expected && !providers.network_ready && !content_mode && HasSearchQuery());
        if (HasSearchQuery() || !browse_folder.empty()) {
            FillC({x, mid - 3, x + 6, mid + 3}, waiting ? D2D1::ColorF(0xe6b44c) : D2D1::ColorF(0x46c27a), 3);
            x += 14;
        }
        std::wstring status = !HasSearchQuery() && browse_folder.empty() ? std::wstring{} : FooterStatus();
        if (HasSearchQuery() && !busy && error.empty() && !content_mode) {
            size_t net = 0;
            for (const auto& row : rows) net += row.network ? 1 : 0;
            if (net) status += L" \x00b7 " + Text(StringId::GlobalSearchLocal) + L" " + std::to_wstring(rows.size() - net) +
                L" \x00b7 " + Text(StringId::GlobalSearchNetwork) + L" " + std::to_wstring(net);
        }
        if (truncated) status += L" \x00b7 " + Text(StringId::GlobalSearchTruncatedShort);
        float right = width - 16;
        if (events.staged_paths) {
            const auto staged = events.staged_paths();
            const auto title = Text(StringId::TrayShelfTitle) + L" " + std::to_wstring(staged.size());
            const float w = TextWidth(title, 12) + 20;
            const D2D1_RECT_F rect{right - w, mid - 14, right, mid + 14};
            AddHot(rect, HotTray); FillC(rect, tray_open ? p.sel : p.chip, 7);
            LabelC(title, {rect.left + 10, rect.top, rect.right - 10, rect.bottom}, 12, p.muted);
            right = rect.left - 10;
        }
        if (!marked_paths.empty()) {
            status = std::to_wstring(marked_paths.size()) + L" " + Text(StringId::GlobalSearchSelect);
            const float w = TextWidth(Text(StringId::CtxAddToTray), 12) + 20;
            if (right - w > x + 100) {
                D2D1_RECT_F rect{right - w, mid - 14, right, mid + 14};
                AddHot(rect, HotStage); FillC(rect, p.chip, 7);
                LabelC(Text(StringId::CtxAddToTray), {rect.left + 10, rect.top, rect.right - 10, rect.bottom}, 12, p.muted);
                right = rect.left - 10;
            }
        }
        handoff_rect = {};
        if (handoff) {
            const auto& label = Text(StringId::GlobalSearchHandoff);
            const float lw = TextWidth(label, 12);
            const float kw = std::max(22.0f, TextWidth(L"Shift \x21B5", 11) + 12);
            const D2D1_RECT_F button{right - lw - kw - 22, mid - 14, right + 6, mid + 14};
            handoff_rect = button;
            if (handoff_hover) FillC(button, p.sel, 6);
            KeyHint(L"Shift \x21B5", label, right - 4, mid, handoff_hover ? p.accent_text : p.muted, p.key);
            right = button.left - 14;
        }
        const float status_w = TextWidth(status, 12);
        const float status_right = x + status_w;
        struct Hint { const wchar_t* key; StringId label; };
        const Hint hints[] = {{L"Tab", content_mode ? StringId::SearchModeName : StringId::SearchModeContent},
                              {L"Space", StringId::Preview}, {L"\x2191\x2193", StringId::GlobalSearchSelect}};
        for (const auto& hint : hints) {
            const float need = TextWidth(Text(hint.label), 12) + std::max(22.0f, TextWidth(hint.key, 11) + 12) + 6;
            if (right - need < status_right + 24) break;
            right = KeyHint(hint.key, Text(hint.label), right, mid, p.muted, p.key) - 14;
        }
        LabelC(status, {x, mid - 10, std::min(right, x + status_w + 2), mid + 10}, 12, p.muted);
    }
    void DrawTray(const Palette& p) {
        if (!tray_open || !events.staged_paths) return;
        const auto paths = events.staged_paths();
        const float right = width - 14, left = std::max(14.0f, right - 310);
        const size_t visible = std::min(paths.size(), static_cast<size_t>(std::max(1.0f, (height - 180) / 32)));
        tray_offset = std::min(tray_offset, paths.size() - visible);
        const float bottom = ListBottom() - 8, top = bottom - 82 - static_cast<float>(visible) * 32;
        tray_bounds = {left, top, right, bottom}; AddHot(tray_bounds, HotTrayPanel);
        FillC({left, top, right, bottom}, dark ? Color(0x2b2c32) : Color(0xffffff), 10); StrokeC({left, top, right, bottom}, p.line, 10);
        LabelC(Text(StringId::TrayShelfTitle) + L" " + std::to_wstring(paths.size()), {left + 12, top + 6, right - 12, top + 34}, 13, p.text, true);
        for (size_t i = 0; i < visible; ++i) {
            const size_t index = tray_offset + i;
            const float y = top + 38 + static_cast<float>(i) * 32;
            AddHot({left + 8, y, right - 38, y + 30}, HotTrayItem, static_cast<int>(index));
            AddHot({right - 34, y, right - 8, y + 30}, HotTrayRemove, static_cast<int>(index));
            LabelC(LeafName(paths[index]), {left + 12, y, right - 38, y + 30}, 12, p.text);
            LabelC(L"\x00d7", {right - 30, y, right - 8, y + 30}, 14, p.muted);
        }
        if (paths.empty()) LabelC(Text(StringId::StagingTrayEmpty), {left + 12, top + 34, right - 12, bottom - 10}, 12, p.faint);
        else {
            AddHot({left + 10, bottom - 34, right - 10, bottom - 6}, HotTrayClear);
            LabelC(Text(StringId::TrayMenuClear), {left + 12, bottom - 34, right - 12, bottom - 6}, 12, p.muted);
        }
    }
    void Paint() {
        PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint);
        Render();
        EndPaint(hwnd, &paint);
    }
    void Render() {
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("enter");
#endif
        SyncPreview();
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("sync-preview-done");
#endif
        if (EnsureTarget()) {
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("target-ready");
#endif
            const auto theme = Theme();
            const auto p = MakePalette(theme);
            const uint64_t now = ui::motion::NowMs();
            ++frame;
            hot.clear();
            if (animations && last_tick) {
                const float dt = std::min(64.0f, static_cast<float>(now - last_tick)) / 1000.0f;
                scroll += (scroll_target - scroll) * std::min(1.0f, dt * 18.0f);
                if (std::fabs(scroll_target - scroll) < 0.5f) scroll = scroll_target;
            } else scroll = scroll_target;
            last_tick = now;
            target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0.0f));
            const bool live = target.Get() == compositor.Dc();
            auto tint = theme.bg;
            if (live && !ui::IsHighContrast() && backdrop && compositor.UsesTransparentComposition()) tint.a = dark ? 0.52f : 0.62f;
            brush->SetColor(tint); target->FillRectangle({0, 0, width, height}, brush.Get());
            DrawHeader(p);
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("header-done");
#endif
            DrawSide(p);
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("side-done");
#endif
            DrawList(p, now);
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("list-done");
#endif
            DrawPreview(p);
            DrawFooter(p);
            DrawTray(p);
            DrawScopeTooltip(theme);
            const HRESULT rendered = target->EndDraw();
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("enddraw-done");
#endif
            if (FAILED(rendered)) {
                compositor.NotifyDeviceLost(rendered); empty_art.Reset(); empty_art_target = nullptr; logo.Reset(); logo_target = nullptr;
                icons.Reset(); brush.Reset(); target.Reset();
            } else if (live) {
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STATUS
                const HRESULT present_status = compositor.Present();
                PULSE_ANIMATION_SEARCH_TRACE_STATUS(present_status);
#else
                compositor.Present();
#endif
            }
#ifdef PULSE_ANIMATION_SEARCH_TRACE_STAGE
PULSE_ANIMATION_SEARCH_TRACE_STAGE("present-done");
#endif
            const bool moving = plate_motion.Active(now) || scope_motion.Active(now) ||
                std::fabs(scroll_target - scroll) >= 0.5f || busy || (!toast.empty() && now < toast_until + 20);
            if (moving && IsWindowVisible(hwnd) && !IsIconic(hwnd)) SetTimer(hwnd, kAnimationTimer, 10, nullptr);
            else KillTimer(hwnd, kAnimationTimer);
        }
    }
    const HotSpot* HitAt(float x, float y) const {
        for (auto it = hot.rbegin(); it != hot.rend(); ++it) if (Contains(it->rect, x, y)) return &*it;
        return nullptr;
    }
    void UpdateHover(float x, float y) {
        const auto* spot = HitAt(x, y);
        const Hot kind = spot ? spot->kind : HotNone;
        const int index = spot ? spot->index : -1;
        scope_pointer_x = x; scope_pointer_y = y;
        if (kind != hover_kind || index != hover_index) {
            hover_kind = kind; hover_index = index;
            HideScopeTooltip();
            if (!TooltipText().empty()) SetTimer(hwnd, kScopeTooltipTimer, 450, nullptr);
            Invalidate();
        }
        if (const bool over = InHandoff(x, y); over != handoff_hover) { handoff_hover = over; Invalidate(); }
    }
    void Click(float x, float y) {
        HideScopeTooltip();
        if (InHandoff(x, y)) { handoff_hover = false; Handoff(); return; }
        const auto* spot = HitAt(x, y);
        if (!spot) return;
        const int index = spot->index;
        switch (spot->kind) {
        case HotRow:
            if (GetKeyState(VK_SHIFT) & 0x8000) MarkRange(selected, index);
            selected = index; keyboard_nav = true;
            if (GetKeyState(VK_CONTROL) & 0x8000) ToggleMarked();
            ClampSelection(); Invalidate(); break;
        case HotView: grid_view = index != 0; scroll = scroll_target = 0; ++layout_epoch; BuildLayout(); Invalidate(); break;
        case HotTileSize: tile_size_dragging = true; SetCapture(hwnd); UpdateTileSize(x); break;
        case HotBack: BackFolder(); break;
        case HotExitFolder: browse_folder.clear(); browse_history.clear(); Changed(); break;
        case HotBreadcrumb:
            if (index >= 0 && index < static_cast<int>(breadcrumb_paths.size()) && breadcrumb_paths[static_cast<size_t>(index)] != browse_folder) {
                browse_history.push_back(browse_folder); browse_folder = breadcrumb_paths[static_cast<size_t>(index)]; Changed();
            }
            break;
        case HotStage: StageSelection(); break;
        case HotTray: tray_open = !tray_open; Invalidate(); break;
        case HotTrayClear: if (events.clear_staging) events.clear_staging(); Invalidate(); break;
        case HotTrayRemove:
        case HotTrayItem:
            if (events.staged_paths) {
                const auto paths = events.staged_paths();
                if (index >= 0 && index < static_cast<int>(paths.size())) {
                    const auto path = paths[static_cast<size_t>(index)];
                    if (spot->kind == HotTrayRemove) { if (events.unstage_path) events.unstage_path(path); }
                    else {
                        const auto route = events.open_path; Hide();
                        if (!route || !route(path, false, true)) OpenResult(path, true);
                        return;
                    }
                }
            }
            Invalidate(); break;
        case HotCalculation: keyboard_nav = false; CopyCalculation(false); Invalidate(); break;
        case HotAlternative:
            if (calculation && index >= 0 && index < static_cast<int>(calculation->alternatives.size())) {
                const auto text = calculation->alternatives[static_cast<size_t>(index)].query;
                SetWindowTextW(edit, text.c_str());
            }
            break;
        case HotToken:
            if (index >= 0 && index < static_cast<int>(parsed_query.tokens.size())) {
                const auto token = parsed_query.tokens[static_cast<size_t>(index)];
                auto text = raw_query; text.erase(token.start, token.length); SetWindowTextW(edit, text.c_str());
            }
            break;
        case HotTimeline:
            if (timeline_first == index && timeline_last == index) {
                timeline_first = timeline_last = timeline_anchor = -1; Changed();
            } else { timeline_anchor = timeline_first = timeline_last = index; SetCapture(hwnd); Invalidate(); }
            break;
        case HotScope: SetScope(index); break;
        case HotAddPlace: PickPlace(); break;
        case HotKind:
            kind_filter = kind_filter == index ? 0 : index; ext_filter.clear();
            if (kKinds[kind_filter] == index::SearchKind::Folder) size_filter = 0;
            Changed(); break;
        case HotSize: size_filter = size_filter == index ? 0 : index; Changed(); break;
        case HotExt:
            if (index >= 0 && index < static_cast<int>(ext_counts.size())) {
                const auto& ext = ext_counts[static_cast<size_t>(index)].first;
                ext_filter = ext_filter == ext ? std::wstring{} : ext; Changed();
            }
            break;
        case HotSort: SortMenu(); break;
        case HotGroupMenu: GroupMenu(); break;
        case HotGroupHeader: ToggleGroup(static_cast<size_t>(index)); break;
        case HotGroupSelect: MarkGroup(static_cast<size_t>(index)); break;
        case HotDate: date_filter = date_filter == index ? 0 : index; Changed(); break;
        case HotMode: SetMode(!content_mode); break;
        case HotSearchScope: SearchScopeMenu(); break;
        case HotSearchClear:
            if (GetWindowTextLengthW(edit) > 0) SetWindowTextW(edit, L""); else Hide();
            break;
        case HotSearchField: SetFocus(edit); break;
        case HotEsc: Hide(); return;
        case HotPreviewOpen:
            if (selected >= 0 && selected < static_cast<int>(rows.size()) && rows[static_cast<size_t>(selected)].directory) EnterFolder();
            else Open(false);
            return;
        case HotPreviewLocation: Open(true); return;
        case HotPreviewCopy: CopyPath(); break;
        case HotRecentQuery:
            if (index >= 0 && index < static_cast<int>(context.recent_queries.size())) {
                SetWindowTextW(edit, context.recent_queries[static_cast<size_t>(index)].c_str());
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }
            break;
        case HotClearFilters: {
            scope = 0; kind_filter = 0; date_filter = 0; timeline_first = timeline_last = -1; size_filter = 0; ext_filter.clear();
            auto text = raw_query;
            for (auto it = parsed_query.tokens.rbegin(); it != parsed_query.tokens.rend(); ++it) text.erase(it->start, it->length);
            SetWindowTextW(edit, text.c_str()); break;
        }
        case HotSearchContent: SetMode(!content_mode); break;
        default: break;
        }
    }
    LRESULT HitTestFrame(LPARAM lp) const {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        RECT rc{}; GetClientRect(hwnd, &rc);
        const int b = Px(kResizeBorder);
        const bool left = pt.x < b, right = pt.x >= rc.right - b, top = pt.y < b, bottom = pt.y >= rc.bottom - b;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        const float x = pt.x / scale, y = pt.y / scale;
        if (y < Header() && !Contains(FieldRect(), x, y) && !HitAt(x, y)) return HTCAPTION;
        return HTCLIENT;
    }
    LRESULT Message(UINT message, WPARAM wp, LPARAM lp) {
        if (message == WM_NCACTIVATE || message == WM_ACTIVATE || message == WM_ACTIVATEAPP ||
            message == WM_SHOWWINDOW || message == WM_SETFOCUS || message == WM_KILLFOCUS)
            Trace("search_window_message", (uint64_t(message) << 32) | static_cast<uint32_t>(wp));
        switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCCALCSIZE: if (wp) return 0; break;
        case WM_NCHITTEST: return HitTestFrame(lp);
        case WM_NCACTIVATE:
            // Like KeepFrostedProc for dialogs, keep the acrylic frame active.
            // Attention flashes must not alternate the whole popup's backdrop;
            // real application deactivation is still handled by WM_ACTIVATEAPP.
            return DefWindowProcW(hwnd, message, TRUE, -1);
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {Px(kMinWidth), Px(kMinHeight)};
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            RECT rect{};
            if (GetWindowRect(hwnd, &rect)) {
                context.positioned = true;
                context.x = rect.left; context.y = rect.top;
                context.width = static_cast<int>(std::lround(width));
                context.height = static_cast<int>(std::lround(height));
                if (events.placement_changed)
                    events.placement_changed(rect.left, rect.top, context.width, context.height);
            }
            SetFocus(edit);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (known_folders::IsSettingsArea(reinterpret_cast<const wchar_t*>(lp))) {
                if (IsWindowVisible(hwnd)) {
                    BuildScopes();
                    Changed();
                }
                return 0;
            }
            [[fallthrough]];
        case WM_THEMECHANGED:
            ui::RefreshHighContrast();
            ApplyAppearance(); return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplyAppearance(); return 0;
        case WM_SIZE: if (edit) Layout(); return 0;
        case WM_DPICHANGED: {
            scale = static_cast<float>(HIWORD(wp)) / 96.0f;
            icons.SetScale(scale);
            const auto* rect = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(); return 0;
        }
        case WM_CLOSE: Hide(); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) {
                // WA_ACTIVE can describe only our background input queue.
                // Do not record foreground acquisition after a rejected request.
                const HWND foreground = GetForegroundWindow();
                DWORD process = 0;
                if (foreground) GetWindowThreadProcessId(foreground, &process);
                if (foreground == hwnd || (foreground && process == GetCurrentProcessId() &&
                    GetAncestor(foreground, GA_ROOTOWNER) == hwnd)) activation.Activated();
                break;
            }
            // Same-process windows do not send WM_ACTIVATEAPP(false).
            [[fallthrough]];
        case WM_ACTIVATEAPP:
            if ((message == WM_ACTIVATE || !wp) && activation.Open()) {
                activation.Deactivated();
                // Activation APIs may synchronously reenter this procedure.
                // Show posts once its stack is complete; do not post from Opening.
                if (activation.Visibility() != app::SearchVisibility::Opening)
                    PostMessageW(hwnd, kActivationReconcile, activation.Generation(), 0);
            }
            return 0;
        case kActivationReconcile: {
            const HWND foreground = GetForegroundWindow();
            DWORD process = 0;
            if (foreground) GetWindowThreadProcessId(foreground, &process);
            // Owned dialogs/menus are part of this interaction, unlike another
            // Pulse main window. Include nested owned popups in that boundary.
            const bool owned_popup = foreground && process == GetCurrentProcessId() &&
                GetAncestor(foreground, GA_ROOTOWNER) == hwnd;
            const bool dismiss = activation.Reconcile(static_cast<uintptr_t>(wp), foreground == hwnd,
                owned_popup,
                foreground && foreground != opening_foreground);
            Trace("search_activation_reconcile", dismiss);
            if (dismiss) Hide();
            return 0;
        }
        case WM_SETFOCUS: SetFocus(edit); return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: if (Key(wp)) return 0; break;
        case WM_COMMAND: if (reinterpret_cast<HWND>(lp) == edit && HIWORD(wp) == EN_CHANGE) Changed(); return 0;
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp); SetBkColor(dc, Background());
            SetTextColor(dc, ui::HcEditText(dark ? RGB(245, 245, 247) : RGB(32, 33, 36)));
            return reinterpret_cast<LRESULT>(ui::EditBackBrush(background));
        }
        case WM_TIMER:
            if (wp == kDiagnosticTimer) {
                const HWND current = GetForegroundWindow();
                if (current != diagnostic_foreground) {
                    diagnostic_foreground = current;
                    Trace("search_foreground_changed");
                }
                if (GetTickCount64() >= diagnostic_until) {
                    KillTimer(hwnd, kDiagnosticTimer);
                    diagnostic_until = 0;
                }
                return 0;
            }
            if (wp == kDebounce) Search();
            else if (wp == kConnectTimeout) {
                KillTimer(hwnd, kConnectTimeout);
                if (busy && !content_mode) TimeoutFilenames();
            } else if (wp == kScopeTooltipTimer) {
                KillTimer(hwnd, kScopeTooltipTimer);
                if (!TooltipText().empty()) { scope_tooltip_visible = true; Invalidate(); }
            } else if (wp == kAnimationTimer) {
                if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) KillTimer(hwnd, kAnimationTimer);
                else Invalidate();
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (tile_size_dragging && (wp & MK_LBUTTON)) UpdateTileSize(static_cast<float>(GET_X_LPARAM(lp)) / scale);
            if (timeline_anchor >= 0 && (wp & MK_LBUTTON)) {
                const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale;
                const int index = std::clamp(static_cast<int>((x - timeline_rect.left) * 12 / (timeline_rect.right - timeline_rect.left)), 0, 11);
                timeline_first = std::min(timeline_anchor, index); timeline_last = std::max(timeline_anchor, index); Invalidate();
            }
            if (row_drag_pending && (wp & MK_LBUTTON)) {
                const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (std::abs(point.x - row_drag_start.x) >= GetSystemMetrics(SM_CXDRAG) ||
                    std::abs(point.y - row_drag_start.y) >= GetSystemMetrics(SM_CYDRAG)) row_dragging = true;
            }
            if (!tracking_mouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                tracking_mouse = TrackMouseEvent(&tracking) != FALSE;
            }
            UpdateHover(static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse = false;
            HideScopeTooltip();
            if (handoff_hover || hover_kind != HotNone) { handoff_hover = false; hover_kind = HotNone; hover_index = -1; Invalidate(); }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && (handoff_hover || (hover_kind != HotNone && hover_kind != HotRow))) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            break;
        case WM_LBUTTONDOWN:
            Click(static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale);
            if (const auto* spot = HitAt(static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale);
                spot && spot->kind == HotRow) {
                row_drag_pending = true; row_drag_start = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; SetCapture(hwnd);
            }
            if (IsWindowVisible(hwnd)) SetFocus(edit);
            return 0;
        case WM_LBUTTONUP:
            if (tile_size_dragging) { UpdateTileSize(static_cast<float>(GET_X_LPARAM(lp)) / scale); tile_size_dragging = false; Invalidate(); }
            if (timeline_anchor >= 0) { timeline_anchor = -1; Changed(); }
            if (row_dragging) {
                const auto* spot = HitAt(static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale);
                if (spot && spot->kind == HotTray) StageSelection();
            }
            row_drag_pending = row_dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED: row_drag_pending = row_dragging = tile_size_dragging = false; timeline_anchor = -1; break;
        case WM_CONTEXTMENU: {
            POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (screen.x == -1 && screen.y == -1) { screen = {Px(ListLeft() + 80), Px(ListTop() + 40)}; ClientToScreen(hwnd, &screen); }
            else {
                POINT client = screen; ScreenToClient(hwnd, &client);
                const auto* spot = HitAt(static_cast<float>(client.x) / scale, static_cast<float>(client.y) / scale);
                if (!spot || spot->kind != HotRow) return 0;
                selected = spot->index; keyboard_nav = true; Invalidate();
            }
            ResultMenu(screen); return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const auto* spot = HitAt(static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale);
            if (spot && spot->kind == HotRow) { selected = spot->index; Open(false); }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(hwnd, &point);
            if (tray_open && Contains(tray_bounds, point.x / scale, point.y / scale)) {
                const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                tray_offset = static_cast<size_t>(std::max(0, static_cast<int>(tray_offset) - steps * 3)); Invalidate(); return 0;
            }
            if (PreviewWidth() > 0 && point.x / scale >= ListRight()) {
                if (preview.visible()) SendMessageW(preview.hwnd(), message, wp, lp);
                return 0;
            }
            if (SideWidth() > 0 && point.x / scale < SideWidth() && point.y / scale > Header()) {
                const float view = ListBottom() - Header() - 1;
                side_scroll = std::clamp(side_scroll - static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 60.0f,
                    0.0f, std::max(0.0f, side_content - view));
                Invalidate(); return 0;
            }
            scroll_target -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * RowHeight() * 2.5f;
            ClampScroll();
            if (!animations) scroll = scroll_target;
            Invalidate(); return 0;
        }
        case kDirectoryResult:
            if (wp == generation && directory_request) {
                std::lock_guard lock(directory_request->mutex);
                rows = std::move(directory_request->rows); error = directory_request->error;
                arranged = false; merged_rows.clear();
                busy = false; total = rows.size(); selected = rows.empty() ? -1 : 0;
                folder_count = static_cast<size_t>(std::count_if(rows.begin(), rows.end(), [](const Row& row) { return row.directory; }));
                ++layout_epoch; BuildLayout(); Invalidate();
            }
            return 0;
        case kIndexStatus: Invalidate(); return 0;
        case kPreviewNavigate:
            if (preview.visible() && !stale) {
                selected += static_cast<INT_PTR>(wp) < 0 ? -1 : 1;
                ClampSelection(); keyboard_nav = true; Invalidate();
            }
            return 0;
        case kPreviewOpen: {
            auto child = preview.TakeOpenPath();
            if (!preview.visible() || stale) return 0;
            if (wp && !child.empty()) {
                Hide(); OpenResult(std::move(child), false);
            } else Open(false);
            return 0;
        }
        case WM_SHOWWINDOW:
            if (!wp) { KillTimer(hwnd, kAnimationTimer); ClosePreview(); }
            else Invalidate();
            break;
        case kAppsReady:
            apps = *app_catalog.Apps();
            if (IsWindowVisible(hwnd) && !query.empty() && !content_mode && !showing_recent && !providers.Busy()) MergeFilenames();
            return 0;
        case kRecycleResult: {
            if (static_cast<uint32_t>(wp) != generation || content_mode || !recycle_request ||
                recycle_request->cancelled) return 0;
            { std::lock_guard lock(recycle_request->mutex); recycle_result = std::move(recycle_request->result); }
            recycle_ready = true;
            MergeFilenames();
            return 0;
        }
        case kIndexResult: {
            index::SearchResult result;
            if (filenames.TakeResult(static_cast<uint32_t>(wp), result) && IsWindowVisible(hwnd)) {
                AcceptFilenames(false, static_cast<uint32_t>(wp), std::move(result));
            }
            return 0;
        }
        case kNetworkResult: {
            index::SearchResult result;
            if (network.TakeResult(static_cast<uint32_t>(wp), result) && IsWindowVisible(hwnd)) {
                AcceptFilenames(true, static_cast<uint32_t>(wp), std::move(result));
            }
            return 0;
        }
        case kLiveResult: {
            if (static_cast<uint32_t>(wp) == generation && !content_mode && live_request && IsWindowVisible(hwnd)) {
                index::SearchResult result;
                bool done = false;
                {
                    std::lock_guard lock(live_request->mutex);
                    result = index::SelectLiveNetworkHits(live_request->query, live_request->matches);
                    done = live_request->matches.finished;
                }
                network_result = std::move(result);
                if (done) providers.Accept(true, network_result.error);
                MergeFilenames();
            }
            return 0;
        }
        case kContentResult: {
            index::ContentSearchUpdate update;
            while (contents.TakeUpdate(update)) {
                if (update.progress.generation != generation || !content_mode || !IsWindowVisible(hwnd)) continue;
                if (stale) { stale = false; rows.clear(); merged_rows.clear(); selected = 0; scroll = scroll_target = 0; }
                if (!arranged) merged_rows.clear();
                jump_count = app_count = 0;
                const bool had_rows = !merged_rows.empty();
                std::wstring selected_path;
                if (selected >= 0 && selected < static_cast<int>(rows.size())) selected_path = rows[static_cast<size_t>(selected)].path;
                for (auto& hit : update.hits) {
                    if (merged_rows.size() >= kMaximumResults) break;
                    Row row{std::move(hit.name), std::move(hit.path), std::move(hit.snippet), false};
                    const auto lead = row.snippet.find_first_not_of(L" \t\r\n");
                    row.snippet.erase(0, lead == std::wstring::npos ? row.snippet.size() : lead);
                    row.network = IsNetworkPath(row.path);
                    if (!row.network) {
                        WIN32_FILE_ATTRIBUTE_DATA data{};
                        if (GetFileAttributesExW(row.path.c_str(), GetFileExInfoStandard, &data)) {
                            row.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                            row.mtime = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
                        }
                    }
                    row.cls = app::GlobalSearchRowClass::Content;
                    merged_rows.push_back(std::move(row));
                }
                Arrange();
                // Streamed hits may land above the selection in a sorted list.
                for (size_t i = 0; i < rows.size(); ++i) if (!selected_path.empty() && rows[i].path == selected_path) { selected = static_cast<int>(i); break; }
                total = merged_rows.size(); truncated = update.progress.truncated;
                busy = !update.progress.done;
                std::wstring pack_title, pack_message;
                if (ContentPackBanner(update.progress.error, pack_title, pack_message)) error = std::move(pack_title);
                else if (update.progress.error && update.progress.error != ERROR_CANCELLED) error = Text(StringId::GlobalSearchFailed);
                if (!had_rows && !rows.empty()) { selected = 0; appear_start = ui::motion::NowMs(); }
                BuildLayout();
                ClampSelection(false); SyncPreview(); Invalidate();
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
    }
    struct ForegroundAttempt {
        bool direct_accepted = false, accepted = false, mouse_pulse_sent = false, retried = false;
        DWORD pulse_error = ERROR_SUCCESS;
    };
    static bool ForegroundInputReleased() {
        for (const int key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU,
                VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN,
                VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) {
            if (GetAsyncKeyState(key) & 0x8000) return false;
        }
        return true;
    }
    ForegroundAttempt RequestForeground(uintptr_t opening_generation) {
        ForegroundAttempt attempt;
        if (activation.Generation() != opening_generation) return attempt;
        const HWND foreground_before = GetForegroundWindow();
        Trace("search_foreground_direct_begin");
        attempt.direct_accepted = attempt.accepted = SetForegroundWindow(hwnd) != FALSE;
        Trace("search_foreground_direct_end", attempt.accepted);
        const HWND foreground_after = GetForegroundWindow();
        if (!app::ShouldPulseSearchForeground(activation.Trigger() == app::SearchTrigger::DoubleCtrl,
                attempt.direct_accepted, activation.Generation() == opening_generation,
                foreground_after == hwnd,
                foreground_after == foreground_before && foreground_before == opening_foreground,
                ForegroundInputReleased())) return attempt;
        // PowerToys Run uses an empty INPUT_MOUSE before activation. No movement,
        // button, wheel or keyboard flags are sent. UIPI may still reject it.
        // https://github.com/microsoft/PowerToys/blob/main/src/modules/launcher/PowerLauncher/MainWindow.xaml.cs
        // Keep this bounded to the current, fully released double-Ctrl gesture.
        INPUT input{};
        input.type = INPUT_MOUSE;
        Trace("search_foreground_mouse_begin");
        SetLastError(ERROR_SUCCESS);
        const UINT sent = SendInput(1, &input, sizeof(input));
        attempt.mouse_pulse_sent = sent == 1;
        attempt.pulse_error = sent == 1 ? ERROR_SUCCESS : GetLastError();
        diagnostics::runtime::Event("search_activation_mouse_pulse", {{"id", diagnostic_id},
            {"sent", sent}, {"error", attempt.pulse_error}, {"generation", opening_generation},
            {"foreground", reinterpret_cast<uintptr_t>(GetForegroundWindow())}});
        if (!attempt.mouse_pulse_sent || activation.Generation() != opening_generation ||
            GetForegroundWindow() != foreground_before || !ForegroundInputReleased()) return attempt;
        attempt.retried = true;
        attempt.accepted = SetForegroundWindow(hwnd) != FALSE;
        Trace("search_foreground_retry_end", attempt.accepted);
        return attempt;
    }
    bool Show(HWND owner, bool use_dark, float requested_scale, const std::wstring& folder, bool pinyin,
              const std::wstring& initial_query, app::SearchTrigger trigger) {
        const auto opening_generation = activation.Begin(trigger);
        opening_foreground = GetForegroundWindow();
        diagnostic_id = diagnostics::runtime::NextId();
        diagnostic_events = 0;
        const uint64_t show_started = GetTickCount64();
        uint64_t phase_started = show_started;
        // Keep slow opens diagnosable even after the short window-message trace expires.
        const auto phase = [&](uint64_t stage) {
            const uint64_t now = GetTickCount64();
            const uint64_t elapsed = now - phase_started;
            diagnostics::runtime::Event("search_show_phase", {{"id", diagnostic_id}, {"stage", stage},
                {"elapsed_ms", elapsed}, {"total_ms", now - show_started}},
                elapsed >= 1000 ? diagnostics::runtime::Level::Warning : diagnostics::runtime::Level::Info);
            phase_started = now;
        };
        diagnostic_until = diagnostics::runtime::Enabled() ? GetTickCount64() + 10000 : 0;
        diagnostic_foreground = GetForegroundWindow();
        Trace("search_show_begin");
        dark = use_dark; scale = requested_scale > 0 ? requested_scale : 1;
        search_pinyin = pinyin;
        animations = ui::motion::SystemAnimationsEnabled();
        plate_motion.SetAnimationsEnabled(animations); scope_motion.SetAnimationsEnabled(animations);
        // Each summon follows the pointer, not the monitor of the saved position.
        HMONITOR active_monitor = app::SearchSummonMonitor();
        const RECT work_area = app::SearchMonitorWorkArea(active_monitor);
        if (const float monitor_scale = MonitorScale(active_monitor); monitor_scale > 0) scale = monitor_scale;
        (void)owner;
        const bool folder_changed = folder != current_folder;
        current_folder = folder;
        current_folder_tip = path::StripExtendedPathPrefix(folder);
        if (folder.empty()) current_only = false;
        HideScopeTooltip();
        if (!Create()) {
            activation.Hide();
            diagnostics::runtime::Event("search_window_create_failed", {{"id", diagnostic_id},
                {"error", GetLastError()}, {"elapsed_ms", GetTickCount64() - show_started}},
                diagnostics::runtime::Level::Error);
            return false;
        }
        phase(1); // window and preview/search clients
        if (folder_changed && scope > 0 && scope < static_cast<int>(scopes.size()) && scopes[static_cast<size_t>(scope)].kind == ScopeKind::Current) scope = 0;
        Prepare();
        if (!initial_query.empty()) SetWindowTextW(edit, initial_query.c_str());
        phase(2); // context, application catalogue and appearance
        BOOL is_dark = dark; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(content_mode ? StringId::SearchContentHint : StringId::SearchNameHint).c_str()));
        const RECT place = PlacementForMonitor(active_monitor, work_area);
        SetWindowPos(hwnd, HWND_NOTOPMOST, place.left, place.top, place.right - place.left, place.bottom - place.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
        Layout();
        phase(3); // monitor placement and control layout
        Changed(true);
        phase(4); // recent items or query refresh
        Render();
        phase(5); // first frame
        if (activation.Generation() != opening_generation) return true;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (activation.Generation() != opening_generation) return true;
        phase(6); // native show
        // Respect the foreground lock. Joining another process's input queue
        // changes shared focus/key state and can reenter Hide during this call.
        const HWND active = GetForegroundWindow();
        const DWORD active_thread = active ? GetWindowThreadProcessId(active, nullptr) : 0;
        const auto foreground_attempt = RequestForeground(opening_generation);
        const bool activated = foreground_attempt.accepted;
        if (activation.Generation() != opening_generation) return true;
        const bool focused = GetForegroundWindow() == hwnd;
        if (focused) { activation.Activated(); SetFocus(edit); }
        diagnostics::runtime::Event("search_activation_attempt", {{"id", diagnostic_id},
            {"accepted", activated}, {"direct_accepted", foreground_attempt.direct_accepted},
            {"mouse_pulse_sent", foreground_attempt.mouse_pulse_sent}, {"pulse_error", foreground_attempt.pulse_error},
            {"retried", foreground_attempt.retried},
            {"foreground_thread", active_thread},
            {"trigger", static_cast<uint64_t>(trigger)}, {"activation_generation", opening_generation},
            {"elapsed_ms", GetTickCount64() - phase_started},
            {"active", reinterpret_cast<uintptr_t>(GetActiveWindow())},
            {"foreground", reinterpret_cast<uintptr_t>(GetForegroundWindow())}});
        phase(7); // foreground handoff
        if (!focused) {
            diagnostics::runtime::Event("search_activation_deferred", {{"id", diagnostic_id},
                {"window", reinterpret_cast<uintptr_t>(hwnd)}, {"visible", IsWindowVisible(hwnd) != FALSE},
                {"foreground", reinterpret_cast<uintptr_t>(GetForegroundWindow())},
                {"accepted", activated != FALSE}, {"attached", false}, {"foreground_thread", active_thread},
                {"elapsed_ms", GetTickCount64() - show_started}}, diagnostics::runtime::Level::Warning);
        }
        // Text carried in from elsewhere keeps being typed; a fresh summon selects the old query.
        if (initial_query.empty()) SendMessageW(edit, EM_SETSEL, 0, -1);
        else SendMessageW(edit, EM_SETSEL, initial_query.size(), initial_query.size());
        diagnostic_until = diagnostics::runtime::Enabled() ? GetTickCount64() + 10000 : 0;
        activation.CompleteOpening(opening_generation);
        PostMessageW(hwnd, kActivationReconcile, opening_generation, 0);
        Trace("search_show_end");
        if (diagnostic_until) SetTimer(hwnd, kDiagnosticTimer, 50, nullptr);
        return true;
    }
    static float MonitorScale(HMONITOR monitor) {
        float result = 0;
        using MonitorDpi = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (const HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
            const auto dpi = reinterpret_cast<MonitorDpi>(GetProcAddress(shcore, "GetDpiForMonitor"));
            UINT x = 96, y = 96;
            if (dpi && SUCCEEDED(dpi(monitor, 0, &x, &y))) result = static_cast<float>(x) / 96.0f;
            FreeLibrary(shcore);
        }
        return result;
    }
    RECT PlacementForMonitor(HMONITOR monitor, const RECT& work) const {
        const bool same_monitor = app::SearchRestorePosition(monitor, context.positioned, POINT{context.x, context.y});
        return Placement(work, same_monitor);
    }
    // Keep DIP size across monitors; restore coordinates only on their own monitor.
    RECT Placement(const RECT& work, bool restore_position = true) const {
        return app::SearchWindowPlacement(work, scale, context.positioned, {context.x, context.y},
            {context.width, context.height}, restore_position);
    }
    // Per show: scopes, the program list and the appearance.
    void Prepare() {
        BuildScopes();
        if (context.show_apps) {
            apps = *app_catalog.Apps();
            app_catalog.Refresh(hwnd, kAppsReady, 10ull * 60 * 1000);
        }
        icons.SetScale(scale);
        ApplyAppearance();
    }
    bool Create() {
        if (hwnd) return true;
        {
            WNDCLASSEXW cls{sizeof(cls)}; cls.style = CS_DBLCLKS;
            cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"Pulse.GlobalSearch";
            RegisterClassExW(&cls);
            // A hidden owner would hide/minimize this window too; the owner only chooses the monitor.
            // WS_THICKFRAME gives the edges a resize grip; WM_NCCALCSIZE keeps the whole window as client.
            if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"Pulse Search",
                WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME,
                0, 0, Px(kDefaultWidth), Px(kDefaultHeight), nullptr, nullptr, cls.hInstance, this)) return false;
            edit = ui::CreateChildEdit(hwnd);
            if (!edit) { DestroyWindow(hwnd); hwnd = nullptr; return false; }
            SetWindowTheme(edit, L"", L"");
            EnsureTarget();
            ShowWindow(edit, SW_SHOW);
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(edit, EM_SETLIMITTEXT, 2048, 0);
            filenames.Start(hwnd, kIndexStatus, kIndexResult);
            network.Start(hwnd, kIndexStatus, kNetworkResult);
            contents.Start(hwnd, kContentResult, index::ContentAgentMode::Instant);
        }
        return true;
    }
    // ---- pre-warming --------------------------------------------------------
    // The first summon paid for the window, the D2D device, DWM's first show
    // and a cold first frame (fonts, glyphs, icons): ~90 ms of ~130. They are
    // done ahead, once, while the user is not busy; the Start menu program
    // list starts loading too, so the first query already finds programs.
    // Thread timers carry no context; there is one popup per process.
    static inline Impl* prewarm_owner = nullptr;
    UINT_PTR prewarm_timer = 0;
    int prewarm_tries = 0;
    std::function<void()> prewarm_prepare;
    void SchedulePrewarm(uint32_t delay_ms, std::function<void()> prepare) {
        if (hwnd || prewarm_timer) return;
        prewarm_prepare = std::move(prepare);
        prewarm_owner = this;
        prewarm_timer = SetTimer(nullptr, 0, delay_ms, PrewarmTimer);
    }
    static void CALLBACK PrewarmTimer(HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        Impl* self = prewarm_owner;
        if (!self || self->prewarm_timer != id) return;
        self->prewarm_timer = 0;
        self->Prewarm();
    }
    void Prewarm() {
        if (hwnd) return;
        // Typing, clicking or dragging: try again in a moment.
        const bool busy_input = HIWORD(GetQueueStatus(QS_INPUT)) != 0 ||
            GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0;
        if (busy_input && ++prewarm_tries < 20) {
            prewarm_timer = SetTimer(nullptr, 0, 1000, PrewarmTimer);
            return;
        }
        if (prewarm_prepare) prewarm_prepare();  // context and appearance, as for a summon
        prewarm_prepare = nullptr;
        if (hwnd) return;  // summoned meanwhile
        HMONITOR monitor_handle = app::SearchSummonMonitor();
        const RECT work_area = app::SearchMonitorWorkArea(monitor_handle);
        if (const float monitor_scale = MonitorScale(monitor_handle); monitor_scale > 0) scale = monitor_scale;
        animations = ui::motion::SystemAnimationsEnabled();
        if (!Create()) return;
        Prepare();
        BOOL is_dark = dark; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        const RECT place = PlacementForMonitor(monitor_handle, work_area);
        // Cloaked: DWM composes the window once but never shows it, and it
        // is not activated, so focus stays where it is.
        BOOL cloak = TRUE;
        const bool cloaked = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
        SetWindowPos(hwnd, HWND_NOTOPMOST, place.left, place.top, place.right - place.left, place.bottom - place.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | (cloaked ? SWP_SHOWWINDOW : 0));
        Layout();
        Changed();  // the empty-query list, as on a summon
        Render();
        KillTimer(hwnd, kAnimationTimer);
        if (cloaked) {
            ShowWindow(hwnd, SW_HIDE);
            cloak = FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
        }
        Cancel();
    }
    ~Impl() {
        if (prewarm_timer) KillTimer(nullptr, prewarm_timer);
        if (prewarm_owner == this) prewarm_owner = nullptr;
        Cancel(); filenames.Stop(); network.Stop(); contents.Stop();
        grid_thumbnails.SetNotifyWindow(nullptr);
        ClosePreview();
        if (edit) RemoveWindowSubclass(edit, EditProc, 1);
        if (hwnd) DestroyWindow(hwnd);
        if (edit_font) DeleteObject(edit_font);
        if (background) DeleteObject(background);
    }
};
GlobalSearchWindow::GlobalSearchWindow() = default;
GlobalSearchWindow::~GlobalSearchWindow() = default;
bool GlobalSearchWindow::Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin,
                              const std::wstring& initial_query, app::SearchTrigger trigger) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->Show(owner, dark, scale, current_folder, search_pinyin, initial_query, trigger);
}
void GlobalSearchWindow::SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->dark = dark; impl_->effect = effect; impl_->background_image = background_image; impl_->accent_color = accent;
    impl_->ApplyAppearance();
}
void GlobalSearchWindow::SetHandoffHandler(GlobalSearchHandoffHandler handler) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->handoff = std::move(handler);
}
void GlobalSearchWindow::SetContext(GlobalSearchContext context) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->context = std::move(context);
    impl_->ApplyContextOrder();
}
void GlobalSearchWindow::SetEvents(GlobalSearchEvents events) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->events = std::move(events);
}
void GlobalSearchWindow::Hide() { if (impl_) impl_->Hide(); }
void GlobalSearchWindow::Prewarm(uint32_t delay_ms, std::function<void()> prepare) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->SchedulePrewarm(delay_ms, std::move(prepare));
}
void GlobalSearchWindow::Shutdown() { impl_.reset(); }
app::SearchVisibility GlobalSearchWindow::Visibility() const {
    return impl_ ? impl_->activation.Visibility() : app::SearchVisibility::Hidden;
}
bool GlobalSearchWindow::Visible() const { return impl_ && impl_->hwnd && IsWindowVisible(impl_->hwnd); }
bool GlobalSearchWindow::SavePresentedSnapshot(const std::wstring& path) const {
    return impl_ && ui::Compositor::FrameCopyEnabled() && impl_->compositor.FrameCopy() &&
        impl_->compositor.SaveSnapshot(path.c_str());
}
}
