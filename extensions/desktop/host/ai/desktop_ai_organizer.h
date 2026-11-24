// desktop_ai_organizer.h - Native AI Desktop Organizer integrated directly with
// Pulse's built-in AI Assistant (%LOCALAPPDATA%\Pulse\ai\ai.json + DPAPI model_key*.bin).
// Reads live desktop metadata & short text snippets, requests structured archive plans
// via WinHTTP OpenAI-compatible chat completions (with instant cancellation), validates
// safety invariants, executes collision-safe real-folder archiving, and supports 100%
// lossless rollback (Undo).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "host/model/desktop_model.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::desktop::ai {

enum class AiOrganizeStrategy : uint8_t {
    ArchiveClutter = 0,   // 归档散落与旧文件（保留近期活跃工作项）
    ByProjectTopic = 1,   // 按项目与主题归档整理
    FullDesktop = 2,      // 全盘收纳桌面文件到子目录
};

struct AiConnectionInfo {
    bool configured = false;
    bool assistant_enabled = true;
    bool needs_key = true;
    std::string service_id;             // e.g. "deepseek", "qwen", "ollama", "gemini"
    std::wstring provider_name;         // e.g. L"DeepSeek", L"通义千问", L"Ollama"
    std::string model_id;               // e.g. "deepseek-flash"
    std::wstring base_url;              // resolved endpoint URL
    std::wstring badge_text() const {
        if (!configured) return L"未连接 AI 助手";
        std::wstring m(model_id.begin(), model_id.end());
        return provider_name.empty() ? m : (provider_name + L" · " + m);
    }
};

struct DesktopAiCandidate {
    std::wstring path;
    std::wstring name;
    std::wstring extension;
    std::wstring snippet;               // <=160 chars plain-text/shortcut/tag context
    std::wstring current_group_title;
    uint64_t size_bytes = 0;
    int32_t age_days = 0;
    bool is_folder = false;
    bool folder_is_empty = false;
    bool is_shortcut = false;
};

struct ExistingDesktopFolder {
    std::wstring name;
    std::wstring path;
    std::vector<std::wstring> sample_children; // up to 6 child names for semantic matching
};

struct AiArchiveItemEntry {
    std::wstring name;
    std::wstring source_path;
    uint64_t size_bytes = 0;
    int32_t age_days = 0;
    bool is_folder = false;
    bool selected = true;
};

struct AiArchiveGroupPlan {
    std::wstring folder_name;
    std::wstring target_folder_path;
    std::wstring reason;
    bool folder_already_exists = false;
    std::vector<AiArchiveItemEntry> items;

    size_t SelectedCount() const noexcept {
        size_t n = 0;
        for (const auto& it : items) {
            if (it.selected) ++n;
        }
        return n;
    }
    bool AllSelected() const noexcept {
        return !items.empty() && SelectedCount() == items.size();
    }
};

struct AiOrganizePlan {
    bool from_ai_model = true;
    std::wstring model_badge;
    std::wstring summary;
    size_t desktop_before_count = 0;
    std::vector<AiArchiveGroupPlan> groups;

    size_t SelectedItemCount() const noexcept {
        size_t total = 0;
        for (const auto& g : groups) total += g.SelectedCount();
        return total;
    }
    size_t ActiveFolderCount() const noexcept {
        size_t folders = 0;
        for (const auto& g : groups) {
            if (g.SelectedCount() > 0) ++folders;
        }
        return folders;
    }
    size_t NewFolderCount() const noexcept {
        size_t created = 0;
        for (const auto& g : groups) {
            if (!g.folder_already_exists && g.SelectedCount() > 0) ++created;
        }
        return created;
    }
    uint64_t SelectedTotalBytes() const noexcept {
        uint64_t bytes = 0;
        for (const auto& g : groups) {
            for (const auto& it : g.items) {
                if (it.selected) bytes += it.size_bytes;
            }
        }
        return bytes;
    }
    size_t DesktopAfterCount() const noexcept {
        const size_t moved = SelectedItemCount();
        const size_t created = NewFolderCount();
        const size_t base = (desktop_before_count >= moved) ? (desktop_before_count - moved) : 0;
        return base + created;
    }
};

struct ArchivedFileMoveRecord {
    std::wstring original_path;
    std::wstring archived_path;
};

struct AiArchiveUndoTransaction {
    std::vector<ArchivedFileMoveRecord> moved_items;
    std::vector<std::wstring> created_folders;
    model::DesktopModelState pre_archive_model;

    bool empty() const noexcept {
        return moved_items.empty() && created_folders.empty();
    }
};

struct AiArchiveExecutionResult {
    bool ok = false;
    size_t moved_count = 0;
    size_t created_folder_count = 0;
    std::wstring error_message;
    AiArchiveUndoTransaction undo_tx;
};

