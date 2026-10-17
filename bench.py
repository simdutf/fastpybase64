"""Compare fastpybase64 with the standard library and pybase64.

Usage: python bench.py [--sizes 16,1024,1048576] [--time 0.2]

Throughput is reported in GB/s of *binary* (decoded) data for both encoding
and decoding, so that the numbers of the two directions are comparable. For
each case we report the best of several repetitions.
"""

import argparse
import base64
import platform
import random
import sys
import timeit

import fastpybase64

LIBRARIES = {"base64 (stdlib)": base64, "fastpybase64": fastpybase64}
try:
    import pybase64

    LIBRARIES["pybase64"] = pybase64
except ImportError:
    pybase64 = None

# name -> (function name, keyword arguments, input preparation)
OPERATIONS = {
    "b64encode": ("b64encode", {}, lambda raw: raw),
    "b64encode(altchars)": ("b64encode", {"altchars": b"*!"}, lambda raw: raw),
    "b64decode": ("b64decode", {}, base64.b64encode),
    "b64decode(validate=True)": ("b64decode", {"validate": True}, base64.b64encode),
    "urlsafe_b64encode": ("urlsafe_b64encode", {}, lambda raw: raw),
    "urlsafe_b64decode": ("urlsafe_b64decode", {}, base64.urlsafe_b64encode),
    "encodebytes": ("encodebytes", {}, lambda raw: raw),
    "decodebytes": ("decodebytes", {}, base64.encodebytes),
}

DEFAULT_SIZES = [16, 64, 256, 1024, 4096, 65536, 1 << 20, 16 << 20]


def human_size(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:g} {unit}"
        n /= 1024


def measure(func, arg, kwargs, min_time, repeat):
    if kwargs:
        def call():
            func(arg, **kwargs)
    else:
        def call():
            func(arg)
    timer = timeit.Timer(call)
    number, elapsed = timer.autorange()
    number = max(1, int(number * min_time / max(elapsed, 1e-9)))
    return min(timer.repeat(repeat=repeat, number=number)) / number


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--sizes",
        default=",".join(map(str, DEFAULT_SIZES)),
        help="comma-separated binary sizes in bytes",
    )
    parser.add_argument("--time", type=float, default=0.2, help="seconds per sample")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument(
        "--ops", default=",".join(OPERATIONS), help="comma-separated operations"
    )
    args = parser.parse_args()
    sizes = [int(s) for s in args.sizes.split(",")]
    ops = args.ops.split(",")

    print(f"Python {sys.version.split()[0]} on {platform.machine()} ({platform.platform()})")
    print(
        f"fastpybase64 {fastpybase64.__version__} "
        f"(simdutf {fastpybase64.simdutf_version}, kernel: "
        f"{fastpybase64.get_implementation()})"
    )
    if pybase64 is not None:
        print(f"pybase64 {pybase64.get_version()}")
    print()
    print("Throughput in GB/s of binary data (higher is better).")
    print()

    rng = random.Random(42)
    for op in ops:
        func_name, kwargs, prepare = OPERATIONS[op]
        print(f"### {op}")
        print()
        names = [n for n, m in LIBRARIES.items() if hasattr(m, func_name)]
        header = ["size"] + names + ["speedup vs stdlib", "speedup vs pybase64"]
        print("| " + " | ".join(header) + " |")
        print("|" + "|".join("---:" for _ in header) + "|")
        for size in sizes:
            raw = rng.randbytes(size)
            arg = prepare(raw)
            expected = getattr(base64, func_name)(arg, **kwargs)
            speeds = {}
            for name in names:
                func = getattr(LIBRARIES[name], func_name)
                assert func(arg, **kwargs) == expected, (name, op, size)
                seconds = measure(func, arg, kwargs, args.time, args.repeat)
                speeds[name] = size / seconds / 1e9
            row = [human_size(size)] + [f"{speeds[n]:.2f}" for n in names]
            row.append(f"{speeds['fastpybase64'] / speeds['base64 (stdlib)']:.1f}x")
            if "pybase64" in speeds:
                row.append(f"{speeds['fastpybase64'] / speeds['pybase64']:.2f}x")
            else:
                row.append("n/a")
            print("| " + " | ".join(row) + " |", flush=True)
        print()


if __name__ == "__main__":
    main()
