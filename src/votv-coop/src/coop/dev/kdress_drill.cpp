// coop/dev/kdress_drill.cpp -- the kdress drill's log, chat, verdict, reads, targets and tick; see coop/dev/kdress_drill.h.
// The host's steps are kdress_drill_host.cpp and kdress_drill_host_arms.cpp, the client's kdress_drill_client.cpp and
// kdress_drill_client_arms.cpp.
//
// Every verdict is decided by an event: the line a peer says after the effect it waits on, or a counter the lane keeps. A
// FAIL is said where it is decided and nothing after it runs; a bound only says INVALID.

#include "coop/dev/kdress_drill.h"
#include "kdress_drill_detail.h"

#include "coop/cheats/item_catalog.h"
#include "coop/cheats/item_spawn.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/roster.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::kdress_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace K  = ue_wrap::kerfur;
namespace KD = ue_wrap::kerfur_dress;
namespace OI = ue_wrap::object_index;
namespace P  = ue_wrap::profile;
namespace DC = coop::dev::drill_chat;
namespace DH = coop::dev::drill_hand;
namespace EL = coop::element;
namespace IS = coop::cheats::item_spawn;
namespace RV = coop::rig_verdict;
using NpcMirrors = coop::element::MirrorManager<coop::element::Npc>;

namespace {

bool g_ended = false, g_armed = false;

char Who() { return coop::roster::LocalIsHost() ? 'H' : 'C'; }

std::string Format(const char* fmt, va_list ap) {
    char buf[700];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    return buf;
}

}  // namespace

int g_stepTicks = 0;
int g_eidC = 0;
ue_wrap::CachedObjRef g_c;

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::kdress_drill);
        if (v == "puton") return Arm::PutOn;
        if (v == "takeoff") return Arm::TakeOff;
        if (v == "pat") return Arm::Pat;
        if (v == "born") return Arm::Born;
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
    UE_LOGI("[KDRESS] [%c] %s", Who(), text.c_str());
}

void Say(const std::string& line) { DC::Say(std::string(kPrefix) + line); }
bool Seen(const std::string& line) { return DC::SeenHas(std::string(kPrefix) + line); }
bool Rest(const std::string& prefix, std::string* rest) { return DC::SeenRestOf(std::string(kPrefix) + prefix, rest); }

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

void Finish() { g_ended = true; }
bool Ended() { return g_ended; }
void Go() { g_stepTicks = 0; }

// ---- reads ---------------------------------------------------------------------------------------------------------------

int OmegaEid(void* omega) {
    if (!omega) return 0;
    std::vector<coop::element::Npc*> elems;
    NpcMirrors::Instance().Snapshot(elems);
    for (coop::element::Npc* el : elems)
        if (el && el->GetActor() == omega) return static_cast<int>(el->GetId());
    return static_cast<int>(coop::npc_sync::GetNpcIdForActor(omega));
}

void* OmegaOf(int eid) {
    auto* const el = eid > 0 ? NpcMirrors::Instance().Get(static_cast<EL::ElementId>(eid)) : nullptr;
    void* const actor = el ? el->GetActor() : nullptr;
    return actor && R::IsLiveByIndex(actor, el->GetInternalIdx()) && K::IsKerfurActor(actor) ? actor : nullptr;
}

std::string DripText(void* omega) {
    std::vector<std::wstring> rows;
    if (!omega || !KD::ReadDrip(omega, rows)) return "?";
    if (rows.empty()) return "-";
    std::string out;
    for (const std::wstring& r : rows) out += (out.empty() ? "" : ",") + Narrow(r);
    return out;
}

bool DripHas(void* omega, const wchar_t* row) {
    std::vector<std::wstring> rows;
    if (!omega || !KD::ReadDrip(omega, rows)) return false;
    for (const std::wstring& r : rows)
        if (r == row) return true;
    return false;
}

bool HandEmpty(void* player) {
    std::wstring name;
    void* held = nullptr;
    return player && ue_wrap::miner::ReadHold(player, name, held) && (!held || !R::IsLive(held));
}

std::string Narrow(const std::wstring& ascii) {
    std::string out;
    for (const wchar_t ch : ascii) out.push_back(static_cast<char>(ch));
    return out;
}

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

int PropEid(void* actor) {
    const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
    return eid == EL::kInvalidId ? 0 : static_cast<int>(eid);
}

void* LiveProp(int eid) {
    return eid > 0 ? EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop) : nullptr;
}

std::vector<void*> LiveOf(const wchar_t* cls) {
    std::vector<void*> out;
    if (void* const c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__"))
                static_cast<std::vector<void*>*>(ctx)->push_back(obj);
        }, &out);
    return out;
}

std::wstring ItemClass(const wchar_t* row) {
    std::wstring cls;
    IS::SpawnClassName(Narrow(row), cls);
    return cls;
}

