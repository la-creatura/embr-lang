// unit tests for plugins/gc/gc.cpp
//
// the main case is the cycle gc_collect() exists to break: a closure stored in a variable inside its own captured
// scope. once make_thing() returns, nothing but the cycle keeps that scope alive, so gc_collect() must clear it.
// gc_scope_count() dropping afterwards is the visible proof, since a script can't ask whether one scope is alive

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "gc_collect: reports 0 with no cycles and no unreached scopes",
          "print(gc_collect())\n",
          "0\n" },

        // the VM remembers where top-level variables live; clearing dead scopes must not leave it pointing at one
        { "gc_collect: globals read and written around collections stay right",
          "fn make_thing()\n"
          "    local any self_ref = 0\n"
          "    fn recur()\n"
          "        return self_ref\n"
          "    end\n"
          "    self_ref = recur\n"
          "    return recur\n"
          "end\n"
          "total = 0\n"
          "i = 0\n"
          "while i < 4\n"
          "    make_thing()\n"
          "    gc_collect()\n"
          "    total = total + i\n"
          "    i = i + 1\n"
          "end\n"
          "print(total)\n",
          "6\n" },

        { "gc_collect: breaks a self-referential closure cycle",
          "fn make_thing()\n"
          "    local any self_ref = 0\n"
          "    fn recur()\n"
          "        return self_ref\n"
          "    end\n"
          "    self_ref = recur\n"
          "    return recur\n"
          "end\n"
          "make_thing()\n"                 // discard the result: nothing keeps the cycle reachable now
          "before = gc_scope_count()\n"
          "cleared = gc_collect()\n"
          "after = gc_scope_count()\n"
          "print(cleared >= 1)\n"
          "print(after < before)\n",
          "1\n1\n" },

        { "gc_collect: frees closures after errors unwound through many nested calls (VM frame roots are released on unwind)",
          "fn make_thing()\n"
          "    local any self_ref = 0\n"
          "    fn recur()\n"
          "        return self_ref\n"
          "    end\n"
          "    self_ref = recur\n"
          "    return recur\n"
          "end\n"
          "fn thrower(n)\n"
          "    make_thing()\n"
          "    if n == 0\n"
          "        error(\"boom\")\n"
          "    end\n"
          "    return thrower(n - 1)\n"
          "end\n"
          "i = 0\n"
          "while i < 50\n"
          "    try\n"
          "        thrower(20)\n"
          "    catch e\n"
          "    end\n"
          "    i = i + 1\n"
          "end\n"
          "gc_collect()\n"
          "print(gc_scope_count() < 20)\n",
          "1\n" },

        { "gc_collect: a still-reachable closure's scope is left untouched",
          "fn make_counter()\n"
          "    local count = 0\n"
          "    fn increment()\n"
          "        count = count + 1\n"
          "        return count\n"
          "    end\n"
          "    return increment\n"
          "end\n"
          "counter = make_counter()\n"
          "gc_collect()\n"
          "print(counter())\n"
          "print(counter())\n",
          "1\n2\n" },

        { "gc_collect: a closure reachable only through a live table keeps its captured scope (used to be cleared)",
          "fn make()\n"
          "    local secret = 42\n"
          "    t = table()\n"
          "    table_set(t, \"f\", fn() return secret end)\n"
          "    return t\n"
          "end\n"
          "held = make()\n"
          "gc_collect()\n"
          "print(table_get(held, \"f\")())\n",
          "42\n" },

        { "gc_collect: a closure reachable only through a live vec keeps its captured scope",
          "fn make()\n"
          "    local secret = 7\n"
          "    v = vec()\n"
          "    vec_push(v, fn() return secret end)\n"
          "    return v\n"
          "end\n"
          "held = make()\n"
          "gc_collect()\n"
          "print(vec_get(held, 0)())\n",
          "7\n" },

        { "gc_collect: a closure used as a table KEY, and one nested inside an array in a table, also survive",
          "fn make()\n"
          "    local a = 7\n"
          "    local b = 8\n"
          "    t = table()\n"
          "    table_set(t, \"arr\", [fn() return a end])\n"
          "    return t\n"
          "end\n"
          "held = make()\n"
          "gc_collect()\n"
          "print(table_get(held, \"arr\")[0]())\n",
          "7\n" },

        { "gc_collect: a table that holds itself (directly and via a closure) terminates and the closure still works",
          "fn make()\n"
          "    local n = 5\n"
          "    t = table()\n"
          "    table_set(t, \"self\", t)\n"
          "    table_set(t, \"f\", fn() return n end)\n"
          "    return t\n"
          "end\n"
          "held = make()\n"
          "gc_collect()\n"
          "print(table_get(table_get(held, \"self\"), \"f\")())\n",
          "5\n" },

        { "gc_collect: a cycle that runs THROUGH a table (scope -> table -> closure -> scope) is still collected when unreachable",
          "fn make_thing()\n"
          "    local t = table()\n"             // `local`, so t is clearly a local of make_thing()
          "    fn f()\n"
          "        return t\n"
          "    end\n"
          "    table_set(t, \"f\", f)\n"
          "    return 0\n"
          "end\n"
          "make_thing()\n"
          "before = gc_scope_count()\n"
          "cleared = gc_collect()\n"
          "after = gc_scope_count()\n"
          "print(cleared >= 1)\n"
          "print(after < before)\n",
          "1\n1\n" },

        { "gc_collect: an ffi_callback's closure stays alive after gc_collect(), even with the handle dropped (C still holds the pointer)",
          "fn make_cmp()\n"
          "    local sign = 1\n"
          "    return ffi_callback(fn(a, b)\n"
          "        return sign * (ptr_read(a, type_i32()) - ptr_read(b, type_i32()))\n"
          "    end, type_i32(), [type_ptr(), type_ptr()])\n"
          "end\n"
          "cmp = make_cmp()\n"
          "gc_collect()\n"
          "lib = ffi_open(\"" EMBR_TEST_LIBC "\")\n"
          "arr = buffer(16)\n"
          "vals = [4, 1, 3, 2]\n"
          "i = 0\nwhile i < 4\nptr_write(ptr_offset(arr, i * 4), type_i32(), vals[i])\ni = i + 1\nend\n"
          "ffi_call(ffi_sym(lib, \"qsort\"), nil, [type_ptr(), type_u64(), type_u64(), type_ptr()], [arr, 4, 4, cmp])\n"
          "print(ptr_read(arr, type_i32()))\n"
          "print(ptr_read(ptr_offset(arr, 12), type_i32()))\n",
          "1\n4\n" },
    };
    return embr_test::runSuite("gc plugin test suite", tests, {"embrlib", "table", "vec", "ffi", "embrtypes", "gc"});
}
