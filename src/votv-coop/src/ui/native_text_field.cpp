// ui/native_text_field.cpp -- see ui/native_text_field.h for WHY this owns its own input.

#include "ui/native_text_field.h"

#include "ui/native_screen.h"

#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <atomic>
#include <vector>

namespace ui::native_text_field {

namespace NS = ui::native_screen;
namespace E  = ue_wrap::engine;
namespace U  = ue_wrap::umg;
namespace T  = coop::text;

struct Field {
    void*        box     = nullptr;   // the framed overlay; what we remove on destroy
    void*        parent  = nullptr;
    void*        text    = nullptr;   // the UTextBlock we drive
    std::wstring value;
    std::wstring hint;
    int32_t      maxLen  = 64;
    bool         focused = false;
    bool         submit  = false;     // Enter edge, consumed by the owner
    // Escape edge, consumed by the owner. See the VK_ESCAPE branch: the screens used to
    // ask `AnyFocused()` on their own key-UP edge, which is always false by then because
    // the blur happens on key-DOWN. This is the same question asked in a way that cannot
    // race.
    bool         ateEscape = false;
    bool         dirty   = true;      // repaint owed
    bool         wasDown = false;     // left button edge, for click-to-focus
    uint64_t     caretAt = 0;         // tick count of the last caret phase flip
    bool         caretOn = false;
    std::string  utf8;                // cache so Text() can return a reference
    // ---- the overflow window (see UpdateWindowing) ----
    bool         alignRight  = false;   // the slot alignment currently in force
    size_t       measuredLen = static_cast<size_t>(-1);  // value length when last measured
    int          remeasureIn = -1;      // ticks until the pending measurement; -1 = none
};

namespace {

// EXACTLY ONE FIELD HOLDS THE KEYBOARD. A registry rather than a bare pointer because the
// detour runs on the game thread (`gate3`, 2026-07-31) but a screen can be torn down from
// its own tick in the same frame; the vector lets Destroy() clear the focus without the
// detour ever seeing a dangling handle.
std::vector<Field*> g_live;
std::atomic<Field*> g_focus{nullptr};

// Repaint budget: the caret blinks at the rate the game's own menus do, and a field that
// is not focused never blinks at all -- so an idle browser costs zero widget writes.
constexpr uint64_t kCaretMs = 530;

// Forward-declared: the windowing and paste helpers below repaint, and they are grouped
// with the concept they belong to rather than sorted by definition order.
void Repaint(Field* f);

// The frame `Create` puts around the text, and the gutter its glyphs sit in. Both are
// this module's own constants, and `UpdateWindowing` has to subtract them to know how much
// room the text really has.
constexpr float kFrameBorderPx = 2.f;
constexpr float kTextGutterPx  = 8.f;

// WHICH END OF AN OVER-LONG VALUE THE PLAYER SEES -- and it must be the end they are
// TYPING at.
//
// THE DEFECT, verbatim from the user on the first build that shipped a field: "ебаный
// текст в ебаное окно ввода ip не помещается". A long address typed INVISIBLY. The text
// block is clipped to its box (Create sets ClipToBounds, which is what stops it painting
// across the screen), and a Left-aligned clipped block keeps its HEAD -- so past the
// box's width the player was typing into a part of the string that is not drawn, caret
// included, with no way to tell whether a keystroke had landed.
//
// THE FIX IS AN ALIGNMENT FLIP, NOT A TRIM SUBSYSTEM. Slate clips whichever end the
// alignment pushes out of the box, so a Right-aligned block inside a clipped frame shows
// the TAIL and the caret and hides the head -- which is exactly the window a text cursor
// wants. It costs one enum, works at any font and any size, needs no glyph measuring, and
// the value itself is never touched (paste and prefill window the same way a keystroke
// does). An earlier design measured character widths against an em constant and sliced the
// string; the whole of it died in one /qf round (`SERVER_BROWSER_ARC.md` section 7.10, R7)
// when this appeared, and it should not be re-derived.
//
// MEASURED ONE TICK LATE, ON PURPOSE. `GetDesiredSize` reads the built SWidget, so a value
// set this frame is not laid out yet and would measure at its PREVIOUS width. The
// evaluation is therefore deferred a tick, and keyed on the value's LENGTH rather than run
// per repaint -- the caret blinks twice a second and changes the drawn width by one glyph,
// so measuring on every repaint would make the flip chatter at the boundary.
void UpdateWindowing(Field* f) {
    if (!f || !f->text || !f->box) return;
    if (f->remeasureIn < 0) return;
    if (f->remeasureIn-- > 0) return;   // the deferred tick has not arrived yet
    ue_wrap::FVector2D desired{}, topLeft{}, allotted{};
    // THE GUARDS RUN BEFORE `measuredLen` IS CLAIMED, and the order is the fix.
    //
    // It used to record "measured at this length" FIRST and then bail on any of these, so a
    // measurement that never happened still disarmed the retry -- and `MarkValueChanged`
    // re-arms only on a LENGTH change. A screen shown with an over-long PREFILL (Slate has
    // not arranged it yet, so the rect is legitimately zero) therefore kept Left alignment
    // and hid its own tail until the player typed a character, which is the exact defect
    // the flip exists to prevent. Re-arming means the next tick tries again.
    // (Post-ship correctness audit, 2026-08-31.)
    auto retry = [f] { f->remeasureIn = 1; };
    if (!U::WidgetDesiredSize(f->text, desired)) { retry(); return; }
    if (!U::WidgetScreenRect(f->box, topLeft, allotted)) { retry(); return; }
    const float inner = allotted.X - 2.f * kFrameBorderPx - kTextGutterPx;
    // A widget Slate has never laid out reports a zero rect, and that is a real answer
    // rather than a failure (umg_build.h) -- but it is not one this can act on, so ask
    // again next tick rather than pretending this length was measured.
    if (inner <= 0.f || desired.X <= 0.f) { retry(); return; }
    f->measuredLen = f->value.size();
    const bool want = desired.X > inner;
    if (want == f->alignRight) return;
    f->alignRight = want;
    U::SetSlotHAlignLive(NS::SlotOf(f->text), want ? NS::kRight : NS::kLeft);
}

// The value changed: schedule the measurement for the tick after Slate has laid it out.
void MarkValueChanged(Field* f) {
    if (f && f->value.size() != f->measuredLen) f->remeasureIn = 1;
}

// CTRL+V, and the reason it lands HERE rather than in each screen: an address or a
// nickname is exactly the kind of string a player has in their clipboard and does not want
// to retype, and a field that accepts typing but not pasting is the sort of gap that reads
// as the field being broken. One implementation, so every screen that has a field has
// paste, and none of them can implement it differently.
//
// ENTRY-TRIMMED. A copied address almost always carries a trailing newline or a leading
// space from wherever it was copied, and `host:port ` is not an address. MTA does the same
// at its own entry (`SharedUtil::Trim` on the connect string) rather than teaching every
// consumer to tolerate whitespace.
//
// APPENDED, NOT REPLACING. The grammar of this field is append + backspace + paste with no
// selection (there is no cursor to select with), so paste is a bulk append and behaves
// exactly like typing the characters would -- same cap, same sanitising, same windowing.
// THE PURE HALF, split from the clipboard read so the selftest can drive it. A test that
// went through the real clipboard would have to WRITE to it, and clobbering what the player
// had copied to prove our paste works is not a trade this makes.
void ApplyPastedText(Field* f, const std::wstring& in) {
    if (!f || in.empty()) return;
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' ||
                                          c == L'\n' || c == L'\f' || c == L'\v'; };
    size_t b = 0, e = in.size();
    while (b < e && isSpace(in[b])) ++b;
    while (e > b && isSpace(in[e - 1])) --e;
    // CONTROL CHARACTERS ARE NOT CONTENT, the same rule `OnChar` applies one keystroke at
    // a time -- an embedded newline would otherwise arrive as a glyph nothing can type.
    std::wstring add;
    for (size_t i = b; i < e; ++i)
        if (in[i] >= 0x20 && in[i] != 0x7F) add += in[i];
    if (add.empty()) return;

