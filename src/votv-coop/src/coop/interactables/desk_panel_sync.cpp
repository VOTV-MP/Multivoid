// coop/interactables/desk_panel_sync.cpp -- see coop/interactables/desk_panel_sync.h.

#include "coop/interactables/desk_panel_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"  // which peer's band an actor's element id is from
#include "coop/net/session.h"
#include "coop/player/hand_item.h"  // a panel in this player's hand
#include "coop/player/players_registry.h"
#include "coop/props/prop_lifecycle.h"        // DestroyLocalProp: the refused take-off's panel prop
#include "coop/props/remote_prop.h"           // IsActorUnderAnyDrive: a panel prop another peer carries
#include "coop/session/net_pump.h"  // IsInAnnouncedWorld: a client's own world load is no edit

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_panels.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // the local hand lets go of a panel the first peer carries
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <chrono>

namespace coop::desk_panel_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace DP = ue_wrap::desk_panels;
namespace CD = ue_wrap::console_desk;
namespace sg = ue_wrap::script_gate;
namespace op = coop::net::desk_panel;
using Clock = std::chrono::steady_clock;

// The one panel the desk never lets go: a wrench on it gets "This pannel does not move".
constexpr int kFixedPanel = 2;
constexpr uint8_t kAllPanels = static_cast<uint8_t>((1u << DP::kPanels) - 1u);

std::atomic<coop::net::Session*> g_session{nullptr};

// The arrays as this peer last sent or took them. Unset until the desk first reads: that first read is
// the save's load, which is no edit.
DP::Masks g_known{};
bool      g_haveKnown = false;

// A canonical that arrived before the desk resolved (a joiner's replay racing its world load).
DP::Masks g_pendingCanon{};
bool      g_havePendingCanon = false;
constexpr auto kParkedRetry = std::chrono::milliseconds(1000);
Clock::time_point g_nextParkedTry{};

uint64_t g_adopted = 0;

// HOST: the take-offs it refused because another peer's came first. The refused peer's press already
// spawned its panel prop, whose birth follows the edit; the birth author reads this to drop it.
struct DenyRec {
    uint8_t sender = 0xFF;
    uint8_t panel = 0;
    Clock::time_point until{};
};
constexpr int kDenyRecs = 4;
constexpr auto kDenyTtl = std::chrono::seconds(10);
DenyRec g_denies[kDenyRecs];
uint64_t g_denied = 0;

UE_FACT_FN(kRowActionOptionIndex, "desk_panel_sync", L"analogDScreenTest_C", L"actionOptionIndex", Declared);
constexpr int kTagPress = 0x44504E4C;  // 'DPNL'
bool g_pressWatched = false;

// The desk's overlap puts a panel prop that enters its lid back on. One desk decides for a panel: its
// carrier's while a player carries it, the host's while nobody does. Every other copy is moved by the
// wire -- a carrier's pose, a release's last pose, a correction -- which can set it back inside the lid
// it just left, so the body is refused there.
UE_FACT_FN(kRowBeginOverlap, "desk_panel_sync", L"analogDScreenTest_C", L"ReceiveActorBeginOverlap", Declared);
constexpr int kTagOverlap = 0x44504F56;  // 'DPOV'
bool g_overlapWatched = false;
R::ParamOffset g_overlapOther{L"OtherActor"};
uint64_t g_overlapsRefused = 0;

void SendEdit(coop::net::Session* s, uint8_t which, int panel, bool value) {
    coop::net::DeskPanelStatePayload p{};
    p.op = which;
    p.panel = static_cast<uint8_t>(panel);
    p.value = value ? 1 : 0;
    s->SendReliableToSlot(0, coop::net::ReliableKind::DeskPanelState, &p, sizeof(p));
}

void HostBroadcastCanonical(coop::net::Session* s, const DP::Masks& m, int onlySlot = -1) {
    coop::net::DeskPanelStatePayload p{};
    p.op = op::kCanonical;
    p.onMask = m.on;
    p.unscrewedMask = m.unscrewed;
    if (onlySlot >= 0)
        s->SendReliableToSlot(onlySlot, coop::net::ReliableKind::DeskPanelState, &p, sizeof(p));
    else
        s->SendReliable(coop::net::ReliableKind::DeskPanelState, &p, sizeof(p));
}

