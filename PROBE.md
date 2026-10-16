# Runner probe result (2026-10-16)

Workflow `.github/workflows/probe.yml`, run 38831399920 on `ubuntu-latest` (public repo):

- `/dev/kvm` exists on the runner (root:kvm 0660; `chmod a+rw` makes it usable); 4 vCPUs
  with virtualization flags; lockdown `[none]`.
- QEMU from apt installs in about half a minute. The Debian 13 genericcloud amd64 image
  downloads in seconds.
- The guest booted under KVM (`-machine q35,accel=kvm -cpu host`): cloud-init reached
  `modules:final` 8.9 s after boot; the whole guest run, including an apt install of linuxptp,
  took 45 s.

Second run (38831693128), with the in-guest script fixed:

- The Debian 13 cloud kernel has ptp_kvm: `modprobe ptp_kvm` registered ptp0 "KVM virtual PTP"
  and udev created `/dev/ptp_kvm`.
- With the guest clock set to 2020-01-01, `phc_ctl /dev/ptp0 get cmp` read host time
  (2026-10-16 00:50:57), i.e. true time from the host is available regardless of the guest clock.

So the Debian genericcloud image is usable as the guest image as it is, and the true-time source
for the simulator exists in CI.
