#include "setup_options.h"
#include <cstdio>
int main() {
    using namespace pulse::setup;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; };
    Tasks t;
    check(t.global_search && t.ai_assistant, "feature defaults");
    ApplyTaskList(L"globalsearch,!aiassistant,startup", t, true);
    check(t.global_search && !t.ai_assistant && t.startup && !t.index_service && !t.desktop_icon, "replace tasks");
    ApplyTaskList(L"!globalsearch,AIASSISTANT", t, false);
    check(!t.global_search && t.ai_assistant && t.startup, "merge tasks");
    Tasks roundtrip;
    ApplyTaskList(FormatTaskList(t), roundtrip, true);
    check(FormatTaskList(t) == FormatTaskList(roundtrip), "round trip");
    const auto options = ParseOptions(L"setup.exe /TASKS=globalsearch /MERGETASKS=!globalsearch,aiassistant");
    Tasks parsed;
    ApplyTaskList(*options.tasks, parsed, true);
    ApplyTaskList(*options.merge_tasks, parsed, false);
    check(!parsed.global_search && parsed.ai_assistant && !parsed.index_service, "command line");
    ApplyRememberedTaskList(L"indexservice,startup", t);
    check(t.global_search && t.ai_assistant && t.startup, "legacy remembered tasks");
    ApplyRememberedTaskList(L"indexservice,!globalsearch,!aiassistant", t);
    check(!t.global_search && !t.ai_assistant, "remember explicit feature opt out");
    ApplyTaskList(L"indexservice", t, true);
    check(!t.global_search && !t.ai_assistant, "explicit old task list replaces all");
    return failures ? 1 : 0;
}
