#!/usr/bin/env python3
"""tools/i18n/translate.py -- fill a translation pack with a LOCAL model.

The pack template (trans/example.json) is English-to-English: every UI string the mod asks for,
keyed by itself. This script hands those strings to a model running on this machine -- llama.cpp's
OpenAI-compatible endpoint or Ollama, whichever is up -- and writes the target-language pack.

Design notes that matter more than the plumbing:
  * ONE request per string, by default. A 3B model asked for a JSON array of thirty translations
    drifts, and a drifted batch costs more than the round trips it saved.
  * A translation is ACCEPTED only if it keeps the source's printf conversions (%s, %d, %zu) and
    its '##' ImGui id markers. A rejected string is left out of the pack, so the runtime falls
    back to English for that one line -- the failure is visible and local instead of a garbled
    panel or a lost widget id.
  * Every accepted string is cached as it lands (--cache), so a crash, a reboot or a Ctrl-C
    resumes instead of starting over.

Example:
  llama-server -m models/qwen2.5-3b.gguf -c 4096 --port 8080
  python tools/i18n/translate.py --lang zh-CN --tag ZH-CN
"""
import argparse
import json
import pathlib
import re
import sys
import threading
import urllib.error
import urllib.request

SRC = pathlib.Path("src/votv-coop")
LANG_NAMES = {
    "zh-CN": "Simplified Chinese (简体中文)",
    "zh-TW": "Traditional Chinese (繁體中文)",
    "ja": "Japanese (日本語)",
    "ko": "Korean (한국어)",
    "de": "German (Deutsch)",
    "fr": "French (français)",
    "es": "Spanish (español)",
    "ru": "Russian (русский)",
    "pt-BR": "Brazilian Portuguese (português do Brasil)",
    "pl": "Polish (polski)",
}
# % conversions and the ImGui id suffix. Both must survive translation: the first is read by the
# formatting call, the second is the widget's identity inside ImGui.
CONV = re.compile(r"%[-+ #0-9.*hlLjztI]*[A-Za-z]")
MARK = re.compile(r"##[^\s\"\\]*")


def ask(base, model, system, user, timeout):
    payload = {
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "temperature": 0.1,
        "max_tokens": 512,
        "stream": False,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(base.rstrip("/") + "/v1/chat/completions", data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = json.loads(r.read().decode("utf-8"))
    return body["choices"][0]["message"]["content"]


def clean(text):
    t = text.strip()
    if len(t) >= 2 and t[0] == t[-1] and t[0] in "'\"":
        t = t[1:-1].strip()
    # Some models prefix the answer with a label ("translation:", or the target language's own
    # word for it); strip it. The Chinese entries are data, not prose -- that is simply what the
    # label looks like when the target is Chinese.
    t = re.sub(r"^(translation|译文|中文)\s*[:：]\s*", "", t, flags=re.IGNORECASE)
    return t.strip()


def check(src, out):
    """Why this translation cannot be shipped, or an empty string when it can."""
    if not out:
        return "empty"
    if "%" in src and sorted(CONV.findall(src)) != sorted(CONV.findall(out)):
        return "conversions changed: " + repr(CONV.findall(src)) + " -> " + repr(CONV.findall(out))
    if sorted(MARK.findall(src)) != sorted(MARK.findall(out)):
        return "## id markers changed: " + repr(MARK.findall(src)) + " -> " + repr(MARK.findall(out))
    if out == src:
        return "not translated (the model echoed the English back)"
    return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--lang", required=True, help="target language tag, e.g. zh-CN")
    ap.add_argument("--tag", required=True, help="the pack file name to write, e.g. ZH-CN")
    ap.add_argument("--base", default="http://127.0.0.1:8080", help="local model endpoint")
    ap.add_argument("--model", default="local", help="model name (llama.cpp ignores it)")
    ap.add_argument("--source", default=str(SRC / "trans" / "example.json"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--cache", default=None)
    ap.add_argument("--limit", type=int, default=0, help="stop after N strings (a smoke run)")
    ap.add_argument("--workers", type=int, default=1)
    args = ap.parse_args()

    out_path = pathlib.Path(args.out or (SRC / "trans" / (args.tag + ".json")))
    cache_path = pathlib.Path(args.cache or (SRC / "trans" / ("." + args.tag + ".cache.json")))

    keys = list(json.loads(pathlib.Path(args.source).read_text(encoding="utf-8")).keys())
    if args.limit:
        keys = keys[: args.limit]
    cache = {}
    if cache_path.is_file():
        cache = json.loads(cache_path.read_text(encoding="utf-8"))
    # A cache is a claim, not evidence. Anything the checks would reject today came from an
    # earlier, weaker run -- re-validate on load, or an old "Host##" survives every later fix.
    dropped = [k for k, v in cache.items() if check(k, v)]
    for k in dropped:
        del cache[k]
    if dropped:
        print("dropped " + str(len(dropped)) + " cached entry(ies) that no longer pass the checks")
    todo = [k for k in keys if k not in cache]

    lang = LANG_NAMES.get(args.lang, args.lang)
    system = ("You are a game UI localization engine. Translate the user's English UI string into "
              + lang + ". Reply with ONLY the translation -- no quotes, no notes, no alternatives. "
              "Keep every %s, %d, %zu placeholder byte-for-byte as it is. Keep any '##' suffix "
              "exactly as it is. Match the source's tone: short labels stay short.")
    print(f"{len(keys)} string(s), {len(todo)} to translate, endpoint {args.base}")

    lock = threading.Lock()
    stats = {"ok": 0, "bad": 0}

    def work(chunk):
        for k in chunk:
            prompt = "English: " + k + "\nTranslation:"
            try:
                raw = ask(args.base, args.model, system, prompt, 300)
            except (urllib.error.URLError, TimeoutError, OSError, KeyError, json.JSONDecodeError) as e:
                with lock:
                    print("  request failed: " + str(e)[:120])
                return
            value = clean(raw)
            why = check(k, value)
            with lock:
                if why:
                    stats["bad"] += 1
                    print("  REJECT " + why + "  src=" + repr(k[:60]) + " got=" + repr(value[:60]))
                else:
                    stats["ok"] += 1
                    cache[k] = value
                    cache_path.write_text(json.dumps(cache, ensure_ascii=False, indent=2),
                                          encoding="utf-8")
                    if stats["ok"] % 20 == 0:
                        print(f"  ...{stats['ok']} done")

    if args.workers > 1:
        chunks = [todo[i::args.workers] for i in range(args.workers)]
        threads = [threading.Thread(target=work, args=(c,)) for c in chunks]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    else:
        work(todo)

    pack = {k: cache[k] for k in keys if k in cache}
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(pack, f, ensure_ascii=False, indent=2, sort_keys=False)
        f.write("\n")
    print(f"wrote {out_path} -- {len(pack)}/{len(keys)} translated, {stats['bad']} rejected this run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
