/**
 * @file SimdUtils.h
 * @brief SIMD/AVX2 accelerated audio processing utilities
 */

#ifndef SIMD_UTILS_H
#define SIMD_UTILS_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm> // for std::min

// 编译时检测 AVX2 支持
#if defined(__x86_64__) || defined(_M_X64)
    // 检查是否启用了 AVX2 编译选项
    #if defined(__AVX2__)
        #define HAS_AVX2 1
        #include <immintrin.h>
    #else
        #define HAS_AVX2 0
    #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define HAS_NEON 1
    #define HAS_AVX2 0
#else
    #define HAS_AVX2 0
    #define HAS_NEON 0
#endif

namespace SimdUtils {

/**
 * @brief Runtime CPU feature detection
 */
static int g_avx512_checked = 0;
static int g_has_avx512 = 0;

static inline int detect_avx512(void) {
    if (!g_avx512_checked) {
        g_avx512_checked = 1;
#if HAS_AVX2
#ifdef __GNUC__
        __builtin_cpu_init();
        g_has_avx512 = __builtin_cpu_supports("avx512f") &&
                       __builtin_cpu_supports("avx512bw");
#endif
#endif
    }
    return g_has_avx512;
}

/**
 * @brief Prefetch audio buffer for upcoming processing
 */
static inline void prefetch(const void* src, size_t size) {
#if HAS_AVX2
    const char* p = static_cast<const char*>(src);
    _mm_prefetch(p, _MM_HINT_T0);
    if (size > 64) _mm_prefetch(p + 64, _MM_HINT_T0);
#endif
}

#if HAS_AVX2
/**
 * @brief Parallel bit reversal for 32 bytes using AVX2
 */
static inline __m256i bit_reverse_avx2(__m256i x) {
    // 4-bit nibble reverse LUT
    static const __m256i nibble_reverse = _mm256_setr_epi8(
        0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
        0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF,
        0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
        0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
    );

    __m256i mask_0f = _mm256_set1_epi8(0x0F);
    __m256i lo_nibbles = _mm256_and_si256(x, mask_0f);
    __m256i hi_nibbles = _mm256_and_si256(_mm256_srli_epi16(x, 4), mask_0f);

    __m256i lo_reversed = _mm256_shuffle_epi8(nibble_reverse, lo_nibbles);
    __m256i hi_reversed = _mm256_shuffle_epi8(nibble_reverse, hi_nibbles);

    // Combine: (lo_reversed << 4) | hi_reversed
    return _mm256_or_si256(_mm256_slli_epi16(lo_reversed, 4), hi_reversed);
}

/**
 * @brief Parallel byte swap (16-bit word swap) using AVX2
 * Used for DSD Little-Endian vs Big-Endian conversion (DSF vs DFF)
 */
static inline __m256i bswap_avx2(__m256i x) {
    // Shuffle mask to swap adjacent bytes: 1,0, 3,2, 5,4, ...
    static const __m256i shuffle_mask = _mm256_setr_epi8(
        1, 0, 3, 2, 5, 4, 7, 6,
        9, 8, 11, 10, 13, 12, 15, 14,
        1, 0, 3, 2, 5, 4, 7, 6,
        9, 8, 11, 10, 13, 12, 15, 14
    );
    return _mm256_shuffle_epi8(x, shuffle_mask);
}

/**
 * @brief AVX2 Stream Copy (Non-Temporal Store)
 * Bypasses cache for writes to avoid polluting L3 cache with transient audio data
 */
static inline void memcpy_stream_avx2(void* dst, const void* src, size_t size) {
    // Check if AVX2 is available at runtime
    static int avx2_checked = 0;
    static int has_avx2 = 0;
    
    if (!avx2_checked) {
        avx2_checked = 1;
#ifdef __GNUC__
        __builtin_cpu_init();
        has_avx2 = __builtin_cpu_supports("avx2");
#endif
    }
    
    if (!has_avx2) {
        // Fallback to standard memcpy if AVX2 is not available
        std::memcpy(dst, src, size);
        return;
    }
    
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    // Initial alignment (to 32 bytes)
    while (((uintptr_t)d & 31) && size > 0) {
        *d++ = *s++;
        size--;
    }

    // Main loop with stream stores
    while (size >= 32) {
        __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(d), data);
        s += 32;
        d += 32;
        size -= 32;
    }

