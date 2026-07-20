# Eka — Locked V1 Specification

> *"One file. One truth. Zero config."*
>
> A single-file, zero-config web scripting language for solo devs, indie hackers, and LLM agents.

**Status:** Locked V1
**Date:** 2026-07-20
**Authors:** Eka design team
**Implementation:** C + libuv, register-based bytecode VM, NaN-boxing, mark-sweep GC

---

## Table of Contents

1. Philosophy (Hard Lock)
2. File Structure (Hard Lock)
3. Single-File Routing (Hard Lock)
4. Execution Model & Scoping (Hard Lock)
5. Syntax Reference (Hard Lock)
6. HTML Integration (Hard Lock)
7. HTTP Handling (Hard Lock)
8. SSE: Non-Blocking Event Streams (Hard Lock)
9. Builtins (24) (Hard Lock)
10. Client Runtime: Server-Driven Swap (Hard Lock)
11. Runtime Architecture (Hard Lock)
12. Single-File Examples (Hard Lock)
13. CLI (Hard Lock)
14. What We Are Not (Hard Lock)

---

## 1. Philosophy (Hard Lock)

| Principle | Value |
|-----------|-------|
| **Name** | Eka (Sanskrit: "one") |
| **Extension** | `.eka` |
| **Entry Point** | `app.eka` (single file, always) |
| **Static Assets** | `public/` directory only |
| **Max Project Size** | ~2,000 LOC, ~10 routes — one file |
| **Deployment** | `eka run` — done |
| **Dependencies** | Zero. 24 builtins + embedded client runtime only. |
| **Build Step** | None. Interpreted. |
| **Config Files** | None. CLI flags only. |
| **Package Manager** | None. |
| **Concurrency Model** | Single event loop, non-blocking I/O, hidden worker pool |

**The Hard Rule:** If you need more than one file, 2,000 lines, ~10 routes, or complex architecture, **Eka is not for you.** Use Next.js, Elixir Phoenix, PHP, Go, or whatever fits. Eka is for solo devs shipping a single-page app, a small SaaS, a landing page with a form, a personal blog. **Be transparent. If the user is building more, say so up front.**

**The Eka runtime is a single static binary** (~3–5 MB), written in **C** with **libuv**. It contains the compiler, the bytecode VM, the HTTP server, the SQLite driver, and the embedded client runtime JS. Zero runtime dependencies. Works on Linux, macOS, Windows.

---

## 2. File Structure (Hard Lock)

```
my-project/
├── app.eka          ← The ONE file. All code lives here.
└── public/          ← Static assets only (optional)
    ├── style.css
    ├── script.js
    └── images/
```

**NO other `.ek` files. NO `pages/` directory. NO `layout.eka`. NO includes. NO imports. NO modules. NO packages.**

Everything is in `app.eka`. The runtime reads exactly one file.

Static files in `public/` are served at root URL:
- `public/style.css` → `GET /style.css`
- `public/images/logo.png` → `GET /images/logo.png`

If the project grows beyond `app.eka`, **stop. Refactor to a real framework.**

---

## 3. Single-File Routing (Hard Lock)

Since there is only `app.eka`, routing is method-block + path based:

```eka
-- app.eka

@get /
  <h1>Home</h1>
@end

@get /about
  <h1>About</h1>
@end

@get /user/[id]
  <h1>User {{ request.param("id") }}</h1>
@end

@post /api/users
  @do
    let user = request.json()
    db.exec("INSERT INTO users (name) VALUES (?)", [user.name])
  @end
  {id: db.lastId(), name: user.name}
@end
```

### Routing Rules (Hard Lock)

| Rule | Behavior |
|------|----------|
| Path match is exact | `/about` matches `/about`, not `/about/` |
| Trailing slash mismatch | `301 Moved Permanently` to canonical, else `404` |
| Dynamic segments | `[id]` captures any non-slash segment |
| Multiple dynamic segments | `/user/[id]/post/[slug]` — both captured |
| No match | `404 Not Found` |
| Method mismatch | `405 Method Not Allowed` |
| No method blocks at all | `app.eka` acts as `@get /` only |

### Accessing Route Parameters

```eka
@get /user/[id]/post/[slug]
  let userId = request.param("id")
  let postSlug = request.param("slug")
@end
```

### API Routes (JSON Response)

Any method block whose last expression is a **map** or **list** auto-serializes to JSON:

```eka
@get /api/users
  @do
    let users = db.query("SELECT id, name FROM users")
  @end
  users  -- auto -> application/json
@end
```

To force JSON even for a single value, wrap it:

```eka
@get /api/count
  {count: db.query("SELECT COUNT(*) AS c FROM users")[0].c}
@end
```

To return raw text from any block, use `response`:

```eka
@get /health
  response.text("ok")
@end
```

To return an empty body (e.g. for `e-swap="delete"`):

```eka
@delete /todo/[id]
  @do
    db.exec("DELETE FROM todos WHERE id = ?", [request.param("id")])
  @end
  ""  -- empty string, e-swap="delete" removes target
@end
```

---

## 4. Execution Model & Scoping (Hard Lock)

### Two-Phase Execution

```
PHASE 1: INIT (runs once at server startup)
PHASE 2: REQUEST (runs per HTTP request, on a worker thread)
```

### Phase 1: Init Code

