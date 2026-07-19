# ESP32IntruderAlarm

An experimental, privacy-preserving intruder alarm that uses Wi-Fi Channel State Information (CSI) from ESP32 boards to detect movement inside a home.

The long-term goal is to place ESP32 sensing nodes (and/or a sending node) around a house and connect them, via the home router, to a Raspberry Pi backend. The ESP32 devices collect CSI features from Wi-Fi traffic. The Raspberry Pi polls each node, fuses their movement scores, exposes a local PWA dashboard, and raises an alarm when movement is detected while the system is armed. The Pi-hosted service is intended to be available via the web interface of the user's choice, e.g., NGINX or Cloudflare Tunnel.

## Current Version

Current prototype version: `0.5.1`.

`0.5.1` adds a separate ESP32-S3-WROOM-1U receiver build with an enhanced CSI
capacity profile while keeping the original ESP32-WROOM-32 receiver and sender
builds backward-compatible. It also adds Pi-managed bounded CSI capture files,
S3 RGB identify feedback, and Pi-side MAC/IP enrichment for CSI histograms using
known telemetry plus intermittent `nmap` discovery.

Recent implementation timeline:

- `0.1.x`: ESP32 CSI node firmware, captive Wi-Fi provisioning, local dashboard,
  node status/config APIs, Pi telemetry posting, and Raspberry Pi PWA scaffold.
- `0.2.0`: Pi-side movement history, aggregate score graph, node settings modal,
  VAPID/push plumbing, version-aware PWA update flow, and mobile UX refinements.
- `0.2.1`: Persistent ESP32 calibration baselines in NVS, calibration
  read/write/delete APIs, Pi proxy routes for calibration, editable calibration
  panels on both dashboards, and fixed high-DPI mobile chart sizing.
- `0.3.0`: Shared Pi version source in `raspberrypi/VERSION`, no `.env` version
  override, generated Python package metadata removed from source control, and
  movement history capped to 24 hours with a 30-minute default view.
- `0.4.0`: Dedicated `esp32-csi-sender` firmware, Pi-managed sender
  start/stop/configuration, receiver-side sender MAC filtering, and updated
  topology documentation for the controlled-packet CSI mode.
- `0.5.1`: Separate `firmware/esp32-s3-wroom` receiver target for
  ESP32-S3-WROOM-1U boards, shared receiver source between WROOM-32 and S3
  builds, S3-enhanced CSI queue/rate/task-stack limits, and board/profile
  reporting in receiver status/config APIs.
- Current `0.5.1` additions: bounded receiver CSI capture from the Pi dashboard
  with downloadable `.ndjson`/metadata files, ESP32-S3 rainbow identify on the
  onboard RGB LED, receiver `sta_mac` status, and nmap-enriched MAC histogram
  identity display in the Pi PWA.

## Why CSI?

Channel State Information describes how a Wi-Fi signal changes as it travels through the environment. Unlike a simple RSSI signal-strength reading, CSI exposes richer per-subcarrier information such as amplitude and phase. People moving through a room disturb multipath Wi-Fi reflections, so CSI can be used as a contactless motion or presence signal without cameras.

This is feasible, but it should be treated as a research/prototyping project rather than a drop-in replacement for commercial alarm sensors. ESP32 CSI has enough public support to justify the experiment:

- Espressif documents CSI support through `esp_wifi_set_csi_rx_cb`, `esp_wifi_set_csi_config`, and `esp_wifi_set_csi`.
- Espressif's ESP-CSI project lists human detection, indoor positioning, and activity monitoring as supported application areas.
- The ESP32-CSI-Tool project demonstrates active and passive CSI collection modes on ESP32 boards.
- Published work has used ESP32 CSI datasets for line-of-sight and non-line-of-sight presence/activity recognition.
- Community projects have already integrated ESP32 CSI motion detection with home-automation-style systems.

## Feasibility Summary

| Area | Assessment | Notes |
| --- | --- | --- |
| Reading CSI on ESP32 | Feasible | ESP-IDF exposes CSI APIs, and reference projects exist. |
| Detecting movement from CSI | Feasible but environment-specific | Movement generally produces visible CSI changes, but thresholds and models must be calibrated per room. |
| Detecting still presence | Harder | CSI is strongest for motion. Static occupancy may need longer observation windows, breathing-level sensitivity, better placement, or machine learning. |
| One ESP32 plus router | Good first prototype | A single station can collect CSI from router traffic/beacons, but packet rate and router behaviour may limit consistency. |
| Multiple ESP32 nodes | Feasible | Three nodes should improve coverage and reduce blind spots if placed with separated signal paths. |
| Dedicated ESP32 packet sender | Recommended for repeatability | A third ESP32 can emit a steady 2.4 GHz broadcast stimulus while receiver nodes filter CSI to its source MAC. This gives receivers a more controlled packet source than mixed household/router traffic. |
| Fewer than three active nodes | Feasible with degraded confidence | The Pi should continue operating with one or two nodes, but reduce coverage, localisation, and alarm confidence accordingly. |
| Coarse disturbance localisation | Research goal | Strategic placement may allow zone-level or heat-map-style inference, but true volumetric mapping is significantly harder than detecting movement. |
| Pi-side learning/training | Feasible if staged | Heuristics, calibration, and small models are realistic on a Raspberry Pi; continuous raw CSI streaming and heavy model training should be avoided or treated as short capture sessions. |
| ESP32 idle/boost sensing | Recommended | Low-rate quiet monitoring with short high-rate burst capture is a good way to reduce Wi-Fi load, Pi CPU, and ESP32 heat. |
| MQTT or UDP ingestion | Optional later | JSON polling is simpler for the first milestone; MQTT/UDP may help if boosted burst streams become too chatty for polling. |
| Rolling retention | Required once deployed | Summary/event retention should be bounded from day one to avoid SD-card wear and unbounded database growth. |
| VAPID push notifications | Feasible | The Pi can send Web Push notifications for alarm events, but subscriptions, user consent, and delivery failures must be handled carefully. |
| Cloudflare Access/App Login | Recommended | Use Cloudflare as the outer login gate for the public hostname, then keep app-level roles, sessions, and audit trails inside the PWA. |
| Cloudflare Access session handling | Required for remote PWA use | The PWA must detect expired Access sessions and force a clean browser re-entry instead of treating HTML login pages as JSON. |
| Version-aware PWA updates | Required | Installed PWAs must check the host version and refresh their service worker, manifest, icons, and app shell when a new payload is available. |
| SQLCipher from first boot | Recommended | Starting with an encrypted database avoids plaintext migration later and is appropriate once the service stores users, sessions, push subscriptions, and alarm history. |
| Internet exposure via Cloudflare Tunnel | Feasible but security-critical | Tunnel avoids opening router ports, but the application still needs strong authentication, rate limiting, session security, and admin controls. |
| Python vs Node.js on the Pi | Prefer split services | Node.js/TypeScript is the better fit for the PWA/auth/push/web layer; Python is the better fit for ESP32 polling, signal processing, and later ML. |
| Reliable intruder alarm | Prototype first | False positives from pets, doors, HVAC, people outside thin walls, and Wi-Fi changes must be measured before trusting it as a security system. |
| Raspberry Pi services and PWA | Straightforward | Use Node.js/TypeScript for the web/PWA service and Python for the polling/signal service, with a shared SQLCipher-backed data layer. |

## Proposed Architecture

