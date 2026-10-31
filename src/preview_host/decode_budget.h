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
// Only SOF is needed. Walk segment headers without reading compressed scan data.
inline bool JpegNeedsCoefficients(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true;
    struct Close { HANDLE h; ~Close() { CloseHandle(h); } } close{file};
    auto byte = [&](uint8_t& value) { DWORD got = 0; return ReadFile(file, &value, 1, &got, nullptr) && got == 1; };
    uint8_t a = 0, b = 0;
    if (!byte(a) || !byte(b) || a != 255 || b != 216) return true;
    for (unsigned segment = 0; segment < 4096; ++segment) {
        if (!byte(a) || a != 255) return true;
        do { if (!byte(b)) return true; } while (b == 255);
        if (b == 0xc0 || b == 0xc1) return false;
        if (b == 0xc2 || b == 0xda || b == 0xd9) return true;
        if (b == 0x01 || (b >= 0xd0 && b <= 0xd7)) continue;
        if (!byte(a)) return true;
        uint8_t low = 0; if (!byte(low)) return true;
        const uint32_t length = uint32_t(a) * 256 + low;
        if (length < 2) return true;
        LARGE_INTEGER jump{}; jump.QuadPart = length - 2;
        if (!SetFilePointerEx(file, jump, nullptr, FILE_CURRENT)) return true;
    }
    return true;
}
inline uint64_t EstimateJpegMemory(IWICBitmapFrameDecode* frame, const std::wstring& path,
                                   uint32_t width, uint32_t height, uint32_t edge, bool dc_only = false) {
    Microsoft::WRL::ComPtr<IWICBitmapSourceTransform> transform;
    const auto longest = (std::max)(width, height);
    UINT native_w = (std::max)(1u, static_cast<UINT>(uint64_t(width) * edge / longest));
    UINT native_h = (std::max)(1u, static_cast<UINT>(uint64_t(height) * edge / longest));
    if (FAILED(frame->QueryInterface(IID_PPV_ARGS(&transform))) ||
        FAILED(transform->GetClosestSize(&native_w, &native_h))) return DecodeEstimate(uint64_t(width) * height, 16, edge);
    const uint64_t native = DecodeEstimate(uint64_t(native_w) * native_h, 16, edge);
    if (uint64_t(width) * height > UINT64_MAX / 8) return UINT64_MAX;
    const uint64_t coefficients = dc_only ? ((uint64_t(width) + 7) / 8) * ((uint64_t(height) + 7) / 8) * 32 :
        JpegNeedsCoefficients(path) ? uint64_t(width) * height * 8 : uint64_t(width) * 128;
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
