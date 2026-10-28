#ifndef EMBR_BACKENDS_VM_H
#define EMBR_BACKENDS_VM_H
// vm backend for embr
// compiles the AST to a flat list of instructions and runs them
//
// usage:
//   embr::Interpreter interp;
//   embr::vm::runSource(src, interp, "<input>");
//
// how it fits together:
//   Compiler   walks the AST once and emits instructions. each function body becomes its own CompiledChunk
//   VmRunner   runs the instructions and keeps its own stack of call frames
//   variables at true top level go through the *_GLOBAL opcodes into the Interpreter's global scope
//   a function compiled here keeps its chunk in ScriptFn::compiledChunk (type-erased, see core/value.h)
//
// YIELD pauses the runner. call step() or run() again to continue, optionally after inject()ing instructions
// inject() lets a plugin splice instructions into the running stream (loops, async continuations, ...)
// see the wiki, architecture page, for the big picture

#include "../core/diagnostics.h"
#include "../core/types.h"
#include "../core/value.h"
#include "../core/ast.h"
#include "../core/registry.h"
#include "../core/lexer.h"
#include "../core/parser.h"
#include "../core/platform.h"

#include <new>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <cassert>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <optional>

#ifdef EMBR_WITH_VM

namespace embr {
namespace vm {

// instruction set

enum class Op : uint8_t {
    // literals
    PUSH_NUM,       // operand = index into CompiledChunk::nums
    PUSH_INT,       // operand = index into CompiledChunk::ints
    PUSH_STR,       // operand = string-table index
    PUSH_NIL,       // push Value(0.0)

    // stack
    POP,            // discard top

    // variables
    LOAD,           // operand = string-table index  ->  push value
    STORE,          // operand = string-table index  <-  pop value, assign-or-create in nearest scope, type-checked if declared
    DEFINE,         // operand = string-table index  <-  pop value, define untyped in current (innermost) scope
    DEFINE_TYPED,   // operand = string-table index, typeHint = declared type  <-  pop value, type-checked
    LOAD_GLOBAL,    // operand = string-table index  ->  read from Interpreter global scope
    STORE_GLOBAL,   // operand = string-table index  <-  write to Interpreter global scope
    DEFINE_GLOBAL,       // operand = string-table index  <-  pop value, define untyped in the Interpreter global scope
    DEFINE_GLOBAL_TYPED, // operand = string-table index, typeHint = declared type  <-  pop value, type-checked, defined globally
    // operand = string-table index. emitted right after a DEFINE_GLOBAL* for a top-level `local`,
    // so a module importer leaves that name out of its exports (the opcode calls Interpreter::markModuleLocal)
    // no stack effect
    MARK_MODULE_LOCAL,

    // compiled from `x = x + simple` (see compileAssign). operand = name, slot = local slot or -1, arg = ip to jump to.
    // if x holds a string, appends the popped value to it in place and jumps. otherwise changes nothing and falls through
    // to the ordinary load/add/store code. saves copying the whole string every time round a loop
    APPEND_VAR,

    // arithmetic / string
    ADD, SUB, MUL, DIV, MOD,
    NEG,            // unary minus

    // comparison  (all pop two, push number 0/1)
    EQ, NEQ, LT, GT, LTE, GTE,

    // logic
    NOT,            // unary !

    // bit operators on ints (64 bit two's complement). pop two, push an int. a shift count outside 0..63 is an error, << wraps, >> keeps the sign
    BAND, BOR, BXOR, SHL, SHR,
    BNOT,           // unary ~

    // control flow
    JUMP,           // operand = absolute instruction index
    JUMP_IF_FALSE,  // operand = absolute instruction index; pops condition

    // jump but leave value on stack
    JUMP_IF_FALSE_PEEK,
    JUMP_IF_TRUE_PEEK,

    // containers
    BUILD_ARRAY,    // operand = element count; pops N, pushes array
    BUILD_MAP,      // operand = entry count;   pops 2N (key,val pairs), pushes map
    BUILD_RANGE,    // operand = 2 or 3: pops end, [second], start and pushes the array of start, start + step, ... up to end, without building it (Value::makeRange)
    INDEX_GET,      // pops container + key, pushes element
    // compiled from `name[simple]`. operand = name, slot = local slot or -1. pops only the key and reads the variable in
    // place, so indexing a big array doesn't copy it first. only used when the key can't run user code (see compileIndexGet)
    // with kStash set, a STASH_VAR for the same name ran first and its result sits under the key
    INDEX_GET_VAR,
    INDEX_SET,      // pops container + key + value, pushes updated container, then STORE follows
    // compiled from `name[key] = value`. operand = name, slot = local slot or -1, arg = ip after the STORE that follows.
    // pops value and key (and the stash, with kStash). if the variable can be changed where it lives, changes it there and
    // jumps over the STORE. otherwise pushes the updated copy and falls through to the STORE
    INDEX_SET_VAR,
    // pushes what a later INDEX_SET_VAR / APPEND_ARR needs of the variable `name` before the key / value code runs. if that
    // code can't change the variable (it is a local of this frame, or kCallFree, or kPureCalls with every callee still native)
    // this is a placeholder and the variable is read when it is needed. otherwise it is a copy, so the key / value code
    // can't change what gets indexed. arg = index into CompiledChunk::pureSets for kPureCalls
    STASH_VAR,
    // compiled from `name = push(name, x)`. stack: [push, stash, x]. when push is embrlib's and the variable is an array,
    // appends in place, pops all three and jumps to arg. otherwise leaves the stack alone so CALL + STORE run as usual
    APPEND_ARR,
    // compiled from `&name[k1]..[kn]` in a call argument. operand = name, slot = local slot or -1, arg = n.
    // pops the n keys, pushes a reference path (value.h). the CALL that follows (flags kHasRef) turns it into a reference
    REF_PATH,

    // destructuring
    // pops one array/map, pushes operand values 
    // array: first N elements, error if too few
    // map: operand must be 2, pushes (keys array, values array)
    // pushed in reverse so the first target ends up on TOS, ready for a following DEFINE/DEFINE_TYPED/STORE
    UNPACK,

    // for loops
    // ITER_INIT pops the iterable and pushes iterator state onto the current frame's private iterator stack
    // operand = 1 or 2, the number of loop variables
    // ITER_NEXT pushes that many values (2 = key then value, value on TOS) and advances
    // once exhausted jumps to operand without pushing anything
    // ITER_POP discards the iterator state. emitted once at the loop's single exit point, shared by both natural exhaustion and break
    ITER_INIT,
    ITER_NEXT,
    ITER_POP,

    // callables
    MAKE_CLOSURE,   // operand = function-table index; snapshots current locals -> pushes callable
    CALL,           // operand = arg count (or -1 = import sentinel); stack: [callee, arg0..argN]
    RETURN,         // pops return value, unwinds frame

    // async / plugin injection
    YIELD,          // suspend this runner; return control to caller of step()
    NOP,            // no-op; useful as a patch target

    // scope management (name erased only)
    SCOPE_PUSH,
    SCOPE_POP,

    // try/catch
    // operand on TRY_PUSH = ip of the catch block's own SCOPE_PUSH patched like a jump target
    // see VmRunner::handleTry()
    TRY_PUSH,
    TRY_POP,

    // made when a program starts to run (see quickenChunk), never by the compiler and never written to a compiled file. each
    // one stands in for the first instruction of a short run of ordinary ones, does the whole job when the locals it names
    // are ints, and otherwise runs the ordinary instructions it replaced (the originals stay in place, so a jump into the
    // middle of the run still works). flags = which comparison or arithmetic op, see fuseCmp / fuseArith
    F_CMP_LI_JIF,    // LOAD local; PUSH_INT k; <cmp>; JUMP_IF_FALSE
    F_CMP_LL_JIF,    // LOAD local; LOAD local; <cmp>; JUMP_IF_FALSE
    F_CMP_JIF,       // <cmp>; JUMP_IF_FALSE                      (both operands on the stack)
    F_ARITH_LI,      // LOAD local; PUSH_INT k; <arith>
    F_ARITH_LL,      // LOAD local; LOAD local; <arith>
    F_ARITH_LI_ST,   // LOAD local; PUSH_INT k; <arith>; STORE local   (operand = the local stored to)
    F_ARITH_LL_ST,   // LOAD local; LOAD local; <arith>; STORE local
    F_ARITH_ST,      // <arith>; STORE local                      (both operands on the stack)
    // the same for globals (top-level code). they need the chunk's lookup cache to be warm for every global they name, and
    // otherwise run the ordinary instructions, which warm it, so only the first turn of a loop is slow
    F_CMP_GI_JIF,    // LOAD_GLOBAL; PUSH_INT k; <cmp>; JUMP_IF_FALSE
    F_CMP_GG_JIF,    // LOAD_GLOBAL; LOAD_GLOBAL; <cmp>; JUMP_IF_FALSE
    F_ARITH_GI,      // LOAD_GLOBAL; PUSH_INT k; <arith>
    F_ARITH_GG,      // LOAD_GLOBAL; LOAD_GLOBAL; <arith>
    F_ARITH_GI_STG,  // LOAD_GLOBAL; PUSH_INT k; <arith>; STORE_GLOBAL
    F_ARITH_STG,     // <arith>; STORE_GLOBAL                     (both operands on the stack)
    // an array element read: the index is a local, a global, or on the stack. the array is the variable INDEX_GET_VAR names
    F_IDX_L,         // LOAD local; INDEX_GET_VAR
    F_IDX_G,         // LOAD_GLOBAL; INDEX_GET_VAR
    F_IDX,           // INDEX_GET_VAR
    // the head of a `for x in v` loop over an array or a range: take the next element (or leave the loop), open the body's scope, define the variable
    F_ITER_DEFINE,   // ITER_NEXT; SCOPE_PUSH; DEFINE local
};

// 16 bytes, so a chunk's code is a flat array that can be written out and read back as it is.
// the double and int64 payloads of PUSH_NUM / PUSH_INT live in the chunk (nums, ints) and `operand` indexes them.
// the source line is in the chunk too (CompiledChunk::lines), so only error paths look at it
struct Instruction {
    Op      op       = Op::NOP;
    uint8_t flags    = 0;                // kStash / kCallFree / kHasRef, for the variable-in-place ops above
    TypeSet typeHint = TypeSet::Any();   // payload for DEFINE_TYPED
    int32_t operand  = 0;                // string-table index, jump target, count, fn index, or a nums/ints index
    // local slot picked at compile time for LOAD/STORE/DEFINE/DEFINE_TYPED
    // (CompiledChunk::internLocal). -1 for every other opcode and for the *_GLOBAL variants
    int32_t slot     = -1;
    int32_t arg      = 0;                // second payload: jump target, pureSets index, path length (see the ops above)
};
static_assert(sizeof(Instruction) == 16, "keep Instruction at 16 bytes");

// Instruction::flags
constexpr int kStash    = 1;   // a STASH_VAR result sits under the operands
constexpr int kCallFree = 2;   // the code between STASH_VAR and the op that uses it can't run script code
constexpr int kHasRef   = 8;   // CALL: some argument is a REF_PATH
// LOAD / LOAD_GLOBAL of the function a call names (`f(x)`): when it holds a function, the load pushes a mark that points at where it is, instead of a
// counted copy, so a call doesn't count a reference up and down. see VmRunner::calleeMark
constexpr int kCallee   = 0x80;
// STORE / STORE_GLOBAL: typeHint, when it is not `any`, is the type of the variable, so the store tests the tag of the value
// right here instead of looking the variable's type up. kProven: the compiler worked out that the value fits, no test at all
constexpr int kProven   = 32;
// DEFINE / DEFINE_GLOBAL: record the exact type of the value as the variable's type (what a plain assignment does)
constexpr int kAuto     = 16;
// special values of Instruction::slot, for names that are not a plain local slot of the running function (see Compiler::resolveNames)
//   slot >= 0           a local that nothing captures: CallFrame::slots[slot]
//   kFreeSlot           no function around has the name either: a global, unless a module layer was captured
//   kBoxedBase - n      a local that an inner function captures: its cell is CallFrame::cells[n]
//   kUpvalBase - n      a variable of an enclosing function: CaptureFrame::upvals[n]
constexpr int kFreeSlot  = -3;
constexpr int kBoxedBase = -1000;
constexpr int kUpvalBase = -1000000;
inline bool isBoxedSlot(int s) { return s <= kBoxedBase && s > kUpvalBase; }
inline bool isUpvalSlot(int s) { return s <= kUpvalBase; }
inline int  boxedIndex(int s)  { return kBoxedBase - s; }
inline int  upvalIndex(int s)  { return kUpvalBase - s; }
constexpr int kPureCalls = 4;  // ... as long as the builtins it calls still are builtins, see CompiledChunk::pureSets


// compiled representation

// one compiled function body owned by CompiledProgram
// pointed to by closures via shared_ptr so the program can be discarded while closures are still live
struct CompiledChunk {
    std::string              name;
    std::vector<Param>       params;
    TypeSet                  returnType = TypeSet::Any();
    std::vector<Instruction> code;
    std::vector<int>         lines;     // source line of each instruction in `code`, same index
    std::vector<double>      nums;      // PUSH_NUM constants
    std::vector<int64_t>     ints;      // PUSH_INT constants
    // where on its line the expression behind each instruction sits (1-based columns, 0 = unknown). only the compiler fills
    // it, so a chunk loaded from a .vmbr has none and its errors show the line without a caret
    struct ColSpan { uint16_t start = 0, end = 0; };
    std::vector<ColSpan>     cols;
    SourceRange rangeAt(int ip) const {
        int line = lineAt(ip);
        if (ip >= 0 && ip < (int)cols.size() && cols[(size_t)ip].start > 0) return {line, cols[(size_t)ip].start, line, cols[(size_t)ip].end};
        return {line, 0, line, 0};
    }
    int lineAt(int ip) const { return ip >= 0 && ip < (int)lines.size() ? lines[(size_t)ip] : 0; }
    std::vector<std::string> strings;   // string table local to this chunk
    SourceRange              definedAt;
    std::string              sourceFile;

    // every function chunk from the same compile() call, this one included. MAKE_CLOSURE looks its function up here,
    // not in the program running right now: a closure exported from a module can run long after that module's program is gone
    // a chunk holds a pointer to itself on purpose, so compiled chunks are never freed
    std::shared_ptr<std::vector<std::shared_ptr<CompiledChunk>>> siblings;

    // every name this chunk defines or assigns locally (not through a *_GLOBAL opcode) gets a slot number the first time
    // the compiler sees it. a frame keeps its locals in a flat array indexed by that number, so LOAD/STORE don't hash
    // the name at runtime. see CallFrame::slots and CallFrame::locals
    std::unordered_map<std::string, int> localSlots;
    int numLocalSlots = 0;
    // the instructions a fused op (F_*) replaced, by Instruction::arg. only filled once the chunk has been quickened
    std::vector<Instruction> quickOrig;
    bool quickened = false;
    // the slot (or boxed code) of each parameter, found the first time the function is called so a call doesn't hash names
    mutable std::vector<int> paramSlots;
    void fillParamSlots() const { paramSlots.clear(); for (const Param& p : params) paramSlots.push_back(slotOf(p.name)); }
    // 0 = not looked at yet, 1 = a call with exactly the declared number of arguments can use the short way in (no rest parameter, no
    // parameter types, no captured locals), 2 = it can't. see VmRunner::pushFrameSimple
    mutable uint8_t callKind = 0;
    mutable bool typedParams = false;   // some parameter has a declared type, see argsFit
    void classifyCall() const {
        if (paramSlots.size() != params.size()) fillParamSlots();
        bool simple = !hasBoxed;
        typedParams = false;
        for (size_t i = 0; i < params.size() && simple; ++i) {
            if (params[i].variadic || paramSlots[i] < 0 || paramSlots[i] >= numLocalSlots) simple = false;
            if (params[i].elem && params[i].elem->isFn) simple = false;     // a function type is checked for its number of arguments when called
            if (!params[i].type.isAny()) typedParams = true;
        }
        callKind = simple ? 1 : 2;
    }
    // true when the arguments fit the declared parameter types (always, with none declared). a call they don't fit goes the
    // general way, which raises the error
    [[gnu::always_inline]] bool argsFit(const Value* args) const { return !typedParams || argsFitTyped(args); }
    [[gnu::noinline]] bool argsFitTyped(const Value* args) const {
        for (size_t i = 0; i < params.size(); ++i)
            if (!params[i].type.contains(args[i].tag())) return false;
        return true;
    }

    // the chunk this function was written inside (an index into the program's chunks), -1 for the top-level chunk
    int parentIdx = -1;

    // the variables of enclosing functions this function (or one inside it) uses, numbered in the order the compiler found them.
    // when MAKE_CLOSURE runs in the parent, each entry says where to get the cell: one of the parent's own boxed locals, or
    // one of the parent's own upvalues. set by Compiler::resolveNames
    struct UpvalDesc { bool fromParentLocal; int index; };
    std::vector<UpvalDesc>               upvalDescs;
    std::unordered_map<std::string, int> upvalNames;   // name -> index, for the lookups that go by name
    bool hasBoxed = false;                              // some local is captured, so a frame needs a cells array

    // for STASH_VAR with kPureCalls: the string-table indexes of the builtins (str, len, ...) the key / value code calls.
    // they are only safe to ignore while each one still resolves to a native function, which STASH_VAR checks when it runs
    std::vector<std::vector<int>> pureSets;

    int internLocal(const std::string& name) {
        auto it = localSlots.find(name);
        if (it != localSlots.end()) return it->second;
        int slot = numLocalSlots++;
        localSlots.emplace(name, slot);
        return slot;
    }

    // the slot code for `name` (see the constants above: a plain slot or a boxed one), -1 if it is not a local of this chunk
    int slotOf(const std::string& name) const {
        auto it = localSlots.find(name);
        return it != localSlots.end() ? it->second : -1;
    }

    // intern a string, return its index
    int intern(const std::string& s) {
        for (int i = 0; i < (int)strings.size(); ++i)
            if (strings[i] == s) return i;
        strings.push_back(s);
        return (int)strings.size() - 1;
    }

    // LOAD_GLOBAL / STORE_GLOBAL remember where a global lives, so a loop doesn't hash its name on every pass.
    // `epoch` is the Interpreter::scopeEpoch() the pointer was found under; once it differs, look the name up again
    struct GlobalCache {
        Value*   ptr   = nullptr;
        uint64_t epoch = 0;
        bool     typed = false;   // the variable has a type (declared, or from its first value)
        TypeSet  type  = TypeSet::Any();   // that type. a store tests the value against it right here
    };
    mutable std::vector<GlobalCache> globalCache;   // one per string-table entry, filled in as it is used
    [[gnu::always_inline]] GlobalCache& globalCacheFor(int idx) const {
        if (globalCache.size() != strings.size()) [[unlikely]] sizeGlobalCache();
        return globalCache[idx];
    }
    [[gnu::noinline]] void sizeGlobalCache() const { globalCache.resize(strings.size()); }

    // what the hot loop reads of a chunk, found once. a call or a return takes the whole struct in one copy instead of looking each piece up. built the
    // first time the chunk runs, after it is quickened (quickening changes instructions in place), see buildFast
    struct Fast {
        const Instruction* code = nullptr;
        const Instruction* codeEnd = nullptr;
        const int64_t* ints = nullptr;
        const double* nums = nullptr;
        GlobalCache* gcs = nullptr;
        const std::string* strs = nullptr;
        size_t nstrs = 0;
        const int* lines = nullptr;   // one per instruction, null when the chunk has fewer
    } fast;
    bool fastReady = false;
    [[gnu::noinline]] void buildFast() {
        if (globalCache.size() != strings.size()) sizeGlobalCache();
        fast.code = code.data(); fast.codeEnd = code.data() + code.size();
        fast.ints = ints.data(); fast.nums = nums.data();
        fast.gcs = globalCache.data();
        fast.strs = strings.data(); fast.nstrs = strings.size();
        fast.lines = lines.size() == code.size() ? lines.data() : nullptr;
        fastReady = true;
    }

    // PUSH_STR pushes a copy of a Value made once per literal: a string is a shared immutable box, so a copy costs one count and the
    // allocation and copy of the text happens the first time only
    mutable std::vector<Value>   strLits;
    mutable std::vector<uint8_t> strLitSet;
    [[gnu::always_inline]] const Value& strValue(int idx) const {
        if (strLits.size() != strings.size()) [[unlikely]] { strLits.assign(strings.size(), Value()); strLitSet.assign(strings.size(), 0); }
        if (!strLitSet[(size_t)idx]) [[unlikely]] { strLits[(size_t)idx] = Value(strings[(size_t)idx]); strLitSet[(size_t)idx] = 1; }
        return strLits[(size_t)idx];
    }

    const std::string& str(int idx) const {
        static const std::string empty;
        if (idx < 0 || idx >= (int)strings.size()) return empty;
        return strings[idx];
    }
};

// full output of a compilation run
struct CompiledProgram {
    // index 0 = top-level chunk
    std::vector<std::shared_ptr<CompiledChunk>> chunks;
    const CompiledChunk& top() const { return *chunks[0]; }
};

// ---- fused instructions -------------------------------------------------------------------------------------------------
// a number a fused op reads: an int or a float
struct FuseNum { bool isInt = true; int64_t i = 0; double d = 0.0; };
[[gnu::always_inline]] inline bool fuseNumOf(const Value& v, FuseNum& n) {
    if (v.isInt())    { n.isInt = true;  n.i = v.asInt();    return true; }
    if (v.isNumber()) { n.isInt = false; n.d = v.asNumber(); return true; }
    return false;
}
// the arithmetic of two numbers, kind 0 + 1 - 2 * 3 % 4 /. false: the ordinary instructions have to run (an int result that does
// not fit widens to a float, a modulo by zero or of floats and a division by zero are the ordinary instructions' business)
[[gnu::always_inline]] inline bool fuseArithNum(int kind, const FuseNum& a, const FuseNum& b, Value& out) {
    if (a.isInt && b.isInt) {
        int64_t r;
        switch (kind) {
            case 0:  if (__builtin_add_overflow(a.i, b.i, &r)) return false; break;
            case 1:  if (__builtin_sub_overflow(a.i, b.i, &r)) return false; break;
            case 2:  if (__builtin_mul_overflow(a.i, b.i, &r)) return false; break;
            case 3:  if (b.i == 0) return false; r = (b.i == -1) ? 0 : a.i % b.i; break;
            default: if (b.i == 0) return false; out = Value((double)a.i / (double)b.i); return true;
        }
        out = Value(r);
        return true;
    }
    const double x = a.isInt ? (double)a.i : a.d, y = b.isInt ? (double)b.i : b.d;
    switch (kind) {
        case 0:  out = Value(x + y); return true;
        case 1:  out = Value(x - y); return true;
        case 2:  out = Value(x * y); return true;
        case 4:  if (y == 0.0) return false; out = Value(x / y); return true;
        default: return false;
    }
}
// a comparison of two numbers, kind 0 < 1 > 2 <= 3 >= 4 == 5 !=. == and != only for two ints (an int and a float are the ordinary
// instructions' business). false: run the ordinary instructions
[[gnu::always_inline]] inline bool fuseCmpNum(int kind, const FuseNum& a, const FuseNum& b, bool& yes) {
    if (kind >= 4) {
        if (!(a.isInt && b.isInt)) return false;
        yes = kind == 4 ? a.i == b.i : a.i != b.i;
        return true;
    }
    if (a.isInt && b.isInt) {
        switch (kind) { case 0: yes = a.i < b.i; break; case 1: yes = a.i > b.i; break; case 2: yes = a.i <= b.i; break; default: yes = a.i >= b.i; }
        return true;
    }
    const double x = a.isInt ? (double)a.i : a.d, y = b.isInt ? (double)b.i : b.d;
    switch (kind) { case 0: yes = x < y; break; case 1: yes = x > y; break; case 2: yes = x <= y; break; default: yes = x >= y; }
    return true;
}
// the usual case of a fused op, done on the bare values: two ints (or two floats), nothing else. kinds as in fuseArithNum / fuseCmpNum.
// false: the general code runs, which gives the same answer for every case this one takes
struct NumBits { uint8_t tg; Value::Bits u; };
[[gnu::always_inline]] inline bool fastArith(int kind, const NumBits& a, const NumBits& b, NumBits& r) {
    if (a.tg == 1 && b.tg == 1) {
        switch (kind) {
            case 0:  if (__builtin_add_overflow(a.u.i, b.u.i, &r.u.i)) return false; break;
            case 1:  if (__builtin_sub_overflow(a.u.i, b.u.i, &r.u.i)) return false; break;
            case 2:  if (__builtin_mul_overflow(a.u.i, b.u.i, &r.u.i)) return false; break;
            case 3:  if (b.u.i == 0) return false; r.u.i = (b.u.i == -1) ? 0 : a.u.i % b.u.i; break;
            default: return false;
        }
        r.tg = 1;
        return true;
    }
    if (a.tg == 0 && b.tg == 0) {
        switch (kind) {
            case 0:  r.u.d = a.u.d + b.u.d; break;
            case 1:  r.u.d = a.u.d - b.u.d; break;
            case 2:  r.u.d = a.u.d * b.u.d; break;
            case 4:  if (b.u.d == 0.0) return false; r.u.d = a.u.d / b.u.d; break;
            default: return false;
        }
        r.tg = 0;
        return true;
    }
    return false;
}
[[gnu::always_inline]] inline bool fastCmp(int kind, const NumBits& a, const NumBits& b, bool& yes) {
    if (a.tg == 1 && b.tg == 1) {
        switch (kind) { case 0: yes = a.u.i < b.u.i; break; case 1: yes = a.u.i > b.u.i; break; case 2: yes = a.u.i <= b.u.i; break;
                        case 3: yes = a.u.i >= b.u.i; break; case 4: yes = a.u.i == b.u.i; break; default: yes = a.u.i != b.u.i; }
        return true;
    }
    return false;
}
inline int fuseCmpKind(Op op) {
    switch (op) {
        case Op::LT: return 0; case Op::GT: return 1; case Op::LTE: return 2;
        case Op::GTE: return 3; case Op::EQ: return 4; case Op::NEQ: return 5;
        default: return -1;
    }
}
inline int fuseArithKind(Op op) {
    switch (op) {
        case Op::ADD: return 0; case Op::SUB: return 1; case Op::MUL: return 2; case Op::MOD: return 3; case Op::DIV: return 4;
        default: return -1;
    }
}
// Instruction::flags of a fused op: the low three bits are the kind above, this one says the constant is a PUSH_NUM (a float)
constexpr int kFuseFloatConst = 0x40;
// the switch is off with EMBR_NO_FUSE=1, or by setting this, so the fuzzer can run a program both ways and compare
inline bool& fuseOpsEnabled() {
    static bool on = []{ const char* e = std::getenv("EMBR_NO_FUSE"); return !(e && *e && *e != '0'); }();
    return on;
}
// EMBR_STATS=1 counts, for each instruction, how often the hot loop (VmRunner::fastRun) stopped at it and left it to the slower
// path, and prints the counts when the program ends (printBailStats, vm_disasm.h). to find what the hot loop should learn next
inline void printBailStats();
inline std::atomic<uint64_t>* bailCounts() {
    static std::atomic<uint64_t> counts[256];
    return counts;
}
inline bool bailStatsOn() {
    static bool on = []{
        const char* e = std::getenv("EMBR_STATS");
        bool yes = e && *e && *e != '0';
        if (yes) std::atexit([]{ printBailStats(); });
        return yes;
    }();
    return on;
}

// the same for the hot loop (VmRunner::fastRun): EMBR_NO_FAST=1, or this, runs every instruction the ordinary way. read when a runner is made
inline bool& fastLoopEnabled() {
    static bool on = []{ const char* e = std::getenv("EMBR_NO_FAST"); return !(e && *e && *e != '0'); }();
    return on;
}

// turns the short runs of ordinary instructions that loops and calls are made of into one fused instruction each (see the F_*
// ops). nothing moves: the first instruction of a run is replaced and the rest stay where they are, so every jump target and
// line number is still right. the replaced one is kept in quickOrig for the case the fused op can't do the job. safe to call
// again, and for any chunk that passed validateProgram (it only looks at ops, slots and jump targets, and never changes the meaning)
inline void quickenChunk(CompiledChunk& c) {
    if (c.quickened) return;
    c.quickened = true;
    if (!fuseOpsEnabled()) return;
    auto& code = c.code;
    const size_t n = code.size();
    auto loadLocal = [&](size_t i) { return i < n && code[i].op == Op::LOAD && code[i].slot >= 0; };
    auto loadGlobal = [&](size_t i) { return i < n && code[i].op == Op::LOAD_GLOBAL; };
    auto storeGlobal = [&](size_t i) { return i < n && code[i].op == Op::STORE_GLOBAL; };
    // 0 = not a constant, 1 = PUSH_INT, 2 = PUSH_NUM
    auto constAt   = [&](size_t i) { return i < n ? (code[i].op == Op::PUSH_INT ? 1 : code[i].op == Op::PUSH_NUM ? 2 : 0) : 0; };
    auto cmpAt     = [&](size_t i) { return i < n ? fuseCmpKind(code[i].op) : -1; };
    auto arithAt   = [&](size_t i) { return i < n ? fuseArithKind(code[i].op) : -1; };
    auto jif       = [&](size_t i) { return i < n && code[i].op == Op::JUMP_IF_FALSE; };
    auto idxVar    = [&](size_t i) { return i < n && code[i].op == Op::INDEX_GET_VAR && !(code[i].flags & kStash); };
    // a store to a local that carries its variable's type, so the fused op tests the result's type against it on the spot
    auto typedStore = [&](size_t i) { return i < n && code[i].op == Op::STORE && code[i].slot >= 0 && !code[i].typeHint.isAny(); };
    constexpr int kKeep = INT32_MIN;   // fuse(): leave `operand` as the first instruction had it
    auto fuse = [&](size_t i, Op op, int kind, int operand, TypeSet hint = TypeSet::Any()) {
        Instruction f = code[i];
        c.quickOrig.push_back(code[i]);
        f.op = op; f.flags = (uint8_t)kind; if (operand != kKeep) f.operand = operand; f.typeHint = hint;
        f.arg = (int32_t)(c.quickOrig.size() - 1);
        code[i] = f;
    };
    for (size_t i = 0; i < n; ) {
        int k, cs;
        if (loadLocal(i) && (cs = constAt(i + 1)) && (k = cmpAt(i + 2)) >= 0 && jif(i + 3))    { fuse(i, Op::F_CMP_LI_JIF, k | (cs == 2 ? kFuseFloatConst : 0), 0); i += 4; continue; }
        if (loadLocal(i) && loadLocal(i + 1) && (k = cmpAt(i + 2)) >= 0 && jif(i + 3))        { fuse(i, Op::F_CMP_LL_JIF, k, 0); i += 4; continue; }
        if (loadLocal(i) && (cs = constAt(i + 1)) && (k = arithAt(i + 2)) >= 0) {
            const int fl = k | (cs == 2 ? kFuseFloatConst : 0);
            if (typedStore(i + 3)) { fuse(i, Op::F_ARITH_LI_ST, fl, code[i + 3].slot, code[i + 3].typeHint); i += 4; continue; }
            fuse(i, Op::F_ARITH_LI, fl, 0); i += 3; continue;
        }
        if (loadLocal(i) && loadLocal(i + 1) && (k = arithAt(i + 2)) >= 0) {
            if (typedStore(i + 3)) { fuse(i, Op::F_ARITH_LL_ST, k, code[i + 3].slot, code[i + 3].typeHint); i += 4; continue; }
            fuse(i, Op::F_ARITH_LL, k, 0); i += 3; continue;
        }
        if (loadGlobal(i) && (cs = constAt(i + 1)) && (k = cmpAt(i + 2)) >= 0 && jif(i + 3))   { fuse(i, Op::F_CMP_GI_JIF, k | (cs == 2 ? kFuseFloatConst : 0), kKeep); i += 4; continue; }
        if (loadGlobal(i) && loadGlobal(i + 1) && (k = cmpAt(i + 2)) >= 0 && jif(i + 3))      { fuse(i, Op::F_CMP_GG_JIF, k, kKeep); i += 4; continue; }
        if (loadGlobal(i) && (cs = constAt(i + 1)) && (k = arithAt(i + 2)) >= 0) {
            const int fl = k | (cs == 2 ? kFuseFloatConst : 0);
            if (storeGlobal(i + 3)) { fuse(i, Op::F_ARITH_GI_STG, fl, kKeep); i += 4; continue; }
            fuse(i, Op::F_ARITH_GI, fl, kKeep); i += 3; continue;
        }
        if (loadGlobal(i) && loadGlobal(i + 1) && (k = arithAt(i + 2)) >= 0)                  { fuse(i, Op::F_ARITH_GG, k, kKeep); i += 3; continue; }
        if (loadLocal(i) && idxVar(i + 1))                                                    { fuse(i, Op::F_IDX_L, 0, 0); i += 2; continue; }
        if (loadGlobal(i) && idxVar(i + 1))                                                   { fuse(i, Op::F_IDX_G, 0, kKeep); i += 2; continue; }
        if ((k = arithAt(i)) >= 0 && storeGlobal(i + 1))                                      { fuse(i, Op::F_ARITH_STG, k, kKeep); i += 2; continue; }
        if ((k = cmpAt(i)) >= 0 && jif(i + 1))                                                { fuse(i, Op::F_CMP_JIF, k, 0); i += 2; continue; }
        if ((k = arithAt(i)) >= 0 && typedStore(i + 1))                                       { fuse(i, Op::F_ARITH_ST, k, code[i + 1].slot, code[i + 1].typeHint); i += 2; continue; }
        if (idxVar(i))                                                                        { fuse(i, Op::F_IDX, 0, kKeep); i += 1; continue; }
        if (i + 2 < n && code[i].op == Op::ITER_NEXT && code[i + 1].op == Op::SCOPE_PUSH && code[i + 2].op == Op::DEFINE &&
            code[i + 2].slot >= 0 && !(code[i + 2].flags & kAuto)) {
            fuse(i, Op::F_ITER_DEFINE, 0, kKeep); code[i].slot = code[i + 2].slot; i += 3; continue;
        }
        ++i;
    }
}
inline void quickenProgram(const CompiledProgram& prog) { for (const auto& c : prog.chunks) quickenChunk(*c); }

// the chunk's code as the compiler wrote it, for the places that read or write instructions (the file writer, the disassembler,
// the checkers): a quickened chunk has the fused ops put back. `tmp` holds the copy when one is needed
inline const std::vector<Instruction>& plainCode(const CompiledChunk& c, std::vector<Instruction>& tmp) {
    if (c.quickOrig.empty()) return c.code;
    tmp = c.code;
    for (Instruction& in : tmp) if (in.op >= Op::F_CMP_LI_JIF) in = c.quickOrig[(size_t)in.arg];
    return tmp;
}

// a local the compiler made for an inline call is its parameter's name, a \x01 and a number. errors show the parameter's name
inline std::string shownName(const std::string& n) { return n.substr(0, n.find('\x01')); }

class Compiler {
public:
    struct Options {
        // the ops that change a variable where it lives (APPEND_VAR, INDEX_GET_VAR, INDEX_SET_VAR, STASH_VAR,
        // APPEND_ARR) are what makes `arr[i] = x` and `s = s + x` cheap. off, the compiler uses the plain
        // load / change / store ops instead, which is slower but simpler. the fuzzer compiles every program both ways
        // and compares the answers, so a bug in one of the fast ops shows up as a difference
        bool inPlaceOps = true;
        // use what the type checker left on the syntax tree: stores that carry their variable's type, and stores it proved need no
        // test (AssignStmt::storeHint). off, every store looks the variable's type up by name, which is slower but needs nothing
        bool useTypes = true;
        // leave out the SCOPE_PUSH / SCOPE_POP of a block that cannot make a name of its own (see blockNeedsScope). off, every block has them.
        // the fuzzer compiles every program both ways and compares the answers
        bool elideScopes = true;
    };
    Options options;

