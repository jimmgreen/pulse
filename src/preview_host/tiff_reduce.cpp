// tiff_reduce.cpp - see tiff_reduce.h.
#include "tiff_reduce.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace pulse::preview {

namespace {

constexpr uint32_t kMaxSide = 1u << 20;
constexpr uint32_t kMaxFactor = 4096;               // keeps the uint32 sums in range
constexpr uint64_t kMaxSegmentBytes = 256ull << 20;  // one decoded strip or tile
constexpr uint32_t kMaxSegments = 1u << 22;

uint16_t Get16(const uint8_t* p, bool be) {
    return static_cast<uint16_t>(be ? (p[0] << 8) | p[1] : p[0] | (p[1] << 8));
}
uint32_t Get32(const uint8_t* p, bool be) {
    return be ? (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]
              : uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

struct Entry {
    uint16_t tag = 0, type = 0;
    uint32_t count = 0;
    uint8_t value[4]{};
};

// SHORT or LONG values of an entry (inline or at its offset).
bool ReadValues(TiffByteSource& source, bool be, const Entry& entry, uint32_t max_count,
                std::vector<uint64_t>& out) {
    const size_t unit = entry.type == 3 ? 2 : entry.type == 4 ? 4 : 0;
    if (!unit || entry.count == 0 || entry.count > max_count) return false;
    const size_t bytes = unit * entry.count;
    std::vector<uint8_t> raw(bytes);
    if (bytes <= 4) {
        std::memcpy(raw.data(), entry.value, bytes);
    } else {
        const uint64_t offset = Get32(entry.value, be);
        if (offset + bytes > source.Size() || !source.Read(offset, bytes, raw.data())) return false;
    }
    out.resize(entry.count);
    for (size_t i = 0; i < entry.count; ++i)
        out[i] = unit == 2 ? Get16(&raw[i * 2], be) : Get32(&raw[i * 4], be);
    return true;
}

struct Layout {
    bool be = false;
    uint32_t width = 0, height = 0;
    uint32_t spp = 1, bytes_per_sample = 1;
    uint32_t color = 1;      // 1 gray, 3 RGB
    bool invert = false;     // WhiteIsZero
    int alpha = 0;           // 0 none, 1 associated, 2 unassociated (sample `color`)
    uint32_t compression = 1, predictor = 1;
    bool tiled = false;
    uint32_t seg_width = 0, seg_height = 0;  // strip: width x rows per strip
    uint32_t across = 1, down = 0;           // segments per segment row, segment rows
    std::vector<uint64_t> offsets, counts;
};

TiffReduceStatus ParseLayout(TiffByteSource& source, Layout& layout) {
    const uint64_t file_size = source.Size();
    uint8_t header[8];
    if (file_size < 8 || !source.Read(0, 8, header)) return TiffReduceStatus::Unsupported;
    if (header[0] == 'I' && header[1] == 'I') layout.be = false;
    else if (header[0] == 'M' && header[1] == 'M') layout.be = true;
    else return TiffReduceStatus::Unsupported;
    const bool be = layout.be;
    if (Get16(header + 2, be) != 42) return TiffReduceStatus::Unsupported;  // 43 = BigTIFF
    const uint64_t ifd = Get32(header + 4, be);
    uint8_t count_bytes[2];
    if (ifd < 8 || ifd + 2 > file_size || !source.Read(ifd, 2, count_bytes)) return TiffReduceStatus::Corrupt;
    const uint32_t count = Get16(count_bytes, be);
    if (count == 0 || ifd + 2 + count * 12ull > file_size) return TiffReduceStatus::Corrupt;
    std::vector<uint8_t> raw(count * 12ull);
    if (!source.Read(ifd + 2, raw.size(), raw.data())) return TiffReduceStatus::Corrupt;
    std::vector<Entry> entries(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = &raw[i * 12ull];
        entries[i].tag = Get16(p, be);
        entries[i].type = Get16(p + 2, be);
        entries[i].count = Get32(p + 4, be);
        std::memcpy(entries[i].value, p + 8, 4);
    }
    auto find = [&](uint16_t tag) -> const Entry* {
        for (const Entry& entry : entries) if (entry.tag == tag) return &entry;
        return nullptr;
    };
    std::vector<uint64_t> values;
    // A single SHORT/LONG value, or fallback when the tag is absent.
    auto scalar = [&](uint16_t tag, uint64_t fallback, uint64_t& value) {
        const Entry* entry = find(tag);
        if (!entry) { value = fallback; return true; }
        if (!ReadValues(source, be, *entry, 1, values)) return false;
        value = values[0];
        return true;
    };
    uint64_t width = 0, height = 0, spp = 0, compression = 0, photometric = 0, planar = 0,
             predictor = 0, fill_order = 0;
    if (!scalar(256, 0, width) || !scalar(257, 0, height) || !scalar(277, 1, spp) ||
        !scalar(259, 1, compression) || !scalar(262, 99, photometric) || !scalar(284, 1, planar) ||
        !scalar(317, 1, predictor) || !scalar(266, 1, fill_order)) return TiffReduceStatus::Corrupt;
    if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide) return TiffReduceStatus::Unsupported;
    if (spp < 1 || spp > 8 || fill_order != 1 || (planar != 1 && spp > 1)) return TiffReduceStatus::Unsupported;
    if (compression != 1 && compression != 5 && compression != 32773) return TiffReduceStatus::Unsupported;
    if (predictor != 1 && predictor != 2) return TiffReduceStatus::Unsupported;
    if (photometric == 0 || photometric == 1) layout.color = 1;
    else if (photometric == 2) layout.color = 3;
    else return TiffReduceStatus::Unsupported;  // palette, CMYK, YCbCr, Lab...
    if (spp < layout.color) return TiffReduceStatus::Unsupported;

    const Entry* bits = find(258);
    if (!bits || !ReadValues(source, be, *bits, static_cast<uint32_t>(spp), values) || values.size() != spp)
        return TiffReduceStatus::Unsupported;
    for (const uint64_t b : values) if (b != values[0]) return TiffReduceStatus::Unsupported;
    if (values[0] != 8 && values[0] != 16) return TiffReduceStatus::Unsupported;
    layout.bytes_per_sample = static_cast<uint32_t>(values[0] / 8);
    if (const Entry* format = find(339)) {
        if (!ReadValues(source, be, *format, static_cast<uint32_t>(spp), values)) return TiffReduceStatus::Corrupt;
        for (const uint64_t f : values) if (f != 1) return TiffReduceStatus::Unsupported;  // signed / float
    }
    if (spp > layout.color) {
        const Entry* extra = find(338);
        if (extra && ReadValues(source, be, *extra, static_cast<uint32_t>(spp), values) &&
            (values[0] == 1 || values[0] == 2))
            layout.alpha = static_cast<int>(values[0]);
    }
    layout.width = static_cast<uint32_t>(width);
    layout.height = static_cast<uint32_t>(height);
    layout.spp = static_cast<uint32_t>(spp);
    layout.invert = photometric == 0;
    layout.compression = static_cast<uint32_t>(compression);
    layout.predictor = static_cast<uint32_t>(predictor);

    const Entry *offsets = find(273), *counts = find(279);
    const Entry *tile_offsets = find(324), *tile_counts = find(325);
    uint64_t segments = 0;
    if (tile_offsets && tile_counts) {
        uint64_t tile_width = 0, tile_height = 0;
        if (!scalar(322, 0, tile_width) || !scalar(323, 0, tile_height)) return TiffReduceStatus::Corrupt;
        if (tile_width == 0 || tile_height == 0 || tile_width > 65536 || tile_height > 65536)
            return TiffReduceStatus::Corrupt;
        layout.tiled = true;
        layout.seg_width = static_cast<uint32_t>(tile_width);
        layout.seg_height = static_cast<uint32_t>(tile_height);
        layout.across = static_cast<uint32_t>((width + tile_width - 1) / tile_width);
        layout.down = static_cast<uint32_t>((height + tile_height - 1) / tile_height);
        offsets = tile_offsets;
        counts = tile_counts;
    } else if (offsets && counts) {
        uint64_t rows = 0;
        if (!scalar(278, height, rows)) return TiffReduceStatus::Corrupt;
        rows = (std::clamp)(rows, uint64_t{1}, height);
        layout.seg_width = layout.width;
        layout.seg_height = static_cast<uint32_t>(rows);
        layout.across = 1;
        layout.down = static_cast<uint32_t>((height + rows - 1) / rows);
    } else {
        return TiffReduceStatus::Corrupt;
    }
    segments = uint64_t{layout.across} * layout.down;
    if (segments > kMaxSegments) return TiffReduceStatus::Unsupported;
    const uint64_t segment_bytes = uint64_t{layout.seg_width} * layout.seg_height * layout.spp *
        layout.bytes_per_sample;
    if (segment_bytes > kMaxSegmentBytes) return TiffReduceStatus::Unsupported;
    const uint32_t n = static_cast<uint32_t>(segments);
    if (!ReadValues(source, be, *offsets, n, layout.offsets) ||
        !ReadValues(source, be, *counts, n, layout.counts) ||
        layout.offsets.size() != n || layout.counts.size() != n) return TiffReduceStatus::Corrupt;
    for (uint32_t i = 0; i < n; ++i) {
        if (layout.counts[i] == 0 || layout.offsets[i] + layout.counts[i] > file_size)
            return TiffReduceStatus::Corrupt;
    }
    return TiffReduceStatus::Made;
}

bool DecodePackBits(const uint8_t* data, size_t size, uint8_t* out, size_t capacity, size_t& written) {
    written = 0;
    size_t pos = 0;
    while (pos < size && written < capacity) {
        const int n = static_cast<int8_t>(data[pos++]);
        if (n >= 0) {
            const size_t run = (std::min)({static_cast<size_t>(n) + 1, size - pos, capacity - written});
            std::memcpy(out + written, data + pos, run);
            pos += static_cast<size_t>(n) + 1;
            written += run;
        } else if (n != -128) {
            if (pos >= size) break;
            const size_t run = (std::min)(static_cast<size_t>(1 - n), capacity - written);
            std::memset(out + written, data[pos++], run);
            written += run;
        }
    }
    return written > 0;
}

// Box sums of one worker's output rows [first_row, first_row + rows).
struct Band {
    uint32_t first_row = 0, rows = 0;
    std::vector<uint32_t> sums;
};

struct Job {
    const Layout* layout = nullptr;
    TiffByteSource* source = nullptr;
    uint32_t factor = 1, out_width = 0, out_channels = 0;
    std::atomic<int> failure{0};  // 1 corrupt, 2 read error
};

// Adds the decoded rows of one segment into the band, one k-pixel run of a
// row at a time. Specialized per pixel layout: this loop sees every pixel.
template <uint32_t kBytes, uint32_t kColor, uint32_t kChannels, int kAlpha, bool kInvert>
void AccumulateRows(const Job& job, const uint8_t* pixels, uint32_t x0, uint32_t y0, uint32_t width,
                    uint32_t rows, size_t row_bytes, Band& band, bool big_endian) {
    const uint32_t k = job.factor;
    const size_t step = size_t{job.layout->spp} * kBytes;
    const size_t hi = kBytes == 2 && !big_endian ? 1 : 0;  // high byte of a 16-bit sample
    for (uint32_t r = 0; r < rows; ++r) {
        const uint32_t y = y0 + r;
        uint32_t* cell = &band.sums[(size_t{y / k - band.first_row} * job.out_width + x0 / k) * kChannels];
        const uint8_t* p = pixels + r * row_bytes + hi;
        uint32_t x = 0, left = k - x0 % k;
        while (x < width) {
            const uint32_t n = (std::min)(left, width - x);
            uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            for (uint32_t i = 0; i < n; ++i, p += step) {
                uint32_t c0 = p[0];
                if constexpr (kInvert) c0 = 255u - c0;
                uint32_t c1 = c0, c2 = c0;
                if constexpr (kColor == 3) { c1 = p[kBytes]; c2 = p[2 * kBytes]; }
                if constexpr (kChannels == 4) {
                    const uint32_t a = kAlpha ? p[kColor * kBytes] : 255u;
                    if constexpr (kAlpha == 2) {
                        c0 = (c0 * a + 127u) / 255u; c1 = (c1 * a + 127u) / 255u; c2 = (c2 * a + 127u) / 255u;
                    }
                    s3 += a;
                }
                s0 += c0; s1 += c1; s2 += c2;
            }
            if constexpr (kChannels == 1) {
                cell[0] += s0;
            } else {
                cell[0] += s2; cell[1] += s1; cell[2] += s0;  // BGR(A)
                if constexpr (kChannels == 4) cell[3] += s3;
            }
            cell += kChannels;
            x += n;
            left = k;
        }
    }
}

template <uint32_t kBytes>
void AccumulateFor(const Job& job, const uint8_t* pixels, uint32_t x0, uint32_t y0, uint32_t width,
                   uint32_t rows, size_t row_bytes, Band& band) {
    const Layout& l = *job.layout;
    const bool be = l.be;
    if (l.color == 3) {
        if (l.alpha == 1) AccumulateRows<kBytes, 3, 4, 1, false>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
        else if (l.alpha == 2) AccumulateRows<kBytes, 3, 4, 2, false>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
        else AccumulateRows<kBytes, 3, 3, 0, false>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
    } else if (l.alpha == 1) {
        if (l.invert) AccumulateRows<kBytes, 1, 4, 1, true>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
        else AccumulateRows<kBytes, 1, 4, 1, false>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
    } else if (l.alpha == 2) {
        if (l.invert) AccumulateRows<kBytes, 1, 4, 2, true>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
        else AccumulateRows<kBytes, 1, 4, 2, false>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
    } else {
        if (l.invert) AccumulateRows<kBytes, 1, 1, 0, true>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
        else AccumulateRows<kBytes, 1, 1, 0, false>(job, pixels, x0, y0, width, rows, row_bytes, band, be);
    }
}

void Accumulate(const Job& job, const uint8_t* pixels, uint32_t x0, uint32_t y0, uint32_t width,
                uint32_t rows, size_t row_bytes, Band& band) {
    if (job.layout->bytes_per_sample == 2) AccumulateFor<2>(job, pixels, x0, y0, width, rows, row_bytes, band);
    else AccumulateFor<1>(job, pixels, x0, y0, width, rows, row_bytes, band);
}

void UndoPredictor(const Layout& layout, uint8_t* pixels, uint32_t width, uint32_t rows, size_t row_bytes) {
    const uint32_t spp = layout.spp;
    for (uint32_t r = 0; r < rows; ++r) {
        uint8_t* p = pixels + r * row_bytes;
        if (layout.bytes_per_sample == 1) {
            for (size_t i = spp; i < size_t{width} * spp; ++i) p[i] = static_cast<uint8_t>(p[i] + p[i - spp]);
        } else {
            for (size_t i = spp; i < size_t{width} * spp; ++i) {
                uint8_t* cur = p + i * 2;
                const uint8_t* prev = p + (i - spp) * 2;
                const uint16_t v = static_cast<uint16_t>(Get16(cur, layout.be) + Get16(prev, layout.be));
                if (layout.be) { cur[0] = static_cast<uint8_t>(v >> 8); cur[1] = static_cast<uint8_t>(v); }
                else { cur[0] = static_cast<uint8_t>(v); cur[1] = static_cast<uint8_t>(v >> 8); }
            }
        }
    }
}

void DecodeRows(Job& job, uint32_t first, uint32_t last, Band& band);

// Decodes segment rows [first, last) into band; never throws.
void RunGroup(Job& job, uint32_t first, uint32_t last, Band& band) noexcept {
    try {
        DecodeRows(job, first, last, band);
    } catch (...) {
        job.failure = 2;  // out of memory: WIC gets the file
    }
}

void DecodeRows(Job& job, uint32_t first, uint32_t last, Band& band) {
    const Layout& layout = *job.layout;
    const size_t row_bytes = size_t{layout.seg_width} * layout.spp * layout.bytes_per_sample;
    std::vector<uint8_t> packed, pixels(row_bytes * layout.seg_height);
    for (uint32_t segment_row = first; segment_row < last && !job.failure.load(std::memory_order_relaxed);
         ++segment_row) {
        const uint32_t y0 = segment_row * layout.seg_height;
        const uint32_t rows = (std::min)(layout.seg_height, layout.height - y0);
        for (uint32_t column = 0; column < layout.across; ++column) {
            const size_t index = size_t{segment_row} * layout.across + column;
            const uint32_t x0 = column * layout.seg_width;
            const uint32_t width = (std::min)(layout.seg_width, layout.width - x0);
            // Strips hold only their rows; tiles are always full size.
            const size_t needed = row_bytes * (layout.tiled ? layout.seg_height : rows);
            const uint64_t offset = layout.offsets[index];
            const size_t stored = static_cast<size_t>(layout.counts[index]);
            size_t written = 0;
            if (layout.compression == 1) {
                if (stored < needed) { job.failure = 1; return; }
                if (!job.source->Read(offset, needed, pixels.data())) { job.failure = 2; return; }
                written = needed;
            } else {
                packed.resize(stored);
                if (!job.source->Read(offset, stored, packed.data())) { job.failure = 2; return; }
                const bool ok = layout.compression == 5
                    ? DecodeTiffLzw(packed.data(), stored, pixels.data(), needed, written)
                    : DecodePackBits(packed.data(), stored, pixels.data(), needed, written);
                if (!ok || written == 0) { job.failure = 1; return; }
                // A short last run is common in the wild; the rest reads as zero.
                if (written < needed) std::memset(pixels.data() + written, 0, needed - written);
            }
            if (layout.predictor == 2) UndoPredictor(layout, pixels.data(), width, rows, row_bytes);
            Accumulate(job, pixels.data(), x0, y0, width, rows, row_bytes, band);
        }
    }
}

class MemorySource final : public TiffByteSource {
public:
    MemorySource(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint64_t Size() const override { return size_; }
    bool Read(uint64_t offset, size_t size, uint8_t* out) override {
        if (offset > size_ || size > size_ - offset) return false;
        std::memcpy(out, data_ + offset, size);
        return true;
    }
private:
    const uint8_t* data_;
    size_t size_;
};

// Positional reads; every thread borrows its own handle (reads on one
// synchronous handle would be serialized).
class FileSource final : public TiffByteSource {
public:
    explicit FileSource(std::wstring path) : path_(std::move(path)) {}
    ~FileSource() override { for (HANDLE handle : idle_) CloseHandle(handle); }
    bool Open() {
        HANDLE handle = OpenHandle();
        if (handle == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        const bool ok = GetFileSizeEx(handle, &size) != 0;
        size_ = ok ? static_cast<uint64_t>(size.QuadPart) : 0;
        Return(handle);
        return ok;
    }
    uint64_t Size() const override { return size_; }
    bool Read(uint64_t offset, size_t size, uint8_t* out) override {
        HANDLE handle = Borrow();
        if (handle == INVALID_HANDLE_VALUE) return false;
        bool ok = true;
        while (ok && size) {
            OVERLAPPED at{};
            at.Offset = static_cast<DWORD>(offset);
            at.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const DWORD chunk = static_cast<DWORD>((std::min)(size, size_t{1} << 30));
            DWORD got = 0;
            ok = ReadFile(handle, out, chunk, &got, &at) && got == chunk;
            offset += chunk; out += chunk; size -= chunk;
        }
        Return(handle);
        return ok;
    }
private:
    HANDLE OpenHandle() const {
        return CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    HANDLE Borrow() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!idle_.empty()) { HANDLE handle = idle_.back(); idle_.pop_back(); return handle; }
        }
        return OpenHandle();
    }
    void Return(HANDLE handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_.push_back(handle);
    }
    std::wstring path_;
    uint64_t size_ = 0;
    std::mutex mutex_;
    std::vector<HANDLE> idle_;
};

} // namespace

