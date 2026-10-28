// vm_bytecode.h
// saves compiled scripts to `.vmbr` files and loads them back. embr only gives you the helpers: it never
// writes a file by itself, the program that embeds it decides when to write, prefer or clean (`embr` does it
// only when you pass --cache-dir).
//
// usage
//   auto prog = embr::vm::compileSourcePreferCached(src, interp, "x.embr", "/path/to/cache");
//   embr::vm::VmRunner(interp).run(prog);
//   embr::vm::cleanCache("/path/to/cache");     // drop files that no longer match their source or this build
//
// good to know
//   - a cached file is only used when its source hash, the format version and the op set all match, so an
//     old file is never misread. a file that is missing, stale or damaged is just a cache miss (nothing throws).
//   - a loaded file is checked before it can run: indexes (strings, constants, jumps, slots, captured variables),
//     and that every path leaves the value stack, the loop stack and the try handlers in a state the ops can rely
//     on (verifyStack). a file that passes can't crash the VM or read outside it. it is still code, so it can do
//     anything a script can: it is not a sandbox. run it under the same plugin policy you would give a script.
//   - a program you load but don't run must go through releaseProgram() (one you run is freed by registerChunkTeardown).
//   - sourceFile of a loaded program is the name you pass in, not the one it was compiled under.
//   - numbers are written little endian, so a file means the same on every platform.

#ifndef EMBR_BACKENDS_VM_BYTECODE_H
#define EMBR_BACKENDS_VM_BYTECODE_H

#include "vm_disasm.h"
#include "../core/vmbr_meta.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

#ifdef EMBR_WITH_VM
namespace embr {
namespace vm {

constexpr uint32_t kVmbrFormatVersion = vmbr::kFormatVersion;   // see core/vmbr_meta.h
constexpr int kOpCount = (int)Op::TRY_POP + 1;   // TRY_POP must stay the last op

inline uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ULL) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
using vmbr::blockHash;
inline uint64_t hashSource(const std::string& src) { return blockHash(src.data(), src.size()); }
// a script with `const import` is made of more than its own text: the hash covers the files it reads, so a cached compile is not used after one changes
inline uint64_t hashSource(const std::string& src, const std::string& file) {
    return hashSource(src) + embr::includeDigest(src, dirOfFile(file));
}

// changes whenever an op is added, removed, renamed or reordered, so a file from another op set is refused
inline uint64_t opSetFingerprint() {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < kOpCount; ++i) {
        const char* n = opName((Op)i);
        h = fnv1a(n, std::strlen(n), h);
        h = fnv1a("\0", 1, h);
    }
    return h;
}

namespace bc_detail {

struct Writer {
    std::string out;
    void u8(uint8_t v)   { out.push_back((char)v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back((char)((v >> (8 * i)) & 0xFF)); }
    void i32(int32_t v)  { u32((uint32_t)v); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back((char)((v >> (8 * i)) & 0xFF)); }
    void i64(int64_t v)  { u64((uint64_t)v); }
    void f64(double d)   { uint64_t b; std::memcpy(&b, &d, 8); u64(b); }
    void str(const std::string& s) { u32((uint32_t)s.size()); out += s; }
};

// every read checks that the bytes are there, and every count is checked against what is left, so a damaged
// file fails cleanly instead of reading past the end or asking for a huge allocation
struct Reader {
    const std::string& in;
    size_t pos = 0;
    explicit Reader(const std::string& s) : in(s) {}
    [[noreturn]] void bad(const char* why) const { raiseError("vmbr", std::string("bad compiled file: ") + why); }
    void need(size_t n) const { if (in.size() - pos < n) bad("unexpected end of file"); }
    uint8_t u8()   { need(1); return (uint8_t)in[pos++]; }
    uint32_t u32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)in[pos + i] << (8 * i); pos += 4; return v; }
    int32_t i32()  { return (int32_t)u32(); }
    uint64_t u64() { need(8); uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (uint64_t)(uint8_t)in[pos + i] << (8 * i); pos += 8; return v; }
    int64_t i64()  { return (int64_t)u64(); }
    double f64()   { uint64_t b = u64(); double d; std::memcpy(&d, &b, 8); return d; }
    std::string str() { uint32_t n = u32(); need(n); std::string s = in.substr(pos, n); pos += n; return s; }
    // a count of items that each take at least `each` bytes
    uint32_t count(size_t each) { uint32_t n = u32(); if (each && (size_t)n > (in.size() - pos) / each) bad("count is larger than the file"); return n; }
};

} // namespace bc_detail

