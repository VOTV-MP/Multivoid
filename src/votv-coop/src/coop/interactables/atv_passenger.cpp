// coop/interactables/atv_passenger.cpp -- see coop/interactables/atv_passenger.h. The host's table is atv_authority.cpp's; this file
// is this machine's own body against it, the table's wire (send, receive, replay) and the remote riders' puppets.

#include "coop/interactables/atv_passenger.h"

#include "coop/input/input_owner.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_index_internal.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_seat.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/atv_passenger.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::atv_passenger {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
namespace AP = ue_wrap::atv_passenger;
namespace AU = coop::atv_authority;
namespace E = ue_wrap::engine;
using coop::atv_sync::AtvEntry;
using coop::atv_sync::detail::g_atvs;
using coop::net::StringFromWireKey;
using coop::net::WireKeyFromString;
using ue_wrap::FVector;

constexpr uint8_t kFree = 0xFF;
constexpr int kSeats = coop::net::kAtvPassengerSeats;
constexpr float kStrayCm = 150.f;          // a rider this far from its seat has been moved by something else
constexpr uint64_t kUseDebounceMs = 450;   // the use key's press and release are two events: one board or leave per press
constexpr uint64_t kJumpGraceMs = 400;     // the key that boarded is not the key that leaves
constexpr uint64_t kMismatchMs = 1500;     // the table seats this player and its own body does not, this long: it leaves
constexpr size_t kMaxPending = 16;

// This machine's own body. One seat at a time: the player is a passenger of at most one ATV.
struct Local {
    bool         seated = false;
    std::wstring key;              // the ATV
    void*        player = nullptr;
    int          seat = -1;
    uint8_t      pendingSeq = 0;   // the latest request sent and not yet answered (0 = none)
    bool         pendingBoard = false;
    uint32_t     worldGen = 0;
    uint64_t     seatedAtMs = 0;
    bool         jumpHeld = false;
};
Local g_local;
uint64_t g_debounceUntilMs = 0;
uint64_t g_mismatchSinceMs = 0;
uint8_t  g_seq = 0;

// A table that reached a client before its ATV's row did (the join replay's order is the ATV's announce, its state, then
// this): kept, the latest per key, and applied once the row exists.
std::unordered_map<std::wstring, coop::net::AtvPassengerTablePayload> g_pending;

// The seat this machine last gave the body's owner (coop/player/puppet_seat) for each remote rider, by slot.
struct PuppetSeat {
    bool         seated = false;
    std::wstring key;
    int          seat = -1;
};
PuppetSeat g_pup[coop::players::kMaxPeers];

uint64_t Now() { return coop::atv_sync::NowMs(); }
uint8_t LocalSlot() { return coop::players::Registry::Get().LocalPeerId(); }

coop::net::Session* ActiveSession() {
    coop::net::Session* s = coop::atv_sync::LaneSession();
    return s && s->connected() ? s : nullptr;
}

AtvEntry* LiveEntry(const std::wstring& key) {
    if (!coop::atv_sync::IndexCurrent()) return nullptr;
    auto it = g_atvs.find(key);
    if (it == g_atvs.end() || !R::IsLiveByIndex(it->second.actor, it->second.idx)) return nullptr;
    return &it->second;
}

// The live row whose actor is `atv`.
AtvEntry* EntryOfActor(void* atv, std::wstring* key) {
    if (!atv || !coop::atv_sync::IndexCurrent()) return nullptr;
    for (auto& kv : g_atvs) {
        if (kv.second.actor != atv || !R::IsLiveByIndex(kv.second.actor, kv.second.idx)) continue;
        if (key) *key = kv.first;
        return &kv.second;
    }
    return nullptr;
}

uint8_t NextSeq() {
    g_seq = g_seq >= 255 ? uint8_t{1} : static_cast<uint8_t>(g_seq + 1);
    return g_seq;
}

// CLIENT: one request to the host, its seq the latest. False when the send was refused.
bool SendRequest(coop::net::Session& s, const std::wstring& key, bool board) {
    coop::net::AtvPassengerRequestPayload p{};
    WireKeyFromString(key, p.key);
    p.action = board ? 1 : 0;
    p.seq = NextSeq();
    if (!s.SendReliable(coop::net::ReliableKind::AtvPassengerRequest, &p, sizeof(p))) return false;
    g_local.pendingSeq = p.seq;
    g_local.pendingBoard = board;
    return true;
}

