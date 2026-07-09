# PWA And Android Push Requirements Assessment

Version: `0.0.1`

## Scope

This assessment compares the ESP32 Intruder Alarm PWA scaffold with the PWA and
push-notification pattern used by `github.com/jamesjhs/taskit`.

TaskIt provides a useful baseline:

- full manifest icon matrix
- maskable Android icons
- monochrome notification badge icons
- versioned asset URLs
- cache cleanup by app version
- network-first API handling
- network-first navigation with offline fallback
- `push` event handling
- `notificationclick` handling that focuses an existing app window
- VAPID public-key endpoint
- authenticated subscribe/unsubscribe endpoints
- server-side validation of push endpoint and browser key material

## Current ESP32 Alarm Status

| Requirement | Status | Notes |
| --- | --- | --- |
| HTTPS public origin | Planned | Cloudflare Tunnel and Cloudflare Access provide HTTPS for `house.jahosi.co.uk`. |
| Root-scope service worker | Complete | `/service-worker.js` is served from the PWA root. |
| Web app manifest | Complete | `manifest.webmanifest` includes `name`, `short_name`, `start_url`, `scope`, `display`, colors, language, categories, icons, and shortcuts. |
| Android install icons | Complete | `72`, `96`, `128`, `144`, `152`, `180`, `192`, `384`, and `512` PNGs exist. |
| Maskable icons | Complete | `maskable-icon-192x192.png` and `maskable-icon-512x512.png` exist. |
| Notification badges | Complete | Monochrome-friendly `72`, `96`, and `128` badges exist. |
| Favicon and Apple icon | Complete | `favicon.png` and `apple-touch-icon.png` exist. |
| Versioned asset URLs | Complete | Manifest, icons, CSS, service worker registration, and cached assets use `v=0.0.1`. |
| Cache versioning | Complete | Cache name is `esp32-alarm-0.0.1`; activation removes old caches. |
| API caching strategy | Complete | `/api/*` requests are network-first and return JSON `503` when unavailable. |
| Navigation strategy | Complete | Navigations are network-first with cached shell fallback. |
| Push event display | Complete | Service worker displays self-contained alarm/status payloads. |
| Notification click handling | Complete | Clicks focus or open the PWA and navigate to the payload URL. |
| Subscription-change signal | Complete | Service worker notifies open clients when browser subscription state changes. |
| VAPID public key endpoint | Stubbed | `/api/push/vapid-public-key` exists and reports configured public key. |
| Subscribe/unsubscribe API | Pending | Requires app users, sessions, SQLCipher tables, and authenticated routes. |
| Server push sender | Pending | Requires `web-push`, stored subscriptions, event fan-out, and invalid-subscription cleanup. |

## Android Push Requirements

For Chrome/Edge on Android, background Web Push requires:

- HTTPS origin
- registered service worker
- user-granted notification permission
- browser Push API support
- server VAPID public/private key pair
- browser subscription saved server-side
- outbound HTTPS access from the Pi to push relay services
- notification payload handled entirely by the service worker

The service worker should not fetch alarm details before showing a notification.
Cloudflare Access sessions may be expired when the push arrives, so payloads must
include safe title/body text, event type, severity, timestamp, event ID, icon,
badge, and deep-link URL.

Recommended alarm payload:

```json
{
  "type": "alarm_triggered",
  "severity": "critical",
  "event_id": "evt_123",
  "title": "Alarm triggered",
  "body": "Movement detected while armed.",
  "url": "/?event=evt_123",
  "timestamp_ms": 1760000000000
}
```

The payload should avoid sensitive location detail. The authenticated PWA can
fetch full event details after the user opens the app.

## Differences From TaskIt

TaskIt is a task/reminder app, so notification tags group by task URL. The alarm
PWA groups by `event_id` or event type so repeated updates for the same alarm
replace each other without hiding distinct alarm events.

TaskIt subscribes users automatically after login. The alarm PWA cannot complete
that until the SQLCipher user/session layer exists. The scaffold therefore only
checks service-worker and push capability in the UI.

TaskIt includes a full authenticated push route. The alarm PWA should implement
the same validation pattern when user accounts exist:

- require authenticated session
- require HTTPS push endpoint
- validate `p256dh` and `auth` as base64url strings
- upsert only if the endpoint belongs to the current user
- do not reassign endpoints across users
- delete failed or expired subscriptions after send failures

## Generated Graphics

The source app icon was generated using the built-in image generation tool and
saved into the project as `public/icons/icon-source-1024.png`. Derived assets
were generated locally from that source.

Generated asset set:

- `favicon.png`
- `apple-touch-icon.png`
- `icons/icon-source-1024.png`
- `icons/icon-72x72.png`
- `icons/icon-96x96.png`
- `icons/icon-128x128.png`
- `icons/icon-144x144.png`
- `icons/icon-152x152.png`
- `icons/icon-180x180.png`
- `icons/icon-192x192.png`
- `icons/icon-384x384.png`
- `icons/icon-512x512.png`
- `icons/maskable-icon-192x192.png`
- `icons/maskable-icon-512x512.png`
- `icons/notification-badge-72x72.png`
- `icons/notification-badge-96x96.png`
- `icons/notification-badge-128x128.png`

## Remaining Implementation Checklist

- Add SQLCipher `push_subscriptions` table.
- Add authenticated `POST /api/push/subscribe`.
- Add authenticated `DELETE /api/push/subscribe`.
- Add VAPID key generation/admin settings.
- Add server-side `web-push` sender.
- Remove subscriptions when push relays return gone/invalid responses.
- Add a user-visible notification subscription control once login exists.
- Add an Android device test over the Cloudflare HTTPS hostname.
