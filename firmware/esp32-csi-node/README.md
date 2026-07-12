# ESP32 CSI Node

Version: `0.5.1`

Standalone ESP-IDF firmware for an ESP32 Wi-Fi CSI movement sensor node.

The node captures Wi-Fi Channel State Information (CSI), reduces it into robust
movement features, and can be provisioned onto a home 2.4 GHz Wi-Fi network
through a captive setup page.

The preferred system architecture is Pi-centred: ESP32 nodes perform local CSI
capture and first-pass trigger scoring, while a Raspberry Pi provides the main
dashboard, sends controlled UDP probe traffic, stores telemetry, and coordinates
configuration.

## Current Release Notes

`0.5.1` keeps this folder as the backward-compatible ESP32-WROOM-32 receiver
build and adds a separate S3 receiver build in `../esp32-s3-wroom`. Both receiver
targets compile the same `main.c`, so future receiver functions should be added
once and picked up by both builds.

The standard build keeps the existing WROOM-32 capacity profile:

- CSI queue length: `256` samples
- Idle CSI ingest ceiling: `100 Hz`
- Boost CSI ingest ceiling: `250 Hz`
- Default boost rate: `80 Hz`

The S3 build reports `board_variant: "ESP32-S3-WROOM-1U"` and enables the
enhanced 512-sample queue, 160 Hz idle ceiling, 400 Hz boost ceiling, 120 Hz
default boost rate, and larger task stacks.

Protected configured-source-MAC diagnostics from the previous controlled-source
work remain available. Receiver nodes keep non-evicting counters for the
configured sender MAC, alongside the existing top-10 CSI MAC histogram:

- `seen_before_filter`: CSI callbacks whose reported MAC matches the configured
  sender before source filtering and quality gates.
- `accepted_after_gates`: matching callbacks that survive source filtering,
  rate throttling, minimum CSI length/SNR checks, spike filtering, and queue
  handoff.
- `last_seen_ms` and `last_accepted_ms`: freshness indicators for the configured
  sender path.

Controlled-source CSI support from `0.4.0` remains in place. Receiver nodes can
be configured with the dedicated sender's station MAC and can ignore CSI frames
from other sources, making movement scoring less dependent on mixed
household/router traffic. These settings are available from the Pi PWA and the
receiver `/api/config` endpoint.

## Build and Flash

Open an ESP-IDF PowerShell so `idf.py`, CMake, Ninja, Python, and the Xtensa
toolchain are on `PATH`.

```powershell
cd C:\GitHub\ESP32IntruderAlarm\firmware\esp32-csi-node
idf.py set-target esp32
idf.py build
idf.py -p COMx flash monitor
```

For ESP32-S3-WROOM-1U receiver boards, use the separate target:

```powershell
cd C:\GitHub\ESP32IntruderAlarm\firmware\esp32-s3-wroom
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the ESP32 serial port. On Windows, this can usually be found
with:

```powershell
Get-PnpDevice -Class Ports
```

If flashing stalls at `Connecting...`, hold the ESP32 BOOT button until writing
starts, then release it.

## Provisioning

On first boot, or when Wi-Fi credentials have been reset, the node starts a setup
access point:

- SSID: `Movement-Setup-xxxxxx`
- Setup page: `http://192.168.4.1/`

The setup AP also runs captive DNS and redirects unknown HTTP GET requests to the
landing page, so phones and laptops should usually open the setup page as a
Wi-Fi sign-in portal.

Submit a 2.4 GHz Wi-Fi SSID and password. The credentials are saved in NVS and
the board reboots into station mode.

## Node Identity

Freshly configured nodes auto-assign a friendly name after joining the real
network. The node scans the local subnet for other configured nodes exposing
`/status.json`, then chooses the next free hexadecimal ID:

- `Movement00` / `device_id = 0x00`
- `Movement01` / `device_id = 0x01`
- `Movement0A` / `device_id = 0x0A`

The assigned name is saved to NVS and used as the station hostname.

## Intended Multi-Node Topology

The current design direction is:

- The Raspberry Pi is the normal dashboard and coordination server.
- A dedicated ESP32 sender is the preferred steady 2.4 GHz packet source. It
  joins the same Wi-Fi network for management, then emits broadcast UDP frames
  that receivers can hear directly.
- Optional Pi UDP probe traffic can still be used for experiments, but should
  be calibrated as part of the normal RF pattern if enabled.
- Each ESP32 runs local CSI feature extraction and trigger detection.
- Each ESP32 pushes compact trigger/status telemetry to the Pi every 5-10
  seconds, and should also send immediate event telemetry when movement is
  detected.
