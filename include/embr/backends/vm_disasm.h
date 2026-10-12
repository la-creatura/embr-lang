// vm_disasm.h
// prints the code the VM compiler made, one instruction per line, for people (and tests) to read.
//
// usage
//   auto prog = embr::vm::compileSource(src, interp, "x.embr");
//   embr::vm::disassemble(prog, std::cout);
//   // or from the shell:  embr dis x.embr
//
// good to know
//   - every op is named in opName() below. a new op that is missing there is a -Wswitch warning, so it can't be forgotten.
//   - a line starting with `>` is the target of a jump, so loops and branches stand out.

#ifndef EMBR_BACKENDS_VM_DISASM_H
#define EMBR_BACKENDS_VM_DISASM_H

#include "vm.h"

#include <iomanip>
#include <ostream>
#include <sstream>
#include <set>

#ifdef EMBR_WITH_VM
namespace embr {
namespace vm {

inline const char* opName(Op op) {
    switch (op) {
        case Op::PUSH_NUM: return "PUSH_NUM";
        case Op::PUSH_INT: return "PUSH_INT";
        case Op::PUSH_STR: return "PUSH_STR";
        case Op::PUSH_NIL: return "PUSH_NIL";
        case Op::POP: return "POP";
        case Op::LOAD: return "LOAD";
        case Op::STORE: return "STORE";
        case Op::DEFINE: return "DEFINE";
        case Op::DEFINE_TYPED: return "DEFINE_TYPED";
        case Op::LOAD_GLOBAL: return "LOAD_GLOBAL";
        case Op::STORE_GLOBAL: return "STORE_GLOBAL";
        case Op::DEFINE_GLOBAL: return "DEFINE_GLOBAL";
        case Op::DEFINE_GLOBAL_TYPED: return "DEFINE_GLOBAL_TYPED";
        case Op::MARK_MODULE_LOCAL: return "MARK_MODULE_LOCAL";
        case Op::APPEND_VAR: return "APPEND_VAR";
        case Op::ADD: return "ADD";
        case Op::SUB: return "SUB";
        case Op::MUL: return "MUL";
        case Op::DIV: return "DIV";
        case Op::MOD: return "MOD";
        case Op::NEG: return "NEG";
        case Op::EQ: return "EQ";
        case Op::NEQ: return "NEQ";
        case Op::LT: return "LT";
        case Op::GT: return "GT";
        case Op::LTE: return "LTE";
        case Op::GTE: return "GTE";
        case Op::NOT: return "NOT";
        case Op::BAND: return "BAND";
        case Op::BOR: return "BOR";
        case Op::BXOR: return "BXOR";
        case Op::SHL: return "SHL";
        case Op::SHR: return "SHR";
        case Op::BNOT: return "BNOT";
        case Op::JUMP: return "JUMP";
        case Op::JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case Op::JUMP_IF_FALSE_PEEK: return "JUMP_IF_FALSE_PEEK";
        case Op::JUMP_IF_TRUE_PEEK: return "JUMP_IF_TRUE_PEEK";
        case Op::BUILD_ARRAY: return "BUILD_ARRAY";
        case Op::BUILD_MAP: return "BUILD_MAP";
        case Op::BUILD_RANGE: return "BUILD_RANGE";
        case Op::INDEX_GET: return "INDEX_GET";
        case Op::INDEX_GET_VAR: return "INDEX_GET_VAR";
        case Op::INDEX_SET: return "INDEX_SET";
        case Op::INDEX_SET_VAR: return "INDEX_SET_VAR";
        case Op::STASH_VAR: return "STASH_VAR";
        case Op::APPEND_ARR: return "APPEND_ARR";
        case Op::REF_PATH: return "REF_PATH";
        case Op::UNPACK: return "UNPACK";
        case Op::ITER_INIT: return "ITER_INIT";
        case Op::ITER_NEXT: return "ITER_NEXT";
        case Op::ITER_POP: return "ITER_POP";
        case Op::MAKE_CLOSURE: return "MAKE_CLOSURE";
        case Op::CALL: return "CALL";
        case Op::RETURN: return "RETURN";
        case Op::YIELD: return "YIELD";
        case Op::NOP: return "NOP";
        case Op::SCOPE_PUSH: return "SCOPE_PUSH";
        case Op::SCOPE_POP: return "SCOPE_POP";
        case Op::TRY_PUSH: return "TRY_PUSH";
        case Op::TRY_POP: return "TRY_POP";
        case Op::F_CMP_LI_JIF: return "F_CMP_LI_JIF";
        case Op::F_CMP_LL_JIF: return "F_CMP_LL_JIF";
        case Op::F_CMP_JIF: return "F_CMP_JIF";
        case Op::F_ARITH_LI: return "F_ARITH_LI";
        case Op::F_ARITH_LL: return "F_ARITH_LL";
        case Op::F_ARITH_LI_ST: return "F_ARITH_LI_ST";
        case Op::F_ARITH_LL_ST: return "F_ARITH_LL_ST";
        case Op::F_ARITH_ST: return "F_ARITH_ST";
        case Op::F_CMP_GI_JIF: return "F_CMP_GI_JIF";
        case Op::F_CMP_GG_JIF: return "F_CMP_GG_JIF";
        case Op::F_ARITH_GI: return "F_ARITH_GI";
        case Op::F_ARITH_GG: return "F_ARITH_GG";
        case Op::F_ARITH_GI_STG: return "F_ARITH_GI_STG";
        case Op::F_ARITH_STG: return "F_ARITH_STG";
        case Op::F_IDX_L: return "F_IDX_L";
        case Op::F_IDX_G: return "F_IDX_G";
        case Op::F_IDX: return "F_IDX";
        case Op::F_ITER_DEFINE: return "F_ITER_DEFINE";
    }
    return "?";
}

// the ops whose `operand` is an instruction index to jump to
inline bool opJumpsByOperand(Op op) {
    return op == Op::JUMP || op == Op::JUMP_IF_FALSE || op == Op::JUMP_IF_FALSE_PEEK ||
           op == Op::JUMP_IF_TRUE_PEEK || op == Op::ITER_NEXT || op == Op::TRY_PUSH;
}
// the ops whose `arg` is an instruction index to jump to
inline bool opJumpsByArg(Op op) {
    return op == Op::APPEND_VAR || op == Op::INDEX_SET_VAR || op == Op::APPEND_ARR;
}

// where a variable lives, as the compiler decided (see Instruction::slot in vm.h)
inline std::string slotText(int slot) {
    if (slot >= 0) return "local " + std::to_string(slot);
    if (slot == kFreeSlot) return "free";
    if (isUpvalSlot(slot)) return "upvalue " + std::to_string(upvalIndex(slot));
    if (isBoxedSlot(slot)) return "boxed " + std::to_string(boxedIndex(slot));
    return "";   // -1: a global, or an op with no variable
}

inline std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else out += c;
    }
    return out + "\"";
}

