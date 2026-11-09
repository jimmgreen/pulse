// device_source_test.cpp - Phones, D1: pulse:device: paths, the fake device
// source contract (what ShellDeviceSource must also do) and the device
// thread. The fake phones live in a temp folder that is removed afterwards;
// the real shell source is only listed (read-only), never written.
#include "../fs/device_path.h"
#include "../app/this_pc_devices.h"
#include <cstring>
#include "../fs/device_source.h"
#include "../fs/device_worker.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace pulse::fs;

namespace {

bool g_passed = true;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    g_passed = g_passed && ok;
}

void WriteUtf8(const std::wstring& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary);
    out << text;
}

void MakeDir(const std::wstring& dir) { SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); }

void RemoveTree(const std::wstring& dir) {
    std::wstring from = dir;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;  // a temp fixture: no recycle bin
    SHFileOperationW(&op);
}

const DeviceInfo* Find(const std::vector<DeviceInfo>& devices, const std::wstring& name) {
    for (const auto& d : devices)
        if (d.name == name) return &d;
    return nullptr;
}

struct Collected {
    std::vector<size_t> batches;
    std::vector<DirEntry> rows;
};

DeviceError Collect(DeviceSource& source, const DevicePath& folder, Collected& out,
                    std::atomic_bool* cancel_after_first = nullptr) {
    std::atomic_bool cancel{false};
    return source.Enumerate(folder, [&](std::vector<DirEntry>&& batch) {
        out.batches.push_back(batch.size());
        for (auto& row : batch) out.rows.push_back(std::move(row));
        if (cancel_after_first) cancel = true;
        return true;
    }, cancel);
}

