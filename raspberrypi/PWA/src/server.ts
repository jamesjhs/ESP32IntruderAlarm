import path from "node:path";
import fs from "node:fs/promises";
import { isIP } from "node:net";
import Fastify from "fastify";
import fastifyStatic from "@fastify/static";
import webpush from "web-push";

import { loadConfig } from "./config";
import { AlarmDatabase, NodeRecord, SecuritySettings, UserRole } from "./db";

// Process-level setup is intentionally done once at module load. The Fastify
// app, route handlers, push sender, and background worker poll all share the
// same configuration and database handle so state changes are immediately
// visible across requests.
const appConfig = loadConfig();
const publicDir = path.resolve(__dirname, "..", "public");
const db = new AlarmDatabase(appConfig.databasePath, appConfig.sqlCipherKey);
const DEFAULT_HISTORY_HOURS = 0.5;
const MAX_HISTORY_HOURS = 24;
const CAPTURE_ID_RE = /^[A-Za-z0-9_. -]{1,40}$/;

// The Python worker can be polled by browsers via /api/status and by the
// background timer below. Cache the most recent good response briefly so the PWA
// can refresh frequently without hammering the worker on every browser request.
let lastWorkerStatus: any = null;
let lastWorkerStatusAt = 0;

type PushSubscriptionBody = {
  endpoint?: string;
  keys?: {
    p256dh?: string;
    auth?: string;
  };
  deviceName?: string;
  userId?: number | null;
};

/**
 * Checks whether an arbitrary request body value is one of the supported admin
 * roles. Keeping this as a type guard lets route handlers validate external JSON
 * and then pass a strongly-typed role into the database layer.
 */
function validRole(role: unknown): role is UserRole {
  return role === "owner" || role === "admin" || role === "resident" || role === "viewer";
}

/**
 * Loads VAPID private settings from the database and configures the web-push
 * library for the current process. Returns false when keys are not configured so
 * callers can skip push delivery cleanly instead of throwing on every attempt.
 */
function configureWebPush() {
  const vapid = db.getVapidPrivateSettings();
  if (vapid.publicKey && vapid.privateKey) {
    webpush.setVapidDetails(vapid.subject || appConfig.vapidSubject, vapid.publicKey, vapid.privateKey);
    return true;
  }
  return false;
}

/**
 * Converts the subscription shape stored in SQLite into the Web Push API shape
 * expected by the `web-push` package. The database stores key columns flat for
 * easier querying; the sender needs them nested under `keys`.
 */
function normalizePushSubscription(row: { endpoint: string; p256dh: string; auth: string }) {
  return {
    endpoint: row.endpoint,
    keys: {
      p256dh: row.p256dh,
      auth: row.auth
    }
  };
}

/**
 * Sends one payload to every enabled push subscription.
 *
 * Expired subscriptions are deleted when push relay services return 404/410.
 * Other failures disable the subscription and retain the error so the admin UI
 * can show why a browser stopped receiving notifications. The return value is a
 * compact delivery summary used by test-push and movement-trigger routes.
 */
async function sendPushToEnabledSubscriptions(payload: unknown) {
  if (!configureWebPush()) {
    return { sent: 0, failed: 0, skipped: true };
  }

  let sent = 0;
  let failed = 0;
  for (const subscription of db.listEnabledPushSubscriptions()) {
    try {
      await webpush.sendNotification(normalizePushSubscription(subscription), JSON.stringify(payload));
      sent += 1;
    } catch (error: any) {
      failed += 1;
      const statusCode = Number(error?.statusCode ?? 0);
      const message = String(error?.body || error?.message || "push send failed");
      if (statusCode === 404 || statusCode === 410) {
        db.deletePushSubscription(subscription.endpoint);
      } else {
        db.disablePushSubscription(subscription.endpoint, message);
      }
    }
  }
  return { sent, failed, skipped: false };
}

/**
 * Reads a public asset and injects the active application version.
 *
 * The files in `public/` contain development sentinels. At runtime this helper
 * replaces those sentinels with the version read from `raspberrypi/VERSION`, so
 * installed PWAs see new cache names and asset query strings after a deploy.
 */