// what the start of a file says, enough to decide whether the rest is worth reading
struct VmbrHeader {
    uint64_t    sourceHash = 0;
    std::string sourcePath;
    std::string meta;   // what a compiled module says about itself (core/modinfo.h). empty if the writer gave none
};


// writes `prog` as a .vmbr file image. sourceHash is hashSource() of the script it came from
inline std::string serializeProgram(const CompiledProgram& prog, uint64_t sourceHash, const std::string& sourcePath = "",
                                    const std::string& meta = "") {
    bc_detail::Writer w;
    w.out.append(vmbr::kMagic, 4);
    w.u32(kVmbrFormatVersion);
    w.u32((uint32_t)kOpCount);
    w.u64(opSetFingerprint());
    w.u64(sourceHash);
    w.str(sourcePath);
    w.str(meta);
    w.u32((uint32_t)prog.chunks.size());
    for (const auto& cp : prog.chunks) {
        const CompiledChunk& c = *cp;
        w.str(c.name);
        w.u32((uint32_t)c.params.size());
        for (const Param& p : c.params) {
            w.str(p.name);
            w.u8(p.type.mask);
            w.u8((uint8_t)((p.optional ? 1 : 0) | (p.variadic ? 2 : 0) | (p.inout ? 4 : 0)));
        }
        w.u8(c.returnType.mask);
        w.i32(c.definedAt.startLine); w.i32(c.definedAt.startCol); w.i32(c.definedAt.endLine); w.i32(c.definedAt.endCol);
        w.i32(c.parentIdx);
        w.i32(c.numLocalSlots);
        w.u8(c.hasBoxed ? 1 : 0);
        w.u32((uint32_t)c.strings.size());
        for (const auto& s : c.strings) w.str(s);
        w.u32((uint32_t)c.nums.size());
        for (double d : c.nums) w.f64(d);
        w.u32((uint32_t)c.ints.size());
        for (int64_t v : c.ints) w.i64(v);
        std::vector<Instruction> plain;
        const std::vector<Instruction>& code = plainCode(c, plain);   // never write a fused op
        w.u32((uint32_t)code.size());
        for (size_t i = 0; i < code.size(); ++i) {
            const Instruction& in = code[i];
            w.u8((uint8_t)in.op); w.u8(in.flags); w.u8(in.typeHint.mask); w.u8(0);
            w.i32(in.operand); w.i32(in.slot); w.i32(in.arg);
            w.i32(c.lineAt((int)i));
        }
        // maps are written in name order, so the same script always makes the same bytes
        std::vector<std::pair<std::string, int>> ls(c.localSlots.begin(), c.localSlots.end());
        std::sort(ls.begin(), ls.end());
        w.u32((uint32_t)ls.size());
        for (auto& kv : ls) { w.str(kv.first); w.i32(kv.second); }
        w.u32((uint32_t)c.upvalDescs.size());
        for (auto& d : c.upvalDescs) { w.u8(d.fromParentLocal ? 1 : 0); w.i32(d.index); }
        std::vector<std::pair<std::string, int>> un(c.upvalNames.begin(), c.upvalNames.end());
        std::sort(un.begin(), un.end());
        w.u32((uint32_t)un.size());
        for (auto& kv : un) { w.str(kv.first); w.i32(kv.second); }
        w.u32((uint32_t)c.pureSets.size());
        for (auto& set : c.pureSets) {
            w.u32((uint32_t)set.size());
            for (int idx : set) w.i32(idx);
        }
    }
    w.u64(blockHash(w.out.data(), w.out.size()));   // last, covers everything before it
    return std::move(w.out);
}

