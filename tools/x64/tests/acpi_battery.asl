/*
 * acpi_battery.asl -- a battery, an AC adapter and a lid for QEMU, which has none
 * (docs/tessronos/03-platform.md 3.10.6): given to QEMU with
 *   iasl -p /tmp/acpi_battery acpi_battery.asl
 *   qemu-system-x86_64 ... -acpitable file=/tmp/acpi_battery.aml
 * The battery's remaining capacity goes down 100 mWh each time _BST is
 * read; every fifth time the adapter is plugged in or out, and the
 * adapter's notification (0x80) is sent, as a machine's firmware does.
 */
DefinitionBlock ("", "SSDT", 2, "TSOS", "BATTEST", 1)
{
    Scope (\_SB)
    {
        Name (BCNT, 0)
        Name (ACON, 1)
        Name (PBST, Package (4) { 0, 0, 0, 0 })

        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 1)
            Method (_STA, 0) { Return (0x1F) }
            Method (_BIF, 0)
            {
                Return (Package (13) { 0, 50000, 48000, 1, 7600, 4800, 480, 1, 1,
                                       "TSBAT1", "0001", "LION", "TESSRON" })
            }
            Method (_BST, 0)
            {
                BCNT++
                If ((BCNT % 5) == 0)
                {
                    ACON ^= 1
                    Notify (\_SB.ADP1, 0x80)
                }
                If (ACON) { PBST [0] = 2 } Else { PBST [0] = 1 }
                PBST [1] = 1500
                PBST [2] = 40000 - (BCNT * 100)
                PBST [3] = 7600
                Return (PBST)
            }
        }

        Device (ADP1)
        {
            Name (_HID, "ACPI0003")
            Method (_STA, 0) { Return (0x0F) }
            Method (_PSR, 0) { Return (ACON) }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0) { Return (1) }
        }
    }
}
