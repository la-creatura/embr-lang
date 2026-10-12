// unit tests for plugins/vec/vec.cpp

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "vec: push, get, set, len",
          "v = vec()\n"
          "vec_push(v, 10)\nvec_push(v, 20)\nvec_push(v, 30)\n"
          "vec_set(v, 1, 99)\n"
          "print(vec_len(v), vec_get(v, 0), vec_get(v, 1), vec_get(v, 2))\n",
          "3 10 99 30\n" },

        { "vec: a copy of the handle is the same vec, vec_copy is a separate one",
          "a = vec()\nvec_push(a, 1)\n"
          "b = a\nvec_push(b, 2)\n"
          "c = vec_copy(a)\nvec_push(c, 3)\n"
          "print(vec_len(a), vec_len(b), vec_len(c))\n",
          "2 2 3\n" },

        { "vec: a function can change the caller's vec",
          "fn add_one(v) vec_push(v, 1) end\n"
          "v = vec()\nadd_one(v)\nadd_one(v)\n"
          "print(vec_len(v))\n",
          "2\n" },

        { "vec_from and vec_to_array round trip, and the array is a copy",
          "arr = [1, 2, 3]\n"
          "v = vec_from(arr)\nvec_push(v, 4)\n"
          "print(arr)\nprint(vec_to_array(v))\n",
          "[1, 2, 3]\n[1, 2, 3, 4]\n" },

        { "vec_pop, vec_insert, vec_remove",
          "v = vec_from([1, 2, 3])\n"
          "print(vec_pop(v))\n"
          "vec_insert(v, 0, 9)\nvec_insert(v, 3, 7)\n"
          "print(vec_to_array(v))\n"
          "print(vec_remove(v, 1))\n"
          "print(vec_to_array(v))\n",
          "3\n[9, 1, 2, 7]\n1\n[9, 2, 7]\n" },

        { "vec_swap, vec_reverse, vec_clear",
          "v = vec_from([1, 2, 3, 4])\n"
          "vec_swap(v, 0, 3)\nprint(vec_to_array(v))\n"
          "vec_reverse(v)\nprint(vec_to_array(v))\n"
          "vec_clear(v)\nprint(vec_len(v))\n",
          "[4, 2, 3, 1]\n[1, 3, 2, 4]\n0\n" },

        { "vec_extend: array, other vec, and itself",
          "v = vec_from([1])\n"
          "vec_extend(v, [2, 3])\n"
          "vec_extend(v, vec_from([4]))\n"
          "vec_extend(v, v)\n"
          "print(vec_to_array(v))\n",
          "[1, 2, 3, 4, 1, 2, 3, 4]\n" },

        { "vec_slice clamps both ends",
          "v = vec_from([1, 2, 3, 4, 5])\n"
          "print(vec_slice(v, 1, 3))\nprint(vec_slice(v, 3))\nprint(vec_slice(v, -5, 99))\nprint(vec_slice(v, 4, 2))\n",
          "[2, 3]\n[4, 5]\n[1, 2, 3, 4, 5]\n[]\n" },

        { "vec holds any value: arrays, maps, functions, other vecs",
          "inner = vec_from([1])\n"
          "v = vec_from([[1, 2], {\"a\": 1}, inner])\n"
          "fn f(x) return x + 1 end\n"
          "vec_push(v, f)\n"
          "vec_push(inner, 2)\n"
          "print(vec_get(v, 0), vec_get(v, 1)[\"a\"], vec_len(vec_get(v, 2)), vec_get(v, 3)(1))\n",
          "[1, 2] 1 2 2\n" },

        { "is_vec",
          "print(is_vec(vec()), is_vec([1]), is_vec(5))\n",
          "1 0 0\n" },

        { "vec: errors are catchable (bounds, empty pop, wrong type)",
          "fn lh(v) return v end v = vec_from([1])\n"
          "try vec_get(v, 1) catch e print(\"get\") end\n"
          "try vec_get(v, -1) catch e print(\"neg\") end\n"
          "try vec_set(v, 5, 0) catch e print(\"set\") end\n"
          "try vec_insert(v, 3, 0) catch e print(\"insert\") end\n"
          "try vec_remove(v, 1) catch e print(\"remove\") end\n"
          "vec_pop(v)\n"
          "try vec_pop(v) catch e print(\"pop\") end\n"
          "try vec_len(lh([1])) catch e print(\"type\") end\n"
          "try vec_len(table()) catch e print(\"tag\") end\n",
          "get\nneg\nset\ninsert\nremove\npop\ntype\ntag\n" },

        { "vec: a vec holding itself does not crash or leak",
          "v = vec()\nvec_push(v, v)\nprint(vec_len(vec_get(v, 0)))\n",
          "1\n" },


        // --- other plugins accept a vec ---

        { "embrlib: len and has work on a vec through the dunder convention",
          "v = vec_from([5, 6, 7])\n"
          "print(len(v), has(v, 2), has(v, 3), has(v, -1))\n",
          "3 1 0 0\n" },

        { "embrlib: push and pop change a vec in place and return it; arrays are still values",
          "v = vec()\n"
          "v = push(v, 1)\nv = push(v, 2)\npush(v, 3)\n"
          "print(vec_to_array(v))\n"
          "pop(v)\n"
          "print(vec_to_array(v))\n"
          "a = [1]\nb = push(a, 2)\nprint(a, b)\n",
          "[1, 2, 3]\n[1, 2]\n[1] [1, 2]\n" },

        { "embrlib: vec_assign + sort is the in-place sort",
          "v = vec_from([3, 1, 2])\nvec_assign(v, sort(vec_to_array(v)))\nprint(vec_to_array(v))\n",
          "[1, 2, 3]\n" },

        { "a pointer that is not a vec still gets a clean error from array functions",
          "fn lh(v) return v end t = lh(table())\n"
          "try sort(t) catch e print(\"sort\") end\n"
          "try push(t, 1) catch e print(\"push\") end\n"
          "try join(t, \",\") catch e print(\"join\") end\n",
          "sort\npush\njoin\n" },

        { "json_stringify writes a vec as an array, also nested",
          "v = vec_from([1, \"a\"])\n"
          "print(json_stringify(v))\n"
          "print(json_stringify({\"k\": [v, vec()]}))\n",
          "[1,\"a\"]\n{\"k\":[[1,\"a\"],[]]}\n" },

        { "json_stringify of a vec that holds itself raises instead of crashing",
          "v = vec()\nvec_push(v, v)\n"
          "try json_stringify(v) catch e print(\"too deep\") end\n",
          "too deep\n" },

        { "csv_stringify takes a vec of rows and vec rows",
          "rows = vec()\n"
          "vec_push(rows, vec_from([\"a\", \"b\"]))\n"
          "vec_push(rows, [1, 2])\n"
          "print(csv_stringify(rows))\n",
          "a,b\n1,2\n\n" },

        { "random: rand_choice reads a vec in place",
          "v = vec_from([7])\n"
          "print(rand_choice(v), vec_to_array(v))\n",
          "7 [7]\n" },

        { "embrlib: other array functions do not take a vec, vec_to_array says what you mean",
          "fn lh(v) return v end v = vec_from([3, 1, 2])\n"
          "w = lh(v)\n"
          "try sort(w) catch e print(\"sort\") end\n"
          "try join(w, \",\") catch e print(\"join\") end\n"
          "print(sort(vec_to_array(v)), sum(vec_to_array(v)))\n",
          "sort\njoin\n[1, 2, 3] 6\n" },

        { "indexing: v[i], v[i] = x and for x in v read like an array",
          "v = vec_from([10, 20, 30])\nprint(v[1])\nv[1] = 99\nprint(v[1])\nv[0] = v[0] + 1\nfor x in v\n  print(x)\nend\n"
          "fn bump(p)\n  p[2] = p[2] * 2\n  return p[2]\nend\nprint(bump(v))\nprint(len(v))\n",
          "20\n99\n11\n99\n30\n60\n3\n" },

        { "indexing: an index out of range or not a number is an error you can catch",
          "v = vec_from([1])\ntry\n  print(v[5])\ncatch e\n  print(\"out of range\")\nend\n"
          "try\n  v[\"a\"] = 1\ncatch e\n  print(\"not a number\")\nend\n",
          "out of range\nnot a number\n" },

        { "vec_each_do: gets the element and its index, in order",
          "v = vec_from([\"a\", \"b\", \"c\"])\n"
          "vec_each_do(v, fn(x, i) print(i, x) end)\n",
          "0 a\n1 b\n2 c\n" },

        { "vec_each_do: a truthy result stops the walk and is returned; otherwise 0",
          "v = vec_from([5, 8, 11, 14])\n"
          "print(vec_each_do(v, fn(x, i) if x > 9 return i end return 0 end))\n"
          "print(vec_each_do(v, fn(x, i) return 0 end))\n",
          "2\n0\n" },

        { "vec_each_do: pushing while walking visits the new elements, removing never crashes",
          "v = vec_from([1, 2])\n"
          "n = 0\n"
          "vec_each_do(v, fn(x, i) if vec_len(v) < 5 vec_push(v, x + 10) end n = n + 1 end)\n"
          "print(n, vec_to_array(v))\n"
          "w = vec_from([1, 2, 3, 4])\n"
          "vec_each_do(w, fn(x, i) vec_clear(w) end)\n"
          "print(vec_len(w))\n",
          "5 [1, 2, 11, 12, 21]\n0\n" },

        { "vec_map_do: replaces each element in place, also when fn shrinks the vec",
          "v = vec_from([1, 2, 3])\n"
          "vec_map_do(v, fn(x, i) return x * 10 + i end)\n"
          "print(vec_to_array(v))\n"
          "w = vec_from([1, 2, 3])\n"
          "vec_map_do(w, fn(x, i) vec_clear(w) return 9 end)\n"
          "print(vec_len(w))\n",
          "[10, 21, 32]\n0\n" },

        { "vec_each_do / vec_map_do: an error in fn propagates and leaves the vec usable",
          "fn lh(v) return v end v = vec_from([1, 2])\n"
          "try vec_each_do(v, fn(x, i) error(\"boom\") end) catch e print(\"caught\") end\n"
          "vec_push(v, 3)\nprint(vec_len(v))\n"
          "try vec_each_do(lh(5), fn(x, i) return 0 end) catch e print(\"type\") end\n",
          "caught\n3\ntype\n" },

        { "vec: a big vec builds in linear time",
          "v = vec()\ni = 0\nwhile i < 200000\n  vec_push(v, i)\n  i = i + 1\nend\n"
          "print(vec_len(v), vec_get(v, 199999))\n",
          "200000 199999\n" },
    };
    return embr_test::runSuite("vec plugin test suite", tests, {"vec", "embrlib", "table", "json", "csv", "random"});
}
