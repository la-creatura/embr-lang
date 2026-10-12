// typed.h
// how a C++ function with plain parameter types (double, int64_t, std::string) becomes a native function.
//
// usage (see Interpreter::bindTyped)
//   interp->bindTyped("math_sqrt", {"x"}, +[](double x) { return std::sqrt(x); });
//
// good to know
//   - a function that needs the interpreter takes it as its first parameter: `+[](Interpreter& in, const Value& v) { ... }`
//   - double takes a number (an int is turned into a float), int64_t takes an int only, const std::string& a string,
//     const Value::array_type& an array, const Value::map_type& a map, const Value& anything. the result type of the script function is the return type of the C++ one.

#ifndef EMBR_CORE_TYPED_H
#define EMBR_CORE_TYPED_H

#include "value.h"
#include "types.h"
#include "fwd.h"

#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>

namespace embr {
namespace typed {

// a parameter: the script type it asks for and how to read it from a Value (the VM has already checked the type)
template <class T> struct Arg;
template <> struct Arg<double>      { static TypeSet type() { return TS::Num; }  static double get(const Value& v) { return v.asNumber(); } };
template <> struct Arg<int64_t>     { static TypeSet type() { return TS::Int; }  static int64_t get(const Value& v) { return v.asInt(); } };
template <> struct Arg<std::string> { static TypeSet type() { return TS::Str; }  static const std::string& get(const Value& v) { return v.asString(); } };
template <> struct Arg<Value>       { static TypeSet type() { return TS::Any; }  static const Value& get(const Value& v) { return v; } };
template <> struct Arg<Value::array_type> { static TypeSet type() { return TS::Arr; } static const Value::array_type& get(const Value& v) { return v.asArray(); } };
template <> struct Arg<Value::map_type>   { static TypeSet type() { return TS::Map; } static const Value::map_type& get(const Value& v) { return v.asMap(); } };

// a result: the script type it always is and how to make the Value
template <class T> struct Ret;
template <> struct Ret<double>      { static TypeSet type() { return TS::Float; } static Value make(double d) { return Value(d); } };
template <> struct Ret<int64_t>     { static TypeSet type() { return TS::Int; }   static Value make(int64_t i) { return Value(i); } };
template <> struct Ret<bool>        { static TypeSet type() { return TS::Int; } static Value make(bool b) { return Value(b); } };   // the int 1 or 0
template <> struct Ret<std::string> { static TypeSet type() { return TS::Str; }   static Value make(std::string s) { return Value(std::move(s)); } };
template <> struct Ret<Value>       { static TypeSet type() { return TS::Any; }   static Value make(Value v) { return v; } };
template <> struct Ret<void>        { static TypeSet type() { return TS::Float; } };   // gives 0

// EMBR_NO_TYPED=1 binds every typed native the ordinary way (no direct call), to compare the two and to check that they answer alike.
// read when the native is bound, so a plugin sees it too
inline bool typedCallsEnabled() {
    const char* e = std::getenv("EMBR_NO_TYPED");
    return !(e && *e && *e != '0');
}

template <class T> using Plain = std::remove_cv_t<std::remove_reference_t<T>>;

template <class R, class... A, size_t... I>
Value call(R (*fn)(A...), const ArgSpan& a, std::index_sequence<I...>) {
    if constexpr (std::is_void_v<R>) { fn(Arg<Plain<A>>::get(a[I])...); return Value(0.0); }
    else return Ret<Plain<R>>::make(fn(Arg<Plain<A>>::get(a[I])...));
}
// the same for a function whose first parameter is the interpreter
template <class R, class... A, size_t... I>
Value callI(R (*fn)(Interpreter&, A...), Interpreter& in, const ArgSpan& a, std::index_sequence<I...>) {
    if constexpr (std::is_void_v<R>) { fn(in, Arg<Plain<A>>::get(a[I])...); return Value(0.0); }
    else return Ret<Plain<R>>::make(fn(in, Arg<Plain<A>>::get(a[I])...));
}

// what the VM calls for a typed native: the function pointer comes back as a void*, and the arguments are read where they sit.
// ctx is the interpreter the native was bound in
template <class R, class... A>
Value thunk(void* fn, void*, const Value* args) {
    return call(reinterpret_cast<R (*)(A...)>(fn), ArgSpan{args, sizeof...(A)}, std::index_sequence_for<A...>{});
}
template <class R, class... A>
Value thunkI(void* fn, void* ctx, const Value* args) {
    return callI(reinterpret_cast<R (*)(Interpreter&, A...)>(fn), *static_cast<Interpreter*>(ctx), ArgSpan{args, sizeof...(A)}, std::index_sequence_for<A...>{});
}

} // namespace typed
} // namespace embr

#endif // EMBR_CORE_TYPED_H
