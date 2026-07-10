"""Shared version loading for the Python worker package."""

from __future__ import annotations

from pathlib import Path


def read_shared_version() -> str:
    """Return the Raspberry Pi project version from the shared VERSION file."""
    version_file = Path(__file__).resolve().parents[3] / "VERSION"
    if version_file.exists():
        return version_file.read_text(encoding="utf-8").strip()
    return "0.0.0"
