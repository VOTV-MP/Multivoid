// coop/interactables/physmods_sync.cpp -- see coop/interactables/physmods_sync.h.

#include "coop/interactables/physmods_sync.h"

#include "coop/interactables/desk_snd_fx.h"  // ScopedWireApply (the shared desk wire guard)
#include "coop/element/mirror_manager.h"
#include "coop/element/prop.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"  // DestroyLocalProp (echo-suppressed)

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/engine/engine.h"  // SpawnActor (the plug-dup REFUND) + TryGetActorLocation
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

namespace coop::physmods_sync {
namespace {

namespace PM = ue_wrap::phys_mods;
namespace CD = ue_wrap::console_desk;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// The parked canonical's wait for the desk, paced as the resolve's own backoff is.
constexpr auto kParkedRetry = std::chrono::milliseconds(1000);
Clock::time_point g_nextParkedTry{};

// A canonical array that arrived before the desk resolved (a joiner's replay
// racing its world load) parks here and applies at resolve.
uint8_t g_pendingCanon[PM::kSlots] = {};
bool    g_havePendingCanon = false;

// HOST: recent unplug-denies, read by the ReelEjectIntent birth author to reap
// the ghost of a module that was dropped before its deny landed.
struct DenyRec {
    uint8_t slot = 0xFF;
    uint8_t byte = 0;
    Clock::time_point until{};
};
constexpr int kDenyRecs = 8;
constexpr auto kDenyTtl = std::chrono::seconds(10);
DenyRec g_denies[kDenyRecs];

void SendOp(coop::net::Session* s, uint8_t op, uint8_t byte) {
    // CLIENT-only: the host's organic changes never ride ops -- its live array
    // IS the canonical. A host that self-applied an already-applied op hit the
    // dup and absent branches, refunding a phantom and broadcasting nothing.
    coop::net::PhysModsStatePayload p{};
    p.op = op;
    p.byte = byte;
    s->SendReliableToSlot(0, coop::net::ReliableKind::PhysModsState, &p, sizeof(p));
}

void HostBroadcastCanonical(coop::net::Session* s, int onlySlot = -1) {
    coop::net::PhysModsStatePayload p{};
    p.op = 2;
    if (!PM::ReadArray(p.bytes)) return;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::PhysModsState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::PhysModsState, &p, sizeof(p));
}

// The ops of one local edit: the array is a SET, so the change decomposes into vanished bytes
// (unplugs) and appeared bytes (plugs) regardless of slot movement.
void SendLocalEdit(coop::net::Session* s, const uint8_t before[PM::kSlots], const uint8_t live[PM::kSlots]) {
    const bool isHost = (s->role() == coop::net::Role::Host);
    bool hostChanged = false;
    for (int i = 0; i < PM::kSlots; ++i) {
        const uint8_t b = before[i];
        if (!b) continue;
        bool still = false;
        for (int j = 0; j < PM::kSlots; ++j) if (live[j] == b) { still = true; break; }
        if (!still) {
            UE_LOGI("physmods: local UNPLUG byte=%u -- %s", b,
                    isHost ? "canonical will carry it" : "op to host");
            if (isHost) hostChanged = true; else SendOp(s, 1, b);
        }
    }
    for (int i = 0; i < PM::kSlots; ++i) {
        const uint8_t b = live[i];
        if (!b) continue;
        bool had = false;
        for (int j = 0; j < PM::kSlots; ++j) if (before[j] == b) { had = true; break; }
        if (!had) {
            UE_LOGI("physmods: local PLUG byte=%u -- %s", b,
                    isHost ? "canonical will carry it" : "op to host");
            if (isHost) hostChanged = true; else SendOp(s, 0, b);
        }
    }
    // The host's live array IS the canonical, so ONE broadcast carries any
    // number of organic changes.
    if (isHost && hostChanged && s->connected()) HostBroadcastCanonical(s);
}

void AdoptCanonical(const uint8_t bytes[PM::kSlots]) {
    coop::desk_snd_fx::ScopedWireApply guard;
    if (!PM::WriteArray(bytes)) return;
    PM::CallUpdPhysMods();
}

// The edits from play are the desk's two verbs that write the array: plugInModule (a module into a
// free slot) and playerHitWith, whose unplug branch reborns the module into the hand and zeroes its
// slot (a hit on a slot with a module in hand runs plugInModule inside it). setData writes the array
// wholesale on a save load and is not an edit, so a joiner's load sends nothing; nor are this lane's
// own applies, which write the array and run updPhysMods, never a verb. Each watched body snapshots
// the array at entry and sends what it changed at exit; after a send every enclosing body's snapshot
// moves to the array as sent, so a plug inside a hit is sent once.
constexpr int kTagPlug = 0x504C5547;  // 'PLUG'
constexpr int kTagHit  = 0x50484954;  // 'PHIT'
bool g_editsWatched = false;

struct InFlight {
    void*   stack;
    uint8_t before[PM::kSlots];
};
std::vector<InFlight> g_inFlight;  // game thread only

sg::Verdict OnEditPre(const sg::Call& call) {
    InFlight f{call.stack, {}};
    if (PM::ReadArray(f.before)) g_inFlight.push_back(f);
    return sg::Verdict::Run;
}

void OnEditPost(const sg::Call& call) {
    // The innermost entry of this body; any above it is a body that ended without its post.
    for (size_t i = g_inFlight.size(); i-- > 0;) {
        if (g_inFlight[i].stack != call.stack) continue;
        uint8_t before[PM::kSlots];
        std::memcpy(before, g_inFlight[i].before, PM::kSlots);
        g_inFlight.resize(i);
        uint8_t live[PM::kSlots];
        if (!PM::ReadArray(live) || std::memcmp(before, live, PM::kSlots) == 0) return;
        auto* s = g_session.load(std::memory_order_acquire);
        if (s && s->connected()) SendLocalEdit(s, before, live);
        for (InFlight& outer : g_inFlight) std::memcpy(outer.before, live, PM::kSlots);
        return;
    }
}

void RecordDeny(uint8_t slot, uint8_t byte) {
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.slot == 0xFF || now >= d.until) { d = {slot, byte, now + kDenyTtl}; return; }
    }
    g_denies[0] = {slot, byte, now + kDenyTtl};  // overwrite oldest-slot-0 (bounded)
}

