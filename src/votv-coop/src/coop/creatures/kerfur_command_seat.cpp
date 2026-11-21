// coop/creatures/kerfur_command_seat.cpp -- see coop/creatures/kerfur_command_seat.h.

#include "coop/creatures/kerfur_command_seat.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/kerfur_command.h"
#include "coop/creatures/kerfur_dress_sync.h"
#include "coop/creatures/served_player.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kPeerIdHost

#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cwchar>
#include <string>

namespace coop::kerfur_command_seat {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// Declared on kerfurOmega_C and on none of its subclasses, so the one name watch matches every Omega skin.
UE_FACT_FN(kRowActionName, "kerfur_command_seat", L"kerfurOmega_C", L"actionName", Declared);
constexpr int kTagActionName = 0x4B435301;  // 'KCS' 1
constexpr int32_t kMaxNameChars = 256;  // the longest action name read, its terminator counted

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false;
bool g_live = false;
bool g_dead = false;
R::ParamOffset g_nameOff{L"name"};

// Never set in a shipped run: a client's seat runs every action, so its own copy runs a relayed verb (the red arm).
bool GateOff() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::kerfur_command_gate_off);
    return s;
}

// The action's case differs by route (the radial's `Get_reports`, a reflected call's `get_reports`): the relayed
// set matches without case. The decision is the name and the role, never where the call came from. On the host the
// press is taken only to hand an Omega that serves a client back to the host's own player; an Omega nobody serves
// runs the host's press untouched.
sg::Verdict OnActionNamePre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return sg::Verdict::Run;  // single player is untouched
    const int32_t off = g_nameOff.Of(c.function);
    if (off < 0 || !c.locals) return sg::Verdict::Run;
    // An FString frame is {Data, Num, Max}; a length no action name has is not read.
    const int32_t num = *reinterpret_cast<const int32_t*>(c.locals + off + sizeof(void*));
    if (num <= 1 || num > kMaxNameChars) return sg::Verdict::Run;
    // The role decides first: a press this machine's seat does not take is read only when its name is as long as "pat" and
    // its NUL (a pat runs where it is picked, either role).
    const bool isClient = s->role() == coop::net::Role::Client;
    const bool untaken = isClient ? GateOff() : coop::served_player::ServedSlot(c.object) == coop::players::kPeerIdHost;
    constexpr int32_t kPatChars = 4;
    if (untaken && num != kPatChars) return sg::Verdict::Run;
    const std::wstring name = ue_wrap::field_io::ReadFStringAt(c.locals, off);
    // At a pat's POST a client asks the host to show it and the host shows it to every client (kerfur_dress_sync).
    if (_wcsicmp(name.c_str(), L"pat") == 0) {
        coop::kerfur_dress_sync::OnPatPre(c.object);
        return sg::Verdict::Run;
    }
    if (untaken) return sg::Verdict::Run;
    if (!coop::kerfur_command::TryRecordMenuCommand(c.object, name, isClient)) return sg::Verdict::Run;
    UE_LOGI("kerfur_command_seat: %s '%ls' on kerfur %p taken at the seat, %s", isClient ? "client" : "host",
            name.c_str(), c.object, c.callerFunction ? "from a Blueprint call" : "from a reflected call");
    return sg::Verdict::Cancel;
}

// The end of any action: a pat the PRE armed has run on this copy.
void OnActionNamePost(const sg::Call& c) { coop::kerfur_dress_sync::OnPatPost(c.object); }

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_dead) return;
    if (!g_watched) {
        g_watched = sg::WatchName(kRowActionName, kTagActionName, &OnActionNamePre, &OnActionNamePost);
        if (!g_watched) {
            g_dead = true;
            UE_LOGE("kerfur_command_seat: the gate took no watch on actionName -- an Omega's radial command runs on "
                    "the machine it is picked on, for the rest of this process");
            return;
        }
    }
    if (g_live) return;
    sg::ResolvePendingNames();
    if ((g_live = sg::NameWatchLive(kRowActionName, kTagActionName))) {
        UE_LOGI("kerfur_command_seat: the Omega's actionName is watched -- its radial commands are decided at the seat");
    } else if (sg::NameWatchSettled(kRowActionName, kTagActionName)) {
        g_dead = true;
        UE_LOGE("kerfur_command_seat: the actionName watch died at the name resolve -- an Omega's radial command runs "
                "on the machine it is picked on, for the rest of this process");
    }
}

void OnDisconnect() { g_session.store(nullptr, std::memory_order_release); }

}  // namespace coop::kerfur_command_seat
