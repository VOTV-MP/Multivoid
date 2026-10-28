#include <atomic>
#include <cstdio>
#include <string>

namespace coop::net {
enum class Role { Host, Client };
struct Session {
    Role side = Role::Client;
    bool live = true;
    bool connected() const { return live; }
    Role role() const { return side; }
};
}
namespace coop::config_registry {
struct EnumRow {};
struct FlagRow {};
namespace rows {
inline constexpr EnumRow pr64_drill{};
inline constexpr FlagRow pr64_coin_local_credit_red{};
}
}
namespace coop::config {
// The drill's red rows stay off here: the production verdict is what the assertions run.
inline std::string ResolveEnum(const coop::config_registry::EnumRow&) { return "off"; }
inline bool ResolveFlag(const coop::config_registry::FlagRow&) { return false; }
}
namespace sg {
enum class Verdict { Run, Cancel };
struct Call { void* object; bool fromOurCode; };
}
namespace fixture {
coop::net::Session session;
coop::net::Session* active = &session;
bool coin = true, mirrored = true, captured = false, sendOk = true;
int forwarded = 0;
}
namespace I {
bool IsCoinActor(void*) { return fixture::coin; }
bool IsCapturedCoin(void*) { return fixture::captured; }
coop::net::Session* Session() { return fixture::active; }
}
bool IsHostOwnedCoin(void*) { return fixture::mirrored; }
bool ForwardCollectToHost(void*, const wchar_t*) {
    ++fixture::forwarded;
    return fixture::sendOk;
}
std::atomic<unsigned long long> g_seenPressHost{0}, g_seenPressClient{0};
#define UE_LOGI(...) ((void)0)
#define UE_LOGW(...) ((void)0)

#include COLLECT_CALLBACK

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    int body = 0;
    const sg::Call press{&body, false};
    auto localCollect = [&] {
        int balance = 100;
        bool coinLive = true;
        if (OnCollectVerb(press) == sg::Verdict::Run) {
            balance += 10;
            coinLive = false;
        }
        return balance == 100 && coinLive;
    };

    check(localCollect(), "refused host-owned pickup keeps local coin and canonical balance");
    check(fixture::forwarded == 1, "host-owned press forwards one intent");
    fixture::sendOk = false;
    check(localCollect(), "failed send leaves the coin available for retry");
    fixture::sendOk = true;

    fixture::mirrored = false;
    fixture::captured = true;
    const int before = fixture::forwarded;
    check(localCollect(), "pre-barrier sale coin cannot grant phantom credit");
    check(fixture::forwarded == before, "unpublished sale coin sends no intent");
    fixture::captured = false;
    check(OnCollectVerb(press) == sg::Verdict::Run, "map coin retains native collection");
    fixture::mirrored = true;
    fixture::session.side = coop::net::Role::Host;
    check(OnCollectVerb(press) == sg::Verdict::Run, "host collection remains native");
    fixture::session.side = coop::net::Role::Client;
    check(OnCollectVerb({&body, true}) == sg::Verdict::Run, "host-dispatched echo runs native verb");
    fixture::coin = false;
    check(OnCollectVerb(press) == sg::Verdict::Run, "other interaction classes are untouched");
    fixture::coin = true;
    fixture::active = nullptr;
    check(OnCollectVerb(press) == sg::Verdict::Run, "solo collection remains native");
    return failures ? 1 : 0;
}