Code at the top level of `app.eka` (outside any method block) runs **once** at startup, on the main thread, before any request is accepted:

```eka
-- This runs ONCE when server starts
let db = sqlite.open("app.db")
db.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)")

let appName = "My App"
let secret = env.get("EKA_SECRET", crypto.randomBytes(32))
```

**Init scope = global scope.** Variables declared here are accessible in all request handlers.

**Init-time variables are read-only from request scope.** To mutate shared state across requests, use `cache` or `db`. The `let` keyword in init is effectively `const` for the request handlers (the binding can be reassigned in init, but the request handler sees the final value and cannot change it for other concurrent requests).

### Phase 2: Request Code

Code inside method blocks (`@get`, `@post`, etc.) runs **per request**, on a worker thread from the pool:

```eka
@get /
  -- This runs on every GET /, on a fresh worker thread
  let posts = db.query("SELECT * FROM posts")
@end
```

**Request scope = isolated.** Variables declared inside a method block live only for that request. No shared mutable state between requests (unless explicitly via `cache` or `db`).

### The `@do` Block (Hard Lock)

`@do` is a **silent code block** — it executes code without producing output:

```eka
@get /
  @do
    let posts = db.query("SELECT * FROM posts")
    let count = posts.length
  @end

  <h1>{{ appName }}</h1>
  <p>{{ count }} posts found</p>

  @for post in posts
    <article>{{ post.title }}</article>
  @end
@end
```

`@do` inside a method block = request-local computation.
`@do` at top level (outside method blocks) = init code (same as bare top-level code).

---

## 5. Syntax Reference (Hard Lock)

### Comments

```eka
-- This is a line comment
-- No block comments. Keep it simple.
```

### Variables

```eka
let name = "Alice"        -- mutable, request-local or init-global
const PI = 3.14159        -- immutable, cannot reassign
```

`let` at init scope is effectively read-only from request scope. `let` at request scope is request-local. There is no shared mutable state across requests.

### Types (Optional Annotations)

```eka
let str: string = "hello"
let num: number = 42
let bool: bool = true
let items: list<string> = ["a", "b", "c"]
let user: map = {name: "Bob", age: 25}
let maybeName: maybe<string> = null
```

**Types are annotations only.** The runtime is fully dynamic. Types are used by `eka check` for linting and IDE support. **No runtime type enforcement.**

**Built-in type names:** `string`, `number`, `bool`, `list<T>`, `map<K,V>`, `maybe<T>`, `func`.

### Strings

```eka
let single = "hello"
let interp = "Hello, ${name}!"
let multiline = "Line 1
Line 2
Line 3"
let escaped = "Use \${name} for literal dollar-brace"
```

String interpolation uses `${expression}`. To escape: `\${}`.

### Numbers

```eka
let n = 42
let f = 3.14
let hex = 0xFF
let neg = -5
```

All numbers are 64-bit floats (IEEE 754). Integer division: use `math.floor(a / b)` (future `math` builtin — for now, no `math` builtin exists, write your own helper).

### Lists

```eka
let items = [1, 2, 3]
items.push(4)          -- mutates, returns null
items.remove(1)        -- removes index 1, returns null
let first = items[0]   -- index access, 0-based
let len = items.length -- property, not method
let last = items[-1]   -- negative indexing supported
```

### Maps

```eka
let user = {name: "Alice", age: 30}
let name = user.name       -- dot access
let age = user["age"]      -- bracket access (for dynamic keys)
user.email = "alice@example.com"  -- assignment
let missing = user.phone   -- null (no error)
```

### Booleans & Logic

```eka
let a = true
let b = false

if a and b
if a or b
if not a
```

### Comparison

```eka
if x == y        -- strict equality (type + value)
if x ~= y        -- loose equality (coerced)
if x > y
if x >= y
if x < y
if x <= y
if x != y        -- strict inequality
```

### Null-Coalescing

```eka
let name = request.query("name") ?? "World"
```

`??` checks for `null` only. `0`, `""`, `false` are NOT coalesced.

### Functions

```eka
func greet(name: string): string
  "Hello, ${name}!"
end

func add(a: number, b: number): number
  a + b
end

-- Named parameters ONLY (self-documenting)
func createUser(name: string, age: number, role: string = "user")
  {name: name, age: age, role: role}
end

let u = createUser(name: "Alice", age: 30)
```

**Implicit return:** The last expression in a function is its return value.
**Explicit return:** Use `return value` for early exit. `return` alone returns `null`.

```eka
func findUser(id: number)
  if id < 1
    return null
  end
  db.query("SELECT * FROM users WHERE id = ?", [id])[0]
end
```

### Control Flow

```eka
if condition
  ...
else if other
  ...
else
  ...
end

for item in list
  ...
end

while condition
  ...
end
```

### Error Handling

```eka
try
  let result = risky()
catch err
  print("Error: ${err}")
end
```

- `err` is always a **string** (the error message).
- Catchable: runtime errors (undefined variable, DB error, file not found, etc.).
- Uncatchable: syntax errors (caught at `eka check` time).
- No re-throw. If you need to propagate, call another function.

---

## 6. HTML Integration (Hard Lock)

### Variable Interpolation

```eka
<h1>Hello, {{ name }}</h1>
<p>Count: {{ items.length }}</p>
```

