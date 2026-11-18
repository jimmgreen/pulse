// app_hover_preview.cpp - The hover preview card (see app_hover_preview.h).
#include "app_hover_preview.h"
#include "../fs/cloud_state.h"
#include "../fs/bounded_enumeration.h"
#include "../fs/count_directory.h"
#include "../fs/drive_kind.h"
#include "app_internal.h"
#include "app_details_tree.h"
#include "entry_sort.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/preview_extensions.h"
#include "../ui/video_preview.h"
#include "../ui/hover_preview_content.h"
#include "../ui/ui_renderer.h"
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

namespace pulse {

// A folder card's listing, read on its own thread: the card cancels it when
// it goes, and reads what it published so far every frame.
struct HoverFolderJob {
    std::wstring path;
    std::atomic_bool cancelled{false};
    std::atomic_bool done{false};
    std::mutex lock;
    std::shared_ptr<const ui::HoverFolderInfo> result;  // null while reading

    std::shared_ptr<const ui::HoverFolderInfo> Result() {
        std::lock_guard<std::mutex> guard(lock);
        return result;
    }
    void Publish(std::shared_ptr<const ui::HoverFolderInfo> info) {
        std::lock_guard<std::mutex> guard(lock);
        result = std::move(info);
    }
};

namespace {

// How long the pointer rests on a row before its preview is asked for.
constexpr UINT kHoverPrefetchSettleMs = 150;

bool HoverPreviewOn(const AppState& s) {
    return s.appPrefs.preview_trigger == app::kPreviewTriggerHover && !s.safeMode;
}

using ui::hover_preview_content::TypeName;

// Nothing else owns the pointer: no drag, marquee, resize or capture.
bool PointerFree(const AppState& s) {
    return !GetCapture() && !s.listSelection.PendingFile() && !s.listSelection.PendingBand() && !s.listSelection.BandActive() &&
        !s.tabDragging && !s.scrollbarDragging && !s.columnResizing && !s.stripResizing &&
        !s.stripHScrolling && !s.splitterDragging && !s.detailsPanelResizing;
}

bool CanShow(AppState& s) {
    return s.hwnd && HoverPreviewOn(s) && GetForegroundWindow() == s.hwnd && !IsIconic(s.hwnd) &&
        !s.shot.active && !s.menushot && !s.quickPreview.visible() && !s.context_menu.menu_open() &&
        s.renameIndex < 0 && PointerFree(s) &&
        s.hoverRegion == static_cast<int>(ui::HitTestResult::Row);
}

app::Tab* TabAtSlot(AppState& s, int slot) {
    app::Pane* pane = slot >= 0 ? PaneAtSlot(s, slot) : s.pane;
    return pane ? pane->ActiveTab() : nullptr;
}

using ui::hover_preview_content::CardVideo;

bool SlowToDecode(const fs::DirEntry& entry, const std::wstring& path) {
    return ui::hover_preview_content::SlowToDecode(entry.name, entry.size, path);
}

// ---- Folder cards ------------------------------------------------------

// Folders open on click: their card waits this much longer than a file's,
// so passing over them on the way to a file shows none.
constexpr int kFolderExtraDwellMs = 300;

uint64_t FileTimeValue(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::wstring JoinPath(const std::wstring& parent, const std::wstring& name) {
    if (parent.empty() || parent.back() == L'\\') return parent + name;
    return parent + L'\\' + name;
}

using ui::hover_preview_content::FolderKind;

// The listing's visibility rule (Tab::AllowsAttributes), for the worker.
bool ShownChild(DWORD attrs, bool show_hidden, bool show_protected) {
    if (!(attrs & FILE_ATTRIBUTE_HIDDEN)) return true;
    return show_hidden && (!(attrs & FILE_ATTRIBUTE_SYSTEM) || show_protected);
}

// Why a folder could not be listed (ERROR_ACCESS_DENIED gets its own note).
DWORD FolderOpenError(const std::wstring& path) {
    const std::wstring pattern = JoinPath(fs::NormalizePath(path), L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return GetLastError();
    FindClose(find);
    return ERROR_SUCCESS;
}

struct FolderReadOptions {
    ui::SortColumn sort_column = ui::SortColumn::Name;
    ui::SortDirection sort_direction = ui::SortDirection::Asc;
    app::FolderSortMode folder_sort = app::FolderSortMode::FoldersFirst;
    bool show_hidden = false;
    bool show_protected = false;
    bool drive = false;
};

struct SortCancelled {};

// PULSE_TEST_HOVER_FOLDER_LOG=file: one timing line per folder read, for the
// performance probe; nothing is measured or written without it.
struct FolderReadTiming {
    std::wstring path;
    LARGE_INTEGER start{};
    double enum_ms = -1.0, sort_ms = -1.0, ready_ms = -1.0;
    size_t total = 0, listed = 0;
    int asked = 0;
    const char* end = "done";

    explicit FolderReadTiming(const std::wstring& folder) : path(folder) { QueryPerformanceCounter(&start); }
    double Ms() const {
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        return static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    }
    ~FolderReadTiming() {
        wchar_t out[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"PULSE_TEST_HOVER_FOLDER_LOG", out, MAX_PATH)) return;
        const double end_ms = Ms();
        std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0,
                                                                 nullptr, nullptr)), '\0');
        if (!utf8.empty()) {
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            utf8.pop_back();
        }
        char line[256];
        const int n = snprintf(line, sizeof(line),
            "\t%s\ttotal=%zu\tlisted=%zu\tenum_ms=%.1f\tsort_ms=%.1f\tready_ms=%.1f\tasked=%d\tend_ms=%.1f\n",
            end, total, listed, enum_ms, sort_ms, ready_ms, asked, end_ms);
        if (n <= 0) return;
        utf8.append(line, static_cast<size_t>((std::min)(n, static_cast<int>(sizeof(line)) - 1)));
        static std::mutex write_lock;
        std::lock_guard<std::mutex> guard(write_lock);
        HANDLE file = CreateFileW(out, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(file);
    }
};

// The worker: the folder's children in the pane's order, then how many
// entries the first subfolders hold, within a short budget.
void ReadFolder(const std::shared_ptr<HoverFolderJob>& job, const FolderReadOptions& options) {
    const auto cancelled = [job] { return job->cancelled.load(); };
    FolderReadTiming timing(job->path);
    auto info = std::make_shared<ui::HoverFolderInfo>();
    std::vector<fs::DirEntry> entries;
    bool listed = false;
    try {
        fs::EnumerationOptions enumeration;
        enumeration.cancelled = cancelled;
        enumeration.timeout_ms = 15000;
        fs::EnumerateDirectory(job->path, entries, enumeration);
        listed = true;
    } catch (...) {
    }
    timing.enum_ms = timing.Ms();
    timing.total = entries.size();
    if (cancelled()) {
        timing.end = "cancelled";
        job->done = true;
        return;
    }
    if (!listed) {
        timing.end = "failed";
        info->state = FolderOpenError(job->path) == ERROR_ACCESS_DENIED ? ui::HoverFolderInfo::State::Denied
                                                                         : ui::HoverFolderInfo::State::Failed;
        job->Publish(info);
        job->done = true;
        return;
    }
    std::erase_if(entries, [&options](const fs::DirEntry& entry) {
        return entry.name == L"." || entry.name == L".." ||
               !ShownChild(entry.attrs, options.show_hidden, options.show_protected);
    });
    for (const auto& entry : entries) {
        if (entry.is_dir) {
            ++info->folders;
        } else {
            ++info->files;
            ++info->kinds[static_cast<size_t>(FolderKind(entry.name))];
        }
    }
    try {
        const app::ScopedFolderSortMode folder_mode(options.folder_sort);
        app::SortFirstEntries(entries, options.sort_column, options.sort_direction, ui::kHoverFolderMaxItems, [&cancelled] {
            if (cancelled()) throw SortCancelled{};
        });
    } catch (...) {
    }
    timing.sort_ms = timing.Ms();
    if (cancelled()) {
        timing.end = "cancelled";
        job->done = true;
        return;
    }
    const size_t keep = (std::min)(entries.size(), ui::kHoverFolderMaxItems);
    info->items.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        const fs::DirEntry& entry = entries[i];
        ui::HoverFolderItem item;
        item.name = entry.name;
        item.attrs = entry.attrs;
        item.is_dir = entry.is_dir;
        item.kind = entry.is_dir ? ui::kHoverKindOther : FolderKind(entry.name);
        item.size = entry.size;
        item.modified = FileTimeValue(entry.mtime);
        info->items.push_back(std::move(item));
    }
    if (options.drive) {
        wchar_t file_system[MAX_PATH + 1]{};
        const std::wstring root = JoinPath(job->path, L"");
        if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, file_system, MAX_PATH + 1))
            info->file_system = file_system;
    }
    info->state = ui::HoverFolderInfo::State::Ready;
    job->Publish(info);
    timing.ready_ms = timing.Ms();
    timing.listed = keep;
    if (fs::IsUncPath(job->path)) {
        timing.end = "unc";
        job->done = true;
        return;
    }
    auto counted = std::make_shared<ui::HoverFolderInfo>(*info);
    const ULONGLONG start = GetTickCount64();
    int asked = 0;
    bool any = false;
    for (auto& item : counted->items) {
        if (cancelled() || asked >= 48 || GetTickCount64() - start > 800) break;
        if (!item.is_dir || (item.attrs & FILE_ATTRIBUTE_REPARSE_POINT) || fs::IsCloudPlaceholder(item.attrs))
            continue;
        ++asked;
        int count = 0;
        try {
            const auto elapsed = GetTickCount64() - start;
            if (elapsed >= 800) break;
            const auto remaining = (std::min)(ULONGLONG{400}, ULONGLONG{800} - elapsed);
            const auto child_path = JoinPath(job->path, item.name);
            count = fs::RunBoundedEnumeration<int>(fs::IsUncPath(child_path) || fs::IsRemoteLikeDrive(child_path),
                [child_path, show_hidden = options.show_hidden, show_protected = options.show_protected]
                (const std::atomic_bool& cancel) {
                    return fs::CountDirectoryChildren(child_path, show_hidden, show_protected, cancel);
                }, cancelled, std::chrono::milliseconds(remaining));
        } catch (...) {
            continue;
        }
        item.children = count;
        any = true;
    }
    if (any && !cancelled()) job->Publish(counted);
    timing.asked = asked;
    if (cancelled()) timing.end = "cancelled";
    job->done = true;
}

