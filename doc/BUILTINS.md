# Eka — Builtins Reference

Eka ships with **27 builtins.** These are the only APIs available. There is no package manager, no imports, no external dependencies. If you need something not listed here, Eka may not be the right tool.

---

## 1. `print`

Debug output to stdout. Visible in terminal (dev) or dashboard logs (production).

```eka
print("Hello, world!")
print("User: ${name}")
print(someObject)  -- stringifies non-strings automatically
```

---

## 2. `request`

HTTP request object. Available in every request handler. See [ROUTING.md](ROUTING.md).

```eka
request.path                    -- "/user/42"
request.method                  -- "GET", "POST", etc.
request.query("q")              -- URL query param (string or null)
request.query("q", "default")   -- With default value
request.form()                  -- POST form data (map) [stub: returns empty map]
request.json()                  -- POST JSON body (map or list)
request.file("avatar")          -- {name, type, size, path, bytes} or null [stub: not yet implemented]
request.header("X-Token")       -- Header value (string or null)
request.param("id")             -- Route parameter from [id]
```

---

## 3. `response`

HTTP response control. Available in every request handler. See [ROUTING.md](ROUTING.md).

```eka
response.status(404)                     -- Set status code
response.redirect("/")                   -- 302 redirect
response.redirect("/new", 301)           -- Permanent redirect
response.header("X-Custom", "value")     -- Set response header
response.html("plain text")             -- Return HTML (Content-Type: text/html)
response.json({key: "value"})           -- Return JSON (Content-Type: application/json)
response.cookie("name", "value")         -- Set cookie with defaults
response.cookie("name", "value", {       -- Set cookie with options
  maxAge: 86400,        -- seconds, default: session (no expiry)
  httpOnly: true,       -- default: true
  secure: true,         -- default: true when behind TLS, false in dev
  sameSite: "Lax",      -- "Strict", "Lax", "None"
  path: "/",            -- default: "/"
  domain: "example.com" -- default: not set (current domain)
})
```

---

## 4. `sqlite`

SQLite database. WAL mode enabled automatically.

```eka
let db = sqlite.open("app.db")
```

### Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `db.query(sql, params?)` | list\<map\> | SELECT queries. Returns list of row maps |
| `db.exec(sql, params?)` | null | INSERT, UPDATE, DELETE, DDL |
| `db.lastId()` | number | Last inserted row ID |

```eka
-- Query with parameters (always use ? placeholders)
let users = db.query("SELECT * FROM users WHERE age > ?", [18])

-- Insert
db.exec("INSERT INTO users (name, email) VALUES (?, ?)", ["Alice", "alice@example.com"])
let newId = db.lastId()

-- Update
db.exec("UPDATE users SET email = ? WHERE id = ?", ["new@example.com", 42])

-- Delete
db.exec("DELETE FROM users WHERE id = ?", [42])
```

**Thread safety:** Each worker thread opens its own SQLite connection. WAL mode allows concurrent readers and one writer. This is transparent to user code — `db.query()` and `db.exec()` just work from any route.

---

## 5. `json`

JSON parsing and serialization.

```eka
let obj = json.parse('{"name": "Alice", "age": 30}')
let str = json.stringify({name: "Bob", age: 25})
let pretty = json.stringify(data, true)  -- pretty-print
```

`json.parse()` returns a map/list. On invalid JSON, throws: `"invalid JSON: <reason>"`.

---

## 6. `crypto`

Cryptographic utilities.

```eka
let hash = crypto.sha256("hello")
let random = crypto.randomBytes(16)   -- 16 random bytes (hex string)
let random32 = crypto.randomBytes(32) -- 32 random bytes
let hmac = crypto.hmac("sha256", key, message)
```

Hash algorithms: `"sha256"`, `"sha512"`.

---

## 7. `markdown`

Markdown to HTML conversion (CommonMark).

```eka
let html = markdown.parse("# Hello\n\nWorld")
-- → "<h1>Hello</h1>\n<p>World</p>\n"
```

Returns `RawString` — output renders as HTML in `{{ }}` without double-escaping:

```eka
<div>{{ markdown.parse(post.body) }}</div>
```

---

## 8. `html`

HTML utilities.

```eka
let safe = html.escape("<script>alert('xss')</script>")
-- → "&lt;script&gt;alert(&#x27;xss&#x27;)&lt;/script&gt;"

let raw = html.raw("<b>Bold</b>")
-- → RawString — bypasses auto-escaping in {{ }}
```

`{{ }}` auto-escapes by default — you rarely need `html.escape()` in templates. Use it only when building HTML strings in code (e.g., for SSE/data payloads).

`html.raw()` is the ONLY way to bypass HTML escaping in `{{ }}`. Use only with trusted content. Functions that produce HTML (like `markdown.parse()`) return `RawString` automatically.

---

## 9. `http`

HTTP client for making outbound requests.

```eka
let res = http.get("https://api.example.com/data")
let res = http.post("https://api.example.com/data", {
  body: json.stringify({key: "value"}),
  headers: {"Authorization": "Bearer token"}
})
let res = http.put(url, options)
let res = http.delete(url, options)
let res = http.patch(url, options)
```

