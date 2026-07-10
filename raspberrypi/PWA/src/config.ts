import fs from "node:fs";
import path from "node:path";
import { config as loadDotenv } from "dotenv";

loadDotenv({ path: path.resolve(__dirname, "..", "..", ".env") });

export interface AppConfig {
  version: string;
  build: string | null;
  host: string;
  port: number;
  workerInternalUrl: string;
  databasePath: string;
  sqlCipherKey: string;
  backupDir: string;
  vapidSubject: string;
}

function loadVersion(): string {
  return fs.readFileSync(path.resolve(__dirname, "..", "..", "VERSION"), "utf8").trim();
}

export function loadConfig(): AppConfig {
  return {
    version: loadVersion(),
    build: process.env.APP_BUILD ?? null,
    host: process.env.PWA_HOST ?? "127.0.0.1",
    port: Number(process.env.PWA_PORT ?? "3015"),
    workerInternalUrl: process.env.WORKER_INTERNAL_URL ?? "http://127.0.0.1:3005",
    databasePath: process.env.ALARM_DATABASE_PATH ?? path.resolve(__dirname, "..", "..", "data", "alarm.sqlite"),
    sqlCipherKey: process.env.SQLCIPHER_KEY ?? "",
    backupDir: process.env.ALARM_BACKUP_DIR ?? path.resolve(__dirname, "..", "..", "backups"),
    vapidSubject: process.env.VAPID_SUBJECT ?? "mailto:admin@example.local"
  };
}
