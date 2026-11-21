#include <cmath>
#include "common/except.h"
#include "resize/filter.h"

#include "gtest/gtest.h"

namespace {

void check_interpolating(const zimg::resize::Filter& f)
{
	unsigned support = f.support();

	EXPECT_EQ(1.0, f(0));
	for (unsigned i = 1; i <= support; ++i) {
		SCOPED_TRACE(i);
		EXPECT_NEAR(0.0, f(-static_cast<double>(i)), 1e-15);
		EXPECT_NEAR(0.0, f(static_cast<double>(i)), 1e-15);
	}
	EXPECT_EQ(0.0, f(std::nextafter(-static_cast<double>(support), -INFINITY)));
	EXPECT_EQ(0.0, f(std::nextafter(static_cast<double>(support), INFINITY)));
}

} // namespace


TEST(FilterTest, test_bilinear)
{
	zimg::resize::BilinearFilter f;
	EXPECT_EQ(1U, f.support());
	check_interpolating(f);
	EXPECT_EQ(0.5, f(0.5));
	EXPECT_EQ(0.5, f(-0.5));
}

TEST(FilterTest, test_bicubic_interpolating)
{
	zimg::resize::BicubicFilter catmull{ 0.0, 0.5 };
	EXPECT_EQ(2U, catmull.support());
	check_interpolating(catmull);
	EXPECT_EQ(0.5625, catmull(0.5));
	EXPECT_EQ(0.5625, catmull(-0.5));
	EXPECT_EQ(-0.0625, catmull(1.5));
	EXPECT_EQ(-0.0625, catmull(-1.5));
}

TEST(FilterTest, test_bicubic_noninterpolating)
{
	zimg::resize::BicubicFilter mitchell{ 1.0 / 3.0, 1.0 / 3.0 };
	EXPECT_DOUBLE_EQ(8.0 / 9.0, mitchell(0.0));
	EXPECT_NEAR(1.0 / 18.0, mitchell(-1.0), 1e-15);
	EXPECT_NEAR(1.0 / 18.0, mitchell(1.0), 1e-15);
	EXPECT_EQ(0.0, mitchell(-2.0));
	EXPECT_EQ(0.0, mitchell(2.0));
	EXPECT_DOUBLE_EQ(77.0 / 144.0, mitchell(0.5));
	EXPECT_DOUBLE_EQ(77.0 / 144.0, mitchell(-0.5));
	EXPECT_NEAR(-5.0 / 144.0 , mitchell(1.5), 1e-15);
	EXPECT_NEAR(-5.0 / 144.0, mitchell(-1.5), 1e-15);
}

TEST(FilterTest, test_spline16)
{
	zimg::resize::Spline16Filter f;
	EXPECT_EQ(2U, f.support());
	check_interpolating(f);
	EXPECT_EQ(0.575, f(0.5));
	EXPECT_EQ(0.575, f(-0.5));
	EXPECT_DOUBLE_EQ(-0.075, f(1.5));
	EXPECT_DOUBLE_EQ(-0.075, f(-1.5));
}

TEST(FilterTest, test_spline36)
{
	zimg::resize::Spline36Filter f;
	EXPECT_EQ(3U, f.support());
	check_interpolating(f);
	EXPECT_NEAR(0.598684, f(0.5), 1e-6);
	EXPECT_NEAR(0.598684, f(-0.5), 1e-6);
	EXPECT_NEAR(-0.118421, f(1.5), 1e-6);
	EXPECT_NEAR(-0.118421, f(-1.5), 1e-6);
	EXPECT_NEAR(0.0197368, f(2.5), 1e-6);
	EXPECT_NEAR(0.0197368, f(-2.5), 1e-6);
}

TEST(FilterTest, test_spline64)
{
	zimg::resize::Spline64Filter f;
	EXPECT_EQ(4U, f.support());
	check_interpolating(f);
	EXPECT_NEAR(0.600352, f(0.5), 1e-6);
	EXPECT_NEAR(0.600352, f(-0.5), 1e-6);
	EXPECT_NEAR(-0.126760, f(1.5), 1e-6);
	EXPECT_NEAR(-0.126760, f(-1.5), 1e-6);
	EXPECT_NEAR(0.0316901, f(2.5), 1e-7);
	EXPECT_NEAR(0.0316901, f(-2.5), 1e-7);
	EXPECT_NEAR(-0.00528169, f(3.5), 1e-8);
	EXPECT_NEAR(-0.00528169, f(-3.5), 1e-8);
}

TEST(FilterTest, test_lanczos)
{
	for (unsigned i = 1; i < 4; ++i) {
		SCOPED_TRACE(i);
		zimg::resize::LanczosFilter f{ i };
		EXPECT_EQ(i, f.support());
		check_interpolating(f);
	}
}

TEST(FilterTest, test_bicubic_parameter_validation)
{
	const double bad_values[] = { -1.0, -0.01, 1.01, 1e9, NAN, INFINITY, -INFINITY };

	// Out-of-range or non-finite "b" or "c" is rejected with a distinguishable message.
	for (double bad : bad_values) {
		SCOPED_TRACE(bad);
		try {
			zimg::resize::BicubicFilter f{ bad, zimg::resize::BicubicFilter::DEFAULT_C };
			FAIL() << "bad b accepted: " << bad;
		} catch (const zimg::error::IllegalArgument &e) {
			EXPECT_STREQ("bicubic parameter b out of range", e.what());
		}

		try {
			zimg::resize::BicubicFilter f{ zimg::resize::BicubicFilter::DEFAULT_B, bad };
			FAIL() << "bad c accepted: " << bad;
		} catch (const zimg::error::IllegalArgument &e) {
			EXPECT_STREQ("bicubic parameter c out of range", e.what());
		}
	}

	// Boundary values of the documented domain are accepted.
	EXPECT_NO_THROW(zimg::resize::BicubicFilter(0.0, 0.0));
	EXPECT_NO_THROW(zimg::resize::BicubicFilter(1.0, 1.0));

	// Default constructor remains equivalent to the documented defaults.
	zimg::resize::BicubicFilter default_filter;
	zimg::resize::BicubicFilter explicit_default{ zimg::resize::BicubicFilter::DEFAULT_B,
	                                             zimg::resize::BicubicFilter::DEFAULT_C };
	for (double x = -2.0; x <= 2.0; x += 0.125) {
		EXPECT_DOUBLE_EQ(explicit_default(x), default_filter(x));
	}
}

TEST(FilterTest, test_lanczos_parameter_validation)
{
	try {
		zimg::resize::LanczosFilter f{ 0 };
		FAIL() << "zero tap count accepted";
	} catch (const zimg::error::IllegalArgument &e) {
		EXPECT_STREQ("lanczos tap count out of range", e.what());
	}

	try {
		zimg::resize::LanczosFilter f{ zimg::resize::LanczosFilter::MAX_TAPS };
		FAIL() << "maximum tap count accepted";
	} catch (const zimg::error::IllegalArgument &e) {
		EXPECT_STREQ("lanczos tap count out of range", e.what());
	}

	// Boundary values of the documented domain are accepted.
	EXPECT_NO_THROW(zimg::resize::LanczosFilter{ zimg::resize::LanczosFilter::MIN_TAPS });
	EXPECT_NO_THROW(zimg::resize::LanczosFilter{ zimg::resize::LanczosFilter::MAX_TAPS - 1 });
}