async function renderVersionedAsset(fileName: string) {
  const filePath = path.join(publicDir, fileName);
  const text = await fs.readFile(filePath, "utf8");
  if (fileName === "index.html") {
    return text
      .replaceAll("?v=dev", `?v=${appConfig.version}`)
      .replace('<span id="version" class="badge">dev</span>', `<span id="version" class="badge">${appConfig.version}</span>`);
  }
  if (fileName === "service-worker.js") {
    return text.replace('const APP_VERSION = "dev";', `const APP_VERSION = ${JSON.stringify(appConfig.version)};`);
  }
  return text.replaceAll("?v=dev", `?v=${appConfig.version}`);
}

/**
 * Mirrors the Python worker's live node list into the PWA database.
 *
 * This keeps the admin node registry fresh with IPs, payload snapshots, and last
 * seen timestamps. The upsert deliberately preserves an existing user-edited Pi
 * display name, while still using the worker-reported name when a node is first
 * discovered. Movement scores are also recorded for history charts.
 */
function syncWorkerNodes(status: any) {
  for (const node of status?.nodes ?? []) {
    db.upsertNode({
      deviceId: Number(node.device_id),
      name: String(node.name ?? `Movement${Number(node.device_id).toString(16).padStart(2, "0")}`),
      ip: String(node.ip ?? ""),
      active: node.state === "online",
      payload: node.payload ?? {}
    });
  }
  db.recordMovementScores(status?.nodes ?? []);
}

/**
 * Evaluates the Pi-side movement trigger against the latest worker status.
 *
 * The ESP32 nodes produce per-node movement scores, but this service owns the
 * aggregate chart threshold and push notification policy. A push is sent only
 * when the aggregate crosses from below to above the configured threshold, which
 * avoids repeat notifications while the signal remains high.
 */
async function checkMovementTrigger(status: any) {
  const settings = db.getMovementTriggerSettings();
  if (!settings.enabled) {
    if (settings.lastAbove) {
      db.setMovementTriggerLastAbove(false);
    }
    return;
  }

  const activeDeviceIds = new Set(db.listNodes().filter((node) => node.active).map((node) => node.deviceId));
  const triggeredNodes = [];
  let aggregateScore = 0;

  for (const node of status?.nodes ?? []) {
    const deviceId = Number(node.device_id);
    if (!activeDeviceIds.has(deviceId)) {
      continue;
    }
    const score = Number(node.payload?.movement_score);
    if (!Number.isFinite(score)) {
      continue;
    }
    aggregateScore = Math.max(aggregateScore, score);
    if (score >= settings.threshold) {
      triggeredNodes.push({
        deviceId,
        name: String(node.name ?? `Movement${deviceId.toString(16).padStart(2, "0")}`),
        score
      });
    }
  }

  const above = aggregateScore >= settings.threshold && triggeredNodes.length > 0;
  if (above && !settings.lastAbove) {
    const timestamp = new Date().toISOString();
    const title = "ESP32 movement trigger";
    const body = `${triggeredNodes.map((node) => node.name).join(", ")} crossed ${settings.threshold.toFixed(2)} at ${timestamp}`;
    const eventId = db.createEvent({
      type: "movement_trigger",
      severity: "warning",
      title,
      body,
      metadata: {
        timestamp,
        threshold: settings.threshold,
        aggregateScore,
        nodes: triggeredNodes
      }
    });
    await sendPushToEnabledSubscriptions({
      type: "movement_trigger",
      severity: "warning",
      event_id: String(eventId),
      title,
      body,
      timestamp_ms: Date.now(),
      url: "/#movement-history"
    });
  }

  if (above !== settings.lastAbove) {
    db.setMovementTriggerLastAbove(above);
  }
}

/**
 * Fetches live status from the Python worker and performs all side effects that
 * should follow a successful poll: sync node records, append movement samples,
 * evaluate push triggers, and update the short-lived in-memory cache.
 */
async function fetchAndProcessWorkerStatus() {
  const response = await fetch(`${appConfig.workerInternalUrl}/internal/status`, {
    headers: { accept: "application/json" }
  });

  const contentType = response.headers.get("content-type") ?? "";
  if (!response.ok || !contentType.includes("application/json")) {
    return {
      ok: false,
      error: "worker status unavailable",
      worker_status: response.status
    };
  }

  const status = await response.json();
  syncWorkerNodes(status);
  await checkMovementTrigger(status);
  lastWorkerStatus = status;
  lastWorkerStatusAt = Date.now();
  return status;
}

