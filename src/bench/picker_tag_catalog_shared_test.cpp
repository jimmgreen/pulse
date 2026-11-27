#include "../common/picker_tag_catalog_shared.h"
#include <cstdio>
#include <thread>

namespace {
using namespace pulse::picker_tags;
int failures = 0;
void Check(bool passed, const char* name) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}
Snapshot Sample() {
    Snapshot value;
    value.available = true;
    value.revision = 42;
    value.tags = {{L"工作", 0x123456}, {L"Second", 0xffffff}};
    value.paths = {{L"C:\\目录\\a.txt", 0}, {L"\\\\server\\share\\b", 1}};
    return value;
}
DWORD Child(const std::wstring& arguments) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 999;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return 998;
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD code = 997;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--read") return Read(argv[2]) == Sample() ? 0 : 1;
    if (argc == 3 && std::wstring(argv[1]) == L"--publish-exit") {
        detail::Writer writer(argv[2]);
        Publish(Sample(), writer);
        return Read(argv[2]) == Sample() ? 0 : 1;
    }
    const std::wstring name = L"Local\\Pulse.TagCatalog.Test." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64());
    detail::Writer writer(name);
    Check(writer.blob && writer.mutex, "isolated mapping created");
    if (!writer.blob || !writer.mutex) return 1;
    Check(!Read(name).available, "unpublished unavailable");
    const auto sample = Sample();
    Publish(sample, writer);
    Check(Read(name) == sample, "complete Unicode UNC catalog round trip");
    Check(Child(L"--read \"" + name + L"\"") == 0, "live cross-process read");
    const auto encodes = writer.encode_count;
    const auto timestamp = writer.blob->header.timestamp;
    Sleep(2);
    Publish(sample, writer);
    Check(writer.encode_count == encodes && writer.blob->header.timestamp > timestamp,
        "identical publish only refreshes timestamp without encoding");
    auto changed = sample;
    changed.tags[0].name = L"Changed";
    Publish(changed, writer);
    Check(Read(name) == changed && writer.encode_count == encodes + 1, "content change with same revision reencodes");
    ++changed.revision;
    Publish(changed, writer);
    Check(Read(name) == changed && writer.encode_count == encodes + 2, "revision change reencodes");
    Publish(sample, writer);
    const auto header = writer.blob->header;
    auto reject_header = [&](const char* label, auto change) {
        writer.blob->header = header;
        change(writer.blob->header);
        Check(!Read(name).available, label);
        writer.blob->header = header;
    };
    reject_header("bad version unavailable", [](auto& h) { h.version = 2; });
    reject_header("bad magic unavailable", [](auto& h) { h.magic = 0; });
    reject_header("unknown flag unavailable", [](auto& h) { h.available = 2; });
    reject_header("reserved header unavailable", [](auto& h) { h.reserved = 1; });
    reject_header("future timestamp unavailable", [](auto& h) { h.timestamp = GetTickCount64() + 100000; });
    reject_header("expired timestamp unavailable", [](auto& h) { h.timestamp = GetTickCount64() - 10001; });
    reject_header("oversize length unavailable", [](auto& h) { h.bytes = static_cast<DWORD>(detail::kMaxPayloadBytes + 1); });
    reject_header("truncated payload unavailable", [](auto& h) { --h.bytes; });
    reject_header("trailing payload unavailable", [](auto& h) { ++h.bytes; });
    reject_header("excess tag count unavailable", [](auto& h) { h.tags = MAXDWORD; });
    reject_header("excess path count unavailable", [](auto& h) { h.paths = MAXDWORD; });
    reject_header("missing pid unavailable", [](auto& h) { h.pid = 0; });
    InterlockedIncrement(&writer.blob->sequence);
    Check(!Read(name).available, "odd sequence unavailable");
    InterlockedIncrement(&writer.blob->sequence);
    writer.blob->reserved = 1;
    Check(!Read(name).available, "reserved blob unavailable");
    writer.blob->reserved = 0;
    auto reject_bytes = [&](const char* label, size_t offset, DWORD value) {
        DWORD original = 0;
        std::memcpy(&original, writer.blob->payload + offset, 4);
        std::memcpy(writer.blob->payload + offset, &value, 4);
        Check(!Read(name).available, label);
        std::memcpy(writer.blob->payload + offset, &original, 4);
    };
    reject_bytes("zero string length unavailable", 0, 0);
    reject_bytes("oversize string length unavailable", 0, MAXDWORD);
    reject_bytes("NUL tag unavailable", 8, 0);
    reject_bytes("invalid color unavailable", 4, 0x1000000);
    const size_t first_path = 16 + (sample.tags[0].name.size() + sample.tags[1].name.size()) * sizeof(wchar_t);
    reject_bytes("invalid rank unavailable", first_path + 4, 2);
    reject_bytes("negative rank unavailable", first_path + 4, MAXDWORD);
    reject_bytes("NUL path unavailable", first_path + 8, 0);
    HANDLE acquired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread holder([&] {
        WaitForSingleObject(writer.mutex, INFINITE);
        SetEvent(acquired);
        WaitForSingleObject(release, INFINITE);
        ReleaseMutex(writer.mutex);
    });
    WaitForSingleObject(acquired, INFINITE);
    const auto start = GetTickCount64();
    Check(!Read(name).available, "busy reader returns unavailable");
    Publish(changed, writer);
    Check(GetTickCount64() - start < 1000 && writer.blob->header.revision == sample.revision,
        "busy writer skips without waiting");
    SetEvent(release);
    holder.join();
    CloseHandle(acquired);
    CloseHandle(release);
    Snapshot invalid = sample;
    invalid.tags[0].rgb = 0x1000000;
    Publish(invalid, writer);
    Check(!Read(name).available && !writer.blob->header.bytes, "invalid source publishes empty unavailable");
    invalid = sample;
    invalid.paths[0].second = -1;
    Publish(invalid, writer);
    Check(!Read(name).available, "invalid source rank unavailable");
    invalid = sample;
    invalid.paths[0].first = std::wstring(4, L'\0');
    Publish(invalid, writer);
    Check(!Read(name).available, "invalid source NUL unavailable");
    Snapshot capacity;
    capacity.available = true;
    capacity.tags = {{std::wstring((detail::kMaxPayloadBytes - 8) / sizeof(wchar_t), L'x'), 1}};
    Publish(capacity, writer);
    Check(Read(name) == capacity && writer.blob->header.bytes == detail::kMaxPayloadBytes, "exact payload capacity accepted");
    capacity.tags[0].name.push_back(L'x');
    Publish(capacity, writer);
    Check(!Read(name).available && !writer.blob->header.bytes && !writer.blob->header.tags,
        "over capacity invalidates full catalog without truncation");
    Snapshot empty;
    empty.available = true;
    empty.revision = 100;
    Publish(empty, writer);
    Check(Read(name) == empty, "available empty catalog preserved");
    Publish(Snapshot{}, writer);
    Check(!Read(name).available, "explicit unavailable replaces catalog");
    Publish(sample, writer);
    Check(Child(L"--publish-exit \"" + name + L"\"") == 0 && !Read(name).available,
        "exited publisher unavailable while mapping stays alive");
    Publish(sample, writer);
    Check(Read(name) == sample, "cached publisher restores after another publisher exits");
    const std::wstring truncated_name = name + L".Truncated";
    HANDLE short_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, truncated_name.c_str());
    HANDLE short_mutex = CreateMutexW(nullptr, FALSE, (truncated_name + L".Lock").c_str());
    Check(short_mapping && short_mutex && !Read(truncated_name).available, "truncated mapping unavailable");
    if (short_mapping) CloseHandle(short_mapping);
    if (short_mutex) CloseHandle(short_mutex);
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
