#include "shell_adapter_host.h"
#include "../../app_commands.h"
#include "../../app_runtime.h"
#include "../../app_state.h"
#include "../../places.h"
#include "../../window_registry.h"
#include "../../../common/json_value.h"
#include "../../../fs/fs_snapshot.h"
#include <objbase.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::extensions::native {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ULL;
constexpr size_t kMaxExcerptBytes = 1024;

int64_t FileTimeToUnixMs(const FILETIME& ft) noexcept {
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                           static_cast<uint64_t>(ft.dwLowDateTime);
    if (ticks <= kWindowsToUnixEpoch100ns) return 0;
    return static_cast<int64_t>((ticks - kWindowsToUnixEpoch100ns) / 10000ULL);
}

ColorTagInfoData ConvertColorTag(const app::ColorTag& tag) {
    ColorTagInfoData out;
    out.id = json::Utf8(tag.id);
    out.name = json::Utf8(tag.name);
    out.argb = 0xFF000000u | (tag.rgb & 0x00FFFFFFu);
    out.favorite_slot = static_cast<int32_t>(tag.favorite);
    return out;
}

void InvalidateCoreWindows(AppState& app) {
    app::ForEachWindowSharingCore(app, [](AppState& window) {
        if (window.hwnd && IsWindow(window.hwnd)) {
            InvalidateRect(window.hwnd, nullptr, FALSE);
        }
    });
}

struct HostMenuSession {
    HWND owner = nullptr;
    uint64_t session_id = 0;
    uint32_t ops_token = 0;
    std::function<void(ShellMenuBackendReply)> deliver;
};

std::mutex g_menu_mutex;
auto& MenuByOwnerSession() {
    static auto* map = new std::map<std::pair<HWND, uint64_t>, std::shared_ptr<HostMenuSession>>();
    return *map;
}
auto& MenuByOpsToken() {
    static auto* map = new std::map<uint32_t, std::shared_ptr<HostMenuSession>>();
    return *map;
}

std::mutex g_preview_mutex;
auto& ActivePreviewCancels() {
    static auto* map = new std::map<std::pair<HWND, uint64_t>, std::shared_ptr<std::atomic<bool>>>();
    return *map;
}

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

bool IsLikelyImageExtension(const std::wstring& ext) {
    static constexpr std::wstring_view kImageExts[] = {
        L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".webp", L".ico",
        L".tif", L".tiff", L".wdp", L".jxr", L".heic", L".heif", L".avif",
    };
    for (const auto candidate : kImageExts) {
        if (ext == candidate) return true;
    }
    return false;
}

std::string TruncateUtf8Cleanly(std::string text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    text.resize(cut);
    return text;
}