- The Pi displays each chip separately, performs multi-node fusion, and handles
  logging/alerting.
- Configuration changes are sent from the Pi to each ESP32 using the node HTTP
  configuration API.
- ESP32 web pages are primarily for Wi-Fi setup, local diagnostics, stillness
  calibration, and chip identifier reassignment.

Avoid disconnect/reconnect sensing cycles. Repeatedly leaving and rejoining the
Wi-Fi network can shift CSI baselines, introduce DHCP/association delays, and
make the detector respond to network topology changes instead of movement. Keep
the nodes associated to the 2.4 GHz network and make the normal data path quiet
and predictable.

## Local Diagnostic Dashboard

`GET /` serves a local diagnostic dashboard. It is useful for setup, calibration,
and tuning, but it should not be treated as the normal production UI when the Pi
dashboard is available. Browser polling adds HTTP traffic, CPU work, and extra
Wi-Fi frames, all of which can perturb CSI measurements.

The dashboard includes sliders and fields for:

- Device ID
- Raspberry Pi IP, port, and receiving API path
- Detection threshold
- Settle threshold
- Sensitivity
- Minimum and maximum sample rate
- Boost hold time before entering cooldown
- Cooldown time
- Feature window
- Graph score range
- Graph sample-rate range
- Graph update rate

The detection sliders update `/api/config`. The graph range and graph update
rate sliders affect only the browser chart and are not persisted.

## Stillness Calibration

Use **Auto-calibrate stillness** on the dashboard after the node is installed and
the room is quiet.

Calibration runs for 10 seconds. During this time the detector suppresses motion
triggers, averages the current still-room CSI feature windows, replaces its
baseline energy, variance, shape, phase, and noise estimates, and saves that
baseline to NVS so it survives reboot or power loss. The dashboard and
`/status.json` expose calibration progress and persistence through:

- `calibrating`
- `calibration_remaining_ms`
- `calibration_persisted`
- `calibration_windows`

The ESP32 dashboard and Pi node settings modal expose the persisted calibration
values for inspection or manual repair:

- `baseline_energy`
- `baseline_variance`
- `baseline_shape`
- `baseline_phase`
- `baseline_phase_variance`
- `baseline_noise`
- `baseline_phase_noise`
- `calibration_windows`

## Local Endpoints

- `GET /` serves the dashboard.
- `GET /status.json` and `GET /api/status` return live node state.
- `GET /log/` returns a plain-text snapshot of the last 100 in-memory log
  lines since boot. `GET /log` is also accepted.
- `GET /api/config` returns tunable sensing configuration.
- `POST /api/config` accepts partial JSON config updates from the Pi dashboard
  or the local diagnostic page. Changes are saved to NVS.
- `POST /api/calibrate` starts a 10 second stillness calibration.
- `POST /api/identify` rapidly blinks the node's blue LED for 10 seconds, then
  restores the normal movement LED state.
- `GET /api/calibration` returns the persisted stillness calibration baseline.
- `POST /api/calibration` accepts partial JSON calibration baseline updates,
  saves them to NVS, and applies them to the live detector.
- `DELETE /api/calibration` deletes the current stillness calibration baseline
  from NVS and resets the live detector baseline.
- `POST /api/provision` stores Wi-Fi credentials from the setup portal.
- `POST /api/reset-wifi` clears stored Wi-Fi credentials and reboots into setup mode.

Example config update:

```json
{
  "device_id": 0,
  "pi_ip": "192.168.1.10",
  "pi_port": 3005,
  "pi_api_path": "/espdata",
  "csi_source_mac": "AA:BB:CC:DD:EE:FF",
  "csi_source_filter_enabled": true,
  "pi_post_interval_ms": 5000,
  "idle_rate_hz": 10,
  "boost_rate_hz": 80,
  "movement_threshold": 3.0,
  "settle_threshold": 1.2,
  "motion_sensitivity": 1.0,
  "boost_duration_ms": 8000,
  "cooldown_ms": 15000,
  "feature_window_ms": 250
}
```

The firmware clamps values before storing them:

- `device_id`: 0-255
- `pi_ip`: empty or valid IPv4 address
- `pi_port`: 1-65535, default 3005
- `pi_api_path`: receiving API path, default `/espdata`
- `csi_source_mac`: empty or a dedicated sender MAC such as `AA:BB:CC:DD:EE:FF`
- `csi_source_filter_enabled`: true to ignore CSI frames from other sources
- `pi_post_interval_ms`: 1000-30000 ms
- `idle_rate_hz`: 10-100 Hz
- `boost_rate_hz`: 10-250 Hz on the standard ESP32-WROOM-32 build; 10-400 Hz
  on the ESP32-S3-WROOM-1U build
