/*
 * Browser dashboard for the ESP32 Intruder Alarm PWA.
 *
 * Purpose:
 * - Owns all client-side behavior for `public/index.html`: live status tiles,
 *   movement history charts, admin panels, node settings modals, push
 *   subscription controls, and version-update prompts.
 * - Talks only to the Raspberry Pi TypeScript service in `src/server.ts`. The
 *   browser does not call ESP32 devices directly; node status/config/calibration
 *   actions are proxied through the Pi so CORS, timeout, and LAN-IP validation
 *   live in one backend.
 * - Registers `service-worker.js`, monitors `/api/version`, and prompts users
 *   to refresh when the host app version changes.
 *
 * Interactions:
 * - Reads `window.ALARM_APP_CONFIG` from `/app-config.js`.
 * - Uses `/api/status` for Python-worker/ESP32 health, `/api/admin/summary` for
 *   admin state, `/api/history/movement` for charts, `/api/push/*` for Web Push,
 *   and `/api/nodes/:deviceId/*` for ESP32 node actions.
 * - Receives service-worker messages when browser push subscription state
 *   changes.
 */
const appConfig = window.ALARM_APP_CONFIG || { version: "dev", build: null };

// Static DOM references. The app shell is intentionally server-rendered HTML,
// with this file attaching behavior and replacing content as API state arrives.
const versionEl = document.querySelector("#version");
const pwaStateEl = document.querySelector("#pwa-state");
const workerStateEl = document.querySelector("#worker-state");
const swStateEl = document.querySelector("#sw-state");
const pushStateEl = document.querySelector("#push-state");
const knownNodesEl = document.querySelector("#known-nodes");
const healthyNodesEl = document.querySelector("#healthy-nodes");
const nodesEl = document.querySelector("#nodes");
const updateBannerEl = document.querySelector("#update-banner");
const updateMessageEl = document.querySelector("#update-message");
const mandatoryUpdateEl = document.querySelector("#mandatory-update");
const vapidConfiguredEl = document.querySelector("#vapid-configured");
const vapidFormEl = document.querySelector("#vapid-form");
const pushCountEl = document.querySelector("#push-count");
const pushSubscriptionsEl = document.querySelector("#push-subscriptions");
const securityFormEl = document.querySelector("#security-form");
const adminMessageEl = document.querySelector("#admin-message");
const eventsListEl = document.querySelector("#events-list");
const auditListEl = document.querySelector("#audit-list");
const nodeSettingsModalEl = document.querySelector("#node-settings-modal");
const nodeSettingsTitleEl = document.querySelector("#node-settings-title");
const nodeStatusTitleEl = document.querySelector("#node-status-title");
const nodeStatusDetailsEl = document.querySelector("#node-status-details");
const nodeConfigFormEl = document.querySelector("#node-config-form");
const nodeMacHistogramPanelEl = document.querySelector("#node-mac-histogram-panel");
const nodeMacHistogramEl = document.querySelector("#node-mac-histogram");
const nodeSourceMacDiagnosticsEl = document.querySelector("#node-source-mac-diagnostics");
const nodeCalibrationFormEl = document.querySelector("#node-calibration-form");
const nodeCalibrationPanelEl = document.querySelector("#node-calibration-panel");
const nodeCapturePanelEl = document.querySelector("#node-capture-panel");
const nodeCaptureFormEl = document.querySelector("#node-capture-form");
const nodeCaptureMessageEl = document.querySelector("#node-capture-message");
const capturesListEl = document.querySelector("#captures-list");
const nodeSettingsMessageEl = document.querySelector("#node-settings-message");
const historyFromEl = document.querySelector("#history-from-hours");
const historyToEl = document.querySelector("#history-to-hours");
const historyFromLabelEl = document.querySelector("#history-from-label");
const historyToLabelEl = document.querySelector("#history-to-label");
const historyRangeLabelEl = document.querySelector("#history-range-label");
const triggerLevelEl = document.querySelector("#trigger-level");
const triggerLevelLabelEl = document.querySelector("#trigger-level-label");
const triggerEnabledEl = document.querySelector("#trigger-enabled");
const aggregateChartEl = document.querySelector("#aggregate-chart");
const comparisonChartEl = document.querySelector("#comparison-chart");
const comparisonLegendEl = document.querySelector("#comparison-legend");
const comparisonScoreEl = document.querySelector("#comparison-score");
const nodeChartsEl = document.querySelector("#node-charts");

// Labels and tooltip text used by renderers. Keeping this copy in one place
// avoids scattering explanatory text through DOM-construction code.
const VERSION_KEY = "esp32-alarm:last-seen-version";
const DEFAULT_HISTORY_HOURS = 0.5;
const MAX_HISTORY_HOURS = 24;
const COMPARISON_COLORS = ["#2f6fed", "#d77b1f", "#1f8a5b", "#8b4dc7", "#c43b57", "#007c89"];
const SECURITY_LABELS = {
  cloudflareAccessExpected: {
    label: "Cloudflare Access expected",
    help: "Marks the public site as protected by Cloudflare Access before traffic reaches the Pi."
  },
  appSessionsEnabled: {
    label: "App sessions enabled",
    help: "Intended to require a local app login after Cloudflare admits the browser. Stored as a setting flag until login enforcement is added."
  },
  csrfProtectionEnabled: {
    label: "CSRF protection enabled",
    help: "Intended to require anti-forgery tokens for browser actions that change alarm or admin state."
  },
  loginRateLimitEnabled: {
    label: "Login rate limiting enabled",
    help: "Intended to slow or block repeated failed login attempts once app login is active."
  },
  auditLoggingEnabled: {
    label: "Audit logging enabled",
    help: "Records security-sensitive actions such as settings changes, backups, VAPID changes, and future login events."
  },
  localRecoveryOnly: {
    label: "Local recovery only",
    help: "Keeps owner recovery/setup flows limited to local or physical access rather than exposing them through the public hostname."
  }
};

const NODE_STATUS_HELP = {
  role: "Firmware role reported by the ESP32: receiver or dedicated CSI sender.",
  board_variant: "Receiver hardware variant reported by the ESP32 firmware.",
  hardware_profile: "Firmware capability profile compiled for this receiver build.",
  ip: "Current LAN IP address reported by the ESP32.",
  sta_mac: "Station MAC address of the ESP32. Receivers use the sender MAC for source filtering.",
  enabled: "Whether the dedicated sender is currently emitting broadcast stimulus packets.",
  sta_connected: "Whether the ESP32 station interface is connected to Wi-Fi.",
  wifi_provisioned: "Whether Wi-Fi credentials have been saved on this ESP32.",
  packet_rate_hz: "Configured sender UDP broadcast packet cadence.",
  udp_port: "UDP destination port used by sender stimulus packets.",
  payload_size: "Number of bytes in each sender stimulus packet.",
  broadcast_ip: "Broadcast address used by the dedicated sender.",
  packets_sent: "Total stimulus packets sent since boot.",
  send_errors: "Sender packet send failures since boot.",
  last_send_ms: "Sender uptime in milliseconds when the last packet was sent.",
  channel: "Current 2.4 GHz Wi-Fi primary channel.",
  secondary_channel: "Current Wi-Fi secondary channel reported by ESP-IDF.",
  uptime_ms: "Milliseconds since this ESP32 booted.",
  sensing_state: "Current firmware sensing mode: idle, boost, or cooldown.",
  sample_rate_hz: "Current target CSI sampling rate selected by the ESP32 sensing state.",
  accepted_csi_rate_hz: "CSI packets accepted into feature processing per second after filtering/throttling.",
  movement_score: "Fused movement score from CSI feature changes. Higher values mean stronger movement evidence.",
  movement_detected: "Final firmware movement flag after confirmation windows and state logic.",
  baseline_noise: "Current estimate of still-room CSI noise used to judge movement against the baseline.",
  confirm_windows: "Recent feature windows above the movement threshold.",
  quiet_windows: "Recent feature windows below the settle threshold.",
  rssi: "Received signal strength for the Wi-Fi packets feeding CSI.",
  noise_floor: "Radio noise floor reported by the ESP32 Wi-Fi stack.",
  packet_count: "Total CSI packet count seen by the node since boot.",
  last_packet_ms: "Milliseconds since the last accepted packet.",
  csi_source_mac: "Configured dedicated sender MAC used for CSI source filtering.",
  csi_source_filter_enabled: "Whether this receiver ignores CSI frames from other source MACs.",
  last_csi_mac: "Raw most recent MAC address reported by ESP-IDF before source filtering. This can be a non-source MAC even when filtering is enabled.",
  last_filtered_csi_mac: "Most recent CSI MAC rejected because it did not match the configured source filter.",
  last_accepted_csi_mac: "Most recent CSI MAC that passed filtering, throttling, quality checks, and queue handoff into feature processing.",
  source_filtered_samples: "CSI frames ignored because they did not come from the configured sender MAC.",
  calibrating: "Whether the node is currently recording a quiet baseline.",
  capture_active: "Whether this receiver is currently streaming a bounded CSI capture to the Pi.",
  capture_records: "Capture records queued by the receiver during the current or most recent capture.",
  capture_drops: "Capture records dropped because the capture queue was full."
};

const NODE_CONFIG_HELP = {
  device_id: "Logical ESP32 node number. Keep each installed node unique.",
  name: "Display and hostname-style label for this ESP32 node.",
  pi_ip: "LAN IP address of the Raspberry Pi telemetry receiver.",
  pi_port: "Port on the Pi worker that accepts ESP32 telemetry posts.",
  pi_api_path: "HTTP path on the Pi worker for telemetry, normally /espdata.",
  sta_mac: "Read-only sender station MAC. Copy this to receiver CSI sender MAC fields when source filtering is used.",
  csi_source_mac: "Optional CSI source MAC to keep. Use receiver status diagnostics to confirm which MAC ESP-IDF actually reports before enabling filtering.",
  csi_source_filter_enabled: "Only score CSI frames from the configured sender MAC.",
  enabled: "Sender-only: turn steady CSI stimulus packets on or off.",
  packet_rate_hz: "Sender-only: target UDP broadcast packet cadence.",
  udp_port: "Sender-only: UDP destination port used by broadcast stimulus packets.",
  payload_size: "Sender-only: number of bytes in each stimulus packet.",
  broadcast_ip: "Sender-only: broadcast address for direct over-air packet stimulus.",
  pi_post_interval_s: "How often this ESP32 posts compact telemetry to the Pi.",
  idle_rate_hz: "Low-rate CSI sampling target used during quiet monitoring.",
  boost_rate_hz: "Higher CSI sampling target used while movement evidence is active.",
  movement_threshold: "Movement score level that marks a feature window as movement evidence.",
  settle_threshold: "Score level considered quiet enough for cooldown/return-to-idle logic.",
  motion_sensitivity: "Multiplier applied to the fused movement score before threshold comparison.",
  boost_duration_s: "How long the node stays in boosted sensing after movement evidence.",
  cooldown_s: "How long the node waits for quiet windows before returning to idle.",
  feature_window_ms: "Feature aggregation window length used when scoring CSI changes."
};

