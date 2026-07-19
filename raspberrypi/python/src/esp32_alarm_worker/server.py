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
import json
import re
import shutil
import socket
from contextlib import suppress
from datetime import datetime, timezone
from ipaddress import IPv4Address, IPv4Network, ip_address
from typing import Any, Optional

from aiohttp import web

from .config import WorkerConfig, load_config
from .state import NodeStore
from .views import render_index_html


CAPTURE_ID_RE = re.compile(r"^[A-Za-z0-9_. -]{1,40}$")
MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")
NMAP_REPORT_RE = re.compile(r"^Nmap scan report for (?:(?P<name>.+) \((?P<named_ip>[0-9.]+)\)|(?P<plain_ip>[0-9.]+))$")
NMAP_MAC_RE = re.compile(r"^MAC Address: (?P<mac>[0-9A-Fa-f:]{17})(?: \((?P<vendor>.*)\))?$")


def safe_capture_id(value: object) -> str:
    """Validate a Pi-generated capture id before using it as a filename stem."""
    capture_id = str(value or "").strip()
    if not CAPTURE_ID_RE.fullmatch(capture_id):
        raise ValueError("capture_id contains unsupported characters")
    return capture_id


def capture_paths(config: WorkerConfig, capture_id: str) -> tuple[Any, Any]:
    """Return the NDJSON data path and JSON metadata path for one capture."""
    config.capture_dir.mkdir(parents=True, exist_ok=True)
    data_path = config.capture_dir / f"{capture_id}.ndjson"
    meta_path = config.capture_dir / f"{capture_id}.json"
    return data_path, meta_path


def normalize_mac(value: object) -> str:
    """Return a normalized MAC address when the text is valid."""
    text = str(value or "").strip().upper().replace("-", ":")
    return text if MAC_RE.fullmatch(text) else ""