**Auto-escaped by default.** XSS-safe. HTML special chars (`<`, `>`, `&`, `"`, `'`) are escaped.

**Interpolation works inside HTML attributes too:**

```html
<button e-delete="/todo/${todo.id}">Delete</button>
```

This is allowed — the runtime interpolates `${...}` in any text position, including attribute values.

### Raw HTML Output

```eka
<p>{{ html.raw(user.bio) }}</p>
```

Use `html.raw()` to bypass escaping. Dangerous — only use with trusted content.

### Logic Blocks

```eka
@if user.isAdmin
  <div class="admin-panel">Admin</div>
@end

@for post in posts
  <article>
    <h2>{{ post.title }}</h2>
  </article>
@end
```

### Silent Code Blocks

```eka
@do
  let db = sqlite.open("data.db")
  let posts = db.query("SELECT * FROM posts")
@end
```

### Raw Passthrough Tags

The following HTML tags have their contents passed through **verbatim** (no Eka parsing inside):

- `<script>` — JavaScript code
- `<style>` — CSS code
- `<pre>` — Preformatted text
- `<textarea>` — Form text
- `<code>` — Inline code

```eka
<script>
  // This is JavaScript, not Eka
  let x = "{{ name }}";  -- This is NOT interpolated!
</script>
```

If you need dynamic values inside `<script>`, write vanilla JS that reads from `window.e` or your own data attributes. See Section 10.

---

## 7. HTTP Handling (Hard Lock)

### Implicit Objects

Every request handler has access to:
- `request` — HTTP request
- `response` — HTTP response

### Request API

```eka
request.path              -- "/user/42"
request.query("q")        -- URL query param (string or null)
request.query("q", "default") -- With default value
request.form()            -- POST form data (map)
request.json()            -- POST JSON body (map or list)
request.file("avatar")    -- {name, type, size, path, bytes} or null
request.header("X-Token") -- Header value (string or null)
request.param("id")       -- Route parameter from [id]
request.isHtmx            -- true if HX-Request header present (for compat)
request.method            -- "GET", "POST", etc.
```

### Response API

```eka
response.status(404)                    -- Set status code
response.redirect("/")                  -- 302 redirect
response.header("X-Custom", "value")    -- Set header
response.cookie("name", "value")        -- Set cookie
response.text("plain text")             -- Return raw text (bypasses HTML/JSON auto)
response.json({key: "value"})           -- Explicit JSON (same as returning map)
```

### File Uploads

```eka
let file = request.file("avatar")
if file
  fs.move(file.path, "public/uploads/" + file.name)
end
```

Uploaded files are stored in a temp directory. `file.path` is valid only during the request. Use `fs.move()` to persist.

---

## 8. SSE: Non-Blocking Event Streams (Hard Lock)

Eka uses a **single event loop** (the libuv main loop, plus a hidden worker thread pool) with non-blocking I/O. SSE is a **special runtime-managed feature** that does NOT block the event loop.

### How It Works

The runtime maintains an internal **SSE registry** (a map of active connections, protected by a mutex). When a client connects to an SSE endpoint, the runtime:

1. Registers the connection in the registry
2. Keeps the HTTP connection alive (long-lived, no timeout)
3. Returns control to the event loop immediately
4. Sends data to the client asynchronously when `sse.broadcast()` or `sse.send()` is called

The connection stays on a dedicated libuv TCP handle. Sending to many connections at once iterates the registry under a short critical section.

### SSE API

```eka
-- SSE endpoint: clients connect here
@get /events
  sse.connect()  -- Registers client, yields immediately. Non-blocking.
@end

-- Broadcast to all connected clients
@post /notify
  @do
    let msg = request.json()
    sse.broadcast("update", json.stringify(msg))
  @end
  {sent: true, listeners: sse.count()}
@end
```

| Method | Description |
|--------|-------------|
| `sse.connect()` | In `@get` route. Registers client, yields to event loop. Non-blocking. |
| `sse.send(event, data)` | Send to the current SSE client only. |
| `sse.broadcast(event, data)` | Send to ALL connected SSE clients. |
| `sse.count()` | Number of active SSE connections. |

### SSE Event Format

Data sent via `sse.send()` or `sse.broadcast()` is formatted as:
```
event: <event-name>\n
data: <data-string>\n\n
```

### Important Constraints

- SSE endpoints MUST be `@get` only.
- `sse.connect()` can only be called once per request.
- If `sse.connect()` is called, the response is automatically `Content-Type: text/event-stream`.
- SSE connections auto-close on client disconnect (detected by runtime via TCP read returning 0 or error).
- Maximum SSE connections: configurable via `EKA_MAX_SSE` env var, default **1000**.

---

## 9. Builtins (24) (Hard Lock)

The runtime ships with **24 builtins.** No more, no less. There is no `ui` builtin; server-driven UI updates are handled by the embedded client runtime at `/_eka.js` (see Section 10).

### 1. `request`
HTTP request object. See Section 7.

### 2. `response`
HTTP response control. See Section 7.

### 3. `sqlite`
SQLite database.
```eka
let db = sqlite.open("app.db")
db.query("SELECT * FROM users WHERE id = ?", [42])     -- Returns list of maps
db.exec("INSERT INTO users (name) VALUES (?)", ["Alice"]) -- Returns null
db.lastId()                                              -- Last inserted row ID
```

