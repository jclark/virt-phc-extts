# virt-phc-extts: background and findings

Written 2026-10-16 on cantal, from reading satpulse (`~/src/satpulse`, master at c7ed193f),
the kernel tree (`~/src/linux`, net-next at 45ad84d2800e, 2026-10-06), the deadlock project
(`~/src/bcm-phy-ptp-deadlock`) and the web. Web facts are marked as such and listed under Sources.

## The problem

satpulse's PTP functionality (satpulsed disciplining a PHC from a GNSS PPS, and the
`satpulsetool sdp` commands) is not smoke-tested in CI because it needs a PHC with an external
timestamp (extts) input and a GNSS receiver on it. CI runners have neither. The existing
simulators (`time/clocksim`, `time/internal/syncsim`) test the controller mathematics in process;
nothing tests the real syscall path: interface lookup, pin and extts ioctls, the event read loop,
cross-timestamps, clock_adjtime, stale-era handling, shutdown (the extts disable where issue #493
hung), and the sdp subcommands.

## What satpulse requires from a PHC (verified in the code)

- **Found through a network interface, never by /dev path.** `[phc] interface` (satpulsed) and
  `--interface` (satpulsetool sdp) go through ETHTOOL_GET_TS_INFO to the ptp index, then open
  `/dev/ptpN` (`time/phc/phc.go` IfPhcIndex, `time/internal/ts/clock.go` OpenClock,
  `internal/sdpcmd/sdpphc.go`). `sdp --show-ifaces` additionally reads
  `/sys/class/ptp/ptpN/{n_programmable_pins,pins/,n_external_timestamps,n_periodic_outputs}`.
  So a bare PHC is unusable; the fake must register a net_device whose ethtool get_ts_info
  reports the PHC index.
- **Driver profile from the driver name** (`time/phc/driver.go`): ETHTOOL_GDRVINFO name "igb" or
  "igc" gives DriverKnown|DriverBothEdges; "bcmgenet" or "macb" makes satpulse read
  `/sys/class/net/X/phydev/phy_id` and a BCM54210 id gives the CM4/CM5 profile (one edge, 4 Hz
  poll, carrier waiting); any other name gives no flags, the generic path, where the number of
  edges is learned from the extts enable result. The Broadcom profile cannot be faked without a
  phylib PHY attached to the interface, so it is out of scope.
- **Open-time checks** (`ts.OpenClock`, `validatePinDesc`): interface must exist; a warning if it
  is not UP; n_ext_ts > 0 and n_pins > 0; pin index < n_pins; channel < n_ext_ts. Carrier is
  only waited for in the Broadcom profile.
- **Start** (`ts.StartWorker`): PTP_PIN_SETFUNC(pin, PTP_PF_EXTTS, chan), then
  PTP_EXTTS_REQUEST2 with PTP_ENABLE_FEATURE|PTP_RISING_EDGE|PTP_STRICT_FLAGS; on EOPNOTSUPP it
  retries rising|falling, then plain PTP_EXTTS_REQUEST (`time/phc/phc.go` ExttsEnable).
- **Events**: poll() then read() of one `ptp_extts_event` (`ReadExtts`), index = channel.
- **Cross-timestamps** (`SysOffset`): PTP_SYS_OFFSET_PRECISE first (needs the driver's
  getcrosststamp), then PTP_SYS_OFFSET_EXTENDED with 6 samples (needs gettimex64). There is no
  fallback to plain PTP_SYS_OFFSET, so **gettimex64 is mandatory** for the fake.
- **Clock control**: clock_gettime; clock_adjtime ADJ_SETOFFSET|ADJ_NANO (adjtime);
  ADJ_FREQUENCY (adjfine, scaled ppm); reading tx.freq back (the ptp core returns the dialed
  value, no driver involvement); max_adj from PTP_CLOCK_GETCAPS.
- **Shutdown and carrier loss**: PTP_EXTTS_REQUEST with enable off (`logExttsEnable`).
- **satpulse never compares GNSS time with the system clock.** The only `time.Now()` uses outside
  read times and timeouts are configuration pacing (`gps/app/gpscfg`) and log replay. A guest
  clock that is wrong by years does not trip anything in the daemon.

## Lock thresholds (phcsync defaults)

| Check | Default | Depends on |
|---|---|---|
| Reset: pulse interval variation (`reset.go` PulseVariation) | 500 ppb, i.e. 500 ns per 1 s | PHC timestamp arithmetic only |
| Reset: pulse-to-message delay (ExpectedDelay 0.1 s, DelayConfidenceWindow 0.6, DelayVariation 0.2) | delay in a 0.6 s window containing 0.1 s, spread under 0.2 s, over PulseWindow = 5 pulses | process scheduling and serial delivery |
| Converging: OffsetLimit, StableWindow | 1 us for 3 samples | PHC timestamp arithmetic only |
| Tracking: OutlierThreshold, MADThreshold, AlignTolerance | 500 ns, 50 ns, 100 ns | PHC timestamp arithmetic only |

