# Python Worker Technical Manual

Version: `0.0.1`

## Responsibilities

The worker owns the local sensing data plane:

- receive ESP32 JSON telemetry
- normalize node identity and timestamps
- keep latest per-node state in memory
- expose internal status to the PWA service
- generate optional UDP probe packets

Future versions should add:

- SQLCipher-backed event writes
- signed or shared-secret node authentication
- sparse polling of ESP32 `/api/status`
- configuration application to ESP32 `/api/config`
- house-level alarm fusion
- degraded coverage calculation

## Telemetry Contract

The worker accepts the current firmware status shape. Important fields include:

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

## UDP Probe Loop

The UDP probe loop is disabled by default. When enabled, it sends a fixed
payload to `UDP_PROBE_TARGET_IP:UDP_PROBE_TARGET_PORT` every
`UDP_PROBE_INTERVAL_SECONDS`.

Probe traffic should be treated as part of the sensing environment. Calibrate
stillness while the normal probe loop is already running.
