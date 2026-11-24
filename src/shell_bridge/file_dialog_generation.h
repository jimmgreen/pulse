#pragma once
#include "../common/dialog_auto_registration.h"
#include "../common/dialog_bundle.h"
#include <objbase.h>
#include <functional>
#include <mutex>
#include <thread>

namespace pulse::shell_bridge {
// Only immutable, content-addressed generations of one installation may share
// admission. Keep both closures leased; providers always remain on the old ABI.
inline bool GenerationLocation(const std::wstring& dll, std::wstring& root, std::wstring& digest) {
    const std::wstring suffix = std::wstring(L"\\") + dialog_auto::DllName;
    if (!dll.ends_with(suffix)) return false;
    const auto end = dll.size() - suffix.size();
    const auto start = dll.rfind(L'\\', end - 1);
    if (start == dll.npos || end - start - 1 != 64) return false;
    digest = dll.substr(start + 1, 64);
    if (digest.find_first_not_of(L"0123456789abcdef") != digest.npos) return false;
    const std::wstring bridge = L"\\DialogBridge";
    const auto parent = dll.substr(0, start);
    if (!parent.ends_with(bridge)) return false;
    root = parent.substr(0, parent.size() - bridge.size());
    return !root.empty();
}

inline std::shared_ptr<const void> VerifyGenerationPair(const std::wstring& own, const std::wstring& current) {
    std::wstring root, digest, current_root, current_digest;
    if (!GenerationLocation(own, root, digest) || !GenerationLocation(current, current_root, current_digest) || root != current_root) return {};
    struct Proof {
        dialog_bundle::BundleLease old_bundle, current_bundle;
        HANDLE old_entry = INVALID_HANDLE_VALUE, current_entry = INVALID_HANDLE_VALUE;
        ~Proof() {
            if (old_entry != INVALID_HANDLE_VALUE) CloseHandle(old_entry);
            if (current_entry != INVALID_HANDLE_VALUE) CloseHandle(current_entry);
        }
    };
    auto proof = std::make_shared<Proof>();
    // A full bundle may omit the optional COM entry. Open it before verification
    // so closure enumeration proves this exact entry belongs to the manifest.
    proof->old_entry = CreateFileW(own.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    proof->current_entry = CreateFileW(current.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (proof->old_entry == INVALID_HANDLE_VALUE || proof->current_entry == INVALID_HANDLE_VALUE) return {};
    std::string hash;
    for (const wchar_t digit : digest) hash.push_back(static_cast<char>(digit));
    if (dialog_bundle::VerifyBundle(own.substr(0, own.find_last_of(L'\\')), hash, proof->old_bundle)) return {};
#ifdef _WIN64
    constexpr unsigned machine = IMAGE_FILE_MACHINE_AMD64;
#else
    constexpr unsigned machine = IMAGE_FILE_MACHINE_I386;
#endif
    if (proof->old_bundle.Machine() != machine || proof->old_bundle.Directory() + L"\\" + dialog_auto::DllName != own) return {};
    if (dialog_bundle::ResolveInstalledBundleForMachine(root, machine, proof->current_bundle) ||
        proof->current_bundle.Directory() + L"\\" + dialog_auto::DllName != current) return {};
    return proof;
}

class GenerationAdmission {
public:
    using Verifier = std::function<std::shared_ptr<const void>(const std::wstring&, const std::wstring&)>;
    explicit GenerationAdmission(Verifier verify = VerifyGenerationPair) : state_(std::make_shared<State>()), verify_(std::move(verify)) {}
    bool Enabled(const std::wstring& own, const dialog_auto::Status& status) {
        if (!status.enabled || status.conflict || status.error || own.empty()) return false;
        if (status.dll_path == own) return true;
        std::wstring root, digest, next_root, next_digest;
        if (!GenerationLocation(own, root, digest) || !GenerationLocation(status.dll_path, next_root, next_digest) || root != next_root) return false;
        auto state = state_;
        std::shared_ptr<Job> job;
        {
            std::lock_guard guard(state->mutex);
            job = state->latest;
            if (job && WaitForSingleObject(job->ready, 0) == WAIT_OBJECT_0) {
                std::lock_guard job_guard(job->mutex);
                // A pointer publication or transient file lock may have failed.
                // Never make that failure permanent for the cached factory.
                if (!job->proof) job.reset();
            }
            if (!job || job->own != own || job->current != status.dll_path) {
                job = std::make_shared<Job>(own, status.dll_path);
                if (!job->ready) return false;
                try {
                    std::thread([state, job, verify = verify_] {
                        std::shared_ptr<const void> proof;
                        try { proof = verify(job->own, job->current); } catch (...) {}
                        {
                            std::lock_guard lock(state->mutex);
                            if (proof && !state->initial_proof) state->initial_proof = proof;
                        }
                        {
                            std::lock_guard lock(job->mutex);
                            job->proof = std::move(proof);
                        }
                        SetEvent(job->ready);
                    }).detach();
                } catch (...) { return false; }
                state->latest = job;
            }
        }
        // Each waiter owns its immutable job/event. A reentrant Show or another
        // STA may request a different generation without resetting this event.
        // The server is pinned before admission; hashing never runs on the STA.
        DWORD index = 0;
        if (FAILED(CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            30000, 1, &job->ready, &index))) return false;
        std::lock_guard guard(job->mutex);
        return !!job->proof;
    }

private:
    struct Job {
        Job(std::wstring old_path, std::wstring new_path) : own(std::move(old_path)), current(std::move(new_path)) {}
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Job() { if (ready) CloseHandle(ready); }
        const std::wstring own, current;
        std::mutex mutex;
        std::shared_ptr<const void> proof;
    };
    struct State {
        std::mutex mutex;
        // Preserve the serving generation even if a later update fails verification.
        std::shared_ptr<const void> initial_proof;
        std::shared_ptr<Job> latest;
    };
    std::shared_ptr<State> state_;
    Verifier verify_;
};
}
