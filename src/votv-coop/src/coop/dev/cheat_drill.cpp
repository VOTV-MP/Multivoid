// coop/dev/cheat_drill.cpp -- see coop/dev/cheat_drill.h.

#include "coop/dev/cheat_drill.h"

#include "coop/cheats/cheat_request.h"
#include "coop/cheats/item_catalog.h"
#include "coop/commands/command_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "cheat_drill_shared.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/event_fire_sync.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/economy.h"
#include "ui/dev_menu.h"
#include "ui/imgui_overlay.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::cheat_drill {

namespace DC = coop::dev::drill_chat;
namespace CH = coop::cheats;
namespace R  = ue_wrap::reflection;
using coop::net::CheatReason;
using coop::net::CheatOp;

namespace detail {

State g;
Result g_results[16];
int g_nResults = 0;

uint64_t Now() { return ::GetTickCount64(); }
void Next() { ++g.step; g.stepAt = Now(); g.sent = false; }
bool Timed(uint64_t bound) { return Now() - g.stepAt > bound; }
bool Seen(const char* line) { return DC::SeenHas(line); }

int Crowbars() {
    int n = 0;
    for (void* o : R::FindObjectsByClass(L"prop_crowbar_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) ++n;
    return n;
}

bool Points(int32_t* out) { return ue_wrap::economy::ReadPoints(out); }

void CloseMenu() {
    if (!g.menuOpen) return;
    ui::imgui_overlay::SetVisible(false);
    g.menuOpen = false;
}

// The run's ending, said the moment it is known: a FAIL is decided by an event (a wrong answer, a wrong count), and the
// peer is told at once so it runs no later arm; a time bound only says INVALID (the run proved nothing), also said.
void End(const char* role, const char* word, const std::string& why) {
    if (std::strcmp(word, "PASS") == 0) UE_LOGI("%s %s PASS -- %s", kTag, role, why.c_str());
    else if (std::strcmp(word, "FAIL") == 0) UE_LOGE("%s %s FAIL -- %s", kTag, role, why.c_str());
    else UE_LOGW("%s %s INVALID -- %s", kTag, role, why.c_str());
    if (std::strcmp(word, "PASS") != 0) DC::Say(std::string("cheatdrill ") + word + " " + role);
    CloseMenu();
    CH::SetResultObserver(nullptr);
    g.done = true;
}
void Verdict(const char* role, bool pass, const std::string& why) { End(role, pass ? "PASS" : "FAIL", why); }
void Invalid(const char* role, const std::string& why) { End(role, "INVALID", why); }
void ClientFail(const char* arm, const std::string& why) { Verdict("client", false, std::string(arm) + ": " + why); }
void ClientInvalid(const char* arm, const std::string& why) { Invalid("client", std::string(arm) + ": " + why); }

}  // namespace detail

namespace {

using namespace detail;

constexpr uint64_t kPaneBoundMs = 90000;    // a pane's first frame; the ask is repeated every kPaneAskMs (a frame can be late to a window the OS has not drawn)
constexpr uint64_t kPaneAskMs = 4000;
constexpr int kSpawnCount = 2;
constexpr int kHostPoints = 777;
constexpr int kClientPoints = 321;
constexpr const char* kHostEvent = "arirGraff_3";
constexpr const char* kClientEvent = "arirGraff_5";
constexpr const char* kStoryEvent = "obelisk";

bool Enabled() {
    static const bool on = coop::config::ResolveEnum(::coop::config_registry::rows::cheat_drill) == "on";
    return on;
}

// The game's list is filled and read whole: a world's prop list runs to hundreds.
bool ItemsListed() {
    const auto names = CH::item_catalog::Names();
    return names && names->size() > 100;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

struct Pane { const char* cat; const char* sub; };

void OnResult(const coop::net::CheatResultPayload& p) {
    // The status line as it stood when this answer arrived: a late answer to an earlier request must leave it alone.
    const CH::Status st = CH::LastStatus();
    if (g_nResults < 16)
        g_results[g_nResults++] = {p.op, p.reason, p.done, p.id, st.pending, static_cast<uint8_t>(st.reason)};
}

// Open each pane in `list` in turn and wait until the render thread drew it. 1 when all drew, 0 while waiting.
// A pane that never draws within the bound ends the drill.
int OpenPanes(const char* role, const Pane* list, size_t n) {
    if (g.paneIdx >= n) {
        g.paneIdx = 0;
        g.paneAsked = false;
        CloseMenu();
        return 1;
    }
    const Pane& p = list[g.paneIdx];
    if (!g.paneAsked) {
        if (!g.menuOpen) { ui::imgui_overlay::SetVisible(true); g.menuOpen = true; }
        ui::dev_menu::RequestSelect(p.cat, p.sub);
        g.paneAsked = true;
        g.stepAt = Now();
        g.askedAt = g.stepAt;
        return 0;
    }
    if (ui::dev_menu::DrawnSelection(p.cat, p.sub)) {
        UE_LOGI("%s %s drew %s > %s", kTag, role, p.cat, p.sub);
        ++g.paneIdx;
        g.paneAsked = false;
        g.stepAt = Now();
        return 0;
    }
    if (Timed(kPaneBoundMs)) { Invalid(role, std::string("the pane ") + p.cat + " > " + p.sub + " never drew"); return 0; }
    if (Now() - g.askedAt > kPaneAskMs) {
        ui::imgui_overlay::SetVisible(true);
        ui::dev_menu::RequestSelect(p.cat, p.sub);
        g.askedAt = Now();
    }
    return 0;
}

// A pane that must NOT be offered: asked for, and not drawn within two seconds. 1 when it held, 0 while waiting.
int PaneStaysShut(const char* role, const Pane& p) {
    if (!g.paneAsked) {
        if (!g.menuOpen) { ui::imgui_overlay::SetVisible(true); g.menuOpen = true; }
        ui::dev_menu::RequestSelect(p.cat, p.sub);
        g.paneAsked = true;
        g.stepAt = Now();
        return 0;
    }
    if (ui::dev_menu::DrawnSelection(p.cat, p.sub)) {
        Verdict(role, false, std::string("the pane ") + p.cat + " > " + p.sub + " is offered and must not be");
        return 0;
    }
    if (!Timed(2000)) return 0;
    UE_LOGI("%s %s was not offered %s > %s", kTag, role, p.cat, p.sub);
    g.paneAsked = false;
    return 1;
}

// ---- the host ---------------------------------------------------------------------------------------------------

enum class H : int {
    WaitReady, Panes, Hello, WaitHello,
    ASay, AWaitArmed, AWaitSpawn, AWaitPass,
    BSay, BWaitArmed, BWaitPoints, BWaitPass,
    CSay, CWaitArmed, CWaitFire, CWaitPass,
    DSay, DWaitPass, DJudge,
    EOn, EWaitOn, ESay, EWaitPass, EJudge,
    Arms,
    FOff, FWaitOff, FSay, FWaitPass, FJudge,
    Finish
};

constexpr Pane kHostPanes[] = {{"Host tools", "Items"},  {"Host tools", "Economy"},  {"Host tools", "Events"},
                               {"Host tools", "Entities"}, {"Host tools", "World"},  {"Host tools", "Myself"},
                               {"Host tools", "Player cheats"}};

bool ClientFailed() { return DC::SeenHas("cheatdrill FAIL client"); }

void HostTick(coop::net::Session* s) {
    namespace EFS = coop::event_fire_sync;
    if (ClientFailed()) { Verdict("host", false, "the client reported a failed arm (its own line has why)"); return; }
    if (Seen("cheatdrill INVALID client")) { Invalid("host", "the client ended the run INVALID (its own line has why)"); return; }
    switch (static_cast<H>(g.step)) {
    case H::WaitReady: {
        CH::item_catalog::RequestRefresh();
        int32_t p = 0;
        if (!s->IsSlotWorldReady(1) || !ItemsListed() || !Points(&p)) {
            if (g.stepAt == 0) g.stepAt = Now();
            if (Timed(kSyncBoundMs)) Invalid("host", "no client world, item list or balance within the bound");
            return;
        }
        UE_LOGI("%s host ready: %zu items listed, setting is %s", kTag, CH::item_catalog::Names()->size(),
                CH::PlayersMayCheat() ? "ON (the drill needs it off)" : "off");
        if (CH::PlayersMayCheat()) { Invalid("host", "net.allow_player_cheats starts on; the drill starts from its default"); return; }
        g.paneIdx = 0;
        g.paneAsked = false;
        Next();
        return;
    }
    case H::Panes:
        if (OpenPanes("host", kHostPanes, sizeof(kHostPanes) / sizeof(kHostPanes[0]))) Next();
        return;
    case H::Hello: DC::Say("cheatdrill host-ready"); Next(); return;
    case H::WaitHello:
        if (Seen("cheatdrill client-ready")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "the client never said it was ready");
        return;

    // A: the host's Items call, a crowbar pair that must reach the client.
    case H::ASay: g.crowbars = Crowbars(); DC::Say("cheatdrill A go"); Next(); return;
    case H::AWaitArmed:
        if (!Seen("cheatdrill A armed")) { if (Timed(kSyncBoundMs)) Invalid("host", "A: the client never armed"); return; }
        Next();
        return;
    case H::AWaitSpawn:
        if (!g.sent) { CH::SpawnItem("crowbar", kSpawnCount, coop::net::kCheatInFront); g.sent = true; g.stepAt = Now(); return; }
        if (Crowbars() == g.crowbars + kSpawnCount) {
            UE_LOGI("%s host A: %d crowbars spawned (%d -> %d)", kTag, kSpawnCount, g.crowbars, g.crowbars + kSpawnCount);
            DC::Say("cheatdrill A spawned");
            Next();
        } else if (Timed(kEffectBoundMs)) {
            Invalid("host", "A: the host's own crowbars did not appear: " + std::to_string(Crowbars()) + " vs " +
                                       std::to_string(g.crowbars) + " + " + std::to_string(kSpawnCount));
        }
        return;
    case H::AWaitPass:
        if (Seen("cheatdrill A pass")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "A: the client never said the crowbars reached it");
        return;

    // B: the host's Economy call.
    case H::BSay: Points(&g.points); DC::Say("cheatdrill B go"); Next(); return;
    case H::BWaitArmed:
        if (Seen("cheatdrill B armed")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "B: the client never armed");
        return;
    case H::BWaitPoints: {
        if (!g.sent) { CH::AddPoints(kHostPoints); g.sent = true; g.stepAt = Now(); return; }
        int32_t p = 0;
        if (Points(&p) && p == g.points + kHostPoints) {
            UE_LOGI("%s host B: balance %d -> %d", kTag, g.points, p);
            DC::Say("cheatdrill B done " + std::to_string(p));
            Next();
        } else if (Timed(kEffectBoundMs)) {
            Invalid("host", "B: the host's balance did not move by " + std::to_string(kHostPoints));
        }
        return;
    }
    case H::BWaitPass:
        if (Seen("cheatdrill B pass")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "B: the client's balance never matched the host's");
        return;

    // C: the host's Events call.
    case H::CSay: g.fires = EFS::HostFireCount(); DC::Say("cheatdrill C go"); Next(); return;
    case H::CWaitArmed:
        if (Seen("cheatdrill C armed")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "C: the client never armed");
        return;
    case H::CWaitFire:
        if (!g.sent) { CH::FireEvent(kHostEvent); g.sent = true; g.stepAt = Now(); return; }
        if (EFS::HostFireCount() >= g.fires + 1) {
            UE_LOGI("%s host C: the host's watch committed the fire of %s", kTag, kHostEvent);
            DC::Say("cheatdrill C fired");
            Next();
        } else if (Timed(kEffectBoundMs)) {
            Invalid("host", std::string("C: the host's watch committed no fire of ") + kHostEvent);
        }
        return;
    case H::CWaitPass:
        if (Seen("cheatdrill C pass")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "C: the client never replayed the event");
        return;

    // D: with the setting off, the client's three requests change nothing and are answered Off.
    case H::DSay:
        Points(&g.points);
        g.crowbars = Crowbars();
        g.fires = EFS::HostFireCount();
        g.answered[0] = CH::AnsweredFor(1, CheatReason::Off);
        DC::Say("cheatdrill D go");
        Next();
        return;
    case H::DWaitPass:
        if (Seen("cheatdrill D pass")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "D: the client never reported its refusals");
        return;
    case H::DJudge: {
        int32_t p = 0;
        Points(&p);
        const uint32_t off = CH::AnsweredFor(1, CheatReason::Off) - g.answered[0];
        if (off != 3) { if (Timed(10000)) Invalid("host", "D: the host answered " + std::to_string(off) + " requests Off, not 3"); return; }
        if (p != g.points || Crowbars() != g.crowbars) {
            Verdict("host", false, "D: a refused request changed the host's world");
            return;
        }
        UE_LOGI("%s host D: three requests answered Off, balance and crowbars unchanged", kTag);
        Next();
        return;
    }

    // E: the host turns the setting on; the client's requests are run.
    case H::EOn:
        coop::command_sync::Submit("set net.allow_player_cheats 1");
        Next();
        return;
    case H::EWaitOn:
        if (CH::PlayersMayCheat()) { UE_LOGI("%s host E: net.allow_player_cheats is on", kTag); Next(); }
        else if (Timed(15000)) Invalid("host", "E: the `set` line did not turn the setting on");
        return;
    case H::ESay:
        Points(&g.points);
        g.crowbars = Crowbars();
        g.fires = EFS::HostFireCount();
        g.answered[0] = CH::AnsweredFor(1, CheatReason::Ok);
        g.answered[1] = CH::AnsweredFor(1, CheatReason::Refused);
        g.answered[2] = CH::AnsweredFor(1, CheatReason::UnknownItem);
        DC::Say("cheatdrill E on");
        Next();
        return;
    case H::EWaitPass:
        if (Seen("cheatdrill E sent")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "E: the client never finished its requests");
        return;
    case H::EJudge: {
        int32_t p = 0;
        Points(&p);
        const bool moved = p == g.points + kClientPoints && Crowbars() == g.crowbars + kSpawnCount + 1 &&
                           EFS::HostFireCount() >= g.fires + 1;
        const uint32_t ok = CH::AnsweredFor(1, CheatReason::Ok) - g.answered[0];
        const uint32_t refused = CH::AnsweredFor(1, CheatReason::Refused) - g.answered[1];
        const uint32_t unknown = CH::AnsweredFor(1, CheatReason::UnknownItem) - g.answered[2];
        if (!moved) {
            if (Timed(kEffectBoundMs)) {
                char buf[200];
                std::snprintf(buf, sizeof(buf), "E: the host's world did not move by the Oks: balance %d (want %d), crowbars %d (want %d), fires %u (want %u)",
                              p, g.points + kClientPoints, Crowbars(), g.crowbars + kSpawnCount + 1, EFS::HostFireCount(), g.fires + 1);
                Invalid("host", buf);
            }
            return;
        }
        if (ok != 4 || refused != 1 || unknown != 1) {
            Verdict("host", false, "E: the host answered Ok " + std::to_string(ok) + ", Refused " + std::to_string(refused) +
                                       ", UnknownItem " + std::to_string(unknown) + " (want 4, 1, 1)");
            return;
        }
        UE_LOGI("%s host E: the client's points, crowbars and event ran on the host (balance %d), the story event and "
                "the unknown prop were refused", kTag, p);
        DC::Say("cheatdrill E host-ok " + std::to_string(p));
        Next();
        return;
    }

    // G..L: the hostile arms, with the setting on and the client's right held (K withholds it, then gives it back).
    case H::Arms:
        if (HostArms(*s)) Next();
        return;

    // F: the host resets the setting; the client's next request is answered Off again.
    case H::FOff:
        g.answered[0] = CH::AnsweredFor(1, CheatReason::Off);
        coop::command_sync::Submit("reset net.allow_player_cheats");
        Next();
        return;
    case H::FWaitOff:
        if (!CH::PlayersMayCheat()) { UE_LOGI("%s host F: net.allow_player_cheats is back off", kTag); Next(); }
        else if (Timed(15000)) Invalid("host", "F: the `reset` line did not turn the setting off");
        return;
    case H::FSay: DC::Say("cheatdrill F off"); Next(); return;
    case H::FWaitPass:
        if (Seen("cheatdrill F pass")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("host", "F: the client never reported its refusal");
        return;
    case H::FJudge:
        if (CH::AnsweredFor(1, CheatReason::Off) - g.answered[0] != 1) {
            if (Timed(10000)) Invalid("host", "F: the host did not answer the request Off");
            return;
        }
        Next();
        return;
    case H::Finish:
        Verdict("host", true, "A host item spawn, points and event reached the client; the client's requests were refused with "
                              "the setting off; with it on (E) four were run (Ok x4) and the story event and an unknown prop were "
                              "refused; arms G..L held (raw requests, the bucket, the removal clamp, two requests in flight, a "
                              "withheld right, Unstuck); refused again once reset");
        DC::Say("cheatdrill done");
        return;
    }
}

// ---- the client -------------------------------------------------------------------------------------------------

enum class C : int {
    WaitReady, Panes, ShutItems, ShutHost, WaitHost, Hello,
    AWait, AArm, AWaitSpawn,
    BWait, BArm, BWaitPoints,
    CWait, CArm, CWaitReplay,
    DWait, DReqPoints, DReqSpawn, DReqEvent, DPass,
    EWait, EWaitOn, EPanes, EReqPoints, EReqSpawn, EReqAt, EReqEvent, EReqStory, EReqBad, ESent, EWaitMirror,
    Arms,
    FWait, FWaitOff, FReq, FPass,
    Finish
};

constexpr Pane kClientPanesOff[] = {{"Player tools", "Myself"}};
constexpr Pane kClientPanesOn[] = {{"Player tools", "Items"}, {"Player tools", "Economy"}, {"Player tools", "Events"}};

void ClientTick(coop::net::Session& session) {
    namespace EFS = coop::event_fire_sync;
    if (Seen("cheatdrill FAIL host")) { Verdict("client", false, "the host reported a failed arm (its own line has why)"); return; }
    if (Seen("cheatdrill INVALID host")) { Invalid("client", "the host ended the run INVALID (its own line has why)"); return; }
    Result r{};
    std::string rest;
    switch (static_cast<C>(g.step)) {
    case C::WaitReady: {
        CH::item_catalog::RequestRefresh();
        int32_t p = 0;
        if (g.stepAt == 0) g.stepAt = Now();
        if (!OwnJoinOver() || !ItemsListed() || !Points(&p)) {
            if (Timed(kSyncBoundMs)) Invalid("client", "no world, item list or balance within the bound");
            return;
        }
        UE_LOGI("%s client ready: %zu items listed, the host's setting is %s", kTag, CH::item_catalog::Names()->size(),
                CH::PlayersMayCheat() ? "ON (the drill needs it off)" : "off");
        if (CH::PlayersMayCheat()) { Invalid("client", "the host's setting starts on"); return; }
        CH::SetResultObserver(&OnResult);
        Next();
        return;
    }
    case C::Panes:
        if (OpenPanes("client", kClientPanesOff, 1)) Next();
        return;
    case C::ShutItems:
        if (PaneStaysShut("client", {"Player tools", "Items"})) Next();
        return;
    case C::ShutHost:
        if (PaneStaysShut("client", {"Host tools", "Items"})) { CloseMenu(); Next(); }
        return;
    case C::WaitHost:
        if (Seen("cheatdrill host-ready")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("client", "the host never said it was ready");
        return;
    case C::Hello: DC::Say("cheatdrill client-ready"); Next(); return;

    case C::AWait:
        if (Seen("cheatdrill A go")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("client", "A: the host never armed");
        return;
    case C::AArm: g.crowbars = Crowbars(); DC::Say("cheatdrill A armed"); Next(); return;
    case C::AWaitSpawn:
        if (!Seen("cheatdrill A spawned")) { if (Timed(kSyncBoundMs)) ClientInvalid("A", "the host never spawned"); return; }
        if (Crowbars() == g.crowbars + kSpawnCount) {
            UE_LOGI("%s client A PASS -- the host's %d crowbars appeared here (%d -> %d)", kTag, kSpawnCount, g.crowbars, Crowbars());
            DC::Say("cheatdrill A pass");
            Next();
        } else if (Timed(kEffectBoundMs + kSyncBoundMs / 6)) {
            ClientInvalid("A", "the host's crowbars did not appear here: " + std::to_string(Crowbars()) + " vs " + std::to_string(g.crowbars));
        }
        return;

    case C::BWait:
        if (Seen("cheatdrill B go")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("client", "B: the host never armed");
        return;
    case C::BArm: Points(&g.points); DC::Say("cheatdrill B armed"); Next(); return;
    case C::BWaitPoints: {
        if (!DC::SeenRestOf("cheatdrill B done ", &rest)) { if (Timed(kSyncBoundMs)) ClientInvalid("B", "the host never said its balance"); return; }
        int32_t p = 0;
        if (Points(&p) && p == std::atoi(rest.c_str())) {
            UE_LOGI("%s client B PASS -- the balance here is the host's %d (it was %d)", kTag, p, g.points);
            DC::Say("cheatdrill B pass");
            Next();
        } else if (Timed(kEffectBoundMs * 2)) {
            ClientInvalid("B", "the balance here is not the host's " + rest);
        }
        return;
    }

    case C::CWait:
        if (Seen("cheatdrill C go")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("client", "C: the host never armed");
        return;
    case C::CArm: g.replays = EFS::ReplayCount(); DC::Say("cheatdrill C armed"); Next(); return;
    case C::CWaitReplay:
        if (!Seen("cheatdrill C fired")) { if (Timed(kSyncBoundMs)) ClientInvalid("C", "the host never fired"); return; }
        if (EFS::ReplayCount() >= g.replays + 1) {
            UE_LOGI("%s client C PASS -- the host's event %s was replayed here", kTag, kHostEvent);
            DC::Say("cheatdrill C pass");
            Next();
        } else if (Timed(kEffectBoundMs + kSyncBoundMs / 6)) {
            ClientInvalid("C", "the host's event was not replayed here (replays " + std::to_string(EFS::ReplayCount()) + ", was " + std::to_string(g.replays) + ")");
        }
        return;

    // D: three requests, each answered Off.
    case C::DWait:
        if (Seen("cheatdrill D go")) { g_nResults = 0; Next(); }
        else if (Timed(kSyncBoundMs)) Invalid("client", "D: the host never armed");
        return;
    case C::DReqPoints:
        if (!Ask([] { CH::AddPoints(50); }, &r, "D")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Off)) { ClientFail("D", "the points request was answered " + std::to_string(r.reason) + ", not Off"); return; }
        Next();
        return;
    case C::DReqSpawn:
        if (!Ask([] { CH::SpawnItem("crowbar", 1, coop::net::kCheatInFront); }, &r, "D")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Off)) { ClientFail("D", "the spawn request was answered " + std::to_string(r.reason) + ", not Off"); return; }
        Next();
        return;
    case C::DReqEvent:
        if (!Ask([] { CH::FireEvent("arirGraff_1"); }, &r, "D")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Off)) { ClientFail("D", "the event request was answered " + std::to_string(r.reason) + ", not Off"); return; }
        Next();
        return;
    case C::DPass:
        UE_LOGI("%s client D PASS -- the host refused all three requests with its setting off", kTag);
        DC::Say("cheatdrill D pass");
        Next();
        return;

    // E: the setting is on; each request runs on the host.
    case C::EWait:
        if (Seen("cheatdrill E on")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("client", "E: the host never turned the setting on");
        return;
    case C::EWaitOn:
        if (CH::PlayersMayCheat()) { UE_LOGI("%s client E: the host's setting arrived: on", kTag); g.paneIdx = 0; g.paneAsked = false; Next(); }
        else if (Timed(20000)) Invalid("client", "E: the host's setting never reached this client");
        return;
    case C::EPanes:
        if (OpenPanes("client", kClientPanesOn, 3)) {
            Points(&g.points);
            g.crowbars = Crowbars();
            g.replays = EFS::ReplayCount();
            g_nResults = 0;
            Next();
        }
        return;
    case C::EReqPoints:
        if (!Ask([] { CH::AddPoints(kClientPoints); }, &r, "E")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Ok)) { ClientFail("E", "the points request was answered " + std::to_string(r.reason) + ", not Ok"); return; }
        Next();
        return;
    case C::EReqSpawn:
        if (!Ask([] { CH::SpawnItem("crowbar", kSpawnCount, coop::net::kCheatInFront); }, &r, "E")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Ok) || r.done != kSpawnCount) {
            ClientFail("E", "the spawn request was answered " + std::to_string(r.reason) + " with " + std::to_string(r.done) + " spawned");
            return;
        }
        Next();
        return;
    case C::EReqAt:
        // The "Spawn at <player>" call: a prop at the host's own body (slot 0), which the client names by its slot.
        if (!Ask([] { CH::SpawnItem("crowbar", 1, 0); }, &r, "E")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Ok) || r.done != 1) {
            ClientFail("E", "the spawn-at-the-host request was answered " + std::to_string(r.reason) + " with " + std::to_string(r.done) + " spawned");
            return;
        }
        Next();
        return;
    case C::EReqEvent:
        if (!Ask([] { CH::FireEvent(kClientEvent); }, &r, "E")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Ok)) { ClientFail("E", "the event request was answered " + std::to_string(r.reason) + ", not Ok"); return; }
        Next();
        return;
    case C::EReqStory:
        if (!Ask([] { CH::FireEvent(kStoryEvent); }, &r, "E")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Refused)) { ClientFail("E", "the story event was answered " + std::to_string(r.reason) + ", not Refused"); return; }
        Next();
        return;
    case C::EReqBad:
        if (!Ask([] { CH::SpawnItem("nosuchitem_xyz", 1, coop::net::kCheatInFront); }, &r, "E")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::UnknownItem)) { ClientFail("E", "the unknown prop was answered " + std::to_string(r.reason) + ", not UnknownItem"); return; }
        Next();
        return;
    case C::ESent: DC::Say("cheatdrill E sent"); Next(); return;
    case C::EWaitMirror: {
        if (!DC::SeenRestOf("cheatdrill E host-ok ", &rest)) { if (Timed(kSyncBoundMs)) ClientInvalid("E", "the host never judged its side"); return; }
        int32_t p = 0;
        const bool mirrored = Points(&p) && p == std::atoi(rest.c_str()) && Crowbars() == g.crowbars + kSpawnCount + 1 &&
                              EFS::ReplayCount() >= g.replays + 1;
        if (mirrored) {
            UE_LOGI("%s client E PASS -- the host ran this client's points (balance %d), %d+1 crowbars and the event; the "
                    "story event and the unknown prop were refused", kTag, p, kSpawnCount);
            DC::Say("cheatdrill E pass");
            Next();
        } else if (Timed(kEffectBoundMs * 2)) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "this client did not mirror the host: balance %d (host %s), crowbars %d (want %d), replays %u (want %u)",
                          p, rest.c_str(), Crowbars(), g.crowbars + kSpawnCount + 1, EFS::ReplayCount(), g.replays + 1);
            ClientInvalid("E", buf);
        }
        return;
    }

