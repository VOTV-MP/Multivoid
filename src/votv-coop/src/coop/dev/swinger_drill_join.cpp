// coop/dev/swinger_drill_join.cpp -- see coop/dev/swinger_drill_join.h.

#include "coop/dev/swinger_drill_join.h"

#include "coop/comms/chat_feed.h"
#include "coop/interactables/interactable_sync.h"
#include "coop/interactables/swinger_join_pose.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/swinger.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace coop::dev::swinger_drill_join {
namespace {

namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace JP = coop::swinger_join_pose;
using Clock = std::chrono::steady_clock;

std::array<bool, coop::net::kMaxPeers> g_greeted{};   // HOST: the slots that have heard their word this occupancy

// HOST: the measure arm's verdict (NoteVerdict). Not cleared by Reset().
bool g_haveVerdict = false;
std::wstring g_verdictKey;
std::string g_verdictResult;
float g_verdictFromArmDeg = -1.f;

Word g_word = Word::None;           // CLIENT: the host's word, once heard
bool g_wordArmed = false;           // the word clock runs from the first PollWord of a life
Clock::time_point g_wordSince{}, g_nextPoll{};

bool g_armClockOn = false, g_armGaveUp = false;   // HOST: the arm's bound runs from its first try
Clock::time_point g_armSince{}, g_armNext{};

bool g_active = false;
bool g_clockOn = false;   // the judge's bound runs from its first tick: the word may arrive before the drill arms
std::wstring g_key;
ue_wrap::FRotator g_restRot{};   // the host's copy at its verdict
ue_wrap::FVector  g_restAt{};
Clock::time_point g_since{}, g_nextRead{};
bool g_haveRead = false;
ue_wrap::FRotator g_lastRot{};   // this copy at the last read
ue_wrap::FVector  g_lastAt{};

void Finish(const char* result, const char* detail) {
    UE_LOGI("[SWING-DRILL] rejoin %s: %s", result, detail);
    UE_LOGI("[SWING-DRILL] rejoin DONE");
    g_active = false;
}

// The word to a greeted client: `go` with no verdict, `rest` with the verdict door's pose after a PASS that moved
// the door past the settle tolerance (else a fresh copy passes without the pose), else `void <token>`.
std::string GreetingWord(int slot) {
    if (!g_haveVerdict) return "go";
    if (g_verdictResult == "INVALID") return "void life1-INVALID";
    if (g_verdictResult != "PASS") return "void life1-FAIL";
    // A joiner's copy starts at the hinge default, as the host's did at its arm: the game saves no swinger pose.
    if (g_verdictFromArmDeg <= kSettleDeg) return "void rest-at-start";
    void* const door = g_verdictKey.empty() ? nullptr : coop::interactable_sync::ContainerActor(g_verdictKey);
    void* const mesh = door ? PR::GetStaticMesh(door) : nullptr;
    ue_wrap::FVector at{};
    if (!mesh || !E::TryGetActorLocation(door, at)) {
        UE_LOGW("[SWING-DRILL] host cannot read the verdict's door for slot %d -- says void", slot);
        return "void door-unreadable";
    }
    // The connect snapshot sends no pose for a lid that reads closed, so a closed door has no pose to wait for.
    bool open = false;
    if (!ue_wrap::swinger::TryReadOpen(door, open) || !open) return "void rest-closed";
    const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
    char line[160];
    std::snprintf(line, sizeof(line), "rest %ls %.2f %.2f %.2f %.1f %.1f %.1f", g_verdictKey.c_str(), w.Pitch, w.Yaw,
                  w.Roll, at.X, at.Y, at.Z);
    return line;
}

// The host's word as a tagged row carries it, or None: `go`, `void <token>`, `rest <lane key> <p> <y> <r> <x> <y> <z>`.
Word ParseWord(const std::vector<std::string>& w) {
    if (w.size() == 1 && w[0] == "go") return Word::Go;
    if (w.size() == 2 && w[0] == "void") return Word::Void;
    if (w.size() == 8 && w[0] == "rest") return Word::Rest;
    return Word::None;
}

void Fail(const char* detail) {
    g_word = Word::Void;
    Finish("FAIL", detail);
}

}  // namespace

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::vector<std::string> SplitWords(const std::string& text) {
    std::vector<std::string> word;
    for (size_t from = 0; from < text.size();) {
        const size_t sp = text.find(' ', from);
        word.push_back(text.substr(from, sp == std::string::npos ? std::string::npos : sp - from));
        from = sp == std::string::npos ? text.size() : sp + 1;
    }
    return word;
}

void NoteVerdict(const std::wstring& laneKey, const char* result, float fromArmDeg) {
    g_haveVerdict = true;
    g_verdictKey = laneKey;
    g_verdictResult = result;
    g_verdictFromArmDeg = fromArmDeg;
}

void GreetReadySlots(coop::net::Session& session, SayFn say) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        const bool ready = session.IsSlotWorldReady(slot);
        if (!ready) { g_greeted[static_cast<size_t>(slot)] = false; continue; }
        if (g_greeted[static_cast<size_t>(slot)]) continue;
        g_greeted[static_cast<size_t>(slot)] = true;
        say(GreetingWord(slot));
    }
}

