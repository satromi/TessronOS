/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtask.h
 *
 *	A request to the desktop and its answer, each one message with its
 *	tail (ts/dtreq.h, DT_TAIL_*), for a program that asks the desktop
 *	itself (a program built on lib/libtv uses ask_desktop there). The
 *	words of a request -- a name, a text, an applist, a question -- go
 *	in the tail, as long as they are; the answer is read whole, however
 *	long its tail, into a buffer made as large as it must be. Nothing
 *	is cut: a part that cannot be kept makes the request fail
 *	(E_NOMEM) rather than go without it.
 *
 *	  DTASK	q;		zeroed: empty (dq_init)
 *	  dq_part(&q, DT_TAIL_TEXT, s, -1);
 *	  er = dq_ask(&q, rq, an, win, wait_ms);
 *	  t = dq_ans_part(&q, DT_TAIL_TEXT, &len);	the answer's text, NULL none
 *	  dq_free(&q);		when the program is done with it
 *
 *	dq_ask sends the parts added since the last dq_ask and starts the
 *	next request empty. dq_read reads one message of a channel whole.
 */

#ifndef __APP_DTASK_H__
#define __APP_DTASK_H__

#include <string.h>
#include <stdlib.h>
#include <ts/ob.h>
#include <ts/dtreq.h>

typedef struct {
	UB	*tail;			/* the next request's tail */
	SZ	cap;
	UINT	tailsz;
	ER	er;			/* E_NOMEM when a part could not be kept */
	UB	*ans;			/* the last answer, whole */
	SZ	acap;
	SZ	asz;			/* its size; 0 none */
} DTASK;

static __inline__ void dq_init( DTASK *q )
{
	memset(q, 0, sizeof(*q));
}

static __inline__ void dq_free( DTASK *q )
{
	free(q->tail);
	free(q->ans);
	memset(q, 0, sizeof(*q));
}

/*
 * Room for more bytes in the next request's tail, for parts put there by
 * another (q->tail, &q->tailsz, q->cap, as dt_tail_put takes them)
 */
static __inline__ ER dq_room( DTASK *q, SZ more )
{
	SZ	need = (SZ)q->tailsz + more;

	if ( need > q->cap ) {
		SZ	cap = ( q->cap == 0 ) ? 256 : q->cap;
		UB	*t;

		while ( cap < need ) cap *= 2;
		t = (UB *)realloc(q->tail, (size_t)cap);
		if ( t == NULL ) {
			q->er = E_NOMEM;
			return E_NOMEM;
		}
		q->tail = t;
		q->cap = cap;
	}
	return E_OK;
}

/* A part of the next request's tail added, len bytes of s (len < 0 up to its 0) */
static __inline__ ER dq_part( DTASK *q, UINT kind, CONST UB *s, SZ len )
{
	SZ	need;

	if ( s == NULL ) s = (CONST UB *)"";
	if ( len < 0 ) len = (SZ)strlen((CONST char *)s);
	need = (SZ)q->tailsz + dt_tail_room(len);
	if ( need > q->cap ) {
		SZ	cap = ( q->cap == 0 ) ? 256 : q->cap;
		UB	*t;

		while ( cap < need ) cap *= 2;
		t = (UB *)realloc(q->tail, (size_t)cap);
		if ( t == NULL ) {
			q->er = E_NOMEM;
			return E_NOMEM;
		}
		q->tail = t;
		q->cap = cap;
	}
	return dt_tail_put(q->tail, &q->tailsz, q->cap, kind, s, len);
}

/* One message of channel k read whole into *p_buf (*p_cap bytes, made larger as it must be): its size, or an error */
static __inline__ SZ dq_read( ID k, UB **p_buf, SZ *p_cap, TMO tmo )
{
	SZ	asz = 0;
	ER	er;

	for ( ;; ) {
		if ( *p_buf == NULL || *p_cap < (SZ)sizeof(T_DTANS) ) {
			UB	*b = (UB *)realloc(*p_buf, sizeof(T_DTANS) + 256);

			if ( b == NULL ) return E_NOMEM;
			*p_buf = b;
			*p_cap = (SZ)sizeof(T_DTANS) + 256;
		}
		er = ob_rea_tmo(k, 0, 0, *p_buf, *p_cap, &asz, tmo);
		if ( er == E_LIMIT && asz > *p_cap ) {
			UB	*b = (UB *)realloc(*p_buf, (size_t)asz);

			if ( b == NULL ) return E_NOMEM;
			*p_buf = b;
			*p_cap = asz;
			continue;		/* the message stays first on the channel: read again, whole */
		}
		return ( er < E_OK ) ? (SZ)er : asz;
	}
}

