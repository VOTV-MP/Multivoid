// coop/player/cremation.cpp -- see coop/player/cremation.h.

#include "coop/player/cremation.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h" // UE_ASSERT_GAME_THREAD
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_player_verbs.h"
#include "ue_wrap/actors/vitals.h"

#include <windows.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace coop::cremation {
namespace {

namespace E = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace OI = ue_wrap::object_index;
namespace R = ue_wrap::reflection;

constexpr uint64_t kTripTimeoutMs = 60000;
constexpr uint64_t kBurnFailSafeMs = 35000;
constexpr char kDisconnectReason[] = "You have been sent to the crematorium.";

struct Trip {
    coop::moderation::PlayerToken token;
    uint64_t startedMs = 0;
    ue_wrap::FVector crematorAt{};
    bool active = false;
};

std::array<Trip, coop::players::kMaxPeers> g_trips{};
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_localLocked = false;
bool g_localLockDesired = false;
bool g_localBurnDeathPending = false;
bool g_localSawBurn = false;
bool g_localKillActive = false;
uint64_t g_localKillNextMs = 0;
uint64_t g_localBurnFallbackMs = 0;
bool g_localCrematorLocked = false;
uint64_t g_nextCrematorEnforceMs = 0;
ue_wrap::FVector g_localCrematorAt{};

uint64_t NowMs() { return GetTickCount64(); }

void* FireboxOf(void* cremator) {
    if (!cremator || !R::IsLive(cremator)) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(cremator), L"firebox");
    if (off < 0) return nullptr;
    void* component = nullptr;
    std::memcpy(&component, static_cast<const char*>(cremator) + off, sizeof(component));
    return component && R::IsLive(component) ? component : nullptr;
}

bool FindFireboxPose(uint8_t slot, coop::net::TeleportClientPayload& pose) {
    auto* puppet = coop::players::Registry::Get().Puppet(slot);
    if (!puppet || !puppet->valid()) return false;
    ue_wrap::FVector target{};
    if (!puppet->TryGetLocation(target)) return false;

    void* cls = OI::ClassByName(L"cremator_C");
    if (!cls) return false;

    struct Search { ue_wrap::FVector from; coop::net::TeleportClientPayload* pose; float best; bool found; };
    Search search{target, &pose, INFINITY, false};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto& s = *static_cast<Search*>(ctx);
        void* firebox = FireboxOf(obj);
        if (!firebox) return;
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!E::TryGetComponentLocation(firebox, at) || !E::TryGetActorRotation(obj, rot)) return;
        const float dx = at.X - s.from.X, dy = at.Y - s.from.Y, dz = at.Z - s.from.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= s.best) return;
        s.best = d2;
        s.pose->locX = at.X; s.pose->locY = at.Y; s.pose->locZ = at.Z;
        s.pose->rotPitch = rot.Pitch; s.pose->rotYaw = rot.Yaw; s.pose->rotRoll = rot.Roll;
        s.found = true;
    }, &search);
    return search.found;
}

void* FindNearestCremator(void* pawn) {
    if (!pawn || !R::IsLive(pawn)) return nullptr;
    ue_wrap::FVector target{};
    if (!E::TryGetActorLocation(pawn, target)) return nullptr;
    void* cls = OI::ClassByName(L"cremator_C");
    if (!cls) return nullptr;
    struct Search { ue_wrap::FVector from; float best; void* actor; };
    Search search{target, INFINITY, nullptr};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto& s = *static_cast<Search*>(ctx);
        void* firebox = FireboxOf(obj);
        ue_wrap::FVector at{};
        if (!firebox || !E::TryGetComponentLocation(firebox, at)) return;
        const float dx = at.X - s.from.X, dy = at.Y - s.from.Y, dz = at.Z - s.from.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < s.best) { s.best = d2; s.actor = obj; }
    }, &search);
    return search.best <= 1000.f * 1000.f ? search.actor : nullptr;
}

void WriteBoolProperty(void* object, const wchar_t* name, bool value) {
    if (!object || !R::IsLive(object)) return;
    int32_t offset = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(object), name, offset, mask) || offset < 0 || !mask) return;
    auto* byte = reinterpret_cast<uint8_t*>(static_cast<char*>(object) + offset);
    *byte = value ? static_cast<uint8_t>(*byte | mask)
                  : static_cast<uint8_t>(*byte & static_cast<uint8_t>(~mask));
}

