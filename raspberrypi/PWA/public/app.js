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
const usersListEl = document.querySelector("#users-list");
const userFormEl = document.querySelector("#user-form");
const vapidConfiguredEl = document.querySelector("#vapid-configured");
const vapidFormEl = document.querySelector("#vapid-form");
const pushCountEl = document.querySelector("#push-count");
const pushSubscriptionsEl = document.querySelector("#push-subscriptions");
const adminNodesListEl = document.querySelector("#admin-nodes-list");
const securityFormEl = document.querySelector("#security-form");
const adminMessageEl = document.querySelector("#admin-message");
const eventsListEl = document.querySelector("#events-list");
const auditListEl = document.querySelector("#audit-list");

const VERSION_KEY = "esp32-alarm:last-seen-version";
const SECURITY_LABELS = {
  cloudflareAccessExpected: "Cloudflare Access expected",
  appSessionsEnabled: "App sessions enabled",
  csrfProtectionEnabled: "CSRF protection enabled",
  loginRateLimitEnabled: "Login rate limiting enabled",
  auditLoggingEnabled: "Audit logging enabled",
  localRecoveryOnly: "Local recovery only"
};

let currentAdmin = null;
let pendingHostVersion = null;

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
    throw new Error(payload.error || `Request failed with ${response.status}`);
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

function renderNodes(nodes) {
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
      item.className = nodeHasMovement(payload) ? "node movement" : "node";
      appendText(item, "strong", String(node.name ?? "Unnamed node"));
      appendText(item, "span", `ID ${node.device_id} · ${node.state}`);
      appendText(item, "span", String(node.ip ?? ""));
      appendText(item, "span", `Score ${formatScore(payload.movement_score)}`);
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

function renderUsers(users) {
  usersListEl.replaceChildren(
    ...users.map((user) => {
      const row = document.createElement("form");
      row.className = "record-grid user-row";
      row.dataset.id = String(user.id);
      appendText(row, "strong", user.username);

      const displayName = document.createElement("input");
      displayName.name = "displayName";
      displayName.value = user.displayName;
      row.appendChild(displayName);

      const role = document.createElement("select");
      role.name = "role";
      for (const value of ["viewer", "resident", "admin", "owner"]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = value;
        role.appendChild(option);
      }
      setSelectValue(role, user.role);
      row.appendChild(role);

      const state = document.createElement("select");
      state.name = "state";
      for (const value of ["active", "disabled", "setup-required"]) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = value;
        state.appendChild(option);
      }
      setSelectValue(state, user.state);
      row.appendChild(state);

      const save = document.createElement("button");
      save.type = "submit";
      save.textContent = "Save";
      row.appendChild(save);

      const deleteButton = document.createElement("button");
      deleteButton.type = "button";
      deleteButton.dataset.deleteUser = String(user.id);
      deleteButton.textContent = "Delete";
      row.appendChild(deleteButton);
      return row;
    })
  );
}

function setSelectValue(select, value) {
  for (const option of select.options) {
    option.selected = option.value === value;
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

function renderAdminNodes(nodes) {
  if (!nodes.length) {
    adminNodesListEl.textContent = "No persistent node records yet.";
    return;
  }
  adminNodesListEl.replaceChildren(
    ...nodes.map((node) => {
      const row = document.createElement("form");
      row.className = "record-grid node-row";
      row.dataset.id = String(node.id);
      appendText(row, "strong", `ID ${node.deviceId}`);

      const name = document.createElement("input");
      name.name = "name";
      name.value = node.name;
      row.appendChild(name);

      const expectedLabel = document.createElement("label");
      const expected = document.createElement("input");
      expected.name = "expected";
      expected.type = "checkbox";
      expected.checked = node.expected;
      expectedLabel.appendChild(expected);
      expectedLabel.append(" expected");
      row.appendChild(expectedLabel);

      const activeLabel = document.createElement("label");
      const active = document.createElement("input");
      active.name = "active";
      active.type = "checkbox";
      active.checked = node.active;
      activeLabel.appendChild(active);
      activeLabel.append(" active");
      row.appendChild(activeLabel);

      const save = document.createElement("button");
      save.type = "submit";
      save.textContent = "Save";
      row.appendChild(save);

      appendText(row, "small", `${node.ip || "no IP"} · ${node.lastSeenAt || "never seen"}`);
      return row;
    })
  );
}

function renderSecurity(settings) {
  securityFormEl.replaceChildren(
    ...Object.entries(SECURITY_LABELS).map(([key, label]) => {
      const row = document.createElement("label");
      row.innerHTML = `<input type="checkbox" name="${key}" ${settings[key] ? "checked" : ""}> ${label}`;
      return row;
    })
  );
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

async function refreshAdmin() {
  currentAdmin = await readJson("/api/admin/summary");
  renderUsers(currentAdmin.users);
  renderPush(currentAdmin);
  renderAdminNodes(currentAdmin.nodes);
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

userFormEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(userFormEl);
  await runAction(async () => {
    await postJson("/api/admin/users", Object.fromEntries(form.entries()));
    userFormEl.reset();
    await refreshAdmin();
  }, "User added.");
});

usersListEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  const formEl = event.target;
  const form = new FormData(formEl);
  await runAction(async () => {
    await putJson(`/api/admin/users/${formEl.dataset.id}`, Object.fromEntries(form.entries()));
    await refreshAdmin();
  }, "User saved.");
});

usersListEl.addEventListener("click", async (event) => {
  const id = event.target.dataset?.deleteUser;
  if (!id) return;
  await runAction(async () => {
    await readJson(`/api/admin/users/${id}`, { method: "DELETE" });
    await refreshAdmin();
  }, "User deleted.");
});

adminNodesListEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  const formEl = event.target;
  const form = new FormData(formEl);
  await runAction(async () => {
    await putJson(`/api/admin/nodes/${formEl.dataset.id}`, {
      name: form.get("name"),
      expected: form.get("expected") === "on",
      active: form.get("active") === "on"
    });
    await refreshAdmin();
  }, "Node saved.");
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
refreshStatus();
refreshAdmin().catch((error) => setMessage(error.message));
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) {
    refreshStatus();
  }
});
setInterval(refreshStatus, 5000);
setInterval(() => refreshAdmin().catch(() => undefined), 30000);
