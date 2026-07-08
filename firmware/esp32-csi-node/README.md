# ESP32 CSI Node

Standalone ESP-IDF firmware for the first CSI proof-of-concept node.

## Build and Flash

```powershell
cd firmware/esp32-csi-node
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

## Provisioning

On first boot, or when no Wi-Fi credentials are stored, the board starts a setup access point:

- SSID: `ESP32-CSI-SETUP-xxxxxx`
- Setup page: `http://192.168.4.1/`

Submit the home 2.4 GHz Wi-Fi SSID and password. Credentials are saved in NVS and the board reboots into station mode.

## Local Endpoints

- `GET /` serves a live inspection page.
- `GET /status.json` and `GET /api/status` return the current node state.
- `GET /api/config` returns tunable sensing configuration.
- `POST /api/config` accepts JSON updates from the Raspberry Pi.
- `POST /api/provision` stores Wi-Fi credentials from the setup portal.
- `POST /api/reset-wifi` clears stored Wi-Fi credentials and reboots into setup mode.

Example config update:

```json
{
  "idle_rate_hz": 3,
  "boost_rate_hz": 80,
  "movement_threshold": 0.35,
  "settle_threshold": 0.18,
  "boost_duration_ms": 8000,
  "cooldown_ms": 15000,
  "feature_window_ms": 250
}
```

The firmware clamps values to safe ranges before storing them.

`idle_rate_hz` and `boost_rate_hz` are local CSI ingest targets. They cap how often the Wi-Fi callback queues reduced CSI samples for the aggregation task; they do not force the router to transmit at that rate. `sample_rate_hz` in `/status.json` is the measured accepted sample rate for the latest feature window.