// CLIENT deny handling: destroy the local hand ghost, else sweep untracked
// module actors of the byte's class (the drop-before-deny case).
void ClientHandleDeny(uint8_t origOp, uint8_t byte) {
    if (origOp == 0) {
        // plug-dup: the HOST refunded (spawned the module back at the desk);
        // our local item is already destroyed by our own plugInModule. Log only.
        UE_LOGW("physmods: plug byte=%u was a duplicate -- host refunded the item", byte);
        return;
    }
    // unplug no-op: our unplug raced another peer's -- our hand/world ghost is
    // illegitimate. Find UNTRACKED (eid-less) actors of the byte's class.
    void* cls = PM::ClassForByte(byte);
    if (!cls) { UE_LOGW("physmods: deny byte=%u -- class unresolved, ghost NOT swept", byte); return; }
    const std::wstring clsName = R::ToString(R::NameOf(cls));
    int swept = 0;
    for (void* a : R::FindObjectsByClass(clsName.c_str())) {
        if (!a || !R::IsLive(a)) continue;
        if (coop::prop_element_tracker::GetPropElementIdForActor(a) !=
            coop::element::kInvalidId) continue;  // tracked = legit
        coop::prop_lifecycle::DestroyLocalProp(a, /*deferred*/true);
        ++swept;
    }
    UE_LOGW("physmods: unplug byte=%u denied (raced) -- swept %d untracked ghost(s)", byte, swept);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_editsWatched)
        g_editsWatched =
            sg::WatchClassName(L"analogDScreenTest_C", L"plugInModule", kTagPlug, &OnEditPre, &OnEditPost) &&
            sg::WatchClassName(L"analogDScreenTest_C", L"playerHitWith", kTagHit, &OnEditPre, &OnEditPost);
}

void Tick() {
    if (!g_havePendingCanon) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextParkedTry) return;
    g_nextParkedTry = now + kParkedRetry;
    if (!PM::EnsureResolved() || !CD::Instance()) return;  // backoff inside
    AdoptCanonical(g_pendingCanon);
    g_havePendingCanon = false;
    UE_LOGI("physmods: parked canonical applied at desk resolve");
}

