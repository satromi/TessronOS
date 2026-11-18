/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ser_svc.c
 *	The serial ports for processes: SVC class 11, sr_ (design 6.2,
 *	include/ts/ser.h).
 *
 *	A process lists the ports (the devices of kind TDK_SERIAL) and opens
 *	one by its name; it is given a number of its own for it, and names
 *	the port only by that number. The kernel holds the device open for
 *	it, and what a process opened is closed when it ends. A read gives
 *	what has come in, waiting up to the time-out the call names for the
 *	first byte (the port's receive time-out is set to it); a write sends
 *	every byte before it returns. Bytes go in and out through a buffer
 *	of the kernel's, with knl_ua_get and knl_ua_put (PAN).
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include <ts/ser.h>

#define SR_OPEN_FIRST	8		/* entries the table of opened ports starts with; it doubles when full */
#define SR_CHUNK	256		/* bytes moved at a time */

typedef struct {
	ID	pid;			/* 0: a free entry */
	ID	dd;
	TMO	rcvtmo;			/* the port's receive time-out as it was set last */
} SROPEN;

/*
 * The ports processes opened, as many as they open: the table holds a
 * pointer to each entry, which is taken once and kept, so that an entry
 * a call found stays where it is when the table grows. A new table is
 * made outside the lock, twice as large, and put in place under it.
 */
LOCAL SROPEN	**sr_open = NULL;
LOCAL INT	sr_cap = 0;
LOCAL T_SPLOCK	sr_lock;

LOCAL BOOL user_ok( CONST void *p, SZ len, BOOL write )
{
	return knl_prc_user_ok(ts_get_pid(), p, len, write);
}

/* A string of the process's, copied in with its terminator; FALSE when it cannot be */
LOCAL BOOL user_str( CONST UB *src, UB *dst, INT max )
{
	INT	i;

	for ( i = 0; i < max; i++ ) {
		if ( !user_ok(src + i, 1, FALSE) ) {		/* a device's name is short: each byte looked at */
			return FALSE;
		}
		dst[i] = UACCESS(src[i]);
		if ( dst[i] == 0 ) {
			return TRUE;
		}
	}
	dst[max - 1] = 0;
	return TRUE;
}

/* The device of a number of the caller's; 0 when it is not */
LOCAL ID dd_of( INT pd, ID pid, SROPEN **p_ent )
{
	ID	dd = 0;
	UINT	imask;

	if ( pd <= 0 || pid <= 0 ) return 0;
	ISpinLock(&sr_lock, &imask);
	if ( pd <= sr_cap && sr_open[pd - 1]->pid == pid ) {
		dd = sr_open[pd - 1]->dd;
		if ( p_ent != NULL ) *p_ent = sr_open[pd - 1];
	}
	ISpinUnlock(&sr_lock, &imask);
	return dd;
}

/*
 * The table of opened ports made twice as large as it was when it was
 * cap entries: E_OK (another call may have made it so first), E_NOMEM
 */
LOCAL ER sr_grow( INT cap )
{
	SROPEN	**t, **was = NULL;
	INT	ncap = ( cap > 0 ) ? cap * 2 : SR_OPEN_FIRST, i, k;
	UINT	imask;

	t = (SROPEN **)Kmalloc(sizeof(SROPEN *) * (SZ)ncap);
	if ( t == NULL ) return E_NOMEM;
	for ( i = cap; i < ncap; i++ ) {
		t[i] = (SROPEN *)Kmalloc(sizeof(SROPEN));
		if ( t[i] == NULL ) {
			for ( k = cap; k < i; k++ ) Kfree(t[k]);
			Kfree(t);
			return E_NOMEM;
		}
		knl_memset(t[i], 0, sizeof(SROPEN));
	}
	ISpinLock(&sr_lock, &imask);
	if ( sr_cap == cap ) {
		for ( i = 0; i < cap; i++ ) t[i] = sr_open[i];
		was = sr_open;
		sr_open = t;
		sr_cap = ncap;
		t = NULL;
	}
	ISpinUnlock(&sr_lock, &imask);
	if ( t != NULL ) {
		/* grown meanwhile by another: what was made here is not needed */
		for ( i = cap; i < ncap; i++ ) Kfree(t[i]);
		Kfree(t);
	}
	if ( was != NULL ) Kfree(was);
	return E_OK;
}

/* sr_lst_prt: the ports, up to max of them into list; their count */
LOCAL INT svc_sr_lst_prt( T_SRPORT *list, INT max )
{
	T_LDEV		ld[8];
	T_SRPORT	one;
	T_SERINFO	info;
	SZ		asz;
	INT		i, n, start = 0, count = 0;
	ID		dd;

	if ( max < 0 ) return E_PAR;
	if ( max > 0 && !user_ok(list, sizeof(*list) * (SZ)max, TRUE) ) return E_MACV;
	for ( ;; ) {
		n = tk_lst_dev(ld, start, 8);
		if ( n <= 0 ) break;
		for ( i = 0; i < n; i++ ) {
			if ( ( ld[i].devatr & TD_DEVKIND ) != TDK_SERIAL ) continue;
			if ( count < max ) {
				knl_memset(&one, 0, sizeof(one));
				knl_strncpy((char *)one.name, (CONST char *)ld[i].devnm, sizeof(one.name) - 1);
				dd = tk_opn_dev(ld[i].devnm, TD_READ);
				if ( dd > 0 ) {
					asz = 0;
					if ( tk_srea_dev(dd, TDN_SER_INFO, &info, sizeof(info), &asz) >= E_OK ) {
						knl_memcpy(one.label, info.label, sizeof(one.label));
						one.label[sizeof(one.label) - 1] = '\0';
						one.console = info.console;
						one.speed = info.speed;
					}
					(void)tk_cls_dev(dd, 0);
				}
				knl_ua_put(&list[count], &one, sizeof(one));
			}
			count++;
		}
		start += n;
	}
	return count;
}

/* sr_opn_prt: a port opened by its name; the caller's number for it */
LOCAL INT svc_sr_opn_prt( CONST UB *name )
{
	UB	nm[L_DEVNM + 1];
	T_RDEV	rd;
	ID	pid = ts_get_pid(), dd;
	INT	i, pd = 0;
	UINT	imask;

	if ( pid <= 0 ) return E_CTX;
	if ( !user_str(name, nm, sizeof(nm)) ) return E_MACV;
	if ( tk_ref_dev(nm, &rd) < E_OK || ( rd.devatr & TD_DEVKIND ) != TDK_SERIAL ) {
		return E_NOEXS;
	}
	dd = tk_opn_dev(nm, TD_UPDATE);
	if ( dd < E_OK ) return dd;
	for ( ;; ) {
		INT	cap;
		ER	er;

		ISpinLock(&sr_lock, &imask);
		for ( i = 0; i < sr_cap; i++ ) {
			if ( sr_open[i]->pid == 0 ) {
				sr_open[i]->pid = pid;
				sr_open[i]->dd = dd;
				sr_open[i]->rcvtmo = -2;	/* not set yet */
				pd = i + 1;
				break;
			}
		}
		cap = sr_cap;
		ISpinUnlock(&sr_lock, &imask);
		if ( pd > 0 ) {
			return pd;
		}
		er = sr_grow(cap);
		if ( er < E_OK ) {
			(void)tk_cls_dev(dd, 0);
			return er;
		}
	}
}

/* sr_cls_prt */
LOCAL ER svc_sr_cls_prt( INT pd )
{
	ID	pid = ts_get_pid(), dd = 0;
	UINT	imask;

	if ( pd <= 0 ) return E_ID;
	ISpinLock(&sr_lock, &imask);
	if ( pd <= sr_cap && sr_open[pd - 1]->pid == pid && pid > 0 ) {
		dd = sr_open[pd - 1]->dd;
		sr_open[pd - 1]->pid = 0;
	}
	ISpinUnlock(&sr_lock, &imask);
	if ( dd <= 0 ) return E_ID;
	return tk_cls_dev(dd, 0);
}

/* sr_rea_prt: what has come in, up to size bytes, waiting up to tmo for the first */
LOCAL INT svc_sr_rea_prt( INT pd, void *buf, INT size, TMO tmo )
{
	UB	kb[SR_CHUNK];
	SROPEN	*e = NULL;
	SZ	asz = 0;
	ID	dd;
	ER	er;

	dd = dd_of(pd, ts_get_pid(), &e);
	if ( dd <= 0 ) return E_ID;
	if ( size < 0 || tmo < TMO_FEVR ) return E_PAR;
	if ( size == 0 ) return 0;
	if ( size > SR_CHUNK ) size = SR_CHUNK;
	if ( !user_ok(buf, size, TRUE) ) return E_MACV;
	if ( e->rcvtmo != tmo ) {
		er = tk_swri_dev(dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz);
		if ( er < E_OK ) return er;
		e->rcvtmo = tmo;
	}
	er = tk_srea_dev(dd, 0, kb, size, &asz);
	if ( er < E_OK ) return er;
	if ( asz > 0 ) knl_ua_put(buf, kb, asz);
	return (INT)asz;
}

/* sr_wri_prt: every byte sent; how many */
LOCAL INT svc_sr_wri_prt( INT pd, CONST void *buf, INT size )
{
	UB	kb[SR_CHUNK];
	SZ	asz;
	INT	done = 0, n;
	ID	dd;
	ER	er;

	dd = dd_of(pd, ts_get_pid(), NULL);
	if ( dd <= 0 ) return E_ID;
	if ( size < 0 ) return E_PAR;
	if ( !user_ok(buf, size, FALSE) ) return E_MACV;
	while ( done < size ) {
		n = ( size - done > SR_CHUNK ) ? SR_CHUNK : size - done;
		knl_ua_get(kb, (CONST UB *)buf + done, n);
		asz = 0;
		er = tk_swri_dev(dd, 0, kb, n, &asz);
		if ( er < E_OK ) return ( done > 0 ) ? done : er;
		done += (INT)asz;
		if ( asz < n ) break;
	}
	return done;
}

/* sr_set_spd: the port's speed, bits per second */
LOCAL ER svc_sr_set_spd( INT pd, UW speed )
{
	SZ	asz = 0;
	ID	dd;
	ER	er;

	dd = dd_of(pd, ts_get_pid(), NULL);
	if ( dd <= 0 ) return E_ID;
	if ( speed == 0 ) return E_PAR;
	er = tk_swri_dev(dd, TDN_SER_SPEED, &speed, sizeof(speed), &asz);
	return ( er < E_OK ) ? er : E_OK;
}

#define F(fn)	((FP)(fn))

EXPORT const FP knl_sr_svc_tbl[TSN_SR_MAX + 1] = {
	[TSN_NUMBER(TSN_SR_LST_PRT)]	= F(svc_sr_lst_prt),
	[TSN_NUMBER(TSN_SR_OPN_PRT)]	= F(svc_sr_opn_prt),
	[TSN_NUMBER(TSN_SR_CLS_PRT)]	= F(svc_sr_cls_prt),
	[TSN_NUMBER(TSN_SR_REA_PRT)]	= F(svc_sr_rea_prt),
	[TSN_NUMBER(TSN_SR_WRI_PRT)]	= F(svc_sr_wri_prt),
	[TSN_NUMBER(TSN_SR_SET_SPD)]	= F(svc_sr_set_spd),
};

/* The ports a process opened, closed as it ends (proc.c): one at a time, each closed outside the lock */
EXPORT void knl_sr_prc_end( ID pid )
{
	ID	dd;
	INT	i;
	UINT	imask;

	if ( pid <= 0 ) return;
	for ( ;; ) {
		dd = 0;
		ISpinLock(&sr_lock, &imask);
		for ( i = 0; i < sr_cap; i++ ) {
			if ( sr_open[i]->pid == pid ) {
				dd = sr_open[i]->dd;
				sr_open[i]->pid = 0;
				break;
			}
		}
		ISpinUnlock(&sr_lock, &imask);
		if ( dd <= 0 ) break;
		(void)tk_cls_dev(dd, 0);
	}
}
