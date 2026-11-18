#include "../ops/verification_worker.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
namespace fs = std::filesystem;
using namespace pulse::ops;
static void Write(const fs::path& p, const char* text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc); out << text; out.close();
    if (!out) throw std::runtime_error("fixture write");
}
static void Check(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(label);
    printf("[PASS] %s\n", label);
}
int wmain() {
    const auto root = fs::absolute(fs::path(L"bench_data") / (L"verification_worker_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    try {
        if (!fs::create_directories(root)) throw std::runtime_error("unique fixture");
        const auto a = root / L"source", b = root / L"target";
        SHA256VerificationWorker worker;
        std::atomic<bool> cancel{false};
        Write(a, "abc"); Write(b, "abc");
        auto result = worker.Verify(a.wstring(), b.wstring(), cancel, {});
        Check(result.Success() && result.source_bytes == 3 && result.target_bytes == 3, "matching full rereads");
        Write(b, "abd");
        Check(worker.Verify(a.wstring(), b.wstring(), cancel, {}).error == VerificationError::Mismatch, "same-size mismatch");
        Write(a, ""); Write(b, "");
        Check(worker.Verify(a.wstring(), b.wstring(), cancel, {}).Success(), "worker reuse resets hash for empty files");
        Write(a, "abc"); Write(b, "abc");
        VerificationStamp before; DWORD error = 0;
        Check(CaptureVerificationStamp(a.wstring(), before, error), "pre-copy stamp");
        Write(a, "changed source"); Write(b, "changed source");
        Check(worker.Verify(a.wstring(), b.wstring(), cancel, {}, &before).error == VerificationError::SourceChanged, "pre-copy source change despite equal hashes");
        int waits = 0;
        result = worker.Verify(a.wstring(), b.wstring(), cancel, [&] { ++waits; cancel.store(true); });
        Check(result.error == VerificationError::Cancelled && waits == 1, "pause hook cancellation before read");
        cancel.store(false);
        result = worker.Verify(a.wstring(), b.wstring(), cancel, [&] { ++waits; Sleep(1); });
        Check(result.Success() && waits > 1, "resume hook and reuse after cancellation");
        int reads = 0;
        result = worker.Verify(a.wstring(), b.wstring(), cancel, [&] {
            if (++reads == 2) Write(a, "source mutated after first read");
        });
        Check(result.error == VerificationError::SourceChanged, "source mutation during hash");
        Check(worker.Verify((root / L"absent").wstring(), b.wstring(), cancel, {}).error == VerificationError::SourceOpen, "source open classification");
        Check(worker.Verify(a.wstring(), (root / L"absent").wstring(), cancel, {}).error == VerificationError::TargetOpen, "target open classification");
        Write(a, "same content"); Write(b, "same content");
        VerificationStamp owned_target;
        Check(CaptureVerificationStamp(b.wstring(), owned_target, error), "owned target identity");
        auto early_callback = owned_target;
        early_callback.bytes = 0;
        early_callback.last_write = {};
        Check(worker.Verify(a.wstring(), b.wstring(), cancel, {}, nullptr, &early_callback).Success(), "target callback compares identity only");
        // Keep the original object alive at another name, avoiding file-ID reuse.
        fs::rename(b, root / L"original_target");
        Write(b, "same content");
        Check(worker.Verify(a.wstring(), b.wstring(), cancel, {}, nullptr, &owned_target).error == VerificationError::TargetChanged,
            "same-content replacement target rejected");
        Check(worker.Verify(a.wstring(), b.wstring(), cancel, {}).Success(), "legacy caller without expected identity remains compatible");
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& e) { fprintf(stderr, "[FAIL] %s fixture=%ls\n", e.what(), root.c_str()); return 1; }
}
