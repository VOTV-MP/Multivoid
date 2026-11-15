// coop/interactables/floppy_slot_wire.cpp -- see coop/interactables/floppy_slot_wire.h.

#include "coop/interactables/floppy_slot_wire.h"

namespace coop::floppy_slot_wire {
namespace {

// A disc actor's stream is a few kilobytes; the slot reads and writes no more than this either (ue_wrap/devices/floppy_slot).
constexpr size_t kMaxObjectBytes = 256 * 1024;

// A row or a JSON can be long, so both take a 32-bit length; the reader bounds them against the
// blob it is reading out of.
void PutStr32(std::vector<uint8_t>& b, const std::wstring& w) {
    const std::string u = coop::text::ToUtf8Exact(w);
    PutU32(b, static_cast<uint32_t>(u.size()));
    b.insert(b.end(), u.begin(), u.end());
}

}  // namespace

bool SameDiscIdentity(const Slot& a, const Slot& b, const std::wstring& keyA, const std::wstring& keyB) {
    if (!keyA.empty() || !keyB.empty()) return keyA == keyB;
    return a.c.objectBytes == b.c.objectBytes;
}

void PackSlot(std::vector<uint8_t>& b, const Slot& s) {
    PutU32(b, static_cast<uint32_t>(s.st.floppyType));
    if (s.st.floppyType < 0) return;
    PutU32(b, static_cast<uint32_t>(s.st.readWrites));
    b.push_back(s.st.zip ? 1 : 0);
    PutStr32(b, s.c.nametype);
    PutStr32(b, s.c.objectData);
    PutU32(b, static_cast<uint32_t>(s.c.objectBytes.size()));
    b.insert(b.end(), s.c.objectBytes.begin(), s.c.objectBytes.end());
    // The count and the rows that follow it are the same number or the reader desyncs, so the
    // clamp binds both.
    const size_t rows = s.c.data.size() < 0xFFFF ? s.c.data.size() : 0xFFFF;
    PutU16(b, static_cast<uint16_t>(rows));
    for (size_t i = 0; i < rows; ++i) PutStr32(b, s.c.data[i]);
}

bool ParseSlot(Reader& r, Slot& out) {
    out.st.floppyType = static_cast<int32_t>(r.U32());
    if (!r.ok) return false;
    if (out.st.floppyType < 0) return true;  // empty: the type is the whole body
    out.st.readWrites = static_cast<int32_t>(r.U32());
    out.st.zip        = r.U8() != 0;
    out.c.nametype    = r.Str32();
    out.c.objectData  = r.Str32();
    {
        const uint32_t n = r.U32();
        if (!r.ok || n > r.n - r.off || n > kMaxObjectBytes) { r.ok = false; return false; }
        out.c.objectBytes.assign(r.p + r.off, r.p + r.off + n);
        r.off += n;
    }
    const uint16_t rows = r.U16();
    for (uint16_t i = 0; i < rows && r.ok; ++i) out.c.data.push_back(r.Str32());
    return r.ok;
}

}  // namespace coop::floppy_slot_wire
