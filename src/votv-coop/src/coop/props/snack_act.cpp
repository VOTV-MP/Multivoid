// coop/props/snack_act.cpp -- see coop/props/snack_act.h.

#include "coop/props/snack_act.h"

#include "coop/net/session.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/snack.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"

#include <string>

namespace coop::snack_act {

namespace internal {

bool WellFormedSnackCommit(uint8_t committed, uint8_t flags) {
    if (committed == 1) return flags == kFlagSpent;
    if (committed == kCommitApplied) return (flags & ~kFlagMask) == 0;
    return false;
}

Bounded Bound(int32_t uses, bool open, bool beer, uint8_t flags) {
    Bounded b{uses, open};
    if ((flags & kFlagOpened) && beer && !b.open) b.open = true;
    if ((flags & kFlagSpent) && (b.open || !beer) && b.uses >= 1) --b.uses;
    return b;
}

}  // namespace internal

CopyState ReadCopy(void* actor) {
    CopyState s;
    s.read = ue_wrap::snack::ReadSnackState(actor, s.uses, s.open);
    return s;
}

CommitWords CommitFor(bool ran, bool destroyed, const CopyState& before, const CopyState& after) {
    if (destroyed) return {1, kFlagSpent};
    if (!ran || !before.read || !after.read) return {0, 0};
    uint8_t flags = 0;
    if (after.uses < before.uses) flags |= kFlagSpent;
    if (!before.open && after.open) flags |= kFlagOpened;
    return {kCommitApplied, flags};
}

namespace {

// A snack with no use left is gone on every peer.
void EndOnEveryPeer(coop::net::Session* s, void* actor, const coop::net::PropActCommitPayload& p, uint8_t senderSlot) {
    ue_wrap::CachedObjRef before;
    before.Set(actor);
    coop::net::PropDestroyPayload dp{};
    dp.elementId = p.eid;
    coop::remote_prop::DestroyJudged(dp, senderSlot);
    if (before.Is(actor)) {
        UE_LOGE("snack_act: the host could not destroy eid %u for slot %u -- the snack stands", static_cast<unsigned>(p.eid),
                static_cast<unsigned>(senderSlot));
        return;
    }
    // A requester whose copy ended already has nothing to destroy; one whose copy lived has it destroyed here.
    s->SendReliable(coop::net::ReliableKind::PropDestroy, &dp, sizeof(dp), p.committed == 1 ? senderSlot : -1);
}

}  // namespace

void OnCommit(coop::net::Session* s, void* actor, const coop::net::PropActCommitPayload& p, uint8_t senderSlot) {
    int32_t uses = 0;
    bool open = false;
    if (!s || !ue_wrap::snack::ReadSnackState(actor, uses, open)) {
        UE_LOGW("snack_act: slot %u eid %u -- the host copy's state is unreadable, nothing applied",
                static_cast<unsigned>(senderSlot), static_cast<unsigned>(p.eid));
        return;
    }
    const internal::Bounded b = internal::Bound(uses, open, ue_wrap::snack::IsBeerFamily(actor), p.flags);
    UE_LOGI("snack_act: slot %u eid %u committed %u flags %u: host copy uses %d open %d -> uses %d open %d",
            static_cast<unsigned>(senderSlot), static_cast<unsigned>(p.eid), static_cast<unsigned>(p.committed),
            static_cast<unsigned>(p.flags), uses, open ? 1 : 0, b.uses, b.open ? 1 : 0);
    if (b.uses <= 0) {
        EndOnEveryPeer(s, actor, p, senderSlot);
        return;
    }
    if ((b.uses != uses || b.open != open) && !ue_wrap::snack::PatchSnackRecord(actor, b.uses, b.open))
        UE_LOGW("snack_act: eid %u -- the host copy's record could not be patched", static_cast<unsigned>(p.eid));
    // The requester's replayed copy, and every other peer's, is put back to the host's state, a refused delta included.
    coop::prop_save_data::Publish(s, actor, ue_wrap::prop::GetInteractableKeyString(actor));
}

}  // namespace coop::snack_act
