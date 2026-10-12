// unit tests for plugins/table/table.cpp (imported once by main() below, plus embrlib for len()/has())
//
// table_keys/table_values/table_items come back in unspecified order (see table.cpp's header), so tests with more
// than one entry check order-independent things (a sum, a count, has()) instead of an exact sequence

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "table_set/table_get: basic round trip with a string key",
          "t = table()\n"
          "table_set(t, \"name\", \"embr\")\n"
          "print(table_get(t, \"name\"))\n",
          "embr\n" },

        { "table: t[key], t[key] = x and for pair in t work like table_get, table_set and table_items",
          "t = table()\nt[\"a\"] = 1\nt[2] = \"two\"\nprint(t[\"a\"])\nprint(t[2])\nt[\"a\"] = t[\"a\"] + 10\nprint(t[\"a\"])\n"
          "n = 0\nfor pair in t\n  n = n + 1\nend\nprint(n)\n"
          "try\n  print(t[\"missing\"])\ncatch e\n  print(\"no key\")\nend\n",
          "1\ntwo\n11\n2\nno key\n" },

        { "table: number keys normalize int and float to the same slot",
          "t = table()\n"
          "table_set(t, 5, \"int five\")\n"
          "print(table_get(t, 5.0))\n"
          "table_set(t, 5.0, \"float five\")\n"
          "print(table_get(t, 5))\n"
          "print(table_len(t))\n",
          "int five\nfloat five\n1\n" },

        { "table: a string key and a number key that stringify the same are distinct slots",
          "t = table()\n"
          "table_set(t, \"5\", \"as string\")\n"
          "table_set(t, 5, \"as number\")\n"
          "print(table_len(t))\n"
          "print(table_get(t, \"5\"))\n"
          "print(table_get(t, 5))\n",
          "2\nas string\nas number\n" },

        { "table_has: reflects presence and absence",
          "t = table()\n"
          "print(table_has(t, \"x\"))\n"
          "table_set(t, \"x\", 1)\n"
          "print(table_has(t, \"x\"))\n",
          "0\n1\n" },

        { "table_delete: removes a key; deleting again returns 0",
          "t = table()\n"
          "table_set(t, \"x\", 1)\n"
          "print(table_delete(t, \"x\"))\n"
          "print(table_has(t, \"x\"))\n"
          "print(table_delete(t, \"x\"))\n",
          "1\n0\n0\n" },

        { "table_get: raises a catchable 'key not found' instead of returning a sentinel",
          "t = table()\n"
          "try\ntable_get(t, \"missing\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "table_len/table_clear: track size across mutations",
          "t = table()\n"
          "table_set(t, 1, \"a\")\n"
          "table_set(t, 2, \"b\")\n"
          "print(table_len(t))\n"
          "table_clear(t)\n"
          "print(table_len(t))\n"
          "print(table_has(t, 1))\n",
          "2\n0\n0\n" },

        { "table: NaN key raises",
          "t = table()\n"
          "try\ntable_set(t, 0.0 / 0.0, 1)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "caught\n" },

        { "table: an array or map key raises instead of silently misbehaving",
          "t = table()\n"
          "try\ntable_set(t, [1,2,3], \"x\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught 1\")\nend\n"
          "try\ntable_set(t, {\"a\": 1}, \"x\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught 2\")\nend\n",
          "caught 1\ncaught 2\n" },

        { "table: a table used as a key compares by identity, not structurally",
          "t = table()\n"
          "inner = table()\n"
          "table_set(t, inner, \"nested\")\n"
          "print(table_get(t, inner))\n"
          "print(table_has(t, table()))\n",
          "nested\n0\n" },

        { "table: a function used as a key compares by identity",
          "t = table()\n"
          "f = fn(x) return x end\n"
          "g = fn(x) return x end\n"
          "table_set(t, f, \"is f\")\n"
          "print(table_get(t, f))\n"
          "print(table_has(t, g))\n",
          "is f\n0\n" },

        { "table_keys/table_values/table_items: order-independent contents",
          "t = table()\n"
          "table_set(t, 1, 10)\n"
          "table_set(t, 2, 20)\n"
          "table_set(t, 3, 30)\n"
          "ksum = 0\n"
          "for k in table_keys(t)\n"
          "  ksum = ksum + k\n"
          "end\n"
          "vsum = 0\n"
          "for v in table_values(t)\n"
          "  vsum = vsum + v\n"
          "end\n"
          "isum = 0\n"
          "for pair in table_items(t)\n"
          "  isum = isum + pair[0] + pair[1]\n"
          "end\n"
          "print(ksum)\n"
          "print(vsum)\n"
          "print(isum)\n"
          "print(len(table_items(t)))\n",
          "6\n60\n66\n3\n" },
    };
    return embr_test::runSuite("table plugin test suite", tests, {"table", "embrlib"});
}
