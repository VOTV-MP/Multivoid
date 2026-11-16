// coop/creatures/kerfur_dress_sync.cpp -- see coop/creatures/kerfur_dress_sync.h.

#include "coop/creatures/kerfur_dress_sync.h"

#include "coop/creatures/npc_sync.h"
#include "coop/element/intent_authority.h"
#include "coop/element/mirror_manager.h"
#include "coop/element/npc.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_reach.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/actors/kerfur_dress.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::kerfur_dress_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace K  = ue_wrap::kerfur;
namespace KD = ue_wrap::kerfur_dress;
namespace sg = ue_wrap::script_gate;
using Payload = N::KerfurDressPayload;
using coop::element::ElementId;

constexpr uint64_t kReadMs = 500;        // the host reads its Omegas' outfits twice a second
constexpr int kMaxIntentsPerSecond = 4;  // a sender's asks past this in one second are dropped
constexpr size_t kMaxRows = 16;          // an outfit longer than this is not carried

UE_FACT_FN(kRowEquip, "kerfur_dress_sync", L"kerfurOmega_C", L"equipItem", Declared);
UE_FACT_FN(kRowUnequip, "kerfur_dress_sync", L"kerfurOmega_C", L"unequipItem", Declared);

enum Row { kEquip, kUnequip, kRowCount };
const ue_wrap::facts::Function* const kRows[kRowCount] = {&kRowEquip, &kRowUnequip};
constexpr int kTagBase = 0x4B445200;   // 'KDR', then the row

std::atomic<N::Session*> g_session{nullptr};
uint32_t g_registered = 0;
int g_applying = 0;   // inside our own apply: no watch reads it as a player's act
R::ParamOffset g_unequipName{L"inputPin"};

uint64_t Now() { return ::GetTickCount64(); }

N::Session* Live() {
    N::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->running() ? s : nullptr;
}

// The wire's id of an Omega on this copy: the host's element id (a client's mirror carries it).
ElementId EidOf(void* actor) {
    if (!actor) return coop::element::kInvalidId;
    std::vector<coop::element::Npc*> elems;
    coop::element::MirrorManager<coop::element::Npc>::Instance().Snapshot(elems);
    for (coop::element::Npc* el : elems)
        if (el && el->GetActor() == actor) return el->GetId();
    return coop::npc_sync::GetNpcIdForActor(actor);
}

void* ActorOf(uint32_t eid) {
    auto* el = coop::element::MirrorManager<coop::element::Npc>::Instance().Get(static_cast<ElementId>(eid));
    void* const actor = el ? el->GetActor() : nullptr;
    return actor && R::IsLiveByIndex(actor, el->GetInternalIdx()) && K::IsKerfurActor(actor) ? actor : nullptr;
}

bool PackNames(Payload& p, const std::vector<std::wstring>& names) {
    size_t at = 0;
    for (const std::wstring& n : names) {
        if (n.empty() || at + n.size() + 1 > sizeof(p.names)) return false;
        for (wchar_t c : n) {
            if (c < 0x20 || c > 0x7e) return false;
            p.names[at++] = static_cast<char>(c);
        }
        p.names[at++] = 0;
    }
    p.count = static_cast<uint8_t>(names.size());
    return true;
}

bool UnpackNames(const Payload& p, std::vector<std::wstring>& out) {
    out.clear();
    size_t at = 0;
    for (uint8_t i = 0; i < p.count; ++i) {
        const size_t len = strnlen(p.names + at, sizeof(p.names) - at);
        if (len == 0 || at + len >= sizeof(p.names)) return false;
        std::wstring n;
        for (size_t k = 0; k < len; ++k) {
            const char c = p.names[at + k];
            if (c < 0x20 || c > 0x7e) return false;
            n.push_back(static_cast<wchar_t>(c));
        }
        out.push_back(n);
        at += len + 1;
    }
    return true;
}

bool Fill(Payload& p, uint32_t eid, uint8_t op, const std::vector<std::wstring>& names) {
    p = Payload{};
    p.elementId = eid;
    p.op = op;
    return PackNames(p, names);
}

// ---- HOST ----

std::unordered_map<uint32_t, std::vector<std::wstring>> g_sent;
uint64_t g_nextRead = 0;

void SendOutfit(N::Session& s, void* actor, uint32_t eid, int slot) {
    std::vector<std::wstring> drip;
    if (!KD::ReadDrip(actor, drip) || drip.size() > kMaxRows) return;
    Payload p;
    if (!Fill(p, eid, N::kKerfurDressOutfit, drip)) return;
    if (slot >= 0) {
        s.SendReliableToSlot(slot, N::ReliableKind::KerfurDressState, &p, sizeof(p));
        return;
    }
    s.SendReliable(N::ReliableKind::KerfurDressState, &p, sizeof(p));
    g_sent[eid] = drip;
}

