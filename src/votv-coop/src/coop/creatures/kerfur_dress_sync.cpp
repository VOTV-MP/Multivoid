// coop/creatures/kerfur_dress_sync.cpp -- see coop/creatures/kerfur_dress_sync.h.

#include "coop/creatures/kerfur_dress_sync.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_host.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::kerfur_dress_sync {
namespace {

namespace N  = coop::net;
namespace K  = ue_wrap::kerfur;
namespace KD = ue_wrap::kerfur_dress;
namespace EL = coop::element;
namespace sg = ue_wrap::script_gate;
using Payload = N::KerfurDressPayload;

// updateDrip is declared on kerfurOmega_C alone, so the one name watch matches every Omega skin. Every writer of `drip`
// ends in it (equipItem, unequipItem, a load), and its POST is where the outfit has changed.
UE_FACT_FN(kRowUpdateDrip, "kerfur_dress_sync", L"kerfurOmega_C", L"updateDrip", Declared);
constexpr int kTagUpdateDrip = 0x4B445201;   // 'KDR' 1

// A word for a copy not bound yet is held per Omega, the newest winning; more Omegas than this are not held.
constexpr size_t kMaxHeld = 32;

// A client's pats: four at once, then one every two seconds.
constexpr N::IntentBudget kPatBudget{4.0f, 0.5f};

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
bool g_taken = false;
bool g_settled = false;
std::unordered_map<uint32_t, Payload> g_held;   // CLIENT: the outfit word of each Omega whose copy is not bound
N::IntentBucket g_pats[N::kMaxPeers];
N::RefusalTally g_refusals{"kerfur_dress"};
void* g_patArmed = nullptr;   // the Omega whose pat the game's guard let play, from the PRE to the POST
uint32_t g_heldDropped = 0;
bool g_heldDropSaid = false;
dev::PatView g_patView;

// The outfit drill's red arms withhold one effect each; a red site says so in the log.
bool RedIs(dev::Red r) { return dev::RedOf() == r; }

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() ? s : nullptr;
}

N::RefusalTally::Clock::time_point Now() { return N::RefusalTally::Clock::now(); }

// An Omega's element on this machine is its NPC element, bound to its copy.
uint32_t EidOf(void* actor) { return static_cast<uint32_t>(EL::Registry::Get().EidForActor(actor)); }

void* ActorOf(uint32_t eid) {
    void* const actor = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Npc);
    return actor && K::IsKerfurActor(actor) ? actor : nullptr;
}

// The rows back to back from names[0], each followed by its NUL. A row or a count past the word's widths is refused,
// never cut: a receiver would otherwise dress the Omega in a name the game has no row for.
bool PackRows(Payload& p, uint32_t eid, const std::vector<std::wstring>& rows, uint8_t flags) {
    p = Payload{};
    p.elementId = eid;
    p.flags = flags;
    if (rows.size() > N::kKerfurDressMaxRows) return false;
    size_t at = 0;
    for (const std::wstring& r : rows) {
        if (r.empty() || r.size() > N::kKerfurDressMaxRowChars) return false;
        for (const wchar_t c : r) {
            if (c < 0x20 || c > 0x7e) return false;
            p.names[at++] = static_cast<char>(c);
        }
        p.names[at++] = '\0';
    }
    p.count = static_cast<uint8_t>(rows.size());
    return true;
}

// The Omegas whose refusal this session already said: a refusal repeats at every updateDrip, so it is said once per Omega.
std::unordered_set<uint32_t> g_unpackableSaid;
std::unordered_set<uint32_t> g_refusedSaid;
std::unordered_set<uint32_t> g_patAskRefusedSaid;   // CLIENT: the Omegas whose pat the session refused to send
uint32_t g_unpackable = 0;

// `omega`'s drip read now and packed with `flags`; false (said once per Omega, counted) when it cannot be read or packed.
bool PackOutfit(void* omega, uint32_t eid, uint8_t flags, Payload& p, size_t& rowCount) {
    std::vector<std::wstring> rows;
    if (KD::ReadDrip(omega, rows) && PackRows(p, eid, rows, flags)) {
        rowCount = rows.size();
        return true;
    }
    rowCount = rows.size();
    ++g_unpackable;
    if (g_unpackableSaid.insert(eid).second)
        UE_LOGW("kerfur_dress: Omega eid=%u's outfit (%zu rows) could not be read or does not fit the word (a row past %u "
                "characters, or more than %u rows) -- not sent (#%u)", eid, rows.size(),
                static_cast<unsigned>(N::kKerfurDressMaxRowChars), static_cast<unsigned>(N::kKerfurDressMaxRows),
                g_unpackable);
    return false;
}

