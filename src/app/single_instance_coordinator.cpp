#include "single_instance_coordinator.h"
#include "open_request_pipe.h"
#include "explorer_submission_port.h"

#include <limits>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <objbase.h>
#include "../common/current_user_security.h"
#include "default_file_manager.h"

namespace pulse::app {
namespace {

constexpr wchar_t kMutexName[] = L"Local\\Pulse.Singleton";
constexpr wchar_t kWindowClass[] = L"PulseMainWindow";
constexpr wchar_t kPrimaryProperty[] = L"Pulse.Singleton.PrimaryPid";
constexpr ULONG_PTR kOpenPathMessage = 0x50554C53; // 'PULS'
constexpr ULONG_PTR kOpenRequestMessage = 0x50554C32; // 'PUL2'
constexpr size_t kMaxForwardedPathChars = 32768;
struct OpenRequestHeader {
    uint32_t version = 2;
    uint32_t chars = 0;
    uint64_t deadline = 0;
    std::array<unsigned char, 16> id{};
};
static_assert(sizeof(OpenRequestHeader) == 32);
struct EndpointRecord {
    volatile LONG published = 0;
    DWORD version = 1;
    DWORD pid = 0;
    FILETIME created{};
    uint64_t window = 0;
    wchar_t nonce[40]{};
};
constexpr DWORD kRawV2Magic = 0x32524550; // PER2
constexpr DWORD kRawV2Capability = 1;
struct RawV2EndpointRecord {
    volatile LONG published = 0;
    DWORD magic = kRawV2Magic, version = 2, size = 128;
    DWORD capabilities = kRawV2Capability, reserved = 0, pid = 0, reserved2 = 0;
    FILETIME created{};
    uint64_t window = 0;
    wchar_t nonce[40]{};
};
static_assert(sizeof(RawV2EndpointRecord) == 128 && offsetof(RawV2EndpointRecord, nonce) == 48);
std::wstring CapabilityName(const std::wstring& base, const std::wstring& nonce) {
    return base + L".Endpoint.RawV2." + nonce;
}
std::wstring EndpointName(std::wstring_view requested) {
    if (!requested.empty()) return std::wstring(requested);
    DWORD session = 0;
    const auto sid = CurrentUserSidString();
    if (sid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    return std::wstring(kMutexName) + L"-" + sid + L"-" + std::to_wstring(session);
}
bool Created(HANDLE process, FILETIME& value) {
    FILETIME exit{}, kernel{}, user{};
    return GetProcessTimes(process, &value, &exit, &kernel, &user) != FALSE;
}

auto RequestKey(const SingleInstanceCoordinator::OpenRequest& request) {
    return std::make_pair(bool(request.explorer), request.id);
}
bool QueueLive(const SingleInstanceCoordinator::OpenRequest& request, uint64_t now) {
    if (request.explorer) return request.explorer->handoff &&
        now < request.explorer->handoff->deadline_tick && request.explorer->handoff->state == HandoffState::Pending;
    return now < (request.execution_started ? request.ui_wait_deadline : request.deadline);
}
void ExpireExplorer(const SingleInstanceCoordinator::OpenRequest& request, uint64_t now) {
    if (request.explorer && request.explorer->handoff && now >= request.explorer->handoff->deadline_tick)
        request.explorer->handoff->Cancel(HandoffState::Expired);
}

} // namespace

std::shared_ptr<const ExplorerOpenContext> MakeExplorerOpenContext(const std::wstring& folder,
    const std::vector<std::wstring>& names, std::shared_ptr<ExplorerHandoff> handoff, bool selection_read) noexcept {
    try {
        if (!handoff || !selection_read) return {};
        auto context = std::make_shared<ExplorerOpenContext>();
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return {};
        std::memcpy(context->id.data(), &id, sizeof(id));
        context->folder = folder; context->names = names; context->selection_read = selection_read;
        context->handoff = handoff;
        return context;
    } catch (...) { return {}; }
}
bool ExplorerSubmissionPort::Submit(std::shared_ptr<const ExplorerOpenContext> context) noexcept {
    if (!context || !context->handoff) return false;
    try {
        std::lock_guard lock(mutex_);
        if (owner_) {
            SingleInstanceCoordinator::OpenRequest request;
            request.id = context->id; request.path = context->folder;
            request.deadline = context->handoff->deadline_tick; request.explorer = context;
            const auto result = owner_->SubmitOpenRequestImpl(std::move(request), GetTickCount64(), host_, wake_, &PostMessageW, true);
            if (result == SingleInstanceCoordinator::OpenSubmission::Accepted ||
                result == SingleInstanceCoordinator::OpenSubmission::Duplicate) return true;
        }
    } catch (...) {}
    // Rejection does not transfer ownership. The producer cancels a new source;
    // a retry may refer to an already dispatched lease which this port cannot revoke.
    return false;
}
void ExplorerSubmissionPort::Revoke() noexcept {
    std::lock_guard lock(mutex_); owner_ = nullptr;
}
std::shared_ptr<ExplorerSubmissionPort> SingleInstanceCoordinator::CreateExplorerSubmissionPort(HWND host, UINT wake) {
    // Creation/Release belong to the coordinator owner thread. Old workers retain
    // only their revoked port; a later Watcher receives a fresh port.
    if (explorer_port_) explorer_port_->Revoke();
    explorer_port_.reset();
    std::lock_guard lock(requests_mutex_);
    DWORD pid = 0;
    if (!accepting_requests_ || !wake || !IsWindow(host) ||
        !GetWindowThreadProcessId(host, &pid) || pid != GetCurrentProcessId()) return {};
    try { explorer_port_ = std::shared_ptr<ExplorerSubmissionPort>(new ExplorerSubmissionPort(this, host, wake)); }
    catch (...) { return {}; }
    return explorer_port_;
}

SingleInstanceCoordinator::SingleInstanceCoordinator() = default;

SingleInstanceCoordinator::~SingleInstanceCoordinator() {
    Release();
}

SingleInstanceCoordinator::AcquireResult SingleInstanceCoordinator::Acquire(
    std::wstring_view mutex_name) {
    if (mutex_) return AcquireResult::Primary;
    endpoint_name_ = EndpointName(mutex_name);
    if (endpoint_name_.empty()) return AcquireResult::Failed;
    CurrentUserSecurityAttributes security;
    if (!security) return AcquireResult::Failed;
    SetLastError(ERROR_SUCCESS);
    // Keep the legacy singleton lock across upgrades. An older owner has no
    // authenticated endpoint, so forwarding fails instead of starting a peer.
    const std::wstring mutex_object = mutex_name.empty() ? kMutexName : std::wstring(mutex_name);
    HANDLE mutex = CreateMutexW(security.get(), TRUE, mutex_object.c_str());
    if (!mutex) return AcquireResult::Failed;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return AcquireResult::Existing;
    }
    mutex_ = mutex;
    { std::lock_guard lock(requests_mutex_); accepting_requests_ = true; }
    return AcquireResult::Primary;
}

void SingleInstanceCoordinator::Release() {
    if (explorer_port_) explorer_port_->Revoke();
    explorer_port_.reset();
    {
        std::lock_guard lock(requests_mutex_);
        accepting_requests_ = false;
        if (endpoint_view_) InterlockedExchange(&static_cast<EndpointRecord*>(endpoint_view_)->published, 0);
        if (capability_view_) InterlockedExchange(&static_cast<RawV2EndpointRecord*>(capability_view_)->published, 0);
    }
    // An in-flight handler needs requests_mutex_. Never join under that lock.
    if (pipe_service_) pipe_service_->Stop();
    pipe_service_.reset();
    std::lock_guard lock(requests_mutex_);
    for (const auto& request : open_queue_) CompleteDelivery(request, false);
    open_queue_.clear(); accepted_.clear();
    if (open_claim_) {
        auto expected = OpenDeliveryState::Queued;
        if (open_claim_->compare_exchange_strong(expected, OpenDeliveryState::Cancelled) && open_claim_explorer_)
            open_claim_explorer_->handoff->Cancel();
    }
    open_claim_explorer_.reset(); open_claim_.reset(); open_failures_ = {};
    if (capability_view_) { UnmapViewOfFile(capability_view_); capability_view_ = nullptr; }
    if (capability_mapping_) { CloseHandle(capability_mapping_); capability_mapping_ = nullptr; }
    if (endpoint_view_) { UnmapViewOfFile(endpoint_view_); endpoint_view_ = nullptr; }
    if (endpoint_window_ && !endpoint_nonce_.empty()) RemovePropW(endpoint_window_, endpoint_nonce_.c_str());
    endpoint_window_ = nullptr;
    if (endpoint_mapping_) { CloseHandle(endpoint_mapping_); endpoint_mapping_ = nullptr; }
    if (!mutex_) return;
    EnumWindows([](HWND window, LPARAM) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId()) RemovePropW(window, kPrimaryProperty);
        return TRUE;
    }, 0);
    ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr;
}

