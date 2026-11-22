/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_admin.c
 *	The administrator's rights for one operation, asked of the desktop
 *	(DT_RQ_ADMIN of include/ts/dtreq.h, docs/tessronos/08-object.md 8.7)
 *
 *	A program started from the desktop acts for the person at the
 *	machine. A call that only an administrator may make answers it
 *	E_OACV; the program then asks here, the desktop asks the person for
 *	the administrator's password in a panel of its own, and when it was
 *	typed right the call is made again and the rights given back:
 *
 *		er = dt_settime(t);
 *		if ( er == E_OACV && ts_admin_ask("時刻の設定", &win) >= E_OK ) {
 *			er = dt_settime(t);
 *			(void)ob_end_adm();
 *		}
 *		if ( er == E_OACV ) ts_no_right("時刻を設定");
 *
 *	Only the calls on objects are used, and the bytes are set by hand,
 *	so that every C program (lib/libts) links it whichever C library it
 *	has.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/dtreq.h>

IMPORT ER tk_dly_tsk( RELTIM dlytim );
IMPORT ER wm_msg_put( CONST char *utf8 );

#define ADM_POLL_MS	50
#define ADM_WAIT_MS	( 10 * 60 * 1000 )	/* a person may take a while to type it */

LOCAL void zero( void *p, SZ n )
{
	UB	*b = (UB *)p;

	while ( n-- > 0 ) *b++ = 0;
}

ER ts_admin_ask( CONST char *what, CONST TS_UUID *win )
{
	T_DTREQ	rq;
	T_DTANS	an;
	T_OBCRE	c;
	TS_UUID	d, reply;
	SZ	asz = 0;
	ID	k, ka;
	INT	i, waited;
	ER	er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) {
		return E_NOEXS;			/* no desktop to ask */
	}
	zero(&c, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) {
		return E_NOMEM;
	}
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	zero(&rq, sizeof(rq));
	rq.req = DT_RQ_ADMIN;
	rq.seq = 1;
	rq.reply = reply;
	for ( i = 0; what != NULL && what[i] != 0 && i < (INT)sizeof(rq.name) - 1; i++ ) {
		rq.name[i] = (UB)what[i];
	}
	if ( win != NULL ) {
		rq.win = *win;
	}
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz) : (ER)k;
	if ( k > 0 ) {
		(void)ob_cls_obj(k);
	}
	for ( waited = 0; er >= E_OK; waited += ADM_POLL_MS ) {
		if ( ob_rea_rec(ka, 0, 0, &an, sizeof(an), &asz) >= E_OK && asz == (SZ)sizeof(an)
		  && an.seq == rq.seq ) {
			er = an.er;
			break;
		}
		if ( waited >= ADM_WAIT_MS ) {
			er = E_TMOUT;
			break;
		}
		(void)tk_dly_tsk(ADM_POLL_MS);
	}
	(void)ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* The person told, on the message line, that what could not be done for want of the rights */
void ts_no_right( CONST char *what )
{
	CONST char *tail = "権限がありません", *suru = "する";
	INT	w = 0, n = 0, i;

	/* the line as long as what it says (this library allocates nothing: it is on the stack) */
	while ( what != NULL && what[w] != 0 ) w++;
	{
		char	line[w + 32];

		for ( i = 0; i < w; i++ ) line[n++] = what[i];
		if ( n > 0 ) {
			for ( i = 0; suru[i] != 0; i++ ) line[n++] = suru[i];
		}
		for ( i = 0; tail[i] != 0; i++ ) line[n++] = tail[i];
		line[n] = 0;
		(void)wm_msg_put(line);
	}
}