bool DecodeTiffLzw(const uint8_t* data, size_t size, uint8_t* out, size_t capacity, size_t& written) {
    written = 0;
    // Pre-TIFF 6 "old-style" LZW is LSB-first; WIC handles those files.
    if (size >= 2 && data[0] == 0 && (data[1] & 1)) return false;
    // Every string in the table was written to the output once already:
    // a code is where that copy starts and how long it is, so emitting it
    // is a copy instead of a walk down the prefix chain.
    struct Code { size_t offset; uint32_t length; };
    Code table[4096];
    uint64_t buffer = 0;
    int bits = 0, width = 9, next = 258;
    bool have_previous = false;
    size_t previous_offset = 0;
    uint32_t previous_length = 0;
    size_t pos = 0;
    for (;;) {
        if (bits < width) {
            while (bits <= 56 && pos < size) { buffer = (buffer << 8) | data[pos++]; bits += 8; }
            if (bits < width) return written > 0;  // ran out without EOI: keep what came
        }
        const int code = static_cast<int>((buffer >> (bits - width)) & ((1u << width) - 1));
        bits -= width;
        if (code == 257) break;
        if (code == 256) { width = 9; next = 258; have_previous = false; continue; }
        const size_t start = written;
        uint32_t length;
        if (code < 256) {
            if (written >= capacity) break;
            out[written++] = static_cast<uint8_t>(code);
            length = 1;
        } else if (!have_previous) {
            return false;
        } else {
            size_t from;
            if (code < next) {
                from = table[code].offset;
                length = table[code].length;
            } else if (code == next) {  // KwKwK: previous + its own first byte
                from = previous_offset;
                length = previous_length + 1;
            } else {
                return false;
            }
            const size_t n = (std::min)(size_t{length}, capacity - written);
            if (from + n <= written) {
                if (n <= 8 && written + 8 <= capacity) {
                    uint64_t chunk;  // one 8-byte move; bytes past n are rewritten later
                    std::memcpy(&chunk, out + from, 8);
                    std::memcpy(out + written, &chunk, 8);
                } else {
                    std::memcpy(out + written, out + from, n);
                }
            } else {
                for (size_t i = 0; i < n; ++i) out[written + i] = out[from + i];  // overlaps itself
            }
            written += n;
            if (n < length) break;
        }
        if (have_previous && next < 4096) {
            table[next].offset = previous_offset;
            table[next].length = previous_length + 1;
            ++next;
        }
        if (next >= (1 << width) - 1 && width < 12) ++width;  // early change
        have_previous = true;
        previous_offset = start;
        previous_length = length;
    }
    return true;
}

