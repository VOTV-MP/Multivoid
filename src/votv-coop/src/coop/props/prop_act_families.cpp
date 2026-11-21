// coop/props/prop_act_families.cpp -- see coop/props/prop_act_families.h.

#include "coop/props/prop_act_families.h"

#include "coop/element/registry.h"

#include "ue_wrap/core/log.h"

namespace coop::prop_act_families {
namespace {

// The table is built on first use, so a registrar of any file runs against a constructed table whatever the order of the
// static initialisers.
struct Table {
    const Family* entries[kMaxFamilies] = {};
    size_t count = 0;
    size_t watchCount = 0;
};
Table& Tab() {
    static Table t;
    return t;
}

}  // namespace

namespace {

// The pointers a family's apply mode dereferences on the host or the client; a null one is a crash on a peer's ask.
bool Complete(const Family& f) {
    if (!f.judge || (f.watchCount != 0 && !f.watches)) return false;
    if (f.apply == Apply::AtGrant) return f.applyAtGrant != nullptr;
    return f.runVerb && f.commitWord && f.applyAtCommit && f.aborted;
}

}  // namespace

Registrar::Registrar(const Family& family) {
    Table& t = Tab();
    if (!Complete(family)) {
        UE_LOGE("prop_act_families: family %u (%s) refused -- a pointer its apply mode needs is null",
                static_cast<unsigned>(family.id), family.name);
        return;
    }
    if (t.count >= kMaxFamilies || Find(family.id)) {
        UE_LOGE("prop_act_families: family %u (%s) refused -- the table is full or the byte is taken",
                static_cast<unsigned>(family.id), family.name);
        return;
    }
    t.entries[t.count++] = &family;
    t.watchCount += family.watchCount;
}

const Family* Find(uint8_t family) {
    const Table& t = Tab();
    for (size_t i = 0; i < t.count; ++i)
        if (t.entries[i]->id == family) return t.entries[i];
    return nullptr;
}

size_t WatchCount() { return Tab().watchCount; }

const WatchDef& WatchAt(size_t i) {
    const Table& t = Tab();
    for (size_t f = 0; f < t.count; ++f) {
        if (i < t.entries[f]->watchCount) return t.entries[f]->watches[i];
        i -= t.entries[f]->watchCount;
    }
    return t.entries[0]->watches[0];   // unreachable for i < WatchCount()
}

void OnRelease(uint8_t slot) {
    const Table& t = Tab();
    for (size_t i = 0; i < t.count; ++i)
        if (t.entries[i]->onRelease) t.entries[i]->onRelease(slot);
}

void OnPeerGone(uint8_t slot) {
    const Table& t = Tab();
    for (size_t i = 0; i < t.count; ++i)
        if (t.entries[i]->onPeerGone) t.entries[i]->onPeerGone(slot);
}

void OnDisconnect() {
    const Table& t = Tab();
    for (size_t i = 0; i < t.count; ++i)
        if (t.entries[i]->onDisconnect) t.entries[i]->onDisconnect();
}

bool SharedTarget(void* actor, coop::element::ElementType type, coop::element::ElementId& eid) {
    namespace EL = coop::element;
    eid = EL::Registry::Get().EidForActor(actor);
    const EL::Element* const el = eid != EL::kInvalidId ? EL::Registry::Get().Get(eid) : nullptr;
    return el && el->IsMirror() && el->GetType() == type;
}

}  // namespace coop::prop_act_families
