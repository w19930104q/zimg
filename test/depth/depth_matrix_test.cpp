#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "common/alloc.h"
#include "common/cpuinfo.h"
#include "common/pixel.h"
#include "depth/depth.h"
#include "depth/quantize.h"
#include "graphengine/filter.h"
#include "graphengine/types.h"

#include "gtest/gtest.h"

namespace {

using zimg::PixelType;
using zimg::PixelFormat;
using zimg::depth::DitherType;

// ---------------------------------------------------------------------------
// Minimal driver for the DepthConversion point filters.
// ---------------------------------------------------------------------------

struct PlaneBuf {
	// 64-byte aligned so the SIMD filters can use aligned loads, and padded
	// by one alignment block so tail handling may overwrite within the row.
	zimg::AlignedVector<uint8_t> data;
	unsigned n;
	unsigned bps;

	PlaneBuf() : n{}, bps{} {}
	PlaneBuf(unsigned count, unsigned bytes_per_sample) :
		data((size_t)count * bytes_per_sample + 64, 0), n(count), bps(bytes_per_sample)
	{}
};

graphengine::BufferDescriptor descriptor(const PlaneBuf &p)
{
	return graphengine::BufferDescriptor{ const_cast<uint8_t *>(p.data.data()),
	                                      (ptrdiff_t)(p.n * p.bps), graphengine::BUFFER_MAX };
}

graphengine::BufferDescriptor descriptor(PlaneBuf &p)
{
	return graphengine::BufferDescriptor{ p.data.data(), (ptrdiff_t)(p.n * p.bps),
	                                      graphengine::BUFFER_MAX };
}

// Allow the build variants to exercise the SIMD dispatch via ZIMG_TEST_CPU.
zimg::CPUClass test_cpu_class()
{
	static zimg::CPUClass cpu = []() {
		const char *e = std::getenv("ZIMG_TEST_CPU");
		if (!e || !*e || !std::strcmp(e, "none"))
			return zimg::CPUClass::NONE;
		if (!std::strcmp(e, "auto"))
			return zimg::CPUClass::AUTO;
#if defined(ZIMG_X86)
		if (!std::strcmp(e, "avx2"))
			return zimg::CPUClass::X86_AVX2;
		if (!std::strcmp(e, "avx512"))
			return zimg::CPUClass::X86_AVX512;
#endif
		return zimg::CPUClass::NONE;
	}();
	return cpu;
}

PlaneBuf convert(const PixelFormat &fin, const PixelFormat &fout, const PlaneBuf &in,
                 DitherType dither = DitherType::NONE)
{
	const unsigned w = in.n;
	auto res = zimg::depth::DepthConversion{ w, 1 }
		.set_pixel_in(fin)
		.set_pixel_out(fout)
		.set_dither_type(dither)
		.set_planes({ true, false, false, false })
		.set_cpu(test_cpu_class())
		.create();

	PlaneBuf out(w, zimg::pixel_size(fout.type));
	if (!res.filter_refs[0]) {
		EXPECT_EQ(in.bps, out.bps);
		out.data = in.data;
		return out;
	}

	const graphengine::Filter *f = res.filter_refs[0];
	const auto &desc = f->descriptor();
	zimg::AlignedVector<uint8_t> ctx(desc.context_size ? desc.context_size : 64);
	zimg::AlignedVector<uint8_t> tmp(desc.scratchpad_size ? desc.scratchpad_size : 64);
	f->init_context(ctx.data());

	graphengine::BufferDescriptor bd_in = descriptor(in);
	graphengine::BufferDescriptor bd_out = descriptor(out);
	unsigned right = w;
	if (desc.alignment_mask)
		right = std::min(w, (right + desc.alignment_mask) & ~desc.alignment_mask);
	f->process(&bd_in, &bd_out, 0, 0, right, ctx.data(), tmp.data());
	return out;
}

// ---- typed access --------------------------------------------------------

uint32_t get_u(const PlaneBuf &p, unsigned j)
{
	if (p.bps == 1)
		return p.data[j];
	uint16_t v;
	std::memcpy(&v, p.data.data() + 2u * j, 2);
	return v;
}

float get_f(const PlaneBuf &p, unsigned j)
{
	float v;
	std::memcpy(&v, p.data.data() + 4u * j, 4);
	return v;
}

uint16_t get_h(const PlaneBuf &p, unsigned j)
{
	uint16_t v;
	std::memcpy(&v, p.data.data() + 2u * j, 2);
	return v;
}

void put_u(PlaneBuf &p, unsigned j, uint32_t v)
{
	if (p.bps == 1)
		p.data[j] = (uint8_t)v;
	else {
		uint16_t u = (uint16_t)v;
		std::memcpy(p.data.data() + 2u * j, &u, 2);
	}
}

PlaneBuf make_u(const PixelFormat &fmt, std::vector<uint32_t> vals)
{
	PlaneBuf p((unsigned)vals.size(), zimg::pixel_size(fmt.type));
	for (size_t i = 0; i < vals.size(); ++i)
		put_u(p, (unsigned)i, vals[i]);
	return p;
}

PlaneBuf make_f(std::vector<float> vals)
{
	PlaneBuf p((unsigned)vals.size(), 4);
	std::memcpy(p.data.data(), vals.data(), vals.size() * 4);
	return p;
}

// ---- reference definitions -----------------------------------------------

// Widening bit replication, see doc/depth-conversion.md.
// For n <= 2*b this equals (v << (n-b)) | (v >> (2*b-n)); the fill loop
// also covers sub-8-bit sources whose pattern must be repeated more than once.
uint32_t ref_replicate(uint32_t v, unsigned b, unsigned n)
{
	uint32_t r = v << (n - b);
	unsigned fill = n - b;
	for (unsigned k = 0; k < fill; ++k) {
		unsigned src_bit = b - 1 - (k % b);
		if (v & (1u << src_bit))
			r |= 1u << (fill - 1 - k);
	}
	return r;
}

// Full-range narrowing: scale to destination range, round half to even.
uint32_t ref_narrow_full(uint32_t v, unsigned n, unsigned b)
{
	double x = (double)v * (double)((1u << b) - 1) / (double)((1u << n) - 1);
	double q = std::floor(x);
	double d = x - q;
	uint32_t r = (d > 0.5 || (d == 0.5 && (((uint32_t)q) & 1u))) ? (uint32_t)q + 1 : (uint32_t)q;
	return std::min<uint32_t>(r, (1u << b) - 1);
}

// Limited-range narrowing: RNE(v / 2^k).
uint32_t ref_narrow_shift(uint32_t v, unsigned n, unsigned b)
{
	unsigned k = n - b;
	uint32_t q = v >> k;
	uint32_t rem = v & ((1u << k) - 1);
	uint32_t half = 1u << (k - 1);
	if (rem > half || (rem == half && (q & 1u)))
		++q;
	return std::min<uint32_t>(q, (1u << b) - 1);
}

PixelFormat int_fmt(PixelType t, unsigned depth, bool fullrange, bool chroma = false)
{
	return PixelFormat{ t, depth, fullrange, chroma, false };
}

} // namespace


