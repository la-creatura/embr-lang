// table.cpp
// a Lua-style table for embr: an associative container keyed by any value, not just strings like embr's map literals
// it's a plugin built on TypedPtr/userdata, rather than widening Value::map_type's key type, which would touch
// MapExpr/BUILD_MAP in both backends and Value's equality/printing
//
// usage
//   import "table"
//   t = table()
//   table_set(t, 1, "one")
//   table_set(t, "name", "embr")
//   print(table_get(t, 1))          # "one"
//   print(table_has(t, "missing"))  # 0
//   for pair in table_items(t)
//       print(pair[0])
//       print(pair[1])
//   end
//
// good to know
//   - not synchronized: use an instance from one thread only. to share across workers see shared_table() in
//     plugins/parallel, which uses the same key rules (table.h) plus a lock
//   - the key rules (table.h) are stricter than Value::operator==, which treats mismatched types as equal if they
//     print the same (5 == "5"). that's fine for `if x == 5` but wrong for a hash table. these follow Lua:
//       numbers:       int and float with the same value are the same key (5 and 5.0). NaN raises
//       strings:       compared by content, never equal to a number key
//       pointers/fns:  compared by identity (pointer address, or the Callable object's address), like Lua
//       arrays/maps:   not supported (raises). they are plain values with no identity, so there is no sound
//                      meaning of "the same key"
//   - iteration order (table_keys/table_values/table_items) is unspecified and can change between runs, like
//     std::unordered_map. keys can mix numbers, strings and identities, so there is no one sort order

#include "table.h"

using namespace embr;
using namespace embrtable;

static Param pPtr(std::string n) { return Param::req(std::move(n), TS::Ptr); }
static Param pAny(std::string n) { return Param::req(std::move(n), TS::Any); }

struct Table {
    TableData data;
};

static Table* requireTable(const Value& v) {
    return v.userdata<Table>("table.handle");
}

