// dup_finder.h - 查找重复文件 without the assistant (task 46): compares a folder
// and its subfolders with the local engine of flow E (ai_dup_scan.h) and lists
// the groups in a 重复文件 tab (pulse:ai-list:<id>) with what each keeps and why.
// Moving the extra copies to the Recycle Bin asks every time and can be undone.
// UI thread only, except the scan itself.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace ops { struct FinishedTask; }
namespace ui { enum class PaneBannerAction : int; }
namespace json { class Value; }
}

namespace pulse::app::dup {

// Default smallest file compared (flow E's): empty and tiny files are noise.
constexpr uint64_t kDefaultMinBytes = 1024;

// Starts comparing `roots` (folder paths, known names; empty: the active
// folder) in a new tab when `show`, else in the background (an AI app asked
// and only wants the answer). Returns the run's id, or 0 with `error` (short
// English, for an AI app) and `why` (for the user).
uint32_t Start(AppState& s, const std::vector<std::wstring>& roots, uint64_t min_bytes, bool show,
               std::string* error = nullptr, std::wstring* why = nullptr);
// 查找重复文件 (palette, folder background menu): the active folder, in a new tab.
void FindInFolder(AppState& s);

// The run's state, for an AI app's step (file.find_duplicates) waiting on it.
bool Finished(uint32_t id);   // also true for an unknown id
// {folders, compared_files, groups:[{size, keep, extra:[...], reason}], ...}:
// paths, sizes and dates only, never contents. False for an unknown id.
bool ResultJson(uint32_t id, json::Value& out);
// 下载 · 4 组: the run's line in an AI app's activity summary.
std::wstring Summary(uint32_t id);

bool IsDupTab(const std::wstring& path);
// Per UI tick: follows the scans this window started. True: repaint.
bool Tick(AppState& s);

// The tab's banner buttons (取消, 保留规则 ▾, 把多出的 N 份移到回收站).
void BannerPress(AppState& s, ui::PaneBannerAction action);
// Row menu: 只保留这份 (one selected row) and 按规则重新标 (the selected rows'
// groups, or all of them).
bool CanKeepSelected(AppState& s);
void KeepSelected(AppState& s);
void ReapplyRules(AppState& s);

// The recycle this window submitted: it shows its own snack (腾出 216 MB).
bool OwnsTask(uint64_t task_id);
void NoteFinished(AppState& s, const ops::FinishedTask& done);

}  // namespace pulse::app::dup