async def ip_neigh_mac_map() -> dict[str, dict[str, str]]:
    """Use `ip neigh` to map known neighbor MAC addresses to IP addresses."""
    ip_command = shutil.which("ip") or next((candidate for candidate in ("/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", "/bin/ip") if shutil.which(candidate)), "ip")
    try:
        process = await asyncio.create_subprocess_exec(
            ip_command,
            "neigh",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        stdout, _ = await asyncio.wait_for(process.communicate(), timeout=1.5)
    except Exception:
        return {}

    neighbors: dict[str, dict[str, str]] = {}
    for line in stdout.decode("utf-8", errors="replace").splitlines():
        parts = line.split()
        if not parts:
            continue
        ip = parts[0]
        try:
            lladdr_index = parts.index("lladdr")
        except ValueError:
            continue
        if lladdr_index + 1 >= len(parts):
            continue
        mac = normalize_mac(parts[lladdr_index + 1])
        if not mac:
            continue
        neighbors[mac] = {
            "mac": mac,
            "ip": ip,
            "dev": parts[parts.index("dev") + 1] if "dev" in parts and parts.index("dev") + 1 < len(parts) else "",
            "state": parts[-1],
            "source": "ip neigh",
        }
    return neighbors


def histogram_macs(payload: dict[str, Any]) -> set[str]:
    """Extract normalized MAC addresses reported in receiver CSI diagnostics."""
    macs: set[str] = set()
    for key in ("last_csi_mac", "last_filtered_csi_mac", "last_accepted_csi_mac", "csi_source_mac"):
        mac = normalize_mac(payload.get(key))
        if mac:
            macs.add(mac)
    for entry in payload.get("csi_mac_histogram") or []:
        if isinstance(entry, dict):
            mac = normalize_mac(entry.get("mac"))
            if mac:
                macs.add(mac)
    return macs


def derive_scan_target(config: WorkerConfig, store: NodeStore) -> str:
    """Choose an nmap target from config or observed node IPs."""
    if config.nmap_scan_target:
        return config.nmap_scan_target
    networks: dict[str, int] = {}
    for node in store.all():
        try:
            parsed = ip_address(node.ip)
        except ValueError:
            continue
        if isinstance(parsed, IPv4Address) and not (parsed.is_loopback or parsed.is_unspecified or parsed.is_multicast):
            network = str(IPv4Network(f"{parsed}/24", strict=False))
            networks[network] = networks.get(network, 0) + 1
    if networks:
        return max(networks.items(), key=lambda item: item[1])[0]
    return ""


def parse_nmap_output(text: str) -> dict[str, dict[str, str]]:
    """Parse `nmap -sn` output into a MAC-keyed identity cache."""
    records: dict[str, dict[str, str]] = {}
    current_ip = ""
    current_name = ""
    for line in text.splitlines():
        line = line.strip()
        report = NMAP_REPORT_RE.match(line)
        if report:
            current_ip = report.group("named_ip") or report.group("plain_ip") or ""
            current_name = report.group("name") or ""
            continue
        mac_line = NMAP_MAC_RE.match(line)
        if mac_line and current_ip:
            mac = normalize_mac(mac_line.group("mac"))
            if not mac:
                continue
            records[mac] = {
                "mac": mac,
                "ip": current_ip,
                "name": current_name,
                "vendor": mac_line.group("vendor") or "",
                "source": "nmap",
                "seen_at": datetime.now(timezone.utc).isoformat(),
            }
    return records


async def run_nmap_scan(target: str) -> dict[str, dict[str, str]]:
    """Run a bounded ping/ARP nmap scan and return discovered MAC records."""
    nmap_command = shutil.which("nmap")
    if not nmap_command or not target:
        return {}
    try:
        process = await asyncio.create_subprocess_exec(
            nmap_command,
            "-sn",
            target,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        stdout, _ = await asyncio.wait_for(process.communicate(), timeout=45)
    except Exception:
        return {}
    return parse_nmap_output(stdout.decode("utf-8", errors="replace"))


class MacDiscovery:
    """Intermittently enrich histogram MACs with IP/name/vendor from nmap."""

    def __init__(self) -> None:
        self._reported_macs: set[str] = set()
        self._records: dict[str, dict[str, str]] = {}
        self._last_scan_epoch = 0.0
        self._scan_task: asyncio.Task[None] | None = None

    def status(self) -> dict[str, Any]:
        now = asyncio.get_running_loop().time()
        return {
            "records": self._records,
            "reported_macs": sorted(self._reported_macs),
            "last_scan_epoch": self._last_scan_epoch,
            "last_scan_age_s": round(now - self._last_scan_epoch, 3) if self._last_scan_epoch > 0 else None,
            "scan_running": self._scan_task is not None and not self._scan_task.done(),
        }

    def observe_payload(self, payload: dict[str, Any], config: WorkerConfig, store: NodeStore) -> None:
        new_macs = histogram_macs(payload) - self._reported_macs
        if new_macs:
            self._reported_macs.update(new_macs)
            self.maybe_schedule(config, store, reason="new_mac")

    def maybe_schedule(self, config: WorkerConfig, store: NodeStore, reason: str = "interval") -> None:
        if not config.nmap_enabled:
            return
        now = asyncio.get_running_loop().time()
        if self._scan_task is not None and not self._scan_task.done():
            return
        if reason != "new_mac" and now - self._last_scan_epoch < config.nmap_min_interval_seconds:
            return
        if reason == "new_mac" and now - self._last_scan_epoch < min(config.nmap_min_interval_seconds, 60.0):
            return
        target = derive_scan_target(config, store)
        if not target:
            return
        self._scan_task = asyncio.create_task(self._scan(target))

    def force_scan(self, config: WorkerConfig, store: NodeStore) -> dict[str, Any]:
        if not config.nmap_enabled:
            return {"started": False, "reason": "nmap discovery is disabled", "target": ""}
        if self._scan_task is not None and not self._scan_task.done():
            return {"started": False, "reason": "scan already running", "target": ""}
        target = derive_scan_target(config, store)
        if not target:
            return {"started": False, "reason": "no scan target available", "target": ""}
        self._scan_task = asyncio.create_task(self._scan(target))
        return {"started": True, "reason": "manual", "target": target}

    async def _scan(self, target: str) -> None:
        self._last_scan_epoch = asyncio.get_running_loop().time()
        records = await run_nmap_scan(target)
        if records:
            self._records.update(records)


async def nmap_discovery_loop(config: WorkerConfig, store: NodeStore, discovery: MacDiscovery) -> None:
    """Refresh nmap discovery occasionally even when no new MAC arrives."""
    while True:
        discovery.maybe_schedule(config, store)
        await asyncio.sleep(max(60.0, min(config.nmap_min_interval_seconds, 300.0)))


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
    discovery = MacDiscovery()
    app = web.Application()
    app["config"] = config
    app["store"] = store
    app["mac_discovery"] = discovery

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
        status_data["mac_neighbors"] = await ip_neigh_mac_map()
        status_data["mac_discovery"] = discovery.status()
        return web.Response(text=render_index_html(status_data), content_type="text/html")

    async def receive_telemetry(request: web.Request) -> web.Response:
        """Accept one ESP32 telemetry post and update the in-memory node store."""
        try:
            payload: dict[str, Any] = await request.json()
            remote_ip = request.remote or ""
            snapshot = store.upsert(payload, remote_ip)
            discovery.observe_payload(payload, config, store)
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

    async def receive_capture(request: web.Request) -> web.Response:
        """Append a chunk of CSI capture records to a Pi-side capture file."""
        try:
            payload: dict[str, Any] = await request.json()
            capture_id = safe_capture_id(payload.get("capture_id"))
            records = payload.get("records")
            if not isinstance(records, list):
                raise ValueError("records must be a list")
            data_path, meta_path = capture_paths(config, capture_id)
        except ValueError as exc:
            return web.json_response({"ok": False, "error": str(exc)}, status=400)
        except Exception:
            return web.json_response({"ok": False, "error": "invalid JSON capture payload"}, status=400)

        received_at = datetime.now(timezone.utc).isoformat()
        existing_meta: dict[str, Any] = {}
        if meta_path.exists():
            with suppress(Exception):
                existing_meta = json.loads(meta_path.read_text(encoding="utf-8"))

        with data_path.open("a", encoding="utf-8") as handle:
            for record in records:
                if isinstance(record, dict):
                    record.setdefault("capture_id", capture_id)
                    record.setdefault("device_id", payload.get("device_id"))
                    record.setdefault("mode", payload.get("mode"))
                    handle.write(json.dumps(record, separators=(",", ":")))
                    handle.write("\n")

        total_records = int(existing_meta.get("records", 0)) + sum(1 for record in records if isinstance(record, dict))
        metadata = {
            **existing_meta,
            "capture_id": capture_id,
            "device_id": payload.get("device_id"),
            "name": payload.get("name"),
            "mode": payload.get("mode", "features"),
            "label": payload.get("label", ""),
            "duration_s": payload.get("duration_s"),
            "started_us": payload.get("started_us"),
            "first_received_at": existing_meta.get("first_received_at", received_at),
            "last_received_at": received_at,
            "finished": bool(payload.get("finished")),
            "records": total_records,
            "records_total_reported": payload.get("records_total"),
            "drops_total_reported": payload.get("drops_total"),
            "data_file": data_path.name,
        }
        meta_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")

        return web.json_response(
            {
                "ok": True,
                "ack": "capture_received",
                "capture_id": capture_id,
                "records": total_records,
                "finished": metadata["finished"],
            }
        )

    async def status(_: web.Request) -> web.Response:
        """Return the full live worker status consumed by the PWA service."""
        status_data = store.status(config.version)
        status_data["mac_neighbors"] = await ip_neigh_mac_map()
        status_data["mac_discovery"] = discovery.status()
        return web.json_response(status_data)

    async def node(request: web.Request) -> web.Response:
        """Return one known node snapshot by device ID."""
        device_id = int(request.match_info["device_id"])
        snapshot = store.get(device_id)
        if snapshot is None:
            return web.json_response({"ok": False, "error": "node not found"}, status=404)
        return web.json_response({"ok": True, "node": snapshot.to_dict()})

    async def manual_nmap_scan(_: web.Request) -> web.Response:
        """Start a manual nmap discovery scan when one is not already running."""
        result = discovery.force_scan(config, store)
        return web.json_response({"ok": True, **result, "mac_discovery": discovery.status()})

    async def start_background(app_: web.Application) -> None:
        """Start optional background tasks after aiohttp is ready."""
        app_["udp_probe_task"] = asyncio.create_task(udp_probe_loop(config))
        app_["nmap_discovery_task"] = asyncio.create_task(nmap_discovery_loop(config, store, discovery))

    async def stop_background(app_: web.Application) -> None:
        """Cancel background tasks during graceful shutdown."""
        for task_name in ("udp_probe_task", "nmap_discovery_task"):
            task = app_.get(task_name)
            if not task:
                continue
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task

    app.router.add_get("/", index)
    app.router.add_get("/healthz", healthz)
    app.router.add_post(config.telemetry_path, receive_telemetry)
    app.router.add_post(config.capture_path, receive_capture)
    app.router.add_get("/internal/status", status)
    app.router.add_get("/internal/nodes/{device_id:\\d+}", node)
    app.router.add_post("/internal/nmap/scan", manual_nmap_scan)
    app.on_startup.append(start_background)
    app.on_cleanup.append(stop_background)
    return app


def main() -> None:
    """Load configuration and run the worker as a standalone process."""
    config = load_config()
    web.run_app(make_app(config), host=config.host, port=config.port)


if __name__ == "__main__":
    main()
