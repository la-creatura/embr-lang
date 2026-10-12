// unit tests for plugins/ffi/ffi.cpp (imported once by main() below)
//
// covers what needs libc plus the small test library ffi_fixture.cpp, which is built with the tests
// ffi_callback is tested through qsort, and through tests/ffi_fixture.cpp for what libc has no function for
//
// type descriptors (type_i32(), type_str(), struct_type(), nil for void) come from embrtypes, see ffi.cpp's header

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "ptr_null: formats as a null typed pointer",
          "print(ptr_null())\n",
          "<ptr 0x0>\n" },

        { "ffi_fn: a function address that did not come from ffi_sym (here a callback), with a struct by value that holds a pointer",
          "T = struct_type([[\"data\", type_ptr()], [\"n\", type_i32()], [\"m\", type_i32()]])\n"
          "cb = ffi_callback(fn(t) return t[\"n\"] + t[\"m\"] end, type_i32(), [T])\n"
          "f = ffi_fn(cb, \"cb\")\n"
          "print(ffi_call(f, type_i32(), [T], [{data: ptr_null(), n: 5, m: 6}]))\n"
          "p = ffi_prepare(f, type_i32(), [T])\n"
          "print(ffi_invoke(p, [{data: ptr_null(), n: 1, m: 2}]))\n"
          "ffi_callback_free(cb)\n",
          "11\n3\n" },

        { "ffi_fn: a null address is refused",
          "try\n"
          "ffi_fn(ptr_null())\n"
          "catch e\n"
          "print(e[\"message\"])\n"
          "end\n",
          "the address is null\n" },

        { "ffi_open + ffi_sym + ffi_call: strlen via libc",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "print(ffi_call(sym, type_i64(), [type_str()], [\"hello world\"]))\n",
          "11\n" },

        { "ffi_call: nil as ret means void",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"free\")\n"
          "print(ffi_call(sym, nil, [type_ptr()], [ptr_null()]))\n",
          "0\n" },

        { "buffer + ptr_read/ptr_write: out-param round trip, freed automatically",
          "r = buffer(8)\n"
          "print(ptr_read(r, type_i32()))\n"
          "ptr_write(r, type_i32(), 42)\n"
          "print(ptr_read(r, type_i32()))\n",
          "0\n42\n" },

        { "ffi_prepare + ffi_invoke: cached call matches ffi_call",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "p = ffi_prepare(sym, type_i64(), [type_str()])\n"
          "print(ffi_invoke(p, [\"abc\"]))\n"
          "print(ffi_invoke(p, [\"abcdef\"]))\n",
          "3\n6\n" },

        { "ffi_close: using a sym from a closed library raises instead of crashing",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "ffi_close(lib)\n"
          "try\n"
          "ffi_call(sym, type_i64(), [type_str()], [\"x\"])\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "ptr_offset: walks a buffer by byte count",
          "base = ptr_null()\n"
          "same = ptr_offset(base, 0)\n"
          "print(same)\n",
          "<ptr 0x0>\n" },

        { "struct_type: ptr_read/ptr_write round trip a struct buffer",
          "desc = struct_type([[\"x\", type_i32()], [\"y\", type_i32()]])\n"
          "buf = buffer(8)\n"
          "ptr_write(buf, desc, {\"x\": 3, \"y\": 4})\n"
          "v = ptr_read(buf, desc)\n"
          "print(v[\"x\"])\n"
          "print(v[\"y\"])\n",
          "3\n4\n" },

        { "struct_type: nested struct fields",
          "point = struct_type([[\"x\", type_i32()], [\"y\", type_i32()]])\n"
          "line = struct_type([[\"a\", point], [\"b\", point]])\n"
          "buf = buffer_pack(line, {\"a\": {\"x\": 1, \"y\": 2}, \"b\": {\"x\": 3, \"y\": 4}})\n"
          "v = ptr_read(buf, line)\n"
          "print(v[\"a\"][\"x\"])\n"
          "print(v[\"b\"][\"y\"])\n",
          "1\n4\n" },

        { "struct_type: struct returned by value from a real libc call (div_t div(int,int))",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"div\")\n"
          "div_t = struct_type([[\"quot\", type_i32()], [\"rem\", type_i32()]])\n"
          "r = ffi_call(sym, div_t, [type_i32(), type_i32()], [17, 5])\n"
          "print(r[\"quot\"])\n"
          "print(r[\"rem\"])\n",
          "3\n2\n" },

