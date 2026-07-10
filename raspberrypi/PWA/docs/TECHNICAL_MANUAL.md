# PWA Technical Manual

Version: `0.2.1`

## Responsibilities

The TypeScript service owns the public application surface:

- static PWA shell
- API version endpoint
- status API that reads the Python worker over loopback
- Cloudflare Access-aware response handling
- persistent admin data, node registry, VAPID push, movement events, and audit logs
- ESP32 node proxy routes for status, configuration, and persisted calibration

## Runtime

The service binds to `127.0.0.1:3015` by default. Cloudflare Tunnel should route
`house.jahosi.co.uk` to this loopback address.

The ESP32 telemetry endpoint is not part of this service. ESP32 devices should
post directly to the Python worker on LAN port `3005` at `/espdata`. The PWA
service does proxy selected ESP32 node actions, including `/api/config`,
`/api/calibrate`, and `/api/calibration`, so the browser does not need direct
node access.

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