bool PlayerIsDownOrDead(void* player) {
    bool ragdoll = false, dead = false;
    return E::ReadMainPlayerRagdollState(player, ragdoll, dead) && (ragdoll || dead);
}

int FirstFree(const AtvEntry& e) {
    for (int i = 0; i < kSeats; ++i)
        if (e.passengerSlot[i] == kFree) return i;
    return -1;
}

void ClearSlotFrom(AtvEntry& e, uint8_t slot) {
    for (int i = 0; i < kSeats; ++i)
        if (e.passengerSlot[i] == slot) e.passengerSlot[i] = kFree;
}

// Take this machine's rider off its seat. `place` puts it beside the ATV (or on the ground under it when the ATV is gone);
// `notify` tells the host (the host's own table directly, a client by AtvPassengerRequest). The state ends here either way.
void LeaveImpl(const char* why, bool place, bool notify) {
    if (!g_local.seated) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player || player != g_local.player) player = R::IsLive(g_local.player) ? g_local.player : nullptr;
    AtvEntry* e = LiveEntry(g_local.key);
    FVector spot{};
    bool haveSpot = false;
    if (place && player) {
        const float half = E::GetActorCharacterHalfHeight(player);
        if (e) {
            haveSpot = AP::FindExitSpot(e->actor, g_local.seat, half, spot);
        } else {
            FVector at{};
            haveSpot = E::TryGetActorLocation(player, at) && AP::FindGroundBelow(player, at, half, spot);
        }
    }
    if (player) AP::ReleaseRider(player, true, haveSpot ? &spot : nullptr);
    coop::net::Session* s = ActiveSession();
    bool asked = false;
    if (notify && s) {
        if (s->role() == coop::net::Role::Host) {
            if (e) AU::JudgePassengerLocal(*e, false, nullptr);
        } else {
            asked = SendRequest(*s, g_local.key, false);
        }
    }
    UE_LOGI("atv_passenger: left seat %d of '%ls' -- %s%s", g_local.seat, g_local.key.c_str(), why,
            place ? (haveSpot ? ", placed beside the ATV" : ", no spot found, left where it stands") : "");
    if (e) ClearSlotFrom(*e, LocalSlot());
    // A leave still unanswered keeps its seq: its answer is what the table's copy waits on.
    const uint8_t pending = asked ? g_local.pendingSeq : uint8_t{0};
    g_local = Local{};
    g_local.pendingSeq = pending;
    g_debounceUntilMs = Now() + kUseDebounceMs;
}

}  // namespace

bool LocalIsPassenger() { return g_local.seated; }

LocalView ReadLocal() {
    LocalView v;
    v.seated = g_local.seated;
    v.seat = g_local.seat;
    v.key = g_local.key;
    if (AtvEntry* e = LiveEntry(g_local.key)) v.atv = e->actor;
    return v;
}

int SeatOfSlot(const std::wstring& key, uint8_t slot, bool* outAttached) {
    if (outAttached) *outAttached = false;
    AtvEntry* e = LiveEntry(key);
    if (!e) return -1;
    const int seat = AU::PassengerSeatOf(*e, slot);
    if (seat < 0 || !outAttached) return seat;
    if (slot == LocalSlot()) {
        *outAttached = AP::IsRiderOf(coop::players::Registry::Get().Local(), e->actor);
    } else if (coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot); rp && rp->valid()) {
        *outAttached = AP::IsRiderOf(rp->GetActor(), e->actor);
    }
    return seat;
}

void LeaveLocal(const char* why, bool place) {
    if (!g_local.seated) return;
    LeaveImpl(why, place, /*notify*/true);
}

bool LeaveFromUse() {
    if (!g_local.seated) return false;
    if (Now() < g_debounceUntilMs) return true;   // the press that boarded, or its release: spent, not acted on
    LeaveImpl("the use key", /*place*/true, /*notify*/true);
    return true;
}

