#pragma once
// Included after picker_places_shared.h. Existing mapping layouts remain unchanged.
namespace pulse::picker_places {
namespace archive_runtime_detail {
inline constexpr DWORD kMagic = 0x31524150;
struct Payload {
    DWORD magic = 0, version = 0, pid = 0, path_length = 0;
    ULONGLONG timestamp = 0, endpoint = 0;
    wchar_t worker[1024]{};
};
struct Blob { volatile LONG sequence = 0; Payload payload; };
static_assert(sizeof(Blob) < 4096);
inline std::wstring Name() {
    const auto base = detail::Name();
    return base.empty() ? base : base + L".ArchiveRuntime.1";
}
struct Writer {
    HANDLE mapping = nullptr, mutex = nullptr;
    Blob* blob = nullptr;
    explicit Writer(const std::wstring& name = Name()) {
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Blob), name.c_str());
        if (mapping) blob = static_cast<Blob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Blob)));
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Valid(const Payload& p, const Snapshot& snapshot, ULONGLONG now) {
    if (p.magic != kMagic || p.version != 1 || !p.pid || p.path_length >= 1024 ||
        p.timestamp > now || now - p.timestamp > 10000 || !LiveEndpoint(snapshot) ||
        p.pid != snapshot.endpoint_pid || p.endpoint != reinterpret_cast<ULONG_PTR>(snapshot.endpoint) ||
        p.worker[p.path_length] || wmemchr(p.worker, 0, p.path_length)) return false;
    return !p.path_length || detail::DiskPath(std::wstring(p.worker, p.path_length));
}
inline void Publish(const Snapshot& snapshot, Writer& writer) {
    if (!writer.blob || !writer.mutex) return;
    Payload value;
    value.magic = kMagic; value.version = 1; value.pid = GetCurrentProcessId();
    value.timestamp = GetTickCount64(); value.endpoint = reinterpret_cast<ULONG_PTR>(snapshot.endpoint);
    if (detail::DiskPath(snapshot.archive_worker)) {
        value.path_length = static_cast<DWORD>(snapshot.archive_worker.size());
        std::memcpy(value.worker, snapshot.archive_worker.data(), value.path_length * sizeof(wchar_t));
    }
    const DWORD wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    writer.blob->payload = value;
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
inline void Read(Snapshot& snapshot, const std::wstring& name) {
    snapshot.archive_worker.clear();
    if (name.empty() || !LiveEndpoint(snapshot)) return;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return;
    const auto* blob = static_cast<const Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Blob)));
    Payload value;
    bool copied = false;
    if (blob) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = blob->sequence; MemoryBarrier();
            if (before & 1) continue;
            value = blob->payload; MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    if (copied && Valid(value, snapshot, GetTickCount64()))
        snapshot.archive_worker.assign(value.worker, value.path_length);
}
}
inline void PublishArchiveRuntime(const Snapshot& snapshot) {
    static archive_runtime_detail::Writer writer;
    archive_runtime_detail::Publish(snapshot, writer);
}
inline void ReadArchiveRuntime(Snapshot& snapshot) {
    archive_runtime_detail::Read(snapshot, archive_runtime_detail::Name());
}
}
