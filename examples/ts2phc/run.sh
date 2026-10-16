#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# A disposable virtual PHC is the only clock this demo adjusts.
set -euo pipefail
demo_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(cd -- "$demo_dir/../.." && pwd)
log_dir=${1:-$(mktemp -d /tmp/vphc-ts2phc.XXXXXX)}
mkdir -p "$log_dir"
log_dir=$(cd -- "$log_dir" && pwd)
if (( EUID != 0 )); then
    echo "Run as root (module loading and virtual PHC adjustment)." >&2
    exit 1
fi
for command in insmod rmmod ts2phc awk readlink; do
    command -v "$command" >/dev/null
done
if [[ -d /sys/module/virt_phc_extts ]]; then
    echo "virt_phc_extts is already loaded; unload it before this demo." >&2
    exit 1
fi
[[ -x "$repo_dir/tools/vphc-pulse" && -f "$repo_dir/module/virt_phc_extts.ko" ]] || {
    echo "Build the module and tools first; see examples/ts2phc/README.md." >&2
    exit 1
}
loaded=0
pulse_pid=
servo_pid=
cleanup() {
    local result=$?
    trap - EXIT
    if [[ -n $pulse_pid ]]; then
        kill -TERM "$pulse_pid" 2>/dev/null || true
        wait "$pulse_pid" || true
    fi
    if [[ -n $servo_pid ]]; then
        kill -TERM "$servo_pid" 2>/dev/null || true
        wait "$servo_pid" || true
    fi
    if (( loaded )); then
        rmmod virt_phc_extts || result=1
    fi
    echo "Logs: $log_dir"
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
insmod "$repo_dir/module/virt_phc_extts.ko" freq_error_ppb=10000 initial_offset_ns=10000000000
loaded=1
target=
for phc in /sys/class/ptp/ptp*; do
    if [[ $(<"$phc/clock_name") == virt-phc-extts &&
          $(readlink -f "$phc") == /sys/devices/platform/virt-phc-extts/ptp/"${phc##*/}" ]]; then
        target=/dev/${phc##*/}
        break
    fi
done
[[ -n $target ]] || { echo "Virtual PHC was not found." >&2; exit 1; }
echo "Disciplining $target with ts2phc generic PPS; injected drift +10000 ppb, offset +10 s."
ts2phc -s generic -c "$target" -f "$demo_dir/ts2phc.cfg" -m >"$log_dir/ts2phc.log" 2>&1 &
servo_pid=$!
"$repo_dir/tools/vphc-pulse" >"$log_dir/pulse.log" 2>&1 &
pulse_pid=$!
for ((elapsed=0; elapsed<120; elapsed++)); do
    if ! kill -0 "$servo_pid" 2>/dev/null || ! kill -0 "$pulse_pid" 2>/dev/null; then
        cat "$log_dir/ts2phc.log" "$log_dir/pulse.log" >&2
        echo "A demo process exited unexpectedly." >&2
        exit 1
    fi
    # linuxptp prints '<clock> offset <ns> s<state> freq <ppb>'.
    if awk '
        / offset / {
            for (i=1; i<=NF; i++) if ($i == "offset") {
                offset=$(i+1); state=$(i+2);
                if ((state == "s2" || state == "s3") && offset < 1000 && offset > -1000)
                    good++;
                else good=0;
                if (good >= 10) passed=1;
            }
        }
        END {exit !passed}
    ' "$log_dir/ts2phc.log"; then
        tail -n 12 "$log_dir/ts2phc.log"
        echo "PASS: ten consecutive locked samples with absolute offset below 1000 ns."
        exit 0
    fi
    sleep 1
done
tail -n 20 "$log_dir/ts2phc.log" >&2
echo "FAIL: ts2phc did not converge within 120 seconds." >&2
exit 1
