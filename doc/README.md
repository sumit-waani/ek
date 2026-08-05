# Eka — Documentation

> *One file. One truth. `eka run` and ship it.*

Eka is the simplest programming language for the web. A single `.eka` file contains your entire application. 27 builtins cover 90–95% of what web apps need. Zero config, zero dependencies.

## Docs

| Document | Contents |
|----------|----------|
| [VISION](VISION.md) | Philosophy, target audience, what Eka is and isn't |
| [LANGUAGE](LANGUAGE.md) | Complete syntax: variables, types, functions, control flow, HTML integration |
| [BUILTINS](BUILTINS.md) | All 27 builtins: full API reference with examples |
| [ROUTING](ROUTING.md) | File structure, method blocks, request/response, sessions, CSRF, SSE, static files |
| [CLIENT-RUNTIME](CLIENT-RUNTIME.md) | `/_eka.js`, `e-*` attributes, swap strategies, JS API, security |
| [ARCHITECTURE](ARCHITECTURE.md) | VM design, process model, NaN-boxing, GC, memory model |
| [DASHBOARD](DASHBOARD.md) | VPS deployment dashboard: project management, domains, TLS |
| [CLI](CLI.md) | CLI commands, environment variables, development workflow |
| [EXAMPLES](EXAMPLES.md) | Worked examples: todo, blog, chat, contact form, API, auth |

## Quick Reference

### File structure
```
my-project/
├── app.eka          ← All code lives here
└── public/          ← Static assets (optional)
```

### Start developing
```bash
curl -fsSL https://eka.dev/install.sh | bash
eka run
# → http://localhost:8080
```

### Deploy
```
Drop app.eka into the Eka Dashboard → done.
```

### Builtins at a glance

| # | Builtin | Purpose |
|---|---------|---------|
| 1 | `print` | Debug output |
| 2 | `request` | HTTP request (path, query, form, json, file, headers) |
| 3 | `response` | HTTP response (status, redirect, headers, cookies, html, json) |
| 4 | `sqlite` | SQLite database (query, exec, lastId) |
| 5 | `json` | JSON parse and stringify |
| 6 | `crypto` | SHA-256, HMAC, random bytes |
| 7 | `markdown` | Markdown → HTML (CommonMark) |
| 8 | `html` | HTML escape and raw passthrough |
| 9 | `http` | HTTP client (GET, POST, PUT, DELETE, PATCH) |
| 10 | `fs` | File system (read, write, move, copy, delete, exists, list, mkdir) |
| 11 | `env` | Environment variables |
| 12 | `datetime` | Date/time formatting and parsing |
| 13 | `regex` | Regular expressions (match, replace, test) |
| 14 | `base64` | Base64 encode/decode |
| 15 | `url` | URL parse and build |
| 16 | `session` | Cookie-based sessions + CSRF |
| 17 | `cache` | In-memory key-value cache with TTL |
| 18 | `email` | Send email via SMTP |
| 19 | `validate` | Input validation (email, url, required, length, range, regex) |
| 20 | `slug` | URL slug generation |
| 21 | `i18n` | Internationalization |
| 22 | `sse` | Server-Sent Events (connect, send, broadcast, count) |
| 23 | `rss` | RSS feed generation |
| 24 | `sitemap` | XML sitemap generation |
| 25 | `str` | String manipulation (split, replace, lower, upper, trim, etc.) |
| 26 | `math` | Math operations (floor, ceil, round, abs, min, max, random) |
| 27 | `number` | Number parsing (parse string to int/float) |
