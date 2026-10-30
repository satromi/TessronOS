/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obdrop.h
 *	The latest drop on a window (ob.h, OB_WR_DROP), read whole by a
 *	program that has the C library's memory (malloc): a drop has as
 *	many links, or as many letters, as were let go.
 */

#ifndef __TS_OBDROP_H__
#define __TS_OBDROP_H__

#include <stdlib.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>

/*
 * The drop, in memory of its own (free it), with a nought after it so
 * that the letters of a drop of letters end; NULL when there has been
 * none, or no memory. It is read from the start with twice the room
 * while it fills what it was read into.
 */
static inline T_OBDROP *ob_drop_read( ID kw )
{
	SZ		cap = (SZ)OB_DROP_SIZE(16), asz;
	T_OBDROP	*d;

	for ( ;; ) {
		d = (T_OBDROP *)malloc((size_t)cap + 1);
		if ( d == NULL ) {
			return NULL;
		}
		asz = 0;
		if ( ob_rea_rec(kw, OB_WR_DROP, 0, d, cap, &asz) < E_OK || asz < (SZ)sizeof(T_OBDROP) ) {
			free(d);
			return NULL;
		}
		if ( asz < cap ) {
			((UB *)d)[asz] = 0;
			if ( d->n > 0 && asz < (SZ)OB_DROP_SIZE(d->n) ) {
				free(d);		/* shorter than it says: not one */
				return NULL;
			}
			return d;
		}
		free(d);
		cap *= 2;
	}
}

#endif /* __TS_OBDROP_H__ */
