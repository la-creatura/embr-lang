// core language + embrlib test suite

#include "harness.h"

int main() {
    using embr_test::Test;
    std::vector<Test> tests = {

        { "arithmetic: basic ops",
          "print(1+2)\nprint(10-3)\nprint(3*4)\nprint(10/4)\n",
          "3\n7\n12\n2.5\n" },

        { "arithmetic: integer display",
          "print(9.0)\nprint(1.0+2.0)\n",
          "9\n3\n" },



        // top-level variables are looked up once and remembered by the VM (see CompiledChunk::GlobalCache), so
        // anything that changes the scopes in between has to be noticed
        { "globals: lookups stay right when names, types and scopes change",
          "fn lh(v) return v end import \"embrlib\"\n"
          "gl_x = 1\n"
          "fn gl_getx()\n"
          "    return gl_x\n"
          "end\n"
          "gl_i = 0\n"
          "while gl_i < 3\n"
          "    gl_x = gl_x + 1\n"
          "    gl_i = gl_i + 1\n"
          "end\n"
          "print(gl_x, gl_getx())\n"
          "gl_j = 0\n"
          "while gl_j < 3\n"
          "    if gl_j == 0\n"
          "        gl_fresh = 10\n"
          "    end\n"
          "    gl_fresh = gl_fresh + 1\n"
          "    gl_j = gl_j + 1\n"
          "end\n"
          "print(gl_fresh)\n"
          "local int gl_n = 1\n"
          "gl_k = 0\n"
          "while gl_k < 2\n"
          "    gl_n = gl_n + 1\n"
          "    gl_k = gl_k + 1\n"
          "end\n"
          "print(gl_n)\n"
          "try\n"
          "    gl_n = lh(\"oops\")\n"
          "catch e\n"
          "    print(\"type error caught\")\n"
          "end\n"
          "gl_n = gl_n + 1\n"
          "print(gl_n)\n"
          "fn gl_bump()\n"
          "    gl_x = gl_x + 100\n"
          "end\n"
          "gl_bump()\n"
          "print(gl_x)\n"
          "gl_z = 5\n"
          "gl_w = 0\n"
          "while gl_w < 2\n"
          "    local gl_z = 99\n"
          "    print(gl_z)\n"
          "    gl_w = gl_w + 1\n"
          "end\n"
          "print(gl_z)\n"
          "gl_q = [1, 2, 3]\n"
          "gl_r = 0\n"
          "while gl_r < 3\n"
          "    gl_q = push(gl_q, gl_r)\n"
          "    gl_r = gl_r + 1\n"
          "end\n"
          "print(len(gl_q))\n",
          "4 4\n13\n3\ntype error caught\n4\n104\n99\n99\n5\n6\n" },

        // `name[key]` reads the variable in place on the VM instead of copying it first
        { "index: name[key] on globals, locals, captured variables, nested containers",
          "ix_g = [10, 20, 30]\n"
          "ix_m = {\"a\": 1, \"b\": 2}\n"
          "ix_s = \"hey\"\n"
          "ix_k = 1\n"
          "print(ix_g[0], ix_g[ix_k], ix_g[ix_k + 1], ix_g[2 - ix_k], ix_m[\"a\"], ix_m[\"b\"], ix_s[ix_k])\n"
          "ix_nest = [[1, 2], [3, 4]]\n"
          "print(ix_nest[1][0], ix_nest[ix_k][ix_k])\n"
          "fn ix_f(arr, i)\n"
          "    loc = [7, 8, 9]\n"
          "    return arr[i] + loc[i]\n"
          "end\n"
          "print(ix_f(ix_g, 2))\n"
          "fn ix_mk()\n"
          "    cap = [5, 6, 7]\n"
          "    return fn(i) return cap[i] end\n"
          "end\n"
          "ix_h = ix_mk()\n"
          "print(ix_h(1), ix_h(2))\n"
          "ix_i = 0\n"
          "while ix_i < 3\n"
          "    print(ix_g[ix_i])\n"
          "    ix_i += 1\n"
          "end\n"
          "try\n"
          "    print(ix_gg[0])\n"
          "catch e\n"
          "    print(\"undefined\")\n"
          "end\n"
          "ix_g = [1, 2]\n"
          "print(ix_g[1])\n",
          "10 20 30 20 1 2 e\n3 4\n39\n6 7\n10\n20\n30\nundefined\n2\n" },

        { "string: concat",
          "print(\"hello \" + \"world\")\n",
          "hello world\n" },

        // `x = x + simple` is compiled to an in-place append on the VM, these must behave like plain `+` on both
        { "string: s = s + x appends (global, local, itself, number, array)",
          "s = \"a\"\n"
          "s = s + \"b\"\n"
          "n = 5\n"
          "s = s + n\n"
          "s = s + s\n"
          "print(s)\n"
          "fn f()\n"
          "    t = \"x\"\n"
          "    t = t + \"y\"\n"
          "    t = t + [1, 2]\n"
          "    return t\n"
          "end\n"
          "print(f())\n"
          "a = 1.0\n"
          "a = a + 2\n"
          "a = a + 0.5\n"
          "print(a)\n"
          "b = 2\n"
          "bz = b + \"z\"\n"
          "print(bz)\n",
          "ab5ab5\nxy[1, 2]\n3.5\n2z\n" },

        { "string: s = s + x copies are independent",
          "s = \"ab\"\n"
          "t = s\n"
          "s = s + \"c\"\n"
          "print(t)\nprint(s)\n",
          "ab\nabc\n" },

        { "string: num coercion in +",
          "print(\"n=\" + 42)\n",
          "n=42\n" },

        { "string: index",
          "s = \"abc\"\nprint(s[1])\n",
          "b\n" },



        { "variables: basic",
          "x = 10\ny = x + 5\nprint(y)\n",
          "15\n" },

        { "variables: local shadows global",
          "x = 1\nlocal x = 99\nprint(x)\n",
          "99\n" },



        { "fn block: basic",
          "fn double(n)\nreturn n * 2\nend\nprint(double(7))\n",
          "14\n" },

        { "fn block: multiple params",
          "fn add(a, b)\nreturn a + b\nend\nprint(add(3, 4))\n",
          "7\n" },



        { "fn assign: single arg",
          "square(x) = x * x\nprint(square(5))\n",
          "25\n" },

        { "fn assign: zero args",
          "answer() = 42\nprint(answer())\n",
          "42\n" },

        { "fn assign: multi arg",
          "mul(a, b) = a * b\nprint(mul(3, 7))\n",
          "21\n" },

        { "fn assign: string result",
          "greet(n) = \"hello \" + n\nprint(greet(\"world\"))\n",
          "hello world\n" },

        { "fn assign: compose",
          "double(x) = x * 2\nquad(x) = double(double(x))\nprint(quad(3))\n",
          "12\n" },



        { "recursion: factorial",
          "fn fact(n)\nif n <= 1\nreturn 1\nend\nreturn n * fact(n-1)\nend\nprint(fact(5))\n",
          "120\n" },

        { "recursion: fibonacci",
          "fn fib(n)\nif n <= 1\nreturn n\nend\nreturn fib(n-1) + fib(n-2)\nend\nprint(fib(8))\n",
          "21\n" },



        { "if/else: true branch",
          "if 1 > 0\nprint(\"yes\")\nelse\nprint(\"no\")\nend\n",
          "yes\n" },

        { "if/else: false branch",
          "if 0\nprint(\"yes\")\nelse\nprint(\"no\")\nend\n",
          "no\n" },

        { "if: no else",
          "if 1\nprint(\"ok\")\nend\n",
          "ok\n" },

        { "elif: chain picks matching branch",
          "x = 3\nif x == 1\nprint(\"a\")\nelif x == 2\nprint(\"b\")\nelif x == 3\nprint(\"c\")\nelse\nprint(\"d\")\nend\n",
          "c\n" },

        { "elif: falls through to else",
          "x = 9\nif x == 1\nprint(\"a\")\nelif x == 2\nprint(\"b\")\nelse\nprint(\"z\")\nend\n",
          "z\n" },



        { "while: sum 0..4",
          "i = 0\ns = 0\nwhile i < 5\ns = s + i\ni = i + 1\nend\nprint(s)\n",
          "10\n" },

        { "while: break stops the loop early",
          "i = 0\nwhile i < 10\n  if i == 3\n    break\n  end\n  print(i)\n  i = i + 1\nend\n",
          "0\n1\n2\n" },

        { "while: continue skips the rest of the body",
          "i = 0\nwhile i < 5\n  i = i + 1\n  if i == 3\n    continue\n  end\n  print(i)\nend\n",
          "1\n2\n4\n5\n" },

        { "for-in: array",
          "for v in [10, 20, 30]\n  print(v)\nend\n",
          "10\n20\n30\n" },

        { "for-in: string",
          "for c in \"ab\"\n  print(c)\nend\n",
          "a\nb\n" },

        { "for-in: map, two vars gives key then value",
          "for k, v in {\"only\": 42}\n  print(k)\n  print(v)\nend\n",
          "only\n42\n" },

        { "for-in: break exits the loop",
          "for v in [1,2,3,4]\n  if v == 3\n    break\n  end\n  print(v)\nend\n",
          "1\n2\n" },



        { "array: index and assign",
          "a = [10, 20, 30]\nprint(a[1])\na[1] = 99\nprint(a[1])\n",
          "20\n99\n" },

        { "array: multiple index and assign",
          "a = [[10, 20], [20, 30], [30, 40]]\nprint(a[1][1])\na[1][1] = 99\nprint(a[1][1])\n",
          "30\n99\n" },

        { "array: len / push / pop",
          "a = [1,2]\na = push(a, 3)\nprint(len(a))\na = pop(a)\nprint(len(a))\n",
          "3\n2\n" },



        { "map: set and get",
          "m = {x: 10}\nprint(m[\"x\"])\nm[\"y\"] = 20\nprint(m[\"y\"])\n",
          "10\n20\n" },
        { "map: multiple index and assign",
          "m = {x: 10}\nprint(m[\"x\"])\nm[\"y\"] = 20\nprint(m[\"y\"])\n",
          "10\n20\n" },



        { "type conversions",
          "print(num(\"3.14\"))\nprint(str(42))\nprint(type(1))\nprint(type(\"a\"))\n",
          "3.14\n42\nint\nstring\n" },



        { "boolean: not",
          "print(!0)\nprint(!1)\nprint(!\"\")\nprint(!\"hi\")\n",
          "1\n0\n1\n0\n" },



        { "globals: includes vars",
          "x = 42\ng = globals()\nprint(g[\"x\"])\n",
          "42\n" },

        { "globals: includes fn",
          "foo() = 1\ng = globals()\nprint(g[\"foo\"])\n",
          "<fn foo>\n" },



        { "hof: iif true",
          "if_do(1, fn() print(\"y\") end, fn() print(\"n\") end)\n",
          "y\n" },

        { "hof: iif false",
          "if_do(0, fn() print(\"y\") end, fn() print(\"n\") end)\n",
          "n\n" },

        { "hof: iloop",
          "i = 0\nwhile_do(fn() return i < 3 end, fn() print(i)\ni = i + 1\nend)\n",
          "0\n1\n2\n" },

        { "hof: call",
          "double(x) = x * 2\nprint(call(double, [7]))\n",
          "14\n" },



        { "modulo: basic",
          "print(10 % 3)\nprint(7 % 7)\nprint(1 % 5)\n",
          "1\n0\n1\n" },

        { "modulo: fizzbuzz snippet",
          "i = 1\nwhile i <= 6\n"
          "  if i % 3 == 0\n    print(\"fizz\")\n"
          "  else\n    print(i)\n  end\n"
          "  i += 1\nend\n",
          "1\n2\nfizz\n4\n5\nfizz\n" },



        { "compound: +=",
          "x = 5\nx += 3\nprint(x)\n",
          "8\n" },

        { "compound: -=",
          "x = 10\nx -= 4\nprint(x)\n",
          "6\n" },

        { "compound: *=",
          "x = 3\nx *= 7\nprint(x)\n",
          "21\n" },

        { "compound: /=",
          "x = 20.0\nx /= 4\nprint(x)\n",
          "5\n" },

        { "compound: loop counter",
          "i = 0\nwhile i < 4\n  i += 1\nend\nprint(i)\n",
          "4\n" },


        { "and: both true",
          "print(1 and 1)\n",
          "1\n" },

        { "and: left false short-circuits",
          "print(0 and 1)\nprint(0 and 0)\n",
          "0\n0\n" },

        { "or: left true short-circuits",
          "print(1 or 0)\nprint(1 or 1)\n",
          "1\n1\n" },

        { "or: both false",
          "print(0 or 0)\n",
          "0\n" },

        { "and/or precedence: and binds tighter than or",
          "print(0 and 1 or 1)\n",
          "1\n" },

        { "and/or: used in if condition",
          "x = 5\nif x > 3 and x < 10\n  print(\"in range\")\nend\n",
          "in range\n" },



        { "multi-statement source block",
          "fn greet(n)\nprint(\"hello \" + n)\nend\ngreet(\"world\")\n",
          "hello world\n" },


        { "did-you-mean: typo in fn name",
          "x = [1,2,3]\nlen(x)\nprint(\"ok\")\n",
          "ok\n" },

        { "did-you-mean: typo raises, and (since there's no implicit recovery) must be caught explicitly to keep going",
          "try\npint(\"hello\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\nprint(\"after\")\n",
          "caught\nafter\n" },



        { "fn_name: native returns plain name",
          "print(fn_name(len))\n",
          "len\n" },

        { "fn_name: script fn includes provenance suffix",
          // fn_name returns  name@filename:line  for script fns
          "foo() = 1\n"
          "n = fn_name(foo)\n"
          "if n == \"foo\"\n"
          "  print(\"no provenance\")\n"
          "else\n"
          "  print(\"has provenance\")\n"
          "end\n",
          "has provenance\n" },

        { "fn_arity: script fn and native",
          "foo(a, b) = a + b\nprint(fn_arity(foo))\nprint(fn_arity(print))\n",
          "2\n0\n" },

        { "fn_sig: unannotated",
          "add(x, y) = x + y\nprint(fn_sig(add))\n",
          "add(x, y)\n" },

        { "fn_sig: annotated params and return",
          "double(x: num) -> num = x * 2\nprint(fn_sig(double))\n",
          "double(x: number|int) -> number|int\n" },
        { "is_native",
          "print(is_native(print))\nbaz() = 0\nprint(is_native(baz))\n",
          "1\n0\n" },



        { "annotation: param type enforced, must be caught explicitly",
          "fn lh(v) return v end double(x: num) -> num = x * 2\n"
          "try\ndouble(lh(\"bad\"))\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\nprint(\"ok\")\n",
          "caught\nok\n" },

        { "annotation: correct types pass through",
          "greet(name: str) -> str = \"hi \" + name\nprint(greet(\"world\"))\n",
          "hi world\n" },

        { "annotation: return type mismatch raises, must be caught explicitly",
          "fn lh(v) return v end fn bad() -> num\nreturn lh(\"oops\")\nend\n"
          "try\nbad()\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\nprint(\"ok\")\n",
          "caught\nok\n" },

        { "assert: passes",
          "assert(1)\nprint(\"ok\")\n",
          "ok\n" },



        { "undefined var raises, must be caught explicitly to keep going",
          "try\nprint(bad_undefined_name)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\nprint(\"after\")\n",
          "caught\nafter\n" },



        { "closure: lambda captures enclosing local",
          "fn make_adder(n)\n"
          "  return fn(x) return x + n end\n"
          "end\n"
          "add5 = make_adder(5)\n"
          "print(add5(3))\n"
          "print(add5(10))\n",
          "8\n15\n" },

        { "parser: chained call directly onto a call result",
          "fn make_adder(n)\n"
          "  return fn(x) return x + n end\n"
          "end\n"
          "print(make_adder(1)(2))\n",
          "3\n" },

        { "parser: triple-chained call (curry)",
          "fn curry(a)\n"
          "  return fn(b)\n"
          "    return fn(c) return a + b + c end\n"
          "  end\n"
          "end\n"
          "print(curry(1)(2)(3))\n",
          "6\n" },

        { "closure: named fn inside fn captures locals",
          "fn make_counter(start)\n"
          "  local count = start\n"
          "  fn get()\n"
          "    return count\n"
          "  end\n"
          "  return get\n"
          "end\n"
          "c = make_counter(7)\n"
          "print(c())\n",
          "7\n" },

        { "closure: multiple closures from same call are independent",
          "fn make_adder(n)\n"
          "  return fn(x) return x + n end\n"
          "end\n"
          "add1 = make_adder(1)\n"
          "add100 = make_adder(100)\n"
          "print(add1(0))\n"
          "print(add100(0))\n",
          "1\n100\n" },

        { "closure: lambda in if captures outer local",
          "fn check(x)\n"
          "  local msg = \"big\"\n"
          "  if x < 5\n"
          "    msg = \"small\"\n"
          "  end\n"
          "  return fn() return msg end\n"
          "end\n"
          "f3 = check(3)\n"
          "f9 = check(9)\n"
          "print(f3())\n"
          "print(f9())\n",
          "small\nbig\n" },

        { "closure: deeply nested capture",
          "fn outer(a)\n"
          "  fn inner(b)\n"
          "    return fn(c) return a + b + c end\n"
          "  end\n"
          "  return inner\n"
          "end\n"
          "mid = outer(1)\n"
          "f = mid(2)\n"
          "print(f(3))\n",
          "6\n" },

        // real upvalue semantics: two closures made in the same enclosing call share the live scope layer they closed over
        // (CaptureFrame in core/value.h), so a write through one shows up through the other and in the enclosing function
        // without sharing, a closure kept its own copy and the others never saw its writes
        { "closure: sibling closures over the same enclosing local observe each other's mutations",
          "fn make_counter()\n"
          "  local count = 0\n"
          "  fn increment()\n"
          "    count = count + 1\n"
          "    return count\n"
          "  end\n"
          "  fn get()\n"
          "    return count\n"
          "  end\n"
          "  return [increment, get]\n"
          "end\n"
          "pair = make_counter()\n"
          "inc = pair[0]\n"
          "get = pair[1]\n"
          "print(inc())\n"
          "print(inc())\n"
          "print(get())\n",
          "1\n2\n2\n" },

        { "closure: two separate calls to the same factory get independent, non-interfering shared state",
          "fn make_counter()\n"
          "  local count = 0\n"
          "  fn inc() count = count + 1 return count end\n"
          "  fn get() return count end\n"
          "  return [inc, get]\n"
          "end\n"
          "p1 = make_counter()\n"
          "p2 = make_counter()\n"
          "print(p1[0]())\n"
          "print(p1[0]())\n"
          "print(p2[0]())\n"
          "print(p1[1]())\n"
          "print(p2[1]())\n",
          "1\n2\n1\n2\n1\n" },

        { "closure: a nested closure's mutation is visible back in the enclosing function too, not just to sibling closures",
          "fn make_and_peek()\n"
          "  local n = 0\n"
          "  fn bump() n = n + 1 end\n"
          "  bump()\n"
          "  bump()\n"
          "  return n\n"
          "end\n"
          "print(make_and_peek())\n",
          "2\n" },

        // scoping is lexical: a function sees its own locals, what it closed over, and globals. never its callers' locals
        { "scope: a function cannot read a local of the function that called it",
          "fn sc_inner() return sc_secret end\n"
          "fn sc_outer()\n"
          "  local sc_secret = 42\n"
          "  return sc_inner()\n"
          "end\n"
          "try\n"
          "  print(sc_outer())\n"
          "catch e\n"
          "  print(\"caught\")\n"
          "end\n",
          "caught\n" },

        { "scope: assigning in a function never writes the caller's local, it makes a new local",
          "fn sc_inner()\n"
          "  sc_v = 1\n"
          "  return sc_v\n"
          "end\n"
          "fn sc_outer()\n"
          "  local sc_v = 42\n"
          "  sc_inner()\n"
          "  return sc_v\n"
          "end\n"
          "print(sc_outer())\n",
          "42\n" },

        // the compiler decides per function whether it needs the running frame captured (a function that uses no variable
        // of an enclosing function does not), and whether a name is a global. these mix every case in one function
        // a variable that an inner function uses lives in its own cell, made when the variable is defined. so a loop variable is a
        // new variable on every pass, and a closure keeps it after the loop is over
        { "closure: each pass of a for loop gets its own variable, and a closure keeps it after the loop",
          "import \"embrlib\"\n"
          "sc_fns = []\n"
          "for sc_i in [1, 2, 3]\n"
          "  sc_fns = push(sc_fns, fn() return sc_i end)\n"
          "end\n"
          "print(sc_fns[0]())\nprint(sc_fns[1]())\nprint(sc_fns[2]())\n"
          "fn sc_mk()\n"
          "  out = []\n"
          "  for k in [10, 20]\n"
          "    out = push(out, fn() k = k + 1 return k end)\n"
          "  end\n"
          "  return out\n"
          "end\n"
          "r = sc_mk()\n"
          "print(r[0]())\nprint(r[0]())\nprint(r[1]())\n",
          "1\n2\n3\n11\n12\n21\n" },

        { "closure: nested functions can call each other whichever is defined first",
          "fn sc_outer()\n"
          "  fn even(n) if n == 0 return 1 end return odd(n - 1) end\n"
          "  fn odd(n) if n == 0 return 0 end return even(n - 1) end\n"
          "  return even(10)\n"
          "end\n"
          "print(sc_outer())\n",
          "1\n" },

        { "closure: a name the enclosing function only assigns resolves to the global when one exists, or to a later local",
          "sc_gx = 0\n"
          "fn sc_outer2()\n"
          "  sc_gx = 5\n"
          "  f = fn() return sc_gx end\n"
          "  return f()\n"
          "end\n"
          "print(sc_outer2())\nprint(sc_gx)\n"
          "fn sc_outer3()\n"
          "  f = fn() return later end\n"
          "  later = 42\n"
          "  return f()\n"
          "end\n"
          "print(sc_outer3())\n",
          "5\n5\n42\n" },

        { "closure: every call of a function has its own captured variables, including parameters",
          "fn sc_adder(x) return fn(y) return x + y end end\n"
          "print(sc_adder(3)(4))\n"
          "fn sc_rec(n)\n"
          "  local v = n\n"
          "  local f = fn() return v end\n"
          "  if n > 0 sc_rec(n - 1) end\n"
          "  return f()\n"
          "end\n"
          "print(sc_rec(3))\n",
          "7\n3\n" },

        { "scope: closures that capture, closures that don't, and a parameter that shadows an outer local",
          "fn sc_outer()\n"
          "  local a = 1\n"
          "  local b = 2\n"
          "  f = fn(x) return x + 1 end\n"
          "  g = fn() a = a + 10 return a end\n"
          "  h = fn() return g() + b end\n"
          "  return [f(5), g(), h(), a]\n"
          "end\n"
          "r = sc_outer()\n"
          "print(r[0])\nprint(r[1])\nprint(r[2])\nprint(r[3])\n"
          "fn sc_mk()\n"
          "  local v = 100\n"
          "  return fn(v) return v end\n"
          "end\n"
          "print(sc_mk()(3))\n"
          "fn sc_l1()\n"
          "  local deep = \"deep\"\n"
          "  fn l2()\n"
          "    fn l3() return deep end\n"
          "    return l3()\n"
          "  end\n"
          "  return l2()\n"
          "end\n"
          "print(sc_l1())\n",
          "6\n11\n23\n21\n3\ndeep\n" },

        { "scope: a closure made at the top level that captures nothing leaves later top-level code alone",
          "sc_cb = fn() return 7 end\n"
          "sc_acc = 0\n"
          "sc_j = 0\n"
          "while sc_j < 5\n"
          "  sc_acc = sc_acc + sc_cb()\n"
          "  sc_j = sc_j + 1\n"
          "end\n"
          "print(sc_acc)\n"
          "for sc_i in [1, 2, 3]\n"
          "  sc_show = fn() return sc_i end\n"
          "  print(sc_show())\n"
          "end\n"
          "sc_total = 0\n"
          "fn sc_bump() sc_total = sc_total + 5 return sc_total end\n"
          "sc_bump()\n"
          "print(sc_bump())\n"
          "print(sc_total)\n",
          "35\n1\n2\n3\n10\n10\n" },

        { "scope: a closure reads the variables it was made next to, not the locals of whoever calls it",
          "fn sc_make()\n"
          "  local sc_v = 7\n"
          "  fn get() return sc_v end\n"
          "  return get\n"
          "end\n"
          "sc_g = sc_make()\n"
          "fn sc_other()\n"
          "  local sc_v = 1\n"
          "  return sc_g()\n"
          "end\n"
          "print(sc_other())\n",
          "7\n" },



        { "int/float: literal defaults",
          "print(type(5))\nprint(type(5.0))\nprint(5 == 5.0)\n",
          "int\nnumber\n1\n" },

        { "int: exact arithmetic beyond double precision",
          "print(9007199254740993 + 1)\n",
          "9007199254740994\n" },

        { "int: division always widens to float",
          "print(7 / 2)\nprint(7 % 2)\n",
          "3.5\n1\n" },

        { "variadic: fn collects trailing args into an array",
          "fn add_all(...nums)\n"
          "  total = 0\n"
          "  for n in nums\n"
          "    total = total + n\n"
          "  end\n"
          "  return total\n"
          "end\n"
          "print(add_all(1,2,3,4))\nprint(add_all())\n",
          "10\n0\n" },

        { "variadic: lambda variadic",
          "f = fn(...args) return len(args) end\nprint(f(1,2,3))\n",
          "3\n" },

        { "local typed: enforces declared type on later assignment",
          "local int x = 5\nprint(x)\nx = 10\nprint(x)\n",
          "5\n10\n" },

        { "destructure: comma form with mixed types",
          "local int a, str b, c = [1, \"2\", 3]\nprint(a)\nprint(b)\nprint(c)\n",
          "1\n2\n3\n" },

        { "destructure: bracket form with shared type",
          "local int [a, b] = [10, 20]\nprint(a)\nprint(b)\n",
          "10\n20\n" },

        { "destructure: map unpacks to (keys, values)",
          "local k, v = {\"only\": 42}\nprint(k[0])\nprint(v[0])\n",
          "only\n42\n" },

        { "destructure: plain multi-assign reuses existing variables",
          "a = 0\nb = 0\na, b = [7, 8]\nprint(a)\nprint(b)\n",
          "7\n8\n" },

        { "try/catch: catches error() and binds message",
          "try\nerror(\"boom\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught: \" + e[\"message\"])\nend\nprint(\"after\")\n",
          "caught: boom\nafter\n" },

        { "try/catch: catches assert() failure",
          "try\nassert(0, \"nope\")\ncatch e\nprint(e[\"message\"])\nend\n",
          "nope\n" },

        // e is a map ({message, kind, line}), not a bare string -- see
        // core/value.h's errorToMap. kind is raiseError()'s own first
        // argument (e.g. "error" for the error() native, "assert" for
        // assert()), reused as-is rather than a separate invented taxonomy.
        { "try/catch: e is a map with message/kind/line, not a bare string",
          "try\nerror(\"boom\")\ncatch e\n"
          "print(e[\"message\"])\n"
          "print(e[\"kind\"])\n"
          "print(e[\"line\"] > 0)\n"
          "end\n"
          "try\nassert(0, \"nope\")\ncatch e\nprint(e[\"kind\"])\nend\n",
          "boom\nerror\n1\nassert\n" },

        { "try/catch: no error means catch block is skipped",
          "try\nprint(\"ok\")\ncatch e\nprint(\"unreachable\")\nend\n",
          "ok\n" },

        { "try/catch: catches an error raised deep in a called function",
          "fn risky(n)\nif n < 0\nerror(\"negative\")\nend\nreturn n * 2\nend\n"
          "fn safe(n)\ntry\nreturn risky(n)\ncatch e\nreturn -1\nend\nend\n"
          "print(safe(5))\nprint(safe(-5))\n",
          "10\n-1\n" },

        { "try/catch: catch var is block-scoped",
          "try\nerror(\"x\")\ncatch e\nprint(e[\"message\"])\nend\n"
          "try\nprint(e)\nprint(\"leaked\")\ncatch e2\nprint(\"properly scoped\")\nend\n",
          "x\nproperly scoped\n" },

        { "try/catch: break inside try still exits the enclosing loop",
          "i = 0\nwhile i < 3\ntry\nif i == 1\nbreak\nend\nprint(i)\ncatch e\nprint(\"unreachable\")\nend\ni = i + 1\nend\nprint(\"i=\" + str(i))\n",
          "0\ni=1\n" },

        { "try/catch: error inside a loop iteration is caught and looping continues",
          "i = 0\nwhile i < 3\ntry\nif i == 1\nerror(\"skip\")\nend\nprint(i)\ncatch e\nprint(\"caught \" + str(i))\nend\ni = i + 1\nend\n",
          "0\ncaught 1\n2\n" },

        { "slice: supports maps by key-index range",
          "m = {\"a\": 1}\nprint(len(slice(m, 0, 1)))\n",
          "1\n" },

        // import's path is an expression, not only a string literal, and importing an embr module changes the importer's
        // scope directly: exported (non-local) top-level names become visible, `local` ones don't. the module is written
        // with write_file(), so no fixture file has to exist on disk
        { "import: computed path expression loads an embr module and exports non-locals, hides local",
          "write_file(\"test_core_importmod.embr\","
          " \"fn __tcim_square(x)\\n    return x * x\\nend\\n\\nlocal __tcim_secret = 1\\n\")\n"
          "name = \"test_core_importmod\"\n"
          "import name + \".embr\"\n"
          "print(__tcim_square(4))\n"
          "try\nprint(__tcim_secret)\nprint(\"unreachable\")\ncatch e\nprint(\"hidden\")\nend\n",
          "16\nhidden\n" },

        // load_module()'s semantics deliberately differ from import: it
        // returns the module's exports as a map instead of touching the
        // caller's scope (see core/invoke.h's loadEmbrModule), and re-loading
        // an already-loaded path is a no-op (returns {}) unless reload is truthy
        { "load_module: returns exports as a map without mutating caller scope; dedups until reload",
          "write_file(\"test_core_loadmod.embr\","
          " \"fn __tclm_square(x)\\n    return x * x\\nend\\n\\nlocal __tclm_secret = 1\\n\")\n"
          "m = load_module(\"test_core_loadmod\")\n"
          "print(m[\"__tclm_square\"](5))\n"
          "print(has(m, \"__tclm_secret\"))\n"
          "try\nprint(__tclm_square(1))\nprint(\"unreachable\")\ncatch e\nprint(\"not in caller scope\")\nend\n"
          "n = load_module(\"test_core_loadmod\")\n"
          "print(len(keys(n)))\n"
          "r = load_module(\"test_core_loadmod\", 1)\n"
          "print(r[\"__tclm_square\"](6))\n",
          "25\n0\nnot in caller scope\n0\n36\n" },

        // regression test for a VM crash: a module-exported function that builds a nested closure (MAKE_CLOSURE), called
        // from the importer after the module's CompiledProgram was destroyed. MAKE_CLOSURE used to look its function up
        // through the running VmRunner's prog_, which is wrong (or dangling) outside the module. now every CompiledChunk
        // carries its own sibling list (CompiledChunk::siblings in vm.h)
        { "import: a module-exported function that builds a nested closure still works after the module is torn down",
          "write_file(\"test_core_nestedclosure.embr\","
          " \"fn make_adder(n)\\n    return fn(x) return x + n end\\nend\\n\")\n"
          "import \"test_core_nestedclosure.embr\"\n"
          "add5 = make_adder(5)\n"
          "print(add5(10))\n"
          "print(add5(20))\n",
          "15\n25\n" },

        // a module's own `local` top-level names (a helper closure and a constant here) must stay reachable when an
        // exported function using them is called from outside the module, even though popModuleScope() removes the
        // module's top scope when `import` finishes. closures share a live reference to that scope, which keeps it alive
        // (CaptureFrame in core/value.h)
        { "import: an exported function can still see the module's own `local` helper/constant after the module is torn down",
          "write_file(\"test_core_modulelocal.embr\","
          " \"local helper = fn(x) return x * 2 end\\n"
          "local greeting = \\\"hi\\\"\\n"
          "fn use_helper(x)\\n    return helper(x)\\nend\\n"
          "fn use_greeting()\\n    return greeting\\nend\\n\")\n"
          "import \"test_core_modulelocal.embr\"\n"
          "print(use_helper(21))\n"
          "print(use_greeting())\n",
          "42\nhi\n" },

        // companion to the test above: non-`local` top-level names must NOT be frozen into per-function snapshots, they
        // keep resolving through the importer's merged scope. modules/ffi.embr relies on this (ffi_typedef() and
        // ffi_cdef() share one plain top-level map). here two exported functions share a plain `registry` map, and a
        // write through one must show up through the other after import
        { "import: two exported functions sharing a non-`local` top-level binding still observe each other's writes to it",
          "write_file(\"test_core_moduleshared.embr\","
          " \"registry = {}\\n"
          "fn set_thing(k, v)\\n    registry[k] = v\\nend\\n"
          "fn get_thing(k)\\n    return registry[k]\\nend\\n\")\n"
          "import \"test_core_moduleshared.embr\"\n"
          "set_thing(\"a\", 1)\n"
          "print(get_thing(\"a\"))\n",
          "1\n" },

        // a module can also share a genuinely `local` (module-private) mutable variable between two exported functions.
        // modules/ffi.embr still keeps __ffi_typedefs as a plain binding, but this confirms the `local` style works too
        { "import: two exported functions can now share a genuinely `local` (module-private) mutable variable",
          "write_file(\"test_core_modulelocalshared.embr\","
          " \"local registry = {}\\n"
          "fn set_thing(k, v)\\n    registry[k] = v\\nend\\n"
          "fn get_thing(k)\\n    return registry[k]\\nend\\n\")\n"
          "import \"test_core_modulelocalshared.embr\"\n"
          "set_thing(\"a\", 1)\n"
          "print(get_thing(\"a\"))\n",
          "1\n" },

        // block comments: #[ ... ]# comments out a whole span without
        // prefixing every line with '#' (see lexer.h's skipBlockComment).
        // doesn't nest -- the first ']#' closes it even if a '#[' appears inside.
        { "block comment: #[ ... ]# skips a multi-line span, including code that would otherwise run",
          "print(1)\n"
          "#[ this entire block, including the print below, is a comment\n"
          "   print(999)\n"
          "   x = 1 / 0\n"
          "]#\n"
          "print(2)\n",
          "1\n2\n" },

        { "block comment: doesn't nest -- a literal '#[' inside one doesn't start a nested comment",
          "#[ this is #[ not nested ]#\n"
          "print(\"after\")\n",
          "after\n" },

        { "sort_cmp: comparator sort is stable and honours descending",
          "words = [\"ccc\", \"a\", \"bb\", \"dd\", \"e\"]\n"
          "print(join(sort_cmp(words, fn(a, b) return len(a) - len(b) end), \",\"))\n"
          "print(join(sort_cmp(words, fn(a, b) return len(a) - len(b) end, 1), \",\"))\n",
          "a,e,bb,dd,ccc\nccc,bb,dd,a,e\n" },

        { "sort_cmp: inconsistent comparator does not crash, non-number result raises",
          "big_in = range(0, 200)\n"
          "big_out = sort_cmp(big_in, fn(x, y) return 1 end)\n"
          "print(len(big_out))\n"
          "try\nsort_cmp([2, 1], fn(x, y) return \"no\" end)\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "200\ncaught\n" },

        { "sort_do: stable for equal weights",
          "print(join(sort_do([\"b1\", \"a1\", \"a2\", \"b2\"], fn(s) return 0 end), \",\"))\n",
          "b1,a1,a2,b2\n" },

        { "zip: pairs by index, stops at the shortest input",
          "z = zip([1,2,3], [10,20])\nprint(len(z))\nprint(z[1][0])\nprint(z[1][1])\n"
          "print(len(zip([1,2], [3,4], [5,6])[0]))\n",
          "2\n2\n20\n3\n" },

        { "enumerate: indices with optional start",
          "e = enumerate([\"a\",\"b\"], 5)\nprint(e[1][0])\nprint(e[1][1])\nprint(enumerate([\"x\"])[0][0])\n",
          "6\nb\n0\n" },

        { "concat: arrays and strings, mixing raises",
          "print(len(concat([1,2], [3], [4,5])))\nprint(concat(\"ab\", \"cd\", \"e\"))\n"
          "try\nconcat([1], \"x\")\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "5\nabcde\ncaught\n" },

        { "items: [key, value] pairs in alphabetical key order",
          "it = items({\"b\": 2, \"a\": 1})\nprint(it[0][0])\nprint(it[0][1])\nprint(it[1][0])\n",
          "a\n1\nb\n" },

        { "min / max / sum: ints stay exact, empty and non-numeric raise",
          "print(min([3, 1, 2]))\nprint(max([3, 1.5, 2]))\nprint(sum([1, 2, 3]))\nprint(sum([1, 2.5]))\nprint(sum([]))\n"
          "try\nmax([])\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n"
          "try\nsum([1, \"x\"])\nprint(\"unreachable\")\ncatch e\nprint(\"caught\")\nend\n",
          "1\n3\n6\n3.5\n0\ncaught\ncaught\n" },

        { "for_each_do + sum compose",
          "print(sum(for_each_do(range(1, 4), fn(x) return x * x end)))\n",
          "14\n" },

        { "slice: string end is an index (not a length); clamps, NaN/huge safe",
          "print(slice(\"abcdef\", 1, 3))\nprint(slice(\"abcdef\", 2))\nprint(slice(\"abcdef\", 4, 2) == \"\")\n"
          "print(slice(\"abcdef\", -5, 100))\nprint(len(slice([1,2,3,4], 1, 3)))\n"
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\nprint(slice(\"abc\", 1, big))\n",
          "bc\ncdef\n1\nabcdef\n2\nbc\n" },

        { "int overflow: + - * and unary - widen to float instead of wrapping (UB)",
          "big = 9223372036854775807\nprint(big + 1)\nprint(-big - 2)\nprint(big * 2)\nprint(big + 0)\n"
          "low = -big - 1\nprint(low)\nprint(-low)\nprint(low - 1)\n",
          "9223372036854775808\n-9223372036854775808\n18446744073709551616\n9223372036854775807\n-9223372036854775808\n9223372036854775808\n-9223372036854775808\n" },

        { "int overflow: INT64_MIN % -1 is 0 (used to kill the process with SIGFPE), ordinary % unchanged",
          "low = -9223372036854775807 - 1\nprint(low % -1)\nprint(7 % -1)\nprint(-7 % 3)\nprint(7 % 3)\n",
          "0\n0\n-1\n1\n" },

        { "scope: a name first assigned inside an if-block, then reassigned at top level, is one variable (VM used to split it and loop forever)",
          "if 1\n  i = 0\nend\ni = 0\nn = 0\nwhile i < 3\n  i += 1\n  n += 1\n  if n > 10\n    break\n  end\nend\nprint(i)\nprint(n)\n",
          "3\n3\n" },

        { "scope: first assignment inside a loop body is visible after the loop and to later top-level code",
          "k = 0\nwhile k < 2\n  made = k * 10\n  k += 1\nend\nprint(made)\nmade = made + 1\nprint(made)\n",
          "10\n11\n" },

        { "return at top level ends the script quietly (it used to std::terminate)",
          "print(\"a\")\nreturn 1\nprint(\"b\")\n",
          "a\n" },

        { "return inside a top-level if/while ends the script too",
          "i = 0\nwhile i < 5\n  print(i)\n  if i == 1\n    return 0\n  end\n  i += 1\nend\nprint(\"unreachable\")\n",
          "0\n1\n" },

        { "error trace: one frame per script function unwound, innermost first, with the call-site line",
          "fn inner(n)\n"                         // 1
          "    error(\"boom \" + str(n))\n"       // 2
          "end\n"                                // 3
          "fn middle(n)\n"                        // 4
          "    inner(n + 1)\n"                    // 5  <- inner called here
          "end\n"                                // 6
          "fn outer()\n"                          // 7
          "    middle(1)\n"                       // 8  <- middle called here
          "end\n"                                // 9
          "try\n"                                // 10
          "    outer()\n"                         // 11 <- outer called here
          "catch e\n"
          "    print(e[\"message\"])\n"
          "    for fr in e[\"trace\"]\n"
          "        print(fr[\"fn\"] + \":\" + str(fr[\"line\"]))\n"
          "    end\n"
          "end\n",
          "boom 2\ninner:5\nmiddle:8\nouter:11\n" },

        { "error trace: empty for an error raised at top level; only the frames below the handler otherwise",
          "try\n  error(\"top\")\ncatch e\n  print(len(e[\"trace\"]))\nend\n"
          "fn inner()\n  error(\"x\")\nend\n"
          "fn middle()\n"
          "    try\n"
          "        inner()\n"
          "    catch e\n"
          "        print(len(e[\"trace\"]))\n"
          "        print(e[\"trace\"][0][\"fn\"])\n"
          "    end\n"
          "end\n"
          "middle()\n",
          "0\n1\ninner\n" },

        { "error trace: a callback invoked by a native reports line 0 (native call), then the script function that called the native",
          "fn run()\n"                                                // 1
          "    for_each_do([1], fn(x) error(\"bad\") end)\n"          // 2
          "end\n"
          "try\n"
          "    run()\n"                                                // 5
          "catch e\n"
          "    for fr in e[\"trace\"]\n"
          "        print(fr[\"fn\"] + \":\" + str(fr[\"line\"]))\n"
          "    end\n"
          "end\n",
          "<lambda>:0\nrun:5\n" },

        { "error trace: a multi-line call expression is reported at the line the call starts on",
          "fn f(a, b)\n"                                               // 1
          "    error(\"x\")\n"                                        // 2
          "end\n"
          "try\n"
          "    f(1,\n"                                                  // 5
          "      2)\n"                                                  // 6
          "catch e\n"
          "    print(e[\"trace\"][0][\"line\"])\n"
          "end\n",
          "5\n" },

        { "callable ==: identity -- same function object is equal, separate closures are not, natives compare by identity too",
          "fn f(x) return x end\n"
          "g = f\n"
          "h = fn(x) return x end\n"
          "k = fn(x) return x end\n"
          "print(f == f)\nprint(g == f)\nprint(f == h)\nprint(h == k)\nprint(h != k)\n"
          "print(print == print)\nprint(print == f)\n"
          "fns = [f, h]\nprint(fns[0] == f)\nprint(fns[1] == k)\n"
          "mk = fn() return fn() return 1 end end\nprint(mk() == mk())\n",
          "1\n1\n0\n0\n1\n1\n0\n1\n0\n0\n" },

        { "scope: a name first assigned inside a function is local to that call (no leak, no sharing between calls)",
          "fn f(n)\n"
          "    seen = n * 2\n"
          "    return seen\n"
          "end\n"
          "print(f(3))\nprint(f(5))\n"
          "try\n  print(seen)\ncatch e\n  print(\"seen is not visible outside\")\nend\n",
          "6\n10\nseen is not visible outside\n" },

        { "scope: recursion gets a fresh local per call (an implicit global would be clobbered by the inner call)",
          "fn fact(n)\n"
          "    if n <= 1\n"
          "        return 1\n"
          "    end\n"
          "    rest = fact(n - 1)\n"
          "    return n * rest\n"
          "end\n"
          "print(fact(5))\n",
          "120\n" },

        { "scope: a name first assigned in a block inside a function is visible for the rest of that function; existing globals and captured names are still updated",
          "counter = 0\n"
          "fn bump()\n"
          "    if 1\n"
          "        tmp = 5\n"
          "    end\n"
          "    counter = counter + tmp\n"
          "    return tmp\n"
          "end\n"
          "print(bump())\nprint(counter)\n"
          "fn make()\n"
          "    n = 0\n"
          "    return fn()\n"
          "        n = n + 1\n"
          "        return n\n"
          "    end\n"
          "end\n"
          "c = make()\nc()\nprint(c())\n",
          "5\n5\n2\n" },

        { "resource limits: a huge range(), and NaN/huge numbers handed to natives, raise instead of exhausting memory or hitting UB",
          "big = 1.5\ni = 0\nwhile i < 40\nbig = big * 1000.0\ni = i + 1\nend\n"
          "r = range(0, 100000000000)\nprint(len(r))\n"
          "try\n  r[0] = 1\n  print(\"unreachable\")\ncatch e\n  print(\"range capped\")\nend\n"
          "try\n  range(0, big)\n  print(\"unreachable\")\ncatch e\n  print(\"range huge capped\")\nend\n"
          "print(len(range(0, 1000)))\n",
          "100000000000\nrange capped\nrange huge capped\n1000\n" },

        { "string escapes: \\xNN is one byte, \\uXXXX and \\u{...} are UTF-8 encoded code points",
          "print(len(\"\\x41\"))\nprint(\"\\x41\\x42\")\nprint(len(\"\\xff\"))\n"
          "print(\"\\u00e9\" == \"\\xc3\\xa9\")\nprint(len(\"\\u00e9\"))\nprint(len(\"\\u20ac\"))\n"
          "print(len(\"\\u{1F600}\"))\nprint(\"\\u{41}\" == \"A\")\nprint(len(\"a\\x00b\"))\n",
          "1\nAB\n1\n1\n2\n3\n4\n1\n3\n" },

        { "number literals: exponent forms are floats (1e3, 2.5e-3, 1E+2); an 'e' without digits is not an exponent",
          "print(1e3)\nprint(2.5e-3)\nprint(1E+2)\nprint(type(1e3))\nprint(type(1000))\nprint(1e3 == 1000)\n"
          "print(6.02e23 > 1e23)\ne5 = 7\nprint(1 + e5)\nx = 3\nprint(x*1e2)\n",
          "1000\n0.0025\n100\nnumber\nint\n1\n1\n8\n300\n" },

        { "embrlib predicate helpers: find_do, find_index_do, index_of, any_do/all_do (short-circuit), count_do, partition",
          "nums = [4, 9, 2, 7]\n"
          "print(find_do(nums, fn(x) return x > 5 end))\nprint(find_do(nums, fn(x) return x > 50 end, -1))\n"
          "try\n  find_do(nums, fn(x) return x > 50 end)\n  print(\"unreachable\")\ncatch e\n  print(\"no match raises\")\nend\n"
          "print(find_index_do(nums, fn(x) return x == 2 end))\nprint(find_index_do(nums, fn(x) return x == 3 end))\n"
          "print(index_of([1, 2, 3], 3))\nprint(index_of([1, 2, 3], 5))\nprint(index_of([1, 2], 2.0))\nprint(index_of([\"5\"], 5))\n"
          "calls = 0\n"
          "print(any_do(nums, fn(x) return x > 3 end))\nprint(any_do([], fn(x) return 1 end))\n"
          "print(all_do(nums, fn(x) return x > 1 end))\nprint(all_do(nums, fn(x) return x > 3 end))\nprint(all_do([], fn(x) return 0 end))\n"
          "print(count_do(nums, fn(x) return x % 2 == 0 end))\n"
          "p = partition(nums, fn(x) return x > 5 end)\nprint(p[0])\nprint(p[1])\n",
          "9\n-1\nno match raises\n2\n-1\n2\n-1\n-1\n-1\n1\n0\n1\n0\n1\n2\n[9, 7]\n[4, 2]\n" },

        { "embrlib reshaping helpers: unique (strict, stable), flatten, reverse, chunk, take/drop, group_by, min_do/max_do",
          "print(unique([3, 1, 3, 2, 1]))\nprint(len(unique([1, 1.0, \"1\", 1])))\nprint(unique([[1], [1], [2]]))\nprint(unique([]))\n"
          "print(flatten([1, [2, [3, [4]]]]))\nprint(flatten([1, [2, [3, [4]]]], 2))\nprint(flatten([1, [2, [3, [4]]]], 0))\n"
          "print(reverse([1, 2, 3]))\nprint(reverse(\"abc\"))\n"
          "print(chunk([1, 2, 3, 4, 5], 2))\nprint(chunk([], 3))\n"
          "print(take([1, 2, 3], 2))\nprint(take([1, 2, 3], 9))\nprint(drop([1, 2, 3], 1))\nprint(drop([1, 2, 3], -4))\n"
          "g = group_by([\"apple\", \"avocado\", \"banana\", \"blueberry\", \"cherry\"], fn(w) return slice(w, 0, 1) end)\n"
          "print(g[\"a\"])\nprint(g[\"b\"])\nprint(len(keys(g)))\n"
          "by_len = group_by([\"a\", \"bb\", \"c\"], fn(w) return len(w) end)\nprint(by_len[\"1\"])\n"
          "words = [\"pear\", \"fig\", \"banana\", \"kiwi\"]\n"
          "print(min_do(words, fn(w) return len(w) end))\nprint(max_do(words, fn(w) return len(w) end))\n"
          "try\n  chunk([1], 0)\n  print(\"unreachable\")\ncatch e\n  print(\"chunk 0 rejected\")\nend\n"
          "try\n  min_do([], fn(w) return 1 end)\n  print(\"unreachable\")\ncatch e\n  print(\"empty rejected\")\nend\n"
          "try\n  group_by([1], fn(x) return [x] end)\n  print(\"unreachable\")\ncatch e\n  print(\"bad key rejected\")\nend\n",
          "[3, 1, 2]\n3\n[[1], [2]]\n[]\n"
          "[1, 2, [3, [4]]]\n[1, 2, 3, [4]]\n[1, [2, [3, [4]]]]\n"
          "[3, 2, 1]\ncba\n[[1, 2], [3, 4], [5]]\n[]\n"
          "[1, 2]\n[1, 2, 3]\n[2, 3]\n[1, 2, 3]\n"
          "[\"apple\", \"avocado\"]\n[\"banana\", \"blueberry\"]\n3\n[\"a\", \"c\"]\n"
          "fig\nbanana\nchunk 0 rejected\nempty rejected\nbad key rejected\n" },

        { "embrlib helpers: callbacks through a __call pointer-compatible path, and a throwing callback propagates (no crash)",
          "fn lh(v) return v end try\n  any_do([1, 2], fn(x) error(\"cb failed\") end)\n  print(\"unreachable\")\ncatch e\n  print(e[\"message\"])\nend\n"
          "try\n  unique(lh(5))\n  print(\"unreachable\")\ncatch e\n  print(\"type checked\")\nend\n"
          "print(len(unique(range(0, 2000))))\n",
          "cb failed\ntype checked\n2000\n" },

        { "in-place index assignment and push keep value semantics (copies made before the write don't change)",
          "ip1_a = [1, 2, 3]\nip1_b = ip1_a\nip1_a[0] = 10\nprint(ip1_a)\nprint(ip1_b)\n"
          "ip1_c = [1]\nip1_d = ip1_c\nip1_c = push(ip1_c, 2)\nprint(ip1_c)\nprint(ip1_d)\n"
          "ip1_m = {\"x\": 1}\nip1_n = ip1_m\nip1_m[\"x\"] = 5\nip1_m[\"y\"] = 6\nprint(ip1_m[\"x\"])\nprint(ip1_n[\"x\"])\nprint(len(keys(ip1_n)))\n"
          "ip1_s = \"hello\"\nip1_t = ip1_s\nip1_s[0] = \"J\"\nprint(ip1_s)\nprint(ip1_t)\n"
          "ip1_g = [[1, 2], [3, 4]]\nip1_h = ip1_g\nip1_g[1][0] = 99\nprint(ip1_g)\nprint(ip1_h)\n"
          "fn f()\n  ip1_arr = []\n  ip1_i = 0\n  while ip1_i < 4\n    ip1_arr = push(ip1_arr, ip1_i)\n    ip1_i += 1\n  end\n  ip1_saved = ip1_arr\n  ip1_arr[1] = 50\n  return [ip1_arr, ip1_saved]\nend\n"
          "print(f())\n",
          "[10, 2, 3]\n[1, 2, 3]\n[1, 2]\n[1]\n5\n1\n1\nJello\nhello\n[[1, 2], [99, 4]]\n[[1, 2], [3, 4]]\n"
          "[[0, 50, 2, 3], [0, 1, 2, 3]]\n" },

        { "index reads and writes with calls in the key or value (the variable is read before the call runs)",
          "fn id(ip2_x)\n  return ip2_x\nend\n"
          "ip2_a = [1, 2, 3]\nprint(ip2_a[id(1)])\nip2_a[id(2)] = id(30)\nprint(ip2_a)\n"
          "fn bump()\n  ip2_a = [7, 7]\n  return 1\nend\n"
          "ip2_a = push(ip2_a, bump())\nprint(ip2_a)\n"
          "fn local_work()\n  ip2_v = [1, 2, 3]\n  ip2_v[id(0)] = ip2_v[id(1)] + 1\n  ip2_v = push(ip2_v, id(9))\n  return ip2_v\nend\nprint(local_work())\n"
          "ip2_m = {}\nip2_m[\"k\" + str(id(1))] = id(5)\nprint(ip2_m[\"k\" + str(id(1))])\n",
          "2\n[1, 2, 30]\n[1, 2, 30, 1]\n[3, 2, 3, 9]\n5\n" },

        { "in-place index assignment still caps nesting depth",
          "local arr<any> ip4_t = [0]\nip4_i = 0\ntry\n  while ip4_i < 1500\n    ip4_t[0] = [ip4_t[0]]\n    ip4_i += 1\n  end\n  print(\"no error\")\ncatch e\n  print(\"too deep\")\nend\n"
          "local arr<any> ip4_u = [0]\nip4_j = 0\ntry\n  while ip4_j < 1500\n    ip4_u = push([], ip4_u)\n    ip4_j += 1\n  end\n  print(\"no error\")\ncatch e\n  print(\"too deep\")\nend\n"
          "local arr<any> ip4_w = [[1]]\nip4_w[0] = 5\nip4_w[0] = [[2]]\nprint(ip4_w)\n",
          "too deep\ntoo deep\n[[[2]]]\n" },

        { "in-out natives: &name changes the variable where it lives, copies made earlier don't change",
          "rf_a = [1, 2]\nrf_n = append(&rf_a, 3)\nprint(rf_a)\nprint(rf_n)\n"
          "rf_b = rf_a\nappend(&rf_a, 4)\nprint(rf_a)\nprint(rf_b)\n"
          "rf_g = [[1], [2, 3]]\nrf_h = rf_g\nappend(&rf_g[1], 9)\nprint(rf_g)\nprint(rf_h)\n"
          "rf_m = {\"list\": [1], \"x\": 2}\nappend(&rf_m[\"list\"], 5)\nprint(rf_m)\n"
          "print(remove_last(&rf_a))\nprint(rf_a)\nprint(remove_at(&rf_a, 0))\nprint(rf_a)\n"
          "insert(&rf_a, 1, 42)\nprint(rf_a)\ninsert(&rf_a, 3, 7)\nprint(rf_a)\n"
          "print(remove_key(&rf_m, \"x\"))\nprint(remove_key(&rf_m, \"x\"))\nprint(rf_m)\n"
          "fn rf_work()\n  l = []\n  i = 0\n  while i < 4\n    append(&l, i * i)\n    i += 1\n  end\n  c = l\n  append(&l, -1)\n"
          "  nested = {\"a\": [[0]]}\n  append(&nested[\"a\"][0], 7)\n  return [l, c, nested]\nend\nprint(rf_work())\n",
          "[1, 2, 3]\n3\n[1, 2, 3, 4]\n[1, 2, 3]\n[[1], [2, 3, 9]]\n[[1], [2, 3]]\n{list: [1, 5], x: 2}\n"
          "4\n[1, 2, 3]\n1\n[2, 3]\n[2, 42, 3]\n[2, 42, 3, 7]\n1\n0\n{list: [1, 5]}\n"
          "[[0, 1, 4, 9, -1], [0, 1, 4, 9], {a: [[0, 7]]}]\n" },

        { "in-out natives: a missing & , a stray & , and a reference that points at nothing are errors",
          "rf_x = [1, 2]\nrf_y = {\"k\": 1}\nfn rf_id(v)\n  return v\nend\n"
          "fn rf_try(code)\n  try\n    code()\n    print(\"unreachable\")\n  catch e\n    print(\"E\")\n  end\nend\n"
          "rf_app = [append][0]\nrf_len = [len][0]\nrf_try(fn() rf_app(rf_x, 1) end)\nrf_try(fn() rf_len(&rf_x) end)\nrf_try(fn() rf_id(&rf_x) end)\n"
          "rf_try(fn() append(&rf_nope, 1) end)\nrf_try(fn() append(&rf_x[10], 1) end)\n"
          "rf_try(fn() append(&rf_y[\"nokey\"], 1) end)\nrf_try(fn() append(&rf_y, 1) end)\n"
          "rf_try(fn() insert(&rf_x, 9, 1) end)\nrf_try(fn() remove_at(&rf_x, 5) end)\n"
          "rf_e = []\nrf_try(fn() remove_last(&rf_e) end)\nprint(rf_x)\nprint(rf_y)\n",
          "E\nE\nE\nE\nE\nE\nE\nE\nE\nE\n[1, 2]\n{k: 1}\n" },

        { "in-out natives: nesting depth is still capped when the reference is deep inside a structure",
          "rf_t = [[]]\nrf_c = []\nrf_i = 0\ntry\n  while rf_i < 1500\n    rf_c = [rf_c]\n    append(&rf_t[0], rf_c)\n    rf_i += 1\n  end\n  print(\"no error\")\ncatch e\n  print(\"too deep\")\nend\n"
          "rf_v = []\nrf_j = 0\nwhile rf_j < 997\n  rf_v = [rf_v]\n  rf_j += 1\nend\n"
          "rf_w = [[[]]]\ntry\n  append(&rf_w[0][0], rf_v)\n  print(\"no error\")\ncatch e\n  print(\"too deep\")\nend\n"
          "print(len(rf_w[0][0]))\nrf_z = []\nappend(&rf_z, rf_v)\nprint(len(rf_z))\n",
          "too deep\ntoo deep\n0\n1\n" },

        { "in-place ops: errors are the same as before and leave the variable alone; a user-defined push or str still works",
          "fn lh(v) return v end local arr<any> ip3_a = [1, 2]\n"
          "try\n  ip3_a[5] = 1\n  print(\"unreachable\")\ncatch e\n  print(\"range\")\nend\n"
          "try\n  zzz[0] = 1\n  print(\"unreachable\")\ncatch e\n  print(\"undefined\")\nend\n"
          "ip3_n = lh(5)\ntry\n  ip3_n[0] = 1\n  print(\"unreachable\")\ncatch e\n  print(\"not indexable\")\nend\n"
          "print(ip3_a)\n"
          "fn push(ip3_x, ip3_y)\n  return [\"mine\"]\nend\n"
          "ip3_a = push(ip3_a, 3)\nprint(ip3_a)\n"
          "ip3_m = {\"k1\": 1}\nfn str(ip3_x)\n  ip3_m = {\"k2\": 42}\n  return \"1\"\nend\n"
          "print(ip3_m[\"k\" + str(1)])\n",
          "range\nundefined\nnot indexable\n[1, 2]\n[\"mine\"]\n1\n" },

        { "copy on write: a copy of a string, array or map never sees later writes to the original, or the other way round",
          "cw_a = [1, 2, 3]\ncw_b = cw_a\ncw_b[0] = 99\nprint(cw_a)\nprint(cw_b)\n"
          "cw_m = {\"k\": [1, 2]}\ncw_n = cw_m\ncw_n[\"k\"][0] = 7\nappend(&cw_n[\"k\"], 5)\nprint(cw_m)\nprint(cw_n)\n"
          "fn cw_mut(x)\n  x[0] = 0\n  return x\nend\ncw_c = [1, 2]\ncw_d = cw_mut(cw_c)\nprint(cw_c)\nprint(cw_d)\n"
          "cw_s = \"ab\"\ncw_t = cw_s\ncw_t[0] = \"z\"\ncw_s = cw_s + \"c\"\nprint(cw_s)\nprint(cw_t)\n"
          "cw_i = [1]\ncw_o = [cw_i, cw_i]\ncw_o[0][0] = 5\nprint(cw_i)\nprint(cw_o)\n"
          "cw_q = [1, 2, 3]\nfor cw_x in cw_q\n  cw_q[1] = 100\n  print(cw_x)\nend\nprint(cw_q)\n",
          "[1, 2, 3]\n[99, 2, 3]\n{k: [1, 2]}\n{k: [7, 2, 5]}\n[1, 2]\n[0, 2]\nabc\nzb\n[1]\n[[5], [1]]\n1\n2\n3\n[1, 100, 3]\n" },

        { "try/catch: an error caught outside a for loop drops that loop's iterator, so the enclosing loop carries on",
          "tc_n = 0\nfor tc_i in [1, 2, 3]\n  try\n    for tc_j in [10, 20]\n      error(\"x\")\n    end\n  catch tc_e\n    tc_n += 1\n  end\n  print(tc_i)\nend\nprint(tc_n)\n"
          "fn tc_f()\n  for tc_k in [1, 2]\n    try\n      for tc_m in [1]\n        error(\"y\")\n      end\n    catch tc_e2\n      print(\"c\")\n    end\n  end\nend\ntc_f()\n",
          "1\n2\n3\n3\nc\nc\n" },

        { "import: an import statement leaves nothing on the stack (inside a loop it must not pile up)",
          "fn im_f()\n  im_i = 0\n  while im_i < 3\n    import \"embrlib\"\n    im_i += 1\n  end\n  return im_i\nend\nprint(im_f())\n"
          "try\n  import \"embrlib\"\ncatch im_e\n  print(\"no\")\nend\nprint(\"ok\")\n",
          "3\nok\n" },

        // fused instructions (see quickenChunk in vm.h): each one has to do what the plain instructions it stands in for do
        { "fused: int loops in a function and at the top level count and add the same as plain code",
          "fn fu_f(n)\n  i = 0\n  t = 0\n  while i < n\n    t = t + i % 7\n    i = i + 1\n  end\n  return t\nend\nprint(fu_f(100))\n"
          "fu_i = 0\nfu_t = 0\nwhile fu_i < 100\n  fu_t = fu_t + fu_i % 7\n  fu_i = fu_i + 1\nend\nprint(fu_t)\n"
          "fu_n = 3\nfu_j = 0\nwhile fu_j < fu_n\n  fu_j = fu_j + 1\nend\nprint(fu_j)\n",
          "295\n295\n3\n" },

        { "fused: an int that overflows widens to a float, and an int next to a float compares as numbers",
          "fn fu_o()\n  x = 9223372036854775807\n  y = x + 1\n  return y > 9000000000000000000\nend\nprint(fu_o())\n"
          "fn fu_c(a)\n  b = 2.5\n  if a < b\n    return 1\n  end\n  return 0\nend\nprint(fu_c(2))\nprint(fu_c(3))\n"
          "fn fu_m(a, b)\n  return a * b\nend\nprint(fu_m(3037000500, 3037000500) > 9000000000000000000)\n",
          "1\n1\n0\n1\n" },

        { "fused: a modulo by zero, a name that is not defined yet and a string operand still raise their errors",
          "fn fu_z(a, b)\n  return a % b\nend\ntry\n  fu_z(5, 0)\ncatch e\n  print(e[\"message\"])\nend\n"
          "fn fu_u(a)\n  if a > 0\n    k = 1\n  end\n  return k + a\nend\ntry\n  fu_u(0)\ncatch e\n  print(\"undefined\")\nend\nprint(fu_u(2))\n"
          "fn fu_s(a, b)\n  return a + b\nend\nprint(fu_s(\"x\", 1))\n",
          "modulo by zero\nundefined\n3\nx1\n" },

        { "fused: a store into a variable declared num, float or int keeps its type rules",
          "fn fu_t()\n  local num t = 0\n  i = 0\n  while i < 3\n    t = t + i\n    i = i + 1\n  end\n  t = t + 0.5\n  return t\nend\nprint(fu_t())\n"
          "fn fu_w()\n  local int w = 1\n  try\n    w = w + 0.5\n  catch e\n    print(\"caught\")\n  end\n  return w\nend\nprint(fu_w())\n",
          "3.5\ncaught\n1\n" },

        // ranges: [a...b] and [a, b...c] are arrays that are not built until something needs the list
        { "range: [a...b] includes both ends, [a, b...c] steps by b - a, and the wrong direction is empty",
          "print([0...5])\nprint([0, 2...10])\nprint([10, 8...0])\nprint([5...0])\nprint([3...3])\nprint([0, 3...10])\n"
          "n = 2\nprint([n...n + 2])\nprint(len([1...4]))\nprint([1...4] == [1, 2, 3, 4])\n",
          "[0, 1, 2, 3, 4, 5]\n[0, 2, 4, 6, 8, 10]\n[10, 8, 6, 4, 2, 0]\n[]\n[3]\n[0, 3, 6, 9]\n[2, 3, 4]\n4\n1\n" },

        { "range: a huge range costs nothing until it is turned into a list: len, an element, a for loop with a break",
          "r = [0...100000000000]\nprint(len(r))\nprint(r[99999999999])\n"
          "for i in [5, 10...100000000000]\n  print(i)\n  if i > 15\n    break\n  end\nend\n",
          "100000000001\n99999999999\n5\n10\n15\n20\n" },

        { "range: it is a plain array when you change it, and building a huge one is an error you can catch",
          "x = [1...3]\nx[0] = 99\nprint(x)\nimport \"embrlib\"\nx = push(x, 7)\nprint(x)\n"
          "try\n  y = [0...1000000000000]\n  y[0] = 1\n  print(\"unreachable\")\ncatch e\n  print(\"too long\")\nend\n",
          "[99, 2, 3]\n[99, 2, 3, 7]\ntoo long\n" },

        { "range: bounds have to be whole numbers and the step cannot be zero",
          "try\n  print([0.5...3])\ncatch e\n  print(e[\"message\"])\nend\n"
          "try\n  print([2, 2...9])\ncatch e\n  print(e[\"message\"])\nend\n"
          "print([1.0...3.0])\n",
          "the start of a range has to be a whole number, got 0.5\nthe step of a range can't be zero: its first two numbers are equal\n[1, 2, 3]\n" },

        { "for: an array is not copied for the loop, but the loop still sees it as it was when it started",
          "import \"embrlib\"\na = [1, 2, 3]\nfor x in a\n  a = push(a, x * 10)\nend\nprint(a)\n"
          "b = [1, 2, 3]\nfor x in b\n  b[2] = 0\n  print(x)\nend\nprint(b)\n",
          "[1, 2, 3, 10, 20, 30]\n1\n2\n3\n[1, 2, 0]\n" },

        { "for: each pass over a range is a new variable that a closure keeps",
          "fns = []\nimport \"embrlib\"\nfor i in [1...3]\n  fns = push(fns, fn() return i end)\nend\n"
          "print(fns[0]())\nprint(fns[1]())\nprint(fns[2]())\n",
          "1\n2\n3\n" },

        // overload groups (`name__N`)
        { "overload: a bare name picks the member by the argument types, a numbered name is always that member",
          "fn ov_show__1(x: int)\n  return \"int \" + str(x)\nend\n"
          "fn ov_show__2(x: str)\n  return \"str \" + x\nend\n"
          "fn ov_show__3(x: num, y: num)\n  return \"two\"\nend\n"
          "print(ov_show(5))\nprint(ov_show(\"a\"))\nprint(ov_show(1, 2.5))\nprint(ov_show__2(\"direct\"))\n"
          "f = ov_show\nprint(f(9))\n",
          "int 5\nstr a\ntwo\nstr direct\nint 9\n" },

        { "overload: when the types are only known at run time the dispatcher chooses, and says what it could have taken",
          "fn ov_d__1(x: int)\n  return \"int\"\nend\nfn ov_d__2(x: str)\n  return \"str\"\nend\n"
          "fn ov_pass(v)\n  return ov_d(v)\nend\nprint(ov_pass(1))\nprint(ov_pass(\"s\"))\n"
          "try\n  ov_pass([1])\ncatch e\n  print(e[\"message\"])\nend\n",
          "int\nstr\nno overload of 'ov_d' takes (array); candidates: ov_d__1(int), ov_d__2(string)\n" },

        { "overload: a new member added later joins the group and changes nothing for a call of an older one",
          "fn ov_l__1(x: int)\n  return 1\nend\nprint(ov_l(2))\nprint(ov_l__1(2))\n"
          "fn ov_l__2(x: str)\n  return 2\nend\nprint(ov_l(\"s\"))\nprint(ov_l__1(3))\n",
          "1\n1\n2\n1\n" },

        { "types: a variable keeps the type of its first value, and a value of another type is refused while running",
          "fn lh(v) return v end ty_x = 1\ntry\n  ty_x = lh(\"s\")\n  print(\"unreachable\")\ncatch ty_e\n  print(ty_e[\"message\"])\nend\nprint(ty_x)\n"
          "fn ty_f()\n  ty_v = 1.5\n  try\n    ty_v = lh(2)\n    print(\"unreachable\")\n  catch ty_e2\n    print(\"caught\")\n  end\n  return ty_v\nend\nprint(ty_f())\n",
          "cannot assign string to 'ty_x' declared as int\n1\ncaught\n1.5\n" },

        { "types: a first value the checker can't see still fixes the type",
          "fn lh(v) return v end ty_u = lh(7)\ntry\n  ty_u = lh(\"s\")\n  print(\"unreachable\")\ncatch ty_e\n  print(\"caught\")\nend\nty_u = lh(9)\nprint(ty_u)\n"
          "fn ty_g()\n  local ty_l = lh(1.5)\n  try\n    ty_l = lh(2)\n    print(\"unreachable\")\n  catch ty_e2\n    print(\"caught\")\n  end\n  return ty_l\nend\nprint(ty_g())\n",
          "caught\n9\ncaught\n1.5\n" },

        { "types: an int that overflows can't go back into an int variable",
          "ty_big = 9223372036854775807\ntry\n  ty_big = ty_big + 1\n  print(\"unreachable\")\ncatch ty_e\n  print(\"caught\")\nend\nprint(type(ty_big + 1))\n",
          "caught\nnumber\n" },

        { "types: declared any and declared num are the ways to hold more than one type",
          "local any ty_a = 1\nty_a = \"s\"\nty_a = [1]\nprint(ty_a)\n"
          "local num ty_n = 1\nty_n = 2.5\nty_n = 3\nprint(ty_n)\nprint(type(ty_n))\n"
          "fn ty_h(p: any)\n  p = \"changed\"\n  return p\nend\nprint(ty_h(1))\n",
          "[1]\n3\nint\nchanged\n" },

        { "types: a store carries the variable's type, and a typed parameter is checked when it is assigned",
          "fn lh(v) return v end fn ty_p(a: int)\n  try\n    a = lh(\"s\")\n    print(\"unreachable\")\n  catch ty_e\n    print(\"caught\")\n  end\n  a = 5\n  return a\nend\nprint(ty_p(1))\n",
          "caught\n5\n" },

        { "element types: a typed container runs like an untyped one",
          "local arr<int> et_a = []\nappend(&et_a, 1)\nappend(&et_a, 2)\nprint(et_a)\n"
          "local map<str> et_m = {\"a\": \"x\"}\nprint(et_m[\"a\"])\n"
          "fn et_f(xs: arr<int>) -> arr<int>\n  return push(xs, 3)\nend\nprint(et_f(et_a))\n"
          "fn et_g(...n: int)\n  return len(n)\nend\nprint(et_g(1, 2, 3))\n"
          "local arr<arr<str>> et_n = [[\"a\"], []]\nprint(len(et_n))\n"
          "et_r = [0...3]\net_r[0] = 5\nprint(et_r)\n",
          "[1, 2]\nx\n[1, 2, 3]\n3\n2\n[5, 1, 2, 3]\n" },

        { "calls: a function named by a local, a captured variable or a global is called without a copy of it",
          "fn ct_apply(f, x)\n  return f(x)\nend\n"
          "print(ct_apply(fn(v) return v + 1 end, 4))\nprint(ct_apply(len, [1, 2, 3]))\n"
          "fn ct_outer(g)\n  h = fn(x) return g(x) * 2 end\n  return h(5)\nend\nprint(ct_outer(fn(v) return v + 1 end))\n"
          "fn ct_mk()\n  n = 0\n  return fn()\n    n = n + 1\n    return n\n  end\nend\nct_c = ct_mk()\nprint(ct_c(), ct_c(), ct_c())\n"
          "ct_a = []\nct_a = push(ct_a, 1)\nct_a = push(ct_a, 2)\nprint(ct_a)\n"
          "ct_g = fn(a, b) return a end\ntry\n  ct_g(1)\n  print(\"unreachable\")\ncatch e\n  print(\"too few\")\nend\n"
          "local any ct_n = 5\ntry\n  ct_n(1)\n  print(\"unreachable\")\ncatch e\n  print(\"not callable\")\nend\n"
          "fn ct_rec(n)\n  if n < 1\n    return 0\n  end\n  return n + ct_rec(n - 1)\nend\nprint(ct_rec(10))\n",
          "5\n3\n12\n1 2 3\n[1, 2]\ntoo few\nnot callable\n55\n" },

        { "calls: a function replaced while its arguments are worked out is called as it is then",
          "ct_f = fn(x) return x end\nfn ct_swap()\n  ct_f = fn(x) return 100 end\n  return 1\nend\nprint(ct_f(ct_swap()))\n"
          "fn ct_loc(f)\n  r = f(1)\n  return r\nend\nprint(ct_loc(fn(x) return x + 41 end))\n",
          "100\n42\n" },

        { "inline: a call of a `name(x) = expr` function is its expression, with each argument worked out once and in order",
          "il_sq(x) = x * x\n"
          "il_hyp(a, b) = il_sq(a) + il_sq(b)\n"
          "local il_n = 0\nfn il_bump()\n  il_n += 1\n  return il_n\nend\n"
          "print(il_sq(7), il_sq(2.5), il_hyp(3, 4))\n"
          "print(il_sq(il_bump()), il_n)\n"
          "print(il_hyp(il_bump(), il_bump()), il_n)\n"
          "local il_x = 10\nprint(il_sq(il_x), il_x)\n"
          "fn il_use(v) return il_sq(v + 1) end\nprint(il_use(4))\n"
          "for il_i in [1, 2, 3]\n  print(il_sq(il_i))\nend\n"
          "il_first(a) = a[0]\nlocal il_xs = [7, 8]\nprint(il_first(il_xs), il_first([5, 6]))\n"
          "local il_p = 3\nprint(il_hyp(il_p, il_p + 1), il_hyp(il_p, 2))\n"
          "local il_f = il_sq\nprint(il_f(9))\n",
          "49 6.25 25\n1 1\n13 3\n100 10\n25\n1\n4\n9\n7 5\n25 13\n81\n" },

        { "inline: a call stays an ordinary one when the name could mean something else there",
          "local il_k = 3\nil_addk(v) = v + il_k\n"
          "fn il_shadow(il_k) return il_addk(1) end\nprint(il_shadow(100), il_addk(1))\n"
          "il_o(x) = x + 1\nprint(il_o(1))\nil_o = fn(x) return x + 100 end\nprint(il_o(1))\n"
          "il_rec(n) = n <= 0 and 100 or n + il_rec(n - 1)\nprint(il_rec(4))\n"
          "il_two() = 2\nprint(il_two() + il_two())\n"
          "fn il_ret(x) -> int\n  return x\nend\nil_big(x) = x\nprint(il_big(5))\n",
          "4 4\n2\n101\n110\n4\n5\n" },

        { "inline: a typed parameter is still checked",
          "il_t(n: int) = n * 2\nprint(il_t(4))\n"
          "local any il_bad = \"a\"\ntry\n  print(il_t(il_bad))\ncatch e\n  print(e.message)\nend\n",
          "8\ncannot assign string to 'n' declared as int\n" },

        { "const: a constant is worked out when the script is compiled, and is still a variable",
          "const cn_n = 10\nconst cn_s = \"a\" + \"b\"\nconst cn_x = cn_n * 2.5 + 1\nconst cn_f = cn_n > 5 and 7\n"
          "const cn_i = 7 / 2\nconst cn_e = 3 != 3\n"
          "print(cn_n, cn_s, cn_x, cn_f, cn_i, cn_e)\nprint(9223372036854775807 + 1 > 9000000000000000000)\n"
          "fn cn_f2(v) return v + cn_n * 2 end\nprint(cn_f2(1))\n"
          "local cn_g = cn_n\nprint(cn_g, 2 * 3 + 4, -(2 + 3), !0, 10 % 4, 7 - 10)\n"
          "try\n  print(1 / 0)\ncatch e\n  print(\"div\")\nend\n",
          "10 ab 26 7 3.5 0\n1\n21\n10 10 -5 1 2 -3\ndiv\n" },

        { "const: a pure native (len, str, type, num, ...) called with constant arguments is worked out when compiling",
          "const cn_len = len(\"abcd\")\nconst cn_mix = cn_len * 2 + len(str(123))\nconst cn_t = type(1.5) + \":\" + str(num(\"4\") + cn_len)\n"
          "print(cn_len, cn_mix, cn_t)\nprint(type(cn_len))\n",
          "4 11 number:8\nint\n" },

        { "scoping in a hot loop: a function's first store assigns an existing global, else makes its own local, typed by that value",
          "fs_g = 0\nfn fs_set(v)\n  fs_g = v\n  fs_tmp = v * 2\n  return fs_tmp\nend\n"
          "fn fs_closure()\n  n = 0\n  return fn()\n    n = n + 1\n    s = \"x\"\n    s = s + str(n)\n    return s\n  end\nend\n"
          "i = 0\nwhile i < 3000\n  fs_set(i)\n  i = i + 1\nend\nprint(fs_g, has(globals(), \"fs_tmp\"))\n"
          "c = fs_closure()\nj = 0\nr = \"\"\nwhile j < 2000\n  r = c()\n  j = j + 1\nend\nprint(r)\n",
          "2999 0\nx2000\n" },

        { "const: a name that a function or block binds again is not folded",
          "const cn_k = 3\nfn cn_shadow(cn_k) return cn_k + 1 end\nprint(cn_shadow(10), cn_k)\n"
          "for cn_k in [1]\n  print(cn_k)\nend\n",
          "11 3\n1\n" },

        { "const fn: a function with statements is written out at its calls",
          "const fn cn_clamp(v, lo, hi)\n  if v < lo\n    return lo\n  elif v > hi\n    return hi\n  end\n  return v\nend\n"
          "const fn cn_find(xs, want)\n  for i in xs\n    for j in [0...2]\n      if i == want\n        return j + 100\n      end\n    end\n  end\n  return -1\nend\n"
          "const fn cn_acc(n)\n  local out = []\n  local k = 0\n  while k < n\n    k += 1\n    if k % 2 == 0\n      continue\n    end\n    out = push(out, k * k)\n    if k > 7\n      break\n    end\n  end\n  return out\nend\n"
          "const fn cn_none(x)\n  local y = x\nend\n"
          "local cn_total = 100\nconst fn cn_sum(n)\n  local cn_total = 0\n  for i in [1...n]\n    cn_total += i\n  end\n  return cn_total\nend\n"
          "print(cn_clamp(15, 0, 10), cn_clamp(-3, 0, 10), cn_clamp(5, 0, 10), cn_clamp(cn_clamp(20, 0, 9), 2, 5))\n"
          "print(cn_find([3, 4, 5], 4), cn_find([3], 9), cn_acc(12), cn_none(5))\n"
          "print(cn_sum(10), cn_total)\n"
          "cn_p = cn_clamp\nprint(cn_p(50, 0, 10))\n"
          "fn cn_inloop()\n  local s = 0\n  local i = 0\n  while i < 5\n    s += cn_clamp(i, 1, 3)\n    i += 1\n  end\n  return s\nend\nprint(cn_inloop())\n"
          "const fn cn_w(a, b)\n  a += 1\n  return a * b\nend\nlocal cn_x = 3\nprint(cn_w(cn_x, 2), cn_x, cn_clamp(cn_x, 0, 2), cn_clamp(cn_x + 1, cn_x, 9))\n"
          "local cn_cnt = 0\nfn cn_bump()\n  cn_cnt += 1\n  return cn_cnt\nend\n"
          "const fn cn_imp(v)\n  cn_bump()\n  return v\nend\nprint(cn_imp(cn_cnt), cn_cnt)\n",
          "10 0 5 5\n100 -1 [1, 9, 25, 49, 81] 0\n55 100\n10\n10\n8 3 2 4\n0 1\n" },

        { "natives: a call that does not fit the signature gives the same messages as before",
          "local any ns_f = len\nlocal any ns_j = join\nlocal any ns_bad = \"abc\"\nlocal any ns_l = [1]\n"
          "try\n  ns_f()\ncatch e\n  print(e.message)\nend\n"
          "try\n  ns_f(1, 2)\ncatch e\n  print(e.message)\nend\n"
          "try\n  ns_f(&ns_l)\ncatch e\n  print(e.message)\nend\n"
          "try\n  ns_j(ns_bad, 1)\ncatch e\n  print(e.message)\nend\n"
          "print(ns_f(ns_bad), ns_f(ns_l))\n",
          "too few arguments to 'len': expected 1 but got 0\ntoo many arguments to 'len': expected at most 1 but got 2\n"
          "argument 'val' to 'len' is not an in-out parameter, so it can't take '&'\nargument 'arr' to 'join': expected array but got string\n3 1\n" },

        { "scoping: a block without a local or fn has no scope, one with them still erases them; a plain assignment makes a variable of the function",
          "fn sc_f()\n  local total = 0\n  local i = 0\n  while i < 3\n    total = total + i\n    if total > 0\n      total = total + 1\n    end\n    i += 1\n  end\n  return total\nend\nprint(sc_f())\n"
          "fn sc_g()\n  local k = 0\n  while k < 2\n    sc_fresh = k * 10\n    k += 1\n  end\n  try\n    print(sc_fresh)\n  catch e\n    print(\"gone\")\n  end\nend\nsc_g()\n"
          "fn sc_h()\n  if 1\n    sc_inner = 5\n  end\n  local j = 0\n  while j < 1\n    sc_inner = 7\n    j += 1\n  end\n  try\n    print(sc_inner)\n  catch e\n    print(\"erased\")\n  end\nend\nsc_h()\n"
          "fn sc_i()\n  local s = 0\n  local n = 0\n  while n < 10\n    n += 1\n    if n % 2 == 0\n      continue\n    end\n    if n > 7\n      break\n    end\n    s += n\n  end\n  return s\nend\nprint(sc_i())\n"
          "sc_t = 0\nsc_n = 0\nwhile sc_n < 4\n  sc_t = sc_t + sc_n\n  sc_n += 1\nend\nprint(sc_t)\n"
          "fn sc_l()\n  local i = 0\n  while i < 2\n    local sc_inner2 = i\n    i += 1\n  end\n  try\n    print(sc_inner2)\n  catch e\n    print(\"local gone\")\n  end\nend\nsc_l()\n"
          "fn sc_c()\n  local fs = []\n  local i = 0\n  while i < 3\n    local k = i * 2\n    fs = push(fs, fn() return k end)\n    i += 1\n  end\n  return fs[0]() + fs[1]() + fs[2]()\nend\nprint(sc_c())\n",
          "5\n10\n7\n16\n6\nlocal gone\n6\n" },

        { "natives: typed bindings in embrlib (keys, values, sum, index_of, type, len, str) refuse a wrong type and give the same answers",
          "local any tn_n = 5\nlocal any tn_s = \"abc\"\nlocal any tn_m = {\"b\": 2, \"a\": 1}\n"
          "try\n  keys(tn_s)\ncatch e\n  print(e.message)\nend\n"
          "try\n  sum([1, tn_s])\ncatch e\n  print(e.message)\nend\n"
          "try\n  index_of(tn_n, 1)\ncatch e\n  print(e.message)\nend\n"
          "try\n  len(tn_n)\ncatch e\n  print(e.message)\nend\n"
          "print(keys(tn_m), values(tn_m), sum([1, 2, 3]), sum([1, 2.5]), index_of([4, 5, 6], 5), index_of([1], \"1\"))\n"
          "print(type(tn_n), type(tn_s), type(tn_m), len(tn_m), len(tn_s), len([1, 2, 3]), str(12), str([1]))\n",
          "argument 'm' to 'keys': expected map but got string\nelement 1 is string, expected a number\n"
          "argument 'arr' to 'index_of': expected array but got int\n"
          "no __<tag>_len defined and val is not a str/arr/map (got int)\n"
          "[\"a\", \"b\"] [1, 2] 6 3.5 1 -1\nint string map 2 3 3 12 [1]\n" },

        { "scoping: a function called from native code keeps its variables to itself (they must not become globals)",
          "fn ty_cb(v)\n  ty_leaked = v\n  return 0\nend\n"
          "sort_cmp([2, 1], fn(a, b) ty_cb(a)\n return a - b end)\n"
          "try\n  print(ty_leaked)\ncatch ty_e\n  print(\"not a global\")\nend\n",
          "not a global\n" },
    };
    return embr_test::runSuite("embr core test suite", tests, {"embrlib"});
}
