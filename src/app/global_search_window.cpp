#include "global_search_window.h"
#include "global_search_result_state.h"
#include "app_catalog.h"
#include "folder_alias.h"
#include "jump_history.h"
#include "jump_match.h"
#include "../ui/FluentTokens.h"
#include "resource.h"
#include "../ui/empty_state_layout.h"
#include "../ui/fluent_components.h"
#include "../ui/typography.h"
#include "../ui/svg_bitmap.h"
#include "../ui/edit_host.h"
#include "../ui/shell_icons.h"
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
#include <cmath>
#include <condition_variable>
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
constexpr UINT kNetworkResult = WM_APP + 184, kThumbnailResult = WM_APP + 185;
constexpr UINT kAppsReady = WM_APP + 186;  // the Start menu app list is in
constexpr UINT_PTR kDebounce = 1;
constexpr UINT_PTR kConnectTimeout = 2;
constexpr UINT_PTR kScopeTooltipTimer = 3;
constexpr UINT_PTR kAnimationTimer = 4;
constexpr uint64_t kSession = 0x50554c534547534full;
// Layout in DIPs (pulse_global_search.html).
constexpr float kHeader = 72, kFooter = 42, kRow = 52, kContentRow = 78, kGroupRow = 30;
constexpr float kSide = 216, kPreview = 318, kResizeBorder = 6;
constexpr float kFieldLeft = 62, kFieldTop = 14, kFieldHeight = 44, kFieldRight = 70;
constexpr float kEditCornerRadius = 8;
constexpr float kMinWidth = 640, kMinHeight = 420, kDefaultWidth = 960, kDefaultHeight = 620;
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
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw)) && raw) out = raw;
    if (raw) CoTaskMemFree(raw);
    return out;
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
// Preview thumbnails come from the shell on a worker: thumbnail handlers and
// network paths can take a while. Only the newest request is served.
struct ThumbnailWorker {
    std::mutex mutex;
    std::condition_variable wake;
    std::wstring path;
    uint64_t wanted = 0, served = 0;
    HBITMAP bitmap = nullptr;
    uint64_t bitmap_id = 0;
    HWND notify = nullptr;
    bool stop = false;
    static void Run(std::shared_ptr<ThumbnailWorker> self) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        for (;;) {
            std::wstring path; uint64_t id = 0; HWND notify = nullptr;
            {
                std::unique_lock lock(self->mutex);
                self->wake.wait(lock, [&] { return self->stop || self->wanted != self->served; });
                if (self->stop) break;
                path = self->path; id = self->wanted; notify = self->notify; self->served = id;
            }
            HBITMAP result = nullptr;
            ComPtr<IShellItemImageFactory> factory;
            if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))))
                factory->GetImage(SIZE{256, 256}, SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK, &result);
            {
                std::lock_guard lock(self->mutex);
                if (self->bitmap) DeleteObject(self->bitmap);
                self->bitmap = result; self->bitmap_id = id;
            }
            if (notify) PostMessageW(notify, kThumbnailResult, 0, 0);
        }
        std::lock_guard lock(self->mutex);
        if (self->bitmap) { DeleteObject(self->bitmap); self->bitmap = nullptr; }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
};
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) { color.a = alpha; return color; }
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
bool Contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
}

struct GlobalSearchWindow::Impl {
    HWND hwnd = nullptr, edit = nullptr;
    HFONT edit_font = nullptr;
    ComPtr<IDWriteTextFormat> edit_format;
    HBRUSH background = nullptr;
    bool dark = true, content_mode = false, current_only = false, composing = false, busy = false, truncated = false, search_pinyin = true;
    float scale = 1, width = kDefaultWidth, height = kDefaultHeight;
    // Extra header height (DIP) when the interface font size makes the query
    // line taller than the default field; everything below moves down by it.
    float grow = 0;
    std::wstring current_folder, current_folder_tip, query, error;
    struct Row {
        std::wstring name, path, snippet; bool directory = false;
        uint64_t size = 0, mtime = 0; bool network = false;
        bool app = false;  // a Start menu program (path: its AppsFolder item)
        std::wstring alias;  // a folder found by its alias, shown as 「alias · name」
    };
    std::vector<Row> rows;
    int selected = 0, first = 0;
    uint32_t generation = 1;
    size_t total = 0;
    index::IndexClient filenames;
    index::NetworkAgentClient network;
    index::SearchResult local_result, network_result;
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
    ui::WindowMaterial material;
    ui::ShellIconCache icons;
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
    struct Scope { ScopeKind kind = ScopeKind::All; std::wstring label, path; const wchar_t* glyph = L""; size_t count = 0; bool counted = false; };
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
    ui::motion::SpringRectMotion plate_motion, scope_motion, mode_motion;
    uint64_t layout_epoch = 1;
    enum Hot { HotNone, HotRow, HotScope, HotAddPlace, HotKind, HotDate, HotMode, HotEsc, HotHandoff, HotPreviewOpen,
               HotPreviewLocation, HotPreviewCopy, HotRecentQuery, HotClearFilters, HotSearchContent };
    struct HotSpot { D2D1_RECT_F rect; Hot kind; int index; };
    std::vector<HotSpot> hot;
    Hot hover_kind = HotNone; int hover_index = -1;
    std::shared_ptr<ThumbnailWorker> thumbnails;
    ComPtr<ID2D1Bitmap> thumbnail;
    std::wstring thumbnail_path, thumbnail_wanted;
    uint64_t thumbnail_request = 0;
    ID2D1RenderTarget* thumbnail_target = nullptr;

    static constexpr index::SearchKind kKinds[] = {index::SearchKind::Any, index::SearchKind::Folder, index::SearchKind::Document,
        index::SearchKind::Image, index::SearchKind::Video, index::SearchKind::Audio, index::SearchKind::Archive, index::SearchKind::Code};
    static constexpr StringId kKindLabels[] = {StringId::FilterAll, StringId::KindFolder, StringId::KindDocument, StringId::KindImage,
        StringId::KindVideo, StringId::KindAudio, StringId::KindArchive, StringId::KindCode};
    static constexpr UINT32 kKindColors[] = {0, 0xf4c04e, 0x2b6fd6, 0x7b5ee0, 0xd0603a, 0x1f9d8a, 0x8a6d3b, 0x6b7385};
    static constexpr const wchar_t* kDateTokens[] = {L"", L" dm:today", L" dm:thisweek", L" dm:thismonth", L" dm:thisyear"};
    static constexpr StringId kDateLabels[] = {StringId::DateAny, StringId::DateToday, StringId::DateThisWeek, StringId::DateThisMonth, StringId::DateThisYear};

