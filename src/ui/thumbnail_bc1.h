#pragma once

#include "../common/fast_bmp_thumb.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pulse::ui {

inline constexpr uint32_t kMicroThumbMaxEdge = 64u;
inline constexpr uint8_t kMicroThumbFormatBc1 = 1u;  // 8 bytes per 4x4 block (DXGI_FORMAT_BC1_UNORM)
inline constexpr uint8_t kMicroThumbFormatBc3 = 2u;  // 16 bytes per 4x4 block (DXGI_FORMAT_BC3_UNORM)

struct MicroThumbData {
    uint16_t width = 0;           // 4-aligned BC width (4..64)
    uint16_t height = 0;          // 4-aligned BC height (4..64)
    uint16_t content_width = 0;   // Unpadded content width (1..64)
    uint16_t content_height = 0;  // Unpadded content height (1..64)
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint8_t format = kMicroThumbFormatBc1;
    std::vector<uint8_t> blocks;
};

namespace bc_detail {

inline uint16_t PackRgb565(uint8_t r, uint8_t g, uint8_t b) noexcept {
    const uint16_t r5 = static_cast<uint16_t>((uint32_t(r) * 31u + 127u) / 255u);
    const uint16_t g6 = static_cast<uint16_t>((uint32_t(g) * 63u + 127u) / 255u);
    const uint16_t b5 = static_cast<uint16_t>((uint32_t(b) * 31u + 127u) / 255u);
    return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

inline void UnpackRgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) noexcept {
    const uint32_t r5 = (c >> 11) & 31u;
    const uint32_t g6 = (c >> 5) & 63u;
    const uint32_t b5 = c & 31u;
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

inline uint32_t ColorDistSq(uint8_t b0, uint8_t g0, uint8_t r0, uint8_t b1, uint8_t g1, uint8_t r1) noexcept {
    const int32_t db = int32_t(b0) - int32_t(b1);
    const int32_t dg = int32_t(g0) - int32_t(g1);
    const int32_t dr = int32_t(r0) - int32_t(r1);
    return static_cast<uint32_t>(db * db + dg * dg + dr * dr);
}

inline void EncodeBc1ColorBlock(const uint8_t block_bgra[16][4], bool transparent_mode, uint8_t out[8]) noexcept {
    uint8_t min_b = 255, min_g = 255, min_r = 255;
    uint8_t max_b = 0, max_g = 0, max_r = 0;
    uint32_t opaque_count = 0;
    for (int i = 0; i < 16; ++i) {
        if (transparent_mode && block_bgra[i][3] < 128u) continue;
        ++opaque_count;
        min_b = (std::min)(min_b, block_bgra[i][0]);
        min_g = (std::min)(min_g, block_bgra[i][1]);
        min_r = (std::min)(min_r, block_bgra[i][2]);
        max_b = (std::max)(max_b, block_bgra[i][0]);
        max_g = (std::max)(max_g, block_bgra[i][1]);
        max_r = (std::max)(max_r, block_bgra[i][2]);
    }
    if (transparent_mode && opaque_count == 0) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        out[4] = 0xFFu;
        out[5] = 0xFFu;
        out[6] = 0xFFu;
        out[7] = 0xFFu;
        return;
    }

    // Project opaque pixels onto the block's dominant color axis so anti-correlated
    // chroma edges (e.g. red<->blue) keep accurate endpoints instead of bbox corners.
    int32_t sum_b = 0, sum_g = 0, sum_r = 0;
    for (int i = 0; i < 16; ++i) {
        if (transparent_mode && block_bgra[i][3] < 128u) continue;
        sum_b += block_bgra[i][0];
        sum_g += block_bgra[i][1];
        sum_r += block_bgra[i][2];
    }
    const int32_t mean_b = sum_b / static_cast<int32_t>(opaque_count);
    const int32_t mean_g = sum_g / static_cast<int32_t>(opaque_count);
    const int32_t mean_r = sum_r / static_cast<int32_t>(opaque_count);

    int32_t cov_bb = 0, cov_gg = 0, cov_rr = 0, cov_bg = 0, cov_br = 0, cov_gr = 0;
    for (int i = 0; i < 16; ++i) {
        if (transparent_mode && block_bgra[i][3] < 128u) continue;
        const int32_t db = int32_t(block_bgra[i][0]) - mean_b;
        const int32_t dg = int32_t(block_bgra[i][1]) - mean_g;
        const int32_t dr = int32_t(block_bgra[i][2]) - mean_r;
        cov_bb += db * db;
        cov_gg += dg * dg;
        cov_rr += dr * dr;
        cov_bg += db * dg;
        cov_br += db * dr;
        cov_gr += dg * dr;
    }
    int32_t ax_b = (int32_t(max_b) - int32_t(min_b)) << 2;
    int32_t ax_g = (int32_t(max_g) - int32_t(min_g)) << 2;
    int32_t ax_r = (int32_t(max_r) - int32_t(min_r)) << 2;
    if (ax_b == 0 && ax_g == 0 && ax_r == 0) ax_g = 1024;
    for (int iter = 0; iter < 4; ++iter) {
        const int64_t nb = int64_t(cov_bb) * ax_b + int64_t(cov_bg) * ax_g + int64_t(cov_br) * ax_r;
        const int64_t ng = int64_t(cov_bg) * ax_b + int64_t(cov_gg) * ax_g + int64_t(cov_gr) * ax_r;
        const int64_t nr = int64_t(cov_br) * ax_b + int64_t(cov_gr) * ax_g + int64_t(cov_rr) * ax_r;
        const int64_t ab = nb < 0 ? -nb : nb;
        const int64_t ag = ng < 0 ? -ng : ng;
        const int64_t ar = nr < 0 ? -nr : nr;
        const int64_t mag = (std::max)({ab, ag, ar});
        if (mag == 0) break;
        ax_b = static_cast<int32_t>((nb * 1024) / mag);
        ax_g = static_cast<int32_t>((ng * 1024) / mag);
        ax_r = static_cast<int32_t>((nr * 1024) / mag);
    }

    int lo_idx = -1, hi_idx = -1;
    int32_t lo_proj = INT32_MAX, hi_proj = INT32_MIN;
    for (int i = 0; i < 16; ++i) {
        if (transparent_mode && block_bgra[i][3] < 128u) continue;
        const int32_t proj = (int32_t(block_bgra[i][0]) - mean_b) * ax_b +
                             (int32_t(block_bgra[i][1]) - mean_g) * ax_g +
                             (int32_t(block_bgra[i][2]) - mean_r) * ax_r;
        if (proj < lo_proj) {
            lo_proj = proj;
            lo_idx = i;
        }
        if (proj > hi_proj) {
            hi_proj = proj;
            hi_idx = i;
        }
    }
    uint8_t ep0_b = max_b, ep0_g = max_g, ep0_r = max_r;
    uint8_t ep1_b = min_b, ep1_g = min_g, ep1_r = min_r;
    if (lo_idx >= 0 && hi_idx >= 0) {
        ep0_b = block_bgra[hi_idx][0];
        ep0_g = block_bgra[hi_idx][1];
        ep0_r = block_bgra[hi_idx][2];
        ep1_b = block_bgra[lo_idx][0];
        ep1_g = block_bgra[lo_idx][1];
        ep1_r = block_bgra[lo_idx][2];
    }

    uint16_t c0 = PackRgb565(ep0_r, ep0_g, ep0_b);
    uint16_t c1 = PackRgb565(ep1_r, ep1_g, ep1_b);
    if (transparent_mode) {
        if (c0 > c1) std::swap(c0, c1);
    } else {
        if (c0 < c1) std::swap(c0, c1);
    }

    uint8_t pal[4][3]{};
    UnpackRgb565(c0, pal[0][2], pal[0][1], pal[0][0]);
    UnpackRgb565(c1, pal[1][2], pal[1][1], pal[1][0]);

    uint32_t indices = 0;
    if (transparent_mode) {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = static_cast<uint8_t>((uint32_t(pal[0][ch]) + uint32_t(pal[1][ch])) / 2u);
        }
        for (int i = 0; i < 16; ++i) {
            uint32_t code = 3u;
            if (block_bgra[i][3] >= 128u) {
                uint32_t best = UINT32_MAX;
                for (uint32_t k = 0; k < 3u; ++k) {
                    const uint32_t d = ColorDistSq(block_bgra[i][0], block_bgra[i][1], block_bgra[i][2],
                                                   pal[k][0], pal[k][1], pal[k][2]);
                    if (d < best) {
                        best = d;
                        code = k;
                    }
                }
            }
            indices |= (code << (i * 2));
        }
    } else if (c0 == c1) {
        indices = 0u;
    } else {
        for (int ch = 0; ch < 3; ++ch) {
            pal[2][ch] = static_cast<uint8_t>((2u * uint32_t(pal[0][ch]) + uint32_t(pal[1][ch]) + 1u) / 3u);
            pal[3][ch] = static_cast<uint8_t>((uint32_t(pal[0][ch]) + 2u * uint32_t(pal[1][ch]) + 1u) / 3u);
        }
        for (int i = 0; i < 16; ++i) {
            uint32_t best = UINT32_MAX;
            uint32_t code = 0;
            for (uint32_t k = 0; k < 4u; ++k) {
                const uint32_t d = ColorDistSq(block_bgra[i][0], block_bgra[i][1], block_bgra[i][2],
                                               pal[k][0], pal[k][1], pal[k][2]);
                if (d < best) {
                    best = d;
                    code = k;
                }
            }
            indices |= (code << (i * 2));
        }
    }

