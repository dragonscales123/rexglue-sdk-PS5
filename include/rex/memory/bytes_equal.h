/**
 * Fast byte-range equality test.
 *
 * The PS5 libc memcmp compares roughly a byte per cycle, which made the hot
 * page and descriptor comparisons on the GPU command thread expensive. Only
 * equality is needed there, so this compares 32 (AVX2) or 16 (SSE2) bytes at
 * a time and stops at the first differing block.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#endif

namespace rex::memory {

inline bool BytesEqual(const void* a_ptr, const void* b_ptr, size_t size) {
  const uint8_t* a = static_cast<const uint8_t*>(a_ptr);
  const uint8_t* b = static_cast<const uint8_t*>(b_ptr);
#if defined(__AVX2__)
  while (size >= 128) {
    __m256i d0 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b)));
    __m256i d1 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 32)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 32)));
    __m256i d2 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 64)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 64)));
    __m256i d3 = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + 96)),
                                  _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + 96)));
    __m256i d = _mm256_or_si256(_mm256_or_si256(d0, d1), _mm256_or_si256(d2, d3));
    if (!_mm256_testz_si256(d, d)) {
      return false;
    }
    a += 128;
    b += 128;
    size -= 128;
  }
  while (size >= 32) {
    __m256i d = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)),
                                 _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b)));
    if (!_mm256_testz_si256(d, d)) {
      return false;
    }
    a += 32;
    b += 32;
    size -= 32;
  }
#elif defined(__SSE2__)
  while (size >= 16) {
    __m128i e = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a)),
                               _mm_loadu_si128(reinterpret_cast<const __m128i*>(b)));
    if (_mm_movemask_epi8(e) != 0xFFFF) {
      return false;
    }
    a += 16;
    b += 16;
    size -= 16;
  }
#endif
  while (size >= 8) {
    uint64_t x, y;
    std::memcpy(&x, a, 8);
    std::memcpy(&y, b, 8);
    if (x != y) {
      return false;
    }
    a += 8;
    b += 8;
    size -= 8;
  }
  while (size) {
    if (*a != *b) {
      return false;
    }
    ++a;
    ++b;
    --size;
  }
  return true;
}

}  // namespace rex::memory