// the header of a file image. throws EmbrError if it is not a .vmbr from this format version and op set
inline VmbrHeader readVmbrHeader(const std::string& bytes) {
    bc_detail::Reader r(bytes);
    r.need(4);
    if (std::memcmp(bytes.data(), vmbr::kMagic, 4) != 0) r.bad("not a .vmbr file");
    r.pos = 4;
    if (r.u32() != kVmbrFormatVersion) r.bad("written by another format version");
    if (r.u32() != (uint32_t)kOpCount || r.u64() != opSetFingerprint()) r.bad("written by a build with a different instruction set");
    VmbrHeader h;
    h.sourceHash = r.u64();
    h.sourcePath = r.str();
    h.meta = r.str();
    return h;
}

// checks that nothing in the program can index outside its tables, so a stale or damaged file can't crash the VM
inline void validateProgram(const CompiledProgram& prog) {
    auto fail = [](size_t chunk, size_t ip, const std::string& why) {
        raiseError("vmbr", "bad compiled file: chunk " + std::to_string(chunk) + ", instruction " + std::to_string(ip) + ": " + why);
    };
    if (prog.chunks.empty()) raiseError("vmbr", "bad compiled file: no chunks");
    const int nChunks = (int)prog.chunks.size();
    for (size_t ci = 0; ci < prog.chunks.size(); ++ci) {
        const CompiledChunk& c = *prog.chunks[ci];
        if (c.lines.size() != c.code.size()) fail(ci, 0, "line table does not match the code");
        if (c.parentIdx < -1 || c.parentIdx >= nChunks || c.parentIdx == (int)ci) fail(ci, 0, "bad parent chunk");
        if (c.numLocalSlots < 0 || c.numLocalSlots > 1000000) fail(ci, 0, "bad local slot count");
        // a name maps to a plain slot, or to a boxed one (see kBoxedBase)
        for (auto& kv : c.localSlots) {
            int v = kv.second;
            bool ok = v >= 0 ? v < c.numLocalSlots : (isBoxedSlot(v) && boxedIndex(v) < c.numLocalSlots);
            if (!ok) fail(ci, 0, "bad local slot number");
        }
        // a closure made from this chunk takes its captured cells from the frame that makes it, so each one has to exist there
        if (!c.upvalDescs.empty() && c.parentIdx < 0) fail(ci, 0, "captures variables but has no enclosing function");
        for (auto& d : c.upvalDescs) {
            if (c.parentIdx < 0) break;
            const CompiledChunk& par = *prog.chunks[(size_t)c.parentIdx];
            bool ok = d.fromParentLocal ? (par.hasBoxed && d.index >= 0 && d.index < par.numLocalSlots)
                                        : (d.index >= 0 && d.index < (int)par.upvalDescs.size());
            if (!ok) fail(ci, 0, "bad captured variable");
        }
        if (!c.hasBoxed) for (auto& kv : c.localSlots) if (isBoxedSlot(kv.second)) fail(ci, 0, "boxed slot in a chunk with no boxed locals");
        for (auto& kv : c.upvalNames) if (kv.second < 0 || kv.second >= (int)c.upvalDescs.size()) fail(ci, 0, "bad upvalue number");
        for (auto& set : c.pureSets) for (int idx : set) if (idx < 0 || idx >= (int)c.strings.size()) fail(ci, 0, "bad string index in a pure set");
        std::vector<Instruction> plain;
        const std::vector<Instruction>& code = plainCode(c, plain);   // check what the compiler wrote, not the fused ops
        const int nStr = (int)c.strings.size(), n = (int)code.size();
        for (size_t ip = 0; ip < code.size(); ++ip) {
            const Instruction& in = code[ip];
            if ((int)in.op >= kOpCount) fail(ci, ip, "unknown op");
            auto strOk = [&](int i) { return i >= 0 && i < nStr; };
            switch (in.op) {
                case Op::PUSH_NUM: if (in.operand < 0 || in.operand >= (int)c.nums.size()) fail(ci, ip, "bad constant"); break;
                case Op::PUSH_INT: if (in.operand < 0 || in.operand >= (int)c.ints.size()) fail(ci, ip, "bad constant"); break;
                case Op::PUSH_STR: case Op::LOAD: case Op::STORE: case Op::DEFINE: case Op::DEFINE_TYPED:
                case Op::LOAD_GLOBAL: case Op::STORE_GLOBAL: case Op::DEFINE_GLOBAL: case Op::DEFINE_GLOBAL_TYPED:
                case Op::MARK_MODULE_LOCAL: case Op::APPEND_VAR: case Op::INDEX_GET_VAR:
                case Op::INDEX_SET_VAR: case Op::STASH_VAR: case Op::APPEND_ARR: case Op::REF_PATH:
                    if (!strOk(in.operand)) fail(ci, ip, "bad string index");
                    break;
                case Op::MAKE_CLOSURE:
                    if (in.operand < 0 || in.operand >= nChunks) fail(ci, ip, "bad function index");
                    else if (prog.chunks[(size_t)in.operand]->parentIdx != (int)ci) fail(ci, ip, "closure of a function written somewhere else");
                    break;
                case Op::CALL: if (in.operand < -1) fail(ci, ip, "bad argument count"); break;
                case Op::BUILD_ARRAY: case Op::BUILD_MAP: case Op::UNPACK: case Op::ITER_INIT:
                    if (in.operand < 0) fail(ci, ip, "bad count"); break;
                case Op::BUILD_RANGE:
                    if (in.operand != 2 && in.operand != 3) fail(ci, ip, "a range takes two or three numbers"); break;
                default: break;
            }
            if (opJumpsByOperand(in.op) && (in.operand < 0 || in.operand > n)) fail(ci, ip, "jump out of range");
            if (opJumpsByArg(in.op) && (in.arg < 0 || in.arg > n)) fail(ci, ip, "jump out of range");
            if (in.op == Op::STASH_VAR && (in.flags & kPureCalls) && (in.arg < 0 || in.arg >= (int)c.pureSets.size())) fail(ci, ip, "bad pure set");
            // where a name is defined or assigned has to be one of this frame's own variables. the VM trusts that
            // (the top-level chunk may also assign a global, which has no slot)
            if ((in.op == Op::DEFINE || in.op == Op::DEFINE_TYPED) && !(in.slot >= 0 || isBoxedSlot(in.slot)))
                fail(ci, ip, "a local is defined without a local slot");
            if (in.op == Op::STORE && ci != 0 && !(in.slot >= 0 || isBoxedSlot(in.slot) || isUpvalSlot(in.slot)))
                fail(ci, ip, "a store has no variable of the function to go to");
            if (in.slot >= c.numLocalSlots && in.slot >= 0) fail(ci, ip, "bad local slot");
            if (isBoxedSlot(in.slot) && (boxedIndex(in.slot) >= c.numLocalSlots || !c.hasBoxed)) fail(ci, ip, "bad boxed slot");
            if (isUpvalSlot(in.slot) && upvalIndex(in.slot) >= (int)c.upvalDescs.size()) fail(ci, ip, "bad upvalue slot");
        }
    }
}

