#include "../ops/transfer_engine.h"
#include "../ops/owned_transfer_output.h"
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <chrono>
#include <thread>
#include <system_error>
#include <winioctl.h>
#include <objbase.h>
#include <array>

using namespace pulse::ops;
namespace {
int failures = 0;
std::wstring ExtendedPath(const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal().wstring();
    if (absolute.starts_with(LR"(\\?\)")) return absolute;
    if (absolute.starts_with(LR"(\\)")) return LR"(\\?\UNC\)" + absolute.substr(2);
    return LR"(\\?\)" + absolute;
}
void Check(bool value, const char* name) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}
bool Write(const std::wstring& path, uint64_t bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> block(1 << 20);
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<unsigned char>((i * 37 + i / 251) & 255);
    bool ok = true;
    while (bytes && ok) {
        DWORD count = static_cast<DWORD>((std::min<uint64_t>)(bytes, block.size())), done = 0;
        ok = WriteFile(h, block.data(), count, &done, nullptr) && done == count;
        bytes -= count;
    }
    CloseHandle(h);
    return ok;
}
uint64_t Hash(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<unsigned char> block(1 << 20);
    uint64_t hash = 14695981039346656037ull;
    DWORD count = 0;
    while (ReadFile(h, block.data(), static_cast<DWORD>(block.size()), &count, nullptr) && count)
        for (DWORD i = 0; i < count; ++i) hash = (hash ^ block[i]) * 1099511628211ull;
    CloseHandle(h);
    return hash;
}
struct Control {
    std::atomic<bool> cancel{false}, pause{false};
    std::mutex mutex;
    std::condition_variable cv;
};
std::vector<SmallCopyResult> Drain(SmallCopyPool& pool) {
    std::vector<SmallCopyResult> results;
    const auto deadline = GetTickCount64() + 15000;
    while (pool.Outstanding() && GetTickCount64() < deadline) {
        auto batch = pool.Take(true);
        results.insert(results.end(), batch.begin(), batch.end());
    }
    Check(pool.Outstanding() == 0, "bounded completion");
    return results;
}
}
namespace {
bool ShaDigest(const std::wstring& path, std::array<unsigned char, 32>& digest) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    struct Cleanup {
        BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash; HANDLE& file;
        ~Cleanup() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } cleanup{algorithm, hash, file};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> bytes(1 << 20);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr)) return false;
        if (!got) break;
        if (BCryptHashData(hash, bytes.data(), got, 0) < 0) return false;
    }
    return BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
}
bool ExactLength(const std::wstring& path, uint64_t bytes) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    const bool matches = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == bytes;
    CloseHandle(file);
    return matches;
}
int AdmissionOnly() {
    namespace fs = std::filesystem;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) { Check(false, "admission GUID"); return 1; }
    wchar_t name[40]{}; StringFromGUID2(guid, name, 40);
    const auto base = fs::absolute(L"bench_data").lexically_normal();
    const auto root = base / (std::wstring(L"pool_admission_") + name);
    std::error_code error;
    const bool created = fs::create_directories(root, error);
    Check(created && !error, "unique admission fixture");
    if (!created || error) return 1;
    constexpr uint64_t actual_bytes = 8ull << 20, weight = 128ull << 20;
    std::array<std::wstring, 2> sources{(root / L"source0.bin").wstring(), (root / L"source1.bin").wstring()};
    std::array<std::array<unsigned char, 32>, 2> expected{};
    bool prepared = true;
    for (size_t i = 0; i < sources.size(); ++i) prepared = Write(sources[i], actual_bytes) && ShaDigest(sources[i], expected[i]) && prepared;
    Check(prepared, "ordinary 8MiB sources and original SHA256");
    if (!prepared) return 1;
    auto& budget = SharedTransferDeviceBudget();
    auto devices = std::make_shared<TransferDevicePair>(
        TransferDeviceIdentity{{L"admission-source-" + std::wstring(name)}, 4},
        TransferDeviceIdentity{{L"admission-target-" + std::wstring(name)}, 4}, budget);
    const auto job = [&](size_t index, const wchar_t* group) {
        SmallCopyJob item{index, sources[index % 2], (root / (std::wstring(group) + std::to_wstring(index) + L".bin")).wstring(),
            FILE_ATTRIBUTE_NORMAL, false, actual_bytes};
        item.verify = true; item.admission_bytes = weight; item.devices = devices;
        return item;
    };
    const auto check_results = [&](const std::vector<SmallCopyResult>& results, const wchar_t* group, size_t expected_count) {
        Check(results.size() == expected_count, "weighted jobs all return results");
        for (const auto& result : results) {
            const auto path = (root / (std::wstring(group) + std::to_wstring(result.index) + L".bin")).wstring();
            std::array<unsigned char, 32> copied{}, unchanged{};
            Check(result.ran && SUCCEEDED(result.hr) && result.bytes == actual_bytes && result.verification &&
                result.verification->Success() && result.verification->source_bytes == actual_bytes &&
                result.verification->target_bytes == actual_bytes, "result and verification report actual bytes, not admission weight");
            Check(ShaDigest(path, copied) && copied == expected[result.index % 2] && ExactLength(path, actual_bytes) &&
                ShaDigest(sources[result.index % 2], unchanged) && unchanged == expected[result.index % 2],
                "weighted job output complete SHA256 and source unchanged");
        }
    };
    {
        Control control; control.pause = true;
        SmallCopyPool pool(2, control.cancel, control.pause, control.mutex, control.cv);
        const auto first = job(0, L"completed");
        auto legacy_weight = first; legacy_weight.admission_bytes = 0;
        Check(legacy_weight.AdmissionBytes() == actual_bytes, "zero admission override retains actual length policy");
        Check(first.AdmissionBytes() == weight && first.bytes == actual_bytes, "admission weight is distinct from file length");
        Check(pool.HasRoom(first.AdmissionBytes()), "first weight admitted"); pool.Submit(first);
        Check(pool.HasRoom(weight), "second 128MiB weight admitted"); pool.Submit(job(1, L"completed"));
        Check(pool.Outstanding() == 2 && !pool.HasRoom(weight), "paused two jobs reserve full 256MiB; third blocked");
        Check(budget.Active() == 0, "paused queued jobs do not hold shared device permits");
        control.pause = false; control.cv.notify_all();
        check_results(Drain(pool), L"completed", 2);
        Check(budget.Active() == 0 && pool.HasRoom(weight), "completion returns shared permits and Take returns pool weight");
        pool.Submit(job(2, L"refill"));
        check_results(Drain(pool), L"refill", 1);
        Check(budget.Active() == 0, "refill returns shared device permits");
    }
    {
        Control control; control.pause = true;
        SmallCopyPool pool(2, control.cancel, control.pause, control.mutex, control.cv);
        pool.Submit(job(3, L"cancelled")); pool.Submit(job(4, L"cancelled"));
        Check(!pool.HasRoom(weight), "cancel fixture holds full admission weight");
        control.cancel = true; control.cv.notify_all();
        const auto cancelled = Drain(pool);
        Check(cancelled.size() == 2 && std::all_of(cancelled.begin(), cancelled.end(), [](const auto& result) { return !result.ran || FAILED(result.hr); }),
            "cancelled weighted jobs never report successful execution");
        Check(pool.Outstanding() == 0 && pool.HasRoom(weight) && budget.Active() == 0, "cancellation and Take return pool weight and permits");
        control.cancel = false; control.pause = false; control.cv.notify_all();
        pool.Submit(job(5, L"aftercancel"));
        check_results(Drain(pool), L"aftercancel", 1);
    }
    {
        Control control; control.pause = true;
        SmallCopyPool pool(2, control.cancel, control.pause, control.mutex, control.cv);
        pool.Submit(job(6, L"stopped")); pool.Submit(job(7, L"stopped"));
        Check(!pool.HasRoom(weight), "stop fixture holds full admission weight");
        pool.Stop();
        const auto stopped = Drain(pool);
        Check(stopped.size() == 2 && std::all_of(stopped.begin(), stopped.end(), [](const auto& result) { return !result.ran || FAILED(result.hr); }) &&
            pool.Outstanding() == 0 && budget.Active() == 0, "Stop drains weighted reservations without shared permit leak");
        Check(!pool.HasRoom(weight), "stopped pool remains closed to new work");
    }
    auto first_lease = budget.TryAcquire(devices->source, devices->target, weight);
    auto second_lease = budget.TryAcquire(devices->source, devices->target, weight);
    Check(first_lease && second_lease && !budget.TryAcquire(devices->source, devices->target, 1), "full shared byte capacity restored after completion cancel and Stop");
    first_lease.Reset(); second_lease.Reset();
    Check(budget.Active() == 0, "shared budget empty after test");
    if (!failures && root.parent_path() == base && root.filename().wstring().starts_with(L"pool_admission_")) {
        fs::remove_all(root, error); Check(!error, "admission fixture cleanup");
    } else printf("[DIAG] retained fixture=%ls\n", root.c_str());
    return failures ? 1 : 0;
}
int CompatibilityOnly() {
    namespace fs = std::filesystem;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) { Check(false, "compatibility GUID"); return 1; }
    wchar_t guid_text[40]{};
    StringFromGUID2(guid, guid_text, 40);
    const auto base = fs::absolute(L"bench_data").lexically_normal();
    const auto root = base / (std::wstring(L"pool_compatibility_") + guid_text);
    std::error_code error;
    const bool created = fs::create_directories(root, error);
    Check(created && !error, "unique compatibility fixture");
    if (!created || error) return 1;
    wchar_t volume[MAX_PATH]{}, filesystem[MAX_PATH]{};
    DWORD flags = 0;
    const bool volume_ok = GetVolumePathNameW(root.c_str(), volume, MAX_PATH) &&
        GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, &flags, filesystem, MAX_PATH);
    Check(volume_ok, "query compatibility volume capabilities");
    if (!volume_ok) return 1;
    const bool ntfs = _wcsicmp(filesystem, L"NTFS") == 0;
    constexpr uint64_t logical_bytes = 8ull << 20;
    unsigned executed = 0, skipped = 0;
    for (int kind = 0; kind < 3; ++kind) {
        const wchar_t* label = kind == 0 ? L"sparse" : kind == 1 ? L"compressed" : L"ads";
        const DWORD capability = kind == 0 ? FILE_SUPPORTS_SPARSE_FILES : kind == 1 ? FILE_FILE_COMPRESSION : FILE_NAMED_STREAMS;
        if ((kind < 2 && !ntfs) || !(flags & capability)) {
            printf("[SKIP] %ls: filesystem=%ls capability=0x%08lx unavailable\n", label, filesystem, capability);
            ++skipped; continue;
        }
        ++executed;
        const auto source = (root / (std::wstring(label) + L"-source.bin")).wstring();
        const auto staged = (root / (std::wstring(label) + L"-staged.bin")).wstring();
        const auto final = (root / (std::wstring(label) + L"-final.bin")).wstring();
        bool fixture_ok = false;
        if (kind == 0) {
            HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD returned = 0;
                LARGE_INTEGER end{}; end.QuadPart = logical_bytes;
                fixture_ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
                    SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
                std::array<unsigned char, 4096> block{};
                for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<unsigned char>((i * 37 + 11) & 255);
                for (const uint64_t offset : {uint64_t{0}, logical_bytes - block.size()}) {
                    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
                    DWORD written = 0;
                    fixture_ok = fixture_ok && SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
                        WriteFile(file, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) && written == block.size();
                }
                CloseHandle(file);
            }
        } else {
            fixture_ok = Write(source, logical_bytes);
            if (fixture_ok && kind == 1) {
                HANDLE file = CreateFileW(source.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                USHORT format = COMPRESSION_FORMAT_DEFAULT; DWORD returned = 0;
                fixture_ok = file != INVALID_HANDLE_VALUE && DeviceIoControl(file, FSCTL_SET_COMPRESSION,
                    &format, sizeof(format), nullptr, 0, &returned, nullptr);
                if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            }
            if (fixture_ok && kind == 2) fixture_ok = Write(source + L":pulse_compatibility", 8193);
        }
        printf("[CASE] %ls\n", label);
        Check(fixture_ok, "prepare advertised filesystem feature");
        if (!fixture_ok) continue;
        const DWORD attributes = GetFileAttributesW(source.c_str());
        const DWORD feature_attribute = kind == 0 ? FILE_ATTRIBUTE_SPARSE_FILE : kind == 1 ? FILE_ATTRIBUTE_COMPRESSED : 0;
        Check(attributes != INVALID_FILE_ATTRIBUTES && (!feature_attribute || (attributes & feature_attribute)), "source feature is present");
        std::array<unsigned char, 32> before{}, after{}, copied{}, ads_before{}, ads_after{}, ads_copied{};
        const bool baseline_ok = ShaDigest(source, before) && (kind != 2 || ShaDigest(source + L":pulse_compatibility", ads_before));
        Check(baseline_ok, "capture source and optional ADS SHA256 before copy");
        if (!baseline_ok) continue;
        Control control;
        SmallCopyPool pool(2, control.cancel, control.pause, control.mutex, control.cv);
        SmallCopyJob job{0, source, staged, attributes, false, logical_bytes};
        job.verify = true;
        pool.Submit(std::move(job));
        const auto results = Drain(pool);
        const bool verified = results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].owned_output &&
            results[0].verification && results[0].verification->Success() &&
            results[0].verification->source_bytes == logical_bytes && results[0].verification->target_bytes == logical_bytes;
        Check(verified, "actual pool SHA256 verification and owned output");
        if (!verified) { if (!results.empty()) printf("[DIAG] hr=0x%08lx\n", static_cast<unsigned long>(results[0].hr)); continue; }
        DWORD commit_error = ERROR_SUCCESS;
        const bool committed = CommitVerifiedOutput(staged, final, *results[0].owned_output, commit_error);
        Check(committed && commit_error == ERROR_SUCCESS, "owned handle commits compatible output");
        if (!committed) { printf("[DIAG] commit=%lu\n", commit_error); continue; }
        Check(ShaDigest(source, after) && ShaDigest(final, copied) && before == after && before == copied &&
            ExactLength(final, logical_bytes), "source unchanged and final complete SHA256 match");
        if (kind == 2) Check(ShaDigest(source + L":pulse_compatibility", ads_after) &&
            ShaDigest(final + L":pulse_compatibility", ads_copied) && ads_before == ads_after && ads_before == ads_copied &&
            ExactLength(final + L":pulse_compatibility", 8193), "ADS retained with exact length and SHA256; source ADS unchanged");
        const DWORD source_after = GetFileAttributesW(source.c_str());
        const DWORD target_after = GetFileAttributesW(final.c_str());
        Check(source_after != INVALID_FILE_ATTRIBUTES && (!feature_attribute || (source_after & feature_attribute)), "source feature attribute unchanged");
        Check(target_after != INVALID_FILE_ATTRIBUTES && GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES, "published output present and staging consumed");
        const auto baseline = (root / (std::wstring(label) + L"-baseline.bin")).wstring();
        COPYFILE2_EXTENDED_PARAMETERS baseline_parameters{};
        baseline_parameters.dwSize = sizeof(baseline_parameters);
        baseline_parameters.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK;
        const HRESULT baseline_hr = CopyFile2(source.c_str(), baseline.c_str(), &baseline_parameters);
        Check(SUCCEEDED(baseline_hr), "direct CopyFile2 compatibility baseline succeeds");
        if (SUCCEEDED(baseline_hr)) {
            const DWORD baseline_attributes = GetFileAttributesW(baseline.c_str());
            constexpr DWORD layout_mask = FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED;
            Check(baseline_attributes != INVALID_FILE_ATTRIBUTES && target_after != INVALID_FILE_ATTRIBUTES &&
                (baseline_attributes & layout_mask) == (target_after & layout_mask),
                "pool sparse/compressed attributes match same-directory direct CopyFile2");
            std::array<unsigned char, 32> baseline_hash{}, baseline_ads{};
            Check(ShaDigest(baseline, baseline_hash) && baseline_hash == before && ExactLength(baseline, logical_bytes),
                "direct baseline complete SHA256 matches original source");
            if (kind == 2) Check(ShaDigest(baseline + L":pulse_compatibility", baseline_ads) && baseline_ads == ads_before &&
                ExactLength(baseline + L":pulse_compatibility", 8193), "direct baseline ADS content and length match");
            Check(ShaDigest(source, after) && after == before &&
                (kind != 2 || (ShaDigest(source + L":pulse_compatibility", ads_after) && ads_after == ads_before)),
                "source unchanged after both pool and direct baseline");
            printf("[DIAG] %ls source_attributes=0x%08lx pool_attributes=0x%08lx baseline_attributes=0x%08lx\n",
                label, source_after, target_after, baseline_attributes);
        } else printf("[DIAG] direct baseline hr=0x%08lx\n", static_cast<unsigned long>(baseline_hr));
    }
    printf("[SUMMARY] compatibility executed=%u skipped=%u failures=%d\n", executed, skipped, failures);
    if (!failures && root.parent_path() == base && root.filename().wstring().starts_with(L"pool_compatibility_")) {
        fs::remove_all(root, error); Check(!error, "compatibility fixture cleanup");
    } else printf("[DIAG] retained fixture=%ls\n", root.c_str());
    return failures ? 1 : (executed == 0 ? 2 : 0);
}
int BudgetWakeBench() {
    namespace fs = std::filesystem;
    // A stuck native call must not leave this standalone test running forever.
    struct Watchdog {
        std::mutex mutex;
        std::condition_variable cv;
        bool finished = false;
        std::thread thread;
        Watchdog() : thread([this] {
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(120), [&] { return finished; })) {
                fprintf(stderr, "[FAIL] budget-wake benchmark exceeded 120-second safety deadline\n");
                fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }) {}
        ~Watchdog() {
            { std::lock_guard lock(mutex); finished = true; }
            cv.notify_all(); thread.join();
        }
    } watchdog;
    struct WakeEnvironment {
        std::wstring previous;
        bool existed = false;
        WakeEnvironment() {
            const DWORD size = GetEnvironmentVariableW(L"PULSE_TRANSFER_BUDGET_WAKE", nullptr, 0);
            existed = size != 0;
            if (existed) {
                previous.resize(size);
                previous.resize(GetEnvironmentVariableW(L"PULSE_TRANSFER_BUDGET_WAKE", previous.data(), size));
            }
        }
        ~WakeEnvironment() {
            SetEnvironmentVariableW(L"PULSE_TRANSFER_BUDGET_WAKE", existed ? previous.c_str() : nullptr);
        }
    } wake_environment;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) { Check(false, "budget-wake fixture GUID"); return 1; }
    wchar_t guid_text[40]{}; StringFromGUID2(guid, guid_text, 40);
    const auto base = fs::absolute(L"bench_data").lexically_normal();
    const auto root = base / (std::wstring(L"pool_budget_wake_") + guid_text);
    std::error_code error;
    Check(fs::create_directories(root, error) && !error, "create isolated budget-wake fixture");
    if (failures) return 1;
    constexpr size_t pools = 2, files_per_pool = 512;
    const std::array<uint64_t, pools> sizes{4096, 4097};
    std::array<std::wstring, pools> sources;
    std::array<std::array<unsigned char, 32>, pools> expected{};
    for (size_t i = 0; i < pools; ++i) {
        sources[i] = (root / (L"source-" + std::to_wstring(i) + L".bin")).wstring();
        Check(Write(sources[i], sizes[i]) && ShaDigest(sources[i], expected[i]), "prepare source and complete SHA256");
    }
    // Three pairs, reverse the middle pair to avoid a fixed winner by run order.
    constexpr std::array<unsigned, 6> modes{0, 1, 1, 0, 0, 1};
    for (size_t round = 0; round < modes.size() && !failures; ++round) {
        Check(SetEnvironmentVariableW(L"PULSE_TRANSFER_BUDGET_WAKE", modes[round] ? L"1" : L"0") != 0,
            "set wake mode before constructing shared budget");
        const auto destination = root / (L"round-" + std::to_wstring(round));
        Check(fs::create_directory(destination, error) && !error, "create round destination");
        if (failures) break;
        TransferDeviceBudget budget;
        const TransferDeviceIdentity device{{L"wake-bench:one-shared-device"}, 1};
        auto pair = std::make_shared<TransferDevicePair>(device, device, budget);
        auto blocker = budget.TryAcquire(device, device, 1);
        Check(static_cast<bool>(blocker), "hold start gate device permit");
        std::array<Control, pools> controls;
        std::array<std::shared_ptr<SmallCopyPhaseProfile>, pools> profiles;
        std::array<std::unique_ptr<SmallCopyPool>, pools> workers;
        std::array<std::vector<std::wstring>, pools> targets;
        for (size_t i = 0; i < pools; ++i) {
            profiles[i] = std::make_shared<SmallCopyPhaseProfile>();
            workers[i] = std::make_unique<SmallCopyPool>(2, controls[i].cancel, controls[i].pause,
                controls[i].mutex, controls[i].cv, false, profiles[i], TransferDeviceBudget::CreateTask());
            targets[i].reserve(files_per_pool);
            for (size_t j = 0; j < files_per_pool; ++j) {
                targets[i].push_back((destination / (std::to_wstring(i) + L"-" + std::to_wstring(j) + L".bin")).wstring());
                SmallCopyJob job{j, sources[i], targets[i].back(), FILE_ATTRIBUTE_NORMAL, false, sizes[i]};
                job.devices = pair;
                workers[i]->Submit(std::move(job));
            }
        }
        const auto gate_deadline = GetTickCount64() + 5000;
        while (budget.Waiting() != pools * 2 && GetTickCount64() < gate_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(budget.Waiting() == pools * 2, "all four workers contend across two task groups before release");
        std::array<size_t, pools> completed{};
        std::array<std::vector<bool>, pools> seen;
        for (auto& flags : seen) flags.resize(files_per_pool);
        const auto started = std::chrono::steady_clock::now();
        blocker.Reset();
        const auto deadline = GetTickCount64() + 20000;
        bool outstanding = true;
        while (outstanding && GetTickCount64() < deadline && !failures) {
            outstanding = false;
            for (size_t i = 0; i < pools; ++i) {
                for (const auto& result : workers[i]->Take(false)) {
                    if (!result.ran || FAILED(result.hr) || result.bytes != sizes[i] ||
                        result.index >= files_per_pool || seen[i][result.index]) {
                        Check(false, "budget-wake actual copy result valid and unique");
                        continue;
                    }
                    seen[i][result.index] = true; ++completed[i];
                }
                outstanding = outstanding || workers[i]->Outstanding() != 0;
            }
            if (outstanding) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double copy_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (outstanding || failures) {
            for (size_t i = 0; i < pools; ++i) {
                controls[i].cancel = true; workers[i]->Stop(); controls[i].cv.notify_all();
            }
        }
        for (auto& worker : workers) worker.reset();
        Check(!outstanding && completed[0] == files_per_pool && completed[1] == files_per_pool,
            "both tasks complete before deadline");
        Check(budget.Active() == 0 && budget.Waiting() == 0, "round releases all permits and tickets");
        size_t verified = 0;
        const auto hash_started = std::chrono::steady_clock::now();
        if (!failures) for (size_t i = 0; i < pools; ++i) {
            for (const auto& target : targets[i]) {
                std::array<unsigned char, 32> digest{};
                if (!ExactLength(target, sizes[i]) || !ShaDigest(target, digest) || digest != expected[i]) {
                    Check(false, "complete destination SHA256 and size match source"); break;
                }
                ++verified;
            }
            std::array<unsigned char, 32> digest{};
            Check(ExactLength(sources[i], sizes[i]) && ShaDigest(sources[i], digest) && digest == expected[i],
                "source remains unchanged after round");
        }
        const double hash_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - hash_started).count();
        Check(verified == pools * files_per_pool, "every copied file fully SHA256 verified");
        const uint64_t wait_us = (profiles[0]->workers.budget_wait_ns + profiles[1]->workers.budget_wait_ns) / 1000;
        printf("[BENCH] wake=%u round=%zu tasks=2 threads=4 files=%zu copy_ms=%.3f sha_ms=%.3f verified=%zu budget_wait_us=%llu\n",
            modes[round], round, pools * files_per_pool, copy_ms, hash_ms, verified,
            static_cast<unsigned long long>(wait_us));
        fflush(stdout);
    }
    if (!failures && root.parent_path() == base && root.filename().wstring().starts_with(L"pool_budget_wake_")) {
        fs::remove_all(root, error);
        Check(!error, "budget-wake isolated fixture cleanup");
    } else printf("[DIAG] retained fixture=%ls\n", root.c_str());
    return failures ? 1 : 0;
}

}
int wmain(int argc, wchar_t** argv) {
    struct FairEnvironment {
        std::wstring previous;
        bool existed = false;
        FairEnvironment() {
            const DWORD size = GetEnvironmentVariableW(L"PULSE_FAIR_TRANSFER_BUDGET", nullptr, 0);
            existed = size != 0;
            if (existed) {
                previous.resize(size);
                const DWORD used = GetEnvironmentVariableW(L"PULSE_FAIR_TRANSFER_BUDGET", previous.data(), size);
                previous.resize(used);
            }
            SetEnvironmentVariableW(L"PULSE_FAIR_TRANSFER_BUDGET", L"1");
        }
        ~FairEnvironment() {
            SetEnvironmentVariableW(L"PULSE_FAIR_TRANSFER_BUDGET", existed ? previous.c_str() : nullptr);
        }
    } fair_environment;
    if (argc == 2 && wcscmp(argv[1], L"--budget-wake-bench") == 0) return BudgetWakeBench();
    if (argc == 2 && wcscmp(argv[1], L"--compatibility-only") == 0) return CompatibilityOnly();
    if (argc == 2 && wcscmp(argv[1], L"--admission-only") == 0) return AdmissionOnly();
    if (argc != 1) { fprintf(stderr, "unknown test arguments\n"); return 2; }
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"pool_control_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(root);
    const auto source = (root / L"source.bin").wstring();
    constexpr uint64_t size = 128ull << 20;
    Check(Write(source, size), "create real medium source");
    const uint64_t source_hash = Hash(source);
    {
        Control c;
        SmallCopyPool pool(2, c.cancel, c.pause, c.mutex, c.cv);
        const auto dest = (root / L"copied.bin").wstring();
        pool.Submit({0, source, dest, FILE_ATTRIBUTE_NORMAL, false, size});
        auto results = Drain(pool);
        Check(results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].bytes == size,
              "medium copy returns final size");
        Check(Hash(dest) == source_hash, "medium content hash");
        Check(pool.TakeProgress().empty(), "completed progress cannot reappear");
        pool.Submit({1, source, dest, FILE_ATTRIBUTE_NORMAL, false, size});
        results = Drain(pool);
        Check(results.size() == 1 && FAILED(results[0].hr) && Hash(dest) == source_hash,
              "target race preserves existing content");
    }
    for (const uint64_t boundary : {(1ull << 20) - 1, 1ull << 20, (1ull << 20) + 1, 3ull << 20}) {
        const auto input = (root / (std::to_wstring(boundary) + L"-source.bin")).wstring();
        const auto dest = (root / (std::to_wstring(boundary) + L"-dest.bin")).wstring();
        Check(Write(input, boundary), "create boundary fixture");
        Control c;
        SmallCopyPool pool(2, c.cancel, c.pause, c.mutex, c.cv);
        pool.Submit({0, input, dest, FILE_ATTRIBUTE_NORMAL, true, boundary});
        const auto results = Drain(pool);
        Check(results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].bytes == boundary &&
              results[0].move_pending && !results[0].source_removed &&
              Hash(input) == Hash(dest), "boundary move preserves source until coordinator commits");
    }
    {
        Control c;
        c.pause = true;
        SmallCopyPool pool(1, c.cancel, c.pause, c.mutex, c.cv);
        pool.Submit({0, source, (root / L"resume.bin").wstring(), FILE_ATTRIBUTE_NORMAL, false, size});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Check(pool.Take(false).empty(), "paused worker does not complete");
        c.pause = false;
        c.cv.notify_all();
        auto results = Drain(pool);
        Check(results.size() == 1 && SUCCEEDED(results[0].hr) &&
              Hash((root / L"resume.bin").wstring()) == source_hash, "pause resume content hash");
    }
    {
        Control c;
        c.pause = true;
        SmallCopyPool pool(1, c.cancel, c.pause, c.mutex, c.cv);
        pool.Submit({0, source, (root / L"stopped.bin").wstring(), FILE_ATTRIBUTE_NORMAL, false, size});
        pool.Submit({1, source, (root / L"queued.bin").wstring(), FILE_ATTRIBUTE_NORMAL, false, size});
        Check(!pool.HasRoom(1), "byte budget provides backpressure");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        pool.Stop();
        auto results = Drain(pool);
        Check(results.size() == 2 && !results[0].ran && !results[1].ran,
              "Stop wakes paused worker and drops queue");
    }
    for (bool stop : {false, true}) {
        Control c;
        SmallCopyPool pool(1, c.cancel, c.pause, c.mutex, c.cv);
        const auto dest = (root / (stop ? L"active_stop.bin" : L"active_resume.bin")).wstring();
        pool.Submit({0, source, dest, FILE_ATTRIBUTE_NORMAL, false, size});
        bool active = false;
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            const auto snapshots = pool.TakeProgress();
            if (!snapshots.empty() && snapshots[0].bytes > 0 && snapshots[0].bytes < snapshots[0].total) {
                active = true;
                c.pause = true;
                break;
            }
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto premature = pool.Take(false);
        Check(active && premature.empty(), "active callback copy pauses before completion");
        if (stop) pool.Stop();
        else { c.pause = false; c.cv.notify_all(); }
        const auto results = Drain(pool);
        if (stop) {
            Check(results.size() == 1 && FAILED(results[0].hr), "Stop wakes active paused callback");
            Check(GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES, "Stop cleans partial copy");
        } else {
            Check(results.size() == 1 && SUCCEEDED(results[0].hr) && Hash(dest) == source_hash,
                  "active callback resume content hash");
        }
    }
    {
        Control c;
        SmallCopyPool pool(1, c.cancel, c.pause, c.mutex, c.cv);
        const auto dest = (root / L"cancel.bin").wstring();
        pool.Submit({0, source, dest, FILE_ATTRIBUTE_NORMAL, false, size});
        bool active = false;
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            auto snapshots = pool.TakeProgress();
            if (!snapshots.empty() && snapshots[0].bytes > 0 && snapshots[0].bytes < snapshots[0].total) {
                active = true;
                c.cancel = true;
                c.cv.notify_all();
                break;
            }
            if (!pool.Outstanding()) break;
            std::this_thread::yield();
        }
        auto results = Drain(pool);
        Check(active && results.size() == 1 && FAILED(results[0].hr), "cancel active callback copy");
        Check(GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES && Hash(source) == source_hash,
              "cancel cleans partial destination and keeps source");
    }
    {
        Control c;
        SmallCopyPool pool(2, c.cancel, c.pause, c.mutex, c.cv);
        const auto input = (root / L"verify_source.bin").wstring();
        const auto dest = (root / L"verify_stage.bin").wstring();
        Check(Write(input, 3ull << 20), "create pooled verification fixture");
        auto stamp = std::make_shared<VerificationStamp>();
        DWORD stamp_error = 0;
        Check(CaptureVerificationStamp(input, *stamp, stamp_error), "capture expected source identity");
        SmallCopyJob job{0, input, dest, FILE_ATTRIBUTE_NORMAL, true, 3ull << 20};
        job.verify = true;
        job.expected_source = stamp;
        pool.Submit(job);
        auto results = Drain(pool);
        Check(results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].verification &&
              results[0].verification->Success() && results[0].move_pending &&
              results[0].verification->source_bytes == (3ull << 20) &&
              results[0].verification->target_bytes == (3ull << 20) && Hash(input) == Hash(dest),
              "pooled verification precedes move commit and reads both sides");
        Check(results.size() == 1 && results[0].copy_completed && results[0].owned_output,
              "copy callback captures actual output identity");
        if (results.size() == 1 && results[0].owned_output) {
            const auto saved = (root / L"verify_stage_saved.bin").wstring();
            Check(MoveFileExW(dest.c_str(), saved.c_str(), 0) != FALSE && Write(dest, 17),
                  "replace output name with a different file");
            DWORD cleanup_error = 0;
            const auto rejected_commit = (root / L"rejected_commit.bin").wstring();
            Check(!CommitVerifiedOutput(dest, rejected_commit, *results[0].owned_output, cleanup_error) &&
                  cleanup_error == ERROR_FILE_INVALID && GetFileAttributesW(rejected_commit.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "owned commit refuses externally replaced staging");
            Check(!DeleteVerifiedOutput(dest, *results[0].owned_output, cleanup_error) &&
                  cleanup_error == ERROR_FILE_INVALID && GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES,
                  "owned cleanup refuses replacement identity");
            Check(DeleteFileW(dest.c_str()) && MoveFileExW(saved.c_str(), dest.c_str(), 0),
                  "restore owned test output");
            Check(SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
                  "mark owned test output read only");
            Check(DeleteVerifiedOutput(dest, *results[0].owned_output, cleanup_error) &&
                  GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "owned cleanup deletes read-only output by pinned handle");
            Check(DeleteVerifiedOutput(dest, *results[0].owned_output, cleanup_error) && cleanup_error == ERROR_SUCCESS,
                  "owned cleanup accepts an output already removed by CopyFile2");
        }
        job.index = 3;
        pool.Submit(job);
        results = Drain(pool);
        if (results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].owned_output) {
            const auto final = (root / L"verified_final.bin").wstring();
            Check(Write(final, 17), "create competing final destination");
            const auto competing_hash = Hash(final);
            DWORD commit_error = 0;
            Check(!CommitVerifiedOutput(dest, final, *results[0].owned_output, commit_error) &&
                  Hash(final) == competing_hash && GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES,
                  "handle commit never replaces competing destination");
            Check(DeleteFileW(final.c_str()) != FALSE, "remove competing fixture");
            Check(SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
                  "mark staging read only before commit");
            Check(CommitVerifiedOutput(dest, final, *results[0].owned_output, commit_error) && !commit_error &&
                  Hash(final) == Hash(input) && GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES &&
                  (GetFileAttributesW(final.c_str()) & FILE_ATTRIBUTE_READONLY),
                  "handle commit publishes verified content and preserves read-only attribute");
            Check(SetFileAttributesW(final.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE,
                  "restore fixture attributes for cleanup");
        } else Check(false, "prepare handle commit fixture");
        ++stamp->bytes;
        const auto refused = (root / L"verify_refused.bin").wstring();
        job.index = 1;
        job.destination = refused;
        pool.Submit(job);
        results = Drain(pool);
        Check(results.size() == 1 && FAILED(results[0].hr) && results[0].verification &&
              results[0].verification->error == VerificationError::SourceChanged &&
              !results[0].move_pending && GetFileAttributesW(refused.c_str()) == INVALID_FILE_ATTRIBUTES,
              "stale source stamp fails before writing target");
        job.index = 2;
        job.source = (root / L"missing-source.bin").wstring();
        job.expected_source.reset();
        pool.Submit(job);
        results = Drain(pool);
        Check(results.size() == 1 && FAILED(results[0].hr) && results[0].verification &&
              results[0].verification->error == VerificationError::SourceStat &&
              GetFileAttributesW(refused.c_str()) == INVALID_FILE_ATTRIBUTES,
              "source stamp failure never starts copy");
    }
    for (int mode = 0; mode < 3; ++mode) {
        Control c;
        TransferDeviceBudget shared_budget;
        const TransferDeviceIdentity device{{L"test:shared-device"}, 1};
        auto devices = std::make_shared<TransferDevicePair>(device, device, shared_budget);
        auto blocker = shared_budget.TryAcquire(device, device, 1);
        SmallCopyPool pool(2, c.cancel, c.pause, c.mutex, c.cv);
        const auto destination = (root / (L"device-wait-" + std::to_wstring(mode) + L".bin")).wstring();
        SmallCopyJob job{0, source, destination, FILE_ATTRIBUTE_NORMAL, false, size};
        job.devices = devices;
        job.verify = true;
        pool.Submit(job);
        const auto ticket_deadline = GetTickCount64() + 5000;
        while (!shared_budget.Waiting() && GetTickCount64() < ticket_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Check(shared_budget.Waiting() == 1, "worker registers persistent shared-budget ticket");
        Check(shared_budget.Active() == 1 && GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES,
              "queued worker waits without acquiring an unavailable shared permit");
        if (mode == 0) {
            c.pause = true;
            c.cv.notify_all();
            const auto pause_deadline = GetTickCount64() + 5000;
            while (shared_budget.Waiting() && GetTickCount64() < pause_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            Check(shared_budget.Waiting() == 0, "pause unregisters shared-budget ticket");
            blocker.Reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Check(shared_budget.Active() == 0 && GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "paused budget waiter does not start when permit becomes available");
            c.pause = false;
            c.cv.notify_all();
            bool copied_under_lease = false, verified_under_lease = false;
            const auto deadline = GetTickCount64() + 15000;
            while (GetTickCount64() < deadline) {
                const auto snapshots = pool.TakeProgress();
                for (const auto& snapshot : snapshots) {
                    if (snapshot.verifying && shared_budget.Active()) verified_under_lease = true;
                    if (!snapshot.verifying && snapshot.bytes && shared_budget.Active()) copied_under_lease = true;
                }
                if (verified_under_lease && !shared_budget.Active()) break;
                std::this_thread::yield();
            }
            Check(copied_under_lease && verified_under_lease,
                  "same device lease covers actual copy and verification work");
            Check(shared_budget.Active() == 0 && pool.Outstanding() == 1,
                  "completed worker returns device permit before coordinator Take");
            const auto results = Drain(pool);
            Check(results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].verification &&
                  results[0].verification->Success(), "budget wait resumes into verified copy");
        } else {
            if (mode == 1) c.cancel = true;
            else { c.pause = true; pool.Stop(); }
            c.cv.notify_all();
            const auto results = Drain(pool);
            Check(results.size() == 1 && !results[0].ran &&
                  GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES,
                  mode == 1 ? "cancel wakes budget waiter without writing" : "Stop wakes paused budget waiter without writing");
            Check(shared_budget.Active() == 1, "dropped waiter leaves external permit unchanged");
            Check(shared_budget.Waiting() == 0, "cancel or Stop unregisters worker ticket before result");
            blocker.Reset();
            Check(shared_budget.Active() == 0, "all budget permits returned");
        }
    }
    const auto check_verified_commit = [&](const std::wstring& input, const std::wstring& staged,
                                           const std::wstring& final, uint64_t bytes, bool long_path) {
        Check(Write(input, bytes), long_path ? "create extended long-path source" : "create empty source");
        Control c;
        SmallCopyPool pool(1, c.cancel, c.pause, c.mutex, c.cv);
        SmallCopyJob job{0, input, staged, FILE_ATTRIBUTE_NORMAL, false, bytes};
        job.verify = true;
        job.staged = true;
        pool.Submit(job);
        const auto results = Drain(pool);
        const bool verified = results.size() == 1 && SUCCEEDED(results[0].hr) && results[0].copy_completed &&
            results[0].owned_output && results[0].verification && results[0].verification->Success() &&
            results[0].bytes == bytes && results[0].verification->source_bytes == bytes &&
            results[0].verification->target_bytes == bytes;
        Check(verified, long_path ? "long-path CopyFile2 verifies and captures actual output identity" :
                                   "empty CopyFile2 verifies and captures callback output identity");
        if (!verified) {
            if (!results.empty()) printf("[DIAG] boundary hr=0x%08lx owned=%d copy_completed=%d\n",
                static_cast<unsigned long>(results[0].hr), results[0].owned_output ? 1 : 0,
                results[0].copy_completed ? 1 : 0);
            return;
        }
        DWORD commit_error = ERROR_SUCCESS;
        const bool committed = CommitVerifiedOutput(staged, final, *results[0].owned_output, commit_error);
        Check(committed && !commit_error, long_path ? "extended long-path handle rename commits" :
                                                    "empty verified file commits by fixed handle");
        if (!committed) printf("[DIAG] boundary commit error=%lu path_chars=%zu\n", commit_error, final.size());
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const bool measured = GetFileAttributesExW(final.c_str(), GetFileExInfoStandard, &info) != FALSE;
        const uint64_t actual = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        Check(committed && measured && actual == bytes && Hash(input) == Hash(final) &&
              GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES,
              long_path ? "long-path committed content and size match source" : "empty committed content and size match source");
    };
    check_verified_commit((root / L"empty-source.bin").wstring(), (root / L"empty-stage.bin").wstring(),
                          (root / L"empty-final.bin").wstring(), 0, false);
    {
        const auto extended_root = ExtendedPath(root);
        std::wstring directory = extended_root;
        bool created = true;
        for (unsigned level = 0; level < 5; ++level) {
            directory += L"\\long_path_component_" + std::to_wstring(level) + std::wstring(44, L'x');
            // Every component is constant and appended beneath this test's own root.
            if (!directory.starts_with(extended_root + L"\\") || !CreateDirectoryW(directory.c_str(), nullptr)) {
                printf("[DIAG] long directory creation error=%lu\n", GetLastError());
                created = false;
                break;
            }
        }
        Check(created && directory.size() > 260, "create isolated extended path beyond MAX_PATH");
        if (created && directory.size() > 260)
            check_verified_commit(directory + L"\\source.bin", directory + L"\\staging.bin",
                                  directory + L"\\final.bin", (3ull << 20) + 17, true);
    }
    std::error_code error;
    const auto fixture_base = std::filesystem::absolute(L"bench_data").lexically_normal();
    const auto resolved_root = root.lexically_normal();
    if (resolved_root.parent_path() == fixture_base && resolved_root.filename().wstring().starts_with(L"pool_control_"))
        std::filesystem::remove_all(std::filesystem::path(ExtendedPath(resolved_root)), error);
    else error = std::make_error_code(std::errc::permission_denied);
    Check(!error, "isolated fixture cleanup");
    return failures ? 1 : 0;
}
