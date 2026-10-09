// l10n/catalog.h -- one language's merged catalogue (private to src/l10n/).
//
// Built once by l10n::Init from up to four sources inserted in rising priority -- the `ll` pack, the
// `ll` override, the `ll_CC` pack, the `ll_CC` override -- so a later insert replaces an earlier one and
// a lookup is one probe whatever the layering. Immutable once published; any thread may look up.
#pragma once

#include "plural_expr.h"
#include "po_reader.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace l10n::detail {

struct Entry {
    bool plural = false;
    std::vector<std::string> forms;     // one for a singular entry
    uint8_t rule = 0;                   // the compiled rule of the file it came from
    mutable std::atomic<uint8_t> hit{0};
};

// gettext's own key: "ctx\x04msgid" with a context, "msgid" without, looked up without building it.
struct KeyView {
    const char*      ctx;               // null: no context
    std::string_view id;
};

struct KeyHash {
    using is_transparent = void;
    static size_t Mix(size_t h, std::string_view s) noexcept {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        return h;
    }
    size_t operator()(std::string_view key) const noexcept { return Mix(1469598103934665603ull, key); }
    size_t operator()(const KeyView& k) const noexcept {
        size_t h = 1469598103934665603ull;
        if (k.ctx) {
            h = Mix(h, k.ctx);
            h = Mix(h, std::string_view("\x04", 1));
        }
        return Mix(h, k.id);
    }
};

struct KeyEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    bool operator()(std::string_view key, const KeyView& k) const noexcept {
        if (!k.ctx) return key == k.id;
        const std::string_view ctx(k.ctx);
        return key.size() == ctx.size() + 1 + k.id.size() && key.compare(0, ctx.size(), ctx) == 0 &&
               key[ctx.size()] == '\x04' && key.compare(ctx.size() + 1, k.id.size(), k.id) == 0;
    }
    bool operator()(const KeyView& k, std::string_view key) const noexcept { return (*this)(key, k); }
};

// What one source contributed, for the log line.
struct SourceReport {
    std::string name;                   // "embedded pack zh_CN", "override zh.po"
    bool        loaded = false;
    std::string refusal;                // why the whole file was refused, when it was
    size_t      inserted = 0;
    std::vector<po::Diag> refused;      // the reader's refused entries and the catalogue's
    std::string pluralNote;             // why this file's Plural-Forms did not compile, when it did not
    std::string translator;             // Last-Translator
    std::string team;                   // Language-Team
};

class Catalog {
public:
    // Inserts a read file's entries over what is there. `fallbackRule` is the rule a plural entry takes
    // when its own file has no parseable Plural-Forms (an override's: the pack's at its level), or -1.
    // Returns the index of the rule this file's plural entries use, or -1 when it has none.
    int Insert(const po::File& f, int fallbackRule, SourceReport& report);

    // A lookup: marks the entry found (its hit flag), as a drawn line does.
    const Entry* Find(const char* ctx, std::string_view id) const;
    // The same probe without marking, for the drill's reading.
    const Entry* Peek(const char* ctx, std::string_view id) const;
    unsigned SelectForm(const Entry& e, unsigned long long n) const;
    size_t Size() const { return map_.size(); }
    size_t FoundCount() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Entry>, KeyHash, KeyEq> map_;
    std::vector<plural::Rule> rules_;
};

}  // namespace l10n::detail