### 4. `json`
```eka
let obj = json.parse('{"a": 1}')
let str = json.stringify(obj)
```

### 5. `crypto`
```eka
let hash = crypto.sha256("secret")
let random = crypto.randomBytes(16)
let hmac = crypto.hmac("sha256", key, message)
```

### 6. `markdown`
```eka
let html = markdown.parse("# Hello\\n\\nWorld")
```

### 7. `html`
```eka
let safe = html.escape("<script>alert('xss')</script>")
let raw = html.raw("<b>Bold</b>")  -- Bypass escaping in {{ }}
```

### 8. `http`
HTTP client.
```eka
let res = http.get("https://api.example.com/data")
let data = res.json()
```

`res` is a map: `{status: number, body: string, headers: map}`

### 9. `fs`
File system (sandboxed to project directory).
```eka
let content = fs.read("notes.txt")
fs.write("output.txt", "Hello")
fs.move("old.txt", "new.txt")
fs.exists("file.txt")  -- true/false
fs.delete("file.txt")
```

### 10. `env`
```eka
let dbUrl = env.get("DATABASE_URL", "localhost")
```

### 11. `datetime`
```eka
let now = datetime.now()
let formatted = now.format("YYYY-MM-DD HH:mm:ss")
let parsed = datetime.parse("2024-01-01", "YYYY-MM-DD")
```

### 12. `regex`
```eka
let m = regex.match("^hello", "hello world")   -- Returns match string or null
let replaced = regex.replace("foo", "foo bar", "baz")
```

### 13. `base64`
```eka
let encoded = base64.encode("hello")
let decoded = base64.decode(encoded)
```

### 14. `url`
```eka
let parsed = url.parse("https://example.com/path?q=1")
let built = url.build({host: "example.com", path: "/api"})
```

### 15. `session`
Cookie-based sessions (server-side, signed cookie).
```eka
session.set("user_id", 42)
let id = session.get("user_id")
session.delete("user_id")
```

Session data stored server-side (memory or SQLite). Session ID is a signed cookie.

### 16. `cache`
In-memory key-value cache, **SQLite-backed by default** for multi-process safety.
```eka
cache.set("key", "value", 300)  -- TTL in seconds
cache.set("key", "value")       -- No TTL, lives until explicitly deleted
let val = cache.get("key")      -- Returns value or null
cache.delete("key")
```

`cache` uses a SQLite file in the project directory by default. Set `EKA_CACHE=memory` to use in-process memory (faster, but split-brain across processes).

### 17. `email`
```eka
email.send({
  to: "user@example.com",
  from: "noreply@example.com",
  subject: "Hello",
  body: "Welcome!",
  smtp: "smtp.example.com:587"
})
```

### 18. `validate`
```eka
validate.email("test@example.com")   -- true/false
validate.url("https://example.com")  -- true/false
validate.required("")                -- false
```

### 19. `slug`
```eka
let slug = slug.make("Hello World!")  -- "hello-world"
```

### 20. `i18n`
```eka
i18n.set("en")
let greeting = i18n.t("hello")  -- Looks up translations/i18n/en.json
```

Translation files live in `translations/i18n/<lang>.json`.

### 21. `sse`
Server-Sent Events. See Section 8.

### 22. `rss`
```eka
let feed = rss.generate({
  title: "My Blog",
  items: [{title: "Post 1", link: "/1", pubDate: "2024-01-01"}]
})
```

### 23. `sitemap`
```eka
let map = sitemap.generate([
  {url: "/", lastmod: "2024-01-01"},
  {url: "/about", lastmod: "2024-01-01"}
])
```

### 24. `template`
**DEPRECATED in single-file model.** Since Eka is strictly one file, `template.render()` is not available. Use functions instead:
```eka
func header(title: string)
  <header><h1>{{ title }}</h1></header>
end
```

---

## 10. Client Runtime: Server-Driven Swap (Hard Lock)

Eka ships an **embedded client runtime** at `/_eka.js`. It is a hand-written JavaScript file (target: **500 LOC, 8–12 KB gzipped**), embedded in the Eka binary as a string constant. Zero external CDN, zero dependencies.

The runtime implements a **server-driven swap** model: the server returns HTML, the client swaps it into the DOM. There is **no reactive state, no signals, no `x-data`, no virtual DOM**. That is the user's responsibility — they write vanilla JS if they need client state.

### 10.1 Runtime Delivery

- **URL:** `GET /_eka.js`
- **Served by:** the Eka runtime itself (virtual route)
- **Caching:** `Cache-Control: public, max-age=3600, immutable`, versioned in the URL
- **Content-Encoding:** `gzip` when client supports
- **Browser baseline:** ES2020 (`Proxy`, `fetch`, `IntersectionObserver`, `AbortController`)

### 10.2 Auto-Injection

When the runtime sends an HTML response, it scans the response body **once, after the route handler returns, before serialization**:

```
regex: \be-[a-z][a-z0-9-]*=
```

If the regex matches ≥1 attribute, Eka injects exactly one tag into the document `<head>`:

```html
<script src="/_eka.js" defer></script>
```

**No `e-*` attrs in response → no script tag injected → zero overhead.**

**Manual opt-out:** set response header `X-Eka-Runtime: skip`.

**Manual opt-in** (for `public/` static files): include the `<script>` tag yourself.

### 10.3 Attribute Set (Hard Lock)