bool ReadBoolProperty(void* object, const wchar_t* name, bool& value) {
    if (!object || !R::IsLive(object)) return false;
    int32_t offset = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(object), name, offset, mask) || offset < 0 || !mask) return false;
    value = (*reinterpret_cast<const uint8_t*>(static_cast<const char*>(object) + offset) & mask) != 0;
    return true;
}

void* CrematorAt(const ue_wrap::FVector& location) {
    void* cls = OI::ClassByName(L"cremator_C");
    if (!cls) return nullptr;
    struct Search { ue_wrap::FVector at; float best; void* actor; };
    Search search{location, INFINITY, nullptr};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto& s = *static_cast<Search*>(ctx);
        ue_wrap::FVector at{};
        void* firebox = FireboxOf(obj);
        if (!firebox || !E::TryGetComponentLocation(firebox, at)) return;
        const float dx = at.X - s.at.X, dy = at.Y - s.at.Y, dz = at.Z - s.at.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < s.best) { s.best = d2; s.actor = obj; }
    }, &search);
    return search.best <= 500.f * 500.f ? search.actor : nullptr;
}

void EnforceCremator(void* cremator, bool activateLever) {
    if (!cremator || !R::IsLive(cremator)) return;
    void* cls = R::ClassOf(cremator);
    if (void* fn = R::FindFunction(cls, L"setClosed")) {
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"close", true);
        ue_wrap::Call(cremator, f);
    }
    void* door = nullptr;
    const int32_t doorOffset = R::FindPropertyOffset(cls, L"swingerDoor");
    if (doorOffset >= 0) std::memcpy(&door, static_cast<const char*>(cremator) + doorOffset, sizeof(door));
    if (door && R::IsLive(door)) WriteBoolProperty(door, L"Locked", true);

    bool active = false;
    ReadBoolProperty(cremator, L"IsActive", active);
    if (activateLever && !active) {
        if (void* fn = R::FindFunction(cls, L"useLever")) {
            ue_wrap::ParamFrame f(fn);
            ue_wrap::Call(cremator, f);
        }
    }
    // These flags make the BP interaction path treat the handle as engaged/in transition;
    // the timeline still owns the visible lever animation.
    WriteBoolProperty(cremator, L"IsActive", true);
    WriteBoolProperty(cremator, L"Anim", true);
}

bool SendMachineState(coop::net::Session& session, uint8_t action, const ue_wrap::FVector& at) {
    coop::net::CremationControlPayload p{action, at.X, at.Y, at.Z};
    bool sent = true;
    if (session.role() == coop::net::Role::Host) {
        for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
            if (!session.IsSlotWorldReady(slot)) continue;
            sent = session.SendReliableToSlot(slot, coop::net::ReliableKind::CremationControl,
                                              &p, sizeof(p)) && sent;
        }
    }
    return sent;
}

void ReleaseTripMachine(coop::net::Session& session, Trip& trip) {
    SendMachineState(session, 4, trip.crematorAt);
    SetLocalCrematorLock(trip.crematorAt.X, trip.crematorAt.Y, trip.crematorAt.Z, false);
}

bool SendInputLock(coop::net::Session& session, int slot, uint32_t generation, bool disabled) {
    if (session.peerGenerationForSlot(slot) != generation) return false;
    coop::net::CremationControlPayload p{static_cast<uint8_t>(disabled ? 1 : 0), 0.f, 0.f, 0.f};
    return session.SendReliableToSlot(slot, coop::net::ReliableKind::CremationControl, &p, sizeof(p));
}

bool SendStartSequence(coop::net::Session& session, int slot, uint32_t generation) {
    if (session.peerGenerationForSlot(slot) != generation) return false;
    coop::net::CremationControlPayload p{2, 0.f, 0.f, 0.f};
    return session.SendReliableToSlot(slot, coop::net::ReliableKind::CremationControl, &p, sizeof(p));
}