int NearCount(const wchar_t* cls, void* omega) {
    ue_wrap::FVector there{};
    if (!omega || !E::TryGetActorLocation(omega, there)) return -1;
    int n = 0;
    for (void* o : LiveOf(cls)) {
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(o, at)) continue;
        const float dx = at.X - there.X, dy = at.Y - there.Y, dz = at.Z - there.Z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kNearCm) ++n;
    }
    return n;
}

// The player's own use of the held item on `omega`: its playerUsedOn with the hand actor and the held name, the call the
// game's use key makes through the interface (the hit and the component stay empty: the Omega's body reads neither).
bool UseOn(void* player, void* omega) {
    void* const hand = coop::hand_item::LocalHandActor();
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(omega), L"playerUsedOn"));
    const int32_t off = R::FindPropertyOffset(R::ClassOf(player), L"holding_name");
    if (!f.valid() || off < 0) return false;
    R::FName held;
    std::memcpy(&held, static_cast<const char*>(player) + off, sizeof(held));
    return f.Set<void*>(L"player", player) && f.Set<void*>(L"holdObject", hand) &&
           f.SetRaw(L"holdPropName", &held, sizeof(held)) && ue_wrap::Call(omega, f);
}

// ---- targets -------------------------------------------------------------------------------------------------------------

void* SpawnOmega(void* who, float fwdCm, float rightCm) {
    ue_wrap::FVector at{};
    if (!who || !E::TryGetActorLocation(who, at)) return nullptr;
    const ue_wrap::FVector f = E::GetActorForwardVector(who);
    return coop::dev::spawn_npc::SpawnAt(P::name::NpcClass_KerfurOmega,
                                         {at.X + f.X * fwdCm - f.Y * rightCm, at.Y + f.Y * fwdCm + f.X * rightCm, at.Z});
}

namespace {

void* g_restOf = nullptr;
ue_wrap::FVector g_restAt{};
int g_restTicks = 0;

}  // namespace

// An Omega is a character that can walk off: at rest is the same spot (within a centimetre) read on kQuietTicks ticks running.
bool AtRest(void* actor) {
    ue_wrap::FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return false;
    const bool same = actor == g_restOf && std::fabs(at.X - g_restAt.X) < 1.f && std::fabs(at.Y - g_restAt.Y) < 1.f &&
                      std::fabs(at.Z - g_restAt.Z) < 1.f;
    g_restOf = actor;
    g_restAt = at;
    g_restTicks = same ? g_restTicks + 1 : 0;
    return g_restTicks >= kQuietTicks;
}

bool HostMayStart(coop::net::Session& s) {
    return s.IsSlotWorldReady(1) && coop::prop_snapshot::IsBracketClosed(1) &&
           !coop::join_window_baseline::IsLateWindowOpen(1);
}

bool ClientMayStart() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// The rows' classes come from the game's prop list, which the game fills some time into a new world; until then the catalog
// can answer nothing about a name, so a step waits for its list (bounded, INVALID only).
bool CatalogReady() {
    std::wstring canonical;
    if (coop::cheats::item_catalog::Resolve(Narrow(kRow), canonical) != coop::cheats::item_catalog::Lookup::Unfilled) return true;
    Late("the game never filled its prop list");
    return false;
}

bool DressOmega(void* me, void* omega) {
    const std::wstring cls = ItemClass(kRow);
    if (cls.empty()) {
        Invalid("the row names no class the game spawns");
        return false;
    }
    const bool held = DH::TakeIntoHandNow(kRow, cls.c_str());
    const bool ran = held && UseOn(me, omega);
    const bool emptied = DH::TakeIntoHandNow(L"None", nullptr);
    Fact("the host's hand took '%ls' held=%d, its playerUsedOn on the Omega ran=%d, the hand emptied=%d", kRow, held ? 1 : 0,
         ran ? 1 : 0, emptied ? 1 : 0);
    if (!DripHas(omega, kRow)) {
        Invalid("the host's own put-on did not dress the Omega");
        return false;
    }
    return true;
}

// ---- the tick --------------------------------------------------------------------------------------------------------------

void TickImpl(coop::net::Session& session) {
    if (ArmOf() == Arm::Off || g_ended) return;
    if (!g_armed) {
        g_armed = true;
        DC::SeenHas(kPrefix);   // the first read starts the chat history
    }
    if (coop::roster::LocalIsHost()) HostTick(session);
    else ClientTick();
}

void Reset() {
    g_ended = g_armed = false;
    g_stepTicks = g_restTicks = g_eidC = 0;
    g_restOf = nullptr;
    g_c.Reset();
    ResetHost();
    ResetClient();
}

}  // namespace coop::dev::kdress_drill::detail

namespace coop::dev::kdress_drill {

void Tick(coop::net::Session* session) {
    if (session) detail::TickImpl(*session);
}

void OnDisconnect() { detail::Reset(); }

}  // namespace coop::dev::kdress_drill