- `movement_threshold`: 1.0-10.0
- `settle_threshold`: 0.2 to `movement_threshold`
- `motion_sensitivity`: 0.3-3.0
- `boost_duration_ms`: 0-20000 ms
- `cooldown_ms`: 0-20000 ms
- `feature_window_ms`: 0-1000 ms. This is the analysis window used to group
  accepted CSI samples before one movement score is calculated; `0` is treated
  as a near-immediate 1 ms window internally, not as "score every event".

The standard ESP32-WROOM-32 build clamps `boost_rate_hz` to 250 Hz. The
ESP32-S3-WROOM-1U build clamps it to 400 Hz and reports `max_boost_rate_hz`,
`max_idle_rate_hz`, and `csi_queue_len` from `GET /api/config`.

When `device_id` is changed without explicitly sending `name`, the node
regenerates its friendly name as `Movement%02X`, for example decimal device ID
2 becomes `Movement02`. The local diagnostic page displays and edits the device
ID in decimal form.

The Pi receiving endpoint is stored as editable pieces: `pi_ip`, `pi_port`, and
`pi_api_path`. `GET /api/config` also reports the derived debug value
`pi_api_url` as `http://{pi-IP-address}:{pi-port}/espdata` by default, using the
configured path when changed. A path sent without a leading slash is normalized
before being saved to flash.

`idle_rate_hz` and `boost_rate_hz` are local CSI ingest targets. They cap how
often the Wi-Fi callback queues reduced CSI samples for the aggregation task;
they do not force the router or access point to transmit at that rate.
`sample_rate_hz` in `/status.json` is the measured accepted sample rate for the
latest feature window.

`accepted_csi_rate_hz` is the measured callback acceptance rate. If this stays
around 30-40 Hz while UDP probe traffic is higher, the limiting factor is likely
the Wi-Fi/CSI delivery path rather than the dashboard graph scale. If
`throttled_samples` rises quickly, the configured idle/boost ingest cap is the
limiter. If `rejected_samples` rises quickly, packets are arriving but failing
quality checks. If `queue_drops` rises, the aggregation task is not draining the
callback queue fast enough.

## CSI Detection Summary

The firmware now uses a more robust CSI movement pipeline than a raw amplitude
threshold:

- Can optionally filter CSI to a dedicated sender MAC so movement scoring uses a
  controlled packet source instead of mixed household traffic.
- Skips invalid first CSI bytes when ESP-IDF marks them invalid.
- Computes per-subcarrier power using `I^2 + Q^2`.
- Rejects malformed, too-short, or poor-SNR CSI samples.
- Clamps one-off packet spikes before window aggregation.
- Scores movement from calibrated energy deviation, variance change, subcarrier
  shape change, rolling median/MAD trend deviation, and a weak phase-proxy
  variance component.
- Applies `motion_sensitivity` as a multiplier to the fused score while keeping
  the repeated-window confirmation logic in place.
- Requires repeated evidence across multiple windows before entering movement
  boost state.
- Requires repeated quiet windows before returning to idle.

## Glossary

This section explains the dashboard tiles, chart lines, configuration fields,
and API values in plain English.

### Live Status Readouts

- `device_id`: The node's numeric identity, from 0 to 255. The dashboard edits
  this in decimal. Internally it is also used to generate names such as
  `Movement00`, `Movement01`, and `Movement0A`.
- `name`: The friendly node name. By default this is `Movement` followed by the
  two-digit hexadecimal device ID. The name is also used as the station hostname
  where the network stack supports it.
- `ip`: The node's current IP address on the Wi-Fi network. `0.0.0.0` means it
  does not yet have a station IP address.
- `uptime_ms`: Milliseconds since the ESP32 firmware booted.
- `sensing_state`: The detector's current mode. `idle` uses the minimum
  sample-rate cap. `boost` means movement has been confirmed and the node is
  using the maximum sample-rate cap. `cooldown` is the wait period after boost
  before the detector can settle back to idle.
- `sample_rate_hz`: The measured number of CSI samples that were actually used
  in the most recent feature window. This is not a command to the router or Pi;
  it is what the ESP32 actually processed.
- `accepted_csi_rate_hz`: A one-second measurement of how many CSI callbacks
  passed the callback-level filters and were accepted by the firmware. This is
  useful for checking whether probe traffic is really reaching the CSI callback.
- `rssi`: Received signal strength for the most recent accepted CSI packet, in
  dBm. Less negative values are stronger, for example `-45` is stronger than
  `-75`. Very weak or unstable RSSI can make movement detection noisy.
