#pragma once
#include <windows.h>
#include "current_user_security.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Publish before the actual native API; CommitLaunch afterwards or Cancel on a
// definitive launch failure. Publication errors NEVER replace the native call.
// An Observer must keep this mapping alive throughout its takeover lifetime.
// Queries are tri-state: Unavailable is not proof that a window is unprotected.
// Call ForgetWindow on DESTROY (and a confirmed new CREATE) to disambiguate HWND
// reuse within the same Explorer process/thread without writable window props.
namespace pulse::native_browse_intent {
struct Request { std::wstring folder, selection; DWORD ttl_ms = 10000; };
struct Ticket { uint64_t epoch = 0, generation = 0; uint32_t slot = 0; };
struct Publication { DWORD error = ERROR_NOT_READY; Ticket ticket; };
enum class State { None, Wait, Retained, Unavailable };
struct Query { State state = State::None; DWORD error = ERROR_SUCCESS; };
namespace detail {
constexpr uint32_t kMagic = 0x49424e50, kVersion = 2;
constexpr size_t kRequests = 64, kRetained = 256, kBefore = 1024, kPath = 32768, kName = 256;
constexpr DWORD kLockMs = 100, kMaxTtlMs = 30000, kSettleMs = 1500;
#pragma pack(push, 8)
struct alignas(8) Identity { uint64_t window = 0, created = 0; uint32_t pid = 0, tid = 0; };
struct alignas(8) Slot {
    uint64_t generation = 0, started = 0, expires = 0, claimed = 0;
    uint32_t state = 0, before_count = 0, folder_length = 0, selection_length = 0;
    uint16_t folder[kPath]{}, selection[kName]{};
    Identity before[kBefore]{};
};
struct alignas(8) Retained { Identity identity; uint64_t generation = 0; };
// Observer-only sidecar: old v2 publishers still see exactly the original ABI.
// Each candidate owns a target snapshot so slot reuse cannot change its intent.
struct alignas(8) Candidate {
    Identity identity;
    uint64_t generation = 0, admission = 0, deadline = 0, seen = 0;
    uint32_t slot = 0, state = 0, folder_length = 0, selection_length = 0;
    uint32_t reason = 0, selection_mismatch = 0;
    uint16_t folder[kPath]{}, selection[kName]{};
};
struct alignas(8) Candidates {
    uint64_t epoch = 0, scope = 0;
    uint32_t version = 0, bytes = 0;
    uint64_t retained_generation[kRetained]{};
    uint32_t retained_reason[kRetained]{};
    Candidate entries[kRetained];
};
struct alignas(8) Layout {
    uint32_t magic = 0, version = 0, bytes = 0, poisoned = 0;
    uint64_t epoch = 0, next_generation = 0, scope = 0;
    uint64_t overload_started = 0, overload_until = 0;
    uint32_t overload_error = 0, overload_before_count = 0;
    Identity overload_before[kBefore]{};
    volatile LONG64 emergency_until = 0;
    volatile LONG emergency_error = 0;
    uint32_t reserved = 0;
    Slot slots[kRequests];
    Retained retained[kRetained];
};
#pragma pack(pop)
static_assert(sizeof(Candidate) == 66128 && sizeof(Candidates) == 16931864);
static_assert(sizeof(wchar_t) == 2 && sizeof(Identity) == 24 && sizeof(Retained) == 32);
static_assert(sizeof(Slot) == 90672 && offsetof(Layout, slots) == 24656 && sizeof(Layout) == 5835856);
inline bool Same(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
inline bool Component(std::wstring_view s) noexcept {
    if (s.empty() || s == L"." || s == L".." || s.size() >= kName || s.back() == L'.' || s.back() == L' ') return false;
    for (const auto c : s) if (c < 32 || c == L'"' || c == L':' || c == L'/' || c == L'\\' || c == L'<' || c == L'>' || c == L'|' || c == L'?' || c == L'*') return false;
    return true;
}
inline bool Normalize(std::wstring_view input, std::wstring& out) {
    if (input.empty() || input.size() >= kPath) return false;
    std::wstring p(input);
    for (auto& c : p) { if (c < 32 || c == L'"') return false; if (c == L'/') c = L'\\'; }
    if (p.size() >= 8 && Same(std::wstring_view(p).substr(0, 8), L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p.erase(0, 4);
    std::wstring root; size_t start = 0;
    if (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) && p[1] == L':' && p[2] == L'\\') {
        root = p.substr(0, 3); start = 3;
    } else if (p.starts_with(L"\\\\")) {
        const auto server_end = p.find(L'\\', 2);
        if (server_end == p.npos) return false;
        const auto share_end = p.find(L'\\', server_end + 1);
        const auto end = share_end == p.npos ? p.size() : share_end;
        if (!Component(std::wstring_view(p).substr(2, server_end - 2)) ||
            !Component(std::wstring_view(p).substr(server_end + 1, end - server_end - 1))) return false;
        root = p.substr(0, end); start = end;
    } else return false;
    std::vector<std::wstring_view> pieces;
    for (size_t i = start; i < p.size();) {
        if (p[i] == L'\\') { ++i; continue; }
        auto end = p.find(L'\\', i); if (end == p.npos) end = p.size();
        const auto part = std::wstring_view(p).substr(i, end - i);
        if (part == L"..") { if (pieces.empty()) return false; pieces.pop_back(); }
        else if (part != L".") { if (!Component(part)) return false; pieces.push_back(part); }
        i = end;
    }
    out = root;
    for (const auto part : pieces) { if (out.back() != L'\\') out += L'\\'; out += part; }
    return out.size() < kPath;
}
inline uint64_t Birth(HANDLE process) noexcept {
    FILETIME created{}, ended{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &ended, &kernel, &user)) return 0;
    return (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
}
inline std::wstring ObjectName(HANDLE object) {
    DWORD bytes = 0; GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) return {};
    std::wstring text(bytes / sizeof(wchar_t), 0);
    if (!GetUserObjectInformationW(object, UOI_NAME, text.data(), bytes, &bytes)) return {};
    text.resize(wcslen(text.c_str())); return text;
}
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
inline thread_local HWND deny_identity_for_test = nullptr;
inline thread_local size_t identity_reads_for_test = 0;
#endif
inline bool ReadIdentity(HWND window, const std::wstring& desktop, Identity& out) noexcept {
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
    ++identity_reads_for_test;
    if (window == deny_identity_for_test) { SetLastError(ERROR_ACCESS_DENIED); return false; }
#endif
    try {
        if (!IsWindow(window)) return false;
        DWORD pid = 0; const DWORD tid = GetWindowThreadProcessId(window, &pid);
        if (!pid || !tid || !Same(ObjectName(GetThreadDesktop(tid)), desktop)) return false;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;
        const auto created = Birth(process); CloseHandle(process);
        DWORD checked = 0;
        if (!created || GetWindowThreadProcessId(window, &checked) != tid || checked != pid) return false;
        out = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window)), created, pid, tid}; return true;
    } catch (...) { return false; }
}
inline bool SameIdentity(const Identity& a, const Identity& b) noexcept {
    return a.window == b.window && a.created == b.created && a.pid == b.pid && a.tid == b.tid;
}
inline std::wstring_view Text(const uint16_t* value, uint32_t length) noexcept {
    return {reinterpret_cast<const wchar_t*>(value), length};
}
inline uint64_t Hash(std::wstring_view value) noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const auto c : value) { hash ^= static_cast<uint16_t>(c); hash *= 1099511628211ull; }
    return hash;
}
struct Channel {
    HANDLE mapping = nullptr, mutex = nullptr, candidates_mapping = nullptr; Layout* data = nullptr;
    Candidates* candidates = nullptr;
    std::wstring desktop; uint64_t scope = 0;
    ~Channel() { if (candidates) UnmapViewOfFile(candidates); if (candidates_mapping) CloseHandle(candidates_mapping); if (data) UnmapViewOfFile(data); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
    DWORD Validate() const noexcept {
        const auto& h = *data;
        if (h.magic != kMagic || h.version != kVersion || h.bytes != sizeof(Layout) || h.poisoned || !h.epoch || h.scope != scope) return ERROR_INVALID_DATA;
        const auto emergency = InterlockedCompareExchange64(&data->emergency_until, 0, 0);
        const auto now = GetTickCount64();
        if (h.overload_before_count > kBefore || h.reserved || emergency < 0 ||
            static_cast<uint64_t>(emergency) > now + kMaxTtlMs ||
            (h.overload_until && (!h.overload_error || h.overload_until < h.overload_started ||
                h.overload_started > now || h.overload_until > now + kMaxTtlMs))) return ERROR_INVALID_DATA;
        for (const auto& s : h.slots) {
            if (!s.state) continue;
            if (s.state > 3 || !s.generation || s.generation > h.next_generation || !s.started || s.started > now ||
                (s.claimed && (s.claimed < s.started || s.claimed > now)) || s.expires < s.started || s.expires - s.started > kMaxTtlMs ||
                s.before_count > kBefore || !s.folder_length || s.folder_length >= kPath || s.selection_length >= kName ||
                s.folder[s.folder_length] || s.selection[s.selection_length]) return ERROR_INVALID_DATA;
            for (uint32_t i = 0; i < s.folder_length; ++i) if (!s.folder[i]) return ERROR_INVALID_DATA;
            for (uint32_t i = 0; i < s.selection_length; ++i) if (!s.selection[i]) return ERROR_INVALID_DATA;
        }
        for (const auto& r : h.retained) if (r.generation && (!r.identity.window || r.identity.window > UINT32_MAX || !r.identity.pid || !r.identity.tid || !r.identity.created)) return ERROR_INVALID_DATA;
        return ERROR_SUCCESS;
    }
    DWORD ValidateCandidates() const noexcept {
        if (!candidates || candidates->version != 1 || candidates->bytes != sizeof(Candidates) ||
            candidates->epoch != data->epoch || candidates->scope != scope) return ERROR_INVALID_DATA;
        for (size_t i = 0; i < kRetained; ++i)
            if (candidates->retained_reason[i] && candidates->retained_reason[i] != ERROR_TIMEOUT &&
                candidates->retained_reason[i] != ERROR_RETRY && candidates->retained_reason[i] != ERROR_NOT_ENOUGH_MEMORY) return ERROR_INVALID_DATA;
        const auto now = GetTickCount64();
        for (const auto& c : candidates->entries) {
            if (!c.state) continue;
            if (c.state > 2 || c.selection_mismatch > 1 || (c.reason && c.reason != ERROR_TIMEOUT && c.reason != ERROR_RETRY) || c.slot >= kRequests || !c.generation || c.generation > data->next_generation ||
                !c.identity.window || c.identity.window > UINT32_MAX || !c.identity.pid || !c.identity.tid || !c.identity.created ||
                !c.seen || c.seen > now || c.admission < c.seen || c.deadline < c.admission ||
                c.deadline - c.seen > kMaxTtlMs || !c.folder_length || c.folder_length >= kPath || c.selection_length >= kName ||
                c.folder[c.folder_length] || c.selection[c.selection_length]) return ERROR_INVALID_DATA;
            for (uint32_t i = 0; i < c.folder_length; ++i) if (!c.folder[i]) return ERROR_INVALID_DATA;
            for (uint32_t i = 0; i < c.selection_length; ++i) if (!c.selection[i]) return ERROR_INVALID_DATA;
        }
        return ERROR_SUCCESS;
    }
    void Emergency(DWORD error, DWORD ttl) noexcept {
        const auto deadline = static_cast<LONG64>(GetTickCount64() + (std::min)((std::max)(ttl, DWORD{1}), kMaxTtlMs));
        InterlockedExchange(&data->emergency_error, static_cast<LONG>(error));
        auto old = InterlockedCompareExchange64(&data->emergency_until, 0, 0);
        while (old < deadline) {
            const auto actual = InterlockedCompareExchange64(&data->emergency_until, deadline, old);
            if (actual == old) break;
            old = actual;
        }
    }
    void Degrade(DWORD error, DWORD ttl, const Slot* snapshot) noexcept {
        const auto now = GetTickCount64();
        if (data->overload_until <= now) {
            data->overload_started = now;
            data->overload_before_count = snapshot ? snapshot->before_count : 0;
            if (snapshot) std::copy_n(snapshot->before, snapshot->before_count, data->overload_before);
        }
        data->overload_error = error;
        data->overload_until = (std::max)(data->overload_until, now + (std::min)((std::max)(ttl, DWORD{1}), kMaxTtlMs));
    }
    void Expire() noexcept {
        const auto now = GetTickCount64();
        for (auto& s : data->slots) if (s.state && s.expires <= now) s.state = 0;
        if (data->overload_until <= now) { data->overload_until = 0; data->overload_started = 0; data->overload_error = 0; data->overload_before_count = 0; }
    }

};
struct Lock {
    Channel& channel; bool owned = false; DWORD error = ERROR_SUCCESS;
    explicit Lock(Channel& c) noexcept : channel(c) {
        const DWORD wait = WaitForSingleObject(c.mutex, kLockMs);
        owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (wait == WAIT_ABANDONED) { c.data->poisoned = 1; error = ERROR_INVALID_DATA; }
        else if (wait == WAIT_TIMEOUT) error = ERROR_TIMEOUT;
        else if (wait != WAIT_OBJECT_0) error = GetLastError();
    }
    ~Lock() { if (owned) ReleaseMutex(channel.mutex); }
};
// This passive retention ledger is intentionally writable across integrity
// levels for the same user. It can only suppress closing, never execute/open a
// request. Low-integrity writers can cause retention/availability denial of
// service; its records must not become trusted commands in another subsystem.
class LedgerSecurity {
public:
    explicit LedgerSecurity(const std::wstring& sid) {
        const auto sddl = L"D:P(A;;GA;;;" + sid + L")S:(ML;;NW;;;LW)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            attributes_.lpSecurityDescriptor = descriptor_;
    }
    ~LedgerSecurity() { if (descriptor_) LocalFree(descriptor_); }
    LedgerSecurity(const LedgerSecurity&) = delete;
    LedgerSecurity& operator=(const LedgerSecurity&) = delete;
    explicit operator bool() const noexcept { return descriptor_ != nullptr; }
    SECURITY_ATTRIBUTES* get() noexcept { return &attributes_; }
    DWORD Apply(HANDLE object) const noexcept {
        // Existing v2 objects may have inherited Medium/High labels. Creation
        // attributes do not update existing objects. Only the observer, with
        // full creator access and after protocol validation, repairs this pair.
        return SetKernelObjectSecurity(object, DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, descriptor_)
            ? ERROR_SUCCESS : GetLastError();
    }
private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};
inline DWORD Connect(std::unique_ptr<Channel>& output, bool create, std::wstring_view test = {}) noexcept {
    output.reset();
    try {
        auto c = std::make_unique<Channel>();
        const auto sid = CurrentUserSidString();
        c->desktop = ObjectName(GetThreadDesktop(GetCurrentThreadId()));
        const auto station = ObjectName(GetProcessWindowStation());
        DWORD session = 0;
        if (sid.empty() || station.empty() || c->desktop.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return ERROR_ACCESS_DENIED;
        if (test.size() > 64 || test.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != test.npos) return ERROR_INVALID_NAME;
        const auto identity = sid + L"|" + std::to_wstring(session) + L"|" + station + L"|" + c->desktop + L"|" + std::wstring(test);
        c->scope = Hash(identity);
        const auto name = L"Local\\Pulse.NativeBrowseIntent.v" + std::to_wstring(kVersion) + L"." + std::to_wstring(c->scope);
        LedgerSecurity security(sid);
        if (!security) return ERROR_ACCESS_DENIED;
        c->mutex = create ? CreateMutexW(security.get(), FALSE, (name + L".mutex").c_str()) : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".mutex").c_str());
        if (!c->mutex) return GetLastError();
        // Initialization is serialized before the view is made available.
        const DWORD wait = WaitForSingleObject(c->mutex, kLockMs);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        struct Release { HANDLE mutex; ~Release() { ReleaseMutex(mutex); } } release{c->mutex};
        bool fresh = false;
        if (create) {
            c->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(Layout)), (name + L".map").c_str());
            const DWORD error = GetLastError(); fresh = c->mapping && error != ERROR_ALREADY_EXISTS;
        } else c->mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, (name + L".map").c_str());
        if (!c->mapping) return GetLastError();
        c->data = static_cast<Layout*>(MapViewOfFile(c->mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Layout)));
        if (!c->data) return GetLastError();
        if (fresh) {
            ZeroMemory(c->data, sizeof(Layout));
            LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter);
            c->data->epoch = (static_cast<uint64_t>(counter.QuadPart) ^ Birth(GetCurrentProcess())) | 1;
            c->data->scope = c->scope; c->data->bytes = sizeof(Layout); c->data->version = kVersion; c->data->magic = kMagic;
        } else if (wait == WAIT_ABANDONED) c->data->poisoned = 1;
        const DWORD error = c->Validate(); if (error) return error;
        if (create) {
            const auto mutex_security = security.Apply(c->mutex); if (mutex_security) return mutex_security;
            const auto mapping_security = security.Apply(c->mapping); if (mapping_security) return mapping_security;
            c->candidates_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0,
                static_cast<DWORD>(sizeof(Candidates)), (name + L".candidates.v1").c_str());
            const auto side_error = GetLastError();
            if (!c->candidates_mapping) return side_error;
            c->candidates = static_cast<Candidates*>(MapViewOfFile(c->candidates_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Candidates)));
            if (!c->candidates) return GetLastError();
            if (side_error != ERROR_ALREADY_EXISTS) {
                // All observers disappeared while an old publisher held the v2
                // mapping. Its provisional records cannot be reconstructed.
                for (const auto& slot : c->data->slots)
                    if ((slot.state == 1 || slot.state == 2) && slot.claimed) return ERROR_INVALID_DATA;
                ZeroMemory(c->candidates, sizeof(Candidates));
                c->candidates->epoch = c->data->epoch; c->candidates->scope = c->scope;
                c->candidates->version = 1; c->candidates->bytes = sizeof(Candidates);
            }
            const auto side_valid = c->ValidateCandidates(); if (side_valid) return side_valid;
            const auto side_security = security.Apply(c->candidates_mapping); if (side_security) return side_security;
        }
        output = std::move(c); return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