bool BoardFromUse(void* atv) {
    coop::net::Session* s = ActiveSession();
    void* player = coop::players::Registry::Get().Local();
    if (!s || !player || !atv || g_local.seated) return false;
    if (Now() < g_debounceUntilMs) return true;   // a press just spent: no second verdict and no deny click
    const bool isHost = s->role() == coop::net::Role::Host;
    if (!isHost && !coop::net_pump::HasAnnouncedWorldReady()) return false;
    std::wstring key;
    AtvEntry* e = EntryOfActor(atv, &key);
    if (!e) return false;
    if (PlayerIsDownOrDead(player)) return false;
    const char* why = nullptr;
    if (!AP::MayBoard(player, &why)) {
        E::MainPlayerGrabState gs{};
        const bool read = E::ReadMainPlayerGrabState(player, gs);
        UE_LOGI("atv_passenger: the press at '%ls' does not board -- %s (hand %ls, grabbing %ls)", key.c_str(),
                why ? why : "refused", read && gs.holdingActor ? R::ClassNameOf(gs.holdingActor).c_str() : L"none",
                read && gs.grabbingActor ? R::ClassNameOf(gs.grabbingActor).c_str() : L"none");
        return false;
    }
    int seat = -1;
    if (isHost) {
        if (!AU::JudgePassengerLocal(*e, true, &seat)) {
            UE_LOGI("atv_passenger: the host's board of '%ls' is refused by the table", key.c_str());
            return false;
        }
    } else {
        seat = FirstFree(*e);
        if (seat < 0) {
            UE_LOGI("atv_passenger: every passenger seat of '%ls' is taken", key.c_str());
            return false;
        }
    }
    if (!AP::SeatRider(player, atv, seat, /*local*/true)) {
        if (isHost) AU::JudgePassengerLocal(*e, false, nullptr);
        UE_LOGW("atv_passenger: the board of '%ls' seat %d did not take (the attach did not dispatch)", key.c_str(), seat);
        return false;
    }
    g_local = Local{};
    g_local.seated = true;
    g_local.key = key;
    g_local.player = player;
    g_local.seat = seat;
    g_local.worldGen = ue_wrap::world_identity::Generation();
    g_local.seatedAtMs = Now();
    g_debounceUntilMs = Now() + kUseDebounceMs;
    if (!isHost) {
        e->passengerSlot[seat] = LocalSlot();   // this machine's own view, until the host's table says
        if (!SendRequest(*s, key, true)) {
            AP::ReleaseRider(player, true);
            e->passengerSlot[seat] = kFree;
            g_local = Local{};
            UE_LOGW("atv_passenger: the board request for '%ls' was not sent -- the player is off the seat", key.c_str());
            return false;
        }
    }
    UE_LOGI("atv_passenger: boarded seat %d of '%ls' (%s)", seat, key.c_str(),
            isHost ? "the host's own table" : "asked of the host, acting at once");
    return true;
}

void OnAnswer(const coop::net::AtvPassengerAnswerPayload& a) {
    const std::wstring key = StringFromWireKey(a.key);
    if (a.seq != g_local.pendingSeq) {
        UE_LOGI("atv_passenger: answer for '%ls' seq %u is not the latest request (%u) -- older, logged only", key.c_str(),
                static_cast<unsigned>(a.seq), static_cast<unsigned>(g_local.pendingSeq));
        return;
    }
    const bool wasBoard = g_local.pendingBoard;
    g_local.pendingSeq = 0;
    AtvEntry* e = LiveEntry(key);
    const uint8_t me = LocalSlot();
    UE_LOGI("atv_passenger: host %s '%ls' seq %u (reason %u) -- seat %u", a.admitted ? "granted" : "refused", key.c_str(),
            static_cast<unsigned>(a.seq), static_cast<unsigned>(a.reason), static_cast<unsigned>(a.seat));
    if (!wasBoard) {
        if (e) ClearSlotFrom(*e, me);   // the leave stood: the table copy says so until the host's own send
        return;
    }
    if (!g_local.seated || g_local.key != key) return;
    if (a.seat != kFree && a.seat < kSeats) {
        if (e) { ClearSlotFrom(*e, me); e->passengerSlot[a.seat] = me; }
        if (a.seat != g_local.seat) {
            if (e) AP::MoveRider(g_local.player, e->actor, a.seat);
            g_local.seat = a.seat;
        }
        return;
    }
    // Refused: the rider yields, beside the ATV, and the host holds no seat of this machine's.
    if (e) ClearSlotFrom(*e, me);
    LeaveImpl("the host refused the board", /*place*/true, /*notify*/false);
}

