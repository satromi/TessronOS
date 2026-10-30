/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	inttarget.c
 *	The processor that takes each interrupt of a device, and the roles
 *	of the processors (design 4.5): those for the system's chores, which
 *	take the devices' interrupts and the system timer, and those kept
 *	for real time, which take no device's interrupt.
 *
 *	An interrupt goes to one processor. SetIntTarget names it for one
 *	interrupt; a device's object names it for all the interrupts of the
 *	device (knl_int_owner, knl_int_dev_target); otherwise the interrupt
 *	goes to the first processor for chores, and the interrupts by
 *	message to the processors for chores in turn. With the default roles
 *	(PRC 1 alone for chores, none for real time) everything goes to PRC
 *	1, as before there were roles.
 *
 *	How an interrupt is sent to a processor is the architecture's
 *	(knl_int_route): the IOAPIC's entry and the message's address on
 *	x64, the GIC distributor's target byte on Arm.
 */

#include "kernel.h"
#include <tm/tmonitor.h>
#include <tk/syslib.h>

EXPORT T_PRCSET	knl_prc_chore;		/* never empty; PRC 1 always, which takes the system timer */
EXPORT T_PRCSET	knl_prc_rt;		/* kept for real time: no device's interrupt */

LOCAL UH	int_fixed[N_INTVEC];	/* PRC named for the interrupt, 0 none */
LOCAL UH	int_at[N_INTVEC];	/* PRC it is sent to now, 0 the one it started on (PRC 1) */
LOCAL BOOL	roles_set = FALSE;
LOCAL T_SPLOCK	it_lock;

/* The devices' interrupts and the processor asked for each device (by its object) */
#define DEV_N		64
LOCAL struct {
	UB	name[L_DEVNM + 1];	/* "" free */
	UH	prc;			/* 0: the default */
} dev_want[DEV_N];
LOCAL UB	int_dev[N_INTVEC][L_DEVNM + 1];	/* the device an interrupt is of, "" none known */

LOCAL void roles_init( void )
{
	if ( !roles_set ) {
		prcset_zero(&knl_prc_chore);
		prcset_add(&knl_prc_chore, 0);
		prcset_zero(&knl_prc_rt);
		roles_set = TRUE;
	}
}

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < L_DEVNM; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
		if ( a[i] == '\0' ) return TRUE;
	}
	return TRUE;
}

LOCAL INT dev_slot( CONST UB *name, BOOL make )
{
	INT	i, k;

	for ( i = 0; i < DEV_N; i++ ) {
		if ( dev_want[i].name[0] != '\0' && same_name(dev_want[i].name, name) ) return i;
	}
	if ( !make ) return -1;
	for ( i = 0; i < DEV_N; i++ ) {
		if ( dev_want[i].name[0] == '\0' ) {
			for ( k = 0; k < L_DEVNM && name[k] != '\0'; k++ ) dev_want[i].name[k] = name[k];
			dev_want[i].name[k] = '\0';
			dev_want[i].prc = 0;
			return i;
		}
	}
	return -1;
}

/*
 * The processor an interrupt goes to when nothing names one: the first
 * for chores; one by message to the processors for chores in turn
 */
LOCAL INT int_auto( UINT intno )
{
	INT	n = knl_num_prc, k = 0, i, pick;

	for ( i = prcset_next(&knl_prc_chore, 0, n); i >= 0; i = prcset_next(&knl_prc_chore, i + 1, n) ) k++;
	if ( k <= 1 || !knl_int_is_msi(intno) ) {
		return 1;
	}
	pick = (INT)( intno % (UINT)k );
	for ( i = prcset_next(&knl_prc_chore, 0, n); i >= 0 && pick > 0; i = prcset_next(&knl_prc_chore, i + 1, n) ) {
		pick--;
	}
	return ( i >= 0 ) ? i + 1 : 1;
}