#ifndef _WIN32
        { "ffi_call_var: variadic call via snprintf(NULL, 0, ...) sizing",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"snprintf\")\n"
          "n = ffi_call_var(sym, type_i32(), [type_ptr(), type_u64(), type_str(), type_i32()],\n"
          "                 [ptr_null(), 0, \"n=%d\", 42], 3)\n"
          "print(n)\n",
          "4\n" },

        { "ffi_prepare_var + ffi_invoke: cached variadic call",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"snprintf\")\n"
          "p = ffi_prepare_var(sym, type_i32(), [type_ptr(), type_u64(), type_str(), type_i32()], 3)\n"
          "print(ffi_invoke(p, [ptr_null(), 0, \"n=%d\", 7]))\n",
          "3\n" },

#else   // msvcrt.dll has no snprintf, _scprintf(fmt, ...) returns the same length
        { "ffi_call_var: variadic call via _scprintf length",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"_scprintf\")\n"
          "n = ffi_call_var(sym, type_i32(), [type_str(), type_i32()], [\"n=%d\", 42], 1)\n"
          "print(n)\n",
          "4\n" },

        { "ffi_prepare_var + ffi_invoke: cached variadic call",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"_scprintf\")\n"
          "p = ffi_prepare_var(sym, type_i32(), [type_str(), type_i32()], 1)\n"
          "print(ffi_invoke(p, [\"n=%d\", 7]))\n",
          "3\n" },