void OnPhysMods(const coop::net::PhysModsStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.op > 3) return;
    const bool isHost = (s->role() == coop::net::Role::Host);

    if (p.op == 2) {  // canonical array (host-authored)
        if (isHost) return;  // the host IS the canonical source
        if (senderSlot != 0) {
            UE_LOGW("physmods: canonical from non-host slot=%u -- dropping", senderSlot);
            return;
        }
        if (!PM::EnsureResolved() || !CD::Instance()) {
            std::memcpy(g_pendingCanon, p.bytes, PM::kSlots);
            g_havePendingCanon = true;
            UE_LOGI("physmods: canonical parked (desk unresolved)");
            return;
        }
        // A local edit was sent at its verb, so adopting the canonical eats nothing.
        AdoptCanonical(p.bytes);
        static uint64_t s_n = 0;
        if ((++s_n % 8) == 1)
            UE_LOGI("physmods: canonical adopted (n=%llu)", (unsigned long long)s_n);
        return;
    }

    if (p.op == 3) {  // deny (host -> this author)
        if (isHost) return;
        if (senderSlot != 0) return;
        ClientHandleDeny(p.byte, p.byte2);
        return;
    }

    // op 0/1: a value op. HOST-terminal.
    if (!isHost) {
        UE_LOGW("physmods: op=%u reached a client -- protocol violation, dropping", p.op);
        return;
    }
    if (!PM::EnsureResolved() || !CD::Instance()) {
        UE_LOGW("physmods: host op=%u byte=%u declined (desk unresolved)", p.op, p.byte);
        return;
    }
    uint8_t arr[PM::kSlots];
    if (!PM::ReadArray(arr)) return;

    if (p.op == 0) {  // plug{byte}
        bool dup = false;
        for (int i = 0; i < PM::kSlots; ++i) if (arr[i] == p.byte) { dup = true; break; }
        if (dup) {
            // Same-byte double-plug race: deny + REFUND (spawn the module back
            // at the desk; the host's spawn watcher expresses + fans it).
            if (senderSlot != 0 && senderSlot < coop::net::kMaxPeers) {
                coop::net::PhysModsStatePayload d{};
                d.op = 3; d.byte = 0; d.byte2 = p.byte;
                s->SendReliableToSlot(senderSlot, coop::net::ReliableKind::PhysModsState,
                                      &d, sizeof(d));
            }
            void* cls = PM::ClassForByte(p.byte);
            void* desk = CD::Instance();
            ue_wrap::FVector deskLoc{};
            if (cls && desk && ue_wrap::engine::TryGetActorLocation(desk, deskLoc)) {
                void* refunded = ue_wrap::engine::SpawnActor(
                    cls, {deskLoc.X, deskLoc.Y, deskLoc.Z + 120.f});
                UE_LOGW("physmods: plug byte=%u DUP from slot %u -- denied + refund %s",
                        p.byte, senderSlot, refunded ? "spawned" : "SPAWN FAILED");
            } else {
                UE_LOGW("physmods: plug byte=%u DUP from slot %u -- denied, refund %s (item lost)", p.byte, senderSlot,
                        (cls && desk) ? "not placed: the desk's location unread" : "class unresolved");
            }
            return;
        }
        int free = -1;
        for (int i = 0; i < PM::kSlots; ++i) if (arr[i] == 0) { free = i; break; }
        if (free < 0) {
            UE_LOGW("physmods: plug byte=%u from slot %u -- array full?! dropping", p.byte, senderSlot);
            return;
        }
        arr[free] = p.byte;
    } else {  // unplug{byte}
        int at = -1;
        for (int i = 0; i < PM::kSlots; ++i) if (arr[i] == p.byte) { at = i; break; }
        if (at < 0) {
            // No-op unplug (raced): deny -> the author destroys its ghost.
            if (senderSlot != 0 && senderSlot < coop::net::kMaxPeers) {
                RecordDeny(senderSlot, p.byte);
                coop::net::PhysModsStatePayload d{};
                d.op = 3; d.byte = 1; d.byte2 = p.byte;
                s->SendReliableToSlot(senderSlot, coop::net::ReliableKind::PhysModsState,
                                      &d, sizeof(d));
                UE_LOGW("physmods: unplug byte=%u from slot %u raced (absent) -- deny sent",
                        p.byte, senderSlot);
            }
            return;
        }
        arr[at] = 0;
    }
    {
        coop::desk_snd_fx::ScopedWireApply guard;
        if (!PM::WriteArray(arr)) {  // never baseline or broadcast an array the write refused
            UE_LOGW("physmods: host WriteArray failed -- op=%u byte=%u not applied", p.op, p.byte);
            return;
        }
        PM::CallUpdPhysMods();
    }
    HostBroadcastCanonical(s);
    UE_LOGI("physmods: host applied op=%u byte=%u from slot %u -- canonical broadcast",
            p.op, p.byte, senderSlot);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!PM::EnsureResolved() || !CD::Instance()) return;
    HostBroadcastCanonical(s, slot);
    UE_LOGI("physmods: canonical -> joiner slot %d", slot);
}

bool HostShouldReapModuleBirth(uint8_t senderSlot, void* moduleClass) {
    if (!moduleClass) return false;
    const uint8_t byte = PM::ByteForClass(moduleClass);
    if (!byte) return false;
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.slot == senderSlot && d.byte == byte && now < d.until) {
            d = DenyRec{};  // one reap per deny
            UE_LOGW("physmods: reaped a denied module birth (slot=%u byte=%u -- the raced ghost)",
                    senderSlot, byte);
            return true;
        }
    }
    return false;
}

void OnDisconnect() {
    g_havePendingCanon = false;
    g_inFlight.clear();
    for (auto& d : g_denies) d = DenyRec{};
    g_nextParkedTry = {};
    PM::ResetCache();
}

}  // namespace coop::physmods_sync
