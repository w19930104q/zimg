#include <algorithm>
#include <type_traits>
#include "quantize.h"

namespace zimg::depth {

namespace {

template <class T, class U>
T bit_cast(const U &x) noexcept
{
	static_assert(sizeof(T) == sizeof(U), "object sizes must match");
	static_assert(std::is_pod_v<T> && std::is_pod_v<U>, "object types must be POD");

	T ret;
	std::copy_n(reinterpret_cast<const char *>(&x), sizeof(x), reinterpret_cast<char *>(&ret));
	return ret;
}

} // namespace


uint32_t integer_scale_fullrange(uint32_t x, unsigned depth_in, unsigned depth_out) noexcept
{
	if (depth_out > depth_in) {
		// Bit replication: place x in the high bits and fill the remaining
		// low bits by cycling through the source bits from MSB to LSB.
		// For n <= 2*b (every depth >= 8) the low part is a prefix of x, so
		// the loop reduces to (x << (n-b)) | (x >> (2*b-n)); for sub-8-bit
		// sources the source pattern is repeated as many times as needed,
		// so full scale still maps to full scale.
		uint32_t r = x << (depth_out - depth_in);
		unsigned fill = depth_out - depth_in;
		for (unsigned k = 0; k < fill; ++k) {
			unsigned src_bit = depth_in - 1 - (k % depth_in);
			if (x & (1U << src_bit))
				r |= 1U << (fill - 1 - k);
		}
		return r;
	} else if (depth_out < depth_in) {
		// Scale to the destination range, round half to even.
		uint64_t num = (static_cast<uint64_t>(1) << depth_out) - 1;
		uint64_t den = (static_cast<uint64_t>(1) << depth_in) - 1;
		uint64_t prod = static_cast<uint64_t>(x) * num;
		uint64_t q = prod / den;
		uint64_t rem = prod - q * den;
		uint64_t twice = rem * 2;

		if (twice > den || (twice == den && (q & 1)))
			++q;

		uint32_t max_out = static_cast<uint32_t>(num);
		return q > max_out ? max_out : static_cast<uint32_t>(q);
	} else {
		return x;
	}
}

uint32_t integer_scale_shift(uint32_t x, unsigned depth_in, unsigned depth_out) noexcept
{
	if (depth_out > depth_in) {
		return x << (depth_out - depth_in);
	} else if (depth_out < depth_in) {
		unsigned shift = depth_in - depth_out;
		uint32_t q = x >> shift;
		uint32_t rem = x & ((1U << shift) - 1);
		uint32_t half = 1U << (shift - 1);

		if (rem > half || (rem == half && (q & 1U)))
			++q;

		uint32_t max_out = (1U << depth_out) - 1;
		return q > max_out ? max_out : q;
	} else {
		return x;
	}
}


#define FLOAT_HALF_MANT_SHIFT (23 - 10)
#define FLOAT_HALF_EXP_ADJUST (127 - 15)
float half_to_float(uint16_t f16w) noexcept
{
	constexpr unsigned exp_nonfinite_f16 = 0x1F;
	constexpr unsigned exp_nonfinite_f32 = 0xFF;

	constexpr uint32_t mant_qnan_f32 = 0x00400000UL;

	uint16_t sign = (f16w & 0x8000U) >> 15;
	uint16_t exp = (f16w & 0x7C00U) >> 10;
	uint16_t mant = (f16w & 0x03FFU) >> 0;

	uint32_t f32dw;
	uint32_t exp_f32;
	uint32_t mant_f32;

	// Non-finite.
	if (exp == exp_nonfinite_f16) {
		exp_f32 = exp_nonfinite_f32;

		// Zero extend mantissa and convert sNaN to qNaN.
		if (mant)
			mant_f32 = (mant << FLOAT_HALF_MANT_SHIFT) | mant_qnan_f32;
		else
			mant_f32 = 0;
	} else {
		uint16_t mant_adjust;

		// Denormal.
		if (exp == 0) {
			// Special zero denorm.
			if (mant == 0) {
				mant_adjust = 0;
				exp_f32 = 0;
			} else {
				unsigned renorm = 0;
				mant_adjust = mant;

				while ((mant_adjust & 0x0400) == 0) {
					mant_adjust <<= 1;
					++renorm;
				}

				mant_adjust &= ~0x0400;
				exp_f32 = FLOAT_HALF_EXP_ADJUST - renorm + 1;
			}
		} else {
			mant_adjust = mant;
			exp_f32 = exp + FLOAT_HALF_EXP_ADJUST;
		}

		mant_f32 = static_cast<uint32_t>(mant_adjust) << FLOAT_HALF_MANT_SHIFT;
	}

	f32dw = (static_cast<uint32_t>(sign) << 31) | (exp_f32 << 23) | mant_f32;
	return bit_cast<float>(f32dw);
}

uint16_t float_to_half(float f32) noexcept
{
	constexpr unsigned exp_nonfinite_f32 = 0xFF;
	constexpr unsigned exp_nonfinite_f16 = 0x1F;

	constexpr unsigned mant_qnan_f16 = 0x0200;
	constexpr unsigned mant_max_f16 = 0x03FF;

	uint32_t f32dw = bit_cast<uint32_t>(f32);
	uint32_t sign = (f32dw & 0x80000000UL) >> 31;
	uint32_t exp = (f32dw & 0x7F800000UL) >> 23;
	uint32_t mant = (f32dw & 0x007FFFFFUL) >> 0;

	uint32_t exp_f16;
	uint32_t mant_f16;

	// Non-finite.
	if (exp == exp_nonfinite_f32) {
		exp_f16 = exp_nonfinite_f16;

		// Truncate mantissa and convert sNaN to qNaN.
		if (mant)
			mant_f16 = (mant >> FLOAT_HALF_MANT_SHIFT) | mant_qnan_f16;
		else
			mant_f16 = 0;
	} else {
		uint32_t mant_adjust;
		uint32_t shift;
		uint32_t half;

		// Denormal.
		if (exp <= FLOAT_HALF_EXP_ADJUST) {
			shift = FLOAT_HALF_MANT_SHIFT + FLOAT_HALF_EXP_ADJUST - exp + 1;

			if (shift > 31)
				shift = 31;

			mant_adjust = mant | (1UL << 23);
			exp_f16 = 0;
		} else {
			shift = FLOAT_HALF_MANT_SHIFT;
			mant_adjust = mant;
			exp_f16 = exp - FLOAT_HALF_EXP_ADJUST;
		}

		half = 1UL << (shift - 1);

		// Round half to even.
		mant_f16 = (mant_adjust + half - 1 + ((mant_adjust >> shift) & 1)) >> shift;

		// Detect overflow.
		if (mant_f16 > mant_max_f16) {
			mant_f16 &= mant_max_f16;
			exp_f16 += 1;
		}
		if (exp_f16 >= exp_nonfinite_f16) {
			exp_f16 = exp_nonfinite_f16;
			mant_f16 = 0;
		}
	}

	return (static_cast<uint16_t>(sign) << 15) | (static_cast<uint16_t>(exp_f16) << 10) | static_cast<uint16_t>(mant_f16);
}
#undef FLOAT_HALF_MANT_SHIFT
#undef FLOAT_HALF_EXP_ADJUST

} // namespace zimg::depth
