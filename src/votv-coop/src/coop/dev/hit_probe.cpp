// coop/dev/hit_probe.cpp -- [dev] a player's melee hit (VOTVCOOP_HIT_PROBE=client|host, the hitter, on both peers), for
// Ghezra/Multivoid#41: a crowbar's hit sound reached no other player.
//
// THE HITTER, 24 s after a client is connected: takes a crowbar into its hand and looks at the floor before it; at 28 s,
// 30 s and 32 s it swings through its own mainPlayer_C::attack, as its attack key does. The sounds are logged by
// coop/world/sound_relay on both peers (`sound_relay:`). VOTVCOOP_HIT_PROBE=knock instead: the host fires the vent
// knocker event (ventKnocker, as the dev event menu does) at 24 s and runs the knocker's own knock at 28 s and 30 s, as
// its box's overlap does.

#include "coop/dev/hit_probe.h"

#include "coop/config/config.h"
#include "coop/dev/drill_hand.h"
#include "coop/dev/event_trigger.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::hit_probe {
namespace {

namespace R = ue_wrap::reflection;

uint64_t Now() { return ::GetTickCount64(); }

const std::string& Who() {
    static const std::string v = coop::config::ReadEnv("VOTVCOOP_HIT_PROBE");
    return v;
}

uint64_t g_startAt = 0;
int g_step = 0;

}  // namespace

void Tick(coop::net::Session* session) {
    if (Who().empty() || !session || !session->running() || !session->connected()) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_startAt) {
        g_startAt = Now();
        UE_LOGI("[HIT] probe armed role=%s hitter=%s", host ? "host" : "client", Who().c_str());
    }
    const uint64_t t = Now() - g_startAt;
    if (Who() == "knock") {
        if (!host) return;
        if (g_step == 0 && t >= 24000u) {
            g_step = 1;
            bool ok = false;
            for (const auto& ev : coop::dev::event_trigger::Events())
                if (std::strcmp(ev.name, "ventKnocker") == 0) ok = coop::dev::event_trigger::Trigger(ev);
            UE_LOGI("[HIT] host: ventKnocker fired (ok=%d)", ok ? 1 : 0);
        }
        if ((g_step == 1 && t >= 28000u) || (g_step == 2 && t >= 30000u)) {
            ++g_step;
            void* kocker = nullptr;
            for (void* o : R::FindObjectsByClass(L"kocker_C"))
                if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) kocker = o;
            ue_wrap::ParamFrame f(kocker ? R::FindDispatchFunctionCached(R::ClassOf(kocker), L"knock") : nullptr);
            UE_LOGI("[HIT] host: the knocker's knock (knocker %d, ran=%d)", kocker ? 1 : 0,
                    f.valid() && ue_wrap::Call(kocker, f) ? 1 : 0);
        }
        return;
    }
    if (Who() != (host ? "host" : "client")) return;
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    if (g_step == 0 && t >= 24000u) {
        g_step = 1;
        const bool held = coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C");
        void* const pc = ue_wrap::engine::GetController(me);
        ue_wrap::FRotator r = pc ? ue_wrap::engine::GetControlRotation(pc) : ue_wrap::FRotator{};
        r.Pitch = -70.f;
        if (pc) ue_wrap::engine::SetControlRotation(pc, r);
        UE_LOGI("[HIT] hitter: a crowbar in the hand (%d), looking at the floor", held ? 1 : 0);
    }
    if (g_step >= 1 && g_step <= 3 && t >= 26000u + 2000u * static_cast<uint64_t>(g_step)) {
        ++g_step;
        ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(me), L"attack"));
        const bool ran = f.valid() && ue_wrap::Call(me, f);
        UE_LOGI("[HIT] hitter: swing %d (ran=%d, hit=%d)", g_step - 1, ran ? 1 : 0, ran && f.Get<bool>(L"hit") ? 1 : 0);
    }
}

}  // namespace coop::dev::hit_probe
