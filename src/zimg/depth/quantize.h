#pragma once

#ifndef ZIMG_DEPTH_QUANTIZE_H_
#define ZIMG_DEPTH_QUANTIZE_H_

#include <cstdint>
#include <utility>
#include "common/pixel.h"

namespace zimg::depth {

constexpr int32_t numeric_max(int bits) noexcept
{
	return (1L << bits) - 1;
}

constexpr int32_t integer_offset(const PixelFormat &format) noexcept
{
	return pixel_is_float(format.type) ? 0
		: format.chroma ? 1L << (format.depth - 1)
		: !format.fullrange ? 16L << (format.depth - 8)
		: 0;
}

constexpr int32_t integer_range(const PixelFormat &format) noexcept
{
	return pixel_is_float(format.type) ? 1
		: format.fullrange ? numeric_max(format.depth)
		: format.chroma && !format.ycgco ? 224L << (format.depth - 8)
		: 219L << (format.depth - 8);
}

inline std::pair<float, float> get_scale_offset(const PixelFormat &pixel_in, const PixelFormat &pixel_out)
{
	double range_in = integer_range(pixel_in);
	double offset_in = integer_offset(pixel_in);
	double range_out = integer_range(pixel_out);
	double offset_out = integer_offset(pixel_out);

	float scale = static_cast<float>(range_out / range_in);
	float offset = static_cast<float>(-offset_in * range_out / range_in + offset_out);

	return{ scale, offset };
}

// Integer-domain scaling for full-range formats (zero offset).
//
// Widening uses bit replication, so the full-scale value of the source maps
// to the full-scale value of the destination. The source pattern is placed in
// the high bits and cycled into the remaining low bits; for n <= 2*b the
// loop reduces to the shift-and-OR form
//   r = (x << (n - b)) | (x >> (2*b - n)).
//
// Narrowing scales by (2^n - 1)/(2^b - 1) and rounds to nearest, ties to even:
//   r = RNE(x * (2^n - 1) / (2^b - 1)), clamped to the destination range.
// All arithmetic is integral, so the result does not depend on floating-point
// rounding modes or intermediate precision.
uint32_t integer_scale_fullrange(uint32_t x, unsigned depth_in, unsigned depth_out) noexcept;

// Integer-domain scaling for limited-range (video) formats.
//
// Widening is an exact left shift:  r = x << k,  k = n - b.
// Narrowing inverts the shift using round-to-nearest, ties to even:
//   r = RNE(x / 2^k), clamped to the destination range.
uint32_t integer_scale_shift(uint32_t x, unsigned depth_in, unsigned depth_out) noexcept;

float half_to_float(uint16_t f16w) noexcept;

uint16_t float_to_half(float f32) noexcept;

} // namespace zimg::depth

#endif // ZIMG_DEPTH_QUANTIZE_H_