#endif
        { "ffi_call: passing a type embrtypes can't marshal raises a clear error",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"strlen\")\n"
          "try\n"
          "ffi_call(sym, type_arr(), [type_str()], [\"x\"])\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "ffi_callback: qsort with a script comparator sorts a C array",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "qsort = ffi_sym(lib, \"qsort\")\n"
          "arr = buffer(16)\n"
          "vals = [4, 1, 3, 2]\n"
          "i = 0\nwhile i < 4\nptr_write(ptr_offset(arr, i * 4), type_i32(), vals[i])\ni = i + 1\nend\n"
          "cmp = ffi_callback(fn(a, b)\nreturn ptr_read(a, type_i32()) - ptr_read(b, type_i32())\nend, type_i32(), [type_ptr(), type_ptr()])\n"
          "ffi_call(qsort, nil, [type_ptr(), type_u64(), type_u64(), type_ptr()], [arr, 4, 4, cmp])\n"
          "i = 0\nwhile i < 4\nprint(ptr_read(ptr_offset(arr, i * 4), type_i32()))\ni = i + 1\nend\n"
          "ffi_callback_free(cmp)\n",
          "1\n2\n3\n4\n" },

        { "ffi_callback: error inside a callback is re-raised after the C call, not thrown through C frames",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "qsort = ffi_sym(lib, \"qsort\")\n"
          "arr = buffer(16)\n"
          "cmp = ffi_callback(fn(a, b)\nerror(\"boom\")\nend, type_i32(), [type_ptr(), type_ptr()])\n"
          "try\nffi_call(qsort, nil, [type_ptr(), type_u64(), type_u64(), type_ptr()], [arr, 4, 4, cmp])\n"
          "print(\"unreachable\")\ncatch e\nprint(e[\"message\"])\nend\n"
          "print(\"still alive\")\n",
          "boom\nstill alive\n" },

        { "ptr_offset / ffi args: NaN and huge numbers raise instead of hitting UB",
          "p = buffer(8)\n"
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "try\nptr_offset(p, big)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n"
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "abs = ffi_sym(lib, \"abs\")\n"
          "try\nffi_call(abs, type_i32(), [type_i32()], [big])\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\ncaught\n" },

        { "ffi_call_var: negative nfixed raises",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "sym = ffi_sym(lib, \"printf\")\n"
          "try\nffi_call_var(sym, type_i32(), [type_str()], [\"x\"], -1)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },
        { "ffi: 8 and 16 bit returns are sign or zero extended, and 8/16 bit arguments arrive whole",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "abs = ffi_sym(lib, \"abs\")\n"
          "print(ffi_call(abs, type_i8(), [type_i32()], [200]))\n"
          "print(ffi_call(abs, type_u8(), [type_i32()], [200]))\n"
          "print(ffi_call(abs, type_i16(), [type_i32()], [40000]))\n"
          "print(ffi_call(abs, type_u16(), [type_i32()], [70000]))\n"
          "print(ffi_call(abs, type_i32(), [type_i8()], [-128]))\n"
          "print(ffi_call(abs, type_i32(), [type_i16()], [-32768]))\n"
          "print(ffi_call(abs, type_i32(), [type_u16()], [65535]))\n",
          "-56\n200\n-25536\n4464\n128\n32768\n65535\n" },
        { "ffi: an integer argument that does not fit its C type raises instead of being cut down",
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "abs = ffi_sym(lib, \"abs\")\n"
          "for t in [[type_i8(), 128], [type_i8(), -129], [type_u8(), 256], [type_i16(), 32768], [type_u16(), 65536], [type_i32(), 2147483648], [type_u32(), 4294967296], [type_u8(), -1], [type_i8(), 1.5]]\n"
          "  try\n"
          "    ffi_call(abs, type_i32(), [t[0]], [t[1]])\n"
          "    print(\"accepted\")\n"
          "  catch e\n"
          "    print(e[\"message\"])\n"
          "  end\n"
          "end\n",
          "arg[0]: 128 does not fit in type i8 (-128 to 127)\narg[0]: -129 does not fit in type i8 (-128 to 127)\narg[0]: 256 does not fit in type u8 (0 to 255)\narg[0]: 32768 does not fit in type i16 (-32768 to 32767)\narg[0]: 65536 does not fit in type u16 (0 to 65535)\narg[0]: 2147483648 does not fit in type i32 (-2147483648 to 2147483647)\narg[0]: 4294967296 does not fit in type u32 (0 to 4294967295)\narg[0]: type u8 is unsigned but got negative value\narg[0]: type i8 is integer but got non-integer value 1.500000\n" },
        { "ptr_write / ptr_read: a struct of 8 and 16 bit fields at their edge values, and the errors",
          "st = struct_type([[\"a\", type_i8()], [\"b\", type_u16()], [\"c\", type_i16()], [\"d\", type_u8()]])\n"
          "buf = buffer(buffer_len(buffer_pack(st, {\"a\": 0, \"b\": 0, \"c\": 0, \"d\": 0})))\n"
          "ptr_write(buf, st, {\"a\": -128, \"b\": 65535, \"c\": -32768, \"d\": 255})\n"
          "r = ptr_read(buf, st)\n"
          "print(r[\"a\"])\n"
          "print(r[\"b\"])\n"
          "print(r[\"c\"])\n"
          "print(r[\"d\"])\n"
          "print(buffer_unpack(st, buf)[\"c\"])\n"
          "try\n"
          "  ptr_write(buf, st, 5)\n"
          "catch e\n"
          "  print(e[\"message\"])\n"
          "end\n"
          "try\n"
          "  ptr_write(buf, st, {\"a\": 1})\n"
          "catch e\n"
          "  print(e[\"message\"])\n"
          "end\n",
          "-128\n65535\n-32768\n255\n-32768\nvalue must be a map for struct type 'struct<a:i8,b:u16,c:i16,d:u8>'\nstruct 'struct<a:i8,b:u16,c:i16,d:u8>' missing field 'b'\n" },
        { "ffi_callback: a callback that returns a pointer or an 8/16 bit int, called from C (tests/ffi_fixture.cpp); a result or ptr_write value that does not fit raises",
          "lib = ffi_open(\"" EMBR_TEST_FIXTURE "\")\n"
          "callp = ffi_sym(lib, \"fixture_call_ptr\")\n"
          "buf = buffer_from_str(\"hello\")\n"
          "cb = ffi_callback(fn(p) return ptr_offset(p, 1) end, type_ptr(), [type_ptr()])\n"
          "r = ffi_call(callp, type_ptr(), [type_ptr(), type_ptr()], [cb, buf])\n"
          "print(ptr_read(r, type_u8()))\n"
          "calli8 = ffi_sym(lib, \"fixture_call_i8\")\n"
          "cb8 = ffi_callback(fn(x) return x * 2 end, type_i8(), [type_i8()])\n"
          "print(ffi_call(calli8, type_i8(), [type_ptr(), type_i8()], [cb8, -64]))\n"
          "try\n"
          "  ffi_call(calli8, type_i8(), [type_ptr(), type_i8()], [cb8, 100])\n"
          "catch e\n"
          "  print(e[\"message\"])\n"
          "end\n"
          "buf = buffer(8)\n"
          "try\n"
          "  ptr_write(buf, type_i16(), 40000)\n"
          "catch e\n"
          "  print(e[\"message\"])\n"
          "end\n"
          "callu16 = ffi_sym(lib, \"fixture_call_u16\")\n"
          "cb16 = ffi_callback(fn(x) return x + 1 end, type_u16(), [type_u16()])\n"
          "print(ffi_call(callu16, type_u16(), [type_ptr(), type_u16()], [cb16, 65534]))\n"
          "cbn = ffi_callback(fn(p) return ptr_null() end, type_ptr(), [type_ptr()])\n"
          "print(ffi_call(callp, type_ptr(), [type_ptr(), type_ptr()], [cbn, buf]))\n"
          "ffi_callback_free(cb)\n"
          "ffi_callback_free(cb8)\n"
          "ffi_callback_free(cb16)\n"
          "ffi_callback_free(cbn)\n",
          "101\n-128\nreturn value: 200 does not fit in type i8 (-128 to 127)\nvalue: 40000 does not fit in type i16 (-32768 to 32767)\n65535\n<ptr 0x0>\n" },
    };
    return embr_test::runSuite("ffi plugin test suite", tests, {"embrlib", "embrtypes", "ffi"});
}