All attributes are prefixed `e-`. The runtime is dormant until one of these is encountered.

| Attribute | Applies to | Description |
|-----------|------------|-------------|
| `e-get="<url>"` | any | Send GET on trigger, swap response into `e-target` |
| `e-post="<url>"` | form, any | Send POST (form-encoded or JSON) |
| `e-put="<url>"` | form, any | Send PUT |
| `e-delete="<url>"` | form, any | Send DELETE |
| `e-patch="<url>"` | form, any | Send PATCH |
| `e-target="<css-selector>"` | any | **Required.** Where to put the response. |
| `e-swap="<mode>"` | any | Swap strategy. Default: `innerHTML`. |
| `e-trigger="<event-list>"` | any | Comma-separated events. Defaults by element type. |
| `e-include="<css-selector>"` | any | Include additional form fields / inputs from elsewhere. |
| `e-confirm="<message>"` | any | Show `confirm()` dialog before sending. |
| `e-error-target="<css-selector>"` | any | Where to swap error responses. |
| `e-error-swap="<mode>"` | any | Swap mode for errors. Default: `innerHTML`. |

**No other `e-*` attributes exist.** Unknown attributes are ignored.

### 10.4 Swap Strategies (Hard Lock)

Five strategies + `none`:

| Mode | Behavior |
|------|----------|
| `innerHTML` | (default) Replace `innerHTML` of target. |
| `outerHTML` | Replace the target element itself. |
| `beforeend` | Append response as last child of target. |
| `afterbegin` | Prepend response as first child of target. |
| `delete` | Remove target from DOM. Response body ignored. |
| `none` | Do nothing with response. Useful for fire-and-forget. |

### 10.5 Trigger Events (Hard Lock)

Five event sources:

| Trigger | When | Default for |
|---------|------|-------------|
| `click` | click on element | `<button>`, `<a>`, `<div>`, etc. |
| `change` | value changes | `<input>`, `<select>`, `<textarea>` |
| `submit` | form submitted | `<form>` |
| `load` | once on DOMContentLoaded | `<body>`, `<html>` |
| `revealed` | element scrolled into view | (opt-in) |

`e-trigger` is a comma-separated list: `e-trigger="click, focus"`.

**No polling.** Users who need polling write 4 lines of JS using `setInterval` + `e.fetch`.

### 10.6 Request Encoding (Hard Lock)

- **Form elements** (`<form>`, `<button>`, `<input type="submit">`): encoding is `application/x-www-form-urlencoded`. Form is prevented from default-submitting.
- **Non-form elements**: no body. `e-include` selector's values are added as `?key=value&...`.
- **All non-GET requests** include header `X-Eka-Request: 1` (for CSRF defense — see Security).
- **All requests** include `X-Requested-With: XMLHttpRequest`.

### 10.7 Public API (Hard Lock)

The runtime exposes `window.e`:

```js
window.e = {
  version,           // "1.0.0"
  fetch(url, opts),  // internal fetch using runtime's pipe
  swap(sel, html, mode), // manual swap
  on(event, fn)      // listen to runtime events
}
```

Events (all bubble, all prefixed `e:`):
- `e:before-request` — before `fetch()` is called
- `e:after-request` — after response received, before swap
- `e:after-swap` — after swap completes (fires even for `swap="none"` with `html: null`)
- `e:request-error` — network error or 4xx/5xx

### 10.8 Loading State (Hard Lock)

During an in-flight request, the runtime applies exactly one class:

```css
.e-busy { /* user styles this */ }
```

To the **triggering element**, removed on completion.

### 10.9 Errors (Hard Lock)

- **Default** (no `e-error-target`): `console.warn`, no swap.
- **With `e-error-target`**: error response is swapped into the target using `e-error-swap` mode.
- **Default timeout:** 30 seconds. Override with `data-e-timeout="<ms>"` per element.
- **`e:request-error` always fires.**

### 10.10 Security (Hard Lock)

- **No auto-CSRF.** Users add a hidden field with `session.get("csrf")` token. Eka's `response.cookie` defaults to `SameSite=Lax`.
- **No auto-escape in `e-swap`.** `innerHTML` is set as-is. Server is responsible for escaping via `html.escape()`.
- **CSP-friendly.** Runtime uses no `eval`, no `Function()`, no inline scripts. Works with `script-src 'self'`.

### 10.11 Worked Example