    out[0] = static_cast<uint8_t>(c0 & 0xFFu);
    out[1] = static_cast<uint8_t>((c0 >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>(c1 & 0xFFu);
    out[3] = static_cast<uint8_t>((c1 >> 8) & 0xFFu);
    out[4] = static_cast<uint8_t>(indices & 0xFFu);
    out[5] = static_cast<uint8_t>((indices >> 8) & 0xFFu);
    out[6] = static_cast<uint8_t>((indices >> 16) & 0xFFu);
    out[7] = static_cast<uint8_t>((indices >> 24) & 0xFFu);
}

inline void EncodeBc3AlphaBlock(const uint8_t block_bgra[16][4], uint8_t out[8]) noexcept {
    uint8_t min_a = 255u;
    uint8_t max_a = 0u;
    for (int i = 0; i < 16; ++i) {
        min_a = (std::min)(min_a, block_bgra[i][3]);
        max_a = (std::max)(max_a, block_bgra[i][3]);
    }
    out[0] = max_a;
    out[1] = min_a;
    if (max_a == min_a) {
        std::memset(out + 2, 0, 6);
        return;
    }
    uint8_t apal[8]{};
    apal[0] = max_a;
    apal[1] = min_a;
    for (uint32_t k = 1; k <= 6u; ++k) {
        apal[k + 1u] = static_cast<uint8_t>(((7u - k) * uint32_t(max_a) + k * uint32_t(min_a) + 3u) / 7u);
    }
    uint64_t idx = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t a = block_bgra[i][3];
        uint32_t best_d = 256u;
        uint32_t code = 0u;
        for (uint32_t k = 0; k < 8u; ++k) {
            const uint32_t d = static_cast<uint32_t>(std::abs(int32_t(a) - int32_t(apal[k])));
            if (d < best_d) {
                best_d = d;
                code = k;
            }
        }
        idx |= (uint64_t(code) << (i * 3));
    }
    for (int b = 0; b < 6; ++b) {
        out[2 + b] = static_cast<uint8_t>((idx >> (b * 8)) & 0xFFu);
    }
}

}  // namespace bc_detail

