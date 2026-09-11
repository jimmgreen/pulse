#include "../ipc/preview_protocol.h"
#include "../preview_host/video_codec.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace pulse;

namespace {
int passed = 0, failed = 0;
void Check(bool value, const wchar_t* name) {
    std::wprintf(L"[%s] %s\n", value ? L"PASS" : L"FAIL", name);
    value ? ++passed : ++failed;
}

bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& bytes,
                uint64_t final_size = 0) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = bytes.empty() ||
        (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
         written == bytes.size());
    if (ok && final_size > bytes.size()) {
        DWORD ignored = 0;
        DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(final_size);
        ok = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    }
    CloseHandle(file);
    return ok;
}

bool WriteBmpRgb(const std::wstring& path, uint32_t width, uint32_t height) {
    const uint32_t row = (width * 3u + 3u) & ~3u;
    const uint32_t pixels = row * height;
    std::vector<unsigned char> file(54 + pixels, 0);
    file[0] = 'B';
    file[1] = 'M';
    const uint32_t total = 54 + pixels;
    std::memcpy(&file[2], &total, 4);
    file[10] = 54;
    file[14] = 40;
    std::memcpy(&file[18], &width, 4);
    std::memcpy(&file[22], &height, 4);
    file[26] = 1;
    file[28] = 24;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            file[54 + y * row + x * 3 + 0] = static_cast<unsigned char>(x * 255u / (std::max)(1u, width));
            file[54 + y * row + x * 3 + 1] = static_cast<unsigned char>(y * 255u / (std::max)(1u, height));
            file[54 + y * row + x * 3 + 2] = 128;
        }
    }
    return WriteBytes(path, file);
}

struct Result {
    ipc::PreviewResponse response{};
    std::wstring text;
    std::wstring error;
    std::vector<std::pair<std::wstring, std::wstring>> properties;
};

class Host {
public:
    bool Start() {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (!slash) return false;
        *(slash + 1) = 0;
        const DWORD owner = GetCurrentProcessId();
        std::wstring command = L"\"" + std::wstring(exe) + L"Pulse.Preview.exe\" " +
                               std::to_wstring(owner);
        STARTUPINFOW si{sizeof(si)};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &process_)) return false;
        const std::wstring pipeName = ipc::PreviewPipeName(owner);
        const ULONGLONG deadline = GetTickCount64() + 3000;
        do {
            pipe_ = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) return true;
            Sleep(10);
        } while (GetTickCount64() < deadline);
        Stop();
        return false;
    }

    void Stop() {
        if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
        if (process_.hProcess) {
            if (WaitForSingleObject(process_.hProcess, 500) == WAIT_TIMEOUT)
                TerminateProcess(process_.hProcess, 0);
            CloseHandle(process_.hProcess);
            CloseHandle(process_.hThread);
            process_ = {};
        }
    }
    ~Host() { Stop(); }

    bool Request(const std::wstring& path, Result& result, DWORD attrs_override = MAXDWORD,
                 uint32_t pixel_size = ipc::kPreviewDefaultPixelSize,
                 ipc::PreviewRequestKind kind = ipc::PreviewRequestKind::Content) {
        ipc::PreviewRequest request{};
        request.request_id = next_++;
        request.generation = request.request_id;
        request.kind = kind;
        request.pixel_size = pixel_size;
        request.attrs = attrs_override == MAXDWORD ? GetFileAttributesW(path.c_str())
                                                   : attrs_override;
        request.path_chars = static_cast<uint32_t>(path.size());
        if (!ipc::WriteAll(pipe_, &request, sizeof(request)) ||
            !ipc::WriteAll(pipe_, path.data(), request.path_chars * sizeof(wchar_t)) ||
            !ipc::ReadAll(pipe_, &result.response, sizeof(result.response))) return false;
        if (result.response.magic != ipc::kPreviewMagic ||
            result.response.request_id != request.request_id) return false;
        std::wstring mapping(result.response.mapping_chars, L'\0');
        if (!mapping.empty() && !ipc::ReadAll(pipe_, mapping.data(),
            result.response.mapping_chars * sizeof(wchar_t))) return false;
        result.text.assign(result.response.text_chars, L'\0');
        if (!result.text.empty() && !ipc::ReadAll(pipe_, result.text.data(),
            result.response.text_chars * sizeof(wchar_t))) return false;
        result.error.assign(result.response.error_chars, L'\0');
        if (!result.error.empty() && !ipc::ReadAll(pipe_, result.error.data(),
            result.response.error_chars * sizeof(wchar_t))) return false;
        result.properties.clear();
        if (result.response.property_count > 6) return false;
        auto read_string = [&](std::wstring& value) {
            uint32_t count = 0;
            if (!ipc::ReadAll(pipe_, &count, sizeof(count)) || count > 32768) return false;
            value.assign(count, L'\0');
            return count == 0 || ipc::ReadAll(pipe_, value.data(), count * sizeof(wchar_t));
        };
        for (uint32_t i = 0; i < result.response.property_count; ++i) {
            std::wstring label, value;
            if (!read_string(label) || !read_string(value)) return false;
            result.properties.emplace_back(std::move(label), std::move(value));
        }
        if (!mapping.empty()) {
            const unsigned char ack = 1;
            if (!ipc::WriteAll(pipe_, &ack, 1)) return false;
        }
        return true;
    }
