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

### 2. TS demuxer diagnostics (`#ifdef WH_TS_DIAG`, off by default)
`src/Audio.cpp` `ts_parsePacket()` — logs PMT stream discovery + the failing
packet's header/bytes at the "PES not found" branch. Kept (compiled out) for
the next TS station that misbehaves. Enable with `-DWH_TS_DIAG`.
