# Eka — CLI Reference

The Eka CLI is for **local development only.** Production deployment uses the [Eka Dashboard](DASHBOARD.md).

## Commands

### `eka run`

Start the development server.

```bash
eka run                      # Read app.eka, serve on port 8080
eka run --port 3000          # Custom port
eka run --static ./assets    # Custom static files directory (default: auto-detect public/)
eka run --file myapp.eka     # Run a specific .eka file (default: app.eka)
```

Behavior:
- Reads `app.eka` in the current directory (or `--file` path)
- Compiles and executes init code
- Starts HTTP server on the configured port
- Watches `app.eka` for changes (auto-reload, except in production)
- Serves static files from `public/` (or `--static` path)
- Prints startup message: `Eka v1.0.0 — http://localhost:8080`

### `eka check`

Syntax and type lint check. Does NOT run the server.

```bash
eka check                    # Check app.eka in current directory
eka check --file myapp.eka   # Check a specific file
```

Checks:
- Syntax errors (unclosed blocks, invalid tokens)
- Type annotation consistency (if annotations are used)
- Duplicate route definitions
- Undefined variable references (best-effort)
- Returns exit code 0 (ok) or 1 (errors found)
- Prints errors to stdout with file:line:column

### `eka fmt`

Format `app.eka` with consistent style.

```bash
eka fmt                      # Format app.eka in-place
eka fmt --check              # Check formatting without modifying (exit 1 if unformatted)
eka fmt --file myapp.eka     # Format a specific file
```

Formatting rules:
- 2-space indentation
- Consistent blank line spacing between blocks
- No trailing whitespace
- No formatting changes to raw passthrough tag contents (`<script>`, `<style>`, etc.)

### `eka version`

Print version and exit.

```bash
eka version
# → eka v1.0.0 (linux/amd64)
```

---

## Environment Variables

### Server Configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `EKA_PORT` | `8080` | Server listen port |
| `EKA_STATIC` | `public/` | Static files directory |
| `EKA_THREAD_POOL` | `min(nproc, 32)` | Worker thread count |
| `EKA_MAX_SSE` | `1000` | Maximum concurrent SSE connections |
| `EKA_CACHE_SIZE` | `64` | Cache size in MB (LRU eviction when full) |
| `EKA_SECRET` | auto-generated | Session signing key (auto-generated on first run) |
| `EKA_ENV` | `development` | Set to `production` to disable file watcher |

### Example

```bash
EKA_PORT=3000 EKA_SECRET=my-secret-key eka run
```

---

## Directory Layout (Development)

```
my-project/
├── app.eka            ← Your application
├── public/            ← Static assets (optional)
│   ├── style.css
│   └── script.js
├── app.db             ← SQLite database (created automatically by sqlite.open())
├── .eka/              ← Runtime data (auto-created)
│   └── sessions.db    ← Session storage
└── translations/      ← i18n translation files (optional)
    └── i18n/
        └── en.json
```

---

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success (server stopped cleanly, or check passed) |
| 1 | Syntax or type error (`eka check` or startup compile failure) |
| 2 | Runtime error (port in use, permission denied, etc.) |
| 130 | Interrupted (SIGINT / Ctrl+C) |

---

## Development Workflow

```bash
# Create a new project
mkdir my-blog && cd my-blog

# Create app.eka (or copy a starter)
touch app.eka

# Check syntax
eka check

# Run development server
eka run

# Edit app.eka → auto-reload on save
# Check formatting
eka fmt --check
eka fmt

# Deploy: drop app.eka into the dashboard, or scp to VPS
```
