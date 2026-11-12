#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulse::shell_bridge::runtime {
struct AutoImportHook { void* original = nullptr; void* replacement = nullptr; };
struct AutoImportResult {
    HRESULT status = E_PENDING;
    uint32_t api_mask = 0, patched_slots = 0, skipped_modules = 0, foreign_slots = 0;
    bool protection_restored = true;
};
// One-shot, current-process static-IAT transaction. No thread suspension or code
// patching. The owner is pinned; scanned modules are held through commit/rollback.
// Own bundle and Windows directories are excluded. Late DLLs, delay imports and
// GetProcAddress calls are intentionally not intercepted. Concurrent foreign
// changes to page protections are not serialized by this component.
class AutoImportPatch {
public:
    AutoImportPatch() noexcept;
    ~AutoImportPatch();
    AutoImportPatch(const AutoImportPatch&) = delete;
    AutoImportPatch& operator=(const AutoImportPatch&) = delete;
    // COM3 hooks, optionally followed by RoGetActivationFactory. Omitted WinRT
    // remains native; its coverage bit is not the CreateProcessW bit.
    using Permit = bool (*)(void*) noexcept;
    AutoImportResult Install(HMODULE owner, std::span<const AutoImportHook> hooks, Permit permit = nullptr, void* context = nullptr) noexcept;
    AutoImportResult Rollback() noexcept;
    void Commit() noexcept;
    // Reacquires each containing module and restores only our current pointer.
    // Foreign replacement and unloaded/reused module slots are never overwritten.
    AutoImportResult RestoreCommitted() noexcept;
    AutoImportResult CurrentCoverage() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
namespace auto_import_detail {
struct ImportSlot { size_t rva = 0; uint32_t api_index = 0; };
HRESULT InspectImage(std::span<const std::byte> image, std::vector<ImportSlot>& slots) noexcept;
struct SlotChange {
    void* volatile* slot = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;
    DWORD saved_protection = 0;
    bool protection_pending = false, owned = false;
};
// Fault-injection seam used only by direct tests. Production uses VirtualProtect.
using Protect = BOOL (WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
HRESULT ChangeSlot(SlotChange& change, bool install, Protect protect = &VirtualProtect) noexcept;
}
}
