// coop/dev/hookdrag_selftest.cpp -- see coop/dev/hookdrag_selftest.h.

#include "coop/dev/hookdrag_selftest.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"            // CollectHandAxisActors: the hand is not a world prop
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"  // FindLiveActorByKey: the tracked world props
#include "coop/props/remote_prop.h"           // ResolveMirrorEidByActor: the eid a client's copy carries
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"  // AddMovementInput: the walk is real input, never a teleport

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace coop::dev::hookdrag_selftest {
namespace {

namespace E  = ue_wrap::engine;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::hookdrag_selftest);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The schedule, from the first tick at which BOTH peers are in the world -- the shared origin the
// disc driver uses, and for the same reason: the two connect clocks sit about ten seconds apart.
constexpr uint64_t kPickMs       =  8000;   // both roles name the target
constexpr uint64_t kPlantMs      = 12000;   // the host plants the hook
constexpr uint64_t kWalkOutMs    = 14000;   // the host walks away from the prop ...
constexpr uint64_t kWalkBackMs   = 19000;   // ... and back
constexpr uint64_t kWalkLegMs    =  3000;
constexpr uint64_t kUnhookMs     = 25000;   // the hook is destroyed; the prop coasts and rests
constexpr uint64_t kVerdictMs    = 33000;
constexpr uint64_t kSampleMs     =   250;   // the position log, both roles
constexpr float    kPickRadiusCm =  1500.f;

uint64_t g_originMs     = 0;
uint64_t g_nextSampleMs = 0;
bool     g_picked = false, g_planted = false, g_walkOutStarted = false, g_walkBackStarted = false;
bool     g_unhooked = false, g_verdict = false;
ue_wrap::CachedObjRef g_target;
ue_wrap::CachedObjRef g_hook;
std::wstring          g_targetKey;
ue_wrap::FVector      g_targetStart{};
ue_wrap::FVector      g_walkBase{}, g_walkDir{};
float                 g_maxMovedCm = 0.f;
float                 g_goneAtS    = -1.f;   // when the target died under the sampler, or -1
const char*           g_plantNote  = "not attempted";

void* LocalPlayer() {
    void* p = coop::players::Registry::Get().Local();
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Where both roles look for the target: the host at its own player, the client at the host's
// puppet, which stands where the host player does.
bool Anchor(bool isHost, ue_wrap::FVector& out) {
    if (isHost) {
        void* p = LocalPlayer();
        if (!p) return false;
        out = E::GetActorLocation(p);
        return true;
    }
    coop::RemotePlayer* pup = coop::players::Registry::Get().Puppet(0);
    if (!pup || !pup->valid() || !pup->GetActor()) return false;
    out = E::GetActorLocation(pup->GetActor());
    return true;
}

// The nearest keyed, simulating, light Aprop_C to `anchor` within the radius: one object-array
// walk, once per run, the nearest-prop shape with the static, frozen and heavy props left out,
// since a hook drags none of them cleanly. Two more exclusions make it a WORLD prop: the hand
// axis (the local hotbar hand and every peer's display mirror stand at a player's hands and
// follow the player whether or not anything drags them), and membership in the tracked-prop key
// index, which is what the census gives a world prop and withholds from a hand actor. Both roles
// run the same rule over the same world, and the log carries the key so the driver can tell when
// they disagreed.
void* PickTarget(const ue_wrap::FVector& anchor, std::wstring& keyOut) {
    void* best = nullptr;
    float bestD2 = kPickRadiusCm * kPickRadiusCm;
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj || !PR::IsDescendantOfProp(obj)) continue;
        bool onHand = false;
        for (size_t h = 0; h < handAxisN; ++h) if (handAxis[h] == obj) { onHand = true; break; }
        if (onHand) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        if (PR::IsStatic(obj) || PR::IsFrozen(obj) || PR::IsHeavy(obj)) continue;
        const std::wstring key = PR::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") continue;
        if (coop::prop_element_tracker::FindLiveActorByKey(key) != obj) continue;  // not a tracked world prop
        const ue_wrap::FVector loc = E::GetActorLocation(obj);
        const float dx = loc.X - anchor.X, dy = loc.Y - anchor.Y, dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = obj; keyOut = key; }
    }
    return best;
}