void PathTests() {
    const auto parsed = ParseDevicePath(L"pulse:device:0123456789AB\\Internal shared storage\\DCIM\\Camera");
    Check(parsed && parsed->device == L"0123456789ab" && parsed->storage == L"Internal shared storage" &&
              parsed->parts.size() == 2 && parsed->parts[1] == L"Camera",
          "path: device key (case-folded), storage and parts parse");
    Check(parsed && FormatDevicePath(*parsed) == L"pulse:device:0123456789ab\\Internal shared storage\\DCIM\\Camera",
          "path: format round-trips");
    const auto root = ParseDevicePath(L"pulse:device:0123456789ab\\");
    Check(root && root->IsDeviceRoot() && FormatDevicePath(*root) == L"pulse:device:0123456789ab",
          "path: device root, trailing backslash tolerated");
    const auto storage = ParseDevicePath(L"PULSE:DEVICE:0123456789ab\\SD");
    Check(storage && storage->IsStorageRoot() && !storage->IsDeviceRoot(), "path: storage root; prefix case-insensitive");
    bool rejected = true;
    for (const wchar_t* bad : {L"pulse:device:", L"pulse:device:xyz", L"pulse:device:0123456789abc",
                               L"pulse:device:0123456789ab\\\\DCIM", L"pulse:device:0123456789ab\\a\\..\\b",
                               L"pulse:device:0123456789ab\\.", L"pulse:device:0123456789ab\\a/b",
                               L"pulse:recycle", L"C:\\Users"})
        rejected = rejected && !ParseDevicePath(bad);
    Check(rejected, "path: bad keys, empty / dot segments, slashes and other paths are rejected");
    Check(IsDevicePath(L"pulse:device:0123456789ab") && !IsDevicePath(L"pulse:devices") && !IsDevicePath(L"C:\\"),
          "path: IsDevicePath");
    const std::wstring key = DeviceKeyFor(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\\\?\\usb#vid_2717&pid_ff40#abc");
    Check(IsValidDeviceKey(key) && key == DeviceKeyFor(L"::{20d04fe0-3aea-1069-a2d8-08002b30309d}\\\\?\\USB#VID_2717&PID_FF40#ABC") &&
              key != DeviceKeyFor(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\\\?\\usb#vid_2717&pid_ff40#abd"),
          "path: device key is 12 hex digits, stable across case, differs per device");
    DevicePath child = DeviceChild(*root, L"Internal");
    child = DeviceChild(child, L"DCIM");
    Check(child.storage == L"Internal" && child.parts.size() == 1, "path: DeviceChild adds a storage, then folders");
}

void FakeSourceTests(const std::wstring& root) {
    // Xiaomi: phone storage + FAT32 card, 450 photos; Pixel: one storage;
    // Locked: waits for 允许; iPhone: read-only; Flaky: disconnects at 250.
    const std::wstring xiaomi = root + L"\\xiaomi";
    MakeDir(xiaomi + L"\\Internal shared storage\\DCIM\\Camera");
    MakeDir(xiaomi + L"\\SD card\\Music");
    WriteUtf8(xiaomi + L"\\device.ini", "\xEF\xBB\xBFname=Xiaomi 14\nkind=android\n");
    WriteUtf8(xiaomi + L"\\Internal shared storage\\storage.ini", "total=256000000000\nfree=100000000000\n");
    WriteUtf8(xiaomi + L"\\SD card\\storage.ini", "total=64000000000\nfree=1000\nfs=FAT32\nremovable=1\n");
    for (int i = 0; i < 450; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"\\IMG_%04d.jpg", i);
        WriteUtf8(xiaomi + L"\\Internal shared storage\\DCIM\\Camera" + name, "x");
    }
    MakeDir(root + L"\\pixel\\Internal storage\\Download");
    WriteUtf8(root + L"\\pixel\\device.ini", "name=Pixel 8\n");
    MakeDir(root + L"\\locked\\Internal storage");
    WriteUtf8(root + L"\\locked\\device.ini", "name=Locked Phone\nlocked=1\n");
    MakeDir(root + L"\\iphone\\Internal Storage\\DCIM\\100APPLE");
    WriteUtf8(root + L"\\iphone\\device.ini", "name=Apple iPhone\nkind=iphone\n");
    WriteUtf8(root + L"\\iphone\\Internal Storage\\DCIM\\100APPLE\\IMG_0001.HEIC", "h");
    MakeDir(root + L"\\flaky\\Internal storage\\big");
    WriteUtf8(root + L"\\flaky\\device.ini", "name=Flaky\ndisconnect_after=250\n");
    for (int i = 0; i < 400; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"\\f%04d.txt", i);
        WriteUtf8(root + L"\\flaky\\Internal storage\\big" + name, "y");
    }

    auto source = MakeFakeDeviceSource(root);
    const auto devices = source->ListDevices();
    const DeviceInfo* mi = Find(devices, L"Xiaomi 14");
    const DeviceInfo* pixel = Find(devices, L"Pixel 8");
    const DeviceInfo* locked = Find(devices, L"Locked Phone");
    const DeviceInfo* iphone = Find(devices, L"Apple iPhone");
    Check(devices.size() == 5 && mi && pixel && locked && iphone, "fake: every device folder is a device (names from device.ini)");
    Check(mi && mi->storages.size() == 2 && mi->storages[0].name == L"Internal shared storage" &&
              mi->storages[0].total == 256000000000ull && !mi->storages[0].removable &&
              mi->storages[1].file_system == L"FAT32" && mi->storages[1].removable && !mi->locked,
          "fake: storages with capacity, FAT32 SD card marked removable");
    Check(locked && locked->locked && locked->storages.empty(), "fake: a locked phone shows no storage");
    Check(iphone && iphone->kind == DeviceKind::IPhone && iphone->storages.size() == 1 && iphone->storages[0].read_only,
          "fake: iPhone storage is read-only");
    Check(pixel && SkipsStorageLevel(*pixel) && mi && !SkipsStorageLevel(*mi) &&
              DeviceOpenTarget(*pixel) == L"pulse:device:" + pixel->key + L"\\Internal storage" &&
              DeviceOpenTarget(*mi) == L"pulse:device:" + mi->key && DeviceOpenTarget(*locked) == L"pulse:device:" + locked->key,
          "fake: single-storage phones open straight into the storage");
    Check(mi && mi->key == DeviceKeyFor(L"fake:xiaomi") && source->ListDevices() == devices,
          "fake: keys are stable across listings");
    if (!mi || !locked || !iphone || !pixel) return;

    DevicePath camera{mi->key, L"Internal shared storage", {L"DCIM", L"Camera"}};
    Collected photos;
    const DeviceError photos_result = Collect(*source, camera, photos);
    Check(photos_result == DeviceError::None && photos.batches == std::vector<size_t>({200, 200, 50}) &&
              photos.rows.size() == 450,
          "enumerate: 450 rows arrive in batches of 200");
    Check(!photos.rows.empty() && photos.rows[0].name == L"IMG_0000.jpg" && !photos.rows[0].is_dir &&
              photos.rows[0].size == 1 && photos.rows[0].full_path == FormatDevicePath(DeviceChild(camera, L"IMG_0000.jpg")) &&
              photos.rows[0].mtime.dwHighDateTime != 0,
          "enumerate: rows carry name, size, time and their pulse:device: path");
    Collected storage_root;
    Check(Collect(*source, DevicePath{mi->key, L"SD card", {}}, storage_root) == DeviceError::None &&
              storage_root.rows.size() == 1 && storage_root.rows[0].name == L"Music" && storage_root.rows[0].is_dir,
          "enumerate: a storage root lists its folders (fixture files hidden)");
    Collected cancelled;
    std::atomic_bool stop_after_first{false};
    Check(Collect(*source, camera, cancelled, &stop_after_first) == DeviceError::Cancelled && cancelled.batches.size() == 1,
          "enumerate: cancel stops after the current batch");
    Collected stopped;
    std::atomic_bool never{false};
    size_t stopped_batches = 0;
    const DeviceError stop_result = source->Enumerate(camera, [&](std::vector<DirEntry>&&) {
        ++stopped_batches;
        return false;
    }, never);
    Check(stop_result == DeviceError::None && stopped_batches == 1, "enumerate: the sink can stop early");
    const DeviceInfo* flaky = Find(devices, L"Flaky");
    Collected partial;
    Check(flaky && Collect(*source, DevicePath{flaky->key, L"Internal storage", {L"big"}}, partial) ==
                       DeviceError::Disconnected && partial.rows.size() == 200,
          "enumerate: a phone unplugged mid-way reports Disconnected after the delivered rows");
    Collected none;
    Check(Collect(*source, DevicePath{mi->key, L"Internal shared storage", {L"Nope"}}, none) == DeviceError::NotFound &&
              Collect(*source, DevicePath{L"0123456789ab", L"x", {}}, none) == DeviceError::Disconnected &&
              Collect(*source, DevicePath{locked->key, L"Internal storage", {}}, none) == DeviceError::Locked &&
              Collect(*source, DevicePath{mi->key, L"", {}}, none) == DeviceError::NotFound,
          "enumerate: missing folder, unknown device, locked phone, device root are errors");

    const DeviceItemCaps photo_caps = source->Caps(DeviceChild(camera, L"IMG_0001.jpg"));
    const DeviceItemCaps folder_caps = source->Caps(camera);
    const DeviceItemCaps storage_caps = source->Caps(DevicePath{mi->key, L"Internal shared storage", {}});
    const DeviceItemCaps apple_caps = source->Caps(DevicePath{iphone->key, L"Internal Storage", {L"DCIM", L"100APPLE", L"IMG_0001.HEIC"}});
    Check(photo_caps.can_copy && photo_caps.can_delete && photo_caps.can_rename && !photo_caps.can_create,
          "caps: an Android file can be copied, deleted, renamed");
    Check(folder_caps.can_create && !storage_caps.can_delete && !storage_caps.can_rename && storage_caps.can_create,
          "caps: folders take new items; a storage root cannot be deleted or renamed");
    Check(apple_caps.can_copy && !apple_caps.can_delete && !apple_caps.can_rename && !apple_caps.can_create,
          "caps: iPhone items are copy-only");

    IShellItem* item = nullptr;
    const HRESULT hr = source->ShellItem(DeviceChild(camera, L"IMG_0002.jpg"), &item);
    bool named = false;
    if (SUCCEEDED(hr) && item) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
            named = std::wstring(raw).ends_with(L"IMG_0002.jpg");
            CoTaskMemFree(raw);
        }
        item->Release();
    }
    IShellItem* gone = nullptr;
    Check(named && FAILED(source->ShellItem(DevicePath{L"0123456789ab", L"x", {}}, &gone)) && !gone,
          "shell item: resolves fake rows, fails for unknown devices");

    // Unplugging = the device folder disappears.
    RemoveTree(root + L"\\pixel");
    Collected after;
    Check(Collect(*source, DevicePath{pixel->key, L"Internal storage", {}}, after) == DeviceError::Disconnected &&
              source->ListDevices().size() == 4,
          "fake: an unplugged phone is gone from the list and reports Disconnected");
}

