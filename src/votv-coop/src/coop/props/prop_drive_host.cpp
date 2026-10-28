// coop/props/prop_drive_host.cpp -- see coop/props/prop_drive_host.h.

#include "coop/props/prop_drive_host.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"            // IsHandAxisActor: the hotbar hand and its mirrors
#include "coop/player/local_streams.h"        // LastHeldActor: this player's grab slot
#include "coop/props/active_drive.h"          // NowMs
#include "coop/props/prop_drive_rules.h"      // the epsilons and the rest rule both authors share
#include "coop/props/prop_drive_stream.h"     // Pump: the apply tick that follows the publish
#include "coop/props/prop_element_tracker.h"  // GetPropElementIdForActor
#include "coop/props/prop_lifecycle.h"        // IsWireSuppressedPropClass: a class no peer holds
#include "coop/props/prop_syncer.h"           // Pump: the role and a client's rows; OnDisconnect
#include "coop/props/prop_syncer_host.h"      // Pump: admit a client's rows; OnDisconnect
#include "coop/props/prop_wire_parity.h"      // PhysFlagsOf: the flags the end edge carries
#include "coop/props/remote_prop.h"           // IsActorUnderAnyDrive: a peer's held-prop stream
#include "prop_drive_rows.h"                  // co-located private header (src tree, not include/)
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::prop_drive_host {
namespace {

namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

using coop::prop_drive_rules::kRestMs;
using coop::prop_drive_rules::kRestProbeMs;
using coop::prop_drive_rules::kRestVelCmS;

std::vector<Driven> g_driven;   // game thread only
size_t              g_turn = 0; // where the next tick's publishing starts, so no prop waits forever
// The claim generation is PER PROP: the receiver's closed-generation gate is keyed by eid and
// compares in a signed 8-bit window, so a counter shared by every prop would read a re-claim as
// older than the closed one after enough claims of other props in between. Outlives the row.
std::unordered_map<uint32_t, uint8_t> g_genByEid;

Driven* Find(void* actor) {
    for (Driven& d : g_driven)
        if (!d.dead && d.ref.Raw() == actor) return &d;
    return nullptr;
}

bool Moved(const Driven& d, const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    if (!d.everSent || d.resend) return true;
    return coop::prop_drive_rules::MovedPastEps(loc, rot, d.sentLoc, d.sentRot);
}

void SendEnd(coop::net::Session& s, const Driven& d, void* actor, const ue_wrap::FVector& loc,
             const ue_wrap::FRotator& rot, const char* why) {
    float lin = 0.f;
    const coop::net::PropDriveEndPayload p = EndFromBody(actor, d.eid, d.gen, loc, rot, &lin);
    s.SendReliable(coop::net::ReliableKind::PropDriveEnd, &p, sizeof(p));
    UE_LOGI("[PROP-DRIVE] HOST END eid=%u gen=%u %s -> (%.1f,%.1f,%.1f) |v|=%.1f cm/s flags=0x%02x",
            d.eid, static_cast<unsigned>(d.gen), why, loc.X, loc.Y, loc.Z, lin,
            static_cast<unsigned>(p.physFlags));
}

// A client row's turn: the last pose admitted from its syncer goes out at the row's generation when it is new
// (or a joiner needs it again), as a host-authored row's would; the host's copy is never read. 1 when the pose
// joined the queue (the tick's rotation counts those), else 0.
size_t PublishAdmitted(coop::net::Session& s, Driven& d) {
    if (!d.admittedEver || !(d.freshAdmit || d.resend)) return 0;
    // The developer red arm of the prop syncer drill: an admitted row is applied to the host's copy (at admission)
    // and never re-published, so the drill's observer must fail. The End still goes. Resolved once.
    static const bool s_redNoRelay =
        coop::config::ResolveEnum(coop::config_registry::rows::prop_syncer_red) == "no_relay";
    if (s_redNoRelay) {
        d.freshAdmit = false;
        d.resend     = false;
        return 0;
    }
    const coop::net::PoseTurn turn = s.PropDrivePoseTurn(d.eid);
    if (turn == coop::net::PoseTurn::Wait) return 0;   // the pose waiting in the queue is refreshed on a later tick
    coop::net::PropPoseSnapshot pp{};
    pp.key       = d.key;
    pp.elementId = d.eid;
    pp.ctx       = d.gen;
    pp.x = d.admitLoc.X; pp.y = d.admitLoc.Y; pp.z = d.admitLoc.Z;
    pp.pitch = ue_wrap::NormalizeAxis(d.admitRot.Pitch);
    pp.yaw   = ue_wrap::NormalizeAxis(d.admitRot.Yaw);
    pp.roll  = ue_wrap::NormalizeAxis(d.admitRot.Roll);
    // A broadcast, the syncer included: MTA relays the accepted sync to every joined player but its source
    // (CUnoccupiedVehicleSync.cpp:472). The pose batch is one shared datagram per tick, so the source is not left out
    // here; its client skips the echo (prop_syncer::SkipsHostPose). The End is sent to the others alone.
    s.PublishPropDrivePose(pp);
    d.freshAdmit = false;
    d.resend     = false;
    return turn == coop::net::PoseTurn::Join ? 1 : 0;
}

// A new row for `actor` when the channel can carry it: a keyed prop with a host element id, in no
// hand, of a class peers hold. Null otherwise. `claimed` false opens it coasting. `syncer` 0 is a host
// verb's row, n a client slot's grant.
Driven* Open(void* actor, bool claimed, const char* why, uint8_t syncer = 0) {
    if (!PR::IsDescendantOfProp(actor)) return nullptr;
    if (InAHand(actor)) return nullptr;   // the held-prop lane owns a prop in a hand
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(actor);
    if (eid == coop::element::kInvalidId || eid == 0u) {
        // A feeder asks again every pass while the tie holds, so the line is per actor, not per pass.
        static void* sRefusedSaid = nullptr;
        if (actor != sRefusedSaid) {
            sRefusedSaid = actor;
            // A class the wire never expresses has no copy on any peer to move, so its motion is
            // parity and its refusal no news.
            if (!coop::prop_lifecycle::IsWireSuppressedPropClass(R::ClassNameOf(actor)))
                UE_LOGW("[PROP-DRIVE] HOST refused: actor %p has no host element id (%s)", actor, why);
        }
        return nullptr;
    }
    if (coop::prop_lifecycle::IsWireSuppressedPropClass(R::ClassNameOf(actor))) return nullptr;
    Driven d;
    d.ref.Set(actor);
    d.eid = static_cast<uint32_t>(eid);
    d.claimed = claimed;
    d.syncer = syncer;
    const std::wstring keyW = PR::GetInteractableKeyString(actor);
    d.key.len = 0;
    for (size_t i = 0; i < keyW.size() && i < sizeof(d.key.data); ++i)
        d.key.data[d.key.len++] = static_cast<char>(keyW[i]);
    d.gen = BumpGen(d.eid);
    d.lastMoveMs = coop::active_drive::NowMs();
    g_driven.push_back(std::move(d));
    UE_LOGI("[PROP-DRIVE] HOST %s eid=%u gen=%u key='%ls' (%s) -- %zu driven",
            syncer ? "GRANT" : (claimed ? "CLAIM" : "COAST"), g_driven.back().eid,
            static_cast<unsigned>(g_driven.back().gen), keyW.c_str(), why, g_driven.size());
    return &g_driven.back();
}

}  // namespace

Driven* FindByEid(uint32_t eid) {
    for (Driven& d : g_driven)
        if (!d.dead && d.eid == eid) return &d;
    return nullptr;
}

uint8_t BumpGen(uint32_t eid) {
    uint8_t& gen = g_genByEid[eid];
    if (++gen == 0) ++gen;   // 0 is the stream's "no generation"
    return gen;
}

coop::net::PropDriveEndPayload EndFromBody(void* actor, uint32_t eid, uint8_t gen, const ue_wrap::FVector& loc,
                                           const ue_wrap::FRotator& rot, float* speedCmS) {
    coop::net::PropDriveEndPayload p{};
    p.eid = eid;
    p.gen = gen;
    // This peer's own flags at this instant: the receiver restores the same parity the join's
    // converge applies, rather than guessing that a simulating prop is what it took off.
    p.physFlags = coop::prop_wire_parity::PhysFlagsOf(actor);
    p.x = loc.X; p.y = loc.Y; p.z = loc.Z;
    p.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
    p.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    p.roll  = ue_wrap::NormalizeAxis(rot.Roll);
    // The body's velocity at this instant. At rest it is noise below kRestVelCmS and goes out as
    // zero, so no receiver wakes a resting body by writing a velocity into it. MTA's syncer
    // (CUnoccupiedVehicleSync.cpp, bSyncVelocity) streams a velocity in every packet it moved in;
    // ours rides the end edge only, since the receiver interpolates a parked body and has no use
    // for one before that.
    const PR::VelocityState v = PR::GetPhysicsVelocity(actor);
    const float lin = std::sqrt(v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                                v.linearCmS.Z * v.linearCmS.Z);
    if (v.ok && lin >= kRestVelCmS) {
        p.linVelX = v.linearCmS.X;   p.linVelY = v.linearCmS.Y;   p.linVelZ = v.linearCmS.Z;
        p.angVelX = v.angularDegS.X; p.angVelY = v.angularDegS.Y; p.angVelZ = v.angularDegS.Z;
    }
    if (speedCmS) *speedCmS = v.ok ? lin : 0.f;
    return p;
}

Driven* OpenClientRow(void* actor, uint8_t slot, const char* why) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::OpenClientRow");
    if (!actor || slot == 0) return nullptr;
    Driven* d = Open(actor, /*claimed=*/true, why ? why : "", slot);
    if (d) d->admitMs = d->lastMoveMs;   // the silence clock starts at the grant
    return d;
}

