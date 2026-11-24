// ai_agent_read.cpp - Explicit-consent read_file tool for Pulse 助手.
#include "ai_agent_internal.h"
#include "ai_file_reader.h"

namespace pulse::ai_agent {

void ToolReadFile(AppState& s, const app::ai::ModelToolCall& call, const json::Value& args) {
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
    int64_t max_chars = 10000;
    if (const json::Value* m = args.find("max_chars")) m->get_int(max_chars, 500, 24000);
    if (!Exists(path)) {
        AddToolResult(a, call.id, ToolOutcome::Fail("not_found", "File does not exist."));
        ++a.call_index;
        NextTool(s);
        return;
    }

    const std::wstring leaf = Leaf(path);
    const std::string call_id = call.id;
    const uint64_t gen = a.generation;

    auto run_read = [path, leaf, max_chars, call_id, gen](AppState& st) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return;
        AddStep(st, ag, ui::AiIcon::Document, Text(I::AiAsstStepReadFile), leaf, Text(I::AiAsstStepReadFileRun));
        RunJob(
            st,
            [path, max_chars](const std::atomic<bool>& cancelled) -> ToolOutcome {
                const app::ai::LocalFileExcerpt ex =
                    app::ai::ReadLocalFileExcerpt(path, static_cast<size_t>(max_chars), cancelled);
                if (!ex.ok) return ToolOutcome::Fail(ex.error_code, ex.error_message);
                json::Value data = json::Value::Object();
                data.set("ok", true);
                data.set("untrusted_file_content", true);
                data.set("path", json::Utf8(path));
                data.set("format", ex.format);
                data.set("size_bytes", static_cast<int64_t>(ex.size_bytes));
                data.set("truncated", ex.truncated);
                if (ex.is_image) {
                    data.set("is_image", true);
                    data.set("note", "This is an image file. Ask the user to attach or paste it in the composer to inspect its visual content.");
                } else {
                    data.set("text", ex.text);
                }
                ToolOutcome out;
                out.data = std::move(data);
                return out;
            },
            [call_id](AppState& s2, const ToolOutcome& out) {
                AiAgent& ag2 = Agent(s2);
                std::wstring sub;
                if (out.ok) {
                    const std::string txt = out.data.get_string("text");
                    size_t chars = 0;
                    for (unsigned char c : txt)
                        if ((c & 0xC0u) != 0x80u) ++chars;
                    sub = Fill(Text(I::AiAsstStepReadFileDone), L"{n}", Num(chars));
                } else {
                    sub = Fill(Text(I::AiAsstStepFailed), L"{reason}", json::Wide(out.error.message));
                }
                EndStep(s2, ag2, out.ok, sub);
                AddToolResult(ag2, call_id, out);
                ++ag2.call_index;
                AiChatPush(s2, 150, [](AppState& s3) {
                    NextTool(s3);
                    return -1;
                });
            });
    };

    // If the user already attached this file in the current turn, skip duplicate prompt.
    const std::wstring norm = ui::AiListLower(NormDir(path));
    if (a.attached_paths.count(norm) != 0) {
        run_read(s);
        return;
    }

    // Privacy boundary: always ask explicit user confirmation before reading an unattached file.
    FoldSteps(s, a);
    AiChat& c = Chat(s);
    AiBlockView& cf = c.Add(AiBlockKind::Confirm, Now());
    cf.icon = ui::AiIcon::Document;
    cf.title = Text(I::AiAsstConfirmReadTitle);
    cf.sub = Fill(Fill(Fill(Text(I::AiAsstConfirmReadSub), L"{name}", leaf), L"{n}", Num(static_cast<size_t>(max_chars))),
                  L"{host}", AiAgentHost());
    cf.accept = Text(I::AiAsstConfirmReadAccept);
    cf.deny = Text(I::Cancel);
    c.confirm_id = cf.id;
    AiChatSetChip(c, ui::StatusAiState::Waiting, Text(I::AiAsstChipWaiting));

    const uint32_t confirm_id = cf.id;
    c.on_confirm = [confirm_id, leaf, call_id, gen, run_read](AppState& st, bool accepted) {
        AiAgent& ag = Agent(st);
        if (ag.generation != gen) return;
        if (AiBlockView* b = Block(st, confirm_id)) {
            b->done = true;
            b->declined = !accepted;
            b->title = Fill(Text(accepted ? I::AiAsstConfirmReadDone : I::AiAsstConfirmReadDeny), L"{name}", leaf);
        }
        AiChat& ch = Chat(st);
        ch.confirm_id = 0;
        ch.on_confirm = nullptr;
        if (!accepted) {
            json::Value decl = json::Value::Object();
            decl.set("ok", false);
            decl.set("declined", true);
            decl.set("reason", "The user declined sharing this file's contents.");
            ToolOutcome out;
            out.data = std::move(decl);
            AddToolResult(ag, call_id, out);
            ++ag.call_index;
            AiChatSetChip(ch, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
            NextTool(st);
            return;
        }
        AiChatSetChip(ch, ui::StatusAiState::Busy, Text(I::AiAsstThinking));
        run_read(st);
    };
}

} // namespace pulse::ai_agent