// One send of a 272, to `slot` or, when negative, to every client but `skipSlot`. The session answers false when no peer
// took it (none connected and world-ready, a dying link); that is said once per Omega, and the caller logs `sent` only on true.
bool Deliver(N::Session& s, const Payload& p, int slot, int skipSlot, const char* what) {
    const bool sent = slot >= 0 ? s.SendReliableToSlot(slot, N::ReliableKind::KerfurDressState, &p, sizeof(p))
                                : s.SendReliable(N::ReliableKind::KerfurDressState, &p, sizeof(p), skipSlot);
    if (!sent && g_refusedSaid.insert(p.elementId).second)
        UE_LOGW("kerfur_dress: HOST's %s of Omega eid=%u was NOT sent -- the session took it for no peer (none world-ready "
                "to receive it, or the link is down)", what, p.elementId);
    return sent;
}

// The rows' own message of a Send: the omega's drip read now.
bool Send(N::Session& s, void* omega, uint32_t eid, int slot, const char* why) {
    Payload p;
    size_t rowCount = 0;
    if (!PackOutfit(omega, eid, 0, p, rowCount) || !Deliver(s, p, slot, -1, "outfit")) return false;
    char to[24] = "every client";
    if (slot >= 0) std::snprintf(to, sizeof(to), "slot %d", slot);
    UE_LOGI("kerfur_dress: HOST Omega eid=%u wears %zu item(s) -- sent to %s (%s)", eid, rowCount, to, why);
    return true;
}

// The POST of updateDrip on an Omega: on the host, the outfit is what it now wears.
void OnUpdateDripPost(const sg::Call& c) {
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Host || !s->connected()) return;
    const uint32_t eid = EidOf(c.object);
    if (eid == 0 || !K::IsKerfurActor(c.object)) return;
    Send(*s, c.object, eid, -1, "updateDrip");
}

void SettleWatch() {
    if (!g_watched || g_settled) return;
    sg::ResolvePendingNames();
    if (g_taken && !sg::NameWatchSettled(kRowUpdateDrip, kTagUpdateDrip)) return;
    g_settled = true;
    if (g_taken && sg::NameWatchLive(kRowUpdateDrip, kTagUpdateDrip))
        UE_LOGI("kerfur_dress: the watch on the Omega's updateDrip is live");
    else
        UE_LOGE("kerfur_dress: the watch on the Omega's updateDrip is dead -- the host's outfit reaches no client");
}

// What a client does with the host's word on a bound copy: its rows when they differ, an open equipment list rebuilt, and
// a pat shown (the montage alone; no copy meows from a received pat).
void Apply(void* omega, uint32_t eid, const std::vector<std::wstring>& want, bool pat) {
    std::vector<std::wstring> have;
    if (!KD::ReadDrip(omega, have) || have != want) {
        const bool ok = KD::ApplyDrip(omega, want);
        const bool listed = ok && KD::RefreshViewer(omega);
        UE_LOGI("kerfur_dress: CLIENT Omega eid=%u wears the host's %zu item(s) (ok=%d, equipment list rebuilt=%d)", eid,
                want.size(), ok ? 1 : 0, listed ? 1 : 0);
    }
    if (pat) {
        const bool played = KD::PlayPat(omega);
        UE_LOGI("kerfur_dress: CLIENT Omega eid=%u patted by another player (montage=%d)", eid, played ? 1 : 0);
    }
}

// True when the pat word went to a peer. The pat is a flag of the outfit word where MTA sends a one-shot animation as its
// own RPC (CStaticFunctionDefinitions.cpp:4539): no new kind was needed, and the cost is that a pat in flight can show the
// outfit of one round trip ago -- a pat arriving between a client's put-on replay and the host's commit-apply puts the old
// rows back on that client's copy until the host's apply word follows.
bool SendPat(N::Session& s, void* omega, uint32_t eid, int skipSlot) {
    if (RedIs(dev::Red::Pat)) {
        UE_LOGW("kerfur_dress: kdress_red=pat -- HOST's pat of Omega eid=%u is not sent", eid);
        return false;
    }
    Payload p;
    size_t rowCount = 0;
    return PackOutfit(omega, eid, N::kKerfurDressFlagPat, p, rowCount) && Deliver(s, p, -1, skipSlot, "pat");
}

}  // namespace

