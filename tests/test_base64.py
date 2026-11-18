"""Differential tests: fastpybase64 must behave exactly like the stdlib."""

import array
import base64
import random

import pytest

import fastpybase64

FUNCTIONS = [
    "b64encode",
    "b64decode",
    "standard_b64encode",
    "standard_b64decode",
    "urlsafe_b64encode",
    "urlsafe_b64decode",
    "encodebytes",
    "decodebytes",
]


def outcome(func, *args, **kwargs):
    try:
        return ("ok", func(*args, **kwargs))
    except Exception as e:  # noqa: BLE001
        return ("error", type(e), str(e))


def check(name, *args, **kwargs):
    expected = outcome(getattr(base64, name), *args, **kwargs)
    actual = outcome(getattr(fastpybase64, name), *args, **kwargs)
    assert actual == expected, (name, args, kwargs)
    if expected[0] == "ok":
        assert type(actual[1]) is bytes


def check_decoders(data):
    check("b64decode", data)
    check("b64decode", data, validate=True)
    check("b64decode", data, altchars=b"-_")
    check("b64decode", data, altchars=b"-_", validate=True)
    check("standard_b64decode", data)
    check("urlsafe_b64decode", data)
    if not isinstance(data, str):
        check("decodebytes", data)


def check_encoders(data):
    check("b64encode", data)
    check("b64encode", data, altchars=b"-_")
    check("b64encode", data, b"*!")
    check("standard_b64encode", data)
    check("urlsafe_b64encode", data)
    check("encodebytes", data)


@pytest.mark.parametrize("name", FUNCTIONS)
def test_exported(name):
    assert callable(getattr(fastpybase64, name))


def test_metadata():
    assert fastpybase64.simdutf_version
    assert fastpybase64.get_implementation()


def test_version_matches_package_metadata():
    from importlib.metadata import PackageNotFoundError, version

    try:
        expected = version("fastpybase64")
    except PackageNotFoundError:
        pytest.skip("fastpybase64 is not installed (in-place build)")
    assert fastpybase64.__version__ == expected


@pytest.mark.parametrize("length", list(range(0, 200)) + [1000, 4096, 65537, 1 << 20])
def test_roundtrip_lengths(length):
    data = random.Random(length).randbytes(length)
    check_encoders(data)
    for encoded in (
        base64.b64encode(data),
        base64.urlsafe_b64encode(data),
        base64.encodebytes(data),
    ):
        check_decoders(encoded)
        check_decoders(encoded.decode("ascii"))


EDGE_CASES = [
    b"",
    b"YQ==",
    b"YR==",
    b"YQ",
    b"YQ=",
    b"YQ===",
    b"YQ==YQ==",
    b"YQ== ",
    b" YQ==",
    b"YW Jj",
    b"YWJj\n",
    b"=YWJj",
    b"YWJjZ",
    b"YWJjZ===",
    b"Y",
    b"=",
    b"==",
    b"====",
    b"YQ=a=",
    b"YQ=\n=",
    b"YQ= =",
    b"YWJ!j",
    b"Y=Q=",
    b"YWJjYQ",
    b"YWJjYR==",
    b"YW\x00Jj",
    b"YW\vJj",
    b"YW-_",
    b"YW+/",
    b"YW-/",
    b"YQ==\x80",
    b"\xff\xfe",
    b"YQ==!",
    b"YQ==-",
    b"YQ!==",
    b"YQ=!=",
    b"!YQ==",
    b"YWJj=",
    b"YWJj==",
    b"YWJ=",
    b"YWJ==",
    b"YWJ=!",
]


@pytest.mark.parametrize("data", EDGE_CASES)
def test_edge_cases(data):
    check_decoders(data)
    check_decoders(data.decode("latin-1"))


@pytest.mark.parametrize(
    "data",
    [
        bytearray(b"aGVsbG8="),
        memoryview(b"aGVsbG8="),
        memoryview(b"xaGVsbG8=x")[1:-1],
        memoryview(b"aGVsbG8=")[::2],
        array.array("B", b"aGVsbG8="),
        array.array("i", [1, 2, 3]),
        "aGVsbG8=",
        "aGVsbG8=é",
        12,
        None,
    ],
    ids=repr,
)
def test_decode_input_types(data):
    check_decoders(data)