bool SingleInstanceCoordinator::NormalizeLaunchPath(const std::wstring& input, std::wstring& output) {
    output.clear();
    if (input.find(L'\0') != std::wstring::npos || input.size() >= 32768) return false;
    if (input.empty() || input.starts_with(L"pulse:") || IsThisPcArgument(input)) {
        output = input; return true;
    }
    // The Recycle Bin verb (设为默认文件管理器) opens Pulse's own view.
    if (IsRecycleBinArgument(input)) { output = L"pulse:recycle"; return true; }
    auto path = input;
    // The Drive verb passes "%1" = "C:\", and the C runtime reads its \" as
    // an escaped quote: the argument arrives as C:". Kept, GetFullPathName
    // resolves it against that drive's current directory, so double-clicking
    // C: in File Explorer opened C:\Windows\System32 (the verb's working
    // directory). Quotes are never part of a path; drop them first.
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
    if (path.empty()) { output.clear(); return true; }
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    wchar_t absolute[32768]{};
    const DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(absolute), absolute, nullptr);
    if (!n || n >= ARRAYSIZE(absolute)) return false;
    output.assign(absolute, n);
    return true;
}

bool SingleInstanceCoordinator::PublishEndpoint(HWND window, UINT wake_message) {
    try {
    if (!mutex_ || endpoint_view_ || !window) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) return false;
    CurrentUserSecurityAttributes security;
    if (!security) return false;
    const auto name = endpoint_name_ + L".Endpoint";
    endpoint_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE,
        0, sizeof(EndpointRecord), name.c_str());
    if (!endpoint_mapping_) return false;
    endpoint_view_ = MapViewOfFile(endpoint_mapping_, FILE_MAP_WRITE, 0, 0, sizeof(EndpointRecord));
    if (!endpoint_view_) { CloseHandle(endpoint_mapping_); endpoint_mapping_ = nullptr; return false; }
    auto* record = static_cast<EndpointRecord*>(endpoint_view_);
    InterlockedExchange(&record->published, 0);
    GUID id{};
    wchar_t nonce[40]{};
    FILETIME created{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, nonce, ARRAYSIZE(nonce)) ||
        !Created(GetCurrentProcess(), created) || !SetPropW(window, nonce, reinterpret_cast<HANDLE>(1))) {
        UnmapViewOfFile(endpoint_view_); endpoint_view_ = nullptr;
        CloseHandle(endpoint_mapping_); endpoint_mapping_ = nullptr; return false;
    }
    endpoint_window_ = window; endpoint_nonce_ = nonce;
    record->version = 1; record->pid = pid; record->created = created;
    record->window = reinterpret_cast<uintptr_t>(window);
    wcscpy_s(record->nonce, nonce);
    if (wake_message) {
        const auto capability_name = CapabilityName(endpoint_name_, nonce);
        capability_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0, sizeof(RawV2EndpointRecord), capability_name.c_str());
        if (!capability_mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) { Release(); return false; }
        capability_view_ = MapViewOfFile(capability_mapping_, FILE_MAP_WRITE, 0, 0, sizeof(RawV2EndpointRecord));
        if (!capability_view_) { Release(); return false; }
        auto* raw = static_cast<RawV2EndpointRecord*>(capability_view_);
        RawV2EndpointRecord value{}; value.pid = pid; value.created = created;
        value.window = record->window; wcscpy_s(value.nonce, nonce);
        std::memcpy(raw, &value, sizeof(value));
        pipe_service_ = std::make_unique<OpenRequestPipe>();
        if (!pipe_service_->Start(OpenRequestPipe::Name(pid, nonce), [this, window, wake_message](std::span<const unsigned char> bytes) {
            COPYDATASTRUCT data{OpenRequestMessageId(), static_cast<DWORD>(bytes.size()), const_cast<unsigned char*>(bytes.data())};
            OpenRequest request;
            if (!DecodeOpenRequest(&data, request)) return OpenRequestPipe::Reply::Invalid;
            switch (SubmitOpenRequest(std::move(request), GetTickCount64(), window, wake_message)) {
            case OpenSubmission::Accepted: return OpenRequestPipe::Reply::Accepted;
            case OpenSubmission::Duplicate: return OpenRequestPipe::Reply::Duplicate;
            case OpenSubmission::Retry: return OpenRequestPipe::Reply::Retry;
            default: return OpenRequestPipe::Reply::Invalid;
            }
        })) { Release(); return false; }
        InterlockedExchange(&raw->published, 1);
    }
    InterlockedExchange(&record->published, 1);
    return true;
    } catch (...) { Release(); return false; }
}
bool SingleInstanceCoordinator::OwnsEndpoint(HWND window) const {
    return mutex_ && endpoint_view_ && endpoint_window_ == window &&
        GetPropW(window, endpoint_nonce_.c_str()) == reinterpret_cast<HANDLE>(1);
}
static std::optional<SingleInstanceCoordinator::Endpoint> ReadPrimary(std::wstring_view endpoint_name) {
    const auto base = EndpointName(endpoint_name);
    if (base.empty()) return std::nullopt;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, (base + L".Endpoint").c_str());
    if (!mapping) return std::nullopt;
    auto* record = static_cast<const EndpointRecord*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(EndpointRecord)));
    EndpointRecord copy{};
    bool valid = record && record->published == 1;
    if (valid) { MemoryBarrier(); std::memcpy(&copy, record, sizeof(copy)); MemoryBarrier();
        valid = record->published == 1 && copy.version == 1 && copy.nonce[39] == 0; }
    if (record) UnmapViewOfFile(record);
    CloseHandle(mapping);
    if (!valid) return std::nullopt;
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(copy.window));
    DWORD pid = 0, session = 0, own_session = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid != copy.pid || !ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || session != own_session ||
        GetPropW(window, copy.nonce) != reinterpret_cast<HANDLE>(1)) return std::nullopt;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return std::nullopt;
    FILETIME created{};
    valid = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && Created(process, created) &&
        CompareFileTime(&created, &copy.created) == 0 && ProcessUserSidString(process) == CurrentUserSidString();
    CloseHandle(process);
    if (!valid) return std::nullopt;
    return SingleInstanceCoordinator::Endpoint{window, pid, copy.created, copy.nonce, {}};
}