// The item's own plant, by hand: a hook spawned at the prop, attach_a with the head on the prop's
// mesh and the tail on the player, then the field the item writes so the owner lane adopts it.
//
// The hook is spawned as a MIRROR, not a canonical one: a canonical hook is a save participant,
// and the game's own autosave and save-on-death would write a driver-planted one into the slot,
// where it outlives the run. A mirror is kept out of the save and has its tick disabled, and a
// drag needs neither: the constraint attach_a builds between the prop's mesh and the player's
// root is independent of both.
bool Plant(void* player, void* target) {
    void* mesh = PR::GetStaticMesh(target);
    if (!mesh) { g_plantNote = "no-static-mesh"; return false; }
    const ue_wrap::FVector loc = E::GetActorLocation(target);
    void* hook = H::SpawnMirror(H::Kind::Hook, loc, ue_wrap::FRotator{});
    if (!hook) { g_plantNote = "hook-did-not-spawn"; return false; }
    if (!H::AttachHead(hook, target, mesh, ue_wrap::FVector{loc.X, loc.Y, loc.Z + 20.f},
                       ue_wrap::FVector{0.f, 0.f, 1.f}, player, /*checkLen=*/true,
                       /*unfreezeFrozen=*/false)) {
        E::DestroyActor(hook);
        g_plantNote = "attach_a-did-not-dispatch";
        return false;
    }
    H::WriteActiveHook(player, hook);
    g_hook.Set(hook);
    g_plantNote = "planted";
    return true;
}

// The shared clock the two peers' samples are aligned on: both run on one machine, so the system
// clock is common to the millisecond, where each peer's own origin is a second apart.
uint64_t WallMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void Sample(uint64_t since, bool isHost) {
    void* t = g_target.Get();
    if (!t) {
        // Said once; the sampling stops with the target, and the verdict carries the time.
        g_goneAtS = static_cast<float>(since / 1000.0);
        UE_LOGW("hookdrag_selftest: POS t=%.2f key='%ls' -- the target is GONE; sampling stops",
                since / 1000.0, g_targetKey.c_str());
        return;
    }
    const ue_wrap::FVector loc = E::GetActorLocation(t);
    const float dx = loc.X - g_targetStart.X, dy = loc.Y - g_targetStart.Y, dz = loc.Z - g_targetStart.Z;
    const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (moved > g_maxMovedCm) g_maxMovedCm = moved;
    UE_LOGI("hookdrag_selftest: POS t=%.2f wall=%llu key='%ls' (%.1f,%.1f,%.1f) moved=%.1f role=%s",
            since / 1000.0, static_cast<unsigned long long>(WallMs()), g_targetKey.c_str(),
            loc.X, loc.Y, loc.Z, moved, isHost ? "HOST" : "CLIENT");
}