Response object:

```eka
res.status    -- number (200, 404, etc.)
res.body      -- string (response body)
res.headers   -- map (response headers)
res.json()    -- parsed JSON body (or throws)
```

Options map for non-GET requests:

```eka
{
  body: "raw string or JSON",
  headers: {"Content-Type": "application/json"},
  timeout: 10000   -- ms, default: 30000
}
```

---

## 10. `fs`

File system operations (sandboxed to project directory).

```eka
let content = fs.read("notes.txt")              -- string or null
fs.write("output.txt", "Hello, world!")         -- overwrites if exists
fs.append("log.txt", "new line\n")              -- append to file
fs.move("old.txt", "new.txt")                   -- rename/move
fs.copy("source.txt", "dest.txt")               -- copy file
fs.exists("file.txt")                            -- bool
fs.delete("file.txt")                            -- remove file
fs.mkdir("uploads")                              -- create directory
fs.list("public")                                -- list\<string\> of filenames
```

All paths are relative to the project root (where `app.eka` lives). Access outside the project directory is blocked.

---

## 11. `env`

Environment variable access.

```eka
let dbUrl = env.get("DATABASE_URL")             -- string or null
let port = env.get("PORT", "8080")              -- with default value
let debug = env.get("DEBUG", "false") == "true" -- convert to bool
```

---

## 12. `datetime`

Date and time utilities.

```eka
let now = datetime.now()
let formatted = now.format("YYYY-MM-DD HH:mm:ss")
let parsed = datetime.parse("2024-01-01", "YYYY-MM-DD")

-- Common format tokens:
-- YYYY  = 4-digit year
-- YY    = 2-digit year
-- MM    = 2-digit month (01-12)
-- DD    = 2-digit day (01-31)
-- HH    = 2-digit hour (00-23)
-- mm    = 2-digit minute (00-59)
-- ss    = 2-digit second (00-59)
```

---

## 13. `regex`

Regular expression operations (PCRE2).

```eka
let m = regex.match("^hello", "hello world")       -- "hello" or null
let replaced = regex.replace("foo", "foo bar", "baz")  -- "baz bar"
let hasMatch = regex.test("^\\d+$", "12345")        -- bool
```

---

## 14. `base64`

Base64 encoding and decoding.

```eka
let encoded = base64.encode("hello")
let decoded = base64.decode(encoded)
```

---

## 15. `url`

URL parsing and building.

```eka
let parsed = url.parse("https://example.com/path?q=1")
-- {
--   scheme: "https",
--   host: "example.com",
--   path: "/path",
--   query: {q: "1"},
--   fragment: null
-- }

let built = url.build({
  scheme: "https",
  host: "example.com",
  path: "/api",
  query: {page: "2"}
})
-- → "https://example.com/api?page=2"
```

---

## 16. `session`

Cookie-based sessions. Session ID is a signed cookie. Data stored server-side in `.eka/sessions.db` (SQLite).

```eka
session.set("user_id", 42)
let userId = session.get("user_id")       -- 42 or null
session.delete("user_id")
session.clear()                            -- remove all session data
session.csrf()                             -- get or create CSRF token
```

