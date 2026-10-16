# Implementation prompt for virt-phc-extts

Paste the text below the line into a new session started in `~/src/virt-phc-extts`.

---

Implement the virt-phc-extts project. Read `BACKGROUND.md` (what was found and why) and
`CONCEPT.md` (what to build) in this directory first; they are the specification. The decisions
in them are settled: one pin, one extts channel, no periodic output, edges scheduled by user
space in CLOCK_MONOTONIC_RAW nanoseconds and timestamped at their nominal time, a dummy netdev
in front of the PHC, true time from the host via /dev/ptp_kvm, an x86_64 guest from a cloud
image we control, satpulse unchanged except for the ubxsim timing mode on a branch.

Where things are:
- satpulse: `~/src/satpulse` (read its `CLAUDE.md`; Go style, make, smoke harness rules apply to
  any satpulse change; the simulator work goes on a branch there, not on master).
- kernel source for reference: `~/src/linux` (shallow, net-next). Use it to read the ptp core
  and `drivers/ptp/ptp_mock.c`; the module is built against the guest kernel's headers, not
  against this tree.
- QEMU usage reference: `~/src/bcm-phy-ptp-deadlock/tools/qemu/` and the "QEMU test" section
  of `~/src/bcm-phy-ptp-deadlock/HANDOFF.md`.
- This directory is the public GitHub repo `virt-phc-extts`; the probe workflow and its result
  are in `.github/workflows/` and `PROBE.md` if the probe has already been run.

Order of work, each step checked before the next:
0. If `PROBE.md` does not exist or says the probe did not run: add the probe workflow described
   in CONCEPT.md "Order of work" step 0, push, watch the run, and record in `PROBE.md` whether
   /dev/kvm exists on the runner, whether the guest booted under KVM or TCG, how long the boot
   took, and whether /dev/ptp_kvm existed in the guest. Stop and report if KVM is not available:
   the design still works under TCG but the true-time source changes, and jjc decides.
1. The module (`module/`), with a Makefile for out-of-tree build, plus a small test tool
   (`tools/`) that sets the pin function, enables extts, writes edges and prints events. Verify
   locally in a QEMU guest (KVM on maasdam or edam, TCG on cantal; QEMU is not installed on
   maasdam or edam yet, say so and ask before installing). Check: timestamps of edges one second
   apart differ by exactly 1 s plus the configured drift; events stop when extts is disabled;
   PTP_SYS_OFFSET_PRECISE and _EXTENDED both work; `satpulsetool sdp --show-ifaces` lists the
   interface.
2. The ubxsim timing mode in satpulse, on a branch. Message field semantics come from the
   u-blox interface description named in satpulse's CLAUDE.local.md, never from memory.
3. The harness (`harness/`): image recipe, run script, cloud-init seed, the five scenarios from
   CONCEPT.md, result line on the console.
4. The workflow that runs the scenarios on push.

Rules:
- Ask before anything costly: large downloads, installing packages on lab hosts, long builds.
  Cloud images are a few hundred MB; say the size and the path's speed first.
- Do not touch lab hosts other than maasdam and edam, and nothing on them beyond what the trial
  needs. Read `~/src/satpulse-systest/CLAUDE.md` before using them.
- Never mention AI tools in commits or the repo. Commit messages explain what and why.
- Keep the module small and plain C in kernel style; no generality beyond the concept.
- Report deltas and failures plainly; when a step is verified, say how.
