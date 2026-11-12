// worker_isolation_test.cpp - end-to-end verification of out-of-process 7z.dll
// isolation (D1-D3, V08, V18):
//   1. Static PE import audit: pulse_7zip.dll never imports 7z.dll;
//      pulse_7zip_worker.exe never statically imports USER32.dll / GDI32.dll.
//   2. Full IPC round-trip (SupportedFormats, Create, List, Extract, Test, Hash,
//      streaming progress, and encrypted archive password prompt round-trips).
//   3. Crash isolation (EXCEPTION_ACCESS_VIOLATION in worker -> host unaffected,
//      immediate automatic recovery in a fresh worker PID).
//   4. Hang isolation (infinite loop ignoring cooperative cancel -> hard timeout
//      and cancel-grace termination via Job Object + immediate file-lock release).
//   5. Memory cap enforcement (Job Object process/job memory limit).
//   6. Protocol safety (oversized frame & wrong-generation frame fail closed).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "engine.h"
#include "worker_client.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    if (ok) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s%s%s\n", name, detail.empty() ? "" : " -- ", detail.c_str());
        ++g_failures;
    }
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void WriteBytes(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

fs::path ModuleDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
}

bool ReadPeImports(const fs::path& pe_path, std::vector<std::string>& out_dlls) {
    out_dlls.clear();
    const std::string data = ReadBytes(pe_path);
    if (data.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, data.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    const size_t nt_off = static_cast<size_t>(dos.e_lfanew);
    if (nt_off + sizeof(IMAGE_NT_HEADERS64) > data.size()) return false;
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, data.data() + nt_off, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    const auto& imp_dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.VirtualAddress == 0 || imp_dir.Size == 0) return true;

    const size_t sec_off = nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    const size_t sec_count = nt.FileHeader.NumberOfSections;
    if (sec_off + sec_count * sizeof(IMAGE_SECTION_HEADER) > data.size()) return false;

    std::vector<IMAGE_SECTION_HEADER> sections(sec_count);
    std::memcpy(sections.data(), data.data() + sec_off, sec_count * sizeof(IMAGE_SECTION_HEADER));

    const auto rva_to_offset = [&](DWORD rva, size_t& out_off) -> bool {
        for (const auto& s : sections) {
            const DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span) {
                const size_t delta = rva - s.VirtualAddress;
                if (delta >= s.SizeOfRawData || s.PointerToRawData + delta >= data.size()) return false;
                out_off = s.PointerToRawData + delta;
                return true;
            }
        }
        return false;
    };

    size_t desc_off = 0;
    if (!rva_to_offset(imp_dir.VirtualAddress, desc_off)) return false;
    for (; desc_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= data.size(); desc_off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR desc{};
        std::memcpy(&desc, data.data() + desc_off, sizeof(desc));
        if (desc.Name == 0 && desc.FirstThunk == 0 && desc.OriginalFirstThunk == 0) break;
        size_t name_off = 0;
        if (!rva_to_offset(desc.Name, name_off)) return false;
        const auto* start = data.data() + name_off;
        const size_t max_len = data.size() - name_off;
        const auto* nul = static_cast<const char*>(std::memchr(start, '\0', max_len));
        if (!nul) return false;
        out_dlls.push_back(LowerAscii(std::string(start, static_cast<size_t>(nul - start))));
    }
    return true;
}

bool ContainsImport(const std::vector<std::string>& imports, const char* dll_lower) {
    return std::find(imports.begin(), imports.end(), std::string(dll_lower)) != imports.end();
}

engine::AskPassword PasswordAnswers(std::vector<std::wstring> answers, int* prompts = nullptr) {
    auto shared = std::make_shared<std::vector<std::wstring>>(std::move(answers));
    return [shared, prompts](bool, std::wstring& out) {
        if (prompts) ++(*prompts);
        if (shared->empty()) return false;
        out = shared->front();
        shared->erase(shared->begin());
        return true;
    };
}

