// coop/dev/creature_hit_drill.cpp -- see coop/dev/creature_hit_drill.h.

// The steps, each ending on a line the peer prints; every wait is bounded to 60 s from when it can hold:
//   1. HOST, once slot 1 is world-ready and its bracket closed: spawns the arm's creature 300 cm along the puppet's
//      yaw. While the drill runs it refills its vitals each second and as a hit is delivered and, on the zombie arm,
//      clears the stuck-count timer every second.
//   2. INBOUND. The client passes once three hits were applied and none came from the mirror's own copy (the
//      fossilhound arm also passes its perception); the host once three were delivered. The client posts
//      "inbound done".
//   3. OUTBOUND. The client empties its hand, the host posts "hand empty seen", the client calls the mirror's
//      addDamage once and the host must refuse it. The client takes an axe, the host posts "axe seen", and the
//      client calls the mirror's addDamage every 600 ms until the mirror is destroyed, turning its own view toward the
//      mirror (the control rotation only; the player does not move) before each swing, so the host's facing bound
//      sees it face the creature. The host passes when its creature died of judged hits and posts "host done"; the
//      client passes on the destroy.
//   4. DONE: the client's after its outbound PASS and the host's line, the host's after its own.

#include "coop/dev/creature_hit_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/creature_hit_intent.h"
#include "coop/creatures/npc_mirror_brain.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_hand.h"
#include "coop/dev/restore_vitals.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/player_damage.h"
#include "coop/player/player_hit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/creature_event_relay.h"

#include "ue_wrap/actors/fossilhound.h"
#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/weapon_catalog.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::dev::creature_hit_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace EL = coop::element;

// An arm: the config value, the creature's class (the NPC allowlist's own spelling), the asset path it is loaded from
// when the object index does not hold the class yet, whether its event is computed per viewer, and whether the
// zombie's stuck-count timer must be held off.
struct Arm {
    const char*    name;
    const wchar_t *cls, *path;
    bool           perViewer, stuckTimer, bodyFacing;
};
constexpr Arm kArms[] = {
    {"zombie", P::name::NpcClass_Zombie, L"/Game/objects/npc/npc_zombie.npc_zombie_C", false, true, false},
    {"fossilhound", P::name::NpcClass_Fossilhound, L"/Game/objects/fossilhound.fossilhound_C", true, false, true},
};

constexpr long long kBoundMs = 60000;
constexpr long long kSwingGapMs = 600;     // a player's swing rate, inside the intent queue's budget of 2 a second
constexpr long long kClearGapMs = 1000;
constexpr long long kRestoreGapMs = 1000;
constexpr float kSpawnAheadCm = 300.f;
constexpr uint32_t kInboundNeeded = 3;
constexpr const wchar_t* kStuckTimer = L"timer_checkForStuck";
constexpr const wchar_t* kAxeItem = L"axe";
constexpr const wchar_t* kAxeClass = L"prop_axe_C";
constexpr const char* kNoSwinging = "no swinging item";

constexpr std::string_view kInboundDone = "chd: inbound done";
constexpr std::string_view kHandEmpty   = "chd: hand empty seen";
constexpr std::string_view kNegDone     = "chd: negative done";
constexpr std::string_view kAxeSeen     = "chd: axe seen";
constexpr std::string_view kHostDone    = "chd: host done";

std::atomic<coop::net::Session*> g_session{nullptr};

const Arm* ArmOf() {
    static const Arm* s = []() -> const Arm* {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::creature_hit_drill);
        for (const Arm& a : kArms)
            if (v == a.name) return &a;
        return nullptr;
    }();
    return s;
}

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Expired(long long sinceMs) { return NowMs() - sinceMs >= kBoundMs; }

// One verdict line: the tag, PASS or FAIL, the step, the role, then the readings (a FAIL's end with its reason).
void Verdict(bool pass, const char* step, bool host, const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[CHD] PASS %s role=%s %s", step, host ? "host" : "client", buf);
    else      UE_LOGW("[CHD] FAIL %s role=%s %s", step, host ? "host" : "client", buf);
}