```eka
-- app.eka

@get /
  <!DOCTYPE html>
  <html>
  <head><title>Todo</title>
    <style>
      .e-busy { opacity: 0.4; }
      li { padding: 4px 8px; }
      .done { text-decoration: line-through; color: #999; }
    </style>
  </head>
  <body>
    <h1>Todos</h1>

    <ul id="todos">
      @do
        let todos = db.query("SELECT * FROM todos ORDER BY id DESC")
      @end
      @for t in todos
        <li id="todo-{{ t.id }}" class="{{ if t.done }}done{{ end }}">
          <input type="checkbox"
                 {{ if t.done }}checked{{ end }}
                 e-post="/todo/{{ t.id }}/toggle"
                 e-target="#todo-{{ t.id }}"
                 e-swap="outerHTML">
          {{ t.text }}
          <button e-delete="/todo/{{ t.id }}"
                  e-target="#todo-{{ t.id }}"
                  e-swap="delete"
                  e-confirm="Delete this?">×</button>
        </li>
      @end
    </ul>

    <form e-post="/todo" e-target="#todos" e-swap="afterbegin">
      <input name="text" required>
      <button>Add</button>
    </form>

    <div id="clock" e-get="/time" e-trigger="load" e-swap="innerHTML"></div>
  </body>
  </html>
@end

@get /time
  datetime.now().format("HH:mm:ss")
@end

@post /todo
  @do
    let text = request.form().text
    db.exec("INSERT INTO todos (text, done) VALUES (?, 0)", [text])
    let id = db.lastId()
  @end
  <li id="todo-{{ id }}">
    <input type="checkbox" e-post="/todo/{{ id }}/toggle" e-target="#todo-{{ id }}" e-swap="outerHTML">
    {{ html.escape(text) }}
    <button e-delete="/todo/{{ id }}" e-target="#todo-{{ id }}" e-swap="delete" e-confirm="Delete?">×</button>
  </li>
@end

@post /todo/[id]/toggle
  @do
    let id = request.param("id")
    db.exec("UPDATE todos SET done = NOT done WHERE id = ?", [id])
    let t = db.query("SELECT * FROM todos WHERE id = ?", [id])[0]
  @end
  <li id="todo-{{ t.id }}" class="{{ if t.done }}done{{ end }}">
    <input type="checkbox"
           {{ if t.done }}checked{{ end }}
           e-post="/todo/{{ t.id }}/toggle"
           e-target="#todo-{{ t.id }}"
           e-swap="outerHTML">
    {{ t.text }}
    <button e-delete="/todo/{{ t.id }}" e-target="#todo/{{ t.id }}" e-swap="delete" e-confirm="Delete?">×</button>
  </li>
@end

@delete /todo/[id]
  @do
    db.exec("DELETE FROM todos WHERE id = ?", [request.param("id")])
  @end
  ""  -- empty body, e-swap="delete" ignores it
@end
```

---

## 11. Runtime Architecture (Hard Lock)

### Implementation Stack

| Layer | Technology | Reason |
|-------|------------|--------|
| Language | **C11** | No GC fights, no async churn, mature toolchain |
| Async I/O | **libuv** | 14 years mature, cross-platform, thread pool built-in |
| HTTP server | Custom on libuv | Tiny, ~500 LOC, no framework overhead |
| SQL | SQLite (via libsqlite3) | Zero-config, embedded, fast for single-user apps |
| Regex | PCRE2 | Standard, fast |
| Crypto | OpenSSL (libcrypto only) | SHA-256, HMAC, random |
| JSON | yyjson or cJSON | Fast, small |
| Markdown | cmark | Standard CommonMark |
| Email | libcurl + libsmtp, or vendored mini-SMTP | Embedded |
| VM | Custom, register-based | Target ~5,000 LOC of C |
| Client runtime JS | Hand-written ES2020 | ~500 LOC, embedded as C string |

### Process Model (Hard Lock)

```
┌──────────────────────────────────────────────┐
│           eka process (single binary)        │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │  Main thread: libuv event loop         │  │
│  │  - TCP accept (HTTP)                   │  │
│  │  - SSE connection management           │  │
│  │  - Idle / timeout timers               │  │
│  │  - File watch (dev mode only)          │  │
│  │  - NEVER runs user code                │  │
│  └────────────────────────────────────────┘  │
│                     │                        │
│                     │ dispatch                │
│                     ▼                        │
│  ┌────────────────────────────────────────┐  │
│  │  Worker thread pool (N = min(CPUs,32)) │  │
│  │  - One worker per HTTP request         │  │
│  │  - User code runs here, sync-looking   │  │
│  │  - Blocking I/O OK (only blocks its    │  │
│  │    own worker, not others)             │  │
│  │  - DB calls, file I/O, HTTP client     │  │
│  │    all block this thread               │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  Configurable via EKA_THREAD_POOL env var.   │
│  Default: min(nproc, 32).                    │
└──────────────────────────────────────────────┘
```

**User code is single-threaded (per request), synchronous-looking, and never sees async/await.** The "single event loop, non-blocking I/O" promise is preserved at the user-code level. The libuv main thread handles I/O multiplexing; the worker pool handles blocking calls.

This is the **Node.js / Mummy model**: one I/O loop + worker pool. The difference is that Eka exposes the synchronous-looking API directly (no `await`, no `Future`, no callback). The user writes `let users = db.query(...)` and it just works.

### VM Design (Hard Lock)

| Feature | Choice |
|---------|--------|
| Architecture | Register-based bytecode |
| Stack | None (registers live in a C struct per request) |
| Value representation | NaN-boxed 64-bit |
| GC | Mark-sweep, generational not needed for v1 |
| GC trigger | When allocated > 1 MB or on request boundary |
| Max registers per function | 256 |
| Bytecode format | 32-bit instructions: `opcode | A | B | C` |
| Constants | Pool per function (strings, numbers) |
| Closures | Upvalues via capture list |
| Tail calls | Not optimized in v1 (stack-limited) |

### NaN-Boxing (Hard Lock)

A 64-bit value is interpreted as:

```
if value is a double NaN (quiet NaN):
   extract 51-bit payload → pointer or integer
else:
   the value is a regular IEEE 754 double
```

