# PWA How-To Guides

Version: `0.0.1`

## How To Check The Version API

```powershell
Invoke-RestMethod http://127.0.0.1:3015/api/version
```

## How To Check Worker Status Through The PWA

Start the Python worker, then run:

```powershell
Invoke-RestMethod http://127.0.0.1:3015/api/status
```

## How To Build For Production

```powershell
cd raspberrypi\PWA
npm install
npm run build
npm start
```

## How To Configure Cloudflare Tunnel

Set the tunnel origin service to:

```text
http://127.0.0.1:3015
```

## How To Install On Android

1. Open the Cloudflare-protected HTTPS hostname in Chrome or Edge on Android.
2. Complete Cloudflare Access/App Login.
3. Open the browser menu.
4. Choose **Add to Home screen** or **Install app**.
5. Confirm the app name and install.
6. Open the installed app and confirm the dashboard loads without the browser
   address bar.

Android install requires HTTPS, a valid manifest, app icons, and a registered
service worker. Local `http://127.0.0.1:3015` is useful for development but is
not the real Android install target.

## How To Check Push Readiness

1. Open the installed PWA.
2. Confirm the **Service worker** row shows `active` or `registered`.
3. Confirm the **Push support** row shows `available`, `granted`, or `denied`.
4. If it shows `denied`, reset notification permission in Android site settings.

Full push delivery also requires VAPID keys, authenticated subscription storage,
and a server-side push sender. Those are documented in
`docs/PWA_PUSH_REQUIREMENTS_ASSESSMENT.md`.

## How To Test A Service Worker Push In DevTools

Chrome DevTools can simulate a push event from the Application tab. Use a JSON
payload like:

```json
{
  "type": "alarm_triggered",
  "severity": "critical",
  "event_id": "dev-test",
  "title": "Alarm triggered",
  "body": "Movement detected while armed.",
  "url": "/?event=dev-test"
}
```

The notification should use the app icon and monochrome badge, and clicking it
should focus or open the PWA.
