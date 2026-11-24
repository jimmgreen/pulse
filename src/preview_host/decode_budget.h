#pragma once
#include "../ipc/preview_protocol.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace pulse::preview {
struct DecodeBudgetDenied {};
struct DecodeBudgetSession {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    uint32_t request = 0;
    uint64_t generation = 0, reserved = 0;
    bool unknown = false;
};
inline thread_local DecodeBudgetSession* decode_budget_session = nullptr;
class DecodeBudgetScope {
public:
    explicit DecodeBudgetScope(DecodeBudgetSession* session) : previous_(decode_budget_session) { decode_budget_session = session; }
    ~DecodeBudgetScope() { decode_budget_session = previous_; }
private:
    DecodeBudgetSession* previous_;
};
inline uint64_t DecodeEstimate(uint64_t pixels, uint64_t bytes_per_pixel, uint32_t edge) {
    constexpr uint64_t overhead = 32ull << 20;
    const uint64_t output = uint64_t((std::min)(edge, 1024u)) * (std::min)(edge, 1024u) * 16;
    if (pixels > (UINT64_MAX - overhead - output) / bytes_per_pixel) return UINT64_MAX;
    return overhead + output + pixels * bytes_per_pixel;
}
inline void RequireDecodeMemory(uint64_t bytes, bool unknown = false) {
    auto* session = decode_budget_session;
    if (!session || (bytes <= session->reserved && (!unknown || session->unknown))) return;
    ipc::PreviewResponse proposal{};
    proposal.magic = ipc::kPreviewBudgetProposalMagic; // PDB1: versioned budget proposal, same fixed header size.
    proposal.request_id = session->request; proposal.generation = session->generation;
    proposal.width = static_cast<uint32_t>(bytes);
    proposal.height = static_cast<uint32_t>(bytes >> 32);
    proposal.flags = unknown ? 1u : 0u;
    uint32_t grant = 0;
    if (!ipc::WriteAll(session->pipe, &proposal, sizeof(proposal)) ||
        !ipc::ReadAll(session->pipe, &grant, sizeof(grant)) || grant != ipc::kPreviewBudgetGrant)
        throw DecodeBudgetDenied{};
    session->reserved = (std::max)(session->reserved, bytes);
    session->unknown |= unknown;
}
// Only SOF is needed. Walk segment headers in buffered chunks without reading compressed scan data.
inline bool JpegInspectFile(const std::wstring& path, bool& is_progressive,
                            bool& needs_coefficients) {
    is_progressive = true;
    needs_coefficients = true;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{file};

    uint8_t buf[4096]{};
    DWORD len = 0;
    DWORD pos = 0;
    auto ensure_byte = [&](uint8_t& value) -> bool {
        if (pos >= len) {
            pos = 0;
            len = 0;
            if (!ReadFile(file, buf, sizeof(buf), &len, nullptr) || len == 0) return false;
        }
        value = buf[pos++];
        return true;
    };
    auto skip_bytes = [&](uint32_t count) -> bool {
        const DWORD avail = len - pos;
        if (count <= avail) {
            pos += count;
            return true;
        }
        const uint32_t remaining = count - avail;
        pos = len = 0;
        LARGE_INTEGER jump{};
        jump.QuadPart = remaining;
        return SetFilePointerEx(file, jump, nullptr, FILE_CURRENT) != FALSE;
    };

    uint8_t a = 0, b = 0;
    if (!ensure_byte(a) || !ensure_byte(b) || a != 255 || b != 216) return false;
    for (unsigned segment = 0; segment < 4096; ++segment) {
        if (!ensure_byte(a) || a != 255) return false;
        do { if (!ensure_byte(b)) return false; } while (b == 255);
        if (b == 0xc0 || b == 0xc1) {
            is_progressive = false;
            needs_coefficients = false;
            return true;
        }
        if (b == 0xc2) {
            is_progressive = true;
            needs_coefficients = true;
            return true;
        }
        if (b == 0xda || b == 0xd9) return true;
        if (b == 0x01 || (b >= 0xd0 && b <= 0xd7)) continue;
        if (!ensure_byte(a)) return false;
        uint8_t low = 0;
        if (!ensure_byte(low)) return false;
        const uint32_t length = uint32_t(a) * 256u + low;
        if (length < 2) return false;
        if (!skip_bytes(length - 2)) return false;
    }
    return false;
}

inline bool JpegNeedsCoefficients(const std::wstring& path) {
    bool is_progressive = true;
    bool needs_coefficients = true;
    JpegInspectFile(path, is_progressive, needs_coefficients);
    return needs_coefficients;
}

