/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devinit.c (rpi4)
 *	Device initialization hooks called by the kernel start-up sequence
 *	(Raspberry Pi 4, Raspberry Pi 400, Compute Module 4).
 */

#include <sys/machine.h>

#ifdef RPI4

/* config.h before anything that defines the types (tk/typedef.h): UW is 32 bits */
#include "kernel.h"
#include <sys/sysdef.h>
#include <tm/tmonitor.h>
#include <tk/device.h>
#include <ts/gpio.h>
#include <ts/net.h>
#include <ts/so.h>
#include <ts/usb.h>
#include <ts/disp.h>
#include <ts/dp.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/snd.h>
#include <ts/cam.h>
#include <ts/ob.h>
#include <ts/ser.h>
#include <ts/board.h>
#include <ts/cpuclk.h>
#include "kernel.h"
#include "sysdepend.h"
#include <ts/fs.h>

/*
 * Each step of the start-up is named on the console before it runs, so
 * that a board that stops shows where (CNF_BOOT_TRACE).
 */
#if CNF_BOOT_TRACE
#define STEP(s)		tm_printf((UB *)"TessronOS: > " s "\n")
#else
#define STEP(s)
#endif

IMPORT void knl_objtest_start( void );
IMPORT INT  knl_tpanel_init( void );	/* tpanel.c: a touch screen on the header's pins */

/* Before the kernel starts (no kernel objects available yet) */
EXPORT ER knl_init_device( void )
{
	return E_OK;
}

/* From the initial task: register device drivers here */
EXPORT ER knl_start_device( void )
{
	UW	rev = 0;

	/*
	 * The board as its revision says, whichever device tree config.txt
	 * picked: the Raspberry Pi 4 Model B, the Raspberry Pi 400 or the
	 * Compute Module 4 (include/ts/board.h).
	 */
	if ( knl_fw_get(TAG_GET_BOARD_REVISION, &rev) >= E_OK ) {
		knl_board_set_rev(rev);
	}
	tm_printf((UB *)"TessronOS: board %s (revision %06x, device tree \"%s\")\n",
		  knl_board.name, rev, knl_dtb_model);
	knl_cpu_start();	/* the firmware starts the processors below their rated clock: up to the default limit */

	knl_gpio_init();	/* the SoC's pins, the activity LED (design 7) */
	gpio_led_blink(2, 150);	/* the kernel is running: two blinks, before anything can be seen */
	STEP("serial");
	knl_ser_init();		/* the serial ports (design 10.5) */
	STEP("sd");
	knl_sd_init();		/* EMMC2: the card slot, or the CM4's eMMC (design 10.6) */
	/* the screen the firmware gives (design 16.5.1): three blinks found, six none */
	STEP("display");
	gpio_led_blink(( knl_disp_init() > 0 ) ? 3 : 6, 150);
	STEP("drawing");
	knl_dp_init();		/* and drawing on it (design 16.4) */
	STEP("windows");
	knl_wm_init();		/* and windows (design 16.5.5) */
	STEP("hid");
	knl_hid_init();		/* keyboard and pointer (design 10.4) */
	STEP("sound");
	knl_snd_init();		/* the sound device: USB audio (design 10.15) */
	knl_cam_init();	/* the camera device (design 10.16) */
	STEP("pcie");
	knl_pcie_rc_init();	/* the lane to the VL805, or to the CM4 carrier's slot (design 10.7) */
	STEP("usb");
	knl_usb_init();		/* the USB sockets, behind the PCIe (design 10.14) */
	STEP("touch screen");
	knl_tpanel_init();	/* a GT911 on the header's pins (design 6.4.2) */
	STEP("ethernet");
	knl_genet_init();	/* the RJ45 socket: GENET and its PHY */
	STEP("netdev");
	knl_netdev_init();	/* and the same card as the device "neta" */
	STEP("network");
	so_main();		/* the protocol stack on top of it (design 12.6) */

	/* File management and the mount table (design 12.2.3). The boot
	   partition of the card is the first FAT one, sda0 or sda1. */
	STEP("file system");
	if ( fs_main() >= E_OK ) {
		fs_regist(&knl_fimp_fat);
		fs_regist(&knl_fimp_exfat);
		fs_regist(&knl_fimp_ntfs);
		if ( fs_attach("fatfs", "sda0", "/boot", 0) < EX_OK ) {
			fs_attach("fatfs", "sda1", "/boot", 0);
		}
	}

	STEP("rng");
	knl_rng_init();		/* random source and UUID v7 (design 11.4) */
	STEP("process");
	knl_proc_init();	/* process management (design 9.4) */
	STEP("objects");
	knl_ob_init();		/* real objects of every kind (design 18) */
	(void)wm_cpu_conf_apply();	/* the processors' clock to the limit of the machine's settings */
	STEP("window objects");
	knl_wmobj_init();	/* and windows among them (design 18.13) */
	knl_objtest_start();	/* the checks of the pins, the LED, the clocks, later (objtest.c) */
	STEP("init");
	knl_fs_posix_mounts();	/* "/", /dev and /tmp for the POSIX programs (design 5.8.1) */
	knl_start_init(CNF_INIT_PATH);	/* the first program, when there is one */
	return E_OK;
}

#if USE_SHUTDOWN
/* the Wi-Fi manager's (device/net/wifi/wifi.c); weak: a build without the Wi-Fi has none */
IMPORT void	knl_wifi_shutdown( void ) __attribute__((weak));

EXPORT ER knl_finish_device( void )
{
	if ( knl_wifi_shutdown != NULL ) knl_wifi_shutdown();	/* the network joined left first */
	knl_fs_finish(NULL, NULL);
	return E_OK;
}
#endif /* USE_SHUTDOWN */

#endif /* RPI4 */
