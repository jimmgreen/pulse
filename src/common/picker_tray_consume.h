#pragma once
#include "picker_places_snapshot.h"
#include <shlobj.h>
#include <unordered_set>

namespace pulse::picker_places {
inline constexpr ULONG_PTR kConsumeTrayMessage = 0x50535443;
inline constexpr ULONG_PTR kStageTrayMessage = 0x50535453;
inline constexpr size_t kMaxConsumeTrayItems = 1024;
inline constexpr size_t kMaxStageTrayItems = 256;

// Pure path resolution matching app::DefaultShelfSnippetsDir() without disk I/O.
inline std::wstring DefaultPickerShelfDirPath() {
    wchar_t fixture[MAX_PATH]{};
    const auto len = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", fixture, ARRAYSIZE(fixture));
    if (len > 0 && len < ARRAYSIZE(fixture)) {
        std::wstring dir(fixture, len);
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        return dir + L"\\shelf";
    }
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local)) && local) {
        dir = std::wstring(local) + L"\\Pulse\\shelf";
        CoTaskMemFree(local);
    }
    return dir;
}
struct TrayConsumeItem {
    uint64_t item_id = 0, batch_id = 0;
    std::wstring path;
};
struct TrayConsumeRequest {
    DWORD publisher_pid = 0;
    HWND source = nullptr;
    uint64_t shelf_id = 0;
    std::vector<TrayConsumeItem> items;
};
struct TrayConsumeResult {
    bool accepted = false;
    uint32_t removed = 0, skipped_owned = 0;
};
namespace consume_detail {
struct Header {
    DWORD version = 1, publisher_pid = 0, count = 0, reserved = 0;
    uint64_t source = 0, shelf_id = 0;
};
struct Row { uint64_t item_id = 0, batch_id = 0; wchar_t path[1024]{}; };
static_assert(sizeof(Header) == 32 && sizeof(Row) == 2064);
}
inline bool ValidConsumeTray(const TrayConsumeRequest& request) {
    if (!request.publisher_pid || !request.source || !request.shelf_id || request.items.empty() ||
        request.items.size() > kMaxConsumeTrayItems) return false;
    std::unordered_set<uint64_t> ids;
    for (const auto& item : request.items)
        if (!item.item_id || !item.batch_id || !detail::DiskPath(item.path) || !ids.insert(item.item_id).second) return false;
    return true;
}
inline std::vector<unsigned char> EncodeConsumeTray(const TrayConsumeRequest& request) {
    if (!ValidConsumeTray(request)) return {};
    consume_detail::Header header;
    header.publisher_pid = request.publisher_pid; header.count = static_cast<DWORD>(request.items.size());
    header.source = reinterpret_cast<ULONG_PTR>(request.source); header.shelf_id = request.shelf_id;
    std::vector<unsigned char> bytes(sizeof(header) + request.items.size() * sizeof(consume_detail::Row));
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (size_t i = 0; i < request.items.size(); ++i) {
        consume_detail::Row row;
        row.item_id = request.items[i].item_id; row.batch_id = request.items[i].batch_id;
        std::memcpy(row.path, request.items[i].path.c_str(), (request.items[i].path.size() + 1) * sizeof(wchar_t));
        std::memcpy(bytes.data() + sizeof(header) + i * sizeof(row), &row, sizeof(row));
    }
    return bytes;
}
inline bool DecodeConsumeTray(const COPYDATASTRUCT* data, TrayConsumeRequest& request) {
    if (!data || data->dwData != kConsumeTrayMessage || !data->lpData || data->cbData < sizeof(consume_detail::Header)) return false;
    consume_detail::Header header;
    std::memcpy(&header, data->lpData, sizeof(header));
    if (header.version != 1 || header.reserved || !header.count || header.count > kMaxConsumeTrayItems ||
        data->cbData != sizeof(header) + header.count * sizeof(consume_detail::Row) ||
        header.source > UINTPTR_MAX) return false;
    TrayConsumeRequest decoded;
    decoded.publisher_pid = header.publisher_pid; decoded.source = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(header.source));
    decoded.shelf_id = header.shelf_id; decoded.items.reserve(header.count);
    const auto* bytes = static_cast<const unsigned char*>(data->lpData);
    for (size_t i = 0; i < header.count; ++i) {
        consume_detail::Row row;
        std::memcpy(&row, bytes + sizeof(header) + i * sizeof(row), sizeof(row));
        if (!wmemchr(row.path, 0, 1024)) return false;
        decoded.items.push_back({row.item_id, row.batch_id, row.path});
    }
    if (!ValidConsumeTray(decoded)) return false;
    request = std::move(decoded); return true;
}
// All replies fit a positive 32-bit LRESULT, including across process bitness.
inline LRESULT EncodeConsumeTrayResult(const TrayConsumeResult& result) {
    if (!result.accepted || result.removed + result.skipped_owned > kMaxConsumeTrayItems) return 0;
    return static_cast<LRESULT>(0x40000000u | result.removed | (result.skipped_owned << 11));
}
inline TrayConsumeResult DecodeConsumeTrayResult(DWORD_PTR value) {
    if ((value & ~DWORD_PTR{0x403fffff}) || !(value & 0x40000000u)) return {};
    TrayConsumeResult result{true, static_cast<uint32_t>(value & 2047), static_cast<uint32_t>((value >> 11) & 2047)};
    return result.removed + result.skipped_owned <= kMaxConsumeTrayItems ? result : TrayConsumeResult{};
}
// Invoke only from the picker's worker, after the selected paths were delivered.
inline TrayConsumeResult RequestConsumeTray(const Snapshot& snapshot, const std::vector<TrayItem>& items) {
    if (!snapshot.tray_available || !LiveEndpoint(snapshot) || snapshot.endpoint_pid != snapshot.tray_publisher_pid ||
        items.empty() || items.size() > kMaxConsumeTrayItems) return {};
    DWORD source_pid = 0;
    if (!GetWindowThreadProcessId(snapshot.tray_source, &source_pid) || source_pid != snapshot.tray_publisher_pid) return {};
    TrayConsumeRequest request{snapshot.tray_publisher_pid, snapshot.tray_source, snapshot.tray_shelf_id, {}};
    for (const auto& item : items) {
        if (item.shelf_id != request.shelf_id) return {};
        request.items.push_back({item.id, item.batch_id, item.path});
    }
    auto bytes = EncodeConsumeTray(request);
    if (bytes.empty()) return {};
    COPYDATASTRUCT data{kConsumeTrayMessage, static_cast<DWORD>(bytes.size()), bytes.data()};
    DWORD_PTR reply = 0;
    if (!SendMessageTimeoutW(snapshot.endpoint, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &reply)) return {};
    auto result = DecodeConsumeTrayResult(reply);
    if (result.accepted && result.removed + result.skipped_owned != items.size()) return {};
    return result;
}

