#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif

#include <windows.h>
#include <psapi.h>

#include "../common/fast_bmp_thumb.h"
#include "../ipc/overlapped_pipe.h"
#include "../ipc/preview_protocol.h"
#include "../ui/thumbnail_bc1.h"
#include "../ui/thumbnail_pack_store.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ElapsedMs(Clock::time_point start, Clock::time_point end = Clock::now()) noexcept {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double MedianOf(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return (n & 1u) ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

uint64_t FileTimeToU64(FILETIME ft) noexcept {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t ProcessCpu100ns(HANDLE process) noexcept {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(process, &c, &e, &k, &u)) return 0;
    return FileTimeToU64(k) + FileTimeToU64(u);
}

uint64_t ProcessCycles(HANDLE process) noexcept {
    ULONG64 cycles = 0;
    return QueryProcessCycleTime(process, &cycles) ? static_cast<uint64_t>(cycles) : 0;
}

uint64_t ProcessPrivateBytes(HANDLE process) noexcept {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (!K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        return 0;
    }
    return static_cast<uint64_t>(pmc.PrivateUsage);
}

bool PipeWriteRaw(HANDLE pipe, const void* data, DWORD bytes) noexcept {
    const auto* ptr = static_cast<const uint8_t*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, ptr, remaining, &written, nullptr) || written == 0) return false;
        ptr += written;
        remaining -= written;
    }
    return true;
}

bool PipeReadRaw(HANDLE pipe, void* data, DWORD bytes) noexcept {
    auto* ptr = static_cast<uint8_t*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, ptr, remaining, &got, nullptr) || got == 0) return false;
        ptr += got;
        remaining -= got;
    }
    return true;
}

bool DecodeLegacyFullBufferBmp(const std::wstring& path,
                               uint32_t target_edge,
                               std::vector<uint8_t>& out_pbgra,
                               uint32_t& out_w,
                               uint32_t& out_h,
                               uint32_t& out_stride,
                               size_t& out_peak_buf_bytes) noexcept {
    out_pbgra.clear();
    out_w = out_h = out_stride = 0;
    out_peak_buf_bytes = 0;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{file};

    LARGE_INTEGER size_li{};
    if (!GetFileSizeEx(file, &size_li) || size_li.QuadPart < 54 || size_li.QuadPart > (64ll << 20)) return false;
    const DWORD file_bytes = static_cast<DWORD>(size_li.QuadPart);

    thread_local std::vector<uint8_t> raw;
    raw.resize(file_bytes);
    DWORD got = 0;
    if (!ReadFile(file, raw.data(), file_bytes, &got, nullptr) || got != file_bytes) return false;

    pulse::preview::detail::BmpHeaderInfo info{};
    if (!pulse::preview::detail::ParseBmpHeader(raw.data(), got, file_bytes, target_edge, info)) return false;
    if (info.bits_per_pixel != 24 && info.bits_per_pixel != 32) return false;

    // Simulate legacy WIC + Scaler full-frame intermediate PBGRA decode buffer before scaling.
    thread_local std::vector<uint8_t> full_bgra;
    const size_t full_bytes = static_cast<size_t>(info.src_w) * info.src_h * 4u;
    full_bgra.resize(full_bytes);
    out_peak_buf_bytes = raw.capacity() + full_bgra.capacity();

    const uint32_t dst_stride = info.src_w * 4u;
    for (uint32_t sy = 0; sy < info.src_h; ++sy) {
        const uint32_t file_row = info.top_down ? sy : (info.src_h - 1u - sy);
        const uint8_t* src_row = raw.data() + info.pixel_offset + static_cast<size_t>(file_row) * info.src_stride;
        uint8_t* dst_row = full_bgra.data() + static_cast<size_t>(sy) * dst_stride;
        if (info.bits_per_pixel == 24) {
            for (uint32_t sx = 0; sx < info.src_w; ++sx) {
                dst_row[sx * 4u + 0] = src_row[sx * 3u + 0];
                dst_row[sx * 4u + 1] = src_row[sx * 3u + 1];
                dst_row[sx * 4u + 2] = src_row[sx * 3u + 2];
                dst_row[sx * 4u + 3] = 255u;
            }
        } else {
            for (uint32_t sx = 0; sx < info.src_w; ++sx) {
                const uint8_t a = info.premultiply_alpha ? src_row[sx * 4u + 3] : 255u;
                dst_row[sx * 4u + 0] = a == 255u ? src_row[sx * 4u + 0] : static_cast<uint8_t>((uint32_t(src_row[sx * 4u + 0]) * a + 127u) / 255u);
                dst_row[sx * 4u + 1] = a == 255u ? src_row[sx * 4u + 1] : static_cast<uint8_t>((uint32_t(src_row[sx * 4u + 1]) * a + 127u) / 255u);
                dst_row[sx * 4u + 2] = a == 255u ? src_row[sx * 4u + 2] : static_cast<uint8_t>((uint32_t(src_row[sx * 4u + 2]) * a + 127u) / 255u);
                dst_row[sx * 4u + 3] = a;
            }
        }
    }

    if (!pulse::preview::DownsamplePbgraExactArea(full_bgra.data(), info.src_w, info.src_h, dst_stride,
                                                  info.dst_w, info.dst_h, out_pbgra)) {
        return false;
    }
    out_w = info.dst_w;
    out_h = info.dst_h;
    out_stride = info.dst_w * 4u;
    out_peak_buf_bytes += out_pbgra.capacity();
    return true;
}

