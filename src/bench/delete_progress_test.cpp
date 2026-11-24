#include "../ops/elevated_delete.h"
#include "../ops/native_recycle.h"
#include "../common/recycle_index.h"
#include "../common/recycle_restore.h"

#include <aclapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

// Adds an explicit "deny list folder" entry for the current user to a
// test-private folder, or puts back the DACL saved before it. The owner keeps
// WRITE_DAC, so the restore works while listing is denied.
bool DenyListing(const std::wstring& folder, std::vector<BYTE>& saved_dacl) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    const bool have_user = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
    CloseHandle(token);
    if (!have_user) return false;
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(folder.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &current, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    if (!current) { LocalFree(descriptor); return false; }
    saved_dacl.assign(reinterpret_cast<const BYTE*>(current), reinterpret_cast<const BYTE*>(current) + current->AclSize);
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_LIST_DIRECTORY;
    access.grfAccessMode = DENY_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    PACL updated = nullptr;
    bool ok = SetEntriesInAclW(1, &access, current, &updated) == ERROR_SUCCESS;
    if (ok) ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(folder.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, updated, nullptr) == ERROR_SUCCESS;
    if (updated) LocalFree(updated);
    LocalFree(descriptor);
    return ok;
}
bool RestoreDacl(const std::wstring& folder, std::vector<BYTE>& saved_dacl) {
    return !saved_dacl.empty() &&
        SetNamedSecurityInfoW(const_cast<LPWSTR>(folder.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, reinterpret_cast<PACL>(saved_dacl.data()), nullptr) == ERROR_SUCCESS;
}

int NativeRecycleUnitTests() {
    using namespace pulse::ops;
    using namespace pulse::ops::native_recycle;
    namespace fs = std::filesystem;

    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        failures += !ok;
    };

    // 1. $I v2 writer <-> ParseIndex byte-exact round-trip.
    {
        FILETIME ft{0x12345678u, 0x01DB0000u};
        const std::wstring orig = L"F:\\workspace\\web\\AFFiNE\\1234\\node_modules";
        const auto bytes = BuildIndexV2(orig, 2044174276ull, ft);
        pulse::recycle::IndexRecord parsed;
        check(!bytes.empty() && pulse::recycle::ParseIndex(bytes.data(), bytes.size(), parsed),
              "BuildIndexV2 produces a valid Windows v2 $I record");
        check(parsed.original_path == orig && parsed.size == 2044174276ull &&
              parsed.deleted.dwLowDateTime == ft.dwLowDateTime &&
              parsed.deleted.dwHighDateTime == ft.dwHighDateTime,
              "ParseIndex round-trips original path, byte size and timestamp");
    }

    const auto root = fs::absolute(fs::path("bench_data/native-recycle-test") /
        ("run-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    const auto fake_bin = root / L"fake_bin";
    const auto src_dir = root / L"src";
    fs::create_directories(fake_bin);
    fs::create_directories(src_dir);

    // 2. Isolated single-file native recycle + RestoreSelectedRecyclePayload round-trip.
    {
        const auto file = src_dir / L"report.txt";
        { std::ofstream f(file, std::ios::binary); f << "hello native recycle"; }
        std::atomic<bool> cancel{false};
        const auto res = RecycleItemNative(file.wstring(), 10ull << 20, cancel, {}, false, fake_bin.wstring());
        check(res.handled && SUCCEEDED(res.hr) && res.mutated && !res.is_directory &&
              !fs::exists(file) && !res.recycled_payload.empty() && fs::exists(res.recycled_payload),
              "native recycle atomically moves single file to isolated bin with $I/$R");

        std::wstring restore_err;
        const bool restored = pulse::recycle::RestoreSelectedRecyclePayload(res.recycled_payload, restore_err, true);
        std::string content;
        { std::ifstream f(file, std::ios::binary); content.assign(std::istreambuf_iterator<char>(f), {}); }
        check(restored && content == "hello native recycle" && !fs::exists(res.recycled_payload),
              "RestoreSelectedRecyclePayload restores native-recycled file cleanly");
    }

    // 3. Isolated directory tree native recycle: single-pass scan + O(1) move + restore.
    {
        const auto tree = src_dir / L"node_modules";
        for (int d = 0; d < 20; ++d) {
            const auto sub = tree / (L"pkg_" + std::to_wstring(d)) / L"lib";
            fs::create_directories(sub);
            for (int i = 0; i < 20; ++i) {
                std::ofstream f(sub / (L"f_" + std::to_wstring(i) + L".js"), std::ios::binary);
                f << "module.exports = 42;\n";
            }
        }
        std::atomic<bool> cancel{false};
        bool saw_progress = false;
        const auto res = RecycleItemNative(
            tree.wstring(), 100ull << 20, cancel,
            [&](const ScanProgress& p) { if (p.scanned_items > 0 || p.moving) saw_progress = true; },
            false, fake_bin.wstring());
        check(res.handled && SUCCEEDED(res.hr) && res.mutated && res.is_directory &&
              !fs::exists(tree) && fs::exists(res.recycled_payload) &&
              res.metrics.scanned_items >= 400 && res.metrics.scanned_bytes == 400ull * 21ull &&
              saw_progress,
              "native recycle measures directory tree once, reports progress, and moves root atomically");

        std::wstring restore_err;
        const bool restored = pulse::recycle::RestoreSelectedRecyclePayload(res.recycled_payload, restore_err, true);
        check(restored && fs::exists(tree / L"pkg_19" / L"lib" / L"f_19.js") && !fs::exists(res.recycled_payload),
              "RestoreSelectedRecyclePayload restores native-recycled directory tree intact");
    }

    // 4. MaxCapacity refusal leaves source completely untouched.
    {
        const auto big = src_dir / L"too_big.bin";
        { std::ofstream f(big, std::ios::binary); f << std::string(4096, 'A'); }
        std::atomic<bool> cancel{false};
        const auto res = RecycleItemNative(big.wstring(), 1024, cancel, {}, false, fake_bin.wstring());
        check(res.handled && res.hr == kRecycleBinTooSmall && !res.mutated && fs::exists(big),
              "native recycle refuses item exceeding MaxCapacity without mutation");
    }

    // 4b. A folder that cannot be listed has an unknown size: with a bin limit
    // the recycle is refused untouched instead of passing on a partial count.
    {
        const auto tree = src_dir / L"unlistable";
        const auto denied = tree / L"denied";
        fs::create_directories(tree / L"open");
        fs::create_directories(denied / L"sub");
        { std::ofstream f(tree / L"open" / L"o.bin", std::ios::binary); f << std::string(64u << 10, 'o'); }
        for (int i = 0; i < 4; ++i) {
            std::ofstream f(denied / (L"d" + std::to_wstring(i) + L".bin"), std::ios::binary);
            f << std::string(64u << 10, 'd');
        }
        const uint64_t limit = 128u << 10;  // smaller than the true 320 KiB, larger than the visible 64 KiB
        std::vector<BYTE> saved_dacl;
        if (!DenyListing(denied.wstring(), saved_dacl)) {
            std::printf("[SKIP] unlistable folder fixture: cannot set a deny entry (error %lu)\n", GetLastError());
        } else {
            std::atomic<bool> cancel{false};
            uint64_t items = 0, bytes = 0;
            const HRESULT unlimited = MeasureFolderFast(tree.wstring(), 0, cancel, items, bytes);
            check(SUCCEEDED(unlimited) && bytes == (64u << 10),
                  "without a limit an unlistable folder is still counted best-effort");
            const auto res = RecycleItemNative(tree.wstring(), limit, cancel, {}, false, fake_bin.wstring());
            check(res.handled && res.hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) && !res.mutated &&
                  GetFileAttributesW(tree.c_str()) != INVALID_FILE_ATTRIBUTES && res.recycled_payload.empty(),
                  "an unlistable subfolder refuses a limited native recycle without mutation");
            // The Shell fallback's size preflight follows the same rule.
            RecycleBinLimits limited;
            limited.max_bytes = limit;
            limited.used_bytes = 100ull << 30;  // limit below used space: the size must be proven
            check(CheckRecycleFits(tree.wstring(), limited, cancel) == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) &&
                  fs::exists(denied / L"d3.bin"),
                  "an unlistable subfolder fails the Shell-path size preflight instead of passing a partial count");
            RecycleBinLimits roomy = limited;
            roomy.max_bytes = roomy.used_bytes;
            check(CheckRecycleFits(tree.wstring(), roomy, cancel) == S_OK,
                  "a bin as large as the used space still takes an unlistable folder");
            check(RestoreDacl(denied.wstring(), saved_dacl), "fixture folder permissions are restored");
            const auto full = RecycleItemNative(tree.wstring(), limit, cancel, {}, false, fake_bin.wstring());
            check(full.handled && full.hr == kRecycleBinTooSmall && !full.mutated && fs::exists(denied / L"d3.bin"),
                  "once listable the whole folder is measured and refused as too large");
            check(CheckRecycleFits(tree.wstring(), limited, cancel) == kRecycleBinTooSmall,
                  "once listable the Shell-path preflight measures everything and refuses as too large");
        }
    }

    // 5. Pre-cancelled native recycle leaves source completely untouched.
    {
        const auto keep = src_dir / L"keep_on_cancel.txt";
        { std::ofstream f(keep, std::ios::binary); f << "keep"; }
        std::atomic<bool> cancel{true};
        const auto res = RecycleItemNative(keep.wstring(), 10ull << 20, cancel, {}, false, fake_bin.wstring());
        check(res.handled && res.cancelled && !res.mutated && fs::exists(keep),
              "pre-cancelled native recycle leaves source untouched");
    }

    // 6. Quota compaction planning picks oldest non-protected victims and PurgeOneVictim deletes payload before $I.
    {
        const auto qbin = root / L"quota_bin";
        fs::create_directories(qbin);
        const auto make_entry = [&](const wchar_t* tag, uint64_t size, uint64_t ft_val, bool folder) {
            const auto i_path = qbin / (std::wstring(L"$I") + tag);
            const auto r_path = qbin / (std::wstring(L"$R") + tag);
            FILETIME ft{static_cast<DWORD>(ft_val & 0xFFFFFFFFu), static_cast<DWORD>(ft_val >> 32)};
            const auto idx = BuildIndexV2(L"C:\\dummy\\" + std::wstring(tag), size, ft);
            { std::ofstream f(i_path, std::ios::binary); f.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(idx.size())); }
            if (folder) {
                fs::create_directories(r_path / L"nested");
                for (int k = 0; k < 5; ++k) {
                    std::ofstream f(r_path / L"nested" / (std::to_wstring(k) + L".txt"), std::ios::binary);
                    f << "victim";
                }
            } else {
                std::ofstream f(r_path, std::ios::binary);
                f << "victim";
            }
            return std::pair(i_path.wstring(), r_path.wstring());
        };
        const auto oldest = make_entry(L"OLD001", 600, 1000, true);
        const auto middle = make_entry(L"MID002", 600, 2000, false);
        const auto newest = make_entry(L"NEW003", 600, 3000, false);

        std::unordered_set<std::wstring> protected_set{detail::UpperFold(newest.second)};
        uint64_t used = 0;
        const auto victims = PlanQuotaCompaction(qbin.wstring(), 1000, protected_set, used);
        check(used == 1800 && victims.size() == 2 &&
              detail::UpperFold(victims[0].payload_path) == detail::UpperFold(oldest.second) &&
              detail::UpperFold(victims[1].payload_path) == detail::UpperFold(middle.second),
              "PlanQuotaCompaction selects oldest non-protected entries and never selects protected payload");

        std::atomic<bool> stop{false};
        uint64_t files_deleted = 0;
        DWORD first_err = ERROR_SUCCESS;
        bool purged_all = true;
        for (const auto& v : victims) purged_all &= detail::PurgeOneVictim(v, stop, files_deleted, first_err);
        check(purged_all && files_deleted == 6 && first_err == ERROR_SUCCESS &&
              !fs::exists(oldest.first) && !fs::exists(oldest.second) &&
              !fs::exists(middle.first) && !fs::exists(middle.second) &&
              fs::exists(newest.first) && fs::exists(newest.second),
              "PurgeOneVictim removes victim payloads and $I records while keeping protected entry");
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

int StageTests() {
    using namespace pulse::ops;
    namespace fs = std::filesystem;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = fs::absolute(fs::path("bench_data/copy-delete-feedback-20261120") /
        ("delete-stage-" + std::to_string(GetCurrentProcessId())));
    fs::create_directories(root / "payload");
    for (int i = 0; i < 32; ++i) std::ofstream(root / "payload" / (std::to_string(i) + ".txt")) << "private deletion fixture";
    int failures = 0;
    const auto check = [&](bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; };
    std::vector<std::pair<DeleteStage, bool>> stages;
    bool balanced = true;
    const auto observe = [&](DeleteStage stage, bool begin, HRESULT hr) {
        if (!begin && (stages.empty() || stages.back() != std::pair(stage, true))) balanced = false;
        stages.emplace_back(stage, begin);
        std::printf("STAGE id=%u begin=%d hr=%08lx\n", static_cast<unsigned>(stage), begin, static_cast<unsigned long>(hr));
    };
    std::atomic<bool> cancel{false};
    auto result = DeleteWithCurrentToken({(root / "payload").wstring()}, true, nullptr, cancel, {}, nullptr, observe);
    check(SUCCEEDED(result.hr) && result.sources.size() == 1 && !fs::exists(root / "payload"),
        "real permanent directory deletion confirms root only after backend return");
    check(balanced && std::find(stages.begin(), stages.end(), std::pair(DeleteStage::PerformOperations, false)) != stages.end(),
        "real COM backend stages produce balanced begin/end records");
    stages.clear(); balanced = true;
    result = DeleteWithCurrentToken({(root / "absent").wstring()}, false, nullptr, cancel, {}, nullptr, observe);
    check(FAILED(result.hr) && !result.mutated && result.sources.empty(), "missing recycle source never becomes fabricated success");
    check(balanced && !stages.empty() && !stages.back().second, "recycle policy/setup failure stage closes with HRESULT evidence");
    std::ofstream(root / "cancel.txt") << "retain on cancellation";
    cancel = true; stages.clear();
    result = DeleteWithCurrentToken({(root / "cancel.txt").wstring()}, true, nullptr, cancel, {}, nullptr, observe);
    check(result.cancelled && fs::exists(root / "cancel.txt") && stages.empty(), "pre-cancelled backend does not mutate or start COM stage");
    cancel = false;
    result = DeleteWithCurrentToken({(root / "cancel.txt").wstring()}, true, nullptr, cancel, {}, nullptr,
        [](DeleteStage, bool, HRESULT) { throw std::runtime_error("observer failure"); });
    check(SUCCEEDED(result.hr) && !fs::exists(root / "cancel.txt"), "diagnostic observer failure cannot change delete semantics");
    fs::remove_all(root);
    CoUninitialize();
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

int RecycleScale() {
    using namespace pulse::ops;
    namespace fs = std::filesystem;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    const auto root = fs::absolute(fs::path("bench_data/copy-stall-final-20261120/delete") /
        ("scale-" + std::to_string(GetCurrentProcessId())));
    const auto payload = root / "node_modules";
    fs::create_directories(payload);
    const auto generation = GetTickCount64();
    for (unsigned i = 0; i < 158005; ++i) {
        const auto directory = payload / ("package" + std::to_string(i / 1000)) / "node_modules" /
            ("dependency" + std::to_string((i / 100) % 10)) / "lib";
        if (i % 100 == 0) fs::create_directories(directory);
        std::ofstream file(directory / ("module" + std::to_string(i % 100) + ".js"), std::ios::binary);
        file << "module.exports = 'private Pulse recycle-scale fixture';\n";
        if (!file) { std::printf("GENERATION_FAILED index=%u\n", i); return 3; }
    }
    std::printf("FIXTURE files=158005 generation_ms=%llu pid=%lu\n", GetTickCount64() - generation, GetCurrentProcessId());
    std::fflush(stdout);
    std::atomic<bool> cancel{false}, finished{false};
    const auto begin = GetTickCount64();
    std::thread monitor([&] {
        while (!finished.load()) {
            if (GetFileAttributesW(payload.c_str()) == INVALID_FILE_ATTRIBUTES) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                    std::printf("SOURCE_MISSING elapsed_ms=%llu\n", GetTickCount64() - begin);
                    std::fflush(stdout);
                    break;
                }
            }
            Sleep(10);
        }
    });
    ULONGLONG stage_begin = begin;
    const auto observe = [&](DeleteStage stage, bool start, HRESULT hr) {
        const auto now = GetTickCount64();
        if (start) stage_begin = now;
        std::printf("STAGE id=%u begin=%d elapsed_ms=%llu duration_ms=%llu hr=%08lx\n",
            static_cast<unsigned>(stage), start, now - begin, start ? 0 : now - stage_begin, static_cast<unsigned long>(hr));
        std::fflush(stdout);
    };
    DeleteHooks hooks;
    hooks.on_native_metrics = [](const native_recycle::RecycleMetrics& m) {
        std::printf("NATIVE used=%d fallback=%u scan_ms=%llu idx_ms=%llu move_ms=%llu notify_ms=%llu items=%llu bytes=%llu\n",
            m.used_native, static_cast<unsigned>(m.fallback), m.scan_ms, m.index_write_ms, m.move_ms,
            m.notify_ms, m.scanned_items, m.scanned_bytes);
        std::fflush(stdout);
    };
    auto result = DeleteWithCurrentToken({payload.wstring()}, false, nullptr, cancel, {}, nullptr, observe, &hooks);
    finished = true;
    monitor.join();
    std::printf("RESULT hr=%08lx mutated=%d cancelled=%d confirmed=%zu source_exists=%d elapsed_ms=%llu\n",
        static_cast<unsigned long>(result.hr), result.mutated, result.cancelled, result.sources.size(),
        fs::exists(payload), GetTickCount64() - begin);
    CoUninitialize();
    return SUCCEEDED(result.hr) && result.sources.size() == 1 && !fs::exists(payload) ? 0 : 1;
}

