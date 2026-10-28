// coop/dev/hostile_drill_walk.cpp -- see coop/dev/hostile_drill_walk.h.

#include "coop/dev/hostile_drill_walk.h"

#include "coop/comms/chat_log.h"
#include "coop/element/registry.h"
#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_save_data.h"  // Covers -- the host's own apply gate, so the arm picks a prop it applies
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/walk_census.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::hostile_drill::walk {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace SW = coop::save_record_wire;
using Clock = std::chrono::steady_clock;

// record_bad_classes: the number of distinct unknown class names the crafted record carries, one per
// `classes` group so the host's base-splice (which overwrites only the few groups the prop's own
// record wrote) leaves almost all of them to reach WriteClassField. Sized to keep the body under
// prop_save_data's 32768 B receive cap (about 18 B of wire per 5-char name here).
constexpr int      kBadNames        = 1024;
constexpr uint32_t kWalkSentinelEid = 65528u;   // top of the peer range, clear of any run's ids
constexpr const char* kSentinelClass = "prop_garbageClump_C";  // a real class so the spawn binds
// bad_alarm: a burst of AlarmState. A genuine red (an unbound trigger) walks once per message; a
// bound trigger walks at most once in total (the first, before it binds) and the rest of the delta
// is only the window's ambient finder walks (a handful). The burst is large so a real red sits far
// above that ambient floor, and the red floor (half the burst) is well clear of it: a bound-trigger
// world -- which a fresh save is -- reads INCONCLUSIVE, never a false FAIL, and the alarm fix is then
// proven by code (record_bad_classes carries the HIGH).
constexpr int      kAlarmBurst      = 64;
// bad_prop_key: PropDestroys naming keys no prop holds, eid 0, so the host's only route is the key; on
// the bytes with the scan behind the key index each pays one whole-array walk.
constexpr int      kKeyBurst        = 64;
// bad_prop_spawn: keyed spawns of a loaded class away from every instance of it. On the bytes with the
// scans each pays two walks, the exact-key dedupe's and the fuzzy dedupe's; the host spawns a real prop
// for each on every bytes, the arm's known cost, so the burst is small.
constexpr int      kSpawnBurst      = 16;
constexpr uint32_t kSpawnEidBase    = 65500u;   // 65500..65515, below the sentinel, clear of any run's ids
constexpr const char* kSpawnClass   = "prop_C";

// record_bad_classes is proven by a large delta (~kBadNames). A clean green is zero plus the
// sentinel spawn's own one class resolution, so anything past a handful is the walk this arc closes.
constexpr unsigned long long kRecordRedFloor = 64;
// bad_alarm: half the burst (see above).
constexpr unsigned long long kAlarmRedFloor  = static_cast<unsigned long long>(kAlarmBurst) / 2;
// The two prop arms: half the walks their red would pay, well above a window's ambient finder walks.
constexpr unsigned long long kKeyRedFloor    = static_cast<unsigned long long>(kKeyBurst) / 2;
constexpr unsigned long long kSpawnRedFloor  = static_cast<unsigned long long>(kSpawnBurst);

constexpr auto kFailBound = std::chrono::seconds(180);  // a failure bound, never a wait

// bad_alarm's chat sentinel is a FIXED string: the host judges it, and a per-peer stamp (minted on
// each side's own clock) would never match across the two. A line the host's record kept from a
// prior run is excluded by the lineSeq baseline the host takes at arm, not by a stamp.
constexpr const char* kAlarmSentinel = "hostile-drill bad_alarm sentinel";

// ---- client (sender) ----
enum class Send { Idle, Sent, Done };
Send              g_send = Send::Idle;

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool SendChatLine(coop::net::Session* s, const std::string& text) {
    coop::net::ChatMessagePayload line{};
    const size_t n = text.size() < sizeof(line.text) ? text.size() : sizeof(line.text);
    std::memcpy(line.text, text.data(), n);
    line.len = static_cast<uint8_t>(n);
    return s->SendReliable(coop::net::ReliableKind::ChatMessage, &line, sizeof(line));
}

// A live covered prop's Key, discovered from this peer's own tracked keys. `Covers` is the SAME gate the
// host's record apply runs -- a class that declares its own getData below Aprop_C AND that no dedicated
// lane has claimed -- so a prop that passes it here is one the host will actually apply the record to
// (OverridesGetData alone is not enough: the ATV container overrides getData but a dedicated lane owns
// it, and the host drops its record unapplied). The tracked keys mirror the host's world, so a key
// covered here is covered on the host too. Empty when the loaded world carries no such prop -- the arm
// then reports it could not stage, never a false pass.
bool FindCoveredPropKey(std::wstring& outKey) {
    std::vector<coop::prop_element_tracker::TrackedKeyEntry> entries;
    coop::prop_element_tracker::CollectTrackedKeyEntries(entries);
    for (const auto& e : entries) {
        if (!e.actor || e.key.empty()) continue;
        if (!R::IsLiveByIndex(e.actor, e.internalIdx)) continue;
        if (coop::prop_save_data::Covers(e.actor)) { outKey = e.key; return true; }
    }
    return false;
}

// The PropSaveDataIntent body, in prop_save_data's own grammar: version, key length, the ASCII key,
// then the serialized record. The record carries only its `classes` group, kBadNames distinct unknown
// names, one per group.
std::vector<uint8_t> BuildBadRecordBody(const std::wstring& key) {
    SR::SaveRecord rec;
    rec.classes.reserve(static_cast<size_t>(kBadNames));
    for (int i = 0; i < kBadNames; ++i) {
        wchar_t name[8];
        std::swprintf(name, 8, L"a%04d", i);  // "a0000".."a1023": distinct, 5 chars, no such class
        rec.classes.push_back({std::wstring(name)});
    }
    std::vector<uint8_t> body;
    body.push_back(1);  // prop_save_data kBodyVersion
    body.push_back(static_cast<uint8_t>(key.size()));
    for (wchar_t c : key) body.push_back(static_cast<uint8_t>(c & 0x7F));
    SW::SerSave(body, rec);
    return body;
}

void PutClassName(coop::net::WireClassName& w, const char* cls) {
    const size_t n = std::strlen(cls);
    std::memcpy(w.data, cls, n);
    w.len = static_cast<uint8_t>(n);
}

// Say the arm could not stage its red precondition: not a pass and not a fail, so the rig's markers do
// not match and the run ends on its own bound, read as "this world could not stage it".
void Inconclusive(Arm a, const char* why) {
    UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: %s", ArmName(a), why);
    g_send = Send::Done;
}

void SendRecordBadClasses(coop::net::Session* s) {
    std::wstring key;
    if (!FindCoveredPropKey(key)) {
        Inconclusive(Arm::RecordBadClasses, "no live covered prop in this world to address");
        return;
    }
    ue_wrap::FVector at{};
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn || !ue_wrap::engine::TryGetActorLocation(pawn, at)) return;  // no body yet; wait
    const std::vector<uint8_t> body = BuildBadRecordBody(key);
    if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::PropSaveDataIntent, 0x7D000001u, body)) {
        Inconclusive(Arm::RecordBadClasses, "the crafted intent was not sent (link refused it)");
        return;
    }
    // One honest trash spawn behind the intent on the same Default FIFO: its binding on the host means the
    // intent before it was drained and applied, so the walk count is final when the judge reads it.
    coop::net::PropSpawnPayload p{};
    PutClassName(p.className, kSentinelClass);
    p.locX = at.X + 120.f;
    p.locY = at.Y;
    p.locZ = at.Z;
    p.scaleX = p.scaleY = p.scaleZ = 1.f;
    p.elementId = kWalkSentinelEid;
    if (!s->SendPropSpawn(p)) {
        Inconclusive(Arm::RecordBadClasses, "the sentinel spawn was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] record_bad_classes sent the intent (key '%ls', %d names) + sentinel "
            "spawn (eid %u)", key.c_str(), kBadNames, kWalkSentinelEid);
    g_send = Send::Sent;
}

