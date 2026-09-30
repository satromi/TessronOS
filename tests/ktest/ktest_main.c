/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_main.c
 *	Runner for the in-kernel tests: called from usermain() when the
 *	kernel is built with KTEST=1 (USE_KTEST).
 *
 *	The tests run in a dedicated task at priority KT_PRI_MAIN so that
 *	helper tasks created at higher priorities (smaller numbers) preempt
 *	it immediately and lower priority helpers (KT_PRI_LOW) do not.
 */

#include "ktest.h"
#include <ts/wm.h>
#include <ts/disp.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/xf.h>
#include <ts/blk.h>

EXPORT KT_STATE	kt;

LOCAL ID	init_tid;

EXPORT void kt_begin( const char *name )
{
	kt.name = name;
	kt.fails = 0;
	kt.asserts = 0;
	kt.cur_skipped = FALSE;
	tm_printf((UB*)"KTEST %s RUN\n", name);	/* identifies a test that hangs */
}

EXPORT void kt_end( void )
{
	if ( kt.cur_skipped ) return;			/* neither passed nor failed */
	kt.total++;
	if ( kt.fails == 0 ) {
		kt.pass++;
		tm_printf((UB*)"KTEST %s PASS (%d asserts)\n", kt.name, kt.asserts);
	} else {
		tm_printf((UB*)"KTEST %s FAIL (%d/%d asserts failed)\n", kt.name, kt.fails, kt.asserts);
	}
}

EXPORT void kt_fail( const char *file, INT line, const char *expr, BINT a, BINT b )
{
	kt.fails++;
	tm_printf((UB*)"  assert failed: %s:%d: %s (%d, %d)\n", file, line, expr, (INT)a, (INT)b);
}

EXPORT void kt_skip( const char *name, const char *reason )
{
	kt.skipped++;
	kt.cur_skipped = TRUE;
	tm_printf((UB*)"KTEST %s SKIP %s\n", name, reason);
}

/*
 * Helper tasks are pinned to the processor of the caller so that the
 * single-processor ordering the tests rely on (a lower priority task runs
 * only while the test task waits) holds on SMP. kt_cre_tsk_any() leaves
 * the task free to run anywhere.
 */
EXPORT ID kt_cre_tsk_any( FP task, PRI pri, void *exinf )
{
	T_CTSK	ctsk;

	ctsk.exinf   = exinf;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = task;
	ctsk.itskpri = pri;
	ctsk.stksz   = 4096;
	ctsk.assprc  = 0;
	return tk_cre_tsk(&ctsk);
}

EXPORT ID kt_cre_tsk( FP task, PRI pri, void *exinf )
{
	T_CTSK	ctsk;

	ctsk.exinf   = exinf;
	ctsk.tskatr  = TA_HLNG | TA_RNG0 | TA_ASSPRC;
	ctsk.task    = task;
	ctsk.itskpri = pri;
	ctsk.stksz   = 4096;
	ctsk.assprc  = 1U << (tk_get_prc() - 1);
	return tk_cre_tsk(&ctsk);
}

/*
 * Which families run. All of them unless one was asked for
 * (make KTGROUP=gui), which is there because a change to the drawing
 * layer should not cost a run of the file system's tests: waiting for
 * what cannot have broken is how a test suite stops being run at all.
 */
#if !defined(KT_ONLY_KERNEL) && !defined(KT_ONLY_FS) \
 && !defined(KT_ONLY_GUI) && !defined(KT_ONLY_NET)
#define KT_KERNEL	1
#define KT_FS		1
#define KT_GUI		1
#define KT_NET		1
#else
#define KT_KERNEL	defined(KT_ONLY_KERNEL)
#define KT_FS		defined(KT_ONLY_FS)
#define KT_GUI		defined(KT_ONLY_GUI)
#define KT_NET		defined(KT_ONLY_NET)
#endif

/*
 * What the kernel lock cost in each suite, and where it was taken from
 * over the whole run (make KLOCKSTAT=1, design 8.6.2). The places are
 * code addresses; tools/klock_report.py names them.
 */
