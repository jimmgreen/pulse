#pragma once
// Downloads, verifies and installs a preview pack on a worker thread.
//
// Each pack file is fetched as download_name or <name>.lzms (Compression API, LZMS,
// available since Windows 8) through the update transport, so mirrors and
// cancellation behave like updates. Both the download and the decompressed
// file have to match the hashes pinned in pack_catalog.h. Files land in
// <key>\<version>.partial, which is renamed once complete; installed.json is
// then replaced atomically, so the preview host only ever sees a whole pack.
#include "../common/windows_compat.h"
#include "pack_catalog.h"
#include "update_transport.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace pulse::app {

enum class PackInstallOutcome : uint32_t { None, Installed, Failed, Cancelled };

// Why an install failed, for what the failure message offers next.
enum class PackFailure : uint32_t {
    None,
    Network,    // no source could be reached (or kept answering with an error)
    Mismatch,   // every source served a file this build does not expect
    Disk,       // the packs folder could not be written
    Other,
};

struct PackInstallProgress {
    std::atomic<bool> cancelled{false};
    std::atomic<uint32_t> failure{0};    // PackFailure of a failed install
    std::atomic<uint64_t> received{0};   // compressed bytes of finished and current files
    std::atomic<uint64_t> total{0};
};

// Synchronous core, used by the worker and by tests. `root` is the packs
// directory; `error` gets a Win32 code on failure (ERROR_CANCELLED when
// progress.cancelled was set). With `reuse_installed`, a file the installed
// version already has with the pinned size and SHA-256 is copied (and
// verified again) instead of downloaded, so an update only fetches what
// changed.
PackInstallOutcome InstallPack(const PackRelease& release, const std::wstring& root,
                               PackInstallProgress& progress, DWORD& error,
                               const UpdateResponseReader& read = ReadUpdateResponse,
                               bool reuse_installed = false);

// Removes a directory tree without following junctions.
bool RemovePackTree(const std::wstring& directory);

class PackInstaller {
public:
    PackInstaller() = default;
    PackInstaller(const PackInstaller&) = delete;
    PackInstaller& operator=(const PackInstaller&) = delete;
    ~PackInstaller();

    // `notify` is invalidated on progress and when the job ends; `read`
    // fetches the files (tests pass a fake server).
    bool Start(const PackRelease& release, HWND notify, UpdateResponseReader read = ReadUpdateResponse);
    void Cancel() noexcept;
    bool running() const noexcept;
    // 0..1 of the compressed bytes.
    float progress() const noexcept;
    // The finished job's outcome, once; None while idle or running.
    PackInstallOutcome TakeOutcome(DWORD& error, PackFailure* failure = nullptr);

private:
    struct Job {
        PackInstallProgress progress;
        std::atomic<bool> running{false};
        std::atomic<uint32_t> outcome{0};
        std::atomic<DWORD> error{0};
    };
    std::shared_ptr<Job> job_;
    std::thread worker_;
};

} // namespace pulse::app
