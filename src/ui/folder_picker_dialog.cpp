#include "view_zoom.h"
#include "picker_legacy_child.h"
#include "picker_navigation_root.h"
#include "picker_details_tree.h"
#include "../picker_inprocess/picker_ui_api_v2.h"
#include "picker_hover_preview.h"
#include "picker_preview_layout.h"
#include "quick_preview_window.h"
#include "../common/picker_shell_namespace.h"
#include "../common/picker_places_snapshot.h"
#include "../common/picker_tray_consume.h"
#include "../common/drag_shake_detector.h"
#include <filesystem>
#include <thread>
#include "../common/preview_extensions.h"
#include "../common/preview_diagnostics.h"
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
#include "../bench/picker_jump_ui_test_api.h"
#include "../app/picker_appearance.h"
#include "../bench/picker_embedded_layout_test.h"
#endif
#include "../common/picker_places_saved.h"
#include "picker_places_ui.h"
#include "picker_places_request.h"
#include "picker_places_menu.h"
#include "picker_ole_drop.h"
#ifdef PULSE_WITH_TEST_HOOKS
#include "../bench/picker_places_test_api.h"
#include "../bench/picker_view_switch_test_api.h"
#include "../bench/picker_interaction_test_api.h"
#endif
#include "../common/picker_user_storage.h"
#include "../common/picker_pulse_open.h"
#include "picker_custom_layout.h"
#include "../common/picker_owner_guard.h"
#include "picker_proof_trace.h"
#include "picker_marquee.h"
#include "picker_sidebar_model.h"
#include "picker_format_segments.h"
#include "picker_incoming_files.h"
#include "picker_save_suggestions.h"
#include "picker_save_feedback.h"
#include "picker_save_delivery.h"
#include "picker_image_dimensions.h"
#include "picker_tray_delivery.h"
#include "picker_save_draft_probe.h"
#include "picker_save_advisory.h"
#include "../index/search_kinds.h"
#include "picker_blank_click.h"
#include "../app/create_rename_intent.h"
#include "../app/jump_match.h"
#include "../app/picker_list_order.h"
#include "../app/picker_tag_catalog.h"
#include "edit_host.h"
#include "../fs/drive_kind.h"
#include "../common/windows_compat.h"
#include "../common/navigation_known_folders.h"
#include "folder_picker_dialog.h"
#include "picker_archive_session.h"
#include "picker_shell_filtered_listing.h"
#include "../common/picker_navigation.h"
#include "../common/picker_layout_settings.h"
#include "../common/runtime_log.h"

#include "address_search_layout.h"
#include "drive_capacity.h"
#include "folder_picker_loader.h"
#include "folder_picker_operations.h"
#include "folder_picker_view.h"
#include "confirm_dialog.h"
#include "breadcrumb_dropdown.h"
#include "fluent_menu.h"
#include "typography.h"
#include "ui_renderer.h"
#include "ui_motion.h"
#include "window_helpers.h"
#include "layer_alphas.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../app/details_column_menu.h"
#include "../app/frame_pump.h"
#include "../ops/clipboard.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#if defined(PULSE_PICKER_INPROCESS_PROOF)
#include <cstdio>
#endif
#include <cwctype>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <deque>
#include <source_location>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {
namespace {

void PickerProofPhase(const char* phase) {
#if defined(PULSE_PICKER_INPROCESS_PROOF)
    const DWORD previous_error = GetLastError();
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS", enabled, 2) == 1 && enabled[0] == L'1') {
        std::fprintf(stderr, "picker-phase tick=%llu %s\n", static_cast<unsigned long long>(GetTickCount64()), phase);
        std::fflush(stderr);
    }
    SetLastError(previous_error);
#else
    (void)phase;
#endif
}

void PickerProofResult(const char* phase, HRESULT hr, DWORD error, bool state) {
#if defined(PULSE_PICKER_INPROCESS_PROOF)
    const DWORD previous_error = GetLastError();
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS", enabled, 2) == 1 && enabled[0] == L'1') {
        std::fprintf(stderr, "picker-result phase=%s hr=%08lX error=%lu state=%u\n", phase,
            static_cast<unsigned long>(hr), static_cast<unsigned long>(error), state ? 1u : 0u);
        std::fflush(stderr);
    }
    SetLastError(previous_error);
#else
    (void)phase; (void)hr; (void)error; (void)state;
#endif
}

void PickerProofFocus(const char* phase, int previous, int next) {
#if defined(PULSE_PICKER_INPROCESS_PROOF)
    const DWORD previous_error = GetLastError();
    wchar_t enabled[2]{};
    if (GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS", enabled, 2) == 1 && enabled[0] == L'1') {
        const HWND focused = GetFocus();
        std::fprintf(stderr, "picker-focus tick=%llu message_time=%lu phase=%s from=%d to=%d hwnd_id=%d\n",
            static_cast<unsigned long long>(GetTickCount64()), static_cast<unsigned long>(GetMessageTime()),
            phase, previous, next, focused ? GetDlgCtrlID(focused) : -1);
        std::fflush(stderr);
    }
    SetLastError(previous_error);
#else
    (void)phase; (void)previous; (void)next;
#endif
}

constexpr wchar_t kPickerClass[] = L"PulseFolderPickerWindow";
constexpr UINT kListingMessage = WM_APP + 1;
constexpr UINT kValidationMessage = WM_APP + 2;
constexpr UINT kSaveDirectoriesMessage = WM_APP + 97;
constexpr UINT kEndAddressMessage = WM_APP + 3;
constexpr UINT kEndRenameMessage = WM_APP + 95;
constexpr UINT kEmbeddedNavigationMessage = WM_APP + 4;
constexpr UINT kFolderCommitMessage = WM_APP + 6;
constexpr UINT kExternalNavigationMessage = WM_APP + 7;
constexpr UINT kPickerDropMessage = WM_APP + 8;
constexpr UINT kPickerFrameMessage = WM_APP + 9;
constexpr UINT kHoverListingMessage = WM_APP + 10;
constexpr DWORD kInitialFolderTimeoutMs = 5000;
constexpr UINT_PTR kSharedPlacesTimer = 0x504c;
constexpr UINT_PTR kSaveDraftTimer = 0x5051;
constexpr UINT_PTR kImageDimensionsTimer = 0x5052;
constexpr UINT_PTR kJumpContextTimer = 0x504d;
constexpr UINT kCustomUpdateMessage = WM_APP + 5;
constexpr UINT kTextUpdateMessage = WM_APP + 11;
constexpr UINT kPropertyValidationMessage = WM_APP + 12;
constexpr UINT kNavigationRootMessage = WM_APP + 13;
constexpr UINT kNavigationRootListingMessage = WM_APP + 14;
constexpr UINT kGitRootMessage = WM_APP + 15;
constexpr UINT kTreeListingMessage = WM_APP + 16;
constexpr int kLegacyChildFocus = -100;
constexpr int kCustomFocusBase = 1000;
// Open filter bar chips beside the type chips (kPickContextFilterBase + type).
constexpr int kPickFilterMore = 544;
constexpr int kPickTimeMenu = 545;      // time menu (includes only-new in open modes)
constexpr int kPickCompactFilters = 546;
constexpr int kPickCategoryBase = 550;  // + category (0 all .. kPickerCategoryCount - 1)
constexpr int kCustomEditBase = 2000;
constexpr size_t PULSE_CUSTOM_EDIT_MAX = 32767;
constexpr UINT_PTR kLoadTimer = 1;
constexpr UINT_PTR kSearchTimer = 2;
constexpr UINT_PTR kMotionTimer = 3;
constexpr UINT_PTR kMarqueeTimer = 4;
constexpr float kListWheelDip = 220.0f;  // one wheel notch in the List view (the main window's step)
constexpr UINT_PTR kHoverTimer = 5;
constexpr int kPathEditId = 101;
constexpr int kFilenameEditId = 102;
constexpr int kSearchEditId = 103;
constexpr UINT kSpinnerDelayMs = 150;
constexpr float kDefaultWidth = 1000.0f;
constexpr float kDefaultHeight = 640.0f;
constexpr float kMinWidth = 640.0f;
constexpr float kMinHeight = 420.0f;
constexpr float kSidebarDip = 208.0f;

// Sidebar sections. The ids only have to be distinct; they follow
// app::SidebarSectionId so the shared renderer sees familiar values.
constexpr int kGroupQuick = 1;
constexpr int kGroupDrives = 3;
constexpr int kGroupPickerTray = 50;
constexpr int kGroupAppCommon = 2;      // 「{程序} 常用」 (kPickerAppCommonSection)
constexpr int kGroupProgramPlaces = 6;  // 程序提供的位置
constexpr int kGroupHiddenNotice = 60;  // header-less: 已隐藏 N 组 · 显示
constexpr UINT_PTR kCommonForgetTimer = 0x5053;
constexpr DWORD kCommonUndoMs = 5000;
constexpr wchar_t kCommonUndoToken[] = L"pulse:common-undo";
constexpr wchar_t kShowHiddenGroupsToken[] = L"pulse:show-hidden-groups";
constexpr wchar_t kHiddenGroupPrefix[] = L"pulse:sidebar-group:";
constexpr wchar_t kPickerTrayPrefix[] = L"pulse-picker-tray:";

// Where the last pick of each mode was made, and how it was viewed, for this
// session.
std::wstring g_last_folder[4];
ViewMode g_last_view[4] = {ViewMode::Details, ViewMode::LargeIcons, ViewMode::Details, ViewMode::Details};

bool KeyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }

std::vector<PickerPlace> KnownPlaces() {
    struct Known {
        const KNOWNFOLDERID* id;
        l10n::StringId label;
        const wchar_t* glyph;
    };
    const Known known[] = {
        {&FOLDERID_Desktop, l10n::StringId::Desktop, L"\xE7F4"},
        {&FOLDERID_Downloads, l10n::StringId::Downloads, L"\xE896"},
        {&FOLDERID_Documents, l10n::StringId::PickerDocuments, L"\xE8B7"},
        {&FOLDERID_Pictures, l10n::StringId::PickerPictures, L"\xE8B7"},
        {&FOLDERID_Profile, l10n::StringId::PickerHome, L"\xE8B7"},
    };
    std::vector<PickerPlace> places;
    for (const Known& k : known) {
        std::wstring path = known_folders::CurrentPath(*k.id);
        if (!path.empty()) places.push_back({l10n::Get(k.label), std::move(path), k.glyph});
    }
    return places;
}

// 此电脑's four everyday folders (Home is reached from the address row).
std::vector<PickerPlace> StandardPlaces() {
    struct Known { const KNOWNFOLDERID* id; l10n::StringId label; const wchar_t* glyph; };
    const Known known[] = {
        {&FOLDERID_Desktop, l10n::StringId::Desktop, L"\xE7F4"},
        {&FOLDERID_Downloads, l10n::StringId::Downloads, L"\xE896"},
        {&FOLDERID_Documents, l10n::StringId::PickerDocuments, L"\xE8A5"},
        {&FOLDERID_Pictures, l10n::StringId::PickerPictures, L"\xEB9F"},
    };
    std::vector<PickerPlace> places;
    for (const Known& k : known) {
        std::wstring path = known_folders::CurrentPath(*k.id);
        if (!path.empty()) places.push_back({l10n::Get(k.label), std::move(path), k.glyph});
    }
    return places;
}

std::wstring DriveDetail(uint64_t free, uint64_t total) {
    return DriveSidebarDetail(free, total);
}

// Drives for the sidebar. Only local fixed disks are asked for their label
// and size: a sleeping network or optical drive must not stall the dialog.
std::vector<SidebarItem> ReadSidebarDrives() {
    std::vector<SidebarItem> items;
    const DWORD mask = GetLogicalDrives();
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
        SidebarItem item;
        item.path = root;
        item.is_drive = true;
        item.icon_glyph = L"\xE7F1";
        std::wstring label;
        const bool remote_like = ::pulse::fs::IsRemoteLikeDrive(root);  // no media query on cloud mounts
        if (type == DRIVE_FIXED && !remote_like) {
            wchar_t volume[MAX_PATH + 1]{};
            if (GetVolumeInformationW(root.c_str(), volume, MAX_PATH, nullptr, nullptr, nullptr,
                                      nullptr, 0))
                label = volume;
            ULARGE_INTEGER free{}, total{};
            if (GetDiskFreeSpaceExW(root.c_str(), &free, &total, nullptr) && total.QuadPart) {
                item.used_ratio = static_cast<float>(1.0 - static_cast<double>(free.QuadPart) /
                                                           static_cast<double>(total.QuadPart));
                item.detail = DriveDetail(free.QuadPart, total.QuadPart);
                item.free_bytes = free.QuadPart;
                item.total_bytes = total.QuadPart;
            }
        }
        if (label.empty()) {
            label = type == DRIVE_REMOTE || remote_like ? l10n::Get(l10n::StringId::NetworkDrive).c_str()
                  : type == DRIVE_CDROM ? l10n::Get(l10n::StringId::CDDrive).c_str()
                  : type == DRIVE_REMOVABLE ? l10n::Get(l10n::StringId::RemovableDisk_2).c_str()
                  : l10n::Get(l10n::StringId::LocalDisk_2).c_str();
        }
        item.label = label + L" (" + root.substr(0, 2) + L")";
        items.push_back(std::move(item));
    }
    SetThreadErrorMode(old_mode, nullptr);
    return items;
}

// "\\server\share" reads better whole than as just "share".
bool IsShareRootName(const std::wstring& path) {
    return path.rfind(L"\\\\", 0) == 0 && PickerParent(path).empty();
}

std::wstring ErrorText(DWORD error, const std::wstring& path) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_DIRECTORY: {
        std::wstring text = l10n::Get(l10n::StringId::PickerNotFound);
        const size_t at = text.find(L"{path}");
        // The full path is already in the address row; the one-line message
        // names the missing folder itself.
        std::wstring name = path::FriendlyPathText(path);
        while (name.size() > 3 && name.back() == L'\\') name.pop_back();
        const size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash + 1 < name.size() && !IsShareRootName(name))
            name = name.substr(slash + 1);
        if (at != std::wstring::npos) text.replace(at, 6, name);
        return text;
    }
    default:
        break;
    }
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = length && buffer ? std::wstring(buffer, length) : std::wstring();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' ||
                             text.back() == L' ' || text.back() == L'.' ||
                             text.back() == 0x3002))
        text.pop_back();
    return text;
}

PickerSort SortFromColumn(SortColumn column) {
    switch (column) {
    case SortColumn::Mtime: return PickerSort::Modified;
    case SortColumn::Size: return PickerSort::Size;
    case SortColumn::Type: return PickerSort::Type;
    default: return PickerSort::Name;
    }
}

SortColumn ColumnFromSort(PickerSort sort) {
    switch (sort) {
    case PickerSort::Modified: return SortColumn::Mtime;
    case PickerSort::Size: return SortColumn::Size;
    case PickerSort::Type: return SortColumn::Type;
    default: return SortColumn::Name;
    }
}

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        if (void* data = GlobalLock(memory)) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

class FolderPickerWindow {
public:
    ~FolderPickerWindow() noexcept { Close(); }
    bool Show(HWND owner, const FolderPickerSpec& spec, bool dark, D2D1_COLOR_F accent,
              FilePickerResult& out) {
        owner_ = owner;
        if (!owner_ && spec.placement_owner && IsWindow(spec.placement_owner)) owner_ = spec.placement_owner;
        dark_ = dark;
        accent_ = accent;
        spec_ = spec;
        mode_ = spec.mode;
        mode_index_ = mode_ == PickerMode::Image ? 1 : mode_ == PickerMode::File ? 2 : mode_ == PickerMode::Save ? 3 : 0;
        allow_multiselect_ = spec.allow_multiselect && mode_ != PickerMode::Save;
        options_.filters = spec.filters;
        options_.show_hidden = spec.show_hidden || spec.show_hidden_files;
        options_.show_protected_os_files = spec.show_hidden || spec.show_protected_os_files;
        options_.no_dereference_links = spec.no_dereference_links;
        options_.advisory_file_filters = spec.advisory_file_filters && !(mode_ == PickerMode::Save && (spec.native_options & FOS_STRICTFILETYPES));
        options_.native_options = spec.native_options;
        options_.skip_access_validation = spec.skip_access_validation;
        options_.allow_missing_open = spec.allow_missing_open;
        options_.require_existing_open_parent = spec.require_existing_open_parent;
        options_.validate_local_open_access_v1 = spec.validate_local_open_access_v1;
        if (options_.filters.empty() && mode_ != PickerMode::Folder) {
            if (mode_ == PickerMode::Image) {
                options_.filters = {{l10n::Get(l10n::StringId::SupportedImages).c_str(), L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.jfif"},
                    {L"JPEG (*.jpg; *.jpeg; *.jfif)", L"*.jpg;*.jpeg;*.jfif"}, {L"PNG (*.png)", L"*.png"},
                    {L"WebP (*.webp)", L"*.webp"}, {L"BMP (*.bmp)", L"*.bmp"}};
            } else options_.filters = {{l10n::Get(l10n::StringId::AllFiles_2).c_str(), L"*.*"}};
        }
        options_.filter_index = options_.filters.empty() ? 0 : (std::min)(spec.filter_index, options_.filters.size() - 1);
        if (spec.enable_archive_selection && !spec.shell_item_filter && (mode_ == PickerMode::File || mode_ == PickerMode::Image))
            options_.archive = std::make_shared<PickerArchiveSession>([] { return picker_places::Read().archive_worker; });
        view_mode_ = g_last_view[mode_index_];
        persist_layout_ = spec.persist_layout && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",nullptr,0)==0;
        if (persist_layout_ && picker_layout::Load(static_cast<int>(mode_),layout_settings_)) {
            view_mode_ = static_cast<ViewMode>(layout_settings_.view_mode);
            icon_dip_ = NormalizeIconDip(view_mode_, layout_settings_.icon_dip, scale_);
            options_.sort = static_cast<PickerSort>(layout_settings_.sort);
            options_.descending = layout_settings_.descending;
            for (unsigned i=0;i<std::size(collapsed_);++i) collapsed_[i]=(layout_settings_.collapsed_mask & (1u<<i))!=0;
        }
        if (IsZoomIconMode(view_mode_)) { last_icon_mode_ = view_mode_; last_icon_dip_ = icon_dip_; }
        filename_text_ = mode_ == PickerMode::Save && spec.native_save_defaults && UsesSaveNameMask(spec.filters)
            ? NativeSaveMaskFilename(spec.filename, spec.filters, spec.filter_index, spec.default_extension, false) : spec.filename;

        chrome_.mode = mode_;
        chrome_.title = (spec.explicit_title || !spec.title.empty()) ? spec.title
            : mode_ == PickerMode::Save ? l10n::Get(l10n::StringId::Save).c_str()
            : mode_ == PickerMode::File ? l10n::Get(l10n::StringId::Open).c_str()
            : l10n::Get(mode_ == PickerMode::Image ? l10n::StringId::TooltipChooseBackground
                                                   : l10n::StringId::PickerTitleFolder);
        chrome_.filename_label = spec.has_file_name_label ? spec.file_name_label : l10n::Get(mode_ == PickerMode::Folder ? l10n::StringId::FolderName : l10n::StringId::FileName);
        chrome_.primary_text = spec.has_ok_button_label ? spec.ok_button_label : l10n::Get(mode_ == PickerMode::Save ? l10n::StringId::Save : mode_ == PickerMode::File ? l10n::StringId::Open : mode_ != PickerMode::Folder
                                             ? l10n::StringId::PickerSelect
                                             : l10n::StringId::PickerSelectFolder);
        normal_primary_text_ = chrome_.primary_text;
        chrome_.cancel_text = spec.has_cancel_button_label ? spec.cancel_button_label : l10n::Get(l10n::StringId::Cancel);
        chrome_.hosted_edit = true;
        LoadSavedFormats();
        if (mode_ == PickerMode::File || mode_ == PickerMode::Image) {
            options_.category_of = MakeCategoryOf();
            FILETIME now{}; GetSystemTimeAsFileTime(&now);
            opened_at_ = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            if (persist_layout_) { try { options_.new_since = picker_layout::LoadLastOpened(static_cast<int>(mode_)); } catch (...) { } }
        }
        if (persist_layout_) {
            try { picker_flags_ = saved_flags_ = picker_layout::LoadFlags(static_cast<int>(mode_)); } catch (...) { }
            try { app_pinned_places_ = picker_layout::LoadAppPinned(); } catch (...) { }
        }
        original_file_path_ = spec.original_file_path;
        if (original_file_path_.empty() && mode_ == PickerMode::Save && !spec.filename.empty() && IsPickerDiskPath(spec.filename)) {
            original_file_path_ = spec.filename;
        }
        UpdateFilterText();
        focus_ = kPickList;

        pulse_places_ = spec.read_shared_places ? spec.read_shared_places() : picker_places::Read();
        if (!pulse_places_.shared_available && !spec.read_shared_places) {
            try { saved_places_ = std::async(std::launch::async, [] { return picker_places::ReadSaved(); }); }
            catch (...) { }
        }
        PickerProofPhase("known-places-before");
        RefreshKnownPlaces();
        PickerProofPhase("known-places-after");
        PickerProofPhase("sidebar-drives-before");
        drives_ = ReadSidebarDrives();
        PickerProofPhase("sidebar-drives-after");

        // A validated save draft is already an exact path from the caller,
        // not editable address-bar input (e.g. a literal %NAME% directory).
        std::wstring start = spec.validate_local_save_access_v1
            ? spec.initial_path : NormalizePickerInput(spec.initial_path);
        if (start.empty()) start = g_last_folder[mode_index_];
        if (start.empty()) start = fallback_;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner_ ? owner_ : GetDesktopWindow()))
               / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.hInstance = spec_.module_instance ? spec_.module_instance : GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kPickerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kPickerClass, &wc)) RegisterClassExW(&wc);

        SetLastError(ERROR_SUCCESS);
        PickerProofPhase("create-window-before");
        // Born on the owner's monitor: CW_USEDEFAULT lands on the primary one and
        // the move to a screen of another scale rescaled the size (WM_DPICHANGED).
        const POINT origin = OwnedWindowOrigin(owner_, static_cast<int>(kDefaultWidth * scale_),
                                               static_cast<int>(kDefaultHeight * scale_));
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kPickerClass, chrome_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
            origin.x, origin.y,
            static_cast<int>(kDefaultWidth * scale_), static_cast<int>(kDefaultHeight * scale_),
            owner_, nullptr, wc.hInstance, this);
        PickerProofPhase("create-window-after");
        if (!hwnd_) {
            if (!native_error_) native_error_ = GetLastError();
            if (!native_error_) native_error_ = ERROR_GEN_FAILURE;
            NoteFailure(native_error_, "create-window");
            return false;
        }
        // WM_CREATE read the DPI of the monitor the window landed on, which
        // can differ from the owner's guess above.
        CenterOwnedWindow(hwnd_, owner_, static_cast<int>(kDefaultWidth * scale_),
                          static_cast<int>(kDefaultHeight * scale_));
        if (persist_layout_) picker_layout::RestorePlacement(hwnd_,owner_,layout_settings_);
        loader_ = std::make_unique<PickerLoader>(hwnd_, kListingMessage, kValidationMessage,
            spec_.on_dialog_event ? kFolderCommitMessage : 0, kSaveDirectoriesMessage, spec_.enable_save_followup);
        if (!spec_.navigation_root.empty()) navigation_root_.Set(hwnd_, kNavigationRootListingMessage, spec_.navigation_root, options_);
        AddClipboardFormatListener(hwnd_);
        PickerProofPhase("clipboard-before");
        ReadCutClipboard();
        PickerProofPhase("clipboard-after");
        initial_load_ = true;
        if (spec_.on_dialog_event) {
            // The actual window exists, but no folder has been committed yet.
            // Initial type notification lets standard sinks acquire its HWND.
            PublishEmbeddedState();
            if (!done_) {
                embedded_ready_ = true;
                if (NotifyEmbedded(PickerDialogEvent::TypeChanged) != S_OK)
                    FailPaintedEdit(ERROR_FUNCTION_FAILED);
            }
            if (StopEmbeddedInitialization()) return false;
        }
        PickerProofPhase("navigate-before");
        Navigate(start, false);
        initial_jump_generation_ = generation_;
        PickerProofPhase("navigate-after");
        if (spec_.on_dialog_event && !WaitInitialFolderCommit()) return false;
        if (spec_.on_dialog_event) {
            // A veto leaves a navigable/cancellable window, not an implicit
            // fallback folder or an acceptable filename without a directory.
            UpdateChosen();
            if (StopEmbeddedInitialization()) return false;
        }
        if (spec_.owner_guard && !spec_.owner_guard->Alive()) { Close(); return false; }
        // Host callbacks can retrieve and reposition the HWND during setup.
        // Keep it hidden until its final layout has a composed frame.
        PickerProofPhase("hidden-first-frame-before");
        preparing_first_frame_ = true;
        Render();
        preparing_first_frame_ = false;
        PickerProofPhase("hidden-first-frame-after");
        if (done_ || !initial_frame_prepared_) {
            if (!done_) FailPaintedEdit(ERROR_FUNCTION_FAILED);
            return false;
        }
        initial_show_allowed_ = true;
        PickerProofPhase("show-window-before");
        ShowWindow(hwnd_, layout_settings_.maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
        layout_shown_ = true;
        if (NotifyEmbedded(PickerDialogEvent::Shown) != S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED, "notify-shown");
        PickerProofPhase("show-window-after");
        SetForegroundWindow(hwnd_);
        // Save dialogs and typed names start in the name field; otherwise the list.
        if (filename_edit_ && !filename_text_.empty()) FocusFilename();
        else SetFocus(hwnd_);
        PickerProofPhase("first-frame-before");
        if (spec_.pulse_painted_plain_child) FlushPaintedEdits(); else PresentEdits();
        PickerProofPhase("first-frame-after");
        PickerProofResult("ready-before-state-is-done", S_OK, native_error_, done_);
        if (spec_.owner_guard && !spec_.owner_guard->Alive()) done_ = true;
        submit_ready_ = !done_;
        if (!done_ && spec_.on_dialog_event) {
            PublishEmbeddedState();
            if (NotifyEmbedded(PickerDialogEvent::Ready) != S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED, "notify-ready");
        }
        if (!done_ && spec_.on_ready) {
            HRESULT notified = E_FAIL;
            PickerProofPhase("on-ready-before");
            try { notified = spec_.on_ready(hwnd_); } catch (...) {}
            PickerProofPhase("on-ready-after");
            if (notified != S_OK) { native_error_ = ERROR_FUNCTION_FAILED; NoteFailure(native_error_, "on-ready-rejected"); done_ = true; }
        }

        if (!done_ && pending_submit_) PostMessageW(hwnd_, WM_COMMAND, IDOK, 0);
        PickerProofPhase("modal-loop-enter");
        MSG message{};
        while (!done_) {
            // Sent messages can request Close while PeekMessage dispatches them.
            // Poll the cancellation handle independently of queue-ready wakeups.
            if(spec_.cancel_event && WaitForSingleObject(spec_.cancel_event,0)==WAIT_OBJECT_0) {PickerProofPhase("pump-cancel-polled");break;}
            if (spec_.owner_guard && !spec_.owner_guard->Alive()) break;
            if (spec_.cancel_event || spec_.owner_guard) {
                const DWORD wait = MsgWaitForMultipleObjectsEx(spec_.cancel_event ? 1 : 0, &spec_.cancel_event, 100,
                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
#if defined(PULSE_PICKER_INPROCESS_PROOF)
                static thread_local ULONGLONG last_wait_trace=0;
                const auto tick=GetTickCount64();wchar_t trace_enabled[2]{};
                if(tick-last_wait_trace>=1000 && GetEnvironmentVariableW(L"PULSE_PICKER_UI_DIAGNOSTICS",trace_enabled,2)==1 && trace_enabled[0]==L'1') {
                    last_wait_trace=tick;std::fprintf(stderr,"picker-wait handle=%p returned=%lu polled=%lu\n",spec_.cancel_event,wait,spec_.cancel_event?WaitForSingleObject(spec_.cancel_event,0):WAIT_FAILED);std::fflush(stderr);
                }
#endif
                if (spec_.cancel_event && wait == WAIT_OBJECT_0) {PickerProofPhase("pump-cancel-observed");break;}
                if (wait == WAIT_TIMEOUT) continue;
                if (wait == WAIT_FAILED) { native_error_ = GetLastError(); NoteFailure(native_error_, "pump-wait"); break; }
                const bool peeked=PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)!=FALSE;
                if(spec_.cancel_event && WaitForSingleObject(spec_.cancel_event,0)==WAIT_OBJECT_0) {PickerProofPhase("pump-cancel-after-sent-message");break;}
                if (!peeked) continue;
                if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
                if (LegacyDialogMessage(message)) continue;
                RedirectStrayModalKey(message, hwnd_);
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
                else { native_error_ = GetLastError(); if (!native_error_) native_error_ = ERROR_GEN_FAILURE; NoteFailure(native_error_, "pump-get-message"); }
                break;
            }
            if (LegacyDialogMessage(message)) continue;
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        PickerProofPhase("modal-close-before");
        Close();
        PickerProofPhase("modal-close-after");

        if (result_.empty()) return false;
        out.paths = result_;
        out.filter_index = options_.filter_index;
        out.navigation_source = archive_source_;
        if (spec_.enable_tray_consume && (mode_ == PickerMode::File || mode_ == PickerMode::Image) &&
            (picker_flags_ & picker_layout::kFlagRemoveTrayAfterOpen))
            out.on_result_delivered = PickerTrayDelivery(std::move(tray_pickups_), out.paths);
        if ((spec_.enable_save_followup || saving_to_tray_) && mode_ == PickerMode::Save && save_before_ && out.paths.size() == 1 &&
            pulse_places_.save_followup_available) {
            picker_save::Request request;
            request.path = out.paths.front(); request.before = *save_before_;
            request.source_pid = GetCurrentProcessId();
            static std::atomic<uint64_t> sequence{0};
            request.request_id = (GetTickCount64() << 24) ^ (static_cast<uint64_t>(request.source_pid) << 8) ^ ++sequence;
            if (!request.request_id) request.request_id = ++sequence;
            wchar_t image[32768]{};
            const DWORD length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
            if (length && length < std::size(image)) request.app_name = PickerLeafName(std::wstring(image, length));
            request.notify_if_no_tab = !saving_to_tray_ && (picker_flags_ & picker_layout::kFlagShowAfterSave) != 0;
            request.copy_path = !saving_to_tray_ && (picker_flags_ & picker_layout::kFlagCopyAfterSave) != 0;
            request.stage_to_tray = saving_to_tray_;
            auto save_delivery = PickerSaveDelivery(pulse_places_, std::move(request));
            out.on_result_delivered = [previous = std::move(out.on_result_delivered), save_delivery = std::move(save_delivery)] {
                if (previous) previous();
                save_delivery();
            };
        }
        if (archive_delivery_) {
            out.on_result_delivered = [lease = std::move(archive_delivery_), tray = std::move(out.on_result_delivered)] {
                lease->Commit();
                if (tray) tray();
            };
        }
        return true;
    }

    DWORD Error() const { return native_error_; }
    const FilePickerFailure& Failure() const { return failure_; }

public:
    HRESULT NavigateExternal(const std::wstring& requested) {
        if (embedded_callback_depth_) {
            if (external_navigation_queue_.size() >= 64) return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
            external_navigation_queue_.push_back(requested);
            if (!PostMessageW(hwnd_, kExternalNavigationMessage, 0, 0)) {
                external_navigation_queue_.pop_back(); return HRESULT_FROM_WIN32(GetLastError());
            }
            return E_PENDING;
        }
        if (external_navigation_ || waiting_ || validating_)
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        if (done_ || !loader_) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        std::wstring path = picker_navigation::IsComputerPath(requested) ? L"" : requested;
        external_navigation_ = true;
        struct Reset {
            FolderPickerWindow* self;
            ~Reset() {
                self->external_navigation_ = false;
                if (!self->done_ && !self->external_navigation_queue_.empty())
                    PostMessageW(self->hwnd_, kExternalNavigationMessage, 0, 0);
            }
        } reset{this};
        const HRESULT veto = NotifyEmbedded(PickerDialogEvent::FolderChanging,
            path.empty() ? picker_navigation::kComputerPath : path);
        if (veto != S_OK) return veto;
        if (done_) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        EndRename(false);
        create_rename_intents_.clear();
        select_after_paths_.clear();
        external_listing_.reset();
        BindListOrder(path, !SamePickerPath(path, current_));
        auto load_options = options_;
        load_options.search.clear();
        const HWND navigation_window = hwnd_;
        const bool was_enabled = IsWindowEnabled(navigation_window) != FALSE;
        EnableWindow(navigation_window, FALSE);
        struct Enable { HWND window; bool enabled; ~Enable() { if (IsWindow(window)) EnableWindow(window, enabled); } } enable{navigation_window, was_enabled};
        loader_->Load(++generation_, path, mode_, load_options);
        const auto deadline = GetTickCount64() + kInitialFolderTimeoutMs;
        while (!external_listing_) {
            if (done_ || (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
                ++generation_; return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }
            if (GetTickCount64() >= deadline) { ++generation_; return HRESULT_FROM_WIN32(ERROR_TIMEOUT); }
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    ++generation_; return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }
                TranslateMessage(&message); DispatchMessageW(&message);
            } else MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        auto listing = std::move(external_listing_);
        if (listing->error) return HRESULT_FROM_WIN32(listing->error);
        history_.Navigate(current_);
        searching_ = false; search_text_.clear(); options_.search.clear();
        if (search_edit_) { setting_edits_ = true; SetWindowTextW(search_edit_, L""); setting_edits_ = false; }
        EndMarquee(false);
        current_ = path;
        selected_ = anchor_ = -1;
        selected_set_.clear(); select_after_.clear(); typeahead_.clear();
        scroll_ = 0; scroll_x_ = 0; error_.clear();
        embedded_folder_pending_ = true;
        external_navigation_ = false;
        OnListing(std::move(listing));
        return done_ ? HRESULT_FROM_WIN32(native_error_ ? native_error_ : ERROR_CANCELLED) : S_OK;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<FolderPickerWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        try { return self->Handle(message, wparam, lparam); }
        catch (const std::bad_alloc&) { self->FailPaintedEdit(ERROR_NOT_ENOUGH_MEMORY); }
        catch (...) { self->FailPaintedEdit(ERROR_FUNCTION_FAILED); }
        if (message == WM_PAINT) ValidateRect(hwnd, nullptr);
        if (message == WM_NCCREATE) return FALSE;
        if (message == WM_CREATE) return -1;
        return 0;
    }

    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

    // ---- navigation -------------------------------------------------------

    bool WithinLegacyRoot(const std::wstring& path) const {
        if (!spec_.legacy_browse || !navigation_root_.Active()) return true;
        const auto candidate = std::filesystem::path(path).lexically_normal();
        const auto root = std::filesystem::path(navigation_root_.Path()).lexically_normal();
        if (!candidate.is_absolute() || !root.is_absolute()) return false;
        auto c = candidate.begin();
        for (const auto& part : root) {
            if (part.empty()) continue;
            if (c == candidate.end() || _wcsicmp(c->c_str(), part.c_str()) != 0) return false;
            ++c;
        }
        return true;
    }

    // By value: callers pass entry paths that the reset below destroys.
    // `verified`: the folder is known to exist (it was just listed as a row, or
    // is the parent of a folder that listed), so it may be picked while it
    // loads. Typed, remembered and history paths wait for their listing.
    void Navigate(std::wstring path, bool push_history, std::wstring select_after = {},
                  bool verified = false) {
        if (!WithinLegacyRoot(path)) return;
        jump_save_generation_ = 0;
        if (external_navigation_) return;
        if (push_history && !initial_load_) user_interacted_ = true;
        create_consent_paths_.clear();
        share_consent_paths_.clear(); share_consent_targets_.clear(); share_consent_save_identity_.reset(); share_consent_save_parent_.reset();
        EndRename(false);
        create_rename_intents_.clear();
        select_after_paths_.clear();
        CancelBlankClick();
        CancelPlacesDrag();
        const bool computer = picker_navigation::IsComputerPath(path);
        if (computer) path.clear();
        const bool moved = !SamePickerPath(path, current_) || (computer && !current_verified_);
        if (spec_.on_dialog_event && embedded_callback_depth_) {
            if (pending_navigation_.size() >= 64) { FailPaintedEdit(ERROR_NOT_ENOUGH_MEMORY); return; }
            pending_navigation_.push_back({std::move(path), push_history, std::move(select_after), verified});
            ++embedded_state_.generation; embedded_state_.candidate.clear(); embedded_state_.candidates.clear();
            PublishEmbeddedState();
            return;
        }
        if (spec_.on_dialog_event && embedded_ready_ && moved) {
            const HRESULT navigation = NotifyEmbedded(PickerDialogEvent::FolderChanging, path.empty() ? picker_navigation::kComputerPath : path);
            if (navigation == S_FALSE || done_) return;
            if (navigation != S_OK) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
            if ((spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0) ||
                (spec_.owner_guard && !spec_.owner_guard->Alive())) return;
        }
        if (moved) KeepSelectionInBasket();
        if (push_history && moved) history_.Navigate(current_);
        if (moved && searching_) {
            // A search belongs to the folder it was started in.
            searching_ = false;
            search_text_.clear();
            options_.search.clear();
            if (search_edit_) { setting_edits_ = true; SetWindowTextW(search_edit_, L""); setting_edits_ = false; }
        }
        ClearTree();
        current_ = path;
        if (mode_ == PickerMode::Save) {
            saving_to_tray_ = !current_.empty() && SamePickerPath(current_, picker_places::DefaultPickerShelfDirPath());
            if (moved) {
                save_total_files_cached_ = -1;
                StartGitRootProbe(current_);
            }
        } else {
            saving_to_tray_ = false;
        }
        if (spec_.on_dialog_event && moved) {
            embedded_folder_pending_ = true;
            embedded_folder_committed_ = false;
        }
        current_verified_ = verified;
        entries_.clear();
        DismissHover();
        image_dimensions_.Reset();
        snapshot_.reset();
        typeahead_.clear();
        selected_ = -1;
        selected_set_.clear();
        anchor_ = -1;
        if (moved && !basket_.empty() && filename_from_selection_) SyncSelectedNames();  // the name lists what the basket keeps
        chrome_.pressed = kPickNone;
        validating_ = false;
        chrome_.notice.clear();
        if (moved) scroll_ = scroll_x_ = 0.0f;
        reload_notice_.clear();
        error_.clear();
        waiting_ = true;
        loading_ = false;
        select_after_ = std::move(select_after);
        EndAddressEdit(false);
        UpdateChosen();
        BindListOrder(path, moved);
        if (loader_) loader_->Load(++generation_, path, mode_, options_);
        if (hwnd_) {
            // Navigation can hide the auto-jump notice. Reclaim its reserved
            // band immediately, including UndoAutoJump clearing the source.
            Relayout();
            SetTimer(hwnd_, kLoadTimer, kSpinnerDelayMs, nullptr);
            Invalidate();
        }
    }

    void GoUp() {
        if (current_.empty()) return;
        const std::wstring child = current_;
        Navigate(picker_shell::IsParsingName(child) ? picker_shell::Parent(child) : PickerParent(child), true, child, current_verified_);
    }

    void GoBack() {
        if (!history_.CanGoBack()) return;
        const std::wstring child = current_;
        Navigate(history_.Back(current_), false, child);
    }

    void GoForward() {
        if (!history_.CanGoForward()) return;
        Navigate(history_.Forward(current_), false);
    }

    void Reload() {
        const std::wstring keep = SelectedEntry() ? SelectedEntry()->path : std::wstring();
        const float scroll = scroll_, scroll_x = scroll_x_;
        Navigate(current_, false, keep, current_verified_);
        scroll_ = scroll; scroll_x_ = scroll_x;
    }

    bool WaitInitialFolderCommit() {
        const ULONGLONG deadline = GetTickCount64() + kInitialFolderTimeoutMs;
        while (embedded_folder_pending_ || !pending_navigation_.empty()) {
            if (StopEmbeddedInitialization()) return false;
            if (GetTickCount64() >= deadline) { FailPaintedEdit(ERROR_TIMEOUT); return false; }
            const DWORD wait = MsgWaitForMultipleObjectsEx(spec_.cancel_event ? 1 : 0, &spec_.cancel_event,
                50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_FAILED) { FailPaintedEdit(GetLastError()); return false; }
            MSG message{};
            for (unsigned count = 0; count < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam)); Cancel(); break;
                }
                const HWND pending_window = hwnd_;
                if (StopEmbeddedInitialization()) {
                    // PeekMessage can dispatch a sent Close before returning a
                    // posted payload; Close only drains messages still queued.
                    if (message.hwnd == pending_window) {
                        if (message.message == kFolderCommitMessage) PickerLoader::TakeFolderCommit(message.lParam);
                        else if (message.message == kListingMessage) PickerLoader::Take(message.lParam);
                        else if (message.message == kValidationMessage) PickerLoader::TakeValidation(message.lParam);
                        else if (message.message == kSaveDirectoriesMessage) PickerLoader::TakeSaveDirectories(message.lParam);
                    }
                    return false;
                }
                TranslateMessage(&message); DispatchMessageW(&message);
                if (StopEmbeddedInitialization()) return false;
            }
        }
        return !StopEmbeddedInitialization();
    }

    void OnFolderCommit(std::unique_ptr<PickerFolderCommit> commit) {
        if (external_navigation_) return;
        if (!commit || commit->generation != generation_ || !embedded_folder_pending_ ||
            !SamePickerPath(commit->path, current_)) return;
        current_verified_ = true;
        embedded_folder_committed_ = !current_.empty();
        embedded_folder_pending_ = false;
        initial_load_ = false;
        if (!initial_show_allowed_ && waiting_) loading_ = true;
        // Rows are still loading. Only the verified folder and its HWND become
        // available to initialization sinks before the first visible frame.
        UpdateChosen();
        if (embedded_ready_ && NotifyEmbedded(PickerDialogEvent::FolderChanged) != S_OK)
            FailPaintedEdit(ERROR_FUNCTION_FAILED);
        Invalidate();
    }

    void OnListing(std::unique_ptr<PickerListing> listing) {
        if (!listing || listing->generation != generation_) return;
        const auto listing_received_at = GetTickCount64();
        diagnostics::runtime::Event("picker_listing_received", {
            {"window", reinterpret_cast<uintptr_t>(hwnd_)}, {"generation", generation_},
            {"rows", listing->entries.size()}, {"shell_filter", spec_.shell_item_filter ? 1u : 0u}});
        CancelPlacesDrag();
        if (external_navigation_) { external_listing_ = std::move(listing); return; }
        KillTimer(hwnd_, kLoadTimer);
        waiting_ = false;
        loading_ = false;
        current_verified_ = listing->error == ERROR_SUCCESS;
        if (spec_.on_dialog_event)
            embedded_folder_committed_ = current_verified_ && !current_.empty();
        if (listing->error != ERROR_SUCCESS) {
            if (initial_load_ && !fallback_.empty() && !SamePickerPath(listing->path, fallback_)) {
                // A remembered or suggested folder that is gone: start from
                // the default place instead of an error page.
                Navigate(fallback_, false);
                return;
            }
            if (spec_.on_dialog_event && !initial_show_allowed_) {
                FailPaintedEdit(listing->error, "initial-listing");
                return;
            }
            error_ = ArchiveError(listing->error, listing->path);
            if (error_.empty()) error_ = l10n::Get(l10n::StringId::PickerOpenFailed);
        }
        initial_load_ = false;
        const bool notify_folder = embedded_folder_pending_ && listing->error == ERROR_SUCCESS;
        embedded_folder_pending_ = false;
        DismissHover();
        if (spec_.shell_item_filter && listing->error == ERROR_SUCCESS) {
            PublishEmbeddedState();
            const auto generation = listing->generation;
            waiting_ = loading_ = true;
            unsigned checks = 0; ULONGLONG next_yield = GetTickCount64() + 8;
            const auto current = [&] { return ContinueShellFilterBatch(generation, checks, next_yield); };
            const HRESULT filtered = FilterPickerShellListing(*listing,
                [&](const std::wstring& path) { return NotifyEmbedded(PickerDialogEvent::IncludeItem, path); }, current);
            if (filtered != S_OK) { AbandonShellFilterBatch(generation); return; }
            if (!current()) { AbandonShellFilterBatch(generation); return; }
            waiting_ = loading_ = false;
        }
        if (new_folder_draft_) EndRename(false);
        if (TreeEligible() && !listing->error) {
            tree_.Reset(*listing);
            auto view = tree_.Build();
            listing->entries = std::move(view.entries);
            listing->snapshot = std::move(view.snapshot);
            tree_rows_ = std::move(view.rows);
        }
        entries_ = std::move(listing->entries);
        diagnostics::runtime::Event("picker_listing_published", {
            {"window", reinterpret_cast<uintptr_t>(hwnd_)}, {"generation", generation_},
            {"filter_ms", GetTickCount64() - listing_received_at}, {"rows", entries_.size()}});
        snapshot_ = std::move(listing->snapshot);
        snapshot_order_ = order_;
        counts_valid_ = listing->counted;
        filter_counts_ = std::move(listing->filter_counts);
        category_counts_ = listing->category_counts;
        new_count_ = listing->new_count;
        if (current_.empty()) RefreshDrivesFromListing();
        if (!select_after_.empty() && select_after_paths_.empty()) select_after_paths_.push_back(select_after_);
        int arrival = -1;
        if (!select_after_paths_.empty()) {
            selected_set_.clear();
            for (size_t i = 0; i < entries_.size(); ++i) {
                if (std::any_of(select_after_paths_.begin(), select_after_paths_.end(),
                    [&](const auto& path) { return SamePickerPath(entries_[i].path, path); })) {
                    selected_set_.insert(static_cast<int>(i));
                    if (arrival < 0) arrival = static_cast<int>(i);
                    if (!allow_multiselect_) break;
                }
            }
            selected_ = anchor_ = arrival;
            SyncSelectedNames();
            select_after_paths_.clear();
            select_after_.clear();
        }
        RestoreBasketSelection();
        if (arrival >= 0) EnsureVisible(arrival);
        ClampScroll();
        UpdateChosen();
        if (notify_folder && embedded_ready_ && NotifyEmbedded(PickerDialogEvent::FolderChanged) != S_OK)
            FailPaintedEdit(ERROR_FUNCTION_FAILED);
        if (!external_navigation_queue_.empty()) PostMessageW(hwnd_, kExternalNavigationMessage, 0, 0);
        if (!reload_notice_.empty()) chrome_.notice = std::exchange(reload_notice_, {});
        std::vector<app::CreateRenameEntry> rename_entries;
        for (const auto& entry : entries_) rename_entries.push_back({entry.name, entry.path});
        if (const auto intent = app::SelectCreateRenameIntent(create_rename_intents_, current_, generation_,
            rename_entries, true, !rename_path_.empty())) {
            Select(static_cast<int>(intent->entry_index));
            app::ConsumeCreateRenameIntent(create_rename_intents_, intent->task_id);
            RenameSelection();
        }
        if (jump_save_generation_ == listing->generation) {
            jump_save_generation_ = 0;
            if (!done_ && !waiting_ && !validating_ && current_verified_ &&
                generation_ == listing->generation && pending_navigation_.empty() &&
                external_navigation_queue_.empty() && filename_text_ == jump_save_filename_)
                SubmitSelection();
        }
        Invalidate();
    }

    // This PC was just listed: its drive rows carry sizes for every drive,
    // including the ones the sidebar skipped at startup.
    void RefreshDrivesFromListing() {
        for (const PickerEntry& entry : entries_) {
            for (SidebarItem& item : drives_) {
                if (!SamePickerPath(item.path, entry.path) || !entry.size) continue;
                item.used_ratio = static_cast<float>(1.0 - static_cast<double>(entry.free) /
                                                           static_cast<double>(entry.size));
                item.detail = DriveDetail(entry.free, entry.size);
                item.free_bytes = entry.free;
                item.total_bytes = entry.size;
            }
        }
    }

    #include "picker_details_tree_controller.inc"

    const PickerEntry* SelectedEntry() const {
        if (TreePlaceholder(selected_)) return nullptr;
        if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return nullptr;
        return &entries_[static_cast<size_t>(selected_)];
    }

    static std::wstring ReadEdit(HWND edit) {
        const int length = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<size_t>((std::max)(length, 0)) + 1, L'\0');
        GetWindowTextW(edit, text.data(), length + 1);
        text.resize(static_cast<size_t>((std::max)(length, 0)));
        return text;
    }

    void UpdateFilterText() {
        chrome_.filter_text = options_.filters.empty()
            ? l10n::Get(l10n::StringId::AllFiles_2).c_str()
            : options_.filters[options_.filter_index].label;
        chrome_.filter_static = mode_ != PickerMode::Folder && options_.filters.size() <= 1;
        SyncFormatSegments();
    }

    // Save: the program's types as segments, 更多 for the rest, *.* last.
    void SyncFormatSegments() {
        chrome_.formats.clear();
        chrome_.format_count = options_.filters.size();
        chrome_.format_selected = -1;
        chrome_.format_note.clear();
        chrome_.format_static_text.clear();
        if (mode_ == PickerMode::Save && options_.filter_index < options_.filters.size()) {
            const auto& filter = options_.filters[options_.filter_index];
            chrome_.format_note = PickerFormatNote(filter);
            if (chrome_.filter_static) {
                chrome_.format_static_text = PickerFormatStaticText(filter);
                if (PickerFilterIsAny(filter)) chrome_.format_note.clear(); // Already explained by the static label.
            }
        }
        if (mode_ != PickerMode::Save || options_.filters.empty() || options_.filters.size() > 64) return;
        // Formats used in this dialog stay among the segments, so clicking one never
        // swaps the others out from under the pointer.
        if (options_.filter_index < options_.filters.size() && !PickerFilterIsAny(options_.filters[options_.filter_index])) {
            recent_formats_.erase(std::remove(recent_formats_.begin(), recent_formats_.end(), options_.filter_index), recent_formats_.end());
            recent_formats_.insert(recent_formats_.begin(), options_.filter_index);
        }
        const auto labels = PickerFormatLabels(options_.filters);
        auto recent = recent_formats_;
        recent.insert(recent.end(), saved_formats_.begin(), saved_formats_.end());
        const auto order = PickerFormatSegmentOrder(options_.filters, options_.filter_index, recent);
        size_t regular = 0, shown = 0;
        for (const auto& filter : options_.filters) if (!PickerFilterIsAny(filter)) ++regular;
        for (const int index : order) if (index >= 0 && !PickerFilterIsAny(options_.filters[static_cast<size_t>(index)])) ++shown;
        for (const int index : order) {
            PickerFormatSegment segment;
            if (index == kPickerFormatMore) {
                segment.text = l10n::Get(l10n::StringId::More) + L" " + std::to_wstring(regular - shown);
            } else {
                const auto filter = static_cast<size_t>(index);
                segment.text = labels[filter];
                segment.filter = index;
                segment.any = PickerFilterIsAny(options_.filters[filter]);
                if (filter == options_.filter_index) chrome_.format_selected = static_cast<int>(chrome_.formats.size());
            }
            chrome_.formats.push_back(std::move(segment));
        }
    }

    bool FormatSegmentsShown() const { return !chrome_layout_.format_segments.empty(); }

    void ActivateFormat(int segment) {
        if (segment < 0 || static_cast<size_t>(segment) >= chrome_.formats.size()) return;
        const auto& format = chrome_.formats[static_cast<size_t>(segment)];
        if (format.filter >= 0) { ApplyFilter(static_cast<size_t>(format.filter)); return; }
        const auto anchor = static_cast<size_t>(segment) < chrome_layout_.format_segments.size()
            ? chrome_layout_.format_segments[static_cast<size_t>(segment)] : chrome_layout_.filter;
        ShowFormatList(anchor);
    }

    // 更多: every type the program passed, searchable by name or extension.
    void ShowFormatList(const D2D1_RECT_F& anchor) {
        if (options_.filters.empty()) return;
        const auto labels = PickerFormatLabels(options_.filters);
        const auto lower = [](std::wstring text) {
            for (auto& c : text) c = static_cast<wchar_t>(std::towlower(c));
            return text;
        };
        const auto build = [this, labels, lower](const std::wstring& typed) {
            std::wstring query = lower(typed);
            while (!query.empty() && std::iswspace(query.front())) query.erase(query.begin());
            while (!query.empty() && std::iswspace(query.back())) query.pop_back();
            if (query.rfind(L"*.", 0) == 0) query.erase(0, 2);
            else if (!query.empty() && query.front() == L'.') query.erase(0, 1);
            std::vector<FluentMenuItem> items;
            for (size_t i = 0; i < options_.filters.size() && i < 64; ++i) {
                const auto& filter = options_.filters[i];
                const auto name = PickerFilterName(filter);
                const auto extensions = PickerFilterExtensions(filter);
                if (!query.empty() && lower(labels[i]).find(query) == std::wstring::npos &&
                    lower(name).find(query) == std::wstring::npos &&
                    std::none_of(extensions.begin(), extensions.end(), [&](const std::wstring& e) { return e.rfind(query, 0) == 0; }))
                    continue;
                FluentMenuItem item;
                item.command = static_cast<int>(i) + 1;
                item.text = labels[i];
                if (name != labels[i]) { item.detail = name; item.detail_inline = true; }
                std::wstring shown;
                for (size_t e = 0; e < extensions.size() && e < 4; ++e) shown += (e ? L" ." : L".") + extensions[e];
                if (extensions.size() > 4) shown += L" \u2026";
                item.shortcut = shown;
                item.radio_group = true;
                item.radio = i == options_.filter_index;
                item.current = i == options_.filter_index;
                items.push_back(std::move(item));
            }
            return items;
        };
        menu_.SetTheme(dark_, accent_);
        menu_.SetFilterPlaceholder(l10n::Get(l10n::StringId::PickerFormatSearch));
        menu_.SetFilterMinWidth(360.0f);
        const RECT screen = ScreenRect(anchor);
        const int command = TrackPickerPopup(POINT{screen.left, screen.top}, build(std::wstring()), build);
        if (done_ || command <= 0) return;
        ApplyFilter(static_cast<size_t>(command - 1));
    }

    // Save types this program saved with before, as filter indices (most recent first).
    void LoadSavedFormats() {
        saved_format_patterns_.clear();
        saved_formats_.clear();
        if (!persist_layout_ || mode_ != PickerMode::Save) return;
        try { saved_format_patterns_ = picker_layout::LoadRecentFormats(static_cast<int>(mode_)); } catch (...) { return; }
        for (const auto& pattern : saved_format_patterns_)
            for (size_t i = 0; i < options_.filters.size(); ++i)
                if (CompareStringOrdinal(options_.filters[i].pattern.c_str(), -1, pattern.c_str(), -1, TRUE) == CSTR_EQUAL) {
                    if (std::find(saved_formats_.begin(), saved_formats_.end(), i) == saved_formats_.end()) saved_formats_.push_back(i);
                    break;
                }
    }

    void SaveUsedFormat() noexcept {
        if (!persist_layout_ || mode_ != PickerMode::Save || result_.empty() || options_.filter_index >= options_.filters.size()) return;
        const auto& used = options_.filters[options_.filter_index];
        if (PickerFilterIsAny(used) || used.pattern.empty()) return;
        try {
            std::vector<std::wstring> patterns{used.pattern};
            for (const auto& pattern : saved_format_patterns_)
                if (CompareStringOrdinal(pattern.c_str(), -1, used.pattern.c_str(), -1, TRUE) != CSTR_EQUAL &&
                    patterns.size() < picker_layout::kMaxRecentFormats) patterns.push_back(pattern);
            picker_layout::SaveRecentFormats(static_cast<int>(mode_), patterns);
        } catch (...) { }
    }

    bool StopEmbeddedInitialization() {
        if (!spec_.on_dialog_event) return false;
        if (done_ || (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0) ||
            (spec_.owner_guard && !spec_.owner_guard->Alive())) {
            result_.clear(); Close(); return true;
        }
        return false;
    }

    HRESULT NotifyEmbedded(PickerDialogEvent kind, const std::wstring& proposed = {}) {
        if (!spec_.on_dialog_event || done_) return S_OK;
        ++embedded_callback_depth_;
        HRESULT hr = E_FAIL;
        try {
            const auto host_path = kind == PickerDialogEvent::FolderChanging && options_.archive && options_.archive->Contains(proposed)
                ? PickerParent(options_.archive->ArchiveFor(proposed)) : proposed;
            hr = spec_.on_dialog_event(hwnd_, kind, embedded_state_, host_path);
        } catch (...) {}
        --embedded_callback_depth_;
        if (!embedded_callback_depth_ && !pending_navigation_.empty() && !done_ &&
            !PostMessageW(hwnd_, kEmbeddedNavigationMessage, 0, 0)) FailPaintedEdit(ERROR_FUNCTION_FAILED);
        return hr;
    }
    void ClearEmbeddedCandidate() {
        if (!spec_.on_dialog_event || embedded_state_.candidate.empty()) return;
        embedded_state_.candidate.clear(); embedded_state_.candidates.clear(); ++embedded_state_.generation;
        if (NotifyEmbedded(PickerDialogEvent::Snapshot) != S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED);
    }
    void PublishEmbeddedState() {
        if (!spec_.on_dialog_event || done_) return;
        std::vector<std::wstring> selections;
        if(allow_multiselect_) {
            std::vector<int> indices(selected_set_.begin(),selected_set_.end());
            std::sort(indices.begin(),indices.end());
            for(int index:indices) if(index>=0 && static_cast<size_t>(index)<entries_.size()) selections.push_back(entries_[static_cast<size_t>(index)].path);
        } else if(const auto* selected=SelectedEntry()) selections.push_back(selected->path);
        if (InArchive()) selections.clear();
        const std::wstring selection=selections.empty() ? std::wstring{} : selections.front();
        const bool selection_changed = embedded_state_.selections != selections;
        const bool type_changed = embedded_state_.filter_index != options_.filter_index;
        const std::wstring published_folder = InArchive() ? PickerParent(options_.archive->ArchiveFor(current_))
            : current_.empty() && current_verified_ ? picker_navigation::kComputerPath : current_;
        const bool changed = selection_changed || type_changed || embedded_state_.folder != published_folder ||
            embedded_state_.filename != filename_text_ || embedded_state_.validating != validating_;
        // The first published snapshot is valid even when every field equals
        // its empty default. Zero is reserved for an unpublished snapshot.
        if (changed || embedded_state_.generation == 0) {
            ++embedded_state_.generation; embedded_state_.candidate.clear(); embedded_state_.candidates.clear();
            embedded_state_.folder = published_folder; embedded_state_.filename = filename_text_;
            embedded_state_.selection = selection; embedded_state_.selections=std::move(selections); embedded_state_.filter_index = options_.filter_index;
            embedded_state_.validating = validating_;
        }
        if (NotifyEmbedded(PickerDialogEvent::Snapshot) != S_OK) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
        if (embedded_ready_ && selection_changed && NotifyEmbedded(PickerDialogEvent::SelectionChanged) != S_OK)
            FailPaintedEdit(ERROR_FUNCTION_FAILED);
        if (embedded_ready_ && type_changed && NotifyEmbedded(PickerDialogEvent::TypeChanged) != S_OK)
            FailPaintedEdit(ERROR_FUNCTION_FAILED);
    }

    void RememberSaveFocus() {
        if (mode_ != PickerMode::Save || save_focus_pending_) return;
        const HWND focused = GetFocus();
        if (focused != hwnd_ && (!focused || !IsChild(hwnd_, focused))) return;
        save_focus_pending_ = true; save_focus_before_validation_ = focused;
        PickerProofFocus("save-focus-remember", focus_, focused == hwnd_ ? 0 : GetDlgCtrlID(focused));
    }
    void RestoreSaveFocus() {
        if (mode_ != PickerMode::Save || !save_focus_pending_ || validating_ || waiting_) return;
        const HWND previous = save_focus_before_validation_;
        save_focus_pending_ = false; save_focus_before_validation_ = nullptr;
        // Never activate a window or undo a user's subsequent focus decision.
        // The pending restoration is consumed even if the user left this dialog.
        if (done_ || GetActiveWindow() != hwnd_ || GetForegroundWindow() != hwnd_ ||
            (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
            (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) return;
        const HWND focused = GetFocus();
        if (focused && IsWindow(focused) && IsWindowVisible(focused) && IsWindowEnabled(focused)) return;
        const bool known_edit = previous && (previous == filename_edit_ || previous == search_edit_ || previous == path_edit_);
        HWND target = known_edit && IsWindow(previous) && IsChild(hwnd_, previous) &&
            IsWindowVisible(previous) && IsWindowEnabled(previous) ? previous : filename_edit_;
        if (!target || !IsWindow(target) || !IsChild(hwnd_, target) || !IsWindowVisible(target) || !IsWindowEnabled(target)) return;
        PickerProofFocus("save-focus-restore", focus_, GetDlgCtrlID(target));
        SetFocus(target); // Preserve text/selection; do not select all or synthesize input.
    }
    static constexpr UINT kDetailsPreviewNavigateMessage = WM_APP + 231;
    static constexpr UINT kDetailsPreviewOpenMessage = WM_APP + 232;

    bool HasRequiredInteraction() const {
        return !(spec_.native_options & FOS_OKBUTTONNEEDSINTERACTION) || user_interacted_;
    }

    void UpdateChosen() {
        std::erase_if(selected_set_, [&](int i) { return TreePlaceholder(i); });
        if (pending_overwrite_ && (!validating_ || overwrite_generation_ != generation_)) ClearOverwrite();
        PublishEmbeddedState();
        const bool no_embedded_folder = spec_.on_dialog_event && !embedded_folder_committed_;
        if (filename_edit_) EnableWindow(filename_edit_, !validating_ && !waiting_ && !no_embedded_folder);
        if (!no_embedded_folder) RestoreSaveFocus();
        chosen_.clear();
        if (!validating_ && !waiting_ && !no_embedded_folder && HasRequiredInteraction()) {
            if (!filename_text_.empty()) chosen_ = filename_text_;
            else if (mode_ == PickerMode::Folder && error_.empty())
                chosen_ = PickerChosenPath(mode_, current_, SelectedEntry());
        }
        RefreshSaveDraft();
    }

    void ClearOverwrite() {
        pending_overwrite_.reset();
        const bool had_banner = chrome_.overwrite_pending;
        chrome_.overwrite_pending = false;
        if (had_banner && hwnd_) Relayout();  // the banner row goes
        if (hwnd_) RemovePropW(hwnd_, L"Pulse.PickerOverwriteGeneration");
        chrome_.notice.clear();
    }
    std::wstring OverwriteAlternative(const std::wstring& path) const {
        const auto leaf = PickerLeafName(path);
        const auto dot = leaf.find_last_of(L'.');
        const auto stem = dot == leaf.npos || dot == 0 ? leaf : leaf.substr(0, dot);
        const auto extension = dot == leaf.npos || dot == 0 ? std::wstring{} : leaf.substr(dot);
        for (unsigned suffix = 2; suffix < 10000; ++suffix) {
            auto candidate = stem + L" (" + std::to_wstring(suffix) + L")" + extension;
            if (std::none_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
                return CompareStringOrdinal(entry.name.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL;
            })) return candidate;
        }
        return stem + L" (10000)" + extension;
    }
    void ResolveOverwrite(bool replace, bool rename = false) {
        if (!pending_overwrite_) return;
        auto snapshot = std::move(*pending_overwrite_);
        const bool current = overwrite_generation_ == generation_ && validating_ && !waiting_;
        ClearOverwrite();
        if (!current || !loader_ || done_) { validating_ = false; UpdateChosen(); Invalidate(); return; }
        if (replace) {
            loader_->RevalidateSave(++generation_, std::move(snapshot));
        } else {
            ++generation_; validating_ = false; ClearEmbeddedCandidate();
            if (rename) {
                const auto parent = PickerParent(snapshot.Path());
                const auto candidate = OverwriteAlternative(snapshot.Path());
                SetFilename(SamePickerPath(parent, current_) ? candidate :
                    parent + (parent.back() == L'\\' ? L"" : L"\\") + candidate);
            }
            UpdateChosen(); FocusFilename();
            if (rename) SubmitSelection();
        }
        UpdateChosen(); Invalidate();
    }

    void SetFilename(std::wstring text) {
        save_format_notice_.clear(); save_format_notice_filename_.clear();
        filename_text_ = std::move(text);
        setting_edits_ = true;
        if (filename_edit_) SetWindowTextW(filename_edit_, filename_text_.c_str());
        setting_edits_ = false;
        UpdateChosen();
    }

    // ---- the multi-select basket (HTML 多选篮): picks survive changing folders ----

    bool BasketEnabled() const { return allow_multiselect_ && mode_ == PickerMode::File; }

    // The files picked here last. Opening a folder starts with a click that selects
    // just the folder, so that click keeps them; clearing the selection drops them.
    void RememberFilePicks() {
        if (!BasketEnabled()) return;
        if (selected_set_.empty()) { last_file_picks_.clear(); return; }
        std::vector<int> picked(selected_set_.begin(), selected_set_.end());
        std::sort(picked.begin(), picked.end());
        std::vector<BasketItem> files;
        for (int index : picked) {
            if (index < 0 || static_cast<size_t>(index) >= entries_.size()) continue;
            const auto& entry = entries_[static_cast<size_t>(index)];
            if (entry.kind == PickerEntryKind::File || entry.kind == PickerEntryKind::Image)
                files.push_back({entry.path, entry.name, entry.size});
        }
        if (!files.empty()) last_file_picks_ = std::move(files);
    }

    void KeepSelectionInBasket() {
        if (!BasketEnabled() || !filename_from_selection_ || editing_selection_names_) { last_file_picks_.clear(); return; }
        for (auto& pick : last_file_picks_) {
            if (std::any_of(basket_.begin(), basket_.end(), [&](const BasketItem& item) { return SamePickerPath(item.path, pick.path); })) continue;
            basket_.push_back(std::move(pick));
        }
        last_file_picks_.clear();
    }

    // Back in a folder: its basket files are selected rows again.
    void RestoreBasketSelection() {
        if (basket_.empty()) return;
        bool restored = false;
        for (auto it = basket_.begin(); it != basket_.end();) {
            int found = -1;
            if (SamePickerPath(PickerParent(it->path), current_))
                for (size_t i = 0; i < entries_.size(); ++i)
                    if (SamePickerPath(entries_[i].path, it->path)) { found = static_cast<int>(i); break; }
            if (found < 0) { ++it; continue; }
            selected_set_.insert(found);
            if (selected_ < 0) selected_ = anchor_ = found;
            it = basket_.erase(it);
            restored = true;
        }
        if (restored) { filename_from_selection_ = true; SyncSelectedNames(); }
    }

    void SyncSelectedNames() {
        std::erase_if(selected_set_, [&](int i) { return TreePlaceholder(i); });
        RememberFilePicks();
        std::vector<int> picked(selected_set_.begin(), selected_set_.end());
        std::sort(picked.begin(), picked.end());
        std::wstring text;
        // Files kept from other folders go first, as full paths.
        const size_t total = basket_.size() + picked.size();
        for (const auto& item : basket_) {
            if (!text.empty()) text += L" ";
            text += total > 1 ? L"\"" + item.path + L"\"" : item.path;
        }
        for (int i : picked) {
            if (i < 0 || i >= static_cast<int>(entries_.size())) continue;
            const PickerEntry& entry = entries_[static_cast<size_t>(i)];
            // Folders are opened, not returned: they do not go into the name.
            if (mode_ != PickerMode::Folder && (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive)) continue;
            if (!text.empty()) text += L" ";
            const auto name = TreeSelectionName(entry);
            text += total > 1 ? L"\"" + name + L"\"" : name;
        }
        if (mode_ != PickerMode::Save || !text.empty()) { SetFilename(std::move(text)); filename_from_selection_ = true; }
    }

    void Select(int index, bool extend = false, bool toggle = false, bool focus_only = false,
                bool reveal = true) {
        if (validating_) { ++generation_; validating_ = false; }
        editing_selection_names_ = false;
        if (TreePlaceholder(index)) return;
        const int count = static_cast<int>(entries_.size());
        if (!count) { selected_ = -1; selected_set_.clear(); UpdateChosen(); return; }
        index = std::clamp(index, 0, count - 1);
        const int prev_selected = selected_;
        selected_ = index;
        if (!focus_only) {
            user_interacted_ = true;
            if (allow_multiselect_ && extend) {
                const int anchor = anchor_ >= 0 ? anchor_ : (prev_selected >= 0 ? prev_selected : index);
                anchor_ = anchor;
                if (!toggle) selected_set_.clear();
                for (int i = (std::min)(anchor, index); i <= (std::max)(anchor, index); ++i)
                    if (!TreePlaceholder(i)) selected_set_.insert(i);
            } else if (allow_multiselect_ && toggle) {
                if (!selected_set_.erase(index)) selected_set_.insert(index);
                anchor_ = index;
            } else {
                selected_set_ = {index};
                anchor_ = index;
            }
            SyncSelectedNames();
        }
        if (reveal) EnsureVisible(index);
        chrome_.notice.clear();
        UpdateChosen();
        Invalidate();
    }

    void ClearSelection() {
        selected_ = -1;
        selected_set_.clear();
        last_file_picks_.clear();
        anchor_ = -1;
        if (mode_ != PickerMode::Folder && mode_ != PickerMode::Save) SetFilename(L"");
        UpdateChosen();
        Invalidate();
    }

    bool InArchive() const { return options_.archive && options_.archive->Contains(current_); }

    std::wstring ArchiveError(DWORD error, const std::wstring& path) const {
        if (options_.archive && options_.archive->Contains(path)) {
            if (error == ERROR_MOD_NOT_FOUND) return l10n::Get(l10n::StringId::ArchivePluginRequired);
            if (error == ERROR_PASSWORD_RESTRICTION) return l10n::Get(l10n::StringId::PickerArchivePassword);
        }
        return ErrorText(error, path);
    }

    void OpenEntry(int index) {
        if (TreePlaceholder(index)) return;
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        const PickerEntry entry = entries_[static_cast<size_t>(index)];
        if (entry.archive && options_.archive) {
            std::erase_if(last_file_picks_, [&](const auto& pick) { return SamePickerPath(pick.path, entry.path); });
            std::erase_if(basket_, [&](const auto& pick) { return SamePickerPath(pick.path, entry.path); });
            options_.archive->Register(entry.path);
            SetFilename(L"");
            Navigate(entry.path, true, {}, true);
        } else if (entry.kind == PickerEntryKind::Image || entry.kind == PickerEntryKind::File) {
            SubmitSelection();
        } else {
            if (mode_ != PickerMode::Save) SetFilename(L"");
            Navigate(entry.path, true, {}, true);
        }
    }

    void TraceFolderSubmit(const wchar_t* phase, uint64_t detail = 0) const noexcept {
        if (mode_ != PickerMode::Folder || !spec_.on_dialog_event) return;
        diagnostics::runtime::TextEvent("picker_folder_submit", {{"phase", phase}}, {
            {"window", reinterpret_cast<UINT_PTR>(hwnd_)}, {"generation", generation_},
            {"detail", detail},
            {"ready", submit_ready_}, {"committed", embedded_folder_committed_},
            {"waiting", waiting_}, {"validating", validating_}, {"primary_enabled", chrome_.primary_enabled},
            {"selected_count", selected_set_.size()}, {"has_chosen", !chosen_.empty()},
            {"filename_from_selection", filename_from_selection_},
        });
    }

    void ValidateNames(std::vector<std::wstring> names, bool typed_names = false, bool directories_checked = false,
                       bool incoming_files = false, bool add_to_selection = false,
                       const std::vector<picker_places::SelectedItem>* selection_items = nullptr) {
        if (!loader_ || validating_ || waiting_ || names.empty()) {
            TraceFolderSubmit(!loader_ ? L"no_loader" : validating_ ? L"already_validating" :
                waiting_ ? L"listing_pending" : L"no_candidate");
            return;
        }
        if (mode_ == PickerMode::Save) save_before_.reset();
        if (!allow_multiselect_ && names.size() > 1) {
            TraceFolderSubmit(L"too_many_candidates", names.size());
            chrome_.notice = l10n::Get(l10n::StringId::SelectOneFileHere).c_str();
            Invalidate();
            return;
        }
        validation_navigation_only_ = false;
        validation_incoming_files_ = incoming_files;
        validation_add_selection_ = add_to_selection;
        validation_selection_items_ = add_to_selection ? (selection_items ? *selection_items : spec_.pulse_selected_items) : std::vector<picker_places::SelectedItem>{};
        create_consent_paths_.clear();
        share_consent_paths_.clear(); share_consent_targets_.clear(); share_consent_save_identity_.reset(); share_consent_save_parent_.reset();
        RememberSaveFocus();
        validating_ = true;
        if (mode_ == PickerMode::Save) { save_draft_probe_.Cancel(); save_draft_due_ = 0; }
        chrome_.notice = l10n::Get(l10n::StringId::CheckingTheSelectedItems).c_str();
        UpdateChosen();
        if (mode_ == PickerMode::Save) {
            SaveTargetRequest request;
            request.directory = current_;
            request.filename = names.front();
            request.filters = options_.filters;
            request.filter_index = options_.filter_index;
            request.default_extension = spec_.default_extension;
            request.native_save_defaults = spec_.native_save_defaults;
            request.filter_changed = save_filter_changed_;
            request.caller_validates_access = spec_.skip_access_validation;
            request.reject_readonly_save = spec_.reject_readonly_save;
            request.no_test_file_create = spec_.no_test_file_create;
            request.require_existing_save_parent = spec_.require_existing_save_parent;
            const std::filesystem::path input(request.filename);
            if (!directories_checked && input.has_parent_path() && !input.is_absolute() && !input.has_root_name() &&
                !input.has_root_directory() && !request.require_existing_save_parent && !request.no_test_file_create &&
                !(spec_.native_options & (FOS_PATHMUSTEXIST | FOS_NOTESTFILECREATE)))
                loader_->PrepareSaveDirectories(++generation_, std::move(request), spec_.native_options);
            else loader_->ValidateSave(++generation_, std::move(request));
        } else {
            auto validation_options = options_;
            validation_options.use_default_extension = spec_.use_open_default_extension;
            validation_options.default_extension = spec_.default_extension;
            validation_options.extension_filter_changed = open_filter_changed_;
            validation_options.typed_names = typed_names;
            ++generation_;
            TraceFolderSubmit(L"validation_queued", names.size());
            loader_->Validate(generation_, current_, std::move(names), mode_, std::move(validation_options));
        }
        Invalidate();
    }

    void SubmitSelection(bool allow_live_conflict = false) {
        if (mode_ == PickerMode::Save && (save_name_invalid_ || (chrome_.live_conflict && !allow_live_conflict))) return;
        TraceFolderSubmit(L"submit_enter");
        if (done_) { TraceFolderSubmit(L"already_closed"); return; }
        if (!legacy_browse_ok_) return;
        // Initial notifications can pump caller commands before the first
        // frame. Coalesce OK until edits and the visible frame are ready.
        if (!submit_ready_) { pending_submit_ = true; TraceFolderSubmit(L"submit_deferred"); return; }
        pending_submit_ = false;
        if (navigation_root_.Active() && !navigation_root_.RootLoaded()) { TraceFolderSubmit(L"root_pending"); return; }
        if (!invalid_properties_.empty()) {
            TraceFolderSubmit(L"invalid_properties", invalid_properties_.size());
            chrome_.notice = ErrorText(ERROR_INVALID_DATA, L""); Invalidate(); return;
        }
        if (new_folder_draft_) { TraceFolderSubmit(L"new_folder_draft"); return; }
        if (spec_.on_dialog_event && !embedded_folder_committed_) { TraceFolderSubmit(L"folder_not_committed"); return; }
        if (!FlushPaintedEdits()) { TraceFolderSubmit(L"submit_edits_failed"); return; }
        if (validating_ || !HasRequiredInteraction()) {
            TraceFolderSubmit(validating_ ? L"already_validating" : L"interaction_required");
            return;
        }
        std::vector<std::wstring> names;
        if (mode_ == PickerMode::Folder && filename_from_selection_ && !selected_set_.empty()) {
            std::vector<int> picked(selected_set_.begin(), selected_set_.end());
            std::sort(picked.begin(), picked.end());
            for (int index : picked) if (index >= 0 && static_cast<size_t>(index) < entries_.size()) {
                const auto& entry = entries_[static_cast<size_t>(index)];
                if (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive) names.push_back(entry.path);
            }
        } else if ((mode_ == PickerMode::File || mode_ == PickerMode::Image) && filename_from_selection_ && (!selected_set_.empty() || !basket_.empty())) {
            for (const auto& item : basket_) names.push_back(item.path);
            std::vector<int> picked(selected_set_.begin(), selected_set_.end());
            std::sort(picked.begin(), picked.end());
            for (int index : picked) if (index >= 0 && static_cast<size_t>(index) < entries_.size()) {
                const auto& entry = entries_[static_cast<size_t>(index)];
                if (entry.kind == PickerEntryKind::File || entry.kind == PickerEntryKind::Image) names.push_back(entry.path);
            }
        } else if (mode_ == PickerMode::Save && !filename_text_.empty()) {
            // Save validates one exact leaf; do not trim a trailing dot/space or
            // reinterpret quoted text as multiple paths before the save model.
            names.push_back(filename_text_);
        } else if (!filename_text_.empty()) {
            if (!ParsePickerNames(filename_text_, names)) {
                TraceFolderSubmit(L"invalid_filename_syntax");
                chrome_.notice = l10n::Get(l10n::StringId::UseMatchingQuotesAroundEachFilename).c_str();
                Invalidate();
                return;
            }
        } else if (!chosen_.empty()) {
            names.push_back(chosen_);
        }
        ValidateNames(std::move(names), mode_ == PickerMode::File && !filename_from_selection_);
    }

    void OnSaveDirectories(std::unique_ptr<PickerSaveDirectories> reply) {
        if (!CheckPaintedEdits() || !reply || done_ || !validating_ || reply->generation != generation_ ||
            (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
            (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) return;
        chrome_.notice.clear();
        if (reply->creating && reply->creation.error) {
            validating_ = false;
            chrome_.notice = ErrorText(reply->creation.error, reply->creation.failed_path);
            if (!reply->creation.created.empty()) {
                chrome_.notice += L"\n" + l10n::Get(l10n::StringId::PickerCreatedFoldersRemain);
                for (const auto& path : reply->creation.created) chrome_.notice += L"\n" + path;
            }
            UpdateChosen(); Invalidate(); return;
        }
        if (!reply->creating && !reply->plan.error && !reply->plan.missing.empty()) {
            const auto prompt_generation = generation_;
            // ConfirmDialog displays at most five rows. Do not hide any directory
            // behind its count-only overflow; gather consent for every batch first.
            for (size_t first = 0; first < reply->plan.missing.size(); first += 5) {
                ConfirmDialogSpec confirm;
                confirm.title = l10n::Get(l10n::StringId::PickerCreateFolders);
                confirm.message = l10n::Get(l10n::StringId::PickerWillCreateFolders);
                if (const auto at = confirm.message.find(L"{paths}"); at != std::wstring::npos)
                    confirm.message.erase(at, 7);
                confirm.note = l10n::Get(l10n::StringId::PickerCreatedFoldersRemain);
                confirm.confirm_text = l10n::Get(l10n::StringId::PickerCreateFolders);
                confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
                confirm.default_choice = ConfirmChoice::Cancel;
                confirm.cancel_event = spec_.cancel_event;
                const size_t end = std::min(first + 5, reply->plan.missing.size());
                confirm.items.assign(reply->plan.missing.begin() + first, reply->plan.missing.begin() + end);
                if (!ShowConfirmDialog(hwnd_, confirm, dark_, accent_) || done_ || generation_ != prompt_generation ||
                    (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                    (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
                    if (!done_ && generation_ == prompt_generation) {
                        validating_ = false; UpdateChosen(); Invalidate();
                    }
                    return;
                }
            }
            if (loader_) loader_->PrepareSaveDirectories(++generation_, std::move(reply->request), spec_.native_options, std::move(reply->plan));
            return;
        }
        // Unsupported plans preserve the original validation/contract behavior.
        // A completed independent create action enters the same FileOk pipeline.
        validating_ = false;
        ValidateNames({reply->request.filename}, false, true);
    }

    void OnValidation(std::unique_ptr<PickerValidation> validation) {
        if (!CheckPaintedEdits()) { TraceFolderSubmit(L"validation_edits_failed"); return; }
        if (!validation || done_ || validation->generation != generation_ || !validating_ ||
            (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
            TraceFolderSubmit(L"validation_discarded", validation ? validation->generation : 0);
            return;
        }
        TraceFolderSubmit(L"validation_result", validation->error);
        validating_ = false;
        if (validation->save_ui_generation && validation->save_ui_generation != embedded_state_.generation) {
            ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return;
        }
        chrome_.notice.clear();
        if (validation->save_access_diagnostic && (validation->save_access_diagnostic->cleanup == SaveAccessCleanup::Unconfirmed ||
            validation->save_access_diagnostic->cleanup_error != ERROR_SUCCESS)) {
            FailPaintedEdit(validation->error ? validation->error : ERROR_FUNCTION_FAILED, "save-access-cleanup"); return;
        }
        if (validation->error == ERROR_SHARING_VIOLATION && (spec_.native_options & FOS_SHAREAWARE) && !validation->paths.empty()) {
            const bool same_save_identity = !validation->save_snapshot ||
                (share_consent_save_identity_ == validation->save_snapshot->TargetIdentity() && share_consent_save_parent_ &&
                    *share_consent_save_parent_ == validation->save_snapshot->ParentIdentity());
            uint32_t response = share_consent_paths_ == validation->paths && same_save_identity &&
                share_consent_targets_.contains(validation->failed_path) ? 1u : 0u;
            if (response != 1 && spec_.on_share_violation) {
                embedded_state_.candidate = validation->paths.front(); embedded_state_.candidates = validation->paths;
                ++embedded_state_.generation;
                const auto callback_generation = embedded_state_.generation;
                if (NotifyEmbedded(PickerDialogEvent::Snapshot) != S_OK) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
                HRESULT hr = E_FAIL;
                ++embedded_callback_depth_;
                try { hr = spec_.on_share_violation(hwnd_, embedded_state_, validation->failed_path, response); } catch (...) {}
                --embedded_callback_depth_;
                if (!embedded_callback_depth_ && !pending_navigation_.empty() && !done_ &&
                    !PostMessageW(hwnd_, kEmbeddedNavigationMessage, 0, 0)) FailPaintedEdit(ERROR_FUNCTION_FAILED);
                if (done_ || validation->generation != generation_ || callback_generation != embedded_state_.generation ||
                    (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                    (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
                    ClearEmbeddedCandidate(); UpdateChosen(); return;
                }
                if (hr != S_OK || response > 2) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
            }
            // Only ACCEPT bypasses this observed sharing conflict. DEFAULT uses
            // the ordinary error surface and REFUSE keeps the dialog open.
            if (response == 1) {
                share_consent_paths_ = validation->paths; share_consent_targets_.insert(validation->failed_path);
                if (validation->save_snapshot) {
                    share_consent_save_identity_ = validation->save_snapshot->TargetIdentity();
                    share_consent_save_parent_ = validation->save_snapshot->ParentIdentity();
                }
                validation->error = ERROR_SUCCESS;
            }
            else if (response == 2) { ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return; }
        }
        if (validation->error != ERROR_SUCCESS) {
            if (spec_.validate_local_save_access_v1) ClearEmbeddedCandidate();
            chrome_.notice = validation->error == ERROR_UNSUPPORTED_TYPE
                ? l10n::Get(l10n::StringId::TheSelectedFileDoesNotMatchTheFileType).c_str()
                : ArchiveError(validation->error, validation->failed_path);
            if (chrome_.notice.empty()) chrome_.notice = l10n::Get(l10n::StringId::PickerOpenFailed);
            if (waiting_ && loader_) loader_->Load(++generation_, current_, mode_, options_);
            UpdateChosen();
            Invalidate();
            return;
        }
        if (!validation->navigate.empty()) {
            if (validation_incoming_files_) {
                chrome_.notice = l10n::Get(l10n::StringId::TheSelectedFileDoesNotMatchTheFileType);
                UpdateChosen(); Invalidate(); return;
            }
            TraceFolderSubmit(L"validation_navigates");
            if (mode_ != PickerMode::Folder && mode_ != PickerMode::Save) SetFilename(L"");
            Navigate(validation->navigate, true);
            FocusList();
            return;
        }
        if (mode_ == PickerMode::Save && validation->save_before) save_before_ = validation->save_before;
        if (validation->paths.empty()) { TraceFolderSubmit(L"validation_empty"); UpdateChosen(); return; }
        if (!legacy_browse_ok_ || std::any_of(validation->paths.begin(),validation->paths.end(),
                [this](const auto& path){return !WithinLegacyRoot(path);})) {
            chrome_.notice = l10n::Get(l10n::StringId::PickerOpenFailed);
            UpdateChosen();Invalidate();return;
        }
        if (validation_navigation_only_) {
            Navigate(PickerParent(validation->paths.front()), true, validation->paths.front());
            FocusList();
            return;
        }
        if (validation_add_selection_ && BasketEnabled()) {
            for (const auto& path : validation->paths) {
                if (std::any_of(basket_.begin(), basket_.end(), [&](const auto& item) { return SamePickerPath(item.path, path); })) continue;
                uint64_t bytes = 0;
                for (const auto& item : validation_selection_items_)
                    if (SamePickerPath(item.path, path)) { bytes = item.size; break; }
                basket_.push_back({path, PickerLeafName(path), bytes});
            }
            filename_from_selection_ = true;
            RestoreBasketSelection();
            SyncSelectedNames(); UpdateChosen(); SyncVm(); Relayout(); Invalidate();
            return;
        }
        if (mode_ == PickerMode::Save && !validation->save_snapshot) {
            chrome_.notice = ErrorText(ERROR_INVALID_DATA, validation->paths.front());
            UpdateChosen();
            Invalidate();
            return;
        }
        if ((spec_.native_options & FOS_STRICTFILETYPES) && mode_ == PickerMode::Save) {
            auto strict = options_; strict.advisory_file_filters = false;
            if (std::any_of(validation->paths.begin(), validation->paths.end(), [&](const auto& path) {
                return !PickerMatchesFilter(PickerLeafName(path), mode_, strict);
            })) {
                ClearEmbeddedCandidate();
                chrome_.notice = l10n::Get(l10n::StringId::TheSelectedFileDoesNotMatchTheFileType);
                UpdateChosen(); Invalidate(); return;
            }
        }
        const bool missing_target = !validation->missing_paths.empty() ||
            (validation->save_snapshot && !validation->save_snapshot->TargetExists());
        if ((spec_.native_options & FOS_CREATEPROMPT) && missing_target && create_consent_paths_ != validation->paths) {
            ConfirmDialogSpec confirm;
            confirm.title = chrome_.title;
            confirm.message = l10n::Get(l10n::StringId::PickerCreateMissingFile);
            confirm.items = validation->missing_paths.empty() ? validation->paths : validation->missing_paths;
            confirm.confirm_text = l10n::Get(l10n::StringId::Create);
            confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
            confirm.default_choice = ConfirmChoice::Cancel;
            confirm.cancel_event = spec_.cancel_event;
            const auto prompt_generation = generation_;
            if (!ShowConfirmDialog(hwnd_, confirm, dark_, accent_) || done_ || generation_ != prompt_generation ||
                (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
                ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return;
            }
            create_consent_paths_ = validation->paths;
        }
        if (validation->overwrite_required && spec_.prompt_overwrite) {
            if (!loader_ || !validation->save_snapshot) return;
            // Keep consent tied to this generation; edits and navigation invalidate it.
            validating_ = true;
            UpdateChosen();
            uint32_t overwrite_response = 0;
            if (spec_.on_overwrite) {
                embedded_state_.candidate = validation->paths.front(); embedded_state_.candidates=validation->paths; ++embedded_state_.generation;
                const uint64_t overwrite_generation = embedded_state_.generation;
                if (NotifyEmbedded(PickerDialogEvent::Snapshot) != S_OK) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
                HRESULT hr = E_FAIL;
                ++embedded_callback_depth_;
                try { hr = spec_.on_overwrite(hwnd_, embedded_state_, validation->paths.front(), overwrite_response); } catch (...) {}
                --embedded_callback_depth_;
                if (!embedded_callback_depth_ && !pending_navigation_.empty() && !done_ &&
                    !PostMessageW(hwnd_, kEmbeddedNavigationMessage, 0, 0)) FailPaintedEdit(ERROR_FUNCTION_FAILED);
                if (done_ || generation_ != validation->generation || embedded_state_.generation != overwrite_generation ||
                    (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                    (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
                    validating_ = false; ClearEmbeddedCandidate(); UpdateChosen(); return;
                }
                if (hr != S_OK || overwrite_response > 2) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
                if (overwrite_response == 2) { validating_ = false; ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return; }
            }
            if (overwrite_response != 1) {
                pending_overwrite_ = std::move(validation->save_snapshot);
                overwrite_generation_ = generation_;
                chrome_.overwrite_pending = true;
                chrome_.notice = l10n::Get(l10n::StringId::TheTargetNameAlreadyExists) + L" " + PickerLeafName(pending_overwrite_->Path());
                for (const auto& entry : entries_) if (SamePickerPath(entry.path, pending_overwrite_->Path())) {
                    if (entry.modified.dwLowDateTime || entry.modified.dwHighDateTime)
                        chrome_.notice += L" · " + format::LocalFileTime(entry.modified);
                    chrome_.notice += L" · " + format::ByteSize(entry.size);
                    break;
                }
                SetPropW(hwnd_, L"Pulse.PickerOverwriteGeneration", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(generation_ + 1)));
                focus_ = kPickOverwriteBack;
                SetFocus(hwnd_);
                UpdateChosen(); Relayout(); Invalidate();
                return;
            }
            loader_->RevalidateSave(++generation_, std::move(*validation->save_snapshot));
            UpdateChosen();
            Invalidate();
            return;
        }
        if (spec_.validate_local_save_access_v1 && !validation->save_access_checked) {
            if (!loader_ || !validation->save_snapshot) { FailPaintedEdit(ERROR_INVALID_DATA); return; }
            validating_ = true; UpdateChosen();
            loader_->CheckSaveAccess(++generation_, std::move(*validation->save_snapshot), false, 0, spec_.require_unencrypted_save, spec_.skip_access_validation, spec_.native_options);
            Invalidate(); return;
        }
        if (!FlushPaintedEdits()) { TraceFolderSubmit(L"candidate_edits_failed"); return; }
        if (spec_.on_dialog_event && !validation->save_file_ok_checked) {
            PublishEmbeddedState();
            if (done_) return;
            embedded_state_.candidate = validation->paths.front(); embedded_state_.candidates=validation->paths;
            ++embedded_state_.generation;
            const auto candidate_generation = embedded_state_.generation;
            TraceFolderSubmit(L"file_ok_enter", validation->paths.size());
            const HRESULT accepted = NotifyEmbedded(PickerDialogEvent::FileOk);
            TraceFolderSubmit(L"file_ok_return", static_cast<uint32_t>(accepted));
            if (!FlushPaintedEdits()) { TraceFolderSubmit(L"file_ok_edits_failed"); return; }
            if (done_ || !legacy_browse_ok_ || candidate_generation != embedded_state_.generation ||
                validation->generation != generation_ ||
                (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
                TraceFolderSubmit(L"file_ok_invalidated", validation->generation);
                ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return;
            }
            if (accepted == S_FALSE) { ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return; }
            if (accepted != S_OK) { ClearEmbeddedCandidate(); FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
        }
        if (spec_.on_file_ok && !validation->save_file_ok_checked) {
            HRESULT accepted = E_FAIL;
            try {
                const FilePickerResult candidate{validation->paths, options_.filter_index};
                accepted = spec_.on_file_ok(hwnd_, candidate);
            } catch (...) {}
            if (!FlushPaintedEdits()) return;
            if ((spec_.owner_guard && !spec_.owner_guard->Alive()) || done_ || validation->generation != generation_ ||
                (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) return;
            if (accepted == S_FALSE) { UpdateChosen(); Invalidate(); return; }
            if (accepted != S_OK) {
                native_error_ = ERROR_FUNCTION_FAILED; NoteFailure(native_error_, "file-ok-rejected"); done_ = true;
                PostMessageW(hwnd_, WM_NULL, 0, 0); return;
            }
        }
        if (spec_.validate_local_save_access_v1 && !validation->save_file_ok_checked) {
            if (!loader_ || !validation->save_snapshot) { FailPaintedEdit(ERROR_INVALID_DATA); return; }
            // Preserve the authorized FileOk snapshot while the final worker
            // check runs. A real UI mutation still advances its generation and
            // invalidates this completion; success does not invent another FileOk.
            RememberSaveFocus();
            validating_ = true;
            if (filename_edit_) EnableWindow(filename_edit_, FALSE);
            loader_->CheckSaveAccess(++generation_, std::move(*validation->save_snapshot), true, embedded_state_.generation, spec_.require_unencrypted_save, spec_.skip_access_validation, spec_.native_options);
            Invalidate(); return;
        }
        if (spec_.owner_guard && !spec_.owner_guard->Alive()) { result_.clear(); done_ = true; return; }
        result_ = std::move(validation->paths);
        archive_delivery_ = std::move(validation->archive_delivery);
        archive_source_ = std::move(validation->archive_source);
        g_last_folder[mode_index_] = !archive_source_.empty() ? PickerParent(archive_source_)
            : current_.empty() ? PickerParent(result_.front()) : current_;
        g_last_view[mode_index_] = view_mode_;
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void SyncDetailsPreview() {
        const auto bounds = renderer_.DetailsPreviewBounds();
        const auto& details = vm_.details;
        if (InArchive() || !vm_.details_visible || details.picker_save_mode || !details.has_selection || waiting_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_) ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top || picker_shell::IsParsingName(details.path)) {
            details_preview_.Close(); return;
        }
        if (!details_preview_.InitializeEmbedded(hwnd_, kDetailsPreviewNavigateMessage, kDetailsPreviewOpenMessage, 0)) return;
        QuickPreviewItem item;
        item.path = details.path; item.name = details.name; item.attrs = details.attrs;
        item.modified = details.modified_value; item.size = details.size_value;
        item.position = selected_ + 1; item.count = static_cast<int>(entries_.size());
        RECT rect{static_cast<LONG>(bounds.left), static_cast<LONG>(bounds.top),
            static_cast<LONG>(bounds.right), static_cast<LONG>(bounds.bottom)};
        details_preview_.SetAccent(accent_);
        const auto& shown = details_preview_.item();
        if (!details_preview_.visible() || shown.path != item.path || shown.modified != item.modified || shown.size != item.size)
            details_preview_.ShowEmbedded(item, rect, dark_, false);
        else details_preview_.SetEmbeddedBounds(rect);
    }

    bool CloseCustomDetails() {
        if (custom_details_open_ && !chrome_.overwrite_pending) {
            custom_details_open_ = false; focus_ = kPickList;
            Relayout(); FocusList(); Invalidate(); return true;
        }
        return false;
    }
    void Cancel() {
        if (operating_) return;
        result_.clear();
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void CaptureLayoutForSave() noexcept {
        if (!persist_layout_ || !layout_shown_ || layout_saved_ || layout_captured_ || !hwnd_ || !IsWindow(hwnd_)) return;
        try {
            layout_settings_.view_mode=static_cast<int>(view_mode_);
            layout_settings_.icon_dip=icon_dip_;
            layout_settings_.sort=static_cast<int>(options_.sort);
            layout_settings_.descending=options_.descending;
            layout_settings_.sidebar_width=renderer_.SidebarWidthDip();
            layout_settings_.collapsed_mask=0;
            for(unsigned i=0;i<std::size(collapsed_);++i) if(collapsed_[i]) layout_settings_.collapsed_mask|=1u<<i;
            picker_layout::CapturePlacement(hwnd_,layout_settings_);
            layout_captured_ = true;
        } catch (...) { }
    }

    void PersistCapturedLayout() noexcept {
        if (!persist_layout_ || !layout_shown_ || layout_saved_ || !layout_captured_) return;
        layout_saved_ = true;
        SaveUsedFormat();
        if (opened_at_) { try { picker_layout::SaveLastOpened(static_cast<int>(mode_), opened_at_); } catch (...) { } }
        if (picker_flags_ != saved_flags_) { try { picker_layout::SaveFlags(static_cast<int>(mode_), picker_flags_); } catch (...) { } }
        try {
            const bool saved=picker_layout::Save(static_cast<int>(mode_),layout_settings_);
            diagnostics::runtime::Event("picker_layout_save",{{"success",saved?1u:0u},{"mode",static_cast<uint32_t>(mode_)}});
        } catch (...) { }
    }

    void SaveLayout() noexcept {
        CaptureLayoutForSave();
        PersistCapturedLayout();
    }

    void Close() noexcept {
        CommitCommonForget();
        done_ = true;
        image_dimensions_.Reset();
        frame_pump_.Disarm();
        const HWND hwnd = hwnd_;
        if (hwnd && IsWindow(hwnd)) {
            CaptureLayoutForSave();
            if (GetCapture() == hwnd) ReleaseCapture();
            if (spec_.owner_guard && spec_.owner_guard->Window()) spec_.owner_guard->RestoreBeforeDialogHide(hwnd);
            else if (owner_) pulse::RestoreExternalPickerOwnerBeforeHide(hwnd, owner_);
            if (IsWindow(hwnd)) {
                PickerProofPhase("close-hide-before");
                HideComposedDialog(hwnd); // Identity-bound guard restores modality before the popup disappears.
                PickerProofPhase("close-hide-after");
            }
        }
        navigation_root_.Close();
        details_preview_.Close();
        EndRename(false);
        CancelBlankClick();
        DismissHover();
        ole_drop_.Revoke();
        queued_drop_.reset();
        queued_use_files_.clear();
        operation_session_.Cancel();
        CancelPlacesDrag();
        EndMarquee(false);
        if (!hwnd_) return;
        PersistCapturedLayout();
        RemoveClipboardFormatListener(hwnd_);
        KillTimer(hwnd_, kLoadTimer);
        KillTimer(hwnd_, kSearchTimer);
        KillTimer(hwnd_, kMotionTimer);
        KillTimer(hwnd_, kSharedPlacesTimer);
        KillTimer(hwnd_, kJumpContextTimer);
        KillTimer(hwnd_, kSaveDraftTimer);
        KillTimer(hwnd_, kImageDimensionsTimer);
        // Stop the loader first: after this no listing can be posted, so the
        // ones already queued are all that must be freed.
        PickerProofPhase("close-loader-before");
        save_draft_probe_.Cancel();
        loader_.reset();
        ClearTree();
        PickerProofPhase("close-loader-after");
        MSG pending{};
        while (PeekMessageW(&pending, hwnd_, kTreeListingMessage, kTreeListingMessage, PM_REMOVE))
            PickerLoader::Take(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kListingMessage, kListingMessage, PM_REMOVE))
            PickerLoader::Take(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kFolderCommitMessage, kFolderCommitMessage, PM_REMOVE))
            PickerLoader::TakeFolderCommit(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kSaveDirectoriesMessage, kSaveDirectoriesMessage, PM_REMOVE))
            PickerLoader::TakeSaveDirectories(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kValidationMessage, kValidationMessage, PM_REMOVE))
            PickerLoader::TakeValidation(pending.lParam);
        if (IsWindow(hwnd_)) {
            PickerProofPhase("close-destroy-before");
            DestroyWindow(hwnd_);
            PickerProofPhase("close-destroy-after");
        }
        hwnd_ = nullptr;
    }

    std::wstring DisplayPath(const std::wstring& path) const {
        return path.empty() ? l10n::Get(l10n::StringId::ThisPc) : path::FriendlyPathText(path);
    }

    // ---- view model --------------------------------------------------------

    float Width() const { return static_cast<float>(compositor_.Width()); }
    float Height() const { return static_cast<float>((std::max)(0, static_cast<int>(compositor_.Height()) - legacy_child_.Height(hwnd_))); }
    D2D1_RECT_F ClientRectF() const { return D2D1::RectF(0, 0, Width(), Height()); }
    bool PropertySidebar() const {
        if (!custom_details_open_ && !PickerPropertiesNeedPanel(spec_.custom_controls)) return false;
        return Width() / std::max(0.1f, scale_) >= 900 && std::any_of(spec_.custom_controls.begin(), spec_.custom_controls.end(), [](const auto& control) { return control.property_layout != 0; });
    }
    float RendererWidth() const { return Width() - (PropertySidebar() ? 272 * scale_ : 0); }
    D2D1_RECT_F RendererRect() const { return D2D1::RectF(0, 0, RendererWidth(), Height()); }
    D2D1_RECT_F PaneBounds() const { return renderer_.ContentRect(RendererWidth(), Height()); }

    SidebarItem PlaceItem(const PickerPlace& place) const {
        SidebarItem item;
        item.label = place.label;
        item.path = place.path;
        item.icon_glyph = place.glyph.empty() ? L"\xE8B7" : place.glyph;
        return item;
    }

    void RefreshJumpContext() {
        if (spec_.poll_jump_context) {
            spec_.filter_index = options_.filter_index;
            const auto original = spec_.initial_path;
            bool changed = false;
            try { changed = spec_.poll_jump_context(spec_); } catch (...) { }
            if (changed) {
                diagnostics::runtime::Event("picker_jump_context_updated", {
                    {"window", reinterpret_cast<uintptr_t>(hwnd_)}, {"generation", generation_},
                    {"has_name", spec_.host_app_name.empty() ? 0u : 1u},
                    {"has_icon", spec_.host_app_icon_pixels ? 1u : 0u}});
                app_icon_bitmap_.reset();
                if (!SamePickerPath(original, spec_.initial_path)) {
                    if (!jump_context_touched_ && generation_ == initial_jump_generation_ && spec_.allow_auto_jump)
                        Navigate(spec_.initial_path, true);
                    else { spec_.initial_path = original; spec_.auto_jump_from.clear(); }
                }
                SyncJumpStrip();
                if (jump_.open) UpdateJump();
                Relayout();
            }
        }
    }
    void RefreshSharedPlaces() {
        auto latest = spec_.read_shared_places ? spec_.read_shared_places() : picker_places::Read();
        if (latest.shared_available || spec_.read_shared_places) pulse_places_ = std::move(latest);
        else {
            pulse_places_.shared_available = false;
            pulse_places_.endpoint = nullptr;
            if (saved_places_.valid() && saved_places_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                try {
                    auto saved = saved_places_.get();
                    pulse_places_.workspaces = std::move(saved.workspaces);
                    pulse_places_.networks = std::move(saved.networks);
                    pulse_places_.starred_items = std::move(saved.starred_items);
                    pulse_places_.starred_groups = std::move(saved.starred_groups);
                } catch (...) { }
            }
        }
        Invalidate();
    }

    void CancelPlacesDrag() {
        const bool captured = places_drag_.pending || places_drag_.active;
        places_drag_ = {};
        tray_drop_armed_ = false;
        shake_detector_.Reset();
        vm_.drag_badge.clear();
        SetDragVisualFeedback({});
        vm_.sidebar_drop_index = -1;
        if (captured && hwnd_ && GetCapture() == hwnd_) ReleaseCapture();
    }

    bool IncomingFilesReady() const {
        return !done_ && !waiting_ && !validating_ && !operating_ && !queued_drop_ && queued_use_files_.empty();
    }

    bool DropOnListBlank(POINT point) {
        const auto hit = HitAt(static_cast<float>(point.x), static_cast<float>(point.y));
        return hit.chrome == kPickNone && (hit.main.region == HitTestResult::Pane || hit.main.region == HitTestResult::None) &&
            ContainsRect(renderer_.PaneListRect(vm_.pane, PaneBounds()), static_cast<float>(point.x), static_cast<float>(point.y));
    }

    bool CanUseIncoming(const std::vector<std::wstring>& paths) const {
        return IncomingFilesReady() && PickerCanUseIncomingFiles(paths, mode_, options_, allow_multiselect_) &&
            std::all_of(paths.begin(), paths.end(), [this](const auto& path) { return WithinLegacyRoot(path); });
    }

    void SyncPulseSelection() {
        const auto previous = std::move(chrome_.pulse_selection);
        chrome_.pulse_selection.clear();
        const auto cancel_stale_press = [&] {
            const bool changed = previous.size() != chrome_.pulse_selection.size() ||
                !std::equal(previous.begin(), previous.end(), chrome_.pulse_selection.begin(),
                    [](const auto& a, const auto& b) { return a.path == b.path && a.enabled == b.enabled; });
            if (!changed) return;
            if (chrome_.pressed == kPickPulseUse || chrome_.pressed == kPickPulseMore ||
                (chrome_.pressed >= kPickPulseFileBase && chrome_.pressed < kPickPulseFileBase + 64)) chrome_.pressed = kPickNone;
            if (focus_ >= kPickPulseFileBase && focus_ < kPickPulseFileBase + 64) focus_ = kPickList;
        };
        if (!spec_.pulse_selection_available || (mode_ != PickerMode::File && mode_ != PickerMode::Image)) { chrome_.pulse_folder.clear(); cancel_stale_press(); return; }
        bool compatible = false;
        std::wstring common_parent;
        bool same_parent = true;
        for (const auto& item : spec_.pulse_selected_items) {
            const bool enabled = !item.is_directory && WithinLegacyRoot(item.path) &&
                PickerCanUseIncomingFiles({item.path}, mode_, options_, false);
            compatible |= enabled;
            const bool in_basket = std::any_of(basket_.begin(), basket_.end(),
                [&](const BasketItem& b) { return SamePickerPath(b.path, item.path); });
            chrome_.pulse_selection.push_back({PickerLeafName(item.path), item.path, enabled, in_basket});
            if (same_parent) {
                const std::wstring parent = PickerParent(item.path);
                if (parent.empty()) same_parent = false;
                else if (common_parent.empty()) common_parent = parent;
                else if (!SamePickerPath(common_parent, parent)) same_parent = false;
            }
        }
        if (!compatible) chrome_.pulse_selection.clear();
        cancel_stale_press();
        chrome_.pulse_logo = renderer_.PulseLogoBitmap();
        chrome_.pulse_caption = l10n::Get(l10n::StringId::PickerPulseSelection);
        chrome_.pulse_folder = !chrome_.pulse_selection.empty() && same_parent && !common_parent.empty()
            ? PickerLeafName(common_parent) : std::wstring();
        chrome_.pulse_use_text = l10n::Get(BasketEnabled() ? l10n::StringId::PickerAddAllSelection : l10n::StringId::PickerUseSelection);
        chrome_.pulse_use_primary = !BasketEnabled();
        chrome_.pulse_selection_enabled = IncomingFilesReady();
    }

    void UsePulseSelection(int index = -1) {
        if (!IncomingFilesReady()) return;
        const std::wstring requested = index >= 0 && static_cast<size_t>(index) < chrome_.pulse_selection.size()
            ? chrome_.pulse_selection[index].path : std::wstring();
        // Refresh at activation, not just paint, so a removed/changed main-window
        // selection cannot be submitted from an old displayed chip.
        if (spec_.poll_jump_context) RefreshJumpContext();
        SyncPulseSelection();
        std::vector<std::wstring> paths;
        if (index >= 0) {
            const auto found = std::find_if(chrome_.pulse_selection.begin(), chrome_.pulse_selection.end(),
                [&](const auto& item) { return item.enabled && SamePickerPath(item.path, requested); });
            if (found == chrome_.pulse_selection.end()) return;
            paths.push_back(found->path);
        } else for (const auto& item : chrome_.pulse_selection) {
            if (!item.enabled) continue;
            paths.push_back(item.path);
            if (!BasketEnabled()) break;
        }
        if (!CanUseIncoming(paths)) return;
        user_interacted_ = true;
        ValidateNames(std::move(paths), false, false, true, BasketEnabled());
    }

    void ShowPulseSelectionMenu() {
        if (!IncomingFilesReady()) return;
        SyncPulseSelection();
        const auto snapshot = chrome_.pulse_selection;
        std::vector<FluentMenuItem> items;
        for (size_t i = 0; i < snapshot.size(); ++i) {
            FluentMenuItem item; item.command = static_cast<int>(i) + 1;
            item.text = snapshot[i].text; item.detail = snapshot[i].path; item.enabled = snapshot[i].enabled;
            items.push_back(std::move(item));
        }
        menu_.SetTheme(dark_, accent_);
        const int chosen = TrackPickerDropdown(ScreenRect(chrome_layout_.pulse_more), std::move(items));
        if (chosen <= 0 || static_cast<size_t>(chosen) > snapshot.size()) return;
        // A live source update during the menu must not retarget the same index.
        SyncPulseSelection();
        for (size_t i = 0; i < chrome_.pulse_selection.size(); ++i)
            if (SamePickerPath(chrome_.pulse_selection[i].path, snapshot[chosen - 1].path)) { UsePulseSelection(static_cast<int>(i)); break; }
    }

    PickerPlacesDrop ResolveFilesystemDrop(const std::vector<std::wstring>& sources, POINT point,
                                          DWORD keys, DWORD allowed, DWORD preferred = 0, bool explicit_transfer = false) {
        if (InArchive() || !IncomingFilesReady() || sources.empty() ||
            std::any_of(sources.begin(), sources.end(), [](const auto& source) {
                return !IsPickerDiskPath(source);
            })) return {};
        const auto hit = HitAt(static_cast<float>(point.x), static_cast<float>(point.y));
        PickerPlacesDrop target;
        if (hit.main.region == HitTestResult::Row && hit.main.index >= 0 &&
            hit.main.index < static_cast<int>(entries_.size())) {
            const auto& entry = entries_[hit.main.index];
            if (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive)
                target.destination = entry.path;
        } else if ((explicit_transfer || (mode_ != PickerMode::File && mode_ != PickerMode::Image)) && hit.chrome == kPickNone &&
                   (hit.main.region == HitTestResult::Pane || hit.main.region == HitTestResult::None) &&
                   ContainsRect(renderer_.PaneListRect(vm_.pane, PaneBounds()),
                                static_cast<float>(point.x), static_cast<float>(point.y)) && current_verified_) {
            target.destination = current_;
        } else {
            std::vector<picker_places::StarredItem> items;
            for (const auto& source : sources) items.push_back({source, 1, {}});
            target = ResolvePickerPlacesDrop(vm_, pulse_places_, hit.main, items, keys);
            // External sources have no trusted folder kind; never guess that
            // metadata or perform filesystem reads on OLE's DragOver thread.
            if (target.favorite) return {};
        }
        if (target.destination.empty() || !IsPickerDiskPath(target.destination) ||
            DropIntoOwnFolder(sources, target.destination)) return {};
        // A folder row is the explicit copy/move destination. Copy unless Shift
        // requests a move, independent of whether source and target share a drive.
        const DWORD transfer_keys = (keys & MK_SHIFT) ? keys : keys | MK_CONTROL;
        if (!(allowed & ((keys & MK_SHIFT) ? DROPEFFECT_MOVE : DROPEFFECT_COPY))) return {};
        target.effect = ComputeDropEffect(transfer_keys, sources.front(), target.destination,
                                         allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE), preferred);
        if (target.effect == DROPEFFECT_MOVE && std::all_of(sources.begin(), sources.end(),
            [&](const auto& source) { return SamePickerPath(PickerParent(source), target.destination); })) return {};
        return target;
    }

    void ShowFilesystemDrop(const PickerPlacesDrop& target, POINT point) {
        if (InArchive()) return;
        const auto hit = HitAt(static_cast<float>(point.x), static_cast<float>(point.y));
        vm_.sidebar_drop_index = target.effect != DROPEFFECT_NONE && hit.main.region == HitTestResult::SidebarItem
            ? hit.main.index : -1;
        vm_.drag_badge = target.effect == DROPEFFECT_NONE ? std::wstring() :
            std::wstring(l10n::Get(target.effect == DROPEFFECT_MOVE ? l10n::StringId::DropMove : l10n::StringId::DropCopy)) +
            PickerLeafName(target.destination);
        vm_.drag_badge_x = static_cast<float>(point.x);
        vm_.drag_badge_y = static_cast<float>(point.y);
        vm_.drag_badge_move = target.effect == DROPEFFECT_MOVE;
        SetDragVisualFeedback(vm_.drag_badge);
        Invalidate();
    }

    void RegisterPickerDrop() {
        DropTargetCallbacks callbacks;
        callbacks.drag_over = [this](const auto& sources, POINT point, DWORD keys, DWORD allowed, DWORD preferred) {
            if (keys & MK_RBUTTON) right_file_drag_ = true;
            else if (keys & MK_LBUTTON) right_file_drag_ = false;
            if (!InArchive() && (mode_ == PickerMode::File || mode_ == PickerMode::Image) &&
                shake_detector_.Feed(point.x, scale_, GetTickCount64())) {
                tray_drop_armed_ = true;
                tray_collapsed_ = false;
                SyncVm();
            }
            if (DropOnListBlank(point) && CanUseIncoming(sources) && PickerUseFilesEffect(allowed)) {
                incoming_dropzone_sources_ = sources;
                const DWORD effect = PickerUseFilesEffect(allowed);
                vm_.sidebar_drop_index = -1;
                vm_.drag_badge = l10n::Get(l10n::StringId::PickerUseIncomingFiles);
                vm_.drag_badge_x = static_cast<float>(point.x); vm_.drag_badge_y = static_cast<float>(point.y);
                vm_.drag_badge_move = false;
                SetDragVisualFeedback(vm_.drag_badge); Invalidate();
                return effect;
            }
            incoming_dropzone_sources_.clear();
            const auto hit = HitAt(static_cast<float>(point.x), static_cast<float>(point.y));
            std::vector<picker_places::StarredItem> tray_items;
            for (const auto& s : sources) tray_items.push_back({s, 1, {}});
            const auto place_target = ResolvePickerPlacesDrop(vm_, pulse_places_, hit.main, tray_items, keys);
            if (place_target.tray) {
                const DWORD effect = place_target.effect != DROPEFFECT_NONE
                    ? ((allowed & DROPEFFECT_LINK) ? DROPEFFECT_LINK : (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE)
                    : DROPEFFECT_NONE;
                vm_.sidebar_drop_index = effect != DROPEFFECT_NONE && hit.main.region == HitTestResult::SidebarItem ? hit.main.index : -1;
                vm_.drag_badge = effect != DROPEFFECT_NONE ? l10n::Get(l10n::StringId::PickerTrayDropBadge) : std::wstring();
                vm_.drag_badge_x = static_cast<float>(point.x); vm_.drag_badge_y = static_cast<float>(point.y);
                vm_.drag_badge_move = false;
                SetDragVisualFeedback(vm_.drag_badge); Invalidate();
                return effect;
            }
            const auto target = ResolveFilesystemDrop(sources, point, keys, allowed, preferred, right_file_drag_);
            ShowFilesystemDrop(target, point);
            return target.effect;
        };
        callbacks.drag_leave = [this] {
            right_file_drag_ = false;
            incoming_dropzone_sources_.clear();
            tray_drop_armed_ = false;
            shake_detector_.Reset();
            vm_.drag_badge.clear(); SetDragVisualFeedback({}); vm_.sidebar_drop_index = -1; Invalidate();
        };
        callbacks.drop = [this](const auto& sources, POINT point, DWORD keys, DWORD allowed, DWORD preferred) {
            const bool right = std::exchange(right_file_drag_, false) || (keys & MK_RBUTTON);
            const bool open = mode_ == PickerMode::File || mode_ == PickerMode::Image;
            incoming_dropzone_sources_.clear();
            const auto hit = HitAt(static_cast<float>(point.x), static_cast<float>(point.y));
            std::vector<picker_places::StarredItem> tray_items;
            for (const auto& s : sources) tray_items.push_back({s, 1, {}});
            const auto place_target = ResolvePickerPlacesDrop(vm_, pulse_places_, hit.main, tray_items, keys);
            tray_drop_armed_ = false;
            shake_detector_.Reset();
            auto target = ResolveFilesystemDrop(sources, point, keys, allowed, preferred, right);
            bool use = open && DropOnListBlank(point) && CanUseIncoming(sources) && PickerUseFilesEffect(allowed);
            vm_.drag_badge.clear(); vm_.sidebar_drop_index = -1; Invalidate();
            SetDragVisualFeedback({});
            if (!right && place_target.tray) {
                if (place_target.effect == DROPEFFECT_NONE) {
                    chrome_.notice = l10n::Get(l10n::StringId::PickerTrayStageUnavailable);
                    Invalidate();
                    return DWORD{DROPEFFECT_NONE};
                }
                const bool okay = StagePathsToTray(sources);
                if (!okay) chrome_.notice = l10n::Get(l10n::StringId::PickerTrayStageUnavailable);
                Invalidate();
                return okay ? ((allowed & DROPEFFECT_LINK) ? DWORD{DROPEFFECT_LINK} : DWORD{DROPEFFECT_COPY}) : DWORD{DROPEFFECT_NONE};
            }
            if (right && open && (use || target.effect != DROPEFFECT_NONE)) {
                const auto generation = generation_;
                std::vector<FluentMenuItem> items;
                FluentMenuItem item; item.command = 1; item.text = l10n::Get(l10n::StringId::PickerUseIncomingFiles);
                item.enabled = CanUseIncoming(sources) && PickerUseFilesEffect(allowed); items.push_back(item);
                const auto copy = ResolveFilesystemDrop(sources, point, MK_CONTROL, allowed, preferred, true);
                const auto move = ResolveFilesystemDrop(sources, point, MK_SHIFT, allowed, preferred, true);
                item.command = 2; item.text = l10n::Get(l10n::StringId::DropCopy) + PickerLeafName(copy.destination);
                item.enabled = copy.effect == DROPEFFECT_COPY; items.push_back(item);
                item.command = 3; item.text = l10n::Get(l10n::StringId::DropMove) + PickerLeafName(move.destination);
                item.enabled = move.effect == DROPEFFECT_MOVE; items.push_back(item);
                item.command = 4; item.text = l10n::Get(l10n::StringId::Cancel); item.enabled = true; items.push_back(item);
                menu_.SetTheme(dark_, accent_);
                const int chosen = TrackAt(point, std::move(items));
                if (!IncomingFilesReady() || generation != generation_ || chosen <= 0 || chosen == 4) return DWORD{DROPEFFECT_NONE};
                use = chosen == 1;
                if (chosen == 2) target = copy;
                if (chosen == 3) target = move;
            }
            if (use) {
                if (!CanUseIncoming(sources) || !PickerUseFilesEffect(allowed)) return DWORD{DROPEFFECT_NONE};
                queued_use_files_ = sources;
                if (!PostMessageW(hwnd_, kPickerDropMessage, 0, 0)) { queued_use_files_.clear(); return DWORD{DROPEFFECT_NONE}; }
                return PickerUseFilesEffect(allowed);
            }
            if (target.effect == DROPEFFECT_NONE) return DWORD{DROPEFFECT_NONE};
            ops::OpRequest request;
            request.type = target.effect == DROPEFFECT_MOVE ? ops::OpType::Move : ops::OpType::Copy;
            request.sources = sources;
            request.dest_dir = target.destination;
            queued_drop_ = std::make_unique<ops::OpRequest>(std::move(request));
            if (!PostMessageW(hwnd_, kPickerDropMessage, 0, 0)) {
                queued_drop_.reset();
                return DWORD{DROPEFFECT_NONE};
            }
            return target.effect;
        };
        ole_drop_.Register(hwnd_, std::move(callbacks));
    }

    void BeginPlacesDrag(int row, POINT point) {
        if (InArchive()) {
            focus_ = kPickList;
            EndAddressEdit(false);
            SetFocus(hwnd_);
            Select(row, KeyDown(VK_SHIFT), KeyDown(VK_CONTROL));
            return;
        }
        if (row < 0 || row >= static_cast<int>(entries_.size())) return;
        CancelPlacesDrag();
        focus_ = kPickList;
        EndAddressEdit(false);
        SetFocus(hwnd_);
        auto& drag = places_drag_;
        drag.filename_before = filename_text_;
        drag.filename_from_selection = filename_from_selection_;
        drag.ctrl = KeyDown(VK_CONTROL);
        drag.shift = KeyDown(VK_SHIFT);
        drag.deferred_click = !drag.shift && selected_set_.contains(row);
        if (!drag.deferred_click) Select(row, drag.shift, drag.ctrl);
        else selected_ = row;
        drag.row = row;
        drag.start = point;
        drag.pending = true;
        SetCapture(hwnd_);
        Invalidate();
    }

    void MovePlacesDrag(POINT point, DWORD keys) {
        auto& drag = places_drag_;
        if (!(keys & MK_LBUTTON)) { CancelPlacesDrag(); Invalidate(); return; }
        if (drag.pending && (std::abs(point.x - drag.start.x) >= GetSystemMetrics(SM_CXDRAG) ||
                             std::abs(point.y - drag.start.y) >= GetSystemMetrics(SM_CYDRAG))) {
            drag.pending = false;
            drag.active = true;
            if (mode_ == PickerMode::Save) {
                SetFilename(drag.filename_before);
                filename_from_selection_ = drag.filename_from_selection;
            }
            std::vector<int> indices(selected_set_.begin(), selected_set_.end());
            std::sort(indices.begin(), indices.end());
            for (int index : indices) if (index >= 0 && index < static_cast<int>(entries_.size())) {
                const auto& entry = entries_[index];
                if (entry.kind != PickerEntryKind::Drive)
                    drag.items.push_back({entry.path, entry.kind == PickerEntryKind::Folder ? 2u : 1u, {}});
            }
        }
        if (!drag.active) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        if (!PtInRect(&client, point) && ole_drop_.Ready() && !drag.items.empty()) {
            std::vector<std::wstring> paths;
            for (const auto& item : drag.items) paths.push_back(item.path);
            CancelPlacesDrag();
            Invalidate();
            ole_drag_out_ = true;
            const DWORD effect = DoFileDragDrop(paths, DROPEFFECT_COPY | DROPEFFECT_MOVE, {}, current_, dark_);
            ole_drag_out_ = false;
            if (!done_) {
                if (queued_drop_ || !queued_use_files_.empty()) PostMessageW(hwnd_, kPickerDropMessage, 0, 0);
                else if (effect != DROPEFFECT_NONE) Reload();
            }
            return;
        }
        if ((mode_ == PickerMode::File || mode_ == PickerMode::Image) &&
            shake_detector_.Feed(point.x, scale_, GetTickCount64())) {
            tray_drop_armed_ = true;
            tray_collapsed_ = false;
            SyncVm();
        }
        const auto hit = HitAt(static_cast<float>(point.x), static_cast<float>(point.y));
        drag.target = ResolvePickerPlacesDrop(vm_, pulse_places_, hit.main, drag.items, keys);
        if (drag.target.pin && spec_.host_app_name.empty()) drag.target = {};
        if (!drag.target.favorite && !drag.target.tray && !drag.target.pin) {
            std::vector<std::wstring> sources;
            for (const auto& item : drag.items) sources.push_back(item.path);
            drag.target = ResolveFilesystemDrop(sources, point, keys, DROPEFFECT_COPY | DROPEFFECT_MOVE);
        } else if (done_ || waiting_ || validating_ || operating_ || queued_drop_) {
            drag.target.effect = DROPEFFECT_NONE;
        }
        vm_.sidebar_drop_index = drag.target.effect != DROPEFFECT_NONE && hit.main.region == HitTestResult::SidebarItem ? hit.main.index : -1;
        vm_.drag_badge = drag.target.pin && drag.target.effect != DROPEFFECT_NONE ? l10n::Get(l10n::StringId::PickerPinToApp) :
            drag.target.tray && drag.target.effect != DROPEFFECT_NONE ? l10n::Get(l10n::StringId::PickerTrayDropBadge) :
            drag.target.favorite && drag.target.effect != DROPEFFECT_NONE ? std::wstring(l10n::Get(l10n::StringId::FavoritesBarDropAdd)) :
            drag.target.effect != DROPEFFECT_NONE ? std::wstring(l10n::Get(drag.target.effect == DROPEFFECT_MOVE ?
                l10n::StringId::DropMove : l10n::StringId::DropCopy)) + PickerLeafName(drag.target.destination) : std::wstring();
        vm_.drag_badge_x = static_cast<float>(point.x);
        vm_.drag_badge_y = static_cast<float>(point.y);
        vm_.drag_badge_move = drag.target.effect == DROPEFFECT_MOVE;
        SetDragVisualFeedback(vm_.drag_badge);
        UpdateHover(hit);
        SetCursor(LoadCursorW(nullptr, drag.target.effect == DROPEFFECT_NONE ? IDC_NO : IDC_ARROW));
        Invalidate();
    }

    bool StagePathsToTray(const std::vector<std::wstring>& paths) {
        if (operating_ || done_ || paths.empty()) return false;
        operating_ = true;
        const bool okay = RunPickerStageTrayRequest(hwnd_, pulse_places_, paths);
        operating_ = false;
        if (done_) return false;
        if (okay) {
            if (spec_.poll_jump_context) RefreshJumpContext();
            RefreshSharedPlaces();
            Relayout();
        }
        return okay;
    }

    void FinishPlacesDrag(POINT point, DWORD keys) {
        if (places_drag_.active) MovePlacesDrag(point, keys | MK_LBUTTON);
        auto drag = std::move(places_drag_);
        places_drag_ = {};
        tray_drop_armed_ = false;
        shake_detector_.Reset();
        vm_.drag_badge.clear();
        SetDragVisualFeedback({});
        vm_.sidebar_drop_index = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (drag.pending) {
            if (drag.deferred_click) Select(drag.row, drag.shift, drag.ctrl);
        } else if (drag.active && drag.target.effect != DROPEFFECT_NONE) {
            if (drag.target.pin) {
                for (const auto& item : drag.items)
                    if (item.kind == 2 && !IsPathPinnedToApp(item.path)) TogglePinPathToApp(item.path);
                Relayout();
            } else if (drag.target.tray) {
                std::vector<std::wstring> paths;
                for (const auto& item : drag.items) paths.push_back(item.path);
                const bool okay = StagePathsToTray(paths);
                if (done_) return;
                chrome_.notice = okay ? std::wstring() : l10n::Get(l10n::StringId::PickerTrayStageUnavailable);
            } else if (drag.target.favorite) {
                operating_ = true;
                const bool okay = RunPickerFavoriteRequest(hwnd_, pulse_places_, drag.items, drag.target.group, spec_.add_shared_favorites);
                operating_ = false;
                if (done_) return;
                chrome_.notice = okay ? std::wstring() : l10n::Get(l10n::StringId::PickerFavoritesUnavailable);
                RefreshSharedPlaces();
            } else {
                ops::OpRequest request;
                request.type = drag.target.effect == DROPEFFECT_MOVE ? ops::OpType::Move : ops::OpType::Copy;
                for (const auto& item : drag.items) request.sources.push_back(item.path);
                request.dest_dir = drag.target.destination;
                ExecuteOperation(std::move(request));
            }
        } else if (drag.active && drag.target.tray) {
            chrome_.notice = l10n::Get(l10n::StringId::PickerTrayStageUnavailable);
        } else if (drag.active && drag.target.favorite) {
            chrome_.notice = l10n::Get(l10n::StringId::PickerFavoritesUnavailable);
        }
        Invalidate();
    }

    void OpenSharedPlace(const std::wstring& path) {
        if (path.starts_with(kPickerGroupPrefix)) {
            const auto group = path.substr(std::size(kPickerGroupPrefix) - 1);
            if (!folded_favorite_groups_.erase(group)) folded_favorite_groups_.insert(group);
            Invalidate();
            return;
        }
        const auto item = std::find_if(pulse_places_.starred_items.begin(), pulse_places_.starred_items.end(),
            [&](const auto& entry) { return SamePickerPath(entry.path, path); });
        if (item != pulse_places_.starred_items.end() && item->kind == 1) {
            const auto split = path.find_last_of(L"\\/");
            if (split != path.npos) Navigate(path.substr(0, split + 1), true, path);
        } else if (!path.empty()) Navigate(path, true);
    }

    bool ContinueShellFilterBatch(uint64_t generation, unsigned& checks, ULONGLONG& next_yield) {
        if (++checks >= 16 || GetTickCount64() >= next_yield) {
            checks = 0; next_yield = GetTickCount64() + 8;
            MSG message{};
            for (unsigned count = 0; count < 16 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); Cancel(); break; }
                TranslateMessage(&message); DispatchMessageW(&message);
                if (done_ || generation_ != generation) break;
            }
        }
        return !done_ && generation_ == generation && pending_navigation_.empty() && external_navigation_queue_.empty() &&
            (!spec_.cancel_event || WaitForSingleObject(spec_.cancel_event, 0) != WAIT_OBJECT_0) &&
            (!spec_.owner_guard || spec_.owner_guard->Alive());
    }
    void AbandonShellFilterBatch(uint64_t generation) {
        if (done_) return; // A callback Close keeps its existing terminal HRESULT.
        if ((spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0) ||
            (spec_.owner_guard && !spec_.owner_guard->Alive())) { Cancel(); return; }
        // A normal reentrant navigation invalidates only the old enumeration.
        // Do not clear a newer loader's waiting state or turn navigation into failure.
        if (generation_ == generation) waiting_ = loading_ = false;
        if (!pending_navigation_.empty()) PostMessageW(hwnd_, kEmbeddedNavigationMessage, 0, 0);
        if (!external_navigation_queue_.empty()) PostMessageW(hwnd_, kExternalNavigationMessage, 0, 0);
        Invalidate();
    }
    void RefreshKnownPlaces() {
        places_ = spec_.places;
        for (PickerPlace& place : KnownPlaces()) {
            const bool duplicate = std::any_of(places_.begin(), places_.end(),
                [&](const PickerPlace& p) { return SamePickerPath(p.path, place.path); });
            if (!duplicate) places_.push_back(std::move(place));
        }
        places_.insert(places_.end(), spec_.bottom_places.begin(), spec_.bottom_places.end());
        standard_places_ = StandardPlaces();
        fallback_ = known_folders::CurrentPath(mode_ == PickerMode::Image ? FOLDERID_Pictures : FOLDERID_Desktop);
    }
    std::wstring TrayToken(uint64_t id) const {
        return std::wstring(kPickerTrayPrefix) + std::to_wstring(spec_.pulse_tray_publisher_pid) + L":" +
            std::to_wstring(spec_.pulse_tray_shelf_id) + L":" + std::to_wstring(spec_.pulse_tray_revision) + L":" + std::to_wstring(id);
    }

    bool TrayCompatible(const picker_places::TrayItem& item) const {
        return item.exists && !item.inflight && !item.is_directory && WithinLegacyRoot(item.path) &&
            PickerCanUseIncomingFiles({item.path}, mode_, options_, false);
    }

    bool TrayPathSelected(const std::wstring& path) const {
        for (const auto& item : basket_) if (SamePickerPath(item.path, path)) return true;
        for (const int index : selected_set_)
            if (index >= 0 && static_cast<size_t>(index) < entries_.size() && SamePickerPath(entries_[index].path, path)) return true;
        return false;
    }

    bool TrayConsumeAvailable() const {
        return mode_ != PickerMode::Save && mode_ != PickerMode::Folder &&
            spec_.enable_tray_consume && spec_.pulse_tray_available && spec_.pulse_tray_source &&
            spec_.pulse_tray_endpoint && spec_.pulse_tray_publisher_pid &&
            spec_.pulse_tray_endpoint_pid == spec_.pulse_tray_publisher_pid;
    }

    void RememberTrayPickup(const picker_places::TrayItem& item) {
        if (!TrayConsumeAvailable()) return;
        std::erase_if(tray_pickups_, [&](const auto& picked) { return SamePickerPath(picked.item.path, item.path); });
        picker_places::Snapshot endpoint;
        endpoint.endpoint = spec_.pulse_tray_endpoint; endpoint.endpoint_pid = spec_.pulse_tray_endpoint_pid;
        endpoint.tray_source = spec_.pulse_tray_source; endpoint.tray_publisher_pid = spec_.pulse_tray_publisher_pid;
        endpoint.tray_shelf_id = spec_.pulse_tray_shelf_id;
        endpoint.tray_available = endpoint.shared_available = true;
        tray_pickups_.push_back({std::move(endpoint), item});
    }

    bool HasSelectedTrayItem() const {
        if (!spec_.pulse_tray_available || spec_.pulse_tray_items.empty()) return false;
        for (const auto& item : spec_.pulse_tray_items)
            if (TrayPathSelected(item.path)) return true;
        return false;
    }

    bool HasCompatibleTrayItem() const {
        if (!spec_.pulse_tray_available) return false;
        for (const auto& item : spec_.pulse_tray_items)
            if (TrayCompatible(item)) return true;
        return false;
    }

    void ToggleSaveToTray() {
        if (mode_ != PickerMode::Save || done_ || operating_) return;
        if (saving_to_tray_) {
            saving_to_tray_ = false;
            if (!save_before_tray_dir_.empty() && !SamePickerPath(save_before_tray_dir_, current_)) {
                Navigate(save_before_tray_dir_, true);
            } else {
                Relayout(); Invalidate();
            }
            return;
        }
        save_before_tray_dir_ = current_;
        const std::wstring shelf_dir = picker_places::DefaultPickerShelfDirPath();
        if (shelf_dir.empty()) return;
        operating_ = true;
        const bool ok = RunPickerPlacesRequest(hwnd_, [shelf_dir] {
            std::error_code ec;
            std::filesystem::create_directories(shelf_dir, ec);
            return !ec || std::filesystem::is_directory(shelf_dir, ec);
        });
        operating_ = false;
        if (done_ || !ok) return;
        saving_to_tray_ = true;
        Navigate(shelf_dir, true);
    }

    bool IsPathPinnedToApp(const std::wstring& path) const {
        if (path.empty()) return false;
        return std::any_of(app_pinned_places_.begin(), app_pinned_places_.end(),
            [&](const auto& p) { return SamePickerPath(p, path); });
    }

    void TogglePinPathToApp(const std::wstring& path) {
        if (path.empty() || !IsPickerDiskPath(path)) return;
        const auto it = std::find_if(app_pinned_places_.begin(), app_pinned_places_.end(),
            [&](const auto& p) { return SamePickerPath(p, path); });
        if (it != app_pinned_places_.end()) {
            app_pinned_places_.erase(it);
        } else {
            app_pinned_places_.insert(app_pinned_places_.begin(), path);
            if (app_pinned_places_.size() > picker_layout::kMaxAppPinnedPlaces)
                app_pinned_places_.resize(picker_layout::kMaxAppPinnedPlaces);
        }
        if (persist_layout_) {
            try { picker_layout::SaveAppPinned(app_pinned_places_); } catch (...) { }
        }
        Relayout(); Invalidate();
    }

    void StartGitRootProbe(const std::wstring& dir) {
        if (mode_ != PickerMode::Save) return;
        if (SamePickerPath(dir, git_repo_dir_)) return;
        git_repo_dir_ = dir;
        git_repo_root_.clear();
        const uint64_t gen = ++git_repo_generation_;
        if (dir.empty() || !IsPickerDiskPath(dir) || !hwnd_) return;
        const HWND target = hwnd_;
        try {
            std::thread([target, dir, gen] {
                std::wstring found;
                try {
                    std::filesystem::path cur(dir);
                    for (int depth = 0; depth < 24 && !cur.empty(); ++depth) {
                        std::error_code ec;
                        if (std::filesystem::exists(cur / L".git", ec)) {
                            found = cur.wstring();
                            break;
                        }
                        const auto parent = cur.parent_path();
                        if (parent == cur) break;
                        cur = parent;
                    }
                } catch (...) { }
                auto* payload = new (std::nothrow) std::wstring(std::move(found));
                if (!payload || !PostMessageW(target, kGitRootMessage, static_cast<WPARAM>(gen), reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                }
            }).detach();
        } catch (...) { }
    }

    void AppendPickerTray() {
        tray_display_items_.clear();
        if (mode_ == PickerMode::Save) {
            SidebarGroup group;
            group.id = kGroupPickerTray;
            group.header = l10n::Get(l10n::StringId::PickerTrayTitle);
            const uint64_t total = spec_.pulse_tray_total_count ? spec_.pulse_tray_total_count : spec_.pulse_tray_items.size();
            if (total > 0) group.header += L"  " + std::to_wstring(total);
            group.icon_glyph = L"\xE7B8";
            group.collapsed = tray_collapsed_;
            SidebarItem save_row;
            save_row.path = TrayToken(0) + L":save_to_tray";
            save_row.label = l10n::Get(l10n::StringId::PickerSaveToTray);
            save_row.icon_glyph = L"\xE7B8";
            save_row.accent_label = true;
            save_row.selected_override = saving_to_tray_;
            group.items.push_back(std::move(save_row));
            vm_.sidebar.push_back(std::move(group));
            return;
        }
        if (mode_ != PickerMode::File && mode_ != PickerMode::Image) return;
        const bool has_items = spec_.pulse_tray_available && !spec_.pulse_tray_items.empty();
        const bool has_overflow = !spec_.pulse_tray_available && spec_.pulse_tray_total_count > 0;
        if (!has_items && !has_overflow && !tray_drop_armed_) return;
        SidebarGroup group; group.id = kGroupPickerTray; group.header = l10n::Get(l10n::StringId::PickerTrayTitle);
        if (!spec_.pulse_tray_shelf_name.empty()) group.header += L" · " + spec_.pulse_tray_shelf_name;
        group.icon_glyph = L"\xE7B8"; group.collapsed = tray_collapsed_ && !tray_drop_armed_;
        group.drop_highlight = tray_drop_armed_;
        if (tray_drop_armed_) {
            SidebarItem slot;
            slot.path = TrayToken(0) + L":drop";
            slot.label = l10n::Get(l10n::StringId::PickerTrayDropPrompt);
            slot.icon_glyph = L"\xE7B8";
            slot.drop_slot = true;
            slot.accent_label = true;
            group.items.push_back(std::move(slot));
        }
        if (!spec_.pulse_tray_available) {
            if (has_overflow) {
                SidebarItem row; row.label = l10n::Get(l10n::StringId::PickerTrayOverflow); row.path = TrayToken(0) + L":unavailable";
                row.icon_glyph = L"\xE7BA"; group.items.push_back(std::move(row));
            }
            vm_.sidebar.push_back(std::move(group)); return;
        }
        const auto identity = TrayToken(0);
        if (tray_page_identity_ != identity) { tray_page_identity_ = identity; tray_page_ = 0; }
        std::vector<const picker_places::TrayItem*> ordered;
        for (const auto& item : spec_.pulse_tray_items) if (TrayCompatible(item)) ordered.push_back(&item);
        const size_t compatible = ordered.size();
        for (const auto& item : spec_.pulse_tray_items) if (!TrayCompatible(item)) ordered.push_back(&item);
        constexpr size_t page_size = 8;
        const size_t page_count = std::max<size_t>(1, (ordered.size() + page_size - 1) / page_size);
        tray_page_ = std::min(tray_page_, page_count - 1);
        const size_t first = tray_expanded_ ? tray_page_ * page_size : 0;
        const size_t shown = tray_expanded_ ? std::min(page_size, ordered.size() - first) : std::min<size_t>(5, compatible);
        for (size_t i = first; i < first + shown; ++i) {
            const auto& item = *ordered[i]; const bool enabled = TrayCompatible(item);
            SidebarItem row; row.path = TrayToken(item.id);
            row.label = item.label.empty() ? PickerLeafName(item.path) : item.label;
            row.icon_glyph = item.is_directory ? L"\xE8B7" : L"\xE8A5";
            row.icon_thumbnail_checkable = BasketEnabled() && enabled;
            row.icon_thumbnail_checked = TrayPathSelected(item.path);
            row.icon_thumbnail_check_enabled = enabled;
            row.detail = item.source_label.empty() ? item.domain : item.source_label;
            std::wstring extension(preview::DiagnosticExtension(item.path));
            CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
            if (!item.is_directory && item.exists && preview::IsImageExtension(extension)) {
                row.icon_path = item.path; row.icon_thumbnail = true;
                row.icon_thumbnail_attrs = item.attrs; row.icon_thumbnail_size = item.size;
            }
            if (!enabled) {
                row.badge = l10n::Get(!item.exists ? l10n::StringId::ShelfMissingTag : item.inflight ? l10n::StringId::PickerTrayBusy : l10n::StringId::PickerTrayIncompatible);
                row.icon_color = HexColor(dark_ ? 0x888888 : 0x777777);
                row.badge_color = row.icon_color;
            }
            tray_display_items_.push_back({row.path, item}); group.items.push_back(std::move(row));
        }
        if (!tray_expanded_ && shown < ordered.size()) {
            SidebarItem more; more.path = TrayToken(0) + L":more"; more.icon_glyph = L"\xE70D";
            const size_t hidden_count = ordered.size() - shown;
            if (shown == compatible && !spec_.host_app_name.empty()) {
                std::wstring text = l10n::Get(l10n::StringId::PickerTrayOthersUnsupported);
                if (const auto pos = text.find(L"{count}"); pos != std::wstring::npos) text.replace(pos, 7, std::to_wstring(hidden_count));
                if (const auto pos = text.find(L"{app}"); pos != std::wstring::npos) text.replace(pos, 5, spec_.host_app_name);
                more.label = std::move(text);
            } else {
                more.label = FillPickerText(l10n::Get(l10n::StringId::PickerTrayOthers), std::to_wstring(hidden_count));
            }
            group.items.push_back(std::move(more));
        }
        if (tray_expanded_ && page_count > 1) {
            const auto add_page = [&](bool previous) {
                SidebarItem row;
                row.path = identity + (previous ? L":prev:" : L":next:") + std::to_wstring(tray_page_);
                row.icon_glyph = previous ? L"\xE70E" : L"\xE70D";
                row.label = FillPickerText(l10n::Get(previous ? l10n::StringId::PickerTrayPreviousPage : l10n::StringId::PickerTrayNextPage),
                    std::to_wstring(tray_page_ + 1) + L" / " + std::to_wstring(page_count));
                group.items.push_back(std::move(row));
            };
            if (tray_page_ > 0) add_page(true);
            if (tray_page_ + 1 < page_count) add_page(false);
        }
        if (tray_expanded_) {
            SidebarItem collapse;
            collapse.path = TrayToken(0) + L":collapse";
            collapse.icon_glyph = L"\xE70E";
            collapse.label = l10n::Get(l10n::StringId::PickerTrayCollapse);
            group.items.push_back(std::move(collapse));
        }
        if (BasketEnabled() && compatible) {
            SidebarItem all; all.path = TrayToken(0) + L":all"; all.icon_glyph = L"\xE710";
            all.label = l10n::Get(l10n::StringId::PickerAddAllSelection); group.items.push_back(std::move(all));
        }
        vm_.sidebar.push_back(std::move(group));
    }

    void UsePickerTray(const std::wstring& token) {
        if (!token.starts_with(kPickerTrayPrefix)) return;
        if (mode_ == PickerMode::Save && token.ends_with(L":save_to_tray")) {
            ToggleSaveToTray();
            return;
        }
        if (!IncomingFilesReady()) return;
        const auto old_prefix = TrayToken(0);
        if (token == old_prefix + L":drop") return;
        if (token == old_prefix + L":consume" && TrayConsumeAvailable()) {
            picker_flags_ ^= picker_layout::kFlagRemoveTrayAfterOpen;
            Relayout(); Invalidate(); return;
        }
        if (token == old_prefix + L":collapse") {
            tray_expanded_ = false;
            tray_page_ = 0;
            Relayout(); Invalidate(); return;
        }
        const bool more = token == old_prefix + L":more";
        const bool previous = token == old_prefix + L":prev:" + std::to_wstring(tray_page_);
        const bool next = token == old_prefix + L":next:" + std::to_wstring(tray_page_);
        if (more || previous || next) {
            if (spec_.poll_jump_context) RefreshJumpContext();
            if (!spec_.pulse_tray_available || old_prefix != TrayToken(0)) return;
            tray_expanded_ = true;
            if (more) tray_page_ = 0;
            else if (previous && tray_page_ > 0) --tray_page_;
            else if (next) ++tray_page_;
            Relayout(); Invalidate(); return;
        }
        const bool all = token == old_prefix + L":all";
        const auto shown = tray_display_items_;
        auto found = std::find_if(shown.begin(), shown.end(), [&](const auto& row) { return row.first == token; });
        if (!all && found == shown.end()) return;
        const auto selected_item = all ? picker_places::TrayItem{} : found->second;
        if (spec_.poll_jump_context) RefreshJumpContext();
        if (!spec_.pulse_tray_available || old_prefix != TrayToken(0)) return;
        std::vector<std::wstring> paths;
        std::vector<picker_places::SelectedItem> metadata;
        std::vector<picker_places::TrayItem> pickups;
        for (const auto& item : spec_.pulse_tray_items) {
            if (!all && (item.id != selected_item.id || item.path != selected_item.path)) continue;
            if (!TrayCompatible(item)) continue;
            if (!all && BasketEnabled() && TrayPathSelected(item.path)) {
                std::erase_if(tray_pickups_, [&](const auto& picked) { return SamePickerPath(picked.item.path, item.path); });
                std::erase_if(basket_, [&](const auto& picked) { return SamePickerPath(picked.path, item.path); });
                std::erase_if(selected_set_, [&](int index) { return index >= 0 && static_cast<size_t>(index) < entries_.size() && SamePickerPath(entries_[index].path, item.path); });
                filename_from_selection_ = true; SyncSelectedNames(); UpdateChosen(); Relayout(); Invalidate(); return;
            }
            if (all && TrayPathSelected(item.path)) continue;
            if (std::any_of(paths.begin(), paths.end(), [&](const auto& path) { return SamePickerPath(path, item.path); })) continue;
            pickups.push_back(item);
            paths.push_back(item.path); metadata.push_back({item.path, false, item.size});
            if (!BasketEnabled()) break;
        }
        if (paths.empty() || !IncomingFilesReady()) return;
        size_t selected_files = basket_.size();
        for (const int index : selected_set_) if (index >= 0 && static_cast<size_t>(index) < entries_.size() &&
            (entries_[index].kind == PickerEntryKind::File || entries_[index].kind == PickerEntryKind::Image)) ++selected_files;
        if (paths.size() + selected_files > PULSE_PICKER_UI_V2_MAX_PATHS) {
            chrome_.notice = l10n::Get(l10n::StringId::PickerSelectionLimit);
            Invalidate(); return;
        }
        user_interacted_ = true;
        for (const auto& item : pickups) RememberTrayPickup(item);
        ValidateNames(std::move(paths), false, false, true, BasketEnabled(), &metadata);
    }

    // 「{程序} 常用」: pinned, then most used, then the last place; no visit counts.
    std::vector<PickerCommonRow> CommonRows() const {
        PickerCommonInput input;
        if (!spec_.host_app_name.empty()) input.pinned = app_pinned_places_;
        for (const auto& place : spec_.app_recent_places) input.history.push_back({place.path, place.label});
        input.last_location = spec_.last_location.path;
        for (const auto& entry : pulse_places_.folders) {
            if (entry.open || entry.pinned) continue;
            picker_user_storage::Path parsed;
            if (picker_user_storage::Parse(entry.path, parsed)) input.pulse_recent.push_back({parsed.value, entry.label});
        }
        for (const auto& item : pulse_places_.starred_items) if (item.kind == 2) input.favorites.push_back(item.path);
        input.forgotten = common_forgotten_;
        if (!common_forget_.path.empty()) input.forgotten.push_back(common_forget_.path);
        // A same-name hint names the parent the way the sidebar does (桌面, not Desktop).
        const auto parent_label = [this](const std::wstring& parent) {
            for (const auto& place : standard_places_) if (SamePickerPath(place.path, parent)) return place.label;
            return PickerLeafName(parent);
        };
        return BuildPickerCommonRows(input, [](const std::wstring& path) { return PickerLeafName(path); }, parent_label);
    }

    bool IsRemovableCommon(const std::wstring& path) const {
        return std::any_of(common_rows_.begin(), common_rows_.end(),
            [&](const auto& row) { return !row.pinned && !row.from_pulse && SamePickerPath(row.path, path); });
    }

    static std::wstring HiddenGroupToken(int id) { return kHiddenGroupPrefix + std::to_wstring(id); }
    bool GroupHidden(int id) const {
        const auto token = HiddenGroupToken(id);
        return std::find(layout_settings_.hidden_places.begin(), layout_settings_.hidden_places.end(), token) !=
            layout_settings_.hidden_places.end();
    }
    // The hidden places the place menu lists (hidden groups have their own menu and notice).
    std::vector<std::wstring> HiddenPlacesOnly() const {
        std::vector<std::wstring> out;
        for (const auto& path : layout_settings_.hidden_places) if (!path.starts_with(kHiddenGroupPrefix)) out.push_back(path);
        return out;
    }

    // Four groups in the order a choice is likely: this program's places, the staging
    // tray, Pulse's favorites, This PC. A folder shows once among the groups that
    // change; This PC is a fixed map and always complete.
    void BuildSidebar() {
        vm_.sidebar.clear();
        if (navigation_root_.Active()) { navigation_root_.Append(vm_.sidebar); return; }
        common_rows_ = CommonRows();
        {
            SidebarGroup common; common.id = kGroupAppCommon; common.collapsed = collapsed_[CollapseSlot(kGroupAppCommon)];
            common.header = spec_.host_app_name.empty() ? l10n::Get(l10n::StringId::Recent)
                : FillPickerText(l10n::Get(l10n::StringId::PickerAppCommonHeader), spec_.host_app_name);
            common.rail_header = l10n::Get(l10n::StringId::PickerRailCommon);
            if (!spec_.host_app_name.empty()) {
                common.add_action = SidebarAddAction::PinToApp;
                common.pin_active = IsPathPinnedToApp(current_);
            }
            for (const auto& row : common_rows_) {
                auto item = PlaceItem({row.label, row.path, L"\xE8B7"});
                item.ellipsize = true;
                if (!row.hint.empty()) { item.detail = row.hint; item.detail_hint = true; }
                if (row.last) {
                    item.badge = l10n::Get(mode_ == PickerMode::Save ? l10n::StringId::PickerCommonLastSaved : l10n::StringId::PickerCommonLast);
                    item.badge_color = accent_;
                } else if (row.pinned) {
                    item.badge = l10n::Get(l10n::StringId::PickerAppPinnedTag);
                    item.badge_color = accent_;
                } else if (row.from_pulse) {
                    item.badge = l10n::Get(l10n::StringId::PickerCommonFromPulse);
                }
                item.removable = !row.pinned && !row.from_pulse;
                common.items.push_back(std::move(item));
            }
            if (!common_forget_.path.empty()) {
                SidebarItem undo;
                undo.path = kCommonUndoToken;
                undo.label = FillPickerText(l10n::Get(l10n::StringId::PickerCommonRemoved), common_forget_.label);
                undo.icon_glyph = L"\xE7A7";
                undo.ellipsize = true;
                undo.badge = l10n::Get(l10n::StringId::Undo);
                undo.badge_color = accent_;
                common.items.push_back(std::move(undo));
            }
            if (!common.items.empty()) vm_.sidebar.push_back(std::move(common));
        }
        AppendPickerTray();
        for (auto& group : vm_.sidebar) if (group.id == kGroupPickerTray) group.rail_header = l10n::Get(l10n::StringId::PickerRailTray);
        if (!(spec_.native_options & FOS_HIDEPINNEDPLACES)) {
            AppendPickerFavoritesMerged(vm_, pulse_places_, collapsed_[CollapseSlot(kPickerFavoritesSection)],
                folded_favorite_groups_, PickerLeafName, [this](const std::wstring& path) {
                    return std::any_of(common_rows_.begin(), common_rows_.end(), [&](const auto& row) { return SamePickerPath(row.path, path); });
                });
        }
        if (!spec_.places.empty() || !spec_.bottom_places.empty()) {
            SidebarGroup caller; caller.id = kGroupProgramPlaces; caller.header = l10n::Get(l10n::StringId::PickerApplicationPlaces);
            caller.rail_header = l10n::Get(l10n::StringId::PickerRailProgram);
            caller.collapsed = collapsed_[CollapseSlot(kGroupProgramPlaces)];
            for (const auto& place : spec_.places) caller.items.push_back(PlaceItem(place));
            for (const auto& place : spec_.bottom_places)
                if (std::none_of(caller.items.begin(), caller.items.end(), [&](const auto& item) { return SamePickerPath(item.path, place.path); }))
                    caller.items.push_back(PlaceItem(place));
            vm_.sidebar.push_back(std::move(caller));
        }
        // 此电脑: the everyday folders (Pulse's quick-access pins, as in the main window,
        // plus any of 桌面/下载/文档/图片 they leave out), the drives, the network places.
        SidebarGroup pc;
        pc.id = kGroupDrives;
        pc.header = l10n::Get(l10n::StringId::ThisPc);
        pc.rail_header = l10n::Get(l10n::StringId::PickerRailThisPc);
        pc.collapsed = collapsed_[CollapseSlot(kGroupDrives)];
        pc.navigable = true;
        pc.navigation_path = L"";
        if (!(spec_.native_options & FOS_HIDEPINNEDPLACES)) {
            const auto add = [&](PickerPlace place) {
                if (place.path.empty() || std::any_of(pc.items.begin(), pc.items.end(),
                    [&](const auto& item) { return SamePickerPath(item.path, place.path); })) return;
                for (const auto& known : standard_places_)
                    if (SamePickerPath(known.path, place.path)) { place.glyph = known.glyph; if (place.label.empty()) place.label = known.label; }
                if (place.label.empty()) place.label = PickerLeafName(place.path);
                pc.items.push_back(PlaceItem(place));
            };
            for (const auto& entry : pulse_places_.folders) {
                if (!entry.pinned) continue;
                picker_user_storage::Path parsed;
                if (picker_user_storage::Parse(entry.path, parsed)) add({entry.label, parsed.value, L"\xE8B7"});
            }
            for (const auto& place : standard_places_) add(place);
        }
        const size_t before_drives = pc.items.size();
        for (const auto& drive : drives_) pc.items.push_back(drive);
        if (before_drives && pc.items.size() > before_drives) pc.items[before_drives].divider_above = true;
        if (!(spec_.native_options & FOS_HIDEPINNEDPLACES)) {
            bool first = true;
            for (const auto& entry : pulse_places_.networks) {
                if (!picker_places::detail::DiskPath(entry.path) ||
                    std::any_of(pc.items.begin(), pc.items.end(), [&](const auto& item) { return SamePickerPath(item.path, entry.path); })) continue;
                SidebarItem item;
                item.path = entry.path;
                item.label = entry.label.empty() ? PickerLeafName(entry.path) : entry.label;
                item.icon_glyph = L"\xE968";
                item.divider_above = first && !pc.items.empty();
                first = false;
                pc.items.push_back(std::move(item));
            }
        }
        vm_.sidebar.push_back(std::move(pc));
        FilterPickerHiddenPlaces(vm_, HiddenPlacesOnly());
        int hidden_groups = 0;
        for (auto& group : vm_.sidebar) {
            group.hidden = GroupHidden(group.id);
            if (group.hidden) ++hidden_groups;
        }
        if (hidden_groups) {
            SidebarGroup notice; notice.id = kGroupHiddenNotice;
            SidebarItem row;
            row.path = kShowHiddenGroupsToken;
            row.label = l10n::Get(l10n::StringId::PickerGroupsHiddenCount);
            if (const auto at = row.label.find(L"{count}"); at != std::wstring::npos) row.label.replace(at, 7, std::to_wstring(hidden_groups));
            row.icon_glyph = L"\xE7B3";
            row.badge = l10n::Get(l10n::StringId::PickerGroupsShow);
            row.badge_color = accent_;
            notice.items.push_back(std::move(row));
            vm_.sidebar.push_back(std::move(notice));
        }
        // Shell filters apply to enumerated view objects, not unrelated navigation
        // shortcuts. Keep sidebar catalog data untouched and never rebind/probe it.
    }

    // × on a 常用 row: hidden at once, written after the undo window (or on close).
    void ForgetCommon(const std::wstring& path) {
        const auto row = std::find_if(common_rows_.begin(), common_rows_.end(),
            [&](const auto& r) { return SamePickerPath(r.path, path); });
        if (row == common_rows_.end() || row->pinned || row->from_pulse) return;
        CommitCommonForget();
        common_forget_.path = row->path;
        common_forget_.label = row->label;
        if (hwnd_) SetTimer(hwnd_, kCommonForgetTimer, kCommonUndoMs, nullptr);
        Relayout(); Invalidate();
    }
    void UndoCommonForget() {
        if (common_forget_.path.empty()) return;
        common_forget_ = {};
        if (hwnd_) KillTimer(hwnd_, kCommonForgetTimer);
        Relayout(); Invalidate();
    }
    void CommitCommonForget() noexcept {
        if (common_forget_.path.empty()) return;
        const auto path = common_forget_.path;
        common_forget_ = {};
        if (hwnd_) KillTimer(hwnd_, kCommonForgetTimer);
        try {
            common_forgotten_.push_back(path);
            std::erase_if(spec_.app_recent_places, [&](const auto& place) { return SamePickerPath(place.path, path); });
            if (SamePickerPath(spec_.last_location.path, path)) spec_.last_location = {};
            if (spec_.forget_recent_place) spec_.forget_recent_place(path);
        } catch (...) {}
    }

    // Right click on a group title: fold it, hide it, or pick the groups shown.
    void ShowSidebarGroupMenu(int index, POINT point) {
        if (index < 0 || index >= static_cast<int>(vm_.sidebar.size())) return;
        const SidebarGroup group = vm_.sidebar[static_cast<size_t>(index)];
        std::vector<FluentMenuItem> items;
        items.push_back(Item(1, l10n::Get(group.collapsed ? l10n::StringId::PickerGroupExpand : l10n::StringId::PickerGroupCollapse)));
        items.push_back(Item(2, l10n::Get(l10n::StringId::PickerGroupHide), L"\xED1A"));
        items.back().separator_after = true;
        FluentMenuItem caption = Item(0, l10n::Get(l10n::StringId::PickerGroupsShown), L"", false);
        items.push_back(std::move(caption));
        std::vector<int> ids;
        for (const auto& g : vm_.sidebar) {
            // Empty groups are not drawn, so offering them would only confuse.
            if (g.id == kGroupHiddenNotice || g.header.empty() || (g.items.empty() && !g.hidden)) continue;
            FluentMenuItem row = Item(10 + static_cast<int>(ids.size()), g.header);
            row.toggle = true;
            row.checked = !g.hidden;
            items.push_back(std::move(row));
            ids.push_back(g.id);
        }
        const int command = TrackAt(point, std::move(items));
        if (done_ || command <= 0) return;
        auto& hidden = layout_settings_.hidden_places;
        const auto set_hidden = [&](int id, bool on) {
            const auto token = HiddenGroupToken(id);
            std::erase(hidden, token);
            if (on) hidden.push_back(token);
        };
        if (command == 1) { bool& c = group.id == kGroupPickerTray ? tray_collapsed_ : collapsed_[CollapseSlot(group.id)]; c = !c; }
        else if (command == 2) set_hidden(group.id, true);
        else if (command >= 10 && command < 10 + static_cast<int>(ids.size())) {
            const int id = ids[static_cast<size_t>(command - 10)];
            set_hidden(id, !GroupHidden(id));
        }
        Relayout(); Invalidate();
    }

    int CollapseSlot(int group_id) const {
        return std::clamp(group_id - 1, 0, 8);
    }

    // The main window's view model, reduced to what the embedded renderer
    // draws: the address row, the sidebar and one pane.
    void SyncVm() {
        chrome_.pulse_selection_enabled = IncomingFilesReady();
        chrome_.show_tray_remove = TrayConsumeAvailable() && (HasSelectedTrayItem() || (!BasketEnabled() && HasCompatibleTrayItem()));
        chrome_.tray_remove_checked = (picker_flags_ & picker_layout::kFlagRemoveTrayAfterOpen) != 0;
        chrome_.tray_remove_text = l10n::Get(l10n::StringId::PickerTrayRemoveAfterOpen);
        vm_.dark = dark_;
        vm_.focused = GetForegroundWindow() == hwnd_;
        vm_.can_go_back = history_.CanGoBack();
        vm_.can_go_forward = history_.CanGoForward();
        vm_.tray_deck.hidden = true;
        vm_.details_visible = PreviewPaneShown();
        vm_.details = {};
        vm_.details.preview_only = true;
        vm_.details.preview_enabled = !InArchive() && mode_ != PickerMode::Save;
        if (vm_.details_visible && mode_ == PickerMode::Save) {
            auto& details = vm_.details;
            details.has_selection = true;
            details.picker_save_mode = true;
            details.picker_save_to_tray = saving_to_tray_;
            details.save_tray_count = static_cast<int>(spec_.pulse_tray_total_count ? spec_.pulse_tray_total_count : spec_.pulse_tray_items.size());
            const auto save_target = ResolveSaveTarget(SaveDraftRequest());
            const auto save_parent = save_target.state == SaveTargetState::Resolved ? PickerParent(save_target.path) : current_;
            details.save_folder_text = saving_to_tray_ ? L"Pulse\\shelf"
                : (save_parent.empty() ? l10n::Get(l10n::StringId::ThisPc) : PickerLeafName(save_parent));
            const PickerEntry* existing_target = nullptr;
            // Expanded siblings can have the same names. Summarize only the
            // actual save parent, which need not be the navigation root.
            int series_count = 0;
            std::wstring series_latest;
            uint64_t latest_mtime = 0;
            FILETIME latest_ft{};
            if (!filename_text_.empty()) {
                const auto target = ResolveSaveTarget(SaveDraftRequest());
                if (target.state == SaveTargetState::Resolved) {
                    const auto key = PickerSaveSeriesKey(target.path);
                    for (const auto& entry : entries_) {
                        if (!SamePickerPath(PickerParent(entry.path), save_parent)) continue;
                        if (SamePickerPath(entry.path, target.path) && entry.kind != PickerEntryKind::Folder && entry.kind != PickerEntryKind::Drive)
                            existing_target = &entry;
                        if (!PickerSaveSeriesMatches(key, entry.name,
                            entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive)) continue;
                        ++series_count;
                        const uint64_t mt = (static_cast<uint64_t>(entry.modified.dwHighDateTime) << 32) | entry.modified.dwLowDateTime;
                        if (series_latest.empty() || mt >= latest_mtime) {
                            latest_mtime = mt;
                            latest_ft = entry.modified;
                            series_latest = entry.name;
                        }
                    }
                }
            }
            if (series_count > 0) {
                std::wstring items_str = l10n::Get(l10n::StringId::ItemsCountFormat);
                if (const auto p = items_str.find(L"%d"); p != std::wstring::npos) items_str.replace(p, 2, std::to_wstring(series_count));
                std::wstring st = l10n::Get(l10n::StringId::PickerSaveSeriesLatest);
                if (const auto p = st.find(L"{items}"); p != std::wstring::npos) st.replace(p, 7, items_str);
                if (const auto p = st.find(L"{latest}"); p != std::wstring::npos) st.replace(p, 8, series_latest);
                if (const auto p = st.find(L"{time}"); p != std::wstring::npos) {
                    const std::wstring tm = latest_mtime ? format::LocalFileTime(latest_ft) : std::wstring();
                    if (!tm.empty()) st.replace(p, 6, tm);
                    else st.replace(p > 0 ? p - 1 : p, p > 0 ? 7 : 6, L"");
                }
                details.save_series_text = std::move(st);
            } else {
                details.save_series_text = l10n::Get(l10n::StringId::PickerSaveSeriesNone);
            }
            // Disk free space from draft probe or cached sidebar drive list (zero UI-thread I/O).
            std::wstring drive_letter = save_parent.size() >= 2 && save_parent[1] == L':' ? save_parent.substr(0, 2) : std::wstring();
            if (!drive_letter.empty()) drive_letter[0] = static_cast<wchar_t>(std::towupper(drive_letter[0]));
            const auto format_disk_free = [&](uint64_t free_bytes) {
                std::wstring dt = l10n::Get(l10n::StringId::PickerSaveDiskFree);
                if (const auto p = dt.find(L"{drive}"); p != std::wstring::npos) dt.replace(p, 7, drive_letter);
                if (const auto p = dt.find(L"{free}"); p != std::wstring::npos) dt.replace(p, 6, format::ByteSize(free_bytes));
                while (!dt.empty() && dt.front() == L' ') dt.erase(dt.begin());
                return dt;
            };
            if (save_draft_result_ && save_draft_result_->space_known) {
                details.save_disk_text = format_disk_free(save_draft_result_->free_bytes);
            } else if (!drive_letter.empty()) {
                for (const auto& d : drives_) {
                    if (d.path.size() >= 2 && std::towupper(d.path[0]) == drive_letter[0] && d.free_bytes != ~uint64_t{0}) {
                        details.save_disk_text = format_disk_free(d.free_bytes);
                        break;
                    }
                }
            }
            if (details.save_disk_text.empty()) details.save_disk_text = L"—";
            const bool ro = save_draft_result_ && save_draft_result_->attributes != INVALID_FILE_ATTRIBUTES &&
                (save_draft_result_->attributes & FILE_ATTRIBUTE_READONLY) != 0;
            details.save_perm_ok = !ro;
            details.save_perm_text = l10n::Get(ro ? l10n::StringId::PickerSaveDestReadOnly : l10n::StringId::PickerSaveDestWritable);
            const bool cloud = save_draft_result_ && save_draft_result_->attributes != INVALID_FILE_ATTRIBUTES &&
                (save_draft_result_->attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0;
            details.save_sync_text = l10n::Get(cloud ? l10n::StringId::PickerSaveDestCloud : l10n::StringId::PickerSaveDestLocal);
            if (chrome_.overwrite_pending && !chrome_.notice.empty()) {
                details.save_clash = true;
                details.save_banner_text = chrome_.notice;
            } else if (chrome_.live_conflict && !chrome_.live_conflict_text.empty()) {
                details.save_clash = true;
                details.save_banner_text = chrome_.live_conflict_text;
            } else if (existing_target) {
                details.save_clash = true;
                auto text = l10n::Get(l10n::StringId::PickerSaveDestClash);
                if (const auto p = text.find(L"{name}"); p != std::wstring::npos) text.replace(p, 6, existing_target->name);
                if (const auto p = text.find(L"{detail}"); p != std::wstring::npos) text.replace(p, 8, format::ByteSize(existing_target->size));
                details.save_banner_text = std::move(text);
            } else {
                details.save_clash = false;
                details.save_banner_text = l10n::Get(l10n::StringId::PickerSaveDestNoClash);
            }
            details.save_after_available = spec_.enable_save_followup && pulse_places_.save_followup_available;
            details.save_after_show = (picker_flags_ & picker_layout::kFlagShowAfterSave) != 0;
            details.save_after_copy = (picker_flags_ & picker_layout::kFlagCopyAfterSave) != 0;
        } else if (vm_.details_visible) {
            if (const auto* entry = SelectedEntry(); entry && selected_set_.size() == 1) {
                auto& details = vm_.details;
                details.has_selection = true;
                details.name = entry->name; details.path = entry->path;
                details.is_dir = entry->kind == PickerEntryKind::Folder || entry->kind == PickerEntryKind::Drive;
                details.picker_archive = entry->archive;
                details.attrs = details.is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0;
                details.modified_value = (static_cast<uint64_t>(entry->modified.dwHighDateTime) << 32) | entry->modified.dwLowDateTime;
                details.size_value = entry->size;
                details.view_generation = generation_;
            }
        }
        vm_.settings_open = false;
        // Paint for the material DWM actually gave the window (see ApplyBackdrop).
        vm_.window_effect = !backdrop_enabled_ ? WindowEffect::None : IsAcrylicBackdrop(hwnd_) ? WindowEffect::Acrylic :
            spec_.window_effect == WindowEffect::Mica ? WindowEffect::Mica : WindowEffect::MicaAlt;
        vm_.address_searching = searching_;
        vm_.address_editing = searching_ ? true : address_editing_;
        vm_.address_search_current = true;
        vm_.address_search_content = false;
        vm_.address_search_text = search_text_;
        vm_.address_search_placeholder = current_.empty() || picker_shell::IsParsingName(current_)
            ? l10n::Get(l10n::StringId::Search)
            : FillPickerText(l10n::Get(l10n::StringId::PickerSearchFolder), PickerLeafName(current_));
        vm_.address_search_has_text = !search_text_.empty();
        vm_.address_search_scope_label = l10n::Get(l10n::StringId::ThisFolder).c_str();
        BuildSidebar();

        PaneViewModel& pane = vm_.pane;
        pane.cut_names.clear();
        for (const auto& cut_path : cut_paths_) {
            if (SamePickerPath(PickerParent(cut_path), current_))
                pane.cut_names.insert(PickerLeafName(cut_path));
        }
        pane.show_file_extensions = spec_.show_file_extensions ? 1 : 0;
        pane.path = current_;
        pane.header_text.clear();
        pane.snapshot = snapshot_;
        pane.tree = new_folder_draft_ ? nullptr : tree_rows_;
        if (!InArchive()) image_dimensions_.Bind(snapshot_, entries_);
        else image_dimensions_.Bind({}, {});
        pane.extension_columns.clear();
        pane.extension_cell = {};
        if (image_dimensions_.Active()) {
            pane.extension_columns.push_back({L"pulse.picker/dimensions", l10n::Get(l10n::StringId::ColumnMediaDims), 112.0f});
            pane.extension_cell = [this](int source, size_t column, std::wstring& text, uint32_t& tone) {
                if (column != 0) return false;
                text = image_dimensions_.Cell(source);
                tone = 0;
                return !text.empty();
            };
        }
        pane.change_badges.clear();
        pane.muted_indices.clear();
        if (mode_ == PickerMode::Save) {
            for (size_t i = 0; i < entries_.size(); ++i) {
                const auto& entry = entries_[i];
                if (entry.kind != PickerEntryKind::Folder && entry.kind != PickerEntryKind::Drive &&
                    !PickerMatchesFilter(entry.name, mode_, options_)) pane.muted_indices.insert(static_cast<int>(i));
            }
        }
        for (size_t i = 0; i < entries_.size(); ++i) if (entries_[i].is_new) {
            ChangeBadge badge; badge.label = l10n::Get(l10n::StringId::PickerNewBadge);
            badge.tooltip = l10n::Get(l10n::StringId::PickerNewFiles); badge.count = 1;
            badge.last_change = (static_cast<uint64_t>(entries_[i].modified.dwHighDateTime) << 32) | entries_[i].modified.dwLowDateTime;
            pane.change_badges.emplace(static_cast<int>(i), std::move(badge));
        }
        if (mode_ == PickerMode::Save && !filename_text_.empty()) {
            const auto target = ResolveSaveTarget(SaveDraftRequest());
            if (target.state == SaveTargetState::Resolved) {
                const auto key = PickerSaveSeriesKey(target.path);
                const auto parent = PickerParent(target.path);
                for (size_t i = 0; i < entries_.size(); ++i) {
                    const auto& entry = entries_[i];
                    if (TreePlaceholder(static_cast<int>(i)) || !SamePickerPath(PickerParent(entry.path), parent)) continue;
                    if (!PickerSaveSeriesMatches(key, entry.name,
                        entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive)) continue;
                    const bool exact_clash = SamePickerPath(entry.path, target.path) &&
                        entry.kind != PickerEntryKind::Folder && entry.kind != PickerEntryKind::Drive;
                    ChangeBadge badge;
                    badge.label = l10n::Get(exact_clash ? l10n::StringId::PickerWillReplaceBadge : l10n::StringId::PickerSameNameSeries);
                    badge.tooltip = badge.label; badge.count = 1;
                    if (exact_clash) badge.status = 2;
                    pane.change_badges[static_cast<int>(i)] = std::move(badge);
                }
            }
        }
        pane.entries.clear();
        pane.is_file_system = !current_.empty();
        pane.can_go_back = vm_.can_go_back;
        pane.can_go_forward = vm_.can_go_forward;
        pane.can_go_up = !current_.empty();
        pane.can_create = !current_.empty() && error_.empty() && !waiting_;
        // selected_ is the keyboard caret and may have been Ctrl-deselected.
        // Use the main pane's separate selection and focus-ring channels.
        const bool caret_selected = selected_set_.contains(selected_);
        pane.selected_index = caret_selected ? selected_ : -1;
        pane.focus_ring_source = caret_selected ? -1 : selected_;
        pane.marquee_active = marquee_.active;
        pane.marquee_rect = marquee_.active ? marquee_.Rect(scroll_, renderer_.PaneListRect(pane, PaneBounds()), ScrollX()) : D2D1_RECT_F{};
        pane.selected_indices = &selected_set_;
        pane.selected_count = static_cast<int>(selected_set_.size());
        pane.rename_index = rename_path_.empty() ? -1 : rename_index_;
        pane.rename_text = rename_edit_ && !rename_path_.empty() ? ReadEdit(rename_edit_) : std::wstring();
        pane.view_mode = view_mode_;
        pane.icon_dip = icon_dip_;
        pane.details_column_dividers = layout_settings_.column_widths;
        pane.sort_column = static_cast<SortColumn>(order_.column);
        pane.sort_direction = order_.descending ? SortDirection::Desc : SortDirection::Asc;
        pane.group_select = allow_multiselect_;
        SyncListGroups(pane);
        pane.focused = focus_ == kPickList;
        pane.scroll_y = scroll_;
        pane.scroll_x = ScrollX();
        pane.loading = waiting_ && loading_;
        pane.is_search = false;
        pane.banner_kind = error_.empty() ? 0 : 3;
        pane.banner_title.clear();
        pane.banner_message = error_;
        pane.hover_index = vm_.hover_region == static_cast<int>(HitTestResult::Row)
                               ? vm_.hover_control_index : -1;
        vm_.breadcrumb_hover = vm_.hover_region == static_cast<int>(HitTestResult::BreadcrumbSegment)
                                   ? vm_.hover_control_index : -1;

        chrome_.selected_names.clear(); chrome_.selected_sources.clear(); chrome_.selected_from_tray.clear();
        selection_tag_paths_.clear();
        std::unordered_set<std::wstring> tray_paths;
        if (allow_multiselect_ && filename_from_selection_ && !editing_selection_names_)
            for (const auto& item : spec_.pulse_tray_items) tray_paths.insert(ListOrderKey(item.path));
        const auto append_selection = [&](const std::wstring& name, const std::wstring& path) {
            chrome_.selected_names.push_back(name); selection_tag_paths_.push_back(path);
            const bool tray = tray_paths.contains(ListOrderKey(path));
            chrome_.selected_from_tray.push_back(tray);
            const auto parent = PickerParent(path);
            auto label = PickerLeafName(parent);
            if (label.empty()) label = path::FriendlyPathText(parent);
            chrome_.selected_sources.push_back(tray ? l10n::Get(l10n::StringId::PickerTrayTitle) : label);
        };
        chrome_.filename_label = spec_.has_file_name_label ? spec_.file_name_label :
            l10n::Get(mode_ == PickerMode::Folder ? l10n::StringId::FolderName : l10n::StringId::FileName);
        selection_tag_indices_.clear();
        if (allow_multiselect_ && filename_from_selection_ && !editing_selection_names_) {
            for (size_t k = 0; k < basket_.size(); ++k) {
                append_selection(basket_[k].name, basket_[k].path);
                selection_tag_indices_.push_back(-1 - static_cast<int>(k));  // negative: a basket file
            }
            std::vector<int> sorted(selected_set_.begin(), selected_set_.end()); std::sort(sorted.begin(), sorted.end());
            for (int index : sorted) if (index >= 0 && index < static_cast<int>(entries_.size())) {
                const auto& entry = entries_[static_cast<size_t>(index)];
                if (entry.kind != PickerEntryKind::Folder && entry.kind != PickerEntryKind::Drive) {
                    append_selection(entry.name, entry.path); selection_tag_indices_.push_back(index);
                }
            }
        }
        chrome_.primary_enabled = legacy_browse_ok_ && !new_folder_draft_ && !chrome_.overwrite_pending && !waiting_ && !validating_ && !chosen_.empty() && !save_name_invalid_ && !chrome_.live_conflict;
        if (!chrome_.selected_names.empty() && !spec_.has_file_name_label) {
            wchar_t count[128]{};
            swprintf_s(count, l10n::Get(l10n::StringId::SelectedCountFormat).c_str(), static_cast<int>(chrome_.selected_names.size()));
            chrome_.filename_label = count;
        }
        chrome_.primary_text = normal_primary_text_;
        if (allow_multiselect_ && chrome_.selected_names.size() > 1)  // HTML 发送 (3)
            chrome_.primary_text += L" (" + std::to_wstring(chrome_.selected_names.size()) + L")";
        chrome_.overwrite_replace_text = chrome_.overwrite_pending ? l10n::Get(l10n::StringId::Replace) : std::wstring();
        chrome_.overwrite_back_text = l10n::Get(l10n::StringId::PickerBackToEdit);
        chrome_.rename_text = pending_overwrite_ ? FillPickerText(l10n::Get(l10n::StringId::PickerSaveAsName),
            OverwriteAlternative(pending_overwrite_->Path())) : l10n::Get(l10n::StringId::PickerRenameSave);
        SyncJumpStrip();
        chrome_.save_format_notice = mode_ == PickerMode::Save && options_.filter_index == save_format_notice_filter_ &&
            filename_text_ == save_format_notice_filename_ ? save_format_notice_ : std::wstring();
        chrome_.filename_focused = focus_ == kPickFilename;
        chrome_.focus = focus_;
    }

    static std::wstring PickerLeafName(const std::wstring& path) {
        std::wstring name = path::FriendlyPathText(path);
        while (name.size() > 3 && name.back() == L'\\') name.pop_back();
        const size_t slash = name.find_last_of(L'\\');
        return slash != std::wstring::npos && slash + 1 < name.size() && !IsShareRootName(name)
            ? name.substr(slash + 1) : name;
    }

    SaveTargetRequest SaveDraftRequest() const {
        SaveTargetRequest request;
        request.directory = current_; request.filename = filename_text_;
        request.filters = options_.filters; request.filter_index = options_.filter_index;
        request.default_extension = spec_.default_extension;
        request.native_save_defaults = spec_.native_save_defaults; request.filter_changed = save_filter_changed_;
        request.caller_validates_access = spec_.skip_access_validation;
        request.reject_readonly_save = spec_.reject_readonly_save;
        request.no_test_file_create = spec_.no_test_file_create;
        request.require_existing_save_parent = spec_.require_existing_save_parent;
        return request;
    }

    void UpdateSaveWarning(const SaveTargetResult& target) {
        chrome_.summary_warning.clear(); chrome_.summary_warning_error = false;
        chrome_.save_space_text.clear();
        chrome_.live_conflict = false; save_name_invalid_ = false;
        if (filename_text_.empty() || chrome_.overwrite_pending || validating_ || waiting_) return;
        if (target.state != SaveTargetState::Resolved) {
            const auto cut = filename_text_.find_last_of(L"\\/");
            const auto leaf = filename_text_.substr(cut == std::wstring::npos ? 0 : cut + 1);
            const bool long_name = leaf.size() > 255 || filename_text_.size() + current_.size() + 1 >= 32760;
            chrome_.summary_warning = l10n::Get(long_name ? l10n::StringId::PickerSaveNameLong :
                target.state == SaveTargetState::InvalidDirectory ? l10n::StringId::PickerSavePathInvalid : l10n::StringId::PickerSaveNameInvalid);
            chrome_.summary_warning_error = save_name_invalid_ = true;
            return;
        }
        const auto leaf = PickerLeafName(target.path);
        PickerSaveAdvisory advisory;
        if (save_draft_result_ && save_draft_result_->path == target.path) {
            advisory = DescribePickerSaveTarget(*save_draft_result_);
            chrome_.save_space_text = advisory.space;
        }
        if (save_draft_result_ && save_draft_result_->path == target.path && save_draft_result_->attributes != INVALID_FILE_ATTRIBUTES) {
            if (save_draft_result_->attributes & FILE_ATTRIBUTE_DIRECTORY) {
                chrome_.summary_warning = l10n::Get(l10n::StringId::PickerLiveDirectoryConflict);
                chrome_.summary_warning_error = save_name_invalid_ = true;
            } else {
                chrome_.live_conflict = true;
                chrome_.live_conflict_text = FillPickerText(l10n::Get(l10n::StringId::PickerLiveConflict), leaf);
                if (!advisory.warning.empty()) {
                    chrome_.live_conflict_text = advisory.warning + L" " + leaf;
                    chrome_.summary_warning = advisory.warning;
                }
                chrome_.live_rename_text = l10n::Get(l10n::StringId::PickerRenameSave);
                chrome_.live_save_text = l10n::Get(l10n::StringId::PickerSaveCheckContinue);
            }
            return;
        }
        if (save_draft_result_ && save_draft_result_->error != ERROR_SUCCESS &&
            save_draft_result_->error != ERROR_FILE_NOT_FOUND && save_draft_result_->error != ERROR_PATH_NOT_FOUND) {
            chrome_.summary_warning = ErrorText(save_draft_result_->error, target.path);
            return; // advisory only; final access checking decides actual writability
        }
        const bool any = options_.filter_index < options_.filters.size() && PickerFilterIsAny(options_.filters[options_.filter_index]);
        if (!advisory.warning.empty()) { chrome_.summary_warning = advisory.warning; return; }
        const auto dot = leaf.find_last_of(L'.');
        if (any && (dot == std::wstring::npos || dot == 0 || dot + 1 == leaf.size()))
            chrome_.summary_warning = FillPickerText(l10n::Get(l10n::StringId::PickerNoExtension), leaf);
    }

    void RefreshSaveDraft() {
        if (mode_ != PickerMode::Save) return;
        const bool old_conflict = chrome_.live_conflict;
        const bool old_suggestions = !chrome_.save_suggestions.empty();
        const auto target = ResolveSaveTarget(SaveDraftRequest());
        const auto key = current_ + L"|" + filename_text_ + L"|" + target.path + L"|" +
            std::to_wstring(options_.filter_index) + L"|" + std::to_wstring(generation_);
        if (key != save_draft_key_) {
            save_draft_key_ = key; ++save_draft_generation_; save_draft_probe_.Cancel(); save_draft_result_.reset();
            save_draft_path_ = target.state == SaveTargetState::Resolved ? target.path : std::wstring();
            save_draft_due_ = save_draft_path_.empty() ? 0 : GetTickCount64() + 180;
            SYSTEMTIME local{}; GetLocalTime(&local); wchar_t date[16]{};
            swprintf_s(date, L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
            save_name_suggestions_ = BuildSaveNameSuggestions(filename_text_, spec_.filename, date);
            chrome_.save_suggestions.clear();
            for (const auto& suggestion : save_name_suggestions_) {
                const auto id = suggestion.kind == SaveNameSuggestionKind::Version ? l10n::StringId::PickerSuggestVersion :
                    suggestion.kind == SaveNameSuggestionKind::Date ? l10n::StringId::PickerSuggestDate :
                    suggestion.kind == SaveNameSuggestionKind::Original ? l10n::StringId::PickerSuggestOriginal : l10n::StringId::PickerSuggestRemoveCopy;
                PickerOptionChip chip; chip.text = PickerLeafName(suggestion.filename) + L" · " + l10n::Get(id);
                chip.label = suggestion.filename; chrome_.save_suggestions.push_back(std::move(chip));
            }
        }
        UpdateSaveWarning(target);
        chrome_.save_actions_enabled = !waiting_ && !validating_ && !operating_ && !done_ && !chrome_.overwrite_pending;
        if (old_conflict != chrome_.live_conflict || old_suggestions != !chrome_.save_suggestions.empty()) {
            chrome_.pressed = kPickNone;
            if (hwnd_) Relayout();
        }
    }

    void PollSaveDraft() {
        if (mode_ != PickerMode::Save || waiting_ || validating_ || done_ || save_draft_path_.empty()) return;
        if (save_draft_due_ && GetTickCount64() >= save_draft_due_)
            save_draft_due_ = save_draft_probe_.Start(save_draft_generation_, save_draft_path_) ? 0 : GetTickCount64() + 1000;
        if (auto result = save_draft_probe_.Take(save_draft_generation_, save_draft_path_)) {
            save_draft_result_ = std::move(result);
            RefreshSaveDraft(); SyncVm(); Relayout(); Invalidate();
        }
    }

    void ApplySaveSuggestion(size_t index) {
        if (!chrome_.save_actions_enabled || index >= save_name_suggestions_.size()) return;
        const auto filename = save_name_suggestions_[index].filename;
        SetFilename(filename); InputChanged(filename_edit_); FocusFilename(); Relayout(); Invalidate();
    }

    void ShowSaveSuggestions() {
        if (!chrome_.save_actions_enabled) return;
        const auto key = save_draft_key_;
        std::vector<FluentMenuItem> items;
        for (size_t i = 0; i < chrome_.save_suggestions.size(); ++i) {
            FluentMenuItem item; item.command = static_cast<int>(i) + 1;
            item.text = chrome_.save_suggestions[i].text; item.detail = chrome_.save_suggestions[i].label;
            items.push_back(std::move(item));
        }
        const int chosen = TrackPickerDropdown(ScreenRect(chrome_layout_.suggestions_more), std::move(items));
        if (key == save_draft_key_ && chosen > 0) ApplySaveSuggestion(static_cast<size_t>(chosen - 1));
    }

    void UpdateSummary() {
        chrome_.summary.clear();
        if (mode_ != PickerMode::Save || chosen_.empty() || chrome_.selected_names.size() > 1) {
            chrome_.summary_warning.clear(); chrome_.summary_warning_error = false;
            chrome_.save_space_text.clear();
        }
        chrome_.summary_lead.clear(); chrome_.summary_folder.clear(); chrome_.summary_leaf.clear();
        if (InArchive()) {
            chrome_.summary = l10n::Get(l10n::StringId::PickerArchiveExtractSelection);
            return;
        }
        if (chrome_.overwrite_pending && pending_overwrite_) {
            // The banner asks; the row under it still names the file that would be replaced.
            const auto target = pending_overwrite_->Path();
            const size_t cut = target.find_last_of(L"\\/");
            if (cut != std::wstring::npos && cut + 1 < target.size()) {
                chrome_.summary_lead = l10n::Get(l10n::StringId::PickerWillSave);
                chrome_.summary_folder = target.substr(0, cut + 1);
                chrome_.summary_leaf = target.substr(cut + 1);
            }
        }
        if (chrome_.selected_names.size() > 1) {
            wchar_t count[128]{};
            swprintf_s(count, l10n::Get(l10n::StringId::SelectedCountFormat).c_str(), static_cast<int>(chrome_.selected_names.size()));
            chrome_.summary = count;
            return;
        }
        if (!chosen_.empty()) {
            // Long choices keep the drive and the chosen folder's name visible.
            const std::wstring prefix = l10n::Get(l10n::StringId::PickerWillSelect);
            const auto measure = [&](std::wstring_view s) {
                return typography::MeasureLine(&compositor_, compositor_.TextFormat(), s);
            };
            const float width = chrome_layout_.summary.right - chrome_layout_.summary.left;
            std::wstring shown = mode_ == PickerMode::Folder ? path::FriendlyPathText(chosen_) : chosen_;
            if (mode_ == PickerMode::Save) {
                SaveTargetRequest request; request.directory = current_;
                request.filename = filename_text_; request.filters = options_.filters;
                request.filter_index = options_.filter_index; request.default_extension = spec_.default_extension;
                request.native_save_defaults = spec_.native_save_defaults; request.filter_changed = save_filter_changed_;
                request.caller_validates_access = spec_.skip_access_validation;
                request.reject_readonly_save = spec_.reject_readonly_save;
                request.no_test_file_create = spec_.no_test_file_create;
                request.require_existing_save_parent = spec_.require_existing_save_parent;
                const auto target = ResolveSaveTarget(request);
                if (target.state == SaveTargetState::Resolved) shown = target.path;
                UpdateSaveWarning(target);
                if (saving_to_tray_ && !chrome_.overwrite_pending && !chrome_.live_conflict && chrome_.summary_warning.empty()) {
                    chrome_.summary = FillPickerText(l10n::Get(l10n::StringId::PickerSaveToTrayStatus), PickerLeafName(shown));
                    return;
                }
            }
            chrome_.summary = prefix + FitPathMiddle(shown, width - measure(prefix), measure);
            // The same target in parts for the drawn line: lead, folder, leaf.
            const size_t cut = shown.find_last_of(L"\\/");
            if (cut != std::wstring::npos && cut + 1 < shown.size()) {
                chrome_.summary_lead = mode_ == PickerMode::Save ? l10n::Get(l10n::StringId::PickerWillSave) : prefix;
                chrome_.summary_folder = shown.substr(0, cut + 1);
                chrome_.summary_leaf = shown.substr(cut + 1);
            }
        } else if (mode_ == PickerMode::Folder && current_.empty() && !waiting_) {
            chrome_.summary = l10n::Get(l10n::StringId::ChooseAFolderOrADrive).c_str();
        } else if (mode_ == PickerMode::Image) {
            chrome_.summary = l10n::Get(l10n::StringId::PickerPickImageHint);
        }
    }

    float MaxScroll() {
        SyncVm();
        return renderer_.MaxScrollForPane(vm_.pane, PaneBounds());
    }

    // Only the List view runs sideways (its columns); other views keep x at 0.
    float ScrollX() const { return view_mode_ == ViewMode::List ? scroll_x_ : 0.0f; }
    float MaxScrollX() {
        if (view_mode_ != ViewMode::List) return 0.0f;
        SyncVm();
        return renderer_.MaxScrollXForPane(vm_.pane, PaneBounds());
    }

    void ClampScroll() {
        if (!compositor_.Dc()) return;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, MaxScroll()));
        scroll_x_ = std::clamp(scroll_x_, 0.0f, std::max(0.0f, MaxScrollX()));
    }

    void EnsureVisible(int index) {
        if (index < 0 || !compositor_.Dc()) return;
        SyncVm();
        const D2D1_RECT_F bounds = PaneBounds();
        const D2D1_RECT_F list = renderer_.PaneListRect(vm_.pane, bounds);
        const D2D1_RECT_F item = renderer_.ItemRectInPane(vm_.pane, bounds, index);
        if (item.top < list.top) scroll_ += item.top - list.top;
        else if (item.bottom > list.bottom) scroll_ += item.bottom - list.bottom;
        if (view_mode_ == ViewMode::List) {
            if (item.left < list.left) scroll_x_ += item.left - list.left;
            else if (item.right > list.right) scroll_x_ += std::min(item.right - list.right, item.left - list.left);
        }
        ClampScroll();
    }

    // ---- hosted edits ------------------------------------------------------

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }
    HBRUSH EditBrush() const { return EditBackBrush(edit_brush_); }
    IDWriteTextFormat* EditFormat(HWND) {
        return compositor_.FileNameFormat() ? compositor_.FileNameFormat() : compositor_.TextFormat();
    }

    void CreateFonts() {
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        font_ = typography::CreateEditFont(scale_, 13.0f);
        if (rename_font_) DeleteObject(rename_font_);
        rename_font_ = typography::CreateEditFont(scale_, 13.0f);
        for (HWND edit : AllEdits())
            if (edit && font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    HWND MakeEdit(int id, const wchar_t* text, DWORD style = 0) {
        HWND edit = spec_.pulse_painted_plain_child ? CreatePulsePaintedChildEdit(hwnd_, text, style)
            : CreateRedirectedChildEdit(hwnd_, text, style);
        if (!edit) {
            if (spec_.pulse_painted_plain_child) {
                const DWORD error = GetLastError(); native_error_ = error ? error : ERROR_GEN_FAILURE;
                NoteFailure(native_error_, "edit-create");
            }
            return nullptr;
        }
        SetWindowLongPtrW(edit, GWLP_ID, id);
        SetWindowTheme(edit, L"", L"");
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (!SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this)) && spec_.pulse_painted_plain_child) {
            const DWORD error = GetLastError(); native_error_ = error ? error : ERROR_GEN_FAILURE;
            NoteFailure(native_error_, "edit-subclass");
            DestroyWindow(edit); return nullptr;
        }
        return edit;
    }

    std::vector<HWND> AllEdits() const {
        std::vector<HWND> edits{path_edit_,filename_edit_,search_edit_,rename_edit_};
        for(HWND edit:custom_edits_)if(edit)edits.push_back(edit);
        return edits;
    }
    void CreateEdits() {
        path_edit_ = MakeEdit(kPathEditId, L"");
        {
            filename_edit_ = MakeEdit(kFilenameEditId, filename_text_.c_str());
            if (filename_edit_) SendMessageW(filename_edit_, EM_SETCUEBANNER, TRUE,
                reinterpret_cast<LPARAM>(l10n::Get(l10n::StringId::TypeANameOrPickOneAbove).c_str()));
        }
        search_edit_ = MakeEdit(kSearchEditId, L"");
        if (search_edit_) SendMessageW(search_edit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(l10n::Get(l10n::StringId::SearchThisFolder).c_str()));
        custom_edits_.resize(spec_.custom_controls.size(),nullptr);
        for(size_t i=0;i<spec_.custom_controls.size();++i)if(spec_.custom_controls[i].kind==8) {
            custom_edits_[i]=MakeEdit(kCustomEditBase+static_cast<int>(i),spec_.custom_controls[i].text.c_str());
            if(!custom_edits_[i]){FailPaintedEdit(ERROR_NOT_ENOUGH_MEMORY);return;}
            SendMessageW(custom_edits_[i],EM_SETLIMITTEXT,PULSE_CUSTOM_EDIT_MAX,0);
            ShowWindow(custom_edits_[i],SW_HIDE);
        }
        if (path_edit_) ShowWindow(path_edit_, SW_HIDE);
        if (search_edit_) ShowWindow(search_edit_, SW_HIDE);
    }

    // Keeps only the first (root-cause) failure; later failures are consequences.
    void NoteFailure(DWORD error, const char* stage,
                     std::source_location site = std::source_location::current()) noexcept {
        if (failure_.stage) return;
        failure_.stage = stage ? stage : "other";
        failure_.line = static_cast<uint32_t>(site.line());
        failure_.error = error;
        failure_.hr = failure_hr_;
        failure_.rename_edit_error = rename_edit_ ? PulsePaintedChildEditError(rename_edit_) : ERROR_SUCCESS;
        failure_.filename_edit_error = filename_edit_ ? PulsePaintedChildEditError(filename_edit_) : ERROR_SUCCESS;
        failure_.painted_frame = painted_frame_;
        failure_.first_present_occluded = first_present_occluded_;
        failure_.submit_ready = submit_ready_;
        failure_.rendering_frame = rendering_frame_;
        failure_.hidden = HiddenFromUser();
    }
    void FailPaintedEdit(DWORD error, const char* stage = "other",
                         std::source_location site = std::source_location::current()) {
#ifdef PULSE_WITH_TEST_HOOKS
        std::fprintf(stderr, "[PICKER-EDIT-FAIL] stage=%s occluded=%d error=%lu rename_error=%lu filename_error=%lu focus_id=%d rename_active=%d rendering=%d frame=%llu\n",
            stage, first_present_occluded_, error, rename_edit_ ? PulsePaintedChildEditError(rename_edit_) : 0,
            filename_edit_ ? PulsePaintedChildEditError(filename_edit_) : 0, GetDlgCtrlID(GetFocus()),
            !rename_path_.empty(), rendering_frame_, static_cast<unsigned long long>(painted_frame_));
        std::fflush(stderr);
#else
        (void)stage;
#endif
        native_error_ = error ? error : ERROR_FUNCTION_FAILED;
        NoteFailure(native_error_, stage, site);
        result_.clear(); done_ = true;
        if (hwnd_) PostMessageW(hwnd_, WM_NULL, 0, 0);
    }
    bool CheckPaintedEdits() {
        if (!spec_.pulse_painted_plain_child) return true;
        if (!compositor_.CustomEditEnabled()) { FailPaintedEdit(ERROR_NOT_SUPPORTED, "edit-check-disabled"); return false; }
        for (HWND edit : AllEdits()) {
            const DWORD error = edit ? PulsePaintedChildEditError(edit) : ERROR_SUCCESS;
            if (error) { FailPaintedEdit(error, "edit-check"); return false; }
        }
        return !done_;
    }
    void PresentEdits() {
        if (!CheckPaintedEdits()) return;
        for (HWND edit : AllEdits()) {
            if (edit && IsWindowVisible(edit) &&
                !PresentChildEdit(compositor_, EditFormat(edit), EditForeground(), EditBackground(), edit) &&
                spec_.pulse_painted_plain_child) { FailPaintedEdit(PulsePaintedChildEditError(edit), "present-edit"); return; }
        }
    }

    // Diagnostics only (FilePickerFailure::hidden): 1 iconic, 2 owner iconic,
    // 4 cloaked by the shell or through the owner (other virtual desktop),
    // 8 not visible, 16 cloaked by an application.
    uint32_t HiddenFromUser() const noexcept {
        if (!hwnd_) return 0;
        uint32_t bits = 0;
        if (IsIconic(hwnd_)) bits |= 1;
        if (owner_ && IsIconic(owner_)) bits |= 2;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd_, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
            if (cloaked & (DWM_CLOAKED_SHELL | DWM_CLOAKED_INHERITED)) bits |= 4;
            if (cloaked & DWM_CLOAKED_APP) bits |= 16;
        }
        if (!IsWindowVisible(hwnd_)) bits |= 8;
        return bits;
    }
    bool FlushPaintedEdits() {
        if (!spec_.pulse_painted_plain_child) return true;
        if(first_present_retrying_)return false;
        if (rendering_frame_) { FailPaintedEdit(ERROR_BUSY, "flush-busy"); return false; }
        if (!CheckPaintedEdits()) return false;
        const auto previous = painted_frame_;
        PresentEdits();
        if (!done_) {
            // Multi-selection chips hide the filename editor. With no visible
            // edits, PresentEdits cannot invalidate the parent, but this flush
            // still requires a newly presented frame before accepting paths.
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
        // A newly shown HWND can remain DXGI-occluded during its first DWM
        // transition. Readiness still requires an actually presented frame.
        struct FirstPresentRetryScope {
            bool& flag;explicit FirstPresentRetryScope(bool& value):flag(value){flag=true;}
            ~FirstPresentRetryScope(){flag=false;}
        } retry_scope(first_present_retrying_);
        const auto deadline=GetTickCount64()+1000;
        while(!done_ && previous==0 && painted_frame_==0 && first_present_occluded_ && GetTickCount64()<deadline) {
            if((spec_.cancel_event && WaitForSingleObject(spec_.cancel_event,0)==WAIT_OBJECT_0) ||
               (spec_.owner_guard && !spec_.owner_guard->Alive())) {done_=true;return false;}
            MsgWaitForMultipleObjectsEx(spec_.cancel_event?1:0,&spec_.cancel_event,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            MSG pending{};
            for(unsigned count=0;count<16 && PeekMessageW(&pending,nullptr,0,0,PM_REMOVE);++count) {
                if(pending.message==WM_QUIT){PostQuitMessage(static_cast<int>(pending.wParam));done_=true;return false;}
                TranslateMessage(&pending);DispatchMessageW(&pending);
                if(done_)return false;
            }
            if(!done_ && painted_frame_==0){InvalidateRect(hwnd_,nullptr,FALSE);UpdateWindow(hwnd_);}
        }
        if (!CheckPaintedEdits()) return false;
        if (painted_frame_ == previous) { FailPaintedEdit(ERROR_FUNCTION_FAILED, "flush-no-present"); return false; }
        return true;
    }
    void DrawPaintedEdits() {
        if (!spec_.pulse_painted_plain_child || !CheckPaintedEdits()) return;
        for (HWND edit : AllEdits()) {
            const bool visible = edit && (preparing_first_frame_
                ? (GetWindowLongPtrW(edit, GWL_STYLE) & WS_VISIBLE) != 0
                : IsWindowVisible(edit) != FALSE);
            if (!visible) continue;
            RECT rect{}; GetClientRect(edit, &rect);
            MapWindowPoints(edit, hwnd_, reinterpret_cast<POINT*>(&rect), 2);
            const auto bounds = D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                static_cast<float>(rect.right), static_cast<float>(rect.bottom));
            if (!DrawPulsePaintedChildEdit(compositor_, EditFormat(edit), EditForeground(), EditBackground(), edit, bounds)) {
                FailPaintedEdit(PulsePaintedChildEditError(edit), "present-edit"); return;
            }
        }
    }

    void PlaceOneEdit(HWND edit, const D2D1_RECT_F& cell, bool visible, float inset_dip = 10.0f) {
        if (!edit || !hwnd_) return;
        if (!visible || cell.right - cell.left < 24.0f * scale_) {
            if (IsWindowVisible(edit)) ShowWindow(edit, SW_HIDE);
            return;
        }
        const int x = static_cast<int>(std::lround(cell.left + inset_dip * scale_));
        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left - 2.0f * inset_dip * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        int line_h = cell_h;
        if (font_) {
            HDC hdc = GetDC(edit);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, font_));
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, old);
            ReleaseDC(edit, hdc);
            line_h = std::max(1, static_cast<int>(tm.tmHeight));
        }
        line_h = std::min(line_h, cell_h);
        const int y = static_cast<int>(std::lround(cell.top)) + std::max(0, (cell_h - line_h) / 2);
        SetWindowPos(edit, HWND_TOP, x, y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void PlaceEdits() {
        if (!compositor_.Dc()) return;
        const float w = RendererWidth();
        PlaceOneEdit(path_edit_, renderer_.AddressBarRect(w), address_editing_ && !searching_);
        PlaceOneEdit(search_edit_, LayoutAddressSearch(renderer_.SearchBarRect(w), scale_).input,
                     searching_, 0.0f);
        PlaceOneEdit(filename_edit_, chrome_layout_.filename, chrome_.selected_names.empty());
        PlaceCustomEdits();
        PlaceRenameEdit();
    }

    void PlaceCustomEdits() {
        for(size_t i=0;i<custom_edits_.size();++i)if(custom_edits_[i]) {
            const auto rect=CustomRect(i),panel=CustomPanel();
            const auto state=EffectiveControl(i);
            EnableWindow(custom_edits_[i],(state&1)!=0);
            PlaceOneEdit(custom_edits_[i],rect,(state&2)!=0 && rect.top>=panel.top && rect.bottom<=panel.bottom);
        }
    }
    void BeginAddressEdit() {
        if (!path_edit_) return;
        if (searching_) ExitSearch();
        address_editing_ = true;
        setting_edits_ = true;
        SetWindowTextW(path_edit_, DisplayPath(current_).c_str());
        setting_edits_ = false;
        PlaceEdits();
        SetFocus(path_edit_);
        SendMessageW(path_edit_, EM_SETSEL, 0, -1);
        Invalidate();
    }

    void EndAddressEdit(bool refocus_list) {
        CloseJump();
        if (!address_editing_) return;
        address_editing_ = false;
        PlaceEdits();
        if (refocus_list) FocusList();
        Invalidate();
    }

    void SubmitPath() {
        const auto text = ReadEdit(path_edit_);
        EndAddressEdit(true);
        if (text == l10n::Get(l10n::StringId::ThisPc) || text.empty()) {
            Navigate(L"", true);
            return;
        }
        if (!loader_) return;
        const auto archive_path = NormalizePickerInput(text);
        if (options_.archive && options_.archive->Contains(archive_path)) {
            Navigate(archive_path, true);
            return;
        }
        validation_navigation_only_ = true;
        validation_incoming_files_ = false;
        validation_add_selection_ = false;
        validating_ = true;
        chrome_.notice = l10n::Get(l10n::StringId::CheckingThePath).c_str();
        PickerOptions all;
        all.no_dereference_links = options_.no_dereference_links;
        loader_->Validate(++generation_, current_, {text}, PickerMode::File, all);
        UpdateChosen();
        Invalidate();
    }

    void BeginSearch() {
        if (!search_edit_) return;
        EndAddressEdit(false);
        searching_ = true;
        PlaceEdits();
        SetFocus(search_edit_);
        SendMessageW(search_edit_, EM_SETSEL, 0, -1);
        Invalidate();
    }

    void ExitSearch() {
        if (!searching_) return;
        KillTimer(hwnd_, kSearchTimer);
        searching_ = false;
        search_text_.clear();
        setting_edits_ = true;
        if (search_edit_) SetWindowTextW(search_edit_, L"");
        setting_edits_ = false;
        PlaceEdits();
        FocusList();
        if (!options_.search.empty()) {
            options_.search.clear();
            Reload();
        }
        Invalidate();
    }

    void ApplySearchNow() {
        KillTimer(hwnd_, kSearchTimer);
        if (options_.search == search_text_) return;
        options_.search = search_text_;
        Reload();
    }

    void FocusList() {
        focus_ = kPickList;
        SetFocus(hwnd_);
        Invalidate();
    }

    void FocusFilename() {
        if (!filename_edit_) return;
        editing_selection_names_ = true; chrome_.selected_names.clear(); PlaceEdits();
        SetFocus(filename_edit_);
        SendMessageW(filename_edit_, EM_SETSEL, 0, -1);
    }

    void InputChanged(HWND edit) {
        if (edit == rename_edit_) { PlaceRenameEdit(); Invalidate(); return; }
        if (setting_edits_) return;
        const int custom_index=GetDlgCtrlID(edit)-kCustomEditBase;
        if(custom_index>=0 && static_cast<size_t>(custom_index)<custom_edits_.size() && custom_edits_[custom_index]==edit) {
            const size_t index=static_cast<size_t>(custom_index);
            if(EffectiveControl(index)!=3)return;
            auto next=spec_.custom_controls[index];next.text=ReadEdit(edit);
            if(next.text.size()>PULSE_CUSTOM_EDIT_MAX){FailPaintedEdit(ERROR_INVALID_DATA);return;}
            if(UpdateCustom(next)!=S_OK || done_)return;
            embedded_state_.control_id=next.id;embedded_state_.control_text=next.text;
            PickerProofPhase("custom-edit-notify-before");
            const auto hr=NotifyEmbedded(PickerDialogEvent::EditChanged);
            PickerProofPhase("custom-edit-notify-after");
            embedded_state_.control_id=0;embedded_state_.control_text.clear();
            if(hr!=S_OK && !done_)FailPaintedEdit(ERROR_FUNCTION_FAILED);
            return;
        }
        chrome_.pressed = kPickNone;
        if (validating_) { ++generation_; validating_ = false; }
        chrome_.notice.clear();
        if (edit == filename_edit_) {
            user_interacted_ = true;
            filename_text_ = ReadEdit(edit);
            save_format_notice_.clear(); save_format_notice_filename_.clear();
            filename_from_selection_ = false;
            selected_set_.clear();
            selected_ = -1;
            anchor_ = -1;
            if (mode_ == PickerMode::Save && !syncing_filename_filter_ && !filename_ime_composing_ && !embedded_callback_depth_) {
                if (const auto inferred = InferSaveFilterFromFilename(filename_text_, options_.filters, options_.filter_index)) {
                    syncing_filename_filter_ = true;
                    struct Reset { bool& value; ~Reset() { value = false; } } reset{syncing_filename_filter_};
                    // Reload invalidates the old validation/listing generation and
                    // publishes TypeChanged through the same path as the dropdown.
                    const auto leaf = PickerLeafName(filename_text_);
                    const auto dot = leaf.find_last_of(L'.');
                    const auto labels = PickerFormatLabels(options_.filters);
                    if (dot != leaf.npos && *inferred < labels.size()) {
                        save_format_notice_ = l10n::Get(l10n::StringId::PickerFormatInferred);
                        const auto replace = [&](const wchar_t* key, const std::wstring& value) {
                            const auto at = save_format_notice_.find(key);
                            if (at != save_format_notice_.npos) save_format_notice_.replace(at, wcslen(key), value);
                        };
                        replace(L"{ext}", leaf.substr(dot)); replace(L"{format}", labels[*inferred]);
                        save_format_notice_filename_ = filename_text_; save_format_notice_filter_ = *inferred;
                    }
                    ApplyFilter(*inferred);
                    return;
                }
            }
        } else if (edit == search_edit_) {
            search_text_ = ReadEdit(edit);
            SetTimer(hwnd_, kSearchTimer, 180, nullptr);
        } else if (edit == path_edit_) {
            UpdateJump();
        }
        UpdateChosen();
        Invalidate();
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_IME_STARTCOMPOSITION:
            if (hwnd == self->filename_edit_) self->filename_ime_composing_ = true;
            break;
        case WM_IME_ENDCOMPOSITION:
            if (hwnd == self->filename_edit_) {
                const auto result = DefSubclassProc(hwnd, msg, wparam, lparam);
                self->filename_ime_composing_ = false;
                self->InputChanged(hwnd);
                return result;
            }
            break;
        case WM_KEYDOWN:
            self->jump_context_touched_ = true;
            if (hwnd == self->rename_edit_) {
                if (wparam == VK_RETURN || wparam == VK_ESCAPE) { self->EndRename(wparam == VK_RETURN); return 0; }
            }
            self->DismissHover();
            if (hwnd == self->path_edit_ && self->JumpKey(wparam)) return 0;
            if (self->HandleViewKey(wparam)) return 0;
            if (wparam == VK_RETURN) {
                if (hwnd == self->path_edit_) self->SubmitPath();
                else if (hwnd == self->filename_edit_) self->SubmitSelection();
                else if(hwnd==self->search_edit_) { self->ApplySearchNow(); self->FocusList(); }
                else self->SubmitSelection();
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                if (self->CloseCustomDetails()) return 0;
                self->Cancel();
                return 0;
            }
            if (wparam == VK_TAB) { self->MoveFocus(KeyDown(VK_SHIFT) ? -1 : 1); return 0; }
            if (KeyDown(VK_CONTROL) && wparam == 'L') { self->BeginAddressEdit(); return 0; }
            if (KeyDown(VK_CONTROL) && wparam == 'F') { self->BeginSearch(); return 0; }
            if (KeyDown(VK_CONTROL) && (wparam == 'K' || wparam == 'E')) { self->BeginSearch(); return 0; }
            if (wparam == VK_DOWN && hwnd == self->search_edit_) {
                self->FocusList();
                if (!self->entries_.empty()) self->Select(std::max(0, self->selected_));
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            return SendMessageW(self->hwnd_, msg, wparam, lparam);
        case WM_SYSKEYDOWN:
            if (wparam == VK_LEFT) { self->GoBack(); return 0; }
            if (wparam == VK_RIGHT) { self->GoForward(); return 0; }
            if (wparam == VK_UP) { self->GoUp(); return 0; }
            if (wparam == '0') { self->UsePulseSelection(); return 0; }
            if (wparam == 'D') { self->BeginAddressEdit(); return 0; }
            if (wparam == 'N' && self->filename_edit_) { self->FocusFilename(); return 0; }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        case WM_SETFOCUS:
            if (hwnd == self->rename_edit_) { self->focus_ = kPickList; break; }
            PickerProofFocus("edit-setfocus", self->focus_, GetDlgCtrlID(hwnd));
            self->focus_ = GetDlgCtrlID(hwnd)>=kCustomEditBase ? kCustomFocusBase+GetDlgCtrlID(hwnd)-kCustomEditBase : hwnd == self->path_edit_ ? kPickAddress
                         : hwnd == self->filename_edit_ ? kPickFilename : kPickSearch;
            break;
        case WM_KILLFOCUS:
            if (hwnd == self->rename_edit_ && !self->rename_path_.empty())
                PostMessageW(self->hwnd_, kEndRenameMessage, self->rename_serial_, 0);
            self->DismissHover();
            // Clicking away from the path field ends the edit, like the main
            // window's address bar. Posted: the edit is mid-message here.
            if (hwnd == self->path_edit_ && self->address_editing_)
                PostMessageW(self->hwnd_, kEndAddressMessage, 0, 0);
            break;
        }
        const bool repaint = msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                             msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        if(msg==WM_CHAR)PickerProofPhase("edit-char-dispatch-before");
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->EditFormat(hwnd),
                                    self->EditForeground(), self->EditBackground(),
                                    self->EditBrush(), hwnd, msg, wparam, lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->EditFormat(hwnd),
                                               self->EditForeground(), self->EditBackground(),
                                               hwnd, msg, wparam, lparam);
        }
        if(msg==WM_CHAR)PickerProofPhase("edit-char-dispatch-after");
        if (repaint && self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    // ---- layout and painting ----------------------------------------------

    void ApplyRendererSettings() {
        SyncCustomOptions();
        vm_.layout_controls.visible = false;
        renderer_.SetResourceModule(spec_.module_instance);
        renderer_.SetEmbeddedPickerLayout(true, kPickerStripDip);
        renderer_.SetEmbeddedPickerPreviewForced((spec_.native_options & FOS_FORCEPREVIEWPANEON) != 0);
        renderer_.SetEmbeddedPickerPreviewMinDip(mode_ == PickerMode::Save ? 600.0f : 760.0f);
        if (mode_ == PickerMode::Save) renderer_.SetDetailsPanelWidth(256.0f);
        renderer_.SetEmbedded(kPickerTitleDip, PickerFooterDip(mode_, chrome_.overwrite_pending || chrome_.live_conflict, !chrome_.options.empty(), !chrome_.formats.empty(), (!chrome_.save_suggestions.empty() || !chrome_.format_note.empty())) + CustomHeightDip());
        renderer_.SetItemClick(false, 0, allow_multiselect_);
        renderer_.SetDetailsPanelVisible(PreviewPaneShown());
        renderer_.SetSidebarWidthDip(layout_settings_.sidebar_width);
        if (spec_.row_height_dip > 0.0f) renderer_.SetRowHeightDip(spec_.row_height_dip);
        renderer_.SetListStyle(spec_.list_smart_date, spec_.list_zebra_rows, spec_.list_size_bar,
                               false, spec_.selection_outline);
        if (spec_.explicit_details_columns || spec_.details_columns) renderer_.SetDetailsColumns(spec_.details_columns);
        renderer_.SetThumbnailBadges(spec_.thumbnail_badges);
        renderer_.SetRowActions(0);
    }

    bool ProminentControl(size_t index) const {
        const auto& control = spec_.custom_controls[index];
        if (control.prominent) return true;
        if (control.has_parent) for (const auto& parent : spec_.custom_controls)
            if (parent.id == control.parent_id) return parent.prominent;
        return false;
    }
    // Hosts use AddText(" ") as a placeholder and can give it a real label later.
    // Keep the original label/ID for callbacks; this is a layout decision only.
    static bool HasCustomCaption(const PickerCustomControl& control) {
        return control.label.find_first_not_of(L' ') != std::wstring::npos;
    }
    const PickerCustomControl* AdjacentComboCaption(size_t index) const {
        if (!index || index >= spec_.custom_controls.size()) return nullptr;
        const auto& combo = spec_.custom_controls[index];
        const auto& text = spec_.custom_controls[index - 1];
        if (combo.kind != 3 || !combo.label.empty() || combo.property_layout ||
            text.kind != kPickerCustomText || text.property_layout || !HasCustomCaption(text) ||
            text.has_parent != combo.has_parent || (combo.has_parent && text.parent_id != combo.parent_id) ||
            !(EffectiveControl(index - 1) & 2) || !(EffectiveControl(index) & 2)) return nullptr;
        return &text;
    }
    const PickerCustomItem* SelectedComboItem(const PickerCustomControl& control) const {
        if (control.kind == 3 && control.has_selected)
            for (const auto& item : control.items)
                if (item.id == control.selected_item_id) return &item;
        return nullptr;
    }
    bool CompactCustom() const {
        if (custom_details_open_ || PickerPropertiesNeedPanel(spec_.custom_controls)) return false;
        bool any = false;
        size_t non_property_interactive = 0;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            const auto& control = spec_.custom_controls[i];
            // Only collapse a standalone text row if its caption is actually carried by the adjacent chip.
            if (control.kind == kPickerCustomText && !control.property_layout && HasCustomCaption(control) &&
                (EffectiveControl(i) & 2) && !AdjacentComboCaption(i + 1)) return false;
            if (!(EffectiveControl(i) & 2) || control.kind == 5 || control.kind == 1 || control.kind == kPickerCustomSeparator || control.kind == kPickerCustomText) continue;
            any = true;
            if (!control.property_layout && ++non_property_interactive > 4) return false;
        }
        return any;
    }
    void SyncCustomOptions() {
        chrome_.options.clear();
        chrome_.footer_tool = {};
        chrome_.more_options_text = l10n::Get(l10n::StringId::AdvMore);
        if (!CompactCustom()) return;
        // File properties (HTML 作者 | SS): one joined chip per field. List values merge into
        // the first value's chip; an empty list shows its add button as "添加X". The inline
        // remove buttons stay in the details panel.
        const auto field_label = [&](uint32_t parent) {
            for (const auto& group : spec_.custom_controls) if (group.kind == 1 && group.id == parent) return group.label;
            return std::wstring();
        };
        const auto field_chip = [&](uint32_t parent) -> PickerOptionChip* {
            for (auto& chip : chrome_.options) {
                if (!chip.property || chip.id < kCustomFocusBase) continue;
                const auto& owner = spec_.custom_controls[static_cast<size_t>(chip.id - kCustomFocusBase)];
                if (owner.has_parent && owner.parent_id == parent) return &chip;
            }
            return nullptr;
        };
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            const auto& control = spec_.custom_controls[i];
            const auto state = EffectiveControl(i);
            if (control.kind == 5 || control.kind == 1 || control.kind == kPickerCustomSeparator || control.kind == kPickerCustomText || !(state & 2)) continue;
            if (control.property_layout == 2) continue;
            if (control.property_layout == 1 || control.property_layout == 3 || control.property_layout == 4) {
                const bool add = control.property_layout == 3;
                if (auto* existing = control.has_parent ? field_chip(control.parent_id) : nullptr) {
                    if (!add && !control.text.empty()) {
                        if (existing->add) { existing->id = kCustomFocusBase + static_cast<int>(i); existing->text.clear(); existing->add = false; }
                        existing->text += (existing->text.empty() ? L"" : L", ") + control.text;
                    }
                    continue;
                }
                PickerOptionChip chip;
                chip.id = kCustomFocusBase + static_cast<int>(i);
                chip.property = true;
                chip.enabled = (state & 1) != 0;
                chip.label = control.has_parent ? field_label(control.parent_id) : control.label;
                if (chip.label.empty()) chip.label = control.label;
                chip.add = add || control.text.empty();
                chip.text = chip.add ? FillPickerText(l10n::Get(l10n::StringId::PickerPropertyAdd), chip.label) : control.text;
                chrome_.options.push_back(std::move(chip));
                continue;
            }
            auto caption = FormatCustomControlCaption(control.label);
            std::wstring label;
            if (control.kind == 8 && !control.text.empty()) {
                label = caption; caption = control.text;
            }
            if (control.kind == 3) {
                if (const auto* text = AdjacentComboCaption(i)) label = FormatCustomControlCaption(text->label);
                if (const auto* item = SelectedComboItem(control)) {
                    if (label.empty()) label = FormatCustomControlCaption(control.label);
                    caption = FormatCustomControlCaption(item->label);
                }
            }
            PickerOptionChip chip;
            chip.id = kCustomFocusBase + static_cast<int>(i); chip.text = std::move(caption);
            chip.checkable = control.kind == 7; chip.checked = control.checked; chip.enabled = (state & 1) != 0;
            chip.drop_down = control.kind == 3 || control.kind == 4 || control.kind == 6 || control.kind == 8;
            chip.label = std::move(label);
            if (mode_ == PickerMode::Save && control.kind == 4 && !chrome_.footer_tool.id)
                chrome_.footer_tool = chip;
            chrome_.options.push_back(std::move(chip));
        }
        if (chrome_.footer_tool.id && compositor_.Dc()) {
            const auto layout = LayoutPickerChrome(Width(), Height(), chrome_, painter_, scale_);
            if (layout.footer_tool.right > layout.footer_tool.left)
                std::erase_if(chrome_.options, [&](const auto& chip) { return chip.id == chrome_.footer_tool.id; });
        }
    }
    void ShowMoreOptions() {
        std::vector<FluentMenuItem> items;
        for (size_t i = chrome_layout_.option_chips.size(); i < chrome_.options.size(); ++i) {
            const auto& option = chrome_.options[i];
            auto caption = option.text;
            if (!option.label.empty() && !option.add) {
                const auto last = option.label.back();
                caption = option.label;
                if (!option.text.empty()) {
                    caption += last == L':' || last == L'：' ? L" " : L": ";
                    caption += option.text;
                }
            }
            auto item = Item(option.id, caption.c_str());
            item.enabled = option.enabled; item.toggle = option.checkable; item.checked = option.checked;
            items.push_back(std::move(item));
        }
        if (items.empty()) return;
        const auto generation = embedded_state_.generation;
        menu_.SetTheme(dark_, accent_);
        const int chosen = TrackPickerDropdown(ScreenRect(chrome_layout_.more_options), std::move(items));
        if (!done_ && generation == embedded_state_.generation && chosen >= kCustomFocusBase) Activate(chosen);
    }
    size_t CompactCustomCount() const {
        size_t count = 0;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i)
            if (spec_.custom_controls[i].kind != 5 && spec_.custom_controls[i].kind != kPickerCustomText && (EffectiveControl(i) & 2)) ++count;
        return count;
    }
    size_t CompactCustomColumns() const { return Width() / std::max(0.1f, scale_) >= 760 ? 2 : 1; }
    bool HasPropertyLayoutControls() const {
        return std::any_of(spec_.custom_controls.begin(), spec_.custom_controls.end(),
            [](const auto& control) { return control.property_layout != 0; });
    }
    bool UseFlowCustomLayout() const {
        return !spec_.custom_controls.empty() && !PropertySidebar() && !CompactCustom() && !HasPropertyLayoutControls();
    }
    void RebuildCustomFlow() {
        custom_flow_ = {};
        if (!UseFlowCustomLayout() || !compositor_.Dc()) return;
        const float s = std::max(0.1f, scale_);
        const float width_dip = Width() / s;
        const float height_dip = Height() / s;
        const float max_viewport = std::clamp(height_dip * 0.34f, 112.0f, 220.0f);
        const auto measure_text_dip = [&](std::wstring_view text) {
            if (text.empty()) return 0.0f;
            return typography::MeasureLine(&compositor_, compositor_.TextFormat(), text) / s;
        };
        std::vector<PickerCustomFlowItem> items;
        items.reserve(spec_.custom_controls.size());
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            const auto& control = spec_.custom_controls[i];
            PickerCustomFlowItem item;
            item.kind = control.kind;
            item.id = control.id;
            item.visible = control.kind != 5 && (EffectiveControl(i) & 2) != 0;
            item.prominent = ProminentControl(i);
            item.has_parent = control.has_parent;
            item.parent_id = control.parent_id;
            const auto display = FormatCustomControlCaption(control.label);
            item.has_caption = HasCustomCaption(control) && !display.empty();
            item.inline_colon_label = (control.kind == 1 || control.kind == kPickerCustomText) &&
                item.has_caption && IsInlineColonCaption(display);
            if (control.kind == 1 || control.kind == kPickerCustomText) {
                item.natural_w = measure_text_dip(display) + 4.0f;
            } else if (control.kind == 2) {
                item.natural_w = std::max(84.0f, painter_.MeasureButtonWidth(display) / s + 6.0f);
            } else if (control.kind == 4) {
                item.natural_w = std::max(96.0f, painter_.MeasureButtonWidth(display, {}, true) / s + 6.0f);
            } else if (control.kind == 3) {
                float btn_w = 132.0f;
                if (const auto* selected = SelectedComboItem(control)) {
                    btn_w = std::max(btn_w, painter_.MeasureButtonWidth(FormatCustomControlCaption(selected->label), {}, true) / s + 6.0f);
                } else {
                    btn_w = std::max(btn_w, painter_.MeasureButtonWidth(display, {}, true) / s + 6.0f);
                }
                for (const auto& choice : control.items) {
                    if (!(choice.state & 2)) continue;
                    btn_w = std::max(btn_w, painter_.MeasureButtonWidth(FormatCustomControlCaption(choice.label), {}, true) / s + 6.0f);
                }
                btn_w = std::min(btn_w, 260.0f);
                const float label_w = (!display.empty() && SelectedComboItem(control)) ? (std::min(measure_text_dip(display), 160.0f) + 8.0f) : 0.0f;
                item.natural_w = label_w + btn_w;
            } else if (control.kind == 7) {
                item.natural_w = 26.0f + measure_text_dip(display) + 8.0f;
            } else if (control.kind == 8) {
                item.natural_w = 180.0f;
            } else if (control.kind == 6) {
                item.radio_item_widths.reserve(control.items.size());
                item.segment_item_widths.reserve(control.items.size());
                for (const auto& choice : control.items) {
                    if (choice.state & 2) {
                        const float text_w = measure_text_dip(FormatCustomControlCaption(choice.label));
                        item.radio_item_widths.push_back(28.0f + text_w + 8.0f);
                        item.segment_item_widths.push_back(std::max(48.0f, text_w + 24.0f));
                    } else {
                        item.radio_item_widths.push_back(-1.0f);
                        item.segment_item_widths.push_back(-1.0f);
                    }
                }
            }
            items.push_back(std::move(item));
        }
        custom_flow_ = LayoutPickerCustomFlow(width_dip, max_viewport, items);
    }
    PickerCustomSections CustomSections() const {
        if (CompactCustom()) return {0, 8.0f + 40.0f * static_cast<float>((CompactCustomCount() + CompactCustomColumns() - 1) / CompactCustomColumns())};
        if (UseFlowCustomLayout()) return {0.0f, custom_flow_.content_height};
        float normal = 0, prominent = 0;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            if (spec_.custom_controls[i].kind == 5 || !(EffectiveControl(i) & 2)) continue;
            (ProminentControl(i) ? prominent : normal) += CustomRowHeight(i);
        }
        return LayoutPickerCustomSections(normal, prominent, CustomHeightDip());
    }
    float CustomHeightDip() const {
        if (PropertySidebar()) return 0.0f;
        if (CompactCustom()) return 0.0f;
        if (UseFlowCustomLayout()) return custom_flow_.viewport_height;
        bool prominent = false, separators = false, any_height = false;
        float content = 20.0f;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            if (spec_.custom_controls[i].kind == 5 || !(EffectiveControl(i) & 2)) continue;
            const float row_h = CustomRowHeight(i);
            if (row_h <= 0.0f) continue;
            any_height = true;
            prominent |= ProminentControl(i);
            separators |= spec_.custom_controls[i].kind == kPickerCustomSeparator ||
                          spec_.custom_controls[i].kind == kPickerCustomText;
            content += row_h;
        }
        if (prominent || separators) return std::clamp(content, 120.0f, 220.0f);
        for (const auto& control : spec_.custom_controls) if (control.property_layout)
            return std::clamp(Height() / std::max(0.1f, scale_) - 260.0f, 120.0f, 220.0f);
        if (any_height) return 120.0f;
        return 0.0f;
    }
    void SyncOpenDropDown() {
        if (chrome_.overwrite_pending) { chrome_.primary_drop_down = false; return; }
        chrome_.primary_drop_down = false;
        for (size_t i=0;i<spec_.custom_controls.size();++i) {
            const auto& control=spec_.custom_controls[i];
            if(control.kind!=5) continue;
            chrome_.primary_drop_down=(EffectiveControl(i)&2)!=0;
            chrome_.primary_enabled=chrome_.primary_enabled && (EffectiveControl(i)&1)!=0;
            if(!control.items.empty()) chrome_.primary_text=control.items.front().label;
        }
    }
    uint32_t EffectiveControl(size_t index) const {
        const auto& item = spec_.custom_controls[index];
        uint32_t state = item.state;
        // Native MakeProminent lifts a child out of its group's effective state,
        // while keeping the recorded parent identity stable for live updates.
        if (item.has_parent && !item.prominent) for (const auto& parent : spec_.custom_controls)
            if (parent.id == item.parent_id) { state &= parent.state; break; }
        return state;
    }
    D2D1_RECT_F CustomPanel() const {
        if (PropertySidebar()) return D2D1::RectF(RendererWidth() + 8 * scale_, (kPickerTitleDip + 8) * scale_, Width() - 8 * scale_, chrome_layout_.footer.top - 8 * scale_);
        return D2D1::RectF(0, chrome_layout_.footer.top - CustomHeightDip() * scale_, Width(), chrome_layout_.footer.top);
    }
    float CustomRowHeight(size_t index) const {
        const auto& control=spec_.custom_controls[index];
        if(control.property_layout==2)return 0.0f;
        if(control.kind==1 || control.kind==kPickerCustomText)return HasCustomCaption(control) ? 26.0f : 0.0f;
        if(control.kind==kPickerCustomSeparator)return 18.0f;
        if(control.kind==6) {
            size_t count=0;for(const auto& item:control.items)if(item.state&2)++count;
            return static_cast<float>(count)*30.0f;
        }
        return 38.0f;
    }
    D2D1_RECT_F RadioRect(size_t index,size_t item_index) const {
        if (UseFlowCustomLayout() && index < custom_flow_.radio_boxes.size() && item_index < custom_flow_.radio_boxes[index].size()) {
            const auto& box = custom_flow_.radio_boxes[index][item_index];
            if (!box.visible) return D2D1::RectF(0, 0, 0, 0);
            const auto panel = CustomPanel();
            return D2D1::RectF(panel.left + box.left * scale_, panel.top + box.top * scale_ - custom_scroll_,
                panel.left + box.right * scale_, panel.top + box.bottom * scale_ - custom_scroll_);
        }
        auto rect=CustomRect(index);size_t row=0;
        for(size_t i=0;i<item_index;++i)if(spec_.custom_controls[index].items[i].state&2)++row;
        rect.top+=static_cast<float>(row)*30.0f*scale_;rect.bottom=rect.top+28.0f*scale_;return rect;
    }
    D2D1_RECT_F CustomRect(size_t index) const {
        if (chrome_.footer_tool.id == kCustomFocusBase + static_cast<int>(index) &&
            chrome_layout_.footer_tool.right > chrome_layout_.footer_tool.left) return chrome_layout_.footer_tool;
        if (CompactCustom()) {
            for (size_t i = 0; i < chrome_layout_.option_chips.size() && i < chrome_.options.size(); ++i)
                if (chrome_.options[i].id == kCustomFocusBase + static_cast<int>(index)) return chrome_layout_.option_chips[i];
            return chrome_layout_.more_options;
        }
        if (UseFlowCustomLayout()) {
            if (index >= custom_flow_.boxes.size() || !custom_flow_.boxes[index].visible) return D2D1::RectF(0, 0, 0, 0);
            const auto& box = custom_flow_.boxes[index];
            const auto panel = CustomPanel();
            return D2D1::RectF(panel.left + box.left * scale_, panel.top + box.top * scale_ - custom_scroll_,
                panel.left + box.right * scale_, panel.top + box.bottom * scale_ - custom_scroll_);
        }
        if (PropertySidebar()) {
            const auto panel = CustomPanel();
            float y = panel.top + 8 * scale_ - custom_scroll_;
            for (size_t i = 0; i < index; ++i) if (spec_.custom_controls[i].kind != 5 && (EffectiveControl(i) & 2)) y += CustomRowHeight(i) * scale_;
            const auto layout = spec_.custom_controls[index].property_layout;
            if (layout == 2) return D2D1::RectF(panel.right - 46 * scale_, y - 38 * scale_, panel.right - 12 * scale_, y - 2 * scale_);
            return D2D1::RectF(panel.left + 12 * scale_, y, layout == 3 ? panel.left + 46 * scale_ : panel.right - (layout == 1 ? 52 : 12) * scale_, y + std::max(0.0f, CustomRowHeight(index) - 2) * scale_);
        }
        const bool prominent = ProminentControl(index);
        float y = CustomPanel().top + (prominent ? CustomSections().prominent_top : 4.0f) * scale_ - custom_scroll_;
        for (size_t i = 0; i < index; ++i) if (spec_.custom_controls[i].kind != 5 && (EffectiveControl(i) & 2) && ProminentControl(i) == prominent)
            y += CustomRowHeight(i) * scale_;
        const auto layout = spec_.custom_controls[index].property_layout;
        if (layout == 2) return D2D1::RectF(Width() - 62 * scale_, y - 38 * scale_, Width() - 28 * scale_, y - 2 * scale_);
        if (layout == 3) return D2D1::RectF(16 * scale_, y, 50 * scale_, y + 36 * scale_);
        if (prominent) return D2D1::RectF(PickerProminentLeft(Width() / scale_) * scale_, y,
            std::max(16.0f, Width() / scale_ - 28.0f) * scale_, y + std::max(0.0f, CustomRowHeight(index) - 2.0f) * scale_);
        return D2D1::RectF(16 * scale_, y, Width() - (layout == 1 ? 68 : layout ? 28 : 16) * scale_,
            y + std::max(0.0f,CustomRowHeight(index)-2.0f) * scale_);
    }
    fluent::ScrollbarSpec CustomScrollbar() const {
        fluent::ScrollbarSpec bar;
        bar.viewport = CustomPanel(); bar.viewport.left = bar.viewport.right - 14 * scale_;
        bar.offset = custom_scroll_; bar.viewport_extent = bar.viewport.bottom - bar.viewport.top;
        bar.content_extent = CustomSections().content_height * scale_; bar.expand_progress = 1.0f;
        return bar;
    }
    void DragCustomScrollbar(float y) {
        const auto bar = CustomScrollbar(); const auto thumb = fluent::ScrollbarThumbRect(bar, scale_);
        const float offset = fluent::ScrollbarDragOffset(bar.viewport.top, bar.viewport_extent, thumb.bottom - thumb.top,
            std::max(0.0f, bar.content_extent - bar.viewport_extent), y, custom_bar_grab_);
        ScrollCustom(offset - custom_scroll_);
    }
    void ScrollCustom(float delta) {
        const float total = CustomSections().content_height * scale_;
        custom_scroll_ = std::clamp(custom_scroll_ + delta, 0.0f, std::max(0.0f, total - (CustomPanel().bottom - CustomPanel().top)));
        PlaceCustomEdits();
        Invalidate();
    }
    void DrawCustom(const Theme& theme) {
        if (spec_.custom_controls.empty() || CompactCustom()) return;
        const auto panel = CustomPanel();
        if (panel.bottom <= panel.top) return;
        auto* dc = compositor_.Dc();
        const bool flow = UseFlowCustomLayout();
        if (PropertySidebar()) {
            painter_.FillRoundedRect(D2D1::RectF(RendererWidth(), kPickerTitleDip * scale_, Width(), chrome_layout_.footer.top),
                0, theme.surface_sheet);
            painter_.FillRoundedRect(panel, 8 * scale_, theme.surface_card);
        } else {
            const bool hc = IsHighContrast();
            if (hc || (chrome_.frame_title_alpha >= 1.0f && chrome_.frame_sheet_alpha >= 1.0f)) {
                painter_.FillRoundedRect(panel, 0, theme.surface_sheet);
            } else {
                painter_.FillRoundedRect(panel, 0, WithAlpha(theme.surface_title, chrome_.frame_title_alpha));
                painter_.FillRoundedRect(panel, 0, WithAlpha(theme.surface_sheet, chrome_.frame_sheet_alpha));
            }
            const float stroke = std::max(1.0f, scale_);
            painter_.FillRoundedRect(D2D1::RectF(panel.left, panel.top, panel.right, panel.top + stroke), 0, theme.stroke_divider);
            painter_.FillRoundedRect(D2D1::RectF(panel.left, panel.bottom - stroke, panel.right, panel.bottom), 0, theme.stroke_divider);
        }
        dc->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            const auto state = EffectiveControl(i);
            if (!(state & 2) || spec_.custom_controls[i].kind == 5) continue;
            const auto rect = CustomRect(i);
            if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
            const auto& control = spec_.custom_controls[i];
            if (control.property_layout && (rect.top < panel.top || rect.bottom > panel.bottom)) continue;
            if (control.kind == kPickerCustomSeparator) {
                const bool vertical = flow && i < custom_flow_.boxes.size() && custom_flow_.boxes[i].vertical_separator;
                const float stroke = std::max(1.0f, scale_);
                if (vertical) {
                    const float cx = (rect.left + rect.right) * 0.5f;
                    const float pad = 6.0f * scale_;
                    painter_.FillRoundedRect(D2D1::RectF(cx, rect.top + pad, cx + stroke, std::max(rect.top + pad + stroke, rect.bottom - pad)), 0, theme.stroke_divider);
                } else {
                    const float y = (rect.top + rect.bottom) * 0.5f;
                    painter_.FillRoundedRect(D2D1::RectF(rect.left, y, rect.right, y + stroke), 0, theme.stroke_divider);
                }
                continue;
            }
            auto button_rect = rect;
            std::wstring caption = FormatCustomControlCaption(control.label);
            const auto measure = [&](std::wstring_view text) {
                return typography::MeasureLine(&compositor_, compositor_.TextFormat(), text);
            };
            if (const auto* item = SelectedComboItem(control)) {
                caption = FormatCustomControlCaption(item->label);
                const auto display_label = FormatCustomControlCaption(control.label);
                if (!display_label.empty()) {
                    auto field_label = rect;
                    field_label.right = rect.left + std::min(measure(display_label), (rect.right - rect.left) * 0.42f);
                    painter_.DrawText(FitTextEnd(display_label, field_label.right - field_label.left, measure),
                        field_label, compositor_.TextFormat(), (state & 1) ? theme.text_secondary : theme.text_disabled);
                    button_rect.left = field_label.right + 8 * scale_;
                }
            }
            const float pad_reserve = (control.kind == 1 || control.kind == kPickerCustomText) ? 0.0f :
                control.kind == 7 ? 26.0f * scale_ :
                (control.kind == 3 || control.kind == 4) ? 36.0f * scale_ :
                control.kind == 2 ? 16.0f * scale_ : 32.0f * scale_;
            const auto label = (control.property_layout == 2 || control.property_layout == 3) ? caption :
                FitTextEnd(caption, std::max(0.0f, button_rect.right - button_rect.left - pad_reserve), measure);
            if (control.kind == 1 || control.kind == kPickerCustomText) {
                if (!label.empty()) {
                    const bool inline_caption = flow && i < custom_flow_.boxes.size() && custom_flow_.boxes[i].inline_caption;
                    auto* format = (!inline_caption && control.kind == 1 && flow) ? compositor_.SmallFormat() : compositor_.TextFormat();
                    const auto color = control.kind == 1
                        ? (control.prominent ? theme.text : theme.text_secondary)
                        : ((state & 1) ? theme.text_secondary : theme.text_disabled);
                    painter_.DrawText(label, rect, format, color);
                }
            }
            else {
                fluent::ControlState visual{}; visual.enabled = (state & 1) != 0;
                const int id = kCustomFocusBase + static_cast<int>(i);
                visual.hovered = chrome_.hover == id; visual.pressed = chrome_.pressed == id;
                visual.keyboard_focus = chrome_.show_focus && focus_ == id;
                if(control.kind==8) {visual.focused=GetFocus()==custom_edits_[i];painter_.DrawTextFieldFrame(rect,visual,true);continue;}
                if(control.kind==7) {visual.checked=control.checked;painter_.DrawCheckBox(rect,label,visual);continue;}
                if(control.kind==6) {
                    const bool segmented = flow && i < custom_flow_.boxes.size() && custom_flow_.boxes[i].segmented_radio;
                    if (segmented) {
                        painter_.DrawSegmentedTrack(rect);
                        size_t visible_count = 0;
                        for (const auto& item : control.items) if (item.state & 2) ++visible_count;
                        size_t vis_index = 0;
                        for (size_t j = 0; j < control.items.size(); ++j) {
                            const auto& item = control.items[j]; if (!(item.state & 2)) continue;
                            const size_t pos_idx = vis_index++;
                            const auto rrect = RadioRect(i, j);
                            if (rrect.right <= rrect.left || rrect.bottom <= rrect.top) continue;
                            fluent::SegmentedItemSpec seg;
                            seg.bounds = rrect;
                            seg.shared_track = true;
                            seg.position = visible_count <= 1 ? fluent::SegmentPosition::Single
                                : pos_idx == 0 ? fluent::SegmentPosition::First
                                : pos_idx + 1 == visible_count ? fluent::SegmentPosition::Last
                                : fluent::SegmentPosition::Middle;
                            seg.state.enabled = visual.enabled && (item.state & 1) != 0;
                            seg.state.selected = control.has_selected && control.selected_item_id == item.id;
                            seg.state.hovered = seg.state.enabled && visual.hovered && custom_hover_sub_ == static_cast<int>(j);
                            seg.state.pressed = seg.state.enabled && visual.pressed && custom_hover_sub_ == static_cast<int>(j);
                            seg.state.keyboard_focus = visual.keyboard_focus && seg.state.selected;
                            const auto seg_label = FitTextEnd(FormatCustomControlCaption(item.label),
                                std::max(0.0f, rrect.right - rrect.left - 12.0f * scale_), measure);
                            seg.text = seg_label;
                            painter_.DrawSegmentedItem(seg);
                        }
                        continue;
                    }
                    for(size_t j=0;j<control.items.size();++j) {
                        const auto& item=control.items[j];if(!(item.state&2))continue;
                        const auto rrect = RadioRect(i, j);
                        if (rrect.right <= rrect.left || rrect.bottom <= rrect.top) continue;
                        auto row_state=visual;row_state.enabled=visual.enabled && (item.state&1)!=0;
                        row_state.hovered = row_state.enabled && visual.hovered && custom_hover_sub_ == static_cast<int>(j);
                        row_state.pressed = row_state.enabled && visual.pressed && custom_hover_sub_ == static_cast<int>(j);
                        row_state.checked=control.has_selected && control.selected_item_id==item.id;
                        const auto radio_label = FitTextEnd(FormatCustomControlCaption(item.label),
                            std::max(0.0f, rrect.right - rrect.left - 28.0f * scale_), measure);
                        painter_.DrawRadioButton(rrect, radio_label, row_state);
                    }
                    continue;
                }
                fluent::ButtonSpec button{button_rect,label,{},fluent::ButtonKind::Standard,visual};
                button.drop_down=control.kind==3 || control.kind==4;
                painter_.DrawButton(button);
            }
        }
        painter_.DrawScrollbar(CustomScrollbar());
        dc->PopAxisAlignedClip();
    }
    HRESULT UpdateCustom(const PickerCustomControl& control) {
        if (done_) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        auto found = std::find_if(spec_.custom_controls.begin(), spec_.custom_controls.end(),
            [&](const auto& item) { return item.id == control.id; });
        if (found == spec_.custom_controls.end() || found->kind != control.kind ||
            found->has_parent != control.has_parent || found->parent_id != control.parent_id) return E_INVALIDARG;
        // All potentially allocating copies precede mutation. If publishing the
        // snapshot fails, restore local state without allocating and end Show:
        // the adapter may have partially copied its snapshot before failing.
        PickerCustomControl previous=control;
        PickerDialogSnapshot previous_snapshot = embedded_state_;
        std::swap(*found,previous);
        const HRESULT published = NotifyEmbedded(PickerDialogEvent::Snapshot);
        if (published != S_OK) {
            std::swap(*found,previous);
            embedded_state_ = std::move(previous_snapshot);
            FailPaintedEdit(ERROR_FUNCTION_FAILED);
            return FAILED(published) ? published : E_UNEXPECTED;
        }
        if (focus_ >= kCustomFocusBase && EffectiveControl(static_cast<size_t>(focus_ - kCustomFocusBase)) != 3)
            focus_ = kPickList;
        const auto index=static_cast<size_t>(found-spec_.custom_controls.begin());
        if(found->kind==8 && index<custom_edits_.size() && custom_edits_[index] && ReadEdit(custom_edits_[index])!=found->text) {
            setting_edits_=true;SetWindowTextW(custom_edits_[index],found->text.c_str());setting_edits_=false;
        }
        Relayout(); ScrollCustom(0); Invalidate(); return S_OK;
    }
    HRESULT UpdateText(uint32_t kind, const std::wstring& text) {
        if (done_) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if (kind < PULSE_PICKER_UI_V2_TEXT_TITLE || kind > PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION) return E_INVALIDARG;
        // Allocate before changing state. A setter invalidates pending acceptance,
        // including a FileOk callback whose candidate used the old filename/extension.
        auto copied = text;
        if (kind == PULSE_PICKER_UI_V2_TEXT_FILENAME && copied == filename_text_) return S_OK;
        if (kind == PULSE_PICKER_UI_V2_TEXT_DEFAULT_EXTENSION && copied == spec_.default_extension) return S_OK;
        if (kind == PULSE_PICKER_UI_V2_TEXT_TITLE) {
            chrome_.title = copied; spec_.title = std::move(copied); spec_.explicit_title = true;
            SetWindowTextW(hwnd_, spec_.title.c_str());
        } else if (kind == PULSE_PICKER_UI_V2_TEXT_OK_LABEL) {
            chrome_.primary_text = normal_primary_text_ = copied; spec_.ok_button_label = std::move(copied); spec_.has_ok_button_label = true;
        } else if (kind == PULSE_PICKER_UI_V2_TEXT_FILENAME_LABEL) {
            chrome_.filename_label = copied; spec_.file_name_label = std::move(copied); spec_.has_file_name_label = true;
        } else {
            ++generation_; validating_ = false;
            ClearEmbeddedCandidate();
            if (kind == PULSE_PICKER_UI_V2_TEXT_FILENAME) {
                filename_from_selection_ = false; selected_set_.clear(); selected_ = anchor_ = -1;
                SetFilename(std::move(copied));
            } else {
                spec_.default_extension = std::move(copied);
                spec_.use_open_default_extension = mode_ != PickerMode::Save;
                UpdateChosen();
            }
        }
        if (done_) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        Relayout(); Invalidate(); return S_OK;
    }

    // ---- jump strip (HTML 跳转条) ---------------------------------------------
    // The host's list, or else the folders the shared Pulse places mark open.
    std::vector<PickerOpenFolder> OpenFolders() const {
        if (!spec_.open_folders.empty()) return spec_.open_folders;
        std::vector<PickerOpenFolder> folders;
        for (const auto& entry : pulse_places_.folders) {
            if (!entry.open) continue;
            picker_user_storage::Path parsed;
            if (!picker_user_storage::Parse(entry.path, parsed)) continue;
            folders.push_back({parsed.value, entry.label, false});
        }
        return folders;
    }
    void JumpToOpenFolder(size_t index) {
        const auto folders = OpenFolders();
        if (index >= folders.size() || folders[index].path.empty()) return;
        Navigate(folders[index].path, true);
        Invalidate();
    }
    void UndoAutoJump() {
        if (spec_.auto_jump_from.empty()) return;
        auto from = spec_.auto_jump_from;
        spec_.auto_jump_from.clear();  // offered once
        Navigate(std::move(from), true);
        Invalidate();
    }
    // ---- jump panel (HTML 输入跳转) --------------------------------------------
    static bool JumpLooksLikePath(const std::wstring& text) {
        return text.find_first_of(L"\\/:") != std::wstring::npos || (!text.empty() && (text[0] == L'%' || text[0] == L'.'));
    }
    static bool JumpMatches(std::wstring hay, std::wstring query) {
        CharLowerBuffW(hay.data(), static_cast<DWORD>(hay.size()));
        CharLowerBuffW(query.data(), static_cast<DWORD>(query.size()));
        return hay.find(query) != std::wstring::npos;
    }
    bool JumpMatchesFolder(const std::wstring& name, const std::wstring& path, const std::wstring& query) const {
        if (JumpMatches(name, query) || JumpMatches(path, query)) return true;
        return pulse_places_.jump_policy_available && pulse_places_.pinyin &&
            !app::RankJumpFolders({{path, name}}, query, true, 1).empty();
    }
    void CloseJump() {
        if (!jump_.open) return;
        jump_.open = false;
        jump_.rows.clear();
        Invalidate();
    }
    void UpdateJump() {
        const std::wstring query = path_edit_ ? ReadEdit(path_edit_) : std::wstring();
        const bool keep_selection = query == jump_.query && jump_.selected >= 0 && static_cast<size_t>(jump_.selected) < jump_.rows.size();
        const auto selected_path = keep_selection ? jump_.rows[static_cast<size_t>(jump_.selected)].path : std::wstring();
        jump_.rows.clear(); jump_.selected = 0; jump_.hover = -1; jump_.query = query;
        jump_.open = address_editing_ && !query.empty() && !JumpLooksLikePath(query);
        if (!jump_.open) { Invalidate(); return; }
        using I = l10n::StringId;
        jump_.titles[kJumpOpen] = l10n::Get(I::PickerOpenFolders);
        jump_.titles[kJumpRecent] = spec_.host_app_name.empty() ? l10n::Get(I::Recent)
            : FillPickerText(l10n::Get(I::PickerJumpRecent), spec_.host_app_name);
        jump_.titles[kJumpFavorite] = l10n::Get(I::StarredItems);
        jump_.titles[kJumpEverywhere] = l10n::Get(spec_.jump_index_status == 1 ? I::PickerJumpUserIndex : I::PickerJumpEverywhere);
        jump_.empty_text = l10n::Get(I::PickerJumpNoMatch);
        jump_.hints[0] = l10n::Get(I::PickerJumpHintPick);
        jump_.hints[1] = l10n::Get(I::PickerJumpHintEnter);
        jump_.hints[2] = l10n::Get(I::PickerJumpHintTab);
        jump_.hints[3] = mode_ == PickerMode::Save ? l10n::Get(I::PickerJumpHintSaveHere) : std::wstring();
        jump_.hints[4] = l10n::Get(I::PickerJumpHintBack);
        jump_.foot_note = l10n::Get(I::PickerJumpNote);
        const auto open_folders = OpenFolders();
        for (size_t i = 0; i < open_folders.size(); ++i) {
            const auto& folder = open_folders[i];
            const auto name = FolderLabel(folder);
            if (!JumpMatchesFolder(name, folder.path, query)) continue;
            PickerJumpRow row;
            row.group = kJumpOpen; row.name = name; row.path = folder.path; row.window = folder.explorer;
            row.note = l10n::Get(folder.explorer ? I::PickerExplorerSource : I::PickerJumpPulseTab);
            if (i < 9) row.key = L"Alt+" + std::to_wstring(i + 1);
            jump_.rows.push_back(std::move(row));
        }
        if (!spec_.last_location.path.empty()) {
            const auto name = FolderLabel(spec_.last_location);
            if (JumpMatchesFolder(name, spec_.last_location.path, query))
                jump_.rows.push_back({kJumpRecent, name, spec_.last_location.path, spec_.last_location_source});
        }
        if (spec_.jump_search) {
            std::vector<PickerJumpResult> found;
            try { found = spec_.jump_search(query); } catch (...) { found.clear(); }
            for (auto& result : found) {
                if (result.path.empty()) continue;
                PickerJumpRow row;
                row.group = std::clamp(result.group, static_cast<int>(kJumpRecent), static_cast<int>(kJumpEverywhere));
                row.name = result.name.empty() ? PickerLeafName(result.path) : result.name;
                row.path = std::move(result.path); row.note = std::move(result.note); row.file = result.file;
                jump_.rows.push_back(std::move(row));
            }
            std::stable_sort(jump_.rows.begin(), jump_.rows.end(), [](const PickerJumpRow& a, const PickerJumpRow& b) { return a.group < b.group; });
        }
        if (!current_.empty() && !picker_navigation::IsComputerPath(current_)) {
            PickerJumpRow create;
            create.group = kJumpCreate;
            create.name = FillPickerText(FillFolderText(l10n::Get(I::PickerJumpCreate), PickerLeafName(current_)), query);
            create.path = current_ + (current_.back() == L'\\' ? L"" : L"\\") + query;
            create.key = L"Ctrl+Shift+Enter";
            jump_.rows.push_back(std::move(create));
        }
        for (auto& detail : jump_.group_details) detail.clear();
        for (int group = 0; group < kJumpCreate; ++group) {
            size_t count = 0;
            for (const auto& row : jump_.rows) if (row.group == group) ++count;
            if (count) jump_.group_details[group] = std::to_wstring(count);
        }
        if (spec_.jump_search_complete && spec_.jump_search_query == query) {
            size_t count = 0;
            for (const auto& row : jump_.rows) if (row.group == kJumpEverywhere) ++count;
            auto text = l10n::Get(I::PickerSearchResultSummary);
            auto at = text.find(L"{count}");
            if (at != text.npos) text.replace(at, 7, std::to_wstring(count));
            at = text.find(L"{ms}");
            if (at != text.npos) text.replace(at, 4, std::to_wstring(spec_.jump_search_elapsed_ms));
            jump_.group_details[kJumpEverywhere] = std::move(text);
        }
        jump_.index_notice = spec_.jump_index_status == 1 ? l10n::Get(I::PickerJumpUserIndex) : std::wstring();
        jump_.index_action_text = spec_.jump_index_status == 1 ? l10n::Get(I::PickerIndexLearn) : std::wstring();
        if (keep_selection) for (size_t i = 0; i < jump_.rows.size(); ++i)
            if (SamePickerPath(jump_.rows[i].path, selected_path)) { jump_.selected = static_cast<int>(i); break; }
        Invalidate();
    }
    static std::wstring FillFolderText(std::wstring text, const std::wstring& folder) {
        const auto at = text.find(L"{folder}");
        if (at != std::wstring::npos) text.replace(at, 8, folder);
        return text;
    }
    void ActivateJumpRow(int index, bool save_here) {
        if (index < 0 || static_cast<size_t>(index) >= jump_.rows.size()) return;
        const PickerJumpRow row = jump_.rows[static_cast<size_t>(index)];
        const auto query = jump_.query;
        CloseJump();
        EndAddressEdit(true);
        if (row.group == kJumpCreate) {
            NewNamedFolder(query);
            return;
        }
        const auto destination = row.file ? PickerParent(row.path) : row.path;
        if (destination.empty()) return;
        const auto before = generation_;
        Navigate(destination, true, row.file ? row.path : std::wstring());
        if (save_here && mode_ == PickerMode::Save && !row.file && waiting_ &&
            generation_ != before && SamePickerPath(current_, destination)) {
            // Resume through the normal save validator only after this listing commits.
            jump_save_generation_ = generation_;
            jump_save_filename_ = filename_text_;
        }
    }
    // Keys while the address row edits with the panel open; true when handled.
    bool JumpKey(WPARAM key) {
        if (!jump_.open) return false;
        const int count = static_cast<int>(jump_.rows.size());
        if (key == VK_DOWN || key == VK_UP) {
            if (count) jump_.selected = (jump_.selected + (key == VK_DOWN ? 1 : count - 1)) % count;
            Invalidate();
            return true;
        }
        if (key == VK_ESCAPE) { CloseJump(); return true; }
        if (key == VK_RETURN) {
            const bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT);
            int index = jump_.selected;
            if (ctrl && shift) {
                index = -1;
                for (int i = 0; i < count; ++i) if (jump_.rows[static_cast<size_t>(i)].group == kJumpCreate) index = i;
            }
            ActivateJumpRow(index, ctrl && !shift);
            return true;
        }
        if (key == VK_TAB && count && jump_.selected < count && jump_.rows[static_cast<size_t>(jump_.selected)].group != kJumpCreate) {
            // Complete to the full path; the panel closes because a path is not a name.
            auto path = jump_.rows[static_cast<size_t>(jump_.selected)].path;
            if (!jump_.rows[static_cast<size_t>(jump_.selected)].file && !path.empty() && path.back() != L'\\') path += L'\\';
            SetWindowTextW(path_edit_, path.c_str());
            SendMessageW(path_edit_, EM_SETSEL, path.size(), path.size());
            return true;
        }
        return false;
    }
    static std::wstring FolderLabel(const PickerOpenFolder& folder) {
        if (!folder.label.empty()) return folder.label;
        return PickerLeafName(folder.path);
    }
    void SyncJumpStrip() {
        chrome_.strip_left = 0.0f;
        chrome_.band_left = scale_ > 0.0f ? renderer_.EffectiveSidebarWidth(static_cast<float>(Width())) / scale_ : 0.0f;
        chrome_.app_name = spec_.host_app_name;
        chrome_.app_icon = AppIconBitmap();
        chrome_.pulse_logo = renderer_.PulseLogoBitmap();
        chrome_.strip_caption = l10n::Get(l10n::StringId::PickerGoTo);
        chrome_.open_folders.clear();
        for (const auto& folder : OpenFolders()) {
            PickerOpenChip chip;
            chip.label = FolderLabel(folder);
            chip.path = folder.path;
            chip.window = folder.explorer;
            chip.source = l10n::Get(folder.explorer ? l10n::StringId::PickerExplorerSource : l10n::StringId::PickerPulseSource);
            chip.active = SamePickerPath(folder.path, current_);
            chrome_.open_folders.push_back(std::move(chip));
        }
        // The last place is the 「上次」 row of 常用 now; the strip holds what is open.
        chrome_.last_location = {};
        chrome_.save_original_location = {};
        chrome_.save_repo_root = {};
        if (mode_ == PickerMode::Save) {
            if (!original_file_path_.empty()) {
                const auto orig_dir = PickerParent(original_file_path_);
                if (!orig_dir.empty()) {
                    chrome_.save_original_location.label = PickerLeafName(orig_dir);
                    chrome_.save_original_location.source = FillPickerText(l10n::Get(l10n::StringId::PickerOriginalLocationSrc), PickerLeafName(original_file_path_));
                    chrome_.save_original_location.path = orig_dir;
                    chrome_.save_original_location.active = SamePickerPath(orig_dir, current_);
                }
            }
            if (!git_repo_root_.empty()) {
                chrome_.save_repo_root.label = PickerLeafName(git_repo_root_);
                chrome_.save_repo_root.source = l10n::Get(l10n::StringId::PickerRepoRoot);
                chrome_.save_repo_root.path = git_repo_root_;
                chrome_.save_repo_root.active = SamePickerPath(git_repo_root_, current_);
            }
        }
        chrome_.show_tray_remove = TrayConsumeAvailable() && (HasSelectedTrayItem() || (!BasketEnabled() && HasCompatibleTrayItem()));
        chrome_.tray_remove_checked = (picker_flags_ & picker_layout::kFlagRemoveTrayAfterOpen) != 0;
        chrome_.tray_remove_text = l10n::Get(l10n::StringId::PickerTrayRemoveAfterOpen);
        chrome_.auto_jump_notice.clear();
        if (!spec_.auto_jump_from.empty() && !current_.empty() && !SamePickerPath(spec_.auto_jump_from, current_))
            chrome_.auto_jump_notice = FillPickerText(l10n::Get(l10n::StringId::PickerAutoJumpNotice), PickerLeafName(current_));
        chrome_.undo_jump_text.clear();
        if (!spec_.auto_jump_from.empty() && !SamePickerPath(spec_.auto_jump_from, current_))
            chrome_.undo_jump_text = FillPickerText(l10n::Get(l10n::StringId::PickerUndoJump), PickerLeafName(spec_.auto_jump_from));
        chrome_.selection_summary.clear(); chrome_.selection_hint.clear();
        if (allow_multiselect_) {
            uint64_t bytes = 0; size_t count = 0;
            for (int index : selected_set_) if (index >= 0 && static_cast<size_t>(index) < entries_.size()) {
                const auto& entry = entries_[static_cast<size_t>(index)];
                if (entry.kind == PickerEntryKind::File) { bytes += entry.size; ++count; }
            }
            std::vector<std::wstring> folders;
            for (const auto& item : basket_) {
                bytes += item.size; ++count;
                const auto parent = PickerParent(item.path);
                if (std::none_of(folders.begin(), folders.end(), [&](const auto& f) { return SamePickerPath(f, parent); })) folders.push_back(parent);
            }
            if (!basket_.empty()) {
                if (std::none_of(folders.begin(), folders.end(), [&](const auto& f) { return SamePickerPath(f, current_); }) &&
                    count > basket_.size()) folders.push_back(current_);
                auto text = l10n::Get(l10n::StringId::PickerBasketSummary);
                const auto fill = [&](const wchar_t* key, const std::wstring& value) {
                    const auto at = text.find(key);
                    if (at != text.npos) text.replace(at, wcslen(key), value);
                };
                fill(L"{count}", std::to_wstring(count));
                fill(L"{size}", format::ByteSize(bytes));
                fill(L"{folders}", std::to_wstring(folders.size()));
                chrome_.selection_summary = std::move(text);
                chrome_.selection_hint = l10n::Get(l10n::StringId::PickerBasketHint);
            } else if (count) {
                auto text = l10n::Get(l10n::StringId::PickerSelectedSizeFormat);
                auto at = text.find(L"{count}");
                if (at != text.npos) text.replace(at, 7, std::to_wstring(count));
                at = text.find(L"{size}");
                if (at != text.npos) text.replace(at, 6, format::ByteSize(bytes));
                chrome_.selection_summary = std::move(text);
                chrome_.selection_hint = l10n::Get(l10n::StringId::PickerSelectionContinue);
            }
        }
        chrome_.new_folder_text = (spec_.hide_new_folder_button || InArchive()) ? std::wstring{} : l10n::Get(l10n::StringId::NewFolder);
        chrome_.new_folder_key = L"Ctrl+Shift+N";
        chrome_.context_filters.clear();
        if (mode_ == PickerMode::Save) {
            BuildSaveFilterBar();
        } else if (mode_ == PickerMode::Folder) {
            PickerOptionChip chip;
            chip.id = kPickContextFilterBase; chip.checked = true; chip.enabled = false;
            chip.text = l10n::Get(l10n::StringId::PickerFoldersOnly);
            chrome_.context_filters.push_back(std::move(chip));
        } else if (mode_ == PickerMode::File || mode_ == PickerMode::Image) {
            BuildFilterBar();
        }
        chrome_.filter_tools_visible = true;
        chrome_.sort_synced = !order_overrides_.contains(ListOrderKey(current_)) && static_cast<bool>(spec_.folder_order);
        chrome_.preview_toggle_visible = mode_ != PickerMode::Folder && !(spec_.native_options & FOS_FORCEPREVIEWPANEON);
        chrome_.preview_toggle_on = PreviewPaneShown();
    }

    void BuildSaveFilterBar() {
        int total_files = 0, same_files = 0;
        for (const auto& entry : entries_) {
            if (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive) continue;
            ++total_files;
            if (PickerMatchesFilter(entry.name, mode_, options_)) ++same_files;
        }
        if (!options_.save_same_format_only) save_total_files_cached_ = total_files;
        const int shown_total = save_total_files_cached_ >= 0 ? save_total_files_cached_ : total_files;
        PickerOptionChip all;
        all.id = kPickSaveFilterAll;
        all.text = l10n::Get(l10n::StringId::PickerSaveFilterAll);
        all.checked = !options_.save_same_format_only;
        all.count = shown_total;
        chrome_.context_filters.push_back(std::move(all));
        if (options_.filter_index < options_.filters.size() && !PickerFilterIsAny(options_.filters[options_.filter_index])) {
            const auto labels = PickerFormatLabels(options_.filters);
            const std::wstring fmt_name = options_.filter_index < labels.size() ? labels[options_.filter_index] : PickerFilterName(options_.filters[options_.filter_index]);
            PickerOptionChip same;
            same.id = kPickSaveFilterSame;
            same.text = l10n::Get(l10n::StringId::PickerSaveFilterSame);
            if (const auto pos = same.text.find(L"{format}"); pos != std::wstring::npos)
                same.text.replace(pos, 8, fmt_name);
            else
                same.text += L" " + fmt_name;
            same.checked = options_.save_same_format_only;
            same.count = same_files;
            chrome_.context_filters.push_back(std::move(same));
        }
        PickerOptionChip time;
        time.id = kPickTimeMenu;
        time.text = TimeFilterText(time_filter_);
        time.checked = time_filter_ != 0;
        time.drop_down = true;
        time.gap_before = true;
        time.fold_late = true;
        chrome_.context_filters.push_back(std::move(time));
    }

    // ---- open filter bar (HTML 筛选条) ----

    // The program's types with files here (counts), 更多 for the rest, *.* last;
    // with only *.*, Pulse's own categories of what is here. Then 时间 ▾ (merging 只看新文件).
    void BuildFilterBar() {
        if (Width() / scale_ < 760.0f) {
            PickerOptionChip chip; chip.id = kPickCompactFilters;
            chip.text = l10n::Get(l10n::StringId::ToolbarFilter);
            chip.checked = time_filter_ != 0 || options_.only_new || options_.category != 0;
            chip.drop_down = true;
            chrome_.context_filters.push_back(std::move(chip));
            return;
        }
        const size_t count = std::min<size_t>(options_.filters.size(), 64);
        bool any_only = true;
        for (size_t i = 0; i < count; ++i) if (!PickerFilterIsAny(options_.filters[i])) any_only = false;
        const auto labels = PickerFormatLabels(options_.filters);
        const auto counted = [&](size_t i) { return counts_valid_ && i < filter_counts_.size() ? static_cast<int>(filter_counts_[i]) : -1; };
        if (!any_only) {
            const auto chip_text = [&](size_t i) {
                const auto name = PickerFilterName(options_.filters[i]);
                const auto extensions = PickerFilterExtensions(options_.filters[i]);
                return !name.empty() && (extensions.size() > 3 || name.size() <= 10) ? name : labels[i];
            };
            for (size_t i = 0; i < count; ++i) {
                if (PickerFilterIsAny(options_.filters[i])) continue;
                const bool current = i == options_.filter_index;
                const int files = counted(i);
                if (!current && i != 0 && files == 0) continue;  // no files of this type here: the type list keeps it
                PickerOptionChip chip;
                chip.id = kPickContextFilterBase + static_cast<int>(i);
                chip.text = chip_text(i);
                chip.label = options_.filters[i].pattern;  // the view picks the chip's icon from it
                chip.checked = current;
                chip.count = files;
                chip.hideable = !current;
                chrome_.context_filters.push_back(std::move(chip));
            }
            PickerOptionChip more;
            more.id = kPickFilterMore; more.text = l10n::Get(l10n::StringId::More); more.more = true;
            chrome_.context_filters.push_back(std::move(more));
            for (size_t i = 0; i < count; ++i) {
                if (!PickerFilterIsAny(options_.filters[i])) continue;
                PickerOptionChip chip;
                chip.id = kPickContextFilterBase + static_cast<int>(i);
                chip.text = PickerFilterName(options_.filters[i]);
                if (chip.text.empty()) chip.text = options_.filters[i].label;
                chip.checked = i == options_.filter_index;
                chip.count = counted(i);
                chip.hideable = !chip.checked;
                chrome_.context_filters.push_back(std::move(chip));
            }
        } else if (counts_valid_) {
            static constexpr l10n::StringId kNames[kPickerCategoryCount] = {l10n::StringId::FilterAll, l10n::StringId::KindImage,
                l10n::StringId::KindDocument, l10n::StringId::KindVideo, l10n::StringId::KindAudio, l10n::StringId::KindArchive,
                l10n::StringId::DetailsOther};
            for (size_t c = 0; c < kPickerCategoryCount; ++c) {
                if (c && !category_counts_[c] && options_.category != static_cast<int>(c)) continue;
                PickerOptionChip chip;
                chip.id = kPickCategoryBase + static_cast<int>(c);
                chip.text = l10n::Get(kNames[c]);
                chip.count = static_cast<int>(category_counts_[c]);
                chip.checked = options_.category == static_cast<int>(c);
                chip.hideable = c != 0 && !chip.checked;  // a short row keeps 全部 and the chosen one
                chrome_.context_filters.push_back(std::move(chip));
            }
        }
        // One dropdown chip for time (HTML 时间 ▾), merging 只看新文件 when available.
        PickerOptionChip time;
        time.id = kPickTimeMenu;
        time.text = options_.only_new ? l10n::Get(l10n::StringId::PickerNewFiles) : TimeFilterText(time_filter_);
        time.checked = time_filter_ != 0 || options_.only_new;
        time.drop_down = true;
        if (options_.only_new && counts_valid_) time.count = static_cast<int>(new_count_);
        time.gap_before = true;
        time.fold_late = true;
        chrome_.context_filters.push_back(std::move(time));
    }

    std::wstring TimeFilterText(int filter) const {
        switch (filter) {
        case 1: return l10n::Get(l10n::StringId::DateToday);
        case 2: return l10n::Get(l10n::StringId::DateThisWeek);
        default: return l10n::Get(l10n::StringId::PickerFilterTime);
        }
    }

    void ShowTimeMenu(const D2D1_RECT_F& anchor) {
        std::vector<FluentMenuItem> items;
        items.push_back(Radio(1, l10n::Get(l10n::StringId::DateAny), !options_.only_new && time_filter_ == 0));
        items.push_back(Radio(2, l10n::Get(l10n::StringId::DateToday), !options_.only_new && time_filter_ == 1));
        items.push_back(Radio(3, l10n::Get(l10n::StringId::DateThisWeek), !options_.only_new && time_filter_ == 2));
        if (mode_ != PickerMode::Save && options_.new_since) {
            items.back().separator_after = true;
            auto fresh = Radio(4, l10n::Get(l10n::StringId::PickerFilterOnlyNew), options_.only_new);
            fresh.detail = l10n::Get(l10n::StringId::PickerFilterOnlyNewSub);
            if (counts_valid_) fresh.badge_text = std::to_wstring(new_count_);
            items.push_back(std::move(fresh));
        }
        menu_.SetTheme(dark_, accent_);
        const int command = TrackPickerDropdown(ScreenRect(anchor), std::move(items));
        if (done_ || command <= 0) return;
        if (command == 4) {
            options_.only_new = !options_.only_new;
            if (options_.only_new) { time_filter_ = 0; options_.modified_after = 0; }
            Reload();
            return;
        }
        const int filter = command - 1;
        if (options_.only_new || filter != time_filter_) {
            options_.only_new = false;
            SetTimeFilter(filter);
        }
    }

    void ShowCompactFilters(const D2D1_RECT_F& anchor) {
        std::vector<FluentMenuItem> items;
        bool any_only = true;
        for (const auto& filter : options_.filters) if (!PickerFilterIsAny(filter)) any_only = false;
        if (!any_only) {
            for (size_t i = 0; i < options_.filters.size() && i < 64; ++i) {
                auto item = Radio(10 + static_cast<int>(i), PickerFilterName(options_.filters[i]), i == options_.filter_index);
                if (counts_valid_ && i < filter_counts_.size()) item.badge_text = std::to_wstring(filter_counts_[i]);
                items.push_back(std::move(item));
            }
        } else {
            static constexpr l10n::StringId names[kPickerCategoryCount] = {l10n::StringId::FilterAll, l10n::StringId::KindImage,
                l10n::StringId::KindDocument, l10n::StringId::KindVideo, l10n::StringId::KindAudio, l10n::StringId::KindArchive,
                l10n::StringId::DetailsOther};
            for (size_t i = 0; i < kPickerCategoryCount; ++i) {
                if (i && counts_valid_ && !category_counts_[i] && options_.category != static_cast<int>(i)) continue;
                auto item = Radio(100 + static_cast<int>(i), l10n::Get(names[i]), options_.category == static_cast<int>(i));
                if (counts_valid_) item.badge_text = std::to_wstring(category_counts_[i]);
                items.push_back(std::move(item));
            }
        }
        if (!items.empty()) items.back().separator_after = true;
        items.push_back(Radio(200, l10n::Get(l10n::StringId::DateAny), time_filter_ == 0));
        items.push_back(Radio(201, l10n::Get(l10n::StringId::DateToday), time_filter_ == 1));
        items.push_back(Radio(202, l10n::Get(l10n::StringId::DateThisWeek), time_filter_ == 2));
        if (options_.new_since) {
            items.back().separator_after = true;
            auto item = Radio(300, l10n::Get(l10n::StringId::PickerNewFiles), options_.only_new);
            if (counts_valid_) item.badge_text = std::to_wstring(new_count_);
            items.push_back(std::move(item));
        }
        menu_.SetTheme(dark_, accent_);
        const int command = TrackPickerDropdown(ScreenRect(anchor), std::move(items));
        if (done_) return;
        if (command >= 10 && command < 74) ApplyFilter(static_cast<size_t>(command - 10));
        else if (command >= 100 && command < 100 + static_cast<int>(kPickerCategoryCount)) SetCategory(command - 100);
        else if (command >= 200 && command <= 202) SetTimeFilter(command - 200);
        else if (command == 300) { options_.only_new = !options_.only_new; Reload(); }
    }

    static uint64_t LocalDayStartTicks(int days_back) {
        SYSTEMTIME now{}; GetLocalTime(&now);
        now.wHour = now.wMinute = now.wSecond = now.wMilliseconds = 0;
        FILETIME local{}, utc{};
        if (!SystemTimeToFileTime(&now, &local) || !LocalFileTimeToFileTime(&local, &utc)) return 0;
        const uint64_t ticks = (static_cast<uint64_t>(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
        return ticks - static_cast<uint64_t>(days_back) * 864000000000ull;
    }
    static uint64_t WeekStartTicks() {
        SYSTEMTIME now{}; GetLocalTime(&now);
        wchar_t first[8]{};
        // LOCALE_IFIRSTDAYOFWEEK: 0 Monday .. 6 Sunday; wDayOfWeek: 0 Sunday.
        const int first_day = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, first, 8) ? (_wtoi(first) + 1) % 7 : 1;
        return LocalDayStartTicks((now.wDayOfWeek + 7 - first_day) % 7);
    }

    // The preview pane: forced by the program, default ON in Save mode (destination panel),
    // else this program's last choice (Alt+P).
    bool PreviewPaneShown() const {
        if (mode_ == PickerMode::Folder) return false;
        if (spec_.native_options & FOS_FORCEPREVIEWPANEON) return true;
        if (mode_ == PickerMode::Save) return (picker_flags_ & picker_layout::kFlagPreviewPaneHidden) == 0;
        return (picker_flags_ & picker_layout::kFlagPreviewPane) != 0;
    }
    void TogglePreviewPane() {
        if (mode_ == PickerMode::Folder || (spec_.native_options & FOS_FORCEPREVIEWPANEON)) return;
        picker_flags_ ^= (mode_ == PickerMode::Save ? picker_layout::kFlagPreviewPaneHidden : picker_layout::kFlagPreviewPane);
        DismissHover();
        Relayout();
        if (selected_ >= 0) EnsureVisible(selected_);
        Invalidate();
    }

    void SetTimeFilter(int filter) {
        time_filter_ = filter;
        options_.modified_after = time_filter_ == 1 ? LocalDayStartTicks(0) : time_filter_ == 2 ? WeekStartTicks()
            : 0;
        Reload();
    }

    void SetCategory(int category) {
        options_.category = options_.category == category ? 0 : category;
        Reload();
    }

    // Pulse's own kinds (the search's type: words) for names when the program said *.*.
    static std::function<int(std::wstring_view)> MakeCategoryOf() {
        auto map = std::make_shared<std::unordered_map<std::wstring, int>>();
        const index::SearchKind kinds[] = {index::SearchKind::Image, index::SearchKind::Document, index::SearchKind::Video,
                                           index::SearchKind::Audio, index::SearchKind::Archive};
        for (int k = 0; k < 5; ++k) {
            const auto list = index::KindExtensions(kinds[k]);
            size_t start = 0;
            while (start < list.size()) {
                size_t end = list.find(L';', start);
                if (end == std::wstring::npos) end = list.size();
                if (end > start) map->emplace(list.substr(start, end - start), k + 1);
                start = end + 1;
            }
        }
        return [map](std::wstring_view name) {
            const size_t dot = name.rfind(L'.');
            if (dot == std::wstring_view::npos || dot == 0 || dot + 1 == name.size()) return 6;
            std::wstring ext(name.substr(dot + 1));
            for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
            const auto it = map->find(ext);
            return it == map->end() ? 6 : it->second;
        };
    }

    static std::wstring FillPickerText(std::wstring text, const std::wstring& name) {
        const auto at = text.find(L"{name}");
        if (at != std::wstring::npos) text.replace(at, 6, name);
        return text;
    }
    // The host program's icon for the title strip, at the pixels the worker extracted
    // (common::LoadIconPixels at the title size). A host that only hands over an HICON
    // goes through the same IconPixels conversion, so alpha and mask icons stay clean.
    ID2D1Bitmap* AppIconBitmap() {
        auto* dc = compositor_.Dc();
        if (!dc) return nullptr;
        common::MenuIconRef pixels = spec_.host_app_icon_pixels;
        if (!pixels && spec_.host_app_icon) {
            if (!app_icon_fallback_) app_icon_fallback_ = common::IconPixels(spec_.host_app_icon);
            pixels = app_icon_fallback_;
        }
        if (!pixels || !pixels->Valid()) return nullptr;
        if (app_icon_bitmap_.get() && app_icon_device_ == dc && app_icon_source_ == pixels) return app_icon_bitmap_.get();
        app_icon_bitmap_.reset();
        app_icon_device_ = dc;
        app_icon_source_ = pixels;
        const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->CreateBitmap(D2D1::SizeU(pixels->width, pixels->height), pixels->bgra.data(), pixels->width * 4, props, &app_icon_bitmap_);
        return app_icon_bitmap_.get();
    }

    void Relayout() {
        if (!compositor_.Dc()) return;
        legacy_child_.Layout(hwnd_);
        DismissHover("hover-layout");
        SyncCustomOptions();
        RebuildCustomFlow();
        renderer_.SetEmbedded(kPickerTitleDip, PickerFooterDip(mode_, chrome_.overwrite_pending || chrome_.live_conflict, !chrome_.options.empty(), !chrome_.formats.empty(), (!chrome_.save_suggestions.empty() || !chrome_.format_note.empty())) + CustomHeightDip());
        SyncPulseSelection();
        const float strip = kPickerStripDip + (chrome_.pulse_selection.empty() ? 0.0f : kPickerPulseSelectionDip);
        if (renderer_.EmbeddedBandDip() != strip) renderer_.SetEmbeddedPickerLayout(true, strip);
        renderer_.SetEmbeddedBand(strip);
        renderer_.SetEmbeddedPickerRecent(
            (!spec_.auto_jump_from.empty() && !current_.empty() && !SamePickerPath(spec_.auto_jump_from, current_) ? kPickerAutoJumpNoticeDip : 0.0f));
        chrome_.strip_top = kPickerTitleDip + renderer_.ToolbarHeightDip() - renderer_.EmbeddedBandDip();
        chrome_.strip_right = RendererWidth() / scale_;
        renderer_.SetDetailsPanelVisible(PreviewPaneShown());
        SyncVm();
        SyncOpenDropDown();
        const auto pane_bounds = PaneBounds();
        chrome_.context_top = pane_bounds.top / scale_;
        chrome_.context_left = pane_bounds.left / scale_;
        chrome_.context_right = renderer_.PaneHeaderTitleRight(vm_.pane, pane_bounds) / scale_;
        chrome_.band_top = renderer_.EmbeddedPickerRecentRect(pane_bounds).top / scale_;
        RefreshChromeLayout();
        ClampScroll();
        PlaceEdits();
    }

    void RefreshChromeLayout() {
        chrome_layout_ = LayoutPickerChrome(Width(), Height(), chrome_, painter_, scale_);
        const float clamped = std::clamp(chrome_.open_scroll, 0.0f, chrome_layout_.open_scroll_max);
        if (std::abs(clamped - chrome_.open_scroll) > 0.01f) {
            chrome_.open_scroll = clamped;
            chrome_layout_ = LayoutPickerChrome(Width(), Height(), chrome_, painter_, scale_);
        }
    }

    void ScrollOpenStrip(float delta_dip) {
        if (chrome_layout_.open_scroll_max <= 0.5f) return;
        const float next = std::clamp(chrome_.open_scroll + delta_dip, 0.0f, chrome_layout_.open_scroll_max);
        if (std::abs(next - chrome_.open_scroll) < 0.01f) return;
        chrome_.open_scroll = next;
        RefreshChromeLayout();
        UpdateHover(HitAt(static_cast<float>(hover_point_.x), static_cast<float>(hover_point_.y)));
        Invalidate();
    }

    void DrawIncomingDropzone(const Theme& theme, bool hc) {
        if (incoming_dropzone_sources_.empty()) return;
        auto* dc = compositor_.Dc();
        if (!dc) return;
        const auto list = renderer_.PaneListRect(vm_.pane, PaneBounds());
        const float inset = 6.0f * scale_;
        const auto zone = D2D1::RectF(list.left + inset, list.top + inset, list.right - inset, list.bottom - inset);
        if (zone.right - zone.left < 120.0f * scale_ || zone.bottom - zone.top < 80.0f * scale_) return;
        const float r = 8.0f * scale_;
        painter_.FillRoundedRect(zone, r, hc ? theme.bg : WithAlpha(theme.accent, dark_ ? 0.14f : 0.08f));
        ComPtr<ID2D1StrokeStyle> dash;
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        if (factory.get()) {
            D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties();
            props.dashStyle = D2D1_DASH_STYLE_DASH;
            factory->CreateStrokeStyle(props, nullptr, 0, &dash);
        }
        ComPtr<ID2D1SolidColorBrush> accent_brush;
        if (SUCCEEDED(dc->CreateSolidColorBrush(hc ? theme.text : theme.accent, &accent_brush))) {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(zone, r, r), accent_brush.get(), std::max(2.0f, 2.0f * scale_), dash.get());
        }
        std::wstring title;
        if (incoming_dropzone_sources_.size() == 1) {
            title = FillPickerText(l10n::Get(l10n::StringId::PickerDropZoneSingle), PickerLeafName(incoming_dropzone_sources_.front()));
        } else {
            title = l10n::Get(l10n::StringId::PickerDropZoneMulti);
            if (const auto pos = title.find(L"{count}"); pos != std::wstring::npos)
                title.replace(pos, 7, std::to_wstring(incoming_dropzone_sources_.size()));
        }
        std::wstring sub;
        if (!spec_.host_app_name.empty()) {
            sub = l10n::Get(l10n::StringId::PickerDropZoneSub);
            if (const auto pos = sub.find(L"{app}"); pos != std::wstring::npos) sub.replace(pos, 5, spec_.host_app_name);
        } else {
            sub = l10n::Get(l10n::StringId::PickerDropZoneSubGeneric);
        }
        const float card_w = std::min(zone.right - zone.left - 24.0f * scale_, std::max(240.0f * scale_,
            std::max(typography::MeasureLine(&compositor_, compositor_.HeaderFormat(), title),
                     typography::MeasureLine(&compositor_, compositor_.SmallFormat(), sub)) + 44.0f * scale_));
        const float card_h = 84.0f * scale_;
        const float cx = (zone.left + zone.right) * 0.5f;
        const float cy = (zone.top + zone.bottom) * 0.5f;
        const auto card = D2D1::RectF(cx - card_w * 0.5f, cy - card_h * 0.5f, cx + card_w * 0.5f, cy + card_h * 0.5f);
        painter_.FillRoundedRect(card, 10.0f * scale_, theme.surface_card);
        painter_.StrokeRoundedRect(card, 10.0f * scale_, hc ? theme.text : theme.stroke_card, std::max(1.0f, scale_));
        painter_.DrawGlyph(L"\xE73E", {cx - 12.0f * scale_, card.top + 8.0f * scale_, cx + 12.0f * scale_, card.top + 32.0f * scale_}, hc ? theme.text : theme.accent);
        painter_.DrawText(title, {card.left + 12.0f * scale_, card.top + 34.0f * scale_, card.right - 12.0f * scale_, card.top + 56.0f * scale_},
                          compositor_.HeaderFormat(), hc ? theme.text : theme.accent, fluent::HorizontalAlignment::Center);
        painter_.DrawText(sub, {card.left + 12.0f * scale_, card.top + 58.0f * scale_, card.right - 12.0f * scale_, card.bottom - 6.0f * scale_},
                          compositor_.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
    }

    void ShowSortSyncMenu(const D2D1_RECT_F& anchor) {
        std::vector<FluentMenuItem> items;
        auto sort_item = SortMenu();
        auto group_item = GroupMenu();
        for (auto& c : sort_item.children) items.push_back(std::move(c));
        if (!items.empty()) items.back().separator_after = true;
        items.push_back(std::move(group_item));
        menu_.SetTheme(dark_, accent_);
        const int command = TrackPickerDropdown(ScreenRect(anchor), std::move(items));
        if (done_ || command <= 0) return;
        HandleSortCommand(command);
    }

    void Render() {
        if (rendering_frame_) return;
        struct FrameScope { bool& active; explicit FrameScope(bool& value) : active(value) { active = true; } ~FrameScope() { active = false; } } frame_scope(rendering_frame_);
        PULSE_PICKER_TRACE_FRAME(spec_.pulse_painted_plain_child);
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            renderer_.SetCompositor(&compositor_);
            renderer_.SetScale(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        if (client.right != compositor_.Width() || client.bottom != compositor_.Height()) {
            compositor_.Resize(client.right, client.bottom);
            Relayout();
        }
        ID2D1DeviceContext* dc = compositor_.Dc();
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        const bool hc = IsHighContrast();
        const Theme theme = hc ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        SyncVm();
        vm_.backdrop_enabled = !hc && compositor_.UsesTransparentComposition() && backdrop_enabled_;
        // The main window's layer opacities over the same material.
        vm_.wallpaper_look = spec_.panel_transparency;
        vm_.frame_transparency = spec_.frame_transparency;
        {
            const auto layers = ComputeLayerAlphas(false, false, vm_.backdrop_enabled, dark_,
                                                   vm_.wallpaper_look, vm_.wallpaper_blur, vm_.frame_transparency);
            chrome_.frame_title_alpha = vm_.backdrop_enabled ? layers.title : 1.0f;
            chrome_.frame_sheet_alpha = vm_.backdrop_enabled ? layers.sheet : 1.0f;
            chrome_.frame_left = RendererWidth();
        }
        PULSE_PICKER_TRACE_POINT("list-before");
        // The tooltip belongs above the chrome painted after the list (strip,
        // toolbar, filter chips), so it is drawn last, not inside Render.
        std::wstring tooltip = std::move(vm_.tooltip_text);
        vm_.tooltip_text.clear();
        renderer_.Render(vm_, RendererRect(), theme);
        vm_.tooltip_text = std::move(tooltip);
        PULSE_PICKER_TRACE_POINT("list-after");
        SyncDetailsPreview();
        painter_.BeginFrame(theme, hc);
        UpdateSummary();
        SyncOpenDropDown();
        const auto motion_now = motion::NowMs();
        const float filter_t = filter_motion_start_ ? std::clamp(
            static_cast<float>(motion_now - filter_motion_start_) / 120.0f, 0.0f, 1.0f) : 1.0f;
        chrome_.filter_turn = filter_motion_from_ +
            ((filter_open_ ? 1.0f : 0.0f) - filter_motion_from_) * motion::EaseOutCubic(filter_t);
        PULSE_PICKER_TRACE_POINT("chrome-before");
        RefreshChromeLayout();
        if (filename_edit_ && (IsWindowVisible(filename_edit_) != FALSE) != (chrome_.selected_names.empty())) PlaceEdits();
        DrawIncomingDropzone(theme, hc);
        DrawPickerChrome(compositor_, painter_, theme, chrome_, chrome_layout_, hc);
        DrawCustom(theme);
        PULSE_PICKER_TRACE_POINT("chrome-after");
        PULSE_PICKER_TRACE_POINT("painted-edits-before");
        DrawPaintedEdits();
        PULSE_PICKER_TRACE_POINT("painted-edits-after");
        if (jump_.open) {
            jump_.hover = chrome_.hover >= kPickJumpRowBase ? chrome_.hover - kPickJumpRowBase : -1;
            jump_layout_ = LayoutPickerJump(jump_, renderer_.AddressBarRect(Width()), Width(),
                                            chrome_layout_.footer.top - 8.0f * scale_, scale_);
            DrawPickerJump(compositor_, painter_, theme, jump_, jump_layout_, hc);
        } else jump_layout_ = {};
        auto hover_bounds = RendererRect();
        hover_bounds.bottom = chrome_layout_.footer.top;
        renderer_.DrawHoverPreview(hover_preview_.view, hover_bounds, theme, hc);
        if (!jump_.open && !menu_.IsOpen() && !hover_preview_.view.visible) renderer_.DrawTooltip(vm_, RendererRect(), theme);
        if (hover_preview_.view.visible) {
            const UINT interval = renderer_.HoverPreviewFrameMs();
            if (interval) SetTimer(hwnd_, kHoverTimer, interval, nullptr);
            else KillTimer(hwnd_, kHoverTimer);
        }
        PULSE_PICKER_TRACE_POINT("main-end-draw-before");
        const HRESULT end_hr = dc->EndDraw();
        PULSE_PICKER_TRACE_POINT("main-end-draw-after");
        if (end_hr == D2DERR_RECREATE_TARGET || end_hr == DXGI_ERROR_DEVICE_REMOVED ||
            end_hr == DXGI_ERROR_DEVICE_RESET || end_hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
            compositor_.NotifyDeviceLost(end_hr);
            Invalidate();
            return;
        }
        if (spec_.pulse_painted_plain_child && FAILED(end_hr)) { failure_hr_ = end_hr; FailPaintedEdit(ERROR_FUNCTION_FAILED, "end-draw"); return; }
#if defined(PULSE_WITH_TEST_HOOKS) || defined(PULSE_PICKER_INPROCESS_PROOF)
        // Test-only snapshot request. Values up to 0xFFFF are global atoms
        // naming the path, usable when the picker HWND lives in another
        // process; larger values are same-process string pointers.
        if (const HANDLE request = RemovePropW(hwnd_, L"Pulse.PickerTestSnapshot")) {
            const auto value = reinterpret_cast<ULONG_PTR>(request);
            if (value <= 0xFFFF) {
                wchar_t path[256]{};
                if (GlobalGetAtomNameW(static_cast<ATOM>(value), path, 256)) compositor_.SaveSnapshot(path);
                GlobalDeleteAtom(static_cast<ATOM>(value));
            } else {
                compositor_.SaveSnapshot(static_cast<const wchar_t*>(request));
            }
        }
#endif
        PULSE_PICKER_TRACE_POINT("main-present-before");
        const HRESULT present_hr = compositor_.Present();
#ifdef PULSE_WITH_TEST_HOOKS
        if (present_hr == S_OK && view_switch_test_.begin && view_switch_test_.count < view_switch_test_.frames.size()) {
            LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
            view_switch_test_.frames[view_switch_test_.count++] = now.QuadPart;
        }
#endif
        PULSE_PICKER_TRACE_POINT("main-present-after");
        if (preparing_first_frame_) {
            // The HWND is deliberately hidden here. DXGI may report occlusion
            // even though drawing succeeded; visible readiness is checked by
            // FlushPaintedEdits after ShowWindow, not by this preparation pass.
            initial_frame_prepared_ = SUCCEEDED(end_hr) &&
                (present_hr == S_OK || present_hr == DXGI_STATUS_OCCLUDED) && !compositor_.NeedsRecovery();
            if (FAILED(present_hr)) { failure_hr_ = present_hr; FailPaintedEdit(ERROR_FUNCTION_FAILED, "prepare-present"); }
            return;
        }
        if (spec_.pulse_painted_plain_child) {
            first_present_occluded_=present_hr==DXGI_STATUS_OCCLUDED && painted_frame_==0;
            if(first_present_occluded_){PickerProofPhase("first-present-occluded-retry");return;}
            if (present_hr != S_OK) {
                PickerProofResult("strict-present-state-is-recovery", present_hr, ERROR_SUCCESS, compositor_.NeedsRecovery());
                failure_hr_ = present_hr; FailPaintedEdit(ERROR_FUNCTION_FAILED, "strict-present"); return;
            }
            if (!done_ && !compositor_.NeedsRecovery()) ++painted_frame_;
        } else PresentEdits();
        // List hover glides, view morphs, the loading shimmer and thumbnail
        // fades run on the shared renderer's clock.
        const bool moving = renderer_.TickMotion(GetTickCount64()) || filter_t < 1.0f;
        bool use_frame_pump = frame_pump_.Running();
#ifdef PULSE_WITH_TEST_HOOKS
        use_frame_pump = use_frame_pump && !view_switch_test_.timer;
#endif
        if (use_frame_pump) {
            if (moving && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
                if (filter_t >= 1.0f && renderer_.DecorativeMotionOnly()) frame_pump_.ArmPaced(1000.0 / 30.0);
                else frame_pump_.Arm();
            } else frame_pump_.Disarm();
        } else if (moving) SetTimer(hwnd_, kMotionTimer, 16, nullptr);
        else KillTimer(hwnd_, kMotionTimer);
    }

    // ---- hit testing -------------------------------------------------------

    struct Hit {
        int chrome = kPickNone;
        int custom_sub = -1;
        HitTestResult main;
    };

    Hit HitAt(float x, float y) {
        Hit hit;
        if (jump_.open) {
            const int row = HitTestPickerJump(jump_layout_, x, y);
            if (row >= 0) { hit.chrome = kPickJumpRowBase + row; return hit; }
            if (row == kPickerJumpIndexInfoHit) { hit.chrome = kPickJumpIndexInfo; return hit; }
            if (row == -1) { hit.chrome = kPickJumpPanel; return hit; }
        }
        hit.chrome = HitTestPickerChrome(chrome_layout_, chrome_, x, y);
        if (PropertySidebar() && x >= RendererWidth() && y >= kPickerTitleDip * scale_ && y < chrome_layout_.footer.top)
            hit.chrome = kPickNone;
        if (!spec_.custom_controls.empty() && ContainsRect(CustomPanel(), x, y)) {
            for (size_t i = 0; i < spec_.custom_controls.size(); ++i)
                if (PickerCustomInteractive(spec_.custom_controls[i].kind) && EffectiveControl(i) == 3 && ContainsRect(CustomRect(i), x, y)) {
                    hit.chrome = kCustomFocusBase + static_cast<int>(i);
                    hit.custom_sub = -1;
                    if (spec_.custom_controls[i].kind == 6) {
                        for (size_t j = 0; j < spec_.custom_controls[i].items.size(); ++j) {
                            if ((spec_.custom_controls[i].items[j].state & 2) && ContainsRect(RadioRect(i, j), x, y)) {
                                hit.custom_sub = static_cast<int>(j);
                                break;
                            }
                        }
                    }
                }
            return hit;
        }
        if (hit.chrome != kPickNone) return hit;
        // The title strip and the footer belong to the picker alone.
        if (y < renderer_.TitleBarHeight() || y >= chrome_layout_.footer.top) return hit;
        SyncVm();
        hit.main = renderer_.HitTest(vm_, RendererRect(), x, y);
        return hit;
    }

    Hit HitAt(LPARAM lparam) {
        return HitAt(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    bool SidebarResizeAt(float x,float y) const {
        const auto sidebar=renderer_.SidebarRect(RendererWidth(),Height());
        return std::abs(x-sidebar.right)<=4.0f*scale_ && y>=sidebar.top && y<sidebar.bottom;
    }

    void ResizeLayout(float x) {
        if (sidebar_resizing_) {
            const auto sidebar=renderer_.SidebarRect(RendererWidth(),Height());
            const float maximum=std::max(kSidebarMinWidthDip,RendererWidth()/scale_-320.0f);
            renderer_.SetSidebarWidthDip(std::clamp((x-sidebar.left)/scale_,kSidebarMinWidthDip,maximum));
            layout_settings_.sidebar_width=renderer_.SidebarWidthDip();
        } else if (column_resizing_>=0) {
            layout_settings_.column_widths=renderer_.ResizeDetailsColumnDivider(
                PaneBounds(),layout_settings_.column_widths,column_resizing_,x,vm_.pane.cloud_status_column);
        }
        Relayout(); Invalidate();
    }

    void DismissHover(const char* reason = "hover-input") {
        if (!hover_preview_.view.path.empty()) PickerProofPhase(reason);
        if (hwnd_) KillTimer(hwnd_, kHoverTimer);
        hover_preview_.Cancel();
        hover_started_ = 0;
        vm_.tooltip_text.clear();
        if (hwnd_ && compositor_.Dc()) {
            renderer_.DrawHoverPreview({}, RendererRect(), MakeTheme(dark_, accent_), IsHighContrast());
            if (!done_ && IsWindowVisible(hwnd_)) Invalidate();
        }
    }

    void HoverTick() {
        const auto foreground = GetForegroundWindow();
        if (done_ || waiting_ || validating_ || operating_ || GetCapture() || menu_.IsOpen() ||
            !IsWindowVisible(hwnd_) || IsIconic(hwnd_) ||
            (foreground != GetAncestor(hwnd_, GA_ROOT) && foreground != GetAncestor(hwnd_, GA_ROOTOWNER))) {
            DismissHover("hover-inactive"); return;
        }
        if (spec_.preview_trigger == 0) {
            hover_preview_.Tick(renderer_, hwnd_, kHoverListingMessage, options_);
        } else if (hover_started_ && GetTickCount64() - hover_started_ >= 500) {
            vm_.tooltip_text = hover_preview_.view.name;
            vm_.tooltip_x = static_cast<float>(hover_point_.x);
            vm_.tooltip_y = static_cast<float>(hover_point_.y);
            KillTimer(hwnd_, kHoverTimer);
        }
        Invalidate();
    }

    void UpdateHover(const Hit& hit) {
        const int region = static_cast<int>(hit.main.region);
        const bool changed = chrome_.hover != hit.chrome || custom_hover_sub_ != hit.custom_sub ||
                             vm_.hover_region != region ||
                             vm_.hover_control_index != hit.main.index ||
                             vm_.hover_sub_index != hit.main.sub_index;
        chrome_.hover = hit.chrome;
        custom_hover_sub_ = hit.custom_sub;
        vm_.hover_region = region;
        vm_.hover_control_index = hit.main.index;
        vm_.hover_sub_index = hit.main.sub_index;
        if (hit.main.region == HitTestResult::Row && hit.main.index >= 0 &&
            static_cast<size_t>(hit.main.index) < entries_.size() && !GetCapture() && !waiting_ && !InArchive()) {
            const auto& entry = entries_[static_cast<size_t>(hit.main.index)];
            if (hover_preview_.view.path != entry.path) {
                DismissHover();
                SyncVm();
                const DWORD attrs = snapshot_ && static_cast<size_t>(hit.main.index) < snapshot_->size()
                    ? (*snapshot_)[static_cast<size_t>(hit.main.index)].attrs : 0;
                hover_preview_.Candidate(entry, attrs, renderer_.ItemRectInPane(vm_.pane, PaneBounds(), hit.main.index),
                    hover_point_, hwnd_, spec_.hover_preview_delay_ms, spec_.hover_card_size);
                PickerProofPhase("hover-candidate");
                hover_started_ = GetTickCount64();
                SetTimer(hwnd_, kHoverTimer, 50, nullptr);
            }
        } else if (!hover_preview_.view.path.empty()) DismissHover("hover-nonrow");
        if (hit.chrome >= kPickOpenFolderBase && hit.chrome < kPickJumpPanel) {
            const auto folders = OpenFolders();
            const auto index = static_cast<size_t>(hit.chrome - kPickOpenFolderBase);
            if (index < folders.size()) vm_.tooltip_text = path::FriendlyPathText(folders[index].path);
        } else if (hit.chrome == kPickLastLocation) {
            vm_.tooltip_text = spec_.last_location_source;
            if (!vm_.tooltip_text.empty()) vm_.tooltip_text += L"\n";
            vm_.tooltip_text += path::FriendlyPathText(spec_.last_location.path);
        } else if (hit.main.region == HitTestResult::SidebarItem && picker_places::detail::DiskPath(hit.main.path)) {
            vm_.tooltip_text = path::FriendlyPathText(hit.main.path);
        } else if (changed && hit.main.region != HitTestResult::Row) {
            vm_.tooltip_text.clear();
        }
        for (size_t i = 0; i < chrome_layout_.selection_tags.size() && i < selection_tag_paths_.size(); ++i)
            if (ContainsRect(chrome_layout_.selection_tags[i], static_cast<float>(hover_point_.x), static_cast<float>(hover_point_.y)))
                vm_.tooltip_text = path::FriendlyPathText(selection_tag_paths_[i]);
        if (hit.chrome >= kPickFormatBase && hit.chrome < kPickPulseFileBase) {
            const size_t segment = static_cast<size_t>(hit.chrome - kPickFormatBase);
            if (segment < chrome_.formats.size() && chrome_.formats[segment].filter >= 0) {
                const size_t index = static_cast<size_t>(chrome_.formats[segment].filter);
                if (index < options_.filters.size()) {
                    vm_.tooltip_text = options_.filters[index].label + L"\n" + PickerFormatNote(options_.filters[index]);
                }
            }
        } else if (hit.chrome == kPickFormatNote || (chrome_.mode == PickerMode::Save && hit.chrome == kPickFilter)) {
            vm_.tooltip_text = chrome_.filter_static ? chrome_.format_static_text : chrome_.filter_text;
            if (!chrome_.format_note.empty()) vm_.tooltip_text += L"\n" + chrome_.format_note;
        }
        if (mode_ == PickerMode::Save && ContainsRect(chrome_layout_.summary,
            static_cast<float>(hover_point_.x), static_cast<float>(hover_point_.y))) {
            vm_.tooltip_text = path::FriendlyPathText(save_draft_path_);
            if (!chrome_.save_space_text.empty()) vm_.tooltip_text += L"\n" + chrome_.save_space_text;
            if (!chrome_.summary_warning.empty()) vm_.tooltip_text += L"\n" + chrome_.summary_warning;
        } else if (mode_ == PickerMode::Save && chrome_.live_conflict && ContainsRect(chrome_layout_.conflict_text,
            static_cast<float>(hover_point_.x), static_cast<float>(hover_point_.y))) {
            vm_.tooltip_text = chrome_.live_conflict_text;
            if (!chrome_.save_space_text.empty()) vm_.tooltip_text += L"\n" + chrome_.save_space_text;
        }
        if (!vm_.tooltip_text.empty()) {
            vm_.tooltip_x = static_cast<float>(hover_point_.x);
            vm_.tooltip_y = static_cast<float>(hover_point_.y);
        }
        if (hit.chrome == kPickTrayRemoveAfterOpen) {
            vm_.tooltip_text = l10n::Get(l10n::StringId::PickerTrayRemoveAfterOpenHint);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.chrome == kPickSortSync) {
            vm_.tooltip_text = l10n::Get(l10n::StringId::PickerSortSyncTip);
            if (chrome_.sort_synced) vm_.tooltip_text += L"\n" + l10n::Get(l10n::StringId::PickerSortSyncSame);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.chrome == kPickPreviewToggle) {
            vm_.tooltip_text = l10n::Get(l10n::StringId::PickerPreviewPaneTip);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.chrome == kPickSaveOrigin && !chrome_.save_original_location.path.empty()) {
            vm_.tooltip_text = path::FriendlyPathText(chrome_.save_original_location.path);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.chrome == kPickSaveRepoRoot && !chrome_.save_repo_root.path.empty()) {
            vm_.tooltip_text = path::FriendlyPathText(chrome_.save_repo_root.path);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.main.region == HitTestResult::DetailsSaveAfterShow || hit.main.region == HitTestResult::DetailsSaveAfterCopy) {
            vm_.tooltip_text = l10n::Get(pulse_places_.save_followup_available ?
                l10n::StringId::PickerSaveAfterHint : l10n::StringId::PickerSaveAfterUnavailable);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.main.region == HitTestResult::SidebarHeaderAction && hit.main.sidebar_action == SidebarAddAction::PinToApp && !spec_.host_app_name.empty()) {
            vm_.tooltip_text = FillPickerText(l10n::Get(IsPathPinnedToApp(current_) ? l10n::StringId::PickerUnpinFromApp : l10n::StringId::PickerPinToApp), spec_.host_app_name);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.main.region == HitTestResult::PaneDetails) {
            vm_.tooltip_text = l10n::Get(l10n::StringId::ViewDetails);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.main.region == HitTestResult::PaneMediumIcons) {
            vm_.tooltip_text = l10n::Get(l10n::StringId::ViewLarge);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.main.path == TrayToken(0) + L":save_to_tray") {
            vm_.tooltip_text = l10n::Get(l10n::StringId::PickerSaveToTrayTip);
            vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
        } else if (hit.main.path.starts_with(kPickerTrayPrefix)) {
            const auto item = std::find_if(tray_display_items_.begin(), tray_display_items_.end(), [&](const auto& row) { return row.first == hit.main.path; });
            if (item != tray_display_items_.end()) {
                vm_.tooltip_text = item->second.path;
                vm_.tooltip_x = static_cast<float>(hover_point_.x); vm_.tooltip_y = static_cast<float>(hover_point_.y);
            }
        }
        if (changed) Invalidate();

    }

    // ---- scrolling ---------------------------------------------------------

    void EndMarquee(bool restore) {
        if (!marquee_.pending && !marquee_.active) return;
        if (restore) {
            selected_set_ = marquee_.before;
            selected_ = marquee_.caret; anchor_ = marquee_.anchor;
        }
        marquee_.Reset();
        KillTimer(hwnd_, kMarqueeTimer);
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (restore) { SyncSelectedNames(); UpdateChosen(); Invalidate(); }
    }

    void BeginMarquee(float x, float y) {
        marquee_.Begin(selected_set_, selected_, anchor_, x, y, scroll_, KeyDown(VK_CONTROL), KeyDown(VK_SHIFT), ScrollX());
        focus_ = kPickList;
        if (address_editing_) EndAddressEdit(false);
        SetFocus(hwnd_);
        SetCapture(hwnd_);
    }

    void UpdateMarquee(float x, float y, bool scroll_tick = false) {
        if (!marquee_.pending && !marquee_.active) return;
        const bool first = marquee_.pending;
        if (!marquee_.Move(x, y, (std::max)(1.0f, GetSystemMetrics(SM_CXDRAG) * scale_),
                (std::max)(1.0f, GetSystemMetrics(SM_CYDRAG) * scale_))) return;
        SyncVm();
        const auto list = renderer_.PaneListRect(vm_.pane, PaneBounds());
        if (scroll_tick) {
            const float edge = 16 * scale_;
            const float overflow = y < list.top + edge ? y - list.top - edge :
                y > list.bottom - edge ? y - list.bottom + edge : 0;
            if (overflow != 0) ScrollPaneTo(scroll_ + std::clamp(overflow * 0.3f, -48 * scale_, 48 * scale_));
            if (view_mode_ == ViewMode::List) {
                const float side = x < list.left + edge ? x - list.left - edge :
                    x > list.right - edge ? x - list.right + edge : 0;
                if (side != 0) ScrollPaneXTo(scroll_x_ + std::clamp(side * 0.3f, -48 * scale_, 48 * scale_));
            }
            SyncVm();
        }
        marquee_.hits.clear();
        renderer_.ItemsInRectInPane(vm_.pane, PaneBounds(), marquee_.Rect(scroll_, list, ScrollX()), marquee_.hits);
        if (marquee_.Apply(selected_set_, static_cast<int>(entries_.size())) || first) {
            user_interacted_ = true;
            selected_ = selected_set_.empty() ? -1 : *std::min_element(selected_set_.begin(), selected_set_.end());
            anchor_ = selected_;
            editing_selection_names_ = false;
            SyncSelectedNames(); UpdateChosen();
        }
        SetTimer(hwnd_, kMarqueeTimer, 16, nullptr);
        Invalidate();
    }

    void ScrollPaneTo(float value) {
        scroll_ = value;
        ClampScroll();
        Invalidate();
    }

    void ScrollPaneXTo(float value) {
        if (view_mode_ != ViewMode::List) return;
        scroll_x_ = value;
        ClampScroll();
        Invalidate();
    }

    // The List view's horizontal bar (as the main window's): the list's bottom
    // strip, its thumb the drawn one at full strip height.
    bool HScrollbarGeometry(D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) {
        if (view_mode_ != ViewMode::List) return false;
        SyncVm();
        track = renderer_.PaneListRect(vm_.pane, PaneBounds());
        max_scroll = renderer_.MaxScrollXForPane(vm_.pane, PaneBounds());
        if (max_scroll <= 0.0f || track.right <= track.left) return false;
        const D2D1_RECT_F drawn = fluent::ScrollbarThumbRect(PaneHScrollbarSpec(vm_.pane, track, max_scroll, scale_), scale_);
        if (drawn.right <= drawn.left) return false;
        track.top = track.bottom - 12.0f * scale_;
        thumb = D2D1::RectF(drawn.left, track.top, drawn.right, track.bottom);
        return true;
    }

    void ScrollSidebarTo(float value) {
        SyncVm();
        const float max_scroll = renderer_.SidebarMaxScroll(vm_, RendererWidth(), Height());
        vm_.sidebar_scroll = std::clamp(value, 0.0f, std::max(0.0f, max_scroll));
        Invalidate();
    }

    bool ScrollbarGeometry(bool sidebar, D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& max_scroll) {
        SyncVm();
        return sidebar ? renderer_.SidebarScrollbarGeometry(vm_, RendererWidth(), Height(), track, thumb, max_scroll)
                       : renderer_.PaneScrollbarGeometry(vm_.pane, PaneBounds(), track, thumb, max_scroll);
    }

    void BeginHScrollbarDrag(float x) {
        D2D1_RECT_F track{}, thumb{};
        float max_scroll = 0.0f;
        if (!HScrollbarGeometry(track, thumb, max_scroll)) return;
        if (x < thumb.left || x >= thumb.right) {
            const float travel = std::max(1.0f, (track.right - track.left) - (thumb.right - thumb.left));
            ScrollPaneXTo(std::clamp((x - track.left - (thumb.right - thumb.left) * 0.5f) *
                                     max_scroll / travel, 0.0f, max_scroll));
        }
        scrollbar_horizontal_ = true;
        scrollbar_sidebar_ = false;
        scrollbar_drag_y_ = x;
        scrollbar_drag_scroll_ = scroll_x_;
        dragging_scrollbar_ = true;
        SetCapture(hwnd_);
    }

    void BeginScrollbarDrag(const HitTestResult& hit, float y) {
        scrollbar_sidebar_ = hit.sub_index == 2;
        scrollbar_horizontal_ = false;
        D2D1_RECT_F track{}, thumb{};
        float max_scroll = 0.0f;
        if (!ScrollbarGeometry(scrollbar_sidebar_, track, thumb, max_scroll)) return;
        const bool outside = y < thumb.top || y >= thumb.bottom;
        if (outside) {
            // A click on the track jumps the thumb's middle to the pointer.
            const float travel = std::max(1.0f, (track.bottom - track.top) - (thumb.bottom - thumb.top));
            const float value = std::clamp((y - track.top - (thumb.bottom - thumb.top) * 0.5f) *
                                           max_scroll / travel, 0.0f, max_scroll);
            if (scrollbar_sidebar_) ScrollSidebarTo(value); else ScrollPaneTo(value);
        }
        scrollbar_drag_y_ = y;
        scrollbar_drag_scroll_ = scrollbar_sidebar_ ? vm_.sidebar_scroll : scroll_;
        dragging_scrollbar_ = true;
        SetCapture(hwnd_);
    }

    // Pointer position along the dragged bar: x for the horizontal one.
    void DragScrollbar(float x, float y) {
        D2D1_RECT_F track{}, thumb{};
        float max_scroll = 0.0f;
        if (scrollbar_horizontal_) {
            if (!HScrollbarGeometry(track, thumb, max_scroll)) return;
            const float travel = std::max(1.0f, (track.right - track.left) - (thumb.right - thumb.left));
            ScrollPaneXTo(scrollbar_drag_scroll_ + (x - scrollbar_drag_y_) * max_scroll / travel);
            return;
        }
        if (!ScrollbarGeometry(scrollbar_sidebar_, track, thumb, max_scroll)) return;
        const float travel = std::max(1.0f, (track.bottom - track.top) - (thumb.bottom - thumb.top));
        const float value = scrollbar_drag_scroll_ + (y - scrollbar_drag_y_) * max_scroll / travel;
        if (scrollbar_sidebar_) ScrollSidebarTo(value); else ScrollPaneTo(value);
    }

    void Wheel(WPARAM wparam, LPARAM lparam) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &point);
        if ((GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) &&
            ContainsRect(PaneBounds(), static_cast<float>(point.x), static_cast<float>(point.y)) &&
            !marquee_.active && !marquee_.pending) {
            const int step = ConsumeIconZoomWheel(GET_WHEEL_DELTA_WPARAM(wparam), GetTickCount64(),
                zoom_wheel_remainder_, zoom_wheel_tick_);
            if (step) ZoomIcons(step, &point);
            return;
        }
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const float delta = -steps * renderer_.RowHeight() * 3.0f;
        const D2D1_RECT_F sidebar = renderer_.SidebarRect(RendererWidth(), Height());
        if (ContainsRect(sidebar, static_cast<float>(point.x), static_cast<float>(point.y)))
            ScrollSidebarTo(vm_.sidebar_scroll + delta);
        else if (view_mode_ == ViewMode::List)
            ScrollPaneXTo(scroll_x_ - steps * kListWheelDip * scale_);  // the List view's columns run sideways
        else
            ScrollPaneTo(scroll_ + delta);
    }

    // WM_MOUSEHWHEEL over the list: sideways in the List view.
    bool HWheel(WPARAM wparam, POINT point) {
        if (view_mode_ != ViewMode::List ||
            !ContainsRect(PaneBounds(), static_cast<float>(point.x), static_cast<float>(point.y))) return false;
        DismissHover();
        ScrollPaneXTo(scroll_x_ + static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * kListWheelDip * scale_);
        return true;
    }

    // ---- menus -------------------------------------------------------------

    RECT ScreenRect(D2D1_RECT_F r) const {
        POINT top{static_cast<LONG>(r.left), static_cast<LONG>(r.top)};
        ClientToScreen(hwnd_, &top);
        return {top.x, top.y, top.x + static_cast<LONG>(r.right - r.left),
                top.y + static_cast<LONG>(r.bottom - r.top)};
    }

    static FluentMenuItem Item(int command, const std::wstring& text, const wchar_t* glyph = L"",
                               bool enabled = true) {
        FluentMenuItem item;
        item.command = command;
        item.text = text;
        item.glyph = glyph;
        item.enabled = enabled;
        return item;
    }

    static FluentMenuItem Radio(int command, const std::wstring& text, bool on) {
        FluentMenuItem item;
        item.command = command;
        item.text = text;
        item.radio_group = true;
        item.radio = on;
        return item;
    }

    void RestoreMenuFocus(HWND previous) {
        if (done_ || !IsWindow(hwnd_) ||
            GetWindowThreadProcessId(GetForegroundWindow(), nullptr) != GetCurrentThreadId()) return;
        SetActiveWindow(hwnd_);
        SetFocus(previous && IsChild(hwnd_, previous) && IsWindowVisible(previous) &&
                 IsWindowEnabled(previous) ? previous : hwnd_);
    }

    int TrackPickerPopup(POINT point, std::vector<FluentMenuItem> items,
                         FluentMenu::FilterFn filter = nullptr) {
        DismissHover();
        const HWND previous = GetFocus();
        const int command = menu_.TrackPopup(point, std::move(items), std::move(filter));
        RestoreMenuFocus(previous);
        return command;
    }

    int TrackPickerDropdown(RECT anchor, std::vector<FluentMenuItem> items) {
        DismissHover();
        const HWND previous = GetFocus();
        const int command = menu_.TrackDropdown(anchor, std::move(items));
        RestoreMenuFocus(previous);
        return command;
    }

    int TrackAt(POINT client, std::vector<FluentMenuItem> items) {
        ClientToScreen(hwnd_, &client);
        menu_.SetTheme(dark_, accent_);
        return TrackPickerPopup(client, std::move(items));
    }

    FluentMenuItem SortMenu() const {
        FluentMenuItem sort = Item(0, l10n::Get(l10n::StringId::SortBy).c_str(), L"\xE8CB");
        static constexpr l10n::StringId labels[] = {l10n::StringId::ColumnName, l10n::StringId::ColumnModified,
            l10n::StringId::ColumnSize, l10n::StringId::ColumnType};
        for (int i = 0; i < 4; ++i)
            sort.children.push_back(Radio(10 + i, l10n::Get(labels[i]).c_str(),
                                          order_.column == static_cast<int>(ColumnFromSort(static_cast<PickerSort>(i)))));
        sort.children.push_back(Radio(40, l10n::Get(l10n::StringId::ColumnCreated).c_str(), order_.column == static_cast<int>(SortColumn::Created)));
        sort.children.push_back(Radio(41, l10n::Get(l10n::StringId::ColumnAccessed).c_str(), order_.column == static_cast<int>(SortColumn::Accessed)));
        sort.children.push_back(Radio(42, l10n::Get(l10n::StringId::ColumnPath).c_str(), order_.column == static_cast<int>(SortColumn::Path)));
        sort.children.back().separator_after = true;
        sort.children.push_back(Radio(14, l10n::Get(l10n::StringId::Ascending).c_str(), !order_.descending));
        sort.children.push_back(Radio(15, l10n::Get(l10n::StringId::Descending).c_str(), order_.descending));
        sort.children.back().separator_after = true;
        static constexpr l10n::StringId folder_labels[] = {l10n::StringId::FolderSortTop, l10n::StringId::FolderSortFollow, l10n::StringId::FolderSortMixed};
        for (int i = 0; i < 3; ++i)
            sort.children.push_back(Radio(44 + i, l10n::Get(folder_labels[i]).c_str(), order_.folder_mode == i));
        return sort;
    }

    // Shared group-by choices; tags require a complete catalog from Pulse.
    FluentMenuItem GroupMenu() const {
        FluentMenuItem group = Item(0, l10n::Get(l10n::StringId::GroupBy).c_str(), L"\xF168", !current_.empty());
        static constexpr l10n::StringId kLabels[] = {l10n::StringId::GroupNone, l10n::StringId::GroupByName,
            l10n::StringId::GroupByDate, l10n::StringId::GroupByType, l10n::StringId::GroupBySize,
            l10n::StringId::GroupByTag, l10n::StringId::Location};
        for (int i = 0; i < 7; ++i) {
            group.children.push_back(Radio(30 + i, l10n::Get(kLabels[i]).c_str(), order_.group_by == i));
            if (i == 5) group.children.back().enabled = order_.tags != nullptr;
            if (i == 0) group.children.back().separator_after = true;
        }
        return group;
    }

    FluentMenuItem HiddenItem(int command) const {
        FluentMenuItem hidden = Item(command, l10n::Get(l10n::StringId::ShowHiddenItems).c_str(), L"\xE7B3");
        hidden.toggle = true;
        hidden.checked = options_.show_hidden;
        return hidden;
    }

    void AppendDisplayItems(std::vector<FluentMenuItem>& items) const {
        auto extensions = Item(50, l10n::Get(l10n::StringId::ToolbarToolExtensions));
        extensions.toggle = true; extensions.checked = spec_.show_file_extensions;
        items.push_back(std::move(extensions));
        auto system = Item(51, l10n::Get(l10n::StringId::SettingsShowProtected));
        system.toggle = true; system.checked = options_.show_protected_os_files;
        items.push_back(std::move(system));
    }

    bool HandleDisplayCommand(int command) {
        if (command == 50) {
            spec_.show_file_extensions = !spec_.show_file_extensions;
            SyncVm(); Relayout(); Invalidate();
        } else if (command == 51) {
            options_.show_protected_os_files = !options_.show_protected_os_files;
            Reload();
        } else return false;
        return true;
    }

    bool HandleSortCommand(int command) {
        PickerListOrder next = order_;
        if (command >= 10 && command <= 13) next.column = static_cast<int>(ColumnFromSort(static_cast<PickerSort>(command - 10)));
        else if (command == 14 || command == 15) next.descending = command == 15;
        else if (command >= 30 && command <= 36) next.group_by = command - 30;
        else if (command == 40) next.column = static_cast<int>(SortColumn::Created);
        else if (command == 41) next.column = static_cast<int>(SortColumn::Accessed);
        else if (command == 42) next.column = static_cast<int>(SortColumn::Path);
        else if (command >= 44 && command <= 46) next.folder_mode = command - 44;
        else return false;
        SetListOrder(next);
        return true;
    }

    // ---- list order: the main window's sort and group-by for each folder ----

    static std::wstring ListOrderKey(const std::wstring& path) {
        std::wstring key = path;
        while (key.size() > 3 && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
        for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));
        return key;
    }

    // Resolves the order for `path` and hands the loader its comparator.
    // A choice made in this dialog wins for that folder until it closes.
    void BindListOrder(const std::wstring& path, bool moved) {
        if (moved) collapsed_list_groups_.clear();
        const auto chosen = order_overrides_.find(ListOrderKey(path));
        if (chosen != order_overrides_.end()) order_ = chosen->second;
        else if (spec_.folder_order && !path.empty() && !picker_shell::IsParsingName(path)) order_ = spec_.folder_order(path);
        else {
            order_ = {};
            order_.column = static_cast<int>(ColumnFromSort(options_.sort));
            order_.descending = options_.descending;
            order_.group_by = default_group_by_;
        }
        order_.group_by = app::PickerSupportedGroup(order_.group_by);
        options_.order_entries = [order = order_, folder = path](std::vector<PickerEntry>& entries, const std::vector<fs::DirEntry>& rows) {
            app::OrderPickerEntries(entries, rows, folder, order);
        };
        ++list_groups_rev_;
    }

    void SetListOrder(PickerListOrder next) {
        next.group_by = app::PickerSupportedGroup(next.group_by);
        if (next.group_by == static_cast<int>(app::GroupBy::Tag) && !next.tags) return;
        order_overrides_[ListOrderKey(current_)] = next;
        // Without the main window's order, the dialog's own memory (per mode).
        if (!spec_.folder_order) {
            options_.sort = SortFromColumn(static_cast<SortColumn>(next.column));
            options_.descending = next.descending;
            default_group_by_ = next.group_by;
        }
        Reload();
    }

    void SyncListGroups(PaneViewModel& pane) {
        pane.group_by = 0;
        pane.groups.reset();
        if (!snapshot_ || current_.empty() || !snapshot_order_.group_by) return;
        if (list_groups_snapshot_ != snapshot_.get() || list_groups_built_rev_ != list_groups_rev_) {
            list_groups_ = app::BuildPickerListGroups(*snapshot_, snapshot_order_, collapsed_list_groups_, current_);
            list_groups_snapshot_ = snapshot_.get();
            list_groups_built_rev_ = list_groups_rev_;
        }
        if (!list_groups_) return;
        pane.group_by = snapshot_order_.group_by;
        pane.groups = list_groups_;
    }

    void ToggleListGroup(int index) {
        if (!list_groups_ || index < 0 || index >= static_cast<int>(list_groups_->size())) return;
        const std::wstring key = (*list_groups_)[static_cast<size_t>(index)].key;
        if (!collapsed_list_groups_.erase(key)) collapsed_list_groups_.insert(key);
        ++list_groups_rev_;
        SyncVm();
        ClampScroll();
        Invalidate();
    }

    void SelectListGroup(int index) {
        if (!allow_multiselect_) { ToggleListGroup(index); return; }
        if (!list_groups_ || index < 0 || index >= static_cast<int>(list_groups_->size())) return;
        const ListGroup group = (*list_groups_)[static_cast<size_t>(index)];
        if (group.count <= 0) return;
        focus_ = kPickList;
        SetFocus(hwnd_);
        Select(group.first, false, false);
        if (group.count > 1) Select(group.first + group.count - 1, true, false);
        Invalidate();
    }

    void ToggleHidden() {
        options_.show_hidden = !options_.show_hidden;
        Reload();
    }

    void SetViewMode(ViewMode mode, float dip = 0, const POINT* pointer = nullptr) {
        dip = NormalizeIconDip(mode, dip, scale_);
        if (view_mode_ == mode && icon_dip_ == dip) return;
        DismissHover();
        SyncVm();
        int anchor = selected_;
        float offset = 0;
        if (pointer) {
            const auto hit = HitAt(static_cast<float>(pointer->x), static_cast<float>(pointer->y));
            if (hit.main.region == HitTestResult::Row) anchor = hit.main.index;
        }
        if (anchor >= 0) offset = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), anchor).top;
        EndMarquee(true);
        if (view_mode_ != mode) scroll_x_ = 0.0f;
        const bool tree_view_changed = (view_mode_ == ViewMode::Details) != (mode == ViewMode::Details);
        view_mode_ = mode;
        icon_dip_ = dip;
        if (IsZoomIconMode(mode)) { last_icon_mode_ = mode; last_icon_dip_ = dip; }
        g_last_view[mode_index_] = mode;
        SyncVm();
        if (anchor >= 0) scroll_ += renderer_.ItemRectInPane(vm_.pane, PaneBounds(), anchor).top - offset;
        ClampScroll();
        if (!pointer && selected_ >= 0) EnsureVisible(selected_);
        if (tree_view_changed) Reload();
        Invalidate();
    }

    void ZoomIcons(int step, const POINT* pointer = nullptr) {
        const auto next = ResolveIconZoom(view_mode_, icon_dip_, last_icon_mode_, last_icon_dip_, step, scale_);
        SetViewMode(next.mode, next.dip, pointer);
    }

    bool HandleViewKey(WPARAM key) {
        if (KeyDown(VK_CONTROL) && KeyDown(VK_SHIFT) && !KeyDown(VK_MENU) && key >= '1' && key <= '8') {
            SetViewMode(static_cast<ViewMode>(key - '1'));
            return true;
        }
        return false;
    }

    // The pane's "…" button: everything the old toolbar row offered.
    void ShowViewMenu(const D2D1_RECT_F& anchor) {
        std::vector<FluentMenuItem> items;
        FluentMenuItem create = Item(1, l10n::Get(l10n::StringId::NewFolder).c_str(), L"\xE8F4",
                                     !InArchive() && !current_.empty() && !waiting_);
        create.shortcut = L"Ctrl+Shift+N";
        create.separator_after = true;
        items.push_back(std::move(create));
        items.push_back(SortMenu());
        items.push_back(GroupMenu());
        FluentMenuItem hidden = HiddenItem(2);
        items.push_back(std::move(hidden));
        AppendDisplayItems(items);
        items.back().separator_after = true;
        const wchar_t* zh[] = {L"超大图标", L"大图标", L"中图标", L"小图标", L"列表", L"详细信息", L"平铺", L"内容"};
        const wchar_t* en[] = {L"Extra large icons", L"Large icons", L"Medium icons", L"Small icons",
                               L"List", L"Details", L"Tiles", L"Content"};
        for (int i = 0; i < 8; ++i)
            items.push_back(Radio(20 + i, l10n::Pick(zh[i], en[i]), static_cast<int>(view_mode_) == i));
        menu_.SetTheme(dark_, accent_);
        const int command = TrackPickerDropdown(ScreenRect(anchor), std::move(items));
        if (done_ || command <= 0) return;
        if (command == 1) NewFolder();
        else if (command == 2) ToggleHidden();
        else if (command >= 20 && command < 28) SetViewMode(static_cast<ViewMode>(command - 20));
        else if (!HandleDisplayCommand(command)) HandleSortCommand(command);
    }

    void ShowFilterMenu() {
        if (chrome_.filter_static) return;
        if (mode_ == PickerMode::Save && chrome_layout_.format_more.right > chrome_layout_.format_more.left)
            ShowFormatList(chrome_layout_.format_more);
        else ShowFilterMenu(chrome_layout_.filter);
    }
    void ShowFilterMenu(const D2D1_RECT_F& anchor) {
        if (options_.filters.empty()) return;
        std::vector<FluentMenuItem> items;
        for (size_t i = 0; i < options_.filters.size(); ++i)
            items.push_back(Radio(static_cast<int>(i) + 1, options_.filters[i].label,
                                  i == options_.filter_index));
        menu_.SetTheme(dark_, accent_);
        SetFilterOpen(true);
        const int command = TrackPickerDropdown(ScreenRect(anchor), std::move(items));
        SetFilterOpen(false);
        if (done_ || command <= 0) return;
        ApplyFilter(static_cast<size_t>(command - 1));
    }

    void ApplyFilter(size_t index) {
        if (!syncing_filename_filter_ && !save_format_notice_.empty()) {
            save_format_notice_.clear(); save_format_notice_filename_.clear();
            chrome_.save_format_notice.clear(); Invalidate();
        }
        if (index >= options_.filters.size() || options_.filter_index == index) return;
        if (mode_ == PickerMode::Save && !syncing_filename_filter_ && !filename_ime_composing_) {
            const auto next = spec_.native_save_defaults && UsesSaveNameMask(options_.filters)
                ? NativeSaveMaskFilename(filename_text_, options_.filters, index, spec_.default_extension, true)
                : RewriteSaveFilenameForFilter(filename_text_, options_.filters, index);
            if (next != filename_text_) {
                DWORD start = 0, end = 0;
                if (filename_edit_) SendMessageW(filename_edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                filename_text_ = next;
                if (filename_edit_) {
                    const bool previous = setting_edits_;
                    setting_edits_ = true;
                    SetWindowTextW(filename_edit_, filename_text_.c_str());
                    setting_edits_ = previous;
                    SendMessageW(filename_edit_, EM_SETSEL, (std::min)(static_cast<size_t>(start), next.size()),
                        (std::min)(static_cast<size_t>(end), next.size()));
                }
            }
        }
        options_.filter_index = index;
        if (mode_ == PickerMode::Save) save_filter_changed_ = true;
        if (mode_ == PickerMode::File) open_filter_changed_ = true;
        UpdateFilterText();
        Reload();
    }

    void SetFilterOpen(bool open) {
        filter_motion_from_ = chrome_.filter_turn;
        filter_open_ = open;
        filter_motion_start_ = motion::SystemAnimationsEnabled() ? motion::NowMs() : 0;
        Invalidate();
    }

    void ManagePlaces(picker_places::ManagePlacesRequest request) {
        if (operating_ || done_) return;
        operating_ = true;
        const bool okay = RunPickerPlacesRequest(hwnd_, [snapshot = pulse_places_, request = std::move(request), mutate = spec_.manage_shared_places] {
            return mutate ? mutate(request) : picker_places::RequestManagePlaces(snapshot, request);
        });
        operating_ = false;
        if (done_) return;
        chrome_.notice = okay ? std::wstring() : l10n::Get(l10n::StringId::PickerPlacesUpdateFailed);
        RefreshSharedPlaces();
    }

    void AddFavorites(std::vector<picker_places::StarredItem> items) {
        if (operating_ || done_ || items.empty()) return;
        operating_ = true;
        const bool okay = RunPickerFavoriteRequest(hwnd_, pulse_places_, std::move(items), {}, spec_.add_shared_favorites);
        operating_ = false;
        if (done_) return;
        chrome_.notice = okay ? std::wstring() : l10n::Get(l10n::StringId::PickerPlacesUpdateFailed);
        RefreshSharedPlaces();
    }

    std::vector<picker_places::StarredItem> SelectedPlaces() const {
        std::vector<int> indices(selected_set_.begin(), selected_set_.end());
        std::sort(indices.begin(), indices.end());
        std::vector<picker_places::StarredItem> items;
        for (int index : indices) if (index >= 0 && index < static_cast<int>(entries_.size())) {
            const auto& entry = entries_[index];
            items.push_back({entry.path, entry.kind == PickerEntryKind::File || entry.kind == PickerEntryKind::Image ? 1u : 2u, {}});
        }
        return items;
    }

    std::wstring AskPlaceName(POINT client, const std::wstring& value, l10n::StringId label) {
        menu_.SetTheme(dark_, accent_);
        menu_.SetInitialFilterText(value);
        menu_.SetSelectAllOnOpen(true);
        menu_.SetFilterPlaceholder(l10n::Get(label).c_str());
        const auto rows = [](const std::wstring& name) {
            return std::vector<FluentMenuItem>{Item(1, l10n::Get(l10n::StringId::ConfirmDefault).c_str(), L"", !name.empty())};
        };
        ClientToScreen(hwnd_, &client);
        const int command = TrackPickerPopup(client, rows(value), rows);
        return !done_ && (command == 1 || menu_.LastFilterCommitted()) ? menu_.LastFilterQuery() : std::wstring();
    }

    void ShowSidebarMenu(const std::wstring& path, POINT point) {
        if (path.starts_with(kPickerTrayPrefix)) return;
        using Action = picker_places::ManagePlacesAction;
        const auto snapshot = pulse_places_;
        const auto hidden = HiddenPlacesOnly();
        const bool removable = IsRemovableCommon(path);
        const int command = TrackAt(point, BuildPickerPlacesMenu(snapshot, path, hidden, snapshot.shared_available, true, spec_.host_app_name,
            IsPathPinnedToApp(path), removable));
        if (done_ || command <= 0) return;
        if (command == 16 || command == 17) { TogglePinPathToApp(path); return; }
        if (command == 18) { ForgetCommon(path); return; }
        const auto group = path.starts_with(kPickerGroupPrefix) ? path.substr(std::size(kPickerGroupPrefix) - 1) : std::wstring();
        if (command == 1 && !path.empty() && std::none_of(hidden.begin(), hidden.end(),
            [&](const auto& item) { return SamePickerPath(item, path); })) layout_settings_.hidden_places.push_back(path);
        else if (command == 2) std::erase_if(layout_settings_.hidden_places, [](const auto& item) { return !item.starts_with(kHiddenGroupPrefix); });
        else if (command >= 200 && command < 200 + static_cast<int>(hidden.size())) {
            const auto& restore = hidden[command - 200];
            std::erase_if(layout_settings_.hidden_places, [&](const auto& item) { return SamePickerPath(item, restore); });
        } else if (command >= 100 && command < 100 + static_cast<int>(snapshot.starred_groups.size()))
            ManagePlaces({Action::SetGroup, {path}, snapshot.starred_groups[command - 100].id, {}});
        else if (command == 3) OpenSharedPlace(path);
        else if (command == 4) CopyTextToClipboard(hwnd_, path::FriendlyPathText(path));
        else if (command == 5) ManagePlaces({Action::Remove, {path}, {}, {}});
        else if (command == 6) AddFavorites({{path, 2, {}}});
        else if (command == 7) ManagePlaces({Action::PinWorkspace, {path}, {}, {}});
        else if (command == 8) ManagePlaces({Action::UnpinWorkspace, {path}, {}, {}});
        else if (command == 10 || command == 11) ManagePlaces({command == 10 ? Action::MoveWorkspaceUp : Action::MoveWorkspaceDown, {path}, {}, {}});
        else if (command == 13) ManagePlaces({Action::DeleteGroup, {}, group, {}});
        else if (command == 15) ManagePlaces({Action::SetGroup, {path}, {}, {}});
        else if (command == 9 || command == 12 || command == 14) {
            std::wstring value = command == 14 ? l10n::Get(l10n::StringId::FavoritesGroupDefaultName) : PickerLeafName(path);
            for (const auto& entry : snapshot.starred_groups) if (entry.id == group) value = entry.name;
            for (const auto& entry : snapshot.workspaces) if (SamePickerPath(entry.root, path)) value = entry.name;
            const auto name = AskPlaceName(point, value, command == 9 ? l10n::StringId::WorkspaceNameLabel : l10n::StringId::FavoritesGroupName);
            if (!name.empty()) ManagePlaces({command == 9 ? Action::RenameWorkspace : command == 12 ? Action::RenameGroup : Action::CreateGroup,
                command == 12 ? std::vector<std::wstring>{} : std::vector<std::wstring>{path}, command == 12 ? group : std::wstring(), name});
        }
        Relayout(); Invalidate();
    }

    void ShowBreadcrumbAncestors(const std::wstring& path, POINT anchor) {
        std::function<bool(const std::wstring&)> allow;
        if (spec_.legacy_browse && navigation_root_.Active())
            allow = [this](const std::wstring& candidate) { return WithinLegacyRoot(candidate); };
        auto built = BuildBreadcrumbAncestors(path, allow);
        if (built.items.empty()) return;
        menu_.SetTheme(dark_, accent_);
        const int command = TrackPickerPopup(anchor, std::move(built.items));
        if (done_ || (spec_.owner_guard && !spec_.owner_guard->Alive())) return;
        if (command > 0 && static_cast<size_t>(command) <= built.paths.size())
            Navigate(built.paths[static_cast<size_t>(command - 1)], true);
    }

    void ShowBreadcrumbChildren(const std::wstring& path, POINT anchor) {
        if (options_.archive && options_.archive->Contains(path)) return;
        std::function<bool(const std::wstring&)> allow;
        if (spec_.legacy_browse && navigation_root_.Active())
            allow = [this](const std::wstring& candidate) { return WithinLegacyRoot(candidate); };
        menu_.SetTheme(dark_, accent_);
        const std::wstring selected = TrackBreadcrumbChildrenMenu(
            menu_, path, anchor,
            [this](POINT pt, std::vector<FluentMenuItem> items, FluentMenu::FilterFn fn) {
                return TrackPickerPopup(pt, std::move(items), std::move(fn));
            },
            std::move(allow));
        if (done_ || (spec_.owner_guard && !spec_.owner_guard->Alive())) return;
        if (!selected.empty()) Navigate(selected, true);
    }

    void ShowAddressMenu(std::wstring path, POINT point) {
        std::vector<FluentMenuItem> items;
        items.push_back(Item(1, l10n::Get(l10n::StringId::EditAddress).c_str(), L"\xE8AC"));
        items.back().shortcut = L"Ctrl+L";
        items.push_back(Item(2, l10n::Get(l10n::StringId::CopyPath).c_str(), L"\xE8C8", !path.empty()));
        const int command = TrackAt(point, std::move(items));
        if (done_) return;
        if (command == 1) BeginAddressEdit();
        else if (command == 2) CopyTextToClipboard(hwnd_, path::FriendlyPathText(path));
    }

    void ShowColumnMenu(POINT point) {
        const auto mask = spec_.explicit_details_columns || spec_.details_columns ? spec_.details_columns : kDetailsColumnsDefault;
        SyncVm();
        const auto columns = renderer_.DetailsColumns(PaneBounds(), vm_.pane);
        uint32_t visible = 0;
        for (int i = 0; i < columns.count; ++i) visible |= 1u << static_cast<unsigned>(columns.kinds[i]);
        const int command = TrackAt(point, app::BuildDetailsColumnMenu(mask, visible));
        if (done_) return;
        const auto next = app::ApplyDetailsColumnCommand(mask, command);
        if (next != mask) {
            spec_.details_columns = next;
            spec_.explicit_details_columns = true;
            renderer_.SetDetailsColumns(next);
            Relayout(); Invalidate();
        }
    }

    void ShowRowMenu(int index, POINT client) {
        if (TreePlaceholder(index)) return;
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        if (selected_set_.find(index) == selected_set_.end()) Select(index);
        const PickerEntry entry = entries_[static_cast<size_t>(index)];
        const bool container = entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive || entry.archive;
        if (InArchive()) {
            std::vector<FluentMenuItem> items;
            items.push_back(Item(1, l10n::Get(container ? l10n::StringId::Open : l10n::StringId::PickerSelect).c_str(), L"\xE838"));
            if (TrackAt(client, std::move(items)) == 1 && !done_) OpenEntry(index);
            return;
        }
        const auto chosen_places = SelectedPlaces();
        const bool all_starred = !chosen_places.empty() && std::all_of(chosen_places.begin(), chosen_places.end(), [&](const auto& item) {
            return std::any_of(pulse_places_.starred_items.begin(), pulse_places_.starred_items.end(), [&](const auto& star) { return SamePickerPath(star.path, item.path); });
        });
        std::vector<FluentMenuItem> items;
        items.push_back(Item(1, container ? l10n::Get(l10n::StringId::Open).c_str()
                                          : l10n::Get(l10n::StringId::PickerSelect).c_str(),
                             container ? L"\xE838" : L"\xE73E"));
        if (!container) items.push_back(Item(2, l10n::Get(l10n::StringId::ShellOpenWith).c_str(), L"\xE7AC"));
        items.back().separator_after = true;
        const bool editable = entry.kind != PickerEntryKind::Drive && !waiting_ && !validating_;
        items.push_back(Item(6, l10n::Get(l10n::StringId::Cut).c_str(), L"\xE8C6", editable));
        items.back().shortcut = L"Ctrl+X";
        items.push_back(Item(7, l10n::Get(l10n::StringId::TrayIntentCopy).c_str(), L"\xE8C8", editable));
        items.back().shortcut = L"Ctrl+C";
        items.push_back(Item(8, l10n::Get(l10n::StringId::Paste).c_str(), L"\xE77F", CanPaste()));
        items.back().shortcut = L"Ctrl+V";
        items.push_back(Item(9, l10n::Get(l10n::StringId::Rename).c_str(), L"\xE8AC",
                             editable && selected_set_.size() == 1));
        items.back().shortcut = L"F2";
        items.back().separator_after = true;
        items.push_back(Item(3, l10n::Get(l10n::StringId::CopyPath).c_str(), L"\xE8C8"));
        items.back().shortcut = L"Ctrl+Shift+C";
        items.push_back(Item(20, l10n::Get(all_starred ? l10n::StringId::Unstar : l10n::StringId::Star).c_str(), L"\xE734", pulse_places_.shared_available));
        if (container && chosen_places.size() == 1)
            items.push_back(Item(21, l10n::Get(l10n::StringId::PinWorkspace).c_str(), L"\xE8F1", pulse_places_.shared_available));
        if (entry.kind != PickerEntryKind::Drive)
            items.push_back(Item(4, l10n::Get(l10n::StringId::Delete).c_str(), L"\xE74D"));
        items.back().separator_after = true;
        items.push_back(Item(5, l10n::Get(l10n::StringId::Properties).c_str(), L"\xE946"));
        const int command = TrackAt(client, std::move(items));
        if (done_ || command <= 0) return;
        switch (command) {
        case 1: OpenEntry(index); break;
        case 2: {
            OPENASINFO info{};
            info.pcszFile = entry.path.c_str();
            info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
            SHOpenWithDialog(hwnd_, &info);
            break;
        }
        case 3: CopySelectionPaths(); break;
        case 4: DeleteSelection(); break;
        case 5: SHObjectProperties(hwnd_, SHOP_FILEPATH, entry.path.c_str(), nullptr); break;
        case 6: CopySelection(true); break;
        case 7: CopySelection(false); break;
        case 8: Paste(); break;
        case 9: RenameSelection(); break;
        case 20:
            if (all_starred) {
                std::vector<std::wstring> paths;
                for (const auto& item : chosen_places) paths.push_back(item.path);
                ManagePlaces({picker_places::ManagePlacesAction::Remove, std::move(paths), {}, {}});
            } else AddFavorites(chosen_places);
            break;
        case 21: ManagePlaces({picker_places::ManagePlacesAction::PinWorkspace, {entry.path}, {}, {}}); break;
        }
    }

    void ShowBlankMenu(POINT client) {
        std::vector<FluentMenuItem> items;
        items.push_back(Item(4, l10n::Get(l10n::StringId::Paste).c_str(), L"\xE77F", CanPaste()));
        items.back().shortcut = L"Ctrl+V";
        items.back().separator_after = true;
        items.push_back(Item(1, l10n::Get(l10n::StringId::NewFolder).c_str(), L"\xE8F4",
                             !InArchive() && !current_.empty() && !waiting_));
        items.push_back(Item(24, l10n::Get(l10n::StringId::NewTextDocument).c_str(), L"\xE8A5", !InArchive() && !current_.empty() && !waiting_));
        items.push_back(Item(2, l10n::Get(l10n::StringId::Refresh).c_str(), L"\xE72C"));
        items.back().separator_after = true;
        items.push_back(SortMenu());
        items.push_back(GroupMenu());
        items.push_back(HiddenItem(3));
        AppendDisplayItems(items);
        items.push_back(Item(20, l10n::Get(l10n::StringId::FavoritesStarHere).c_str(), L"\xE734", pulse_places_.shared_available && !InArchive() && !current_.empty()));
        items.push_back(Item(21, l10n::Get(l10n::StringId::PinWorkspace).c_str(), L"\xE8F1", pulse_places_.shared_available && !InArchive() && !current_.empty()));
        items.push_back(Item(22, l10n::Get(l10n::StringId::CopyPath).c_str(), L"\xE8C8", !current_.empty()));
        items.push_back(Item(23, l10n::Get(l10n::StringId::Undo).c_str(), L"\xE7A7", operation_session_.CanUndo() && !operating_));
        items.back().shortcut = L"Ctrl+Z";
        const int command = TrackAt(client, std::move(items));
        if (done_ || command <= 0) return;
        if (command == 1) NewFolder();
        else if (command == 24) NewItem(false);
        else if (command == 2) Reload();
        else if (command == 3) ToggleHidden();
        else if (HandleDisplayCommand(command)) {}
        else if (command == 4) Paste();
        else if (command == 20) AddFavorites({{current_, 2, {}}});
        else if (command == 21) ManagePlaces({picker_places::ManagePlacesAction::PinWorkspace, {current_}, {}, {}});
        else if (command == 22) CopyTextToClipboard(hwnd_, path::FriendlyPathText(current_));
        else if (command == 23) UndoOperation();
        else HandleSortCommand(command);
    }

    void CopySelectionPaths() {
        std::wstring text;
        for (const auto& item : SelectedPlaces()) {
            if (!text.empty()) text += L"\r\n";
            text += path::FriendlyPathText(item.path);
        }
        if (text.empty()) text = path::FriendlyPathText(current_);
        if (!text.empty()) CopyTextToClipboard(hwnd_, text);
    }

    void UndoOperation() {
        if (operating_ || waiting_ || validating_ || !operation_session_.CanUndo()) return;
        operating_ = true;
        const auto result = operation_session_.Undo(hwnd_, dark_, accent_);
        operating_ = false;
        if (done_) return;
        bool removed_current = false;
        for (const auto& completion : result.completed) {
            if (completion.type != ops::OpType::RecycleDelete && completion.type != ops::OpType::RealDelete) continue;
            for (const auto& source : completion.sources) removed_current |= SamePickerPath(source, current_);
        }
        if (removed_current) Navigate(PickerParent(current_), true);
        else Reload();
        reload_notice_ = result.error;
        chrome_.notice = result.error;
        FocusList();
    }

    std::vector<std::wstring> OperationPaths() const {
        if (new_folder_draft_) return {};
        std::vector<int> indices(selected_set_.begin(), selected_set_.end());
        std::sort(indices.begin(), indices.end());
        std::vector<std::wstring> paths;
        for (int i : indices) {
            if (i >= 0 && i < static_cast<int>(entries_.size()) &&
                entries_[static_cast<size_t>(i)].kind != PickerEntryKind::Drive)
                paths.push_back(entries_[static_cast<size_t>(i)].path);
        }
        return paths;
    }

    void CopySelection(bool cut) {
        if (InArchive()) return;
        if (picker_shell::IsParsingName(current_)) return;
        if (waiting_ || validating_ || operating_) return;
        const auto paths = OperationPaths();
        if (paths.empty()) return;
        chrome_.notice = ops::WriteClipboard(paths, cut)
            ? l10n::Pick(cut ? L"已剪切，进入目标文件夹后粘贴。" : L"已复制，可在目标文件夹粘贴。",
                         cut ? L"Cut. Paste in the destination folder." : L"Copied. Paste in the destination folder.")
            : l10n::Get(l10n::StringId::CouldNotWriteToTheClipboardTryAgain).c_str();
        Invalidate();
    }

    void ReadCutClipboard() {
        ops::ClipboardData clipboard;
        cut_paths_.clear();
        if (ops::ReadClipboard(clipboard) && clipboard.cut) cut_paths_ = std::move(clipboard.paths);
        Invalidate();
    }

    bool CanPaste() const {
        return !InArchive() && !current_.empty() && !picker_shell::IsParsingName(current_) && current_verified_ && !waiting_ && !validating_ && !operating_ &&
               ops::ClipboardHasPasteableFiles();
    }

    void ExecuteOperation(ops::OpRequest request, const ops::ClipboardData* clipboard = nullptr,
                          bool unique_create = false, bool enter_created = false) {
        if (operating_ || done_) return;
        const bool create_item = request.type == ops::OpType::CreateFolder || request.type == ops::OpType::CreateTextFile;
        const auto operation_parent = current_;
        const auto operation_generation = generation_;
        operating_ = true;
        const auto result = operation_session_.Run(hwnd_, std::move(request), dark_, accent_, unique_create);
        operating_ = false;
        if (done_) return;
        std::vector<std::wstring> destinations;
        uint64_t create_task = 0;
        std::vector<std::wstring> moved;
        for (const auto& completion : result.completed) {
            if (completion.refresh_only) continue;
            if (completion.type == ops::OpType::Move)
                moved.insert(moved.end(), completion.sources.begin(), completion.sources.end());
            for (const auto& path : completion.destinations)
                if (SamePickerPath(PickerParent(path), operation_parent)) destinations.push_back(path);
            if (create_item && !destinations.empty()) create_task = completion.task_id;
        }
        if (clipboard && clipboard->cut && std::all_of(clipboard->paths.begin(), clipboard->paths.end(),
            [&](const auto& source) {
                return std::any_of(moved.begin(), moved.end(), [&](const auto& p) { return SamePickerPath(source, p); });
            }))
            ops::CompleteCutClipboard(clipboard->sequence, clipboard->paths);
        if (generation_ != operation_generation || !SamePickerPath(current_, operation_parent)) return;
        if (enter_created && create_task && !destinations.empty()) {
            Navigate(destinations.front(), true);
            FocusList();
            return;
        }
        if (!destinations.empty()) ClearSelection();
        Reload();
        select_after_paths_ = destinations;
        if (create_item && create_task && !destinations.empty()) {
            app::QueueCreateRenameIntent(create_rename_intents_, create_task, current_, generation_);
            app::CompleteCreateRenameIntent(create_rename_intents_, create_task, current_, generation_, destinations.front());
        }
        reload_notice_ = result.error;
        chrome_.notice = result.error;
        FocusList();
    }

    void Paste() {
        if (!CanPaste()) return;
        if (!IsClipboardFormatAvailable(CF_HDROP) && ops::ClipboardHasShellFiles()) {
            const auto operation_parent = current_;
            const auto operation_generation = generation_;
            const auto sequence = GetClipboardSequenceNumber();
            operating_ = true;
            const auto result = operation_session_.RunBackground(hwnd_,
                [operation_parent, sequence](const std::atomic_bool& cancelled) -> std::wstring {
                    const HRESULT hr = ops::PasteShellClipboardAtSequence(operation_parent, sequence, &cancelled);
                    if (SUCCEEDED(hr)) return {};
                    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return l10n::Get(l10n::StringId::Canceled);
                    return HRESULT_FACILITY(hr) == FACILITY_WIN32
                        ? ErrorText(HRESULT_CODE(hr), operation_parent)
                        : l10n::Get(l10n::StringId::OpFailed);
                }, dark_, accent_);
            operating_ = false;
            if (done_ || generation_ != operation_generation || !SamePickerPath(current_, operation_parent)) return;
            // A failed/cancelled Shell transfer can still have produced files.
            // Refresh once, but never replay the transfer or clear its clipboard.
            Reload();
            reload_notice_ = result.error;
            chrome_.notice = result.error;
            FocusList();
            return;
        }
        ops::ClipboardData clipboard;
        if (!ops::ReadClipboard(clipboard) || clipboard.paths.empty()) {
            chrome_.notice = l10n::Get(l10n::StringId::CouldNotReadClipboardFilesTryAgain).c_str();
            Invalidate();
            return;
        }
        ops::OpRequest request;
        request.type = clipboard.cut ? ops::OpType::Move : ops::OpType::Copy;
        request.sources = clipboard.paths;
        request.dest_dir = current_;
        ExecuteOperation(std::move(request), &clipboard);
    }

    void PlaceRenameEdit() {
        if (!rename_edit_ || rename_path_.empty() || rename_index_ < 0) return;
        SyncVm();
        auto field = renderer_.RenameFieldRect(vm_.pane, renderer_.PaneListRect(vm_.pane, PaneBounds()), rename_index_);
        const bool wraps = view_mode_ == ViewMode::MediumIcons || view_mode_ == ViewMode::LargeIcons || view_mode_ == ViewMode::ExtraLargeIcons;
        if (wraps) SetPropW(rename_edit_, L"Pulse.EditWrap", reinterpret_cast<HANDLE>(1));
        else RemovePropW(rename_edit_, L"Pulse.EditWrap");
        // Like the main window, native EDIT stays single-line; the shared
        // presenter owns icon-grid wrapping and maps selection to those lines.
        const float wrap_width = field.right - field.left - 6 * scale_ - 4;
        if (wraps && wrap_width > 1)
            SetPropW(rename_edit_, L"Pulse.EditWrapW", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(std::lround(wrap_width * 64))));
        else RemovePropW(rename_edit_, L"Pulse.EditWrapW");
        const HFONT font = wraps ? rename_font_ : font_;
        if (font && reinterpret_cast<HFONT>(SendMessageW(rename_edit_, WM_GETFONT, 0, 0)) != font)
            SendMessageW(rename_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        const float inset = 3 * scale_;
        SetWindowPos(rename_edit_, HWND_TOP, static_cast<int>(field.left + inset), static_cast<int>(field.top + 2 * scale_),
            std::max(24, static_cast<int>(field.right - field.left - 2 * inset)),
            std::max(18, static_cast<int>(field.bottom - field.top - 4 * scale_)), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void EndRename(bool commit) {
        if (rename_path_.empty()) return;
        const auto path = rename_path_;
        const auto name = rename_edit_ ? ReadEdit(rename_edit_) : std::wstring();
        const bool current = rename_generation_ == generation_ &&
            (SamePickerPath(PickerParent(path), current_) || (tree_rows_ &&
                std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) { return SamePickerPath(entry.path, path); })));
        if (commit && current && (name.empty() || !IsPickerFolderName(name))) {
            chrome_.notice = ErrorText(ERROR_INVALID_NAME, name);
            Invalidate();
            return;
        }
        const bool creating_folder = new_folder_draft_;
        if (creating_folder) {
            new_folder_draft_ = false;
            snapshot_ = std::move(new_folder_snapshot_);
            if (!entries_.empty()) entries_.erase(entries_.begin());
            selected_ = -1; selected_set_.clear(); anchor_ = -1;
        }
        const bool owned_focus = rename_edit_ && GetFocus() == rename_edit_;
        rename_path_.clear();
        rename_index_ = -1;
        ++rename_serial_;
        if (rename_edit_) ShowWindow(rename_edit_, SW_HIDE);
        if (owned_focus && !done_) FocusList();
        if (creating_folder) {
            UpdateChosen(); Invalidate();
            if (commit && current) NewNamedFolder(name);
            return;
        }
        if (!commit || !current || name == PickerLeafName(path)) { Invalidate(); return; }
        ops::OpRequest request;
        request.type = ops::OpType::Rename;
        request.sources = {path}; request.new_name = name;
        ExecuteOperation(std::move(request));
    }

    void RenameSelection() {
        if (InArchive()) return;
        if (picker_shell::IsParsingName(current_)) return;
        if (waiting_ || validating_ || operating_ || selected_set_.size() != 1) return;
        const auto paths = OperationPaths();
        if (paths.size() != 1) return;
        EndRename(false);
        EnsureVisible(selected_);
        if (!rename_edit_) rename_edit_ = MakeEdit(104, L"");
        if (!rename_edit_) return;
        rename_path_ = paths.front(); rename_index_ = selected_; rename_generation_ = generation_; ++rename_serial_;
        const auto name = PickerLeafName(rename_path_);
        SetWindowTextW(rename_edit_, name.c_str());
        PlaceRenameEdit();
        SetFocus(rename_edit_);
        const bool folder = SelectedEntry() && SelectedEntry()->kind == PickerEntryKind::Folder;
        if (!folder) SetPropW(rename_edit_, L"Pulse.EditDimExt", reinterpret_cast<HANDLE>(1));
        else RemovePropW(rename_edit_, L"Pulse.EditDimExt");
        const auto dot = name.find_last_of(L'.');
        SendMessageW(rename_edit_, EM_SETSEL, 0, !folder && dot != std::wstring::npos && dot > 0 ? static_cast<LPARAM>(dot) : -1);
        Invalidate();
    }

    void ShowSearchSetting(HitTestResult::Region region) {
        if (!searching_) BeginSearch();
        const auto layout = LayoutAddressSearch(renderer_.SearchBarRect(RendererWidth()), scale_);
        const bool scope = region == HitTestResult::AddressSearchScope;
        const bool mode = region == HitTestResult::AddressSearchMode || region == HitTestResult::AddressSearchContent;
        std::vector<FluentMenuItem> items;
        if (!mode) items.push_back(Radio(1, l10n::Get(l10n::StringId::ThisFolderNoSubfolders).c_str(), true));
        if (!scope) items.push_back(Radio(2, l10n::Get(l10n::StringId::FileNamesOnlyNoContents).c_str(), true));
        menu_.SetTheme(dark_, accent_);
        TrackPickerDropdown(ScreenRect(scope ? layout.scope : mode ? layout.mode : layout.options), std::move(items));
    }

    void DeleteSelection(bool permanent = false) {
        if (InArchive()) return;
        if (picker_shell::IsParsingName(current_)) return;
        if (waiting_ || validating_ || operating_) return;
        const auto paths = OperationPaths();
        if (paths.empty()) return;
        ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(permanent ? l10n::StringId::PermanentlyDelete : l10n::StringId::Delete);
        confirm.confirm_text = confirm.title;
        confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
        wchar_t message[256]{};
        swprintf_s(message, l10n::Get(permanent ? l10n::StringId::PermanentDeleteManyFormat : l10n::StringId::RecycleConfirmManyFormat).c_str(), paths.size());
        confirm.message = message;
        confirm.danger = permanent;
        confirm.default_choice = ConfirmChoice::Cancel;
        confirm.cancel_event = spec_.cancel_event;
        confirm.items_total = paths.size();
        for (size_t i = 0; i < std::min<size_t>(paths.size(), 5); ++i) confirm.items.push_back(PickerLeafName(paths[i]));
        if (!ShowConfirmDialog(hwnd_, confirm, dark_, accent_) || done_) return;
        ops::OpRequest request;
        request.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
        request.sources = paths;
        ExecuteOperation(std::move(request));
    }

    void NewFolder() {
        if (InArchive()) return;
        if (picker_shell::IsParsingName(current_) || current_.empty() || !current_verified_ ||
            waiting_ || validating_ || operating_ || !loader_) return;
        EndRename(false);
        if (!rename_edit_) rename_edit_ = MakeEdit(104, L"");
        if (!rename_edit_) return;
        if (tree_rows_) {
            tree_loader_.reset(); tree_active_ = 0; tree_pending_.clear();
            tree_.CollapseAll(); RebuildTree();
        }
        const auto name = l10n::Get(l10n::StringId::NewFolder);
        PickerEntry draft;
        draft.name = name;
        draft.path = current_ + (current_.back() == L'\\' ? L"" : L"\\") + name;
        draft.kind = PickerEntryKind::Folder;
        new_folder_snapshot_ = snapshot_;
        auto rows = std::make_shared<std::vector<fs::DirEntry>>();
        if (snapshot_) *rows = *snapshot_;
        fs::DirEntry row;
        row.name = name; row.attrs = FILE_ATTRIBUTE_DIRECTORY; row.is_dir = true;
        rows->insert(rows->begin(), std::move(row));
        snapshot_ = std::move(rows);
        entries_.insert(entries_.begin(), std::move(draft));
        new_folder_draft_ = true;
        selected_ = anchor_ = 0; selected_set_.clear();
        rename_path_ = entries_.front().path; rename_index_ = 0;
        rename_generation_ = generation_; ++rename_serial_;
        SetWindowTextW(rename_edit_, name.c_str());
        RemovePropW(rename_edit_, L"Pulse.EditDimExt");
        EnsureVisible(0); PlaceRenameEdit(); SetFocus(rename_edit_);
        SendMessageW(rename_edit_, EM_SETSEL, 0, -1);
        Invalidate();
    }

    void NewNamedFolder(const std::wstring& name) {
        if (InArchive()) return;
        if (picker_shell::IsParsingName(current_) || current_.empty() || !current_verified_ ||
            waiting_ || validating_ || operating_ || !loader_) return;
        if (!IsPickerFolderName(name)) {
            chrome_.notice = ErrorText(ERROR_INVALID_NAME, name);
            Invalidate();
            return;
        }
        EndRename(false);
        ops::OpRequest request;
        request.type = ops::OpType::CreateFolder;
        request.sources = {current_ + (current_.back() == L'\\' ? L"" : L"\\") + name};
        ExecuteOperation(std::move(request), nullptr, false, true);
    }

    void NewItem(bool folder) {
        if (InArchive()) return;
        if (picker_shell::IsParsingName(current_)) return;
        if (current_.empty() || waiting_ || validating_ || operating_ || !loader_) return;
        EndRename(false);
        ops::OpRequest request;
        request.type = folder ? ops::OpType::CreateFolder : ops::OpType::CreateTextFile;
        const auto name = l10n::Get(folder ? l10n::StringId::NewFolder : l10n::StringId::NewTextDocument) +
            (folder ? L"" : L".txt");
        request.sources = {current_ + (current_.back() == L'\\' ? L"" : L"\\") + name};
        ExecuteOperation(std::move(request), nullptr, true);
    }

    // ---- focus and keys ----------------------------------------------------

    std::vector<int> FocusOrder() const {
        if (chrome_.overwrite_pending) return {kPickRename, kPickOverwriteReplace, kPickCancel};
        std::vector<int> order = {kPickAddress, kPickSearch, kPickList};
        if (chrome_.pulse_selection_enabled && !chrome_.pulse_selection.empty()) {
            for (size_t i = 0; i < chrome_layout_.pulse_items.size(); ++i)
                if (chrome_.pulse_selection[i].enabled && chrome_layout_.pulse_items[i].right > chrome_layout_.pulse_items[i].left)
                    order.push_back(kPickPulseFileBase + static_cast<int>(i));
            if (chrome_layout_.pulse_more.right > chrome_layout_.pulse_more.left) order.push_back(kPickPulseMore);
            order.push_back(kPickPulseUse);
        }
        for (size_t i = 0; i < chrome_layout_.context_filters.size() && i < chrome_.context_filters.size(); ++i)
            if (chrome_layout_.context_filters[i].right > chrome_layout_.context_filters[i].left)
                order.push_back(chrome_.context_filters[i].id);
        if (!chrome_.selected_names.empty()) {
            for (size_t i = 0; i < chrome_layout_.selection_tags.size(); ++i) order.push_back(kPickRemoveSelectionBase + static_cast<int>(i));
            if (!chrome_layout_.selection_more_text.empty()) order.push_back(kPickSelectionMore);
            order.push_back(kPickClearSelection);
        }
        if (filename_edit_) {
            if (IsWindowVisible(filename_edit_) && IsWindowEnabled(filename_edit_)) order.push_back(kPickFilename);
            if (FormatSegmentsShown()) {
                if (!chrome_.filter_static) {
                    for (size_t i = 0; i < chrome_layout_.format_segments.size(); ++i)
                        if (chrome_layout_.format_segments[i].right > chrome_layout_.format_segments[i].left)
                            order.push_back(kPickFormatBase + static_cast<int>(i));
                    if (chrome_layout_.format_more.right > chrome_layout_.format_more.left) order.push_back(kPickFilter);
                }
            } else if (!chrome_.filter_static) order.push_back(kPickFilter);
        }
        if (chrome_.save_actions_enabled) {
            for (size_t i = 0; i < chrome_layout_.suggestion_items.size(); ++i)
                if (chrome_layout_.suggestion_items[i].right > chrome_layout_.suggestion_items[i].left) order.push_back(kPickSaveSuggestionBase + static_cast<int>(i));
            if (chrome_layout_.suggestions_more.right > chrome_layout_.suggestions_more.left) order.push_back(kPickSaveSuggestionsMore);
            if (chrome_.live_conflict) { order.push_back(kPickLiveRename); order.push_back(kPickLiveSave); }
        }
        if (CompactCustom()) {
            for (size_t i = 0; i < chrome_layout_.option_chips.size() && i < chrome_.options.size(); ++i)
                if (chrome_.options[i].enabled) order.push_back(chrome_.options[i].id);
            if (chrome_layout_.option_chips.size() < chrome_.options.size()) order.push_back(kPickMoreOptions);
        } else for (size_t i = 0; i < spec_.custom_controls.size(); ++i)
            if (PickerCustomInteractive(spec_.custom_controls[i].kind) && EffectiveControl(i) == 3) order.push_back(kCustomFocusBase + static_cast<int>(i));
        if (chrome_.footer_tool.enabled && chrome_layout_.footer_tool.right > chrome_layout_.footer_tool.left)
            order.push_back(chrome_.footer_tool.id);
        order.push_back(kPickCancel);
        if (chrome_.primary_enabled) { order.push_back(kPickPrimary); if(chrome_.primary_drop_down) order.push_back(kPickPrimaryDropDown); }
        if (legacy_child_.Height(hwnd_)) order.push_back(kLegacyChildFocus);
        return order;
    }
    bool LegacyDialogMessage(MSG& message) {
        int direction = 0;
        if (legacy_child_.TabExit(hwnd_, message, direction)) { focus_ = kLegacyChildFocus; MoveFocus(direction); return true; }
        return legacy_child_.DialogMessage(hwnd_, message);
    }

    void MoveFocus(int direction) {
        const auto order = FocusOrder();
        const auto it = std::find(order.begin(), order.end(), focus_);
        int index = it == order.end() ? 0 : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) % static_cast<int>(order.size());
        const int next = order[static_cast<size_t>(index)];
        PickerProofFocus("tab-transition", focus_, next);
        chrome_.show_focus = true;
        if (address_editing_ && next != kPickAddress) EndAddressEdit(false);
        if (next == kPickAddress) { BeginAddressEdit(); return; }
        if (next == kPickSearch) { BeginSearch(); return; }
        if (next == kPickFilename) { FocusFilename(); return; }
        if (next == kLegacyChildFocus) { focus_ = next; legacy_child_.Focus(hwnd_, direction < 0); return; }
        focus_ = next;
        if (next >= kCustomFocusBase && !CompactCustom()) {
            const auto rect = CustomRect(static_cast<size_t>(next - kCustomFocusBase));
            const auto panel = CustomPanel();
            if (rect.top < panel.top) ScrollCustom(rect.top - panel.top);
            else if (rect.bottom > panel.bottom) ScrollCustom(rect.bottom - panel.bottom);
        }
        if(next>=kCustomFocusBase && static_cast<size_t>(next-kCustomFocusBase)<custom_edits_.size() && custom_edits_[next-kCustomFocusBase])
            SetFocus(custom_edits_[next-kCustomFocusBase]);
        else SetFocus(hwnd_);
        Invalidate();
    }

    void Activate(int id) {
        if (id == kPickSaveSuggestionsMore) { ShowSaveSuggestions(); return; }
        if (id >= kPickSaveSuggestionBase && id < kPickSaveSuggestionBase + 4) { ApplySaveSuggestion(static_cast<size_t>(id - kPickSaveSuggestionBase)); return; }
        if (id == kPickLiveSave) {
            if (chrome_.live_conflict && chrome_.save_actions_enabled) SubmitSelection(true);
            return; // final validation/overwrite snapshot and FileOk remain mandatory
        }
        if (id == kPickLiveRename) {
            if (!chrome_.live_conflict || !chrome_.save_actions_enabled) return;
            for (size_t i = 0; i < save_name_suggestions_.size(); ++i)
                if (save_name_suggestions_[i].kind == SaveNameSuggestionKind::Version) { ApplySaveSuggestion(i); return; }
            const auto target = ResolveSaveTarget(SaveDraftRequest());
            if (target.state == SaveTargetState::Resolved) {
                const auto parent = PickerParent(target.path), leaf = OverwriteAlternative(target.path);
                SetFilename(SamePickerPath(parent, current_) ? leaf : parent + L"\\" + leaf);
                InputChanged(filename_edit_); FocusFilename(); Relayout(); Invalidate();
            }
            return;
        }
        if (id == kPickPulseUse) { UsePulseSelection(); return; }
        if (id == kPickPulseMore) { ShowPulseSelectionMenu(); return; }
        if (id >= kPickPulseFileBase && id < kPickPulseFileBase + 64) { UsePulseSelection(id - kPickPulseFileBase); return; }
        if (id == kPickJumpIndexInfo) {
            ConfirmDialogSpec info;
            info.title = l10n::Get(l10n::StringId::PickerIndexLearn);
            info.message = l10n::Get(l10n::StringId::PickerUserIndexExplanation);
            info.confirm_text = l10n::Get(l10n::StringId::Close);
            info.cancel_text = l10n::Get(l10n::StringId::Cancel);
            info.cancel_event = spec_.cancel_event;
            ShowConfirmDialog(hwnd_, info, dark_, accent_);
            return;
        }
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
        test_last_activate_ = id;
#endif
        if (id == kPickMoreOptions) { ShowMoreOptions(); return; }
        if (id >= kPickContextFilterBase && id < kPickContextFilterBase + 64) {
            if (!chrome_.overwrite_pending) ApplyFilter(static_cast<size_t>(id - kPickContextFilterBase));
            return;
        }
        if (id == kPickFilterMore) {
            for (size_t i = 0; i < chrome_.context_filters.size() && i < chrome_layout_.context_filters.size(); ++i)
                if (chrome_.context_filters[i].id == id) { ShowFilterMenu(chrome_layout_.context_filters[i]); break; }
            return;
        }
        if (id == kPickCompactFilters) {
            for (size_t i = 0; i < chrome_.context_filters.size() && i < chrome_layout_.context_filters.size(); ++i)
                if (chrome_.context_filters[i].id == id) { ShowCompactFilters(chrome_layout_.context_filters[i]); break; }
            return;
        }
        if (id == kPickSortSync) { ShowSortSyncMenu(chrome_layout_.sort_sync); return; }
        if (id == kPickSaveFilterAll) {
            if (options_.save_same_format_only) { options_.save_same_format_only = false; Reload(); }
            return;
        }
        if (id == kPickSaveFilterSame) {
            if (!options_.save_same_format_only) { options_.save_same_format_only = true; Reload(); }
            return;
        }
        if (id == kPickTrayRemoveAfterOpen && TrayConsumeAvailable()) {
            picker_flags_ ^= picker_layout::kFlagRemoveTrayAfterOpen;
            Relayout(); Invalidate();
            return;
        }
        if (id == kPickSaveOrigin && !chrome_.save_original_location.path.empty()) {
            Navigate(chrome_.save_original_location.path, true);
            return;
        }
        if (id == kPickSaveRepoRoot && !chrome_.save_repo_root.path.empty()) {
            Navigate(chrome_.save_repo_root.path, true);
            return;
        }
        if (id == kPickTimeMenu) {
            for (size_t i = 0; i < chrome_.context_filters.size() && i < chrome_layout_.context_filters.size(); ++i)
                if (chrome_.context_filters[i].id == id) { ShowTimeMenu(chrome_layout_.context_filters[i]); break; }
            return;
        }
        if (id == kPickPreviewToggle) { TogglePreviewPane(); return; }
        if (id >= kPickCategoryBase && id < kPickCategoryBase + static_cast<int>(kPickerCategoryCount)) { SetCategory(id - kPickCategoryBase); return; }
        if (id >= kPickFormatBase && id < kPickFormatBase + 64) {  // inside the selection-tag range, so first
            if (!chrome_.overwrite_pending) ActivateFormat(id - kPickFormatBase);
            return;
        }
        // The jump strip (its chip ids sit inside the selection-tag range, so first).
        if (id == kPickOpenScrollLeft || id == kPickOpenScrollRight) {
            const float vp_dip = (chrome_layout_.open_viewport.right - chrome_layout_.open_viewport.left) / std::max(0.5f, scale_);
            const float step = std::max(140.0f, vp_dip * 0.6f);
            ScrollOpenStrip(id == kPickOpenScrollLeft ? -step : step);
            return;
        }
        if (id >= kPickOpenFolderBase && id < kPickJumpPanel) { JumpToOpenFolder(static_cast<size_t>(id - kPickOpenFolderBase)); return; }
        if (id == kPickLastLocation) { if (!spec_.last_location.path.empty()) Navigate(spec_.last_location.path, true); return; }
        if (id == kPickUndoJump) { UndoAutoJump(); return; }
        if (id == kPickNewFolder) { NewFolder(); return; }
        if (id == kPickClearSelection) {
            if (validating_) return;
            basket_.clear(); selected_set_.clear(); last_file_picks_.clear();
            selected_ = anchor_ = -1; editing_selection_names_ = false;
            SyncSelectedNames(); focus_ = kPickList; SetFocus(hwnd_);
            Relayout(); Invalidate(); return;
        }
        if (id == kPickSelectionMore) {
            std::vector<FluentMenuItem> items;
            const auto paths = selection_tag_paths_;
            for (size_t i = 0; i < paths.size(); ++i) {
                auto item = Item(static_cast<int>(i + 1), chrome_.selected_names[i] + L"  ·  " + chrome_.selected_sources[i], L"\xE711");
                const auto parent = PickerParent(paths[i]);
                const auto parent_label = PickerLeafName(parent);
                item.detail = (parent_label.empty() ? L"" : parent_label + L" · ") + path::FriendlyPathText(parent);
                item.tooltip = path::FriendlyPathText(paths[i]);
                items.push_back(std::move(item));
            }
            const int command = TrackPickerDropdown(ScreenRect(chrome_layout_.selection_more), std::move(items));
            if (done_ || command <= 0 || static_cast<size_t>(command) > paths.size()) return;
            const auto& path = paths[static_cast<size_t>(command - 1)];
            std::erase_if(basket_, [&](const auto& item) { return SamePickerPath(item.path, path); });
            for (auto it = selected_set_.begin(); it != selected_set_.end();) {
                if (*it >= 0 && static_cast<size_t>(*it) < entries_.size() && SamePickerPath(entries_[*it].path, path)) it = selected_set_.erase(it);
                else ++it;
            }
            editing_selection_names_ = false; SyncSelectedNames(); Relayout(); Invalidate(); return;
        }
        if (id >= kPickRemoveSelectionBase && id < kCustomFocusBase) {
            const auto index = static_cast<size_t>(id - kPickRemoveSelectionBase);
            if (index < selection_tag_indices_.size()) {
                const int tagged = selection_tag_indices_[index];
                if (tagged < 0) {
                    const auto k = static_cast<size_t>(-1 - tagged);
                    if (k < basket_.size()) basket_.erase(basket_.begin() + static_cast<std::ptrdiff_t>(k));
                } else selected_set_.erase(tagged);
                editing_selection_names_ = false;
                SyncSelectedNames(); Relayout(); Invalidate();
            }
            return;
        }
        if (chrome_.overwrite_pending) {
            if (id == kPickOverwriteReplace) { ResolveOverwrite(true); return; }
            if (id == kPickPrimary) return;
            if (id == kPickOverwriteBack || id == kPickRename) { ResolveOverwrite(false, id == kPickRename); return; }
        }
        PickerProofFocus("activate", focus_, id);
        if (id == kPickPrimaryDropDown) {
            if (!chrome_.primary_enabled) return;
            for(size_t i=0;i<spec_.custom_controls.size();++i) if(spec_.custom_controls[i].kind==5) {
                Activate(kCustomFocusBase+static_cast<int>(i)); return;
            }
        }
        if (id >= kCustomFocusBase) {
            const size_t index = static_cast<size_t>(id - kCustomFocusBase);
            // A compact chip that needs typing (an edit, a property's add) opens the details panel there.
            if (CompactCustom() && index < spec_.custom_controls.size() && EffectiveControl(index) == 3 &&
                (spec_.custom_controls[index].kind == 8 || spec_.custom_controls[index].kind == 6 ||
                 spec_.custom_controls[index].property_layout == 3)) {
                custom_details_open_ = true;
                Relayout(); ScrollCustom(0);
                const auto rect = CustomRect(index), panel = CustomPanel();
                if (rect.top < panel.top) ScrollCustom(rect.top - panel.top);
                else if (rect.bottom > panel.bottom) ScrollCustom(rect.bottom - panel.bottom);
                focus_ = id;
                if (spec_.custom_controls[index].kind == 8) SetFocus(custom_edits_[index]);
                else SetFocus(hwnd_);
                Invalidate(); return;
            }
            if(index<spec_.custom_controls.size() && EffectiveControl(index)==3) {
                const auto control=spec_.custom_controls[index];
                if(control.kind==8) {SetFocus(custom_edits_[index]);return;}
                if(control.kind==7) {
                    auto next=control;next.checked=!next.checked;
                    if(UpdateCustom(next)!=S_OK || done_)return;
                    embedded_state_.control_id=next.id;embedded_state_.item_id=next.checked?1u:0u;
                    const auto hr=NotifyEmbedded(PickerDialogEvent::CheckToggled);
                    embedded_state_.control_id=0;embedded_state_.item_id=0;
                    if(hr!=S_OK && !done_)FailPaintedEdit(ERROR_FUNCTION_FAILED);return;
                }
                if(control.kind==6) {
                    size_t selected=control.items.size();
                    for(size_t j=0;j<control.items.size();++j)if(control.has_selected && control.items[j].id==control.selected_item_id)selected=j;
                    size_t chosen=control.items.size();
                    if(custom_radio_item_>=0)chosen=static_cast<size_t>(custom_radio_item_);
                    else for(size_t n=0;n<control.items.size();++n) {
                        const size_t j=(selected==control.items.size()?n:(selected+n+1)%control.items.size());
                        if(control.items[j].state==3){chosen=j;break;}
                    }
                    custom_radio_item_=-1;
                    if(chosen>=control.items.size() || control.items[chosen].state!=3)return;
                    auto next=control;next.has_selected=true;next.selected_item_id=control.items[chosen].id;
                    if(UpdateCustom(next)!=S_OK || done_)return;
                    embedded_state_.control_id=next.id;embedded_state_.item_id=next.selected_item_id;
                    const auto hr=NotifyEmbedded(PickerDialogEvent::ItemSelected);
                    embedded_state_.control_id=0;embedded_state_.item_id=0;
                    if(hr!=S_OK && !done_)FailPaintedEdit(ERROR_FUNCTION_FAILED);return;
                }
            }
            if(index < spec_.custom_controls.size() && EffectiveControl(index)==3 && spec_.custom_controls[index].kind>=3 && spec_.custom_controls[index].kind<=5) {
                if(spec_.custom_controls[index].kind>=4) {
                    embedded_state_.control_id=spec_.custom_controls[index].id;
                    const HRESULT activating=NotifyEmbedded(PickerDialogEvent::ControlActivating);
                    embedded_state_.control_id=0;
                    if(done_ || activating!=S_OK || EffectiveControl(index)!=3) return;
                }
                const auto control=spec_.custom_controls[index];
                const auto generation=embedded_state_.generation;
                std::vector<FluentMenuItem> items;
                for(size_t i=0;i<control.items.size();++i) {
                    const auto& item=control.items[i];if(!(item.state&2)) continue;
                    auto row=Radio(static_cast<int>(i)+1,FormatCustomControlCaption(item.label),control.kind==3 && control.has_selected && item.id==control.selected_item_id);
                    row.enabled=(item.state&1)!=0;items.push_back(std::move(row));
                }
                if(items.empty()) return;
                menu_.SetTheme(dark_,accent_);
                const int command=TrackPickerDropdown(ScreenRect(control.kind==5 ? chrome_layout_.primary : CustomRect(index)),std::move(items));
                if(done_ || generation!=embedded_state_.generation || command<=0 || static_cast<size_t>(command)>control.items.size() ||
                    (spec_.owner_guard && !spec_.owner_guard->Alive())) return;
                const auto item=control.items[static_cast<size_t>(command-1)];
                if(item.state!=3) return;
                if(control.kind!=4) {
                    auto next=control;next.has_selected=true;next.selected_item_id=item.id;
                    if(UpdateCustom(next)!=S_OK || done_) return;
                }
                embedded_state_.control_id=control.id;embedded_state_.item_id=item.id;
                const HRESULT hr=NotifyEmbedded(control.kind==5 ? PickerDialogEvent::OpenDropDownSelected : PickerDialogEvent::ItemSelected);
                embedded_state_.control_id=0;embedded_state_.item_id=0;
                if(!done_ && hr!=S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED);
                if(!done_ && hr==S_OK && control.kind==5) SubmitSelection();
                return;
            }
            if (index < spec_.custom_controls.size() && EffectiveControl(index) == 3 && spec_.custom_controls[index].kind == 2) {
                embedded_state_.control_id = spec_.custom_controls[index].id;
                if (NotifyEmbedded(PickerDialogEvent::ButtonClicked) != S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED);
                embedded_state_.control_id = 0;
                if ((spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                    (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) Cancel();
            }
        } else if (id == kPickClose || id == kPickCancel) Cancel();
        else if (id == kPickPrimary && chrome_.primary_enabled) SubmitSelection();
        else if (id == kPickFilename) FocusFilename();
        else if (id == kPickFilter) ShowFilterMenu();
        if (!done_) Invalidate();
    }

    void EnterOnList() {
        if (selected_set_.size() > 1) { SubmitSelection(); return; }
        if (SelectedEntry()) OpenEntry(selected_);
        else if (!chosen_.empty()) SubmitSelection();
    }

    void SelectAll() {
        if (!allow_multiselect_) return;
        user_interacted_ = true;
        if (validating_) { ++generation_; validating_ = false; }
        selected_set_.clear();
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const auto kind = entries_[static_cast<size_t>(i)].kind;
            if (!TreePlaceholder(i) && (kind == PickerEntryKind::Image || kind == PickerEntryKind::File)) selected_set_.insert(i);
        }
        SyncSelectedNames();
        Invalidate();
    }

    bool HandleKey(WPARAM key) {
        DismissHover();
        if (key == VK_ESCAPE && CloseCustomDetails()) return true;
        if (HandleViewKey(key)) return true;
        if (marquee_.pending || marquee_.active) {
            if (key == VK_ESCAPE) EndMarquee(true);
            return true;
        }
        if (key == 'G' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            auto latest = picker_places::Read();
            picker_user_storage::Path parsed;
            if (picker_user_storage::Parse(latest.current, parsed)) { pulse_places_ = std::move(latest); Navigate(parsed.value, true); }
            return true;
        }
        if (chrome_.overwrite_pending && (key == VK_LEFT || key == VK_RIGHT)) {
            MoveFocus(key == VK_LEFT ? -1 : 1); return true;
        }
        if(focus_>=kCustomFocusBase && (key==VK_UP || key==VK_DOWN || key==VK_LEFT || key==VK_RIGHT)) {
            const size_t index=static_cast<size_t>(focus_-kCustomFocusBase);
            if(index<spec_.custom_controls.size() && spec_.custom_controls[index].kind==6 && EffectiveControl(index)==3) {
                const auto& control=spec_.custom_controls[index];const size_t count=control.items.size();
                size_t selected=count;for(size_t i=0;i<count;++i)if(control.has_selected && control.items[i].id==control.selected_item_id)selected=i;
                for(size_t step=1;step<=count;++step) {
                    const size_t i=selected==count?step-1:(key==VK_UP || key==VK_LEFT?(selected+count-step)%count:(selected+step)%count);
                    if(control.items[i].state==3){custom_radio_item_=static_cast<int>(i);Activate(focus_);break;}
                }
                return true;
            }
        }
        if (TreeKey(key)) return true;
        const bool ctrl = KeyDown(VK_CONTROL);
        const bool shift = KeyDown(VK_SHIFT);
        const int count = static_cast<int>(entries_.size());
        auto list_move = [&](int index) {
            focus_ = kPickList;
            chrome_.show_focus = true;
            if (count > 0) Select(SkipTreePlaceholder(index, index < selected_ ? -1 : 1), shift, ctrl && shift, ctrl && !shift);
        };
        auto step = [&](int dx, int dy) {
            if (count <= 0) return;
            if (selected_ < 0) { list_move(dy < 0 || dx < 0 ? count - 1 : 0); return; }
            SyncVm();
            list_move(renderer_.MoveViewIndex(vm_.pane, PaneBounds(), selected_, dx, dy));
        };
        switch (key) {
        case VK_ESCAPE:
            if (chrome_.overwrite_pending) ResolveOverwrite(false);
            else Cancel();
            return true;
        case VK_TAB: MoveFocus(shift ? -1 : 1); return true;
        case VK_RETURN:
            if (KeyDown(VK_MENU) && focus_ == kPickList && SelectedEntry()) {
                SHObjectProperties(hwnd_, SHOP_FILEPATH, SelectedEntry()->path.c_str(), nullptr);
                return true;
            }
            if (focus_ != kPickList) Activate(focus_);
            else EnterOnList();
            return true;
        case VK_SPACE:
            if (focus_ != kPickList) Activate(focus_);
            else if (PickerSpaceTogglesPreview(true, ctrl, shift, KeyDown(VK_MENU))) TogglePreviewPane();
            else if (selected_ >= 0) Select(selected_, shift, ctrl);
            return true;
        case VK_LEFT: step(-1, 0); return true;
        case VK_RIGHT: step(1, 0); return true;
        case VK_UP: step(0, -1); return true;
        case VK_DOWN: step(0, 1); return true;
        case VK_HOME: list_move(0); return true;
        case VK_END: list_move(count - 1); return true;
        case VK_PRIOR:
        case VK_NEXT: {
            SyncVm();
            const int page = std::max(1, renderer_.PageDelta(vm_.pane, PaneBounds()));
            list_move(std::max(0, selected_) + (key == VK_PRIOR ? -page : page));
            return true;
        }
        case VK_BACK: GoUp(); return true;
        case VK_F5: Reload(); return true;
        case VK_F4: BeginAddressEdit(); return true;
        case VK_F3: BeginSearch(); return true;
        case VK_F2: if (focus_ == kPickList) RenameSelection(); return true;
        case VK_F7: NewFolder(); return true;
        case VK_DELETE: if (focus_ == kPickList) DeleteSelection(shift); return true;
        case VK_F10: if (!shift) break; [[fallthrough]];
        case VK_APPS: {
            if (selected_ >= 0) {
                SyncVm();
                const D2D1_RECT_F item = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), selected_);
                ShowRowMenu(selected_, {static_cast<LONG>(item.left + 24.0f * scale_), static_cast<LONG>(item.bottom)});
            } else { const auto pane = PaneBounds(); ShowBlankMenu({static_cast<LONG>(pane.left + 24 * scale_), static_cast<LONG>(pane.top + 64 * scale_)}); }
            return true;
        }
        case 'L': if (ctrl) { BeginAddressEdit(); return true; } break;
        case 'F': if (ctrl) { BeginSearch(); return true; } break;
        case 'K': if (ctrl) { BeginSearch(); return true; } break;
        case 'E': if (ctrl) { BeginSearch(); return true; } break;
        case 'A': if (ctrl) { SelectAll(); return true; } break;
        case 'C': if (ctrl && focus_ == kPickList) { if (shift) CopySelectionPaths(); else CopySelection(false); return true; } break;
        case 'Z': if (ctrl && focus_ == kPickList) { UndoOperation(); return true; } break;
        case 'X': if (ctrl && focus_ == kPickList) { CopySelection(true); return true; } break;
        case 'V': if (ctrl && focus_ == kPickList) { Paste(); return true; } break;
        case 'N': if (ctrl && shift) { NewFolder(); return true; } break;
        case 'H': if (ctrl) { ToggleHidden(); return true; } break;
        }
        return false;
    }

    void TypeAhead(wchar_t character) {
        const auto now = GetTickCount64();
        if (now - typeahead_time_ > 900) typeahead_.clear();
        const bool cycling = typeahead_.size() == 1 && towlower(typeahead_.front()) == towlower(character);
        if (!cycling) typeahead_.push_back(character);
        typeahead_time_ = now;
        const int count = static_cast<int>(entries_.size());
        for (int offset = 0; offset < count; ++offset) {
            const int start = selected_ < 0 ? 0 : selected_ + (typeahead_.size() == 1 ? 1 : 0);
            const int index = (start + offset) % count;
            if (_wcsnicmp(entries_[static_cast<size_t>(index)].name.c_str(), typeahead_.c_str(),
                          typeahead_.size()) == 0) {
                Select(index);
                break;
            }
        }
    }

    // ---- mouse -------------------------------------------------------------

    void CancelBlankClick() {
        blank_click_.Reset();
        const bool captured = std::exchange(blank_capture_, false);
        if (captured && GetCapture() == hwnd_) ReleaseCapture();
    }

    bool BlankListHit(const Hit& hit, float x, float y) {
        return hit.chrome == kPickNone &&
            (hit.main.region == HitTestResult::Pane || hit.main.region == HitTestResult::None) &&
            ContainsRect(renderer_.PaneListRect(vm_.pane, PaneBounds()), x, y);
    }

    void RunBlankDoubleClick() {
        switch (PickerBlankNavigation(spec_.blank_click_action, history_.CanGoBack(), !current_.empty())) {
        case PickerBlankAction::Back: GoBack(); break;
        case PickerBlankAction::Up: GoUp(); break;
        default: break;
        }
    }

    void OpenPathInPulse(const std::wstring& requested, bool file) {
        std::wstring path = requested;
        if (options_.archive && options_.archive->Contains(path)) {
            path = options_.archive->ArchiveFor(path);
            file = true;
        }
        if (path.empty()) return;
        DismissHover();
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
        if (test_middle_capture_) {
            test_middle_path_ = path; test_middle_file_ = file; ++test_middle_launch_count_;
            return;
        }
#endif
        if (!picker_pulse_open::Launch(path, file)) MessageBeep(MB_ICONWARNING);
    }

    void PressMain(const HitTestResult& hit, float x, float y, bool double_click) {
        using R = HitTestResult;
        switch (hit.region) {
        case R::DetailsOpen:
            if (selected_ >= 0 && static_cast<size_t>(selected_) < entries_.size() && entries_[selected_].archive)
                OpenEntry(selected_);
            return;
        case R::DetailsCopyPath:
            if (const auto* entry = SelectedEntry()) CopyTextToClipboard(hwnd_, path::FriendlyPathText(entry->path));
            return;
        case R::DetailsNewTab:
            if (const auto* entry = SelectedEntry())
                OpenPathInPulse(entry->path, entry->kind == PickerEntryKind::File || entry->kind == PickerEntryKind::Image);
            return;
        case R::DetailsSaveAfterShow:
        case R::DetailsSaveAfterCopy:
            if (mode_ == PickerMode::Save && spec_.enable_save_followup && pulse_places_.save_followup_available) {
                picker_flags_ ^= hit.region == R::DetailsSaveAfterCopy ? picker_layout::kFlagCopyAfterSave : picker_layout::kFlagShowAfterSave;
                Relayout(); Invalidate();
            }
            return;
        case R::NavBack: GoBack(); return;
        case R::NavForward: GoForward(); return;
        case R::NavUp: GoUp(); return;
        case R::NavRefresh: Reload(); return;
        case R::AddressBar: BeginAddressEdit(); return;
        case R::BreadcrumbSegment:
            if (double_click) { BeginAddressEdit(); return; }
            if (address_editing_) EndAddressEdit(false);
            Navigate(hit.path, true);
            return;
        case R::BreadcrumbArrow: {
            if (address_editing_) EndAddressEdit(false);
            POINT anchor{static_cast<LONG>(hit.control_bounds.left),
                         static_cast<LONG>(hit.control_bounds.bottom)};
            ClientToScreen(hwnd_, &anchor);
            if (hit.index == -2) ShowBreadcrumbAncestors(hit.path, anchor);
            else ShowBreadcrumbChildren(hit.path, anchor);
            return;
        }
        case R::AddressSearch:
        case R::AddressSearchInput:
            BeginSearch();
            return;
        case R::AddressSearchScope:
        case R::AddressSearchMode:
        case R::AddressSearchContent:
        case R::AddressSearchOptions:
            ShowSearchSetting(hit.region);
            return;
        case R::AddressSearchClear:
            if (search_edit_) SetWindowTextW(search_edit_, L"");
            BeginSearch();
            return;
        case R::AddressSearchClose: ExitSearch(); return;
        case R::RowCheck:
            if (allow_multiselect_ && hit.index >= 0) {
                focus_ = kPickList; SetFocus(hwnd_);
                Select(hit.index, KeyDown(VK_SHIFT), true);
            }
            return;
        case R::Row:
            if (hit.index < 0) break;
            focus_ = kPickList;
            if (address_editing_) EndAddressEdit(false);
            SetFocus(hwnd_);
            Select(hit.index, KeyDown(VK_SHIFT), KeyDown(VK_CONTROL));
            if (double_click) OpenEntry(hit.index);
            return;
        case R::ColumnHeader: {
            if (hit.sub_index == static_cast<int>(MainRenderer::ColumnKind::Status)) return;   // display-only
            PickerListOrder next = order_;
            if (next.column == static_cast<int>(hit.column)) next.descending = !next.descending;
            else { next.column = static_cast<int>(hit.column); next.descending = false; }
            SetListOrder(next);
            return;
        }
        case R::GroupHeader:
            ToggleListGroup(hit.index);
            return;
        case R::GroupSelect:
            SelectListGroup(hit.index);
            return;
        case R::ColumnDivider:
            if (double_click) {
                std::array<float,4> search{};
                renderer_.AutoFitColumnDivider(PaneBounds(),layout_settings_.column_widths,false,search,hit.index,vm_.pane.cloud_status_column);
                Relayout();
            } else {
                column_resizing_=hit.index;
                SetCapture(hwnd_);
            }
            return;
        case R::Scrollbar:
            if (hit.sub_index == 1) BeginHScrollbarDrag(x);
            else BeginScrollbarDrag(hit, y);
            return;
        case R::PaneDetails: SetViewMode(ViewMode::Details); return;
        case R::PaneMediumIcons:
            SetViewMode(ViewMode::LargeIcons);
            return;
        case R::PaneViewButton: ShowViewMenu(hit.control_bounds.right > hit.control_bounds.left
                                                 ? hit.control_bounds
                                                 : renderer_.PaneViewButtonRect(PaneBounds()));
            return;
        case R::PaneEmptyNewFolder: NewFolder(); return;
        case R::SidebarItem:
        case R::SidebarItemExpand:
            if (navigation_root_.Active() && hit.region == R::SidebarItemExpand) {
                navigation_root_.Toggle(hit.path); Relayout(); Invalidate(); return;
            }
            if (hit.path.starts_with(kPickerTrayPrefix)) { UsePickerTray(hit.path); return; }
            if (hit.path == kCommonUndoToken) { UndoCommonForget(); return; }
            if (hit.path == kShowHiddenGroupsToken) {
                std::erase_if(layout_settings_.hidden_places, [](const auto& item) { return item.starts_with(kHiddenGroupPrefix); });
                Relayout(); Invalidate(); return;
            }
            OpenSharedPlace(hit.path);
            return;
        case R::SidebarItemAction:
            if (IsRemovableCommon(hit.path)) ForgetCommon(hit.path);
            return;
        case R::SidebarHeaderAction:
            if (hit.sidebar_action == SidebarAddAction::PinToApp) TogglePinPathToApp(current_);
            return;
        case R::SidebarHeader:
            if (hit.index >= 0 && hit.index < static_cast<int>(vm_.sidebar.size())) {
                const SidebarGroup& group = vm_.sidebar[static_cast<size_t>(hit.index)];
                if (group.id == kGroupPickerTray) { tray_collapsed_ = !tray_collapsed_; Relayout(); Invalidate(); return; }
                // The This PC title opens This PC; the chevron folds.
                if (hit.sub_index == 1 && group.navigable) Navigate(group.navigation_path, true);
                else {
                    bool& collapsed = collapsed_[CollapseSlot(group.id)];
                    collapsed = !collapsed;
                    Invalidate();
                }
            }
            return;
        case R::Pane:
        case R::None:
            // Blank space in the list clears the selection, as in Explorer.
            if (ContainsRect(PaneBounds(), x, y)) {
                focus_ = kPickList;
                if (address_editing_) EndAddressEdit(false);
                SetFocus(hwnd_);
                ClearSelection();
            }
            return;
        default:
            return;
        }
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == PickerLegacyChildMessage()) {
            if (legacy_destroying_) { legacy_child_.Clear(); return wparam ? 0 : kPickerLegacyChildAttached; }
            if (!legacy_child_.Attach(hwnd_, reinterpret_cast<HWND>(wparam), static_cast<int>(kMinHeight * scale_))) return 0;
            Relayout(); Invalidate(); return kPickerLegacyChildAttached;
        }
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_LBUTTONDOWN || message == WM_MOUSEWHEEL)
            jump_context_touched_ = true;
        if (external_navigation_ && (message == WM_COMMAND ||
            (message == WM_TIMER && wparam == kSearchTimer) ||
            message == kEmbeddedNavigationMessage)) return 0;
        if (spec_.pulse_painted_plain_child && message == PulsePaintedChildEditFailureMessage()) {
            const HWND edit = reinterpret_cast<HWND>(wparam);
            // Same-thread edit presentation can call this synchronously. Only
            // mark terminal state; destruction stays after the message loop.
            if (edit == path_edit_ || edit == filename_edit_ || edit == search_edit_)
                FailPaintedEdit(static_cast<DWORD>(lparam));
            return 0;
        }
        switch (message) {
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
        case kPickerJumpUiTestMessage: {
            DWORD sender_pid = 0;
            GetWindowThreadProcessId(reinterpret_cast<HWND>(wparam), &sender_pid);
            if (sender_pid != GetCurrentProcessId() || !lparam) return FALSE;
            auto* request = reinterpret_cast<PickerJumpUiTest*>(lparam);
            if (request->size != sizeof(*request) || request->version != 1) return FALSE;
            if (request->save_followup_endpoint && spec_.enable_save_followup) {
                picker_places::Snapshot fixture;
                fixture.shared_available = true; fixture.save_followup_available = true;
                fixture.endpoint = request->save_followup_endpoint;
                GetWindowThreadProcessId(fixture.endpoint, &fixture.endpoint_pid);
                spec_.read_shared_places = [fixture] { return fixture; };
                pulse_places_ = fixture;
                Relayout(); Invalidate();
            }
            if (request->archive_worker && options_.archive) {
                const std::wstring worker = request->archive_worker;
                options_.archive = std::make_shared<PickerArchiveSession>([worker] { return worker; }, request->archive_staging ? request->archive_staging : L"");
                Reload();
            }
            if (request->client_width_dip >= kMinWidth && request->client_width_dip <= 1600) {
                RECT window{}, client{}; GetWindowRect(hwnd_, &window); GetClientRect(hwnd_, &client);
                SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(request->client_width_dip * scale_) +
                    (window.right - window.left) - (client.right - client.left), window.bottom - window.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (request->display_prefs_json) {
                app::PickerAppearanceSnapshot appearance;
                if (!app::ParsePickerAppearanceJson(request->display_prefs_json, appearance)) return FALSE;
                app::ApplyPickerAppearanceToSpec(appearance, spec_);
                options_.show_hidden = spec_.show_hidden || spec_.show_hidden_files;
                options_.show_protected_os_files = spec_.show_hidden || spec_.show_protected_os_files;
                order_overrides_.clear(); Reload();
            }
            if (request->tag_mapping) {
                auto next = order_;
                next.tags = app::ReadPickerTagCatalog(request->tag_mapping);
                order_overrides_[ListOrderKey(current_)] = std::move(next);
                Reload();
            }
            if (request->display_command == 2) ToggleHidden();
            else if (request->display_command && !HandleDisplayCommand(request->display_command))
                HandleSortCommand(request->display_command);
            if (request->set_new_since) { options_.new_since = request->set_new_since; Reload(); }
            if (request->set_save_probe_fixture && mode_ == PickerMode::Save && !save_draft_path_.empty()) {
                save_draft_probe_.Cancel(); save_draft_due_ = 0;
                PickerSaveDraftProbe::Result sample;
                sample.generation = save_draft_generation_; sample.path = save_draft_path_;
                sample.attributes = request->save_probe_attributes;
                sample.error = sample.attributes == INVALID_FILE_ATTRIBUTES ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
                sample.space_known = request->save_probe_space_known; sample.free_bytes = request->save_probe_free_bytes;
                save_draft_result_ = std::move(sample); RefreshSaveDraft(); Relayout(); Invalidate();
            }
            if (request->tray_snapshot) {
                const auto& tray = *request->tray_snapshot;
                spec_.poll_jump_context = {};
                spec_.pulse_tray_available = tray.tray_available; spec_.pulse_tray_items = tray.tray_items;
                spec_.pulse_tray_shelf_id = tray.tray_shelf_id; spec_.pulse_tray_shelf_name = tray.tray_shelf_name;
                spec_.pulse_tray_revision = tray.tray_revision; spec_.pulse_tray_total_count = tray.tray_total_count;
                spec_.pulse_tray_publisher_pid = tray.tray_publisher_pid; spec_.pulse_tray_source = tray.tray_source;
                spec_.pulse_tray_endpoint = tray.endpoint; spec_.pulse_tray_endpoint_pid = tray.endpoint_pid;
                SyncVm(); Relayout(); Invalidate();
            }
            if (request->set_tray_fixture) {
                spec_.poll_jump_context = {}; spec_.pulse_tray_items.clear();
                spec_.pulse_tray_shelf_id = 1; spec_.pulse_tray_shelf_name = L"Fixture shelf";
                spec_.pulse_tray_revision = request->tray_revision; spec_.pulse_tray_publisher_pid = GetCurrentProcessId();
                spec_.pulse_tray_total_count = request->tray_total ? request->tray_total : request->tray_count;
                spec_.pulse_tray_available = spec_.pulse_tray_total_count == request->tray_count;
                if (spec_.pulse_tray_available) for (unsigned i = 0; i < std::min(128u, request->tray_count); ++i) {
                    if (!request->tray_paths[i]) continue;
                    picker_places::TrayItem item; item.id = i + 1; item.shelf_id = 1; item.batch_id = 1;
                    item.path = request->tray_paths[i]; item.size = request->tray_sizes[i] ? request->tray_sizes[i] : 1; item.exists = !(request->tray_flags[i] & 1);
                    item.inflight = (request->tray_flags[i] & 2) != 0; item.is_directory = (request->tray_flags[i] & 4) != 0;
                    spec_.pulse_tray_items.push_back(std::move(item));
                }
                SyncVm(); Relayout(); Invalidate();
            }
            if (request->set_pulse_fixture) {
                spec_.poll_jump_context = {};
                spec_.pulse_selection_available = true;
                spec_.pulse_selected_items.clear();
                for (unsigned i = 0; i < std::min(8u, request->pulse_count); ++i)
                    if (request->pulse_paths[i]) spec_.pulse_selected_items.push_back({request->pulse_paths[i], request->pulse_directories[i], request->pulse_sizes_present ? request->pulse_sizes[i] : 1});
                Relayout(); Invalidate();
            }
            if (request->middle_fixture_folder) {
                test_middle_capture_ = true;
                spec_.open_folders = {{request->middle_fixture_folder, L"Middle-click fixture", false}};
                spec_.places = {{L"Middle-click fixture", request->middle_fixture_folder, L"\xE8B7"}};
                BuildSidebar(); Relayout();
            }
            if (request->jump_path) {
                const std::wstring path(request->jump_path);
                const bool file = request->jump_file;
                spec_.jump_search = [path, file](const std::wstring& query) {
                    if (query != L"fixture-result") return std::vector<PickerJumpResult>{};
                    return std::vector<PickerJumpResult>{{path, L"fixture-result", {}, 3, file}};
                };
            }
            if (request->inspect_hover) {
                // Observe the production hit/hover path atomically: a synthetic
                // WM_MOUSEMOVE must not depend on the user's physical cursor position.
                hover_point_ = request->inspect_hover_point;
                UpdateHover(HitAt(static_cast<float>(hover_point_.x), static_cast<float>(hover_point_.y)));
            }
            if (request->tree_toggle_path || request->tree_select_path) {
                const auto requested = request->tree_toggle_path ? request->tree_toggle_path : request->tree_select_path;
                for (size_t i = 0; i < entries_.size(); ++i) if (!TreePlaceholder(static_cast<int>(i)) && SamePickerPath(entries_[i].path, requested)) {
                    if (request->tree_toggle_path) ToggleTree(static_cast<int>(i));
                    else { FocusList(); Select(static_cast<int>(i), false, request->tree_select_add); }
                    break;
                }
            }
            SyncVm();
            auto result_storage = std::make_unique<PickerJumpUiTest>();
            auto& result = *result_storage;
            result.tree_ready = tree_rows_ && !waiting_ && !tree_active_ && tree_pending_.empty();
            wcsncpy_s(result.tree_save_folder, vm_.details.save_folder_text.c_str(), _TRUNCATE);
            wcsncpy_s(result.tree_save_series, vm_.details.save_series_text.c_str(), _TRUNCATE);
            result.tree_save_clash = vm_.details.save_clash;
            if (tree_rows_) {
                result.tree_row_count = static_cast<unsigned>(std::min<size_t>(64, entries_.size()));
                for (unsigned i = 0; i < result.tree_row_count; ++i) {
                    const auto* row = tree_rows_->At(static_cast<int>(i));
                    if (!row) continue;
                    wcsncpy_s(result.tree_paths[i], entries_[i].path.c_str(), _TRUNCATE);
                    result.tree_depths[i] = row->depth; result.tree_kinds[i] = static_cast<uint8_t>(row->kind);
                    result.tree_expanded[i] = row->expanded;
                    const auto rect = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), static_cast<int>(i));
                    const float y = (rect.top + rect.bottom) / 2;
                    result.tree_row_points[i] = {static_cast<LONG>(rect.left + 150 * scale_), static_cast<LONG>(y)};
                    if (row->expandable) for (float x = rect.left; x < std::min(rect.right, rect.left + 400 * scale_); x += 2 * scale_)
                        if (renderer_.TreeChevronAt(vm_.pane, PaneBounds(), x, y) == static_cast<int>(i)) {
                            result.tree_chevrons[i] = {static_cast<LONG>(x + 3 * scale_), static_cast<LONG>(y)}; break;
                        }
                }
            }
            result.save_followup_available = pulse_places_.save_followup_available;
            result.save_notify = (picker_flags_ & picker_layout::kFlagShowAfterSave) != 0;
            result.save_copy = (picker_flags_ & picker_layout::kFlagCopyAfterSave) != 0;
            result.archive_enabled = options_.archive != nullptr;
            for (size_t i = 0; i < std::min<size_t>(entries_.size(), 32); ++i) result.archive_rows[i] = entries_[i].archive;
            result.last_down_hit = test_last_down_;
            result.last_up_hit = test_last_up_;
            result.last_activate = test_last_activate_;
            result.current_pressed = chrome_.pressed;
            result.ready = IncomingFilesReady();
            const auto rect = [](D2D1_RECT_F value) { return RECT{static_cast<LONG>(value.left), static_cast<LONG>(value.top), static_cast<LONG>(value.right), static_cast<LONG>(value.bottom)}; };
            for (size_t i = 0; i < chrome_.context_filters.size() && i < chrome_layout_.context_filters.size(); ++i) {
                const int filter = chrome_.context_filters[i].id - kPickContextFilterBase;
                if (filter >= 0 && filter < 8) result.tree_filter_buttons[filter] = rect(chrome_layout_.context_filters[i]);
            }
            result.show_hidden = options_.show_hidden;
            result.show_protected = options_.show_protected_os_files;
            result.show_extensions = spec_.show_file_extensions;
            result.sort_column = snapshot_order_.column;
            result.sort_descending = snapshot_order_.descending;
            result.folder_mode = snapshot_order_.folder_mode;
            result.group_by = vm_.pane.group_by;
            if (vm_.pane.groups) {
                result.group_count = static_cast<unsigned>(std::min<size_t>(8, vm_.pane.groups->size()));
                for (unsigned i = 0; i < result.group_count; ++i) {
                    const auto& group = (*vm_.pane.groups)[i];
                    wcsncpy_s(result.group_labels[i], group.label.c_str(), _TRUNCATE);
                    result.group_colors[i] = group.color_rgb;
                    result.group_counts[i] = group.count;
                    result.group_collapsed[i] = group.collapsed;
                }
                if (request->inspect_group_headers) {
                    const auto pane = PaneBounds();
                    const float x = pane.left + 100 * scale_;
                    for (float y = pane.top; y < std::min(pane.bottom, chrome_layout_.footer.top); y += 4 * scale_) {
                        const auto hit = renderer_.HitTest(vm_, RendererRect(), x, y);
                        if (hit.region == HitTestResult::Region::GroupHeader && hit.index >= 0 &&
                            static_cast<unsigned>(hit.index) < result.group_count && !result.group_header_points[hit.index].y)
                            result.group_header_points[hit.index] = {static_cast<LONG>(x), static_cast<LONG>(y)};
                    }
                }
            }
            wcsncpy_s(result.validation_notice, chrome_.notice.c_str(), _TRUNCATE);
            result.strict_type_rejected = chrome_.notice == l10n::Get(l10n::StringId::TheSelectedFileDoesNotMatchTheFileType);
            result.muted_count = static_cast<unsigned>(vm_.pane.muted_indices.size());
            result.dimensions_column = image_dimensions_.Active() && !vm_.pane.extension_columns.empty();
            wcsncpy_s(result.format_tooltip, vm_.tooltip_text.c_str(), _TRUNCATE);
            wcsncpy_s(result.format_note, chrome_.format_note.c_str(), _TRUNCATE);
            wcsncpy_s(result.format_static_text, chrome_.format_static_text.c_str(), _TRUNCATE);
            result.format_note_rect = rect(chrome_layout_.format_note);
            result.display_count = static_cast<unsigned>(std::min<size_t>(entries_.size(), 32));
            for (unsigned i = 0; i < result.display_count; ++i) {
                wcsncpy_s(result.dimensions[i], image_dimensions_.Cell(static_cast<int>(i)).c_str(), _TRUNCATE);
                result.muted_rows[i] = vm_.pane.muted_indices.contains(static_cast<int>(i));
                wcsncpy_s(result.raw_names[i], entries_[i].name.c_str(), _TRUNCATE);
                if (vm_.pane.row_cache) {
                    const auto row = vm_.pane.row_cache->rows.find(i);
                    if (row != vm_.pane.row_cache->rows.end())
                        wcsncpy_s(result.drawn_names[i], ShownName(row->second).c_str(), _TRUNCATE);
                }
            }
            result.view_menu = rect(renderer_.PaneViewButtonRect(PaneBounds(), vm_.pane.filter_expand));
            result.preview_requested = PreviewPaneShown();
            result.preview_width_dip = renderer_.DetailsPanelWidth(RendererWidth()) / scale_;
            result.preview_bounds = rect(renderer_.DetailsPreviewBounds());
            result.selected_count = static_cast<unsigned>(selected_set_.size());
            result.only_new = options_.only_new; result.active_time_filter = time_filter_;
            result.new_badge_count = 0;
            result.series_badge_count = 0;
            for (const auto& item : vm_.pane.change_badges) {
                if (item.second.label == l10n::Get(l10n::StringId::PickerSameNameSeries)) ++result.series_badge_count;
                else if (item.second.label == l10n::Get(l10n::StringId::PickerNewBadge)) ++result.new_badge_count;
            }
            wcsncpy_s(result.save_format_notice, chrome_.save_format_notice.c_str(), _TRUNCATE);
            const auto panel = renderer_.DetailsPanelRect(RendererWidth(), Height());
            if (panel.right > panel.left && vm_.details.has_selection) {
                if (vm_.details.picker_save_mode && !vm_.details.picker_save_to_tray) {
                    const auto save_layout = LayoutPickerSavePreview(panel, scale_, false);
                    if (save_layout.after_show.right > save_layout.after_show.left &&
                        renderer_.HitTest(vm_, RendererRect(), (save_layout.after_show.left + save_layout.after_show.right) / 2,
                        (save_layout.after_show.top + save_layout.after_show.bottom) / 2).region == HitTestResult::DetailsSaveAfterShow)
                        result.save_notify_option = rect(save_layout.after_show);
                    if (save_layout.after_copy.right > save_layout.after_copy.left &&
                        renderer_.HitTest(vm_, RendererRect(), (save_layout.after_copy.left + save_layout.after_copy.right) / 2,
                        (save_layout.after_copy.top + save_layout.after_copy.bottom) / 2).region == HitTestResult::DetailsSaveAfterCopy)
                        result.save_copy_option = rect(save_layout.after_copy);
                } else if (!vm_.details.picker_save_mode) {
                    const auto preview_layout = LayoutPickerPreview(panel, scale_, 0, vm_.details.picker_archive);
                    if (renderer_.HitTest(vm_, RendererRect(), (preview_layout.copy_path.left + preview_layout.copy_path.right) / 2,
                        (preview_layout.copy_path.top + preview_layout.copy_path.bottom) / 2).region == HitTestResult::DetailsCopyPath)
                        result.preview_copy = rect(preview_layout.copy_path);
                    if (renderer_.HitTest(vm_, RendererRect(), (preview_layout.reveal.left + preview_layout.reveal.right) / 2,
                        (preview_layout.reveal.top + preview_layout.reveal.bottom) / 2).region == HitTestResult::DetailsNewTab)
                        result.preview_reveal = rect(preview_layout.reveal);
                }
            }
            for (size_t i = 0; i < chrome_.context_filters.size() && i < chrome_layout_.context_filters.size(); ++i) {
                if (chrome_.context_filters[i].id == kPickCompactFilters) result.compact_filters = rect(chrome_layout_.context_filters[i]);
                if (chrome_.context_filters[i].id == kPickTimeMenu) result.time_filter = rect(chrome_layout_.context_filters[i]);
                if (chrome_.context_filters[i].id == kPickSaveFilterAll) result.save_filter_all = rect(chrome_layout_.context_filters[i]);
                if (chrome_.context_filters[i].id == kPickSaveFilterSame) result.save_filter_same = rect(chrome_layout_.context_filters[i]);
            }
            if (chrome_.show_tray_remove && chrome_layout_.tray_remove.right > chrome_layout_.tray_remove.left)
                result.tray_remove_option = rect(chrome_layout_.tray_remove);
            result.saving_to_tray = saving_to_tray_;
            if (mode_ == PickerMode::Save) wcsncpy_s(result.save_to_tray_token, (TrayToken(0) + L":save_to_tray").c_str(), _TRUNCATE);
            result.option_count = static_cast<unsigned>(std::min<size_t>(4, chrome_layout_.option_chips.size()));
            for (unsigned i = 0; i < result.option_count; ++i) result.options[i] = rect(chrome_layout_.option_chips[i]);
            result.more = rect(chrome_layout_.more_options);
            result.new_folder = rect(chrome_layout_.new_folder);
            result.overwrite_replace = rect(chrome_layout_.overwrite_replace);
            result.overwrite_rename = rect(chrome_layout_.rename);
            result.primary_enabled = chrome_.primary_enabled;
            result.footer_tool = rect(chrome_layout_.footer_tool);
            result.middle_launch_count = test_middle_launch_count_;
            result.middle_file = test_middle_file_;
            wcsncpy_s(result.middle_path, test_middle_path_.c_str(), _TRUNCATE);
            if (!chrome_layout_.open_chips.empty()) result.middle_open_folder = rect(chrome_layout_.open_chips.front());
            result.pulse_use = rect(chrome_layout_.pulse_use);
            result.pulse_more = rect(chrome_layout_.pulse_more);
            result.pulse_count = static_cast<unsigned>(chrome_.pulse_selection.size());
            result.basket_count = static_cast<unsigned>(basket_.size());
            result.selection_clear = rect(chrome_layout_.selection_clear);
            result.selection_clear_focused = focus_ == kPickClearSelection;
            result.selection_more = rect(chrome_layout_.selection_more);
            result.footer_selection_count = static_cast<unsigned>(chrome_.selected_names.size());
            wcsncpy_s(result.selection_summary, chrome_.selection_summary.c_str(), _TRUNCATE);
            for (size_t i = 0; i < std::min<size_t>(8, chrome_.selected_sources.size()); ++i) {
                wcsncpy_s(result.selection_sources[i], chrome_.selected_sources[i].c_str(), _TRUNCATE);
                result.selection_tray[i] = chrome_.selected_from_tray[i];
            }
            result.validation_generation = generation_;
            result.tray_remove_after_open = (picker_flags_ & picker_layout::kFlagRemoveTrayAfterOpen) != 0;
            if (TrayConsumeAvailable()) wcsncpy_s(result.tray_consume_token, (TrayToken(0)+L":consume").c_str(), _TRUNCATE);
            result.tray_page = static_cast<unsigned>(tray_page_);
            result.tray_total = spec_.pulse_tray_total_count;
            result.tray_count = static_cast<unsigned>(std::min<size_t>(8, tray_display_items_.size()));
            for (unsigned i = 0; i < result.tray_count; ++i) wcsncpy_s(result.tray_tokens[i], tray_display_items_[i].first.c_str(), _TRUNCATE);
            for (const auto& group : vm_.sidebar) if (group.id == kGroupPickerTray) {
                result.tray_present = true;
                for (const auto& item : group.items) {
                    if (item.path.ends_with(L":more")) wcsncpy_s(result.tray_more_token, item.path.c_str(), _TRUNCATE);
                    if (item.path.find(L":prev:") != std::wstring::npos) wcsncpy_s(result.tray_previous_token, item.path.c_str(), _TRUNCATE);
                    if (item.path.find(L":next:") != std::wstring::npos) wcsncpy_s(result.tray_next_token, item.path.c_str(), _TRUNCATE);
                    if (item.path.ends_with(L":all")) wcsncpy_s(result.tray_all_token, item.path.c_str(), _TRUNCATE);
                }
            }
            result.live_conflict = chrome_.live_conflict; result.save_invalid = save_name_invalid_;
            result.save_draft_checked = save_draft_result_.has_value();
            result.live_rename = rect(chrome_layout_.live_rename); result.live_save = rect(chrome_layout_.live_save);
            result.suggestions_more = rect(chrome_layout_.suggestions_more);
            result.suggestion_count = static_cast<unsigned>(std::min<size_t>(4, save_name_suggestions_.size()));
            for (unsigned i = 0; i < result.suggestion_count; ++i) {
                result.suggestion_kinds[i] = static_cast<int>(save_name_suggestions_[i].kind);
                if (i < chrome_layout_.suggestion_items.size()) result.suggestions[i] = rect(chrome_layout_.suggestion_items[i]);
            }
            wcsncpy_s(result.save_warning, chrome_.summary_warning.c_str(), _TRUNCATE);
            wcsncpy_s(result.save_space, chrome_.save_space_text.c_str(), _TRUNCATE);
            wcsncpy_s(result.save_conflict_text, chrome_.live_conflict_text.c_str(), _TRUNCATE);
            result.save_summary = rect(chrome_layout_.summary);
            result.save_space_rect = rect(chrome_layout_.summary_space);
            result.save_conflict_rect = rect(chrome_layout_.conflict_text);
            for (unsigned i = 0; i < std::min<size_t>(8, chrome_layout_.pulse_items.size()); ++i)
                result.pulse_items[i] = rect(chrome_layout_.pulse_items[i]);
            result.filename = rect(chrome_layout_.filename);
            result.format_more = rect(chrome_layout_.format_more);
            result.format_count = static_cast<unsigned>(std::min<size_t>(64, chrome_.formats.size()));
            for (unsigned i = 0; i < result.format_count; ++i) {
                result.format_indices[i] = static_cast<int>(chrome_.formats[i].filter);
                if (i < chrome_layout_.format_segments.size()) result.formats[i] = rect(chrome_layout_.format_segments[i]);
            }
            result.context_filter_count = static_cast<unsigned>(std::min<size_t>(2, chrome_layout_.context_filters.size()));
            for (unsigned i = 0; i < result.context_filter_count; ++i)
                result.context_filters[i] = rect(chrome_layout_.context_filters[i]);
            result.remove_count = static_cast<unsigned>(std::min<size_t>(2, chrome_layout_.selection_tags.size()));
            for (unsigned i = 0; i < result.remove_count; ++i) {
                auto value = chrome_layout_.selection_tags[i]; value.left = std::max(value.left, value.right - 24 * scale_);
                result.remove_selection[i] = rect(value);
            }
            const auto list = renderer_.PaneListRect(vm_.pane, PaneBounds());
            int previous = -1;
            for (float y = list.top; y < list.bottom && result.row_check_count < 2; y += 3 * scale_)
                for (float x = list.left; x < std::min(list.right, list.left + 70 * scale_); x += 3 * scale_) {
                    const auto hit = renderer_.HitTest(vm_, RendererRect(), x, y);
                    if (hit.region == HitTestResult::RowCheck && hit.index != previous) {
                        previous = hit.index;
                        float right = x, bottom = y;
                        for (float at = x + 1; at < list.right; at += 1) {
                            const auto next = renderer_.HitTest(vm_, RendererRect(), at, y);
                            if (next.region != HitTestResult::RowCheck || next.index != hit.index) break;
                            right = at;
                        }
                        for (float at = y + 1; at < list.bottom; at += 1) {
                            const auto next = renderer_.HitTest(vm_, RendererRect(), (x + right) * 0.5f, at);
                            if (next.region != HitTestResult::RowCheck || next.index != hit.index) break;
                            bottom = at;
                        }
                        result.row_checks[result.row_check_count++] = {static_cast<LONG>((x + right) * 0.5f), static_cast<LONG>((y + bottom) * 0.5f)};
                        break;
                    }
                }
            *request = result; return TRUE;
        }
#endif
        case kPickerDropMessage: {
            if (ole_drag_out_) return 0;
            if (!queued_use_files_.empty()) {
                auto files = std::move(queued_use_files_); queued_use_files_.clear();
                if (CanUseIncoming(files)) {
                    user_interacted_ = true;
                    ValidateNames(std::move(files), false, false, true);
                }
                return 0;
            }
            auto request = std::move(queued_drop_);
            if (request && !done_) ExecuteOperation(std::move(*request));
            return 0;
        }
#ifdef PULSE_WITH_TEST_HOOKS
        case kPickerOleTestMessage: {
            auto* query = reinterpret_cast<PickerOleTestDrop*>(lparam);
            auto* target = ole_drop_.TestTarget();
            if (!query || !query->source || !target) return FALSE;
            std::vector<std::wstring> sources{query->source};
            if (query->second_source) sources.push_back(query->second_source);
            auto* data = FileDataObject::Create(sources);
            if (!data) return FALSE;
            POINT point = query->point;
            ClientToScreen(hwnd_, &point);
            DWORD effect = query->allowed;
            const POINTL screen{point.x, point.y};
            const HRESULT entered = target->DragEnter(data, query->keys, screen, &effect);
            HRESULT dropped = E_FAIL;
            if (SUCCEEDED(entered) && effect) { effect = query->allowed; dropped = target->Drop(data, query->keys & ~MK_RBUTTON, screen, &effect); }
            else target->DragLeave();
            query->effect = effect;
            query->logical_effect = data->LogicalEffect();
            query->performed_effect = data->PerformedEffect();
            data->Release();
            return SUCCEEDED(dropped);
        }
        case kPickerPlacesTestPointMessage: {
            auto* query = reinterpret_cast<PickerPlacesTestPoint*>(lparam);
            if (!query || waiting_ || !initial_show_allowed_) return FALSE;
            SyncVm();
            if (query->kind == PickerPlacesTestPoint::Kind::Row) {
                for (size_t i = 0; i < entries_.size(); ++i) if (query->path && SamePickerPath(entries_[i].path, query->path)) {
                    const auto cell = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), static_cast<int>(i));
                    const float y = (cell.top + cell.bottom) / 2;
                    for (float x = cell.left; x < cell.right; x += 2) {
                        if (renderer_.PointInItemName(vm_.pane, PaneBounds(), static_cast<int>(i), x, y)) {
                            query->point = {static_cast<LONG>(x + 2), static_cast<LONG>(y)}; return TRUE;
                        }
                    }
                }
                return FALSE;
            }
            const auto sidebar = renderer_.SidebarRect(RendererWidth(), Height());
            const float maximum = renderer_.SidebarMaxScroll(vm_, RendererWidth(), Height());
            for (float scroll = 0;; scroll = std::min(maximum, scroll + 80 * scale_)) {
                vm_.sidebar_scroll = scroll;
                for (float y = sidebar.top; y < std::min(sidebar.bottom, chrome_layout_.footer.top); y += 3) {
                    const float x = (sidebar.left + sidebar.right) / 2;
                    const auto hit = renderer_.HitTest(vm_, RendererRect(), x, y);
                    const bool matched = query->kind == PickerPlacesTestPoint::Kind::FavoriteHeader
                        ? hit.region == HitTestResult::SidebarHeader && hit.index >= 0 && hit.index < static_cast<int>(vm_.sidebar.size()) && vm_.sidebar[hit.index].id == kPickerFavoritesSection
                        : hit.region == HitTestResult::SidebarItem && query->path && SamePickerPath(hit.path, query->path);
                    if (matched) { query->point = {static_cast<LONG>(x), static_cast<LONG>(y + 2)}; Invalidate(); return TRUE; }
                }
                if (scroll >= maximum) break;
            }
            return FALSE;
        }
#endif
        case WM_CREATE: {
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_, spec_.window_effect);
            compositor_.SetCompositionAboveChildren(spec_.pulse_painted_plain_child);
            PickerProofPhase("compositor-init-before");
            const bool compositor_initialized = compositor_.Init(hwnd_);
            PickerProofPhase("compositor-init-after");
            if (!compositor_initialized) { NoteFailure(GetLastError(), "compositor-init"); return -1; }
            if (spec_.pulse_painted_plain_child && !compositor_.UsesTransparentComposition()) {
                native_error_ = ERROR_NOT_SUPPORTED; NoteFailure(native_error_, "create-unsupported"); return -1;
            }
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            renderer_.SetCompositor(&compositor_);
            renderer_.SetScale(scale_);
            renderer_.SetIconNotifyWindow(hwnd_);
            frame_pump_.Start(hwnd_, kPickerFrameMessage);
            ApplyRendererSettings();
            CreateFonts();
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            if (spec_.pulse_painted_plain_child && !compositor_.CustomEditEnabled()) {
                native_error_ = ERROR_NOT_SUPPORTED; NoteFailure(native_error_, "create-unsupported"); return -1;
            }
            CreateEdits();
            if (spec_.pulse_painted_plain_child && (!path_edit_ || !search_edit_ ||
                !filename_edit_)) {
                if (!native_error_) native_error_ = ERROR_GEN_FAILURE;
                NoteFailure(native_error_, "create-edits");
                return -1;
            }
            menu_.Create(hwnd_, &compositor_, scale_);
            menu_.SetTheme(dark_, accent_);
            Relayout();
            SetTimer(hwnd_, kSharedPlacesTimer, 1000, nullptr);
            SetTimer(hwnd_, kImageDimensionsTimer, 250, nullptr);
            if (spec_.poll_jump_context) SetTimer(hwnd_, kJumpContextTimer, 100, nullptr);
            if (mode_ == PickerMode::Save) SetTimer(hwnd_, kSaveDraftTimer, 100, nullptr);
            RegisterPickerDrop();
            return 0;
        }
        case WM_WINDOWPOSCHANGING:
            if (!initial_show_allowed_ && lparam)
                reinterpret_cast<WINDOWPOS*>(lparam)->flags &= ~SWP_SHOWWINDOW;
            break;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, chrome_layout_.title.bottom, chrome_layout_.close);
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * scale_);
            info->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale_) + legacy_child_.Height(hwnd_);
            return 0;
        }
        case kDetailsPreviewNavigateMessage:
            if (const int next = selected_ + static_cast<int>(wparam); next >= 0 && next < static_cast<int>(entries_.size())) Select(next);
            return 0;
        case kDetailsPreviewOpenMessage: {
            const auto path = details_preview_.TakeOpenPath();
            if (!path.empty()) { user_interacted_ = true; ValidateNames({path}); }
            else OpenEntry(selected_);
            return 0;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            Relayout();
            Invalidate();
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            PlaceEdits();
            SyncDetailsPreview();
            return 0;
        case WM_SETTINGCHANGE:
            if (known_folders::IsSettingsArea(reinterpret_cast<const wchar_t*>(lparam))) {
                RefreshKnownPlaces();
                Relayout();
                return 0;
            }
            [[fallthrough]];
        case WM_THEMECHANGED:
            RefreshHighContrast();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            renderer_.SetScale(scale_);
            renderer_.InvalidateTypography();
            menu_.Create(hwnd_, &compositor_, scale_);
            CreateFonts();
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            }
            Relayout();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) DismissHover();
            Invalidate();
            break;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBrush());
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDOK) { SubmitSelection(); return 0; }
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == IDCANCEL) { Cancel(); return 0; }
            if (HIWORD(wparam) == EN_CHANGE) InputChanged(reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_CLIPBOARDUPDATE:
            ReadCutClipboard();
            return 0;
        case kExternalNavigationMessage:
            if (!done_ && !embedded_callback_depth_ && !external_navigation_ && !waiting_ && !external_navigation_queue_.empty()) {
                auto path = std::move(external_navigation_queue_.front());
                external_navigation_queue_.pop_front();
                const HRESULT result = NavigateExternal(path);
                if (spec_.on_navigation_complete) {
                    ++embedded_callback_depth_;
                    try { spec_.on_navigation_complete(hwnd_, path, result); }
                    catch (...) { --embedded_callback_depth_; FailPaintedEdit(ERROR_FUNCTION_FAILED); return 0; }
                    --embedded_callback_depth_;
                }
            }
            return 0;
        case kEmbeddedNavigationMessage:
            if (!done_ && !embedded_callback_depth_ && !pending_navigation_.empty()) {
                auto request = std::move(pending_navigation_.front()); pending_navigation_.pop_front();
                Navigate(std::move(request.path), request.history, std::move(request.selection), request.verified);
            }
            return 0;
        case kEndRenameMessage:
            if (wparam == rename_serial_) EndRename(!new_folder_draft_);
            return 0;
        case kEndAddressMessage:
            if (address_editing_ && GetFocus() != path_edit_) EndAddressEdit(false);
            return 0;
#ifdef PULSE_WITH_TEST_HOOKS
        case kPickerInteractionTestMessage: {
            auto* request = reinterpret_cast<PickerInteractionTest*>(lparam);
            if (!request) return FALSE;
            if (request->command == 24) NewItem(false);
            if (request->command >= kPickContextFilterBase && request->command < kPickContextFilterBase + 64)
                ApplyFilter(static_cast<size_t>(request->command - kPickContextFilterBase));
            request->filter_index = options_.filter_index;
            request->filter_label = chrome_.filter_text;
            if (request->key) {
                BYTE before[256]{}; GetKeyboardState(before);
                BYTE keys[256]{};
                keys[VK_CONTROL] = request->ctrl ? 0x80 : 0;
                keys[VK_SHIFT] = request->shift ? 0x80 : 0;
                SetKeyboardState(keys);
                HWND target = request->edit_id ? GetDlgItem(hwnd_, request->edit_id) : hwnd_;
                SendMessageW(target, WM_KEYDOWN, request->key, 0);
                SetKeyboardState(before);
            }
            SyncVm();
            request->ready = !waiting_ && !entries_.empty();
            request->current = current_;
            request->selected_paths = OperationPaths();
            request->rename_edit = !rename_path_.empty() && rename_edit_ && IsWindowVisible(rename_edit_) ? rename_edit_ : nullptr;
            request->selected_visible = false;
            if (selected_ >= 0) {
                const auto row = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), selected_);
                const auto list = renderer_.PaneListRect(vm_.pane, PaneBounds());
                request->selected_visible = row.top >= list.top && row.bottom <= list.bottom;
            }
            request->blank_ready = false;
            const auto blank_list = renderer_.PaneListRect(vm_.pane, PaneBounds());
            for (float y = blank_list.bottom - 12; y > blank_list.top && !request->blank_ready; y -= 12) {
                const float x = blank_list.right - 24;
                if (BlankListHit(HitAt(x, y), x, y)) {
                    request->blank = {static_cast<LONG>(x), static_cast<LONG>(y)};
                    request->blank_ready = !waiting_;
                }
            }
            request->mode = static_cast<int>(view_mode_);
            request->dip = IconDip(view_mode_, icon_dip_, scale_);
            request->visible = hover_preview_.view.visible;
            request->listing_ready = hover_preview_.view.listing &&
                hover_preview_.view.listing->state == HoverFolderInfo::State::Ready;
            for (size_t i = 0; i < entries_.size(); ++i) {
                if ((entries_[i].kind == PickerEntryKind::Folder) != request->folder) continue;
                const auto rect = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), static_cast<int>(i));
                request->item = {static_cast<LONG>(rect.left + 40 * scale_), static_cast<LONG>((rect.top + rect.bottom) / 2)};
                request->screen_item = request->item; ClientToScreen(hwnd_, &request->screen_item);
                break;
            }
            const auto card = renderer_.HoverCardRect();
            request->card = {static_cast<LONG>(card.left), static_cast<LONG>(card.top), static_cast<LONG>(card.right), static_cast<LONG>(card.bottom)};
            request->cancel_text = chrome_.cancel_text;
            request->cancel = {static_cast<LONG>(chrome_layout_.cancel.left), static_cast<LONG>(chrome_layout_.cancel.top),
                static_cast<LONG>(chrome_layout_.cancel.right), static_cast<LONG>(chrome_layout_.cancel.bottom)};
            request->primary = {static_cast<LONG>(chrome_layout_.primary.left), static_cast<LONG>(chrome_layout_.primary.top),
                static_cast<LONG>(chrome_layout_.primary.right), static_cast<LONG>(chrome_layout_.primary.bottom)};
            request->footer = {0, static_cast<LONG>(chrome_layout_.footer.top), static_cast<LONG>(Width()), static_cast<LONG>(Height())};
            return TRUE;
        }
        case kPickerViewSwitchTestMessage: {
            auto* request = reinterpret_cast<PickerViewSwitchTest*>(lparam);
            if (!request) return FALSE;
            if (request->begin) {
                frame_pump_.Disarm();
                KillTimer(hwnd_, kMotionTimer);
                view_switch_test_ = *request;
                view_switch_test_.count = 0;
                SetViewMode(static_cast<ViewMode>(request->mode));
            }
            *request = view_switch_test_;
            request->ready = !waiting_ && !entries_.empty();
            return TRUE;
        }
#endif
        case kPickerFrameMessage:
            frame_pump_.FrameConsumed();
            if (done_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
                frame_pump_.Disarm();
                return 0;
            }
            Invalidate();
            // Like the main window, paint on the compositor clock while giving
            // queued input precedence over animation frames.
            if (!rendering_frame_ && HIWORD(GetQueueStatus(QS_INPUT)) == 0) UpdateWindow(hwnd_);
            return 0;
        case kHoverListingMessage:
            hover_preview_.Receive(PickerLoader::Take(lparam));
            Invalidate(); return 0;
        case WM_TIMER:
            if (wparam == kImageDimensionsTimer) { if (image_dimensions_.Changed()) Invalidate(); return 0; }
            if (wparam == kHoverTimer) { HoverTick(); return 0; }
            if (wparam == kCommonForgetTimer) { CommitCommonForget(); Relayout(); Invalidate(); return 0; }
            if (wparam == kSharedPlacesTimer) { RefreshSharedPlaces(); return 0; }
            if (wparam == kSaveDraftTimer) { PollSaveDraft(); return 0; }
            if (wparam == kJumpContextTimer) { RefreshJumpContext(); return 0; }
            if (wparam == kMarqueeTimer) { UpdateMarquee(marquee_.cursor.x, marquee_.cursor.y, true); return 0; }
            if (wparam == kSearchTimer) { ApplySearchNow(); return 0; }
            if (wparam == kMotionTimer) { Invalidate(); return 0; }
            if (wparam == kLoadTimer) {
                KillTimer(hwnd_, kLoadTimer);
                // Fast folders never flash a spinner; slow ones show the
                // shared list's loading state after the delay.
                if (waiting_) { loading_ = true; Invalidate(); }
            }
            return 0;
        case kFolderCommitMessage:
            OnFolderCommit(PickerLoader::TakeFolderCommit(lparam));
            return 0;
        case kTreeListingMessage:
            OnTreeListing(PickerLoader::Take(lparam)); return 0;
        case kListingMessage:
            OnListing(PickerLoader::Take(lparam));
            return 0;
        case kSaveDirectoriesMessage:
            OnSaveDirectories(PickerLoader::TakeSaveDirectories(lparam));
            return 0;
        case kValidationMessage:
            OnValidation(PickerLoader::TakeValidation(lparam));
            return 0;
        case WM_MOUSEMOVE: {
            blank_click_.Move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
                GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
            if (places_drag_.pending || places_drag_.active) {
                MovePlacesDrag(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, static_cast<DWORD>(wparam)); return 0;
            }
            if (marquee_.pending || marquee_.active) {
                UpdateMarquee(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam))); return 0;
            }
            if (sidebar_resizing_ || column_resizing_>=0) { ResizeLayout(static_cast<float>(GET_X_LPARAM(lparam))); return 0; }
            if (custom_bar_drag_) { DragCustomScrollbar(static_cast<float>(GET_Y_LPARAM(lparam))); return 0; }
            if (dragging_scrollbar_) {
                DragScrollbar(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            }
            hover_point_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (hover_preview_.view.visible && hover_preview_.view.folder &&
                ContainsRect(renderer_.HoverCardRect(), static_cast<float>(hover_point_.x), static_cast<float>(hover_point_.y))) return 0;
            UpdateHover(HitAt(lparam));
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            UpdateHover({});
            return 0;
        case kCustomUpdateMessage:
            try { return static_cast<LRESULT>(UpdateCustom(*reinterpret_cast<const PickerCustomControl*>(lparam))); }
            catch (const std::bad_alloc&) { return static_cast<LRESULT>(E_OUTOFMEMORY); }
            catch (...) { return static_cast<LRESULT>(E_FAIL); }
        case kTextUpdateMessage:
            try { return static_cast<LRESULT>(UpdateText(static_cast<uint32_t>(wparam), *reinterpret_cast<const std::wstring*>(lparam))); }
            catch (const std::bad_alloc&) { return static_cast<LRESULT>(E_OUTOFMEMORY); }
            catch (...) { return static_cast<LRESULT>(E_FAIL); }
        case kNavigationRootMessage:
            try {
                navigation_root_.Set(hwnd_, kNavigationRootListingMessage, *reinterpret_cast<const std::wstring*>(lparam), options_);
                Relayout(); Invalidate(); return static_cast<LRESULT>(S_OK);
            } catch (...) { return static_cast<LRESULT>(E_OUTOFMEMORY); }
        case kNavigationRootListingMessage: {
            const DWORD error = navigation_root_.Complete(lparam);
            if (error && !navigation_root_.RootLoaded()) { FailPaintedEdit(error); return 0; }
            if (error) chrome_.notice = ErrorText(error, L"");
            Relayout(); Invalidate(); return 0;
        }
        case kPropertyValidationMessage:
            try {
                const auto index = static_cast<uint32_t>(wparam);
                std::erase(invalid_properties_, index);
                if (lparam) invalid_properties_.push_back(index);
                chrome_.notice = invalid_properties_.empty() ? std::wstring() : ErrorText(ERROR_INVALID_DATA, L"");
                Invalidate(); return static_cast<LRESULT>(S_OK);
            } catch (...) { return static_cast<LRESULT>(E_OUTOFMEMORY); }
        case BFFM_ENABLEOK:
            if (!spec_.legacy_browse) break;
            legacy_browse_ok_ = lparam != 0;
            UpdateChosen();Invalidate();return 0;
        case BFFM_SETOKTEXT:
            if (!spec_.legacy_browse) break;
            if (const auto text=reinterpret_cast<const wchar_t*>(lparam); text && wcsnlen_s(text,1024)<1024) {
                try {normal_primary_text_=text;UpdateChosen();Relayout();Invalidate();} catch (...) {}
            }
            return 0;
        case BFFM_SETSELECTIONA:
        case BFFM_SETSELECTIONW: {
            if (!spec_.legacy_browse) break;
            if (!lparam) return 0;
            try {
                std::wstring path;
                if (!wparam) {
                    wchar_t buffer[32768]{};
                    if (SHGetPathFromIDListEx(reinterpret_cast<PCIDLIST_ABSOLUTE>(lparam),buffer,ARRAYSIZE(buffer),GPFIDL_DEFAULT))
                        path=buffer;
                } else if (message == BFFM_SETSELECTIONW) {
                    const auto text=reinterpret_cast<const wchar_t*>(lparam);
                    const auto size=wcsnlen_s(text,32768);
                    if (size<32768)path.assign(text,size);
                } else {
                    const auto text=reinterpret_cast<const char*>(lparam);
                    const auto bytes=strnlen_s(text,32768);
                    if (bytes && bytes<32768) {
                        const int units=MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,text,static_cast<int>(bytes),nullptr,0);
                        if (units>0) {
                            path.resize(static_cast<size_t>(units));
                            if (!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,text,static_cast<int>(bytes),path.data(),units))path.clear();
                        }
                    }
                }
                if (!path.empty() && std::filesystem::path(path).is_absolute()) Navigate(std::move(path),true);
            } catch (...) {}
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT card_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &card_point);
            if (!(GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) && hover_preview_.view.visible && hover_preview_.view.folder &&
                ContainsRect(renderer_.HoverCardRect(), static_cast<float>(card_point.x), static_cast<float>(card_point.y))) {
                hover_preview_.view.folder_scroll = std::clamp(hover_preview_.view.folder_scroll -
                    static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * renderer_.HoverFolderRowHeight() * 3,
                    0.0f, renderer_.HoverFolderScrollMax());
                Invalidate(); return 0;
            }
            DismissHover();
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd_, &point);
            if (chrome_layout_.open_scroll_max > 0.5f &&
                ContainsRect(chrome_layout_.strip, static_cast<float>(point.x), static_cast<float>(point.y))) {
                hover_point_ = point;
                ScrollOpenStrip(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 96.0f);
                return 0;
            }
            if (!spec_.custom_controls.empty() && ContainsRect(CustomPanel(), static_cast<float>(point.x), static_cast<float>(point.y))) {
                ScrollCustom(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 38 * scale_); return 0;
            }
            Wheel(wparam, lparam);
            }
            return 0;
        case WM_MOUSEHWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd_, &point);
            if (chrome_layout_.open_scroll_max > 0.5f &&
                ContainsRect(chrome_layout_.strip, static_cast<float>(point.x), static_cast<float>(point.y))) {
                DismissHover();
                hover_point_ = point;
                ScrollOpenStrip(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 96.0f);
                return 0;
            }
            if (HWheel(wparam, point)) return 0;
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            DismissHover();
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            const Hit blank_hit = HitAt(x, y);
            if (ContainsRect(chrome_layout_.primary, x, y)) {
                TraceFolderSubmit(blank_hit.chrome == kPickPrimary ? L"mouse_down_hit" : L"mouse_down_miss");
            }
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
            test_last_down_ = blank_hit.chrome ? blank_hit.chrome : -static_cast<int>(blank_hit.main.region);
#endif
            const bool blank_press = spec_.blank_click_action != app::kBlankClickOff &&
                !waiting_ && !validating_ && !operating_ && !searching_ &&
                !KeyDown(VK_CONTROL) && !KeyDown(VK_SHIFT) && !KeyDown(VK_MENU) &&
                BlankListHit(blank_hit, x, y);
            blank_click_.Begin(blank_press, message == WM_LBUTTONDBLCLK, generation_,
                static_cast<int>(x), static_cast<int>(y));
            const auto custom_bar = CustomScrollbar();
            if (fluent::ScrollbarHitTest(custom_bar, x, y)) {
                const auto thumb = fluent::ScrollbarThumbRect(custom_bar, scale_);
                custom_bar_grab_ = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
                custom_bar_drag_ = true; SetCapture(hwnd_); DragCustomScrollbar(y); return 0;
            }
            const Hit& hit = blank_hit;
            chrome_.show_focus = false;
            if (hit.chrome == kPickJumpPanel) return 0;
            if (jump_.open && hit.chrome >= kPickJumpRowBase && hit.chrome < kPickJumpRowEnd) { ActivateJumpRow(hit.chrome - kPickJumpRowBase, false); return 0; }
            CloseJump();
            if (hit.chrome == kPickFilename) { FocusFilename(); return 0; }
            if (hit.chrome != kPickNone) {
                if (hit.chrome >= kCustomFocusBase) { focus_ = hit.chrome; SetFocus(hwnd_); }
                chrome_.pressed = hit.chrome;
                SetCapture(hwnd_);
                Invalidate();
                return 0;
            }
            if (SidebarResizeAt(x,y)) {
                if (message==WM_LBUTTONDBLCLK) {
                    layout_settings_.sidebar_width=kSidebarDip;
                    renderer_.SetSidebarWidthDip(kSidebarDip);
                    Relayout(); Invalidate();
                } else { sidebar_resizing_=true; SetCapture(hwnd_); }
                return 0;
            }
            if (TreeMouse(hit.main, x, y)) return 0;
            if (message == WM_LBUTTONDOWN && allow_multiselect_ && !waiting_ && !validating_ && !operating_ &&
                ContainsRect(renderer_.PaneListRect(vm_.pane, PaneBounds()), x, y) &&
                (hit.main.region == HitTestResult::Pane || hit.main.region == HitTestResult::None ||
                 (hit.main.region == HitTestResult::Row && !KeyDown(VK_SHIFT) &&
                  !renderer_.PointInItemGrip(vm_.pane, PaneBounds(), hit.main.index, x, y)))) {
                BeginMarquee(x, y); return 0;
            }
            if (message == WM_LBUTTONDOWN && hit.main.region == HitTestResult::Row &&
                !waiting_ && !validating_ && !operating_) {
                BeginPlacesDrag(hit.main.index, POINT{static_cast<LONG>(x), static_cast<LONG>(y)});
                return 0;
            }
            PressMain(hit.main, x, y, message == WM_LBUTTONDBLCLK);
            if (blank_press && !done_) { blank_capture_ = true; SetCapture(hwnd_); }
            if (!done_) Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            const bool blank_capture = std::exchange(blank_capture_, false);
            const int release_x = GET_X_LPARAM(lparam), release_y = GET_Y_LPARAM(lparam);
            if (chrome_.pressed == kPickPrimary || ContainsRect(chrome_layout_.primary,
                    static_cast<float>(release_x), static_cast<float>(release_y))) {
                TraceFolderSubmit(HitAt(lparam).chrome == kPickPrimary ? L"mouse_up_hit" : L"mouse_up_miss",
                    (chrome_.pressed == kPickPrimary ? 1u : 0u) | (GetCapture() == hwnd_ ? 2u : 0u));
            }
            app::BlankPaneClickRelease blank_release;
            blank_release.marquee_active = marquee_.active;
            blank_release.owns_capture = GetCapture() == hwnd_;
            blank_release.modified = KeyDown(VK_CONTROL) || KeyDown(VK_SHIFT) || KeyDown(VK_MENU);
            blank_release.blank_list_hit = !waiting_ && !validating_ && !operating_ && !searching_ &&
                BlankListHit(HitAt(lparam), static_cast<float>(release_x), static_cast<float>(release_y));
            blank_release.drag_width = GetSystemMetrics(SM_CXDRAG);
            blank_release.drag_height = GetSystemMetrics(SM_CYDRAG);
            const bool blank_double = blank_click_.Release(blank_release, generation_, release_x, release_y);
            if (places_drag_.pending || places_drag_.active) {
                FinishPlacesDrag(POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, static_cast<DWORD>(wparam)); return 0;
            }
            if (marquee_.pending || marquee_.active) {
                const bool click = marquee_.pending;
                const bool modified = marquee_.additive;
                const auto hit = HitAt(lparam);
                if (!click) UpdateMarquee(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
                EndMarquee(false);
                if (click) {
                    if (hit.main.region == HitTestResult::Row) Select(hit.main.index, KeyDown(VK_SHIFT), KeyDown(VK_CONTROL));
                    else if (!modified) ClearSelection();
                }
                if (blank_double) RunBlankDoubleClick();
                Invalidate(); return 0;
            }
            if (blank_double) {
                if (GetCapture() == hwnd_) ReleaseCapture();
                RunBlankDoubleClick();
                Invalidate(); return 0;
            }
            if (blank_capture && GetCapture() == hwnd_) ReleaseCapture();
            if (sidebar_resizing_ || column_resizing_>=0) { sidebar_resizing_=false; column_resizing_=-1; ReleaseCapture(); return 0; }
            if (custom_bar_drag_) { custom_bar_drag_ = false; ReleaseCapture(); return 0; }
            if (dragging_scrollbar_) {
                dragging_scrollbar_ = false;
                ReleaseCapture();
                return 0;
            }
            const int pressed = chrome_.pressed;
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
            test_last_up_ = HitAt(lparam).chrome;
#endif
            chrome_.pressed = kPickNone;
            if (pressed != kPickNone) {
                ReleaseCapture();
                if (HitAt(lparam).chrome == pressed) {
                    bool activate=true;
                    if(pressed>=kCustomFocusBase) {
                        const size_t index=static_cast<size_t>(pressed-kCustomFocusBase);
                        if(index<spec_.custom_controls.size() && spec_.custom_controls[index].kind==6) {
                            activate=false;
                            for(size_t j=0;j<spec_.custom_controls[index].items.size();++j)
                                if((spec_.custom_controls[index].items[j].state&2) && ContainsRect(RadioRect(index,j),static_cast<float>(GET_X_LPARAM(lparam)),static_cast<float>(GET_Y_LPARAM(lparam)))) {
                                    custom_radio_item_=static_cast<int>(j);activate=true;break;
                                }
                        }
                    }
                    if(activate)Activate(pressed);
                }
            }
            Invalidate();
            return 0;
        }
        case WM_MBUTTONUP: {
            if (waiting_ || validating_ || operating_ || GetCapture()) return 0;
            const Hit hit = HitAt(lparam);
            std::wstring path;
            bool file = false;
            if (hit.chrome >= kPickOpenFolderBase && hit.chrome < kPickJumpPanel) {
                const auto folders = OpenFolders();
                const size_t index = static_cast<size_t>(hit.chrome - kPickOpenFolderBase);
                if (index < folders.size()) path = folders[index].path;
            } else if (hit.chrome == kPickLastLocation) path = spec_.last_location.path;
            else if (hit.chrome == kPickNone && hit.main.region == HitTestResult::SidebarItem && !hit.main.path.starts_with(L"pulse:")) path = hit.main.path;
            else if (hit.chrome == kPickNone && hit.main.region == HitTestResult::Row && hit.main.index >= 0 &&
                     static_cast<size_t>(hit.main.index) < entries_.size()) {
                const auto& entry = entries_[static_cast<size_t>(hit.main.index)];
                path = entry.path;
                file = entry.kind == PickerEntryKind::File || entry.kind == PickerEntryKind::Image;
            }
            if (path.starts_with(kPickerTrayPrefix)) {
                const auto item = std::find_if(tray_display_items_.begin(), tray_display_items_.end(), [&](const auto& row) { return row.first == path; });
                if (item == tray_display_items_.end() || !TrayCompatible(item->second)) return 0;
                path = item->second.path; file = true;
            }
            OpenPathInPulse(path, file);
            return 0;
        }
        case WM_RBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            const Hit hit = HitAt(x, y);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (hit.main.region == HitTestResult::SidebarItem || hit.main.region == HitTestResult::SidebarItemExpand || hit.main.region == HitTestResult::SidebarHeader ||
                hit.main.region == HitTestResult::SidebarBlank) {
                if (hit.main.region == HitTestResult::SidebarHeader) ShowSidebarGroupMenu(hit.main.index, point);
                else if (hit.main.path.starts_with(kCommonUndoToken) || hit.main.path.starts_with(kShowHiddenGroupsToken)) {}
                else ShowSidebarMenu(hit.main.region==HitTestResult::SidebarItem || hit.main.region==HitTestResult::SidebarItemExpand ? hit.main.path : std::wstring(),point);
            } else if (hit.main.region == HitTestResult::ColumnHeader || hit.main.region == HitTestResult::ColumnDivider) {
                ShowColumnMenu(point);
            } else if (hit.main.region == HitTestResult::BreadcrumbSegment || hit.main.region == HitTestResult::AddressBar) {
                ShowAddressMenu(hit.main.region == HitTestResult::BreadcrumbSegment ? hit.main.path : current_, point);
            } else if (hit.main.region == HitTestResult::Row && hit.main.index >= 0) {
                focus_ = kPickList;
                SetFocus(hwnd_);
                ShowRowMenu(hit.main.index, point);
            } else if (ContainsRect(PaneBounds(), x, y) && hit.chrome == kPickNone &&
                       (hit.main.region == HitTestResult::Pane || hit.main.region == HitTestResult::None)) {
                ShowBlankMenu(point);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam)==HTCLIENT) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd_,&point);
                const auto hit=HitAt(static_cast<float>(point.x),static_cast<float>(point.y));
                if (sidebar_resizing_ || column_resizing_>=0 || (hit.chrome==kPickNone &&
                    (SidebarResizeAt(static_cast<float>(point.x),static_cast<float>(point.y)) || hit.main.region==HitTestResult::ColumnDivider))) {
                    SetCursor(LoadCursorW(nullptr,IDC_SIZEWE)); return TRUE;
                }
            }
            break;
        case WM_KILLFOCUS:
            DismissHover();
            break;
        case WM_CANCELMODE:
            CancelBlankClick();
            DismissHover();
            CancelPlacesDrag();
            EndMarquee(true);
            break;
        case WM_CAPTURECHANGED:
            blank_capture_ = false;
            blank_click_.CancelPress();
            CancelPlacesDrag();
            EndMarquee(true);
            sidebar_resizing_=false; column_resizing_=-1;
            dragging_scrollbar_ = false;
            return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) GoBack();
            else if (GET_XBUTTON_WPARAM(wparam) == XBUTTON2) GoForward();
            return TRUE;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && (places_drag_.pending || places_drag_.active)) {
                CancelPlacesDrag(); Invalidate(); return 0;
            }
            if (HandleKey(wparam)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_RETURN && HandleKey(wparam)) return 0;
            if (wparam == '0') { UsePulseSelection(); return 0; }
            if (wparam == VK_UP) { GoUp(); return 0; }
            if (wparam == VK_LEFT) { GoBack(); return 0; }
            if (wparam == VK_RIGHT) { GoForward(); return 0; }
            if (wparam == 'D') { BeginAddressEdit(); return 0; }
            if (wparam == 'N' && filename_edit_) { FocusFilename(); return 0; }
            if (wparam == 'P') { TogglePreviewPane(); return 0; }
            if (wparam >= '1' && wparam <= '9') {
                const size_t idx = static_cast<size_t>(wparam - '1');
                const auto folders = OpenFolders();
                if (idx < folders.size()) {
                    JumpToOpenFolder(idx);
                    return 0;
                }
                if (idx == folders.size() && !chrome_.last_location.path.empty()) {
                    Navigate(chrome_.last_location.path, true);
                    return 0;
                }
            }
            break;
        case WM_CHAR:
            if (wparam >= 0x20 && wparam != 0x7F && focus_ == kPickList && !KeyDown(VK_CONTROL)) {
                TypeAhead(static_cast<wchar_t>(wparam));
                return 0;
            }
            break;
        case WM_SETFOCUS:
            PickerProofFocus("parent-setfocus", focus_, focus_);
            if (focus_ == kPickAddress || focus_ == kPickSearch || focus_ == kPickFilename)
                focus_ = kPickList;
            Invalidate();
            return 0;
        case kGitRootMessage: {
            std::unique_ptr<std::wstring> found(reinterpret_cast<std::wstring*>(lparam));
            if (static_cast<uint64_t>(wparam) == git_repo_generation_ && found) {
                if (git_repo_root_ != *found) {
                    git_repo_root_ = std::move(*found);
                    Relayout(); Invalidate();
                }
            }
            return 0;
        }
        case WM_CLOSE:
            Cancel();
            return 0;
        case WM_PAINT: {
            if (!initial_show_allowed_) { ValidateRect(hwnd_, nullptr); return 0; }
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            legacy_destroying_ = true; legacy_child_.Clear();
            DismissHover();
            frame_pump_.Stop();
            ole_drop_.Revoke();
            queued_drop_.reset();
            queued_use_files_.clear();
            SaveLayout();
            menu_.Dismiss();
            KillTimer(hwnd_, kMotionTimer);
            renderer_.SetIconNotifyWindow(nullptr);
            for (HWND edit : AllEdits()) {
                if (edit) { RemoveWindowSubclass(edit, EditProc, 1); DestroyWindow(edit); }
            }
            path_edit_ = filename_edit_ = search_edit_ = nullptr;
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (rename_font_) { DeleteObject(rename_font_); rename_font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    bool rendering_frame_ = false;
    app::FramePump frame_pump_;
#ifdef PULSE_WITH_TEST_HOOKS
    PickerViewSwitchTest view_switch_test_;
#endif
    uint64_t painted_frame_ = 0;
    bool initial_show_allowed_ = false;
    bool preparing_first_frame_ = false;
    bool initial_frame_prepared_ = false;
    bool first_present_occluded_=false;
    bool first_present_retrying_=false;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND path_edit_ = nullptr;
    HWND filename_edit_ = nullptr;
    PickerLegacyChild legacy_child_;
    PickerNavigationRoot navigation_root_;
    bool legacy_destroying_ = false;
    std::vector<uint32_t> invalid_properties_;
    HWND search_edit_ = nullptr;
    std::vector<HWND> custom_edits_;
    int custom_radio_item_=-1;
    bool operating_ = false;
    bool filter_open_ = false;
    float filter_motion_from_ = 0.0f;
    uint64_t filter_motion_start_ = 0;
    std::wstring reload_notice_;
    std::vector<std::wstring> cut_paths_;
    HFONT font_ = nullptr;
    HFONT rename_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    MainRenderer renderer_;
    FluentMenu menu_;
    WindowViewModel vm_;
    FolderPickerChrome chrome_;
    FolderPickerChromeLayout chrome_layout_;
    PickerCustomFlowLayout custom_flow_;
    int custom_hover_sub_ = -1;
    FolderPickerSpec spec_;
    PickerMode mode_ = PickerMode::Folder;
    PickerHistory history_;
    PickerOptions options_;
    std::shared_ptr<PickerArchiveDelivery> archive_delivery_;
    std::wstring archive_source_;
    std::optional<picker_save::Stamp> save_before_;
    PickerListOrder order_;            // the order the next listing is read in
    std::vector<size_t> recent_formats_;  // types selected in this dialog, most recent first
    std::vector<size_t> saved_formats_;   // types this program saved with before
    std::vector<PickerTrayPickup> tray_pickups_;
    bool tray_expanded_ = false, tray_collapsed_ = false;
    bool saving_to_tray_ = false;
    std::wstring save_before_tray_dir_;
    app::DragShakeDetector shake_detector_;
    bool tray_drop_armed_ = false;
    std::vector<std::wstring> incoming_dropzone_sources_;
    std::vector<std::wstring> app_pinned_places_;
    std::vector<PickerCommonRow> common_rows_;     // 常用 as last built (menus and × look rows up here)
    struct { std::wstring path, label; } common_forget_;   // removed, still undoable
    std::vector<std::wstring> common_forgotten_;   // removed and written this session
    std::wstring original_file_path_;
    std::wstring git_repo_root_, git_repo_dir_;
    uint64_t git_repo_generation_ = 0;
    int save_total_files_cached_ = -1;
    size_t tray_page_ = 0;
    std::wstring tray_page_identity_;
    std::vector<std::pair<std::wstring, picker_places::TrayItem>> tray_display_items_;
    PickerSaveDraftProbe save_draft_probe_;
    std::optional<PickerSaveDraftProbe::Result> save_draft_result_;
    std::wstring save_draft_key_, save_draft_path_;
    uint64_t save_draft_generation_ = 0;
    ULONGLONG save_draft_due_ = 0;
    bool save_name_invalid_ = false;
    std::vector<SaveNameSuggestion> save_name_suggestions_;
    bool counts_valid_ = false;           // the open filter bar's counts, from the last listing
    std::vector<uint32_t> filter_counts_;
    std::array<uint32_t, kPickerCategoryCount> category_counts_{};
    uint32_t new_count_ = 0;
    int time_filter_ = 0;                 // 0 any, 1 today, 2 this week
    uint64_t opened_at_ = 0;              // this dialog's start: next time's 新文件 boundary
    uint32_t picker_flags_ = 0, saved_flags_ = 0;  // picker_layout::kFlag*, per program and mode
    std::vector<std::wstring> saved_format_patterns_;
    PickerImageDimensions image_dimensions_;
    PickerListOrder snapshot_order_;   // the order snapshot_ was read in
    std::map<std::wstring, PickerListOrder> order_overrides_;  // chosen here, per folder, until close
    int default_group_by_ = 0;         // without the main window's order
    std::set<std::wstring> collapsed_list_groups_;
    std::shared_ptr<const ListGroups> list_groups_;
    const void* list_groups_snapshot_ = nullptr;
    uint64_t list_groups_rev_ = 1, list_groups_built_rev_ = 0;
    std::unique_ptr<PickerLoader> loader_;
    PickerDetailsTree tree_;
    std::shared_ptr<DetailsTreeRows> tree_rows_;
    std::unique_ptr<PickerLoader> tree_loader_;
    std::deque<std::wstring> tree_pending_;
    std::wstring tree_active_path_;
    uint64_t tree_serial_ = 0, tree_active_ = 0;
    std::vector<PickerPlace> places_;
    std::vector<PickerPlace> standard_places_;     // 此电脑's everyday folders (RefreshKnownPlaces)
    std::vector<SidebarItem> drives_;
    bool collapsed_[9] = {};
    std::unordered_set<std::wstring> folded_favorite_groups_;
    std::future<picker_places::Snapshot> saved_places_;
    PickerPlacesDrag places_drag_;
    PickerOleDrop ole_drop_;
    std::unique_ptr<ops::OpRequest> queued_drop_;
    std::vector<std::wstring> queued_use_files_;
    bool right_file_drag_ = false;
    std::vector<picker_places::SelectedItem> validation_selection_items_;
    bool validation_incoming_files_ = false, validation_add_selection_ = false;
    bool ole_drag_out_ = false;
    picker_places::Snapshot pulse_places_;
    std::wstring current_;
    std::vector<PickerEntry> entries_;
    fs::SnapshotPtr snapshot_;
    std::unordered_set<int> selected_set_;
    PickerMarquee marquee_;
    PickerBlankClick blank_click_;
    bool blank_capture_ = false;
    PickerOperationSession operation_session_;
    int selected_ = -1;
    int anchor_ = -1;
    float scroll_ = 0.0f;
    float scroll_x_ = 0.0f;   // List view only (ScrollX)
    ViewMode view_mode_ = ViewMode::Details;
    float icon_dip_ = 0, last_icon_dip_ = 0;
    ViewMode last_icon_mode_ = ViewMode::MediumIcons;
    int zoom_wheel_remainder_ = 0;
    ULONGLONG zoom_wheel_tick_ = 0;
    PickerHoverPreview hover_preview_;
    QuickPreviewWindow details_preview_;
    bool user_interacted_ = false;
    std::vector<std::wstring> create_consent_paths_, share_consent_paths_;
    std::unordered_set<std::wstring> share_consent_targets_;
    std::optional<SaveObjectIdentity> share_consent_save_identity_, share_consent_save_parent_;
    ULONGLONG hover_started_ = 0;
    POINT hover_point_{};
    picker_layout::State layout_settings_;
    bool persist_layout_=false, layout_shown_=false, layout_captured_=false, layout_saved_=false;
    bool sidebar_resizing_=false;
    int column_resizing_=-1;
    std::wstring error_;
    std::wstring chosen_;
    std::wstring filename_text_;
    std::wstring search_text_;
    std::wstring fallback_;
    std::wstring select_after_;
    std::vector<std::wstring> select_after_paths_;
    std::vector<app::CreateRenameIntent> create_rename_intents_;
    HWND rename_edit_ = nullptr;
    bool new_folder_draft_ = false;
    fs::SnapshotPtr new_folder_snapshot_;
    std::wstring rename_path_;
    int rename_index_ = -1;
    uint64_t rename_generation_ = 0;
    WPARAM rename_serial_ = 0;
    std::vector<std::wstring> result_;
    bool custom_bar_drag_ = false;
    float custom_bar_grab_ = 0;
    float custom_scroll_ = 0;
    int focus_ = kPickList;
    bool waiting_ = false;
    bool loading_ = false;
    bool validating_ = false;
    std::optional<SaveConfirmationSnapshot> pending_overwrite_;
    uint64_t overwrite_generation_ = 0;
    std::wstring normal_primary_text_;
    bool legacy_browse_ok_ = true;
    ComPtr<ID2D1Bitmap> app_icon_bitmap_;
    PickerJumpPanel jump_;
    PickerJumpLayout jump_layout_;
    const void* app_icon_device_ = nullptr;
    common::MenuIconRef app_icon_source_, app_icon_fallback_;
    bool save_focus_pending_ = false;
    HWND save_focus_before_validation_ = nullptr;
    bool address_editing_ = false;
    bool searching_ = false;
    bool allow_multiselect_ = false;
    bool save_filter_changed_ = false;
    bool syncing_filename_filter_ = false;
    bool filename_ime_composing_ = false;
    bool open_filter_changed_ = false;
    bool filename_from_selection_ = false;
    bool editing_selection_names_ = false;
    std::vector<std::wstring> selection_tag_paths_;
    std::wstring save_format_notice_, save_format_notice_filename_;
    size_t save_format_notice_filter_ = static_cast<size_t>(-1);
    std::vector<int> selection_tag_indices_;  // per tag: entry index, or -1 - basket index
    struct BasketItem { std::wstring path, name; uint64_t size = 0; };
    std::vector<BasketItem> basket_;          // picks kept from other folders (File mode, multi-select)
    std::vector<BasketItem> last_file_picks_; // the files picked last in this folder
    bool validation_navigation_only_ = false;
    bool setting_edits_ = false;
    bool dragging_scrollbar_ = false;
    bool scrollbar_sidebar_ = false;
    bool scrollbar_horizontal_ = false;  // the List view's bottom bar; scrollbar_drag_y_ then holds x
    float scrollbar_drag_y_ = 0.0f;
    float scrollbar_drag_scroll_ = 0.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    struct PendingNavigation { std::wstring path; bool history; std::wstring selection; bool verified; };
    std::deque<PendingNavigation> pending_navigation_;
    PickerDialogSnapshot embedded_state_;
    unsigned embedded_callback_depth_ = 0;
    bool embedded_ready_ = false, embedded_folder_pending_ = false;
    bool submit_ready_ = false, pending_submit_ = false;
    bool embedded_folder_committed_ = false;
    std::deque<std::wstring> external_navigation_queue_;
    bool external_navigation_ = false;
    std::unique_ptr<PickerListing> external_listing_;
    uint64_t generation_ = 0;
    uint64_t initial_jump_generation_ = 0;
    bool jump_context_touched_ = false;
    bool custom_details_open_ = false;
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
    int test_last_down_ = 0, test_last_up_ = 0, test_last_activate_ = 0;
    bool test_middle_capture_ = false, test_middle_file_ = false;
    unsigned test_middle_launch_count_ = 0;
    std::wstring test_middle_path_;
#endif
    uint64_t jump_save_generation_ = 0;
    std::wstring jump_save_filename_;
    ULONGLONG typeahead_time_ = 0;
    std::wstring typeahead_;
    size_t mode_index_ = 0;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    DWORD native_error_ = ERROR_SUCCESS;
    FilePickerFailure failure_;
    HRESULT failure_hr_ = S_OK;
    bool initial_load_ = false;
    bool current_verified_ = false;
};

} // namespace

HRESULT NavigatePickerFolder(HWND window, const std::wstring& path) {
    if (!IsWindow(window)) return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    wchar_t class_name[64]{};
    if (!GetClassNameW(window, class_name, 64) || wcscmp(class_name, kPickerClass) != 0) return E_INVALIDARG;
    auto* picker = reinterpret_cast<FolderPickerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return picker ? picker->NavigateExternal(path) : E_INVALIDARG;
}

HRESULT UpdatePickerCustomControl(HWND window, const PickerCustomControl& control) {
    if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    return static_cast<HRESULT>(SendMessageW(window, kCustomUpdateMessage, 0, reinterpret_cast<LPARAM>(&control)));
}
HRESULT UpdatePickerText(HWND window, uint32_t kind, const std::wstring& text) {
    if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    return static_cast<HRESULT>(SendMessageW(window, kTextUpdateMessage, kind, reinterpret_cast<LPARAM>(&text)));
}
HRESULT SetPickerPropertyValidation(HWND window, uint32_t index, bool invalid) {
    if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    return static_cast<HRESULT>(SendMessageW(window, kPropertyValidationMessage, index, invalid ? 1 : 0));
}
HRESULT SetPickerNavigationRoot(HWND window, const std::wstring& root) {
    if (!window || GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    return static_cast<HRESULT>(SendMessageW(window, kNavigationRootMessage, 0, reinterpret_cast<LPARAM>(&root)));
}

FilePickerOutcome ShowFilePickerDetailed(HWND owner, const FolderPickerSpec& spec, bool dark,
                                         D2D1_COLOR_F accent) {
    FilePickerOutcome outcome;
    if (spec.mode == PickerMode::Save && spec.allow_multiselect) {
        outcome.error = ERROR_INVALID_PARAMETER;
        return outcome;
    }
    if (owner && !IsWindow(owner)) {
        outcome.error = ERROR_INVALID_WINDOW_HANDLE;
        return outcome;
    }
    if (spec.cancel_event) {
        const DWORD wait = WaitForSingleObject(spec.cancel_event, 0);
        if (wait == WAIT_FAILED) { outcome.error = GetLastError(); return outcome; }
        if (wait == WAIT_OBJECT_0) { outcome.status = FilePickerStatus::Cancelled; return outcome; }
    }
    std::unique_ptr<pulse::PickerOwnerGuard> local_guard;
    if (!spec.owner_guard) local_guard = std::make_unique<pulse::PickerOwnerGuard>(owner);
    auto* guard = spec.owner_guard ? spec.owner_guard : local_guard.get();
    if (guard->Window() != owner) { outcome.error = ERROR_INVALID_PARAMETER; return outcome; }
    if (guard->Error()) { outcome.error = guard->Error(); return outcome; }
    if (!guard->Alive()) { outcome.status = FilePickerStatus::Cancelled; return outcome; }
    auto guarded_spec = spec;
    guarded_spec.owner_guard = guard;
    FolderPickerWindow window;
    const bool picked = window.Show(owner, guarded_spec, dark, accent, outcome.result);
    if (!guard->Alive()) { outcome.result = {}; outcome.status = FilePickerStatus::Cancelled; return outcome; }
    outcome.error = window.Error();
    outcome.failure = window.Failure();
    outcome.status = picked ? FilePickerStatus::Accepted :
        outcome.error ? FilePickerStatus::Failed : FilePickerStatus::Cancelled;
    return outcome;
}

bool ShowFilePicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result) {
    auto outcome = ShowFilePickerDetailed(owner, spec, dark, accent);
    if (outcome.status != FilePickerStatus::Accepted) return false;
    result = std::move(outcome.result);
    return true;
}

bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path) {
    auto single = spec;
    single.allow_multiselect = false;
    FilePickerResult result;
    if (!ShowFilePicker(owner, single, dark, accent, result) || result.paths.empty()) return false;
    path = std::move(result.paths.front());
    return true;
}

} // namespace pulse::ui
#if defined(PULSE_PICKER_JUMP_UI_TEST_HOOKS)
extern "C" __declspec(dllexport) int WINAPI PulsePickerJumpLayoutTest() {
    return pulse::ui::TestPickerEmbeddedLayout();
}
#endif