void HostRead(N::Session& s) {
    if (Now() < g_nextRead) return;
    g_nextRead = Now() + kReadMs;
    for (void* k : K::AllKerfurs()) {
        const ElementId eid = EidOf(k);
        if (eid == coop::element::kInvalidId) continue;
        std::vector<std::wstring> drip;
        if (!KD::ReadDrip(k, drip)) continue;
        const auto it = g_sent.find(eid);
        if (it == g_sent.end() ? drip.empty() : it->second == drip) continue;
        SendOutfit(s, k, eid, -1);
        UE_LOGI("kerfur_dress: HOST Omega eid=%u wears %zu item(s) -- sent", eid, drip.size());
    }
}

struct Rate {
    uint64_t second = 0;
    int n = 0;
};
Rate g_rate[N::kMaxPeers];

void Judge(N::Session& s, uint8_t slot, const Payload& p) {
    void* const actor = ActorOf(p.elementId);
    if (!actor || !s.IsSlotWorldReady(slot)) {
        UE_LOGI("kerfur_dress: slot %u's ask names no Omega here (eid=%u)", static_cast<unsigned>(slot), p.elementId);
        return;
    }
    if (p.op == N::kKerfurDressPat) {
        // A pat is shown, not judged: the host's copy plays it and every other client sees it.
        const bool played = KD::PlayPat(actor);
        UE_LOGI("kerfur_dress: slot %u patted Omega eid=%u -- shown here (montage=%d) and sent on",
                static_cast<unsigned>(slot), p.elementId, played ? 1 : 0);
        Payload out = p;
        s.SendReliable(N::ReliableKind::KerfurDressState, &out, sizeof(out), slot);
        return;
    }
    std::vector<std::wstring> names;
    const char* why = nullptr;
    if (!UnpackNames(p, names) || names.size() != 1) why = "malformed";
    if (!why && coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) why = "dead";
    if (!why) {
        const auto subject = coop::element::IntentTarget::ForClientIntent(s, slot, coop::props::container_reach::kReachUU)
                                 .Authorize(actor);
        if (!subject) why = coop::element::OutcomeName(subject.outcome);
    }
    std::vector<std::wstring> drip;
    if (!why && !KD::ReadDrip(actor, drip)) why = "the outfit did not read";
    const bool worn = !why && std::find(drip.begin(), drip.end(), names[0]) != drip.end();
    if (!why && p.op == N::kKerfurDressEquip && worn) why = "already worn";
    if (!why && p.op == N::kKerfurDressEquip && drip.size() >= kMaxRows) why = "full";
    if (!why && p.op == N::kKerfurDressUnequip && !worn) why = "not worn";
    if (why) {
        UE_LOGI("kerfur_dress: slot %u's %s of '%ls' refused: %s", static_cast<unsigned>(slot),
                p.op == N::kKerfurDressEquip ? "put-on" : "take-off", names.empty() ? L"" : names[0].c_str(), why);
        SendOutfit(s, actor, p.elementId, slot);
        return;
    }
    ++g_applying;
    bool ok;
    if (p.op == N::kKerfurDressEquip) {
        drip.push_back(names[0]);
        ok = KD::ApplyDrip(actor, drip);
        KD::Meow(actor);
    } else {
        ok = KD::Unequip(actor, names[0]);
    }
    --g_applying;
    UE_LOGI("kerfur_dress: slot %u %s '%ls' on Omega eid=%u (ok=%d)", static_cast<unsigned>(slot),
            p.op == N::kKerfurDressEquip ? "put on" : "took off", names[0].c_str(), p.elementId, ok ? 1 : 0);
    g_nextRead = 0;   // the change goes out on this tick's read
}

// ---- CLIENT: the watches ----

void* g_equipKerfur = nullptr;
std::vector<std::wstring> g_equipBefore;

void SendIntent(N::Session& s, void* kerfur, uint8_t op, const std::wstring& name) {
    const ElementId eid = EidOf(kerfur);
    if (eid == coop::element::kInvalidId) return;
    Payload p;
    std::vector<std::wstring> names;
    if (!name.empty()) names.push_back(name);
    if (!Fill(p, eid, op, names)) return;
    s.SendReliable(N::ReliableKind::KerfurDressIntent, &p, sizeof(p));
    UE_LOGI("kerfur_dress: this player's %s '%ls' on Omega eid=%u -- asked of the host",
            op == N::kKerfurDressEquip ? "put-on" : (op == N::kKerfurDressUnequip ? "take-off" : "pat"), name.c_str(), eid);
}

