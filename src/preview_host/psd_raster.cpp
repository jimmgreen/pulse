// psd_raster.cpp — see psd_raster.h. Format reference: Adobe Photoshop File
// Formats Specification (header, color mode data, image resources, layer and
// mask information, image data).
#include "psd_raster.h"
#include "icc_profile.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pulse::preview {
namespace {

using Microsoft::WRL::ComPtr;

constexpr ULONGLONG kDecodeBudgetMs = 4000;   // composite streaming deadline
// First-pass grid thumbnails decode the composite up to this size. Larger
// documents show their embedded JPEG until the queued composite refinement.
constexpr uint64_t kGridCompositePixels = 32ull * 1000 * 1000;
// Documents up to this size always use the composite (a few milliseconds).
constexpr uint64_t kGridSmallPixels = 1000ull * 1000;
// Embedded ICC profiles beyond this are ignored (press CMYK runs ~0.5 MB).
constexpr uint32_t kIccBudget = 4u * 1024u * 1024u;
constexpr size_t kReadChunk = 1u << 20;

// Sequential big-endian reader over a file with a 1 MB window.
class Reader {
public:
    explicit Reader(const std::wstring& path) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER size{};
        if (file_ != INVALID_HANDLE_VALUE && GetFileSizeEx(file_, &size)) size_ = size.QuadPart;
    }
    ~Reader() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool ok() const { return file_ != INVALID_HANDLE_VALUE && !failed_; }
    uint64_t size() const { return size_; }
    uint64_t tell() const { return base_ + pos_; }

    bool Seek(uint64_t offset) {
        if (offset > size_) return Fail();
        if (offset >= base_ && offset <= base_ + len_) { pos_ = static_cast<size_t>(offset - base_); return true; }
        base_ = offset; pos_ = len_ = 0;
        return true;
    }
    bool Skip(uint64_t n) { return Seek(tell() + n); }

    bool Read(void* out, size_t n) {
        auto* dst = static_cast<unsigned char*>(out);
        while (n > 0) {
            if (pos_ == len_ && !Fill()) return Fail();
            const size_t take = (std::min)(n, len_ - pos_);
            std::memcpy(dst, buffer_.data() + pos_, take);
            pos_ += take; dst += take; n -= take;
        }
        return true;
    }
    // Pointer into the window when n bytes are available contiguously.
    const unsigned char* Peek(size_t n) {
        if (n > kReadChunk) return nullptr;
        if (len_ - pos_ < n) {
            const uint64_t at = tell();
            base_ = at; pos_ = len_ = 0;
            if (!Fill() || len_ < n) return nullptr;
        }
        return buffer_.data() + pos_;
    }
    void Advance(size_t n) { pos_ += n; }

    uint8_t U8() { uint8_t v = 0; Read(&v, 1); return v; }
    uint16_t U16() { uint8_t b[2]{}; Read(b, 2); return static_cast<uint16_t>(b[0] << 8 | b[1]); }
    uint32_t U32() { uint8_t b[4]{}; Read(b, 4); return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }
    uint64_t U64() { const uint64_t hi = U32(); return hi << 32 | U32(); }

private:
    bool Fill() {
        if (buffer_.empty()) buffer_.resize(kReadChunk);
        base_ += pos_; pos_ = 0; len_ = 0;
        if (base_ >= size_) return false;
        LARGE_INTEGER at{}; at.QuadPart = static_cast<LONGLONG>(base_);
        if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>((std::min<uint64_t>)(kReadChunk, size_ - base_));
        if (!ReadFile(file_, buffer_.data(), want, &got, nullptr) || got == 0) return false;
        len_ = got;
        return true;
    }
    bool Fail() { failed_ = true; return false; }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    uint64_t base_ = 0;
    std::vector<unsigned char> buffer_;
    size_t pos_ = 0, len_ = 0;
    bool failed_ = false;
};

struct Header {
    bool psb = false;
    uint16_t channels = 0;
    uint32_t height = 0, width = 0;
    uint16_t depth = 0, mode = 0;
    unsigned char palette[768]{};
    bool has_palette = false;
    std::vector<unsigned char> thumbnail;   // JFIF from resource 1036
    uint32_t thumbnail_edge = 0;            // its long edge in pixels
    std::vector<unsigned char> icc;         // ICC profile, resource 1039 (gray, CMYK; RGB unless sRGB)
    bool real_merged = true;                // resource 1057 hasRealMergedData
    bool merged_alpha = false;              // merged image carries transparency
    uint64_t image_data = 0;                // offset of the image data section
};

