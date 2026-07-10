import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

export type UserRole = "owner" | "admin" | "resident" | "viewer";

export interface AdminUser {
  id: number;
  username: string;
  displayName: string;
  role: UserRole;
  state: string;
  createdAt: string;
}

export interface VapidSettings {
  publicKey: string;
  privateKey?: string;
  privateKeyConfigured: boolean;
  subject: string;
  updatedAt: string | null;
}

export interface PushSubscriptionRecord {
  id: number;
  userId: number | null;
  deviceName: string;
  endpoint: string;
  enabled: boolean;
  failureCount: number;
  lastError: string | null;
  createdAt: string;
  updatedAt: string;
}

export interface NodeRecord {
  id: number;
  deviceId: number;
  name: string;
  ip: string;
  expected: boolean;
  active: boolean;
  lastSeenAt: string | null;
  payload: unknown;
}

export interface MovementScoreSample {
  deviceId: number;
  nodeName: string;
  score: number;
  movementDetected: boolean;
  sampledAt: string;
}

export interface MovementTriggerSettings {
  threshold: number;
  enabled: boolean;
  lastAbove: boolean;
  updatedAt: string | null;
}

export interface SecuritySettings {
  cloudflareAccessExpected: boolean;
  appSessionsEnabled: boolean;
  csrfProtectionEnabled: boolean;
  loginRateLimitEnabled: boolean;
  auditLoggingEnabled: boolean;
  localRecoveryOnly: boolean;
}

const DEFAULT_SECURITY_SETTINGS: SecuritySettings = {
  cloudflareAccessExpected: true,
  appSessionsEnabled: false,
  csrfProtectionEnabled: false,
  loginRateLimitEnabled: false,
  auditLoggingEnabled: true,
  localRecoveryOnly: true
};

const migrations = [
  `
  CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('owner','admin','resident','viewer')),
    state TEXT NOT NULL DEFAULT 'active',
    password_hash TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash TEXT NOT NULL UNIQUE,
    device_name TEXT,
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen_at TEXT
  );

  CREATE TABLE IF NOT EXISTS vapid_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    public_key TEXT NOT NULL DEFAULT '',
    private_key TEXT NOT NULL DEFAULT '',
    subject TEXT NOT NULL DEFAULT 'mailto:admin@example.local',
    updated_at TEXT
  );

  CREATE TABLE IF NOT EXISTS push_subscriptions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
    device_name TEXT NOT NULL DEFAULT 'Browser',
    endpoint TEXT NOT NULL UNIQUE,
    p256dh TEXT NOT NULL,
    auth TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1,
    failure_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id INTEGER NOT NULL UNIQUE,
    name TEXT NOT NULL,
    ip TEXT NOT NULL,
    expected INTEGER NOT NULL DEFAULT 1,
    active INTEGER NOT NULL DEFAULT 1,
    last_seen_at TEXT,
    payload_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    type TEXT NOT NULL,
    severity TEXT NOT NULL DEFAULT 'info',
    title TEXT NOT NULL,
    body TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS calibrations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id INTEGER REFERENCES nodes(id) ON DELETE CASCADE,
    label TEXT NOT NULL,
    baseline_json TEXT NOT NULL DEFAULT '{}',
    model_metadata_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    actor_user_id INTEGER REFERENCES users(id) ON DELETE SET NULL,
    action TEXT NOT NULL,
    target TEXT,
    metadata_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS security_settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS movement_scores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id INTEGER NOT NULL,
    node_name TEXT NOT NULL,
    score REAL NOT NULL,
    movement_detected INTEGER NOT NULL DEFAULT 0,
    sampled_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
  );

  CREATE INDEX IF NOT EXISTS idx_movement_scores_sampled_at ON movement_scores(sampled_at);
  CREATE INDEX IF NOT EXISTS idx_movement_scores_device_sampled ON movement_scores(device_id, sampled_at);

  CREATE TABLE IF NOT EXISTS movement_trigger_settings (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    threshold REAL NOT NULL DEFAULT 3.0,
    enabled INTEGER NOT NULL DEFAULT 1,
    last_above INTEGER NOT NULL DEFAULT 0,
    updated_at TEXT
  );

  INSERT OR IGNORE INTO vapid_settings (id) VALUES (1);
  INSERT OR IGNORE INTO movement_trigger_settings (id) VALUES (1);
  `
];

function boolToDb(value: boolean): number {
  return value ? 1 : 0;
}

function dbToBool(value: number | boolean): boolean {
  return value === 1 || value === true;
}

function readJson(raw: string): unknown {
  try {
    return JSON.parse(raw);
  } catch {
    return {};
  }
}

