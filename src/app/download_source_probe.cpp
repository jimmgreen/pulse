#include "download_source_probe.h"
#include "app_messages.h"
#include "update_transport.h"
#include "pulse_update_config.h"

#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace pulse::app {
namespace {

constexpr uint64_t kMaximumProbeBytes = 1024 * 1024;
constexpr UpdateSource kRowSource[kSourceProbeCount] = {UpdateSource::Mirror1, UpdateSource::Mirror2,
                                                        UpdateSource::GitHub};

std::atomic<int> g_result[kSourceProbeCount] = {kSourceProbeNotRun, kSourceProbeNotRun, kSourceProbeNotRun};
std::atomic<int> g_running{0};
std::mutex g_proxy_lock;
std::wstring* g_proxy = nullptr;   // never freed: probe threads may outlive the window

std::wstring ReadProxy() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config)) return {};
    std::wstring proxy = config.lpszProxy ? config.lpszProxy : L"";
    if (config.lpszProxy) GlobalFree(config.lpszProxy);
    if (config.lpszProxyBypass) GlobalFree(config.lpszProxyBypass);
    if (config.lpszAutoConfigUrl) GlobalFree(config.lpszAutoConfigUrl);
    // "host:port", or per scheme: "http=host:port;https=host:port".
    if (proxy.find(L'=') != std::wstring::npos) {
        std::wstring picked;
        size_t start = 0;
        while (start <= proxy.size()) {
            size_t end = proxy.find(L';', start);
            if (end == std::wstring::npos) end = proxy.size();
            const std::wstring part = proxy.substr(start, end - start);
            if (part.rfind(L"https=", 0) == 0) { picked = part.substr(6); break; }
            if (picked.empty() && part.rfind(L"http=", 0) == 0) picked = part.substr(5);
            start = end + 1;
        }
        proxy = picked;
    }
    const size_t scheme = proxy.find(L"://");
    if (scheme != std::wstring::npos) proxy.erase(0, scheme + 3);
    return proxy;
}

void Probe(int row, std::wstring url, HWND notify) {
    int result = kSourceProbeFailed;
    if (!url.empty()) {
        std::atomic<bool> cancelled{false};
        UpdateError category{};
        DWORD error = 0;
        const ULONGLONG started = GetTickCount64();
        const bool ok = ReadUpdateResponse(url, kMaximumProbeBytes, cancelled,
                                           [](const void*, DWORD) { return true; }, category, error);
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (ok) result = static_cast<int>(elapsed > 600000 ? 600000 : elapsed);
    }
    g_result[row].store(result);
    g_running.fetch_sub(1);
    if (notify && IsWindow(notify)) PostMessageW(notify, WM_SOURCE_PROBE_DONE, 0, 0);
}

} // namespace

void StartSourceProbe(HWND notify) {
    if (g_running.load() > 0) return;
    const std::wstring manifest = PULSE_UPDATE_MANIFEST_URL;
    {
        std::lock_guard<std::mutex> lock(g_proxy_lock);
        if (!g_proxy) g_proxy = new std::wstring;
        *g_proxy = ReadProxy();
    }
    for (int row = 0; row < kSourceProbeCount; ++row) {
        std::wstring url = manifest.empty() ? std::wstring{} : UpdateSourceUrl(manifest, kRowSource[row]);
        g_result[row].store(kSourceProbeRunning);
        g_running.fetch_add(1);
        try {
            std::thread(Probe, row, std::move(url), notify).detach();
        } catch (...) {
            g_result[row].store(kSourceProbeFailed);
            g_running.fetch_sub(1);
        }
    }
}

bool SourceProbeRunning() noexcept { return g_running.load() > 0; }

int SourceProbeResult(int row) noexcept {
    return row >= 0 && row < kSourceProbeCount ? g_result[row].load() : kSourceProbeNotRun;
}

std::wstring SourceProbeProxy() {
    std::lock_guard<std::mutex> lock(g_proxy_lock);
    return g_proxy ? *g_proxy : std::wstring{};
}

} // namespace pulse::app
