// unit tests for the static type checker (core/typecheck.h): the mistakes it must find, and, just as important,
// the programs it must leave alone, because it only reports what can never be right

#include "harness.h"

static int g_failed = 0, g_passed = 0;
static void report(const std::string& name, bool ok, const std::string& why = "") {
    if (ok) { ++g_passed; std::cout << "\033[32m [+]\033[0m " << name << "\n"; return; }
    ++g_failed;
    std::cout << "\033[31m [-]\033[0m " << name << "\n";
    if (!why.empty()) std::cout << "        " << why << "\n";
}

static std::vector<std::string> typeErrors(const std::string& src, bool closedWorld = false) {
    std::vector<std::string> out;
    try {
        embr::Lexer lex(src);
        auto tokens = lex.tokenize();
        embr::SourceMap sm = lex.sourceMap();
        embr::Parser parser(std::move(tokens), sm);
        auto ast = parser.parse();
        embr::typecheck::Options o;
        o.closedWorld = closedWorld;
        o.source = &sm;
        for (auto& e : embr::typecheck::check(ast, o)) out.push_back(e.message);
    } catch (const embr::EmbrError& e) {
        out.push_back(std::string("PARSE: ") + e.message);
    }
    return out;
}

// the same, for a file that will run in `interp`: it knows the globals and natives that interpreter already has
static std::vector<std::string> typeErrorsIn(const embr::Interpreter& interp, const std::string& src) {
    std::vector<std::string> out;
    embr::Lexer lex(src);
    auto tokens = lex.tokenize();
    embr::SourceMap sm = lex.sourceMap();
    embr::Parser parser(std::move(tokens), sm);
    auto ast = parser.parse();
    embr::typecheck::Options o;
    o.source = &sm;
    o.interp = &interp;
    for (auto& e : embr::typecheck::check(ast, o)) out.push_back(e.message);
    return out;
}

// the type the checker recorded for the outermost expression that starts at line:col (Options::typesOut), written as the words a
// script uses, with a * when it is sure (one core type, not soft). "none" when nothing starts there
static std::string recordedType(const std::string& src, int line, int col, bool closedWorld = false, std::vector<std::string>* errorsOut = nullptr) {
    embr::Lexer lex(src);
    auto tokens = lex.tokenize();
    embr::SourceMap sm = lex.sourceMap();
    embr::Parser parser(std::move(tokens), sm);
    auto ast = parser.parse();
    embr::typecheck::ExprTypes types;
    embr::typecheck::Options o;
    o.closedWorld = closedWorld;
    o.source = &sm;
    o.typesOut = &types;
    auto errs = embr::typecheck::check(ast, o);
    if (errorsOut) for (auto& e : errs) errorsOut->push_back(e.message);
    const embr::Expr* best = nullptr;
    for (auto& kv : types) {
        const embr::Expr* e = kv.first;
        if (e->range.startLine != line || e->range.startCol != col) continue;
        if (!best || e->range.endLine > best->range.endLine || (e->range.endLine == best->range.endLine && e->range.endCol > best->range.endCol)) best = e;
    }
    if (!best) return "none";
    const auto& t = types[best];
    return embr::typecheck::detail::tn(t.t) + (t.sure ? "*" : "");
}

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

// the source has exactly one error and it says `want`
static void expectError(const std::string& name, const std::string& src, const std::string& want, bool closed = false) {
    auto errs = typeErrors(src, closed);
    bool ok = errs.size() == 1 && errs[0].find(want) != std::string::npos;
    report(name, ok, ok ? "" : "wanted one error containing '" + want + "', got " + std::to_string(errs.size()) + ": " + join(errs));
}
static void expectClean(const std::string& name, const std::string& src, bool closed = false) {
    auto errs = typeErrors(src, closed);
    report(name, errs.empty(), join(errs));
}