const NODE_CALIBRATION_HELP = {
  valid: "Whether this node has a persisted stillness baseline saved in ESP32 NVS.",
  calibration_windows: "Number of feature windows averaged when the baseline was captured.",
  baseline_energy: "Still-room average CSI energy used as the primary movement reference.",
  baseline_variance: "Still-room CSI energy variance used to derive normal noise.",
  baseline_shape: "Still-room subcarrier shape variation reference.",
  baseline_phase: "Still-room phase-proxy reference.",
  baseline_phase_variance: "Still-room phase-proxy variance reference.",
  baseline_noise: "Minimum energy deviation divisor. Higher values make the baseline more forgiving.",
  baseline_phase_noise: "Minimum phase deviation divisor. Higher values make phase scoring more forgiving."
};

const NODE_ACTION_HELP = {
  refreshNodeStatus: "Fetch the latest live status directly from this ESP32 node through the Pi.",
  calibrateNode: "Ask the ESP32 to record a fresh quiet baseline. Keep the area still while it runs.",
  identifyNode: "Ask this ESP32 node to run its board-specific visual identify pattern.",
  deleteNodeCalibration: "Clear the node's saved baseline so it can learn a new one.",
  startNodeCapture: "Start a bounded CSI capture session and stream records to the Pi.",
  stopNodeCapture: "Stop the current CSI capture session early.",
  refreshCaptures: "Refresh the Pi-side list of downloadable CSI capture files.",
  saveNodeConfig: "Send these configuration values to the ESP32 node's /api/config endpoint.",
  saveNodeCalibration: "Persist these calibration baseline values to the ESP32 node's NVS.",
  reloadNodeCalibration: "Reload the calibration baseline currently saved on the ESP32 node."
};

let currentAdmin = null;
let pendingHostVersion = null;
let lastStatusNodes = [];
let selectedNodeDeviceId = null;
let selectedNodeRole = "csi_receiver";
let movementHistory = { range: { fromHours: DEFAULT_HISTORY_HOURS, toHours: 0, availableHours: 0 }, samples: [] };
let movementTrigger = { threshold: 3, enabled: true };
let historyWindow = { fromHours: DEFAULT_HISTORY_HOURS, toHours: 0 };

/** True while a node friendly-name input owns focus and must not be re-rendered. */
function isEditingNodeName() {
  return document.activeElement?.classList?.contains("node-name-input");
}

/**
 * Converts thrown fetch/API errors into messages that make sense to the user.
 * The "Not Found" branch is especially helpful after backend changes, when a
 * browser may still be pointed at an older running server process.
 */
function apiUnavailableMessage(error) {
  if (String(error?.message || "").includes("Not Found")) {
    return "Admin API unavailable. Rebuild and restart the PWA server so the new routes are active.";
  }
  return error?.message || "Request failed.";
}

/**
 * Runs a top-level UI action and reports success/failure in the main admin
 * message area. Use this for actions outside the node settings modal.
 */
async function runAction(action, successMessage) {
  try {
    const result = await action();
    if (successMessage) {
      setMessage(typeof successMessage === "function" ? successMessage(result) : successMessage);
    }
    return result;
  } catch (error) {
    setMessage(apiUnavailableMessage(error));
    return null;
  }
}

/** Appends a text-only element and returns it for optional further decoration. */
function appendText(parent, tagName, text, className) {
  const element = document.createElement(tagName);
  element.textContent = text;
  if (className) {
    element.className = className;
  }
  parent.appendChild(element);
  return element;
}

/** Safely coerces API values into finite numbers for charts and labels. */
function asNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

/** Formats ESP32 movement scores consistently across tiles and charts. */
function formatScore(value) {
  const number = asNumber(value);
  return number === null ? "n/a" : number.toFixed(3);
}

/** Formats range-slider hour values without noisy trailing zeroes. */
function formatHours(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return "0h";
  if (number > 0 && number < 1) return `${Math.round(number * 60)}m`;
  return Number.isInteger(number) ? `${number}h` : `${number.toFixed(2).replace(/0+$/, "").replace(/\.$/, "")}h`;
}

/** Formats a node age into a compact, friendly "last seen" label. */
function formatLastSeenAge(seconds) {
  const age = asNumber(seconds);
  if (age === null) return "unknown";
  if (age < 5) return "just now";
  if (age < 60) return `${Math.round(age)}s ago`;
  if (age < 3600) return `${Math.round(age / 60)}m ago`;
  if (age < 86400) return `${Math.round(age / 3600)}h ago`;
  return `${Math.round(age / 86400)}d ago`;
}

/** Truncates form decimals before sending them to the ESP32 firmware. */
function truncateToDecimals(value, decimals) {
  const number = asNumber(value);
  if (number === null) return null;
  const factor = 10 ** decimals;
  return Math.trunc((number + Number.EPSILON) * factor) / factor;
}

/** Truncates and formats a decimal for stable form display. */
function formatTruncated(value, decimals) {
  const number = truncateToDecimals(value, decimals);
  return number === null ? "" : number.toFixed(decimals);
}

/** Adds the small hover/focus help marker used throughout settings forms. */
function appendHelp(parent, text) {
  const help = document.createElement("span");
  help.className = "hover-help";
  help.tabIndex = 0;
  help.setAttribute("aria-label", text);
  help.dataset.tooltip = text;
  help.textContent = "?";
  parent.appendChild(help);
  return help;
}

/** Parses SQLite timestamp strings as UTC so chart windows align across clients. */
function parseSampleTime(value) {
  if (!value) return Date.now();
  const normalized = String(value).includes("T") ? String(value) : String(value).replace(" ", "T");
  const timestamp = Date.parse(normalized.endsWith("Z") ? normalized : `${normalized}Z`);
  return Number.isFinite(timestamp) ? timestamp : Date.now();
}

/** Infers whether a node card should be highlighted as currently seeing movement. */
function nodeHasMovement(payload) {
  const flags = [
    payload.movement_detected,
    payload.motion_detected,
    payload.movement,
    payload.motion,
    payload.detected,
    payload.alarm_triggered
  ];
  if (flags.some((flag) => flag === true || flag === 1 || flag === "true" || flag === "yes")) {
    return true;
  }

  const score = asNumber(payload.movement_score);
  const threshold = asNumber(payload.movement_threshold ?? payload.threshold);
  return score !== null && threshold !== null && score >= threshold;
}

/**
 * Fetches JSON from the Pi API and rejects non-JSON responses.
 *
 * Cloudflare Access or an offline fallback may return HTML; treating that as an
 * error keeps the dashboard from trying to parse a login page as alarm data.
 */
async function readJson(url, options = {}) {
  const response = await fetch(url, {
    cache: "no-store",
    headers: { accept: "application/json", "content-type": "application/json", ...(options.headers || {}) },
    ...options
  });
  const contentType = response.headers.get("content-type") || "";
  if (!contentType.includes("application/json")) {
    throw new Error("Expected JSON but received a non-JSON response");
  }
  const payload = await response.json();
  if (!response.ok) {
    throw new Error(payload.detail ? `${payload.error}: ${payload.detail}` : payload.error || `Request failed with ${response.status}`);
  }
  return payload;
}

/** Convenience wrapper for JSON POST requests. */
function postJson(url, body = {}) {
  return readJson(url, { method: "POST", body: JSON.stringify(body) });
}

/** Convenience wrapper for JSON PUT requests. */
function putJson(url, body = {}) {
  return readJson(url, { method: "PUT", body: JSON.stringify(body) });
}

/** Writes a main-page status message. */
function setMessage(message) {
  adminMessageEl.textContent = message;
}

/** Writes a node-settings-modal status message. */
function setNodeSettingsMessage(message) {
  nodeSettingsMessageEl.textContent = message;
}

function setCalibrationControlsDisabled(disabled) {
  for (const element of [
    ...nodeCalibrationFormEl.elements,
    document.querySelector("#calibrate-node"),
    document.querySelector("#delete-node-calibration"),
    document.querySelector("#reload-node-calibration")
  ]) {
    if (element) {
      element.disabled = disabled;
    }
  }
}

/** Shows only controls that make sense for the selected ESP32 role. */
function setNodeSettingsRole(role) {
  const isSender = role === "csi_sender";
  nodeStatusTitleEl.textContent = isSender ? "Sender Status" : "Receiver Status";
  nodeCalibrationPanelEl.classList.toggle("hidden", isSender);
  nodeMacHistogramPanelEl.classList.toggle("hidden", isSender);
  nodeCapturePanelEl.classList.toggle("hidden", isSender);
  for (const label of nodeConfigFormEl.querySelectorAll("[data-config-role]")) {
    const targetRole = label.dataset.configRole;
    label.classList.toggle("hidden", targetRole !== "all" && targetRole !== (isSender ? "sender" : "receiver"));
  }
}

/** Runs an action scoped to the node settings modal. */
async function runNodeAction(action, successMessage) {
  try {
    const result = await action();
    if (successMessage) {
      setNodeSettingsMessage(typeof successMessage === "function" ? successMessage(result) : successMessage);
    }
    return result;
  } catch (error) {
    setNodeSettingsMessage(apiUnavailableMessage(error));
    return null;
  }
}

/** Finds the persisted Pi registry row that corresponds to an ESP32 device_id. */
function nodeRecordFor(deviceId) {
  return currentAdmin?.nodes?.find((node) => Number(node.deviceId) === Number(deviceId)) || null;
}

