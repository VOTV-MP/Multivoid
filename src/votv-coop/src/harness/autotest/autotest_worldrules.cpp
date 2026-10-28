// harness/autotest/autotest_worldrules.cpp -- the world-rules probe
// (VOTVCOOP_RUN_WORLDRULES_PROBE): reads BOTH copies of the world rules on a peer and logs every
// rule, so a rig can compare the copies and the peers. The interface and the description live in
// harness/autotest.h.

#include "harness/autotest.h"

#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/world/game_rules.h"
#include "ue_wrap/world/game_rules_pane.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;
namespace GR = ue_wrap::game_rules;
namespace GP = ue_wrap::game_rules_pane;

std::string ValueText(const GR::RuleField& f) {
    char buf[32];
    switch (f.kind) {
        case GR::Kind::Bool:  return f.bval ? "On" : "Off";
        case GR::Kind::Float: std::snprintf(buf, sizeof(buf), "%.2f", f.fval); return buf;
        case GR::Kind::Enum:  std::snprintf(buf, sizeof(buf), "#%d", f.ival); return buf;
    }
    return "?";
}

// This peer's world is up and, on a client, its join is over: the moment the rules a session runs
// under are whatever they are going to be. Game thread.
bool PeerIsReady() {
    void* player = coop::players::Registry::Get().Local();
    if (!player || !ue_wrap::reflection::IsLive(player) || !ue_wrap::engine::GetController(player)) return false;
    if (!IsClientRole()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

// One task on the game thread, waited for.
template <typename Fn>
void OnGameThread(Fn fn) {
    auto ran = std::make_shared<std::atomic<bool>>(false);
    GT::Post([fn, ran] { fn(); ran->store(true); });
    while (!ran->load()) ::Sleep(5);
}

const GR::RuleField* Find(const std::vector<GR::RuleField>& rules, const char* key) {
    for (const GR::RuleField& f : rules) if (f.key == key) return &f;
    return nullptr;
}

// The three values the local player keeps from the rules, against the rules they come from. The
// player refills them whenever the game applies its settings (mainPlayer.intComs_settingsApplied),
// the first time a few seconds into a world, so they trail the rules and are waited for. Returns
// how many disagree, -1 when the player or a rule is not there. Game thread.
int LatchDisagreements(std::string& text) {
    text.clear();
    void* player = coop::players::Registry::Get().Local();
    GR::Snapshot s;
    if (!player || !ue_wrap::reflection::IsLive(player) || !GR::ReadLocal(s) || !s.valid) return -1;
    void* cls = ue_wrap::reflection::ClassOf(player);
    struct Row { const wchar_t* latch; const char* rule; bool fromSaved; };
    // Blood loss is one of the three rules the game reads off the save object, not the process copy.
    static const Row kRows[] = {{L"canFallDamage", "fallDamage", false},
                                {L"enableWaterFallDamage", "enableWaterFallDamage", false},
                                {L"enableBloodLoss", "bloodLoss", true}};
    int disagree = 0;
    for (const Row& row : kRows) {
        int32_t off = -1;
        uint8_t mask = 0;
        const GR::RuleField* rule = Find(row.fromSaved ? s.saved : s.fields, row.rule);
        if (!rule || !ue_wrap::reflection::FindBoolProperty(cls, row.latch, off, mask) || off < 0) return -1;
        const bool latched = (reinterpret_cast<uint8_t*>(player)[off] & mask) != 0;
        if (latched != rule->bval) ++disagree;
        char item[96];
        std::snprintf(item, sizeof(item), " %ls=%s(rule %s)", row.latch, latched ? "On" : "Off",
                      rule->bval ? "On" : "Off");
        text += item;
    }
    return disagree;
}

}  // namespace

// --- World-rules probe (VOTVCOOP_RUN_WORLDRULES_PROBE=1) ----------------------
//
// The game keeps the world rules twice: the SAVED copy on the save object (localGameRules) and the
// PER-PROCESS copy on the GameInstance (gameRules), which is the one nearly every rule is read
// from. The probe logs both on each peer, one line per rule, and marks a rule whose copies differ;
// then it waits for the values the local player keeps from the rules to agree with them. It can
// only tell anything on a world whose rules are not the defaults: two peers showing the same
// default is not a value that travelled.
//   worldrules:   <key> process=<v> saved=<v>[ MISMATCH]
//   worldrules: player latches: <latch>=<v>(rule <v>) ... -- agree after <n> s | STILL DISAGREE
//   worldrules: DONE role=<host|client> rules=<n> mismatches=<m> latch-disagreements=<k>
// The last is the line a rig waits on; the role is in it because a rig reads both logs and the
// host is done long before a client. The probe waits on readiness, not on a clock: the host reads
// once its player stands, a client once its join is over, and the latches are polled until they
// agree, with a ceiling that ends the wait as a failure.
void RunWorldRulesProbe() {
    const char* role = IsClientRole() ? "client" : "host";
    UE_LOGI("worldrules: probe start (%s, waiting for this peer to be ready)", role);
    for (bool ready = false; !ready; ) {
        OnGameThread([&ready] { ready = PeerIsReady(); });
        if (!ready) ::Sleep(250);
    }

    int rules = 0, mismatches = 0;
    OnGameThread([&rules, &mismatches] {
        GR::Snapshot s;
        if (!GR::ReadLocal(s) || !s.valid) {
            UE_LOGW("worldrules: ReadLocal failed / not valid (GameInstance not up?)");
            return;
        }
        rules = static_cast<int>(s.fields.size());
        UE_LOGI("worldrules: gamemode=%s rules=%d saved-copy=%s", s.gamemodeName.c_str(), rules,
                s.savedValid ? "read" : "UNAVAILABLE");
        for (size_t i = 0; i < s.fields.size(); ++i) {
            const GR::RuleField& f = s.fields[i];
            const std::string process = ValueText(f);
            const std::string saved = (s.savedValid && i < s.saved.size()) ? ValueText(s.saved[i]) : "?";
            const bool differs = s.savedValid && saved != process;
            if (differs) ++mismatches;
            UE_LOGI("worldrules:   %-24s process=%-5s saved=%-5s%s", f.key.c_str(), process.c_str(),
                    saved.c_str(), differs ? " MISMATCH" : "");
        }
    });

    // The game's own pane layout, joined to the rules the way the panel joins it: a rule the pane
    // does not place, or a row that finds no rule, shows here before it shows on screen.
    OnGameThread([] {
        GP::Pane pane;
        GR::Snapshot s;
        if (!GP::Read(pane) || !GR::ReadLocal(s)) {
            UE_LOGW("worldrules: pane layout UNAVAILABLE (the rules widget class is not loaded)");
            return;
        }
        int rows = 0, unresolved = 0;
        for (const GP::Category& cat : pane.categories) {
            for (const GP::Row& row : cat.rows) {
                const GR::Kind kind = row.control == GP::Control::Check ? GR::Kind::Bool
                                    : row.control == GP::Control::Slider ? GR::Kind::Float : GR::Kind::Enum;
                const GR::RuleField* rule = GR::NthOfKind(s.fields, kind, row.index);
                ++rows;
                if (!rule) ++unresolved;
                UE_LOGI("worldrules: pane [%s%s] '%s' -> %s = %s", cat.name.c_str(), cat.hidden ? ", hidden" : "",
                        row.label.c_str(), rule ? rule->key.c_str() : "(no rule)",
                        rule ? (rule->kind == GR::Kind::Enum && !rule->valueName.empty() ? rule->valueName
                                                                                          : ValueText(*rule)).c_str()
                             : "?");
            }
        }
        UE_LOGI("worldrules: pane categories=%d rows=%d unresolved=%d", static_cast<int>(pane.categories.size()),
                rows, unresolved);
    });

    constexpr int kLatchCeilingMs = 30000;  // the game's first settings apply lands seconds into a world
    int disagree = -1, waitedMs = 0;
    std::string text;
    for (;;) {
        OnGameThread([&disagree, &text] { disagree = LatchDisagreements(text); });
        if (disagree == 0 || waitedMs >= kLatchCeilingMs) break;
        ::Sleep(250);
        waitedMs += 250;
    }
    if (disagree == 0) UE_LOGI("worldrules: player latches:%s -- agree after %d s", text.c_str(), waitedMs / 1000);
    else UE_LOGW("worldrules: player latches:%s -- STILL DISAGREE after %d s", text.c_str(), waitedMs / 1000);
    // The save's day: the boot hands it to the gamemode through mainGameInstance.startDay, and the
    // gamemode zeroes that field once it has taken it. So a day > 0 in the boot's log line and a 0
    // here is the block having run; a value still standing here is a gamemode that never took it.
    // Read AFTER the latches agree: the player's latches are filled in the gamemode's BeginPlay, the
    // same synchronous chain that takes the day, and a host's player stands before that chain runs.
    OnGameThread([] {
        namespace R = ue_wrap::reflection;
        void* gi = R::FindObjectByClass(L"mainGameInstance_C");
        const int32_t off = gi ? R::FindPropertyOffset(R::ClassOf(gi), L"startDay") : -1;
        if (off < 0) { UE_LOGW("worldrules: startDay unreadable"); return; }
        UE_LOGI("worldrules: startDay now=%d (0 = the gamemode took the save's day)",
                *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(gi) + off));
    });

    UE_LOGI("worldrules: DONE role=%s rules=%d mismatches=%d latch-disagreements=%d", role, rules, mismatches,
            disagree);
}

DWORD WINAPI WorldRulesProbeThread(LPVOID /*arg*/) {
    RunWorldRulesProbe();
    return 0;
}

}  // namespace harness::autotest
