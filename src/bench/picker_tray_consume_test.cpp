#include "../app/picker_tray_consume.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>

namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
namespace {
namespace pp = pulse::picker_places;
using pulse::app::StagingTray;
int failures = 0;
void Check(bool ok, const char* text) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); if (!ok) ++failures; }
struct Receiver { StagingTray tray; HWND window = nullptr; };
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCCREATE) {
        auto* receiver = static_cast<Receiver*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(receiver)); receiver->window = hwnd;
    }
    if (message == WM_COPYDATA) {
        auto* receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        pp::TrayConsumeRequest request;
        if (!receiver || !pp::DecodeConsumeTray(reinterpret_cast<COPYDATASTRUCT*>(lp), request)) return 0;
        return pp::EncodeConsumeTrayResult(pulse::app::ApplyPickerTrayConsume(receiver->tray, request, GetCurrentProcessId(), hwnd));
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
HWND MakeWindow(Receiver& receiver) {
    return CreateWindowExW(0, L"PulseTrayConsumeIsolatedTest", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                          GetModuleHandleW(nullptr), &receiver);
}
pp::Snapshot Capture(const Receiver& receiver, uint64_t shelf) {
    pp::Snapshot snapshot;
    pp::CaptureTray(*receiver.tray.FindShelf(shelf), L"fixture", receiver.window, snapshot);
    snapshot.endpoint = receiver.window; snapshot.endpoint_pid = GetCurrentProcessId(); snapshot.shared_available = true;
    return snapshot;
}
pp::TrayConsumeRequest Request(const pp::Snapshot& snapshot) {
    pp::TrayConsumeRequest request{snapshot.tray_publisher_pid, snapshot.tray_source, snapshot.tray_shelf_id, {}};
    for (const auto& item : snapshot.tray_items) request.items.push_back({item.id, item.batch_id, item.path});
    return request;
}
pp::TrayConsumeResult Apply(Receiver& receiver, const pp::TrayConsumeRequest& request) {
    return pulse::app::ApplyPickerTrayConsume(receiver.tray, request, GetCurrentProcessId(), receiver.window);
}
void PumpUntil(HANDLE done) {
    const auto deadline = GetTickCount64() + 15000;
    while (WaitForSingleObject(done, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        MsgWaitForMultipleObjects(1, &done, FALSE, 20, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    Check(WaitForSingleObject(done, 0) == WAIT_OBJECT_0, "worker/process completed within deadline");
}
pp::TrayConsumeResult WorkerSend(const pp::Snapshot& snapshot) {
    pp::TrayConsumeResult result;
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker([&] { result = pp::RequestConsumeTray(snapshot, snapshot.tray_items); SetEvent(done); });
    PumpUntil(done); worker.join(); CloseHandle(done); return result;
}
std::wstring File(const std::filesystem::path& root, const wchar_t* name) {
    const auto path = root / name; std::ofstream stream(path, std::ios::binary); stream << "backing survives consume"; stream.close(); return path.wstring();
}
bool Has(const StagingTray& tray, uint64_t shelf, uint64_t id) {
    const auto* found = tray.FindShelf(shelf);
    if (found) for (const auto& batch : found->batches) for (const auto& item : batch.items) if (item.id == id) return true;
    return false;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 9 && std::wstring(argv[1]) == L"--send") {
        pp::Snapshot snapshot;
        snapshot.endpoint = snapshot.tray_source = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(argv[2], nullptr, 10)));
        snapshot.endpoint_pid = snapshot.tray_publisher_pid = static_cast<DWORD>(_wcstoui64(argv[3], nullptr, 10));
        snapshot.tray_shelf_id = _wcstoui64(argv[4], nullptr, 10);
        snapshot.shared_available = snapshot.tray_available = true;
        pp::TrayItem item; item.id = _wcstoui64(argv[5], nullptr, 10); item.batch_id = _wcstoui64(argv[6], nullptr, 10);
        item.shelf_id = snapshot.tray_shelf_id; item.path = argv[7];
        const auto result = pp::RequestConsumeTray(snapshot, {item});
        return result.accepted && result.removed == 1 && result.skipped_owned == 0 ? 0 : 1;
    }
    if (argc < 2) return 2;
    const auto root = std::filesystem::absolute(argv[1]); std::filesystem::create_directories(root);
    const auto file = File(root, L"regular.txt"), second = File(root, L"second.txt"), owned = File(root, L"owned.txt");
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"PulseTrayConsumeIsolatedTest";
    RegisterClassW(&wc);
    Receiver receiver; MakeWindow(receiver);
    Check(receiver.window != nullptr, "isolated hidden receiver created");
    const auto shelf = receiver.tray.CreateShelf(L"original");
    receiver.tray.CollectRichToShelf(shelf, {{.path = file}, {.path = second}}, false);
    auto snapshot = Capture(receiver, shelf); auto request = Request(snapshot);
    const auto active = receiver.tray.CreateShelf(L"other"); receiver.tray.SwitchShelf(active);
    receiver.tray.CollectRichToShelf(active, {{.path = file}}, false);
    const auto other_item = Capture(receiver, active).tray_items[0].id;
    if (argc > 2 && std::wstring(argv[2]) == L"--baseline") {
        const bool removed = receiver.tray.RemoveItemById(request.items[0].item_id);
        Check(removed, "baseline active-only RemoveItemById consumes captured non-active shelf");
        std::printf("baseline_removed=%u original_retained=%u active_unchanged=%u\n", removed,
            Has(receiver.tray, shelf, request.items[0].item_id), receiver.tray.active_shelf_id() == active);
        DestroyWindow(receiver.window); return failures ? 1 : 0;
    }
    const auto unchanged = receiver.tray.FullSnapshotJson();
    for (int mutation = 0; mutation != 6; ++mutation) {
        auto bad = request;
        if (mutation == 0) ++bad.publisher_pid;
        if (mutation == 1) bad.source = nullptr;
        if (mutation == 2) ++bad.shelf_id;
        if (mutation == 3) ++bad.items[1].batch_id;
        if (mutation == 4) bad.items[1].item_id += 99999;
        if (mutation == 5) bad.items[1].path += L".changed";
        Check(!Apply(receiver, bad).accepted && receiver.tray.FullSnapshotJson() == unchanged, "mismatched identity rejects complete batch without mutation");
    }
    auto duplicate = request; duplicate.items.push_back(duplicate.items[0]);
    Check(!Apply(receiver, duplicate).accepted, "duplicate item IDs rejected");
    Receiver other_receiver; MakeWindow(other_receiver);
    auto forged_source = request; forged_source.source = other_receiver.window;
    Check(!Apply(receiver, forged_source).accepted && receiver.tray.FullSnapshotJson() == unchanged,
          "different live source HWND in same process rejected");
    DestroyWindow(other_receiver.window);
    receiver.tray.SwitchShelf(shelf); receiver.tray.MarkInFlight(991, {request.items[1].item_id}, false);
    const auto inflight = receiver.tray.FullSnapshotJson();
    Check(!Apply(receiver, request).accepted && receiver.tray.FullSnapshotJson() == inflight, "one in-flight item rejects entire batch atomically");
    receiver.tray.CompleteTask(991, {}); receiver.tray.SwitchShelf(active);
    auto result = WorkerSend(snapshot);
    Check(result.accepted && result.removed == 2 && result.skipped_owned == 0 && receiver.tray.active_shelf_id() == active,
          "same-process worker WM_COPYDATA removes exact non-active shelf without switching");
    Check(std::filesystem::exists(file) && std::filesystem::exists(second), "ordinary backing files remain on disk");
    Check(Has(receiver.tray, active, other_item), "same path on active shelf is not consumed by source-shelf request");
    receiver.tray.CollectRichToShelf(shelf, {{.path = file}}, false);
    const auto replacement = Capture(receiver, shelf);
    Check(!WorkerSend(snapshot).accepted && Has(receiver.tray, shelf, replacement.tray_items[0].id), "stale retry cannot delete newly collected same-path item");
    auto bytes = pp::EncodeConsumeTray(Request(replacement));
    COPYDATASTRUCT wire{pp::kConsumeTrayMessage, static_cast<DWORD>(bytes.size() - 1), bytes.data()};
    pp::TrayConsumeRequest decoded;
    Check(!pp::DecodeConsumeTray(&wire, decoded), "truncated wire rejected");
    wire.cbData++; bytes[0] = 2;
    Check(!pp::DecodeConsumeTray(&wire, decoded), "unknown wire version rejected");
    bytes[0] = 1; std::memset(bytes.data() + sizeof(pp::consume_detail::Header) + 16, 1, 2048);
    Check(!pp::DecodeConsumeTray(&wire, decoded), "unterminated path rejected");
    pulse::app::TrayItem snippet; snippet.path = owned; snippet.owned_snippet = true; snippet.content_kind = 2;
    receiver.tray.CollectRichToShelf(shelf, {snippet}, false);
    pulse::app::TrayItem content; content.path = second; content.content_kind = 3;
    receiver.tray.CollectRichToShelf(shelf, {content}, false);
    result = WorkerSend(Capture(receiver, shelf));
    std::vector<std::wstring> references; receiver.tray.AllReferencedSnippetPaths(references);
    Check(result.accepted && result.removed == 1 && result.skipped_owned == 2, "mixed selection removes only file reference and reports retained content");
    Check(std::filesystem::exists(owned) && references.size() == 1 && std::filesystem::equivalent(references[0], owned), "owned backing remains referenced for orphan sweep and exists");
    result = WorkerSend(Capture(receiver, shelf));
    Check(result.accepted && result.removed == 0 && result.skipped_owned == 2, "all-content request explicitly succeeds with zero removal and skipped count");
    const auto cross_shelf = receiver.tray.CreateShelf(L"cross-process");
    receiver.tray.CollectRichToShelf(cross_shelf, {{.path = file}}, false);
    const auto cross = Capture(receiver, cross_shelf); const auto& row = cross.tray_items[0];
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --send " + std::to_wstring(reinterpret_cast<ULONG_PTR>(receiver.window)) +
        L" " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(cross_shelf) + L" " + std::to_wstring(row.id) +
        L" " + std::to_wstring(row.batch_id) + L" \"" + row.path + L"\" done";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    const bool launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
    Check(launched, "separate sender process launched without visible windows");
    if (launched) { PumpUntil(process.hProcess); DWORD status = 99; GetExitCodeProcess(process.hProcess, &status);
        Check(status == 0 && !Has(receiver.tray, cross_shelf, row.id) && std::filesystem::exists(file), "cross-process WM_COPYDATA removes record and preserves backing file");
        CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    const auto bulk_shelf = receiver.tray.CreateShelf(L"bulk");
    std::vector<pulse::app::TrayItem> bulk;
    for (size_t i = 0; i < 1024; ++i) { pulse::app::TrayItem item; item.path = File(root, (L"bulk-" + std::to_wstring(i) + L".txt").c_str()); bulk.push_back(std::move(item)); }
    Check(receiver.tray.CollectRichToShelf(bulk_shelf, bulk, false) == 1024, "1024 real backing files collected");
    const auto bulk_snapshot = Capture(receiver, bulk_shelf);
    auto excessive = Request(bulk_snapshot); excessive.items.push_back({9999999, 9999999, file});
    Check(pp::EncodeConsumeTray(excessive).empty(), "1025-item selection rejected before sending");
    result = WorkerSend(bulk_snapshot);
    Check(result.accepted && result.removed == 1024 && result.skipped_owned == 0 && receiver.tray.FindShelf(bulk_shelf)->batches.empty(), "1024-item WM_COPYDATA batch consumed through real model");
    Check(std::all_of(bulk.begin(), bulk.end(), [](const auto& item) { return std::filesystem::exists(item.path); }), "all 1024 backing files survive");
    Check(receiver.tray.active_shelf_id() == active, "active shelf remains unchanged throughout consumption");
    {
        pp::TrayStageRequest stage;
        stage.publisher_pid = GetCurrentProcessId();
        stage.source = receiver.window;
        stage.paths = {file, second};
        Check(pp::ValidStageTray(stage), "valid stage-to-tray request accepted");
        auto sbytes = pp::EncodeStageTray(stage);
        COPYDATASTRUCT swire{pp::kStageTrayMessage, static_cast<DWORD>(sbytes.size()), sbytes.data()};
        pp::TrayStageRequest sdecoded;
        Check(pp::DecodeStageTray(&swire, sdecoded) && sdecoded.publisher_pid == stage.publisher_pid &&
              sdecoded.source == stage.source && sdecoded.paths == stage.paths, "stage-to-tray wire roundtrips paths");
        swire.cbData -= 1;
        Check(!pp::DecodeStageTray(&swire, sdecoded), "truncated stage-to-tray wire rejected");
        pp::TrayStageRequest empty_stage{GetCurrentProcessId(), receiver.window, 0, {}};
        Check(!pp::ValidStageTray(empty_stage) && pp::EncodeStageTray(empty_stage).empty(), "empty stage-to-tray rejected");
        const auto prepend_shelf = receiver.tray.CreateShelf(L"prepend");
        receiver.tray.CollectRichToShelf(prepend_shelf, {{.path = file}}, false);
        receiver.tray.CollectRichToShelf(prepend_shelf, {{.path = second}}, false, nullptr, true);
        const auto* ps = receiver.tray.FindShelf(prepend_shelf);
        Check(ps && ps->batches.size() == 2 && !ps->batches[0].items.empty() &&
              std::filesystem::equivalent(ps->batches[0].items[0].path, second),
              "prepend puts newly staged item at front of tray shelf");
    }
    DestroyWindow(receiver.window);
    Check(!pp::RequestConsumeTray(bulk_snapshot, bulk_snapshot.tray_items).accepted, "closed publisher window rejected");
    std::printf("failures=%d\n", failures); return failures ? 1 : 0;
}
