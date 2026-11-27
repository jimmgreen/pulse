#include "../common/picker_places_snapshot.h"
#include "../app/app_model.h"
#include <cstdio>
#include <chrono>

using namespace pulse::picker_places;
namespace {
int checks = 0, failures = 0;
void Check(bool okay, const char* text) { ++checks; failures += !okay; std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", text); }
HWND Endpoint() { return CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr); }
pulse::app::TrayShelfRecord Shelf(size_t count) {
    pulse::app::TrayShelfRecord shelf; shelf.id = 42; shelf.name = L"完整托盘";
    pulse::app::TrayBatch batch; batch.id = 9;
    for (size_t i = 0; i < count; ++i) {
        pulse::app::TrayItem item; item.id = i + 100; item.path = L"C:\\资料\\文件-" + std::to_wstring(i) + L".txt";
        item.display_name = L"第" + std::to_wstring(i) + L"项"; item.size = i; item.exists = true;
        batch.items.push_back(std::move(item));
    }
    shelf.batches.push_back(std::move(batch)); return shelf;
}
Snapshot Capture(size_t count, HWND source) { auto shelf = Shelf(count); Snapshot s; CaptureTray(shelf, shelf.name, source, s); return s; }
PROCESS_INFORMATION Start(const std::wstring& args) {
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" " + args;
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION p{};
    CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &p); return p;
}
DWORD Finish(PROCESS_INFORMATION& p) {
    DWORD code = 99;
    if (p.hProcess && WaitForSingleObject(p.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(p.hProcess, &code);
    if (p.hThread) CloseHandle(p.hThread); if (p.hProcess) CloseHandle(p.hProcess); return code;
}
}
int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--reader") {
        Snapshot s;
        return tray_pages_detail::Read(s, argv[2]) == tray_pages_detail::ReadStatus::Valid && s.tray_items.size() == 129 &&
            s.tray_items[64].id == 164 && s.tray_items.back().id == 228 && s.tray_items.back().size == 128 ? 0 : 1;
    }
    if (argc == 5 && std::wstring_view(argv[1]) == L"--publisher") {
        tray_pages_detail::Writer writer(argv[2]); HWND endpoint = Endpoint(); auto s = Capture(129, endpoint);
        tray_pages_detail::Publish(s, writer);
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[3]), quit = OpenEventW(SYNCHRONIZE, FALSE, argv[4]);
        if (!ready || !quit) return 2;
        SetEvent(ready); WaitForSingleObject(quit, 10000); CloseHandle(ready); CloseHandle(quit); DestroyWindow(endpoint); return 0;
    }
    const auto name = L"Local\\Pulse.TrayPagesFixture." + std::to_wstring(GetCurrentProcessId());
    HWND endpoint = Endpoint(); auto s = Capture(129, endpoint); Snapshot read;
    Check(s.tray_available && s.tray_items.size() == 129 && s.tray_total_count == 129, "capture retains all items beyond one 64-row page");
    tray_pages_detail::Writer writer(name);
    Check(tray_pages_detail::Publish(s, writer), "publish complete multi-page generation");
    Check(tray_pages_detail::Read(read, name) == tray_pages_detail::ReadStatus::Valid && read.tray_items == s.tray_items &&
        read.tray_total_count == 129, "worker reassembles full ordered tray rather than a silently truncated first page");
    auto child = Start(L"--reader \"" + name + L"\""); Check(Finish(child) == 0, "full multi-page snapshot crosses actual process boundary");
    const auto original_revision = read.tray_revision;
    Check(ReadTrayPage(read, 1, original_revision, GetCurrentProcessId(), name) && read.tray_items.size() == 64 &&
        read.tray_items.front().id == 164 && read.tray_items.back().id == 227 && read.tray_total_count == 129,
        "explicit middle page retains total count and bound identity");
    Check(ReadTrayPage(read, 2, original_revision, GetCurrentProcessId(), name) && read.tray_items.size() == 1 && read.tray_items[0].id == 228,
        "last page contains exact tail not stale padding");
    Check(!ReadTrayPage(read, 3, original_revision, GetCurrentProcessId(), name) && read.tray_items.empty(), "out-of-range page clears previous results");
    Check(!ReadTrayPage(read, 0, original_revision + 1, GetCurrentProcessId(), name), "page request rejects stale expected revision");
    Check(!ReadTrayPage(read, 0, original_revision, GetCurrentProcessId() + 1, name), "page request rejects changed publisher identity");
    const auto* first_page = writer.pages[0].get();
    tray_pages_detail::Publish(s, writer);
    Check(writer.blob->value.revision == original_revision && writer.pages[0].get() == first_page, "heartbeat updates manifest without rebuilding immutable pages");
    writer.pages[1]->blob->payload.timestamp = GetTickCount64() - 20000;
    Check(tray_pages_detail::Read(read, name) == tray_pages_detail::ReadStatus::Valid && read.tray_items.size() == 129,
        "fresh manifest keeps immutable older pages alive beyond heartbeat window");
    const auto saved_revision = writer.pages[1]->blob->payload.revision;
    ++writer.pages[1]->blob->payload.revision;
    Check(tray_pages_detail::Read(read, name) == tray_pages_detail::ReadStatus::Invalid && read.tray_items.empty() && !read.tray_available,
        "mixed-generation page invalidates whole snapshot without exposing prefix");
    writer.pages[1]->blob->payload.revision = saved_revision;
    writer.pages[1].reset();
    Check(tray_pages_detail::Read(read, name) == tray_pages_detail::ReadStatus::Invalid && read.tray_items.empty(), "missing middle page clears complete result");
    ++s.tray_items[0].size; tray_pages_detail::Publish(s, writer); tray_pages_detail::Read(read, name);
    Check(read.tray_available && read.tray_items.size() == 129 && read.tray_revision != original_revision,
        "changed content publishes coherent fresh generation after missing page");
    const auto valid_manifest = writer.blob->value;
    writer.blob->value.timestamp = GetTickCount64() - 10001;
    Check(tray_pages_detail::Read(read, name, true) == tray_pages_detail::ReadStatus::Invalid && read.tray_items.empty(), "expired manifest clears even cached full snapshot");
    writer.blob->value = valid_manifest; ++writer.blob->value.pages;
    Check(tray_pages_detail::Read(read, name) == tray_pages_detail::ReadStatus::Invalid, "corrupt page count rejected before opening pages");
    writer.blob->value = valid_manifest; InterlockedIncrement(&writer.pages[0]->blob->sequence);
    Check(tray_pages_detail::Read(read, name) == tray_pages_detail::ReadStatus::Invalid && read.tray_items.empty(), "in-progress page never leaks partial tray");
    InterlockedIncrement(&writer.pages[0]->blob->sequence);
    s.tray_items[64].id = s.tray_items[0].id; tray_pages_detail::Publish(s, writer); tray_pages_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty() && read.tray_total_count == 129, "duplicate identity across different pages rejects entire generation");
    s = Capture(kMaxTraySnapshotItems, endpoint); tray_pages_detail::Publish(s, writer); tray_pages_detail::Read(read, name);
    Check(read.tray_available && read.tray_items.size() == 4096 && writer.blob->value.pages == 64 && read.tray_items.back().id == 4195,
        "4096-item upper bound is completely browsable in 64 bounded pages");
    s = Capture(kMaxTraySnapshotItems + 1, endpoint); tray_pages_detail::Publish(s, writer); tray_pages_detail::Read(read, name);
    Check(!read.tray_available && read.tray_items.empty() && read.tray_total_count == 4097,
        "explicit total upper bound reports unavailable and full count without truncation");
    s = Capture(0, endpoint); s.tray_shelf_id = 77; tray_pages_detail::Publish(s, writer); tray_pages_detail::Read(read, name);
    Check(read.tray_available && read.tray_items.empty() && read.tray_shelf_id == 77, "empty shelf switch discards all old pages");
    tray_detail::Writer legacy(name + L".legacy"); auto legacy_source = Capture(2, endpoint); tray_detail::Publish(legacy_source, legacy);
    tray_pages_detail::ReadWithLegacy(read, name + L".absent", name + L".legacy");
    Check(read.tray_available && read.tray_items.size() == 2, "missing v2 falls back to valid v1 publisher");
    writer.blob->value.version = 99; tray_pages_detail::ReadWithLegacy(read, name, name + L".legacy");
    Check(!read.tray_available && read.tray_items.empty(), "invalid v2 never resurrects older v1 items");
    s = Capture(129, endpoint); tray_pages_detail::Publish(s, writer); tray_detail::Publish(s, legacy); Snapshot old;
    tray_detail::Read(old, name + L".legacy");
    Check(!old.tray_available && old.tray_total_count == 129 && old.tray_items.empty(), "legacy v1 readers keep explicit over-capacity behavior");
    {
        tray_pages_detail::Writer other(name); auto newer = Capture(65, endpoint); tray_pages_detail::Publish(newer, other);
        const auto other_revision = other.blob->value.revision; tray_pages_detail::Publish(s, writer); tray_pages_detail::Read(read, name);
        Check(read.tray_available && read.tray_items.size() == 129 && read.tray_revision != other_revision,
            "independent writers cannot reuse another writer generation on heartbeat");
    }
    {
        tray_pages_detail::Publisher async(name + L".async", name + L".async-old"); async.Post(s);
        for (int i = 0; i < 100; ++i) {
            tray_pages_detail::Read(read, name + L".async"); if (read.tray_available) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Check(read.tray_available && read.tray_items.size() == 129, "dedicated publisher posts full memory snapshot on background worker");
    }
    tray_pages_detail::Read(read, name + L".async"); Check(!read.tray_available && read.tray_items.empty(), "publisher destruction joins worker and releases mappings");
    const auto child_name = name + L".child", ready_name = name + L".ready", quit_name = name + L".quit";
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()), quit = CreateEventW(nullptr, TRUE, FALSE, quit_name.c_str());
    child = Start(L"--publisher \"" + child_name + L"\" \"" + ready_name + L"\" \"" + quit_name + L"\"");
    const bool signaled = WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0;
    HANDLE retained = signaled ? OpenFileMappingW(FILE_MAP_READ, FALSE, child_name.c_str()) : nullptr;
    tray_pages_detail::Read(read, child_name); Check(signaled && retained && read.tray_items.size() == 129, "child publisher exposes real paged generation");
    SetEvent(quit); Check(Finish(child) == 0, "paged publisher child exits normally"); tray_pages_detail::Read(read, child_name);
    Check(!read.tray_available && read.tray_items.empty() && !read.tray_revision, "publisher death clears retained manifest and identity");
    if (retained) CloseHandle(retained); CloseHandle(ready); CloseHandle(quit); DestroyWindow(endpoint);
    std::printf("checks=%d failures=%d\n", checks, failures); return failures ? 1 : 0;
}
