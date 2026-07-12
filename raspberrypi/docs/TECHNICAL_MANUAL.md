# Raspberry Pi Technical Manual

Version: `0.5.1`

## Architecture

The Raspberry Pi runtime is split into two services.

The TypeScript PWA service owns browser-facing concerns:

- PWA shell and static assets
- version endpoint
- Cloudflare Access-aware session handling
- persistent admin records, VAPID settings, push subscriptions, events, nodes,
  movement trigger settings, and audit log tables
- ESP32 node proxy APIs for receiver status/configuration/calibration and sender
  status/configuration/start-stop control

The Python worker owns LAN sensor concerns:

- `POST /espdata` ESP32 telemetry ingest
- in-memory live node state
- receiver and sender role relay for the PWA
- sparse health and diagnostics APIs
- UDP probe traffic timer
- compact telemetry relay for the PWA status and history pipeline

The services communicate over loopback. The PWA service reads worker status from
`http://127.0.0.1:3005/internal/status`.

The controlled-source topology uses the same LAN telemetry path for both
receiver and sender ESP32 devices. Sender telemetry includes `role:
"csi_sender"` and its station MAC. Receiver configuration can then enable
`csi_source_filter_enabled` and set `csi_source_mac` so CSI scoring only uses
frames from that known packet source.

Version `0.5.1` adds a separate ESP32-S3-WROOM-1U receiver firmware target that
shares source code with the standard ESP32-WROOM-32 receiver target. Receiver
status/config payloads now identify `role: "csi_receiver"`, `board_variant`,
and `hardware_profile`; the S3 build reports `ESP32-S3-WROOM-1U` and
`s3-enhanced`. The existing `csi_source_mac_diagnostics` block remains the
protected counter block for the configured sender MAC with `seen_before_filter`,
`accepted_after_gates`, `last_seen_ms`, and `last_accepted_ms`.

## Ports

| Service | Bind | Purpose |
| --- | --- | --- |
| PWA/API | `127.0.0.1:3015` | Cloudflare Tunnel target and local browser UI. |
| Python worker | `0.0.0.0:3005` | LAN ESP32 telemetry receiver at `/espdata`. |

## Security Model

Cloudflare Access/App Login protects the public hostname before traffic reaches
the Pi. The local TypeScript app must still keep its own sessions and roles so
alarm actions, push subscriptions, and audit events are tied to named users.

The worker is not public. It should be reachable only from the home LAN. Later
versions should add per-node shared secrets or signed telemetry to reduce the
risk of another LAN client spoofing an ESP32.

## Data Model Direction

The Python worker stores live node state in memory. The TypeScript PWA service
owns the current SQLite database, which already contains tables for admin
records, push, node registry, movement history, events, settings, and audit
records. A production deployment should use a SQLCipher-capable SQLite build so
the same schema is encrypted at rest.

The persistent database should contain:

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
