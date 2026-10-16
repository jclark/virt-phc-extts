# Virtual PHC interface

## Device and clock

Loading `virt_phc_extts.ko` creates:

- A platform device: `/sys/devices/platform/virt-phc-extts`.
- A PTP clock named `virt-phc-extts`, accessible through its allocated `/dev/ptpN`.
- Pin 0 named `SDP0`, with EXTTS channel 0.
- A dummy `vphc%d` network interface exposing the PHC index through ethtool.

There is no physical PHC dependency. There are no periodic outputs, PPS device,
hardware packet timestamps, or real Ethernet traffic.

The PHC's time is an affine mapping of `CLOCK_MONOTONIC_RAW`, initially anchored
to system realtime plus `initial_offset_ns`. Intrinsic drift and the consumer's
frequency adjustment are additive. System realtime steps do not step this PHC.

Read-only module parameters (set at load):

| Parameter | Default | Meaning |
| --- | --- | --- |
| `freq_error_ppb` | `10000` | Intrinsic frequency error relative to raw time; range ±62,499,999 ppb |
| `initial_offset_ns` | `10000000000` | Initial offset from system realtime in ns |
| `no_crosststamp` | `0` | Disable precise cross-timestamps to test the extended fallback |
| `driver_name` | `virt_phc_extts` | Advertised ethtool driver name |

Changing `driver_name` does not emulate another driver's hardware or restrictions.
In particular, both-edge scheduling does not constitute a complete I210/I225 model.

## Input schedule

Path: `/sys/devices/platform/virt-phc-extts/extts_schedule`.

The attribute describes the whole remaining input schedule. It is root-writable
and readable. A successful write atomically replaces all still-future edges;
it does not append implicitly.

Each whitespace-separated token has one of these forms:

| Form | Edges |
| --- | --- |
| `R,F` | Rising at R, falling at F |
| `R` or `R,` | Rising at R |
| `,F` | Falling at F |

R and F are unsigned decimal absolute `CLOCK_MONOTONIC_RAW` nanoseconds on
the machine running the module. They are neither PHC timestamps nor realtime
timestamps, and must not come from another machine's raw clock.

All individual edges, including the two within a pair, must be strictly increasing
across the entire write. Duplicate deadlines, reversed pairs, and unordered tokens
are rejected. A bare comma, signs, nondecimal characters and embedded NUL bytes
are invalid. Either edge may be absent, but not both. The module does not require
alternating polarity or otherwise enforce an electrical waveform.

For example, if raw time is currently 10 seconds, this is a valid schedule:

```text
11000000000,11100000000 12000000000 ,12100000000
```

It describes two 100 ms pulses, with the second pair split across tokens.

### Replacement, elapsed edges, and errors

The complete text and its ordering are validated first. At installation, deadlines
less than or equal to the current raw time are silently discarded. This also
applies independently to the edges of a pair: if its rise has passed but its fall
is still future, only the fall remains.

A schedule may contain at most 16 future edges (eight complete pulses), with no
edge more than three seconds ahead at installation. Elapsed entries do not count
against the 16-edge limit. A blank/whitespace-only write or an entirely elapsed
schedule clears all future edges. In shell, use:

```sh
printf '\n' | sudo tee /sys/devices/platform/virt-phc-extts/extts_schedule >/dev/null
```

A zero-byte shell write does not necessarily invoke a sysfs store operation;
write a newline to clear reliably.

Malformed/unordered input or a deadline beyond the horizon returns `EINVAL`;
excess future edges return `ENOSPC`; an overflowing integer returns `ERANGE`.
A failed write leaves the existing future schedule intact. Already scheduled edges
may naturally elapse during a read or write, including one which ultimately fails.

Readback is a snapshot of remaining future edges, not an event log. It is
non-consuming and ends in a newline; an empty schedule reads as just a newline.
Adjacent rise/fall edges are grouped as `R,F`; lone rises use `R` and lone falls
use `,F`. Original whitespace/grouping is not preserved.

To append, read the current schedule, append strictly later tokens, then write
the combined list. Edges that elapsed in between are ignored rather than replayed.
This is safe for a single writer; concurrent writers need external coordination.
Use a fresh read or seek back to offset zero to obtain a new snapshot.

## Capture and delivery

The pulse generator controls the input schedule. The consumer controls capture
through the existing Linux PTP ABI:

1. Assign pin 0 to `PTP_PF_EXTTS`, channel 0, with `PTP_PIN_SETFUNC`.
2. Enable channel 0 using `PTP_EXTTS_REQUEST2`, `PTP_ENABLE_FEATURE` and
   `PTP_RISING_EDGE`, `PTP_FALLING_EDGE`, or both.
3. Poll/read `struct ptp_extts_event` records from the PHC device.

Legacy `PTP_EXTTS_REQUEST` is also supported; enabling without edge flags selects
rising edges. Unsupported request types/channels/flags are rejected.

An edge produces an event only when the pin is assigned and its polarity is
enabled at its scheduled instant. Otherwise it is discarded. Each event has
channel index 0 and the PHC timestamp at that instant. The standard EXTTS event
does not label its polarity, so a both-edge consumer must distinguish edges using
pulse width or other knowledge, just as with physical hardware.

Before applying a clock step, frequency change, pin assignment, or capture-state
change, the module retires elapsed edges using the old state. Delayed callbacks
therefore do not retroactively timestamp an earlier edge with a later clock
adjustment. Changing PHC time does not move raw-clock deadlines.

Clearing/replacing the input schedule does not retract events already queued in
the PTP core for readers. Normal kernel event-queue limits and scheduling latency
still apply.

## Clock operations

The device supports `clock_gettime`, `clock_settime`, relative steps and
frequency adjustment through `clock_adjtime`; maximum adjustment is 62,499,999 ppb.
`PTP_SYS_OFFSET_PRECISE` maps PHC, realtime and raw time using one system snapshot.
`PTP_SYS_OFFSET_EXTENDED` brackets device reads with system timestamps.
Setting `no_crosststamp=1` makes the precise operation unavailable.

A reference clock's frequency may differ from raw time (for example, NTP steers
system realtime). A servo's required correction is consequently the reference's
rate relative to raw time minus the module's intrinsic drift, not necessarily
just the negative of `freq_error_ppb`.

## Userspace generator

`tools/vphc-pulse` supplies generic 1 Hz pulses; `--help` lists its options.
It predicts whole seconds from a fitted mapping between raw time and either
system realtime (default) or a separate PHC supporting precise cross-timestamps.
It schedules rise/fall pairs about two seconds ahead, with a default 100 ms width.
It owns the entire schedule while running and clears it on normal shutdown.
It neither assigns the capture pin nor adjusts/enables the target PHC.

The generator is optional. A GNSS simulator, test program, or any other userspace
producer can write the schedule directly.
