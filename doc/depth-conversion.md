# Bit-Depth Conversion Semantics

This document specifies the exact rounding and scaling rules used when
converting between the supported pixel types: **BYTE** (8-bit unsigned),
**WORD** (up to 16-bit unsigned), **HALF** (IEEE 754 binary16), and
**FLOAT** (IEEE 754 binary32). All rules are independent of the host
floating-point rounding mode.

## 1. Notation

| Symbol | Meaning |
|--------|---------|
| `b`    | source bit depth |
| `n`    | destination bit depth |
| `x`    | source sample, `0 <= x <= 2^b - 1` |
| `r`    | destination sample |
| `RNE`  | round to nearest, ties to even |
| `clamp_n(y)` | `min(max(y, 0), 2^n - 1)` |

Integer formats exist in two range conventions:

- **Full range** (`fullrange = true`, JPEG style): code 0 is black and
  `2^d - 1` is white; there is no additive offset.
- **Limited range** (`fullrange = false`, video style): the signal occupies
  `16·2^(d-8) .. 235·2^(d-8)` for luma and `16·2^(d-8) .. 240·2^(d-8)` for
  chroma, with a fixed offset. Integer-to-integer limited-range conversions
  never apply the offset; they only rescale the code values, so the code
  structure is preserved exactly.

## 2. Integer-to-integer conversion (dither type `NONE`)

### 2.1 Widening, full range — bit replication

When `n > b` and both formats are full range, `x` is placed in the high
`b` bits of the result and the remaining `n - b` low bits are filled by
cycling through the source bits from MSB to LSB:

```
r = x << (n - b)
for k in 0 .. n-b-1:   r[fill-1-k] = x[(b-1) - (k mod b)],  fill = n - b   (1)
```

In the common case `n <= 2b` (8 → 16, 10 → 16, 12 → 16, …) the fill region is
a prefix of the source bits, so the loop reduces to the shift-and-OR form:

```
r = (x << (n - b)) | (x >> (2b - n))
```

For sub-8-bit sources the source pattern is repeated as many times as
necessary (e.g. 1 → 16 bit maps 0 → 0 and 1 → 65535), which the single
shift-OR expression cannot express.

This makes the mapping scale-consistent in both directions and, critically,
maps the full-scale value onto the full-scale value:

```
x = 2^b - 1  =>  every replicated bit is 1  =>  r = 2^n - 1
```

Examples:

| Conversion | `x` | formula | `r` |
|------------|-----|---------|-----|
| 8 → 16 bit | 1   | `(1<<8) \| 1` | 257 |
| 8 → 16 bit | 255 | `(255<<8) \| 255` | **65535** |
| 10 → 16 bit | 1023 | bit replication | **65535** |
| 12 → 16 bit | 4095 | `(4095<<4) \| (4095>>8)` | **65535** |

### 2.2 Widening, limited range — left shift

When `n > b` and both formats are limited range:

```
r = x << (n - b)                               (2)
```

This is a lossless repositioning of the code bits.

### 2.3 Narrowing, full range — scaled round-half-to-even

When `n < b` and both formats are full range, the sample is rescaled onto the
destination code range and rounded to nearest with ties resolving to the even
destination code:

```
                x · (2^n - 1)
r = RNE ( --------------------- )              (3)
                  2^b - 1
```

Equivalently, using exact integer arithmetic (the implementation in
`quantize.cpp::integer_scale_fullrange`):

```
q = x · (2^n - 1) div (2^b - 1)
m = x · (2^n - 1) mod (2^b - 1)
r = q + 1   if 2m > (2^b - 1), or 2m == (2^b - 1) and q is odd
r = q       otherwise
r = clamp_n(r)
```

Because `2^b - 1` is odd, an exact tie (`2m == 2^b - 1`) cannot occur for
full-range inputs; the rule is still stated as RNE for symmetry and for
future depth combinations. RNE is preferred over round-half-up because it is
unbiased and matches the IEEE 754 default rounding used by the floating-point
pipeline (see §4).

Examples (16 → 8 bit, full range):

| `x` | exact quotient | `r` |
|-----|----------------|-----|
| 0   | 0              | 0 |
| 128 | 0.498…         | 0 |
| 256 | 0.996…         | 1 |
| 65535 | 255          | **255** |

