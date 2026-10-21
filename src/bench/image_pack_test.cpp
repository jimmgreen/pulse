// image_pack_test - image preview pack: parsing units, pack resolution and an
// end-to-end run of pulse-imgpack.exe, directly and through Pulse.Preview.exe.
//
// Integration needs a built pack (PULSE_TEST_IMGPACK=<folder holding
// pulse-imgpack.exe>) and FFmpeg to generate fixtures (PULSE_TEST_FFMPEG, else
// C:\ffmpeg\bin). HEIC fixtures are optional: PULSE_TEST_HEIC_DIR with
// photo.heic (1600x1200, a 320 px thumbnail), alpha.heic, pq10.heic (10-bit
// PQ). Missing pieces are skipped (reported, not failed). Everything runs
// under a private LOCALAPPDATA, so the user's packs and settings are untouched.
#include "../preview_host/image_pack.h"
#include "../common/image_pack_protocol.h"
#include "../common/preview_packs.h"
#include "../common/ffmpeg_tool.h"
#include "preview_host_client.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace pulse;

static int g_failures = 0;
static int g_checks = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

static std::wstring Env(const wchar_t* name) {
    wchar_t buffer[32768];
    const DWORD n = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    return n && n < ARRAYSIZE(buffer) ? std::wstring(buffer, n) : std::wstring{};
}

static void WriteBytes(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << bytes;
}

static std::string ReadBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static bool RunWait(const std::wstring& command, DWORD timeout_ms = 60000) {
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = command;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    const bool done = WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0;
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (!done) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return done && code == 0;
}

static std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

static bool PixelsVary(const std::vector<uint8_t>& px) {
    if (px.size() < 8) return false;
    for (size_t i = 4; i < px.size(); i += 4)
        if (px[i] != px[0] || px[i + 1] != px[1] || px[i + 2] != px[2]) return true;
    return false;
}

static bool HasLabel(const std::vector<preview::PreviewPropertyValue>& rows, const wchar_t* label,
                     const wchar_t* contains) {
    return std::any_of(rows.begin(), rows.end(), [&](const preview::PreviewPropertyValue& row) {
        return row.label == label && row.value.find(contains) != std::wstring::npos;
    });
}

static std::string Header(uint32_t w, uint32_t h, uint32_t stride, uint32_t magic = imgpack::kMagic) {
    imgpack::FrameHeader header;
    header.magic = magic;
    header.width = w; header.height = h; header.stride = stride;
    header.source_width = w * 4; header.source_height = h * 4;
    header.flags = imgpack::kFlagAlpha;
    return std::string(reinterpret_cast<const char*>(&header), sizeof(header));
}

