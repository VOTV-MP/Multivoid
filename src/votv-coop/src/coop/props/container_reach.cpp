// coop/props/container_reach.cpp -- see coop/props/container_reach.h.

#include "coop/props/container_reach.h"

#include "coop/element/element.h"
#include "coop/net/session.h"
#include "ue_wrap/actors/container_openers.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <set>
#include <utility>

namespace coop::props::container_reach {
namespace {

// A container the game opens through another actor stands where no player does -- the drone's and the
// ATV's, tens to hundreds of metres from them -- and its author reaches it through the actor that opens
// it: the game's own field on the opener, never the author's claim.
void* OpenerInReach(const coop::element::IntentTarget& tok, void* container) {
    struct Ctx {
        const coop::element::IntentTarget* tok;
        void* reached;
    } ctx{&tok, nullptr};
    ue_wrap::container_openers::ForEach(container, [](void* c, void* opener) {
        Ctx& x = *static_cast<Ctx*>(c);
        if (x.tok->Authorize(opener).outcome != coop::element::IntentOutcome::Ok) return true;
        x.reached = opener;
        return false;
    }, &ctx);
    return ctx.reached;
}

// Said once per container and author, the first time an author reaches it through an opener.
std::set<std::pair<uint32_t, uint8_t>> g_reachedThrough;

}  // namespace

ReachResult Judge(coop::net::Session& session, uint8_t authorSlot, uint32_t eid) {
    // REACH, and only where it is answerable. NoRow and StaleDead are not refusals here: they mean
    // the element has not arrived (or has gone), which is the hold's question, not this one.
    const auto tok = coop::element::IntentTarget::ForClientIntent(session, authorSlot, kReachUU);
    const auto sub = tok.Resolve(static_cast<coop::element::ElementId>(eid),
                                 coop::element::ElementType::Prop);
    if (sub.outcome == coop::element::IntentOutcome::NoBody)
        return {Reach::NoBody, sub.outcome, sub.distUU, sub.reachUU};
    // Far, or with no place to measure to: the author may still stand at an actor that opens it.
    const bool unmet = sub.outcome == coop::element::IntentOutcome::OutOfReach ||
                       sub.outcome == coop::element::IntentOutcome::NoTarget;
    void* const opener = unmet ? OpenerInReach(tok, sub.actor) : nullptr;
    if (opener && g_reachedThrough.insert({eid, authorSlot}).second)
        UE_LOGI("container_contents: eid=%u slot %u reached through the %ls that opens it (the container: "
                "%s, %.0f uu away)", eid, static_cast<unsigned>(authorSlot),
                ue_wrap::reflection::ClassNameOf(opener).c_str(), coop::element::OutcomeName(sub.outcome),
                sub.distUU);
    if (unmet && !opener) return {Reach::Unreachable, sub.outcome, sub.distUU, sub.reachUU};
    return {Reach::Reached, sub.outcome, sub.distUU, sub.reachUU};
}

void Reset() {
    g_reachedThrough.clear();
}

}  // namespace coop::props::container_reach