void EraseRow(Driven& row) {
    row.dead = true;   // Tick's sweep erases it; every reader skips a dead row
}

std::vector<Driven>& Rows() {
    return g_driven;
}

bool InAHand(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::InAHand");
    return actor == coop::local_streams::LastHeldActor() ||
           coop::remote_prop::IsActorUnderAnyDrive(actor) ||
           coop::hand_item::IsHandAxisActor(actor);
}

void Claim(void* actor, const char* reason) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Claim");
    if (!actor) return;
    const char* why = reason ? reason : "";
    if (Driven* d = Find(actor)) {
        // A host verb on a prop a client syncs: the verb wins and the row becomes the host's (MTA's persistent
        // OverrideSyncer, CUnoccupiedVehicleSync.cpp:59-82, .h:37-38).
        if (d->syncer != 0) coop::prop_syncer_host::EndClientRow(*d, coop::prop_syncer_host::Ending::HostVerb);
        if (!d->claimed) {
            d->claimed     = true;   // a re-claim while coasting: the stream simply continues
            d->nextProbeMs = 0;
            UE_LOGI("[PROP-DRIVE] HOST re-claim eid=%u (%s)", d->eid, why);
        }
        return;
    }
    Open(actor, /*claimed=*/true, why);
}

void Coast(void* actor, const char* reason) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Coast");
    if (!actor) return;
    if (Driven* d = Find(actor)) {
        // A push on a prop a client syncs: the row becomes the host's, coasting from the client's last pose (the
        // same override as Claim's).
        if (d->syncer != 0) {
            coop::prop_syncer_host::EndClientRow(*d, coop::prop_syncer_host::Ending::HostVerb);
            d->claimed = false;
        }
        // A claimed prop stays its verb's; either way the push starts its rest clock again and it is
        // read every tick.
        d->nextProbeMs = 0;
        d->lastMoveMs = coop::active_drive::NowMs();
        return;
    }
    Open(actor, /*claimed=*/false, reason ? reason : "");
}

