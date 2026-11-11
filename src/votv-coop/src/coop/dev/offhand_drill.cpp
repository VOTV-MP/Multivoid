// coop/dev/offhand_drill.cpp -- see coop/dev/offhand_drill.h.

#include "coop/dev/offhand_drill.h"

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace coop::dev::offhand_drill {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

enum class Phase { Arm, Held, PressedWait, Said, Dropped, Done };
Phase g_phase = Phase::Arm;
uint64_t g_atMs = 0;
bool g_hostSaw = false;
bool g_hostDone = false;
uint64_t g_hostAtMs = 0;
std::string g_droppedKey;

const std::string kHeldHead = "[OFFHAND-DRILL] client holds in the off hand: ";
const std::string kDropHead = "[OFFHAND-DRILL] client dropped key=";

bool FeedSaid(const std::string& head, std::string* rest) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = found ? std::string_view::npos : r.line.find(head);
        if (at == std::string_view::npos) return;
        found = true;
        if (rest) *rest = std::string(r.line.substr(at + head.size()));
    });
    return found;
}

bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

void Finish(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[OFFHAND-DRILL] client DONE %s", verdict);
}

std::wstring HandClass(void* hand) { return hand && R::IsLive(hand) ? R::ClassNameOf(hand) : std::wstring(); }

void ClientTick() {
    void* const player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player)) return;
    const uint64_t now = ::GetTickCount64();
    if (g_phase == Phase::Arm) {
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"crowbar", L"prop_crowbar_C")) {
            Finish("the crowbar could not be taken into the main hand -- INCONCLUSIVE");
            return;
        }
        g_atMs = now;
        g_phase = Phase::Held;
        return;
    }
    E::MainPlayerGrabState hand{};
    E::ReadMainPlayerGrabState(player, hand);
    if (g_phase == Phase::Held) {
        if (now - g_atMs < 1500) return;
        if (HandClass(hand.mainHand) != L"prop_crowbar_C") {
            Finish("the main hand does not hold the crowbar -- INCONCLUSIVE");
            return;
        }
        // The off-hand key's own handler: it moves the selected hotbar item into the off-hand slot.
        void* const fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"InpActEvt_offhand_K2Node_InputActionEvent_2");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || !ue_wrap::Call(player, f)) {
            Finish("the off-hand key's event did not run -- FAIL");
            return;
        }
        UE_LOGI("[OFFHAND-DRILL] client pressed the off-hand key's event with the crowbar in the main hand");
        g_atMs = now;
        g_phase = Phase::PressedWait;
        return;
    }
    if (g_phase == Phase::PressedWait) {
        if (HandClass(hand.offHand) == L"prop_crowbar_C" && now - g_atMs >= 3000) {
            UE_LOGI("[OFFHAND-DRILL] client off hand holds %ls, main hand holds '%ls'", HandClass(hand.offHand).c_str(),
                    HandClass(hand.mainHand).c_str());
            coop::chat_sync::QueueSend(kHeldHead + "prop_crowbar_C");
            g_atMs = now;
            g_phase = Phase::Said;
            return;
        }
        if (now - g_atMs > 8000) Finish("the off hand never held the crowbar after the key's event -- FAIL");
        return;
    }
    if (g_phase == Phase::Said) {
        if (now - g_atMs >= 3500) {   // the chat paces its lines: the drop's line follows the first by a few seconds
            g_atMs = now;
            g_phase = Phase::Dropped;
            // Drop it: the drop that sets the off hand's item down where the player aims.
            void* const fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"dropItem_offhand");
            ue_wrap::ParamFrame df(fn);
            if (!fn || !df.valid() || !df.Set<bool>(L"surface", true) || !df.Set<bool>(L"playercamera", false) ||
                !df.Set<bool>(L"dropone", false) || !ue_wrap::Call(player, df)) {
                Finish("dropItem_offhand did not run -- FAIL");
                return;
            }
            void* const dropped = df.Get<void*>(L"droppedObject");
            const std::wstring k = dropped ? ue_wrap::prop::GetInteractableKeyString(dropped) : L"";
            g_droppedKey.assign(k.begin(), k.end());
            UE_LOGI("[OFFHAND-DRILL] client dropped the off-hand item: key='%s' cls=%ls", g_droppedKey.c_str(),
                    HandClass(dropped).c_str());
            if (g_droppedKey.empty()) {
                Finish("the off-hand drop produced no keyed prop -- FAIL");
                return;
            }
            coop::chat_sync::QueueSend(kDropHead + g_droppedKey);
            return;
        }
        if (now - g_atMs > 8000) Finish("the off hand never held the crowbar after the key's event -- FAIL");
        return;
    }
    if (g_phase == Phase::Dropped && now - g_atMs >= 4000) {
        const bool landed = coop::prop_key_index::Find(std::wstring(g_droppedKey.begin(), g_droppedKey.end())) != nullptr;
        E::MainPlayerGrabState after{};
        E::ReadMainPlayerGrabState(player, after);
        Finish(landed && !after.offHand ? "the off-hand item was held, dropped and stands in this world, the off hand empty -- PASS"
                                       : "the dropped prop or the emptied off hand is not as expected -- FAIL");
    }
}

// The host's judge of what the client says: the off-hand mirror is in the log (hand_item), and the dropped prop's
// key has to resolve to a live prop in this world.
void HostTick() {
    const uint64_t now = ::GetTickCount64();
    std::string rest;
    if (!g_hostSaw && FeedSaid(kHeldHead, &rest)) {
        g_hostSaw = true;
        UE_LOGI("[OFFHAND-DRILL] host heard: the client holds %s in its off hand", rest.c_str());
    }
    if (!g_hostDone && FeedSaid(kDropHead, &rest)) {
        if (!g_hostAtMs) g_hostAtMs = now;
        if (now - g_hostAtMs < 3000) return;
        g_hostDone = true;
        const std::wstring key(rest.begin(), rest.end());
        void* const prop = coop::prop_key_index::Find(key);
        UE_LOGI("[OFFHAND-DRILL] host %s: the client's dropped key '%s' %s here (cls=%ls)",
                prop ? "PASS" : "FAIL", rest.c_str(), prop ? "resolves to a live prop" : "is not found", HandClass(prop).c_str());
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::offhand_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled()) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (coop::roster::LocalIsHost()) {
        HostTick();
        return;
    }
    if (g_phase != Phase::Done) ClientTick();
}

void OnDisconnect() {
    g_phase = Phase::Arm;
    g_hostSaw = g_hostDone = false;
    g_hostAtMs = 0;
    g_droppedKey.clear();
}

}  // namespace coop::dev::offhand_drill