private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_{};
    uint32_t next_ = 1;
};
}

bool RunThumbnailCacheTests();

int wmain(int argc, wchar_t** argv) {
    Check(preview::VideoCodecDisplayName(L"{34363248-0000-0010-8000-00AA00389B71}") == L"H.264 (AVC)",
          L"codec: screenshot H264 subtype is a readable codec name");
    Check(preview::VideoCodecDisplayName(L" 34363268-0000-0010-8000-00aa00389b71 ") == L"H.264 (AVC)",
          L"codec: lowercase subtype and missing braces are supported");
    Check(preview::VideoCodecDisplayName(L"{43564548-0000-0010-8000-00AA00389B71}") == L"H.265 (HEVC)",
          L"codec: HEVC subtype is recognized");
    Check(preview::VideoCodecDisplayName(L"avc1") == L"H.264 (AVC)" &&
          preview::VideoCodecDisplayName(L"hvc1") == L"H.265 (HEVC)" &&
          preview::VideoCodecDisplayName(L"av01") == L"AV1" &&
          preview::VideoCodecDisplayName(L"VP90") == L"VP9",
          L"codec: common video FOURCC aliases are recognized");
    Check(preview::VideoCodecDisplayName(L"{44434241-0000-0010-8000-00AA00389B71}") == L"ABCD",
          L"codec: unknown printable FOURCC remains readable without guessing");
    Check(preview::VideoCodecDisplayName(L"{34363248-1111-0010-8000-00AA00389B71}").empty() &&
          preview::VideoCodecDisplayName(L"{00000001-0000-0010-8000-00AA00389B71}").empty() &&
          preview::VideoCodecDisplayName(L"{34363248-broken}").empty(),
          L"codec: unrelated GUIDs and malformed identifiers are not mislabeled");
    Check(preview::VideoCodecDisplayName(L"Apple ProRes 422") == L"Apple ProRes 422" &&
          preview::VideoCodecDisplayName(L" ").empty(),
          L"codec: existing descriptive names and empty properties are preserved");
    Check(RunThumbnailCacheTests(), L"thumbnail cache regressions");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring root = std::wstring(temp) + L"PulsePreviewTest-" +
                              std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    const auto path = [&](const wchar_t* name) { return root + L"\\" + name; };

    std::vector<unsigned char> utf8 = {'h','e','l','l','o','\n'};
    std::vector<unsigned char> le = {0xFF,0xFE,'A',0,0x2D,0x4E};
    std::vector<unsigned char> be = {0xFE,0xFF,0, 'B',0x4E,0x2D};
    std::vector<unsigned char> acp = {'A', 0xE9, 'B'};
    std::vector<unsigned char> lpm = {'[', 'p', 'u', 'l', 's', 'e', ']', '\n'};
    std::vector<unsigned char> binary(400, 0);
    for (size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<unsigned char>(i);
    std::vector<unsigned char> large(32 * 1024, 'x');
    const std::vector<unsigned char> bmp = {
        0x42,0x4D,70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,
        2,0,0,0,2,0,0,0,1,0,24,0,0,0,0,0,16,0,0,0,
        0x13,0x0B,0,0,0x13,0x0B,0,0,0,0,0,0,0,0,0,0,
        0,0,255,0,255,0,0,0,255,0,0,255,255,255,0,0
    };
    Check(WriteBytes(path(L"utf8.txt"), utf8), L"create UTF-8 fixture");
    Check(WriteBytes(path(L"utf16le.txt"), le), L"create UTF-16 LE fixture");
    Check(WriteBytes(path(L"utf16be.txt"), be), L"create UTF-16 BE fixture");
    Check(WriteBytes(path(L"acp.txt"), acp), L"create ACP fallback fixture");
    Check(WriteBytes(path(L"settings.lpm"), lpm), L"create LPM text fixture");
    Check(WriteBytes(path(L"sample.bin"), binary), L"create binary fixture");
    Check(WriteBytes(path(L"image.bmp"), bmp), L"create bitmap fixture");
    Check(WriteBytes(path(L"sparse.txt"), large, 1024ull * 1024ull * 1024ull),
          L"create 1 GB sparse text fixture");

    Host host;
    Check(host.Start(), L"start isolated preview host");
    // Optional real H.264 fixture exercises the Windows property provider too.
    if (argc > 1) {
        Result video;
        const bool received = host.Request(argv[1], video, MAXDWORD,
            ipc::kPreviewDefaultPixelSize, ipc::PreviewRequestKind::Properties);
        bool h264 = false, raw_guid = false;
        for (const auto& [label, value] : video.properties) {
            h264 |= value == L"H.264 (AVC)";
            raw_guid |= value.find(L"34363248-") != std::wstring::npos;
        }
        Check(received && h264 && !raw_guid,
              L"codec: actual H264 file returns friendly name through preview host protocol");
    }
    auto expectText = [&](const wchar_t* name, const wchar_t* contains) {
        Result result;
        const bool ok = host.Request(path(name), result);
        Check(ok && result.response.status == 0 &&
              result.response.kind == ipc::PreviewContentKind::Text &&
              result.text.find(contains) != std::wstring::npos, name);
        return result;
    };
    expectText(L"utf8.txt", L"hello");
    expectText(L"utf16le.txt", L"A");
    expectText(L"utf16be.txt", L"B");
    expectText(L"acp.txt", L"A");
    expectText(L"settings.lpm", L"pulse");

    Result hex;
    Check(host.Request(path(L"sample.bin"), hex) &&
          hex.response.kind == ipc::PreviewContentKind::Hex &&
          hex.response.bytes_read == 256 &&
          (hex.response.flags & ipc::kPreviewFlagTruncated),
          L"binary sniff returns 256-byte truncated hex");

    Result sparse;
    Check(host.Request(path(L"sparse.txt"), sparse) &&
          sparse.response.kind == ipc::PreviewContentKind::Text &&
          sparse.response.bytes_read <= 32 * 1024 &&
          (sparse.response.flags & ipc::kPreviewFlagTruncated),
          L"1 GB text reads at most 32 KiB and reports truncation");

    std::vector<double> bitmapTimings;
    for (int i = 0; i < 10; ++i) {
        Result result;
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = host.Request(path(L"image.bmp"), result);
        const auto end = std::chrono::steady_clock::now();
        if (ok && result.response.kind == ipc::PreviewContentKind::Bitmap &&
            result.response.width > 0 && result.response.height > 0)
            bitmapTimings.push_back(
                std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::sort(bitmapTimings.begin(), bitmapTimings.end());
    const double bitmapP95 = bitmapTimings.empty() ? 9999.0
        : bitmapTimings[(bitmapTimings.size() * 95 - 1) / 100];
    std::wprintf(L"[INFO] bitmap/WIC image decode P95 %.2f ms\n", bitmapP95);
    Check(bitmapTimings.size() == 10 && bitmapP95 <= 500.0,
          L"bitmap/WIC image decode P95 <= 500 ms");

    Result tiny;
    Check(host.Request(path(L"image.bmp"), tiny) &&
          tiny.response.kind == ipc::PreviewContentKind::Bitmap &&
          tiny.response.source_width == 2 && tiny.response.source_height == 2 &&
          tiny.response.width == 2 && tiny.response.height == 2,
          L"2x2 bitmap reports source and decoded size");

    Check(WriteBmpRgb(path(L"large.bmp"), 1280, 720), L"create 1280x720 bitmap fixture");
    Result scaled512;
    Check(host.Request(path(L"large.bmp"), scaled512, MAXDWORD, 512) &&
          scaled512.response.kind == ipc::PreviewContentKind::Bitmap &&
          scaled512.response.source_width == 1280 && scaled512.response.source_height == 720 &&
          (std::max)(scaled512.response.width, scaled512.response.height) <= 512,
          L"request 512 keeps decoded longest edge <= 512 and reports source size");
    Result scaled1024;
    Check(host.Request(path(L"large.bmp"), scaled1024, MAXDWORD, 1024) &&
          scaled1024.response.kind == ipc::PreviewContentKind::Bitmap &&
          scaled1024.response.source_width == 1280 && scaled1024.response.source_height == 720 &&
          (std::max)(scaled1024.response.width, scaled1024.response.height) <= 1024 &&
          (std::max)(scaled1024.response.width, scaled1024.response.height) > 512,
          L"request 1024 keeps decoded longest edge <= 1024");
    Result clamped;
    Check(host.Request(path(L"large.bmp"), clamped, MAXDWORD, 2048) &&
          clamped.response.kind == ipc::PreviewContentKind::Bitmap &&
          (std::max)(clamped.response.width, clamped.response.height) <= 1024,
          L"host clamps pixel_size above 1024");

    Check(CreateDirectoryW(path(L"subdir").c_str(), nullptr), L"create directory fixture");
    Result dirExtended;
    Check(host.Request(L"\\\\?\\" + path(L"subdir"), dirExtended) &&
          dirExtended.response.status == 0 &&
          dirExtended.response.kind == ipc::PreviewContentKind::Bitmap &&
          dirExtended.response.width > 0 && dirExtended.response.height > 0,
          L"shell thumbnail accepts \\\\?\\ extended path");

    Result offline;
    Check(host.Request(path(L"utf8.txt"), offline, 0x00400000u) &&
          offline.response.kind == ipc::PreviewContentKind::Unsupported &&
          offline.response.bytes_read == 0 && offline.error == L"offline-placeholder",
          L"offline placeholder is not read or hydrated");
    Result recallOnOpen;
    Check(host.Request(path(L"utf8.txt"), recallOnOpen, 0x00040000u) &&
          recallOnOpen.response.kind == ipc::PreviewContentKind::Unsupported &&
          recallOnOpen.response.bytes_read == 0 &&
          recallOnOpen.error == L"offline-placeholder",
          L"recall-on-open placeholder is not read or hydrated");
    Result missing;
    Check(host.Request(path(L"missing.txt"), missing) &&
          missing.response.kind == ipc::PreviewContentKind::Unsupported &&
          missing.error == L"path-unavailable",
          L"disappeared path does not break preview host");

    std::vector<double> timings;
    for (int i = 0; i < 25; ++i) {
        Result result;
        const auto begin = std::chrono::steady_clock::now();
        const bool ok = host.Request(path(L"utf8.txt"), result);
        const auto end = std::chrono::steady_clock::now();
        if (ok) timings.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::sort(timings.begin(), timings.end());
    const double p95 = timings.empty() ? 9999.0 : timings[(timings.size() * 95 - 1) / 100];
    std::wprintf(L"[INFO] local text P95 %.2f ms\n", p95);
    Check(timings.size() == 25 && p95 <= 150.0, L"local text protocol P95 <= 150 ms");

    host.Stop();
    Check(host.Start(), L"preview host restarts after termination");
    Result restarted;
    Check(host.Request(path(L"utf8.txt"), restarted) &&
          restarted.response.kind == ipc::PreviewContentKind::Text,
          L"restarted host serves requests");
    host.Stop();

    DeleteFileW(path(L"utf8.txt").c_str());
    DeleteFileW(path(L"utf16le.txt").c_str());
    DeleteFileW(path(L"utf16be.txt").c_str());
    DeleteFileW(path(L"acp.txt").c_str());
    DeleteFileW(path(L"sample.bin").c_str());
    DeleteFileW(path(L"image.bmp").c_str());
    DeleteFileW(path(L"large.bmp").c_str());
    DeleteFileW(path(L"sparse.txt").c_str());
    RemoveDirectoryW(path(L"subdir").c_str());
    RemoveDirectoryW(root.c_str());
    std::wprintf(L"\n== preview self test: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
