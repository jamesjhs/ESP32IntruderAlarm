from __future__ import annotations

import time
from dataclasses import dataclass, field
from ipaddress import ip_address
from typing import Any, Optional


@dataclass
class NodeSnapshot:
    device_id: int
    name: str
    ip: str
    last_seen_epoch: float
    payload: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        age = max(0.0, time.time() - self.last_seen_epoch)
        return {
            "device_id": self.device_id,
            "name": self.name,
            "ip": self.ip,
            "last_seen_epoch": self.last_seen_epoch,
            "last_seen_age_s": round(age, 3),
            "state": "online" if age < 30 else "stale",
            "payload": self.payload,
        }


class NodeStore:
    def __init__(self) -> None:
        self._nodes: dict[int, NodeSnapshot] = {}

    def _usable_ip(self, value: object) -> bool:
        try:
            parsed = ip_address(str(value))
        except ValueError:
            return False
        return not (parsed.is_unspecified or parsed.is_loopback or parsed.is_multicast)

    def upsert(self, payload: dict[str, Any], remote_ip: str) -> NodeSnapshot:
        device_id = int(payload.get("device_id", -1))
        if device_id < 0 or device_id > 255:
            raise ValueError("device_id must be an integer from 0 to 255")

        name = str(payload.get("name") or f"Movement{device_id:02X}")
        payload_ip = payload.get("ip")
        ip = str(payload_ip) if self._usable_ip(payload_ip) else remote_ip
        snapshot = NodeSnapshot(
            device_id=device_id,
            name=name,
            ip=ip,
            last_seen_epoch=time.time(),
            payload=payload,
        )
        self._nodes[device_id] = snapshot
        return snapshot

    def get(self, device_id: int) -> Optional[NodeSnapshot]:
        return self._nodes.get(device_id)

    def all(self) -> list[NodeSnapshot]:
        return sorted(self._nodes.values(), key=lambda node: node.device_id)

    def status(self, version: str) -> dict[str, Any]:
        nodes = [node.to_dict() for node in self.all()]
        healthy = sum(1 for node in nodes if node["state"] == "online")
        return {
            "version": version,
            "service": "esp32-alarm-worker",
            "healthy_nodes": healthy,
            "known_nodes": len(nodes),
            "nodes": nodes,
        }
