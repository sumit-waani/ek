# Eka — Architecture

## Implementation Stack

| Layer | Technology | Reason |
|-------|------------|--------|
| Language | **C11** | No GC fights, mature toolchain, single binary |
| Async I/O | **libuv** | Cross-platform, thread pool built-in, 14 years mature |
| HTTP Server | Custom on libuv | ~500 LOC, zero framework overhead |
| SQL | SQLite via libsqlite3 | Zero-config, embedded, WAL mode |
| Regex | PCRE2 | Standard, fast |
| Crypto | OpenSSL (libcrypto only) | SHA-256, HMAC, random bytes |
| JSON | yyjson | Fast, small |
| Markdown | cmark | Standard CommonMark |
| Email | libcurl + vendored mini-SMTP | Embedded |
| Client Runtime JS | Hand-written ES2020 | ~500 LOC, embedded as C string constant |

## Process Model

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
│  │  - Blocking I/O OK (blocks only its    │  │
│  │    own worker, not others)             │  │
│  │  - DB calls, file I/O, HTTP client     │  │
│  │    all block this thread               │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  Configurable via EKA_THREAD_POOL env var.   │
│  Default: min(nproc, 32).                    │
└──────────────────────────────────────────────┘
```

User code is **single-threaded per request, synchronous-looking.** No async/await. No callbacks. The libuv main thread handles I/O multiplexing. The worker pool handles blocking calls. A user writes `let users = db.query(...)` and it just works — the query blocks the worker, not the event loop.

## Two-Phase Execution

### Phase 1: Init

Code at the top level of `app.eka` (outside any method block) runs **once at server startup**, on the main thread, before any request is accepted:

```eka
let db = sqlite.open("app.db")
db.exec("CREATE TABLE IF NOT EXISTS todos (id INTEGER PRIMARY KEY, text TEXT, done INTEGER DEFAULT 0)")
let appName = "Todo App"
```

Init scope = global scope. Variables declared here are accessible in all request handlers. Init variables are **read-only from request scope** — request handlers cannot mutate them for other concurrent requests. Each request handler receives a **snapshot copy** of init variables; mutations in a handler are discarded after the response. Use `cache`, `session`, or `db` for shared mutable state.

### Phase 2: Request

Code inside method blocks (`@get`, `@post`, etc.) runs **per request**, on a worker thread from the pool:

```eka
@get /
  let todos = db.query("SELECT * FROM todos ORDER BY id DESC")
  -- render HTML...
@end
```

Request scope = isolated. Variables declared inside a method block live only for that request. No shared mutable state between requests except via `cache` or `db`.

## VM Design

| Feature | Choice |
|---------|--------|
| Architecture | Register-based bytecode |
| Stack | None (registers live in a C struct per request) |
| Value representation | NaN-boxed 64-bit |
| GC | Mark-sweep |
| GC trigger | When allocated > 1 MB or on request boundary |
| Max registers per function | 256 |
| Bytecode format | 32-bit instructions: `opcode \| A \| B \| C` |
| Constants | Pool per function (strings, numbers) |
| Closures | Upvalues via capture list |
| Tail calls | Not optimized in V1 |

## NaN-Boxing

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
- `true` / `false` = specific quiet NaN values
- `int` = 51-bit unsigned
- `double` = normal IEEE 754
- `string` / `list` / `map` / `RawString` = pointer

**One machine word per value.** No per-value boxing overhead in the common case.

## Memory Model

- Init code runs once on main thread, before any request. Result is a frozen global object.
- Each request gets a fresh VM state (registers, stack, locals) but **shares the global object by reference** (read-only).
- GC roots: global object, current request's stack/registers, SSE registry.
- Heap: bump allocator with mark-sweep, ~1 MB arenas, freed on sweep.
- No concurrent GC. Worker threads run GC at request boundary (after response sent, before worker returns to pool).

## SQLite and Worker Threads

Each worker opens its own SQLite connection to the database file using WAL mode. This allows multiple concurrent readers and one writer — matching the worker pool model perfectly. The `db` variable in init scope acts as a connection factory; workers use it to get their own connection transparently.

WAL mode is enabled automatically on first database open. The user never configures this.

## File Watcher (Dev Mode)

- `eka run` watches `app.eka` via `libuv fs_event`.
- On change: re-parse, re-compile, swap the global object atomically.
- SSE clients receive a standard `retry` field so browsers reconnect automatically.
- New requests use the new code immediately after compile.
- In production (`EKA_ENV=production`), no watcher. Restart to update.

## Template Rendering

The template engine processes `.eka` files in a single pass:

1. Code outside method blocks → Phase 1 (init), compiled to bytecode
2. Code inside method blocks → Phase 2 (request handlers), compiled to bytecode
3. HTML text between code → string literals, emitted verbatim
4. `{{ expression }}` → evaluate at render time, auto-escape, insert into output
5. `@if`, `@for`, `@do` → control flow, processed at render time
6. `<script>`, `<style>`, `<pre>`, `<textarea>`, `<code>` → raw passthrough, contents not parsed

### Fault Tolerance

Template interpolation handles null references gracefully. If an expression inside `{{ }}` evaluates to `null` (nil), it renders as an empty string. Property access on `null` (e.g., `{{ user.name }}` when `user` is null) also returns `null` and renders as empty. The page continues rendering.

**Note:** Runtime errors (division by zero, calling non-functions, stack overflow) still return a 500 error. Fault tolerance applies only to null/nil values, not to all errors.

### HTML Escaping

All `{{ }}` output is HTML-escaped by default. The escaping logic:

```
if value is RawString → output as-is (no escaping)
if value is null → output empty string
everything else → html.escape(stringify(value))
```

`RawString` is created by `html.raw()` and by functions that produce HTML (like `markdown.parse()`). There is no other way to bypass escaping. `RawString` is consumed at render time and cannot be converted back to a regular string.

## Client Runtime Delivery

The embedded client runtime is served at `/_eka.js?v=<version>` where version is the Eka binary version. Cache header: `Cache-Control: public, max-age=31536000, immutable`. Browser re-fetches only when Eka is upgraded.

When the runtime sends an HTML response, it scans for `e-*` attributes. If found, it injects `<script src="/_eka.js?v=X.Y.Z" defer></script>` into the `<head>`. No `e-*` attributes → no script injection → zero overhead.

Manual opt-out: `response.header("X-Eka-Runtime", "skip")`.
Manual opt-in for static HTML in `public/`: include the script tag yourself.

## SSE (Server-Sent Events)

The runtime maintains an internal SSE registry (map of active connections, mutex-protected). When a client connects:

1. Registers the connection in the registry
2. Keeps the HTTP connection alive (long-lived, no timeout)
3. Returns control to the event loop immediately
4. Sends data asynchronously when `sse.broadcast()` or `sse.send()` is called

The connection stays on a dedicated libuv TCP handle. Sending iterates the registry under a short critical section.

Browser reconnection uses the standard SSE `retry` field (set to 3000ms on connection). No custom reconnect protocol.

Maximum SSE connections: configurable via `EKA_MAX_SSE`, default 1000.
