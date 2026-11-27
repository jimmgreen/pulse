#include "../common/picker_save_request.h"
#include <cstdio>
#include <limits>

namespace {
using namespace pulse::picker_save;
int failures = 0;
int received = 0;
HWND received_source = nullptr;
bool source_was_live = false, accept_request = true;
Request received_request;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}
bool Same(const Request& left, const Request& right) {
    return left.path == right.path && left.app_name == right.app_name &&
        left.source_pid == right.source_pid && left.request_id == right.request_id &&
        left.notify_if_no_tab == right.notify_if_no_tab && left.copy_path == right.copy_path &&
        left.stage_to_tray == right.stage_to_tray &&
        left.before.exists == right.before.exists && left.before.volume == right.before.volume &&
        left.before.file_id == right.before.file_id && left.before.size == right.before.size &&
        left.before.write_time == right.before.write_time && left.before.attributes == right.before.attributes;
}
bool Read(std::vector<BYTE>& bytes, Request& result) {
    COPYDATASTRUCT data{kMessage, static_cast<DWORD>(bytes.size()), bytes.data()};
    return Decode(&data, result);
}
LRESULT CALLBACK EndpointProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message != WM_COPYDATA) return DefWindowProcW(hwnd, message, wparam, lparam);
    ++received;
    received_source = reinterpret_cast<HWND>(wparam);
    source_was_live = IsWindow(received_source) != FALSE;
    const bool decoded = Decode(reinterpret_cast<COPYDATASTRUCT*>(lparam), received_request);
    return decoded && accept_request ? TRUE : FALSE;
}
}