inline bool Before(const Slot& s, const Identity& identity) noexcept {
    for (uint32_t i = 0; i < s.before_count; ++i) if (SameIdentity(s.before[i], identity)) return true;
    return false;
}
inline bool RetainedIdentity(const Channel& c, const Identity& identity) noexcept {
    for (const auto& r : c.data->retained) if (r.generation && SameIdentity(r.identity, identity)) return true;
    return false;
}
}

class Publisher {
public:
    DWORD Open() noexcept { return detail::Connect(channel_, false); }
    void Close() noexcept { channel_.reset(); }
    Publication Publish(const Request& request) noexcept {
        if (!channel_) return {ERROR_NOT_READY, {}};
        try {
            std::wstring folder;
            if (!request.ttl_ms || request.ttl_ms > detail::kMaxTtlMs || !detail::Normalize(request.folder, folder) ||
                (!request.selection.empty() && !detail::Component(request.selection))) return {ERROR_INVALID_NAME, {}};
            auto staged = std::make_unique<detail::Slot>();
            staged->folder_length = static_cast<uint32_t>(folder.size()); staged->selection_length = static_cast<uint32_t>(request.selection.size());
            std::memcpy(staged->folder, folder.c_str(), (folder.size() + 1) * sizeof(wchar_t));
            std::memcpy(staged->selection, request.selection.c_str(), (request.selection.size() + 1) * sizeof(wchar_t));
            struct Enumeration { detail::Slot& slot; const std::wstring& desktop; DWORD error = 0; } enumeration{*staged, channel_->desktop};
            SetLastError(ERROR_SUCCESS);
            const BOOL enumerated = EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND window, LPARAM context) -> BOOL {
                auto& e = *reinterpret_cast<Enumeration*>(context);
                if (e.slot.before_count == detail::kBefore) { e.error = ERROR_BUFFER_OVERFLOW; return FALSE; }
                detail::Identity identity;
                // Conservatively record even inaccessible existing handles; later
                // matching excludes them by HWND, rather than treating them as new.
                if (!detail::ReadIdentity(window, e.desktop, identity)) identity.window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window));
                e.slot.before[e.slot.before_count++] = identity; return TRUE;
            }, reinterpret_cast<LPARAM>(&enumeration));
            // Win32 returns FALSE/ERROR_SUCCESS for a genuinely empty desktop.
            // No callback ran and no failure was reported: that is an empty,
            // complete prelaunch snapshot, not an inability to publish.
            if (!enumerated && (enumeration.error || staged->before_count || GetLastError()))
                return Failure(enumeration.error ? enumeration.error : (GetLastError() ? GetLastError() : ERROR_GEN_FAILURE), request.ttl_ms);
            detail::Lock lock(*channel_); if (lock.error) { channel_->Emergency(lock.error, request.ttl_ms); return {lock.error, {}}; }
            const auto error = channel_->Validate(); if (error) return {error, {}};
            channel_->Expire();
            for (uint32_t i = 0; i < detail::kRequests; ++i) {
                auto& slot = channel_->data->slots[i]; if (slot.state == 1 || slot.state == 2) continue;
                if (channel_->data->next_generation == UINT64_MAX) return {ERROR_ARITHMETIC_OVERFLOW, {}};
                staged->generation = ++channel_->data->next_generation;
                staged->started = GetTickCount64(); staged->expires = staged->started + request.ttl_ms; staged->state = 1;
                slot = *staged;
                return {ERROR_SUCCESS, {channel_->data->epoch, staged->generation, i}};
            }
            channel_->Degrade(ERROR_BUSY, request.ttl_ms, staged.get());
            return {ERROR_BUSY, {}};
        } catch (...) { return Failure(ERROR_NOT_ENOUGH_MEMORY, request.ttl_ms); }
    }
    DWORD CommitLaunch(const Ticket& ticket) noexcept { return Update(ticket, false); }
    DWORD Cancel(const Ticket& ticket) noexcept { return Update(ticket, true); }
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
    DWORD OpenForTest(std::wstring_view name) noexcept { return name.empty() ? ERROR_INVALID_NAME : detail::Connect(channel_, false, name); }