namespace {

// A table applied to its row, and this machine's own body brought to it.
void ApplyTable(AtvEntry& e, const std::wstring& key, const coop::net::AtvPassengerTablePayload& t) {
    for (int i = 0; i < kSeats; ++i) e.passengerSlot[i] = t.slot[i];
    if (!g_local.seated || g_local.key != key || g_local.pendingSeq != 0) return;
    const int seat = AU::PassengerSeatOf(e, LocalSlot());
    if (seat < 0) {
        LeaveImpl("the host's table no longer seats this player", /*place*/true, /*notify*/false);
    } else if (seat != g_local.seat) {
        AP::MoveRider(g_local.player, e.actor, seat);
        g_local.seat = seat;
    }
}

bool SendTable(coop::net::Session& s, int toSlot, const std::wstring& key, const AtvEntry& e) {
    coop::net::AtvPassengerTablePayload t{};
    WireKeyFromString(key, t.key);
    for (int i = 0; i < kSeats; ++i) t.slot[i] = e.passengerSlot[i];
    bool sent = true;
    if (toSlot >= 0) {
        sent = s.SendReliableToSlot(static_cast<uint8_t>(toSlot), coop::net::ReliableKind::AtvPassengerTable, &t, sizeof(t));
    } else {
        for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot)
            if (s.IsSlotWorldReady(slot))
                sent = s.SendReliableToSlot(slot, coop::net::ReliableKind::AtvPassengerTable, &t, sizeof(t)) && sent;
    }
    return sent;
}

bool AnyRider(const AtvEntry& e) {
    for (int i = 0; i < kSeats; ++i)
        if (e.passengerSlot[i] != kFree) return true;
    return false;
}

// This machine's own rider, kept honest. The first rule that holds ends it.
void TickLocal(uint64_t now) {
    if (!g_local.seated) return;
    if (g_local.worldGen != ue_wrap::world_identity::Generation()) {
        // A new world: the rider's body and the ATV are another world's. Let the old body go where it stands.
        if (R::IsLive(g_local.player)) AP::ReleaseRider(g_local.player, true);
        UE_LOGI("atv_passenger: the world changed -- seat %d of '%ls' is let go", g_local.seat, g_local.key.c_str());
        g_local = Local{};
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    if (!player || player != g_local.player) {
        UE_LOGI("atv_passenger: the local player is another body -- seat %d of '%ls' is let go", g_local.seat,
                g_local.key.c_str());
        g_local = Local{};
        return;
    }
    AtvEntry* e = LiveEntry(g_local.key);
    if (!e) return LeaveImpl("the ATV is gone", /*place*/true, /*notify*/false);
    if (PlayerIsDownOrDead(player)) return LeaveImpl("the player is down", /*place*/false, /*notify*/true);
    if (A::GetHealth(e->actor) <= 0.f) return LeaveImpl("the ATV was destroyed", /*place*/true, /*notify*/true);
    if (!AP::IsRiderOf(player, e->actor)) return LeaveImpl("something detached the player", /*place*/false, /*notify*/true);
    FVector seatAt{}, at{};
    if (AP::SeatWorldLocation(e->actor, g_local.seat, seatAt) && E::TryGetActorLocation(player, at)) {
        const FVector d{at.X - seatAt.X, at.Y - seatAt.Y, at.Z - seatAt.Z};
        if (coop::atv_sync::Len(d) > kStrayCm) return LeaveImpl("the player was moved off its seat", /*place*/false, /*notify*/true);
    }
    // The jump key leaves, as the use key does. A key held from before the board is not a press.
    const bool down = coop::input::input_owner::MayTakeKey(VK_SPACE) && (::GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    const bool pressed = down && !g_local.jumpHeld;
    g_local.jumpHeld = down;
    if (pressed && now - g_local.seatedAtMs >= kJumpGraceMs) LeaveImpl("the jump key", /*place*/true, /*notify*/true);
}

// CLIENT: tables that waited for their row, and the one this player holds that its own body does not.
void TickClient(coop::net::Session& s, uint64_t now) {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (AtvEntry* e = LiveEntry(it->first)) {
            ApplyTable(*e, it->first, it->second);
            it = g_pending.erase(it);
        } else {
            ++it;
        }
    }
    // The table seats this player on an ATV its own body is not on and no request is out: the table is stale or a leave was
    // lost. Held for a while first (a leave's own send is on its way), then the player asks to leave.
    if (g_local.seated || g_local.pendingSeq != 0 || !coop::net_pump::HasAnnouncedWorldReady()) {
        g_mismatchSinceMs = 0;
        return;
    }
    const uint8_t me = LocalSlot();
    for (auto& kv : g_atvs) {
        if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx) || AU::PassengerSeatOf(kv.second, me) < 0) continue;
        if (g_mismatchSinceMs == 0) { g_mismatchSinceMs = now; return; }
        if (now - g_mismatchSinceMs < kMismatchMs) return;
        g_mismatchSinceMs = 0;
        coop::net::AtvPassengerRequestPayload p{};
        WireKeyFromString(kv.first, p.key);
        p.action = 0;
        p.seq = NextSeq();
        if (s.SendReliable(coop::net::ReliableKind::AtvPassengerRequest, &p, sizeof(p))) g_local.pendingSeq = p.seq;
        ClearSlotFrom(kv.second, me);
        UE_LOGI("atv_passenger: the table seated this player on '%ls' with no body there -- asked to leave", kv.first.c_str());
        return;
    }
    g_mismatchSinceMs = 0;
}

}  // namespace

