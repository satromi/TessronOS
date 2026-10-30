/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wpaproc.c (x64 PC, Raspberry Pi 5)
 *	The kernel's side of wpa_supplicant's process (design 9.6.11,
 *	WPA_MODE=proc): the device object "wpasup" whose record 1 carries
 *	the messages of wpaproc.h, the starting and watching of the process
 *	"wpasupp", and the way wpaproc_call.c hands a call over to it.
 *
 *	There is one call at a time (call_mtx). The kernel puts its message
 *	in the down slot and signals dn_sem; the process's read of record 1
 *	takes it. The process's write of record 1 puts its message in the
 *	up slot and signals up_sem, which the caller waits on. Each CALL
 *	has its number (WPP_HDR.seq), and the process's messages carry it
 *	back: a message of a call that has given up is thrown away.
 *
 *	Only the process the watcher started reaches record 1 (its pid);
 *	any other, an administrator's included, is refused (E_OACV). The
 *	slots are wiped when their message is taken.
 *
 *	Every wait of a kernel task on the process ends: a call's answer
 *	in WPP_ANSWER_MS, its turn in the same time, the first start of the
 *	process (reading its program from the disk) in WPP_START_MS. A
 *	process that does not answer is ended by the watcher and started
 *	again; while it is away, or being started again, calls fail at once.
 *	The watcher waits 1, 2, 4 ... 60 seconds before each start again,
 *	and goes back to 1 second once a process has run a minute.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "obj/obj.h"
#include "wpa_glue.h"
#include "wpaproc.h"

IMPORT ID	knl_obfile_vol_of( CONST TS_UUID *uuid );

#define WPP_ANSWER_MS	5000		/* a call's answer, its callbacks' included */
#define WPP_START_MS	10000		/* a process started, until its HELLO */
#define WPP_READ_MS	1000		/* the process's read of record 1, at most */
#define WPP_PRI		22		/* the watcher: below the Wi-Fi manager */
#define WPP_PRC_PRI	21		/* the process's main task */

/* What the process is */
#define S_NONE		0		/* the watcher not made */
#define S_STARTING	1		/* started, its HELLO not yet come */
#define S_READY		2
#define S_AWAY		3		/* ended or not started: waiting to start again */

LOCAL volatile INT	wpp_init;	/* 0, 1 being made, 2 made */
LOCAL volatile INT	state = S_NONE;
LOCAL volatile ID	sup_pid;	/* the process the watcher started */
LOCAL volatile BOOL	hung;		/* a call gave up: the watcher ends the process */
LOCAL ID		call_mtx, dn_sem, up_sem;
LOCAL TS_UUID		chan_uuid;

LOCAL UB		dn[WPP_MAX] __attribute__((aligned(8)));
LOCAL volatile INT	dn_len;
LOCAL UB		up[WPP_MAX] __attribute__((aligned(8)));
LOCAL volatile INT	up_len;
LOCAL UW		seq;		/* the number of the call there is */
LOCAL volatile BOOL	ever_ready;	/* a process has said HELLO since the system started */
LOCAL ID		lock_sem;	/* the slots, between the caller and the process's task */

LOCAL void slot_lock( void )   { (void)tk_wai_sem(lock_sem, 1, TMO_FEVR); }
LOCAL void slot_unlock( void ) { (void)tk_sig_sem(lock_sem, 1); }

/* The semaphore emptied: what an earlier call or process left signalled */
LOCAL void drain( ID sem )
{
	while ( tk_wai_sem(sem, 1, TMO_POL) == E_OK ) ;
}

/* ---------------------------------------------------------------- the device object */

LOCAL INT c_text( void *ctx, UB *t, INT n, INT max )
{
	(void)ctx;
	return knl_oj_put(t, n, max, "<p>The way between the Wi-Fi manager and the process wpasupp, which runs "
			  "wpa_supplicant. Record 1: its messages; only that process reads and writes it.</p>");
}

LOCAL INT c_attr( void *ctx, UB *j, INT n, INT max )
{
	(void)ctx;
	n = knl_oj_put(j, n, max, ",\"pid\":");
	return knl_oj_put_num(j, n, max, (D)sup_pid);
}

LOCAL BOOL from_the_process( void )
{
	ID	me = ts_get_pid();

	return (BOOL)( me > 0 && me == sup_pid );
}