// checks that every path through every chunk leaves the value stack, the loop (iterator) stack and the try
// handlers in a state the ops can rely on: an op never pops more than is there, ITER_NEXT / ITER_POP / TRY_POP have
// something to act on, and two paths that meet at one instruction agree on all three. the VM only asserts these,
// so without this a hand-made file could read from an empty stack. depths are counted from where the chunk starts,
// since a returning chunk drops whatever it left above its own base (see doReturn)
//
// each op's effect is in the comments next to the Op enum in vm.h. the ops with two outcomes are APPEND_VAR,
// INDEX_SET_VAR and APPEND_ARR (they either do their work in place and jump, or fall through to the ordinary code)
// and ITER_NEXT. a try block's catch code is reached with the stack as it was at TRY_PUSH, plus the error value
inline void verifyStack(const CompiledProgram& prog, const char* what = "bad compiled file") {
    constexpr int64_t kMaxCount = 1 << 20;   // BUILD_ARRAY 4000000000 would otherwise ask for a huge allocation
    struct State {
        int64_t depth = 0;
        std::vector<uint8_t> iters;   // loop variable count (1 or 2) of each open for loop
        int tries = 0;
        bool operator==(const State& o) const { return depth == o.depth && tries == o.tries && iters == o.iters; }
    };
    for (size_t ci = 0; ci < prog.chunks.size(); ++ci) {
        const CompiledChunk& c = *prog.chunks[ci];
        std::vector<Instruction> plain;
        const std::vector<Instruction>& code = plainCode(c, plain);
        const int n = (int)code.size();
        auto fail = [&](int ip, const std::string& why) {
            raiseError("vmbr", std::string(what) + ": chunk " + std::to_string(ci) + ", instruction " + std::to_string(ip) + ": " + why);
        };
        std::vector<std::optional<State>> at((size_t)n + 1);   // the state before each instruction. [n] is the end of the chunk
        std::vector<int> work;
        auto reach = [&](int from, int ip, State s) {
            if (ip < 0 || ip > n) fail(from, "jump out of range");
            auto& slot = at[(size_t)ip];
            if (!slot) { slot = std::move(s); if (ip < n) work.push_back(ip); }
            else if (!(*slot == s)) fail(from, "the stack is not the same on every path that reaches instruction " + std::to_string(ip));
        };
        reach(0, 0, State{});
        while (!work.empty()) {
            const int ip = work.back(); work.pop_back();
            State s = *at[(size_t)ip];
            const Instruction& in = code[(size_t)ip];
            auto need = [&](int64_t k) { if (s.depth < k) fail(ip, std::string(opName(in.op)) + " needs " + std::to_string(k) + " value(s) on the stack, has " + std::to_string(s.depth)); };
            auto count = [&](int64_t v) { if (v < 0 || v > kMaxCount) fail(ip, "count out of range"); return v; };
            auto next = [&](State t) { reach(ip, ip + 1, std::move(t)); };
            auto effect = [&](int64_t pops, int64_t pushes) { need(pops); s.depth += pushes - pops; next(s); };
            switch (in.op) {
                case Op::PUSH_NUM: case Op::PUSH_INT: case Op::PUSH_STR: case Op::PUSH_NIL:
                case Op::LOAD: case Op::LOAD_GLOBAL: case Op::STASH_VAR: case Op::MAKE_CLOSURE:
                    effect(0, 1); break;
                case Op::POP: case Op::STORE: case Op::STORE_GLOBAL: case Op::DEFINE: case Op::DEFINE_TYPED:
                case Op::DEFINE_GLOBAL: case Op::DEFINE_GLOBAL_TYPED:
                    effect(1, 0); break;
                case Op::MARK_MODULE_LOCAL: case Op::NOP: case Op::YIELD: case Op::SCOPE_PUSH: case Op::SCOPE_POP:
                    effect(0, 0); break;
                case Op::NEG: case Op::NOT: case Op::BNOT: effect(1, 1); break;
                case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD:
                case Op::BAND: case Op::BOR: case Op::BXOR: case Op::SHL: case Op::SHR:
                case Op::EQ: case Op::NEQ: case Op::LT: case Op::GT: case Op::LTE: case Op::GTE:
                case Op::INDEX_GET:
                    effect(2, 1); break;
                case Op::INDEX_SET: effect(3, 1); break;
                case Op::JUMP: reach(ip, in.operand, s); break;
                case Op::JUMP_IF_FALSE: need(1); s.depth -= 1; reach(ip, in.operand, s); next(s); break;
                case Op::JUMP_IF_FALSE_PEEK: case Op::JUMP_IF_TRUE_PEEK: need(1); reach(ip, in.operand, s); next(s); break;
                case Op::APPEND_VAR:       // in place: takes the value and jumps. otherwise changes nothing and falls through
                    need(1); next(s); s.depth -= 1; reach(ip, in.arg, s); break;
                case Op::BUILD_ARRAY: { int64_t k = count(in.operand); effect(k, 1); break; }
                case Op::BUILD_MAP:   { int64_t k = count(in.operand); effect(2 * k, 1); break; }
                case Op::BUILD_RANGE: { int64_t k = count(in.operand); effect(k, 1); break; }
                case Op::UNPACK:      { int64_t k = count(in.operand); effect(1, k); break; }
                case Op::REF_PATH:    { int64_t k = count(in.arg); effect(k, 1); break; }
                case Op::INDEX_GET_VAR: effect((in.flags & kStash) ? 2 : 1, 1); break;
                case Op::INDEX_SET_VAR: {  // in place: takes its operands and jumps. otherwise pushes the updated copy for the STORE after it
                    int64_t k = (in.flags & kStash) ? 3 : 2;
                    need(k); s.depth -= k; reach(ip, in.arg, s); s.depth += 1; next(s); break;
                }
                case Op::APPEND_ARR:       // in place: takes callee, stash and value and jumps. otherwise leaves them for the CALL
                    need(3); next(s); s.depth -= 3; reach(ip, in.arg, s); break;
                case Op::CALL:
                    if (in.operand == -1) effect(1, 1);                       // import: path in, 0 out
                    else { int64_t k = count(in.operand); effect(k + 1, 1); }
                    break;
                case Op::RETURN: need(1); break;                                  // no way on from here
                case Op::ITER_INIT:
                    if (in.operand != 1 && in.operand != 2) fail(ip, "a for loop has one or two variables");
                    need(1); s.depth -= 1; s.iters.push_back((uint8_t)in.operand); next(s); break;
                case Op::ITER_NEXT: {
                    if (s.iters.empty()) fail(ip, "ITER_NEXT with no open for loop");
                    reach(ip, in.operand, s);                                  // exhausted: jumps, pushes nothing
                    s.depth += s.iters.back(); next(s); break;
                }
                case Op::ITER_POP:
                    if (s.iters.empty()) fail(ip, "ITER_POP with no open for loop");
                    s.iters.pop_back(); next(s); break;
                case Op::TRY_PUSH: {
                    State caught = s; caught.depth += 1;                       // the error value; the handler is already gone
                    reach(ip, in.operand, caught);
                    s.tries += 1; next(s); break;
                }
                case Op::TRY_POP:
                    if (s.tries <= 0) fail(ip, "TRY_POP with no open try block");
                    s.tries -= 1; next(s); break;
                // made at run time, never in code that is checked (plainCode() puts the originals back first)
                case Op::F_CMP_LI_JIF: case Op::F_CMP_LL_JIF: case Op::F_CMP_JIF: case Op::F_ARITH_LI: case Op::F_ARITH_LL:
                case Op::F_ARITH_LI_ST: case Op::F_ARITH_LL_ST: case Op::F_ARITH_ST:
                case Op::F_CMP_GI_JIF: case Op::F_CMP_GG_JIF: case Op::F_ARITH_GI: case Op::F_ARITH_GG: case Op::F_ARITH_GI_STG:
                case Op::F_ARITH_STG: case Op::F_IDX_L: case Op::F_IDX_G: case Op::F_IDX: case Op::F_ITER_DEFINE:
                    fail(ip, "a fused op in code that should be plain");
            }
        }
    }
}