void SendBadAlarm(coop::net::Session* s) {
    coop::net::AlarmStatePayload a{};
    for (int i = 0; i < kAlarmBurst; ++i) {
        a.active = static_cast<uint8_t>(i & 1);  // toggle, so each message is a real transition request
        if (!s->SendReliable(coop::net::ReliableKind::AlarmState, &a, sizeof(a))) {
            Inconclusive(Arm::BadAlarm, "the AlarmState burst was not sent");
            return;
        }
    }
    if (!SendChatLine(s, kAlarmSentinel)) {
        Inconclusive(Arm::BadAlarm, "the sentinel was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] bad_alarm sent %d AlarmState + sentinel", kAlarmBurst);
    g_send = Send::Sent;
}

void PutKey(coop::net::WireKey& w, const char* key) {
    const size_t n = std::strlen(key);
    std::memcpy(w.data, key, n);
    w.len = static_cast<uint8_t>(n);
}

// The Default-lane sentinel behind a burst on the same lane: its binding on the host means every message before
// it was drained and applied, so the walk count is final when the judge reads it.
bool SendSentinelSpawn(coop::net::Session* s, const ue_wrap::FVector& at) {
    coop::net::PropSpawnPayload p{};
    PutClassName(p.className, kSentinelClass);
    p.locX = at.X + 120.f;
    p.locY = at.Y;
    p.locZ = at.Z;
    p.scaleX = p.scaleY = p.scaleZ = 1.f;
    p.elementId = kWalkSentinelEid;
    return s->SendPropSpawn(p);
}

void SendBadPropKey(coop::net::Session* s, Arm a) {
    ue_wrap::FVector at{};
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn || !ue_wrap::engine::TryGetActorLocation(pawn, at)) return;  // no body yet; wait
    for (int i = 0; i < kKeyBurst; ++i) {
        coop::net::PropDestroyPayload d{};
        char key[24];
        std::snprintf(key, sizeof(key), "nokey_%04d", i);
        PutKey(d.key, key);
        d.elementId = 0;
        if (!s->SendPropDestroy(d)) { Inconclusive(a, "the PropDestroy burst was not sent"); return; }
    }
    if (!SendSentinelSpawn(s, at)) { Inconclusive(a, "the sentinel spawn was not sent"); return; }
    UE_LOGI("[HOSTILE-DRILL] bad_prop_key sent %d PropDestroy + sentinel spawn (eid %u)", kKeyBurst,
            kWalkSentinelEid);
    g_send = Send::Sent;
}

