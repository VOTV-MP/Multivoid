// coop/dev/dingus_drill.cpp -- the dingus drill's log, chat, verdict, reads, targets, the lane's two reads and the two verbs a
// peer runs on a dingus, and the tick; see coop/dev/dingus_drill.h. The host's steps are dingus_drill_host.cpp, the client's
// dingus_drill_client.cpp.
//
// Every verdict is decided by an event, the line a peer says after the effect it waits on, or the lane's own edge (the host's
// birth drain, a client's bind line); a bound only says INVALID. A FAIL is said where it is decided and nothing after it runs.

#include "coop/dev/dingus_drill.h"

#include "dingus_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/roster.h"
#include "coop/props/dingus_sync.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/dingus.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::dingus_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace OI = ue_wrap::object_index;
namespace D  = ue_wrap::dingus;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace EL = coop::element;
namespace HB = coop::hand_item;
namespace RV = coop::rig_verdict;

namespace {

bool g_ended = false, g_armed = false;
int  g_stepTicks = 0, g_quiet = 0;

constexpr size_t kMaxRecs = 64;
std::vector<Rec> g_born, g_bound;

char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

std::string Format(const char* fmt, va_list ap) {
    char buf[600];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::dingus_drill);
        if (v == "find") return Arm::Find;
        if (v == "pocket") return Arm::Pocket;
        if (v == "loaded") return Arm::Loaded;
        if (v == "join") return Arm::Join;
        return Arm::Off;
    }();
    return a;
}

// ---- the log, the chat, the verdict and the bound -------------------------------------------------------------------------

void Fact(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    UE_LOGI("[DINGUS-DRILL] [%c] %s", Who(), text.c_str());
}

void Say(const std::string& line) { DC::Say(std::string(kPrefix) + line); }
bool Seen(const std::string& line) { return DC::SeenHas(std::string(kPrefix) + line); }
bool Rest(const std::string& prefix, std::string* rest) { return DC::SeenRestOf(std::string(kPrefix) + prefix, rest); }

// n integers off the front of `text`; false when one is missing.
bool ParseInts(const std::string& text, int n, int* out) {
    const char* p = text.c_str();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p) return false;
        p = end;
    }
    return true;
}

bool Late(const char* why) {
    if (++g_stepTicks <= kBoundTicks) return false;
    Invalid(why);
    return true;
}

void Invalid(const char* why) {
    RV::Say(kTag, RV::Outcome::Invalid, "%s", why);
    g_ended = true;
}

void Fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = Format(fmt, ap);
    va_end(ap);
    RV::Say(kTag, RV::Outcome::Fail, "%s", text.c_str());
    g_ended = true;
}

void Pass(const char* why) {
    RV::Say(kTag, RV::Outcome::Pass, "%s", why);
    g_ended = true;
}

bool Ended() { return g_ended; }
void Go() { g_stepTicks = 0; }
void Finish() { g_ended = true; }

// ---- reads of a copy ------------------------------------------------------------------------------------------------------

std::wstring KeyOf(void* d) { return d ? ue_wrap::prop::GetInteractableKeyString(d) : std::wstring(); }

int EidOf(void* actor) {
    const EL::ElementId eid = actor ? EL::Registry::Get().EidForActor(actor) : EL::kInvalidId;
    return eid == EL::kInvalidId ? 0 : static_cast<int>(eid);
}

void* LiveProp(int eid) {
    return eid > 0 ? EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop) : nullptr;
}

Reading ReadingOf(int found, int music) {
    if (found < 0 || music < 0) return Reading::Unread;
    return found == 1 && music == 0 ? Reading::Quiet : Reading::Loud;
}

// A key as the chat carries it: the save keys are ASCII, one byte a character.
std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

std::string Narrow(const std::wstring& ascii) {
    std::string out;
    for (const wchar_t ch : ascii) out.push_back(static_cast<char>(ch));
    return out;
}

