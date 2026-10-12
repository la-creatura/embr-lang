// hello.cpp
// the worked example for the writing-plugins wiki page
// it is built and exercised by CTest so the tutorial cannot drift from the real plugin API
//
// import "hello"
// print(hello_greet("wowwd"))                       # hewwo, wowwd uwu!
// c = hello_counter()                               # an opaque, unforgeable handle
// hello_counter_add(c, 5)
// print(hello_counter_get(c))                       # 5
// print(hello_map_sum([1, 2, 3], fn(x) return x * 10 end))   # 60  (calls back into script code)
// print(hello_describe("abc"), hello_describe(21))          # two overloads, one name

#include <embr/embr.h>

using namespace embr;

// tiny helpers so each signature below reads as one line
static Param pStr(std::string n)                      { return Param::req(std::move(n), TS::Str); }
static Param pNum(std::string n)                      { return Param::req(std::move(n), TS::Num); }
static Param pArr(std::string n)                      { return Param::req(std::move(n), TS::Arr); }
static Param pPtr(std::string n)                      { return Param::req(std::move(n), TS::Ptr); }
static Param pFn (std::string n)                      { return Param::req(std::move(n), TS::Fn | TS::Ptr); }
static Param pOpt(std::string n, TypeSet m = TS::Any) { return Param::opt(std::move(n), m);       }

// the C++ object behind the handle
// scripts never see this type, only an opaque pointer tagged "hello.counter"
struct Counter { int64_t value = 0; };

EMBR_PLUGIN {

    // hello_greet(name: str, greeting?: str) -> str
    // bindSig() attaches the signature.
    // embr checks arity and argument types BEFORE calling the lambda
    // so args[0] is known to be a string and args.size() is 1 or 2
    // the error a script gets for hello_greet(5) is generated for you
    interp->bindSig("hello_greet", {pStr("name"), pOpt("greeting", TS::Str)},
    [](const std::vector<Value>& args) -> Value {
        std::string greeting = args.size() >= 2 ? args[1].asString() : "hewwo";
        return Value(greeting + ", " + args[0].asString() + " uwu!");
    });

    // two functions with the same name and a number on the end are one overload group, hello_describe. a script calls the bare name and
    // embr picks the one that takes the argument. the number is part of each function's name: a script that calls hello_describe__2 gets
    // that one for ever, so add hello_describe__3 rather than changing a number
    interp->bindSig("hello_describe__1", {pStr("text")},
    [](const std::vector<Value>& args) -> Value {
        return Value("a string of " + std::to_string(args[0].asString().size()) + " bytes");
    });
    interp->bindSig("hello_describe__2", {pNum("n")},
    [](const std::vector<Value>& args) -> Value {
        return Value("a number, doubled it is " + formatNumber(args[0].asNumber() * 2));
    });

    // hello_counter() -> ptr<hello.counter>
    // a handle is a typed pointer
    // makeUserdata<T> moves the object into a shared_ptr owned by the Value, so it is freed when the last copy of the handle goes away
    interp->bindSig("hello_counter", {},
    [](const std::vector<Value>&) -> Value {
        return Value::makeUserdata<Counter>("hello.counter", Counter{});
    });

    // hello_counter_add(c: ptr, n: num) -> num   (returns the new total)
    // userdata<T>(tag) checks the tag and raises a clean error if a script passes some OTHER kind
    // of pointer (or a non-pointer): you never have to validate the handle by hand.
    interp->bindSig("hello_counter_add", {pPtr("c"), pNum("n")},
    [](const std::vector<Value>& args) -> Value {
        auto* c = args[0].userdata<Counter>("hello.counter");
        if (!args[1].isInt())
            raiseError("hello_counter_add", "n must be an integer, got " + args[1].formatAsString());
        c->value += args[1].asInt();
        return Value(c->value);
    });

    // hello_counter_get(c: ptr) -> num
    interp->bindSig("hello_counter_get", {pPtr("c")},
    [](const std::vector<Value>& args) -> Value {
        return Value(args[0].userdata<Counter>("hello.counter")->value);
    });

    // hello_map_sum(values: arr, fn: fn|ptr) -> num
    // calling back into script code go through embr::invoke()
    // also accepts a typed pointer whose tag has a __<tag>_call dunder
    // the interpreter is captured by the lambda because invoke() needs it
    interp->bindSig("hello_map_sum", {pArr("values"), pFn("fn")},
    [interp](const std::vector<Value>& args) -> Value {
        double total = 0;
        for (const Value& v : args[0].asArray()) {
            Value r = embr::invoke(*interp, args[1], {v});
            if (!r.isNumeric())
                raiseError("hello_map_sum", "fn must return a number, got " + r.typeName());
            total += r.asNumber();
        }
        return Value(total);
    });

    // what hello_greet always gives back, so the checker knows
    interp->setResultTypes({{"hello_greet", TS::Str}, {"hello_describe__1", TS::Str}, {"hello_describe__2", TS::Str}});
    // hello_greet depends only on its arguments, so a const may call it: const HI = hello_greet("you")
    interp->setPure({"hello_greet"});
}

// what this plugin says about itself. embr reads it before it runs embr_register, so it can refuse a plugin that needs
// something the host does not allow, and `embr plugin hello` can print it (see the writing-plugins wiki page)
EMBR_PLUGIN_INFO("hello", "1.0.0", "the worked example for the writing-plugins wiki page", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"hello_counter", "a new counter handle, starting at 0"},
    {"hello_counter_add", "adds n to the counter and gives back the new total"},
    {"hello_counter_get", "the counter's current total"},
    {"hello_describe__1", "describes a string by its size in bytes"},
    {"hello_describe__2", "describes a number by what it is doubled"},
    {"hello_greet", "a greeting for name, with an optional greeting word instead of the default"},
    {"hello_map_sum", "calls fn on every value of the array and adds up the results"});