Word PollWord(const char* tag, Clock::time_point now) {
    if (g_word != Word::None) return Word::None;
    if (!g_wordArmed) { g_wordArmed = true; g_wordSince = now; g_nextPoll = now; }
    if (now - g_wordSince > kWordBound) { Fail("no word from the host within the bound"); return Word::Void; }
    if (now < g_nextPoll) return Word::None;
    g_nextPoll = now + kSampleEvery;
    unsigned long long newest = 0;
    bool got = false;
    std::vector<std::string> heard;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        const size_t at = r.line.find(tag);
        // A past life's row is retained, a live one is not.
        if (r.retained || at == std::string_view::npos || (got && r.key <= newest)) return;
        std::vector<std::string> w = SplitWords(std::string(r.line.substr(at + std::strlen(tag))));
        if (ParseWord(w) == Word::None) return;
        newest = r.key;
        got = true;
        heard = std::move(w);
    });
    const Word word = ParseWord(heard);
    if (word == Word::None) return Word::None;
    g_word = word;
    if (word == Word::Void) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "the host gave no rest pose (%s)", heard[1].c_str());
        Fail(detail);
    } else if (word == Word::Rest) {
        float v[6];
        for (size_t i = 0; i < 6; ++i) v[i] = std::strtof(heard[i + 2].c_str(), nullptr);
        Begin(heard[1], {v[0], v[1], v[2]}, {v[3], v[4], v[5]});
    }
    return word;
}

bool HeardGo() { return g_word == Word::Go; }

void Begin(const std::string& laneKey, const ue_wrap::FRotator& rot, const ue_wrap::FVector& at) {
    if (g_active) return;
    g_key.assign(laneKey.begin(), laneKey.end());
    g_restRot = rot;
    g_restAt = at;
    g_clockOn = false;
    g_haveRead = false;
    g_active = true;
    UE_LOGI("[SWING-DRILL] rejoin judging key='%ls' against the host's (%.2f, %.2f, %.2f) place (%.1f, %.1f, %.1f)",
            g_key.c_str(), rot.Pitch, rot.Yaw, rot.Roll, at.X, at.Y, at.Z);
}

bool Active() { return g_active; }

void Tick(Clock::time_point now) {
    if (!g_active) return;
    if (!g_clockOn) { g_clockOn = true; g_since = g_nextRead = now; }
    const JP::dev::State st = JP::dev::Outcome(g_key);
    if (st == JP::dev::State::Dropped) { Finish("FAIL", "the pose was dropped"); return; }
    if (now - g_since > kClientBound) {
        Finish("FAIL", st == JP::dev::State::Applied ? "the placed copy never came to rest within the bound"
                       : st == JP::dev::State::Pending ? "the pose never applied within the bound"
                                                       : "no pose arrived within the bound");
        return;
    }
    if (st != JP::dev::State::Applied || now < g_nextRead) return;
    g_nextRead = now + kSampleEvery;
    void* const door = coop::interactable_sync::ContainerActor(g_key);
    void* const mesh = door ? PR::GetStaticMesh(door) : nullptr;
    ue_wrap::FVector at{};
    if (!mesh || !E::TryGetActorLocation(door, at) || !E::IsComponentSimulatingPhysics(mesh)) {
        g_haveRead = false;
        return;
    }
    const ue_wrap::FRotator w = E::GetComponentWorldRotation(mesh);
    if (!g_haveRead || E::RotatorAngleDeg(w, g_lastRot) > kRestDeg || Dist(at, g_lastAt) > kRestCm) {
        g_lastRot = w;
        g_lastAt = at;
        g_haveRead = true;
        return;
    }
    const float deg = E::RotatorAngleDeg(w, g_restRot);
    const float cm = Dist(at, g_restAt);
    bool open = false;
    const char* const opened = ue_wrap::swinger::TryReadOpen(door, open) ? (open ? "1" : "0") : "unread";
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%.2f deg, %.1f cm (need <= %.0f, <= %.0f) | opened %s", deg, cm,
                  kSettleDeg, kSettleCm, opened);
    Finish(deg <= kSettleDeg && cm <= kSettleCm ? "PASS" : "FAIL", detail);
}

bool HostArmDue(coop::net::Session& session, Clock::time_point now) {
    if (g_armGaveUp || !session.AnyWorldReadyPeer()) return false;
    if (!g_armClockOn) {
        g_armClockOn = true;
        g_armSince = g_armNext = now;
    }
    if (now - g_armSince > kArmBoundMs) {
        g_armGaveUp = true;
        UE_LOGW("[SWING-DRILL] host INVALID: no non-lockable lid to watch after the bound");
        return false;
    }
    if (now < g_armNext) return false;
    g_armNext = now + kSampleEvery;
    return true;
}

void Reset() {
    g_armClockOn = g_armGaveUp = false;
    g_greeted.fill(false);
    g_word = Word::None;
    g_wordArmed = false;
    g_active = false;
    g_haveRead = false;
}

}  // namespace coop::dev::swinger_drill_join
