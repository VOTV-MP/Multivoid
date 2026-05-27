// coop/npc_sync.cpp -- Phase 5N1 NPC sync foundation (extracted 2026-05-25).
//
// See coop/npc_sync.h for the public interface.
// Architecture findings: research/findings/votv-npc-sync-prereqs-RE-2026-05-24.md.

#include "coop/npc_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::npc_sync {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// Cached session pointer set by Install(). The interceptor reads role()/
// connected()/SendEntitySpawn() through this. nullptr until first Install.
//
// Audit C2 (2026-05-27): atomic Session* (was plain pointer). The interceptor
// fires from parallel-anim worker threads; plain-pointer deref races with
// harness SetSession(nullptr) on shutdown. Mirrors item_activate.cpp +
// prop_lifecycle.cpp atomic pattern.
std::atomic<coop::net::Session*> g_session_ptr{nullptr};

inline coop::net::Session* LoadSession() {
    return g_session_ptr.load(std::memory_order_acquire);
}

// Idempotency: once installed (or permanently failed) we short-circuit.
bool g_installed = false;

// 12 NPC UClass* pointers (zombie/kerfur/krampus/funguy/goreSlither/insomniacs/
// fossilhounds/antibreathers/orborbs + 3 ariral variants), resolved at install
// from the kNpcAllowlist names in sdk_profile.h. Sized by kNpcAllowlistSize
// so a future addition to the allowlist constant binds the array length too.
void* g_npcAllowlist[ue_wrap::profile::name::kNpcAllowlistSize] = {};

void* g_npcSpawnFn = nullptr;
int32_t g_npcSpawnActorClassParamOff = -1;
int32_t g_npcSpawnReturnParamOff = -1;

// Bypass slot for Inc2/Inc3's wire-received NPC spawns. Set immediately
// before the client-side BeginDeferred call; consumed by the next
// interceptor fire that sees the matching class.
void* g_incomingNpcSpawnClass = nullptr;

// Host-side monotonic sessionId counter (1-based; 0 = invalid). Reset per
// session if/when we add disconnect cleanup.
uint32_t g_nextNpcSessionId = 1;

// Host-side tracked-NPC set. Populated by the Inc3 K2_DestroyActor PRE
// hook so EntityDestroy carries the right sessionId.
std::unordered_map<void*, uint32_t> g_npcSessionByActor;

// Phase 5N1 Inc2 (2026-05-25): host-side env var gate. Set
// VOTVCOOP_NPC_SYNC=1 to enable the host-side EntitySpawn broadcast
// when this interceptor fires on the host for an allowlisted NPC class.
// Default OFF -- Inc2 ships the wire layer but doesn't activate it
// until the user verifies hands-on. Inc3 will wire the client-side
// receiver and we'll likely flip this on by default.
bool NpcSyncEnabled() {
    static bool resolved = false;
    static bool enabled = false;
    if (!resolved) {
        wchar_t buf[8] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"VOTVCOOP_NPC_SYNC", buf, 8);
        enabled = (n > 0 && buf[0] == L'1');
        resolved = true;
        UE_LOGI("npc-sync: VOTVCOOP_NPC_SYNC=%ls -> EntitySpawn broadcast %s",
                n > 0 ? buf : L"<unset>", enabled ? "ENABLED" : "disabled");
    }
    return enabled;
}

// Subclass-aware allowlist match: an exact-pointer match leaks all 30+ NPC
// subclasses (e.g. kerfurOmega has 20 subclasses incl. kerfurOmega_
// mannequinSpawner -- and npc_zombie has 14 subclasses, etc). Walk the
// SuperStruct chain like ue_wrap::prop::IsClassDescendantOfProp does. ~16
// hops max covers the deepest VOTV BP chain; we bound at kSuperStructHops
// to cap the loop in pathological cases.
constexpr int kSuperStructHops = 16;
bool IsClassOrDerivedFromAnyAllowlisted(void* cls) {
    if (!cls) return false;
    for (int hops = 0; hops < kSuperStructHops && cls; ++hops) {
        for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
            if (g_npcAllowlist[i] && g_npcAllowlist[i] == cls) return true;
        }
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}

