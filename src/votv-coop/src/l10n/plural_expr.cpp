// l10n/plural_expr.cpp -- see l10n/plural_expr.h.

#include "l10n/plural_expr.h"

#include <cctype>

namespace l10n::plural {

enum Op : uint8_t {
    kNum, kVarN, kNot, kMul, kDiv, kMod, kAdd, kSub,
    kLt, kGt, kLe, kGe, kEq, kNe, kAnd, kOr, kCond,
};

// Recursive descent, one function per precedence level of GNU's grammar (plural.y): `?:` binds
// loosest and associates to the right, then `||`, `&&`, `== !=`, `< > <= >=`, `+ -`, `* / %`, and
// the unary `!`. Nesting counts the levels that can recurse without consuming an operator chain --
// a parenthesis, a `!`, a `?:` branch -- so a long flat rule (ru, pl) is never refused for its
// length of precedence levels, and a hostile `((((...` or `!!!!...` is refused at kMaxNesting.
class Parser {
public:
    Parser(std::string_view s, std::vector<Rule::Node>& out) : s_(s), out_(out) {}

    bool Parse(int32_t* root, std::string* why) {
        const int32_t r = Cond();
        Skip();
        if (!err_ && pos_ != s_.size()) Fail("input left over after the expression");
        if (err_) { if (why) *why = err_; return false; }
        *root = r;
        return true;
    }

private:
    void Skip() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_; }
    bool Eat(char c) {
        Skip();
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }
    bool Eat2(char c0, char c1) {
        Skip();
        if (pos_ + 1 < s_.size() && s_[pos_] == c0 && s_[pos_ + 1] == c1) { pos_ += 2; return true; }
        return false;
    }
    char Peek() { Skip(); return pos_ < s_.size() ? s_[pos_] : '\0'; }
    char Peek2() { Skip(); return pos_ + 1 < s_.size() ? s_[pos_ + 1] : '\0'; }
    int32_t Fail(const char* why) { if (!err_) err_ = why; return -1; }
    int32_t Emit(uint8_t op, int32_t a = -1, int32_t b = -1, int32_t c = -1, uint64_t v = 0) {
        if (err_) return -1;
        Rule::Node n;
        n.op = op; n.a = a; n.b = b; n.c = c; n.value = v;
        out_.push_back(n);
        return static_cast<int32_t>(out_.size() - 1);
    }
    bool Enter() {
        if (++depth_ > kMaxNesting) { Fail("the expression nests deeper than 32"); return false; }
        return true;
    }

    int32_t Cond() {
        const int32_t c = Or();
        if (err_ || !Eat('?')) return c;
        if (!Enter()) return -1;
        const int32_t t = Cond();
        if (!Eat(':')) return Fail("a '?' without its ':'");
        const int32_t f = Cond();
        --depth_;
        return Emit(kCond, c, t, f);
    }
    int32_t Or() {
        int32_t l = And();
        while (!err_ && Eat2('|', '|')) l = Emit(kOr, l, And());
        return l;
    }
    int32_t And() {
        int32_t l = Eq();
        while (!err_ && Eat2('&', '&')) l = Emit(kAnd, l, Eq());
        return l;
    }
    int32_t Eq() {
        int32_t l = Rel();
        for (;;) {
            if (err_) return -1;
            if (Eat2('=', '=')) l = Emit(kEq, l, Rel());
            else if (Eat2('!', '=')) l = Emit(kNe, l, Rel());
            else return l;
        }
    }
    int32_t Rel() {
        int32_t l = Add();
        for (;;) {
            if (err_) return -1;
            if (Eat2('<', '=')) l = Emit(kLe, l, Add());
            else if (Eat2('>', '=')) l = Emit(kGe, l, Add());
            else if (Eat('<')) l = Emit(kLt, l, Add());
            else if (Eat('>')) l = Emit(kGt, l, Add());
            else return l;
        }
    }
    int32_t Add() {
        int32_t l = Mul();
        for (;;) {
            if (err_) return -1;
            if (Eat('+')) l = Emit(kAdd, l, Mul());
            else if (Eat('-')) l = Emit(kSub, l, Mul());
            else return l;
        }
    }
    int32_t Mul() {
        int32_t l = Unary();
        for (;;) {
            if (err_) return -1;
            if (Eat('*')) l = Emit(kMul, l, Unary());
            else if (Eat('/')) l = Emit(kDiv, l, Unary());
            else if (Eat('%')) l = Emit(kMod, l, Unary());
            else return l;
        }
    }
    int32_t Unary() {
        // `!=` is the equality operator, not a `!` before `=`.
        if (Peek() == '!' && Peek2() != '=') {
            ++pos_;
            if (!Enter()) return -1;
            const int32_t a = Unary();
            --depth_;
            return Emit(kNot, a);
        }
        return Primary();
    }
    int32_t Primary() {
        const char c = Peek();
        if (c == 'n') { ++pos_; return Emit(kVarN); }
        if (c >= '0' && c <= '9') {
            uint64_t v = 0;
            int digits = 0;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                if (++digits > 19) return Fail("a number longer than 19 digits");
                v = v * 10 + static_cast<uint64_t>(s_[pos_++] - '0');
            }
            return Emit(kNum, -1, -1, -1, v);
        }
        if (c == '(') {
            ++pos_;
            if (!Enter()) return -1;
            const int32_t e = Cond();
            if (!Eat(')')) return Fail("a '(' without its ')'");
            --depth_;
            return e;
        }
        return Fail(c ? "a token outside the plural grammar" : "the expression ends early");
    }

    std::string_view s_;
    std::vector<Rule::Node>& out_;
    size_t pos_ = 0;
    unsigned depth_ = 0;
    const char* err_ = nullptr;
};

