// unit tests for dunder dispatch (core/registry.h: dunderName/resolveDunder/arithDunderOp/lookupMapChain, used by
// vm.h's Op::CALL, arithmetic opcodes and Op::INDEX_GET)
//
// it's a core feature with no plugin of its own, but testing it needs a tagged pointer to hang a dunder on, so
// embrtypes' descriptors (tag "types.desc") and table() handles (tag "table.handle") are used as fixtures

#include "harness.h"

// dunderName() is a pure C++ helper (core/registry.h) with no embr syntax
// to hang a test on -- there's no way for a script to construct an
// arbitrary custom-tagged pointer to probe its collision behavior against.
// checked directly here instead of via embr_test::Test/runSuite.
static bool checkDunderNameCollisionFix() {
    // the exact collision case from the card this fixes: a naive "just
    // replace '.' with '_'" scheme maps both of these to "__a_b_c_call".
    // escaping (doubling literal '_', not just mapping '.') must diverge them.
    std::string a = embr::dunderName("a.b_c", "call");
    std::string b = embr::dunderName("a_b.c", "call");
    bool ok = a != b;
    std::cout << (ok ? "\033[32m [+]\033[0m" : "\033[31m [-]\033[0m")
              << " dunderName: \"a.b_c\" and \"a_b.c\" no longer collide (\""
              << a << "\" vs \"" << b << "\")\n";
    return ok;
}