```text
Mobile browser / PWA
        |
        | HTTPS: https://house.jahosi.co.uk
        v
Cloudflare Tunnel
        |
        v
Raspberry Pi services
  - Node.js/TypeScript web service: PWA, auth, VAPID, API
  - Python polling service: ESP32 polling, feature processing, fusion
  - SQLCipher database: users, settings, events, node state
        |
        v
Home 2.4/5 GHz router / LAN
        |
        +-- ESP32 receiver node 0: CSI capture + local JSON API
        +-- ESP32 receiver node 1: CSI capture + local JSON API
        +-- ESP32 sender node: managed 2.4 GHz broadcast packet source
```

The first firmware target should be an ESP-IDF application rather than Arduino-only code, because the CSI APIs and Espressif reference material are centred around ESP-IDF.

The home router is the first "illuminator" for the sensing field: normal 2.4 GHz traffic, beacons, and management frames provide a baseline signal path that ESP32 nodes can observe. ESP32 CSI support is mainly a 2.4 GHz capability, so the nodes should be treated as 2.4 GHz sensors even if the home router also serves a 5 GHz SSID. If passive router traffic proves too sparse or inconsistent, the next step is controlled probe traffic rather than assuming the router alone will provide a stable sensing rate.

For controlled CSI experiments, `firmware/esp32-csi-sender` turns the third ESP32
into that probe source. It joins the same 2.4 GHz network for Pi management, then
emits steady UDP broadcast frames that receiver nodes can hear directly and
filter by source MAC. The sender is not a clock synchronizer; it is a stable,
known packet source that makes the receiver-side CSI experiment more repeatable.

Receiver status now exposes both the normal evictable CSI MAC histogram and a
protected configured-source-MAC diagnostic block. The protected block reports
whether the configured sender MAC is being seen before source filtering, whether
any of those frames survive the receiver's throttling/quality/queue gates, and
when that source was last seen or accepted. This is the preferred way to debug a
sender that appears briefly in the histogram but does not contribute usable CSI
samples.

## Physical Placement And Environmental Constraints

CSI movement sensing is highly dependent on the physical radio channel between a
sender and receiver. The system does not simply detect "motion in a room"; it
detects changes in the multipath Wi-Fi field between known devices. The best
installation is therefore one where the radio path is stable when the room is
still, but human movement causes a measurable disturbance.

For the current three-ESP32 topology, start with one dedicated sender and two
receiver nodes on the same 2.4 GHz Wi-Fi channel. A good first layout is a
shallow triangle around the monitored area:

- Sender-to-receiver distance: start around 3 m.
- Normal practical range: roughly 2-6 m in a domestic room.
- Avoid very short links below about 1 m because they can be dominated by
  near-field effects, antenna orientation, body blocking, and gain changes.
- Treat links above about 8-10 m, or links through multiple walls, as advanced
  experiments that need more calibration and validation.
- Receiver-to-receiver distance: roughly 2-5 m, with receivers placed at
  meaningfully different angles rather than side-by-side.

Place the ESP32 boards at about waist to chest height, with consistent antenna
orientation, and keep them fixed after calibration. Do not press boards against
metal, radiators, TVs, large appliances, mirrors, foil-backed insulation, dense
electronics, or the router itself. Cables, enclosures, nearby furniture, and
rotating a board can all change the baseline enough to matter.

The most likely environmental disruptors are moving reflectors and variable
2.4 GHz transmitters. People, pets, doors, curtains, fans, robot vacuums,
washing machines, metallic blinds, and large water-filled objects can all
change the channel. Wi-Fi disruption can come from neighbouring routers,
Bluetooth-heavy areas, microwave ovens, router auto-channel changes, band
steering, and mixed packets from many source MAC addresses. This is why the
dedicated sender and receiver-side source-MAC filtering are preferred over
depending only on ordinary household/router traffic.

For repeatable tests, keep the router's 2.4 GHz channel fixed if possible,
provision the sender and receivers onto the same 2.4 GHz network, start the
sender at the intended packet rate, then calibrate while the space is still and
in its normal state. Doors, fans, heating, curtains, and large movable objects
should be in their usual positions during calibration. Calibration should be a
deliberate user action, not something that automatically happens after power
loss while someone may be moving nearby.

## Project Goals

### Goal 1: Single ESP32 CSI Proof of Concept

Build one standalone ESP32 node that proves CSI can be captured and that the signal changes when a person moves near the Wi-Fi path.

Required behaviour:

- Configure Wi-Fi using a self-hosted SoftAP onboarding flow.
- Store Wi-Fi credentials in non-volatile storage.
- Join the home Wi-Fi as a station after provisioning.
- Enable CSI capture with ESP-IDF Wi-Fi CSI APIs.
- Avoid heavy processing inside the CSI callback; queue or aggregate samples in a normal task.
- Compute simple rolling features, for example amplitude variance, subcarrier energy delta, RSSI, packet count, and a movement score.
- Implement a simple local sensing state machine:
  - `idle`: low-rate feature windows for quiet monitoring, roughly 2-5 Hz if stable.
  - `boost`: short high-rate windows after a movement threshold is crossed, potentially up to the highest reliable sample rate measured on the board.
  - `cooldown`: return to idle after movement settles, while preserving enough recent context for the Pi to classify the event.
- Accept configuration from the Pi for sample-rate targets, thresholds, boost duration, cooldown timing, and feature-window size.
- Persist stillness calibration baselines on the ESP32 so calibrated nodes
  survive reboot and power loss without automatically recalibrating during
  possible movement.
- Serve a local web page for human inspection.
- Serve a JSON endpoint for machine polling.

Candidate JSON shape:

```json
{
  "device_id": 0,
  "name": "esp32-csi-0",
  "ip": "192.168.1.42",
  "uptime_ms": 123456,
  "sample_rate_hz": 80,
  "rssi": -54,
  "noise_floor": -92,
  "movement_score": 0.37,
  "movement_detected": false,
  "calibration_persisted": true,
  "calibration_windows": 40,
  "baseline_age_s": 620,
  "last_packet_ms": 20
}
```

Firmware implementation notes:

- Use FreeRTOS queues/tasks to separate CSI capture, feature aggregation, Wi-Fi management, local HTTP handling, and configuration updates.
- Keep the CSI callback short enough that packet capture is not disrupted by JSON encoding, logging, database-style work, or network calls.
- Consider task/core pinning only after profiling. A dual-core ESP32 may benefit from keeping capture/aggregation work away from web/config handling, but correctness should not depend on a specific core layout.
- Avoid hardcoded sensitivity limits. The Pi should be able to send bounded configuration values, while the ESP32 enforces safe minimum/maximum rates to protect stability.

Success criteria:

- CSI packets are visible at a stable enough rate to graph over time.
- Walking, waving, opening a door, and no-movement baselines produce measurably different feature traces.
- The ESP32 remains reachable from the LAN and can be polled repeatedly without crashing.

### Goal 2: Device Discovery and Numbering

Allow multiple ESP32 nodes to coexist without manual IP tracking.

Preferred approach:

- Use a stable unique identity based on MAC address or a generated device UUID.
- Let the Raspberry Pi assign a logical node number (`0`, `1`, `2`, etc.) during registration.
- Advertise via mDNS where possible, for example `esp32-csi-<id>.local`.
- Persist the assigned node number on the ESP32, but treat the Raspberry Pi as the source of truth.