// ---------------------------------------------------------------------------
// Property 1: widening preserves full scale (255 -> 65535, 1023 -> 65535).
// Every integer full-scale value maps to the destination full-scale value,
// and the mapping equals bit replication for all source values.
// ---------------------------------------------------------------------------

TEST(DepthMatrixTest, widen_fullrange_endpoints_and_bit_replication)
{
	struct Case { unsigned b; PixelType st; };
	for (Case c : { Case{ 8, PixelType::BYTE }, Case{ 10, PixelType::WORD },
	                Case{ 12, PixelType::WORD }, Case{ 14, PixelType::WORD } }) {
		SCOPED_TRACE(c.b);
		PixelFormat fin = int_fmt(c.st, c.b, true);
		PixelFormat fout = int_fmt(PixelType::WORD, 16, true);

		uint32_t src_max = (1u << c.b) - 1;
		PlaneBuf out = convert(fin, fout, make_u(fin, { src_max }));
		EXPECT_EQ(get_u(out, 0), 65535u); // full-scale endpoint, asserted per depth

		// Every value must equal the bit-replication reference.
		std::vector<uint32_t> vals;
		for (uint32_t v = 0; v <= src_max; ++v)
			vals.push_back(v);
		out = convert(fin, fout, make_u(fin, vals));
		for (uint32_t v = 0; v <= src_max; ++v) {
			EXPECT_EQ(get_u(out, v), ref_replicate(v, c.b, 16)) << "v=" << v;
		}
	}
}

