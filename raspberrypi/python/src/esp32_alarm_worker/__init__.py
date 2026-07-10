"""ESP32 Alarm Python worker package.

This package contains the LAN-facing telemetry receiver used by the Raspberry Pi
side of ESP32IntruderAlarm. The package version is read from the shared
`raspberrypi/VERSION` file so health endpoints, package metadata, and the PWA
can report one coherent release number without duplicating it in source.
"""

from ._version import read_shared_version

__version__ = read_shared_version()
