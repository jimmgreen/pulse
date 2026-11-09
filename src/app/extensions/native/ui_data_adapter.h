#pragma once
#include "../../../../sdk/native/include/pulse/ui_data.h"
#include "../../../ui/extension_data_controller.h"
#include <functional>
#include <memory>
#include <optional>
#include <set>
namespace pulse::extensions::native {
struct UiDataBackend {
    std::function<bool()> alive;
    std::function<bool(uint64_t)> valid;
    std::function<std::optional<UiNode>(const std::string &)> node;
    std::function<std::optional<std::string>(const std::string &)> state;
    std::function<std::set<std::string>()> commands;
    std::function<bool(const std::string &, const std::optional<UiDataSource> &)> bind;
    std::function<bool(const std::string &, const std::string &)> request;
};
class UiDataAdapter {
  public:
    explicit UiDataAdapter(UiDataBackend backend);
    ~UiDataAdapter();
    PulseStatus Query(uint32_t version, void *table, uint32_t size);
    ui::UiDataCancel Request(const ui::UiDataRequest &request, ui::UiDataCompletion completion);
    void ResetBindings();
    void Pause(bool value);
    void Stop();
    bool InCallback() const;
  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace pulse::extensions::native
