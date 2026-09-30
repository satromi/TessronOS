<#
  TessronOS
  Copyright (C) 2026 satromi
  This software is distributed under the T-License 2.2.

  run_desktop.ps1 -- run the TessronOS desktop in QEMU for Windows

  The kernel is a DESKTOP=1 build and the disk the test disk of a KTEST=1
  build (docs/build-and-run.md 5.2):

    make TARGET=_QEMU_VIRT_ DESKTOP=1 BUILD_ID=desk            -> build_make/tessronosdesk.elf
    make TARGET=_QEMU_VIRT_ KTEST=1 obj/_QEMU_VIRT__ktest/test_disk.img

  The machine is the one the tests use (4 cores, 2GB) with a screen, a USB
  keyboard and tablet, the user mode network (DHCP, reaches the Internet
  through Windows) and, unless -NoSound, USB audio. The console goes to
  this window. What is saved on the desktop is written to the disk; -Fresh
  runs on a copy in the temporary directory instead.

    powershell -ExecutionPolicy Bypass -File tools\run_desktop.ps1 [-Fresh] [-NoNet] [-NoSound]
#>
param(
    [string]$Elf = "$PSScriptRoot\..\build_make\tessronosdesk.elf",
    [string]$Disk = "$PSScriptRoot\..\build_make\obj\_QEMU_VIRT__ktest\test_disk.img",
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-aarch64.exe",
    [string]$Display = "gtk",
    [switch]$Fresh,
    [switch]$NoNet,
    [switch]$NoSound
)

foreach ($f in @($Qemu, $Elf, $Disk)) {
    if (-not (Test-Path $f)) {
        Write-Error "run_desktop: not found: $f"
        exit 1
    }
}
if ($Fresh) {
    $copy = Join-Path $env:TEMP "tessronos_desktop.img"
    Copy-Item $Disk $copy -Force
    $Disk = $copy
}

$qargs = @(
    "-M", "virt,gic-version=2", "-cpu", "cortex-a76", "-smp", "4", "-m", "2G",
    "-no-reboot",
    "-global", "virtio-mmio.force-legacy=false",
    "-drive", "file=$Disk,if=none,format=raw,id=hd0", "-device", "virtio-blk-device,drive=hd0",
    "-device", "qemu-xhci,id=xhci",
    "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
    "-device", "bochs-display", "-display", $Display,
    "-serial", "stdio",
    "-kernel", $Elf
)
if (-not $NoNet) {
    $qargs += @("-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0")
}
if (-not $NoSound) {
    $qargs += @("-audiodev", "dsound,id=snd0", "-device", "usb-audio,bus=xhci.0,audiodev=snd0")
}
& $Qemu @qargs
