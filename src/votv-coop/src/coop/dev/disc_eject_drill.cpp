// coop/dev/disc_eject_drill.cpp -- see coop/dev/disc_eject_drill.h.

#include "coop/dev/disc_eject_drill.h"

#include "coop/config/config.h"
#include "coop/dev/drill_chat.h"
#include "coop/gamefix/disc_eject_fix.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/engine/engine.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::dev::disc_eject_drill {
namespace {

namespace DC = coop::dev::drill_chat;
namespace FD = ue_wrap::floppy_disc;
namespace FS = ue_wrap::floppy_slot;
namespace L  = ue_wrap::laptop;
namespace R  = ue_wrap::reflection;

enum class Arm { Off, Host, Client, Zip, NoFix };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::disc_eject_drill);
        return v == "host" ? Arm::Host : v == "client" ? Arm::Client : v == "zip" ? Arm::Zip
             : v == "nofix" ? Arm::NoFix : Arm::Off;
    }();
    return a;
}

bool Host() { return coop::roster::LocalIsHost(); }
char Who() { return Host() ? 'H' : 'C'; }
bool LocalActs() { return (ArmOf() == Arm::Client) != Host(); }

const wchar_t* const kMark      = L"gfxdrill";
const wchar_t* const kRowOld    = L"gfxdrill row 0";
const wchar_t* const kRowAdded  = L"gfxdrill added";
constexpr int32_t    kSeedWrites = 20;
const char* const    kSaySeeded = "gfx seeded";
const char* const    kSayDisc   = "gfx disc ";
constexpr uint64_t   kBoundMs   = 15000;

// What a peer's copy of the marked disc holds: rows, writes left, the added row, the dropped row.
struct Seen {
    void* actor = nullptr;
    int   rows = 0;
    int   writes = 0;
    bool  added = false;
    bool  old = false;
};

std::unordered_map<void*, bool> g_isDisc;

bool FindMarked(Seen& out) {
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        auto it = g_isDisc.find(cls);
        if (it == g_isDisc.end()) it = g_isDisc.emplace(cls, FD::IsDiscClass(cls)).first;
        if (!it->second || R::NameStartsWith(R::NameOf(obj), L"Default__") || !R::IsLive(obj)) continue;
        FD::DiscContent c;
        if (!FD::ReadDiscContent(obj, c)) continue;
        Seen s;
        bool marked = false;
        for (const std::wstring& row : c.data) {
            if (row.rfind(kMark, 0) == 0) marked = true;
            if (row == kRowAdded) s.added = true;
            if (row == kRowOld) s.old = true;
        }
        if (!marked) continue;
        s.actor = obj;
        s.rows = static_cast<int>(c.data.size());
        s.writes = c.readWrites;
        out = s;
        return true;
    }
    return false;
}

std::string Describe(bool found, const Seen& s) {
    if (!found) return "none";
    char b[64];
    std::snprintf(b, sizeof(b), "%d %d %d %d", s.rows, s.writes, s.added ? 1 : 0, s.old ? 1 : 0);
    return b;
}

enum class Step { Seed, Stamp, Find, Inserted, Edit, Eject, Ejected, Judge, Theirs };

Step        g_step = Step::Seed;
uint64_t    g_ms = 0;        // when the current step began
uint64_t    g_readyMs = 0;   // when the join was over
bool        g_done = false;
bool        g_seeded = false;
void*       g_disc = nullptr;
std::string g_said;
std::string g_mine;

void Go(Step s) {
    g_step = s;
    g_ms = coop::net::NowMs();
}
uint64_t Since() { return coop::net::NowMs() - g_ms; }

void Verdict(bool pass, const char* fmt, ...) {
    char msg[360];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (pass) UE_LOGI("[DISC-EJECT-DRILL] [%c] PASS %s", Who(), msg);
    else      UE_LOGW("[DISC-EJECT-DRILL] [%c] FAIL %s", Who(), msg);
    g_done = true;
}

