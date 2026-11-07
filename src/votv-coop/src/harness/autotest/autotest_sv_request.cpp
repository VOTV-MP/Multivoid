// harness/autotest/autotest_sv_request.cpp -- TEST-ONLY: the server information sv.request writes onto a disc, across two peers.
//
// The host puts a disc in the first dish's server box, seeded with VOTVCOOP_SV_REQUEST_SEED rows of the length a real
// one has (31 makes the request's own row the disc's 32nd and last write). One peer, VOTVCOOP_SV_REQUEST_WHO (host
// or client), types the line at its own terminal, ejects the disc and inserts it in the laptop. After each step the
// peer that acted publishes what it reads and the other waits to read the same; the host judges and says
// `rig: VERDICT sv-request PASS|FAIL|INVALID`. Before any of it the host prints the wire size of a full box disc and
// of a full laptop disc (`sv-request: MEASURE`), beside the ceiling both lanes refuse past. Red arm: the disc_ceiling_32k
// flag on the peers, whose lanes then refuse the full disc: the box's slot does not cross and the verdict is FAIL.
// Gated by VOTVCOOP_RUN_SV_REQUEST; the peers' folder is VOTVCOOP_CAN_DIR.
//
// The join arm (VOTVCOOP_SV_REQUEST_REJOIN=1 on both peers, mp.py smoke --rejoin): the chain ends with the full disc in
// the host's laptop slot; the client's first life then leaves, and the rejoined life (VOTVCOOP_SV_REQUEST_REJOINED=1,
// --rejoin-env) must read that slot as the host's, which the connect set carried. The host's verdict waits for it. Red arm:
// --rejoin-env VOTVCOOP_DISC_CEILING_32K=1 as well, so the rejoined life refuses the full set and its laptop stays empty.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/items/save_record_wire.h"
#include "coop/player/players_registry.h"
#include "coop/props/disc_record_ceiling.h"
#include "coop/session/rig_verdict.h"
#include "harness/session_runtime.h"
#include "ue_wrap/actors/floppy_disc.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/desk/sat_console.h"
#include "ue_wrap/devices/floppy_slot.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/serverbox.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace D = ue_wrap::dish;
namespace E = ue_wrap::engine;
namespace FD = ue_wrap::floppy_disc;
namespace FS = ue_wrap::floppy_slot;
namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace SB = ue_wrap::serverbox;
namespace SC = ue_wrap::sat_console;
namespace SR = ue_wrap::save_record;
using disc_drill::OnGameThread;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

constexpr const char* kTag = "sv-request";
constexpr int kDiscWrites = 32;        // a disc's writes (the server box's export loop), 31 rows of them on a laptop
constexpr int kLaptopRows = 31;        // what the laptop's updFloppy keeps of a disc's rows
constexpr int kStepSec = 60;           // the bound on one step of the other peer's
constexpr int kConvergeSec = 45;       // the bound on this peer's reading catching up with the acting peer's
constexpr int kExportSec = 150;        // ten packets, each a coin toss on a low upgrade

struct Fixture {
    int dish = -1;
    void* box = nullptr;
};

