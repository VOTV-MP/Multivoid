// coop/dev/profile_fill_drill.cpp -- see coop/dev/profile_fill_drill.h.

#include "coop/dev/profile_fill_drill.h"

#include "coop/comms/chat_sync.h"
#include "coop/config/config.h"
#include "coop/dev/prop_act_drill.h"   // the act drill's handshake reads: the host's line, OwnJoinOver, NowMs
#include "coop/items/inventory_wire.h"
#include "coop/items/player_inventory_sync.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/player/player_profile_store.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/player_handshake.h"
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::profile_fill_drill {
namespace {

namespace R   = ue_wrap::reflection;
namespace E   = ue_wrap::engine;
namespace PAD = coop::dev::prop_act_drill::detail;
namespace PIS = coop::player_inventory_sync;

constexpr const char* kTag = "[PFILL-DRILL]";
constexpr uint64_t kVolumeBoundMs = 240000;   // the game's volume tables arrive about 2.5 minutes after a world loads
constexpr uint64_t kSendBoundMs   = 20000;    // after the fill: the next profile send
constexpr uint64_t kHostBoundMs   = 30000;    // after that: the host's stored line
constexpr int      kPerTick       = 6;        // own spawns pocketed per tick
constexpr int      kMaxItems      = 3000;     // a bound on a fill that never refuses (volume 0 items)
constexpr float    kLiftCm        = 60.f;

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    return s;
}

void* PlayerInventoryComponent(void* player) {
    static R::InstanceOffset sComp{L"inventoryComponent_player"};
    const int32_t o = player ? sComp.Of(player) : -1;
    void* const comp = o < 0 ? nullptr : *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(player) + o);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

std::string Hex(uint64_t h) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

// ---- HOST: say what the stored profile of slot 1 looks like each time one arrives ------------------------------------

void HostTick() {
    static uint32_t s_seen = 0;
    const uint32_t n = PIS::ProfilesReceived();
    if (n == s_seen) return;
    s_seen = n;
    const std::string& guid = coop::player_handshake::GuidForSlot(1);
    std::vector<uint8_t> blob;
    if (guid.empty() || coop::player_profile_store::Get(guid, blob) != coop::player_profile_store::Found::Yes) return;
    coop::player_profile::Profile p;
    if (!coop::inventory_wire::Deserialize(blob, p)) {
        UE_LOGE("%s host: the stored profile (%zu bytes) does not deserialize", kTag, blob.size());
        return;
    }
    const std::string line = "pfill stored " + Hex(coop::blob_chunks::Fnv64(p.record)) + " r=" +
                             std::to_string(p.record.size()) + " w=" + std::to_string(blob.size());
    UE_LOGI("%s host: %s", kTag, line.c_str());
    coop::chat_sync::QueueSend(line);
}

// ---- CLIENT -----------------------------------------------------------------------------------------------------------

enum class Step { WaitReady, Fill, WaitSend, WaitHost, Done };

struct Candidate { float volume; std::wstring name; };

struct State {
    Step step = Step::WaitReady;
    uint64_t armAt = 0, stepAt = 0;
    std::vector<Candidate> cands;
    size_t cand = 0;
    void* cls = nullptr;
    int taken = 0;
    float volume = 0.f;
    std::wstring item;
    PIS::SendStats base{};      // the last send before the fill: the empty inventory's profile
    PIS::SendStats atFill{};    // the last send when the fill ended
    PIS::SendStats judged{};
    bool forced = false;
};
State g;

void Verdict(bool pass, const std::string& why) {
    UE_LOGI("%s client %s -- %s", kTag, pass ? "PASS" : "FAIL", why.c_str());
    UE_LOGI("%s client DONE", kTag);
    g.step = Step::Done;
}

// The next candidate whose class loads; false when none is left.
bool NextClass() {
    while (g.cand < g.cands.size()) {
        const Candidate& c = g.cands[g.cand];
        const std::wstring path = L"/Game/objects/prop_" + c.name + L".prop_" + c.name + L"_C";
        void* const cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
        if (cls) {
            g.cls = cls;
            g.volume = c.volume;
            g.item = c.name;
            UE_LOGI("%s item '%s' volume %.1f: class loaded", kTag, Narrow(c.name).c_str(), c.volume);
            return true;
        }
        UE_LOGI("%s item '%s' volume %.1f: no class at %s", kTag, Narrow(c.name).c_str(), c.volume,
                Narrow(path).c_str());
        ++g.cand;
    }
    return false;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

void ClientTick(const std::string& want) {
    void* const player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player) || !E::GetController(player)) return;
    const uint64_t now = PAD::NowMs();
    switch (g.step) {
    case Step::Done: return;
    case Step::WaitReady: {
        if (!PAD::OwnJoinOver()) { g.armAt = 0; return; }
        if (!g.armAt) g.armAt = now;
        float v = 0.f; bool known = false;
        if (!(ue_wrap::prop_gather::VolumeOfName(L"crowbar", v, known) && known && v > 0.f)) {
            if (now - g.armAt > kVolumeBoundMs) Verdict(false, "the game's volume tables never arrived");
            return;
        }
        if (PIS::LastSend().count == 0) {   // the profile stream is open once a first profile has gone
            if (now - g.armAt > kVolumeBoundMs) Verdict(false, "no profile was ever sent");
            return;
        }
        g.forced = want != "1";
        std::vector<std::wstring> names;
        if (!ue_wrap::prop_gather::ListNames(names)) { Verdict(false, "the prop name list does not read"); return; }
        int zero = 0, unknown = 0;
        const std::wstring forced = Lower(std::wstring(want.begin(), want.end()));
        for (const std::wstring& n : names) {
            float vol = 0.f; bool valid = false;
            if (!ue_wrap::prop_gather::VolumeOfName(n, vol, valid) || !valid) { ++unknown; continue; }
            if (vol <= 0.f) { ++zero; continue; }
            if (g.forced && Lower(n) != forced) continue;
            g.cands.push_back({vol, n});
        }
        std::stable_sort(g.cands.begin(), g.cands.end(),
                         [](const Candidate& a, const Candidate& b) { return a.volume < b.volume; });
        UE_LOGI("%s %zu names listed, %zu with a volume, %d at volume 0, %d unreadable", kTag, names.size(),
                g.cands.size(), zero, unknown);
        for (size_t i = 0; i < g.cands.size() && i < 12; ++i)
            UE_LOGI("%s   smallest[%zu] '%s' volume %.1f", kTag, i, Narrow(g.cands[i].name).c_str(), g.cands[i].volume);
        if (g.cands.empty()) { Verdict(false, "no item to fill with"); return; }
        g.base = PIS::LastSend();
        UE_LOGI("%s the empty inventory's profile: wire %zu bytes, record %zu", kTag, g.base.wireBytes, g.base.recordBytes);
        if (!NextClass()) { Verdict(false, "no candidate's class loads"); return; }
        g.step = Step::Fill;
        g.stepAt = now;
        return;
    }
    case Step::Fill: {
        void* const comp = PlayerInventoryComponent(player);
        float total = -1.f, max = -1.f;
        if (!comp || !ue_wrap::container_inventory::CurrentVolume(comp, total) ||
            !ue_wrap::container_inventory::MaxVolume(comp, max)) {
            Verdict(false, "the player's inventory volumes do not read");
            return;
        }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(player, at)) { Verdict(false, "the player's place does not read"); return; }
        for (int i = 0; i < kPerTick && g.step == Step::Fill; ++i) {
            void* const own = E::SpawnActor(g.cls, {at.X, at.Y, at.Z + kLiftCm});
            bool taken = false;
            if (!own || !E::CallMainPlayerAddObjectToInventory(player, own, taken)) {
                UE_LOGI("%s '%s' could not be spawned or pocketed; next candidate", kTag, Narrow(g.item).c_str());
                ++g.cand;
                if (g.taken == 0 && NextClass()) continue;
                Verdict(false, "an own spawn could not be spawned or pocketed");
                return;
            }
            if (taken) { ++g.taken; continue; }
            // Refused. With room left for the item the pocket does not take this class: try the next one.
            float t2 = 0.f;
            ue_wrap::container_inventory::CurrentVolume(comp, t2);
            if (g.taken == 0 && t2 + g.volume <= max) {
                UE_LOGI("%s '%s' is refused with room (total %.1f + %.1f <= %.1f); next candidate", kTag,
                        Narrow(g.item).c_str(), t2, g.volume, max);
                ++g.cand;
                if (!NextClass()) { Verdict(false, "no candidate the pocket takes"); return; }
                continue;
            }
            UE_LOGI("%s the inventory is full after %d x '%s' (volume %.1f): totalVolume=%.1f maxVolume=%.1f", kTag,
                    g.taken, Narrow(g.item).c_str(), g.volume, t2, max);
            g.atFill = PIS::LastSend();
            g.step = Step::WaitSend;
            g.stepAt = now;
            return;
        }
        if (g.taken >= kMaxItems) Verdict(false, "the inventory never refused");
        return;
    }
    case Step::WaitSend: {
        const PIS::SendStats s = PIS::LastSend();
        // A send after the fill that carries the items: the record is larger than the empty inventory's.
        if (s.count > g.atFill.count && s.recordBytes > g.base.recordBytes && now - g.stepAt > 3000) {
            g.judged = s;
            UE_LOGI("%s the full inventory's profile was sent: wire %zu bytes, record %zu bytes (empty: wire %zu, record %zu), "
                    "hash %s; %d items -> %.1f record bytes per item", kTag, s.wireBytes, s.recordBytes,
                    g.base.wireBytes, g.base.recordBytes, Hex(s.recordHash).c_str(), g.taken,
                    g.taken ? static_cast<double>(s.recordBytes - g.base.recordBytes) / g.taken : 0.0);
            g.step = Step::WaitHost;
            g.stepAt = now;
            return;
        }
        if (now - g.stepAt > kSendBoundMs)
            Verdict(false, "the full inventory's profile was not sent (see the player_inventory ERROR above): " +
                           std::to_string(g.taken) + " items, last send record " + std::to_string(s.recordBytes) +
                           " bytes, wire " + std::to_string(s.wireBytes));
        return;
    }
    case Step::WaitHost: {
        std::string rest;
        if (PAD::HostSaid("pfill stored " + Hex(g.judged.recordHash), &rest)) {
            Verdict(true, std::to_string(g.taken) + " x '" + Narrow(g.item) + "' (volume " + std::to_string(g.volume) +
                          ") filled the inventory; the profile went as " + std::to_string(g.judged.wireBytes) +
                          " wire bytes (record " + std::to_string(g.judged.recordBytes) + ", ceiling " +
                          std::to_string(coop::blob_chunks::MaxBlobBytes()) + ") and the host stored the same record: " + rest);
            return;
        }
        if (now - g.stepAt > kHostBoundMs)
            Verdict(false, "the host never stored the client's last profile (hash " + Hex(g.judged.recordHash) + ")");
        return;
    }
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const std::string s_val = coop::config::ReadEnv("VOTVCOOP_PROFILE_FILL_DRILL");
    if (s_val.empty() || s_val == "0" || !session) return;   // `0` is how a rig's second life says: no fill, only the join's apply
    if (coop::roster::LocalIsHost()) { HostTick(); return; }
    ClientTick(s_val);
}

}  // namespace coop::dev::profile_fill_drill
