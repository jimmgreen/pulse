#include "../common/picker_places_snapshot.h"
#include "../common/picker_user_storage.h"
#include "picker_custom_layout.h"
#include "../common/picker_owner_guard.h"
#include "picker_proof_trace.h"
#include "edit_host.h"
#include "../fs/drive_kind.h"
#include "../common/windows_compat.h"
#include "folder_picker_dialog.h"
#include "../common/picker_navigation.h"

#include "address_search_layout.h"
#include "folder_picker_loader.h"
#include "folder_picker_operations.h"
#include "folder_picker_view.h"
#include "confirm_dialog.h"
#include "fluent_menu.h"
#include "typography.h"
#include "ui_renderer.h"
#include "ui_motion.h"
#include "window_helpers.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ops/clipboard.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#if defined(PULSE_PICKER_INPROCESS_PROOF)
#include <cstdio>
#endif
#include <cwctype>
#include <memory>
#include <deque>
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
constexpr UINT kEndAddressMessage = WM_APP + 3;
constexpr UINT kEmbeddedNavigationMessage = WM_APP + 4;
constexpr UINT kFolderCommitMessage = WM_APP + 6;
constexpr UINT kExternalNavigationMessage = WM_APP + 7;
constexpr DWORD kInitialFolderTimeoutMs = 5000;
constexpr UINT kCustomUpdateMessage = WM_APP + 5;
constexpr int kCustomFocusBase = 1000;
constexpr int kCustomEditBase = 2000;
constexpr size_t PULSE_CUSTOM_EDIT_MAX = 32767;
constexpr UINT_PTR kLoadTimer = 1;
constexpr UINT_PTR kSearchTimer = 2;
constexpr UINT_PTR kMotionTimer = 3;
constexpr int kPathEditId = 101;
constexpr int kFilenameEditId = 102;
constexpr int kSearchEditId = 103;
constexpr UINT kSpinnerDelayMs = 150;
constexpr float kDefaultWidth = 1000.0f;
constexpr float kDefaultHeight = 640.0f;
constexpr float kMinWidth = 640.0f;
constexpr float kMinHeight = 420.0f;
constexpr float kSidebarDip = 196.0f;

// Sidebar sections. The ids only have to be distinct; they follow
// app::SidebarSectionId so the shared renderer sees familiar values.
constexpr int kGroupQuick = 1;
constexpr int kGroupDrives = 3;

// Where the last pick of each mode was made, and how it was viewed, for this
// session.
std::wstring g_last_folder[4];
ViewMode g_last_view[4] = {ViewMode::Details, ViewMode::LargeIcons, ViewMode::Details, ViewMode::Details};

bool KeyDown(int key) { return (GetKeyState(key) & 0x8000) != 0; }

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw)
        path = raw;
    if (raw) CoTaskMemFree(raw);
    return path;
}

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
        std::wstring path = KnownFolder(*k.id);
        if (!path.empty()) places.push_back({l10n::Get(k.label), std::move(path), k.glyph});
    }
    return places;
}

