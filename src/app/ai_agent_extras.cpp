// ai_agent_extras.cpp - Archive inspect/extract and dual-pane/folder compare tools for Pulse 助手.
#include "ai_agent_internal.h"
#include "ai_archive_tools.h"

namespace pulse::ai_agent {
namespace {

std::wstring DefaultExtractFolder(const std::wstring& archive_path) {
    std::wstring p = archive_path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    const std::wstring parent = slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
    std::wstring leaf = slash == std::wstring::npos ? p : p.substr(slash + 1);
    const size_t dot = leaf.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) leaf.resize(dot);
    if (leaf.empty()) leaf = L"Extracted";
    return parent.empty() ? leaf : JoinPath(parent, leaf);
}

json::Value DiffItemsToJson(const std::vector<app::ai::FolderDiffItem>& items, size_t cap = 60) {
    json::Value arr = json::Value::Array();
    for (size_t i = 0; i < items.size() && i < cap; ++i) {
        json::Value o = json::Value::Object();
        o.set("name", json::Utf8(items[i].name));
        o.set("is_dir", items[i].is_dir);
        if (!items[i].is_dir) {
            if (items[i].left_size > 0) o.set("left_size", static_cast<int64_t>(items[i].left_size));
            if (items[i].right_size > 0) o.set("right_size", static_cast<int64_t>(items[i].right_size));
        }
        arr.push(std::move(o));
    }
    return arr;
}

} // namespace

