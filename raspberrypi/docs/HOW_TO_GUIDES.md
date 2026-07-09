# Raspberry Pi How-To Guides

Version: `0.0.1`

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

Check:

```powershell
Invoke-RestMethod http://127.0.0.1:1000/healthz
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
Invoke-RestMethod http://127.0.0.1:3000/api/version
```

## How To Send A Test ESP32 Packet

```powershell
Invoke-RestMethod `
  -Uri http://127.0.0.1:1000/espdata `
  -Method Post `
  -ContentType application/json `
  -Body '{"device_id":0,"name":"Movement00","ip":"192.168.1.42","movement_score":0.25,"movement_detected":false}'
```

Then open:

```powershell
Invoke-RestMethod http://127.0.0.1:1000/internal/status
```

## How To Point Cloudflare Tunnel

Set the tunnel service target to:

```text
http://127.0.0.1:3000
```

Do not route `:1000` or `/espdata` through Cloudflare.