#endif
private:
    Publication Failure(DWORD error, DWORD ttl) noexcept {
        // Best effort only when the existing channel can still be locked and
        // validated. A disconnected/poisoned IPC channel remains an explicit
        // error: callers must still perform their native operation once.
        if (channel_) {
            detail::Lock lock(*channel_);
            if (!lock.error && !channel_->Validate()) channel_->Degrade(error, ttl, nullptr);
            else if (lock.error) channel_->Emergency(error, ttl);
        }
        return {error, {}};
    }
    DWORD Update(const Ticket& ticket, bool cancel) noexcept {
        if (!channel_) return ERROR_NOT_READY;
        detail::Lock lock(*channel_); if (lock.error) return lock.error;
        const auto error = channel_->Validate(); if (error) return error;
        channel_->Expire();
        if (ticket.epoch != channel_->data->epoch || ticket.slot >= detail::kRequests) return ERROR_INVALID_HANDLE;
        auto& slot = channel_->data->slots[ticket.slot];
        if (!slot.state || !ticket.generation || slot.generation != ticket.generation) {
            for (const auto& retained : channel_->data->retained)
                if (retained.generation == ticket.generation && ticket.generation) return ERROR_SUCCESS;
            return ERROR_NOT_FOUND;
        }
        if (cancel) slot.state = 0;
        else if (slot.state != 3) slot.state = 2;
        return ERROR_SUCCESS;
    }
    std::unique_ptr<detail::Channel> channel_;
};

