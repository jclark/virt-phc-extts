#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
demo_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(cd -- "$demo_dir/../.." && pwd)
log_dir=${1:-$(mktemp -d /tmp/vphc-chrony.XXXXXX)}
polarity=${2:-rising}
case $polarity in
    rising) driver_options=; correction=0 ;;
    falling) driver_options=:clear; correction=0.1 ;;
    *) echo "Usage: $0 [LOG_DIRECTORY] [rising|falling]" >&2; exit 2 ;;
esac
if (( EUID != 0 )); then
    echo "Run as root (module loading and PTP pin configuration)." >&2
    exit 1
fi
for command in insmod rmmod chronyd chronyc awk stat readlink id chown cp; do
    command -v "$command" >/dev/null
done
if [[ -d /sys/module/virt_phc_extts ]]; then
    echo "virt_phc_extts is already loaded; unload it before this demo." >&2
    exit 1
fi
[[ -x "$repo_dir/tools/vphc-pulse" && -f "$repo_dir/module/virt_phc_extts.ko" ]] || {
    echo "Build the module and tools first; see examples/chrony/README.md." >&2
    exit 1
}
mkdir -p "$log_dir"
log_dir=$(cd -- "$log_dir" && pwd)
# Keep socket paths short, private, and independent of the system daemon.
runtime_dir=$(mktemp -d /tmp/vphc-chrony-run.XXXXXX)
if [[ ! -d /run/chrony ]]; then
    chrony_user=root
    for candidate in chrony _chrony; do
        if id "$candidate" >/dev/null 2>&1; then
            chrony_user=$candidate
            break
        fi
    done
    mkdir -m 750 /run/chrony
    chown "$chrony_user:$(id -gn "$chrony_user")" /run/chrony
    if command -v restorecon >/dev/null; then
        restorecon /run/chrony
    fi
fi
# chronyd requires its socket directory to belong to its runtime user.
chrony_user=$(stat -c %U /run/chrony)
instance=${runtime_dir##*/}
socket=/run/chrony/$instance.sock
loaded=0
pulse_pid=
chrony_pid=
cleanup() {
    local result=$?
    trap - EXIT
    if [[ -n $pulse_pid ]]; then
        kill -TERM "$pulse_pid" 2>/dev/null || true
        wait "$pulse_pid" || true
    fi
    if [[ -n $chrony_pid ]]; then
        kill -TERM "$chrony_pid" 2>/dev/null || true
        wait "$chrony_pid" || true
    fi
    if (( loaded )); then
        rmmod virt_phc_extts || result=1
    fi
    # Keep the generated configuration and runtime directory for inspection.
    echo "Logs: $log_dir; runtime files: $runtime_dir"
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
printf '%s\n' \
    "refclock PHC $target:extpps:pin=0:channel=0$driver_options refid VPPS poll 0 dpoll -2 filter 1 width 0.1 offset $correction" \
    'local stratum 10' \
    'port 0' \
    'cmdport 0' \
    "bindcmdaddress $socket" \
    "pidfile /run/chrony/$instance.pid" >"$runtime_dir/chrony.conf"
echo "Measuring $polarity PPS through $target; chronyd -x cannot adjust system time."
# Command-line directives avoid distro restrictions on custom configuration
# paths; the socket and PID use the distribution's standard runtime location.
mapfile -t config <"$runtime_dir/chrony.conf"
chronyd -x -d -u "$chrony_user" "${config[@]}" >"$log_dir/chronyd.log" 2>&1 &
chrony_pid=$!
"$repo_dir/tools/vphc-pulse" >"$log_dir/pulse.log" 2>&1 &
pulse_pid=$!
# Fedora confines chronyc separately: inherited /root log descriptors can be
# replaced by /dev/null. Capture under /tmp first, then copy as the script user.
report() {
    local name=$1
    shift
    chronyc -h "$socket" "$@" >"$runtime_dir/$name" || return
    cp "$runtime_dir/$name" "$log_dir/$name"
}
for ((elapsed=0; elapsed<60; elapsed++)); do
    if ! kill -0 "$chrony_pid" 2>/dev/null || ! kill -0 "$pulse_pid" 2>/dev/null; then
        cat "$log_dir/chronyd.log" "$log_dir/pulse.log" >&2
        echo "A demo process exited unexpectedly." >&2
        exit 1
    fi
    if report sourcestats.csv -c sourcestats 2>/dev/null && awk -F, '
        $1 == "VPPS" && $2 >= 10 && $7 < 0.000001 && $7 > -0.000001 {pass=1}
        END {exit !pass}
    ' "$log_dir/sourcestats.csv"; then
        report sources.log sources
        report sourcestats.log sourcestats
        report tracking.log tracking
        cat "$log_dir/sources.log" "$log_dir/sourcestats.log"
        echo "PASS: at least ten accepted PPS samples and estimated offset below 1 us."
        exit 0
    fi
    sleep 1
done
cat "$log_dir/chronyd.log" >&2
[[ ! -f $log_dir/sourcestats.csv ]] || cat "$log_dir/sourcestats.csv" >&2
echo "FAIL: chrony did not accept ten accurate PPS samples within 60 seconds." >&2
exit 1