    ui::Theme Theme() const { return ui::IsHighContrast() ? ui::MakeHighContrastTheme() : ui::MakeTheme(dark, accent_color); }
    D2D1_COLOR_F EditBackground() const { return FieldColor(); }
    D2D1_COLOR_F FieldColor() const {
        const auto theme = Theme();
        if (ui::IsHighContrast()) return theme.header_bg;
        return dark ? D2D1::ColorF(0x2c2d33) : D2D1::ColorF(0xfbfbfd);
    }
    static UINT32 Rgb(D2D1_COLOR_F color) {
        return (static_cast<UINT32>(std::lround(color.r * 255)) << 16) |
            (static_cast<UINT32>(std::lround(color.g * 255)) << 8) | static_cast<UINT32>(std::lround(color.b * 255));
    }
    ui::WindowEffect EffectiveEffect() const {
        return !ui::IsHighContrast() && ui::SystemAllowsPopupAcrylic() ? ui::WindowEffect::Acrylic : ui::WindowEffect::None;
    }
    void ApplyAppearance() {
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
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    float Header() const { return kHeader + grow; }
    float ListTop() const { return Header() + 1; }
    float ListBottom() const { return height - kFooter; }
    float SideWidth() const { return width >= 700 ? kSide : 0; }
    float PreviewWidth() const { return preview_open && width >= 760 ? kPreview : 0; }
    float ListLeft() const { return SideWidth() + (SideWidth() > 0 ? 1.0f : 0.0f); }
    float ListRight() const { return width - PreviewWidth(); }
    float RowHeight() const { return content_mode && !showing_recent ? kContentRow : kRow; }
    int PageSize() const { return std::max(1, static_cast<int>((ListBottom() - ListTop() - 12) / RowHeight())); }

    // ---- list layout -----------------------------------------------------
    struct Group { StringId title; size_t start, count; };
    std::vector<Group> Groups() const {
        std::vector<Group> out;
        if (rows.empty()) return out;
        if (showing_recent) { out.push_back({StringId::ColumnRecentOpened, 0, rows.size()}); return out; }
        if (content_mode) { out.push_back({StringId::GlobalSearchContentMatches, 0, rows.size()}); return out; }
        const size_t j = (std::min)(jump_count, rows.size());
        if (j) out.push_back({StringId::GlobalSearchJump, 0, j});
        // 跳转, 最佳匹配, 应用, then the (long) folder and file lists.
        const size_t b = (std::min)(best_count, rows.size() - j);
        if (b) out.push_back({StringId::GlobalSearchBestMatch, j, b});
        const size_t a = (std::min)(app_count, rows.size() - j - b);
        if (a) out.push_back({StringId::GlobalSearchGroupApps, j + b, a});
        const size_t f = (std::min)(folder_count, rows.size() - j - b - a);
        if (f) out.push_back({StringId::Folder, j + b + a, f});
        const size_t files = rows.size() - j - b - a - f;
        if (files) out.push_back({StringId::FilterFiles, j + b + a + f, files});
        return out;
    }
    void BuildLayout() {
        row_top.assign(rows.size(), 0);
        float y = 6;
        recent_block = showing_recent && !context.recent_queries.empty() ? 70.0f : 0.0f;
        y += recent_block;
        for (const auto& group : Groups()) {
            y += kGroupRow;
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
            const float top = row_top[static_cast<size_t>(selected)], bottom = top + RowHeight();
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
        ++generation;
        filenames.CancelSession(kSession);
        contents.Suspend();
        busy = false;
        if (hwnd) { KillTimer(hwnd, kDebounce); KillTimer(hwnd, kConnectTimeout); }
    }
    void Hide() {
        Cancel();
        HideScopeTooltip();
        handoff_hover = false;
        hover_kind = HotNone; hover_index = -1;
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
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
    }
    void Changed() {
        Cancel();
        const int count = GetWindowTextLengthW(edit);
        query.resize(static_cast<size_t>(count) + 1);
        GetWindowTextW(edit, query.data(), count + 1);
        query.resize(static_cast<size_t>(count));
        // Refining a search keeps the previous rows (dimmed) until the new
        // results land, so the list does not blank out on every keystroke.
        const bool keep = !query.empty() && !showing_recent && !rows.empty() && content_mode == stale_content_mode;
        stale = keep;
        stale_content_mode = content_mode;
        if (!keep) { rows.clear(); selected = first = 0; best_count = folder_count = jump_count = app_count = 0; scroll = scroll_target = 0; }
        ComputeJump();
        ComputeApps();
        if (!keep && (!jump_rows.empty() || !app_rows.empty())) {
            // Known folders and programs answer at once; the index results
            // join around them.
            rows = jump_rows; jump_count = rows.size();
            rows.insert(rows.end(), app_rows.begin(), app_rows.end()); app_count = app_rows.size();
            selected = 0;
        }
        total = 0; error.clear(); truncated = false;
        local_result = {}; network_result = {}; providers = {};
        if (CountsKey() != counts_key) for (auto& item : scopes) item.counted = false;
        keyboard_nav = false;
        showing_recent = query.empty();
        if (showing_recent) { FillRecent(); selected = rows.empty() ? -1 : 0; appear_start = ui::motion::NowMs(); }
        else {
            // Known folders and apps match in memory, so they show at once; the
            // index results join them.
            if (!keep && !rows.empty()) { selected = 0; appear_start = ui::motion::NowMs(); }
            total = jump_count + app_count;
        }
        ++layout_epoch;
        BuildLayout();
        if (!query.empty() && !composing) { busy = true; SetTimer(hwnd, kDebounce, 140, nullptr); }
        RequestThumbnail();
        Invalidate();
    }
    std::wstring FilterTokens() const {
        if (content_mode) return {};
        std::wstring out;
        const auto kind = kKinds[kind_filter];
        if (kind == index::SearchKind::Folder) out += L" folder:";
        else if (kind != index::SearchKind::Any) out += std::wstring(L" type:") + index::KindId(kind);
        out += kDateTokens[date_filter];
        return out;
    }
    std::wstring ScopePath() const {
        if (scope <= 0 || scope >= static_cast<int>(scopes.size())) return {};
        return scopes[static_cast<size_t>(scope)].path;
    }
    void Search() {
        KillTimer(hwnd, kDebounce);
        if (!IsWindowVisible(hwnd) || composing || query.empty()) return;
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
            if (!folder.empty()) request.root = folder;
            else request.roots = app::DefaultContentSearchRoots(config.shared_scope);
            contents.SearchAsync(std::move(request));
        } else {
            index::Query request;
            request.session_id = kSession; request.needle = query + FilterTokens(); request.limit = kMaximumResults;
            if (!search_pinyin) request.needle = L"nopinyin: " + request.needle;
            request.path_prefix = folder;
            filenames.SearchAsync(request, generation);
            const bool network_scope = folder.empty() || IsNetworkPath(folder);
            providers.ExpectNetwork(network_scope && !network.Roots().empty());
            if (network_scope) network.SearchAsync(request, generation);
            SetTimer(hwnd, kConnectTimeout, 10000, nullptr);
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
        if (content_mode || query.empty() || date_filter != 0) return;
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
            jump_rows.push_back(std::move(row));
        }
    }
    // 应用: Start menu programs whose name matches, the most started first
    // among equal matches. Only for a plain search everywhere.
    void ComputeApps() {
        app_rows.clear();
        if (!context.show_apps || content_mode || query.empty() || date_filter != 0 || kind_filter != 0 || scope != 0 || apps.empty()) return;
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
            for (const auto& row : rows) if (UnderFolder(row.path, item.path)) ++item.count;
        }
    }
    // Folders whose alias (folder_alias.h) matches the query, as the omnibar
    // ranks them. Only the alias counts: the index finds folder names itself.
    // They obey the scope and the kind and date chips like any result.
    std::vector<Row> AliasRows() const {
        std::vector<Row> out;
        if (content_mode || showing_recent || query.empty()) return out;
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
        const auto merged = index::MergeSearchResults(request, local_result, network_result, true);
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
            if (listed(aliases, hit.path) || listed(jump_rows, hit.path)) {
                ++duplicates;  // already listed under its alias or 跳转
                continue;
            }
            Row row{hit.name, hit.path, {}, hit.is_dir};
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
        truncated = total > rows.size(); busy = providers.Busy();
        error = providers.Error() ? Text(rows.empty() ? StringId::GlobalSearchFailed : StringId::SearchIncomplete) : std::wstring{};
        if (!busy) KillTimer(hwnd, kConnectTimeout);
        if (!had_rows && !rows.empty()) appear_start = ui::motion::NowMs();
        BuildLayout();
        const int restored = position.Selection(rows);
        selected = restored;
        ClampSelection(false);
        if (restored < 0) selected = rows.empty() ? -1 : 0;
        UpdateCounts();
        RequestThumbnail();
        Invalidate();
    }
    void TimeoutFilenames() {
        providers.Timeout();
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
        if (stale || rows.empty() || selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto& row = rows[static_cast<size_t>(selected)];
        auto path = row.path;
        if (row.app) {
            // An app has no location to reveal and stays out of the recent files.
            Hide(); open_result(std::move(path), false);
            return;
        }
        const bool directory = row.directory;
        const auto search = MakeHandoff();
        const auto opened = events.opened;
        Hide(); open_result(path, location);
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
        if (hover_kind != HotScope || hover_index <= 0 || hover_index >= static_cast<int>(scopes.size())) return {};
        return path::StripExtendedPathPrefix(scopes[static_cast<size_t>(hover_index)].path);
    }
    void CopyPath() {
        if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto path = path::StripExtendedPathPrefix(rows[static_cast<size_t>(selected)].path);
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
        scope = scope == index && index != 0 ? 0 : index;
        Changed();
    }
    void SetMode(bool content) {
        if (content_mode == content) return;
        content_mode = content;
        Changed();
    }
    void TogglePreview() {
        preview_open = !preview_open;
        RequestThumbnail();
        ClampSelection();
        Invalidate();
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
        const std::wstring selected_path = scope > 0 && scope < static_cast<int>(scopes.size()) ? scopes[static_cast<size_t>(scope)].path : L"";
        const auto selected_kind = scope > 0 && scope < static_cast<int>(scopes.size()) ? scopes[static_cast<size_t>(scope)].kind : ScopeKind::All;
        scopes.clear();
        scopes.push_back({ScopeKind::All, Text(StringId::GlobalSearchEverywhere), L"", L"\xE774"});
        if (!current_folder.empty()) scopes.push_back({ScopeKind::Current, Text(StringId::GlobalSearchCurrentFolder), current_folder, L"\xE8B7"});
        const struct { REFKNOWNFOLDERID id; StringId label; const wchar_t* glyph; } known[] = {
            {FOLDERID_Desktop, StringId::Desktop, L"\xE7F4"}, {FOLDERID_Documents, StringId::PickerDocuments, L"\xE8A5"},
            {FOLDERID_Downloads, StringId::Downloads, L"\xE896"}};
        std::vector<std::wstring> taken;
        for (const auto& folder : known) {
            auto path = KnownFolder(folder.id);
            if (path.empty()) continue;
            taken.push_back(path);
            scopes.push_back({ScopeKind::Known, Text(folder.label), std::move(path), folder.glyph});
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
        if (selected_kind == ScopeKind::Current && !current_folder.empty()) scope = 1;
        else if (!selected_path.empty())
            for (size_t i = 1; i < scopes.size(); ++i) if (path::EqualInsensitive(scopes[i].path, selected_path)) scope = static_cast<int>(i);
        counts_key.clear();
    }
    bool Key(WPARAM key) {
        if (composing) return false;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (key == VK_ESCAPE) { if (preview_open) TogglePreview(); else Hide(); return true; }
        if (key == VK_RETURN) {
            if (shift) Handoff();
            else Open(ctrl);
            return true;
        }
        if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
            if (rows.empty()) return true;
            if (selected < 0) selected = key == VK_UP || key == VK_PRIOR ? static_cast<int>(rows.size()) - 1 : 0;
            else selected = std::max(0, selected + (key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -PageSize() : PageSize()));
            keyboard_nav = true;
            ClampSelection(); RequestThumbnail(); Invalidate(); return true;
        }
        if (key == VK_TAB) { content_mode = !content_mode; Changed(); return true; }
        if (key == VK_SPACE && (keyboard_nav || preview_open) && !ctrl && !rows.empty()) {
            swallow_char = L' '; TogglePreview(); return true;
        }
        if (ctrl && key == 'C' && edit) {
            DWORD start = 0, end = 0;
            SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
            if (start == end && selected >= 0) { swallow_char = 3; CopyPath(); return true; }
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
        if (message == WM_IME_STARTCOMPOSITION) { self->composing = true; self->Cancel(); }
        if (message == WM_IME_ENDCOMPOSITION) {
            self->composing = false;
            const auto result = self->DefaultEdit(window, message, wp, lp);
            self->Changed(); return result;
        }
        if (message == WM_KEYDOWN && self->Key(wp)) return 0;
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
    // Segment (文件名 | 内容) inside the right end of the field.
    D2D1_RECT_F FieldRect() const { return {kFieldLeft, kFieldTop, width - kFieldRight, kFieldTop + kFieldHeight + grow}; }
    float ModeItemWidth(bool content_item) { return TextWidth(Text(content_item ? StringId::SearchModeContent : StringId::SearchModeName), 12.5f) + 24; }
    D2D1_RECT_F ModeRect() {
        const auto field = FieldRect();
        const float w = ModeItemWidth(false) + ModeItemWidth(true) + 6;
        const float mid = (field.top + field.bottom) / 2;
        return {field.right - 7 - w, mid - 15, field.right - 7, mid + 15};
    }
    void Layout() {
        RECT rect{}; GetClientRect(hwnd, &rect);
        width = static_cast<float>(rect.right) / scale; height = static_cast<float>(rect.bottom) / scale;
        if (target) {
            if (target.Get() == compositor.Dc()) compositor.Resize(rect.right, rect.bottom);
            target->SetDpi(96 * scale, 96 * scale);
            ComPtr<ID2D1HwndRenderTarget> window_target;
            if (SUCCEEDED(target.As(&window_target))) window_target->Resize(D2D1::SizeU(static_cast<UINT32>(rect.right), static_cast<UINT32>(rect.bottom)));
        }
        // The query line follows the interface font size like every other
        // hosted edit (UI family, 20 DIP x font size x DPI).
        if (HFONT font = ui::typography::CreateEditFont(scale, 20.0f)) {
            if (edit_font) DeleteObject(edit_font);
            edit_font = font;
        }
        if (EnsureTarget()) {
            edit_format.Reset();
            write_factory->CreateTextFormat(ui::typography::PreferredTextFamily(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                static_cast<float>(ui::typography::EditFontPixels(scale, 20.0f)), l10n::LocaleName(), &edit_format);
        }
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
        // The hint reads lighter than the query: 16 DIP against 20.
        SetPropW(edit, L"Pulse.EditCueSize",
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(ui::typography::EditFontPixels(scale, 16.0f))));
        // One text line plus 4 DIP inside the 44 DIP field; taller fonts grow the header.
        const float line_dip = static_cast<float>(ui::EditLineHeight(edit, edit_font, 1 << 16)) / scale;
        grow = std::max(0.0f, std::ceil(line_dip + 14.0f - kFieldHeight));
        const float field_h = std::ceil(line_dip + 4.0f);
        const auto field = FieldRect();
        const float edit_left = field.left + 42;
        const float edit_right = ModeRect().left - 10;
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
    // An empty face means "whatever Settings > Interface font says", the same rule
    // the main window follows; only glyph runs name a family of their own.
    const wchar_t* UiFace(const wchar_t* face) const {
        return face && *face ? face : ui::typography::PreferredTextFamily();
    }
    void Label(const std::wstring& text, D2D1_RECT_F rect, float size, UINT32 color, bool bold = false, const wchar_t* face = nullptr, bool highlight = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        LabelC(text, rect, size, Color(color), bold, face, highlight, alignment);
    }
    void LabelC(const std::wstring& text, D2D1_RECT_F rect, float size, D2D1_COLOR_F color, bool bold = false, const wchar_t* face = nullptr, bool highlight = false, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING, bool wrap = false) {
        if (text.empty() || rect.right <= rect.left) return;
        ComPtr<IDWriteTextFormat> format;
        // Glyph icons keep their slots; text follows Settings > Interface font size.
        const bool icon_face = face && std::wstring_view(face).find(L"Icons") != std::wstring_view::npos;
        const float text_size = icon_face ? size : size * ui::typography::UiFontScale();
        if (FAILED(write_factory->CreateTextFormat(UiFace(face), nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
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
            if (SUCCEEDED(target->CreateSolidColorBrush(MarkColor(), &accent))) {
                // Every word of the query is marked where it occurs.
                size_t begin = 0;
                while ((begin = query.find_first_not_of(L" \t", begin)) != std::wstring::npos) {
                    const size_t end = std::min(query.find_first_of(L" \t", begin), query.size());
                    const std::wstring word = query.substr(begin, end - begin);
                    begin = end;
                    if (word.find(L':') != std::wstring::npos) continue;
                    for (size_t start = 0; start + word.size() <= text.size(); ++start) {
                        if (CompareStringOrdinal(text.data() + start, static_cast<int>(word.size()), word.data(), static_cast<int>(word.size()), TRUE) == CSTR_EQUAL) {
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
        if (SUCCEEDED(write_factory->CreateTextFormat(ui::typography::PreferredTextFamily(), nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
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
        LabelC(text, rect, symbols ? 11.5f : 11, color, false, symbols ? L"Segoe UI Symbol" : nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
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
    void RequestThumbnail() {
        if (!preview_open || selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto& path = rows[static_cast<size_t>(selected)].path;
        if (path == thumbnail_wanted) return;
        thumbnail_wanted = path;
        thumbnail.Reset(); thumbnail_path.clear();
        if (!thumbnails) {
            thumbnails = std::make_shared<ThumbnailWorker>();
            thumbnails->notify = hwnd;
            std::thread(ThumbnailWorker::Run, thumbnails).detach();
        }
        {
            std::lock_guard lock(thumbnails->mutex);
            thumbnails->path = path; thumbnails->wanted = ++thumbnail_request;
        }
        thumbnails->wake.notify_one();
    }
    void TakeThumbnail() {
        if (!thumbnails || !target) return;
        HBITMAP bitmap = nullptr; uint64_t id = 0;
        {
            std::lock_guard lock(thumbnails->mutex);
            bitmap = thumbnails->bitmap; id = thumbnails->bitmap_id; thumbnails->bitmap = nullptr;
        }
        if (!bitmap) return;
        if (id == thumbnail_request) {
            ComPtr<IWICImagingFactory> wic;
            ComPtr<IWICBitmap> source;
            ComPtr<IWICFormatConverter> converter;
            thumbnail.Reset();
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
                SUCCEEDED(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &source)) &&
                SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &thumbnail))) {
                thumbnail_path = thumbnail_wanted; thumbnail_target = target.Get();
            }
            Invalidate();
        }
        DeleteObject(bitmap);
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
    void AddHot(D2D1_RECT_F rect, Hot kind, int index = 0) { hot.push_back({rect, kind, index}); }
    bool Hovered(Hot kind, int index = 0) const { return hover_kind == kind && hover_index == index; }

    void DrawHeader(const Palette& p) {
        const float mid = (kFieldTop + kFieldTop + kFieldHeight + grow) / 2;
        DrawLogo({20, mid - 15, 50, mid + 15});
        const auto field = FieldRect();
        FillC(field, FieldColor(), kEditCornerRadius);
        StrokeC(field, p.line, kEditCornerRadius);
        FillC({field.left + 10, field.bottom - 2, field.right - 10, field.bottom}, p.accent, 1);
        LabelC(L"\xE721", {field.left + 14, mid - 12, field.left + 38, mid + 12}, 17, p.muted, false, L"Segoe Fluent Icons");
        // name / content segment
        const auto seg = ModeRect();
        AddHot(seg, HotMode);
        FillC(seg, p.chip, 7);
        const float name_w = ModeItemWidth(false);
        const D2D1_RECT_F name_rect{seg.left + 3, seg.top + 3, seg.left + 3 + name_w, seg.bottom - 3};
        const D2D1_RECT_F content_rect{name_rect.right, seg.top + 3, seg.right - 3, seg.bottom - 3};
        const auto plate = mode_motion.Update(1, content_mode ? 1 : 0, content_mode ? content_rect : name_rect, frame, ui::motion::NowMs(), ui::motion::spring::Snappy);
        FillC(plate, dark ? D2D1::ColorF(0x464851) : D2D1::ColorF(0xffffff), 5);
        if (!dark) StrokeC(plate, p.line, 5);
        LabelC(Text(StringId::SearchModeName), name_rect, 12.5f, content_mode ? p.muted : p.text, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        LabelC(Text(StringId::SearchModeContent), content_rect, 12.5f, content_mode ? p.text : p.muted, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        // Esc
        const D2D1_RECT_F esc{width - 56, mid - 11, width - 20, mid + 11};
        AddHot(esc, HotEsc);
        FillC(esc, Hovered(HotEsc) ? p.hover : p.key, 5);
        LabelC(L"Esc", esc, 11, p.muted, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (busy) {
            // Thin sweep under the header while providers work.
            const float t = static_cast<float>(ui::motion::NowMs() % 1100) / 1100.0f;
            const float w = width * 0.28f, x = -w + (width + w) * t;
            FillC({std::max(0.0f, x), Header() - 1, std::min(width, x + w), Header() + 1}, WithAlpha(p.accent_text, 0.9f));
        }
        FillC({0, Header(), width, Header() + 1}, p.line);
    }
    void DrawChip(D2D1_RECT_F rect, const std::wstring& text, bool on, bool hovered, bool enabled, UINT32 dot, const Palette& p) {
        const D2D1_COLOR_F fill = on ? p.accent : hovered && enabled ? p.hover : p.chip;
        FillC(rect, enabled ? fill : WithAlpha(fill, fill.a * 0.5f), (rect.bottom - rect.top) / 2);
        float x = rect.left + 9;
        D2D1_COLOR_F color = on ? D2D1::ColorF(1, 1, 1, 1) : hovered ? p.text : p.muted;
        if (!enabled) color = WithAlpha(p.faint, 0.6f);
        if (dot) {
            FillC({x, (rect.top + rect.bottom) / 2 - 4, x + 8, (rect.top + rect.bottom) / 2 + 4}, on ? D2D1::ColorF(1, 1, 1, 0.85f) : Color(dot), 2);
            x += 13;
        }
        LabelC(text, {x, rect.top, rect.right - 6, rect.bottom}, 12, color);
    }
    float ChipWidth(const std::wstring& text, bool dot) { return TextWidth(text, 12) + 18 + (dot ? 13 : 0); }
    void DrawSide(const Palette& p) {
        const float side = SideWidth();
        if (side <= 0) return;
        const float top = ListTop(), bottom = ListBottom();
        FillC({side, top, side + 1, bottom}, p.line);
        target->PushAxisAlignedClip({0, top, side, bottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float y = top + 8;
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
            const auto plate = scope_motion.Update(2, scope, plate_target, frame, ui::motion::NowMs(), ui::motion::spring::Snappy);
            FillC(plate, p.sel, 6);
            FillC({plate.left, plate.top + 8, plate.left + 3, plate.bottom - 8}, p.accent, 1.5f);
        }
        float row_y = top + 32;
        for (size_t i = 0; i < scopes.size(); ++i) {
            const auto& item = scopes[i];
            const bool on = static_cast<int>(i) == scope;
            const bool zero = item.counted && item.count == 0 && !query.empty() && !on;
            LabelC(item.glyph, {22, row_y + 7, 40, row_y + 23}, 14, on ? p.accent_text : zero ? WithAlpha(p.muted, 0.55f) : p.muted, false, L"Segoe Fluent Icons");
            float count_left = side - 20;
            if (item.counted && !query.empty()) {
                const auto count = std::to_wstring(item.count);
                const float cw = TextWidth(count, 11.5f);
                LabelC(count, {side - 20 - cw, row_y, side - 20, row_y + 30}, 11.5f, on ? p.accent_text : p.faint);
                count_left = side - 26 - cw;
            }
            const float label_w = std::min(TextWidth(item.label, 13), count_left - 46);
            LabelC(item.label, {46, row_y, 46 + std::max(0.0f, label_w) + 1, row_y + 30}, 13, zero ? p.faint : p.text);
            if (item.kind == ScopeKind::Current && !current_folder.empty()) {
                const float sub_left = 46 + label_w + 6;
                LabelC(LeafName(current_folder), {sub_left, row_y + 1, count_left, row_y + 30}, 11, p.faint);
            }
            row_y += 30;
        }
        {
            const D2D1_RECT_F rect{10, row_y, side - 10, row_y + 30};
            AddHot(rect, HotAddPlace);
            const bool hovered = Hovered(HotAddPlace);
            if (hovered) FillC(rect, p.hover, 6);
            LabelC(L"\xE710", {22, row_y + 8, 40, row_y + 22}, 12, hovered ? p.accent_text : p.faint, false, L"Segoe Fluent Icons");
            LabelC(Text(StringId::GlobalSearchAddPlace), {46, row_y, side - 14, row_y + 30}, 12.5f, hovered ? p.accent_text : p.faint);
            row_y += 30;
        }
        // type chips
        auto chips = [&](float& cy, int count, auto label_of, auto dot_of, int current, Hot kind) {
            float cx = 16;
            const bool enabled = !content_mode;
            for (int i = 0; i < count; ++i) {
                const std::wstring label = label_of(i);
                const UINT32 dot = dot_of(i);
                const float w = ChipWidth(label, dot != 0);
                if (cx + w > side - 10 && cx > 16) { cx = 16; cy += 31; }
                const D2D1_RECT_F rect{cx, cy, cx + w, cy + 26};
                if (enabled) AddHot(rect, kind, i);
                DrawChip(rect, label, enabled && i == current, Hovered(kind, i), enabled, dot, p);
                cx += w + 5;
            }
            cy += 31;
        };
        row_y += 8;
        LabelC(Text(StringId::SearchChipType), {20, row_y, side - 12, row_y + 22}, 11.5f, p.faint);
        row_y += 26;
        chips(row_y, 8, [&](int i) { return Text(kKindLabels[i]); }, [&](int i) { return kKindColors[i]; }, kind_filter, HotKind);
        row_y += 4;
        LabelC(Text(StringId::SearchChipDate), {20, row_y, side - 12, row_y + 22}, 11.5f, p.faint);
        row_y += 26;
        chips(row_y, 5, [&](int i) { return Text(kDateLabels[i]); }, [](int) { return 0u; }, date_filter, HotDate);
        target->PopAxisAlignedClip();
    }
    void DrawRow(size_t i, float y, float left, float right, const Palette& p, float alpha) {
        const auto& row = rows[i];
        const bool sel = static_cast<int>(i) == selected && (keyboard_nav || !showing_recent);
        const float h = RowHeight();
        const bool content_row = content_mode && !showing_recent;
        if (alpha < 1.0f) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(), alpha), nullptr);
        const D2D1_RECT_F rect{left, y, right, y + h};
        if (Hovered(HotRow, static_cast<int>(i)) && !sel) FillC(rect, p.hover, 7);
        const float icon_top = content_row ? y + 10 : y + (h - 28) / 2;
        icons.Draw(compositor.Dc(), {left + 12, icon_top, left + 40, icon_top + 28}, row.path, row.name, row.directory,
            row.directory ? FILE_ATTRIBUTE_DIRECTORY : 0);
        const float text_left = left + 52;
        const float hints_w = sel ? (row.app ? 90.0f : row.directory || preview_open ? 150.0f : 205.0f) : 0.0f;
        const float meta_w = sel ? 0.0f : 112.0f;
        const float text_right = right - 12 - std::max(hints_w, meta_w);
        const float name_top = content_row ? y + 8 : y + 7;
        LabelC(row.alias.empty() ? row.name : row.alias + L" \x00B7 " + row.name, {text_left, name_top, text_right, name_top + 20}, 14,
               p.text, false, nullptr, !showing_recent);
        const float path_top = name_top + 20;
        std::wstring parent = row.app ? Text(StringId::TypeApplication) : path::StripExtendedPathPrefix(ParentPath(row.path));
        float path_left = text_left;
        if (row.network) {
            LabelC(L"\xE8CE", {text_left, path_top + 2, text_left + 14, path_top + 15}, 10, p.faint, false, L"Segoe Fluent Icons");
            path_left += 16;
        }
        LabelC(MiddleEllipsis(parent, text_right - path_left, 12), {path_left, path_top, text_right, path_top + 17}, 12, p.faint);
        if (content_row && !row.snippet.empty())
            LabelC(row.snippet, {text_left, path_top + 19, right - 16, path_top + 41}, 12.5f, p.muted, false, nullptr, true);
        if (!sel && !row.app) {
            const auto time = FormatTime(row.mtime);
            const auto size = row.directory ? Text(StringId::KindFolder) : row.size || row.mtime ? FormatSize(row.size) : std::wstring{};
            const float mt = content_row ? y + 9 : y + 8;
            LabelC(time, {right - 12 - meta_w, mt, right - 12, mt + 17}, 11.5f, p.faint, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
            LabelC(size, {right - 12 - meta_w, mt + 17, right - 12, mt + 34}, 11.5f, p.faint, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (sel) {
            const float mid = content_row ? y + 20 : y + h / 2;
            float r = right - 12;
            if (!row.app) r = KeyHint(L"Ctrl \x21B5", Text(StringId::Location), r, mid, p.muted, p.key) - 12;
            r = KeyHint(L"\x21B5", Text(StringId::Open), r, mid, p.muted, p.key) - 12;
            if (!row.directory && !row.app && !preview_open) KeyHint(L"Space", Text(StringId::Preview), r, mid, p.muted, p.key);
        }
        if (alpha < 1.0f) target->PopLayer();
    }
    void DrawEmpty(float left, float right, float top, float bottom, const Palette& p) {
        const float cx = (left + right) / 2;
        float y = (top + bottom) / 2 - 40;
        if (busy) {
            LabelC(Text(StringId::GlobalSearchLoading), {left, y, right, y + 40}, 14, p.muted, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (!error.empty()) {
            LabelC(error, {left + 16, y, right - 16, y + 40}, 14, p.muted, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        if (query.empty()) {
            if (empty_art_target != target.Get() || empty_art_scale != scale) {
                empty_art.Reset(); empty_art_target = target.Get(); empty_art_scale = scale;
                ui::CreateSvgResourceBitmap(target.Get(), IDR_EMPTY_FOLDER_SVG,
                    D2D1::SizeU(static_cast<UINT32>(Px(200)), static_cast<UINT32>(Px(200 * 360.0f / 512))), D2D1::SizeF(512, 360), &empty_art);
            }
            if (empty_art) target->DrawBitmap(empty_art.Get(), {cx - 100, y - 110, cx + 100, y + 30}, dark ? 0.9f : 0.6f);
            LabelC(Text(StringId::GlobalSearchEmpty), {left, y + 36, right, y + 62}, 14, p.muted, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            return;
        }
        const bool filtered = scope != 0 || (!content_mode && (kind_filter != 0 || date_filter != 0));
        LabelC(Text(StringId::GlobalSearchNoResults), {left, y, right, y + 26}, 15, p.text, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (filtered) LabelC(Text(StringId::GlobalSearchNoFilteredResults), {left, y + 26, right, y + 48}, 12.5f, p.muted, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        const auto& action = Text(filtered ? StringId::EmptyActClearFilters : content_mode ? StringId::SearchModeName : StringId::EmptyActContent);
        const float bw = TextWidth(action, 12.5f) + 28;
        const D2D1_RECT_F button{cx - bw / 2, y + 60, cx + bw / 2, y + 90};
        const Hot kind = filtered ? HotClearFilters : HotSearchContent;
        AddHot(button, kind);
        FillC(button, Hovered(kind) ? p.hover : p.chip, 7);
        LabelC(action, button, 12.5f, p.accent_text, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    void DrawList(const Palette& p, uint64_t now) {
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
                LabelC(L"\xE81C", {x + 10, y + 7, x + 24, y + 21}, 11, p.faint, false, L"Segoe Fluent Icons");
                LabelC(text, {x + 28, y, x + w - 10, y + 28}, 12.5f, p.muted);
                x += w + 6;
            }
        }
        if (rows.empty()) {
            DrawEmpty(left, right, top + recent_block, bottom, p);
        } else {
            // Selection plate under the rows.
            if (selected >= 0 && selected < static_cast<int>(rows.size()) && (keyboard_nav || !showing_recent)) {
                const float y = origin + row_top[static_cast<size_t>(selected)];
                const D2D1_RECT_F target_rect{lx, y, rx, y + RowHeight()};
                const auto plate = plate_motion.Update(layout_epoch, selected, target_rect, frame, now, ui::motion::spring::Plate);
                FillC(plate, p.sel, 7);
                StrokeC(plate, p.sel_stroke, 7);
                FillC({plate.left, plate.top + 14, plate.left + 3, plate.bottom - 14}, p.accent, 1.5f);
            } else plate_motion.Hide();
            const float appear_ms = static_cast<float>(now - appear_start);
            int visible_index = 0;
            for (const auto& group : Groups()) {
                const float gy = origin + row_top[group.start] - kGroupRow;
                if (gy + kGroupRow >= top && gy <= bottom) {
                    const auto& title = Text(group.title);
                    const auto count = std::to_wstring(group.count);
                    const float tw = TextWidth(title, 11.5f), cw = TextWidth(count, 11.5f);
                    LabelC(title, {lx + 12, gy + 8, lx + 14 + tw, gy + kGroupRow}, 11.5f, p.faint);
                    LabelC(count, {lx + 20 + tw, gy + 8, lx + 22 + tw + cw, gy + kGroupRow}, 11.5f, p.faint);
                    FillC({lx + 30 + tw + cw, gy + 19, rx - 12, gy + 20}, p.line);
                }
                for (size_t i = group.start; i < group.start + group.count; ++i) {
                    float y = origin + row_top[i];
                    if (y + RowHeight() < top || y > bottom) continue;
                    float alpha = 1.0f;
                    if (animations && appear_ms < 520) {
                        const float t = std::clamp((appear_ms - static_cast<float>(std::min(visible_index, 10)) * 22.0f) / 260.0f, 0.0f, 1.0f);
                        const float eased = ui::motion::EaseOutCubic(t);
                        alpha = eased; y += (1.0f - eased) * 6.0f;
                    }
                    ++visible_index;
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
            LabelC(L"\xE73E", {pill.left + 14, pill.top + 9, pill.left + 30, pill.bottom - 9}, 12, D2D1::ColorF(0x7fd6a0), false, L"Segoe Fluent Icons");
            LabelC(toast, {pill.left + 34, pill.top, pill.right - 10, pill.bottom}, 12.5f, D2D1::ColorF(1, 1, 1, 1));
        }
    }
    void DrawPreview(const Palette& p) {
        const float pw = PreviewWidth();
        if (pw <= 0) return;
        const float x0 = width - pw, top = ListTop(), bottom = ListBottom();
        FillC({x0, top, x0 + 1, bottom}, p.line);
        if (selected < 0 || selected >= static_cast<int>(rows.size())) return;
        const auto& row = rows[static_cast<size_t>(selected)];
        const float l = x0 + 16, r = width - 16;
        const D2D1_RECT_F thumb_box{l, top + 16, r, top + 196};
        FillC(thumb_box, p.chip, 9);
        StrokeC(thumb_box, p.line, 9);
        if (thumbnail && thumbnail_target == target.Get() && thumbnail_path == row.path) {
            const auto size = thumbnail->GetSize();
            const float box_w = thumb_box.right - thumb_box.left - 24, box_h = thumb_box.bottom - thumb_box.top - 24;
            const float k = std::min({box_w / std::max(1.0f, size.width), box_h / std::max(1.0f, size.height), 1.0f * scale});
            const float w = size.width * k, h = size.height * k;
            const float cx = (thumb_box.left + thumb_box.right) / 2, cy = (thumb_box.top + thumb_box.bottom) / 2;
            target->DrawBitmap(thumbnail.Get(), {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            const float cx = (thumb_box.left + thumb_box.right) / 2, cy = (thumb_box.top + thumb_box.bottom) / 2;
            icons.Draw(compositor.Dc(), {cx - 24, cy - 24, cx + 24, cy + 24}, row.path, row.name, row.directory, row.directory ? FILE_ATTRIBUTE_DIRECTORY : 0);
        }
        float y = thumb_box.bottom + 14;
        LabelC(row.name, {l, y, r, y + 44}, 15, p.text, true, nullptr, !showing_recent, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        y += TextWidth(row.name, 15, true) > r - l ? 46 : 24;
        if (!row.app) {
            LabelC(path::StripExtendedPathPrefix(row.path), {l, y, r, y + 36}, 12, p.faint, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_LEADING, true);
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
        // actions
        const float b = bottom - 16;
        const D2D1_RECT_F location{l, b - 32, (l + r) / 2 - 3, b};
        const D2D1_RECT_F copy{(l + r) / 2 + 3, b - 32, r, b};
        // A program has no location or path to copy: just 打开.
        const D2D1_RECT_F open = row.app ? D2D1_RECT_F{l, b - 32, r, b} : D2D1_RECT_F{l, b - 70, r, b - 38};
        AddHot(open, HotPreviewOpen);
        FillC(open, Hovered(HotPreviewOpen) ? Theme().accent_hover : p.accent, 7);
        LabelC(Text(StringId::Open), open, 13, D2D1::ColorF(1, 1, 1, 1), false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (row.app) return;
        AddHot(location, HotPreviewLocation); AddHot(copy, HotPreviewCopy);
        for (auto [rect, label, kind] : {std::tuple{location, StringId::HintActOpenPath, HotPreviewLocation}, std::tuple{copy, StringId::CopyPath, HotPreviewCopy}}) {
            FillC(rect, Hovered(kind) ? p.hover : p.chip, 7);
            StrokeC(rect, p.line, 7);
            LabelC(Text(label), rect, 12.5f, p.text, false, nullptr, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    void DrawFooter(const Palette& p) {
        const float bottom = ListBottom(), mid = bottom + kFooter / 2;
        FillC({0, bottom, width, bottom + 1}, p.line);
        float x = 20;
        const bool waiting = busy || (providers.network_expected && !providers.network_ready && !content_mode && !query.empty());
        if (!query.empty()) {
            FillC({x, mid - 3, x + 6, mid + 3}, waiting ? D2D1::ColorF(0xe6b44c) : D2D1::ColorF(0x46c27a), 3);
            x += 14;
        }
        std::wstring status = query.empty() ? std::wstring{} : FooterStatus();
        if (!query.empty() && !busy && error.empty() && !content_mode) {
            size_t net = 0;
            for (const auto& row : rows) net += row.network ? 1 : 0;
            if (net) status += L" \x00b7 " + Text(StringId::GlobalSearchLocal) + L" " + std::to_wstring(rows.size() - net) +
                L" \x00b7 " + Text(StringId::GlobalSearchNetwork) + L" " + std::to_wstring(net);
        }
        if (truncated) status += L" \x00b7 " + Text(StringId::GlobalSearchTruncatedShort);
        float right = width - 16;
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
    void Paint() {
        PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint);
        Render();
        EndPaint(hwnd, &paint);
    }
    void Render() {
        if (EnsureTarget()) {
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
            DrawSide(p);
            DrawList(p, now);
            DrawPreview(p);
            DrawFooter(p);
            DrawScopeTooltip(theme);
            const HRESULT rendered = target->EndDraw();
            if (FAILED(rendered)) {
                compositor.NotifyDeviceLost(rendered); empty_art.Reset(); empty_art_target = nullptr; logo.Reset(); logo_target = nullptr;
                thumbnail.Reset(); thumbnail_target = nullptr; icons.Reset(); brush.Reset(); target.Reset();
            } else if (live) compositor.Present();
            const bool moving = plate_motion.Active(now) || scope_motion.Active(now) || mode_motion.Active(now) ||
                std::fabs(scroll_target - scroll) >= 0.5f || (animations && now - appear_start < 560) || busy || (!toast.empty() && now < toast_until + 20);
            if (moving) SetTimer(hwnd, kAnimationTimer, 10, nullptr);
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
            selected = index; keyboard_nav = true; ClampSelection(); RequestThumbnail(); Invalidate(); break;
        case HotScope: SetScope(index); break;
        case HotAddPlace: PickPlace(); break;
        case HotKind: kind_filter = kind_filter == index ? 0 : index; Changed(); break;
        case HotDate: date_filter = date_filter == index ? 0 : index; Changed(); break;
        case HotMode: SetMode(x >= ModeRect().left + 3 + ModeItemWidth(false)); break;
        case HotEsc: Hide(); return;
        case HotPreviewOpen: Open(false); return;
        case HotPreviewLocation: Open(true); return;
        case HotPreviewCopy: CopyPath(); break;
        case HotRecentQuery:
            if (index >= 0 && index < static_cast<int>(context.recent_queries.size())) {
                SetWindowTextW(edit, context.recent_queries[static_cast<size_t>(index)].c_str());
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }
            break;
        case HotClearFilters: scope = 0; kind_filter = 0; date_filter = 0; Changed(); break;
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
        return HTCLIENT;
    }
    LRESULT Message(UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_PAINT: Paint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_NCCALCSIZE: if (wp) return 0; break;
        case WM_NCHITTEST: return HitTestFrame(lp);
        case WM_NCACTIVATE: return DefWindowProcW(hwnd, message, wp, -1);
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {Px(kMinWidth), Px(kMinHeight)};
            return 0;
        }
        case WM_EXITSIZEMOVE:
            if (events.resized) events.resized(static_cast<int>(std::lround(width)), static_cast<int>(std::lround(height)));
            SetFocus(edit);
            return 0;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
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
        case WM_ACTIVATEAPP: if (!wp) Hide(); return 0;
        case WM_SETFOCUS: SetFocus(edit); return 0;
        case WM_KEYDOWN: if (Key(wp)) return 0; break;
        case WM_COMMAND: if (reinterpret_cast<HWND>(lp) == edit && HIWORD(wp) == EN_CHANGE) Changed(); return 0;
        case WM_CTLCOLOREDIT: {
            const auto dc = reinterpret_cast<HDC>(wp); SetBkColor(dc, Background());
            SetTextColor(dc, ui::HcEditText(dark ? RGB(245, 245, 247) : RGB(32, 33, 36)));
            return reinterpret_cast<LRESULT>(ui::EditBackBrush(background));
        }
        case WM_TIMER:
            if (wp == kDebounce) Search();
            else if (wp == kConnectTimeout) {
                KillTimer(hwnd, kConnectTimeout);
                if (busy && !content_mode) TimeoutFilenames();
            } else if (wp == kScopeTooltipTimer) {
                KillTimer(hwnd, kScopeTooltipTimer);
                if (!TooltipText().empty()) { scope_tooltip_visible = true; Invalidate(); }
            } else if (wp == kAnimationTimer) {
                Invalidate();
            }
            return 0;
        case WM_MOUSEMOVE: {
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
            if (IsWindowVisible(hwnd)) SetFocus(edit);
            return 0;
        case WM_LBUTTONDBLCLK: {
            const auto* spot = HitAt(static_cast<float>(GET_X_LPARAM(lp)) / scale, static_cast<float>(GET_Y_LPARAM(lp)) / scale);
            if (spot && spot->kind == HotRow) { selected = spot->index; Open(false); }
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_target -= static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * RowHeight() * 2.5f;
            ClampScroll();
            if (!animations) scroll = scroll_target;
            Invalidate(); return 0;
        case kIndexStatus: Invalidate(); return 0;
        case kThumbnailResult: TakeThumbnail(); return 0;
        case kAppsReady:
            apps = *app_catalog.Apps();
            if (IsWindowVisible(hwnd) && !query.empty() && !content_mode && !showing_recent && !providers.Busy()) MergeFilenames();
            return 0;
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
        case kContentResult: {
            index::ContentSearchUpdate update;
            while (contents.TakeUpdate(update)) {
                if (update.progress.generation != generation || !content_mode || !IsWindowVisible(hwnd)) continue;
                if (stale) { stale = false; rows.clear(); selected = 0; scroll = scroll_target = 0; }
                jump_count = app_count = 0;
                const bool had_rows = !rows.empty();
                for (auto& hit : update.hits) {
                    if (rows.size() >= kMaximumResults) break;
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
                    rows.push_back(std::move(row));
                }
                total = rows.size(); truncated = update.progress.truncated;
                busy = !update.progress.done;
                std::wstring pack_title, pack_message;
                if (ContentPackBanner(update.progress.error, pack_title, pack_message)) error = std::move(pack_title);
                else if (update.progress.error && update.progress.error != ERROR_CANCELLED) error = Text(StringId::GlobalSearchFailed);
                if (!had_rows && !rows.empty()) { selected = 0; appear_start = ui::motion::NowMs(); }
                BuildLayout();
                ClampSelection(false); RequestThumbnail(); Invalidate();
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
    bool Show(HWND owner, bool use_dark, float requested_scale, const std::wstring& folder, bool pinyin) {
        dark = use_dark; scale = requested_scale > 0 ? requested_scale : 1;
        search_pinyin = pinyin;
        animations = ui::motion::SystemAnimationsEnabled();
        plate_motion.SetAnimationsEnabled(animations); scope_motion.SetAnimationsEnabled(animations); mode_motion.SetAnimationsEnabled(animations);
        const HWND foreground = GetForegroundWindow();
        POINT cursor{}; GetCursorPos(&cursor);
        const HMONITOR active_monitor = foreground && foreground != hwnd ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST) :
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (const float monitor_scale = MonitorScale(active_monitor); monitor_scale > 0) scale = monitor_scale;
        (void)owner;
        const bool folder_changed = folder != current_folder;
        current_folder = folder;
        current_folder_tip = path::StripExtendedPathPrefix(folder);
        if (folder.empty()) current_only = false;
        HideScopeTooltip();
        if (!Create()) return false;
        if (folder_changed && scope > 0 && scope < static_cast<int>(scopes.size()) && scopes[static_cast<size_t>(scope)].kind == ScopeKind::Current) scope = 0;
        Prepare();
        BOOL is_dark = dark; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(StringId::GlobalSearchPlaceholder).c_str()));
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(active_monitor, &monitor);
        const RECT place = Placement(monitor.rcWork);
        SetWindowPos(hwnd, HWND_TOPMOST, place.left, place.top, place.right - place.left, place.bottom - place.top, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        Layout(); SetForegroundWindow(hwnd); SetFocus(edit); SendMessageW(edit, EM_SETSEL, 0, -1);
        Changed(); return true;
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
    // Centred, a quarter down the work area, at the saved size.
    RECT Placement(const RECT& work) const {
        const float want_w = context.width > 0 ? static_cast<float>(context.width) : kDefaultWidth;
        const float want_h = context.height > 0 ? static_cast<float>(context.height) : kDefaultHeight;
        const int w = std::clamp(Px(want_w), std::min(Px(kMinWidth), static_cast<int>(work.right - work.left) - Px(32)), static_cast<int>(work.right - work.left) - Px(32));
        const int h = std::clamp(Px(want_h), std::min(Px(kMinHeight), static_cast<int>(work.bottom - work.top) - Px(32)), static_cast<int>(work.bottom - work.top) - Px(32));
        const int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 4;
        return {x, y, x + w, y + h};
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
            if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"Pulse Search",
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
        POINT cursor{}; GetCursorPos(&cursor);
        const HMONITOR monitor_handle = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        if (const float monitor_scale = MonitorScale(monitor_handle); monitor_scale > 0) scale = monitor_scale;
        animations = ui::motion::SystemAnimationsEnabled();
        if (!Create()) return;
        Prepare();
        BOOL is_dark = dark; DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(monitor_handle, &monitor);
        const RECT place = Placement(monitor.rcWork);
        // Cloaked: DWM composes the window once but never shows it, and it
        // is not activated, so focus stays where it is.
        BOOL cloak = TRUE;
        const bool cloaked = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
        SetWindowPos(hwnd, HWND_TOPMOST, place.left, place.top, place.right - place.left, place.bottom - place.top,
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
        if (thumbnails) {
            { std::lock_guard lock(thumbnails->mutex); thumbnails->stop = true; thumbnails->notify = nullptr; }
            thumbnails->wake.notify_one();
        }
        if (edit) RemoveWindowSubclass(edit, EditProc, 1);
        if (hwnd) DestroyWindow(hwnd);
        if (edit_font) DeleteObject(edit_font);
        if (background) DeleteObject(background);
    }
};
GlobalSearchWindow::GlobalSearchWindow() = default;
GlobalSearchWindow::~GlobalSearchWindow() = default;
bool GlobalSearchWindow::Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->Show(owner, dark, scale, current_folder, search_pinyin);
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
bool GlobalSearchWindow::Visible() const { return impl_ && impl_->hwnd && IsWindowVisible(impl_->hwnd); }
}