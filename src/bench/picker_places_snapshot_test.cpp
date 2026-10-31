#include "../common/picker_places_snapshot.h"
#include <iostream>
#include <memory>

int main() {
    using namespace pulse::picker_places;
    int failed = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!ok) ++failed;
    };
    auto valid = std::make_unique<detail::Payload>();
    valid->magic = detail::kMagic;
    valid->version = 1;
    valid->pid = GetCurrentProcessId();
    valid->timestamp = GetTickCount64();
    const std::wstring current = L"C:\\资料\\项目";
    valid->current_length = static_cast<DWORD>(current.size());
    std::memcpy(valid->current, current.data(), current.size() * sizeof(wchar_t));
    valid->count = 1;
    auto& row = valid->rows[0];
    row.path_length = static_cast<DWORD>(current.size());
    std::memcpy(row.path, current.data(), current.size() * sizeof(wchar_t));
    row.label_length = 2;
    row.label[0] = L'资'; row.label[1] = L'料'; row.flags = 3;
    check(detail::Valid(*valid), "absolute Unicode path and both flags");
    auto changed = std::make_unique<detail::Payload>();
    auto rejects = [&](const char* name, auto mutate) {
        *changed = *valid;
        mutate(*changed);
        check(!detail::Valid(*changed), name);
    };
    rejects("wrong magic", [](auto& p) { p.magic = 0; });
    rejects("unknown version", [](auto& p) { p.version = 2; });
    rejects("zero process", [](auto& p) { p.pid = 0; });
    rejects("future timestamp", [](auto& p) { p.timestamp = GetTickCount64() + 60000; });
    if (GetTickCount64() >= 10001)
        rejects("expired snapshot", [](auto& p) { p.timestamp = GetTickCount64() - 10001; });
    rejects("excess rows", [](auto& p) { p.count = 97; });
    rejects("excess current length", [](auto& p) { p.current_length = 1025; });
    rejects("empty row path", [](auto& p) { p.rows[0].path_length = 0; });
    rejects("excess path length", [](auto& p) { p.rows[0].path_length = 1025; });
    rejects("excess label length", [](auto& p) { p.rows[0].label_length = 129; });
    rejects("unknown flags", [](auto& p) { p.rows[0].flags = 4; });
    rejects("embedded current NUL", [](auto& p) { p.current[1] = 0; });
    rejects("embedded path NUL", [](auto& p) { p.rows[0].path[1] = 0; });
    rejects("embedded label NUL", [](auto& p) { p.rows[0].label[1] = 0; });
    *changed = *valid;
    changed->current_length = 1024;
    std::wmemset(changed->current, L'文', 1024);
    changed->count = 96;
    for (auto& r : changed->rows) {
        r.path_length = 1024; r.label_length = 128; r.flags = 0;
        std::wmemset(r.path, L'文', 1024);
        r.path[0] = L'C'; r.path[1] = L':'; r.path[2] = L'\\';
        std::wmemset(r.label, L'文', 128);
    }
    check(detail::Valid(*changed), "maximum rows and Unicode string lengths");
    *changed = *valid;
    changed->count = 0; changed->current_length = 0;
    check(detail::Valid(*changed), "empty snapshot");
    const auto isolated_name = L"Local\\Pulse.PickerPlaces.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    detail::Writer writer(isolated_name);
    check(writer.blob != nullptr && writer.mutex != nullptr, "isolated mapping creation");
    if (writer.blob) {
        valid->timestamp = GetTickCount64();
        writer.blob->payload = *valid;
        InterlockedExchange(&writer.blob->sequence, 2);
        const auto roundtrip = detail::ReadMapping(isolated_name);
        check(roundtrip.current == current && roundtrip.folders.size() == 1 &&
              roundtrip.folders[0].path == current && roundtrip.folders[0].label == L"资料" &&
              roundtrip.folders[0].open && roundtrip.folders[0].pinned, "isolated mapping Unicode and flags roundtrip");
        InterlockedExchange(&writer.blob->sequence, 3);
        check(detail::ReadMapping(isolated_name).folders.empty(), "odd sequence returns without waiting");
        writer.blob->payload.rows[0].path_length = 1025;
        InterlockedExchange(&writer.blob->sequence, 4);
        check(detail::ReadMapping(isolated_name).folders.empty(), "mapping rejects invalid length");
        writer.blob->payload = *valid;
        writer.blob->payload.timestamp = GetTickCount64() >= 10001 ? GetTickCount64() - 10001 : GetTickCount64() + 60000;
        InterlockedExchange(&writer.blob->sequence, 6);
        check(detail::ReadMapping(isolated_name).folders.empty(), "mapping rejects stale or future timestamp");
    }
    return failed ? 1 : 0;
}

