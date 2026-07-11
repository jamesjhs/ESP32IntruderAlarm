# Python Worker Explanation Manual

Version: `0.4.1`

The Python worker is the local listener for the ESP32 devices.

Each ESP32 sends small JSON messages to the Pi. Receiver boards send movement
and CSI health values, while the dedicated sender board sends its own status,
including whether it is emitting packets and at what rate. The worker receives
those messages, remembers the latest status for each chip, and makes that
status available to the web dashboard.

This means the Python worker does not need to understand every radio detail. It
acts as the local registry and relay: the PWA asks it which devices are alive,
then the PWA can use the stored device address to proxy settings back to a
receiver or to the sender. Movement history still comes from the receiver
payloads; the sender exists to provide a steadier Wi-Fi signal source for those
receivers to analyse.

The worker can also send small UDP packets on a timer. Those packets can help
keep Wi-Fi traffic predictable for CSI sensing, but the dedicated ESP32 sender
is the cleaner controlled-source approach because receiver firmware can filter
CSI to the sender's known MAC address.

In `0.4.1`, receiver status includes protected diagnostics for that configured
sender MAC. The worker simply preserves those fields in the live node snapshot;
the PWA displays them so you can tell whether sender packets are seen before
filtering and accepted after the receiver's CSI gates.