**CSRF protection** is semi-automatic. See [ROUTING.md](ROUTING.md#csrf-protection) for details.

- Session IDs are 256-bit random values.
- Session cookie: `HttpOnly`, `SameSite=Lax`, signed with `EKA_SECRET`.
- Sessions survive server restarts (stored in SQLite).
- If `EKA_SECRET` is not set, a random key is generated on first run (sessions invalidate on restart when key regenerates).

---

## 17. `cache`

In-memory key-value cache with optional TTL.

```eka
cache.set("key", "value")           -- no expiry
cache.set("key", "value", 3600)     -- TTL in seconds
let val = cache.get("key")          -- value or null
cache.delete("key")                 -- remove key
cache.clear()                        -- remove all keys
```

- Max size: `EKA_CACHE_SIZE` MB (default: 64).
- Eviction: LRU when full.
- Ephemeral: cache resets on server restart.
- NOT shared across multiple Eka processes.

---

## 18. `email`

Send email via SMTP.

```eka
email.send({
  to: "user@example.com",
  from: "noreply@example.com",
  subject: "Welcome!",
  body: "Thanks for signing up.",
  smtp: env.get("SMTP_SERVER")   -- "smtp.example.com:587"
})
```

Options:

| Field | Required | Description |
|-------|----------|-------------|
| `to` | Yes | Recipient email |
| `from` | Yes | Sender email |
| `subject` | Yes | Email subject |
| `body` | Yes | Plain text body |
| `smtp` | Yes | SMTP server address with port |
| `html` | No | HTML body (alternative to plain text) |
| `cc` | No | CC recipient |
| `bcc` | No | BCC recipient |

---

## 19. `validate`

Input validation.

```eka
validate.email("test@example.com")   -- bool
validate.url("https://example.com")  -- bool
validate.required("")                -- bool (false = empty)
validate.required("hello")           -- bool (true = non-empty)
validate.minLength("abc", 5)         -- bool (false = too short)
validate.maxLength("abc", 2)         -- bool (false = too long)
validate.range(50, 0, 100)           -- bool (true = in range)
validate.match("abc123", "^[a-z]+")  -- bool (regex match)
```

---

## 20. `slug`

URL slug generation.

```eka
let slug = slug.make("Hello World!")     -- "hello-world"
let slug2 = slug.make("Café & Bar")      -- "cafe-bar"
```

---

## 21. `i18n`

Internationalization. Translation files live in `translations/i18n/<lang>.json`.

```eka
i18n.set("en")
let greeting = i18n.t("hello")
let greetingName = i18n.t("hello_name", {name: "Alice"})
```

Translation file format (`translations/i18n/en.json`):

```json
{
  "hello": "Hello",
  "hello_name": "Hello, {{ name }}!"
}
```

---

## 22. `sse`

Server-Sent Events. See [ARCHITECTURE.md](ARCHITECTURE.md) for the connection model and [ROUTING.md](ROUTING.md#sse-server-sent-events) for usage.

```eka
-- SSE endpoint: clients connect here
@get /events
  sse.connect()
@end

-- Send to current client only
sse.send("update", json.stringify(data))

-- Broadcast to all connected clients
sse.broadcast("message", json.stringify(msg))

-- Active connections
let count = sse.count()
```

- `sse.connect()` — only callable once per request, in a `@get` route.
- Calling `sse.connect()` sets `Content-Type: text/event-stream` automatically.
- Wire format: standard SSE (`event:` / `data:` pairs).
- Browser reconnection: uses standard `retry` field (3000ms).
- Max connections: `EKA_MAX_SSE` (default 1000).

---

## 23. `rss`

RSS feed generation.

```eka
let feed = rss.generate({
  title: "My Blog",
  link: "https://example.com",
  description: "A blog about things",
  items: [
    {title: "Post 1", link: "/post/1", description: "Content", pubDate: "2024-01-01"}
  ]
})
```

---

## 24. `sitemap`

XML sitemap generation.

```eka
let map = sitemap.generate([
  {url: "/", lastmod: "2024-01-01", changefreq: "daily", priority: "1.0"},
  {url: "/about", lastmod: "2024-01-01"}
])
```

---

## 25. `str`

String manipulation.

| Function | Returns | Description |
|----------|---------|-------------|
| `str.len(s)` | number | Character count (same as `s.length`) |
| `str.lower(s)` | string | Lowercase |
| `str.upper(s)` | string | Uppercase |
| `str.trim(s)` | string | Trim leading/trailing whitespace |
| `str.split(s, delim)` | list\<string\> | Split by delimiter |
| `str.replace(s, old, new)` | string | Replace all occurrences |
| `str.substr(s, start, len?)` | string | Substring (0-based start, optional length) |
| `str.contains(s, sub)` | bool | Whether s contains sub |
| `str.index(s, sub)` | number or null | First index of sub, or null |
| `str.starts(s, prefix)` | bool | Whether s starts with prefix |
| `str.ends(s, suffix)` | bool | Whether s ends with suffix |

```eka
str.split("a,b,c", ",")          -- ["a", "b", "c"]
str.replace("foo bar", "bar", "baz")  -- "foo baz"
str.lower("HELLO")               -- "hello"
str.trim("  hi  ")               -- "hi"
str.substr("hello", 1, 3)        -- "ell"
```

---

## 26. `math`

Mathematical operations.

| Function | Returns | Description |
|----------|---------|-------------|
| `math.floor(n)` | number | Round down to integer |
| `math.ceil(n)` | number | Round up to integer |
| `math.round(n)` | number | Round to nearest integer |
| `math.abs(n)` | number | Absolute value |
| `math.min(a, b)` | number | Minimum of two numbers |
| `math.max(a, b)` | number | Maximum of two numbers |
| `math.pow(base, exp)` | number | Raise base to exponent |
| `math.sqrt(n)` | number | Square root |
| `math.log(n)` | number | Natural logarithm |
| `math.random()` | number | Random float between 0 and 1 |
| `math.randomInt(min, max)` | number | Random integer inclusive |

```eka
math.ceil(3.2)          -- 4
math.round(3.5)         -- 4
math.pow(2, 10)         -- 1024
math.sqrt(144)          -- 12
math.log(2.718281828)   -- ~1
math.randomInt(1, 100)  -- 42
```

---

## 27. `number`

Number parsing and conversion.

| Function | Returns | Description |
|----------|---------|-------------|
| `number.parse(str)` | number or null | Parse string to number. Returns `int` for whole numbers, `float` for decimals, `null` for invalid |

```eka
number.parse("42")         -- 42 (int)
number.parse("3.14")       -- 3.14 (float)
number.parse("-7")         -- -7 (int)
number.parse("hello")      -- null
number.parse("")           -- null
```