// the operand of one instruction in words, e.g. `"total"  (local 2)` or `-> 14`
inline std::string operandText(const CompiledProgram& prog, const CompiledChunk& c, const Instruction& in) {
    auto str = [&](int i) { return i >= 0 && i < (int)c.strings.size() ? quoted(c.strings[(size_t)i]) : "<bad string " + std::to_string(i) + ">"; };
    std::ostringstream o;
    switch (in.op) {
        case Op::PUSH_NUM: o << (in.operand >= 0 && in.operand < (int)c.nums.size() ? formatNumber(c.nums[(size_t)in.operand]) : "<bad constant>"); break;
        case Op::PUSH_INT: o << (in.operand >= 0 && in.operand < (int)c.ints.size() ? std::to_string(c.ints[(size_t)in.operand]) : "<bad constant>"); break;
        case Op::PUSH_STR: o << str(in.operand); break;
        case Op::LOAD: case Op::STORE: case Op::DEFINE: case Op::LOAD_GLOBAL: case Op::STORE_GLOBAL:
        case Op::DEFINE_GLOBAL: case Op::MARK_MODULE_LOCAL:
        case Op::APPEND_VAR: case Op::INDEX_GET_VAR: case Op::INDEX_SET_VAR: case Op::STASH_VAR: case Op::APPEND_ARR: case Op::REF_PATH:
            o << str(in.operand); break;
        case Op::DEFINE_TYPED: case Op::DEFINE_GLOBAL_TYPED:
            o << str(in.operand) << " : " << in.typeHint.name(); break;
        case Op::BUILD_ARRAY: case Op::BUILD_MAP: case Op::BUILD_RANGE: case Op::UNPACK: case Op::ITER_INIT:
            o << in.operand; break;
        case Op::CALL:
            if (in.operand < 0) o << "import"; else o << in.operand << (in.operand == 1 ? " arg" : " args");
            break;
        case Op::MAKE_CLOSURE: {
            o << "fn " << in.operand;
            if (in.operand >= 0 && in.operand < (int)prog.chunks.size()) o << " " << quoted(prog.chunks[(size_t)in.operand]->name);
            break;
        }
        default:
            if (opJumpsByOperand(in.op)) o << "-> " << in.operand;
            break;
    }
    // a store that carries its variable's type
    if ((in.op == Op::STORE || in.op == Op::STORE_GLOBAL) && !in.typeHint.isAny()) o << " : " << in.typeHint.name();
    // the second payload and the compile-time slot
    if (opJumpsByArg(in.op)) o << "  -> " << in.arg;
    if (in.op == Op::REF_PATH) o << "  keys " << in.arg;
    if (in.op == Op::STASH_VAR && (in.flags & kPureCalls)) o << "  pure set " << in.arg;
    std::string sl = slotText(in.slot);
    if (!sl.empty()) o << "  (" << sl << ")";
    if (in.flags) {
        o << "  [";
        bool first = true;
        auto flag = [&](int bit, const char* name) { if (in.flags & bit) { if (!first) o << ","; o << name; first = false; } };
        flag(kStash, "stash"); flag(kCallFree, "call-free"); flag(kPureCalls, "pure-calls"); flag(kHasRef, "has-ref"); flag(kAuto, "auto"); flag(kProven, "proven");
        o << "]";
    }
    return o.str();
}

