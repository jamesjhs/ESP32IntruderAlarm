# Raspberry Pi Server

Version: `0.4.1`

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

Since `0.4.0`, the Pi also manages a dedicated ESP32 CSI sender. The sender
joins the same 2.4 GHz network as the receiver nodes, posts telemetry to the
same Python worker endpoint with `role: "csi_sender"`, and can be started,
stopped, or tuned from the PWA through the existing node proxy path.

`0.4.1` adds receiver-side source-MAC diagnostics to that flow. Each receiver's
status can now show the configured sender MAC independently of the evictable
top-10 histogram, including counts for frames seen before filtering and frames
accepted after throttling, quality checks, and queue handoff.

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