    // G..L: the hostile arms, with the setting on and this client's right held until K.
    case C::Arms:
        if (ClientArms(session)) Next();
        return;

    // F: the setting reset; the next request is Off.
    case C::FWait:
        if (Seen("cheatdrill F off")) Next();
        else if (Timed(kSyncBoundMs)) Invalid("client", "F: the host never reset the setting");
        return;
    case C::FWaitOff:
        if (!CH::PlayersMayCheat()) { g_nResults = 0; Next(); }
        else if (Timed(20000)) Invalid("client", "F: the host's reset never reached this client");
        return;
    case C::FReq:
        if (!Ask([] { CH::AddPoints(5); }, &r, "F")) return;
        if (r.reason != static_cast<uint8_t>(CheatReason::Off)) { ClientFail("F", "the request was answered " + std::to_string(r.reason) + ", not Off"); return; }
        Next();
        return;
    case C::FPass:
        UE_LOGI("%s client F PASS -- refused again once the host reset the setting", kTag);
        DC::Say("cheatdrill F pass");
        Next();
        return;
    case C::Finish:
        if (Seen("cheatdrill done")) Verdict("client", true, "every arm passed");
        else if (Timed(kSyncBoundMs)) Invalid("client", "the host never said it was done");
        return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!Enabled() || g.done || !session || !session->running()) return;
    if (session->role() == coop::net::Role::Host) HostTick(session);
    else ClientTick(*session);
}

void OnDisconnect() {
    CloseMenu();
    CH::SetResultObserver(nullptr);
    g = State{};
    g_nResults = 0;
}

}  // namespace coop::dev::cheat_drill