// The folder a row's card lists: a real folder or a This PC drive; not the
// recycle bin, change records or virtual locations.
bool FolderCardPath(const app::Tab& tab, int row, std::wstring& path) {
    const fs::DirEntry& entry = tab.EntryAt(static_cast<size_t>(row));
    if (!entry.is_dir || entry.change_record_only || IsRecycleTab(&tab)) return false;
    path = EntryFullPath(tab, row);
    return !path.empty() && !fs::IsVirtualPath(path);
}

void CancelFolderWork(AppState& s) {
    auto& card = s.hoverPreview;
    if (card.folder_job) {
        card.folder_job->cancelled = true;
        card.folder_job.reset();
    }
    if (card.size_started) {
        // A count the card started goes with it; the folder's own label
        // keeps whatever was known before.
        card.size_started = false;
        const auto work = s.folderSizes.GetWork(card.path);
        if (work.manual && work.Running()) s.folderSizes.Cancel(card.path);
    }
}

// Starts reading `path` for the card, unless that is already under way
// (the settled pointer asked for it before the card opened).
void StartFolderJob(AppState& s, const app::Tab& tab, const fs::DirEntry& entry, const std::wstring& path,
                    bool wait = false) {
    auto& card = s.hoverPreview;
    if (card.folder_job && card.folder_job->path == path && !card.folder_job->cancelled) return;
    if (card.folder_job) card.folder_job->cancelled = true;
    auto job = std::make_shared<HoverFolderJob>();
    job->path = path;
    card.folder_job = job;
    if (entry.drive_locked) {
        auto info = std::make_shared<ui::HoverFolderInfo>();
        info->state = ui::HoverFolderInfo::State::Failed;
        job->Publish(info);
        job->done = true;
        return;
    }
    FolderReadOptions options;
    options.sort_column = tab.sort_column;
    options.sort_direction = tab.sort_direction;
    options.folder_sort = tab.EffectiveFolderSort();
    options.show_hidden = tab.show_hidden_files;
    options.show_protected = tab.show_protected_os_files;
    options.drive = entry.drive_type != 0;
    if (wait) {  // --shot: the listing is in before the picture is taken
        ReadFolder(job, options);
        return;
    }
    try {
        std::thread([job, options] { ReadFolder(job, options); }).detach();
    } catch (...) {
        job->done = true;
    }
}