enum Mode : uint16_t { Bitmap = 0, Gray = 1, Indexed = 2, Rgb = 3, Cmyk = 4, Multichannel = 7, Duotone = 8, Lab = 9 };

// Tagged blocks with an 8-byte length in PSB documents.
bool LongTaggedBlock(const char* key) {
    static const char* const kKeys[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                        "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    for (const char* k : kKeys)
        if (std::memcmp(key, k, 4) == 0) return true;
    return false;
}

bool TaggedSignatureAt(Reader& r, uint64_t at, uint64_t end) {
    char sig[4]{};
    return at + 12 <= end && r.Seek(at) && r.Read(sig, 4) &&
        (std::memcmp(sig, "8BIM", 4) == 0 || std::memcmp(sig, "8B64", 4) == 0);
}

// Additional layer information after the global layer mask: signature, key,
// length, data. 16/32-bit documents keep their layers in "Lr16"/"Lr32" (the
// 8-bit layer info is empty), so the merged-transparency sign is read there
// too; "Mtrn"/"Mt16"/"Mt32" mark merged transparency outright.
void ScanTaggedBlocks(Reader& r, Header& h, uint64_t at, uint64_t end) {
    if (at > end || end - at < 4 || !r.Seek(at)) return;
    const uint32_t mask_len = r.U32();
    if (!r.ok() || mask_len > end - at - 4) return;
    at += 4 + uint64_t{mask_len};
    for (int blocks = 0; blocks < 256 && r.ok(); ++blocks) {
        // Writers disagree on padding block data to 2 or 4 bytes.
        uint64_t found = 0;
        for (const uint64_t candidate : {at, (at + 1) & ~uint64_t{1}, (at + 3) & ~uint64_t{3}}) {
            if (TaggedSignatureAt(r, candidate, end)) { found = candidate; break; }
        }
        if (!found) return;
        char key[4]{};
        if (!r.Read(key, 4)) return;
        const bool wide = h.psb && LongTaggedBlock(key);
        if (found + 12 + (wide ? 4u : 0u) > end) return;
        const uint64_t len = wide ? r.U64() : r.U32();
        const uint64_t data = r.tell();
        if (!r.ok() || len > end - data) return;
        if (std::memcmp(key, "Mtrn", 4) == 0 || std::memcmp(key, "Mt16", 4) == 0 ||
            std::memcmp(key, "Mt32", 4) == 0) {
            h.merged_alpha = true;
        } else if (len >= 2 && (std::memcmp(key, "Lr16", 4) == 0 ||
                   std::memcmp(key, "Lr32", 4) == 0 || std::memcmp(key, "Layr", 4) == 0)) {
            if (static_cast<int16_t>(r.U16()) < 0) h.merged_alpha = true;
        }
        at = data + len;
    }
}

bool ParseHeader(Reader& r, Header& h, std::wstring* error) {
    auto fail = [&](const wchar_t* why) { if (error) *error = why; return false; };
    char sig[4]{};
    if (!r.Read(sig, 4) || std::memcmp(sig, "8BPS", 4) != 0) return fail(L"psd-not-photoshop");
    const uint16_t version = r.U16();
    if (version != 1 && version != 2) return fail(L"psd-bad-version");
    h.psb = version == 2;
    r.Skip(6);
    h.channels = r.U16();
    h.height = r.U32();
    h.width = r.U32();
    h.depth = r.U16();
    h.mode = r.U16();
    const uint32_t max_edge = h.psb ? 300000u : 30000u;
    if (!r.ok() || h.channels < 1 || h.channels > 56 || !h.width || !h.height ||
        h.width > max_edge || h.height > max_edge ||
        (h.depth != 1 && h.depth != 8 && h.depth != 16 && h.depth != 32))
        return fail(L"psd-bad-header");

    // Color mode data: indexed documents keep their 256-entry palette here.
    const uint32_t color_len = r.U32();
    if (h.mode == Indexed && color_len >= 768) {
        r.Read(h.palette, 768);
        h.has_palette = true;
        r.Skip(color_len - 768);
    } else {
        r.Skip(color_len);
    }

    // Image resources: embedded thumbnail, ICC profile and the merged-data flag.
    const uint32_t res_len = r.U32();
    const uint64_t res_end = r.tell() + res_len;
    while (r.ok() && r.tell() + 12 <= res_end) {
        char tag[4]{};
        r.Read(tag, 4);
        if (std::memcmp(tag, "8BIM", 4) != 0) break;
        const uint16_t id = r.U16();
        const uint8_t name_len = r.U8();
        r.Skip((name_len + 1) % 2 == 0 ? name_len : name_len + 1u);   // pascal string, even total
        const uint32_t size = r.U32();
        const uint64_t data_end = r.tell() + size + (size & 1u);
        if (data_end > res_end) break;
        if (id == 1036 && size > 28) {
            const uint32_t format = r.U32();
            const uint32_t thumb_w = r.U32(), thumb_h = r.U32();
            r.Skip(16);
            if (format == 1 && size - 28 < 8u * 1024u * 1024u) {
                h.thumbnail.resize(size - 28);
                if (!r.Read(h.thumbnail.data(), h.thumbnail.size())) h.thumbnail.clear();
                else h.thumbnail_edge = (std::max)(thumb_w, thumb_h);
            }
        } else if (id == 1039 && size >= 128 && size <= kIccBudget &&
                   (h.mode == Gray || h.mode == Cmyk || (h.mode == Rgb && h.depth <= 16))) {
            // 32-bit documents hold linear light, already encoded for display.
            h.icc.resize(size);
            if (!r.Read(h.icc.data(), size) || (h.mode == Rgb && icc::IsSrgb(h.icc.data(), h.icc.size())))
                h.icc.clear();
        } else if (id == 1057 && size >= 5) {
            r.Skip(4);
            h.real_merged = r.U8() != 0;
        }
        r.Seek(data_end);
    }
    if (!r.Seek(res_end)) return fail(L"psd-truncated");

    // Layer and mask information. The first extra channel is the merged
    // transparency when the layer count is negative, or when the tagged
    // blocks after the layer info say so (see ScanTaggedBlocks): 16-bit
    // documents with an empty 8-bit layer info otherwise read as opaque and
    // their white-matted composite showed as a white sheet.
    const uint64_t lm_len = h.psb ? r.U64() : r.U32();
    const uint64_t lm_start = r.tell();
    if (!r.ok() || lm_len > r.size() - (std::min)(r.size(), lm_start)) return fail(L"psd-truncated");
    const uint64_t lm_end = lm_start + lm_len;
    if (lm_len >= (h.psb ? 8u : 4u)) {
        const uint64_t layer_len = h.psb ? r.U64() : r.U32();
        const uint64_t layer_start = r.tell();
        if (r.ok() && layer_start <= lm_end && layer_len <= lm_end - layer_start) {
            if (layer_len >= 2) h.merged_alpha = static_cast<int16_t>(r.U16()) < 0;
            if (r.ok()) ScanTaggedBlocks(r, h, layer_start + layer_len, lm_end);
        }
    }
    if (!r.Seek(lm_end) || !r.ok()) return fail(L"psd-truncated");
    h.image_data = lm_end;
    return true;
}

int ColorChannels(uint16_t mode) {
    switch (mode) {
    case Rgb: case Lab: return 3;
    case Cmyk: return 4;
    default: return 1;
    }
}

bool PackBits(const unsigned char* src, size_t src_len, unsigned char* dst, size_t dst_len) {
    size_t i = 0, o = 0;
    while (i < src_len) {
        const int n = static_cast<int8_t>(src[i++]);
        if (n >= 0) {
            const size_t count = static_cast<size_t>(n) + 1;
            if (count > src_len - i || count > dst_len - o) return false;
            std::memcpy(dst + o, src + i, count);
            i += n + 1; o += count;
        } else if (n != -128) {
            if (i >= src_len) return false;
            const size_t count = static_cast<size_t>(1 - n);
            if (count > dst_len - o) return false;
            std::memset(dst + o, src[i++], count);
            o += count;
        }
    }
    return o == dst_len;
}

// Linear coverage (32-bit alpha) -> byte; only colour takes the display curve.
uint8_t LinearToByte(float f) {
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 255;
    return static_cast<uint8_t>(std::lround(f * 255.0f));
}

// Linear float (32-bit documents) -> display byte.
uint8_t FloatToByte(float f) {
    static const auto lut = [] {
        std::vector<uint8_t> t(4096);
        for (int i = 0; i < 4096; ++i)
            t[i] = static_cast<uint8_t>(std::lround(255.0 * std::pow(i / 4095.0, 1.0 / 2.2)));
        return t;
    }();
    if (!(f > 0.0f)) return 0;
    if (f >= 1.0f) return 255;
    return lut[static_cast<int>(f * 4095.0f)];
}

void LabToRgb(uint8_t l8, uint8_t a8, uint8_t b8, uint8_t& r, uint8_t& g, uint8_t& b) {
    const double L = l8 * 100.0 / 255.0, A = a8 - 128.0, B = b8 - 128.0;
    double fy = (L + 16.0) / 116.0, fx = fy + A / 500.0, fz = fy - B / 200.0;
    auto inv = [](double t) { return t > 6.0 / 29.0 ? t * t * t : 3.0 * (6.0 / 29.0) * (6.0 / 29.0) * (t - 4.0 / 29.0); };
    const double X = 0.9642 * inv(fx), Y = inv(fy), Z = 0.8249 * inv(fz);   // D50
    double lr = 3.1339 * X - 1.6169 * Y - 0.4906 * Z;
    double lg = -0.9788 * X + 1.9161 * Y + 0.0335 * Z;
    double lb = 0.0719 * X - 0.2290 * Y + 1.4052 * Z;
    auto enc = [](double v) {
        v = (std::clamp)(v, 0.0, 1.0);
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<uint8_t>(std::lround(v * 255.0));
    };
    r = enc(lr); g = enc(lg); b = enc(lb);
}

// Gray and CMYK are converted to sRGB through the document's ICC profile:
// Photoshop grays are dot-gain encoded and press CMYK is not RGB minus ink,
// so plain values showed grays far too dark and CMYK far too saturated.
// CMYK without a profile goes through the system's SWOP profile (RSWOP.icm),
// as Photoshop falls back to a SWOP working space. RGB documents tagged with
// another profile than sRGB (Adobe RGB, Display P3, ProPhoto) are converted
// the same way. device holds one value (gray) or four (CMYK ink, 255 = full)
// per pixel; RGB reads the pixels themselves. On success the colour bytes of
// the BGRA pixels are replaced. Otherwise they keep the plain values.
// Matrix/TRC RGB profiles skip the system transform (icc::MatrixToSrgb).
bool ManageColour(const Header& h, std::vector<unsigned char>& device, UINT width, UINT height,
                  std::vector<unsigned char>& pixels) {
    const bool cmyk = h.mode == Cmyk;
    const bool rgb = h.mode == Rgb;
    if (rgb && pixels.size() >= size_t{width} * height * 4) {
        icc::MatrixToSrgb matrix;
        if (matrix.Init(h.icc.data(), h.icc.size())) {
            matrix.Apply(pixels.data(), size_t{width} * height);
            return true;
        }
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICColorContext> source, target;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateColorContext(&source)) || FAILED(factory->CreateColorContext(&target)) ||
        FAILED(target->InitializeFromExifColorSpace(1)))   // sRGB
        return false;
    HRESULT hr = E_FAIL;
    if (!h.icc.empty())
        hr = source->InitializeFromMemory(const_cast<BYTE*>(h.icc.data()), static_cast<UINT>(h.icc.size()));
    if (FAILED(hr) && cmyk) {
        wchar_t dir[MAX_PATH]{};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n && n < MAX_PATH)
            hr = source->InitializeFromFilename((std::wstring(dir, n) + L"\\spool\\drivers\\color\\RSWOP.icm").c_str());
    }
    if (FAILED(hr)) return false;
    // 32bppBGR reads the BGRA pixels and ignores their alpha byte.
    std::vector<unsigned char>& input = rgb ? pixels : device;
    const UINT channels = cmyk || rgb ? 4u : 1u;
    const WICPixelFormatGUID format = rgb ? GUID_WICPixelFormat32bppBGR
        : cmyk ? GUID_WICPixelFormat32bppCMYK : GUID_WICPixelFormat8bppGray;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICColorTransform> transform;
    std::vector<unsigned char> bgrx(pixels.size());
    if (FAILED(factory->CreateBitmapFromMemory(width, height, format, width * channels,
                                               static_cast<UINT>(input.size()), input.data(), &bitmap)) ||
        FAILED(factory->CreateColorTransformer(&transform)) ||
        FAILED(transform->Initialize(bitmap.Get(), source.Get(), target.Get(), GUID_WICPixelFormat32bppBGR)) ||
        FAILED(transform->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgrx.size()), bgrx.data())))
        return false;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        pixels[i] = bgrx[i];
        pixels[i + 1] = bgrx[i + 1];
        pixels[i + 2] = bgrx[i + 2];
    }
    return true;
}

