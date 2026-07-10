# Python Worker Technical Manual

Version: `0.4.0`

## Responsibilities

The worker owns the local sensing data plane:

- receive ESP32 receiver and sender JSON telemetry
- normalize node identity and timestamps
- keep latest per-node state in memory
- expose internal status to the PWA service
- generate optional UDP probe packets

Future versions should add:

- SQLCipher-backed event writes
- signed or shared-secret node authentication
- sparse polling of ESP32 `/api/status`
- direct configuration application to ESP32 `/api/config`
- house-level alarm fusion
- degraded coverage calculation

## Telemetry Contract

The worker accepts the current firmware status shape from both receiver and
sender ESP32 devices. Important receiver fields include:

- `device_id`
- `name`
- `ip`
- `movement_score`
- `movement_detected`
- `sensing_state`
- `sample_rate_hz`
- `accepted_csi_rate_hz`
- `rssi`
- `last_packet_ms`

Unknown fields are preserved in the node snapshot so firmware can add values
without breaking the worker.

Sender nodes use the same `/espdata` ingestion path but identify themselves with
`role: "csi_sender"`. They usually provide fields such as `enabled`,
`packet_rate_hz`, `udp_port`, `payload_size`, `broadcast_ip`, and `sta_mac`.
The worker stores these fields without interpreting them as movement data so the
PWA can discover the sender and proxy configuration requests to the sender's
own HTTP API.

## UDP Probe Loop

The UDP probe loop is disabled by default. When enabled, it sends a fixed
payload to `UDP_PROBE_TARGET_IP:UDP_PROBE_TARGET_PORT` every
`UDP_PROBE_INTERVAL_SECONDS`.

Probe traffic should be treated as part of the sensing environment. Calibrate
stillness while the normal probe loop is already running.

For the preferred controlled-source topology, use the dedicated ESP32 sender
instead of relying on the Pi UDP probe loop. The sender gives receiver ESP32
boards a known 2.4 GHz source MAC to filter against, while the Pi remains the
management and telemetry hub.
