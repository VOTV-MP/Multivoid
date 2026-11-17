// coop/props/glowstick_stick_judge.cpp -- see coop/props/glowstick_stick_judge.h.

#include "coop/props/glowstick_stick_judge.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/props/glowstick_stick_record.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_record_gates.h"
#include "coop/props/prop_save_data.h"
#include "remote_prop_drive.h"   // impl-private (src-local): g_holds, the hold lane's own record of what a slot held last

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace coop::glowstick_stick_judge {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace SK = coop::glowstick_stick_record;
using ue_wrap::FVector;

// The bounds, set from the lines this judge logged for every honest stick of the drill's static, grab, attached and joiner
// arms: loc-to-copy 28.0 to 83.9 (ray 5), rel1 28.6 to 33.6, rel2 35.0, xform-to-loc 28.6 to 35.0, quat 1.000, scale 1.00,
// the struck prop's bounds 0.0 from the point. Every one of those sticks struck a keyed prop; no stick to world geometry
// was measured before the `world` arm, and none of the frame bounds below applies to one.
//
// The point's distance from the host's copy over the glowstick's own ray. The copy is a delayed puppet of the holder's hand
// or of a thrown glowstick, so the gap is latency times speed: a WAN link's pose delay and the drive's interpolation
// (about 150 + 100 ms, and a lost pose at 30 Hz adds 33) at a glowstick thrown at 1500 cm/s is about 400 cm; the LAN rig
// measured 79 cm over the ray. 600 holds that with a margin (MTA's own cut-offs sit an order above human reach: 50 m to
// enter a vehicle, CGame.cpp:3115). What binds a hostile point is not this radius but the surface test of a static
// stick, the struck prop's bounds of an attached one, and one stick per hold.
constexpr float kReachSlackCm = 600.f;
// relLoc1, relLoc2 and the struck component's origin from the point: the stick point in the struck body's frame, as large as
// the body (a vehicle's several metres); 35 was measured only on small props. 1000 refuses the absurd values a hostile
// record sends (1e5 and up) and no real prop's. Asked of an ATTACHED stick only: a static stick's restore overwrites them.
constexpr float kFrameMaxCm = 1000.f;
// The struck prop's colliding bounds from the point, over the ray: 0.0 measured; the prop may have moved since the stick
// (a vehicle at 1000 cm/s, 100 ms of lag is 100 cm).
constexpr float kStruckSlackCm = 100.f;
// The transform of an attached stick is a game transform: a unit quaternion (1.000 measured) and a scale near unit (1.00).
constexpr float kQuatNormSlack = 0.05f;
constexpr float kScaleMin = 0.01f, kScaleMax = 100.f;
// A static stick's point lies on a surface: a short trace from it along each axis meets world geometry within this (the
// ray is 5 cm and a sphere of 1), so a static stick cannot be walked through the air (MTA M5; the point is the client's claim).
constexpr float kSurfaceCm = 10.f;

coop::net::Session* g_session = nullptr;
bool g_registered = false;
uint32_t g_judged = 0, g_refused = 0, g_corrected = 0;
// The stick this judge accepted from each slot, by the hold it was made in: one stick per hold (a stick drops the glowstick
// from the hand, so a second one needs a new grab), and the same record asked again (it parks, then lands) passes unchanged.
struct Accepted {
    bool have = false;
    uint16_t gen = 0;
    std::wstring key;
    SK::StickFields fields;
};
std::array<Accepted, coop::players::kMaxPeers> g_accepted;

std::wstring KeyOf(void* actor) { return ue_wrap::prop::GetInteractableKeyString(actor); }

// The glowstick's own ray length, read off the actor (the fs, kelj and ghost variants override it); -1 when unread.
float RayOf(void* actor) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(actor), L"length");
    if (off < 0) return -1.f;
    float v = 0.f;
    std::memcpy(&v, static_cast<const uint8_t*>(actor) + off, sizeof(v));
    return v;
}

float Len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float Dist3(const float* a, const FVector& b) {
    const float d[3] = {a[0] - b.X, a[1] - b.Y, a[2] - b.Z};
    return Len3(d);
}

// Whether the sender's most recent hold of a prop was the glowstick keyed `key`.
bool HeldLast(int slot, const std::wstring& key) {
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return false;
    const coop::net::WireKey& k = coop::remote_prop::g_holds[slot].driveKey;
    if (k.len == 0 || k.len != key.size()) return false;
    for (uint8_t i = 0; i < k.len; ++i)
        if (static_cast<wchar_t>(static_cast<unsigned char>(k.data[i])) != key[i]) return false;
    return true;
}
// The same stick for this judge: the same point on the same struck prop (the frames can differ by the frame of the capture).
bool SamePoint(const SK::StickFields& a, const SK::StickFields& b) {
    return a.attachKey == b.attachKey && a.stickStatic == b.stickStatic &&
           Dist3(a.loc, FVector{b.loc[0], b.loc[1], b.loc[2]}) < 1.f;
}