class Observer {
public:
    DWORD Start() noexcept { Stop(); return detail::Connect(channel_, true); }
    ~Observer() { Stop(); }
    void Stop() noexcept {
        // Graceful observer replacement must not lose already observed HWNDs.
        if (channel_ && channel_->candidates) {
            detail::Lock lock(*channel_);
            if (!lock.error && !channel_->Validate() && !channel_->ValidateCandidates()) {
                const auto now = GetTickCount64();
                for (auto& c : channel_->candidates->entries) if (c.state == 1) {
                    const auto& s = channel_->data->slots[c.slot];
                    if (((s.generation == c.generation && (s.state == 1 || s.state == 2)) || now >= c.deadline) && !c.selection_mismatch) {
                        const auto saved = StoreRetained(c.identity, c.generation, ERROR_RETRY);
                        // With the mutex held, repeatedly scanning the same full
                        // live table cannot create capacity. Leave unresolved
                        // candidates claimed so a lost sidecar fails explicitly.
                        if (saved == ERROR_NOT_ENOUGH_MEMORY) break;
                        if (!saved) c.reason = ERROR_RETRY;
                    }
                }
                for (auto& slot : channel_->data->slots) if ((slot.state == 1 || slot.state == 2) && slot.claimed) {
                    bool protected_all = true; uint64_t admission = slot.expires;
                    for (const auto& c : channel_->candidates->entries) if (c.state && c.generation == slot.generation) {
                        admission = c.admission;
                        if (c.state == 1 && !c.selection_mismatch && !detail::RetainedIdentity(*channel_, c.identity)) protected_all = false;
                    }
                    // Do not consume all same-target tickets merely because one
                    // ambiguous HWND was seen. Persist that HWND, then restore
                    // the original admission for future windows after restart.
                    if (protected_all) { slot.expires = admission; slot.claimed = 0; }
                }
                // A peer observer may still use the shared candidate records.
            }
        }
        channel_.reset();
    }
    Query IsRetained(HWND window) noexcept { return Inspect(window, {}, {}, false, false, true); }
    Query ShouldWait(HWND window, std::wstring_view folder, bool selection_ready) noexcept {
        return Inspect(window, folder, {}, selection_ready, false, false);
    }
    Query MatchAndRetain(HWND window, std::wstring_view folder, const std::vector<std::wstring>& names, bool selection_ready) noexcept {
        return Inspect(window, folder, names, selection_ready, true, false);
    }
    DWORD ForgetWindow(HWND window) noexcept {
        if (!channel_) return ERROR_NOT_READY;
        detail::Lock lock(*channel_); if (lock.error) return lock.error;
        const auto error = channel_->Validate(); if (error) return error;
        const auto value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window));
        for (auto& r : channel_->data->retained) if (r.identity.window == value) r.generation = 0;
        if (channel_->candidates) for (auto& c : channel_->candidates->entries) if (c.identity.window == value) c.state = 2;
        for (uint32_t i = 0; i < channel_->data->overload_before_count;)
            if (channel_->data->overload_before[i].window == value)
                channel_->data->overload_before[i] = channel_->data->overload_before[--channel_->data->overload_before_count];
            else ++i;
        for (auto& s : channel_->data->slots) for (uint32_t i = 0; i < s.before_count;)
            if (s.before[i].window == value) s.before[i] = s.before[--s.before_count]; else ++i;
        return ERROR_SUCCESS;
    }
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
    DWORD StartForTest(std::wstring_view name) noexcept { Stop(); return name.empty() ? ERROR_INVALID_NAME : detail::Connect(channel_, true, name); }
    // Test access is explicit and absent from production builds, never an env switch.
    detail::Channel* ChannelForTest() noexcept { return channel_.get(); }
    void CandidateLeaseForTest(DWORD ms) noexcept { candidate_lease_ms_ = ms; }
    void RetainedCapacityForTest(size_t capacity) noexcept { retained_capacity_ = (std::min)(capacity, detail::kRetained); }
