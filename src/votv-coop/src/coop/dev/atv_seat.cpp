// coop/dev/atv_seat.cpp -- see coop/dev/atv_seat.h.

#include "atv_seat.h"

#include "coop/interactables/atv_sync.h"
#include "coop/net/net_clock.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"

#include <cstdint>
#include <string>

namespace coop::dev::atv_seat {
namespace {

namespace R = ue_wrap::reflection;

void* g_actionNameFn = nullptr;  // ATV_C::actionName(player, hit, name)

// Which of the seat verb's three gates refused. Two are cheap and side-effect free (the fall
// velocity is a field read; checkEquip is a pure read of the held item), so the arm measures
// them; the third (the seat overlap) is left to elimination, with the occupant pointer as the
// nearest proxy.
void DiagnoseSeatRefusal(void* local, void* atv) {
    void* pcls = R::ClassOf(local);
    float fallZ = 0.f;
    bool haveFall = false;
    if (pcls) {
        const int32_t off = R::FindPropertyOffset(pcls, L"fallVeloc");
        if (off >= 0) {
            const ue_wrap::FVector v =
                *reinterpret_cast<ue_wrap::FVector*>(reinterpret_cast<uint8_t*>(local) + off);
            fallZ = v.Z;
            haveFall = true;
        }
    }

    int emptyHands = -1;              // -1 = could not ask
    std::wstring holdingClass = L"?";
    if (pcls) {
        if (void* fn = R::FindFunction(pcls, L"checkEquip")) {
            ue_wrap::ParamFrame pf(fn);
            if (pf.valid() && ue_wrap::Call(local, pf))
                emptyHands = pf.Get<bool>(L"empty") ? 1 : 0;
        }
        const int32_t hoff = R::FindPropertyOffset(pcls, L"holding_actor");
        if (hoff >= 0) {
            void* held = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(local) + hoff);
            holdingClass = held ? R::ClassNameOf(held) : std::wstring(L"<none>");
        }
    }

    UE_LOGW("[ATVP] ARM sit gates: fallVeloc.Z=%s (need |Z|<800), checkEquip.empty=%s "
            "(need 1), holding_actor=%ls, atv.occupant=%p (need null)",
            haveFall ? std::to_string(fallZ).c_str() : "?",
            emptyHands < 0 ? "?" : (emptyHands ? "1" : "0"),
            holdingClass.c_str(), ue_wrap::atv::GetOccupantPlayer(atv));
}

// ATV_C::actionName, resolved once.
bool ResolveActionName() {
    if (!g_actionNameFn) {
        if (void* cls = R::FindClass(L"ATV_C")) g_actionNameFn = R::FindFunction(cls, L"actionName");
        if (!g_actionNameFn) UE_LOGW("[ATVP] actionName unresolved");
    }
    return g_actionNameFn != nullptr;
}


}  // namespace

bool CallActionName(void* player, void* atv, const wchar_t* verb) {
    if (!ResolveActionName()) return false;
    // The verb dispatches on a string compare, so the frame carries a real FString aliasing this
    // local for the synchronous call; the callee only compares it.
    std::wstring name = verb;
    R::FString fs{ name.data(),
                   static_cast<int32_t>(name.size()) + 1,
                   static_cast<int32_t>(name.size()) + 1 };

    ue_wrap::ParamFrame pf(g_actionNameFn);
    if (!pf.valid()) { UE_LOGE("[ATVP] actionName(%ls): ParamFrame invalid", verb); return false; }
    pf.Set<void*>(L"player", player);          // `hit` stays the frame's zeroed HitResult
    pf.Set<R::FString>(L"name", fs);
    return ue_wrap::Call(atv, pf);
}

bool TrySit(void* player, void* atv, SitTry& t) {
    // The host's grant is the stamp that names this peer's seat.
    if (coop::atv_sync::ActorSeatedBy(coop::players::Registry::Get().LocalPeerId()) == atv) return true;
    if (t.called) {
        // The verb took the seat and is not called again on an occupied one. The grant is awaited as an edge: the
        // local seat lost before it came is the host's refusal, which the lane yielded to.
        if (!ue_wrap::atv::IsDriven(atv) || ue_wrap::atv::GetOccupantPlayer(atv) != player) {
            UE_LOGW("[ATVP] ARM sit: the host refused the seat the verb took on atv=%p -- the lane yielded it", atv);
            return true;
        }
        return false;
    }
    const uint64_t now = coop::net::NowMs();
    if (now < t.nextMs) return false;
    if (t.tries >= kSitRetries) return true;   // the verb has been tried and refused kSitRetries times
    if (!ResolveActionName()) {
        UE_LOGW("[ATVP] ARM sit: actionName unresolved -- arm cannot fire");
        return true;
    }

    const bool ok = CallActionName(player, atv, L"sit");
    ++t.tries;
    t.nextMs = now + kSitRetryMs;

    const bool driven = ue_wrap::atv::IsDriven(atv);
    UE_LOGI("[ATVP] ARM sit: actionName(local=%p, 'sit') on atv=%p -> %s, try %d/%d, driven=%d",
            player, atv, ok ? "called" : "CALL FAILED", t.tries, kSitRetries, driven ? 1 : 0);
    // The verb took the seat for the local player (a seat another occupant drives is a refusal); the lane puts the
    // request to the host's table on its next tick, and the next steps wait for the grant.
    if (driven && ue_wrap::atv::GetOccupantPlayer(atv) == player) {
        t.called = true;
        return false;
    }
    DiagnoseSeatRefusal(player, atv);
    if (t.tries >= kSitRetries) {
        // The three gates are named in the log; the seat overlap cannot be read without a second
        // write into the player.
        UE_LOGW("[ATVP] ARM sit: REFUSED %d times -- the seat verb is gated on "
                "|fallVeloc.Z|<800, empty hands (checkEquip), and an unoccupied playerHit "
                "[disasm @46420/@46522/@46645]; the seat was not taken",
                t.tries);
        return true;
    }
    return false;
}

}  // namespace coop::dev::atv_seat