void OverrideClientSyncer(void* actor, const char* reason) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::OverrideClientSyncer");
    const Driven* d = actor ? Find(actor) : nullptr;
    if (d && d->syncer != 0) Coast(actor, reason);
}

size_t CoastingCount() {
    size_t n = 0;
    for (const Driven& d : g_driven)
        if (!d.claimed && !d.dead) ++n;
    return n;
}

DriveContext ContextOf(void* actor, uint32_t eid) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::ContextOf");
    DriveContext c;
    const auto it = g_genByEid.find(eid);   // find, never operator[]: a read inserts nothing
    if (it != g_genByEid.end()) c.gen = it->second;
    c.open = actor && Find(actor) != nullptr;
    return c;
}

void Release(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Release");
    Driven* d = Find(actor);
    if (!d || !d->claimed || d->syncer != 0) return;   // a client's row is not this verb's to let go
    d->claimed     = false;
    d->lastMoveMs  = coop::active_drive::NowMs();   // the rest clock starts now
    d->nextProbeMs = 0;                              // and the probe goes back to per tick
    UE_LOGI("[PROP-DRIVE] HOST release eid=%u -- coasting until it rests", d->eid);
}

void OnPeerWorldReady() {
    UE_ASSERT_GAME_THREAD("prop_drive_host::OnPeerWorldReady");
    if (g_driven.empty()) return;
    // Every driven prop's pose goes out again on the next tick, resting ones included, so the
    // joiner parks them where the other peers already have them. MTA's ResyncForPlayer
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp) sends its
    // unoccupied vehicles, resting ones included, reliably and to that player alone, when it enters
    // a dimension; ours rides the unreliable broadcast stream once per join, so the others receive a
    // pose they already hold, and a joiner that loses the datagram of a prop at rest does not park it
    // until the prop moves again (docs/props.md, Known limits).
    for (Driven& d : g_driven) {
        d.resend      = true;
        d.nextProbeMs = 0;
    }
    UE_LOGI("[PROP-DRIVE] HOST world-ready re-send armed for %zu driven prop(s)", g_driven.size());
}