Payload encoding:
- bit 0 = 0 → tagged pointer to heap object (48 bits usable)
- bit 0 = 1 → small integer (up to 2^50)

This gives:
- `nil` = a specific quiet NaN
- `true` / `false` = specific quiet NaN
- `int` = 51-bit unsigned
- `double` = normal IEEE 754
- `string` / `list` / `map` = pointer

**One machine word per value. No boxing overhead in the common case.**

### Memory Model (Hard Lock)

- Init code runs once on main thread, before any request. Result is a frozen global object.
- Each request gets a fresh VM state (registers, stack, locals) but **shares the global object by reference** (read-only).
- GC roots: global object, current request's stack/registers, SSE registry.
- Heap: bump allocator with mark-sweep, ~1 MB arenas, freed on sweep.
- No concurrent GC. Worker threads do their own GC at request boundary.

### File Watcher (Hard Lock, dev mode only)

- `eka run` watches `app.eka` via `libuv fs_event`.
- On change: re-parse, re-compile, swap the global object atomically.
- SSE clients get a reconnect hint; new requests use the new code.
- In production (`EKA_ENV=production`), no watcher. Restart to update.

### CLI Commands (Hard Lock)

```bash
eka run                    # Run server (reads app.eka, default port 8080)
eka run --port 3000        # Custom port
eka run --static ./public  # Static files directory (default: auto-detect public/)
eka check                  # Syntax + type lint check on app.eka
eka fmt                    # Format app.eka
```

### Environment Variables

```bash
EKA_PORT=3000 eka run
EKA_STATIC=./assets eka run       # Static assets directory
EKA_MAX_SSE=500 eka run           # Max SSE connections
EKA_SECRET=<key> eka run          # Session signing key
EKA_THREAD_POOL=16 eka run        # Worker thread count (default: min(nproc, 32))
EKA_CACHE=memory eka run          # Force in-memory cache (default: SQLite-backed)
EKA_ENV=production eka run        # Disable file watcher
```

---

## 12. Single-File Examples (Hard Lock)

### Example 1: Coming Soon Page

```eka
-- app.eka
-- Zero dependencies, zero config

@get /
  <!DOCTYPE html>
  <html>
  <head>
    <title>Launching Soon</title>
    <style>
      body { font-family: system-ui; text-align: center; padding: 20vh 2rem; }
      h1 { font-size: 3rem; margin-bottom: 1rem; }
      p { color: #666; }
    </style>
  </head>
  <body>
    <h1>Coming Soon</h1>
    <p>We are building something awesome.</p>
    <p>Check back on {{ datetime.now().format("MMMM YYYY") }}</p>
  </body>
  </html>
@end
```

### Example 2: Blog with SQLite

```eka
-- app.eka

@do
  let db = sqlite.open("blog.db")
  db.exec("CREATE TABLE IF NOT EXISTS posts (id INTEGER PRIMARY KEY, title TEXT, body TEXT, created_at TEXT)")
@end

@get /
  <!DOCTYPE html>
  <html>
  <head><title>My Blog</title>
    <style>
      body { font-family: system-ui; max-width: 40rem; margin: 4rem auto; padding: 0 1rem; }
      article { background: #f9f9f9; padding: 1rem; margin: 1rem 0; border-radius: 4px; }
      form { display: flex; flex-direction: column; gap: 0.5rem; margin-top: 2rem; }
      input, textarea, button { padding: 0.5rem; font: inherit; }
      button { background: #2563eb; color: white; border: none; border-radius: 4px; cursor: pointer; }
    </style>
  </head>
  <body>
    <h1>My Blog</h1>

    <div id="posts">
      @do
        let posts = db.query("SELECT * FROM posts ORDER BY created_at DESC")
      @end
      @for post in posts
        <article>
          <h2>{{ post.title }}</h2>
          <div>{{ markdown.parse(post.body) }}</div>
          <small>{{ post.created_at }}</small>
        </article>
      @end
    </div>

    <form e-post="/post" e-target="#posts" e-swap="afterbegin">
      <input name="title" placeholder="Title" required>
      <textarea name="body" placeholder="Body"></textarea>
      <button>Publish</button>
    </form>
  </body>
  </html>
@end

@post /post
  @do
    let form = request.form()
    let now = datetime.now().format("YYYY-MM-DD HH:mm:ss")
    db.exec("INSERT INTO posts (title, body, created_at) VALUES (?, ?, ?)",
            [form.title, form.body, now])
    let id = db.lastId()
  @end
  <article>
    <h2>{{ form.title }}</h2>
    <div>{{ markdown.parse(form.body) }}</div>
    <small>{{ now }}</small>
    <button e-delete="/post/{{ id }}" e-target="closest article" e-swap="delete" e-confirm="Delete?">Delete</button>
  </article>
@end

@delete /post/[id]
  @do
    db.exec("DELETE FROM posts WHERE id = ?", [request.param("id")])
  @end
  ""
@end
```

### Example 3: Real-Time Chat with SSE