    f->value = T::CapCodepoints(f->value + add, static_cast<size_t>(f->maxLen));
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    MarkValueChanged(f);
    Repaint(f);
}

void Paste(Field* f) {
    if (!f) return;
    if (!::OpenClipboard(nullptr)) return;
    std::wstring in;
    if (HANDLE h = ::GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(::GlobalLock(h))) {
            // Bounded by the field's own cap plus slack for what the trim will remove: a
            // clipboard can hold megabytes and none of it can reach the value.
            const size_t cap = static_cast<size_t>(f->maxLen) * 4 + 64;
            for (size_t i = 0; i < cap && p[i]; ++i) in += p[i];
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
    ApplyPastedText(f, in);
}

void Repaint(Field* f) {
    if (!f || !f->text) return;
    std::wstring shown;
    if (f->value.empty() && !f->focused) {
        shown = f->hint;
    } else {
        shown = f->value;
        if (f->focused && f->caretOn) shown += L'|';
    }
    E::SetWidgetText(f->text, shown.c_str());
    // The hint is dimmer than real content -- the style doc's secondary grey. A dimmed
    // placeholder is how a player tells "nothing typed yet" from "someone typed this".
    E::SetTextBlockColorDispatch(
        f->text, (f->value.empty() && !f->focused) ? NS::Dim() : NS::Text());
    f->dirty = false;
}

}  // namespace

Field* Create(void* parent, const wchar_t* hint, int32_t maxLen, float widthPx) {
    if (!parent) return nullptr;

    // A REAL SizeBox CARRIES THE WIDTH, and the frame goes INSIDE it.
    //
    // The first version called `SetSizeBoxWidth` on what `AddFramedBox` returns -- which
    // is an OVERLAY, as its own comment says. That writes a float at a SizeBox's property
    // offset into an object that has no such property, and the game paid for it three
    // frames later: `PE detour-outer-callback AV caught -- function='SpawnObject' ...
    // 0xC0000005` three times over, then the whole action bar failing to build
    // (`refresh=0 host=0 connect=0`) because the next spawns landed in corrupted memory.
    // The crash was NOT at the write; a wrong-offset write never is.
    void* sizer = NS::Spawn(L"SizeBox", parent);
    if (!sizer) return nullptr;
    if (widthPx > 0.f) U::SetSizeBoxWidth(sizer, widthPx);

    void* box = NS::AddFramedBox(sizer, NS::RowBg(), 2.f);
    if (!box) return nullptr;
    // The frame is the hit target, so it must be a real Visible widget rather than the
    // HitTestInvisible chrome AddFramedBox gives its chrome images (two flat, or three once the native border material is cloned).
    E::SetWidgetVisibility(box, 0);
    void* tb = NS::AddText(box, hint ? hint : L"", NS::kBtnFontPx, NS::Dim(), NS::kLeft, 0.f);
    if (!tb) return nullptr;
    // CLIP TO THE BOX. `AddText` only sets clipping for a FILL slot, and this one is
    // auto-sized, so a value longer than the field painted straight out of its own frame
    // and across whatever sat beside it. The user saw exactly that on the first build that
    // shipped a field: "ебаный текст в ебаное окно ввода ip не помещается".
    // EWidgetClipping::ClipToBounds = 1.
    U::SetClipping(tb, 1);

    // ATTACH, and attach at BIRTH. `AddFramedBox` spawns with `parent` as the OUTER but
    // does not add the widget to it -- every other caller in this tree does its own
    // AddChild, and the first version of this function did not. An unattached widget tree
    // is GC food: it renders until the next collection and then vanishes, which this
    // project has already paid for once with the MULTIPLAYER button
    // (`[[lesson-an-unattached-widget-tree-is-gc-food]]`).
    U::SetContent(sizer, box);
    if (!U::AddChild(parent, sizer)) return nullptr;

    auto* f = new Field();
    f->box    = sizer;   // the OUTERMOST widget -- what attaches, and what must be removed
    f->parent = parent;
    f->text   = tb;
    f->hint   = hint ? hint : L"";
    f->maxLen = maxLen > 0 ? maxLen : 64;
    g_live.push_back(f);
    Repaint(f);
    return f;
}

// Everything teardown does EXCEPT touching the engine. See the header: a caller running on
// a dead menu instance must not dispatch into its widgets.
void Release(Field* f) {
    if (!f) return;
    Field* expect = f;
    g_focus.compare_exchange_strong(expect, nullptr);
    for (size_t i = 0; i < g_live.size(); ++i)
        if (g_live[i] == f) { g_live.erase(g_live.begin() + static_cast<long>(i)); break; }
    delete f;
}

void Destroy(Field* f) {
    if (!f) return;
    // Clear focus FIRST: the detour reads g_focus without a lock, so the window in which
    // it could reach a half-destroyed field has to be closed before anything is freed.
    Field* expect = f;
    g_focus.compare_exchange_strong(expect, nullptr);
    for (size_t i = 0; i < g_live.size(); ++i)
        if (g_live[i] == f) { g_live.erase(g_live.begin() + static_cast<long>(i)); break; }
    if (f->box && f->parent) U::RemoveChild(f->parent, f->box);
    delete f;
}

void Focus(Field* f) {
    if (!f) return;
    Field* prev = g_focus.exchange(f);
    if (prev && prev != f) { prev->focused = false; prev->dirty = true; Repaint(prev); }
    f->focused = true;
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    Repaint(f);
}

void Blur(Field* f) {
    if (!f) return;
    Field* expect = f;
    g_focus.compare_exchange_strong(expect, nullptr);
    f->focused = false;
    f->caretOn = false;
    f->dirty   = true;
    Repaint(f);
}

bool Focused(const Field* f) { return f && f->focused; }
bool AnyFocused()            { return g_focus.load() != nullptr; }

const std::string& Text(const Field* f) {
    static const std::string kEmpty;
    if (!f) return kEmpty;
    auto* m = const_cast<Field*>(f);
    m->utf8 = T::ToUtf8(f->value);
    return m->utf8;
}

void SetText(Field* f, const std::string& utf8) {
    if (!f) return;
    // Lossy on the way IN is right: this is a local string the player or our own ini
    // supplied, and refusing it whole would leave the field mysteriously empty. The
    // STRICT boundary is the wire, and it is elsewhere.
    f->value = T::CapCodepoints(T::FromUtf8Lossy(utf8.data(), utf8.size()),
                                static_cast<size_t>(f->maxLen));
    f->dirty = true;
    MarkValueChanged(f);
    Repaint(f);
}

void Tick(Field* f) {
    if (!f) return;

    // CLICK TO FOCUS, by geometry -- the same mechanism the rows and the NEW GAME row use,
    // and the only one measured to work in this tree. A press anywhere else blurs, so a
    // player clicking a server row is not still typing into the address box.
    const bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !f->wasDown) {
        if (NS::CursorOverWidget(f->box)) Focus(f);
        else if (f->focused)              Blur(f);
    }
    f->wasDown = down;

