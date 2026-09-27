#!/usr/bin/env python3
"""Boot the converted macOS image under QEMU, and MEASURE what happened.

Why this exists
---------------
The VMware VM occupies ~46 GB on C: (33.8 GB of split-sparse VMDK extents, an
8 GB `.vmem` while powered on, plus a 3 GB BaseSystem.img and a 0.8 GB .dmg).
The same guest converted to a compressed qcow2 is 26.7 GiB, and QEMU itself is
1.2 GB installed. That is the whole low-storage argument, and it is measured
rather than assumed -- see QEMU-LOWSTORAGE.md.

What this script is NOT
-----------------------
It does not claim the guest boots. `--probe-seconds` runs QEMU headless, waits,
asks QMP for a framebuffer dump, and prints what it OBSERVED: whether QEMU
stayed alive, what it wrote to stderr, and how many bytes the screendump
produced. A PNG on disk is evidence; an exit code is a claim. Open the PNG.

The AppleSMC OSK
----------------
`isa-applesmc` has an `osk=` property and QEMU ships NO default for it. macOS's
AppleSMC kext validates that key, so a boot without it is expected to fail --
that failure is a legitimate measurement, not a bug in this script. The key is
read out of Apple hardware you own; this file does not carry one. Pass it with
`--osk` if you have it.

Handing the firmware an APFS driver (`--vvfat`)
-----------------------------------------------
QEMU-LOWSTORAGE.md §5 measured why this image does not boot: `boot.efi` lives in
the APFS **Preboot** volume, the ESP is empty apart from a volume label, and
stock edk2 has no APFS filesystem driver -- so `BdsDxe` reports `Not Found` on a
perfectly intact disk and falls through to the EFI shell.

`--vvfat DIR` exposes DIR to the guest as a read-only FAT volume, so an
`apfs.efi` driver plus a `startup.nsh` can be handed to that shell WITHOUT
writing a byte to the macOS image. That matters: a failed experiment costs a
rerun rather than a 26.7 GiB reconversion, and the VMware source stays the only
thing anything has booted.

Usage
-----
    python tools/qemu-boot-sonoma.py --dry-run
    python tools/qemu-boot-sonoma.py --probe-seconds 90 \
        --screenshot qemu-results/boot-probe.png
    python tools/qemu-boot-sonoma.py --probe-seconds 120 \
        --vvfat F:/nv3050-qemu/vvfat \
        --serial-log qemu-results/apfs-load-serial.log
    python tools/qemu-boot-sonoma.py            # interactive window

Exit codes follow this project's convention: 2 for "you asked wrongly"
(argparse does this for unknown flags), 1 for "the thing you asked about is not
there", 0 only when the requested action completed.
"""

import argparse
import json
import os
import subprocess
import sys
import time

DEFAULT_QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
DEFAULT_SHARE = r"C:\Program Files\qemu\share"
DEFAULT_IMAGE = r"F:\nv3050-qemu\sonoma-base.qcow2"
DEFAULT_VARS = r"F:\nv3050-qemu\OVMF_VARS.fd"


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        prog="qemu-boot-sonoma.py",
        description="Boot the converted macOS qcow2 under QEMU and measure the result.",
    )
    p.add_argument("--qemu", default=DEFAULT_QEMU,
                   help="qemu-system-x86_64 executable")
    p.add_argument("--share", default=DEFAULT_SHARE,
                   help="QEMU share directory holding the edk2 firmware images")
    p.add_argument("--image", default=DEFAULT_IMAGE,
                   help="qcow2 holding the converted guest")
    p.add_argument("--ovmf-vars", default=DEFAULT_VARS,
                   help="WRITABLE copy of edk2-i386-vars.fd (the shipped one is read-only)")
    p.add_argument("--memory", default="4096", help="guest RAM in MiB")
    p.add_argument("--smp", default="2", help="guest vCPU count")
    p.add_argument("--cpu", default="Penryn,vendor=GenuineIntel,+invtsc,+hypervisor,kvm=on",
                   help="-cpu argument; Penryn is the model Apple's kernel is happiest with")
    p.add_argument("--accel", default="whpx", choices=["whpx", "tcg"],
                   help="whpx is hardware accelerated; tcg is pure emulation and very slow")
    p.add_argument("--osk", default=None,
                   help="AppleSMC OSK read from Apple hardware you own. Omitted by default.")
    p.add_argument("--probe-seconds", type=int, default=None, metavar="N",
                   help="run headless for N seconds, screendump over QMP, then quit")
    p.add_argument("--screenshot", default="qemu-results/boot-probe.png",
                   help="where --probe-seconds writes the framebuffer dump")
    p.add_argument("--serial-log", default="qemu-results/boot-probe-serial.log",
                   help="capture the guest serial port here; edk2 mirrors its console "
                        "to serial, so this is the only TEXT channel available")
    p.add_argument("--vvfat", default=None, metavar="DIR",
                   help="expose DIR to the guest as a read-only FAT volume via QEMU's "
                        "vvfat driver. Nothing is written to the macOS image, so a "
                        "failed experiment costs a rerun rather than a reconversion. "
                        "Put apfs.efi and a startup.nsh here: the measured boot lands "
                        "in the EFI shell, which auto-runs startup.nsh from the first "
                        "mapped filesystem.")
    p.add_argument("--dry-run", action="store_true",
                   help="print the command that would run, touch nothing")
    return p.parse_args(argv)


