// coop/dev/drive_selftest.cpp -- see drive_selftest.h.

#include "coop/dev/drive_selftest.h"

#include "coop/config/config.h"
#include "coop/element/registry.h"
#include "coop/interactables/signal_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/drive_chain.h"
#include "ue_wrap/engine/engine.h"  // SpawnActor + GetActorLocation (the host-side rack seed)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::dev::drive_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace DC = ue_wrap::drive_chain;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::IsIniKeyTrue("drive_selftest");
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

// The measured rack (first live prop_driveRack found; re-validated each use).
uint32_t g_rackEid = 0;
void* g_rack = nullptr;

bool ResolveRack() {
    if (g_rack) {
        void* a = coop::element::LivePropActor(g_rackEid);
        if (a == g_rack) return true;
        g_rack = nullptr;
        g_rackEid = 0;
    }
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(
        coop::element::ElementType::Prop, pairs);
    for (const auto& p : pairs) {
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx)) continue;
        if (!DC::IsRackClass(R::ClassOf(p.actor))) continue;
        g_rack = p.actor;
        g_rackEid = static_cast<uint32_t>(p.id);
        return true;
    }
    return false;
}

// Digest = Fnv64 over [has byte + serialized row] x16, read directly from the
// engine arrays. Deterministic: no eids, no seqs, no wall-clock anywhere.
bool RackDigest(uint64_t& outDigest, int& outFilled) {
    DC::RackRow rows[DC::kRackSlots];
    int n = 0;
    if (!DC::ReadRack(g_rack, rows, n)) return false;
    std::vector<uint8_t> acc;
    outFilled = 0;
    for (int i = 0; i < DC::kRackSlots; ++i) {
        acc.push_back(rows[i].has ? 1 : 0);
        if (rows[i].has) ++outFilled;
        std::vector<uint8_t> rb = coop::signal_wire::Serialize(rows[i].row, false);
        acc.insert(acc.end(), rb.begin(), rb.end());
    }
    outDigest = coop::blob_chunks::Fnv64(acc);
    return true;
}

void LogDigest(const wchar_t* tag) {
    uint64_t d = 0;
    int filled = 0;
    if (!RackDigest(d, filled)) return;
    UE_LOGI("drive_selftest: DIGEST %ls rack eid=%u d=%016llx filled=%d",
            tag, g_rackEid, static_cast<unsigned long long>(d), filled);
}

// Fixed inject content (comparability: constants, non-zero date so no
// receiver re-stamp path can vary it).
DC::RackRow MakeRow(bool host) {
    DC::RackRow r;
    r.has = true;
    r.row.name = host ? L"DRIVE-SELFTEST-HOST" : L"DRIVE-SELFTEST-CLIENT";
    r.row.id = host ? L"ST-H" : L"ST-C";
    r.row.object = L"selftest";
    r.row.signal = L"selftest";
    r.row.size = 10.f;
    r.row.decoded = 10.f;
    r.row.date = 638000000000000000LL;
    r.row.frequency = host ? 1 : 2;
    r.row.quality = 3;
    return r;
}

int g_stage = 0;
uint64_t g_at = 0;
uint64_t g_nextDigest = 0;

// A fresh save has NO rack (shop item) -- the HOST spawns one once, ~5 s
// after connect, at the desk play slot (exists in every save). The
// host-authored spawn is fanned to the client by host_spawn_watcher; the
// client's ResolveRack then finds the mirror. (Run-1 measurement 2026-07-18:
// "connect seed ... 0 racks" -- the instrument armed against nothing.)
bool g_spawnTried = false;
uint64_t g_spawnAt = 0;

void HostSeedRackIfMissing(bool host, uint64_t now) {
    if (!host || g_spawnTried) return;
    if (!g_spawnAt) { g_spawnAt = now + 5000; return; }
    if (now < g_spawnAt) return;
    g_spawnTried = true;
    void* cls = R::FindClass(L"prop_driveRack_C");
    void* slot = DC::SlotActor(DC::kRoleDeskPlay);
    if (!cls || !slot) {
        UE_LOGW("drive_selftest: rack seed failed (cls=%p slot=%p)", cls, slot);
        return;
    }
    const auto loc = ue_wrap::engine::GetActorLocation(slot);
    void* rack = ue_wrap::engine::SpawnActor(cls, {loc.X + 100.f, loc.Y, loc.Z + 60.f});
    UE_LOGI("drive_selftest: rack %s at desk slot (+100,+0,+60)",
            rack ? "SPAWNED" : "SPAWN FAILED");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (!DC::EnsureResolved()) return;
    const bool isHost = s->role() == coop::net::Role::Host;
    if (!ResolveRack()) {
        HostSeedRackIfMissing(isHost, NowMs());
        return;
    }

    const uint64_t now = NowMs();
    if (now >= g_nextDigest) {
        g_nextDigest = now + 5000;
        LogDigest(L"tick");
    }

    const bool host = isHost;
    const int idx = host ? 0 : 1;
    // Host circle at +10 s (remove +18 s); client circle at +25 s (remove +33 s)
    // -- sequential phases, one injection per circle (op-count==1 assert).
    switch (g_stage) {
        case 0:
            g_at = now + (host ? 10000 : 25000);
            g_stage = 1;
            break;
        case 1: {
            if (now < g_at) break;
            // Native-shaped mutation: raw row write + gen, NO prime, outside
            // any wire guard -- the lane's own machinery (host organic detect /
            // client op derivation) must ship it or the digests diverge.
            DC::RackRow r = MakeRow(host);
            if (!DC::WriteRackRow(g_rack, idx, r)) break;
            DC::CallRackGen(g_rack);
            LogDigest(L"inject");
            g_at = now + 8000;
            g_stage = 2;
            break;
        }
        case 2: {
            if (now < g_at) break;
            DC::RackRow empty;  // has=false
            if (!DC::WriteRackRow(g_rack, idx, empty)) break;
            DC::CallRackGen(g_rack);
            LogDigest(L"remove");
            g_stage = 3;
            break;
        }
        default: break;  // done; the 5 s tick digests keep logging
    }
}

}  // namespace coop::dev::drive_selftest