// One tick of the host's walk: movement INPUT, re-issued every tick, which the character movement
// consumes -- walls stop the player and the floor carries it, where a teleported step can land the
// player inside geometry.
void WalkStep(bool outward) {
    void* p = LocalPlayer();
    if (!p) return;
    const float s = outward ? 1.f : -1.f;
    E::AddMovementInput(p, ue_wrap::FVector{g_walkDir.X * s, g_walkDir.Y * s, 0.f}, 1.0f, true);
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
    // The subsystem install list is a per-tick ensure, so the line is said once.
    static bool sSaid = false;
    if (sSaid) return;
    sSaid = true;
    UE_LOGI("hookdrag_selftest: ENABLED -- the host plants a hook into the nearest keyed physics "
            "prop and drags it; both peers log the prop's position");
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const bool isHost = s->role() == coop::net::Role::Host;
    const uint64_t now = NowMs();
    if (!g_originMs) {
        // Only a CLIENT announces ClientWorldReady; the host learns the same fact as that slot
        // going world-ready.
        if (isHost ? !s->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
        g_originMs = now;
        UE_LOGI("hookdrag_selftest: ARMED role=%s (pick +%llus, plant +%llus, walk +%llus..+%llus, "
                "unhook +%llus, verdict +%llus)", isHost ? "HOST" : "CLIENT",
                static_cast<unsigned long long>(kPickMs / 1000),
                static_cast<unsigned long long>(kPlantMs / 1000),
                static_cast<unsigned long long>(kWalkOutMs / 1000),
                static_cast<unsigned long long>((kWalkBackMs + kWalkLegMs) / 1000),
                static_cast<unsigned long long>(kUnhookMs / 1000),
                static_cast<unsigned long long>(kVerdictMs / 1000));
    }
    const uint64_t since = now - g_originMs;
    if (!H::EnsureResolved()) return;

    if (!g_picked && since >= kPickMs) {
        g_picked = true;
        ue_wrap::FVector anchor{};
        if (!Anchor(isHost, anchor)) {
            UE_LOGW("hookdrag_selftest: PICK role=%s -- no anchor (the host %s is not here yet)",
                    isHost ? "HOST" : "CLIENT", isHost ? "player" : "puppet");
        } else if (void* t = PickTarget(anchor, g_targetKey)) {
            g_target.Set(t);
            g_targetStart = E::GetActorLocation(t);
            const float dx = g_targetStart.X - anchor.X, dy = g_targetStart.Y - anchor.Y,
                        dz = g_targetStart.Z - anchor.Z;
            // The eid is the cross-peer identity: the host's local element, the client's mirror
            // row from the join snapshot. Two keys can differ where two eids agree.
            const coop::element::ElementId eid =
                isHost ? coop::prop_element_tracker::GetPropElementIdForActor(t)
                       : coop::remote_prop::ResolveMirrorEidByActor(t);
            UE_LOGI("hookdrag_selftest: TARGET role=%s key='%ls' eid=%u at (%.1f,%.1f,%.1f), %.0f cm "
                    "from the anchor", isHost ? "HOST" : "CLIENT", g_targetKey.c_str(),
                    eid == coop::element::kInvalidId ? 0u : static_cast<unsigned>(eid),
                    g_targetStart.X, g_targetStart.Y, g_targetStart.Z,
                    std::sqrt(dx * dx + dy * dy + dz * dz));
            g_nextSampleMs = now;
        } else {
            UE_LOGW("hookdrag_selftest: PICK role=%s -- no keyed physics prop within %.0f cm",
                    isHost ? "HOST" : "CLIENT", kPickRadiusCm);
        }
    }
    if (g_target.Raw() && g_goneAtS < 0.f && now >= g_nextSampleMs && since < kVerdictMs) {
        g_nextSampleMs = now + kSampleMs;
        Sample(since, isHost);
    }
    if (isHost) {
        if (!g_planted && since >= kPlantMs) {
            g_planted = true;
            void* p = LocalPlayer();
            void* t = g_target.Get();
            if (p && t && Plant(p, t))
                UE_LOGI("hookdrag_selftest: PLANT key='%ls' hook=%p -- attach_a with the head on the "
                        "prop and the tail on the player", g_targetKey.c_str(), g_hook.Raw());
            else
                UE_LOGW("hookdrag_selftest: PLANT FAILED -- %s", g_plantNote);
        }
        if (g_hook.Raw() && since >= kWalkOutMs && since < kWalkOutMs + kWalkLegMs) {
            if (!g_walkOutStarted) {
                g_walkOutStarted = true;
                void* p = LocalPlayer();
                void* t = g_target.Get();
                if (p) g_walkBase = E::GetActorLocation(p);
                // Away from the prop in the horizontal plane; straight +X if the two coincide.
                const ue_wrap::FVector tl = t ? E::GetActorLocation(t) : g_walkBase;
                float dx = g_walkBase.X - tl.X, dy = g_walkBase.Y - tl.Y;
                const float n = std::sqrt(dx * dx + dy * dy);
                if (n > 1.f) { dx /= n; dy /= n; } else { dx = 1.f; dy = 0.f; }
                g_walkDir = ue_wrap::FVector{dx, dy, 0.f};
                UE_LOGI("hookdrag_selftest: WALK OUT %llu ms along (%.2f,%.2f) from (%.1f,%.1f,%.1f)",
                        static_cast<unsigned long long>(kWalkLegMs), dx, dy, g_walkBase.X,
                        g_walkBase.Y, g_walkBase.Z);
            }
            WalkStep(/*outward=*/true);
        }
        if (g_hook.Raw() && since >= kWalkBackMs && since < kWalkBackMs + kWalkLegMs) {
            if (!g_walkBackStarted) {
                g_walkBackStarted = true;
                UE_LOGI("hookdrag_selftest: WALK BACK");
            }
            WalkStep(/*outward=*/false);
        }
        if (!g_unhooked && since >= kUnhookMs) {
            g_unhooked = true;
            // The item's cancel: the field this driver wrote is cleared whether or not the hook
            // still stands, then the actor goes.
            if (g_hook.Raw()) {
                if (void* p = LocalPlayer()) H::WriteActiveHook(p, nullptr);
            }
            if (void* h = g_hook.Get()) {
                E::DestroyActor(h);
                UE_LOGI("hookdrag_selftest: UNHOOK -- the hook is destroyed; the prop coasts and rests");
            } else if (g_hook.Raw()) {
                UE_LOGW("hookdrag_selftest: UNHOOK -- the hook had already died");
            }
            g_hook.Reset();
        }
    }
    if (!g_verdict && since >= kVerdictMs) {
        g_verdict = true;
        EmitVerdict();
    }
}

void EmitVerdict() {
    if (!Enabled() || !g_originMs) return;
    auto* s = g_session.load(std::memory_order_acquire);
    const bool isHost = s && s->role() == coop::net::Role::Host;
    UE_LOGI("hookdrag_selftest: VERDICT role=%s key='%ls' plant=%s maxMoved=%.1f cm goneAt=%.2f",
            isHost ? "HOST" : "CLIENT", g_targetKey.c_str(), isHost ? g_plantNote : "n/a",
            g_maxMovedCm, g_goneAtS);
}

void OnDisconnect() {
    g_originMs = 0;
    g_nextSampleMs = 0;
    g_picked = g_planted = g_walkOutStarted = g_walkBackStarted = g_unhooked = g_verdict = false;
    g_target.Reset();
    g_hook.Reset();
    g_targetKey.clear();
    g_maxMovedCm = 0.f;
    g_goneAtS    = -1.f;
    g_plantNote  = "not attempted";
}

}  // namespace coop::dev::hookdrag_selftest