bool ForceLocalFatalHit(void* pawn) {
    if (!pawn || !R::IsLive(pawn)) return false;
    bool isRagdoll = false, dead = false;
    if (E::ReadMainPlayerRagdollState(pawn, isRagdoll, dead) && dead) return true;

    // The native damage verb refuses damage during spawn protection. Clear that game flag, then
    // use Add Player Damage with blood enabled so its regular death/ragdoll path runs.
    E::WriteMainPlayerStartInvinc(pawn, false);
    float health = 100.f, maxHealth = 100.f;
    ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &health);
    ue_wrap::vitals::Read(ue_wrap::vitals::Field::MaxHealth, &maxHealth);
    const float lethal = (std::max)(200.f, 2.f * (std::max)(health, maxHealth));
    return E::InvokeAddPlayerDamage(pawn, lethal, /*blood=*/true);
}

void StartOnGameThread(coop::moderation::PlayerToken token) {
    UE_ASSERT_GAME_THREAD("cremation::StartOnGameThread");
    if (!token.valid() || token.slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    auto& trip = g_trips[static_cast<size_t>(token.slot)];
    if (trip.active) {
        UE_LOGI("cremation: request ignored -- slot %d already has an active trip", token.slot);
        return;
    }
    if (coop::roster_ledger::Get(token.slot).playerNo != token.playerNo) return;

    // Host-only proof is the same live-generation token moderation uses for kicks and teleports.
    auto* puppet = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(token.slot));
    if (!puppet || !puppet->valid() ||
        coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(token.slot)) !=
            coop::players::PuppetLife::Alive) {
        UE_LOGW("cremation: target slot %d has no living puppet", token.slot);
        return;
    }
    coop::net::Session* session = g_session.load(std::memory_order_acquire);
    if (!session || !session->running() || session->role() != coop::net::Role::Host ||
        session->peerGenerationForSlot(token.slot) != token.generation) return;

    coop::net::TeleportClientPayload pose{};
    if (!FindFireboxPose(static_cast<uint8_t>(token.slot), pose)) {
        UE_LOGW("cremation: no loaded cremator firebox could be resolved; slot %d not moved", token.slot);
        return;
    }
    if (!std::isfinite(pose.locX) || !std::isfinite(pose.locY) || !std::isfinite(pose.locZ) ||
        std::fabs(pose.locX) > coop::net::kMaxCoord || std::fabs(pose.locY) > coop::net::kMaxCoord ||
        std::fabs(pose.locZ) > coop::net::kMaxCoord) return;

    const ue_wrap::FVector crematorAt{pose.locX, pose.locY, pose.locZ};
    SetLocalCrematorLock(crematorAt.X, crematorAt.Y, crematorAt.Z, true);
    if (!SendMachineState(*session, 3, crematorAt)) {
        SetLocalCrematorLock(crematorAt.X, crematorAt.Y, crematorAt.Z, false);
        UE_LOGW("cremation: could not broadcast cremator lock for slot %d", token.slot);
        return;
    }

    // The lock and teleport share the High lane, so the client cannot move during the transition.
    if (!SendInputLock(*session, token.slot, token.generation, true) ||
        !session->SendReliableToSlot(token.slot, coop::net::ReliableKind::TeleportClient, &pose, sizeof(pose)) ||
        !SendStartSequence(*session, token.slot, token.generation)) {
        SendInputLock(*session, token.slot, token.generation, false);
        SendMachineState(*session, 4, crematorAt);
        SetLocalCrematorLock(crematorAt.X, crematorAt.Y, crematorAt.Z, false);
        UE_LOGW("cremation: could not send the firebox sequence to slot %d", token.slot);
        return;
    }
    trip.token = token;
    trip.startedMs = NowMs();
    trip.crematorAt = crematorAt;
    trip.active = true;
    UE_LOGI("cremation: slot %d sent to cremator firebox", token.slot);
}

}  // namespace

coop::moderation::ModResult SendToCremator(const coop::moderation::PlayerToken& token) {
    if (!coop::moderation::HostedSessionRunning()) return coop::moderation::ModResult::NoSession;
    if (!token.valid()) return coop::moderation::ModResult::Gone;
    GT::Post([token] { StartOnGameThread(token); });
    return coop::moderation::ModResult::Done;
}