// The remote riders' bodies. The table says who sits where; the bodies' one owner (coop/player/puppet_seat) attaches each
// to this machine's copy of the ATV, seat index 1 + the passenger seat. A slot's seat is told to it when the table's
// answer for the slot changed, and never per frame.
void SyncPuppets() {
    const uint8_t me = LocalSlot();
    PuppetSeat want[coop::players::kMaxPeers];
    if (ActiveSession() && coop::atv_sync::IndexCurrent()) {
        for (auto& kv : g_atvs) {
            const AtvEntry& e = kv.second;
            if (!R::IsLiveByIndex(e.actor, e.idx)) continue;
            for (int i = 0; i < kSeats; ++i) {
                const uint8_t who = e.passengerSlot[i];
                if (who == kFree || who == me || who >= coop::players::kMaxPeers) continue;
                want[who].seated = true;
                want[who].key = kv.first;
                want[who].seat = i;
            }
        }
    }
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        PuppetSeat& ps = g_pup[slot];
        const PuppetSeat& w = want[slot];
        if (ps.seated == w.seated && ps.seat == w.seat && ps.key == w.key) continue;
        if (w.seated) {
            coop::puppet_seat::Set(slot, w.key, static_cast<uint8_t>(1 + w.seat));
            UE_LOGI("atv_passenger: slot %u sits at passenger seat %d of '%ls' on this machine's copy", static_cast<unsigned>(slot),
                    w.seat, w.key.c_str());
        } else {
            // A rider that took the driver's seat of the same ATV holds the driver's target there, which is not this one's to clear.
            if (coop::atv_sync::ActorSeatedBy(slot) == nullptr) coop::puppet_seat::Clear(slot, ps.key);
            UE_LOGI("atv_passenger: slot %u left passenger seat %d of '%ls' -- its body follows the pose stream again",
                    static_cast<unsigned>(slot), ps.seat, ps.key.c_str());
        }
        ps = w;
    }
}

void Tick() {
    coop::net::Session* s = ActiveSession();
    if (!s || !A::EnsureResolved() || !coop::atv_sync::IndexCurrent()) return;
    const uint64_t now = Now();
    if (s->role() == coop::net::Role::Host) {
        for (auto& kv : g_atvs) {
            AtvEntry& e = kv.second;
            if (!e.passengerDue || !R::IsLiveByIndex(e.actor, e.idx)) continue;
            e.passengerDue = false;
            SendTable(*s, -1, kv.first, e);
        }
    } else {
        TickClient(*s, now);
    }
    TickLocal(now);
    SyncPuppets();
}

void OnTable(const coop::net::AtvPassengerTablePayload& t) {
    const std::wstring key = StringFromWireKey(t.key);
    if (key.empty()) return;
    if (AtvEntry* e = LiveEntry(key)) {
        ApplyTable(*e, key, t);
        g_pending.erase(key);
        return;
    }
    if (g_pending.size() < kMaxPending || g_pending.count(key)) g_pending[key] = t;
}

void SendTablesToSlot(coop::net::Session& s, int peerSlot) {
    int sent = 0;
    for (auto& kv : g_atvs) {
        if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx) || !AnyRider(kv.second)) continue;
        if (SendTable(s, peerSlot, kv.first, kv.second)) ++sent;
    }
    if (sent > 0) UE_LOGI("atv_passenger: connect replay -- sent %d passenger table(s) to slot %d", sent, peerSlot);
}

void OnDisconnect() {
    if (g_local.seated && R::IsLive(g_local.player)) AP::ReleaseRider(g_local.player, true);
    for (PuppetSeat& ps : g_pup) ps = PuppetSeat{};  // the bodies' owner drops every slot at its own session end
    g_local = Local{};
    g_pending.clear();
    g_debounceUntilMs = 0;
    g_mismatchSinceMs = 0;
    g_seq = 0;
}

}  // namespace coop::atv_passenger
