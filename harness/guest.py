#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Standalone module checks inside the disposable test guest."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SCHEDULE = Path("/sys/devices/platform/virt-phc-extts/extts_schedule")


def run(args):
    return subprocess.run([str(a) for a in args], check=True)


def module_check(no_cross=False, drift=10000):
    run(["insmod", ROOT / "module/virt_phc_extts.ko", f"freq_error_ppb={drift}",
         "initial_offset_ns=10000000000", f"no_crosststamp={int(no_cross)}"])
    try:
        # Match both the clock name and platform parent before clock changes.
        clocks = [p for p in Path("/sys/class/ptp").glob("ptp*")
                  if (p / "clock_name").read_text().strip() == "virt-phc-extts"
                  and p.resolve().parent.parent.name == "virt-phc-extts"]
        if len(clocks) != 1:
            raise RuntimeError(f"expected one virtual PHC, found {len(clocks)}")
        phc = Path("/dev") / clocks[0].name
        run([ROOT / "tools/test-vphc", phc, SCHEDULE, str(drift), str(int(no_cross))])
    finally:
        run(["rmmod", "virt_phc_extts"])


def main():
    if Path("/etc/hostname").read_text().strip() != "vphc-test":
        raise RuntimeError("expected disposable test guest")
    if not Path("/root/guest-ready").exists():
        raise RuntimeError("guest provisioning did not finish")
    module_check()
    module_check(no_cross=True)
    module_check(drift=-10000)
    module_check(drift=62499999)
    print("RESULT: PASS", flush=True)


if __name__ == "__main__":
    main()
