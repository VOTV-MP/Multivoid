#include <cstdint>
#include <cstdio>

namespace fixture {
bool sendOk = false, readable = true, writable = true;
int attempts = 0, delivered = 0, hud = 0;
int32_t points = 100, received = 0;
}
namespace coop::net {
enum class Role { Host, Client };
enum class ReliableKind { BalanceSync };
struct BalancePayload { int32_t total; };
struct Session {
    Role side = Role::Host;
    bool live = true;
    bool connected() const { return live; }
    Role role() const { return side; }
    bool SendReliable(ReliableKind, const void* data, int) {
        ++fixture::attempts;
        if (!fixture::sendOk) return false;
        ++fixture::delivered;
        fixture::received = static_cast<const BalancePayload*>(data)->total;
        return true;
    }
    bool SendReliableToSlot(int, ReliableKind kind, const void* data, int size) {
        return SendReliable(kind, data, size);
    }
};
}
namespace ue_wrap::economy {
bool ReadPoints(int32_t* value) { *value = fixture::points; return fixture::readable; }
bool WritePoints(int32_t value) {
    if (!fixture::writable) return false;
    fixture::points = value;
    return true;
}
void RefreshPointsHud() { ++fixture::hud; }
void AddPoints(int32_t amount) { fixture::points += amount; }
}
namespace ue_wrap::game_thread {
template<class F> void Post(F callback) { callback(); }
}
#define UE_LOGI(...) ((void)0)
#define UE_LOGW(...) ((void)0)
#include BALANCE_SYNC_SOURCE

int main() {
    using namespace fixture;
    namespace B = coop::balance_sync;
    int checks = 0;
    auto check = [&](bool ok, const char* message) {
        ++checks;
        if (!ok) std::fprintf(stderr, "FAIL: %s\n", message);
        return ok;
    };
    coop::net::Session session;
    B::SetSession(&session);
    B::OnDisconnect();
    B::Tick();
    if (!check(attempts == 1 && delivered == 0, "initial broadcast refused")) return 1;
    sendOk = true;
    B::Tick();
    if (!check(attempts == 2 && delivered == 1 && received == 100,
               "unchanged initial balance retries after refusal")) return 1;
    B::Tick();
    if (!check(attempts == 2, "accepted balance is deduplicated")) return 1;
    points = -25;
    sendOk = false;
    B::Tick();
    B::Tick();
    if (!check(attempts == 4 && delivered == 1, "changed balance keeps retrying")) return 1;
    points = 0;
    sendOk = true;
    B::Tick();
    if (!check(delivered == 2 && received == 0, "retry sends latest balance")) return 1;
    B::Tick();
    if (!check(attempts == 5, "zero balance is deduplicated")) return 1;
    B::OnDisconnect();
    B::Tick();
    if (!check(attempts == 6, "session reset republishes unchanged balance")) return 1;
    readable = false;
    points = 500;
    B::Tick();
    if (!check(attempts == 6, "unresolved balance is not published")) return 1;
    readable = true;
    session.side = coop::net::Role::Client;
    writable = false;
    B::ApplyFromHost(42);
    B::Tick();
    if (!check(points == 500 && hud == 0, "client waits for writable save")) return 1;
    writable = true;
    B::Tick();
    B::Tick();
    if (!check(points == 42 && hud == 1 && attempts == 6,
               "client applies pending balance once without broadcasting")) return 1;
    B::SetSession(nullptr);
    std::printf("balance sync: %d checks passed\n", checks);
}
