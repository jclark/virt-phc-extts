#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build and exercise the module in a persistent, isolated local KVM guest."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parent.parent
STATE = ROOT / ".local"
IMAGE_URL = ("https://cloud.debian.org/images/cloud/trixie/latest/"
             "debian-13-genericcloud-amd64.qcow2")
FEDORA_IMAGE_URL = ("https://dl.fedoraproject.org/pub/fedora/linux/releases/44/"
                    "Cloud/x86_64/images/Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2")


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def ssh_args(port):
    return ["ssh", "-i", STATE / "id_ed25519", "-p", str(port),
            "-o", "BatchMode=yes", "-o", "ConnectTimeout=3",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", f"UserKnownHostsFile={STATE / 'known_hosts'}",
            "root@127.0.0.1"]


def ssh(port, command, **kw):
    return run(ssh_args(port) + [command], **kw)


def wait_ssh(port):
    end = time.monotonic() + 180
    while time.monotonic() < end:
        p = subprocess.run([str(a) for a in ssh_args(port)] + ["true"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if p.returncode == 0:
            return
        time.sleep(1)
    raise RuntimeError(f"guest SSH did not start; inspect {STATE / 'console.log'}")


def prepare(port, distro):
    STATE.mkdir(parents=True, exist_ok=True)
    image = STATE / f"{distro}.qcow2"
    if not image.exists():
        download = STATE / f"{distro}.download"
        url = FEDORA_IMAGE_URL if distro == "fedora" else IMAGE_URL
        run(["curl", "-fL", "--retry", "3", "-o", download, url])
        download.rename(image)
    if not (STATE / "id_ed25519").exists():
        run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", STATE / "id_ed25519"])
    if not (STATE / "guest.qcow2").exists():
        run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2", "-b",
             image, STATE / "guest.qcow2", "12G"])
    config = {
        "disable_root": False,
        "users": [{"name": "root", "ssh_authorized_keys": [
            (STATE / "id_ed25519.pub").read_text().strip()]}],
        "ssh_pwauth": False,
        "package_update": True,
        "packages": ["build-essential", "linux-headers-cloud-amd64",
                     "linux-image-cloud-amd64", "linuxptp", "python3"],
        "runcmd": [["touch", "/root/guest-ready"]],
    }
    if distro == "fedora":
        config["packages"] = ["gcc", "make", "kernel", "kernel-devel",
                              "kernel-modules-extra", "elfutils-libelf-devel",
                              "openssl-devel", "linuxptp", "python3"]
    (STATE / "user-data").write_text("#cloud-config\n" + json.dumps(config))
    (STATE / "meta-data").write_text("instance-id: vphc-local-1\nlocal-hostname: vphc-test\n")
    if not (STATE / "seed.img").exists():
        run(["cloud-localds", STATE / "seed.img", STATE / "user-data", STATE / "meta-data"])
    # The lock belongs to QEMU itself, so a stale PID cannot trick us into
    # signalling an unrelated process after a host restart.
    pidfile = STATE / "qemu.pid"
    alive = False
    if pidfile.exists():
        pid = pidfile.read_text().strip()
        try:
            alive = str(STATE / "guest.qcow2") in Path(f"/proc/{pid}/cmdline").read_text()
        except FileNotFoundError:
            pass
    if not alive:
        kvm = os.access("/dev/kvm", os.R_OK | os.W_OK)
        run(["qemu-system-x86_64", "-machine", f"q35,accel={'kvm' if kvm else 'tcg'}",
             "-cpu", "host" if kvm else "max", "-smp", "4", "-m", "4096",
             "-display", "none", "-monitor", "none", "-daemonize",
             "-pidfile", pidfile, "-serial", f"file:{STATE / 'console.log'}",
             "-drive", f"file={STATE / 'guest.qcow2'},format=qcow2,if=virtio",
             "-drive", f"file={STATE / 'seed.img'},format=raw,if=virtio",
             "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:22",
             "-device", "virtio-net-pci,netdev=n0"])
    wait_ssh(port)
    try:
        ssh(port, "timeout 900 cloud-init status --wait --long")
    except subprocess.CalledProcessError as e:
        # Fedora may report a recoverable early-boot hostname/D-Bus warning,
        # then successfully set it during the network stage. Exit 2 denotes
        # warnings; retain the diagnostic above and verify readiness below.
        if e.returncode != 2:
            raise
    ssh(port, "test -f /root/guest-ready")
    ssh(port, "test \"$(cat /etc/hostname)\" = vphc-test")
    p = subprocess.run([str(a) for a in ssh_args(port)] +
                       ["test -d /lib/modules/$(uname -r)/build"])
    if p.returncode:
        ssh(port, "reboot")
        time.sleep(3)
        wait_ssh(port)
        ssh(port, "test -d /lib/modules/$(uname -r)/build")
    ssh(port, "uname -r")


