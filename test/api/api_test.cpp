#include <cmath>
#include <cstddef>
#include <cstring>
#include "api/zimg.h"

#include "gtest/gtest.h"

namespace {

zimg_image_format make_format(unsigned width, unsigned height)
{
	zimg_image_format format{};
	zimg_image_format_default(&format, ZIMG_API_VERSION);
	format.width = width;
	format.height = height;
	format.pixel_type = ZIMG_PIXEL_BYTE;
	format.color_family = ZIMG_COLOR_GREY;
	return format;
}

struct GraphBuildResult {
	zimg_filter_graph *graph;
	zimg_error_code_e error;
	char message[256];
};

GraphBuildResult try_build_graph(const zimg_graph_builder_params &params)
{
	zimg_image_format src = make_format(64, 64);
	zimg_image_format dst = make_format(32, 48);

	zimg_clear_last_error();
	zimg_filter_graph *graph = zimg_filter_graph_build(&src, &dst, &params);

	GraphBuildResult result{};
	result.error = zimg_get_last_error(result.message, sizeof(result.message));
	result.graph = graph;
	return result;
}

} // namespace

TEST(APITest, test_api_2_0_compat)
{
	const unsigned API_2_0 = ZIMG_MAKE_API_VERSION(2, 0);
	const size_t extra_off = offsetof(zimg_image_format, active_region);
	const size_t extra_len = sizeof(zimg_image_format) - extra_off;

	zimg_image_format format;
	std::memset(reinterpret_cast<unsigned char *>(&format) + extra_off, 0xCC, extra_len);

	zimg_image_format_default(&format, API_2_0);
	EXPECT_EQ(API_2_0, format.version);
	for (size_t i = extra_off; i < extra_off + extra_len; ++i) {
		EXPECT_EQ(0xCC, *(reinterpret_cast<unsigned char *>(&format) + i));
	}

	format.width = 640;
	format.height = 480;
	format.pixel_type = ZIMG_PIXEL_BYTE;

	// Should trigger error in API 2.1+.
	format.active_region.left = 0;
	format.active_region.top = 0;
	format.active_region.width = -INFINITY;
	format.active_region.height = -INFINITY;

	zimg_graph_builder_params params;
	zimg_graph_builder_params_default(&params, API_2_0);
	EXPECT_EQ(API_2_0, params.version);

	zimg_filter_graph *graph = zimg_filter_graph_build(&format, &format, &params);
	EXPECT_TRUE(graph);
	zimg_filter_graph_free(graph);
}

TEST(APITest, test_api_2_1_compat)
{
	const unsigned API_2_1 = ZIMG_MAKE_API_VERSION(2, 1);
	const size_t extra_off = offsetof(zimg_graph_builder_params, nominal_peak_luminance);
	const size_t extra_len = sizeof(zimg_graph_builder_params) - extra_off;

	zimg_graph_builder_params params;
	std::memset(reinterpret_cast<unsigned char *>(&params) + extra_off, 0xCC, extra_len);

	zimg_graph_builder_params_default(&params, API_2_1);
	EXPECT_EQ(API_2_1, params.version);
	for (size_t i = extra_off; i < extra_off + extra_len; ++i) {
		EXPECT_EQ(0xCC, *(reinterpret_cast<unsigned char *>(&params) + i));
	}
}

TEST(APITest, test_api_2_3_compat)
{
	const unsigned API_2_3 = ZIMG_MAKE_API_VERSION(2, 3);
	const size_t extra_off = offsetof(zimg_image_format, alpha);
	const size_t extra_len = sizeof(zimg_image_format) - extra_off;

	zimg_image_format format;
	std::memset(reinterpret_cast<unsigned char *>(&format) + extra_off, 0xCC, extra_len);

	zimg_image_format_default(&format, API_2_3);
	EXPECT_EQ(API_2_3, format.version);
	for (size_t i = extra_off; i < extra_off + extra_len; ++i) {
		EXPECT_EQ(0xCC, *(reinterpret_cast<unsigned char *>(&format) + i));
	}
}

TEST(APITest, test_api_2_4_compat)
{
	const unsigned API_2_4 = ZIMG_MAKE_API_VERSION(2, 4);
	const size_t extra_off = offsetof(zimg_graph_builder_params, scene_referred);
	const size_t extra_len = sizeof(zimg_graph_builder_params) - extra_off;

	zimg_graph_builder_params format;
	std::memset(reinterpret_cast<unsigned char *>(&format) + extra_off, 0xCC, extra_len);

	zimg_graph_builder_params_default(&format, API_2_4);
	EXPECT_EQ(API_2_4, format.version);
	for (size_t i = extra_off; i < extra_off + extra_len; ++i) {
		EXPECT_EQ(0xCC, *(reinterpret_cast<unsigned char *>(&format) + i));
	}
}