void RecordDeny(uint8_t sender, uint8_t panel) {
    const auto now = Clock::now();
    DenyRec* slot = &g_denies[0];
    for (auto& d : g_denies)
        if (d.sender == 0xFF || now >= d.until) { slot = &d; break; }
    *slot = DenyRec{sender, panel, now + kDenyTtl};
}

// CLIENT: this peer's take-off of `panel` came after another's. Its press spawned a panel prop the
// host will not admit. The first peer's panel is born at the same place, and its birth, when it
// came before this deny, took the press's prop as its copy and bound it to the host's element id:
// that copy stays and this player's hand lets it go, since the first peer carries it. A panel prop
// of that place under no host id is the press's own still, and is destroyed now, before a birth can
// take it. The canonical that follows puts the desk back.
void ClientHandleDeny(uint8_t panel) {
    void* cls = DP::ClassForPanel(panel);
    if (!cls) {
        UE_LOGW("desk_panels: take-off of panel %u refused -- its prop class is unresolved, nothing swept", panel);
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    int swept = 0, letGo = 0;
    for (void* a : R::FindObjectsByClass(R::ToString(R::NameOf(cls)).c_str())) {
        if (!a || !R::IsLive(a)) continue;
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(a);
        if (!coop::element::Registry::IsAllowedHostAllocatedEid(eid)) {
            coop::prop_lifecycle::DestroyLocalProp(a, /*deferred*/false);
            ++swept;
        } else if (player && ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(player, a)) {
            ++letGo;
        }
    }
    ++g_denied;
    UE_LOGW("desk_panels: take-off of panel %u refused, another peer took it first -- destroyed %d panel prop(s) "
            "of this peer's press, let go of %d the first peer carries", panel, swept, letGo);
}

// Write the masks into this peer's desk and repaint it. A panel that goes from on to gone raises
// panelTakeOff, which the desk's own end-overlap clears once the panel prop has left every lid.
bool WriteDesk(const DP::Masks& from, const DP::Masks& to) {
    if (!DP::Write(to)) return false;
    if ((from.on & ~to.on) != 0) DP::WriteTakeOff(true);
    DP::CallUpdPanels();
    g_known = to;
    g_haveKnown = true;
    return true;
}

// The arrays against what this peer last knew: a difference is this peer's own player's edit. The
// host's arrays are the canonical, so it broadcasts them; a client sends the host each bit that moved.
void SendLocalEdits() {
    if (!DP::EnsureResolved() || !CD::Instance()) return;
    DP::Masks live{};
    if (!DP::Read(live)) return;
    if (!g_haveKnown) {
        g_known = live;
        g_haveKnown = true;
        return;
    }
    if (live == g_known) return;
    const DP::Masks before = g_known;
    g_known = live;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return;
    const bool isHost = (s->role() == coop::net::Role::Host);
    // A desk of a world this client has not announced ready is still taking the host's save: what its
    // load writes is known, not sent, and the connect replay's canonical follows the announce.
    if (!isHost && !coop::net_pump::IsInAnnouncedWorld(CD::Instance())) return;
    UE_LOGI("desk_panels: local edit on=0x%X->0x%X unscrewed=0x%X->0x%X -- %s", before.on, live.on,
            before.unscrewed, live.unscrewed, isHost ? "canonical broadcast" : "edits to the host");
    if (isHost) {
        HostBroadcastCanonical(s, live);
        return;
    }
    // The bolts first: the host takes a panel off only once its own desk has it unbolted.
    for (int i = 0; i < DP::kPanels; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((before.unscrewed ^ live.unscrewed) & bit) SendEdit(s, op::kSetUnscrewed, i, (live.unscrewed & bit) != 0);
    }
    for (int i = 0; i < DP::kPanels; ++i) {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((before.on ^ live.on) & bit) SendEdit(s, op::kSetOn, i, (live.on & bit) != 0);
    }
}

// The end of the desk's E press: a take-off's edit goes out here, ahead of the birth of the panel prop
// the press spawned, which the birth lanes send on a later pass.
void OnPressPost(const sg::Call&) { SendLocalEdits(); }

sg::Verdict OnOverlapPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected()) return sg::Verdict::Run;
    const int32_t off = c.function ? g_overlapOther.Of(c.function) : -1;
    if (off < 0 || !c.locals) return sg::Verdict::Run;
    void* other = *reinterpret_cast<void* const*>(c.locals + off);
    if (!other || !R::IsLive(other) || !DP::EnsureResolved() || !DP::IsPanelClass(R::ClassOf(other)))
        return sg::Verdict::Run;
    void* player = coop::players::Registry::Get().Local();
    if ((player && ue_wrap::engine::IsMainPlayerGrabbing(player, other)) ||
        coop::hand_item::IsLocalHandViewActor(other))
        return sg::Verdict::Run;  // this player carries it
    const bool carried = coop::remote_prop::IsActorUnderAnyDrive(other);
    if (s->role() == coop::net::Role::Host && !carried) return sg::Verdict::Run;  // nobody does
    if (g_overlapsRefused++ % 50 == 0)
        UE_LOGI("desk_panels: a panel prop's copy entered the desk here -- %s puts it back (n=%llu)",
                carried ? "its carrier's desk" : "the host's desk",
                static_cast<unsigned long long>(g_overlapsRefused));
    return sg::Verdict::Cancel;
}