std::string ReadTextExcerptFromFile(const std::wstring& path, const std::atomic<bool>& cancelled) {
    if (cancelled.load(std::memory_order_acquire)) return {};
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    uint8_t buf[2048]{};
    DWORD bytes_read = 0;
    const BOOL ok = ReadFile(file, buf, sizeof(buf), &bytes_read, nullptr);
    CloseHandle(file);
    if (!ok || bytes_read == 0) return {};

    // UTF-16 LE BOM
    if (bytes_read >= 2 && buf[0] == 0xFFu && buf[1] == 0xFEu) {
        const size_t wchar_count = (bytes_read - 2) / sizeof(wchar_t);
        std::wstring wide;
        wide.resize(wchar_count);
        if (wchar_count > 0) {
            std::memcpy(wide.data(), buf + 2, wchar_count * sizeof(wchar_t));
        }
        std::erase(wide, L'\0');
        return TruncateUtf8Cleanly(json::Utf8(wide), kMaxExcerptBytes);
    }
    // UTF-16 BE BOM
    if (bytes_read >= 2 && buf[0] == 0xFEu && buf[1] == 0xFFu) {
        const size_t wchar_count = (bytes_read - 2) / sizeof(wchar_t);
        std::wstring wide;
        wide.reserve(wchar_count);
        for (size_t i = 0; i < wchar_count; ++i) {
            const uint16_t hi = buf[2 + i * 2];
            const uint16_t lo = buf[2 + i * 2 + 1];
            const wchar_t wc = static_cast<wchar_t>((hi << 8) | lo);
            if (wc != L'\0') wide.push_back(wc);
        }
        return TruncateUtf8Cleanly(json::Utf8(wide), kMaxExcerptBytes);
    }

    size_t offset = 0;
    if (bytes_read >= 3 && buf[0] == 0xEFu && buf[1] == 0xBBu && buf[2] == 0xBFu) {
        offset = 3;
    }

    const uint8_t* start = buf + offset;
    size_t len = bytes_read - offset;
    if (len == 0) return {};

    // Reject binary payloads containing NUL or excessive control bytes.
    size_t control_bytes = 0;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = start[i];
        if (b == 0) return {};
        if (b < 0x20u && b != '\r' && b != '\n' && b != '\t' && b != '\f') {
            ++control_bytes;
        }
    }
    if (control_bytes * 20 > len) return {};

    // Trim trailing incomplete UTF-8 multi-byte sequence if buffer hit EOF mid-sequence.
    size_t valid_len = (std::min)(len, kMaxExcerptBytes);
    while (valid_len > 0) {
        const int wlen = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(start),
            static_cast<int>(valid_len),
            nullptr, 0);
        if (wlen > 0) {
            return std::string(reinterpret_cast<const char*>(start), valid_len);
        }
        if (len - valid_len >= 4) break;
        --valid_len;
    }

    // Fallback to system ANSI code page if plain 8-bit text.
    const int ansi_wlen = MultiByteToWideChar(
        CP_ACP, MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(start),
        static_cast<int>((std::min)(len, kMaxExcerptBytes)),
        nullptr, 0);
    if (ansi_wlen > 0) {
        std::wstring wide(static_cast<size_t>(ansi_wlen), L'\0');
        MultiByteToWideChar(
            CP_ACP, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(start),
            static_cast<int>((std::min)(len, kMaxExcerptBytes)),
            wide.data(), ansi_wlen);
        return TruncateUtf8Cleanly(json::Utf8(wide), kMaxExcerptBytes);
    }
    return {};
}

void DecodeWicThumbnail(const std::wstring& path,
                        uint32_t max_thumb_px,
                        const std::atomic<bool>& cancelled,
                        ShellPreviewData& out) {
    if (cancelled.load(std::memory_order_acquire)) return;

    struct ComScope {
        HRESULT hr = E_FAIL;
        ComScope() noexcept { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
        ~ComScope() noexcept {
            if (hr == S_OK || hr == S_FALSE) CoUninitialize();
        }
    } com;

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    struct HandleGuard {
        HANDLE h = INVALID_HANDLE_VALUE;
        ~HandleGuard() noexcept {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    } file_guard{file};

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) || !factory) {
        return;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFileHandle(
            reinterpret_cast<ULONG_PTR>(file),
            nullptr,
            WICDecodeMetadataCacheOnLoad,
            &decoder)) || !decoder) {
        return;
    }

    if (cancelled.load(std::memory_order_acquire)) return;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return;

    UINT orig_w = 0;
    UINT orig_h = 0;
    if (FAILED(frame->GetSize(&orig_w, &orig_h)) || orig_w == 0 || orig_h == 0) return;
    out.image_width = orig_w;
    out.image_height = orig_h;

    if (cancelled.load(std::memory_order_acquire)) return;

    UINT thumb_w = orig_w;
    UINT thumb_h = orig_h;
    if (orig_w > max_thumb_px || orig_h > max_thumb_px) {
        if (orig_w >= orig_h) {
            thumb_w = max_thumb_px;
            thumb_h = (std::max)(1u, static_cast<UINT>((static_cast<uint64_t>(orig_h) * max_thumb_px + orig_w / 2) / orig_w));
        } else {
            thumb_h = max_thumb_px;
            thumb_w = (std::max)(1u, static_cast<UINT>((static_cast<uint64_t>(orig_w) * max_thumb_px + orig_h / 2) / orig_h));
        }
    }

    ComPtr<IWICBitmapSource> source = frame;
    if (thumb_w != orig_w || thumb_h != orig_h) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || !scaler) return;
        if (FAILED(scaler->Initialize(frame.Get(), thumb_w, thumb_h, WICBitmapInterpolationModeFant))) return;
        source = scaler;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) return;
    if (FAILED(converter->Initialize(
            source.Get(),
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom))) {
        return;
    }

    if (cancelled.load(std::memory_order_acquire)) return;

    const UINT stride = thumb_w * 4u;
    const size_t total_bytes = static_cast<size_t>(stride) * static_cast<size_t>(thumb_h);
    std::vector<uint8_t> pixels(total_bytes);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(total_bytes), pixels.data()))) {
        return;
    }

    out.thumb_width = thumb_w;
    out.thumb_height = thumb_h;
    out.thumb_stride = stride;
    out.thumb_bgra = std::move(pixels);
}

