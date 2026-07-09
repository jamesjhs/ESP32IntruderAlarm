import path from "node:path";
import Fastify from "fastify";
import fastifyStatic from "@fastify/static";

import { loadConfig } from "./config";

const appConfig = loadConfig();

export function buildServer() {
  const server = Fastify({
    logger: true
  });

  server.register(fastifyStatic, {
    root: path.resolve(__dirname, "..", "public"),
    prefix: "/"
  });

  server.get("/api/healthz", async () => ({
    ok: true,
    service: "esp32-alarm-pwa",
    version: appConfig.version
  }));

  server.get("/api/version", async () => ({
    version: appConfig.version,
    build: null,
    mandatory: false,
    notes: "Initial Raspberry Pi server-side scaffold"
  }));

  server.get("/api/status", async (_request, reply) => {
    const response = await fetch(`${appConfig.workerInternalUrl}/internal/status`, {
      headers: { accept: "application/json" }
    });

    const contentType = response.headers.get("content-type") ?? "";
    if (!response.ok || !contentType.includes("application/json")) {
      reply.code(502);
      return {
        ok: false,
        error: "worker status unavailable",
        worker_status: response.status
      };
    }

    return response.json();
  });

  server.get("/api/push/vapid-public-key", async () => ({
    configured: appConfig.vapidPublicKey.length > 0,
    publicKey: appConfig.vapidPublicKey
  }));

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
