// vmbr_meta.h
// the few facts about a `.vmbr` file that tools need without loading the VM: the format number, the checksum, and the
// description a compiled module carries (see core/modinfo.h). the rest of the format is in backends/vm_bytecode.h.
//
// good to know
//   - the description sits right after the source path, before the code, so it can be read without walking the chunks
//   - numbers are little endian, the same as the rest of the file

#ifndef EMBR_CORE_VMBR_META_H
#define EMBR_CORE_VMBR_META_H

#include <cstdint>
#include <cstring>
#include <string>

namespace embr {
namespace vmbr {

// bump when the file layout or the meaning of an instruction changes in a way the op names can't show
constexpr uint32_t kFormatVersion = 3;
constexpr char     kMagic[4] = {'V', 'M', 'B', 'R'};

// eight bytes at a time, about eight times faster on a big file than a byte loop. used for the file checksum and the
// source hash. a changed byte always changes the result (each step is a bijection)
inline uint64_t blockHash(const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = 1469598103934665603ULL;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ULL;
        h ^= h >> 29;   // an xor-multiply alone leaves the top bits of a word barely mixed in
    }
    for (; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h ^ (uint64_t)n;
}

inline uint32_t readU32(const std::string& s, size_t pos) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)(uint8_t)s[pos + i] << (8 * i);
    return v;
}

// the description inside a file image. false if it is not an intact .vmbr of this format version. `meta` is empty when the file
// has none (a script compiled by a program that does not write one)
inline bool readMeta(const std::string& bytes, std::string& meta) {
    meta.clear();
    if (bytes.size() < 4 + 4 + 4 + 8 + 8 + 4 + 4 + 8 || std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    if (readU32(bytes, 4) != kFormatVersion) return false;
    uint64_t sum = 0;
    for (int i = 0; i < 8; ++i) sum |= (uint64_t)(uint8_t)bytes[bytes.size() - 8 + i] << (8 * i);
    if (sum != blockHash(bytes.data(), bytes.size() - 8)) return false;
    size_t pos = 4 + 4 + 4 + 8 + 8;                      // magic, version, op count, op fingerprint, source hash
    uint64_t n = readU32(bytes, pos);                    // the source path
    pos += 4 + n;
    if (pos + 4 > bytes.size() - 8) return false;
    n = readU32(bytes, pos);
    pos += 4;
    if (n > bytes.size() - 8 - pos) return false;
    meta = bytes.substr(pos, (size_t)n);
    return true;
}

} // namespace vmbr
} // namespace embr

#endif // EMBR_CORE_VMBR_META_H