// Whether a chat row (either tier) holds `needle`.
bool HasRow(std::string_view needle) {
    static std::string_view s_needle;
    static bool s_found = false;
    static const std::function<void(const coop::chat_feed::RowView&)> fn = [](const coop::chat_feed::RowView& r) {
        if (r.line.find(s_needle) != std::string_view::npos) s_found = true;
    };
    s_needle = needle;
    s_found = false;
    coop::chat_feed::ForEachRow(fn);
    return s_found;
}

void Say(std::string_view line) { coop::chat_sync::QueueSend(std::string(line)); }

// K2_ClearTimer(Object, FunctionName) on KismetSystemLibrary's default object, the name an FString whose count and
// capacity include the terminator.
void ClearTimer(void* actor, const wchar_t* fn) {
    static void* s_ksl = nullptr;
    static void* s_clear = nullptr;
    if (!s_ksl) s_ksl = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (s_ksl && !s_clear) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary")) s_clear = R::FindFunction(kc, L"K2_ClearTimer");
    }
    if (!s_ksl || !s_clear) {
        UE_LOGW("[CHD] K2_ClearTimer unresolved -- the creature's timer is NOT cleared");
        return;
    }
    ue_wrap::ParamFrame f(s_clear);
    if (!f.valid()) return;
    f.Set<void*>(L"Object", actor);
    const int32_t num = static_cast<int32_t>(std::wcslen(fn)) + 1;
    struct { const wchar_t* data; int32_t n; int32_t m; } fs{fn, num, num};
    if (!f.SetRaw(L"FunctionName", &fs, sizeof(fs))) return;
    ue_wrap::Call(s_ksl, f);
}

// ---- the host -----------------------------------------------------------------------------------
enum class HostStep { Wait, WaitPuppet, Inbound, AwaitClient, Negative, AwaitAxe, Outbound, Done };

struct HostState {
    HostStep              step = HostStep::Wait;
    long long             stepMs = 0;
    long long             clearMs = 0;
    long long             restoreMs = 0;
    ue_wrap::CachedObjRef creature;
    uint32_t              delivered0 = 0, deliveredSeen = 0;
} g_h;

void HostTo(HostStep s) { g_h.step = s; g_h.stepMs = NowMs(); }

void HostStop() { g_h.step = HostStep::Done; }

// Slot 1's puppet actor once it is live, else null: a slot's world-ready and its bracket's close come before the
// puppet's spawn can be counted on, so the spawn step waits for it.
void* LivePuppet() {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
    void* puppet = rp ? rp->GetActor() : nullptr;
    return (puppet && R::IsLive(puppet)) ? puppet : nullptr;
}

void HostSpawn(const Arm& arm, void* puppet) {
    ue_wrap::FVector at{};
    ue_wrap::FRotator facing{};
    void* cls = ue_wrap::object_index::ClassByName(arm.cls);
    if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(arm.path);
    if (!cls || !puppet || !E::TryGetActorLocation(puppet, at) || !E::TryGetActorRotation(puppet, facing)) {
        Verdict(false, "spawn", true, "class=%d puppet=%d reason=the class or the puppet's pose could not be read",
                cls ? 1 : 0, puppet ? 1 : 0);
        HostStop();
        return;
    }
    const float yaw = facing.Yaw * 0.01745329252f;
    at.X += std::cos(yaw) * kSpawnAheadCm;
    at.Y += std::sin(yaw) * kSpawnAheadCm;
    void* creature = coop::dev::spawn_npc::SpawnAt(arm.cls, at);
    const EL::ElementId eid = creature ? EL::Registry::Get().EidForActor(creature) : EL::kInvalidId;
    if (eid == EL::kInvalidId) {
        Verdict(false, "spawn", true, "spawned=%d reason=the creature was not made or not enrolled as an Npc element",
                creature ? 1 : 0);
        HostStop();
        return;
    }
    g_h.creature.Set(creature);
    g_h.delivered0 = g_h.deliveredSeen = coop::player_damage::DeliveredCount();
    UE_LOGI("[CHD] host spawned eid=%u", eid);
    HostTo(HostStep::Inbound);
}

