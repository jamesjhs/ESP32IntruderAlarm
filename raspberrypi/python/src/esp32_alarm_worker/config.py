from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv


def _load_env() -> None:
    here = Path(__file__).resolve()
    raspberrypi_dir = here.parents[3]
    load_dotenv(raspberrypi_dir / ".env")


def _bool_env(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class WorkerConfig:
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
    _load_env()
    telemetry_path = os.getenv("ESP32_TELEMETRY_PATH", "/espdata")
    if not telemetry_path.startswith("/"):
        telemetry_path = f"/{telemetry_path}"

    return WorkerConfig(
        version=os.getenv("APP_VERSION", "0.0.1"),
        host=os.getenv("WORKER_HOST", "0.0.0.0"),
        port=int(os.getenv("WORKER_PORT", "1000")),
        telemetry_path=telemetry_path,
        udp_probe_enabled=_bool_env("UDP_PROBE_ENABLED", False),
        udp_probe_target_ip=os.getenv("UDP_PROBE_TARGET_IP", ""),
        udp_probe_target_port=int(os.getenv("UDP_PROBE_TARGET_PORT", "9")),
        udp_probe_interval_seconds=float(os.getenv("UDP_PROBE_INTERVAL_SECONDS", "5")),
        udp_probe_payload=os.getenv("UDP_PROBE_PAYLOAD", "esp32-alarm-probe").encode("utf-8"),
    )
