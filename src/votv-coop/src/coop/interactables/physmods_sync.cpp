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
// the ghost of a module that was dropped before its deny landed. Keyed on the
// sender and the module's type, the one identity the birth carries here.
struct DenyRec {
    uint8_t sender = 0xFF;  // the denied op's peer slot
    uint8_t byte = 0;
    Clock::time_point until{};
};
constexpr int kDenyRecs = 8;
constexpr auto kDenyTtl = std::chrono::seconds(10);
DenyRec g_denies[kDenyRecs];

constexpr uint8_t kOpPlug      = 0;
constexpr uint8_t kOpUnplug    = 1;
constexpr uint8_t kOpCanonical = 2;
constexpr uint8_t kOpDeny      = 3;

void SendOp(coop::net::Session* s, uint8_t op, int slot, uint8_t byte) {
    // CLIENT-only: the host's organic changes never ride ops -- its live array
    // IS the canonical. A host that self-applied an already-applied op hit the
    // occupied and absent branches, refunding a phantom and broadcasting nothing.
    coop::net::PhysModsStatePayload p{};
    p.op = op;
    p.byte = byte;
    p.slot = static_cast<uint8_t>(slot);
    s->SendReliableToSlot(0, coop::net::ReliableKind::PhysModsState, &p, sizeof(p));
}

void HostBroadcastCanonical(coop::net::Session* s, int onlySlot = -1) {
    coop::net::PhysModsStatePayload p{};
    p.op = kOpCanonical;
    if (!PM::ReadArray(p.bytes)) return;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::PhysModsState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::PhysModsState, &p, sizeof(p));
}

// A refused op goes back to its author with the canonical array behind it: the author's own array
// left the canonical when its verb ran, and adopting it is what brings the two together again. MTA
// answers a refused element-data change the same way, with the server's value sent to that player
// alone (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2779, Packet_CustomData).
void SendDeny(coop::net::Session* s, uint8_t author, const coop::net::PhysModsStatePayload& op) {
    if (author == 0 || author >= coop::net::kMaxPeers) return;
    coop::net::PhysModsStatePayload d{};
    d.op = kOpDeny;
    d.byte = op.op;
    d.byte2 = op.byte;
    d.slot = op.slot;
    s->SendReliableToSlot(author, coop::net::ReliableKind::PhysModsState, &d, sizeof(d));
    HostBroadcastCanonical(s, author);
}

// The ops of one local edit, slot by slot: a slot that lost its module is an unplug of it, one that
// gained a module a plug of it. The desk takes two modules of one type (isModuleAllowed is a list of
// types, not a duplicate check), so an op names its slot as well as its module.
void SendLocalEdit(coop::net::Session* s, const uint8_t before[PM::kSlots], const uint8_t live[PM::kSlots]) {
    const bool isHost = (s->role() == coop::net::Role::Host);
    bool changed = false;
    for (int i = 0; i < PM::kSlots; ++i) {
        if (before[i] == live[i]) continue;
        changed = true;
        if (before[i]) {
            UE_LOGI("physmods: local UNPLUG slot=%d byte=%u -- %s", i, before[i],
                    isHost ? "canonical will carry it" : "op to host");
            if (!isHost) SendOp(s, kOpUnplug, i, before[i]);
        }
        if (live[i]) {
            UE_LOGI("physmods: local PLUG slot=%d byte=%u -- %s", i, live[i],
                    isHost ? "canonical will carry it" : "op to host");
            if (!isHost) SendOp(s, kOpPlug, i, live[i]);
        }
    }
    // The host's live array IS the canonical, so ONE broadcast carries any
    // number of organic changes.
    if (isHost && changed && s->connected()) HostBroadcastCanonical(s);
}

uint64_t g_adopted = 0;  // canonical arrays written into this peer's desk, game thread

bool AdoptCanonical(const uint8_t bytes[PM::kSlots]) {
    coop::desk_snd_fx::ScopedWireApply guard;
    if (!PM::WriteArray(bytes)) return false;
    ++g_adopted;
    PM::CallUpdPhysMods();
    return true;
}

