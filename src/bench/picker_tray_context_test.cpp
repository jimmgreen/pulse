#include "../picker_inprocess/picker_jump_context.h"
#include "../ui/folder_picker_dialog.h"
#include "../common/localization.h"
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdio>

int main() {
    using namespace pulse;
    int checks = 0, failures = 0;
    auto check = [&](bool okay, const char* text) { ++checks; failures += !okay; std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", text); };
    struct Fixture { std::mutex mutex; picker_places::Snapshot snapshot; };
    auto fixture = std::make_shared<Fixture>();
    fixture->snapshot.tray_available = true; fixture->snapshot.tray_shelf_id = 42;
    fixture->snapshot.tray_revision = 9; fixture->snapshot.tray_total_count = 1;
    fixture->snapshot.tray_shelf_name = L"临时架"; fixture->snapshot.tray_publisher_pid = GetCurrentProcessId();
    picker_places::TrayItem item;
    item.id = 1; item.batch_id = 2; item.shelf_id = 42; item.path = L"C:\\fixture\\image.png";
    item.label = L"截图"; item.size = 123; item.exists = true; item.owned_snippet = true; item.content_kind = 1;
    fixture->snapshot.tray_items.push_back(item);
    picker_inprocess::JumpContextOptions options; options.enumerate_explorer = false;
    options.read_places = [fixture] { std::lock_guard lock(fixture->mutex); return fixture->snapshot; };
    picker_inprocess::JumpContext context(GetCurrentProcessId(), static_cast<int>(ui::PickerMode::File), options);
    ui::FolderPickerSpec spec;
    auto wait = [&](auto expected) {
        for (int i = 0; i < 150; ++i) { context.Poll(spec); if (expected()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        return false;
    };
    check(wait([&] { return spec.pulse_tray_available && spec.pulse_tray_items.size() == 1; }), "worker transports initial tray into FolderPickerSpec");
    check(spec.pulse_tray_shelf_id == 42 && spec.pulse_tray_revision == 9 && spec.pulse_tray_total_count == 1 &&
        spec.pulse_tray_shelf_name == L"临时架" && spec.pulse_tray_publisher_pid == GetCurrentProcessId() &&
        spec.pulse_tray_items.size() == 1 && spec.pulse_tray_items[0] == item,
        "spec retains complete item shelf revision and publisher consumption identity");
    {
        std::lock_guard lock(fixture->mutex);
        fixture->snapshot.tray_items[0].inflight = true; fixture->snapshot.tray_items[0].exists = false;
        ++fixture->snapshot.tray_revision;
    }
    check(wait([&] { return spec.pulse_tray_revision == 10 && spec.pulse_tray_items[0].inflight && !spec.pulse_tray_items[0].exists; }),
        "worker updates in-flight and existence observations without opening backing file");
    {
        std::lock_guard lock(fixture->mutex);
        fixture->snapshot.tray_shelf_id = 77; fixture->snapshot.tray_shelf_name = L"新架";
        fixture->snapshot.tray_items.clear(); fixture->snapshot.tray_total_count = 0; ++fixture->snapshot.tray_revision;
    }
    check(wait([&] { return spec.pulse_tray_available && spec.pulse_tray_items.empty() && spec.pulse_tray_shelf_id == 77; }),
        "empty shelf switch clears old items in spec");
    {
        std::lock_guard lock(fixture->mutex);
        fixture->snapshot.tray_available = false; fixture->snapshot.tray_total_count = 65; ++fixture->snapshot.tray_revision;
    }
    check(wait([&] { return !spec.pulse_tray_available && spec.pulse_tray_items.empty() && spec.pulse_tray_total_count == 65; }),
        "overflow remains distinguishable from empty shelf in spec");
    {
        std::lock_guard lock(fixture->mutex);fixture->snapshot.tray_available=true;fixture->snapshot.tray_total_count=129;
        fixture->snapshot.tray_items.clear();
        for(uint64_t i=1;i<=129;++i){auto row=item;row.id=i;row.shelf_id=77;row.path=L"C:\\fixture\\file"+std::to_wstring(i)+L".txt";fixture->snapshot.tray_items.push_back(std::move(row));}
        ++fixture->snapshot.tray_revision;
    }
    check(wait([&]{return spec.pulse_tray_available&&spec.pulse_tray_items.size()==129&&spec.pulse_tray_total_count==129&&spec.pulse_tray_items.back().id==129;}),
        "worker to Spec preserves every item across three transport pages");
    {
        std::lock_guard lock(fixture->mutex); fixture->snapshot = {};
    }
    check(wait([&] { return !spec.pulse_tray_available && spec.pulse_tray_items.empty() && spec.pulse_tray_revision == 0 &&
        spec.pulse_tray_shelf_id == 0 && spec.pulse_tray_shelf_name.empty() && spec.pulse_tray_publisher_pid == 0; }),
        "dead or expired source clears shelf and publisher identity as well as items");
    {
        std::lock_guard lock(fixture->mutex);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) context.Poll(spec);
        check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
            "Poll never waits on or invokes blocking source callback");
    }
    std::printf("checks=%d failures=%d\n", checks, failures); return failures ? 1 : 0;
}