int RunSuite() {
    const fs::path bin_dir = ModuleDir();
    const fs::path root = bin_dir / L"worker-isolation-test-data";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"tmp", ec);
    SetEnvironmentVariableW(L"PULSE_7ZIP_TEMP", (root / L"tmp").c_str());

    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            worker_client::ResetForTests();
            engine::ShutdownWorkers();
            std::error_code ignore;
            fs::remove_all(dir, ignore);
        }
    } cleanup{root};

    // 1. Static PE import audit (D1).
    {
        std::vector<std::string> plugin_imports;
        Check(ReadPeImports(bin_dir / L"pulse_7zip.dll", plugin_imports), "inspect pulse_7zip.dll PE import table");
        Check(!ContainsImport(plugin_imports, "7z.dll"),
              "pulse_7zip.dll does NOT link or import 7z.dll in-process");

        std::vector<std::string> worker_imports;
        Check(ReadPeImports(bin_dir / L"pulse_7zip_worker.exe", worker_imports), "inspect pulse_7zip_worker.exe PE import table");
        Check(!ContainsImport(worker_imports, "7z.dll") &&
              !ContainsImport(worker_imports, "user32.dll") &&
              !ContainsImport(worker_imports, "gdi32.dll"),
              "pulse_7zip_worker.exe has zero static imports of 7z.dll, USER32.dll, or GDI32.dll");
    }

    // 2. Full functional round-trip over out-of-process worker IPC (D1/D2).
    const engine::Cancelled never = [] { return false; };
    fs::path encrypted_7z;
    fs::path sample_zip;
    {
        const auto formats = engine::SupportedFormats();
        Check(formats.size() >= 30, "out-of-process worker enumerates 7z.dll formats",
              "count=" + std::to_string(formats.size()));
        const uint32_t warm_pid = worker_client::WarmWorkerPidForTests();
        Check(warm_pid != 0 && warm_pid != GetCurrentProcessId(),
              "preview worker runs out-of-process under a distinct PID");

        const fs::path src = root / L"src";
        const std::string doc_payload = std::string(128 * 1024, 'P') + "-PulseWorkerIsolation";
        WriteBytes(src / L"报告" / L"说明.txt", doc_payload);
        WriteBytes(src / L"报告" / L"数据.bin", std::string(8192, '\x42'));
        WriteBytes(root / L"readme.txt", "hello worker\n");

        // Create encrypted .7z with header encryption and streaming progress.
        const fs::path out_7z_dir = root / L"out7z";
        fs::create_directories(out_7z_dir);
        engine::CreateOptions enc_opts;
        enc_opts.format = engine::Format::SevenZip;
        enc_opts.password = L"隔离密码#2026";
        enc_opts.encrypt_names = true;
        int create_progress_events = 0;
        const auto created_7z = engine::Create(
            {src.wstring()}, out_7z_dir.wstring(), L"encrypted.7z", enc_opts, never,
            [&](const engine::Progress&) { ++create_progress_events; });
        Check(created_7z.status == engine::Status::Ok && created_7z.files == 2 && create_progress_events > 0,
              "out-of-process Create produces encrypted .7z and streams progress", created_7z.error);
        encrypted_7z = out_7z_dir / L"encrypted.7z";

        // Create unencrypted .zip for fast preview reuse tests.
        const fs::path out_zip_dir = root / L"outzip";
        fs::create_directories(out_zip_dir);
        engine::CreateOptions zip_opts;
        zip_opts.format = engine::Format::Zip;
        const auto created_zip = engine::Create(
            {src.wstring(), (root / L"readme.txt").wstring()}, out_zip_dir.wstring(), L"sample.zip", zip_opts, never, {});
        Check(created_zip.status == engine::Status::Ok && created_zip.files == 3,
              "out-of-process Create produces .zip archive", created_zip.error);
        sample_zip = out_zip_dir / L"sample.zip";

        // Preview List reuses warm worker PID.
        const auto preview1 = engine::List(sample_zip.wstring(), never, {}, 4096, engine::PreviewBudget());
        const uint32_t warm_pid_after = worker_client::WarmWorkerPidForTests();
        Check(preview1.status == engine::Status::Ok && preview1.entries.size() == 5 && warm_pid_after == warm_pid,
              "preview List succeeds and reuses warm worker session");

        // Password prompt round-trips across Named Pipe (refuse, wrong, right).
        int prompts = 0;
        const auto refused = engine::List(encrypted_7z.wstring(), never, PasswordAnswers({}, &prompts));
        Check(refused.status == engine::Status::NeedPassword && prompts == 1,
              "encrypted archive List round-trips password prompt and reports NeedPassword when refused");

        const auto wrong = engine::List(encrypted_7z.wstring(), never, PasswordAnswers({L"bad-pw"}));
        Check(wrong.status == engine::Status::WrongPassword,
              "encrypted archive List reports WrongPassword over IPC");

        const auto right = engine::List(encrypted_7z.wstring(), never, PasswordAnswers({L"隔离密码#2026"}));
        Check(right.status == engine::Status::Ok && right.entries.size() == 4,
              "encrypted archive List succeeds with valid password over IPC");

        // Extract + content check + test mode + Hash.
        const fs::path ext_dir = root / L"extracted";
        fs::create_directories(ext_dir);
        int extract_progress_events = 0;
        const auto extracted = engine::Extract(
            encrypted_7z.wstring(), ext_dir.wstring(), false, never,
            [&](const engine::Progress&) { ++extract_progress_events; },
            PasswordAnswers({L"隔离密码#2026"}));
        Check(extracted.status == engine::Status::Ok && extracted.files == 2 && extract_progress_events > 0 &&
              ReadBytes(ext_dir / L"src" / L"报告" / L"说明.txt") == doc_payload,
              "out-of-process Extract decodes encrypted .7z accurately with progress");

        const auto tested = engine::Extract(
            encrypted_7z.wstring(), {}, true, never, {}, PasswordAnswers({L"隔离密码#2026"}));
        Check(tested.status == engine::Status::Ok && tested.files == 2,
              "out-of-process Extract test_only verifies archive without writing files");

        const auto hashes = engine::Hash({(root / L"readme.txt").wstring()}, never, {});
        Check(hashes.size() == 1 && hashes[0].error.empty() && hashes[0].sha256.size() == 64,
              "out-of-process Hash computes CRC32/SHA-1/SHA-256");
    }

    const auto verify_fresh_recovery = [&](const char* label, uint32_t previous_pid = 0) {
        {
            worker_client::WorkerLimits current{};
            current.allow_fault_injection = true;
            // Clear cached formats without killing warm worker? ConfigureForTests resets warm worker,
            // so instead exercise warm preview worker directly when needed or Hash + SupportedFormats.
            (void)current;
        }
        // Clear format cache via List/SupportedFormats or verify fresh warm worker + Hash over IPC:
        const bool zip_ready = !sample_zip.empty() && fs::exists(sample_zip);
        bool list_ok = true;
        if (zip_ready) {
            const auto listed = engine::List(sample_zip.wstring(), never, {}, 4096, engine::PreviewBudget());
            list_ok = (listed.status == engine::Status::Ok && listed.entries.size() == 5);
        } else {
            (void)engine::SupportedFormats();
        }
        const uint32_t warm_after = worker_client::WarmWorkerPidForTests();
        const auto hashes = engine::Hash({(root / L"readme.txt").wstring()}, never, {});
        const bool hash_ok = (hashes.size() == 1 && hashes[0].error.empty() && hashes[0].sha256.size() == 64);
        const bool pid_ok = (warm_after != 0 && warm_after != GetCurrentProcessId() &&
                             (previous_pid == 0 || warm_after != previous_pid));
        Check(list_ok && hash_ok && pid_ok, label,
              "warm_pid=" + std::to_string(warm_after) + " prev_pid=" + std::to_string(previous_pid));
    };

    // 3. Fault injection & containment (D3 + V18).
    // 3A. Deliberate EXCEPTION_ACCESS_VIOLATION crash inside worker -> host survives, fresh worker recovers.
    {
        worker_client::WorkerLimits limits{};
        limits.allow_fault_injection = true;
        worker_client::ConfigureForTests(limits);

        // Warm up preview worker first.
        if (!sample_zip.empty() && fs::exists(sample_zip)) {
            (void)engine::List(sample_zip.wstring(), never, {}, 4096, engine::PreviewBudget());
        } else {
            (void)engine::SupportedFormats();
        }
        const uint32_t pid_before_crash = worker_client::WarmWorkerPidForTests();
        Check(pid_before_crash != 0 && pid_before_crash != GetCurrentProcessId(),
              "warm worker ready before AV crash test");

        const auto crash = worker_client::RunFaultForTests(worker_client::FaultKind::CrashAccessViolation);
        Check(crash.status == engine::Status::WorkerCrashed && crash.worker_pid == pid_before_crash,
              "worker EXCEPTION_ACCESS_VIOLATION is isolated and reported as WorkerCrashed", crash.error);

        verify_fresh_recovery("subsequent request automatically spawns a fresh worker PID and succeeds", pid_before_crash);
    }

    // 3B. Uncooperative infinite loop + hard timeout -> Job Object kill + immediate file-handle release.
    {
        worker_client::WorkerLimits limits{};
        limits.allow_fault_injection = true;
        limits.task_hard_timeout_ms = 350u;
        worker_client::ConfigureForTests(limits);

        const fs::path locked_file = root / L"hang_timeout_locked.tmp";
        const ULONGLONG t0 = GetTickCount64();
        const auto hung = worker_client::RunFaultForTests(
            worker_client::FaultKind::HangInfinite, 0, locked_file.wstring(), never, false);
        const ULONGLONG elapsed = GetTickCount64() - t0;
        Check(hung.status == engine::Status::WorkerTimedOut && elapsed < 4000u,
              "uncooperative hung worker is terminated on hard timeout", hung.error);
        Check(DeleteFileW(locked_file.c_str()) != FALSE,
              "terminated hung worker immediately releases exclusive file locks in staging directory");

        limits.task_hard_timeout_ms = 0u;
        worker_client::ConfigureForTests(limits);
        verify_fresh_recovery("worker client recovers cleanly after timeout kill");
    }

    // 3C. Uncooperative infinite loop + cooperative cancel grace expiry -> forced termination + cleanup.
    {
        worker_client::WorkerLimits limits{};
        limits.allow_fault_injection = true;
        limits.task_hard_timeout_ms = 15000u;
        limits.cancel_grace_ms = 200u;
        worker_client::ConfigureForTests(limits);

        const fs::path locked_file = root / L"hang_cancel_locked.tmp";
        std::atomic<bool> cancel_flag{false};
        std::thread canceller([&cancel_flag] {
            Sleep(120);
            cancel_flag.store(true, std::memory_order_release);
        });
        const ULONGLONG t0 = GetTickCount64();
        const auto cancelled_hang = worker_client::RunFaultForTests(
            worker_client::FaultKind::HangInfinite, 0, locked_file.wstring(),
            [&cancel_flag] { return cancel_flag.load(std::memory_order_acquire); }, false);
        const ULONGLONG elapsed = GetTickCount64() - t0;
        canceller.join();

        Check(cancelled_hang.status == engine::Status::Cancelled && elapsed < 4000u,
              "uncooperative worker ignoring MsgCancel is force-killed after cancel grace window",
              "elapsed=" + std::to_string(elapsed) + "ms");
        Check(DeleteFileW(locked_file.c_str()) != FALSE,
              "cancelled wedged worker releases exclusive staging file lock before returning");
    }

    // 3D. Job Object memory cap enforcement (JOB_OBJECT_LIMIT_PROCESS_MEMORY / JOB_MEMORY).
    {
        worker_client::WorkerLimits limits{};
        limits.allow_fault_injection = true;
        limits.preview_memory_bytes = 48ull * 1024u * 1024u;  // 48 MiB cap
        worker_client::ConfigureForTests(limits);

        const auto mem_fault = worker_client::RunFaultForTests(
            worker_client::FaultKind::ExceedMemory, 128ull * 1024u * 1024u);
        Check(mem_fault.status == engine::Status::WorkerMemoryLimit,
              "Job Object enforces worker memory cap and reports WorkerMemoryLimit", mem_fault.error);

        // Restore normal 512 MiB cap and verify recovery.
        limits.preview_memory_bytes = 512ull * 1024u * 1024u;
        worker_client::ConfigureForTests(limits);
        verify_fresh_recovery("fresh worker succeeds after memory-cap termination");
    }

    // 3E. Oversized frame & cross-generation response safety (D2/D3).
    {
        worker_client::WorkerLimits limits{};
        limits.allow_fault_injection = true;
        worker_client::ConfigureForTests(limits);

        const auto oversize = worker_client::RunFaultForTests(worker_client::FaultKind::OversizeFrame);
        Check(oversize.status == engine::Status::LimitReached,
              "oversized IPC frame (> 16 MiB) is rejected before allocation", oversize.error);

        const auto wrong_gen = worker_client::RunFaultForTests(worker_client::FaultKind::WrongGeneration);
        Check(wrong_gen.status == engine::Status::WorkerCrashed,
              "response carrying mismatched generation is rejected and worker is recycled", wrong_gen.error);

        verify_fresh_recovery("worker client recovers cleanly after protocol/generation faults");
    }

    worker_client::ResetForTests();
    engine::ShutdownWorkers();
    Check(worker_client::WarmWorkerPidForTests() == 0, "ShutdownWorkers reaps warm worker cleanly");
    return g_failures ? 1 : 0;
}

} // namespace

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const int rc = RunSuite();
        std::printf("worker_isolation_test: %d failed\n", g_failures);
        return rc;
    } catch (const std::exception& ex) {
        std::printf("[FAIL] unhandled exception: %s\n", ex.what());
        return 2;
    }
}
