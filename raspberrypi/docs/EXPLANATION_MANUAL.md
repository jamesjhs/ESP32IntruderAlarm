# Raspberry Pi Explanation Manual

Version: `0.5.1`

The Raspberry Pi is the coordinator for the alarm system.

The ESP32 receiver boards watch Wi-Fi CSI changes and send compact status
messages to the Pi. A dedicated sender ESP32 can provide a steady 2.4 GHz packet
source that the receivers listen for and filter by MAC address. The Pi combines
those messages, decides whether the house looks quiet or active, stores history,
and provides a phone-friendly PWA dashboard.

There are two Pi programs because they do different jobs:

- The Python worker listens to ESP32 devices on the local network.
- The TypeScript PWA service shows the dashboard and talks to browsers through
  Cloudflare.

Cloudflare protects the website login before traffic reaches the Pi. The ESP32
receiver is not part of the public website and should stay LAN-only.

Version `0.5.1` is still a prototype, but the foundation now includes the
service split, version-aware PWA shell, persistent admin database, movement
history, push subscription plumbing, remote receiver configuration/calibration,
Pi-managed control of a dedicated CSI sender, protected source-MAC diagnostics,
Pi-managed bounded CSI captures, nmap-enriched MAC/IP discovery for histogram
rows, and a separate ESP32-S3-WROOM-1U receiver build for the
higher-capability receiver boards. Full app login enforcement and polished
alarm arming workflows remain future hardening work.