```eka
-- app.eka

@do
  let db = sqlite.open("chat.db")
  db.exec("CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, text TEXT, created_at TEXT)")
@end

@get /
  <!DOCTYPE html>
  <html>
  <head><title>Chat</title>
    <style>
      body { font-family: system-ui; max-width: 30rem; margin: 4rem auto; padding: 0 1rem; }
      #messages { display: flex; flex-direction: column; gap: 0.5rem; max-height: 24rem; overflow-y: auto; }
      .msg { background: #f9f9f9; padding: 0.5rem 1rem; border-radius: 4px; }
      form { display: flex; gap: 0.5rem; margin-top: 1rem; }
      input { flex: 1; padding: 0.5rem; }
      button { padding: 0.5rem 1rem; background: #2563eb; color: white; border: none; border-radius: 4px; }
    </style>
  </head>
  <body>
    <h1>Chat</h1>
    <div id="messages">
      @do
        let msgs = db.query("SELECT * FROM messages ORDER BY created_at DESC LIMIT 50")
      @end
      @for msg in msgs
        <div class="msg">
          <p>{{ html.escape(msg.text) }}</p>
          <small>{{ msg.created_at }}</small>
        </div>
      @end
    </div>
    <form e-post="/send" e-target="#messages" e-swap="afterbegin">
      <input name="text" placeholder="Type..." required>
      <button>Send</button>
    </form>

    <script>
      const source = new EventSource('/events');
      source.addEventListener('message', (e) => {
        const msg = JSON.parse(e.data);
        const div = document.createElement('div');
        div.className = 'msg';
        div.innerHTML = '<p>' + msg.text + '</p><small>' + msg.time + '</small>';
        document.getElementById('messages').prepend(div);
      });
    </script>
  </body>
  </html>
@end

@get /events
  sse.connect()
@end

@post /send
  @do
    let text = request.form().text
    let now = datetime.now().format("HH:mm:ss")
    db.exec("INSERT INTO messages (text, created_at) VALUES (?, ?)", [text, now])
    sse.broadcast("message", json.stringify({text: text, time: now}))
  @end
  <div class="msg">
    <p>{{ html.escape(text) }}</p>
    <small>{{ now }}</small>
  </div>
@end
```

### Example 4: Contact Form with Email

```eka
-- app.eka

@get /
  <!DOCTYPE html>
  <html>
  <body style="font-family: system-ui; max-width: 30rem; margin: 4rem auto; padding: 0 1rem;">
    <h1>Contact Us</h1>

    @if request.query("sent")
      <div style="background: #d1fae5; color: #065f46; padding: 0.5rem 1rem; border-radius: 4px; margin-bottom: 1rem;">
        Message sent!
      </div>
    @end

    <form method="post" action="/contact" style="display: flex; flex-direction: column; gap: 0.5rem;">
      <input name="name" placeholder="Name" required>
      <input name="email" type="email" placeholder="Email" required>
      <textarea name="message" placeholder="Message" required></textarea>
      <button style="background: #2563eb; color: white; border: none; padding: 0.5rem; border-radius: 4px;">
        Send
      </button>
    </form>
  </body>
  </html>
@end

@post /contact
  @do
    let form = request.form()
    if validate.email(form.email) and validate.required(form.message)
      email.send({
        to: "admin@example.com",
        from: form.email,
        subject: "Contact from ${form.name}",
        body: form.message,
        smtp: env.get("SMTP_SERVER")
      })
      response.redirect("/?sent=1")
    end
  @end
@end
```

---

## 13. CLI (Hard Lock)

```bash
eka run                    # Run server (reads app.eka, default port 8080)
eka run --port 3000        # Custom port
eka run --static ./public  # Serve static files (default: auto-detect public/)
eka check                  # Syntax + type lint check on app.eka
eka fmt                    # Format app.eka
```

### Environment Variables

```bash
EKA_PORT=3000 eka run
EKA_STATIC=./assets eka run       # Static assets directory
EKA_MAX_SSE=500 eka run           # Max SSE connections
EKA_SECRET=<key> eka run          # Session signing key
EKA_THREAD_POOL=16 eka run        # Worker thread count (default: min(nproc, 32))
EKA_CACHE=memory eka run          # Force in-memory cache (default: SQLite-backed)
EKA_ENV=production eka run        # Disable file watcher
```

---

## 14. What We Are Not (Hard Lock)

Eka explicitly does NOT include:

- ❌ **Multiple files** — One `app.eka` only
- ❌ **File imports/includes** — No `import`, `require`, `template.render()`
- ❌ **Package manager** — 24 builtins + embedded client runtime
- ❌ **Build step** — Interpreted, not compiled
- ❌ **Type enforcement** — Optional annotations, dynamic runtime
- ❌ **Async/await** — Single event loop + hidden worker pool
- ❌ **Module system** — Functions are your modules
- ❌ **ORM** — Raw SQL with parameter binding
- ❌ **Middleware chain** — Direct HTTP blocks
- ❌ **Plugin system** — No extensions
- ❌ **Framework on top** — Just the language + server
- ❌ **Deployment platform** — `eka run` anywhere
- ❌ **WebSocket** — SSE only (simpler, HTTP-compatible)
- ❌ **Default UI libraries** — All OFF, explicit opt-in via `e-*` runtime
- ❌ **HTMX / Alpine / Tailwind CDN** — Server-driven swap via embedded `/_eka.js`
- ❌ **Client reactive state** — User writes vanilla JS if needed
- ❌ **Classical multithreading** — Worker pool, no user-visible threads

If you need any of these, Eka is not for you. Use a battle-tested stack.

---

> **Eka — Locked V1.**
> *One file. One truth. `eka run` and ship it.*
