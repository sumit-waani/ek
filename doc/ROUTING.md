# Eka — Routing & HTTP

## File Structure

```
my-project/
├── app.eka          ← The ONE file. All code lives here.
└── public/          ← Static assets only (optional)
    ├── style.css
    ├── script.js
    └── images/
        └── logo.png
```

**NO other `.eka` files. NO `pages/` directory. NO `layout.eka`. NO imports. NO modules.**

Everything is in `app.eka`. The runtime reads exactly one file.

## Init Code

Code at the top level of `app.eka` (outside any method block) runs **once at startup**:

```eka
-- app.eka — init code goes here (bare, no wrapper)
let db = sqlite.open("app.db")
db.exec("CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, text TEXT, done INTEGER DEFAULT 0)")
let appName = "Todo App"
let secret = env.get("EKA_SECRET", crypto.randomBytes(32))
```

Init variables are:
- Accessible in all request handlers (global scope)
- Read-only from request scope (cannot be mutated across requests)
- **Snapshot copies:** Each request handler receives a copy of init variables. Mutations in a handler are discarded after the response. Use `cache`, `session`, or `db` for shared mutable state.

`@do` blocks at top level are **not allowed.** For init code, write bare expressions. `@do` is only valid inside method blocks.

## Method Blocks (Routes)

Routes are method-block + path based:

```eka
@get /
  <h1>Home</h1>
@end

@get /about
  <h1>About Us</h1>
@end

@post /api/users
  @do
    let data = request.json()
    db.exec("INSERT INTO users (name) VALUES (?)", [data.name])
  @end
  {id: db.lastId(), name: data.name}
@end

@delete /todo/[id]
  @do
    db.exec("DELETE FROM todos WHERE id = ?", [request.param("id")])
  @end
  ""
@end
```

Available method blocks: `@get`, `@post`, `@put`, `@delete`, `@patch`.

If `app.eka` has no method blocks at all, it acts as `@get /` only (the entire file is served as HTML).

## Route Parameters

```eka
@get /user/[id]
  let userId = request.param("id")
  ...
@end

@get /user/[id]/post/[slug]
  let userId = request.param("id")
  let postSlug = request.param("slug")
  ...
@end
```

- `[id]` captures any non-slash segment.
- Multiple dynamic segments are supported.
- Access via `request.param("name")` → string or null.

## Path Matching (Exact)

| Request | `@get /about` | `@get /about/team` |
|---------|---------------|-------------------|
| `/about` | ✅ Match | ❌ 404 |
| `/about/` | 301 → `/about` | ❌ 404 |
| `/about?q=1` | ✅ Match | ❌ 404 |
| `/about/team` | ❌ 404 | ✅ Match |

- Path matching is **segment-exact.** The number of segments must match.
- Trailing slash mismatch → 301 redirect to canonical path.
- Query strings are ignored for route matching.
- No route matches → 404.
- Method mismatch (e.g., `POST` to `@get`) → 405 Method Not Allowed.
- Parameterized segments `[id]` match exactly one segment of any value.

## Response Auto-Detection

The return value of a method block determines the response:

| Last expression type | Auto-behavior |
|---------------------|---------------|
| `map` or `list` | `Content-Type: application/json`, JSON-serialized |
| Anything else (string, number, null, bool) | `Content-Type: text/html`, stringified |

### Explicit Overrides

```eka
response.json(value)   -- Force JSON, regardless of type
response.html(value)   -- Force HTML, regardless of type
```

### Empty Responses

```eka
response.status(204)   -- No content (for deletes, fire-and-forget)
""                     -- Empty string body (200 with no visible content)
```

## Static Files

Files in `public/` are served at the root URL:

| Disk path | URL |
|-----------|-----|
| `public/logo.png` | `/logo.png` |
| `public/css/style.css` | `/css/style.css` |
| `public/js/app.js` | `/js/app.js` |

### Rules

- `public/index.html` is served at `/` ONLY if no `@get /` route exists. Routes take priority.
- MIME types auto-detected from file extension (built-in table of ~50 common types).
- Cache: `Cache-Control: public, max-age=3600` (1 hour for static files).
- Versioned files (`app.a1b2c3.js`): `Cache-Control: public, max-age=31536000, immutable` (1 year).
- Hidden files (starting with `.`) are NOT served → 404.
- Directory listing disabled → 404 for directory paths.
- If `public/` doesn't exist, all static file requests return 404.

### 404 Debugging

Static file 404s include the path in the response:
```
404 Not Found: /images/missing.png (no matching route or static file)
```

## Request Object (`request`)

Available implicitly in every request handler.

```eka
request.path                    -- "/user/42"
request.method                  -- "GET", "POST", "PUT", "DELETE", "PATCH"
request.query("q")              -- string or null
request.query("q", "default")   -- string (with default)
request.form()                  -- map (POST form data) ⚠ stub: returns empty map
request.json()                  -- map or list (POST JSON body)
request.file("avatar")          -- null ⚠ stub: multipart upload not yet implemented
request.header("X-Token")       -- string or null
request.param("id")             -- string (route param) or null
```

