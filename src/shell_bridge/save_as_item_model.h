#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace pulse::shell_bridge {
struct SaveAsItemSnapshot {
    uint64_t generation = 0;
    bool cleared = false, filename_set = false;
    HRESULT filename_result = E_PENDING;
    Microsoft::WRL::ComPtr<IShellItem> original_item, source_parent, initial_folder;
    std::wstring source_path, source_parent_path, source_name;
    std::wstring initial_directory, initial_filename;
};
struct SaveAsItemSeed {
    Microsoft::WRL::ComPtr<IShellItem> original_item, initial_folder;
    Microsoft::WRL::ComPtr<IPropertyStore> metadata_snapshot;
    std::wstring directory, filename;
    bool cleared = false, filename_set = false;
};
// Caller-STA capture only; not an accepted result, property import or provider
// authorization. Keep the original native HRESULT and every existing gate.
// A failed native setter can itself clear native state: failed capture therefore
// invalidates seed access instead of silently reusing the prior snapshot.
class SaveAsItemModel {
public:
    SaveAsItemModel() = default;
    SaveAsItemModel(const SaveAsItemModel&) = delete;
    SaveAsItemModel& operator=(const SaveAsItemModel&) = delete;
    HRESULT ObserveSetSaveAsItem(IShellItem* item, IFileDialog* native, HRESULT native_result) noexcept;
    HRESULT RefreshAfterSetter(IFileDialog* native, HRESULT native_result) noexcept;
    HRESULT Snapshot(SaveAsItemSnapshot* output) const noexcept;
    // A metadata combination additionally requires the wrapper's owned, detached
    // property snapshot. Paths alone never authorize the property-store contract.
    HRESULT BuildFilesystemSeed(bool property_contract_requested, SaveAsItemSeed* output,
        IPropertyStore* owned_metadata_snapshot = nullptr) const noexcept;
    HRESULT Reset() noexcept;
    bool HasCall() const noexcept { return has_call_; }
    uint64_t Generation() const noexcept { return snapshot_.generation; }
    HRESULT LastCaptureResult() const noexcept { return capture_result_; }
private:
    HRESULT Begin(HRESULT native_result, uint64_t& epoch) noexcept;
    HRESULT Commit(SaveAsItemSnapshot&& candidate, uint64_t epoch) noexcept;
    DWORD thread_ = GetCurrentThreadId();
    uint64_t epoch_ = 0;
    bool capturing_ = false, has_call_ = false;
    HRESULT capture_result_ = E_PENDING;
    SaveAsItemSnapshot snapshot_;
};
}