def upload(port, payload, destination):
    paths = [ROOT / "module", ROOT / "tools", ROOT / "harness"]
    if (ROOT / "examples").is_dir():
        paths.append(ROOT / "examples")
    ssh(port, "mkdir -p /root/virt-phc-extts")
    tar = subprocess.Popen(["tar", "--exclude=__pycache__", "--exclude=test-vphc",
                            "--exclude=vphc-pulse", "--exclude=*.o",
                            "--exclude=*.ko", "-czf", "-", "-C", str(ROOT)] +
                           [p.name for p in paths], stdout=subprocess.PIPE)
    try:
        ssh(port, "tar -xzf - -C /root/virt-phc-extts", stdin=tar.stdout)
    finally:
        tar.stdout.close()
    if tar.wait():
        raise RuntimeError("source archive failed")
    if payload:
        tar = subprocess.Popen(["tar", "-czf", "-", "-C", str(payload), "."],
                               stdout=subprocess.PIPE)
        try:
            target = shlex.quote(destination)
            ssh(port, f"mkdir -p {target} && tar -xzf - -C {target}", stdin=tar.stdout)
        finally:
            tar.stdout.close()
        if tar.wait():
            raise RuntimeError("payload archive failed")
    # The same guest can boot different installed kernels. Clean copied Kbuild
    # metadata and prior objects before rebuilding against the running one.
    ssh(port, "make -C /root/virt-phc-extts/module clean && "
              "make -C /root/virt-phc-extts/module && "
              "make -C /root/virt-phc-extts/tools clean && make -C /root/virt-phc-extts/tools")


def test(port, command, artifacts):
    command = command or ["python3", "/root/virt-phc-extts/harness/guest.py"]
    p = subprocess.Popen([str(a) for a in ssh_args(port)] +
                         [shlex.join(command)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    with open(STATE / "results.log", "w") as log:
        for line in p.stdout:
            print(line, end="", flush=True)
            log.write(line)
            log.flush()
    rc = p.wait()
    if artifacts:
        # Collect regular files only: a test may leave PTY or socket symlinks.
        with open(STATE / "results.tar.gz", "wb") as archive:
            ssh(port, f"cd {shlex.quote(artifacts)} && "
                      "find . -type f -print0 | tar --null -T - -czf -",
                stdout=archive)
        with tarfile.open(STATE / "results.tar.gz") as archive:
            archive.extractall(STATE / "results", filter="data")
    if rc:
        raise RuntimeError(f"guest tests failed (status {rc}); transcript in {STATE / 'results.log'}")


def main():
    global STATE
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=["prepare", "upload", "test", "ssh", "stop"])
    p.add_argument("--distro", choices=["debian", "fedora"], default="debian")
    p.add_argument("--port", type=int)
    p.add_argument("--payload", type=Path, help="directory whose contents upload copies to --destination")
    p.add_argument("--destination", default="/root/payload", help="guest directory for --payload")
    p.add_argument("--artifacts", help="guest directory of files to collect after test, even on failure")
    p.add_argument("command", nargs="*")
    args = p.parse_args()
    if args.payload and not args.payload.is_dir():
        p.error("--payload must name a directory")
    if args.distro == "fedora":
        STATE = STATE / "fedora"
    if args.port is None:
        args.port = 22223 if args.distro == "fedora" else 22222
    if args.action == "prepare":
        prepare(args.port, args.distro)
    elif args.action == "upload":
        upload(args.port, args.payload, args.destination)
    elif args.action == "test":
        test(args.port, args.command, args.artifacts)
    elif args.action == "stop":
        ssh(args.port, "poweroff")
    else:
        ssh(args.port, shlex.join(args.command) if args.command else "uname -a")


if __name__ == "__main__":
    main()