// The word the drill ends on, said by whichever peer finds the failure.
bool Need(bool ok, const char* what) {
    UE_LOGI("sv-request: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "%s", what);
    return ok;
}

void Invalid(const char* why) { coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", why); }

// A disc's or a slot's rows as one line: their count and a digest of every character.
std::string Digest(const std::vector<std::wstring>& rows, int32_t writes) {
    uint64_t h = 1469598103934665603ull;
    for (const std::wstring& r : rows) {
        for (wchar_t c : r) {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        h ^= 0xFFFF;
        h *= 1099511628211ull;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "rows=%zu writes=%d digest=%016llx", rows.size(), writes, static_cast<unsigned long long>(h));
    return b;
}

void LogRows(const char* where, const std::vector<std::wstring>& rows) {
    for (size_t i = 0; i < rows.size() && i < 3; ++i) {
        std::wstring shown = rows[i].substr(0, 110);
        for (wchar_t& c : shown)
            if (c == L'\r' || c == L'\n') c = L'|';
        UE_LOGI("sv-request: %hs row %zu len=%zu '%ls'", where, i, rows[i].size(), shown.c_str());
    }
}

void* ServerOf(void* dish) {
    const int32_t off = dish ? R::FindPropertyOffset(R::ClassOf(dish), L"server") : -1;
    return off < 0 ? nullptr : *reinterpret_cast<void**>(static_cast<uint8_t*>(dish) + off);
}

// The first dish with a server, by its index: the same on both peers.
int PickDish() {
    const int n = D::Count();
    for (int i = 0; i < n; ++i)
        if (ServerOf(D::DishByIndex(i))) return i;
    return -1;
}

void ResolveAll() {
    FS::EnsureResolved(FS::DeviceKind::ServerBox);
    FS::EnsureResolved(FS::DeviceKind::Laptop);
    FD::EnsureResolved();
    SB::EnsureResolved();
    D::EnsureResolved();
    SC::EnsureResolved();
    ue_wrap::laptop::EnsureResolved();
}

bool ReadSlot(FS::DeviceKind kind, void* device, std::vector<std::wstring>& rows, int32_t& writes, int32_t& type) {
    FS::Scalars s;
    FS::Content c;
    if (!FS::ReadScalars(kind, device, s) || !FS::ReadContent(kind, device, c)) return false;
    rows = c.data;
    writes = s.readWrites;
    type = s.floppyType;
    return true;
}

// This peer's reading of the box: what it holds, for the other peer to compare with its own.
std::string StepBox(void* box) {
    std::vector<std::wstring> rows;
    int32_t writes = 0, type = -1;
    std::string d = "unread";
    OnGameThread([&] {
        if (ReadSlot(FS::DeviceKind::ServerBox, box, rows, writes, type)) d = "type=" + std::to_string(type) + " " + Digest(rows, writes);
    });
    return d;
}

size_t BoxRows(void* box) {
    std::vector<std::wstring> rows;
    int32_t writes = 0, type = -1;
    OnGameThread([&] { ReadSlot(FS::DeviceKind::ServerBox, box, rows, writes, type); });
    return rows.size();
}

void* FindDisc(const std::wstring& key) {
    void* found = nullptr;
    for (void* o : R::FindObjectsByClass(L"prop_floppyDisc_R_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__") && ue_wrap::prop::GetKeyString(o) == key) found = o;
    return found;
}

// The discs that stand now, by key, of any class of the family.
std::vector<std::wstring> DiscKeys() {
    std::vector<std::wstring> keys;
    for (void* o : R::FindObjectsByClass(L"prop_floppyDisc_R_C"))
        if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) keys.push_back(ue_wrap::prop::GetKeyString(o));
    return keys;
}

// The key of a disc that stands now and did not stand in `before`; empty when there is none.
std::wstring NewDiscKey(const std::vector<std::wstring>& before) {
    for (const std::wstring& k : DiscKeys())
        if (!k.empty() && std::find(before.begin(), before.end(), k) == before.end()) return k;
    return std::wstring();
}

std::string DiscReading(const std::wstring& key) {
    std::string text = "unread";
    OnGameThread([&] {
        FD::DiscContent c;
        void* disc = key.empty() ? nullptr : FindDisc(key);
        if (disc && FD::ReadDiscContent(disc, c)) text = Digest(c.data, c.readWrites);
    });
    return text;
}

std::string LaptopReading() {
    std::string text = "unread";
    OnGameThread([&] {
        void* laptop = nullptr;
        for (void* o : R::FindObjectsByClass(L"laptop_C"))
            if (o && R::IsLive(o) && !R::NameStartsWith(R::NameOf(o), L"Default__")) laptop = o;
        std::vector<std::wstring> rows;
        int32_t writes = 0, type = -1;
        if (laptop && ReadSlot(FS::DeviceKind::Laptop, laptop, rows, writes, type)) {
            // The laptop shows at most kLaptopRows: its own updFloppy clamps floppyData (ui_laptop.cpp:6510), and a peer
            // whose laptop has not refreshed since the insert still holds the 32nd row the game will drop.
            if (rows.size() > static_cast<size_t>(kLaptopRows)) rows.resize(kLaptopRows);
            text = "type=" + std::to_string(type) + " " + Digest(rows, writes);
        }
    });
    return text;
}

// A row as a real request leaves one: a dish's code, the length the game's export writes. The code is never the acting
// dish's own (the one PickDish names, where the request is typed): checkFloppy ends the export on a disc that holds it.
// With no other dish the row carries a filler of the acting dish's code length that no dish owns.
std::wstring RowOf(int k) {
    const int n = D::Count();
    const int acting = PickDish();
    std::vector<int> others;
    for (int i = 0; i < n; ++i)
        if (i != acting) others.push_back(i);
    const int dish = others.empty() ? acting : others[static_cast<size_t>(k) % others.size()];
    std::wstring code;
    if (others.empty()) {
        std::wstring own;
        D::ReadHashcode(acting, own);
        code.assign(own.size(), L'#');
    } else {
        D::ReadHashcode(dish, code);
    }
    const std::wstring eol(L"\r\n");
    return L"$t" + eol + L"Server data: /n name ; /d data" + eol + L"/n " + D::TechName(dish) + eol + L"/d 00" + eol + code;
}

std::vector<std::wstring> RowsOf(int count) {
    std::vector<std::wstring> rows;
    for (int i = 0; i < count; ++i) rows.push_back(RowOf(i));
    return rows;
}

// A disc beside the host's player with `rows` seeded rows and `writesLeft` writes; null when it cannot be made.
void* SpawnDisc(int rows, int writesLeft) {
    void* cls = R::FindClass(L"prop_floppyDisc_R_C");
    void* pawn = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!cls || !pawn || !E::TryGetActorLocation(pawn, at)) return nullptr;
    void* disc = E::SpawnActor(cls, {at.X + 100.f, at.Y, at.Z + 50.f});
    if (!disc) return nullptr;
    FD::DiscContent content;
    content.data = RowsOf(rows);
    content.readWrites = writesLeft;
    FD::WriteDiscContent(disc, content);
    return disc;
}

// What a disc's record weighs on the wire, built as prop_save_data's body is: the version byte, the key's length, the key,
// then the record. Zero when the disc cannot be made or captured.
size_t RecordBytes(int rows, int writesLeft) {
    size_t bytes = 0;
    OnGameThread([&] {
        void* disc = SpawnDisc(rows, writesLeft);
        if (!disc) return;
        SR::SaveRecord rec;
        if (SR::CaptureRecord(disc, rec)) {
            std::vector<uint8_t> body;
            coop::save_record_wire::SerSave(body, rec);
            bytes = 2 + ue_wrap::prop::GetKeyString(disc).size() + body.size();
        }
        E::DestroyActor(disc);
    });
    return bytes;
}

// The host's measurement: a box disc at its 32 writes and a laptop disc at its 31, beside the ceiling.
void MeasureFullDiscs() {
    size_t rowChars = 0;
    OnGameThread([&] { rowChars = RowOf(0).size(); });
    const size_t box = RecordBytes(kDiscWrites, 0);
    const size_t laptop = RecordBytes(kLaptopRows, kDiscWrites - kLaptopRows);
    UE_LOGI("sv-request: MEASURE record of a box disc at %d writes (%d rows): %zu B; of a laptop disc at %d writes (%d rows): "
            "%zu B; the ceiling %zu B; a row is %zu chars", kDiscWrites, kDiscWrites, box, kLaptopRows, kLaptopRows, laptop,
            coop::disc_record::MaxBodyBytes(), rowChars);
}

// One step of the chain. The peer that acted publishes what it reads and the other waits to read the same; the host
// judges, whichever of the two it is. False after the verdict was said (or the other peer's word never came).
// `refSec` is how long the other peer waits for the acting peer's reference: the acting peer's own work before it.
bool Converge(const std::string& step, bool iAct, bool host, const std::function<std::string()>& read, const char* what,
              int refSec = kStepSec) {
    std::string word;
    if (iAct) {
        Mark(("ref_" + step).c_str(), read());
        if (!AwaitText(("seen_" + step).c_str(), word, kStepSec + kConvergeSec)) return false;
    } else {
        std::string ref;
        if (!AwaitText(("ref_" + step).c_str(), ref, refSec)) return false;
        const bool same = PS::WaitFor("this peer's reading to match", kConvergeSec, [&] { return read() == ref; }, false);
        word = same ? std::string("1") : "0 " + read();
        UE_LOGI("sv-request: %hs: here %hs, there %hs", step.c_str(), read().c_str(), ref.c_str());
        Mark(("seen_" + step).c_str(), word);
    }
    if (!host) return true;
    if (word.empty() || word[0] != '1') UE_LOGW("sv-request: %hs: the other peer read '%hs'", step.c_str(), word.c_str());
    return Need(!word.empty() && word[0] == '1', what);
}

// The dish and its server box, once both exist here.
bool ResolveBox(Fixture& fx, int wantDish) {
    return PS::WaitFor("a dish with a server box", 240, [&] {
        OnGameThread([&] {
            ResolveAll();
            fx.dish = PickDish();
            fx.box = fx.dish >= 0 ? ServerOf(D::DishByIndex(fx.dish)) : nullptr;
        });
        return fx.box && (wantDish < 0 || fx.dish == wantDish);
    });
}

// The steps both peers walk, in the one order. `iAct` is whether this peer types the request, ejects and inserts.
bool Chain(const Fixture& fx, bool iAct, bool host, size_t wantRows) {
    if (!Converge("inserted", host, host, [&] { return StepBox(fx.box); }, "the full disc in the slot reads the same on both peers"))
        return false;

    if (iAct) {
        OnGameThread([&] {
            void* term = SC::LocalTerminal();
            void* dish = D::DishByIndex(fx.dish);
            if (!term || !dish) return;
            SC::CallInit(term, D::TechName(fx.dish), false, dish);
            SC::RunCommand(term, L"clr");
            SC::RunCommand(term, L"sv.request");
            UE_LOGI("sv-request: typed sv.request at dish %d", fx.dish);
        });
        // The row count is logged every 10 s, so a run shows the export's progress, or that sv.request wrote nothing.
        const ULONGLONG exportStart = ::GetTickCount64();
        ULONGLONG lastLog = exportStart;
        if (!Need(PS::WaitFor("the export's row", kExportSec, [&] {
                      const size_t rows = BoxRows(fx.box);
                      const ULONGLONG now = ::GetTickCount64();
                      if (rows < wantRows && now - lastLog >= 10000) {
                          lastLog = now;
                          UE_LOGI("sv-request: export wait %llu s: %zu of %zu rows in this peer's slot",
                                  static_cast<unsigned long long>((now - exportStart) / 1000), rows, wantRows);
                      }
                      return rows >= wantRows;
                  }, false),
                  "the export wrote a row into this peer's slot"))
            return false;
        bool namesCode = false;
        std::vector<std::wstring> rows;
        int32_t writes = 0, type = -1;
        OnGameThread([&] {
            ReadSlot(FS::DeviceKind::ServerBox, fx.box, rows, writes, type);
            std::wstring code;
            D::ReadHashcode(fx.dish, code);
            for (const std::wstring& r : rows)
                if (!code.empty() && r.find(code) != std::wstring::npos) namesCode = true;
            UE_LOGI("sv-request: this peer's dish code has %zu characters", code.size());
        });
        LogRows("slot", rows);
        if (!Need(namesCode, "the row holds this peer's own code for that dish")) return false;
    }
    if (!Converge("exported", iAct, host, [&] { return StepBox(fx.box); }, "the exported rows read the same on both peers",
                  kExportSec + kStepSec))
        return false;

    // Both peers note which discs stand before the eject, and only then does the acting peer eject.
    std::vector<std::wstring> before;
    OnGameThread([&] { before = DiscKeys(); });
    if (!Converge("ready_eject", iAct, host, [&] { return std::string("x"); }, "both peers stood ready for the eject")) return false;
    if (iAct) {
        OnGameThread([&] { SB::CallEjectFloppy(fx.box); });
        UE_LOGI("sv-request: ejected");
    }
    std::wstring discKey;
    const bool stands = PS::WaitFor("the ejected disc", 60, [&] {
        OnGameThread([&] { discKey = NewDiscKey(before); });
        return !discKey.empty();
    }, false);
    if (!Need(stands, "the ejected disc stands on this peer")) return false;
    if (!Converge("ejected", iAct, host, [&] { return DiscReading(discKey); }, "the ejected disc's rows read the same on both peers"))
        return false;

    if (iAct) {
        OnGameThread([&] {
            if (void* disc = FindDisc(discKey)) ue_wrap::laptop::CallInsertDisc(disc);
        });
        UE_LOGI("sv-request: inserted in the laptop");
        if (!Need(PS::WaitFor("the laptop's rows", 60, [] { return LaptopReading().find("rows=0") == std::string::npos &&
                                                                    LaptopReading() != "unread"; }, false),
                  "the laptop holds the rows on this peer"))
            return false;
    }
    return Converge("laptop", iAct, host, [&] { return LaptopReading(); }, "the laptop's rows read the same on both peers");
}

bool JoinArm() { return coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_REJOIN") == "1"; }

// The join arm, host side: once the client's first life has left slot 1, the rejoined life must read the laptop's disc as
// this peer does. False after the verdict was said (or a word never came).
bool HostRejoin() {
    auto& session = harness::session_runtime::Session();
    const uint32_t life1Gen = session.peerGenerationForSlot(1);
    Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [&session, life1Gen] {
            return !session.IsSlotConnected(1) || session.peerGenerationForSlot(1) != life1Gen;
        }))
        return false;
    if (!Await("rejoin_ready", 400)) return false;
    const std::string row = LaptopReading();
    UE_LOGI("sv-request: host for the rejoin: laptop %hs", row.c_str());
    if (row == "unread" || row.find("rows=0") != std::string::npos) {
        Invalid("the host's laptop holds no disc for the rejoined client to read");
        return false;
    }
    Mark("e_row", row);
    std::string word;
    if (!AwaitText("rejoin_done", word, 400)) return false;
    if (word.empty() || word[0] != '1') UE_LOGW("sv-request: the rejoined life read '%hs'", word.c_str());
    return Need(!word.empty() && word[0] == '1', "the rejoined client reads the full disc in the laptop as the host does");
}

