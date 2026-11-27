#pragma once
#include "picker_save_watch.h"
#include "picker_places_snapshot.h"
#include "picker_user_storage.h"
#include <algorithm>
#include <cstring>

namespace pulse::picker_save {
inline constexpr ULONG_PTR kMessage = 0x50535731;
namespace wire {
struct Header {
    DWORD version = 1, flags = 0, source_pid = 0, path_units = 0;
    DWORD app_units = 0, exists = 0, attributes = 0, reserved = 0;
    uint64_t source_window = 0, request_id = 0, issued_at = 0;
    uint64_t volume = 0, size = 0, write_time = 0;
    std::array<BYTE, 16> file_id{};
};
static_assert(sizeof(Header) == 96);
inline bool Valid(const Request& request) {
    picker_user_storage::Path parsed;
    return request.source_pid && request.request_id && request.path.size() < 32768 &&
        picker_user_storage::Parse(request.path, parsed) && request.path.find(L'\0') == request.path.npos &&
        request.app_name.size() <= 256 && request.app_name.find(L'\0') == request.app_name.npos &&
        !(request.before.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
}
inline std::vector<BYTE> Encode(const Request& request, HWND source) {
    DWORD pid = 0;
    if (!wire::Valid(request) || !GetWindowThreadProcessId(source, &pid) || pid != request.source_pid) return {};
    wire::Header header;
    header.flags = (request.notify_if_no_tab ? 1u : 0u) | (request.copy_path ? 2u : 0u) |
        (request.stage_to_tray ? 4u : 0u);
    header.source_pid = request.source_pid; header.source_window = reinterpret_cast<UINT_PTR>(source);
    header.request_id = request.request_id; header.issued_at = GetTickCount64();
    header.path_units = static_cast<DWORD>(request.path.size()); header.app_units = static_cast<DWORD>(request.app_name.size());
    header.exists = request.before.exists ? 1u : 0u; header.volume = request.before.volume;
    header.file_id = request.before.file_id; header.size = request.before.size;
    header.write_time = request.before.write_time; header.attributes = request.before.attributes;
    std::vector<BYTE> bytes(sizeof(header) + (request.path.size() + request.app_name.size()) * sizeof(wchar_t));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), request.path.data(), request.path.size() * sizeof(wchar_t));
    std::memcpy(bytes.data() + sizeof(header) + request.path.size() * sizeof(wchar_t), request.app_name.data(), request.app_name.size() * sizeof(wchar_t));
    return bytes;
}
inline bool Decode(const COPYDATASTRUCT* data, Request& request) {
    if (!data || data->dwData != kMessage || !data->lpData || data->cbData < sizeof(wire::Header)) return false;
    wire::Header h; std::memcpy(&h, data->lpData, sizeof(h));
    const auto now = GetTickCount64();
    if (h.version != 1 || (h.flags & ~7u) || h.reserved || h.exists > 1 || !h.path_units ||
        h.path_units >= 32768 || h.app_units > 256 || h.source_window > UINTPTR_MAX ||
        !h.issued_at || h.issued_at > now || now - h.issued_at > 10000 ||
        data->cbData != sizeof(h) + (h.path_units + h.app_units) * sizeof(wchar_t)) return false;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<UINT_PTR>(h.source_window)), &pid) || pid != h.source_pid) return false;
    Request next;
    next.path.resize(h.path_units); next.app_name.resize(h.app_units);
    const auto* bytes = static_cast<const BYTE*>(data->lpData) + sizeof(h);
    std::memcpy(next.path.data(), bytes, h.path_units * sizeof(wchar_t));
    std::memcpy(next.app_name.data(), bytes + h.path_units * sizeof(wchar_t), h.app_units * sizeof(wchar_t));
    next.source_pid = h.source_pid; next.request_id = h.request_id;
    next.notify_if_no_tab = (h.flags & 1) != 0; next.copy_path = (h.flags & 2) != 0;
    next.stage_to_tray = (h.flags & 4) != 0;
    next.before = {h.exists != 0, h.volume, h.file_id, h.size, h.write_time, h.attributes};
    if (!wire::Valid(next)) return false;
    request = std::move(next); return true;
}
// Worker-only send: result delivery itself must never wait for another process.
inline bool Send(const picker_places::Snapshot& endpoint, const Request& request) {
    if (!picker_places::LiveEndpoint(endpoint)) return false;
    HWND source = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!source) return false;
    const auto bytes = Encode(request, source);
    DWORD_PTR accepted = 0;
    if (!bytes.empty()) {
        COPYDATASTRUCT data{kMessage, static_cast<DWORD>(bytes.size()), const_cast<BYTE*>(bytes.data())};
        SendMessageTimeoutW(endpoint.endpoint, WM_COPYDATA, reinterpret_cast<WPARAM>(source), reinterpret_cast<LPARAM>(&data),
            SMTO_BLOCK | SMTO_ABORTIFHUNG, 1500, &accepted);
    }
    DestroyWindow(source);
    return accepted == TRUE;
}
}
