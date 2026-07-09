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
      item.className = "node";
      const payload = node.payload || {};
      item.innerHTML = `
        <strong>${node.name}</strong>
        <span>ID ${node.device_id} · ${node.state}</span>
        <span>${node.ip}</span>
        <span>Score ${payload.movement_score ?? "n/a"}</span>
      `;
      return item;
    })
  );
}

function showUpdateBanner(version, mandatory) {
  updateMessageEl.textContent = mandatory ? `Version ${version} is required` : `Version ${version} is available`;
  updateBannerEl.classList.remove("hidden");
  mandatoryUpdateEl.classList.toggle("hidden", !mandatory);
}

async function refreshAppPayload() {
  const registrations = await navigator.serviceWorker?.getRegistrations?.();
  if (registrations) {
    await Promise.all(registrations.map((registration) => registration.unregister()));
  }
  if ("caches" in window) {
    const keys = await caches.keys();
    await Promise.all(keys.filter((key) => key.startsWith("esp32-alarm-")).map((key) => caches.delete(key)));
  }
  localStorage.setItem(VERSION_KEY, appConfig.version);
  window.location.reload();
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
      row.innerHTML = `
        <strong>${user.username}</strong>
        <input name="displayName" value="${user.displayName}">
        <select name="role">
          ${["viewer", "resident", "admin", "owner"]
            .map((role) => `<option value="${role}" ${role === user.role ? "selected" : ""}>${role}</option>`)
            .join("")}
        </select>
        <select name="state">
          ${["active", "disabled", "setup-required"]
            .map((state) => `<option value="${state}" ${state === user.state ? "selected" : ""}>${state}</option>`)
            .join("")}
        </select>
        <button type="submit">Save</button>
        <button type="button" data-delete-user="${user.id}">Delete</button>
      `;
      return row;
    })
  );
}

function renderPush(admin) {
  vapidConfiguredEl.textContent = admin.vapid.privateKeyConfigured ? "yes" : "no";
  pushCountEl.textContent = String(admin.pushSubscriptions.length);
  pushSubscriptionsEl.replaceChildren(
    ...admin.pushSubscriptions.map((subscription) => {
      const item = document.createElement("div");
      item.className = "record";
      item.innerHTML = `
        <strong>${subscription.deviceName}</strong>
        <span>${subscription.enabled ? "enabled" : "disabled"} · failures ${subscription.failureCount}</span>
        <small>${subscription.endpoint}</small>
      `;
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
      row.innerHTML = `
        <strong>ID ${node.deviceId}</strong>
        <input name="name" value="${node.name}">
        <label><input name="expected" type="checkbox" ${node.expected ? "checked" : ""}> expected</label>
        <label><input name="active" type="checkbox" ${node.active ? "checked" : ""}> active</label>
        <button type="submit">Save</button>
        <small>${node.ip || "no IP"} · ${node.lastSeenAt || "never seen"}</small>
      `;
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
      item.innerHTML = `
        <strong>${record.title || record.action}</strong>
        <span>${record.type || record.target || ""}</span>
        <small>${record.createdAt}</small>
      `;
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
  await postJson("/api/admin/users", Object.fromEntries(form.entries()));
  userFormEl.reset();
  setMessage("User added.");
  await refreshAdmin();
});

usersListEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  const formEl = event.target;
  const form = new FormData(formEl);
  await putJson(`/api/admin/users/${formEl.dataset.id}`, Object.fromEntries(form.entries()));
  setMessage("User saved.");
  await refreshAdmin();
});

usersListEl.addEventListener("click", async (event) => {
  const id = event.target.dataset?.deleteUser;
  if (!id) return;
  await readJson(`/api/admin/users/${id}`, { method: "DELETE" });
  setMessage("User deleted.");
  await refreshAdmin();
});

adminNodesListEl.addEventListener("submit", async (event) => {
  event.preventDefault();
  const formEl = event.target;
  const form = new FormData(formEl);
  await putJson(`/api/admin/nodes/${formEl.dataset.id}`, {
    name: form.get("name"),
    expected: form.get("expected") === "on",
    active: form.get("active") === "on"
  });
  setMessage("Node saved.");
  await refreshAdmin();
});

document.querySelector("#generate-vapid").addEventListener("click", async () => {
  await postJson("/api/admin/vapid/generate");
  setMessage("VAPID keys generated.");
  await refreshAdmin();
});

document.querySelector("#subscribe-push").addEventListener("click", async () => {
  await subscribePush();
  setMessage("Device subscribed.");
  await refreshAdmin();
});

document.querySelector("#unsubscribe-push").addEventListener("click", async () => {
  await unsubscribePush();
  setMessage("Device unsubscribed.");
  await refreshAdmin();
});

document.querySelector("#test-push").addEventListener("click", async () => {
  const result = await postJson("/api/push/test", {});
  setMessage(result.skipped ? "Push skipped because VAPID is not configured." : `Push sent to ${result.sent} device(s).`);
  await refreshAdmin();
});

document.querySelector("#save-security").addEventListener("click", async () => {
  const form = new FormData(securityFormEl);
  const settings = {};
  for (const key of Object.keys(SECURITY_LABELS)) {
    settings[key] = form.get(key) === "on";
  }
  await postJson("/api/admin/security", settings);
  setMessage("Security settings saved.");
  await refreshAdmin();
});

document.querySelector("#backup-db").addEventListener("click", async () => {
  const result = await postJson("/api/admin/backup");
  setMessage(`Backup created: ${result.path}`);
  await refreshAdmin();
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
