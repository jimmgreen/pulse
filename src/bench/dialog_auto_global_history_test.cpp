// Exercise the same auto-entry build and real bounded logger without attachment.
#define PULSE_RUNTIME_ENTRY_TEST
#ifndef PULSE_RUNTIME_SOURCE
#define PULSE_RUNTIME_SOURCE "../shell_bridge/file_dialog_runtime.cpp"
#endif
#include PULSE_RUNTIME_SOURCE
#include "../common/dialog_takeover_history.h"
#include "../common/runtime_log.h"
#include <fstream>
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    namespace fs = std::filesystem;
    namespace log = pulse::diagnostics::runtime;
    namespace history = pulse::dialog_history;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool yes, const char* name) {
        ++checks; if (!yes) ++failures;
        std::printf("[%s] %s\n", yes ? "PASS" : "FAIL", name);
    };
    const auto root = fs::absolute(argv[1]);
    if (!fs::create_directory(root)) return 2;
    if (!log::Initialize(root.wstring(), "dialog_runtime", {256 * 1024, 60000})) return 3;
    const auto session = root / L"session";
    fs::create_directory(session);
    Configuration state;
    state.admin = true;
    state.control = session.wstring();
    state.show_log_records = 0;
    shell::FileDialogShowObservation value;
    value.factory = shell::DialogFactory::Save;
    value.sequence = 1;
    LogShow(state, "IFileDialog", 1, true, value);
    value.native_fallback = true;
    value.contract_reason = shell::NativeReason::UnsupportedExtension;
    value.fallback = shell::FileDialogShowFallback::Contract;
    LogShow(state, "IFileDialog", 1, false, value);
    value = {};
    value.factory = shell::DialogFactory::Open;
    value.sequence = 1;
    LogShow(state, "IFileDialog", 2, true, value);
    value.native_fallback = true;
    value.contract_reason = shell::NativeReason::UnsupportedFilename;
    value.fallback = shell::FileDialogShowFallback::Contract;
    LogShow(state, "IFileDialog", 2, false, value);
    check(log::Flush(5000), "auto-entry global receipts flush");
    const auto health = log::GetHealth();
    check(!health.write_errors && !health.dropped_events && !health.pending_events,
        "auto-entry logger reports no write errors or dropped receipts");
    log::Shutdown();
    const auto global = root / L"Diagnostics" / L"Runtime";
    history::History archive;
    const auto collected = history::CollectTakeoverHistory(global.wstring(), {}, archive);
    const auto classified = [&] {
        return archive.records.size() == 1 && archive.records[0].count == 2 &&
            archive.records[0].state == history::State::Fallback && archive.records[0].reason == history::Reason::UnsupportedRequest &&
            archive.records[0].operation == history::Operation::Unknown;
    };
    check(!collected.error && classified(), "global-only archive groups two administrator Open and Save contract fallbacks");
    const auto& identity = shell::application_identity::CurrentApplication();
    check(archive.identities.size() == 1 && archive.identities[0].creation_time == identity.creation_time &&
        archive.identities[0].image_id == identity.instance_id && archive.identities[0].pid == GetCurrentProcessId(),
        "global receipt retains target producer identity, not helper identity");
    const auto duplicate = history::CollectTakeoverHistory(global.wstring(), {session.wstring()}, archive);
    check(!duplicate.error && !duplicate.added && classified(),
        "global and session receipts share sequence and deduplicate");
    history::History local_first;
    const auto local = history::CollectTakeoverHistory((root / L"absent").wstring(), {session.wstring()}, local_first);
    const auto global_again = history::CollectTakeoverHistory(global.wstring(), {}, local_first);
    check(!local.error && !global_again.error && !global_again.added && local_first.records.size() == 1 &&
        local_first.records[0].count == 2,
        "session-first then global receipt also deduplicates");
    std::string terminal;
    for (const auto& file : fs::directory_iterator(global)) {
        if (file.path().extension() != L".jsonl") continue;
        std::ifstream input(file.path());
        std::string line;
        while (std::getline(input, line)) if (line.find("\"event\":\"dialog_show_terminal\"") != line.npos) terminal += line + '\n';
    }
    check(terminal.find("source_session") != terminal.npos && terminal.find("source_event_sequence") != terminal.npos &&
        terminal.find("selected_path") == terminal.npos && terminal.find("document_path") == terminal.npos &&
        terminal.find("SetDefaultExtension") == terminal.npos,
        "terminal preserves deduplication keys without document text");
    check(terminal.find("UnsupportedFilename") != terminal.npos && terminal.find("UnsupportedExtension") != terminal.npos,
        "global native receipts preserve both parameter contract reasons");
    std::printf("AUTO_GLOBAL_HISTORY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

