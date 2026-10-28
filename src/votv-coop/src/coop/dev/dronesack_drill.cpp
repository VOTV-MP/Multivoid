// coop/dev/dronesack_drill.cpp -- see coop/dev/dronesack_drill.h: the client's half, the destroy hooks and what the
// two halves share.

#include "coop/dev/dronesack_drill.h"

#include "coffee_drill_detail.h"     // co-located private header: SpawnProp is the host's; HeldByDrive is read here
#include "dronesack_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_echo_suppress.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/actors/container_inventory.h"
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

const char* const kRestPrefixes[] = {kSackPrefix, kBoundPrefix, kCratePrefix, kPropsPrefix, kIndexPrefix, kTookPrefix};
const char* const kHeardLines[] = {kDoneLine, kPutsDoneLine, kCallLine, kSoldLine};

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

enum class Ph : uint8_t { Wait, Mirror, Props, Route, Watch, Done };
// One loose prop's walk, grab, carry and release (arm 2).
enum class Put : uint8_t { Pick, Resolve, Drive, Walk, Grab, Carry, Over, Settle };

struct Client {
    Ph                    ph = Ph::Wait;
    uint64_t              phaseMs = 0, armMs = 0;
    uint32_t              sackEid = 0;
    ue_wrap::CachedObjRef sack;
    // arm 2
    std::vector<uint32_t> eids;
    size_t                i = 0;
    Put                   st = Put::Pick;
    uint64_t              stMs = 0;
    int                   grabTries = 0;
    ue_wrap::CachedObjRef prop;
    int                   aimTicks = 0;                 // the Over leg: ticks spent steering the held prop
    float                 bestMiss = 1e9f, bestFlat = 0.f, bestAbove = 0.f;   // ... and the nearest it came to the window
    bool                  letGo = false;                // the Settle leg: the drop was called, not yet checked
    int                   census = 0;   // 0 none yet, 1 before taken, 2 after taken, 3 after+5s taken and compared
    uint64_t              soldSeenMs = 0;     // arm 4: when this peer first saw the host's sold line
    bool                  probed[4] = {};     // ... and which of the after-sale reads have been made
    int                   took = -1;          // ... and whether the client took a record out of the sold sack (-1: not yet)
    uint64_t              afterMs = 0;
    std::shared_ptr<D::BackgroundWalk> walk;
    std::unique_ptr<D::AimedGrab>      grab;
};
Client g_c;

void CancelWalk() {
    if (g_c.walk && g_c.walk->state.load() == 0) D::CancelBackgroundWalk(*g_c.walk);
    g_c.walk.reset();
}

void Abandon(const char* why) {
    UE_LOGW("[SACK-DRILL] ABANDONED on the client: %s", why);
    CancelWalk();
    g_c.grab.reset();
    g_c.ph = Ph::Done;
}

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

}  // namespace