Avoid relying only on "which other devices are found" during ESP32 boot, because simultaneous boots and router DHCP timing can make numbering unstable.

### Goal 3: Raspberry Pi Services

Build Raspberry Pi services that supervise the sensing network while keeping the internet-facing web/PWA layer separate from the ESP32 polling and signal-processing loop.

Required behaviour:

- Discover or configure ESP32 node addresses.
- Poll each node's JSON status endpoint.
- Track per-node baselines, movement scores, packet freshness, and health.
- Fuse multiple nodes into a house-level alarm state.
- Keep the sensing pipeline alive during movement and reduce polling/storage work during quiet periods.
- Push configuration updates to ESP32 nodes, including idle rate, boost rate, sensitivity thresholds, cooldowns, and raw-capture permissions.
- Read, edit, and clear each node's persisted stillness calibration baseline
  through the Pi dashboard when manual repair or transfer is needed.
- Keep high-frequency samples in memory during burst windows and write compact summaries to storage.
- Enforce bounded retention for events, summaries, calibration captures, and audit logs.
- Provide modes for calibration, heuristic tuning, labelled data capture, and optional model training.
- Expose a local API for a PWA.
- Store recent events and movement history locally in SQLite, with SQLCipher
  preferred for deployed systems.

Recommended split-service stack:

- Node.js/TypeScript web service for the PWA, authentication, VAPID Web Push, admin screens, versioned service worker, and public HTTPS-facing API.
- Python 3 polling service for ESP32 polling, CSI feature ingestion, rolling baselines, movement scoring, node health, signal processing, and future model inference.
- SQLCipher-backed SQLite for persistent users, settings, node registry, events, calibration data, audit logs, and push subscriptions.
- A narrow internal interface between the services: internal HTTP on loopback, a local queue, or database-backed event tables.
- Optional MQTT, UDP, or Home Assistant integration later if burst ingestion or ecosystem integration justifies it.

Data retention and SD-card wear:

- Normal operation should store per-window summaries rather than every CSI packet.
- Candidate summary rows include mean variance, peak variance, trigger count, packet count, RSSI range, node health, coverage level, and alarm decision metadata.
- Use a rolling FIFO retention policy, for example 30 days for detailed summaries and longer only for coarse alarm/audit events.
- Keep raw CSI captures short, labelled, and explicitly started by a user or calibration routine.
- Avoid writing 100 Hz JSON streams directly to SQLite; aggregate in RAM and commit compact rows in batches.

Python polling-service candidates:

- `httpx` or `aiohttp` for polling ESP32 nodes.
- NumPy/SciPy for feature processing.
- scikit-learn for lightweight local classifiers, if needed.
- `systemd` service with restart policy and structured logs.

Node web-service candidates:

- Express or Fastify with TypeScript.
- `web-push` or equivalent for VAPID notifications.
- SQLCipher-capable SQLite driver.
- Static PWA hosting with versioned service-worker assets.

The web service should be the only public application exposed through Cloudflare Tunnel. The polling service should be LAN/internal only.

### Goal 4: PWA Dashboard

Create a mobile-friendly local dashboard for setup, calibration, and arming.

Core views:

- Live node health: online/offline, RSSI, packet rate, movement score.
- Calibration: capture quiet baseline, set sensitivity, test detection, and label known movement zones.
- Remote node configuration: idle Hz, boosted Hz, sensitivity thresholds, boost duration, cooldown timers, and raw-capture mode.
- Alarm state: disarmed, armed, triggered, muted, forced armed, and forced disarmed.
- Schedule/override controls: active hours, temporary mute, vacation/away mode, and acknowledgement flow.
- Event history: movement windows, node confidence, alarm transitions.
- Installable PWA shell for quick phone access on the home network.
- Push notification subscription management per user and per device.
- Version/update status so installed PWAs can confirm they are running the latest host payload.

### Goal 5: Three-Node Intruder Alarm Prototype

Deploy three ESP32 nodes and evaluate whole-house detection.

Prototype questions:

- Which placements produce the strongest movement response?
- Does each node cover a useful zone, or do their signals overlap too much?
- How many false positives occur over a normal day?
- How does detection change when doors open, heating starts, pets move, or the router changes Wi-Fi channel?
- Is a simple threshold enough, or is a trained classifier needed?

### Goal 6: Graceful Degradation with Fewer Active Nodes

Design the Raspberry Pi service so the system remains useful when fewer than three ESP32 nodes are installed, powered, reachable, or healthy.

The system should not fail hard when a node goes offline. Instead, it should recalculate its operating mode, confidence, and available features from the currently healthy nodes. The PWA should make this visible with clear node health and coverage status.

Expected capability by active node count:

| Active ESP32 nodes | Expected functionality | Limitations |
| --- | --- | --- |
| 0 | PWA, configuration, history, health checks, and optional router/Pi self-test only. | No CSI-based movement detection. Alarm should report unavailable or fall back to non-CSI integrations if later added. |
| 1 | Single-link or single-zone movement detection, baseline learning, sensitivity tuning, raw CSI capture, and local graphing. | Limited coverage, higher false-positive/false-negative risk, weak localisation, and little cross-checking. |
| 2 | Better movement confirmation, simple link comparison, reduced false positives, and possible two-zone inference if placement is good. | Still limited volumetric mapping; blind spots remain likely. |
| 3 | Target prototype mode with multi-node fusion, stronger confidence, better coverage, and coarse zone/localisation experiments. | Still needs calibration and should not imply precise 3D tracking. |

Fallback behaviour:

- Maintain a per-node health state: `online`, `stale`, `offline`, `calibrating`, `degraded`, or `faulted`.
- Track the number of healthy nodes and expose an overall coverage level: `unavailable`, `minimal`, `partial`, or `target`.
- Reweight alarm decisions around available nodes rather than waiting forever for missing data.
- Disable or down-rank features that require multiple independent links, such as coarse localisation.
- Increase uncertainty when operating with fewer nodes and show that uncertainty in API responses and the PWA.
- Continue storing events, baselines, and node diagnostics so offline nodes can rejoin without full setup.
- Notify the user when the alarm is armed in a degraded state.

Candidate house-level status JSON:

```json
{
  "armed": true,
  "coverage": "partial",
  "healthy_nodes": 2,
  "expected_nodes": 3,
  "movement_detected": false,
  "alarm_confidence": 0.58,
  "disabled_features": ["volumetric_map"],
  "nodes": [
    { "device_id": 0, "state": "online", "movement_score": 0.12 },
    { "device_id": 1, "state": "online", "movement_score": 0.18 },
    { "device_id": 2, "state": "offline", "last_seen_s": 940 }
  ]
}
```

Success criteria:

- The backend and PWA remain usable with any number of active nodes.
- Missing nodes reduce confidence and features but do not crash polling, training, or event storage.
- Alarm decisions record how many nodes contributed, so later analysis can distinguish full-coverage and degraded events.

### Goal 7: Coarse Position and Volumetric Disturbance Mapping

Investigate whether strategically placed ESP32 devices can estimate where in the house a Wi-Fi disturbance occurred, rather than only whether movement happened.

The practical near-term target is not a precise 3D image of a person. A more realistic target is a coarse occupancy or disturbance map: for example, "movement near the hallway", "movement between the router and kitchen node", or a low-resolution heat map over known room zones. True volumetric mapping would require many stable signal paths, careful calibration, known node positions, controlled traffic, and likely trained models.