Everything except the message delay compares the PHC timestamp of a pulse with earlier pulses and
with GNSS time; none of it looks at when the daemon read the event (TReadMono feeds NTP samples
and logs only). Consequence: a fake that stamps a pulse with the PHC time of its **nominal** edge
is exact, however late the code that emits it runs. A fake that stamps "now" when a timer fires
would fail reset mode in every environment (timer latency is tens of microseconds under KVM,
milliseconds under TCG).

## Why replaying packet logs does not work for this

Reset mode pairs pulses with messages by the pulse-to-message delay (above). `satpulsetool pack
--realtime` paces packets against the first packet's log time and an arbitrary wall time
(`internal/packcmd/packcmd.go`), so the phase of replayed messages relative to any fake pulse is
random, and the messages carry GNSS time from the capture date. A receiver simulator that emits
the pulse and then the messages for that second is the right shape; log replay is not.

## The receiver simulator that exists

`gps/app/ubxsim` (`satpulsetool ubxsim`) is a hardware-free u-blox receiver for smoke tests: it
answers CFG-VALGET/VALSET etc. with ACK/NAK semantics and replays a bank of recorded nav epochs on
a timer (`nav.go` navEngine). It has no notion of true time. The u-blox messages satpulse turns
into time messages are NAV-TIMEGPS, NAV-TIMEUTC, NAV-PVT, TIM-TP (PrePulse, for the next pulse)
and TIM-TOS (PostPulse) (`gps/internal/ubx/ubxtime.go`); `gpsprot.TimeMsg` carries TAI/UTC time,
accuracy, PulseOffset, GNSS and Ref. The code to read a PHC cross-timestamp (what reading
/dev/ptp_kvm needs) is `time/phc` SysOffsetPrecise.

## Kernel PTP core facts (ptp_clock.c, ptp_chardev.c)

- `ptp_clock_register` requires gettime64 or gettimex64, and settime64.
- PTP_PIN_SETFUNC calls `info->verify` unconditionally (ptp_chardev.c:108): with n_pins > 0 the
  driver must supply verify and a `pin_config` array of n_pins entries. The core holds
  `pincfg_mux` around every `->enable` call from the ioctls.
- extts flag check: PTP_EXTTS_REQUEST2 adds PTP_STRICT_FLAGS and requires an edge bit; flags not
  in `info->supported_extts_flags` (plus PTP_ENABLE_FEATURE, plus both edges unless the driver
  sets PTP_STRICT_FLAGS) give EOPNOTSUPP. `supported_extts_flags` exists since 6.15; older
  kernels need a version guard. bcm-phy-ptp uses PTP_STRICT_FLAGS|PTP_RISING_EDGE; igb i210 uses
  rising|falling|strict.
- PTP_SYS_OFFSET_EXTENDED returns EOPNOTSUPP when gettimex64 is NULL; PRECISE likewise for
  getcrosststamp.
- `ptp_clock_event(PTP_CLOCK_EXTTS)` enqueues on every reader whose channel mask includes
  `event->index` and wakes pollers (ptp_clock.c:557).
- The dialed frequency is stored by the core (`ptp->dialed_frequency`) and returned on read.
- `drivers/ptp/ptp_mock.c` (for netdevsim): timecounter over `ktime_get_raw_ns`, shift 31,
  max_adj 32,000,000 ppb, adjfine changes `cc.mult`, do_aux_work refreshes every 5 s because the
  timecounter delta must stay under about 8.3 s. It has no pins, no extts, no gettimex64. Good
  model for the clock arithmetic; not usable as is (SysOffsetExtended would fail).
- `hrtimer`s run on CLOCK_MONOTONIC (or REALTIME/BOOTTIME), not MONOTONIC_RAW.
- `ktime_get_snapshot()` gives system time and raw monotonic at one instant (field names changed
  across versions: `real`/`raw` earlier, `systime`/`monoraw` in this tree).

## Host true time inside a guest

- **ptp_kvm** (`drivers/ptp/ptp_kvm_*.c`): host CLOCK_REALTIME cross-timestamped with the guest
  counter, read with PTP_SYS_OFFSET_PRECISE on /dev/ptp_kvm; the result includes device time,
  system realtime and system monoraw. Requires KVM: x86 init fails without the KVM paravirt
  CPUID (`kvm_para_available`), arm64 needs the hypervisor's SMCCC call. **A TCG guest has no
  /dev/ptp_kvm.** The x86 helper is not exported, so a module cannot call it; user space can.
- The other virtual PHC drivers (hyperv in `drivers/hv/hv_util.c`, ptp_vmw, ptp_vmclock,
  virtio_rtc) all have n_ext_ts = 0 and n_pins = 0. None does external timestamps.
