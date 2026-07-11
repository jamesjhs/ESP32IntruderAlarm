# ESP32 CSI Sender

Version: `0.4.1`

Dedicated packet-source firmware for the ESP32IntruderAlarm CSI experiment.

This device is not a movement detector. It is a controlled 2.4 GHz Wi-Fi packet
source. Receiver ESP32 devices can filter CSI to this sender's station MAC so
they observe a more repeatable RF stimulus than mixed household/router traffic.

`0.4.1` keeps the sender firmware behaviour from `0.4.0` and documents the new
receiver-side source-MAC diagnostics. After flashing receiver nodes with
`0.4.1`, use the sender `sta_mac` as the receiver `csi_source_mac`, then check
the receiver's protected source-MAC panel to confirm whether sender frames are
seen before filtering and accepted after CSI gates.

## Topology

```text
Raspberry Pi --Ethernet--> household router/AP
                              |
                              | same 2.4 GHz Wi-Fi channel for management
                              v
ESP32 CSI sender  ))) UDP broadcast packets ))) ESP32 CSI receivers
```

The sender joins the same 2.4 GHz network as the receivers so it stays on the
same RF channel and can be reached by the Raspberry Pi/PWA. Its broadcast frames
do not need to be routed by the router before receivers can extract CSI; the
receivers hear the over-air frames directly.

## API

- `GET /status.json` or `GET /api/status`: live sender status.
- `GET /api/config`: current sender configuration.
- `POST /api/config`: partial JSON configuration update.
- `POST /api/start`: enable packet transmission.
- `POST /api/stop`: disable packet transmission.
- `POST /api/provision`: set Wi-Fi credentials using JSON or form data.
- `POST /api/reset-wifi`: clear Wi-Fi credentials and reboot into setup mode.

Important fields:

- `sta_mac`: configure receiver nodes to filter CSI to this MAC.
- `enabled`: whether the sender is emitting UDP broadcast packets.
- `packet_rate_hz`: target packet cadence.
- `udp_port`: destination UDP port for broadcast frames.
- `payload_size`: payload bytes per packet.
- `pi_ip`, `pi_port`, `pi_api_path`: worker telemetry target.

The sender's local landing page exposes the same operational controls for field
setup: start/stop, packet rate, UDP port, payload size, broadcast address,
device identity, and Pi telemetry target. The Pi PWA can adjust the same values
through the sender's `/api/config` endpoint after the sender has posted
telemetry to the worker.

## Build And Flash

```powershell
cd C:\GitHub\ESP32IntruderAlarm\firmware\esp32-csi-sender
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