// Sub-8-bit full-range sources are stored in a byte container; when the
// destination has more than twice the source bits, replication degenerates
// to a pure shift. Full scale must still reach the destination full scale.
TEST(DepthMatrixTest, widen_sub8_fullrange)
{
	for (unsigned b : { 1u, 4u, 7u }) {
		SCOPED_TRACE(b);
		PixelFormat fin = int_fmt(PixelType::BYTE, b, true);
		PixelFormat fout = int_fmt(PixelType::WORD, 16, true);

		uint32_t src_max = (1u << b) - 1;
		PlaneBuf ep = convert(fin, fout, make_u(fin, { 0u, src_max }));
		EXPECT_EQ(get_u(ep, 0), 0u);
		EXPECT_EQ(get_u(ep, 1), 65535u);

		std::vector<uint32_t> vals;
		for (uint32_t v = 0; v <= src_max; ++v)
			vals.push_back(v);
		PlaneBuf out = convert(fin, fout, make_u(fin, vals));
		for (uint32_t v = 0; v <= src_max; ++v)
			EXPECT_EQ(get_u(out, v), ref_replicate(v, b, 16)) << "v=" << v;
	}
}

TEST(DepthMatrixTest, widen_limited_range_is_left_shift)
{
	for (bool chroma : { false, true }) {
		SCOPED_TRACE(chroma);
		for (unsigned b : { 8u, 10u, 12u }) {
			SCOPED_TRACE(b);
			PixelType st = b == 8 ? PixelType::BYTE : PixelType::WORD;
			PixelFormat fin = int_fmt(st, b, false, chroma);
			PixelFormat fout = int_fmt(PixelType::WORD, 16, false, chroma);

			std::vector<uint32_t> vals;
			for (uint32_t v = 0; v < (1u << b); ++v)
				vals.push_back(v);
			PlaneBuf out = convert(fin, fout, make_u(fin, vals));
			for (uint32_t v = 0; v < (1u << b); ++v)
				EXPECT_EQ(get_u(out, v), v << (16 - b)) << "v=" << v;
		}
	}
}

// ---------------------------------------------------------------------------
// Property 2: narrowing uses a fixed rounding rule (round half to even).
// ---------------------------------------------------------------------------

TEST(DepthMatrixTest, narrow_fullrange_round_half_to_even)
{
	struct Case { unsigned n; unsigned b; PixelType dt; };
	for (Case c : { Case{ 16, 8, PixelType::BYTE }, Case{ 16, 10, PixelType::WORD },
	                Case{ 16, 12, PixelType::WORD }, Case{ 10, 8, PixelType::BYTE } }) {
		SCOPED_TRACE(c.n);
		SCOPED_TRACE(c.b);
		PixelFormat fin = int_fmt(PixelType::WORD, c.n, true);
		PixelFormat fout = int_fmt(c.dt, c.b, true);

		std::vector<uint32_t> vals;
		uint32_t top = (1u << c.n) - 1;
		for (uint32_t v = 0; v <= top; ++v)
			vals.push_back(v);
		PlaneBuf out = convert(fin, fout, make_u(fin, vals));
		for (uint32_t v = 0; v <= top; ++v)
			EXPECT_EQ(get_u(out, v), ref_narrow_full(v, c.n, c.b)) << "v=" << v;

		// Endpoints stay at full scale.
		PlaneBuf ep = convert(fin, fout, make_u(fin, { 0u, top }));
		EXPECT_EQ(get_u(ep, 0), 0u);
		EXPECT_EQ(get_u(ep, 1), (1u << c.b) - 1);
	}
}

TEST(DepthMatrixTest, narrow_limited_range_round_half_to_even)
{
	struct Case { unsigned n; unsigned b; PixelType dt; bool chroma; };
	for (Case c : { Case{ 16, 8, PixelType::BYTE, false },
	                Case{ 16, 10, PixelType::WORD, false },
	                Case{ 10, 8, PixelType::BYTE, false },
	                Case{ 16, 8, PixelType::BYTE, true } }) {
		SCOPED_TRACE(c.n);
		SCOPED_TRACE(c.b);
		PixelFormat fin = int_fmt(PixelType::WORD, c.n, false, c.chroma);
		PixelFormat fout = int_fmt(c.dt, c.b, false, c.chroma);

		std::vector<uint32_t> vals;
		uint32_t top = (1u << c.n) - 1;
		for (uint32_t v = 0; v <= top; ++v)
			vals.push_back(v);
		PlaneBuf out = convert(fin, fout, make_u(fin, vals));
		for (uint32_t v = 0; v <= top; ++v)
			EXPECT_EQ(get_u(out, v), ref_narrow_shift(v, c.n, c.b)) << "v=" << v;
	}
}