@pytest.mark.parametrize(
    "data",
    [
        bytearray(b"hello"),
        memoryview(b"hello"),
        memoryview(b"hello world")[::2],
        array.array("B", b"hello"),
        array.array("i", [1, 2, 3]),
        memoryview(bytes(12)).cast("B", (3, 4)),
        "hello",
        12,
        None,
    ],
    ids=repr,
)
def test_encode_input_types(data):
    check_encoders(data)


@pytest.mark.parametrize(
    "altchars", [None, b"+/", b"-_", b"/+", b"*!", "-_", "+/", b"-", b"-_+", 3]
)
def test_altchars(altchars):
    data = bytes(range(256)) * 3
    check("b64encode", data, altchars)
    check("b64encode", data, altchars=altchars)
    for encoded in (base64.b64encode(data), base64.urlsafe_b64encode(data)):
        for validate in (False, True):
            check("b64decode", encoded, altchars, validate)
            check("b64decode", encoded, altchars=altchars, validate=validate)


def test_bad_arguments():
    check("b64encode")
    check("b64encode", b"x", b"-_", 3)
    check("b64encode", s=b"x")
    check("b64encode", b"x", s=b"x")
    check("b64encode", b"x", bogus=1)
    check("b64decode", b"YQ==", None, True, 4)
    check("b64decode", s=b"YQ==", validate=1)
    check("b64decode", b"YQ==", validate=[])
    check("urlsafe_b64decode", s=b"YQ==")
    check("decodebytes", b"YQ==", b"x")

    class Bad:
        def __bool__(self):
            raise RuntimeError("no truth")

    check("b64decode", b"YQ==", validate=Bad())
    check("b64decode", b"Y Q==", validate=Bad())


ALPHABET = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/-_="
NOISE = ALPHABET + b" \t\n\r\f\v\x00!.*\x80\xff"


def mutate(rng, data):
    data = bytearray(data)
    for _ in range(rng.randint(1, 4)):
        op = rng.randrange(4)
        pos = rng.randint(0, len(data))
        if op == 0:
            data.insert(pos, rng.choice(NOISE))
        elif op == 1 and data:
            del data[min(pos, len(data) - 1)]
        elif op == 2 and data:
            data[min(pos, len(data) - 1)] = rng.choice(NOISE)
        else:
            data[pos:pos] = b"=" * rng.randint(1, 3)
    return bytes(data)


@pytest.mark.parametrize("seed", range(20))
def test_fuzz(seed):
    rng = random.Random(seed)
    for _ in range(500):
        raw = rng.randbytes(rng.choice([rng.randint(0, 12), rng.randint(0, 200)]))
        encoded = rng.choice(
            [base64.b64encode, base64.urlsafe_b64encode, base64.encodebytes]
        )(raw)
        check_decoders(mutate(rng, encoded))


@pytest.mark.parametrize("seed", range(5))
def test_fuzz_random_strings(seed):
    rng = random.Random(1000 + seed)
    for _ in range(2000):
        length = rng.randint(0, 24)
        data = bytes(rng.choice(ALPHABET + b" \n!") for _ in range(length))
        check_decoders(data)


def test_large_with_noise():
    data = random.Random(7).randbytes(300_000)
    encoded = base64.encodebytes(data)
    check_decoders(encoded)
    check_decoders(encoded.replace(b"\n", b"!\n"))
    check_decoders(encoded + b"YQ==")


def test_exact_sizing_setting(sizing):
    assert fastpybase64.get_exact_sizing() is sizing
    fastpybase64.set_exact_sizing(0)
    assert fastpybase64.get_exact_sizing() is False
    fastpybase64.set_exact_sizing([1])
    assert fastpybase64.get_exact_sizing() is True
    with pytest.raises(TypeError):
        fastpybase64.set_exact_sizing()


@pytest.mark.parametrize("length", [0, 1, 2, 3, 57, 1000, 1 << 20])
def test_mime_crlf(length):
    data = random.Random(length).randbytes(length)
    encoded = base64.encodebytes(data).replace(b"\n", b"\r\n")
    check_decoders(encoded)
    check_decoders(b" \r\n\t" + encoded + b"\r\n \r\n")


def test_only_whitespace():
    check_decoders(b" \r\n" * 1000)
