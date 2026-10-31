#pragma once
#include <windows.h>
#include <propsys.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace pulse::shell_bridge {
struct DialogPropertyField {
    PROPERTYKEY key{};
    VARTYPE type = VT_EMPTY;
    std::wstring label;
    std::vector<std::wstring> values;
};
// Bounded in-memory snapshots only. No property handlers or file writes.
class FileDialogProperties {
public:
    HRESULT SetStore(IPropertyStore* store) noexcept;
    HRESULT SetCollected(IPropertyDescriptionList* list) noexcept;
    HRESULT Edit(REFPROPERTYKEY key, const std::vector<std::wstring>& values) noexcept;
    HRESULT Snapshot(IPropertyStore** store) const noexcept;
    HRESULT ResetEdits() noexcept;
    const std::vector<DialogPropertyField>& Fields() const noexcept { return fields_; }
private:
    struct Description { PROPERTYKEY key{}; VARTYPE type = VT_EMPTY; std::wstring label; };
    static HRESULT Clone(IPropertyStore* source, IPropertyStore** result);
    HRESULT MakeFields(IPropertyStore* store, const std::vector<Description>& descriptions,
        std::vector<DialogPropertyField>& result) const;
    Microsoft::WRL::ComPtr<IPropertyStore> original_;
    Microsoft::WRL::ComPtr<IPropertyStore> working_;
    std::vector<Description> descriptions_;
    std::vector<DialogPropertyField> fields_;
};
}
