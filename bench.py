"""Compare fastpybase64 with the standard library and pybase64.

Usage: python bench.py [--sizes 16,1024,1048576] [--time 0.2] [--same-process]
                       [--exact-sizing]

Throughput is reported in GB/s of *binary* (decoded) data for both encoding
and decoding, so that the numbers of the two directions are comparable. For
each case we report the best of several repetitions.
"""

import argparse
import base64
import json
import os
import platform
import random
import subprocess
import sys
import tempfile
import timeit

import fastpybase64

LIBRARIES = {"base64 (stdlib)": base64, "fastpybase64": fastpybase64}
try:
    import pybase64

    LIBRARIES["pybase64"] = pybase64
except ImportError:
    pybase64 = None


def mime_crlf(raw):
    return base64.encodebytes(raw).replace(b"\n", b"\r\n")


# name -> (function name, keyword arguments, input preparation)
OPERATIONS = {
    "b64encode": ("b64encode", {}, lambda raw: raw),
    "b64decode": ("b64decode", {}, base64.b64encode),
    "b64decode(validate=True)": ("b64decode", {"validate": True}, base64.b64encode),
    "urlsafe_b64encode": ("urlsafe_b64encode", {}, lambda raw: raw),
    "urlsafe_b64decode": ("urlsafe_b64decode", {}, base64.urlsafe_b64encode),
    "encodebytes": ("encodebytes", {}, lambda raw: raw),
    "decodebytes": ("decodebytes", {}, base64.encodebytes),
    # MIME as found in email: lines of 76 characters ending with CRLF.
    "b64decode(MIME CRLF)": ("b64decode", {}, mime_crlf),
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


def measure_libraries(op, arg, size, min_time, repeat):
    """Returns {library name: GB/s} for one operation on one input."""
    func_name, kwargs, _ = OPERATIONS[op]
    names = [n for n, m in LIBRARIES.items() if hasattr(m, func_name)]
    speeds, outputs = {}, {}
    # fastpybase64 goes first, and the results are checked afterwards: the
    # other libraries may free large temporary buffers, which would change
    # the allocator's state (see run_in_child).
    for name in sorted(names, key=lambda n: n != "fastpybase64"):
        func = getattr(LIBRARIES[name], func_name)
        outputs[name] = func(arg, **kwargs)
        seconds = measure(func, arg, kwargs, min_time, repeat)
        speeds[name] = size / seconds / 1e9
    expected = getattr(base64, func_name)(arg, **kwargs)
    for name, output in outputs.items():
        assert output == expected, (name, op, size)
    return speeds


def run_in_child(op, arg, size, args):
    # Each measurement runs in a fresh process that reads its input from a
    # file. The allocator remembers what was freed before: glibc raises its
    # mmap threshold after freeing a large block, so building the input (or
    # an earlier measurement) could hide the cost of allocating large
    # outputs, a cost that a program decoding a file it just read does pay.
    with tempfile.NamedTemporaryFile(suffix=".bench", delete=False) as f:
        f.write(arg)
    try:
        out = subprocess.run(
            [sys.executable, __file__, "--child", f.name, "--ops", op,
             "--sizes", str(size), "--time", str(args.time),
             "--repeat", str(args.repeat)]
            + (["--exact-sizing"] if args.exact_sizing else []),
            check=True, stdout=subprocess.PIPE, text=True,
        ).stdout
    finally:
        os.unlink(f.name)
    return json.loads(out)


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
    parser.add_argument(
        "--same-process",
        action="store_true",
        help="measure in this process (by default each measurement runs in a fresh one)",
    )
    parser.add_argument(
        "--exact-sizing",
        action="store_true",
        help="call fastpybase64.set_exact_sizing(True) before measuring",
    )
    parser.add_argument("--child", metavar="FILE", help=argparse.SUPPRESS)
    args = parser.parse_args()
    sizes = [int(s) for s in args.sizes.split(",")]
    ops = args.ops.split(",")
    for op in ops:
        if op not in OPERATIONS:
            parser.error(f"unknown operation {op!r}; choose among {', '.join(OPERATIONS)}")

    fastpybase64.set_exact_sizing(args.exact_sizing)
    if args.child:
        with open(args.child, "rb") as f:
            arg = f.read()
        speeds = measure_libraries(ops[0], arg, sizes[0], args.time, args.repeat)
        print(json.dumps(speeds))
        return
    print_header()
    run(ops, sizes, args)


def print_header():
    print(f"Python {sys.version.split()[0]} on {platform.machine()} ({platform.platform()})")
    print(
        f"fastpybase64 {fastpybase64.__version__} "
        f"(simdutf {fastpybase64.simdutf_version}, kernel: "
        f"{fastpybase64.get_implementation()}"
        f"{', exact sizing' if fastpybase64.get_exact_sizing() else ''})"
    )
    if pybase64 is not None:
        print(f"pybase64 {pybase64.get_version()}")
    print()
    print("Throughput in GB/s of binary data (higher is better).")
    print(flush=True)


def run(ops, sizes, args):
    rng = random.Random(42)
    for op in ops:
        func_name, _, prepare = OPERATIONS[op]
        print(f"### {op}")
        print()
        names = [n for n, m in LIBRARIES.items() if hasattr(m, func_name)]
        header = ["size"] + names + ["speedup vs stdlib", "speedup vs pybase64"]
        print("| " + " | ".join(header) + " |")
        print("|" + "|".join("---:" for _ in header) + "|", flush=True)
        for size in sizes:
            arg = prepare(rng.randbytes(size))
            if args.same_process:
                speeds = measure_libraries(op, arg, size, args.time, args.repeat)
            else:
                speeds = run_in_child(op, arg, size, args)
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
