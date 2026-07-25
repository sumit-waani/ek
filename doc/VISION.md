# Eka — Vision

> *One file. One truth. `eka run` and ship it.*

## What Eka Is

Eka is the simplest programming language for the web. A single `.eka` file contains your entire application — routes, database queries, HTML templates, and server logic. It comes pre-packed with everything 90–95% of web apps need. Zero dependencies, zero config files, zero build steps.

| Principle | Value |
|-----------|-------|
| **Name** | Eka (Sanskrit: "one") |
| **Extension** | `.eka` |
| **Entry Point** | `app.eka` (single file, always) |
| **Static Assets** | `public/` directory only |
| **Max Project Size** | ~2,000 LOC, ~10 routes |
| **Deployment** | `eka run` for dev; Eka Dashboard for production |
| **Dependencies** | Zero. 26 builtins + embedded client runtime |
| **Build Step** | None. Interpreted. |
| **Config Files** | None. CLI flags and env vars only. |
| **Package Manager** | None. |

## Who Eka Is For

**Solo developers, indie hackers, and AI coding agents** building:

- A landing page with a contact form
- A personal blog
- A small SaaS with authentication
- A real-time dashboard or chat
- An API for a mobile app
- An internal tool or admin panel

**Non-technical users** who want to deploy a web app by dropping a file into a dashboard.

## Who Eka Is NOT For

If you need more than one file, 2,000 lines, ~10 routes, or complex architecture, **Eka is not for you.** Use Next.js, Elixir Phoenix, Go, or whatever fits your scale.

Eka explicitly does NOT include:

- Multiple files, imports, or module systems
- A package manager or external dependencies
- A build step or compilation
- Type enforcement at runtime
- Async/await in user code
- An ORM — raw SQL with parameter binding
- Middleware chains — direct HTTP blocks
- A plugin system
- WebSocket — SSE only
- Reactive client state — server-driven swap via embedded runtime
- Any CDN dependencies

## The Hard Rule

**One way to do anything.** Every feature, every syntax, every API has exactly one canonical path. This is for AI agents (no hallucination surface) and for humans (no decision fatigue).

If the spec offers two ways, one of them is a bug.

## The User Experience

### For Developers (Local)

```bash
# Install once
curl -fsSL https://eka.dev/install.sh | bash

# Create a project
mkdir my-blog
cd my-blog
echo '' > app.eka    # or use eka init

# Write your app in one file
# eka run
# → http://localhost:8080
```

### For Non-Technical Users (VPS)

```
1. Run the install command on your VPS (once)
2. Open the Eka Dashboard in your browser
3. Drag and drop your .eka file
4. Done — it's live at your domain
```

The end user never touches a terminal after installation. The dashboard handles deployment, domains, HTTPS certificates, and logs.

## Runtime

The Eka runtime is a **single static binary** (~3–5 MB), written in C with libuv. It contains:

- The compiler and bytecode VM
- The HTTP server
- The SQLite driver
- The embedded client runtime JavaScript (`/_eka.js`)

Zero runtime dependencies. Works on Linux, macOS, and Windows (via WSL2).

## Design Philosophy

1. **One file, one truth.** All code in `app.eka`. No imports, no modules, no splitting.
2. **Pre-packed, not extensible.** 26 builtins cover 90–95% of web needs. If you need more, use a bigger framework.
3. **Server-driven UI.** The client runtime swaps HTML from the server. No virtual DOM, no signals, no reactive state in the client.
4. **Synchronous-looking code.** Single event loop + hidden worker pool. User code reads top-to-bottom with no async/await.
5. **Fault-tolerant templates.** A null reference in `{{ }}` renders as empty string, not a 500 error.
6. **Transparent limits.** ~2,000 LOC, ~10 routes. If you hit these, Eka tells you to graduate to a real framework.
