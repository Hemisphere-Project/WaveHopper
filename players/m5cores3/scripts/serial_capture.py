#!/usr/bin/env python3
"""Non-interactive serial capture for the CoreS3 (USB-CDC needs DTR asserted).

Unlike `pio device monitor`, works without a TTY — usable from scripts and AI
agents. Prints whatever the device sends for --seconds, optionally resetting
it first so you capture a full boot.

Usage:
  python3 scripts/serial_capture.py [--port /dev/ttyACM0] [--seconds 30] [--reset]
"""

import argparse
import sys
import time

import serial  # ships with the platformio venv


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--reset', action='store_true',
                    help='pulse RTS/DTR to reset the board before capturing')
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.5) as port:
        if args.reset:
            # esptool hard-reset sequence for the USB-JTAG bridge: RTS drives
            # EN only while DTR is LOW (the coupled-transistor circuit cancels
            # when both are high).
            port.dtr = False
            port.rts = True
            time.sleep(0.1)
            port.rts = False
            time.sleep(0.2)
            port.reset_input_buffer()
        port.dtr = True
        port.rts = False

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = port.read(4096)
            if data:
                sys.stdout.write(data.decode('utf-8', errors='replace'))
                sys.stdout.flush()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