// The client's first life, after the chain: it leaves as a player's quit does (network first) once the host watches its
// slot, so the host sees slot 1 empty at once.
void LeaveForRejoin() {
    Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "sv-request: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
}

// The rejoined life: the connect set carried the full disc into the laptop slot, and it reads the host's.
void RunRejoined() {
    if (!disc_drill::HostPuppetUp()) return;
    OnGameThread([] { ResolveAll(); });
    Mark("rejoin_ready");
    std::string host;
    if (!AwaitText("e_row", host, kStepSec + kConvergeSec)) return;
    const bool same = PS::WaitFor("the rejoined laptop to read the host's", kConvergeSec, [&] { return LaptopReading() == host; }, false);
    const std::string here = LaptopReading();
    UE_LOGI("sv-request: rejoined: here %hs, there %hs", here.c_str(), host.c_str());
    Mark("rejoin_done", same ? std::string("1") : "0 " + here);
    UE_LOGI("sv-request: REJOIN DONE");
}

void RunHost(bool iAct, int seed) {
    if (!disc_drill::HostWorldUp()) return;
    Fixture fx;
    if (!ResolveBox(fx, -1)) return;
    UE_LOGI("sv-request: dish %d", fx.dish);
    MeasureFullDiscs();
    // The host names the dish once the client says it stands beside it: a signal written before the client's drill began
    // would not count as its answer.
    if (!Await("client_ready", 480)) return;
    Mark("dish", std::to_string(fx.dish));
    if (!Await("client_resolved", kStepSec)) return;

    // The fixture: a disc in the first dish's server, seeded, as a player's insert does.
    const int rows = seed;
    bool inserted = false;
    OnGameThread([&] {
        void* disc = SpawnDisc(rows, kDiscWrites - rows);
        if (disc) inserted = SB::CallProcessFloppy(fx.box, disc);
        UE_LOGI("sv-request: the disc holds %d rows before the request; the insert was called (%d)", rows, inserted ? 1 : 0);
    });
    if (!inserted) return Invalid("the fixture disc could not be made or inserted");
    OnGameThread([&] {
        std::wstring code;
        D::ReadHashcode(fx.dish, code);
        bool carried = code.empty();
        for (const std::wstring& r : RowsOf(rows))
            if (!code.empty() && r.find(code) != std::wstring::npos) carried = true;
        UE_LOGI("sv-request: dish %d acts; no seeded row carries its code (%d)", fx.dish, carried ? 0 : 1);
    });
    if (!PS::WaitFor("the disc in the host's slot", 30, [&] { return BoxRows(fx.box) == static_cast<size_t>(rows); }, false))
        return Invalid("the fixture disc never read in the host's slot");

    if (!Chain(fx, iAct, true, static_cast<size_t>(rows) + 1)) return;
    std::string clientWord;
    if (!AwaitText("client_done", clientWord, kStepSec)) return;
    if (JoinArm() && !HostRejoin()) return;
    UE_LOGI("sv-request: DONE on host");
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass, "a disc of %d rows crossed the box, the eject and the laptop%s", rows + 1,
                           JoinArm() ? ", and the rejoined client read it" : "");
}

