#include "../app/win_e_context.h"
#include "../common/runtime_log.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

namespace pulse::app {
struct WinEContextTestPeer {
    static HANDLE WINAPI Denied(DWORD, BOOL, DWORD) { SetLastError(ERROR_ACCESS_DENIED); return nullptr; }
    static void DenyProcess(WinEContextObserver& observer) { observer.open_process_ = &Denied; }
};
}
namespace {
std::string EventLine(const std::string& contents, const char* event, uint64_t id) {
    const std::string context = "\"context_id\":" + std::to_string(id) + ",";
    size_t begin = 0;
    while (begin < contents.size()) {
        const auto end = contents.find('\n', begin);
        const auto line = contents.substr(begin, end == std::string::npos ? end : end - begin);
        if (line.find(event) != std::string::npos && line.find(context) != std::string::npos) return line;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}
}
int main() {
    using namespace pulse;
    namespace log = diagnostics::runtime;
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"win-e-context-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    bool ok = true;
    const auto check = [&](bool result, const char* label) {
        std::printf("[%s] %s\n", result ? "PASS" : "FAIL", label); ok &= result;
    };
    check(log::Initialize(root.wstring(), "winecontext"), "initialize real runtime logger");
    const HWND window = CreateWindowExW(0, L"STATIC", L"private-window-title-never-log",
        WS_OVERLAPPED, 15, 25, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create isolated real process window");
    app::WinEContextObserver observer;
    const auto success = observer.Poll(window, 100);
    check(success && observer.CurrentContextId() == success && observer.Poll(window, 101) == success,
        "unchanged foreground retains its context without another query");
    const auto unknown = observer.Poll(reinterpret_cast<HWND>(1), 102);
    app::WinEContextTestPeer::DenyProcess(observer);
    const auto denied = observer.Poll(window, 103);
    const auto forced = observer.Poll(window, 104, true);
    check(success != unknown && unknown != denied && denied != forced, "changes and force allocate distinct contexts");
    check(log::Flush(10000), "all context events flush to disk");
    log::Shutdown();
    if (window) DestroyWindow(window);
    const auto file = root / L"Diagnostics" / L"Runtime" /
        (L"winecontext-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    std::ifstream stream(file, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    const auto process = EventLine(contents, "win_e_context_process", success);
    const auto security = EventLine(contents, "win_e_context_security", success);
    check(process.find("\"pid\":" + std::to_string(GetCurrentProcessId())) != std::string::npos &&
        process.find("\"process_error\":0") != std::string::npos &&
        process.find("\"creation_time\":0") == std::string::npos && !process.empty(),
        "persisted process event has PID and process reuse identity");
    check(process.find("\"program\":\"\"") == std::string::npos && process.find(".exe") != std::string::npos &&
        process.find("\\\\") == std::string::npos && contents.find("private-window-title") == std::string::npos,
        "only program basename is persisted, never full path or window title");
    check(security.find("\"token_error\":0") != std::string::npos &&
        security.find("\"integrity\":0") == std::string::npos &&
        security.find("\"self_integrity\":0") == std::string::npos && !security.empty(),
        "persisted security event contains foreground and self integrity");
    check(!EventLine(contents, "win_e_context_window", success).empty() &&
        !EventLine(contents, "win_e_context_monitor", success).empty(),
        "window and monitor geometry persist as separate correlated events");
    const auto invalid = EventLine(contents, "win_e_context_process", unknown);
    check(invalid.find("\"pid\":0") != std::string::npos &&
        invalid.find("\"window_error\":1400") != std::string::npos,
        "unknown HWND persists its actual window-query error");
    const auto failure = EventLine(contents, "win_e_context_process", denied);
    check(failure.find("\"process_error\":5") != std::string::npos &&
        EventLine(contents, "win_e_context_security", denied).find("\"token_error\":5") != std::string::npos &&
        !EventLine(contents, "win_e_context_window", denied).empty(),
        "denied process read persists error and independent window evidence");
    check(contents.find("fields_truncated") == std::string::npos, "no context event exceeds logger field capacity");
    std::wprintf(L"Log: %ls\n", file.c_str());
    return ok ? 0 : 1;
}