// one chunk: a header line, then one line per instruction
inline void disassembleChunk(const CompiledProgram& prog, size_t index, std::ostream& os) {
    const CompiledChunk& c = *prog.chunks[index];
    std::vector<Instruction> plain;
    const std::vector<Instruction>& code = plainCode(c, plain);   // a program that already ran shows the code the compiler wrote
    os << "== chunk " << index << " " << quoted(c.name);
    if (!c.params.empty()) {
        os << " (";
        for (size_t i = 0; i < c.params.size(); ++i) os << (i ? ", " : "") << c.params[i].name;
        os << ")";
    }
    os << "  " << code.size() << " instructions, " << c.numLocalSlots << " local slots";
    if (c.parentIdx >= 0) os << ", inside chunk " << c.parentIdx;
    os << "\n";
    std::set<int> targets;
    for (const Instruction& in : code) {
        if (opJumpsByOperand(in.op)) targets.insert(in.operand);
        if (opJumpsByArg(in.op)) targets.insert(in.arg);
    }
    int lastLine = -1;
    for (size_t ip = 0; ip < code.size(); ++ip) {
        const Instruction& in = code[ip];
        int line = c.lineAt((int)ip);
        os << (targets.count((int)ip) ? '>' : ' ') << std::setw(5) << ip << "  ";
        if (line != lastLine && line > 0) os << "L" << std::left << std::setw(5) << line << std::right;
        else os << "       ";
        lastLine = line;
        os << " " << std::left << std::setw(20) << opName(in.op) << std::right << " " << operandText(prog, c, in) << "\n";
    }
}

inline void disassemble(const CompiledProgram& prog, std::ostream& os) {
    for (size_t i = 0; i < prog.chunks.size(); ++i) {
        if (i) os << "\n";
        disassembleChunk(prog, i, os);
    }
}

// EMBR_STATS=1: where the hot loop stopped, most often first (see bailStatsOn in vm.h)
inline void printBailStats() {
    std::vector<std::pair<uint64_t, int>> rows;
    uint64_t total = 0;
    for (int op = 0; op < 256; ++op) {
        uint64_t n = bailCounts()[op].load();
        if (n) { rows.push_back({n, op}); total += n; }
    }
    std::sort(rows.rbegin(), rows.rend());
    std::fprintf(stderr, "embr stats: the hot loop stopped %llu times, at\n", (unsigned long long)total);
    for (const auto& [n, op] : rows)
        std::fprintf(stderr, "  %-22s %12llu  %5.1f%%\n", opName((Op)op), (unsigned long long)n, 100.0 * (double)n / (double)total);
}

} // namespace vm
} // namespace embr
#endif // EMBR_WITH_VM
#endif // EMBR_BACKENDS_VM_DISASM_H