/* The process takes the kernel's next message (E_TMOUT: none in WPP_READ_MS) */
LOCAL ER c_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	ER	er;
	INT	n;

	(void)ctx;
	(void)pos;
	(void)off;
	if ( recno != 1 ) return E_PAR;
	if ( !from_the_process() ) return E_OACV;
	if ( size < (SZ)WPP_MAX ) return E_PAR;
	er = tk_wai_sem(dn_sem, 1, WPP_READ_MS);
	if ( er < E_OK ) return er;
	slot_lock();
	n = dn_len;
	if ( n > 0 ) {
		knl_memcpy(buf, dn, (SZ)n);
		knl_memset(dn, 0, (SZ)n);
		dn_len = 0;
	}
	slot_unlock();
	if ( p_asize != NULL ) *p_asize = (SZ)n;
	return ( n > 0 ) ? E_OK : E_TMOUT;
}

/* The process's message: its HELLO, or one for the call there is */
LOCAL ER c_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	WPP_R	r;
	CONST WPP_HDR *h;

	(void)ctx;
	(void)pos;
	(void)off;
	if ( recno != 1 ) return E_PAR;
	if ( !from_the_process() ) return E_OACV;
	h = wpp_r_begin(&r, buf, (int)size, 0);
	if ( h == NULL ) return E_PAR;
	if ( h->kind == WPP_HELLO ) {
		if ( h->ret != WPP_VERSION ) return E_PAR;
		state = S_READY;
		ever_ready = TRUE;
		tm_printf((UB *)"wpa: the supplicant (pid %d) is ready\n", (INT)sup_pid);
	} else if ( h->kind == WPP_RET || h->kind == WPP_CB ) {
		slot_lock();
		if ( h->seq == seq ) {
			knl_memcpy(up, buf, size);
			up_len = (INT)size;
			slot_unlock();
			(void)tk_sig_sem(up_sem, 1);
		} else {
			slot_unlock();		/* of a call that gave up */
		}
	} else {
		return E_PAR;
	}
	if ( p_asize != NULL ) *p_asize = size;
	return E_OK;
}

LOCAL UD c_size( void *ctx, INT recno )
{
	(void)ctx;
	return ( recno == 1 ) ? (UD)WPP_MAX : 0;
}

LOCAL CONST T_OBDVOPS c_ops = { OB_S_NET, "wpasup", 2, 0600, 0, 0, c_text, c_attr, c_rea, c_wri, c_size, NULL };

/* ---------------------------------------------------------------- the watcher */

LOCAL void w_task( INT stacd, void *exinf )
{
	TS_UUID	prog;
	T_CPRC	cprc;
	T_PSTS	ps;
	WPP_ARG	arg;
	SYSTIM	t0, t1;
	ID	vol, pid;
	INT	wait_s = 1, i;
	ER	er;

	(void)stacd;
	(void)exinf;
	(void)ts_str_to_uuid(SYSDEF_PROG_WPASUPP, &prog);
	for ( ;; ) {
		vol = knl_obfile_vol_of(&prog);
		pid = -1;
		if ( vol > 0 ) {
			knl_memset(&arg, 0, sizeof(arg));
			arg.magic = WPP_MAGIC;
			arg.version = WPP_VERSION;
			knl_memcpy(arg.chan, &chan_uuid, sizeof(arg.chan));
			knl_memset(&cprc, 0, sizeof(cprc));
			cprc.pri = WPP_PRC_PRI;
			cprc.prcatr = TS_PRC_SYSTEM;
			cprc.arg = &arg;
			cprc.argsz = sizeof(arg);
			hung = FALSE;
			state = S_STARTING;
			pid = ts_cre_prc_obj(vol, &prog, &cprc);
		}
		if ( pid <= 0 ) {
			state = S_AWAY;
			tm_printf((UB *)"wpa: the supplicant could not be started (%d), again in %d s\n",
				  (INT)pid, wait_s);
		} else {
			sup_pid = pid;
			(void)tk_get_otm(&t0);
			/* its end waited for; one that does not answer, or does not say HELLO, ended */
			for ( ;; ) {
				er = ts_wai_prc(pid, &ps, 500);
				if ( er >= E_OK || er != E_TMOUT ) break;
				(void)tk_get_otm(&t1);
				if ( hung || ( state == S_STARTING
				  && (UD)( t1.lo - t0.lo ) > (UD)( WPP_START_MS + 2000 ) ) ) {
					(void)ts_ter_prc(pid, 0);
				}
			}
			state = S_AWAY;
			sup_pid = 0;
			(void)tk_sig_sem(up_sem, 1);		/* a caller waiting gives up now */
			(void)tk_get_otm(&t1);
			if ( (UD)( t1.lo - t0.lo ) >= 60000 ) wait_s = 1;
			tm_printf((UB *)"wpa: the supplicant (pid %d) ended (%d%s), started again in %d s\n",
				  (INT)pid, ( er >= E_OK ) ? ps.exitcd : (INT)er, hung ? ", it did not answer" : "",
				  wait_s);
		}
		for ( i = 0; i < wait_s * 10; i++ ) (void)tk_dly_tsk(100);
		wait_s = ( wait_s >= 32 ) ? 60 : wait_s * 2;
	}
}

