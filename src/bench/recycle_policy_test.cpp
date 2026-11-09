#include "../ops/elevated_delete.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <vector>

// Exercise the real preparation path but cancel at its last progress callback,
// before RecycleItem. No fixture ever enters the user's Recycle Bin.
static int PrepareBenchmark() {
    namespace filesystem = std::filesystem;
    using namespace pulse::ops;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const auto root = filesystem::current_path() / L"bench_data" /
        (L"delete-prepare-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!filesystem::create_directories(root)) { CoUninitialize(); return 2; }
    int failures = 0;
    constexpr int count = 24;
    for (int i = 0; i < count; ++i) std::ofstream(root / (std::to_wstring(i) + L".txt")) << "isolated fixture";
    for (int round = 0; round < 3; ++round) {
        std::vector<double> timings;
        double total = 0;
        for (int i = 0; i < count; ++i) {
            const auto source = root / (std::to_wstring(i) + L".txt");
            std::atomic<bool> cancel{false};
            bool reached_transfer = false;
            const auto start = std::chrono::steady_clock::now();
            ShellTransferResult result;
            {
                RecycleCache cache; // Each click is an independent request.
                result = DeleteWithCurrentToken({source.wstring()}, false, nullptr, cancel,
                    [&](const std::wstring&, float) { reached_transfer = true; cancel.store(true); }, &cache);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            timings.push_back(ms); total += ms;
            if (!reached_transfer || !result.cancelled || result.mutated || !result.sources.empty() ||
                !filesystem::exists(source) || filesystem::file_size(source) != 16) {
                std::printf("[FAIL] preparation item=%d hr=%08lx reached=%d cancelled=%d mutated=%d\n",
                    i, static_cast<unsigned long>(result.hr), reached_transfer, result.cancelled, result.mutated);
                ++failures;
                break;
            }
        }
        const double first = timings.front();
        std::sort(timings.begin() + 1, timings.end());
        std::printf("[%s] round=%d independent_requests=%zu first_ms=%.3f subsequent_median_ms=%.3f total_ms=%.3f\n",
            failures ? "FAIL" : "PASS", round, timings.size(), first, timings[timings.size() / 2], total);
    }
    filesystem::remove_all(root);
    CoUninitialize();
    return failures ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--prepare-bench") == 0) return PrepareBenchmark();
    namespace fs = std::filesystem;
    using namespace pulse::ops;
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return 1;
    const auto root = fs::path(temp) / (L"PulseRecyclePolicyTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    int failed = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failed; };
    try {
        fs::create_directories(root / L"sub");
        { std::ofstream out(root / L"sub" / L"file", std::ios::binary); out << std::string(8192, 'x'); }
        std::atomic<bool> cancel{true};
        RecycleBinLimits unlimited;
        check(CheckRecycleFits(root.wstring(), unlimited, cancel) == HRESULT_FROM_WIN32(ERROR_CANCELLED), "cancel precedes unlimited-capacity shortcut");
        cancel.store(false);
        RecycleBinLimits tiny; tiny.max_bytes = 4096; tiny.used_bytes = 65536;
        check(CheckRecycleFits(root.wstring(), tiny, cancel) == kRecycleBinTooSmall, "recursive size refuses oversized recycle");
        check(CheckRecycleFits(root.wstring(), tiny, cancel, false) == S_OK, "advisory check avoids recursive traversal");
        const auto policy = QueryRecyclePolicy(root.wstring());
        DWORD error = 0;
        const auto legacy = ReadRecycleBinLimits(root.wstring(), &error);
        check(policy.volume_error == error && policy.limits.disabled == legacy.disabled && policy.limits.max_bytes == legacy.max_bytes, "combined policy matches public capacity query");
        const auto unc = QueryRecyclePolicy(L"\\\\pulse-invalid-test-host\\share\\file");
        check(!unc.available && !unc.volume_error, "UNC is refused without contacting network");
        const auto extended_unc = QueryRecyclePolicy(L"\\\\?\\UNC\\pulse-invalid-test-host\\share\\file");
        check(!extended_unc.available && !extended_unc.volume_error, "extended UNC is refused without contacting network");
        check(fs::file_size(root / L"sub" / L"file") == 8192, "policy checks preserve source");
        fs::remove_all(root);
    } catch (const std::exception& e) { std::printf("[FAIL] %s\n", e.what()); ++failed; }
    return failed ? 1 : 0;
}