HWND SingleInstanceCoordinator::FindPrimaryWindow(std::wstring_view endpoint_name) {
    try { const auto endpoint = ReadPrimary(endpoint_name); return endpoint ? endpoint->window : nullptr; }
    catch (...) { return nullptr; }
}
std::optional<SingleInstanceCoordinator::Endpoint> SingleInstanceCoordinator::FindPrimaryEndpoint(std::wstring_view endpoint_name) {
    try {
        auto endpoint = ReadPrimary(endpoint_name);
        if (!endpoint) return std::nullopt;
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, CapabilityName(EndpointName(endpoint_name), endpoint->nonce).c_str());
        if (!mapping) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return std::nullopt;
            const auto again = ReadPrimary(endpoint_name);
            if (!again || again->window != endpoint->window || again->pid != endpoint->pid || again->nonce != endpoint->nonce ||
                CompareFileTime(&again->created, &endpoint->created)) return std::nullopt;
            return endpoint; // Explicitly absent capability, verified legacy endpoint.
        }
        const auto* view = static_cast<const RawV2EndpointRecord*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(RawV2EndpointRecord)));
        RawV2EndpointRecord copy{};
        bool valid = view && view->published == 1;
        if (valid) { MemoryBarrier(); std::memcpy(&copy, view, sizeof(copy)); MemoryBarrier(); valid = view->published == 1; }
        if (view) UnmapViewOfFile(view);
        CloseHandle(mapping);
        if (!valid || copy.magic != kRawV2Magic || copy.version != 2 || copy.size != sizeof(copy) || copy.capabilities != kRawV2Capability ||
            copy.reserved || copy.reserved2 || copy.nonce[39] || copy.pid != endpoint->pid || copy.window != reinterpret_cast<uintptr_t>(endpoint->window) ||
            CompareFileTime(&copy.created, &endpoint->created) || endpoint->nonce != copy.nonce) return std::nullopt;
        endpoint->pipe_name = OpenRequestPipe::Name(endpoint->pid, endpoint->nonce);
        if (endpoint->pipe_name.empty()) return std::nullopt;
        return endpoint;
    } catch (...) { return std::nullopt; }
}

