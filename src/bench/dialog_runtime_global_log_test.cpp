// Exercise the production global logger without redirecting a shipping path.
#define PULSE_DIALOG_HISTORY_GLOBAL 1
#define main BrowseContractMain
#include "browse_folder_runtime_test.cpp"
#undef main

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    namespace log = pulse::diagnostics::runtime;
    const auto root = std::filesystem::absolute(argv[1]);
    if (!std::filesystem::create_directory(root)) return 2;
    if (!log::Initialize(root.wstring(), "dialog_runtime", {256 * 1024, 60000})) return 3;
    char program[] = "browse_contract";
    char contract[] = "--contract";
    char* arguments[]{program, contract};
    const int contract_result = BrowseContractMain(2, arguments);
    Check(contract_result == 0, "global logging preserves all contract results");
    Check(log::Flush(5000), "production async logger flushes owned fixture");
    const auto health = log::GetHealth();
    Check(health.write_errors == 0 && health.dropped_events == 0 && health.pending_events == 0,
        "global receipts have no write errors or dropped events");
    log::Shutdown();
    const auto directory = root / L"Diagnostics" / L"Runtime";
    std::string records;
    unsigned files = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() != L".jsonl") continue;
        std::ifstream input(file.path());
        records.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        ++files;
    }
    Check(files == 1, "owned directory contains actual dialog_runtime JSONL file");
    Check(Count(records, "\"event\":\"dialog_identity\"") == 1 &&
        records.find("\"app_basename\":\"dialog_runtime_global_log_test.exe\"") != std::string::npos &&
        records.find("\"identity_revision\":2") != std::string::npos &&
        records.find("\"creation_time_filetime\":0") == std::string::npos,
        "real global identity carries executable basename and creation time");
    Check(Count(records, "\"event\":\"dialog_show_begin\"") == 12 &&
        Count(records, "\"event\":\"dialog_show_terminal\"") == 12 &&
        Count(records, "\"source_event_sequence\":") == 24 &&
        Count(records, "\"source_session\":") == 25,
        "production global branch writes all begin terminal and deduplication keys");
    Check(Count(records, "\"terminal_status\":\"accepted\"") == 6 &&
        Count(records, "\"terminal_status\":\"cancelled\"") == 6,
        "global terminal status preserves successful and cancelled outcomes");
    Check(records.find("probe.txt") == std::string::npos && records.find("Browse fixture") == std::string::npos &&
        records.find("selected_filename") == std::string::npos && records.find("document_path") == std::string::npos,
        "actual global file excludes selected documents and dialog titles");
    std::printf("GLOBAL_LOG checks=%u failures=%u files=%u\n", checks, failures, files);
    return failures ? 1 : 0;
}