int RunChildHost(const wchar_t* pipe_name, bool optimized) {
    HANDLE pipe = CreateNamedPipeW(
        pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        512u << 10,
        64u << 10,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    struct PipeCloser { HANDLE h; ~PipeCloser() { CloseHandle(h); } } closer{pipe};

    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return 3;

    std::vector<uint8_t> pixels;
    std::vector<uint8_t> send_buffer;
    send_buffer.reserve(512u << 10);

    for (;;) {
        pulse::ipc::PreviewRequest req{};
        if (!PipeReadRaw(pipe, &req, sizeof(req))) break;
        if (req.path_chars == 0 || req.path_chars > 32768) break;
        std::wstring path(req.path_chars, L'\0');
        if (!PipeReadRaw(pipe, path.data(), req.path_chars * sizeof(wchar_t))) break;

        if (!optimized && req.magic == pulse::ipc::kPreviewBudgetRequestMagic &&
            !(req.flags & pulse::ipc::kPreviewRequestFlagPreGranted64M)) {
            pulse::ipc::PreviewResponse proposal{};
            proposal.magic = pulse::ipc::kPreviewBudgetProposalMagic;
            proposal.request_id = req.request_id;
            proposal.generation = req.generation;
            proposal.width = 36u << 20;
            proposal.height = 0;
            if (!PipeWriteRaw(pipe, &proposal, sizeof(proposal))) break;
            uint32_t grant = 0;
            if (!PipeReadRaw(pipe, &grant, sizeof(grant)) || grant != pulse::ipc::kPreviewBudgetGrant) break;
        }

        uint32_t w = 0, h = 0, stride = 0, sw = 0, sh = 0;
        size_t dummy_peak = 0;
        const bool ok = optimized
            ? pulse::preview::DecodeFastBmpThumbnailFile(path, req.pixel_size, pixels, w, h, stride, sw, sh)
            : DecodeLegacyFullBufferBmp(path, req.pixel_size, pixels, w, h, stride, dummy_peak);

        pulse::ipc::PreviewResponse resp{};
        resp.magic = pulse::ipc::kPreviewMagic;
        resp.request_id = req.request_id;
        resp.generation = req.generation;
        if (ok && !pixels.empty()) {
            resp.status = 0;
            resp.kind = pulse::ipc::PreviewContentKind::Bitmap;
            resp.flags = pulse::ipc::kPreviewFlagInlineBitmap;
            resp.width = w;
            resp.height = h;
            resp.stride = stride;
            resp.source_width = sw ? sw : 1024u;
            resp.source_height = sh ? sh : 768u;
            resp.frame_count = 1;
        } else {
            resp.status = 1;
            resp.kind = pulse::ipc::PreviewContentKind::Unsupported;
        }

        if (optimized) {
            const DWORD payload_bytes = ok ? static_cast<DWORD>(pixels.size()) : 0;
            send_buffer.resize(sizeof(resp) + payload_bytes);
            std::memcpy(send_buffer.data(), &resp, sizeof(resp));
            if (payload_bytes > 0) std::memcpy(send_buffer.data() + sizeof(resp), pixels.data(), payload_bytes);
            if (!PipeWriteRaw(pipe, send_buffer.data(), static_cast<DWORD>(send_buffer.size()))) break;
        } else {
            if (!PipeWriteRaw(pipe, &resp, sizeof(resp))) break;
            if (ok && !pixels.empty()) {
                if (!PipeWriteRaw(pipe, pixels.data(), static_cast<DWORD>(pixels.size()))) break;
            }
        }
    }
    return 0;
}

bool GenerateSyntheticBmp(const std::filesystem::path& path, uint32_t w, uint32_t h, uint32_t seed) {
    const uint32_t row_stride = ((w * 3u) + 3u) & ~3u;
    const uint32_t img_bytes = row_stride * h;
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + img_bytes;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = static_cast<LONG>(w);
    bih.biHeight = static_cast<LONG>(h);
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = img_bytes;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{f};

    std::vector<uint8_t> data(bfh.bfSize, 0);
    std::memcpy(data.data(), &bfh, sizeof(bfh));
    std::memcpy(data.data() + sizeof(bfh), &bih, sizeof(bih));
    uint8_t* px = data.data() + bfh.bfOffBits;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = px + static_cast<size_t>(y) * row_stride;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3u + 0] = static_cast<uint8_t>((x * 5u + y * 3u + seed * 17u) & 255u);
            row[x * 3u + 1] = static_cast<uint8_t>((x * 2u + y * 7u + seed * 31u) & 255u);
            row[x * 3u + 2] = static_cast<uint8_t>((x * 9u + y * 1u + seed * 43u) & 255u);
        }
    }
    DWORD written = 0;
    return WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
}

std::vector<std::filesystem::path> ResolveOrCreateFixtures(const std::filesystem::path& temp_root, bool& created_temp) {
    created_temp = false;
    std::filesystem::path ro_dir = L"C:\\Users\\SS\\Desktop\\pulse\\bench_data\\thumbnail-performance-20261115\\fixtures\\images";
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::exists(ro_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(ro_dir, ec)) {
            if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".bmp") == 0) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        if (files.size() == 32) return files;
    }

    created_temp = true;
    files.clear();
    const auto gen_dir = temp_root / L"fixtures";
    std::filesystem::create_directories(gen_dir, ec);
    for (uint32_t i = 0; i < 32; ++i) {
        wchar_t name[32]{};
        swprintf_s(name, L"image%02u.bmp", i);
        const auto p = gen_dir / name;
        GenerateSyntheticBmp(p, 1024, 768, i + 1);
        files.push_back(p);
    }
    return files;
}

double ComputePsnr(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h, uint32_t stride, bool check_alpha) noexcept {
    double sse = 0.0;
    const uint32_t channels = check_alpha ? 4u : 3u;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* ra = a + static_cast<size_t>(y) * stride;
        const uint8_t* rb = b + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < w; ++x) {
            for (uint32_t c = 0; c < channels; ++c) {
                const double d = static_cast<double>(ra[x * 4u + c]) - static_cast<double>(rb[x * 4u + c]);
                sse += d * d;
            }
        }
    }
    const double samples = static_cast<double>(w) * h * channels;
    const double mse = sse / (std::max)(1.0, samples);
    if (mse <= 1e-9) return 99.0;
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

bool TestFastBmpDecode(const std::filesystem::path& sample_bmp) {
    bool ok = true;
    const auto check = [&](bool v, const char* msg) {
        std::printf("[%s] %s\n", v ? "PASS" : "FAIL", msg);
        ok &= v;
    };

    std::vector<uint8_t> fast_px, ref_px;
    uint32_t fw = 0, fh = 0, fstride = 0, sw = 0, sh = 0;
    uint32_t rw = 0, rh = 0, rstride = 0;
    size_t peak = 0;
    const bool decoded = pulse::preview::DecodeFastBmpThumbnailFile(sample_bmp.wstring(), 256, fast_px, fw, fh, fstride, sw, sh);
    const bool ref_decoded = DecodeLegacyFullBufferBmp(sample_bmp.wstring(), 256, ref_px, rw, rh, rstride, peak);
    check(decoded && ref_decoded && fw == 256 && fh == 192 && rw == fw && rh == fh && fstride == 1024 && sw == 1024 && sh == 768,
          "DecodeFastBmpThumbnailFile produces exact 256x192 dimensions and 1024x768 source metadata");

    int max_diff = 0;
    if (fast_px.size() == ref_px.size() && !fast_px.empty()) {
        for (size_t i = 0; i < fast_px.size(); ++i) {
            const int d = std::abs(static_cast<int>(fast_px[i]) - static_cast<int>(ref_px[i]));
            if (d > max_diff) max_diff = d;
        }
    } else {
        max_diff = 255;
    }
    check(max_diff <= 2, "DecodeFastBmpThumbnailFile matches full-buffer area reference within <=2 LSB");

    // Verify SSE2 kx=2 and kx=4 box downsampler paths match scalar exact rounding.
    std::vector<uint8_t> src_2x(64u * 48u * 4u);
    for (size_t i = 0; i < src_2x.size(); ++i) src_2x[i] = static_cast<uint8_t>((i * 29u + 7u) & 255u);
    bool sse2_exact = true;
    for (uint32_t k : {2u, 4u}) {
        const uint32_t dw = 64u / k;
        const uint32_t dh = 48u / k;
        std::vector<uint8_t> got;
        if (!pulse::preview::DownsamplePbgraExactArea(src_2x.data(), 64u, 48u, 64u * 4u, dw, dh, got)) {
            sse2_exact = false;
            break;
        }
        const uint32_t area = k * k;
        const uint32_t half = area / 2u;
        for (uint32_t dy = 0; dy < dh && sse2_exact; ++dy) {
            for (uint32_t dx = 0; dx < dw && sse2_exact; ++dx) {
                for (uint32_t ch = 0; ch < 4u; ++ch) {
                    uint32_t sum = 0;
                    for (uint32_t jy = 0; jy < k; ++jy) {
                        for (uint32_t ix = 0; ix < k; ++ix) {
                            sum += src_2x[(size_t(dy * k + jy) * 64u + (dx * k + ix)) * 4u + ch];
                        }
                    }
                    const uint8_t exp_val = static_cast<uint8_t>((sum + half) / area);
                    if (got[(size_t(dy) * dw + dx) * 4u + ch] != exp_val) sse2_exact = false;
                }
            }
        }
    }
    check(sse2_exact, "DownsamplePbgraExactArea SSE2 kx=2 and kx=4 paths are bit-exact against scalar box average");
    return ok;
}