DWORD Apartment() {
    APTTYPE type{};
    APTTYPEQUALIFIER qualifier{};
    return SUCCEEDED(CoGetApartmentType(&type, &qualifier)) ? static_cast<DWORD>(type) : 0xFFFFFFFF;
}

void WorkerTests() {
    {
        DeviceWorker worker(200);
        Check(!worker.HasThread() && worker.ThreadsStarted() == 0, "worker: no thread before the first post");
        std::atomic<int> order{0};
        std::atomic<DWORD> apartment{0xFFFFFFFF};
        std::atomic<bool> in_order{true};
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (int i = 1; i <= 5; ++i)
            worker.Post([&, i] {
                if (i == 1) apartment = Apartment();
                if (order.fetch_add(1) + 1 != i) in_order = false;
                if (i == 5) SetEvent(done);
            });
        const bool ran = WaitForSingleObject(done, 3000) == WAIT_OBJECT_0;
        Check(ran && in_order && order == 5, "worker: tasks run in posting order");
        Check(apartment == static_cast<DWORD>(APTTYPE_STA) || apartment == static_cast<DWORD>(APTTYPE_MAINSTA),
              "worker: tasks run in a single-threaded apartment (shell / MTP)");
        Check(worker.ThreadsStarted() == 1, "worker: one thread serves the burst");
        Sleep(700);
        Check(!worker.HasThread(), "worker: the thread exits after the idle time");
        ResetEvent(done);
        worker.Post([&] { SetEvent(done); });
        Check(WaitForSingleObject(done, 3000) == WAIT_OBJECT_0 && worker.ThreadsStarted() == 2,
              "worker: the next post starts a fresh thread");
        CloseHandle(done);
    }
    {
        DeviceWorker worker(5000);
        std::atomic<bool> stuck_reported{false};
        std::atomic<bool> stuck_finished{false};
        std::atomic<DWORD> stuck_thread{0};
        std::atomic<DWORD> next_thread{0};
        HANDLE next_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto started = std::chrono::steady_clock::now();
        worker.Post([&] {
            stuck_thread = GetCurrentThreadId();
            Sleep(1500);  // a driver call that does not return in time
            stuck_finished = true;
        }, 300, [&] { stuck_reported = true; });
        worker.Post([&] {
            next_thread = GetCurrentThreadId();
            SetEvent(next_done);
        });
        const bool next_ran = WaitForSingleObject(next_done, 3000) == WAIT_OBJECT_0;
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        Check(next_ran && stuck_reported && !stuck_finished && waited.count() < 1200,
              "worker: an overrunning task is abandoned; the queue moves on without waiting for it");
        Check(next_thread != 0 && next_thread != stuck_thread && worker.ThreadsAbandoned() == 1 && worker.ThreadsStarted() == 2,
              "worker: the queued task runs on a fresh thread");
        HANDLE fast_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<bool> fast_flagged{false};
        worker.Post([&] { SetEvent(fast_done); }, 2000, [&] { fast_flagged = true; });
        WaitForSingleObject(fast_done, 3000);
        Sleep(2300);
        Check(!fast_flagged && worker.ThreadsAbandoned() == 1, "worker: tasks finishing within budget are never abandoned");
        Sleep(400);
        Check(stuck_finished, "worker: the abandoned call still finishes on its own thread");
        CloseHandle(next_done);
        CloseHandle(fast_done);
        worker.Stop(1000);
        Check(!worker.HasThread(), "worker: Stop ends the live thread");
    }
}

