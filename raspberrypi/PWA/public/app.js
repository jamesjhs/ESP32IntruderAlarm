const appConfig = window.ALARM_APP_CONFIG || { version: "0.0.1", build: null };
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
const nodeStatusDetailsEl = document.querySelector("#node-status-details");
const nodeConfigFormEl = document.querySelector("#node-config-form");
const nodeSettingsMessageEl = document.querySelector("#node-settings-message");

const VERSION_KEY = "esp32-alarm:last-seen-version";
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
  calibrating: "Whether the node is currently recording a quiet baseline."
};

const NODE_CONFIG_HELP = {
  device_id: "Logical ESP32 node number. Keep each installed node unique.",
  name: "Display and hostname-style label for this ESP32 node.",
  pi_ip: "LAN IP address of the Raspberry Pi telemetry receiver.",
  pi_port: "Port on the Pi worker that accepts ESP32 telemetry posts.",
  pi_api_path: "HTTP path on the Pi worker for telemetry, normally /espdata.",
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

const NODE_ACTION_HELP = {
  refreshNodeStatus: "Fetch the latest live status directly from this ESP32 node through the Pi.",
  calibrateNode: "Ask the ESP32 to record a fresh quiet baseline. Keep the area still while it runs.",
  deleteNodeCalibration: "Clear the node's saved baseline so it can learn a new one.",
  saveNodeConfig: "Send these configuration values to the ESP32 node's /api/config endpoint."
};

let currentAdmin = null;
let pendingHostVersion = null;
let lastStatusNodes = [];
let selectedNodeDeviceId = null;

function apiUnavailableMessage(error) {
  if (String(error?.message || "").includes("Not Found")) {
    return "Admin API unavailable. Rebuild and restart the PWA server so the new routes are active.";
  }
  return error?.message || "Request failed.";
}

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

function appendText(parent, tagName, text, className) {
  const element = document.createElement(tagName);
  element.textContent = text;
  if (className) {
    element.className = className;
  }
  parent.appendChild(element);
  return element;
}

function asNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function formatScore(value) {
  const number = asNumber(value);
  return number === null ? "n/a" : number.toFixed(3);
}

function truncateToDecimals(value, decimals) {
  const number = asNumber(value);
  if (number === null) return null;
  const factor = 10 ** decimals;
  return Math.trunc((number + Number.EPSILON) * factor) / factor;
}

function formatTruncated(value, decimals) {
  const number = truncateToDecimals(value, decimals);
  return number === null ? "" : number.toFixed(decimals);
}

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

function postJson(url, body = {}) {
  return readJson(url, { method: "POST", body: JSON.stringify(body) });
}

function putJson(url, body = {}) {
  return readJson(url, { method: "PUT", body: JSON.stringify(body) });
}

function setMessage(message) {
  adminMessageEl.textContent = message;
}

function setNodeSettingsMessage(message) {
  nodeSettingsMessageEl.textContent = message;
}

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

function nodeRecordFor(deviceId) {
  return currentAdmin?.nodes?.find((node) => Number(node.deviceId) === Number(deviceId)) || null;
}

function renderNodes(nodes) {
  lastStatusNodes = nodes;
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
      const registryNode = nodeRecordFor(node.device_id);
      item.className = nodeHasMovement(payload) ? "node movement" : "node";
      appendText(item, "strong", String(node.name ?? "Unnamed node"));
      appendText(item, "span", `ID ${node.device_id} · ${node.state}`);
      appendText(item, "span", String(node.ip ?? ""));
      appendText(item, "span", `Score ${formatScore(payload.movement_score)}`);

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
    renderNodes([]);
  }
}

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

function attachNodeSettingsHelp() {
  for (const [name, help] of Object.entries(NODE_CONFIG_HELP)) {
    const input = nodeConfigFormEl.elements[name];
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
    ['#node-config-form button[type="submit"]', NODE_ACTION_HELP.saveNodeConfig]
  ];
  for (const [selector, help] of actions) {
    const button = document.querySelector(selector);
    if (button) {
      button.title = help;
      button.setAttribute("aria-label", help);
    }
  }
}

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

function renderNodeStatus(status) {
  const keys = [
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
    "packet_count",
    "last_packet_ms",
    "calibrating"
  ];
  nodeStatusDetailsEl.replaceChildren(
    ...keys.map((key) => {
      const row = document.createElement("div");
      const label = appendText(row, "dt", key);
      appendHelp(label, NODE_STATUS_HELP[key] || "Live status value reported by the ESP32 node.");
      const value = status?.[key];
      const display = ["movement_score", "baseline_noise"].includes(key) ? formatScore(value) : String(value ?? "n/a");
      appendText(row, "dd", display);
      return row;
    })
  );
}

function setFormNumber(form, name, value) {
  form.elements[name].value = value === undefined || value === null ? "" : String(value);
}

