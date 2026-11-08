/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devinit.c (x64 PC)
 *	Device initialization hooks called by the kernel start-up sequence.
 */

#include <sys/machine.h>

#ifdef X64

#include "kernel.h"			/* first: the types as config.h sets them */
#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include <tk/device.h>
#include <ts/net.h>
#include <ts/pcie.h>
#include <ts/usb.h>
#include <ts/disp.h>
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/snd.h>
#include <ts/cam.h>
#include <ts/ser.h>
#include <ts/so.h>
#include <ts/ob.h>
#include <ts/gpu.h>
#include "sysdepend.h"
#include "../acpi/acpi.h"
#include "vmd.h"

IMPORT INT	knl_nvme_init( void );		/* device/nvme/nvme.c */
IMPORT void	knl_nvme_report( void );	/* device/nvme/nvme.c */
IMPORT void	knl_nvme_shutdown( void );	/* device/nvme/nvme.c */
IMPORT ER	knl_ps2_init( void );		/* device/ps2/ps2.c */
IMPORT INT	knl_ahci_init( void );		/* device/ahci/ahci.c */
IMPORT INT	knl_sd_init( void );		/* device/sd/sdhci.c */
IMPORT INT	knl_e1k_init( void );		/* device/net/x86_64/e1k.c */
IMPORT INT	knl_re_init( void );		/* device/net/x86_64/re.c */
IMPORT INT	knl_rge_init( void );		/* device/net/x86_64/rge.c */
IMPORT INT	knl_igc_init( void );		/* device/net/x86_64/igc.c */
IMPORT void	knl_acpi_power_start( void );	/* kernel/sysdepend/acpi/acpi_power.c */
IMPORT void	knl_pm_init( void );		/* pm.c */
IMPORT INT	knl_dwi2c_init( void );		/* device/i2c/x86_64/dw_i2c.c */
IMPORT INT	knl_hidi2c_init( void );	/* device/i2c/x86_64/hid_i2c.c */
IMPORT INT	knl_cio2_init( void );		/* device/cam/x86_64/cio2.c */
IMPORT BOOL	knl_bootdisk_find( CONST T_BOOTINFO *bi, TMO tmo );	/* bootdisk.c */
IMPORT void	knl_bootlog_start( void );	/* bootlog.c */
IMPORT void	knl_bootlog_final( CONST char *why );	/* bootlog.c */
IMPORT void	knl_bootlog_unmount( void );		/* bootlog.c */
IMPORT void	knl_vtd_init( BOOL keep );	/* vtd.c */
IMPORT unsigned char	knl_bootvol_name[];
IMPORT unsigned char	knl_logvol_name[];

EXPORT unsigned long long	knl_pcie_ecam_pa = 0;

/*
 * The bus as the firmware left it: the configuration space of segment 0
 * from the MCFG table, its functions' windows where the firmware put
 * them (PCIE_KEEP_FW_BARS)
 */
EXPORT INT knl_pcie_init( void )
{
	INT	i;

	for ( i = 0; i < knl_acpi.nmcfg; i++ ) {
		if ( knl_acpi.mcfg[i].seg == 0 ) {
			knl_pcie_ecam_pa = knl_acpi.mcfg[i].base;
			break;
		}
	}
	if ( knl_pcie_ecam_pa == 0 ) {
		tm_printf((UB *)"TessronOS x64: no MCFG: no PCI Express\n");
		return 0;
	}
	return knl_pcie_scan(0, 0);
}

/* Before the kernel starts (no kernel objects available yet) */
EXPORT ER knl_init_device( void )
{
	return E_OK;
}

/*
 * A word of the start-up settings (cmdline.txt on the EFI system
 * partition, which can be edited on any machine): "acpi=0" leaves the
 * ACPI interpreter out, for a machine on which it stops
 */
LOCAL BOOL setting( CONST char *w )
{
	CONST char	*c = knl_bootinfo->cmdline;
	INT		k;

	for ( ; *c != '\0'; c++ ) {
		if ( c != knl_bootinfo->cmdline && c[-1] != ' ' ) continue;
		for ( k = 0; w[k] != '\0' && c[k] == w[k]; k++ ) ;
		if ( w[k] == '\0' && ( c[k] == '\0' || c[k] == ' ' ) ) return TRUE;
	}
	return FALSE;
}

/*
 * From the initial task: register device drivers here. Each step is
 * named in the band at the top of the screen (knl_fbcon_stage), so that
 * where a machine without a serial port stopped can be read off it.
 */