#if CNF_KLOCK_STAT
#include <ts/klstat.h>

LOCAL T_KLSITE	kl_before;

LOCAL UD kl_us( UD ticks )
{
	UD	f;

	Asm("mrs %0, cntfrq_el0" : "=r"(f));
	return ( f > 0 ) ? ticks * 1000000U / f : 0;
}

LOCAL void kl_before_suite( void )
{
	(void)knl_klock_stat(&kl_before, NULL, 0);
}

LOCAL void kl_after_suite( CONST char *name )
{
	T_KLSITE	now;

	(void)knl_klock_stat(&now, NULL, 0);
	tm_printf((UB *)"KLOCK suite %s acq %ld cont %ld wait_us %ld hold_us %ld\n", name,
		  (UD)( now.acq - kl_before.acq ), (UD)( now.cont - kl_before.cont ),
		  kl_us(now.wait - kl_before.wait), kl_us(now.hold - kl_before.hold));
}

LOCAL void kl_report( void )
{
	T_KLSITE	all, *s = (T_KLSITE *)Kmalloc(sizeof(T_KLSITE) * 40);
	INT		n, i;

	n = knl_klock_stat(&all, s, 40);
	tm_printf((UB *)"KLOCK all acq %ld cont %ld wait_us %ld hold_us %ld places %d\n",
		  all.acq, all.cont, kl_us(all.wait), kl_us(all.hold), n);
	for ( i = 0; s != NULL && i < n && i < 40; i++ ) {
		tm_printf((UB *)"KLOCK site %lx acq %ld cont %ld wait_us %ld hold_us %ld\n",
			  (UD)s[i].pc, s[i].acq, s[i].cont, kl_us(s[i].wait), kl_us(s[i].hold));
	}
	if ( s != NULL ) Kfree(s);
}
#define KL_BEFORE()	kl_before_suite()
#define KL_AFTER(n)	kl_after_suite(n)
#else
#define KL_BEFORE()
#define KL_AFTER(n)
#endif

/*
 * One suite instead of all of them: make KTONLY=tv. A change to one
 * layer should cost one suite's run, not the whole book's; a suite that
 * takes ten minutes to answer a question is a suite nobody asks.
 */
#ifdef KT_ONE
LOCAL BOOL kt_wanted( CONST char *name )
{
	CONST char	*want = KT_ONE;			/* one name, or names split by ',' */
	INT		i;

	while ( *want != 0 ) {
		for ( i = 0; want[i] != 0 && want[i] != ',' && want[i] == name[i]; i++ ) ;
		if ( ( want[i] == 0 || want[i] == ',' ) && name[i] == 0 ) {
			return TRUE;
		}
		while ( *want != 0 && *want != ',' ) want++;
		if ( *want == ',' ) want++;
	}
	return FALSE;
}
#define KT_WANTED(n)	kt_wanted(#n)
#else
#define KT_WANTED(n)	TRUE
#endif
#define RUN_SUITE(n)	do { if ( KT_WANTED(n) ) { KL_BEFORE(); ktest_##n(); KL_AFTER(#n); } } while (0)

/*
 * A suite the Raspberry Pi 5 cannot run at all -- its PCI Express is
 * not the flat configuration space the generic walk reads -- is said to
 * be skipped once, with the reason, rather than left out without a
 * word; it is not built there. Elsewhere it runs.
 */
