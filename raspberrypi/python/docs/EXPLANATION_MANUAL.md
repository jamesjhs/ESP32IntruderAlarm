# Python Worker Explanation Manual

Version: `0.2.1`

The Python worker is the local listener for the ESP32 devices.

Each ESP32 sends small JSON messages to the Pi. The worker receives those
messages, remembers the latest status for each chip, and makes that status
available to the web dashboard.

The worker can also send small UDP packets on a timer. Those packets can help
keep Wi-Fi traffic predictable for CSI sensing, but they should be used gently
because network traffic itself affects the measurement.
