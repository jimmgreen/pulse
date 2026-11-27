#pragma once
#include <windows.h>
#include <memory>
#include <functional>
#include "../common/picker_places_snapshot.h"
#include "../ui/folder_picker_model.h"
#include <string>
#include <vector>
namespace pulse::ui { struct FolderPickerSpec; struct PickerJumpResult; }
namespace pulse::picker_inprocess {
// The UI polls only immutable, in-memory snapshots. Shell and version resources
// are resolved on a worker; destruction never waits for an Explorer COM server.
struct JumpContextOptions {
    std::function<picker_places::Snapshot()> read_places;
    std::wstring history_root;
    bool enumerate_explorer = true;
};
class JumpContext {
public:
    explicit JumpContext(DWORD process_id, int mode, JumpContextOptions options = {});
    void Accepted(const std::wstring& path);
    // The user removed this folder from the program's 常用 list: drop it from the history.
    void Forget(const std::wstring& path);
    std::vector<ui::PickerJumpResult> Search(const std::wstring& query);
    ~JumpContext();
    bool Poll(ui::FolderPickerSpec& spec);
    JumpContext(const JumpContext&) = delete;
    JumpContext& operator=(const JumpContext&) = delete;
private:
    struct State;
    std::shared_ptr<State> state_;
    unsigned revision_ = 0;
    size_t filter_index_ = static_cast<size_t>(-1);
};
std::vector<ui::PickerJumpResult> FilterJumpResults(std::vector<ui::PickerJumpResult> results,
    ui::PickerMode mode, const ui::PickerOptions& filter);
std::shared_ptr<JumpContext> AttachJumpContext(ui::FolderPickerSpec& spec, DWORD process_id);
}