static std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static void UnitTests() {
    using namespace preview;
    Check(IsImagePackExtension(L".heic") && IsImagePackExtension(L".QOI") && !IsImagePackExtension(L".png"),
          "pack extensions");
    Check(IsImagePackOnlyExtension(L".jxl") && IsImagePackOnlyExtension(L".exr") &&
          IsImagePackOnlyExtension(L".hdr") && !IsImagePackOnlyExtension(L".heic") &&
          !IsImagePackOnlyExtension(L".avif"), "pack-only extensions are the ones WIC's list lacks");
    Check(ImagePackConcurrency(1) == 1 && ImagePackConcurrency(8) == 2 && ImagePackConcurrency(64) == 3,
          "decoder concurrency follows the CPU count, capped");

    auto sniff = [](const std::string& s) { return SniffImagePackFormat(reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
    Check(sniff(std::string("\0\0\0\x18" "ftypheic\0\0\0\0", 16)) == L"HEIF", "sniff HEIC");
    Check(sniff(std::string("\0\0\0\x1c" "ftypavif\0\0\0\0", 16)) == L"AVIF", "sniff AVIF");
    Check(sniff(std::string("\0\0\0\x18" "ftypmp42\0\0\0\0", 16)).empty(), "an MP4 named .heic is not a picture");
    Check(sniff("\xFF\x0A\xFA\x7F") == L"JPEG XL", "sniff JPEG XL codestream");
    Check(sniff(std::string("\0\0\0\x0CJXL \x0D\x0A\x87\x0A", 12)) == L"JPEG XL", "sniff JPEG XL container");
    Check(sniff("\x76\x2F\x31\x01\x02\0\0\0") == L"OpenEXR", "sniff OpenEXR");
    Check(sniff("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n") == L"Radiance HDR", "sniff Radiance HDR");
    Check(sniff("#?RGBE\n") == L"Radiance HDR" && sniff("#?PFSTOOLS\r\n") == L"Radiance HDR",
          "sniff Radiance HDR of other programs");
    Check(sniff("FORMAT=32-bit_rle_xyze\n\n-Y 1 +X 1\n") == L"Radiance HDR", "sniff Radiance HDR without magic");
    Check(sniff("#?\n").empty() && sniff("#!/bin/sh\n").empty(), "a bare #? is not Radiance");
    Check(sniff("ENVI\ndescription = {") .empty(), "an ENVI .hdr header is text");
    Check(sniff(std::string("qoif\0\0\0\x10\0\0\0\x10\x04\0", 14)) == L"QOI", "sniff QOI");
    Check(SniffImagePackFormat(nullptr, 0).empty(), "sniff nothing");

    ImagePackFrame frame;
    const std::string pixels(2 * 3 * 4, '\x7F');
    Check(ParseImagePackFrame(Bytes(Header(2, 3, 8) + pixels), 256, frame) && frame.width == 2 && frame.height == 3 &&
          frame.stride == 8 && frame.source_width == 8 && frame.flags == imgpack::kFlagAlpha &&
          frame.pixels.size() == pixels.size(), "frame parses");
    Check(!ParseImagePackFrame(Bytes(Header(2, 3, 8, 0x12345678) + pixels), 256, frame), "bad magic rejected");
    Check(!ParseImagePackFrame(Bytes(Header(2, 3, 12) + std::string(36, 0)), 256, frame), "padded stride rejected");
    Check(!ParseImagePackFrame(Bytes(Header(2, 3, 8) + pixels.substr(1)), 256, frame), "short pixels rejected");
    Check(!ParseImagePackFrame(Bytes(Header(2, 3, 8) + pixels + "x"), 256, frame), "trailing bytes rejected");
    Check(!ParseImagePackFrame(Bytes(Header(300, 1, 1200) + std::string(1200, 0)), 256, frame), "larger than cap rejected");
    Check(!ParseImagePackFrame(Bytes(Header(0, 1, 0)), 256, frame), "empty frame rejected");

    const auto avif = ParseImagePackProbe("format=AVIF\nwidth=3840\nheight=2160\nbit_depth=10\nhdr=PQ\nprimaries=BT.2020\nalpha=yes\n");
    Check(HasLabel(avif, L"尺寸", L"3840 x 2160"), "probe size row");
    Check(HasLabel(avif, L"格式", L"AVIF · 10 位 · HDR PQ · BT.2020 · 透明"), "probe format row");
    const auto exr = ParseImagePackProbe("format=OpenEXR\r\nwidth=2048\r\nheight=1024\r\nchannels=A, B, G, R, Z\r\ncompression=PIZ\r\nparts=3\r\n");
    Check(HasLabel(exr, L"压缩", L"PIZ") && HasLabel(exr, L"通道", L"A, B, G, R, Z") && HasLabel(exr, L"图层", L"3"),
          "probe EXR rows (CRLF)");
    const auto jxl = ParseImagePackProbe("format=JPEG XL\nwidth=10\nheight=20\nbit_depth=8\nlossless=yes\nanimated=yes\nalpha=\n");
    Check(HasLabel(jxl, L"格式", L"JPEG XL · 无损 · 动画") && !HasLabel(jxl, L"格式", L"透明") && !HasLabel(jxl, L"格式", L"位"),
          "probe JPEG XL row, 8-bit and empty values omitted");
    Check(ParseImagePackProbe("garbage\n=\nwidth=0\n").empty(), "probe garbage gives no rows");
}

static void ResolutionTests(const fs::path& profile) {
    using namespace packs;
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    Check(ResolvePackUncached(PackId::Images).source == ToolSource::None, "no image pack -> unavailable");
    const fs::path base = fs::path(PacksRoot()) / L"images";
    WriteBytes(base / L"installed.json", "{\"version\":\"1\",\"dir\":\"1\"}");
    Check(!ReadInstalledPack(PackId::Images).present, "installed.json without pulse-imgpack.exe is not present");
    WriteBytes(base / L"1" / L"pulse-imgpack.exe", "MZ");
    Check(ResolvePackUncached(PackId::Images).source == ToolSource::Pack, "installed image pack resolves");
    PackSettings settings;
    settings.enabled[static_cast<uint32_t>(PackId::Images)] = false;
    SavePackSettings(settings);
    Check(ResolvePackUncached(PackId::Images).source == ToolSource::None, "disabled image pack does not resolve");
    settings.use_custom_ffmpeg = true;   // the FFmpeg override never applies to images
    settings.custom_ffmpeg = L"C:\\Windows\\System32\\cmd.exe";
    settings.enabled[static_cast<uint32_t>(PackId::Images)] = true;
    SavePackSettings(settings);
    Check(ResolvePackUncached(PackId::Images).directory == (base / L"1").wstring(), "custom FFmpeg does not redirect images");
    fs::remove_all(profile / L"Pulse");
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    const fs::path root = fs::temp_directory_path() / (L"pulse_image_pack_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    const std::wstring saved_profile = Env(L"LOCALAPPDATA");

    UnitTests();
    ResolutionTests(root / L"profile_units");

    const std::wstring pack_dir = Env(L"PULSE_TEST_IMGPACK");
    std::wstring ffmpeg = Env(L"PULSE_TEST_FFMPEG");
    if (ffmpeg.empty() && packs::IsRegularFile(L"C:\\ffmpeg\\bin\\ffmpeg.exe")) ffmpeg = L"C:\\ffmpeg\\bin\\ffmpeg.exe";
    if (pack_dir.empty() || !packs::IsRegularFile(pack_dir + L"\\pulse-imgpack.exe") || ffmpeg.empty()) {
        printf("SKIP integration: set PULSE_TEST_IMGPACK (and PULSE_TEST_FFMPEG)\n");
    } else {
        // Fixtures. A space and CJK in the folder name on purpose.
        const fs::path images = root / L"图片 dir";
        fs::create_directories(images);
        auto gen = [&](const std::wstring& args, const fs::path& out) {
            return RunWait(Quote(ffmpeg) + L" -hide_banner -loglevel error -y " + args + L" " + Quote(out.wstring()));
        };
        const std::wstring src = L"-f lavfi -i testsrc2=size=1280x720:rate=1 -frames:v 1 ";
        const fs::path qoi = images / L"frame.qoi", hdr = images / L"sky.hdr", exr = images / L"render.exr",
            avif = images / L"photo.avif", jxl = images / L"photo.jxl", big_exr = images / L"big.exr",
            dark_exr = images / L"dark.exr", junk = images / L"junk.exr", envi = images / L"envi.hdr",
            truncated = images / L"cut.qoi";
        Check(gen(src + L"-c:v qoi", qoi), "fixture: qoi");
        Check(gen(src + L"-pix_fmt gbrpf32le -c:v hdr", hdr), "fixture: radiance hdr");
        Check(gen(src + L"-pix_fmt gbrapf32le -c:v exr -compression 3", exr), "fixture: exr (zip)");
        Check(gen(L"-f lavfi -i testsrc2=size=8000x4000:rate=1 -frames:v 1 -pix_fmt gbrpf32le -c:v exr -compression 1", big_exr),
              "fixture: 8000x4000 exr");
        Check(gen(src + L"-vf lutrgb=r=val/16:g=val/16:b=val/16 -pix_fmt gbrpf32le -c:v exr", dark_exr), "fixture: dark exr");
        const bool have_avif = gen(src + L"-c:v libaom-av1 -still-picture 1 -crf 30 -cpu-used 8", avif) ||
                               gen(src + L"-c:v libsvtav1", avif);
        Check(have_avif, "fixture: avif");
        const bool have_jxl = gen(src + L"-c:v libjxl -distance 1", jxl);
        Check(have_jxl, "fixture: jxl");
        WriteBytes(junk, std::string("\x76\x2F\x31\x01") + std::string(5000, '\x55'));
        WriteBytes(envi, "ENVI\ndescription = {test}\nsamples = 100\n");
        const std::string q = ReadBytes(qoi);
        WriteBytes(truncated, q.substr(0, q.size() / 3));

        // The pack, installed into the private profile like the installer does.
        const fs::path profile = root / L"profile";
        SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
        const fs::path installed = fs::path(packs::PacksRoot()) / L"images" / L"test";
        fs::create_directories(installed);
        for (const auto& entry : fs::directory_iterator(pack_dir))
            if (entry.is_regular_file()) fs::copy_file(entry.path(), installed / entry.path().filename());
        WriteBytes(installed.parent_path() / L"installed.json", "{\"version\":\"test\",\"dir\":\"test\"}");
        Check(preview::ImagePackAvailable(), "ImagePackAvailable");

        struct Case { const char* name; fs::path file; UINT w, h; bool tonemapped; };
        std::vector<Case> cases = {
            {"qoi", qoi, 1280, 720, false}, {"hdr", hdr, 1280, 720, true}, {"exr", exr, 1280, 720, true}};
        if (have_avif) cases.push_back({"avif", avif, 1280, 720, false});
        if (have_jxl) cases.push_back({"jxl", jxl, 1280, 720, false});
        const std::wstring heic_dir = Env(L"PULSE_TEST_HEIC_DIR");
        const bool have_heic = !heic_dir.empty() && packs::IsRegularFile(heic_dir + L"\\photo.heic");
        if (have_heic) {
            cases.push_back({"heic", fs::path(heic_dir) / L"photo.heic", 1600, 1200, false});
            cases.push_back({"heic alpha", fs::path(heic_dir) / L"alpha.heic", 640, 480, false});
            cases.push_back({"heic pq", fs::path(heic_dir) / L"pq10.heic", 960, 540, true});
        } else {
            printf("SKIP heic: set PULSE_TEST_HEIC_DIR\n");
        }
        char what[160];
        for (const Case& c : cases) {
            preview::ImagePackFrame frame;
            ULONGLONG t0 = GetTickCount64();
            const bool grid_ok = preview::ImagePackDecode(c.file.wstring(), 256, true, frame);
            const ULONGLONG grid_ms = GetTickCount64() - t0;
            const UINT gw = c.w >= c.h ? 256 : static_cast<UINT>(256.0 * c.w / c.h + 0.5);
            printf("     %-10s grid %ux%u in %llu ms (flags %u)\n", c.name, frame.width, frame.height, grid_ms, frame.flags);
            snprintf(what, sizeof(what), "%s: grid thumbnail fits 256 and keeps the source size", c.name);
            Check(grid_ok && frame.width == gw && frame.source_width == c.w && frame.source_height == c.h &&
                  PixelsVary(frame.pixels), what);
            snprintf(what, sizeof(what), "%s: tone-mapped flag %s", c.name, c.tonemapped ? "set" : "clear");
            Check(((frame.flags & imgpack::kFlagToneMapped) != 0) == c.tonemapped, what);
            t0 = GetTickCount64();
            const bool view_ok = preview::ImagePackDecode(c.file.wstring(), 4096, false, frame);
            printf("     %-10s view %ux%u in %llu ms\n", c.name, frame.width, frame.height, GetTickCount64() - t0);
            snprintf(what, sizeof(what), "%s: full view is never enlarged", c.name);
            Check(view_ok && frame.width == c.w && frame.height == c.h && frame.stride == c.w * 4 &&
                  frame.pixels.size() == static_cast<size_t>(c.w) * c.h * 4, what);
            std::vector<preview::PreviewPropertyValue> rows;
            snprintf(what, sizeof(what), "%s: details rows", c.name);
            Check(preview::ImagePackProperties(c.file.wstring(), rows, 6) && HasLabel(rows, L"尺寸", L" x ") &&
                  HasLabel(rows, L"格式", L""), what);
        }
        if (have_heic) {
            preview::ImagePackFrame frame;
            Check(preview::ImagePackDecode(heic_dir + L"\\photo.heic", 256, true, frame) &&
                  (frame.flags & imgpack::kFlagEmbeddedThumbnail), "heic: grid uses the embedded thumbnail");
            Check(preview::ImagePackDecode(heic_dir + L"\\photo.heic", 1024, false, frame) &&
                  !(frame.flags & imgpack::kFlagEmbeddedThumbnail), "heic: larger views decode the photo");
            Check(preview::ImagePackDecode(heic_dir + L"\\alpha.heic", 256, true, frame) &&
                  (frame.flags & imgpack::kFlagAlpha) && frame.pixels[3] < 40, "heic: transparency is kept (premultiplied)");
            std::vector<preview::PreviewPropertyValue> rows;
            preview::ImagePackProperties(heic_dir + L"\\pq10.heic", rows, 6);
            Check(HasLabel(rows, L"格式", L"HDR PQ"), "heic: PQ shows in the details");
        }
        {   // Auto exposure lifts a dark render toward middle grey.
            preview::ImagePackFrame dark, normal;
            preview::ImagePackDecode(dark_exr.wstring(), 64, true, dark);
            preview::ImagePackDecode(exr.wstring(), 64, true, normal);
            double sd = 0, sn = 0;
            for (size_t i = 0; i < dark.pixels.size(); ++i) if (i % 4 != 3) sd += dark.pixels[i];
            for (size_t i = 0; i < normal.pixels.size(); ++i) if (i % 4 != 3) sn += normal.pixels[i];
            printf("     exposure: dark mean %.1f, normal mean %.1f\n", sd / (dark.pixels.size() * 0.75),
                   sn / (normal.pixels.size() * 0.75));
            Check(!dark.pixels.empty() && sd > sn * 0.6, "exr: auto exposure brightens a dark render");
        }
        {   // 32 MP float EXR: streamed in bands, small output.
            preview::ImagePackFrame frame;
            const ULONGLONG t0 = GetTickCount64();
            const bool ok = preview::ImagePackDecode(big_exr.wstring(), 256, true, frame);
            printf("     big exr 8000x4000 -> %ux%u in %llu ms\n", frame.width, frame.height, GetTickCount64() - t0);
            Check(ok && frame.width == 256 && frame.height == 128 && frame.source_width == 8000, "exr: 8000x4000 thumbnail");
        }
        {   // Failures are quick and clean.
            preview::ImagePackFrame frame;
            ULONGLONG t0 = GetTickCount64();
            Check(!preview::ImagePackDecode(junk.wstring(), 256, true, frame) && frame.pixels.empty(), "damaged exr fails");
            Check(!preview::ImagePackDecode(truncated.wstring(), 256, true, frame), "truncated qoi fails");
            Check(!preview::ImagePackDecode(envi.wstring(), 256, true, frame), "ENVI text .hdr is not decoded");
            Check(!preview::ImagePackDecode((images / L"missing.qoi").wstring(), 256, true, frame), "missing file fails");
            Check(GetTickCount64() - t0 < 3000, "failures answer within 3 s");
            Check(!preview::LooksLikeImagePackFile(envi.wstring()) && preview::LooksLikeImagePackFile(qoi.wstring()),
                  "sniffing tells pictures from text");
        }
        {   // Radiance files from other tools: bottom-up / mirrored / column
            // scanlines, CRLF headers, other magics, XYZE pixels.
            std::string px;
            for (int i = 0; i < 16 * 8; ++i) px += std::string{static_cast<char>(10 + i), 40, 90, static_cast<char>(129)};
            const char* heads[] = {"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+Y 8 +X 16\n",
                                   "#?RGBE\r\nEXPOSURE=1\r\n\r\n-Y 8 -X 16\r\n",
                                   "#?PFSTOOLS\nFORMAT=32-bit_rle_xyze\n\n+X 16 -Y 8\n"};
            int n = 0;
            for (const char* head : heads) {
                const fs::path file = images / (L"variant" + std::to_wstring(n++) + L".hdr");
                WriteBytes(file, head + px);
                preview::ImagePackFrame frame;
                snprintf(what, sizeof(what), "radiance variant %d previews", n);
                Check(preview::LooksLikeImagePackFile(file.wstring()) &&
                      preview::ImagePackDecode(file.wstring(), 256, true, frame) && frame.width == 16 &&
                      frame.height == 8 && PixelsVary(frame.pixels), what);
            }
        }
        {   // A decoder that cannot even start (its DLLs fail to initialise when
            // commit runs out: 0xC0000142) must fail quietly. With the default
            // error mode Windows shows "application error" and the process waits
            // for a click; children inherit the launcher's SEM_FAILCRITICALERRORS.
            const UINT saved_mode = SetErrorMode(0);
            ffmpeg::Process process;
            ffmpeg::LaunchOptions options;
            options.memory_limit = 2u * 1024 * 1024;
            options.discard_stderr = true;
            const bool started = process.Start((installed / L"pulse-imgpack.exe").wstring(), L"version", options);
            Check(started && process.Wait(10000), "starved decoder exits by itself (no error box)");
            Check(started && ffmpeg::StartupFailed(process.ExitCode()), "starved decoder reports a start-up failure");
            Check((GetErrorMode() & SEM_FAILCRITICALERRORS) != 0, "launcher turns on SEM_FAILCRITICALERRORS");
            process.Terminate();
            SetErrorMode(saved_mode);
        }
        {   // A folder's worth of thumbnails at once: all succeed, slots bound the processes.
            std::atomic<int> ok{0};
            std::vector<std::thread> threads;
            const ULONGLONG t0 = GetTickCount64();
            for (int i = 0; i < 12; ++i)
                threads.emplace_back([&, i] {
                    preview::ImagePackFrame frame;
                    const fs::path& file = i % 3 == 0 ? qoi : i % 3 == 1 ? exr : hdr;
                    if (preview::ImagePackDecode(file.wstring(), 256, true, frame)) ++ok;
                });
            for (auto& t : threads) t.join();
            printf("     12 parallel thumbnails in %llu ms\n", GetTickCount64() - t0);
            Check(ok == 12, "12 parallel thumbnails all decode");
        }

        // End to end through the preview host and its decoder table.
        {
            pulse_test::Host host;
            Check(host.Start(), "preview host starts with the image pack");
            const uint32_t grid = ipc::kPreviewRequestFlagGrid;
            for (const fs::path& file : {qoi, exr, hdr}) {
                pulse_test::Result r;
                snprintf(what, sizeof(what), "host: %ls grid thumbnail", file.filename().c_str());
                Check(host.Request(file.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, grid) &&
                      r.response.kind == ipc::PreviewContentKind::Bitmap && r.response.width == 256, what);
            }
            if (have_jxl) {
                pulse_test::Result r;
                Check(host.Request(jxl.wstring(), r, MAXDWORD, 1024, ipc::PreviewRequestKind::Content, 0) &&
                      r.response.kind == ipc::PreviewContentKind::Bitmap, "host: jxl quick look (WIC or pack)");
            }
            if (have_heic) {
                pulse_test::Result r;
                Check(host.Request(heic_dir + L"\\photo.heic", r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, grid) &&
                      r.response.kind == ipc::PreviewContentKind::Bitmap, "host: heic grid thumbnail (WIC or pack)");
            }
            pulse_test::Result r;
            Check(host.Request(exr.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Properties) &&
                  std::any_of(r.properties.begin(), r.properties.end(), [](const auto& p) { return p.first == L"压缩"; }),
                  "host: exr details from the pack");
            r = {};
            Check(host.Request(envi.wstring(), r, MAXDWORD, 1024, ipc::PreviewRequestKind::Content, 0) &&
                  r.response.kind != ipc::PreviewContentKind::Bitmap && r.error != L"image-pack-missing",
                  "host: ENVI .hdr keeps the text preview");
        }
        // Without the pack: Quick Look learns which pack would read the file.
        fs::remove(installed.parent_path() / L"installed.json");
        {
            pulse_test::Host host;
            Check(host.Start(), "preview host starts without the image pack");
            pulse_test::Result r;
            host.Request(exr.wstring(), r, MAXDWORD, 1024, ipc::PreviewRequestKind::Content, 0);
            Check(r.response.kind != ipc::PreviewContentKind::Bitmap && r.error == L"image-pack-missing",
                  "host: exr without the pack names the pack");
            r = {};
            host.Request(qoi.wstring(), r, MAXDWORD, 256, ipc::PreviewRequestKind::Content, ipc::kPreviewRequestFlagGrid);
            Check(r.response.kind != ipc::PreviewContentKind::Bitmap && r.error != L"image-pack-missing",
                  "host: grid thumbnails without the pack stay quiet");
        }
    }

    SetEnvironmentVariableW(L"LOCALAPPDATA", saved_profile.empty() ? nullptr : saved_profile.c_str());
    std::error_code ec;
    fs::remove_all(root, ec);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
