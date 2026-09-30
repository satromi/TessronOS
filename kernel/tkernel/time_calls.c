/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.08 / TessronOS
 *
 *    Copyright (C) 2006-2026 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	time_calls.c
 *	Time Management Function
 *	TessronOS: nanosecond time base on the tickless timer, microsecond
 *	API, RTC write-back, ts_get_mono (design 7.3, 7.6).
 */

#include "kernel.h"
#include "timer.h"
#include "wait.h"
#include "check.h"
#include "time_calls.h"

/* ------------------------------------------------------------------------ */
/*
 *	Time Management
 */
#if USE_TIMEMANAGEMENT

/*
 * System clock (UTC) in ns: monotonic clock + offset
 */
LOCAL D knl_get_utc_ns( void )
{
	D	t;

	BEGIN_CRITICAL_SECTION;
	t = (D)knl_get_mono_ns() + knl_real_time_ofs;
	END_CRITICAL_SECTION;

	return t;
}

/*
 * Set the system clock (UTC ns). Only the offset changes, so the
 * monotonic clock and pending timers are unaffected. The RTC is written
 * back when USE_RTC_WRITEBACK is set (design 7.3).
 */
LOCAL void knl_set_utc_ns( D utc_ns )
{
	BEGIN_CRITICAL_SECTION;
	knl_real_time_ofs = utc_ns - (D)knl_get_mono_ns();
	END_CRITICAL_SECTION;

#if USE_RTC_WRITEBACK
	if ( utc_ns >= 0 ) {
		knl_rtc_write((UD)utc_ns / NS_PER_SEC);
	}
#endif
}

#ifdef USE_FUNC_TK_SET_UTC
/*
 * Set system clock
 */
SYSCALL ER tk_set_utc( CONST SYSTIM *pk_tim )
{
	CHECK_PAR(pk_tim->hi >= 0);

	knl_set_utc_ns(knl_toLSYSTIM(pk_tim) * (D)NS_PER_MS);

	return E_OK;
}
#endif /* USE_FUNC_TK_SET_UTC */

#ifdef USE_FUNC_TK_GET_UTC
/*
 * Refer system clock
 */
SYSCALL ER tk_get_utc( SYSTIM *pk_tim )
{
	*pk_tim = knl_toSYSTIM(knl_get_utc_ns() / (D)NS_PER_MS);

	return E_OK;
}
#endif /* USE_FUNC_TK_GET_UTC */

#ifdef USE_FUNC_TK_SET_TIM
/*
 * Set system clock (TRON Time)
 */
SYSCALL ER tk_set_tim( CONST SYSTIM *pk_tim )
{
	CHECK_PAR(pk_tim->hi >= 0);

	knl_set_utc_ns((knl_toLSYSTIM(pk_tim) + DIFF_TRON_UTC) * (D)NS_PER_MS);

	return E_OK;
}
#endif /* USE_FUNC_TK_SET_TIM */

#ifdef USE_FUNC_TK_GET_TIM
/*
 * Refer system clock (TRON Time)
 */
SYSCALL ER tk_get_tim( SYSTIM *pk_tim )
{
	*pk_tim = knl_toSYSTIM(knl_get_utc_ns() / (D)NS_PER_MS - DIFF_TRON_UTC);

	return E_OK;
}
#endif /* USE_FUNC_TK_GET_TIM */

#ifdef USE_FUNC_TK_GET_OTM
/*
 * Refer system operating time
 */
SYSCALL ER tk_get_otm( SYSTIM *pk_tim )
{
	*pk_tim = knl_toSYSTIM((LSYSTIM)(knl_get_mono_ns() / NS_PER_MS));

	return E_OK;
}
#endif /* USE_FUNC_TK_GET_OTM */

/*
 * Microsecond versions (T-Kernel 2.0)
 *	ofs receives the nanoseconds below the microsecond (0..999);
 *	it may be NULL.
 */
