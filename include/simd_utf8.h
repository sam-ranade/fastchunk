#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fastchunk
{

class SimdUtf8Validator
{
public:
    // Returns true if the buffer contains valid UTF-8.
    // If invalid, sets error_offset to the byte index of the first malformed
    // byte.
    static bool validate(const std::byte* data, std::size_t size,
        std::size_t& error_offset) noexcept
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(data);
        std::size_t i = 0;

#if defined(__AVX2__)
        // Vectorized ASCII Fast-Path (AVX2 - 32 Bytes per iteration)
        const __m256i ascii_mask = _mm256_set1_epi8(static_cast<char>(0x80));
        for (; i + 32 <= size; i += 32)
        {
            __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(bytes + i));
            // If any bit 0x80 is set, we encountered non-ASCII characters
            if (_mm256_movemask_epi8(_mm256_and_si256(chunk, ascii_mask)) != 0)
            {
                break; // Fall back to precise scalar validation for non-ASCII block
            }
        }
#elif defined(__ARM_NEON)
        // Vectorized ASCII Fast-Path (ARM NEON - 16 Bytes per iteration)
        const uint8x16_t ascii_mask = vdupq_n_u8(0x80);
        for (; i + 16 <= size; i += 16)
        {
            uint8x16_t chunk = vld1q_u8(bytes + i);
            uint8x16_t tested = vtstq_u8(chunk, ascii_mask);
            if (vmaxvq_u32(freinterpretq_u32_u8(tested)) != 0)
            {
                break; // Non-ASCII block detected
            }
        }
#endif

        // Precise UTF-8 State Machine Validation Loop
        while (i < size)
        {
            std::size_t advance = 0;
            if (!validate_utf8_sequence(bytes + i, size - i, advance))
            {
                error_offset = i;
                return false;
            }
            i += advance;
        }

        return true;
    }

private:
    static inline bool validate_utf8_sequence(const unsigned char* bytes,
        std::size_t remaining,
        std::size_t& advance) noexcept
    {
        if (remaining == 0)
            return false;

        // 1-Byte ASCII
        if (bytes[0] <= 0x7F)
        {
            advance = 1;
            return true;
        }
        // 2-Byte Sequence
        if ((bytes[0] & 0xE0) == 0xC0)
        {
            if (remaining >= 2 && (bytes[1] & 0xC0) == 0x80 && bytes[0] >= 0xC2)
            {
                advance = 2;
                return true;
            }
        }
        // 3-Byte Sequence
        else if ((bytes[0] & 0xF0) == 0xE0)
        {
            if (remaining >= 3 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80)
            {
                // Prevent surrogate halves (0xD800-0xDFFF)
                if (bytes[0] == 0xED && bytes[1] >= 0xA0)
                    return false;
                advance = 3;
                return true;
            }
        }
        // 4-Byte Sequence
        else if ((bytes[0] & 0xF8) == 0xF0)
        {
            if (remaining >= 4 && (bytes[1] & 0xC0) == 0x80 && (bytes[2] & 0xC0) == 0x80 && (bytes[3] & 0xC0) == 0x80)
            {
                // Range check U+10FFFF
                if (bytes[0] > 0xF4 || (bytes[0] == 0xF4 && bytes[1] > 0x8F))
                    return false;
                advance = 4;
                return true;
            }
        }

        return false;
    }
};

} // namespace fastchunk