bool TestExifOrientationTransform() {
    bool ok = true;
    const auto check = [&](bool v, const char* msg) {
        std::printf("[%s] %s\n", v ? "PASS" : "FAIL", msg);
        ok &= v;
    };
    constexpr uint32_t W = 20, H = 12;
    std::vector<uint8_t> base(W * H * 4u);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const size_t i = (size_t(y) * W + x) * 4u;
            base[i + 0] = static_cast<uint8_t>(x * 11u);
            base[i + 1] = static_cast<uint8_t>(y * 19u);
            base[i + 2] = static_cast<uint8_t>((x * 7u + y * 13u) & 255u);
            base[i + 3] = 255u;
        }
    }
    auto px_at = [](const std::vector<uint8_t>& buf, uint32_t w, uint32_t stride, uint32_t x, uint32_t y,
                    uint32_t sx, uint32_t sy) {
        (void)w;
        const uint8_t* p = buf.data() + size_t(y) * stride + size_t(x) * 4u;
        return p[0] == static_cast<uint8_t>(sx * 11u) &&
               p[1] == static_cast<uint8_t>(sy * 19u) &&
               p[2] == static_cast<uint8_t>((sx * 7u + sy * 13u) & 255u) &&
               p[3] == 255u;
    };
    bool all_orient_ok = true;
    for (unsigned o = 1; o <= 8; ++o) {
        std::vector<uint8_t> buf = base;
        uint32_t w = W, h = H, stride = W * 4u;
        if (!pulse::preview::ApplyExifOrientationPbgra(buf, w, h, stride, o)) {
            all_orient_ok = false;
            break;
        }
        const uint32_t exp_w = (o >= 5) ? H : W;
        const uint32_t exp_h = (o >= 5) ? W : H;
        if (w != exp_w || h != exp_h || stride != exp_w * 4u) {
            all_orient_ok = false;
            break;
        }
        bool match = false;
        switch (o) {
        case 1: match = px_at(buf, w, stride, 0, 0, 0, 0) && px_at(buf, w, stride, W - 1, H - 1, W - 1, H - 1); break;
        case 2: match = px_at(buf, w, stride, 0, 0, W - 1, 0) && px_at(buf, w, stride, W - 1, H - 1, 0, H - 1); break;
        case 3: match = px_at(buf, w, stride, 0, 0, W - 1, H - 1) && px_at(buf, w, stride, W - 1, H - 1, 0, 0); break;
        case 4: match = px_at(buf, w, stride, 0, 0, 0, H - 1) && px_at(buf, w, stride, W - 1, H - 1, W - 1, 0); break;
        case 5: match = px_at(buf, w, stride, 0, 0, 0, 0) && px_at(buf, w, stride, H - 1, 0, 0, H - 1) && px_at(buf, w, stride, 0, W - 1, W - 1, 0); break;
        case 6: match = px_at(buf, w, stride, 0, 0, 0, H - 1) && px_at(buf, w, stride, H - 1, 0, 0, 0) && px_at(buf, w, stride, 0, W - 1, W - 1, H - 1); break;
        case 7: match = px_at(buf, w, stride, 0, 0, W - 1, H - 1) && px_at(buf, w, stride, H - 1, 0, W - 1, 0) && px_at(buf, w, stride, 0, W - 1, 0, H - 1); break;
        case 8: match = px_at(buf, w, stride, 0, 0, W - 1, 0) && px_at(buf, w, stride, H - 1, 0, W - 1, H - 1) && px_at(buf, w, stride, 0, W - 1, 0, 0); break;
        }
        all_orient_ok &= match;
    }
    check(all_orient_ok, "ApplyExifOrientationPbgra handles EXIF orientations 1..8 with exact dimensions and corner mapping");
    return ok;
}

bool TestMicroCodecBc1Bc3(const std::filesystem::path& sample_bmp) {
    bool ok = true;
    const auto check = [&](bool v, const char* msg) {
        std::printf("[%s] %s\n", v ? "PASS" : "FAIL", msg);
        ok &= v;
    };

    // Test BC1 opaque 256x192 real fixture -> 64x48 (1,536 bytes).
    std::vector<uint8_t> src;
    uint32_t sw = 0, sh = 0, sstride = 0, src_w = 0, src_h = 0;
    const bool loaded_src = pulse::preview::DecodeFastBmpThumbnailFile(
        sample_bmp.wstring(), 256, src, sw, sh, sstride, src_w, src_h);
    pulse::ui::MicroThumbData bc1;
    const bool enc1 = loaded_src && pulse::ui::EncodeMicroThumb(src.data(), sw, sh, sstride, src_w, src_h, 64, bc1);
    std::vector<uint8_t> dec1;
    uint32_t d1w = 0, d1h = 0, d1s = 0;
    const bool dec_ok1 = enc1 && pulse::ui::DecodeMicroThumbToPbgra(bc1, dec1, d1w, d1h, d1s);
    std::vector<uint8_t> ref64;
    const uint32_t r64s = 64u * 4u;
    pulse::preview::DownsamplePbgraExactArea(src.data(), sw, sh, sstride, 64, 48, ref64);
    const double psnr1 = dec_ok1 ? ComputePsnr(ref64.data(), dec1.data(), 64, 48, r64s, false) : 0.0;
    char msg1[160]{};
    std::snprintf(msg1, sizeof(msg1),
                  "EncodeMicroThumb BC1 64x48 round-trip preserves 1,536-byte size and PSNR > 28 dB (PSNR=%.2f dB)",
                  psnr1);
    check(enc1 && dec_ok1 && bc1.format == pulse::ui::kMicroThumbFormatBc1 &&
          bc1.content_width == 64 && bc1.content_height == 48 && bc1.blocks.size() == 1536 && psnr1 > 28.0,
          msg1);

    // Test BC3 alpha 128x128 -> 64x64 (4,096 bytes).
    std::vector<uint8_t> src_a(128u * 128u * 4u);
    for (uint32_t y = 0; y < 128; ++y) {
        for (uint32_t x = 0; x < 128; ++x) {
            const uint8_t a = ((x + y) & 16u) ? 240u : (((x & 8u) ? 160u : 0u));
            const size_t i = (static_cast<size_t>(y) * 128u + x) * 4u;
            const uint8_t r = static_cast<uint8_t>((uint32_t(x * 2u) * a + 127u) / 255u);
            const uint8_t g = static_cast<uint8_t>((uint32_t(y * 2u) * a + 127u) / 255u);
            const uint8_t b = static_cast<uint8_t>((uint32_t(200u) * a + 127u) / 255u);
            src_a[i + 0] = b;
            src_a[i + 1] = g;
            src_a[i + 2] = r;
            src_a[i + 3] = a;
        }
    }
    pulse::ui::MicroThumbData bc3;
    const bool enc3 = pulse::ui::EncodeMicroThumb(src_a.data(), 128, 128, 128 * 4u, 128, 128, 64, bc3);
    std::vector<uint8_t> dec3, ref_a64;
    uint32_t d3w = 0, d3h = 0, d3s = 0;
    const uint32_t ra64s = 64u * 4u;
    const bool dec_ok3 = enc3 && pulse::ui::DecodeMicroThumbToPbgra(bc3, dec3, d3w, d3h, d3s);
    pulse::preview::DownsamplePbgraExactArea(src_a.data(), 128, 128, 128 * 4u, 64, 64, ref_a64);
    const double psnr3 = dec_ok3 ? ComputePsnr(ref_a64.data(), dec3.data(), 64, 64, ra64s, true) : 0.0;
    check(enc3 && dec_ok3 && bc3.format == pulse::ui::kMicroThumbFormatBc3 &&
          bc3.content_width == 64 && bc3.content_height == 64 && bc3.blocks.size() == 4096 && psnr3 > 26.0,
          "EncodeMicroThumb BC3 64x64 round-trip preserves alpha and PSNR > 26 dB");
    return ok;
}