/**
 * Finds the PWA database record for a logical ESP32 `device_id`.
 *
 * Routes use this registry record to discover the current LAN IP for proxying
 * commands to a node. Returning undefined lets the caller produce a clean 404.
 */
function findNodeByDeviceId(deviceId: number): NodeRecord | undefined {
  return db.listNodes().find((node) => node.deviceId === deviceId);
}

/**
 * Builds the base URL for direct ESP32 node requests.
 *
 * Only ordinary LAN IPv4 addresses are allowed. Empty, `0.0.0.0`, loopback, and
 * non-IP values are rejected so browser-triggered proxy routes cannot be abused
 * to call local services on the Pi or arbitrary hostnames.
 */
function nodeBaseUrl(node: NodeRecord) {
  if (!node.ip || isIP(node.ip) !== 4 || node.ip === "0.0.0.0" || node.ip.startsWith("127.")) {
    throw new Error(`node IP is unavailable or invalid: ${node.ip || "empty"}`);
  }
  return `http://${node.ip}`;
}

/**
 * Proxies a JSON request to a selected ESP32 node and normalizes the response.
 *
 * This is the shared transport for node status/config/calibration/identify
 * routes. It adds JSON headers, applies a short timeout so the web UI remains
 * responsive when a node is offline, and converts network failures into a 502
 * payload the browser can display.
 */
async function fetchNodeJson(deviceId: number, apiPath: string, init: RequestInit = {}) {
  const node = findNodeByDeviceId(deviceId);
  if (!node) {
    return { status: 404, body: { ok: false, error: "node not found" } };
  }

  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const targetUrl = `${nodeBaseUrl(node)}${apiPath}`;
    const response = await fetch(targetUrl, {
      ...init,
      signal: controller.signal,
      headers: {
        accept: "application/json",
        ...(init.body ? { "content-type": "application/json" } : {}),
        ...(init.headers ?? {})
      }
    });
    const contentType = response.headers.get("content-type") ?? "";
    const body = contentType.includes("application/json") ? await response.json() : { text: await response.text() };
    return { status: response.status, body };
  } catch (error: any) {
    const message = String(error?.message || "");
    return {
      status: 502,
      body: {
        ok: false,
        error: error?.name === "AbortError" ? "node request timed out" : "node request failed",
        detail: message
      }
    };
  } finally {
    clearTimeout(timeout);
  }
}

function safeCaptureId(value: unknown) {
  const captureId = String(value ?? "").trim();
  if (!CAPTURE_ID_RE.test(captureId)) {
    throw new Error("capture_id contains unsupported characters");
  }
  return captureId;
}

function createCaptureId(deviceId: number) {
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  return `${stamp}-node${deviceId.toString(16).padStart(2, "0")}`;
}

async function listCaptures() {
  await fs.mkdir(appConfig.captureDir, { recursive: true });
  const entries = await fs.readdir(appConfig.captureDir, { withFileTypes: true });
  const captures = [];
  for (const entry of entries) {
    if (!entry.isFile() || !entry.name.endsWith(".json")) {
      continue;
    }
    const filePath = path.join(appConfig.captureDir, entry.name);
    try {
      const metadata = JSON.parse(await fs.readFile(filePath, "utf8"));
      const captureId = safeCaptureId(metadata.capture_id ?? entry.name.slice(0, -5));
      const dataPath = path.join(appConfig.captureDir, `${captureId}.ndjson`);
      let dataBytes = 0;
      try {
        dataBytes = (await fs.stat(dataPath)).size;
      } catch {
        dataBytes = 0;
      }
      captures.push({
        ...metadata,
        capture_id: captureId,
        data_bytes: dataBytes,
        data_download_url: `/api/captures/${encodeURIComponent(captureId)}/download`,
        metadata_download_url: `/api/captures/${encodeURIComponent(captureId)}/metadata`
      });
    } catch {
      continue;
    }
  }
  captures.sort((a, b) => String(b.first_received_at ?? b.capture_id).localeCompare(String(a.first_received_at ?? a.capture_id)));
  return captures;
}