#ifdef RPI5
#define RUN_SUITE_QEMU(n, why) \
	do { if ( KT_WANTED(n) ) kt_skip(#n, "not run (Raspberry Pi 5: " why ")"); } while (0)
#else
#define RUN_SUITE_QEMU(n, why)	RUN_SUITE(n)
#endif

EXPORT BOOL kt_screen( void )
{
	T_DISPSPEC	spec;

	return (BOOL)( ts_disp_ref(&spec) >= E_OK );
}

/* A suite that draws: skipped whole when no screen was found */
#define RUN_SUITE_SCREEN(n) \
	do { if ( KT_WANTED(n) ) { \
		if ( kt_screen() ) { KL_BEFORE(); ktest_##n(); KL_AFTER(#n); } \
		else kt_skip(#n, KT_NO_SCREEN); } } while (0)

/* A suite that formats the scratch partition: skipped whole without one */
#define RUN_SUITE_SCRATCH(n) \
	do { if ( KT_WANTED(n) ) { \
		if ( kt_scratch() != NULL ) { KL_BEFORE(); ktest_##n(); KL_AFTER(#n); } \
		else kt_skip(#n, KT_NO_SCRATCH); } } while (0)

/*
 * The screen as a picture on the test disk. The header is the plain one
 * so that anything can read it; the rows are written whole, because a
 * write per pixel through the file layer would take longer than the
 * test it is meant to show.
 */

/*
 * All of it, however many calls that takes.
 *
 * A short write is not an error and the file system is allowed to make
 * one. A picture written with a call per row and no count checked
 * loses a byte somewhere down the page, and every pixel after that
 * point has its colours rotated -- which looks like a fault in the
 * drawing and is not one. That cost an hour to find once.
 */
LOCAL ER write_all( INT fd, CONST UB *buf, INT len )
{
	INT	done = 0;

	while ( done < len ) {
		INT	n = fs_write(fd, buf + done, (SZ)( len - done ));

		if ( n <= 0 ) {
			return E_IO;
		}
		done += n;
	}

	return E_OK;
}

LOCAL ID	kt_in_key = 0;		/* 入力, opened once to put events in */

EXPORT ER kt_inject( const T_HIDEV *ev )
{
	SZ	asz = 0;
	ER	er;

	if ( kt_in_key <= 0 ) {
		kt_in_key = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE | OB_OP_EXEC);
		if ( kt_in_key <= 0 ) {
			er = kt_in_key;
			kt_in_key = 0;
			return er;
		}
	}
	er = ob_wri_rec(kt_in_key, OB_IN_EVENTS, 0, ev, sizeof(*ev), &asz);
	return ( er >= E_OK && asz != (SZ)sizeof(*ev) ) ? E_IO : er;
}

EXPORT ER kt_sysobj( const char *box, const char *file, TS_UUID *p_uuid )
{
	TS_UUID	b;

	if ( ts_str_to_uuid(box, &b) < E_OK ) {
		return E_SYS;
	}
	return xf_find(&b, (CONST UB *)file, p_uuid);
}

EXPORT UB *kt_obj_data( const TS_UUID *uuid, SZ *p_size )
{
	T_OBREC	rec[4];
	UB	*d = NULL;
	SZ	size = 0, asz;
	D	at = 0;
	INT	cnt = 0, i, r = xf_data_rec(uuid);
	ID	key = ob_opn_obj(uuid, OB_OP_R);

	if ( key <= 0 ) {
		return NULL;
	}
	if ( ob_lst_rec(key, rec, 4, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( rec[i].recno == r ) size = (SZ)rec[i].size;
		}
	}
	if ( size > 0 ) {
		d = (UB *)Kmalloc(size);
	}
	while ( d != NULL && at < (D)size ) {
		asz = 0;
		if ( ob_rea_rec(key, r, at, d + at, size - at, &asz) < E_OK || asz <= 0 ) {
			Kfree(d);
			d = NULL;
			break;
		}
		at += asz;
	}
	ob_cls_obj(key);
	if ( d != NULL ) {
		*p_size = size;
	}
	return d;
}

EXPORT ER kt_shot( const char *path )
{
	T_DISPSPEC	spec;
	UB		*fb, *row;
	UW		*shot;
	INT		fd, x, y;
	UB		head[32];
	INT		n = 0;

	if ( ts_disp_ref(&spec) < E_OK ) {
		return E_NOEXS;
	}
	fb = (UB *)ts_disp_buffer();
	if ( fb == NULL ) {
		return E_NOEXS;
	}
	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		return (ER)fd;
	}
	/* "P6
<w> <h>
255
" written out by hand: there is no printf here */
	head[n++] = 'P';  head[n++] = '6';  head[n++] = 0x0A;
	{
		INT	v[2], k, i, d;

		v[0] = (INT)spec.width;
		v[1] = (INT)spec.height;
		for ( k = 0; k < 2; k++ ) {
			UB	tmp[8];
			INT	m = 0;

			d = v[k];
			do {
				tmp[m++] = (UB)('0' + (d % 10));
				d /= 10;
			} while ( d > 0 );
			for ( i = m - 1; i >= 0; i-- ) {
				head[n++] = tmp[i];
			}
			head[n++] = ( k == 0 ) ? ' ' : 0x0A;
		}
	}
	head[n++] = '2';  head[n++] = '5';  head[n++] = '5';  head[n++] = 0x0A;
	write_all(fd, head, n);

	row = (UB *)Kmalloc((SZ)spec.width * 3);
	if ( row == NULL ) {
		fs_close(fd);
		return E_NOMEM;
	}
	/*
	 * Once the window manager is up, the screen is copied whole while it
	 * builds nothing: read row by row as the file is written, a screen
	 * rebuilt meanwhile would leave rows of the ground in the picture.
	 */
	shot = (UW *)Kmalloc((SZ)spec.width * spec.height * sizeof(UW));
	if ( shot != NULL && wm_copy_screen(shot, (INT)spec.width) < E_OK ) {
		Kfree(shot);
		shot = NULL;
	}
	for ( y = 0; y < (INT)spec.height; y++ ) {
		UW	*p = ( shot != NULL ) ? shot + (SZ)y * spec.width
					      : (UW *)(fb + (UBINT)y * spec.pitch);

		for ( x = 0; x < (INT)spec.width; x++ ) {
			UW	c = p[x];

			row[x * 3]     = (UB)((c >> 16) & 0xFF);
			row[x * 3 + 1] = (UB)((c >> 8) & 0xFF);
			row[x * 3 + 2] = (UB)(c & 0xFF);
		}
		if ( write_all(fd, row, (INT)spec.width * 3) < E_OK ) {
			break;
		}
	}
	Kfree(row);
	if ( shot != NULL ) {
		Kfree(shot);
	}
	fs_close(fd);

	return E_OK;
}

/*
 * The partition the tests may format. On the QEMU machine it is the
 * first partition of the test disk. On the Raspberry Pi 5 it is the
 * partition of the card whose GPT type is the scratch type that
 * tools/mksd.py --scratch-mb gives, 54465331-0000-4000-8000-5446532d5343:
 * the type says it was made for this, which a place on the card or a
 * size would not. The system volume and /boot are never it.
 */
#ifdef RPI5
LOCAL CONST UB	scratch_type[16] = {		/* in GPT mixed-endian form */
	0x31, 0x53, 0x46, 0x54, 0x00, 0x00, 0x00, 0x40,
	0x80, 0x00, 0x54, 0x46, 0x53, 0x2d, 0x53, 0x43
};

LOCAL ER disk_read( void *exinf, UD start, UD nsect, void *buf )
{
	SZ	asz = 0;

	return tk_srea_dev(*(ID *)exinf, (W)start, buf, (SZ)( nsect * BLK_SECTOR_SIZE ), &asz);
}

LOCAL BOOL same_str( CONST char *a, CONST char *b )
{
	while ( *a != '\0' && *a == *b ) {
		a++;
		b++;
	}
	return (BOOL)( *a == *b );
}

EXPORT CONST char *kt_scratch( void )
{
	static char	name[8];
	static BOOL	looked = FALSE, found = FALSE;
	T_PARTTBL	*tbl;
	DiskInfo	di;
	SZ		asz = 0;
	ID		dd;
	INT		i, k, n;

	if ( looked ) {
		return found ? name : NULL;
	}
	looked = TRUE;
	tbl = (T_PARTTBL *)Kmalloc(sizeof(T_PARTTBL));
	dd = tk_opn_dev((UB *)KT_DISK, TD_READ);
	if ( tbl != NULL && dd > 0
	  && tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz) >= E_OK
	  && ( n = knl_read_parttbl(disk_read, &dd, (UD)di.blockcont, tbl) ) > 0 && tbl->gpt ) {
		for ( i = 0; i < n && !found; i++ ) {
			for ( k = 0; k < 16 && tbl->part[i].type_guid[k] == scratch_type[k]; k++ ) ;
			if ( k < 16 || !tbl->part[i].valid || i > 9 ) {
				continue;
			}
			for ( k = 0; KT_DISK[k] != '\0'; k++ ) {
				name[k] = KT_DISK[k];
			}
			name[k++] = (char)( '0' + i );
			name[k] = '\0';
			found = (BOOL)( !same_str(name, KT_BOOTDEV) && !same_str(name, CNF_OB_SYSVOL) );
		}
	}
	if ( dd > 0 ) {
		tk_cls_dev(dd, 0);
	}
	if ( tbl != NULL ) {
		Kfree(tbl);
	}
	if ( found ) {
		tm_printf((UB *)"KTEST scratch partition %s\n", name);
	} else {
		tm_printf((UB *)"KTEST no scratch partition on %s: the tests that format one are skipped\n", KT_DISK);
	}
	return found ? name : NULL;
}
#else
EXPORT CONST char *kt_scratch( void )
{
	return "vblka0";
}
#endif

/*
 * A machine whose clock was never set (a Raspberry Pi 5 without the RTC
 * battery) makes no UUIDs, so no objects either (design 11.4). For the
 * suites after the clock's own, such a clock is set to the time the
 * tests were built, and the log says so.
 */
LOCAL void kt_clock_floor( void )
{
#ifdef KT_BUILD_UNIX
	SYSTIM	tim;
	D	ms;

	if ( tk_get_tim(&tim) < E_OK ) {
		return;
	}
	ms = ((D)tim.hi << 32) | tim.lo;
	if ( ms + 473385600000LL >= 1000000000000LL ) {
		return;				/* set */
	}
	ms = (D)KT_BUILD_UNIX * 1000 - 473385600000LL;
	tim.hi = (W)(ms >> 32);
	tim.lo = (UW)ms;
	if ( tk_set_tim(&tim) >= E_OK ) {
		tm_printf((UB *)"KTEST the clock was not set: set to the build time for the suites that follow\n");
	}
#endif
}

LOCAL void kt_main_task( INT stacd, void *exinf )
{
	tm_printf((UB*)"KTEST START\n");

	/*
	 * No pointer while the tests run. Several of them read single
	 * pixels of the screen and compare them with what the look
	 * table says; an arrow lying across one of those places would
	 * fail a test that is about something else entirely. The suite
	 * that takes pictures turns it back on for its own.
	 */
	wm_show_pointer(FALSE);

#if KT_KERNEL
	RUN_SUITE(task);
	RUN_SUITE(sync);
	RUN_SUITE(time);
	kt_clock_floor();
	RUN_SUITE(mem);
	RUN_SUITE(el0);
	RUN_SUITE(smp);
	RUN_SUITE(proc);
	RUN_SUITE(prcobj);
	RUN_SUITE(cxx);
	RUN_SUITE(v8);
	RUN_SUITE(gpu);
#endif
	kt_clock_floor();		/* when the clock's suite did not run */
#if KT_FS
	/*
	 * The SD card first, before any suite writes to a disk. On the
	 * Raspberry Pi 5 the card is the one the machine started from; if
	 * its driver fails its own checks, nothing more is written to it
	 * and the book stops here.
	 */
	{
#ifdef RPI5
		INT	failed = kt.total - kt.pass - kt.skipped;
#endif

		RUN_SUITE(sd);
#ifdef RPI5
		if ( kt.total - kt.pass - kt.skipped > failed ) {
			tm_printf((UB *)"KTEST the SD card driver failed its checks: "
				  "no further suite runs, so nothing more is written to the card\n");
			goto summary;
		}
#endif
	}
	RUN_SUITE(blk);
	RUN_SUITE(ser);
	RUN_SUITE(fs);
	RUN_SUITE(fat);
	RUN_SUITE(uuid);
	RUN_SUITE(tsfs);
	RUN_SUITE_SCRATCH(tsfsblk);
	RUN_SUITE_SCRATCH(tsfsbtree);
	RUN_SUITE_SCRATCH(tsfsjrnl);
	RUN_SUITE_SCRATCH(tsfsobj);
	RUN_SUITE(ob);
#ifdef KT_ONE
	RUN_SUITE_SCRATCH(tsfscut);	/* without end: only when it is named */
#endif
#endif
#if KT_GUI
	RUN_SUITE(tad);
	RUN_SUITE(om);
	RUN_SUITE_SCREEN(disp);
	RUN_SUITE_SCREEN(dp);
	/* the font comes before the windows: a window's name is drawn with
	   it, so a run that set it up afterwards would test the names
	   against a system that had none */
	RUN_SUITE(fn);
	RUN_SUITE_SCREEN(wm);
	RUN_SUITE_SCREEN(part);
	RUN_SUITE_SCREEN(mn);
	RUN_SUITE_SCREEN(ms);
	RUN_SUITE(unp);
	RUN_SUITE(btbk);
	RUN_SUITE(bkp);
	RUN_SUITE(tray);
	RUN_SUITE(xf);
	RUN_SUITE(xfu);
	RUN_SUITE_SCREEN(xfc);
	RUN_SUITE_SCREEN(drop);
	RUN_SUITE_SCREEN(fault);
	RUN_SUITE_SCREEN(cab);
	RUN_SUITE_SCREEN(tv);
	RUN_SUITE(store);
	/* last: every resource the suites before made is looked for as an object */
	RUN_SUITE(bt2);
#endif
#if KT_NET
	RUN_SUITE_QEMU(pcie, "no flat PCI Express configuration space");
	RUN_SUITE(snd);
	RUN_SUITE(usb);
	RUN_SUITE(devob);
	RUN_SUITE(net);
	RUN_SUITE(so);
	RUN_SUITE(svcio);
#endif

#if defined(RPI5) && KT_FS
summary:
#endif
#if CNF_KLOCK_STAT
	kl_report();
#endif
	tm_printf((UB*)"KTEST SUMMARY %d/%d (skipped %d)\n", kt.pass, kt.total, kt.skipped);
	tk_wup_tsk(init_tid);
	tk_ext_tsk();
}

EXPORT INT ktest_main( void )
{
	ID	id;

	init_tid = tk_get_tid();
	/*
	 * A stack of its own, larger than the helper tasks get. Turning a
	 * CFF outline into pixels needs about 16KB of stack on its own --
	 * the interpreter keeps its working state there -- and a task that
	 * runs out writes past the bottom of its stack into whatever the
	 * allocator handed out below it, silently.
	 */
	{
		T_CTSK	ctsk;

		ctsk.exinf   = NULL;
		ctsk.tskatr  = TA_HLNG | TA_RNG0 | TA_ASSPRC;
		ctsk.task    = (FP)kt_main_task;
		ctsk.itskpri = KT_PRI_MAIN;
		ctsk.stksz   = 32768;
		ctsk.assprc  = 1U << (tk_get_prc() - 1);
		id = tk_cre_tsk(&ctsk);
	}
	if ( id <= 0 ) {
		tm_printf((UB*)"KTEST FAIL cannot create the test task (%d)\n", id);
		return 1;
	}
	tk_sta_tsk(id, 0);
	tk_slp_tsk(TMO_FEVR);		/* woken up by kt_main_task at the end */
	tk_del_tsk(id);
	return ( kt.pass == kt.total ) ? 0 : 1;
}
