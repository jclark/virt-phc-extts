# Synchronizing the virtual PHC with linuxptp

This example uses `ts2phc` from [linuxptp](https://www.linuxptp.org/) to
discipline a virtual clock with a simulated 1 Hz input. It needs neither
GNSS messages nor a physical PHC. It runs on a normal Linux machine or in a VM.
Only the virtual PHC is adjusted; the system clock is used as a reference.

Install the compiler, headers for the running kernel, and `linuxptp`.
On Debian, the packages are `build-essential linux-headers-$(uname -r) linuxptp`;
on Fedora, `gcc make kernel-devel-$(uname -r) linuxptp`. Build from the repository
root:

```sh
make -C module
make -C tools
sudo bash examples/ts2phc/run.sh
```

The module must initially be unloaded. The script loads it with a +10 second
offset from realtime and +10000 ppb intrinsic frequency error, discovers its
`/dev/ptpN`, and runs:

```sh
ts2phc -s generic -c /dev/ptpN -f examples/ts2phc/ts2phc.cfg -m
tools/vphc-pulse
```

`vphc-pulse` estimates the relationship between `CLOCK_REALTIME` and
`CLOCK_MONOTONIC_RAW`. It places rising edges on whole realtime seconds and
falling edges 100 ms later, scheduling about two seconds ahead. A fitted clock
mapping smooths sampling noise; queued deadlines are kept fixed while subsequent
pulses use newer estimates. A reference clock step or missed pulse causes the
generator to exit. Normal frequency slewing is supported, though rapid changes
can temporarily increase the error of the fitted prediction.

`ts2phc -s generic` gets the second number from the system clock and uses the
captured timestamps to discipline the virtual PHC. The configuration requests
both rising and falling timestamps, then uses `ts2phc.pulsewidth` to reject the
trailing edge, as it would for hardware that captures both edges. To exercise
kernel rising-only filtering instead, change `ts2phc.extts_polarity` to `rising`.
The generator still submits both edges.

The PHC is synchronized to **TAI**, while system realtime is UTC. `ts2phc` uses
the kernel UTC-to-TAI offset if set, or the configured leap-seconds file otherwise.
Keep the `tzdata` package current; an expired leap table can prevent startup when
the kernel offset is unset. This example does not set the system clock or its
TAI offset. The initial +10 seconds is relative to UTC, so the first correction
also includes the UTC/TAI difference.

The script reports success after ten consecutive locked offsets smaller than
1 microsecond. It leaves logs in a printed temporary directory (or a directory
given as its first argument), stops both processes, clears the scheduled pulses,
and unloads the module. The threshold checks convergence; it is not a claim
about absolute UTC accuracy. Both the simulated pulses and second numbers
derive from the same system clock.

For manual experiments, `tools/vphc-pulse --help` describes the small CLI.
`--count 20` emits twenty pulses and exits after their falling edges;
`--width 0.2` changes pulse width (also change the ts2phc configuration).
`--source /dev/ptpN` uses an independent reference PHC supporting
`PTP_SYS_OFFSET_PRECISE`, such as `ptp_kvm`; this must not be the PHC being
disciplined. The reference's whole-second boundaries must agree with the
system-clock second numbering used by `generic`.

The generator owns the entire `extts_schedule` while running, clears it on
normal exit or SIGINT/SIGTERM, and does not configure the target PHC or enable
capture. Do not run another writer on the same schedule.

See the [ts2phc manual](https://www.linuxptp.org/documentation/ts2phc/)
for generic sources, edge polarity, pulse width, and leap-second handling.
