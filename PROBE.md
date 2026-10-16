# Runner probe result (2026-10-16)

Workflow `.github/workflows/probe.yml`, run 38831399920 on `ubuntu-latest` (public repo):

- `/dev/kvm` exists on the runner (root:kvm 0660; `chmod a+rw` makes it usable); 4 vCPUs
  with virtualization flags; lockdown `[none]`.
- QEMU from apt installs in about half a minute. The Debian 13 genericcloud amd64 image
  downloads in seconds.
- The guest booted under KVM (`-machine q35,accel=kvm -cpu host`): cloud-init reached
  `modules:final` 8.9 s after boot; the whole guest run, including an apt install of linuxptp,
  took 45 s.

Not yet established: whether the Debian cloud kernel has ptp_kvm and reads host time with the
guest clock set wrong. The first run's in-guest script did not execute (a YAML quoting error,
fixed in the next commit); a second run was started and its console log is the `probe-console`
artifact of the latest run. If ptp_kvm is missing, pick an image whose kernel has it; this is an
image choice, not a design question.
