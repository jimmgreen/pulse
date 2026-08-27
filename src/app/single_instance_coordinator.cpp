#include "single_instance_coordinator.h"

#include <limits>

namespace pulse::app {
namespace {

constexpr wchar_t kMutexName[] = L"Local\\Pulse.Singleton";
constexpr wchar_t kWindowClass[] = L"PulseMainWindow";
constexpr ULONG_PTR kOpenPathMessage = 0x50554C53; // 'PULS'
constexpr size_t kMaxForwardedPathChars = 32768;

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
    HWND hwnd = nullptr;
    for (int i = 0; i < 50 && !hwnd; ++i) {
        hwnd = FindWindowW(kWindowClass, nullptr);
        if (!hwnd) Sleep(50);
    }
    if (!hwnd) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) AllowSetForegroundWindow(pid);
    if (path.size() >= kMaxForwardedPathChars ||
        path.size() > ((std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) - 1) {
        return false;
    }
    COPYDATASTRUCT data{};
    data.dwData = kOpenPathMessage;
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(path.c_str());
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                               SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout_ms, &result) != 0 &&
           result != FALSE;
}

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