SYSCALL_U ER tk_set_tim_u( SYSTIM_U tim_u )
{
	CHECK_PAR(tim_u >= 0);

	knl_set_utc_ns(tim_u * (D)NS_PER_US + DIFF_TRON_UTC_NS);

	return E_OK;
}

SYSCALL_U ER tk_get_tim_u( SYSTIM_U *tim_u, UW *ofs )
{
	D	t = knl_get_utc_ns() - DIFF_TRON_UTC_NS;

	*tim_u = t / (D)NS_PER_US;
	if ( ofs != NULL ) {
		*ofs = (UW)(t % (D)NS_PER_US);
	}

	return E_OK;
}

SYSCALL_U ER tk_get_otm_u( SYSTIM_U *tim_u, UW *ofs )
{
	UD	t = knl_get_mono_ns();

	*tim_u = (SYSTIM_U)(t / NS_PER_US);
	if ( ofs != NULL ) {
		*ofs = (UW)(t % NS_PER_US);
	}

	return E_OK;
}

/*
 * Monotonic clock in ns (TessronOS, design 7.3)
 */
SYSCALL ER ts_get_mono( UD *p_ns )
{
	*p_ns = knl_get_mono_ns();

	return E_OK;
}

# ifdef USE_FUNC_TD_GET_UTC
/*
 * Refer system clock (ofs: ns below the millisecond)
 */
SYSCALL ER td_get_utc( SYSTIM *tim, UW *ofs )
{
	D	t = knl_get_utc_ns();

	*tim = knl_toSYSTIM(t / (D)NS_PER_MS);
	*ofs = (UW)(t % (D)NS_PER_MS);

	return E_OK;
}
# endif /* USE_FUNC_TD_GET_UTC */

#if USE_DBGSPT
#ifdef USE_FUNC_TD_GET_TIM
/*
 * Refer system clock (TRON Time; ofs: ns below the millisecond)
 */
SYSCALL ER td_get_tim( SYSTIM *tim, UW *ofs )
{
	D	t = knl_get_utc_ns() - DIFF_TRON_UTC_NS;

	*tim = knl_toSYSTIM(t / (D)NS_PER_MS);
	*ofs = (UW)(t % (D)NS_PER_MS);

	return E_OK;
}
#endif /* USE_FUNC_TD_GET_TIM */

#ifdef USE_FUNC_TD_GET_OTM
/*
 * Refer system operating time (ofs: ns below the millisecond)
 */
SYSCALL ER td_get_otm( SYSTIM *tim, UW *ofs )
{
	UD	t = knl_get_mono_ns();

	*tim = knl_toSYSTIM((LSYSTIM)(t / NS_PER_MS));
	*ofs = (UW)(t % NS_PER_MS);

	return E_OK;
}
#endif /* USE_FUNC_TD_GET_OTM */
#endif /* USE_DBGSPT */
#endif /* USE_TIMEMANAGEMENT */


/* ------------------------------------------------------------------------ */
/*
 *	Cyclic handler
 */

#if USE_CYCLICHANDLER

Noinit(EXPORT CYCCB knl_cyccb_table[NUM_CYCID]);	/* Cyclic handler control block */
Noinit(EXPORT QUEUE	knl_free_cyccb);	/* FreeQue */

#if USE_OBJECT_NAME
#define CYC_DSNAME(p)	((p)->dsname)
#else
#define CYC_DSNAME(p)	(NULL)
#endif

/*
 * Initialization of cyclic handler control block
 */
EXPORT ER knl_cyclichandler_initialize( void )
{
	CYCCB	*cyccb, *end;

	/* Get system information */
	if ( NUM_CYCID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_cyccb);
	end = knl_cyccb_table + NUM_CYCID;
	for ( cyccb = knl_cyccb_table; cyccb < end; cyccb++ ) {
		cyccb->cychdr = NULL; /* Unregistered handler */
		QueInsert((QUEUE*)cyccb, &knl_free_cyccb);
	}

	return E_OK;
}