/** Normalizes MAC text so histogram entries can be matched to known node data. */
function normalizeMac(value) {
  const text = String(value ?? "").trim().toUpperCase().replaceAll("-", ":");
  return /^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(text) ? text : "";
}

/** Builds a MAC-to-node lookup from live telemetry and the Pi's persisted registry. */
function knownMacDetails() {
  const details = new Map();

  const upsert = (mac, detail) => {
    const normalized = normalizeMac(mac);
    if (!normalized) return;
    const existing = details.get(normalized);
    if (!existing || (detail.match === "station" && existing.match !== "station") || (!existing.ip && detail.ip)) {
      details.set(normalized, detail);
    }
  };

  for (const [mac, detail] of Object.entries(currentAdmin?.macIdentities || {})) {
    upsert(mac, {
      name: detail.friendlyName,
      ip: detail.ip,
      role: detail.role,
      match: detail.confidence === "known" ? "station" : detail.confidence,
      source: detail.source,
      notes: detail.notes || []
    });
  }

  const addNode = (node, deviceIdKey) => {
    const deviceId = Number(node?.[deviceIdKey]);
    const registryNode = nodeRecordFor(deviceId);
    const payload = node?.payload || {};
    const name = String(registryNode?.name || node?.name || `Node ${Number.isFinite(deviceId) ? deviceId : ""}`).trim();
    const ip = String(node?.ip || registryNode?.ip || "").trim();
    const role = payload.role === "csi_sender" ? "sender" : "node";

    upsert(payload.sta_mac, { name, ip, role, match: "station", source: "live telemetry", notes: ["Station MAC reported by this ESP32 node."] });
    upsert(payload.csi_source_mac, { name: `Configured for ${name}`, ip: "", role: "source", match: "configured", source: "receiver config", notes: [`Configured as the CSI source on ${name}.`] });
  };

  for (const node of currentAdmin?.nodes ?? []) addNode(node, "deviceId");
  for (const node of lastStatusNodes) addNode(node, "device_id");
  return details;
}

/** Selects or clears the receiver CSI source from a histogram row checkbox. */
async function setHistogramSourceMac(mac, enabled) {
  if (selectedNodeDeviceId === null || selectedNodeRole !== "csi_receiver") return;
  const selectedMac = normalizeMac(mac);
  if (enabled && !selectedMac) return;

  const payload = {
    csi_source_mac: enabled ? selectedMac : "",
    csi_source_filter_enabled: enabled
  };
  if (nodeConfigFormEl.elements.csi_source_mac) {
    nodeConfigFormEl.elements.csi_source_mac.value = payload.csi_source_mac;
  }
  if (nodeConfigFormEl.elements.csi_source_filter_enabled) {
    nodeConfigFormEl.elements.csi_source_filter_enabled.checked = payload.csi_source_filter_enabled;
  }

  await runNodeAction(async () => {
    const saved = await postJson(`/api/nodes/${selectedNodeDeviceId}/config`, payload);
    populateNodeConfigForm(saved);
    await refreshSelectedNodeStatus();
    await refreshStatus();
  }, enabled ? `CSI source set to ${selectedMac}.` : "CSI source filter disabled.");
}

/**
 * Renders the ESP32 Nodes card from live worker telemetry plus Pi registry data.
 *
 * The Pi registry supplies user-edited names and active flags; live telemetry
 * supplies IP, state, score, and device_id. Settings and Identify buttons call
 * Pi proxy routes, not ESP32 devices directly.
 */
function renderNodes(nodes) {
  lastStatusNodes = nodes;
  if (isEditingNodeName()) return;

  if (!nodes.length) {
    nodesEl.className = "nodes empty";
    nodesEl.textContent = "No node telemetry yet.";
    return;
  }

  nodesEl.className = "nodes";
  nodesEl.replaceChildren(
    ...nodes.map((node) => {
      const item = document.createElement("div");
      const payload = node.payload || {};
      const isSender = payload.role === "csi_sender";
      const registryNode = nodeRecordFor(node.device_id);
      item.className = nodeHasMovement(payload) ? "node movement" : "node";

      const nameInput = document.createElement("input");
      nameInput.className = "node-name-input";
      nameInput.value = String(registryNode?.name || node.name || "Unnamed node");
      nameInput.dataset.nodeRecordId = registryNode ? String(registryNode.id) : "";
      nameInput.dataset.deviceId = String(node.device_id);
      nameInput.disabled = !registryNode;
      nameInput.title = "Friendly display name for this ESP32 node.";
      item.appendChild(nameInput);

      const nodeLink = document.createElement("a");
      nodeLink.className = "node-link";
      nodeLink.href = node.ip ? `http://${node.ip}` : "#";
      nodeLink.target = "_blank";
      nodeLink.rel = "noopener noreferrer";
      const stateText = node.state === "stale" ? `stale · last seen ${formatLastSeenAge(node.last_seen_age_s)}` : node.state;
      nodeLink.textContent = `ID ${node.device_id} · ${stateText}`;
      nodeLink.title = node.ip ? `Open this ESP32 node at http://${node.ip}` : `Node IP unavailable; last seen ${formatLastSeenAge(node.last_seen_age_s)}`;
      if (!node.ip) {
        nodeLink.addEventListener("click", (event) => event.preventDefault());
      }
      item.appendChild(nodeLink);
      appendText(item, "span", String(node.ip ?? ""));
      appendText(item, "span", isSender ? `Sender ${payload.enabled ? "on" : "off"}` : `Score ${formatScore(payload.movement_score)}`);

      const activeLabel = document.createElement("label");
      activeLabel.className = "node-active";
      const active = document.createElement("input");
      active.type = "checkbox";
      active.checked = registryNode ? registryNode.active : node.state === "online";
      active.disabled = !registryNode;
      active.dataset.nodeRecordId = registryNode ? String(registryNode.id) : "";
      active.title = "Include or exclude this ESP32 node from active monitoring decisions in the Pi dashboard.";
      activeLabel.appendChild(active);
      activeLabel.append(" Active");
      item.appendChild(activeLabel);

      if (isSender) {
        const toggle = document.createElement("button");
        toggle.type = "button";
        toggle.className = "node-sender-toggle-button";
        toggle.dataset.senderDeviceId = String(node.device_id);
        toggle.dataset.senderEnabled = payload.enabled ? "true" : "false";
        toggle.textContent = payload.enabled ? "Stop Sender" : "Start Sender";
        toggle.title = "Turn the dedicated CSI packet sender on or off.";
        item.appendChild(toggle);
      } else {
        const identify = document.createElement("button");
        identify.type = "button";
        identify.className = "node-identify-button";
        identify.dataset.identifyDeviceId = String(node.device_id);
        identify.textContent = "Identify";
        identify.title = "Run this ESP32 node's visual identify pattern.";
        item.appendChild(identify);
      }

      const settings = document.createElement("button");
      settings.type = "button";
      settings.className = "node-settings-button";
      settings.dataset.deviceId = String(node.device_id);
      settings.textContent = "Settings";
      settings.title = "Open this ESP32 node's live status, calibration, and configuration controls.";
      item.appendChild(settings);
      return item;
    })
  );
}

function showUpdateBanner(version, mandatory) {
  pendingHostVersion = version;
  updateMessageEl.textContent = mandatory ? `Version ${version} is required` : `Version ${version} is available`;
  updateBannerEl.classList.remove("hidden");
  mandatoryUpdateEl.classList.toggle("hidden", !mandatory);
}

/**
 * Forces a full app-payload refresh after a version change.
 *
 * This unregisters older service workers and removes old versioned caches before
 * reloading, which is more reliable for installed PWAs than a normal page reload.
 */
async function refreshAppPayload() {
  try {
    const registrations = await navigator.serviceWorker?.getRegistrations?.();
    if (registrations) {
      await Promise.all(registrations.map((registration) => registration.unregister()));
    }
  } catch (error) {
    console.warn("Unable to unregister old service workers", error);
  }
  try {
    if ("caches" in window) {
      const keys = await caches.keys();
      await Promise.all(keys.filter((key) => key.startsWith("esp32-alarm-")).map((key) => caches.delete(key)));
    }
  } catch (error) {
    console.warn("Unable to clear old app caches", error);
  }
  if (pendingHostVersion) {
    localStorage.setItem(VERSION_KEY, pendingHostVersion);
  } else {
    localStorage.removeItem(VERSION_KEY);
  }
  window.location.replace(`${window.location.pathname}?refresh=${Date.now()}${window.location.hash}`);
}

/** Checks the host version and shows an update banner if this client is stale. */
async function checkVersion() {
  const version = await readJson("/api/version");
  versionEl.textContent = version.version;
  pwaStateEl.textContent = "online";
  const lastSeen = localStorage.getItem(VERSION_KEY);
  if (!lastSeen) {
    localStorage.setItem(VERSION_KEY, version.version);
  } else if (lastSeen !== version.version) {
    showUpdateBanner(version.version, Boolean(version.mandatory));
  }
  return version;
}

/** Refreshes the live system status tiles and node list from /api/status. */
async function refreshStatus() {
  try {
    await checkVersion();
  } catch {
    pwaStateEl.textContent = "error";
  }

  try {
    const status = await readJson("/api/status");
    workerStateEl.textContent = "online";
    knownNodesEl.textContent = String(status.known_nodes ?? 0);
    healthyNodesEl.textContent = String(status.healthy_nodes ?? 0);
    renderNodes(status.nodes ?? []);
  } catch {
    workerStateEl.textContent = "offline";
    knownNodesEl.textContent = "0";
    healthyNodesEl.textContent = "0";
    if (!isEditingNodeName()) {
      renderNodes([]);
    }
  }
}

/** Renders VAPID and push subscription admin state. */
function renderPush(admin) {
  vapidConfiguredEl.textContent = admin.vapid.privateKeyConfigured ? "yes" : "no";
  vapidFormEl.elements.subject.value = admin.vapid.subject || "";
  vapidFormEl.elements.publicKey.value = admin.vapid.publicKey || "";
  vapidFormEl.elements.privateKey.value = admin.vapid.privateKey || "";
  pushCountEl.textContent = String(admin.pushSubscriptions.length);
  pushSubscriptionsEl.replaceChildren(
    ...admin.pushSubscriptions.map((subscription) => {
      const item = document.createElement("div");
      item.className = "record";
      appendText(item, "strong", subscription.deviceName);
      appendText(item, "span", `${subscription.enabled ? "enabled" : "disabled"} · failures ${subscription.failureCount}`);
      appendText(item, "small", subscription.endpoint);
      return item;
    })
  );
}