/* The processor the interrupt is to go to now: named, by its device, or by the roles */
LOCAL INT int_want( UINT intno )
{
	INT	p = int_fixed[intno], d;

	if ( p == 0 && int_dev[intno][0] != '\0' && ( d = dev_slot(int_dev[intno], FALSE) ) >= 0 ) {
		p = dev_want[d].prc;
	}
	if ( p > 0 && p <= knl_num_prc && !prcset_has(&knl_prc_rt, p - 1) ) {
		return p;
	}
	return int_auto(intno);
}

/* Sent where it is to go, if it is not there; under it_lock */
LOCAL ER int_place( UINT intno )
{
	INT	want = int_want(intno), now = ( int_at[intno] != 0 ) ? int_at[intno] : 1;
	ER	er;

	if ( want == now ) {
		return E_OK;
	}
	er = knl_int_route(intno, want);
	if ( er >= E_OK ) {
		int_at[intno] = (UH)want;
	}
	return er;
}

/*
 * The processor that takes an interrupt (TessronOS's own call):
 * prcid 0 lets the system choose again
 */
EXPORT ER SetIntTarget( UINT intno, ID prcid )
{
	UINT	s;
	UH	was;
	ER	er;

	if ( intno >= N_INTVEC || !knl_int_is_dev(intno) ) {
		return E_PAR;
	}
	if ( prcid < 0 || prcid > knl_num_prc ) {
		return E_PAR;
	}
	ISpinLock(&it_lock, &s);
	roles_init();
	if ( prcid > 0 && prcset_has(&knl_prc_rt, prcid - 1) ) {
		ISpinUnlock(&it_lock, &s);
		return E_OBJ;			/* kept for real time */
	}
	was = int_fixed[intno];
	int_fixed[intno] = (UH)prcid;
	er = int_place(intno);
	if ( er < E_OK ) {
		int_fixed[intno] = was;
	}
	ISpinUnlock(&it_lock, &s);
	return er;
}

EXPORT ID GetIntTarget( UINT intno )
{
	if ( intno >= N_INTVEC || !knl_int_is_dev(intno) ) {
		return E_PAR;
	}
	return ( int_at[intno] != 0 ) ? (ID)int_at[intno] : 1;
}

/* The processor a message interrupt is to be sent to as it is made (cpu_msi.h) */
EXPORT INT knl_int_target_of( UINT intno )
{
	INT	p;
	UINT	s;

	if ( intno >= N_INTVEC ) {
		return 1;
	}
	ISpinLock(&it_lock, &s);
	roles_init();
	p = int_want(intno);
	int_at[intno] = (UH)p;
	ISpinUnlock(&it_lock, &s);
	return p;
}

/*
 * A driver says which device an interrupt is of: the processor the
 * device's object asks for is taken at once, and whenever it changes
 */
EXPORT void knl_int_owner( UINT intno, CONST UB *devnm )
{
	UINT	s;
	INT	k;

	if ( intno >= N_INTVEC || devnm == NULL ) {
		return;
	}
	ISpinLock(&it_lock, &s);
	roles_init();
	for ( k = 0; k < L_DEVNM && devnm[k] != '\0'; k++ ) int_dev[intno][k] = devnm[k];
	int_dev[intno][k] = '\0';
	(void)int_place(intno);
	ISpinUnlock(&it_lock, &s);
}

/* The processor a device's interrupts go to, from its object (0: the default) */
EXPORT ER knl_int_dev_target( CONST UB *devnm, ID prcid )
{
	UINT	s, i;
	INT	d;
	ER	er = E_OK, e;

	if ( devnm == NULL || prcid < 0 || prcid > CNF_MAX_PRCID ) {
		return E_PAR;
	}
	ISpinLock(&it_lock, &s);
	roles_init();
	if ( prcid > 0 && prcid <= knl_num_prc && prcset_has(&knl_prc_rt, prcid - 1) ) {
		ISpinUnlock(&it_lock, &s);
		return E_OBJ;
	}
	d = dev_slot(devnm, prcid != 0);
	if ( d >= 0 ) {
		dev_want[d].prc = (UH)prcid;
		for ( i = 0; i < N_INTVEC; i++ ) {
			if ( int_dev[i][0] != '\0' && same_name(int_dev[i], devnm) ) {
				e = int_place(i);
				if ( e < E_OK ) er = e;
			}
		}
	} else if ( prcid != 0 ) {
		er = E_LIMIT;
	}
	ISpinUnlock(&it_lock, &s);
	return er;
}

