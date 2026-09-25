// coop/dev/container_opener_probe.cpp -- [dev] container openers on real objects. See the header.
#include "coop/dev/container_opener_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_write_policy.h"
#include "ue_wrap/actors/container_openers.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace coop::dev::container_opener_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
namespace E  = ue_wrap::engine;
using ue_wrap::FVector;

constexpr float kReachUU = coop::props::container_write_policy::kReachUU;

// The openers and their fields, as the game declares them; the probe reads them itself so the reverse
// lookup is checked against an independent read.
struct Kind {
    const wchar_t* cls;
    const wchar_t* field;
};
const Kind kKinds[] = {{L"drone_C", L"container"}, {L"prop_dronesack_C", L"container"}, {L"ATV_C", L"spawnedContainer"}};

// Each kind's classes with live instances, its subclasses included (the ATV has five), found in the index's
// class set and kept until that set changes or a kind's class is not the object they were found from.
struct KindClasses {
    void* kind = nullptr;
    int32_t off = -1;
    std::vector<void*> classes;
};
KindClasses g_kinds[std::size(kKinds)];
uint64_t g_kindsVersion = UINT64_MAX;

void RefreshKinds() {
    const uint64_t v = ue_wrap::object_index::ClassSetVersion();
    void* now[std::size(kKinds)];
    bool stale = v != g_kindsVersion;
    for (size_t i = 0; i < std::size(kKinds); ++i) {
        now[i] = ue_wrap::object_index::ClassByName(kKinds[i].cls);
        if (now[i] != g_kinds[i].kind) stale = true;
    }
    if (!stale) return;
    g_kindsVersion = v;
    for (size_t i = 0; i < std::size(kKinds); ++i) {
        g_kinds[i] = KindClasses{};
        g_kinds[i].kind = now[i];
        if (g_kinds[i].kind) g_kinds[i].off = R::FindPropertyOffset(g_kinds[i].kind, kKinds[i].field);
    }
    ue_wrap::object_index::ForEachClass([](void*, void* cls, void*) {
        for (KindClasses& k : g_kinds)
            if (k.kind && R::IsDescendantOfAny(cls, &k.kind, 1)) {
                k.classes.push_back(cls);
                return;
            }
    }, nullptr);
}

// A client's body, once a pose has placed its puppet: the reach is measured from it.
void* Body(int slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    return rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
}

// The host's own verdict, the one container_write_policy judges a slice by, on the container and on the
// actor that opens it.
void JudgeReach(coop::net::Session& s, int slot, void* container, void* opener) {
    const auto eid = EL::Registry::Get().EidForActor(container);
    const auto tok = EL::IntentTarget::ForClientIntent(s, static_cast<uint8_t>(slot),
                                                       coop::props::container_write_policy::kReachUU);
    const auto atContainer = tok.Resolve(eid, EL::ElementType::Prop);
    const auto atOpener = tok.Authorize(opener);
    UE_LOGI("container_opener_probe: slot %d -- the container eid=%u %s (dist=%.0f allowed=%.0f), its %ls %s "
            "(dist=%.0f allowed=%.0f)", slot, static_cast<unsigned>(eid), EL::OutcomeName(atContainer.outcome),
            atContainer.distUU, atContainer.reachUU, R::ClassNameOf(opener).c_str(),
            EL::OutcomeName(atOpener.outcome), atOpener.distUU, atOpener.reachUU);
}

struct Pass {
    coop::net::Session* session;
    int32_t off;
    int pairs;
    int listed;
};

