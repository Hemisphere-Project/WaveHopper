#!/usr/bin/env python3
"""Randomized soak/fuzz test of the player over the serial console.

Fires weighted random actions (tunes, rapid surf bursts, retunes, volume spam,
idle gaps, occasional wifi drops and reboots) and checks invariants:

  crash       panic / Guru Meditation / abort / watchdog / unexpected reset
  ui-hang     the UI loop stops answering `status` within 3 s
  player-hang the newest requested tune never starts ("tuning [N]")
  no-lock     after a settle period, not playing the station last asked for
              (only flagged when the supervisor didn't legitimately skip on)
  heap        reported as a trend over settled samples (leak hunting)

Every anomaly is printed with the device log around it; the full timestamped
log goes to --log. Deterministic per --seed.

  ~/.platformio/penv/bin/python3 scripts/fuzz.py --minutes 20 --seed 1 --log /tmp/fuzz.log
"""

from __future__ import annotations

import argparse
import random
import re
import time

from wh_console import Device

CRASH = re.compile(r'Guru Meditation|panic|abort\(\)|LoadProhibited|StoreProhibited|'
                   r'Stack canary|stack overflow|assert failed|Task watchdog|TG1WDT|RTCWDT|'
                   r'CORRUPT HEAP|Backtrace:')
BOOT = re.compile(r'fw \d+\.\d+\.\d+ \(build')
STATUS = re.compile(r'@status state=(.+?) idx=(-?\d+) id=(\S+) buf=(\d+) .* heap=(\d+) '
                    r'maxblk=(\d+) .* up=(\d+)s')