TiffReduceStatus ReduceTiff(TiffByteSource& source, uint32_t min_edge, unsigned threads, TiffReduced& out) {
    out = {};
    if (min_edge == 0) return TiffReduceStatus::Unsupported;
    Layout layout;
    if (const TiffReduceStatus parsed = ParseLayout(source, layout); parsed != TiffReduceStatus::Made)
        return parsed;
    const uint32_t longest = (std::max)(layout.width, layout.height);
    const uint32_t factor = longest / min_edge;
    if (factor < 2) return TiffReduceStatus::Unsupported;
    if (factor > kMaxFactor) return TiffReduceStatus::Unsupported;

    Job job;
    job.layout = &layout;
    job.source = &source;
    job.factor = factor;
    job.out_width = (layout.width + factor - 1) / factor;
    const uint32_t out_height = (layout.height + factor - 1) / factor;
    job.out_channels = layout.alpha ? 4 : layout.color;

    // Contiguous runs of segment rows with about equal compressed bytes.
    if (threads == 0) threads = (std::min)(8u, (std::max)(1u, std::thread::hardware_concurrency()));
    const uint32_t groups = (std::min)(threads, layout.down);
    std::vector<uint64_t> row_bytes(layout.down, 0);
    uint64_t total = 0;
    for (uint32_t r = 0; r < layout.down; ++r) {
        for (uint32_t c = 0; c < layout.across; ++c) row_bytes[r] += layout.counts[size_t{r} * layout.across + c];
        total += row_bytes[r];
    }
    std::vector<uint32_t> bounds{0};
    uint64_t running = 0;
    for (uint32_t r = 0; r < layout.down && bounds.size() < groups; ++r) {
        running += row_bytes[r];
        if (running * groups >= total * bounds.size() && layout.down - (r + 1) >= groups - bounds.size())
            bounds.push_back(r + 1);
    }
    bounds.push_back(layout.down);

    std::vector<Band> bands(bounds.size() - 1);
    for (size_t g = 0; g + 1 < bounds.size(); ++g) {
        const uint32_t y_first = bounds[g] * layout.seg_height;
        const uint32_t y_last = (std::min)(layout.height, bounds[g + 1] * layout.seg_height) - 1;
        bands[g].first_row = y_first / factor;
        bands[g].rows = y_last / factor - bands[g].first_row + 1;
        bands[g].sums.assign(size_t{bands[g].rows} * job.out_width * job.out_channels, 0);
    }
    std::vector<std::thread> workers;
    for (size_t g = 1; g < bands.size(); ++g) {
        try {
            workers.emplace_back([&, g] { RunGroup(job, bounds[g], bounds[g + 1], bands[g]); });
        } catch (...) {
            RunGroup(job, bounds[g], bounds[g + 1], bands[g]);  // no thread: do it here
        }
    }
    if (!bands.empty()) RunGroup(job, bounds[0], bounds[1], bands[0]);
    for (std::thread& worker : workers) worker.join();
    if (job.failure) return TiffReduceStatus::Corrupt;

    const uint32_t channels = job.out_channels;
    std::vector<uint32_t> sums(size_t{job.out_width} * out_height * channels, 0);
    for (const Band& band : bands) {
        uint32_t* target = &sums[size_t{band.first_row} * job.out_width * channels];
        for (size_t i = 0; i < band.sums.size(); ++i) target[i] += band.sums[i];
    }
    out.width = job.out_width;
    out.height = out_height;
    out.source_width = layout.width;
    out.source_height = layout.height;
    out.factor = factor;
    out.channels = channels;
    out.stride = (job.out_width * channels + 3u) & ~3u;
    out.pixels.assign(size_t{out.stride} * out_height, 0);
    for (uint32_t oy = 0; oy < out_height; ++oy) {
        const uint32_t box_h = (std::min)(factor, layout.height - oy * factor);
        for (uint32_t ox = 0; ox < job.out_width; ++ox) {
            const uint32_t box = box_h * (std::min)(factor, layout.width - ox * factor);
            const uint32_t* sum = &sums[(size_t{oy} * job.out_width + ox) * channels];
            uint8_t* pixel = &out.pixels[size_t{oy} * out.stride + size_t{ox} * channels];
            for (uint32_t c = 0; c < channels; ++c)
                pixel[c] = static_cast<uint8_t>((sum[c] + box / 2) / box);
        }
    }
    return TiffReduceStatus::Made;
}