// While the drill runs: the host refills its vitals (and so the client's) each time a hit is delivered and once a
// second besides, since the creature also bites the host, which stands 25 cm from the client's puppet, and the hits on
// the host are not deliveries. The zombie's stuck count, which kills one that stands still to attack, is held off.
void HostKeepAlive(const Arm& arm, long long now) {
    const uint32_t d = coop::player_damage::DeliveredCount();
    if (d > g_h.deliveredSeen || now - g_h.restoreMs >= kRestoreGapMs) {
        g_h.restoreMs = now;
        coop::dev::restore_vitals::Restore();
    }
    g_h.deliveredSeen = d;
    if (arm.stuckTimer && now - g_h.clearMs >= kClearGapMs) {
        g_h.clearMs = now;
        if (void* z = g_h.creature.Get()) ClearTimer(z, kStuckTimer);
    }
}

void HostTick(coop::net::Session* s, const Arm& arm) {
    if (g_h.step == HostStep::Done) return;
    if (g_h.step == HostStep::Wait) {
        if (s->IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1)) HostTo(HostStep::WaitPuppet);
        return;
    }
    if (g_h.step == HostStep::WaitPuppet) {
        if (void* puppet = LivePuppet()) {
            HostSpawn(arm, puppet);
        } else if (Expired(g_h.stepMs)) {
            Verdict(false, "spawn", true, "puppet=0 reason=slot 1's puppet never became live");
            HostStop();
        }
        return;
    }
    HostKeepAlive(arm, NowMs());
    switch (g_h.step) {
    case HostStep::Inbound: {
        const uint32_t sent = coop::player_damage::DeliveredCount() - g_h.delivered0;
        if (sent >= kInboundNeeded) {
            Verdict(true, "inbound", true, "sent=%u", sent);
            HostTo(HostStep::AwaitClient);
        } else if (Expired(g_h.stepMs)) {
            Verdict(false, "inbound", true, "sent=%u reason=bound", sent);
            HostStop();
        }
        return;
    }
    case HostStep::AwaitClient: {
        const bool line = HasRow(kInboundDone);
        const std::wstring held = coop::hand_item::HeldItem(1);
        if (line && held.empty()) {
            Say(kHandEmpty);
            HostTo(HostStep::Negative);
        } else if (Expired(g_h.stepMs)) {
            Verdict(false, "negative", true, "line=%d held=%ls reason=bound", line ? 1 : 0, held.c_str());
            HostStop();
        }
        return;
    }
    case HostStep::Negative: {
        const uint32_t refused = coop::creature_hit_intent::RefusedCount(1, kNoSwinging);
        if (refused >= 1) {
            Verdict(true, "negative", true, "refused=%u", refused);
            Say(kNegDone);  // the client takes its axe only now, so the refused hit is judged empty-handed
            HostTo(HostStep::AwaitAxe);
        } else if (Expired(g_h.stepMs)) {
            Verdict(false, "negative", true, "refused=%u reason=bound", refused);
            HostStop();
        }
        return;
    }
    case HostStep::AwaitAxe: {
        const std::wstring held = coop::hand_item::HeldItem(1);
        if (held == kAxeItem) {
            Say(kAxeSeen);
            HostTo(HostStep::Outbound);
        } else if (Expired(g_h.stepMs)) {
            Verdict(false, "outbound", true, "held=%ls reason=the axe was not seen", held.c_str());
            HostStop();
        }
        return;
    }
    case HostStep::Outbound: {
        const uint32_t applied = coop::creature_hit_intent::AppliedCount();
        if (!g_h.creature.Get()) {
            if (applied >= 1) {
                Verdict(true, "outbound", true, "applied=%u", applied);
                Say(kHostDone);
                UE_LOGI("[CHD] DONE role=host refused_sends=%u", coop::player_hit::RefusedSendCount());
            } else {
                Verdict(false, "outbound", true, "applied=%u reason=died of no judged hit", applied);
            }
            HostStop();
        } else if (Expired(g_h.stepMs)) {
            Verdict(false, "outbound", true, "applied=%u alive=1 reason=bound", applied);
            HostStop();
        }
        return;
    }
    default:
        return;
    }
}

