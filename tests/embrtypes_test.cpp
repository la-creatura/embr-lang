// unit tests for plugins/embrtypes/embrtypes.cpp (imported once by main() below)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "type_check: scalar kinds",
          "print(type_check(type_int(), 5))\n"
          "print(type_check(type_int(), 5.5))\n"
          "print(type_check(type_float(), 5.5))\n"
          "print(type_check(type_num(), 5))\n"
          "print(type_check(type_num(), 5.5))\n"
          "print(type_check(type_str(), \"x\"))\n"
          "print(type_check(type_str(), 1))\n",
          "1\n0\n1\n1\n1\n1\n0\n" },

        { "type_arr: homogeneous element type",
          "t = type_arr(type_int())\n"
          "print(type_check(t, [1,2,3]))\n"
          "print(type_check(t, [1,\"x\",3]))\n"
          "print(type_check(t, []))\n",
          "1\n0\n1\n" },

        { "type_arr: untyped matches any array",
          "print(type_check(type_arr(), [1,\"x\",{}]))\n"
          "print(type_check(type_arr(), {}))\n",
          "1\n0\n" },

        { "type_map: homogeneous value type",
          "t = type_map(type_str())\n"
          "print(type_check(t, {\"a\": \"x\", \"b\": \"y\"}))\n"
          "print(type_check(t, {\"a\": \"x\", \"b\": 2}))\n",
          "1\n0\n" },

        { "nested: arr of arr of int",
          "t = type_arr(type_arr(type_int()))\n"
          "print(type_check(t, [[1,2],[3,4]]))\n"
          "print(type_check(t, [[1,2],[\"x\"]]))\n",
          "1\n0\n" },

        { "type_union: matches any alternative",
          "t = type_union([type_int(), type_str()])\n"
          "print(type_check(t, 5))\n"
          "print(type_check(t, \"hi\"))\n"
          "print(type_check(t, 5.5))\n",
          "1\n1\n0\n" },

        { "type_ptr: no tags matches any pointer",
          "print(type_check(type_ptr(), ptr_null()))\n"
          "print(type_check(type_ptr(), 5))\n",
          "1\n0\n" },

        { "type_ptr: multiple tags is a union of typed pointers",
          "t = type_name(type_ptr(\"a.x\", \"a.y\"))\n"
          "print(t)\n",
          "ptr<a.x|a.y>\n" },

        { "type_name: renders composite descriptors",
          "print(type_name(type_arr(type_int())))\n"
          "print(type_name(type_map(type_str())))\n"
          "print(type_name(type_union([type_int(), type_str()])))\n"
          "print(type_name(type_any()))\n",
          "arr<int>\nmap<str>\nunion<int|str>\nany\n" },

        { "type_assert: passes through the value on match",
          "print(type_assert(type_int(), 42))\n",
          "42\n" },

        { "type_assert: raises a catchable error on mismatch",
          "try\n"
          "type_assert(type_int(), \"nope\")\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught: \" + e[\"message\"])\n"
          "end\n",
          "caught: expected int, got string\n" },

        { "type_assert: custom message",
          "try\n"
          "type_assert(type_int(), \"nope\", \"custom msg\")\n"
          "catch e\n"
          "print(e[\"message\"])\n"
          "end\n",
          "custom msg\n" },

        { "type_of: infers uniform array element type",
          "print(type_name(type_of([1,2,3])))\n"
          "print(type_name(type_of([1,\"x\",3])))\n"
          "print(type_name(type_of([])))\n",
          "arr<int>\narr\narr\n" },

        { "type_of: infers uniform map value type",
          "print(type_name(type_of({\"a\": 1, \"b\": 2})))\n"
          "print(type_name(type_of({\"a\": 1, \"b\": \"x\"})))\n",
          "map<int>\nmap\n" },

        { "type_of: nested containers",
          "print(type_name(type_of([[1,2],[3,4]])))\n",
          "arr<arr<int>>\n" },

        { "type_of: scalars and pointers",
          "print(type_name(type_of(5)))\n"
          "print(type_name(type_of(5.5)))\n"
          "print(type_name(type_of(\"x\")))\n"
          "print(type_name(type_of(ptr_null())))\n",
          "int\nfloat\nstr\nptr\n" },

        { "true/false: truthy() treats them correctly",
          "print(if_do(true, fn() return \"t\" end, fn() return \"f\" end))\n"
          "print(if_do(false, fn() return \"t\" end, fn() return \"f\" end))\n",
          "t\nf\n" },

        { "type_bool: recognizes true/false but not other pointers or numbers",
          "print(type_check(type_bool(), true))\n"
          "print(type_check(type_bool(), false))\n"
          "print(type_check(type_bool(), 1))\n"
          "print(type_check(type_bool(), ptr_null()))\n",
          "1\n1\n0\n0\n" },

        { "type_of: infers a bool as ptr<bool>",
          "print(type_name(type_of(true)))\n",
          "ptr<bool>\n" },

        { "make_true/make_false: zero-arg callables usable as a while_do condition",
          "while_do(make_false(), fn() print(\"unreachable\") end)\n"
          "print(\"never ran\")\n"
          "print(type_check(type_bool(), call(make_true(), [])))\n"
          "print(if_do(call(make_true(), []), fn() return \"t\" end, fn() return \"f\" end))\n"
          "print(if_do(call(make_false(), []), fn() return \"t\" end, fn() return \"f\" end))\n",
          "never ran\n1\nt\nf\n" },

        { "nil: distinct from false, and falsy (a null pointer tagged differently than false)",
          "print(is_nil(nil))\n"
          "print(is_nil(false))\n"
          "print(is_nil(0))\n"
          "print(!nil)\n"
          "print(nil == false)\n",
          "1\n0\n0\n1\n0\n" },

        { "type_nil: recognizes nil but not false or other pointers",
          "print(type_check(type_nil(), nil))\n"
          "print(type_check(type_nil(), false))\n"
          "print(type_check(type_nil(), ptr_null()))\n",
          "1\n0\n0\n" },

        { "struct_type: named heterogeneous shape validation",
          "person = struct_type([[\"name\", type_str()], [\"age\", type_int()]])\n"
          "print(type_check(person, {\"name\": \"sam\", \"age\": 30}))\n"
          "print(type_check(person, {\"name\": \"sam\"}))\n"
          "print(type_check(person, {\"name\": 5, \"age\": 30}))\n"
          "print(type_name(person))\n",
          "1\n0\n0\nstruct<name:str,age:int>\n" },

        { "buffer_pack/buffer_unpack: round trips a packable struct",
          "point = struct_type([[\"x\", type_i32()], [\"y\", type_i32()]])\n"
          "buf = buffer_pack(point, {\"x\": 10, \"y\": -5})\n"
          "print(buffer_len(buf))\n"
          "back = buffer_unpack(point, buf)\n"
          "print(back[\"x\"])\n"
          "print(back[\"y\"])\n",
          "8\n10\n-5\n" },

        { "buffer_pack/buffer_unpack: nested packable structs",
          "point = struct_type([[\"x\", type_i32()], [\"y\", type_i32()]])\n"
          "line = struct_type([[\"a\", point], [\"b\", point]])\n"
          "buf = buffer_pack(line, {\"a\": {\"x\": 1, \"y\": 2}, \"b\": {\"x\": 3, \"y\": 4}})\n"
          "print(buffer_len(buf))\n"
          "back = buffer_unpack(line, buf)\n"
          "print(back[\"a\"][\"x\"])\n"
          "print(back[\"b\"][\"y\"])\n",
          "16\n1\n4\n" },

        { "buffer_pack/buffer_unpack: a bare fixed-width descriptor, not just structs",
          "buf = buffer_pack(type_f64(), 3.5)\n"
          "print(buffer_len(buf))\n"
          "print(buffer_unpack(type_f64(), buf))\n",
          "8\n3.5\n" },

        { "buffer_pack: raises on a struct_type with a non-packable field",
          "bad = struct_type([[\"name\", type_str()]])\n"
          "try\n"
          "buffer_pack(bad, {\"name\": \"x\"})\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "struct_type: a ptr field is pointer-sized and packs, a nested struct is aligned like C",
          "T = struct_type([[\"data\", type_ptr()], [\"n\", type_i32()], [\"m\", type_i32()]])\n"
          "b = buffer_pack(T, {data: ptr_null(), n: 5, m: 6})\n"
          "print(buffer_len(b))\n"
          "r = buffer_unpack(T, b)\n"
          "print(r[\"n\"], r[\"m\"], r[\"data\"])\n"
          "inner = struct_type([[\"x\", type_f32()], [\"y\", type_f32()]])\n"
          "outer = struct_type([[\"c\", type_i8()], [\"i\", inner]])\n"
          "print(buffer_len(buffer_pack(outer, {c: 1, i: {x: 1.5, y: 2.5}})))\n"
          "try\n"
          "buffer_pack(T, {data: 5, n: 1, m: 2})\n"
          "catch e\n"
          "print(\"a number is not a ptr\")\n"
          "end\n",
          "16\n5 6 <ptr 0x0>\n12\na number is not a ptr\n" },

        { "buffer: raw byte get/set/slice/concat/str round trip",
          "raw = buffer(2)\n"
          "buffer_set(raw, 0, 65)\n"
          "buffer_set(raw, 1, 66)\n"
          "print(buffer_get(raw, 0))\n"
          "print(buffer_get(raw, 1))\n"
          "s = buffer_from_str(\"hi\")\n"
          "print(buffer_to_str(s))\n"
          "print(buffer_to_str(buffer_concat(s, s)))\n"
          "print(buffer_to_str(buffer_slice(buffer_from_str(\"hello\"), 1, 3)))\n",
          "65\n66\nhi\nhihi\nel\n" },

        { "type_buffer: recognizes buffers but not other values",
          "print(type_check(type_buffer(), buffer(1)))\n"
          "print(type_check(type_buffer(), 5))\n",
          "1\n0\n" },

        { "buffer: a huge or NaN-ish size / index / value raises instead of bad_alloc or UB",
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "try\n  buffer(big)\n  print(\"unreachable\")\ncatch e\n  print(\"size capped\")\nend\n"
          "try\n  buffer(100000000000)\n  print(\"unreachable\")\ncatch e\n  print(\"size capped 2\")\nend\n"
          "b = buffer(4)\n"
          "try\n  buffer_get(b, big)\n  print(\"unreachable\")\ncatch e\n  print(\"index rejected\")\nend\n"
          "try\n  buffer_set(b, 0, big)\n  print(\"unreachable\")\ncatch e\n  print(\"value rejected\")\nend\n"
          "buffer_set(b, 1, 300)\nprint(buffer_get(b, 1))\n",
          "size capped\nsize capped 2\nindex rejected\nvalue rejected\n44\n" },
        { "buffer_pack/buffer_unpack: every integer width at its edge values (size, then value)",
          "for t in [[type_i8(), -128], [type_i8(), 127], [type_u8(), 255], [type_i16(), -32768], [type_i16(), 32767], [type_u16(), 65535], [type_i32(), -2147483648], [type_u32(), 4294967295], [type_i64(), -9223372036854775807]]\n"
          "  b = buffer_pack(t[0], t[1])\n"
          "  print(str(buffer_len(b)) + \" \" + str(buffer_unpack(t[0], b)))\n"
          "end\n"
          "print(type_name(type_i8()) + \" \" + type_name(type_u8()) + \" \" + type_name(type_i16()) + \" \" + type_name(type_u16()) + \" \" + type_name(type_u32()) + \" \" + type_name(type_f32()))\n",
          "1 -128\n1 127\n1 255\n2 -32768\n2 32767\n2 65535\n4 -2147483648\n4 4294967295\n8 -9223372036854775807\ni8 u8 i16 u16 u32 f32\n" },
        { "buffer_pack: a value that would lose bytes (too big, negative for unsigned, a fraction) raises and names the field",
          "st = struct_type([[\"a\", type_i8()], [\"b\", type_u16()]])\n"
          "tries = [fn() return buffer_pack(type_u8(), 300) end, fn() return buffer_pack(type_i8(), 200) end, fn() return buffer_pack(type_u16(), -1) end, fn() return buffer_pack(type_i32(), 1.5) end, fn() return buffer_pack(st, {\"a\": 1, \"b\": 70000}) end, fn() return buffer_pack(type_u8(), \"x\") end]\n"
          "for t in tries\n"
          "  try\n"
          "    t()\n"
          "    print(\"accepted\")\n"
          "  catch e\n"
          "    print(e[\"message\"])\n"
          "  end\n"
          "end\n",
          "value: 300 does not fit in type u8 (0 to 255)\nvalue: 200 does not fit in type i8 (-128 to 127)\nvalue: type u16 is unsigned but got negative value\nvalue: type i32 is integer but got non-integer value 1.500000\nfield 'b': 70000 does not fit in type u16 (0 to 65535)\nvalue: expects u8, got string\n" },
    };
    return embr_test::runSuite("embrtypes plugin test suite", tests, {"embrtypes", "ffi", "embrlib"});
}
