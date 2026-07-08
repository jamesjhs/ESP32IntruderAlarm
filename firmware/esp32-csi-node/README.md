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


## Installation

# Build and flash ESP32

> how to buidld the files and send to the esp32 chip?

<details><summary>6 previous messages</summary>

> I’ll take a quick look at the repo layout so I can give you the exact build/upload path for this project instead of generic ESP32 advice.
>
> <details><summary>Ran 2 commands</summary>
>
> - Ran `Get-ChildItem -Force`
> - Ran `rg --files`
>
> </details>
>
> This is an ESP-IDF style firmware project under `firmware\esp32-csi-node`; I’m checking its local README/config now for board-specific notes.
>
> <details><summary>Ran 3 commands</summary>
>
> - Ran `Get-Content -Raw firmware\esp32-csi-node\README.md`
> - Ran `Get-Content -Raw firmware\esp32-csi-node\CMakeLists.txt`
> - Ran `Get-Content -Raw firmware\esp32-csi-node\sdkconfig.defaults`
>
> </details>
>
> The repo already has the essential commands, so I’m just checking whether the top-level README adds install or troubleshooting details before I boil it down.
>
> <details><summary>Ran 2 commands</summary>
>
> - Ran `Select-String -Path README.md -Pattern "ESP-IDF|idf.py|flash|COM|firmware|build" -Context 2,3`
> - Ran `Get-Command idf.py -ErrorAction SilentlyContinue | Format-List *`
>
> </details>
</details>

This repo uses **ESP-IDF**, not Arduino. The firmware project is here:

`C:\GitHub\ESP32IntruderAlarm\firmware\esp32-csi-node`

On your machine, `idf.py` does **not** appear to be available in the current PowerShell PATH, so first install/open ESP-IDF.

**Steps on Windows**

1. Install ESP-IDF from Espressif:
   - Use the Windows ESP-IDF installer.
   - After install, open **ESP-IDF PowerShell**, not normal PowerShell.

2. Plug the ESP32 into USB.

3. Find the COM port:
   ```powershell
   Get-PnpDevice -Class Ports
   ```
   Look for something like `COM3`, `COM4`, etc.

4. Build the firmware:
   ```powershell
   cd C:\GitHub\ESP32IntruderAlarm\firmware\esp32-csi-node
   idf.py set-target esp32
   idf.py build
   ```

5. Flash it to the ESP32, replacing `COMx` with your port:
   ```powershell
   idf.py -p COMx flash monitor
   ```

   Example:
   ```powershell
   idf.py -p COM4 flash monitor
   ```

If flashing gets stuck at “Connecting…”, hold the ESP32 **BOOT** button while it starts flashing, then release it once writing begins.

After flashing, the board should start a Wi-Fi setup access point named something like:

```text
ESP32-CSI-SETUP-xxxxxx
```

Connect to that Wi-Fi network, then open:

```text
http://192.168.4.1/
```

From there you can enter your 2.4 GHz Wi-Fi SSID/password.
