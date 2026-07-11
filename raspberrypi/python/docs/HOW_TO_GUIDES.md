# Python Worker How-To Guides

Version: `0.4.1`

## How To Start The Worker

```powershell
cd raspberrypi\python
.\.venv\Scripts\Activate.ps1
python -m esp32_alarm_worker.server
```

## How To Enable UDP Probing

Edit `raspberrypi/.env`:

```text
UDP_PROBE_ENABLED=true
UDP_PROBE_TARGET_IP=192.168.1.1
UDP_PROBE_TARGET_PORT=9
UDP_PROBE_INTERVAL_SECONDS=5
```

Restart the worker.

## How To Check Node State

```powershell
Invoke-RestMethod http://127.0.0.1:3005/internal/status
```

Receiver and sender ESP32 devices both appear in this response after they have
posted telemetry to `/espdata`. Sender entries identify themselves with `role:
"csi_sender"` and carry operational fields such as `enabled`, `packet_rate_hz`,
`sta_mac`, and `broadcast_ip`; receiver entries carry movement and CSI health
fields such as `movement_score`, `sample_rate_hz`, and `rssi`.
