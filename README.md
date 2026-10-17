# fastpybase64

[![Tests](https://github.com/simdutf/fastpybase64/actions/workflows/tests.yml/badge.svg)](https://github.com/simdutf/fastpybase64/actions/workflows/tests.yml)

A Python binding for the base64 functions of
[simdutf](https://github.com/simdutf/simdutf), the library that Node.js uses
for base64. It is a drop-in replacement for the standard
library's `base64` module: every function returns the same bytes and raises
the same exceptions as its counterpart in `base64`, for every input. It
decodes up to 80 times faster than the standard library and up to 13 times
faster than [pybase64](https://github.com/mayeut/pybase64), and it is
several times faster than both on small inputs.

With pip:

```sh
pip install fastpybase64
```

With uv:

```sh
uv pip install fastpybase64      # in a uv project: uv add fastpybase64
```

Wheels are built for Linux, macOS and Windows, for Python 3.10 to 3.14,
including free-threaded Python 3.14.

## Usage

```python
import fastpybase64 as base64

base64.b64encode(b"hello")                     # b'aGVsbG8='
base64.b64decode("aGVsbG8=")                   # b'hello'
base64.b64decode(b"aGVsbG8=", validate=True)   # b'hello'
base64.urlsafe_b64encode(b"\xfb\xff")          # b'-_8='
base64.encodebytes(data)                       # MIME: lines of 76 characters
```

| function | |
|---|---|
| `b64encode(s, altchars=None)` | standard alphabet, or any two replacement characters for `+` and `/` |
| `b64decode(s, altchars=None, validate=False)` | |
| `standard_b64encode(s)`, `standard_b64decode(s)` | |
| `urlsafe_b64encode(s)`, `urlsafe_b64decode(s)` | alphabet with `-` and `_` |
| `encodebytes(s)`, `decodebytes(s)` | MIME style: a newline after every 76 characters |
| `get_implementation()` | the kernel simdutf selected for this processor (`icelake`, `haswell`, `arm64`, ...) |
| `simdutf_version`, `__version__` | versions of simdutf and of fastpybase64 |

The functions accept what `base64` accepts: `bytes`, `bytearray`,
`memoryview` and other buffers, and ASCII `str` for the decoders.

### Same results as `base64`

simdutf decodes base64 following the WHATWG forgiving-base64 rules, used by
`atob` in web browsers. These rules differ from those of Python's `binascii`
module: simdutf skips ASCII spaces and accepts missing padding, whereas the
standard library requires padding and, with `validate=True`, rejects spaces.
fastpybase64 keeps Python's behavior. Each call decodes with simdutf, then a
cheap check confirms that `base64` would return the same bytes for this
input. When it cannot confirm it, fastpybase64 calls `base64` with the same
arguments and returns its result or lets its exception propagate. So invalid
input raises exactly the `binascii.Error` that `base64` would raise, with
the same message, and the rare inputs where `base64` is lenient in odd ways
(characters after the padding, for example) give the same bytes.

* With `validate=False` (the default), spaces, newlines and other characters
  outside the alphabet are ignored, as in `base64`. simdutf skips spaces and
  line breaks while it decodes: 1 MiB of MIME data (a newline every 76
  characters) decodes at 12.0 GB/s on an Apple M4 Max, against 14.3 GB/s
  without newlines (pybase64: 4.1 GB/s, `base64`: 1.3 GB/s).
* With `validate=True`, any character outside the alphabet, including a
  space or a newline, raises `binascii.Error`.
* Characters after the padding are handled as in the version of Python you
  run: Python 3.13 and earlier stop decoding at the padding, Python 3.14
  continues or raises.
* `urlsafe_b64decode`, like the standard library, accepts both alphabets
  (`+/` and `-_`).
* An `altchars` other than `b"+/"` and `b"-_"` is handled by encoding with
  simdutf and then replacing the two characters; decoding with such an
  alphabet is left to `base64`.

### Threads

The GIL is released while encoding or decoding 64 KiB or more, so several
threads can work at once. The module is marked free-threading compatible
(`Py_MOD_GIL_NOT_USED`), so importing it on a free-threaded build of Python
does not enable the GIL. Do not change a `bytearray` while another thread
encodes or decodes it: the result would be unpredictable (its size cannot
change, so memory stays safe).

## Build and test

Python 3.10 or newer, and a C++17 compiler (clang, GCC or MSVC). The simdutf
9.3.2 amalgamation is in `vendor/`. Only its base64 functions are compiled
(see `setup.py`).

pip:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -U pip
python -m pip install -e ".[test]"
pytest tests
```

uv:

```sh
uv venv
. .venv/bin/activate
uv pip install -e ".[test]"
pytest tests
```

The tests compare every function with `base64`, results and exceptions, on
inputs of every length from 0 to 199 bytes and larger ones, on the edge cases
of padding and of characters outside the alphabet, on every supported input
type, on wrong arguments, and on thousands of randomly corrupted inputs.

`bench.py` compares fastpybase64 with `base64` and pybase64 (the `bench`
extra) from 16 bytes to 16 MiB:

```sh
python -m pip install -e ".[bench]"
python bench.py
python bench.py --sizes 64,4096,65536 --ops b64encode,b64decode
```

## Benchmarks

Intel Xeon Gold 6548N (Emerald Rapids), one core, Python 3.14.6,
fastpybase64 0.1.1 (simdutf 9.3.2, `icelake` kernel), pybase64 1.5.1 (its
AVX512VBMI kernel), random bytes. Speeds are in GB/s of binary data, for
encoding and decoding alike (higher is better).

### Decoding

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdutf/fastpybase64/main/doc/perf_decode_dark.png">
  <img src="https://raw.githubusercontent.com/simdutf/fastpybase64/main/doc/perf_decode.png" width="80%" alt="Decoding 64 KiB: fastpybase64 b64decode 32.20 GB/s, fastpybase64 validate=True 30.49, pybase64 validate=True 14.74, pybase64 b64decode 3.44, base64 validate=True 0.55, base64 b64decode 0.49">
</picture>

| function | size | `base64` | pybase64 | fastpybase64 |
|---|---:|---:|---:|---:|
| `b64decode` | 64 B | 0.24 | 0.43 | **0.97** |
| | 4 KiB | 0.48 | 3.06 | **21.51** |
| | 64 KiB | 0.49 | 3.44 | **32.20** |
| | 1 MiB | 0.50 | 3.47 | **19.16** |
| `b64decode(validate=True)` | 64 B | 0.19 | 0.23 | **0.42** |
| | 4 KiB | 0.53 | 7.22 | **14.24** |
| | 64 KiB | 0.55 | 14.74 | **30.49** |
| | 1 MiB | 0.55 | 15.39 | **20.64** |
| `urlsafe_b64decode` | 64 B | 0.16 | 0.16 | **0.98** |
| | 64 KiB | 0.41 | 2.52 | **34.14** |
| `decodebytes` | 64 KiB | 0.49 | | **12.19** |

pybase64 is fast only with `validate=True`; by default (`validate=False`, as
in `base64`) and in `urlsafe_b64decode`, it is 4 to 13 times slower than
fastpybase64 from 4 KiB on. pybase64 has no
`decodebytes`.

### Encoding

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdutf/fastpybase64/main/doc/perf_encode_dark.png">
  <img src="https://raw.githubusercontent.com/simdutf/fastpybase64/main/doc/perf_encode.png" width="80%" alt="Encoding 64 KiB: fastpybase64 b64encode 33.21 GB/s, pybase64 b64encode 32.31, fastpybase64 urlsafe_b64encode 30.52, pybase64 urlsafe_b64encode 9.59, base64 b64encode 0.98, base64 urlsafe_b64encode 0.69">
</picture>

| function | size | `base64` | pybase64 | fastpybase64 |
|---|---:|---:|---:|---:|
| `b64encode` | 64 B | 0.43 | 0.39 | **1.34** |
| | 4 KiB | 0.95 | 16.05 | **32.62** |
| | 64 KiB | 0.98 | 32.31 | 33.21 |
| | 1 MiB | 0.75 | 18.35 | 18.49 |
| `urlsafe_b64encode` | 64 B | 0.30 | 0.21 | **1.34** |
| | 64 KiB | 0.69 | 9.59 | **30.52** |
| `encodebytes` | 64 B | 0.11 | 0.50 | **1.29** |
| | 64 KiB | 0.38 | 1.18 | **22.08** |

From 64 KiB on, `b64encode` runs at about the same speed in pybase64 and
fastpybase64 (within a few percent): both use the same AVX-512 algorithm. On smaller inputs, and for the URL-safe
alphabet and MIME line breaks, fastpybase64 is faster.

### Small inputs

On small inputs, the cost of the call dominates. fastpybase64 parses its
arguments without creating objects and writes the result directly into the
`bytes` object it returns.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/simdutf/fastpybase64/main/doc/perf_small_dark.png">
  <img src="https://raw.githubusercontent.com/simdutf/fastpybase64/main/doc/perf_small.png" width="80%" alt="Time per call for 64 bytes: fastpybase64 b64encode 48 ns, pybase64 164 ns, base64 149 ns; fastpybase64 b64decode 66 ns, pybase64 149 ns, base64 267 ns">
</picture>

### Apple M4 Max

macOS, Python 3.14.5, simdutf's `arm64` (NEON) kernel, pybase64 with its
NEON kernel. GB/s:

| function | size | `base64` | pybase64 | fastpybase64 |
|---|---:|---:|---:|---:|
| `b64encode` | 64 B | 0.57 | 0.79 | **1.67** |
| | 64 KiB | 1.17 | 24.77 | 24.60 |
| `b64decode` | 64 B | 0.34 | 0.63 | **1.19** |
| | 64 KiB | 0.94 | 4.34 | **14.32** |
| `b64decode(validate=True)` | 64 KiB | 0.98 | 12.49 | **13.97** |
| `urlsafe_b64decode` | 64 KiB | 0.67 | 3.22 | **13.17** |
| `encodebytes` | 64 KiB | 0.47 | 8.78 | **13.35** |

## Releases

`./release.sh X.Y.Z` sets the version in `pyproject.toml` and
`src/fastpybase64.cpp`, runs the tests in a fresh virtual environment,
commits, tags `vX.Y.Z` and pushes. The tag starts the `buildwheels` workflow,
which builds the wheels and the source distribution and uploads them to PyPI.

## References

* Daniel Lemire, [Fast Forgiving Base64 Decoding with SIMD Instructions](https://dl.acm.org/doi/10.1145/3859286), ACM Transactions on Architecture and Code Optimization, 2026.
* Wojciech Muła, Daniel Lemire, [Base64 encoding and decoding at almost the speed of a memory copy](https://arxiv.org/abs/1910.05109), Software: Practice and Experience 50 (2), 2020.
* Wojciech Muła, Daniel Lemire, [Faster Base64 Encoding and Decoding using AVX2 Instructions](https://arxiv.org/abs/1704.00605), ACM Transactions on the Web 12 (3), 2018.

## License

Apache-2.0 or MIT, at your option, like simdutf.
