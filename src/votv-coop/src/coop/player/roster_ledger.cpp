// coop/player/roster_ledger.cpp -- see coop/player/roster_ledger.h.

#include "coop/player/roster_ledger.h"

#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <vector>

namespace coop::roster_ledger {
namespace {

std::array<Row, kMaxSlots> g_rows{};

// Session-monotonic player-number counter. Starts ABOVE the host's role
// constant and the host never draws from it, so re-seeding row 0 cannot mint a
// second #1. Never reused within a session.
uint16_t g_nextPlayerNo = kHostPlayerNo + 1;

struct PerSlotClearReg { PerSlotClearFn fn; void* self; };

// Function-local statics: a file-scope PerSlotState<T> in ANY translation unit
// registers from its constructor before main, and these must already exist.
// (Static-init order across TUs is unspecified; construct-on-first-use is not.)
std::vector<PerSlotClearReg>& PerSlotRegistry() {
    static std::vector<PerSlotClearReg> v;
    return v;
}
std::vector<SlotReplacedFn>& Subscribers() {
    static std::vector<SlotReplacedFn> v;
    return v;
}

const Row& EmptyRow() {
    static const Row kEmpty;
    return kEmpty;
}

bool ValidSlot(int slot) { return slot >= 0 && slot < kMaxSlots; }

// The ONE place a row's occupant changes. Snapshots both sides FIRST, then
// mutates, then clears the registered per-slot state, then fans out to
// subscribers. Subscribers therefore see a consistent pair and can read the
// departed person's fields without racing the clear -- which is what lets the
// "<X> left the game" line and the teardown stop depending on each other's
// ordering (they used to: the toast had to print before the nick was cleared).
void Transition(int slot, const Row& incoming) {
    const Row outgoing = g_rows[slot];  // by value on purpose: this is the snapshot
    if (outgoing.playerNo == incoming.playerNo) return;  // no occupancy change

    g_rows[slot] = incoming;

    // Per-slot person-state resets run BEFORE the subscribers so a subscriber
    // that reads such state sees it already clean for the incoming person.
    for (const auto& reg : PerSlotRegistry()) reg.fn(reg.self, slot);

    for (SlotReplacedFn fn : Subscribers()) fn(slot, outgoing, incoming);

    if (outgoing.occupied() && incoming.occupied()) {
        UE_LOGI("ledger: slot %d REPLACED -- #%u '%ls' -> #%u", slot,
                static_cast<unsigned>(outgoing.playerNo), outgoing.nick.c_str(),
                static_cast<unsigned>(incoming.playerNo));
    } else if (incoming.occupied()) {
        UE_LOGI("ledger: slot %d occupied by #%u", slot,
                static_cast<unsigned>(incoming.playerNo));
    } else {
        UE_LOGI("ledger: slot %d emptied (was #%u '%ls')", slot,
                static_cast<unsigned>(outgoing.playerNo), outgoing.nick.c_str());
    }
}

}  // namespace

const Row& Get(int slot) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::Get)");
    if (!ValidSlot(slot)) return EmptyRow();
    return g_rows[slot];
}

const std::wstring& DisplayName(int slot) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::DisplayName)");
    // The single fallback. Six sites used to carry their own and they disagreed
    // -- one of them printed the LOCAL player's name for an unnamed REMOTE peer.
    static const std::wstring kUnknown = L"Remote player";
    if (!ValidSlot(slot)) return kUnknown;
    const Row& r = g_rows[slot];
    return r.nick.empty() ? kUnknown : r.nick;
}

bool Occupied(int slot) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::Occupied)");
    return ValidSlot(slot) && g_rows[slot].occupied();
}

int OccupiedCount() {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::OccupiedCount)");
    int n = 0;
    for (const Row& r : g_rows) if (r.occupied()) ++n;
    return n;
}

void InstallRow(int slot, uint16_t playerNo, uint32_t bornGeneration) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::InstallRow)");
    if (!ValidSlot(slot) || playerNo == 0) return;
    if (g_rows[slot].playerNo == playerNo) {
        // Same person re-asserted (the repair pulse does this constantly).
        // Refresh the generation -- it is the only field that can legitimately
        // change without the person changing -- but fire no transition.
        g_rows[slot].bornGeneration = bornGeneration;
        return;
    }
    Row incoming;
    incoming.playerNo = playerNo;
    incoming.bornGeneration = bornGeneration;
    Transition(slot, incoming);
}

void ClearRow(int slot) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::ClearRow)");
    if (!ValidSlot(slot)) return;
    Transition(slot, Row{});
}

