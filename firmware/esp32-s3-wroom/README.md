# ESP32-S3-WROOM-1U CSI Receiver

Version: `0.5.1`

ESP-IDF receiver build for ESP32-S3-WROOM-1U boards.

This folder is a separate build target for the S3 receiver hardware, but it
compiles the shared receiver implementation from `../esp32-csi-node/main/main.c`.
That keeps backward compatibility with the original ESP32-WROOM-32 receiver
while making future receiver fixes and features land in both builds.

## S3 Enhanced Profile

The S3 build enables a higher-capacity receiver profile:

- ESP-IDF target: `esp32s3`
- Board/status variant: `ESP32-S3-WROOM-1U`
- CSI queue length: `512` samples
- Idle CSI ingest ceiling: `160 Hz`
- Boost CSI ingest ceiling: `400 Hz`
- Default boost rate: `120 Hz`
- Larger CSI aggregation and telemetry task stacks
- 8 MB flash default for common ESP32-S3-WROOM-1U DevKit-style boards

The original `firmware/esp32-csi-node` build remains the backward-compatible
ESP32-WROOM-32 receiver target with its existing 256-sample queue, 100 Hz idle
ceiling, 250 Hz boost ceiling, and 80 Hz default boost rate.

## Build and Flash

Open an ESP-IDF PowerShell so `idf.py`, CMake, Ninja, Python, and the ESP32-S3
toolchain are on `PATH`.

```powershell
cd C:\GitHub\ESP32IntruderAlarm\firmware\esp32-s3-wroom
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the ESP32-S3 serial port.

If your ESP32-S3-WROOM-1U module has a different flash size than 8 MB, run
`idf.py menuconfig` and adjust the serial flasher flash size before building.

## Runtime API Differences

The S3 receiver exposes the same endpoints as the standard receiver. Its
`/status.json` and `/api/config` responses identify the board with:

```json
{
  "role": "csi_receiver",
  "board_variant": "ESP32-S3-WROOM-1U",
  "hardware_profile": "s3-enhanced"
}
```

`GET /api/config` also reports `max_idle_rate_hz`, `max_boost_rate_hz`, and
`csi_queue_len` so the Pi/PWA can show the active receiver capability without
guessing from the board name.
