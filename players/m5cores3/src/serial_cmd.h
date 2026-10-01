// USB-serial command console — bench/debug control without touching the
// device (scripts/serial_capture.py --cmd, fuzz tests). Line-based, one
// command per line; every reply line starts with "@" so a host can filter it
// out of the log stream. `help` lists the commands.
#pragma once

namespace serial_cmd {
// Non-blocking: drains whatever arrived on Serial, runs complete lines.
// Call from loop() (UI task) — commands reuse the same entry points as touch.
// Also called from setup()'s boot wifi wait: until setReady(), only the
// boot-safe commands run (wifi provisioning, net, reboot) — the player and
// catalog don't exist yet.
void poll();
void setReady();
}  // namespace serial_cmd