bool SingleInstanceCoordinator::ForwardOpenPath(const std::wstring& path,
                                                DWORD timeout_ms, std::array<unsigned char, 16>* out_attempted_id) const {
    if (out_attempted_id) out_attempted_id->fill(0);
    try {
        OpenRequest request;
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return false;
        std::memcpy(request.id.data(), &id, sizeof(id));
        if (out_attempted_id) *out_attempted_id = request.id;
        request.deadline = GetTickCount64() + timeout_ms;
        if (!NormalizeLaunchPath(path, request.path)) return false;
        auto payload = EncodeOpenRequest(request);
        if (payload.empty()) return false;
        COPYDATASTRUCT data{};
        data.dwData = kOpenRequestMessage;
        data.cbData = static_cast<DWORD>(payload.size());
        data.lpData = payload.data();
        // All attempts use the same ID and deadline. A timeout may mean that the
        // first message was accepted, so never retry with a fresh ID.
        std::optional<Endpoint> endpoint;
        while (!endpoint && GetTickCount64() < request.deadline) {
            endpoint = FindPrimaryEndpoint(endpoint_name_);
            if (!endpoint) Sleep(25);
        }
        if (!endpoint) return false;
        AllowSetForegroundWindow(endpoint->pid);
        while (GetTickCount64() < request.deadline) {
            const auto active = ReadPrimary(endpoint_name_);
            if (!active || active->pid != endpoint->pid || active->window != endpoint->window || active->nonce != endpoint->nonce ||
                CompareFileTime(&active->created, &endpoint->created)) return false;
            if (!endpoint->pipe_name.empty()) {
                const auto attempt = (std::min)(request.deadline, GetTickCount64() + uint64_t{1000});
                const auto result = OpenRequestPipe::Exchange(endpoint->pipe_name, {endpoint->pid, endpoint->created}, payload, attempt);
                if (result.reply == OpenRequestPipe::Reply::Accepted || result.reply == OpenRequestPipe::Reply::Duplicate) return true;
                if (result.reply && *result.reply != OpenRequestPipe::Reply::Retry) return false;
                // Even a pre-send failure is not permission to downgrade a RawV2 endpoint.
            } else {
                const auto now = GetTickCount64(); if (now >= request.deadline) break;
                const UINT remaining = static_cast<UINT>((std::min)(uint64_t{500}, request.deadline - now));
                DWORD_PTR result = 0;
                if (SendMessageTimeoutW(endpoint->window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, remaining, &result)) {
                    if (result == TRUE) return true;
                    if (result != RetryOpenReply) return false;
                }
            }
            Sleep(25);
        }
        return false;
    } catch (...) { return false; }
}

