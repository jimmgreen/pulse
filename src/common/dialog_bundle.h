#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <string_view>

namespace pulse::dialog_bundle {
inline constexpr wchar_t kAppRuntimeDirectory[] = L"AppRuntime";

// Numeric values are persisted as bundle_stage in launcher diagnostics.
enum class ResolveStage : unsigned {
    None = 0,
    InstallDirectory = 1,
    PointerOpen = 2,
    PointerData = 3,
    BundleDirectory = 4,
    Manifest = 5,
    VerificationMarker = 6,
    Member = 7,
    DependencyClosure = 8,
    DirectoryContents = 9,
    BundleKind = 10,
};

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
    friend struct BundleVerifier;
    friend DWORD ResolveInstalledBundle(const std::wstring&, BundleLease&, ResolveStage*) noexcept;
    friend DWORD ResolveInstalledBundleForMachine(const std::wstring&, unsigned, BundleLease&) noexcept;
    friend DWORD RecheckDirectory(const BundleLease&) noexcept;
};
// Empty expected digest means explicit first selection. A saved profile must bind
// a nonempty digest. Failure clears output; all work belongs on a worker thread.
DWORD VerifyBundle(const std::wstring& absolute_directory,
                   std::string_view expected_manifest_digest, BundleLease& output,
                   ResolveStage* stage = nullptr) noexcept;
// AppRuntime uses a distinct manifest kind. Dialog consumers must continue
// using VerifyBundle and ResolveInstalledBundle, which reject that kind.
DWORD VerifyAppRuntime(const std::wstring& absolute_directory,
                       std::string_view expected_manifest_digest, BundleLease& output,
                       ResolveStage* stage = nullptr) noexcept;
// Only the application launch path tolerates an empty, ordinary root log directory.
// Installation, cleanup and all dialog verification still use their strict APIs.
DWORD VerifyAppRuntimeForStartup(const std::wstring& absolute_directory,
                                std::string_view expected_manifest_digest, BundleLease& output,
                                ResolveStage* stage = nullptr) noexcept;
DWORD ResolveInstalledAppRuntime(const std::wstring& app_directory, BundleLease& output,
                                 ResolveStage* stage = nullptr) noexcept;
// Prefer AppRuntime. A present but invalid app package never downgrades to
// DialogBridge. The latter is used only for an installation without AppRuntime.
DWORD ResolveStartupBundle(const std::wstring& app_directory, BundleLease& output,
                           ResolveStage* stage = nullptr) noexcept;
// Resolves a bounded, strict dialog-current.json snapshot on a worker. The
// pointer is released before return; only the selected immutable bundle is leased.
// If requested, report the failed verification step without logging filesystem paths.
DWORD ResolveInstalledBundle(const std::wstring& app_directory, BundleLease& output,
                             ResolveStage* stage = nullptr) noexcept;
// Resolves the primary pointer first. Only an x86 companion may be selected;
// a present but invalid primary/companion fails closed, never a flat fallback.
DWORD ResolveInstalledBundleForMachine(const std::wstring& app_directory, unsigned machine,
                                      BundleLease& output) noexcept;
// Re-enumerates and rechecks file identities without changing files or loading DLLs.
DWORD RecheckDirectory(const BundleLease&) noexcept;
}