// ---- the client ---------------------------------------------------------------------------------
enum class ClientStep { WaitJoin, FindMirror, Inbound, AwaitHandEmpty, AwaitNegative, AwaitAxe, Swing, PerceptionEnd,
                        AwaitHostDone, Done };

struct ClientState {
    ClientStep            step = ClientStep::WaitJoin;
    long long             stepMs = 0;
    long long             swingMs = 0;
    ue_wrap::CachedObjRef mirror;
    EL::ElementId         eid = EL::kInvalidId;
    float                 damage = 0.f;
    uint32_t              hits = 0;
    bool                  inboundPassed = false, perceived = false, swingFaulted = false, turnFaulted = false;
} g_c;
std::vector<EL::Npc*> g_scratch;

// The client's verdict on a creature whose body turns apart from its root (the hound): its mirror's mesh world yaw
// must leave the first sample's by more than kTurnDeg in kTurnsNeeded samples, or the body never turned.
constexpr float kTurnDeg = 30.f;
constexpr int kTurnsNeeded = 3;
struct FacingState {
    bool  have = false, passed = false;
    float firstYaw = 0.f;
    int   turned = 0;
} g_face;

void ClientTo(ClientStep s) { g_c.step = s; g_c.stepMs = NowMs(); }

void ClientStop() { g_c.step = ClientStep::Done; }

// The mirror of the arm's creature, by a bounded scan of the NPC mirrors while the step waits.
bool FindMirror(const Arm& arm) {
    EL::NpcMirrors().Snapshot(g_scratch);
    for (EL::Npc* n : g_scratch) {
        void* actor = n ? n->LiveActor() : nullptr;
        if (!actor || R::ClassNameOf(actor) != arm.cls) continue;
        g_c.mirror.Set(actor);
        g_c.eid = n->GetId();
        void* ctrl = E::GetController(actor);
        const std::wstring cn = ctrl ? R::ClassNameOf(ctrl) : std::wstring();
        UE_LOGI("[CHD] mirror controller=%ls", cn.empty() ? L"none" : cn.c_str());
        return true;
    }
    return false;
}

bool MirrorGone() { return !g_c.mirror.Get() || EL::NpcMirrors().Get(g_c.eid) == nullptr; }

// The call mainPlayer_C::attack makes on the struck actor: the mirror's addDamage, this player the damager, the hit
// at the mirror's location, no impact.
bool CallAddDamage() {
    void* mirror = g_c.mirror.Get();
    void* me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!mirror || !me || !E::TryGetActorLocation(mirror, at)) return false;
    return ue_wrap::int_objects::InvokeAddDamage(mirror, me, g_c.damage, at, ue_wrap::FVector{});
}

// Turn this player's own view toward the mirror: the controller's control rotation set to the look-at rotation from
// its camera to the mirror's location, which is the point CallAddDamage claims. The host's facing bound reads the
// aim the pose stream carries, so the swing is made facing the creature. The player is not moved.
void FaceMirror() {
    void* mirror = g_c.mirror.Get();
    void* me = coop::players::Registry::Get().Local();
    void* ctrl = me ? E::GetController(me) : nullptr;
    ue_wrap::FVector at{};
    if (mirror && ctrl && E::TryGetActorLocation(mirror, at)) {
        E::SetControlRotation(ctrl, coop::director::LookAt(E::GetCameraLocation(), at));
    } else if (!g_c.turnFaulted) {
        g_c.turnFaulted = true;
        UE_LOGW("[CHD] the client's view could not be turned toward the mirror");
    }
}