// reads a file image back into a program. throws EmbrError (kind "vmbr") if it is damaged or from another build.
// `filename` becomes the sourceFile of every chunk. call registerChunkTeardown(interp, prog) before running it
inline CompiledProgram deserializeProgram(const std::string& bytes, const std::string& filename, VmbrHeader* headerOut = nullptr) {
    if (bytes.size() < 8) raiseError("vmbr", "bad compiled file: too short");
    // the checksum is the last 8 bytes and covers everything before it
    {
        bc_detail::Reader tail(bytes);
        tail.pos = bytes.size() - 8;
        if (tail.u64() != blockHash(bytes.data(), bytes.size() - 8)) raiseError("vmbr", "bad compiled file: checksum does not match");
    }
    VmbrHeader hdr = readVmbrHeader(bytes);
    bc_detail::Reader r(bytes);
    r.pos = 4 + 4 + 4 + 8 + 8;
    r.str();   // the source path and the description, already in hdr
    r.str();
    uint32_t nChunks = r.count(1);
    CompiledProgram prog;
    for (uint32_t ci = 0; ci < nChunks; ++ci) {
        auto cp = std::make_shared<CompiledChunk>();
        CompiledChunk& c = *cp;
        c.sourceFile = filename;
        c.name = r.str();
        uint32_t np = r.count(6);
        for (uint32_t i = 0; i < np; ++i) {
            Param p; p.name = r.str(); p.type = TypeSet(r.u8());
            uint8_t fl = r.u8(); p.optional = fl & 1; p.variadic = fl & 2; p.inout = fl & 4;
            c.params.push_back(std::move(p));
        }
        c.returnType = TypeSet(r.u8());
        c.definedAt.startLine = r.i32(); c.definedAt.startCol = r.i32(); c.definedAt.endLine = r.i32(); c.definedAt.endCol = r.i32();
        c.parentIdx = r.i32();
        c.numLocalSlots = r.i32();
        c.hasBoxed = r.u8() != 0;
        uint32_t ns = r.count(4);
        for (uint32_t i = 0; i < ns; ++i) c.strings.push_back(r.str());
        uint32_t nn = r.count(8);
        for (uint32_t i = 0; i < nn; ++i) c.nums.push_back(r.f64());
        uint32_t ni = r.count(8);
        for (uint32_t i = 0; i < ni; ++i) c.ints.push_back(r.i64());
        uint32_t nc = r.count(20);
        c.code.reserve(nc); c.lines.reserve(nc);
        for (uint32_t i = 0; i < nc; ++i) {
            Instruction in;
            uint8_t op = r.u8();
            if (op >= kOpCount) r.bad("unknown op");
            in.op = (Op)op; in.flags = r.u8(); in.typeHint = TypeSet(r.u8()); r.u8();
            in.operand = r.i32(); in.slot = r.i32(); in.arg = r.i32();
            c.code.push_back(in);
            c.lines.push_back(r.i32());
        }
        uint32_t nl = r.count(8);
        for (uint32_t i = 0; i < nl; ++i) { std::string k = r.str(); c.localSlots.emplace(std::move(k), r.i32()); }
        uint32_t nu = r.count(5);
        for (uint32_t i = 0; i < nu; ++i) { CompiledChunk::UpvalDesc d; d.fromParentLocal = r.u8() != 0; d.index = r.i32(); c.upvalDescs.push_back(d); }
        uint32_t nun = r.count(8);
        for (uint32_t i = 0; i < nun; ++i) { std::string k = r.str(); c.upvalNames.emplace(std::move(k), r.i32()); }
        uint32_t nps = r.count(4);
        for (uint32_t i = 0; i < nps; ++i) {
            uint32_t m = r.count(4);
            std::vector<int> set;
            for (uint32_t j = 0; j < m; ++j) set.push_back(r.i32());
            c.pureSets.push_back(std::move(set));
        }
        prog.chunks.push_back(std::move(cp));
    }
    if (r.pos != bytes.size() - 8) r.bad("extra bytes after the last chunk");
    validateProgram(prog);
    verifyStack(prog);
    // the same shared list the compiler makes (see CompiledChunk::siblings)
    auto siblings = std::make_shared<std::vector<std::shared_ptr<CompiledChunk>>>(prog.chunks);
    for (auto& c : prog.chunks) c->siblings = siblings;
    if (headerOut) *headerOut = hdr;
    return prog;
}