// The hold generation the slot's last drive belongs to; 0 when the hold is unversioned.
uint16_t HoldGen(int slot) {
    return slot >= 0 && slot < static_cast<int>(coop::players::kMaxPeers) ? coop::remote_prop::g_holds[slot].driveGen : 0;
}

// Whether a short trace from `p` along each axis meets world geometry: the point lies on a surface.
bool NearSurface(void* actor, const float* p) {
    static const uint8_t types[4] = {0, 1, 3, 4};   // WorldStatic, WorldDynamic, PhysicsBody, Vehicle: the glowstick's own ray's set
    const FVector c{p[0], p[1], p[2]};
    const float d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& a : d) {
        ue_wrap::trace::Hit h;
        if (ue_wrap::trace::LineHitForObjects(actor, c, FVector{c.X + a[0] * kSurfaceCm, c.Y + a[1] * kSurfaceCm, c.Z + a[2] * kSurfaceCm},
                                              types, 4, nullptr, 0, &h) && h.blocked)
            return true;
    }
    return false;
}

bool CopyStuck(void* actor) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(actor), L"sticked", off, mask) &&
           (static_cast<const uint8_t*>(actor)[off] & mask) != 0;
}

// The distance from `p` to the nearest point of the actor's colliding bounds; -1 when they cannot be read.
float BoxDist(void* actor, const float* p) {
    FVector c, e;
    if (!ue_wrap::engine::GetActorBounds(actor, true, c, e) || (e.X == 0.f && e.Y == 0.f && e.Z == 0.f)) return -1.f;   // no colliding bounds reads as an empty box at the origin
    const float d[3] = {std::max(0.f, std::fabs(p[0] - c.X) - e.X), std::max(0.f, std::fabs(p[1] - c.Y) - e.Y),
                        std::max(0.f, std::fabs(p[2] - c.Z) - e.Z)};
    return Len3(d);
}

// The glowstick's own record, to the sender alone: the client's copy is released by the host's truth.
void Correct(void* actor, uint8_t slot, const std::wstring& key) {
    static const bool s_off = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_correct_off);
    if (!g_session) return;
    if (s_off) {
        UE_LOGI("glowstick_judge: no record goes to slot %u to correct its copy of '%ls' (glowstick_correct_off)",
                static_cast<unsigned>(slot), key.c_str());
        return;
    }
    const bool sent = coop::prop_save_data::PublishToSlot(g_session, slot, actor, key);
    if (sent) ++g_corrected;
    UE_LOGI("glowstick_judge: the host's record of '%ls' goes to slot %u to correct its copy (sent %d)", key.c_str(),
            static_cast<unsigned>(slot), sent ? 1 : 0);
}

bool Refuse(void* actor, uint8_t slot, const std::wstring& key, const char* why) {
    ++g_refused;
    UE_LOGW("glowstick_judge: slot %u's stick of '%ls' is refused -- %s", static_cast<unsigned>(slot), key.c_str(), why);
    Correct(actor, slot, key);
    return false;
}