// Probes %LOCALAPPDATA%\Pulse\ai\ai.json + DPAPI model_key*.bin (optional override_ai_dir for tests).
AiConnectionInfo ProbePulseAiAssistantConnection(const std::wstring& override_ai_dir = {});

//Validates a proposed Windows subfolder name (rejects reserved names, slashes, '..', >80 chars).
bool IsValidArchiveFolderName(std::wstring_view folder_name) noexcept;

// Scans desktop items, resolves existing desktop subfolders + sample child names, and extracts
// lightweight plain-text snippets (<=160 chars) for text/markdown/code/config files.
void CollectDesktopAiContext(const std::wstring& desktop_dir,
                             std::span<const model::DesktopItem> items,
                             std::span<const model::DesktopGroup> groups,
                             int64_t now_unix_ms,
                             std::vector<DesktopAiCandidate>& out_candidates,
                             std::vector<ExistingDesktopFolder>& out_existing_folders);

// Builds the OpenAI-compatible chat/completions JSON body (with provider-appropriate thinking suppression).
std::string BuildAiOrganizeChatBody(std::string_view service_id,
                                    std::string_view model_id,
                                    AiOrganizeStrategy strategy,
                                    std::wstring_view custom_instruction,
                                    std::span<const DesktopAiCandidate> candidates,
                                    std::span<const ExistingDesktopFolder> existing_folders);

// Parses AI reply (tolerating markdown code fences) and strictly validates every folder & item against candidates.
bool ParseAndValidateAiOrganizeReply(std::string_view raw_utf8,
                                     const std::wstring& desktop_dir,
                                     std::span<const DesktopAiCandidate> candidates,
                                     std::span<const ExistingDesktopFolder> existing_folders,
                                     AiOrganizePlan& out_plan,
                                     std::wstring& out_error);

// Offline / local semantic organizer when AI Assistant is unconfigured or user chooses local preview.
AiOrganizePlan BuildSmartLocalOrganizePlan(const std::wstring& desktop_dir,
                                           AiOrganizeStrategy strategy,
                                           std::wstring_view custom_instruction,
                                           std::span<const DesktopAiCandidate> candidates,
                                           std::span<const ExistingDesktopFolder> existing_folders);

// Executes checked moves into real Desktop subfolders (collision-safe KeepBoth naming, zero overwrites)
// and records a complete Undo transaction. If pin_created_folders_as_groups is true, also updates inout_post_model.
AiArchiveExecutionResult ExecuteAiArchivePlan(const AiOrganizePlan& plan,
                                              const model::DesktopModelState& pre_model,
                                              bool pin_created_folders_to_tiles = false,
                                              model::DesktopModelState* out_updated_model = nullptr);

// Reverses ExecuteAiArchivePlan: moves every archived item back to its original Desktop path,
// removes newly created archive folders if empty, and restores pre_archive_model.
bool UndoAiArchiveTransaction(const AiArchiveUndoTransaction& tx,
                              model::DesktopModelState& out_restored_model,
                              size_t* out_restored_files = nullptr);

// Non-destructive option: applies checked AI groups as Desktop Virtual Groups without moving disk files.
void ApplyAiPlanAsVirtualGroups(const AiOrganizePlan& plan,
                                model::DesktopModelState& inout_model,
                                const model::RectI& work_area,
                                const model::RectI& tile_board_rect);

// Asynchronous background session with instant WinHttpCloseHandle cancellation.
class AiOrganizeSession {
public:
    struct Request {
        std::wstring desktop_dir;
        std::wstring override_ai_dir;
        AiOrganizeStrategy strategy = AiOrganizeStrategy::ArchiveClutter;
        std::wstring custom_instruction;
        bool force_local_fallback = false;
        std::vector<model::DesktopItem> items;
        std::vector<model::DesktopGroup> groups;
        int64_t now_unix_ms = 0;
    };

    struct Outcome {
        bool ok = false;
        bool cancelled = false;
        bool unconfigured = false;
        int http_status = 0;
        std::wstring error_message;
        AiOrganizePlan plan;
    };

    static std::shared_ptr<AiOrganizeSession> Start(Request req,
                                                    std::function<void(Outcome)> on_complete);
    ~AiOrganizeSession();
    AiOrganizeSession(const AiOrganizeSession&) = delete;
    AiOrganizeSession& operator=(const AiOrganizeSession&) = delete;

    void Cancel() noexcept;
    bool IsCancelled() const noexcept;

    struct Impl;
private:
    AiOrganizeSession() = default;
    std::shared_ptr<Impl> impl_;
};

} // namespace pulse::desktop::ai