/*
 * Cyclic handler routine
 */
EXPORT void knl_call_cychdr( CYCCB *cyccb )
{
	/* Set next startup time */
	knl_cyc_timer_insert(cyccb, knl_cyc_next_time(cyccb));

	/* Execute cyclic handler / Enable interrupt nest */
	ENABLE_INTERRUPT_UPTO(TIMER_INTLEVEL);
	CallUserHandlerP1(cyccb->exinf, cyccb->cychdr, cyccb);
	DISABLE_INTERRUPT;
}

/*
 * Immediate call of cyclic handler
 */
LOCAL void knl_immediate_call_cychdr( CYCCB *cyccb )
{
	/* Set next startup time */
	knl_cyc_timer_insert(cyccb, knl_cyc_next_time(cyccb));

	/* Execute cyclic handler in task-independent part
	   (Keep interrupt disabled) */
	ENTER_TASK_INDEPENDENT;
	CallUserHandlerP1(cyccb->exinf, cyccb->cychdr, cyccb);
	LEAVE_TASK_INDEPENDENT;
}

#if CHK_RSATR
LOCAL CONST ATR knl_valid_cycatr = {
	 TA_HLNG
	|TA_STA
	|TA_PHS
#if USE_OBJECT_NAME
	|TA_DSNAME
#endif
};
#endif

/*
 * Create cyclic handler (common part; times in ns)
 */
