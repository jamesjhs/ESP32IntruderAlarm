# TypeScript PWA/API Service

Version: `0.0.1`

This app serves the Raspberry Pi web dashboard and browser-facing API on
`127.0.0.1:3000`. It is the only service intended to sit behind Cloudflare
Tunnel and Cloudflare Access/App Login.

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
| `GET` | `/api/push/vapid-public-key` | VAPID public key placeholder. |

## PWA And Push Assets

The app includes Android install icons, maskable icons, monochrome notification
badges, `favicon.png`, and `apple-touch-icon.png`. The service worker supports
offline shell caching, Android push display, notification click handling, and
subscription-change messages to open clients.

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
`http://127.0.0.1:1000`.

If Cloudflare shows the app but API calls fail after some idle time, the browser
may be receiving a Cloudflare Access HTML challenge instead of JSON. The PWA
client must treat non-JSON API responses as an expired outer session and reload
through the protected origin.