def missing_inputs(args, code_fd):
    """Return a list of (label, path) for every required input that is absent.

    Reported as a LIST rather than a first-failure abort: on a fresh machine
    several of these are missing at once, and being told about them one power
    cycle at a time is how a five-minute setup becomes an afternoon.
    """
    out = []
    for label, path in (("qemu", args.qemu),
                        ("image", args.image),
                        ("ovmf-code", code_fd),
                        ("ovmf-vars", args.ovmf_vars)):
        if not os.path.exists(path):
            out.append((label, path))
    # --vvfat is optional, so it is only required once it is asked for. Checked
    # with isdir rather than exists: QEMU's vvfat driver given a FILE fails
    # inside the block layer with a message about the image format, which reads
    # as a broken driver rather than a mistyped argument.
    if args.vvfat and not os.path.isdir(args.vvfat):
        out.append(("vvfat-dir", args.vvfat))
    return out


def build_command(args, code_fd):
    smc = "isa-applesmc"
    if args.osk:
        smc += ",osk=" + args.osk
    # `-nodefaults` below removes the default serial device, so nothing is
    # listening unless one is added back explicitly. That matters more than it
    # looks: the framebuffer is the wrong channel for reading TEXT. A 1280-wide
    # console dump is downscaled below the legibility of an 8x16 font by whatever
    # views it, so an EFI shell full of error text and a blank screen decode to
    # the same unreadable smear. edk2 mirrors its console to serial, so this is
    # the only channel that yields characters instead of pixels.
    serial = None
    if args.serial_log:
        serial = os.path.abspath(args.serial_log)
        parent = os.path.dirname(serial)
        if parent:
            os.makedirs(parent, exist_ok=True)
    cmd = [
        args.qemu,
        "-machine", "q35",
        "-accel", args.accel,
        "-cpu", args.cpu,
        "-m", args.memory,
        "-smp", args.smp,
        # OVMF is a PAIR: read-only code plus writable vars. Handing QEMU the
        # shipped vars file directly makes the firmware fail its first NVRAM
        # write, which surfaces as a hang with no message.
        "-drive", "if=pflash,format=raw,readonly=on,file=" + code_fd,
        "-drive", "if=pflash,format=raw,file=" + args.ovmf_vars,
        "-device", smc,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "if=none,id=macdisk,format=qcow2,file=" + args.image,
        "-device", "ide-hd,bus=ahci.0,drive=macdisk",
        "-device", "nec-usb-xhci,id=xhci",
        "-device", "usb-kbd,bus=xhci.0",
        "-device", "usb-tablet,bus=xhci.0",
        "-device", "vmware-svga",
        "-nodefaults",
        "-no-reboot",
    ]
    if serial:
        cmd += ["-serial", "file:" + serial]
    if args.vvfat:
        # `snapshot=on`, NOT `readonly=on`, and NOT `fat:rw:`. All three protect
        # the host directory; only one of them actually attaches.
        #
        # Measured 2026-08-14: `readonly=on` + `ide-hd` dies at device setup with
        #   -device ide-hd,bus=ahci.1,drive=vvfat: Block node is read-only
        # and qemu exits BEFORE firmware runs -- 0-byte serial log, no framebuffer.
        # An ide-hd is a hard disk, and qemu will not build one over a read-only
        # node. `fat:rw:` would satisfy it, but vvfat's write path is documented
        # as able to corrupt the host directory it is serving.
        #
        # `snapshot=on` resolves both: qemu layers a temporary overlay over the
        # vvfat node, so the block layer sees something writable while every
        # write lands in a temp file that is discarded at exit. The host
        # directory is never written. That is the property this needs -- the
        # firmware only has to READ apfs.efi.
        #
        # No `format=` argument. The `fat:` prefix already selects the vvfat
        # driver, and naming a format alongside it is a conflict rather than a
        # clarification.
        #
        # A Windows path survives the prefix parse: vvfat consumes `fat:` and
        # then only the exact tokens `12:`/`16:`/`32:`/`floppy:`/`rw:`, so the
        # colon in `F:` is not eaten and the remainder becomes the directory.
        cmd += [
            "-drive", "if=none,id=vvfat,snapshot=on,file=fat:" + args.vvfat,
            "-device", "ide-hd,bus=ahci.1,drive=vvfat",
        ]
    return cmd