std::wstring FolderTypeName(const fs::DirEntry& entry) {
    if (!entry.drive_type) return l10n::Get(l10n::StringId::KindFolder).c_str();
    switch (entry.drive_remote_like ? static_cast<uint8_t>(DRIVE_REMOTE) : entry.drive_type) {
    case DRIVE_REMOVABLE: return l10n::Get(l10n::StringId::RemovableDisk_2).c_str();
    case DRIVE_REMOTE: return l10n::Get(l10n::StringId::NetworkDrive).c_str();
    case DRIVE_CDROM: return l10n::Get(l10n::StringId::OpticalDrive).c_str();
    case DRIVE_FIXED: return l10n::Get(l10n::StringId::LocalDisk_2).c_str();
    default: return l10n::Get(l10n::StringId::Drive).c_str();
    }
}

// The card's folder fields for the row it opens on; a file's card clears them.
void SetFolderCard(AppState& s, const app::Tab& tab, const fs::DirEntry& entry, bool wait = false) {
    auto& card = s.hoverPreview;
    card.folder = entry.is_dir;
    card.drive = entry.is_dir && entry.drive_type != 0;
    card.drive_locked = card.drive && entry.drive_locked;
    card.drive_total = card.drive ? entry.drive_total : 0;
    card.drive_free = card.drive ? entry.drive_free : 0;
    card.folder_scroll = 0.0f;
    card.size_started = false;
    if (!card.folder) {
        if (card.folder_job) {
            card.folder_job->cancelled = true;
            card.folder_job.reset();
        }
        return;
    }
    StartFolderJob(s, tab, entry, card.path, wait);
    // Its size: what the folder size service knows, else a count of its own
    // (local folders; a drive shows its capacity instead).
    if (card.drive || fs::IsUncPath(card.path)) return;
    const auto value = s.folderSizes.Get(card.path);
    const auto work = s.folderSizes.GetWork(card.path);
    if (!value.has_value && !work.Running()) {
        s.folderSizes.Calculate(card.path);
        card.size_started = true;
    }
}

// The 大小 fact: a running count shows how far it got.
void FolderSizeFact(AppState& s, const std::wstring& path, std::wstring& text, bool& running) {
    const auto value = s.folderSizes.Get(path);
    const auto work = s.folderSizes.GetWork(path);
    running = work.Running();
    if (running) {
        const uint64_t bytes = (std::max)(work.bytes, value.has_value ? value.bytes : 0ull);
        text = bytes ? L"\u2265 " + format::ByteSize(bytes) : std::wstring(L"\u2026");
    } else if (value.has_value) {
        text = (value.partial ? L"\u2265 " : L"") + format::ByteSize(value.bytes);
    } else {
        text = L"\u2014";
    }
}

// The pointer on the folder card, or in the gap between it and its row.
bool InFolderCard(const AppState& s, POINT point) {
    const auto& card = s.hoverPreview;
    if (!card.visible || !card.folder) return false;
    const D2D1_RECT_F r = s.renderer.HoverCardRect();
    if (r.right <= r.left) return false;
    const float grace = 10.0f * s.scale;
    const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
    return x >= r.left - grace && x < r.right + grace && y >= r.top && y < r.bottom;
}

// Repaints for the card's animations (dwell line, loading sweep, video
// progress); 0 stops them.
void SetFrameTimer(AppState& s, UINT ms) {
    auto& card = s.hoverPreview;
    if (!s.hwnd || ms == card.frame_ms) return;
    card.frame_ms = ms;
    if (ms) SetTimer(s.hwnd, kTimerHoverPreviewFrame, ms, nullptr);
    else KillTimer(s.hwnd, kTimerHoverPreviewFrame);
}

