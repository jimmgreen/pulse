#pragma once
#include "file_dialog_contract.h"
#include "file_dialog_places.h"
#include "file_dialog_properties_view.h"
#include "../picker_inprocess/picker_ui_api_v2.h"
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace pulse::shell_bridge {
// Explicit injection only. Keep the implementation module alive through Shutdown.
// Production callers require advertised capabilities; cap-zero UI is test-only.
struct FileDialogInprocessProvider {
    PulsePickerUiV2Api api{};
    std::shared_ptr<void> module_lifetime;
    bool allow_unverified_proof = false;
    // Remote providers bind HWND to their authenticated live child and session.
    std::function<bool(PulsePickerUiV2Session, HWND)> validate_remote_window;
};
struct FileDialogInprocessSnapshot {
    uint64_t show = 0, generation = 0;
    HWND window = nullptr;
    uint32_t flags = 0, filter_index = 0;
    std::wstring folder, filename, selection, candidate;
    std::vector<std::wstring> selections, candidates;
};
struct FileDialogCustomItem {
    uint32_t id = 0, state = 3;
    std::wstring label;
};
struct FileDialogCustomControl {
    uint32_t kind = 0, id = 0, parent = 0, state = 3;
    bool has_parent = false;
    std::wstring label;
    std::vector<FileDialogCustomItem> items;
    bool has_selected = false;
    uint32_t selected_item_id = 0;
    bool checked = false;
    std::wstring text;
    PulsePickerUiV2Control Abi(std::vector<PulsePickerUiV2ControlItem>& backing) const {
        backing.clear(); backing.reserve(items.size());
        for (const auto& item : items) backing.push_back({sizeof(PulsePickerUiV2ControlItem), item.id, item.state, 0, {item.label.data(), static_cast<uint32_t>(item.label.size())}});
        PulsePickerUiV2Control value{}; value.size = sizeof(value); value.kind = kind; value.id = id;
        value.parent_id = parent; value.has_parent = has_parent ? 1u : 0u; value.state = state;
        value.label = {label.data(), static_cast<uint32_t>(label.size())};
        value.items = backing.data(); value.item_count = static_cast<uint32_t>(backing.size());
        value.has_selected = has_selected ? 1u : 0u; value.selected_item_id = selected_item_id;
        value.checked=checked ? 1u : 0u;value.text={text.data(),static_cast<uint32_t>(text.size())};
        return value;
    }
};
class FileDialogInprocessSession {
public:
    explicit FileDialogInprocessSession(std::shared_ptr<const FileDialogInprocessProvider> provider, std::function<void(const char*)> diagnostic = {});
    ~FileDialogInprocessSession();
    FileDialogInprocessSession(const FileDialogInprocessSession&) = delete;
    FileDialogInprocessSession& operator=(const FileDialogInprocessSession&) = delete;
    HRESULT Prepare(const ContractDecision&, HWND owner, const std::vector<FileDialogCustomControl>& controls = {}, const std::vector<FileDialogPlace>& places = {}, const std::vector<FileDialogPropertyView>& properties = {});
    HRESULT UpdateControl(const FileDialogCustomControl&);
    HRESULT Navigate(const std::wstring& folder);
    HRESULT Show(const PulsePickerUiV2Callbacks&, PulsePickerUiV2Result&, std::wstring& path);
    HRESULT Observe(const PulsePickerUiV2Event&);
    HRESULT ObserveOverwrite(const PulsePickerUiV2OverwriteEvent&);
    HRESULT ObserveShareViolation(const PulsePickerUiV2OverwriteEvent&);
    void LeaveEvent() noexcept { event_kind_ = 0; event_generation_ = 0; }
    HRESULT Read(FileDialogInprocessSnapshot&) const noexcept;
    HRESULT CompleteFileOk(HRESULT);
    HRESULT GetWindow(HWND*) const;
    HRESULT Close(HRESULT);
    bool InFileOk() const noexcept { return event_kind_ == PULSE_PICKER_UI_V2_EVENT_FILE_OK; }
    const std::vector<std::wstring>& AcceptedPaths() const noexcept { return accepted_paths_; }
    bool Closed() const noexcept { return close_requested_; }
    HRESULT CloseResult() const noexcept { return close_result_; }
private:
    void Report(const char*) const noexcept;
    std::function<void(const char*)> diagnostic_;
    std::shared_ptr<const FileDialogInprocessProvider> provider_;
    PulsePickerUiV2Session session_ = 0;
    PulsePickerUiV2Config config_{};
    std::wstring folder_, filename_, extension_, title_, ok_label_, filename_label_;
    std::vector<std::wstring> labels_, patterns_;
    std::vector<PulsePickerUiV2Filter> filters_;
    std::vector<FileDialogPropertyView> properties_;
    std::vector<std::vector<PulsePickerUiV2Text>> property_values_;
    std::vector<PulsePickerUiV2Property> property_abi_;
    std::vector<FileDialogPlace> places_;
    std::vector<PulsePickerUiV2Place> place_abi_;
    std::vector<FileDialogCustomControl> custom_;
    std::vector<PulsePickerUiV2Control> controls_;
    std::vector<std::vector<PulsePickerUiV2ControlItem>> control_items_;
    uint64_t show_ = 0, event_generation_ = 0, accepted_generation_ = 0;
    std::wstring accepted_candidate_;
    std::vector<std::wstring> accepted_paths_;
    uint32_t accepted_index_ = 0;
    uint32_t event_kind_ = 0;
    bool close_requested_ = false;
    HRESULT close_result_ = S_OK;
};
}
