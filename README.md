# virt-phc-extts

A virtual Linux PTP hardware clock (PHC) with an external-timestamp input.
Test software that consumes pulse timestamps without a timing NIC, GNSS receiver,
or physical wiring.

Developed for use with [SatPulse](https://github.com/jclark/satpulse), but independent
of it: any program using Linux PHC external timestamping can use this device.
Included examples demonstrate [linuxptp's ts2phc](examples/ts2phc/README.md) and
[chrony's PHC refclock with extpps](examples/chrony/README.md).

## Concept

A physical PHC has its own clock and captures that clock's value when an electrical
edge arrives on an input pin. This module substitutes a software clock and a list
of scheduled edges:

```text
reference clock → userspace pulse generator → raw-clock edge schedule
                                                    ↓
                                             virtual PHC
                                                    ↓
                                      Linux EXTTS events → consumer
```

The virtual PHC runs from `CLOCK_MONOTONIC_RAW`, with configurable frequency error
and initial offset. Consumers can read, step and frequency-adjust it through the
normal Linux PTP interfaces. Userspace schedules input edges in absolute raw-clock
nanoseconds, independently of the PHC's current time or adjustment.

Each event contains the PHC time at the edge's **scheduled instant**, not at the
timer callback's actual execution time. Scheduling delays can delay delivery but
do not add timestamp jitter. This tests clock servos and the real ioctl/poll/read
paths; it does not model hardware capture noise or prove real-time delivery
performance.

No guest, existing PHC, or GNSS is required. A VM is convenient for isolation.
In a KVM guest, an optional `ptp_kvm` reference lets a generator use host time
even when the guest's system time is deliberately wrong.

## What belongs here

- `module/`: the C kernel module and its build.
- `tools/`: standalone C tools: `test-vphc` checks the device API;
  `vphc-pulse` generates a 1 Hz stream from system time or a reference PHC.
- `examples/`: complete demonstrations with independent timestamp consumers.
- `harness/`: generic Python orchestration for disposable Debian/Fedora guests,
  standalone checks, and arbitrary external test payloads.

Application-specific simulators and integration assertions belong in their
consumers' repositories. The module and tools here have no consumer-specific
build or runtime dependencies.

## Build and try

Install a compiler, make, and headers/development files matching the running
kernel. Typical packages are `build-essential linux-headers-$(uname -r)` on
Debian, or `gcc make kernel-devel-$(uname -r)` on Fedora.

```sh
make -C module
make -C tools
# Install linuxptp, then run the complete demonstration:
sudo bash examples/ts2phc/run.sh
```

The demo loads its own virtual PHC, injects a +10 second initial offset and
+10 ppm drift, schedules paired rising/falling edges, and runs ts2phc until it
converges. It adjusts only the virtual PHC, then stops its processes and unloads
the module. See the [demo instructions](examples/ts2phc/README.md) for details,
logs, UTC/TAI handling, and manual operation.

The [chrony example](examples/chrony/README.md) exercises the other direction:
using external timestamps as a reference for chronyd. Its test instance uses
`-x`, so it does not change the system clock.

For manual loading:

```sh
sudo insmod module/virt_phc_extts.ko
cat /sys/class/ptp/ptp*/clock_name
# Only needed by consumers which discover the PHC through a network interface:
sudo ip link set vphc0 up
# When all consumers and generators have stopped:
sudo rmmod virt_phc_extts
```

Loading registers a PHC named `virt-phc-extts`, one pin `SDP0`, one EXTTS
channel (both index 0), and a dummy network interface `vphc%d`. Do not assume
the PHC is `/dev/ptp0`: other clocks may already exist. The network interface
supports PHC discovery through ethtool, not real traffic or packet timestamping.

Use `make -C module KDIR=/path/to/kernel/build` for a different target kernel.
An enforcing Secure Boot system will reject an unsigned module; use your normal
module-signing procedure or an isolated guest. The examples do not change that
security policy.

## Scheduling API

See [docs/API.md](docs/API.md) for the complete contract.

`/sys/devices/platform/virt-phc-extts/extts_schedule` is a readable/writable list
of future input edges. Each write replaces the schedule. Tokens are `R,F`,
`R` (or `R,`), or `,F`, with decimal absolute `CLOCK_MONOTONIC_RAW`
nanoseconds. Tokens and all individual edges must be in chronological order.

Read the remaining schedule, append new tokens and write it back to extend it.
Elapsed edges are silently ignored, so edges that pass during read/modify/write
are not replayed. A blank line clears the schedule. Multiple writers must
coordinate externally.

The consumer separately assigns pin 0 to EXTTS channel 0 and enables rising,
falling, or both edges using the standard PTP API. Scheduling alone does not
enable capture. There is no custom device ioctl.

## Local guest tests

Install QEMU, `qemu-img`, `cloud-localds`, and an ISO image builder. Fedora
packages: `qemu-system-x86-core qemu-img cloud-utils genisoimage`; Debian:
`qemu-system-x86 qemu-utils cloud-image-utils`. Python 3.12+, OpenSSH and curl
are also needed.

```sh
python3 harness/local.py prepare
python3 harness/local.py upload
python3 harness/local.py test
python3 harness/local.py test --artifacts /root/ts2phc-results -- \
    bash /root/virt-phc-extts/examples/ts2phc/run.sh /root/ts2phc-results
python3 harness/local.py stop
```

The default is Debian 13. Add `--distro fedora` to each command for Fedora 44.
The harness downloads a cloud image, provisions compiler/headers/linuxptp, builds
inside the guest and caches a 12 GB sparse disk, four CPUs and 4 GB RAM.
It uses KVM when available, otherwise TCG. TCG runtime is not yet validated.

Debian state is in `.local/`, with SSH bound to `127.0.0.1:22222`; Fedora
uses `.local/fedora/` and port 22223. `--port` overrides this. Guests persist
between commands until `stop`. Test output is saved as `results.log`; optional
artifacts are collected into `results/`, including after test failure.

Standalone tests cover capability/pin ioctls, rising/falling/both selection,
replacement/readback/clearing, malformed and unordered schedules, elapsed-edge
handling, capacity/horizon limits, nominal intervals, frequency steering, clock
steps, both cross-timestamp APIs, and capture disabling. They run with positive
and negative drift, maximum positive drift, and precise cross-timestamps disabled.

To test another project, `upload --payload DIR --destination /root/payload`
copies arbitrary files alongside the module, and `test -- COMMAND ARG...`
runs its test command. `ssh -- COMMAND ARG...` provides direct investigation.
There is no built-in consumer-specific integration suite.

The standalone C test changes its target clock's time, frequency, and pin state.
Use it only with the virtual device; the guest harness verifies the device name
and platform parent before invoking it.

### Validation

Local runtime checks passed on Debian `6.12.111+deb13-cloud-amd64` and Fedora
`7.1.10-200.fc44.x86_64` / `7.2.10-200.fc44.x86_64`. The matching Fedora host
build passed; its signature policy prevented unsigned host loading, so runtime
testing used guests. The Fedora 7.1 standalone run had no other PHC present.
This is a tested set, not a promise of compatibility with all Linux kernels:
internal kernel APIs change.

The Debian ts2phc both-edge demo reached ten consecutive locked offsets between
−5 and +18 ns. This measures consistency against the simulated reference, not
absolute UTC accuracy.

## License

The project is available under the [MIT license](LICENSE). The kernel module is
additionally offered under [GPL-2.0-only](LICENSES/GPL-2.0-only.txt), with SPDX
`GPL-2.0-only OR MIT` and the kernel's `Dual MIT/GPL` module declaration.
