import path from "node:path";
import fs from "node:fs/promises";
import { isIP } from "node:net";
import Fastify from "fastify";
import fastifyStatic from "@fastify/static";
import webpush from "web-push";

import { loadConfig } from "./config";
import { AlarmDatabase, NodeRecord, SecuritySettings, UserRole } from "./db";

const appConfig = loadConfig();
const publicDir = path.resolve(__dirname, "..", "public");
const db = new AlarmDatabase(appConfig.databasePath, appConfig.sqlCipherKey);
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

function validRole(role: unknown): role is UserRole {
  return role === "owner" || role === "admin" || role === "resident" || role === "viewer";
}

function configureWebPush() {
  const vapid = db.getVapidPrivateSettings();
  if (vapid.publicKey && vapid.privateKey) {
    webpush.setVapidDetails(vapid.subject || appConfig.vapidSubject, vapid.publicKey, vapid.privateKey);
    return true;
  }
  return false;
}

function normalizePushSubscription(row: { endpoint: string; p256dh: string; auth: string }) {
  return {
    endpoint: row.endpoint,
    keys: {
      p256dh: row.p256dh,
      auth: row.auth
    }
  };
}

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

async function renderVersionedAsset(fileName: string) {
  const filePath = path.join(publicDir, fileName);
  const text = await fs.readFile(filePath, "utf8");
  if (fileName === "index.html") {
    return text.replaceAll("?v=0.0.1", `?v=${appConfig.version}`);
  }
  if (fileName === "service-worker.js") {
    return text.replace('const APP_VERSION = "0.0.1";', `const APP_VERSION = ${JSON.stringify(appConfig.version)};`);
  }
  return text.replaceAll("?v=0.0.1", `?v=${appConfig.version}`);
}

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

function findNodeByDeviceId(deviceId: number): NodeRecord | undefined {
  return db.listNodes().find((node) => node.deviceId === deviceId);
}

function nodeBaseUrl(node: NodeRecord) {
  if (!node.ip || isIP(node.ip) !== 4 || node.ip === "0.0.0.0" || node.ip.startsWith("127.")) {
    throw new Error(`node IP is unavailable or invalid: ${node.ip || "empty"}`);
  }
  return `http://${node.ip}`;
}

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

export function buildServer() {
  const server = Fastify({
    logger: true
  });

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

  server.get("/api/status", async (_request, reply) => {
    const status = Date.now() - lastWorkerStatusAt < 2500 && lastWorkerStatus ? lastWorkerStatus : await fetchAndProcessWorkerStatus();
    if (status?.ok === false) {
      reply.code(502);
      return status;
    }
    return status;
  });

  server.get<{ Querystring: { fromHours?: string; toHours?: string } }>("/api/history/movement", async (request) => {
    const fromHours = Number(request.query.fromHours ?? "6");
    const toHours = Number(request.query.toHours ?? "0");
    return db.movementHistory(Number.isFinite(fromHours) ? fromHours : 6, Number.isFinite(toHours) ? toHours : 0);
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

  server.post<{ Body: { endpoint?: string } }>("/api/push/unsubscribe", async (request, reply) => {
    if (!request.body?.endpoint) {
      reply.code(400);
      return { ok: false, error: "endpoint is required" };
    }
    db.deletePushSubscription(request.body.endpoint);
    return { ok: true };
  });

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

  server.get("/api/admin/summary", async () => ({
    users: db.listUsers(),
    vapid: db.getVapidSettings({ includePrivateKey: true }),
    pushSubscriptions: db.listPushSubscriptions(),
    nodes: db.listNodes(),
    security: db.getSecuritySettings(),
    events: db.recentEvents(25),
    auditLog: db.auditLog(25)
  }));

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

  server.delete<{ Params: { deviceId: string } }>("/api/nodes/:deviceId/calibration", async (request, reply) => {
    const result = await fetchNodeJson(Number(request.params.deviceId), "/api/calibration", { method: "DELETE" });
    reply.code(result.status);
    return result.body;
  });

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

  server.post<{ Body: SecuritySettings }>("/api/admin/security", async (request) => {
    db.saveSecuritySettings(request.body);
    return { ok: true, security: db.getSecuritySettings() };
  });

  server.post("/api/admin/backup", async () => ({
    ok: true,
    path: db.backup(appConfig.backupDir)
  }));

  server.register(fastifyStatic, {
    root: publicDir,
    prefix: "/"
  });

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

async function main() {
  const server = buildServer();
  await server.listen({ host: appConfig.host, port: appConfig.port });
}

if (require.main === module) {
  main().catch((error) => {
    console.error(error);
    process.exit(1);
  });
}