## Response Object (`response`)

Available implicitly in every request handler.

```eka
response.status(404)                     -- Set status code
response.redirect("/")                   -- 302 redirect
response.redirect("/new", 301)           -- Permanent redirect
response.header("X-Custom", "value")     -- Set response header
response.html("<h1>Hello</h1>")         -- Force HTML response
response.json({key: "value"})           -- Force JSON response
response.cookie("name", "value")         -- Set cookie with defaults
response.cookie("session", token, {      -- Set cookie with options
  maxAge: 86400,
  httpOnly: true,
  secure: true,
  sameSite: "Lax",
  path: "/"
})
```

## File Uploads

> **⚠ Not yet implemented.** `request.file()` currently returns `null`. Multipart parsing is planned for a future release. The API below documents the intended design.

```eka
let file = request.file("avatar")
if file
  fs.move(file.path, "public/uploads/" + file.name)
end
```

Uploaded file object:

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Original filename |
| `type` | string | MIME type (e.g., `"image/png"`) |
| `size` | number | File size in bytes |
| `path` | string | Temp file path (valid during request only) |
| `bytes` | string | File contents as raw bytes |

**Temp file lifecycle:**

```
Request starts
  → file uploaded to /tmp/eka-uploads/<random-id>/<original-name>
  → request.file("avatar") returns the file object
  → user reads, copies (fs.copy), or moves (fs.move) the file
Request ends
  → /tmp/eka-uploads/<random-id>/ is deleted
```

To persist an upload, call `fs.move(file.path, "public/uploads/" + file.name)` inside the request handler. After the request handler returns, `file.path` is invalid — accessing it throws `"file no longer available"`.

## Sessions

Sessions are cookie-based with server-side storage in `.eka/sessions.db` (SQLite).

```eka
session.set("user_id", 42)
let userId = session.get("user_id")       -- 42 or null
session.delete("user_id")
session.clear()
```

- Session ID: 256-bit random value, stored in a signed cookie.
- Cookie settings: `HttpOnly`, `SameSite=Lax`, `Secure` (when behind TLS).
- Signed with `EKA_SECRET` env var. Auto-generated on first run if not set.
- Sessions survive server restarts (SQLite-backed).
- If `EKA_SECRET` changes (auto-generated on first run), all existing sessions are invalidated.

## CSRF Protection

CSRF protection is **semi-automatic** — the check is automatic, you just include the token.

### Automatic Check

All `POST`, `PUT`, `PATCH`, `DELETE` requests are checked for:

1. A valid `_csrf` form field matching the session token, **OR**
2. An `X-Eka-Request: 1` header (sent automatically by the `e-*` client runtime)

If neither is present → `403 Forbidden` with body `"CSRF validation failed"`.

`GET` and `HEAD` requests are never checked.

### Including the Token

In HTML forms:

```eka
@get /form
  <form method="post" action="/submit">
    <input type="hidden" name="_csrf" value="{{ session.csrf() }}">
    <input name="email" type="email" required>
    <button>Submit</button>
  </form>
@end
```

`session.csrf()` creates the token on first call per session and returns it. Subsequent calls return the same token.

For `e-*` requests: the client runtime sends `X-Eka-Request: 1` automatically, so no manual token needed.

### Opt-Out

For public endpoints (webhooks, APIs):

```eka
@post /webhook
  @csrf off
  -- handle webhook without CSRF check
@end
```

## SSE (Server-Sent Events)

```eka
@get /events
  sse.connect()
@end

@post /notify
  @do
    let msg = request.json()
    sse.broadcast("update", json.stringify(msg))
  @end
  {sent: true, listeners: sse.count()}
@end
```

- SSE endpoints MUST be `@get` only.
- `sse.connect()` can only be called once per request.
- Calling `sse.connect()` automatically sets `Content-Type: text/event-stream`.
- Browser reconnection: standard SSE `retry` field set to 3000ms.
- Max connections: `EKA_MAX_SSE` (default 1000).
- Connections auto-close on client disconnect.

## Redirect Handling with `e-*`

When an `e-*` request receives a redirect (301, 302, 303, 307, 308), the client runtime follows the redirect via `fetch()` and swaps the final response into `e-target`. This is standard `fetch` behavior.

For full-page redirects from `e-*` requests, the server should return a normal response and let the client JS handle navigation with `window.location`.

## Error Responses

- 404: `"404 Not Found: /path (no matching route)"`
- 405: `"405 Method Not Allowed: POST /path"`
- 403 (CSRF): `"403 Forbidden: CSRF validation failed"`
- 500 (runtime error): `"500 Internal Server Error: <error message>"` (dev mode) or `"500 Internal Server Error"` (production)

In production (`EKA_ENV=production`), error details are suppressed in responses but logged to the server log.