int main() {
    using namespace pulse::picker_save;
    const HWND source = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!source) { std::puts("[FAIL] message-only source creation"); return 2; }
    Request request;
    request.path = L"C:\\Documents\\保存结果.docx";
    request.app_name = L"Writer 编辑器";
    request.source_pid = GetCurrentProcessId();
    request.request_id = 0x1122334455667788;
    request.before.exists = true;
    request.before.volume = 0x9988776655443322;
    request.before.size = 0x123456789a;
    request.before.write_time = 0x1337799aabbccdd;
    request.before.attributes = FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY;
    for (size_t i = 0; i < request.before.file_id.size(); ++i) request.before.file_id[i] = static_cast<BYTE>(i * 13);
    auto bytes = Encode(request, source);
    Request decoded;
    Check(sizeof(wire::Header) == 96 && bytes.size() == 96 + (request.path.size() + request.app_name.size()) * sizeof(wchar_t),
        "wire header and UTF-16 byte lengths are exact");
    Check(Read(bytes, decoded) && Same(request, decoded), "Unicode roundtrip preserves full before stamp and 64-bit values");
    for (unsigned flags = 0; flags < 8; ++flags) {
        auto candidate = request;
        candidate.notify_if_no_tab = (flags & 1) != 0;
        candidate.copy_path = (flags & 2) != 0;
        candidate.stage_to_tray = (flags & 4) != 0;
        auto encoded = Encode(candidate, source);
        Check(Read(encoded, decoded) && Same(candidate, decoded), "all explicit flag combinations roundtrip");
    }
    auto absent = request;
    absent.before = {};
    absent.app_name.clear();
    auto absent_bytes = Encode(absent, source);
    Check(Read(absent_bytes, decoded) && Same(absent, decoded), "absent before stamp and empty app name roundtrip");
    auto recent_bytes = bytes;
    wire::Header recent_header;
    std::memcpy(&recent_header, recent_bytes.data(), sizeof(recent_header));
    recent_header.issued_at = GetTickCount64() - 9000;
    std::memcpy(recent_bytes.data(), &recent_header, sizeof(recent_header));
    Check(Read(recent_bytes, decoded) && Same(request, decoded), "request within 10-second freshness window is accepted");
    const auto original = bytes;
    auto reject_bytes = [&](const char* label, std::vector<BYTE> malformed) {
        Request unchanged = request;
        unchanged.path = L"sentinel";
        Request output = unchanged;
        Check(!Read(malformed, output) && Same(unchanged, output), label);
    };
    auto reject_header = [&](const char* label, auto mutate) {
        auto malformed = original;
        wire::Header header;
        std::memcpy(&header, malformed.data(), sizeof(header));
        mutate(header);
        std::memcpy(malformed.data(), &header, sizeof(header));
        reject_bytes(label, std::move(malformed));
    };
    COPYDATASTRUCT data{kMessage, static_cast<DWORD>(bytes.size()), bytes.data()};
    Check(!Decode(nullptr, decoded), "null COPYDATA rejected");
    data.dwData ^= 1;
    Check(!Decode(&data, decoded), "unknown COPYDATA message rejected");
    data.dwData = kMessage;
    data.lpData = nullptr;
    Check(!Decode(&data, decoded), "null payload rejected");
    auto short_header = original;
    short_header.resize(sizeof(wire::Header) - 1);
    reject_bytes("truncated header rejected without mutating output", std::move(short_header));
    auto short_body = original;
    short_body.pop_back();
    reject_bytes("truncated UTF-16 body rejected without mutating output", std::move(short_body));
    auto trailing = original;
    trailing.push_back(0);
    reject_bytes("trailing payload byte rejected without mutating output", std::move(trailing));
    reject_header("unknown version rejected", [](auto& h) { h.version = 2; });
    reject_header("unknown flags rejected", [](auto& h) { h.flags = 8; });
    reject_header("nonzero reserved rejected", [](auto& h) { h.reserved = 1; });
    reject_header("invalid exists boolean rejected", [](auto& h) { h.exists = 2; });
    reject_header("zero request ID rejected", [](auto& h) { h.request_id = 0; });
    reject_header("zero source PID rejected", [](auto& h) { h.source_pid = 0; });
    reject_header("mismatched source PID rejected", [](auto& h) { ++h.source_pid; });
    reject_header("missing source HWND rejected", [](auto& h) { h.source_window = 0; });
    reject_header("invalid source HWND rejected", [](auto& h) { h.source_window = UINT64_MAX; });
    reject_header("zero timestamp rejected", [](auto& h) { h.issued_at = 0; });
    reject_header("future timestamp rejected", [](auto& h) { h.issued_at = GetTickCount64() + 10000; });
    reject_header("expired timestamp rejected", [](auto& h) { h.issued_at = GetTickCount64() - 10001; });
    reject_header("empty path length rejected", [](auto& h) { h.path_units = 0; });
    reject_header("32768-unit path length rejected", [](auto& h) { h.path_units = 32768; });
    reject_header("DWORD-overflow path length rejected", [](auto& h) { h.path_units = MAXDWORD; });
    reject_header("257-unit app length rejected", [](auto& h) { h.app_units = 257; });
    reject_header("directory before stamp rejected", [](auto& h) { h.attributes |= FILE_ATTRIBUTE_DIRECTORY; });
    reject_header("reparse before stamp rejected", [](auto& h) { h.attributes |= FILE_ATTRIBUTE_REPARSE_POINT; });
    auto path_nul = original;
    path_nul[sizeof(wire::Header) + 6] = path_nul[sizeof(wire::Header) + 7] = 0;
    reject_bytes("embedded path NUL rejected", std::move(path_nul));
    auto app_nul = original;
    const auto app_offset = sizeof(wire::Header) + request.path.size() * sizeof(wchar_t);
    app_nul[app_offset] = app_nul[app_offset + 1] = 0;
    reject_bytes("embedded app NUL rejected", std::move(app_nul));
    auto invalid_path = original;
    invalid_path[sizeof(wire::Header)] = L'?';
    reject_bytes("invalid absolute path body rejected", std::move(invalid_path));

    auto invalid_encode = [&](const char* label, auto mutate) {
        auto value = request;
        mutate(value);
        Check(Encode(value, source).empty(), label);
    };
    invalid_encode("encode rejects relative path", [](auto& r) { r.path = L"relative.docx"; });
    invalid_encode("encode rejects parent traversal", [](auto& r) { r.path = L"C:\\folder\\..\\file"; });
    invalid_encode("encode rejects incomplete UNC", [](auto& r) { r.path = L"\\\\server"; });
    invalid_encode("encode rejects alternate stream path", [](auto& r) { r.path = L"C:\\file:stream"; });
    invalid_encode("encode rejects path NUL", [](auto& r) { r.path.push_back(0); });
    invalid_encode("encode rejects app NUL", [](auto& r) { r.app_name.push_back(0); });
    invalid_encode("encode rejects oversized path", [](auto& r) { r.path = L"C:\\" + std::wstring(32765, L'a'); });
    invalid_encode("encode rejects oversized app name", [](auto& r) { r.app_name.assign(257, L'a'); });
    invalid_encode("encode rejects mismatched PID", [](auto& r) { ++r.source_pid; });
    invalid_encode("encode rejects zero request ID", [](auto& r) { r.request_id = 0; });
    invalid_encode("encode rejects directory stamp", [](auto& r) { r.before.attributes |= FILE_ATTRIBUTE_DIRECTORY; });
    invalid_encode("encode rejects reparse stamp", [](auto& r) { r.before.attributes |= FILE_ATTRIBUTE_REPARSE_POINT; });
    auto maximum = request;
    maximum.path = L"C:\\" + std::wstring(32764, L'a');
    maximum.app_name.assign(256, L'a');
    auto maximum_bytes = Encode(maximum, source);
    Check(Read(maximum_bytes, decoded) && Same(maximum, decoded), "maximum protocol path and app lengths roundtrip");
    for (const wchar_t* path : {L"\\\\server\\share\\文件.docx", L"\\\\?\\C:\\文件.docx", L"\\\\?\\UNC\\server\\share\\文件.docx"}) {
        auto value = request;
        value.path = path;
        auto encoded = Encode(value, source);
        Check(Read(encoded, decoded) && Same(value, decoded), "UNC or extended absolute path roundtrip without filesystem IO");
    }
    Check(Encode(request, nullptr).empty(), "encode rejects absent sender");
    DestroyWindow(source);
    Check(Encode(request, source).empty() && !Read(bytes, decoded), "destroyed sender invalidates encode and previously encoded payload");

    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulsePickerSaveRequestTest";
    wc.lpfnWndProc = EndpointProc;
    RegisterClassW(&wc);
    HWND endpoint = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    pulse::picker_places::Snapshot snapshot;
    snapshot.endpoint = endpoint;
    snapshot.endpoint_pid = GetCurrentProcessId();
    snapshot.shared_available = true;
    Check(endpoint && Send(snapshot, request) && received == 1 && source_was_live && Same(request, received_request),
        "Send delivers real WM_COPYDATA while temporary source remains live");
    Check(!IsWindow(received_source), "Send destroys temporary source after accepted delivery");
    accept_request = false;
    Check(!Send(snapshot, request) && received == 2 && source_was_live && !IsWindow(received_source),
        "receiver rejection returns false and destroys temporary source");
    auto bad_request = request;
    bad_request.request_id = 0;
    Check(!Send(snapshot, bad_request) && received == 2, "invalid request is not delivered");
    DestroyWindow(endpoint);
    Check(!Send(snapshot, request) && received == 2, "closed endpoint prevents sender delivery");
    std::printf("RESULT: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
