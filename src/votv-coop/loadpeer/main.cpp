// loadpeer/main.cpp -- the net library's closure probe (target coop-net-closure, dev only, never
// shipped). It starts a session as a client and stops it, and links against coop-net ALONE: the link
// succeeding, with no stub file beside it, is the proof that the session and everything it pulls
// stand without the game. It is also the seed of the load peer (a process that joins a host and
// holds a slot without a game).

#include "coop/net/session.h"

int main() {
    coop::net::Session session;
    coop::net::Config cfg;
    cfg.role = coop::net::Role::Client;
    coop::net::Refusal why{};
    const bool ok = session.Start(cfg, &why);
    session.Stop();
    return ok ? 0 : 1;
}
