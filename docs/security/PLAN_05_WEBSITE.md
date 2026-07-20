# Plan 05 — the future website (write these rules before the site exists)

**Closes:** `TRACKER` **A8** (stored XSS via lobby fields).
**Status: DESIGN — and deliberately written before the site.** The site is roadmap phase work
(lobby list, online counters, stats on the same VPS as the master).

**This costs nothing today and gets expensive the moment the site ships.** That is the entire reason
it has its own plan while being only a MEDIUM finding.

---

## 1. The finding `[A]`

`master.rs:356-364` → `common.rs:78-111`: `clamp_str` strips control characters, bidi overrides and
zero-width characters, but **deliberately not** `< > & " '`.

**That is correct for a game label** — a player whose lobby is called `<3 co-op` should see it
rendered as typed, and mangling it in storage corrupts the in-game display too (Rule S4).

It is fatal only if a web page later drops those bytes into `innerHTML`. And the field is planted by
a single anonymous `POST /v1/host` — no game client, no account, no rate limit worth the name.

### Affected fields

| Field | Max length | Source |
|---|---|---|
| `name` | 63 | Attacker-controlled |
| `world` | 39 | Attacker-controlled |
| `version` / `game` | 23 | Attacker-controlled |

`lobbyId`, `proto`, `players_*`, `age` are server-minted or integers and are **safe**.

---

## 2. The rules the site must follow

### 2.1 Never `innerHTML` a lobby field

Use `textContent` / `createTextNode`, or a default-escaping template: React `{}`, Vue `{{ }}`,
Jinja/Handlebars autoescape.

**Forbidden on these fields:** `dangerouslySetInnerHTML`, `v-html`, `{{{ }}}`, `.innerHTML =`,
`insertAdjacentHTML`, `document.write`.

### 2.2 Attribute contexts need attribute-escaping

Never interpolate a lobby field into an attribute without attribute-escaping, and **never at all**
into `href`, `src`, `style`, or any inline `on*` handler. HTML-escaping is not sufficient in a
`javascript:` URL context.

### 2.3 Never build a JSON island by concatenation

`</script>` inside a lobby name breaks out of a `<script>` block **even with HTML escaping applied**.
Serialize server-side with a JSON encoder that escapes `<`, or fetch the data over XHR instead of
inlining it.

### 2.4 Ship a CSP

```
default-src 'self'; script-src 'self'; object-src 'none'; base-uri 'none'
```

Defence in depth — it turns a missed escape from "account-level compromise of every visitor" into
"a broken-looking table cell". Note it only helps if there is no `unsafe-inline`, which means no
inline event handlers anywhere on the page.

### 2.5 Consume `/v1/lobbies` only

Never proxy `/v1/host`, `/v1/join`, or `/healthz` to a browser. `/v1/join` in particular discloses a
direct host's IP (`TRACKER` **A9**) and mutates signaling state.

### 2.6 A reverse proxy must OVERWRITE `X-Real-IP`, not append

`resolve_client_ip` trusts XFF only from loopback and takes the rightmost element — which is correct
today. If a reverse proxy is ever placed in front of the master and **appends** rather than
overwrites, every per-IP rate limit and cap collapses into a single bucket, silently disabling the
A6 and A7 fixes.

**This is a trap for future-us, not an attack.** It belongs in the deployment checklist, not just
here.

---

## 3. What the site must NOT do

- **Do not "fix" this in `clamp_str`.** Wrong layer (Rule S4): it breaks legitimate names, corrupts
  the in-game display, and still would not protect an attribute or JSON context. The output context
  decides the escaping, and only the renderer knows the context.
- Do not add a second sanitizer in the master "just in case" — two sanitizers means neither is
  authoritative and both rot.

---

## 4. Acceptance

Before the site goes live, a lobby named exactly:

```
<img src=x onerror=alert(1)>
```

must be registerable via `POST /v1/host` (it is a legal game label), display **literally** on the
site, and execute nothing. Test it as a live check, not as a unit test — the bug lives in the render
path, which is where the test must run.

Add the same string to `world` and `version` and repeat: three fields, three contexts.

---

## 5. Status

Nothing to build until the site exists. **When the site's spec is written, these rules go into it**
— that is the handoff. This file is done until then.

---

Back to: `README.md` · `TRACKER.md` **A8** · `RULES.md` S4.
