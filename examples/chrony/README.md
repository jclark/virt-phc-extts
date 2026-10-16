# Receiving external PPS with chrony

This demo runs chrony's `PHC` refclock driver in `extpps` mode against the
virtual PHC. It works on an ordinary Linux machine without GNSS, a physical PHC,
KVM, or an independent time source. It exercises timestamp capture and chrony's
conversion from an unsynchronized PHC to system time.

Install `chrony`, a C compiler, make, and the running kernel's development
headers. From the repository root:

```sh
make -C module
make -C tools
sudo bash examples/chrony/run.sh
```

The module must initially be unloaded. The script loads its own virtual PHC with
a +10 second offset and +10000 ppb drift. `vphc-pulse` schedules 100 ms pulses at
whole system realtime seconds. The generated chrony configuration contains:

```text
refclock PHC /dev/ptpN:extpps:pin=0:channel=0 refid VPPS poll 0 dpoll -2 filter 1 width 0.1
local stratum 10
```

The script substitutes the actual virtual device. `extpps` captures rising-edge
timestamps; chrony maps these to system time using PHC cross-timestamps. The PHC
does not need to be synchronized. Pulses alone supply no second number;
`local stratum 10` permits this phase-only experiment using the current local
clock. It is the **global `local` directive**, not the refclock `local` option.

The process always runs with **`chronyd -x`**, so it does not adjust the system
clock. Its configuration and logs are separate from the system service, with
uniquely named PID and command-socket files under `/run/chrony`.
NTP and command UDP ports are disabled; it does not disturb a system
chrony service. It also leaves the virtual PHC free-running. Both the scheduled
pulses and measured reference derive from the system clock: this validates the
capture/conversion path, not absolute UTC accuracy or synchronization to an
independent reference. For a real PPS source, another time source supplies the
second number; a `lock REFID` can associate PPS with a specific refclock.

Tested with chrony 4.6.1 on Debian 13 (rising and falling edges), and chrony 4.8
on Fedora 44 with SELinux enforcing (rising edges).

The script checks that chrony accepts at least ten PPS samples and estimates
an offset below 1 microsecond. It saves `chronyd.log`, `sources.log`,
`sourcestats.csv`, `sourcestats.log`, and `tracking.log` in a printed directory.
It then stops both processes, clears the schedule, and unloads the module.
The generated configuration remains in a printed runtime directory. Directives
are passed as command-line arguments; unique socket and PID files live under
`/run/chrony`, which also works with Debian's default AppArmor policy.
Reports are captured under `/tmp` before copying to the chosen log directory
to accommodate Fedora's confined `chronyc` client.

To exercise falling-edge capture as well:

```sh
sudo bash examples/chrony/run.sh /tmp/chrony-falling-demo falling
```

This adds `:clear` to the PHC driver parameter and `offset 0.1` to account for the
100 ms between the rising edge's whole second and the falling edge. The pulse
generator still submits both edges; the capture request selects the falling
ones.

The [chrony configuration manual](https://chrony-project.org/doc/4.8/chrony.conf.html#refclock)
describes `PHC`, `extpps`, `clear`, and reference-clock options; the
[chronyd manual](https://chrony-project.org/doc/4.8/chronyd.html) documents `-x`.
