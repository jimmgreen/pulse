#include "pbgra_blend.h"
#include "gif_decoder.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <array>

namespace pulse::preview {
using Microsoft::WRL::ComPtr;
namespace {
uint32_t U16(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8); }
struct Meta { uint32_t x, y, w, h, disposal, delay; bool transparent; };
using Color = std::array<uint8_t, 4>;
void FillBackground(std::vector<uint8_t>& canvas, uint32_t width, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, const Color& color) {
    for (uint32_t row = y; row < y + h; ++row)
        for (uint32_t col = x; col < x + w; ++col)
            std::copy(color.begin(), color.end(), canvas.begin() + (size_t(row) * width + col) * 4);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool Same(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
// Validate container bounds before allowing WIC to allocate a source raster.
bool Inspect(std::vector<uint8_t>& data, UINT& w, UINT& h,
             std::vector<Meta>& frames, uint32_t& loops, uint64_t& source_bytes, Color& background, std::wstring& error, bool first_frame_only) {
    auto bad = [&](const wchar_t* why) { error = why; return false; };
    if (data.size() < 13 || (memcmp(data.data(), "GIF87a", 6) && memcmp(data.data(), "GIF89a", 6))) return bad(L"gif-invalid");
    w = U16(data.data() + 6); h = U16(data.data() + 8);
    if (!w || !h || uint64_t(w) * h * 4 > MAXDWORD) return bad(L"gif-canvas-budget");
    size_t at = 13;
    auto take = [&](size_t n) { if (n > data.size() - at) return false; at += n; return true; };
    background = {};
    if (data[10] & 0x80) {
        const size_t colors = size_t(1) << ((data[10] & 7) + 1);
        if (!take(colors * 3)) return bad(L"gif-truncated");
        if (data[11] >= colors) return bad(L"gif-invalid-background");
        const auto* rgb = data.data() + 13 + size_t(data[11]) * 3;
        background = {rgb[2], rgb[1], rgb[0], 255};
    }
    auto blocks = [&] {
        while (at < data.size()) { const auto n = data[at++]; if (!n) return true; if (!take(n)) return false; }
        return false;
    };
    uint32_t disposal = 0, delay = 100;
    bool transparent = false;
    while (at < data.size()) {
        const auto marker = data[at++];
        if (marker == 0x3b) return !frames.empty() || bad(L"gif-no-frames");
        if (marker == 0x21) {
            if (at == data.size()) return bad(L"gif-truncated");
            const auto label = data[at++];
            if (label == 0xf9) {
                if (data.size() - at < 6 || data[at] != 4 || data[at + 5] != 0) return bad(L"gif-invalid-control");
                disposal = (data[at + 1] >> 2) & 7;
                transparent = (data[at + 1] & 1) != 0;
                if (disposal > 3) return bad(L"gif-invalid-disposal");
                delay = U16(data.data() + at + 2) * 10;
                delay = delay ? std::clamp(delay, 20u, 2000u) : 100;
                at += 6;
            } else {
                if (label == 0xff && data.size() - at >= 17 && data[at] == 11 &&
                    (!memcmp(data.data() + at + 1, "NETSCAPE2.0", 11) || !memcmp(data.data() + at + 1, "ANIMEXTS1.0", 11)) &&
                    data[at + 12] == 3 && data[at + 13] == 1) loops = U16(data.data() + at + 14);
                if (!blocks()) return bad(L"gif-truncated");
            }
        } else if (marker == 0x2c) {
            if (data.size() - at < 9) return bad(L"gif-truncated");
            Meta f{U16(data.data() + at), U16(data.data() + at + 2), U16(data.data() + at + 4),
                   U16(data.data() + at + 6), disposal, delay, transparent};
            const auto packed = data[at + 8]; at += 9;
            if (!f.w || !f.h || f.x > w || f.y > h || f.w > w - f.x || f.h > h - f.y ||
                uint64_t(f.w) * f.h * 4 > MAXDWORD) return bad(L"gif-frame-bounds");
            source_bytes += uint64_t(f.w) * f.h * 4;
            frames.push_back(f); disposal = 0; delay = 100; transparent = false;
            if ((packed & 0x80) && !take(size_t(3) << ((packed & 7) + 1))) return bad(L"gif-truncated");
            if (!take(1) || !blocks()) return bad(L"gif-truncated");
            if (first_frame_only) { data.resize(at); data.push_back(0x3b); return true; }
        } else return bad(L"gif-invalid-block");
    }
    return bad(L"gif-truncated");
}
}
struct GifDecoder::State {
    DWORD thread = GetCurrentThreadId();
    BY_HANDLE_FILE_INFORMATION identity{};
    std::wstring path;
    std::vector<uint8_t> file, canvas, saved;
    std::vector<Meta> frames;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    UINT w = 0, h = 0;
    uint32_t next = 0, loops = 0;
    uint64_t source_bytes = 0;
    Color background{};
    GifStats stats;
    void Clear() {
        decoder.Reset(); stream.Reset(); factory.Reset();
        std::vector<uint8_t>().swap(file); std::vector<uint8_t>().swap(canvas); std::vector<uint8_t>().swap(saved);
        std::vector<Meta>().swap(frames); path.clear(); next = 0; loops = 0; source_bytes = 0; stats.reserved_bytes = 0;
    }
};
GifDecoder::GifDecoder() : state_(std::make_unique<State>()) {}
GifDecoder::~GifDecoder() = default;
void GifDecoder::Reset() { state_->Clear(); }
GifStats GifDecoder::Stats() const {
    auto stats = state_->stats;
    stats.backup_capacity_bytes = state_->saved.capacity();
    return stats;
}
bool GifDecoder::Decode(const std::wstring& path, UINT edge, uint32_t index, GifFrame& result, HANDLE cancel, bool first_frame_only) {
    auto& s = *state_; result = {};
    if (s.thread != GetCurrentThreadId()) { result.error = L"gif-wrong-thread"; return false; }
    auto fail = [&](const wchar_t* error) { result.error = error; result.pixels.clear(); s.Clear(); return false; };
    auto interrupted = [&]() -> const wchar_t* {
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return L"gif-cancelled";
        return nullptr;
    };
    try {
        if (const auto error = interrupted()) return fail(error);
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        BY_HANDLE_FILE_INFORMATION identity{};
        if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &identity)) return fail(L"gif-open-failed");
        const uint64_t file_bytes = (uint64_t(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow;
        const uint64_t bytes = first_frame_only ? (std::min)(file_bytes, 16ull * 1024 * 1024) : file_bytes;
        if (first_frame_only) { s.Clear(); index = 0; edge = (std::min)(edge, 512u); }
        if (!bytes || bytes > MAXDWORD) return fail(L"gif-file-budget");
        if (s.path != path || !s.decoder || !Same(identity, s.identity)) {
            s.Clear();
            size_t loaded = 0;
            for (;;) {
                const size_t wanted = first_frame_only ? (std::min)(static_cast<size_t>(bytes),
                    (std::max)(size_t(65536), loaded * 2)) : static_cast<size_t>(bytes);
                s.file.resize(wanted);
                DWORD read = 0;
                const auto added = static_cast<DWORD>(wanted - loaded);
                if (!ReadFile(file.value, s.file.data() + loaded, added, &read, nullptr) || read != added)
                    return fail(L"gif-read-failed");
                loaded = wanted;
                std::wstring error;
                s.frames.clear(); s.source_bytes = 0; s.loops = 0;
                if (Inspect(s.file, s.w, s.h, s.frames, s.loops, s.source_bytes, s.background, error, first_frame_only)) break;
                if (!first_frame_only || error != L"gif-truncated" || loaded == bytes) {
                    result.error = first_frame_only && loaded < file_bytes && error == L"gif-truncated" ?
                        L"gif-thumbnail-budget" : error;
                    s.Clear(); return false;
                }
                if (const auto why = interrupted()) return fail(why);
            }
            // Canvas + disposal backup + decoded source + WIC bitmap copy + output,
            // with another canvas allowance for scaler scratch. Bound all declared
            // source rasters too, rather than assuming WIC releases previous frames.
            const uint64_t reserve = s.file.capacity() + uint64_t(s.w) * s.h * 4 * 6 + uint64_t(s.frames.size()) * sizeof(Meta) + s.source_bytes * 2;
            if (first_frame_only && reserve > 192ull * 1024 * 1024) return fail(L"gif-thumbnail-budget");
            if (const auto why = interrupted()) return fail(why);
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s.factory))) ||
                FAILED(s.factory->CreateStream(&s.stream)) ||
                FAILED(s.stream->InitializeFromMemory(s.file.data(), static_cast<DWORD>(s.file.size()))) ||
                FAILED(s.factory->CreateDecoderFromStream(s.stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &s.decoder)))
                return fail(L"gif-decoder-failed");
            UINT count = 0;
            if (FAILED(s.decoder->GetFrameCount(&count)) || count != s.frames.size()) return fail(L"gif-frame-count");
            s.identity = identity; s.path = path;
            s.canvas.assign(static_cast<size_t>(s.w) * s.h * 4, 0);
            if (!s.frames.front().transparent)
                FillBackground(s.canvas, s.w, 0, 0, s.w, s.h, s.background);
            s.stats.reserved_bytes = reserve; ++s.stats.cache_loads;
        }
        index = (std::min)(index, static_cast<uint32_t>(s.frames.size() - 1));
        result.count = static_cast<uint32_t>(s.frames.size()); result.loops = s.loops;
        result.source_width = s.w; result.source_height = s.h; result.delay = s.frames[index].delay;
        if (index + 1 < s.next) {
            FillBackground(s.canvas, s.w, 0, 0, s.w, s.h, s.frames.front().transparent ? Color{} : s.background);
            s.saved.clear(); s.next = 0; ++s.stats.resets;
        }
        for (; s.next <= index; ++s.next) {
            if (const auto error = interrupted()) return fail(error);
            if (s.next) {
                const auto& previous = s.frames[s.next - 1];
                if (previous.disposal == 2) {
                    // Transparent animations clear to transparent; opaque ones restore
                    // the logical screen background from the global color table.
                    FillBackground(s.canvas, s.w, previous.x, previous.y, previous.w, previous.h,
                        previous.transparent ? Color{} : s.background);
                } else if (previous.disposal == 3 &&
                           s.saved.size() == static_cast<size_t>(previous.w) * previous.h * 4) {
                    for (UINT y = 0; y < previous.h; ++y)
                        std::memcpy(s.canvas.data() + (static_cast<size_t>(previous.y + y) * s.w + previous.x) * 4,
                            s.saved.data() + static_cast<size_t>(y) * previous.w * 4,
                            static_cast<size_t>(previous.w) * 4);
                }
            }
            const auto& frame = s.frames[s.next];
            if (frame.disposal == 3 && !first_frame_only) {
                // This frame can only change its validated rectangle. Preserve that
                // region for disposal instead of retaining a second full canvas.
                s.saved.resize(static_cast<size_t>(frame.w) * frame.h * 4);
                for (UINT y = 0; y < frame.h; ++y)
                    std::memcpy(s.saved.data() + static_cast<size_t>(y) * frame.w * 4,
                        s.canvas.data() + (static_cast<size_t>(frame.y + y) * s.w + frame.x) * 4,
                        static_cast<size_t>(frame.w) * 4);
            } else s.saved.clear();
            ComPtr<IWICBitmapFrameDecode> decoded;
            ComPtr<IWICFormatConverter> converter;
            if (FAILED(s.decoder->GetFrame(s.next, &decoded)) || FAILED(s.factory->CreateFormatConverter(&converter)) ||
                FAILED(converter->Initialize(decoded.Get(), GUID_WICPixelFormat32bppPBGRA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return fail(L"gif-frame-decode");
            UINT w = 0, h = 0;
            if (FAILED(converter->GetSize(&w, &h)) || w != frame.w || h != frame.h) return fail(L"gif-frame-size");
            std::vector<uint8_t> raster(static_cast<size_t>(w) * h * 4);
            if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(raster.size()), raster.data()))) return fail(L"gif-frame-decode");
            ++s.stats.decoded_frames;
            for (UINT y = 0; y < h; ++y) {
                if ((y & 63) == 0) if (const auto error = interrupted()) return fail(error);
                BlendPbgraSpan(s.canvas.data() + (static_cast<size_t>(frame.y + y) * s.w + frame.x) * 4,
                    raster.data() + static_cast<size_t>(y) * w * 4, w);
            }
        }
        if (const auto error = interrupted()) return fail(error);
        edge = (std::max)(1u, edge);
        const double scale = (std::min)(1.0, double(edge) / (std::max)(s.w, s.h));
        result.width = (std::max)(1u, static_cast<UINT>(s.w * scale + 0.5));
        result.height = (std::max)(1u, static_cast<UINT>(s.h * scale + 0.5)); result.stride = result.width * 4;
        if (scale == 1.0) result.pixels = s.canvas;
        else {
            ComPtr<IWICBitmap> bitmap; ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(s.factory->CreateBitmapFromMemory(s.w, s.h, GUID_WICPixelFormat32bppPBGRA, s.w * 4,
                    static_cast<UINT>(s.canvas.size()), s.canvas.data(), &bitmap)) ||
                FAILED(s.factory->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(bitmap.Get(), result.width, result.height, WICBitmapInterpolationModeFant))) return fail(L"gif-scale-failed");
            result.pixels.resize(static_cast<size_t>(result.stride) * result.height);
            if (FAILED(scaler->CopyPixels(nullptr, result.stride, static_cast<UINT>(result.pixels.size()), result.pixels.data()))) return fail(L"gif-scale-failed");
        }
        if (const auto error = interrupted()) { result.pixels.clear(); return fail(error); }
        return true;
    } catch (const std::bad_alloc&) { return fail(L"gif-memory-budget"); }
}
namespace { thread_local std::unique_ptr<GifDecoder> cached; }
bool DecodeGifCached(const std::wstring& path, UINT edge, uint32_t index, GifFrame& result) {
    if (!cached) cached = std::make_unique<GifDecoder>();
    return cached->Decode(path, edge, index, result);
}
bool DecodeGifFirstFrameThumbnail(const std::wstring& path, UINT edge, GifFrame& result) {
    GifDecoder decoder;
    return decoder.Decode(path, edge, 0, result, nullptr, true);
}
void ResetGifDecodeCache() { cached.reset(); }
}