bool Judge(void* actor, uint8_t slot, const SR::SaveRecord& rec) {
    const std::wstring key = KeyOf(actor);
    SK::StickFields f;
    const SK::Read rd = SK::ReadStick(rec, f);
    if (rd == SK::Read::Malformed) return Refuse(actor, slot, key, "the record is not a glowstick's layout");
    if (rd == SK::Read::NonFinite) return Refuse(actor, slot, key, "a field of the stick is not finite");
    if (!f.sticked) {
        // A release is a grab's: the host frees its copy at the hold's begin and publishes it. A free record that would
        // take a stuck copy off its point is no honest client's, whoever it claims to be.
        if (CopyStuck(actor)) return Refuse(actor, slot, key, "a client's release of a stuck glowstick (a grab's act)");
        return true;
    }

    // One stick per hold, and a record asked again (it parked, then lands) is the same stick: judged once, said once.
    const uint16_t gen = HoldGen(slot);
    Accepted* acc = slot < g_accepted.size() ? &g_accepted[slot] : nullptr;
    const bool sameHold = acc && acc->have && gen != 0 && acc->gen == gen && acc->key == key;
    if (sameHold && SamePoint(acc->fields, f)) return true;

    ++g_judged;
    static const bool s_refuseAll = coop::config::ResolveFlag(coop::config_registry::rows::glowstick_stick_refuse_all);
    if (s_refuseAll) return Refuse(actor, slot, key, "every stick is refused (glowstick_stick_refuse_all)");
    char why[160] = {};
    size_t used = 0;
    auto note = [&](const char* s) { used += static_cast<size_t>(std::snprintf(why + used, sizeof(why) - used, "%s ", s)); };

    const bool held = HeldLast(slot, key);
    if (!held) note("not-its-last-hold");
    else if (sameHold) note("second-stick-of-one-hold");

    FVector copy{};
    const bool haveCopy = ue_wrap::engine::TryGetActorLocation(actor, copy);
    const float ray = RayOf(actor);
    const float dLoc = haveCopy ? Dist3(f.loc, copy) : -1.f;
    if (dLoc < 0.f || ray < 0.f || dLoc > ray + kReachSlackCm) note("point-far-from-copy");

    // An attached stick's frames feed the host's constraint (relLoc1/2 and the transform); a static one's restore overwrites
    // them from the hook, so a stick to world geometry, whose component's origin and scale are the level's, is judged by
    // its point alone.
    const bool attached = !f.stickStatic && !f.attachKey.empty();
    const float rel1 = Len3(f.rel1), rel2 = Len3(f.rel2);
    const float xlate = Dist3(f.xform + 4, FVector{f.loc[0], f.loc[1], f.loc[2]});
    const float quat = std::sqrt(f.xform[0] * f.xform[0] + f.xform[1] * f.xform[1] + f.xform[2] * f.xform[2] +
                                 f.xform[3] * f.xform[3]);
    const float sMin = std::min({f.xform[7], f.xform[8], f.xform[9]});
    const float sMax = std::max({f.xform[7], f.xform[8], f.xform[9]});
    if (attached) {
        if (rel1 > kFrameMaxCm || rel2 > kFrameMaxCm || xlate > kFrameMaxCm) note("frame-out-of-bounds");
        if (std::fabs(quat - 1.f) > kQuatNormSlack || sMin < kScaleMin || sMax > kScaleMax) note("transform-not-a-game-transform");
    }

    float originD = -1.f, boxD = -1.f;
    const char* struck = "none";
    if (!f.attachKey.empty()) {
        // The prop the stick names must be one the host holds in its prop index. A keyed actor outside it (a vehicle) is
        // refused and counted here, and the sender's copy is corrected, rather than recorded as a static stick.
        void* st = coop::prop_key_index::Find(f.attachKey);
        if (!st) {
            struck = "absent";
            note("struck-key-not-live");
        } else {
            struck = "live";
            FVector so{};
            if (ue_wrap::engine::TryGetActorLocation(st, so)) originD = Dist3(f.loc, so);
            boxD = BoxDist(st, f.loc);
            // A prop whose bounds do not read is judged by its origin against the frame bound.
            if (boxD >= 0.f ? boxD > ray + kStruckSlackCm : originD < 0.f || originD > kFrameMaxCm) note("struck-far-from-point");
        }
    } else if (!NearSurface(actor, f.loc)) {
        struck = "air";
        note("static-point-not-on-a-surface");
    }

    UE_LOGI("glowstick_judge: slot %u '%ls' stick at (%.0f,%.0f,%.0f) static=%d key='%ls' held-last=%d loc-to-copy=%.1f "
            "ray=%.1f rel1=%.1f rel2=%.1f xform-to-loc=%.1f quat=%.3f scale=%.2f..%.2f struck=%s origin=%.1f box=%.1f -- %s%s",
            static_cast<unsigned>(slot), key.c_str(), f.loc[0], f.loc[1], f.loc[2], f.stickStatic ? 1 : 0, f.attachKey.c_str(),
            held ? 1 : 0, dLoc, ray, rel1, rel2, xlate, quat, sMin, sMax, struck, originD, boxD, used ? "refused: " : "passes", why);
    if (used) return Refuse(actor, slot, key, why);
    if (acc) {
        acc->have = true;
        acc->gen = gen;
        acc->key = key;
        acc->fields = f;
    }
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session = session;
    if (g_registered) return;
    g_registered = true;
    coop::prop_record_gates::RegisterIntentRecordGate(L"prop_glowstick_C", &Judge);
}

void OnDisconnect() {
    if (g_judged || g_refused)
        UE_LOGI("glowstick_judge: this session judged %u stick(s), %u refused",
                g_judged, g_refused);
    g_judged = g_refused = g_corrected = 0;
    for (Accepted& a : g_accepted) a = Accepted{};
    g_session = nullptr;
}


namespace dev {
Counts Get() { return Counts{g_judged, g_refused, g_corrected}; }
}  // namespace dev

}  // namespace coop::glowstick_stick_judge
