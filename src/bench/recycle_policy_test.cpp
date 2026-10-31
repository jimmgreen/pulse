#include "../ops/elevated_delete.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
int main() {
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
