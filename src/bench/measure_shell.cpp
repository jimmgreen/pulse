// measure_shell.cpp — Stage 0 Shell icon/thumbnail cost benchmark
// Usage: pulse_bench_shell [dir] [N]
//   dir: directory to sample (default: C:\Windows\System32)
//   N:   number of items to sample (default: 200)

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <combaseapi.h>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <cmath>

namespace fs = std::filesystem;

static double millis(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

static double percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    std::sort(sorted.begin(), sorted.end());
    double idx = p * (sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    double t = idx - lo;
    return sorted[lo] * (1 - t) + sorted[hi] * t;
}

static void report_times(const std::string& label, const std::vector<double>& vals_in, int64_t extrapolate_n) {
    std::vector<double> vals = vals_in;
    if (vals.empty()) {
        std::cout << "  " << label << ": no samples\n";
        return;
    }
    std::sort(vals.begin(), vals.end());
    double sum = std::accumulate(vals.begin(), vals.end(), 0.0);
    double mean = sum / vals.size();
    double med = percentile(vals, 0.5);
    double p95 = percentile(vals, 0.95);
    double mx = vals.back();
    std::cout << std::fixed << std::setprecision(3)
              << "  " << std::left << std::setw(22) << label
              << " mean=" << std::right << std::setw(8) << mean
              << " med=" << std::setw(8) << med
              << " p95=" << std::setw(8) << p95
              << " max=" << std::setw(8) << mx
              << "  (n=" << vals.size() << ")\n";
    if (extrapolate_n > 0) {
        double total_s = (mean * extrapolate_n) / 1000.0;
        std::cout << std::fixed << std::setprecision(1)
                  << "    -> 100k first-screen items @ mean = " << total_s << " s\n";
    }
}

static std::vector<std::wstring> list_files(const std::wstring& dir, int64_t max_n) {
    std::vector<std::wstring> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (out.size() >= static_cast<size_t>(max_n)) break;
        out.push_back(e.path().wstring());
    }
    return out;
}

static void time_smallicon(const std::wstring& path, double& ms) {
    auto t0 = std::chrono::steady_clock::now();
    SHFILEINFOW sfi{};
    SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                   SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    auto t1 = std::chrono::steady_clock::now();
    ms = millis(t1 - t0);
    if (sfi.hIcon) DestroyIcon(sfi.hIcon);
}

static void time_sysiconindex(const std::wstring& path, double& ms) {
    auto t0 = std::chrono::steady_clock::now();
    SHFILEINFOW sfi{};
    SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                   SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES);
    auto t1 = std::chrono::steady_clock::now();
    ms = millis(t1 - t0);
}

static void time_thumbnail(const std::wstring& path, double& ms, bool& ok) {
    ok = false;
    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        HBITMAP hbmp = nullptr;
        auto t0 = std::chrono::steady_clock::now();
        hr = factory->GetImage({256, 256}, SIIGBF_THUMBNAILONLY, &hbmp);
        auto t1 = std::chrono::steady_clock::now();
        ms = millis(t1 - t0);
        if (SUCCEEDED(hr) && hbmp) {
            ok = true;
            DeleteObject(hbmp);
        }
        factory->Release();
    }
}

static void run_for_directory(const std::wstring& dir, int64_t n) {
    std::wcout << L"\n=== " << dir << L" (first " << n << L" items) ===\n";
    std::vector<std::wstring> files = list_files(dir, n);
    if (files.empty()) {
        std::wcout << L"no files\n";
        return;
    }
    std::wcout << L"sampled " << files.size() << L" items\n";

    std::vector<double> t1, t2, t3;
    std::vector<double> t3_success;
    int thumbs_hit = 0;

    for (size_t i = 0; i < files.size(); ++i) {
        const auto& p = files[i];
        double a, b, c;
        bool ok;
        time_smallicon(p, a);
        time_sysiconindex(p, b);
        time_thumbnail(p, c, ok);
        t1.push_back(a);
        t2.push_back(b);
        t3.push_back(c);
        if (ok) {
            t3_success.push_back(c);
            ++thumbs_hit;
        }
        if ((i + 1) % 50 == 0) {
            std::wcout << L"  processed " << (i + 1) << L" / " << files.size() << L"\n";
        }
    }

    report_times("SHGFI_ICON|SMALLICON", t1, 100000);
    report_times("SHGFI_SYSICONINDEX", t2, 100000);
    report_times("IShellItemImageFactory thumb", t3, 100000);
    if (!t3_success.empty()) {
        report_times("thumbnail (hits only)", t3_success, 100000);
    }
    std::cout << "  thumbnail hit rate: " << thumbs_hit << " / " << files.size()
              << " (" << std::fixed << std::setprecision(1)
              << (100.0 * thumbs_hit / files.size()) << "%)\n";
}

static int usage(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [dir] [N]\n";
    return 1;
}

int wmain(int argc, wchar_t* argv[]);

int main() {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int r = wmain(argc, argv);
    LocalFree(argv);
    return r;
}

int wmain(int argc, wchar_t* argv[]) {
    std::ios::sync_with_stdio(false);

    int64_t n = 200;

    if (argc > 1) n = _wtoi64(argv[1]);
    if (argc > 2) return usage("pulse_bench_shell");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        std::wcerr << L"CoInitializeEx failed: " << hr << L"\n";
        return 1;
    }

    run_for_directory(std::filesystem::absolute(L"bench_data/d10k").wstring(), n);
    run_for_directory(L"C:\\Windows\\System32", n);

    CoUninitialize();
    return 0;
}