/*
 * The count a driver keeps of an interrupt, for the device's object to
 * show: the driver increments it in its handler and names it here once
 * (any number of times: the last one named is shown). NULL: none kept.
 */
LOCAL volatile UW	*int_cnt[N_INTVEC];

EXPORT void knl_int_count_at( UINT intno, volatile UW *cnt )
{
	if ( intno < N_INTVEC ) {
		__atomic_store_n(&int_cnt[intno], cnt, __ATOMIC_RELEASE);
	}
}

/* The count of an interrupt its driver keeps: FALSE when it keeps none */
EXPORT BOOL knl_int_count_of( UINT intno, UW *p_n )
{
	volatile UW	*c = ( intno < N_INTVEC ) ? __atomic_load_n(&int_cnt[intno], __ATOMIC_ACQUIRE) : NULL;

	if ( c == NULL ) {
		return FALSE;
	}
	*p_n = *c;
	return TRUE;
}

/*
 * What a device's object shows: the processor asked for (0 none), and
 * the device's interrupts with the processor each goes to now, as many
 * as n holds; answers how many there are
 */
EXPORT INT knl_int_dev_info( CONST UB *devnm, ID *p_want, UINT *intno, ID *prc, INT n )
{
	UINT	s, i;
	INT	d, k = 0;

	ISpinLock(&it_lock, &s);
	d = dev_slot(devnm, FALSE);
	if ( p_want != NULL ) *p_want = ( d >= 0 ) ? dev_want[d].prc : 0;
	for ( i = 0; i < N_INTVEC; i++ ) {
		if ( int_dev[i][0] == '\0' || !same_name(int_dev[i], devnm) ) continue;
		if ( k < n ) {
			intno[k] = i;
			prc[k] = ( int_at[i] != 0 ) ? int_at[i] : 1;
		}
		k++;
	}
	ISpinUnlock(&it_lock, &s);
	return k;
}

/*
 * The devices whose drivers named their interrupts, as many as n holds;
 * answers how many there are when they all fit, and more than n when
 * they did not (each name is told from the others only among those
 * kept): the caller asks again with more room until the answer is not
 * more than it gave
 */
EXPORT INT knl_int_dev_names( UB (*names)[L_DEVNM + 1], INT n )
{
	UINT	s, i;
	INT	k = 0, j;

	ISpinLock(&it_lock, &s);
	for ( i = 0; i < N_INTVEC; i++ ) {
		if ( int_dev[i][0] == '\0' ) continue;
		for ( j = 0; j < k && j < n && !same_name(names[j], int_dev[i]); j++ ) ;
		if ( j < k ) continue;
		if ( k < n ) knl_memcpy(names[k], int_dev[i], L_DEVNM + 1);
		k++;
	}
	ISpinUnlock(&it_lock, &s);
	return k;
}

LOCAL INT prc_count( CONST T_PRCSET *set )
{
	INT	i, k = 0;

	for ( i = prcset_next(set, 0, knl_num_prc); i >= 0; i = prcset_next(set, i + 1, knl_num_prc) ) k++;
	return k;
}

/*
 * The processors' roles (the machine's settings CPU_CHORE and CPU_RT):
 * PRC 1 is always for chores, a processor for chores is not for real
 * time, those beyond the ones online are left out. Every device's
 * interrupt is sent again where it is to go.
 */