void SendBadPropSpawn(coop::net::Session* s, Arm a) {
    ue_wrap::FVector at{};
    void* pawn = coop::players::Registry::Get().Local();
    if (!pawn || !ue_wrap::engine::TryGetActorLocation(pawn, at)) return;  // no body yet; wait
    for (int i = 0; i < kSpawnBurst; ++i) {
        coop::net::PropSpawnPayload p{};
        PutClassName(p.className, kSpawnClass);
        char key[24];
        std::snprintf(key, sizeof(key), "nospawn_%02d", i);
        PutKey(p.key, key);
        // A 4x4 lattice of spots 3 m apart and 2 m up, each well clear of the others and of any prop at
        // rest; a prop within the dedupe's radius of one would be adopted, which the verdict ignores.
        p.locX = at.X + 300.f * static_cast<float>((i % 4) - 1.5f);
        p.locY = at.Y + 300.f * static_cast<float>((i / 4) - 1.5f);
        p.locZ = at.Z + 200.f;
        p.scaleX = p.scaleY = p.scaleZ = 1.f;
        p.elementId = kSpawnEidBase + static_cast<uint32_t>(i);
        if (!s->SendPropSpawn(p)) { Inconclusive(a, "the PropSpawn burst was not sent"); return; }
    }
    if (!SendSentinelSpawn(s, at)) { Inconclusive(a, "the sentinel spawn was not sent"); return; }
    UE_LOGI("[HOSTILE-DRILL] bad_prop_spawn sent %d keyed %s spawns + sentinel spawn (eid %u)", kSpawnBurst,
            kSpawnClass, kWalkSentinelEid);
    g_send = Send::Sent;
}

