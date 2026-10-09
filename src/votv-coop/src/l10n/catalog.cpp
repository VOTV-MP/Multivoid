// l10n/catalog.cpp -- see l10n/catalog.h.

#include "catalog.h"

namespace l10n::detail {

int Catalog::Insert(const po::File& f, int fallbackRule, SourceReport& report) {
    report.translator = f.lastTranslator;
    report.refused.insert(report.refused.end(), f.refused.begin(), f.refused.end());
    int rule = fallbackRule;
    if (!f.pluralForms.empty()) {
        plural::Rule r;
        std::string why;
        if (r.Compile(f.pluralForms, &why)) {
            rules_.push_back(std::move(r));
            rule = static_cast<int>(rules_.size() - 1);
        } else {
            report.pluralNote = why;
        }
    } else {
        report.pluralNote = "no Plural-Forms in the header";
    }
    for (const po::Entry& e : f.entries) {
        if (e.plural) {
            // Each plural entry keeps the rule of the file it came from, so a pack's entry is never
            // indexed by an override's rule.
            if (rule < 0) {
                report.refused.push_back(po::Diag{e.line, "a plural entry whose file has no usable Plural-Forms"});
                continue;
            }
            if (e.str.size() != rules_[static_cast<size_t>(rule)].Count()) {
                report.refused.push_back(po::Diag{e.line, "a count of msgstr[n] other than the rule's nplurals"});
                continue;
            }
        }
        std::string key;
        if (e.hasCtx) {
            key.reserve(e.ctx.size() + 1 + e.id.size());
            key.append(e.ctx).push_back('\x04');
        }
        key.append(e.id);
        auto entry = std::make_unique<Entry>();
        entry->plural = e.plural;
        entry->forms = e.str;
        entry->rule = static_cast<uint8_t>(e.plural ? rule : 0);
        map_[std::move(key)] = std::move(entry);
        ++report.inserted;
    }
    report.loaded = true;
    return rule;
}

const Entry* Catalog::Peek(const char* ctx, std::string_view id) const {
    const auto it = map_.find(KeyView{ctx, id});
    return it == map_.end() ? nullptr : it->second.get();
}

const Entry* Catalog::Find(const char* ctx, std::string_view id) const {
    const Entry* e = Peek(ctx, id);
    // One relaxed load per found lookup; the store happens once per entry.
    if (e && !e->hit.load(std::memory_order_relaxed)) e->hit.store(1, std::memory_order_relaxed);
    return e;
}

unsigned Catalog::SelectForm(const Entry& e, unsigned long long n) const {
    if (!e.plural || e.rule >= rules_.size()) return 0;
    return rules_[e.rule].Select(n);
}

size_t Catalog::FoundCount() const {
    size_t n = 0;
    for (const auto& kv : map_)
        if (kv.second->hit.load(std::memory_order_relaxed)) ++n;
    return n;
}

}  // namespace l10n::detail