Possible approaches:

- Treat each router-to-ESP32 or ESP32-to-ESP32 path as a sensing beam and look for which links are disturbed at the same time.
- Place nodes around the edges of important spaces so movement intersects different signal paths from different angles.
- Generate controlled probe traffic between known transmitters and receivers to make the signal paths more repeatable than passive router traffic alone.
- Build a labelled calibration dataset by walking through known points or zones in the house.
- Use simple geometry first, then compare it with data-driven models once enough labelled examples exist.
- Represent the output as zones or voxels with confidence scores, not as a camera-like image.

Initial map JSON could look like:

```json
{
  "timestamp_ms": 123456789,
  "zones": [
    { "id": "hallway", "movement_score": 0.82, "confidence": 0.71 },
    { "id": "kitchen", "movement_score": 0.24, "confidence": 0.45 },
    { "id": "living_room", "movement_score": 0.12, "confidence": 0.38 }
  ],
  "links": [
    { "from": "router", "to": "esp32-csi-0", "disturbance": 0.78 },
    { "from": "esp32-csi-1", "to": "esp32-csi-2", "disturbance": 0.43 }
  ]
}
```

Success criteria:

- The system can distinguish movement in at least two or three known zones better than chance.
- Localisation confidence degrades gracefully when a node is offline or a signal path is noisy.
- The dashboard can show a simple floor-plan or zone map without implying more precision than the data supports.

### Goal 8: Secure Remote Access, Users, Database, and Push Notifications

Run the Raspberry Pi web service as an internet-reachable but tightly controlled home application at `https://house.jahosi.co.uk`, with Cloudflare Tunnel providing the public route and the Pi remaining behind the home router.

This is feasible and a good fit for a PWA-based alarm dashboard, but it changes the risk profile. Once the service is reachable from the internet, authentication, session management, user enrolment, audit logging, and rate limiting become core alarm-system features rather than polish.

The website should use a TypeScript web service with VAPID Web Push for background browser/PWA notifications, Cloudflare Access/App Login as the outer public gate, SQLCipher-backed settings and secrets, and a version-aware PWA/service-worker update path.

Target network structure:

```text
Phone / desktop browser
        |
        | HTTPS
        v
Cloudflare edge: house.jahosi.co.uk
        |
        | Cloudflare Tunnel
        v
cloudflared on Raspberry Pi
        |
        v
Local PWA/web backend: http://127.0.0.1:3015
Local ESP32 telemetry receiver: http://<pi-lan-ip>:3005/espdata
        |
        v
Home router / LAN / ESP32 nodes
```

Cloudflare Tunnel and Access assessment:

- Cloudflare Tunnel is suitable because it lets the Pi make outbound tunnel connections without opening inbound ports on the home router.
- The public hostname should map only to the Pi web application on local port `3015`, not directly to ESP32 devices or the telemetry receiver.
- The local PWA/web app should bind to `127.0.0.1:3015` where possible, with `cloudflared` as the intended ingress path.
- The ESP32 telemetry receiver should remain on port `3005` at `/espdata` for LAN-only node posts and should not be routed through Cloudflare.
- A local reverse proxy such as Nginx is optional. It may be useful later for static-file serving, compression, TLS termination on the LAN, or routing multiple local services, but it is not required if `cloudflared` can route directly to the Node web service.
- Cloudflare Access/App Login should be the outer gate for `house.jahosi.co.uk`.
- Application-level login is still required even if Cloudflare Access is used, because the app needs roles, audit trails, push subscriptions, and alarm actions tied to specific users.

Cloudflare Access and PWA session handling:

- If Cloudflare Access protects `house.jahosi.co.uk`, the frontend HTTP client must expect that an expired Access session may return an HTML login page where JSON was expected.
- API helpers should check `Content-Type`, HTTP status, and parse failures. If an authenticated API call receives `text/html` or a Cloudflare Access challenge instead of JSON, the PWA should stop background retries and force a top-level browser reload or navigation to the protected origin.
- This reload path should preserve only safe local state, then let Cloudflare re-authenticate the browser before the app resumes.
- The app should show a clear "session expired" state for user-initiated actions, but background polling should fail quietly and avoid flooding the tunnel.
- The service worker must not cache Cloudflare Access login/challenge pages as app-shell responses.

VAPID Web Push assessment:

- VAPID Web Push is a practical way for the Pi service to notify phones and desktops when an alarm triggers, a node goes offline, or the system is armed in degraded mode.
- Each browser/device creates its own push subscription after user consent. The backend stores the subscription against a user account.
- The VAPID private key must be generated once, stored securely on the Pi, and never sent to clients.
- Provide a public endpoint like `GET /api/push/vapid-public-key` and authenticated endpoints to subscribe/unsubscribe the current device.
- Provide owner/admin endpoints to view whether the private key is configured, generate new VAPID keys, and save VAPID settings without ever returning the private key after storage.
- Detect VAPID public-key changes on the client. If the installed PWA has an old subscription, unsubscribe it and create a fresh subscription on the next page load or login.
- Push should be treated as best-effort. The alarm should not depend on push delivery as its only alerting mechanism.
- The backend must handle expired or rejected subscriptions by deleting or disabling them.
- Notification payloads should be self-contained enough for the service worker to display without fetching through a possibly expired Cloudflare Access session.
- Payloads should still avoid sensitive detail. A good pattern is an event type, severity, timestamp, safe title/body text, and a short event ID; the authenticated PWA can fetch full details after login.

Cloudflare Access/App Login assessment:

- Turnstile is not required while the public hostname is protected by Cloudflare Access/App Login.
- Keep invite, password reset, and first-run setup flows behind Cloudflare Access rather than exposing unauthenticated public forms.
- The TypeScript web service should still enforce its own sessions, roles, CSRF protection, rate limits, and audit logs after Cloudflare has admitted the browser.
- Treat Cloudflare identity headers as useful context only after verifying they came through the trusted tunnel path; do not let direct LAN requests spoof public identity.
- Keep local recovery/setup paths local-only or physical-presence gated, not internet-public.

SQLCipher database assessment:

- Use SQLCipher from the first migration, not as a later retrofit.
- Store users, roles, password hashes, sessions/refresh tokens, VAPID settings, push subscriptions, node registry, calibration data, alarm events, and audit logs in the encrypted database.
- Keep the SQLCipher key outside the database file. Candidate options include an environment file with restricted permissions, `systemd` credentials, a boot-time passphrase, or a hardware-backed secret later.
- Backups must preserve encryption and key-handling rules. A copied database without the key should be useless.
- If the service needs unattended reboot, key storage must balance security with availability. A fully manual boot passphrase is safer but less convenient after power cuts.

Suggested initial tables:

- `users`: identity, display name, password hash, role, state, timestamps.
- `sessions`: server-side session or refresh-token records, expiry, device metadata.
- `vapid_settings`: singleton public key, private key, subject, update timestamp.
- `push_subscriptions`: endpoint, public keys, user/device association, enabled state, failure count.
- `nodes`: ESP32 identity, assigned node number, expected/active state.
- `events`: alarm, movement, node health, login, admin action, and configuration events.
- `calibrations`: per-node baseline versions and feature/model metadata.
- `audit_log`: security-sensitive actions and authentication outcomes.

User and login safety protocols:

- First-run setup should create a single owner/admin account and disable open registration by default.
- Additional users should be invite-only, created by an owner/admin, and optionally time-limited until accepted.
- Require strong password hashing such as Argon2id or bcrypt; never store plaintext or reversible passwords.
- Add login throttling and temporary lockouts for repeated failed attempts.
- Use secure, HTTP-only, same-site cookies for browser sessions.
- Regenerate session identifiers after login and privilege changes.
- Require re-authentication for high-risk actions such as adding users, changing notification recipients, exporting data, disabling the alarm, or resetting calibration.
- Consider TOTP/WebAuthn MFA, at least for owner/admin accounts and remote access.
- Record audit events for login attempts, lockouts, user changes, arming/disarming, alert acknowledgement, and notification changes.
- Do not expose ESP32 node APIs through Cloudflare; only the Pi should talk to nodes on the LAN.
- Add CSRF protection for state-changing browser requests.
- Keep a recovery path for the owner account, but make it physical or local-only where possible.

User roles:

| Role | Intended access |
| --- | --- |
| Owner | Full setup, user management, security settings, node management, database backup/export, and alarm control. |
| Admin | Alarm control, node calibration, notification management, and user invitations if allowed by owner. |
| Resident | Arm/disarm, view status, receive notifications, acknowledge alarms. |
| Viewer | View current status and history but cannot change alarm state. |

Success criteria:

- `https://house.jahosi.co.uk` reaches only the Pi service through Cloudflare Tunnel.
- The service is usable by more than one named user, with per-user push subscriptions and auditable alarm actions.
- VAPID configuration is owner/admin manageable and stored encrypted at rest.
- Public access is gated by Cloudflare Access/App Login, while invite/reset/setup flows remain protected by app sessions, CSRF protection, throttling, and audit logging.
- The SQLCipher database exists before any user/session/push data is written.
- Failed login attempts, stale sessions, and unwanted registrations are blocked and logged.
- A stolen database file does not reveal users, push endpoints, alarm history, or calibration data without the database key.

### Goal 9: PWA Versioning and Update Enforcement

Make the PWA update itself reliably from the host whenever the deployed version changes.

Installed PWAs can otherwise keep stale HTML, JavaScript, manifests, icons, and service-worker caches longer than expected. For an alarm dashboard this is not cosmetic: stale clients may miss new security checks, API contracts, notification handling, or alarm-state presentation. From the first PWA implementation, the app should behave like TaskIt's version-aware model: every client knows the host version, checks it with `cache: no-store`, and can refresh its full payload when the version changes.

Required behaviour:

- Expose `GET /api/version` returning the current server/app version and, optionally, a build timestamp or asset revision.
- Store the last seen version locally, for example in `localStorage`.
- Check `/api/version` on every PWA launch, login page load, visibility return, and periodically while the app is open.
- If the host version differs from the stored client version, show an update banner and provide a one-tap refresh.
- For critical security/API updates, allow the server to mark the update as mandatory so the client blocks sensitive actions until refreshed.
- Version service-worker cache names, for example `alarm-<version>`, and delete old caches on activation.
- Apply version query strings to cache-sensitive assets such as `manifest.json`, icons, CSS, app shell JavaScript, and notification badges.
- Use network-first behaviour for app-shell navigations and API calls, with cached fallback only for offline display.
- On update, unregister old service workers if needed, clear old caches, preserve only safe local preferences, and reload from the host.
- Ensure push notification icons/badges use versioned absolute URLs so installed apps refresh notification artwork after deployment.
- Keep a small visible version label in the PWA and admin/about views for support.

Candidate update JSON:

```json
{
  "version": "<VERSION>",
  "build": "2026-07-08T12:00:00Z",
  "mandatory": false,
  "notes": "PWA shell and push handling update"
}
```

Success criteria:

- A browser or installed PWA detects a host version change without relying on the user clearing cache.
- The service worker activates the new cache and removes old versioned caches.
- The client refreshes manifest, icons, CSS, app shell, and service worker payload after a version bump.
- The app can prevent security-sensitive actions from stale clients when the host declares a mandatory update.

### Goal 10: Pi-Side Learning, Training, and Heuristics

Use the Raspberry Pi as the place where node data is calibrated, labelled, compared, and turned into alarm decisions.

This should be feasible if the system is careful about what the ESP32 devices send. The Pi should not need continuous full-rate raw CSI from every node for normal operation. The preferred design is for each ESP32 to calculate small feature summaries locally and send those summaries often, while raw CSI is only captured during short diagnostic or training sessions.

Recommended data modes:

| Mode | Purpose | Network/CPU impact | Recommended use |
| --- | --- | --- | --- |
| Feature polling | Normal alarm operation | Low | ESP32 sends movement score, RSSI, packet rate, variance, selected subcarrier statistics, and health fields. |
| Idle/boost summaries | Efficient event capture | Low to moderate | ESP32 stays in low-rate idle mode, then emits denser summaries or burst data briefly when local variance crosses a threshold. |
| Window summaries | Better detection/fusion | Low to moderate | ESP32 sends per-second or per-window feature vectors for the Pi to fuse across nodes. |
| Raw CSI burst capture | Calibration and debugging | Moderate to high | Implemented as Pi-started bounded receiver captures. Default 30 seconds; outputs downloadable `.ndjson` data and `.json` metadata. |
| Continuous raw CSI streaming | Research only | High | Avoid for normal operation, especially with three nodes over a busy home Wi-Fi network. |
| Offline model training | Model development | Variable | Train on a laptop/desktop if models become large; deploy small trained models back to the Pi. |

Practical Pi-side methods:

- Baseline learning: record quiet periods and maintain rolling per-node baselines.
- Adaptive thresholds: adjust sensitivity by time of day, recent noise, packet rate, and node stability.
- Heuristic fusion: trigger only when movement persists for several windows or appears on meaningful combinations of links.
- Labelled calibration: let the user mark "empty", "walking hallway", "kitchen movement", "door opened", or "pet movement" from the PWA.
- Lightweight classifiers: try logistic regression, random forest, gradient boosting, k-nearest neighbours, or small neural networks on summary features.
- Drift detection: warn when the radio environment has changed enough that recalibration is needed.
- Confidence scoring: expose uncertainty instead of only binary motion/no-motion.

Processing assessment:

- A Raspberry Pi should comfortably handle polling and heuristic fusion for three ESP32 nodes if each node sends compact JSON summaries.
- A Raspberry Pi 4 or 5 should also handle lightweight scikit-learn inference and occasional training on summary features.
- A Raspberry Pi Zero or older Pi may still handle polling and simple thresholds, but model training and live plotting should be kept modest.
- Raw CSI can grow quickly because each packet may contain many subcarrier values. Sending every packet from three nodes as JSON would waste bandwidth and CPU.
- Binary or compact encoded burst uploads are preferable if raw CSI must be transferred.
- The current capture path is intentionally bounded and Pi-owned: the ESP32
  streams chunks, while the Pi stores and serves files for offline analysis.
- CSI histogram MAC rows are enriched on the Pi with known ESP32 node metadata,
  configured source-MAC relationships, intermittent `nmap -sn` scan output, and
  `ip neigh` as a fallback.
- The most robust architecture is edge feature extraction on the ESP32, online decision-making on the Pi, and heavier model exploration on a development machine.
- MQTT or UDP may become useful for high-rate boosted windows, but the first implementation should prove the lower-risk JSON polling path before adding a broker.