// frees a program you decided not to run. chunks hold each other through `siblings` on purpose (see
// CompiledChunk::siblings), so dropping the program alone would leak them. a program you do run is freed by
// registerChunkTeardown(interp, prog) instead
inline void releaseProgram(CompiledProgram& prog) {
    if (!prog.chunks.empty() && prog.chunks[0]->siblings) prog.chunks[0]->siblings->clear();
}

// files

namespace fs_ = std::filesystem;

// the name a script's cached file gets inside a cache directory: <name>-<hash of its full path>.vmbr, so two
// scripts with the same file name in different folders don't share a file
inline std::string cacheFileName(const std::string& sourcePath) {
    std::error_code ec;
    fs_::path abs = fs_::absolute(sourcePath, ec);
    std::string full = ec ? sourcePath : abs.lexically_normal().string();
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)fnv1a(full.data(), full.size()));
    std::string stem = fs_::path(sourcePath).stem().string();
    if (stem.empty()) stem = "script";
    return stem + "-" + hex + ".vmbr";
}

// writes a program to `path`. goes through a temporary file and a rename, so a reader never sees half a file.
// returns false (and leaves nothing behind) if it can't. creates the folder if needed
inline bool writeCompiledFile(const std::string& path, const CompiledProgram& prog, uint64_t sourceHash, const std::string& sourcePath = "",
                              const std::string& meta = "") {
    std::error_code ec;
    fs_::path p(path);
    if (p.has_parent_path()) fs_::create_directories(p.parent_path(), ec);
    std::string bytes = serializeProgram(prog, sourceHash, sourcePath, meta);
    fs_::path tmp = p; tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f) { f.close(); fs_::remove(tmp, ec); return false; }
    }
    fs_::rename(tmp, p, ec);
    if (ec) { fs_::remove(tmp, ec); return false; }
    return true;
}