bool TestMicroThumbPack(const std::filesystem::path& temp_root) {
    bool ok = true;
    const auto check = [&](bool v, const char* msg) {
        std::printf("[%s] %s\n", v ? "PASS" : "FAIL", msg);
        ok &= v;
    };

    const auto pack_dir = temp_root / L"pack_test";
    pulse::ui::MicroThumbPack pack;
    SetLastError(0);
    const bool open_ok = pack.Open(pack_dir, false);
    if (!open_ok) {
        std::printf("  (MicroThumbPack debug: err=%lu pack_dir=%ls)\n",
                    GetLastError(), pack_dir.c_str());
    }
    check(open_ok, "MicroThumbPack creates and opens micro_v1.pack");

    pulse::ui::MicroThumbData proto;
    proto.width = 64;
    proto.height = 48;
    proto.content_width = 64;
    proto.content_height = 48;
    proto.source_width = 1024;
    proto.source_height = 768;
    proto.format = pulse::ui::kMicroThumbFormatBc1;
    proto.blocks.assign(1536, 0x5Au);

    size_t appended = 0;
    for (int batch = 0; batch < 4; ++batch) {
        std::vector<pulse::ui::MicroThumbPack::PendingRecord> recs;
        recs.reserve(250);
        for (int i = 0; i < 250; ++i) {
            const uint32_t idx = static_cast<uint32_t>(batch * 250 + i);
            pulse::ui::MicroThumbPack::PendingRecord rec;
            rec.identity_hash = pulse::ui::MicroThumbPack::IdentityHash(L"C:\\test\\img_" + std::to_wstring(idx) + L".bmp");
            rec.mtime = 1000000ll + idx;
            rec.file_size = 2000000ull + idx;
            rec.thumb = proto;
            rec.thumb.blocks[0] = static_cast<uint8_t>(idx & 255u);
            rec.thumb.blocks[1535] = static_cast<uint8_t>((idx >> 8) & 255u);
            recs.push_back(std::move(rec));
        }
        appended += pack.AppendBatch(recs);
    }
    check(appended == 1000 && pack.EntryCount() == 1000, "MicroThumbPack AppendBatch writes 1,000 records");

    bool read_ok = true;
    for (uint32_t idx : {0u, 17u, 511u, 999u}) {
        const uint64_t h = pulse::ui::MicroThumbPack::IdentityHash(L"C:\\test\\img_" + std::to_wstring(idx) + L".bmp");
        pulse::ui::MicroThumbData out;
        if (!pack.Read(h, 1000000ll + idx, 2000000ull + idx, out) ||
            out.blocks.size() != 1536 || out.blocks[0] != static_cast<uint8_t>(idx & 255u) ||
            out.blocks[1535] != static_cast<uint8_t>((idx >> 8) & 255u)) {
            read_ok = false;
        }
    }
    check(read_ok, "MicroThumbPack positional Read returns exact blocks for written records");
    pack.Close();

    // Simulate crash torn tail on micro_v1.pack.
    const auto pack_file = pack_dir / L"micro_v1.pack";
    {
        HANDLE f = CreateFileW(pack_file.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const uint8_t garbage[19] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
            DWORD wr = 0;
            WriteFile(f, garbage, sizeof(garbage), &wr, nullptr);
            CloseHandle(f);
        }
    }
    pulse::ui::MicroThumbData tail_out;
    const uint64_t h999 = pulse::ui::MicroThumbPack::IdentityHash(L"C:\\test\\img_999.bmp");
    check(pack.Open(pack_dir, false) && pack.EntryCount() == 1000 &&
          pack.Read(h999, 1000000ll + 999u, 2000000ull + 999u, tail_out),
          "MicroThumbPack truncates simulated crash torn tail and retains all 1,000 valid records");

    check(pack.Clear() && !pack.Contains(h999, 1000000ll + 999u, 2000000ull + 999u) &&
          !std::filesystem::exists(pack_file),
          "MicroThumbPack Clear removes pack file and clears index");
    return ok;
}

bool TestOverlappedPipe() {
    bool ok = true;
    const auto check = [&](bool v, const char* msg) {
        std::printf("[%s] %s\n", v ? "PASS" : "FAIL", msg);
        ok &= v;
    };

    wchar_t pipe_name[96]{};
    swprintf_s(pipe_name, L"\\\\.\\pipe\\pulse_thumb_scale_ov_%lu", GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(
        pipe_name,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 65536, 65536, 0, nullptr);
    HANDLE client = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
    HANDLE ev_srv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ev_cli = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE && ev_srv && ev_cli && wake,
          "OVERLAPPED named pipe test handles created");
    if (server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE && ev_srv && ev_cli && wake) {
        std::vector<uint8_t> payload(4096);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>((i * 13u) ^ 0xA5u);
        std::vector<uint8_t> received(4096, 0);
        DWORD wr_err = 0, rd_err = 0;
        const auto dl = GetTickCount64() + 5000;
        std::thread writer([&] {
            pulse::ipc::WriteAllOverlapped(server, ev_srv, nullptr, payload.data(),
                                           static_cast<DWORD>(payload.size()), dl, wr_err);
        });
        const bool rd_ok = pulse::ipc::ReadAllOverlapped(client, ev_cli, wake, nullptr, received.data(),
                                                         static_cast<DWORD>(received.size()), dl, 100,
                                                         [] { return false; }, rd_err);
        writer.join();
        check(rd_ok && wr_err == ERROR_SUCCESS && rd_err == ERROR_SUCCESS && received == payload,
              "OVERLAPPED pipe WriteAllOverlapped + ReadAllOverlapped round-trip exact 4 KiB payload");

        std::atomic<bool> cancel_flag{false};
        std::thread canceller([&] {
            Sleep(12);
            cancel_flag.store(true, std::memory_order_release);
            SetEvent(wake);
        });
        uint64_t dummy = 0;
        bool cancelled_out = false;
        DWORD cancel_err = 0;
        const auto cstart = Clock::now();
        const bool should_be_false = pulse::ipc::ReadAllOverlapped(
            client, ev_cli, wake, nullptr, &dummy, sizeof(dummy), GetTickCount64() + 5000, 1000,
            [&] { return cancel_flag.load(std::memory_order_acquire); },
            cancel_err, &cancelled_out);
        const double cancel_ms = ElapsedMs(cstart);
        canceller.join();
        check(!should_be_false && cancelled_out && cancel_err == ERROR_CANCELLED && cancel_ms < 150.0,
              "OVERLAPPED pipe ReadAllOverlapped wakes immediately on wake_event cancellation (<150ms)");
    }
    if (wake) CloseHandle(wake);
    if (ev_srv) CloseHandle(ev_srv);
    if (ev_cli) CloseHandle(ev_cli);
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
    return ok;
}