def qmp_probe(cmd, seconds, shot_path):
    """Run QEMU headless, screendump after `seconds`, and report observations.

    QMP rather than the human monitor on purpose: HMP over a pipe echoes
    keystrokes back and swallows the first character, so `info pci` arrives as
    `nfo pci` and the transcript reads like a QEMU failure. QMP is line
    delimited JSON with no readline layer.
    """
    shot_dir = os.path.dirname(os.path.abspath(shot_path))
    if shot_dir:
        os.makedirs(shot_dir, exist_ok=True)
    if os.path.exists(shot_path):
        os.remove(shot_path)

    full = list(cmd) + ["-display", "none", "-qmp", "stdio"]
    print("probe command:")
    print("  " + " ".join('"%s"' % a if " " in a else a for a in full))
    print()

    proc = subprocess.Popen(full, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, bufsize=1)

    def send(obj):
        proc.stdin.write(json.dumps(obj) + "\n")
        proc.stdin.flush()

    banner = proc.stdout.readline()
    print("qmp banner  : " + banner.strip()[:160])
    send({"execute": "qmp_capabilities"})
    print("capabilities: " + proc.stdout.readline().strip()[:160])

    print("running headless for %d s ..." % seconds)
    for _ in range(seconds):
        time.sleep(1)
        if proc.poll() is not None:
            break

    alive = proc.poll() is None
    print("qemu alive after wait: %s" % ("yes" if alive else "NO (exit %s)" % proc.returncode))

    dump_reply = "(not attempted -- qemu had already exited)"
    if alive:
        # Ask for PNG EXPLICITLY. QMP `screendump` defaults to PPM, so the
        # first version of this function wrote a P6 PPM into a file named
        # `.png` -- 3 MB, right dimensions, and unreadable by anything that
        # checks magic bytes instead of the extension. Nothing in the reply said
        # so; the mislabel only surfaced on an actual decode attempt. A wrong
        # extension is a lie the whole toolchain downstream repeats.
        send({"execute": "screendump",
              "arguments": {"filename": os.path.abspath(shot_path),
                            "format": "png"}})
        dump_reply = proc.stdout.readline().strip()[:200]
        if '"error"' in dump_reply:
            # Older QEMU has no `format` argument. Fall back to the default PPM
            # under its TRUE extension rather than writing PPM bytes into a
            # .png, then convert with tools/ppm-to-png.py.
            print("screendump format=png refused: " + dump_reply)
            ppm_path = os.path.splitext(shot_path)[0] + ".ppm"
            send({"execute": "screendump",
                  "arguments": {"filename": os.path.abspath(ppm_path)}})
            dump_reply = proc.stdout.readline().strip()[:200]
            conv = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "ppm-to-png.py")
            if os.path.exists(ppm_path) and os.path.exists(conv):
                rc = subprocess.call([sys.executable, conv, ppm_path, shot_path])
                print("ppm-to-png.py exit: %d" % rc)
        send({"execute": "quit"})
    print("screendump reply: " + dump_reply)

    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        print("qemu did not exit on quit -- killed")

    err = (proc.stderr.read() or "").strip()
    print()
    print("--- qemu stderr (%d bytes) ---" % len(err))
    for line in err.splitlines()[:25]:
        print("  " + line)

    print()
    if os.path.exists(shot_path):
        size = os.path.getsize(shot_path)
        print("SCREENSHOT: %s (%d bytes)" % (shot_path, size))
        print("            A PNG is evidence of a FRAMEBUFFER, not of a booted")
        print("            macOS. Open it and read what is on the screen.")
    else:
        print("SCREENSHOT: absent -- no framebuffer dump was produced.")
    return 0


def main(argv=None):
    args = parse_args(argv)
    code_fd = os.path.join(args.share, "edk2-x86_64-code.fd")

    gone = missing_inputs(args, code_fd)
    if gone:
        print("FAIL: required inputs are missing:")
        for label, path in gone:
            print("    %-10s %s" % (label, path))
        print()
        print("      The qcow2 is produced by:")
        print("        qemu-img convert -p -O qcow2 -c Sonoma.vmdk sonoma-base.qcow2")
        print("      The vars file is a WRITABLE copy of the shipped firmware:")
        print("        cp share/edk2-i386-vars.fd F:/nv3050-qemu/OVMF_VARS.fd")
        return 1

    cmd = build_command(args, code_fd)

    if not args.osk:
        print("NOTE: no --osk given. isa-applesmc will present an empty key and")
        print("      Apple's AppleSMC kext is expected to reject it. A failed")
        print("      boot here measures the missing key, not a broken image.")
        print()

    if args.dry_run:
        print("dry run -- nothing launched. Command would be:")
        print("  " + " ".join('"%s"' % a if " " in a else a for a in cmd))
        return 0

    if args.probe_seconds is not None:
        return qmp_probe(cmd, args.probe_seconds, args.screenshot)

    print("launching interactive QEMU window ...")
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
