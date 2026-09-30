/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2026/07.
 *
 *----------------------------------------------------------------------
 */

/*
 *	config.h
 *	User Configuration Definition
 */

#ifndef __TK_CONFIG__
#define __TK_CONFIG__
/*---------------------------------------------------------------------- */
/*  Target Name
	Define the system target name. Alternatively, define the target name 
	in the development environment.

	TessronOS targets (normally passed by the makefile as -D$(TARGET))
	  #define _RPI5_		Raspberry Pi 5 (BCM2712)
	  #define _QEMU_VIRT_		QEMU virt (gic-version=2, cortex-a76)
 */

/*---------------------------------------------------------------------- */
/* SYSCONF : micro T-Kernel system configuration
 */

#define	CNF_SYSTEMAREA_TOP	0	/* 0: Use system default address */
#define CNF_SYSTEMAREA_END	0	/* 0: Use system default address */

#define	CNF_MAX_TSKPRI		32	/* Task Max priority */

#define CNF_TIMER_PERIOD	1	/* Granularity of the ms API (ms); the timer itself is tickless (design 7.4) */

/* Maximum number of kernel objects */
#define CNF_MAX_TSKID		32768	/* Task: the kernel's own and every task of every process */
#define CNF_MAX_SEMID		98304	/* Semaphore (each socket takes two or four, each channel object two, each process's message queue two) */
#define CNF_MAX_FLGID		40960	/* Event flag (each process one for its end) */
#define CNF_MAX_MBXID		12	/* Mailbox*/
#define CNF_MAX_MTXID		36864	/* Mutex (the file system, each volume, each device, the process table and each manager of objects take one) */
#define CNF_MAX_MBFID		4096	/* Message buffer */
#define CNF_MAX_MPLID		4	/* Memory pool */
#define CNF_MAX_MPFID		8	/* Fixed size memory pool */
#define CNF_MAX_CYCID		4	/* Cyclic handler */
#define CNF_MAX_ALMID		8	/* Alarm handler */

/* Device configuration */
#define CNF_MAX_REGDEV		(16)	/* Max registered device */
#define CNF_MAX_OPNDEV		(32)	/* Max open device (each mounted volume holds its partition open: /boot, the system volume, stores, up to 8 USB disks under /media; sound channels, serial ports, the network) */
#define CNF_MAX_REQDEV		(16)	/* Max request device */
#define CNF_DEVT_MBFSZ0		(-1)	/* message buffer size for event notification */
#define CNF_DEVT_MBFSZ1		(-1)	/* message max size for event notification */

/* Version Number */
#define CNF_VER_MAKER		0
#define CNF_VER_PRID		0
#define CNF_VER_PRVER		3
#define CNF_VER_PRNO1		0
#define CNF_VER_PRNO2		0
#define CNF_VER_PRNO3		0
#define CNF_VER_PRNO4		0


/*---------------------------------------------------------------------- */
/* Backwards compatible api support 
 *      micro T-Kernel2.0 API support (Rendezvous)
 */
#define USE_LEGACY_API		(0)	/* 1: Valid  0: Invalid */
#define CNF_MAX_PORID		(0)	/* Maximum number of Rendezvous */


/*---------------------------------------------------------------------- */
/* Stack size definition
 */
#define CNF_EXC_STACK_SIZE	(8192)	/* Exception stack size */
#define	CNF_TMP_STACK_SIZE	(4096)	/* Temporary stack size: dispatcher/idle stack of each processor (TessronOS) */


/*---------------------------------------------------------------------- */
/* System function selection
 *  1: Use function.  0: No use function.
 */