    if (f->focused) {
        const uint64_t now = ::GetTickCount64();
        if (now - f->caretAt >= kCaretMs) {
            f->caretAt = now;
            f->caretOn = !f->caretOn;
            f->dirty   = true;
        }
    }
    if (f->dirty) Repaint(f);
    // AFTER the repaint: the measurement is about the text that was just written, and it
    // is deferred internally by a tick anyway.
    UpdateWindowing(f);
}

bool ConsumeEscape(Field* f) {
    if (!f || !f->ateEscape) return false;
    f->ateEscape = false;
    return true;
}

bool ConsumeSubmit(Field* f) {
    if (!f || !f->submit) return false;
    f->submit = false;
    return true;
}

bool OnChar(wchar_t c) {
    Field* f = g_focus.load();
    if (!f) return false;
    // Control characters are not content. Backspace and Enter arrive as WM_CHAR too on
    // some layouts, and they are handled on the KEYDOWN edge instead -- taking them here
    // as well would type a box glyph AND act on them.
    if (c < 0x20 || c == 0x7F) return true;   // swallowed, deliberately not inserted
    if (static_cast<int32_t>(T::CountCodepoints(f->value)) >= f->maxLen) return true;
    f->value += c;
    f->caretOn = true;
    f->caretAt = ::GetTickCount64();
    f->dirty   = true;
    MarkValueChanged(f);
    Repaint(f);
    return true;
}