int main() {
    using embr_test::Test;

    std::cout << "\n\033[1m dunder dispatch test suite \033[0m\n";
    bool rawOk = checkDunderNameCollisionFix();

    std::vector<Test> tests = {

        { "pointer printing: nil/false/true reflect their tag and fixed sentinel address",
          "print(nil)\nprint(false)\nprint(true)\n",
          "<nil 0x0>\n<bool 0x0>\n<bool 0x1>\n" },

        { "true: the same fixed sentinel address across independently-constructed plugins (embrtypes vs json)",
          "print(true == json_parse(\"true\"))\n"
          "print(false == json_parse(\"false\"))\n",
          "1\n1\n" },

        { "pointer equality: still requires the SAME address, not just the same tag (two distinct type_int() descriptors)",
          "print(type_int() == type_int())\n",
          "0\n" },

        { "dunder call: a typed pointer that isn't callable dispatches to __tag_call(pointer, ...args)",
          "fn __types_desc_call(d, x)\n"
          "    return \"called with \" + str(x)\n"
          "end\n"
          "d = type_int()\n"
          "print(d(5))\n",
          "called with 5\n" },

        { "dunder index: v[k] on a typed pointer calls __tag_index(v, k) as an ordinary call, also inside a function",
          "fn __types_desc_index(d, k)\n    return \"idx \" + str(k)\nend\n"
          "d = type_int()\nprint(d[3])\nprint(d[\"a\"])\n"
          "fn look(p, i)\n    return p[i] + \"!\"\nend\nprint(look(d, 9))\n",
          "idx 3\nidx a\nidx 9!\n" },

        { "dunder index: without __tag_index a typed pointer still can't be indexed, and the error says so",
          "d = type_int()\ntry\n    print(d[0])\ncatch e\n    print(\"caught\")\nend\n",
          "caught\n" },

        { "dunder setindex: v[k] = x calls __tag_setindex(v, k, x), and what it returns is stored back into the variable",
          "fn __types_desc_setindex(d, k, v)\n    print(\"set\", k, v)\n    return d\nend\n"
          "fn __types_desc_index(d, k)\n    return \"idx \" + str(k)\nend\n"
          "d = type_int()\nd[1] = 5\nd[\"k\"] = \"v\"\nprint(d[0])\n"
          "fn put(p)\n    p[2] = 7\n    return p[2]\nend\nprint(put(d))\n",
          "set 1 5\nset k v\nidx 0\nset 2 7\nidx 2\n" },

        { "dunder iter: for x in v calls __tag_iter(v) and loops over what it returns (an array, a map or a string)",
          "fn __types_desc_iter(d)\n    return [7, 8, 9]\nend\n"
          "for x in type_int()\n    print(x)\nend\n"
          "fn __table_handle_iter(t)\n    return {\"a\": 1}\nend\n"
          "for k in table()\n    print(k)\nend\n",
          "7\n8\n9\na\n" },

        { "dunder arithmetic: a script dunder gets a frame of its own and the result goes where the operator's result would",
          "fn __types_desc_add(a, b)\n    return \"sum\"\nend\n"
          "d = type_int()\nx = d + 1\nprint(x)\nprint((d + d) + 2)\n"
          "fn twice(p)\n    return p + p\nend\nprint(twice(d))\n",
          "sum\nsum2\nsum\n" },

        { "dunder call: still raises cleanly when no __tag_call is defined for the tag",
          "t = table()\n"
          "try\n"
          "t(1)\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "dunder arithmetic: __tag_add is tried on either operand",
          "fn __types_desc_add(a, b)\n"
          "    return \"sum-ish\"\n"
          "end\n"
          "d2 = type_str()\n"
          "print(d2 + 1)\n"
          "print(1 + d2)\n",
          "sum-ish\nsum-ish\n" },

        { "dunder arithmetic: comparison operators are dispatchable too (__tag_gt)",
          "fn __types_desc_gt(a, b)\n"
          "    return 1\n"
          "end\n"
          "print(type_str() > 5)\n",
          "1\n" },

        { "dunder unary: __tag_neg handles unary '-' on a non-numeric operand",
          "fn __types_desc_neg(a)\n"
          "    return \"negated\"\n"
          "end\n"
          "print(-type_int())\n",
          "negated\n" },

        { "dunder: the bit operators go through __tag_band, bor, bxor, shl, shr and bnot",
          "fn __types_desc_band(a, b)\n    return \"band\"\nend\n"
          "fn __types_desc_bor(a, b)\n    return \"bor\"\nend\n"
          "fn __types_desc_bxor(a, b)\n    return \"bxor\"\nend\n"
          "fn __types_desc_shl(a, b)\n    return \"shl\"\nend\n"
          "fn __types_desc_shr(a, b)\n    return \"shr\"\nend\n"
          "fn __types_desc_bnot(a)\n    return \"bnot\"\nend\n"
          "t = type_int()\n"
          "print(t & 1, t | 1, t ^ 1, 1 << t, t >> 1, ~t)\n",
          "band bor bxor shl shr bnot\n" },

        { "dunder unary: still raises cleanly when no __tag_neg is defined for the tag",
          "try\n"
          "-table()\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "map prototype chain: a missing own key falls back to a map __proto__'s own slot",
          "base = {\"greet\": fn(self) return \"hi \" + self[\"name\"] end}\n"
          "obj = {\"name\": \"sam\", \"__proto__\": base}\n"
          "print(obj[\"greet\"](obj))\n",
          "hi sam\n" },

        { "map prototype chain: multi-level (obj -> proto -> proto's own proto)",
          "base = {\"greet\": fn(self) return \"hi \" + self[\"name\"] end}\n"
          "mid = {\"__proto__\": base}\n"
          "obj = {\"name\": \"al\", \"__proto__\": mid}\n"
          "print(obj[\"greet\"](obj))\n",
          "hi al\n" },

        { "map prototype chain: an own key shadows the same key on the prototype",
          "base = {\"x\": 1}\n"
          "obj = {\"x\": 2, \"__proto__\": base}\n"
          "print(obj[\"x\"])\n",
          "2\n" },

        { "map prototype chain: a typed-pointer __proto__ falls back to a __tag_<key> global",
          "fn __types_desc_area(self)\n"
          "    return 42\n"
          "end\n"
          "shape_obj = {\"__proto__\": type_int()}\n"
          "print(shape_obj[\"area\"](shape_obj))\n",
          "42\n" },

        { "map prototype chain: still raises 'key not found' on a total miss",
          "obj = {\"__proto__\": {\"y\": 1}}\n"
          "try\n"
          "print(obj[\"z\"])\n"
          "print(\"unreachable\")\n"
          "catch e\n"
          "print(\"caught\")\n"
          "end\n",
          "caught\n" },

        // embrlib functions that consult dunders: str()/len()/has() use __tag_str/__tag_len/__tag_has. call()/if_do()/
        // while_do()/do()/for_each_do()/filter()/reduce()/sort_do() accept a dunder-callable pointer wherever they take
        // a fn (they pass it to embr::invoke(), which resolves __tag_call). fn_name()/fn_arity()/fn_sig()/is_native()
        // describe the __tag_call function itself for such a pointer

        { "type(): a __tag_type override replaces the generic \"ptr\" TypeTag name",
          "fn __types_desc_type(d)\n"
          "    return \"typedesc\"\n"
          "end\n"
          "print(type(type_int()))\n",
          "typedesc\n" },

        { "type(): falls back to the generic TypeTag name when no __tag_type is defined",
          "print(type(table()))\n",
          "pointer\n" },

        { "str(): a __tag_str override replaces the generic <TAG 0xADDR> form",
          "fn __types_desc_str(d)\n"
          "    return \"a type descriptor\"\n"
          "end\n"
          "print(str(type_int()))\n",
          "a type descriptor\n" },

        { "str(): falls back to the generic form when no __tag_str is defined",
          "t = table()\n"
          "print(str_startswith(str(t), \"<table.handle \"))\n",
          "1\n" },

        { "len(): a __tag_len override lets len() work on a typed pointer",
          "fn __table_handle_len(t)\n"
          "    return 3\n"
          "end\n"
          "print(len(table()))\n",
          "3\n" },

        { "len(): still raises cleanly when no __tag_len is defined for the tag",
          "try\nlen(type_int())\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "has(): a __tag_has override lets has() work on a typed pointer",
          "fn __table_handle_has(t, k)\n"
          "    return table_has(t, k)\n"
          "end\n"
          "t = table()\n"
          "table_set(t, \"x\", 1)\n"
          "print(has(t, \"x\"))\n"
          "print(has(t, \"y\"))\n",
          "1\n0\n" },

        { "call(): a dunder-callable pointer works the same as a real function",
          "fn __types_desc_call(d, x)\n"
          "    return x * 2\n"
          "end\n"
          "print(call(type_int(), [21]))\n",
          "42\n" },

        { "if_do(): accepts a dunder-callable pointer as its then_fn/else_fn (called with no extra args)",
          "fn __types_desc_call(d)\n"
          "    return \"branch taken\"\n"
          "end\n"
          "print(if_do(1, type_int(), type_int()))\n",
          "branch taken\n" },

        { "filter(): accepts a dunder-callable pointer as its predicate (called with one extra arg)",
          "fn __types_desc_call(d, x)\n"
          "    return x > 2\n"
          "end\n"
          "print(filter([1,2,3,4], type_int()))\n",
          "[3, 4]\n" },

        { "reduce(): also accepts a dunder-callable pointer as its combining function",
          "fn __types_desc_call(d, acc, el)\n"
          "    return acc + el\n"
          "end\n"
          "print(reduce([1,2,3,4], type_int(), 0))\n",
          "10\n" },

        { "fn_name/fn_sig/is_native: introspect a dunder-callable pointer's __tag_call function",
          "fn __types_desc_call(d, x)\n"
          "    return x\n"
          "end\n"
          // fn_name() includes @file:line for a script fn (the harness
          // names each test's own source after its test name), so check
          // the prefix rather than an exact match.
          "print(str_startswith(fn_name(type_int()), \"__types_desc_call@\"))\n"
          "print(fn_sig(type_int()))\n"
          "print(is_native(type_int()))\n",
          "1\n__types_desc_call(d, x)\n0\n" },

        { "fn_arity: a dunder-callable pointer's arity discounts the dunder's own synthetic leading argument",
          "fn __types_desc_call(d, x, y)\n"
          "    return x + y\n"
          "end\n"
          "print(fn_arity(type_int()))\n"
          "print(fn_arity(fn(a, b) return a end))\n",
          "2\n2\n" },

        { "fn_name/fn_arity/etc: raise cleanly for a plain non-callable pointer with no __tag_call",
          "try\nfn_arity(table())\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "sort(): an array of typed pointers sharing a tag uses that tag's __tag_lt dunder as the comparator",
          "fn __types_desc_lt(a, b)\n"
          "    return str_ord(type_name(a)[0]) < str_ord(type_name(b)[0])\n"
          "end\n"
          "sorted = sort([type_str(), type_int()])\n"
          "print(type_name(sorted[0]))\n"
          "print(type_name(sorted[1]))\n",
          "int\nstr\n" },

        { "sort(): descending reverses the __tag_lt-sorted order (no __tag_gt needed)",
          "fn __types_desc_lt(a, b)\n"
          "    return str_ord(type_name(a)[0]) < str_ord(type_name(b)[0])\n"
          "end\n"
          "sorted = sort([type_str(), type_int()], 1)\n"
          "print(type_name(sorted[0]))\n"
          "print(type_name(sorted[1]))\n",
          "str\nint\n" },

        { "sort(): still raises cleanly for a non-numeric array with no shared __tag_lt",
          "try\nsort([table(), table()])\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "sort(): still raises cleanly for a mixed-tag array (no single shared tag)",
          "try\nsort([type_int(), table()])\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },
        { "dunder arithmetic and comparison: every operator that can be taken over, with the typed pointer on the left or on the right",
          "fn __types_desc_sub(a, b)\n"
          "    return \"sub\"\n"
          "end\n"
          "fn __types_desc_mul(a, b)\n"
          "    return \"mul\"\n"
          "end\n"
          "fn __types_desc_div(a, b)\n"
          "    return \"div\"\n"
          "end\n"
          "fn __types_desc_mod(a, b)\n"
          "    return \"mod\"\n"
          "end\n"
          "fn __types_desc_lt(a, b)\n"
          "    return \"lt\"\n"
          "end\n"
          "fn __types_desc_gt(a, b)\n"
          "    return \"gt\"\n"
          "end\n"
          "fn __types_desc_lte(a, b)\n"
          "    return \"lte\"\n"
          "end\n"
          "fn __types_desc_gte(a, b)\n"
          "    return \"gte\"\n"
          "end\n"
          "d = type_int()\n"
          "print([d - 1, d * 2, d / 3, d % 4, d < 5, d > 6, d <= 7, d >= 8])\n"
          "print([1 - d, 2 * d, 3 / d, 4 % d, 5 < d, 6 > d, 7 <= d, 8 >= d])\n"
          "fn cmp(a, b)\n"
          "    if a < b\n"
          "        return \"yes\"\n"
          "    end\n"
          "    return \"no\"\n"
          "end\n"
          "print(cmp(d, d))\n",
          "[\"sub\", \"mul\", \"div\", \"mod\", \"lt\", \"gt\", \"lte\", \"gte\"]\n[\"sub\", \"mul\", \"div\", \"mod\", \"lt\", \"gt\", \"lte\", \"gte\"]\nyes\n" },
        { "dunder: a comparison or arithmetic with no dunder defined names the operator and the types",
          "d = type_int()\n"
          "try\n"
          "    print(d < d)\n"
          "catch e\n"
          "    print(e[\"message\"])\n"
          "end\n"
          "try\n"
          "    print(d * 2)\n"
          "catch e\n"
          "    print(e[\"message\"])\n"
          "end\n",
          "unsupported operands for '<': pointer and pointer\nunsupported operands for '*': pointer and int\n" },
        { "operators on values the checker can't see (from json_parse) raise a clear error naming the operator and both types",
          "vals = json_parse(\"[[1], {}, \\\"s\\\"]\")\n"
          "fs = [fn(x) return x - 1 end, fn(x) return x * 2 end, fn(x) return x / 2 end, fn(x) return x % 2 end, fn(x) return x < 1 end, fn(x) return x <= 1 end, fn(x) return x > 1 end, fn(x) return x >= 1 end, fn(x) return -x end]\n"
          "for v in vals\n"
          "    for f in fs\n"
          "        try\n"
          "            print(f(v))\n"
          "        catch e\n"
          "            print(e[\"message\"])\n"
          "        end\n"
          "    end\n"
          "end\n",
          "unsupported operands for \'-\': array and int\nunsupported operands for \'*\': array and int\nunsupported operands for \'/\': array and int\nunsupported operands for \'%\': array and int\nunsupported operands for \'<\': array and int\nunsupported operands for \'<=\': array and int\nunsupported operands for \'>\': array and int\nunsupported operands for \'>=\': array and int\nunsupported operand for unary \'-\': array\nunsupported operands for \'-\': map and int\nunsupported operands for \'*\': map and int\nunsupported operands for \'/\': map and int\nunsupported operands for \'%\': map and int\nunsupported operands for \'<\': map and int\nunsupported operands for \'<=\': map and int\nunsupported operands for \'>\': map and int\nunsupported operands for \'>=\': map and int\nunsupported operand for unary \'-\': map\nunsupported operands for \'-\': string and int\nunsupported operands for \'*\': string and int\nunsupported operands for \'/\': string and int\nunsupported operands for \'%\': string and int\nunsupported operands for \'<\': string and int\nunsupported operands for \'<=\': string and int\nunsupported operands for \'>\': string and int\nunsupported operands for \'>=\': string and int\nunsupported operand for unary \'-\': string\n" },
    };

    const std::vector<std::string> plugins = {"embrtypes", "json", "table", "embrlib", "ffi", "str"};

    int passed = rawOk ? 1 : 0, failed = rawOk ? 0 : 1;
    for (const auto& t : tests) {
        auto interp = embr_test::freshInterpreter(plugins);
        auto r = embr_test::runTest(t, *interp);
        embr_test::printResult(r);
        r.passed ? ++passed : ++failed;
    }

    std::cout << "\n"
              << "  " << passed << " passed, " << failed << " failed"
              << (failed ? "  \033[31mX\033[0m" : "  \033[32m√\033[0m") << "\n\n";
    return failed == 0 ? 0 : 1;
}
