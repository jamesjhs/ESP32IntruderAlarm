# Python ESP32 Telemetry Worker

Version: `0.5.1`

The Python worker receives compact telemetry from ESP32 CSI receiver and sender
nodes on LAN port `3005` at `/espdata`. It keeps live node state in memory,
exposes internal status endpoints for the TypeScript PWA service, and can send
optional UDP probe traffic on a timer.

In the controlled-source topology, the dedicated sender posts the same telemetry
shape with `role: "csi_sender"`. The worker stores it like any other node so the
PWA can proxy configuration requests to the sender, while movement history still
comes from receiver payloads that report `movement_score`.

`0.5.1` receiver firmware also reports `role`, `board_variant`,
`hardware_profile`, and `csi_source_mac_diagnostics` in live status. The worker
does not interpret those fields; it preserves the receiver status payload so the
PWA can show whether a node is the standard ESP32-WROOM-32 build or the
ESP32-S3-WROOM-1U `s3-enhanced` build, and whether the configured sender MAC is
seen before filtering and accepted after the receiver's CSI gates.

## Install

Supported runtime:

- Python `>=3.9`
- Current dependency minimums: `aiohttp>=3.13.5,<3.14` and
  `python-dotenv>=1.2.1,<2`
- Build backend: `setuptools>=68,<83`

```powershell
cd raspberrypi\python
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip setuptools
pip install -e .
pip check
```

On Debian, use the same commands with the virtualenv activation script at
`.venv/bin/activate`.

## Run

```powershell
python -m esp32_alarm_worker.server
```

The worker reads environment from `../.env` when present, falling back to safe
defaults.

## Endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Human-readable landing page showing worker status and known nodes. |
| `GET` | `/healthz` | Process health check. |
| `POST` | `/espdata` | ESP32 telemetry ingest. Responds with `ok: true` and `ack: "espdata_received"` when stored. |
| `GET` | `/internal/status` | Live node state for the PWA service. |
| `GET` | `/internal/nodes/{device_id}` | One node snapshot. |

## Troubleshooting

If installation fails on Debian, make sure Python virtual environments are
available:

```bash
sudo apt install python3-venv
```

Then recreate the virtual environment and run `pip check` again.

On Python `3.9`, the worker pins `python-dotenv` below `1.2.2` because
`python-dotenv` `1.2.2` requires Python `3.10` or newer.

If ESP32 posts fail, confirm:

- The worker is listening on port `3005`.
- The Pi firewall allows LAN clients to reach port `3005`.
- The ESP32 `pi_ip`, `pi_port`, and `pi_api_path` are set to the Pi LAN IP,
  `3005`, and `/espdata`.
- Cloudflare is not involved in ESP32 telemetry.

If UDP probing appears to disturb calibration, disable it, recalibrate, then
re-enable it at a slower interval and calibrate again with the normal probe
pattern running.