void ShellSourceSmoke() {
    // Read-only: lists This PC's portable devices (usually none here).
    std::vector<DeviceInfo> devices;
    bool listed = false;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto shell = MakeShellDeviceSource();
        devices = shell->ListDevices();
        Collected none;
        std::atomic_bool cancel{false};
        const DeviceError gone = shell->Enumerate(DevicePath{L"0123456789ab", L"x", {}},
            [](std::vector<DirEntry>&&) { return true; }, cancel);
        listed = gone == DeviceError::Disconnected;
        CoUninitialize();
    }).join();
    bool consistent = true;
    for (const auto& d : devices) consistent = consistent && d.key == DeviceKeyFor(d.id) && d.locked == d.storages.empty();
    Check(listed && consistent, "shell source: lists This PC devices; unknown keys are Disconnected");
    std::printf("  shell devices here: %zu, portable interface present: %d\n", devices.size(),
                AnyPortableDevicePresent() ? 1 : 0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--this-pc") == 0) {
        DeviceInfo phone;
        phone.key = L"0123456789ab";
        phone.name = L"Test phone";
        DeviceInfo second = phone;
        second.key = L"abcdef012345";
        second.locked = true;
        const std::vector<std::vector<DeviceInfo>> snapshots = {{}, {phone}, {phone, second}, {phone}, {}};
        const char* labels[] = {"no device: no group rows", "connected: one group row",
            "two devices including locked: two group rows", "one removed: remaining row", "last removed: no group rows"};
        for (size_t i = 0; i < snapshots.size(); ++i) {
            std::vector<DirEntry> entries(1);
            entries.front().name = L"Drive";
            pulse::app::AppendThisPcDevices(entries, snapshots[i]);
            size_t device_rows = 0;
            for (const auto& entry : entries) if (entry.pc_device) ++device_rows;
            Check(device_rows == snapshots[i].size() && entries.size() == snapshots[i].size() + 1
                && entries.front().name == L"Drive", labels[i]);
        }
        return g_passed ? 0 : 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pulse_device_test_" + std::to_wstring(GetCurrentProcessId());
    MakeDir(root);
    PathTests();
    FakeSourceTests(root);
    WorkerTests();
    ShellSourceSmoke();
    RemoveTree(root);
    Check(GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES, "fixture folder removed");
    CoUninitialize();
    return g_passed ? 0 : 1;
}