inline bool EncodeMicroThumb(const uint8_t* src_pbgra,
                             uint32_t src_w,
                             uint32_t src_h,
                             uint32_t src_stride,
                             uint32_t source_w,
                             uint32_t source_h,
                             uint32_t max_edge,
                             MicroThumbData& out) noexcept {
    if (!src_pbgra || src_w == 0 || src_h == 0 || src_stride < src_w * 4u) return false;
    const uint32_t cap = (std::max)(4u, (std::min)(max_edge, kMicroThumbMaxEdge));
    const uint32_t longest = (std::max)(src_w, src_h);
    uint32_t cw = src_w;
    uint32_t ch = src_h;
    if (longest > cap) {
        const double ratio = static_cast<double>(cap) / longest;
        cw = static_cast<uint32_t>((std::max)(1l, std::lround(src_w * ratio)));
        ch = static_cast<uint32_t>((std::max)(1l, std::lround(src_h * ratio)));
        cw = (std::min)(cw, cap);
        ch = (std::min)(ch, cap);
    }
    thread_local std::vector<uint8_t> downsampled_buf;
    downsampled_buf.clear();
    const uint8_t* micro_ptr = src_pbgra;
    uint32_t micro_stride = src_stride;
    if (cw != src_w || ch != src_h) {
        if (!pulse::preview::DownsamplePbgraExactArea(src_pbgra, src_w, src_h, src_stride, cw, ch, downsampled_buf)) {
            return false;
        }
        micro_ptr = downsampled_buf.data();
        micro_stride = cw * 4u;
    }

    bool smooth_alpha = false;
    bool punch_alpha = false;
    for (uint32_t y = 0; y < ch; ++y) {
        const uint8_t* row = micro_ptr + size_t(y) * micro_stride;
        for (uint32_t x = 0; x < cw; ++x) {
            const uint8_t a = row[x * 4u + 3u];
            if (a >= 16u && a <= 239u) {
                smooth_alpha = true;
                break;
            }
            if (a < 128u) punch_alpha = true;
        }
        if (smooth_alpha) break;
    }

    const uint32_t pw = (cw + 3u) & ~3u;
    const uint32_t ph = (ch + 3u) & ~3u;
    const uint32_t bx_count = pw / 4u;
    const uint32_t by_count = ph / 4u;
    const uint8_t format = smooth_alpha ? kMicroThumbFormatBc3 : kMicroThumbFormatBc1;
    const uint32_t block_size = format == kMicroThumbFormatBc3 ? 16u : 8u;

    out.width = static_cast<uint16_t>(pw);
    out.height = static_cast<uint16_t>(ph);
    out.content_width = static_cast<uint16_t>(cw);
    out.content_height = static_cast<uint16_t>(ch);
    out.source_width = source_w ? source_w : src_w;
    out.source_height = source_h ? source_h : src_h;
    out.format = format;
    out.blocks.resize(size_t(bx_count) * by_count * block_size);

    uint8_t* dst = out.blocks.data();
    for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
            uint8_t block[16][4]{};
            bool block_transparent = false;
            for (uint32_t iy = 0; iy < 4u; ++iy) {
                const uint32_t sy = (std::min)(by * 4u + iy, ch - 1u);
                const uint8_t* row = micro_ptr + size_t(sy) * micro_stride;
                for (uint32_t ix = 0; ix < 4u; ++ix) {
                    const uint32_t sx = (std::min)(bx * 4u + ix, cw - 1u);
                    const uint8_t* px = row + sx * 4u;
                    const uint32_t idx = iy * 4u + ix;
                    block[idx][0] = px[0];
                    block[idx][1] = px[1];
                    block[idx][2] = px[2];
                    block[idx][3] = px[3];
                    if (px[3] < 128u) block_transparent = true;
                }
            }
            if (format == kMicroThumbFormatBc3) {
                bc_detail::EncodeBc3AlphaBlock(block, dst);
                bc_detail::EncodeBc1ColorBlock(block, false, dst + 8);
                dst += 16;
            } else {
                bc_detail::EncodeBc1ColorBlock(block, punch_alpha && block_transparent, dst);
                dst += 8;
            }
        }
    }
    return true;
}