/** Renders persisted security posture flags into editable checkboxes. */
function renderSecurity(settings) {
  securityFormEl.replaceChildren(
    ...Object.entries(SECURITY_LABELS).map(([key, option]) => {
      const row = document.createElement("label");
      const checkbox = document.createElement("input");
      checkbox.type = "checkbox";
      checkbox.name = key;
      checkbox.checked = Boolean(settings[key]);
      row.appendChild(checkbox);
      row.append(` ${option.label}`);

      appendHelp(row, option.help);
      return row;
    })
  );
}

/** Attaches help text to node settings fields and modal action buttons. */
function attachNodeSettingsHelp() {
  for (const [name, help] of Object.entries(NODE_CONFIG_HELP)) {
    const input = nodeConfigFormEl.elements[name];
    const label = input?.closest("label");
    if (label && !label.querySelector(".hover-help")) {
      appendHelp(label, help);
      input.title = help;
    }
  }

  for (const [name, help] of Object.entries(NODE_CALIBRATION_HELP)) {
    const input = nodeCalibrationFormEl.elements[name];
    const label = input?.closest("label");
    if (label && !label.querySelector(".hover-help")) {
      appendHelp(label, help);
      input.title = help;
    }
  }

  const actions = [
    ["#close-node-settings", "Close this ESP32 node settings window."],
    ["#refresh-node-status", NODE_ACTION_HELP.refreshNodeStatus],
    ["#calibrate-node", NODE_ACTION_HELP.calibrateNode],
    ["#delete-node-calibration", NODE_ACTION_HELP.deleteNodeCalibration],
    ['#node-capture-form button[type="submit"]', NODE_ACTION_HELP.startNodeCapture],
    ["#stop-node-capture", NODE_ACTION_HELP.stopNodeCapture],
    ["#refresh-captures", NODE_ACTION_HELP.refreshCaptures],
    ['#node-config-form button[type="submit"]', NODE_ACTION_HELP.saveNodeConfig],
    ['#node-calibration-form button[type="submit"]', NODE_ACTION_HELP.saveNodeCalibration],
    ["#reload-node-calibration", NODE_ACTION_HELP.reloadNodeCalibration]
  ];
  for (const [selector, help] of actions) {
    const button = document.querySelector(selector);
    if (button) {
      button.title = help;
      button.setAttribute("aria-label", help);
    }
  }
}

/** Renders simple record lists such as events and audit log entries. */
function renderRecords(target, records, emptyText) {
  if (!records.length) {
    target.textContent = emptyText;
    return;
  }
  target.replaceChildren(
    ...records.map((record) => {
      const item = document.createElement("div");
      item.className = "record";
      appendText(item, "strong", record.title || record.action);
      appendText(item, "span", record.type || record.target || "");
      appendText(item, "small", record.createdAt);
      return item;
    })
  );
}