std::vector<unsigned char> SingleInstanceCoordinator::EncodeOpenRequest(const OpenRequest& request) {
    try {
        if (request.explorer || request.explorer_received || request.path.size() >= kMaxForwardedPathChars || request.path.find(L'\0') != std::wstring::npos)
            return {};
        OpenRequestHeader header;
        header.chars = static_cast<uint32_t>(request.path.size() + 1);
        header.deadline = request.deadline;
        header.id = request.id;
        std::vector<unsigned char> payload(sizeof(header) + header.chars * sizeof(wchar_t));
        std::memcpy(payload.data(), &header, sizeof(header));
        std::memcpy(payload.data() + sizeof(header), request.path.c_str(), header.chars * sizeof(wchar_t));
        return payload;
    } catch (...) { return {}; }
}

bool SingleInstanceCoordinator::DecodeOpenRequest(const COPYDATASTRUCT* data, OpenRequest& request) {
    try {
        if (!data || data->dwData != kOpenRequestMessage || !data->lpData ||
            data->cbData < sizeof(OpenRequestHeader)) return false;
        OpenRequestHeader header;
        std::memcpy(&header, data->lpData, sizeof(header));
        if (header.version != 2 || !header.chars || header.chars > kMaxForwardedPathChars ||
            data->cbData != sizeof(header) + header.chars * sizeof(wchar_t) ||
            std::all_of(header.id.begin(), header.id.end(), [](unsigned char c) { return c == 0; })) return false;
        std::wstring path(header.chars, L'\0');
        std::memcpy(path.data(), static_cast<const unsigned char*>(data->lpData) + sizeof(header),
                    header.chars * sizeof(wchar_t));
        if (path.back() != L'\0') return false;
        path.pop_back();
        if (path.find(L'\0') != std::wstring::npos) return false;
        request = {header.id, header.deadline, std::move(path)};
        return true;
    } catch (...) { return false; }
}