- QEMU's vmclock device (`hw/acpi/vmclock.c`, web) only maintains a disruption marker; it never
  fills in clock parameters (counter INVALID, status UNKNOWN). Not a time source.
- QEMU's virtio-rtc device (`hw/virtio/virtio-rtc.c`, in master since 2026-06, web) answers
  reads with the host wall clock over a virtqueue, no accelerator dependence, no counter-based
  cross-timestamp, no alarms. Far newer than any distro QEMU. A possible TCG true-time source
  later, not now.
- QEMU `-rtc`: the guest RTC follows host time by default; `-rtc base=<datetime>` sets it wrong
  deliberately (QEMU invocation docs, web). `-icount` is a TCG-only deterministic mode; without
  it guest timers run in real time.
- TCG is QEMU's software CPU emulator (Tiny Code Generator), used when there is no KVM; ten to
  fifty times slower than KVM with coarser timer behaviour.

## CI facts

- satpulse workflows run on `ubuntu-latest` with sudo (`.github/workflows/build-linux.yml` runs
  `smoketest/run.py --sudo`; `system-smoke.yml` installs the deb and runs `smoketest/system.py`
  as root). The smoke harness has `REQUIRES_ROOT` scenarios. `smoketest/system.py` already
  renders `[phc] interface = ""`.
- GitHub standard x86 runners for public repositories: 4 vCPU, 16 GB (web). /dev/kvm on them is
  reported working by the Determinate Systems and Lix Nix installer actions, which enable it by
  default; GitHub's own docs call nested virtualization unsupported but possible. **Unverified
  for this repo until a workflow prints `ls -l /dev/kvm` and boots a guest.** Evidence is for
  public repos; private repos use 2 vCPU runners and nothing found says whether they get KVM.
- GitHub arm64 hosted runners (public since 2025) do **not** expose KVM (runner-images issue
  #14062 closed as not planned, web). Hence the guest is x86_64.
- satpulsed and satpulsetool are statically linked Go binaries, so any cloud image serves as the
  guest root without libc concerns.

## Previous fake hardware (bcm-phy-ptp-deadlock)

`~/src/bcm-phy-ptp-deadlock/tools/qemu/`: `fake-bcm-mdio.c` (71 lines) is an MDIO bus with a
fake BCM54210E so that bcm-phy-ptp registers a PHC in QEMU; `ptpstress.c` a static /init;
`qemu.fragment` an allnoconfig fragment; `initramfs.list`; run with
`qemu-system-aarch64 -M virt -cpu max -smp 4 -m 1G -nographic -no-reboot -kernel Image -initrd
initramfs.cpio` under TCG on cantal. Useful as a reference for driving QEMU and reading a result
line from the console. Not reused for the fake itself: satpulse needs a netdev in front of the
PHC (a fake MAC attaching the PHY) and the fake PHY would have to emulate Broadcom's timestamp
registers; that is a Broadcom-specific second profile at best. Emulating an i210 for igb would
need a QEMU PCI device model (igb talks through MMIO and MSI-X), which is heavier still.

## Local trial hosts

maasdam and edam (both desk, wired, Fedora 44 kernel 7.1.10, /dev/kvm present and world
accessible, passwordless sudo, no QEMU installed yet) were chosen by jjc as the places to try
this locally. maasdam: i5-12600H, 16 threads, 32 GB, 873 GB free. edam: N5105, 4 threads, 16 GB,
45 GB free. QEMU is an apt/dnf install of a few tens of MB over wired LAN. cantal has no
/dev/kvm (it is itself a VM), so QEMU on cantal is TCG only.

## Sources (web)

- GitHub-hosted runners reference: https://docs.github.com/en/actions/reference/runners/github-hosted-runners
- Determinate Systems, KVM on GitHub Actions: https://determinate.systems/posts/kvm-on-github-actions
- Lix install action, KVM out of the box: https://git.lix.systems/lix-project/lix-install-action/commit/07b8bcba1b22d847db7ee507180c33e115499665
- runner-images, KVM on ARM runners (closed): https://github.com/actions/runner-images/issues/14062
- arm64 runners GA for public repos: https://github.blog/changelog/2025-08-07-arm64-hosted-runners-for-public-repositories-are-now-generally-available/
- QEMU invocation docs (-rtc, -icount): https://www.qemu.org/docs/master/system/invocation.html
- QEMU vmclock device: https://raw.githubusercontent.com/qemu/qemu/master/hw/acpi/vmclock.c
- QEMU virtio-rtc device: https://raw.githubusercontent.com/qemu/qemu/master/hw/virtio/virtio-rtc.c
- vmclock QEMU series v7: https://lists.gnu.org/archive/html/qemu-devel/2025-01/msg02905.html
- virtme-ng (considered, not chosen): https://sources.debian.org/src/virtme-ng/1.40-2/README.md
