/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obatr.h
 *	An object's metadata (ob.h, ob_get_atr) and the list of its records
 *	(ob_lst_rec) read whole by a program that has the C library's memory
 *	(malloc): each is as long as it is.
 */

#ifndef __TS_OBATR_H__
#define __TS_OBATR_H__

#include <stdlib.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>

/*
 * The metadata of the object open as key, in memory of its own (free
 * it) with room bytes more after the text and a nought after that: its
 * length, or an error. It is read from the start with twice the room
 * while it fills what it was read into, up to OB_META_LIMIT.
 */
static inline INT ob_atr_read( ID key, INT room, UB **p_buf )
{
	SZ	cap = 4096, asz;
	UB	*b;
	ER	er;

	*p_buf = NULL;
	if ( room < 0 ) {
		return E_PAR;
	}
	for ( ;; ) {
		b = (UB *)malloc((size_t)cap + (size_t)room + 1);
		if ( b == NULL ) {
			return E_NOMEM;
		}
		asz = 0;
		er = ob_get_atr(key, b, cap, &asz);
		if ( er < E_OK ) {
			free(b);
			return (INT)er;
		}
		if ( asz < cap ) {
			b[asz] = 0;
			*p_buf = b;
			return (INT)asz;
		}
		free(b);
		if ( cap >= OB_META_LIMIT ) {
			return E_LIMIT;
		}
		cap *= 2;
	}
}

/*
 * The records of the object open as key, every one, in memory of its own
 * (free it), and how many in *p_cnt: listed again with twice the room
 * while they fill it. NULL when there is no memory for them, or the list
 * cannot be read (*p_cnt 0).
 */
static inline T_OBREC *ob_rec_read( ID key, INT *p_cnt )
{
	T_OBREC	*r;
	INT	cap = 64, cnt;

	*p_cnt = 0;
	for ( ;; ) {
		r = (T_OBREC *)malloc(sizeof(T_OBREC) * (size_t)cap);
		if ( r == NULL ) {
			return NULL;
		}
		cnt = 0;
		if ( ob_lst_rec(key, r, cap, &cnt) < E_OK ) {
			free(r);
			return NULL;
		}
		if ( cnt < cap ) {
			*p_cnt = cnt;
			return r;
		}
		free(r);
		cap *= 2;
	}
}

#endif /* __TS_OBATR_H__ */
