#pragma once
#include <windows.h>
#include <emmintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pulse::preview {

// Re-usable exact-area box downsampler for 32bpp PBGRA buffers (dst_w <= src_w, dst_h <= src_h).
// Produces clean anti-aliased thumbnails without WIC scalar allocations.
inline bool DownsamplePbgraExactArea(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                                     uint32_t src_stride, uint32_t dst_w, uint32_t dst_h,
                                     std::vector<uint8_t>& dst) noexcept {
    if (!src || !src_w || !src_h || !dst_w || !dst_h ||
        src_stride < src_w * 4u || dst_w > src_w || dst_h > src_h)
        return false;
    dst.resize(static_cast<size_t>(dst_w) * dst_h * 4u);
    if (src_w == dst_w && src_h == dst_h) {
        const size_t row_bytes = static_cast<size_t>(dst_w) * 4u;
        for (uint32_t y = 0; y < dst_h; ++y) {
            std::memcpy(dst.data() + static_cast<size_t>(y) * row_bytes,
                        src + static_cast<size_t>(y) * src_stride, row_bytes);
        }
        return true;
    }

    // Fast path for exact integer box ratio (e.g. 1024x768 -> 256x192 / 128x96 / 64x48).
    if ((src_w % dst_w) == 0 && (src_h % dst_h) == 0) {
        const uint32_t kx = src_w / dst_w;
        const uint32_t ky = src_h / dst_h;
        const uint64_t area = static_cast<uint64_t>(kx) * ky;
        const uint64_t half = area / 2u;
        thread_local std::vector<uint32_t> row_acc;
        const size_t acc_len = static_cast<size_t>(dst_w) * 4u;
        if (row_acc.size() < acc_len) row_acc.resize(acc_len);
        const __m128i zero = _mm_setzero_si128();
        for (uint32_t dy = 0; dy < dst_h; ++dy) {
            std::fill_n(row_acc.data(), acc_len, 0u);
            for (uint32_t j = 0; j < ky; ++j) {
                const uint8_t* srow = src + static_cast<size_t>(dy * ky + j) * src_stride;
                uint32_t dx = 0;
                if (kx == 2u) {
                    for (; dx + 2u <= dst_w; dx += 2u) {
                        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srow + static_cast<size_t>(dx) * 8u));
                        const __m128i lo = _mm_unpacklo_epi8(v, zero);
                        const __m128i hi = _mm_unpackhi_epi8(v, zero);
                        const __m128i sum16 = _mm_add_epi16(_mm_unpacklo_epi64(lo, hi), _mm_unpackhi_epi64(lo, hi));
                        uint32_t* acc = row_acc.data() + static_cast<size_t>(dx) * 4u;
                        const __m128i a0 = _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc)),
                                                         _mm_unpacklo_epi16(sum16, zero));
                        const __m128i a1 = _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + 4)),
                                                         _mm_unpackhi_epi16(sum16, zero));
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(acc), a0);
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(acc + 4), a1);
                    }
                } else if (kx == 4u) {
                    for (; dx < dst_w; ++dx) {
                        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(srow + static_cast<size_t>(dx) * 16u));
                        const __m128i lo = _mm_unpacklo_epi8(v, zero);
                        const __m128i hi = _mm_unpackhi_epi8(v, zero);
                        const __m128i s0123 = _mm_add_epi16(lo, hi);
                        const __m128i sum16 = _mm_add_epi16(s0123, _mm_srli_si128(s0123, 8));
                        uint32_t* acc = row_acc.data() + static_cast<size_t>(dx) * 4u;
                        const __m128i a0 = _mm_add_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc)),
                                                         _mm_unpacklo_epi16(sum16, zero));
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(acc), a0);
                    }
                }
                for (; dx < dst_w; ++dx) {
                    const uint8_t* px = srow + static_cast<size_t>(dx * kx) * 4u;
                    uint32_t b = 0, g = 0, r = 0, a = 0;
                    for (uint32_t i = 0; i < kx; ++i) {
                        b += px[0];
                        g += px[1];
                        r += px[2];
                        a += px[3];
                        px += 4;
                    }
                    uint32_t* acc = row_acc.data() + static_cast<size_t>(dx) * 4u;
                    acc[0] += b;
                    acc[1] += g;
                    acc[2] += r;
                    acc[3] += a;
                }
            }
            uint8_t* drow = dst.data() + static_cast<size_t>(dy) * dst_w * 4u;
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                const uint32_t* acc = row_acc.data() + static_cast<size_t>(dx) * 4u;
                drow[0] = static_cast<uint8_t>((acc[0] + half) / area);
                drow[1] = static_cast<uint8_t>((acc[1] + half) / area);
                drow[2] = static_cast<uint8_t>((acc[2] + half) / area);
                drow[3] = static_cast<uint8_t>((acc[3] + half) / area);
                drow += 4;
            }
        }
        return true;
    }

    struct AxisSpan {
        uint32_t start;
        uint32_t end;  // exclusive
        uint32_t first_w;
        uint32_t mid_w;
        uint32_t last_w;
    };
    auto build_spans = [](uint32_t src_len, uint32_t dst_len, std::vector<AxisSpan>& spans) {
        spans.resize(dst_len);
        const uint64_t d = dst_len;
        const uint64_t s = src_len;
        for (uint32_t dx = 0; dx < dst_len; ++dx) {
            const uint64_t x0 = static_cast<uint64_t>(dx) * s;
            const uint64_t x1 = static_cast<uint64_t>(dx + 1) * s;
            const uint32_t sx0 = static_cast<uint32_t>(x0 / d);
            const uint32_t sx1 = static_cast<uint32_t>((x1 + d - 1u) / d);
            AxisSpan sp{};
            sp.start = sx0;
            sp.end = (std::min)(src_len, (std::max)(sx0 + 1u, sx1));
            if (sp.start + 1u == sp.end) {
                sp.first_w = static_cast<uint32_t>(s);
                sp.mid_w = 0;
                sp.last_w = 0;
            } else {
                sp.first_w = static_cast<uint32_t>((static_cast<uint64_t>(sx0 + 1u) * d) - x0);
                sp.mid_w = dst_len;
                sp.last_w = static_cast<uint32_t>(x1 - static_cast<uint64_t>(sp.end - 1u) * d);
            }
            spans[dx] = sp;
        }
    };

    thread_local std::vector<AxisSpan> x_spans;
    build_spans(src_w, dst_w, x_spans);

    thread_local std::vector<uint64_t> v_acc;
    const size_t vacc_len = static_cast<size_t>(dst_w) * 4u;
    if (v_acc.size() < vacc_len) v_acc.resize(vacc_len);
    const uint64_t norm = static_cast<uint64_t>(src_w) * src_h;
    const uint64_t half_norm = norm / 2u;

    const uint64_t s_h = src_h;
    const uint64_t d_h = dst_h;
    for (uint32_t dy = 0; dy < dst_h; ++dy) {
        const uint64_t y0 = static_cast<uint64_t>(dy) * s_h;
        const uint64_t y1 = static_cast<uint64_t>(dy + 1u) * s_h;
        const uint32_t sy0 = static_cast<uint32_t>(y0 / d_h);
        const uint32_t sy1 = (std::min)(src_h, static_cast<uint32_t>((y1 + d_h - 1u) / d_h));
        std::fill_n(v_acc.data(), vacc_len, 0ull);
        for (uint32_t sy = sy0; sy < sy1; ++sy) {
            const uint64_t ry0 = static_cast<uint64_t>(sy) * d_h;
            const uint64_t ry1 = static_cast<uint64_t>(sy + 1u) * d_h;
            const uint64_t wy = (std::min)(y1, ry1) - (std::max)(y0, ry0);
            const uint8_t* srow = src + static_cast<size_t>(sy) * src_stride;
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                const AxisSpan& sp = x_spans[dx];
                uint32_t sb = 0, sg = 0, sr = 0, sa = 0;
                const uint8_t* p = srow + static_cast<size_t>(sp.start) * 4u;
                sb += p[0] * sp.first_w;
                sg += p[1] * sp.first_w;
                sr += p[2] * sp.first_w;
                sa += p[3] * sp.first_w;
                p += 4;
                if (sp.start + 1u < sp.end) {
                    const uint32_t mid_end = sp.end - 1u;
                    uint32_t mb = 0, mg = 0, mr = 0, ma = 0;
                    for (uint32_t sx = sp.start + 1u; sx < mid_end; ++sx) {
                        mb += p[0];
                        mg += p[1];
                        mr += p[2];
                        ma += p[3];
                        p += 4;
                    }
                    sb += mb * sp.mid_w + p[0] * sp.last_w;
                    sg += mg * sp.mid_w + p[1] * sp.last_w;
                    sr += mr * sp.mid_w + p[2] * sp.last_w;
                    sa += ma * sp.mid_w + p[3] * sp.last_w;
                }
                uint64_t* va = v_acc.data() + static_cast<size_t>(dx) * 4u;
                va[0] += static_cast<uint64_t>(sb) * wy;
                va[1] += static_cast<uint64_t>(sg) * wy;
                va[2] += static_cast<uint64_t>(sr) * wy;
                va[3] += static_cast<uint64_t>(sa) * wy;
            }
        }
        uint8_t* drow = dst.data() + static_cast<size_t>(dy) * dst_w * 4u;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const uint64_t* va = v_acc.data() + static_cast<size_t>(dx) * 4u;
            drow[0] = static_cast<uint8_t>((va[0] + half_norm) / norm);
            drow[1] = static_cast<uint8_t>((va[1] + half_norm) / norm);
            drow[2] = static_cast<uint8_t>((va[2] + half_norm) / norm);
            drow[3] = static_cast<uint8_t>((va[3] + half_norm) / norm);
            drow += 4;
        }
    }
    return true;
}