#define USE_NOINIT		(0)	/* Use zero-clear bss section */
#define USE_IMALLOC		(1)	/* Use dynamic memory allocation */
#define USE_SHUTDOWN		(1)	/* Use System shutdown */
#define USE_STATIC_IVT		(0)	/* Use static interrupt vector table */
#define USE_MMU			(1)	/* TessronOS: MMU on, kernel in the linear map (design 6.2) */
#define CNF_IMALLOC_SIZE	(64*1024*1024)	/* TessronOS: Imalloc area reserved from the page allocator (bytes) */
#define CNF_VMAP_SIZE		(1024UL*1024*1024)	/* TessronOS: kernel variable area (vmap) size (bytes) */
#define CNF_MAX_SMB		(256)	/* TessronOS: max number of tk_get_smb blocks */
#define CNF_USER_STKSZ		(64*1024)	/* TessronOS: user stack of an EL0 task (bytes) */
#define CNF_SVC_STKSZ		(64*1024)	/* TessronOS: least system stack of an EL0 task, what its system calls run on (bytes) */
#define CNF_TIMER_SLACK_NS	(50000)	/* TessronOS: relative timeouts are rounded up to this grid (ns, 0: none) */
#define USE_TIME_US_API		(1)	/* TessronOS: microsecond API (tk_dly_tsk_u, tk_get_tim_u, ...) */
#define USE_RTC_WRITEBACK	(1)	/* TessronOS: tk_set_tim/tk_set_utc write the RTC */
#define CNF_OB_STORE		"/boot"	/* TessronOS: the volume of real objects attached at start (design 18.11) */
/* TessronOS: the native system volume, attached instead when the machine has one (design 11.15) */
#ifdef _QEMU_VIRT_
#define CNF_OB_SYSVOL		"vblka2"
#else
#define CNF_OB_SYSVOL		"sda1"
#endif
#define CNF_OB_MAX_GROUPS	64	/* TessronOS: groups of one user (design 18.10) */
#define CNF_MAX_PRC		32000	/* TessronOS: processes at a time; memory is what bounds them first */
#define CNF_PID_MAX		32768	/* TessronOS: process numbers are 1 to CNF_PID_MAX - 1, handed out in turn */
#define CNF_KNL_TSK_ROOM	512	/* TessronOS: tasks, and objects of each kind, kept for the kernel whatever the processes take */
#define CNF_INIT_PATH		"/boot/INIT.ELF"	/* TessronOS: the init program (design 9.14) */
#define CNF_INIT_PRI		(16)	/* TessronOS: priority of the init process (1..CNF_MAX_TSKPRI) */
#define USE_SMP			(1)	/* TessronOS: start the secondary processors (design 8) */
#define CNF_MAX_PRCID		(4)	/* TessronOS: processors supported (per-CPU data) */
#ifndef CNF_RP1_CONSOLE
#define CNF_RP1_CONSOLE		(0)	/* TessronOS (rpi5): 1 moves the console from the 3 pin debug UART to UART0 of the RP1, on header pins 8 and 10 */
#endif
#ifndef CNF_BOOT_TRACE
#define CNF_BOOT_TRACE		(1)	/* TessronOS (rpi5): 1 names each step of the device start-up on the console, so a board that stops shows where (design 14.3.1) */
#endif
#ifndef CNF_RP1_UART0
#define CNF_RP1_UART0		(CNF_RP1_CONSOLE)	/* TessronOS (rpi5): 1 sets up UART0 of the RP1 (header pins 8 and 10) as a serial port; only with enable_rp1_uart=1 in config.txt, without which the firmware leaves the port without a clock and any access to it ends in an SError (make RP1_UART=1) */
#endif
#define CNF_RP1_CONSOLE_BAUD	(115200)	/* TessronOS (rpi5): speed of that console */
#ifndef CNF_DISP_SWAP_RB
#define CNF_DISP_SWAP_RB	(0)	/* TessronOS (rpi5): 1 reads the firmware's pixel order the other way round, for a screen that shows red and blue swapped (make DISP_SWAP_RB=1, design 16.5.1) */
#endif
#ifndef CNF_DISP_MAX_W
#define CNF_DISP_MAX_W		(1920)	/* TessronOS (rpi5): the widest screen asked of the firmware; a larger display is scaled (design 16.5.1) */
#define CNF_DISP_MAX_H		(1200)
#endif
#define CNF_SND_I2S		(1)	/* TessronOS (rpi5): 1 plays the sound device through I2S0 on GPIO 18-21 (a DAC board) when no USB audio is plugged in */
#ifndef CNF_NET_DHCP
#define CNF_NET_DHCP		(0)	/* TessronOS: 1 asks for an address as soon as the stack is up (the Raspberry Pi 5 build: 1) */
#endif
#ifndef CNF_NET_SNTP
#define CNF_NET_SNTP		(0)	/* TessronOS: 1 sets the system clock from a time server once there is an address (the Raspberry Pi 5 build: 1, design 12.6) */
#endif
#ifndef CNF_NTP_SERVER
#define CNF_NTP_SERVER		"pool.ntp.org"	/* TessronOS: the time server SNTP asks, by name */
#endif
#ifndef CNF_TIMEZONE
#define CNF_TIMEZONE		(540)	/* TessronOS: the zone, minutes east of UTC, until the calendar says (JST; design 12.3) */
#endif


