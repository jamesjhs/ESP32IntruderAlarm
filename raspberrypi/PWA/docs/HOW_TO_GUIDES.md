# PWA How-To Guides

Version: `0.5.1`

## How To Check The Version API

```powershell
Invoke-RestMethod http://127.0.0.1:3015/api/version
```

## How To Check Worker Status Through The PWA

Start the Python worker, then run:

```powershell
Invoke-RestMethod http://127.0.0.1:3015/api/status
```

## How To Check A Node's Persisted Calibration Through The PWA

Replace `0` with the ESP32 `device_id`:

```powershell
Invoke-RestMethod http://127.0.0.1:3015/api/nodes/0/calibration
```

## How To Configure The Dedicated CSI Sender Through The PWA

Provision the sender ESP32 onto the same 2.4 GHz Wi-Fi network as the receiver
ESP32 boards and let it post telemetry to the Python worker at
`http://<pi-lan-ip>:3005/espdata`. Once it appears in the ESP32 Nodes card, open
Settings for that sender and set the packet rate, UDP port, payload size,
broadcast address, and Pi telemetry target. Use Start/Stop Sender from the node
card to enable or pause packet emission without removing the device from Wi-Fi.

For each receiver ESP32, open Settings, copy the sender `sta_mac` into `CSI
sender MAC`, and enable `Filter to sender`. Calibrate the receivers while the
sender is already running at the intended rate so the baseline includes the
normal controlled packet stream.

Open the receiver Settings/status modal after calibration and check the
protected source-MAC diagnostics below the CSI MAC histogram. `Seen before
filter` confirms the configured sender MAC is reaching the CSI callback.
`Accepted after gates` confirms those packets are also passing the receiver's
source filter, throttling, quality checks, and queue handoff.

The histogram rows also show Pi-side identity enrichment when available:
friendly ESP32 node names first, then nmap-derived IP/hostname/vendor details,
then any `ip neigh` fallback.

For ESP32-S3-WROOM-1U receivers, flash `firmware/esp32-s3-wroom` instead of
`firmware/esp32-csi-node`. In the receiver status modal, confirm the board
fields show `ESP32-S3-WROOM-1U` and `s3-enhanced` before tuning higher S3-only
sample-rate ceilings.

## How To Download A CSI Capture

Open a receiver Settings modal and use **CSI Capture**:

1. Choose a duration, default `30` seconds.
2. Choose `features` for compact per-packet features or `raw_csi` for those
   fields plus a bounded raw CSI byte prefix.
3. Add an optional label.
4. Start the capture and wait for it to complete, or stop it early.
5. Use **Refresh Captures**, then download data or metadata.

The browser downloads from the Pi, not directly from the ESP32.

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

Full push delivery also requires VAPID keys, browser permission, at least one
stored subscription, and outbound HTTPS access from the Pi to push relay
services. Those are documented in
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
