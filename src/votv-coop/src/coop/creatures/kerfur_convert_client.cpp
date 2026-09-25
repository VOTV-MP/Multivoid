// coop/creatures/kerfur_convert_client.cpp -- the client half of the kerfur conversion: the
// KerfurConvert wire apply. Interface: coop/creatures/kerfur_convert_client.h; the feature narrative
// is in kerfur_convert.h. The session arrives from the kerfur_convert install.

#include "coop/creatures/kerfur_convert_client.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/kerfur_entity.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::kerfur_convert_client {
namespace {

namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

coop::net::Session* LoadSession() {
    return g_session.load(std::memory_order_acquire);
}

// Materialise a kerfur mirror of the new form at host-range `eid`. NPC: a fresh NPC mirror. Prop: the
// PropSpawn receiver, with a synthetic key derived from the eid.
void MaterializeKerfurMirror(bool toNpc, coop::element::ElementId eid, const std::wstring& classW,
                             float lx, float ly, float lz, float rp, float ry, float rr,
                             void* localPlayer) {
    if (toNpc) {
        void* actorClass = R::FindClass(classW.c_str());
        if (!actorClass) {
            UE_LOGW("kerfur_convert[client]: cannot resolve class '%ls' for NPC materialize eid=%u",
                    classW.c_str(), eid);
            return;
        }
        coop::npc_mirror::SpawnFreshNpcMirror(classW, actorClass, static_cast<uint32_t>(eid),
                                              lx, ly, lz, rp, ry, rr);
        return;
    }
    const std::string key8 = "coopkerfur#" + std::to_string(static_cast<unsigned>(eid));
    coop::net::PropSpawnPayload sp{};
    sp.className.len = 0;
    for (size_t i = 0; i < classW.size() && i < 63; ++i)
        sp.className.data[sp.className.len++] = static_cast<char>(classW[i]);
    sp.key.len = 0;
    for (size_t i = 0; i < key8.size() && i < 31; ++i) sp.key.data[sp.key.len++] = key8[i];
    sp.propName.len = 0;
    sp.locX = lx; sp.locY = ly; sp.locZ = lz;
    sp.rotPitch = rp; sp.rotYaw = ry; sp.rotRoll = rr;
    sp.scaleX = sp.scaleY = sp.scaleZ = 1.f;
    sp.physFlags = 0;
    sp.chipType = 0;
    sp.elementId = static_cast<uint32_t>(eid);
    // Both flags are passed explicitly. deferKerfur is false: the polled class-and-pose adopter is for
    // a save-loaded kerfur prop still loading beside its host copy, and a conversion's new form has no
    // local twin to wait for, so it materialises now.
    coop::remote_prop_spawn::OnSpawn(sp, /*senderSlot=*/0, localPlayer,
                                     /*fromConvert=*/false, /*deferKerfur=*/false);
}

}  // namespace

void OnKerfurConvert(const coop::net::KerfurConvertBroadcastPayload& p, void* localPlayer) {
    auto* s = LoadSession();
    if (!s) return;
    // Client-only: the host converged inline (a silent register and the send), so it never applies
    // its own broadcast.
    if (s->role() != coop::net::Role::Client) return;
    const coop::element::ElementId oldEid = static_cast<coop::element::ElementId>(p.oldEid);
    const coop::element::ElementId newEid = static_cast<coop::element::ElementId>(p.newEid);
    const bool toNpc = (p.toForm == 0);  // toForm: 0 = NPC (turn-on), 1 = prop (turn_off)
    std::wstring classW;
    classW.reserve(p.newClassName.len);
    for (uint8_t i = 0; i < p.newClassName.len && i < 63; ++i)
        classW.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.newClassName.data[i])));

    // Destroy the old-form mirror at oldEid, then materialise the new form at the authoritative newEid.
    if (!toNpc) {
        // The new form is a prop, so the old form was the NPC.
        coop::net::EntityDestroyPayload dp{};
        dp.elementId = static_cast<uint32_t>(oldEid);
        coop::npc_mirror::OnEntityDestroy(dp);
    } else {
        // The new form is the NPC, so the old form was a prop: an eid-only teardown of the old prop
        // mirror. Its actor is evicted from the client held-pose map first, before the destroy
        // frees it; the map self-heals on read too.
        if (auto* oldEl = coop::element::Registry::Get().Get(oldEid))
            coop::kerfur_entity::ForgetKerfurPropMirror(oldEl->GetActor());
        coop::net::PropDestroyPayload dp{};
        dp.key.len = 0;
        dp.elementId = static_cast<uint32_t>(oldEid);
        coop::remote_prop::OnDestroy(dp, localPlayer);
    }
    MaterializeKerfurMirror(toNpc, newEid, classW, p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll,
                            localPlayer);
    // The old-form retire for a join-window turn-on is not armed here: that broadcast cannot reach
    // a joiner mid save-transfer, so the NPC EntitySpawn receiver arms it and the quiescence sweep
    // runs it. A steady-state turn-on retired the old form through the destroy above.
    UE_LOGI("kerfur_convert[client]: applied KerfurConvert K=%u %s oldEid=%u -> newEid=%u class='%ls'",
            p.kerfurId, toNpc ? "->NPC(turn-on)" : "->prop(turn_off)", p.oldEid, p.newEid, classW.c_str());
}

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

}  // namespace coop::kerfur_convert_client