/**
 * Constructs the Fastify application and registers all HTTP routes.
 *
 * The function is exported for tests and tools, while `main()` below handles the
 * normal command-line startup. Route groups are organized by purpose: PWA shell
 * assets, health/version, worker status/history, push, admin state, ESP32 node
 * proxying, and lifecycle hooks.
 */
export function buildServer() {
  const server = Fastify({
    logger: true
  });

  // PWA shell routes. These are served with no-store headers because installed
  // PWAs and service workers need to discover version changes promptly. Static
  // files that are safe to cache are still served later through fastifyStatic.
  server.get("/", async (_request, reply) => {
    reply.type("text/html; charset=utf-8").header("Cache-Control", "no-store");
    return renderVersionedAsset("index.html");
  });

  server.get("/index.html", async (_request, reply) => {
    reply.type("text/html; charset=utf-8").header("Cache-Control", "no-store");
    return renderVersionedAsset("index.html");
  });

  server.get("/service-worker.js", async (_request, reply) => {
    reply.type("application/javascript; charset=utf-8").header("Cache-Control", "no-store");
    return renderVersionedAsset("service-worker.js");
  });

  server.get("/manifest.webmanifest", async (_request, reply) => {
    reply.type("application/manifest+json; charset=utf-8").header("Cache-Control", "no-store");
    return renderVersionedAsset("manifest.webmanifest");
  });

  server.get("/app-config.js", async (_request, reply) => {
    reply.type("application/javascript; charset=utf-8").header("Cache-Control", "no-store");
    return `window.ALARM_APP_CONFIG = ${JSON.stringify({ version: appConfig.version, build: appConfig.build })};`;
  });

  // Lightweight service probes. /api/healthz is for process supervision; the
  // richer /api/version response is used by browsers to decide whether a cached
  // PWA payload is stale and should be refreshed.
  server.get("/api/healthz", async () => ({
    ok: true,
    service: "esp32-alarm-pwa",
    version: appConfig.version
  }));

  server.get("/api/version", async () => ({
    version: appConfig.version,
    build: appConfig.build,
    mandatory: false,
    notes: "Persistent admin, VAPID, and PWA update enforcement"
  }));

  // Live system status. Prefer the short-lived worker cache when it is fresh;
  // otherwise poll the Python worker and let that poll update node/history state.
  server.get("/api/status", async (_request, reply) => {
    const status = Date.now() - lastWorkerStatusAt < 2500 && lastWorkerStatus ? lastWorkerStatus : await fetchAndProcessWorkerStatus();
    if (status?.ok === false) {
      reply.code(502);
      return status;
    }
    return status;
  });

  // Movement history and alert threshold configuration. These endpoints power
  // the aggregate chart, per-node charts, and Pi-side trigger line in the PWA.
  server.get<{ Querystring: { fromHours?: string; toHours?: string } }>("/api/history/movement", async (request) => {
    const fromHours = Number(request.query.fromHours ?? String(DEFAULT_HISTORY_HOURS));
    const toHours = Number(request.query.toHours ?? "0");
    const safeFromHours = Math.min(MAX_HISTORY_HOURS, Math.max(0, Number.isFinite(fromHours) ? fromHours : DEFAULT_HISTORY_HOURS));
    const safeToHours = Math.min(MAX_HISTORY_HOURS, Math.max(0, Number.isFinite(toHours) ? toHours : 0));
    return db.movementHistory(Math.max(safeFromHours, safeToHours), Math.min(safeFromHours, safeToHours));
  });

  server.get("/api/history/movement/trigger", async () => db.getMovementTriggerSettings());

  server.post<{ Body: { threshold?: number; enabled?: boolean } }>("/api/history/movement/trigger", async (request, reply) => {
    const threshold = Number(request.body?.threshold);
    if (!Number.isFinite(threshold)) {
      reply.code(400);
      return { ok: false, error: "threshold is required" };
    }
    db.saveMovementTriggerSettings({
      threshold,
      enabled: request.body?.enabled ?? true,
      lastAbove: false
    });
    return { ok: true, trigger: db.getMovementTriggerSettings() };
  });

  server.get("/api/captures", async () => ({
    ok: true,
    captures: await listCaptures()
  }));

  server.get<{ Params: { captureId: string } }>("/api/captures/:captureId/download", async (request, reply) => {
    const captureId = safeCaptureId(decodeURIComponent(request.params.captureId));
    const filePath = path.join(appConfig.captureDir, `${captureId}.ndjson`);
    const body = await fs.readFile(filePath, "utf8");
    reply
      .type("application/x-ndjson; charset=utf-8")
      .header("Content-Disposition", `attachment; filename="${captureId}.ndjson"`);
    return body;
  });

  server.get<{ Params: { captureId: string } }>("/api/captures/:captureId/metadata", async (request, reply) => {
    const captureId = safeCaptureId(decodeURIComponent(request.params.captureId));
    const filePath = path.join(appConfig.captureDir, `${captureId}.json`);
    const body = await fs.readFile(filePath, "utf8");
    reply
      .type("application/json; charset=utf-8")
      .header("Content-Disposition", `attachment; filename="${captureId}.json"`);
    return body;
  });

  // Browser push endpoints. VAPID public key is safe to expose; subscriptions
  // are stored server-side so later movement events can wake installed PWAs.
  server.get("/api/push/vapid-public-key", async () => ({
    configured: db.getVapidSettings().publicKey.length > 0,
    publicKey: db.getVapidSettings().publicKey
  }));

  server.post<{ Body: PushSubscriptionBody }>("/api/push/subscribe", async (request, reply) => {
    const body = request.body;
    if (!body?.endpoint || !body.keys?.p256dh || !body.keys?.auth) {
      reply.code(400);
      return { ok: false, error: "endpoint, keys.p256dh, and keys.auth are required" };
    }
    db.upsertPushSubscription({
      endpoint: body.endpoint,
      keys: { p256dh: body.keys.p256dh, auth: body.keys.auth },
      deviceName: body.deviceName,
      userId: body.userId
    });
    return { ok: true };
  });

  // Remove the exact browser endpoint supplied by the client. This mirrors the
  // Push API subscription object and avoids guessing which device is unsubscribing.
  server.post<{ Body: { endpoint?: string } }>("/api/push/unsubscribe", async (request, reply) => {
    if (!request.body?.endpoint) {
      reply.code(400);
      return { ok: false, error: "endpoint is required" };
    }
    db.deletePushSubscription(request.body.endpoint);
    return { ok: true };
  });

  // Manual push test used during setup. It creates an event record first so the
  // notification deep link points at a real event/history context.
  server.post<{ Body: { title?: string; body?: string; severity?: string } }>("/api/push/test", async (request) => {
    const eventId = db.createEvent({
      type: "push_test",
      severity: request.body?.severity ?? "info",
      title: request.body?.title ?? "ESP32 alarm push test",
      body: request.body?.body ?? "Push delivery is configured."
    });
    const result = await sendPushToEnabledSubscriptions({
      type: "push_test",
      severity: request.body?.severity ?? "info",
      event_id: String(eventId),
      title: request.body?.title ?? "ESP32 alarm push test",
      body: request.body?.body ?? "Push delivery is configured.",
      timestamp_ms: Date.now(),
      url: "/#events"
    });
    return { ok: true, eventId, ...result };
  });

  // Single admin bootstrap payload. The front end can refresh one endpoint and
  // repaint users, VAPID status, subscriptions, nodes, security flags, events,
  // and audit records without coordinating multiple independent requests.
  server.get("/api/admin/summary", async () => ({
    users: db.listUsers(),
    vapid: db.getVapidSettings({ includePrivateKey: true }),
    pushSubscriptions: db.listPushSubscriptions(),
    nodes: db.listNodes(),
    security: db.getSecuritySettings(),
    events: db.recentEvents(25),
    auditLog: db.auditLog(25)
  }));

  // ESP32 node proxy routes. The browser talks to the Pi, and the Pi talks to
  // the node over LAN. This avoids CORS issues and centralizes timeout/error
  // handling for nodes that are offline or changing IP address.
  server.get<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/status", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/status.json");
    reply.code(result.status);
    return result.body;
  });

  server.get<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/config", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/config");
    reply.code(result.status);
    return result.body;
  });

  server.post<{ Params: { deviceId: string }; Body: Record<string, unknown> }>(
    "/api/nodes/:deviceId/config",
    async (request, reply) => {
      const result = await fetchNodeJson(Number(request.params.deviceId), "/api/config", {
        method: "POST",
        body: JSON.stringify(request.body ?? {})
      });
      reply.code(result.status);
      return result.body;
    }
  );

  server.post<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/calibrate", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/calibrate", { method: "POST" });
    reply.code(result.status);
    return result.body;
  });

  server.post<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/identify", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/identify", { method: "POST" });
    reply.code(result.status);
    return result.body;
  });

  server.get<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/calibration", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/calibration");
    reply.code(result.status);
    return result.body;
  });

  server.post<{ Params: { deviceId: string }; Body: Record<string, unknown> }>(
    "/api/nodes/:deviceId/calibration",
    async (request, reply) => {
      const result = await fetchNodeJson(Number(request.params.deviceId), "/api/calibration", {
        method: "POST",
        body: JSON.stringify(request.body ?? {})
      });
      reply.code(result.status);
      return result.body;
    }
  );

  server.delete<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/calibration", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/calibration", { method: "DELETE" });
    reply.code(result.status);
    return result.body;
  });

  server.get<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/capture/status", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/capture/status");
    reply.code(result.status);
    return result.body;
  });

  server.post<{ Params: { deviceId: string }; Body: { duration_seconds?: number; mode?: string; label?: string } }>(
    "/api/nodes/:deviceId/capture/start",
    async (request, reply) => {
      const deviceId = Number(request.params.deviceId);
      const duration = Number(request.body?.duration_seconds ?? 30);
      const safeDuration = Math.min(300, Math.max(5, Number.isFinite(duration) ? duration : 30));
      const mode = request.body?.mode === "raw_csi" ? "raw_csi" : "features";
      const label = String(request.body?.label ?? "").slice(0, 60);
      const captureId = createCaptureId(deviceId);
      const result = await fetchNodeJson(deviceId, "/api/capture/start", {
        method: "POST",
        body: JSON.stringify({
          capture_id: captureId,
          duration_seconds: safeDuration,
          mode,
          label
        })
      });
      reply.code(result.status);
      return {
        ...(typeof result.body === "object" && result.body !== null ? result.body : {}),
        capture_id: captureId,
        download_url: `/api/captures/${encodeURIComponent(captureId)}/download`,
        metadata_url: `/api/captures/${encodeURIComponent(captureId)}/metadata`
      };
    }
  );

  server.post<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/capture/stop", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/capture/stop", { method: "POST" });
    reply.code(result.status);
    return result.body;
  });

  // User administration. These are currently local admin records; future login
  // enforcement can build on the same role and state fields.
  server.post<{ Body: { username?: string; displayName?: string; role?: string; state?: string } }>(
    "/api/admin/users",
    async (request, reply) => {
      const role = request.body?.role;
      if (!request.body?.username || !request.body.displayName || !validRole(role)) {
        reply.code(400);
        return { ok: false, error: "username, displayName, and valid role are required" };
      }
      const id = db.createUser({
        username: request.body.username,
        displayName: request.body.displayName,
        role,
        state: request.body.state
      });
      return { ok: true, id };
    }
  );

  server.put<{ Params: { id: string }; Body: { displayName?: string; role?: string; state?: string } }>(
    "/api/admin/users/:id",
    async (request, reply) => {
      const role = request.body?.role;
      if (!request.body?.displayName || !request.body.state || !validRole(role)) {
        reply.code(400);
        return { ok: false, error: "displayName, state, and valid role are required" };
      }
      db.updateUser(Number(request.params.id), {
        displayName: request.body.displayName,
        role,
        state: request.body.state
      });
      return { ok: true };
    }
  );

  server.delete<{ Params: { id: string } }>("/api/admin/users/:id", async (request) => {
    db.deleteUser(Number(request.params.id));
    return { ok: true };
  });

  // VAPID administration. Keys can either be pasted from another generator or
  // generated locally; saving keys immediately enables later push sends.
  server.post<{ Body: { publicKey?: string; privateKey?: string; subject?: string } }>(
    "/api/admin/vapid",
    async (request, reply) => {
      if (!request.body?.publicKey || !request.body.privateKey) {
        reply.code(400);
        return { ok: false, error: "publicKey and privateKey are required" };
      }
      db.saveVapidSettings({
        publicKey: request.body.publicKey,
        privateKey: request.body.privateKey,
        subject: request.body.subject || appConfig.vapidSubject
      });
      return { ok: true, vapid: db.getVapidSettings({ includePrivateKey: true }) };
    }
  );

  server.post("/api/admin/vapid/generate", async () => {
    const keys = webpush.generateVAPIDKeys();
    db.saveVapidSettings({
      publicKey: keys.publicKey,
      privateKey: keys.privateKey,
      subject: appConfig.vapidSubject
    });
    return { ok: true, vapid: db.getVapidSettings({ includePrivateKey: true }) };
  });

  // Persistent node registry controls. These settings are Pi-side metadata used
  // by dashboards and trigger decisions; live ESP32 telemetry updates IP/payload
  // but no longer overwrites a user-edited display name.
  server.put<{ Params: { id: string }; Body: { name?: string; expected?: boolean; active?: boolean } }>(
    "/api/admin/nodes/:id",
    async (request, reply) => {
      if (!request.body?.name || typeof request.body.expected !== "boolean" || typeof request.body.active !== "boolean") {
        reply.code(400);
        return { ok: false, error: "name, expected, and active are required" };
      }
      db.updateNode(Number(request.params.id), {
        name: request.body.name,
        expected: request.body.expected,
        active: request.body.active
      });
      return { ok: true };
    }
  );

  server.post<{ Params: { id: string }; Body: { active?: boolean } }>("/api/admin/nodes/:id/active", async (request, reply) => {
    if (typeof request.body?.active !== "boolean") {
      reply.code(400);
      return { ok: false, error: "active is required" };
    }
    db.setNodeActive(Number(request.params.id), request.body.active);
    return { ok: true };
  });

  server.post<{ Params: { id: string }; Body: { name?: string } }>("/api/admin/nodes/:id/name", async (request, reply) => {
    const name = String(request.body?.name ?? "").trim();
    if (!name) {
      reply.code(400);
      return { ok: false, error: "name is required" };
    }
    db.renameNode(Number(request.params.id), name);
    return { ok: true };
  });

  // Security flags are stored before full auth enforcement exists so the UI and
  // audit trail can track intended deployment posture.
  server.post<{ Body: SecuritySettings }>("/api/admin/security", async (request) => {
    db.saveSecuritySettings(request.body);
    return { ok: true, security: db.getSecuritySettings() };
  });

  // On-demand SQLite backup. The database layer chooses the exact checkpoint and
  // copy behavior so this route only needs to expose the resulting path.
  server.post("/api/admin/backup", async () => ({
    ok: true,
    path: db.backup(appConfig.backupDir)
  }));

  // Static file fallback for cacheable assets such as app.js, CSS, icons, and
  // images. Version-sensitive shell files above are registered first, so those
  // custom handlers win before this generic static plugin.
  server.register(fastifyStatic, {
    root: publicDir,
    prefix: "/"
  });

  // Background worker poll. This keeps node records/history fresh even when no
  // browser is actively asking for /api/status, and it also drives movement
  // trigger push notifications. The timer is cleared when Fastify shuts down.
  let workerPollTimer: NodeJS.Timeout | undefined;
  server.addHook("onReady", async () => {
    workerPollTimer = setInterval(() => {
      fetchAndProcessWorkerStatus().catch((error) => server.log.warn({ error }, "worker background poll failed"));
    }, 5000);
  });
  server.addHook("onClose", async () => {
    if (workerPollTimer) {
      clearInterval(workerPollTimer);
    }
  });

  return server;
}

/**
 * Starts the HTTP server for normal production/development execution.
 *
 * Keeping this separate from `buildServer()` lets tests import and exercise the
 * configured Fastify app without binding a port.
 */
async function main() {
  const server = buildServer();
  await server.listen({ host: appConfig.host, port: appConfig.port });
}

// Only listen when this file is run directly. Importing `buildServer()` from a
// future test harness or management command should not start a network listener.
if (require.main === module) {
  main().catch((error) => {
    console.error(error);
    process.exit(1);
  });
}
