# PWA Explanation Manual

Version: `0.0.1`

The PWA is the phone and desktop dashboard for the alarm.

It is the part you open in a browser. It shows the current server version, asks
the Python worker for live ESP32 status, and will eventually handle arming,
disarming, event history, notification subscriptions, and settings.

Cloudflare protects the public website before the browser reaches the Pi. The
app will still keep its own users and roles because alarm actions need to be
tracked to named people.

On Android, installing the PWA makes it behave more like a small native app. The
home-screen icon comes from the generated alarm icon assets, and future alarm
push notifications will be displayed by the service worker even when the
dashboard window is closed.
