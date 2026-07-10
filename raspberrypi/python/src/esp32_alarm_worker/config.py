"""Configuration loading for the ESP32 telemetry worker.

This module is the Python worker's equivalent of the PWA service's
`src/config.ts`. It reads shared deployment settings from `raspberrypi/.env`,
normalizes values that affect HTTP routing, and returns an immutable
`WorkerConfig` object consumed by `server.py`.

Interactions:
- `server.py` calls `load_config()` during startup and stores the result on the
  aiohttp application.
- ESP32 firmware posts telemetry to `telemetry_path`, usually `/espdata`.
- Optional UDP probe settings are used by `server.udp_probe_loop()` to stimulate
  CSI traffic at a predictable rate during experiments.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv


def _load_env() -> None:
    """Load the shared Raspberry Pi `.env` file if it exists."""
    here = Path(__file__).resolve()
    raspberrypi_dir = here.parents[3]
    load_dotenv(raspberrypi_dir / ".env")


def _bool_env(name: str, default: bool) -> bool:
    """Parse common truthy environment values while preserving a default."""
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class WorkerConfig:
    """Runtime settings for the LAN-facing telemetry worker."""

    version: str
    host: str
    port: int
    telemetry_path: str
    udp_probe_enabled: bool
    udp_probe_target_ip: str
    udp_probe_target_port: int
    udp_probe_interval_seconds: float
    udp_probe_payload: bytes


def load_config() -> WorkerConfig:
    """Read environment variables and return a sanitized worker configuration."""
    _load_env()
    telemetry_path = os.getenv("ESP32_TELEMETRY_PATH", "/espdata")
    if not telemetry_path.startswith("/"):
        telemetry_path = f"/{telemetry_path}"

    return WorkerConfig(
        version=os.getenv("APP_VERSION", "0.2.1"),
        host=os.getenv("WORKER_HOST", "0.0.0.0"),
        port=int(os.getenv("WORKER_PORT", "3005")),
        telemetry_path=telemetry_path,
        udp_probe_enabled=_bool_env("UDP_PROBE_ENABLED", False),
        udp_probe_target_ip=os.getenv("UDP_PROBE_TARGET_IP", ""),
        udp_probe_target_port=int(os.getenv("UDP_PROBE_TARGET_PORT", "9")),
        udp_probe_interval_seconds=float(os.getenv("UDP_PROBE_INTERVAL_SECONDS", "5")),
        udp_probe_payload=os.getenv("UDP_PROBE_PAYLOAD", "esp32-alarm-probe").encode("utf-8"),
    )
