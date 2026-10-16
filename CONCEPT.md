# virt-phc-extts: goal and concept

## Goal

Smoke-test satpulse's PTP functionality in CI (and on any developer machine) without timing
hardware: a QEMU guest gets a PHC with one external-timestamp pin, a simulated GNSS receiver
drives it with pulses and time messages at true seconds, and satpulsed, satpulsetool sdp and
the rest of the chain run inside the guest exactly as they do on hardware.

True time is the host's clock, not the guest's. The guest's own clock may be deliberately wrong,
because things must work when the machine's time is wrong; the fake receiver still reports the
true time, as a real receiver would. A related benefit, not pursued first: any KVM guest can then
be a PTP grandmaster for testing (satpulse plus ptp4l), without a NIC that has a PHC.

We are testing satpulse, not the kernel. Nothing here needs to be upstreamable, so the
convenient shape wins.

## Non-goals

- No periodic output (perout), no PPS device, no hardware packet timestamping, no real Ethernet.
- One pin, one extts channel. Nothing is gained by more.
- No Broadcom (CM4/CM5) driver profile: it needs a phylib PHY under the interface.
- No kernel debugging configuration (lockdep etc.); the guest kernel is a stock distro kernel.

## The three pieces

### 1. The kernel module: a virtual PHC with an extts pin

An out-of-tree module, built against the guest kernel's headers, that registers:

