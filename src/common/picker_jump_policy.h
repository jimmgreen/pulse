#pragma once
// Separate versioned mapping: older places readers keep their original layout.
namespace pulse::picker_places {
namespace jump_policy_detail {
struct Payload { DWORD magic = 0, version = 0, pid = 0, flags = 0; ULONGLONG timestamp = 0; };
struct Blob { volatile LONG sequence = 0; Payload payload; };
inline std::wstring Name() { const auto base = detail::Name(); return base.empty() ? base : base + L".JumpPolicy.1"; }
struct Writer {
    HANDLE mapping = nullptr, mutex = nullptr;
    Blob* blob = nullptr;
    Writer() {
        const auto name = Name(); if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Blob), name.c_str());
        if (mapping) blob = static_cast<Blob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Blob)));
    }
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Valid(const Payload& value, ULONGLONG now) {
    return value.magic == 0x314a5050 && value.version == 1 && value.pid && !(value.flags & ~15u) &&
        value.timestamp <= now && now - value.timestamp <= 10000;
}
}
inline void PublishJumpPolicy(const Snapshot& snapshot) {
    static jump_policy_detail::Writer writer;
    if (!writer.blob || !writer.mutex) return;
    const DWORD wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    jump_policy_detail::Payload value{0x314a5050, 1, GetCurrentProcessId(),
        (snapshot.auto_jump ? 1u : 0u) | (snapshot.remember ? 2u : 0u) |
        (snapshot.pinyin ? 4u : 0u) | (snapshot.index ? 8u : 0u), GetTickCount64()};
    if (writer.blob->sequence & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    writer.blob->payload = value;
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
inline void ReadJumpPolicy(Snapshot& snapshot) {
    const auto name = jump_policy_detail::Name(); if (name.empty()) return;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()); if (!mapping) return;
    const auto* blob = static_cast<const jump_policy_detail::Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(jump_policy_detail::Blob)));
    jump_policy_detail::Payload value{}; bool copied = false;
    if (blob) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = blob->sequence; MemoryBarrier(); if (before & 1) continue;
            value = blob->payload; MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    if (!copied || !jump_policy_detail::Valid(value, GetTickCount64())) return;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, value.pid); if (!process) return;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT; CloseHandle(process); if (!alive) return;
    snapshot.jump_policy_available = true;
    snapshot.auto_jump = (value.flags & 1) != 0; snapshot.remember = (value.flags & 2) != 0;
    snapshot.pinyin = (value.flags & 4) != 0; snapshot.index = (value.flags & 8) != 0;
}
}