- `noise_floor`: The radio's reported background noise level for the most recent
  packet. Comparing RSSI with noise floor gives a rough signal-to-noise sense.
- `amplitude_variance`: The variance of CSI energy inside the most recent
  feature window. Higher variance means the CSI amplitude was changing more
  within that window.
- `subcarrier_energy_delta`: The normalized difference between the current
  window's CSI energy and the calibrated baseline. This is one contributor to
  `movement_score`.
- `movement_score`: The fused movement score after sensitivity is applied. It
  combines energy change, variance change, subcarrier shape change, recent trend
  deviation, and a weak phase proxy. A single spike above threshold is only
  movement evidence; the detector normally needs repeated windows before
  `movement_detected` turns true.
- `baseline_noise`: The detector's estimate of normal still-room CSI noise. A
  higher baseline means the link is noisy, so the same physical movement may
  produce a lower score.
- `trend_score`: A robust comparison between the current window and a rolling
  recent median. It helps identify sudden departures from the recent normal
  pattern while reducing the effect of isolated outliers.
- `phase_score`: A cautious phase-like score derived from CSI I/Q changes. ESP32
  CSI phase is noisy, so this is deliberately weighted lightly.
- `movement_detected`: The final movement event flag. It becomes true only after
  enough recent windows support movement, or while the detector is already in
  boost state.
- `baseline_age_s`: Seconds since the current baseline was created or last
  replaced by stillness calibration.
- `last_packet_ms`: Milliseconds since the last accepted CSI packet. If this is
  large, the node is not currently receiving usable CSI.
- `last_csi_mac`: Raw most recent MAC address reported by ESP-IDF before source
  filtering. When `csi_source_filter_enabled` is true, this can still show
  router or household-device MACs because it is captured before rejection.
- `last_filtered_csi_mac`: Most recent MAC rejected by the source filter. When
  filtering is working in a busy network, this often shows non-sender devices
  and is useful evidence that the filter is actively discarding other traffic.
- `last_accepted_csi_mac`: Most recent MAC that passed source filtering,
  throttling, CSI length/SNR checks, spike filtering, and queue handoff into
  feature processing. With `Filter to sender` enabled and usable sender frames
  arriving, this should match `csi_source_mac`.
- `accepted_samples`: Total accepted CSI samples since boot. This should climb
  steadily when the sender, Pi probe traffic, or router is generating useful
  traffic.
- `csi_source_mac_diagnostics`: Protected diagnostics for the configured sender
  MAC. Unlike the top-10 histogram, this object is not evicted by louder router
  or household devices. `seen_before_filter` counts matching CSI callbacks
  before source filtering, while `accepted_after_gates` counts matching samples
  that survive the receiver's source filter, rate throttle, quality checks, and
  queue handoff into feature processing. If `seen_before_filter` climbs but
  `accepted_after_gates` does
  not, the sender is visible but later rejected or throttled. If neither climbs,
  ESP-IDF is not reporting CSI callbacks with the configured MAC.
- `packet_count`: Total CSI samples included in completed detection windows
  since boot.
- `last_window_packets`: Number of accepted CSI samples in the most recent
  feature window. If this is below 3, the current scoring guard treats the
  window as too sparse to support a movement decision, so `movement_score` can
  read as zero even though occasional CSI packets are arriving.
- `rejected_samples`: CSI packets discarded before analysis because they were
  malformed, too short, or had poor signal quality.
- `filtered_samples`: Packets accepted but clipped or filtered because they
  looked like isolated spikes. This helps prevent one-off packet artefacts from
  polluting the baseline or triggering false positives.
- `throttled_samples`: CSI packets intentionally skipped because they arrived
  faster than the current idle or boost sample-rate cap.
- `queue_drops`: CSI packets lost because the FreeRTOS queue was full. If this
  rises, the Wi-Fi callback is producing data faster than the aggregation task
  can drain it.
- `confirm_windows`: Count of recent/consecutive windows that crossed the
  movement threshold. This explains why a one-window graph spike may not trigger
  `movement_detected`.
- `quiet_windows`: Count of windows below the settle threshold. These windows
  are used to decide when the detector is calm enough to leave cooldown or
  return to idle.
- `calibrating`: True while the 10 second stillness calibration is running.
  Movement detection is suppressed during this period.
- `calibration_remaining_ms`: Milliseconds remaining in the current stillness
  calibration.
- `wifi_provisioned`: True when Wi-Fi station credentials exist in flash.
- `sta_connected`: True when the node is currently connected to the configured
  Wi-Fi network as a station.