EMBR_PLUGIN {

    // table() -> ptr<table.handle>
    interp->bindSpan("table", {}, [interp](const ArgSpan&) -> Value {
        Value h = Value::makeUserdata<Table>("table.handle");
        // at interpreter teardown, empty this table (see Interpreter::addClearable): breaks a table that holds itself
        interp->addClearable(h.asPointer().owner(), [w = std::weak_ptr<Table>(std::static_pointer_cast<Table>(h.asPointer().owner()))] {
            if (auto t = w.lock()) t->data.clear();
        });
        // let gc_collect() see the keys and values this table holds (a closure stored here would
        // otherwise look unreachable and have its captured scope cleared out from under it)
        Table* t = static_cast<Table*>(h.asPointer().ptr);
        h.withTracer([t](const TraceVisitor& visit) {
            for (const auto& kv : t->data) { visit(kv.second.first); visit(kv.second.second); }
        });
        return h;
    });

    // table_set(t, key, value) -> 0
    interp->bindSpan("table_set", {pPtr("t"), pAny("key"), pAny("value")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_set", args[1]);
        t->data[k] = {args[1], args[2]};
        return Value(0.0);
    });

    // table_get(t, key) -> any
    // raises "key not found" if key isn't in t (matches how indexing a
    // core map with [] behaves), use table_has() to check first.
    interp->bindSpan("table_get", {pPtr("t"), pAny("key")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_get", args[1]);
        auto it = t->data.find(k);
        if (it == t->data.end())
            throwTableError("table_get", "key not found: " + valueRepr(args[1]));
        return it->second.second;
    });

    // table_has(t, key) -> num
    interp->bindSpan("table_has", {pPtr("t"), pAny("key")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_has", args[1]);
        return Value(t->data.count(k) ? (int64_t)1 : (int64_t)0);
    });

    // table_delete(t, key) -> num
    // returns 1 if a key was actually removed, 0 if it wasn't present
    // (a no-op, not an error, matches fs_remove()/meta_undef()'s convention).
    interp->bindSpan("table_delete", {pPtr("t"), pAny("key")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table_delete", args[1]);
        return Value(t->data.erase(k) > 0 ? (int64_t)1 : (int64_t)0);
    });

    // table_len(t) -> num
    interp->bindSpan("table_len", {pPtr("t")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        return Value((int64_t)t->data.size());
    });

    // table_clear(t) -> 0
    interp->bindSpan("table_clear", {pPtr("t")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        t->data.clear();
        return Value(0.0);
    });

    // table_keys(t) -> arr
    interp->bindSpan("table_keys", {pPtr("t")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) out.push_back(kv.first);
        return Value(std::move(out));
    });

    // table_values(t) -> arr
    interp->bindSpan("table_values", {pPtr("t")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) out.push_back(kv.second);
        return Value(std::move(out));
    });

    // table_items(t) -> arr of [key, value] pairs
    // the way to iterate a table's contents (embr's `for k, v in ...`
    // syntax only understands core map/array values, not a plugin userdata
    // handle), e.g. `for pair in table_items(t) ... pair[0]/pair[1] ... end`
    interp->bindSpan("table_items", {pPtr("t")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) {
            Value::array_type pair;
            pair.push_back(kv.first);
            pair.push_back(kv.second);
            out.emplace_back(std::move(pair));
        }
        return Value(std::move(out));
    });

    // t[key], t[key] = x and `for pair in t` go through these (the dunder convention, see core/registry.h). index raises when the key is
    // missing, like table_get. setindex returns the table, which the assignment writes back to the variable. iter gives the
    // [key, value] pairs, like table_items
    interp->bindSpan("__table_handle_index", {pPtr("t"), pAny("key")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table index", args[1]);
        auto it = t->data.find(k);
        if (it == t->data.end()) throwTableError("table index", "key not found: " + valueRepr(args[1]));
        return it->second.second;
    });
    interp->bindSpan("__table_handle_setindex", {pPtr("t"), pAny("key"), pAny("value")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        TableKey k = makeKey("table index", args[1]);
        t->data[k] = {args[1], args[2]};
        return args[0];
    });
    interp->bindSpan("__table_handle_iter", {pPtr("t")},
    [](const ArgSpan& args) -> Value {
        Table* t = requireTable(args[0]);
        Value::array_type out;
        out.reserve(t->data.size());
        for (auto& [k, kv] : t->data) {
            Value::array_type pair;
            pair.push_back(kv.first);
            pair.push_back(kv.second);
            out.emplace_back(std::move(pair));
        }
        return Value(std::move(out));
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"__table_handle_setindex", TS::Ptr}, {"__table_handle_iter", TS::Arr},
        {"table", TS::Ptr}, {"table_set", TS::Float}, {"table_has", TS::Int}, {"table_delete", TS::Int}, {"table_len", TS::Int},
        {"table_clear", TS::Float}, {"table_keys", TS::Arr}, {"table_values", TS::Arr}, {"table_items", TS::Arr},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("table", "1.0.0", "lua-style tables with non-string keys", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"__table_handle_index", "runs for t[key]; the same as table_get"},
    {"__table_handle_iter", "runs for `for pair in t`; gives the [key, value] pairs"},
    {"__table_handle_setindex", "runs for t[key] = value; the same as table_set"},
    {"table", "new empty table handle"},
    {"table_clear", "removes every entry"},
    {"table_delete", "1 if removed, 0 if absent (no-op)"},
    {"table_get", "raises \"key not found: <repr>\" if absent. t[key] does the same, t[key] = x is table_set, and for pair in t loops over the [key, value] pairs (the __table_handle_index, _setindex and _iter dunders)"},
    {"table_has", "1 if key is in the table, else 0"},
    {"table_items", "the documented iteration idiom: for pair in table_items(t) ... pair[0]/pair[1] ... end"},
    {"table_keys", "unspecified bucket order"},
    {"table_len", "the number of entries"},
    {"table_set", "stores value under key, replacing an old value"},
    {"table_values", "unspecified bucket order"});