function formatBytes(value) {
  const bytes = asNumber(value);
  if (bytes === null) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function renderCaptures(captures) {
  const selected = captures.filter((capture) => Number(capture.device_id) === Number(selectedNodeDeviceId));
  if (!selected.length) {
    capturesListEl.textContent = "No captures for this receiver yet.";
    return;
  }

  capturesListEl.replaceChildren(
    ...selected.map((capture) => {
      const item = document.createElement("div");
      item.className = "record";
      appendText(item, "strong", capture.label ? `${capture.label} (${capture.mode})` : `${capture.capture_id} (${capture.mode})`);
      appendText(
        item,
        "span",
        `${capture.records ?? 0} records · ${formatBytes(capture.data_bytes)} · ${capture.finished ? "complete" : "receiving"}`
      );
      appendText(item, "small", capture.first_received_at || capture.capture_id);
      const actions = document.createElement("div");
      actions.className = "actions";
      const dataLink = document.createElement("a");
      dataLink.className = "node-link";
      dataLink.href = capture.data_download_url;
      dataLink.textContent = "Download data";
      const metaLink = document.createElement("a");
      metaLink.className = "node-link";
      metaLink.href = capture.metadata_download_url;
      metaLink.textContent = "Metadata";
      actions.append(dataLink, metaLink);
      item.appendChild(actions);
      return item;
    })
  );
}

async function refreshCaptures() {
  const payload = await readJson("/api/captures");
  renderCaptures(payload.captures || []);
  return payload;
}

function renderMacHistogram(status) {
  const histogram = Array.isArray(status?.csi_mac_histogram) ? status.csi_mac_histogram : [];
  const entries = histogram
    .filter((entry) => entry?.mac)
    .slice()
    .sort((a, b) => Number(b.count || 0) - Number(a.count || 0));
  if (!entries.length) {
    nodeMacHistogramEl.className = "mac-histogram empty";
    nodeMacHistogramEl.textContent = "No CSI MACs observed yet.";
    return;
  }

  const maxCount = Math.max(...entries.map((entry) => Number(entry.count || 0)), 1);
  const macDetails = knownMacDetails();
  const activeSourceMac = status?.csi_source_filter_enabled ? normalizeMac(status?.csi_source_mac || status?.csi_source_mac_diagnostics?.mac) : "";
  nodeMacHistogramEl.className = "mac-histogram";
  nodeMacHistogramEl.replaceChildren(
    ...entries.map((entry) => {
      const entryMac = normalizeMac(entry.mac);
      const row = document.createElement("div");
      row.className = "mac-histogram-row";
      const sourceInput = document.createElement("input");
      sourceInput.type = "checkbox";
      sourceInput.className = "mac-source-checkbox";
      sourceInput.checked = Boolean(entryMac && entryMac === activeSourceMac);
      sourceInput.title = sourceInput.checked ? "This MAC is the active CSI source." : "Use this MAC as the CSI source.";
      sourceInput.addEventListener("change", () => setHistogramSourceMac(entry.mac, sourceInput.checked));
      const label = document.createElement("div");
      label.className = "mac-histogram-label";
      appendText(label, "span", String(entry.mac));
      const detail = macDetails.get(normalizeMac(entry.mac));
      if (detail) {
        const detailParts = [
          detail.role,
          detail.name,
          detail.ip,
          detail.source,
          detail.match && detail.match !== "station" ? detail.match : ""
        ].filter(Boolean);
        appendText(label, "small", detailParts.join(" · "), "mac-histogram-device");
        row.title = [...detailParts, ...(detail.notes || [])].filter(Boolean).join("\n");
      }
      const track = document.createElement("div");
      track.className = "mac-histogram-track";
      const bar = document.createElement("div");
      bar.className = "mac-histogram-bar";
      bar.style.width = `${Math.max(4, (Number(entry.count || 0) / maxCount) * 100)}%`;
      track.appendChild(bar);
      row.appendChild(sourceInput);
      appendText(row, "strong", String(entry.count ?? 0));
      row.append(label, track);
      appendText(row, "small", `${entry.last_seen_ms ?? "n/a"} ms ago`);
      return row;
    })
  );
}

function renderSourceMacDiagnostics(status) {
  const diagnostics = status?.csi_source_mac_diagnostics;
  if (!diagnostics) {
    nodeSourceMacDiagnosticsEl.className = "source-mac-diagnostics";
    nodeSourceMacDiagnosticsEl.replaceChildren();
    return;
  }

  const items = [
    ["Configured source", diagnostics.mac || "unset"],
    ["Filter", diagnostics.filter_enabled ? "enabled" : "disabled"],
    ["Seen before filter", String(diagnostics.seen_before_filter ?? 0)],
    ["Accepted after gates", String(diagnostics.accepted_after_gates ?? 0)],
    ["Last seen", diagnostics.last_seen_ms === null || diagnostics.last_seen_ms === undefined ? "never" : `${diagnostics.last_seen_ms} ms ago`],
    ["Last accepted", diagnostics.last_accepted_ms === null || diagnostics.last_accepted_ms === undefined ? "never" : `${diagnostics.last_accepted_ms} ms ago`]
  ];

  nodeSourceMacDiagnosticsEl.className = "source-mac-diagnostics";
  nodeSourceMacDiagnosticsEl.replaceChildren(
    ...items.map(([labelText, valueText]) => {
      const item = document.createElement("div");
      item.className = "source-mac-diagnostic";
      appendText(item, "span", labelText);
      appendText(item, "strong", valueText);
      return item;
    })
  );
}

/** Renders the selected ESP32 node's live status fields in the modal. */
function renderNodeStatus(status) {
  const receiverKeys = [
    "role",
    "board_variant",
    "hardware_profile",
    "sensing_state",
    "sample_rate_hz",
    "accepted_csi_rate_hz",
    "movement_score",
    "movement_detected",
    "baseline_noise",
    "confirm_windows",
    "quiet_windows",
    "rssi",
    "noise_floor",
    "csi_source_mac",
    "csi_source_filter_enabled",
    "last_csi_mac",
    "last_filtered_csi_mac",
    "last_accepted_csi_mac",
    "source_filtered_samples",
    "packet_count",
    "last_packet_ms",
    "calibrating",
    "capture_active",
    "capture_records",
    "capture_drops"
  ];
  const senderKeys = [
    "role",
    "sta_mac",
    "ip",
    "enabled",
    "sta_connected",
    "wifi_provisioned",
    "packet_rate_hz",
    "udp_port",
    "payload_size",
    "broadcast_ip",
    "packets_sent",
    "send_errors",
    "last_send_ms",
    "channel",
    "secondary_channel",
    "uptime_ms"
  ];
  const keys = selectedNodeRole === "csi_sender" ? senderKeys : receiverKeys;
  if (selectedNodeRole === "csi_sender" && nodeConfigFormEl.elements.sta_mac) {
    nodeConfigFormEl.elements.sta_mac.value = status?.sta_mac ?? "";
  }
  const displayStatus = {
    ...status,
    capture_active: status?.capture?.active,
    capture_records: status?.capture?.records,
    capture_drops: status?.capture?.drops
  };
  nodeStatusDetailsEl.replaceChildren(
    ...keys.map((key) => {
      const row = document.createElement("div");
      const label = appendText(row, "dt", key);
      appendHelp(label, NODE_STATUS_HELP[key] || "Live status value reported by the ESP32 node.");
      const value = displayStatus?.[key];
      const display = ["movement_score", "baseline_noise"].includes(key) ? formatScore(value) : String(value ?? "n/a");
      appendText(row, "dd", display);
      return row;
    })
  );
  if (selectedNodeRole === "csi_receiver") {
    renderMacHistogram(status);
    renderSourceMacDiagnostics(status);
  }
}

/** Writes a number-ish value into a form field without decimal formatting. */
function setFormNumber(form, name, value) {
  form.elements[name].value = value === undefined || value === null ? "" : String(value);
}

/** Writes a decimal value into a form field with fixed precision. */
function setFormDecimal(form, name, value, decimals) {
  form.elements[name].value = formatTruncated(value, decimals);
}

/** Applies receiver capability limits reported by the firmware config API. */
function applyReceiverCapabilityLimits(config) {
  const idleInput = nodeConfigFormEl.elements.idle_rate_hz;
  const boostInput = nodeConfigFormEl.elements.boost_rate_hz;
  if (idleInput && config?.max_idle_rate_hz) {
    idleInput.max = String(config.max_idle_rate_hz);
  }
  if (boostInput && config?.max_boost_rate_hz) {
    boostInput.max = String(config.max_boost_rate_hz);
  }
}

/** Populates the ESP32 configuration form from the node /api/config response. */
function populateNodeConfigForm(config) {
  attachNodeSettingsHelp();
  applyReceiverCapabilityLimits(config);
  nodeConfigFormEl.elements.device_id.value = config.device_id ?? "";
  nodeConfigFormEl.elements.name.value = config.name ?? "";
  nodeConfigFormEl.elements.sta_mac.value = config.sta_mac ?? "";
  nodeConfigFormEl.elements.pi_ip.value = config.pi_ip ?? "";
  setFormNumber(nodeConfigFormEl, "pi_port", config.pi_port ?? 3005);
  nodeConfigFormEl.elements.pi_api_path.value = config.pi_api_path ?? "/espdata";
  nodeConfigFormEl.elements.csi_source_mac.value = config.csi_source_mac ?? "";
  nodeConfigFormEl.elements.csi_source_filter_enabled.checked = Boolean(config.csi_source_filter_enabled);
  nodeConfigFormEl.elements.enabled.checked = Boolean(config.enabled);
  setFormNumber(nodeConfigFormEl, "packet_rate_hz", config.packet_rate_hz);
  setFormNumber(nodeConfigFormEl, "udp_port", config.udp_port);
  setFormNumber(nodeConfigFormEl, "payload_size", config.payload_size);
  nodeConfigFormEl.elements.broadcast_ip.value = config.broadcast_ip ?? "";
  setFormNumber(nodeConfigFormEl, "pi_post_interval_s", Math.round((Number(config.pi_post_interval_ms) || 5000) / 1000));
  setFormNumber(nodeConfigFormEl, "idle_rate_hz", config.idle_rate_hz);
  setFormNumber(nodeConfigFormEl, "boost_rate_hz", config.boost_rate_hz);
  setFormDecimal(nodeConfigFormEl, "movement_threshold", config.movement_threshold, 2);
  setFormDecimal(nodeConfigFormEl, "settle_threshold", config.settle_threshold, 2);
  setFormNumber(nodeConfigFormEl, "motion_sensitivity", config.motion_sensitivity);
  setFormNumber(nodeConfigFormEl, "boost_duration_s", Math.round((Number(config.boost_duration_ms) || 0) / 1000));
  setFormNumber(nodeConfigFormEl, "cooldown_s", Math.round((Number(config.cooldown_ms) || 0) / 1000));
  setFormNumber(nodeConfigFormEl, "feature_window_ms", config.feature_window_ms);
}

/** Populates persisted calibration fields from the node /api/calibration response. */
function populateNodeCalibrationForm(calibration) {
  attachNodeSettingsHelp();
  nodeCalibrationFormEl.elements.valid.value = calibration.valid ? "yes" : "no";
  setFormNumber(nodeCalibrationFormEl, "calibration_windows", calibration.calibration_windows ?? 0);
  setFormDecimal(nodeCalibrationFormEl, "baseline_energy", calibration.baseline_energy, 6);
  setFormDecimal(nodeCalibrationFormEl, "baseline_variance", calibration.baseline_variance, 6);
  setFormDecimal(nodeCalibrationFormEl, "baseline_shape", calibration.baseline_shape, 6);
  setFormDecimal(nodeCalibrationFormEl, "baseline_phase", calibration.baseline_phase, 6);
  setFormDecimal(nodeCalibrationFormEl, "baseline_phase_variance", calibration.baseline_phase_variance, 6);
  setFormDecimal(nodeCalibrationFormEl, "baseline_noise", calibration.baseline_noise, 6);
  setFormDecimal(nodeCalibrationFormEl, "baseline_phase_noise", calibration.baseline_phase_noise, 6);
}

/** Builds the JSON patch sent to the ESP32 /api/config endpoint. */
function nodeConfigPayload() {
  const form = new FormData(nodeConfigFormEl);
  const apiPath = String(form.get("pi_api_path") || "/espdata").trim();
  const common = {
    device_id: Number(form.get("device_id")),
    name: String(form.get("name") || "").trim(),
    pi_ip: String(form.get("pi_ip") || "").trim(),
    pi_port: Number(form.get("pi_port")),
    pi_api_path: apiPath.startsWith("/") ? apiPath : `/${apiPath}`,
    pi_post_interval_ms: Number(form.get("pi_post_interval_s")) * 1000
  };

  if (selectedNodeRole === "csi_sender") {
    return {
      ...common,
      enabled: form.get("enabled") === "on",
      packet_rate_hz: Number(form.get("packet_rate_hz")),
      udp_port: Number(form.get("udp_port")),
      payload_size: Number(form.get("payload_size")),
      broadcast_ip: String(form.get("broadcast_ip") || "").trim()
    };
  }

  return {
    ...common,
    csi_source_mac: String(form.get("csi_source_mac") || "").trim(),
    csi_source_filter_enabled: form.get("csi_source_filter_enabled") === "on",
    idle_rate_hz: Number(form.get("idle_rate_hz")),
    boost_rate_hz: Number(form.get("boost_rate_hz")),
    movement_threshold: truncateToDecimals(form.get("movement_threshold"), 2),
    settle_threshold: truncateToDecimals(form.get("settle_threshold"), 2),
    motion_sensitivity: Number(form.get("motion_sensitivity")),
    boost_duration_ms: Number(form.get("boost_duration_s")) * 1000,
    cooldown_ms: Number(form.get("cooldown_s")) * 1000,
    feature_window_ms: Number(form.get("feature_window_ms"))
  };
}

/** Builds the JSON patch sent to the ESP32 /api/calibration endpoint. */
function nodeCalibrationPayload() {
  const form = new FormData(nodeCalibrationFormEl);
  return {
    calibration_windows: Number(form.get("calibration_windows")),
    baseline_energy: Number(form.get("baseline_energy")),
    baseline_variance: Number(form.get("baseline_variance")),
    baseline_shape: Number(form.get("baseline_shape")),
    baseline_phase: Number(form.get("baseline_phase")),
    baseline_phase_variance: Number(form.get("baseline_phase_variance")),
    baseline_noise: Number(form.get("baseline_noise")),
    baseline_phase_noise: Number(form.get("baseline_phase_noise"))
  };
}

/** Refreshes only the selected node's live status panel. */
async function refreshSelectedNodeStatus() {
  if (selectedNodeDeviceId === null) return;
  if (selectedNodeRole === "csi_receiver") {
    await refreshAdmin().catch(() => undefined);
  }
  const status = await readJson(`/api/nodes/${selectedNodeDeviceId}/status`);
  renderNodeStatus(status);
}

/** Refreshes only the selected node's persisted calibration panel. */
async function refreshSelectedNodeCalibration() {
  if (selectedNodeDeviceId === null) return null;
  const calibration = await readJson(`/api/nodes/${selectedNodeDeviceId}/calibration`);
  populateNodeCalibrationForm(calibration);
  return calibration;
}

/**
 * Opens the node settings modal and loads status, config, and calibration data.
 * The modal keeps the current `device_id` in module state so button handlers can
 * share one selected-node context.
 */
async function openNodeSettings(deviceId) {
  selectedNodeDeviceId = Number(deviceId);
  const liveNode = lastStatusNodes.find((node) => Number(node.device_id) === selectedNodeDeviceId);
  selectedNodeRole = liveNode?.payload?.role === "csi_sender" ? "csi_sender" : "csi_receiver";
  nodeSettingsTitleEl.textContent = liveNode ? `${liveNode.name} Settings` : `Node ${selectedNodeDeviceId} Settings`;
  setNodeSettingsRole(selectedNodeRole);
  setCalibrationControlsDisabled(selectedNodeRole === "csi_sender");
  if (selectedNodeRole === "csi_sender" && nodeConfigFormEl.elements.sta_mac) {
    nodeConfigFormEl.elements.sta_mac.value = liveNode?.payload?.sta_mac ?? "";
  }
  setNodeSettingsMessage("Loading node settings...");
  nodeSettingsModalEl.classList.remove("hidden");

  const config = await runNodeAction(async () => {
    const payload = await readJson(`/api/nodes/${selectedNodeDeviceId}/config`);
    populateNodeConfigForm(payload);
    return payload;
  });

  await runNodeAction(async () => {
    await refreshSelectedNodeStatus();
  });

  if (selectedNodeRole === "csi_receiver") {
    await runNodeAction(async () => {
      await refreshSelectedNodeCalibration();
    });
    await runNodeAction(async () => {
      await refreshCaptures();
    });
  } else {
    nodeCalibrationFormEl.reset();
    nodeCalibrationFormEl.elements.valid.value = "sender";
    capturesListEl.textContent = "CSI captures are available on receiver nodes.";
  }

  if (config) {
    setNodeSettingsMessage("Settings loaded.");
  }
}

/** Closes and resets the node settings modal. */
function closeNodeSettings() {
  nodeSettingsModalEl.classList.add("hidden");
  selectedNodeDeviceId = null;
  selectedNodeRole = "csi_receiver";
  setNodeSettingsRole(selectedNodeRole);
  setCalibrationControlsDisabled(false);
  nodeStatusDetailsEl.replaceChildren();
  nodeMacHistogramEl.className = "mac-histogram empty";
  nodeMacHistogramEl.textContent = "No CSI MACs observed yet.";
  nodeSourceMacDiagnosticsEl.replaceChildren();
  nodeConfigFormEl.reset();
  nodeCalibrationFormEl.reset();
  nodeCaptureFormEl.reset();
  capturesListEl.replaceChildren();
  nodeCaptureMessageEl.textContent = "";
  setNodeSettingsMessage("");
}

/** Normalizes the dual history sliders into a valid [from, to] hour window. */
function normalizeHistoryWindow() {
  let fromHours = Number(historyFromEl.value);
  let toHours = Number(historyToEl.value);
  const max = Math.min(MAX_HISTORY_HOURS, Number(historyFromEl.max) || MAX_HISTORY_HOURS);
  if (!Number.isFinite(fromHours)) fromHours = Math.min(DEFAULT_HISTORY_HOURS, max);
  if (!Number.isFinite(toHours)) toHours = 0;
  fromHours = Math.min(Math.max(fromHours, 0), max);
  toHours = Math.min(Math.max(toHours, 0), max);
  if (fromHours < toHours) {
    [fromHours, toHours] = [toHours, fromHours];
  }
  historyWindow = { fromHours, toHours };
  historyFromEl.value = String(fromHours);
  historyToEl.value = String(toHours);
  historyFromLabelEl.textContent = formatHours(fromHours);
  historyToLabelEl.textContent = formatHours(toHours);
  historyRangeLabelEl.textContent = `Showing ${formatHours(fromHours)} to ${formatHours(toHours)} before now.`;
}

/**
 * Prepares a canvas for high-DPI drawing without letting device-pixel-ratio
 * scaling inflate the visible chart height on every redraw.
 */
function chartDimensions(canvas) {
  const rect = canvas.getBoundingClientRect();
  const scale = window.devicePixelRatio || 1;
  const width = Math.max(320, Math.floor(rect.width || canvas.parentElement.clientWidth || 640));
  const baseHeight = Number(canvas.dataset.logicalHeight || canvas.getAttribute("height")) || 180;
  const height = Math.max(160, baseHeight);
  canvas.dataset.logicalHeight = String(height);
  canvas.style.height = `${height}px`;
  canvas.width = Math.floor(width * scale);
  canvas.height = Math.floor(height * scale);
  const ctx = canvas.getContext("2d");
  ctx.setTransform(scale, 0, 0, scale, 0, 0);
  return { ctx, width, height };
}

/** Filters and sorts movement samples for the currently selected history window. */
function pointsForSamples(samples, now, fromHours, toHours) {
  const fromTime = now - fromHours * 3600000;
  const toTime = now - toHours * 3600000;
  return samples
    .map((sample) => ({ ...sample, time: parseSampleTime(sample.sampledAt) }))
    .filter((sample) => sample.time >= fromTime && sample.time <= toTime)
    .sort((a, b) => a.time - b.time);
}

/** Collapses all nodes into one aggregate series using the max score per timestamp. */
function aggregateSamples(samples) {
  const buckets = new Map();
  for (const sample of samples) {
    const key = sample.sampledAt;
    const existing = buckets.get(key) || {
      sampledAt: sample.sampledAt,
      score: 0,
      nodeNames: [],
      deviceIds: []
    };
    existing.score = Math.max(existing.score, Number(sample.score) || 0);
    if ((Number(sample.score) || 0) >= movementTrigger.threshold) {
      existing.nodeNames.push(sample.nodeName);
      existing.deviceIds.push(sample.deviceId);
    }
    buckets.set(key, existing);
  }
  return [...buckets.values()];
}

/** Groups samples by node so comparison and per-node charts share ordering. */
function samplesByNode(samples) {
  const byNode = new Map();
  for (const sample of samples) {
    const key = String(sample.deviceId);
    if (!byNode.has(key)) {
      byNode.set(key, { name: sample.nodeName || `Node ${key}`, samples: [] });
    }
    byNode.get(key).samples.push(sample);
  }
  return byNode;
}

/** Converts asynchronous node samples into comparable normalized trend lines. */
function comparisonSeries(samples) {
  const now = Date.now();
  const fromHours = historyWindow.fromHours;
  const toHours = historyWindow.toHours;
  const fromTime = now - fromHours * 3600000;
  const toTime = now - toHours * 3600000;
  const duration = Math.max(1, toTime - fromTime);
  const binMs = Math.max(15000, Math.ceil(duration / 120));
  const byNode = samplesByNode(samples);
  const series = [];

  [...byNode.entries()].forEach(([deviceId, group], index) => {
    const bins = new Map();
    for (const point of pointsForSamples(group.samples, now, fromHours, toHours)) {
      const bin = Math.floor((point.time - fromTime) / binMs);
      const existing = bins.get(bin) || { total: 0, count: 0, time: fromTime + bin * binMs + binMs / 2 };
      existing.total += Number(point.score) || 0;
      existing.count += 1;
      bins.set(bin, existing);
    }
    const rawPoints = [...bins.values()]
      .sort((a, b) => a.time - b.time)
      .map((bin) => ({ time: bin.time, score: bin.total / Math.max(1, bin.count) }));
    const min = Math.min(...rawPoints.map((point) => point.score));
    const max = Math.max(...rawPoints.map((point) => point.score));
    const spread = max - min;
    series.push({
      deviceId,
      name: group.name,
      color: COMPARISON_COLORS[index % COMPARISON_COLORS.length],
      points: rawPoints.map((point) => ({
        bin: Math.floor((point.time - fromTime) / binMs),
        time: point.time,
        score: spread > 0.000001 ? (point.score - min) / spread : 0.5
      }))
    });
  });

  return { series, fromTime, toTime };
}

/** Pearson trend alignment across overlapping normalized comparison points. */
function trendAlignment(series) {
  const correlations = [];
  for (let i = 0; i < series.length; i += 1) {
    for (let j = i + 1; j < series.length; j += 1) {
      const left = series[i].points;
      const right = series[j].points;
      const rightByBin = new Map(right.map((point) => [point.bin, point.score]));
      const matched = left
        .filter((point) => rightByBin.has(point.bin))
        .map((point) => [point.score, rightByBin.get(point.bin)]);
      if (matched.length < 4) continue;
      const leftValues = matched.map(([leftValue]) => leftValue);
      const rightValues = matched.map(([, rightValue]) => rightValue);
      const count = matched.length;
      const leftMean = leftValues.reduce((total, value) => total + value, 0) / count;
      const rightMean = rightValues.reduce((total, value) => total + value, 0) / count;
      let numerator = 0;
      let leftVariance = 0;
      let rightVariance = 0;
      for (let k = 0; k < count; k += 1) {
        const leftDelta = leftValues[k] - leftMean;
        const rightDelta = rightValues[k] - rightMean;
        numerator += leftDelta * rightDelta;
        leftVariance += leftDelta * leftDelta;
        rightVariance += rightDelta * rightDelta;
      }
      const denominator = Math.sqrt(leftVariance * rightVariance);
      if (denominator > 0.000001) {
        correlations.push(numerator / denominator);
      }
    }
  }
  if (correlations.length === 0) return null;
  const average = correlations.reduce((total, value) => total + value, 0) / correlations.length;
  return Math.round(Math.max(0, Math.min(1, (average + 1) / 2)) * 100);
}

/**
 * Draws one movement score chart onto a canvas.
 *
 * The y-axis auto-scales to visible data and trigger threshold, while the canvas
 * height remains fixed by chartDimensions().
 */
function drawChart(canvas, samples, options = {}) {
  const { ctx, width, height } = chartDimensions(canvas);
  const now = Date.now();
  const fromHours = historyWindow.fromHours;
  const toHours = historyWindow.toHours;
  const fromTime = now - fromHours * 3600000;
  const toTime = now - toHours * 3600000;
  const points = pointsForSamples(samples, now, fromHours, toHours);
  const visibleMax = Math.max(0, ...points.map((point) => Number(point.score) || 0));
  const yMax = Math.max(0.5, visibleMax, options.triggerLine ? movementTrigger.threshold : 0) * 1.15;
  const yMaxLabel = yMax.toFixed(1);
  ctx.font = "12px Arial, Helvetica, sans-serif";
  const padding = {
    left: Math.max(42, Math.ceil(ctx.measureText(yMaxLabel).width) + 16),
    right: 16,
    top: 14,
    bottom: 28
  };
  const innerWidth = width - padding.left - padding.right;
  const innerHeight = height - padding.top - padding.bottom;
  const x = (time) => padding.left + ((time - fromTime) / Math.max(1, toTime - fromTime)) * innerWidth;
  const y = (score) => padding.top + innerHeight - (Number(score) / yMax) * innerHeight;

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = "#edf1f5";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i += 1) {
    const yy = padding.top + (innerHeight * i) / 4;
    ctx.beginPath();
    ctx.moveTo(padding.left, yy);
    ctx.lineTo(width - padding.right, yy);
    ctx.stroke();
  }

  ctx.strokeStyle = "#cad3df";
  ctx.beginPath();
  ctx.moveTo(padding.left, padding.top);
  ctx.lineTo(padding.left, padding.top + innerHeight);
  ctx.lineTo(width - padding.right, padding.top + innerHeight);
  ctx.stroke();

  ctx.fillStyle = "#53606f";
  ctx.fillText(yMaxLabel, 6, padding.top + 4);
  ctx.fillText("0", 24, padding.top + innerHeight);
  ctx.fillText(`${formatHours(fromHours)} ago`, padding.left, height - 8);
  ctx.textAlign = "right";
  ctx.fillText(toHours === 0 ? "now" : `${formatHours(toHours)} ago`, width - padding.right, height - 8);
  ctx.textAlign = "left";

  if (options.triggerLine) {
    const triggerY = y(movementTrigger.threshold);
    ctx.strokeStyle = "#d77b1f";
    ctx.setLineDash([6, 4]);
    ctx.beginPath();
    ctx.moveTo(padding.left, triggerY);
    ctx.lineTo(width - padding.right, triggerY);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = "#d77b1f";
    ctx.fillText(`trigger ${movementTrigger.threshold.toFixed(2)}`, padding.left + 6, triggerY - 6);
  }

  if (points.length === 0) {
    ctx.fillStyle = "#53606f";
    ctx.fillText("No movement score samples in this window.", padding.left + 12, padding.top + 32);
    return;
  }

  ctx.strokeStyle = options.color || "#2f6fed";
  ctx.lineWidth = 2;
  ctx.beginPath();
  points.forEach((point, index) => {
    const px = x(point.time);
    const py = y(point.score);
    if (index === 0) {
      ctx.moveTo(px, py);
    } else {
      ctx.lineTo(px, py);
    }
  });
  ctx.stroke();

  ctx.fillStyle = options.color || "#2f6fed";
  for (const point of points) {
    const px = x(point.time);
    const py = y(point.score);
    ctx.beginPath();
    ctx.arc(px, py, 2.5, 0, Math.PI * 2);
    ctx.fill();
  }
}