namespace {

// The value after `key=` up to the next ';' (or the end), trimmed.
bool Field(std::string_view s, std::string_view key, std::string_view* out) {
    for (size_t at = 0; (at = s.find(key, at)) != std::string_view::npos; at += key.size()) {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(s[at - 1])) || s[at - 1] == '_')) continue;
        size_t p = at + key.size();
        while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
        if (p >= s.size() || s[p] != '=') continue;
        ++p;
        size_t end = s.find(';', p);
        if (end == std::string_view::npos) end = s.size();
        std::string_view v = s.substr(p, end - p);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
        *out = v;
        return true;
    }
    return false;
}

}  // namespace

bool Rule::Compile(std::string_view headerValue, std::string* why) {
    nodes_.clear();
    root_ = -1;
    nplurals_ = 0;
    std::string_view count, expr;
    if (!Field(headerValue, "nplurals", &count) || !Field(headerValue, "plural", &expr)) {
        if (why) *why = "no nplurals= and plural= pair";
        return false;
    }
    unsigned n = 0;
    if (count.empty() || count.size() > 2) {
        if (why) *why = "nplurals is not a number from 1 to 8";
        return false;
    }
    for (char c : count) {
        if (c < '0' || c > '9') { if (why) *why = "nplurals is not a number from 1 to 8"; return false; }
        n = n * 10 + static_cast<unsigned>(c - '0');
    }
    if (n < 1 || n > kMaxForms) {
        if (why) *why = "nplurals is not a number from 1 to 8";
        return false;
    }
    if (expr.size() > kMaxExprChars) {
        if (why) *why = "the plural expression is longer than 256 characters";
        return false;
    }
    std::vector<Node> nodes;
    int32_t root = -1;
    Parser p(expr, nodes);
    if (!p.Parse(&root, why)) return false;
    nodes_ = std::move(nodes);
    root_ = root;
    nplurals_ = n;
    return true;
}

unsigned Rule::Select(uint64_t n) const {
    if (root_ < 0) return 0;
    // The rule is immutable after Compile and Select may run on several threads at once, so `n`
    // cannot be written into the node array: the walk carries it instead.
    struct Walk {
        const std::vector<Node>& nodes;
        uint64_t n;
        bool divZero;
        uint64_t Go(int32_t i) {
            const Node& k = nodes[static_cast<size_t>(i)];
            switch (k.op) {
            case kNum:  return k.value;
            case kVarN: return n;
            case kNot:  return Go(k.a) == 0 ? 1 : 0;
            case kAnd:  return (Go(k.a) != 0 && Go(k.b) != 0) ? 1 : 0;
            case kOr:   return (Go(k.a) != 0 || Go(k.b) != 0) ? 1 : 0;
            case kCond: return Go(k.a) != 0 ? Go(k.b) : Go(k.c);
            default: break;
            }
            const uint64_t l = Go(k.a), r = Go(k.b);
            switch (k.op) {
            case kMul: return l * r;
            case kDiv: if (r == 0) { divZero = true; return 0; } return l / r;
            case kMod: if (r == 0) { divZero = true; return 0; } return l % r;
            case kAdd: return l + r;
            case kSub: return l - r;
            case kLt:  return l < r ? 1 : 0;
            case kGt:  return l > r ? 1 : 0;
            case kLe:  return l <= r ? 1 : 0;
            case kGe:  return l >= r ? 1 : 0;
            case kEq:  return l == r ? 1 : 0;
            case kNe:  return l != r ? 1 : 0;
            default:   return 0;
            }
        }
    } w{nodes_, n, false};
    const uint64_t form = w.Go(root_);
    if (w.divZero || form >= nplurals_) return 0;
    return static_cast<unsigned>(form);
}

}  // namespace l10n::plural