void ClearDwell(AppState& s) {
    auto& card = s.hoverPreview;
    if (card.dwell_row < 0) return;
    card.dwell_row = card.dwell_pane = -1;
    card.dwell_start = 0;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Starts the dwell for the hovered row: the chosen delay, or a short one
// when the pointer just left a card (browsing a column of files).
void Arm(AppState& s) {
    auto& card = s.hoverPreview;
    if (!s.hwnd) return;
    KillTimer(s.hwnd, kTimerHoverPreview);
    KillTimer(s.hwnd, kTimerHoverPrefetch);
    ClearDwell(s);
    if (!HoverPreviewOn(s) || s.hoverRow < 0 || card.wait_move) return;
    if (s.hoverRow == card.quiet_row && s.hoverPaneIndex == card.quiet_pane) return;
    card.quiet_row = card.quiet_pane = -1;
    const ULONGLONG now = GetTickCount64();
    const bool browsing = card.hidden_at != 0 && now - card.hidden_at < 450;
    int delay = browsing ? 60 : app::NormalizeHoverPreviewDelay(s.appPrefs.hover_preview_delay_ms);
    {
        app::Tab* hovered = TabAtSlot(s, s.hoverPaneIndex);
        if (hovered && hovered->snapshot && !hovered->loading && s.hoverRow < static_cast<int>(hovered->EntryCount()) &&
            hovered->EntryAt(static_cast<size_t>(s.hoverRow)).is_dir)
            delay += kFolderExtraDwellMs;
    }
    SetTimer(s.hwnd, kTimerHoverPreview, static_cast<UINT>(delay), nullptr);
    if (browsing) return;
    // Decoding starts once the pointer has settled, not when the card opens:
    // the dwell then covers most of it (a pointer only passing over the
    // row starts nothing).
    SetTimer(s.hwnd, kTimerHoverPrefetch, kHoverPrefetchSettleMs, nullptr);
    app::Tab* tab = TabAtSlot(s, s.hoverPaneIndex);
    if (!tab || !tab->snapshot || tab->loading || s.hoverRow >= static_cast<int>(tab->EntryCount())) return;
    const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(s.hoverRow));
    if (entry.is_dir || entry.change_record_only || entry.pc_launcher || entry.pc_device ||
        !SlowToDecode(entry, EntryFullPath(*tab, s.hoverRow)))
        return;
    card.dwell_row = s.hoverRow;
    card.dwell_pane = s.hoverPaneIndex;
    card.dwell_start = now;
    card.dwell_ms = static_cast<UINT>(delay);
    SetFrameTimer(s, 16);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// The settled row's preview goes into the card's cache ahead of the card.
void Prefetch(AppState& s) {
    if (s.hoverRow < 0 || !CanShow(s) || s.hoverPreview.visible) return;
    app::Tab* tab = TabAtSlot(s, s.hoverPaneIndex);
    if (!tab || !tab->snapshot || tab->loading || s.hoverRow >= static_cast<int>(tab->EntryCount())) return;
    const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(s.hoverRow));
    if (entry.change_record_only) return;
    if (entry.is_dir) {
        // A folder's listing is read over the rest of the dwell.
        std::wstring path;
        if (FolderCardPath(*tab, s.hoverRow, path)) StartFolderJob(s, *tab, entry, path);
        return;
    }
    ui::QuickPreviewItem item;
    if (!QuickPreviewItemForRow(s, *tab, s.hoverRow, item)) return;
    s.renderer.PrefetchHoverPreview(item.path, item.attrs, item.size, item.modified);
}

void Show(AppState& s) {
    auto& card = s.hoverPreview;
    if (s.hoverRow < 0 || !CanShow(s)) return;
    app::Tab* tab = TabAtSlot(s, s.hoverPaneIndex);
    if (!tab || !tab->snapshot || tab->loading || s.hoverRow >= static_cast<int>(tab->EntryCount()))
        return;
    const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(s.hoverRow));
    if (entry.change_record_only) return;
    ui::QuickPreviewItem item;
    if (entry.is_dir) {
        // A folder's card lists what is inside (its dwell is longer: folders
        // open on click, and passing over them must not raise cards).
        if (!FolderCardPath(*tab, s.hoverRow, item.path)) return;
        if (PointOnDetailsTreeChevron(s, s.hoverPaneIndex, s.hoverPoint)) return;
        item.name = entry.name;
        item.attrs = entry.attrs;
        item.modified = FileTimeValue(entry.mtime);
    } else if (!QuickPreviewItemForRow(s, *tab, s.hoverRow, item)) {
        return;
    }
    card.visible = true;
    card.row = s.hoverRow;
    card.pane = s.hoverPaneIndex;
    card.path = std::move(item.path);
    card.name = std::move(item.name);
    card.attrs = item.attrs;
    card.size = item.size;
    card.modified = item.modified;
    card.type_text = entry.is_dir ? FolderTypeName(entry) : TypeName(card.name);
    card.point = s.hoverPoint;
    SetFolderCard(s, *tab, entry);
    // Browsing (the previous card just went): the card came after a short
    // delay, and a video waits for the card to settle before its player
    // opens. After the full dwell the pointer has already settled: the
    // player opens at once, which brings the first moving frame ~220 ms
    // closer.
    const ULONGLONG now = GetTickCount64();
    const bool browsing = card.hidden_at != 0 && now - card.hidden_at < 600;
    card.hidden_at = 0;
    card.video = !entry.is_dir && CardVideo(card.path);
    card.slow = !entry.is_dir && SlowToDecode(entry, card.path);
    card.shown_at = browsing ? now : now - 220;
    ClearDwell(s);
    SetFrameTimer(s, 33);  // the first frames settle the loading sweep / video
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// A click, key or wheel: the card goes and stays away until the pointer
// moves again (after a click, until it reaches another row).
void Quiet(AppState& s, bool this_row) {
    auto& card = s.hoverPreview;
    HideHoverPreview(s);
    card.hidden_at = 0;
    if (this_row) {
        card.quiet_row = s.hoverRow;
        card.quiet_pane = s.hoverPaneIndex;
    }
    card.wait_move = true;
    POINT point{};
    if (s.hwnd && GetCursorPos(&point) && ScreenToClient(s.hwnd, &point)) card.quiet_point = point;
    else card.quiet_point = s.hoverPoint;
}

} // namespace