Success criteria:

- Normal operation remains responsive while polling all nodes and serving the PWA.
- CPU, memory, and Wi-Fi use remain low enough that the Pi does not miss events during dashboard use.
- The system can run for days using feature summaries without filling storage.
- Raw CSI capture can be enabled briefly for labelled experiments without destabilising alarm monitoring.

### Goal 11: Runtime and Repository Structure

Keep the ESP32 polling service separate from the web/front-end service while keeping both in the same repository.

The best fit is a small monorepo with independently runnable services. This preserves clean ownership boundaries without creating deployment friction on the Pi.

Recommended repository shape:

```text
ESP32IntruderAlarm/
  firmware/
    esp32-csi-node/
    esp32-s3-wroom/
    esp32-csi-sender/
  services/
    web/
      package.json
      src/
      public/
    poller/
      pyproject.toml
      src/
  shared/
    api-contracts/
    db-schema/
  deploy/
    systemd/
    cloudflared/
  docs/
```

Recommended runtime layout on the Raspberry Pi:

| Component | Preferred stack | Responsibility |
| --- | --- | --- |
| Web/PWA service | Node.js + TypeScript on `127.0.0.1:3015` | Public app, auth, roles, VAPID, PWA assets, admin UI, Cloudflare-facing API. |
| Polling service | Python with LAN receiver on `:3005/espdata` | ESP32 telemetry ingest, sparse polling, feature windows, movement scoring, degraded-node handling, UDP probe traffic, future ML inference. |
| Database | SQLCipher SQLite | Persistent state shared through controlled access patterns. |
| Node process manager | PM2 | Run and restart the Node.js/TypeScript web service. |
| System services | `systemd` | Run the Python poller and `cloudflared` with restart policies. |

Python vs Node.js assessment:

- Node.js with TypeScript is the required fit for the web layer because the project needs typed browser-facing APIs, VAPID, PWA versioning, and maintainable auth/session workflows.
- Python is the stronger fit for CSI polling and signal analysis because its ecosystem is better for numerical processing, calibration experiments, plotting, and lightweight ML.
- A pure Node.js build would simplify deployment but make signal-processing and future ML work less natural.
- A pure Python build would simplify polling/ML but make the PWA/auth/VAPID layer less aligned with the intended TypeScript web service.
- Running one Node process and one Python process on a Raspberry Pi 4 or 5 should be comfortably within resource limits if raw CSI streaming is avoided during normal operation.
- The split also reduces blast radius: the web service can restart or deploy without interrupting the poller, and the poller can be tuned without touching login/security code.
- PM2 is a good fit for the Node web service because it provides process restart, logs, environment handling, startup integration, and simple deployment ergonomics for a long-running JavaScript app on the Pi.

Service-boundary recommendation:

- Let the Python poller own live device polling and short-term signal calculations.
- Let the Node web service own users, sessions, push notifications, admin settings, PWA assets, and external APIs.
- Prefer the web service as the main database writer for security-sensitive tables.
- For sensor events, either let the poller call an internal authenticated loopback API on the web service, or write only to narrowly scoped sensor/event tables.
- Avoid two services freely mutating the same security-critical rows.
- If both services access SQLCipher directly, enable careful transaction discipline and use short writes to avoid SQLite lock contention.
- Record a schema/API contract in `shared/` so both services agree on node status, alarm state, event payloads, and version metadata.

Success criteria:

- The Pi can start, stop, and restart the web service and polling service independently.
- PM2 supervises the Node web service and restarts it after crashes or reboot.
- The Cloudflare Tunnel exposes only the Node web service on local port `3015`.
- ESP32 nodes post telemetry only to the Python worker on LAN port `3005` at `/espdata`.
- The polling service continues collecting node health even when the PWA is not open.
- Shared contracts make it clear which service owns each API route, database table, and background job.

## Technical Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Packet rate is too low or inconsistent | Generate controlled traffic between ESP32 nodes, use active sender/receiver modes, or tune router/AP settings. |
| Router channel changes break calibration | Pin 2.4 GHz channel during tests where possible and record channel in node status. |
| False positives from non-intruder movement | Require multi-window confirmation, combine scores across nodes, and add per-room calibration. |
| Static presence is missed | Define the first alarm target as movement detection; treat still presence as a later research goal. |
| Node outage reduces coverage silently | Expose coverage state, healthy node count, and disabled features in both API and PWA before allowing the user to rely on the alarm. |
| Volumetric mapping is overinterpreted | Start with coarse zones and confidence scores; require labelled calibration before displaying any position estimate. |
| Too few independent signal paths | Use strategic perimeter placement and controlled ESP32-to-ESP32 traffic if router-only paths are not enough. |
| Raw CSI overwhelms Wi-Fi or the Pi | Use feature summaries for normal operation; reserve raw CSI for short labelled capture sessions and prefer compact encodings. |
| High-rate storage wears the Pi SD card | Aggregate in RAM, write compact summary rows in batches, use WAL/checkpoint discipline where appropriate, and enforce FIFO retention. |
| Idle/boost thresholds flap between modes | Add hysteresis, minimum state durations, cooldown timers, and Pi-visible state-transition logs. |
| Training overfits to one house layout | Keep labelled test sessions separate, evaluate across days, and expose confidence/drift rather than assuming a model is permanently valid. |
| Internet exposure enables unwanted login attempts | Require Cloudflare Access/App Login for the public hostname, then use invite-only users, login throttling, MFA for privileged accounts, secure sessions, CSRF protection, and audit logging inside the app. |
| Cloudflare Access expiry breaks background API calls | Detect HTML/challenge responses in the PWA HTTP client, stop background retry loops, and force a top-level reload for re-authentication. |
| Push notification data leaks sensitive information | Store push subscriptions encrypted, keep payloads minimal, and require login to view alarm details. |
| SQLCipher key is mishandled | Keep key material outside the database, restrict file permissions, document backup/restore flow, and decide explicitly between unattended boot and manual unlock. |
| Installed PWA runs stale code | Use `/api/version`, versioned cache names, versioned asset URLs, service-worker activation cleanup, and mandatory-update blocking for sensitive changes. |
| Web and poller services fight over SQLite | Define table ownership, keep transactions short, prefer a loopback API or queue for cross-service writes, and avoid concurrent writes to auth/security tables. |
| Mixed Node/Python stack becomes hard to deploy | Keep both services in one repo, add PM2 config for the Node web service, `systemd` units for the Python poller and `cloudflared`, shared `.env` documentation, health checks, and one deployment checklist. |
| ESP32 callback overload | Keep CSI callbacks minimal and process features asynchronously. |
| Unstable device numbering | Assign node IDs from the Pi based on stable device identity rather than boot-order discovery. |
| Security of setup portal/API | Keep provisioning local, use a per-device setup secret, and avoid exposing the dashboard outside the LAN in early versions. |

## Milestones

1. **Research spike**
   - Flash an ESP32 CSI example.
   - Capture raw CSI over serial.
   - Plot quiet vs movement traces on a laptop.

2. **Standalone ESP32 node**
   - Add SoftAP provisioning.
   - Add local web UI and `/status.json`.
   - Add rolling movement score.

3. **Raspberry Pi polling service**
   - Poll one node.
   - Store time-series summaries.
   - Add bounded FIFO retention for summaries/events.
   - Display live graph and node health.