bool JoinOver(coop::net::Session& s) {
    if (!Host())
        return coop::net_pump::HasAnnouncedWorldReady() &&
               coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot)
        if (s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
            !coop::join_window_baseline::IsLateWindowOpen(slot))
            return true;
    return false;
}

bool SlotHolds(bool& holds) {
    void* pc = L::Instance();
    FS::Scalars sc{};
    if (!pc || !FS::EnsureResolved(FS::DeviceKind::Laptop) || !FS::ReadScalars(FS::DeviceKind::Laptop, pc, sc))
        return false;
    holds = sc.floppyType >= 0;
    return true;
}

// The host seeds the disc beside its own player, away from the PC's hitboxes, then stamps and publishes it.
void TickSeed(coop::net::Session& s) {
    if (g_step == Step::Seed) {
        bool holds = false;
        if (!L::EnsureResolved() || !SlotHolds(holds)) {
            if (Since() > kBoundMs) Verdict(false, "fixture: the PC or its slot did not resolve");
            return;
        }
        if (holds || L::FloppyBusy()) return Verdict(false, "fixture: the PC's slot must start empty and idle");
        void* cls = R::FindClass(ArmOf() == Arm::Zip ? L"prop_floppyDisc_Wh_C" : L"prop_floppyDisc_R_C");
        void* player = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!cls || !player || !ue_wrap::engine::TryGetActorLocation(player, at))
            return Verdict(false, "fixture: no disc class or no host player (%p %p)", cls, player);
        g_disc = ue_wrap::engine::SpawnActor(cls, {at.X + 70.f, at.Y, at.Z + 20.f});
        if (!g_disc) return Verdict(false, "fixture: the disc did not spawn");
        return Go(Step::Stamp);
    }
    if (Since() < 1500) return;
    if (!R::IsLive(g_disc)) return Verdict(false, "fixture: the seeded disc is gone");
    FD::DiscContent c;
    c.readWrites = kSeedWrites;
    c.data = {kRowOld, L"gfxdrill row 1", L"gfxdrill row 2"};
    const std::wstring key = ue_wrap::prop::GetKeyString(g_disc);
    const bool wrote = FD::WriteDiscContent(g_disc, c);
    const bool published = wrote && coop::prop_save_data::Publish(&s, g_disc, key);
    UE_LOGI("[DISC-EJECT-DRILL] [H] seeded disc key='%ls' with 3 rows and %d writes (write=%d published=%d)",
            key.c_str(), kSeedWrites, wrote ? 1 : 0, published ? 1 : 0);
    if (!wrote) return Verdict(false, "fixture: the seeded disc did not take its rows");
    g_seeded = true;
    DC::Say(kSaySeeded);
    Go(Step::Find);
}

