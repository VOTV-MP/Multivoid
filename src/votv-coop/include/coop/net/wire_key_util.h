// coop/net/wire_key_util.h -- shared WireKey <-> wstring converters + the FNV-1a
// key hash used by the keyed-sync modules (interactable_sync / keypad_sync /
// window_sync). Extracted 2026-06-08 (RULE 2: these byte-identical pure helpers had
// been copy-pasted into each keyed-sync TU; window_sync would have been the third
// copy). Header-only inline (ODR-safe across TUs); no engine/network state.
//
// VOTV instance Keys (Aactor_save_C::Key / AtriggerBase_C::Key / Aprop_C::Key) are
// ASCII FNames; the wire carries them as a fixed 32-byte WireKey (len + 31 chars).
// Bytes beyond `len` are zeroed so equality compares hold.

#pragma once

#include "coop/net/protocol.h"  // WireKey

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::net {

inline void WireKeyFromString(const std::wstring& key, WireKey& out) {
    std::memset(&out, 0, sizeof(out));
    size_t n = key.size();
    if (n > sizeof(out.data)) n = sizeof(out.data);  // 31
    for (size_t i = 0; i < n; ++i) out.data[i] = static_cast<char>(key[i] & 0xFF);
    out.len = static_cast<uint8_t>(n);
}

inline std::wstring StringFromWireKey(const WireKey& k) {
    size_t n = k.len;
    if (n > sizeof(k.data)) n = sizeof(k.data);
    std::wstring out; out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])));
    return out;
}

// Order-independent FNV-1a over the low byte of each wchar -- the keyed-sync index
// stability hash (compare host vs client logs to confirm cross-peer Key stability).
inline uint64_t FnvKey(const std::wstring& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (wchar_t c : s) { h ^= static_cast<uint8_t>(c & 0xFF); h *= 0x100000001b3ULL; }
    return h;
}

}  // namespace coop::net
