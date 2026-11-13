// coop/dev/dronesack_drill.cpp -- see coop/dev/dronesack_drill.h: the client's half (its arm 4 take is
// dronesack_drill_take.cpp), the destroy hooks and what the two halves share.

#include "coop/dev/dronesack_drill.h"

#include "coffee_drill_detail.h"     // co-located private header: SpawnProp is the host's; HeldByDrive is read here
#include "dronesack_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::dronesack_drill {
namespace {

namespace D  = coop::director;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;
namespace UP = ue_wrap::prop;
namespace AR = coop::props::container_act_request;
namespace SR = ue_wrap::save_record;
namespace CI = ue_wrap::container_inventory;
using namespace detail;

constexpr size_t kMaxDestroys = 16;
constexpr size_t kMaxReceived = 32;

// ---- what the hooks fill ----
bool                   g_armed = false;   // the hooks record only while armed
bool                   g_isHost = false;
void*                  g_containerCls = nullptr;
void*                  g_propCls = nullptr;   // arm 2: the loose props' class
uint32_t               g_crateEid = 0;
std::string            g_crateKey;
std::vector<uint32_t>  g_watch;               // arm 2: the loose props' element ids
std::vector<Destroyed> g_destroys;
size_t                 g_destroysDropped = 0;
std::vector<Received>  g_recv;

// ---- the chat, latched on sight (a live row retires after a few more lines) ----
std::map<std::string, std::string> g_rests;
std::set<std::string>              g_heard;

const char* const kRestPrefixes[] = {kSackPrefix, kBoundPrefix, kCratePrefix, kPropsPrefix, kIndexPrefix, kTookPrefix, kPutsPrefix};
const char* const kHeardLines[] = {kDoneLine, kCallLine, kSoldLine};

void PollChat() {
    if (!DC::PollDue()) return;
    for (const char* prefix : kRestPrefixes) {
        std::string rest;
        if (DC::LiveRestOf(prefix, &rest)) g_rests[prefix] = rest;
    }
    for (const char* line : kHeardLines)
        if (DC::ChatHasLive(line)) g_heard.insert(line);
}

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// ---- the client ----

void CancelWalk() {
    if (g_c.walk && g_c.walk->state.load() == 0) D::CancelBackgroundWalk(*g_c.walk);
    g_c.walk.reset();
}

void Abandon(const char* why) {
    UE_LOGW("[SACK-DRILL] ABANDONED on the client: %s", why);
    if (Mode() == 2) coop::rig_verdict::Say("dronesack-arm2", coop::rig_verdict::Outcome::Invalid, "%s", why);
    CancelWalk();
    g_c.grab.reset();
    g_c.ph = ClientPh::Done;
}

}  // namespace

namespace detail {

Client g_c;

uint64_t NowMs() { return ::GetTickCount64(); }

void Say(const std::string& line) { DC::Say(line); }

uint32_t ParseEid(const std::string& s) {
    return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
}

std::vector<uint32_t> ParseEids(const std::string& csv) {
    std::vector<uint32_t> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (const uint32_t e = ParseEid(tok)) out.push_back(e);
    return out;
}

bool Rest(const std::string& prefix, std::string* rest) {
    const auto it = g_rests.find(prefix);
    if (it == g_rests.end()) return false;
    *rest = it->second;
    return true;
}

bool Heard(const std::string& line) { return g_heard.count(line) != 0; }

// dronesack_prop=drive (both peers): arms 2 and 3 use zip disks (prop_drive_C) instead of cups.
bool LooseIsDrive() {
    static const bool d = coop::config::ResolveEnum(::coop::config_registry::rows::dronesack_prop) == "drive";
    return d;
}
const wchar_t* LooseClass() { return LooseIsDrive() ? L"prop_drive_C" : L"prop_C"; }
const wchar_t* LooseName() { return LooseIsDrive() ? L"drive" : L"cup"; }

int Mode() {
    static const int m = static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::dronesack_drill));
    return m;
}

