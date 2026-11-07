// coop/props/toolgun_sight.h -- could a client have struck a prop with the tool gun, from where the host holds its
// eye? The one judge of the host's toolgun lanes (the remover, the prop-properties tool): the claimed origin must lie
// within the pose-staleness ball around the eye, the hit point on the prop, and the segment from the eye to the hit
// point clear of what the shot itself would stop at. Source takes a shot's origin from the server's own eye and never
// from the client (reference/source-sdk-2013/src/game/shared/baseplayer_shared.cpp:817 and
// reference/source-sdk-2013/src/game/shared/basecombatweapon_shared.cpp:2313); MTA bounds the claimed origin to its
// copy of the player and traces nothing
// (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CBulletsyncPacket.cpp:167-191). This judge does both,
// because the host has the world. Game thread.

#pragma once

#include "coop/net/refusal_tally.h"

#include "ue_wrap/core/types.h"

namespace coop::toolgun_sight {

// What the judge found. `ok` is the sight standing. When it is not, `judged` says whether the host could tell: false
// is a bound or a trace that did not read or run (the host failing the sender), true is a sight that does not stand.
// `why` is the branch, a static string for the lane's tally. `blocked` is a segment that met a blocker short of the
// hit point, with the blocker and the distances (from the eye to the blocker and to the hit point, and from the
// claimed origin to the eye) for the lane's diagnostic line.
struct SightResult {
    bool        ok = false;
    bool        judged = true;
    const char* why = "";
    bool        blocked = false;
    void*       blockerActor = nullptr;
    float       dEyeBlock = 0.f;
    float       dEyeHit = 0.f;
    float       dOriginEye = 0.f;
};

// `puppet` is the sender's posed body (the trace's world context), `actor` the prop, `origin` and `hit` the
// sender's claim in world centimetres, `eye` the sender's eye as the host held it when the request arrived.
SightResult JudgeToolgunSight(void* puppet, void* actor, const ue_wrap::FVector& origin, const ue_wrap::FVector& hit,
                              const ue_wrap::FVector& eye);

// Says a blocked segment, once per refusal-tally window: the blocker's class and how far it stood from the eye, how far
// the hit point was and how far the claimed origin lay from the eye. A diagnostic for an honest shot refused for sight,
// bounded like the tally's lines so a sender cannot make the host print per message. `last` is the lane's own latch
// and `lane` its log prefix. Game thread.
void NoteBlocked(const char* lane, coop::net::RefusalTally::Clock::time_point& last, const SightResult& s);

}  // namespace coop::toolgun_sight
