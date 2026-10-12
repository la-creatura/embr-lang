#ifndef EMBR_PLUGINS_TABLE_H
#define EMBR_PLUGINS_TABLE_H

// the key representation shared by table.cpp's Table and plugins/parallel's SharedTable, so both use the same key
// equality and hash rules (a plain #include, no runtime link, same idea as embrtypes.h with ffi.cpp)
// the reasoning behind the key rules is in table.cpp's header

#include <embr/embr.h>
#include <unordered_map>
#include <cmath>

namespace embrtable {

using embr::Value;
using embr::raiseError;

enum class KeyKind { Num, Str, Identity };

struct TableKey {
    KeyKind     kind;
    double      num = 0.0;
    std::string str;
    const void* identity = nullptr;

    bool operator==(const TableKey& o) const {
        if (kind != o.kind) return false;
        switch (kind) {
            case KeyKind::Num:      return num == o.num;
            case KeyKind::Str:      return str == o.str;
            case KeyKind::Identity: return identity == o.identity;
        }
        return false;
    }
};

struct TableKeyHash {
    size_t operator()(const TableKey& k) const {
        switch (k.kind) {
            case KeyKind::Num:      return std::hash<double>()(k.num);
            case KeyKind::Str:      return std::hash<std::string>()(k.str) ^ 0x9E3779B97F4A7C15ULL;
            case KeyKind::Identity: return std::hash<const void*>()(k.identity) ^ 0x517CC1B727220A95ULL;
        }
        return 0;
    }
};

// stores the original key Value alongside the stored value so
// *_keys()/*_items() can hand back what the caller actually passed (e.g.
// the int 5, not some normalized double) rather than the internal TableKey
// representation.
using TableData = std::unordered_map<TableKey, std::pair<Value, Value>, TableKeyHash>;

[[noreturn]] inline void throwTableError(const std::string& fn, const std::string& msg) {
    raiseError("[table:" + fn + "]", msg);
}

inline TableKey makeKey(const char* fn, const Value& v) {
    if (v.isNumeric()) {
        double d = v.asNumber();
        if (std::isnan(d)) throwTableError(fn, "table index is NaN");
        return TableKey{KeyKind::Num, d, {}, nullptr};
    }
    if (v.isString())   return TableKey{KeyKind::Str, 0.0, v.asString(), nullptr};
    if (v.isPointer())  return TableKey{KeyKind::Identity, 0.0, {}, v.asPointer().ptr};
    if (v.isCallable()) return TableKey{KeyKind::Identity, 0.0, {}, (const void*)&v.asCallable()};
    throwTableError(fn, "unsupported key type '" + v.typeName() +
                    "' (arrays and maps can't be used as table keys -- see table.cpp's file header)");
}

} // namespace embrtable

#endif // EMBR_PLUGINS_TABLE_H