void RunClient(bool iAct, int seed) {
    if (!disc_drill::HostPuppetUp()) return;
    if (!meet::WalkToHost(kTag)) return;
    Mark("client_ready");
    std::string hostDish;
    if (!AwaitText("dish", hostDish, kStepSec)) return;
    Fixture fx;
    if (!ResolveBox(fx, std::atoi(hostDish.c_str()))) return;
    Mark("client_resolved");
    if (!Chain(fx, iAct, false, static_cast<size_t>(seed) + 1)) return;
    Mark("client_done");
    if (JoinArm()) LeaveForRejoin();
    UE_LOGI("sv-request: DONE on client");
}

}  // namespace

DWORD WINAPI SvRequestThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    const bool client = IsClientRole();
    const bool clientActs = coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_WHO") == "client";
    // The seeded rows, left one write for the request (a disc has 32).
    int seed = std::atoi(coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_SEED").c_str());
    seed = seed < 0 ? 0 : seed > kDiscWrites - 1 ? kDiscWrites - 1 : seed;
    if (client && coop::config::ReadEnv("VOTVCOOP_SV_REQUEST_REJOINED") == "1") RunRejoined();
    else if (client) RunClient(client == clientActs, seed);
    else RunHost(client == clientActs, seed);
    return 0;
}

}  // namespace harness::autotest
