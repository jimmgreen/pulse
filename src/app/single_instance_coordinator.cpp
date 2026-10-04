#include "single_instance_coordinator.h"

#include <limits>
#include <algorithm>
#include <cstring>
#include <objbase.h>

namespace pulse::app {
namespace {

constexpr wchar_t kMutexName[] = L"Local\\Pulse.Singleton";
constexpr wchar_t kWindowClass[] = L"PulseMainWindow";
constexpr ULONG_PTR kOpenPathMessage = 0x50554C53; // 'PULS'
constexpr ULONG_PTR kOpenRequestMessage = 0x50554C32; // 'PUL2'
constexpr ULONG_PTR kTabTransferMessage = 0x50544254; // 'PTBT'
constexpr ULONG_PTR kTabTransferSelectionMessage = 0x50544253; // 'PTBS'
constexpr ULONG_PTR kDrainRequestMessage = 0x50554452; // 'PUDR'
constexpr size_t kMaxForwardedPathChars = 32768;
struct OpenRequestHeader {
    uint32_t version = 2;
    uint32_t chars = 0;
    uint64_t deadline = 0;
    std::array<unsigned char, 16> id{};
};
static_assert(sizeof(OpenRequestHeader) == 32);
// Room for a folder, a focus name and a few hundred entry names.
constexpr size_t kMaxTabTransferChars = 1u << 18;

// One COPYDATASTRUCT of UTF-16 text to one window.
bool SendBlobPayload(HWND target, ULONG_PTR id, const void* bytes, size_t size, DWORD timeout_ms) {
    if (!target || !IsWindow(target)) return false;
    if (!bytes || size < sizeof(wchar_t) || size % sizeof(wchar_t) != 0 ||
        size > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid && pid != GetCurrentProcessId()) AllowSetForegroundWindow(pid);
    COPYDATASTRUCT data{};
    data.dwData = id;
    data.cbData = static_cast<DWORD>(size);
    data.lpData = const_cast<void*>(bytes);
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    const LRESULT sent = SendMessageTimeoutW(target, WM_COPYDATA, 0,
                                             reinterpret_cast<LPARAM>(&data),
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                             timeout_ms, &result);
    if (sent) return result != FALSE;
    // A receiver busy with the message has taken it: the path arrives even
    // though the reply did not, so only a delivery failure is a real failure.
    return GetLastError() == ERROR_TIMEOUT;
}

// A COPYDATASTRUCT carrying one UTF-16 path, terminated, to one window.
bool SendPathPayload(HWND target, ULONG_PTR id, const std::wstring& path, DWORD timeout_ms) {
    if (path.size() >= kMaxForwardedPathChars) return false;
    return SendBlobPayload(target, id, path.c_str(),
                           (path.size() + 1) * sizeof(wchar_t), timeout_ms);
}

bool DecodePathPayload(const COPYDATASTRUCT* data, ULONG_PTR id, std::wstring& path) {
    path.clear();
    if (!data || data->dwData != id || !data->lpData ||
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

} // namespace

SingleInstanceCoordinator::~SingleInstanceCoordinator() {
    Release();
}

SingleInstanceCoordinator::AcquireResult SingleInstanceCoordinator::Acquire(
    std::wstring_view mutex_name) {
    if (mutex_) return AcquireResult::Primary;
    const std::wstring name = mutex_name.empty() ? kMutexName : std::wstring(mutex_name);
    SetLastError(ERROR_SUCCESS);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!mutex) return AcquireResult::Failed;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return AcquireResult::Existing;
    }
    mutex_ = mutex;
    return AcquireResult::Primary;
}

void SingleInstanceCoordinator::Release() {
    if (!mutex_) return;
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
    mutex_ = nullptr;
}

bool SingleInstanceCoordinator::ForwardOpenPath(const std::wstring& path,
                                                DWORD timeout_ms) const {
    OpenRequest request;
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return false;
    std::memcpy(request.id.data(), &id, sizeof(id));
    request.deadline = GetTickCount64() + timeout_ms;
    request.path = path;
    auto payload = EncodeOpenRequest(request);
    if (payload.empty()) return false;
    COPYDATASTRUCT data{};
    data.dwData = kOpenRequestMessage;
    data.cbData = static_cast<DWORD>(payload.size());
    data.lpData = payload.data();
    // All attempts use the same ID and deadline. A timeout may mean that the
    // first message was accepted, so never retry with a fresh ID.
    while (GetTickCount64() < request.deadline) {
        if (HWND hwnd = FindWindowW(kWindowClass, nullptr)) {
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

bool SingleInstanceCoordinator::SendOpenPathToWindow(HWND target, const std::wstring& path,
                                                     DWORD timeout_ms) {
    return SendPathPayload(target, kOpenPathMessage, path, timeout_ms);
}

bool SingleInstanceCoordinator::SendTabTransfer(HWND target, const std::wstring& path,
                                                DWORD timeout_ms) {
    return SendPathPayload(target, kTabTransferMessage, path, timeout_ms);
}

bool SingleInstanceCoordinator::DecodeTabTransfer(const COPYDATASTRUCT* data,
                                                  std::wstring& path) {
    return DecodePathPayload(data, kTabTransferMessage, path);
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

bool SingleInstanceCoordinator::SendTabTransfer(HWND target, const TabTransfer& transfer,
                                                DWORD timeout_ms) {
    // folder\0 focus\0 name\0 name\0 ... : every field terminated, so no entry
    // name can smuggle itself into the next field.
    std::wstring blob;
    auto append = [&blob](const std::wstring& field) {
        blob.append(field);
        blob.push_back(L'\0');
    };
    append(transfer.path);
    append(transfer.focus_name);
    for (const auto& name : transfer.selected_names) append(name);
    if (blob.size() > kMaxTabTransferChars) return false;
    return SendBlobPayload(target, kTabTransferSelectionMessage, blob.data(),
                           blob.size() * sizeof(wchar_t), timeout_ms);
}

bool SingleInstanceCoordinator::DecodeTabTransfer(const COPYDATASTRUCT* data,
                                                  TabTransfer& transfer) {
    transfer = {};
    if (!data || data->dwData != kTabTransferSelectionMessage || !data->lpData ||
        data->cbData < 2 * sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0) {
        return false;
    }
    const size_t chars = data->cbData / sizeof(wchar_t);
    if (chars > kMaxTabTransferChars) return false;
    const auto* text = static_cast<const wchar_t*>(data->lpData);
    if (text[chars - 1] != L'\0') return false;
    size_t pos = 0;
    const auto next = [&](std::wstring& field) {
        const size_t end = std::wstring_view(text + pos, chars - pos).find(L'\0');
        if (end == std::wstring_view::npos) return false;
        field.assign(text + pos, end);
        pos += end + 1;
        return true;
    };
    if (!next(transfer.path) || !next(transfer.focus_name)) return false;
    while (pos < chars) {
        std::wstring name;
        if (!next(name)) return false;
        if (!name.empty()) transfer.selected_names.push_back(std::move(name));
    }
    return !transfer.path.empty();
}

bool SingleInstanceCoordinator::DecodeOpenPath(const COPYDATASTRUCT* data,
                                               std::wstring& path) {
    return DecodePathPayload(data, kOpenPathMessage, path);
}

bool SingleInstanceCoordinator::SendDrainRequest(HWND target, HWND sink, DWORD timeout_ms) {
    if (!sink) return false;
    return SendBlobPayload(target, kDrainRequestMessage, &sink, sizeof(sink), timeout_ms);
}

bool SingleInstanceCoordinator::DecodeDrainRequest(const COPYDATASTRUCT* data, HWND& sink) {
    sink = nullptr;
    if (!data || data->dwData != kDrainRequestMessage || !data->lpData ||
        data->cbData != sizeof(HWND)) {
        return false;
    }
    std::memcpy(&sink, data->lpData, sizeof(sink));
    return sink != nullptr && IsWindow(sink);
}

ULONG_PTR SingleInstanceCoordinator::DrainRequestMessageId() noexcept {
    return kDrainRequestMessage;
}

const wchar_t* SingleInstanceCoordinator::WindowClassName() noexcept {
    return kWindowClass;
}

ULONG_PTR SingleInstanceCoordinator::OpenPathMessageId() noexcept {
    return kOpenPathMessage;
}

} // namespace pulse::app
