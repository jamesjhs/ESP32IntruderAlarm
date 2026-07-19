# PWA Technical Manual

Version: `0.5.1`

## Responsibilities

The TypeScript service owns the public application surface:

- static PWA shell
- API version endpoint
- status API that reads the Python worker over loopback
- Cloudflare Access-aware response handling
- persistent admin data, node registry, VAPID push, movement events, and audit logs
- ESP32 node proxy routes for receiver status, configuration, persisted
  calibration, bounded capture, and sender status/configuration/start-stop
  control

## Runtime

The service binds to `127.0.0.1:3015` by default. Cloudflare Tunnel should route
`house.jahosi.co.uk` to this loopback address.

The ESP32 telemetry endpoint is not part of this service. ESP32 receiver and
sender devices should post directly to the Python worker on LAN port `3005` at
`/espdata`. The PWA service proxies selected ESP32 node actions, including
receiver `/api/config`, `/api/calibrate`, and `/api/calibration` calls plus
sender `/api/config`, `/api/start`, and `/api/stop` behaviour through the same
registered-node route shape. This keeps the browser talking to the Pi rather
than requiring direct node access.

In the controlled-source CSI topology, sender telemetry carries `role:
"csi_sender"` and the sender station MAC. The client uses that role to label
the node as a packet sender, show Start/Stop Sender and packet-rate settings,
and hide receiver-only calibration controls. Receiver settings include
`csi_source_mac` and `csi_source_filter_enabled`, which tell the receiver
firmware to analyse CSI frames from the known sender instead of mixed household
traffic.

For receiver nodes running `0.5.1` or later, the status proxy carries
`role`, `board_variant`, and `hardware_profile` in addition to
`csi_source_mac_diagnostics`. The PWA renders the board/profile fields in the
receiver status list and renders the diagnostics beneath the CSI MAC histogram
so operators can distinguish "sender MAC was seen by the CSI callback" from
"sender MAC survived filtering, throttling, quality checks, and queue handoff."

The CSI MAC histogram also consumes a Pi-side MAC identity map from
`/api/admin/summary`. The map prefers known ESP32 telemetry and database names,
then uses the Python worker's intermittent `nmap` discovery cache to show IP,
hostname, and vendor detail for other LAN devices. The worker's `ip neigh` data
is retained as a cheap fallback, but nmap is the preferred active discovery
source. Receiver modals show whether nmap is running or how long ago the last
scan completed, and expose a manual **Run nmap Scan** action through
`POST /api/nmap/scan`.

Receiver modals include bounded CSI capture controls. The PWA asks the receiver
to start a capture through `/api/nodes/:deviceId/capture/start`, defaulting to
30 seconds, while the receiver streams capture chunks to the Python worker. The
PWA lists completed or in-progress files through `/api/captures` and downloads
`.ndjson` data or `.json` metadata from the Pi.

## API Design

The PWA should use network-first API requests and verify `Content-Type` before
parsing JSON. If Cloudflare Access returns an HTML login/challenge page, the
client should stop background polling and navigate back to the protected origin.

State-changing routes should continue moving toward:

- app session cookies
- CSRF protection
- role checks
- audit logging
- re-authentication for high-risk actions

Push subscription routes follow the TaskIt-oriented shape and should continue
to enforce:

- return the VAPID public key from a public read-only endpoint
- require an authenticated user for subscribe/unsubscribe
- require HTTPS push endpoints
- validate `p256dh` and `auth` as base64url key material
- upsert only the current user's existing endpoint
- never reassign another user's endpoint
- delete subscriptions that push relay services reject as expired

## PWA Versioning

`GET /api/version` returns the current server version. The service worker uses a
versioned cache name so installed clients can discard old assets after a deploy.

## Android PWA And Push Assets

The manifest follows the TaskIt-style PWA asset matrix: standard app icons,
maskable Android icons, monochrome notification badges, a favicon, and an Apple
touch icon. All asset URLs include the server version query string.

The service worker handles `push` events with self-contained notification
payloads. This is important because Cloudflare Access sessions may be expired
when Android wakes the service worker. The notification click handler focuses an
existing PWA window or opens the payload URL.

See [PWA push requirements](PWA_PUSH_REQUIREMENTS_ASSESSMENT.md).