bool DecodeComposite(Reader& r, const Header& h, UINT max_edge, std::vector<unsigned char>& pixels,
                     UINT& out_w, UINT& out_h, UINT& stride, std::wstring* error) {
    auto fail = [&](const wchar_t* why) { if (error) *error = why; return false; };
    if (h.mode == Indexed && !h.has_palette) return fail(L"psd-no-palette");
    const int color = ColorChannels(h.mode);
    if (h.channels < color) return fail(L"psd-missing-channels");
    const bool alpha = h.merged_alpha && h.channels > color && h.mode != Indexed && h.mode != Bitmap;
    const int used = color + (alpha ? 1 : 0);

    if (!r.Seek(h.image_data)) return fail(L"psd-truncated");
    const uint16_t compression = r.U16();
    if (compression > 1) return fail(L"psd-zip-composite");

    const double scale = (std::min)(1.0, static_cast<double>(max_edge) / (std::max)(h.width, h.height));
    out_w = (std::max)(1u, static_cast<UINT>(std::lround(h.width * scale)));
    out_h = (std::max)(1u, static_cast<UINT>(std::lround(h.height * scale)));
    const size_t row_bytes = (static_cast<size_t>(h.width) * h.depth + 7) / 8;
    const size_t rows_total = static_cast<size_t>(h.channels) * h.height;

    std::vector<uint32_t> counts;
    if (compression == 1) {
        constexpr size_t kCountBudget = 16u * 1024u * 1024u;
        if (rows_total > kCountBudget / sizeof(uint32_t)) return fail(L"psd-row-table-limit");
        if (r.tell() > r.size() || rows_total * (h.psb ? 4ull : 2ull) > r.size() - r.tell())
            return fail(L"psd-truncated");
        counts.resize(rows_total);
        uint64_t packed_total = 0;
        for (size_t i = 0; i < rows_total; ++i) {
            counts[i] = h.psb ? r.U32() : r.U16();
            packed_total += counts[i];
        }
        if (!r.ok()) return fail(L"psd-truncated");
        if (packed_total > r.size() - r.tell()) return fail(L"psd-truncated");
        // Bound even skipped channels before any row allocation. PackBits can
        // contain arbitrary no-ops, so this is an explicit preview work budget:
        // a preview does not allocate attacker-controlled multi-gigabyte rows,
        // even when the file contains enough padding to hold them.
        const size_t packed_limit = (std::max)(size_t{65536}, row_bytes * 2 + 1024);
        uint64_t remaining = r.size() - r.tell();
        for (uint32_t count : counts) {
            if (count > remaining) return fail(L"psd-truncated");
            if (count > packed_limit) return fail(L"psd-packed-row-limit");
            remaining -= count;
        }
    }

    // Column/row -> output cell maps and per-cell sample counts.
    std::vector<uint32_t> col_to(h.width), rows_in(out_h, 0), cols_in(out_w, 0);
    for (uint32_t x = 0; x < h.width; ++x) {
        col_to[x] = (std::min)(out_w - 1, static_cast<UINT>(static_cast<uint64_t>(x) * out_w / h.width));
        ++cols_in[col_to[x]];
    }
    for (uint32_t y = 0; y < h.height; ++y)
        ++rows_in[(std::min)(out_h - 1, static_cast<UINT>(static_cast<uint64_t>(y) * out_h / h.height))];

    const int accumulated = h.mode == Indexed ? 3 : used;
    constexpr size_t kAccumulatorBudget = 128u * 1024u * 1024u;
    if (static_cast<uint64_t>(out_w) * out_h >
        kAccumulatorBudget / sizeof(uint64_t) / static_cast<size_t>(accumulated))
        return fail(L"psd-output-limit");
    std::vector<std::vector<uint64_t>> acc(accumulated, std::vector<uint64_t>(static_cast<size_t>(out_w) * out_h, 0));
    std::vector<unsigned char> raw(row_bytes), packed;
    std::vector<uint8_t> row8(h.width);
    const ULONGLONG deadline = GetTickCount64() + kDecodeBudgetMs;

    for (int c = 0; c < h.channels; ++c) {
        const bool keep = c < used;
        for (uint32_t y = 0; y < h.height; ++y) {
            const size_t index = static_cast<size_t>(c) * h.height + y;
            if (compression == 1) {
                const uint32_t n = counts[index];
                if (!keep) { r.Skip(n); continue; }
                if (const unsigned char* p = r.Peek(n)) {
                    if (!PackBits(p, n, raw.data(), row_bytes)) return fail(L"psd-bad-packbits");
                    r.Advance(n);
                } else {
                    packed.resize(n);
                    if (!r.Read(packed.data(), n)) return fail(L"psd-truncated");
                    if (!PackBits(packed.data(), n, raw.data(), row_bytes)) return fail(L"psd-bad-packbits");
                }
            } else {
                if (!keep) { r.Skip(row_bytes); continue; }
                if (!r.Read(raw.data(), row_bytes)) return fail(L"psd-truncated");
            }
            if (!r.ok()) return fail(L"psd-truncated");
            if ((y & 63) == 0 && GetTickCount64() > deadline) return fail(L"psd-timeout");

            switch (h.depth) {
            case 1:
                for (uint32_t x = 0; x < h.width; ++x)
                    row8[x] = (raw[x >> 3] >> (7 - (x & 7)) & 1) ? 0 : 255;
                break;
            case 8: std::memcpy(row8.data(), raw.data(), h.width); break;
            case 16: for (uint32_t x = 0; x < h.width; ++x) row8[x] = raw[x * 2]; break;
            case 32:
                for (uint32_t x = 0; x < h.width; ++x) {
                    const uint32_t bits = uint32_t(raw[x * 4]) << 24 | uint32_t(raw[x * 4 + 1]) << 16 |
                                          uint32_t(raw[x * 4 + 2]) << 8 | raw[x * 4 + 3];
                    float f; std::memcpy(&f, &bits, 4);
                    row8[x] = alpha && c == color ? LinearToByte(f) : FloatToByte(f);
                }
                break;
            }
            const UINT oy = (std::min)(out_h - 1, static_cast<UINT>(static_cast<uint64_t>(y) * out_h / h.height));
            if (h.mode == Indexed) {
                for (int component = 0; component < 3; ++component) {
                    auto* dst = acc[component].data() + static_cast<size_t>(oy) * out_w;
                    for (uint32_t x = 0; x < h.width; ++x)
                        dst[col_to[x]] += h.palette[component * 256 + row8[x]];
                }
            } else {
                auto* dst = acc[c].data() + static_cast<size_t>(oy) * out_w;
                for (uint32_t x = 0; x < h.width; ++x) dst[col_to[x]] += row8[x];
            }
        }
    }

    stride = out_w * 4;
    pixels.assign(static_cast<size_t>(stride) * out_h, 0);
    // Gray and CMYK cells are also kept as device values for ManageColour;
    // RGB with a profile is converted from the pixels themselves.
    const bool device_values = h.mode == Gray || h.mode == Cmyk;
    const bool managed = device_values || (h.mode == Rgb && !h.icc.empty());
    const int device_channels = h.mode == Cmyk ? 4 : 1;
    std::vector<unsigned char> device(device_values ? static_cast<size_t>(out_w) * out_h * device_channels : 0);
    for (UINT oy = 0; oy < out_h; ++oy) {
        for (UINT ox = 0; ox < out_w; ++ox) {
            const size_t i = static_cast<size_t>(oy) * out_w + ox;
            const uint64_t n = (std::max)(uint64_t{1}, uint64_t{rows_in[oy]} * cols_in[ox]);
            auto v = [&](int c) { return static_cast<uint8_t>((acc[c][i] + n / 2) / n); };
            uint8_t R = 0, G = 0, B = 0;
            switch (h.mode) {
            case Rgb: R = v(0); G = v(1); B = v(2); break;
            case Cmyk: {
                // Photoshop stores CMYK inverted (255 = no ink).
                const uint32_t k = v(3);
                R = static_cast<uint8_t>(v(0) * k / 255); G = static_cast<uint8_t>(v(1) * k / 255);
                B = static_cast<uint8_t>(v(2) * k / 255);
                break;
            }
            case Indexed: {
                R = v(0); G = v(1); B = v(2);
                break;
            }
            case Lab: LabToRgb(v(0), v(1), v(2), R, G, B); break;
            default: R = G = B = v(0); break;
            }
            if (device_values)
                for (int c = 0; c < device_channels; ++c)
                    device[i * device_channels + c] = h.mode == Cmyk ? static_cast<unsigned char>(255 - v(c)) : v(c);
            unsigned char* px = pixels.data() + i * 4;
            px[0] = B; px[1] = G; px[2] = R; px[3] = alpha ? v(color) : uint8_t{255};
        }
    }
    if (managed) ManageColour(h, device, out_w, out_h, pixels);
    if (!alpha) return true;
    // Photoshop stores the merged image of a transparent document composited
    // over white: stored = colour * a + 255 * (1 - a), so the premultiplied
    // colour is stored - (255 - a). The relation is linear and holds for
    // box-averaged cells too. Scaling the matted colour by alpha kept the
    // white in soft edges and clear areas. Managed colour maps paper white to
    // sRGB white, so this follows ManageColour.
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const int a = pixels[i + 3];
        for (size_t c = 0; c < 3; ++c)
            pixels[i + c] = static_cast<unsigned char>((std::clamp)(int{pixels[i + c]} + a - 255, 0, a));
    }
    return true;
}