sg::Verdict OnPre(const sg::Call& call) {
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount || g_applying || !coop::spawn_authority::IsActiveClientSession())
        return sg::Verdict::Run;
    N::Session* const s = Live();
    if (!s) return sg::Verdict::Run;
    if (row == kEquip) {
        g_equipKerfur = call.object;
        KD::ReadDrip(call.object, g_equipBefore);
        return sg::Verdict::Run;
    }
    // A take-off spawns the item: the host's copy does it, and its outfit comes back.
    const int32_t off = g_unequipName.Of(call.function);
    const std::wstring name = off >= 0 && call.locals ? ue_wrap::field_io::ReadFStringAt(call.locals, off) : std::wstring();
    if (!name.empty()) SendIntent(*s, call.object, N::kKerfurDressUnequip, name);
    return sg::Verdict::Cancel;
}

void OnPost(const sg::Call& call) {
    if (call.tag - kTagBase != kEquip || call.object != g_equipKerfur) return;
    g_equipKerfur = nullptr;
    N::Session* const s = Live();
    if (!s || s->role() != N::Role::Client) return;
    std::vector<std::wstring> after;
    if (!KD::ReadDrip(call.object, after)) return;
    for (const std::wstring& n : after)
        if (std::find(g_equipBefore.begin(), g_equipBefore.end(), n) == g_equipBefore.end())
            SendIntent(*s, call.object, N::kKerfurDressEquip, n);
}

void Register() {
    for (int i = 0; i < kRowCount; ++i) {
        if (g_registered & (1u << i)) continue;
        if (sg::WatchName(*kRows[i], kTagBase + i, &OnPre, i == kEquip ? &OnPost : nullptr)) g_registered |= 1u << i;
    }
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* s = Live();
    if (!s) return;
    if (g_registered != (1u << kRowCount) - 1) {
        static uint64_t nextTry = 0;
        if (Now() >= nextTry) {
            nextTry = Now() + 1000;
            Register();
            if (g_registered == (1u << kRowCount) - 1) UE_LOGI("kerfur_dress: the Omega's equip watches are live");
        }
    }
    if (s->role() == N::Role::Host) HostRead(*s);
}

void OnLocalPat(void* kerfur) {
    N::Session* s = Live();
    if (!s || !s->connected() || g_applying) return;
    if (s->role() == N::Role::Client) {
        SendIntent(*s, kerfur, N::kKerfurDressPat, std::wstring());
        return;
    }
    const ElementId eid = EidOf(kerfur);
    if (eid == coop::element::kInvalidId) return;
    Payload p;
    Fill(p, eid, N::kKerfurDressPat, {});
    s->SendReliable(N::ReliableKind::KerfurDressState, &p, sizeof(p));
}

void OnState(const Payload& p) {
    void* const actor = ActorOf(p.elementId);
    if (!actor) return;
    ++g_applying;
    if (p.op == N::kKerfurDressPat) {
        const bool played = KD::PlayPat(actor);
        UE_LOGI("kerfur_dress: CLIENT Omega eid=%u patted by another player (montage=%d)", p.elementId, played ? 1 : 0);
    } else if (p.op == N::kKerfurDressOutfit) {
        std::vector<std::wstring> want, have;
        if (UnpackNames(p, want) && (!KD::ReadDrip(actor, have) || have != want)) {
            const bool ok = KD::ApplyDrip(actor, want);
            UE_LOGI("kerfur_dress: CLIENT Omega eid=%u wears the host's %zu item(s) (ok=%d)", p.elementId, want.size(),
                    ok ? 1 : 0);
        }
    }
    --g_applying;
}

void OnIntent(int senderSlot, const Payload& p) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host || senderSlot <= 0 || senderSlot >= N::kMaxPeers) return;
    Rate& r = g_rate[senderSlot];
    const uint64_t sec = Now() / 1000;
    if (r.second != sec) r = Rate{sec, 0};
    if (++r.n > kMaxIntentsPerSecond) return;
    Judge(*s, static_cast<uint8_t>(senderSlot), p);
}

void SendJoin(int slot) {
    N::Session* s = Live();
    if (!s || s->role() != N::Role::Host) return;
    for (void* k : K::AllKerfurs()) {
        const ElementId eid = EidOf(k);
        if (eid != coop::element::kInvalidId) SendOutfit(*s, k, eid, slot);
    }
}

void OnDisconnect() {
    g_sent.clear();
    for (Rate& r : g_rate) r = Rate{};
    g_equipKerfur = nullptr;
    g_nextRead = 0;
}

}  // namespace coop::kerfur_dress_sync