class Fuzzer:
    def __init__(self, dev: Device, rng: random.Random, n: int):
        self.d = dev
        self.rng = rng
        self.n = n
        self.anomalies: list[tuple[str, str]] = []
        self.expected_resets = 0
        self.scan_from = dev.mark()
        self.last_tune: int | None = None
        self.tune_mark = 0
        self.heap: list[tuple[float, int, int]] = []
        self.actions = 0

    # -- helpers ------------------------------------------------------------
    def flag(self, kind: str, detail: str, context_from: int | None = None) -> None:
        ctx = self.d.since(max(0, (context_from or self.d.mark()) - 25))
        tail = '\n'.join('      ' + l[:170] for _, l in ctx[-40:])
        self.anomalies.append((kind, detail))
        print(f'!! {kind}: {detail}\n{tail}', flush=True)

    def scan_log(self) -> None:
        """Crash signatures and resets the fuzzer did not ask for."""
        new = self.d.since(self.scan_from)
        base = self.scan_from
        self.scan_from += len(new)
        for i, (_, l) in enumerate(new):
            if CRASH.search(l):
                self.flag('crash', l.strip(), base + i)
            if BOOT.search(l):
                if self.expected_resets > 0:
                    self.expected_resets -= 1
                else:
                    self.flag('reset', 'device rebooted without being asked', base + i)

    def status(self) -> re.Match | None:
        m = self.d.mark()
        self.d.send('status')
        hit = self.d.wait_for(r'^@status', m, 3.0)
        if not hit:
            return None
        return STATUS.search(hit[1])

    def wait_boot(self) -> None:
        m = self.d.mark()
        if not self.d.wait_for(BOOT.pattern, m, 30):
            self.flag('reset', 'no boot banner within 30 s of reboot', m)
        # setup() (sync, OTA check) runs before the console answers
        t = time.monotonic()
        while time.monotonic() - t < 40:
            if self.status():
                return
            time.sleep(0.5)  # the boot-time console answers `state=booting` at once
        self.flag('ui-hang', 'console never came back after reboot', m)

    # -- actions ------------------------------------------------------------
    def a_tune(self) -> None:
        idx = self.rng.randrange(self.n)
        self.tune_mark = self.d.mark()
        self.d.send(f'tune {idx}')
        self.last_tune = idx

    def a_surf(self) -> None:
        # faster than any connect can finish: exercises queue coalescing
        for _ in range(self.rng.randint(2, 7)):
            self.d.send(self.rng.choice(['next', 'prev']))
            time.sleep(self.rng.uniform(0.05, 0.6))
        self.last_tune = None  # resolved from the device's own status below

    def a_retune(self) -> None:
        self.d.send('retune')

    def a_vol(self) -> None:
        for _ in range(self.rng.randint(1, 10)):
            self.d.send(f'vol {self.rng.randint(4, 21)}')
            time.sleep(0.03)

    def a_idle(self) -> None:
        time.sleep(self.rng.uniform(0.2, 6))

    def a_wifi_drop(self) -> None:
        # the supervisor drops pending tunes while offline and resumes the
        # current station on reconnect — not a hang
        self.last_tune = None
        self.d.send('wifi-drop')
        time.sleep(1)

    def a_reboot(self) -> None:
        self.expected_resets += 1
        self.d.send('reboot')
        self.wait_boot()
        self.last_tune = None

    # -- checks -------------------------------------------------------------
    def check_responsive(self) -> re.Match | None:
        st = self.status()
        if not st:
            m = self.d.mark()
            time.sleep(2)
            st = self.status()
            if not st:
                self.flag('ui-hang', 'status unanswered twice (3 s each)', m)
        return st

    def settle(self, budget: float = 45) -> None:
        """Let the supervisor finish, then check it is playing something sane."""
        m = self.d.mark()
        want = self.last_tune
        if want is not None:
            started = self.d.wait_for(rf'tuning \[{want}\]', self.tune_mark, 20)
            if not started:
                st = self.status()  # tuneTo() ignores the station already playing
                if not (st and st.group(1) == 'playing' and int(st.group(2)) == want):
                    self.flag('player-hang', f'tune {want} never started within 20 s', m)
        deadline = time.monotonic() + budget
        st = None
        while time.monotonic() < deadline:
            st = self.check_responsive()
            if st and st.group(1) == 'playing':
                break
            time.sleep(2)
        if not st:
            return
        state, idx = st.group(1), int(st.group(2))
        if state != 'playing':
            self.flag('no-lock', f'state={state} idx={idx} {budget:.0f} s after settle began', m)
            return
        skipped = [l for _, l in self.d.since(self.tune_mark)
                   if 'tune deadline' in l or 'FAILED' in l or 'stall on' in l]
        if want is not None and idx != want and not skipped:
            self.flag('no-lock', f'asked for {want}, playing {idx} without a logged skip', m)
        self.heap.append((time.monotonic(), int(st.group(5)), int(st.group(6))))

    # -- main loop ----------------------------------------------------------
    def run(self, minutes: float) -> None:
        weights = [(self.a_tune, 30), (self.a_surf, 18), (self.a_retune, 6), (self.a_vol, 10),
                   (self.a_idle, 20), (self.a_wifi_drop, 2), (self.a_reboot, 2)]
        funcs, w = zip(*weights)
        end = time.monotonic() + minutes * 60
        since_settle = 0
        while time.monotonic() < end:
            act = self.rng.choices(funcs, w)[0]
            self.actions += 1
            print(f'[{time.strftime("%H:%M:%S")}] #{self.actions} {act.__name__[2:]}', flush=True)
            act()
            self.scan_log()
            since_settle += 1
            # mostly chaos; every few actions let it settle and assert
            if act in (self.a_reboot, self.a_wifi_drop) or since_settle >= self.rng.randint(3, 8):
                # a wifi re-kick can be refused once (AUTH_FAIL) → 2 × 20 s + lock
                self.settle(90 if act is self.a_wifi_drop else 45)
                self.scan_log()
                since_settle = 0
            elif self.rng.random() < 0.5:
                self.check_responsive()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--minutes', type=float, default=15)
    ap.add_argument('--seed', type=int, default=int(time.time()))
    ap.add_argument('--log')
    args = ap.parse_args()

    dev = Device(args.port, args.log)
    try:
        if not dev.sync():
            print('no answer from the console')
            return 1
        n = len([l for l in dev.ask('list') if l.startswith('@list')])
        print(f'seed={args.seed} stations={n} minutes={args.minutes}', flush=True)
        fz = Fuzzer(dev, random.Random(args.seed), n)
        fz.run(args.minutes)
        fz.settle()
        fz.scan_log()
        print('\n=== summary ===')
        print(f'actions={fz.actions} anomalies={len(fz.anomalies)}')
        kinds: dict[str, int] = {}
        for k, _ in fz.anomalies:
            kinds[k] = kinds.get(k, 0) + 1
        for k, c in sorted(kinds.items()):
            print(f'  {k}: {c}')
        if fz.heap:
            hs = [h for _, h, _ in fz.heap]
            ms = [m for _, _, m in fz.heap]
            print(f'heap (settled) first={hs[0]} min={min(hs)} last={hs[-1]} | '
                  f'maxblk min={min(ms)} last={ms[-1]} | samples={len(hs)}')
        return 1 if fz.anomalies else 0
    finally:
        dev.close()


if __name__ == '__main__':
    raise SystemExit(main())