SingleInstanceCoordinator::OpenAcceptance SingleInstanceCoordinator::AcceptOpenRequest(
    const OpenRequest& request, uint64_t now) {
    std::lock_guard lock(requests_mutex_);
    if (!accepting_requests_ || request.explorer || request.explorer_received) return OpenAcceptance::Invalid;
    if (now >= request.deadline || request.deadline - now > 60000) return OpenAcceptance::Invalid;
    std::erase_if(accepted_, [now](const auto& item) { return item.second.deadline <= now &&
        (!item.second.delivery || item.second.delivery->load() != OpenDeliveryState::Queued); });
    if (const auto found = accepted_.find(RequestKey(request)); found != accepted_.end())
        return found->second.path == request.path && found->second.deadline == request.deadline
            ? OpenAcceptance::Duplicate : OpenAcceptance::Invalid;
    // Do not evict a live ID: its delayed retry would open a duplicate tab.
    if (accepted_.size() >= 256) return OpenAcceptance::Invalid;
    accepted_.emplace(RequestKey(request), request);
    return OpenAcceptance::New;
}

std::optional<SingleInstanceCoordinator::OpenDeliveryState> SingleInstanceCoordinator::ObserveOpenDelivery(
    const std::array<unsigned char, 16>& id) const {
    std::lock_guard lock(requests_mutex_);
    const auto found = accepted_.find(std::make_pair(false, id));
    if (found == accepted_.end() || !found->second.delivery) return std::nullopt;
    return found->second.delivery->load();
}