inline std::optional<std::string> readWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    if (size < 0) return std::nullopt;
    std::string out((size_t)size, '\0');
    f.seekg(0, std::ios::beg);
    if (size > 0 && !f.read(&out[0], size)) return std::nullopt;
    return out;
}

// the program in `path`, if the file exists, is intact, comes from this build and was made from a script with
// this sourceHash. anything else is a cache miss, so it returns nothing and never throws
inline std::optional<CompiledProgram> readCompiledFile(const std::string& path, uint64_t sourceHash, const std::string& filename) {
    auto bytes = readWholeFile(path);
    if (!bytes) return std::nullopt;
    try {
        if (readVmbrHeader(*bytes).sourceHash != sourceHash) return std::nullopt;   // cheap check first
        return deserializeProgram(*bytes, filename);
    } catch (const EmbrError&) {
        return std::nullopt;
    }
}

// compileSource(), but with a cache folder: a valid cached file is loaded instead of compiling, and a fresh
// compile is written there for next time. an empty cacheDir means no cache at all. a write that fails is ignored
inline CompiledProgram compileSourcePreferCached(const std::string& src, Interpreter& interp,
                                                 const std::string& filename, const std::string& cacheDir,
                                                 const CompileOptions& copts) {
    if (cacheDir.empty()) return compileSource(src, interp, filename, copts);
    const uint64_t h = hashSource(src, filename);
    const std::string path = (fs_::path(cacheDir) / cacheFileName(filename)).string();
    if (auto cached = readCompiledFile(path, h, filename)) {
        interp.setSource(src, filename);          // what compileSource() does besides compiling
        registerChunkTeardown(interp, *cached);
        return std::move(*cached);
    }
    CompiledProgram prog = compileSource(src, interp, filename, copts);
    std::error_code ec;
    std::string abs = fs_::absolute(filename, ec).string();
    writeCompiledFile(path, prog, h, ec ? filename : abs);
    return prog;
}