void Tick(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("cremation::Tick");
    g_session.store(&session, std::memory_order_release);
    if (session.role() == coop::net::Role::Client && session.running()) {
        if (g_localCrematorLocked && NowMs() >= g_nextCrematorEnforceMs) {
            EnforceCremator(CrematorAt(g_localCrematorAt), true);
            g_nextCrematorEnforceMs = NowMs() + 250;
        }
        ApplyLocalControl(g_localLockDesired);
        if (g_localBurnDeathPending || g_localKillActive) {
            void* pawn = coop::players::Registry::Get().Local();
            bool isRagdoll = false, dead = false;
            if (pawn && E::ReadMainPlayerRagdollState(pawn, isRagdoll, dead) && dead) {
                g_localBurnDeathPending = false;
                g_localKillActive = false;
                UE_LOGI("cremation: local death confirmed; fatal-hit retries stopped");
            } else if (g_localBurnDeathPending) {
                float burningTime = 0.f;
                const bool haveBurningTime =
                    ue_wrap::vitals::Read(ue_wrap::vitals::Field::BurningTime, &burningTime);
                if (haveBurningTime && burningTime > 0.f) {
                    g_localSawBurn = true;
                    // Keep the failsafe behind the live game's burn timer if the cremator extends it.
                    g_localBurnFallbackMs = NowMs() +
                        static_cast<uint64_t>(burningTime * 1000.f) + 2000;
                }
                if ((haveBurningTime && burningTime <= 0.f && g_localSawBurn) ||
                    NowMs() >= g_localBurnFallbackMs) {
                    g_localBurnDeathPending = false;
                    g_localSawBurn = false;
                    g_localKillActive = true;
                    g_localKillNextMs = NowMs();
                    UE_LOGI("cremation: burn ended; applying fatal damage if the pawn survived");
                }
            }
            if (g_localKillActive && NowMs() >= g_localKillNextMs) {
                const bool hit = ForceLocalFatalHit(pawn);
                g_localKillNextMs = NowMs() + 250;
                if (!hit)
                    UE_LOGW("cremation: fatal hit did not dispatch; retrying while local pawn is alive");
            }
        }
        return;
    }
    if (session.role() != coop::net::Role::Host || !session.running()) return;
    const uint64_t now = NowMs();
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        Trip& trip = g_trips[static_cast<size_t>(slot)];
        if (!trip.active) continue;
        if (session.peerGenerationForSlot(slot) != trip.token.generation ||
            coop::roster_ledger::Get(slot).playerNo != trip.token.playerNo) {
            ReleaseTripMachine(session, trip);
            trip = Trip{};
            continue;
        }
        if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(slot)) == coop::players::PuppetLife::Dead) {
            const auto result = coop::moderation::KickPlayer(trip.token, kDisconnectReason);
            UE_LOGI("cremation: slot %d death observed; kick result=%u", slot, static_cast<unsigned>(result));
            ReleaseTripMachine(session, trip);
            trip = Trip{};
            continue;
        }
        if (now - trip.startedMs >= kTripTimeoutMs) {
            SendInputLock(session, slot, trip.token.generation, false);
            ReleaseTripMachine(session, trip);
            UE_LOGW("cremation: slot %d did not die within %llu ms; controls restored",
                    slot, static_cast<unsigned long long>(kTripTimeoutMs));
            trip = Trip{};
        }
    }
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        const Trip& trip = g_trips[static_cast<size_t>(slot)];
        if (trip.active && now >= g_nextCrematorEnforceMs) {
            EnforceCremator(CrematorAt(trip.crematorAt), true);
        }
    }
    if (now >= g_nextCrematorEnforceMs) g_nextCrematorEnforceMs = now + 250;
}

