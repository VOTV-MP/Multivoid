// coop/commands/command_dispatcher.cpp -- Dispatch and the built-in /help.
//
// The order of a dispatch: split, find the root (an alias becomes its expansion), walk to the
// sub-verb (taking the target of each targeted parent on the way), refuse a console-only verb to
// anyone else, ask the policy about the reached verb's node, parse and resolve the effective
// arguments (the targets first), ask about the selector node when `@a` or `@r` was used,
// apply the qualifiers (the host as a target, an offline target and whether the host knows its id,
// an exempt target, who is told),
// run the handler. Each refusal is one reply line and sets no `ran`.

#include "coop/commands/command_dispatcher.h"

#include "coop/commands/command_args.h"
#include "coop/commands/command_line.h"
#include "coop/permissions/gameplay_nodes.h"
#include "coop/permissions/grants_core.h"

#include <algorithm>
#include <cstddef>

namespace coop::commands {

namespace {

constexpr size_t kPresetOffset = static_cast<size_t>(-1);
constexpr const char* kSelectorNode = "multivoid.command.selector";

// The typed line and its effective words: for an alias, [root name] + the preset words + the
// words typed after the alias. A preset word has no offset in the typed line.
struct Expanded {
    ParsedLine typed;
    std::vector<std::string> words;
    std::vector<size_t> begins;  // kPresetOffset for a preset word
    size_t typedStart = 1;       // index in `words` of the first word typed after the alias
};

// The root the line's first word names, or null (unknown, or no word).
const CommandSpec* Expand(const Registry& reg, std::string_view line, Expanded& ex) {
    ex.typed = SplitLine(line);
    if (ex.typed.words.empty()) return nullptr;
    const Alias* alias = nullptr;
    const CommandSpec* root = reg.FindRoot(ex.typed.words[0], &alias);
    if (root == nullptr) return nullptr;
    if (alias == nullptr) {
        ex.words = ex.typed.words;
        ex.begins = ex.typed.begins;
        ex.typedStart = 1;
        return root;
    }
    ex.words.push_back(root->name);
    ex.begins.push_back(ex.typed.begins[0]);
    const ParsedLine preset = SplitLine(alias->presetWords);
    for (const std::string& w : preset.words) {
        ex.words.push_back(w);
        ex.begins.push_back(kPresetOffset);
    }
    ex.typedStart = ex.words.size();
    for (size_t i = 1; i < ex.typed.words.size(); ++i) {
        ex.words.push_back(ex.typed.words[i]);
        ex.begins.push_back(ex.typed.begins[i]);
    }
    return root;
}

struct Walked {
    const CommandSpec* spec;
    size_t next;                 // the first effective word the walk did not consume
    std::vector<size_t> pivots;  // indices into the words of the targeted parents' targets, in order
};

// While the next word equals (ASCII, case-insensitive) a sub-verb of the current spec, descend. At
// a targeted parent the word at `next` is its target and the word after it names the sub-verb;
// the walk goes on only when both exist and the sub-verb matches, else it stops AT the parent.
Walked Walk(const CommandSpec* root, const Expanded& ex) {
    Walked w{root, 1, {}};
    while (w.next < ex.words.size()) {
        const bool targeted = w.spec->Targeted();
        const size_t verbAt = targeted ? w.next + 1 : w.next;
        if (verbAt >= ex.words.size()) break;
        const CommandSpec* hit = nullptr;
        for (const CommandSpec& sub : w.spec->subVerbs)
            if (EqualsAsciiNoCase(ex.words[verbAt], sub.name)) { hit = &sub; break; }
        if (hit == nullptr) break;
        if (targeted) w.pivots.push_back(w.next);
        w.spec = hit;
        w.next = verbAt + 1;
    }
    return w;
}

std::string TrimRight(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') --n;
    return std::string(s.substr(0, n));
}

// The raw text from effective word `at` to the end of the line. A word typed in the line takes
// the line from its offset; a preset word has none, so the preset words from there are joined by
// one space, then one space and the raw typed remainder after the alias.
std::string RawRest(const Expanded& ex, size_t at, std::string_view line) {
    if (ex.begins[at] != kPresetOffset) return TrimRight(line.substr(ex.begins[at]));
    std::string out;
    for (size_t i = at; i < ex.typedStart; ++i) {
        if (!out.empty()) out += ' ';
        out += ex.words[i];
    }
    if (ex.typedStart < ex.words.size()) {
        out += ' ';
        out += line.substr(ex.begins[ex.typedStart]);
        return TrimRight(out);
    }
    return out;
}

bool Passes(const Registry& reg, const Policy& policy, const Caller& caller, std::string_view node) {
    if (policy.check == nullptr) return false;
    const NodeDecl* d = reg.FindNode(node);
    return policy.check(caller, node, d != nullptr && d->defaultGranted);
}

DispatchResult Said(std::string line) {
    DispatchResult r;
    r.replies.push_back(std::move(line));
    return r;
}

const Qualifier* FindQualifier(const CommandSpec& spec, QualKind kind) {
    for (const Qualifier& q : spec.qualifiers)
        if (q.kind == kind) return &q;
    return nullptr;
}

std::string NickOf(const std::vector<PlayerView>& players, int slot) {
    for (const PlayerView& p : players)
        if (p.slot == slot) return p.nick;
    return std::string();
}

// A player id as a person reads it: its first eight characters.
std::string ShortId(const std::string& id) { return id.substr(0, 8); }

// The qualifier steps over the resolved targets, before the handler: the host as a target, an
// offline target, an exempt target, then who is told. Returns the refusal line, or empty when the
// handler may run (`ctx.notifySlots` is then filled). The offline, exempt and identity steps judge
// the one Player / PlayerOrId argument of the effective list (`ctx.args`), when it was given.
std::string ApplyQualifiers(Context& ctx) {
    const CommandSpec& spec = ctx.spec;
    const Caller& caller = ctx.caller;
    const Policy& policy = ctx.policy;
    const std::string node = ctx.registry.NodeOf(spec);

    // Register refuses a GateOffline or Exempt qualifier on a spec without exactly one such
    // argument, and a PlayerOrId argument on a spec without GateOffline.
    size_t at = ctx.args.size();
    for (size_t i = 0; i < ctx.args.size(); ++i) {
        const ArgKind kind = ctx.args[i]->kind;
        if (kind == ArgKind::Player || kind == ArgKind::PlayerOrId) at = i;
    }

    for (size_t i = 0; i < ctx.args.size(); ++i) {
        if (!ctx.given[i] || !ctx.args[i]->notHost) continue;
        for (int slot : ctx.targets[i].slots)
            if (slot == 0) return "That is the host -- it cannot be " + spec.pastTense + ".";
    }

    if (at != ctx.args.size() && ctx.given[at]) {
        const TargetResult& t = ctx.targets[at];
        const Qualifier* gate = FindQualifier(spec, QualKind::GateOffline);
        if (gate != nullptr && t.offline) {
            const std::string offlineNode = node + "." + gate->name;
            if (!Passes(ctx.registry, policy, caller, offlineNode))
                return "You do not have permission for /" + ctx.registry.PathOf(spec) +
                       " on an offline player (" + offlineNode + ").";
            // An id the host never saw is the console's to act on: a caller holding the offline
            // node could otherwise add a persisted record per line at the bucket rate.
            if (!caller.isOperator && (policy.known == nullptr || !policy.known(t.offlineId)))
                return ShortId(t.offlineId) +
                       " has never played here; only the host can act on an unknown id.";
        }

        const Qualifier* exempt = FindQualifier(spec, QualKind::Exempt);
        if (exempt != nullptr && !caller.isOperator) {
            const std::string exemptNode = node + "." + exempt->name;
            for (size_t k = 0; k < t.slots.size(); ++k) {
                const std::string& id = t.playerIds[k];
                const std::string nick = NickOf(ctx.players, t.slots[k]);
                if (id.empty() || policy.isExplicit == nullptr)
                    return nick + "'s identity is not proved yet.";
                if (policy.isExplicit(id, exemptNode))
                    return nick + " cannot be " + spec.pastTense + ".";
            }
            if (t.offline) {
                if (policy.isExplicit == nullptr)
                    return ShortId(t.offlineId) + "'s identity is not proved yet.";
                if (policy.isExplicit(t.offlineId, exemptNode))
                    return ShortId(t.offlineId) + " cannot be " + spec.pastTense + ".";
            }
        }
    }

    const Qualifier* notify = FindQualifier(spec, QualKind::Notify);
    if (notify != nullptr && policy.holds != nullptr) {
        const std::string notifyNode = notify->node.empty() ? node + "." + notify->name : notify->node;
        for (const PlayerView& p : ctx.players)
            if (p.slot > 0 && p.slot != caller.slot && !p.playerId.empty() &&
                policy.holds(p.playerId, notifyNode, false))
                ctx.notifySlots.push_back(p.slot);
        std::sort(ctx.notifySlots.begin(), ctx.notifySlots.end());
    }
    return std::string();
}

void HelpHandler(Context& ctx) {
    if (!ctx.given[0]) {
        ctx.Reply("Commands you can use:");
        for (const CommandSpec* v : ctx.registry.AllVerbs()) {
            if (v->consoleOnly && !ctx.caller.isOperator) continue;
            if (!Passes(ctx.registry, ctx.policy, ctx.caller, ctx.registry.NodeOf(*v))) continue;
            ctx.Reply(ctx.registry.Usage(*v) + " -- " + v->description);
        }
        return;
    }
    Expanded ex;
    const CommandSpec* root = Expand(ctx.registry, ctx.texts[0], ex);
    if (root == nullptr) {
        const std::string first = ex.typed.words.empty() ? std::string() : ex.typed.words[0];
        ctx.Reply("Unknown command '/" + first + "'.");
        return;
    }
    const Walked w = Walk(root, ex);
    ctx.Reply(ctx.registry.Usage(*w.spec) + " -- " + w.spec->description);
}

}  // namespace

DispatchResult Dispatch(const Registry& reg, const Caller& caller, std::string_view line,
                        const std::vector<PlayerView>& players, const Policy& policy) {
    Expanded ex;
    const CommandSpec* root = Expand(reg, line, ex);
    if (ex.typed.words.empty()) return Said(std::string(kHelpHint));
    if (root == nullptr)
        return Said("Unknown command '/" + ex.typed.words[0] + "'. Type /help for the commands.");

    const Walked walked = Walk(root, ex);
    const CommandSpec& spec = *walked.spec;
    if (spec.handler == nullptr) return Said("Usage: " + reg.Usage(spec));

    if (spec.consoleOnly && !caller.isOperator)
        return Said("/" + reg.PathOf(spec) + " is the host's until /tp can name a destination.");

    const std::string node = reg.NodeOf(spec);
    if (!Passes(reg, policy, caller, node))
        return Said("You do not have permission for /" + reg.PathOf(spec) + " (" + node + ").");

    Context ctx{.caller = caller,
                .spec = spec,
                .players = players,
                .registry = reg,
                .policy = policy,
                .args = reg.ArgsOf(spec),
                .targets = {},
                .integers = {},
                .texts = {},
                .given = {},
                .booleans = {},
                .contexts = {},
                .replies = {},
                .notifySlots = {}};
    const size_t argCount = ctx.args.size();
    ctx.targets.resize(argCount);
    ctx.integers.assign(argCount, 0);
    ctx.texts.resize(argCount);
    ctx.given.assign(argCount, false);
    ctx.booleans.assign(argCount, false);

    // The first `pivotCount` arguments are the targeted parents' words, always given: a target
    // names ONE player, whose id must be proved, since the leaf below keys a store by it.
    const size_t pivotCount = walked.pivots.size();
    bool usedSelector = false;
    // One argument's word, parsed and resolved into its slot of the context. True when the word is
    // taken. False with `refusal` empty when an optional Boolean or Duration LOOKS AHEAD and the
    // word is not its kind: the argument is absent and the word stays for the next one (`/mute
    // <who> [duration] [reason]` reads `/mute Bob spamming` as no duration). False with a
    // `refusal` line when the word is wrong.
    auto parseWord = [&](size_t i, const std::string& word, std::string& refusal) -> bool {
        const ArgSpec& a = *ctx.args[i];
        if (a.kind == ArgKind::Boolean) {
            bool value = false;
            if (!ParseBoolean(word, &value)) {
                if (!a.optional)
                    refusal = "'" + word + "' is not true or false. Usage: " + reg.Usage(spec);
                return false;
            }
            ctx.booleans[i] = value;
        } else if (a.kind == ArgKind::Duration) {
            if (policy.nowSeconds == nullptr) {
                refusal = "The clock is not available.";
                return false;
            }
            switch (ParseDuration(word, policy.nowSeconds(), &ctx.integers[i])) {
                case DurationError::None: break;
                case DurationError::NotADuration:
                    if (!a.optional)
                        refusal = "'" + word + "' is not a duration (like 30m, 2h or 1d12h). Usage: " +
                                  reg.Usage(spec);
                    return false;
                case DurationError::Zero:
                    refusal = "'" + word + "' is no time at all.";
                    return false;
                case DurationError::Passed:
                    refusal = "'" + word +
                              "' has already passed (a bare number is a moment in epoch seconds; "
                              "for a length write 30m).";
                    return false;
                case DurationError::TooFar:
                    refusal = "'" + word + "' is more than 100 years away.";
                    return false;
            }
        } else if (a.kind == ArgKind::Integer) {
            if (!ParseInteger(word, &ctx.integers[i])) {
                refusal = "'" + word + "' is not a whole number. Usage: " + reg.Usage(spec);
                return false;
            }
        } else if (a.kind == ArgKind::Player || a.kind == ArgKind::PlayerOrId ||
                   a.kind == ArgKind::Players) {
            if (i < pivotCount && !word.empty() && word[0] == '@') {
                refusal = "Name one player, not @a, @p, @r or @s.";
                return false;
            }
            TargetResult r = ResolveTarget(word, a.kind != ArgKind::Players,
                                           a.kind == ArgKind::PlayerOrId, caller, players, policy.pick);
            if (r.error != TargetError::None) {
                refusal = DescribeTargetError(r, word, players);
                return false;
            }
            if (i < pivotCount && !r.slots.empty() && r.playerIds[0].empty()) {
                refusal = NickOf(players, r.slots[0]) + "'s identity is not proved yet.";
                return false;
            }
            usedSelector = usedSelector || r.usedSelector;
            ctx.targets[i] = std::move(r);
        }
        ctx.given[i] = true;
        ctx.texts[i] = word;
        return true;
    };

    size_t next = walked.next;
    for (size_t i = 0; i < argCount; ++i) {
        const ArgSpec& a = *ctx.args[i];
        const bool pivot = i < pivotCount;
        if (!pivot && next >= ex.words.size()) {
            if (a.optional) continue;
            return Said("Usage: " + reg.Usage(spec));
        }
        if (!pivot && a.kind == ArgKind::Rest) {
            ctx.given[i] = true;
            ctx.texts[i] = RawRest(ex, next, line);
            next = ex.words.size();
            continue;
        }
        if (!pivot && a.kind == ArgKind::Contexts) {
            for (size_t k = next; k < ex.words.size(); ++k) {
                const std::string& pair = ex.words[k];
                const size_t eq = pair.find('=');
                if (eq == std::string::npos || eq == 0 || eq + 1 == pair.size())
                    return Said("'" + pair + "' is not key=value. Usage: " + reg.Usage(spec));
                if (!ctx.contexts.Add(std::string_view(pair).substr(0, eq),
                                      std::string_view(pair).substr(eq + 1)))
                    return Said("'" + pair + "' is not a valid context.");
            }
            ctx.given[i] = true;
            ctx.texts[i] = RawRest(ex, next, line);
            next = ex.words.size();
            continue;
        }
        std::string refusal;
        const bool taken = parseWord(i, pivot ? ex.words[walked.pivots[i]] : ex.words[next], refusal);
        if (!refusal.empty()) return Said(refusal);
        if (!pivot && taken) ++next;
    }
    if (next < ex.words.size()) return Said("Usage: " + reg.Usage(spec));

    if (usedSelector && !Passes(reg, policy, caller, kSelectorNode))
        return Said(std::string("You do not have permission to use @a or @r (") + kSelectorNode + ").");

    const std::string refusal = ApplyQualifiers(ctx);
    if (!refusal.empty()) return Said(refusal);

    spec.handler(ctx);
    DispatchResult out;
    out.ran = true;
    out.replies = std::move(ctx.replies);
    return out;
}

bool RegisterBuiltins(Registry& reg) {
    if (!reg.DeclareNode({kSelectorNode, false, "Use @a and @r in a command's target."}, nullptr))
        return false;
    // The nodes a machine shows locally (the dev features): declared from the core's table, the
    // one place that names them.
    for (const coop::permissions::grants::Entry& entry : coop::permissions::grants::kProjected) {
        if (!reg.DeclareNode({entry.node, entry.defaultGranted, entry.description}, nullptr))
            return false;
    }
    // The nodes a player's acts in the shared world answer to: the gameplay domain's own table.
    if (!coop::permissions::gameplay_nodes::Install(reg)) return false;
    // The node a permission change's Notify qualifier names: told of every change.
    if (!reg.DeclareNode({kAdminLogNode, false, "Told of every permission change."}, nullptr))
        return false;
    CommandSpec help;
    help.name = "help";
    help.defaultGranted = true;
    help.description = "Lists the commands you can use, or shows how to use one.";
    help.args = {{"command", ArgKind::Rest, true}};
    help.handler = &HelpHandler;
    return reg.Register(std::move(help), nullptr);
}

}  // namespace coop::commands
