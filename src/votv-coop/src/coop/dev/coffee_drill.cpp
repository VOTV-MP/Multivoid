// coop/dev/coffee_drill.cpp -- see coop/dev/coffee_drill.h. The drill's switch, its chat edges and its failure line;
// the roles are coffee_drill_host.cpp and coffee_drill_client.cpp.

#include "coop/dev/coffee_drill.h"

#include "coffee_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::dev::coffee_drill {
namespace {

namespace DC = coop::dev::drill_chat;
namespace D = detail;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::coffee_drill);
    return s;
}

// The rows of the lobby chat are six live ones, each retiring as the next come: every line either role waits for is
// latched on sight, from the first tick, so a peer busy walking does not lose an edge.
std::unordered_set<std::string> g_heard;
std::unordered_map<std::string, std::string> g_rests;

const std::vector<std::string>& Watched() {
    static const std::vector<std::string> lines = [] {
        std::vector<std::string> v;
        for (int n = 1; n <= D::kArms; ++n) {
            v.push_back(D::ArmLine(n));
            v.push_back(D::AtLine(n));
            v.push_back(D::HostSeenLine(n));
            v.push_back(D::ClientSeenLine(n));
        }
        for (const char* l : {"coffee client staged", "coffee client hit 4", "coffee client inconclusive 4", "coffee client inconclusive 5", "coffee host invalid"})
            v.push_back(l);
        return v;
    }();
    return lines;
}

const std::vector<std::string>& RestPrefixes() {
    static const std::vector<std::string> prefixes = [] {
        std::vector<std::string> v = {"coffee keys ", "coffee expect ", "coffee client ready ", D::kFillPrefix};
        for (const int n : {1, 3, 4, 5}) v.push_back(D::CupPrefix(n));
        return v;
    }();
    return prefixes;
}

}  // namespace

namespace detail {

bool Heard(const std::string& line) { return g_heard.count(line) != 0; }

bool Rest(const std::string& prefix, std::string* rest) {
    const auto it = g_rests.find(prefix);
    if (it == g_rests.end()) return false;
    *rest = it->second;
    return true;
}

void Say(const std::string& line) { DC::Say(line); }

std::string ArmLine(int n) { return "coffee host arm " + std::to_string(n); }
std::string AtLine(int n) { return "coffee client at " + std::to_string(n); }
std::string HostSeenLine(int n) { return "coffee host seen " + std::to_string(n); }
std::string ClientSeenLine(int n) { return "coffee client seen " + std::to_string(n); }
std::string CupPrefix(int n) { return "coffee host cup " + std::to_string(n) + " "; }

void FailLine(const char* peer, const char* arm, const char* why) {
    Flags f;
    const bool read = Maker() && ReadFlags(Maker(), f);
    UE_LOGW("[COFFEE] FAIL: %s: %s | %s maker %s points=%d", arm, why, peer,
            read ? FlagsText(f).c_str() : "unread", static_cast<int>(Points()));
}

uint64_t NowMs() { return coop::net::NowMs(); }

}  // namespace detail

void Install(coop::net::Session* s) {
    if (!Enabled()) return;
    g_session.store(s, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (DC::PollDue()) {
        for (const std::string& line : Watched())
            if (!g_heard.count(line) && DC::ChatHasLive(line)) g_heard.insert(line);
        for (const std::string& prefix : RestPrefixes()) {
            std::string rest;
            if (DC::LiveRestOf(prefix, &rest)) g_rests[prefix] = rest;
        }
    }
    if (s->role() == coop::net::Role::Host) D::HostTick(s);
    else if (s->connected()) D::ClientTick(s);
    D::MakerProbeTick();
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_heard.clear();
    g_rests.clear();
    D::ClientReset();
    D::MakerProbeReset();
    if (!D::HostOnDisconnect()) D::ResetCast();
}

}  // namespace coop::dev::coffee_drill
