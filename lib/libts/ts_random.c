/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_random.c
 *	Random bytes for a process, from the random source object 乱数
 *	(ob_uuid_random, OB_RND_DATA).
 *
 *	The object is opened once, the first time bytes are asked for, and
 *	the key kept for as long as the process runs; each call is then one
 *	read of its record 1. Two threads that ask at once for the first
 *	time may both open it: the one that comes second closes its own key
 *	and uses the first. Only the calls on objects are used, so the C
 *	programs (lib/libts) and the C++ ones (lib/libcxxrt) both link it.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>

static ID	rnd_key = 0;

/* The key to 乱数, opened the first time */
ID ts_random_key( void )
{
	CONST TS_UUID	u = OB_UUID_RANDOM_INIT;
	ID		k = __atomic_load_n(&rnd_key, __ATOMIC_ACQUIRE), none = 0;

	if ( k > 0 ) {
		return k;
	}
	k = ob_opn_obj(&u, OB_OP_READ);
	if ( k <= 0 ) {
		return k;
	}
	if ( !__atomic_compare_exchange_n(&rnd_key, &none, k, FALSE, __ATOMIC_ACQ_REL,
					  __ATOMIC_ACQUIRE) ) {
		(void)ob_cls_obj(k);		/* another thread's came first */
		k = none;
	}
	return k;
}

ER ts_get_random( void *buf, SZ len )
{
	ID	k;
	SZ	asz = 0;
	ER	er;

	if ( len <= 0 ) {
		return ( len == 0 ) ? E_OK : E_PAR;
	}
	k = ts_random_key();
	if ( k <= 0 ) {
		return ( k < 0 ) ? (ER)k : E_NOEXS;
	}
	er = ob_rea_rec(k, OB_RND_DATA, 0, buf, len, &asz);
	if ( er < E_OK ) {
		return er;
	}
	return ( asz == len ) ? E_OK : E_IO;
}