namespace detail {

uint64_t NowMs() { return ::GetTickCount64(); }

void Say(const std::string& line) { DC::Say(line); }

bool Rest(const std::string& prefix, std::string* rest) {
    const auto it = g_rests.find(prefix);
    if (it == g_rests.end()) return false;
    *rest = it->second;
    return true;
}

bool Heard(const std::string& line) { return g_heard.count(line) != 0; }

int Mode() {
    static const int m = static_cast<int>(coop::config::ResolveInt(::coop::config_registry::rows::dronesack_drill));
    return m;
}

bool Arm(bool isHost) {
    g_isHost = isHost;
    if (!g_containerCls) g_containerCls = R::FindClass(L"prop_container_C");
    if (Mode() >= 2 && !g_propCls) g_propCls = R::FindClass(L"prop_C");
    g_armed = g_containerCls != nullptr && (Mode() < 2 || g_propCls != nullptr);
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

// A prop this client could not put: said, and the route goes on to the next.
void Skip(const char* why) {
    UE_LOGI("[SACK-DRILL] client put eid=%u: not made: %s", g_c.eids[g_c.i], why);
    CancelWalk();
    g_c.grab.reset();
    ++g_c.i;
    g_c.st = Put::Pick;
}

void GoPut(Put st) {
    g_c.st = st;
    g_c.stMs = NowMs();
}

int WalkState() { return g_c.walk ? g_c.walk->state.load() : 2; }

void RouteTick(void* player) {
    const uint64_t now = NowMs();
    if (now - g_c.phaseMs > kClientBoundMs) { Abandon("the route did not finish"); return; }
    const uint32_t eid = g_c.i < g_c.eids.size() ? g_c.eids[g_c.i] : 0;
    switch (g_c.st) {
    case Put::Pick:
        if (g_c.i >= g_c.eids.size()) {
            Say(kPutsDoneLine);
            UE_LOGI("[SACK-DRILL] client puts done: %zu props tried", g_c.eids.size());
            g_c.ph = Ph::Watch;
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
        UE_LOGI("[SACK-DRILL] client put eid=%u: grabbed; carrying it to the sack", eid);
        g_c.walk = D::StartBackgroundWalk(at, kSackReachCm, kWalkDeadlineS, true);
        GoPut(Put::Carry);
        return;
    }
    case Put::Carry: {
        // The host's copy, driven by this hold, may touch the sack on the way and be absorbed: the mirror then dies here.
        if (!g_c.prop.Get()) { CancelWalk(); GoPut(Put::Settle); return; }
        const int w = WalkState();
        if (w == 0) return;
        g_c.walk.reset();
        if (w == 2) return Skip("the carry to the sack failed or ran out its deadline");
        g_c.aimTicks = 0;
        g_c.bestMiss = 1e9f;
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
        if (now - g_c.stMs > kDriveBoundMs) return Skip("the prop never came over the sack");
        // The carry's reach is flat: the walker has arrived only when it stands at the sack's height too.
        if (std::fabs(me.Z - sa.Z) > kFeetCm) return;
        // Where the held prop is, measured: the release waits for it to be over the sack, not for the aim.
        const float flat = std::hypot(pp.X - sa.X, pp.Y - sa.Y), above = pp.Z - sa.Z;
        const float miss = (std::max)(0.f, flat - kOverFlatCm) + (std::max)(0.f, (std::max)(kOverMinCm - above, above - kOverMaxCm));
        if (miss < g_c.bestMiss) { g_c.bestMiss = miss; g_c.bestFlat = flat; g_c.bestAbove = above; }
        if (miss == 0.f) {
            if (!D::CallOnPlayer(player, L"dropGrabObject")) return Skip("dropGrabObject did not run");
            UE_LOGI("[SACK-DRILL] client put eid=%u: release at flat=%.0f above=%.0f", eid, flat, above);
            g_c.letGo = true;
            GoPut(Put::Settle);
            return;
        }
        if (++g_c.aimTicks > kAimTicks) {
            char why[96];
            std::snprintf(why, sizeof(why), "closest flat=%.0f above=%.0f", g_c.bestFlat, g_c.bestAbove);
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

std::wstring FirstRecordClass(void* container) {
    void* const inv = CI::InventoryOf(container);
    std::vector<SR::SaveRecord> recs;
    if (!inv || !CI::ReadContents(inv, recs, 4096) || recs.empty()) return L"?";
    return recs[0].className;
}

// A player's take: the container's own getObject(0), the call the inventory UI makes when a record is pressed. It
// takes record 0 out of the inventory and spawns its item at the player, and hands the spawned actor back.
void TakeFromSold(void* container) {
    const int before = RecordsOf(container);
    if (before <= 0) {
        UE_LOGI("[SACK-DRILL] client took nothing from the sold sack: it reads %d records", before);
        g_c.took = 0;
    } else {
        const std::wstring cls = FirstRecordClass(container);
        void* const fn = R::FindDispatchFunctionCached(R::ClassOf(container), L"getObject");
        ue_wrap::ParamFrame f(fn);
        const ue_wrap::FVector none{};
        if (!fn || !f.valid() || !f.Set<int32_t>(L"index", 0) || !f.Set(L"customLoc", false) ||
            !f.SetRaw(L"loc", &none, sizeof(none)) || !ue_wrap::Call(container, f)) {
            UE_LOGI("[SACK-DRILL] client could not take from the sold sack: getObject does not run for class '%ls'",
                    R::ClassNameOf(container).c_str());
            g_c.took = 0;
        } else {
            void* const out = f.Get<void*>(L"OutputPin");
            const bool live = out && R::IsLive(out);
            UE_LOGI("[SACK-DRILL] client took from the sold sack through getObject: records %d -> %d, record 0 was '%ls', "
                    "returned '%ls'", before, RecordsOf(container), cls.c_str(),
                    live ? R::ClassNameOf(out).c_str() : L"nothing");
            g_c.took = live ? 1 : 0;
        }
    }
    Say(std::string(kTookPrefix) + std::to_string(g_c.took));
}

void SoldProbes(uint64_t now) {
    if (!g_c.soldSeenMs) {
        if (!Heard(kSoldLine)) return;
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
        if (i == 1) TakeFromSold(ContainerOf(g_c.sack.Get()));
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
    case Ph::Wait:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        g_c.phaseMs = now;
        g_c.ph = Ph::Mirror;
        return;
    case Ph::Mirror: {
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
        g_c.phaseMs = now;
        g_c.ph = Mode() == 2 ? Ph::Props : Ph::Watch;   // arms 1 and 3: the host does the work, this peer reads
        return;
    }
    case Ph::Props: {
        std::string rest;
        if (!Rest(kPropsPrefix, &rest)) return;   // the host has not named its loose props
        g_c.eids = ParseEids(rest);
        if (g_c.eids.empty()) { Abandon("the host named no loose prop"); return; }
        SetWatch(g_c.eids);
        UE_LOGI("[SACK-DRILL] client READY %zu loose props to put", g_c.eids.size());
        g_c.phaseMs = now;
        g_c.ph = Ph::Route;
        return;
    }
    case Ph::Route:
        if (void* const player = coop::players::Registry::Get().Local()) RouteTick(player);
        return;
    case Ph::Watch: {
        if (Mode() >= 2) CensusStep(now);
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
        g_c.ph = Ph::Done;
        return;
    }
    case Ph::Done:
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
