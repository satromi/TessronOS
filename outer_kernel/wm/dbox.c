/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dbox.c
 *	The data box (design 16.2.3)
 *
 *	A table of definitions, each a copy the box owns. Nothing here
 *	knows what a definition means: a panel and a pattern are both bytes
 *	under a name, and what they are for is the business of whoever put
 *	them there.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dbox.h>

typedef struct {
	BOOL	used;
	UINT	kind;
	INT	num;
	ID	owner;
	SZ	size;
	UB	*data;
} DBENT;

LOCAL DBENT	db_tab[DB_MAX_ENT];
LOCAL ID	db_mtxid = 0;

EXPORT INT knl_db_init( void )
{
	T_CMTX	cmtx;
	INT	i;

	if ( db_mtxid > 0 ) {
		return 1;
	}
	for ( i = 0; i < DB_MAX_ENT; i++ ) {
		db_tab[i].used = FALSE;
		db_tab[i].data = NULL;
	}
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	cmtx.ceilpri = 0;
	db_mtxid = tk_cre_mtx(&cmtx);

	return ( db_mtxid > 0 ) ? 1 : 0;
}

LOCAL DBENT *find( UINT kind, INT num, ID owner )
{
	INT	i;

	for ( i = 0; i < DB_MAX_ENT; i++ ) {
		if ( db_tab[i].used && db_tab[i].kind == kind
		  && db_tab[i].num == num && db_tab[i].owner == owner ) {
			return &db_tab[i];
		}
	}

	return NULL;
}

/* The owner's own first, then the system's */
LOCAL DBENT *look_up( UINT kind, INT num, ID owner )
{
	DBENT	*e = find(kind, num, owner);

	if ( e == NULL && owner != 0 ) {
		e = find(kind, num, 0);
	}

	return e;
}

EXPORT ER db_put( UINT kind, INT num, ID owner, CONST void *data, SZ size )
{
	DBENT	*e;
	UB	*copy;
	INT	i;
	SZ	k;

	if ( kind == 0 || data == NULL || size <= 0 || size > DB_MAX_SIZE ) {
		return E_PAR;
	}
	if ( knl_db_init() == 0 ) {
		return E_NOEXS;
	}
	copy = (UB *)Kmalloc(size);
	if ( copy == NULL ) {
		return E_NOMEM;
	}
	for ( k = 0; k < size; k++ ) {
		copy[k] = ((CONST UB *)data)[k];
	}
	tk_loc_mtx(db_mtxid, TMO_FEVR);
	e = find(kind, num, owner);
	if ( e == NULL ) {
		for ( i = 0; i < DB_MAX_ENT; i++ ) {
			if ( !db_tab[i].used ) {
				e = &db_tab[i];
				break;
			}
		}
		if ( e == NULL ) {
			tk_unl_mtx(db_mtxid);
			Kfree(copy);
			return E_LIMIT;
		}
		e->used  = TRUE;
		e->kind  = kind;
		e->num   = num;
		e->owner = owner;
		e->data  = NULL;
	}
	if ( e->data != NULL ) {
		Kfree(e->data);		/* whatever stood under this name */
	}
	e->data = copy;
	e->size = size;
	tk_unl_mtx(db_mtxid);

	return E_OK;
}

EXPORT INT db_size( UINT kind, INT num, ID owner )
{
	DBENT	*e;
	INT	n;

	if ( db_mtxid <= 0 ) {
		return E_NOEXS;
	}
	tk_loc_mtx(db_mtxid, TMO_FEVR);
	e = look_up(kind, num, owner);
	n = ( e != NULL ) ? (INT)e->size : E_NOEXS;
	tk_unl_mtx(db_mtxid);

	return n;
}

EXPORT INT db_get( UINT kind, INT num, ID owner, void *buf, SZ max )
{
	DBENT	*e;
	INT	n;
	SZ	k;

	if ( buf == NULL || max <= 0 ) {
		return E_PAR;
	}
	if ( db_mtxid <= 0 ) {
		return E_NOEXS;
	}
	tk_loc_mtx(db_mtxid, TMO_FEVR);
	e = look_up(kind, num, owner);
	if ( e == NULL ) {
		n = E_NOEXS;
	} else if ( e->size > max ) {
		n = E_LIMIT;		/* nothing written: the caller asks again */
	} else {
		for ( k = 0; k < e->size; k++ ) {
			((UB *)buf)[k] = e->data[k];
		}
		n = (INT)e->size;
	}
	tk_unl_mtx(db_mtxid);

	return n;
}

EXPORT ER db_del( UINT kind, INT num, ID owner )
{
	DBENT	*e;

	if ( db_mtxid <= 0 ) {
		return E_NOEXS;
	}
	tk_loc_mtx(db_mtxid, TMO_FEVR);
	e = find(kind, num, owner);
	if ( e == NULL ) {
		tk_unl_mtx(db_mtxid);
		return E_NOEXS;
	}
	if ( e->data != NULL ) {
		Kfree(e->data);
	}
	e->data = NULL;
	e->used = FALSE;
	tk_unl_mtx(db_mtxid);

	return E_OK;
}

EXPORT INT db_del_owner( ID owner )
{
	INT	i, n = 0;

	if ( db_mtxid <= 0 ) {
		return 0;
	}
	tk_loc_mtx(db_mtxid, TMO_FEVR);
	for ( i = 0; i < DB_MAX_ENT; i++ ) {
		if ( db_tab[i].used && db_tab[i].owner == owner ) {
			if ( db_tab[i].data != NULL ) {
				Kfree(db_tab[i].data);
			}
			db_tab[i].data = NULL;
			db_tab[i].used = FALSE;
			n++;
		}
	}
	tk_unl_mtx(db_mtxid);

	return n;
}

EXPORT INT db_count( void )
{
	INT	i, n = 0;

	for ( i = 0; i < DB_MAX_ENT; i++ ) {
		if ( db_tab[i].used ) {
			n++;
		}
	}

	return n;
}
