// coop/interactables/floppy_slot_wire.h -- the BYTES of a floppy slot's claim and canonical blob: the head, a slot entry
// and its reader. Pure byte work; what a slot change MEANS belongs to floppy_slot_sync.

#pragma once

#include "coop/text/utf8_codec.h"
#include "ue_wrap/devices/floppy_slot.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coop::floppy_slot_wire {

// head [u8 op][u8 deviceKind]; op 0=claim{[u8 index][slot]}, 1=canonical{[u32 generation][u16 n]{[u8 index][slot]}}
// The generation counts the host's changes of the laptop's occupancy (0 for the boxes): the laptop's file quad carries
// it too, so the two lanes agree which disc an edit belongs to.
// slot [i32 type][i32 rw][u8 zip][u32 nametypeLen][utf8][u32 jsonLen][utf8][u32 bytesLen][bytes][u16 rows]{[u32 len][utf8]}
// bytes is the 0.10 disc's ActorToBytes stream (floppyObjectDataBytes): the slot's disc is that, not the JSON of 0.9.
// nametype is empty for every class but the laptop's, and it is on the wire because the format
// claims to leave room for that device: a member the digest raises an edge on and the wire drops
// is a field the receiver would blank on every change.

constexpr uint8_t kOpClaim     = 0;
constexpr uint8_t kOpCanonical = 1;

inline void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
inline void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

struct Reader {
    const uint8_t* p; size_t n; size_t off = 0;
    bool ok = true;
    uint8_t U8() { if (off + 1 > n) { ok = false; return 0; } return p[off++]; }
    uint16_t U16() { if (off + 2 > n) { ok = false; return 0; } uint16_t v = static_cast<uint16_t>(p[off] | (p[off + 1] << 8)); off += 2; return v; }
    uint32_t U32() { if (off + 4 > n) { ok = false; return 0; } uint32_t v = 0; std::memcpy(&v, p + off, 4); off += 4; return v; }
    std::wstring Str32() {
        const uint32_t len = U32();
        if (!ok || len > n - off) { ok = false; return std::wstring(); }
        std::wstring w;
        const bool wellFormed = coop::text::FromUtf8Strict(reinterpret_cast<const char*>(p + off), len, &w);
        off += len;
        if (!wellFormed) { ok = false; return std::wstring(); }
        return w;
    }
};

struct Slot {
    ue_wrap::floppy_slot::Scalars st;
    ue_wrap::floppy_slot::Content c;
};

// Whether two occupied slots hold one disc: by the save key where the JSON carries one (0.9), else by the ActorToBytes
// stream (0.10, where the JSON is empty and the stream is the disc).
bool SameDiscIdentity(const Slot& a, const Slot& b, const std::wstring& keyA, const std::wstring& keyB);

// An empty slot is a type and nothing else. What the device still holds in its other fields is residue its own eject
// left behind, so carrying it would put a stale disc's JSON on the wire for every empty box in the connect set.
void PackSlot(std::vector<uint8_t>& b, const Slot& s);

// One slot entry read out of `r`; false when the blob ends early or a field breaks its bound (r.ok says so too).
bool ParseSlot(Reader& r, Slot& out);

}  // namespace coop::floppy_slot_wire