// ---- SELFTEST -----------------------------------------------------------------------
//
// UN-GATED, and it runs on every boot. The editing rules here are PURE -- append, cap,
// backspace, the Enter edge, Escape -- so they need no widget and no game to exercise:
// a Field with null `box`/`text` makes Repaint a no-op and everything else is the real
// production path, `OnChar` and `OnKeyDown` included. This is deliberately not a dev flag.
// Three separate instruments today reported confident wrong answers because nobody had
// run them against the failing axis; a test that only runs when someone remembers to arm
// it is the same bet.
//
// What it CANNOT cover, stated so the coverage is not overclaimed: whether the WndProc
// seam actually delivers a keystroke to this module. That is a live-process question and
// belongs to a run, not to a unit check.
bool RunSelftest() {
    int checks = 0, failed = 0;
    auto ok = [&](bool cond, const char* what) {
        ++checks;
        if (!cond) { ++failed; UE_LOGE("native_text_field selftest FAIL: %s", what); }
    };

    // STATIC, not a stack local, and the reason is a real hazard rather than style:
    // `Focus()` publishes this pointer into `g_focus`, which the WndProc detour reads from
    // the GAME thread. A stack object would leave that global pointing at a dead frame the
    // moment this function returned -- the window is microseconds and the detour is not
    // even installed this early in boot, so it has probably never fired, but "probably
    // never" is not a lifetime argument. A function-local static cannot dangle, and the
    // final check below still proves the focus was released.
    static Field f;               // no widgets: Repaint no-ops, the logic is untouched
    f.maxLen = 5;
    Focus(&f);
    ok(AnyFocused(), "focus is taken");

    ok(OnChar(L'1') && OnChar(L'2'), "printable characters are consumed");
    ok(Text(&f) == "12", "typed characters land in the value");

    ok(OnChar(L'\r'), "a control character is consumed");
    ok(Text(&f) == "12", "...but is NOT inserted as content");

    ok(OnKeyDown(VK_BACK), "backspace is consumed");
    ok(Text(&f) == "1", "backspace removes one character");

    OnChar(L'2'); OnChar(L'3'); OnChar(L'4'); OnChar(L'5');
    ok(Text(&f) == "12345", "the field fills to maxLen");
    OnChar(L'6');
    ok(Text(&f) == "12345", "maxLen is a CAP, not a suggestion");

    ok(!ConsumeSubmit(&f), "no submit before Enter");
    ok(OnKeyDown(VK_RETURN), "Enter is consumed");
    ok(ConsumeSubmit(&f), "Enter raises the submit edge");
    ok(!ConsumeSubmit(&f), "...and the edge is consumed ONCE");

    // A surrogate pair is ONE character to the player. Deleting half of it would leave an
    // unpaired surrogate in a string that is about to be encoded to UTF-8.
    f.maxLen = 8;
    SetText(&f, "");
    OnChar(static_cast<wchar_t>(0xD83D)); OnChar(static_cast<wchar_t>(0xDE00));  // U+1F600
    const size_t pairLen = Text(&f).size();
    ok(pairLen == 4, "an astral character encodes to 4 UTF-8 bytes");
    OnKeyDown(VK_BACK);
    ok(Text(&f).empty(), "backspace removes the WHOLE surrogate pair");

    // PASTE, driven through the pure half (the clipboard read is the untestable part and
    // is one GetClipboardData call above it).
    f.maxLen = 24;
    SetText(&f, "");
    ApplyPastedText(&f, L"  10.0.0.5:7777\r\n");
    ok(Text(&f) == "10.0.0.5:7777", "paste trims leading and trailing whitespace");
    SetText(&f, "");
    ApplyPastedText(&f, L"a\nb\tc");
    ok(Text(&f) == "abc", "paste drops embedded control characters");
    SetText(&f, "abc");
    ApplyPastedText(&f, L"def");
    ok(Text(&f) == "abcdef", "paste APPENDS -- the grammar has no selection to replace");
    f.maxLen = 4;
    SetText(&f, "ab");
    ApplyPastedText(&f, L"cdefgh");
    ok(Text(&f) == "abcd", "paste obeys maxLen");
    SetText(&f, "keep");
    ApplyPastedText(&f, L"   \r\n  ");
    ok(Text(&f) == "keep", "an all-whitespace paste changes nothing");
    f.maxLen = 8;

    ok(!ConsumeEscape(&f), "no escape edge before Escape");
    ok(OnKeyDown(VK_ESCAPE), "Escape is consumed");
    ok(!AnyFocused(), "Escape leaves the field");
    // THE LATCH SURVIVES THE BLUR, which is the whole point -- the owner asks AFTER the
    // field has already let go, and `AnyFocused()` cannot answer for it by then.
    ok(ConsumeEscape(&f), "the escape edge is readable after the blur");
    ok(!ConsumeEscape(&f), "and it is consumed exactly once");
    ok(!OnChar(L'x'), "an unfocused module refuses the key so the game still gets it");

    // Blur() cleared g_focus; the local Field is about to die, so nothing may still point
    // at it. (Destroy() is not used here: it would try to touch a parent that never existed.)
    ok(g_focus.load() == nullptr, "no dangling focus at teardown");

    if (failed == 0) UE_LOGI("native_text_field selftest: ALL PASS (%d checks)", checks);
    else             UE_LOGE("native_text_field selftest: %d/%d FAILED", failed, checks);
    return failed == 0;
}