// removes the .vmbr files in cacheDir that can't be used any more: damaged ones, ones from another build, and
// ones whose script is gone or has changed. removeAll deletes every .vmbr file. returns how many were removed
inline size_t cleanCache(const std::string& cacheDir, bool removeAll = false) {
    std::error_code ec;
    size_t removed = 0;
    if (!fs_::is_directory(cacheDir, ec)) return 0;
    std::vector<fs_::path> files;
    for (auto& e : fs_::directory_iterator(cacheDir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".vmbr") files.push_back(e.path());
    for (auto& p : files) {
        bool drop = removeAll;
        if (!drop) {
            auto bytes = readWholeFile(p.string());
            if (!bytes) { drop = true; }
            else {
                try {
                    VmbrHeader hdr;
                    CompiledProgram prog = deserializeProgram(*bytes, "", &hdr);   // whole file: also catches damage, not just old headers
                    releaseProgram(prog);
                    auto src = readWholeFile(hdr.sourcePath);
                    drop = !src || hashSource(*src, hdr.sourcePath) != hdr.sourceHash;
                } catch (const EmbrError&) { drop = true; }
            }
        }
        if (drop && fs_::remove(p, ec)) ++removed;
    }
    return removed;
}

} // namespace vm
} // namespace embr
#endif // EMBR_WITH_VM
#endif // EMBR_BACKENDS_VM_BYTECODE_H
