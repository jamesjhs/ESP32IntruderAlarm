const versionEl = document.querySelector("#version");
const pwaStateEl = document.querySelector("#pwa-state");
const workerStateEl = document.querySelector("#worker-state");
const swStateEl = document.querySelector("#sw-state");
const pushStateEl = document.querySelector("#push-state");
const knownNodesEl = document.querySelector("#known-nodes");
const healthyNodesEl = document.querySelector("#healthy-nodes");
const nodesEl = document.querySelector("#nodes");

async function readJson(url) {
  const response = await fetch(url, {
    cache: "no-store",
    headers: { accept: "application/json" }
  });
  const contentType = response.headers.get("content-type") || "";
  if (!contentType.includes("application/json")) {
    throw new Error("Expected JSON but received a non-JSON response");
  }
  if (!response.ok) {
    throw new Error(`Request failed with ${response.status}`);
  }
  return response.json();
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

async function refresh() {
  try {
    const version = await readJson("/api/version");
    versionEl.textContent = version.version;
    pwaStateEl.textContent = "online";
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

async function registerServiceWorker() {
  if (!("serviceWorker" in navigator)) {
    swStateEl.textContent = "unsupported";
    pushStateEl.textContent = "unsupported";
    return;
  }

  try {
    const registration = await navigator.serviceWorker.register("/service-worker.js?v=0.0.1");
    swStateEl.textContent = registration.active ? "active" : "registered";

    if ("PushManager" in window && "Notification" in window) {
      const permission = Notification.permission;
      pushStateEl.textContent = permission === "default" ? "available" : permission;
    } else {
      pushStateEl.textContent = "unsupported";
    }
  } catch {
    swStateEl.textContent = "error";
    pushStateEl.textContent = "unavailable";
  }
}

if ("serviceWorker" in navigator) {
  navigator.serviceWorker.addEventListener("message", (event) => {
    if (event.data?.type === "PUSH_SUBSCRIPTION_CHANGED") {
      pushStateEl.textContent = "resubscribe needed";
    }
  });
}

registerServiceWorker();
refresh();
setInterval(refresh, 5000);