// The edits from play are the desk's two verbs that write the array (its ubergraph's control flow):
// plugInModule, a module into an empty slot, which the in-hand use (playerUsedOn) and a module dropped
// onto a slot (the slots' overlap events) both run; and actionOptionIndex, the E press, whose one write
// is the unplug: the slot's module is reborn into the hand and the slot goes to 0. playerHitWith is an
// empty body on the desk. setData writes the array wholesale on a save load and is not an edit, so a
// joiner's load sends nothing; nor are this lane's own applies, which write the array and run
// updPhysMods, never a verb. Each watched body snapshots the array at entry and sends what it changed
// at exit. Neither verb runs the other; a module reborn onto a slot's trigger would plug inside the
// press, so after a send every enclosing body's snapshot moves to the array as sent, and a change is
// sent once.
constexpr const wchar_t* kDeskClass = L"analogDScreenTest_C";
constexpr const wchar_t* kPlugVerb  = L"plugInModule";
constexpr const wchar_t* kPressVerb = L"actionOptionIndex";
constexpr int kTagPlug  = 0x504C5547;  // 'PLUG'
constexpr int kTagPress = 0x50505253;  // 'PPRS'
bool g_editsWatched = false;

struct InFlight {
    int     depth;  // the body's place on the gate's chain of watched bodies
    void*   stack;
    uint8_t before[PM::kSlots];
};
std::vector<InFlight> g_inFlight;  // game thread only

// The gate's own chain is the scope: a body at depth d has only its ancestors below it, so an entry
// at d or deeper belongs to a body that ended without its post (another watcher's Cancel, or a fault
// the firewall absorbed, skips every post). Such an entry goes, and the stack never outgrows the chain.
void DropFrom(int depth) {
    while (!g_inFlight.empty() && g_inFlight.back().depth >= depth) g_inFlight.pop_back();
}

sg::Verdict OnEditPre(const sg::Call& call) {
    DropFrom(call.depth);
    InFlight f{call.depth, call.stack, {}};
    if (PM::EnsureResolved() && PM::ReadArray(f.before)) g_inFlight.push_back(f);
    return sg::Verdict::Run;
}

void OnEditPost(const sg::Call& call) {
    DropFrom(call.depth + 1);
    if (g_inFlight.empty() || g_inFlight.back().depth != call.depth || g_inFlight.back().stack != call.stack)
        return;  // its entry read no array
    uint8_t before[PM::kSlots];
    std::memcpy(before, g_inFlight.back().before, PM::kSlots);
    g_inFlight.pop_back();
    uint8_t live[PM::kSlots];
    if (!PM::ReadArray(live) || std::memcmp(before, live, PM::kSlots) == 0) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->connected()) SendLocalEdit(s, before, live);
    for (InFlight& outer : g_inFlight) std::memcpy(outer.before, live, PM::kSlots);
}

void RecordDeny(uint8_t sender, uint8_t byte) {
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.sender == 0xFF || now >= d.until) { d = {sender, byte, now + kDenyTtl}; return; }
    }
    g_denies[0] = {sender, byte, now + kDenyTtl};  // overwrite oldest-slot-0 (bounded)
}

// CLIENT deny handling: destroy the local hand ghost, else sweep untracked
// module actors of the byte's class (the drop-before-deny case). The canonical
// array follows the deny and puts this peer's desk back.
void ClientHandleDeny(uint8_t origOp, uint8_t slot, uint8_t byte) {
    if (origOp == kOpPlug) {
        // The slot was taken first: the HOST refunded (spawned the module back at
        // the desk); our local item is already destroyed by our own plugInModule.
        UE_LOGW("physmods: plug slot=%u byte=%u refused, the slot was taken first -- host refunded the item",
                slot, byte);
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
    UE_LOGW("physmods: unplug slot=%u byte=%u denied (raced) -- swept %d untracked ghost(s)", slot, byte, swept);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_editsWatched)
        g_editsWatched = sg::WatchClassName(kDeskClass, kPlugVerb, kTagPlug, &OnEditPre, &OnEditPost) &&
                         sg::WatchClassName(kDeskClass, kPressVerb, kTagPress, &OnEditPre, &OnEditPost);
}

void Tick() {
    if (!g_havePendingCanon) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const auto now = Clock::now();
    if (now < g_nextParkedTry) return;
    g_nextParkedTry = now + kParkedRetry;
    if (!PM::EnsureResolved() || !CD::Instance()) return;  // backoff inside
    g_havePendingCanon = false;
    if (AdoptCanonical(g_pendingCanon))
        UE_LOGI("physmods: parked canonical applied at desk resolve");
    else
        UE_LOGW("physmods: parked canonical not written at desk resolve (WriteArray refused it)");
}