TEST(APITest, test_resize_filter_parameter_validation)
{
	zimg_graph_builder_params params{};
	zimg_graph_builder_params_default(&params, ZIMG_API_VERSION);

	// Leaving all parameters at NAN succeeds for every filter.
	const zimg_resample_filter_e all_filters[] = {
		ZIMG_RESIZE_POINT, ZIMG_RESIZE_BILINEAR, ZIMG_RESIZE_BICUBIC,
		ZIMG_RESIZE_SPLINE16, ZIMG_RESIZE_SPLINE36, ZIMG_RESIZE_SPLINE64,
		ZIMG_RESIZE_LANCZOS,
	};
	for (zimg_resample_filter_e f : all_filters) {
		SCOPED_TRACE(f);
		params.resample_filter = f;
		params.filter_param_a = NAN;
		params.filter_param_b = NAN;
		GraphBuildResult ok = try_build_graph(params);
		EXPECT_NE(nullptr, ok.graph);
		EXPECT_EQ(ZIMG_ERROR_SUCCESS, ok.error);
		zimg_filter_graph_free(ok.graph);
	}

	// Bicubic: partial parameter specifications are rejected with one stable message.
	params.resample_filter = ZIMG_RESIZE_BICUBIC;
	params.filter_param_a = 0.0;
	params.filter_param_b = NAN;
	GraphBuildResult only_a = try_build_graph(params);
	EXPECT_EQ(nullptr, only_a.graph);
	EXPECT_EQ(ZIMG_ERROR_ILLEGAL_ARGUMENT, only_a.error);
	EXPECT_STREQ("bicubic parameters b and c must be specified together", only_a.message);

	params.filter_param_a = NAN;
	params.filter_param_b = 0.5;
	GraphBuildResult only_b = try_build_graph(params);
	EXPECT_EQ(nullptr, only_b.graph);
	EXPECT_EQ(ZIMG_ERROR_ILLEGAL_ARGUMENT, only_b.error);
	EXPECT_STREQ(only_a.message, only_b.message);

	// Bicubic: out-of-domain values are rejected with parameter-specific messages.
	const struct { double a; double b; const char *message; } bicubic_cases[] = {
		{ -1.0,   0.5, "bicubic parameter b out of range" },
		{ 0.0,   -1.0, "bicubic parameter c out of range" },
		{ 1e9,    0.5, "bicubic parameter b out of range" },
		{ 0.0,    1e9, "bicubic parameter c out of range" },
		{ NAN,    0.5, "bicubic parameters b and c must be specified together" },
		{ 0.0,    NAN, "bicubic parameters b and c must be specified together" },
		{ INFINITY, 0.5, "bicubic parameter b out of range" },
		{ 0.0, INFINITY, "bicubic parameter c out of range" },
	};
	for (const auto &tc : bicubic_cases) {
		SCOPED_TRACE(tc.a);
		SCOPED_TRACE(tc.b);
		params.filter_param_a = tc.a;
		params.filter_param_b = tc.b;
		GraphBuildResult bad = try_build_graph(params);
		EXPECT_EQ(nullptr, bad.graph);
		EXPECT_EQ(ZIMG_ERROR_ILLEGAL_ARGUMENT, bad.error);
		EXPECT_STREQ(tc.message, bad.message);
	}

	// Bicubic: complete, in-domain specifications are accepted.
	params.filter_param_a = 1.0 / 3.0;
	params.filter_param_b = 1.0 / 3.0;
	GraphBuildResult mitchell = try_build_graph(params);
	EXPECT_NE(nullptr, mitchell.graph);
	zimg_filter_graph_free(mitchell.graph);

	// Lanczos: tap count must be a finite integer in [1, 15].
	params.resample_filter = ZIMG_RESIZE_LANCZOS;
	const struct { double taps; const char *message; } lanczos_cases[] = {
		{ -1.0,      "lanczos tap count out of range" },
		{ 0.0,       "lanczos tap count out of range" },
		{ 16.0,      "lanczos tap count out of range" },
		{ 1e9,       "lanczos tap count out of range" },
		{ INFINITY,  "lanczos tap count out of range" },
		{ 2.5,       "lanczos tap count must be an integer" },
	};
	for (const auto &tc : lanczos_cases) {
		SCOPED_TRACE(tc.taps);
		params.filter_param_a = tc.taps;
		params.filter_param_b = NAN;
		GraphBuildResult bad = try_build_graph(params);
		EXPECT_EQ(nullptr, bad.graph);
		EXPECT_EQ(ZIMG_ERROR_ILLEGAL_ARGUMENT, bad.error);
		EXPECT_STREQ(tc.message, bad.message);
	}

	// Lanczos: domain boundaries, the NAN default, and an explicit default are accepted.
	const double lanczos_ok_taps[] = { std::nan(""), 1.0, 3.0, 15.0 };
	for (double taps : lanczos_ok_taps) {
		SCOPED_TRACE(taps);
		params.filter_param_a = taps;
		GraphBuildResult ok = try_build_graph(params);
		EXPECT_NE(nullptr, ok.graph);
		zimg_filter_graph_free(ok.graph);
	}
}
