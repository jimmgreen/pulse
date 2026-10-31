#pragma once
#include <cstddef>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace pulse::preview {
inline void BlendPbgraPixel(uint8_t* dst, const uint8_t* src) noexcept {
    const uint32_t alpha = src[3];
    if (alpha == 255) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        return;
    }
    if (alpha == 0) return;
    const uint32_t inverse = 255u - alpha;
    for (size_t c = 0; c < 4; ++c)
        dst[c] = static_cast<uint8_t>(src[c] + (dst[c] * inverse + 127u) / 255u);
}

// Source-over, exactly matching the integer PBGRA decoder convention. Buffers
// need not be aligned; each span stays within its row (including a short tail).
inline void BlendPbgraSpan(uint8_t* dst, const uint8_t* src, size_t pixels) noexcept {
#if defined(_M_X64) || defined(__x86_64__)
    const __m128i zero = _mm_setzero_si128();
    const __m128i full = _mm_set1_epi16(255);
    const __m128i rounding = _mm_set1_epi16(128);
    const auto blend = [&](const __m128i source, const __m128i target) {
        const __m128i alpha = _mm_shufflehi_epi16(_mm_shufflelo_epi16(source, _MM_SHUFFLE(3,3,3,3)), _MM_SHUFFLE(3,3,3,3));
        const __m128i product = _mm_mullo_epi16(target, _mm_sub_epi16(full, alpha));
        // For 0 <= x <= 65025: floor((x+127)/255) == (t+(t>>8))>>8, t=x+128.
        const __m128i t = _mm_add_epi16(product, rounding);
        const __m128i scaled = _mm_srli_epi16(_mm_add_epi16(t, _mm_srli_epi16(t, 8)), 8);
        const __m128i result = _mm_and_si128(_mm_add_epi16(source, scaled), full);
        const __m128i transparent = _mm_cmpeq_epi16(alpha, zero);
        return _mm_or_si128(_mm_and_si128(transparent, target), _mm_andnot_si128(transparent, result));
    };
    while (pixels >= 4) {
        const __m128i source = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        const __m128i alpha = _mm_srli_epi32(source, 24);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(alpha, zero)) != 0xffff) {
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(alpha, _mm_set1_epi32(255))) == 0xffff) {
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), source);
            } else {
                const __m128i target = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst));
                const __m128i low = blend(_mm_unpacklo_epi8(source, zero), _mm_unpacklo_epi8(target, zero));
                const __m128i high = blend(_mm_unpackhi_epi8(source, zero), _mm_unpackhi_epi8(target, zero));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), _mm_packus_epi16(low, high));
            }
        }
        src += 16; dst += 16; pixels -= 4;
    }
#endif
    while (pixels--) {
        BlendPbgraPixel(dst, src);
        src += 4; dst += 4;
    }
}
} // namespace pulse::preview
