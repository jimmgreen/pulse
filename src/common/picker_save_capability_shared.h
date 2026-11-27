#pragma once
// Included after picker_places_shared.h; existing wire layouts stay unchanged.
namespace pulse::picker_places {
namespace save_capability_detail {
inline constexpr DWORD kMagic = 0x31435350;
struct Payload {
    DWORD magic = 0, version = 0, pid = 0, available = 0;
    ULONGLONG timestamp = 0, endpoint = 0;
};
struct Blob { volatile LONG sequence = 0; Payload payload; };
static_assert(sizeof(Blob) <= 64);
inline std::wstring Name() {
    const auto base = detail::Name();
    return base.empty() ? base : base + L".SaveFollowup.1";
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
inline bool Valid(const Payload& value, const Snapshot& snapshot, ULONGLONG now) {
    return value.magic == kMagic && value.version == 1 && value.pid && value.available <= 1 &&
        value.timestamp <= now && now - value.timestamp <= 10000 && LiveEndpoint(snapshot) &&
        value.pid == snapshot.endpoint_pid && value.endpoint == reinterpret_cast<ULONG_PTR>(snapshot.endpoint);
}
inline void Publish(const Snapshot& snapshot, Writer& writer) {
    if (!writer.blob || !writer.mutex) return;
    Payload value;
    value.magic = kMagic;
    value.version = 1;
    value.pid = GetCurrentProcessId();
    value.available = snapshot.save_followup_available ? 1u : 0u;
    value.timestamp = GetTickCount64();
    value.endpoint = reinterpret_cast<ULONG_PTR>(snapshot.endpoint);
    const DWORD wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    writer.blob->payload = value;
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
inline void Read(Snapshot& snapshot, const std::wstring& name) {
    snapshot.save_followup_available = false;
    if (name.empty() || !LiveEndpoint(snapshot)) return;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return;
    const auto* blob = static_cast<const Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Blob)));
    Payload value;
    bool copied = false;
    if (blob) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = blob->sequence;
            MemoryBarrier();
            if (before & 1) continue;
            value = blob->payload;
            MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    if (copied && Valid(value, snapshot, GetTickCount64())) snapshot.save_followup_available = value.available != 0;
}
}
inline void PublishSaveCapability(const Snapshot& snapshot) {
    static save_capability_detail::Writer writer;
    save_capability_detail::Publish(snapshot, writer);
}
inline void ReadSaveCapability(Snapshot& snapshot) {
    save_capability_detail::Read(snapshot, save_capability_detail::Name());
}
}
