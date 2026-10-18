// coop/commands/command_registry.cpp -- the command tree, its node declarations, and the rules a
// tree must satisfy before the registry takes it.
//
// Shapes: LuckPerms' command tree with a node per sub-command (`/lp user <p> permission set`) and
// EssentialsX's base node derived from the command's name, `essentials.<name>`
// (reference/EssentialsX/Essentials/src/main/java/com/earth2me/essentials/User.java:133-135);
// MTA keeps one command table whose entries carry their default into the single check
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CConsoleCommand.h:16-35).

#include "coop/commands/command_registry.h"

#include "coop/commands/command_line.h"

#include <algorithm>
#include <set>

namespace coop::commands {

namespace {

constexpr size_t kMaxNameLen = 32;

bool ValidName(std::string_view s) {
    if (s.empty() || s.size() > kMaxNameLen) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// Words that name nodes which are not commands (`multivoid.printer.*`, `multivoid.admin.menu`,
// `multivoid.command.selector`, `multivoid.gameplay.*`, `multivoid.dev.*`): no command may take them.
bool Reserved(std::string_view name) {
    return name == "printer" || name == "content" || name == "admin" || name == "command" || name == "gameplay" ||
           name == "dev";
}

using NodeMap = std::map<std::string, NodeDecl, std::less<>>;
using QualifierNodeMap = std::map<std::string, QualKind>;
using PlainNodeSet = std::set<std::string, std::less<>>;

// One walk over a tree that is not yet taken: the first broken rule stops it with a reason.
struct TreeCheck {
    const NodeMap& declared;
    const QualifierNodeMap& declaredQualifiers;  // the qualifier nodes among `declared`
    const PlainNodeSet& plainNodes;  // the nodes among `declared` that DeclareNode declared
    std::set<std::string> byThisTree;  // nodes declared by specs earlier in depth-first order
    QualifierNodeMap qualifiersByThisTree;  // the qualifier nodes among byThisTree
    std::string why;

    bool Fail(std::string reason) {
        why = std::move(reason);
        return false;
    }

    bool IsDeclared(const std::string& node) const {
        return declared.find(node) != declared.end() || byThisTree.count(node) != 0;
    }

    // The kind that declared a qualifier node, or null when the node is not a qualifier node.
    const QualKind* QualifierKindOf(const std::string& node) const {
        const auto d = declaredQualifiers.find(node);
        if (d != declaredQualifiers.end()) return &d->second;
        const auto t = qualifiersByThisTree.find(node);
        return t != qualifiersByThisTree.end() ? &t->second : nullptr;
    }

    // The qualifiers of one spec against its effective arguments (the targeted ancestors' and its
    // own): a valid name each, one qualifier per kind and per name, and the target-argument rules
    // the dispatcher relies on. Exempt and GateOffline are judged on the one Player / PlayerOrId
    // argument; GateOffline gates only a PlayerOrId.
    bool Qualifiers(const CommandSpec& s, const std::vector<const ArgSpec*>& effective) {
        size_t targets = 0;
        ArgKind targetKind = ArgKind::Word;
        bool orId = false;
        for (const ArgSpec* a : effective) {
            if (a->kind == ArgKind::Player || a->kind == ArgKind::PlayerOrId ||
                a->kind == ArgKind::Players) {
                ++targets;
                targetKind = a->kind;
            }
            orId = orId || a->kind == ArgKind::PlayerOrId;
        }
        bool gates = false;
        for (size_t i = 0; i < s.qualifiers.size(); ++i) {
            const Qualifier& q = s.qualifiers[i];
            if (!ValidName(q.name))
                return Fail("'" + s.name + "': the qualifier '" + q.name + "' is not 1..32 of [a-z0-9]");
            for (size_t j = 0; j < i; ++j) {
                if (s.qualifiers[j].kind == q.kind)
                    return Fail("'" + s.name + "': two qualifiers of one kind");
                if (s.qualifiers[j].name == q.name)
                    return Fail("'" + s.name + "': two qualifiers named '" + q.name + "'");
            }
            if (!q.node.empty()) {
                if (q.kind != QualKind::Notify)
                    return Fail("'" + s.name + "': only a Notify qualifier names a node");
                // One test for every node that is not a plain one: a command's, a qualifier's, an
                // undeclared one, the spec's own derived one.
                if (plainNodes.find(q.node) == plainNodes.end())
                    return Fail("'" + s.name + "': the notify node '" + q.node +
                                "' is not a declared non-command node");
            }
            if (q.kind != QualKind::GateOffline && q.kind != QualKind::Exempt) continue;
            if (targets != 1 || targetKind == ArgKind::Players)
                return Fail("'" + s.name + "': the qualifier '" + q.name +
                            "' needs exactly one Player or PlayerOrId argument");
            if (q.kind == QualKind::GateOffline) {
                if (targetKind != ArgKind::PlayerOrId)
                    return Fail("'" + s.name + "': the qualifier '" + q.name +
                                "' needs a PlayerOrId argument, a Player is never offline");
                gates = true;
            }
        }
        if (orId && !gates)
            return Fail("'" + s.name + "': a PlayerOrId argument needs a GateOffline qualifier");
        return true;
    }

    // `inherited` is the targeted ancestors' arguments, root first.
    bool Spec(const CommandSpec& s, const std::string& parentNode, bool isRoot,
              const std::vector<const ArgSpec*>& inherited) {
        if (!ValidName(s.name)) return Fail("'" + s.name + "' is not 1..32 of [a-z0-9]");
        if (isRoot && Reserved(s.name)) return Fail("'" + s.name + "' is a reserved word");
        if (!isRoot && !s.aliases.empty()) return Fail("'" + s.name + "': an alias on a sub-verb");
        if (s.Targeted() && !s.aliases.empty())
            return Fail("'" + s.name + "': an alias on a spec that takes a target");
        for (const Alias& a : s.aliases) {
            if (!ValidName(a.name)) return Fail("alias '" + a.name + "' is not 1..32 of [a-z0-9]");
            if (!s.subVerbs.empty()) {
                const ParsedLine preset = SplitLine(a.presetWords);
                bool names = false;
                if (!preset.words.empty())
                    for (const CommandSpec& sub : s.subVerbs)
                        names = names || EqualsAsciiNoCase(preset.words[0], sub.name);
                if (!names)
                    return Fail("alias '" + a.name + "': its first preset word names no sub-verb of '" +
                                s.name + "'");
            }
        }
        if (s.handler == nullptr && s.subVerbs.empty())
            return Fail("'" + s.name + "' has neither a handler nor sub-verbs");
        // A spec without a handler is judged here and by the verbs below it; its qualifiers and
        // its argument rules are the leaves'.
        if (s.handler == nullptr) {
            if (!s.qualifiers.empty())
                return Fail("'" + s.name +
                            "': a qualifier on a spec without a handler; put it on the verbs below");
            if (s.Targeted()) {
                const ArgSpec& target = s.args[0];
                const bool kindOk = target.kind == ArgKind::Word || target.kind == ArgKind::Player ||
                                    target.kind == ArgKind::PlayerOrId;
                if (s.args.size() != 1 || target.optional || !kindOk)
                    return Fail("'" + s.name +
                                "': a spec without a handler takes one required Word, Player or "
                                "PlayerOrId argument, its target");
            }
        } else {
            std::vector<const ArgSpec*> effective = inherited;
            for (const ArgSpec& a : s.args) effective.push_back(&a);
            bool sawOptional = false;
            for (size_t i = 0; i < effective.size(); ++i) {
                const ArgSpec& a = *effective[i];
                if (a.kind == ArgKind::Rest && i + 1 != effective.size())
                    return Fail("'" + s.name + "': a Rest argument must be last");
                if (a.kind == ArgKind::Contexts && (i + 1 != effective.size() || !a.optional))
                    return Fail("'" + s.name + "': a Contexts argument must be last and optional");
                // An optional Boolean directly before a required Duration or Integer is read by
                // look-ahead (no Boolean word parses as either), so it is no "optional one".
                const bool readAhead = a.kind == ArgKind::Boolean && a.optional &&
                                       i + 1 < effective.size() && !effective[i + 1]->optional &&
                                       (effective[i + 1]->kind == ArgKind::Duration ||
                                        effective[i + 1]->kind == ArgKind::Integer);
                if (a.optional) {
                    if (!readAhead) sawOptional = true;
                } else if (sawOptional) {
                    return Fail("'" + s.name + "': a required argument after an optional one");
                }
            }
            if (s.pastTense.empty()) {
                bool needs = false;
                for (const ArgSpec* a : effective) needs = needs || a->notHost;
                for (const Qualifier& q : s.qualifiers) needs = needs || q.kind == QualKind::Exempt;
                if (needs)
                    return Fail("'" + s.name +
                                "': a notHost argument or an Exempt qualifier needs a pastTense");
            }
            if (!Qualifiers(s, effective)) return false;
        }

        std::string node;
        if (!s.nodeOf.empty()) {
            if (!IsDeclared(s.nodeOf))
                return Fail("'" + s.name + "': nodeOf names the undeclared node '" + s.nodeOf + "'");
            if (QualifierKindOf(s.nodeOf) != nullptr)
                return Fail("'" + s.name + "': nodeOf names the qualifier node '" + s.nodeOf + "'");
            node = s.nodeOf;
        } else {
            node = (isRoot ? std::string("multivoid") : parentNode) + "." + s.name;
            // A spec without a handler declares no node: the string only roots its sub-verbs'.
            if (s.handler != nullptr) {
                if (IsDeclared(node)) return Fail("the node '" + node + "' is already declared");
                byThisTree.insert(node);
            }
        }
        // A qualifier node another spec already declared is not declared twice, and only a
        // qualifier of the same kind reuses it; a node that is not a qualifier node is never taken
        // over, and a command node later derived onto a qualifier node is refused as already
        // declared. A Notify qualifier that names a node of its own declares none.
        for (const Qualifier& q : s.qualifiers) {
            if (!q.node.empty()) continue;
            const std::string qualNode = node + "." + q.name;
            if (IsDeclared(qualNode)) {
                const QualKind* declaredKind = QualifierKindOf(qualNode);
                if (declaredKind == nullptr)
                    return Fail("'" + s.name + "': the qualifier node '" + qualNode +
                                "' is already declared and is not a qualifier node");
                if (*declaredKind != q.kind)
                    return Fail("'" + s.name + "': the qualifier node '" + qualNode +
                                "' is already declared by a qualifier of another kind");
                continue;
            }
            byThisTree.insert(qualNode);
            qualifiersByThisTree.emplace(qualNode, q.kind);
        }

        std::vector<const ArgSpec*> below = inherited;
        if (s.Targeted()) below.push_back(&s.args[0]);
        std::set<std::string> subNames;
        for (const CommandSpec& sub : s.subVerbs) {
            if (!subNames.insert(sub.name).second)
                return Fail("'" + s.name + "': two sub-verbs named '" + sub.name + "'");
            if (!Spec(sub, node, false, below)) return false;
        }
        return true;
    }
};

}  // namespace

bool Registry::Register(CommandSpec spec, std::string* why) {
    auto refuse = [&](const std::string& reason) {
        if (why != nullptr) *why = reason;
        return false;
    };

    std::set<std::string> taken;
    for (const auto& r : roots_) {
        taken.insert(r->name);
        for (const Alias& a : r->aliases) taken.insert(a.name);
    }
    if (taken.count(spec.name) != 0) return refuse("'" + spec.name + "' is already a command or alias");
    std::set<std::string> own{spec.name};
    for (const Alias& a : spec.aliases) {
        if (taken.count(a.name) != 0) return refuse("alias '" + a.name + "' is already a command or alias");
        if (!own.insert(a.name).second) return refuse("alias '" + a.name + "' is named twice");
    }

    TreeCheck check{nodes_, qualifierNodes_, plainNodes_, {}, {}, {}};
    if (!check.Spec(spec, std::string(), true, {})) return refuse(check.why);

    auto owned = std::make_unique<CommandSpec>(std::move(spec));
    Record(*owned, std::string(), std::string(), std::string(), {});
    roots_.push_back(std::move(owned));
    return true;
}

void Registry::Record(const CommandSpec& c, const std::string& parentPath,
                      const std::string& parentNode, const std::string& parentUsage,
                      const std::vector<const ArgSpec*>& inherited) {
    SpecInfo info;
    info.path = parentPath.empty() ? c.name : parentPath + " " + c.name;
    info.usageHead = parentUsage.empty() ? "/" + c.name : parentUsage + " " + c.name;
    info.args = inherited;
    for (const ArgSpec& a : c.args) info.args.push_back(&a);
    if (!c.nodeOf.empty()) {
        info.node = c.nodeOf;
    } else {
        info.node = (parentNode.empty() ? std::string("multivoid") : parentNode) + "." + c.name;
        if (c.handler != nullptr)
            nodes_.emplace(info.node, NodeDecl{info.node, c.defaultGranted, c.description});
    }
    for (const Qualifier& q : c.qualifiers) {
        if (!q.node.empty()) continue;
        const std::string qualNode = info.node + "." + q.name;
        nodes_.emplace(qualNode, NodeDecl{qualNode, false, c.description + " -- " + q.name});
        qualifierNodes_.emplace(qualNode, q.kind);
    }
    const std::string path = info.path;
    const std::string node = info.node;
    std::string below = info.usageHead;
    std::vector<const ArgSpec*> belowArgs = inherited;
    if (c.Targeted()) {
        below += " <" + c.args[0].name + ">";
        belowArgs.push_back(&c.args[0]);
    }
    info_.emplace(&c, std::move(info));
    for (const CommandSpec& sub : c.subVerbs) Record(sub, path, node, below, belowArgs);
}

bool Registry::DeclareNode(NodeDecl d, std::string* why) {
    auto refuse = [&](const char* reason) {
        if (why != nullptr) *why = reason;
        return false;
    };
    if (d.node.empty()) return refuse("an empty node");
    if (nodes_.find(d.node) != nodes_.end()) return refuse("the node is already declared");
    const std::string key = d.node;
    nodes_.emplace(key, std::move(d));
    plainNodes_.insert(key);
    return true;
}

const CommandSpec* Registry::FindRoot(std::string_view word, const Alias** alias) const {
    if (alias != nullptr) *alias = nullptr;
    for (const auto& r : roots_) {
        if (EqualsAsciiNoCase(word, r->name)) return r.get();
        for (const Alias& a : r->aliases) {
            if (!EqualsAsciiNoCase(word, a.name)) continue;
            if (alias != nullptr) *alias = &a;
            return r.get();
        }
    }
    return nullptr;
}

std::vector<const CommandSpec*> Registry::AllVerbs() const {
    std::vector<const CommandSpec*> roots;
    roots.reserve(roots_.size());
    for (const auto& r : roots_) roots.push_back(r.get());
    std::sort(roots.begin(), roots.end(),
              [](const CommandSpec* a, const CommandSpec* b) { return a->name < b->name; });

    std::vector<const CommandSpec*> out;
    struct Walk {
        static void Into(const CommandSpec& c, std::vector<const CommandSpec*>& out) {
            if (c.handler != nullptr) out.push_back(&c);
            for (const CommandSpec& sub : c.subVerbs) Into(sub, out);
        }
    };
    for (const CommandSpec* r : roots) Walk::Into(*r, out);
    return out;
}

const NodeDecl* Registry::FindNode(std::string_view node) const {
    const auto it = nodes_.find(node);
    return it == nodes_.end() ? nullptr : &it->second;
}

void Registry::ForEachNode(const std::function<void(const NodeDecl&)>& fn) const {
    for (const auto& entry : nodes_) fn(entry.second);
}

std::string Registry::NodeOf(const CommandSpec& c) const {
    const auto it = info_.find(&c);
    return it == info_.end() ? std::string() : it->second.node;
}

std::string Registry::PathOf(const CommandSpec& c) const {
    const auto it = info_.find(&c);
    return it == info_.end() ? std::string() : it->second.path;
}

std::vector<const ArgSpec*> Registry::ArgsOf(const CommandSpec& c) const {
    const auto it = info_.find(&c);
    return it == info_.end() ? std::vector<const ArgSpec*>() : it->second.args;
}

std::string Registry::Usage(const CommandSpec& c) const {
    const auto it = info_.find(&c);
    std::string out = it == info_.end() ? std::string("/") : it->second.usageHead;
    for (const ArgSpec& a : c.args) {
        out += a.optional ? " [" : " <";
        out += a.name;
        if (a.kind == ArgKind::Rest || a.kind == ArgKind::Contexts) out += "...";
        out += a.optional ? "]" : ">";
    }
    if (!c.subVerbs.empty()) {
        std::string names;
        for (const CommandSpec& sub : c.subVerbs) {
            if (!names.empty()) names += "|";
            names += sub.name;
        }
        out += c.handler != nullptr ? " [" + names + "]" : " <" + names + ">";
    }
    return out;
}

}  // namespace coop::commands