// Per host tick: see Pump.
static void Tick(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_drive_host::Tick");
    if (g_driven.empty()) return;
    const uint64_t now = coop::active_drive::NowMs();
    static uint32_t sPublished = 0;
    // Publishing starts where the last tick's joins left off, so with more driven props than one send
    // carries, none waits forever for its turn.
    const size_t n = g_driven.size();
    size_t joined = 0;
    for (size_t k = 0; k < n; ++k) {
        Driven& d = g_driven[(g_turn + k) % n];
        if (d.dead) continue;   // closed since the last sweep by a client row's ending
        void* actor = d.ref.Get();
        if (!actor) {
            // The destroy crossed on its own seam; a receiver drops a drive whose actor died.
            UE_LOGI("[PROP-DRIVE] HOST eid=%u died -- dropped", d.eid);
            if (d.syncer != 0) coop::prop_syncer_host::EndClientRow(d, coop::prop_syncer_host::Ending::Died);
            d.dead = true;
            continue;
        }
        if (InAHand(actor)) {
            if (d.syncer != 0) {   // a client's row: the ending releases the copy and tells the syncer
                coop::prop_syncer_host::EndClientRow(d, coop::prop_syncer_host::Ending::Hand);
                continue;
            }
            // A hand took it: the held-prop lane streams it from here and its release hands the
            // velocity back. The end edge closes this stream's generation first, whatever the
            // prop's turn in the queue.
            // A failed read ends at the last pose the receivers have; with none sent there is nothing to end.
            ue_wrap::FVector endLoc{};
            ue_wrap::FRotator endRot{};
            if (E::TryGetActorLocation(actor, endLoc) && E::TryGetActorRotation(actor, endRot))
                SendEnd(s, d, actor, endLoc, endRot, "taken by a hand");
            else if (d.everSent)
                SendEnd(s, d, actor, d.sentLoc, d.sentRot, "taken by a hand");
            d.dead = true;
            continue;
        }
        if (d.syncer != 0) {   // a client's row ends by its own rules (prop_syncer_host), never by the rest clock
            joined += PublishAdmitted(s, d);
            continue;
        }
        if (now < d.nextProbeMs) continue;   // resting: read at the slow cadence
        // A pose read before its turn would be overwritten before any send.
        const coop::net::PoseTurn turn = s.PropDrivePoseTurn(d.eid);
        if (turn == coop::net::PoseTurn::Wait) continue;
        // A failed read is a dispatch that faulted, which the next tick repeats: nothing is seen to
        // move, so the rest clock runs as for a prop at rest -- a claimed row drops to the slow cadence
        // and a coasting one ends at the last pose it sent. Ending a claimed row here would not hold:
        // the claim lane's next pass re-opens it.
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        const bool poseRead = E::TryGetActorLocation(actor, loc) && E::TryGetActorRotation(actor, rot);
        if (!poseRead && !d.unreadSaid) {
            d.unreadSaid = true;
            UE_LOGW("[PROP-DRIVE] HOST eid=%u -- its location or rotation could not be read; it streams nothing and "
                    "rests", d.eid);
        }
        if (poseRead && Moved(d, loc, rot)) {
            coop::net::PropPoseSnapshot pp{};
            pp.key       = d.key;
            pp.elementId = d.eid;
            pp.ctx       = d.gen;
            pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
            pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            s.PublishPropDrivePose(pp);   // queued: it goes out in its turn, refreshed until then
            if (turn == coop::net::PoseTurn::Join) ++joined;
            d.everSent    = true;
            d.resend      = false;
            d.sentLoc     = loc;
            d.sentRot     = rot;
            d.lastMoveMs  = now;
            d.nextProbeMs = 0;
            if ((++sPublished % 120) == 1)
                UE_LOGI("[PROP-DRIVE] HOST publish eid=%u gen=%u %s -> (%.1f,%.1f,%.1f)",
                        d.eid, static_cast<unsigned>(d.gen), d.claimed ? "claimed" : "coasting",
                        loc.X, loc.Y, loc.Z);
            continue;
        }
        if (now - d.lastMoveMs >= kRestMs) {
            if (!d.claimed) {
                if (poseRead)        SendEnd(s, d, actor, loc, rot, "rested");
                else if (d.everSent) SendEnd(s, d, actor, d.sentLoc, d.sentRot, "rested");
                d.dead = true;
                continue;
            }
            d.nextProbeMs = now + kRestProbeMs;   // claimed and resting: the slow cadence
        }
    }
    g_turn = n ? (g_turn + joined) % n : 0;
    g_driven.erase(std::remove_if(g_driven.begin(), g_driven.end(),
                                  [](const Driven& d) { return d.dead; }),
                   g_driven.end());
}

void Pump(coop::net::Session& s, bool isHost) {
    coop::prop_syncer::Tick(s, isHost);   // first: the role for a feeder's Claim, and a client's rows of the props it syncs
    if (isHost) {
        // The order is the point: a client's rows are judged first, so the row's turn in Tick publishes what
        // this tick admitted, and the apply tick then advances the host's copies they just fed.
        coop::prop_syncer_host::Admit(s);
        Tick(s);
    }
    coop::prop_drive_stream::TickApplyAndDrive(s);
}

void OnDisconnect() {
    g_driven.clear();
    g_turn = 0;
    g_genByEid.clear();
    coop::prop_syncer_host::OnDisconnect();   // its buckets, hand-back records and counters go with the rows
    coop::prop_syncer::OnDisconnect();        // a client's table, backoffs and counters (a link that drops ends the session)
}

}  // namespace coop::prop_drive_host
