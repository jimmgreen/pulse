#pragma once
#include "../app/extensions/ui_tree.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace pulse::ui {
struct UiDataRequest {
    std::string node_id, source, filter, sort, cursor;
    std::string operation = "read";
    int page_size = 100;
    uint64_t generation = 0;
};
struct UiDataPage {
    std::string status = "ready";
    std::vector<extensions::UiDataRow> rows;
    std::string next_cursor, revision, error;
    int progress = 100;
    bool delta = false;
    std::string base_revision;
    std::vector<std::string> removed_ids;
};
using UiDataCompletion = std::function<void(UiDataPage)>;
using UiDataCancel = std::function<void()>;
using UiDataProvider = std::function<UiDataCancel(const UiDataRequest &, UiDataCompletion)>;
// Provider completion may run on any thread. Drain and all other methods run on the owning UI thread.
class ExtensionDataController {
  public:
    using Apply = std::function<void(const UiDataPage &)>;
    ExtensionDataController(UiDataProvider provider, std::function<void()> wake, Apply apply);
    ~ExtensionDataController();
    void Request(UiDataRequest request, bool append = false);
    void Cancel();
    void Drain();
    void Close();
    const std::string &NextCursor() const { return next_cursor_; }

  private:
    struct Inbox;
    std::shared_ptr<Inbox> inbox_;
    UiDataProvider provider_;
    Apply apply_;
    UiDataCancel cancel_;
    std::vector<extensions::UiDataRow> rows_;
    std::string next_cursor_, revision_;
    bool append_ = false;
    bool has_snapshot_ = false;
};
} // namespace pulse::ui
