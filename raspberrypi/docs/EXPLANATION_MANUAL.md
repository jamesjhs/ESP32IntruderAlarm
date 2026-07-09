# Raspberry Pi Explanation Manual

Version: `0.0.1`

The Raspberry Pi is the coordinator for the alarm system.

The ESP32 boards watch Wi-Fi CSI changes and send compact status messages to the
Pi. The Pi combines those messages, decides whether the house looks quiet or
active, stores history, and provides a phone-friendly PWA dashboard.

There are two Pi programs because they do different jobs:

- The Python worker listens to ESP32 devices on the local network.
- The TypeScript PWA service shows the dashboard and talks to browsers through
  Cloudflare.

Cloudflare protects the website login before traffic reaches the Pi. The ESP32
receiver is not part of the public website and should stay LAN-only.

The first version is a foundation. It proves the ports, service split, PWA
versioning, telemetry endpoint, and UDP probe configuration before heavier
features such as SQLCipher migrations, user management, push subscriptions, and
alarm fusion are filled in.