bool OnKeyDown(int vk) {
    Field* f = g_focus.load();
    if (!f) return false;
    switch (vk) {
        case VK_BACK:
            if (!f->value.empty()) {
                // Pop a whole CODEPOINT: a surrogate pair is one character to the player,
                // and deleting half of it leaves an unpaired surrogate in a string that is
                // about to be encoded to UTF-8.
                size_t n = f->value.size();
                if (n >= 2 && (f->value[n - 1] & 0xFC00) == 0xDC00 &&
                              (f->value[n - 2] & 0xFC00) == 0xD800) f->value.resize(n - 2);
                else                                                f->value.resize(n - 1);
                f->dirty = true;
                MarkValueChanged(f);
                Repaint(f);
            }
            return true;
        case 'V':
            // CTRL+V. The modifier is read here rather than passed in because the detour
            // hands us only the virtual key -- and it runs synchronously on the message
            // (`gate3`: WndProc is the game thread), so `GetKeyState` reports the state
            // that key was pressed WITH. A bare V is content and must fall through to
            // WM_CHAR; only the chord is ours.
            if ((::GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;
            Paste(f);
            return true;
        case VK_RETURN:
            f->submit = true;
            return true;
        case VK_ESCAPE:
            // Escape LEAVES THE FIELD, it does not close the screen. Swallowing the
            // message is NOT enough to make that true: the screens read Escape with
            // GetAsyncKeyState, which sees the physical key whatever the detour does.
            //
            // AND `AnyFocused()` WAS THE WRONG QUESTION FOR THEM TO ASK. This runs on
            // WM_KEYDOWN, which strictly precedes the key-UP the screens take their edge
            // on -- so by the time they asked, the field had already blurred and the
            // guard could NEVER fire. A player who pressed Escape to stop editing lost
            // the window and the password they had typed (post-ship audit, 2026-08-31).
            // The latch below is the deterministic answer: it says "an Escape was MINE",
            // and it cannot race, because it is set by the same event that consumed it.
            f->ateEscape = true;
            Blur(f);
            return true;
        default:
            // Everything else falls through to whoever else wants it. A focused address
            // box has no business swallowing F1 or the movement keys.
            return false;
    }
}

}  // namespace ui::native_text_field
