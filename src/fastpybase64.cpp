// fastpybase64: base64 encoding/decoding backed by simdutf.
//
// Every public function is a drop-in replacement for its counterpart in the
// standard library's `base64` module. The fast path handles the common cases
// with simdutf. Whenever the input or the arguments fall outside what the fast
// path can prove it handles identically to the standard library (unusual
// argument types, custom alphabets, malformed input...), we call the standard
// library with the very same arguments, so that results and exceptions match.

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstring>

#define FASTPYBASE64_VERSION "0.1.1"

// Unity build: compiling simdutf in this translation unit keeps the build
// simple and lets the compiler inline across the boundary.
#include "simdutf.cpp"

namespace {

// Release the GIL when processing at least this many bytes.
constexpr Py_ssize_t kReleaseGilThreshold = 64 * 1024;

// stdlib.encodebytes emits lines of at most 76 characters.
constexpr size_t kMimeLineLength = 76;

// ---------------------------------------------------------------------------
// Fallback to the standard library
// ---------------------------------------------------------------------------

PyObject *call_stdlib(const char *name, PyObject *const *args,
                      Py_ssize_t nargs, PyObject *kwnames) {
  PyObject *module = PyImport_ImportModule("base64");
  if (module == nullptr) {
    return nullptr;
  }
  PyObject *func = PyObject_GetAttrString(module, name);
  Py_DECREF(module);
  if (func == nullptr) {
    return nullptr;
  }
  PyObject *result = PyObject_Vectorcall(func, args, nargs, kwnames);
  Py_DECREF(func);
  return result;
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

// Minimal vectorcall argument parser. Returns false (without setting an
// exception) on any problem: the caller then defers to the standard library,
// which produces the appropriate error message.
bool parse_args(PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames,
                const char *const *names, int nparams, int nrequired,
                PyObject **out) {
  if (nargs > nparams) {
    return false;
  }
  for (int i = 0; i < nparams; i++) {
    out[i] = nullptr;
  }
  for (Py_ssize_t i = 0; i < nargs; i++) {
    out[i] = args[i];
  }
  if (kwnames != nullptr) {
    Py_ssize_t nkw = PyTuple_GET_SIZE(kwnames);
    for (Py_ssize_t k = 0; k < nkw; k++) {
      PyObject *key = PyTuple_GET_ITEM(kwnames, k);
      int index = -1;
      for (int i = 0; i < nparams; i++) {
        if (PyUnicode_CompareWithASCIIString(key, names[i]) == 0) {
          index = i;
          break;
        }
      }
      if (index < 0 || out[index] != nullptr) {
        return false;
      }
      out[index] = args[nargs + k];
    }
  }
  for (int i = 0; i < nrequired; i++) {
    if (out[i] == nullptr) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Input views
// ---------------------------------------------------------------------------

// A read-only view over the bytes of an argument, owning the buffer if any.
struct InputView {
  Py_buffer buffer{};
  bool has_buffer = false;
  const char *data = nullptr;
  size_t length = 0;

  InputView() = default;
  InputView(const InputView &) = delete;
  InputView &operator=(const InputView &) = delete;
  ~InputView() {
    if (has_buffer) {
      PyBuffer_Release(&buffer);
    }
  }

  // Contiguous bytes-like object (what binascii accepts).
  bool from_bytes_like(PyObject *obj) {
    if (PyBytes_CheckExact(obj)) {
      data = PyBytes_AS_STRING(obj);
      length = size_t(PyBytes_GET_SIZE(obj));
      return true;
    }
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_SIMPLE) != 0) {
      PyErr_Clear();
      return false;
    }
    has_buffer = true;
    data = static_cast<const char *>(buffer.buf);
    length = size_t(buffer.len);
    return true;
  }

  // Bytes-like object or ASCII string (what base64.b64decode accepts).
  bool from_decode_data(PyObject *obj) {
    if (PyUnicode_Check(obj)) {
      if (!PyUnicode_IS_ASCII(obj)) {
        return false;
      }
      data = static_cast<const char *>(PyUnicode_DATA(obj));
      length = size_t(PyUnicode_GET_LENGTH(obj));
      return true;
    }
    return from_bytes_like(obj);
  }

  // One-dimensional byte buffer (what base64._input_type_check accepts).
  bool from_byte_buffer(PyObject *obj) {
    if (PyBytes_CheckExact(obj)) {
      return from_bytes_like(obj);
    }
    if (PyObject_GetBuffer(obj, &buffer, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) !=
        0) {
      PyErr_Clear();
      return false;
    }
    has_buffer = true;
    const char *format = buffer.format;
    bool byte_format = format == nullptr || std::strcmp(format, "B") == 0 ||
                       std::strcmp(format, "b") == 0 ||
                       std::strcmp(format, "c") == 0;
    if (buffer.ndim != 1 || !byte_format) {
      return false;
    }
    data = static_cast<const char *>(buffer.buf);
    length = size_t(buffer.len);
    return true;
  }
};

// Reads a two-character alphabet override; returns false if `obj` is not a
// bytes object or ASCII string of length 2.
bool read_altchars(PyObject *obj, char altchars[2]) {
  const char *data;
  Py_ssize_t length;
  if (PyBytes_Check(obj)) {
    data = PyBytes_AS_STRING(obj);
    length = PyBytes_GET_SIZE(obj);
  } else if (PyUnicode_Check(obj) && PyUnicode_IS_ASCII(obj)) {
    data = static_cast<const char *>(PyUnicode_DATA(obj));
    length = PyUnicode_GET_LENGTH(obj);
  } else {
    return false;
  }
  if (length != 2) {
    return false;
  }
  altchars[0] = data[0];
  altchars[1] = data[1];
  return true;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

PyObject *encode(const InputView &input, simdutf::base64_options options) {
  size_t out_length = simdutf::base64_length_from_binary(input.length, options);
  PyObject *result = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(out_length));
  if (result == nullptr) {
    return nullptr;
  }
  char *out = PyBytes_AS_STRING(result);
  if (Py_ssize_t(input.length) >= kReleaseGilThreshold) {
    Py_BEGIN_ALLOW_THREADS
    simdutf::binary_to_base64(input.data, input.length, out, options);
    Py_END_ALLOW_THREADS
  } else {
    simdutf::binary_to_base64(input.data, input.length, out, options);
  }
  return result;
}

// Same as encode(), followed by a substitution of '+' and '/'.
PyObject *encode_with_altchars(const InputView &input, const char altchars[2]) {
  PyObject *result = encode(input, simdutf::base64_default);
  if (result == nullptr) {
    return nullptr;
  }
  char *out = PyBytes_AS_STRING(result);
  Py_ssize_t length = PyBytes_GET_SIZE(result);
  const char plus = altchars[0];
  const char slash = altchars[1];
  // Store every byte so the compiler can vectorize the substitution.
  for (Py_ssize_t i = 0; i < length; i++) {
    const char c = out[i];
    out[i] = c == '+' ? plus : (c == '/' ? slash : c);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

inline bool is_ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

inline bool is_alphabet(char c, bool url) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '+' || c == '/' ||
         (url && (c == '-' || c == '_'));
}

// Number of '=' characters the standard library expects after the data
// characters, given the number of decoded bytes.
inline size_t required_padding(size_t decoded_length) {
  static const size_t padding[3] = {0, 2, 1};
  return padding[decoded_length % 3];
}

// Fast-path acceptance checks. simdutf is more lenient than the standard
// library in some respects (it ignores ASCII spaces and tolerates missing
// padding). Each check only accepts results for which the standard library
// would return the exact same bytes; anything else goes to the fallback.

// validate=True: binascii.a2b_base64(strict_mode=True) rejects spaces and
// requires full padding. With L input characters, a padding-terminated input
// free of spaces has L % 4 == 0 and decodes to 3 * L / 4 - (trailing '='s)
// bytes. Any space would make the decoded length strictly smaller.
bool strict_result_ok(const char *in, size_t length, size_t decoded) {
  if (length % 4 != 0) {
    return false;
  }
  size_t padding = 0;
  while (padding < length && in[length - 1 - padding] == '=') {
    padding++;
  }
  return decoded + padding == length / 4 * 3;
}

// validate=False, simdutf succeeded without accept_garbage: the input is data
// characters and spaces, followed by optional padding. The standard library
// ignores the spaces too, but requires the exact amount of padding.
bool lenient_result_ok(const char *in, size_t length, size_t decoded) {
  size_t end = length;
  while (end > 0 && is_ascii_space(in[end - 1])) {
    end--;
  }
  size_t padding = 0;
  while (padding < end && in[end - 1 - padding] == '=') {
    padding++;
  }
  return padding == required_padding(decoded);
}

// validate=False, simdutf succeeded with accept_garbage: simdutf decoded the
// data characters located before the first '=' (if any). Accept the result if
// that '=' starts a run of exactly the required length and nothing that the
// standard library would decode or treat as padding comes afterward.
bool garbage_result_ok(const char *in, size_t length, size_t decoded,
                       bool url) {
  const char *end = in + length;
  const char *pad =
      static_cast<const char *>(std::memchr(in, '=', length));
  size_t required = required_padding(decoded);
  if (pad == nullptr) {
    if (required != 0) {
      return false;
    }
    // With accept_garbage, simdutf silently drops a lone trailing data
    // character, whereas the standard library raises: count them.
    size_t data_characters = 0;
    for (size_t i = 0; i < length; i++) {
      data_characters += is_alphabet(in[i], url);
    }
    return data_characters % 4 == 0;
  }
  const char *p = pad;
  while (p < end && *p == '=') {
    p++;
  }
  if (size_t(p - pad) != required) {
    return false;
  }
  for (; p < end; p++) {
    if (*p == '=' || is_alphabet(*p, url)) {
      return false;
    }
  }
  return true;
}

simdutf::result run_decoder(const InputView &input, char *out,
                            simdutf::base64_options options) {
  simdutf::result r;
  if (Py_ssize_t(input.length) >= kReleaseGilThreshold) {
    Py_BEGIN_ALLOW_THREADS
    r = simdutf::base64_to_binary(input.data, input.length, out, options);
    Py_END_ALLOW_THREADS
  } else {
    r = simdutf::base64_to_binary(input.data, input.length, out, options);
  }
  return r;
}

// Attempts a fast decode. Returns a new reference on success; returns nullptr
// with no exception set when the caller must fall back to the standard
// library, and nullptr with an exception set on a genuine error.
PyObject *try_decode(const InputView &input, bool url, bool validate) {
  if (input.length == 0) {
    return PyBytes_FromStringAndSize(nullptr, 0);
  }
  size_t max_length =
      simdutf::maximal_binary_length_from_base64(input.data, input.length);
  PyObject *result = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(max_length));
  if (result == nullptr) {
    return nullptr;
  }
  char *out = PyBytes_AS_STRING(result);

  simdutf::base64_options options =
      url ? simdutf::base64_default_or_url : simdutf::base64_default;
  simdutf::result r = run_decoder(input, out, options);
  bool ok;
  if (r.error == simdutf::error_code::SUCCESS) {
    ok = validate ? strict_result_ok(input.data, input.length, r.count)
                  : lenient_result_ok(input.data, input.length, r.count);
  } else if (!validate) {
    // The input may contain characters that the standard library discards.
    options = url ? simdutf::base64_default_or_url_accept_garbage
                  : simdutf::base64_default_accept_garbage;
    r = run_decoder(input, out, options);
    ok = r.error == simdutf::error_code::SUCCESS &&
         garbage_result_ok(input.data, input.length, r.count, url);
  } else {
    ok = false;
  }
  if (!ok) {
    Py_DECREF(result);
    return nullptr;
  }
  if (r.count != max_length) {
    if (_PyBytes_Resize(&result, Py_ssize_t(r.count)) != 0) {
      return nullptr;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

PyObject *b64encode(PyObject *, PyObject *const *args, Py_ssize_t nargs,
                    PyObject *kwnames) {
  static const char *const names[] = {"s", "altchars"};
  PyObject *params[2];
  InputView input;
  if (parse_args(args, nargs, kwnames, names, 2, 1, params) &&
      input.from_bytes_like(params[0])) {
    PyObject *altchars_obj = params[1];
    if (altchars_obj == nullptr || altchars_obj == Py_None) {
      return encode(input, simdutf::base64_default);
    }
    char altchars[2];
    if (PyBytes_Check(altchars_obj) && read_altchars(altchars_obj, altchars)) {
      if (altchars[0] == '+' && altchars[1] == '/') {
        return encode(input, simdutf::base64_default);
      }
      if (altchars[0] == '-' && altchars[1] == '_') {
        return encode(input, simdutf::base64_url_with_padding);
      }
      return encode_with_altchars(input, altchars);
    }
  }
  return call_stdlib("b64encode", args, nargs, kwnames);
}

PyObject *b64decode(PyObject *, PyObject *const *args, Py_ssize_t nargs,
                    PyObject *kwnames) {
  static const char *const names[] = {"s", "altchars", "validate"};
  PyObject *params[3];
  InputView input;
  if (parse_args(args, nargs, kwnames, names, 3, 1, params) &&
      input.from_decode_data(params[0])) {
    bool known_alphabet = true;
    bool url = false;
    PyObject *altchars_obj = params[1];
    if (altchars_obj != nullptr && altchars_obj != Py_None) {
      char altchars[2];
      if (read_altchars(altchars_obj, altchars) && altchars[0] == '+' &&
          altchars[1] == '/') {
        url = false;
      } else if (read_altchars(altchars_obj, altchars) && altchars[0] == '-' &&
                 altchars[1] == '_') {
        // The standard library maps '-_' to '+/' and then decodes, so it
        // accepts both alphabets.
        url = true;
      } else {
        known_alphabet = false;
      }
    }
    // Only bool and int values of validate mean the same thing in every
    // Python version: before 3.12, binascii rejects other types.
    int validate = 0;
    if (params[2] != nullptr) {
      if (PyLong_Check(params[2])) {
        validate = PyObject_IsTrue(params[2]);
      } else {
        known_alphabet = false;
      }
    }
    if (known_alphabet) {
      PyObject *result = try_decode(input, url, validate != 0);
      if (result != nullptr || PyErr_Occurred()) {
        return result;
      }
    }
  }
  return call_stdlib("b64decode", args, nargs, kwnames);
}

PyObject *encode_one_arg(const char *name, PyObject *const *args,
                         Py_ssize_t nargs, PyObject *kwnames,
                         simdutf::base64_options options) {
  static const char *const names[] = {"s"};
  PyObject *params[1];
  InputView input;
  if (parse_args(args, nargs, kwnames, names, 1, 1, params) &&
      input.from_bytes_like(params[0])) {
    return encode(input, options);
  }
  return call_stdlib(name, args, nargs, kwnames);
}

PyObject *decode_one_arg(const char *name, PyObject *const *args,
                         Py_ssize_t nargs, PyObject *kwnames, bool url) {
  static const char *const names[] = {"s"};
  PyObject *params[1];
  InputView input;
  if (parse_args(args, nargs, kwnames, names, 1, 1, params) &&
      input.from_decode_data(params[0])) {
    PyObject *result = try_decode(input, url, false);
    if (result != nullptr || PyErr_Occurred()) {
      return result;
    }
  }
  return call_stdlib(name, args, nargs, kwnames);
}

PyObject *standard_b64encode(PyObject *, PyObject *const *args,
                             Py_ssize_t nargs, PyObject *kwnames) {
  return encode_one_arg("standard_b64encode", args, nargs, kwnames,
                        simdutf::base64_default);
}

PyObject *standard_b64decode(PyObject *, PyObject *const *args,
                             Py_ssize_t nargs, PyObject *kwnames) {
  return decode_one_arg("standard_b64decode", args, nargs, kwnames, false);
}

PyObject *urlsafe_b64encode(PyObject *, PyObject *const *args,
                            Py_ssize_t nargs, PyObject *kwnames) {
  return encode_one_arg("urlsafe_b64encode", args, nargs, kwnames,
                        simdutf::base64_url_with_padding);
}

PyObject *urlsafe_b64decode(PyObject *, PyObject *const *args,
                            Py_ssize_t nargs, PyObject *kwnames) {
  return decode_one_arg("urlsafe_b64decode", args, nargs, kwnames, true);
}

PyObject *encodebytes(PyObject *, PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwnames) {
  static const char *const names[] = {"s"};
  PyObject *params[1];
  InputView input;
  if (parse_args(args, nargs, kwnames, names, 1, 1, params) &&
      input.from_byte_buffer(params[0])) {
    if (input.length == 0) {
      return PyBytes_FromStringAndSize(nullptr, 0);
    }
    // simdutf separates lines with '\n'; the standard library also
    // terminates the last line.
    size_t encoded_length = simdutf::base64_length_from_binary_with_lines(
        input.length, simdutf::base64_default, kMimeLineLength);
    PyObject *result =
        PyBytes_FromStringAndSize(nullptr, Py_ssize_t(encoded_length + 1));
    if (result == nullptr) {
      return nullptr;
    }
    char *out = PyBytes_AS_STRING(result);
    if (Py_ssize_t(input.length) >= kReleaseGilThreshold) {
      Py_BEGIN_ALLOW_THREADS
      simdutf::binary_to_base64_with_lines(input.data, input.length, out,
                                           kMimeLineLength);
      Py_END_ALLOW_THREADS
    } else {
      simdutf::binary_to_base64_with_lines(input.data, input.length, out,
                                           kMimeLineLength);
    }
    out[encoded_length] = '\n';
    return result;
  }
  return call_stdlib("encodebytes", args, nargs, kwnames);
}

PyObject *decodebytes(PyObject *, PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwnames) {
  static const char *const names[] = {"s"};
  PyObject *params[1];
  InputView input;
  if (parse_args(args, nargs, kwnames, names, 1, 1, params) &&
      input.from_byte_buffer(params[0])) {
    PyObject *result = try_decode(input, false, false);
    if (result != nullptr || PyErr_Occurred()) {
      return result;
    }
  }
  return call_stdlib("decodebytes", args, nargs, kwnames);
}

PyObject *get_implementation(PyObject *, PyObject *) {
  const simdutf::implementation *impl = simdutf::get_active_implementation();
  auto name = impl->name();
  return PyUnicode_FromStringAndSize(name.data(), Py_ssize_t(name.size()));
}

#define FASTCALL_KW(f) reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(f))

PyMethodDef methods[] = {
    {"b64encode", FASTCALL_KW(b64encode), METH_FASTCALL | METH_KEYWORDS,
     "b64encode(s, altchars=None)\n--\n\n"
     "Encode the bytes-like object s using Base64 and return a bytes object."},
    {"b64decode", FASTCALL_KW(b64decode), METH_FASTCALL | METH_KEYWORDS,
     "b64decode(s, altchars=None, validate=False)\n--\n\n"
     "Decode the Base64 encoded bytes-like object or ASCII string s."},
    {"standard_b64encode", FASTCALL_KW(standard_b64encode),
     METH_FASTCALL | METH_KEYWORDS,
     "standard_b64encode(s)\n--\n\n"
     "Encode bytes-like object s using the standard Base64 alphabet."},
    {"standard_b64decode", FASTCALL_KW(standard_b64decode),
     METH_FASTCALL | METH_KEYWORDS,
     "standard_b64decode(s)\n--\n\n"
     "Decode bytes encoded with the standard Base64 alphabet."},
    {"urlsafe_b64encode", FASTCALL_KW(urlsafe_b64encode),
     METH_FASTCALL | METH_KEYWORDS,
     "urlsafe_b64encode(s)\n--\n\n"
     "Encode bytes using the URL- and filesystem-safe Base64 alphabet."},
    {"urlsafe_b64decode", FASTCALL_KW(urlsafe_b64decode),
     METH_FASTCALL | METH_KEYWORDS,
     "urlsafe_b64decode(s)\n--\n\n"
     "Decode bytes using the URL- and filesystem-safe Base64 alphabet."},
    {"encodebytes", FASTCALL_KW(encodebytes), METH_FASTCALL | METH_KEYWORDS,
     "encodebytes(s)\n--\n\n"
     "Encode a bytestring into a bytes object containing multiple lines of "
     "base-64 data."},
    {"decodebytes", FASTCALL_KW(decodebytes), METH_FASTCALL | METH_KEYWORDS,
     "decodebytes(s)\n--\n\n"
     "Decode a bytestring of base-64 data into a bytes object."},
    {"get_implementation", get_implementation, METH_NOARGS,
     "get_implementation()\n--\n\n"
     "Return the name of the SIMD kernel selected by simdutf at runtime."},
    {nullptr, nullptr, 0, nullptr},
};

int module_exec(PyObject *module) {
  if (PyModule_AddStringConstant(module, "__version__",
                                 FASTPYBASE64_VERSION) < 0) {
    return -1;
  }
  return PyModule_AddStringConstant(module, "simdutf_version",
                                    SIMDUTF_VERSION);
}

PyModuleDef_Slot slots[] = {
    {Py_mod_exec, reinterpret_cast<void *>(module_exec)},
#ifdef Py_mod_multiple_interpreters
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#ifdef Py_mod_gil
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, nullptr},
};

PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "fastpybase64",
    "Fast Base64 encoding and decoding backed by simdutf.\n\n"
    "The functions mirror the standard library's base64 module and return\n"
    "identical results (and raise identical exceptions) for every input.",
    0,
    methods,
    slots,
    nullptr,
    nullptr,
    nullptr,
};

} // namespace

PyMODINIT_FUNC PyInit_fastpybase64(void) { return PyModuleDef_Init(&module_def); }
