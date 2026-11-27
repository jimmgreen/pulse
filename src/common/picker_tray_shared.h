#pragma once
// Included by picker_places_snapshot.h after the legacy places helpers.
#include <cstddef>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_set>

namespace pulse::picker_places {

// The real model's active shelf only. The const model accessors never probe paths.
template<class Shelf>
void CaptureTray(const Shelf& shelf, const std::wstring& name, HWND source, Snapshot& snapshot) {
    snapshot.tray_items.clear(); snapshot.tray_available = true;
    snapshot.tray_shelf_id = shelf.id; snapshot.tray_shelf_name = name;
    snapshot.tray_source = source; snapshot.tray_publisher_pid = GetCurrentProcessId();
    snapshot.tray_total_count = 0; snapshot.tray_revision = 0;
    for (const auto& batch : shelf.batches) {
        const auto count = static_cast<uint64_t>(batch.items.size());
        if (UINT64_MAX - snapshot.tray_total_count < count) snapshot.tray_total_count = UINT64_MAX;
        else snapshot.tray_total_count += count;
    }
    if (snapshot.tray_total_count > kMaxTraySnapshotItems) { snapshot.tray_available = false; return; }
    for (const auto& batch : shelf.batches) for (const auto& item : batch.items) {
        TrayItem entry;
        entry.id = item.id; entry.batch_id = batch.id; entry.shelf_id = shelf.id;
        entry.path = item.path; entry.label = item.display_name;
        entry.is_directory = item.is_dir; entry.size = item.size; entry.exists = item.exists;
        entry.inflight = item.inflight_task != 0; entry.owned_snippet = item.owned_snippet;
        entry.attrs = item.attrs; entry.content_kind = item.content_kind;
        entry.domain = item.domain; entry.source_label = item.source_label;
        snapshot.tray_items.push_back(std::move(entry));
    }
}
inline bool RefreshTraySnapshot(Snapshot& target, const Snapshot& source) {
    const bool available = source.tray_available && source.tray_items.size() <= kMaxTraySnapshotItems;
    const bool same_items = available ? target.tray_items == source.tray_items : target.tray_items.empty();
    if (target.tray_available == available && same_items && target.tray_shelf_id == source.tray_shelf_id &&
        target.tray_revision == source.tray_revision && target.tray_shelf_name == source.tray_shelf_name &&
        target.tray_total_count == source.tray_total_count && target.tray_source == source.tray_source &&
        target.tray_publisher_pid == source.tray_publisher_pid) return false;
    target.tray_available = available;
    target.tray_items = available ? source.tray_items : std::vector<TrayItem>{};
    target.tray_shelf_id = source.tray_shelf_id; target.tray_shelf_name = source.tray_shelf_name;
    target.tray_revision = source.tray_revision; target.tray_total_count = source.tray_total_count;
    target.tray_source = source.tray_source; target.tray_publisher_pid = source.tray_publisher_pid;
    return true;
}
namespace tray_detail {
inline std::wstring Name() { const auto base = detail::Name(); return base.empty() ? base : base + L".tray.1"; }
struct Row {
    uint64_t id = 0, batch_id = 0, size = 0;
    DWORD flags = 0, attrs = 0, content_kind = 0, reserved = 0;
    wchar_t path[1024]{}, label[256]{}, domain[128]{}, source_label[128]{};
};
struct Payload {
    DWORD magic = 0, version = 0, pid = 0, count = 0, available = 0, reserved = 0;
    uint64_t timestamp = 0, source = 0, shelf_id = 0, revision = 0, total_count = 0;
    wchar_t shelf_name[128]{};
    Row rows[kMaxTrayItems]{};
};
struct Blob { volatile LONG sequence; DWORD reserved; Payload payload; };
static_assert(sizeof(Row) == 3112 && offsetof(Payload, rows) == 320 && offsetof(Blob, payload) == 8);
struct Writer {
    HANDLE mapping = nullptr, mutex = nullptr;
    Blob* blob = nullptr;
    uint64_t revision = 0;
    std::unique_ptr<Payload> previous;
    explicit Writer(const std::wstring& name = Name()) {
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Blob), name.c_str());
        if (mapping) blob = static_cast<Blob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Blob)));
    }
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Valid(const Payload& p, bool require_fresh = true) {
    const auto now = GetTickCount64();
    if (p.magic != detail::kMagic || p.version != 1 || !p.pid || !p.shelf_id || !p.revision || p.available > 1 ||
        p.count > kMaxTrayItems || (p.available ? p.total_count != p.count : p.count != 0) ||
        p.timestamp > now || (require_fresh && now - p.timestamp > 10000) || !wmemchr(p.shelf_name, 0, 128)) return false;
    DWORD owner = 0;
    if (!GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(p.source)), &owner) || owner != p.pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, p.pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    if (!alive) return false;
    for (DWORD i = 0; i < p.count; ++i) {
        const auto& row = p.rows[i];
        if (!row.id || !row.batch_id || row.flags & ~15u || row.content_kind > 3 ||
            !wmemchr(row.path, 0, 1024) || !detail::DiskPath(row.path) || !wmemchr(row.label, 0, 256) ||
            !wmemchr(row.domain, 0, 128) || !wmemchr(row.source_label, 0, 128)) return false;
        for (DWORD j = 0; j < i; ++j) if (p.rows[j].id == row.id) return false;
    }
    return true;
}
inline std::unique_ptr<Payload> Encode(const Snapshot& snapshot) {
    auto p = std::make_unique<Payload>();
    p->magic = detail::kMagic; p->version = 1; p->pid = GetCurrentProcessId();
    p->source = reinterpret_cast<ULONG_PTR>(snapshot.tray_source); p->shelf_id = snapshot.tray_shelf_id;
    p->total_count = snapshot.tray_total_count;
    const bool name_valid = detail::Text(snapshot.tray_shelf_name, 128);
    p->available = snapshot.tray_available && name_valid && snapshot.tray_items.size() <= kMaxTrayItems &&
        snapshot.tray_total_count == snapshot.tray_items.size();
    if (name_valid) std::memcpy(p->shelf_name, snapshot.tray_shelf_name.c_str(), (snapshot.tray_shelf_name.size() + 1) * sizeof(wchar_t));
    if (p->available) for (const auto& item : snapshot.tray_items) {
        bool duplicate = false;
        for (DWORD j = 0; j < p->count; ++j) duplicate |= p->rows[j].id == item.id;
        if (!item.id || !item.batch_id || item.shelf_id != p->shelf_id || duplicate || !detail::DiskPath(item.path) ||
            !detail::Text(item.label, 256) || !detail::Text(item.domain, 128) || !detail::Text(item.source_label, 128) || item.content_kind > 3) {
            p->available = 0; p->count = 0;
            for (auto& row : p->rows) row = {};
            break;
        }
        auto& row = p->rows[p->count++];
        row.id = item.id; row.batch_id = item.batch_id; row.size = item.size; row.attrs = item.attrs; row.content_kind = item.content_kind;
        row.flags = (item.is_directory ? 1u : 0u) | (item.exists ? 2u : 0u) | (item.inflight ? 4u : 0u) | (item.owned_snippet ? 8u : 0u);
        const auto copy = [](wchar_t* dest, const std::wstring& text) { std::memcpy(dest, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); };
        copy(row.path, item.path); copy(row.label, item.label); copy(row.domain, item.domain); copy(row.source_label, item.source_label);
    }
    return p;
}
inline void Publish(const Snapshot& snapshot, Writer& writer) {
    if (!writer.blob || !writer.mutex) return;
    auto p = Encode(snapshot);
    auto next = std::make_unique<Payload>(*p);
    const auto wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    // Explicit wire fields leave no padding; timestamps/revisions are excluded from content identity.
    if (!writer.previous || std::memcmp(writer.previous.get(), p.get(), sizeof(*p))) ++writer.revision;
    writer.previous = std::move(next);
    p->revision = writer.revision; p->timestamp = GetTickCount64();
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    std::memcpy(&writer.blob->payload, p.get(), sizeof(*p));
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
inline void Read(Snapshot& snapshot, const std::wstring& name) {
    snapshot.tray_items.clear(); snapshot.tray_available = false;
    snapshot.tray_shelf_id = snapshot.tray_revision = snapshot.tray_total_count = 0;
    snapshot.tray_shelf_name.clear(); snapshot.tray_source = nullptr; snapshot.tray_publisher_pid = 0;
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
    snapshot.tray_available = p->available != 0; snapshot.tray_shelf_id = p->shelf_id;
    snapshot.tray_shelf_name = p->shelf_name; snapshot.tray_revision = p->revision; snapshot.tray_total_count = p->total_count;
    snapshot.tray_source = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(p->source)); snapshot.tray_publisher_pid = p->pid;
    for (DWORD i = 0; i < p->count; ++i) {
        const auto& row = p->rows[i];
        TrayItem item;
        item.id = row.id; item.batch_id = row.batch_id; item.shelf_id = p->shelf_id;
        item.path = row.path; item.label = row.label; item.size = row.size; item.attrs = row.attrs; item.content_kind = row.content_kind;
        item.is_directory = (row.flags & 1) != 0; item.exists = (row.flags & 2) != 0;
        item.inflight = (row.flags & 4) != 0; item.owned_snippet = (row.flags & 8) != 0;
        item.domain = row.domain; item.source_label = row.source_label;
        snapshot.tray_items.push_back(std::move(item));
    }
}
}
namespace tray_pages_detail {
inline std::wstring Name() { const auto base = detail::Name(); return base.empty() ? base : base + L".tray.2"; }
struct Manifest {
    DWORD magic = 0, version = 2, pid = 0, available = 0, pages = 0, reserved = 0;
    uint64_t timestamp = 0, source = 0, shelf_id = 0, revision = 0, total_count = 0;
    wchar_t shelf_name[128]{};
};
struct ManifestBlob { volatile LONG sequence; DWORD reserved; Manifest value; };
static_assert(sizeof(Manifest) == 320 && offsetof(ManifestBlob, value) == 8);
inline uint64_t NextRevision() {
    static std::atomic<uint64_t> sequence{GetTickCount64() << 16};
    return sequence.fetch_add(1) + 1;
}
inline std::wstring PageName(const std::wstring& base, const Manifest& m, size_t page) {
    return base + L".p." + std::to_wstring(m.pid) + L"." + std::to_wstring(m.revision) + L"." + std::to_wstring(page);
}
inline bool Valid(const Manifest& m) {
    const auto now = GetTickCount64();
    if (m.magic != detail::kMagic || m.version != 2 || !m.pid || !m.shelf_id || !m.revision || m.available > 1 ||
        m.timestamp > now || now - m.timestamp > 10000 || !wmemchr(m.shelf_name, 0, 128)) return false;
    if (m.available ? (m.total_count > kMaxTraySnapshotItems || m.pages != (m.total_count + kMaxTrayItems - 1) / kMaxTrayItems) : m.pages != 0) return false;
    DWORD owner = 0;
    if (!GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(m.source)), &owner) || owner != m.pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, m.pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process); return alive;
}
// Missing is distinct from invalid: a corrupt/new v2 must never resurrect stale v1 data.
enum class ReadStatus { Missing, Invalid, Valid };
inline ReadStatus ReadManifest(Manifest& m, const std::wstring& name) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return GetLastError() == ERROR_FILE_NOT_FOUND ? ReadStatus::Missing : ReadStatus::Invalid;
    const auto* blob = static_cast<const ManifestBlob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(ManifestBlob)));
    bool copied = false;
    if (blob) {
        for (int i = 0; i < 3; ++i) {
            const LONG before = blob->sequence; MemoryBarrier();
            if (before & 1) continue;
            std::memcpy(&m, &blob->value, sizeof(m)); MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    return copied && Valid(m) ? ReadStatus::Valid : ReadStatus::Invalid;
}
inline bool SameIdentity(const Manifest& a, const Manifest& b) {
    return a.pid == b.pid && a.source == b.source && a.shelf_id == b.shelf_id && a.revision == b.revision &&
        a.available == b.available && a.total_count == b.total_count && a.pages == b.pages;
}
inline void Metadata(Snapshot& snapshot, const Manifest& m) {
    snapshot.tray_available = m.available != 0; snapshot.tray_shelf_id = m.shelf_id;
    snapshot.tray_shelf_name = m.shelf_name; snapshot.tray_revision = m.revision; snapshot.tray_total_count = m.total_count;
    snapshot.tray_source = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(m.source)); snapshot.tray_publisher_pid = m.pid;
}
inline bool ReadPage(Snapshot& result, const Manifest& m, size_t index, const std::wstring& base) {
    if (!m.available || index >= m.pages) return false;
    const auto name = PageName(base, m, index);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return false;
    const auto* blob = static_cast<const tray_detail::Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(tray_detail::Blob)));
    auto p = std::make_unique<tray_detail::Payload>(); bool copied = false;
    if (blob) {
        for (int i = 0; i < 3; ++i) {
            const LONG before = blob->sequence; MemoryBarrier(); if (before & 1) continue;
            std::memcpy(p.get(), &blob->payload, sizeof(*p)); MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    const auto remaining = m.total_count - index * kMaxTrayItems;
    const auto expected = remaining < kMaxTrayItems ? remaining : kMaxTrayItems;
    if (!copied || !tray_detail::Valid(*p, false) || !p->available || p->pid != m.pid || p->source != m.source ||
        p->shelf_id != m.shelf_id || p->revision != m.revision || p->timestamp > m.timestamp || p->count != expected ||
        std::wcscmp(p->shelf_name, m.shelf_name) != 0) return false;
    for (DWORD i = 0; i < p->count; ++i) {
        const auto& r = p->rows[i]; TrayItem item;
        item.id = r.id; item.batch_id = r.batch_id; item.shelf_id = m.shelf_id; item.path = r.path; item.label = r.label;
        item.size = r.size; item.attrs = r.attrs; item.content_kind = r.content_kind;
        item.is_directory = (r.flags & 1) != 0; item.exists = (r.flags & 2) != 0;
        item.inflight = (r.flags & 4) != 0; item.owned_snippet = (r.flags & 8) != 0;
        item.domain = r.domain; item.source_label = r.source_label; result.tray_items.push_back(std::move(item));
    }
    return true;
}
inline bool SameSource(const Snapshot& a, const Snapshot& b) {
    return a.tray_available == b.tray_available && a.tray_items == b.tray_items && a.tray_shelf_id == b.tray_shelf_id &&
        a.tray_shelf_name == b.tray_shelf_name && a.tray_total_count == b.tray_total_count && a.tray_source == b.tray_source;
}
struct Writer {
    std::wstring name;
    HANDLE mapping = nullptr, mutex = nullptr;
    ManifestBlob* blob = nullptr;
    Snapshot previous;
    bool initialized = false;
    uint64_t last_revision = 0;
    std::vector<std::unique_ptr<tray_detail::Writer>> pages, previous_pages;
    explicit Writer(const std::wstring& value = Name()) : name(value) {
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ManifestBlob), name.c_str());
        if (mapping) blob = static_cast<ManifestBlob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(ManifestBlob)));
    }
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Publish(const Snapshot& snapshot, Writer& writer) {
    if (!writer.blob || !writer.mutex) return false;
    Manifest m;
    const bool current_valid = ReadManifest(m, writer.name) == ReadStatus::Valid;
    const bool changed = !writer.initialized || !current_valid || m.pid != GetCurrentProcessId() ||
        m.revision != writer.last_revision || !SameSource(writer.previous, snapshot);
    std::vector<std::unique_ptr<tray_detail::Writer>> pages;
    if (changed) {
        m = {};
        m.magic = detail::kMagic; m.pid = GetCurrentProcessId(); m.source = reinterpret_cast<ULONG_PTR>(snapshot.tray_source);
        m.shelf_id = snapshot.tray_shelf_id; m.total_count = snapshot.tray_total_count; m.revision = NextRevision();
        const bool name_valid = detail::Text(snapshot.tray_shelf_name, 128);
        if (name_valid) std::memcpy(m.shelf_name, snapshot.tray_shelf_name.c_str(), (snapshot.tray_shelf_name.size() + 1) * sizeof(wchar_t));
        m.available = snapshot.tray_available && name_valid && snapshot.tray_items.size() <= kMaxTraySnapshotItems && m.total_count == snapshot.tray_items.size();
        std::unordered_set<uint64_t> ids;
        if (m.available) for (const auto& item : snapshot.tray_items)
            if (!ids.insert(item.id).second) { m.available = 0; break; }
        if (m.available) for (size_t begin = 0; begin < snapshot.tray_items.size(); begin += kMaxTrayItems) {
            Snapshot chunk; chunk.tray_source = snapshot.tray_source; chunk.tray_shelf_id = snapshot.tray_shelf_id;
            chunk.tray_shelf_name = snapshot.tray_shelf_name; chunk.tray_available = true;
            const auto count = (std::min)(kMaxTrayItems, snapshot.tray_items.size() - begin);
            chunk.tray_total_count = count;
            chunk.tray_items.assign(snapshot.tray_items.begin() + begin, snapshot.tray_items.begin() + begin + count);
            auto payload = tray_detail::Encode(chunk);
            if (!payload->available) { m.available = 0; break; }
            payload->revision = m.revision; payload->timestamp = GetTickCount64();
            auto page = std::make_unique<tray_detail::Writer>(PageName(writer.name, m, pages.size()));
            if (!page->blob || !page->mutex) { m.available = 0; break; }
            // This generation's mapping is unpublished until the manifest commit below.
            InterlockedIncrement(&page->blob->sequence);
            std::memcpy(&page->blob->payload, payload.get(), sizeof(*payload));
            InterlockedIncrement(&page->blob->sequence);
            pages.push_back(std::move(page));
        }
        if (!m.available) pages.clear();
        m.pages = static_cast<DWORD>(pages.size());
    }
    Snapshot remembered;
    if (changed) RefreshTraySnapshot(remembered, snapshot);
    const auto lock = WaitForSingleObject(writer.mutex, 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) return false;
    m.timestamp = GetTickCount64();
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence); std::memcpy(&writer.blob->value, &m, sizeof(m)); InterlockedIncrement(&writer.blob->sequence);
    if (changed) {
        writer.previous_pages = std::move(writer.pages); writer.pages = std::move(pages);
        writer.previous = std::move(remembered); writer.initialized = true; writer.last_revision = m.revision;
    }
    ReleaseMutex(writer.mutex); return true;
}
inline ReadStatus Read(Snapshot& snapshot, const std::wstring& name, bool reuse = false) {
    Manifest m; const auto status = ReadManifest(m, name);
    if (status != ReadStatus::Valid) { RefreshTraySnapshot(snapshot, {}); return status; }
    if (reuse && snapshot.tray_available == (m.available != 0) && snapshot.tray_publisher_pid == m.pid &&
        snapshot.tray_source == reinterpret_cast<HWND>(static_cast<ULONG_PTR>(m.source)) && snapshot.tray_revision == m.revision &&
        snapshot.tray_shelf_id == m.shelf_id && snapshot.tray_total_count == m.total_count &&
        snapshot.tray_items.size() == (m.available ? m.total_count : 0)) return status;
    Snapshot result; Metadata(result, m);
    if (m.available) {
        result.tray_items.reserve(static_cast<size_t>(m.total_count));
        for (size_t i = 0; i < m.pages; ++i) if (!ReadPage(result, m, i, name)) { RefreshTraySnapshot(snapshot, {}); return ReadStatus::Invalid; }
        std::unordered_set<uint64_t> ids;
        for (const auto& item : result.tray_items) if (!ids.insert(item.id).second) { RefreshTraySnapshot(snapshot, {}); return ReadStatus::Invalid; }
    }
    Manifest after;
    if (ReadManifest(after, name) != ReadStatus::Valid || !SameIdentity(m, after)) { RefreshTraySnapshot(snapshot, {}); return ReadStatus::Invalid; }
    RefreshTraySnapshot(snapshot, result); return ReadStatus::Valid;
}
inline void ReadWithLegacy(Snapshot& snapshot, const std::wstring& name, const std::wstring& legacy, bool reuse = false) {
    if (Read(snapshot, name, reuse) == ReadStatus::Missing) tray_detail::Read(snapshot, legacy);
}
class Publisher {
public:
    explicit Publisher(std::wstring name = Name(), std::wstring legacy = tray_detail::Name())
        : thread_([this, name = std::move(name), legacy = std::move(legacy)] {
            Writer writer(name); tray_detail::Writer old(legacy);
            for (;;) {
                std::unique_ptr<Snapshot> snapshot;
                { std::unique_lock lock(mutex_); wake_.wait(lock, [&] { return stopping_ || pending_; });
                    if (!pending_) break; snapshot = std::move(pending_); }
                try { Publish(*snapshot, writer); tray_detail::Publish(*snapshot, old); } catch (...) { }
            }
        }) {}
    ~Publisher() { { std::lock_guard lock(mutex_); stopping_ = true; } wake_.notify_one(); if (thread_.joinable()) thread_.join(); }
    void Post(const Snapshot& snapshot) {
        auto pending = std::make_unique<Snapshot>(); RefreshTraySnapshot(*pending, snapshot);
        { std::lock_guard lock(mutex_); pending_ = std::move(pending); } wake_.notify_one();
    }
private:
    std::mutex mutex_; std::condition_variable wake_;
    bool stopping_ = false;
    std::unique_ptr<Snapshot> pending_;
    std::thread thread_;
};
}
inline bool ReadTrayPage(Snapshot& snapshot, size_t page, uint64_t expected_revision = 0, DWORD expected_pid = 0,
                        const std::wstring& name = tray_pages_detail::Name()) {
    tray_pages_detail::Manifest m; Snapshot result;
    if (tray_pages_detail::ReadManifest(m, name) != tray_pages_detail::ReadStatus::Valid ||
        (expected_revision && m.revision != expected_revision) || (expected_pid && m.pid != expected_pid) ||
        !tray_pages_detail::ReadPage(result, m, page, name)) { RefreshTraySnapshot(snapshot, {}); return false; }
    tray_pages_detail::Manifest after;
    if (tray_pages_detail::ReadManifest(after, name) != tray_pages_detail::ReadStatus::Valid || !tray_pages_detail::SameIdentity(m, after)) {
        RefreshTraySnapshot(snapshot, {}); return false;
    }
    tray_pages_detail::Metadata(result, m); RefreshTraySnapshot(snapshot, result); return true;
}
inline void PublishTray(const Snapshot& snapshot) { static tray_pages_detail::Publisher publisher; publisher.Post(snapshot); }
inline void ReadTray(Snapshot& snapshot) {
    tray_pages_detail::ReadWithLegacy(snapshot, tray_pages_detail::Name(), tray_detail::Name(), true);
}
}
