// Standalone experiment only: no production backend registration.
// Build with verification_worker.cpp, bcrypt.lib and ole32.lib (C++20).
// Usage: --checks-only | [--pairs 3] [--size-mib 65|256] [--blocks 2|4]
//        [--block-kib 256|1024|4096]. Default runs both sizes, three alternating pairs.
#include "../ops/verification_worker.h"
#include <windows.h>
#include <winioctl.h>
#include <cstdlib>
#include <cwchar>
#include <objbase.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Digest = std::array<unsigned char, 32>;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Hash {
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    bool good = false;
    Hash() {
        DWORD bytes = 0, returned = 0;
        if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&bytes),
                              sizeof(bytes), &returned, 0) < 0) return;
        object.resize(bytes);
        good = BCryptCreateHash(provider, &hash, object.data(), bytes, nullptr, 0, 0) >= 0;
    }
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (provider) BCryptCloseAlgorithmProvider(provider, 0); }
    bool Add(const unsigned char* data, DWORD bytes) {
        return good && BCryptHashData(hash, const_cast<PUCHAR>(data), bytes, 0) >= 0;
    }
    bool Finish(Digest& output) { return good && BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) >= 0; }
};
struct Result {
    bool ok = false, unsupported = false, cancelled = false;
    DWORD error = ERROR_SUCCESS;
    uint64_t source_read = 0, target_read = 0;
    double ms = 0;
    Digest source_hash{}, target_hash{};
};
uint64_t Size(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}
bool SameTimes(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return CompareFileTime(&a.ftCreationTime, &b.ftCreationTime) == 0 &&
        CompareFileTime(&a.ftLastAccessTime, &b.ftLastAccessTime) == 0 &&
        CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
bool Supported(const std::wstring& path, HANDLE file, BY_HANDLE_FILE_INFORMATION& info, Result& result) {
    if (!GetFileInformationByHandle(file, &info)) { result.error = GetLastError(); return false; }
    constexpr DWORD excluded = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
        FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED |
        FILE_ATTRIBUTE_OFFLINE;
    if (info.dwFileAttributes & excluded) { result.unsupported = true; result.error = ERROR_NOT_SUPPORTED; return false; }
    WIN32_FIND_STREAM_DATA stream{};
    HANDLE search = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &stream, 0);
    if (search == INVALID_HANDLE_VALUE) {
        result.error = GetLastError();
        // An unqueryable stream set is not evidence that the file has no ADS.
        result.unsupported = true;
        return false;
    }
    bool ordinary = true;
    do { if (wcscmp(stream.cStreamName, L"::$DATA") != 0) ordinary = false; }
    while (FindNextStreamW(search, &stream));
    const DWORD end = GetLastError();
    FindClose(search);
    if (!ordinary || end != ERROR_HANDLE_EOF) {
        result.unsupported = true;
        result.error = ordinary ? end : ERROR_NOT_SUPPORTED;
        return false;
    }
    return true;
}
bool ApplyMetadata(HANDLE target, const BY_HANDLE_FILE_INFORMATION& source, DWORD& error) {
    if (!SetFileTime(target, &source.ftCreationTime, &source.ftLastAccessTime, &source.ftLastWriteTime)) {
        error = GetLastError(); return false;
    }
    FILE_BASIC_INFO basic{};
    basic.FileAttributes = source.dwFileAttributes;
    if (!SetFileInformationByHandle(target, FileBasicInfo, &basic, sizeof(basic))) { error = GetLastError(); return false; }
    BY_HANDLE_FILE_INFORMATION actual{};
    if (!GetFileInformationByHandle(target, &actual)) { error = GetLastError(); return false; }
    if (Size(actual) != Size(source) || !SameTimes(actual, source) ||
        ((actual.dwFileAttributes ^ source.dwFileAttributes) & FILE_ATTRIBUTE_READONLY)) {
        error = ERROR_INVALID_DATA; return false;
    }
    return true;
}
// Owns only a CREATE_NEW object. Cleanup uses this pinned handle after writer join.
struct Output {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool keep = false;
    explicit Output(const std::wstring& path) {
        value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    ~Output() {
        if (value == INVALID_HANDLE_VALUE) return;
        if (!keep) {
            FILE_BASIC_INFO basic{}; basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            SetFileInformationByHandle(value, FileBasicInfo, &basic, sizeof(basic));
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
        CloseHandle(value);
    }
};
Result PipelineWork(const std::wstring& source, const std::wstring& target, unsigned blocks, DWORD block_bytes,
                std::atomic<bool>& cancel, uint64_t cancel_after = UINT64_MAX, bool* source_locked = nullptr) {
    Result result;
    Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
    BY_HANDLE_FILE_INFORMATION initial{};
    if (!Supported(source, input.value, initial, result)) return result;
    if (source_locked) {
        Handle writer(CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr));
        Handle remover(CreateFileW(source.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, 0, nullptr));
        *source_locked = writer.value == INVALID_HANDLE_VALUE && remover.value == INVALID_HANDLE_VALUE;
    }
    if (cancel.load()) { result.cancelled = true; result.error = ERROR_REQUEST_ABORTED; return result; }
    struct Slot { std::vector<unsigned char> bytes; DWORD count = 0; };
    std::vector<Slot> ring(blocks);
    for (auto& slot : ring) slot.bytes.resize(block_bytes);
    Hash source_hash, target_hash;
    if (!source_hash.good || !target_hash.good) { result.error = ERROR_INTERNAL_ERROR; return result; }
    Output output(target);
    if (output.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
    std::mutex mutex;
    std::condition_variable changed;
    unsigned used = 0, ready = 0;
    bool finished = false;
    std::atomic<DWORD> writer_error{ERROR_SUCCESS};
    std::thread writer([&] {
        unsigned index = 0;
        uint64_t written = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(25), [&] { return ready || finished || cancel.load(); });
                if (cancel.load()) break;
                if (!ready) { if (finished) break; else continue; }
                --ready;
            }
            auto& slot = ring[index];
            DWORD offset = 0;
            while (offset < slot.count && !cancel.load()) {
                DWORD done = 0;
                const BOOL ok = WriteFile(output.value, slot.bytes.data() + offset, slot.count - offset, &done, nullptr);
                if (!ok || !done) {
                    writer_error = !ok ? GetLastError() : ERROR_WRITE_FAULT;
                    cancel = true;
                    changed.notify_all();
                    break;
                }
                offset += done;
                written += done;
                if (written >= cancel_after) { cancel = true; changed.notify_all(); }
            }
            {
                std::lock_guard lock(mutex);
                --used;
            }
            changed.notify_all();
            index = (index + 1) % blocks;
        }
    });
    struct JoinWriter {
        std::thread& writer;
        std::atomic<bool>& cancel;
        std::mutex& mutex;
        std::condition_variable& changed;
        bool& finished;
        ~JoinWriter() {
            if (!writer.joinable()) return;
            cancel = true;
            { std::lock_guard lock(mutex); finished = true; }
            changed.notify_all();
            writer.join();
        }
    } join_writer{writer, cancel, mutex, changed, finished};
    unsigned index = 0;
    while (!cancel.load()) {
        {
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(25), [&] { return used < blocks || cancel.load(); });
            if (cancel.load()) break;
            if (used == blocks) continue;
        }
        auto& slot = ring[index];
        if (!ReadFile(input.value, slot.bytes.data(), block_bytes, &slot.count, nullptr)) {
            result.error = GetLastError(); cancel = true; break;
        }
        if (!slot.count) break;
        if (!source_hash.Add(slot.bytes.data(), slot.count)) { result.error = ERROR_INTERNAL_ERROR; cancel = true; break; }
        result.source_read += slot.count;
        {
            std::lock_guard lock(mutex);
            ++ready; ++used;
        }
        changed.notify_all();
        index = (index + 1) % blocks;
    }
    {
        std::lock_guard lock(mutex);
        finished = true;
    }
    changed.notify_all();
    writer.join(); // No failure/cleanup path may outlive the writer.
    if (writer_error.load()) result.error = writer_error.load();
    if (cancel.load()) {
        result.cancelled = result.error == ERROR_SUCCESS;
        if (!result.error) result.error = ERROR_REQUEST_ABORTED;
        return result;
    }
    BY_HANDLE_FILE_INFORMATION after{};
    if (!GetFileInformationByHandle(input.value, &after)) { result.error = GetLastError(); return result; }
    if (result.source_read != Size(initial) || Size(after) != Size(initial) ||
        CompareFileTime(&initial.ftLastWriteTime, &after.ftLastWriteTime)) { result.error = ERROR_FILE_INVALID; return result; }
    if (!source_hash.Finish(result.source_hash)) { result.error = ERROR_INTERNAL_ERROR; return result; }
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(output.value, zero, nullptr, FILE_BEGIN)) { result.error = GetLastError(); return result; }
    auto& reread = ring[0].bytes;
    for (;;) {
        if (cancel.load()) { result.cancelled = true; result.error = ERROR_REQUEST_ABORTED; return result; }
        DWORD count = 0;
        if (!ReadFile(output.value, reread.data(), block_bytes, &count, nullptr)) { result.error = GetLastError(); return result; }
        if (!count) break;
        result.target_read += count;
        if (!target_hash.Add(reread.data(), count)) { result.error = ERROR_INTERNAL_ERROR; return result; }
    }
    if (!target_hash.Finish(result.target_hash) || result.source_hash != result.target_hash ||
        result.target_read != result.source_read) { result.error = ERROR_CRC; return result; }
    if (!ApplyMetadata(output.value, initial, result.error)) return result;
    output.keep = true;
    result.ok = true;
    return result;
}
Result Pipeline(const std::wstring& source, const std::wstring& target, unsigned blocks, DWORD block_bytes,
                std::atomic<bool>& cancel, uint64_t cancel_after = UINT64_MAX, bool* source_locked = nullptr) {
    const auto started = Clock::now();
    auto result = PipelineWork(source, target, blocks, block_bytes, cancel, cancel_after, source_locked);
    result.ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return result;
}
Result BaselineWork(const std::wstring& source, const std::wstring& target) {
    Result result;
    Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (input.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
    if (!Supported(source, input.value, info, result)) return result;
    pulse::ops::VerificationStamp stamp;
    DWORD error = 0;
    if (!pulse::ops::CaptureVerificationStamp(source, stamp, error)) { result.error = error; return result; }
    COPYFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS;
    const HRESULT hr = CopyFile2(source.c_str(), target.c_str(), &parameters);
    if (FAILED(hr)) { result.error = HRESULT_CODE(hr); return result; }
    std::atomic<bool> cancel{false};
    pulse::ops::SHA256VerificationWorker verifier;
    const auto verified = verifier.Verify(source, target, cancel, {}, &stamp);
    result.source_read = verified.source_bytes;
    result.target_read = verified.target_bytes;
    if (!verified.Success()) { result.error = verified.win32_error ? verified.win32_error : ERROR_CRC; return result; }
    Handle destination(CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (destination.value == INVALID_HANDLE_VALUE) { result.error = GetLastError(); return result; }
    if (!ApplyMetadata(destination.value, info, result.error)) return result;
    result.ok = true;
    return result;
}
Result Baseline(const std::wstring& source, const std::wstring& target) {
    const auto started = Clock::now();
    auto result = BaselineWork(source, target);
    result.ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return result;
}
bool Fixture(const std::wstring& path, uint64_t bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_NEW, 0, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> buffer(1 << 20);
    uint32_t state = 0x8be57a31;
    for (auto& value : buffer) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; value = static_cast<unsigned char>(state); }
    while (bytes) {
        const DWORD count = static_cast<DWORD>((std::min<uint64_t>)(bytes, buffer.size()));
        DWORD done = 0;
        if (!WriteFile(file.value, buffer.data(), count, &done, nullptr) || done != count) return false;
        bytes -= count;
    }
    FILETIME fixed{0x69000000, 0x01d90000};
    return SetFileTime(file.value, &fixed, &fixed, &fixed) != FALSE;
}
bool RemoveFixture(const std::wstring& path) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) != FALSE;
}
int Checks(const std::filesystem::path& root, unsigned blocks, DWORD block_bytes) {
    int failed = 0;
    const auto check = [&](bool ok, const char* label) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failed;
    };
    const auto source = (root / L"checks-source.bin").wstring();
    const auto target = (root / L"checks-target.bin").wstring();
    check(Fixture(source, 8ull << 20), "isolated ordinary fixture");
    std::atomic<bool> cancel{false};
    bool locked = false;
    auto result = Pipeline(source, target, blocks, block_bytes, cancel, UINT64_MAX, &locked);
    check(result.ok && result.source_read == (8ull << 20) && result.target_read == result.source_read &&
          result.source_hash == result.target_hash, "pipeline source SHA and complete target reread SHA");
    check(locked, "source handle rejects write and replacement access");
    check(result.ok, "size and all three times preserved");
    cancel = false;
    auto existing = Pipeline(source, target, blocks, block_bytes, cancel);
    check(!existing.ok && (existing.error == ERROR_FILE_EXISTS || existing.error == ERROR_ALREADY_EXISTS),
          "CREATE_NEW preserves existing destination");
    pulse::ops::SHA256VerificationWorker retained_verifier;
    const auto retained = retained_verifier.Verify(source, target, cancel, {});
    check(retained.Success(), "existing destination content still equals source");
    check(RemoveFixture(target), "remove successful output");
    cancel = false;
    result = Pipeline(source, target, blocks, block_bytes, cancel, block_bytes);
    check(result.cancelled && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES,
          "cancel joins writer before removing partial output");
    check(SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "readonly source fixture");
    cancel = false;
    result = Pipeline(source, target, blocks, block_bytes, cancel);
    check(result.ok && (GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_READONLY), "readonly metadata preserved");
    check(RemoveFixture(target), "remove readonly output");
    const auto baseline = Baseline(source, target);
    check(baseline.ok && baseline.source_read == (8ull << 20) && baseline.target_read == baseline.source_read,
          "CopyFile2 plus existing SHA256 worker baseline");
    check(RemoveFixture(target), "remove baseline output");
    SetFileAttributesW(source.c_str(), FILE_ATTRIBUTE_NORMAL);
    const auto ads = source + L":experiment-ads";
    if (Fixture(ads, 17)) {
        cancel = false;
        result = Pipeline(source, target, blocks, block_bytes, cancel);
        check(result.unsupported && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES,
              "ADS rejected before target creation");
        DeleteFileW(ads.c_str());
    } else printf("[SKIP] filesystem did not permit ADS fixture\n");
    {
        Handle sparse(CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        DWORD returned = 0;
        if (sparse.value != INVALID_HANDLE_VALUE && DeviceIoControl(sparse.value, FSCTL_SET_SPARSE,
            nullptr, 0, nullptr, 0, &returned, nullptr)) {
            CloseHandle(sparse.value); sparse.value = INVALID_HANDLE_VALUE;
            cancel = false;
            result = Pipeline(source, target, blocks, block_bytes, cancel);
            check(result.unsupported && GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "sparse file rejected before target creation");
        } else printf("[SKIP] filesystem did not permit sparse fixture\n");
    }
    return failed;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    unsigned blocks = 2, block_kib = 1024, pairs = 3, selected_size = 0;
    bool checks_only = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--checks-only") == 0) { checks_only = true; continue; }
        if (i + 1 == argc) return 2;
        const std::wstring option = argv[i];
        wchar_t* end = nullptr;
        const unsigned long value = wcstoul(argv[++i], &end, 10);
        if (!end || *end) return 2;
        if (option == L"--blocks") blocks = static_cast<unsigned>(value);
        else if (option == L"--block-kib") block_kib = static_cast<unsigned>(value);
        else if (option == L"--pairs") pairs = static_cast<unsigned>(value);
        else if (option == L"--size-mib") selected_size = static_cast<unsigned>(value);
        else return 2;
    }
    if ((blocks != 2 && blocks != 4) || (block_kib != 256 && block_kib != 1024 && block_kib != 4096) ||
        pairs < 3 || pairs > 20 || (selected_size && selected_size != 65 && selected_size != 256)) return 2;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return 2;
    wchar_t guid_text[40]{};
    if (!StringFromGUID2(guid, guid_text, 40)) return 2;
    const auto base = std::filesystem::absolute(L"bench_data").lexically_normal();
    const auto root = (base / (std::wstring(L"block_experiment_") + guid_text)).lexically_normal();
    if (root.parent_path() != base || !std::filesystem::create_directories(root)) return 2;
    printf("EXPERIMENT_ONLY blocks=%u block_kib=%u buffer_bytes=%llu pairs=%u\n", blocks, block_kib,
           static_cast<unsigned long long>(blocks) * block_kib * 1024, pairs);
    int failed = Checks(root, blocks, block_kib * 1024);
    if (!checks_only && !failed) {
        for (unsigned size : {65u, 256u}) {
            if (selected_size && size != selected_size) continue;
            const auto source = (root / (L"source-" + std::to_wstring(size) + L".bin")).wstring();
            if (!Fixture(source, static_cast<uint64_t>(size) << 20)) { ++failed; break; }
            for (unsigned pair = 0; pair < pairs; ++pair) {
                for (unsigned position = 0; position < 2; ++position) {
                    const bool pipeline = ((pair + position) % 2) != 0;
                    const auto target = (root / (L"out-" + std::to_wstring(size) + L"-" +
                        std::to_wstring(pair) + L"-" + std::to_wstring(position) + L".bin")).wstring();
                    std::atomic<bool> cancel{false};
                    const auto result = pipeline ? Pipeline(source, target, blocks, block_kib * 1024, cancel)
                                                 : Baseline(source, target);
                    printf("[%s] size_mib=%u pair=%u position=%u backend=%s wall_ms=%.3f source_hash_bytes=%llu target_reread_bytes=%llu error=%lu\n",
                        result.ok ? "PASS" : "FAIL", size, pair, position, pipeline ? "pipeline" : "CopyFile2+SHA256", result.ms,
                        static_cast<unsigned long long>(result.source_read), static_cast<unsigned long long>(result.target_read), result.error);
                    if (!result.ok) ++failed;
                    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES && !RemoveFixture(target)) ++failed;
                }
            }
        }
    }
    // The sole recursive cleanup is this newly-created GUID child of bench_data.
    if (root.parent_path() == base && root.filename().wstring().starts_with(L"block_experiment_")) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(root))
            if (item.is_regular_file()) SetFileAttributesW(item.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code error;
        std::filesystem::remove_all(root, error);
        if (error) { printf("[FAIL] isolated fixture cleanup\n"); ++failed; }
    } else ++failed;
    printf("Experiment only; no production backend enabled. failures=%d\n", failed);
    return failed ? 1 : 0;
}
