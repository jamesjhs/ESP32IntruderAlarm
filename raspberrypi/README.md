# Raspberry Pi Server

Version: `0.5.1`

This directory contains the Raspberry Pi side of ESP32IntruderAlarm.

- `PWA/`: Node.js + TypeScript web/PWA/API service on `127.0.0.1:3015`.
- `python/`: Python ESP32 telemetry worker on LAN port `3005` at `/espdata`.
- `.env.example`: shared first-install configuration template.
- `VERSION`: server-side Raspberry Pi project version.

The intended network split is:

```text
Cloudflare Access/App Login
  -> Cloudflare Tunnel
  -> TypeScript PWA service: http://127.0.0.1:3015

ESP32 receiver and sender nodes on LAN
  -> Python worker: http://<pi-lan-ip>:3005/espdata
```

Cloudflare should expose only the PWA service. The ESP32 telemetry receiver is
LAN-only and should not be routed through Cloudflare.

The Pi install now treats `nmap` as a prerequisite. The Python worker uses it
intermittently, especially after receiver histograms report new MAC addresses,
to enrich the PWA CSI MAC histogram with best-effort IP address, DNS name, and
vendor details. Known ESP32 telemetry and stored node names remain the preferred
identity source; nmap is the LAN fallback for household/router devices.

Since `0.4.0`, the Pi also manages a dedicated ESP32 CSI sender. The sender
joins the same 2.4 GHz network as the receiver nodes, posts telemetry to the
same Python worker endpoint with `role: "csi_sender"`, and can be started,
stopped, or tuned from the PWA through the existing node proxy path.

`0.5.1` adds a separate ESP32-S3-WROOM-1U receiver firmware target while keeping
the original ESP32-WROOM-32 receiver and sender targets compatible. S3 receiver
telemetry reports `role: "csi_receiver"`, `board_variant:
"ESP32-S3-WROOM-1U"`, and `hardware_profile: "s3-enhanced"`; the PWA preserves
and displays those fields through the normal worker/PWA status flow.

The Pi dashboard can also start bounded receiver CSI captures, defaulting to 30
seconds, and download the resulting `.ndjson` sample file plus `.json` metadata
from `data/captures`.

## Quick Start

Copy `.env.example` to `.env`, edit secrets, then start each app from its own
directory.

For a Debian Raspberry Pi deployment with `systemd` for the Python worker and
PM2 for the TypeScript PWA service, run:

```bash
cd raspberrypi
chmod +x ./install-and-run.sh
./install-and-run.sh
```

The installer checks for `nmap`, installs it with `apt` when missing, and gives
the worker service network capabilities needed for local MAC/IP discovery where
the Pi OS allows it.

To update an existing install from the old `3000`/`1000` ports to `3015`/`3005`
without reinstalling everything, run:

```bash
cd raspberrypi
bash ./amend-ports.sh
```

Python worker:

```powershell
cd raspberrypi\python
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -e .
python -m esp32_alarm_worker.server
```

TypeScript PWA:

```powershell
cd raspberrypi\PWA
npm install
npm run dev
```

Open `http://127.0.0.1:3015` for the PWA service. Point ESP32 nodes at
`http://<pi-lan-ip>:3005/espdata`.

## Documentation

- [Project technical manual](docs/TECHNICAL_MANUAL.md)
- [Project explanation manual](docs/EXPLANATION_MANUAL.md)
- [Project how-to guides](docs/HOW_TO_GUIDES.md)
- [Python worker README](python/README.md)
- [PWA README](PWA/README.md)
