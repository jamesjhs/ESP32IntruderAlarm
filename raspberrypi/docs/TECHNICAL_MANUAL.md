# Raspberry Pi Technical Manual

Version: `0.0.1`

## Architecture

The Raspberry Pi runtime is split into two services.

The TypeScript PWA service owns browser-facing concerns:

- PWA shell and static assets
- version endpoint
- Cloudflare Access-aware session handling
- app users, roles, sessions, and audit logging in later migrations
- VAPID push settings and subscriptions in later migrations
- authenticated admin APIs

The Python worker owns LAN sensor concerns:

- `POST /espdata` ESP32 telemetry ingest
- in-memory live node state
- sparse health and diagnostics APIs
- UDP probe traffic timer
- future movement fusion and database event writes

The services communicate over loopback. The PWA service reads worker status from
`http://127.0.0.1:1000/internal/status`.

## Ports

| Service | Bind | Purpose |
| --- | --- | --- |
| PWA/API | `127.0.0.1:3000` | Cloudflare Tunnel target and local browser UI. |
| Python worker | `0.0.0.0:1000` | LAN ESP32 telemetry receiver at `/espdata`. |

## Security Model

Cloudflare Access/App Login protects the public hostname before traffic reaches
the Pi. The local TypeScript app must still keep its own sessions and roles so
alarm actions, push subscriptions, and audit events are tied to named users.

The worker is not public. It should be reachable only from the home LAN. Later
versions should add per-node shared secrets or signed telemetry to reduce the
risk of another LAN client spoofing an ESP32.

## Data Model Direction

The initial scaffold stores live node state in memory. The planned persistent
SQLCipher database should contain:

- users
- sessions
- VAPID settings
- push subscriptions
- SMTP settings
- nodes
- node configuration history
- events
- calibrations
- audit log

The SQLCipher key must live outside the database file.

## Runtime Ownership

The PWA service should be the main writer for security-sensitive tables. The
Python worker may write sensor/event tables directly, or it may call an internal
loopback API on the PWA service. Avoid two services freely mutating the same
auth or settings rows.
