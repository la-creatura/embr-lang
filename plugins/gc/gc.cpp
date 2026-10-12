// gc.cpp
// opt-in mark-and-sweep cycle collector for embr's closures and scopes
//
// closures hold their captured scopes, and reference counting can't free a cycle. for example:
//
//   fn make_thing()
//       local self_ref = 0
//       fn recur()
//           return self_ref
//       end
//       self_ref = recur   # the scope holds recur, and recur's capture holds the scope:
//                          # neither count ever reaches 0
//       return recur
//   end
//
// that's harmless for a short script, but matters for a long-lived Interpreter (parallel/async workers, an
// embedding host). it's a plugin so nobody pays for it unless they `import "gc"`
//
// usage
//   import "gc"
//   gc_collect()       # returns how many scopes were swept
//   gc_scope_count()   # how many scopes and captured-variable cells are alive right now
//
// how it works
//   1. mark: start from every scope and variable cell reachable from an active call stack (Interpreter::gcRoots() and
//      gcCellRoots()), follow every Value they hold (arrays, maps, closures' captured cells and scopes) and mark each
//      Scope or Cell found
//   2. sweep: every live scope or cell that wasn't marked (Interpreter::gcAllLiveScopes(), gcAllLiveCells()) gets its
//      contents cleared.
//      this doesn't free the Scope itself (something may still hold it), but dropping its entries breaks the
//      cycle so the closures can finally be freed
//
// plugin userdata: a Value reachable only through a plugin's opaque handle (TypedPtr::owner) is invisible to
// the walk, unless the plugin attaches a tracer (Value::withTracer in core/value.h) that visits
// each Value it holds. table handles do. this matters for correctness, not just leaks: a closure reachable
// only through an untraced handle looks like garbage, so its scope is cleared and the live closure then fails
// with "undefined variable". a plugin that stores script Values (closures, or arrays/maps containing them)
// must attach a tracer. handles that hold only plain data (shared_table, async promises, buffers) need none

#include <embr/embr.h>
#include <unordered_set>

using namespace embr;

namespace {

// everything the mark phase remembers: scopes already walked (also the cycle guard for closures that
// reference each other) and traced handles already walked (the cycle guard for a table that holds itself,
// directly or through a closure)
struct Marked {
    std::unordered_set<Scope*>     scopes;
    std::unordered_set<Cell*>      cells;
    std::unordered_set<const void*> handles;
};

void markScope(Scope* s, Marked& m);
void markCell(Cell* c, Marked& m);

// follows one Value to the Scopes it can reach: a script closure's captured layers, anything inside an array/map,
// or (through a tracer a plugin attached with Value::withTracer) what a plugin's userdata holds. native closures, numbers, strings and
// untraced pointers reach no Scope
void markValue(const Value& v, Marked& m) {
    if (v.isCallable()) {
        const auto& c = v.asCallable();
        if (c.isScript() && c.script.captured) {
            for (auto& layer : c.script.captured->layers)
                markScope(layer.get(), m);
            if (c.script.captured->upvals)
                for (auto& cell : *c.script.captured->upvals)
                    markCell(cell.get(), m);
        }
    } else if (v.isArray()) {
        for (auto& el : v.asArray()) markValue(el, m);
    } else if (v.isMap()) {
        for (auto& [k, val] : v.asMap()) markValue(val, m);
    } else if (v.isPointer()) {
        const auto& p = v.asPointer();
        if (p.h && p.h->tracer && m.handles.insert(p.ptr).second)
            (*p.h->tracer)([&m](const Value& held) { markValue(held, m); });
    }
}

// marks one scope reachable and walks every value it holds
// the visited set doubles as cycle protection: two scopes whose closures reference each other back
// (directly or through several hops) would otherwise recurse forever
void markScope(Scope* s, Marked& m) {
    if (!s || !m.scopes.insert(s).second) return;
    for (auto& [k, v] : *s) markValue(v, m);
}

// a variable cell reached from a closure or a running frame: walks the value it holds
void markCell(Cell* c, Marked& m) {
    if (!c || !m.cells.insert(c).second) return;
    markValue(c->v, m);
}

} // namespace

EMBR_PLUGIN {

    // gc_collect() -> num
    // marks every scope reachable from the active call stack (either backend) and clears any live scope that wasn't
    // reached, which breaks the cycle holding it. returns how many scopes were cleared
    interp->bindSpan("gc_collect", {}, [interp](const ArgSpan&) -> Value {
        Marked visited;
        for (auto& root : interp->gcRoots())
            markScope(root.get(), visited);
        // a VM frame keeps its locals as raw Values (CallFrame::slots in vm.h) and the ones a closure captured in cells,
        // not in a Scope that gcRoots() covers. walk those too, so a closure in one of them isn't mistaken for garbage
        for (auto& root : interp->gcValueRoots())
            for (auto& v : *root) markValue(v, visited);
        for (auto& root : interp->gcCellRoots())
            for (auto& cell : *root) markCell(cell.get(), visited);
        // values plugins keep in their own state independent of any handle (see addGcRootProvider)
        for (auto& provider : interp->gcRootProviders())
            provider([&visited](const Value& held) { markValue(held, visited); });

        double cleared = 0.0;
        for (auto& scope : interp->gcAllLiveScopes()) {
            if (!visited.scopes.count(scope.get()) && !scope->empty()) {
                scope->clear();
                interp->scopesChanged();
                cleared += 1.0;
            }
        }
        // a cell only a cycle holds: empty it, the same way
        for (auto& cell : interp->gcAllLiveCells()) {
            if (!visited.cells.count(cell.get()) && cell->defined) {
                cell->v = Value();
                cell->defined = false;
                cleared += 1.0;
            }
        }
        return Value(cleared);
    });

    // gc_scope_count() -> num
    // how many scopes and captured-variable cells are alive right now (on any stack, captured by a live closure, or
    // stuck in an uncollected cycle). mostly a diagnostic for seeing what gc_collect() did
    interp->bindSpan("gc_scope_count", {}, [interp](const ArgSpan&) -> Value {
        return Value((int64_t)(interp->gcAllLiveScopes().size() + interp->gcAllLiveCells().size()));
    });

    // what each always returns (see Value::Callable::returns)
    interp->setResultTypes({
        {"gc_collect", TS::Float}, {"gc_scope_count", TS::Int},
    });
}

// what this plugin says about itself. embr reads it before it runs embr_register (see core/plugininfo.h)
EMBR_PLUGIN_INFO("gc", "1.0.0", "opt-in cycle collector", EMBR_CAP_NONE, EMBR_CAP_NONE,
    {"gc_collect", "how many scopes and cells were cleared"},
    {"gc_scope_count", "scopes and variable cells alive right now (diagnostic / testing hook)"});