### 2.4 Narrowing, limited range — shifted round-half-to-even

When `n < b` and both formats are limited range, formula (2) is inverted by
dividing by the power-of-two shift and applying RNE:

```
r = clamp_n( RNE( x / 2^(b - n) ) )            (4)
```

In integer terms, with `k = b - n`:

```
q = x >> k
m = x & (2^k - 1)
r = q + 1   if m > 2^(k-1), or m == 2^(k-1) and q is odd
r = q       otherwise
```

Exact ties occur here (e.g. 16 → 8 bit, `x = 128` gives `0.5`, which rounds
to the even code 0; `x = 384` gives `1.5`, which rounds to 2).

### 2.5 Cases that keep the floating-point pipeline

A direct integer formula is only used when both endpoints are integer, the
range flags and color family match, no additive offset is involved, and the
dither type is `NONE`. In particular:

- full-range **chroma** carries a `2^(d-1)` offset and is converted via float;
- conversions between full- and limited-range are converted via float;
- `ORDERED`, `RANDOM`, and `ERROR_DIFFUSION` dither always use the
  floating-point dithering pipeline.

## 3. Integer ↔ floating-point conversion

Integer-to-float and float-to-integer conversions apply the affine map
defined by the format offsets and ranges:

```
f = x · scale + offset,   scale = range_out / range_in              (5)
```

with the constants from `quantize.cpp` (`integer_offset`, `integer_range`).
Float-to-integer quantization clamps to `[0, 2^d - 1]` and rounds using
`lrint`, i.e. the current rounding mode — in the default mode this is
round-half-to-even, matching formula (3)/(4). The SIMD implementations use
`vcvtps2dq` / `vcvtnq_u32`, which implement the same tie-to-even rule.

## 4. Path equivalence: direct vs. via-float vs. via-half

For any integer pair `A → B`, three routes are possible:

1. **direct**: the integer filter of §2;
2. **via float**: `A → FLOAT → B`, two affine maps (5);
3. **via half**: `A → HALF → B`, additionally quantizing through binary16.

### 4.1 Direct vs. via float — at most 1 LSB

The direct path uses exact integer RNE; the float path evaluates the same
scale in binary32 with one rounding per stage. For the supported depths
(≤ 16 bit) the scale constants and every scaled value are represented with
relative error below `2^-24`, so the two routes can only disagree on a sample
that lies within one float ULP of an integer boundary. The observed maximum
difference is **1 destination LSB** (and zero for 8 → 16 and 16 → 8 bit,
where the ratios `257` and `255/65535` happen to round identically in both
implementations). The bound is asserted exhaustively in
`depth_matrix_test.cpp`.

### 4.2 Direct vs. via half — at most one half ULP

Binary16 carries 11 significant bits, so at unit scale its ULP near 1.0 is
`2^-10` of full scale = **64 WORD LSB** (and correspondingly 0.25 BYTE LSB).
The via-half route therefore differs from the exact integer route by at most
one half ULP; the test asserts the 64-LSB bound on every sample. This is an
inherent precision limitation of the intermediary type, not a rounding-policy
mismatch.

## 5. Round-trip error bounds

For a round trip `A → B → A`, the error cannot exceed one quantization step
of the coarsest format encountered:

| Round trip | Bound (in source LSB) | Rationale |
|------------|-----------------------|-----------|
| byte → word → byte | 0 (exact) | bit replication is inverted exactly by RNE |
| word → byte → word | 257 (= `2^16 / 2^8`) | one byte quantization step in word units; observed worst case 128 |
| word → half → word | 64 | one binary16 ULP at unit scale |
| byte → half → byte | 0 observed | half ULP (0.25 byte LSB) never crosses a byte boundary after RNE |
| word → float → word | 0 | binary32 represents the 16-bit scale exactly |
| float → half → float | 1 half ULP | standard binary32 ↔ binary16 rounding |

## 6. Half ↔ float conversion

`half_to_float` / `float_to_half` implement IEEE 754 binary16 conversion with
round-half-to-even, including denormals, NaN propagation (sNaN → qNaN), and
infinity overflow. The result of `float → half → float` is the binary16
neighbor of the input and never differs by more than one half ULP.
