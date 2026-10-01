# WaveHopper patches to ESP32-audioI2S

Vendored from upstream tag **3.4.6** (github.com/schreibfaul1/ESP32-audioI2S)
so the TS demuxer can be patched for stations whose HLS the stock lib rejects.

Keep this list current — it's the diff against upstream and the thing to
re-apply if we ever bump the base tag.

## Patches

### 1. TS: skip adaptation-field-only packets on the audio PID  ✅ FIXED
`src/Audio.cpp` `ts_parsePacket()`, AAC-PID branch. Livepeer (The Lot) emits
packets with `AFC=3` and an adaptation field that fills the entire payload
area (`adaptation_field_length` 183 → payload start = `5 + 183 = 188` = end of
the 188-byte packet, i.e. zero payload bytes — typically PCR/stuffing packets).
Stock 3.4.6 only special-cases `AFC=2` (adaptation-only); for `AFC=3` it then
reads `packet[188..]` **out of bounds**, mistakes the garbage for a missing PES
start code, and aborts with "PES not found". Fix: when
`posOfPacketStart >= TS_PACKET_SIZE`, treat the packet as no-payload
(`packetLength = 0`) and continue. LYL never emits these packets, which is why
only The Lot hit it. **Verified**: The Lot decodes and plays on the CoreS3.

Worth upstreaming to schreibfaul1 — it's a general Livepeer-HLS bug, not
WaveHopper-specific.

### 3. Bounded TLS handshake on stream connects  ✅
`src/Audio.cpp` constructor: `clientsecure.setHandshakeTimeout(8)`. The
Arduino `NetworkClientSecure` default is **120 s**, and `setConnectionTimeout`
does not cover the handshake — measured on-device, a stalled handshake held
`connecttohost` (and so the whole player task) for 25.5 s across two tune
deadlines; queued tunes could not run. 8 s bounds it under the 15 s tune
deadline (normal handshakes: 0.4 s, ≤4.3 s on a congested channel).

### 4. TS reader: no busy-loop when the socket is idle  ✅
`src/Audio.cpp` end of `processWebStreamTS()`. `m_pwsst.f_nextRound` is set
after the first packet and only cleared once the segment's Content-Length has
been read, and the function ends with `if (f_nextRound) goto nextRound;` — so
mid-segment, once `available()` returns 0, it loops without bound. On a live HLS socket that is a busy-wait for the next bytes (starves
core 0, where the wifi stack runs); on a dead one (wifi drop mid-segment) it
never returns: the player task can't take the stop command and the task
watchdog resets the device (`task_wdt: IDLE0 … CPU 0: wh_player`, backtrace in
`processWebStreamTS`, found by scripts/fuzz.py). Fix: loop only while the
round actually had bytes; otherwise return to `loop()`. Check upstream before
re-basing — the same pattern may exist in `processWebStreamHLS()`.

### 5. Output-health counters  ✅
`Audio::i2sUnderruns` (I2S `on_send_q_ovf` callback, registered right after
`i2s_new_channel`: a DMA buffer went out without fresh data = an audible gap)
and `Audio::decodeBusyUs` / `decodeMaxUs` (wall time inside the codec's
`decode()`). Static, read + reset by the sketch — `player.cpp` prints them in
its 10 s `[buf]` line as `under=`, `dec=` (% of one core) and `max=`.

### 6. Skip per-sample work nobody consumes  ✅
`playChunk()`: the VU meter + spectrum feeder (`calculateVUlevel`, float AGC
with a divide per sample, PSRAM delay lines) and its 10 Hz FFT now run only
when `enableAnalysis(true)` (default on, as upstream); the 3-biquad tone EQ
is skipped while all gains are 0 dB (identity filter). WaveHopper calls
`enableAnalysis(false)` and never sets a tone. Measured on the Fire (classic
ESP32): ~10 points of the decode core; AAC stations underran without it.

### 2. Diagnostics (`#ifdef WH_TS_DIAG`, off by default)
`src/Audio.cpp` — kept (compiled out) for the next misbehaving TS/HLS station:
- `ts_parsePacket()`: PMT stream discovery + the "PES not found" packet dump.
- m3u8 loop / `httpPrint()`: per-segment inter-arrival + buffer, connection
  reuse vs reconnect, and the live-edge 2 s wait. Enable with `-DWH_TS_DIAG`.

## Profiling notes (The Lot / Livepeer, 2026-07-05)
Segment-fetch timing measured on hardware: **the lib is efficient here.** Keep-
alive is reused on every segment (no per-segment TLS — handshakes only twice at
startup, ~330 ms each); steady-state cadence is 2.0 s, matched to the 2.04 s
segments; when the network keeps up the buffer holds dead-steady (~119.5 KB for
20 s+). The occasional drain is purely per-segment fetch time exceeding 2 s
under marginal RF, absorbed only by Livepeer's ~5 s DVR back-buffer. No lib
overhead to reclaim — it's a stream/network ceiling.