namespace internal {

bool WellFormed(const Payload& p) {
    if (p.count > N::kKerfurDressMaxRows || (p.flags & ~N::kKerfurDressFlagPat) != 0 || p.pad[0] != 0 || p.pad[1] != 0)
        return false;
    size_t at = 0;
    for (uint8_t i = 0; i < p.count; ++i) {
        const size_t len = strnlen(p.names + at, N::kKerfurDressNamesBytes - at);
        if (len == 0 || len > N::kKerfurDressMaxRowChars || at + len >= N::kKerfurDressNamesBytes) return false;
        for (size_t k = 0; k < len; ++k)
            if (p.names[at + k] < 0x20 || p.names[at + k] > 0x7e) return false;
        at += len + 1;
    }
    for (size_t k = at; k < N::kKerfurDressNamesBytes; ++k)
        if (p.names[k] != 0) return false;
    return true;
}

void Unpack(const Payload& p, std::vector<std::wstring>& out) {
    out.clear();
    size_t at = 0;
    for (uint8_t i = 0; i < p.count && i < N::kKerfurDressMaxRows && at < N::kKerfurDressNamesBytes; ++i) {
        const size_t len = strnlen(p.names + at, N::kKerfurDressNamesBytes - at);
        out.emplace_back(p.names + at, p.names + at + len);
        at += len + 1;
    }
}

}  // namespace internal

void Install(N::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched) return;
    g_watched = true;
    g_taken = sg::WatchName(kRowUpdateDrip, kTagUpdateDrip, nullptr, &OnUpdateDripPost);
    if (!g_taken) UE_LOGE("kerfur_dress: the watch on the Omega's updateDrip was refused -- the host's outfit reaches no client");
}

void Tick() {
    SettleWatch();
    g_refusals.Flush(Now());
}

bool SendOutfit(void* omega, int slot) {
    N::Session* const s = Live();
    const uint32_t eid = EidOf(omega);
    return s && s->role() == N::Role::Host && eid != 0 && Send(*s, omega, eid, slot, "asked");
}

void SendJoin(int slot) {
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Host) return;
    if (RedIs(dev::Red::Snapshot)) {
        UE_LOGW("kerfur_dress: kdress_red=snapshot -- HOST sends the joiner in slot %d no Omega's outfit", slot);
        return;
    }
    for (void* k : K::AllKerfurs()) {
        const uint32_t eid = EidOf(k);
        if (eid != 0) Send(*s, k, eid, slot, "join");
    }
}

void OnPatPre(void* kerfur) {
    N::Session* const s = Live();
    g_patArmed = s && s->connected() && KD::CanPat(kerfur) ? kerfur : nullptr;
}

void OnPatPost(void* kerfur) {
    if (!kerfur || kerfur != g_patArmed) return;
    g_patArmed = nullptr;
    N::Session* const s = Live();
    const uint32_t eid = EidOf(kerfur);
    if (!s || !s->connected() || eid == 0) return;
    if (s->role() == N::Role::Client) {
        const N::KerfurDressPatPayload p{eid};
        if (s->SendReliable(N::ReliableKind::KerfurDressIntent, &p, sizeof(p), -1)) {
            UE_LOGI("kerfur_dress: this player's pat on Omega eid=%u -- asked of the host", eid);
        } else if (g_patAskRefusedSaid.insert(eid).second) {
            UE_LOGW("kerfur_dress: this player's pat on Omega eid=%u was NOT sent -- the session took it for the host's link "
                    "(down or not ready), so no other player sees it", eid);
        }
        return;
    }
    if (SendPat(*s, kerfur, eid, -1)) UE_LOGI("kerfur_dress: HOST pat on Omega eid=%u -- sent to every client", eid);
}

void OnState(const Payload& p) {
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Client) return;   // the dispatcher took WellFormed
    std::vector<std::wstring> want;
    internal::Unpack(p, want);
    void* const omega = ActorOf(p.elementId);
    if (omega) {
        Apply(omega, p.elementId, want, (p.flags & N::kKerfurDressFlagPat) != 0);
        return;
    }
    // The copy is not bound yet: the host's word waits for it, the newest of each Omega, and a pat is not replayed late.
    // MTA carries an element's data inside its add packet (CEntityAddPacket.cpp:176-188), so no hold exists there; here
    // EntitySpawn is a shared kind and a copy binds late (a class-load wait, the adoption of the save's twin), so the word
    // can arrive first and waits at the bind seam.
    if (g_held.size() >= kMaxHeld && g_held.find(p.elementId) == g_held.end()) {
        ++g_heldDropped;
        if (!g_heldDropSaid) {
            g_heldDropSaid = true;
            UE_LOGW("kerfur_dress: CLIENT holds %zu Omegas' words already -- the word for eid=%u is dropped and counted "
                    "(said once per session)", g_held.size(), p.elementId);
        }
        return;
    }
    Payload held = p;
    held.flags = 0;
    g_held[p.elementId] = held;
    UE_LOGI("kerfur_dress: CLIENT Omega eid=%u has no bound copy -- the host's word waits for it", p.elementId);
}