- **A PHC** (`ptp_clock_register`): a timecounter over the guest's raw monotonic clock, as
  `drivers/ptp/ptp_mock.c` does (shift 31, periodic refresh through do_aux_work so the
  timecounter delta never overflows). A reading is the raw clock scaled by a multiplier plus an
  offset. `adjfine` changes the multiplier, `adjtime` and `settime64` the offset. Two parameters
  make it behave like hardware instead of a copy of the guest clock: `freq_error_ppb`, a built-in
  frequency error applied on top of whatever user space dials, so the PHC drifts until satpulse
  steers it, and `initial_offset_ns`, so reset mode has a step to make. `max_adj` as the i210
  (62,499,999 ppb). `gettimex64` brackets the read with `ptp_read_system_prets/postts` (needed
  by satpulse's PTP_SYS_OFFSET_EXTENDED path). `getcrosststamp` is exact, since the PHC is
  defined on the guest's own clocksource: one `ktime_get_snapshot` gives raw and realtime at
  the same instant. A parameter `no_crosststamp` hides it so satpulse's fallback path is
  exercised too.
- **One pin, one channel**: `n_pins = 1`, `n_ext_ts = 1`, `n_per_out = 0`, `pps = 0`,
  `pin_config[0]` named "SDP0", `verify` accepting PTP_PF_NONE and PTP_PF_EXTTS on channel 0.
  `supported_extts_flags = PTP_STRICT_FLAGS | PTP_RISING_EDGE` (as bcm-phy-ptp), so satpulse's
  first attempt (rising edge, strict) succeeds and the fallbacks are left alone. `enable`
  records whether extts is on. (Guard `supported_extts_flags` by kernel version; before 6.15
  the field does not exist and edges are hints.)
- **A dummy Ethernet device** ("vphc%d"): `ndo_start_xmit` frees the skb; open sets carrier on;
  ethtool `get_ts_info` returns the PHC index; ethtool `get_drvinfo` returns the module
  parameter `driver_name` (default the module's own name, giving satpulse's generic profile;
  "igb" gives the known/both-edges profile). The test brings the interface UP, since satpulse
  warns when it is down.

**Where the edges come from.** The module never generates pulses from any clock. User space
schedules each edge by writing the edge's time, in CLOCK_MONOTONIC_RAW nanoseconds, to a
write-only sysfs attribute (`edge`) of the module's device. The module keeps a small ordered
queue of pending edges and one hrtimer for the earliest (converted raw to monotonic for the
timer only). When the timer fires it emits `ptp_clock_event(PTP_CLOCK_EXTTS)` with index 0 and
timestamp = the PHC time of the **nominal** edge time, computed through the timecounter from
the written raw value, never from the time the timer ran. So timestamps are exact however late
the timer is, and edges scheduled one second apart give PHC intervals that differ only by the
configured drift; this is what makes reset mode's 500 ppb interval check and tracking's 50 ns
MAD attainable even under TCG. Edges in the past, or more than a few seconds ahead, are rejected
with EINVAL, so the simulator always schedules a little ahead and the question of which
frequency was in effect never arises. An edge is dropped silently if the pin is not assigned to
extts or extts is not enabled, as hardware would ignore it.

**Why CLOCK_MONOTONIC_RAW for the edge time.** It is the module's own base clock, so no
conversion is involved in the timestamp, and PTP_SYS_OFFSET_PRECISE on /dev/ptp_kvm returns the
system monoraw alongside the host time, so the simulator gets the mapping it needs directly.

### 2. The receiver simulator: true time, pulses and messages

A timing mode for `satpulsetool ubxsim` (satpulse code, on a branch of satpulse). It takes a
true-time source:

- `ptp_kvm`: cross-timestamps from /dev/ptp_kvm (host CLOCK_REALTIME against guest monoraw),
  fitted over a window so that pulses are scheduled from a line, not from individual noisy
  samples; the fit's rate error shows up as PHC frequency error, which satpulse absorbs.
- the guest clock (CLOCK_REALTIME against monoraw), for TCG or for runs where the host link is
  not wanted.

For every true second it writes the edge's monoraw time to the module's `edge` attribute a
little ahead, then, after the configured pulse-to-message delay (default about 100 ms, like a
real receiver), emits the u-blox messages for that second on the serial side, with the field
semantics from the u-blox interface description (vendor docs location in satpulse's
CLAUDE.local.md): NAV-TIMEUTC, NAV-TIMEGPS, NAV-PVT, TIM-TOS (post-pulse) and TIM-TP
(pre-pulse, for the next pulse, with the quantisation error as PulseOffset). Configuration
answering stays as it is. Options for the delay, for dropped pulses and for message gaps come
later; the first version is a perfect receiver.

The serial side is a pty, as the smoke harness already does for ubxsim.

### 3. The harness: a guest we control, run from a script

- A stock Debian or Ubuntu x86_64 cloud image. Customised once (headers and compiler so the
  module builds against exactly that kernel; python3; later linuxptp and chrony) and stored as a
  release asset of this repo, so a test run downloads one cached file.
- One script starts QEMU: KVM when /dev/kvm exists, TCG otherwise; a cloud-init NoCloud seed
  carrying the module source, the static satpulse binaries and the test script; serial console
  to a file; `-rtc base=<wrong date>` when the scenario wants a wrong guest clock. In the guest,
  cloud-init builds and loads the module, brings up vphc0, starts the simulator and satpulsed,
  runs the checks, prints a result line and powers off. The runner reads the console log.
- The same script runs locally (maasdam or edam, KVM) and in GitHub Actions (public repo,
  `ubuntu-latest`, x86 runner). The satpulse smoke harness's REQUIRES_ROOT conventions and
  result conventions are the model.

## Scenarios for the first version

1. Module loads; `satpulsetool sdp --show-ifaces` lists vphc0 with one pin and one channel.
2. `satpulsetool sdp` extts mode shows injected edges with the expected PHC timestamps.
3. satpulsed with `[phc] interface = "vphc0"` and the simulator: reaches tracking; the dialed
   frequency equals minus `freq_error_ppb` within tolerance; offset within tracking limits;
   SIGINT shutdown disables extts and exits 0 without hanging.
4. Scenario 3 with the guest clock set wrong by years (`-rtc base=`). Same outcome.
5. Scenario 3 with `no_crosststamp=1` (PTP_SYS_OFFSET_EXTENDED path).

Later: ptp4l as grandmaster in the guest controlled through satpulse's pmc path; a second guest
as client; the Broadcom profile if a cheap way appears.

## Order of work

0. **Find out what GitHub public runners support** (this comes first): a workflow that prints
   `/dev/kvm`, installs QEMU, boots a cloud image under KVM (TCG fallback) with a cloud-init
   seed, and inside the guest shows /dev/ptp_kvm exists and reads host time against a
   deliberately wrong guest clock. Record the boot time and whether KVM was used.
1. The module, with a small C or Go tool (testptp-like) to assign the pin, enable extts, write
   edges and read events, so the module can be checked without satpulse.
2. The simulator timing mode in satpulse.
3. The harness script and the image recipe; the five scenarios.
4. The workflow that runs them; then how satpulse's own CI uses it.
