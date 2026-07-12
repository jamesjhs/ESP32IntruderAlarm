# PWA Explanation Manual

Version: `0.5.1`

The PWA is the phone and desktop dashboard for the alarm.

It is the part you open in a browser. It shows the current server version, asks
the Python worker for live ESP32 status, displays movement score history, manages
push notification subscriptions, and provides node settings panels.

In the controlled-source CSI topology, the PWA also manages the dedicated sender
ESP32. Sender devices show up in the same ESP32 Nodes card as receivers, but the
PWA labels them as senders, offers Start/Stop Sender, and lets their packet rate
and broadcast settings be changed through the Settings panel. Receiver settings
include the sender MAC filter used to focus CSI scoring on that known source.
Receiver status also shows a protected source-MAC diagnostic panel below the
normal CSI MAC histogram so the sender can still be tracked even when louder
router or household devices dominate the histogram.

The PWA also shows the receiver board variant and hardware profile reported by
the firmware. That matters in `0.5.1` because ESP32-S3-WROOM-1U receivers use a
separate `s3-enhanced` firmware target with higher CSI queue and sample-rate
ceilings, while older ESP32-WROOM-32 receivers remain supported by the standard
target.

The node settings panel can also read and save an ESP32 node's persisted
stillness calibration baseline. That baseline lives on the ESP32 in NVS, so a
calibrated node can recover after power loss without automatically
recalibrating while someone might be moving nearby.

Cloudflare protects the public website before the browser reaches the Pi. The
app will still keep its own users and roles because alarm actions need to be
tracked to named people.

On Android, installing the PWA makes it behave more like a small native app. The
home-screen icon comes from the generated alarm icon assets, and alarm or test
push notifications are displayed by the service worker even when the dashboard
window is closed.
