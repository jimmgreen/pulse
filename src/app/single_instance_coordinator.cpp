#include "single_instance_coordinator.h"

#include <limits>
#include <algorithm>
#include <cstring>
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

} // namespace

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
    return AcquireResult::Primary;
}

void SingleInstanceCoordinator::Release() {
    if (endpoint_view_) {
        InterlockedExchange(&static_cast<EndpointRecord*>(endpoint_view_)->published, 0);
        UnmapViewOfFile(endpoint_view_); endpoint_view_ = nullptr;
    }
    if (endpoint_window_ && !endpoint_nonce_.empty())
        RemovePropW(endpoint_window_, endpoint_nonce_.c_str());
    endpoint_window_ = nullptr;
    if (endpoint_mapping_) { CloseHandle(endpoint_mapping_); endpoint_mapping_ = nullptr; }
    if (!mutex_) return;
    EnumWindows([](HWND window, LPARAM) -> BOOL {
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId()) RemovePropW(window, kPrimaryProperty);
        return TRUE;
    }, 0);
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
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
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    wchar_t absolute[32768]{};
    const DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(absolute), absolute, nullptr);
    if (!n || n >= ARRAYSIZE(absolute)) return false;
    output.assign(absolute, n);
    return true;
}

bool SingleInstanceCoordinator::PublishEndpoint(HWND window) {
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
    InterlockedExchange(&record->published, 1);
    return true;
}
bool SingleInstanceCoordinator::OwnsEndpoint(HWND window) const {
    return mutex_ && endpoint_view_ && endpoint_window_ == window &&
        GetPropW(window, endpoint_nonce_.c_str()) == reinterpret_cast<HANDLE>(1);
}
HWND SingleInstanceCoordinator::FindPrimaryWindow(std::wstring_view endpoint_name) {
    const auto base = EndpointName(endpoint_name);
    if (base.empty()) return nullptr;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, (base + L".Endpoint").c_str());
    if (!mapping) return nullptr;
    auto* record = static_cast<const EndpointRecord*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(EndpointRecord)));
    EndpointRecord copy{};
    bool valid = record && record->published == 1;
    if (valid) { MemoryBarrier(); std::memcpy(&copy, record, sizeof(copy)); MemoryBarrier();
        valid = record->published == 1 && copy.version == 1 && copy.nonce[39] == 0; }
    if (record) UnmapViewOfFile(record);
    CloseHandle(mapping);
    if (!valid) return nullptr;
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(copy.window));
    DWORD pid = 0, session = 0, own_session = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid != copy.pid || !ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || session != own_session ||
        GetPropW(window, copy.nonce) != reinterpret_cast<HANDLE>(1)) return nullptr;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return nullptr;
    FILETIME created{};
    valid = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && Created(process, created) &&
        CompareFileTime(&created, &copy.created) == 0 && ProcessUserSidString(process) == CurrentUserSidString();
    CloseHandle(process);
    return valid ? window : nullptr;
}

bool SingleInstanceCoordinator::ForwardOpenPath(const std::wstring& path,
                                                DWORD timeout_ms) const {
    OpenRequest request;
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return false;
    std::memcpy(request.id.data(), &id, sizeof(id));
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
    while (GetTickCount64() < request.deadline) {
        if (HWND hwnd = FindPrimaryWindow(endpoint_name_)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            const auto now = GetTickCount64();
            if (now >= request.deadline) break;
            const UINT remaining = static_cast<UINT>((std::min)(uint64_t{500}, request.deadline - now));
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, remaining, &result))
                return result == TRUE;
        }
        Sleep(25);
    }
    return false;
}

std::vector<unsigned char> SingleInstanceCoordinator::EncodeOpenRequest(const OpenRequest& request) {
    if (request.path.size() >= kMaxForwardedPathChars || request.path.find(L'\0') != std::wstring::npos)
        return {};
    OpenRequestHeader header;
    header.chars = static_cast<uint32_t>(request.path.size() + 1);
    header.deadline = request.deadline;
    header.id = request.id;
    std::vector<unsigned char> payload(sizeof(header) + header.chars * sizeof(wchar_t));
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), request.path.c_str(), header.chars * sizeof(wchar_t));
    return payload;
}

bool SingleInstanceCoordinator::DecodeOpenRequest(const COPYDATASTRUCT* data, OpenRequest& request) {
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
}

SingleInstanceCoordinator::OpenAcceptance SingleInstanceCoordinator::AcceptOpenRequest(
    const OpenRequest& request, uint64_t now) {
    if (now >= request.deadline || request.deadline - now > 60000) return OpenAcceptance::Invalid;
    std::erase_if(accepted_, [now](const auto& item) { return item.second.deadline <= now; });
    if (const auto found = accepted_.find(request.id); found != accepted_.end())
        return found->second.path == request.path && found->second.deadline == request.deadline
            ? OpenAcceptance::Duplicate : OpenAcceptance::Invalid;
    // Do not evict a live ID: its delayed retry would open a duplicate tab.
    if (accepted_.size() >= 256) return OpenAcceptance::Invalid;
    accepted_.emplace(request.id, request);
    return OpenAcceptance::New;
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