void OnOpener(void* ctx, void* opener, int32_t index) {
    Pass& p = *static_cast<Pass*>(ctx);
    if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
    void* const container = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(opener) + p.off);
    if (!container) return;
    ++p.pairs;
    struct Find {
        void* want;
        bool found;
    } find{opener, false};
    ue_wrap::container_openers::ForEach(container, [](void* c, void* candidate) {
        Find& f = *static_cast<Find*>(c);
        if (candidate != f.want) return true;
        f.found = true;
        return false;
    }, &find);
    const bool forward = ue_wrap::container_openers::Opens(opener) == container;
    if (find.found && forward) ++p.listed;
    FVector at{}, from{};
    E::TryGetActorLocation(container, at);
    E::TryGetActorLocation(opener, from);
    UE_LOGI("container_opener_probe: %ls at (%.0f, %.0f, %.0f) opens %ls eid=%u at (%.0f, %.0f, %.0f) -- %s by the "
            "reverse lookup, %s by the forward one", R::ClassNameOf(opener).c_str(), from.X, from.Y, from.Z,
            R::ClassNameOf(container).c_str(), static_cast<unsigned>(EL::Registry::Get().EidForActor(container)),
            at.X, at.Y, at.Z, find.found ? "listed" : "MISSED", forward ? "named" : "MISSED");
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (p.session->IsSlotWorldReady(slot) && Body(slot)) JudgeReach(*p.session, slot, container, opener);
}

// The census pass: every opener, the reverse lookup, each client's reach.
void CensusPass(coop::net::Session& session) {
    Pass pass{&session, -1, 0, 0};
    RefreshKinds();
    for (const KindClasses& k : g_kinds) {
        if (k.off < 0) continue;
        pass.off = k.off;
        for (void* cls : k.classes) ue_wrap::object_index::ForEachInstance(cls, &OnOpener, &pass);
    }
    UE_LOGI("container_opener_probe: VERDICT %s (%d of %d openers found by the reverse lookup)",
            pass.pairs > 0 && pass.listed == pass.pairs ? "PASS" : "FAIL", pass.listed, pass.pairs);
}

// The opener a client's body stands at: its reach sphere within the arm of the body.
struct Near {
    FVector body;
    void* opener;
};

void* OpenerAt(void* body) {
    Near n{};
    if (!E::TryGetActorLocation(body, n.body)) return nullptr;
    RefreshKinds();
    for (const KindClasses& k : g_kinds) {
        for (void* cls : k.classes) {
            ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
                Near& x = *static_cast<Near*>(ctx);
                if (x.opener || (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)))
                    return;
                if (!ue_wrap::container_openers::Opens(obj)) return;
                FVector c{};
                float r = 0.f;
                if (!E::ActorReachSphere(obj, c, r)) return;
                const float dx = c.X - x.body.X, dy = c.Y - x.body.Y, dz = c.Z - x.body.Z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kReachUU + r) x.opener = obj;
            }, &n);
            if (n.opener) return n.opener;
        }
    }
    return nullptr;
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::container_opener_probe);
    if (!s_on || !session || session->role() != coop::net::Role::Host) return;
    // A census per client world-ready edge, once the index holds the world, so a container the load brought
    // is among the pairs, and once every ready client has a body to measure from.
    static bool s_ready[coop::net::kMaxPeers] = {};
    static bool s_pending = false;
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        const bool ready = session->IsSlotWorldReady(slot);
        if (ready && !s_ready[slot]) s_pending = true;
        s_ready[slot] = ready;
    }
    if (s_pending && ue_wrap::object_index::Backlog() == 0) {
        bool bodies = true;
        for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
            if (s_ready[slot] && !Body(slot)) bodies = false;
        if (bodies) {
            s_pending = false;
            CensusPass(*session);
        }
    }
    // And the reach again on the tick a client's body comes to stand at an opener, where a slice through
    // it would be judged.
    static void* s_at[coop::net::kMaxPeers] = {};
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        void* const body = s_ready[slot] ? Body(slot) : nullptr;
        void* const at = body ? OpenerAt(body) : nullptr;
        if (at && at != s_at[slot]) {
            UE_LOGI("container_opener_probe: slot %d stands at the %ls", slot, R::ClassNameOf(at).c_str());
            if (void* const container = ue_wrap::container_openers::Opens(at)) JudgeReach(*session, slot, container, at);
        }
        s_at[slot] = at;
    }
}

}  // namespace coop::dev::container_opener_probe
