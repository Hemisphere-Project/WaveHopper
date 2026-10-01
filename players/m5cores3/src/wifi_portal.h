// Wi-Fi setup portal — give the radio a network from a phone, no typing on
// the device (the Fire has only 3 buttons; the CoreS3 keyboard is cramped).
//
// The device opens a WPA2 access point "Waverz-XXXX" with a random key and
// shows two QR codes: (1) join that AP, (2) open http://192.168.4.1. A
// catch-all DNS makes most phones pop the page up by themselves (captive-
// portal detection). The page lists the networks scanned at start (plus a
// free-text name) and a password field; the device verifies the join, then
// saves + reboots. Runs only at boot or before the player starts — never
// alongside a stream (AP + web server + scan want the radio and the heap).
#pragma once

#include <Arduino.h>

namespace wifi_portal {

enum class Result : uint8_t { Cancelled, TimedOut };

// Blocking modal. On a verified join it saves the credentials and REBOOTS
// (never returns). cancellable: BtnB (or the on-screen "cancel") returns
// Cancelled. idleTimeoutMs > 0: returns TimedOut after that long with no
// phone attached to the AP (so a device whose stored network was merely
// down goes back to retrying it).
Result run(bool cancellable, uint32_t idleTimeoutMs);

}  // namespace wifi_portal