// ---- host (judge) ----
enum class Step { WaitArm, Judge, Done };
Step                 g_step = Step::WaitArm;
int                  g_slot = -1;
unsigned long long   g_walkBase = 0;
uint32_t             g_lineSeqBase = 0;  // bad_alarm: the last chat line committed at arm
Clock::time_point    g_armAt{};

void Done(const char* marker, Arm a, const char* detailFmt, unsigned long long n) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), detailFmt, n);
    UE_LOGI("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), marker, buf);
    g_step = Step::Done;
}

uint32_t LastLineSeq() {
    uint32_t last = 0;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) { if (r.lineSeq > last) last = r.lineSeq; });
    return last;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    // An id already bound would make the record arm's verdict about an earlier run's element.
    if (a != Arm::BadAlarm && coop::element::Registry::Get().Get(kWalkSentinelEid)) {
        UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: the sentinel eid was in use at arm", ArmName(a));
        g_step = Step::Done;
        return;
    }
    g_walkBase = ue_wrap::walk_census::ArrayWalkCountTotal();
    if (a == Arm::BadAlarm) g_lineSeqBase = LastLineSeq();
    g_armAt = Clock::now();
    g_step = Step::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (whole-array walks baseline %llu)", ArmName(a), g_slot,
            g_walkBase);
}

bool SentinelLanded(Arm a) {
    if (a != Arm::BadAlarm) return coop::element::Registry::Get().Get(kWalkSentinelEid) != nullptr;
    bool seen = false;
    const std::string want = kAlarmSentinel;
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (r.lineSeq > g_lineSeqBase && r.slot == static_cast<uint8_t>(g_slot) && r.text == want)
            seen = true;
    });
    return seen;
}

void JudgeHost(coop::net::Session* s, Arm a) {
    if (!s->HasPeerConn(g_slot)) { Done("INCONCLUSIVE", a, "the judged slot left before the verdict (%llu)", 0); return; }
    if (!SentinelLanded(a)) {
        if (Clock::now() - g_armAt > kFailBound)
            Done("INCONCLUSIVE", a, "the sentinel never arrived (%llu)", 0);
        return;
    }
    const unsigned long long delta = ue_wrap::walk_census::ArrayWalkCountTotal() - g_walkBase;
    const unsigned long long floor = a == Arm::RecordBadClasses ? kRecordRedFloor
                                   : a == Arm::BadPropKey       ? kKeyRedFloor
                                   : a == Arm::BadPropSpawn     ? kSpawnRedFloor
                                                                : kAlarmRedFloor;
    if (delta >= floor)
        Done("FAIL", a, "walked the object array %llu time(s) on the sender's names", delta);
    else if (a != Arm::BadAlarm)
        Done("PASS", a, "no counted whole-array walk on the sender's names (%llu)", delta);
    else
        // bad_alarm green is ambiguous (a bound trigger never walks either way), so it is never a PASS.
        Done("INCONCLUSIVE", a, "no walk observed (%llu); the trigger was bound -- alarm proven by code",
             delta);
}

}  // namespace

const char* ArmName(Arm a) {
    switch (a) {
    case Arm::RecordBadClasses: return "record_bad_classes";
    case Arm::BadAlarm:         return "bad_alarm";
    case Arm::BadPropKey:       return "bad_prop_key";
    case Arm::BadPropSpawn:     return "bad_prop_spawn";
    default:                    return "off";
    }
}

void TickClient(coop::net::Session* s, Arm a) {
    if (g_send != Send::Idle || !OwnJoinOver()) return;
    if (a == Arm::RecordBadClasses) SendRecordBadClasses(s);
    else if (a == Arm::BadAlarm)    SendBadAlarm(s);
    else if (a == Arm::BadPropKey)  SendBadPropKey(s, a);
    else if (a == Arm::BadPropSpawn) SendBadPropSpawn(s, a);
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm)    ArmHost(s, a);
    else if (g_step == Step::Judge) JudgeHost(s, a);
}

void OnDisconnect() {
    g_send = Send::Idle;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_walkBase = 0;
    g_lineSeqBase = 0;
    g_armAt = {};
}

}  // namespace coop::dev::hostile_drill::walk