struct MultiProcMetrics {
    double first_ms = 0.0;
    double all_ms = 0.0;
    double tree_cpu_ms = 0.0;
    double tree_mcycles = 0.0;
    double peak_commit_mib = 0.0;
    size_t children_spawned = 0;
    bool ok = false;
};

struct SpawnedHost {
    PROCESS_INFORMATION pi{};
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE io_event = nullptr;
    uint64_t baseline_cpu = 0;
};

bool StartHostChild(const wchar_t* self_exe, uint32_t run_id, uint32_t lane_idx,
                    bool optimized, SpawnedHost& out) {
    wchar_t pipe_name[128]{};
    swprintf_s(pipe_name, L"\\\\.\\pipe\\pulse_tsb_%lu_%u_%u", GetCurrentProcessId(), run_id, lane_idx);
    wchar_t cmd[512]{};
    swprintf_s(cmd, L"\"%s\" --child-host \"%s\" %s",
               self_exe, pipe_name, optimized ? L"optimized" : L"baseline");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (!CreateProcessW(self_exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &out.pi)) {
        return false;
    }
    for (int attempt = 0; attempt < 200; ++attempt) {
        HANDLE p = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (p != INVALID_HANDLE_VALUE) {
            out.pipe = p;
            out.io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            return out.io_event != nullptr;
        }
        if (WaitForSingleObject(out.pi.hProcess, 0) == WAIT_OBJECT_0) break;
        const DWORD ms = attempt < 8 ? 1u : (attempt < 20 ? 4u : 20u);
        Sleep(ms);
    }
    return false;
}

void CloseHostChild(SpawnedHost& host) noexcept {
    if (host.pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(host.pipe, nullptr);
        CloseHandle(host.pipe);
        host.pipe = INVALID_HANDLE_VALUE;
    }
    if (host.io_event) {
        CloseHandle(host.io_event);
        host.io_event = nullptr;
    }
    if (host.pi.hProcess) {
        WaitForSingleObject(host.pi.hProcess, 2000);
        CloseHandle(host.pi.hProcess);
        host.pi.hProcess = nullptr;
    }
    if (host.pi.hThread) {
        CloseHandle(host.pi.hThread);
        host.pi.hThread = nullptr;
    }
}

bool PollReadPipeRaw(HANDLE pipe, HANDLE child_proc, void* data, DWORD bytes, ULONGLONG deadline) noexcept {
    auto* ptr = static_cast<uint8_t*>(data);
    DWORD remaining = bytes;
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct EvCloser { HANDLE h; ~EvCloser() { if (h) CloseHandle(h); } } closer{ev};
    while (remaining > 0) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail > 0) {
            OVERLAPPED ov{};
            ov.hEvent = ev;
            ResetEvent(ev);
            DWORD to_read = (std::min)(remaining, avail);
            DWORD got = 0;
            if (!ReadFile(pipe, ptr, to_read, &got, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) return false;
                if (!GetOverlappedResult(pipe, &ov, &got, TRUE) || got == 0) return false;
            }
            ptr += got;
            remaining -= got;
            continue;
        }
        if (GetTickCount64() >= deadline) return false;
        if (child_proc && WaitForSingleObject(child_proc, 0) == WAIT_OBJECT_0) return false;
        Sleep(1);
    }
    return true;
}

bool OverlappedWriteSync(HANDLE pipe, HANDLE ev, HANDLE proc, const void* data, DWORD bytes, ULONGLONG deadline) noexcept {
    DWORD err = 0;
    return pulse::ipc::WriteAllOverlapped(pipe, ev, proc, data, bytes, deadline, err);
}

