#!/usr/bin/env python3
"""Drive the M5 firmware over its serial command console (src/serial_cmd.cpp).

Ports: CoreS3 = /dev/ttyACM0 (USB-CDC, the default), Fire = /dev/ttyUSB0.

One process owns the port: it logs everything the device prints and sends
commands in between. Replies from the console start with '@'.

Usage:
  python3 scripts/wh_console.py cmd status list          # run commands, print replies
  python3 scripts/wh_console.py boot --runs 5            # reboot N times, time first lock
  python3 scripts/wh_console.py log --seconds 60         # plain capture (no reset)

Run with the platformio venv python (ships pyserial):
  ~/.platformio/penv/bin/python3 scripts/wh_console.py ...
"""

from __future__ import annotations

import argparse
import re
import sys
import threading
import time

import serial


def is_uart_bridge(port: str) -> bool:
    """Fire (CP210x/CH9102 UART bridge) vs CoreS3 (native USB-CDC)."""
    return 'ttyUSB' in port or 'usbserial' in port or 'SLAB' in port


def open_port(port: str) -> serial.Serial:
    """Open without resetting, lines in the board's idle state.

    USB-CDC (CoreS3) needs DTR asserted or the device's Serial stays silent.
    A UART bridge (Fire) wires DTR->GPIO0 / RTS->EN through the auto-reset
    transistors: DTR asserted pulls GPIO0 low = fights Module Audio's MCLK
    (GPIO0 on the Fire), so both lines stay released there.

    Order matters on the bridge: the kernel opens the tty with BOTH lines
    asserted (the transistors cancel — no reset); dropping DTR first would
    pass through RTS-only = EN low = a reset (it killed a wifi-join mid-way).
    Drop RTS first (DTR-only just holds GPIO0 low for microseconds), then DTR.
    """
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.rts = False
    ser.dtr = not is_uart_bridge(port)
    return ser


class Device:
    """Background reader + line log; send() writes a command line."""

    def __init__(self, port: str, logfile: str | None = None):
        self.ser = open_port(port)
        self.lines: list[tuple[float, str]] = []
        self.lock = threading.Lock()
        self.log = open(logfile, 'a', encoding='utf-8') if logfile else None
        self.t0 = time.monotonic()
        self._stop = False
        self._buf = b''
        self.th = threading.Thread(target=self._reader, daemon=True)
        self.th.start()

    def _reader(self) -> None:
        while not self._stop:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                time.sleep(0.5)  # USB-CDC re-enumerates across a reboot
                self._reopen()
                continue
            if not data:
                continue
            self._buf += data
            while b'\n' in self._buf:
                raw, self._buf = self._buf.split(b'\n', 1)
                line = raw.decode('utf-8', errors='replace').rstrip('\r')
                now = time.monotonic() - self.t0
                with self.lock:
                    self.lines.append((now, line))
                if self.log:
                    self.log.write(f'{now:9.2f} {line}\n')
                    self.log.flush()

    def _reopen(self) -> None:
        port = self.ser.port
        for _ in range(40):
            try:
                self.ser.close()
                self.ser = open_port(port)
                return
            except serial.SerialException:
                time.sleep(0.25)

    def reset(self) -> None:
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False
        time.sleep(0.2)
        self.ser.dtr = not is_uart_bridge(self.ser.port)

    def mark(self) -> int:
        with self.lock:
            return len(self.lines)

    def since(self, mark: int) -> list[tuple[float, str]]:
        with self.lock:
            return self.lines[mark:]

    def send(self, cmd: str) -> None:
        try:
            self.ser.write((cmd + '\n').encode())
        except serial.SerialException:
            pass

    def wait_for(self, pattern: str, mark: int, timeout: float) -> tuple[float, str] | None:
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        seen = mark
        while time.monotonic() < deadline:
            new = self.since(seen)
            seen += len(new)
            for t, line in new:
                if rx.search(line):
                    return t, line
            time.sleep(0.05)
        return None

    def ask(self, cmd: str, timeout: float = 12.0) -> list[str]:
        """Send a command, return its '@' reply lines (empty = no answer)."""
        m = self.mark()
        self.send(cmd)
        first = self.wait_for(r'^@', m, timeout)
        if not first:
            return []
        time.sleep(0.15)  # multi-line replies (list) arrive back to back
        return [l for _, l in self.since(m) if l.startswith('@')]

    def sync(self, tries: int = 6) -> bool:
        """Handshake until the console answers: after the port sat closed for a
        while, the first bytes sent are sometimes dropped by the USB-CDC link."""
        for _ in range(tries):
            m = self.mark()
            self.send('')
            self.send('status')
            if self.wait_for(r'^@status', m, 1.5):
                return True
        return False

    def close(self) -> None:
        self._stop = True
        self.th.join(timeout=1)
        self.ser.close()
        if self.log:
            self.log.close()


def cmd_main(dev: Device, args) -> int:
    if not dev.sync():
        print('no answer from the console')
        return 1
    for c in args.commands:
        # wifi-join verifies the link before replying (up to ~15 s).
        for line in dev.ask(c, timeout=25.0 if c == 'wifi-join' else 12.0) or ['(no reply)']:
            print(line)
    return 0


def boot_main(dev: Device, args) -> int:
    """Reboot repeatedly; report boot→first 'playing' and the tune timing lines."""
    if not dev.sync():
        print('no answer from the console')
        return 1
    for run in range(args.runs):
        m = dev.mark()
        dev.send('reboot')
        boot = dev.wait_for(r'fw \d+\.\d+\.\d+ \(build', m, 30)
        if not boot:
            print(f'run {run}: no boot banner')
            continue
        play = dev.wait_for(r'playing \[', m, args.timeout)
        tunes = [l for _, l in dev.since(m) if 'tune timing' in l or 'select returned' in l
                 or 'deadline' in l]
        if play:
            print(f'run {run}: playing {play[0] - boot[0]:.1f}s after banner')
        else:
            print(f'run {run}: NOT playing after {args.timeout}s')
        for l in tunes:
            print('    ' + l[:170])
    return 0


def log_main(dev: Device, args) -> int:
    if args.reset:
        dev.reset()
    time.sleep(args.seconds)
    for _, l in dev.since(0):
        print(l)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--log', help='append the full timestamped device log here')
    sub = ap.add_subparsers(dest='mode', required=True)
    c = sub.add_parser('cmd')
    c.add_argument('commands', nargs='+')
    b = sub.add_parser('boot')
    b.add_argument('--runs', type=int, default=3)
    b.add_argument('--timeout', type=float, default=60)
    lg = sub.add_parser('log')
    lg.add_argument('--seconds', type=float, default=30)
    lg.add_argument('--reset', action='store_true')
    args = ap.parse_args()

    dev = Device(args.port, args.log)
    try:
        return {'cmd': cmd_main, 'boot': boot_main, 'log': log_main}[args.mode](dev, args)
    finally:
        dev.close()


if __name__ == '__main__':
    raise SystemExit(main())