    // Fence needed after NT stores if data is read back immediately (usually not needed for network send)
    // _mm_sfence(); 

    // Remainder
    while (size > 0) {
        *d++ = *s++;
        size--;
    }
}

#endif

/**
 * @brief Pre-calculated bit reversal lookup table (scalar fallback)
 */
static constexpr uint8_t kBitReverseTable[256] = {
    0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
    0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
    0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,
    0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
    0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,
    0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
    0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,
    0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
    0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,
    0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
    0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,
    0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
    0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,
    0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
    0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,
    0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF
};

/**
 * @brief Auto-selecting memory copy
 * Uses stream copy for large blocks to spare cache, standard copy for small ones.
 */
static inline void memcpy_fixed(void* dst, const void* src, size_t size) {
#if HAS_AVX2
    // Threshold for non-temporal stores
    // 64KB is roughly L1 cache size; anything larger shouldn't pollute L2/L3
    if (size >= 65536) { 
        memcpy_stream_avx2(dst, src, size);
    } else {
        // Cached copy for small blocks (stay with standard libc memcpy or AVX load/store)
        std::memcpy(dst, src, size);
    }
#else
    std::memcpy(dst, src, size);
#endif
}

/**
 * @brief Audio-specific memory copy with fixed timing
 * Uses overlapping stores for tail handling to eliminate timing variance
 */
static inline void memcpy_audio_fixed(void* dst, const void* src, size_t size) {
#if HAS_AVX2
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);

    while (size >= 128) {
        __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 0));
        __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i r2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 64));
        __m256i r3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 96));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 0), r0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32), r1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 64), r2);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 96), r3);
        s += 128;
        d += 128;
        size -= 128;
    }

    if (size >= 64) {
        __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + 32));
        __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 64));
        __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), a0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + 32), a1);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 64), b0);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 32), b1);
    } else if (size >= 32) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s + size - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d), a);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(d + size - 32), b);
    } else if (size >= 16) {
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + size - 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d + size - 16), b);
    } else if (size >= 8) {
        uint64_t a;
        uint64_t b;
        std::memcpy(&a, s, 8);
        std::memcpy(&b, s + size - 8, 8);
        std::memcpy(d, &a, 8);
        std::memcpy(d + size - 8, &b, 8);
    } else if (size >= 4) {
        uint32_t a;
        uint32_t b;
        std::memcpy(&a, s, 4);
        std::memcpy(&b, s + size - 4, 4);
        std::memcpy(d, &a, 4);
        std::memcpy(d + size - 4, &b, 4);
    } else if (size > 0) {
        d[0] = s[0];
        if (size > 1) d[size - 1] = s[size - 1];
        if (size > 2) d[1] = s[1];
    }

    _mm256_zeroupper();
#else
    std::memcpy(dst, src, size);
#endif
}

/**
 * @brief Optimized prefetch for audio buffers
 * Tuned for 180-1500 byte buffers
 */
static inline void prefetch_audio(const void* src, size_t size) {
#if HAS_AVX2
    const char* p = static_cast<const char*>(src);

    _mm_prefetch(p, _MM_HINT_T0);

    if (size > 256) {
        _mm_prefetch(p + 64, _MM_HINT_T0);
    }
    if (size > 512) {
        _mm_prefetch(p + size - 64, _MM_HINT_T0);
    }
#elif HAS_NEON
    const char* p = static_cast<const char*>(src);
    // ARM64 prefetch implementation
    __builtin_prefetch(p, 0, 0);
    if (size > 256) {
        __builtin_prefetch(p + 64, 0, 0);
    }
    if (size > 512) {
        __builtin_prefetch(p + size - 64, 0, 0);
    }
#endif
}

} // namespace SimdUtils

#endif // SIMD_UTILS_H