std::wstring DriveDetail(uint64_t free, uint64_t total) {
    if (!total) return {};
    return format::ByteSize(free, false, format::ByteSizeStyle::Compact) + L" / " +
           format::ByteSize(total, false, format::ByteSizeStyle::Compact);
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
        dark_ = dark;
        accent_ = accent;
        spec_ = spec;
        mode_ = spec.mode;
        mode_index_ = mode_ == PickerMode::Image ? 1 : mode_ == PickerMode::File ? 2 : mode_ == PickerMode::Save ? 3 : 0;
        allow_multiselect_ = spec.allow_multiselect && mode_ != PickerMode::Save;
        options_.filters = spec.filters;
        options_.show_hidden = spec.show_hidden;
        options_.no_dereference_links = spec.no_dereference_links;
        options_.advisory_file_filters = spec.advisory_file_filters;
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
        view_mode_ = g_last_view[mode_index_];
        filename_text_ = spec.filename;

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
        chrome_.cancel_text = l10n::Get(l10n::StringId::Cancel);
        chrome_.hosted_edit = true;
        UpdateFilterText();
        focus_ = kPickList;

        pulse_places_ = picker_places::Read();
        places_ = spec.places;
        PickerProofPhase("known-places-before");
        for (PickerPlace& place : KnownPlaces()) {
            const bool duplicate = std::any_of(places_.begin(), places_.end(),
                [&](const PickerPlace& p) { return SamePickerPath(p.path, place.path); });
            if (!duplicate) places_.push_back(std::move(place));
        }
        places_.insert(places_.end(), spec.bottom_places.begin(), spec.bottom_places.end());
        PickerProofPhase("known-places-after");
        PickerProofPhase("sidebar-drives-before");
        drives_ = ReadSidebarDrives();
        PickerProofPhase("sidebar-drives-after");

        fallback_ = KnownFolder(mode_ == PickerMode::Image ? FOLDERID_Pictures : FOLDERID_Desktop);
        std::wstring start = NormalizePickerInput(spec.initial_path);
        if (start.empty()) start = g_last_folder[mode_index_];
        if (start.empty()) start = fallback_;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow()))
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
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kPickerClass, chrome_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT,
            static_cast<int>(kDefaultWidth * scale_), static_cast<int>(kDefaultHeight * scale_),
            owner, nullptr, wc.hInstance, this);
        PickerProofPhase("create-window-after");
        if (!hwnd_) {
            if (!native_error_) native_error_ = GetLastError();
            if (!native_error_) native_error_ = ERROR_GEN_FAILURE;
            return false;
        }
        // WM_CREATE read the DPI of the monitor the window landed on, which
        // can differ from the owner's guess above.
        CenterOwnedWindow(hwnd_, owner_, static_cast<int>(kDefaultWidth * scale_),
                          static_cast<int>(kDefaultHeight * scale_));
        loader_ = std::make_unique<PickerLoader>(hwnd_, kListingMessage, kValidationMessage,
            spec_.on_dialog_event ? kFolderCommitMessage : 0);
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
        ShowWindow(hwnd_, SW_SHOW);
        if (NotifyEmbedded(PickerDialogEvent::Shown) != S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED);
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
        if (!done_ && spec_.on_dialog_event) {
            PublishEmbeddedState();
            if (NotifyEmbedded(PickerDialogEvent::Ready) != S_OK) FailPaintedEdit(ERROR_FUNCTION_FAILED);
        }
        if (!done_ && spec_.on_ready) {
            HRESULT notified = E_FAIL;
            PickerProofPhase("on-ready-before");
            try { notified = spec_.on_ready(hwnd_); } catch (...) {}
            PickerProofPhase("on-ready-after");
            if (notified != S_OK) { native_error_ = ERROR_FUNCTION_FAILED; done_ = true; }
        }

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
                if (wait == WAIT_FAILED) { native_error_ = GetLastError(); break; }
                const bool peeked=PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)!=FALSE;
                if(spec_.cancel_event && WaitForSingleObject(spec_.cancel_event,0)==WAIT_OBJECT_0) {PickerProofPhase("pump-cancel-after-sent-message");break;}
                if (!peeked) continue;
                if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
                RedirectStrayModalKey(message, hwnd_);
                TranslateMessage(&message);
                DispatchMessageW(&message);
                continue;
            }
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
                else { native_error_ = GetLastError(); if (!native_error_) native_error_ = ERROR_GEN_FAILURE; }
                break;
            }
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
        return true;
    }

    DWORD Error() const { return native_error_; }

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
        external_listing_.reset();
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
        current_ = path;
        selected_ = anchor_ = -1;
        selected_set_.clear(); select_after_.clear(); typeahead_.clear();
        scroll_ = 0; error_.clear();
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

    // By value: callers pass entry paths that the reset below destroys.
    // `verified`: the folder is known to exist (it was just listed as a row, or
    // is the parent of a folder that listed), so it may be picked while it
    // loads. Typed, remembered and history paths wait for their listing.
    void Navigate(std::wstring path, bool push_history, std::wstring select_after = {},
                  bool verified = false) {
        if (external_navigation_) return;
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
        if (push_history && moved) history_.Navigate(current_);
        if (moved && searching_) {
            // A search belongs to the folder it was started in.
            searching_ = false;
            search_text_.clear();
            options_.search.clear();
            if (search_edit_) { setting_edits_ = true; SetWindowTextW(search_edit_, L""); setting_edits_ = false; }
        }
        current_ = path;
        if (spec_.on_dialog_event && moved) {
            embedded_folder_pending_ = true;
            embedded_folder_committed_ = false;
        }
        current_verified_ = verified;
        entries_.clear();
        snapshot_.reset();
        typeahead_.clear();
        selected_ = -1;
        selected_set_.clear();
        anchor_ = -1;
        chrome_.pressed = kPickNone;
        validating_ = false;
        chrome_.notice.clear();
        if (moved) scroll_ = 0.0f;
        reload_notice_.clear();
        error_.clear();
        waiting_ = true;
        loading_ = false;
        select_after_ = std::move(select_after);
        EndAddressEdit(false);
        UpdateChosen();
        if (loader_) loader_->Load(++generation_, path, mode_, options_);
        if (hwnd_) {
            SetTimer(hwnd_, kLoadTimer, kSpinnerDelayMs, nullptr);
            Invalidate();
        }
    }

    void GoUp() {
        if (current_.empty()) return;
        const std::wstring child = current_;
        Navigate(PickerParent(child), true, child, current_verified_);
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
        const float scroll = scroll_;
        Navigate(current_, false, keep, current_verified_);
        scroll_ = scroll;
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
                FailPaintedEdit(listing->error);
                return;
            }
            error_ = ErrorText(listing->error, listing->path);
            if (error_.empty()) error_ = l10n::Get(l10n::StringId::PickerOpenFailed);
        }
        initial_load_ = false;
        const bool notify_folder = embedded_folder_pending_ && listing->error == ERROR_SUCCESS;
        embedded_folder_pending_ = false;
        entries_ = std::move(listing->entries);
        snapshot_ = std::move(listing->snapshot);
        if (current_.empty()) RefreshDrivesFromListing();
        if (!select_after_.empty()) {
            for (size_t i = 0; i < entries_.size(); ++i) {
                if (SamePickerPath(entries_[i].path, select_after_)) {
                    Select(static_cast<int>(i), false, false, false, false);
                    break;
                }
            }
            select_after_.clear();
        }
        ClampScroll();
        UpdateChosen();
        if (notify_folder && embedded_ready_ && NotifyEmbedded(PickerDialogEvent::FolderChanged) != S_OK)
            FailPaintedEdit(ERROR_FUNCTION_FAILED);
        if (!external_navigation_queue_.empty()) PostMessageW(hwnd_, kExternalNavigationMessage, 0, 0);
        if (!reload_notice_.empty()) chrome_.notice = std::exchange(reload_notice_, {});
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
            }
        }
    }

    const PickerEntry* SelectedEntry() const {
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
        try { hr = spec_.on_dialog_event(hwnd_, kind, embedded_state_, proposed); } catch (...) {}
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
        const std::wstring selection=selections.empty() ? std::wstring{} : selections.front();
        const bool selection_changed = embedded_state_.selections != selections;
        const bool type_changed = embedded_state_.filter_index != options_.filter_index;
        const std::wstring published_folder = current_.empty() && current_verified_ ? picker_navigation::kComputerPath : current_;
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
    void UpdateChosen() {
        if (pending_overwrite_ && (!validating_ || overwrite_generation_ != generation_)) ClearOverwrite();
        PublishEmbeddedState();
        const bool no_embedded_folder = spec_.on_dialog_event && !embedded_folder_committed_;
        if (filename_edit_) EnableWindow(filename_edit_, !validating_ && !waiting_ && !no_embedded_folder);
        if (!no_embedded_folder) RestoreSaveFocus();
        chosen_.clear();
        if (validating_ || waiting_ || no_embedded_folder) return;
        if (!filename_text_.empty()) {
            chosen_ = filename_text_;
            return;
        }
        // Folder mode: the selected folder, else the folder being shown. This
        // PC itself is not a folder that can be returned.
        if (mode_ == PickerMode::Folder && error_.empty())
            chosen_ = PickerChosenPath(mode_, current_, SelectedEntry());
    }

    void ClearOverwrite() {
        pending_overwrite_.reset();
        chrome_.overwrite_pending = false;
        if (hwnd_) RemovePropW(hwnd_, L"Pulse.PickerOverwriteGeneration");
        chrome_.notice.clear();
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
                const auto leaf = PickerLeafName(snapshot.Path());
                const auto dot = leaf.find_last_of(L'.');
                const auto stem = dot == leaf.npos ? leaf : leaf.substr(0, dot);
                const auto extension = dot == leaf.npos ? std::wstring{} : leaf.substr(dot);
                std::wstring candidate;
                for (unsigned suffix = 2; suffix < 10000; ++suffix) {
                    candidate = stem + L" (" + std::to_wstring(suffix) + L")" + extension;
                    if (std::none_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
                        return CompareStringOrdinal(entry.name.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL;
                    })) break;
                }
                SetFilename(std::move(candidate));
            }
            UpdateChosen(); FocusFilename();
            if (rename) SubmitSelection();
        }
        UpdateChosen(); Invalidate();
    }

    void SetFilename(std::wstring text) {
        filename_text_ = std::move(text);
        setting_edits_ = true;
        if (filename_edit_) SetWindowTextW(filename_edit_, filename_text_.c_str());
        setting_edits_ = false;
        UpdateChosen();
    }

    void SyncSelectedNames() {
        std::vector<int> picked(selected_set_.begin(), selected_set_.end());
        std::sort(picked.begin(), picked.end());
        std::wstring text;
        for (int i : picked) {
            if (i < 0 || i >= static_cast<int>(entries_.size())) continue;
            const PickerEntry& entry = entries_[static_cast<size_t>(i)];
            // Folders are opened, not returned: they do not go into the name.
            if (mode_ != PickerMode::Folder && (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive)) continue;
            if (!text.empty()) text += L" ";
            text += picked.size() > 1 ? L"\"" + entry.name + L"\"" : entry.name;
        }
        if (mode_ != PickerMode::Save || !text.empty()) { SetFilename(std::move(text)); filename_from_selection_ = true; }
    }

    void Select(int index, bool extend = false, bool toggle = false, bool focus_only = false,
                bool reveal = true) {
        if (validating_) { ++generation_; validating_ = false; }
        editing_selection_names_ = false;
        const int count = static_cast<int>(entries_.size());
        if (!count) { selected_ = -1; selected_set_.clear(); UpdateChosen(); return; }
        index = std::clamp(index, 0, count - 1);
        selected_ = index;
        if (!focus_only) {
            if (allow_multiselect_ && extend && anchor_ >= 0) {
                if (!toggle) selected_set_.clear();
                for (int i = (std::min)(anchor_, index); i <= (std::max)(anchor_, index); ++i)
                    selected_set_.insert(i);
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
        anchor_ = -1;
        if (mode_ != PickerMode::Folder && mode_ != PickerMode::Save) SetFilename(L"");
        UpdateChosen();
        Invalidate();
    }

    void OpenEntry(int index) {
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        const PickerEntry entry = entries_[static_cast<size_t>(index)];
        if (entry.kind == PickerEntryKind::Image || entry.kind == PickerEntryKind::File) {
            SubmitSelection();
        } else {
            if (mode_ != PickerMode::Save) SetFilename(L"");
            Navigate(entry.path, true, {}, true);
        }
    }

    void ValidateNames(std::vector<std::wstring> names, bool typed_names = false) {
        if (!loader_ || validating_ || waiting_ || names.empty()) return;
        if (!allow_multiselect_ && names.size() > 1) {
            chrome_.notice = l10n::Get(l10n::StringId::SelectOneFileHere).c_str();
            Invalidate();
            return;
        }
        validation_navigation_only_ = false;
        RememberSaveFocus();
        validating_ = true;
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
            request.no_test_file_create = spec_.no_test_file_create;
            request.require_existing_save_parent = spec_.require_existing_save_parent;
            loader_->ValidateSave(++generation_, std::move(request));
        } else {
            auto validation_options = options_;
            validation_options.use_default_extension = spec_.use_open_default_extension;
            validation_options.default_extension = spec_.default_extension;
            validation_options.extension_filter_changed = open_filter_changed_;
            validation_options.typed_names = typed_names;
            loader_->Validate(++generation_, current_, std::move(names), mode_, std::move(validation_options));
        }
        Invalidate();
    }

    void SubmitSelection() {
        if (spec_.on_dialog_event && !embedded_folder_committed_) return;
        if (!FlushPaintedEdits()) return;
        if (validating_) return;
        std::vector<std::wstring> names;
        if (mode_ == PickerMode::Folder && filename_from_selection_ && !selected_set_.empty()) {
            std::vector<int> picked(selected_set_.begin(), selected_set_.end());
            std::sort(picked.begin(), picked.end());
            for (int index : picked) if (index >= 0 && static_cast<size_t>(index) < entries_.size()) {
                const auto& entry = entries_[static_cast<size_t>(index)];
                if (entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive) names.push_back(entry.path);
            }
        } else if (mode_ == PickerMode::File && filename_from_selection_ && !selected_set_.empty()) {
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
                chrome_.notice = l10n::Get(l10n::StringId::UseMatchingQuotesAroundEachFilename).c_str();
                Invalidate();
                return;
            }
        } else if (!chosen_.empty()) {
            names.push_back(chosen_);
        }
        ValidateNames(std::move(names), mode_ == PickerMode::File && !filename_from_selection_);
    }

    void OnValidation(std::unique_ptr<PickerValidation> validation) {
        if (!CheckPaintedEdits()) return;
        if (!validation || done_ || validation->generation != generation_ || !validating_ ||
            (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) return;
        validating_ = false;
        if (validation->save_ui_generation && validation->save_ui_generation != embedded_state_.generation) {
            ClearEmbeddedCandidate(); UpdateChosen(); Invalidate(); return;
        }
        chrome_.notice.clear();
        if (validation->save_access_diagnostic && (validation->save_access_diagnostic->cleanup == SaveAccessCleanup::Unconfirmed ||
            validation->save_access_diagnostic->cleanup_error != ERROR_SUCCESS)) {
            FailPaintedEdit(validation->error ? validation->error : ERROR_FUNCTION_FAILED); return;
        }
        if (validation->error != ERROR_SUCCESS) {
            if (spec_.validate_local_save_access_v1) ClearEmbeddedCandidate();
            chrome_.notice = validation->error == ERROR_UNSUPPORTED_TYPE
                ? l10n::Get(l10n::StringId::TheSelectedFileDoesNotMatchTheFileType).c_str()
                : ErrorText(validation->error, validation->failed_path);
            if (chrome_.notice.empty()) chrome_.notice = l10n::Get(l10n::StringId::PickerOpenFailed);
            if (waiting_ && loader_) loader_->Load(++generation_, current_, mode_, options_);
            UpdateChosen();
            Invalidate();
            return;
        }
        if (!validation->navigate.empty()) {
            if (mode_ != PickerMode::Folder && mode_ != PickerMode::Save) SetFilename(L"");
            Navigate(validation->navigate, true);
            FocusList();
            return;
        }
        if (validation->paths.empty()) { UpdateChosen(); return; }
        if (validation_navigation_only_) {
            Navigate(PickerParent(validation->paths.front()), true, validation->paths.front());
            FocusList();
            return;
        }
        if (mode_ == PickerMode::Save && !validation->save_snapshot) {
            chrome_.notice = ErrorText(ERROR_INVALID_DATA, validation->paths.front());
            UpdateChosen();
            Invalidate();
            return;
        }
        if (validation->overwrite_required) {
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
            loader_->CheckSaveAccess(++generation_, std::move(*validation->save_snapshot), false, 0, spec_.require_unencrypted_save, spec_.skip_access_validation);
            Invalidate(); return;
        }
        if (!FlushPaintedEdits()) return;
        if (spec_.on_dialog_event && !validation->save_file_ok_checked) {
            PublishEmbeddedState();
            if (done_) return;
            embedded_state_.candidate = validation->paths.front(); embedded_state_.candidates=validation->paths;
            ++embedded_state_.generation;
            const auto candidate_generation = embedded_state_.generation;
            const HRESULT accepted = NotifyEmbedded(PickerDialogEvent::FileOk);
            if (!FlushPaintedEdits()) return;
            if (done_ || candidate_generation != embedded_state_.generation ||
                validation->generation != generation_ ||
                (spec_.owner_guard && !spec_.owner_guard->Alive()) ||
                (spec_.cancel_event && WaitForSingleObject(spec_.cancel_event, 0) == WAIT_OBJECT_0)) {
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
                native_error_ = ERROR_FUNCTION_FAILED; done_ = true;
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
            loader_->CheckSaveAccess(++generation_, std::move(*validation->save_snapshot), true, embedded_state_.generation, spec_.require_unencrypted_save, spec_.skip_access_validation);
            Invalidate(); return;
        }
        if (spec_.owner_guard && !spec_.owner_guard->Alive()) { result_.clear(); done_ = true; return; }
        result_ = std::move(validation->paths);
        g_last_folder[mode_index_] = current_.empty() ? PickerParent(result_.front()) : current_;
        g_last_view[mode_index_] = view_mode_;
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void Cancel() {
        if (operating_) return;
        result_.clear();
        done_ = true;
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void Close() noexcept {
        done_ = true;
        if (!hwnd_) return;
        RemoveClipboardFormatListener(hwnd_);
        KillTimer(hwnd_, kLoadTimer);
        KillTimer(hwnd_, kSearchTimer);
        KillTimer(hwnd_, kMotionTimer);
        // Stop the loader first: after this no listing can be posted, so the
        // ones already queued are all that must be freed.
        PickerProofPhase("close-loader-before");
        loader_.reset();
        PickerProofPhase("close-loader-after");
        MSG pending{};
        while (PeekMessageW(&pending, hwnd_, kListingMessage, kListingMessage, PM_REMOVE))
            PickerLoader::Take(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kFolderCommitMessage, kFolderCommitMessage, PM_REMOVE))
            PickerLoader::TakeFolderCommit(pending.lParam);
        while (PeekMessageW(&pending, hwnd_, kValidationMessage, kValidationMessage, PM_REMOVE))
            PickerLoader::TakeValidation(pending.lParam);
        if (IsWindow(hwnd_)) {
            if(spec_.owner_guard) spec_.owner_guard->RestoreBeforeDialogHide(hwnd_);
            PickerProofPhase("close-hide-before");
            HideComposedDialog(hwnd_); // Identity-bound guard restores modality before the popup disappears.
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
    float Height() const { return static_cast<float>(compositor_.Height()); }
    D2D1_RECT_F ClientRectF() const { return D2D1::RectF(0, 0, Width(), Height()); }
    bool PropertySidebar() const {
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

    void BuildSidebar() {
        vm_.sidebar.clear();
        auto add_context = [&](int id, const std::wstring& label, bool opened) {
            SidebarGroup group; group.id = id; group.header = label; group.collapsed = collapsed_[id - 1];
            for (const auto& entry : pulse_places_.folders) {
                if (entry.open != opened || (!opened && entry.pinned)) continue;
                picker_user_storage::Path parsed;
                if (!picker_user_storage::Parse(entry.path, parsed)) continue;
                group.items.push_back(PlaceItem({entry.label.empty() ? PickerLeafName(parsed.value) : entry.label, parsed.value, L"\xE8B7"}));
            }
            if (!group.items.empty()) vm_.sidebar.push_back(std::move(group));
        };
        if (!spec_.places.empty()) {
            SidebarGroup caller; caller.id = 6; caller.header = l10n::Get(l10n::StringId::PickerApplicationPlaces); caller.collapsed = collapsed_[5];
            for (const auto& place : spec_.places) caller.items.push_back(PlaceItem(place));
            vm_.sidebar.push_back(std::move(caller));
        }
        add_context(4, l10n::Get(l10n::StringId::PickerPulseOpenPlaces), true);
        add_context(5, l10n::Get(l10n::StringId::Recent), false);
        SidebarGroup quick;
        quick.id = kGroupQuick;
        quick.header = l10n::Get(l10n::StringId::PickerQuickAccess);
        quick.collapsed = collapsed_[0];
        for (const PickerPlace& place : places_) {
            if (std::any_of(spec_.places.begin(), spec_.places.end(), [&](const auto& added) { return SamePickerPath(added.path, place.path); })) continue;
            quick.items.push_back(PlaceItem(place));
        }
        for (const auto& entry : pulse_places_.folders) {
            if (!entry.pinned) continue;
            picker_user_storage::Path parsed;
            if (!picker_user_storage::Parse(entry.path, parsed) || std::any_of(quick.items.begin(), quick.items.end(), [&](const auto& item) { return SamePickerPath(item.path, parsed.value); })) continue;
            quick.items.push_back(PlaceItem({entry.label.empty() ? PickerLeafName(parsed.value) : entry.label, parsed.value, L"\xE8B7"}));
        }
        vm_.sidebar.push_back(std::move(quick));

        SidebarGroup drives;
        drives.id = kGroupDrives;
        drives.header = l10n::Get(l10n::StringId::ThisPc);
        drives.collapsed = collapsed_[2];
        drives.navigable = true;
        drives.navigation_path = L"";
        drives.items = drives_;
        vm_.sidebar.push_back(std::move(drives));
    }

    int CollapseSlot(int group_id) const {
        return std::clamp(group_id - 1, 0, 5);
    }

    // The main window's view model, reduced to what the embedded renderer
    // draws: the address row, the sidebar and one pane.
    void SyncVm() {
        vm_.dark = dark_;
        vm_.focused = GetForegroundWindow() == hwnd_;
        vm_.can_go_back = history_.CanGoBack();
        vm_.can_go_forward = history_.CanGoForward();
        vm_.tray_deck.hidden = true;
        vm_.details_visible = false;
        vm_.settings_open = false;
        vm_.window_effect = WindowEffect::MicaAlt;
        vm_.address_searching = searching_;
        vm_.address_editing = searching_ ? true : address_editing_;
        vm_.address_search_current = true;
        vm_.address_search_content = false;
        vm_.address_search_text = search_text_;
        vm_.address_search_has_text = !search_text_.empty();
        vm_.address_search_scope_label = l10n::Get(l10n::StringId::ThisFolder).c_str();
        BuildSidebar();

        PaneViewModel& pane = vm_.pane;
        pane.cut_names.clear();
        for (const auto& cut_path : cut_paths_) {
            if (SamePickerPath(PickerParent(cut_path), current_))
                pane.cut_names.insert(PickerLeafName(cut_path));
        }
        pane.path = current_;
        pane.header_text.clear();
        pane.snapshot = snapshot_;
        pane.entries.clear();
        pane.is_file_system = !current_.empty();
        pane.can_go_back = vm_.can_go_back;
        pane.can_go_forward = vm_.can_go_forward;
        pane.can_go_up = !current_.empty();
        pane.can_create = !current_.empty() && error_.empty() && !waiting_;
        pane.selected_index = selected_;
        pane.selected_indices = &selected_set_;
        pane.selected_count = static_cast<int>(selected_set_.size());
        pane.view_mode = view_mode_;
        pane.sort_column = ColumnFromSort(options_.sort);
        pane.sort_direction = options_.descending ? SortDirection::Desc : SortDirection::Asc;
        pane.focused = focus_ == kPickList;
        pane.scroll_y = scroll_;
        pane.scroll_x = 0.0f;
        pane.loading = waiting_ && loading_;
        pane.is_search = false;
        pane.banner_kind = error_.empty() ? 0 : 3;
        pane.banner_title.clear();
        pane.banner_message = error_;
        pane.hover_index = vm_.hover_region == static_cast<int>(HitTestResult::Row)
                               ? vm_.hover_control_index : -1;
        vm_.breadcrumb_hover = vm_.hover_region == static_cast<int>(HitTestResult::BreadcrumbSegment)
                                   ? vm_.hover_control_index : -1;

        chrome_.selected_names.clear();
        selection_tag_indices_.clear();
        if (allow_multiselect_ && filename_from_selection_ && !editing_selection_names_) {
            std::vector<int> sorted(selected_set_.begin(), selected_set_.end()); std::sort(sorted.begin(), sorted.end());
            for (int index : sorted) if (index >= 0 && index < static_cast<int>(entries_.size())) {
                const auto& entry = entries_[static_cast<size_t>(index)];
                if (entry.kind != PickerEntryKind::Folder && entry.kind != PickerEntryKind::Drive) {
                    chrome_.selected_names.push_back(entry.name); selection_tag_indices_.push_back(index);
                }
            }
        }
        chrome_.primary_enabled = chrome_.overwrite_pending || (!waiting_ && !validating_ && !chosen_.empty());
        chrome_.primary_text = chrome_.overwrite_pending ? l10n::Get(l10n::StringId::OpReplaceFile) : normal_primary_text_;
        chrome_.overwrite_back_text = l10n::Get(l10n::StringId::PickerBackToEdit);
        chrome_.rename_text = l10n::Get(l10n::StringId::PickerRenameSave);
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

    void UpdateSummary() {
        chrome_.summary.clear();
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
                request.no_test_file_create = spec_.no_test_file_create;
                request.require_existing_save_parent = spec_.require_existing_save_parent;
                const auto target = ResolveSaveTarget(request);
                if (!target.error) shown = target.path;
            }
            chrome_.summary = prefix + FitPathMiddle(shown, width - measure(prefix), measure);
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

    void ClampScroll() {
        if (!compositor_.Dc()) return;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, MaxScroll()));
    }

    void EnsureVisible(int index) {
        if (index < 0 || !compositor_.Dc()) return;
        SyncVm();
        const D2D1_RECT_F bounds = PaneBounds();
        const D2D1_RECT_F list = renderer_.PaneListRect(vm_.pane, bounds);
        const D2D1_RECT_F item = renderer_.ItemRectInPane(vm_.pane, bounds, index);
        if (item.top < list.top) scroll_ += item.top - list.top;
        else if (item.bottom > list.bottom) scroll_ += item.bottom - list.bottom;
        ClampScroll();
    }

    // ---- hosted edits ------------------------------------------------------

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }
    HBRUSH EditBrush() const { return EditBackBrush(edit_brush_); }

    void CreateFonts() {
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        font_ = typography::CreateEditFont(scale_);
        for (HWND edit : AllEdits())
            if (edit && font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    HWND MakeEdit(int id, const wchar_t* text) {
        HWND edit = spec_.pulse_painted_plain_child ? CreatePulsePaintedChildEdit(hwnd_, text)
            : CreateRedirectedChildEdit(hwnd_, text);
        if (!edit) {
            if (spec_.pulse_painted_plain_child) {
                const DWORD error = GetLastError(); native_error_ = error ? error : ERROR_GEN_FAILURE;
            }
            return nullptr;
        }
        SetWindowLongPtrW(edit, GWLP_ID, id);
        SetWindowTheme(edit, L"", L"");
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (!SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this)) && spec_.pulse_painted_plain_child) {
            const DWORD error = GetLastError(); native_error_ = error ? error : ERROR_GEN_FAILURE;
            DestroyWindow(edit); return nullptr;
        }
        return edit;
    }

    std::vector<HWND> AllEdits() const {
        std::vector<HWND> edits{path_edit_,filename_edit_,search_edit_};
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

    void FailPaintedEdit(DWORD error) {
        native_error_ = error ? error : ERROR_FUNCTION_FAILED;
        result_.clear(); done_ = true;
        if (hwnd_) PostMessageW(hwnd_, WM_NULL, 0, 0);
    }
    bool CheckPaintedEdits() {
        if (!spec_.pulse_painted_plain_child) return true;
        if (!compositor_.CustomEditEnabled()) { FailPaintedEdit(ERROR_NOT_SUPPORTED); return false; }
        for (HWND edit : AllEdits()) {
            const DWORD error = edit ? PulsePaintedChildEditError(edit) : ERROR_SUCCESS;
            if (error) { FailPaintedEdit(error); return false; }
        }
        return !done_;
    }
    void PresentEdits() {
        if (!CheckPaintedEdits()) return;
        for (HWND edit : AllEdits()) {
            if (edit && IsWindowVisible(edit) &&
                !PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), edit) &&
                spec_.pulse_painted_plain_child) { FailPaintedEdit(PulsePaintedChildEditError(edit)); return; }
        }
    }

    bool FlushPaintedEdits() {
        if (!spec_.pulse_painted_plain_child) return true;
        if(first_present_retrying_)return false;
        if (rendering_frame_) { FailPaintedEdit(ERROR_BUSY); return false; }
        if (!CheckPaintedEdits()) return false;
        const auto previous = painted_frame_;
        PresentEdits();
        if (!done_) UpdateWindow(hwnd_);
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
        if (painted_frame_ == previous) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return false; }
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
            if (!DrawPulsePaintedChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), edit, bounds)) {
                FailPaintedEdit(PulsePaintedChildEditError(edit)); return;
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
        PlaceOneEdit(filename_edit_, chrome_layout_.filename, chrome_.selected_names.size() <= 1);
        PlaceCustomEdits();
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
        validation_navigation_only_ = true;
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
            filename_text_ = ReadEdit(edit);
            filename_from_selection_ = false;
            selected_set_.clear();
            selected_ = -1;
            anchor_ = -1;
        } else if (edit == search_edit_) {
            search_text_ = ReadEdit(edit);
            SetTimer(hwnd_, kSearchTimer, 180, nullptr);
        }
        UpdateChosen();
        Invalidate();
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<FolderPickerWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) {
                if (hwnd == self->path_edit_) self->SubmitPath();
                else if (hwnd == self->filename_edit_) self->SubmitSelection();
                else if(hwnd==self->search_edit_) { self->ApplySearchNow(); self->FocusList(); }
                else self->SubmitSelection();
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                if (hwnd == self->path_edit_) self->EndAddressEdit(true);
                else if (hwnd == self->search_edit_) {
                    if (!ReadEdit(hwnd).empty()) SetWindowTextW(hwnd, L"");
                    else self->ExitSearch();
                } else self->Cancel();
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
        case WM_SYSKEYDOWN:
            if (wparam == VK_LEFT) { self->GoBack(); return 0; }
            if (wparam == VK_RIGHT) { self->GoForward(); return 0; }
            if (wparam == VK_UP) { self->GoUp(); return 0; }
            if (wparam == 'D') { self->BeginAddressEdit(); return 0; }
            if (wparam == 'N' && self->filename_edit_) { self->FocusFilename(); return 0; }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        case WM_SETFOCUS:
            PickerProofFocus("edit-setfocus", self->focus_, GetDlgCtrlID(hwnd));
            self->focus_ = GetDlgCtrlID(hwnd)>=kCustomEditBase ? kCustomFocusBase+GetDlgCtrlID(hwnd)-kCustomEditBase : hwnd == self->path_edit_ ? kPickAddress
                         : hwnd == self->filename_edit_ ? kPickFilename : kPickSearch;
            break;
        case WM_KILLFOCUS:
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
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(),
                                    self->EditForeground(), self->EditBackground(),
                                    self->EditBrush(), hwnd, msg, wparam, lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(),
                                               self->EditForeground(), self->EditBackground(),
                                               hwnd, msg, wparam, lparam);
        }
        if(msg==WM_CHAR)PickerProofPhase("edit-char-dispatch-after");
        if (repaint && self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    // ---- layout and painting ----------------------------------------------

    void ApplyRendererSettings() {
        renderer_.SetResourceModule(spec_.module_instance);
        renderer_.SetEmbedded(kPickerTitleDip, PickerFooterDip(mode_) + CustomHeightDip());
        renderer_.SetSidebarWidthDip(kSidebarDip);
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
    bool CompactCustom() const {
        bool any = false;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            const auto& control = spec_.custom_controls[i];
            if (!(EffectiveControl(i) & 2) || control.kind == 5) continue;
            if (control.property_layout || control.kind == 1 || control.kind == 6 || control.has_parent) return false;
            any = true;
        }
        return any;
    }
    size_t CompactCustomCount() const {
        size_t count = 0;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i)
            if (spec_.custom_controls[i].kind != 5 && (EffectiveControl(i) & 2)) ++count;
        return count;
    }
    size_t CompactCustomColumns() const { return Width() / std::max(0.1f, scale_) >= 760 ? 2 : 1; }
    PickerCustomSections CustomSections() const {
        if (CompactCustom()) return {0, 8.0f + 40.0f * static_cast<float>((CompactCustomCount() + CompactCustomColumns() - 1) / CompactCustomColumns())};
        float normal = 0, prominent = 0;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            if (spec_.custom_controls[i].kind == 5 || !(EffectiveControl(i) & 2)) continue;
            (ProminentControl(i) ? prominent : normal) += CustomRowHeight(i);
        }
        return LayoutPickerCustomSections(normal, prominent, CustomHeightDip());
    }
    float CustomHeightDip() const {
        if (PropertySidebar()) return 0.0f;
        if (CompactCustom()) return std::min(128.0f, 8.0f + 40.0f * static_cast<float>((CompactCustomCount() + CompactCustomColumns() - 1) / CompactCustomColumns()));
        bool prominent = false;
        float content = 20.0f;
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            if (spec_.custom_controls[i].kind == 5 || !(EffectiveControl(i) & 2)) continue;
            prominent |= ProminentControl(i);
            content += CustomRowHeight(i);
        }
        if (prominent) return std::clamp(content, 120.0f, 220.0f);
        for (const auto& control : spec_.custom_controls) if (control.property_layout)
            return std::clamp(Height() / std::max(0.1f, scale_) - 260.0f, 120.0f, 220.0f);
        for (size_t i=0;i<spec_.custom_controls.size();++i) if (spec_.custom_controls[i].kind != 5 && (EffectiveControl(i)&2)) return 120.0f;
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
        if(control.kind==1)return 26.0f;
        if(control.kind==6) {
            size_t count=0;for(const auto& item:control.items)if(item.state&2)++count;
            return static_cast<float>(count)*30.0f;
        }
        return 38.0f;
    }
    D2D1_RECT_F RadioRect(size_t index,size_t item_index) const {
        auto rect=CustomRect(index);size_t row=0;
        for(size_t i=0;i<item_index;++i)if(spec_.custom_controls[index].items[i].state&2)++row;
        rect.top+=static_cast<float>(row)*30.0f*scale_;rect.bottom=rect.top+28.0f*scale_;return rect;
    }
    D2D1_RECT_F CustomRect(size_t index) const {
        if (CompactCustom()) {
            size_t ordinal = 0;
            for (size_t i = 0; i < index; ++i) if (spec_.custom_controls[i].kind != 5 && (EffectiveControl(i) & 2)) ++ordinal;
            const auto columns = CompactCustomColumns();
            const float width = (Width() - 32 * scale_ - static_cast<float>(columns - 1) * 12 * scale_) / static_cast<float>(columns);
            const float x = 16 * scale_ + static_cast<float>(ordinal % columns) * (width + 12 * scale_);
            const float y = CustomPanel().top + (4 + 40 * static_cast<float>(ordinal / columns)) * scale_ - custom_scroll_;
            return D2D1::RectF(x, y, x + width, y + 36 * scale_);
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
        if (spec_.custom_controls.empty()) return;
        auto* dc = compositor_.Dc();
        if (PropertySidebar()) painter_.FillRoundedRect(CustomPanel(), 8 * scale_, theme.surface_card);
        dc->PushAxisAlignedClip(CustomPanel(), D2D1_ANTIALIAS_MODE_ALIASED);
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i) {
            const auto state = EffectiveControl(i);
            if (!(state & 2) || spec_.custom_controls[i].kind == 5) continue;
            const auto rect = CustomRect(i);
            const auto& control = spec_.custom_controls[i];
            if (control.property_layout && (rect.top < CustomPanel().top || rect.bottom > CustomPanel().bottom)) continue;
            std::wstring caption=control.label;
            if(control.kind==3 && control.has_selected) for(const auto& item:control.items)
                if(item.id==control.selected_item_id) { caption=item.label;break; }
            const auto label = (control.property_layout == 2 || control.property_layout == 3) ? caption : FitTextEnd(caption, std::max(0.0f, rect.right - rect.left - 32 * scale_),
                [&](std::wstring_view text) { return typography::MeasureLine(&compositor_, compositor_.TextFormat(), text); });
            if (control.kind == 1) painter_.DrawText(label, rect, compositor_.TextFormat(), control.prominent ? theme.text : theme.text_secondary);
            else {
                fluent::ControlState visual{}; visual.enabled = (state & 1) != 0;
                const int id = kCustomFocusBase + static_cast<int>(i);
                visual.hovered = chrome_.hover == id; visual.pressed = chrome_.pressed == id;
                visual.keyboard_focus = chrome_.show_focus && focus_ == id;
                if(control.kind==8) {visual.focused=GetFocus()==custom_edits_[i];painter_.DrawTextFieldFrame(rect,visual,true);continue;}
                if(control.kind==7) {visual.checked=control.checked;painter_.DrawCheckBox(rect,label,visual);continue;}
                if(control.kind==6) {
                    for(size_t j=0;j<control.items.size();++j) {
                        const auto& item=control.items[j];if(!(item.state&2))continue;
                        auto row_state=visual;row_state.enabled=visual.enabled && (item.state&1)!=0;
                        row_state.checked=control.has_selected && control.selected_item_id==item.id;
                        painter_.DrawRadioButton(RadioRect(i,j),item.label,row_state);
                    }
                    continue;
                }
                fluent::ButtonSpec button{rect,label,{},fluent::ButtonKind::Standard,visual};
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
        if(found->items.size()!=control.items.size()) return E_NOTIMPL;
        for(size_t i=0;i<found->items.size();++i) if(found->items[i].id!=control.items[i].id) return E_NOTIMPL;
        // All potentially allocating copies precede mutation. If publishing the
        // snapshot fails, restore local state without allocating and end Show:
        // the adapter may have partially copied its snapshot before failing.
        PickerCustomControl previous=control;
        PickerDialogSnapshot previous_snapshot = embedded_state_;
        std::swap(*found,previous);
        embedded_state_.candidate.clear(); embedded_state_.candidates.clear();
        ++embedded_state_.generation;
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

    void Relayout() {
        if (!compositor_.Dc()) return;
        renderer_.SetEmbedded(kPickerTitleDip, PickerFooterDip(mode_) + CustomHeightDip());
        SyncVm();
        SyncOpenDropDown();
        chrome_layout_ = LayoutPickerChrome(Width(), Height(), chrome_, painter_, scale_);
        ClampScroll();
        PlaceEdits();
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
        PULSE_PICKER_TRACE_POINT("list-before");
        renderer_.Render(vm_, RendererRect(), theme);
        PULSE_PICKER_TRACE_POINT("list-after");
        painter_.BeginFrame(theme, hc);
        UpdateSummary();
        SyncOpenDropDown();
        const auto motion_now = motion::NowMs();
        const float filter_t = filter_motion_start_ ? std::clamp(
            static_cast<float>(motion_now - filter_motion_start_) / 120.0f, 0.0f, 1.0f) : 1.0f;
        chrome_.filter_turn = filter_motion_from_ +
            ((filter_open_ ? 1.0f : 0.0f) - filter_motion_from_) * motion::EaseOutCubic(filter_t);
        PULSE_PICKER_TRACE_POINT("chrome-before");
        chrome_layout_ = LayoutPickerChrome(Width(), Height(), chrome_, painter_, scale_);
        if (filename_edit_ && (IsWindowVisible(filename_edit_) != FALSE) != (chrome_.selected_names.size() <= 1)) PlaceEdits();
        DrawPickerChrome(compositor_, painter_, theme, chrome_, chrome_layout_, hc);
        DrawCustom(theme);
        PULSE_PICKER_TRACE_POINT("chrome-after");
        PULSE_PICKER_TRACE_POINT("painted-edits-before");
        DrawPaintedEdits();
        PULSE_PICKER_TRACE_POINT("painted-edits-after");
        PULSE_PICKER_TRACE_POINT("main-end-draw-before");
        const HRESULT end_hr = dc->EndDraw();
        PULSE_PICKER_TRACE_POINT("main-end-draw-after");
        if (end_hr == D2DERR_RECREATE_TARGET || end_hr == DXGI_ERROR_DEVICE_REMOVED ||
            end_hr == DXGI_ERROR_DEVICE_RESET || end_hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
            compositor_.NotifyDeviceLost(end_hr);
            Invalidate();
            return;
        }
        if (spec_.pulse_painted_plain_child && FAILED(end_hr)) { FailPaintedEdit(ERROR_FUNCTION_FAILED); return; }
        if (const auto snapshot = reinterpret_cast<const wchar_t*>(
                RemovePropW(hwnd_, L"Pulse.PickerTestSnapshot")))
            compositor_.SaveSnapshot(snapshot);
        PULSE_PICKER_TRACE_POINT("main-present-before");
        const HRESULT present_hr = compositor_.Present();
        PULSE_PICKER_TRACE_POINT("main-present-after");
        if (preparing_first_frame_) {
            initial_frame_prepared_ = present_hr == S_OK && !compositor_.NeedsRecovery();
            if (FAILED(present_hr)) FailPaintedEdit(ERROR_FUNCTION_FAILED);
            return;
        }
        if (spec_.pulse_painted_plain_child) {
            first_present_occluded_=present_hr==DXGI_STATUS_OCCLUDED && painted_frame_==0;
            if(first_present_occluded_){PickerProofPhase("first-present-occluded-retry");return;}
            if (present_hr != S_OK) {
                PickerProofResult("strict-present-state-is-recovery", present_hr, ERROR_SUCCESS, compositor_.NeedsRecovery());
                FailPaintedEdit(ERROR_FUNCTION_FAILED); return;
            }
            if (!done_ && !compositor_.NeedsRecovery()) ++painted_frame_;
        } else PresentEdits();
        // List hover glides, view morphs, the loading shimmer and thumbnail
        // fades run on the shared renderer's clock.
        if (renderer_.TickMotion(GetTickCount64()) || filter_t < 1.0f) SetTimer(hwnd_, kMotionTimer, 16, nullptr);
        else KillTimer(hwnd_, kMotionTimer);
    }

    // ---- hit testing -------------------------------------------------------

    struct Hit {
        int chrome = kPickNone;
        HitTestResult main;
    };

    Hit HitAt(float x, float y) {
        Hit hit;
        hit.chrome = HitTestPickerChrome(chrome_layout_, chrome_, x, y);
        if (!spec_.custom_controls.empty() && ContainsRect(CustomPanel(), x, y)) {
            for (size_t i = 0; i < spec_.custom_controls.size(); ++i)
                if (spec_.custom_controls[i].kind != 1 && spec_.custom_controls[i].kind != 5 && EffectiveControl(i) == 3 && ContainsRect(CustomRect(i), x, y))
                    hit.chrome = kCustomFocusBase + static_cast<int>(i);
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

    void UpdateHover(const Hit& hit) {
        const int region = static_cast<int>(hit.main.region);
        const bool changed = chrome_.hover != hit.chrome || vm_.hover_region != region ||
                             vm_.hover_control_index != hit.main.index ||
                             vm_.hover_sub_index != hit.main.sub_index;
        chrome_.hover = hit.chrome;
        vm_.hover_region = region;
        vm_.hover_control_index = hit.main.index;
        vm_.hover_sub_index = hit.main.sub_index;
        if (changed) Invalidate();
    }

    // ---- scrolling ---------------------------------------------------------

    void ScrollPaneTo(float value) {
        scroll_ = value;
        ClampScroll();
        Invalidate();
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

    void BeginScrollbarDrag(const HitTestResult& hit, float y) {
        scrollbar_sidebar_ = hit.sub_index == 2;
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

    void DragScrollbar(float y) {
        D2D1_RECT_F track{}, thumb{};
        float max_scroll = 0.0f;
        if (!ScrollbarGeometry(scrollbar_sidebar_, track, thumb, max_scroll)) return;
        const float travel = std::max(1.0f, (track.bottom - track.top) - (thumb.bottom - thumb.top));
        const float value = scrollbar_drag_scroll_ + (y - scrollbar_drag_y_) * max_scroll / travel;
        if (scrollbar_sidebar_) ScrollSidebarTo(value); else ScrollPaneTo(value);
    }

    void Wheel(WPARAM wparam, LPARAM lparam) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &point);
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const float delta = -steps * renderer_.RowHeight() * 3.0f;
        const D2D1_RECT_F sidebar = renderer_.SidebarRect(RendererWidth(), Height());
        if (ContainsRect(sidebar, static_cast<float>(point.x), static_cast<float>(point.y)))
            ScrollSidebarTo(vm_.sidebar_scroll + delta);
        else
            ScrollPaneTo(scroll_ + delta);
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
        const HWND previous = GetFocus();
        const int command = menu_.TrackPopup(point, std::move(items), std::move(filter));
        RestoreMenuFocus(previous);
        return command;
    }

    int TrackPickerDropdown(RECT anchor, std::vector<FluentMenuItem> items) {
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
        const wchar_t* zh[] = {L"名称", L"修改日期", L"大小", L"类型"};
        const wchar_t* en[] = {L"Name", L"Date modified", L"Size", L"Type"};
        for (int i = 0; i < 4; ++i)
            sort.children.push_back(Radio(10 + i, l10n::Pick(zh[i], en[i]),
                                          static_cast<int>(options_.sort) == i));
        sort.children.back().separator_after = true;
        sort.children.push_back(Radio(14, l10n::Get(l10n::StringId::Ascending).c_str(), !options_.descending));
        sort.children.push_back(Radio(15, l10n::Get(l10n::StringId::Descending).c_str(), options_.descending));
        return sort;
    }

    FluentMenuItem HiddenItem(int command) const {
        FluentMenuItem hidden = Item(command, l10n::Get(l10n::StringId::ShowHiddenItems).c_str(), L"\xE7B3");
        hidden.toggle = true;
        hidden.checked = options_.show_hidden;
        return hidden;
    }

    bool HandleSortCommand(int command) {
        if (command >= 10 && command <= 13) options_.sort = static_cast<PickerSort>(command - 10);
        else if (command == 14 || command == 15) options_.descending = command == 15;
        else return false;
        Reload();
        return true;
    }

    void ToggleHidden() {
        options_.show_hidden = !options_.show_hidden;
        Reload();
    }

    void SetViewMode(ViewMode mode) {
        if (view_mode_ == mode) return;
        view_mode_ = mode;
        g_last_view[mode_index_] = mode;
        ClampScroll();
        if (selected_ >= 0) EnsureVisible(selected_);
        Invalidate();
    }

    // The pane's "…" button: everything the old toolbar row offered.
    void ShowViewMenu(const D2D1_RECT_F& anchor) {
        std::vector<FluentMenuItem> items;
        FluentMenuItem create = Item(1, l10n::Get(l10n::StringId::NewFolder).c_str(), L"\xE8F4",
                                     !current_.empty() && !waiting_);
        create.shortcut = L"Ctrl+Shift+N";
        create.separator_after = true;
        items.push_back(std::move(create));
        items.push_back(SortMenu());
        FluentMenuItem hidden = HiddenItem(2);
        hidden.separator_after = true;
        items.push_back(std::move(hidden));
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
        else HandleSortCommand(command);
    }

    void ShowFilterMenu() {
        if (options_.filters.empty()) return;
        std::vector<FluentMenuItem> items;
        for (size_t i = 0; i < options_.filters.size(); ++i)
            items.push_back(Radio(static_cast<int>(i) + 1, options_.filters[i].label,
                                  i == options_.filter_index));
        menu_.SetTheme(dark_, accent_);
        SetFilterOpen(true);
        const int command = TrackPickerDropdown(ScreenRect(chrome_layout_.filter), std::move(items));
        SetFilterOpen(false);
        if (done_ || command <= 0) return;
        if (options_.filter_index == static_cast<size_t>(command - 1)) return;
        options_.filter_index = static_cast<size_t>(command - 1);
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

    void ShowRowMenu(int index, POINT client) {
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        if (selected_set_.find(index) == selected_set_.end()) Select(index);
        const PickerEntry entry = entries_[static_cast<size_t>(index)];
        const bool container = entry.kind == PickerEntryKind::Folder || entry.kind == PickerEntryKind::Drive;
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
        case 3: CopyTextToClipboard(hwnd_, path::FriendlyPathText(entry.path)); break;
        case 4: DeleteSelection(); break;
        case 5: SHObjectProperties(hwnd_, SHOP_FILEPATH, entry.path.c_str(), nullptr); break;
        case 6: CopySelection(true); break;
        case 7: CopySelection(false); break;
        case 8: Paste(); break;
        case 9: RenameSelection(); break;
        }
    }

    void ShowBlankMenu(POINT client) {
        std::vector<FluentMenuItem> items;
        items.push_back(Item(4, l10n::Get(l10n::StringId::Paste).c_str(), L"\xE77F", CanPaste()));
        items.back().shortcut = L"Ctrl+V";
        items.back().separator_after = true;
        items.push_back(Item(1, l10n::Get(l10n::StringId::NewFolder).c_str(), L"\xE8F4",
                             !current_.empty() && !waiting_));
        items.push_back(Item(2, l10n::Get(l10n::StringId::Refresh).c_str(), L"\xE72C"));
        items.back().separator_after = true;
        items.push_back(SortMenu());
        items.push_back(HiddenItem(3));
        const int command = TrackAt(client, std::move(items));
        if (done_ || command <= 0) return;
        if (command == 1) NewFolder();
        else if (command == 2) Reload();
        else if (command == 3) ToggleHidden();
        else if (command == 4) Paste();
        else HandleSortCommand(command);
    }

    std::vector<std::wstring> OperationPaths() const {
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
        return !current_.empty() && current_verified_ && !waiting_ && !validating_ && !operating_ &&
               IsClipboardFormatAvailable(CF_HDROP);
    }

    void ExecuteOperation(ops::OpRequest request, const ops::ClipboardData* clipboard = nullptr) {
        if (operating_ || done_) return;
        operating_ = true;
        const auto result = RunPickerOperation(hwnd_, std::move(request), dark_, accent_);
        operating_ = false;
        if (done_) return;
        std::wstring destination;
        std::vector<std::wstring> moved;
        for (const auto& completion : result.completed) {
            if (completion.refresh_only) continue;
            if (completion.type == ops::OpType::Move)
                moved.insert(moved.end(), completion.sources.begin(), completion.sources.end());
            if (!completion.destinations.empty()) destination = completion.destinations.front();
        }
        if (clipboard && clipboard->cut && std::all_of(clipboard->paths.begin(), clipboard->paths.end(),
            [&](const auto& source) {
                return std::any_of(moved.begin(), moved.end(), [&](const auto& p) { return SamePickerPath(source, p); });
            }))
            ops::CompleteCutClipboard(clipboard->sequence, clipboard->paths);
        if (!destination.empty()) ClearSelection();
        Reload();
        if (!destination.empty()) select_after_ = destination;
        reload_notice_ = result.error;
        chrome_.notice = result.error;
        FocusList();
    }

    void Paste() {
        if (!CanPaste()) return;
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

    void RenameSelection() {
        if (waiting_ || validating_ || operating_ || selected_set_.size() != 1) return;
        const auto paths = OperationPaths();
        if (paths.size() != 1) return;
        const std::wstring name = PickerLeafName(paths.front());
        menu_.SetTheme(dark_, accent_);
        menu_.SetInitialFilterText(name);
        menu_.SetSelectAllOnOpen(true);
        menu_.SetFilterPlaceholder(l10n::Get(l10n::StringId::NewName).c_str());
        auto rows = [](const std::wstring& value) {
            return std::vector<FluentMenuItem>{Item(1, l10n::Get(l10n::StringId::Rename).c_str(), L"\xE8AC", !value.empty())};
        };
        SyncVm();
        const auto anchor = ScreenRect(renderer_.ItemRectInPane(vm_.pane, PaneBounds(), *selected_set_.begin()));
        const int command = TrackPickerPopup({anchor.left, anchor.bottom}, rows(name), rows);
        if (done_ || (command != 1 && !menu_.LastFilterCommitted())) return;
        const auto new_name = menu_.LastFilterQuery();
        if (new_name.empty() || new_name == name) return;
        ops::OpRequest request;
        request.type = ops::OpType::Rename;
        request.sources = paths;
        request.new_name = new_name;
        ExecuteOperation(std::move(request));
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

    // Deletes to the Recycle Bin with the shell's own confirmation and progress.
    void DeleteSelection() {
        std::wstring list;
        for (int i : selected_set_) {
            if (i < 0 || i >= static_cast<int>(entries_.size())) continue;
            const PickerEntry& entry = entries_[static_cast<size_t>(i)];
            if (entry.kind == PickerEntryKind::Drive) continue;
            list += entry.path;
            list.push_back(L'\0');
        }
        if (list.empty()) return;
        list.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = FO_DELETE;
        op.pFrom = list.c_str();
        op.fFlags = FOF_ALLOWUNDO;
        SHFileOperationW(&op);
        if (!done_) { ClearSelection(); Reload(); }
    }

    void NewFolder() {
        if (current_.empty() || waiting_ || validating_ || !loader_) return;
        menu_.SetTheme(dark_, accent_);
        menu_.SetInitialFilterText(l10n::Get(l10n::StringId::NewFolder).c_str());
        menu_.SetSelectAllOnOpen(true);
        menu_.SetFilterPlaceholder(l10n::Get(l10n::StringId::FolderName).c_str());
        auto rows = [](const std::wstring& name) {
            FluentMenuItem item;
            item.command = 1;
            item.text = l10n::Get(l10n::StringId::CreateFolder).c_str();
            item.enabled = !name.empty();
            return std::vector<FluentMenuItem>{std::move(item)};
        };
        // Anchored under the pane's view button, where the command lives.
        const D2D1_RECT_F anchor = renderer_.PaneViewButtonRect(PaneBounds());
        const RECT screen = ScreenRect(anchor);
        const int picked = TrackPickerPopup({screen.left, screen.bottom}, rows(L"New folder"), rows);
        if (done_ || (picked != 1 && !menu_.LastFilterCommitted())) return;
        const auto name = menu_.LastFilterQuery();
        if (name.empty()) return;
        validation_navigation_only_ = false;
        validating_ = true;
        chrome_.notice = l10n::Get(l10n::StringId::CreatingFolder).c_str();
        UpdateChosen();
        loader_->CreateFolder(++generation_, current_, name);
        Invalidate();
    }

    // ---- focus and keys ----------------------------------------------------

    std::vector<int> FocusOrder() const {
        if (chrome_.overwrite_pending) return {kPickOverwriteBack, kPickRename, kPickCancel, kPickPrimary};
        std::vector<int> order = {kPickAddress, kPickSearch, kPickList};
        if (filename_edit_) {
            if (IsWindowEnabled(filename_edit_)) order.push_back(kPickFilename);
            order.push_back(kPickFilter);
        }
        for (size_t i = 0; i < spec_.custom_controls.size(); ++i)
            if (spec_.custom_controls[i].kind != 1 && spec_.custom_controls[i].kind != 5 && EffectiveControl(i) == 3)
                order.push_back(kCustomFocusBase + static_cast<int>(i));
        order.push_back(kPickCancel);
        if (!chosen_.empty() && !waiting_ && !validating_) { order.push_back(kPickPrimary); if(chrome_.primary_drop_down) order.push_back(kPickPrimaryDropDown); }
        return order;
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
        focus_ = next;
        if (next >= kCustomFocusBase) {
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
        if (id >= kPickRemoveSelectionBase && id < kCustomFocusBase) {
            const auto index = static_cast<size_t>(id - kPickRemoveSelectionBase);
            if (index < selection_tag_indices_.size()) {
                selected_set_.erase(selection_tag_indices_[index]); editing_selection_names_ = false;
                SyncSelectedNames(); Relayout(); Invalidate();
            }
            return;
        }
        if (chrome_.overwrite_pending) {
            if (id == kPickPrimary) { ResolveOverwrite(true); return; }
            if (id == kPickOverwriteBack || id == kPickRename) { ResolveOverwrite(false, id == kPickRename); return; }
        }
        PickerProofFocus("activate", focus_, id);
        if (id == kPickPrimaryDropDown) {
            for(size_t i=0;i<spec_.custom_controls.size();++i) if(spec_.custom_controls[i].kind==5) {
                Activate(kCustomFocusBase+static_cast<int>(i)); return;
            }
        }
        if (id >= kCustomFocusBase) {
            const size_t index = static_cast<size_t>(id - kCustomFocusBase);
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
                    auto row=Radio(static_cast<int>(i)+1,item.label,control.kind==3 && control.has_selected && item.id==control.selected_item_id);
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
        else if (id == kPickPrimary) SubmitSelection();
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
        if (validating_) { ++generation_; validating_ = false; }
        selected_set_.clear();
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const auto kind = entries_[static_cast<size_t>(i)].kind;
            if (kind == PickerEntryKind::Image || kind == PickerEntryKind::File) selected_set_.insert(i);
        }
        SyncSelectedNames();
        Invalidate();
    }

    bool HandleKey(WPARAM key) {
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
        const bool ctrl = KeyDown(VK_CONTROL);
        const bool shift = KeyDown(VK_SHIFT);
        const int count = static_cast<int>(entries_.size());
        auto list_move = [&](int index) {
            focus_ = kPickList;
            chrome_.show_focus = true;
            if (count > 0) Select(index, shift, ctrl && shift, ctrl && !shift);
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
            else if (searching_) ExitSearch();
            else Cancel();
            return true;
        case VK_TAB: MoveFocus(shift ? -1 : 1); return true;
        case VK_RETURN:
            if (focus_ != kPickList) Activate(focus_);
            else EnterOnList();
            return true;
        case VK_SPACE:
            if (focus_ != kPickList) Activate(focus_);
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
        case VK_DELETE: if (focus_ == kPickList) DeleteSelection(); return true;
        case VK_APPS: {
            if (selected_ >= 0) {
                SyncVm();
                const D2D1_RECT_F item = renderer_.ItemRectInPane(vm_.pane, PaneBounds(), selected_);
                ShowRowMenu(selected_, {static_cast<LONG>(item.left + 24.0f * scale_), static_cast<LONG>(item.bottom)});
            }
            return true;
        }
        case 'L': if (ctrl) { BeginAddressEdit(); return true; } break;
        case 'F': if (ctrl) { BeginSearch(); return true; } break;
        case 'K': if (ctrl) { BeginSearch(); return true; } break;
        case 'E': if (ctrl) { BeginSearch(); return true; } break;
        case 'A': if (ctrl) { SelectAll(); return true; } break;
        case 'C': if (ctrl && focus_ == kPickList) { CopySelection(false); return true; } break;
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

    void PressMain(const HitTestResult& hit, float x, float y, bool double_click) {
        using R = HitTestResult;
        switch (hit.region) {
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
        case R::Row:
            if (hit.index < 0) break;
            focus_ = kPickList;
            if (address_editing_) EndAddressEdit(false);
            SetFocus(hwnd_);
            Select(hit.index, KeyDown(VK_SHIFT), KeyDown(VK_CONTROL));
            if (double_click) OpenEntry(hit.index);
            return;
        case R::ColumnHeader: {
            const PickerSort sort = SortFromColumn(hit.column);
            if (options_.sort == sort) options_.descending = !options_.descending;
            else { options_.sort = sort; options_.descending = false; }
            Reload();
            return;
        }
        case R::Scrollbar:
            if (hit.sub_index == 1) return;  // the picker never scrolls sideways
            BeginScrollbarDrag(hit, y);
            return;
        case R::PaneDetails: SetViewMode(ViewMode::Details); return;
        case R::PaneMediumIcons:
            SetViewMode(ViewMode::MediumIcons);
            return;
        case R::PaneViewButton: ShowViewMenu(hit.control_bounds.right > hit.control_bounds.left
                                                 ? hit.control_bounds
                                                 : renderer_.PaneViewButtonRect(PaneBounds()));
            return;
        case R::PaneEmptyNewFolder: NewFolder(); return;
        case R::SidebarItem:
            if (!hit.path.empty()) Navigate(hit.path, true);
            return;
        case R::SidebarHeader:
            if (hit.index >= 0 && hit.index < static_cast<int>(vm_.sidebar.size())) {
                const SidebarGroup& group = vm_.sidebar[static_cast<size_t>(hit.index)];
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
        case WM_CREATE: {
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            compositor_.SetCompositionAboveChildren(spec_.pulse_painted_plain_child);
            PickerProofPhase("compositor-init-before");
            const bool compositor_initialized = compositor_.Init(hwnd_);
            PickerProofPhase("compositor-init-after");
            if (!compositor_initialized) return -1;
            if (spec_.pulse_painted_plain_child && !compositor_.UsesTransparentComposition()) {
                native_error_ = ERROR_NOT_SUPPORTED; return -1;
            }
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            renderer_.SetCompositor(&compositor_);
            renderer_.SetScale(scale_);
            renderer_.SetIconNotifyWindow(hwnd_);
            ApplyRendererSettings();
            CreateFonts();
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            if (spec_.pulse_painted_plain_child && !compositor_.CustomEditEnabled()) {
                native_error_ = ERROR_NOT_SUPPORTED; return -1;
            }
            CreateEdits();
            if (spec_.pulse_painted_plain_child && (!path_edit_ || !search_edit_ ||
                !filename_edit_)) {
                if (!native_error_) native_error_ = ERROR_GEN_FAILURE;
                return -1;
            }
            menu_.Create(hwnd_, &compositor_, scale_);
            menu_.SetTheme(dark_, accent_);
            Relayout();
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
            info->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale_);
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
            Invalidate();
            break;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBrush());
        }
        case WM_COMMAND:
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
        case kEndAddressMessage:
            if (address_editing_ && GetFocus() != path_edit_) EndAddressEdit(false);
            return 0;
        case WM_TIMER:
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
        case kListingMessage:
            OnListing(PickerLoader::Take(lparam));
            return 0;
        case kValidationMessage:
            OnValidation(PickerLoader::TakeValidation(lparam));
            return 0;
        case WM_MOUSEMOVE: {
            if (custom_bar_drag_) { DragCustomScrollbar(static_cast<float>(GET_Y_LPARAM(lparam))); return 0; }
            if (dragging_scrollbar_) {
                DragScrollbar(static_cast<float>(GET_Y_LPARAM(lparam)));
                return 0;
            }
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
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(hwnd_, &point);
            if (!spec_.custom_controls.empty() && ContainsRect(CustomPanel(), static_cast<float>(point.x), static_cast<float>(point.y))) {
                ScrollCustom(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 38 * scale_); return 0;
            }
            Wheel(wparam, lparam);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            const auto custom_bar = CustomScrollbar();
            if (fluent::ScrollbarHitTest(custom_bar, x, y)) {
                const auto thumb = fluent::ScrollbarThumbRect(custom_bar, scale_);
                custom_bar_grab_ = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
                custom_bar_drag_ = true; SetCapture(hwnd_); DragCustomScrollbar(y); return 0;
            }
            const Hit hit = HitAt(x, y);
            chrome_.show_focus = false;
            if (hit.chrome == kPickFilename) { FocusFilename(); return 0; }
            if (hit.chrome != kPickNone) {
                if (hit.chrome >= kCustomFocusBase) { focus_ = hit.chrome; SetFocus(hwnd_); }
                chrome_.pressed = hit.chrome;
                SetCapture(hwnd_);
                Invalidate();
                return 0;
            }
            PressMain(hit.main, x, y, message == WM_LBUTTONDBLCLK);
            if (!done_) Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (custom_bar_drag_) { custom_bar_drag_ = false; ReleaseCapture(); return 0; }
            if (dragging_scrollbar_) {
                dragging_scrollbar_ = false;
                ReleaseCapture();
                return 0;
            }
            const int pressed = chrome_.pressed;
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
        case WM_RBUTTONUP: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            const Hit hit = HitAt(x, y);
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (hit.main.region == HitTestResult::BreadcrumbSegment || hit.main.region == HitTestResult::AddressBar) {
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
        case WM_CAPTURECHANGED:
            dragging_scrollbar_ = false;
            return 0;
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) GoBack();
            else if (GET_XBUTTON_WPARAM(wparam) == XBUTTON2) GoForward();
            return TRUE;
        case WM_KEYDOWN:
            if (HandleKey(wparam)) return 0;
            break;
        case WM_SYSKEYDOWN:
            if (wparam == VK_UP) { GoUp(); return 0; }
            if (wparam == VK_LEFT) { GoBack(); return 0; }
            if (wparam == VK_RIGHT) { GoForward(); return 0; }
            if (wparam == 'D') { BeginAddressEdit(); return 0; }
            if (wparam == 'N' && filename_edit_) { FocusFilename(); return 0; }
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
            menu_.Dismiss();
            KillTimer(hwnd_, kMotionTimer);
            renderer_.SetIconNotifyWindow(nullptr);
            for (HWND edit : AllEdits()) {
                if (edit) { RemoveWindowSubclass(edit, EditProc, 1); DestroyWindow(edit); }
            }
            path_edit_ = filename_edit_ = search_edit_ = nullptr;
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    bool rendering_frame_ = false;
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
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    MainRenderer renderer_;
    FluentMenu menu_;
    WindowViewModel vm_;
    FolderPickerChrome chrome_;
    FolderPickerChromeLayout chrome_layout_;
    FolderPickerSpec spec_;
    PickerMode mode_ = PickerMode::Folder;
    PickerHistory history_;
    PickerOptions options_;
    std::unique_ptr<PickerLoader> loader_;
    std::vector<PickerPlace> places_;
    std::vector<SidebarItem> drives_;
    bool collapsed_[6] = {};
    picker_places::Snapshot pulse_places_;
    std::wstring current_;
    std::vector<PickerEntry> entries_;
    fs::SnapshotPtr snapshot_;
    std::unordered_set<int> selected_set_;
    int selected_ = -1;
    int anchor_ = -1;
    float scroll_ = 0.0f;
    ViewMode view_mode_ = ViewMode::Details;
    std::wstring error_;
    std::wstring chosen_;
    std::wstring filename_text_;
    std::wstring search_text_;
    std::wstring fallback_;
    std::wstring select_after_;
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
    bool save_focus_pending_ = false;
    HWND save_focus_before_validation_ = nullptr;
    bool address_editing_ = false;
    bool searching_ = false;
    bool allow_multiselect_ = false;
    bool save_filter_changed_ = false;
    bool open_filter_changed_ = false;
    bool filename_from_selection_ = false;
    bool editing_selection_names_ = false;
    std::vector<int> selection_tag_indices_;
    bool validation_navigation_only_ = false;
    bool setting_edits_ = false;
    bool dragging_scrollbar_ = false;
    bool scrollbar_sidebar_ = false;
    float scrollbar_drag_y_ = 0.0f;
    float scrollbar_drag_scroll_ = 0.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    struct PendingNavigation { std::wstring path; bool history; std::wstring selection; bool verified; };
    std::deque<PendingNavigation> pending_navigation_;
    PickerDialogSnapshot embedded_state_;
    unsigned embedded_callback_depth_ = 0;
    bool embedded_ready_ = false, embedded_folder_pending_ = false;
    bool embedded_folder_committed_ = false;
    std::deque<std::wstring> external_navigation_queue_;
    bool external_navigation_ = false;
    std::unique_ptr<PickerListing> external_listing_;
    uint64_t generation_ = 0;
    ULONGLONG typeahead_time_ = 0;
    std::wstring typeahead_;
    size_t mode_index_ = 0;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    DWORD native_error_ = ERROR_SUCCESS;
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
