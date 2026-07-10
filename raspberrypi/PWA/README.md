# TypeScript PWA/API Service

Version: `0.2.1`

This app serves the Raspberry Pi web dashboard and browser-facing API on
`127.0.0.1:3015`. It is the only service intended to sit behind Cloudflare
Tunnel and Cloudflare Access/App Login.

## Current Release Notes

`0.2.1` adds Pi-side read/save/delete controls for ESP32 persisted calibration
baselines, keeps PWA cache/version assets aligned with the app version, and
retains the movement history, trigger threshold, and push notification plumbing
introduced during the `0.2.x` dashboard work.

## Install

Supported runtime:

- Node.js `>=22.21.0`
- npm `>=10`
- Tested in this workspace with Node `25.6.1` / npm `11.14.1`
- Designed for the Pi's Debian 13 Node.js `22.21` install

```powershell
cd raspberrypi\PWA
npm install
npm audit fix
npm run build
```

For a clean clone or deployment, prefer the lockfile install:

```powershell
cd raspberrypi\PWA
npm ci
npm run build
```

## Development Run

```powershell
npm run dev
```

## Production Build

```powershell
npm run build
npm start
```

## Endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | PWA app shell. |
| `GET` | `/api/version` | Server version for PWA update checks. |
| `GET` | `/api/healthz` | Web service health check. |
| `GET` | `/api/status` | Live worker/node status via loopback. |
| `GET` | `/api/history/movement` | Movement score samples for aggregate and per-node charts. |
| `GET` | `/api/history/movement/trigger` | Current Pi-side movement trigger threshold and push flag. |
| `POST` | `/api/history/movement/trigger` | Save Pi-side movement trigger threshold and push flag. |
| `GET` | `/api/push/vapid-public-key` | Browser-safe VAPID public key. |
| `POST` | `/api/push/subscribe` | Store/update the current browser push subscription. |
| `POST` | `/api/push/unsubscribe` | Remove the current browser push subscription. |
| `POST` | `/api/push/test` | Send a test push to enabled subscriptions. |
| `GET` | `/api/admin/summary` | Admin data for users, VAPID, subscriptions, nodes, security, events, and audit log. |
| `POST` | `/api/admin/users` | Create a user record. |
| `PUT` | `/api/admin/users/:id` | Update display name, role, and state. |
| `DELETE` | `/api/admin/users/:id` | Delete a user record. |
| `POST` | `/api/admin/vapid/generate` | Generate and persist VAPID keys. |
| `POST` | `/api/admin/vapid` | Save externally generated VAPID keys. |
| `PUT` | `/api/admin/nodes/:id` | Update persistent node metadata. |
| `POST` | `/api/admin/nodes/:id/active` | Include or exclude a node from active monitoring. |
| `POST` | `/api/admin/nodes/:id/name` | Rename a known node. |
| `POST` | `/api/admin/security` | Save security-setting flags. |
| `POST` | `/api/admin/backup` | Checkpoint and copy the SQLite database to the backup directory. |
| `GET` | `/api/nodes/:deviceId/status` | Proxy live status from a selected ESP32 node. |
| `GET` | `/api/nodes/:deviceId/config` | Proxy node configuration from a selected ESP32 node. |
| `POST` | `/api/nodes/:deviceId/config` | Save node configuration through the ESP32 `/api/config` endpoint. |
| `POST` | `/api/nodes/:deviceId/calibrate` | Start a 10 second quiet stillness calibration on the ESP32. |
| `GET` | `/api/nodes/:deviceId/calibration` | Read the ESP32 persisted calibration baseline. |
| `POST` | `/api/nodes/:deviceId/calibration` | Save ESP32 calibration baseline values to NVS. |
| `DELETE` | `/api/nodes/:deviceId/calibration` | Clear the ESP32 persisted calibration baseline. |

## Persistence

The web service creates a persistent database at
`../data/alarm.sqlite` by default. Override with:

- `ALARM_DATABASE_PATH`
- `ALARM_BACKUP_DIR`
- `SQLCIPHER_KEY`
- `VAPID_SUBJECT`

When `SQLCIPHER_KEY` is set the service applies `PRAGMA key` on startup. A
normal SQLite build will still run for development; a SQLCipher-capable SQLite
deployment can use the same schema with encryption enabled.

## PWA And Push Assets

The app includes Android install icons, maskable icons, monochrome notification
badges, `favicon.png`, and `apple-touch-icon.png`. The service worker supports
offline shell caching, Android push display, notification click handling, and
subscription-change messages to open clients.

The browser app tracks the last seen server version in `localStorage`, polls
`/api/version` on launch, visibility return, and while open, shows an update
banner when the host version changes, and can unregister old service workers,
clear old `esp32-alarm-*` caches, and reload from the host. The service serves
version-injected `service-worker.js`, `manifest.webmanifest`, and
`app-config.js` responses with `Cache-Control: no-store`.

## Node Calibration

The node settings modal can start a fresh ESP32 stillness calibration, delete a
stored baseline, or inspect/edit the persisted calibration values saved in the
ESP32's NVS. The Pi does not invent these values; it proxies the ESP32
`/api/calibration` endpoint so calibrated devices can survive power loss without
automatic recalibration during possible movement.

See [PWA push requirements](docs/PWA_PUSH_REQUIREMENTS_ASSESSMENT.md) for the
TaskIt comparison and remaining server-side push checklist.

## Troubleshooting

If `npm install` warns about Node versions, check:

```powershell
node --version
npm --version
```

The service expects Node `22.21.0` or newer. The package is intentionally not
limited to Node 22 so development on newer Windows sandboxes also works.

If the PWA cannot show node status, check that the Python worker is running at
`http://127.0.0.1:3005`.

If Cloudflare shows the app but API calls fail after some idle time, the browser
may be receiving a Cloudflare Access HTML challenge instead of JSON. The PWA
client must treat non-JSON API responses as an expired outer session and reload
through the protected origin.