bool NpcSuppress_Interceptor(void* self, void* params) {
    (void)self;  // self = the UGameplayStatics CDO; we don't use it
    // Cheapest checks first.
    if (!params || g_npcSpawnActorClassParamOff < 0) return false;
    auto* s = LoadSession();
    if (!s || !s->connected()) return false;  // pre-connect: don't filter

    // Phase 5N1 Inc2: HOST-side broadcast path. When env var is set and
    // ActorClass is allowlisted, the host emits an EntitySpawn reliable
    // packet and lets the spawn proceed normally. The interceptor runs
    // BEFORE the original UFunction; we read the SpawnTransform from
    // params before the spawn actually happens. Returns FALSE so the
    // original runs (host wants the NPC to actually spawn locally).
    //
    // Inc3 will:
    //   - track the returned AActor* in g_npcSessionByActor (POST hook)
    //   - hook K2_DestroyActor PRE to send EntityDestroy
    //   - wire client-side receiver to materialize a mirror via
    //     MarkIncomingNpcSpawn + BeginDeferred + FinishSpawning
    //
    // For Inc2 (this commit): host detects spawns + sends EntitySpawn.
    // Client receives the packet but doesn't yet act on it (the
    // event_feed receiver is wired-to-log-only). Detect-only mode.
    if (s->role() == coop::net::Role::Host) {
        if (!NpcSyncEnabled()) return false;
        void* actorClass = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(params) + g_npcSpawnActorClassParamOff);
        if (!actorClass || !IsClassOrDerivedFromAnyAllowlisted(actorClass)) {
            return false;  // not an NPC; let it run, no broadcast
        }
        // Read the SpawnTransform param. Offset resolved at install time
        // for both endpoints; for now we read via FindParamOffset each
        // call (cached statically below).
        static int32_t sXformOff = -2;
        if (sXformOff == -2) {
            sXformOff = R::FindParamOffset(g_npcSpawnFn, L"SpawnTransform");
        }
        coop::net::EntitySpawnPayload p{};
        if (sXformOff >= 0) {
            // FTransform layout: FQuat Rotation (16B) + FVector Translation (12B)
            // + FVector Scale3D (12B) -- 48 bytes total, but UE4 also aligns to
            // 16 so it's typically 48 with internal padding.
            const uint8_t* xform = reinterpret_cast<const uint8_t*>(params) + sXformOff;
            // FQuat (XYZW)
            const float qx = *reinterpret_cast<const float*>(xform + 0);
            const float qy = *reinterpret_cast<const float*>(xform + 4);
            const float qz = *reinterpret_cast<const float*>(xform + 8);
            const float qw = *reinterpret_cast<const float*>(xform + 12);
            // FVector translation @ +0x10 (16; after the 16-byte FQuat).
            p.locX = *reinterpret_cast<const float*>(xform + 0x10);
            p.locY = *reinterpret_cast<const float*>(xform + 0x14);
            p.locZ = *reinterpret_cast<const float*>(xform + 0x18);
            // Convert FQuat -> FRotator (in degrees) for wire compatibility
            // with our existing FRotator-based pose pipeline. Standard UE4
            // conversion: pitch = asin(2(wy - zx)); yaw = atan2(2(wz + xy),
            // 1 - 2(yy + zz)); roll = atan2(2(wx + yz), 1 - 2(xx + yy)).
            // We use a single normalize for sin clamp.
            const float sinp = 2.f * (qw * qy - qz * qx);
            const float sinp_clamped = sinp >  1.f ?  1.f
                                     : sinp < -1.f ? -1.f : sinp;
            const float pitchRad = std::asin(sinp_clamped);
            const float yawRad   = std::atan2(2.f * (qw * qz + qx * qy),
                                              1.f - 2.f * (qy * qy + qz * qz));
            const float rollRad  = std::atan2(2.f * (qw * qx + qy * qz),
                                              1.f - 2.f * (qx * qx + qy * qy));
            constexpr float kRadToDeg = 57.29577951308232f;
            p.rotPitch = pitchRad * kRadToDeg;
            p.rotYaw   = yawRad   * kRadToDeg;
            p.rotRoll  = rollRad  * kRadToDeg;
        }
        // Class name -> wire string. actorClass IS a UClass*; NameOf returns
        // its own FName (e.g., "npc_zombie_C"). (ClassNameOf would return the
        // class-of-the-class, i.e., "Class" -- wrong.)
        const std::wstring cls = R::ToString(R::NameOf(actorClass));
        p.className.len = 0;
        for (size_t i = 0; i < cls.size() && i < 63; ++i) {
            p.className.data[p.className.len++] = static_cast<char>(cls[i]);
        }
        p.sessionId = g_nextNpcSessionId++;
        if (s->SendEntitySpawn(p)) {
            UE_LOGI("npc-sync[host]: broadcast EntitySpawn class='%ls' sessionId=%u loc=(%.0f, %.0f, %.0f) rot=(p=%.1f y=%.1f r=%.1f)",
                    cls.c_str(), p.sessionId, p.locX, p.locY, p.locZ,
                    p.rotPitch, p.rotYaw, p.rotRoll);
        } else {
            UE_LOGW("npc-sync[host]: SendEntitySpawn failed (reliable channel busy?) -- NPC sessionId=%u not broadcast",
                    p.sessionId);
        }
        // ALWAYS let the original spawn proceed on the host. Returning
        // false from the interceptor = pass-through to the real UFunction.
        return false;
    }

    // Host runs spawners normally; only CLIENT suppresses.
    if (s->role() != coop::net::Role::Client) return false;

    // Read the ActorClass UClass* param from the FFrame buffer.
    void* actorClass = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(params) + g_npcSpawnActorClassParamOff);
    if (!actorClass) return false;  // BP bug or default; let UE4 handle

    // Audit-fix C2 (2026-05-25): bypass slot for wire-received NPC spawns.
    // Inc2's client-side dispatcher will set g_incomingNpcSpawnClass = cls
    // immediately before calling BeginDeferredActorSpawnFromClass to
    // materialize a host-streamed NPC. We consume the slot here and allow
    // the original through. Single-shot: cleared on consume so a stray
    // local spawn of the same class doesn't accidentally pass through.
    if (g_incomingNpcSpawnClass && g_incomingNpcSpawnClass == actorClass) {
        UE_LOGI("npc-suppress[client]: allow-through wire-received spawn for class=%p (bypass slot consumed)",
                actorClass);
        g_incomingNpcSpawnClass = nullptr;
        return false;
    }

    // Subclass-aware allowlist match. See IsClassOrDerivedFromAnyAllowlisted.
    if (IsClassOrDerivedFromAnyAllowlisted(actorClass)) {
        UE_LOGI("npc-suppress[client]: skipping BeginDeferredActorSpawnFromClass for class=%p (matches NPC allowlist by hierarchy walk)",
                actorClass);
        // Zero the AActor* return so the BP graph receives nullptr
        // and bails. UE4's K2Node_SpawnActorFromClass emits a null-check
        // before the FinishSpawningActor call per UE4 engine convention --
        // NOT YET IDA-confirmed on VOTV spawners; the RE TODO in
        // research/findings/votv-npc-sync-prereqs-RE-2026-05-24.md
        // section 4 (lines 559-563) tracks this. Per UE4 standard
        // emission patterns the risk is low, but if a specific VOTV
        // spawner BP emits non-null-checking code, we'd see a
        // FinishSpawningActor(nullptr, ...) crash here.
        if (g_npcSpawnReturnParamOff >= 0) {
            *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(params) + g_npcSpawnReturnParamOff) = nullptr;
        }
        return true;  // SKIP the original
    }
    return false;  // not an NPC; let it run
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);
}

