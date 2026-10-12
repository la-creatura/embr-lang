// unit tests for plugins/embrmeta/embrmeta.cpp (imported once by main() below,
// alongside embrlib for write_file()/import, used only to build a throwaway
// module fixture for the shadowing test, not itself under test here)

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "meta_set: defines a new global variable, readable as an ordinary identifier",
          "meta_set(\"mm_x\", 42)\nprint(mm_x)\n",
          "42\n" },

        { "meta_set: overwrites an existing global",
          "meta_set(\"mm_y\", 1)\nmeta_set(\"mm_y\", 2)\nprint(mm_y)\n",
          "2\n" },

        { "meta_set: installs a callable that behaves like any other function",
          "meta_set(\"mm_add\", fn(a, b) return a + b end)\nprint(mm_add(3, 4))\n",
          "7\n" },

        { "meta_has: reflects presence and absence",
          "print(meta_has(\"mm_never_defined\"))\n"
          "meta_set(\"mm_present\", 1)\n"
          "print(meta_has(\"mm_present\"))\n",
          "0\n1\n" },

        { "meta_undef: removes a global; meta_has and later use both reflect it's gone",
          "meta_set(\"mm_temp\", 5)\n"
          "print(meta_has(\"mm_temp\"))\n"
          "print(meta_undef(\"mm_temp\"))\n"
          "print(meta_has(\"mm_temp\"))\n"
          "try\nprint(mm_temp)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "1\n1\n0\ncaught\n" },

        { "meta_overload: numbers the members itself, returns the name, and the bare name picks by type",
          "print(meta_overload(\"mo_area\", fn(r: int) return r * r end))\n"
          "print(meta_overload(\"mo_area\", fn(w: str) return len(w) end))\n"
          "print(mo_area(5))\nprint(mo_area(\"abc\"))\nprint(meta_overloads(\"mo_area\"))\nprint(mo_area__1(3))\n",
          "mo_area__1\nmo_area__2\n25\n3\n[\"mo_area__1\", \"mo_area__2\"]\n9\n" },

        { "meta_overload: a number is never reused, even after its function was removed",
          "meta_overload(\"mo_n\", fn(a: int) return 1 end)\nmeta_overload(\"mo_n\", fn(a: str) return 2 end)\n"
          "print(meta_undef(\"mo_n__2\"))\nprint(meta_overload(\"mo_n\", fn(a: str) return 3 end))\nprint(mo_n(\"x\"))\n",
          "1\nmo_n__3\n3\n" },

        { "meta_overload: refuses a plain name, an empty name and a name that is already numbered",
          "meta_set(\"mo_plain\", fn(x) return x end)\n"
          "try\n  meta_overload(\"mo_plain\", fn(x: int) return x end)\ncatch e\n  print(\"plain refused\")\nend\n"
          "try\n  meta_overload(\"\", fn(x: int) return x end)\ncatch e\n  print(\"empty refused\")\nend\n"
          "try\n  meta_overload(\"mo_z__2\", fn(x: int) return x end)\ncatch e\n  print(\"numbered refused\")\nend\n",
          "plain refused\nempty refused\nnumbered refused\n" },

        { "meta_overload: a call no member takes lists the members",
          "meta_overload(\"mo_e\", fn(a: int) return 1 end)\nmeta_overload(\"mo_e\", fn(a: str) return 2 end)\n"
          "try\n  mo_e([1])\ncatch e\n  print(e[\"message\"])\nend\n",
          "no overload of 'mo_e' takes (array); candidates: mo_e__1(int), mo_e__2(string)\n" },

        { "meta_undef: removing something already gone returns 0, doesn't raise",
          "print(meta_undef(\"mm_was_never_there\"))\n",
          "0\n" },

        { "meta_get: reads a global by name, raises on an undefined one",
          "meta_set(\"mm_g\", \"hello\")\n"
          "print(meta_get(\"mm_g\"))\n"
          "try\nmeta_get(\"mm_totally_missing\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "hello\ncaught\n" },

        // meta_get/meta_has read the native/global layer, the one bind() and a top-level assignment fall back to, not
        // whatever shadows the name in the caller's scope. the module is written with write_file(), so no fixture file is needed
        { "meta_get: sees the global binding even when a module shadows the same name locally",
          "meta_set(\"mm_shadow_test\", 111)\n"
          "write_file(\"test_embrmeta_shadow.embr\","
          " \"local mm_shadow_test = 222\\nmm_shadow_result = meta_get(\\\"mm_shadow_test\\\")\\n\")\n"
          "import \"test_embrmeta_shadow.embr\"\n"
          "print(mm_shadow_result)\n",
          "111\n" },

        { "meta_plugins: one summary per imported plugin, in import order",
          "ps = meta_plugins()\nprint(len(ps))\nprint(ps[0][\"name\"])\nprint(ps[1][\"name\"])\nprint(ps[0][\"kind\"])\n"
          "print(ps[0][\"capabilities\"])\nprint(has(ps[0], \"functions\"))\n",
          "2\nembrmeta\nembrlib\ncxx\n[]\n0\n" },

        { "meta_plugin: name, version, description, kind, interface number and capabilities",
          "p = meta_plugin(\"embrlib\")\nprint(p[\"name\"])\nprint(p[\"version\"])\nprint(p[\"kind\"])\nprint(p[\"abi\"])\n"
          "print(p[\"capabilities\"])\nprint(p[\"optional_capabilities\"])\nprint(p[\"declared\"])\n"
          "print(len(p[\"description\"]) > 10)\nprint(len(p[\"build_id\"]) > 10)\n",
          "embrlib\n1.0.0\ncxx\n1\n[]\n[\"filesystem\"]\n1\n1\n1\n" },

        { "meta_plugin: functions have a signature read from the bound function and a doc line",
          "fns = meta_plugin(\"embrmeta\")[\"functions\"]\nby = {}\nfor f in fns\n  by[f[\"name\"]] = f\nend\n"
          "print(by[\"meta_has\"][\"signature\"])\nprint(by[\"meta_has\"][\"doc\"])\nprint(by[\"meta_plugin\"][\"signature\"])\n"
          "print(len(fns))\n",
          "fn meta_has(name: str) -> int\n1 if a global with this name exists, else 0\nfn meta_plugin(name: str) -> map\n11\n" },

        { "meta_plugin: every function the plugin binds has a doc line",
          "undocumented = []\nfor f in meta_plugin(\"embrlib\")[\"functions\"]\n  if f[\"doc\"] == \"\"\n    undocumented = push(undocumented, f[\"name\"])\n  end\nend\nprint(undocumented)\n",
          "[]\n" },

        { "meta_plugin: raises for a plugin that was not imported",
          "try\n  meta_plugin(\"nope_not_here\")\n  print(\"unreachable\")\ncatch e\n  print(e[\"message\"])\nend\n",
          "plugin 'nope_not_here' has not been imported (import \"nope_not_here\" first)\n" },

        { "meta_modules / meta_module: a module's declaration, its capabilities and the doc line of its functions, read from the file",
          "write_file(\"t_meta_mod.embr\", \"__module__ = {\\\"version\\\": \\\"3.0.0\\\", \\\"description\\\": \\\"a test module\\\", \\\"capabilities\\\": [\\\"filesystem\\\"]}\\n"
          "import \\\"fs\\\"\\n\\n# adds one.\\n# to the number.\\nfn add_one(n)\\n    return n + 1\\nend\\n\\nfn undocumented()\\n    return 0\\nend\\n\")\n"
          "import \"t_meta_mod.embr\"\n"
          "ms = meta_modules()\nprint(len(ms))\nprint(ms[0][\"name\"], ms[0][\"version\"], ms[0][\"capabilities\"], ms[0][\"declared\"])\n"
          "m = meta_module(\"t_meta_mod\")\nprint(m[\"description\"])\nprint(m[\"declared_capabilities\"], m[\"inferred_capabilities\"])\n"
          "print(m[\"imports\"][0][\"path\"], m[\"imports\"][0][\"kind\"], m[\"imports\"][0][\"capabilities\"])\n"
          "print(m[\"functions\"][0][\"signature\"], \"|\", m[\"functions\"][0][\"doc\"])\n"
          "print(m[\"functions\"][1][\"name\"], \"|\", m[\"functions\"][1][\"doc\"])\n"
          "print(add_one(1))\nprint(has(globals(), \"__module__\"))\n",
          "1\nt_meta_mod 3.0.0 [\"filesystem\"] 1\na test module\n[\"filesystem\"] [\"filesystem\"]\nfs plugin [\"filesystem\"]\n"
          "fn add_one(n) | adds one. to the number.\nundocumented | \n2\n0\n" },

        { "meta_module_file: capabilities are inferred from the imports when the module declares nothing, and a computed import is reported",
          "write_file(\"t_meta_file1.embr\", \"import \\\"os\\\"\\nimport \\\"str\\\"\\nfn f() return 1 end\\n\")\n"
          "write_file(\"t_meta_file2.embr\", \"which = \\\"str\\\"\\nimport which\\nfn g() return 1 end\\n\")\n"
          "a = meta_module_file(\"t_meta_file1.embr\")\nprint(a[\"declared\"], a[\"capabilities\"], a[\"dynamic_imports\"])\n"
          "b = meta_module_file(\"t_meta_file2.embr\")\nprint(b[\"declared\"], b[\"capabilities\"], b[\"dynamic_imports\"])\n",
          "0 [\"process\", \"env\"] 0\n0 [] 1\n" },

        { "meta_module_file: a declaration that leaves out something the module imports is a problem, a bad declaration too, and nothing is run",
          "write_file(\"t_meta_file3.embr\", \"__module__ = {\\\"capabilities\\\": [\\\"network\\\"]}\\nimport \\\"fs\\\"\\nerror(\\\"this must not run\\\")\\n\")\n"
          "write_file(\"t_meta_file4.embr\", \"__module__ = {\\\"version\\\": 1 + 1, \\\"bogus\\\": \\\"x\\\"}\\n\")\n"
          "p = meta_module_file(\"t_meta_file3.embr\")[\"problems\"]\nprint(len(p))\nprint(p[0])\n"
          "q = meta_module_file(\"t_meta_file4.embr\")[\"problems\"]\nprint(len(q))\nprint(q[0])\n",
          "1\ndeclares network but imports \"fs\", which needs filesystem. add it to \"capabilities\" in __module__\n2\n\"version\" in __module__ must be a string written out in the file\n" },

        { "meta_module / meta_module_file: raise for a module that was not imported or a file that is not there",
          "try\n  meta_module(\"nope_mod\")\ncatch e\n  print(e[\"message\"])\nend\n"
          "try\n  meta_module_file(\"nope_mod_file.embr\")\ncatch e\n  print(e[\"message\"])\nend\n",
          "module 'nope_mod' has not been imported (import \"nope_mod.embr\" first, or read the file with meta_module_file)\ncannot read 'nope_mod_file.embr'\n" },
    };
    return embr_test::runSuite("embrmeta plugin test suite", tests, {"embrmeta", "embrlib"});
}
