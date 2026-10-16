# Implementation prompt for virt-phc-extts

Paste the text below the line into a new session started in `~/src/virt-phc-extts`.

---

Implement the virt-phc-extts project. Read `BACKGROUND.md` (what was found and why) and
`CONCEPT.md` (what to build) in this directory first; they are the specification. The decisions
in them are settled: one pin, one extts channel, no periodic output, edges scheduled by user
space in CLOCK_MONOTONIC_RAW nanoseconds and timestamped at their nominal time, a dummy netdev
in front of the PHC, true time from the host via /dev/ptp_kvm, an x86_64 guest from a cloud
image we control, satpulse unchanged except for the ubxsim timing mode on a branch.

Where things are (this session runs on maasdam, an x86 Fedora 44 host with KVM):
- This directory, `~/src/virt-phc-extts`, is the public GitHub repo `jclark/virt-phc-extts`
  (gh is logged in; commits as James Clark). The runner probe has been run; its result is in
  `PROBE.md`.
- satpulse is not checked out on maasdam. Clone `https://github.com/jclark/satpulse` into
  `~/src/satpulse` (about 80 MB) and read its `CLAUDE.md`; Go style, `make`, and the smoke
  harness rules apply to any satpulse change. The simulator work goes on a branch there, not on
  master. Go 1.26 is installed.
- The u-blox interface description (needed for the ubxsim timing mode) is not on maasdam and
  satpulse has no `CLAUDE.local.md` here: ask jjc where it is before step 2.
- There is no kernel tree on maasdam. For the ptp core and `drivers/ptp/ptp_mock.c`, fetch the
  individual files from https://git.kernel.org (raw, small) or install `kernel-devel` for
  the headers. The module is built against the guest kernel's headers in the guest.
- QEMU is not installed on maasdam (`qemu-system-x86-core` and friends, a few tens of MB from
  Fedora's mirrors); say so and ask before installing. `/dev/kvm` is present and world
  accessible.
- QEMU usage reference from the earlier deadlock work lives on cantal in
  `~/src/bcm-phy-ptp-deadlock/tools/qemu/` and `HANDOFF.md` there; maasdam cannot ssh to cantal
  until cantal's host key is accepted (all LAN hosts are trusted). The probe workflow in this
  repo is a working example of booting a cloud image with a cloud-init seed and reading the
  console.

Order of work, each step checked before the next:
0. Read `PROBE.md`: the runner has KVM and the guest boots under it. If it says ptp_kvm was
   missing in the Debian cloud kernel, the image recipe must use a kernel that has it (check
   Debian's non-cloud kernel or Ubuntu's cloud image) and the probe should be rerun with that
   image before step 3.
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