function toNumber(value: unknown): number | null {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function toDetected(value: unknown): boolean {
  return value === true || value === 1 || value === "true" || value === "yes";
}

function isoHoursAgo(hours: number): string {
  return new Date(Date.now() - Math.max(0, hours) * 3600000).toISOString().slice(0, 19).replace("T", " ");
}

export class AlarmDatabase {
  private readonly db: Database.Database;
  private readonly dbPath: string;

  constructor(dbPath: string, sqlCipherKey: string) {
    this.dbPath = dbPath;
    fs.mkdirSync(path.dirname(dbPath), { recursive: true });
    this.db = new Database(dbPath);
    this.db.pragma("journal_mode = WAL");
    this.db.pragma("foreign_keys = ON");
    if (sqlCipherKey) {
      this.db.pragma(`key = '${sqlCipherKey.replaceAll("'", "''")}'`);
    }
    this.migrate();
  }

  migrate() {
    for (const migration of migrations) {
      this.db.exec(migration);
    }
    this.ensureSecuritySettings();
    this.ensureOwnerPlaceholder();
  }

  listUsers(): AdminUser[] {
    return this.db
      .prepare(
        "SELECT id, username, display_name AS displayName, role, state, created_at AS createdAt FROM users ORDER BY id"
      )
      .all() as AdminUser[];
  }

  createUser(input: { username: string; displayName: string; role: UserRole; state?: string }) {
    const info = this.db
      .prepare("INSERT INTO users (username, display_name, role, state) VALUES (?, ?, ?, ?)")
      .run(input.username, input.displayName, input.role, input.state ?? "active");
    this.audit("user.create", `user:${info.lastInsertRowid}`, { username: input.username, role: input.role });
    return info.lastInsertRowid;
  }

  updateUser(id: number, input: { displayName: string; role: UserRole; state: string }) {
    this.db
      .prepare("UPDATE users SET display_name = ?, role = ?, state = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?")
      .run(input.displayName, input.role, input.state, id);
    this.audit("user.update", `user:${id}`, input);
  }

  deleteUser(id: number) {
    this.db.prepare("DELETE FROM users WHERE id = ?").run(id);
    this.audit("user.delete", `user:${id}`);
  }

  getVapidSettings(options: { includePrivateKey?: boolean } = {}): VapidSettings {
    const row = this.db
      .prepare("SELECT public_key AS publicKey, private_key AS privateKey, subject, updated_at AS updatedAt FROM vapid_settings WHERE id = 1")
      .get() as { publicKey: string; privateKey: string; subject: string; updatedAt: string | null };
    return {
      publicKey: row.publicKey,
      privateKey: options.includePrivateKey ? row.privateKey : undefined,
      privateKeyConfigured: row.privateKey.length > 0,
      subject: row.subject,
      updatedAt: row.updatedAt
    };
  }

  getVapidPrivateSettings() {
    return this.db
      .prepare("SELECT public_key AS publicKey, private_key AS privateKey, subject FROM vapid_settings WHERE id = 1")
      .get() as { publicKey: string; privateKey: string; subject: string };
  }

  saveVapidSettings(input: { publicKey: string; privateKey: string; subject: string }) {
    this.db
      .prepare(
        "UPDATE vapid_settings SET public_key = ?, private_key = ?, subject = ?, updated_at = CURRENT_TIMESTAMP WHERE id = 1"
      )
      .run(input.publicKey, input.privateKey, input.subject);
    this.audit("vapid.update", "vapid_settings");
  }

  listPushSubscriptions(): PushSubscriptionRecord[] {
    return this.db
      .prepare(
        `SELECT id, user_id AS userId, device_name AS deviceName, endpoint, enabled, failure_count AS failureCount,
          last_error AS lastError, created_at AS createdAt, updated_at AS updatedAt
         FROM push_subscriptions ORDER BY updated_at DESC`
      )
      .all()
      .map((row: any) => ({ ...row, enabled: dbToBool(row.enabled) }));
  }

  upsertPushSubscription(input: {
    userId?: number | null;
    deviceName?: string;
    endpoint: string;
    keys: { p256dh: string; auth: string };
  }) {
    this.db
      .prepare(
        `INSERT INTO push_subscriptions (user_id, device_name, endpoint, p256dh, auth)
         VALUES (?, ?, ?, ?, ?)
         ON CONFLICT(endpoint) DO UPDATE SET
          user_id = excluded.user_id,
          device_name = excluded.device_name,
          p256dh = excluded.p256dh,
          auth = excluded.auth,
          enabled = 1,
          last_error = NULL,
          updated_at = CURRENT_TIMESTAMP`
      )
      .run(input.userId ?? null, input.deviceName ?? "Browser", input.endpoint, input.keys.p256dh, input.keys.auth);
    this.audit("push.subscribe", "push_subscription", { endpoint: input.endpoint });
  }

  disablePushSubscription(endpoint: string, error?: string) {
    this.db
      .prepare(
        `UPDATE push_subscriptions
         SET enabled = 0, failure_count = failure_count + 1, last_error = ?, updated_at = CURRENT_TIMESTAMP
         WHERE endpoint = ?`
      )
      .run(error ?? "disabled", endpoint);
  }

  deletePushSubscription(endpoint: string) {
    this.db.prepare("DELETE FROM push_subscriptions WHERE endpoint = ?").run(endpoint);
    this.audit("push.unsubscribe", "push_subscription", { endpoint });
  }

  listEnabledPushSubscriptions() {
    return this.db
      .prepare("SELECT endpoint, p256dh, auth FROM push_subscriptions WHERE enabled = 1")
      .all() as Array<{ endpoint: string; p256dh: string; auth: string }>;
  }

  listNodes(): NodeRecord[] {
    return this.db
      .prepare(
        `SELECT id, device_id AS deviceId, name, ip, expected, active, last_seen_at AS lastSeenAt, payload_json AS payloadJson
         FROM nodes ORDER BY device_id`
      )
      .all()
      .map((row: any) => ({
        id: row.id,
        deviceId: row.deviceId,
        name: row.name,
        ip: row.ip,
        expected: dbToBool(row.expected),
        active: dbToBool(row.active),
        lastSeenAt: row.lastSeenAt,
        payload: readJson(row.payloadJson)
      }));
  }

  upsertNode(input: { deviceId: number; name: string; ip: string; active: boolean; payload: unknown }) {
    this.db
      .prepare(
        `INSERT INTO nodes (device_id, name, ip, active, last_seen_at, payload_json)
         VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP, ?)
         ON CONFLICT(device_id) DO UPDATE SET
          name = excluded.name,
          ip = excluded.ip,
          last_seen_at = CURRENT_TIMESTAMP,
          payload_json = excluded.payload_json,
          updated_at = CURRENT_TIMESTAMP`
      )
      .run(input.deviceId, input.name, input.ip, boolToDb(input.active), JSON.stringify(input.payload ?? {}));
  }

  recordMovementScores(nodes: Array<{ device_id: unknown; name: unknown; payload?: Record<string, unknown> }>) {
    const insert = this.db.prepare(
      "INSERT INTO movement_scores (device_id, node_name, score, movement_detected, sampled_at) VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)"
    );
    const transaction = this.db.transaction(() => {
      for (const node of nodes) {
        const deviceId = toNumber(node.device_id);
        const payload = node.payload ?? {};
        const score = toNumber(payload.movement_score);
        if (deviceId === null || score === null) {
          continue;
        }
        insert.run(deviceId, String(node.name ?? `Movement${deviceId.toString(16).padStart(2, "0")}`), score, boolToDb(toDetected(payload.movement_detected)));
      }
    });
    transaction();
  }

  movementHistory(hoursFrom: number, hoursTo: number) {
    const fromHours = Math.max(hoursFrom, hoursTo);
    const toHours = Math.min(hoursFrom, hoursTo);
    const fromIso = isoHoursAgo(fromHours);
    const toIso = isoHoursAgo(toHours);
    const samples = this.db
      .prepare(
        `SELECT device_id AS deviceId, node_name AS nodeName, score, movement_detected AS movementDetected,
          sampled_at AS sampledAt
         FROM movement_scores
         WHERE sampled_at >= ? AND sampled_at <= ?
         ORDER BY sampled_at ASC, device_id ASC`
      )
      .all(fromIso, toIso)
      .map((row: any) => ({ ...row, movementDetected: dbToBool(row.movementDetected) })) as MovementScoreSample[];
    const maxHoursRow = this.db
      .prepare("SELECT MIN(sampled_at) AS firstSampleAt FROM movement_scores")
      .get() as { firstSampleAt: string | null };
    const availableHours = maxHoursRow.firstSampleAt
      ? Math.max(0, (Date.now() - Date.parse(`${maxHoursRow.firstSampleAt.replace(" ", "T")}Z`)) / 3600000)
      : 0;
    return {
      range: { fromHours, toHours, availableHours },
      samples
    };
  }

  getMovementTriggerSettings(): MovementTriggerSettings {
    const row = this.db
      .prepare(
        "SELECT threshold, enabled, last_above AS lastAbove, updated_at AS updatedAt FROM movement_trigger_settings WHERE id = 1"
      )
      .get() as { threshold: number; enabled: number; lastAbove: number; updatedAt: string | null };
    return {
      threshold: row.threshold,
      enabled: dbToBool(row.enabled),
      lastAbove: dbToBool(row.lastAbove),
      updatedAt: row.updatedAt
    };
  }

  saveMovementTriggerSettings(input: { threshold: number; enabled: boolean; lastAbove?: boolean }) {
    const current = this.getMovementTriggerSettings();
    this.db
      .prepare(
        `UPDATE movement_trigger_settings
         SET threshold = ?, enabled = ?, last_above = ?, updated_at = CURRENT_TIMESTAMP
         WHERE id = 1`
      )
      .run(input.threshold, boolToDb(input.enabled), boolToDb(input.lastAbove ?? current.lastAbove));
    this.audit("movement_trigger.update", "movement_trigger_settings", input);
  }

  setMovementTriggerLastAbove(lastAbove: boolean) {
    this.db
      .prepare("UPDATE movement_trigger_settings SET last_above = ?, updated_at = CURRENT_TIMESTAMP WHERE id = 1")
      .run(boolToDb(lastAbove));
  }

  updateNode(id: number, input: { name: string; expected: boolean; active: boolean }) {
    this.db
      .prepare("UPDATE nodes SET name = ?, expected = ?, active = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?")
      .run(input.name, boolToDb(input.expected), boolToDb(input.active), id);
    this.audit("node.update", `node:${id}`, input);
  }

  setNodeActive(id: number, active: boolean) {
    this.db.prepare("UPDATE nodes SET active = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?").run(boolToDb(active), id);
    this.audit("node.active", `node:${id}`, { active });
  }

  renameNode(id: number, name: string) {
    this.db.prepare("UPDATE nodes SET name = ?, updated_at = CURRENT_TIMESTAMP WHERE id = ?").run(name, id);
    this.audit("node.rename", `node:${id}`, { name });
  }

  createEvent(input: { type: string; severity: string; title: string; body?: string; metadata?: unknown }) {
    const info = this.db
      .prepare("INSERT INTO events (type, severity, title, body, metadata_json) VALUES (?, ?, ?, ?, ?)")
      .run(input.type, input.severity, input.title, input.body ?? "", JSON.stringify(input.metadata ?? {}));
    return Number(info.lastInsertRowid);
  }

  recentEvents(limit = 50) {
    return this.db
      .prepare(
        "SELECT id, type, severity, title, body, metadata_json AS metadataJson, created_at AS createdAt FROM events ORDER BY id DESC LIMIT ?"
      )
      .all(limit)
      .map((row: any) => ({ ...row, metadata: readJson(row.metadataJson), metadataJson: undefined }));
  }

  getSecuritySettings(): SecuritySettings {
    const rows = this.db.prepare("SELECT key, value FROM security_settings").all() as Array<{ key: string; value: string }>;
    const settings: Record<string, boolean> = {};
    for (const row of rows) {
      settings[row.key] = row.value === "true";
    }
    return { ...DEFAULT_SECURITY_SETTINGS, ...settings };
  }

  saveSecuritySettings(settings: SecuritySettings) {
    const statement = this.db.prepare(
      `INSERT INTO security_settings (key, value, updated_at) VALUES (?, ?, CURRENT_TIMESTAMP)
       ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = CURRENT_TIMESTAMP`
    );
    const transaction = this.db.transaction(() => {
      for (const [key, value] of Object.entries(settings)) {
        statement.run(key, String(value));
      }
    });
    transaction();
    this.audit("security.update", "security_settings", settings);
  }

  audit(action: string, target?: string, metadata?: unknown) {
    this.db
      .prepare("INSERT INTO audit_log (action, target, metadata_json) VALUES (?, ?, ?)")
      .run(action, target ?? null, JSON.stringify(metadata ?? {}));
  }

  auditLog(limit = 100) {
    return this.db
      .prepare(
        "SELECT id, action, target, metadata_json AS metadataJson, created_at AS createdAt FROM audit_log ORDER BY id DESC LIMIT ?"
      )
      .all(limit)
      .map((row: any) => ({ ...row, metadata: readJson(row.metadataJson), metadataJson: undefined }));
  }

  backup(targetDir: string) {
    fs.mkdirSync(targetDir, { recursive: true });
    const stamp = new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
    const backupPath = path.join(targetDir, `alarm-${stamp}.sqlite`);
    this.db.prepare("PRAGMA wal_checkpoint(TRUNCATE)").run();
    fs.copyFileSync(this.dbPath, backupPath);
    this.audit("database.backup", backupPath);
    return backupPath;
  }

  private ensureSecuritySettings() {
    const statement = this.db.prepare("INSERT OR IGNORE INTO security_settings (key, value) VALUES (?, ?)");
    for (const [key, value] of Object.entries(DEFAULT_SECURITY_SETTINGS)) {
      statement.run(key, String(value));
    }
  }

  private ensureOwnerPlaceholder() {
    const count = this.db.prepare("SELECT COUNT(*) AS count FROM users").get() as { count: number };
    if (count.count === 0) {
      this.createUser({
        username: "owner",
        displayName: "Owner",
        role: "owner",
        state: "setup-required"
      });
    }
  }
}
