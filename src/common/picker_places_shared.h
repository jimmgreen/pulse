#pragma once
// Included by picker_places_snapshot.h; the v1 mapping remains byte-for-byte compatible.
namespace pulse::picker_places {
namespace detail {
constexpr ULONG_PTR kAddStarredMessage = 0x50535432;
struct SharedRow {
    DWORD type = 0, kind = 0, color = 0;
    wchar_t path[1024]{}, name[128]{}, group[128]{};
};
struct SharedPayload {
    DWORD magic = 0, version = 0, pid = 0, count = 0;
    ULONGLONG timestamp = 0, endpoint = 0;
    SharedRow rows[384]{};
};
struct SharedBlob { volatile LONG sequence; SharedPayload payload; };
inline std::wstring SharedName() { auto name = Name(); return name.empty() ? name : name + L".v2"; }
struct SharedWriter {
    HANDLE mapping = nullptr, mutex = nullptr;
    SharedBlob* blob = nullptr;
    SharedWriter() {
        const auto name = SharedName();
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBlob), name.c_str());
        if (mapping) blob = static_cast<SharedBlob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedBlob)));
    }
    ~SharedWriter() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Text(const std::wstring& value, size_t capacity) {
    return value.size() < capacity && value.find(L'\0') == std::wstring::npos;
}
inline bool DiskPath(const std::wstring& path) {
    if (!Text(path, 1024) || path.find_first_of(L"\r\n") != std::wstring::npos) return false;
    if (path.starts_with(L"\\\\?\\UNC\\")) return DiskPath(L"\\\\" + path.substr(8));
    if (path.starts_with(L"\\\\?\\") && path.size() >= 7 && path[5] == L':') return DiskPath(path.substr(4));
    const bool drive = path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    if (drive) return true;
    if (!path.starts_with(L"\\\\") || path.size() < 5 || path[2] == L'.' || path[2] == L'?') return false;
    const auto split = path.find(L'\\', 2);
    return split != std::wstring::npos && split > 2 && split + 1 < path.size() && path[split + 1] != L'\\';
}
inline bool ValidShared(const SharedPayload& p) {
    const auto now = GetTickCount64();
    if (p.magic != kMagic || p.version != 2 || !p.pid || p.count > 384 ||
        p.timestamp > now || now - p.timestamp > 10000) return false;
    DWORD owner = 0;
    GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(p.endpoint)), &owner);
    if (owner != p.pid) return false;
    for (DWORD i = 0; i < p.count; ++i) {
        const auto& r = p.rows[i];
        if (r.type < 1 || r.type > 3 || r.kind > 2 || !wmemchr(r.path, 0, 1024) ||
            !wmemchr(r.name, 0, 128) || !wmemchr(r.group, 0, 128)) return false;
        if ((r.type != 3 && !DiskPath(r.path)) || (r.type == 3 && !r.group[0])) return false;
    }
    return true;
}
}
inline void PublishShared(const Snapshot& snapshot) {
    static detail::SharedWriter writer;
    if (!writer.blob || !writer.mutex || !snapshot.endpoint) return;
    auto p = std::make_unique<detail::SharedPayload>();
    p->magic = detail::kMagic; p->version = 2; p->pid = GetCurrentProcessId();
    p->timestamp = GetTickCount64(); p->endpoint = reinterpret_cast<ULONG_PTR>(snapshot.endpoint);
    auto add = [&](DWORD type, const std::wstring& path, const std::wstring& name,
                   const std::wstring& group, DWORD kind, DWORD color) {
        if (p->count == 384 || !detail::Text(path, 1024) || !detail::Text(name, 128) ||
            !detail::Text(group, 128) || (type != 3 && !detail::DiskPath(path))) return;
        auto& r = p->rows[p->count++]; r.type = type; r.kind = kind; r.color = color;
        std::memcpy(r.path, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
        std::memcpy(r.name, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        std::memcpy(r.group, group.c_str(), (group.size() + 1) * sizeof(wchar_t));
    };
    for (const auto& w : snapshot.workspaces) add(1, w.root, w.name, {}, 0, w.color_rgb);
    for (const auto& g : snapshot.starred_groups) add(3, {}, g.name, g.id, 0, 0);
    for (const auto& i : snapshot.starred_items) add(2, i.path, {}, i.group, i.kind, 0);
    const auto wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    std::memcpy(&writer.blob->payload, p.get(), sizeof(*p));
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
inline void ReadShared(Snapshot& snapshot) {
    const auto name = detail::SharedName();
    if (name.empty()) return;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return;
    const auto* blob = static_cast<const detail::SharedBlob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(detail::SharedBlob)));
    auto p = std::make_unique<detail::SharedPayload>();
    bool copied = false;
    if (blob) {
        for (int i = 0; i < 3; ++i) {
            const LONG before = blob->sequence; MemoryBarrier();
            if (before & 1) continue;
            std::memcpy(p.get(), &blob->payload, sizeof(*p)); MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    if (!copied || !detail::ValidShared(*p)) return;
    snapshot.endpoint = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(p->endpoint));
    snapshot.endpoint_pid = p->pid;
    snapshot.shared_available = true;
    for (DWORD i = 0; i < p->count; ++i) {
        const auto& r = p->rows[i];
        if (r.type == 1) snapshot.workspaces.push_back({r.path, r.name, r.color});
        if (r.type == 2) snapshot.starred_items.push_back({r.path, r.kind, r.group});
        if (r.type == 3) snapshot.starred_groups.push_back({r.group, r.name});
    }
}
struct AddStarredRequest { std::wstring path; uint32_t kind = 0; std::wstring group; };
inline bool LiveEndpoint(const Snapshot& snapshot) {
    DWORD owner = 0;
    return snapshot.shared_available && snapshot.endpoint && snapshot.endpoint_pid &&
        GetWindowThreadProcessId(snapshot.endpoint, &owner) && owner == snapshot.endpoint_pid;
}
struct AddStarredWire { DWORD version = 2, kind = 0; wchar_t path[1024]{}, group[128]{}; };
constexpr ULONG_PTR kAddStarredBatchMessage = 0x50534232;
inline bool ValidAddStarred(const AddStarredRequest& r) {
    return detail::DiskPath(r.path) && r.kind >= 1 && r.kind <= 2 &&
        detail::Text(r.group, 128) && (r.group.empty() || r.kind == 2);
}
inline bool DecodeAddStarred(const COPYDATASTRUCT* data, AddStarredRequest& request) {
    if (!data || data->dwData != detail::kAddStarredMessage || !data->lpData || data->cbData != sizeof(AddStarredWire)) return false;
    AddStarredWire wire;
    std::memcpy(&wire, data->lpData, sizeof(wire));
    if (wire.version != 2 || !wmemchr(wire.path, 0, 1024) || !wmemchr(wire.group, 0, 128)) return false;
    AddStarredRequest result{wire.path, wire.kind, wire.group};
    if (!ValidAddStarred(result)) return false;
    request = std::move(result); return true;
}
inline bool DecodeAddStarredBatch(const COPYDATASTRUCT* data, std::vector<AddStarredRequest>& requests) {
    if (!data || data->dwData != kAddStarredBatchMessage || !data->lpData ||
        !data->cbData || data->cbData % sizeof(AddStarredWire) || data->cbData / sizeof(AddStarredWire) > 64) return false;
    std::vector<AddStarredRequest> result;
    const auto* bytes = static_cast<const unsigned char*>(data->lpData);
    for (size_t offset = 0; offset < data->cbData; offset += sizeof(AddStarredWire)) {
        COPYDATASTRUCT item{detail::kAddStarredMessage, sizeof(AddStarredWire), const_cast<unsigned char*>(bytes + offset)};
        AddStarredRequest request;
        if (!DecodeAddStarred(&item, request)) return false;
        result.push_back(std::move(request));
    }
    requests = std::move(result); return true;
}
inline bool RequestAddStarredBatch(const Snapshot& snapshot, const std::vector<StarredItem>& items, const std::wstring& group = {}) {
    if (!LiveEndpoint(snapshot) || items.empty() || items.size() > 64) return false;
    std::vector<AddStarredWire> wires(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (!ValidAddStarred({item.path, item.kind, group})) return false;
        auto& wire = wires[i]; wire.kind = item.kind;
        std::memcpy(wire.path, item.path.c_str(), (item.path.size() + 1) * sizeof(wchar_t));
        std::memcpy(wire.group, group.c_str(), (group.size() + 1) * sizeof(wchar_t));
    }
    COPYDATASTRUCT data{kAddStarredBatchMessage, static_cast<DWORD>(wires.size() * sizeof(AddStarredWire)), wires.data()};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(snapshot.endpoint, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result) && result != 0;
}
enum class ManagePlacesAction : DWORD {
    Remove = 1, SetGroup, CreateGroup, RenameGroup, DeleteGroup,
    PinWorkspace, UnpinWorkspace, RenameWorkspace, MoveWorkspaceUp, MoveWorkspaceDown
};
struct ManagePlacesRequest {
    ManagePlacesAction action = ManagePlacesAction::Remove;
    std::vector<std::wstring> paths;
    std::wstring group, name;
};
inline constexpr ULONG_PTR kManagePlacesMessage = 0x50534D32;
struct ManagePlacesWire {
    DWORD version = 2, action = 0, count = 0;
    wchar_t group[128]{}, name[128]{}, paths[64][1024]{};
};
inline std::wstring TrimGroupName(const std::wstring& name) {
    const auto first = name.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? std::wstring{} : name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
}
inline bool ValidManagePlaces(const ManagePlacesRequest& request) {
    if (request.paths.size() > 64 || !detail::Text(request.group, 128) || !detail::Text(request.name, 128) ||
        request.group.find_first_of(L"\r\n") != std::wstring::npos || request.name.find_first_of(L"\r\n") != std::wstring::npos) return false;
    for (const auto& path : request.paths) if (!detail::DiskPath(path)) return false;
    switch (request.action) {
    case ManagePlacesAction::Remove:
        return !request.paths.empty() && request.group.empty() && request.name.empty();
    case ManagePlacesAction::SetGroup:
        return !request.paths.empty() && request.name.empty();
    case ManagePlacesAction::CreateGroup:
        return request.paths.size() == 1 && request.group.empty() && !TrimGroupName(request.name).empty();
    case ManagePlacesAction::RenameGroup:
        return request.paths.empty() && !request.group.empty() && !TrimGroupName(request.name).empty();
    case ManagePlacesAction::DeleteGroup:
        return request.paths.empty() && !request.group.empty() && request.name.empty();
    case ManagePlacesAction::PinWorkspace:
        return request.paths.size() == 1 && request.group.empty() && (request.name.empty() || !TrimGroupName(request.name).empty());
    case ManagePlacesAction::RenameWorkspace:
        return request.paths.size() == 1 && request.group.empty() && !TrimGroupName(request.name).empty();
    case ManagePlacesAction::UnpinWorkspace:
    case ManagePlacesAction::MoveWorkspaceUp:
    case ManagePlacesAction::MoveWorkspaceDown:
        return request.paths.size() == 1 && request.group.empty() && request.name.empty();
    default: return false;
    }
}
inline bool DecodeManagePlaces(const COPYDATASTRUCT* data, ManagePlacesRequest& request) {
    if (!data || data->dwData != kManagePlacesMessage || !data->lpData || data->cbData != sizeof(ManagePlacesWire)) return false;
    auto wire = std::make_unique<ManagePlacesWire>();
    std::memcpy(wire.get(), data->lpData, sizeof(*wire));
    if (wire->version != 2 || wire->count > 64 || !wmemchr(wire->group, 0, 128) || !wmemchr(wire->name, 0, 128)) return false;
    ManagePlacesRequest result{static_cast<ManagePlacesAction>(wire->action), {}, wire->group, wire->name};
    for (DWORD i = 0; i < wire->count; ++i) {
        if (!wmemchr(wire->paths[i], 0, 1024)) return false;
        result.paths.emplace_back(wire->paths[i]);
    }
    if (!ValidManagePlaces(result)) return false;
    request = std::move(result); return true;
}
// Worker-only; success acknowledges in-memory application and a queued save.
inline bool RequestManagePlaces(const Snapshot& snapshot, const ManagePlacesRequest& request) {
    if (!LiveEndpoint(snapshot) || !ValidManagePlaces(request)) return false;
    auto wire = std::make_unique<ManagePlacesWire>();
    wire->action = static_cast<DWORD>(request.action); wire->count = static_cast<DWORD>(request.paths.size());
    std::memcpy(wire->group, request.group.c_str(), (request.group.size() + 1) * sizeof(wchar_t));
    std::memcpy(wire->name, request.name.c_str(), (request.name.size() + 1) * sizeof(wchar_t));
    for (size_t i = 0; i < request.paths.size(); ++i)
        std::memcpy(wire->paths[i], request.paths[i].c_str(), (request.paths[i].size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT data{kManagePlacesMessage, sizeof(ManagePlacesWire), wire.get()};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(snapshot.endpoint, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result) && result != 0;
}
inline bool RequestRemoveStarred(const Snapshot& s, const std::vector<std::wstring>& paths) {
    return RequestManagePlaces(s, {ManagePlacesAction::Remove, paths, {}, {}});
}
inline bool RequestSetStarredGroup(const Snapshot& s, const std::vector<std::wstring>& paths, const std::wstring& group) {
    return RequestManagePlaces(s, {ManagePlacesAction::SetGroup, paths, group, {}});
}
inline bool RequestCreateStarredGroup(const Snapshot& s, const std::wstring& first_path, const std::wstring& name) {
    return RequestManagePlaces(s, {ManagePlacesAction::CreateGroup, {first_path}, {}, name});
}
inline bool RequestRenameStarredGroup(const Snapshot& s, const std::wstring& group, const std::wstring& name) {
    return RequestManagePlaces(s, {ManagePlacesAction::RenameGroup, {}, group, name});
}
inline bool RequestDeleteStarredGroup(const Snapshot& s, const std::wstring& group) {
    return RequestManagePlaces(s, {ManagePlacesAction::DeleteGroup, {}, group, {}});
}
inline bool RequestPinWorkspace(const Snapshot& s, const std::wstring& root, const std::wstring& name = {}) {
    return RequestManagePlaces(s, {ManagePlacesAction::PinWorkspace, {root}, {}, name});
}
inline bool RequestUnpinWorkspace(const Snapshot& s, const std::wstring& root) {
    return RequestManagePlaces(s, {ManagePlacesAction::UnpinWorkspace, {root}, {}, {}});
}
inline bool RequestRenameWorkspace(const Snapshot& s, const std::wstring& root, const std::wstring& name) {
    return RequestManagePlaces(s, {ManagePlacesAction::RenameWorkspace, {root}, {}, name});
}
inline bool RequestMoveWorkspace(const Snapshot& s, const std::wstring& root, bool up) {
    return RequestManagePlaces(s, {up ? ManagePlacesAction::MoveWorkspaceUp : ManagePlacesAction::MoveWorkspaceDown, {root}, {}, {}});
}
// Invoke on a worker thread: the endpoint may be busy or unavailable.
inline bool RequestAddStarred(const Snapshot& snapshot, const std::wstring& path, uint32_t kind, const std::wstring& group = {}) {
    if (!LiveEndpoint(snapshot) || !ValidAddStarred({path, kind, group})) return false;
    AddStarredWire wire; wire.kind = kind;
    std::memcpy(wire.path, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    std::memcpy(wire.group, group.c_str(), (group.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT data{detail::kAddStarredMessage, sizeof(wire), &wire};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(snapshot.endpoint, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &result) && result != 0;
}
}