### Configuration Fields

- `pi_ip`: The Raspberry Pi server IP address. It may be blank until the Pi has
  a known address.
- `pi_port`: The Raspberry Pi HTTP port for the receiving API. The default is
  `3005`.
- `pi_api_path`: The receiving API path on the Pi. The default is `/espdata`.
  If a path is posted without a leading slash, the firmware adds one before
  saving it.
- `pi_api_url`: A derived debug value returned by `GET /api/config`. It is built
  from `pi_ip`, `pi_port`, and `pi_api_path`; it is not stored separately.
- `pi_post_interval_ms`: How often the node should post compact telemetry to the
  Pi receiving API. It is stored in milliseconds, defaults to 5000 ms, and is
  configurable from 1000 to 30000 ms.
- `idle_rate_hz`: The local CSI ingest cap while the detector is idle or in
  cooldown. It limits how often accepted samples are queued locally; it cannot
  force the router or Pi to transmit that often.
- `boost_rate_hz`: The local CSI ingest cap while the detector is in boost after
  movement is confirmed.
- `movement_threshold`: The `movement_score` level that marks a feature window
  as movement evidence. Lower values are more sensitive but can increase false
  positives.
- `settle_threshold`: The score level considered quiet. It should remain below
  `movement_threshold`. Lower values make the detector demand a calmer link
  before it returns to idle.
- `motion_sensitivity`: A multiplier applied to the fused score. Higher values
  make scores larger, but this does not bypass the repeated-window confirmation
  rule.
- `boost_duration_ms`: How long the node remains in boost after movement is
  confirmed. It is configurable from 0 to 20000 ms.
- `cooldown_ms`: How long the node waits after boost before it is allowed to
  return to idle, provided enough quiet windows have arrived. It is configurable
  from 0 to 20000 ms.
- `feature_window_ms`: How much time is grouped into one detection decision.
  Think of it as the CSI analysis window or detector shutter speed. Shorter
  windows can react faster but contain fewer packets and are noisier; longer
  windows collect more evidence and reject more false positives, but respond
  more slowly. This is related to sensitivity, but it is not an inverse
  sensitivity knob and it is not an event trigger. With the current sparse-window
  guard, very short windows, especially `0`, can produce mostly zero scores
  because each window often contains fewer than 3 accepted CSI packets.

### Chart and Page Controls

- Movement score line: The green chart line. It shows the latest
  `movement_score` values over time.
- Sample rate line: The blue chart line. It shows `sample_rate_hz`, scaled
  against the right-side Hz axis.
- Detection threshold line: The yellow dashed chart line. Score windows above
  this line count as movement evidence.
- Settle threshold line: The dark grey chart line. Score windows below this line
  count as quiet evidence.
- Graph Score Max: The top of the chart's score axis. This is display-only and
  is not saved to flash.
- Graph Rate Max: The top of the chart's sample-rate axis. This is display-only
  and is not saved to flash.
- Graph Update Rate: How often the browser polls the ESP32 and redraws the
  chart. Higher values make the page feel more live, but they add HTTP and Wi-Fi
  traffic that can disturb CSI measurements.
- Auto-calibrate stillness: Starts a 10 second calibration period. Keep the room
  still and keep normal Pi probe traffic running during this period so the
  baseline matches real operating conditions.
- Pi Posting Interval: How often the node should send compact telemetry to the
  configured Pi API. The dashboard shows this in seconds from 1 to 30.

## Throughput Notes

Testing showed that direct UDP traffic to the ESP32 can drive the movement score
very high because the probe traffic changes airtime, RSSI/AGC behavior, frame
timing, and CSI amplitude/phase statistics. Calibrate stillness while the normal
Pi probe traffic is already running, then detect movement on top of that stable
traffic pattern.

Observed usable CSI rates may top out around 30-40 Hz on this ESP32/AP/link
combination, especially while the ESP32 diagnostic page is being viewed. Closing
the ESP32 page and using the Pi as the only dashboard should reduce self-induced
traffic and gives a cleaner measurement. The Pi should poll ESP32 HTTP endpoints
sparingly; routine telemetry should be pushed from the ESP32 nodes to the Pi in
compact periodic/event messages.

## Troubleshooting

If `idf.py` is not recognized, the current shell has not loaded the ESP-IDF
environment. Open the ESP-IDF PowerShell shortcut installed by Espressif, or run
the ESP-IDF export/init script for your install before building.

If the setup page does not open automatically, connect to the `Movement-Setup-*`
Wi-Fi network and browse directly to:

```text
http://192.168.4.1/
```