function setFormDecimal(form, name, value, decimals) {
  form.elements[name].value = formatTruncated(value, decimals);
}

function populateNodeConfigForm(config) {
  attachNodeSettingsHelp();
  nodeConfigFormEl.elements.device_id.value = config.device_id ?? "";
  nodeConfigFormEl.elements.name.value = config.name ?? "";
  nodeConfigFormEl.elements.pi_ip.value = config.pi_ip ?? "";
  setFormNumber(nodeConfigFormEl, "pi_port", config.pi_port ?? 3005);
  nodeConfigFormEl.elements.pi_api_path.value = config.pi_api_path ?? "/espdata";
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

function nodeConfigPayload() {
  const form = new FormData(nodeConfigFormEl);
  const apiPath = String(form.get("pi_api_path") || "/espdata").trim();
  return {
    device_id: Number(form.get("device_id")),
    name: String(form.get("name") || "").trim(),
    pi_ip: String(form.get("pi_ip") || "").trim(),
    pi_port: Number(form.get("pi_port")),
    pi_api_path: apiPath.startsWith("/") ? apiPath : `/${apiPath}`,
    pi_post_interval_ms: Number(form.get("pi_post_interval_s")) * 1000,
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

async function refreshSelectedNodeStatus() {
  if (selectedNodeDeviceId === null) return;
  const status = await readJson(`/api/nodes/${selectedNodeDeviceId}/status`);
  renderNodeStatus(status);
}

async function openNodeSettings(deviceId) {
  selectedNodeDeviceId = Number(deviceId);
  const liveNode = lastStatusNodes.find((node) => Number(node.device_id) === selectedNodeDeviceId);
  nodeSettingsTitleEl.textContent = liveNode ? `${liveNode.name} Settings` : `Node ${selectedNodeDeviceId} Settings`;
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

  if (config) {
    setNodeSettingsMessage("Settings loaded.");
  }
}

function closeNodeSettings() {
  nodeSettingsModalEl.classList.add("hidden");
  selectedNodeDeviceId = null;
  nodeStatusDetailsEl.replaceChildren();
  nodeConfigFormEl.reset();
  setNodeSettingsMessage("");
}

async function refreshAdmin() {
  currentAdmin = await readJson("/api/admin/summary");
  renderPush(currentAdmin);
  if (lastStatusNodes.length) {
    renderNodes(lastStatusNodes);
  }
  renderSecurity(currentAdmin.security);
  renderRecords(eventsListEl, currentAdmin.events, "No events yet.");
  renderRecords(auditListEl, currentAdmin.auditLog, "No audit records yet.");
}

function urlBase64ToUint8Array(base64String) {
  const padding = "=".repeat((4 - (base64String.length % 4)) % 4);
  const base64 = (base64String + padding).replace(/-/g, "+").replace(/_/g, "/");
  const rawData = window.atob(base64);
  return Uint8Array.from([...rawData].map((char) => char.charCodeAt(0)));
}

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

nodesEl.addEventListener("change", async (event) => {
  const checkbox = event.target;
  if (checkbox.type !== "checkbox" || !checkbox.dataset.nodeRecordId) return;
  await runAction(async () => {
    await postJson(`/api/admin/nodes/${checkbox.dataset.nodeRecordId}/active`, { active: checkbox.checked });
    await refreshAdmin();
  }, "Node active state saved.");
});

nodesEl.addEventListener("click", async (event) => {
  const deviceId = event.target.dataset?.deviceId;
  if (!deviceId) return;
  await runAction(() => openNodeSettings(deviceId));
});

document.querySelector("#close-node-settings").addEventListener("click", closeNodeSettings);

document.querySelector("#refresh-node-status").addEventListener("click", async () => {
  await runNodeAction(async () => {
    await refreshSelectedNodeStatus();
  }, "Node status refreshed.");
});

document.querySelector("#calibrate-node").addEventListener("click", async () => {
  await runNodeAction(async () => {
    await postJson(`/api/nodes/${selectedNodeDeviceId}/calibrate`);
    await refreshSelectedNodeStatus();
  }, "Calibration started.");
});

document.querySelector("#delete-node-calibration").addEventListener("click", async () => {
  await runNodeAction(async () => {
    await readJson(`/api/nodes/${selectedNodeDeviceId}/calibration`, { method: "DELETE" });
    await refreshSelectedNodeStatus();
  }, "Calibration deleted.");
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

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.addEventListener("message", (event) => {
    if (event.data?.type === "PUSH_SUBSCRIPTION_CHANGED") {
      pushStateEl.textContent = "resubscribe needed";
    }
  });
}

registerServiceWorker();
attachNodeSettingsHelp();
refreshStatus();
refreshAdmin().catch((error) => setMessage(error.message));
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) {
    refreshStatus();
  }
});
setInterval(refreshStatus, 5000);
setInterval(() => refreshAdmin().catch(() => undefined), 30000);
