# Eka — Dashboard (VPS Deployment)

The Eka Dashboard is the production interface for Eka. It's a thin web UI that wraps the `eka` binary on a VPS, designed for users who never want to touch a terminal after the initial install.

## Architecture

```
VPS
├── eka-dashboard (binary) → ports 80 & 443 (public-facing)
│   ├── Manages /var/eka/projects/<name>/
│   ├── Reverse-proxies to project ports
│   ├── Handles Let's Encrypt certificate provisioning and renewal
│   └── Serves the dashboard web UI
│
├── project-a → port 9001 (internal, HTTP only)
├── project-b → port 9002 (internal, HTTP only)
└── ...
```

The dashboard binary is separate from `eka` the language runtime. `eka` stays CLI-only for local development. The dashboard is the production layer.

## Installation

Single command on a fresh Linux VPS (Ubuntu 20.04+, Debian 11+):

```bash
curl -fsSL https://eka.dev/install.sh | bash
```

This installs:
1. The `eka` binary (language runtime)
2. The `eka-dashboard` binary (management layer)
3. A systemd service for `eka-dashboard` (auto-starts on boot)
4. Default self-signed cert (replaced by Let's Encrypt on first domain setup)

After install, the dashboard is available at `http://<vps-ip>:8081`.

## Dashboard Features

### Project Management

| Action | Description |
|--------|-------------|
| **New Project** | Creates a folder in `/var/eka/projects/<name>/`, seeds a starter `app.eka` |
| **Upload** | Drag-and-drop `.eka` file to overwrite the project's `app.eka` |
| **Start/Stop** | Toggle the project's `eka` process on/off |
| **Delete** | Remove project folder and stop the process |
| **Duplicate** | Clone a project with a new name |

### File Viewer

- View and edit `app.eka` in a browser code editor (syntax-highlighted)
- Upload files to `public/` directory
- Browse directory tree

### Logs

- Stream project logs via SSE (last N lines)
- Filter by level (info, warn, error)
- Download full logs

### Environment Variables

- Key-value editor per project
- Secrets masked by default (toggle to reveal)
- No `.env` files — managed through the UI

### Domain Management

- Bind one or more domains to a project
- Auto-provisions Let's Encrypt certificates
- Auto-renews certificates (cron job, checks weekly)
- HTTP → HTTPS redirect (port 80 → 443)
- Wildcard domains not supported (use individual subdomains)

### SSL/TLS

- Let's Encrypt via ACME protocol (certbot under the hood)
- Auto-renewal 30 days before expiry
- Dashboard itself runs on HTTPS (self-signed until first domain setup)
- Project `eka` processes run on HTTP internally (localhost ports)
- Dashboard terminates TLS and reverse-proxies to project ports

## Project Lifecycle

### Start

```
1. Dashboard spawns: eka run --port <assigned-port> --dir /var/eka/projects/<name>
2. Process runs as a systemd-managed unit
3. Dashboard configures reverse proxy: <domain> → localhost:<port>
```

### Stop

```
1. Dashboard sends SIGTERM to the eka process
2. Reverse proxy route is removed
3. Port is released
```

### Deploy (Upload new app.eka)

```
1. User drops .eka file in dashboard
2. File is written to /var/eka/projects/<name>/app.eka
3. If project is running and EKA_ENV=production: dashboard restarts the process
4. If project is stopped: file is saved, no restart
```

## Port Allocation

- Dashboard: 80 (HTTP redirect), 443 (HTTPS), 8081 (admin, local-only or IP-restricted)
- Projects: auto-assigned from range 9001–9999
- Port is persistent across restarts (stored in project config)

## Security Model

- Dashboard admin UI: accessible at `http://<vps-ip>:8081` by default
- On first domain setup, admin UI moves to `https://<domain>/_eka/admin`
- Admin auth: password set on first login, stored as bcrypt hash
- Project processes run as isolated systemd units with limited privileges
- Each project gets its own port, filesystem path, and environment
- Project processes cannot access other projects' files
- Dashboard reverse proxy adds `X-Forwarded-For`, `X-Forwarded-Proto` headers

## Upgrades

```bash
curl -fsSL https://eka.dev/install.sh | bash
```

The install script detects existing installation and upgrades in-place. Running projects are restarted one by one with a 5-second delay. The dashboard itself restarts last.

## Resource Limits

Configurable per project via dashboard:

| Setting | Default | Description |
|---------|---------|-------------|
| Max memory | 256 MB | Per-project memory limit (systemd MemoryMax) |
| Max CPU | 50% | Per-project CPU limit (systemd CPUQuota) |
| Max storage | 1 GB | Per-project disk quota (project folder) |
| Request timeout | 30s | HTTP request timeout at reverse proxy |

## Backup

- Dashboard provides a "Download Backup" button per project → zip of project folder
- Optional: configure S3-compatible backup destination for automated daily backups
- Database files (`.db`, `.eka/sessions.db`) are included in backups

## What the Dashboard Does NOT Do

- ❌ Multi-server orchestration — single VPS only
- ❌ Git integration — file-based deployment via upload or dashboard editor
- ❌ CI/CD pipelines — manual upload or edit
- ❌ Team/org management — single user (the VPS owner)
- ❌ Usage analytics — zero telemetry, zero tracking
- ❌ Custom reverse proxy rules — domain → project mapping only