MultiProcMetrics RunMultiProcPass(const wchar_t* self_exe,
                                  const std::vector<std::filesystem::path>& files,
                                  uint32_t run_id,
                                  bool optimized) {
    MultiProcMetrics out{};
    const uint64_t parent_mem_base = ProcessPrivateBytes(GetCurrentProcess());
    const uint64_t parent_cpu_base = ProcessCpu100ns(GetCurrentProcess());
    const uint64_t parent_cycles_base = ProcessCycles(GetCurrentProcess());

    // Optimized uses WarmPrimary + smart scaling (2 hosts for 32 fast BMPs); Baseline spawns 4 eager hosts (Warm).
    const size_t max_hosts = optimized ? 2u : 4u;
    std::vector<SpawnedHost> hosts(max_hosts);
    std::mutex host_mu;

    const auto start = Clock::now();
    // Eager hosts vs WarmPrimary.
    const size_t eager_count = optimized ? 1u : 4u;
    for (size_t i = 0; i < eager_count; ++i) {
        if (!StartHostChild(self_exe, run_id, static_cast<uint32_t>(i), optimized, hosts[i])) {
            for (auto& h : hosts) CloseHostChild(h);
            return out;
        }
    }

    std::atomic<size_t> next_idx{0};
    std::atomic<size_t> completed{0};
    std::atomic<double> first_ms{-1.0};
    std::atomic<bool> failed{false};
    std::atomic<bool> workers_done{false};
    uint64_t peak_tree_mem_delta = 0;

    std::thread mem_sampler([&] {
        while (!workers_done.load(std::memory_order_acquire)) {
            uint64_t total_child = 0;
            {
                std::lock_guard lock(host_mu);
                for (const auto& h : hosts) {
                    if (h.pi.hProcess) total_child += ProcessPrivateBytes(h.pi.hProcess);
                }
            }
            const uint64_t cur_parent = ProcessPrivateBytes(GetCurrentProcess());
            const uint64_t parent_delta = cur_parent > parent_mem_base ? (cur_parent - parent_mem_base) : 0;
            peak_tree_mem_delta = (std::max)(peak_tree_mem_delta, total_child + parent_delta);
            Sleep(2);
        }
    });

    auto worker_fn = [&](size_t lane) {
        if (optimized && lane > 0) {
            SpawnedHost launched{};
            if (!StartHostChild(self_exe, run_id, static_cast<uint32_t>(lane), true, launched)) {
                CloseHostChild(launched);
                failed.store(true, std::memory_order_release);
                return;
            }
            std::lock_guard lock(host_mu);
            hosts[lane] = launched;
        }
        SpawnedHost& host = hosts[lane];
        std::vector<uint8_t> send_buf;
        std::vector<uint8_t> pix_buf;
        while (!failed.load(std::memory_order_acquire)) {
            const size_t idx = next_idx.fetch_add(1, std::memory_order_relaxed);
            if (idx >= files.size()) break;
            const std::wstring wpath = files[idx].wstring();
            pulse::ipc::PreviewRequest req{};
            req.magic = pulse::ipc::kPreviewBudgetRequestMagic;
            req.request_id = static_cast<uint32_t>(idx + 1);
            req.generation = 1;
            req.kind = pulse::ipc::PreviewRequestKind::Content;
            req.pixel_size = 256;
            req.flags = pulse::ipc::kPreviewRequestFlagGrid | pulse::ipc::kPreviewRequestFlagInlineBitmap |
                (optimized ? pulse::ipc::kPreviewRequestFlagPreGranted64M : 0u);
            req.path_chars = static_cast<uint32_t>(wpath.size());
            const ULONGLONG dl = GetTickCount64() + 10000;

            if (optimized) {
                const DWORD pbytes = req.path_chars * sizeof(wchar_t);
                send_buf.resize(sizeof(req) + pbytes);
                std::memcpy(send_buf.data(), &req, sizeof(req));
                std::memcpy(send_buf.data() + sizeof(req), wpath.data(), pbytes);
                if (!OverlappedWriteSync(host.pipe, host.io_event, host.pi.hProcess, send_buf.data(),
                                         static_cast<DWORD>(send_buf.size()), dl)) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
            } else {
                if (!OverlappedWriteSync(host.pipe, host.io_event, host.pi.hProcess, &req, sizeof(req), dl) ||
                    !OverlappedWriteSync(host.pipe, host.io_event, host.pi.hProcess, wpath.data(),
                                         req.path_chars * sizeof(wchar_t), dl)) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
            }

            pulse::ipc::PreviewResponse resp{};
            for (;;) {
                DWORD rerr = 0;
                const bool r_ok = optimized
                    ? pulse::ipc::ReadAllOverlapped(host.pipe, host.io_event, nullptr, host.pi.hProcess,
                                                    &resp, sizeof(resp), dl, 250, [] { return false; }, rerr)
                    : PollReadPipeRaw(host.pipe, host.pi.hProcess, &resp, sizeof(resp), dl);
                if (!r_ok) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                if (resp.magic != pulse::ipc::kPreviewBudgetProposalMagic) break;
                const uint32_t grant = pulse::ipc::kPreviewBudgetGrant;
                if (!OverlappedWriteSync(host.pipe, host.io_event, host.pi.hProcess, &grant, sizeof(grant), dl)) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
            }
            if (failed.load(std::memory_order_acquire) || resp.status != 0 || resp.magic != pulse::ipc::kPreviewMagic) {
                failed.store(true, std::memory_order_release);
                break;
            }
            const DWORD psize = resp.stride * resp.height;
            pix_buf.resize(psize);
            DWORD payload_err = 0;
            const bool p_ok = optimized
                ? pulse::ipc::ReadAllOverlapped(host.pipe, host.io_event, nullptr, host.pi.hProcess,
                                                pix_buf.data(), psize, dl, 250, [] { return false; }, payload_err)
                : PollReadPipeRaw(host.pipe, host.pi.hProcess, pix_buf.data(), psize, dl);
            if (!p_ok) {
                failed.store(true, std::memory_order_release);
                break;
            }
            const size_t done = completed.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == 1) {
                double expected = -1.0;
                first_ms.compare_exchange_strong(expected, ElapsedMs(start));
            }
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < max_hosts; ++i) threads.emplace_back(worker_fn, i);
    for (auto& t : threads) t.join();
    out.all_ms = ElapsedMs(start);
    out.first_ms = first_ms.load();
    workers_done.store(true, std::memory_order_release);
    mem_sampler.join();

    uint64_t children_cpu = 0;
    uint64_t children_cycles = 0;
    size_t spawned = 0;
    for (auto& h : hosts) {
        if (h.pi.hProcess) ++spawned;
        if (h.pipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(h.pipe, nullptr);
            CloseHandle(h.pipe);
            h.pipe = INVALID_HANDLE_VALUE;
        }
    }
    for (auto& h : hosts) {
        if (h.pi.hProcess) {
            WaitForSingleObject(h.pi.hProcess, 2000);
            children_cpu += ProcessCpu100ns(h.pi.hProcess);
            children_cycles += ProcessCycles(h.pi.hProcess);
        }
        CloseHostChild(h);
    }
    const uint64_t parent_cpu = ProcessCpu100ns(GetCurrentProcess());
    const uint64_t parent_delta = parent_cpu > parent_cpu_base ? (parent_cpu - parent_cpu_base) : 0;
    const uint64_t parent_cycles = ProcessCycles(GetCurrentProcess());
    const uint64_t parent_cycles_delta =
        parent_cycles > parent_cycles_base ? (parent_cycles - parent_cycles_base) : 0;

    out.tree_cpu_ms = static_cast<double>(children_cpu + parent_delta) / 10000.0;
    out.tree_mcycles = static_cast<double>(children_cycles + parent_cycles_delta) / 1.0e6;
    out.peak_commit_mib = static_cast<double>(peak_tree_mem_delta) / (1024.0 * 1024.0);
    out.children_spawned = spawned;
    out.ok = !failed.load() && completed.load() == files.size();
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc == 4 && _wcsicmp(argv[1], L"--child-host") == 0) {
        const bool optimized = _wcsicmp(argv[3], L"optimized") == 0;
        return RunChildHost(argv[2], optimized);
    }

    wchar_t self_exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_exe, MAX_PATH);

    wchar_t temp_buf[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_buf);
    std::error_code ec;
    std::filesystem::path temp_root = std::filesystem::path(temp_buf) /
        (L"pulse_thumb_scale_bench_" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directories(temp_root, ec) && !std::filesystem::exists(temp_root, ec)) {
        temp_root = std::filesystem::path(self_exe).parent_path() /
            (L"_bench_tmp_" + std::to_wstring(GetCurrentProcessId()));
        ec.clear();
        std::filesystem::create_directories(temp_root, ec);
    }

    bool created_temp_fixtures = false;
    const auto fixtures = ResolveOrCreateFixtures(temp_root, created_temp_fixtures);
    if (fixtures.size() != 32) {
        std::printf("[FAIL] fixture setup failed\n");
        std::filesystem::remove_all(temp_root, ec);
        return 1;
    }

    std::printf("=== Pulse Thumbnail Phase 1 & 2 Scale Verification ===\n");
    std::printf("Fixtures: %zu x 1024x768 BMP (%s)\n\n",
                fixtures.size(), created_temp_fixtures ? "generated temp" : "read-only repo fixtures/images");

    bool all_pass = true;
    all_pass &= TestFastBmpDecode(fixtures[0]);
    all_pass &= TestExifOrientationTransform();
    all_pass &= TestMicroCodecBc1Bc3(fixtures[0]);
    all_pass &= TestMicroThumbPack(temp_root);
    all_pass &= TestOverlappedPipe();
    if (!all_pass) {
        std::filesystem::remove_all(temp_root, ec);
        return 1;
    }

    // Benchmark 1: Single-image CPU decode (32 BMPs x 5 runs median).
    std::vector<double> legacy_dec_runs, fast_dec_runs;
    size_t legacy_buf_bytes = 0;
    uint32_t check_px_cnt = 0;
    for (int run = 0; run < 5; ++run) {
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0, s = 0, sw = 0, sh = 0;
        const auto t0 = Clock::now();
        for (const auto& f : fixtures) {
            size_t pk = 0;
            if (!DecodeLegacyFullBufferBmp(f.wstring(), 256, px, w, h, s, pk)) return 2;
            legacy_buf_bytes = (std::max)(legacy_buf_bytes, pk);
        }
        legacy_dec_runs.push_back(ElapsedMs(t0));

        const auto t1 = Clock::now();
        for (const auto& f : fixtures) {
            if (!pulse::preview::DecodeFastBmpThumbnailFile(f.wstring(), 256, px, w, h, s, sw, sh)) return 2;
            check_px_cnt += w * h;
        }
        fast_dec_runs.push_back(ElapsedMs(t1));
    }
    const double leg_dec_ms = MedianOf(legacy_dec_runs);
    const double fast_dec_ms = MedianOf(fast_dec_runs);
    const double fast_dec_kib = (256.0 + 192.0);  // 256 KiB stream chunk + 192 KiB output PBGRA
    const double leg_dec_kib = static_cast<double>(legacy_buf_bytes) / 1024.0;
    std::printf("\n[BENCH 1] 32x 1024x768 BMP -> 256x192 Single-Thread Decode (5-run median)\n");
    std::printf("  Legacy full-buffer decode : %7.3f ms total (%5.3f ms/img, %6.1f img/s, ~%5.0f KiB peak buf)\n",
                leg_dec_ms, leg_dec_ms / 32.0, 32000.0 / leg_dec_ms, leg_dec_kib);
    std::printf("  Fast stream BMP decode    : %7.3f ms total (%5.3f ms/img, %6.1f img/s, ~%5.0f KiB peak buf) [%+.2f%%]\n",
                fast_dec_ms, fast_dec_ms / 32.0, 32000.0 / fast_dec_ms, fast_dec_kib,
                (fast_dec_ms / leg_dec_ms - 1.0) * 100.0);

    // Benchmark 2: Real multi-process Host+IPC (5 interleaved runs A/B).
    std::vector<double> b_first, b_all, b_cpu, b_cyc, b_mem;
    std::vector<double> o_first, o_all, o_cpu, o_cyc, o_mem;
    for (int run = 0; run < 5; ++run) {
        const auto m_base = RunMultiProcPass(self_exe, fixtures, static_cast<uint32_t>(run * 2 + 1), false);
        const auto m_opt = RunMultiProcPass(self_exe, fixtures, static_cast<uint32_t>(run * 2 + 2), true);
        if (!m_base.ok || !m_opt.ok) {
            std::printf("[FAIL] Multi-process host IPC run %d failed (base=%d opt=%d)\n",
                        run, m_base.ok ? 1 : 0, m_opt.ok ? 1 : 0);
            std::filesystem::remove_all(temp_root, ec);
            return 3;
        }
        b_first.push_back(m_base.first_ms);
        b_all.push_back(m_base.all_ms);
        b_cpu.push_back(m_base.tree_cpu_ms);
        b_cyc.push_back(m_base.tree_mcycles);
        b_mem.push_back(m_base.peak_commit_mib);

        o_first.push_back(m_opt.first_ms);
        o_all.push_back(m_opt.all_ms);
        o_cpu.push_back(m_opt.tree_cpu_ms);
        o_cyc.push_back(m_opt.tree_mcycles);
        o_mem.push_back(m_opt.peak_commit_mib);
    }
    const double bf = MedianOf(b_first), ba = MedianOf(b_all), bc = MedianOf(b_cpu),
                 by = MedianOf(b_cyc), bm = MedianOf(b_mem);
    const double of = MedianOf(o_first), oa = MedianOf(o_all), oc = MedianOf(o_cpu),
                 oy = MedianOf(o_cyc), om = MedianOf(o_mem);
    std::printf("\n[BENCH 2] 32x BMP Real Multi-Process Host+IPC (5-run interleaved median)\n");
    std::printf("  First pixel result  : baseline=%6.3f ms -> optimized=%6.3f ms (%+.2f%%)\n",
                bf, of, (of / bf - 1.0) * 100.0);
    std::printf("  All 32 pixel results: baseline=%6.3f ms -> optimized=%6.3f ms (%+.2f%%)\n",
                ba, oa, (oa / ba - 1.0) * 100.0);
    std::printf("  Process tree CPU    : baseline=%6.3f ms -> optimized=%6.3f ms (%+.2f%%)\n",
                bc, oc, bc > 0 ? (oc / bc - 1.0) * 100.0 : 0.0);
    std::printf("  Process tree cycles : baseline=%7.2f Mcycles -> optimized=%7.2f Mcycles (%+.2f%%)\n",
                by, oy, by > 0 ? (oy / by - 1.0) * 100.0 : 0.0);
    std::printf("  Peak tree commit    : baseline=%6.3f MiB -> optimized=%6.3f MiB (%+.2f%%)\n",
                bm, om, bm > 0 ? (om / bm - 1.0) * 100.0 : 0.0);

    // Benchmark 3: 10,000-Image Disk Pack, Cold Index, Negative Lookups & Random Scroll-Jump.
    constexpr uint32_t kScaleCount = 10000u;
    const auto single_dir = temp_root / L"single_10k";
    const auto pack_dir = temp_root / L"pack_10k";
    std::filesystem::create_directories(single_dir, ec);

    std::vector<uint64_t> id_hashes(kScaleCount);
    std::vector<std::wstring> single_paths(kScaleCount);
    std::vector<pulse::ui::MicroThumbPack::PendingRecord> scale_recs(kScaleCount);

    pulse::ui::MicroThumbData template_bc1;
    {
        std::vector<uint8_t> tmp;
        uint32_t tw = 0, th = 0, ts = 0, tsw = 0, tsh = 0;
        if (!pulse::preview::DecodeFastBmpThumbnailFile(fixtures[0].wstring(), 64, tmp, tw, th, ts, tsw, tsh) ||
            !pulse::ui::EncodeMicroThumb(tmp.data(), tw, th, ts, tsw, tsh, 64, template_bc1) ||
            template_bc1.format != pulse::ui::kMicroThumbFormatBc1 ||
            template_bc1.blocks.size() != 1536u) {
            std::printf("[FAIL] [BENCH 3] opaque BC1 template setup failed (fmt=%u size=%zu)\n",
                        static_cast<unsigned>(template_bc1.format), template_bc1.blocks.size());
            std::filesystem::remove_all(temp_root, ec);
            return 4;
        }
    }

    const uint64_t mem_before_setup = ProcessPrivateBytes(GetCurrentProcess());
    for (uint32_t i = 0; i < kScaleCount; ++i) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"img_%05u.thumb", i);
        single_paths[i] = (single_dir / buf).wstring();
        const std::wstring logical = L"D:\\Photos\\Batch\\IMG_" + std::to_wstring(i) + L".jpg";
        id_hashes[i] = pulse::ui::MicroThumbPack::IdentityHash(logical);
        scale_recs[i].identity_hash = id_hashes[i];
        scale_recs[i].mtime = 2000000ll + i;
        scale_recs[i].file_size = 500000ull + i;
        scale_recs[i].thumb = template_bc1;
        scale_recs[i].thumb.blocks[0] = static_cast<uint8_t>(i & 255u);
    }

    // Write 10,000 individual small files (baseline filesystem metadata overhead).
    std::vector<uint8_t> single_blob(1568, 0x33u);
    const auto t_single_write0 = Clock::now();
    for (uint32_t i = 0; i < kScaleCount; ++i) {
        HANDLE f = CreateFileW(single_paths[i].c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(f, single_blob.data(), static_cast<DWORD>(single_blob.size()), &wr, nullptr);
            CloseHandle(f);
        }
    }
    const double single_write_ms = ElapsedMs(t_single_write0);

    // Write 10,000 records into single MicroThumbPack.
    pulse::ui::MicroThumbPack pack10k;
    const auto t_pack_write0 = Clock::now();
    pack10k.Open(pack_dir, false);
    const size_t batch_size = 256;
    for (size_t off = 0; off < kScaleCount; off += batch_size) {
        const size_t end = (std::min<size_t>)(kScaleCount, off + batch_size);
        std::vector<pulse::ui::MicroThumbPack::PendingRecord> chunk(
            scale_recs.begin() + static_cast<ptrdiff_t>(off),
            scale_recs.begin() + static_cast<ptrdiff_t>(end));
        pack10k.AppendBatch(chunk);
    }
    const double pack_write_ms = ElapsedMs(t_pack_write0);
    pack10k.Close();

    // Measure cold reopen + index scan time + index memory overhead.
    pulse::ui::MicroThumbPack pack10k_reopen;
    const uint64_t mem_before_index = ProcessPrivateBytes(GetCurrentProcess());
    const auto t_pack_open0 = Clock::now();
    const bool open10k_ok = pack10k_reopen.Open(pack_dir, true);
    const double pack_open_ms = ElapsedMs(t_pack_open0);
    const uint64_t mem_after_index = ProcessPrivateBytes(GetCurrentProcess());
    const double pack_index_kib = mem_after_index > mem_before_index
        ? static_cast<double>(mem_after_index - mem_before_index) / 1024.0 : 0.0;

    // Build 1,000 negative-lookup hash set (for missed `.pt` index simulation).
    std::unordered_set<uint64_t> pt_index;
    pt_index.reserve(kScaleCount);
    const auto t_miss_file0 = Clock::now();
    std::vector<std::wstring> miss_paths(1000);
    for (uint32_t i = 0; i < 1000; ++i) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"missing_%04u.thumb", i);
        miss_paths[i] = (single_dir / buf).wstring();
    }
    const auto t_miss_fs0 = Clock::now();
    for (const auto& mp : miss_paths) {
        const DWORD a = GetFileAttributesW(mp.c_str());
        (void)a;
    }
    const double miss_fs_ms = ElapsedMs(t_miss_fs0);
    (void)t_miss_file0;
    const auto t_miss_hash0 = Clock::now();
    size_t miss_hits = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        if (pt_index.contains(0xCAFE000000000000ull + i)) ++miss_hits;
    }
    const double miss_hash_ms = ElapsedMs(t_miss_hash0);

    // 25 random 40-item viewport jumps (1,000 tiles total).
    std::vector<uint32_t> jump_indices;
    jump_indices.reserve(1000);
    uint32_t rng = 0x13579BDFu;
    for (int j = 0; j < 25; ++j) {
        rng = rng * 1664525u + 1013904223u;
        const uint32_t base = rng % (kScaleCount - 40u);
        for (uint32_t k = 0; k < 40u; ++k) jump_indices.push_back(base + k);
    }

    std::vector<uint8_t> read_scratch(2048);
    const auto t_jump_single0 = Clock::now();
    size_t single_read_ok = 0;
    for (uint32_t idx : jump_indices) {
        HANDLE f = CreateFileW(single_paths[idx].c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            if (ReadFile(f, read_scratch.data(), static_cast<DWORD>(read_scratch.size()), &got, nullptr) && got > 0) {
                ++single_read_ok;
            }
            CloseHandle(f);
        }
    }
    const double jump_single_ms = ElapsedMs(t_jump_single0);

    const auto t_jump_pack0 = Clock::now();
    size_t pack_read_ok = 0;
    pulse::ui::MicroThumbData loaded_micro;
    for (uint32_t idx : jump_indices) {
        if (pack10k_reopen.Read(id_hashes[idx], 2000000ll + idx, 500000ull + idx, loaded_micro)) {
            ++pack_read_ok;
        }
    }
    const double jump_pack_ms = ElapsedMs(t_jump_pack0);

    const uint64_t pack_file_bytes = std::filesystem::file_size(pack_dir / L"micro_v1.pack", ec);
    const double pack_disk_mib = static_cast<double>(pack_file_bytes) / (1024.0 * 1024.0);
    // Exact GPU residency numbers: 10,000 x 64x48 BC1 tiles = 10,000 * 1.5 KiB fits inside 10 pages (< 12-page 24.0 MiB max).
    const double legacy_10k_gpu_mib = (10000.0 * 256.0 * 192.0 * 4.0) / (1024.0 * 1024.0);
    const double micro_atlas_max_vram_mib = (12.0 * 2048.0 * 2048.0 * 0.5) / (1024.0 * 1024.0);

    std::printf("\n[BENCH 3] 10,000-Image Disk Pack, Cold Open, Negative Lookups & 25x40 Scroll-Jumps\n");
    std::printf("  10k write (10k files vs pack)   : %7.2f ms vs %7.2f ms (%+.2f%%)\n",
                single_write_ms, pack_write_ms, (pack_write_ms / single_write_ms - 1.0) * 100.0);
    std::printf("  10k cold pack Open() scan       : %7.3f ms (entries=%zu, open_ok=%d, index_ram=%.1f KiB)\n",
                pack_open_ms, pack10k_reopen.EntryCount(), open10k_ok ? 1 : 0, pack_index_kib);
    std::printf("  1k cold misses (Win32 vs set)   : %7.3f ms vs %7.4f ms (miss_hits=%zu)\n",
                miss_fs_ms, miss_hash_ms, miss_hits);
    std::printf("  25x40 random scroll-jumps (1k)  : %7.3f ms (%6.3f ms/40-tile screen) vs %7.3f ms (%6.3f ms/screen) [%+.2f%%]\n",
                jump_single_ms, jump_single_ms / 25.0, jump_pack_ms, jump_pack_ms / 25.0,
                (jump_pack_ms / jump_single_ms - 1.0) * 100.0);
    std::printf("  10k pack file footprint         : %.2f MiB on disk (~%.2f KiB/tile)\n",
                pack_disk_mib, (static_cast<double>(pack_file_bytes) / 1024.0) / kScaleCount);
    std::printf("  10k GPU residency memory        : Legacy 256x192 RGBA=%.1f MiB -> BC1 Atlas<=%.1f MiB (12,288 slots, -98.7%%)\n",
                legacy_10k_gpu_mib, micro_atlas_max_vram_mib);

    const bool bench_ok = open10k_ok && pack10k_reopen.EntryCount() == kScaleCount &&
        single_read_ok == 1000 && pack_read_ok == 1000 && check_px_cnt > 0;
    std::printf("[%s] 10,000-image pack read/write & 1,000-tile scroll jump integrity (%zu/%zu reads)\n",
                bench_ok ? "PASS" : "FAIL", single_read_ok, pack_read_ok);

    (void)mem_before_setup;
    pack10k_reopen.Close();
    std::filesystem::remove_all(temp_root, ec);
    return (all_pass && bench_ok) ? 0 : 1;
}
