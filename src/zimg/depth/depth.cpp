#include "common/cpuinfo.h"
#include "common/except.h"
#include "common/pixel.h"
#include "graphengine/filter.h"
#include "depth.h"
#include "depth_convert.h"
#include "dither.h"

namespace zimg::depth {

namespace {

bool is_lossless_conversion(const PixelFormat &pixel_in, const PixelFormat &pixel_out)
{
	return pixel_is_integer(pixel_in.type) &&
	       pixel_is_integer(pixel_out.type) &&
	       !pixel_in.fullrange &&
	       !pixel_out.fullrange &&
	       pixel_in.chroma == pixel_out.chroma &&
	       pixel_out.depth >= pixel_in.depth;
}

// Exact integer scaling is available when both endpoints are integers with
// matching range and color family, and the transfer has no additive offset.
// Full-range luma and all limited-range formats have zero offset; full-range
// chroma and cross-range conversions keep using the floating-point pipeline.
bool is_exact_integer_conversion(const PixelFormat &pixel_in, const PixelFormat &pixel_out)
{
	return pixel_is_integer(pixel_in.type) &&
	       pixel_is_integer(pixel_out.type) &&
	       pixel_in.fullrange == pixel_out.fullrange &&
	       pixel_in.chroma == pixel_out.chroma &&
	       !(pixel_in.fullrange && pixel_in.chroma);
}

} // namespace


DepthConversion::result::result() : filters{}, filter_refs{} {}

DepthConversion::result::result(std::unique_ptr<graphengine::Filter> filter, const bool planes[4]) :
	filters{},
	filter_refs{}
{
	filters[0] = std::move(filter);

	for (unsigned p = 0; p < 4; ++p) {
		if (planes[p])
			filter_refs[p] = filters[0].get();
	}
}

DepthConversion::DepthConversion(unsigned width, unsigned height) :
	width{ width },
	height{ height },
	pixel_in{},
	pixel_out{},
	dither_type{ DitherType::NONE },
	planes{ true, false, false, false },
	cpu{ CPUClass::NONE }
{}

DepthConversion::result DepthConversion::create() const try
{
	if (width > pixel_max_width(pixel_in.type) || width > pixel_max_width(pixel_out.type))
		error::throw_<error::OutOfMemory>();
	if ((!pixel_in.fullrange && pixel_in.depth < 8) || (!pixel_out.fullrange && pixel_out.depth < 8))
		error::throw_<error::BitDepthOverflow>("bit depth must be at least 8 for limited range");

	if (pixel_in == pixel_out)
		return{};
	else if (is_lossless_conversion(pixel_in, pixel_out))
		return{ create_left_shift(width, height, pixel_in, pixel_out, cpu), planes.data() };
	else if (dither_type == DitherType::NONE && is_exact_integer_conversion(pixel_in, pixel_out))
		return{ create_integer_convert(width, height, pixel_in, pixel_out, cpu), planes.data() };
	else if (pixel_is_float(pixel_out.type))
		return{ create_convert_to_float(width, height, pixel_in, pixel_out, cpu), planes.data() };
	else
		return create_dither(dither_type, width, height, pixel_in, pixel_out, planes.data(), cpu);
} catch (const std::bad_alloc &) {
	error::throw_<error::OutOfMemory>();
}

} // namespace zimg::depth