void SingleInstanceCoordinator::CompleteDelivery(const OpenRequest& request, bool dispatched) noexcept {
    if (!dispatched && request.explorer && request.explorer->handoff &&
        (!request.delivery || request.delivery->load() != OpenDeliveryState::Dispatched)) request.explorer->handoff->Cancel();
    if (!request.delivery) return;
    auto expected = OpenDeliveryState::Queued;
    request.delivery->compare_exchange_strong(expected, dispatched ? OpenDeliveryState::Dispatched : OpenDeliveryState::Cancelled);
}
SingleInstanceCoordinator::OpenSubmission SingleInstanceCoordinator::SubmitOpenRequest(
    OpenRequest request, uint64_t now, HWND host, UINT wake, WakePoster post) {
    if (request.explorer || request.explorer_received) return OpenSubmission::Invalid;
    return SubmitOpenRequestImpl(std::move(request), now, host, wake, post, false);
}
SingleInstanceCoordinator::OpenSubmission SingleInstanceCoordinator::SubmitOpenRequestImpl(
    OpenRequest request, uint64_t now, HWND host, UINT wake, WakePoster post, bool internal) {
    std::lock_guard lock(requests_mutex_);
    // A committed delivery owns its lease independently of the queue. Recognize
    // only that exact context before applying Pending admission requirements.
    if (internal && accepting_requests_ && now < request.deadline) {
        const auto found = accepted_.find(RequestKey(request));
        if (found != accepted_.end() && found->second.explorer == request.explorer &&
            found->second.path == request.path && found->second.deadline == request.deadline &&
            found->second.delivery && found->second.delivery->load() == OpenDeliveryState::Dispatched)
            return OpenSubmission::Duplicate;
    }
    if (internal && (!request.explorer || !request.explorer->selection_read || !QueueLive(request, now)))
        return OpenSubmission::Invalid;
    if (!accepting_requests_ || request.execution_started || request.ui_wait_deadline ||
        now >= request.deadline || request.deadline - now > 60000 ||
        request.path.size() >= kMaxForwardedPathChars || request.path.find(L'\0') != std::wstring::npos ||
        std::all_of(request.id.begin(), request.id.end(), [](unsigned char value) { return value == 0; })) return OpenSubmission::Invalid;
    // A paused UI must not let expired owned requests exhaust admission capacity.
    // Match the delivery identity: an ID may already have a newer acceptance.
    for (auto queued = open_queue_.begin(); queued != open_queue_.end();) {
        if (QueueLive(*queued, now)) { ++queued; continue; }
        if (!queued->explorer && open_failures_.timed_out != UINT32_MAX) ++open_failures_.timed_out;
        ExpireExplorer(*queued, now);
        CompleteDelivery(*queued, false);
        const auto accepted = accepted_.find(RequestKey(*queued));
        if (accepted != accepted_.end() && accepted->second.delivery == queued->delivery)
            accepted_.erase(accepted);
        queued = open_queue_.erase(queued);
    }
    std::erase_if(accepted_, [now](const auto& item) { return item.second.deadline <= now &&
        (!item.second.delivery || item.second.delivery->load() != OpenDeliveryState::Queued); });
    if (const auto found = accepted_.find(RequestKey(request)); found != accepted_.end())
        return found->second.path == request.path && found->second.deadline == request.deadline && found->second.explorer == request.explorer ? OpenSubmission::Duplicate : OpenSubmission::Invalid;
    if (open_queue_.size() + (open_claim_ ? 1u : 0u) >= OpenQueueCapacity || accepted_.size() >= 256) return OpenSubmission::Retry;
    DWORD pid = 0;
    if (!host || !IsWindow(host) || !GetWindowThreadProcessId(host, &pid) || pid != GetCurrentProcessId() || !wake || !post)
        return OpenSubmission::Retry;
    if (request.delivery && request.delivery->load() != OpenDeliveryState::Pending) return OpenSubmission::Invalid;
    try {
        if (!request.delivery) request.delivery = std::make_shared<std::atomic<OpenDeliveryState>>(OpenDeliveryState::Pending);
        open_queue_.push_back(std::move(request));
        try { accepted_.emplace(RequestKey(open_queue_.back()), open_queue_.back()); }
        catch (...) { open_queue_.pop_back(); return OpenSubmission::Retry; }
    } catch (...) { return OpenSubmission::Retry; }
    // Only a wake is posted. The mutex prevents consumption before the commit.
    if (!post(host, wake, 0, 0)) {
        accepted_.erase(RequestKey(open_queue_.back()));
        open_queue_.pop_back();
        return OpenSubmission::Retry;
    }
    open_queue_.back().delivery->store(OpenDeliveryState::Queued);
    return OpenSubmission::Accepted;
}
std::optional<SingleInstanceCoordinator::OpenRequest> SingleInstanceCoordinator::TakeQueuedOpenRequest(uint64_t now, bool reserve_slot) {
    std::lock_guard lock(requests_mutex_);
    if (!accepting_requests_ || (reserve_slot && open_claim_)) return std::nullopt;
    while (!open_queue_.empty()) {
        auto request = std::move(open_queue_.front()); open_queue_.pop_front();
        if (QueueLive(request, now)) {
            if (reserve_slot) { open_claim_ = request.delivery; open_claim_explorer_ = request.explorer; request.execution_started = true; }
            return std::optional<OpenRequest>(std::move(request));
        }
        if (!request.explorer && open_failures_.timed_out != UINT32_MAX) ++open_failures_.timed_out;
        ExpireExplorer(request, now);
        CompleteDelivery(request, false);
        const auto found = accepted_.find(RequestKey(request));
        if (found != accepted_.end() && found->second.delivery == request.delivery) accepted_.erase(found);
    }
    return std::nullopt;
}

