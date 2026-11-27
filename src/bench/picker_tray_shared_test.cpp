#include "../common/picker_places_snapshot.h"
#include "../app/app_model.h"
#include <cstdio>

using namespace pulse::picker_places;
namespace {
int checks = 0, failures = 0;
void Check(bool okay, const char* text) { ++checks; failures += !okay; std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", text); }
HWND Endpoint() { return CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr); }
pulse::app::TrayShelfRecord Fixture() {
    pulse::app::TrayShelfRecord shelf; shelf.id = 42; shelf.name = L"工作架";
    pulse::app::TrayBatch batch; batch.id = 9;
    pulse::app::TrayItem file; file.id = 10; file.path = L"C:\\资料\\截图.png"; file.display_name = L"屏幕截图";
    file.size = 123; file.exists = true; file.content_kind = 1; file.owned_snippet = true;
    file.source_label = L"截图来源"; file.domain = L"example.test"; file.attrs = FILE_ATTRIBUTE_ARCHIVE;
    batch.items.push_back(file);
    file.id = 11; file.path = L"\\\\server\\share\\目录"; file.display_name = L"目录"; file.is_dir = true;
    file.size = 0; file.exists = false; file.content_kind = 0; file.owned_snippet = false; file.inflight_task = 99;
    batch.items.push_back(file); shelf.batches.push_back(batch); return shelf;
}
PROCESS_INFORMATION Start(const std::wstring& args) {
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + args;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION p{};
    CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &p);
    return p;
}
DWORD Finish(PROCESS_INFORMATION& p) {
    DWORD code = 99;
    if (p.hProcess && WaitForSingleObject(p.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(p.hProcess, &code);
    if (p.hThread) CloseHandle(p.hThread); if (p.hProcess) CloseHandle(p.hProcess);
    return code;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("tray-test entered argc=%d\n", argc);
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--reader") {
        Snapshot s; tray_detail::Read(s, argv[2]);
        return s.tray_available && s.tray_items.size() == 2 && s.tray_items[0].id == 10 &&
            s.tray_items[0].size == 123 && s.tray_items[1].is_directory && s.tray_items[1].inflight ? 0 : 1;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"--publisher") {
        tray_detail::Writer writer(argv[2]); auto shelf = Fixture(); Snapshot s;
        HWND endpoint = Endpoint(); CaptureTray(shelf, shelf.name, endpoint, s); tray_detail::Publish(s, writer);
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]), quit = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        if (!ready || !quit) return 2;
        SetEvent(ready); WaitForSingleObject(quit, 10000); CloseHandle(ready); CloseHandle(quit);
        DestroyWindow(endpoint); return 0;
    }
    const auto name = L"Local\\Pulse.TrayFixture." + std::to_wstring(GetCurrentProcessId());
    tray_detail::Writer writer(name); HWND endpoint = Endpoint();
    auto shelf = Fixture(); Snapshot captured;
    CaptureTray(shelf, shelf.name, endpoint, captured);
    Check(captured.tray_available && captured.tray_shelf_id == 42 && captured.tray_total_count == 2 && captured.tray_items.size() == 2,
        "actual TrayShelfRecord captured in active-shelf batch order");
    const auto before = captured.tray_items;
    CaptureTray(shelf, shelf.name, endpoint, captured);
    Check(captured.tray_items == before && shelf.batches[0].items[0].owned_snippet && shelf.batches[0].items[1].inflight_task == 99,
        "const capture preserves model paths owned backing and in-flight state");
    tray_detail::Publish(captured, writer); Snapshot read;
    tray_detail::Read(read, name);
    Check(read.tray_available && read.tray_items == before && read.tray_shelf_name == shelf.name && read.tray_revision == 1,
        "tray mapping roundtrips identity Unicode UNC labels type size exists inflight and snippet metadata");
    auto child = Start(L"--reader \"" + name + L"\"");
    Check(Finish(child) == 0, "tray snapshot crosses a real process boundary");
    tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(read.tray_revision == 1, "identical heartbeat does not change consumption revision");
    captured.tray_items[0].size = 124; tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(read.tray_revision == 2 && read.tray_items[0].size == 124, "metadata change advances revision");
    Snapshot worker; Check(RefreshTraySnapshot(worker, read), "worker accepts initial tray snapshot");
    Check(!RefreshTraySnapshot(worker, read), "unchanged snapshot avoids redundant UI revision");
    shelf.id = 43; shelf.name = L"另一架"; shelf.batches.clear(); CaptureTray(shelf, shelf.name, endpoint, captured);
    tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(read.tray_available && read.tray_items.empty() && read.tray_shelf_id == 43 && read.tray_revision == 3,
        "switch to empty shelf clears items instead of retaining previous shelf");
    Check(RefreshTraySnapshot(worker, read) && worker.tray_items.empty(), "worker propagates empty shelf");
    shelf = Fixture(); shelf.batches[0].items = std::vector<pulse::app::TrayItem>(65, shelf.batches[0].items.front());
    CaptureTray(shelf, shelf.name, endpoint, captured); tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty() && read.tray_total_count == 65 && read.tray_shelf_id == 42,
        "over-limit shelf is explicitly unavailable with full count and no truncated items");
    shelf = Fixture(); shelf.batches[0].items = std::vector<pulse::app::TrayItem>(64, shelf.batches[0].items.front());
    for (size_t i = 0; i < shelf.batches[0].items.size(); ++i) shelf.batches[0].items[i].id = i + 100;
    CaptureTray(shelf, shelf.name, endpoint, captured); tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    if (!read.tray_available || read.tray_items.size() != 64)
        std::printf("capacity diagnostics captured=%zu available=%d wire_count=%lu wire_available=%lu wire_valid=%d total=%llu first_path_length=%zu first_id=%llu last_id=%llu\n",
            captured.tray_items.size(), captured.tray_available, writer.blob->payload.count, writer.blob->payload.available,
            tray_detail::Valid(writer.blob->payload), captured.tray_total_count, captured.tray_items.empty() ? 0 : captured.tray_items[0].path.size(),
            captured.tray_items.empty() ? 0 : captured.tray_items.front().id, captured.tray_items.empty() ? 0 : captured.tray_items.back().id);
    Check(read.tray_available && read.tray_items.size() == 64 && read.tray_items.back().id == 163,
        "exact 64-item capacity preserves all identities and order");
    const auto populated_revision = read.tray_revision;
    shelf.batches.clear(); CaptureTray(shelf, shelf.name, endpoint, captured); tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(read.tray_available && read.tray_items.empty() && read.tray_shelf_id == 42 && read.tray_revision > populated_revision,
        "clearing same active shelf advances revision and clears items");
    shelf = Fixture();
    for (uint8_t kind = 2; kind <= 3; ++kind) {
        shelf.batches[0].items[0].content_kind = kind;
        CaptureTray(shelf, shelf.name, endpoint, captured); tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
        Check(read.tray_available && read.tray_items[0].content_kind == kind && read.tray_items[0].owned_snippet,
            "text and link materializations retain backing ownership metadata without mutation");
    }
    shelf = Fixture(); CaptureTray(shelf, shelf.name, endpoint, captured);
    captured.tray_items[1].path = L"relative"; tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty(), "invalid row rejects whole snapshot not a valid prefix");
    CaptureTray(shelf, shelf.name, endpoint, captured); captured.tray_items[1].id = captured.tray_items[0].id;
    tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty(), "duplicate item identity never reaches consumer");
    CaptureTray(shelf, shelf.name, endpoint, captured); captured.tray_items[0].path = L"C:\\" + std::wstring(800, L'x');
    tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(read.tray_available && read.tray_items[0].path == captured.tray_items[0].path, "bounded long paths roundtrip without IO");
    captured.tray_items[0].path.assign(1024, L'x'); tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty(), "overlong path disables snapshot without truncation");
    CaptureTray(shelf, shelf.name, endpoint, captured); captured.tray_items[0].label.assign(256, L'x');
    tray_detail::Publish(captured, writer); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty(), "overlong label cannot truncate protocol content");
    CaptureTray(shelf, shelf.name, endpoint, captured); tray_detail::Publish(captured, writer);
    const auto valid = std::make_unique<tray_detail::Payload>(writer.blob->payload);
    const auto reject = [&](const char* text, auto mutate) {
        writer.blob->payload = *valid; mutate(writer.blob->payload); tray_detail::Read(read, name);
        Check(!read.tray_available && read.tray_items.empty() && read.tray_revision == 0 && read.tray_shelf_id == 0, text);
    };
    reject("expired tray clears items and identity", [](auto& p) { p.timestamp = GetTickCount64() - 10001; });
    reject("future timestamp rejected", [](auto& p) { p.timestamp = GetTickCount64() + 10000; });
    reject("unknown tray version rejected", [](auto& p) { ++p.version; });
    reject("unknown tray flags rejected", [](auto& p) { p.rows[0].flags = 16; });
    reject("unknown content kind rejected", [](auto& p) { p.rows[0].content_kind = 4; });
    reject("invalid identity rejected", [](auto& p) { p.rows[0].id = 0; });
    reject("unterminated path rejected", [](auto& p) { std::wmemset(p.rows[0].path, L'x', 1024); });
    reject("row count mismatch rejected", [](auto& p) { ++p.total_count; });
    reject("publisher owner mismatch rejected", [](auto& p) { ++p.pid; });
    writer.blob->payload = *valid; InterlockedIncrement(&writer.blob->sequence); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty(), "in-progress writer clears stale tray instead of blocking");
    InterlockedIncrement(&writer.blob->sequence);
    Snapshot disconnected; Check(RefreshTraySnapshot(worker, disconnected) && !worker.tray_available && worker.tray_items.empty(), "worker clears disconnected source");
    // Retain the mapping after the child publisher exits to test liveness, not mere absence.
    const auto child_name = name + L".child", ready_name = name + L".ready", quit_name = name + L".quit";
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()), quit = CreateEventW(nullptr, TRUE, FALSE, quit_name.c_str());
    child = Start(L"--publisher \"" + child_name + L"\" \"" + ready_name + L"\" \"" + quit_name + L"\"");
    const bool signaled = WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0;
    HANDLE retained = signaled ? OpenFileMappingW(FILE_MAP_READ, FALSE, child_name.c_str()) : nullptr;
    tray_detail::Read(read, child_name); Check(signaled && retained && read.tray_available, "live child publisher supplies a real snapshot");
    SetEvent(quit); Check(Finish(child) == 0, "child publisher exits normally"); tray_detail::Read(read, child_name);
    Check(!read.tray_available && read.tray_items.empty() && !read.tray_publisher_pid, "dead publisher clears retained mapped snapshot");
    if (retained) CloseHandle(retained); CloseHandle(ready); CloseHandle(quit);
    DestroyWindow(endpoint); tray_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty(), "dead endpoint invalidates parent snapshot");
    std::printf("checks=%d failures=%d\n", checks, failures); return failures ? 1 : 0;
}