bool DecodeThumbnail(const std::vector<unsigned char>& jpeg, UINT max_edge,
                     std::vector<unsigned char>& pixels, UINT& out_w, UINT& out_h, UINT& stride) {
    if (jpeg.empty()) return false;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), static_cast<DWORD>(jpeg.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                     nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (!w || !h) return false;
    ComPtr<IWICBitmapSource> source = converter;
    if ((std::max)(w, h) > max_edge) {
        const double s = static_cast<double>(max_edge) / (std::max)(w, h);
        const UINT sw = (std::max)(1u, static_cast<UINT>(std::lround(w * s)));
        const UINT sh = (std::max)(1u, static_cast<UINT>(std::lround(h * s)));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(converter.Get(), sw, sh, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler;
        w = sw; h = sh;
    }
    stride = w * 4;
    pixels.resize(static_cast<size_t>(stride) * h);
    if (FAILED(source->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
        return false;
    out_w = w; out_h = h;
    return true;
}

} // namespace

bool RasterizePsdFile(const std::wstring& path, UINT max_edge, bool prefer_embedded,
                      std::vector<unsigned char>& pixels, UINT& width, UINT& height,
                      UINT& stride, UINT& source_width, UINT& source_height,
                      std::wstring* error, bool* embedded) {
    if (embedded) *embedded = false;
    Reader reader(path);
    if (!reader.ok() || reader.size() < 26) { if (error) *error = L"psd-open-failed"; return false; }
    Header header;
    if (!ParseHeader(reader, header, error)) return false;
    source_width = header.width;
    source_height = header.height;
    max_edge = (std::max)(16u, max_edge);

    // The embedded JPEG has no transparency (it is the white-matted image), is
    // lossy and small. Grid thumbnails use it only for opaque documents too
    // big to decode in a few milliseconds whose JPEG already covers the
    // requested edge; otherwise the composite, while affordable.
    const uint64_t document_pixels = static_cast<uint64_t>(header.width) * header.height;
    const uint32_t wanted_edge = (std::min)(max_edge, (std::max)(header.width, header.height));
    const bool jpeg_covers = header.thumbnail_edge >= wanted_edge;
    const bool composite_first = header.real_merged && document_pixels <= kGridCompositePixels &&
        (header.merged_alpha || !jpeg_covers || document_pixels <= kGridSmallPixels);
    if (prefer_embedded && !composite_first &&
        DecodeThumbnail(header.thumbnail, max_edge, pixels, width, height, stride)) {
        if (embedded) *embedded = true;
        return true;
    }
    if (header.real_merged &&
        DecodeComposite(reader, header, max_edge, pixels, width, height, stride, error))
        return true;
    pixels.clear();
    if (DecodeThumbnail(header.thumbnail, max_edge, pixels, width, height, stride)) {
        if (error) error->clear();
        if (embedded) *embedded = true;
        return true;
    }
    if (error && error->empty()) *error = L"psd-no-composite";
    return false;
}

} // namespace pulse::preview
