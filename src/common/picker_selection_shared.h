#pragma once
// Included after picker_places_shared.h. A separate mapping leaves v1/v2/v3 unchanged.
namespace pulse::picker_places {
namespace selection_detail {
inline std::wstring Name() { const auto base = detail::Name(); return base.empty() ? base : base + L".selection.1"; }
struct Row { wchar_t path[1024]{}; DWORD directory = 0; uint64_t size = 0; };
struct Payload {
    DWORD magic = detail::kMagic, version = 1, pid = 0, count = 0, available = 0;
    ULONGLONG timestamp = 0, source = 0;
    Row rows[kMaxSelectedItems]{};
};
struct Blob { volatile LONG sequence; Payload payload; };
struct Writer {
    HANDLE mapping = nullptr, mutex = nullptr;
    Blob* blob = nullptr;
    explicit Writer(const std::wstring& name = Name()) {
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Blob), name.c_str());
        if (mapping) blob = static_cast<Blob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Blob)));
    }
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Valid(const Payload& p) {
    const auto now = GetTickCount64();
    if (p.magic != detail::kMagic || p.version != 1 || !p.pid || p.available > 1 ||
        p.count > kMaxSelectedItems || (!p.available && p.count) || p.timestamp > now || now - p.timestamp > 10000) return false;
    DWORD owner = 0;
    if (!GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(p.source)), &owner) || owner != p.pid) return false;
    for (DWORD i = 0; i < p.count; ++i) {
        const auto& row = p.rows[i];
        if (row.directory > 1 || !wmemchr(row.path, 0, 1024) || !detail::DiskPath(row.path)) return false;
    }
    return true;
}
inline void Publish(const Snapshot& snapshot, Writer& writer) {
    if (!writer.blob || !writer.mutex) return;
    auto p = std::make_unique<Payload>();
    p->pid = GetCurrentProcessId(); p->timestamp = GetTickCount64();
    p->source = reinterpret_cast<ULONG_PTR>(snapshot.selection_source);
    p->available = snapshot.selection_available && snapshot.selected_items.size() <= kMaxSelectedItems;
    if (p->available) for (const auto& item : snapshot.selected_items) {
        // Never publish a truncated subset of a user's selection.
        if (!detail::DiskPath(item.path)) { p->available = 0; p->count = 0; break; }
        auto& row = p->rows[p->count++];
        std::memcpy(row.path, item.path.c_str(), (item.path.size() + 1) * sizeof(wchar_t));
        row.directory = item.is_directory ? 1 : 0; row.size = item.size;
    }
    const auto wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    std::memcpy(&writer.blob->payload, p.get(), sizeof(*p));
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
inline void Read(Snapshot& snapshot, const std::wstring& name) {
    snapshot.selected_items.clear(); snapshot.selection_available = false; snapshot.selection_source = nullptr;
    if (name.empty()) return;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return;
    const auto* blob = static_cast<const Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Blob)));
    auto p = std::make_unique<Payload>();
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
    if (!copied || !Valid(*p)) return;
    snapshot.selection_source = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(p->source));
    snapshot.selection_available = p->available != 0;
    for (DWORD i = 0; i < p->count; ++i)
        snapshot.selected_items.push_back({p->rows[i].path, p->rows[i].directory != 0, p->rows[i].size});
}
}
inline void PublishSelection(const Snapshot& snapshot) {
    static selection_detail::Writer writer;
    selection_detail::Publish(snapshot, writer);
}
inline void ReadSelection(Snapshot& snapshot) { selection_detail::Read(snapshot, selection_detail::Name()); }
}
