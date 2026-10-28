// ai_agent_dupfind.cpp - run_actions' file.find_duplicates step for AI apps
// (task 46 ⑧): the 重复文件 tab's compare (dup_finder.h), waited for here, and
// its groups back to the app as paths, sizes and dates. Reading files to
// compare them is a read; moving the copies stays with the user (the tab's
// button asks every time).
#include "ai_agent_internal.h"
#include "dup_finder.h"

namespace pulse {
namespace ai_agent {

// How long the call waits for a compare before answering with what it has
// (the tab keeps going); the MCP call itself gives up after 2 minutes.
constexpr uint64_t kDupStepWaitMs = 100000;

bool DupStepAsk(AppState& s, std::shared_ptr<CtlRun> run, const json::Value& args) {
    auto st = std::make_shared<CtlStep>();
    st->icon = ui::AiIcon::Duplicate;
    st->title = StepTitle(s, "file.find_duplicates");
    const auto answer = [run, st] {
        run->file_step = st;
        run->file_state = 2;
    };
    if (!run->mcp) {
        // The assistant has its own flow (E) with the funnel and group cards.
        st->error = "file.find_duplicates is for AI apps; in Pulse use find_duplicates (its cards keep the user in charge)";
        st->why = Text(I::AiActWhyLater);
        answer();
        return false;
    }
    std::vector<std::wstring> names;
    if (const json::Value* f = args.find("folders"); f && f->is_array())
        for (size_t i = 0; i < f->size() && i < 8; ++i)
            if (f->at(i).is_string()) names.push_back(json::Wide(f->at(i).as_string()));
    if (const json::Value* f = args.find("folders"); f && f->is_string()) names.push_back(json::Wide(f->as_string()));
    uint64_t min_bytes = app::dup::kDefaultMinBytes;
    int64_t n = 0;
    if (const json::Value* m = args.find("min_size"); m && m->get_int(n, 0, int64_t{1} << 42) && n > 0)
        min_bytes = static_cast<uint64_t>(n);
    const bool show = args.get_bool("show", true);
    std::string error;
    std::wstring why;
    const uint32_t id = app::dup::Start(s, names, min_bytes, show, &error, &why);
    if (!id) {
        st->error = error.empty() ? std::string("cannot compare these folders") : error;
        st->why = why;
        answer();
        return false;
    }
    run->file_state = 1;
    const uint64_t started = Now();
    Later(s, *run, 120, [run, st, id, started](AppState& s2) {
        if (RunGone(s2, *run)) return -1;
        if (!app::dup::Finished(id) && Now() - started < kDupStepWaitMs) return 120;
        json::Value out;
        if (!app::dup::ResultJson(id, out) || out.get_bool("cancelled")) {
            st->error = "the compare was cancelled (the user closed its tab)";
            st->why = Text(I::Canceled);
        } else {
            st->ok = true;
            st->arg = app::dup::Summary(id);
            st->sub = st->arg;
            if (out.get_bool("running"))
                out.set("note", "still comparing after 100 s; the user sees the groups in the tab when it ends");
            st->data = std::move(out);
        }
        run->file_step = st;
        run->file_state = 2;
        CtlNext(s2, run);
        return -1;
    });
    return true;
}

}  // namespace ai_agent
}  // namespace pulse