void SingleInstanceCoordinator::FinishOpenClaim(OpenRequest request, bool defer) {
    std::lock_guard lock(requests_mutex_);
    if (!open_claim_ || open_claim_ != request.delivery) { CompleteDelivery(request, false); return; }
    if (defer && accepting_requests_ && (request.explorer ? QueueLive(request, GetTickCount64()) : request.ui_wait_deadline != 0) && request.delivery->load() == OpenDeliveryState::Queued) {
        try { open_queue_.push_back(std::move(request)); open_claim_explorer_.reset(); open_claim_.reset(); return; }
        catch (...) {
            if (!request.explorer && open_failures_.failed != UINT32_MAX) ++open_failures_.failed;
        }
    }
    ExpireExplorer(request, GetTickCount64());
    CompleteDelivery(request, false);
    open_claim_explorer_.reset(); open_claim_.reset();
}
SingleInstanceCoordinator::OpenFailures SingleInstanceCoordinator::TakeOpenFailures() {
    std::lock_guard lock(requests_mutex_);
    const auto failures = open_failures_; open_failures_ = {}; return failures;
}

ULONG_PTR SingleInstanceCoordinator::OpenRequestMessageId() noexcept { return kOpenRequestMessage; }

bool SingleInstanceCoordinator::DecodeOpenPath(const COPYDATASTRUCT* data,
                                               std::wstring& path) {
    path.clear();
    if (!data || data->dwData != kOpenPathMessage || !data->lpData ||
        data->cbData < sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0) {
        return false;
    }
    const size_t chars = data->cbData / sizeof(wchar_t);
    if (chars > kMaxForwardedPathChars) return false;
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    if (text[chars - 1] != L'\0') return false;
    path.assign(text, chars - 1);
    return path.find(L'\0') == std::wstring::npos;
}

const wchar_t* SingleInstanceCoordinator::WindowClassName() noexcept {
    return kWindowClass;
}

ULONG_PTR SingleInstanceCoordinator::OpenPathMessageId() noexcept {
    return kOpenPathMessage;
}

} // namespace pulse::app