// Explicit tie cases: an exact .5 must resolve to the even destination level.
TEST(DepthMatrixTest, narrow_ties_go_to_even)
{
	// The full-range rational map x*(2^b-1)/(2^n-1) has no exact half-integer
	// points (the denominator 2^n-1 is odd, so 2*x*num can never equal an odd
	// multiple of it); ties therefore only arise in limited-range shifting,
	// where the divider is a power of two.
	PixelFormat w16 = int_fmt(PixelType::WORD, 16, false);
	PixelFormat b8 = int_fmt(PixelType::BYTE, 8, false);
	// v/256: 256->1.0, 384->1.5 (tie 1|2 -> even 2), 128->0.5 (tie 0|1 -> even 0),
	//        640->2.5 (tie 2|3 -> even 2).
	PlaneBuf out = convert(w16, b8, make_u(w16, { 256u, 384u, 128u, 640u }));
	EXPECT_EQ(get_u(out, 0), 1u);
	EXPECT_EQ(get_u(out, 1), 2u);
	EXPECT_EQ(get_u(out, 2), 0u);
	EXPECT_EQ(get_u(out, 3), 2u);
}

// Full-range chroma has an additive offset (128/512/2048), so it is not a pure
// scale: such conversions remain on the floating-point pipeline, by design.
TEST(DepthMatrixTest, fullrange_chroma_keeps_float_pipeline)
{
	PixelFormat w16 = int_fmt(PixelType::WORD, 16, true, true);
	PixelFormat b8 = int_fmt(PixelType::BYTE, 8, true, true);

	// A mid-scale chroma value must pick up the signed offset on its way
	// through float: 32768 (zero chroma in 16-bit) maps to 128 in 8-bit,
	// whereas a pure unsigned scale would yield 127.
	PlaneBuf out = convert(w16, b8, make_u(w16, { 32768u }));
	EXPECT_EQ(get_u(out, 0), 128u);
}

// Requesting an actual dither method must not be silently replaced by the
// exact integer path (which contains no noise shaping).
TEST(DepthMatrixTest, dither_request_overrides_exact_path)
{
	PixelFormat w16 = int_fmt(PixelType::WORD, 16, true);
	PixelFormat b8 = int_fmt(PixelType::BYTE, 8, true);
	std::vector<uint32_t> vals;
	for (unsigned j = 0; j < 64; ++j)
		vals.push_back(257u * j + 1);

	PlaneBuf exact = convert(w16, b8, make_u(w16, vals), DitherType::NONE);
	PlaneBuf ordered = convert(w16, b8, make_u(w16, vals), DitherType::ORDERED);
	bool differs = false;
	for (unsigned j = 0; j < vals.size(); ++j)
		differs = differs || get_u(exact, j) != get_u(ordered, j);
	EXPECT_TRUE(differs);
}

// ---------------------------------------------------------------------------
// Property 3: direct conversion vs floating-point / half intermediary.
// ---------------------------------------------------------------------------