void HideHoverPreview(AppState& s) {
    auto& card = s.hoverPreview;
    if (s.hwnd) {
        KillTimer(s.hwnd, kTimerHoverPreview);
        KillTimer(s.hwnd, kTimerHoverPrefetch);
    }
    ClearDwell(s);
    CancelFolderWork(s);
    if (!card.visible) return;
    card.visible = false;
    card.hidden_at = GetTickCount64();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NoteHoverPreviewRow(AppState& s) {
    HideHoverPreview(s);
    Arm(s);
}

bool HoverPreviewHoldsPointer(AppState& s, POINT point) {
    if (!s.hwnd || !InFolderCard(s, point)) return false;
    // Only while the pointer is really over this window (a hover refresh
    // after it left passes the last point it had).
    POINT screen = point;
    return ClientToScreen(s.hwnd, &screen) && WindowFromPoint(screen) == s.hwnd;
}

bool HoverPreviewMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (hwnd != s.hwnd) return false;
    auto& card = s.hoverPreview;
    switch (msg) {
    case WM_TIMER:
        if (wparam == kTimerHoverPreviewFrame) {
            if (!card.visible && card.dwell_row < 0) SetFrameTimer(s, 0);
            else InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        if (wparam == kTimerHoverPrefetch) {
            KillTimer(hwnd, kTimerHoverPrefetch);
            Prefetch(s);
            return true;
        }
        if (wparam != kTimerHoverPreview) return false;
        KillTimer(hwnd, kTimerHoverPreview);
        Show(s);
        return true;
    case WM_MOUSEMOVE:
        if (card.wait_move) {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (std::abs(point.x - card.quiet_point.x) + std::abs(point.y - card.quiet_point.y) > 2) {
                card.wait_move = false;
                if (!card.visible) Arm(s);
            }
        }
        return false;
    case WM_KEYUP:
        // Ctrl enlarges the card while held (FillHoverPreviewView reads it).
        if (wparam == VK_CONTROL && card.visible) InvalidateRect(hwnd, nullptr, FALSE);
        return false;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam == VK_CONTROL && card.visible && !(lparam & (1 << 30))) InvalidateRect(hwnd, nullptr, FALSE);
        if (wparam == VK_SHIFT || wparam == VK_CONTROL || wparam == VK_MENU) return false;
        // Space with a card up opens it in quick look (app_input.cpp).
        if (msg == WM_KEYDOWN && wparam == VK_SPACE && card.visible) return false;
        Quiet(s, false);
        return false;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK: {
        // A click on a folder card goes no further than the card: the rows
        // under it are not clicked.
        const bool on_card = InFolderCard(s, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        Quiet(s, true);
        return on_card;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // The wheel on a folder card scrolls its list.
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (ScreenToClient(hwnd, &point) && InFolderCard(s, point)) {
            if (msg == WM_MOUSEWHEEL) {
                const float step = s.renderer.HoverFolderRowHeight() * 3.0f;
                const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                const float max_scroll = (std::max)(0.0f, s.renderer.HoverFolderScrollMax());
                card.folder_scroll = std::clamp(card.folder_scroll - notches * step, 0.0f, max_scroll);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return true;
        }
        Quiet(s, false);
        return false;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) HideHoverPreview(s);
        return false;
    case WM_KILLFOCUS:
    case WM_ENTERSIZEMOVE:
    case WM_SIZE:
        HideHoverPreview(s);
        return false;
    default:
        return false;
    }
}

bool OpenHoverPreviewInQuickLook(AppState& s) {
    auto& card = s.hoverPreview;
    if (!card.visible) return false;
    const int row = card.row;
    const int slot = card.pane;
    HideHoverPreview(s);
    card.hidden_at = 0;
    // No card over this row again when quick look closes.
    card.quiet_row = row;
    card.quiet_pane = slot;
    app::Pane* pane = slot >= 0 ? PaneAtSlot(s, slot) : s.pane;
    app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
    if (!tab || !tab->snapshot || row < 0 || row >= static_cast<int>(tab->EntryCount())) return false;
    if (pane != s.pane) FocusPane(s, pane);
    if (s.quickPreview.visible()) s.quickPreview.Close();
    tab->SelectOnly(row);
    EnsureRowVisible(s, *tab, row);
    ToggleQuickPreview(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool StageHoverDwellShot(AppState& s, int nth_file) {
    if (!StageHoverPreviewShot(s, nth_file)) return false;
    auto& card = s.hoverPreview;
    card.visible = false;
    card.dwell_row = card.row;
    card.dwell_pane = card.pane;
    card.dwell_ms = 500;
    card.dwell_start = GetTickCount64() - 300;  // 60% through the dwell
    return true;
}

bool StageHoverPreviewShot(AppState& s, int nth_file) {
    app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (!tab || !tab->snapshot) return false;
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const D2D1_RECT_F bounds = FocusedPaneRect(s);
    const int count = static_cast<int>(tab->EntryCount());
    // PULSE_TEST_HOVER_FOLDER=1: the Nth folder instead (its listing read
    // before the shot); PULSE_TEST_HOVER_SCROLL=px scrolls its list.
    wchar_t env[16]{};
    const bool folders = GetEnvironmentVariableW(L"PULSE_TEST_HOVER_FOLDER", env, 16) > 0 && env[0] == L'1';
    int seen = 0;
    for (int row = 0; row < count; ++row) {
        const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(row));
        if (entry.is_dir != folders || entry.change_record_only) continue;
        if (seen++ < nth_file) continue;
        const int index = pane.ViewIndex(row);
        if (index < 0) return false;
        const D2D1_RECT_F cell = s.renderer.ItemRectInPane(pane, bounds, index);
        ui::QuickPreviewItem item;
        if (folders) {
            if (!FolderCardPath(*tab, row, item.path)) return false;
            item.name = entry.name;
            item.attrs = entry.attrs;
            item.modified = FileTimeValue(entry.mtime);
        } else if (!QuickPreviewItemForRow(s, *tab, row, item)) {
            return false;
        }
        auto& card = s.hoverPreview;
        s.hoverRegion = static_cast<int>(ui::HitTestResult::Row);
        s.hoverRow = row;
        s.hoverPaneIndex = -1; // the focused pane (vm.pane)
        s.hoverPoint = POINT{static_cast<LONG>(cell.left + 120.0f),
                             static_cast<LONG>((cell.top + cell.bottom) * 0.5f)};
        card.visible = true;
        card.row = row;
        card.pane = s.hoverPaneIndex;
        card.path = std::move(item.path);
        card.name = std::move(item.name);
        card.attrs = item.attrs;
        card.size = item.size;
        card.modified = item.modified;
        card.type_text = folders ? FolderTypeName(entry) : TypeName(card.name);
        card.point = s.hoverPoint;
        card.video = !folders && CardVideo(card.path);
        card.slow = !folders && SlowToDecode(entry, card.path);
        card.shown_at = GetTickCount64() - 1000;  // settled: a video opens at once
        SetFolderCard(s, *tab, entry, true);
        if (folders && GetEnvironmentVariableW(L"PULSE_TEST_HOVER_SCROLL", env, 16) > 0)
            card.folder_scroll = static_cast<float>((std::max)(0, _wtoi(env)));
        return true;
    }
    return false;
}

void FillHoverPreviewView(AppState& s, const ui::WindowViewModel& vm, ui::HoverPreviewView& view) {
    view = {};
    auto& card = s.hoverPreview;
    // The row's cell in this frame's layout; false when it is gone or
    // scrolled out of the list.
    const auto cell_of = [&](int slot, int row, D2D1_RECT_F& cell) {
        const ui::PaneViewModel* pane = &vm.pane;
        D2D1_RECT_F bounds = FocusedPaneRect(s);
        if (slot >= 0 && slot < static_cast<int>(vm.pane_slots.size())) {
            pane = &vm.pane_slots[static_cast<size_t>(slot)].pane;
            bounds = vm.pane_slots[static_cast<size_t>(slot)].rect;
        }
        const int index = pane->ViewIndex(row);
        if (index < 0) return false;
        cell = s.renderer.ItemRectInPane(*pane, bounds, index);
        const D2D1_RECT_F list = s.renderer.PaneListRect(*pane, bounds);
        const float cy = (cell.top + cell.bottom) * 0.5f;
        return cy >= list.top && cy <= list.bottom;
    };
    if (!card.visible) {
        if (card.dwell_row < 0) return;
        if (card.dwell_row != s.hoverRow || card.dwell_pane != s.hoverPaneIndex || !HoverPreviewOn(s) ||
            !card.dwell_ms) {
            card.dwell_row = card.dwell_pane = -1;
            return;
        }
        D2D1_RECT_F cell{};
        if (!cell_of(card.dwell_pane, card.dwell_row, cell)) return;
        view.dwell = true;
        view.dwell_row = cell;
        view.dwell_progress = (std::min)(1.0f,
            static_cast<float>(GetTickCount64() - card.dwell_start) / static_cast<float>(card.dwell_ms));
        return;
    }
    const auto drop = [&] {
        CancelFolderWork(s);
        card.visible = false;
        card.hidden_at = 0;
    };
    const bool staged = s.shot.active; // --shot-hover-preview: no foreground, no pointer
    if (!HoverPreviewOn(s) || card.row != s.hoverRow || card.pane != s.hoverPaneIndex ||
        s.quickPreview.visible() || s.renameIndex >= 0 || s.context_menu.menu_open() ||
        (!staged && (GetForegroundWindow() != s.hwnd || !PointerFree(s)))) {
        drop();
        return;
    }
    app::Tab* tab = TabAtSlot(s, card.pane);
    if (!tab || !tab->snapshot || card.row >= static_cast<int>(tab->EntryCount()) ||
        tab->EntryAt(static_cast<size_t>(card.row)).name != card.name) {
        drop();
        return;
    }
    D2D1_RECT_F cell{};
    if (!cell_of(card.pane, card.row, cell)) {
        drop();
        return;
    }
    view.visible = true;
    view.anchor = cell;
    view.pointer = D2D1::Point2F(static_cast<float>(card.point.x), static_cast<float>(card.point.y));
    view.path = card.path;
    view.name = card.name;
    view.type_text = card.type_text;
    view.attrs = card.attrs;
    view.size = card.size;
    view.modified = card.modified;
    view.video = card.video;
    view.slow = card.slow;
    view.owner = s.hwnd;
    view.shown_ms = card.shown_at;
    view.size_step = app::NormalizeHoverCardSize(s.appPrefs.hover_card_size);
    view.instant = staged;
    if (card.folder) {
        view.folder = true;
        view.drive = card.drive;
        view.drive_locked = card.drive_locked;
        view.drive_total = card.drive_total;
        view.drive_free = card.drive_free;
        if (card.folder_job) view.listing = card.folder_job->Result();
        view.folder_scroll = card.folder_scroll;
        if (!card.drive) FolderSizeFact(s, card.path, view.size_text, view.size_running);
    }
    if (staged) {
        wchar_t zoom[4]{};
        view.zoom = GetEnvironmentVariableW(L"PULSE_TEST_HOVER_ZOOM", zoom, 4) > 0 && zoom[0] == L'1';
    } else {
        view.zoom = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    }
}

void WriteHoverVideoProbe(AppState& s, const std::wstring& shot_output) {
    std::wstring path = shot_output;
    if (const size_t dot = path.find_last_of(L'.'); dot != std::wstring::npos) path.resize(dot);
    path += L".video.txt";
    const auto video = s.renderer.HoverVideoState();
    char text[512];
    const int n = snprintf(text, sizeof(text),
        "card_video=%d\nready=%d\nplaying=%d\nmuted=%d\nffmpeg=%d\nposition_ms=%lld\nduration_ms=%lld\n"
        "size=%ux%u\nerror=0x%08lX\nvisible=%d dwell_row=%d dwell_pane=%d hover_row=%d hover_pane=%d frame_ms=%u\n",
        s.hoverPreview.video ? 1 : 0, video.ready ? 1 : 0, video.playing ? 1 : 0, video.muted ? 1 : 0,
        video.ffmpeg ? 1 : 0, static_cast<long long>(video.position / 10000),
        static_cast<long long>(video.duration / 10000), video.width, video.height,
        static_cast<unsigned long>(video.error), s.hoverPreview.visible ? 1 : 0, s.hoverPreview.dwell_row,
        s.hoverPreview.dwell_pane, s.hoverRow, s.hoverPaneIndex, s.renderer.HoverPreviewFrameMs());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    if (n > 0) WriteFile(file, text, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

namespace {

std::string BenchUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring BenchEnv(const wchar_t* name) {
    wchar_t value[2048]{};
    const DWORD n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return n && n < ARRAYSIZE(value) ? std::wstring(value, n) : std::wstring();
}

std::vector<std::wstring> BenchList(const std::wstring& path) {
    std::vector<std::wstring> lines;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return lines;
    std::string bytes;
    char buffer[65536];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) bytes.append(buffer, got);
    CloseHandle(file);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF) bytes.erase(0, 3);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>((std::max)(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

const char* BenchResultName(int result) {
    switch (result) {
    case static_cast<int>(ui::PreviewDrawResult::Bitmap): return "bitmap";
    case static_cast<int>(ui::PreviewDrawResult::Text): return "text";
    case static_cast<int>(ui::PreviewDrawResult::Hex): return "hex";
    case static_cast<int>(ui::PreviewDrawResult::Failed): return "failed";
    case static_cast<int>(ui::PreviewDrawResult::Archive): return "archive";
    case static_cast<int>(ui::PreviewDrawResult::Markdown): return "markdown";
    case static_cast<int>(ui::PreviewDrawResult::Table): return "table";
    case static_cast<int>(ui::PreviewDrawResult::Tree): return "tree";
    default: return "timeout";
    }
}

} // namespace

// --bench-hover: PULSE_BENCH_HOVER_LIST names a UTF-8 list of files, one per
// line; each is asked for the way the card asks (cold, then again at a new
// size with the preview host and file cache warm), videos also opened in the
// silent player. One tab-separated row per file goes to PULSE_BENCH_HOVER_OUT.
// The card asks for the preview before the facts; PULSE_BENCH_HOVER_ORDER=
// properties times the old order (facts first).
bool RunHoverBench(AppState& s) {
    const std::wstring list_path = BenchEnv(L"PULSE_BENCH_HOVER_LIST");
    const std::wstring out_path = BenchEnv(L"PULSE_BENCH_HOVER_OUT");
    if (list_path.empty() || out_path.empty()) return false;
    const std::wstring timeout_text = BenchEnv(L"PULSE_BENCH_HOVER_TIMEOUT");
    const uint32_t timeout_ms = timeout_text.empty() ? 15000u : static_cast<uint32_t>(_wtoi(timeout_text.c_str()));
    const bool properties_first = BenchEnv(L"PULSE_BENCH_HOVER_ORDER") == L"properties";
    // PULSE_BENCH_HOVER_PREFETCH=N: the settled-pointer prefetch, then N ms of
    // dwell; cold_ms is then what is left to wait once the card opens.
    const std::wstring prefetch_text = BenchEnv(L"PULSE_BENCH_HOVER_PREFETCH");
    const DWORD prefetch_ms = prefetch_text.empty() ? 0u : static_cast<DWORD>(_wtoi(prefetch_text.c_str()));
    const int prefetch_mode = _wtoi(BenchEnv(L"PULSE_BENCH_HOVER_PREFETCH_MODE").c_str());
    const std::vector<std::wstring> files = BenchList(list_path);
    // PULSE_BENCH_HOVER_DECOY: a file whose decode is left in flight before
    // each sample, PULSE_BENCH_HOVER_DECOY_MS (default 450) earlier.
    const std::wstring decoy = BenchEnv(L"PULSE_BENCH_HOVER_DECOY");
    const std::wstring decoy_text = BenchEnv(L"PULSE_BENCH_HOVER_DECOY_MS");
    const DWORD decoy_ms = decoy_text.empty() ? 450u : static_cast<DWORD>(_wtoi(decoy_text.c_str()));
    HANDLE out = CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return true;
    const auto write = [&](const std::string& line) {
        DWORD written = 0;
        WriteFile(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        FlushFileBuffers(out);
    };
    write("path\text\tbytes\tresult\twidth\theight\tcold_ms\tprops_ms\tupload_ms\twarm_ms\tvideo_ready_ms\tvideo_frame_ms\tpf_first\tpf_after\tfirst_ms\n");
    s.renderer.SetHoverBench(true);
    timeBeginPeriod(1);  // 1 ms polls: the numbers are not rounded to the 15.6 ms tick
    // The hover cache's preview host starts with the first request: timed
    // apart, on a file of our own.
    {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring warm = std::wstring(temp) + L"pulse_bench_warmup.txt";
        HANDLE file = CreateFileW(warm.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, "warm up\r\n", 9, &written, nullptr);
            CloseHandle(file);
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        GetFileAttributesExW(warm.c_str(), GetFileExInfoStandard, &data);
        const uint64_t modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                  data.ftLastWriteTime.dwLowDateTime;
        const auto host = s.renderer.BenchHoverPreview(warm, data.dwFileAttributes, 9, modified, 1024, timeout_ms,
                                                       properties_first);
        char line[256];
        snprintf(line, sizeof(line), "#host_start\t\t9\t%s\t0\t0\t%.1f\t%.1f\t0\t0\t-1\t-1\n",
                 BenchResultName(host.result), host.content_ms, host.properties_ms);
        write(line);
    }
    for (const auto& path : files) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            write(BenchUtf8(path) + "\t\t0\tmissing\t0\t0\t0\t-1\t0\t0\t-1\t-1\n");
            continue;
        }
        const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        const uint64_t modified = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                  data.ftLastWriteTime.dwLowDateTime;
        int pf_first = -9, pf_after = -9;
        if (!decoy.empty()) {
            // The pointer passing over a slow file first: its decode is in
            // flight when this one is asked for (the abandon path).
            WIN32_FILE_ATTRIBUTE_DATA other{};
            if (GetFileAttributesExW(decoy.c_str(), GetFileExInfoStandard, &other)) {
                s.renderer.EvictThumbnails();
                s.renderer.PrefetchHoverPreview(decoy, other.dwFileAttributes,
                    (static_cast<uint64_t>(other.nFileSizeHigh) << 32) | other.nFileSizeLow,
                    (static_cast<uint64_t>(other.ftLastWriteTime.dwHighDateTime) << 32) |
                        other.ftLastWriteTime.dwLowDateTime, false);
                const ULONGLONG until = GetTickCount64() + decoy_ms;
                while (GetTickCount64() < until) Sleep(1);
            }
        }
        if (prefetch_ms) {
            if (!(prefetch_mode & 2)) s.renderer.EvictThumbnails();
            pf_first = s.renderer.PrefetchHoverPreview(path, data.dwFileAttributes, size, modified);
            const ULONGLONG until = GetTickCount64() + prefetch_ms;
            while (GetTickCount64() < until) {
                MSG msg{};
                while (!(prefetch_mode & 1) && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(1);
            }
            if (prefetch_mode & 4) pf_after = s.renderer.PrefetchHoverPreview(path, data.dwFileAttributes, size, modified);
        }
        const auto cold = s.renderer.BenchHoverPreview(path, data.dwFileAttributes, size, modified, 1024, timeout_ms,
                                                       properties_first);
        const auto warm = s.renderer.BenchHoverPreview(path, data.dwFileAttributes, size, modified, 1000, timeout_ms,
                                                       properties_first);
        double video_ready = -1.0, video_frame = -1.0;
        if (CardVideo(path)) video_frame = s.renderer.BenchHoverVideo(s.hwnd, path, 10000, &video_ready);
        char numbers[512];
        snprintf(numbers, sizeof(numbers), "\t%llu\t%s\t%u\t%u\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t%d\t%.1f\n",
                 static_cast<unsigned long long>(size), BenchResultName(cold.result), cold.width, cold.height,
                 cold.content_ms, cold.properties_ms, cold.upload_ms, warm.content_ms, video_ready, video_frame,
                 pf_first, pf_after, cold.first_ms);
        write(BenchUtf8(path) + numbers);
    }
    write("#done\n");
    timeEndPeriod(1);
    CloseHandle(out);
    s.renderer.SetHoverBench(false);
    return true;
}

void AfterHoverPreviewDraw(AppState& s) {
    auto& card = s.hoverPreview;
    UINT ms = s.renderer.HoverPreviewFrameMs();
    // A card whose video is still opening keeps polling until it plays.
    if (!ms && card.visible && card.video && GetTickCount64() - card.shown_at < 1500) ms = 33;
    // A folder card whose listing is still coming in (subfolder counts).
    if (card.visible && card.folder && card.folder_job && !card.folder_job->done) ms = ms ? (std::min)(ms, 120u) : 120u;
    SetFrameTimer(s, ms);
}

} // namespace pulse