EXPORT ER knl_start_device( void )
{
	INT	card;

	knl_fbcon_stage("PCI Express");
	knl_pcie_init();	/* the PCI Express bus (design 10.4) */
	knl_vmd_init(setting("vmd=off"));	/* the disks behind an Intel VMD among its functions (06-device 6.2.2) */
#ifdef USE_KTEST
	if ( setting("vmdsim") ) knl_vmd_sim();
	if ( setting("pcifake=amd") ) knl_pcie_fake_amd();
#endif
	knl_fbcon_stage("VT-d");
	knl_vtd_init(setting("vtd=keep"));	/* the IOMMU as the firmware left it, off before any device starts (3.10.9) */
	if ( !setting("acpi=0") ) {
		knl_fbcon_stage("ACPI interpreter");
		(void)knl_acpica_init();	/* the namespace, the SCI, the power button (03-platform 3.10.6) */
	}
	knl_fbcon_stage("serial ports");
	knl_ser_init();		/* the serial ports (design 10.5) */
	knl_fbcon_stage("NVMe");
	knl_nvme_init();	/* the NVMe disks (design 10.4) */
	knl_fbcon_stage("SATA");
	knl_ahci_init();	/* the SATA disks behind AHCI */
	knl_fbcon_stage("SD and eMMC");
	knl_sd_init();		/* the first SD or eMMC slot with a card (06-device 6.2.3) */
	knl_fbcon_stage("network card");
	knl_vnet_init();	/* virtio-net on the bus (design 10.4) */
	knl_e1k_init();		/* Intel's e1000e family (the I219) */
	knl_re_init();		/* Realtek's RTL8111/8168 */
	knl_rge_init();		/* Realtek's RTL8125 */
	knl_igc_init();		/* Intel's I225/I226 */
	card = knl_netdev_init();	/* and the same card as the device "neta" */
	knl_fbcon_stage("screen, drawing, windows");
	knl_disp_init();	/* the screen (design 16.5) */
	knl_fbcon_stage("drawing");
	knl_dp_init();	/* and drawing on it (design 16.4) */
	knl_fbcon_stage("windows");
	knl_wm_init();	/* and windows (design 16.5.5) */
	knl_fbcon_stage("windows: done");
	knl_fbcon_stage("GPU");
	(void)knl_igt_init();	/* Intel's render and copy engines (docs/tessronos/13-gpu.md 13.11) */
	knl_fbcon_stage("keyboard, sound, camera");
	knl_hid_init();	/* keyboard and pointer (design 10.4) */
	knl_snd_init();	/* the sound device (design 10.15) */
	knl_cam_init();	/* the camera device (design 10.16) */
	knl_fbcon_stage("USB");
	knl_usb_init();	/* and the USB devices behind them (design 10.14) */
	knl_fbcon_stage("PS/2 keyboard and pointer");
	(void)knl_ps2_init();	/* the i8042's, beside the USB ones */
	knl_fbcon_stage("I2C: touch and pen");
	if ( knl_dwi2c_init() > 0 ) {
		(void)knl_hidi2c_init();	/* HID over I2C, through the chipset's I2C */
		knl_fbcon_stage("camera: IPU3");
		(void)knl_cio2_init();		/* the notebook's own camera, its sensor on the chipset's I2C */
	}
	if ( card <= 0 ) {
		card = knl_netdev_init();	/* no card on the bus: a USB LAN adapter, when there is one */
	}
	knl_fbcon_stage("network");
	so_main();		/* the protocol stack on top of it (design 12.6) */

	/*
	 * File management and the mount table (design 12.2.3): /boot is the
	 * EFI system partition of the disk started from, and only of it
	 */
	knl_fbcon_stage("the disk started from");
	if ( fs_main() >= E_OK ) {
		fs_regist(&knl_fimp_fat);
		fs_regist(&knl_fimp_exfat);
		fs_regist(&knl_fimp_ntfs);
		if ( knl_bootdisk_find(knl_bootinfo, 15000) ) {
			/* the records' partition TSLOG of the same disk, a copy of the records for Windows */
			if ( knl_logvol_name[0] != '\0'
			  && fs_attach("fatfs", (CONST char *)knl_logvol_name, "/tslog", 0) < E_OK ) {
				knl_logvol_name[0] = '\0';
			}
			if ( fs_attach("fatfs", (CONST char *)knl_bootvol_name, "/boot", 0) >= E_OK ) {
				knl_bootlog_start();	/* the console and the devices to files there */
			}
		}
	}

	knl_rng_init();		/* random source and UUID v7 (design 11.4) */
	knl_fbcon_stage("processes, objects");
	knl_proc_init();	/* process management (design 9.4) */
	knl_ob_init();		/* real objects of every kind (design 18) */
	knl_acpi_power_start();	/* the batteries, the adapter, the lid, the power button as objects */
	knl_pm_init();		/* sleep: which way the firmware offers (03-platform 3.10.11) */
	knl_wmobj_init();	/* and windows among them (design 18.13) */
	knl_fs_posix_mounts();	/* "/", /dev and /tmp for the POSIX programs (design 5.8.1) */
	knl_fbcon_stage("the first program");
	knl_start_init(CNF_INIT_PATH);	/* the first program, when there is one */
	return E_OK;
}