4. **Calibration and thresholding**
   - Add quiet-baseline capture.
   - Add sensitivity settings.
   - Measure false positives over several days.

5. **Three-node fusion**
   - Register three ESP32 nodes.
   - Compare per-node and fused detection.
   - Add alarm state machine.

6. **Graceful degradation**
   - Support zero, one, two, and three active-node modes.
   - Expose coverage level and disabled features.
   - Record node contribution count on every event.

7. **Coarse localisation experiments**
   - Record a floor-plan zone model.
   - Label movement at known positions.
   - Test whether disturbed links can infer room or zone.

8. **Secure remote access and notifications**
   - Publish `https://house.jahosi.co.uk` through Cloudflare Tunnel.
   - Create SQLCipher database before first user setup.
   - Add owner account, invite-only users, roles, sessions, rate limiting, audit log, VAPID push, and Cloudflare Access-aware session handling.
   - Add Cloudflare Access-aware frontend handling for expired sessions returning HTML instead of JSON.

9. **PWA version enforcement**
   - Add `/api/version`, versioned service-worker cache names, and versioned asset URLs.
   - Add update banner and mandatory-update state for stale clients.
   - Verify installed PWAs refresh the full payload after a version bump.

10. **Learning and heuristic tuning**
   - Add labelled capture mode in the PWA.
   - Compare adaptive thresholds with lightweight classifiers.
   - Add model/version metadata and recalibration warnings.

11. **Split-service Pi runtime**
   - Add `services/web` and `services/poller` in the same repository.
   - Define shared API/database contracts.
   - Add PM2 configuration for the Node web service.
   - Add `systemd` units for the Python poller and Cloudflare Tunnel.

12. **PWA and operational polish**
   - Add mobile dashboard.
   - Add arming/disarming.
   - Add event history and notifications.

## Open Design Decisions

- Whether the ESP32 nodes should only observe router traffic or also generate controlled probe traffic.
- Whether the first firmware should start from Espressif ESP-CSI, ESP32-CSI-Tool, or a minimal custom ESP-IDF app.
- Whether movement detection should run mostly on-device, mostly on the Pi, or as a hybrid.
- Whether MQTT/UDP should be added for boosted burst ingestion, or whether JSON polling remains sufficient.
- What idle, boost, and cooldown defaults are safe for the first ESP32 board revision.
- Whether the system should allow arming with degraded coverage or require explicit user acknowledgement.
- Whether coarse position inference should be geometric, machine-learning based, or a hybrid.
- Whether the system should model rooms as named zones, a 2D floor-plan grid, or a sparse 3D voxel map.
- Whether ESP32 nodes should expose raw CSI only on request, or always include a compact rolling feature vector.
- Whether model training should happen on the Pi or on a more powerful development machine with only inference deployed to the Pi.
- Whether the poller should communicate with the web service through an internal HTTP API, direct SQLCipher writes, a local queue, or a hybrid.
- Use Node.js/TypeScript for the web service from day one, with Python reserved for the poller.
- Whether Cloudflare Access should be mandatory for all remote access or allow a local-only bypass on the LAN.
- Whether `cloudflared` should route directly to the Node service or through a local Nginx reverse proxy.
- Whether SQLCipher unlock should be unattended after reboot or require a local/manual unlock step.
- Whether owner/admin accounts should require MFA before any public exposure.
- Which notification events should be pushed immediately and which should only appear in the PWA event history.
- Which update types should be mandatory and block stale clients until refresh.
- Whether the final system should integrate with Home Assistant, MQTT, or remain standalone.
- Whether future hardware should prefer newer ESP32-C5/C6/S3 boards over original ESP32 boards for better CSI support.

## Discussion

### ESP32 Board Variant Assessment for CSI Sensing

The project now keeps the original ESP32-WROOM-32 receiver build for backward
compatibility and adds a separate ESP32-S3-WROOM-1U receiver build at
`firmware/esp32-s3-wroom`. This section assesses the practical limitations of
the WROOM-32 boards for CSI motion sensing and compares them with two upgrade
candidates — the ESP32-S3-WROOM-1/1U and the ESP32-C6 — in terms of reliability
and signal-to-noise ratio.

#### Baseline: ESP32-WROOM-32 DevKit1

| Parameter | Value |
| --- | --- |
| CPU | Dual-core Xtensa LX6 @ 240 MHz |
| SRAM | 520 KB internal |
| Wi-Fi | 2.4 GHz 802.11 b/g/n only |
| Antenna | Single PCB trace (~1–2 dBi) |
| CSI subcarriers | 52 (HT20) / 114 (HT40) |
| Antenna paths | 1 RX, 1 TX |

Known weaknesses for CSI sensing:

- Single receive path — no antenna diversity, so orientation-dependent fades are
  permanent once the board is installed. Small rotations after calibration shift
  baselines measurably.
- 520 KB SRAM fills quickly during burst-mode CSI accumulation. Packet drops
  introduce statistical gaps in variance estimates.
- The LX6 dual-core leaves limited headroom for simultaneously running the CSI
  callback, FreeRTOS queue processing, HTTP server, and mDNS without contention at
  high burst rates.

#### Upgrade Candidate: ESP32-S3-WROOM-1

| Parameter | Change vs WROOM-32 |
| --- | --- |
| CPU | Dual-core Xtensa LX7 @ 240 MHz — approximately 40% faster IPC |
| SRAM | 512 KB internal + up to 8 MB PSRAM via OSPI |
| Wi-Fi | 2.4 GHz 802.11 b/g/n — same band, same CSI API |
| CSI subcarriers | Same count (52/114) |
| Antenna | Dual antenna with hardware diversity switch on the WROOM-1 variant; IPEX/U.FL external connector on the WROOM-1U variant |
| Firmware compatibility | Drop-in — same ESP-IDF CSI API surface |

The key gains for CSI sensing are antenna diversity and PSRAM. Maximum Ratio
Combining across two antenna paths raises the effective received SNR by approximately
3–3.5 dB in a typical indoor multipath environment:

```
3 dB gain   →  linear SNR ×2.0  →  +100% SNR improvement
3.5 dB gain →  linear SNR ×2.2  →  +120% SNR improvement (practical midpoint)
```

This matters because the alarm relies on detecting small amplitude and phase
perturbations in individual subcarriers. A lower noise floor means the minimum
detectable movement shrinks. PSRAM eliminates the burst-mode buffer pressure that
causes packet drops under load, improving sample completeness from roughly 90–95%
under heavy burst rates to near 100%.

If the WROOM-1U variant is used (external antenna connector), an external 2.4 GHz
stub antenna (typically 2–5 dBi) adds a further 1–3 dB over the PCB trace, stacking
on top of the diversity gain for a combined total of approximately 5–6.5 dB
improvement in effective SNR (×3–4.5 linear). Critically, the external antenna also
decouples the board's physical mounting position from the antenna orientation, which
eliminates one of the most common post-calibration baseline drift causes — someone
nudging or reorienting the board.

**Note:** The WROOM-1U variant has an IPEX/U.FL connector in place of the PCB trace
antenna. Confirm the `-1U` suffix on the module label before ordering.

#### Upgrade Candidate: ESP32-C6-DevKitC

| Parameter | Change vs WROOM-32 |
| --- | --- |
| CPU | Single-core RISC-V @ 160 MHz — weaker than the WROOM-32 |
| SRAM | 512 KB |
| Wi-Fi | 2.4 GHz Wi-Fi 6 (802.11ax) |
| Antenna | Single antenna |
| CSI tooling maturity | Low — community tooling is primarily built around the original ESP32 and ESP32-S3 |

