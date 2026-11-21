// coop/props/prop_act_families.h -- the families of prop_act beyond its own three verbs: a client's verb with a held item on
// a shared target, asked of the host, granted, replayed, committed, judged and applied per family.
//
// A family is one entry the file that owns the verb registers (coop/props/prop_act_omega.cpp is the first): the wire byte
// it answers to, the element type its target is (a prop, an NPC), its watches on the client's verb, how a granted client
// replays the verb and what its commit says, and on the host its judge and when its result is written (at the grant, or at
// the client's commit). prop_act_request and prop_act_host reach every family through this table and name none of them, so a
// new family is a new file with an entry, a wire byte in protocol.h and a line in the source list, and nothing else in
// prop_act changes. The verb's argument rides the request in a family-neutral field (PropActRequestPayload::verb): the
// Omega's family puts a list_kerfurDrip row there. Game thread.

#pragma once

#include "coop/element/element.h"
#include "coop/net/protocol.h"
#include "coop/props/prop_act_host.h"

#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace coop::net { class Session; }

namespace coop::prop_act_families {

using Verb = char[coop::net::kPropActVerbBytes];

// One watch on the client's verb: the body is cancelled at its PRE and asked of the host. `verb` names the act for the line
// a dead watch prints, `dead` says what a dead watch costs.
struct WatchDef {
    const ue_wrap::facts::Function& row;
    int tag;
    ue_wrap::script_gate::PreFn pre;
    const char* verb;
    const char* dead;
};

// When the host writes the family's result: at the client's commit, after its replay ran; or at the grant, with no replay
// and no commit (the host runs the verb itself, and the reservation ends there).
enum class Apply : uint8_t { AtCommit, AtGrant };

struct Family {
    uint8_t id;                    // the PropActFamily byte this entry answers to
    const char* name;
    coop::element::ElementType target;   // the element type the request's eid names
    Apply apply;
    // The client's watches on this verb (the family's own PREs ask through prop_act_request::Ask).
    const WatchDef* watches;
    size_t watchCount;
    // CLIENT, a granted request that replays (apply AtCommit): the verb as the player runs it, under prop_act's latch, and
    // the commit word after it (1 or 0; the target is never destroyed). Null for apply AtGrant.
    bool (*runVerb)(void* target, void* local, const Verb& verb);
    uint8_t (*commitWord)(void* target, const Verb& verb);
    // HOST: the family's own conditions after prop_act's live, hold and reach steps: the term that refuses the request,
    // none when it fits. May record what the apply needs for the slot until onRelease.
    std::optional<coop::prop_act_host::Tally> (*judge)(coop::net::Session& session, uint8_t slot,
                                                       const coop::net::PropActRequestPayload& req, void* target);
    // HOST apply AtGrant: the result, written before the grant is answered; false when it could not be written, and then the
    // request is refused. Null for apply AtCommit.
    bool (*applyAtGrant)(uint8_t slot, void* target);
    // HOST apply AtCommit: the result, written at the client's commit of 1; the term that refused it, none when written.
    std::optional<coop::prop_act_host::Tally> (*applyAtCommit)(uint8_t slot, void* target);
    // HOST: the client's commit was 0: put its copy back.
    void (*aborted)(uint8_t slot, void* target);
    void (*onRelease)(uint8_t slot);     // the slot's reservation ended
    void (*onPeerGone)(uint8_t slot);    // the slot left
    void (*onDisconnect)();              // the session ended
};

// Registers an entry at static initialisation. A file's entries are registered by one `static const Registrar` each; the
// table holds kMaxFamilies, and an entry past it, a second entry for one byte, or one whose apply mode lacks a pointer it
// needs (the judge always; runVerb, commitWord, applyAtCommit and aborted for AtCommit; applyAtGrant for AtGrant) is
// refused and said.
class Registrar {
public:
    explicit Registrar(const Family& family);
};
inline constexpr size_t kMaxFamilies = 16;

// The entry for a wire byte, or null (the three prop verbs are not in the table).
const Family* Find(uint8_t family);

// Every registered watch, in registration order.
size_t WatchCount();
const WatchDef& WatchAt(size_t i);

// The fan-outs of the host's slot events.
void OnRelease(uint8_t slot);
void OnPeerGone(uint8_t slot);
void OnDisconnect();

// CLIENT, for a family's PRE: whether `actor` is the shared mirror of an element of `type` (its eid returned), so the
// client's verb on it is the host's to grant. A copy the host does not own (the client's own) is left to the game.
bool SharedTarget(void* actor, coop::element::ElementType type, coop::element::ElementId& eid);

}  // namespace coop::prop_act_families
