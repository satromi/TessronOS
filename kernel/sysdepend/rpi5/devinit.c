/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	devinit.c (rpi5)
 *	Device initialization hooks called by the kernel start-up sequence.
 */

#include <sys/machine.h>

#ifdef RPI5

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
#include <ts/gpu.h>
#include <ts/board.h>
#include <ts/hevc.h>
#include <ts/cpuclk.h>
#include "kernel.h"
#include "sysdepend.h"

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
IMPORT void knl_sndtest_start( void );
IMPORT void knl_hevctest_start( void );

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
	 * picked: the Raspberry Pi 5 or the Compute Module 5, with or
	 * without its eMMC (include/ts/board.h).
	 */
	if ( knl_fw_get(TAG_GET_BOARD_REVISION, &rev) >= E_OK ) {
		knl_board_set_rev(rev);
	}
	tm_printf((UB *)"TessronOS: board %s (revision %06x, device tree \"%s\")\n",
		  knl_board.name, rev, knl_dtb_model);
	knl_cpu_start();	/* the firmware starts the processors below their rated clock: up to the default limit */

	knl_gpio_init();	/* always-on GPIO, activity LED (design 7) */
	gpio_led_blink(2, 150);	/* the kernel is running: two blinks, before anything can be seen */
	STEP("pcie");
	knl_pcie_rc_init();	/* the x4 link the RP1 hangs on (design 4.7) */
	STEP("rp1 gpio");
	knl_rp1_gpio_init();	/* the 40 pin header, behind the RP1 */
	STEP("fan");
	(void)knl_fan_init();	/* the fan by the processor's temperature (fan.c) */
#if CNF_RP1_CONSOLE
	knl_board.console_rp1 = TRUE;
#endif
	/*
	 * Move the console to the header pins, when built so or when the
	 * command line asks (the Compute Module 5 as the card image sets it
	 * up: its carrier has no debug connector). The debug UART keeps
	 * everything printed up to here, and keeps the rest too when the
	 * RP1 window is not mapped.
	 */
	if ( knl_board.console_rp1 ) {
		tm_printf((UB *)"TessronOS: console to RP1 UART0 (header pins 8 and 10)\n");
		if ( rp1_uart_console(CNF_RP1_CONSOLE_BAUD) < E_OK ) {
			knl_board.console_rp1 = FALSE;
			tm_printf((UB *)"TessronOS: RP1 UART0 not there; the console stays here\n");
		}
	}
	STEP("serial");
	knl_ser_init();		/* the serial ports (design 10.5) */
	STEP("sd");
	knl_sd_init();	/* SD host controller (design 10.6) */
	/* the screen the firmware gives (design 16.5.1): three blinks found, six none */
	STEP("display");
	gpio_led_blink(( knl_disp_init() > 0 ) ? 3 : 6, 150);
	STEP("drawing");
	knl_dp_init();	/* and drawing on it (design 16.4) */
	STEP("windows");
	knl_wm_init();	/* and windows (design 16.5.5) */
	STEP("hid");
	knl_hid_init();	/* keyboard and pointer (design 10.4) */
	STEP("sound");
	knl_snd_init();	/* the sound device (design 10.15) */
	knl_cam_init();	/* the camera device (design 10.16) */
	STEP("usb");
	knl_usb_init();	/* the USB sockets, behind the RP1 (design 10.14) */
	STEP("ethernet");
	knl_eth_rp1_init();	/* the RJ45 socket, behind the RP1 (design 10.4) */
	STEP("netdev");
	knl_netdev_init();	/* and the same card as the device "neta" */
	STEP("network");
	so_main();		/* the protocol stack on top of it (design 12.6) */

	/* File management and the mount table (design 12.2.3). The boot
	   partition of the card is the first FAT one, sda0 or sda1. */
	STEP("file system");
	if ( fs_main() >= E_OK ) {
		fs_regist(&knl_fimp_fat);
		if ( fs_attach("fatfs", "sda0", "/boot", 0) < EX_OK ) {
			fs_attach("fatfs", "sda1", "/boot", 0);
		}
	}

	STEP("rng");
	knl_rng_init();		/* random source and UUID v7 (design 11.4) */
	STEP("gpu");
	knl_v3d_init();		/* the V3D, only when built with V3D=1, 2 or 3 (13-gpu.md) */
	STEP("process");
	knl_proc_init();	/* process management (design 9.4) */
	STEP("objects");
	knl_ob_init();		/* real objects of every kind (design 18) */
	STEP("hevc");
	(void)knl_hevc_init();	/* the HEVC decoder as the device object (design 6.7) */
	(void)wm_cpu_conf_apply();	/* the processors' clock to the limit of the machine's settings */
	STEP("window objects");
	knl_wmobj_init();	/* and windows among them (design 18.13) */
	knl_objtest_start();	/* the checks of the pins, the LED, the clocks, later (objtest.c) */
	knl_sndtest_start();	/* the checks of the sound's outputs, later, with SNDTEST=1 (sndtest.c) */
	knl_hevctest_start();	/* the check of the HEVC decoder, later, with HEVCTEST=1 (hevctest.c) */
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
	knl_sd_finish();	/* a card at 1.8V powered off, so that the firmware finds it at 3.3V */
	return E_OK;
}
#endif /* USE_SHUTDOWN */

#endif /* RPI5 */