TiffReduceStatus ReduceTiffMemory(const uint8_t* data, size_t size, uint32_t min_edge, unsigned threads,
                                  TiffReduced& out) {
    if (!data) return TiffReduceStatus::Unsupported;
    MemorySource source(data, size);
    return ReduceTiff(source, min_edge, threads, out);
}

bool EstimateReducedTiffMemory(const std::wstring& path, uint32_t min_edge, uint64_t& bytes) {
    FileSource source(path);
    Layout layout;
    if (!min_edge || !source.Open() || ParseLayout(source, layout) != TiffReduceStatus::Made) return false;
    const uint32_t factor = (std::max)(layout.width, layout.height) / min_edge;
    if (factor < 2 || factor > kMaxFactor) return false;
    const uint64_t groups = (std::min)(layout.down, (std::min)(8u, (std::max)(1u, std::thread::hardware_concurrency())));
    const uint64_t raster = uint64_t(layout.seg_width) * layout.seg_height * layout.spp * layout.bytes_per_sample;
    const uint64_t packed = layout.counts.empty() ? 0 : *std::max_element(layout.counts.begin(), layout.counts.end());
    const uint64_t output = ((uint64_t(layout.width) + factor - 1) / factor) * ((uint64_t(layout.height) + factor - 1) / factor);
    // Per-group compressed and decoded segment; band sums + merged sums,
    // reduced image, WIC copy, conversion/scaling and IPC output.
    const auto multiply = [](uint64_t a, uint64_t b) {
        return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
    };
    const auto add = [](uint64_t a, uint64_t b) {
        return b > UINT64_MAX - a ? UINT64_MAX : a + b;
    };
    bytes = add(32ull << 20, multiply(groups, add(raster, packed)));
    bytes = add(bytes, multiply(output, 64));
    bytes = add(bytes, multiply(layout.counts.size(), 48));
    bytes = add(bytes, multiply(multiply(min_edge, min_edge), 16));
    return true;
}

bool ReduceTiffFile(const std::wstring& path, uint32_t min_edge, TiffReduced& out) {
    FileSource source(path);
    if (!source.Open()) return false;
    return ReduceTiff(source, min_edge, 0, out) == TiffReduceStatus::Made;
}

} // namespace pulse::preview