void ClientInbound(const Arm& arm) {
    const uint32_t a = coop::player_hit::AppliedCount();
    const uint32_t m = coop::player_damage::MirrorHitCount();
    if (m > 0) {
        Verdict(false, "inbound", false, "applied=%u mirror_hits=%u reason=mirror hit", a, m);
        ClientStop();
        return;
    }
    if (arm.perViewer && !g_c.perceived) {
        const uint32_t t = coop::creature_event_relay::PerViewerTrues(g_c.eid);
        if (t >= 1) {
            g_c.perceived = true;
            Verdict(true, "perception", false, "trues=%u", t);
        }
    }
    if (!g_c.inboundPassed && a >= kInboundNeeded) {
        g_c.inboundPassed = true;
        Verdict(true, "inbound", false, "applied=%u mirror_hits=%u", a, m);
    }
    if (g_c.inboundPassed && (!arm.perViewer || g_c.perceived)) {
        Say(kInboundDone);
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr)) {
            Verdict(false, "outbound", false, "hits=0 reason=the hand could not be emptied");
            ClientStop();
            return;
        }
        ClientTo(ClientStep::AwaitHandEmpty);
    } else if (Expired(g_c.stepMs)) {
        if (!g_c.inboundPassed)
            Verdict(false, "inbound", false, "applied=%u mirror_hits=%u reason=bound", a, m);
        else
            Verdict(false, "perception", false, "trues=%u reason=bound",
                    coop::creature_event_relay::PerViewerTrues(g_c.eid));
        ClientStop();
    }
}

void ClientTick(const Arm& arm) {
    switch (g_c.step) {
    case ClientStep::WaitJoin:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        ClientTo(ClientStep::FindMirror);
        return;
    case ClientStep::FindMirror:
        if (FindMirror(arm)) {
            ClientTo(ClientStep::Inbound);
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "inbound", false, "applied=0 mirror_hits=0 mirror=0 reason=bound");
            ClientStop();
        }
        return;
    case ClientStep::Inbound:
        ClientInbound(arm);
        return;
    case ClientStep::AwaitHandEmpty:
        if (HasRow(kHandEmpty)) {
            ue_wrap::weapon_catalog::Swing sw;
            if (!ue_wrap::weapon_catalog::Lookup(kAxeItem, sw) || !sw.canSwing || !(sw.maxDamage > 0.f)) {
                Verdict(false, "outbound", false, "hits=0 reason=the axe's row could not be read");
                ClientStop();
                return;
            }
            g_c.damage = sw.maxDamage;
            // The call's own result is not judged here: the host's negative step reads whether it arrived.
            FaceMirror();
            if (!CallAddDamage()) UE_LOGW("[CHD] the empty-hand call of the mirror's addDamage did not run");
            ClientTo(ClientStep::AwaitNegative);
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "outbound", false, "hits=0 reason=the host's hand line did not come");
            ClientStop();
        }
        return;
    case ClientStep::AwaitNegative:
        // The host judged the empty-handed hit; only now does the hand take the axe.
        if (HasRow(kNegDone)) {
            if (!coop::dev::drill_hand::TakeIntoHandNow(kAxeItem, kAxeClass)) {
                Verdict(false, "outbound", false, "hits=0 reason=the axe could not be taken");
                ClientStop();
                return;
            }
            ClientTo(ClientStep::AwaitAxe);
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "outbound", false, "hits=0 reason=the host's negative line did not come");
            ClientStop();
        }
        return;
    case ClientStep::AwaitAxe:
        if (HasRow(kAxeSeen)) {
            ClientTo(ClientStep::Swing);
            g_c.swingMs = g_c.stepMs - kSwingGapMs;
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "outbound", false, "hits=0 reason=the host's axe line did not come");
            ClientStop();
        } else {
            FaceMirror();   // the pose stream carries the aim before the first swing
        }
        return;
    case ClientStep::Swing: {
        const long long now = NowMs();
        if (MirrorGone()) {
            Verdict(true, "outbound", false, "hits=%u", g_c.hits);
            ClientTo(arm.perViewer ? ClientStep::PerceptionEnd : ClientStep::AwaitHostDone);
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "outbound", false, "hits=%u reason=bound", g_c.hits);
            ClientStop();
        } else {
            // Every tick, so the streamed aim follows the creature as it walks; the swing's own turn is the last.
            FaceMirror();
            if (now - g_c.swingMs >= kSwingGapMs) {
                g_c.swingMs = now;
                if (CallAddDamage()) ++g_c.hits;
                else if (!g_c.swingFaulted) {
                    g_c.swingFaulted = true;
                    UE_LOGW("[CHD] the mirror's addDamage call did not run");
                }
            }
        }
        return;
    }
    case ClientStep::PerceptionEnd: {
        const uint32_t t = coop::creature_event_relay::PerViewerTrues(g_c.eid);
        const uint32_t f = coop::creature_event_relay::PerViewerFalses(g_c.eid);
        if (t >= 1 && f == t) {
            Verdict(true, "perception-end", false, "trues=%u falses=%u", t, f);
            ClientTo(ClientStep::AwaitHostDone);
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "perception-end", false, "trues=%u falses=%u reason=bound", t, f);
            ClientStop();
        }
        return;
    }
    case ClientStep::AwaitHostDone:
        if (HasRow(kHostDone)) {
            if (arm.bodyFacing && !g_face.passed)
                Verdict(false, "facing", false, "turned=%d reason=the mirror's body never turned", g_face.turned);
            UE_LOGI("[CHD] DONE role=client adopted_kick_skips=%u", coop::npc_mirror_brain::AdoptedKickSkips());
            ClientStop();
        } else if (Expired(g_c.stepMs)) {
            Verdict(false, "done", false, "hits=%u reason=the host's done line did not come", g_c.hits);
            ClientStop();
        }
        return;
    case ClientStep::Done:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != nullptr; }

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

