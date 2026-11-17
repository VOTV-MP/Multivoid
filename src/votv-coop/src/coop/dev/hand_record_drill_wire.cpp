// coop/dev/hand_record_drill_wire.cpp -- the held item's record drill's can reads and its forged record; see
// coop/dev/hand_record_drill.h and hand_record_drill_detail.h.

#include "hand_record_drill_detail.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace coop::dev::hand_record_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;

int ReadBool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) ? 1 : 0;
}

}  // namespace

Can ReadCan(void* actor) {
    Can c;
    c.opened = ReadBool(actor, L"opened");
    const int32_t off = actor ? R::FindPropertyOffset(R::ClassOf(actor), L"foods") : -1;
    if (off >= 0) c.foods = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(actor) + off);
    return c;
}

std::string Fmt(const Can& c) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "opened=%d foods=%d", c.opened, c.foods);
    return buf;
}

bool ParseCan(const std::string& text, Can& out) {
    const size_t o = text.find("opened=");
    const size_t f = text.find("foods=");
    if (o == std::string::npos || f == std::string::npos) return false;
    out.opened = std::atoi(text.c_str() + o + 7);
    out.foods = std::atoi(text.c_str() + f + 6);
    return true;
}

bool CallOn(void* actor, const wchar_t* fn, void* player) {
    void* const f = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), fn) : nullptr;
    ue_wrap::ParamFrame p(f);
    if (!p.valid()) return false;
    if (player && R::FindParamOffset(f, L"player") >= 0) p.Set<void*>(L"player", player);
    return ue_wrap::Call(actor, p);
}

void* HeldCan() {
    void* const hand = coop::hand_item::LocalHandActor();
    return hand && R::ClassNameOf(hand) == kCanClass ? hand : nullptr;
}

void* HeldBox() {
    void* const hand = coop::hand_item::LocalHandActor();
    if (!hand || R::ClassNameOf(hand) != kBoxClass) return nullptr;
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(hand);
    return key.empty() || key == L"None" ? nullptr : hand;
}

bool SendForged(coop::net::Session& session, Forgery what) {
    void* const hand = HeldCan();
    SR::SaveRecord rec;
    if (!hand || !SR::CaptureRecord(hand, rec)) return false;
    rec.xform = {};
    switch (what) {
    case Forgery::Class:        rec.className = L"prop_fireExt_C"; break;                     // a class this hand does not hold
    case Forgery::UnknownClass: rec.classes.push_back({L"prop_mvDrillBogus_C"}); break;       // a name no class has
    case Forgery::Keyless:      rec.key.clear(); break;
    case Forgery::Oversize:     rec.strings.push_back({std::wstring(4000, L'x')}); break;      // past the 4 KB lane ceiling alone
    case Forgery::Bounds:       rec.strings.push_back({std::wstring(300, L'x')}); break;       // one string past the field bound
    case Forgery::NonFinite:    rec.floats.push_back({std::numeric_limits<float>::quiet_NaN()}); break;
    }
    std::vector<uint8_t> blob;
    W::AppU8(blob, coop::players::Registry::Get().LocalPeerId());
    W::SerSave(blob, rec);
    static uint32_t s_seq = 0x40000000u;
    return coop::blob_chunks::SendBlob(&session, coop::net::ReliableKind::HandRecord, ++s_seq, blob);
}

}  // namespace coop::dev::hand_record_drill::detail