// Applies EXIF orientations 1..8 directly on an already-thumbnail-sized 32bpp PBGRA buffer,
// avoiding an intermediate WIC bitmap allocation and WICBitmapFlipRotator pass.
inline bool ApplyExifOrientationPbgra(std::vector<uint8_t>& pixels,
                                      uint32_t& w, uint32_t& h, uint32_t& stride,
                                      unsigned orientation) noexcept {
    if (orientation <= 1u || orientation > 8u) return true;
    if (w == 0 || h == 0 || stride < w * 4u || pixels.size() < static_cast<size_t>(stride) * h) return false;
    const uint32_t sw = w;
    const uint32_t sh = h;
    const bool quarter_turn = orientation >= 5u;
    const uint32_t dw = quarter_turn ? sh : sw;
    const uint32_t dh = quarter_turn ? sw : sh;
    const uint32_t dstride = dw * 4u;
    std::vector<uint8_t> rotated(static_cast<size_t>(dstride) * dh);
    for (uint32_t sy = 0; sy < sh; ++sy) {
        const uint32_t* srow = reinterpret_cast<const uint32_t*>(pixels.data() + static_cast<size_t>(sy) * stride);
        for (uint32_t sx = 0; sx < sw; ++sx) {
            uint32_t dx = sx;
            uint32_t dy = sy;
            switch (orientation) {
            case 2: dx = sw - 1u - sx; dy = sy; break;
            case 3: dx = sw - 1u - sx; dy = sh - 1u - sy; break;
            case 4: dx = sx; dy = sh - 1u - sy; break;
            case 5: dx = sy; dy = sx; break;
            case 6: dx = sh - 1u - sy; dy = sx; break;
            case 7: dx = sh - 1u - sy; dy = sw - 1u - sx; break;
            case 8: dx = sy; dy = sw - 1u - sx; break;
            default: break;
            }
            uint32_t* drow = reinterpret_cast<uint32_t*>(rotated.data() + static_cast<size_t>(dy) * dstride);
            drow[dx] = srow[sx];
        }
    }
    pixels = std::move(rotated);
    w = dw;
    h = dh;
    stride = dstride;
    return true;
}

