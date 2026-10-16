# virt-phc-extts

A virtual PTP hardware clock with one external-timestamp pin, for smoke-testing satpulse's
PTP functionality in a QEMU guest without timing hardware.

- `BACKGROUND.md`: what was found in satpulse, the kernel and the CI environment, and why.
- `CONCEPT.md`: the goal and the design.
- `PROMPT.md`: the brief for the implementation.
- `probe/`, `.github/workflows/probe.yml`: the first step, finding out what a GitHub-hosted
  runner supports.
