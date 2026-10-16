#!/bin/bash
# Boot the Debian cloud image with a cloud-init seed that reports, on the serial
# console, what the guest sees: its kernel, its PTP clocks, whether ptp_kvm exists,
# and host time read through ptp_kvm after the guest clock has been set wrong.
set -eu
cd "$(dirname "$0")"
img=${IMG:-disk.qcow2}

cat > user-data <<'YAML'
#cloud-config
package_update: true
packages: [linuxptp]
write_files:
  - path: /root/probe.sh
    permissions: '0755'
    content: |
      #!/bin/bash
      exec >/dev/ttyS0 2>&1
      set -x
      uname -r
      for p in /sys/class/ptp/ptp*; do echo "$p: $(cat $p/clock_name)"; done
      modprobe ptp_kvm || echo 'modprobe ptp_kvm failed'
      for p in /sys/class/ptp/ptp*; do echo "$p: $(cat $p/clock_name)"; done
      ls -l /dev/ptp*
      date -u
      date -s '2020-01-01 00:00:00' >/dev/null && echo 'guest clock set wrong'
      date -u
      for p in /dev/ptp[0-9]*; do echo "== $p"; phc_ctl $p get cmp; done
      echo PROBE-DONE
runcmd:
  - [ /root/probe.sh ]
  - [ poweroff ]
YAML
printf 'instance-id: probe-1\nlocal-hostname: probe\n' > meta-data
cloud-localds seed.img user-data meta-data

accel=tcg; cpu=max
if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then accel=kvm; cpu=host; fi
echo "accel=$accel cpu=$cpu"
t0=$(date +%s)
timeout 1200 qemu-system-x86_64 -machine q35,accel=$accel -cpu $cpu -smp 2 -m 2048 \
  -display none -monitor none -no-reboot \
  -drive file=$img,format=qcow2,if=virtio \
  -drive file=seed.img,format=raw,if=virtio \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -serial file:console.log || echo "qemu exit status $?"
echo "guest ran for $(( $(date +%s) - t0 )) s (accel=$accel)"
echo "===== console.log (tail) ====="
tail -n 120 console.log
echo "===== summary ====="
grep -E "running 'modules:final'|Up [0-9.]+ seconds" console.log | tail -2 || true
grep -qx PROBE-DONE console.log && echo "RESULT: probe script completed" || { echo "RESULT: probe script did not complete"; exit 1; }
grep -q "KVM virtual PTP" console.log && echo "RESULT: ptp_kvm present in guest" || echo "RESULT: no ptp_kvm in guest (TCG or module missing)"