bool AdoptCanonical(const DP::Masks& m) {
    DP::Masks live{};
    if (!DP::Read(live)) return false;
    if (live == m) {
        g_known = m;
        g_haveKnown = true;
        return true;
    }
    if (!WriteDesk(live, m)) return false;
    ++g_adopted;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_pressWatched) g_pressWatched = sg::WatchName(kRowActionOptionIndex, kTagPress, nullptr, &OnPressPost);
    if (!g_overlapWatched) g_overlapWatched = sg::WatchName(kRowBeginOverlap, kTagOverlap, &OnOverlapPre, nullptr);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (g_havePendingCanon) {
        const auto now = Clock::now();
        if (now < g_nextParkedTry) return;
        g_nextParkedTry = now + kParkedRetry;
        if (!DP::EnsureResolved() || !CD::Instance()) return;  // backoff inside
        g_havePendingCanon = false;
        if (AdoptCanonical(g_pendingCanon))
            UE_LOGI("desk_panels: parked canonical applied at desk resolve");
        else
            UE_LOGW("desk_panels: parked canonical not written at desk resolve (the desk refused it)");
        return;
    }
    SendLocalEdits();
}

void OnDeskPanel(const coop::net::DeskPanelStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const bool isHost = (s->role() == coop::net::Role::Host);

    if (p.op == op::kCanonical) {  // host-authored
        if (isHost) return;  // the host is the canonical source
        if (senderSlot != 0) {
            UE_LOGW("desk_panels: canonical from non-host slot=%u -- dropping", senderSlot);
            return;
        }
        const DP::Masks m{static_cast<uint8_t>(p.onMask & kAllPanels), static_cast<uint8_t>(p.unscrewedMask & kAllPanels)};
        if (!DP::EnsureResolved() || !CD::Instance()) {
            g_pendingCanon = m;
            g_havePendingCanon = true;
            UE_LOGI("desk_panels: canonical parked (desk unresolved)");
            return;
        }
        if (AdoptCanonical(m))
            UE_LOGI("desk_panels: canonical on=0x%X unscrewed=0x%X (n=%llu written)", m.on, m.unscrewed,
                    static_cast<unsigned long long>(g_adopted));
        return;
    }
    if (p.op == op::kDeny) {  // host-authored, to the refused take-off's author
        if (isHost || senderSlot != 0) return;
        if (p.panel < DP::kPanels) ClientHandleDeny(p.panel);
        return;
    }
    if (p.op != op::kSetOn && p.op != op::kSetUnscrewed) return;

    // An edit. HOST-terminal.
    if (!isHost) {
        UE_LOGW("desk_panels: op=%u reached a client -- protocol violation, dropping", p.op);
        return;
    }
    if (senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return;
    DP::Masks live{};
    if (!DP::EnsureResolved() || !CD::Instance() || !DP::Read(live)) {
        UE_LOGW("desk_panels: host op=%u panel=%u declined (desk unresolved)", p.op, p.panel);
        return;
    }
    // The desk's own rules, as its Blueprint holds them: the fixed panel is never unbolted, only a panel
    // on the desk has bolts to turn, and only an unbolted panel that is on the desk comes off. A panel
    // is put back whatever its bolts. An edit the host's desk does not allow gets the canonical back
    // to its author, and a take-off of a panel already off is also denied by name: another peer's
    // came first, and the author's press left it a panel prop to destroy.
    const uint8_t bit = (p.panel < DP::kPanels) ? static_cast<uint8_t>(1u << p.panel) : 0;
    const bool value = p.value != 0;
    DP::Masks next = live;
    bool allowed = bit != 0 && p.panel != kFixedPanel;
    if (allowed && p.op == op::kSetUnscrewed) {
        allowed = (live.on & bit) != 0;
        next.unscrewed = value ? (live.unscrewed | bit) : (live.unscrewed & static_cast<uint8_t>(~bit));
    } else if (allowed) {
        const bool taken = !value && (live.on & bit) == 0;
        allowed = value || ((live.unscrewed & bit) != 0 && !taken);
        next.on = value ? (live.on | bit) : (live.on & static_cast<uint8_t>(~bit));
        if (taken) {
            RecordDeny(senderSlot, p.panel);
            coop::net::DeskPanelStatePayload deny{};
            deny.op = op::kDeny;
            deny.panel = p.panel;
            s->SendReliableToSlot(senderSlot, coop::net::ReliableKind::DeskPanelState, &deny, sizeof(deny));
        }
    }
    if (!allowed) {
        UE_LOGW("desk_panels: host op=%u panel=%u value=%u from slot %u refused (on=0x%X unscrewed=0x%X) -- "
                "canonical to its author", p.op, p.panel, p.value, senderSlot, live.on, live.unscrewed);
        HostBroadcastCanonical(s, live, senderSlot);
        return;
    }
    if (next != live && !WriteDesk(live, next)) {
        UE_LOGW("desk_panels: host write failed -- op=%u panel=%u not applied", p.op, p.panel);
        return;
    }
    HostBroadcastCanonical(s, next);
    UE_LOGI("desk_panels: host applied op=%u panel=%u value=%u from slot %u -- canonical on=0x%X unscrewed=0x%X",
            p.op, p.panel, p.value, senderSlot, next.on, next.unscrewed);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    DP::Masks live{};
    if (!DP::EnsureResolved() || !CD::Instance() || !DP::Read(live)) return;
    HostBroadcastCanonical(s, live, slot);
    UE_LOGI("desk_panels: canonical -> joiner slot %d", slot);
}

bool HostShouldReapPanelBirth(uint8_t senderSlot, void* panelClass) {
    const int panel = DP::PanelForClass(panelClass);
    if (panel < 0) return false;
    const auto now = Clock::now();
    for (auto& d : g_denies) {
        if (d.sender != senderSlot || d.panel != panel || now >= d.until) continue;
        d = DenyRec{};  // one birth per refused take-off
        UE_LOGW("desk_panels: dropped the panel birth of a refused take-off (slot %u, panel %d)", senderSlot, panel);
        return true;
    }
    return false;
}

uint64_t CanonicalsAdopted() { return g_adopted; }
uint64_t TakeOffsDenied() { return g_denied; }

void OnDisconnect() {
    g_haveKnown = false;
    g_havePendingCanon = false;
    g_nextParkedTry = {};
    for (auto& d : g_denies) d = DenyRec{};
}

}  // namespace coop::desk_panel_sync
