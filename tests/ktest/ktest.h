/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest.h
 *	In-kernel test framework (design 14.3).
 *
 *	Output: one line per result "KTEST <name> PASS|FAIL <detail>" and a
 *	final "KTEST SUMMARY <pass>/<total>". A failed assertion marks the
 *	current test as failed and continues; nothing panics.
 */

#ifndef _KTEST_H_
#define _KTEST_H_

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/uuid.h>

typedef struct {
	const char	*name;		/* current test name */
	INT		fails;		/* assertion failures in the current test */
	INT		asserts;	/* assertions in the current test */
	INT		pass;		/* passed tests */
	INT		total;		/* run tests */
	INT		skipped;
	BOOL		cur_skipped;	/* current test called KT_SKIP */
} KT_STATE;

IMPORT KT_STATE	kt;

IMPORT void kt_begin( const char *name );
IMPORT void kt_end( void );
IMPORT void kt_fail( const char *file, INT line, const char *expr, BINT a, BINT b );
IMPORT void kt_skip( const char *name, const char *reason );

/*
 * The screen written to the test disk as a picture, for a person to
 * look at afterwards. Drawing is judged by reading pixels back in the
 * tests themselves; this is for the cases where what went wrong is
 * only visible to an eye.
 */
IMPORT ER   kt_shot( const char *path );

/*
 * One event put in as if a keyboard or the pointer had reported it,
 * written to the input object 入力 (OB_IN_EVENTS) with a key the tests
 * keep open
 */
#include <ts/hid.h>
IMPORT ER   kt_inject( const T_HIDEV *ev );

/*
 * A file the system brings, as the object its box links to (the 書体箱
 * or the 壁紙箱, by the name the file had), and the bytes of that
 * object's data record read whole into memory to be freed with Kfree.
 */
IMPORT ER   kt_sysobj( const char *box, const char *file, TS_UUID *p_uuid );
IMPORT UB  *kt_obj_data( const TS_UUID *uuid, SZ *p_size );