/* The watcher, the object and the locks made, once */
LOCAL BOOL wpp_make( void )
{
	T_CMTX	cm;
	T_CSEM	cs;
	T_CTSK	ct;
	ID	t;
	INT	none = 0;

	if ( wpp_init == 2 ) return TRUE;
	if ( !__atomic_compare_exchange_n(&wpp_init, &none, 1, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ) {
		while ( wpp_init == 1 ) (void)tk_dly_tsk(10);
		return (BOOL)( wpp_init == 2 );
	}
	knl_memset(&cm, 0, sizeof(cm));
	cm.mtxatr = TA_INHERIT;
	call_mtx = tk_cre_mtx(&cm);
	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 1;
	dn_sem = tk_cre_sem(&cs);
	up_sem = tk_cre_sem(&cs);
	cs.isemcnt = 1;
	lock_sem = tk_cre_sem(&cs);
	if ( call_mtx <= 0 || dn_sem <= 0 || up_sem <= 0 || lock_sem <= 0
	  || knl_obdev_add((CONST UB *)"wpasup", NULL, &c_ops, NULL, &chan_uuid) < E_OK ) {
		tm_putstring((UB *)"wpa: no room for the supplicant's way\n");
		wpp_init = 3;
		return FALSE;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)w_task;
	ct.itskpri = WPP_PRI;
	ct.stksz = 8192;
	t = tk_cre_tsk(&ct);
	if ( t <= 0 || tk_sta_tsk(t, 0) < E_OK ) {
		wpp_init = 3;
		return FALSE;
	}
	wpp_init = 2;
	return TRUE;
}

/* ---------------------------------------------------------------- what wpaproc_call.c uses */

EXPORT int wpp_k_begin( void )
{
	INT	i;

	if ( !wpp_make() ) return -1;
	/* the first process being started is waited for; one away, or started again, is not */
	for ( i = 0; !ever_ready && ( state == S_NONE || state == S_STARTING ) && i < WPP_START_MS / 50; i++ ) {
		(void)tk_dly_tsk(50);
	}
	if ( state != S_READY ) return -1;
	if ( tk_loc_mtx(call_mtx, WPP_ANSWER_MS) < E_OK ) return -1;
	if ( state != S_READY ) {
		(void)tk_unl_mtx(call_mtx);
		return -1;
	}
	return 0;
}

EXPORT void wpp_k_end( void )
{
	(void)tk_unl_mtx(call_mtx);
}

/* The message handed down and the process's next one taken up (its length; -1: none in time) */
EXPORT int wpp_k_xchg( unsigned char *buf, int len )
{
	WPP_HDR	*h = (WPP_HDR *)(void *)buf;
	INT	n;
	ER	er;

	if ( len <= 0 || len > WPP_MAX || state != S_READY ) return -1;
	if ( h->kind == WPP_CALL ) h->seq = ++seq;
	else h->seq = seq;
	slot_lock();
	drain(up_sem);
	up_len = 0;
	knl_memcpy(dn, buf, (SZ)len);
	dn_len = len;
	slot_unlock();
	drain(dn_sem);
	(void)tk_sig_sem(dn_sem, 1);
	er = tk_wai_sem(up_sem, 1, WPP_ANSWER_MS);
	slot_lock();
	n = up_len;
	if ( er >= E_OK && n > 0 ) {
		knl_memcpy(buf, up, (SZ)n);
		knl_memset(up, 0, (SZ)n);
	} else {
		n = -1;
		knl_memset(dn, 0, sizeof(dn));
		dn_len = 0;
	}
	up_len = 0;
	slot_unlock();
	if ( n < 0 && state == S_READY ) {
		tm_printf((UB *)"wpa: the supplicant did not answer in %d ms\n", WPP_ANSWER_MS);
		hung = TRUE;
		state = S_AWAY;
	}
	return n;
}
