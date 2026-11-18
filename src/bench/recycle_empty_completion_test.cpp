#include "../ops/recycle_empty.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    namespace recycle = pulse::recycle_empty;
    int failures = 0;
    auto check = [&](bool passed, const char* message) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << message << '\n';
        if (!passed) ++failures;
    };
    const auto root = fs::absolute(fs::path("bench_data") /
        ("empty-completion-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())));
    fs::create_directories(root);
    for (bool orphan : {false, true}) {
        const auto bin = root / (orphan ? "orphan" : "payload");
        fs::create_directory(bin);
        { std::ofstream file(bin / "$Ifixture"); file << "isolated index"; }
        { std::ofstream file(bin / "desktop.ini"); file << "must remain"; }
        if (!orphan) { std::ofstream file(bin / "$Rfixture"); file << "isolated payload"; }
        const auto locked = bin / (orphan ? "$Ifixture" : "$Rfixture");
        HANDLE handle = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "lock only an isolated recycle-shaped fixture");
        const std::vector<std::wstring> folders{bin.wstring()};
        const auto native = recycle::EmptyFolders(folders, {}, {}, 1);
        check(native.failed_entries > 0 && native.first_error != ERROR_SUCCESS && fs::exists(locked),
            "native pass records a real failure and leaves the locked file intact");
        check(FAILED(recycle::FinalizeResult(native, S_OK, folders)),
            "zero Shell count or successful fallback cannot override remaining I/R entries");
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        const auto fallback = recycle::EmptyFolders(folders, {}, {}, 1);
        check(!fallback.failed_entries && !fs::exists(bin / "$Ifixture") && !fs::exists(bin / "$Rfixture"),
            "isolated fallback or external cleanup actually removes the failed entries");
        const bool old_failure = FAILED(S_OK) || native.failed_entries != 0;
        check(old_failure && SUCCEEDED(recycle::FinalizeResult(native, S_OK, folders)),
            "same historical native result fails old predicate but succeeds after verified empty reconciliation");
        check(fs::exists(bin / "desktop.ini"), "verification ignores and preserves unrelated bin metadata");
        check(FAILED(recycle::FinalizeResult(native, E_ACCESSDENIED, folders)),
            "unknown Shell query or failed fallback is not disguised by zero items");
        auto cancelled = native; cancelled.cancelled = true;
        check(recycle::FinalizeResult(cancelled, S_OK, folders) == HRESULT_FROM_WIN32(ERROR_CANCELLED),
            "cancellation stays cancelled even after the directory becomes empty");
        check(FAILED(recycle::FinalizeResult(native, S_OK, {})),
            "missing original directory evidence cannot clear a prior failure");
        const auto invalid = root / "not-a-directory";
        { std::ofstream file(invalid); file << "not enumerable"; }
        check(FAILED(recycle::FinalizeResult(native, S_OK, {invalid.wstring()})),
            "failed directory verification preserves failure");
    }
    fs::remove_all(root);
    return failures ? 1 : 0;
}