struct TrayStageRequest {
    DWORD publisher_pid = 0;
    HWND source = nullptr;
    uint64_t shelf_id = 0; // 0 = active shelf
    std::vector<std::wstring> paths;
};
namespace stage_detail {
struct Header {
    DWORD version = 1, publisher_pid = 0, count = 0, reserved = 0;
    uint64_t source = 0, shelf_id = 0;
};
struct Row { wchar_t path[1024]{}; };
static_assert(sizeof(Header) == 32 && sizeof(Row) == 2048);
}
inline bool ValidStageTray(const TrayStageRequest& request) {
    if (!request.publisher_pid || !request.source || request.paths.empty() ||
        request.paths.size() > kMaxStageTrayItems) return false;
    for (const auto& path : request.paths)
        if (!detail::DiskPath(path)) return false;
    return true;
}
inline std::vector<unsigned char> EncodeStageTray(const TrayStageRequest& request) {
    if (!ValidStageTray(request)) return {};
    stage_detail::Header header;
    header.publisher_pid = request.publisher_pid;
    header.count = static_cast<DWORD>(request.paths.size());
    header.source = reinterpret_cast<ULONG_PTR>(request.source);
    header.shelf_id = request.shelf_id;
    std::vector<unsigned char> bytes(sizeof(header) + request.paths.size() * sizeof(stage_detail::Row));
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (size_t i = 0; i < request.paths.size(); ++i) {
        stage_detail::Row row;
        std::memcpy(row.path, request.paths[i].c_str(), (request.paths[i].size() + 1) * sizeof(wchar_t));
        std::memcpy(bytes.data() + sizeof(header) + i * sizeof(row), &row, sizeof(row));
    }
    return bytes;
}
inline bool DecodeStageTray(const COPYDATASTRUCT* data, TrayStageRequest& request) {
    if (!data || data->dwData != kStageTrayMessage || !data->lpData || data->cbData < sizeof(stage_detail::Header)) return false;
    stage_detail::Header header;
    std::memcpy(&header, data->lpData, sizeof(header));
    if (header.version != 1 || header.reserved || !header.count || header.count > kMaxStageTrayItems ||
        data->cbData != sizeof(header) + header.count * sizeof(stage_detail::Row) ||
        header.source > UINTPTR_MAX) return false;
    TrayStageRequest decoded;
    decoded.publisher_pid = header.publisher_pid;
    decoded.source = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(header.source));
    decoded.shelf_id = header.shelf_id;
    decoded.paths.reserve(header.count);
    const auto* bytes = static_cast<const unsigned char*>(data->lpData);
    for (size_t i = 0; i < header.count; ++i) {
        stage_detail::Row row;
        std::memcpy(&row, bytes + sizeof(header) + i * sizeof(row), sizeof(row));
        if (!wmemchr(row.path, 0, 1024)) return false;
        decoded.paths.emplace_back(row.path);
    }
    if (!ValidStageTray(decoded)) return false;
    request = std::move(decoded);
    return true;
}
// Invoke only off the picker UI thread (worker or modal runner).
inline uint32_t RequestStageTray(const Snapshot& snapshot, const std::vector<std::wstring>& paths) {
    if (!LiveEndpoint(snapshot) || paths.empty() || paths.size() > kMaxStageTrayItems) return 0;
    const HWND source = snapshot.tray_source ? snapshot.tray_source : snapshot.endpoint;
    const DWORD pub_pid = snapshot.tray_publisher_pid ? snapshot.tray_publisher_pid : snapshot.endpoint_pid;
    if (!source || !pub_pid) return 0;
    TrayStageRequest request{pub_pid, source, snapshot.tray_shelf_id, paths};
    auto bytes = EncodeStageTray(request);
    if (bytes.empty()) return 0;
    COPYDATASTRUCT data{kStageTrayMessage, static_cast<DWORD>(bytes.size()), bytes.data()};
    DWORD_PTR reply = 0;
    if (!SendMessageTimeoutW(snapshot.endpoint, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &reply)) return 0;
    return static_cast<uint32_t>(reply);
}
}