std::string DescribeDisplayType(const std::wstring& path, DWORD attrs) {
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), attrs, &sfi, sizeof(sfi),
                       SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) != 0 &&
        sfi.szTypeName[0] != L'\0') {
        return json::Utf8(sfi.szTypeName);
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) return "Folder";
    const std::wstring ext = std::filesystem::path(path).extension().wstring();
    if (!ext.empty()) return json::Utf8(ext) + " File";
    return "File";
}

void RunPreviewWorker(HWND owner,
                      uint64_t request_id,
                      std::wstring norm_path,
                      uint32_t max_thumb_px,
                      std::shared_ptr<std::atomic<bool>> cancelled,
                      std::function<void(ShellPreviewData)> complete) {
    auto finish = [&](ShellPreviewData result) {
        {
            std::lock_guard lock(g_preview_mutex);
            ActivePreviewCancels().erase({owner, request_id});
        }
        if (!cancelled->load(std::memory_order_acquire) && complete) {
            complete(std::move(result));
        }
    };

    if (cancelled->load(std::memory_order_acquire)) {
        std::lock_guard lock(g_preview_mutex);
        ActivePreviewCancels().erase({owner, request_id});
        return;
    }

    ShellPreviewData data;
    data.path = json::Utf8(norm_path);

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(norm_path.c_str(), GetFileExInfoStandard, &fad)) {
        data.status = PULSE_ERROR_ARGUMENT;
        finish(std::move(data));
        return;
    }

    const bool is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    data.status = PULSE_OK;
    data.size_bytes = is_dir
        ? 0ULL
        : ((static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | static_cast<uint64_t>(fad.nFileSizeLow));
    data.modified_unix_ms = FileTimeToUnixMs(fad.ftLastWriteTime);
    data.display_type = DescribeDisplayType(norm_path, fad.dwFileAttributes);

    if (!is_dir && !cancelled->load(std::memory_order_acquire)) {
        const std::wstring ext = LowerExtension(norm_path);
        if (IsLikelyImageExtension(ext)) {
            DecodeWicThumbnail(norm_path, max_thumb_px, *cancelled, data);
        } else if (data.size_bytes > 0) {
            data.text_excerpt = ReadTextExcerptFromFile(norm_path, *cancelled);
        }
    }

    finish(std::move(data));
}

} // namespace

bool DeliverShellAdapterMenuItems(uint32_t token, const std::vector<ops::ShellMenuItem>& items, bool partial) {
    if (token == 0) return false;
    std::shared_ptr<HostMenuSession> session;
    {
        std::lock_guard lock(g_menu_mutex);
        auto& by_tok = MenuByOpsToken();
        const auto it = by_tok.find(token);
        if (it == by_tok.end()) return false;
        session = it->second;
    }
    if (!session || !session->deliver) return true;

    ShellMenuBackendReply reply;
    reply.status = PULSE_OK;
    reply.partial = partial;
    reply.items.reserve(items.size());
    for (const auto& src : items) {
        ShellMenuItemData dst;
        dst.item_id = src.id;
        dst.flags = 0;
        if (src.enabled) dst.flags |= PULSE_SHELL_MENU_ITEM_ENABLED;
        if (src.separator_after) dst.flags |= PULSE_SHELL_MENU_ITEM_SEPARATOR_AFTER;
        if (src.has_children) dst.flags |= PULSE_SHELL_MENU_ITEM_SUBMENU;
        if (src.child || src.depth > 0) dst.flags |= PULSE_SHELL_MENU_ITEM_CHILD;
        dst.depth = src.depth > 0 ? static_cast<uint32_t>(src.depth) : (src.child ? 1u : 0u);
        dst.mnemonic_codepoint = static_cast<uint32_t>(src.mnemonic);
        dst.verb = json::Utf8(src.verb);
        dst.label = json::Utf8(src.text);
        dst.handler_name = json::Utf8(src.handler);
        reply.items.push_back(std::move(dst));
    }
    session->deliver(std::move(reply));
    return true;
}