// LSB-normalized absolute difference between two integer outputs.
unsigned diff_u(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

TEST(DepthMatrixTest, direct_vs_via_float_within_one_lsb)
{
	for (bool fullrange : { false, true }) {
		SCOPED_TRACE(fullrange);
		struct Case { unsigned b; PixelType st; unsigned n; };
		for (Case c : { Case{ 8, PixelType::BYTE, 16 }, Case{ 10, PixelType::WORD, 16 },
		                Case{ 12, PixelType::WORD, 16 } }) {
			SCOPED_TRACE(c.b);
			PixelFormat fin = int_fmt(c.st, c.b, fullrange);
			PixelFormat fmid = PixelFormat{ PixelType::FLOAT, 32, fullrange };
			PixelFormat fout = int_fmt(PixelType::WORD, 16, fullrange);

			std::vector<uint32_t> vals;
			for (uint32_t v = 0; v < (1u << c.b); ++v)
				vals.push_back(v);
			PlaneBuf pin = make_u(fin, vals);
			PlaneBuf direct = convert(fin, fout, pin);
			PlaneBuf via = convert(fmid, fout, convert(fin, fmid, pin));

			for (uint32_t v = 0; v < (1u << c.b); ++v)
				EXPECT_LE(diff_u(get_u(direct, v), get_u(via, v)), 1u) << "v=" << v;
		}
	}
}

TEST(DepthMatrixTest, narrow_direct_vs_via_float_within_one_lsb)
{
	// Narrowing side of property 3, full-range integer pairs.
	struct Case { unsigned n; unsigned b; PixelType dt; };
	for (Case c : { Case{ 16, 8, PixelType::BYTE }, Case{ 16, 10, PixelType::WORD },
	                Case{ 10, 8, PixelType::BYTE } }) {
		SCOPED_TRACE(c.n);
		SCOPED_TRACE(c.b);
		PixelFormat fin = int_fmt(PixelType::WORD, c.n, true);
		PixelFormat fmid = PixelFormat{ PixelType::FLOAT, 32, true };
		PixelFormat fout = int_fmt(c.dt, c.b, true);

		std::vector<uint32_t> vals;
		uint32_t top = (1u << c.n) - 1;
		for (uint32_t v = 0; v <= top; ++v)
			vals.push_back(v);
		PlaneBuf pin = make_u(fin, vals);
		PlaneBuf direct = convert(fin, fout, pin);
		PlaneBuf via = convert(fmid, fout, convert(fin, fmid, pin));
		for (uint32_t v = 0; v <= top; ++v)
			EXPECT_LE(diff_u(get_u(direct, v), get_u(via, v)), 1u) << "v=" << v;
	}
}

TEST(DepthMatrixTest, direct_vs_via_half_bounded_by_half_ulp){
	// Half precision quantizes [0,1] to ~1024 levels: its ULP at unit scale is
	// 2^-10 of full scale, i.e. 64 word-LSB. The via-half path may differ from
	// the exact integer path by up to one half ULP; assert the documented
	// bound of 64 word-LSB on every sample.
	for (unsigned b : { 8u, 10u, 12u }) {
		SCOPED_TRACE(b);
		PixelType st = b == 8 ? PixelType::BYTE : PixelType::WORD;
		PixelFormat fin = int_fmt(st, b, true);
		PixelFormat fmid = PixelFormat{ PixelType::HALF, 16, true };
		PixelFormat fout = int_fmt(PixelType::WORD, 16, true);

		std::vector<uint32_t> vals;
		for (uint32_t v = 0; v < (1u << b); ++v)
			vals.push_back(v);
		PlaneBuf pin = make_u(fin, vals);
		PlaneBuf direct = convert(fin, fout, pin);
		PlaneBuf via = convert(fmid, fout, convert(fin, fmid, pin));

		for (uint32_t v = 0; v < (1u << b); ++v)
			EXPECT_LE(diff_u(get_u(direct, v), get_u(via, v)), 64u) << "v=" << v;
	}
}

// Full 4x4 type matrix: every ordered pair of byte/word/half/float converts
// successfully and outputs stay inside the destination representable domain.
TEST(DepthMatrixTest, full_type_matrix_smoke)
{
	const PixelType types[] = { PixelType::BYTE, PixelType::WORD,
	                            PixelType::HALF, PixelType::FLOAT };
	const unsigned N = 31; // odd width exercises tail handling

	for (PixelType tin : types) {
		for (PixelType tout : types) {
			SCOPED_TRACE(static_cast<int>(tin));
			SCOPED_TRACE(static_cast<int>(tout));

			PixelFormat fin{ tin, zimg::pixel_depth(tin), true };
			PixelFormat fout{ tout, zimg::pixel_depth(tout), true };
			PlaneBuf in(N, zimg::pixel_size(tin));

			// Fill with in-domain values (half/float use 0..1, integers mid scale).
			for (unsigned j = 0; j < N; ++j) {
				if (tin == PixelType::BYTE)
					put_u(in, j, (uint32_t)j * 255 / (N - 1));
				else if (tin == PixelType::WORD)
					put_u(in, j, (uint32_t)j * 65535 / (N - 1));
				else if (tin == PixelType::HALF) {
					uint16_t h = zimg::depth::float_to_half((float)j / (N - 1));
					std::memcpy(in.data.data() + 2u * j, &h, 2);
				} else {
					float f = (float)j / (N - 1);
					std::memcpy(in.data.data() + 4u * j, &f, 4);
				}
			}

			PlaneBuf out = convert(fin, fout, in);
			EXPECT_EQ(out.n, N);
			EXPECT_EQ(out.bps, zimg::pixel_size(tout));

			if (tout == PixelType::BYTE) {
				for (unsigned j = 0; j < N; ++j)
					EXPECT_LE(get_u(out, j), 255u);
			} else if (tout == PixelType::WORD && zimg::pixel_is_integer(tout)) {
				for (unsigned j = 0; j < N; ++j)
					EXPECT_LE(get_u(out, j), 65535u);
			} else if (tout == PixelType::FLOAT) {
				for (unsigned j = 0; j < N; ++j) {
					EXPECT_TRUE(std::isfinite(get_f(out, j)));
					EXPECT_GE(get_f(out, j), 0.0f);
					EXPECT_LE(get_f(out, j), 1.0f);
				}
			} else if (tout == PixelType::HALF) {
				for (unsigned j = 0; j < N; ++j) {
					float f = zimg::depth::half_to_float(get_h(out, j));
					EXPECT_TRUE(std::isfinite(f));
					EXPECT_GE(f, 0.0f);
					EXPECT_LE(f, 1.0f);
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Property 4: round-trip error is bounded by the destination quantization step.
// ---------------------------------------------------------------------------

TEST(DepthMatrixTest, integer_roundtrips_within_quant_step)
{
	// byte -> word -> byte: exact (the byte quantization step, 257 word-LSB,
	// dominates and bit replication is lossless through the RNE inverse).
	{
		PixelFormat b8 = int_fmt(PixelType::BYTE, 8, true);
		PixelFormat w16 = int_fmt(PixelType::WORD, 16, true);
		for (uint32_t v = 0; v < 256; ++v) {
			PlaneBuf p = make_u(b8, { v });
			uint32_t r = get_u(convert(w16, b8, convert(b8, w16, p)), 0);
			EXPECT_EQ(r, v);
		}
	}

	// word -> byte -> word: error <= one byte step measured in word units (257).
	{
		PixelFormat b8 = int_fmt(PixelType::BYTE, 8, true);
		PixelFormat w16 = int_fmt(PixelType::WORD, 16, true);
		uint32_t worst = 0;
		for (uint32_t v = 0; v < 65536; ++v) {
			PlaneBuf p = make_u(w16, { v });
			uint32_t r = get_u(convert(b8, w16, convert(w16, b8, p)), 0);
			worst = std::max(worst, diff_u(v, r));
		}
		EXPECT_LE(worst, 257u);
		EXPECT_GE(worst, 128u); // RNE centers the error, so the bound is tight
	}

	// word -> half -> word: error <= half quantization step (~64 word-LSB).
	{
		PixelFormat w16 = int_fmt(PixelType::WORD, 16, true);
		PixelFormat h16{ PixelType::HALF, 16, true };
		uint32_t worst = 0;
		for (uint32_t v = 0; v < 65536; ++v) {
			PlaneBuf p = make_u(w16, { v });
			uint32_t r = get_u(convert(h16, w16, convert(w16, h16, p)), 0);
			worst = std::max(worst, diff_u(v, r));
		}
		EXPECT_LE(worst, 64u);
	}

	// word -> float -> word: float32 represents the scale exactly, RNE gives 0.
	{
		PixelFormat w16 = int_fmt(PixelType::WORD, 16, true);
		PixelFormat f32{ PixelType::FLOAT, 32, true };
		uint32_t worst = 0;
		for (uint32_t v = 0; v < 65536; v += 7) {
			PlaneBuf p = make_u(w16, { v });
			uint32_t r = get_u(convert(f32, w16, convert(w16, f32, p)), 0);
			worst = std::max(worst, diff_u(v, r));
		}
		EXPECT_EQ(worst, 0u);
	}

	// float -> half -> float: error <= one half ULP at the sample magnitude.
	{
		PixelFormat f32{ PixelType::FLOAT, 32, true };
		PixelFormat h16{ PixelType::HALF, 16, true };
		std::vector<float> samples;
		for (int e = -14; e <= 0; ++e)
			samples.push_back(std::ldexp(1.0f, e));
		PlaneBuf pin = make_f(samples);
		PlaneBuf back = convert(h16, f32, convert(f32, h16, pin));
		for (size_t j = 0; j < samples.size(); ++j) {
			float x = samples[j];
			float ulp = std::ldexp(1.0f, std::ilogb(x) - 9); // half has 10 mantissa bits
			EXPECT_LE(std::fabs(get_f(back, j) - x), ulp);
		}
	}
}