    // the Interpreter is reserved for a LOAD vs LOAD_GLOBAL optimisation
    explicit Compiler(const Interpreter* interp = nullptr) : interp_(interp) {}

    CompiledProgram compile(const std::vector<StmtPtr>& ast,
                            const std::string& filename = "<input>") {
        prog_ = CompiledProgram{};
        // top-level chunk
        auto top = std::make_shared<CompiledChunk>();
        top->name       = "<top>";
        top->sourceFile = filename;
        prog_.chunks.push_back(top);
        cur_ = top.get();
        binds_.clear(); inlineFns_.clear(); tmplScans_.clear(); consts_.clear(); tmpl_.clear(); renames_ = nullptr; expanding_.clear();
        scanBlock(ast, false);
        for (auto& s : ast) compileStmt(s.get());
        cur_ = nullptr;
        resolveNames();

        // point every chunk's siblings at the same shared list before any of them can escape as a Value
        // (MAKE_CLOSURE needs it, see CompiledChunk::siblings)
        auto siblings = std::make_shared<std::vector<std::shared_ptr<CompiledChunk>>>(prog_.chunks);
        for (auto& c : prog_.chunks) c->siblings = siblings;

        return std::move(prog_);
    }

private:
    const Interpreter* interp_;   // the natives already registered, for a const that calls a pure one. may be null
    CompiledProgram       prog_;
    CompiledChunk*        cur_  = nullptr;  // current chunk being written

    // compile-time mirror of the SCOPE_PUSH/SCOPE_POP nesting depth emitted so far in the current chunk, so
    // break/continue know how many SCOPE_POPs to backfill before jumping out of the scopes between them and their loop
    int scopeDepth_ = 0;

    // nesting depth of try statements, so break/continue know how many TRY_POPs to backfill when jumping out of
    // a try block that sits inside the loop
    int tryDepth_ = 0;

    struct LoopCtx {
        std::vector<int> breakJumps;   // JUMP instrs to patch once the loop's single exit point is known
        int continueTarget;            // instruction index 'continue' jumps to (the loop's re-check point)
        int scopeDepthAtEntry;         // scopeDepth_ when this loop's body started
        int tryDepthAtEntry;           // tryDepth_ when this loop's body started
        bool isFor;                    // a for loop has an iterator to let go of when a const fn returns from inside it
    };
    std::vector<LoopCtx> loopStack_;

    // the source range of the node being compiled, so each instruction can say which part of its line it came from
    SourceRange curRange_;
    struct RangeScope {
        Compiler& c; SourceRange saved;
        RangeScope(Compiler& cc, const SourceRange& r) : c(cc), saved(cc.curRange_) { c.curRange_ = r; }
        ~RangeScope() { c.curRange_ = saved; }
    };
    void pushCol(int line) {
        CompiledChunk::ColSpan sp;
        if (curRange_.startLine == line && curRange_.startCol > 0 && curRange_.startCol < 65535) {
            sp.start = (uint16_t)curRange_.startCol;
            // a range that goes on to later lines is underlined to the end of its first line
            sp.end = curRange_.endLine == line ? (uint16_t)std::min(curRange_.endCol, 65535) : (uint16_t)65535;
        }
        cur_->cols.push_back(sp);
    }

    // ---- functions written out at their calls (FnStmt::isExprForm) -----------------------------------------------------------------------
    // a call by name of a function written as `name(params) = expr` (FnStmt::isExprForm) becomes the expression, written where the
    // call is. the function is still defined as a variable as usual, so it can be passed around and called any other way. to be sure the name means
    // the same function at the call, it must be bound once in the whole file, and every other name the expression reads must
    // not be bound inside any function or block (else the call site could have its own variable of that name)
    struct BindCount { int top = 0, nested = 0; };
    std::unordered_map<std::string, BindCount> binds_;
    std::unordered_map<std::string, const FnStmt*> inlineFns_;
    // while an expansion is compiled: each parameter name stands for a local (names) or for a literal written at the call (lits)
    struct Renames {
        std::unordered_map<std::string, std::string> names;
        std::unordered_map<std::string, const Expr*> lits;
    };
    const Renames* renames_ = nullptr;
    std::vector<const FnStmt*> expanding_;
    int inlineSerial_ = 0;