void ClearAll() {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::ClearAll)");
    for (int slot = 0; slot < kMaxSlots; ++slot) Transition(slot, Row{});
    g_nextPlayerNo = kHostPlayerNo + 1;
}

void Reset() {
    // DELIBERATELY NOT UE_ASSERT_GAME_THREAD, and this is the only accessor
    // without it. Reset is the SESSION-BRINGUP entry point: it is called from
    // event_feed::OnSessionStart, which runs on the bringup thread at
    // harness/session_runtime.cpp:386 -- BEFORE g_session.Start() at :430 spawns
    // the net thread and before the pump can tick a running session. There is no
    // concurrent reader by construction, and Start()'s thread creation is the
    // happens-before edge for everything after. Same discipline the neighbouring
    // SetLocalNickname writer already documents. (The smoke's HotPathGuard caught
    // the assert firing here; the ACCESS is fine, the assert was mis-scoped.)
    //
    // UNCONDITIONAL per-slot clears, unlike ClearAll: they run for every slot
    // whether or not the ledger believed it occupied. The difference matters
    // because some per-slot state is written from the wire BEFORE its row exists
    // (a peer's display prefs land with its Join), so an occupancy-gated clear
    // would leave exactly that state behind for the next session to inherit.
    //
    // NO SlotReplacedFn FANOUT HERE, and that is structural rather than a
    // convention. A SlotReplaced subscriber exists to react to a PERSON changing
    // seats; at bringup nobody is leaving, and the subscriber bodies do engine
    // work (`chat_feed::Push`, `puppet_drive::DestroySlot`) that is not legal on
    // this thread. This loop used to fan out on the theory that every outgoing
    // row is already empty here and every subscriber early-returns on an
    // unoccupied outgoing row -- BOTH halves of which were wrong (2026-07-27
    // audit): `OnSlotReplaced_ArmPulse` carries no such guard and ran every time,
    // and the emptiness itself is a precondition nothing enforces, so a Stop path
    // that skipped ClearAll would have run a puppet destroy off the game thread.
    // Safety is now a property of the code rather than of a comment.
    for (int slot = 0; slot < kMaxSlots; ++slot) {
        g_rows[slot] = Row{};
        for (const auto& reg : PerSlotRegistry()) reg.fn(reg.self, slot);
    }
    g_nextPlayerNo = kHostPlayerNo + 1;
}

uint16_t MintPlayerNo() {
    UE_ASSERT_GAME_THREAD("g_nextPlayerNo (roster_ledger::MintPlayerNo)");
    // Skip 0 (the empty sentinel), the host's role constant, and any number a
    // LIVE row still holds. The live-row skip is what keeps "a changed playerNo
    // means a replacement" true for any session length: after the uint16 wraps,
    // a recycled number could otherwise collide with a peer still sitting in
    // another slot and that peer's row would read as unchanged.
    for (int attempt = 0; attempt < 0x10000; ++attempt) {
        const uint16_t candidate = g_nextPlayerNo++;
        if (candidate == 0 || candidate == kHostPlayerNo) continue;
        bool taken = false;
        for (const Row& r : g_rows) if (r.playerNo == candidate) { taken = true; break; }
        if (!taken) return candidate;
    }
    // Unreachable with kMaxSlots << 65534, but a silent 0 would read as "empty".
    UE_LOGE("ledger: player-number space exhausted -- reusing %u",
            static_cast<unsigned>(kHostPlayerNo + 1));
    return kHostPlayerNo + 1;
}

void EnsureRowZeroSeeded(const coop::net::Session& session, const std::wstring& localNick) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::EnsureRowZeroSeeded)");
    if (session.role() != coop::net::Role::Host) return;  // a client's row 0 is the HOST, wire-born
    if (g_rows[0].playerNo != kHostPlayerNo) {
        Row me;
        me.playerNo = kHostPlayerNo;
        me.bornGeneration = 0;  // host-self is seeded, never connection-derived
        me.nick = localNick;
        Transition(0, me);
        return;
    }
    // Already seeded: keep the name current (the host can rename mid-session)
    // without disturbing occupancy.
    g_rows[0].nick = localNick;
}

void SetNick(int slot, std::wstring nick) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::SetNick)");
    if (!ValidSlot(slot) || !g_rows[slot].occupied()) return;
    g_rows[slot].nick = std::move(nick);
}

void SetGuid(int slot, std::string guid) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::SetGuid)");
    if (!ValidSlot(slot) || !g_rows[slot].occupied()) return;
    g_rows[slot].guid = std::move(guid);
}

void SetSkin(int slot, std::string skin) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::SetSkin)");
    if (!ValidSlot(slot) || !g_rows[slot].occupied()) return;
    g_rows[slot].skin = std::move(skin);
}