/*
 * rq sent with the parts added since the last one, over the window win
 * (NULL: the one rq names), and its answer waited for at most wait_ms:
 * copied to an, and kept whole for dq_ans_part. The answer's er, or why
 * there is none.
 */
static __inline__ ER dq_ask( DTASK *q, T_DTREQ *rq, T_DTANS *an, CONST TS_UUID *win, INT wait_ms )
{
	static UINT	seq;
	TS_UUID		d, reply;
	T_OBCRE		c;
	SZ		asz = 0, msz;
	ID		k, ka;
	ER		er;
	UB		*msg;

	q->asz = 0;
	if ( q->er < E_OK ) {
		er = q->er;
		q->er = E_OK;
		q->tailsz = 0;
		return er;
	}
	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) {
		q->tailsz = 0;
		return E_NOEXS;
	}
	msz = (SZ)sizeof(*rq) + (SZ)q->tailsz;
	msg = (UB *)malloc((size_t)msz);
	if ( msg == NULL ) {
		q->tailsz = 0;
		return E_NOMEM;
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) {
		free(msg);
		q->tailsz = 0;
		return E_NOMEM;
	}
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		free(msg);
		q->tailsz = 0;
		return E_OBJ;
	}
	rq->seq = ++seq;
	rq->reply = reply;
	if ( win != NULL ) rq->win = *win;
	rq->tailsz = q->tailsz;
	memcpy(msg, rq, sizeof(*rq));
	if ( q->tailsz > 0 ) memcpy(msg + sizeof(*rq), q->tail, q->tailsz);
	q->tailsz = 0;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, msg, msz, &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	free(msg);
	while ( er >= E_OK ) {
		SZ	n = dq_read(ka, &q->ans, &q->acap, wait_ms);

		if ( n < 0 ) {
			er = (ER)n;
			break;
		}
		if ( n >= (SZ)sizeof(*an) && ((T_DTANS *)q->ans)->seq == rq->seq ) {
			q->asz = n;
			memcpy(an, q->ans, sizeof(*an));
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* A part of the last answer's tail: its bytes (ending in 0) and length, NULL when it has none */
static __inline__ CONST UB *dq_ans_part( CONST DTASK *q, UINT kind, SZ *p_len )
{
	CONST T_DTANS	*a = (CONST T_DTANS *)q->ans;

	if ( p_len != NULL ) *p_len = 0;
	if ( a == NULL || q->asz < (SZ)sizeof(T_DTANS) ) return NULL;
	return dt_tail_get(dt_ans_tail(a, q->asz), a->tailsz, kind, p_len);
}

/* The same, "" when it has none */
static __inline__ CONST UB *dq_ans_text( CONST DTASK *q, UINT kind )
{
	CONST UB	*t = dq_ans_part(q, kind, NULL);

	return ( t != NULL ) ? t : (CONST UB *)"";
}

/* The same in memory of its own, to be changed and freed: "" when it has none, NULL no memory */
static __inline__ char *dq_ans_dup( CONST DTASK *q, UINT kind )
{
	SZ		len = 0;
	CONST UB	*t = dq_ans_part(q, kind, &len);
	char		*s = (char *)malloc((size_t)len + 1);

	if ( s == NULL ) return NULL;
	if ( t != NULL && len > 0 ) memcpy(s, t, (size_t)len);
	s[len] = 0;
	return s;
}

/* A part of a notice read whole (n bytes at m): "" when it has none */
static __inline__ CONST UB *dq_told_text( CONST void *m, SZ n, UINT kind, SZ *p_len )
{
	CONST T_DTANS	*a = (CONST T_DTANS *)m;
	CONST UB	*t;

	if ( p_len != NULL ) *p_len = 0;
	if ( m == NULL || n < (SZ)sizeof(T_DTANS) ) return (CONST UB *)"";
	t = dt_tail_get(dt_ans_tail(a, n), a->tailsz, kind, p_len);
	return ( t != NULL ) ? t : (CONST UB *)"";
}

#endif /* __APP_DTASK_H__ */