void LogPlaced() {
    struct Count {
        const wchar_t* name;
        int n;
    } c[4] = {{L"prop_dingus_C", 0}, {L"prop_dingus2_C", 0}, {L"prop_dingus4ugly_C", 0}, {L"prop_dingus5erie_C", 0}};
    D::ForEachDingus([](void* ctx, void* actor) {
        Count* const counts = static_cast<Count*>(ctx);
        const std::wstring name = R::ClassNameOf(actor);
        for (int i = 0; i < 4; ++i)
            if (name == counts[i].name) ++counts[i].n;
    }, c);
    Fact("the dingus copies in this world at world-ready: prop_dingus_C=%d prop_dingus2_C=%d prop_dingus4ugly_C=%d "
         "prop_dingus5erie_C=%d", c[0].n, c[1].n, c[2].n, c[3].n);
}

// ---- targets --------------------------------------------------------------------------------------------------------------

// A dingus of class `cls` `fwdCm` before `who` and `rightCm` to its right, dropped from the actor's height and finished.
void* SpawnDingus(void* who, float fwdCm, float rightCm, const wchar_t* cls) {
    void* c = OI::ClassByName(cls);
    if (!c) {
        std::wstring stem = cls;
        stem.resize(stem.size() - 2);   // the class name ends in "_C"
        c = ue_wrap::asset_load::LoadObjectByPath((L"/Game/objects/" + stem + L"." + stem + L"_C").c_str());
    }
    ue_wrap::FVector at{};
    if (!c || !who || !E::TryGetActorLocation(who, at)) return nullptr;
    const ue_wrap::FVector f = E::GetActorForwardVector(who);
    return E::SpawnActor(c, {at.X + f.X * fwdCm - f.Y * rightCm, at.Y + f.Y * fwdCm + f.X * rightCm, at.Z});
}

bool Quiet(void* const* actors, int n) {
    for (int i = 0; i < n; ++i) {
        if (actors[i] && DG::Quiet(actors[i])) continue;
        g_quiet = 0;
        return false;
    }
    return ++g_quiet >= kQuietTicks;
}

bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1) &&
           !coop::join_window_baseline::IsLateWindowOpen(1);
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// ---- the lane's two reads -------------------------------------------------------------------------------------------------

namespace {

Rec ReadRec(void* actor, const std::wstring& key) {
    Rec r;
    r.key = key;
    r.eid = EidOf(actor);
    r.found = D::ReadFound(actor);
    r.music = D::ReadMusicOn(actor);
    return r;
}

void Keep(std::vector<Rec>& to, const char* what, void* actor, const std::wstring& key) {
    if (to.size() >= kMaxRecs) {
        Invalid("the drill's record of copies is full");
        return;
    }
    to.push_back(ReadRec(actor, key));
    const Rec& r = to.back();
    Fact("%s of '%ls': eid=%d isUsed=%d music=%d", what, key.c_str(), r.eid, r.found, r.music);
}

void OnBornRead(void* actor, const std::wstring& key) { Keep(g_born, "the host's birth drain read a copy", actor, key); }
void OnBoundRead(void* actor, const std::wstring& key) { Keep(g_bound, "this client's bind line read a copy", actor, key); }

const coop::dingus_sync::dev::Observer g_observer{&OnBornRead, &OnBoundRead};

const Rec* Find(const std::vector<Rec>& from, const std::wstring& key, int exceptEid, int alsoExceptEid, int onlyEid) {
    for (const Rec& r : from) {
        if ((!key.empty() && r.key != key) || r.eid == 0 || r.eid == exceptEid || (alsoExceptEid != 0 && r.eid == alsoExceptEid)) continue;
        if (onlyEid != 0 && r.eid != onlyEid) continue;
        return &r;
    }
    return nullptr;
}

}  // namespace

void ArmObserver() { coop::dingus_sync::dev::SetObserver(&g_observer); }
const Rec* FindBorn(const std::wstring& key, int exceptEid, int alsoExceptEid) { return Find(g_born, key, exceptEid, alsoExceptEid, 0); }
const Rec* FindBound(const std::wstring& key, int exceptEid, int onlyEid) { return Find(g_bound, key, exceptEid, 0, onlyEid); }

// ---- one grab, then the let-go --------------------------------------------------------------------------------------------

GrabRun::GrabRun() = default;
GrabRun::~GrabRun() = default;

void GrabRun::Begin(void* player, void* prop) {
    player_ = player;
    letGo_ = false;
    why_ = "";
    grab_ = std::make_unique<coop::director::AimedGrab>(player, prop);
}