#define KT_ASSERT(cond) \
	do { kt.asserts++; if ( !(cond) ) kt_fail(__FILE__, __LINE__, #cond, 0, 0); } while (0)

#define KT_ASSERT_EQ(a, b) \
	do { BINT _a = (BINT)(a), _b = (BINT)(b); kt.asserts++; \
	     if ( _a != _b ) kt_fail(__FILE__, __LINE__, #a " == " #b, _a, _b); } while (0)

#define KT_ASSERT_ER(er, expect) \
	do { BINT _a = (BINT)(er), _b = (BINT)(expect); kt.asserts++; \
	     if ( _a != _b ) kt_fail(__FILE__, __LINE__, #er " == " #expect, _a, _b); } while (0)

#define KT_RUN(fn) \
	do { kt_begin(#fn); fn(); kt_end(); } while (0)

#define KT_SKIP(reason) \
	do { kt_skip(kt.name, reason); return; } while (0)

/*
 * The machine the tests run on. The QEMU machine has the drawing layer
 * and a test disk made for the tests (vblka, tools/mkdisk.py). The
 * Raspberry Pi 5 has neither: its disk is the SD card it started from
 * (sda, tools/mksd.py), with /boot and the system volume on it, and
 * the USB disks, network and sound there are the user's.
 *
 *	KT_DISK		the whole disk the machine started from
 *	KT_BOOTDEV	its FAT partition, on /boot
 *	KT_DISK_PARTS	partitions that disk has at the least
 *	kt_scratch()	the partition the tests may format and write
 *			sectors of, or NULL when there is none. On the
 *			Raspberry Pi 5 that is only a partition the card
 *			was made with for the purpose (mksd.py
 *			--scratch-mb): nothing else on it is written to
 *			below the file system.
 */
#ifdef RPI5
#define KT_DISK		"sda"
#define KT_BOOTDEV	"sda0"
#define KT_DISK_PARTS	2
#else
#define KT_DISK		"vblka"
#define KT_BOOTDEV	"vblka1"
#define KT_DISK_PARTS	3
#endif
IMPORT CONST char *kt_scratch( void );

#define KT_NO_SCRATCH	"no scratch partition (Raspberry Pi 5: the card needs one, make KTEST=1 sdimg)"

/*
 * A test that the Raspberry Pi 5 does not run: on it the test is
 * reported skipped with the reason, and the function need not exist
 * there. Everywhere else it runs as KT_RUN does.
 */
#ifdef RPI5
#define KT_RUN_EXCEPT_RPI5(fn, why) \
	do { kt_begin(#fn); kt_skip(#fn, "not run (Raspberry Pi 5: " why ")"); kt_end(); } while (0)
#else
#define KT_RUN_EXCEPT_RPI5(fn, why)	KT_RUN(fn)
#endif

/*
 * A test that needs the screen: the windows, the desktop, or a program
 * driven through its window. Without a screen -- the Raspberry Pi 5
 * found no framebuffer, or the machine was started without one -- it is
 * reported skipped with that reason.
 */
IMPORT BOOL kt_screen( void );

#define KT_NO_SCREEN	"no screen (no framebuffer was found at start)"

#define KT_RUN_SCREEN(fn) \
	do { if ( kt_screen() ) { KT_RUN(fn); } \
	     else { kt_begin(#fn); kt_skip(#fn, KT_NO_SCREEN); kt_end(); } } while (0)

/* progress marker for locating a hang */
#define KT_MARK() \
	tm_printf((UB*)"  @%s:%d\n", __FILE__, __LINE__)

/* Priorities: the test task runs at KT_PRI_MAIN */
#define KT_PRI_MAIN	20
#define KT_PRI_HIGH	5		/* preempts the test task */
#define KT_PRI_LOW	25		/* runs only while the test task waits */

/* Test groups */
IMPORT void ktest_task( void );
IMPORT void ktest_sync( void );
IMPORT void ktest_time( void );
IMPORT void ktest_mem( void );
IMPORT void ktest_el0( void );
IMPORT void ktest_smp( void );
IMPORT void ktest_blk( void );
IMPORT void ktest_sd( void );
IMPORT void ktest_ser( void );
IMPORT void ktest_fs( void );
IMPORT void ktest_fat( void );
IMPORT void ktest_proc( void );
IMPORT void ktest_prcobj( void );
IMPORT void ktest_cxx( void );
IMPORT void ktest_v8( void );
IMPORT void ktest_uuid( void );
IMPORT void ktest_tsfs( void );
IMPORT void ktest_tsfsblk( void );
IMPORT void ktest_tsfscut( void );
IMPORT void ktest_tsfsbtree( void );
IMPORT void ktest_tsfsjrnl( void );
IMPORT void ktest_tsfsobj( void );
IMPORT void ktest_ob( void );
IMPORT void ktest_tad( void );
IMPORT void ktest_om( void );
IMPORT void ktest_pcie( void );
IMPORT void ktest_usb( void );
IMPORT void ktest_snd( void );
IMPORT void ktest_disp( void );
IMPORT void ktest_dp( void );
IMPORT void ktest_wm( void );
IMPORT void ktest_cab( void );
IMPORT void ktest_tv( void );
IMPORT void ktest_store( void );
IMPORT void ktest_part( void );
IMPORT void ktest_mn( void );
IMPORT void ktest_ms( void );
IMPORT void ktest_unp( void );
IMPORT void ktest_btbk( void );
IMPORT void ktest_bkp( void );
IMPORT void ktest_tray( void );
IMPORT void ktest_xf( void );
IMPORT void ktest_xfu( void );
IMPORT void ktest_xfc( void );
IMPORT void ktest_drop( void );
IMPORT void ktest_fault( void );
IMPORT void ktest_bt2( void );
IMPORT void ktest_devob( void );
IMPORT void ktest_fn( void );
IMPORT void ktest_net( void );
IMPORT void ktest_so( void );
IMPORT void ktest_svcio( void );
IMPORT void ktest_gpu( void );

/* Helpers shared by the groups */
IMPORT ID kt_cre_tsk( FP task, PRI pri, void *exinf );	/* TA_HLNG|TA_RNG0, 4KB stack */
IMPORT ID kt_cre_tsk_any( FP task, PRI pri, void *exinf );	/* same, not pinned to a processor */

#endif /* _KTEST_H_ */
