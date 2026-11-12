#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::dialog_bundle {
// Experimental package integrity only: no signature, trust or UI capability claim.
// Keep alive through process creation/handshake. Files deny write/delete sharing;
// the lease does not make the directory immutable after it is released.
class BundleLease {
public:
    struct State;
    BundleLease() noexcept;
    ~BundleLease();
    BundleLease(BundleLease&&) noexcept;
    BundleLease& operator=(BundleLease&&) noexcept;
    BundleLease(const BundleLease&) = delete;
    BundleLease& operator=(const BundleLease&) = delete;
    explicit operator bool() const noexcept;
    const std::wstring& Directory() const noexcept;
    const std::wstring& Launcher() const noexcept;
    const std::wstring& Host() const noexcept;
    const std::wstring& Runtime() const noexcept;
    const std::wstring& UiProvider() const noexcept;
    const std::wstring& AdminAutoHelper() const noexcept;
    const std::wstring& AutoRuntime() const noexcept;
    const std::string& ManifestDigest() const noexcept;
    const std::string& Version() const noexcept;
    unsigned Machine() const noexcept;
    bool IsThinCompanion() const noexcept;
private:
    std::unique_ptr<State> state_;
    friend DWORD VerifyBundle(const std::wstring&, std::string_view, BundleLease&) noexcept;
    friend DWORD ResolveInstalledBundle(const std::wstring&, BundleLease&) noexcept;
    friend DWORD ResolveInstalledBundleForMachine(const std::wstring&, unsigned, BundleLease&) noexcept;
    friend DWORD RecheckDirectory(const BundleLease&) noexcept;
};
// Empty expected digest means explicit first selection. A saved profile must bind
// a nonempty digest. Failure clears output; all work belongs on a worker thread.
DWORD VerifyBundle(const std::wstring& absolute_directory,
                   std::string_view expected_manifest_digest, BundleLease& output) noexcept;
// Resolves a bounded, strict dialog-current.json snapshot on a worker. The
// pointer is released before return; only the selected immutable bundle is leased.
DWORD ResolveInstalledBundle(const std::wstring& app_directory, BundleLease& output) noexcept;
// Resolves the primary pointer first. Only an x86 companion may be selected;
// a present but invalid primary/companion fails closed, never a flat fallback.
DWORD ResolveInstalledBundleForMachine(const std::wstring& app_directory, unsigned machine,
                                      BundleLease& output) noexcept;
// Re-enumerates and rechecks file identities without changing files or loading DLLs.
DWORD RecheckDirectory(const BundleLease&) noexcept;
}