void MarkIncomingNpcSpawn(void* npcClass) {
    g_incomingNpcSpawnClass = npcClass;
}

void OnDisconnect() {
    // sessionId must restart at 1 next session (zero is reserved for
    // "invalid"). The g_npcSessionByActor map holds stale actor pointers
    // from the previous session -- not safe to keep, and the next
    // session's allocator wouldn't recognize them anyway.
    g_npcSessionByActor.clear();
    g_nextNpcSessionId = 1;
    g_incomingNpcSpawnClass = nullptr;  // any in-flight bypass slot
}

void Install(coop::net::Session* session) {
    g_session_ptr.store(session, std::memory_order_release);  // cache (caller guarantees outlives us)
    if (g_installed) return;
    void* gsCls = R::FindClass(P::name::GameplayStaticsClass);
    if (!gsCls) return;  // not loaded yet (loads with /Script/Engine; very early)
    void* fn = R::FindFunction(gsCls, P::name::BeginDeferredSpawnFn);
    if (!fn) {
        UE_LOGW("npc-suppress: %ls.%ls UFunction not found -- Phase 5N1 disabled permanently this session",
                P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
        g_installed = true;  // stop retry
        return;
    }

    // Resolve the ActorClass param FFrame offset.
    const int32_t classOff = R::FindParamOffset(fn, L"ActorClass");
    if (classOff < 0) {
        UE_LOGW("npc-suppress: %ls.%ls 'ActorClass' param not found (BP recook?) -- disabled",
                P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
        g_installed = true;
        return;
    }
    // ReturnValue is the AActor* the function returns. UE4 convention: param
    // named "ReturnValue" on the FUNCTION's properties list.
    const int32_t retOff = R::FindParamOffset(fn, L"ReturnValue");
    if (retOff < 0) {
        UE_LOGW("npc-suppress: %ls.%ls 'ReturnValue' param not found -- can't zero the return; disabled",
                P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn);
        g_installed = true;
        return;
    }

    // Resolve the 12 NPC classes. Partial-resolution OK: missing classes
    // just won't be suppressed. Most VOTV NPC BP classes are loaded with
    // /Game/Content/blueprints/npc/... -- present on gameplay-level entry.
    // We retry the WHOLE install until all 12 resolve.
    size_t resolved = 0;
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        if (!g_npcAllowlist[i]) {
            g_npcAllowlist[i] = R::FindClass(P::name::kNpcAllowlist[i]);
        }
        if (g_npcAllowlist[i]) ++resolved;
    }
    if (resolved < P::name::kNpcAllowlistSize) {
        // Wait for the rest -- retry next NetPumpTick. Do NOT install yet;
        // we want all 12 cached before going live, to avoid a window where
        // some NPCs are suppressed and others aren't.
        UE_LOGI("npc-suppress: NPC class load partial (%zu/%zu) -- retry next tick",
                resolved, P::name::kNpcAllowlistSize);
        return;
    }

    // All 12 NPC classes resolved. Cache the function pointer + offsets
    // and register the interceptor. RegisterInterceptor stores one slot in
    // the multi-slot table; we own that slot for the whole session.
    g_npcSpawnFn = fn;
    g_npcSpawnActorClassParamOff = classOff;
    g_npcSpawnReturnParamOff = retOff;
    ue_wrap::game_thread::RegisterInterceptor(fn, &NpcSuppress_Interceptor);
    g_installed = true;
    UE_LOGI("npc-suppress: installed interceptor on %ls.%ls @ %p (ActorClass@%d, ReturnValue@%d, 12/12 NPC classes resolved)",
            P::name::GameplayStaticsClass, P::name::BeginDeferredSpawnFn,
            fn, classOff, retOff);
    for (size_t i = 0; i < P::name::kNpcAllowlistSize; ++i) {
        UE_LOGI("npc-suppress: allowlist[%zu] '%ls' = %p",
                i, P::name::kNpcAllowlist[i], g_npcAllowlist[i]);
    }
}

}  // namespace coop::npc_sync