/*---------------------------------------------------------------------- */
/* Check API parameter
 *   1: Check parameter  0: Do not check parameter
 */
#define CHK_NOSPT		(1)	/* Check unsupported function (E_NOSPT) */
#define CHK_RSATR		(1)	/* Check reservation attribute error (E_RSATR) */
#define CHK_PAR			(1)	/* Check parameter (E_PAR) */
#define CHK_ID			(1)	/* Check object ID range (E_ID) */
#define CHK_OACV		(1)	/* Check Object Access Violation (E_OACV) */
#define CHK_CTX			(1)	/* Check whether task-independent part is running (E_CTX) */
#define CHK_CTX1		(1)	/* Check dispatch disable part */
#define CHK_CTX2		(1)	/* Check task independent part */
#define CHK_SELF		(1)	/* Check if its own task is specified (E_OBJ) */

#define	CHK_TKERNEL_CONST	(1)	/* Check const-type parameter */

/*---------------------------------------------------------------------- */
/* User initialization program (UserInit)
 *
 */
#define	USE_USERINIT		(0)	/*  1: Use UserInit  0: Do not use UserInit */
#define RI_USERINIT		(0)	/* UserInit start address */

/* ------------------------------------------------------------------------ */
/*
 * Static allocation of system memory
 *     Enabling this setting statically allocates system memory space as variables.
 */
#define USE_STATIC_SYS_MEM	(0)		/* 1:Valid   0:invalid */
#define SYSTEM_MEM_SIZE		(15*1024)	/* Memory size to statically allocate. */

/* ------------------------------------------------------------------------ */
/*
 *  System memory area information (For debugging)
 */
#define USE_DEBUG_SYSMEMINFO   (1)		/* 1:Valid   0:invalid */

/*---------------------------------------------------------------------- */
/* Debugger support function
 *   1: Valid  0: Invalid
 */
#define USE_DBGSPT		(1)	/* Use mT-Kernel/DS */
#define USE_OBJECT_NAME		(0)	/* Use DS object name */

#define OBJECT_NAME_LENGTH	(8)	/* DS Object name length */

/*---------------------------------------------------------------------- */
/* Use T-Monitor Compatible API Library  & Message to terminal.
 *  1: Valid  0: Invalid
 */
#define	USE_TMONITOR		(1)	/* T-Monitor API */
#define USE_SYSTEM_MESSAGE	(1)	/* System Message */
#define USE_EXCEPTION_DBG_MSG	(1)	/* Excepttion debug message */
#define USE_TASK_DBG_MSG	(0)	/* Tsak debug message */

/*---------------------------------------------------------------------- */
/* Use Cache.
 *  1: Valid  0: Invalid
 */
#define	USE_CACHE		(1)	/*　Use Cache */

/*---------------------------------------------------------------------- */
/* Use Co-Processor.
 *  1: Valid  0: Invalid
 */
#define	USE_FPU			(1)	/* Use FPU */
#define	USE_DSP			(0)	/* Use DSP */

#define	ALWAYS_FPU_ATR		(1)	/* Always set the TA_FPU attribute on all tasks */

/*---------------------------------------------------------------------- */
/* Use Physical timer.
 *  1: Valid  0: Invalid
 */
#define USE_PTMR		(0)	/* Use Physical timer */

/*---------------------------------------------------------------------- */
/* Use Sample device driver.
 *  1: Valid  0: Invalid
 */
#define USE_SDEV_DRV		(0)	/* Use Sample device driver */

/*---------------------------------------------------------------------- */
/*
 *	Use Standard C include file
 */
#define USE_STDINC_STDDEF	(1)	/* Use <stddef.h> */
#define USE_STDINC_STDINT	(1)	/* Use <stdint.h> */

/*---------------------------------------------------------------------- */
/*
 *	Don't use reset handler (Use knl_start_mtkernel() )
 */
#define DONT_USE_RESETHDR	(1)

/*---------------------------------------------------------------------- */
/*
 *	Add a prefix to the main function (for BSP)
 */
#define ADD_PREFIX_MAIN_FUNC	(1)

/* ------------------------------------------------------------------------ */
/*
 *  Stack pointer monitoring function
 */
#define USE_SPMON		(1)	// 1:Valid   0:invalid

/*---------------------------------------------------------------------- */
/*
 *	Extensions definition
 */
#if EXT_SEC
#include <config/config_sec.h>
#endif

/*---------------------------------------------------------------------- */
/*
 *	Defining the functions to use
 */
#include "config_func.h"


#endif /* __TK_CONFIG__ */
