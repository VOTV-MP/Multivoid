// coop/dev/hostile_drill_kdress_client.cpp -- the kdress_ask arm's sender: see coop/dev/hostile_drill_kdress.h. The client
// waits for the host's targets line, takes the glasses into its hand with the verb a player's hand key runs, then sends raw
// PropActRequest and PropActCommit messages for the families OmegaPutOn and OmegaTakeOff through the session's own send,
// bypassing the Omega's watches, one at a time and each after the host's reply to the one before (a reply that matches no
// open record is the lane's stale one, and carries what the host answered), and checks each reply against the table. The pats
// go in one tick.

#include "hostile_drill_kdress_detail.h"  // co-located private header (src tree, not include/)

#include "kdress_drill_detail.h"  // the Omega drill's reads (co-located private header of the kdress drill)

#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_act_request.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::hostile_drill::kdress::detail {
namespace {

namespace DC  = coop::dev::drill_chat;
namespace R   = ue_wrap::reflection;
namespace PAR = coop::prop_act_request;
namespace HB  = coop::hand_item;
namespace K   = coop::dev::kdress_drill::detail;
using Kind = coop::net::ReliableKind;
using Fam  = coop::net::PropActFamily;

enum class CStep : uint8_t { WaitTargets, WaitItem, WaitHand, WaitGo, Asking, Done };
CStep             g_step = CStep::WaitTargets;
bool              g_idleSaid = false, g_historyOn = false;
Clock::time_point g_since{};
const char*       g_waiting = nullptr;
int               g_eids[3] = {};    // the near Omega, the far Omega, the glasses
ue_wrap::CachedObjRef g_item;
int               g_phase = 0, g_ask = 0, g_win = 0;
uint32_t          g_staleBase = 0;

// The asks, in the table's order: the phase they belong to, the family, the Omega they name (0 near, 1 far), the row, and what
// the host must answer: granted, or the reason. `commit` is set on the ask whose grant the client commits as an honest replay
// would, so the next ask finds the row worn.
struct Ask {
    int phase;
    Fam family;
    int omega;
    const wchar_t* row;
    uint8_t granted, reason;
    bool commit;
    const char* name;
};
const Ask kAsks[] = {
    {0, Fam::OmegaPutOn, 0, L"nosuchrow", 0, 2, false, "a put-on of a row no table has"},
    {0, Fam::OmegaPutOn, 0, kBucket, 0, 2, false, "a put-on of a row the hand never held"},
    {0, Fam::OmegaPutOn, 1, kGlasses, 0, 4, false, "a put-on on the Omega out of reach"},
    {0, Fam::OmegaPutOn, 0, kGlasses, 1, 0, true, "the honest put-on"},
    {0, Fam::OmegaPutOn, 0, kGlasses, 0, 2, false, "the same hold put on twice"},
    {1, Fam::OmegaTakeOff, 0, kGlasses, 1, 0, false, "the first take-off"},
    {1, Fam::OmegaTakeOff, 0, kBucket, 1, 0, false, "the second take-off"},
    {1, Fam::OmegaTakeOff, 0, kMaid, 0, 5, false, "the take-off past the drop budget"},
};
constexpr int kAskCount = sizeof(kAsks) / sizeof(kAsks[0]);

void Inconclusive(const std::string& why) {
    Verdict("INCONCLUSIVE", why);
    g_step = CStep::Done;
}

bool Late(const char* what) {
    if (!g_waiting || std::strcmp(g_waiting, what) != 0) {
        g_waiting = what;
        g_since = Clock::now();
    }
    return Clock::now() - g_since > kBound;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool SendAsk(coop::net::Session* s, int index, uint16_t id) {
    const Ask& a = kAsks[index];
    coop::net::PropActRequestPayload p{};
    p.id = id;
    p.family = static_cast<uint8_t>(a.family);
    p.eid = static_cast<uint32_t>(g_eids[a.omega]);
    for (size_t i = 0; a.row[i] && i < sizeof(p.verb) - 1; ++i) p.verb[i] = static_cast<char>(a.row[i]);
    return s->SendReliable(Kind::PropActRequest, &p, sizeof(p));
}

bool SendCommit(coop::net::Session* s, uint16_t id, uint32_t eid) {
    coop::net::PropActCommitPayload p{};
    p.id = id;
    p.committed = 1;
    p.eid = eid;
    return s->SendReliable(Kind::PropActCommit, &p, sizeof(p));
}

// The ask `g_ask` of this phase, one reply at a time: the id is the ask's number; the reply is the lane's stale one.
void RunAsks(coop::net::Session* s) {
    if (g_ask >= kAskCount || kAsks[g_ask].phase != g_phase) {
        DC::Say(Line((std::string("p") + std::to_string(g_phase + 1) + "-done").c_str()));
        UE_LOGI("[HOSTILE-DRILL] kdress_ask sent the asks of phase %d", g_phase + 1);
        g_waiting = nullptr;
        g_step = CStep::WaitGo;
        ++g_phase;
        return;
    }
    const Ask& a = kAsks[g_ask];
    const uint16_t id = static_cast<uint16_t>(g_ask + 1);
    const PAR::dev::LastReply last = PAR::dev::LastStaleReply();
    if (g_win == 0) {   // not yet sent
        g_staleBase = last.count;
        if (!SendAsk(s, g_ask, id)) return Inconclusive(std::string("the ask '") + a.name + "' was not sent");
        g_win = 1;
        g_since = Clock::now();
        g_waiting = "the host's reply";
        return;
    }
    if (last.count == g_staleBase) {
        if (Late("the host's reply")) Inconclusive(std::string("the host never answered the ask '") + a.name + "'");
        return;
    }
    if (last.id != id || last.granted != a.granted || last.reason != a.reason) {
        Verdict("FAIL", std::string("the ask '") + a.name + "' was answered granted=" + std::to_string(last.granted) +
                            " reason=" + std::to_string(last.reason) + " (id " + std::to_string(last.id) + "), wanted granted=" +
                            std::to_string(a.granted) + " reason=" + std::to_string(a.reason));
        g_step = CStep::Done;
        return;
    }
    if (a.commit && !SendCommit(s, id, static_cast<uint32_t>(g_eids[a.omega]))) {
        Inconclusive("the commit of the honest put-on was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] kdress_ask the ask '%s' was answered granted=%u reason=%u", a.name,
            static_cast<unsigned>(last.granted), static_cast<unsigned>(last.reason));
    ++g_ask;
    g_win = 0;
}

// The pats, in one tick: a short payload, the Omega out of reach, then six of the near Omega.
void SendPats(coop::net::Session* s) {
    bool ok = true;
    const coop::net::KerfurDressPatPayload farPat{static_cast<uint32_t>(g_eids[1])};
    const coop::net::KerfurDressPatPayload nearPat{static_cast<uint32_t>(g_eids[0])};
    const uint8_t shortPayload[3] = {0, 0, 0};
    ok = ok && s->SendReliable(Kind::KerfurDressIntent, shortPayload, sizeof(shortPayload));
    ok = ok && s->SendReliable(Kind::KerfurDressIntent, &farPat, sizeof(farPat));
    for (int i = 0; i < 6; ++i) ok = ok && s->SendReliable(Kind::KerfurDressIntent, &nearPat, sizeof(nearPat));
    if (!ok) return Inconclusive("a forged pat was not sent");
    UE_LOGI("[HOSTILE-DRILL] kdress_ask sent 8 pats in one tick");
    DC::Say(Line("p3-done"));
    g_step = CStep::Done;
}

}  // namespace

void ClientTick(coop::net::Session* s) {
    if (g_step == CStep::Done) return;
    // The first read starts the chat history, so a line the host says before this client's join is over is still seen.
    if (!g_historyOn) {
        g_historyOn = true;
        DC::SeenHas(Line("targets"));
    }
    if (!OwnJoinOver()) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    void* const me = coop::players::Registry::Get().Local();
    if (!me || self == 0 || self >= coop::players::kMaxPeers) return;
    if (self != kSlot) {
        if (!g_idleSaid) {
            g_idleSaid = true;
            UE_LOGI("[HOSTILE-DRILL] kdress_ask idles: this client is slot %u, the family acts on slot %u",
                    static_cast<unsigned>(self), static_cast<unsigned>(kSlot));
        }
        g_step = CStep::Done;
        return;
    }
    switch (g_step) {
    case CStep::WaitTargets: {
        std::string rest;
        if (!DC::SeenRestOf(Line("targets "), &rest)) {
            if (Late("the host's targets line")) Inconclusive("the host never said its targets");
            return;
        }
        if (!K::ParseInts(rest, 3, g_eids)) return Inconclusive("the host's targets line could not be read");
        g_waiting = nullptr;
        g_step = CStep::WaitItem;
        return;
    }
    case CStep::WaitItem: {   // this client's copy may bind after the host's line arrives
        g_item.Set(K::LiveProp(g_eids[2]));
        void* const item = g_item.Get();
        if (!item) {
            if (Late("this client's copy of the glasses")) Inconclusive("this client's copy of the glasses never stood here");
            return;
        }
        // The player's own hand-taking verb, as the hand key runs it: the hold the host's hand evidence reads.
        bool collected = false;
        if (!ue_wrap::engine::CallMainPlayerHoldObject(me, item, collected)) return Inconclusive("Hold Object did not run");
        g_waiting = nullptr;
        g_win = 0;
        g_step = CStep::WaitHand;
        return;
    }
    case CStep::WaitHand: {
        if (PAR::dev::PendingId() != 0) {
            if (Late("the host's answer to the hand-taking")) Inconclusive("the host never answered the hand-taking");
            return;
        }
        void* const hand = HB::LocalHandActor();
        if (!(hand && HB::OwnAnnounced(R::ClassNameOf(hand).c_str()))) {
            if (++g_win >= K::kReadTicks) Inconclusive("the hand holds nothing the lane announced after the verb");
            return;
        }
        DC::Say(Line("held"));
        g_waiting = nullptr;
        g_phase = 0;
        g_step = CStep::WaitGo;
        return;
    }
    case CStep::WaitGo: {
        if (g_phase >= kPhases) {
            g_step = CStep::Done;
            return;
        }
        if (!DC::SeenHas(Line((std::string("go") + std::to_string(g_phase + 1)).c_str()))) {
            if (Late("the host's go line")) Inconclusive("the host never said go");
            return;
        }
        g_waiting = nullptr;
        if (g_phase == 2) return SendPats(s);
        g_win = 0;
        g_step = CStep::Asking;
        return;
    }
    case CStep::Asking:
        return RunAsks(s);
    case CStep::Done:
        return;
    }
}

void ClientReset() {
    g_step = CStep::WaitTargets;
    g_idleSaid = g_historyOn = false;
    g_since = {};
    g_waiting = nullptr;
    g_eids[0] = g_eids[1] = g_eids[2] = 0;
    g_item.Reset();
    g_phase = g_ask = g_win = 0;
    g_staleBase = 0;
}

}  // namespace coop::dev::hostile_drill::kdress::detail