inline bool DecodeMicroThumbToPbgra(const MicroThumbData& in,
                                    std::vector<uint8_t>& out_pbgra,
                                    uint32_t& out_w,
                                    uint32_t& out_h,
                                    uint32_t& out_stride) noexcept {
    if (in.width == 0 || in.height == 0 || in.content_width == 0 || in.content_height == 0 ||
        (in.width & 3u) != 0 || (in.height & 3u) != 0 ||
        in.content_width > in.width || in.content_height > in.height) {
        return false;
    }
    if (in.format != kMicroThumbFormatBc1 && in.format != kMicroThumbFormatBc3) return false;
    const uint32_t bx_count = in.width / 4u;
    const uint32_t by_count = in.height / 4u;
    const uint32_t block_size = in.format == kMicroThumbFormatBc3 ? 16u : 8u;
    if (in.blocks.size() != size_t(bx_count) * by_count * block_size) return false;

    out_w = in.content_width;
    out_h = in.content_height;
    out_stride = out_w * 4u;
    out_pbgra.resize(size_t(out_stride) * out_h);

    const uint8_t* src = in.blocks.data();
    for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
            uint8_t apal[8]{};
            uint64_t aidx = 0;
            const uint8_t* color_blk = src;
            if (in.format == kMicroThumbFormatBc3) {
                apal[0] = src[0];
                apal[1] = src[1];
                if (apal[0] > apal[1]) {
                    for (uint32_t k = 1; k <= 6u; ++k) {
                        apal[k + 1u] = static_cast<uint8_t>(((7u - k) * uint32_t(apal[0]) + k * uint32_t(apal[1]) + 3u) / 7u);
                    }
                } else {
                    for (uint32_t k = 1; k <= 4u; ++k) {
                        apal[k + 1u] = static_cast<uint8_t>(((5u - k) * uint32_t(apal[0]) + k * uint32_t(apal[1]) + 2u) / 5u);
                    }
                    apal[6] = 0u;
                    apal[7] = 255u;
                }
                for (int b = 0; b < 6; ++b) {
                    aidx |= (uint64_t(src[2 + b]) << (b * 8));
                }
                color_blk = src + 8;
                src += 16;
            } else {
                src += 8;
            }

            const uint16_t c0 = static_cast<uint16_t>(color_blk[0] | (uint16_t(color_blk[1]) << 8));
            const uint16_t c1 = static_cast<uint16_t>(color_blk[2] | (uint16_t(color_blk[3]) << 8));
            const uint32_t cidx = static_cast<uint32_t>(color_blk[4]) |
                                  (static_cast<uint32_t>(color_blk[5]) << 8) |
                                  (static_cast<uint32_t>(color_blk[6]) << 16) |
                                  (static_cast<uint32_t>(color_blk[7]) << 24);
            uint8_t cpal[4][4]{};
            bc_detail::UnpackRgb565(c0, cpal[0][2], cpal[0][1], cpal[0][0]);
            bc_detail::UnpackRgb565(c1, cpal[1][2], cpal[1][1], cpal[1][0]);
            cpal[0][3] = 255u;
            cpal[1][3] = 255u;
            if (in.format == kMicroThumbFormatBc3 || c0 > c1) {
                for (int ch = 0; ch < 3; ++ch) {
                    cpal[2][ch] = static_cast<uint8_t>((2u * uint32_t(cpal[0][ch]) + uint32_t(cpal[1][ch]) + 1u) / 3u);
                    cpal[3][ch] = static_cast<uint8_t>((uint32_t(cpal[0][ch]) + 2u * uint32_t(cpal[1][ch]) + 1u) / 3u);
                }
                cpal[2][3] = 255u;
                cpal[3][3] = 255u;
            } else {
                for (int ch = 0; ch < 3; ++ch) {
                    cpal[2][ch] = static_cast<uint8_t>((uint32_t(cpal[0][ch]) + uint32_t(cpal[1][ch])) / 2u);
                }
                cpal[2][3] = 255u;
                cpal[3][0] = 0u;
                cpal[3][1] = 0u;
                cpal[3][2] = 0u;
                cpal[3][3] = 0u;
            }

            for (uint32_t iy = 0; iy < 4u; ++iy) {
                const uint32_t dy = by * 4u + iy;
                if (dy >= out_h) continue;
                uint8_t* dst_row = out_pbgra.data() + size_t(dy) * out_stride;
                for (uint32_t ix = 0; ix < 4u; ++ix) {
                    const uint32_t dx = bx * 4u + ix;
                    if (dx >= out_w) continue;
                    const uint32_t pix = iy * 4u + ix;
                    const uint32_t code = (cidx >> (pix * 2u)) & 3u;
                    uint8_t* out_px = dst_row + dx * 4u;
                    out_px[0] = cpal[code][0];
                    out_px[1] = cpal[code][1];
                    out_px[2] = cpal[code][2];
                    if (in.format == kMicroThumbFormatBc3) {
                        const uint8_t a = apal[(aidx >> (pix * 3u)) & 7u];
                        out_px[3] = a;
                        out_px[0] = (std::min)(out_px[0], a);
                        out_px[1] = (std::min)(out_px[1], a);
                        out_px[2] = (std::min)(out_px[2], a);
                    } else {
                        out_px[3] = cpal[code][3];
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace pulse::ui
