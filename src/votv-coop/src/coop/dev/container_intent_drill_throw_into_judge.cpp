// coop/dev/container_intent_drill_throw_into_judge.cpp -- the throwinto arm's judgement of one try: the host's machine
// (container_intent_drill_throw_into_host.cpp) hands it the two reads and the deltas, and prints the verdict line itself.

#include "container_intent_drill_throw_into_judge.h"

#include <cstdarg>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

Outcome Bad(const char* fmt, ...) {
    Outcome o;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(o.why, sizeof(o.why), fmt, ap);
    va_end(ap);
    return o;
}

}  // namespace

bool ReadEnd(const std::string& s, ClientEnd& c) {
    return Num(s, "seq", c.seq) && Num(s, "c0", c.c0) && Num(s, "gone", c.gone) && IntsAfter(s, "rest=", c.rest, 3);
}

bool ReadLate(const std::string& s, ClientEnd& c) {
    Num(s, "cpre", c.cpre);   // the arming measure's order fields: 0 when the client names none
    Num(s, "cdes", c.cdes);
    Num(s, "gr", c.grab);
    Num(s, "sa", c.sa);
    Num(s, "se", c.se);
    Num(s, "sk", c.sk);
    Num(s, "fwh", c.fwh);
    return Num(s, "c1", c.c1) && Num(s, "stray", c.stray) && Num(s, "ov", c.ov) && Num(s, "adds", c.adds) &&
           Num(s, "addms", c.addMs) && Num(s, "alive", c.alive);
}

bool ReadObserver(const std::string& s, ClientEnd& c) {
    return Num(s, "c0", c.obs0) && Num(s, "c1", c.obs1) && Num(s, "stray", c.obsStray);
}

Outcome Judge(const JudgeIn& in, const Frozen& f, const ClientEnd& c) {
    Outcome o;
    const int dh = in.dh, dc = in.dc;
    if (f.alive && !f.settled)
        return Bad("the host's copy never came to rest (still moving %llu s after the client's end line)", U(kHostSettleMs / 1000));
    if (dh == 0 && dc == 0 && f.alive && f.restCm > kHitCm) {
        o.miss = true;
        return o;
    }
    if (in.c0 != 0 || c.c0 != in.c0)
        return Bad("baselines differ (host starts at %d, client at %lld, 0 expected)", in.c0, c.c0);
    if (ThrownRun(in.r) && c.seq == 0) return Bad("the throw carried no sequence (seq=0)");
    if (ThrownRun(in.r) && in.intThrown == 0)
        return Bad("the drop intent carried no throw sequence (%u intent(s) seen, none numbered)", in.intSeen);
    if (ThrownRun(in.r) && !in.elected) return Bad("the throw was not elected (no HOST ELECT for it)");
    if (HeldRun(in.r) && (in.relThrown != 0 || in.elected || c.seq != 0)) return Bad("the control carried a throw sequence or was elected");
    if (dh >= 2) return Bad("a duplicate (host +%d)", dh);
    if (dh == 0 && !f.alive && !c.alive)
        return Bad("the item was lost (host +0, the prop gone; %s)", f.hadElem ? "the host's element is gone" : "no element ever reached the host");
    if ((dh == 1 || dc == 1) && (f.strayH > 0 || c.stray > 0))
        return Bad("a stray copy (host %+d, client %+d, a live prop on the %s)", dh, dc,
                   f.strayH > 0 && c.stray > 0 ? "host and the client" : f.strayH > 0 ? "host" : "client");
    if (dh != dc) return Bad("the client's mirror diverged (host %+d, client %+d)", dh, dc);
    if (in.r == 5 && dh == 1) {   // two clients: the first one's mirror is judged beside the second's
        if (c.obs0 != in.c0 || c.obs1 - c.obs0 != 1)
            return Bad("the first client's mirror diverged (host %+d, first client %lld -> %lld)", dh, c.obs0, c.obs1);
        if (c.obsStray > 0) return Bad("a stray copy on the first client (%lld live)", c.obsStray);
    }
    if (dh == 1 && in.r == 3 && c.grab) return Bad("the client's grab did not end when the prop died (its grabbing_actor is still set)");
    if (dh == 1) return o;
    if (dh == 0) return Bad("the item was not recorded (host +0, the prop rests %.0f cm from the basket)", static_cast<double>(f.restCm));
    return Bad("the count changed by neither 0 nor +1 (host %+d, client %+d)", dh, dc);
}

}  // namespace coop::dev::container_intent_drill::detail::into