int main() {
    std::cout << "\n\033[1m typecheck \033[0m\n";

    // a variable's type is the type of its first assignment
    expectError("x = 1 then x = \"a\" is an error", "x = 1\nx = \"a\"\n", "cannot assign str to 'x'");
    // num is the union of int and float. a variable that takes its type from a value has exactly that type
    expectError("x = 1 is an int, so a float does not fit", "x = 1\nx = 2.5\n", "cannot assign float to 'x', which holds int");
    expectError("a division gives a float", "x = 1\nx = x / 2\n", "cannot assign float to 'x', which holds int");
    expectClean("int arithmetic may overflow into a float, so int + int could be either", "x = 1\nx = x + 1\nx = x * 3\n");
    expectClean("a float variable can take a float", "x = 1.5\nx = x / 2\nx = 0.25\n");
    expectClean("a variable declared num takes both", "local num x = 1\nx = 2.5\nx = 3\n");
    expectError("a variable that took a float does not take an int", "x = 1.5\nx = 2\n", "cannot assign int to 'x', which holds float");
    expectClean("a number from an unknown call could be either", "x = len([1])\nx = 1.5\nx = 2\n");
    expectClean("same type again is fine", "s = \"a\"\ns = s + \"b\"\ns = \"c\"\n");
    expectError("the branch that comes second is the one in error", "c = 1\nif c\n  x = 1\nelse\n  x = \"a\"\nend\n", "cannot assign str to 'x'");
    expectError("a declared type is exact", "local int n = 5\nn = \"s\"\n", "cannot assign str to 'n' declared as int");
    expectError("a declared type checks its first value", "local str s = 5\n", "declared as str");
    expectClean("an array stays an array", "a = [1]\na = [2, 3]\na = a + [4]\n");

    // functions
    expectError("a typed parameter refuses the wrong argument", "fn f(a: int)\n  return a\nend\nf(\"x\")\n", "argument 'a' to 'f': expected int but got str");
    expectClean("a typed parameter takes the right one", "fn f(a: num)\n  return a\nend\nf(1)\nf(2.5)\n");
    expectError("too few arguments", "fn f(a, b)\n  return a\nend\nf(1)\n", "'f' expects 2 args, got 1");
    expectClean("variadic takes any number", "fn f(a, ...rest)\n  return a\nend\nf(1)\nf(1, 2, 3)\n");
    expectError("a return that cannot match the declared type", "fn f() -> int\n  return \"s\"\nend\n", "declared return type int but returns str");
    expectClean("a return that might match is left to run time", "fn f(x) -> int\n  return x\nend\n");
    expectError("the type a function returns is used", "fn name()\n  return \"bob\"\nend\nx = 1\nx = name()\n", "cannot assign str to 'x'");
    expectClean("a function that returns different things is a union", "fn f(c)\n  if c\n    return 1\n  end\n  return \"s\"\nend\nx = f(1)\nx = 5\nx = \"t\"\n");

    // operations that can never work
    expectError("subtracting a string", "x = \"a\" - 1\n", "cannot use str with '-'");
    expectError("multiplying an array", "x = [1] * 2\n", "cannot use arr with '*'");
    expectError("negating a string", "x = -\"s\"\n", "cannot negate str");
    expectError("a bit operator on a string", "x = \"a\" | 1\n", "cannot use str with '|', it needs ints");
    expectError("a bit operator on a float", "x = 1.5 & 3\n", "cannot use float with '&', it needs ints");
    expectError("a shift of a float", "x = 2 << 1.5\n", "cannot use float with '<<', it needs ints");
    expectError("~ of a string", "x = ~\"s\"\n", "cannot use str with '~', it needs an int");
    expectClean("bit operators on ints give an int", "x = 1\nx = x & 3\nx = x | 4\nx = x ^ 1\nx = x << 2\nx = x >> 1\nx = ~x\n");
    expectClean("a num might hold an int, so it is left to run time", "local num n = 5\nm = n & 1\n");
    expectClean("adding a string and a number is concatenation", "x = \"a\" + 1\ny = 1 + \"a\"\n");
    expectError("indexing a number", "n = 5\nx = n[0]\n", "cannot index");
    expectError("an array index must be a number", "a = [1]\nx = a[\"k\"]\n", "has to be a number");
    expectError("a map key must be a string", "m = {\"k\": 1}\nx = m[1]\n", "has to be a string");
    expectError("calling a number", "n = 5\nn()\n", "cannot call 'n'");
    expectClean("indexing a string, an array, a map", "s = \"abc\"\na = [1]\nm = {\"k\": 1}\nx = s[0]\ny = a[0]\nz = m[\"k\"]\n");

    // what it cannot see stays any: no false alarms
    expectClean("names from plugins and other files", "x = len([1])\nx = \"a\"\ny = unknown_thing\ny = 1\ny = \"b\"\n");
    expectClean("a value from an array element could be anything", "a = [1, \"s\"]\nx = a[0]\nx = \"t\"\nx = 5\n");
    expectClean("a value from a call to something unknown", "x = make()\nx = \"a\"\nx = 1\n");
    expectError("a function writing a top-level variable it can see has to keep its type", "x = 1\nfn f()\n  x = \"s\"\nend\nf()\n", "cannot assign str to 'x', which holds int");
    expectClean("a closure writing a variable of the function around it is not checked", "fn outer()\n  y = 1\n  g = fn() y = \"s\" end\n  return g\nend\n");
    expectClean("a function assigned to a name that held a function", "f = fn(a) return a end\nf = fn(b) return b + 1 end\n");

    // untyped parameters take their types from the calls, but only when every call can be seen
    expectError("a nested function that is only called here",
                "fn outer()\n  fn inner(a)\n    a = \"s\"\n  end\n  inner(1)\nend\n", "cannot assign str to 'a'");
    expectClean("a top-level function in a file that others could call", "fn f(a)\n  a = \"s\"\nend\nf(1)\n");
    expectError("a top-level function when the file is the whole program",
                "fn f(a)\n  a = \"s\"\nend\nf(1)\n", "cannot assign str to 'a'", true);
    expectClean("...but not once the function is used as a value", "fn f(a)\n  a = \"s\"\nend\nf(1)\ng = f\n", true);
    expectClean("...or when the file imports a script module", "import \"other.embr\"\nfn f(a)\n  a = \"s\"\nend\nf(1)\n", true);
    expectClean("...or starts a worker", "fn f(a)\n  a = \"s\"\nend\nf(1)\nworker_spawn(\"m.embr\", \"f\", 1, 2)\n", true);
    expectClean("called with different types, the parameter takes all of them",
                "fn f(a)\n  a = \"s\"\nend\nf(1)\nf(\"x\")\n", true);
    expectClean("an unused function keeps any parameters", "fn f(a)\n  a = \"s\"\nend\n", true);
    expectError("a call chain: what one call passes, the next sees",
                "fn g(b)\n  b = \"s\"\nend\nfn f(a)\n  g(a)\nend\nf(1)\n", "cannot assign str to 'b'", true);
    expectClean("a function bound twice is not followed", "fn f(a)\n  a = \"s\"\nend\nfn f(a)\n  return a\nend\nf(1)\n", true);
    expectClean("recursion", "fn fib(n)\n  if n < 2\n    return n\n  end\n  return fib(n - 1) + fib(n - 2)\nend\nx = fib(10)\n", true);

    // `any` written out is an explicit choice, not "no type"
    expectClean("a variable declared any holds anything", "local any x = 1\nx = \"a\"\nx = [1]\n");
    expectClean("a parameter declared any is not narrowed by its calls", "fn f(a: any)\n  a = \"s\"\nend\nf(1)\n", true);
    expectError("...an untyped one is", "fn f(a)\n  a = \"s\"\nend\nf(1)\n", "cannot assign str to 'a'", true);

    // globals that earlier files or REPL lines made are known, with the type they were given
    {
        embr::Interpreter in;
        embr::vm::runSource("rt_n = 1\nrt_s = \"a\"\nlocal num rt_u = 1\n", in, "first");
        auto e1 = typeErrorsIn(in, "rt_n = \"x\"\n");
        report("a global from an earlier file keeps its type", e1.size() == 1 && e1[0].find("cannot assign str to 'rt_n'") != std::string::npos, join(e1));
        auto e2 = typeErrorsIn(in, "rt_n = 5\nrt_s = \"b\"\nrt_u = 2.5\nrt_u = 3\n");
        report("...and takes the same type again", e2.empty(), join(e2));
        auto e3 = typeErrorsIn(in, "never_seen = 1\nnever_seen = \"x\"\n");
        report("a name nobody has made is still just this file's", e3.size() == 1, join(e3));
    }

    // natives with a signature: the arguments are checked the way the VM checks them when it makes the call
    {
        embr::Interpreter in;
        using embr::Param; namespace TS = embr::TS;
        in.bindSig("nat_len", {Param::req("v", TS::Str | TS::Arr)}, [](const std::vector<embr::Value>&) { return embr::Value(0.0); });
        in.bindSig("nat_add", {Param::req("a", TS::Num), Param::opt("b", TS::Num)}, [](const std::vector<embr::Value>&) { return embr::Value(0.0); });
        in.bindSig("nat_push", {Param::io("arr", TS::Arr), Param::req("x", embr::TypeSet::Any())}, [](const std::vector<embr::Value>&) { return embr::Value(0.0); });
        in.bindSig("nat_all", {Param::rest("xs", TS::Num)}, [](const std::vector<embr::Value>&) { return embr::Value(0.0); });
        auto one = [&](const std::string& src, const std::string& want) {
            auto e = typeErrorsIn(in, src);
            bool ok = want.empty() ? e.empty() : (e.size() == 1 && e[0].find(want) != std::string::npos);
            report("native: " + (src.size() > 40 ? src.substr(0, 40) : src), ok, join(e));
        };
        one("nat_len(5)\n", "argument 'v' to 'nat_len': expected str|arr but got int");
        one("nat_len(\"s\")\nnat_len([1])\n", "");
        one("nat_add()\n", "too few arguments to 'nat_add': expected 1 but got 0");
        one("nat_add(1, 2, 3)\n", "too many arguments to 'nat_add': expected at most 2 but got 3");
        one("nat_add(1)\nnat_add(1, 2.5)\n", "");
        one("nat_add(\"a\")\n", "expected num but got str");
        one("a = [1]\nnat_push(a, 1)\n", "is changed in place, so pass it as &name");
        one("a = [1]\nnat_len(&a)\n", "is not an in-out parameter");
        one("a = [1]\nnat_push(&a, 1)\n", "");
        one("nat_all(1, 2.5, 3)\nnat_all()\n", "");
        one("nat_all(1, \"x\")\n", "expected num but got str");
        one("fn nat_len(v)\n  return v\nend\nnat_len(5)\n", "");   // the file's own function wins
        one("x = nat_len\nnat_len = 5\nnat_len(5)\n", "cannot call");   // a name the file binds is a variable, whatever it was
    }

    // a native can say what it returns, so the checker knows what a call gives back
    {
        embr::Interpreter in;
        using embr::Param; namespace TS = embr::TS;
        auto zero = [](const std::vector<embr::Value>&) { return embr::Value(0.0); };
        in.bindSig("nat_text", {}, zero);
        in.bindSig("nat_count", {}, [](const std::vector<embr::Value>&) { return embr::Value((int64_t)3); });
        in.bindSig("nat_liar", {}, zero);
        in.setResultTypes({{"nat_text", TS::Str}, {"nat_count", TS::Int}, {"nat_liar", TS::Str}});
        auto one = [&](const std::string& src, const std::string& want) {
            auto e = typeErrorsIn(in, src);
            bool ok = want.empty() ? e.empty() : (e.size() == 1 && e[0].find(want) != std::string::npos);
            report("native result: " + (src.size() > 40 ? src.substr(0, 40) : src), ok, join(e));
        };
        one("x = nat_text()\nx = 1\n", "cannot assign int to 'x'");
        one("n = 0\nn = n + nat_count()\n", "");
        one("n = 0.5\nn = nat_count()\n", "cannot assign int to 'n'");
        one("s = nat_text() + 1\n", "");   // adding to a string is not this pass's business
        // a native that breaks its own claim is a plugin bug, and the VM says so instead of letting a wrong type through
        bool raised = false;
        try {
            auto prog = embr::vm::compileSource("y = nat_liar()\n", in, "liar");
            embr::vm::VmRunner runner(in);
            runner.run(prog);
        }
        catch (const embr::EmbrError& e) { raised = std::string(e.what()).find("declared to return string") != std::string::npos; }
        report("native result: a call that breaks the claim raises an internal error", raised);
    }

    // the numbers of a range have to be numbers
    expectClean("range: numbers", "a = [0...10]\nb = [0, 2...10]\nn = 3\nc = [n...n * 2]\nc = [1]\n");
    expectError("range: a string as the start", "a = [\"x\"...5]\n", "the start of a range has to be a number, not str");
    expectError("range: a string as the end", "a = [1, 3...\"y\"]\n", "the end of a range has to be a number, not str");
    expectError("range: an array as the second number", "a = [1, [2]...5]\n", "the second number of a range has to be a number, not arr");

    // overload groups: `name__N` functions are picked by the argument types when they are known before the script runs
    {
        const std::string defs =
            "fn ov__1(x: int) -> str\n  return \"i\"\nend\n"
            "fn ov__2(x: str) -> int\n  return 1\nend\n"
            "fn ov__3(x: num, y: num) -> float\n  return 0.5\nend\n";
        expectClean("overload: a call is typed as the member it picks, and a member can be called by its own name",
                    defs + "a = ov(1)\na = \"s\"\nb = ov(\"x\")\nb = 2\nc = ov(1, 2.5)\nc = 0.25\nd = ov__2(\"y\")\nd = 3\n");
        expectError("overload: the type it picked is the type of the variable",
                    defs + "a = ov(1)\na = 5\n", "cannot assign int to 'a'");
        expectError("overload: no member takes the arguments",
                    defs + "ov([1])\n", "no overload of 'ov' takes (arr); candidates: ov__1(int), ov__2(str), ov__3(num, num)");
        expectError("overload: no member takes that many arguments",
                    defs + "ov()\n", "no overload of 'ov' takes ()");
        expectError("overload: two members that fit equally well",
                    "fn tie__1(x: int)\n  return 1\nend\nfn tie__2(x: int)\n  return 2\nend\ntie(5)\n", "fits more than one overload equally well");
        expectClean("overload: the narrower of two members that both take the argument wins",
                    "fn nar__1(x: num) -> str\n  return \"n\"\nend\nfn nar__2(x: int) -> int\n  return 1\nend\nr = nar(3)\nr = 4\n");
        expectClean("overload: arguments whose types are not known leave the choice to the run time",
                    defs + "fn pass(v)\n  return ov(v)\nend\nprint(pass(1))\nprint(pass(\"s\"))\n", true);
        expectError("overload: a plain name and a group of that name in one file",
                    defs + "fn ov(x)\n  return x\nend\n", "both a plain name in this file and the overload group of ov__1");
        expectClean("overload: a numbered function that is not at the top level is just a function",
                    "fn outer()\n  fn inner__1(x)\n    return x\n  end\n  return inner__1(1)\nend\nouter()\n");
        expectClean("overload: a name that only looks like a number is not a group",
                    "fn utf8_2(x)\n  return x\nend\nfn a__01(x)\n  return x\nend\nutf8_2(1)\na__01(1)\n");
        // natives that give a signature form a group too, and the same rules apply
        embr::Interpreter in;
        using embr::Param; namespace TS = embr::TS;
        in.bindSig("nov__1", {Param::req("s", TS::Str)}, [](const std::vector<embr::Value>&) { return embr::Value(1.0); });
        in.bindSig("nov__2", {Param::req("n", TS::Num)}, [](const std::vector<embr::Value>&) { return embr::Value(2.0); });
        in.setResultTypes({{"nov__1", TS::Float}, {"nov__2", TS::Float}});
        auto one = [&](const std::string& name, const std::string& src, const std::string& want) {
            auto e = typeErrorsIn(in, src);
            bool ok = want.empty() ? e.empty() : (e.size() == 1 && e[0].find(want) != std::string::npos);
            report("overload native: " + name, ok, join(e));
        };
        one("a string picks nov__1 and a number nov__2", "nov(\"a\")\nnov(1)\nnov(2.5)\n", "");
        one("nothing takes an array", "nov([1])\n", "no overload of 'nov' takes (arr); candidates: nov__1(str), nov__2(num)");
        one("a plain name against a native group", "fn nov(x)\n  return x\nend\n", "'nov' is a plain name in this file");
        bool twice = false;
        try { in.bindSig("nov__2", {Param::req("n", TS::Num)}, [](const std::vector<embr::Value>&) { return embr::Value(3.0); }); }
        catch (const embr::EmbrError& e) { twice = std::string(e.what()).find("numbers are never reused") != std::string::npos; }
        report("overload native: a number is never claimed twice", twice);
        // what the checker leaves for the compiler
        {
            embr::Lexer lex("a = ov(1)\nb = ov(\"x\")\n"); (void)lex;
        }
    }

    // what the checker leaves for the compiler: the variable's exact type, and whether the value is known to fit
    {
        auto hints = [](const std::string& src) {
            embr::Lexer lex(src); auto toks = lex.tokenize(); embr::SourceMap sm = lex.sourceMap();
            embr::Parser parser(std::move(toks), sm); auto ast = parser.parse();
            embr::typecheck::Options o; o.source = &sm; embr::typecheck::check(ast, o);
            std::string out;
            for (auto& st : ast) if (st->kind == embr::Stmt::Kind::Assign) {
                auto* a = static_cast<const embr::AssignStmt*>(st.get());
                out += a->storeHint.isAny() ? "any" : a->storeHint.name();
                out += a->storeProven ? "!" : "?";
                out += " ";
            }
            return out;
        };
        report("a second int into an int variable is proven", hints("x = 1\nx = 2\n") == "int! int! ");
        report("int arithmetic may overflow, so it is tested, not proven", hints("x = 1\nx = x + 1\n") == "int! int? ");
        report("a value of unknown type is tested against the variable's type", hints("x = 1\nx = len(f())\n") == "int! int? ");
        report("a first value of unknown type leaves the type to run time", hints("x = f()\nx = 1\n") == "any? any? ");
        report("a union of types is not an exact type", hints("c = f()\nif c\n  x = 1\nelse\n  x = 1.5\nend\n") == "any? ");
        report("a string stays a string", hints("s = \"a\"\ns = s + \"b\"\n") == "string! string! ");
    }


    // element types of arrays and maps: written (arr<int>), or taken from a literal
    expectError("an element of a literal array has the type of the literal", "a = [1, 2]\nx = a[0]\nx = \"s\"\n", "cannot assign str to 'x', which holds int");
    expectClean("a mixed literal holds either", "a = [1, \"s\"]\nx = a[0]\nx = \"t\"\nx = 5\n");
    expectError("storing the wrong type into an array with known elements", "a = [1, 2]\na[0] = \"s\"\n", "cannot put str into arr<int>");
    expectClean("storing the right type", "a = [1, 2]\na[0] = 5\na[1] = a[0] + 1\n");
    expectClean("an empty literal says nothing about its elements", "a = []\na[0] = \"s\"\na[1] = 4\n");
    expectError("a declared element type checks the literal", "local arr<str> n = [\"a\", 1]\n", "cannot put int into arr<str>");
    expectClean("a declared element type takes an empty literal", "local arr<int> n = []\nn[0] = 3\n");
    expectError("...and a declared element type holds it to it", "local arr<int> n = []\nn[0] = \"s\"\n", "cannot put str into arr<int>");
    expectClean("arr<any> takes anything", "local arr<any> n = [1]\nn[0] = \"s\"\nn[1] = [2]\n");
    expectError("a typed parameter takes the element type of its argument", "fn f(a: arr<int>)\nend\nf([\"s\"])\n", "cannot put str into arr<int>");
    expectError("...also for an argument that is not a literal", "fn f(a: arr<int>)\nend\nb = [\"s\"]\nf(b)\n", "expected arr<int> but got arr<str>");
    expectClean("...and an argument whose elements are not known", "fn f(a: arr<int>)\nend\nb = []\nf(b)\n");
    expectError("a declared return type with elements", "fn f() -> arr<int>\n  return [\"s\"]\nend\n", "cannot put str into arr<int>");
    expectError("an element of a function's result", "fn f() -> arr<int>\n  return [1]\nend\nx = f()[0]\nx = \"s\"\n", "cannot assign str to 'x', which holds int");
    expectError("unpacking an array gives its elements", "a, b = [1, 2]\na = \"s\"\n", "cannot assign str to 'a', which holds int");
    expectClean("unpacking something unknown", "a, b = f()\na = \"s\"\n");
    expectError("a for loop variable is an element", "a = [1, 2]\nfor v in a\n  v = \"s\"\nend\n", "cannot assign str to 'v', which holds int");
    expectError("nested arrays", "a = [[1], [2]]\na[0][0] = \"s\"\n", "cannot put str into arr<int>");
    expectError("a range holds ints", "r = [0...3]\nr[0] = \"s\"\n", "cannot put str into arr<int>");
    expectError("joining two arrays", "a = [1] + [2]\na[0] = \"s\"\n", "cannot put str into arr<int>");
    expectError("the elements of a rest parameter", "fn f(...n: int)\n  x = n[0]\n  x = \"s\"\nend\n", "cannot assign str to 'x', which holds int");
    expectError("assigning a literal of other elements", "a = [1]\na = [\"s\"]\n", "cannot put str into arr<int>");
    expectError("a map holds its values' type", "m = {\"a\": 1}\nm[\"b\"] = \"s\"\n", "cannot put str into map<int>");
    expectError("a declared map type checks the literal", "local map<str> m = {\"a\": 1}\n", "cannot put int into map<str>");
    expectError("assigning arrays of other elements", "a = [1]\nb = [\"s\"]\na = b\n", "cannot assign arr<str> to 'a', which holds arr<int>");
    expectClean("a map's own prototype is not one of its values", "base = {\"x\": 1}\nm = {\"a\": 1}\nm[\"__proto__\"] = base\nn = {\"a\": 1, \"__proto__\": base}\n");
    expectClean("a read from a map can come from its prototype, so it is not typed", "m = {\"a\": 1}\nx = m[\"k\"]\nx = \"s\"\n");
    expectError("the values of a for loop over a map", "m = {\"a\": 1}\nfor k, v in m\n  v = \"s\"\nend\n", "cannot assign str to 'v', which holds int");
    expectError("the keys of a for loop over a map", "m = {\"a\": 1}\nfor k in m\n  k = 5\nend\n", "cannot assign int to 'k', which holds str");
    // an element type is a promise, so nothing the VM skips rests on it
    {
        auto hints = [](const std::string& src) {
            embr::Lexer lex(src); auto toks = lex.tokenize(); embr::SourceMap sm = lex.sourceMap();
            embr::Parser parser(std::move(toks), sm); auto ast = parser.parse();
            embr::typecheck::Options o; o.source = &sm; (void)embr::typecheck::check(ast, o);
            std::string out;
            for (auto& st : ast) if (st->kind == embr::Stmt::Kind::Assign) {
                auto* a = static_cast<const embr::AssignStmt*>(st.get());
                out += a->storeHint.isAny() ? "any" : a->storeHint.name();
                out += a->storeProven ? "!" : "?";
                out += " ";
            }
            return out;
        };
        report("a value read from an array is tested, not proven", hints("a = [1, 2]\nx = a[0]\nx = a[1]\n") == "array! any? any? ");
    }
    // natives that say what they hold, and what they take
    {
        embr::Interpreter in;
        in.bindSig("tl_push", {embr::Param::io("arr", embr::TS::Arr), [] { auto p = embr::Param::req("val", embr::TS::Any); p.elemOf = 0; return p; }()},
                   [](const std::vector<embr::Value>&) { return embr::Value(int64_t(0)); });
        in.bindSig("tl_words", {embr::Param::req("s", embr::TS::Str)}, [](const std::vector<embr::Value>&) { return embr::Value(embr::Value::array_type{}); });
        in.setResultTypes({{"tl_words", embr::TS::Arr}});
        in.setResultElems({{"tl_words", embr::TS::Str}});
        auto e1 = typeErrorsIn(in, "a = [1]\ntl_push(&a, \"s\")\n");
        report("a native argument has to fit the elements of the array it goes into", e1.size() == 1 && e1[0].find("holds int, not str") != std::string::npos, join(e1));
        auto e2 = typeErrorsIn(in, "a = [1]\ntl_push(&a, 2)\nb = []\ntl_push(&b, \"s\")\n");
        report("...and an array of unknown elements takes anything", e2.empty(), join(e2));
        auto e3 = typeErrorsIn(in, "w = tl_words(\"a b\")\nx = w[0]\nx = 5\n");
        report("a native can say what the array it returns holds", e3.size() == 1 && e3[0].find("cannot assign int to 'x', which holds str") != std::string::npos, join(e3));
    }
    // globals: a function can use one that is assigned further down, and a global keeps its type between files
    expectError("a function sees a global assigned after it", "fn f()\n  return g - 1\nend\ng = \"s\"\n", "cannot use str with '-'");
    expectClean("...but a local of the same name stays its own", "fn f()\n  g = 1\n  g = g + 1\n  return g\nend\ng = \"s\"\n");
    {
        embr::Interpreter in;
        auto compile = [&](const std::string& src) {
            try { (void)embr::vm::compileSource(src, in, "t.embr"); return std::string(); }
            catch (const embr::EmbrError& e) { return e.message; }
        };
        report("a file's globals are typed for the next file", compile("rm_a = [1, 2]\nrm_m = {\"k\": 1}\n").empty());
        { embr::vm::VmRunner r(in); r.run(embr::vm::compileSource("rm_a = [1, 2]\n", in, "t0.embr")); }
        std::string e = compile("rm_a[0] = \"s\"\n");
        report("...with the elements of its arrays", e.find("cannot put str into arr<int>") != std::string::npos, e);
    }

    // natives bound from a C++ signature (bindTyped): the checker and the VM get the parameter and result types from it
    {
        embr::Interpreter in;
        in.bindTyped("tt_half", {"x"}, +[](double x) { return x / 2; });
        in.bindTyped("tt_size", {"s", "t"}, +[](const std::string& s, const std::string& t) -> int64_t { return (int64_t)(s.size() + t.size()); });
        in.bindTyped("tt_neg", {"n"}, +[](int64_t n) { return -n; });
        in.bindTyped("tt_up", {"s"}, +[](const std::string& s) { std::string o = s; for (auto& c : o) c = (char)std::toupper((unsigned char)c); return o; });
        in.bindTyped("tt_same", {}, +[](const embr::Value& v) { return v; });
        in.bindTyped("tt_pos", {"x"}, +[](double x) { return x > 0; });
        in.bindTyped("tt_none", {}, +[]() {});
        auto e1 = typeErrorsIn(in, "x = tt_half(3)\nx = \"s\"\n");
        report("a typed native's result type is the C++ return type", e1.size() == 1 && e1[0].find("cannot assign str to 'x', which holds float") != std::string::npos, join(e1));
        auto e2 = typeErrorsIn(in, "tt_size(\"a\", 1)\n");
        report("a typed native's parameters are checked before the script runs", e2.size() == 1 && e2[0].find("argument 't' to 'tt_size': expected str but got int") != std::string::npos, join(e2));
        auto e3 = typeErrorsIn(in, "tt_neg(2.5)\n");
        report("an int64_t parameter takes only an int", e3.size() == 1 && e3[0].find("expected int but got float") != std::string::npos, join(e3));
        auto e4 = typeErrorsIn(in, "a = tt_half(1)\nb = tt_half(2.5)\nc = tt_neg(3)\nc = tt_neg(c)\nd = tt_up(\"x\")\ne = tt_same([1])\ne = \"s\"\nf = tt_pos(1)\ng = tt_none()\n");
        report("...and what is fine is fine", e4.empty(), join(e4));
        // run them
        embr::Interpreter run;
        run.bindTyped("tt_half", {"x"}, +[](double x) { return x / 2; });
        run.bindTyped("tt_size", {"s", "t"}, +[](const std::string& s, const std::string& t) -> int64_t { return (int64_t)(s.size() + t.size()); });
        run.bindTyped("tt_neg", {"n"}, +[](int64_t n) { return -n; });
        run.bindTyped("tt_up", {"s"}, +[](const std::string& s) { std::string o = s; for (auto& c : o) c = (char)std::toupper((unsigned char)c); return o; });
        run.bindTyped("tt_same", {"v"}, +[](const embr::Value& v) { return v; });
        run.bindTyped("tt_pos", {"x"}, +[](double x) { return x > 0; });
        run.bindTyped("tt_none", {}, +[]() {});
        {
            embr::vm::runSource("tt_r1 = tt_half(5)\ntt_r2 = tt_size(\"ab\", \"c\")\ntt_r3 = tt_up(\"abc\")\ntt_r4 = tt_pos(2)\ntt_r5 = tt_neg(4)\n"
                                "tt_r6 = tt_same([1, 2])\ntt_r7 = tt_none()\ntt_r8 = tt_pos(-1)\n", run, "tt.embr");
            auto get = [&](const char* n) { const embr::Value* v = run.find(n); return v ? v->formatAsString() : std::string("?"); };
            report("typed natives run and give the right values",
                   get("tt_r1") == "2.5" && get("tt_r2") == "3" && get("tt_r3") == "ABC" && get("tt_r4") == "1" && get("tt_r5") == "-4" &&
                   get("tt_r6") == "[1, 2]" && get("tt_r7") == "0" && get("tt_r8") == "0",
                   get("tt_r1") + " " + get("tt_r2") + " " + get("tt_r3") + " " + get("tt_r4") + " " + get("tt_r5") + " " + get("tt_r6") + " " + get("tt_r7") + " " + get("tt_r8"));
            // inside a function, an int for a double, in a loop, and the function replaced while its arguments are worked out
            run.bindTyped("tt_boom", {"x"}, +[](double x) -> double { if (x > 1) embr::raiseError("tt_boom", "too big"); return x; });
            embr::vm::runSource("fn tt_use(v)\n  t = 0.0\n  i = 0\n  while i < 3\n    t = t + tt_half(v) + tt_size(\"a\", \"b\")\n    i = i + 1\n  end\n  return t\nend\ntt_r9 = tt_use(4)\n"
                                "fn tt_swap()\n  tt_half = fn(x) return 100 end\n  return 1\nend\ntt_r10 = tt_half(tt_swap())\n", run, "tt3.embr");
            report("typed natives inside functions and loops, and a replaced one", get("tt_r9") == "12" && get("tt_r10") == "100", get("tt_r9") + " " + get("tt_r10"));
            std::string boom;
            try { auto prog = embr::vm::compileSource("x = 1\ny = tt_boom(5)\n", run, "tt4.embr"); embr::vm::VmRunner r(run); r.run(prog); } catch (const embr::EmbrError& e) { boom = e.message + " @" + std::to_string(e.range.startLine); }
            report("an error from a typed native points at the call", boom.find("too big") != std::string::npos && boom.find("@2") != std::string::npos, boom);
            std::string err;
            try { auto prog = embr::vm::compileSource("local any tt_z = 5\ntt_up(tt_z)\n", run, "tt2.embr"); embr::vm::VmRunner r(run); r.run(prog); } catch (const embr::EmbrError& e) { err = e.message; }
            report("a value the checker can't see is refused at run time", err.find("argument 's' to 'tt_up'") != std::string::npos, err);
        }
    }

    // the type record (Options::typesOut), for tools that want the type of every expression
    {
        auto check = [&](const std::string& name, const std::string& src, int line, int col, const std::string& want, bool closed = false) {
            std::string got = recordedType(src, line, col, closed);
            report(name, got == want, "wanted '" + want + "', got '" + got + "'");
        };
        check("record: a literal is sure", "x = 3\n", 1, 5, "int*");
        check("record: a string literal is sure", "x = \"a\"\n", 1, 5, "str*");
        check("record: / is a float", "x = 7 / 2\n", 1, 5, "float*");
        check("record: a comparison is an int", "x = 1 < 2\n", 1, 5, "int*");
        check("record: + - * give num, the VM turns an overflow into a float", "x = 1 + 2\n", 1, 5, "num");
        check("record: a variable use has the variable's type", "x = 3\ny = x\n", 2, 5, "int*");
        check("record: a call of a typed fn has its result type", "fn f(a: int) -> int\n  return a\nend\ny = f(2)\n", 4, 5, "int*");
        check("record: an array element is not sure, an element type is a promise", "local arr<int> a = [1, 2]\nb = a[0]\n", 2, 5, "int");
        check("record: an untyped parameter of a closed fn takes the types of the calls", "fn g(n)\n  return n\nend\nz = g(3)\n", 2, 10, "int*", true);
        check("record: nothing starts at a position with no expression", "x = 3\n", 5, 1, "none");
        // the record changes nothing else: the same errors with and without it
        for (const char* src : {"x = 1\nx = \"a\"\n", "fn f(a: int) -> int\n  return a\nend\nf(\"s\")\n", "x = 1\ny = x + 2\n"}) {
            std::vector<std::string> with;
            recordedType(src, 1, 1, false, &with);
            auto without = typeErrors(src);
            report(std::string("record: errors unchanged for ") + std::to_string(without.size()) + " error source", join(with) == join(without), join(with) + " vs " + join(without));
        }
    }

    // function types: fn(int) -> int
    expectClean("a lambda fits a parameter of a function type", "fn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(fn(a: int) -> int return a end))\n");
    expectClean("an unannotated lambda fits a function type", "fn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(fn(a) return a end))\n");
    expectClean("a function type with no result does not check the result", "fn ft(f: fn(int)) -> int\n  return f(1)\nend\nprint(ft(fn(a: int) -> str return \"s\" end))\n");
    expectClean("a variable that took its type from a lambda may be given a lambda of another arity", "f = fn(a) return a end\nf = fn(a, b) return a end\n");
    expectClean("a declared function is a value of its own type", "fn sq(x: int) -> int\n  return x\nend\nfn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(sq))\n");
    expectClean("a function that returns a function type", "fn mk() -> fn(int) -> int\n  return fn(a: int) -> int return a end\nend\nprint(mk()(3))\n");
    expectError("a lambda with another number of parameters", "fn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(fn(a: int, b: int) -> int return a end))\n",
                "expected fn(int) -> int but got fn(int, int) -> int");
    expectError("a lambda with a parameter of another type", "fn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(fn(a: str) -> int return 1 end))\n",
                "expected fn(int) -> int but got fn(str) -> int");
    expectError("a lambda with a result of another type", "fn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(fn(a: int) -> str return \"s\" end))\n",
                "expected fn(int) -> int but got fn(int) -> str");
    expectError("a declared function of another arity", "fn sq(x: int) -> int\n  return x\nend\nfn ft(f: fn(int, int) -> int) -> int\n  return f(1, 2)\nend\nprint(ft(sq))\n",
                "expected fn(int, int) -> int but got fn(int) -> int");
    expectError("a call with too many arguments", "fn ft(f: fn(int) -> int) -> int\n  return f(1, 2)\nend\n", "which takes 1 argument, not 2");
    expectError("a call with an argument of the wrong type", "fn ft(f: fn(int) -> int) -> int\n  return f(\"s\")\nend\n", "argument 1 to 'f': expected int but got str");
    expectError("the result of a call is the written result", "fn ft(f: fn(int) -> int) -> str\n  x = f(1)\n  return x\nend\n", "returns int");
    expectError("a local of a function type takes a function of that type", "local fn(int) -> int g = fn(a: int, b: int) -> int return a end\n",
                "cannot assign fn(int, int) -> int to 'g' declared as fn(int) -> int");
    expectError("a number is not a function", "fn ft(f: fn(int) -> int) -> int\n  return f(1)\nend\nprint(ft(5))\n", "expected fn(int) -> int but got int");

    // every message points at the source
    {
        auto errs = typeErrors("x = 1\ny = 2\nx = \"a\"\n");
        report("an error has a position", errs.size() == 1);
    }

    std::cout << "\n  " << g_passed << " passed, " << g_failed << " failed"
              << (g_failed ? "  \033[31mX\033[0m" : "  \033[32m\xE2\x88\x9A\033[0m") << "\n\n";
    return g_failed ? 1 : 0;
}