#endif
private:
    Query Inspect(HWND window, std::wstring_view folder, const std::vector<std::wstring>& names,
                  bool selection_ready, bool retain, bool retained_only) noexcept {
        if (!channel_) return {State::Unavailable, ERROR_NOT_READY};
        try {
            detail::Identity identity;
            if (!detail::ReadIdentity(window, channel_->desktop, identity)) {
                if (!IsWindow(window)) return {};
                const auto desktop = detail::ObjectName(GetThreadDesktop(GetWindowThreadProcessId(window, nullptr)));
                if (!desktop.empty() && !detail::Same(desktop, channel_->desktop)) return {};
                return {State::Unavailable, ERROR_ACCESS_DENIED};
            }
            std::wstring normalized;
            const bool have_folder = detail::Normalize(folder, normalized);
            detail::Lock lock(*channel_); if (lock.error) return {State::Unavailable, lock.error};
            const auto error = channel_->Validate(); if (error) return {State::Unavailable, error};
            const auto side_error = channel_->ValidateCandidates(); if (side_error) return {State::Unavailable, side_error};
            if (detail::RetainedIdentity(*channel_, identity)) {
                for (size_t i = 0; i < detail::kRetained; ++i) {
                    const auto& r = channel_->data->retained[i];
                    if (r.generation && detail::SameIdentity(r.identity, identity) &&
                        channel_->candidates->retained_generation[i] == r.generation)
                        return {State::Retained, channel_->candidates->retained_reason[i]};
                }
                return {State::Retained, ERROR_SUCCESS};
            }
            const auto now = GetTickCount64();
            bool candidate_wait = false;
            // Process observed candidates BEFORE Expire. Original v2 Cancel and
            // Expire both write state=0. Before the pinned deadline that is a
            // cancellation; afterwards it is ambiguous and conservatively only
            // this recorded identity is retained, never a fresh HWND.
            for (auto& c : channel_->candidates->entries) {
                if (c.state != 1 || !detail::SameIdentity(c.identity, identity)) continue;
                const auto& slot = channel_->data->slots[c.slot];
                if ((slot.generation != c.generation || !slot.state) && now < c.deadline) { c.state = 2; continue; }
                if (slot.generation == c.generation && slot.state == 3) { c.state = 2; continue; }
                const bool folder_matches = have_folder && detail::Same(detail::Text(c.folder, c.folder_length), normalized);
                if (have_folder && !folder_matches && now - c.seen >= detail::kSettleMs) { c.state = 2; continue; }
                const bool matches = folder_matches && selection_ready && (!c.selection_length ||
                    (names.size() == 1 && detail::Component(names[0]) && detail::Same(detail::Text(c.selection, c.selection_length), names[0])));
                if (retain && folder_matches) c.selection_mismatch = selection_ready && !matches ? 1u : 0u;
                if (now >= c.deadline && c.selection_mismatch) { c.state = 2; continue; }
                const bool precise = retain && matches;
                if (precise || now >= c.deadline) {
                    const auto saved = StoreRetained(identity, c.generation, precise ? DWORD{ERROR_SUCCESS} : DWORD{ERROR_TIMEOUT});
                    if (saved) return {State::Unavailable, saved};
                    // A timeout preserves only this uncertain identity; it is
                    // not evidence that the shared request was fulfilled. Other
                    // admitted candidates may be the real delayed native HWND.
                    if (precise && slot.generation == c.generation) {
                        auto& bound = channel_->data->slots[c.slot]; bound.state = 3; bound.claimed = now;
                    }
                    c.state = 2; c.reason = precise ? DWORD{ERROR_SUCCESS} : DWORD{ERROR_TIMEOUT};
                    return {State::Retained, c.reason};
                }
                candidate_wait = true;
            }
            // Rejected candidates must not pin an otherwise unused request.
            for (auto& slot : channel_->data->slots) if ((slot.state == 1 || slot.state == 2) && slot.claimed) {
                bool active = false; uint64_t admission = slot.expires;
                for (const auto& c : channel_->candidates->entries) if (c.state && c.generation == slot.generation) {
                    admission = c.admission; active |= c.state == 1;
                }
                if (!active) { slot.expires = admission; slot.claimed = 0; }
            }
            channel_->Expire();
            const auto emergency = InterlockedCompareExchange64(&channel_->data->emergency_until, 0, 0);
            if (emergency > static_cast<LONG64>(now)) {
                const auto reason = static_cast<DWORD>(InterlockedCompareExchange(&channel_->data->emergency_error, 0, 0));
                return {State::Unavailable, reason ? reason : ERROR_INVALID_DATA};
            }
            if (channel_->data->overload_until > now) {
                bool existed = false;
                for (uint32_t i = 0; i < channel_->data->overload_before_count; ++i) {
                    const auto& old = channel_->data->overload_before[i];
                    if (detail::SameIdentity(old, identity) || (!old.created && old.window == identity.window)) { existed = true; break; }
                }
                if (!existed) return {State::Unavailable, channel_->data->overload_error};
            }
            if (retained_only) return {};
            bool wait = candidate_wait;
            for (uint32_t index = 0; index < detail::kRequests; ++index) {
                auto& slot = channel_->data->slots[index];
                if (slot.state != 1 && slot.state != 2) continue;
                bool before = detail::Before(slot, identity);
                for (uint32_t i = 0; !before && i < slot.before_count; ++i)
                    before = !slot.before[i].created && slot.before[i].window == identity.window;
                if (before) continue;
                uint64_t admission = slot.expires;
                bool recorded = false, seen = false;
                for (const auto& c : channel_->candidates->entries) if (c.state && c.generation == slot.generation) {
                    admission = c.admission; recorded = true;
                    seen |= detail::SameIdentity(c.identity, identity);
                }
                if (seen || now >= admission) continue;
                if (slot.claimed && !recorded) return {State::Unavailable, ERROR_INVALID_DATA};
                const bool matches = have_folder && detail::Same(detail::Text(slot.folder, slot.folder_length), normalized);
                // A late SHOW with an unreadable initial Home view is still a
                // candidate throughout original admission, not merely 1.5s
                // after Publish. A known different folder keeps bounded settle.
                if (have_folder && !matches && now - slot.started >= detail::kSettleMs) continue;
                detail::Candidate* free = nullptr;
                for (auto& c : channel_->candidates->entries) {
                    const auto& previous = channel_->data->slots[c.slot < detail::kRequests ? c.slot : 0];
                    if (!c.state || (c.state == 2 && (now >= c.deadline || previous.generation != c.generation))) { free = &c; break; }
                    detail::Identity current;
                    const auto old = reinterpret_cast<HWND>(static_cast<uintptr_t>(c.identity.window));
                    if (detail::ReadIdentity(old, channel_->desktop, current) ? !detail::SameIdentity(current, c.identity) : !IsWindow(old)) { free = &c; break; }
                }
                if (!free) {
                    // Bounded candidate capacity is not permission to close an
                    // untracked fallback later. Preserve this exact HWND now.
                    const auto saved = StoreRetained(identity, slot.generation, ERROR_NOT_ENOUGH_MEMORY);
                    return {saved ? State::Unavailable : State::Retained, ERROR_NOT_ENOUGH_MEMORY};
                }
                DWORD lease = detail::kMaxTtlMs;
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
                lease = candidate_lease_ms_;
#endif
                const auto deadline = (std::max)(admission, (std::min)(slot.started + detail::kMaxTtlMs, now + lease));
                free->identity = identity; free->generation = slot.generation; free->slot = index;
                free->admission = admission; free->deadline = deadline; free->seen = now;
                free->folder_length = slot.folder_length; free->selection_length = slot.selection_length;
                std::copy_n(slot.folder, slot.folder_length + 1, free->folder);
                std::copy_n(slot.selection, slot.selection_length + 1, free->selection);
                free->state = 1; free->reason = 0;
                free->selection_mismatch = retain && selection_ready && matches && slot.selection_length &&
                    (names.size() != 1 || !detail::Component(names[0]) || !detail::Same(detail::Text(slot.selection, slot.selection_length), names[0])) ? 1u : 0u;
                slot.expires = (std::max)(slot.expires, deadline); slot.claimed = now;
                if (retain && selection_ready && matches && (!slot.selection_length ||
                    (names.size() == 1 && detail::Component(names[0]) && detail::Same(detail::Text(slot.selection, slot.selection_length), names[0])))) {
                    const auto saved = StoreRetained(identity, slot.generation);
                    if (saved) return {State::Unavailable, saved};
                    slot.state = 3; free->state = 2;
                    return {State::Retained, ERROR_SUCCESS};
                }
                wait = true;
            }
            return {wait ? State::Wait : State::None, ERROR_SUCCESS};
        } catch (...) { return {State::Unavailable, ERROR_NOT_ENOUGH_MEMORY}; }
    }
    DWORD StoreRetained(const detail::Identity& identity, uint64_t generation, DWORD reason = ERROR_SUCCESS) noexcept {
        const auto retire_candidates = [&]() noexcept {
            // The retained identity now owns its lifetime. Other requests that
            // observed the same HWND keep their tickets, but no longer need a
            // pending candidate that future retained-query early returns skip.
            for (auto& c : channel_->candidates->entries)
                if (c.state == 1 && detail::SameIdentity(c.identity, identity)) c.state = 2;
        };
        if (detail::RetainedIdentity(*channel_, identity)) { retire_candidates(); return ERROR_SUCCESS; }
        size_t capacity = detail::kRetained;
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
        capacity = retained_capacity_;
#endif
        for (size_t i = 0; i < capacity; ++i) {
            auto& r = channel_->data->retained[i];
            if (r.generation) {
                detail::Identity current;
                const auto existing = reinterpret_cast<HWND>(static_cast<uintptr_t>(r.identity.window));
                if (detail::ReadIdentity(existing, channel_->desktop, current)) {
                    if (detail::SameIdentity(current, r.identity)) continue;
                } else if (IsWindow(existing)) continue;
            }
            r = {identity, generation};
            channel_->candidates->retained_generation[i] = generation; channel_->candidates->retained_reason[i] = reason;
            retire_candidates();
            return ERROR_SUCCESS;
        }
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    std::unique_ptr<detail::Channel> channel_;
#ifdef PULSE_NATIVE_BROWSE_INTENT_TEST
    size_t retained_capacity_ = detail::kRetained;
    DWORD candidate_lease_ms_ = detail::kMaxTtlMs;
#endif
};
}