/** Draws normalized sensor trends on one time-aligned chart. */
function drawComparisonChart() {
  const { ctx, width, height } = chartDimensions(comparisonChartEl);
  const { series, fromTime, toTime } = comparisonSeries(movementHistory.samples);
  const padding = { left: 42, right: 16, top: 14, bottom: 28 };
  const innerWidth = width - padding.left - padding.right;
  const innerHeight = height - padding.top - padding.bottom;
  const x = (time) => padding.left + ((time - fromTime) / Math.max(1, toTime - fromTime)) * innerWidth;
  const y = (score) => padding.top + innerHeight - Math.max(0, Math.min(1, score)) * innerHeight;

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, width, height);
  ctx.strokeStyle = "#edf1f5";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i += 1) {
    const yy = padding.top + (innerHeight * i) / 4;
    ctx.beginPath();
    ctx.moveTo(padding.left, yy);
    ctx.lineTo(width - padding.right, yy);
    ctx.stroke();
  }

  ctx.strokeStyle = "#cad3df";
  ctx.beginPath();
  ctx.moveTo(padding.left, padding.top);
  ctx.lineTo(padding.left, padding.top + innerHeight);
  ctx.lineTo(width - padding.right, padding.top + innerHeight);
  ctx.stroke();

  ctx.font = "12px Arial, Helvetica, sans-serif";
  ctx.fillStyle = "#53606f";
  ctx.fillText("1.0", 10, padding.top + 4);
  ctx.fillText("0", 24, padding.top + innerHeight);
  ctx.fillText(`${formatHours(historyWindow.fromHours)} ago`, padding.left, height - 8);
  ctx.textAlign = "right";
  ctx.fillText(historyWindow.toHours === 0 ? "now" : `${formatHours(historyWindow.toHours)} ago`, width - padding.right, height - 8);
  ctx.textAlign = "left";

  if (series.length < 2) {
    ctx.fillText("Need at least two nodes in this window.", padding.left + 12, padding.top + 32);
  }

  for (const line of series) {
    if (line.points.length === 0) continue;
    ctx.strokeStyle = line.color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    line.points.forEach((point, index) => {
      const px = x(point.time);
      const py = y(point.score);
      if (index === 0) {
        ctx.moveTo(px, py);
      } else {
        ctx.lineTo(px, py);
      }
    });
    ctx.stroke();
  }

  const alignment = trendAlignment(series);
  comparisonScoreEl.textContent = alignment === null ? "n/a" : `${alignment}% aligned`;
  comparisonLegendEl.replaceChildren(
    ...series.map((line) => {
      const item = document.createElement("span");
      item.className = "legend-item";
      const swatch = document.createElement("span");
      swatch.className = "legend-swatch";
      swatch.style.background = line.color;
      item.append(swatch, document.createTextNode(line.name));
      return item;
    })
  );
}