void OnPhysMods(const coop::net::PhysModsStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.op > kOpDeny) return;
    const bool isHost = (s->role() == coop::net::Role::Host);

    if (p.op == kOpCanonical) {  // host-authored
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
        if (AdoptCanonical(p.bytes) && (g_adopted % 8) == 1)
            UE_LOGI("physmods: canonical adopted (n=%llu)", (unsigned long long)g_adopted);
        return;
    }

    if (p.op == kOpDeny) {  // host -> this author
        if (isHost) return;
        if (senderSlot != 0) return;
        ClientHandleDeny(p.byte, p.slot, p.byte2);
        return;
    }

    // op 0/1: a value op. HOST-terminal.
    if (!isHost) {
        UE_LOGW("physmods: op=%u reached a client -- protocol violation, dropping", p.op);
        return;
    }
    if (!PM::EnsureResolved() || !CD::Instance()) {
        UE_LOGW("physmods: host op=%u slot=%u byte=%u declined (desk unresolved)", p.op, p.slot, p.byte);
        return;
    }
    // A client op names a desk slot and a module the desk itself takes (its isModuleAllowed), or it is
    // refused: the host writes nothing else into its array, its save or its updPhysMods, and the author
    // gets the canonical back, as for any refusal.
    if (p.slot >= PM::kSlots || !PM::IsModuleAllowed(p.byte)) {
        UE_LOGW("physmods: host op=%u slot=%u byte=%u from slot %u -- not a desk slot and module it takes, "
                "refused", p.op, p.slot, p.byte, senderSlot);
        if (senderSlot != 0 && senderSlot < coop::net::kMaxPeers) HostBroadcastCanonical(s, senderSlot);
        return;
    }
    uint8_t arr[PM::kSlots];
    if (!PM::ReadArray(arr)) return;

    if (p.op == kOpPlug) {
        if (arr[p.slot] != 0) {
            // Two peers plugged into one slot and this op came second: deny + REFUND (spawn the module
            // back at the desk; the host's spawn watcher expresses + fans it).
            SendDeny(s, senderSlot, p);
            void* cls = PM::ClassForByte(p.byte);
            void* desk = CD::Instance();
            ue_wrap::FVector deskLoc{};
            if (cls && desk && ue_wrap::engine::TryGetActorLocation(desk, deskLoc)) {
                void* refunded = ue_wrap::engine::SpawnActor(
                    cls, {deskLoc.X, deskLoc.Y, deskLoc.Z + 120.f});
                UE_LOGW("physmods: plug slot=%u byte=%u from slot %u -- the slot holds byte=%u, denied + refund %s",
                        p.slot, p.byte, senderSlot, arr[p.slot], refunded ? "spawned" : "SPAWN FAILED");
            } else {
                UE_LOGW("physmods: plug slot=%u byte=%u from slot %u -- the slot holds byte=%u, denied, refund %s "
                        "(item lost)", p.slot, p.byte, senderSlot, arr[p.slot],
                        (cls && desk) ? "not placed: the desk's location unread" : "class unresolved");
            }
            return;
        }
        arr[p.slot] = p.byte;
    } else {  // kOpUnplug
        if (arr[p.slot] != p.byte) {
            // Raced: the slot was emptied or refilled first. Deny -> the author destroys its ghost.
            if (senderSlot != 0 && senderSlot < coop::net::kMaxPeers) RecordDeny(senderSlot, p.byte);
            SendDeny(s, senderSlot, p);
            UE_LOGW("physmods: unplug slot=%u byte=%u from slot %u raced (the slot holds byte=%u) -- deny sent",
                    p.slot, p.byte, senderSlot, arr[p.slot]);
            return;
        }
        arr[p.slot] = 0;
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
    UE_LOGI("physmods: host applied op=%u slot=%u byte=%u from slot %u -- canonical broadcast",
            p.op, p.slot, p.byte, senderSlot);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!PM::EnsureResolved() || !CD::Instance()) return;
    HostBroadcastCanonical(s, slot);
    UE_LOGI("physmods: canonical -> joiner slot %d", slot);
}

uint64_t CanonicalsAdopted() { return g_adopted; }

bool HostShouldReapModuleBirth(uint8_t senderSlot, void* moduleClass) {
    if (!moduleClass) return false;
    const uint8_t byte = PM::ByteForClass(moduleClass);
    if (!byte) return false;
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.sender == senderSlot && d.byte == byte && now < d.until) {
            d = DenyRec{};  // one reap per deny
            UE_LOGW("physmods: reaped a denied module birth (from slot %u, byte=%u -- the raced ghost)",
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