#if USE_SHUTDOWN
/* the Wi-Fi manager's (device/net/wifi/wifi.c); weak: a build without the Wi-Fi has none */
IMPORT void	knl_wifi_shutdown( void ) __attribute__((weak));

/* The power management capability, and in its control and status register (PMCSR, at +4) */
#define PCI_CAP_PM		0x01
#define PMCSR_PME_EN		0x0100
#define PMCSR_PME_STS		0x8000

/*
 * No PCI function may wake the machine from S5: none is asked to, and a
 * PME a function signals (a codec's jack, a USB port's connection, the
 * Wi-Fi card left armed for waking by the firmware or by an earlier
 * system) would turn the machine on again the moment it goes off,
 * through the PCH's PME GPE or the PCIe WAKE# that PM1 enable does not
 * mask. So every function's PME enable is turned off and a PME already
 * signalled cleared; each one found so is said, for the records.
 */
LOCAL void pci_wake_off( void )
{
	T_PCIDEV	d;
	INT		i, n = ts_pcie_count(), armed = 0;
	UINT		pm;
	UW		v;

	for ( i = 0; i < n; i++ ) {
		if ( ts_pcie_get(i, &d) < E_OK || ( pm = ts_pcie_cap_find(&d, PCI_CAP_PM) ) == 0 ) continue;
		v = ts_pcie_cfg_read(d.bus, d.dev, d.fn, pm + 4, 2);
		if ( ( v & ( PMCSR_PME_EN | PMCSR_PME_STS ) ) == 0 ) continue;
		tm_printf((UB *)"pci: %02x:%02x.%x %04x:%04x: PMCSR %04x (%s%s), wake turned off\n",
			  d.bus, d.dev, d.fn, d.vendor, d.device, v,
			  ( v & PMCSR_PME_EN ) ? "PME enabled" : "",
			  ( v & PMCSR_PME_STS ) ? ( ( v & PMCSR_PME_EN ) ? ", PME pending" : "PME pending" ) : "");
		ts_pcie_cfg_write(d.bus, d.dev, d.fn, pm + 4, ( v & ~PMCSR_PME_EN ) | PMCSR_PME_STS, 2);
		armed++;
	}
	tm_printf((UB *)"pci: %d of %d functions had their wake on\n", armed, n);
}

/*
 * Before the machine goes off or starts again (the file systems were
 * written out by whoever turned it off, the desktop before usermain
 * returns): \_TTS(5) first, while every device still works (the
 * firmware is told of the transition before anything stops); the
 * network joined left (its access point told, so that it drops the
 * station at once); no PCI function left able to wake the machine; what
 * the last step will do said, and the NVMe controllers' counts; then the
 * records written last, while the disks still work, so that they hold
 * all of the above, and the volumes they are on unmounted, so that the
 * next start need not look them over. The NVMe controllers are shut down after that, the
 * last writing done: their caches flushed and the shutdown told, so that
 * the power going off is not one they count as unsafe.
 */
EXPORT ER knl_finish_device( void )
{
	(void)knl_acpica_tts(5);
	if ( knl_wifi_shutdown != NULL ) knl_wifi_shutdown();
	pci_wake_off();
	knl_acpica_off_plan();
	knl_nvme_report();
	knl_fs_finish("/boot", "/tslog");	/* the records of this go there, last */
	knl_bootlog_final("goes off or starts again");
	knl_bootlog_unmount();
	knl_fs_finish(NULL, NULL);		/* those two, when there are no records */
	knl_nvme_shutdown();
	return E_OK;
}
#endif /* USE_SHUTDOWN */

#endif /* X64 */