// The hound's facing on each peer every 500 ms -- the actor's yaw, its velocity, the SpringArm's world rotation (the
// graph's tick turns it toward the velocity, and it carries the mesh) and the mesh's world yaw -- so the host's body
// facing and the mirror's compare line by line. On the client it is also the facing verdict's sample (g_face).
void FacingProbe(bool host) {
    static long long lastMs = 0;
    const long long now = NowMs();
    if (now - lastMs < 500) return;
    lastMs = now;
    void* hound = host ? g_h.creature.Get() : g_c.mirror.Get();
    if (!hound || !R::IsLive(hound)) return;
    ue_wrap::FRotator rot{};
    E::TryGetActorRotation(hound, rot);
    const ue_wrap::FVector v = E::GetActorVelocity(hound);
    const float speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    const float velYaw = std::atan2(v.Y, v.X) * 57.2957795f;
    void* arm = ue_wrap::fossilhound::SpringArm(hound);
    const ue_wrap::FRotator armRot = arm ? E::GetComponentWorldRotation(arm) : ue_wrap::FRotator{};
    void* mesh = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(hound) + P::off::ACharacter_Mesh);
    const ue_wrap::FRotator meshRot = (mesh && R::IsLive(mesh)) ? E::GetComponentWorldRotation(mesh) : ue_wrap::FRotator{};
    UE_LOGI("[CHD] facing role=%s actorYaw=%.1f speed=%.0f velYaw=%.1f velZ=%.0f arm=%d armPitch=%.1f armYaw=%.1f "
            "meshPitch=%.1f meshYaw=%.1f meshRoll=%.1f", host ? "host" : "client", rot.Yaw, speed, velYaw, v.Z, arm ? 1 : 0,
            armRot.Pitch, armRot.Yaw, meshRot.Pitch, meshRot.Yaw, meshRot.Roll);
    if (host || !mesh || !R::IsLive(mesh) || g_face.passed) return;
    if (!g_face.have) {
        g_face = FacingState{true, false, meshRot.Yaw, 0};
        return;
    }
    const float d = std::remainder(meshRot.Yaw - g_face.firstYaw, 360.f);
    if (std::fabs(d) > kTurnDeg && ++g_face.turned >= kTurnsNeeded) {
        g_face.passed = true;
        Verdict(true, "facing", false, "turned=%d last=%.1f first=%.1f", g_face.turned, meshRot.Yaw, g_face.firstYaw);
    }
}

void Tick() {
    const Arm* arm = ArmOf();
    if (!arm) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (host) HostTick(s, *arm);
    else ClientTick(*arm);
    if (arm->bodyFacing) FacingProbe(host);
}

void OnDisconnect() {
    g_c = ClientState{};
    g_face = FacingState{};
}

}  // namespace coop::dev::creature_hit_drill