LOCAL ID knl_cre_cyc_ns( void *exinf, ATR cycatr, FP cychdr, CONST UB *dsname, UD cyctim_ns, UD cycphs_ns )
{
	CYCCB	*cyccb;
	ABSTIM	tm;
	ER	ercd = E_OK;

	CHECK_RSATR(cycatr, knl_valid_cycatr);
	CHECK_PAR(cychdr != NULL);
	CHECK_PAR(cyctim_ns > 0);

	BEGIN_CRITICAL_SECTION;
	/* Get control block from FreeQue */
	cyccb = (CYCCB*)QueRemoveNext(&knl_free_cyccb);
	if ( cyccb == NULL ) {
		ercd = E_LIMIT;
		goto error_exit;
	}

	/* Initialize control block */
	cyccb->exinf   = exinf;
	cyccb->cycatr  = cycatr;
	cyccb->cychdr  = cychdr;
	cyccb->cyctim  = cyctim_ns;
#if USE_OBJECT_NAME
	if ( (cycatr & TA_DSNAME) != 0 ) {
		knl_strncpy((char*)cyccb->name, (char*)dsname, OBJECT_NAME_LENGTH);
	}
#else
	(void)dsname;
#endif

	/* First startup time: the phase after now */
	tm = knl_get_mono_ns() + cycphs_ns;

	if ( (cycatr & TA_STA) != 0 ) {
		/* Start cyclic handler */
		cyccb->cycstat = TCYC_STA;

		if ( cycphs_ns == 0 ) {
			/* Immediate execution */
			cyccb->cyctmeb.time = tm;
			knl_immediate_call_cychdr(cyccb);
		} else {
			/* Register onto timer event queue */
			knl_cyc_timer_insert(cyccb, tm);
		}
	} else {
		/* Initialize only counter */
		cyccb->cycstat = TCYC_STP;
		cyccb->cyctmeb.time = tm;
	}

	ercd = ID_CYC(cyccb - knl_cyccb_table);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

/*
 * Create cyclic handler
 */
SYSCALL ID tk_cre_cyc( CONST T_CCYC *pk_ccyc )
{
	CHECK_RELTIM(pk_ccyc->cyctim);

	return knl_cre_cyc_ns(pk_ccyc->exinf, pk_ccyc->cycatr, pk_ccyc->cychdr, CYC_DSNAME(pk_ccyc),
				knl_ms_to_ns(pk_ccyc->cyctim), knl_ms_to_ns(pk_ccyc->cycphs));
}

SYSCALL_U ID tk_cre_cyc_u( CONST T_CCYC_U *pk_ccyc_u )
{
	CHECK_RELTIM_U(pk_ccyc_u->cyctim_u);

	return knl_cre_cyc_ns(pk_ccyc_u->exinf, pk_ccyc_u->cycatr, pk_ccyc_u->cychdr, CYC_DSNAME(pk_ccyc_u),
				knl_us_to_ns(pk_ccyc_u->cyctim_u), knl_us_to_ns(pk_ccyc_u->cycphs_u));
}

#ifdef USE_FUNC_TK_DEL_CYC
/*
 * Delete cyclic handler
 */
SYSCALL ER tk_del_cyc( ID cycid )
{
	CYCCB	*cyccb;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		if ( (cyccb->cycstat & TCYC_STA) != 0 ) {
			/* Delete timer event queue */
			knl_timer_delete(&cyccb->cyctmeb);
		}

		/* Return to FreeQue */
		QueInsert((QUEUE*)cyccb, &knl_free_cyccb);
		cyccb->cychdr = NULL; /* Unregistered handler */
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_CYC */

#ifdef USE_FUNC_TK_STA_CYC
/*
 * Start cyclic handler
 */
SYSCALL ER tk_sta_cyc( ID cycid )
{
	CYCCB	*cyccb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
		goto error_exit;
	}

	cur = knl_get_mono_ns();

	if ( (cyccb->cycatr & TA_PHS) != 0 ) {
		/* Continue cyclic phase */
		if ( (cyccb->cycstat & TCYC_STA) == 0 ) {
			/* Start cyclic handler */
			tm = cyccb->cyctmeb.time;
			if ( knl_abstim_reached(cur, tm) ) {
				tm = knl_cyc_next_time(cyccb);
			}
			knl_cyc_timer_insert(cyccb, tm);
		}
	} else {
		/* Reset cyclic interval */
		if ( (cyccb->cycstat & TCYC_STA) != 0 ) {
			/* Stop once */
			knl_timer_delete(&cyccb->cyctmeb);
		}

		/* First activation: one period from now */
		tm = cur + cyccb->cyctim;

		/* Start cyclic handler */
		knl_cyc_timer_insert(cyccb, tm);
	}
	cyccb->cycstat |= TCYC_STA;

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_STA_CYC */

#ifdef USE_FUNC_TK_STP_CYC
/*
 * Stop cyclic handler
 */
SYSCALL ER tk_stp_cyc( ID cycid )
{
	CYCCB	*cyccb;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		if ( (cyccb->cycstat & TCYC_STA) != 0 ) {
			/* Stop cyclic handler */
			knl_timer_delete(&cyccb->cyctmeb);
		}
		cyccb->cycstat &= ~TCYC_STA;
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_STP_CYC */

/*
 * Refer cyclic handler state (common part; remaining time in ns)
 */
LOCAL ER knl_ref_cyc_ns( ID cycid, void **exinf, UD *left_ns, UINT *cycstat )
{
	CYCCB	*cyccb;
	ABSTIM	tm, cur;
	ER	ercd = E_OK;

	CHECK_CYCID(cycid);

	cyccb = get_cyccb(cycid);

	BEGIN_CRITICAL_SECTION;
	if ( cyccb->cychdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		tm = cyccb->cyctmeb.time;
		cur = knl_get_mono_ns();
		if ( (cyccb->cycstat & TCYC_STA) == 0 ) {
			if ( knl_abstim_reached(cur, tm) ) {
				tm = knl_cyc_next_time(cyccb);
			}
		}

		*exinf   = cyccb->exinf;
		*left_ns = knl_timer_left_ns(tm, cur);
		*cycstat = cyccb->cycstat;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_REF_CYC
/*
 * Refer cyclic handler state
 */
SYSCALL ER tk_ref_cyc( ID cycid, T_RCYC* pk_rcyc )
{
	UD	left;
	ER	ercd;

	ercd = knl_ref_cyc_ns(cycid, &pk_rcyc->exinf, &left, &pk_rcyc->cycstat);
	if ( ercd == E_OK ) {
		pk_rcyc->lfttim = (RELTIM)(left / NS_PER_MS);
	}

	return ercd;
}
#endif /* USE_FUNC_TK_REF_CYC */

SYSCALL_U ER tk_ref_cyc_u( ID cycid, T_RCYC_U *pk_rcyc_u )
{
	UD	left;
	ER	ercd;

	ercd = knl_ref_cyc_ns(cycid, &pk_rcyc_u->exinf, &left, &pk_rcyc_u->cycstat);
	if ( ercd == E_OK ) {
		pk_rcyc_u->lfttim_u = left / NS_PER_US;
	}

	return ercd;
}

#if USE_DBGSPT

#if USE_OBJECT_NAME
/*
 * Get object name from control block
 */
EXPORT ER knl_cyclichandler_getname(ID id, UB **name)
{
	CYCCB	*cyccb;
	ER	ercd = E_OK;

	CHECK_CYCID(id);

	BEGIN_DISABLE_INTERRUPT;
	cyccb = get_cyccb(id);
	if ( cyccb->cychdr == NULL ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (cyccb->cycatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = cyccb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_CYC
/*
 * Refer cyclic handler usage state
 */
SYSCALL INT td_lst_cyc( ID list[], INT nent )
{
	CYCCB	*cyccb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_cyccb_table + NUM_CYCID;
	for ( cyccb = knl_cyccb_table; cyccb < end; cyccb++ ) {
		/* Unregistered handler */
		if ( cyccb->cychdr == NULL ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_CYC(cyccb - knl_cyccb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_CYC */

#ifdef USE_FUNC_TD_REF_CYC
/*
 * Refer cyclic handler state
 */
SYSCALL ER td_ref_cyc( ID cycid, TD_RCYC* pk_rcyc )
{
	UD	left;
	ER	ercd;

	ercd = knl_ref_cyc_ns(cycid, &pk_rcyc->exinf, &left, &pk_rcyc->cycstat);
	if ( ercd == E_OK ) {
		pk_rcyc->lfttim = (RELTIM)(left / NS_PER_MS);
	}

	return ercd;
}
#endif /* USE_FUNC_TD_REF_CYC */

#endif /* USE_DBGSPT */
#endif /* USE_CYCLICHANDLER */

/* ------------------------------------------------------------------------ */
/*
 *	Alarm handler
 */

#if USE_ALARMHANDLER

Noinit(EXPORT ALMCB knl_almcb_table[NUM_ALMID]);	/* Alarm handler control block */
Noinit(EXPORT QUEUE	knl_free_almcb);	/* FreeQue */


/*
 * Initialization of alarm handler control block
 */
EXPORT ER knl_alarmhandler_initialize( void )
{
	ALMCB	*almcb, *end;

	/* Get system information */
	if ( NUM_ALMID < 1 ) {
		return E_SYS;
	}

	/* Register all control blocks onto FreeQue */
	QueInit(&knl_free_almcb);
	end = knl_almcb_table + NUM_ALMID;
	for ( almcb = knl_almcb_table; almcb < end; almcb++ ) {
		almcb->almhdr = NULL; /* Unregistered handler */
		QueInsert((QUEUE*)almcb, &knl_free_almcb);
	}

	return E_OK;
}


/*
 * Alarm handler start routine
 */
EXPORT void knl_call_almhdr( ALMCB *almcb )
{
	almcb->almstat &= ~TALM_STA;

	/* Execute alarm handler/ Enable interrupt nesting */
	ENABLE_INTERRUPT_UPTO(TIMER_INTLEVEL);
	CallUserHandlerP1(almcb->exinf, almcb->almhdr, almcb);
	DISABLE_INTERRUPT;
}


/*
 * Create alarm handler
 */
SYSCALL ID tk_cre_alm( CONST T_CALM *pk_calm )
{
#if CHK_RSATR
	const ATR VALID_ALMATR = {
		 TA_HLNG
#if USE_OBJECT_NAME
		|TA_DSNAME
#endif
	};
#endif
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_RSATR(pk_calm->almatr, VALID_ALMATR);
	CHECK_PAR(pk_calm->almhdr != NULL);

	BEGIN_CRITICAL_SECTION;
	/* Get control block from free queue */
	almcb = (ALMCB*)QueRemoveNext(&knl_free_almcb);
	if ( almcb == NULL ) {
		ercd = E_LIMIT;
		goto error_exit;
	}

	/* Initialize control block */
	almcb->exinf   = pk_calm->exinf;
	almcb->almatr  = pk_calm->almatr;
	almcb->almhdr  = pk_calm->almhdr;
	almcb->almstat = TALM_STP;
#if USE_OBJECT_NAME
	if ( (pk_calm->almatr & TA_DSNAME) != 0 ) {
		knl_strncpy((char*)almcb->name, (char*)pk_calm->dsname, OBJECT_NAME_LENGTH);
	}
#endif

	ercd = ID_ALM(almcb - knl_almcb_table);

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_DEL_ALM
/*
 * Delete alarm handler
 */
SYSCALL ER tk_del_alm( ID almid )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			/* Delete from timer event queue */
			knl_timer_delete(&almcb->almtmeb);
		}

		/* Return to FreeQue */
		QueInsert((QUEUE*)almcb, &knl_free_almcb);
		almcb->almhdr = NULL; /* Unregistered handler */
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_DEL_ALM */

/*
 * Alarm handler immediate call
 */
LOCAL void knl_immediate_call_almhdr( ALMCB *almcb )
{
	almcb->almstat &= ~TALM_STA;

	/* Execute alarm handler in task-independent part
	   (Keep interrupt disabled) */
	ENTER_TASK_INDEPENDENT;
	CallUserHandlerP1(almcb->exinf, almcb->almhdr, almcb);
	LEAVE_TASK_INDEPENDENT;
}

/*
 * Start alarm handler (common part; relative time in ns)
 */
LOCAL ER knl_sta_alm_ns( ID almid, UD almtim_ns )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
		goto error_exit;
	}

	if ( (almcb->almstat & TALM_STA) != 0 ) {
		/* Cancel current settings */
		knl_timer_delete(&almcb->almtmeb);
	}

	if ( almtim_ns > 0 ) {
		/* Register onto timer event queue */
		knl_alm_timer_insert(almcb, almtim_ns);
		almcb->almstat |= TALM_STA;
	} else {
		/* Immediate execution */
		knl_immediate_call_almhdr(almcb);
	}

    error_exit:
	END_CRITICAL_SECTION;

	return ercd;
}

/*
 * Start alarm handler
 */
SYSCALL ER tk_sta_alm( ID almid, RELTIM almtim )
{
	CHECK_RELTIM(almtim);

	return knl_sta_alm_ns(almid, knl_ms_to_ns(almtim));
}

SYSCALL_U ER tk_sta_alm_u( ID almid, RELTIM_U almtim_u )
{
	CHECK_RELTIM_U(almtim_u);

	return knl_sta_alm_ns(almid, knl_us_to_ns(almtim_u));
}

#ifdef USE_FUNC_TK_STP_ALM
/*
 * Stop alarm handler
 */
SYSCALL ER tk_stp_alm( ID almid )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			/* Stop alarm handler address */
			knl_timer_delete(&almcb->almtmeb);
			almcb->almstat &= ~TALM_STA;
		}
	}
	END_CRITICAL_SECTION;

	return ercd;
}
#endif /* USE_FUNC_TK_STP_ALM */

/*
 * Refer alarm handler state (common part; remaining time in ns)
 */
LOCAL ER knl_ref_alm_ns( ID almid, void **exinf, UD *left_ns, UINT *almstat )
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(almid);

	almcb = get_almcb(almid);

	BEGIN_CRITICAL_SECTION;
	if ( almcb->almhdr == NULL ) { /* Unregistered handler */
		ercd = E_NOEXS;
	} else {
		if ( (almcb->almstat & TALM_STA) != 0 ) {
			*left_ns = knl_timer_left_ns(almcb->almtmeb.time, knl_get_mono_ns());
		} else {
			*left_ns = 0;
		}

		*exinf   = almcb->exinf;
		*almstat = almcb->almstat;
	}
	END_CRITICAL_SECTION;

	return ercd;
}

#ifdef USE_FUNC_TK_REF_ALM
/*
 * Refer alarm handler state
 */
SYSCALL ER tk_ref_alm( ID almid, T_RALM *pk_ralm )
{
	UD	left;
	ER	ercd;

	ercd = knl_ref_alm_ns(almid, &pk_ralm->exinf, &left, &pk_ralm->almstat);
	if ( ercd == E_OK ) {
		pk_ralm->lfttim = (RELTIM)(left / NS_PER_MS);
	}

	return ercd;
}
#endif /* USE_FUNC_TK_REF_ALM */

SYSCALL_U ER tk_ref_alm_u( ID almid, T_RALM_U *pk_ralm_u )
{
	UD	left;
	ER	ercd;

	ercd = knl_ref_alm_ns(almid, &pk_ralm_u->exinf, &left, &pk_ralm_u->almstat);
	if ( ercd == E_OK ) {
		pk_ralm_u->lfttim_u = left / NS_PER_US;
	}

	return ercd;
}

#if USE_DBGSPT

#if USE_OBJECT_NAME
/*
 * Get object name from control block
 */
EXPORT ER knl_alarmhandler_getname(ID id, UB **name)
{
	ALMCB	*almcb;
	ER	ercd = E_OK;

	CHECK_ALMID(id);

	BEGIN_DISABLE_INTERRUPT;
	almcb = get_almcb(id);
	if ( almcb->almhdr == NULL ) {
		ercd = E_NOEXS;
		goto error_exit;
	}
	if ( (almcb->almatr & TA_DSNAME) == 0 ) {
		ercd = E_OBJ;
		goto error_exit;
	}
	*name = almcb->name;

    error_exit:
	END_DISABLE_INTERRUPT;

	return ercd;
}
#endif /* USE_OBJECT_NAME */

#ifdef USE_FUNC_TD_LST_ALM
/*
 * Refer alarm handler usage state
 */
SYSCALL INT td_lst_alm( ID list[], INT nent )
{
	ALMCB	*almcb, *end;
	INT	n = 0;

	BEGIN_DISABLE_INTERRUPT;
	end = knl_almcb_table + NUM_ALMID;
	for ( almcb = knl_almcb_table; almcb < end; almcb++ ) {
		/* Unregistered handler */
		if ( almcb->almhdr == NULL ) {
			continue;
		}

		if ( n++ < nent ) {
			*list++ = ID_ALM(almcb - knl_almcb_table);
		}
	}
	END_DISABLE_INTERRUPT;

	return n;
}
#endif /* USE_FUNC_TD_LST_ALM */

#ifdef USE_FUNC_TD_REF_ALM
/*
 * Refer alarm handler state
 */
SYSCALL ER td_ref_alm( ID almid, TD_RALM *pk_ralm )
{
	UD	left;
	ER	ercd;

	ercd = knl_ref_alm_ns(almid, &pk_ralm->exinf, &left, &pk_ralm->almstat);
	if ( ercd == E_OK ) {
		pk_ralm->lfttim = (RELTIM)(left / NS_PER_MS);
	}

	return ercd;
}
#endif /* USE_FUNC_TD_REF_ALM */

#endif /* USE_DBGSPT */
#endif /* USE_ALARMHANDLER */