    void bindName(const std::string& n, bool nested) { (nested ? binds_[n].nested : binds_[n].top)++; }
    void scanBlock(const std::vector<StmtPtr>& b, bool nested) { for (auto& s : b) scanStmt(s.get(), nested); }
    void scanExpr(const Expr* e) {
        switch (e->kind) {
        case Expr::Kind::Unary:  scanExpr(static_cast<const UnaryExpr*>(e)->right.get()); break;
        case Expr::Kind::Binary: scanExpr(static_cast<const BinaryExpr*>(e)->left.get()); scanExpr(static_cast<const BinaryExpr*>(e)->right.get()); break;
        case Expr::Kind::And: case Expr::Kind::Or:
            scanExpr(static_cast<const LogicExpr*>(e)->left.get()); scanExpr(static_cast<const LogicExpr*>(e)->right.get()); break;
        case Expr::Kind::Index:  scanExpr(static_cast<const IndexExpr*>(e)->object.get()); scanExpr(static_cast<const IndexExpr*>(e)->index.get()); break;
        case Expr::Kind::Ref:    scanExpr(static_cast<const RefExpr*>(e)->target.get()); break;
        case Expr::Kind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            scanExpr(c->callee.get());
            for (auto& a : c->args) scanExpr(a.get());
            break;
        }
        case Expr::Kind::Array: for (auto& x : static_cast<const ArrayExpr*>(e)->elements) scanExpr(x.get()); break;
        case Expr::Kind::Map:   for (auto& kv : static_cast<const MapExpr*>(e)->entries) { scanExpr(kv.first.get()); scanExpr(kv.second.get()); } break;
        case Expr::Kind::Lambda: {
            auto* l = static_cast<const LambdaExpr*>(e);
            for (auto& p : l->params) bindName(p.name, true);
            scanBlock(l->ownedBody, true);
            break;
        }
        default: break;
        }
    }
    void scanStmt(const Stmt* s, bool nested) {
        switch (s->kind) {
        case Stmt::Kind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            if (a->target->kind == Expr::Kind::Var) bindName(static_cast<const VarExpr*>(a->target.get())->name, nested);
            else scanExpr(a->target.get());
            scanExpr(a->value.get());
            break;
        }
        case Stmt::Kind::MultiAssign: {
            auto* m = static_cast<const MultiAssignStmt*>(s);
            for (auto& n : m->names) bindName(n, nested);
            scanExpr(m->value.get());
            break;
        }
        case Stmt::Kind::Local: {
            auto* l = static_cast<const LocalStmt*>(s);
            for (auto& t : l->targets) bindName(t.name, nested);
            if (l->value) scanExpr(l->value.get());
            break;
        }
        case Stmt::Kind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            scanExpr(i->cond.get()); scanBlock(i->thenBlock, true); scanBlock(i->elseBlock, true);
            break;
        }
        case Stmt::Kind::Fn: {
            auto* f = static_cast<const FnStmt*>(s);
            bindName(f->name, nested);
            for (auto& p : f->params) bindName(p.name, true);
            scanBlock(f->body, true);
            break;
        }
        case Stmt::Kind::Return: { auto* r = static_cast<const ReturnStmt*>(s); if (r->value) scanExpr(r->value.get()); break; }
        case Stmt::Kind::While:  { auto* w = static_cast<const WhileStmt*>(s); scanExpr(w->cond.get()); scanBlock(w->body, true); break; }
        case Stmt::Kind::For: {
            auto* f = static_cast<const ForStmt*>(s);
            if (!f->keyName.empty()) bindName(f->keyName, true);
            bindName(f->varName, true);
            scanExpr(f->iterable.get()); scanBlock(f->body, true);
            break;
        }
        case Stmt::Kind::Expr:   scanExpr(static_cast<const ExprStmt*>(s)->expr.get()); break;
        case Stmt::Kind::Try: {
            auto* t = static_cast<const TryStmt*>(s);
            bindName(t->catchVar, true);
            scanBlock(t->tryBlock, true); scanBlock(t->catchBlock, true);
            break;
        }
        case Stmt::Kind::Import: scanExpr(static_cast<const ImportStmt*>(s)->pathExpr.get()); break;
        default: break;
        }
    }
    static constexpr int kInlineMaxSize = 40;   // nodes in the expression, so a big one is not copied into every call
    static int exprSize(const Expr* e) {
        int n = 1;
        switch (e->kind) {
        case Expr::Kind::Unary:  n += exprSize(static_cast<const UnaryExpr*>(e)->right.get()); break;
        case Expr::Kind::Binary: n += exprSize(static_cast<const BinaryExpr*>(e)->left.get()) + exprSize(static_cast<const BinaryExpr*>(e)->right.get()); break;
        case Expr::Kind::And: case Expr::Kind::Or:
            n += exprSize(static_cast<const LogicExpr*>(e)->left.get()) + exprSize(static_cast<const LogicExpr*>(e)->right.get()); break;
        case Expr::Kind::Index:  n += exprSize(static_cast<const IndexExpr*>(e)->object.get()) + exprSize(static_cast<const IndexExpr*>(e)->index.get()); break;
        case Expr::Kind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            n += exprSize(c->callee.get());
            for (auto& a : c->args) n += exprSize(a.get());
            break;
        }
        case Expr::Kind::Array: for (auto& x : static_cast<const ArrayExpr*>(e)->elements) n += exprSize(x.get()); break;
        case Expr::Kind::Map:   for (auto& kv : static_cast<const MapExpr*>(e)->entries) n += exprSize(kv.first.get()) + exprSize(kv.second.get()); break;
        default: break;
        }
        return n;
    }
    static void varsOf(const Expr* e, std::unordered_set<std::string>& out) {
        switch (e->kind) {
        case Expr::Kind::Var:    out.insert(static_cast<const VarExpr*>(e)->name); break;
        case Expr::Kind::Unary:  varsOf(static_cast<const UnaryExpr*>(e)->right.get(), out); break;
        case Expr::Kind::Binary: varsOf(static_cast<const BinaryExpr*>(e)->left.get(), out); varsOf(static_cast<const BinaryExpr*>(e)->right.get(), out); break;
        case Expr::Kind::And: case Expr::Kind::Or:
            varsOf(static_cast<const LogicExpr*>(e)->left.get(), out); varsOf(static_cast<const LogicExpr*>(e)->right.get(), out); break;
        case Expr::Kind::Index:  varsOf(static_cast<const IndexExpr*>(e)->object.get(), out); varsOf(static_cast<const IndexExpr*>(e)->index.get(), out); break;
        case Expr::Kind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            varsOf(c->callee.get(), out);
            for (auto& a : c->args) varsOf(a.get(), out);
            break;
        }
        case Expr::Kind::Array: for (auto& x : static_cast<const ArrayExpr*>(e)->elements) varsOf(x.get(), out); break;
        case Expr::Kind::Map:   for (auto& kv : static_cast<const MapExpr*>(e)->entries) { varsOf(kv.first.get(), out); varsOf(kv.second.get(), out); } break;
        default: break;
        }
    }
    // called once the declaration is compiled: from here on, calls of this name may be expanded
    void registerInline(const FnStmt* f) {
        if (!f->returnType.isAny() || f->returnElem) return;
        for (auto& p : f->params) if (p.optional || p.variadic || p.inout || p.elem) return;
        const Expr* body = static_cast<const ReturnStmt*>(f->body[0].get())->value.get();
        if (hasLambdaOrRef(body) || exprSize(body) > kInlineMaxSize) return;
        auto it = binds_.find(f->name);
        if (it == binds_.end() || it->second.top != 1 || it->second.nested != 0) return;
        std::unordered_set<std::string> vars;
        varsOf(static_cast<const ReturnStmt*>(f->body[0].get())->value.get(), vars);
        for (auto& p : f->params) vars.erase(p.name);
        for (auto& v : vars) {
            auto b = binds_.find(v);
            if (b != binds_.end() && b->second.nested != 0) return;
        }
        inlineFns_[f->name] = f;
    }
    // true when working out the expression cannot run script code that changes a variable: only operators, builtins that just read,
    // and calls that will themselves be written out (the same test tryInline makes for them)
    bool pureForInline(const Expr* e, const FnStmt* self, int depth) {
        switch (e->kind) {
        case Expr::Kind::Number: case Expr::Kind::Int: case Expr::Kind::String: case Expr::Kind::Var: return true;
        case Expr::Kind::Unary:  return pureForInline(static_cast<const UnaryExpr*>(e)->right.get(), self, depth);
        case Expr::Kind::Binary: return pureForInline(static_cast<const BinaryExpr*>(e)->left.get(), self, depth) && pureForInline(static_cast<const BinaryExpr*>(e)->right.get(), self, depth);
        case Expr::Kind::And: case Expr::Kind::Or:
            return pureForInline(static_cast<const LogicExpr*>(e)->left.get(), self, depth) && pureForInline(static_cast<const LogicExpr*>(e)->right.get(), self, depth);
        case Expr::Kind::Index:  return pureForInline(static_cast<const IndexExpr*>(e)->object.get(), self, depth) && pureForInline(static_cast<const IndexExpr*>(e)->index.get(), self, depth);
        case Expr::Kind::Array:  for (auto& x : static_cast<const ArrayExpr*>(e)->elements) if (!pureForInline(x.get(), self, depth)) return false; return true;
        case Expr::Kind::Map:    for (auto& kv : static_cast<const MapExpr*>(e)->entries) if (!pureForInline(kv.first.get(), self, depth) || !pureForInline(kv.second.get(), self, depth)) return false; return true;
        case Expr::Kind::Call: {
            auto* c = static_cast<const CallExpr*>(e);
            if (c->callee->kind != Expr::Kind::Var || !c->resolved.empty()) return false;
            for (auto& a : c->args) if (!pureForInline(a.get(), self, depth)) return false;
            const std::string& n = static_cast<const VarExpr*>(c->callee.get())->name;
            auto b = binds_.find(n);
            if (isPureBuiltin(n)) return b == binds_.end();   // the name still means the builtin
            auto it = inlineFns_.find(n);
            if (it == inlineFns_.end() || !it->second->isExprForm || it->second == self || depth >= 4 || c->args.size() != it->second->params.size()) return false;
            for (auto* x : expanding_) if (x == it->second) return false;
            return pureForInline(static_cast<const ReturnStmt*>(it->second->body[0].get())->value.get(), it->second, depth + 1);
        }
        default: return false;
        }
    }
    // writes the call as its expression and returns true, or returns false and emits nothing (the call is then an ordinary one)
    bool tryInline(CallExpr* c) {
        if (c->callee->kind != Expr::Kind::Var || !c->resolved.empty()) return false;
        auto it = inlineFns_.find(static_cast<VarExpr*>(c->callee.get())->name);
        if (it == inlineFns_.end()) return false;
        const FnStmt* f = it->second;
        if (c->args.size() != f->params.size() || expanding_.size() >= 8) return false;
        for (auto& a : c->args) if (a->kind == Expr::Kind::Ref) return false;
        for (auto* e : expanding_) if (e == f) return false;
        if (f->isTemplate) return expandTemplate(c, f);
        const int line = c->range.startLine;
        const int serial = inlineSerial_++;
        const Expr* body = static_cast<const ReturnStmt*>(f->body[0].get())->value.get();
        // an argument that is a literal, or a variable when nothing in the call can change variables, is used as it stands
        // and needs no local of its own
        bool plainArgs = pureForInline(body, f, 0);
        for (auto& a : c->args) plainArgs = plainArgs && (a->kind == Expr::Kind::Var || isLiteral(a.get()));
        Renames map;
        bool scoped = false;
        // each other argument is worked out once, in order, in the caller's names, into a local nobody else can name
        for (size_t i = 0; i < f->params.size(); ++i) {
            const Param& p = f->params[i];
            const Expr* arg = c->args[i].get();
            if (p.type.isAny() && isLiteral(arg)) { map.lits[p.name] = arg; continue; }
            if (p.type.isAny() && plainArgs && arg->kind == Expr::Kind::Var) {
                const std::string& v = static_cast<const VarExpr*>(arg)->name;
                if (renames_ && renames_->lits.count(v)) map.lits[p.name] = renames_->lits.at(v);
                else map.names[p.name] = renamed(v);
                continue;
            }
            if (!scoped) { emitScopePush(); scoped = true; }
            std::string hidden = p.name + "\x01" + std::to_string(serial);
            compileExpr(c->args[i].get());
            const int slot = cur_->internLocal(hidden);
            emit(p.type.isAny() ? Op::DEFINE : Op::DEFINE_TYPED, cur_->intern(hidden), 0.0, line, p.type, slot);
            map.names[p.name] = std::move(hidden);
        }
        auto* savedRenames = renames_;
        renames_ = &map;
        expanding_.push_back(f);
        compileExpr(const_cast<Expr*>(body));
        expanding_.pop_back();
        renames_ = savedRenames;
        if (scoped) emitScopePop();
        return true;
    }
    static bool isLiteral(const Expr* e) { return e->kind == Expr::Kind::Number || e->kind == Expr::Kind::Int || e->kind == Expr::Kind::String; }
    const std::string& renamed(const std::string& n) const {
        if (renames_) { auto it = renames_->names.find(n); if (it != renames_->names.end()) return it->second; }
        return n;
    }
    const Expr* litFor(const std::string& n) const {
        if (!renames_) return nullptr;
        auto it = renames_->lits.find(n);
        return it == renames_->lits.end() ? nullptr : it->second;
    }

    // ---- constants -----------------------------------------------------------------------------------------------
    // a value known when the script is compiled: what a `const` holds, and what an operator on such values gives. an expression
    // that is all constants is compiled as one literal. anything the ordinary instructions would do differently or fail on (an
    // int result too big, a division by zero, a number added to a string) is left to them
    struct ConstVal {
        enum Kind { Int, Num, Str } kind = Int;
        int64_t i = 0; double d = 0.0; std::string s;
    };
    std::unordered_map<std::string, ConstVal> consts_;
    bool constCalls_ = false;   // set while a const's value is worked out: only there may a pure native be called
    static Value valueOfConst(const ConstVal& v) {
        if (v.kind == ConstVal::Int) return Value(v.i);
        if (v.kind == ConstVal::Num) return Value(v.d);
        return Value(v.s);
    }
    // `len("abc")` in a const: a pure native (Value::Callable::pure) this file does not make a name of its own for, called now
    // with constant arguments. false when any of that does not hold, the arguments don't fit its parameters, it raises, or the
    // result is not a number or a string
    bool foldPureCall(const CallExpr* c, ConstVal& out) {
        if (!constCalls_ || !interp_ || c->callee->kind != Expr::Kind::Var) return false;
        const std::string& written = static_cast<const VarExpr*>(c->callee.get())->name;
        const std::string& nm = c->resolved.empty() ? written : c->resolved;   // the member the checker picked from an overload group
        if (binds_.count(written) || !interp_->hasGlobal(nm)) return false;
        const Value& fv = interp_->getGlobal(nm);
        if (!fv.isCallable() || !fv.asCallable().isNative() || !fv.asCallable().pure) return false;
        const Value::Callable& fc = fv.asCallable();
        std::vector<Value> args;
        for (const auto& a : c->args) {
            ConstVal v;
            if (!a || !foldExpr(a.get(), v)) return false;
            args.push_back(valueOfConst(v));
        }
        // the checks the VM makes before it calls a native: how many arguments, and their types
        size_t fixed = 0, required = 0;
        bool variadic = false;
        for (const auto& p : fc.sig) {
            if (p.inout) return false;
            if (p.variadic) { variadic = true; break; }
            ++fixed;
            if (!p.optional) ++required;
        }
        if (args.size() < required || (!variadic && args.size() > fixed)) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            const Param& p = fc.sig[i < fixed ? i : fixed];
            if (!p.type.contains(args[i].tag())) return false;
        }
        Value r;
        try { r = fc.native(args); } catch (...) { return false; }
        if (r.isInt())         { out.kind = ConstVal::Int; out.i = r.asInt(); return true; }
        if (r.isNumber())      { out.kind = ConstVal::Num; out.d = r.asNumber(); return true; }
        if (r.isString())      { out.kind = ConstVal::Str; out.s = r.asString(); return true; }
        return false;
    }
    // why a call in a const can't be worked out, for the error: "" when the reason is not about which function is called
    std::string constCallProblem(const Expr* e) const {
        if (!e) return "";
        if (e->kind == Expr::Kind::Call) {
            auto* c = static_cast<const CallExpr*>(e);
            if (c->callee->kind != Expr::Kind::Var) return "only a named native can be called";
            const std::string& written = static_cast<const VarExpr*>(c->callee.get())->name;
            if (binds_.count(written)) return "'" + written + "' is a name this file makes, not a native";
            const std::string& nm = c->resolved.empty() ? written : c->resolved;
            if (!interp_ || !interp_->hasGlobal(nm))
                return "'" + nm + "' is not known yet when the file is compiled (import its plugin at the top of the file)";
            const Value& fv = interp_->getGlobal(nm);
            if (!fv.isCallable() || !fv.asCallable().isNative() || !fv.asCallable().pure)
                return "'" + nm + "' is not a pure native, so a const can't call it";
            for (const auto& a : c->args) { std::string p = constCallProblem(a.get()); if (!p.empty()) return p; }
            return "";
        }
        switch (e->kind) {
        case Expr::Kind::Unary:  return constCallProblem(static_cast<const UnaryExpr*>(e)->right.get());
        case Expr::Kind::Binary: { auto* b = static_cast<const BinaryExpr*>(e); std::string p = constCallProblem(b->left.get()); return p.empty() ? constCallProblem(b->right.get()) : p; }
        case Expr::Kind::And: case Expr::Kind::Or: { auto* g = static_cast<const LogicExpr*>(e); std::string p = constCallProblem(g->left.get()); return p.empty() ? constCallProblem(g->right.get()) : p; }
        default: return "";
        }
    }
    static bool constNum(const ConstVal& v, FuseNum& n) {
        if (v.kind == ConstVal::Int) { n.isInt = true; n.i = v.i; return true; }
        if (v.kind == ConstVal::Num) { n.isInt = false; n.d = v.d; return true; }
        return false;
    }
    static ConstVal constOf(const Value& v) {
        ConstVal c;
        if (v.isInt()) { c.kind = ConstVal::Int; c.i = v.asInt(); } else { c.kind = ConstVal::Num; c.d = v.asNumber(); }
        return c;
    }
    static ConstVal constBool(bool b) { ConstVal c; c.kind = ConstVal::Int; c.i = b ? 1 : 0; return c; }   // comparisons give the int 1 or 0
    bool foldExpr(const Expr* e, ConstVal& out) {
        switch (e->kind) {
        case Expr::Kind::Int:    out.kind = ConstVal::Int; out.i = static_cast<const IntExpr*>(e)->v; return true;
        case Expr::Kind::Number: out.kind = ConstVal::Num; out.d = static_cast<const NumberExpr*>(e)->v; return true;
        case Expr::Kind::String: out.kind = ConstVal::Str; out.s = static_cast<const StringExpr*>(e)->v; return true;
        case Expr::Kind::Var: {
            if (consts_.empty()) return false;
            const std::string& n = static_cast<const VarExpr*>(e)->name;
            if (renames_ && (renames_->names.count(n) || renames_->lits.count(n))) return false;
            auto it = consts_.find(n);
            if (it == consts_.end()) return false;
            out = it->second;
            return true;
        }
        case Expr::Kind::Unary: {
            auto* u = static_cast<const UnaryExpr*>(e);
            ConstVal v; FuseNum n;
            if (!foldExpr(u->right.get(), v) || !constNum(v, n)) return false;
            if (u->op == "-") {
                if (n.isInt) { if (n.i == INT64_MIN) return false; out.kind = ConstVal::Int; out.i = -n.i; }
                else { out.kind = ConstVal::Num; out.d = -n.d; }
                return true;
            }
            if (u->op == "!") { out = constBool(n.isInt ? n.i == 0 : n.d == 0.0); return true; }
            if (u->op == "~" && n.isInt) { out.kind = ConstVal::Int; out.i = ~n.i; return true; }
            return false;
        }
        case Expr::Kind::Binary: {
            auto* b = static_cast<const BinaryExpr*>(e);
            ConstVal l, r;
            if (!foldExpr(b->left.get(), l) || !foldExpr(b->right.get(), r)) return false;
            FuseNum x, y;
            if (constNum(l, x) && constNum(r, y)) {
                Value res;
                int k = b->op == "+" ? 0 : b->op == "-" ? 1 : b->op == "*" ? 2 : b->op == "%" ? 3 : b->op == "/" ? 4 : -1;
                if (k >= 0) { if (!fuseArithNum(k, x, y, res)) return false; out = constOf(res); return true; }
                if (x.isInt && y.isInt && (b->op == "&" || b->op == "|" || b->op == "^" || b->op == "<<" || b->op == ">>")) {
                    int64_t r;
                    const char k = b->op == "&" ? '&' : b->op == "|" ? '|' : b->op == "^" ? '^' : b->op == "<<" ? 'l' : 'r';
                    if (!bitOpInts(k, x.i, y.i, r)) return false;   // a bad shift count is the run time error's to report
                    out.kind = ConstVal::Int; out.i = r;
                    return true;
                }
                int c = b->op == "<" ? 0 : b->op == ">" ? 1 : b->op == "<=" ? 2 : b->op == ">=" ? 3 : b->op == "==" ? 4 : b->op == "!=" ? 5 : -1;
                bool yes;
                if (c >= 0 && fuseCmpNum(c, x, y, yes)) { out = constBool(yes); return true; }
                return false;
            }
            if (l.kind == ConstVal::Str && r.kind == ConstVal::Str) {
                if (b->op == "+")  { out.kind = ConstVal::Str; out.s = l.s + r.s; return true; }
                if (b->op == "==") { out = constBool(l.s == r.s); return true; }
                if (b->op == "!=") { out = constBool(l.s != r.s); return true; }
            }
            return false;
        }
        case Expr::Kind::And: case Expr::Kind::Or: {
            auto* g = static_cast<const LogicExpr*>(e);
            ConstVal l; FuseNum x;
            if (!foldExpr(g->left.get(), l) || !constNum(l, x)) return false;
            const bool truthy = x.isInt ? x.i != 0 : x.d != 0.0;
            if (e->kind == Expr::Kind::And ? !truthy : truthy) { out = std::move(l); return true; }
            return foldExpr(g->right.get(), out);
        }
        case Expr::Kind::Call: return foldPureCall(static_cast<const CallExpr*>(e), out);
        default: return false;
        }
    }
    void emitConst(const ConstVal& v, int line) {
        if (v.kind == ConstVal::Int) emitPushInt(v.i, line);
        else if (v.kind == ConstVal::Num) emit(Op::PUSH_NUM, 0, v.d, line);
        else emit(Op::PUSH_STR, cur_->intern(v.s), 0.0, line);
    }

    // ---- const fn: a function written out at its calls, statements and all ------------------------------------------
    // the body is copied into each call that names the function, inside a block. the parameters and every name the body makes
    // (locals, loop variables) become hidden locals, and a `return` becomes a jump to the end of the copy. so it is not a call:
    // there is no frame of its own and no line of its own in an error trace. a call it cannot be written out for is an ordinary one
    struct TmplCtx { std::vector<int> ends; int baseScope; size_t loopBase; };
    std::vector<TmplCtx> tmpl_;
    struct TmplScan {
        std::unordered_set<std::string> declared, reads, written;   // names the body makes, reads, and changes (the root of `a[i] = v` too)
        std::vector<const Expr*> exprs;                             // every expression in it
        bool pure = false;                                          // none of them can run script code
        int size = 0;
    };
    std::unordered_map<const FnStmt*, TmplScan> tmplScans_;
    void tmplScanExpr(const Expr* e, TmplScan& t) { t.size += exprSize(e); varsOf(e, t.reads); t.exprs.push_back(e); }
    void tmplScanBlock(const std::vector<StmtPtr>& b, TmplScan& t) { for (auto& s : b) tmplScanStmt(s.get(), t); }
    void tmplScanStmt(const Stmt* s, TmplScan& t) {
        ++t.size;
        switch (s->kind) {
        case Stmt::Kind::Local: {
            auto* l = static_cast<const LocalStmt*>(s);
            t.declared.insert(l->targets[0].name);
            tmplScanExpr(l->value.get(), t);
            break;
        }
        case Stmt::Kind::Assign: {
            auto* a = static_cast<const AssignStmt*>(s);
            const Expr* root = a->target.get();
            if (root->kind == Expr::Kind::Index) tmplScanExpr(root, t);
            while (root->kind == Expr::Kind::Index) root = static_cast<const IndexExpr*>(root)->object.get();
            if (root->kind == Expr::Kind::Var) t.written.insert(static_cast<const VarExpr*>(root)->name);
            tmplScanExpr(a->value.get(), t);
            break;
        }
        case Stmt::Kind::If: {
            auto* i = static_cast<const IfStmt*>(s);
            tmplScanExpr(i->cond.get(), t); tmplScanBlock(i->thenBlock, t); tmplScanBlock(i->elseBlock, t);
            break;
        }
        case Stmt::Kind::While: { auto* w = static_cast<const WhileStmt*>(s); tmplScanExpr(w->cond.get(), t); tmplScanBlock(w->body, t); break; }
        case Stmt::Kind::For: {
            auto* f = static_cast<const ForStmt*>(s);
            if (!f->keyName.empty()) t.declared.insert(f->keyName);
            t.declared.insert(f->varName);
            tmplScanExpr(f->iterable.get(), t); tmplScanBlock(f->body, t);
            break;
        }
        case Stmt::Kind::Expr:   tmplScanExpr(static_cast<const ExprStmt*>(s)->expr.get(), t); break;
        case Stmt::Kind::Return: tmplScanExpr(static_cast<const ReturnStmt*>(s)->value.get(), t); break;
        default: break;
        }
    }
    static constexpr int kTemplateMaxSize = 150;
    void registerTemplate(const FnStmt* f) {
        TmplScan t;
        for (auto& p : f->params) t.declared.insert(p.name);
        tmplScanBlock(f->body, t);
        for (auto& w : t.written)
            if (!t.declared.count(w))
                raiseError("compiler", "a const fn can only change its own parameters and locals, and '" + w + "' is neither", f->range);
        if (t.size > kTemplateMaxSize) return;
        auto it = binds_.find(f->name);
        if (it == binds_.end() || it->second.top != 1 || it->second.nested != 0) return;
        for (auto& v : t.reads) {
            if (t.declared.count(v)) continue;
            auto b = binds_.find(v);
            if (b != binds_.end() && b->second.nested != 0) return;
        }
        inlineFns_[f->name] = f;
        t.pure = true;
        for (auto* e : t.exprs) if (!pureForInline(e, f, 0)) { t.pure = false; break; }
        tmplScans_[f] = std::move(t);
    }
    void declaredIn(const std::vector<StmtPtr>& b, std::vector<std::string>& out) {
        TmplScan t;
        tmplScanBlock(b, t);
        for (auto& n : t.declared) out.push_back(n);
    }
    bool expandTemplate(CallExpr* c, const FnStmt* f) {
        const int line = c->range.startLine;
        const int serial = inlineSerial_++;
        Renames map;
        emitScopePush();
        const int base = scopeDepth_;
        const TmplScan& scan = tmplScans_.at(f);
        // a literal or a variable given for a parameter the body never changes is used as it stands, when nothing in the body can
        // change a variable behind its back
        bool plainArgs = scan.pure;
        for (auto& a : c->args) plainArgs = plainArgs && pureForInline(a.get(), f, 0);
        // the arguments first, in order, in the caller's names
        for (size_t i = 0; i < f->params.size(); ++i) {
            const Param& p = f->params[i];
            const Expr* arg = c->args[i].get();
            if (plainArgs && p.type.isAny() && !scan.written.count(p.name) && (isLiteral(arg) || arg->kind == Expr::Kind::Var)) {
                if (isLiteral(arg)) { map.lits[p.name] = arg; continue; }
                const std::string& v = static_cast<const VarExpr*>(arg)->name;
                if (const Expr* lit = litFor(v)) map.lits[p.name] = lit; else map.names[p.name] = renamed(v);
                continue;
            }
            std::string hidden = p.name + "\x01" + std::to_string(serial);
            compileExpr(c->args[i].get());
            emit(p.type.isAny() ? Op::DEFINE : Op::DEFINE_TYPED, cur_->intern(hidden), 0.0, line, p.type, cur_->internLocal(hidden));
            map.names[p.name] = std::move(hidden);
        }
        std::vector<std::string> mine;
        declaredIn(f->body, mine);
        for (auto& n : mine) if (!map.names.count(n)) map.names[n] = n + "\x01" + std::to_string(serial);
        auto* savedRenames = renames_;
        renames_ = &map;
        expanding_.push_back(f);
        tmpl_.push_back({{}, base, loopStack_.size()});
        for (auto& st : f->body) compileStmt(st.get());
        emit(Op::PUSH_NIL, 0, 0.0, line);
        for (int j : tmpl_.back().ends) patchJump(j);
        tmpl_.pop_back();
        expanding_.pop_back();
        renames_ = savedRenames;
        emitScopePop();
        return true;
    }

    // emit helpers
    void emit(Op op, int operand = 0, double num = 0.0, int line = 0, TypeSet th = TypeSet::Any(), int slot = -1) {
        Instruction i; i.op = op; i.operand = operand; i.typeHint = th; i.slot = slot;
        if (op == Op::PUSH_NUM) { i.operand = (int)cur_->nums.size(); cur_->nums.push_back(num); }
        cur_->code.push_back(i);
        cur_->lines.push_back(line);
        pushCol(line);
    }
    void emitPushInt(int64_t v, int line) {
        Instruction i; i.op = Op::PUSH_INT; i.operand = (int)cur_->ints.size(); cur_->ints.push_back(v);
        cur_->code.push_back(i);
        cur_->lines.push_back(line);
        pushCol(line);
    }
    void emitScopePush() { emit(Op::SCOPE_PUSH); ++scopeDepth_; }
    void emitScopePop()  { emit(Op::SCOPE_POP);  --scopeDepth_; }

    // a block needs SCOPE_PUSH / SCOPE_POP only to have the names it makes erased when it ends. only `local`, `fn`, a for loop's
    // variable, a catch variable and the temporary of a nested index write (`a[i][j] = v`) are made that way: a plain assignment to a
    // new name makes a variable of the whole function, which no block erases. so a block without a `local`, `fn` or nested index write
    // of its own (a nested block decides for itself) has nothing for the pop to do
    bool blockNeedsScope(const std::vector<StmtPtr>& b) const {
        if (!options.elideScopes) return true;
        for (auto& s : b) {
            if (s->kind == Stmt::Kind::Local || s->kind == Stmt::Kind::Fn) return true;
            if (s->kind == Stmt::Kind::Assign) {
                auto* t = static_cast<const AssignStmt*>(s.get())->target.get();
                if (t->kind == Expr::Kind::Index && static_cast<const IndexExpr*>(t)->object->kind == Expr::Kind::Index) return true;
            }
        }
        return false;
    }

    // returns the index of the emitted instruction for later back-patching
    int emitJump(Op op, int line = 0) {
        Instruction i; i.op = op; i.operand = -1;
        cur_->code.push_back(i);
        cur_->lines.push_back(line);
        pushCol(line);
        return (int)cur_->code.size() - 1;
    }

    void patchJump(int idx) {
        assert(idx >= 0 && idx < (int)cur_->code.size());
        cur_->code[idx].operand = (int)cur_->code.size();
    }

    int here() const { return (int)cur_->code.size(); }

    // true only for code at the true top level
    bool atTopLevel() const {
        return cur_ == prog_.chunks[0].get() && scopeDepth_ == 0;
    }

    // expression compilation
    void compileExpr(Expr* e) {
        RangeScope rangeScope(*this, e->range);
        if (e->kind == Expr::Kind::Binary || e->kind == Expr::Kind::Unary || e->kind == Expr::Kind::Var ||
            e->kind == Expr::Kind::And || e->kind == Expr::Kind::Or || (constCalls_ && e->kind == Expr::Kind::Call)) {
            ConstVal cv;
            if (foldExpr(e, cv)) { emitConst(cv, e->range.startLine); return; }
        }
        switch (e->kind) {

        case Expr::Kind::Number:
            emit(Op::PUSH_NUM, 0, static_cast<NumberExpr*>(e)->v, e->range.startLine);
            break;

        case Expr::Kind::Int:
            emitPushInt(static_cast<IntExpr*>(e)->v, e->range.startLine);
            break;

        case Expr::Kind::String: {
            auto* se = static_cast<StringExpr*>(e);
            emit(Op::PUSH_STR, cur_->intern(se->v), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Var: {
            auto* ve = static_cast<VarExpr*>(e);
            if (const Expr* lit = litFor(ve->name)) { compileExpr(const_cast<Expr*>(lit)); break; }
            emitLoad(ve->name, e->range.startLine);
            break;
        }

        case Expr::Kind::Unary: {
            auto* u = static_cast<UnaryExpr*>(e);
            compileExpr(u->right.get());
            if (u->op == "-") emit(Op::NEG, 0, 0.0, e->range.startLine);
            else if (u->op == "!") emit(Op::NOT, 0, 0.0, e->range.startLine);
            else if (u->op == "~") emit(Op::BNOT, 0, 0.0, e->range.startLine);
            else raiseError("compiler", "unknown unary op: " + u->op, e->range);
            break;
        }

        case Expr::Kind::Binary:
            compileBinary(static_cast<BinaryExpr*>(e));
            break;

        // if left is falsy push left and skip right
        case Expr::Kind::And: {
            auto* log = static_cast<LogicExpr*>(e);
            compileExpr(log->left.get());
            int skipRight = emitJump(Op::JUMP_IF_FALSE_PEEK, e->range.startLine);
            emit(Op::POP);                       // discard left; we want right's value
            compileExpr(log->right.get());
            patchJump(skipRight);
            break;
        }

        // if left is truthy push left and skip right
        case Expr::Kind::Or: {
            auto* log = static_cast<LogicExpr*>(e);
            compileExpr(log->left.get());
            int skipRight = emitJump(Op::JUMP_IF_TRUE_PEEK, e->range.startLine);
            emit(Op::POP);
            compileExpr(log->right.get());
            patchJump(skipRight);
            break;
        }

        case Expr::Kind::Call: {
            auto* c = static_cast<CallExpr*>(e);
            if (tryInline(c)) break;
            // the type checker may have picked one member of an overload group for this call, then it is a call of that function by name
            if (!c->resolved.empty() && c->callee->kind == Expr::Kind::Var) emitLoad(c->resolved, c->callee->range.startLine);
            else compileExpr(c->callee.get());
            const size_t calleeAt = cur_->code.size() - 1;
            const bool calleeIsLoad = c->callee->kind == Expr::Kind::Var && (cur_->code[calleeAt].op == Op::LOAD || cur_->code[calleeAt].op == Op::LOAD_GLOBAL);
            bool hasRef = false;
            for (auto& arg : c->args) {
                if (arg->kind == Expr::Kind::Ref) { compileRefArg(static_cast<RefExpr*>(arg.get())); hasRef = true; }
                else compileExpr(arg.get());
            }
            emit(Op::CALL, (int)c->args.size(), 0.0, e->range.startLine);
            if (hasRef) cur_->code.back().flags = kHasRef;
            else if (calleeIsLoad) { cur_->code[calleeAt].flags |= kCallee; }
            break;
        }

        case Expr::Kind::Ref:
            raiseError("compiler", "'&' only works on an argument of a call", e->range);

        case Expr::Kind::Array: {
            auto* a = static_cast<ArrayExpr*>(e);
            for (auto& el : a->elements) compileExpr(el.get());
            emit(a->isRange ? Op::BUILD_RANGE : Op::BUILD_ARRAY, (int)a->elements.size(), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Map: {
            auto* m = static_cast<MapExpr*>(e);
            for (auto& [k, v] : m->entries) {
                compileExpr(k.get());
                compileExpr(v.get());
            }
            emit(Op::BUILD_MAP, (int)m->entries.size(), 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Index: {
            auto* idx = static_cast<IndexExpr*>(e);
            if (options.inPlaceOps && idx->object->kind == Expr::Kind::Var && !litFor(static_cast<VarExpr*>(idx->object.get())->name)) {
                // the variable is read after the key is worked out, which only matters if the key could change it.
                // a key with no calls can't. one with calls gets a STASH_VAR first, see there
                const std::string& nm = static_cast<VarExpr*>(idx->object.get())->name;
                std::vector<std::string> callees;
                Purity p = purityOf(idx->index.get(), &callees);
                if (p != Purity::CallFree) emitStash(nm, e->range.startLine, p, callees);
                compileExpr(idx->index.get());
                emitVarOp(Op::INDEX_GET_VAR, nm, e->range.startLine, p == Purity::CallFree ? 0 : kStash);
                break;
            }
            compileExpr(idx->object.get());
            compileExpr(idx->index.get());
            emit(Op::INDEX_GET, 0, 0.0, e->range.startLine);
            break;
        }

        case Expr::Kind::Lambda: {
            auto* lam = static_cast<LambdaExpr*>(e);
            int fnIdx = compileFunction(
                "<lambda>", lam->params, lam->returnType,
                lam->ownedBody, e->range, cur_->sourceFile);
            emit(Op::MAKE_CLOSURE, fnIdx, 0.0, e->range.startLine);
            break;
        }

        default:
            raiseError("compiler", "unhandled expression kind", e->range);
        }
    }

    // emits one of the variable-in-place ops (STASH_VAR, INDEX_GET_VAR, INDEX_SET_VAR, APPEND_ARR) for `name`.
    // returns its index so a jump target can be patched into it
    int emitVarOp(Op op, const std::string& name0, int line, int flags) {
        const std::string& name = renamed(name0);
        emit(op, cur_->intern(name), 0.0, line, TypeSet::Any(), atTopLevel() ? -1 : cur_->internLocal(name));
        cur_->code.back().flags = (uint8_t)flags;
        return (int)cur_->code.size() - 1;
    }
    // STASH_VAR for code of the given purity (see purityOf). `callees` are the pure builtins it calls
    void emitStash(const std::string& name, int line, Purity p, const std::vector<std::string>& callees) {
        int flags = p == Purity::CallFree ? kCallFree : p == Purity::PureCalls ? kPureCalls : 0;
        int at = emitVarOp(Op::STASH_VAR, name, line, flags);
        if (p == Purity::PureCalls) {
            std::vector<int> set;
            for (auto& c : callees) set.push_back(cur_->intern(c));
            cur_->code[at].arg = (int)cur_->pureSets.size();
            cur_->pureSets.push_back(std::move(set));
        }
    }

    // `&name`, `&name[k1][k2]`: pushes the keys in order, then REF_PATH turns them into a reference path (see value.h)
    void compileRefArg(RefExpr* r) {
        std::vector<IndexExpr*> chain;
        Expr* cur = r->target.get();
        while (cur->kind == Expr::Kind::Index) { chain.push_back(static_cast<IndexExpr*>(cur)); cur = chain.back()->object.get(); }
        const std::string& nm = static_cast<VarExpr*>(cur)->name;
        for (size_t i = chain.size(); i-- > 0; ) compileExpr(chain[i]->index.get());
        int at = emitVarOp(Op::REF_PATH, nm, r->range.startLine, 0);
        cur_->code[at].arg = (int64_t)chain.size();
    }

    void compileBinary(BinaryExpr* b) {
        compileExpr(b->left.get());
        compileExpr(b->right.get());
        int ln = b->range.startLine;
        const auto& op = b->op;
        if      (op == "+")  emit(Op::ADD,  0, 0.0, ln);
        else if (op == "-")  emit(Op::SUB,  0, 0.0, ln);
        else if (op == "*")  emit(Op::MUL,  0, 0.0, ln);
        else if (op == "/")  emit(Op::DIV,  0, 0.0, ln);
        else if (op == "%")  emit(Op::MOD,  0, 0.0, ln);
        else if (op == "&")  emit(Op::BAND, 0, 0.0, ln);
        else if (op == "|")  emit(Op::BOR,  0, 0.0, ln);
        else if (op == "^")  emit(Op::BXOR, 0, 0.0, ln);
        else if (op == "<<") emit(Op::SHL,  0, 0.0, ln);
        else if (op == ">>") emit(Op::SHR,  0, 0.0, ln);
        else if (op == "==") emit(Op::EQ,   0, 0.0, ln);
        else if (op == "!=") emit(Op::NEQ,  0, 0.0, ln);
        else if (op == "<")  emit(Op::LT,   0, 0.0, ln);
        else if (op == ">")  emit(Op::GT,   0, 0.0, ln);
        else if (op == "<=") emit(Op::LTE,  0, 0.0, ln);
        else if (op == ">=") emit(Op::GTE,  0, 0.0, ln);
        else raiseError("compiler", "unknown binary op: " + op, b->range);
    }

    // statement compilation
    void compileStmt(Stmt* s) {
        RangeScope rangeScope(*this, s->range);
        switch (s->kind) {

        case Stmt::Kind::Expr: {
            if (static_cast<ExprStmt*>(s)->inlineC)
                raiseError("compiler", "this is C text for aembr (c \"\"\" ... \"\"\"), embr cannot run it; compile the file with aembr", s->range);
            compileExpr(static_cast<ExprStmt*>(s)->expr.get());
            emit(Op::POP, 0, 0.0, s->range.startLine); // discard result
            break;
        }

        case Stmt::Kind::Local: {
            auto* l = static_cast<LocalStmt*>(s);
            int line = s->range.startLine;
            compileExpr(l->value.get());
            bool destructure = l->targets.size() > 1 || l->bracketed;
            if (!destructure) {
                emitDefineTarget(l->targets[0], line, l->inferredHint);
            } else {
                emit(Op::UNPACK, (int)l->targets.size(), 0.0, line);
                for (auto& t : l->targets) emitDefineTarget(t, line);
            }
            break;
        }

        case Stmt::Kind::MultiAssign: {
            auto* m = static_cast<MultiAssignStmt*>(s);
            int line = s->range.startLine;
            compileExpr(m->value.get());
            emit(Op::UNPACK, (int)m->names.size(), 0.0, line);
            for (auto& nm : m->names) emitStore(nm, line);
            break;
        }

        case Stmt::Kind::Assign:
            compileAssign(static_cast<AssignStmt*>(s));
            break;

        case Stmt::Kind::If: {
            auto* i = static_cast<IfStmt*>(s);
            compileExpr(i->cond.get());
            int toElse = emitJump(Op::JUMP_IF_FALSE, s->range.startLine);

            const bool thenScope = blockNeedsScope(i->thenBlock);
            if (thenScope) emitScopePush();
            for (auto& st : i->thenBlock) compileStmt(st.get());
            if (thenScope) emitScopePop();

            if (!i->elseBlock.empty()) {
                int toEnd = emitJump(Op::JUMP, s->range.startLine);
                patchJump(toElse);
                const bool elseScope = blockNeedsScope(i->elseBlock);
                if (elseScope) emitScopePush();
                for (auto& st : i->elseBlock) compileStmt(st.get());
                if (elseScope) emitScopePop();
                patchJump(toEnd);
            } else {
                patchJump(toElse);
            }
            break;
        }

        case Stmt::Kind::While: {
            auto* w = static_cast<WhileStmt*>(s);
            int line = s->range.startLine;
            int loopTop = here();
            compileExpr(w->cond.get());
            int toEnd = emitJump(Op::JUMP_IF_FALSE, line);

            loopStack_.push_back({{}, loopTop, scopeDepth_, tryDepth_, false});
            const bool bodyScope = blockNeedsScope(w->body);
            if (bodyScope) emitScopePush();
            for (auto& st : w->body) compileStmt(st.get());
            if (bodyScope) emitScopePop();
            emit(Op::JUMP, loopTop, 0.0, line);

            LoopCtx ctx = std::move(loopStack_.back());
            loopStack_.pop_back();
            patchJump(toEnd);
            for (int idx : ctx.breakJumps) patchJump(idx);
            break;
        }

        case Stmt::Kind::For: {
            auto* f = static_cast<ForStmt*>(s);
            int line = s->range.startLine;
            compileExpr(f->iterable.get());
            int varCount = f->keyName.empty() ? 1 : 2;
            emit(Op::ITER_INIT, varCount, 0.0, line);

            int loopTop = here();
            int toEnd = emitJump(Op::ITER_NEXT, line);

            loopStack_.push_back({{}, loopTop, scopeDepth_, tryDepth_, true});
            emitScopePush();
            if (varCount == 2) {
                emit(Op::DEFINE, cur_->intern(renamed(f->varName)), 0.0, line, TypeSet::Any(), cur_->internLocal(renamed(f->varName))); // pops value (TOS)
                emit(Op::DEFINE, cur_->intern(renamed(f->keyName)),  0.0, line, TypeSet::Any(), cur_->internLocal(renamed(f->keyName))); // pops key
            } else {
                emit(Op::DEFINE, cur_->intern(renamed(f->varName)), 0.0, line, TypeSet::Any(), cur_->internLocal(renamed(f->varName)));
            }
            for (auto& st : f->body) compileStmt(st.get());
            emitScopePop();
            emit(Op::JUMP, loopTop, 0.0, line);

            LoopCtx ctx = std::move(loopStack_.back());
            loopStack_.pop_back();
            patchJump(toEnd);
            for (int idx : ctx.breakJumps) patchJump(idx);
            emit(Op::ITER_POP, 0, 0.0, line);
            break;
        }

        case Stmt::Kind::Break: {
            // the parser rejects break/continue outside a loop, so loopStack_ should be non-empty;
            // check anyway, back() on an empty vector is undefined behaviour
            if (loopStack_.empty()) raiseError("compiler", "'break' outside a loop", s->range);
            auto& ctx = loopStack_.back();
            for (int i = 0; i < scopeDepth_ - ctx.scopeDepthAtEntry; ++i) emit(Op::SCOPE_POP);
            for (int i = 0; i < tryDepth_ - ctx.tryDepthAtEntry; ++i) emit(Op::TRY_POP);
            ctx.breakJumps.push_back(emitJump(Op::JUMP, s->range.startLine));
            break;
        }

        case Stmt::Kind::Continue: {
            if (loopStack_.empty()) raiseError("compiler", "'continue' outside a loop", s->range);
            auto& ctx = loopStack_.back();
            for (int i = 0; i < scopeDepth_ - ctx.scopeDepthAtEntry; ++i) emit(Op::SCOPE_POP);
            for (int i = 0; i < tryDepth_ - ctx.tryDepthAtEntry; ++i) emit(Op::TRY_POP);
            emit(Op::JUMP, ctx.continueTarget, 0.0, s->range.startLine);
            break;
        }

        case Stmt::Kind::Try: {
            auto* t = static_cast<TryStmt*>(s);
            int line = s->range.startLine;
            int tryPush = emitJump(Op::TRY_PUSH, line);
            ++tryDepth_;
            emitScopePush();
            for (auto& st : t->tryBlock) compileStmt(st.get());
            emitScopePop();
            --tryDepth_;
            emit(Op::TRY_POP, 0, 0.0, line);
            int toEnd = emitJump(Op::JUMP, line);

            patchJump(tryPush); // catch block starts here
            emitScopePush();
            emit(Op::DEFINE, cur_->intern(t->catchVar), 0.0, line, TypeSet::Any(), cur_->internLocal(t->catchVar)); // pops the error value handleTry() pushed
            for (auto& st : t->catchBlock) compileStmt(st.get());
            emitScopePop();
            patchJump(toEnd);
            break;
        }

        case Stmt::Kind::Fn: {
            auto* f = static_cast<FnStmt*>(s);
            if (f->returnElem && f->returnElem->isHost)
                raiseError("compiler", "the result type 'host' of '" + f->name + "' is for aembr, the compiler: embr does not run it", f->range);
            int fnIdx = compileFunction(
                f->name, f->params, f->returnType,
                f->body, f->range, cur_->sourceFile);
            emit(Op::MAKE_CLOSURE, fnIdx, 0.0, s->range.startLine);
            // functions defined at statement level go into the current scope so later code in the same chunk can call them
            if (atTopLevel()) emit(Op::DEFINE_GLOBAL, cur_->intern(f->name), 0.0, s->range.startLine);
            else              emit(Op::DEFINE, cur_->intern(f->name), 0.0, s->range.startLine, TypeSet::Any(), cur_->internLocal(f->name));
            if (f->isTemplate && atTopLevel()) registerTemplate(f);
            else if (f->isExprForm && atTopLevel()) registerInline(f);
            break;
        }

        case Stmt::Kind::Return: {
            auto* r = static_cast<ReturnStmt*>(s);
            compileExpr(r->value.get());
            if (!tmpl_.empty()) {
                // returning from a const fn written out here: leave its blocks and loops, then jump to the end of the copy
                const int base = tmpl_.back().baseScope; const size_t loopBase = tmpl_.back().loopBase;
                for (int i = 0; i < scopeDepth_ - base; ++i) emit(Op::SCOPE_POP);
                for (size_t k = loopStack_.size(); k-- > loopBase; ) if (loopStack_[k].isFor) emit(Op::ITER_POP);
                tmpl_.back().ends.push_back(emitJump(Op::JUMP, s->range.startLine));
                break;
            }
            emit(Op::RETURN, 0, 0.0, s->range.startLine);
            break;
        }

        case Stmt::Kind::Import: {
            // import changes the Interpreter (loads a plugin or a module), so it compiles to a call to the
            // built-in __import__ helper, which the VM hands to execImport. the path is a normal expression,
            // not always a string literal, so it's compiled like any other argument
            auto* im = static_cast<ImportStmt*>(s);
            compileExpr(im->pathExpr.get());
            // operand -1 = sentinel so the VM knows this CALL is an import
            emit(Op::CALL, -1, 0.0, s->range.startLine);
            emit(Op::POP, 0, 0.0, s->range.startLine);   // the VM pushes a placeholder result. a statement has no use for it
            break;
        }

        }
    }

    // emits the right DEFINE variant for a local/destructuring target. at true top level that is the
    // *_GLOBAL variant, plus a module-local mark so an importer doesn't export the name
    // `inferred` is the exact type of the value when the type checker knows it. a target with no written type takes the
    // type of its value: with it known the stores that follow carry the type, so a local needs nothing recorded and a global
    // is recorded for later files. without it, the type of the value is recorded when it is defined (kAuto)
    void emitDefineTarget(const DestructureTarget& t, int line, TypeSet inferred = TypeSet::Any()) {
        bool g = atTopLevel();
        const std::string& name = renamed(t.name);
        int  slot = g ? -1 : cur_->internLocal(name);
        if (!t.type.isAny())  emit(g ? Op::DEFINE_GLOBAL_TYPED : Op::DEFINE_TYPED, cur_->intern(name), 0.0, line, t.type, slot);
        else if (t.explicitType) emit(g ? Op::DEFINE_GLOBAL : Op::DEFINE, cur_->intern(name), 0.0, line, TypeSet::Any(), slot);   // written as any
        else if (options.useTypes && !inferred.isAny()) {
            if (g) emit(Op::DEFINE_GLOBAL_TYPED, cur_->intern(name), 0.0, line, inferred, slot);
            else   emit(Op::DEFINE, cur_->intern(name), 0.0, line, TypeSet::Any(), slot);
        } else {
            emit(g ? Op::DEFINE_GLOBAL : Op::DEFINE, cur_->intern(name), 0.0, line, TypeSet::Any(), slot);
            cur_->code.back().flags |= kAuto;
        }
        if (g) emit(Op::MARK_MODULE_LOCAL, cur_->intern(name), 0.0, line);
    }

    // compile an assignment, including chained index write-backs
    void compileAssign(AssignStmt* a) {
        if (a->isConst) {
            if (!atTopLevel()) raiseError("compiler", "a const goes at the top level of a file", a->range);
            ConstVal cv;
            constCalls_ = true;
            const bool folded = foldExpr(a->value.get(), cv);
            constCalls_ = false;
            if (!folded) {
                const std::string why = constCallProblem(a->value.get());
                raiseError("compiler", "not a constant: " + (why.empty() ? std::string("a const is made of numbers, strings, other consts, "
                           "operators on them and calls of pure natives (like len) with constant arguments") : why), a->value->range);
            }
            const std::string& nm = static_cast<VarExpr*>(a->target.get())->name;
            constCalls_ = true;          // so the store gets the worked-out value, not the calls
            compileAssignPlain(a);
            constCalls_ = false;
            auto b = binds_.find(nm);
            if (b != binds_.end() && b->second.top == 1 && b->second.nested == 0) consts_[nm] = std::move(cv);
            return;
        }
        compileAssignPlain(a);
    }
    void compileAssignPlain(AssignStmt* a) {
        // what the type checker worked out for the store into a plain variable
        const TypeSet storeHint   = options.useTypes ? a->storeHint : TypeSet::Any();
        const bool    storeProven = options.useTypes && a->storeProven;
        if (a->target->kind == Expr::Kind::Var) {
            auto* ve = static_cast<VarExpr*>(a->target.get());
            int ln = a->range.startLine;
            // `x = x + simple`: try to append in place. the right side is compiled twice (once for the fast path, once for the
            // fallback), so it must be something that has no side effects
            if (options.inPlaceOps && a->value->kind == Expr::Kind::Binary) {
                auto* b = static_cast<BinaryExpr*>(a->value.get());
                bool simple = b->right->kind == Expr::Kind::String || b->right->kind == Expr::Kind::Var ||
                              b->right->kind == Expr::Kind::Int    || b->right->kind == Expr::Kind::Number;
                // the append is only for strings: a variable known to hold numbers skips it (no lookup, no wasted push)
                const bool maybeString = (storeHint.mask & TS::Str.mask) != 0;
                if (b->op == "+" && simple && maybeString && b->left->kind == Expr::Kind::Var &&
                    static_cast<VarExpr*>(b->left.get())->name == ve->name) {
                    bool g = atTopLevel();
                    compileExpr(b->right.get());
                    int at = (int)cur_->code.size();
                    emit(Op::APPEND_VAR, cur_->intern(renamed(ve->name)), 0.0, ln, TypeSet::Any(), g ? -1 : cur_->internLocal(renamed(ve->name)));
                    emit(Op::POP, 0, 0.0, ln);
                    compileExpr(a->value.get());
                    emitStore(ve->name, ln, storeHint, storeProven);
                    cur_->code[at].arg = (int)cur_->code.size();
                    return;
                }
            }
            // `x = push(x, v)`: same idea, grows the array in place when push is embrlib's. the stack is
            // [push, stash, v], which is exactly what the ordinary call needs if the fast path doesn't apply
            if (options.inPlaceOps && a->value->kind == Expr::Kind::Call) {
                auto* c = static_cast<CallExpr*>(a->value.get());
                if (c->callee->kind == Expr::Kind::Var && static_cast<VarExpr*>(c->callee.get())->name == "push" &&
                    c->args.size() == 2 && c->args[0]->kind == Expr::Kind::Var &&
                    static_cast<VarExpr*>(c->args[0].get())->name == ve->name) {
                    std::vector<std::string> callees;
                    Purity p = purityOf(c->args[1].get(), &callees);
                    compileExpr(c->callee.get());
                    emitStash(ve->name, ln, p, callees);
                    compileExpr(c->args[1].get());
                    int at = emitVarOp(Op::APPEND_ARR, ve->name, ln, 0);
                    emit(Op::CALL, 2, 0.0, ln);
                    emitStore(ve->name, ln, storeHint, storeProven);
                    cur_->code[at].arg = here();
                    return;
                }
            }
            compileExpr(a->value.get());
            emitStore(ve->name, ln, storeHint, storeProven);
            return;
        }
        if (a->target->kind == Expr::Kind::Index) {
            compileIndexAssign(static_cast<IndexExpr*>(a->target.get()),
                               a->value.get(), a->range.startLine);
            return;
        }
        raiseError("compiler", "invalid assignment target", a->target->range);
    }

    // eval container, eval key, eval value, INDEX_SET, then write the updated container back to wherever a lives
    // recurse nested
    void compileIndexAssign(IndexExpr* idx, Expr* rhs, int line) {
        if (options.inPlaceOps && idx->object->kind == Expr::Kind::Var) {
            compileVarIndexSet(static_cast<VarExpr*>(idx->object.get())->name, idx->index.get(), rhs, line,
                               [&] { compileExpr(rhs); });
            return;
        }
        compileExpr(idx->object.get());  // container
        compileExpr(idx->index.get());   // key
        compileExpr(rhs);                // new value
        emit(Op::INDEX_SET, 0, 0.0, line);
        // stack now has the updated container; write it back
        writeBackTarget(idx->object.get(), line);
    }

    // `name[key] = <whatever emitValue pushes>`. INDEX_SET_VAR changes the variable where it lives when it can, and jumps over the
    // STORE. otherwise it leaves an updated copy for the STORE, which is what INDEX_SET did every time
    template <class F>
    void compileVarIndexSet(const std::string& name, Expr* key, const Expr* value, int line, F emitValue) {
        std::vector<std::string> callees;
        Purity p = purityOf(key, &callees);
        if (value) { Purity pv = purityOf(value, &callees); if (pv > p) p = pv; }   // null value: nothing to run
        if (p != Purity::CallFree) emitStash(name, line, p, callees);
        compileExpr(key);
        emitValue();
        int at = emitVarOp(Op::INDEX_SET_VAR, name, line, p == Purity::CallFree ? 0 : kStash);
        emitStore(name, line, TypeSet::Any(), true);   // the container goes back into its own variable: same type
        cur_->code[at].arg = here();
    }

    // after INDEX_SET leaves the updated container on the stack store it back into the appropriate target
    void writeBackTarget(Expr* target, int line) {
        if (target->kind == Expr::Kind::Var) {
            auto* ve = static_cast<VarExpr*>(target);
            emitStore(ve->name, line, TypeSet::Any(), true);
        } else if (target->kind == Expr::Kind::Index) {
            auto* idx = static_cast<IndexExpr*>(target);
            std::string tmp = "__wb_tmp_" + std::to_string(here());
            int tmpSlot = cur_->internLocal(tmp);
            emit(Op::DEFINE, cur_->intern(tmp), 0.0, line, TypeSet::Any(), tmpSlot);  // pop+store the updated sub-container
            if (options.inPlaceOps && idx->object->kind == Expr::Kind::Var) {
                compileVarIndexSet(static_cast<VarExpr*>(idx->object.get())->name, idx->index.get(), nullptr, line, [&] {
                    emit(Op::LOAD, cur_->intern(tmp), 0.0, line, TypeSet::Any(), tmpSlot);
                });
                return;
            }
            compileExpr(idx->object.get());                  // outer container
            compileExpr(idx->index.get());                   // outer key
            emit(Op::LOAD, cur_->intern(tmp), 0.0, line, TypeSet::Any(), tmpSlot);    // reload updated sub-container as the value
            emit(Op::INDEX_SET, 0, 0.0, line);
            writeBackTarget(idx->object.get(), line);
        } else {
            raiseError("compiler", "cannot assign to temporary", {});
        }
    }

    // function/lambda helper

    // compile a function body into a new chunk, return its index in prog_.chunks
    int compileFunction(
        const std::string&        name,
        const std::vector<Param>& params,
        TypeSet                   returnType,
        const std::vector<StmtPtr>& body,
        const SourceRange&        range,
        const std::string&        sourceFile)
    {
        for (const Param& p : params)
            if (p.elem && p.elem->isHost)
                raiseError("compiler", "the parameter type 'host' of '" + name + "' is for aembr, the compiler: embr does not run it", range);
        for (const Param& p : params)
            if (p.def.kind != ParamDefault::None || p.inout)
                raiseError("compiler", "a default value or an in-out parameter (&name) on '" + name + "' is for aembr, the compiler: embr does not run it", range);
        auto chunk = std::make_shared<CompiledChunk>();
        chunk->name       = name;
        chunk->params     = params;
        chunk->returnType = returnType;
        chunk->definedAt  = range;
        chunk->sourceFile = sourceFile;
        // every parameter gets a slot up front, even one the body never
        // references, pushFrame() binds args straight into slots by name
        // via slotOf(), which must never miss for a declared parameter.
        for (auto& p : params) chunk->internLocal(p.name);

        int idx = (int)prog_.chunks.size();
        for (int k = 0; k < idx; ++k)
            if (prog_.chunks[k].get() == cur_) { chunk->parentIdx = k; break; }
        prog_.chunks.push_back(chunk);

        // switch compilation target to the new chunk
        // break/continue can't cross a fn boundary, so start this chunk with an empty loop context too
        CompiledChunk* savedChunk      = cur_;
        int            savedScopeDepth = scopeDepth_;
        int            savedTryDepth   = tryDepth_;
        auto           savedLoopStack  = std::move(loopStack_);
        cur_        = chunk.get();
        scopeDepth_ = 0;
        tryDepth_   = 0;
        loopStack_.clear();

        for (auto& s : body) compileStmt(s.get());

        // implicit return 0 at end of function if no explicit return reached
        emit(Op::PUSH_NIL);
        emit(Op::RETURN);

        cur_        = savedChunk;
        scopeDepth_ = savedScopeDepth;
        tryDepth_   = savedTryDepth;
        loopStack_  = std::move(savedLoopStack);
        return idx;
    }

    // runs once every chunk of the program is written. scoping is lexical, so the compiler can tell, for every use of a name, where
    // the name lives:
    //  - a local of this function: a parameter, a name a DEFINE* op binds, or a name a bare assignment (STORE) makes when no
    //    enclosing function has it. a local that an inner function uses is "boxed": it lives in a cell, so the closure and the
    //    frame share it, and a loop variable gets a fresh cell per iteration
    //  - a variable of an enclosing function (an upvalue): the closure holds the cell, and the name is an index into them
    //  - neither: a global. in a function it gets kFreeSlot and a read goes straight through the global cache, at the top
    //    level it becomes a global op
    // slots are renumbered so only real locals take one
    void resolveNames() {
        const size_t n = prog_.chunks.size();
        using NameSet = std::unordered_set<std::string>;
        std::vector<NameSet> definite(n), stores(n), uses(n), binds(n), freeNames(n), boxed(n);
        auto isDefine = [](Op op) { return op == Op::DEFINE || op == Op::DEFINE_TYPED; };
        for (size_t i = 0; i < n; ++i) {
            const CompiledChunk& c = *prog_.chunks[i];
            for (auto& pr : c.params) definite[i].insert(pr.name);
            for (const Instruction& in : c.code) {
                if (isDefine(in.op)) { definite[i].insert(c.str(in.operand)); continue; }
                if (in.slot < 0) continue;
                uses[i].insert(c.str(in.operand));
                if (in.op == Op::STORE && i != 0) stores[i].insert(c.str(in.operand));
            }
            for (auto& set : c.pureSets) for (int idx : set) uses[i].insert(c.str(idx));
        }
        // the function around that has the name, nearest first (-1: none). a parent always comes before its children
        auto binderOf = [&](size_t i, const std::string& nm) {
            for (int a = prog_.chunks[i]->parentIdx; a >= 0; a = prog_.chunks[a]->parentIdx)
                if (binds[a].count(nm)) return a;
            return -1;
        };
        for (size_t i = 0; i < n; ++i) {
            binds[i] = definite[i];
            for (auto& nm : stores[i]) if (binderOf(i, nm) < 0) binds[i].insert(nm);
        }
        // the names each function (and the ones inside it) needs from outside it
        for (size_t i = n; i-- > 1; ) {
            for (auto& nm : uses[i]) if (!binds[i].count(nm)) freeNames[i].insert(nm);
            int par = prog_.chunks[i]->parentIdx;
            if (par > 0)
                for (auto& nm : freeNames[i]) if (!binds[par].count(nm)) freeNames[par].insert(nm);
            for (auto& nm : freeNames[i]) { int b = binderOf(i, nm); if (b >= 0) boxed[b].insert(nm); }
        }

        // pass 1: number the locals, boxed ones get a cell slot
        for (size_t i = 0; i < n; ++i) {
            CompiledChunk& c = *prog_.chunks[i];
            std::vector<std::string> ordered;
            for (auto& kv : c.localSlots) if (binds[i].count(kv.first)) ordered.push_back(kv.first);
            std::sort(ordered.begin(), ordered.end());
            std::unordered_map<std::string, int> slots;
            int next = 0;
            for (auto& nm : ordered) {
                int s = next++;
                bool box = boxed[i].count(nm) > 0;
                slots[nm] = box ? kBoxedBase - s : s;
                if (box) c.hasBoxed = true;
            }
            c.localSlots    = std::move(slots);
            c.numLocalSlots = next;
        }
        // pass 2: the upvalues, parents first so a function can point at its parent's own upvalues
        for (size_t i = 1; i < n; ++i) {
            CompiledChunk& c = *prog_.chunks[i];
            std::vector<std::string> ordered;
            for (auto& nm : freeNames[i]) if (binderOf(i, nm) >= 0) ordered.push_back(nm);
            std::sort(ordered.begin(), ordered.end());
            for (auto& nm : ordered) {
                int b = binderOf(i, nm);
                CompiledChunk::UpvalDesc d;
                if (b == c.parentIdx) { d.fromParentLocal = true;  d.index = boxedIndex(prog_.chunks[b]->localSlots.at(nm)); }
                else                  { d.fromParentLocal = false; d.index = prog_.chunks[c.parentIdx]->upvalNames.at(nm); }
                c.upvalNames[nm] = (int)c.upvalDescs.size();
                c.upvalDescs.push_back(d);
            }
        }
        // pass 3: point every instruction at where its name lives
        for (size_t i = 0; i < n; ++i) {
            CompiledChunk& c = *prog_.chunks[i];
            for (Instruction& in : c.code) {
                if (in.slot < 0) continue;
                const std::string& nm = c.str(in.operand);
                auto it = c.localSlots.find(nm);
                if (it != c.localSlots.end()) { in.slot = it->second; continue; }
                auto up = c.upvalNames.find(nm);
                if (up != c.upvalNames.end()) { in.slot = kUpvalBase - up->second; continue; }
                if (i == 0) {
                    in.slot = -1;
                    if (in.op == Op::LOAD)  in.op = Op::LOAD_GLOBAL;
                    if (in.op == Op::STORE) in.op = Op::STORE_GLOBAL;
                } else {
                    in.slot = kFreeSlot;
                }
            }
        }
    }

    // variable load/store helpers

    // LOAD/STORE for everything except true top level
    void emitLoad(const std::string& name0, int line) {
        const std::string& name = renamed(name0);
        bool g = atTopLevel();
        emit(g ? Op::LOAD_GLOBAL : Op::LOAD, cur_->intern(name), 0.0, line, TypeSet::Any(), g ? -1 : cur_->internLocal(name));
    }

    // hint and proven come from the type checker (see AssignStmt::storeHint). a store back into a variable after an index
    // write (`a[i] = x`) changes nothing about the variable's type, so those pass proven
    void emitStore(const std::string& name0, int line, TypeSet hint = TypeSet::Any(), bool proven = false) {
        const std::string& name = renamed(name0);
        bool g = atTopLevel();
        emit(g ? Op::STORE_GLOBAL : Op::STORE, cur_->intern(name), 0.0, line, hint, g ? -1 : cur_->internLocal(name));
        if (proven) cur_->code.back().flags |= kProven;
    }
};


// one iteration in progress for a for loop
// lives on the owning CallFrame's iterStack (not the value stack) so a native callback re-entering the VM mid-loop can't corrupt it
struct IterState {
    int               varCount = 1;   // 1 or 2 loop variables per step
    Value::array_type keys;           // populated only for varCount == 2 (map iteration)
    Value::array_type items;          // values to iterate: 1-char strings, or map keys (mode Items)
    size_t            idx = 0;
    // an array is not copied: the loop holds the array's box (a counted reference, so a change made to the variable inside the loop
    // copies it first and the loop still sees the array as it was) and reads the elements from it. a range (`[0...n]`) that has not
    // been built is not built for the loop either: its numbers are worked out one at a time
    enum class Mode : uint8_t { Items, Array, Range };
    Mode              mode = Mode::Items;
    Value             src;            // mode Array
    size_t            count = 0;      // how many elements, modes Array and Range
    int64_t           rStart = 0, rStep = 1;   // mode Range
};

// every chunk of a compiled program shares one sibling list that contains the chunks themselves (a
// deliberate strong cycle, see CompiledChunk::siblings). register a teardown hook so the list is emptied when the
// Interpreter goes away and the chunks are freed instead of leaked at exit. weak_ptr: the hook must not keep
// the list alive by itself. call this for every program compiled for an Interpreter (scripts and modules).
inline void registerChunkTeardown(Interpreter& interp, const CompiledProgram& prog) {
    if (prog.chunks.empty() || !prog.chunks[0]->siblings) return;
    interp.addTeardownHook([w = std::weak_ptr<std::vector<std::shared_ptr<CompiledChunk>>>(prog.chunks[0]->siblings)] {
        if (auto list = w.lock()) list->clear();
    });
}

// one activation record on the call stack
// the VM's value stack. a std::vector here compiled into a call per push, which was visible in the dispatch loop,
// so this is the few operations the VM uses, written to inline. the stack never shrinks its memory
class ValueStack {
public:
    ValueStack() = default;
    ValueStack(const ValueStack&) = delete;
    ValueStack& operator=(const ValueStack&) = delete;
    ~ValueStack() { clear(); ::operator delete((void*)d_); }

    [[gnu::always_inline]] size_t size() const { return n_; }
    [[gnu::always_inline]] bool empty() const { return n_ == 0; }
    [[gnu::always_inline]] Value& back() { return d_[n_ - 1]; }
    [[gnu::always_inline]] Value& operator[](size_t i) { return d_[i]; }
    [[gnu::always_inline]] Value* data() { return d_; }
    [[gnu::always_inline]] size_t capacity() const { return cap_; }
    // for code that pushed and popped through raw pointers (VmRunner::fastRun): the values are already constructed or destroyed
    [[gnu::always_inline]] void setSize(size_t n) { n_ = n; }
    [[gnu::always_inline]] void push_back(Value&& v) {
        if (n_ == cap_) [[unlikely]] grow();
        new (d_ + n_) Value(std::move(v));
        ++n_;
    }
    [[gnu::always_inline]] void push_back(const Value& v) {
        if (n_ == cap_) [[unlikely]] { Value copy(v); grow(); new (d_ + n_) Value(std::move(copy)); ++n_; return; }
        new (d_ + n_) Value(v);
        ++n_;
    }
    [[gnu::always_inline]] void pop_back() { --n_; d_[n_].~Value(); }
    void clear() { while (n_) pop_back(); }

private:
    [[gnu::noinline]] void grow() {
        size_t nc = cap_ ? cap_ * 2 : 256;
        Value* nd = static_cast<Value*>(::operator new(nc * sizeof(Value)));
        for (size_t i = 0; i < n_; ++i) { new (nd + i) Value(std::move(d_[i])); d_[i].~Value(); }
        ::operator delete((void*)d_);
        d_ = nd; cap_ = nc;
    }
    Value* d_ = nullptr;
    size_t n_ = 0, cap_ = 0;
};

// the declared or first-value types of a frame's locals, by name. a frame has a handful at most, so a flat list beats a hash map (a typed
// local used to pay a hash and an allocation every time its block ran)
class LocalTypes {
public:
    bool empty() const { return v_.empty(); }
    void clear() { v_.clear(); }
    // the names are pointers into a chunk's string table, which lives as long as anything can run that chunk (see CallFrame::chunk)
    const TypeSet* find(const std::string& n) const { for (const auto& e : v_) if (*e.first == n) return &e.second; return nullptr; }
    void set(const std::string& n, TypeSet t) { for (auto& e : v_) if (*e.first == n) { e.second = t; return; } v_.emplace_back(&n, t); }
    void erase(const std::string& n) {
        for (size_t i = 0; i < v_.size(); ++i)
            if (*v_[i].first == n) { if (i + 1 != v_.size()) v_[i] = v_.back(); v_.pop_back(); return; }
    }
private:
    std::vector<std::pair<const std::string*, TypeSet>> v_;
};

// a vector of plain bytes or small structs whose assign() is inline: a call sizes the defined flags and the types of its locals this way,
// and std::vector's assign was a function call each
template <class T>
class FlatVec {
public:
    FlatVec() = default;
    FlatVec(FlatVec&&) = default;
    FlatVec& operator=(FlatVec&&) = default;
    [[gnu::always_inline]] size_t size() const { return n_; }
    [[gnu::always_inline]] bool empty() const { return n_ == 0; }
    [[gnu::always_inline]] T& operator[](size_t i) { return d_[i]; }
    [[gnu::always_inline]] const T& operator[](size_t i) const { return d_[i]; }
    [[gnu::always_inline]] T* data() { return d_.get(); }
    [[gnu::always_inline]] const T* data() const { return d_.get(); }
    [[gnu::always_inline]] void clear() { n_ = 0; }
    [[gnu::always_inline]] void assign(size_t n, T v) {
        if (n > cap_) [[unlikely]] grow(n);
        for (size_t i = 0; i < n; ++i) { d_[i] = v; __asm__ volatile("" ::: "memory"); }   // a few bytes: a plain loop, not a memset call
        n_ = n;
    }
private:
    [[gnu::noinline]] void grow(size_t n) { d_.reset(new T[n]); cap_ = n; }
    std::unique_ptr<T[]> d_;
    size_t n_ = 0, cap_ = 0;
};

struct CallFrame {
    // written out so building a frame doesn't first zero all of it (an implicit default constructor does that, and
    // it was a visible part of the cost of a call)
    CallFrame() {}
    // the function being executed. a plain pointer: compiled chunks are never freed while anything can run them (a chunk keeps its
    // siblings, itself included, alive, see CompiledChunk::siblings), and a call shouldn't pay two atomic counts to hold one
    CompiledChunk*                           chunk = nullptr;
    int                                      ip = 0;      // instruction pointer into chunk->code
    int                                      stackBase;   // index of this frame's first local on the value stack

    // this frame's locals, indexed by the slot the compiler picked for each name (CompiledChunk::resolveNames).
    // slotDefined[i] says whether slot i holds a value right now: cleared by SCOPE_POP, set by DEFINE or the first STORE
    // slots is a shared_ptr so its address survives frames_ growing, because the gc has it registered as a root
    std::vector<Value>                       slots;
    FlatVec<uint8_t>                         slotDefined;

    // the locals that an inner function captures live in cells instead, indexed by the same slot number. null when the
    // chunk has none. a cell outlives the frame if a closure holds it. shared_ptr for the same reason as slots
    std::shared_ptr<std::vector<std::shared_ptr<Cell>>> cells;

    // this frame's own local `name`, or null if it isn't defined here. does not look at the captured variables or globals
    // (callers do that). knownSlot is the slot code the compiler already picked for the instruction running now, leave
    // it at -2 to look it up by name
    Value* findOwn(const std::string& name, int knownSlot = -2) {
        int sc = knownSlot != -2 ? knownSlot : chunk->slotOf(name);
        if (sc >= 0) {
            if (sc >= (int)slotDefined.size() || !slotDefined[sc]) return nullptr;
            return &slots[sc];
        }
        if (isBoxedSlot(sc)) {
            size_t n = (size_t)boxedIndex(sc);
            if (!cells || n >= cells->size()) return nullptr;
            const auto& c = (*cells)[n];
            return c && c->defined ? &c->v : nullptr;
        }
        return nullptr;
    }

    // unconditionally (re)defines this frame's own local, used by DEFINE* (always the current frame) and by
    // resolveStore()'s make-a-new-local path. a boxed local gets a fresh cell, unless a closure already made one early
    // for this variable (it is still undefined), then that one is filled so the closure sees it
    void defineOwn(Interpreter& interp, const std::string& name, Value v, int knownSlot = -2) {
        int sc = knownSlot != -2 ? knownSlot : chunk->slotOf(name);
        if (sc >= 0) {
            slots[sc] = std::move(v);
            slotDefined[sc] = true;
            return;
        }
        // every DEFINE/STORE target was given a slot at compile time. getting here without one means
        // a name reached the own-local path without being compiled as a local, which should not happen
        assert(isBoxedSlot(sc));
        auto& c = (*cells)[(size_t)boxedIndex(sc)];
        if (!c || c->defined) c = interp.newCell();
        c->v = std::move(v);
        c->defined = true;
    }

    // clears this frame's own local (SCOPE_POP and try-catch scope unwind). a boxed local just lets go of its cell,
    // a closure that captured it keeps it
    void eraseOwn(const std::string& name) {
        int sc = chunk->slotOf(name);
        if (sc >= 0) { if (sc < (int)slotDefined.size()) slotDefined[sc] = false; }
        else if (isBoxedSlot(sc) && cells) (*cells)[(size_t)boxedIndex(sc)].reset();
    }

    // the type a variable has (declared, or the type of its first value), tested by every store to it that the compiler could not settle.
    // a local with a slot keeps it in slotTypes[slot] (any when it has none), the others (a local in a cell) by name in localTypes
    FlatVec<TypeSet>                         slotTypes;
    // where this frame's locals are, for the hot loop. set by updateFast() once the frame is built, and good until it is popped
    struct Fast {
        Value*   slots = nullptr;
        uint8_t* def = nullptr;
        TypeSet* types = nullptr;
        uint32_t nslots = 0;
        bool     modLayer = false;   // some layer this function captured is a module's own scope
        std::vector<std::shared_ptr<Cell>>* ups = nullptr;   // the cells of the variables this closure captured, or null
    } fast;
    void updateFast() {
        fast.slots = slots.data(); fast.def = slotDefined.data(); fast.types = slotTypes.data();
        fast.nslots = (uint32_t)slotDefined.size();
        fast.modLayer = captured && captured->hasModule;
        fast.ups = captured && captured->upvals ? captured->upvals.get() : nullptr;
    }
    LocalTypes                               localTypes;
    int slotFor(const std::string& name, int knownSlot) const { return knownSlot != -2 ? knownSlot : chunk->slotOf(name); }
    void setType(const std::string& name, int knownSlot, TypeSet t) {
        const int sc = slotFor(name, knownSlot);
        if (sc >= 0 && (size_t)sc < slotTypes.size()) slotTypes[(size_t)sc] = t;
        else if (t.isAny()) { if (!localTypes.empty()) localTypes.erase(name); }
        else localTypes.set(name, t);
    }
    TypeSet typeOf(const std::string& name, int knownSlot) const {
        const int sc = slotFor(name, knownSlot);
        if (sc >= 0 && (size_t)sc < slotTypes.size()) return slotTypes[(size_t)sc];
        const TypeSet* t = localTypes.find(name);
        return t ? *t : TypeSet::Any();
    }
    std::shared_ptr<CaptureFrame>            captured;    // closure env may be null
    TypeSet                                  returnType = TypeSet::Any();

    // per-frame so a nested call mid-loop-body/mid-iteration can't corrupt the caller's own bookkeeping
    int                                   scopeDepth = 0;
    // names defined inside each open block, so SCOPE_POP can erase them. one flat list plus where each block's names start,
    // so opening and closing a block (every loop turn) allocates and frees nothing
    struct ScopedName { const std::string* name; int slot; };   // a pointer into the chunk's string table. slot: the local's slot if it has one, else -2 (found by name)
    std::vector<ScopedName>               scopeFlat;
    std::vector<uint32_t>                 scopeMarks;
    std::vector<IterState>                iterStack;

    // one entry per currently-open try block in this frame. LIFO
    struct TryHandler {
        int    catchIp;             // ip of the catch block's own SCOPE_PUSH
        int    scopeDepthAtEntry;   // this frame's scopeDepth when TRY_PUSH ran
        size_t stackBase;           // VmRunner::stack_ size when TRY_PUSH ran
        size_t iterDepth;           // this frame's iterStack size when TRY_PUSH ran
    };
    std::vector<TryHandler> tryHandlers;

    // true for a frame pushed by invokeChunk(): entered because native code (a plugin callback,
    // for_each_do, sort_cmp, ...) called into script code, not by a script CALL instruction, so there is
    // no script call site, error traces report such a frame as "called from native code" (line 0)
    bool fromNative = false;

    // set only on the persistent frame VmRunner::run() creates for its very first call
    // doReturn() special-cases it to survive running off the end of its chunk so locals defined by one run() call are still there for the next
    bool isPersistentTop = false;

    // back to a fresh frame, keeping the memory of the buffers for the next call. the locals are emptied here so what they
    // held is let go of when the call ends, as it was when the frame was destroyed
    [[gnu::always_inline]] void reset() {
        chunk = nullptr;
        ip = 0;
        // the locals are let go of here, but the buffer keeps its size and every slot past this frame's own holds a plain number, so the next
        // call only grows it when it needs more
        {
            Value* sv = slots.data();
            const size_t n = std::min(slotDefined.size(), slots.size());
            for (size_t i = 0; i < n; ++i) if (sv[i].tg >= 2) sv[i] = Value();
        }
        slotDefined.clear();
        if (cells) cells.reset();
        if (!localTypes.empty()) localTypes.clear();
        slotTypes.clear();
        if (captured) captured.reset();
        returnType = TypeSet::Any();
        scopeDepth = 0;
        if (!scopeFlat.empty()) scopeFlat.clear();
        scopeMarks.clear();
        if (!iterStack.empty()) iterStack.clear();
        if (!tryHandlers.empty()) tryHandlers.clear();
        fromNative = false;
        isPersistentTop = false;
    }
};

// the call frames. a popped frame is not destroyed, it is cleaned and kept for the next call at that depth, so a call
// reuses the buffers (locals, scope names, ...) of an earlier one instead of allocating and freeing them. the few
// operations the VM uses; a reference to a frame stays valid until the stack grows past what it has held before
class FrameStack {
public:
    [[gnu::always_inline]] size_t size() const { return n_; }
    [[gnu::always_inline]] bool empty() const { return n_ == 0; }
    [[gnu::always_inline]] CallFrame& back() { return v_[n_ - 1]; }
    [[gnu::always_inline]] const CallFrame& back() const { return v_[n_ - 1]; }
    [[gnu::always_inline]] CallFrame& operator[](size_t i) { return v_[i]; }
    [[gnu::always_inline]] const CallFrame& operator[](size_t i) const { return v_[i]; }
    // a clean frame on top (its locals buffer may be left over from an earlier call, already emptied)
    [[gnu::always_inline]] CallFrame& emplace_back() {
        if (n_ == cap_) [[unlikely]] { v_.emplace_back(); cap_ = v_.size(); }
        return v_[n_++];
    }
    [[gnu::always_inline]] void pop_back() { v_[--n_].reset(); }
private:
    std::vector<CallFrame> v_;
    size_t n_ = 0;
    size_t cap_ = 0;   // v_.size(), kept so a push doesn't divide by the size of a CallFrame
};

// what compileSource() does besides compiling
struct CompileOptions {
    bool inPlaceOps = true;    // see Compiler::Options
    bool elideScopes = true;   // see Compiler::Options
    bool closedWorld = false;  // the file is the whole program, see typecheck::Options
    bool typeCheck = true;     // run the static type checker and stop at its first error
    bool useTypes = true;      // let the compiler use what the checker worked out (typed and proven stores). with typeCheck off
                               // the checker still runs, its errors are ignored. the fuzzer's oracle turns this off for the second run
    bool preloadPlugins = false;  // register the plugins a script imports at its top level before checking it, so their
                                  // signatures are known. only for the script `embr` runs, see typecheck::Options::interp
};

// defined in vm_bytecode.h (included right after this file). import uses them to reuse and load compiled modules
struct VmbrHeader;
inline CompiledProgram compileSourcePreferCached(const std::string& src, Interpreter& interp,
                                                 const std::string& filename, const std::string& cacheDir,
                                                 const CompileOptions& opts = CompileOptions{});
inline CompiledProgram deserializeProgram(const std::string& bytes, const std::string& filename, VmbrHeader* headerOut);
inline std::optional<std::string> readWholeFile(const std::string& path);
inline void verifyStack(const CompiledProgram& prog, const char* what);

// the member of an overload group for some arguments, defined after VmRunner (see there)
inline const Value* pickOverload(Interpreter& interp, const std::string& base, const Value* args, size_t n, std::string& err);

class VmRunner final : private Interpreter::GcFrameSource {
public:
    enum class State { Ready, Running, Suspended, Done, Error };

    explicit VmRunner(Interpreter& interp) : interp_(interp) {
        gcHandle_ = interp_.gcAddFrameSource(this);
        if (!interp_.backend.invoke) installBackend(interp_);   // so a plugin can call back into script code (core/invoke.h)
    }
    VmRunner(const VmRunner&) = delete;
    VmRunner& operator=(const VmRunner&) = delete;

    // the destructor doesn't touch interp_: cli.cpp keeps its VmRunner in a static that outlives the Interpreter, so it may
    // be gone by then. it only drops its entry in the gc's list of runners through a weak handle, which is safe either way
    ~VmRunner() { Interpreter::gcRemoveFrameSource(gcHandle_, this); }

    // what the gc needs from the running frames: the locals, the variable cells, and the module scopes they captured
    void gcCollectRoots(std::vector<std::shared_ptr<Scope>>& scopes,
                        std::vector<const std::vector<Value>*>& slots,
                        std::vector<const std::vector<std::shared_ptr<Cell>>*>& cells) const override {
        for (size_t i = 0; i < frames_.size(); ++i) {
            const CallFrame& f = frames_[i];
            slots.push_back(&f.slots);
            if (f.cells) cells.push_back(f.cells.get());
            if (f.captured) {
                if (f.captured->gcRoots) scopes.insert(scopes.end(), f.captured->gcRoots->begin(), f.captured->gcRoots->end());
                if (f.captured->upvals) cells.push_back(f.captured->upvals.get());
            }
        }
    }

    // run a compiled program to completion or until YIELD
    // calling this again on the same VmRunner after a prior call completed reuses the persistent top-level frame so its locals survive
    Value run(const CompiledProgram& prog) {
        beginRun(prog);
        return dispatch();
    }

    // run(), but gives up after maxSteps instructions. returns true if the program finished, false if it ran out of steps
    // (it is then left suspended where it stopped). for an embedder that runs code it doesn't trust, or fuzzing
    bool runLimited(const CompiledProgram& prog, size_t maxSteps) {
        beginRun(prog);
        for (size_t n = 0; n < maxSteps; ++n) {
            if (state_ != State::Running || frames_.empty()) { state_ = State::Done; refreshHot(); return true; }
            try {
                execOneOutOfLine();
            } catch (EmbrError& e) {
                if (!handleTry(e, 0)) throw;
            } catch (const std::bad_alloc&) { outOfMemory(0);
            } catch (const std::length_error&) { outOfMemory(0); }
        }
        return state_ != State::Running || frames_.empty();
    }

private:
    // the part of run() before the dispatch loop: sets up the persistent top-level frame for `prog`
    void beginRun(const CompiledProgram& prog) {
        prog_ = &prog;
        quickenProgram(prog);
        if (frames_.empty()) {
            CallFrame& frame = frames_.emplace_back();
            frame.chunk          = prog.chunks[0].get();
            frame.stackBase      = 0;
            frame.isPersistentTop = true;
            frame.slots.assign(frame.chunk->numLocalSlots, Value());
            frame.slotDefined.assign(frame.chunk->numLocalSlots, false);
            frame.slotTypes.assign(frame.chunk->numLocalSlots, TypeSet::Any());
            frame.cells = std::make_shared<std::vector<std::shared_ptr<Cell>>>(frame.chunk->hasBoxed ? frame.chunk->numLocalSlots : 0);
            frame.updateFast();
            syncTop();
        } else {
            while (frames_.size() > 1) {
                dropTopFrame();
            }
            stack_.clear();
            auto& f = frames_[0];
            // reusing the persistent top-level frame for another program: only block-local names live in its slots (a
            // top-level name is a global) and a block ends by erasing them, so the new chunk starts with a clean frame
            f.chunk = prog.chunks[0].get();
            f.ip    = 0;
            f.scopeDepth = 0;
            f.scopeFlat.clear(); f.scopeMarks.clear();
            f.iterStack.clear();
            f.localTypes.clear();
            f.slots.assign(f.chunk->numLocalSlots, Value());
            f.slotDefined.assign(f.chunk->numLocalSlots, false);
            f.slotTypes.assign(f.chunk->numLocalSlots, TypeSet::Any());
            f.cells->assign(f.chunk->hasBoxed ? f.chunk->numLocalSlots : 0, nullptr);
            f.updateFast();
            syncTop();
        }
        state_ = State::Running; refreshHot();
    }

public:
    // execute a single instruction returns false when Done/Error/Suspended
    bool step() {
        if (state_ != State::Running) return false;
        if (frames_.empty()) { state_ = State::Done; refreshHot(); return false; }
        execOneOutOfLine();
        return state_ == State::Running;
    }

    // resume after a YIELD
    Value resume() {
        if (state_ == State::Suspended) state_ = State::Running; refreshHot();
        return dispatch();
    }

    // start a fresh runner directly on a VM-compiled script function (instead of a whole program like run())
    // the callee's chunk becomes the first frame and runs through the same dispatch() loop as run()/resume()
    // use this, not invoke()/invokeChunk(), when the code must be able to YIELD: invoke() assumes the
    // callee always runs to completion and would drop the suspended frames. coroutine_create() does this,
    // with one VmRunner per coroutine. call it once per runner, then resume() for every later call
    Value start(const Value& callee, const std::vector<Value>& args, const SourceRange& site = {}) {
        if (!frames_.empty())
            vmError("VmRunner::start() called on a runner that has already been started", site.startLine);
        if (!callee.isCallable())
            vmError("cannot start: value is not callable (got " + callee.typeName() + ")", site.startLine);
        const auto& c = callee.asCallable();
        if (!c.isScript() || !c.script.compiledChunk)
            vmError("cannot start: callee must be a script function compiled by the VM backend", site.startLine);
        auto* chunk = static_cast<CompiledChunk*>(c.script.compiledChunk.get());
        if (chunk->siblings) for (const auto& sib : *chunk->siblings) quickenChunk(*sib);
        quickenChunk(*chunk);
        pushFrame(chunk, c.script.captured, args, c.script.returnType, c.script.name);
        state_ = State::Running; refreshHot();
        return dispatch();
    }

    // pause this runner right now, as if Op::YIELD had just run. for a native function (called via CALL)
    // that wants to hand control back to whoever called run()/resume()/start(). simpler than inject()ing
    // a YIELD because it doesn't touch injBuf_. the caller is coroutine_yield() in plugins/coroutine
    void suspend() { state_ = State::Suspended; refreshHot(); }

    // drops every frame this runner still has: gc roots and call-depth count go back to the Interpreter. for a
    // coroutine that is dropped while suspended, or died with an error. the destructor can't do it (see above), so the
    // owner calls this while the Interpreter is still alive
    void releaseFrames() {
        while (!frames_.empty()) {
            CallFrame& f = frames_.back();
            if (!f.isPersistentTop) interp_.exitCall();
            frames_.pop_back(); syncTop();
        }
        stack_.clear();
        injBuf_.reset();
        state_ = State::Done; refreshHot();
    }

    // replace the value on top of the stack, or push one if the stack is empty. coroutine_resume() uses it:
    // coroutine_yield() pushes a placeholder return value before suspending (a native must return something),
    // and on resume this swaps the placeholder for the values passed to coroutine_resume()
    void setYieldResult(Value v) {
        if (stack_.empty()) stack_.push_back(std::move(v));
        else                stack_.back() = std::move(v);
    }

    // invoke a callable from C++. used by embr::invoke() and by native functions that wanna operate on runner's frame stack directly
    Value invoke(const Value& callee, const std::vector<Value>& args,
                 const SourceRange& site = {}) {
        if (!callee.isCallable()) {
            // dunder fallback, see core/registry.h's resolveDunder()
            Value dfn;
            if (resolveDunder(interp_, callee, "call", dfn)) {
                std::vector<Value> dargs;
                dargs.reserve(args.size() + 1);
                dargs.push_back(callee);
                dargs.insert(dargs.end(), args.begin(), args.end());
                return invoke(dfn, dargs, site);
            }
            vmError("value is not callable (got " + callee.typeName() + ")", site.startLine);
        }

        const auto& c = callee.asCallable();
        if (c.isNative()) {
            checkNativeSig(c, args, site.startLine, true);
            try {
                return c.native(args);
            } catch (EmbrError& e) {
                if (!e.hasLocation && site.valid()) {
                    // carry over trace frames collected if this native called back into script code
                    try { raiseError(e.keepContext ? e.context : c.name(), e.message, site, interp_.sourceMap()); }
                    catch (EmbrError& located) { located.trace = std::move(e.trace); located.keepContext = e.keepContext; throw; }
                }
                throw;
            }
        }

        // script call via compiled chunk
        const ScriptFn& sf = c.script;
        if (sf.compiledChunk) {
            auto* chunk = static_cast<CompiledChunk*>(sf.compiledChunk.get());
            return invokeChunk(chunk, sf.captured, args, site, sf.returnType, sf.name);
        }

        vmError("cannot invoke '" + sf.name + "': it was not compiled by the VM", site.startLine);
    }

    // inject instructions to be executed before the current instruction pointer resumes
    // useful for plugins that want to prepend a loop or async continuation into the running stream
    //
    // the new code goes in at the current ip, so injecting twice in a row only goes in front of what
    // hasn't run yet. inserting at the start of the buffer would replay instructions that had already
    // run, and could loop forever
    void inject(std::vector<Instruction> code) {
        auto& cur = frames_.back();
        if (!injBuf_) {
            // copy remaining instructions from the current chunk into a mutable buffer
            injBuf_ = std::make_unique<InjBuf>();
            injBuf_->assign(cur.chunk->code.begin() + cur.ip, cur.chunk->code.end());
            injBuf_->lines.assign(cur.chunk->lines.begin() + std::min<size_t>((size_t)cur.ip, cur.chunk->lines.size()), cur.chunk->lines.end());
            injBuf_->lines.resize(injBuf_->size(), 0);
            cur.ip = 0;
        }
        // insert right before whatever hasn't run yet (not at the start of the buffer)
        injBuf_->insert(injBuf_->begin() + cur.ip, code.begin(), code.end());
        injBuf_->lines.insert(injBuf_->lines.begin() + cur.ip, code.size(), 0);
        refreshHot();
    }

    State           state()   const { return state_; }
    Interpreter&    interp()        { return interp_; }

private:
    Interpreter&                          interp_;
    const CompiledProgram*                prog_   = nullptr;
    FrameStack                            frames_;
    // the running frame's chunk and code, kept in step with frames_ (syncTop() after every push and pop), so
    // execOne doesn't chase frames_.back().chunk->code for every instruction
    CompiledChunk*     topChunk_ = nullptr;
    const Instruction* topCode_  = nullptr;
    size_t             topLen_   = 0;
    CallFrame*         top_      = nullptr;   // &frames_.back(), kept by syncTop(); the loop reads the running frame through it
    const int*         topLines_ = nullptr;   // topChunk_->lines, one per instruction (a chunk always has as many lines as code)
    [[gnu::always_inline]] void syncTop() {
        if (frames_.empty()) { top_ = nullptr; topChunk_ = nullptr; topCode_ = nullptr; topLen_ = 0; topLines_ = nullptr; hot_ = false; return; }
        top_ = &frames_.back();
        topChunk_ = frames_.back().chunk;
        if (!topChunk_->fastReady) [[unlikely]] topChunk_->buildFast();
        const CompiledChunk::Fast& cf = topChunk_->fast;
        topCode_  = cf.code;
        topLen_   = (size_t)(cf.codeEnd - cf.code);
        topLines_ = cf.lines;
        refreshHot();
    }
    // true while the loops can run the next instruction straight from the chunk without looking at anything else: the
    // runner is running, there is a frame above the depth the loop stops at, and no code is injected. whatever changes
    // one of those calls refreshHot(); the loops only read hot_
    bool   hot_       = false;
    bool   fastOn_    = fastLoopEnabled();
    size_t stopDepth_ = 0;
    [[gnu::always_inline]] void refreshHot() {
        hot_ = fastOn_ && state_ == State::Running && topCode_ && !injBuf_ && frames_.size() > stopDepth_;
    }
    ValueStack                            stack_;
    State                                 state_  = State::Ready;
    // optional mutable override buffer (used by inject())
    // injected code, with the source line of each instruction next to it
    struct InjBuf : std::vector<Instruction> { std::vector<int> lines; };
    std::unique_ptr<InjBuf> injBuf_;
    int lastLine_ = 0;   // the line of the instruction fetch() just returned

    // slot buffers from returned frames, kept so a call doesn't have to allocate them again
    std::weak_ptr<Interpreter::GcFrameSourceList> gcHandle_;

    // the argument list of a call that goes through a vector (natives, and script calls the short way in can't take). the vectors
    // are kept between calls, so a call doesn't allocate one. it is a vector in every other way
    std::vector<std::vector<Value>> argPool_;
    struct ArgVec : std::vector<Value> {
        VmRunner& r;
        ArgVec(VmRunner& rr, size_t n) : r(rr) {
            if (!r.argPool_.empty()) { static_cast<std::vector<Value>&>(*this) = std::move(r.argPool_.back()); r.argPool_.pop_back(); }
            resize(n);
        }
        ~ArgVec() {
            clear();
            if (r.argPool_.size() < 16 && capacity() <= 64) r.argPool_.push_back(std::move(static_cast<std::vector<Value>&>(*this)));
        }
    };

    void dropTopFrame() {
        frames_.pop_back(); syncTop();
        interp_.exitCall();
    }

    // stack helpers
    [[gnu::always_inline]] void   push(Value v)  { stack_.push_back(std::move(v)); }
    [[gnu::always_inline]] Value  pop()          { assert(!stack_.empty()); Value v = std::move(stack_.back()); stack_.pop_back(); return v; }
    [[gnu::always_inline]] Value& peek()         { assert(!stack_.empty()); return stack_.back(); }

    // frame/scope helpers

    // container[key] for INDEX_GET and INDEX_GET_VAR: pushes the element. never calls into script code
    void indexGet(const Value& cont, const Value& key, int line) {
        if (cont.isString()) {
            int ii = (int)key.asNumber();
            const auto& s = cont.asString();
            if (ii < 0 || ii >= (int)s.size())
                vmError("string index " + std::to_string(ii) + " out of bounds", line);
            push(Value(std::string(1, s[ii])));
        } else if (cont.isMap()) {
            if (!key.isString())
                vmError("map key must be string", line);
            // own slot, then (if key isn't one) the map's own "__proto__" chain, see lookupMapChain() in core/registry.h
            Value found;
            if (lookupMapChain(interp_, cont, key.asString(), found)) push(std::move(found));
            else vmError("key not found: " + key.asString(), line);
        } else if (cont.isArray()) {
            // an int64 index: a range can be longer than an int holds
            const double kd = key.isInt() ? 0.0 : key.asNumber();
            const int64_t ii = key.isInt() ? key.asInt() : (kd >= -9.2e18 && kd <= 9.2e18 ? (int64_t)kd : INT64_MIN);
            if (ii < 0 || (uint64_t)ii >= (uint64_t)cont.arraySize())
                vmError("array index " + std::to_string(ii) + " out of bounds", line);
            push(cont.arrayGet((size_t)ii));
        } else {
            vmError("cannot index " + cont.typeName(), line);
        }
    }

    // the variable an in-place op (INDEX_GET_VAR and friends) works on, or null if there is none. same lookup as LOAD
    Value* varForOp(const Instruction& in) { return varBySlot(in.operand, in.slot); }
    [[gnu::always_inline]] Value* varBySlot(int nameIdx, int slot) {
        CallFrame& f = frames_.back();
        if (slot == -1) return globalPtrOrNull(nameIdx);   // top-level code: a global
        if (slot == kFreeSlot && !(f.captured && f.captured->hasModule)) return globalPtrOrNull(nameIdx);
        if (slotIsOwnLocal(f, slot)) return &f.slots[slot];
        if (slot >= 0 && !(f.captured && f.captured->hasModule)) return globalPtrOrNull(nameIdx);   // an own local that is not defined: the global of that name
        if (slot < 0 && slot != kFreeSlot) return findVar(chunkStr(nameIdx), slot);   // a boxed local or a captured variable
        if (onlyGlobalsLeft(f)) return globalPtrOrNull(nameIdx);
        return findVar(chunkStr(nameIdx), slot);
    }
    // a plain local of the running frame. no closure can see it (a local a closure uses is boxed), so only this frame's own code can change it
    static bool slotIsOwnLocal(const CallFrame& f, int slot) {
        return slot >= 0 && slot < (int)f.slotDefined.size() && f.slotDefined[slot];
    }

    // true when every builtin this STASH_VAR's code calls still resolves to a native function (not a script function of the same name)
    bool pureCalleesNative(const Instruction& in) {
        CallFrame& f = frames_.back();
        for (int idx : f.chunk->pureSets[in.arg]) {
            const Value* c = onlyGlobalsLeft(f) ? globalPtrOrNull(idx) : findVar(chunkStr(idx), -2);
            if (!c || !c->isCallable() || !c->asCallable().isNative()) return false;
        }
        return true;
    }

    // the Value STASH_VAR pushes when the variable is read later instead. no real Value has this nesting depth
    // a callee that is pushed without a reference count (kCallee): a number whose payload points at where the function lives (a global, a local, a captured
    // variable), and that no real value looks like. only the CALL that the compiler put after it takes it, and looks at what is there then. anything it
    // does not run straight puts a real copy of the function where the mark was, and the ordinary call goes on
    static constexpr uint32_t kCalleeMark = 0xFFFFFFFEu;
    static bool isCalleeMark(const Value& v) { return v.tg == 0 && v.nestDepth == kCalleeMark; }
    static Value calleeMark(const Value* where) { Value m; m.u.p = const_cast<Value*>(where); m.nestDepth = kCalleeMark; return m; }
    // the value a stack entry stands for, when it is the callee: what a mark points at
    static const Value& calleeValue(const Value& v) { return isCalleeMark(v) ? *static_cast<const Value*>(v.u.p) : v; }
    static bool typedCallee(const Value& v) { return v.tg == 5 && v.asCallable().typed.thunk != nullptr; }
    // a typed native called with `argc` values that are all of the types it takes
    static bool typedFits(const Value::Callable& c, const Value* args, int argc) {
        const Value::Callable::Typed& t = c.typed;
        if (t.n != argc) return false;
        for (int i = 0; i < argc; ++i) if (!((t.mask[i] >> args[i].tg) & 1)) return false;
        return true;
    }
    // runs the typed native on the arguments where they sit. an error it raises gets the position of the call, like callSpanNative's
    [[gnu::noinline]] Value callTypedNative(const Value::Callable& c, const Value* args, int line) {
        try {
            return c.typed.thunk(c.typed.fn, c.typed.ctx, args);
        } catch (EmbrError& e) {
            if (!e.hasLocation) {
                try { raiseError(e.keepContext ? e.context : c.name(), e.message, errRange(line), interp_.sourceMap()); }
                catch (EmbrError& located) { located.trace = std::move(e.trace); located.keepContext = e.keepContext; throw; }
            }
            throw;
        }
    }
    static constexpr uint32_t kStashMark = 0xFFFFFFFFu;
    static bool isStashMark(const Value& v) { return v.nestDepth == kStashMark; }

    // `cont[key] = newVal` on the container itself, no copy. checks the index before it changes anything
    void indexSetInPlace(Value& cont, const Value& key, Value&& newVal, int line) {
        if (cont.isArray()) {
            int ii = (int)key.asNumber();
            if (ii < 0 || ii >= (int)cont.asArray().size())
                vmError("array index " + std::to_string(ii) + " out of bounds", line);
            cont.arraySet((size_t)ii, std::move(newVal));
        } else if (cont.isMap()) {
            if (!key.isString())
                vmError("map key must be string", line);
            cont.mapSet(key.asString(), std::move(newVal));
        } else if (cont.isString()) {
            int ii = (int)key.asNumber();
            if (ii < 0 || ii >= (int)cont.asString().size())
                vmError("string index " + std::to_string(ii) + " out of bounds", line);
            cont.stringSetAt((size_t)ii, newVal.asString());
        } else {
            vmError("cannot index-assign on " + cont.typeName(), line);
        }
    }

    // finds a global through the chunk's lookup cache (see CompiledChunk::GlobalCache). same answer as interp_.find()
    [[gnu::always_inline]] Value* globalPtrOrNull(int nameIdx) {
        CompiledChunk::GlobalCache& gc = topChunk_->globalCacheFor(nameIdx);
        if (gc.epoch == interp_.scopeEpoch()) return gc.ptr;
        if (Value* g = interp_.findForCache(chunkStr(nameIdx), gc.typed, &gc.type)) {
            gc.ptr = g; gc.epoch = interp_.scopeEpoch();
            return g;
        }
        return nullptr;
    }
    [[gnu::always_inline]] Value* globalPtr(int nameIdx, int line) {
        if (Value* g = globalPtrOrNull(nameIdx)) [[likely]] return g;
        globalMissing(nameIdx, line);
    }
    [[noreturn, gnu::noinline]] void globalMissing(int nameIdx, int line) {
        const std::string& nm = chunkStr(nameIdx);
        // same wording (and "did you mean") as resolveLoad()
        std::string hint = suggest(nm);
        vmError("undefined variable: " + nm + (hint.empty() ? "" : "\n  did you mean '" + hint + "'?"), line);
    }
    [[gnu::always_inline]] void pushGlobal(int nameIdx, int line) { stack_.push_back(*globalPtr(nameIdx, line)); }

    // pops the top of the stack into a global, same as interp_.set() (a declared type is still checked)
    // the value is tested against the type the global has recorded, which the cache keeps next to the pointer, so a file
    // compiled long ago (a cached one) or against other globals can't store a wrong type. hint is the variable's type
    // when the compiler knows it: it is what a brand new global is given
    [[gnu::always_inline]] void storeGlobal(int nameIdx, TypeSet hint = TypeSet::Any(), int line = 0) {
        CompiledChunk::GlobalCache& gc = topChunk_->globalCacheFor(nameIdx);
        if (gc.epoch == interp_.scopeEpoch() && !stack_.empty()) [[likely]] {
            if (gc.typed && !gc.type.contains(stack_.back().tag())) [[unlikely]] storeGlobalWrongType(nameIdx, gc, line);
            *gc.ptr = std::move(stack_.back());
            stack_.pop_back();
            return;
        }
        storeGlobalSlow(nameIdx, hint, line, gc);
    }
    [[noreturn, gnu::noinline]] void storeGlobalWrongType(int nameIdx, const CompiledChunk::GlobalCache& gc, int line) {
        vmError("cannot assign " + stack_.back().typeName() + " to '" + shownName(chunkStr(nameIdx)) + "' declared as " + gc.type.name(), line);
    }
    [[gnu::noinline]] void storeGlobalSlow(int nameIdx, TypeSet hint, int line, CompiledChunk::GlobalCache& gc) {
        (void)line;
        Value v = pop();
        const std::string& nm = chunkStr(nameIdx);
        interp_.set(nm, std::move(v), hint);   // checks the type. a brand new name takes the hint (or its value's type) and bumps the scope epoch
        bool typed = false;
        TypeSet ty;
        if (Value* g = interp_.findForCache(nm, typed, &ty)) { gc.ptr = g; gc.epoch = interp_.scopeEpoch(); gc.typed = typed; gc.type = ty; }
    }

    // true when a LOAD/STORE with no usable slot can only mean a global: the one and only frame, no captured layers
    // only the top-level code of a script or module is the one frame that makes globals. a function called from native code
    // (a test runner, a sort callback) is the first frame of its own runner too, but its variables are its own
    bool onlyGlobalsLeft(const CallFrame& f) const { return frames_.size() == 1 && f.isPersistentTop && !f.captured; }

    // the cell of a variable of an enclosing function that the running function captured, or null if it has none by this
    // name. knownSlot is the slot code of the instruction running now, -2 to look the name up
    Cell* upvalCell(const std::string& name, int knownSlot) {
        CallFrame& f = frames_.back();
        if (!f.captured || !f.captured->upvals) return nullptr;
        size_t u;
        if (isUpvalSlot(knownSlot)) u = (size_t)upvalIndex(knownSlot);
        else if (knownSlot == -2) {
            auto it = f.chunk->upvalNames.find(name);
            if (it == f.chunk->upvalNames.end()) return nullptr;
            u = (size_t)it->second;
        } else return nullptr;
        return u < f.captured->upvals->size() ? (*f.captured->upvals)[u].get() : nullptr;
    }

    // look up a name. scoping is lexical: the running function's own locals, then the variables of the enclosing functions it
    // captured (its upvalues), then the module scope it was written in (if any), then globals. the callers on the call stack
    // are never searched. knownSlot is the compile-time slot code for the instruction running now (see CallFrame::findOwn)
    Value resolveLoad(const std::string& name, int line, int knownSlot = -2) {
        if (Value* v = findVar(name, knownSlot)) return *v;
        // not found suggest closest match
        std::string hint = suggest(name);
        std::string msg  = "undefined variable: " + name;
        if (!hint.empty()) msg += "\n  did you mean '" + hint + "'?";
        vmError(msg, line);
    }

    // like resolveLoad() but hands back the variable itself, or null if there is no such variable
    Value* findVar(const std::string& name, int knownSlot) {
        CallFrame& f = frames_.back();
        if (Value* v = f.findOwn(name, knownSlot)) return v;
        if (Cell* c = upvalCell(name, knownSlot)) if (c->defined) return &c->v;
        if (f.captured)
            for (auto it = f.captured->layers.rbegin(); it != f.captured->layers.rend(); ++it) {
                auto found = (*it)->find(name);
                if (found != (*it)->end()) return &found->second;
            }
        return interp_.findMut(name);
    }

    // store to the running function's own local, else a captured variable, else a module-level name, else an existing global,
    // else make a new local (a new global at the top level). a caller's locals are never written. knownSlot: see resolveLoad()
    // hint: the variable's type if the compiler knows it. settled: the value was already tested against it (or is proven
    // to fit), so no recorded type needs checking. a store that makes a new local records its type: the hint, or the value's
    void resolveStore(const std::string& name, Value v, int line, int knownSlot = -2,
                      TypeSet hint = TypeSet::Any(), bool settled = false) {
        CallFrame& f = frames_.back();
        if (Value* existing = f.findOwn(name, knownSlot)) {
            const TypeSet ty = settled ? TypeSet::Any() : f.typeOf(name, knownSlot);
            if (!ty.contains(v.tag()))
                vmError("cannot assign " + v.typeName() + " to '" + shownName(name) + "' declared as " + ty.name(), line);
            *existing = std::move(v);
            return;
        }
        // writes straight into the shared cell, so every other closure sharing it sees the change
        Cell* up = upvalCell(name, knownSlot);
        if (up && up->defined) { up->v = std::move(v); return; }
        if (f.captured)
            for (auto it = f.captured->layers.rbegin(); it != f.captured->layers.rend(); ++it) {
                auto found = (*it)->find(name);
                if (found != (*it)->end()) { found->second = std::move(v); return; }
            }
        // not found in this function or what it captured
        // if it's an existing Interpreter global update it there instead of shadowing it with a new local
        // (not from a function of an imported module: a temporary of library code must not write the importer's variable, it is a local.
        // the module's own names were found above, in its scope)
        if (interp_.has(name) && !(f.captured && f.captured->hasModule)) { interp_.set(name, std::move(v), hint); return; }
        // a name we've never seen. in the top-level frame it has to become a global, same as an assignment
        // at depth 0 (compiled to STORE_GLOBAL). otherwise a name first assigned inside an if/while at top
        // level becomes a frame-local that later splits from the global (this once made
        // `if 1 / i = 0 / end / i = 0 / while i < 3 / i += 1 / end` loop forever)
        if (frames_.size() == 1 && frames_[0].isPersistentTop) { interp_.set(name, std::move(v), hint); return; }
        // the variable belongs to an enclosing function that has not defined it yet: this assignment does
        if (up) { up->v = std::move(v); up->defined = true; return; }
        // inside a function, the innermost frame's local is the right place. with a hint, the stores of this variable carry
        // the type themselves. without one, the type of this first value is what the variable has from now on
        if (hint.isAny()) f.setType(name, knownSlot, TypeSet(v.tag()));
        f.defineOwn(interp_, name, std::move(v), knownSlot);
    }

    // ends the innermost block: erases the names it defined
    void closeScope(CallFrame& f) {
        const size_t from = f.scopeMarks.back();
        if (from < f.scopeFlat.size()) {
            for (size_t i = from; i < f.scopeFlat.size(); ++i) {
                const auto& sn = f.scopeFlat[i];
                if (sn.slot >= 0) {   // no name lookup
                    if ((size_t)sn.slot < f.slotDefined.size()) { f.slotDefined[(size_t)sn.slot] = false; f.slotTypes[(size_t)sn.slot] = TypeSet::Any(); }
                } else {
                    f.eraseOwn(*sn.name);
                    if (!f.localTypes.empty()) f.localTypes.erase(*sn.name);
                }
            }
            f.scopeFlat.resize(from);
        }
        f.scopeMarks.pop_back();
    }

    // record that name was DEFINE'd while at least one SCOPE_PUSH is active in this frame so the matching SCOPE_POP knows to erase it
    // a name defined at frame scope depth isn't tracked here and correctly survives to the end of the frame
    void trackScopedDefine(CallFrame& f, const std::string& name, int slot = -2) {
        if (!f.scopeMarks.empty()) f.scopeFlat.push_back({&name, slot});
    }

    // define (untyped) in the innermost frame. clears any type constraint set when the name was declared before
    void resolveDef(const std::string& name, Value v, int knownSlot = -2, bool autoType = false) {
        auto& f = frames_.back();
        if (autoType) f.setType(name, knownSlot, TypeSet(v.tag()));
        f.defineOwn(interp_, name, std::move(v), knownSlot);
        if (!autoType) f.setType(name, knownSlot, TypeSet::Any());
        trackScopedDefine(f, name, knownSlot);
    }

    void resolveDefTyped(const std::string& name, Value v, TypeSet ts, int line, int knownSlot = -2) {
        if (!ts.isAny() && !ts.contains(v.tag()))
            vmError("cannot assign " + v.typeName() + " to '" + shownName(name) + "' declared as " + ts.name(), line);
        auto& f = frames_.back();
        f.defineOwn(interp_, name, std::move(v), knownSlot);
        f.setType(name, knownSlot, ts);
        trackScopedDefine(f, name, knownSlot);
    }

    // the CaptureFrame for a function made right now by the running frame: the cells of the enclosing variables it uses (the
    // compiler listed them in chunk.upvalDescs), taken from the running frame's own cells or its own upvalues, and the module
    // scope if the function is written in an imported module. null for a function that needs neither. the callers further
    // down the stack are not part of it, which is what makes the scoping lexical
    std::shared_ptr<CaptureFrame> makeCaptured(const CompiledChunk& chunk) {
        CallFrame& f = frames_.back();
        const bool inModule  = interp_.insideImportedModule();
        const bool hasModule = inModule || (f.captured && f.captured->hasModule);
        if (chunk.upvalDescs.empty() && !hasModule) return nullptr;
        auto cap = std::make_shared<CaptureFrame>();
        cap->hasModule = hasModule;
        if (f.captured) cap->layers = f.captured->layers;
        if (inModule) {
            auto ms = interp_.currentModuleScope();
            if (cap->layers.empty() || cap->layers.back() != ms) cap->layers.push_back(ms);
        }
        if (!chunk.upvalDescs.empty()) {
            cap->upvals = std::make_shared<std::vector<std::shared_ptr<Cell>>>();
            cap->upvals->reserve(chunk.upvalDescs.size());
            for (auto& d : chunk.upvalDescs) {
                if (d.fromParentLocal) {
                    auto& c = (*f.cells)[(size_t)d.index];
                    if (!c) c = interp_.newCell();   // the variable isn't defined yet: make its cell now, the definition fills it
                    cap->upvals->push_back(c);
                } else {
                    cap->upvals->push_back((*f.captured->upvals)[(size_t)d.index]);
                }
            }
        }
        if (!cap->layers.empty())
            cap->gcRoots = std::make_shared<std::vector<std::shared_ptr<Scope>>>(cap->layers);
        return cap;
    }

    // one step used: called on a backward jump and on a script call. see Interpreter::Budget
    void budgetStep() {
        uint64_t used = ++interp_.stepsUsed;
        if (interp_.cancelFlag && interp_.cancelFlag->load(std::memory_order_relaxed)) budgetExceeded("cancelled");
        if (interp_.budget.maxSteps && used > interp_.budget.maxSteps)
            budgetExceeded("step limit of " + std::to_string(interp_.budget.maxSteps) + " steps reached");
        if (interp_.budget.seconds > 0 && (used & 1023) == 0 && std::chrono::steady_clock::now() > interp_.budget.deadline)
            budgetExceeded("time limit of " + formatNumber(interp_.budget.seconds) + " seconds reached");
    }
    [[noreturn, gnu::noinline]] void budgetExceeded(const std::string& what) {
        try { vmError("script stopped: " + what, lastLine_); }
        catch (EmbrError& e) { e.fatal = true; throw; }
    }

    // running out of memory (a string or array that doubled until it did not fit) is an error a script can
    // catch, not a crash. the value that failed to grow is already gone, so the handler gets the memory back
    void outOfMemory(int minDepth) {
        try { vmError("out of memory: a value grew too large", lastLine_); }
        catch (EmbrError& e) { if (!handleTry(e, minDepth)) throw; }
    }

    // dispatch loop

    Value dispatch() {
        while (state_ == State::Running && !frames_.empty()) {
            try {
                refreshHot();
                if (hot_) { do { fastRun(); if (hot_) execOne<true>(); } while (hot_); }
                else execOne<false>();
            } catch (EmbrError& e) {
                if (!handleTry(e, 0)) throw;
            } catch (const std::bad_alloc&) { outOfMemory(0);
            } catch (const std::length_error&) { outOfMemory(0); }
        }
        if (state_ == State::Done || frames_.empty()) {
            state_ = State::Done; refreshHot();
            return stack_.empty() ? Value(0.0) : pop();
        }
        return Value(0.0); // suspended
    }

    // find the nearest open try handler in frames_[floor..], innermost first. if there is one, unwind to it
    // and jump to its catch block. floor stops a nested dispatch loop from catching an error meant for the
    // caller's own try (the caller finds it when the exception propagates out). returns false if none is open
    bool handleTry(EmbrError& e, int floor) {
        if (e.fatal) { appendTrace(e, floor - 1); return false; }   // nothing catches it, the trace still says where
        // first work out which frame (if any) will catch this, so the trace records exactly the script
        // calls being unwound between the raise and the handler (or, with no handler in range, every
        // frame this loop owns), before the unwinding below pops them
        int handler = floor - 1;
        for (int fi = (int)frames_.size() - 1; fi >= floor; --fi)
            if (!frames_[fi].tryHandlers.empty()) { handler = fi; break; }
        appendTrace(e, handler);

        for (int fi = (int)frames_.size() - 1; fi >= floor; --fi) {
            if (frames_[fi].tryHandlers.empty()) continue;
            auto h = frames_[fi].tryHandlers.back();
            frames_[fi].tryHandlers.pop_back();

            // discard any frames pushed by calls that were still in progress when the error was raised
            while ((int)frames_.size() - 1 > fi) {
                dropTopFrame();
            }

            auto& f = frames_[fi];
            // unwind scopes the try block itself opened
            while (f.scopeDepth > h.scopeDepthAtEntry) {
                closeScope(f);
                f.scopeDepth--;
            }
            while (stack_.size() > h.stackBase) stack_.pop_back();
            // loops that the error cut short are over: without this their iterators stay on top and the
            // enclosing loop would read the wrong one
            if (f.iterStack.size() > h.iterDepth) f.iterStack.resize(h.iterDepth);

            push(errorToMap(e)); // consumed by the catch block's own DEFINE
            f.ip = h.catchIp;
            injBuf_.reset();
            state_ = State::Running; refreshHot();
            return true;
        }
        return false;
    }

    // appends one TraceFrame per script function frame above `handlerFrame`, innermost first. the call
    // site of frames_[k] is the CALL instruction the caller frames_[k-1] just executed (its ip has
    // already advanced past it, hence ip - 1). top-level script code is not a call, and a frame entered
    // from native code has no script call site (line 0).
    void appendTrace(EmbrError& e, int handlerFrame) {
        for (int k = (int)frames_.size() - 1; k > handlerFrame; --k) {
            const CallFrame& fr = frames_[k];
            if (fr.isPersistentTop) continue;
            TraceFrame t;
            t.fn = fr.chunk ? fr.chunk->name : std::string();
            if (k > 0) {
                const CallFrame& caller = frames_[k - 1];
                if (caller.chunk) {
                    t.file = caller.chunk->sourceFile;
                    int ip = caller.ip - 1;
                    if (!fr.fromNative && ip >= 0 && ip < (int)caller.chunk->code.size())
                        t.line = caller.chunk->lineAt(ip);
                }
            }
            e.trace.push_back(std::move(t));
        }
    }

    // get the current instruction, advancing ip
    const Instruction& fetch() {
        auto& f = frames_.back();
        if (injBuf_ && f.ip < (int)injBuf_->size()) {
            lastLine_ = injBuf_->lines[(size_t)f.ip];
            return (*injBuf_)[f.ip++];
        }
        // if exhausted the injection buffer, switch back to the chunk
        if (injBuf_ && f.ip >= (int)injBuf_->size()) {
            injBuf_.reset();
            f.ip = 0;
        }
        assert(f.ip < (int)f.chunk->code.size());
        lastLine_ = f.chunk->lineAt(f.ip);
        return f.chunk->code[f.ip++];
    }

    bool atEnd() const {
        if (frames_.empty()) return true;
        const auto& f = frames_.back();
        if (injBuf_) return f.ip >= (int)injBuf_->size();
        return f.ip >= (int)f.chunk->code.size();
    }

    const std::string& chunkStr(int idx) const {
        return topChunk_->str(idx);
    }

    // execOne() is inlined into the two loops that run whole programs (dispatch() and invokeChunk()), which lets the
    // compiler give each op its own jump back to the loop. step() and runLimited() are not hot, so they share this one copy
    [[gnu::noinline]] void execOneOutOfLine() { execOne<false>(); }

    // operands of the fused ops. false when the operand is not a number or not there (a local not defined yet, a global that is not in
    // the lookup cache yet), and the fused op then runs the ordinary instructions
    [[gnu::always_inline]] static bool fuseLocal(const CallFrame& f, int slot, FuseNum& n) {
        const uint32_t s = (uint32_t)slot;
        return s < f.slotDefined.size() && f.slotDefined[s] && fuseNumOf(f.slots[s], n);
    }
    [[gnu::always_inline]] bool fuseGlobal(int nameIdx, FuseNum& n) {
        CompiledChunk::GlobalCache& g = topChunk_->globalCacheFor(nameIdx);
        return g.epoch == interp_.scopeEpoch() && fuseNumOf(*g.ptr, n);
    }
    [[gnu::always_inline]] bool fuseConst(int flags, int idx, FuseNum& n) const {
        if (flags & kFuseFloatConst) { n.isInt = false; n.d = topChunk_->nums[(size_t)idx]; }
        else                         { n.isInt = true;  n.i = topChunk_->ints[(size_t)idx]; }
        return true;
    }

    struct LazyLine {
        const VmRunner* r; int ip; bool fromChunk;
        [[gnu::always_inline]] operator int() const { return fromChunk ? (r->topLines_ ? r->topLines_[ip] : 0) : r->lastLine_; }
    };


    // what fastRun needs to know about the running frame, see there
    struct FastCtx : CompiledChunk::Fast, CallFrame::Fast {
        Value* base; Value* limit;
        uint64_t epoch; bool budget;
    };

    // the CALL of a native that takes its arguments as a view (bindSpan): called on the values where they sit on the stack, no vector. the callee is
    // below the arguments, and is popped with them, so `c` is not used after that
    [[gnu::noinline]] void callSpanNative(const Value::Callable& c, int argc, int line) {
        const ArgSpan as{stack_.data() + (stack_.size() - (size_t)argc), (size_t)argc};
        checkNativeSig(c, as, line, false);   // a call with `&` never gets here
        Value result;
        try {
            result = c.span(as);
        } catch (EmbrError& e) {
            if (!e.hasLocation) {
                try { raiseError(e.keepContext ? e.context : c.name(), e.message, errRange(line), interp_.sourceMap()); }
                catch (EmbrError& located) { located.trace = std::move(e.trace); located.keepContext = e.keepContext; throw; }
            }
            throw;
        }
        if (!c.returns.isAny() && !c.returns.contains(result.tag()))
            vmError("internal error: '" + c.name() + "' returned " + result.typeName() + " but is declared to return " +
                    c.returns.name() + " (a bug in the plugin)", line);
        for (int i = 0; i <= argc; ++i) stack_.pop_back();   // the arguments, then the callee
        stack_.push_back(std::move(result));
    }

    // the hot loop: runs the common instructions on plain pointers (the instruction, the value stack, the local slots) that stay in
    // registers, so an instruction costs a few machine instructions and not a trip through the runner's fields. at the first
    // instruction it does not handle, or whose operands are not what it expects (not numbers, a cold global cache, a type to check),
    // it stops with that instruction not started, and execOne() runs it. so every case here must change nothing before it gives up
    // the work of the hot loop's rarer cases, kept out of fastRun's body so its common cases keep their registers. each returns
    // false, having changed nothing, when it can't do the job, and the loop then gives up as usual
    [[gnu::noinline]] static Cell* frUpval(const FastCtx& c, int sv) {
        if (!c.ups || (size_t)upvalIndex(sv) >= c.ups->size()) return nullptr;
        Cell* cl = (*c.ups)[(size_t)upvalIndex(sv)].get();
        return cl->defined ? cl : nullptr;
    }
    // `s + x` or `s = s + x` with s a string: grows `into` in place (strMut copies it first when it is shared). a result over 1 MB is left
    // to execOne, which turns running out of memory into a script error
    [[gnu::noinline]] static bool frAppend(Value& into, const Value& x) {
        std::string rs = x.tg == 2 ? std::string() : x.formatAsString();
        const std::string& rhs = x.tg == 2 ? x.asString() : rs;
        if (into.asString().size() + rhs.size() > (1u << 20)) return false;
        into.strMut() += rhs;
        return true;
    }
    // `m[k]` on a map that has the string key k itself (a missing one may come from __proto__, execOne looks)
    [[gnu::noinline]] static bool frMapGet(const Value& cont, int need, Value*& sp) {
        const Value::map_type& m = static_cast<MapBox*>(cont.u.p)->m;
        auto it = m.find(sp[-1].asString());
        if (it == m.end()) return false;
        Value r = it->second;
        for (int k = 0; k < need; ++k) { --sp; sp->~Value(); }
        new (sp) Value(std::move(r)); ++sp;
        return true;
    }
    [[gnu::noinline]] static void frMapSet(Value& cont, int need, Value*& sp) {
        cont.mapSet(sp[-2].asString(), std::move(sp[-1]));
        for (int k = 0; k < need; ++k) { --sp; sp->~Value(); }
    }
    // the first value of a local. the scoping rule (resolveStore) makes it this call's own local, typed by that value, unless the
    // function captured anything or a global of that name exists. those go the slow way
    [[gnu::noinline]] bool frDefineLocal(const CallFrame& f, FastCtx& c, const Instruction& I, Value*& sp) {
        const uint32_t s = (uint32_t)I.slot;
        if (f.captured || frames_.size() < 2 || (size_t)I.operand >= c.nstrs || interp_.has(c.strs[I.operand])) return false;
        if (I.typeHint.isAny()) c.types[s] = TypeSet(sp[-1].tag());
        c.slots[s] = std::move(sp[-1]); c.def[s] = 1; --sp;
        return true;
    }

    [[gnu::noinline]] void fastRun() {
        // the running frame and the value stack. read again (reload) after a call or a return changes the frame. everything but the instruction
        // pointer, the stack pointer and the frame stays in c, so the compiler has only those three to keep in registers
        FastCtx c;
        CallFrame* fp;
        const Instruction* ip;
        Value* sp;
        c.epoch = interp_.scopeEpoch();
        c.budget = interp_.budgetOn;
#define f (*fp)
    reload:
        c.base = stack_.data();
        c.limit = c.base + stack_.capacity();
        sp = c.base + stack_.size();
    reloadFrame:   // the same, for a call or return that cannot have moved the value stack, with sp already set
        fp = top_;
        static_cast<CompiledChunk::Fast&>(c) = topChunk_->fast;
        static_cast<CallFrame::Fast&>(c) = f.fast;
        ip = c.code + f.ip;

        // a number operand: the local, the global, the constant, or the stack
#define EMBR_FR_LOCAL(slotv, n) { const uint32_t s_ = (uint32_t)(slotv); if (s_ >= c.nslots || !c.def[s_] || !fuseNumOf(c.slots[s_], n)) goto bail; }
#define EMBR_FR_GLOBAL(idx, n) { const CompiledChunk::GlobalCache& g_ = c.gcs[(idx)]; if (g_.epoch != c.epoch || !fuseNumOf(*g_.ptr, n)) goto bail; }
#define EMBR_FR_CONST(flags, idx, n) { if ((flags) & kFuseFloatConst) { n.isInt = false; n.d = c.nums[(idx)]; } else { n.isInt = true; n.i = c.ints[(idx)]; } }
// the variable an in-place op works on (see varBySlot): a defined local of this frame, else a global through the lookup cache
#define EMBR_FR_VAR(nameIdx, slotv, out) { const int sv_ = (slotv); \
    if (sv_ >= 0 && (uint32_t)sv_ < c.nslots && c.def[sv_]) out = &c.slots[sv_]; \
    else if (sv_ == -1 || ((sv_ == kFreeSlot || sv_ >= 0) && !c.modLayer)) { const CompiledChunk::GlobalCache& g_ = c.gcs[(nameIdx)]; if (g_.epoch != c.epoch) goto bail; out = g_.ptr; } \
    else if (isUpvalSlot(sv_)) { Cell* cl_ = frUpval(c, sv_); if (!cl_) goto bail; out = &cl_->v; } \
    else goto bail; }
#define EMBR_FR_PUSH(val) { if (sp >= c.limit) goto bail; new (sp) Value(val); ++sp; }
        for (;;) {
            if (ip >= c.codeEnd) goto bail;
            const Instruction& I = *ip;
            switch (I.op) {
            case Op::NOP: ++ip; break;
            case Op::PUSH_INT: EMBR_FR_PUSH((int64_t)c.ints[I.operand]) ++ip; break;
            case Op::PUSH_NUM: EMBR_FR_PUSH((double)c.nums[I.operand]) ++ip; break;
            case Op::POP: if (sp == c.base) goto bail; --sp; sp->~Value(); ++ip; break;

            case Op::LOAD: {
                const uint32_t s = (uint32_t)I.slot;
                if (sp >= c.limit) goto bail;
                if (s < c.nslots && c.def[s]) {
                    if ((I.flags & kCallee) && c.slots[s].tg == 5) new (sp) Value(calleeMark(&c.slots[s]));
                    else new (sp) Value(c.slots[s]);
                    ++sp; ++ip;
                    break;
                }
                // a name no function around has, or a local that is not defined here: a global
                if ((I.slot == kFreeSlot || I.slot >= 0) && !c.modLayer) {
                    const CompiledChunk::GlobalCache& g = c.gcs[I.operand];
                    if (g.epoch != c.epoch) goto bail;
                    if ((I.flags & kCallee) && g.ptr->tg == 5) { new (sp) Value(calleeMark(g.ptr)); ++sp; ++ip; break; }
                    new (sp) Value(*g.ptr); ++sp; ++ip;
                    break;
                }
                // a variable this closure captured
                if (Cell* cl = isUpvalSlot(I.slot) ? frUpval(c, I.slot) : nullptr) {
                    if ((I.flags & kCallee) && cl->v.tg == 5) new (sp) Value(calleeMark(&cl->v)); else new (sp) Value(cl->v);
                    ++sp; ++ip;
                    break;
                }
                goto bail;
            }
            case Op::STORE: {
                const uint32_t s = (uint32_t)I.slot;
                const bool typedHere = !I.typeHint.isAny(), proven = (I.flags & kProven) != 0;
                if (sp == c.base) goto bail;
                if (typedHere && !proven && !I.typeHint.contains(sp[-1].tag())) goto bail;   // execOne raises the error
                // a variable this closure captured: write its cell, when no declared type of this frame's has to be checked
                if (Cell* cl = isUpvalSlot(I.slot) ? frUpval(c, I.slot) : nullptr) {
                    if (!(typedHere || proven) && !f.localTypes.empty()) goto bail;
                    cl->v = std::move(sp[-1]); --sp; ++ip;
                    break;
                }
                if (s >= c.nslots) goto bail;
                // the first value of a local. the scoping rule (resolveStore) makes it this call's own local, typed by that value, unless
                // the function captured anything or a global of that name exists. those go the slow way
                if (!c.def[s]) {
                    if (!frDefineLocal(f, c, I, sp)) goto bail;
                    ++ip;
                    break;
                }
                if (!(typedHere || proven) && !c.types[s].contains(sp[-1].tag())) goto bail;
                c.slots[s] = std::move(sp[-1]); --sp; ++ip;
                break;
            }
            case Op::LOAD_GLOBAL: {
                const CompiledChunk::GlobalCache& g = c.gcs[I.operand];
                if (g.epoch != c.epoch || sp >= c.limit) goto bail;
                if ((I.flags & kCallee) && g.ptr->tg == 5) { new (sp) Value(calleeMark(g.ptr)); ++sp; ++ip; break; }
                new (sp) Value(*g.ptr); ++sp; ++ip;
                break;
            }
            case Op::STORE_GLOBAL: {
                const CompiledChunk::GlobalCache& g = c.gcs[I.operand];
                if (g.epoch != c.epoch || sp == c.base || (g.typed && !g.type.contains(sp[-1].tag()))) goto bail;
                *g.ptr = std::move(sp[-1]); --sp; ++ip;
                break;
            }

            // arithmetic and comparison on two numbers on the stack. anything else (a string, a dunder, an overflow, a zero divisor) is execOne's
            case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD: {
                if (sp - c.base < 2) goto bail;
                // a string plus a string or a number: the left one grows in place when nothing else shares it (strMut copies it
                // otherwise). a result over 1 MB goes to execOne, which turns running out of memory into a script error
                if (I.op == Op::ADD && sp[-2].tg == 2 && sp[-1].tg <= 2) {
                    if (!frAppend(sp[-2], sp[-1])) goto bail;
                    --sp; sp->~Value(); ++ip;
                    break;
                }
                FuseNum a, b; Value out;
                if (!fuseNumOf(sp[-2], a) || !fuseNumOf(sp[-1], b) || !fuseArithNum(fuseArithKind(I.op), a, b, out)) goto bail;
                --sp; sp[-1] = std::move(out); ++ip;   // both operands were numbers, nothing to release
                break;
            }
            case Op::LT: case Op::GT: case Op::LTE: case Op::GTE: {
                if (sp - c.base < 2) goto bail;
                FuseNum a, b; bool yes;
                if (!fuseNumOf(sp[-2], a) || !fuseNumOf(sp[-1], b) || !fuseCmpNum(fuseCmpKind(I.op), a, b, yes)) goto bail;
                --sp; sp[-1] = Value(yes); ++ip;
                break;
            }

            case Op::JUMP:
                if (c.budget) goto bail;
                ip = c.code + I.operand;
                break;
            case Op::JUMP_IF_FALSE: {
                if (sp == c.base || sp[-1].tg >= 2) goto bail;
                const bool t = sp[-1].tg == 1 ? sp[-1].u.i != 0 : sp[-1].u.d != 0.0;
                --sp;
                ip = t ? ip + 1 : c.code + I.operand;
                break;
            }

            case Op::SCOPE_PUSH:
                if (f.scopeMarks.size() == f.scopeMarks.capacity()) goto bail;
                f.scopeDepth++;
                f.scopeMarks.push_back((uint32_t)f.scopeFlat.size());
                ++ip;
                break;
            case Op::SCOPE_POP: {
                if (f.scopeDepth <= 0 || f.scopeMarks.empty()) goto bail;
                const size_t from = f.scopeMarks.back();
                if (from < f.scopeFlat.size()) {
                    for (size_t i = from; i < f.scopeFlat.size(); ++i)
                        if (f.scopeFlat[i].slot < 0) goto bail;
                    for (size_t i = from; i < f.scopeFlat.size(); ++i) {
                        const uint32_t sl = (uint32_t)f.scopeFlat[i].slot;
                        if (sl < c.nslots) { c.def[sl] = 0; c.types[sl] = TypeSet::Any(); }
                    }
                    f.scopeFlat.resize(from);
                }
                f.scopeMarks.pop_back();
                f.scopeDepth--;
                ++ip;
                break;
            }

            // the fused ops (quickenChunk). the instructions they replaced follow them in the chunk, so the extra operands are at ip[1], ip[2], ip[3]
            case Op::F_ARITH_LI: case Op::F_ARITH_LI_ST: {
                // the usual case: an int or float local and a constant of the same kind
                if ((uint32_t)I.slot < c.nslots && c.def[I.slot]) {
                    const Value& av = c.slots[I.slot];
                    NumBits b; b.tg = (I.flags & kFuseFloatConst) ? 0 : 1;
                    if (b.tg) b.u.i = c.ints[ip[1].operand]; else b.u.d = c.nums[ip[1].operand];
                    NumBits r;
                    if (av.tg < 2 && fastArith(I.flags & 7, NumBits{av.tg, av.u}, b, r)) {
                        if (I.op == Op::F_ARITH_LI) {
                            if (sp >= c.limit) goto bail;
                            new (sp) Value(); sp->tg = r.tg; sp->u = r.u; ++sp; ip += 3;
                            break;
                        }
                        const uint32_t d = (uint32_t)I.operand;
                        if (d < c.nslots && c.def[d] && c.slots[d].tg < 2 && I.typeHint.contains(r.tg ? TypeTag::Int : TypeTag::Number)) {
                            Value& dv = c.slots[d]; dv.tg = r.tg; dv.u = r.u;
                            ip += 4;
                            break;
                        }
                    }
                }
                FuseNum a, b; Value out;
                EMBR_FR_LOCAL(I.slot, a) EMBR_FR_CONST(I.flags, ip[1].operand, b)
                if (!fuseArithNum(I.flags & 7, a, b, out)) goto bail;
                if (I.op == Op::F_ARITH_LI) { EMBR_FR_PUSH(std::move(out)) ip += 3; break; }
                const uint32_t d = (uint32_t)I.operand;
                if (d >= c.nslots || !c.def[d] || !I.typeHint.contains(out.tag())) goto bail;
                c.slots[d] = std::move(out);
                ip += 4;
                break;
            }
            case Op::F_ARITH_LL: case Op::F_ARITH_LL_ST: {
                FuseNum a, b; Value out;
                EMBR_FR_LOCAL(I.slot, a) EMBR_FR_LOCAL(ip[1].slot, b)
                if (!fuseArithNum(I.flags & 7, a, b, out)) goto bail;
                if (I.op == Op::F_ARITH_LL) { EMBR_FR_PUSH(std::move(out)) ip += 3; break; }
                const uint32_t d = (uint32_t)I.operand;
                if (d >= c.nslots || !c.def[d] || !I.typeHint.contains(out.tag())) goto bail;
                c.slots[d] = std::move(out);
                ip += 4;
                break;
            }
            case Op::F_CMP_LI_JIF: {
                if ((uint32_t)I.slot < c.nslots && c.def[I.slot] && !(I.flags & kFuseFloatConst)) {
                    const Value& av = c.slots[I.slot];
                    NumBits b; b.tg = 1; b.u.i = c.ints[ip[1].operand];
                    bool yes;
                    if (av.tg == 1 && fastCmp(I.flags & 7, NumBits{av.tg, av.u}, b, yes)) {
                        ip = yes ? ip + 4 : c.code + ip[3].operand;
                        break;
                    }
                }
                FuseNum a, b; bool yes;
                EMBR_FR_LOCAL(I.slot, a) EMBR_FR_CONST(I.flags, ip[1].operand, b)
                if (!fuseCmpNum(I.flags & 7, a, b, yes)) goto bail;
                ip = yes ? ip + 4 : c.code + ip[3].operand;
                break;
            }
            case Op::F_CMP_LL_JIF: {
                FuseNum a, b; bool yes;
                EMBR_FR_LOCAL(I.slot, a) EMBR_FR_LOCAL(ip[1].slot, b)
                if (!fuseCmpNum(I.flags & 7, a, b, yes)) goto bail;
                ip = yes ? ip + 4 : c.code + ip[3].operand;
                break;
            }
            case Op::F_CMP_JIF: {
                FuseNum a, b; bool yes;
                if (sp - c.base < 2 || !fuseNumOf(sp[-2], a) || !fuseNumOf(sp[-1], b) || !fuseCmpNum(I.flags & 7, a, b, yes)) goto bail;
                sp -= 2;
                ip = yes ? ip + 2 : c.code + ip[1].operand;
                break;
            }
            case Op::F_ARITH_ST: {
                FuseNum a, b; Value out;
                if (sp - c.base < 2 || !fuseNumOf(sp[-2], a) || !fuseNumOf(sp[-1], b) || !fuseArithNum(I.flags & 7, a, b, out)) goto bail;
                const uint32_t d = (uint32_t)I.operand;
                if (d >= c.nslots || !c.def[d] || !I.typeHint.contains(out.tag())) goto bail;
                sp -= 2;
                c.slots[d] = std::move(out);
                ip += 2;
                break;
            }
            case Op::F_CMP_GI_JIF: case Op::F_CMP_GG_JIF: {
                FuseNum a, b; bool yes;
                EMBR_FR_GLOBAL(I.operand, a)
                if (I.op == Op::F_CMP_GG_JIF) EMBR_FR_GLOBAL(ip[1].operand, b) else EMBR_FR_CONST(I.flags, ip[1].operand, b)
                if (!fuseCmpNum(I.flags & 7, a, b, yes)) goto bail;
                ip = yes ? ip + 4 : c.code + ip[3].operand;
                break;
            }
            case Op::F_ARITH_GI: case Op::F_ARITH_GG: case Op::F_ARITH_GI_STG: {
                FuseNum a, b; Value out;
                EMBR_FR_GLOBAL(I.operand, a)
                if (I.op == Op::F_ARITH_GG) EMBR_FR_GLOBAL(ip[1].operand, b) else EMBR_FR_CONST(I.flags, ip[1].operand, b)
                if (!fuseArithNum(I.flags & 7, a, b, out)) goto bail;
                if (I.op != Op::F_ARITH_GI_STG) { EMBR_FR_PUSH(std::move(out)) ip += 3; break; }
                const CompiledChunk::GlobalCache& gd = c.gcs[ip[3].operand];
                if (gd.epoch != c.epoch || (gd.typed && !gd.type.contains(out.tag()))) goto bail;
                *gd.ptr = std::move(out);
                ip += 4;
                break;
            }
            // the head of a for loop over an array or a range, see F_ITER_DEFINE in execOne
            case Op::F_ITER_DEFINE: {
                if (f.iterStack.empty()) goto bail;
                IterState& st = f.iterStack.back();
                const uint32_t sl = (uint32_t)I.slot;
                if (st.mode == IterState::Mode::Items || st.varCount != 1 || sl >= c.nslots) goto bail;
                if (st.idx >= st.count) { ip = c.code + I.operand; break; }
                if (f.scopeMarks.size() == f.scopeMarks.capacity() || f.scopeFlat.size() == f.scopeFlat.capacity() || (uint32_t)ip[2].operand >= c.nstrs) goto bail;
                if (st.mode == IterState::Mode::Array && (st.src.tg != 3 || static_cast<ArrBox*>(st.src.u.p)->state.load(std::memory_order_acquire) != 0)) goto bail;
                Value next = st.mode == IterState::Mode::Range
                    ? Value((int64_t)((uint64_t)st.rStart + (uint64_t)st.idx * (uint64_t)st.rStep))
                    : Value(static_cast<ArrBox*>(st.src.u.p)->v[st.idx]);
                ++st.idx;
                f.scopeDepth++;
                f.scopeMarks.push_back((uint32_t)f.scopeFlat.size());
                c.slots[sl] = std::move(next);
                c.def[sl] = 1;
                c.types[sl] = TypeSet::Any();
                f.scopeFlat.push_back({c.strs + ip[2].operand, I.slot});
                ip += 3;
                break;
            }
            case Op::F_ARITH_STG: {
                FuseNum a, b; Value out;
                if (sp - c.base < 2 || !fuseNumOf(sp[-2], a) || !fuseNumOf(sp[-1], b) || !fuseArithNum(I.flags & 7, a, b, out)) goto bail;
                const CompiledChunk::GlobalCache& gd = c.gcs[ip[1].operand];
                if (gd.epoch != c.epoch || (gd.typed && !gd.type.contains(out.tag()))) goto bail;
                sp -= 2;
                *gd.ptr = std::move(out);
                ip += 2;
                break;
            }

            case Op::PUSH_NIL: EMBR_FR_PUSH(0.0) ++ip; break;
            case Op::PUSH_STR: {
                const CompiledChunk& ch = *topChunk_;
                if ((uint32_t)I.operand >= c.nstrs || ch.strLitSet.size() != c.nstrs || !ch.strLitSet[(size_t)I.operand] || sp >= c.limit) goto bail;
                new (sp) Value(ch.strLits[(size_t)I.operand]); ++sp; ++ip;
                break;
            }
            // bit operators on two ints. a float, a bad shift count or a dunder is execOne's
            case Op::BAND: case Op::BOR: case Op::BXOR: case Op::SHL: case Op::SHR: {
                if (sp - c.base < 2 || sp[-2].tg != 1 || sp[-1].tg != 1) goto bail;
                const char code = I.op == Op::BAND ? '&' : I.op == Op::BOR ? '|' : I.op == Op::BXOR ? '^' : I.op == Op::SHL ? 'l' : 'r';
                int64_t out;
                if (!bitOpInts(code, sp[-2].u.i, sp[-1].u.i, out)) goto bail;
                --sp; sp[-1].u.i = out; ++ip;
                break;
            }
            case Op::BNOT: {
                if (sp == c.base || sp[-1].tg != 1) goto bail;
                sp[-1].u.i = ~sp[-1].u.i; ++ip;
                break;
            }
            case Op::NOT: {
                if (sp == c.base || sp[-1].tg >= 2) goto bail;
                const bool t = sp[-1].tg == 1 ? sp[-1].u.i != 0 : sp[-1].u.d != 0.0;
                sp[-1] = Value(!t); ++ip;
                break;
            }
            case Op::NEG: {
                if (sp == c.base || sp[-1].tg >= 2) goto bail;
                if (sp[-1].tg == 1) { if (sp[-1].u.i == INT64_MIN) goto bail; sp[-1].u.i = -sp[-1].u.i; }
                else sp[-1].u.d = -sp[-1].u.d;
                ++ip;
                break;
            }
            // == and != of two c.ints or two floats. an int against a float is the ordinary instruction's
            case Op::EQ: case Op::NEQ: {
                if (sp - c.base < 2 || sp[-2].tg != sp[-1].tg || sp[-1].tg >= 2) goto bail;
                const bool same = sp[-1].tg == 1 ? sp[-2].u.i == sp[-1].u.i : sp[-2].u.d == sp[-1].u.d;
                --sp; sp[-1] = Value(I.op == Op::EQ ? same : !same); ++ip;
                break;
            }
            case Op::JUMP_IF_FALSE_PEEK: case Op::JUMP_IF_TRUE_PEEK: {
                if (sp == c.base || sp[-1].tg >= 2) goto bail;
                const bool t = sp[-1].tg == 1 ? sp[-1].u.i != 0 : sp[-1].u.d != 0.0;
                ip = t == (I.op == Op::JUMP_IF_TRUE_PEEK) ? c.code + I.operand : ip + 1;
                break;
            }

            case Op::DEFINE: {
                const uint32_t sl = (uint32_t)I.slot;
                const bool scoped = !f.scopeMarks.empty();
                if (sl >= c.nslots || sp == c.base || (uint32_t)I.operand >= c.nstrs || (scoped && f.scopeFlat.size() == f.scopeFlat.capacity())) goto bail;
                c.types[sl] = (I.flags & kAuto) ? TypeSet(sp[-1].tag()) : TypeSet::Any();
                c.slots[sl] = std::move(sp[-1]); --sp;
                c.def[sl] = 1;
                if (scoped) f.scopeFlat.push_back({c.strs + I.operand, I.slot});
                ++ip;
                break;
            }

            // reading an element of an array (the quickened INDEX_GET_VAR, see F_IDX in quickenChunk). the array has to be built, the index an int inside it
            case Op::F_IDX_L: case Op::F_IDX_G: case Op::F_IDX: {
                const bool onStack = I.op == Op::F_IDX;
                const Instruction& iv = onStack ? I : ip[1];
                FuseNum key;
                if (onStack) { if (sp == c.base || !fuseNumOf(sp[-1], key)) goto bail; }
                else if (I.op == Op::F_IDX_L) EMBR_FR_LOCAL(I.slot, key) else EMBR_FR_GLOBAL(I.operand, key)
                if (!key.isInt) goto bail;
                Value* cont;
                EMBR_FR_VAR(iv.operand, iv.slot, cont)
                if (cont->tg != 3) goto bail;
                ArrBox* ab = static_cast<ArrBox*>(cont->u.p);
                if (ab->state.load(std::memory_order_acquire) != 0 || (uint64_t)key.i >= (uint64_t)ab->v.size()) goto bail;
                if (onStack) { Value el(ab->v[(size_t)key.i]); sp[-1] = std::move(el); ++ip; }
                else { EMBR_FR_PUSH(ab->v[(size_t)key.i]) ip += 2; }
                break;
            }
            // `name[key] = value` on an array variable nobody else holds, replacing a number with a number. anything else (a string, a map, a shared array that has to be copied first) is execOne's
            // `m[k] = v` on a map with a string key and a number or string value (no nesting to check); mapSet copies a shared map first
            // `m[k]` on a map with a string key the map has itself (a missing one may come from its __proto__, execOne looks)
            case Op::INDEX_GET_VAR: {
                const int need = (I.flags & kStash) ? 2 : 1;
                if (sp - c.base < need || sp[-1].tg != 2 || sp >= c.limit) goto bail;
                if ((I.flags & kStash) && !isStashMark(sp[-2])) goto bail;
                Value* cont;
                EMBR_FR_VAR(I.operand, I.slot, cont)
                if (cont->tg != 4 || !frMapGet(*cont, need, sp)) goto bail;
                ++ip;
                break;
            }
            case Op::INDEX_SET_VAR: {
                const int need = (I.flags & kStash) ? 3 : 2;
                if (sp - c.base < need) goto bail;
                if ((I.flags & kStash) && !isStashMark(sp[-3])) goto bail;
                if (sp[-2].tg == 2 && sp[-1].tg <= 2) {
                    Value* mcont;
                    EMBR_FR_VAR(I.operand, I.slot, mcont)
                    if (mcont->tg != 4) goto bail;
                    frMapSet(*mcont, need, sp);
                    ip = c.code + I.arg;
                    break;
                }
                if (sp[-2].tg != 1 || sp[-1].tg >= 2) goto bail;
                Value* cont;
                EMBR_FR_VAR(I.operand, I.slot, cont)
                if (cont->tg != 3) goto bail;
                ArrBox* ab = static_cast<ArrBox*>(cont->u.p);
                if (ab->state.load(std::memory_order_acquire) != 0 || ab->rc.load(std::memory_order_acquire) != 1 ||
                    (uint64_t)sp[-2].u.i >= (uint64_t)ab->v.size()) goto bail;
                Value& el = ab->v[(size_t)sp[-2].u.i];
                if (el.tg >= 2) goto bail;
                el.u = sp[-1].u; el.tg = sp[-1].tg;
                sp -= need;
                ip = c.code + I.arg;
                break;
            }

            // a script function called with the exact number of arguments (CompiledChunk::callKind 1), or a native that takes a view of its arguments.
            // the frame is made by the same c.code execOne uses, and then this loop carries on in the new frame
            case Op::CALL: {
                const int argc = I.operand;
                if (argc < 0 || (I.flags & kHasRef) || sp - c.base <= argc) goto bail;
                const int ipAt = (int)(ip - c.code);
                // a mark stands for a function that lives somewhere else; this puts the real one where the mark was before giving the call up
#define EMBR_FR_CALL_GIVE_UP { if (isCalleeMark(sp[-argc - 1])) sp[-argc - 1] = calleeValue(sp[-argc - 1]); goto bail; }
                const Value& cv = calleeValue(sp[-argc - 1]);
                if (cv.tg != 5) EMBR_FR_CALL_GIVE_UP
                const Value::Callable& fc = cv.asCallable();
                if (!fc.isNative()) {
                    if (!fc.script.compiledChunk) EMBR_FR_CALL_GIVE_UP
                    auto* fchunk = static_cast<CompiledChunk*>(fc.script.compiledChunk.get());
                    if (fchunk->callKind == 0) [[unlikely]] fchunk->classifyCall();
                    if (fchunk->callKind != 1 || (size_t)argc != fchunk->params.size() || !fchunk->argsFit(sp - argc)) EMBR_FR_CALL_GIVE_UP
                    f.ip = ipAt + 1;
                    stack_.setSize((size_t)(sp - c.base));
                    pushFrameSimpleOut(fchunk, fc.script.captured, (size_t)argc, fc.script.returnType);
                    if (!hot_) return;
                    sp -= argc + 1;   // the callee and its arguments are gone
                    goto reloadFrame;
                } else if (fc.typed.thunk && typedFits(fc, sp - argc, argc)) {
                    f.ip = ipAt + 1;
                    stack_.setSize((size_t)(sp - c.base));
                    Value r = callTypedNative(fc, sp - argc, topLines_ ? topLines_[ipAt] : 0);
                    for (int i = 0; i < argc; ++i) { --sp; sp->~Value(); }
                    sp[-1] = std::move(r);
                    ++ip;
                    break;
                } else if (fc.span) {
                    f.ip = ipAt + 1;
                    stack_.setSize((size_t)(sp - c.base));
                    callSpanNative(fc, argc, topLines_ ? topLines_[ipAt] : 0);
                } else EMBR_FR_CALL_GIVE_UP
#undef EMBR_FR_CALL_GIVE_UP
                if (!hot_) return;
                goto reload;
            }
            // the usual return: from a function that declared no return type, to a caller that is another frame
            case Op::RETURN: {
                if (sp == c.base || frames_.size() < 2) goto bail;
                if (!f.returnType.isAny() && !f.returnType.contains(sp[-1].tag())) [[unlikely]] goto bail;   // a wrong type: execOne raises
                const int leavingBase = f.stackBase;
                if (sp - c.base == leavingBase + 1) [[likely]] {
                    // only the result is left above the frame, so it is already where the caller wants it
                    stack_.setSize((size_t)(sp - c.base));
                    frames_.pop_back(); syncTop();
                    interp_.exitCall();
                    if (!hot_) return;
                    goto reloadFrame;
                }
                Value result = std::move(sp[-1]);
                --sp;
                stack_.setSize((size_t)(sp - c.base));
                frames_.pop_back(); syncTop();
                interp_.exitCall();
                while ((int)stack_.size() > leavingBase) stack_.pop_back();
                stack_.push_back(std::move(result));
                if (!hot_) return;
                goto reload;
            }

            // `x = x + simple` on a variable that holds a string appends in place (execOne does that). on anything else this changes nothing
            // `s = s + x` on a string variable: grows it in place and skips the ADD and the STORE. on anything else this changes nothing.
            // a result over 1 MB goes to execOne, which turns running out of memory into a script error
            case Op::APPEND_VAR: {
                Value* v;
                EMBR_FR_VAR(I.operand, I.slot, v)
                if (v->tg != 2) { ++ip; break; }
                if (sp == c.base || sp[-1].tg > 2 || !frAppend(*v, sp[-1])) goto bail;
                --sp; sp->~Value();
                ip = c.code + I.arg;
                break;
            }
            // what INDEX_SET_VAR / APPEND_ARR need of the variable: a mark when nothing between can change it, else a copy
            case Op::STASH_VAR: {
                Value* v;
                EMBR_FR_VAR(I.operand, I.slot, v)
                if (sp >= c.limit) goto bail;
                if ((I.flags & kCallFree) || (I.slot >= 0 && (uint32_t)I.slot < c.nslots && c.def[I.slot]) ||
                    ((I.flags & kPureCalls) && pureCalleesNative(I))) { new (sp) Value(); sp->nestDepth = kStashMark; }
                else if (I.flags & kPureCalls) goto bail;
                else new (sp) Value(*v);
                ++sp; ++ip;
                break;
            }
            // `name = push(name, x)` on an array variable: appends in place and skips the CALL and the STORE
            case Op::APPEND_ARR: {
                if (sp - c.base < 3 || !isStashMark(sp[-2]) || calleeValue(sp[-3]).tg != 5) goto bail;
                const Value::Callable& cf = calleeValue(sp[-3]).asCallable();
                if (!cf.isNative() || cf.name() != "push") goto bail;
                Value* v;
                EMBR_FR_VAR(I.operand, I.slot, v)
                if (v->tg != 3) goto bail;
                Value x = std::move(sp[-1]);
                sp -= 3;   // the mark, the callee and the moved-from element
                sp->~Value(); sp[1].~Value(); sp[2].~Value();
                f.ip = (int)(ip - c.code) + 1;
                stack_.setSize((size_t)(sp - c.base));
                v->arrayAppend(std::move(x));
                ip = c.code + I.arg;
                break;
            }
            default: goto bail;
            }
        }
    bail:
        if (bailStatsOn() && ip < c.codeEnd) [[unlikely]] bailCounts()[(uint8_t)ip->op].fetch_add(1, std::memory_order_relaxed);
        f.ip = (int)(ip - c.code);
        stack_.setSize((size_t)(sp - c.base));
#undef EMBR_FR_LOCAL
#undef EMBR_FR_GLOBAL
#undef EMBR_FR_CONST
#undef EMBR_FR_PUSH
#undef EMBR_FR_VAR
#undef f
    }

    // Fast is true when the loop has checked hot_ already: the next instruction is in the chunk, no injected code
    template <bool Fast>
    [[gnu::always_inline]] inline void execOne() {
        // the common case first: no injected code, so the next instruction comes from the cached chunk code, and is read in
        // place (no copy). injected code is copied, because the injection buffer can be reallocated while the instruction runs
        alignas(Instruction) unsigned char injRaw[sizeof(Instruction)];   // not initialised: only the injected-code path uses it
        const Instruction* ipIn;
        int  ipAt = 0;
        bool fromChunk = false;
        if (Fast || (!injBuf_ && topCode_)) {
            CallFrame& cf = (*top_);
            if ((size_t)cf.ip >= topLen_) { doReturn(Value(0.0)); return; }
            ipAt = cf.ip;
            ipIn = &topCode_[cf.ip++];
            fromChunk = true;
        } else {
            if (atEnd()) {
                // implicit return from a frame that ran off the end
                doReturn(Value(0.0));
                return;
            }
            ipIn = new (injRaw) Instruction(fetch());
        }
        const Instruction* cur = ipIn;
        const LazyLine line{this, ipAt, fromChunk};
    redispatch:
        const Instruction& instr = *cur;
        // an operator that a typed pointer's dunder takes over (`__<tag>_add`, `_index`, ...) is a call of that global, made the way a CALL is: the
        // function and its arguments go on the value stack and the CALL runs, so a script function gets a frame of its own and no runner
        // is nested. these are for a case that has already popped its operands
#define EMBR_CALL_ON_STACK(argc) { Instruction ci_; ci_.op = Op::CALL; ci_.operand = (argc); cur = new (injRaw) Instruction(ci_); goto redispatch; }
#define EMBR_CALL_DUNDER1(fn, a) { stack_.push_back(fn); stack_.push_back(a); EMBR_CALL_ON_STACK(1) }
#define EMBR_CALL_DUNDER2(fn, a, b) { stack_.push_back(fn); stack_.push_back(a); stack_.push_back(b); EMBR_CALL_ON_STACK(2) }
#define EMBR_CALL_DUNDER3(fn, a, b, c) { stack_.push_back(fn); stack_.push_back(a); stack_.push_back(b); stack_.push_back(c); EMBR_CALL_ON_STACK(3) }
        // a fused op that can't do its job runs the instruction it replaced instead
#define EMBR_FUSED_FALLBACK { cur = new (injRaw) Instruction(topChunk_->quickOrig[(size_t)instr.arg]); goto redispatch; }

        switch (instr.op) {

        // fused ops (see quickenChunk). `ipAt` is this instruction's index in the chunk, and frame.ip is already ipAt + 1. each reads its
        // operands, and when they are numbers it can handle does the whole job, otherwise it runs the instruction it replaced
        case Op::F_ARITH_LI: case Op::F_ARITH_LI_ST: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            CallFrame& f = (*top_);
            FuseNum a, b; Value out;
            if (!fuseLocal(f, instr.slot, a) || !fuseConst(instr.flags, topCode_[ipAt + 1].operand, b) ||
                !fuseArithNum(instr.flags & 7, a, b, out)) EMBR_FUSED_FALLBACK
            if (instr.op == Op::F_ARITH_LI) { stack_.push_back(std::move(out)); f.ip += 2; break; }
            const uint32_t d = (uint32_t)instr.operand;
            if (d >= f.slotDefined.size() || !f.slotDefined[d] || !instr.typeHint.contains(out.tag())) EMBR_FUSED_FALLBACK
            f.slots[d] = std::move(out);
            f.ip += 3;
            break;
        }
        case Op::F_ARITH_LL: case Op::F_ARITH_LL_ST: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            CallFrame& f = (*top_);
            FuseNum a, b; Value out;
            if (!fuseLocal(f, instr.slot, a) || !fuseLocal(f, topCode_[ipAt + 1].slot, b) ||
                !fuseArithNum(instr.flags & 7, a, b, out)) EMBR_FUSED_FALLBACK
            if (instr.op == Op::F_ARITH_LL) { stack_.push_back(std::move(out)); f.ip += 2; break; }
            const uint32_t d = (uint32_t)instr.operand;
            if (d >= f.slotDefined.size() || !f.slotDefined[d] || !instr.typeHint.contains(out.tag())) EMBR_FUSED_FALLBACK
            f.slots[d] = std::move(out);
            f.ip += 3;
            break;
        }
        case Op::F_CMP_LI_JIF: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            CallFrame& f = (*top_);
            FuseNum a, b; bool yes;
            if (!fuseLocal(f, instr.slot, a) || !fuseConst(instr.flags, topCode_[ipAt + 1].operand, b) ||
                !fuseCmpNum(instr.flags & 7, a, b, yes)) EMBR_FUSED_FALLBACK
            f.ip = yes ? ipAt + 4 : topCode_[ipAt + 3].operand;
            break;
        }
        case Op::F_CMP_LL_JIF: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            CallFrame& f = (*top_);
            FuseNum a, b; bool yes;
            if (!fuseLocal(f, instr.slot, a) || !fuseLocal(f, topCode_[ipAt + 1].slot, b) ||
                !fuseCmpNum(instr.flags & 7, a, b, yes)) EMBR_FUSED_FALLBACK
            f.ip = yes ? ipAt + 4 : topCode_[ipAt + 3].operand;
            break;
        }
        case Op::F_CMP_JIF: {
            FuseNum a, b; bool yes;
            if (!fromChunk || stack_.size() < 2 || !fuseNumOf(stack_[stack_.size() - 2], a) || !fuseNumOf(stack_.back(), b) ||
                !fuseCmpNum(instr.flags & 7, a, b, yes)) EMBR_FUSED_FALLBACK
            stack_.pop_back(); stack_.pop_back();
            (*top_).ip = yes ? ipAt + 2 : topCode_[ipAt + 1].operand;
            break;
        }
        case Op::F_ARITH_ST: {
            FuseNum a, b; Value out;
            if (!fromChunk || stack_.size() < 2 || !fuseNumOf(stack_[stack_.size() - 2], a) || !fuseNumOf(stack_.back(), b) ||
                !fuseArithNum(instr.flags & 7, a, b, out)) EMBR_FUSED_FALLBACK
            CallFrame& f = (*top_);
            const uint32_t d = (uint32_t)instr.operand;
            if (d >= f.slotDefined.size() || !f.slotDefined[d] || !instr.typeHint.contains(out.tag())) EMBR_FUSED_FALLBACK
            stack_.pop_back(); stack_.pop_back();
            f.slots[d] = std::move(out);
            f.ip += 1;
            break;
        }

        // fused ops on globals. a global is read through the chunk's lookup cache, which must be warm
        case Op::F_CMP_GI_JIF: case Op::F_CMP_GG_JIF: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            FuseNum a, b; bool yes;
            if (!fuseGlobal(instr.operand, a)) EMBR_FUSED_FALLBACK
            if (instr.op == Op::F_CMP_GG_JIF ? !fuseGlobal(topCode_[ipAt + 1].operand, b) : !fuseConst(instr.flags, topCode_[ipAt + 1].operand, b)) EMBR_FUSED_FALLBACK
            if (!fuseCmpNum(instr.flags & 7, a, b, yes)) EMBR_FUSED_FALLBACK
            (*top_).ip = yes ? ipAt + 4 : topCode_[ipAt + 3].operand;
            break;
        }
        case Op::F_ARITH_GI: case Op::F_ARITH_GG: case Op::F_ARITH_GI_STG: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            FuseNum a, b; Value out;
            if (!fuseGlobal(instr.operand, a)) EMBR_FUSED_FALLBACK
            if (instr.op == Op::F_ARITH_GG ? !fuseGlobal(topCode_[ipAt + 1].operand, b) : !fuseConst(instr.flags, topCode_[ipAt + 1].operand, b)) EMBR_FUSED_FALLBACK
            if (!fuseArithNum(instr.flags & 7, a, b, out)) EMBR_FUSED_FALLBACK
            CallFrame& f = (*top_);
            if (instr.op != Op::F_ARITH_GI_STG) { stack_.push_back(std::move(out)); f.ip += 2; break; }
            CompiledChunk::GlobalCache& gd = topChunk_->globalCacheFor(topCode_[ipAt + 3].operand);
            if (gd.epoch != interp_.scopeEpoch() || (gd.typed && !gd.type.contains(out.tag()))) EMBR_FUSED_FALLBACK
            *gd.ptr = std::move(out);
            f.ip += 3;
            break;
        }
        case Op::F_ARITH_STG: {
            FuseNum a, b; Value out;
            if (!fromChunk || stack_.size() < 2 || !fuseNumOf(stack_[stack_.size() - 2], a) || !fuseNumOf(stack_.back(), b) ||
                !fuseArithNum(instr.flags & 7, a, b, out)) EMBR_FUSED_FALLBACK
            CompiledChunk::GlobalCache& gd = topChunk_->globalCacheFor(topCode_[ipAt + 1].operand);
            if (gd.epoch != interp_.scopeEpoch() || (gd.typed && !gd.type.contains(out.tag()))) EMBR_FUSED_FALLBACK
            stack_.pop_back(); stack_.pop_back();
            *gd.ptr = std::move(out);
            (*top_).ip += 1;
            break;
        }

        // the head of a for loop: ITER_NEXT and the DEFINE of the loop variable. a loop over a built list (a map's keys, a string) or over two
        // variables takes the ordinary path
        case Op::F_ITER_DEFINE: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            CallFrame& f = *top_;
            if (f.iterStack.empty()) EMBR_FUSED_FALLBACK
            auto& st = f.iterStack.back();
            const uint32_t sl = (uint32_t)instr.slot;
            if (st.mode == IterState::Mode::Items || st.varCount != 1 || sl >= f.slotDefined.size()) EMBR_FUSED_FALLBACK
            if (st.idx >= st.count) { f.ip = instr.operand; break; }
            Value next = st.mode == IterState::Mode::Range
                ? Value((int64_t)((uint64_t)st.rStart + (uint64_t)st.idx * (uint64_t)st.rStep))
                : st.src.arrayGet(st.idx);
            ++st.idx;
            // the SCOPE_PUSH of the loop body, then the DEFINE of the variable inside it
            f.scopeDepth++;
            f.scopeMarks.push_back((uint32_t)f.scopeFlat.size());
            const std::string& nm = chunkStr(topCode_[ipAt + 2].operand);
            f.slots[sl] = std::move(next);
            f.slotDefined[sl] = 1;
            f.slotTypes[sl] = TypeSet::Any();
            f.scopeFlat.push_back({&nm, instr.slot});
            f.ip += 2;   // past the SCOPE_PUSH and the DEFINE
            break;
        }

        // an array element read through INDEX_GET_VAR. the array has to be an array and the index an int inside it
        case Op::F_IDX_L: case Op::F_IDX_G: case Op::F_IDX: {
            if (!fromChunk) EMBR_FUSED_FALLBACK
            const bool keyOnStack = instr.op == Op::F_IDX;
            const Instruction& iv = keyOnStack ? instr : topCode_[ipAt + 1];
            FuseNum key;
            if (keyOnStack) { if (stack_.empty() || !fuseNumOf(stack_.back(), key)) EMBR_FUSED_FALLBACK }
            else if (instr.op == Op::F_IDX_L ? !fuseLocal((*top_), instr.slot, key) : !fuseGlobal(instr.operand, key)) EMBR_FUSED_FALLBACK
            if (!key.isInt) EMBR_FUSED_FALLBACK
            const Value* cont = varBySlot(iv.operand, iv.slot);
            if (!cont || !cont->isArray()) EMBR_FUSED_FALLBACK
            if ((uint64_t)key.i >= (uint64_t)cont->arraySize()) EMBR_FUSED_FALLBACK   // arraySize / arrayGet don't build a range
            if (keyOnStack) stack_.back() = cont->arrayGet((size_t)key.i);
            else { stack_.push_back(cont->arrayGet((size_t)key.i)); (*top_).ip += 1; }
            break;
        }

        // literals
        case Op::PUSH_NUM:   push(Value(topChunk_->nums[(size_t)instr.operand])); break;
        case Op::PUSH_INT:   push(Value(topChunk_->ints[(size_t)instr.operand])); break;
        case Op::PUSH_STR:
            if ((uint32_t)instr.operand < topChunk_->strings.size()) [[likely]] stack_.push_back(topChunk_->strValue(instr.operand));
            else push(Value(chunkStr(instr.operand)));   // a damaged operand: the same empty string it always was
            break;
        case Op::PUSH_NIL:   push(Value(0.0)); break;

        case Op::POP: pop(); break;
        case Op::NOP: break;

        // variables
        case Op::LOAD: {
            // the common case: a variable of the running function that already has a slot. same answer as
            // resolveLoad(), which would find it first too, minus the frame walk and the extra Value moves
            CallFrame& f = (*top_);
            if (instr.slot >= 0 && instr.slot < (int)f.slotDefined.size() && f.slotDefined[instr.slot]) {
                if ((instr.flags & kCallee) && f.slots[instr.slot].tg == 5) stack_.push_back(calleeMark(&f.slots[instr.slot]));
                else stack_.push_back(f.slots[instr.slot]);
                break;
            }
            // a name no function around this one can have: a global (a module layer, if one was captured, can still hold it)
            // a name no function around can have, or a local of this function that is not defined (yet), is a global unless a module layer
            // was captured. `x[i] = v` in a function counts as a binding of x, so a global array used that way lands here every time
            if ((instr.slot == kFreeSlot || instr.slot >= 0) && !(f.captured && f.captured->hasModule)) {
                if (instr.flags & kCallee) {
                    Value* gp = globalPtr(instr.operand, line);
                    if (gp->tg == 5) { stack_.push_back(calleeMark(gp)); break; }
                }
                pushGlobal(instr.operand, line);
                break;
            }
            // a local that a closure captures (its cell is this frame's), or a variable this function captured
            if (instr.slot < 0) {
                if (isBoxedSlot(instr.slot)) {
                    if (Value* v = f.findOwn(chunkStr(instr.operand), instr.slot)) {
                        if ((instr.flags & kCallee) && v->tg == 5) stack_.push_back(calleeMark(v)); else stack_.push_back(*v);
                        break;
                    }
                } else if (isUpvalSlot(instr.slot) && f.captured && f.captured->upvals) {
                    auto& ups = *f.captured->upvals;
                    size_t u = (size_t)upvalIndex(instr.slot);
                    if (u < ups.size() && ups[u]->defined) {
                        if ((instr.flags & kCallee) && ups[u]->v.tg == 5) stack_.push_back(calleeMark(&ups[u]->v)); else stack_.push_back(ups[u]->v);
                        break;
                    }
                }
            }
            if (onlyGlobalsLeft(f)) { pushGlobal(instr.operand, line); break; }
            push(resolveLoad(chunkStr(instr.operand), line, instr.slot));
            break;
        }

        case Op::STORE: {
            // same shortcut as LOAD. a type the variable has (localTypes) needs the checked path in resolveStore(),
            // unless the compiler already settled it: it gave this store the variable's type (test it here) or proved the value fits
            CallFrame& f = (*top_);
            const bool proven = (instr.flags & kProven) != 0;
            const bool typedHere = !instr.typeHint.isAny();
            if (typedHere && !proven && !stack_.empty() && !instr.typeHint.contains(stack_.back().tag()))
                vmError("cannot assign " + stack_.back().typeName() + " to '" + shownName(chunkStr(instr.operand)) + "' declared as " +
                        instr.typeHint.name(), line);
            const bool settled = typedHere || proven;
            if (instr.slot >= 0 && instr.slot < (int)f.slotDefined.size() && f.slotDefined[instr.slot] &&
                !stack_.empty() && (settled || f.slotTypes[(size_t)instr.slot].contains(stack_.back().tag()))) {
                f.slots[instr.slot] = std::move(stack_.back());
                stack_.pop_back();
                break;
            }
            // a boxed local or a captured variable that is already defined: write its cell
            if (instr.slot < 0 && (settled || f.localTypes.empty()) && !stack_.empty()) {
                if (isBoxedSlot(instr.slot)) {
                    if (Value* v = f.findOwn(chunkStr(instr.operand), instr.slot)) { *v = std::move(stack_.back()); stack_.pop_back(); break; }
                } else if (isUpvalSlot(instr.slot) && f.captured && f.captured->upvals) {
                    auto& ups = *f.captured->upvals;
                    size_t u = (size_t)upvalIndex(instr.slot);
                    if (u < ups.size() && ups[u]->defined) { ups[u]->v = std::move(stack_.back()); stack_.pop_back(); break; }
                }
            }
            // not a local yet: with only the top frame running, resolveStore() would end up in interp_.set() anyway
            if (onlyGlobalsLeft(f) && (settled || f.localTypes.empty()) && !slotIsOwnLocal(f, instr.slot)) { storeGlobal(instr.operand, instr.typeHint, line); break; }
            resolveStore(chunkStr(instr.operand), pop(), line, instr.slot, instr.typeHint, settled);
            break;
        }

        case Op::DEFINE: {
            // the usual one: a plain local slot of this frame. same as resolveDef(), minus its generality
            CallFrame& f = *top_;
            const uint32_t sl = (uint32_t)instr.slot;
            if (sl < f.slotDefined.size() && !stack_.empty()) [[likely]] {
                const std::string& nm = chunkStr(instr.operand);
                f.slotTypes[sl] = (instr.flags & kAuto) ? TypeSet(stack_.back().tag()) : TypeSet::Any();
                f.slots[sl] = std::move(stack_.back());
                stack_.pop_back();
                f.slotDefined[sl] = 1;
                if (!f.scopeMarks.empty()) f.scopeFlat.push_back({&nm, instr.slot});
                break;
            }
            resolveDef(chunkStr(instr.operand), pop(), instr.slot, (instr.flags & kAuto) != 0);
            break;
        }

        case Op::DEFINE_TYPED: {
            CallFrame& f = *top_;
            const uint32_t sl = (uint32_t)instr.slot;
            if (sl < f.slotDefined.size() && !stack_.empty() && !instr.typeHint.isAny() && instr.typeHint.contains(stack_.back().tag())) [[likely]] {
                const std::string& nm = chunkStr(instr.operand);
                f.slots[sl] = std::move(stack_.back());
                stack_.pop_back();
                f.slotDefined[sl] = 1;
                f.slotTypes[sl] = instr.typeHint;
                if (!f.scopeMarks.empty()) f.scopeFlat.push_back({&nm, instr.slot});
                break;
            }
            resolveDefTyped(chunkStr(instr.operand), pop(), instr.typeHint, line, instr.slot);
            break;
        }

        case Op::LOAD_GLOBAL:
            if (instr.flags & kCallee) {
                Value* gp = globalPtr(instr.operand, line);
                if (gp->tg == 5) { stack_.push_back(calleeMark(gp)); break; }
            }
            pushGlobal(instr.operand, line);
            break;

        case Op::STORE_GLOBAL:
            storeGlobal(instr.operand, instr.typeHint, line);
            break;

        case Op::DEFINE_GLOBAL: {
            Value v = pop();
            if (instr.flags & kAuto) { TypeSet ts(v.tag()); interp_.define(chunkStr(instr.operand), std::move(v), ts); }
            else                     interp_.define(chunkStr(instr.operand), std::move(v));
            break;
        }

        case Op::DEFINE_GLOBAL_TYPED: {
            Value v = pop();
            const std::string& nm = chunkStr(instr.operand);
            if (!instr.typeHint.isAny() && !instr.typeHint.contains(v.tag()))
                vmError("cannot assign " + v.typeName() + " to '" + shownName(nm) + "' declared as " +
                        instr.typeHint.name(), line);
            interp_.define(nm, std::move(v), instr.typeHint);
            break;
        }

        case Op::APPEND_VAR: {
            Value* target = varForOp(instr);   // fills the global cache too, so the hot loop finds the variable next time
            if (target && target->isString() && !stack_.empty()) {
                Value r = pop();
                target->strMut() += r.isString() ? r.asString() : r.formatAsString();
                (*top_).ip = (int)instr.arg;
                if (injBuf_) injBuf_.reset();
            }
            break;
        }

        case Op::MARK_MODULE_LOCAL:
            interp_.markModuleLocal(chunkStr(instr.operand));
            break;

        // arithmetic
        // Int stays exact for +,-,*,%. mixed or float widens to double. division always gives a float
        // a pointer operand whose tag has a matching __tag_<op> dunder (add/sub/mul/div/mod/gt/lt/gte/lte)
        // calls it instead of raising, left operand first then right (see resolveDunder() in core/registry.h)
        case Op::ADD: {
            // two ints (the hot case) are added in place on the stack, no pops and pushes
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = intAdd(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() + rr.asNumber()); stack_.pop_back(); break; }   // a float (or an int and a float), in place too
            }
            Value r = pop(), l = pop();
            if (l.isString()) {
                // l is our own copy of the variable, so grow it instead of building a third string
                std::string& ls = l.strMut();
                if (r.isString()) ls += r.asString(); else ls += r.formatAsString();
                push(std::move(l));
                break;
            }
            if (r.isString())                        { push(Value(l.formatAsString() + r.asString())); break; }
            if (l.isInt() && r.isInt())               { push(intAdd(l.asInt(), r.asInt())); break; }
            if (l.isNumeric() && r.isNumeric())       { push(Value(l.asNumber() + r.asNumber())); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "add", dfn) || resolveDunder(interp_, r, "add", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '+': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::SUB: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = intSub(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() - rr.asNumber()); stack_.pop_back(); break; }   // a float (or an int and a float), in place too
            }
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt())         { push(intSub(l.asInt(), r.asInt())); break; }
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.asNumber() - r.asNumber())); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "sub", dfn) || resolveDunder(interp_, r, "sub", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '-': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::MUL: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = intMul(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() * rr.asNumber()); stack_.pop_back(); break; }   // a float (or an int and a float), in place too
            }
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt())         { push(intMul(l.asInt(), r.asInt())); break; }
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.asNumber() * r.asNumber())); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "mul", dfn) || resolveDunder(interp_, r, "mul", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '*': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::DIV: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isNumeric() && rr.isNumeric() && rr.asNumber() != 0.0) { ll = Value(ll.asNumber() / rr.asNumber()); stack_.pop_back(); break; }
            }
            Value r = pop(), l = pop();
            if (l.isNumeric() && r.isNumeric()) {
                double rd = r.asNumber();
                if (rd == 0.0) vmError("division by zero", line);
                push(Value(l.asNumber() / rd));
                break;
            }
            Value dfn;
            if (resolveDunder(interp_, l, "div", dfn) || resolveDunder(interp_, r, "div", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '/': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::MOD: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt() && rr.asInt() != 0) { ll = intMod(ll.asInt(), rr.asInt()); stack_.pop_back(); break; }
            }
            Value r = pop(), l = pop();
            if (l.isInt() && r.isInt()) {
                int64_t rv = r.asInt();
                if (rv == 0) vmError("modulo by zero", line);
                push(intMod(l.asInt(), rv));
                break;
            }
            if (l.isNumeric() && r.isNumeric()) {
                double rd = r.asNumber();
                if (rd == 0.0) vmError("modulo by zero", line);
                push(Value(std::fmod(l.asNumber(), rd)));
                break;
            }
            Value dfn;
            if (resolveDunder(interp_, l, "mod", dfn) || resolveDunder(interp_, r, "mod", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '%': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::BAND: case Op::BOR: case Op::BXOR: case Op::SHL: case Op::SHR: {
            Value r = pop(), l = pop();
            const char* sym = instr.op == Op::BAND ? "&" : instr.op == Op::BOR ? "|" : instr.op == Op::BXOR ? "^" : instr.op == Op::SHL ? "<<" : ">>";
            const char code = instr.op == Op::BAND ? '&' : instr.op == Op::BOR ? '|' : instr.op == Op::BXOR ? '^' : instr.op == Op::SHL ? 'l' : 'r';
            if (l.isInt() && r.isInt()) {
                int64_t out;
                if (!bitOpInts(code, l.asInt(), r.asInt(), out)) vmError("shift count " + std::to_string(r.asInt()) + " is out of range, it has to be 0 to 63", line);
                push(Value(out));
                break;
            }
            Value dfn;
            const char* dn = instr.op == Op::BAND ? "band" : instr.op == Op::BOR ? "bor" : instr.op == Op::BXOR ? "bxor" : instr.op == Op::SHL ? "shl" : "shr";
            if (resolveDunder(interp_, l, dn, dfn) || resolveDunder(interp_, r, dn, dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError(std::string("'") + sym + "' needs two ints, got " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::BNOT: {
            Value v = pop();
            if (v.isInt()) { push(Value(~v.asInt())); break; }
            Value dfn;
            if (resolveDunder(interp_, v, "bnot", dfn)) { EMBR_CALL_DUNDER1(dfn, v) }
            vmError("'~' needs an int, got " + v.typeName(), line);
            break;
        }
        case Op::NEG: {
            Value v = pop();
            if (v.isNumeric()) { push(v.isInt() ? intNeg(v.asInt()) : Value(-v.asNumber())); break; }
            // dunder fallback. '!' (Op::NOT) has none, truthy() already gives every value an answer
            Value dfn;
            if (resolveDunder(interp_, v, "neg", dfn)) { EMBR_CALL_DUNDER1(dfn, v) }
            vmError("unsupported operand for unary '-': " + v.typeName(), line);
            break;
        }
        case Op::NOT: { Value v = pop(); push(Value(!v.truthy())); break; }

        // comparison. "==" and "!=" are never dunder-overridable (see arithDunderOp())
        case Op::EQ:  { Value r=pop(),l=pop(); push(Value(l==r)); break; }
        case Op::NEQ: { Value r=pop(),l=pop(); push(Value(l!=r)); break; }
        case Op::LT: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() < rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() < rr.asNumber()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()<r.asInt()) : (l.asNumber()<r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "lt", dfn) || resolveDunder(interp_, r, "lt", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '<': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::GT: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() > rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() > rr.asNumber()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()>r.asInt()) : (l.asNumber()>r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "gt", dfn) || resolveDunder(interp_, r, "gt", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '>': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::LTE: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() <= rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() <= rr.asNumber()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()<=r.asInt()) : (l.asNumber()<=r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "lte", dfn) || resolveDunder(interp_, r, "lte", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '<=': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }
        case Op::GTE: {
            if (stack_.size() >= 2) {
                Value& rr = stack_.back(); Value& ll = stack_[stack_.size() - 2];
                if (ll.isInt() && rr.isInt()) { ll = Value(ll.asInt() >= rr.asInt()); stack_.pop_back(); break; }
                if (ll.isNumeric() && rr.isNumeric()) { ll = Value(ll.asNumber() >= rr.asNumber()); stack_.pop_back(); break; }
            }
            Value r=pop(),l=pop();
            if (l.isNumeric() && r.isNumeric()) { push(Value(l.isInt()&&r.isInt() ? (l.asInt()>=r.asInt()) : (l.asNumber()>=r.asNumber()))); break; }
            Value dfn;
            if (resolveDunder(interp_, l, "gte", dfn) || resolveDunder(interp_, r, "gte", dfn)) { EMBR_CALL_DUNDER2(dfn, l, r) }
            vmError("unsupported operands for '>=': " + l.typeName() + " and " + r.typeName(), line);
            break;
        }

        // control flow
        case Op::JUMP:
            if (interp_.budgetOn && instr.operand <= (*top_).ip) budgetStep();
            (*top_).ip = instr.operand;
            if (injBuf_) injBuf_.reset(); // jump clears injection buffer
            break;

        case Op::JUMP_IF_FALSE:
            if (!pop().truthy()) {
                (*top_).ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        case Op::JUMP_IF_FALSE_PEEK:
            if (!peek().truthy()) {
                (*top_).ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        case Op::JUMP_IF_TRUE_PEEK:
            if (peek().truthy()) {
                (*top_).ip = instr.operand;
                if (injBuf_) injBuf_.reset();
            }
            break;

        // scope
        case Op::SCOPE_PUSH: {
            auto& f = (*top_);
            f.scopeDepth++;
            f.scopeMarks.push_back((uint32_t)f.scopeFlat.size());
            break;
        }

        case Op::SCOPE_POP: {
            auto& f = (*top_);
            if (f.scopeDepth > 0 && !f.scopeMarks.empty()) {
                closeScope(f);
                f.scopeDepth--;
            }
            break;
        }

        // try/catch
        case Op::TRY_PUSH: {
            auto& f = (*top_);
            f.tryHandlers.push_back({instr.operand, f.scopeDepth, stack_.size(), f.iterStack.size()});
            break;
        }

        case Op::TRY_POP:
            (*top_).tryHandlers.pop_back();
            break;

        // destructuring
        case Op::UNPACK: {
            int count = instr.operand;
            Value src = pop();
            std::vector<Value> vals;
            if (src.isArray()) {
                const auto& arr = src.asArray();
                if ((int)arr.size() < count)
                    vmError("cannot unpack " + std::to_string(count) + " variable" + (count == 1 ? "" : "s") +
                            " from an array of " + std::to_string(arr.size()) + " element" +
                            (arr.size() == 1 ? "" : "s"), line);
                vals.assign(arr.begin(), arr.begin() + count);
            } else if (src.isMap()) {
                if (count != 2)
                    vmError("unpacking a map requires exactly 2 targets (keys, values), got " +
                            std::to_string(count), line);
                Value::array_type ks, vs;
                for (const auto& [k, v] : src.asMap()) { ks.push_back(Value(k)); vs.push_back(v); }
                vals = { Value(std::move(ks)), Value(std::move(vs)) };
            } else {
                vmError("cannot unpack " + src.typeName() + " into " + std::to_string(count) +
                         " variable" + (count == 1 ? "" : "s"), line);
            }
            for (int i = count - 1; i >= 0; --i) push(vals[i]);
            break;
        }

        // for
        case Op::ITER_INIT: {
            Value src = pop();
            // `for x in v` on a typed pointer calls __<tag>_iter(v), which returns something to loop over (an array, a map or a string).
            // the ITER_INIT then runs again on that result: the ip goes back to it before the call
            if (src.isPointer() && fromChunk) {
                Value dfn;
                if (resolveDunder(interp_, src, "iter", dfn)) { (*top_).ip -= 1; EMBR_CALL_DUNDER1(dfn, src) }
            }
            IterState st;
            st.varCount = instr.operand;
            if (src.isArray()) {
                if (st.varCount == 2)
                    vmError("for loop over an array takes a single loop variable, not 'for k, v in ...'", line);
                int64_t rs, rt, rc;
                if (src.rangeParts(rs, rt, rc)) { st.mode = IterState::Mode::Range; st.rStart = rs; st.rStep = rt; st.count = (size_t)rc; }
                else { st.mode = IterState::Mode::Array; st.count = src.arraySize(); st.src = std::move(src); }
            } else if (src.isString()) {
                if (st.varCount == 2)
                    vmError("for loop over a string takes a single loop variable, not 'for k, v in ...'", line);
                for (char c : src.asString()) st.items.push_back(Value(std::string(1, c)));
            } else if (src.isMap()) {
                for (const auto& [k, v] : src.asMap()) {
                    if (st.varCount == 2) { st.keys.push_back(Value(k)); st.items.push_back(v); }
                    else                    st.items.push_back(Value(k));
                }
            } else {
                vmError("cannot iterate over " + src.typeName() + " with for loop", line);
            }
            (*top_).iterStack.push_back(std::move(st));
            break;
        }

        case Op::ITER_NEXT: {
            auto& st = (*top_).iterStack.back();
            if (st.mode != IterState::Mode::Items) {   // an array or a range: no list was made for the loop
                if (st.idx >= st.count) { (*top_).ip = instr.operand; break; }
                if (st.mode == IterState::Mode::Range)
                    stack_.push_back(Value((int64_t)((uint64_t)st.rStart + (uint64_t)st.idx * (uint64_t)st.rStep)));
                else
                    stack_.push_back(st.src.arrayGet(st.idx));
                ++st.idx;
                break;
            }
            if (st.idx >= st.items.size()) {
                (*top_).ip = instr.operand;
                break;
            }
            if (st.varCount == 2) {
                push(st.keys[st.idx]);   // key (below)
                push(st.items[st.idx]);  // value (TOS)
            } else {
                push(st.items[st.idx]);
            }
            ++st.idx;
            break;
        }

        case Op::ITER_POP:
            (*top_).iterStack.pop_back();
            break;

        // collections
        case Op::BUILD_ARRAY: {
            int n = instr.operand;
            Value::array_type arr(n);
            for (int i = n - 1; i >= 0; --i) arr[i] = pop();
            push(Value(std::move(arr)));
            break;
        }

        case Op::BUILD_RANGE: {
            // [a...b] and [a, b...c]: the numbers are whole, the end is included, the step is second - start
            auto whole = [&](Value& v, const char* what) -> int64_t {
                if (v.isInt()) return v.asInt();
                if (v.isNumber()) {
                    double d = v.asNumber();
                    if (d == std::floor(d) && d >= -9.2e18 && d <= 9.2e18) return (int64_t)d;
                }
                vmError(std::string("the ") + what + " of a range has to be a whole number, got " + v.formatAsString(), line);
            };
            const bool stepped = instr.operand == 3;
            Value ev = pop();
            Value sv = stepped ? pop() : Value(int64_t(0));
            Value bv = pop();
            const int64_t start = whole(bv, "start"), end = whole(ev, "end");
            const int64_t second = stepped ? whole(sv, "second number") : 0;
            // magnitudes in unsigned 64-bit, so the whole int64 range works without overflow
            auto gap = [](int64_t from, int64_t to, bool& up) -> uint64_t { up = to >= from; return up ? (uint64_t)to - (uint64_t)from : (uint64_t)from - (uint64_t)to; };
            bool stepUp = true, endUp = true;
            const uint64_t sm = stepped ? gap(start, second, stepUp) : 1;
            if (stepped && sm == 0) vmError("the step of a range can't be zero: its first two numbers are equal", line);
            const uint64_t dm = gap(start, end, endUp);
            // empty when the end is on the wrong side of the start for the direction of the step
            const uint64_t count64 = (dm == 0 || endUp == stepUp) ? dm / sm + 1 : 0;
            if (sm > (uint64_t)INT64_MAX || count64 > (uint64_t)INT64_MAX) vmError("this range is too long", line);
            const int64_t step = stepUp ? (int64_t)sm : -(int64_t)sm, count = (int64_t)count64;
            push(Value::makeRange(start, step, count));
            break;
        }

        case Op::BUILD_MAP: {
            int n = instr.operand;
            Value::map_type m;
            // pairs were pushed in order: key0, val0, key1, val1, ...
            std::vector<std::pair<Value,Value>> pairs(n);
            for (int i = n - 1; i >= 0; --i) {
                pairs[i].second = pop(); // val
                pairs[i].first  = pop(); // key
            }
            for (auto& [k, v] : pairs) {
                if (!k.isString())
                    vmError("map keys must be strings", line);
                m[k.asString()] = std::move(v);
            }
            push(Value(std::move(m)));
            break;
        }

        case Op::INDEX_GET: {
            Value key  = pop();
            Value cont = pop();
            { Value dfn; if (cont.isPointer() && resolveDunder(interp_, cont, "index", dfn)) EMBR_CALL_DUNDER2(dfn, cont, key) }   // v[i] on a typed pointer
            indexGet(cont, key, line);
            break;
        }

        case Op::INDEX_GET_VAR: {
            Value key = pop();
            // the variable itself, not a copy. indexGet() runs no user code, so nothing can change it meanwhile
            if (instr.flags & kStash) {
                Value stash = pop();
                if (!isStashMark(stash)) {   // a copy taken before the key ran
                    Value dfn; if (stash.isPointer() && resolveDunder(interp_, stash, "index", dfn)) EMBR_CALL_DUNDER2(dfn, stash, key)
                    indexGet(stash, key, line); break;
                }
            }
            const Value* cont = varForOp(instr);
            if (!cont) { Value v = resolveLoad(chunkStr(instr.operand), line, instr.slot); indexGet(v, key, line); break; }
            { Value dfn; if (cont->isPointer() && resolveDunder(interp_, *cont, "index", dfn)) { Value copy = *cont; EMBR_CALL_DUNDER2(dfn, copy, key) } }
            indexGet(*cont, key, line);
            break;
        }

        case Op::STASH_VAR: {
            Value* v = varForOp(instr);
            if (!v) { push(resolveLoad(chunkStr(instr.operand), line, instr.slot)); break; }   // raises undefined variable
            if ((instr.flags & kCallFree) || slotIsOwnLocal((*top_), instr.slot) ||
                ((instr.flags & kPureCalls) && pureCalleesNative(instr))) {
                Value mark; mark.nestDepth = kStashMark;
                push(std::move(mark));
            } else {
                push(*v);
            }
            break;
        }

        case Op::REF_PATH: {
            RefPath path;
            path.nameIdx = instr.operand;
            path.slot    = instr.slot;
            path.name    = chunkStr(instr.operand);
            path.keys.resize((size_t)instr.arg);
            for (size_t i = path.keys.size(); i-- > 0; ) path.keys[i] = pop();
            push(makeRefPath(std::move(path)));
            break;
        }

        case Op::INDEX_SET: {
            // stack: [container, key, new_value] (new_value on top)
            Value newVal = pop();
            Value key    = pop();
            Value cont   = pop();
            // v[i] = x on a typed pointer calls __<tag>_setindex(v, i, x), which returns the object, and that is what the store writes back
            { Value dfn; if (cont.isPointer() && resolveDunder(interp_, cont, "setindex", dfn)) EMBR_CALL_DUNDER3(dfn, cont, key, newVal) }
            indexSetInPlace(cont, key, std::move(newVal), line);
            push(std::move(cont));
            break;
        }

        case Op::INDEX_SET_VAR: {
            // stack: [stash?, key, new_value]
            Value newVal = pop();
            Value key    = pop();
            Value stash;
            bool  copy = false;
            if (instr.flags & kStash) { stash = pop(); copy = !isStashMark(stash); }
            if (copy) {
                // the variable may have changed while the key and value ran, so work on the copy and let the STORE write it back
                { Value dfn; if (stash.isPointer() && resolveDunder(interp_, stash, "setindex", dfn)) EMBR_CALL_DUNDER3(dfn, stash, key, newVal) }
                indexSetInPlace(stash, key, std::move(newVal), line);
                push(std::move(stash));
                break;
            }
            Value* target = varForOp(instr);
            if (!target) { resolveLoad(chunkStr(instr.operand), line, instr.slot); break; }   // raises undefined variable
            // a typed pointer: the dunder returns the object and the STORE after this instruction writes it back, so it is not skipped
            { Value dfn; if (target->isPointer() && resolveDunder(interp_, *target, "setindex", dfn)) { Value copy = *target; EMBR_CALL_DUNDER3(dfn, copy, key, newVal) } }
            indexSetInPlace(*target, key, std::move(newVal), line);
            (*top_).ip = (int)instr.arg;   // skip the STORE
            if (injBuf_) injBuf_.reset();
            break;
        }

        case Op::APPEND_ARR: {
            if (stack_.size() < 3) break;
            const Value& callee = calleeValue(stack_[stack_.size() - 3]);   // may be a mark
            Value& stash  = stack_[stack_.size() - 2];
            bool   fresh  = isStashMark(stash);
            Value* target = fresh ? varForOp(instr) : nullptr;
            if (fresh && !target) { resolveLoad(chunkStr(instr.operand), line, instr.slot); break; }   // raises undefined variable
            if (target && target->isArray() && callee.isCallable() && callee.asCallable().isNative() &&
                callee.asCallable().name() == "push") {
                Value x = pop(); pop(); pop();
                target->arrayAppend(std::move(x));
                (*top_).ip = (int)instr.arg;   // skip the CALL and the STORE
                if (injBuf_) injBuf_.reset();
                break;
            }
            if (fresh) stash = *target;   // the ordinary call below needs the real array
            break;
        }

        // functions
        case Op::MAKE_CLOSURE: {
            int fnIdx = instr.operand;
            // looked up in the sibling list of the chunk running right now, not prog_: a chunk that escaped from
            // another program (say a module's exported function called later) runs under a different prog_
            // see CompiledChunk::siblings
            const auto& siblings = *(*top_).chunk->siblings;
            if (fnIdx < 0 || fnIdx >= (int)siblings.size())
                vmError("invalid function index " + std::to_string(fnIdx), line);
            auto chunk = siblings[fnIdx];
            ScriptFn sf;
            sf.name          = chunk->name;
            sf.params        = chunk->params;
            sf.returnType    = chunk->returnType;
            sf.definedAt     = chunk->definedAt;
            sf.sourceFile    = chunk->sourceFile;
            sf.captured      = makeCaptured(*chunk);
            sf.compiledChunk = chunk;
            push(Value::makeScript(std::move(sf)));
            break;
        }

        case Op::CALL: {
            int argc = instr.operand;

            // import sentinel
            if (argc == -1) {
                Value pathVal = pop();
                if (!pathVal.isString())
                    vmError("import path must evaluate to a string, got " + pathVal.typeName(), line);
                execImport(pathVal.asString(), line);
                push(Value(0.0));
                break;
            }

            // a callee pushed by a kCallee load: a mark that points at where the function lives. a plain script function, a typed native or a native that
            // takes a view of its arguments is called from there, and anything else puts a real copy where the mark was and goes the ordinary way
            if (stack_.size() > (size_t)argc && isCalleeMark(stack_[stack_.size() - (size_t)argc - 1])) {
                const size_t calleeAt = stack_.size() - (size_t)argc - 1;
                const Value& cv = calleeValue(stack_[calleeAt]);
                if (cv.tg == 5) {
                    const Value::Callable& fc = cv.asCallable();
                    if (!fc.isNative()) {
                        if (fc.script.compiledChunk) {
                            auto* fchunk = static_cast<CompiledChunk*>(fc.script.compiledChunk.get());
                            if (fchunk->callKind == 0) [[unlikely]] fchunk->classifyCall();
                            if (fchunk->callKind == 1 && (size_t)argc == fchunk->params.size() && fchunk->argsFit(stack_.data() + calleeAt + 1)) {
                                pushFrameSimple(fchunk, fc.script.captured, (size_t)argc, fc.script.returnType);
                                break;
                            }
                        }
                    } else if (fc.typed.thunk && typedFits(fc, stack_.data() + calleeAt + 1, argc)) {
                        Value r = callTypedNative(fc, stack_.data() + calleeAt + 1, line);
                        for (int i = 0; i < argc; ++i) stack_.pop_back();
                        stack_.back() = std::move(r);
                        break;
                    } else if (fc.span) {
                        callSpanNative(fc, argc, line);
                        break;
                    }
                }
                stack_[calleeAt] = calleeValue(stack_[calleeAt]);
            }
            // normal call stack is [callee, arg0, arg1, ..., argN]
            // args are on TOS, callee below them
            // fast path: a plain script call takes its args straight off the stack, no vector
            if (!(instr.flags & kHasRef) && stack_.size() > (size_t)argc) {
                const Value& cv = stack_[stack_.size() - argc - 1];
                if (cv.isCallable()) {
                    const auto& fc = cv.asCallable();
                    if (!fc.isNative() && fc.script.compiledChunk) {
                        const ScriptFn& fsf = fc.script;
                        auto* fchunk = static_cast<CompiledChunk*>(fsf.compiledChunk.get());
                        if (fchunk->callKind == 0) [[unlikely]] fchunk->classifyCall();
                        if (fchunk->callKind == 1 && (size_t)argc == fchunk->params.size() &&
                            fchunk->argsFit(stack_.data() + stack_.size() - (size_t)argc)) [[likely]]
                            pushFrameSimple(fchunk, fsf.captured, (size_t)argc, fsf.returnType);
                        else
                            pushFrameFromStack(fchunk, fsf.captured, (size_t)argc, fsf.returnType, fsf.name);
                        break;
                    }
                }
            }
            // the bare name of an overload group: the member that fits these arguments goes where the callee is, and the CALL runs again
            if (stack_.size() > (size_t)argc) {
                Value& cs = stack_[stack_.size() - (size_t)argc - 1];
                if (cs.isCallable() && cs.asCallable().overloadDispatcher) {
                    std::string err;
                    const Value* member = pickOverload(interp_, cs.asCallable().name(), stack_.data() + stack_.size() - (size_t)argc, (size_t)argc, err);
                    if (!member) vmError(err, line);
                    cs = *member;
                    goto redispatch;
                }
            }
            // a native that takes a view of its arguments is called on the values where they are on the stack, with no vector
            if (!(instr.flags & kHasRef) && stack_.size() > (size_t)argc) {
                const Value& cv = stack_[stack_.size() - (size_t)argc - 1];
                if (cv.isCallable() && cv.asCallable().span) {
                    callSpanNative(cv.asCallable(), argc, line);
                    break;
                }
            }
            ArgVec args(*this, (size_t)argc);
            for (int i = argc - 1; i >= 0; --i) args[i] = pop();
            Value callee = pop();

            if (!callee.isCallable()) {
                // dunder fallback, see core/registry.h's resolveDunder().
                Value dfn;
                if (resolveDunder(interp_, callee, "call", dfn)) {
                    args.insert(args.begin(), callee);
                    callee = dfn;
                } else {
                    vmError("value is not callable (got " + callee.typeName() + ")", line);
                }
            }

            const auto& c = callee.asCallable();
            const bool hasRef = (instr.flags & kHasRef) != 0;
            if (hasRef) resolveRefArgs(c, args, line);
            if (c.isNative()) {
                checkNativeSig(c, args, line, true);
                try {
                    Value result = c.native(args);
                    if (!c.returns.isAny() && !c.returns.contains(result.tag()))
                        vmError("internal error: '" + c.name() + "' returned " + result.typeName() + " but is declared to return " +
                                c.returns.name() + " (a bug in the plugin)", line);
                    if (hasRef) {
                        settleRefs(args);
                        if (isRef(result)) vmError("'" + c.name() + "' returned a reference. natives must not hand one back", line);
                    }
                    push(std::move(result));
                } catch (EmbrError& e) {
                    if (hasRef) settleRefs(args);
                    // same as invoke()'s re-raise below: add this call's line and the native's name only if the native's
                    // own error didn't already carry a location. otherwise rethrow it as it was, so its kind ("assert",
                    // "error", a plugin's own tag, ...) reaches catch intact
                    if (!e.hasLocation) {
                        try { raiseError(e.keepContext ? e.context : c.name(), e.message, errRange(line), interp_.sourceMap()); }
                        catch (EmbrError& located) { located.trace = std::move(e.trace); located.keepContext = e.keepContext; throw; }
                    }
                    throw;
                }
                break;
            }

            // script call
            const ScriptFn& sf = c.script;
            if (sf.compiledChunk) {
                // push a new call frame and continue the dispatch loop
                auto* chunk = static_cast<CompiledChunk*>(sf.compiledChunk.get());
                pushFrame(chunk, sf.captured, args, sf.returnType, sf.name);
            } else {
                vmError("cannot invoke '" + sf.name + "': it was not compiled by the VM", line);
            }
            break;
        }

        case Op::RETURN: {
            // the usual return: from a function that declared no return type, to a caller that is another frame
            CallFrame& lf = (*top_);
            if (frames_.size() > 1 && !stack_.empty() && (lf.returnType.isAny() || lf.returnType.contains(stack_.back().tag()))) [[likely]] {   // a wrong type: doReturn raises
                const int leavingBase = lf.stackBase;
                Value result = pop();
                if (injBuf_) injBuf_.reset();   // injected code belongs to the frame that is leaving, see doReturn
                frames_.pop_back(); syncTop();
                interp_.exitCall();
                while ((int)stack_.size() > leavingBase) stack_.pop_back();
                push(std::move(result));
                break;
            }
            Value result = pop();
            doReturn(std::move(result));
            break;
        }

        // async
        case Op::YIELD:
            state_ = State::Suspended; refreshHot();
            break;

        } // end switch
    }

    // frame management

    void pushFrame(
        CompiledChunk*                        chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        const std::vector<Value>&             args,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        // copies the args into the slots, nothing is taken off the value stack
        pushFrameImpl(chunk, captured, const_cast<Value*>(args.data()), args.size(), false, 0, returnType, fnName);
    }

    // CALL with the callee and argc args on top of the value stack: moves the args straight into the new frame's
    // slots and pops them. chunk, captured and fnName may point into the callee on the stack, they are copied first
    void pushFrameFromStack(
        CompiledChunk*                        chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        size_t                                argc,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        pushFrameImpl(chunk, captured, stack_.data() + (stack_.size() - argc), argc, true, argc + 1, returnType, fnName);
    }

    // the call that nearly every call is: exactly the declared number of arguments, no rest parameter, no parameter or return
    // types, nothing captured in a cell (CompiledChunk::callKind). the same frame pushFrameImpl makes, without its checks
    [[gnu::always_inline]] void pushFrameSimple(CompiledChunk* chunk, const std::shared_ptr<CaptureFrame>& captured, size_t argc,
                                                TypeSet returnType) {
        if (interp_.budgetOn) [[unlikely]] budgetStep();
        interp_.enterCall();
        CallFrame& frame = frames_.emplace_back();
        frame.chunk = chunk;
        if (!returnType.isAny()) [[unlikely]] frame.returnType = returnType;   // a reused frame is already any
        if (captured) frame.captured = captured;
        const size_t n = (size_t)chunk->numLocalSlots;
        if (frame.slots.size() < n) frame.slots.resize(n);
        frame.slotDefined.assign(n, 0);
        frame.slotTypes.assign(n, TypeSet::Any());
        Value* args = stack_.data() + (stack_.size() - argc);
        Value* sl = frame.slots.data();
        for (size_t i = 0; i < argc; ++i) {
            const int sc = chunk->paramSlots[i];
            Value& dst = sl[sc];
            if (dst.tg < 2) [[likely]] {   // a fresh slot holds a plain number: the argument is moved in by hand
                dst.u = args[i].u; dst.nestDepth = args[i].nestDepth; dst.tg = args[i].tg;
                args[i].tg = 0; args[i].nestDepth = 0;
            } else dst = std::move(args[i]);
            frame.slotDefined[(size_t)sc] = 1;
        }
        // the arguments were moved out, so they hold plain numbers now; the callee is a mark or a counted copy
        {
            Value& callee = args[-1];
            if (callee.tg >= 2) callee.~Value();
            stack_.setSize(stack_.size() - argc - 1);
        }
        frame.stackBase = (int)stack_.size();
        frame.updateFast();
        syncTop();
        if (injBuf_) injBuf_.reset();
    }

    // the same, as a call of its own: the hot loop keeps it out of its body so the compiler has fewer things to keep in registers there
    [[gnu::noinline]] void pushFrameSimpleOut(CompiledChunk* chunk, const std::shared_ptr<CaptureFrame>& captured, size_t argc,
                                              TypeSet returnType) {
        pushFrameSimple(chunk, captured, argc, returnType);
    }

    // a function handed to a parameter of a written function type takes the number of arguments the type says. a native without a
    // signature, and a function with optional or rest parameters, take more than one number, so they are let through when it fits any
    void checkFnArity(const Value& f, const TypeAnn& want, const std::string& param, const std::string& fnName) {
        const Value::Callable& c = f.asCallable();
        size_t lo = 0, hi = 0;
        if (c.isNative()) {
            if (c.sig.empty()) return;
            lo = c.sigInfo.minArgs; hi = c.sigInfo.maxArgs;
        } else {
            bool variadic = false;
            for (const Param& p : c.script.params) { if (p.variadic) variadic = true; else { ++hi; if (!p.optional) ++lo; } }
            if (variadic) hi = SIZE_MAX;
        }
        const size_t n = want.params.size();
        if (n >= lo && n <= hi) return;
        auto args = [](size_t k) { return std::to_string(k) + (k == 1 ? " argument" : " arguments"); };
        vmError("argument '" + param + "' to '" + fnName + "': expected a function that takes " + args(n) + ", but got one that takes " +
                (lo == hi ? args(lo) : "at least " + args(lo)), 0);
    }

    // popCount values are taken off the value stack once the args are bound (the args live there for the stack path)
    void pushFrameImpl(
        CompiledChunk*                        chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        Value*                                args,
        size_t                                argc,
        bool                                  moveArgs,
        size_t                                popCount,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        if (interp_.budgetOn) budgetStep();
        bool   hasVariadic = !chunk->params.empty() && chunk->params.back().variadic;
        size_t fixedCount  = hasVariadic ? chunk->params.size() - 1 : chunk->params.size();

        // typecheck fixed params
        for (size_t i = 0; i < fixedCount && i < argc; ++i) {
            const auto& p = chunk->params[i];
            if (!p.type.isAny() && !p.type.contains(args[i].tag()))
                vmError("argument '" + p.name + "' to '" + fnName + "': expected " +
                        p.type.name() + " but got " + args[i].typeName(), 0);
            // a function type is only checked for how many arguments it takes, the rest of it is for the static checker
            if (p.elem && p.elem->isFn && args[i].isCallable()) checkFnArity(args[i], *p.elem, p.name, fnName);
        }
        if (argc < fixedCount)
            vmError("'" + fnName + "' expects " + (hasVariadic ? "at least " : "") +
                    std::to_string(fixedCount) + " args, got " + std::to_string(argc), 0);
        // typecheck the variadic tail, if it declares a type
        if (hasVariadic && !chunk->params.back().type.isAny()) {
            const auto& vp = chunk->params.back();
            for (size_t i = fixedCount; i < argc; ++i)
                if (!vp.type.contains(args[i].tag()))
                    vmError("argument " + std::to_string(i) + " to '" + fnName + "' ('..." + vp.name +
                            "'): expected " + vp.type.name() + " but got " + args[i].typeName(), 0);
        }

        // recursion guard. paired with the exitCall() in dropTopFrame()/doReturn() where this frame is popped.
        // a scope-based guard wouldn't work here because the frame outlives pushFrame() for a plain in-VM CALL
        interp_.enterCall();

        // built where it will live. moving a CallFrame is not cheap, it has a lot of members
        frames_.emplace_back();
        CallFrame& frame = frames_.back();
        frame.chunk      = chunk;
        frame.captured   = captured;
        frame.returnType = returnType;

        if (frame.slots.size() < (size_t)chunk->numLocalSlots) frame.slots.resize((size_t)chunk->numLocalSlots);
        frame.slotDefined.assign((size_t)chunk->numLocalSlots, false);
        frame.slotTypes.assign((size_t)chunk->numLocalSlots, TypeSet::Any());
        if (chunk->hasBoxed)
            frame.cells = std::make_shared<std::vector<std::shared_ptr<Cell>>>(chunk->numLocalSlots);
        // every param got a slot up front in Compiler::compileFunction, so slotOf() here can never miss. a parameter that
        // an inner function captures lives in a cell
        auto bindParam = [&](int sc, Value&& v) {
            // a parameter with no slot of its own can only come from a damaged compiled file
            if (sc >= 0 ? (size_t)sc >= frame.slotDefined.size() : (!isBoxedSlot(sc) || !frame.cells || (size_t)boxedIndex(sc) >= frame.cells->size()))
                vmError("damaged compiled code: a parameter of '" + fnName + "' has no local slot", 0);
            if (sc >= 0) { frame.slots[sc] = std::move(v); frame.slotDefined[sc] = true; return; }
            auto cell = interp_.newCell();
            cell->v = std::move(v);
            cell->defined = true;
            (*frame.cells)[(size_t)boxedIndex(sc)] = std::move(cell);
        };
        if (chunk->paramSlots.size() != chunk->params.size()) [[unlikely]] chunk->fillParamSlots();
        for (size_t i = 0; i < fixedCount; ++i) {
            int slot = chunk->paramSlots[i];
            bindParam(slot, moveArgs ? std::move(args[i]) : Value(args[i]));
        }
        if (hasVariadic) {
            int slot = chunk->paramSlots.back();
            Value::array_type rest;
            rest.reserve(argc - fixedCount);
            for (size_t i = fixedCount; i < argc; ++i) rest.push_back(moveArgs ? std::move(args[i]) : args[i]);
            bindParam(slot, Value(std::move(rest)));
        }
        for (size_t i = 0; i < popCount; ++i) stack_.pop_back();
        frame.stackBase = (int)stack_.size();

        frame.updateFast();
        syncTop();
        // clear injection buffer when entering a new frame
        injBuf_.reset();
    }

    void doReturn(Value result) {
        if (frames_.empty()) { state_ = State::Done; refreshHot(); push(std::move(result)); return; }

        if (frames_.size() == 1 && frames_[0].isPersistentTop) {
            // the persistent top-level frame
            // don't pop it, so its locals survive for a later run() call
            // discard anything the frame left on the stack and stop
            while ((int)stack_.size() > frames_[0].stackBase) stack_.pop_back();
            state_ = State::Done; refreshHot();
            push(std::move(result));
            return;
        }

        // take what is needed from the frame, then pop it where it is (moving it out first costs more than it saves)
        CallFrame& lf = frames_.back();
        const TypeSet leavingType = lf.returnType;
        const int     leavingBase = lf.stackBase;
        const bool    badType     = !leavingType.isAny() && !leavingType.contains(result.tag());
        std::string   badName;
        if (badType) badName = lf.chunk->name;   // a function value's name is always its chunk's
        // injected code belongs to the frame that is leaving. kept, the caller would go on running the rest of it
        injBuf_.reset();
        frames_.pop_back(); syncTop();
        interp_.exitCall();

        // typecheck return value
        if (badType)
            vmError("'" + badName + "' declared return type " + leavingType.name() + " but returned " + result.typeName(), 0);

        // discard any values the returning frame left on the stack above its base
        while ((int)stack_.size() > leavingBase) stack_.pop_back();

        if (frames_.empty()) state_ = State::Done; refreshHot();
        push(std::move(result)); // caller receives return value
    }

    // import: a ".embr" path is a script module, a ".vmbr" path is a compiled one, anything else is a native plugin
    void execImport(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;
        const auto ext = fs::path(rawPath).extension();
        if (ext == ".embr")      execImportEmbrModule(rawPath, line);
        else if (ext == ".vmbr") execImportCompiledModule(rawPath, line);
        else                     execImportPlugin(rawPath, line);
    }

    // import a native plugin (.so / .dll / .dylib)
    void execImportPlugin(const std::string& rawPath, int line) {
        // locating, policy-checking and registering a plugin is shared by every backend (core/module.h)
        std::string err = importNativePlugin(interp_, rawPath);
        if (!err.empty()) vmError(err, line);
    }

    // import an embr script as a module. with a cache folder set (Interpreter::cacheDir) a compiled copy is reused
    void execImportEmbrModule(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;

        auto cands = fileCandidates(rawPath, interp_.scriptDir, ".embr", "modules");
        fs::path resolved = findExistingCandidate(cands);
        if (resolved.empty())
            vmError("cannot find module \"" + rawPath + "\"\n  tried:" + describeCandidates(cands), line);

        const std::string canonStr = canonicalOrSelf(resolved);
        if (interp_.loadedModules_.count(canonStr)) return;
        interp_.loadedModules_.insert(canonStr);

        auto src = tryReadFile(resolved);
        if (!src) vmError("cannot open module file: " + resolved.string(), line);

        // a wrong `__module__`, or a module that needs a capability the host denies, is refused before any of it runs
        if (std::string why = checkModuleBeforeRun(interp_, resolved, *src); !why.empty()) {
            interp_.loadedModules_.erase(canonStr);
            vmError(why, line);
        }

        runModule(resolved, [&] {
            return compileSourcePreferCached(*src, interp_, resolved.string(), interp_.cacheDir);
        });
    }

    // import a compiled module (a .vmbr file). there is no source, so an error in it shows no source line
    void execImportCompiledModule(const std::string& rawPath, int line) {
        namespace fs = std::filesystem;

        auto cands = fileCandidates(rawPath, interp_.scriptDir, ".vmbr", "modules");
        fs::path resolved = findExistingCandidate(cands);
        if (resolved.empty())
            vmError("cannot find module \"" + rawPath + "\"\n  tried:" + describeCandidates(cands), line);

        const std::string canonStr = canonicalOrSelf(resolved);
        if (interp_.loadedModules_.count(canonStr)) return;
        interp_.loadedModules_.insert(canonStr);

        auto bytes = readWholeFile(resolved.string());
        if (!bytes) vmError("cannot open module file: " + resolved.string(), line);
        if (std::string why = checkCompiledBeforeRun(interp_, resolved, *bytes); !why.empty()) {
            interp_.loadedModules_.erase(canonStr);
            vmError(why, line);
        }

        runModule(resolved, [&] {
            CompiledProgram compiled = deserializeProgram(*bytes, resolved.string(), nullptr);
            registerChunkTeardown(interp_, compiled);
            interp_.setSource(SourceMap(), resolved.string());
            return compiled;
        });
    }

    // runs a module's program in a scope of its own and merges what it defines into the importer's scope (see
    // exportModuleScope in module.h, it does not return a map the way load_module() does). `make` produces the
    // program, so compile errors are handled like run errors. it gets a fresh VmRunner, because running it on
    // this one would clobber the frame that is executing now
    template<class MakeProgram>
    void runModule(const std::filesystem::path& resolved, MakeProgram&& make) {
        std::string savedDir  = interp_.scriptDir;
        SourceMap   savedMap  = interp_.sourceMap();
        std::string savedFile = interp_.currentFile();
        interp_.scriptDir = resolved.parent_path().string();

        interp_.pushModuleScope();
        try {
            CompiledProgram compiled = make();
            VmRunner moduleRunner(interp_);
            moduleRunner.run(compiled);
        } catch (...) {
            interp_.popModuleScope();  // discard result on error
            interp_.scriptDir = savedDir;
            interp_.setSource(savedMap, savedFile);
            throw;
        }

        auto res = interp_.popModuleScope();
        interp_.scriptDir = savedDir;
        interp_.setSource(savedMap, savedFile);

        exportModuleScope(interp_, std::move(res), /*mutateCallerScope=*/true);
    }

    // invoke helper for the public invoke() method
    Value invokeChunk(
        CompiledChunk*                        chunk,
        const std::shared_ptr<CaptureFrame>&  captured,
        const std::vector<Value>&             args,
        const SourceRange&                    /*site*/,
        TypeSet                               returnType,
        const std::string&                    fnName)
    {
        // push new frame and run dispatch loop until the frame returns
        // record the depth to detect when the frame returns
        int targetDepth = (int)frames_.size();
        pushFrame(chunk, captured, args, returnType, fnName);
        frames_.back().fromNative = true;

        // RAII: if anything escapes this call (an uncaught EmbrError or any other exception),
        // pop every frame pushed since targetDepth so their gc roots and call-depth entries are released
        // dismissed on normal exit, where doReturn()/handleTry() already did this bookkeeping
        struct UnwindGuard {
            VmRunner& vm; int depth; bool armed = true;
            ~UnwindGuard() {
                if (!armed) return;
                while ((int)vm.frames_.size() > depth) {
                    vm.dropTopFrame();
                }
            }
        } guard{*this, targetDepth};

        State savedState = state_;
        struct StopDepth {
            VmRunner& vm; size_t saved;
            ~StopDepth() { vm.stopDepth_ = saved; vm.refreshHot(); }
        } stopGuard{*this, stopDepth_};
        stopDepth_ = (size_t)targetDepth;
        state_ = State::Running; refreshHot();
        while ((int)frames_.size() > targetDepth && state_ == State::Running) {
            try {
                refreshHot();
                if (hot_) { do { fastRun(); if (hot_) execOne<true>(); } while (hot_); }
                else execOne<false>();
            } catch (EmbrError& e) {
                // a handler at or below targetDepth belongs to whatever outer call pushed frames up to targetDepth not to this invocation
                // leave it for that caller's own loop and let the exception propagate out of this call instead
                if (!handleTry(e, targetDepth)) throw;
            } catch (const std::bad_alloc&) { outOfMemory(targetDepth);
            } catch (const std::length_error&) { outOfMemory(targetDepth); }
        }

        guard.armed = false;
        Value result = stack_.empty() ? Value(0.0) : pop();
        state_ = savedState; refreshHot();
        return result;
    }

    // turns the reference paths among a call's arguments into references. done after every argument is in, so
    // evaluating a later argument can't move the container a reference points into
    void resolveRefArgs(const Value::Callable& c, std::vector<Value>& args, int line) {
        if (!c.isNative() || c.sig.empty())
            vmError("'" + c.name() + "' can't take '&': only natives with an in-out parameter do", line);
        for (auto& a : args) {
            if (!isRefPath(a)) continue;
            RefPath& p = refPathOf(a);
            Value* root = varBySlot(p.nameIdx, p.slot);
            if (!root) { resolveLoad(p.name, line, p.slot); vmError("undefined variable: " + p.name, line); }   // resolveLoad raises
            std::shared_ptr<RefChain> chain;
            std::string err;
            Value* target = resolveRefPath(root, p, chain, err);
            if (!target) vmError("&" + p.name + ": " + err, line);
            a = makeRef(target, std::move(chain));
        }
    }

    // native sig validation. the quick test covers a call that is plainly fine (see Callable::SigInfo); anything else, an error included,
    // goes through the full check below, so the messages come from one place. mayHaveRefs: the call site may have passed `&name`
    template <class Args>   // a std::vector<Value> or an ArgSpan
    [[gnu::always_inline]] void checkNativeSig(const Value::Callable& c, const Args& args, int line, bool mayHaveRefs) {
        if (c.sig.empty()) return;
        const auto& si = c.sigInfo;
        const size_t n = args.size();
        if (si.quick && n >= si.minArgs && n <= si.maxArgs) {
            size_t i = 0;
            for (; i < n; ++i) {
                if (!si.types[i].contains(args[i].tag())) break;
                if (mayHaveRefs && args[i].tg == 6 && isRef(args[i])) break;
            }
            if (i == n) return;
        }
        checkNativeSig(c.name(), c.sig, args, line);
    }
    template <class Args>   // a std::vector<Value> or an ArgSpan
    void checkNativeSig(const std::string& fname,
                        const std::vector<Param>& sig,
                        const Args& args,
                        int line) {
        if (sig.empty()) return;
        size_t minArgs = 0;
        bool   hasVariadic = false;
        for (const auto& p : sig) {
            if (p.variadic) { hasVariadic = true; break; }
            if (!p.optional) ++minArgs;
        }
        size_t maxArgs = hasVariadic ? SIZE_MAX : sig.size();
        if (args.size() < minArgs)
            vmError("too few arguments to '" + fname + "': expected " +
                    std::to_string(minArgs) + " but got " +
                    std::to_string(args.size()), line);
        if (args.size() > maxArgs)
            vmError("too many arguments to '" + fname + "': expected at most " +
                    std::to_string(maxArgs) + " but got " +
                    std::to_string(args.size()), line);
        for (size_t i = 0; i < args.size(); ++i) {
            const Param* p = (i < sig.size() && !sig[i].variadic) ? &sig[i] : nullptr;
            if (!p) for (auto it = sig.rbegin(); it != sig.rend(); ++it)
                if (it->variadic) { p = &*it; break; }
            const bool ref = isRef(args[i]);
            if (p && p->inout != ref)
                vmError(ref ? "argument '" + p->name + "' to '" + fname + "' is not an in-out parameter, so it can't take '&'"
                            : "argument '" + p->name + "' to '" + fname + "' is changed in place, so pass it as &name",
                        line);
            if (!p && ref) vmError("'" + fname + "' can't take '&' for argument " + std::to_string(i + 1), line);
            if (!p || p->type.isAny()) continue;
            const Value& shown = ref ? refTarget(args[i]) : args[i];
            if (!p->type.contains(shown.tag()))
                vmError("argument '" + p->name + "' to '" + fname + "': expected " +
                        p->type.name() + " but got " + shown.typeName(), line);
        }
    }

    // error helper
    // the range of the instruction that is running, with columns when the chunk has them. falls back to just the line
    SourceRange errRange(int line) const {
        if (line > 0 && !injBuf_ && !frames_.empty()) {
            const auto& f = frames_.back();
            if (f.chunk && f.ip >= 1 && f.chunk->lineAt(f.ip - 1) == line) {
                SourceRange r = f.chunk->rangeAt(f.ip - 1);
                if (r.startCol > 0) {
                    // clamp to the line, a multi-line expression is underlined to the end of its first line
                    int len = (int)interp_.sourceMap().get(line).size();
                    r.endCol = std::max(r.startCol + 1, std::min(r.endCol, len + 1));
                    return r;
                }
            }
        }
        return {line, 0, line, 0};
    }

    [[noreturn]] void vmError(const std::string& msg, int line) const {
        SourceRange r = errRange(line);
        std::ostringstream out;
        out << "[vm] " << msg;
        if (line > 0) out << " at line " << line;
        if (r.startCol > 0) out << ", col " << r.startCol;
        out << "\n";
        if (line > 0) {
            const std::string& ln = interp_.sourceMap().get(line);
            if (!ln.empty()) {
                out << "  " << ln << "\n";
                if (r.startCol > 0)
                    out << "  " << std::string((size_t)(r.startCol - 1), ' ') << std::string((size_t)(r.endCol - r.startCol), '^') << "\n";
            }
        }
        throw EmbrError(out.str(), line > 0, r, msg, "vm");
    }

    static size_t editDist(const std::string& a, const std::string& b, size_t cap = 3) {
        if (a.size() > b.size() + cap || b.size() > a.size() + cap) return cap + 1;
        const size_t na = a.size(), nb = b.size();
        std::vector<std::vector<size_t>> dp(na+1, std::vector<size_t>(nb+1));
        for (size_t i = 0; i <= na; ++i) dp[i][0] = i;
        for (size_t j = 0; j <= nb; ++j) dp[0][j] = j;
        for (size_t i = 1; i <= na; ++i) {
            size_t rowMin = SIZE_MAX;
            for (size_t j = 1; j <= nb; ++j) {
                size_t cost = (a[i-1] == b[j-1]) ? 0 : 1;
                size_t v = std::min({dp[i-1][j]+1, dp[i][j-1]+1, dp[i-1][j-1]+cost});
                if (i>1&&j>1&&a[i-1]==b[j-2]&&a[i-2]==b[j-1]) v=std::min(v,dp[i-2][j-2]+1);
                dp[i][j] = v; rowMin = std::min(rowMin, v);
            }
            if (rowMin > cap) return cap + 1;
        }
        return dp[na][nb];
    }

    // typo suggestions
    std::string suggest(const std::string& name) const {
        std::string best; size_t bestD = 3;
        auto consider = [&](const std::string& k) {
            size_t d = editDist(name, k, bestD);
            if (d < bestD) { bestD = d; best = k; }
        };
        if (!frames_.empty()) {
            const CallFrame& f = frames_.back();
            for (auto& [k, sc] : f.chunk->localSlots) {
                if (sc >= 0) { if (sc < (int)f.slotDefined.size() && f.slotDefined[sc]) consider(k); }
                else if (isBoxedSlot(sc) && f.cells) {
                    const auto& c = (*f.cells)[(size_t)boxedIndex(sc)];
                    if (c && c->defined) consider(k);
                }
            }
            if (f.captured && f.captured->upvals)
                for (auto& [k, u] : f.chunk->upvalNames)
                    if ((size_t)u < f.captured->upvals->size() && (*f.captured->upvals)[(size_t)u]->defined) consider(k);
            if (f.captured)
                for (auto& layer : f.captured->layers)
                    for (auto& [k, _] : *layer) consider(k);
        }
        for (auto& [k, _] : interp_.globals()) {
            size_t d = editDist(name, k, bestD);
            if (d < bestD) { bestD = d; best = k; }
        }
        return best;
    }
};


// public API


// compile a source string returning the program
// ---- overload groups at run time -----------------------------------------------------------------------------------------
// a call of an overload group's bare name (`abs(x)`) whose argument types were not known before the script ran comes here. the
// members are `abs__1`, `abs__2`, ... (overloadName). the one whose parameters take every argument and are the narrowest wins; two
// that fit equally well, or none, is an error that lists them
// the member of the group `base` for these arguments, or null with `err` set (none fits, or two fit equally well)
inline const Value* pickOverload(Interpreter& interp, const std::string& base, const Value* args, size_t n, std::string& err) {
    // a member's parameter types; false when it can't be a member (a native without a signature)
    auto paramsOf = [](const Value::Callable& c, std::vector<TypeSet>& ps, size_t& minArgs, bool& variadic) {
        ps.clear(); minArgs = 0; variadic = false;
        if (c.isNative()) {
            if (c.sig.empty()) return false;
            for (const Param& p : c.sig) {
                if (p.variadic) { variadic = true; continue; }
                ps.push_back(p.type);
                if (!p.optional) minArgs = ps.size();
            }
            return true;
        }
        if (!c.script.compiledChunk) return false;
        const CompiledChunk* ch = static_cast<CompiledChunk*>(c.script.compiledChunk.get());
        for (const Param& p : ch->params) { if (p.variadic) variadic = true; else ps.push_back(p.type); }
        minArgs = ps.size();
        return true;
    };
    const Value* best = nullptr; int bestScore = 0; bool tie = false;
    std::vector<TypeSet> ps; size_t minArgs; bool variadic;
    for (const auto& [num, nm] : interp.overloadGroupCached(base)) {
        (void)num;
        const Value* fv = interp.find(nm);
        if (!fv || !fv->isCallable() || !paramsOf(fv->asCallable(), ps, minArgs, variadic)) continue;
        if (n < minArgs || (!variadic && n > ps.size())) continue;
        bool ok = true; int score = 0;
        for (size_t i = 0; i < n && ok; ++i) {
            const TypeSet p = i < ps.size() ? ps[i] : TypeSet::Any();
            if (!p.contains(args[i].tag())) ok = false;
            score += __builtin_popcount(p.mask);
        }
        if (!ok) continue;
        if (!best || score < bestScore) { best = fv; bestScore = score; tie = false; }
        else if (score == bestScore) tie = true;
    }
    if (best && !tie) return best;
    // the error: what was passed, and the members (all of them, or the ones that tied)
    std::string have, list;
    for (size_t i = 0; i < n; ++i) have += (i ? ", " : "") + args[i].typeName();
    for (const auto& [num, nm] : interp.overloadGroupCached(base)) {
        (void)num;
        const Value* fv = interp.find(nm);
        if (!fv || !fv->isCallable() || !paramsOf(fv->asCallable(), ps, minArgs, variadic)) continue;
        if (tie) {   // only the members that fit as well as the best one
            if (n < minArgs || (!variadic && n > ps.size())) continue;
            bool ok = true; int score = 0;
            for (size_t i = 0; i < n && ok; ++i) {
                const TypeSet p = i < ps.size() ? ps[i] : TypeSet::Any();
                if (!p.contains(args[i].tag())) ok = false;
                score += __builtin_popcount(p.mask);
            }
            if (!ok || score != bestScore) continue;
        }
        std::string text = nm + "(";
        for (size_t i = 0; i < ps.size(); ++i) text += (i ? ", " : "") + ps[i].name();
        text += variadic ? (ps.empty() ? "...)" : ", ...)") : ")";
        list += (list.empty() ? "" : ", ") + text;
    }
    if (!best) err = "no overload of '" + base + "' takes (" + have + "); candidates: " + (list.empty() ? "none" : list);
    else       err = "the call of '" + base + "' with (" + have + ") fits more than one overload equally well: " + list;
    return nullptr;
}

// the dispatcher as a plain native, for a call that doesn't come from a script CALL (a native like sort calling it back).
// a script's own call of the bare name is picked by the VM instead (see Op::CALL) and runs as an ordinary call
inline Value dispatchOverload(Interpreter& interp, const std::string& base, const std::vector<Value>& args) {
    std::string err;
    const Value* member = pickOverload(interp, base, args.data(), args.size(), err);
    if (!member) raiseError("overload", err);
    VmRunner r(interp);
    return r.invoke(*member, args, {});
}
// makes the bare name of each group the runtime dispatcher, unless something else already has that name
inline void installOverloads(Interpreter& interp, const std::set<std::string>& bases) {
    if (!interp.overloadInstaller) {
        Interpreter* self = &interp;
        interp.overloadInstaller = [self](const std::string& base) { installOverloads(*self, {base}); };
    }
    for (const std::string& base : bases) {
        if (interp.isOverloadDispatcher(base)) continue;
        if (interp.hasGlobal(base))
            raiseError("overload", "'" + base + "' is already a plain name, so it can't also be the overload group of " + base + "__N");
        Interpreter* ip = &interp;
        Value disp = Value::makeNative(base, [ip, base](const std::vector<Value>& args) { return dispatchOverload(*ip, base, args); }, {});
        const_cast<Value::Callable&>(disp.asCallable()).overloadDispatcher = true;
        interp.defineGlobal(base, std::move(disp));
        interp.markOverloadDispatcher(base);
    }
}

// what the static checker may know about `import "x.embr"`: the names the module makes at its top level, from checking its source. null when
// it can't be found, read or parsed (the import itself reports that when it runs). the module's own mistakes are not reported here
struct ModuleTypeResolver {
    Interpreter* interp;   // may be null: the natives are then not known
    std::map<std::string, std::unique_ptr<typecheck::ModuleExports>> cache;
    std::set<std::string> inProgress;
    explicit ModuleTypeResolver(Interpreter* i) : interp(i) {}

    typecheck::Options optionsFor(const std::string& baseDir) {
        typecheck::Options o;
        o.interp = interp;
        o.importModule = [this, baseDir](const std::string& raw) { return resolve(baseDir, raw); };
        return o;
    }
    const typecheck::ModuleExports* resolve(const std::string& baseDir, const std::string& rawPath) {
        namespace fs = std::filesystem;
        auto found = findExistingCandidate(fileCandidates(rawPath, baseDir, ".embr", "modules"));
        if (found.empty()) return nullptr;
        const std::string key = canonicalOrSelf(found);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second.get();
        if (!inProgress.insert(key).second) return nullptr;   // two modules that import each other
        auto src = tryReadFile(found);
        std::unique_ptr<typecheck::ModuleExports> out;
        if (src) {
            try {
                Lexer lex(*src);
                auto tokens = lex.tokenize();
                SourceMap sm = lex.sourceMap();
                sm.setBaseDir(found.parent_path().string());
                Parser parser(std::move(tokens), sm);
                auto ast = parser.parse();
                out = std::make_unique<typecheck::ModuleExports>();
                typecheck::Options o = optionsFor(found.parent_path().string());
                o.isModule = true;
                o.source = &sm;
                o.exportsOut = out.get();
                (void)typecheck::check(ast, o);
            } catch (const EmbrError&) { out.reset(); }
        }
        inProgress.erase(key);
        return (cache[key] = std::move(out)).get();
    }
};

inline CompiledProgram compileSource(const std::string& src,
                                     Interpreter& interp,
                                     const std::string& filename = "<input>",
                                     const CompileOptions& opts = CompileOptions{}) {
    Lexer lex(src);
    auto tokens = lex.tokenize();
    SourceMap sm = lex.sourceMap();
    sm.setBaseDir(dirOfFile(filename));
    interp.setSource(sm, filename);
    Parser parser(std::move(tokens), sm);
    // the parser keeps going after an error to find the rest. all of them are shown, and the script does not run
    std::vector<EmbrError> parseErrors;
    parser.collectErrors(&parseErrors);
    auto ast = parser.parse();
    if (parseErrors.size() == 1) throw parseErrors[0];
    if (!parseErrors.empty()) {
        std::string all;
        for (const EmbrError& e : parseErrors) all += e.what();
        const EmbrError& first = parseErrors[0];
        throw EmbrError(all, first.hasLocation, first.range, first.message, first.context);
    }
    if (opts.preloadPlugins) {
        // an import at the top of the file that can't fail the way a script module can: a plugin named by a literal.
        // a failure is left for the import itself to report when it runs
        for (auto& st : ast) {
            if (st->kind != Stmt::Kind::Import) continue;
            auto* im = static_cast<ImportStmt*>(st.get());
            if (!im->pathExpr || im->pathExpr->kind != Expr::Kind::String) continue;
            const std::string& pth = static_cast<StringExpr*>(im->pathExpr.get())->v;
            if (pth.find(".embr") != std::string::npos || pth.find(".vmbr") != std::string::npos) continue;
            try { importNativePlugin(interp, pth, /*runModules=*/false); } catch (...) {}
        }
    }
    if (opts.typeCheck || opts.useTypes) {   // a mistake that can't be right stops the script before it runs (see core/typecheck.h)
        ModuleTypeResolver modules(&interp);
        typecheck::Options topt = modules.optionsFor(interp.scriptDir);
        topt.closedWorld = opts.closedWorld;
        topt.isModule = interp.insideImportedModule();
        topt.source = &sm;
        typecheck::ModuleExports made;
        topt.exportsOut = &made;
        auto errors = typecheck::check(ast, topt);
        if (opts.typeCheck && !errors.empty()) throw errors.front();
        // what the checker found out about the elements of this file's globals, for the next file or REPL line
        for (const auto& [n, e] : made) interp.setStaticElem(n, e.el);
    }
    {   // the bare name of every overload group this file declares or the interpreter already has is its runtime dispatcher
        std::set<std::string> bases;
        std::string b;
        for (auto& st : ast)
            if (st->kind == Stmt::Kind::Fn && overloadName(static_cast<FnStmt*>(st.get())->name, &b)) bases.insert(b);
        for (const std::string& nb : interp.overloadBases()) bases.insert(nb);
        installOverloads(interp, bases);
    }
    Compiler compiler(&interp);
    compiler.options.inPlaceOps = opts.inPlaceOps;
    compiler.options.elideScopes = opts.elideScopes;
    compiler.options.useTypes = opts.useTypes;
    auto prog = compiler.compile(ast, filename);
#ifndef NDEBUG
    verifyStack(prog, "internal error, the compiler made code the stack checker refuses");   // the checker and the compiler must agree
#endif
    registerChunkTeardown(interp, prog);
    return prog;
}

// compile and run in one call
inline void runSource(const std::string& src,
                      Interpreter& interp,
                      const std::string& filename = "<input>") {
    auto prog = compileSource(src, interp, filename);
    VmRunner runner(interp);
    try {
        runner.run(prog);
    } catch (const EmbrError& e) {
        std::cerr << e.what() << formatTrace(e);
    }
}

} // namespace vm
} // namespace embr

#endif // EMBR_WITH_VM
#endif // EMBR_BACKENDS_VM_H