bool Arm(bool isHost) {
    g_isHost = isHost;
    if (!g_containerCls) g_containerCls = R::FindClass(L"prop_container_C");
    if (Mode() >= 2 && !g_propCls) g_propCls = R::FindClass(LooseClass());
    g_armed = g_containerCls != nullptr && (Mode() < 2 || g_propCls != nullptr);
    // Arm 2's red (CLIENT, container_overlap_no_cancel): this client's cancel of an overlap put on the shared mirror is
    // skipped, so the sack's call runs on the client and its destroy takes the host's copy with no record: the verdict FAILs.
    static const bool noCancel = coop::config::ResolveFlag(::coop::config_registry::rows::container_overlap_no_cancel);
    if (g_armed && !isHost && Mode() == 2 && noCancel) {
        AR::dev::NoOverlapCancel(true);
        UE_LOGW("[SACK-DRILL] client RED: the overlap put on the shared mirror is not cancelled (container_overlap_no_cancel)");
    }
    return g_armed;
}

void SetCrate(uint32_t eid, const std::string& key) {
    g_crateEid = eid;
    g_crateKey = key;
}

void SetWatch(const std::vector<uint32_t>& eids) { g_watch = eids; }

std::vector<Destroyed>& Destroys() { return g_destroys; }
std::vector<Received>& Recv() { return g_recv; }
uint32_t CrateEid() { return g_crateEid; }

int RecordsOf(void* containerActor) {
    if (!containerActor) return -1;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(containerActor), L"propInventory");
    if (off < 0) return -1;
    void* const inv = ReadAt<void*>(containerActor, off);
    if (!inv || !R::IsLive(inv)) return -1;
    const uint8_t* const slot = CI::ContentsSlot(inv);
    return slot ? SR::ReadArr(slot, 0).num : -1;
}

void* ContainerOf(void* holder) {
    if (!holder) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(holder), L"container");
    if (off < 0) return nullptr;
    void* const container = ReadAt<void*>(holder, off);
    return container && R::IsLive(container) ? container : nullptr;
}

int SackRecords(void* sack) { return RecordsOf(ContainerOf(sack)); }

void PrintDestroy(const char* peer, Destroyed& d, uint32_t crateEid, uint64_t refMs, bool withBroadcast) {
    d.printed = true;
    const unsigned long long at = d.ms > refMs ? d.ms - refMs : 0;
    if (crateEid != 0 && d.eid == crateEid) {
        if (withBroadcast)
            UE_LOGI("[SACK-DRILL] %s crate mirror eid=%u destroyed locally by '%ls' at +%llums, broadcast=%d", peer,
                    d.eid, d.by.c_str(), at, d.broadcast ? 1 : 0);
        else
            UE_LOGI("[SACK-DRILL] %s crate eid=%u destroyed locally by '%ls' at +%llums", peer, d.eid, d.by.c_str(), at);
        return;
    }
    UE_LOGI("[SACK-DRILL] %s other %s eid=%u destroyed locally by '%ls' at +%llums, broadcast=%d, wire=%d", peer,
            d.container ? "container" : "prop", d.eid, d.by.c_str(), at, d.broadcast ? 1 : 0, d.wire ? 1 : 0);
}

}  // namespace detail