EXPORT ER knl_cpu_roles( CONST T_PRCSET *chore, CONST T_PRCSET *rt )
{
	T_PRCSET	c, r;
	UINT		s, i;
	INT		k, n = knl_num_prc;
	ER		er = E_OK, e;
	BOOL		changed;

	prcset_zero(&c);
	prcset_zero(&r);
	prcset_add(&c, 0);
	for ( k = 0; k < n; k++ ) {
		if ( chore != NULL && prcset_has(chore, k) ) prcset_add(&c, k);
	}
	for ( k = 0; k < n; k++ ) {
		if ( rt != NULL && prcset_has(rt, k) && !prcset_has(&c, k) ) prcset_add(&r, k);
	}
	ISpinLock(&it_lock, &s);
	roles_init();
	changed = FALSE;
	for ( k = 0; k < PRCSET_WORDS; k++ ) {
		if ( knl_prc_chore.w[k] != c.w[k] || knl_prc_rt.w[k] != r.w[k] ) changed = TRUE;
	}
	knl_prc_chore = c;
	knl_prc_rt = r;
	for ( i = 0; i < N_INTVEC; i++ ) {
		if ( knl_int_is_dev(i) && knl_int_in_use(i) ) {
			e = int_place(i);
			if ( e < E_OK ) er = e;
		}
	}
	ISpinUnlock(&it_lock, &s);
	if ( changed ) {
		tm_printf((UB *)"cpu: roles: for chores %d, for real time %d (bits of PRC 1 up: %lx, %lx)\n",
			  prc_count(&c), prc_count(&r), (UD)c.w[0], (UD)r.w[0]);
	}
	return er;
}

/*
 * A list of processors as the machine's settings write it, len bytes of
 * it however long it is: PRC numbers split by commas or spaces, n-m for
 * those from n to m ("2,4-6"). Numbers beyond the first nprc are left
 * out; a number too large to be one is taken as beyond them.
 */
EXPORT void knl_prcset_parse( CONST char *s, INT len, INT nprc, T_PRCSET *set )
{
	INT	i = 0, a, b;

	prcset_zero(set);
	if ( nprc > CNF_MAX_PRCID ) nprc = CNF_MAX_PRCID;
	while ( s != NULL && i < len ) {
		while ( i < len && ( s[i] == ' ' || s[i] == ',' || s[i] == '\t' ) ) i++;
		if ( i >= len ) break;
		if ( s[i] < '0' || s[i] > '9' ) {
			while ( i < len && s[i] != ' ' && s[i] != ',' ) i++;
			continue;
		}
		for ( a = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++ ) {
			if ( a <= nprc ) a = a * 10 + ( s[i] - '0' );
		}
		b = a;
		if ( i < len && s[i] == '-' ) {
			for ( i++, b = 0; i < len && s[i] >= '0' && s[i] <= '9'; i++ ) {
				if ( b <= nprc ) b = b * 10 + ( s[i] - '0' );
			}
		}
		if ( b > nprc ) b = nprc;
		for ( ; a <= b; a++ ) {
			if ( a >= 1 ) prcset_add(set, a - 1);
		}
	}
}

LOCAL INT str_len( CONST char *s )
{
	INT	n = 0;

	while ( s != NULL && s[n] != '\0' ) n++;
	return n;
}

EXPORT ER knl_cpu_roles_text( CONST char *chore, CONST char *rt )
{
	T_PRCSET	c, r;

	knl_prcset_parse(chore, str_len(chore), knl_num_prc, &c);
	knl_prcset_parse(rt, str_len(rt), knl_num_prc, &r);
	return knl_cpu_roles(&c, &r);
}

/* The roles as they are: the sets of PRC n+1 at bit n */
EXPORT void knl_cpu_roles_get( T_PRCSET *chore, T_PRCSET *rt )
{
	UINT	s;

	ISpinLock(&it_lock, &s);
	roles_init();
	if ( chore != NULL ) *chore = knl_prc_chore;
	if ( rt != NULL ) *rt = knl_prc_rt;
	ISpinUnlock(&it_lock, &s);
}