void ToolInspectArchive(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    std::wstring path;
    ToolOutcome err;
    const json::Value* pv = args.find("path");
    if (!pv || !app::ai::CheckPathArg(*pv, "path", path, err)) {
        AddToolResult(a, call.id, pv ? err : ToolOutcome::Fail("invalid_args", "path is required"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    int64_t max_entries = 150;
    if (const json::Value* m = args.find("max_entries")) m->get_int(max_entries, 10, 500);
    if (!Exists(path)) {
        AddToolResult(a, call.id, ToolOutcome::Fail("not_found", "Archive file does not exist."));
        ++a.call_index;
        NextTool(s);
        return;
    }

    const std::wstring leaf = Leaf(path);
    const std::string call_id = call.id;
    AddStep(s, a, ui::AiIcon::Package, Text(I::AiAsstStepArchiveList), leaf, Text(I::AiAsstStepArchiveListRun));
    RunJob(
        s,
        [path, max_entries](const std::atomic<bool>& cancelled) -> ToolOutcome {
            const app::ai::ArchiveInspectResult r =
                app::ai::InspectArchive(path, static_cast<size_t>(max_entries), cancelled);
            if (!r.ok) return ToolOutcome::Fail(r.error_code, r.error_message);
            json::Value data = json::Value::Object();
            data.set("ok", true);
            data.set("path", json::Utf8(path));
            data.set("format", r.format);
            data.set("total_entries", static_cast<int64_t>(r.total_entries));
            data.set("file_count", static_cast<int64_t>(r.file_count));
            data.set("dir_count", static_cast<int64_t>(r.dir_count));
            data.set("total_uncompressed_bytes", static_cast<int64_t>(r.total_uncompressed));
            data.set("has_encrypted", r.has_encrypted);
            data.set("truncated", r.truncated);
            json::Value arr = json::Value::Array();
            for (const auto& e : r.entries) {
                json::Value item = json::Value::Object();
                item.set("path", json::Utf8(e.path));
                item.set("is_dir", e.is_dir);
                if (!e.is_dir) item.set("size", static_cast<int64_t>(e.size));
                if (e.encrypted) item.set("encrypted", true);
                arr.push(std::move(item));
            }
            data.set("entries", std::move(arr));
            ToolOutcome out;
            out.data = std::move(data);
            return out;
        },
        [call_id](AppState& st, const ToolOutcome& out) {
            AiAgent& ag = Agent(st);
            std::wstring sub;
            if (out.ok) {
                int64_t total = 0, bytes = 0;
                if (const json::Value* v = out.data.find("total_entries")) v->get_int(total, 0, INT64_MAX);
                if (const json::Value* v = out.data.find("total_uncompressed_bytes")) v->get_int(bytes, 0, INT64_MAX);
                sub = Fill(Fill(Text(I::AiAsstStepArchiveListDone), L"{n}", Num(static_cast<size_t>(total))),
                           L"{size}", AiChatFormatSize(static_cast<uint64_t>(bytes)));
            } else {
                sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
            }
            EndStep(st, ag, out.ok, sub);
            AddToolResult(ag, call_id, out);
            ++ag.call_index;
            AiChatPush(st, 150, [](AppState& s2) {
                NextTool(s2);
                return -1;
            });
        });
}

void ToolExtractArchive(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    std::wstring path;
    ToolOutcome err;
    const json::Value* pv = args.find("path");
    if (!pv || !app::ai::CheckPathArg(*pv, "path", path, err)) {
        AddToolResult(a, call.id, pv ? err : ToolOutcome::Fail("invalid_args", "path is required"));
        ++a.call_index;
        NextTool(s);
        return;
    }
    if (!Exists(path)) {
        AddToolResult(a, call.id, ToolOutcome::Fail("not_found", "Archive file does not exist."));
        ++a.call_index;
        NextTool(s);
        return;
    }
    std::wstring dest;
    if (const json::Value* dv = args.find("dest"); dv && dv->is_string() && !dv->as_string().empty()) {
        dest = ResolveTaskDest(json::Wide(dv->as_string()));
    } else {
        dest = DefaultExtractFolder(path);
    }
    if (dest.empty()) {
        AddToolResult(a, call.id, ToolOutcome::Fail("invalid_args", "Could not resolve destination folder."));
        ++a.call_index;
        NextTool(s);
        return;
    }
    std::vector<std::wstring> entries;
    if (const json::Value* ev = args.find("entries"); ev && ev->is_array()) {
        for (const json::Value& item : ev->items()) {
            if (item.is_string() && !item.as_string().empty()) entries.push_back(json::Wide(item.as_string()));
        }
    }

    const std::wstring leaf = Leaf(path);
    const std::wstring dest_label = FolderLabel(dest);
    const std::string call_id = call.id;
    const uint64_t gen = a.generation;

    // File-creation boundary: always show an amber confirmation card before extracting.
    FoldSteps(s, a);
    AiChat& c = Chat(s);
    AiBlockView& cf = c.Add(AiBlockKind::Confirm, Now());
    cf.icon = ui::AiIcon::Package;
    cf.title = Text(I::AiAsstConfirmExtractTitle);
    cf.sub = Fill(Fill(Text(I::AiAsstConfirmExtractSub), L"{name}", leaf), L"{folder}", dest_label);
    cf.accept = Text(I::AiAsstConfirmExtractAccept);
    cf.deny = Text(I::Cancel);
    c.confirm_id = cf.id;
    AiChatSetChip(c, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));

    const uint32_t confirm_id = cf.id;
    c.on_confirm = [confirm_id, path, dest, dest_label, leaf, entries = std::move(entries), call_id, gen](
                       AppState& st, bool accepted) mutable {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return;
        if (AiBlockView* b = Block(st, confirm_id)) {
            b->done = true;
            b->declined = !accepted;
        }
        AiChat& ch = Chat(st);
        ch.confirm_id = 0;
        ch.on_confirm = nullptr;
        if (!accepted) {
            json::Value decl = json::Value::Object();
            decl.set("ok", false);
            decl.set("declined", true);
            decl.set("reason", "The user cancelled archive extraction.");
            ToolOutcome out;
            out.data = std::move(decl);
            AddToolResult(ag, call_id, out);
            ++ag.call_index;
            AiChatSetChip(ch, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            NextTool(st);
            return;
        }
        AiChatSetChip(ch, ui::StatusAiState::Busy, Text(I::AiAsstStepArchiveExtractRun));
        AddStep(st, ag, ui::AiIcon::Package, Text(I::AiAsstStepArchiveExtract), leaf,
                Text(I::AiAsstStepArchiveExtractRun));
        RunJob(
            st,
            [path, dest, entries](const std::atomic<bool>& cancelled) -> ToolOutcome {
                const app::ai::ArchiveExtractResult r = app::ai::ExtractArchive(path, dest, entries, cancelled);
                if (!r.ok) return ToolOutcome::Fail(r.error_code, r.error_message);
                json::Value data = json::Value::Object();
                data.set("ok", true);
                data.set("dest", json::Utf8(dest));
                data.set("extracted_files", static_cast<int64_t>(r.extracted_files));
                data.set("extracted_dirs", static_cast<int64_t>(r.extracted_dirs));
                data.set("extracted_bytes", static_cast<int64_t>(r.extracted_bytes));
                data.set("skipped_unsafe", static_cast<int64_t>(r.skipped_unsafe));
                json::Value top = json::Value::Array();
                for (const std::wstring& n : r.top_level_created) top.push(json::Utf8(n));
                data.set("created", std::move(top));
                ToolOutcome out;
                out.data = std::move(data);
                return out;
            },
            [call_id, dest, dest_label](AppState& s2, const ToolOutcome& out) {
                AiAgent& ag2 = Agent(s2);
                std::wstring sub;
                if (out.ok) {
                    int64_t files = 0;
                    if (const json::Value* v = out.data.find("extracted_files")) v->get_int(files, 0, INT64_MAX);
                    sub = Fill(Fill(Text(I::AiAsstStepArchiveExtractDone), L"{n}", Num(static_cast<size_t>(files))),
                               L"{folder}", dest_label);
                    RefreshPath(s2, dest);
                    // Also refresh dest's parent if dest was newly created.
                    const size_t slash = dest.find_last_of(L'\\');
                    if (slash != std::wstring::npos && slash > 2) {
                        RefreshPath(s2, dest.substr(0, slash));
                        AiListFlashRows(dest.substr(0, slash), {dest.substr(slash + 1)});
                    }
                    std::vector<std::wstring> created;
                    if (const json::Value* c_arr = out.data.find("created"); c_arr && c_arr->is_array()) {
                        for (const json::Value& it : c_arr->items()) created.push_back(json::Wide(it.as_string()));
                    }
                    if (!created.empty()) AiListFlashRows(dest, created);
                } else {
                    sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
                }
                EndStep(s2, ag2, out.ok, sub);
                AiChatSetChip(Chat(s2), ui::StatusAiState::Busy, Text(I::AiAsstThinking));
                AddToolResult(ag2, call_id, out);
                ++ag2.call_index;
                AiChatPush(s2, 150, [](AppState& s3) {
                    NextTool(s3);
                    return -1;
                });
            });
    };
}

void ToolCompareFolders(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
    AiAgent& a = Agent(s);
    std::wstring left, right;
    bool using_panes = false;
    if (const json::Value* lv = args.find("left"); lv && lv->is_string() && !lv->as_string().empty()) {
        ToolOutcome err;
        if (!app::ai::CheckPathArg(*lv, "left", left, err)) {
            AddToolResult(a, call.id, err);
            ++a.call_index;
            NextTool(s);
            return;
        }
    }
    if (const json::Value* rv = args.find("right"); rv && rv->is_string() && !rv->as_string().empty()) {
        ToolOutcome err;
        if (!app::ai::CheckPathArg(*rv, "right", right, err)) {
            AddToolResult(a, call.id, err);
            ++a.call_index;
            NextTool(s);
            return;
        }
    }
    if (left.empty() || right.empty()) {
        std::vector<app::Pane*> panes;
        if (Root(s)) Root(s)->CollectPanes(panes);
        if (panes.size() >= 2 && panes[0] && panes[1]) {
            const app::Tab* ta = panes[0]->ActiveTab();
            const app::Tab* tb = panes[1]->ActiveTab();
            if (ta && tb && !ta->current_path.empty() && !tb->current_path.empty() &&
                !fs::IsVirtualPath(ta->current_path) && !fs::IsVirtualPath(tb->current_path)) {
                if (left.empty()) left = ta->current_path;
                if (right.empty()) right = tb->current_path;
                using_panes = true;
            }
        }
    }
    if (left.empty() || right.empty()) {
        AddToolResult(a, call.id,
                      ToolOutcome::Fail("need_two_folders",
                                        "Provide both 'left' and 'right' folder paths, or open two real folders in split panes."));
        ++a.call_index;
        NextTool(s);
        return;
    }

    const bool activate_ui = args.get_bool("activate_ui", using_panes);
    if (using_panes && activate_ui) {
        SetFolderCompare(s, true);
    }

    const std::wstring arg_label = FolderLabel(left) + L" \u2194 " + FolderLabel(right);
    const std::string call_id = call.id;
    AddStep(s, a, ui::AiIcon::Rows, Text(I::AiAsstStepCompare), arg_label, {});
    RunJob(
        s,
        [left, right](const std::atomic<bool>& cancelled) -> ToolOutcome {
            const app::ai::FolderCompareSummary r = app::ai::CompareFolderPaths(left, right, cancelled);
            if (!r.ok) return ToolOutcome::Fail(r.error_code, r.error_message);
            json::Value data = json::Value::Object();
            data.set("ok", true);
            data.set("left", json::Utf8(r.left_path));
            data.set("right", json::Utf8(r.right_path));
            data.set("left_total", static_cast<int64_t>(r.left_total));
            data.set("right_total", static_cast<int64_t>(r.right_total));
            data.set("same_count", static_cast<int64_t>(r.same_count));
            data.set("only_left_count", static_cast<int64_t>(r.only_left.size()));
            data.set("only_right_count", static_cast<int64_t>(r.only_right.size()));
            data.set("newer_left_count", static_cast<int64_t>(r.newer_left.size()));
            data.set("newer_right_count", static_cast<int64_t>(r.newer_right.size()));
            data.set("different_size_count", static_cast<int64_t>(r.different_size.size()));
            data.set("only_left", DiffItemsToJson(r.only_left));
            data.set("only_right", DiffItemsToJson(r.only_right));
            data.set("newer_left", DiffItemsToJson(r.newer_left));
            data.set("newer_right", DiffItemsToJson(r.newer_right));
            data.set("different_size", DiffItemsToJson(r.different_size));
            ToolOutcome out;
            out.data = std::move(data);
            return out;
        },
        [call_id](AppState& st, const ToolOutcome& out) {
            AiAgent& ag = Agent(st);
            std::wstring sub;
            if (out.ok) {
                int64_t ol = 0, or_ = 0, nl = 0, nr = 0, ds = 0, same = 0;
                if (const json::Value* v = out.data.find("only_left_count")) v->get_int(ol, 0, INT64_MAX);
                if (const json::Value* v = out.data.find("only_right_count")) v->get_int(or_, 0, INT64_MAX);
                if (const json::Value* v = out.data.find("newer_left_count")) v->get_int(nl, 0, INT64_MAX);
                if (const json::Value* v = out.data.find("newer_right_count")) v->get_int(nr, 0, INT64_MAX);
                if (const json::Value* v = out.data.find("different_size_count")) v->get_int(ds, 0, INT64_MAX);
                if (const json::Value* v = out.data.find("same_count")) v->get_int(same, 0, INT64_MAX);
                const size_t diff = static_cast<size_t>(ol + or_ + nl + nr + ds);
                sub = Fill(Fill(Text(I::AiAsstStepCompareDone), L"{n}", Num(diff)), L"{same}",
                           Num(static_cast<size_t>(same)));
            } else {
                sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
            }
            EndStep(st, ag, out.ok, sub);
            AddToolResult(ag, call_id, out);
            ++ag.call_index;
            AiChatPush(st, 150, [](AppState& s2) {
                NextTool(s2);
                return -1;
            });
        });
}

} // namespace pulse::ai_agent