inline uint64_t EstimateJpegMemory(IWICBitmapFrameDecode* frame, const std::wstring& path,
                                   uint32_t width, uint32_t height, uint32_t edge,
                                   bool dc_only = false,
                                   const bool* prechecked_needs_coefficients = nullptr) {
    Microsoft::WRL::ComPtr<IWICBitmapSourceTransform> transform;
    const auto longest = (std::max)(width, height);
    UINT native_w = (std::max)(1u, static_cast<UINT>(uint64_t(width) * edge / longest));
    UINT native_h = (std::max)(1u, static_cast<UINT>(uint64_t(height) * edge / longest));
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&transform))) ||
        FAILED(transform->GetClosestSize(&native_w, &native_h))) return DecodeEstimate(uint64_t(width) * height, 16, edge);
    const uint64_t native = DecodeEstimate(uint64_t(native_w) * native_h, 16, edge);
    if (uint64_t(width) * height > UINT64_MAX / 8) return UINT64_MAX;
    const bool needs_coeffs = dc_only ? false
        : (prechecked_needs_coefficients ? *prechecked_needs_coefficients : JpegNeedsCoefficients(path));
    const uint64_t coefficients = dc_only ? ((uint64_t(width) + 7) / 8) * ((uint64_t(height) + 7) / 8) * 32 :
        needs_coeffs ? uint64_t(width) * height * 8 : uint64_t(width) * 128;
    return coefficients > UINT64_MAX - native ? UINT64_MAX : native + coefficients;
}
inline uint64_t PngRowEstimate(const std::wstring& path, uint32_t edge) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uint8_t header[33]{}; DWORD got = 0;
    const bool read = ReadFile(file, header, sizeof(header), &got, nullptr) && got == sizeof(header);
    CloseHandle(file);
    if (!read || memcmp(header, "\x89PNG\r\n\x1a\n", 8) || memcmp(header + 12, "IHDR", 4) || header[28]) return 0;
    const uint64_t width = (uint32_t(header[16]) << 24) | (uint32_t(header[17]) << 16) |
        (uint32_t(header[18]) << 8) | header[19];
    // PNG pack's documented 64 MiB ancillary cap, zlib/filter rows and the
    // integer downscaler/output/IPC copies. File-backed input is not commit.
    return (64ull << 20) + width * 64 + uint64_t(edge) * edge * 64;
}
inline uint64_t PsdDecodeEstimate(const std::wstring& path, uint32_t edge) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uint8_t header[26]{}; DWORD got = 0;
    const bool read = ReadFile(file, header, sizeof(header), &got, nullptr) && got == sizeof(header);
    CloseHandle(file);
    if (!read || memcmp(header, "8BPS", 4) != 0) return 0;
    const uint32_t version = (uint32_t(header[4]) << 8) | header[5];
    if (version != 1 && version != 2) return 0;
    const uint32_t channels = (uint32_t(header[12]) << 8) | header[13];
    const uint32_t height = (uint32_t(header[14]) << 24) | (uint32_t(header[15]) << 16) |
        (uint32_t(header[16]) << 8) | header[17];
    const uint32_t width = (uint32_t(header[18]) << 24) | (uint32_t(header[19]) << 16) |
        (uint32_t(header[20]) << 8) | header[21];
    const uint32_t depth = (uint32_t(header[22]) << 8) | header[23];
    const uint32_t max_dim = version == 2 ? 300000u : 30000u;
    if (channels < 1 || channels > 56 || !width || !height || width > max_dim || height > max_dim ||
        (depth != 1 && depth != 8 && depth != 16 && depth != 32))
        return 0;
    // psd_raster streams channel scanlines into a bounded box-average grid
    // instead of materialising the full document canvas via WIC.
    const uint32_t capped = (std::min)((std::max)(edge, 16u), (std::min)((std::max)(width, height), 4096u));
    const uint64_t count_work = (std::min)(16ull << 20, uint64_t(channels) * height * (version == 2 ? 8u : 4u));
    const uint64_t acc_work = (std::min)(128ull << 20, uint64_t(capped) * capped * 32u);
    const uint64_t row_work = uint64_t(width) * (((depth + 7u) / 8u) * 8u + 16u);
    return DecodeEstimate(0, 1, edge) + (8ull << 20) + count_work + acc_work + row_work;
}
inline void RequireUnknownImageMemory(const std::wstring& path, uint32_t edge, bool allow_png_rows = false) {
    if (!decode_budget_session) return;
    if (allow_png_rows) if (const auto rows = PngRowEstimate(path, edge)) { RequireDecodeMemory(rows); return; }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h)
        RequireDecodeMemory(DecodeEstimate(uint64_t(w) * h, 20, edge));
    else RequireDecodeMemory(768ull << 20, true);
}
}