The C6 does not offer a reliability improvement for this application at this stage.
Its single-core RISC-V at 160 MHz is slower than the current dual-core LX6, and the
absence of antenna diversity provides no SNR gain over the WROOM-32. The Wi-Fi 6
receiver noise figure may be marginally better (+0.5–1 dB), yielding only a 12–26%
linear SNR improvement — far smaller than the diversity gain available from the S3.
The ESP-CSI community ecosystem, reference datasets, and example code are almost
entirely built around the original ESP32 and ESP32-S3; the C6 CSI port is immature
by comparison.

#### Summary

| Board | Est. reliability gain vs WROOM-32 | Est. SNR gain vs WROOM-32 | Key driver | Note |
| --- | --- | --- | --- | --- |
| ESP32-WROOM-32 DevKit1 | Baseline | Baseline | — | SRAM pressure at burst rates; single fixed-orientation antenna |
| ESP32-S3-WROOM-1 DevKitC | ~25–40% | ~+3–3.5 dB (~×2–2.2 linear) | Dual-antenna MRC + PSRAM + LX7 headroom | Drop-in firmware migration; WROOM-1U variant adds further ~+2–3 dB from external antenna |
| ESP32-C6-DevKitC | Neutral to slightly negative | ~+0.5–1 dB (~×1.1–1.3 linear) | Wi-Fi 6 receiver NF only | Single antenna; weaker CPU; immature CSI tooling — not recommended yet |

The ESP32-S3-WROOM-1U is the strongest upgrade for the receiver role in this
project. The C6 should be reconsidered once Espressif matures its Wi-Fi 6 CSI stack
and community tooling validates its sensing performance.

### Recommended Hardware Topology

Based on the assessment above, the recommended hardware topology for the three-board
prototype is:

- **Two ESP32-S3-WROOM-1U boards** as CSI receiver nodes, each fitted with a
  2.4 GHz external stub antenna.
- **One ESP32-WROOM-32 DevKit1** as the dedicated CSI sender node.

#### Sender Role: ESP32-WROOM-32

The sender's job is to emit a steady, known-MAC UDP broadcast at a configurable rate.
It does not process CSI or perform signal analysis. The requirements it must meet —
sustained UDP broadcast at 20–100 pps, network stability over hours, and Pi-managed
start/stop — are well within the WROOM-32's capability. The existing
`firmware/esp32-csi-sender` target already runs on this board, so no firmware changes
are required.

The sender's single antenna is not a limitation here. Received SNR for CSI sensing is
a receiver-side measurement; the dual-antenna MRC on the S3 receivers compensates for
multipath and orientation effects regardless of the sender's antenna. The one
practical requirement is to fix the sender's physical position and antenna orientation
after calibration, because changes to the sender's radiation pattern shift receiver
baselines in the same way that moving any board does.

#### Receiver Role: ESP32-S3-WROOM-1U

Use `firmware/esp32-s3-wroom` for the S3 receiver build:

```powershell
cd C:\GitHub\ESP32IntruderAlarm\firmware\esp32-s3-wroom
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

This target compiles the same receiver firmware source as
`firmware/esp32-csi-node`, but with the `ESP32-S3-WROOM-1U` board variant,
`s3-enhanced` hardware profile, a 512-sample CSI queue, 160 Hz idle ingest
ceiling, 400 Hz boost ingest ceiling, 120 Hz default boost rate, and larger
receiver task stacks. The standard `firmware/esp32-csi-node` target remains the
ESP32-WROOM-32-compatible build.

The external antenna provides two compounding benefits on the receiver side:

- **Repositionable antenna**: the board can be mounted inside an enclosure or fixed
  to a surface at any angle, while the antenna is aimed into the monitored space
  independently.
- **Stable baseline**: because the antenna position is fixed separately from the PCB,
  accidental board nudges no longer shift the calibrated channel state. This addresses
  one of the most common sources of baseline drift noted in the physical placement
  guidance above.

Combined with the hardware diversity gain, the estimated total improvement over the
WROOM-32 baseline is approximately **5–6.5 dB in effective received SNR** (×3–4.5
linear). For detection reliability, this translates to an estimated **25–40%
improvement** in overall alarm reliability, driven primarily by the elimination of
orientation-dependent fades and burst-mode packet drops.

#### Topology Summary

| Role | Board | Reason |
| --- | --- | --- |
| Receiver node 0 | ESP32-S3-WROOM-1U | Dual-antenna MRC + external antenna + PSRAM |
| Receiver node 1 | ESP32-S3-WROOM-1U | Dual-antenna MRC + external antenna + PSRAM |
| Sender node | ESP32-WROOM-32 | Adequate for broadcast-only sender role; existing firmware; cost-efficient |

This topology applies the upgrade budget to the boards that benefit from it and keeps
the proven WROOM-32 firmware unchanged on the sender.

For `0.5.1` tests, validate the topology from each receiver rather than relying
only on the evictable CSI MAC histogram. The receiver and Pi dashboards expose a
protected configured-source-MAC panel for the sender, showing whether frames are
seen before filtering and whether they are accepted after the receiver CSI
gates. This is the main diagnostic for confirming that the WROOM-32 sender is a
usable CSI source for the upgraded receiver nodes.

## Reference Material

- [ESP-IDF Wi-Fi API: CSI callback/config/enable functions](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_wifi.html)
- [Espressif ESP-CSI solution overview](https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/solution-introduction/esp-csi/esp-csi-solution.html)
- [Espressif ESP-CSI repository](https://github.com/espressif/esp-csi)
- [ESP32-CSI-Tool](https://github.com/StevenMHernandez/ESP32-CSI-Tool)
- [ESP-IDF Wi-Fi provisioning](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/provisioning/provisioning.html)
- [ESP-IDF HTTP server](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_server.html)
- [WiFi CSI-Based Long-Range Through-Wall Human Activity Recognition with the ESP32 dataset](https://zenodo.org/records/8021099)
- [Home Assistant community example: ESP32 CSI human presence](https://community.home-assistant.io/t/human-presence-using-wifi-sensing-csi-on-esp32/791452)
- [MDN Push API](https://developer.mozilla.org/en-US/docs/Web/API/Push_API)
- [W3C Push API](https://www.w3.org/TR/push-api/)
- [Cloudflare Tunnel setup](https://developers.cloudflare.com/tunnel/setup/)
- [Cloudflare Tunnel public hostname routing](https://developers.cloudflare.com/tunnel/routing/)
- [Cloudflare Access for self-hosted applications](https://developers.cloudflare.com/cloudflare-one/access-controls/applications/http-apps/self-hosted-public-app/)
- [SQLCipher documentation](https://www.zetetic.net/sqlcipher/documentation/)
- [SQLCipher API reference](https://www.zetetic.net/sqlcipher/sqlcipher-api/)
- [OWASP Authentication Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html)
- [OWASP Session Management Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Session_Management_Cheat_Sheet.html)
- [OWASP Multifactor Authentication Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/Multifactor_Authentication_Cheat_Sheet.html)
- [TaskIt reference repository](https://github.com/jamesjhs/taskit)
- [TaskIt VAPID Push Guide](https://github.com/jamesjhs/taskit/blob/main/VAPID_PUSH_GUIDE.md)
