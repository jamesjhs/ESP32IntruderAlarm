# Raspberry Pi Server

Version: `0.0.1`

This directory contains the Raspberry Pi side of ESP32IntruderAlarm.

- `PWA/`: Node.js + TypeScript web/PWA/API service on `127.0.0.1:3000`.
- `python/`: Python ESP32 telemetry worker on LAN port `1000` at `/espdata`.
- `.env.example`: shared first-install configuration template.
- `VERSION`: server-side Raspberry Pi project version.

The intended network split is:

```text
Cloudflare Access/App Login
  -> Cloudflare Tunnel
  -> TypeScript PWA service: http://127.0.0.1:3000

ESP32 nodes on LAN
  -> Python worker: http://<pi-lan-ip>:1000/espdata
```

Cloudflare should expose only the PWA service. The ESP32 telemetry receiver is
LAN-only and should not be routed through Cloudflare.

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

Open `http://127.0.0.1:3000` for the PWA service. Point ESP32 nodes at
`http://<pi-lan-ip>:1000/espdata`.

## Documentation

- [Project technical manual](docs/TECHNICAL_MANUAL.md)
- [Project explanation manual](docs/EXPLANATION_MANUAL.md)
- [Project how-to guides](docs/HOW_TO_GUIDES.md)
- [Python worker README](python/README.md)
- [PWA README](PWA/README.md)