void OnMirrorBound(uint32_t eid) {
    const auto it = g_held.find(eid);
    if (it == g_held.end()) return;
    void* const omega = ActorOf(eid);
    if (!omega) {
        UE_LOGW("kerfur_dress: CLIENT Omega eid=%u has no live copy at its bind -- the host's word stays held", eid);
        return;
    }
    const Payload held = it->second;
    g_held.erase(it);
    std::vector<std::wstring> want;
    internal::Unpack(held, want);
    Apply(omega, eid, want, false);
}

void OnMirrorEnded(uint32_t eid) {
    if (!g_held.empty()) g_held.erase(eid);
}

void OnPatAsk(int senderSlot, const N::KerfurDressPatPayload& p) {
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Host || senderSlot < 1 || senderSlot >= N::kMaxPeers) return;
    const uint8_t slot = static_cast<uint8_t>(senderSlot);
    const char* why = nullptr;
    void* omega = nullptr;
    uint32_t* term = nullptr;   // the counter of the term that refused
    const uint64_t nowMs = N::NowMs();
    if (g_patView.firstMs == 0) g_patView.firstMs = nowMs;
    g_patView.lastMs = nowMs;
    if (!s->IsSlotWorldReady(slot)) { why = "sender not world-ready"; term = &g_patView.notReady; }
    else if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) { why = "sender not alive"; term = &g_patView.dead; }
    else if (!g_pats[slot].Take(kPatBudget, nowMs)) { why = "over the pat budget"; term = &g_patView.budget; }
    else if (!(omega = ActorOf(p.elementId))) { why = "no Omega by that id"; term = &g_patView.noOmega; }
    else if (const auto subject = EL::IntentTarget::ForClientIntent(*s, slot, coop::prop_act_host::kActReachUU).Authorize(omega);
             !subject) { why = EL::OutcomeName(subject.outcome); term = &g_patView.unreachable; }
    else if (!KD::CanPat(omega)) { why = "the Omega's state plays no pat"; term = &g_patView.noPat; }
    if (why) {
        ++*term;
        g_refusals.Note(slot, why, Now());
        return;
    }
    ++g_patView.accepted;
    // The pat is shown on the host's copy and sent on to every client but the patter, whose copy ran it. The host's own
    // copy neither stops nor meows: the pat is a look.
    const bool played = KD::PlayPat(omega);
    UE_LOGI("kerfur_dress: slot %u patted Omega eid=%u -- shown here (montage=%d)", slot, p.elementId, played ? 1 : 0);
    if (SendPat(*s, omega, p.elementId, slot))
        UE_LOGI("kerfur_dress: slot %u's pat on Omega eid=%u -- sent on to the other clients", slot, p.elementId);
}

void NoteIntentDropped() { ++g_patView.dropped; }

namespace dev {

Red RedOf() {
    static const Red red = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::kdress_red);
        if (v == "apply") return Red::Apply;
        if (v == "ask") return Red::Ask;
        if (v == "pat") return Red::Pat;
        if (v == "snapshot") return Red::Snapshot;
        return Red::Off;
    }();
    return red;
}

PatView Pats() { return g_patView; }
void ResetPatSpan() { g_patView.firstMs = g_patView.lastMs = 0; }

}  // namespace dev

void OnDisconnect() {
    if (g_heldDropped != 0)
        UE_LOGW("kerfur_dress: this session dropped %u Omega word(s) past the hold of %zu", g_heldDropped, kMaxHeld);
    g_held.clear();
    for (N::IntentBucket& b : g_pats) b.Reset();
    g_refusals.Clear();
    g_patArmed = nullptr;
    g_heldDropped = 0;
    g_heldDropSaid = false;
    g_patAskRefusedSaid.clear();
    g_unpackableSaid.clear();
    g_refusedSaid.clear();
    g_unpackable = 0;
}

}  // namespace coop::kerfur_dress_sync
