z.lib
======
The "z" library implements the commonly required image processing basics of
scaling, colorspace conversion, and depth conversion. A simple API enables
conversion between any supported formats to operate with minimal knowledge
from the programmer. All library routines were designed from the ground-up
with correctness, flexibility, and thread-safety as first priorities.
Allocation, buffering, and I/O are cleanly separated from processing, allowing
the programmer to adapt "z" to many scenarios.

Requirements
-----
- Byte-addressable architecture
- Two's complement integer encoding
- 32-bit or greater machine word
- C++17 compiler
- Platforms: Microsoft Windows, POSIX

Building
-----
The officially supported build system is GNU autotools. Use the provided
"autogen.sh" script to instantiate the familiar "configure" and "make" build
system. Visual Studio project files are not stable and are subject to change.
Download all submodules before compiling by `git submodule update --init
--recursive`.

Capabilities
-----
### Colorspace

Colorspaces: SMPTE-C (NTSC), Rec.709, Rec.2020

The colorspace module provides for conversion between any combination of
colorspaces, as defined by the commonly used triplet of matrix coefficients,
transfer characteristics, and color primaries. Conversions are implemented
with intelligent logic that minimizes the number of intermediate
representations required for common scenarios, such as conversion between
YCbCr and RGB. Support is also provided for the non-traditional YCbCr system
of ITU-R BT.2020 constant luminance (CL), which retains higher fidelity with
chroma subsampling. Note that "z" is not a color management system and should
not be used to perform drastic contrast or gamut reduction, such as BT.2020
to BT.709.

### Depth

Formats: BYTE, WORD, HALF, FLOAT

The depth module provides for conversion between any pixel (number) format,
including one and two-byte integer formats as well as IEEE-754 binary16
(OpenEXR) and binary32 formats. Limited range (16-235) and full swing (0-255)
integer formats are supported, including conversion between such formats.
Multiple dithering methods are available when converting to integer formats,
from basic rounding to high quality error diffusion.

### Resize

The resize module provides high fidelity linear resamplers, including the
popular Bicubic and Lanczos filters. Resampling ratios of up to 100x are
supported for upsampling and downsampling. Full support is provided for
various coordinate systems, including the various chroma siting conventions
(e.g. JPEG and MPEG2) as well as interlaced images.

The parameterizable filters are configured through `filter_param_a` and
`filter_param_b` of `zimg_graph_builder_params`. Parameters left at their
default NAN value select the library default. Any parameter outside the
documented domain is rejected with `ZIMG_ERROR_ILLEGAL_ARGUMENT` and a
message describing the offending parameter.

| Filter                | `filter_param_a`                         | `filter_param_b`                         |
|-----------------------|------------------------------------------|------------------------------------------|
| ZIMG_RESIZE_POINT     | unused                                   | unused                                   |
| ZIMG_RESIZE_BILINEAR  | unused                                   | unused                                   |
| ZIMG_RESIZE_BICUBIC   | "b": finite, [0.0, 1.0], default 0.0     | "c": finite, [0.0, 1.0], default 0.5     |
| ZIMG_RESIZE_SPLINE16  | unused                                   | unused                                   |
| ZIMG_RESIZE_SPLINE36  | unused                                   | unused                                   |
| ZIMG_RESIZE_SPLINE64  | unused                                   | unused                                   |
| ZIMG_RESIZE_LANCZOS   | tap count: integer, [1, 15], default 3   | unused                                   |

For ZIMG_RESIZE_BICUBIC the "b" and "c" parameters must be specified
together: either both remain NAN (library defaults) or both are set. A
partial specification (only one of the two parameters) is rejected rather
than silently combined with a default, as are non-finite values and values
outside [0.0, 1.0]. For ZIMG_RESIZE_LANCZOS, the tap count must be a finite
integral value in [1, 15]; fractional, negative, zero, and excessively large
tap counts are rejected.

Performance
-----
"z" is optimized for Intel(R) Architecture and features faster processing times
than industry standard swscale software.

Time (ms) to resize FHD image to UHD with Lanczos filter.

|                                | z.lib 2.8 | swscale 4.0.2* |
|--------------------------------|-----------|----------------|
| Intel(R) Core(TM) i7-8565U     |       7.7 |           15.2 |
| Intel(R) Xeon(R) Platinum 8176 |      10.8 |           22.2 |

Time (ms) to convert FHD BT.709 (YUV) to FHD BT.2020.

|                                | z.lib 2.8 | swscale 4.0.2** |
|--------------------------------|-----------|-----------------|
| Intel(R) Core(TM) i7-8565U     |       8.3 |            17.5 |
| Intel(R) Xeon(R) Platinum 8176 |      11.5 |            25.6 |

\* `scale=3840:2160:sws_flags=lanczos+accurate_rnd:sws_dither=none`

\** `colorspace=all=bt2020:iall=bt709:format=yuv420p10`

