"""aiohttp application for the LAN-facing ESP32 telemetry worker.

Purpose:
- Receives compact JSON telemetry from ESP32 CSI nodes on the local network,
  normally at `/espdata`.
- Keeps the latest node state in memory through `state.NodeStore`.
- Exposes `/internal/status` for the TypeScript PWA service, which persists
  movement history and renders the user-facing dashboard.
- Optionally emits UDP probe packets to stimulate CSI traffic during
  experiments, using values loaded by `config.load_config()`.

Interactions:
- ESP32 firmware posts status JSON here using its configured `pi_ip`, `pi_port`,
  and `pi_api_path`.
- The PWA service in `raspberrypi/PWA/src/server.ts` polls `/internal/status`
  and proxies user actions directly to ESP32 nodes.
- `views.py` renders the simple worker landing page for local diagnostics.
"""

from __future__ import annotations

import asyncio
import socket
from contextlib import suppress
from typing import Any, Optional

from aiohttp import web

from .config import WorkerConfig, load_config
from .state import NodeStore
from .views import render_index_html


async def udp_probe_loop(config: WorkerConfig) -> None:
    """Send periodic UDP probe payloads when probe traffic is enabled.

    Probe traffic is experimental. It can increase the CSI packet rate seen by
    ESP32 nodes, but it can also perturb calibration, so it is opt-in and should
    match the conditions used during stillness calibration.
    """
    if not config.udp_probe_enabled or not config.udp_probe_target_ip:
        return

    target = (config.udp_probe_target_ip, config.udp_probe_target_port)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        while True:
            sock.sendto(config.udp_probe_payload, target)
            await asyncio.sleep(max(1.0, config.udp_probe_interval_seconds))


def make_app(config: Optional[WorkerConfig] = None) -> web.Application:
    """Create and configure the aiohttp worker application.

    Tests can pass an explicit config. Normal startup loads the shared `.env`.
    The `NodeStore` is attached to the app so every request handler sees the same
    live node registry.
    """
    config = config or load_config()
    store = NodeStore()
    app = web.Application()
    app["config"] = config
    app["store"] = store

    async def healthz(_: web.Request) -> web.Response:
        """Return a process health response for service supervisors."""
        return web.json_response(
            {
                "ok": True,
                "service": "esp32-alarm-worker",
                "version": config.version,
            }
        )

    async def index(_: web.Request) -> web.Response:
        """Render a small human-readable status page for local diagnostics."""
        status_data = store.status(config.version)
        return web.Response(text=render_index_html(status_data), content_type="text/html")

    async def receive_telemetry(request: web.Request) -> web.Response:
        """Accept one ESP32 telemetry post and update the in-memory node store."""
        try:
            payload: dict[str, Any] = await request.json()
            remote_ip = request.remote or ""
            snapshot = store.upsert(payload, remote_ip)
        except ValueError as exc:
            return web.json_response({"ok": False, "error": str(exc)}, status=400)
        except Exception:
            return web.json_response({"ok": False, "error": "invalid JSON telemetry"}, status=400)

        return web.json_response(
            {
                "ok": True,
                "ack": "espdata_received",
                "received": True,
                "device_id": snapshot.device_id,
                "node": snapshot.to_dict(),
            }
        )

    async def status(_: web.Request) -> web.Response:
        """Return the full live worker status consumed by the PWA service."""
        return web.json_response(store.status(config.version))

    async def node(request: web.Request) -> web.Response:
        """Return one known node snapshot by device ID."""
        device_id = int(request.match_info["device_id"])
        snapshot = store.get(device_id)
        if snapshot is None:
            return web.json_response({"ok": False, "error": "node not found"}, status=404)
        return web.json_response({"ok": True, "node": snapshot.to_dict()})

    async def start_background(app_: web.Application) -> None:
        """Start optional background tasks after aiohttp is ready."""
        task = asyncio.create_task(udp_probe_loop(config))
        app_["udp_probe_task"] = task

    async def stop_background(app_: web.Application) -> None:
        """Cancel background tasks during graceful shutdown."""
        task = app_.get("udp_probe_task")
        if task:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task

    app.router.add_get("/", index)
    app.router.add_get("/healthz", healthz)
    app.router.add_post(config.telemetry_path, receive_telemetry)
    app.router.add_get("/internal/status", status)
    app.router.add_get("/internal/nodes/{device_id:\\d+}", node)
    app.on_startup.append(start_background)
    app.on_cleanup.append(stop_background)
    return app


def main() -> None:
    """Load configuration and run the worker as a standalone process."""
    config = load_config()
    web.run_app(make_app(config), host=config.host, port=config.port)


if __name__ == "__main__":
    main()
