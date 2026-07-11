# Raspberry Pi How-To Guides

Version: `0.4.1`

## How To Configure First Install

1. Copy `raspberrypi/.env.example` to `raspberrypi/.env`.
2. Change `APP_ADMIN_USERNAME` and `APP_ADMIN_PASSWORD`.
3. Replace `SQLCIPHER_KEY` with a long random value.
4. Set SMTP and VAPID values if they are already available.
5. Leave `APP_BOOTSTRAP_ENABLED=true` only until the first owner account exists.

## How To Run The Worker

```powershell
cd raspberrypi\python
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip setuptools
pip install -e .
pip check
python -m esp32_alarm_worker.server
```

The worker supports Python `3.9` or newer. If the install fails while fetching
build dependencies, check the active interpreter with `python3 --version`.
On Python `3.9`, the worker uses the latest compatible `aiohttp` 3.13.x line
because `aiohttp` 3.14 requires Python `3.10` or newer. It also uses
`python-dotenv` 1.2.1 because `python-dotenv` 1.2.2 requires Python `3.10` or
newer.

Check:

```powershell
Invoke-RestMethod http://127.0.0.1:3005/healthz
```

## How To Run The PWA Service

```powershell
cd raspberrypi\PWA
npm install
npm audit fix
npm run build
npm run dev
```

For repeatable deployment installs, use:

```powershell
cd raspberrypi\PWA
npm ci
npm run build
npm start
```

Check:

```powershell
Invoke-RestMethod http://127.0.0.1:3015/api/version
```

## How To Send A Test ESP32 Packet

```powershell
Invoke-RestMethod `
  -Uri http://127.0.0.1:3005/espdata `
  -Method Post `
  -ContentType application/json `
  -Body '{"device_id":0,"name":"Movement00","ip":"192.168.1.42","movement_score":0.25,"movement_detected":false}'
```

Then open:

```powershell
Invoke-RestMethod http://127.0.0.1:3005/internal/status
```

## How To Set Up The Dedicated CSI Sender

Flash `firmware/esp32-csi-sender` to the third ESP32, provision it onto the
same 2.4 GHz Wi-Fi network as the receiver nodes, then set its Pi target to
`http://<pi-lan-ip>:3005/espdata`. Once the sender appears in the PWA ESP32
Nodes card, use its Settings panel or local IP page to choose packet rate, UDP
port, payload size, broadcast IP, and enabled state.

Copy the sender `sta_mac` value into each receiver's `CSI sender MAC` field,
enable `Filter to sender`, start the sender, and then run stillness calibration
on the receivers while the sender is already running at the intended packet
rate.

After the sender is running, open each receiver's MAC histogram in the PWA or on
the ESP32 local page. The protected configured-source-MAC panel should show the
sender MAC. `Seen before filter` rising means ESP-IDF is reporting CSI callbacks
for that MAC. `Accepted after gates` rising means those callbacks are surviving
the receiver's filter, throttle, quality checks, and queue handoff. If the
normal histogram evicts the sender but the protected panel still updates, the
sender is present but quieter than louder router or household traffic.

## How To Point Cloudflare Tunnel

Set the tunnel service target to:

```text
http://127.0.0.1:3015
```

Do not route `:3005` or `/espdata` through Cloudflare.