void ApplyLocalControl(bool disabled) {
    UE_ASSERT_GAME_THREAD("cremation::ApplyLocalControl");
    g_localLockDesired = disabled;
    if (g_localLocked == disabled) return;
    void* pawn = coop::players::Registry::Get().Local();
    void* controller = pawn ? E::GetController(pawn) : nullptr;
    if (!pawn || !controller) return;
    void* pcClass = OI::ClassByName(L"PlayerController");
    if (pcClass) {
        if (void* fn = R::FindFunction(pcClass, L"SetIgnoreMoveInput")) {
            ue_wrap::ParamFrame f(fn); f.Set<bool>(L"bNewMoveInput", disabled); ue_wrap::Call(controller, f);
        }
        if (void* fn = R::FindFunction(pcClass, L"SetIgnoreLookInput")) {
            // Keep mouse look active while movement is suppressed.
            ue_wrap::ParamFrame f(fn); f.Set<bool>(L"bNewLookInput", false); ue_wrap::Call(controller, f);
        }
    }
    g_localLocked = disabled;
    UE_LOGI("cremation: local controls %s", disabled ? "disabled" : "enabled");
}

void StartLocalSequence() {
    UE_ASSERT_GAME_THREAD("cremation::StartLocalSequence");
    void* pawn = coop::players::Registry::Get().Local();
    void* cremator = FindNearestCremator(pawn);
    if (!pawn || !R::IsLive(pawn)) return;

    ue_wrap::FVector crematorAt{};
    void* firebox = FireboxOf(cremator);
    if (firebox) E::TryGetComponentLocation(firebox, crematorAt);
    if (firebox) SetLocalCrematorLock(crematorAt.X, crematorAt.Y, crematorAt.Z, true);

    bool doorLocked = false;
    bool crematorStarted = false;
    if (cremator) {
        if (void* fn = R::FindFunction(R::ClassOf(cremator), L"setClosed")) {
            ue_wrap::ParamFrame f(fn);
            f.Set<bool>(L"close", true);
            doorLocked = ue_wrap::Call(cremator, f);
        }
        if (void* fn = R::FindFunction(R::ClassOf(cremator), L"igniteInside")) {
            ue_wrap::ParamFrame f(fn);
            crematorStarted = ue_wrap::Call(cremator, f);
        }
    }
    const bool playerIgnited = E::InvokeMainPlayerIgnite(pawn, 30.f);
    g_localBurnDeathPending = true;
    g_localSawBurn = false;
    g_localKillActive = false;
    g_localBurnFallbackMs = NowMs() + kBurnFailSafeMs;
    UE_LOGI("cremation: local sequence doorLocked=%d crematorStarted=%d playerIgnited=%d; waiting for burn to end",
            doorLocked ? 1 : 0, crematorStarted ? 1 : 0, playerIgnited ? 1 : 0);
}

void SetLocalCrematorLock(float locX, float locY, float locZ, bool locked) {
    UE_ASSERT_GAME_THREAD("cremation::SetLocalCrematorLock");
    if (!std::isfinite(locX) || !std::isfinite(locY) || !std::isfinite(locZ)) return;
    g_localCrematorAt = {locX, locY, locZ};
    g_localCrematorLocked = locked;
    if (!locked) {
        void* cremator = CrematorAt(g_localCrematorAt);
        if (cremator) {
            if (void* fn = R::FindFunction(R::ClassOf(cremator), L"setClosed")) {
                ue_wrap::ParamFrame f(fn); f.Set<bool>(L"close", false); ue_wrap::Call(cremator, f);
            }
            void* door = nullptr;
            const int32_t off = R::FindPropertyOffset(R::ClassOf(cremator), L"swingerDoor");
            if (off >= 0) std::memcpy(&door, static_cast<const char*>(cremator) + off, sizeof(door));
            if (door && R::IsLive(door)) WriteBoolProperty(door, L"Locked", false);
            WriteBoolProperty(cremator, L"Anim", false);
        }
        return;
    }
    EnforceCremator(CrematorAt(g_localCrematorAt), true);
    g_nextCrematorEnforceMs = NowMs() + 250;
}

void OnSessionEnd() {
    UE_ASSERT_GAME_THREAD("cremation::OnSessionEnd");
    g_localLockDesired = false;
    if (g_localLocked) ApplyLocalControl(false);
    if (g_localCrematorLocked)
        SetLocalCrematorLock(g_localCrematorAt.X, g_localCrematorAt.Y, g_localCrematorAt.Z, false);
    g_localBurnDeathPending = false;
    g_localSawBurn = false;
    g_localKillActive = false;
    g_localCrematorLocked = false;
    g_trips.fill(Trip{});
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::cremation