void SetJoinAnnounced(int slot, bool announced) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::SetJoinAnnounced)");
    if (!ValidSlot(slot) || !g_rows[slot].occupied()) return;
    g_rows[slot].joinAnnounced = announced;
}

LinkFacts DisplayLink(int slot) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::DisplayLink)");
    // LocalPeerId is 0 on the host and 1..3 on a client (0xFF until assigned, which
    // is out of range and so falls through to the row's own facts).
    const int localSlot = static_cast<int>(coop::players::Registry::Get().LocalPeerId());
    if (slot == 0 && localSlot > 0 && localSlot < kMaxSlots) {
        const Row& own = Get(localSlot);
        if (own.occupied() && own.pingMs >= 0) return {own.linkKind, own.pingMs};
    }
    const Row& r = Get(slot);
    return {r.linkKind, r.pingMs};
}

void SetLinkFacts(int slot, coop::net::LinkKind kind, int16_t pingMs) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::SetLinkFacts)");
    if (!ValidSlot(slot) || !g_rows[slot].occupied()) return;
    g_rows[slot].linkKind = kind;
    g_rows[slot].pingMs = pingMs;
}

void RefreshLinkFacts(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::RefreshLinkFacts)");
    // HOST ONLY: a client's rows carry what the host published, and re-deriving
    // them locally is exactly the per-viewer answer this whole lane retired.
    if (session.role() != coop::net::Role::Host) return;

    // Row 0 is the host. Their traffic never crosses a socket, so the honest
    // answers are Local and "no RTT" -- NOT 0, which the scoreboard renders as
    // "<1ms", a measured-looking sub-millisecond latency nobody measured.
    if (g_rows[0].occupied()) SetLinkFacts(0, coop::net::LinkKind::Local, -1);

    for (int slot = 1; slot < kMaxSlots; ++slot) {
        if (!g_rows[slot].occupied()) continue;
        const int rtt = session.rttMsForSlot(slot);
        // Clamp into the wire's int16 before it can wrap. The sampler already
        // rejects >=60000 (session.cpp), so this only guards the type edge.
        const int16_t ping = (rtt < 0) ? int16_t{-1}
                                       : static_cast<int16_t>(rtt > 30000 ? 30000 : rtt);
        SetLinkFacts(slot, session.LinkKindForSlot(slot), ping);
    }
}

void SubscribeSlotReplaced(SlotReplacedFn fn) {
    // IDEMPOTENT by function pointer. Registration sites are per-session install
    // paths that legitimately run more than once in a process (Stop/Start), and
    // a duplicate subscriber would double-fire every teardown -- a bug whose
    // symptom (state cleared twice) is invisible until the day one of the
    // subscribers stops being idempotent itself.
    if (!fn) return;
    for (SlotReplacedFn existing : Subscribers()) if (existing == fn) return;
    Subscribers().push_back(fn);
}

void RegisterPerSlotClear(PerSlotClearFn fn, void* self) {
    if (!fn || !self) return;
    for (const auto& reg : PerSlotRegistry()) if (reg.self == self) return;  // idempotent
    PerSlotRegistry().push_back(PerSlotClearReg{fn, self});
}

void ReconcileFromSession(coop::net::Session& session) {
    UE_ASSERT_GAME_THREAD("g_rows (roster_ledger::ReconcileFromSession)");
    // HOST ONLY. A client's slots 1..3 carry permanently-zero generations (only
    // the host mints), so reconciling there would erase exactly the rows the
    // wire just delivered.
    if (session.role() != coop::net::Role::Host) return;
    if (!session.running()) return;

    for (int slot = 1; slot < kMaxSlots; ++slot) {  // slot 0 is seeded, not derived
        const uint32_t liveGen = session.peerGenerationForSlot(slot);

        // DEATH first, and unconditionally. A generation that no longer matches
        // the row's means the previous occupant is gone -- whether the slot is
        // now empty (gen 0) or already refilled by a successor still connecting
        // (gen changed). Deferring the death until the successor is ready would
        // leave the departed person's nick, skin and voice attributed to the
        // slot in the meantime.
        if (g_rows[slot].occupied() && g_rows[slot].bornGeneration != liveGen)
            ClearRow(slot);

        // BIRTH waits for READY, never accept: a doomed connect that never
        // reaches Connected+lane-config must create no row at all, otherwise the
        // birth pairs with a teardown and resurrects the false "left the game"
        // the 2026-07-16 fix removed.
        if (liveGen != 0 && !g_rows[slot].occupied() && session.IsSlotReady(slot))
            InstallRow(slot, MintPlayerNo(), liveGen);
    }
}

}  // namespace coop::roster_ledger