namespace detail {

struct BmpHeaderInfo {
    uint32_t pixel_offset = 0;
    uint32_t src_w = 0;
    uint32_t src_h = 0;
    uint32_t dst_w = 0;
    uint32_t dst_h = 0;
    uint32_t bits_per_pixel = 0;
    bool top_down = false;
    bool premultiply_alpha = false;
    uint64_t src_stride = 0;
};

inline uint16_t ReadLe16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t ReadLe32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t ReadLeI32(const uint8_t* p) noexcept {
    const uint32_t v = ReadLe32(p);
    int32_t out = 0;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

inline bool ParseBmpHeader(const uint8_t* header, size_t header_bytes, uint64_t total_bytes,
                           uint32_t target_edge, BmpHeaderInfo& info) noexcept {
    if (header_bytes < 54 || target_edge < 16) return false;
    if (header[0] != 'B' || header[1] != 'M') return false;
    const uint32_t bf_off = ReadLe32(header + 10);
    const uint32_t dib_size = ReadLe32(header + 14);
    if (dib_size < 40 || dib_size > 256 || bf_off < 14u + dib_size) return false;
    if (header_bytes < (std::min)(header_bytes, static_cast<size_t>(14u + (std::min)(dib_size, 124u)))) return false;

    const int32_t raw_w = ReadLeI32(header + 18);
    const int32_t raw_h = ReadLeI32(header + 22);
    const uint16_t planes = ReadLe16(header + 26);
    const uint16_t bpp = ReadLe16(header + 28);
    const uint32_t comp = ReadLe32(header + 30);

    if (planes != 1 || raw_w <= 0 || raw_h == 0 || raw_h == INT32_MIN) return false;
    const uint32_t src_w = static_cast<uint32_t>(raw_w);
    const uint32_t src_h = static_cast<uint32_t>(raw_h < 0 ? -int64_t(raw_h) : int64_t(raw_h));
    if (src_w > 32768u || src_h > 32768u || static_cast<uint64_t>(src_w) * src_h > 64000000ull)
        return false;

    bool premul = false;
    if (bpp == 24) {
        if (comp != 0u) return false;  // BI_RGB only
    } else if (bpp == 32) {
        if (comp == 0u) {
            premul = false;  // WIC treats BI_RGB 32bpp as BGRX (opaque)
        } else if (comp == 3u) {
            // BI_BITFIELDS: verify standard BGRA or BGRX channel masks.
            if (dib_size == 40 || dib_size == 52) {
                if (header_bytes < 66 || bf_off < 66) return false;
                const uint32_t r = ReadLe32(header + 54);
                const uint32_t g = ReadLe32(header + 58);
                const uint32_t b = ReadLe32(header + 62);
                if (r != 0x00FF0000u || g != 0x0000FF00u || b != 0x000000FFu) return false;
            } else if (dib_size >= 56) {
                if (header_bytes < 70) return false;
                const uint32_t r = ReadLe32(header + 54);
                const uint32_t g = ReadLe32(header + 58);
                const uint32_t b = ReadLe32(header + 62);
                const uint32_t a = ReadLe32(header + 66);
                if (r != 0x00FF0000u || g != 0x0000FF00u || b != 0x000000FFu ||
                    (a != 0xFF000000u && a != 0u)) return false;
                premul = (a == 0xFF000000u);
            } else {
                return false;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }

    // Embedded/linked color profiles fall back to WIC ToSrgb.
    if (dib_size >= 108) {
        if (header_bytes < 74) return false;
        const uint32_t cs_type = ReadLe32(header + 70);
        // LCS_sRGB = 'sRGB' (0x73524742), LCS_WINDOWS_COLOR_SPACE = 'Win ' (0x57696E20).
        if (cs_type != 0x73524742u && cs_type != 0x57696E20u) return false;
    }

    const uint64_t row_bytes_unpadded = static_cast<uint64_t>(src_w) * (bpp / 8u);
    const uint64_t stride = ((row_bytes_unpadded + 3ull) / 4ull) * 4ull;
    if (stride > 256ull * 1024u) return false;
    const uint64_t needed = static_cast<uint64_t>(bf_off) + stride * src_h;
    if (total_bytes < needed) return false;

    const uint32_t longest = (std::max)(src_w, src_h);
    uint32_t dst_w = src_w, dst_h = src_h;
    if (longest > target_edge) {
        const double ratio = static_cast<double>(target_edge) / static_cast<double>(longest);
        dst_w = (std::max)(1u, static_cast<uint32_t>(static_cast<double>(src_w) * ratio + 0.5));
        dst_h = (std::max)(1u, static_cast<uint32_t>(static_cast<double>(src_h) * ratio + 0.5));
    }
    if (dst_w > src_w || dst_h > src_h) return false;

    info.pixel_offset = bf_off;
    info.src_w = src_w;
    info.src_h = src_h;
    info.dst_w = dst_w;
    info.dst_h = dst_h;
    info.bits_per_pixel = bpp;
    info.top_down = raw_h < 0;
    info.premultiply_alpha = premul;
    info.src_stride = stride;
    return true;
}

template <class RowReader>
inline bool StreamDecodeBmp(const BmpHeaderInfo& info, RowReader&& read_row,
                          std::vector<uint8_t>& out_pbgra) noexcept {
    const uint32_t src_w = info.src_w;
    const uint32_t src_h = info.src_h;
    const uint32_t dst_w = info.dst_w;
    const uint32_t dst_h = info.dst_h;
    const uint32_t dst_stride = dst_w * 4u;
    out_pbgra.resize(static_cast<size_t>(dst_stride) * dst_h);

    const bool bpp24 = info.bits_per_pixel == 24;
    const bool premul = info.premultiply_alpha;

    auto decode_pixel_pbgra = [bpp24, premul](const uint8_t* p, uint32_t& b, uint32_t& g,
                                              uint32_t& r, uint32_t& a) noexcept {
        b = p[0];
        g = p[1];
        r = p[2];
        if (bpp24) {
            a = 255u;
        } else if (!premul) {
            a = 255u;
        } else {
            a = p[3];
            if (a == 0u) {
                b = g = r = 0u;
            } else if (a < 255u) {
                b = (b * a + 127u) / 255u;
                g = (g * a + 127u) / 255u;
                r = (r * a + 127u) / 255u;
            }
        }
    };

    // Fast path 1: 1:1 copy without scaling.
    if (src_w == dst_w && src_h == dst_h) {
        for (uint32_t file_row = 0; file_row < src_h; ++file_row) {
            const uint8_t* row = read_row();
            if (!row) return false;
            const uint32_t sy = info.top_down ? file_row : (src_h - 1u - file_row);
            uint8_t* dst = out_pbgra.data() + static_cast<size_t>(sy) * dst_stride;
            if (bpp24) {
                for (uint32_t x = 0; x < src_w; ++x) {
                    dst[0] = row[0];
                    dst[1] = row[1];
                    dst[2] = row[2];
                    dst[3] = 255u;
                    row += 3;
                    dst += 4;
                }
            } else if (!premul) {
                std::memcpy(dst, row, dst_stride);
                for (uint32_t x = 0; x < src_w; ++x) dst[x * 4u + 3u] = 255u;
            } else {
                for (uint32_t x = 0; x < src_w; ++x) {
                    uint32_t b, g, r, a;
                    decode_pixel_pbgra(row, b, g, r, a);
                    dst[0] = static_cast<uint8_t>(b);
                    dst[1] = static_cast<uint8_t>(g);
                    dst[2] = static_cast<uint8_t>(r);
                    dst[3] = static_cast<uint8_t>(a);
                    row += 4;
                    dst += 4;
                }
            }
        }
        return true;
    }

    // Fast path 2: exact integer box ratio (e.g. 1024x768 -> 256x192, 128x96, 64x48, 512x384).
    if ((src_w % dst_w) == 0 && (src_h % dst_h) == 0) {
        const uint32_t kx = src_w / dst_w;
        const uint32_t ky = src_h / dst_h;
        const uint32_t area = kx * ky;
        const uint32_t half = area / 2u;
        thread_local std::vector<uint32_t> acc;
        acc.assign(static_cast<size_t>(dst_w) * 4u, 0u);
        uint32_t rows_in_bucket = 0;
        const uint32_t px_bytes = bpp24 ? 3u : 4u;

        for (uint32_t file_row = 0; file_row < src_h; ++file_row) {
            const uint8_t* row = read_row();
            if (!row) return false;
            const uint32_t sy = info.top_down ? file_row : (src_h - 1u - file_row);
            const uint32_t dy = sy / ky;

            if (bpp24) {
                for (uint32_t dx = 0; dx < dst_w; ++dx) {
                    const uint8_t* p = row + static_cast<size_t>(dx) * kx * 3u;
                    uint32_t sb = 0, sg = 0, sr = 0;
                    for (uint32_t i = 0; i < kx; ++i) {
                        sb += p[0];
                        sg += p[1];
                        sr += p[2];
                        p += 3;
                    }
                    uint32_t* dst_acc = acc.data() + static_cast<size_t>(dx) * 4u;
                    dst_acc[0] += sb;
                    dst_acc[1] += sg;
                    dst_acc[2] += sr;
                }
            } else if (!premul && (kx == 4u || kx == 8u || kx == 16u)) {
                // Four BGRX pixels per load. Even at 16:1 the lane sums fit
                // in 16 bits. X is ignored: BI_RGB/BGRX output is opaque.
                const __m128i zero = _mm_setzero_si128();
                const uint8_t* p = row;
                for (uint32_t dx = 0; dx < dst_w; ++dx) {
                    __m128i sum = zero;
                    for (uint32_t i = 0; i < kx; i += 4, p += 16) {
                        const auto v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                        sum = _mm_add_epi16(sum, _mm_add_epi16(_mm_unpacklo_epi8(v, zero),
                                                              _mm_unpackhi_epi8(v, zero)));
                    }
                    sum = _mm_add_epi16(sum, _mm_srli_si128(sum, 8));
                    auto* a = reinterpret_cast<__m128i*>(acc.data() + size_t(dx) * 4);
                    _mm_storeu_si128(a, _mm_add_epi32(_mm_loadu_si128(a), _mm_unpacklo_epi16(sum, zero)));
                }
            } else if (!premul) {
                for (uint32_t dx = 0; dx < dst_w; ++dx) {
                    const uint8_t* p = row + static_cast<size_t>(dx) * kx * 4u;
                    uint32_t sb = 0, sg = 0, sr = 0;
                    for (uint32_t i = 0; i < kx; ++i) {
                        sb += p[0];
                        sg += p[1];
                        sr += p[2];
                        p += 4;
                    }
                    uint32_t* dst_acc = acc.data() + static_cast<size_t>(dx) * 4u;
                    dst_acc[0] += sb;
                    dst_acc[1] += sg;
                    dst_acc[2] += sr;
                }
            } else {
                for (uint32_t dx = 0; dx < dst_w; ++dx) {
                    const uint8_t* p = row + static_cast<size_t>(dx) * kx * px_bytes;
                    uint32_t sb = 0, sg = 0, sr = 0, sa = 0;
                    for (uint32_t i = 0; i < kx; ++i) {
                        uint32_t b, g, r, a;
                        decode_pixel_pbgra(p, b, g, r, a);
                        sb += b; sg += g; sr += r; sa += a;
                        p += px_bytes;
                    }
                    uint32_t* dst_acc = acc.data() + static_cast<size_t>(dx) * 4u;
                    dst_acc[0] += sb; dst_acc[1] += sg; dst_acc[2] += sr; dst_acc[3] += sa;
                }
            }

            ++rows_in_bucket;
            if (rows_in_bucket == ky) {
                uint8_t* drow = out_pbgra.data() + static_cast<size_t>(dy) * dst_stride;
                if (bpp24 || !premul) {
                    for (uint32_t dx = 0; dx < dst_w; ++dx) {
                        uint32_t* a = acc.data() + static_cast<size_t>(dx) * 4u;
                        drow[0] = static_cast<uint8_t>((a[0] + half) / area);
                        drow[1] = static_cast<uint8_t>((a[1] + half) / area);
                        drow[2] = static_cast<uint8_t>((a[2] + half) / area);
                        drow[3] = 255u;
                        a[0] = a[1] = a[2] = 0u;
                        drow += 4;
                    }
                } else {
                    for (uint32_t dx = 0; dx < dst_w; ++dx) {
                        uint32_t* a = acc.data() + static_cast<size_t>(dx) * 4u;
                        drow[0] = static_cast<uint8_t>((a[0] + half) / area);
                        drow[1] = static_cast<uint8_t>((a[1] + half) / area);
                        drow[2] = static_cast<uint8_t>((a[2] + half) / area);
                        drow[3] = static_cast<uint8_t>((a[3] + half) / area);
                        a[0] = a[1] = a[2] = a[3] = 0u;
                        drow += 4;
                    }
                }
                rows_in_bucket = 0;
            }
        }
        if (acc.capacity() > 16384u) std::vector<uint32_t>{}.swap(acc);
        return true;
    }

    // General exact-area box filter using a 2-row ping-pong vertical accumulator (16 KiB at 256px).
    struct AxisSpan {
        uint32_t start;
        uint32_t end;
        uint32_t first_w;
        uint32_t mid_w;
        uint32_t last_w;
    };
    std::vector<AxisSpan> x_spans(dst_w);
    {
        const uint64_t d = dst_w;
        const uint64_t s = src_w;
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const uint64_t x0 = static_cast<uint64_t>(dx) * s;
            const uint64_t x1 = static_cast<uint64_t>(dx + 1u) * s;
            const uint32_t sx0 = static_cast<uint32_t>(x0 / d);
            const uint32_t sx1 = static_cast<uint32_t>((x1 + d - 1u) / d);
            AxisSpan sp{};
            sp.start = sx0;
            sp.end = (std::min)(src_w, (std::max)(sx0 + 1u, sx1));
            if (sp.start + 1u == sp.end) {
                sp.first_w = static_cast<uint32_t>(s);
            } else {
                sp.first_w = static_cast<uint32_t>((static_cast<uint64_t>(sx0 + 1u) * d) - x0);
                sp.mid_w = dst_w;
                sp.last_w = static_cast<uint32_t>(x1 - static_cast<uint64_t>(sp.end - 1u) * d);
            }
            x_spans[dx] = sp;
        }
    }

    const size_t acc_row_elems = static_cast<size_t>(dst_w) * 4u;
    thread_local std::vector<uint64_t> v_acc;
    thread_local std::vector<uint32_t> h_row;
    v_acc.assign(acc_row_elems * 2u, 0ull);
    h_row.resize(acc_row_elems);

    const uint64_t norm = static_cast<uint64_t>(src_w) * src_h;
    const uint64_t half_norm = norm / 2u;
    const uint64_t s_h = src_h;
    const uint64_t d_h = dst_h;
    const uint32_t px_bytes = bpp24 ? 3u : 4u;

    auto emit_row = [&](uint32_t dy) noexcept {
        uint64_t* va = v_acc.data() + (dy & 1u) * acc_row_elems;
        uint8_t* drow = out_pbgra.data() + static_cast<size_t>(dy) * dst_stride;
        if (bpp24 || !premul) {
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                uint64_t* p = va + static_cast<size_t>(dx) * 4u;
                drow[0] = static_cast<uint8_t>((p[0] + half_norm) / norm);
                drow[1] = static_cast<uint8_t>((p[1] + half_norm) / norm);
                drow[2] = static_cast<uint8_t>((p[2] + half_norm) / norm);
                drow[3] = 255u;
                p[0] = p[1] = p[2] = 0ull;
                drow += 4;
            }
        } else {
            for (uint32_t dx = 0; dx < dst_w; ++dx) {
                uint64_t* p = va + static_cast<size_t>(dx) * 4u;
                drow[0] = static_cast<uint8_t>((p[0] + half_norm) / norm);
                drow[1] = static_cast<uint8_t>((p[1] + half_norm) / norm);
                drow[2] = static_cast<uint8_t>((p[2] + half_norm) / norm);
                drow[3] = static_cast<uint8_t>((p[3] + half_norm) / norm);
                p[0] = p[1] = p[2] = p[3] = 0ull;
                drow += 4;
            }
        }
    };

    for (uint32_t file_row = 0; file_row < src_h; ++file_row) {
        const uint8_t* row = read_row();
        if (!row) return false;
        const uint32_t sy = info.top_down ? file_row : (src_h - 1u - file_row);

        // Horizontal shrink of the single source row into h_row.
        for (uint32_t dx = 0; dx < dst_w; ++dx) {
            const AxisSpan& sp = x_spans[dx];
            const uint8_t* p = row + static_cast<size_t>(sp.start) * px_bytes;
            uint32_t sb = 0, sg = 0, sr = 0, sa = 0;
            uint32_t b, g, r, a;
            decode_pixel_pbgra(p, b, g, r, a);
            sb += b * sp.first_w; sg += g * sp.first_w; sr += r * sp.first_w; sa += a * sp.first_w;
            p += px_bytes;
            if (sp.start + 1u < sp.end) {
                const uint32_t mid_end = sp.end - 1u;
                uint32_t mb = 0, mg = 0, mr = 0, ma = 0;
                for (uint32_t sx = sp.start + 1u; sx < mid_end; ++sx) {
                    decode_pixel_pbgra(p, b, g, r, a);
                    mb += b; mg += g; mr += r; ma += a;
                    p += px_bytes;
                }
                sb += mb * sp.mid_w; sg += mg * sp.mid_w; sr += mr * sp.mid_w; sa += ma * sp.mid_w;
                decode_pixel_pbgra(p, b, g, r, a);
                sb += b * sp.last_w; sg += g * sp.last_w; sr += r * sp.last_w; sa += a * sp.last_w;
            }
            uint32_t* hr = h_row.data() + static_cast<size_t>(dx) * 4u;
            hr[0] = sb; hr[1] = sg; hr[2] = sr; hr[3] = sa;
        }

        // Source row `sy` covers [ry0, ry1) in scaled space and overlaps at most 2 dst_y rows (since src_h >= dst_h).
        const uint64_t ry0 = static_cast<uint64_t>(sy) * d_h;
        const uint64_t ry1 = static_cast<uint64_t>(sy + 1u) * d_h;
        const uint32_t dy0 = static_cast<uint32_t>(ry0 / s_h);
        const uint32_t dy1 = (std::min)(dst_h - 1u, static_cast<uint32_t>((ry1 - 1u) / s_h));

        auto accum_into = [&](uint32_t dy, uint64_t wy) noexcept {
            uint64_t* va = v_acc.data() + (dy & 1u) * acc_row_elems;
            if (bpp24 || !premul) {
                for (size_t i = 0; i < acc_row_elems; i += 4) {
                    va[i + 0] += static_cast<uint64_t>(h_row[i + 0]) * wy;
                    va[i + 1] += static_cast<uint64_t>(h_row[i + 1]) * wy;
                    va[i + 2] += static_cast<uint64_t>(h_row[i + 2]) * wy;
                }
            } else {
                for (size_t i = 0; i < acc_row_elems; ++i) {
                    va[i] += static_cast<uint64_t>(h_row[i]) * wy;
                }
            }
        };

        if (dy0 == dy1) {
            accum_into(dy0, d_h);
            // Check completion in traversal direction:
            if (info.top_down) {
                const uint32_t last_sy = static_cast<uint32_t>((static_cast<uint64_t>(dy0 + 1u) * s_h - 1u) / d_h);
                if (sy == last_sy) emit_row(dy0);
            } else {
                const uint32_t first_sy = static_cast<uint32_t>((static_cast<uint64_t>(dy0) * s_h) / d_h);
                if (sy == first_sy) emit_row(dy0);
            }
        } else {
            // Split between dy0 and dy0 + 1 (dst_h >= 2 guaranteed here).
            const uint64_t split = static_cast<uint64_t>(dy0 + 1u) * s_h;
            const uint64_t w0 = split - ry0;
            const uint64_t w1 = ry1 - split;
            accum_into(dy0, w0);
            accum_into(dy1, w1);
            if (info.top_down) {
                // Moving downward, sy finished dy0.
                emit_row(dy0);
                if (sy + 1u == src_h) emit_row(dy1);
            } else {
                // Moving upward, sy finished dy1.
                emit_row(dy1);
                if (sy == 0u) emit_row(dy0);
            }
        }
    }
    if (v_acc.capacity() > 65536u) std::vector<uint64_t>{}.swap(v_acc);
    if (h_row.capacity() > 32768u) std::vector<uint32_t>{}.swap(h_row);
    return true;
}

}  // namespace detail

inline bool DecodeFastBmpThumbnailMemory(const uint8_t* data, size_t bytes, uint32_t target_edge,
                                         std::vector<uint8_t>& out_pbgra,
                                         uint32_t& out_w, uint32_t& out_h, uint32_t& out_stride,
                                         uint32_t& out_source_w, uint32_t& out_source_h) noexcept {
    out_pbgra.clear();
    out_w = out_h = out_stride = out_source_w = out_source_h = 0;
    if (!data) return false;
    detail::BmpHeaderInfo info;
    if (!detail::ParseBmpHeader(data, bytes, bytes, target_edge, info)) return false;

    size_t cursor = info.pixel_offset;
    const size_t stride = static_cast<size_t>(info.src_stride);
    auto read_row = [&]() noexcept -> const uint8_t* {
        if (cursor + stride > bytes) return nullptr;
        const uint8_t* row = data + cursor;
        cursor += stride;
        return row;
    };
    if (!detail::StreamDecodeBmp(info, read_row, out_pbgra)) {
        out_pbgra.clear();
        return false;
    }
    out_w = info.dst_w;
    out_h = info.dst_h;
    out_stride = info.dst_w * 4u;
    out_source_w = info.src_w;
    out_source_h = info.src_h;
    return true;
}

inline bool DecodeFastBmpThumbnailFile(const std::wstring& path, uint32_t target_edge,
                                       std::vector<uint8_t>& out_pbgra,
                                       uint32_t& out_w, uint32_t& out_h, uint32_t& out_stride,
                                       uint32_t& out_source_w, uint32_t& out_source_h) noexcept {
    out_pbgra.clear();
    out_w = out_h = out_stride = out_source_w = out_source_h = 0;
    if (path.empty() || target_edge < 16) return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    struct CloseFile { HANDLE h; ~CloseFile() { CloseHandle(h); } } closer{file};

    LARGE_INTEGER file_size_li{};
    if (!GetFileSizeEx(file, &file_size_li) || file_size_li.QuadPart < 54) return false;
    const uint64_t total_bytes = static_cast<uint64_t>(file_size_li.QuadPart);

    constexpr size_t kChunkBytes = 256u * 1024u;
    thread_local std::vector<uint8_t> stream_buf;
    if (stream_buf.size() < kChunkBytes) stream_buf.resize(kChunkBytes);

    DWORD initial_read = 0;
    const DWORD to_read = static_cast<DWORD>((std::min<uint64_t>)(total_bytes, kChunkBytes));
    if (!ReadFile(file, stream_buf.data(), to_read, &initial_read, nullptr) || initial_read < 54)
        return false;

    detail::BmpHeaderInfo info;
    if (!detail::ParseBmpHeader(stream_buf.data(), initial_read, total_bytes, target_edge, info))
        return false;

    const size_t stride = static_cast<size_t>(info.src_stride);
    size_t buf_pos = 0;
    size_t buf_len = initial_read;
    if (info.pixel_offset <= buf_len) {
        buf_pos = info.pixel_offset;
    } else {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(info.pixel_offset);
        if (!SetFilePointerEx(file, pos, nullptr, FILE_BEGIN)) return false;
        buf_pos = 0;
        buf_len = 0;
    }

    auto read_row = [&]() noexcept -> const uint8_t* {
        if (buf_pos + stride <= buf_len) {
            const uint8_t* row = stream_buf.data() + buf_pos;
            buf_pos += stride;
            return row;
        }
        const size_t leftover = buf_len - buf_pos;
        if (leftover > 0) std::memmove(stream_buf.data(), stream_buf.data() + buf_pos, leftover);
        buf_pos = 0;
        buf_len = leftover;
        while (buf_len < stride) {
            const DWORD want = static_cast<DWORD>(stream_buf.size() - buf_len);
            DWORD got = 0;
            if (!ReadFile(file, stream_buf.data() + buf_len, want, &got, nullptr) || got == 0)
                return nullptr;
            buf_len += got;
        }
        const uint8_t* row = stream_buf.data();
        buf_pos = stride;
        return row;
    };

    if (!detail::StreamDecodeBmp(info, read_row, out_pbgra)) {
        out_pbgra.clear();
        return false;
    }
    out_w = info.dst_w;
    out_h = info.dst_h;
    out_stride = info.dst_w * 4u;
    out_source_w = info.src_w;
    out_source_h = info.src_h;
    return true;
}

}  // namespace pulse::preview
