#!/usr/bin/env python3
"""Tests of what the desktop shows, quickly, on the x64 PC (QEMU q35 + OVMF).

  tools/x64/ui_run64.py --disk obj/_X64_<id>/sd.img [ui_run.py's options] STEPS...

The same runner as tools/ui_run.py, with the machine of the x64 image
(docs/tessronos/03-platform.md 3.10): the disk image of `make TARGET=_X64_
DESKTOP=1 UITEST=1 BUILD_ID=<id> sdimg` as an NVMe SSD, from which the
firmware starts the loader and the kernel, the console on COM1 and the
steps on COM2 (serb). --elf need not be given: the kernel is on the disk,
and the disk alone says whether a kept state still fits.

UI_EXTRA adds its words, split at the spaces, to QEMU's arguments as
they are (as ktest64.sh's EXTRA): "-acpitable file=X.aml" gives the
machine the battery and the AC adapter of tools/x64/tests/acpi_battery.asl.

UI_ACCEL=whpx runs the machine on Windows' hypervisor (WHPX) rather than
QEMU's own translation, many times as fast. A state is not kept between
runs: QEMU cannot take the NVMe controller's state along.

Run with the Windows Python, which runs the Windows QEMU
(C:\\Program Files\\qemu\\qemu-system-x86_64.exe and its OVMF).
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import ui_run  # noqa: E402

QEMU64 = os.environ.get("QEMU64", r"C:\Program Files\qemu\qemu-system-x86_64.exe")
OVMF = os.environ.get("OVMF", r"C:\Program Files\qemu\share\edk2-x86_64-code.fd")


def machine_args(elf, disk, con_log, ut_port, mon_port, net="socket", wav=None, cdrom=None, vols=False):
    extra = []
    if vols:
        extra += ["-device", "usb-hub,bus=xhci.0,id=hubv"]
    if wav:
        extra = ["-audiodev", "wav,id=snd0,path=%s" % wav, "-device", "usb-audio,bus=xhci.0,audiodev=snd0"]
    if cdrom:
        extra += ["-drive", "file=%s,if=none,id=btcd,format=raw,media=cdrom,readonly=on" % cdrom,
                  "-device", "usb-storage,bus=xhci.0,drive=btcd,removable=on"]
    extra += os.environ.get("UI_EXTRA", "").split()
    accel = []
    cpu = "max"
    if os.environ.get("UI_ACCEL") == "whpx":
        # Windows' hypervisor: the processor's own model, without what OVMF then
        # tries to set up and the hypervisor refuses (IA32_FEATURE_CONTROL)
        accel = ["-accel", "whpx"]
        cpu = "host,vmx=off,lmce=off,sgx=off,sgxlc=off"
    if os.environ.get("UI_USBBOOT") == "1":
        # the disk in a USB card reader, as a PC started from a USB card has it: the
        # system volume is uda1, and "hmp device_del sysdisk" pulls it out. On a root
        # port, since the firmware starts nothing from a disk behind a hub. UI_USBBOOT_OPT
        # adds properties to the disk (",write-cache=off": a disk without a write cache).
        boot = ["-device", "qemu-xhci,id=xhci", "-device", "usb-kbd", "-device", "usb-mouse",
                "-drive", "file=%s,if=none,format=raw,id=usd" % disk,
                "-device", "usb-storage,bus=xhci.0,drive=usd,id=sysdisk,removable=on,bootindex=0"
                + os.environ.get("UI_USBBOOT_OPT", "")]
    else:
        boot = ["-drive", "file=%s,if=none,format=raw,id=nvd" % disk,
                "-device", "nvme,serial=tessronos,drive=nvd",
                "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd", "-device", "usb-mouse"]
    # the CMOS clock keeps the local time, as a PC with Windows keeps it (design 9.5.1)
    return [QEMU64, "-M", "q35"] + accel + ["-cpu", cpu, "-smp", "4", "-m", "2G",
            "-display", "none", "-no-reboot", "-rtc", "base=localtime",
            "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF] + boot + extra + [
            "-netdev", "user,id=n0" if net == "user" else "socket,id=n0,mcast=230.0.0.1:12347",
            "-device", "virtio-net-pci,netdev=n0",
            "-serial", "file:%s" % con_log,
            "-chardev", "socket,id=ut,host=127.0.0.1,port=%d,server=on,wait=off" % ut_port,
            "-serial", "chardev:ut",
            "-monitor", "tcp:127.0.0.1:%d,server,nowait" % mon_port]


def main():
    ui_run.machine_args = machine_args
    # the NVMe controller is not migrated: no state is kept, every run starts the machine
    ui_run.save_state = lambda mon, state: None
    if "--elf" not in sys.argv:
        # the kernel is on the disk: the disk stands for it in the key of a kept state
        try:
            disk = sys.argv[sys.argv.index("--disk") + 1]
        except (ValueError, IndexError):
            sys.stderr.write("ui_run64: --disk obj/_X64_<id>/sd.img is needed\n")
            return 2
        sys.argv[1:1] = ["--elf", disk]
    if "--net" not in sys.argv:
        # the Windows QEMU has no multicast socket network: QEMU's own user network
        sys.argv[1:1] = ["--net", "user"]
    return ui_run.main()


if __name__ == "__main__":
    sys.exit(main())
