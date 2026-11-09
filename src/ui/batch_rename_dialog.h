// batch_rename_dialog.h — Modal 批量重命名 dialog (steps + live preview).
#pragma once
#include "../app/batch_rename.h"
#include "../app/batch_rename_plan.h"
#include "../app/batch_rename_presets.h"
#include <windows.h>
#include <d2d1.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pulse::ui {

struct BatchRenameDialogResult {
    bool accepted = false;
    app::BatchRenameRule rule;                 // legacy; unused by the step dialog
    std::vector<app::BatchRenameItem> items;   // Ok items in a chain-safe order
    bool executed = false;                     // options.execute ran them (task below)
    uint64_t task = 0;
};

// What options.poll reports about a running rename task.
struct BatchRenameProgress {
    bool finished = false;
    size_t done = 0;   // items handled so far
};

// One request to the model for 一句话生成步骤, run by the caller.
struct BatchRenameAiTask {
    virtual ~BatchRenameAiTask() = default;
    // UI thread, every frame while it runs. True once finished; then either
    // reply (the model's text, UTF-8) or error (shown to the user) is set.
    virtual bool Poll(std::string& reply, std::wstring& error) = 0;
    virtual void Cancel() = 0;
};

struct BatchRenameDialogOptions {
    // Host-prepared names are reviewed using the normal preview and confirmation.
    // Sources/plan are prepared off the UI thread; this mode never edits rules or presets.
    std::optional<std::vector<std::wstring>> proposed_names;
    std::vector<app::rename::Source> proposed_sources;
    std::optional<app::rename::Plan> proposed_plan;
    std::function<bool()> review_valid;
    // Internal isolated-host probe only; delivered after two actual preview frames.
    std::function<void(HWND, std::function<bool(const std::wstring&)>)> on_review_ready;

    std::wstring ai_model;   // shown in the AI bar and sent as "model"; empty = no connection
    // Starts a chat/completions request with this body. Null (with error
    // set) when it cannot start. Unset: the AI bar cannot send.
    std::function<std::shared_ptr<BatchRenameAiTask>(const std::string& body, std::wstring& error)> ai_start;
    std::wstring ai_provider;   // the connection's service, as the privacy note names it
    // 按内容命名. can_read: whether text can be read from this extension
    // (".pdf"). read_texts runs on a worker thread: reads up to max_chars of
    // text from each path in order and calls done(i, text) for each (empty:
    // nothing readable); stops when cancelled() turns true. Unset: content
    // naming skips every file.
    std::function<bool(const std::wstring& ext)> can_read;
    std::function<void(const std::vector<std::wstring>& paths, size_t max_chars, const std::function<bool()>& cancelled,
                       const std::function<void(size_t, std::wstring)>& done)>
        read_texts;
    // When set, Enter runs the renames while the dialog shows them going
    // through (progress line, a check per row) and closes when they are done.
    // Returns the task id (0: nothing started). Unset: the caller runs items.
    std::function<uint64_t(const std::vector<app::BatchRenameItem>&)> execute;
    std::function<BatchRenameProgress(uint64_t)> poll;
    // Opens with these steps instead of the usual first preset (Pulse 助手's
    // 在批量重命名中调整…). Template tokens may be the English ones.
    std::optional<app::rename::Preset> preset;
};

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner,
                                              const std::vector<std::wstring>& paths,
                                              bool dark,
                                              D2D1_COLOR_F accent,
                                              const BatchRenameDialogOptions& options = {});

} // namespace pulse::ui