/** Redraws the aggregate chart and per-node charts from cached history data. */
function renderMovementCharts() {
  normalizeHistoryWindow();
  const aggregate = aggregateSamples(movementHistory.samples);
  drawChart(aggregateChartEl, aggregate, { triggerLine: true, color: "#1f8a5b" });
  drawComparisonChart();

  const byNode = samplesByNode(movementHistory.samples);

  nodeChartsEl.replaceChildren(
    ...[...byNode.entries()].map(([deviceId, group]) => {
      const card = document.createElement("article");
      card.className = "chart-card";
      appendText(card, "h3", `${group.name} Score`);
      const canvas = document.createElement("canvas");
      canvas.height = 160;
      card.appendChild(canvas);
      requestAnimationFrame(() => drawChart(canvas, group.samples, { color: "#2f6fed" }));
      return card;
    })
  );
}

/** Extends the history sliders to cover all retained movement samples. */
function updateHistorySliderMax(availableHours) {
  const available = Number.isFinite(availableHours) && availableHours > 0 ? availableHours : DEFAULT_HISTORY_HOURS;
  const max = Math.min(MAX_HISTORY_HOURS, Math.max(DEFAULT_HISTORY_HOURS, Math.ceil(available * 4) / 4));
  for (const input of [historyFromEl, historyToEl]) {
    input.max = String(max);
  }
  if (movementHistory.range.availableHours > 0 && historyWindow.fromHours > max) {
    historyWindow.fromHours = max;
  }
  historyFromEl.value = String(Math.min(historyWindow.fromHours, max));
  historyToEl.value = String(Math.min(historyWindow.toHours, max));
}

/** Fetches movement history for the selected time window and redraws charts. */
async function refreshMovementHistory() {
  normalizeHistoryWindow();
  const query = new URLSearchParams({
    fromHours: String(historyWindow.fromHours),
    toHours: String(historyWindow.toHours)
  });
  movementHistory = await readJson(`/api/history/movement?${query}`);
  updateHistorySliderMax(movementHistory.range.availableHours);
  renderMovementCharts();
}

/** Fetches Pi-side trigger threshold/settings and redraws trigger line. */
async function refreshMovementTrigger() {
  movementTrigger = await readJson("/api/history/movement/trigger");
  triggerLevelEl.max = "30";
  triggerLevelEl.value = String(Math.min(30, Number(movementTrigger.threshold) || 3));
  triggerLevelLabelEl.textContent = Number(movementTrigger.threshold).toFixed(2);
  triggerEnabledEl.checked = Boolean(movementTrigger.enabled);
  renderMovementCharts();
}

/** Persists the Pi-side trigger threshold/settings. */
async function saveMovementTrigger() {
  movementTrigger = {
    threshold: Number(triggerLevelEl.value),
    enabled: triggerEnabledEl.checked
  };
  triggerLevelLabelEl.textContent = movementTrigger.threshold.toFixed(2);
  renderMovementCharts();
  await postJson("/api/history/movement/trigger", movementTrigger);
}

/** Refreshes admin summary data used by push, records, nodes, and settings panels. */
async function refreshAdmin() {
  currentAdmin = await readJson("/api/admin/summary");
  renderPush(currentAdmin);
  if (lastStatusNodes.length && !isEditingNodeName()) {
    renderNodes(lastStatusNodes);
  }
  renderSecurity(currentAdmin.security);
  renderRecords(eventsListEl, currentAdmin.events, "No events yet.");
  renderRecords(auditListEl, currentAdmin.auditLog, "No audit records yet.");
}

/** Converts a VAPID public key from URL-safe base64 into PushManager bytes. */
function urlBase64ToUint8Array(base64String) {
  const padding = "=".repeat((4 - (base64String.length % 4)) % 4);
  const base64 = (base64String + padding).replace(/-/g, "+").replace(/_/g, "/");
  const rawData = window.atob(base64);
  return Uint8Array.from([...rawData].map((char) => char.charCodeAt(0)));
}

/** Registers the service worker and reports its current state in the dashboard. */
async function registerServiceWorker() {
  if (!("serviceWorker" in navigator)) {
    swStateEl.textContent = "unsupported";
    pushStateEl.textContent = "unsupported";
    return null;
  }

  try {
    const registration = await navigator.serviceWorker.register(`/service-worker.js?v=${appConfig.version}`);
    swStateEl.textContent = registration.active ? "active" : "registered";

    if ("PushManager" in window && "Notification" in window) {
      const permission = Notification.permission;
      pushStateEl.textContent = permission === "default" ? "available" : permission;
    } else {
      pushStateEl.textContent = "unsupported";
    }
    return registration;
  } catch {
    swStateEl.textContent = "error";
    pushStateEl.textContent = "unavailable";
    return null;
  }
}

/** Requests browser notification permission and stores this browser subscription. */
async function subscribePush() {
  const vapid = await readJson("/api/push/vapid-public-key");
  if (!vapid.publicKey) {
    throw new Error("Generate VAPID keys first");
  }
  const registration = await navigator.serviceWorker.ready;
  const permission = await Notification.requestPermission();
  if (permission !== "granted") {
    throw new Error("Notification permission was not granted");
  }
  const subscription = await registration.pushManager.subscribe({
    userVisibleOnly: true,
    applicationServerKey: urlBase64ToUint8Array(vapid.publicKey)
  });
  await postJson("/api/push/subscribe", {
    ...subscription.toJSON(),
    deviceName: navigator.userAgent.slice(0, 80)
  });
  pushStateEl.textContent = "subscribed";
}

/** Removes this browser's push subscription locally and from the Pi database. */
async function unsubscribePush() {
  const registration = await navigator.serviceWorker.ready;
  const subscription = await registration.pushManager.getSubscription();
  if (subscription) {
    await postJson("/api/push/unsubscribe", { endpoint: subscription.endpoint });
    await subscription.unsubscribe();
  }
  pushStateEl.textContent = "available";
}

document.querySelector("#refresh-app").addEventListener("click", refreshAppPayload);
document.querySelector("#mandatory-refresh").addEventListener("click", refreshAppPayload);