ShellBackend MakeShellBackend(HWND owner) {
    ShellBackend backend;
    backend.valid_owner = [owner] { return IsWindow(owner) != FALSE; };
    backend.get_tag_catalog = [owner](std::vector<ColorTagInfoData>& out) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        out.clear();
        out.reserve(app->core.places.tags.size());
        for (const auto& tag : app->core.places.tags) {
            out.push_back(ConvertColorTag(tag));
        }
        return PULSE_OK;
    };
    backend.get_path_tags = [owner](const std::wstring& raw_path, std::vector<ColorTagInfoData>& out) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        if (raw_path.empty()) return PULSE_ERROR_ARGUMENT;
        if (app->core.places.tags.empty()) {
            out.clear();
            return PULSE_OK;
        }
        const std::wstring norm = fs::NormalizePath(raw_path);
        if (norm.empty()) return PULSE_ERROR_ARGUMENT;
        out.clear();
        const std::vector<int> indices = app->core.places.TagsForPath(norm);
        out.reserve(indices.size());
        for (const int idx : indices) {
            if (idx >= 0 && static_cast<size_t>(idx) < app->core.places.tags.size()) {
                out.push_back(ConvertColorTag(app->core.places.tags[static_cast<size_t>(idx)]));
            }
        }
        return PULSE_OK;
    };
    backend.set_path_tag = [owner](const std::wstring& raw_path, const std::wstring& tag_id, bool tagged) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        const std::wstring norm = fs::NormalizePath(raw_path);
        if (norm.empty()) return PULSE_ERROR_ARGUMENT;
        if (!tagged && (tag_id == L"*" || tag_id == L"clear")) {
            ClearTagsForSelection(*app, {norm});
            InvalidateCoreWindows(*app);
            return PULSE_OK;
        }
        const std::wstring resolved = app->core.places.ResolveTagRef(tag_id);
        const int idx = app->core.places.FindTagIndex(resolved.empty() ? tag_id : resolved);
        if (idx < 0) return PULSE_ERROR_ARGUMENT;
        const app::TagId canonical_id = app->core.places.tags[static_cast<size_t>(idx)].id;
        std::vector<app::TagAdsUpdate> updates;
        if (app->core.places.SetTagsBatch(canonical_id, {norm}, tagged, &updates)) {
            QueueTagAds(*app, std::move(updates));
            if (tagged) {
                auto& recent = app->appPrefs.recent_tags;
                std::erase(recent, canonical_id);
                recent.insert(recent.begin(), canonical_id);
                if (recent.size() > 5) recent.resize(5);
                app->appPrefs.Save();
            }
        }
        InvalidateCoreWindows(*app);
        return PULSE_OK;
    };
    backend.query_context_menu = [owner](uint64_t session_id,
                                         const std::vector<std::wstring>& raw_paths,
                                         uint32_t flags,
                                         std::function<void(ShellMenuBackendReply)> deliver) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        std::vector<std::wstring> norm_paths;
        norm_paths.reserve(raw_paths.size());
        for (const auto& p : raw_paths) {
            std::wstring norm = fs::NormalizePath(p);
            if (norm.empty()) return PULSE_ERROR_ARGUMENT;
            norm_paths.push_back(std::move(norm));
        }

        auto session = std::make_shared<HostMenuSession>();
        session->owner = owner;
        session->session_id = session_id;
        session->deliver = deliver;

        uint32_t token = 0;
        if (!app->isolatedTest && !app->safeMode) {
            const bool background = (flags & PULSE_SHELL_MENU_QUERY_BACKGROUND) != 0;
            const bool extended = (flags & PULSE_SHELL_MENU_QUERY_EXTENDED) != 0;
            token = app->core.ops.QueryShellMenu(
                norm_paths,
                owner,
                background,
                extended,
                app->core.ctxMenuPrefs.DisabledHandlerClsids());
        }
        session->ops_token = token;
        {
            std::lock_guard lock(g_menu_mutex);
            MenuByOwnerSession()[{owner, session_id}] = session;
            if (token != 0) {
                MenuByOpsToken()[token] = session;
            }
        }
        if (token == 0) {
            ShellMenuBackendReply empty;
            empty.status = PULSE_OK;
            empty.partial = false;
            deliver(std::move(empty));
        }
        return PULSE_OK;
    };
    backend.invoke_context_menu = [owner](uint64_t session_id, uint32_t item_id, const std::wstring& verb) -> PulseStatus {
        auto* app = GetAppState(owner);
        if (!app || !IsWindow(owner)) return PULSE_ERROR_STATE;
        uint32_t token = 0;
        {
            std::lock_guard lock(g_menu_mutex);
            auto& by_sess = MenuByOwnerSession();
            const auto it = by_sess.find({owner, session_id});
            if (it == by_sess.end()) return PULSE_ERROR_STATE;
            if (it->second) {
                token = it->second->ops_token;
                if (token != 0) MenuByOpsToken().erase(token);
            }
            by_sess.erase(it);
        }
        if (token != 0) {
            app->core.ops.InvokeShellMenu(token, item_id, verb, {});
        }
        return PULSE_OK;
    };
    backend.close_context_menu = [owner](uint64_t session_id) -> PulseStatus {
        uint32_t token = 0;
        {
            std::lock_guard lock(g_menu_mutex);
            auto& by_sess = MenuByOwnerSession();
            const auto it = by_sess.find({owner, session_id});
            if (it != by_sess.end()) {
                if (it->second) {
                    token = it->second->ops_token;
                    if (token != 0) MenuByOpsToken().erase(token);
                }
                by_sess.erase(it);
            }
        }
        if (token != 0) {
            if (auto* app = GetAppState(owner)) {
                app->core.ops.CloseShellMenu(token);
            }
        }
        return PULSE_OK;
    };
    backend.read_preview = [owner](uint64_t request_id,
                                   const std::wstring& raw_path,
                                   uint32_t max_thumb_px,
                                   std::function<void(ShellPreviewData)> complete) -> PulseStatus {
        if (!IsWindow(owner)) return PULSE_ERROR_STATE;
        const std::wstring norm = fs::NormalizePath(raw_path);
        if (norm.empty()) return PULSE_ERROR_ARGUMENT;
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard lock(g_preview_mutex);
            ActivePreviewCancels()[{owner, request_id}] = cancelled;
        }
        std::thread([owner, request_id, norm = std::move(norm), max_thumb_px,
                     cancelled = std::move(cancelled), complete = std::move(complete)]() mutable {
            RunPreviewWorker(owner, request_id, std::move(norm), max_thumb_px,
                             std::move(cancelled), std::move(complete));
        }).detach();
        return PULSE_OK;
    };
    backend.cancel_preview = [owner](uint64_t request_id) {
        std::shared_ptr<std::atomic<bool>> cancelled;
        {
            std::lock_guard lock(g_preview_mutex);
            auto& active = ActivePreviewCancels();
            const auto it = active.find({owner, request_id});
            if (it != active.end()) {
                cancelled = it->second;
                active.erase(it);
            }
        }
        if (cancelled) {
            cancelled->store(true, std::memory_order_release);
        }
    };
    return backend;
}

ShellAdapter::ShellAdapter(HWND owner, std::function<bool()> alive,
                           std::shared_ptr<DiagnosticsSink> diagnostics,
                           std::function<void()> wake)
    : ShellAdapter(MakeShellBackend(owner), std::move(alive), std::move(diagnostics), std::move(wake)) {}

} // namespace pulse::extensions::native