namespace {

// ---- arm 2: the client's route ----

float WrapDeg(float a) {
    while (a > 180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

// Walks the carrier, still holding the prop, to the planned stand: g_c.standCm from the sack's origin on the side it is on,
// ending within `reachCm` of it or at `deadlineS`. False when the player's place does not read.
bool StartStandWalk(void* player, const ue_wrap::FVector& at, float reachCm, int deadlineS) {
    ue_wrap::FVector me{};
    if (!E::TryGetActorLocation(player, me)) return false;
    float ux = me.X - at.X, uy = me.Y - at.Y;
    const float len = std::hypot(ux, uy);
    if (len < 1.f) { ux = 1.f; uy = 0.f; } else { ux /= len; uy /= len; }
    const ue_wrap::FVector stand{at.X + ux * g_c.standCm, at.Y + uy * g_c.standCm, me.Z};
    g_c.walk = D::StartBackgroundWalk(stand, reachCm, deadlineS, true);
    return true;
}

// The put's line, from what the hooks saw of the prop's mirror.
void PutLine(uint32_t eid) {
    for (Destroyed& d : g_destroys) {
        if (d.eid != eid || d.printed) continue;
        d.printed = true;
        UE_LOGI("[SACK-DRILL] client put eid=%u: absorbed locally=%d by '%ls', broadcast=%d", eid, d.wire ? 0 : 1,
                d.by.c_str(), d.broadcast ? 1 : 0);
        if (d.wire)
            UE_LOGI("[SACK-DRILL] client put eid=%u: the mirror was destroyed on the host's PropDestroy", eid);
        return;
    }
    UE_LOGI("[SACK-DRILL] client put eid=%u: absorbed locally=0 by '-', broadcast=0 (the mirror %s)", eid,
            EL::LivePropActor(eid) ? "still lives here" : "is gone with no destroy seen");
}
// The put props this client let go of are named in its puts line: the host waits on each of them, not on a clock.
void NoteLetGo(uint32_t eid) {
    for (const uint32_t e : g_c.released)
        if (e == eid) return;
    g_c.released.push_back(eid);
}


// A prop this client could not put: said, and the route goes on to the next.
void Skip(const char* why) {
    UE_LOGI("[SACK-DRILL] client put eid=%u: not made: %s", g_c.eids[g_c.i], why);
    // A prop still in the hand is let go where it is, as a player lets go before the next grab: a grab of the next prop
    // over it changes the hold with no release, and the host's copy stays where the hand last held it.
    if (void* const player = coop::players::Registry::Get().Local()) {
        void* const p = g_c.prop.Get();
        if (p && D::Grabbing(player) == p && D::CallOnPlayer(player, L"dropGrabObject")) NoteLetGo(g_c.eids[g_c.i]);
    }
    CancelWalk();
    g_c.grab.reset();
    ++g_c.i;
    g_c.st = Put::Pick;
}

void GoPut(Put st) {
    g_c.st = st;
    g_c.stMs = NowMs();
}

void RouteTick(void* player) {
    const uint64_t now = NowMs();
    if (now - g_c.phaseMs > kClientBoundMs) { Abandon("the route did not finish"); return; }
    const uint32_t eid = g_c.i < g_c.eids.size() ? g_c.eids[g_c.i] : 0;
    switch (g_c.st) {
    case Put::Pick:
        if (g_c.i >= g_c.eids.size()) {
            std::string csv;
            for (const uint32_t e : g_c.released) csv += (csv.empty() ? "" : ",") + std::to_string(e);
            Say(std::string(kPutsPrefix) + (csv.empty() ? "none" : csv));
            UE_LOGI("[SACK-DRILL] client puts done: %zu props tried, let go of: %s", g_c.eids.size(), csv.empty() ? "none" : csv.c_str());
            g_c.phaseMs = NowMs();   // the Watch leg's bound counts from here
            g_c.ph = ClientPh::Watch;
            return;
        }
        GoPut(Put::Resolve);
        return;
    case Put::Resolve:
        if (void* const p = EL::LivePropActor(eid)) { g_c.prop.Set(p); GoPut(Put::Drive); return; }
        if (now - g_c.stMs > kMirrorBoundMs) Skip("this world holds no mirror of the prop");
        return;
    case Put::Drive: {
        void* const p = g_c.prop.Get();
        if (!p) return Skip("the prop died before the walk");
        if (coop::dev::coffee_drill::detail::HeldByDrive(p)) {   // the host's prop is still on the host's drive
            if (now - g_c.stMs > kDriveBoundMs) Skip("the prop never left the host's drive");
            return;
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(p, at)) return Skip("the prop's place is unread");
        g_c.walk = D::StartBackgroundWalk(at, kGrabReachCm, kWalkDeadlineS);
        GoPut(Put::Walk);
        return;
    }
    case Put::Walk: {
        const int w = WalkState();
        if (w == 0) return;
        g_c.walk.reset();
        if (w == 2) return Skip("the director's walk to the prop failed or ran out its deadline");
        void* const p = g_c.prop.Get();
        if (!p) return Skip("the prop died before the grab");
        g_c.grab = std::make_unique<D::AimedGrab>(player, p);
        g_c.grabTries = 0;
        GoPut(Put::Grab);
        return;
    }
    case Put::Grab: {
        void* const p = g_c.prop.Get();
        if (!p) return Skip("the prop died during the grab");
        const D::GrabState gs = g_c.grab->Tick();
        if (gs == D::GrabState::Working) return;
        if (gs == D::GrabState::Failed) {
            UE_LOGI("[SACK-DRILL] client put eid=%u: grab try %d failed: %s", eid, g_c.grabTries + 1, g_c.grab->Why());
            if (++g_c.grabTries >= kGrabTries) return Skip("the grab failed");
            g_c.grab = std::make_unique<D::AimedGrab>(player, p);
            return;
        }
        g_c.grab.reset();
        ue_wrap::FVector at{};
        void* const sack = g_c.sack.Get();
        if (!sack || !E::TryGetActorLocation(sack, at)) return Skip("the sack mirror's place is unread");
        // The hold point is the camera plus its forward times the hold length, so the camera's height over the window's
        // middle (kOverAimCm above the sack's origin) and that length fix both the pitch and how far from the sack the
        // carrier must stand: horizontal = sqrt(hold^2 - drop^2) (the sister hold loop's derivation,
        // container_intent_drill_throw_into_hold.cpp HoldStandCm).
        E::MainPlayerGrabState grip{};
        if (!E::ReadMainPlayerGrabState(player, grip) || grip.grabLen <= 0.f) return Skip("the hold length is unread");
        const float hold = grip.grabLen, drop = E::GetCameraLocation().Z - (at.Z + kOverAimCm);
        if (std::fabs(drop) >= hold) {
            char why[112];
            std::snprintf(why, sizeof(why), "the hold of %.0f cm cannot reach the window from a camera %.0f cm above it", hold, drop);
            return Skip(why);
        }
        g_c.standCm = std::sqrt(hold * hold - drop * drop);
        g_c.planPitch = -std::asin(drop / hold) * (180.f / 3.14159265f);
        UE_LOGI("[SACK-DRILL] client put eid=%u: grabbed; carrying it to %.0f cm from the sack (hold %.0f cm, camera %.0f cm "
                "above the window's middle, planned pitch %.1f)", eid, g_c.standCm, hold, drop, g_c.planPitch);
        g_c.restood = false;
        if (!StartStandWalk(player, at, kStandReachCm, kWalkDeadlineS)) return Skip("the player's place is unread");
        GoPut(Put::Carry);
        return;
    }
    case Put::Carry: {
        // The host's copy, driven by this hold, may touch the sack on the way and be absorbed: the mirror then dies here.
        if (!g_c.prop.Get()) { CancelWalk(); GoPut(Put::Settle); return; }
        const int w = WalkState();
        if (w == 0) return;
        g_c.walk.reset();
        if (w == 2 && !g_c.restood) return Skip("the carry to the sack failed or ran out its deadline");
        if (w == 2) UE_LOGI("[SACK-DRILL] client put eid=%u: the second walk did not reach the stand; putting from where it stands", eid);
        g_c.aimTicks = 0;
        g_c.aimed = false;
        g_c.bestMiss = 1e9f;
        g_c.bestSpeed = 1e9f;
        GoPut(Put::Over);
        return;
    }
    case Put::Over: {
        void* const p = g_c.prop.Get();
        if (!p) { GoPut(Put::Settle); return; }
        void* const sack = g_c.sack.Get();
        void* const ctrl = E::GetController(player);
        ue_wrap::FVector sa{}, pp{}, me{};
        if (!ctrl || !sack || !E::TryGetActorLocation(sack, sa) || !E::TryGetActorLocation(p, pp) ||
            !E::TryGetActorLocation(player, me))
            return Skip("no controller, sack, prop or player place to aim with");
        if (now - g_c.stMs > kDriveBoundMs) {
            if (g_c.bestSpeed >= 1e8f) return Skip("the prop never came over the sack");
            char why[112];
            std::snprintf(why, sizeof(why), "the prop never came to rest over the sack (slowest %.1f cm/s)", g_c.bestSpeed);
            return Skip(why);
        }
        // The carry's reach is flat: the walker has arrived only when it stands at the sack's height too.
        if (std::fabs(me.Z - sa.Z) > kFeetCm) return;
        // On arrival: the walk ends anywhere within kStandReachCm of the planned stand, and the planned pitch is for that
        // stand. One that stopped further off than kStandTolCm is walked to again, tighter, once; if it is still off the put
        // goes on from where it stands, and the line says where that is.
        if (!g_c.aimed) {
            const float stood = std::hypot(me.X - sa.X, me.Y - sa.Y);
            g_c.aimed = true;
            UE_LOGI("[SACK-DRILL] client put eid=%u: stands %.0f cm out (planned %.0f)%s", eid, stood, g_c.standCm,
                    g_c.restood ? ", after walking to it again" : "");
            if (std::fabs(stood - g_c.standCm) > kStandTolCm && !g_c.restood && StartStandWalk(player, sa, kStandTolCm, kRestandDeadlineS)) {
                g_c.restood = true;
                GoPut(Put::Carry);
                return;
            }
        }
        // Where the held prop is, measured: the release waits for it to be over the sack, not for the aim.
        const float flat = std::hypot(pp.X - sa.X, pp.Y - sa.Y), above = pp.Z - sa.Z;
        const float miss = (std::max)(0.f, flat - kOverFlatCm) + (std::max)(0.f, (std::max)(kOverMinCm - above, above - kOverMaxCm));
        if (miss < g_c.bestMiss) { g_c.bestMiss = miss; g_c.bestFlat = flat; g_c.bestAbove = above; }
        // A release only once the held prop is over the sack and at rest in the hand, as a player lets go: one let go
        // while it swings throws it, and the throw is what the hold's speed measures. Nothing announces the settling,
        // so its state is read here.
        if (miss == 0.f) {
            const UP::VelocityState v = UP::GetPhysicsVelocity(p, /*linearOnly=*/true);
            if (!v.ok) return Skip("the held prop's speed is unread");
            const float speed = std::sqrt(v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                                          v.linearCmS.Z * v.linearCmS.Z);
            if (speed < g_c.bestSpeed) g_c.bestSpeed = speed;
            if (speed > kSettledCmS) return;   // over the sack, still swinging: wait for it to settle in the hand
            if (!D::CallOnPlayer(player, L"dropGrabObject")) return Skip("dropGrabObject did not run");
            UE_LOGI("[SACK-DRILL] client put eid=%u: release at flat=%.0f above=%.0f at rest (%.1f cm/s), pitch %.1f (planned %.1f), "
                    "stood %.0f cm out (planned %.0f)", eid, flat, above, speed, E::GetControlRotation(ctrl).Pitch, g_c.planPitch,
                    std::hypot(me.X - sa.X, me.Y - sa.Y), g_c.standCm);
            g_c.letGo = true;
            NoteLetGo(eid);
            GoPut(Put::Settle);
            return;
        }
        if (++g_c.aimTicks > kAimTicks) {
            char why[160];
            std::snprintf(why, sizeof(why), "closest flat=%.0f above=%.0f, pitch %.1f (planned %.1f), stood %.0f cm out (planned %.0f)",
                          g_c.bestFlat, g_c.bestAbove, E::GetControlRotation(ctrl).Pitch, g_c.planPitch,
                          std::hypot(me.X - sa.X, me.Y - sa.Y), g_c.standCm);
            return Skip(why);
        }
        // A small step: the angle between where the held prop is and where it should be, as seen from the camera.
        const ue_wrap::FVector cam = E::GetCameraLocation();
        const ue_wrap::FRotator want = D::LookAt(cam, {sa.X, sa.Y, sa.Z + kOverAimCm});
        const ue_wrap::FRotator have = D::LookAt(cam, pp);
        ue_wrap::FRotator r = E::GetControlRotation(ctrl);
        r.Pitch += std::clamp(kAimGain * WrapDeg(want.Pitch - have.Pitch), -kAimStepDeg, kAimStepDeg);
        r.Yaw   += std::clamp(kAimGain * WrapDeg(want.Yaw - have.Yaw), -kAimStepDeg, kAimStepDeg);
        E::SetControlRotation(ctrl, r);
        return;
    }
    case Put::Settle: {
        if (g_c.letGo) {   // the drop is measured: a prop still in the hand means it did not run, and the effect seam lets go
            g_c.letGo = false;
            void* const p = g_c.prop.Get();
            if (p && D::Grabbing(player) == p) {
                E::ReleaseMainPlayerGrabIfHolding(player, p);
                UE_LOGI("[SACK-DRILL] client put eid=%u: dropGrabObject left the prop in the hand; released it at the effect seam", eid);
            }
        }
        bool seen = false;
        for (const Destroyed& d : g_destroys) seen = seen || d.eid == eid;
        if (!seen && now - g_c.stMs <= kPutBoundMs) return;
        PutLine(eid);
        ++g_c.i;
        GoPut(Put::Pick);
        return;
    }
    }
}

// ---- arm 4: does the client's sack keep what the sale sold ----

constexpr unsigned kProbeS[4] = {1, 5, 15, 30};   // seconds after the host's sold line

void SoldProbes(uint64_t now) {
    if (!g_c.soldSeenMs) {
        if (!Heard(kSoldLine)) {   // before the sale: the walk to the sack and its screen opened, then held
            if (g_c.take == Take::Idle) TakeStart(now);
            TakeTick(now);
            return;
        }
        g_c.soldSeenMs = now;
        void* const container = ContainerOf(g_c.sack.Get());
        const EL::ElementId eid = container ? EL::Registry::Get().EidForActor(container) : EL::kInvalidId;
        UE_LOGI("[SACK-DRILL] client sack records are read from the sack's container field: '%ls' eid=%u",
                container ? R::ClassNameOf(container).c_str() : L"none", eid == EL::kInvalidId ? 0u : static_cast<unsigned>(eid));
        UE_LOGI("[SACK-DRILL] client drone container in sync scope=%d", ContainerInSyncScope(container) ? 1 : 0);
    }
    for (int i = 0; i < 4; ++i) {
        if (g_c.probed[i] || now - g_c.soldSeenMs < kProbeS[i] * 1000ULL) continue;
        g_c.probed[i] = true;
        UE_LOGI("[SACK-DRILL] client after sale +%us sack records=%d", kProbeS[i], SackRecords(g_c.sack.Get()));
    }
    MirrorWatch(now);
    TakeTick(now);   // the press, once the mirror has been watched alone and still lists records
    // The verdict has both halves: the press (refused Gone with nothing out, or nothing to take) and the mirror (0 in
    // time). The mirror's FAIL is said here, after the press has answered, so the press runs in the red too.
    if (g_c.take == Take::Done && g_c.mirror != 0 && !g_c.verdicted) {
        g_c.verdicted = true;
        if (g_c.mirror == 2)
            UE_LOGW("[SACK-DRILL] client arm 4 FAIL: the mirror still showed %d records 2 s after the sale, before any press",
                    g_c.mirrorLeft);
        else if (g_c.takeOk)
            UE_LOGI("[SACK-DRILL] client arm 4 PASS: the mirror read 0 within 2 s of the sale, and every press was "
                    "refused Gone or none was made");
    }
}

// Arm 3: which containers hold the index the host read off its crate, when the host said one.
void IndexHolders(Moment m) {
    std::string rest;
    if (Rest(kIndexPrefix, &rest)) CensusIndexHolders("client", m, std::atoi(rest.c_str()));
}

// The census edges: the host's call (before), its sale (after), and kCensusLaterMs after that (the second read).
void CensusStep(uint64_t now) {
    if (g_c.census == 0 && Heard(kCallLine)) {
        CensusTake("client", Moment::Before);
        g_c.census = 1;
    }
    if (g_c.census <= 1 && Heard(kSoldLine)) {
        CensusTake("client", Moment::After);
        IndexHolders(Moment::After);
        g_c.afterMs = now;
        g_c.census = 2;
    }
    if (g_c.census == 2 && now - g_c.afterMs >= kCensusLaterMs) {
        CensusTake("client", Moment::AfterLater);
        IndexHolders(Moment::AfterLater);
        CensusDiff("client", g_c.sack.Get());
        g_c.census = 3;
    }
    if (Mode() == 4) SoldProbes(now);
}

void ClientTickImpl() {
    const uint64_t now = NowMs();
    switch (g_c.ph) {
    case ClientPh::Wait:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        g_c.phaseMs = now;
        g_c.ph = ClientPh::Mirror;
        return;
    case ClientPh::Mirror: {
        std::string rest;
        if (!Rest(kSackPrefix, &rest)) return;   // the host has not named its sack
        g_c.sackEid = ParseEid(rest);
        void* const mirror = g_c.sackEid ? EL::LivePropActor(g_c.sackEid) : nullptr;
        if (!mirror) {
            if (now - g_c.phaseMs > kMirrorBoundMs) Abandon("this world holds no mirror of the host's sack");
            return;
        }
        if (!Arm(false)) { Abandon("the container class does not resolve"); return; }
        g_c.sack.Set(mirror);
        g_c.armMs = now;
        Say(std::string(kBoundPrefix) + std::to_string(g_c.sackEid));
        UE_LOGI("[SACK-DRILL] client READY sack mirror eid=%u records=%d", g_c.sackEid, SackRecords(mirror));
        if (Mode() == 4) WalkToSack();
        g_c.phaseMs = now;
        g_c.ph = Mode() == 2 ? ClientPh::Props : ClientPh::Watch;   // arms 1 and 3: the host does the work, this peer reads
        return;
    }
    case ClientPh::Props: {
        std::string rest;
        if (!Rest(kPropsPrefix, &rest)) return;   // the host has not named its loose props
        g_c.eids = ParseEids(rest);
        if (g_c.eids.empty()) { Abandon("the host named no loose prop"); return; }
        SetWatch(g_c.eids);
        UE_LOGI("[SACK-DRILL] client READY %zu loose props to put", g_c.eids.size());
        g_c.phaseMs = now;
        g_c.ph = ClientPh::Route;
        return;
    }
    case ClientPh::Route:
        if (void* const player = coop::players::Registry::Get().Local()) RouteTick(player);
        return;
    case ClientPh::Watch: {
        if (Mode() >= 3) CensusStep(now);   // arms 3 and 4 read the container census round the sale; arm 2 ends at its verdict
        if (!Heard(kDoneLine) && now - g_c.phaseMs > kWatchBoundMs) { Abandon("the host never said it is done"); return; }
        // The census after the sale is compared before the run ends; a host that ended with no sale has none to wait for.
        if (!Heard(kDoneLine) || g_c.census == 2) return;
        if (Mode() == 4 && g_c.soldSeenMs && g_c.took < 0) return;   // the take is made before the host can end
        std::string rest;
        const uint32_t crateEid = Rest(kCratePrefix, &rest) ? ParseEid(rest) : 0;
        for (Destroyed& d : g_destroys)
            if (!d.printed) PrintDestroy("client", d, crateEid, g_c.armMs, true);
        if (g_destroysDropped)
            UE_LOGW("[SACK-DRILL] client: %zu more destroys were not recorded", g_destroysDropped);
        if (Mode() == 4) {
            UE_LOGI("[SACK-DRILL] client DONE arm 4 sack records=%d took=%d", SackRecords(g_c.sack.Get()), g_c.took > 0 ? 1 : 0);
        } else if (Mode() >= 2) {
            UE_LOGI("[SACK-DRILL] client DONE arm %d sack records=%d", Mode(), SackRecords(g_c.sack.Get()));
        } else {
            const bool alive = crateEid != 0 && EL::LivePropActor(crateEid) != nullptr;
            UE_LOGI("[SACK-DRILL] client DONE sack records=%d crate mirror alive=%d", SackRecords(g_c.sack.Get()),
                    alive ? 1 : 0);
        }
        g_c.ph = ClientPh::Done;
        return;
    }
    case ClientPh::Done:
        return;
    }
}

}  // namespace

bool IsEnabled() { return detail::Mode() != 0; }

void Tick(coop::net::Session* s) {
    if (!IsEnabled() || !s || !s->connected()) return;
    PollChat();
    if (s->role() == coop::net::Role::Host) detail::HostTick(s);
    else ClientTickImpl();
}

void OnDisconnect() {
    CancelWalk();
    g_armed = false;
    g_isHost = false;
    g_crateEid = 0;
    g_crateKey.clear();
    g_watch.clear();
    g_destroys.clear();
    g_destroysDropped = 0;
    g_recv.clear();
    g_rests.clear();
    g_heard.clear();
    g_c = Client{};
    detail::HostReset();   // which also clears the census rows both roles keep
}

void NoteDestroy(void* actor, void* caller) {
    if (!g_armed || !actor) return;
    void* const cls = R::ClassOf(actor);
    const bool container = cls == g_containerCls;
    if (!container && (!g_propCls || cls != g_propCls)) return;
    if (g_destroys.size() >= kMaxDestroys) { ++g_destroysDropped; return; }
    Destroyed d;
    d.actor = actor;
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    d.eid = eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    d.ms = NowMs();
    d.by = caller ? R::ClassNameOf(caller) : std::wstring(L"<native>");
    d.wire = coop::prop_echo_suppress::IsIncomingDestroy(actor);
    d.container = container;
    g_destroys.push_back(std::move(d));
}

void NoteBroadcast(void* actor) {
    if (!g_armed) return;
    for (size_t i = g_destroys.size(); i-- > 0;)
        if (g_destroys[i].actor == actor) { g_destroys[i].broadcast = true; return; }
}

void NoteWirePropDestroy(uint32_t eid, const char* key, uint8_t keyLen, int slot) {
    if (!g_armed || !g_isHost || slot <= 0 || g_recv.size() >= kMaxReceived) return;
    const bool crateByKey = eid == 0 && keyLen != 0 && g_crateEid != 0 && g_crateKey.size() == keyLen &&
                            std::memcmp(key, g_crateKey.data(), keyLen) == 0;
    const uint32_t named = crateByKey ? g_crateEid : eid;
    const bool watched = named != 0 && (named == g_crateEid || std::find(g_watch.begin(), g_watch.end(), named) != g_watch.end());
    if (!watched) return;
    for (const Received& r : g_recv)
        if (r.eid == named) return;   // the first word about an eid is the one kept
    Received r;
    r.eid = named;
    r.slot = slot;
    r.ms = NowMs();
    r.alive = EL::LivePropActor(named) != nullptr;
    g_recv.push_back(r);
}

}  // namespace coop::dev::dronesack_drill