// Node list interactions use event delegation because the ESP32 Nodes card is
// re-rendered whenever fresh worker/admin data arrives.
nodesEl.addEventListener("change", async (event) => {
  const checkbox = event.target;
  if (checkbox.type !== "checkbox" || !checkbox.dataset.nodeRecordId) return;
  await runAction(async () => {
    await postJson(`/api/admin/nodes/${checkbox.dataset.nodeRecordId}/active`, { active: checkbox.checked });
    await refreshAdmin();
  }, "Node active state saved.");
});

/**
 * Saves a node display name to the Pi registry and, when reachable, to the ESP32
 * itself. Keeping both names aligned prevents the next telemetry poll from
 * making the UI look like the rename did not stick.
 */
async function saveNodeFriendlyName(input) {
  if (!input.dataset.nodeRecordId) return;
  const name = input.value.trim();
  if (!name) {
    input.value = "Unnamed node";
  }
  await runAction(async () => {
    const savedName = input.value.trim() || "Unnamed node";
    await postJson(`/api/admin/nodes/${input.dataset.nodeRecordId}/name`, { name: savedName });
    if (input.dataset.deviceId) {
      try {
        await postJson(`/api/nodes/${input.dataset.deviceId}/config`, { name: savedName });
      } catch (error) {
        console.warn("ESP32 node name update failed", error);
      }
    }
    await refreshAdmin();
  }, "Node name saved.");
}

nodesEl.addEventListener("focusout", async (event) => {
  if (!event.target.classList?.contains("node-name-input")) return;
  await saveNodeFriendlyName(event.target);
  renderNodes(lastStatusNodes);
});

// Enter commits a name edit by blurring the input, which triggers the same save
// path as clicking elsewhere.
nodesEl.addEventListener("keydown", async (event) => {
  if (!event.target.classList?.contains("node-name-input")) return;
  if (event.key === "Enter") {
    event.preventDefault();
    event.target.blur();
  }
});

// A focused name input is an edit surface, not a node navigation/settings click.
["click", "pointerdown", "mousedown", "touchstart"].forEach((eventName) => {
  nodesEl.addEventListener(eventName, (event) => {
    if (event.target.classList?.contains("node-name-input")) {
      event.stopImmediatePropagation();
    }
  });
});

// Distinguish Identify from Settings by using separate data attributes on the
// buttons. Both actions call Pi proxy endpoints.
nodesEl.addEventListener("click", async (event) => {
  const identifyDeviceId = event.target.dataset?.identifyDeviceId;
  if (identifyDeviceId) {
    await runAction(async () => {
      await postJson(`/api/nodes/${identifyDeviceId}/identify`);
    }, "Node identify blink started.");
    return;
  }

  const senderDeviceId = event.target.dataset?.senderDeviceId;
  if (senderDeviceId) {
    const nextEnabled = event.target.dataset.senderEnabled !== "true";
    await runAction(async () => {
      await postJson(`/api/nodes/${senderDeviceId}/config`, { enabled: nextEnabled });
      await refreshStatus();
    }, nextEnabled ? "CSI sender started." : "CSI sender stopped.");
    return;
  }

  const deviceId = event.target.dataset?.deviceId;
  if (!deviceId) return;
  await runAction(() => openNodeSettings(deviceId));
});

document.querySelector("#close-node-settings").addEventListener("click", closeNodeSettings);

// Node settings modal actions. These all operate through the Pi proxy so the
// browser never needs direct LAN access to an ESP32.
document.querySelector("#refresh-node-status").addEventListener("click", async () => {
  await runNodeAction(async () => {
    await refreshSelectedNodeStatus();
  }, "Node status refreshed.");
});

document.querySelector("#calibrate-node").addEventListener("click", async () => {
  if (selectedNodeRole === "csi_sender") return;
  await runNodeAction(async () => {
    await postJson(`/api/nodes/${selectedNodeDeviceId}/calibrate`);
    await refreshSelectedNodeStatus();
  }, "Calibration started.");
});

document.querySelector("#delete-node-calibration").addEventListener("click", async () => {
  if (selectedNodeRole === "csi_sender") return;
  await runNodeAction(async () => {
    await readJson(`/api/nodes/${selectedNodeDeviceId}/calibration`, { method: "DELETE" });
    await refreshSelectedNodeStatus();
    await refreshSelectedNodeCalibration();
  }, "Calibration deleted.");
});

nodeCaptureFormEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (selectedNodeRole === "csi_sender") return;
  await runNodeAction(async () => {
    const form = new FormData(nodeCaptureFormEl);
    const result = await postJson(`/api/nodes/${selectedNodeDeviceId}/capture/start`, {
      duration_seconds: Number(form.get("duration_seconds") || 30),
      mode: String(form.get("mode") || "features"),
      label: String(form.get("label") || "").trim()
    });
    nodeCaptureMessageEl.textContent = `Capture started: ${result.capture_id}`;
    await refreshSelectedNodeStatus();
    await refreshCaptures();
    return result;
  }, null);
});

document.querySelector("#stop-node-capture").addEventListener("click", async () => {
  if (selectedNodeRole === "csi_sender") return;
  await runNodeAction(async () => {
    await postJson(`/api/nodes/${selectedNodeDeviceId}/capture/stop`);
    await refreshSelectedNodeStatus();
    await refreshCaptures();
  }, "Capture stop requested.");
});

document.querySelector("#refresh-captures").addEventListener("click", async () => {
  await runNodeAction(async () => {
    await refreshCaptures();
  }, "Capture list refreshed.");
});

nodeConfigFormEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  await runNodeAction(async () => {
    const saved = await postJson(`/api/nodes/${selectedNodeDeviceId}/config`, nodeConfigPayload());
    populateNodeConfigForm(saved);
    await refreshSelectedNodeStatus();
    await refreshStatus();
  }, "Configuration saved to ESP32.");
});

document.querySelector("#reload-node-calibration").addEventListener("click", async () => {
  if (selectedNodeRole === "csi_sender") return;
  await runNodeAction(async () => {
    await refreshSelectedNodeCalibration();
  }, "Calibration reloaded.");
});

nodeCalibrationFormEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  if (selectedNodeRole === "csi_sender") return;
  await runNodeAction(async () => {
    const saved = await postJson(`/api/nodes/${selectedNodeDeviceId}/calibration`, nodeCalibrationPayload());
    populateNodeCalibrationForm(saved);
    await refreshSelectedNodeStatus();
  }, "Calibration saved to ESP32 NVS.");
});

// Admin and push management actions.
document.querySelector("#generate-vapid").addEventListener("click", async () => {
  await runAction(async () => {
    await postJson("/api/admin/vapid/generate");
    await refreshAdmin();
  }, "VAPID keys generated.");
});

vapidFormEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(vapidFormEl);
  await runAction(async () => {
    await postJson("/api/admin/vapid", Object.fromEntries(form.entries()));
    await refreshAdmin();
  }, "VAPID keys saved.");
});

document.querySelector("#subscribe-push").addEventListener("click", async () => {
  await runAction(async () => {
    await subscribePush();
    await refreshAdmin();
  }, "Device subscribed.");
});

document.querySelector("#unsubscribe-push").addEventListener("click", async () => {
  await runAction(async () => {
    await unsubscribePush();
    await refreshAdmin();
  }, "Device unsubscribed.");
});

document.querySelector("#test-push").addEventListener("click", async () => {
  await runAction(async () => {
    const result = await postJson("/api/push/test", {});
    await refreshAdmin();
    return result;
  }, (result) =>
    result.skipped ? "Push skipped because VAPID is not configured." : `Push sent to ${result.sent} device(s).`
  );
});

document.querySelector("#save-security").addEventListener("click", async () => {
  const form = new FormData(securityFormEl);
  const settings = {};
  for (const key of Object.keys(SECURITY_LABELS)) {
    settings[key] = form.get(key) === "on";
  }
  await runAction(async () => {
    await postJson("/api/admin/security", settings);
    await refreshAdmin();
  }, "Security settings saved.");
});

document.querySelector("#backup-db").addEventListener("click", async () => {
  await runAction(async () => {
    const result = await postJson("/api/admin/backup");
    await refreshAdmin();
    return result;
  }, (result) => `Backup created: ${result.path}`);
});

// Movement history controls redraw immediately while sliding, then fetch fresh
// persisted samples when the user releases/commits the range.
for (const input of [historyFromEl, historyToEl]) {
  input.addEventListener("input", renderMovementCharts);
  input.addEventListener("change", () => {
    refreshMovementHistory().catch((error) => setMessage(apiUnavailableMessage(error)));
  });
}

// Trigger threshold is a Pi-side display/alert setting, independent of the
// ESP32 firmware threshold configured on each node.
triggerLevelEl.addEventListener("input", () => {
  movementTrigger.threshold = Number(triggerLevelEl.value);
  triggerLevelLabelEl.textContent = movementTrigger.threshold.toFixed(2);
  renderMovementCharts();
});

triggerLevelEl.addEventListener("change", () => {
  saveMovementTrigger().catch((error) => setMessage(apiUnavailableMessage(error)));
});

triggerEnabledEl.addEventListener("change", () => {
  saveMovementTrigger().catch((error) => setMessage(apiUnavailableMessage(error)));
});

window.addEventListener("resize", renderMovementCharts);

// Service worker may tell open pages that browser push subscription state has
// changed, for example after the user agent rotates a subscription.
if ("serviceWorker" in navigator) {
  navigator.serviceWorker.addEventListener("message", (event) => {
    if (event.data?.type === "PUSH_SUBSCRIPTION_CHANGED") {
      pushStateEl.textContent = "resubscribe needed";
    }
  });
}

// Initial boot sequence. These calls are intentionally independent enough that a
// failure in push/service-worker setup does not prevent status and admin data
// from rendering.
registerServiceWorker();
attachNodeSettingsHelp();
refreshStatus();
refreshAdmin().catch((error) => setMessage(error.message));
refreshMovementTrigger()
  .then(refreshMovementHistory)
  .catch((error) => setMessage(apiUnavailableMessage(error)));

// Installed PWAs can remain open for long periods. Refresh when the user returns
// to the app and on intervals while visible/open.
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) {
    refreshStatus();
    refreshMovementHistory().catch(() => undefined);
  }
});
setInterval(refreshStatus, 5000);
setInterval(() => refreshAdmin().catch(() => undefined), 30000);
setInterval(() => refreshMovementHistory().catch(() => undefined), 30000);
