import path from "node:path";
import { config as loadDotenv } from "dotenv";

loadDotenv({ path: path.resolve(__dirname, "..", "..", ".env") });

export interface AppConfig {
  version: string;
  host: string;
  port: number;
  workerInternalUrl: string;
  vapidPublicKey: string;
}

export function loadConfig(): AppConfig {
  return {
    version: process.env.APP_VERSION ?? "0.0.1",
    host: process.env.PWA_HOST ?? "127.0.0.1",
    port: Number(process.env.PWA_PORT ?? "3015"),
    workerInternalUrl: process.env.WORKER_INTERNAL_URL ?? "http://127.0.0.1:3005",
    vapidPublicKey: process.env.VAPID_PUBLIC_KEY ?? ""
  };
}