void TickActor() {
    Seen seen;
    bool holds = false;
    switch (g_step) {
    case Step::Find: {
        if (Since() < 2500) return;
        const bool found = FindMarked(seen);
        if (found && seen.rows == 3 && seen.old && seen.writes == kSeedWrites) {
            const bool ok = L::EnsureResolved() && L::CallInsertDisc(seen.actor, ArmOf() == Arm::Zip);
            UE_LOGI("[DISC-EJECT-DRILL] [%c] inserted the marked disc (%d)", Who(), ok ? 1 : 0);
            if (!ok) return Verdict(false, "the PC refused the insert");
            return Go(Step::Inserted);
        }
        if (Since() > kBoundMs)
            Verdict(false, "this peer's copy of the seeded disc never read 3 rows and %d writes (reads %s)",
                    kSeedWrites, Describe(found, seen).c_str());
        return;
    }
    case Step::Inserted:
        if (SlotHolds(holds) && holds && !L::FloppyBusy()) return Go(Step::Edit);
        if (Since() > kBoundMs) Verdict(false, "the slot never held the disc");
        return;
    case Step::Edit: {
        if (Since() < 1500) return;
        L::BufferQuad q;
        if (!L::ReadQuad(q) || q.data.size() != 3 || q.readWrites != kSeedWrites)
            return Verdict(false, "the PC's working rows read %zu rows and %d writes after the insert", q.data.size(),
                           q.readWrites);
        q.data.erase(q.data.begin());
        q.data.push_back(kRowAdded);
        q.readWrites -= 1;
        const bool ok = L::WriteQuadAndRebuild(q);
        UE_LOGI("[DISC-EJECT-DRILL] [%c] edited the working rows: first row dropped, one added, a write spent (%d)",
                Who(), ok ? 1 : 0);
        return Go(Step::Eject);
    }
    case Step::Eject: {
        if (Since() < 2500) return;
        const bool ok = L::CallEjectDisc();
        UE_LOGI("[DISC-EJECT-DRILL] [%c] ejected (%d)", Who(), ok ? 1 : 0);
        if (!ok) return Verdict(false, "the eject call failed");
        return Go(Step::Ejected);
    }
    case Step::Ejected:
        if (SlotHolds(holds) && !holds && !L::FloppyBusy()) return Go(Step::Judge);
        if (Since() > kBoundMs) Verdict(false, "the slot never emptied");
        return;
    case Step::Judge: {
        if (Since() < 2500) return;
        const bool found = FindMarked(seen);
        g_mine = Describe(found, seen);
        const bool good = found && seen.rows == 3 && seen.added && !seen.old && seen.writes == kSeedWrites - 1;
        if (!good)
            return Verdict(false, "the ejected disc reads [%s] here (rows writes added old); [3 %d 1 0] wanted; fix "
                           "applied %llu time(s)", g_mine.c_str(), kSeedWrites - 1,
                           static_cast<unsigned long long>(coop::gamefix::disc_eject_fix::Applied()));
        return Go(Step::Theirs);
    }
    case Step::Theirs: {
        std::string theirs;
        const bool said = DC::SeenRestOf(kSayDisc, &theirs);
        if (said && theirs == g_mine)
            return Verdict(true, "the ejected disc reads [%s] on both peers (rows writes added old); fix applied "
                           "%llu time(s)", g_mine.c_str(),
                           static_cast<unsigned long long>(coop::gamefix::disc_eject_fix::Applied()));
        if (Since() > kBoundMs)
            Verdict(false, "the ejected disc reads [%s] here and [%s] on the other peer", g_mine.c_str(),
                    said ? theirs.c_str() : "nothing said");
        return;
    }
    default:
        return;
    }
}

// The watching peer says what its copy of the marked disc holds whenever that changes.
void TickWatcher() {
    if (!DC::PollDue()) return;
    Seen seen;
    const std::string now = Describe(FindMarked(seen), seen);
    if (now == g_said) return;
    g_said = now;
    DC::Say(std::string(kSayDisc) + now);
    UE_LOGI("[DISC-EJECT-DRILL] [%c] the marked disc reads [%s] here", Who(), now.c_str());
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || g_done || !session->connected()) return;
    DC::SeenHas(kSaySeeded);  // the chat history records from its first read on
    if (!JoinOver(*session)) return;
    if (!g_readyMs) {
        g_readyMs = coop::net::NowMs();
        g_ms = g_readyMs;
        if (ArmOf() == Arm::NoFix) coop::gamefix::disc_eject_fix::SetEnabled(false);
        if (!Host()) g_step = Step::Find;
    }
    if (coop::net::NowMs() - g_readyMs < 3000) return;
    if (Host() && (g_step == Step::Seed || g_step == Step::Stamp)) return TickSeed(*session);
    if (!Host() && !g_seeded) {
        if (!DC::SeenHas(kSaySeeded)) return;
        g_seeded = true;
        Go(Step::Find);
    }
    if (LocalActs()) TickActor();
    else TickWatcher();
}

void OnDisconnect() {
    g_step = Step::Seed;
    g_ms = g_readyMs = 0;
    g_done = false;
    g_seeded = false;
    g_disc = nullptr;
    g_said.clear();
    g_mine.clear();
    g_isDisc.clear();
}

}  // namespace coop::dev::disc_eject_drill
