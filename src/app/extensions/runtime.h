#pragma once

#include "manifest.h"
#include "authorization.h"
#include <functional>
#include "../../common/json_value.h"

namespace pulse::extensions {

struct CallerLease {
    std::string ticket;
    authorization::PackageIdentity package;
    authorization::OwnerHandle owner;
};
// UI-thread only: capture a trusted package and live owner; never accept claimed identity.
using CallerBinding = authorization::PrincipalBinding;
std::optional<CallerBinding> CaptureCallerBinding(size_t package, uintptr_t owner);
bool ValidateCallerBinding(const CallerBinding& binding);
// Host implementation only. Static lifetime outlives operation workers; never an ABI for packages.
authorization::Registry& HostAuthorizationRegistry();
std::optional<CallerLease> IssueCaller(const CallerBinding& binding);
std::optional<CallerLease> IssueCaller(size_t package, uintptr_t owner);
void ReleaseCallerOwner(uintptr_t owner);
// Thread safe: consults only the registry, not AppState or the runtime package vector.
bool ValidateCaller(const CallerLease& caller);
// Explicit package ownership. Startup is transactional; UI changes never access disk.
size_t PackageCount();
const Manifest* PackageManifest(size_t package);
// Immutable startup path, paired with the native digest in PackageManifest.
std::wstring PackageNativePath(size_t package);
bool PackageEnabled(size_t package);
void SetPackageEnabled(size_t package, bool enabled);
uint64_t PackageGeneration(size_t package);
bool PackageStoreMode();
// Host-only UI-thread diagnostic; no policy contents or authorization authority.
uint64_t StoreTrustCheckSequence();
// Legacy single-package compatibility refers only to slot zero, never the selected UI panel.
const Manifest* CurrentManifest();
bool Enabled();
void SetEnabled(bool enabled);

struct StartupResult {
    bool exit_requested = false;
    int exit_code = 0;
};

StartupResult PrepareStartup(int argc, wchar_t** argv);
// Last-host shutdown, before CRT/static teardown. Drains accepted audit records;
// the host's existing exit watchdog remains responsible for forced-exit limits.
void PrepareShutdown();
bool DevelopmentMode();
uint64_t CurrentManifestGeneration();
bool StartDevelopmentReload();
bool PollDevelopmentReload(const std::function<void()>& revoke_old_ui);
void StopDevelopmentReload();
json::Value DevelopmentStatus();

}