bool GrabRun::NoAim() const { return grab_ && grab_->NoAim(); }

GrabRun::R GrabRun::Tick() {
    if (!letGo_) {
        const coop::director::GrabState gs = grab_->Tick();
        if (gs == coop::director::GrabState::Working) return R::Working;
        if (gs == coop::director::GrabState::Failed) {
            why_ = grab_->Why();
            return R::Failed;
        }
        Fact("the grab ended grabbed after %d fan pose(s); chain: playerGrabbed_pre=%d useAction=%d playerGrabbed=%d", grab_->AimPoses(),
             grab_->ChainPre() ? 1 : 0, grab_->ChainUse() ? 1 : 0, grab_->ChainPost() ? 1 : 0);
        Fact("the let-go (dropGrabObject) ran=%d", coop::director::CallOnPlayer(player_, L"dropGrabObject") ? 1 : 0);
        letGo_ = true;
    }
    return coop::director::Grabbing(player_) ? R::Working : R::Grabbed;
}

// ---- the pocket verb and the drop -----------------------------------------------------------------------------------------

namespace {

// The game's own drop of the hand item, as the drop key's call does it: not into the inventory (dontCollect).
bool DropHeld(void* player) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"simulateDrop");
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<bool>(L"dontCollect", true) && ue_wrap::Call(player, f);
}

}  // namespace

void PocketRun::Begin(void* player, void* target, bool client) {
    player_.Set(player);
    target_.Set(target);
    client_ = client;
    stage_ = ticks_ = 0;
    why_ = "";
}

PocketRun::R PocketRun::Tick() {
    void* const me = player_.Get();
    if (!me) {
        why_ = "the player is gone";
        return R::Failed;
    }
    switch (stage_) {
    case 0: {
        void* const t = target_.Get();
        if (!t) {
            why_ = "the dingus is gone before the pocket verb";
            return R::Failed;
        }
        // The player's own pickup: `Hold Object` with `manual` set, the call the hand key makes on what it looks at. It runs no
        // `touch`: a hold never finds a dingus.
        bool collected = false;
        const bool ran = E::CallMainPlayerHoldObject(me, t, collected);
        Fact("the pocket verb (Hold Object) on '%ls' ran=%d collected=%d", KeyOf(t).c_str(), ran ? 1 : 0, collected ? 1 : 0);
        if (!ran) {
            why_ = "Hold Object did not run";
            return R::Failed;
        }
        stage_ = 1;
        ticks_ = 0;
        return R::Working;
    }
    default: {
        if (client_ && coop::prop_act_request::dev::PendingId() != 0) return R::Working;
        void* const hand = HB::LocalHandActor();
        if (!(hand && D::IsDingus(hand) && HB::OwnAnnounced(ue_wrap::reflection::ClassNameOf(hand).c_str()))) {
            if (++ticks_ < kHandTicks) return R::Working;
            why_ = "the hand holds no dingus the lane announced after the pocket verb";
            return R::Failed;
        }
        // The held dingus is announced: the drop goes in the same tick.
        if (!DropHeld(me)) {
            why_ = "simulateDrop did not run";
            return R::Failed;
        }
        Fact("the drop (simulateDrop, not into the inventory) ran");
        return R::Dropped;
    }
    }
}

// ---- the tick -------------------------------------------------------------------------------------------------------------

void ArmFirstTick() {
    if (g_armed) return;
    g_armed = true;
    DC::SeenHas(kPrefix);   // the first read starts the chat history
    ArmObserver();
}

void ResetAll() {
    g_ended = g_armed = false;
    g_stepTicks = g_quiet = 0;
    g_born.clear();
    g_bound.clear();
    coop::dingus_sync::dev::SetObserver(nullptr);
    ResetHost();
    ResetClient();
}

}  // namespace coop::dev::dingus_drill::detail

namespace coop::dev::dingus_drill {

void Tick(coop::net::Session* session) {
    namespace dd = detail;
    if (!session || dd::ArmOf() == dd::Arm::Off || dd::Ended()) return;
    dd::ArmFirstTick();
    if (coop::roster::LocalIsHost()) dd::HostTick(*session);
    else dd::ClientTick();
}

void OnDisconnect() { detail::ResetAll(); }

}  // namespace coop::dev::dingus_drill