int PreflightScale(const char* source) {
    using namespace pulse::ops;
    namespace fs = std::filesystem;
    const auto payload = fs::absolute(source);
    const auto permitted = fs::absolute("bench_data/copy-stall-final-20261120/delete");
    const auto relative = payload.lexically_relative(permitted);
    if (relative.empty() || relative.native().starts_with(L"..") || !fs::is_directory(payload)) return 2;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    std::atomic<bool> cancel{false};
    RecycleBinLimits limits; limits.max_bytes = 1; limits.used_bytes = 1000000000;
    const auto denied = CheckRecycleFits(payload.wstring(), limits, cancel);
    check(denied == kRecycleBinTooSmall && fs::exists(payload), "small recycle capacity refuses without mutation");
    limits.max_bytes = 100000000; limits.used_bytes = 1000000000;
    std::thread cancellation([&] { Sleep(10); cancel = true; });
    const auto start = GetTickCount64();
    const auto cancelled = CheckRecycleFits(payload.wstring(), limits, cancel);
    cancellation.join();
    std::printf("CANCEL hr=%08lx elapsed_ms=%llu\n", static_cast<unsigned long>(cancelled), GetTickCount64() - start);
    check(cancelled == HRESULT_FROM_WIN32(ERROR_CANCELLED) && fs::exists(payload), "in-flight size preflight cancellation retains source");
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--preflight-scale") return PreflightScale(argv[2]);
    if (argc == 2 && std::string(argv[1]) == "--recycle-scale") return RecycleScale();
    if (argc == 2 && std::string(argv[1]) == "--stages-only") return StageTests();
    if (argc != 1) return 2;
    return NativeRecycleUnitTests();
}
