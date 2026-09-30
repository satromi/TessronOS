/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obuser.c
 *	Users, their passwords, and logging in (design 18.10)
 *
 *	A user is a file object whose metadata says so:
 *	  "tessronos": { "user": { "name": ..., "groups": [ UUID, ... ],
 *				"salt": hex, "hash": hex, "iter": N } }
 *	The hash is PBKDF2-HMAC-SHA256 of the password with the salt; the
 *	password itself is kept nowhere. Logging in checks it and gives the
 *	calling process the user's credentials: the user and its groups,
 *	the first of them its own group.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/proc.h>
#include "obj.h"

#define OBU_SALT	16
#define OBU_ITER	4096

LOCAL INT hex_of( UB c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/* Hex text as bytes: how many, or -1 */
LOCAL INT unhex( CONST UB *s, UB *out, INT max )
{
	INT	n = 0, a, b;

	while ( s[0] != 0 && s[1] != 0 && n < max ) {
		a = hex_of(s[0]);
		b = hex_of(s[1]);
		if ( a < 0 || b < 0 ) return -1;
		out[n++] = (UB)( ( a << 4 ) | b );
		s += 2;
	}
	return ( s[0] == 0 ) ? n : -1;
}

LOCAL void tohex( CONST UB *in, INT n, UB *out )
{
	CONST char	*h = "0123456789abcdef";
	INT		i;

	for ( i = 0; i < n; i++ ) {
		out[2 * i] = (UB)h[in[i] >> 4];
		out[2 * i + 1] = (UB)h[in[i] & 15];
	}
	out[2 * n] = 0;
}

/* The same bytes, looked at all the way through so the time says nothing */
LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	UB	d = 0;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		d |= (UB)( a[i] ^ b[i] );
	}
	return (BOOL)( d == 0 );
}

/*
 * Whether a process acts as an administrator: the system, or a user in
 * the administrators' group. The kernel (0) is.
 */
EXPORT BOOL knl_ob_pid_admin( ID pid )
{
	CONST T_OBCRD	*crd;
	INT		i;

	if ( pid <= 0 ) {
		return TRUE;
	}
	crd = knl_ob_crd_of(pid);
	if ( crd == NULL ) {
		return FALSE;
	}
	if ( ts_uuid_cmp(&crd->user, &ob_user_system) == 0 ) {
		return TRUE;
	}
	for ( i = 0; i < crd->ngrp && i < CNF_OB_MAX_GROUPS; i++ ) {
		if ( ts_uuid_cmp(&crd->grp[i], &ob_group_admin) == 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT ER knl_ob_login_check( CONST TS_UUID *user, CONST UB *password, T_OBCRD *crd )
{
	UB	*j, s[80], salt[64], want[OB_SHA256_LEN], got[OB_SHA256_LEN];
	INT	len = 0, u, arr, pos, e, nsalt, pwlen = 0;
	D	iter = OBU_ITER;
	ER	er;

	if ( user == NULL || password == NULL || crd == NULL ) {
		return E_PAR;
	}
	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	er = knl_ob_meta_read(user, j, OB_META_MAX, &len);
	if ( er < E_OK ) {
		goto out;
	}
	u = knl_oj_path(j, len, "tessronos", "user");
	if ( u < 0 || j[u] != '{' ) {
		er = E_OBJ;			/* not a user */
		goto out;
	}
	/*
	 * Only a user the system made counts: one owned by the system, or
	 * by itself, which only an administrator can arrange. Anyone may
	 * write an object that says it is a user of the administrators'
	 * group; logging in as that must not work.
	 */
	{
		CONST T_OBMGR	*m = knl_ob_mgr_of(user);
		T_OBPRT		*prt = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
		BOOL		trusted = FALSE;

		if ( prt != NULL && m != NULL && m->prot != NULL
		  && m->prot(user, prt) >= E_OK ) {
			trusted = (BOOL)( ts_uuid_cmp(&prt->owner, &ob_user_system) == 0
				       || ts_uuid_cmp(&prt->owner, user) == 0 );
		}
		if ( prt != NULL ) Kfree(prt);
		if ( !trusted ) {
			er = E_OACV;
			goto out;
		}
	}
	if ( knl_oj_str(j, len, knl_oj_member(j, len, u, "salt"), s, sizeof(s)) < 0
	  || ( nsalt = unhex(s, salt, sizeof(salt)) ) <= 0
	  || knl_oj_str(j, len, knl_oj_member(j, len, u, "hash"), s, sizeof(s)) < 0
	  || unhex(s, want, OB_SHA256_LEN) != OB_SHA256_LEN ) {
		er = E_OACV;			/* no password set: nobody logs in as it */
		goto out;
	}
	(void)knl_oj_num(j, len, knl_oj_member(j, len, u, "iter"), &iter);
	if ( iter < 1 || iter > 1000000 ) {
		iter = OBU_ITER;
	}
	while ( password[pwlen] != 0 ) pwlen++;
	knl_ob_pbkdf2(password, (SZ)pwlen, salt, (SZ)nsalt, (INT)iter, got);
	if ( !same_bytes(got, want, OB_SHA256_LEN) ) {
		er = E_OACV;
		goto out;
	}

	knl_memset(crd, 0, sizeof(*crd));
	crd->user = *user;
	arr = knl_oj_member(j, len, u, "groups");
	pos = arr;
	while ( arr >= 0 && crd->ngrp < CNF_OB_MAX_GROUPS
	     && ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
		UB	us[TS_UUID_STRLEN + 1];

		if ( knl_oj_str(j, len, e, us, sizeof(us)) == TS_UUID_STRLEN
		  && ts_str_to_uuid((CONST char *)us, &crd->grp[crd->ngrp]) >= E_OK ) {
			crd->ngrp++;
		}
	}
	er = E_OK;
    out:
	Kfree(j);
	return er;
}

EXPORT ER ob_login( CONST TS_UUID *user, CONST UB *password )
{
	T_OBCRD	*crd;
	ID	pid = ts_get_pid();
	ER	er;

	if ( pid <= 0 ) {
		return E_OBJ;			/* the kernel acts as the system, always */
	}
	crd = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	if ( crd == NULL ) {
		return E_NOMEM;
	}
	er = knl_ob_login_check(user, password, crd);
	if ( er >= E_OK ) {
		er = knl_ob_set_crd(pid, crd);
	}
	Kfree(crd);

	return er;
}

/* A new password: for the user itself, or an administrator */
EXPORT ER ob_set_pwd( CONST TS_UUID *user, CONST UB *password )
{
	CONST T_OBCRD	*me = knl_ob_crd_of(ts_get_pid());
	UB		*j, salt[OBU_SALT], hash[OB_SHA256_LEN];
	UB		hs[2 * OB_SHA256_LEN + 3], ss[2 * OBU_SALT + 3], q[2 * OB_SHA256_LEN + 3];
	INT		len = 0, u, pwlen = 0, n;
	ER		er;

	if ( user == NULL || password == NULL ) {
		return E_PAR;
	}
	if ( ts_uuid_cmp(&me->user, user) != 0 && !knl_ob_is_admin(me) ) {
		return E_OACV;
	}
	er = ts_get_random(salt, sizeof(salt));
	if ( er < E_OK ) {
		return er;
	}
	while ( password[pwlen] != 0 ) pwlen++;
	knl_ob_pbkdf2(password, (SZ)pwlen, salt, sizeof(salt), OBU_ITER, hash);

	j = (UB *)Kmalloc(OB_META_MAX);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	er = knl_ob_meta_read(user, j, OB_META_MAX, &len);
	if ( er < E_OK ) {
		Kfree(j);
		return er;
	}
	u = knl_oj_path(j, len, "tessronos", "user");
	if ( u < 0 || j[u] != '{' ) {
		Kfree(j);
		return E_OBJ;
	}
	tohex(salt, sizeof(salt), ss);
	tohex(hash, sizeof(hash), hs);

	n = knl_oj_put_str(q, 0, sizeof(q), ss);
	len = knl_oj_set(j, len, OB_META_MAX, u, "salt", q, n);
	u = ( len > 0 ) ? knl_oj_path(j, len, "tessronos", "user") : -1;
	n = knl_oj_put_str(q, 0, sizeof(q), hs);
	len = ( u >= 0 ) ? knl_oj_set(j, len, OB_META_MAX, u, "hash", q, n) : -1;
	u = ( len > 0 ) ? knl_oj_path(j, len, "tessronos", "user") : -1;
	n = knl_oj_put_num(q, 0, sizeof(q), OBU_ITER);
	len = ( u >= 0 ) ? knl_oj_set(j, len, OB_META_MAX, u, "iter", q, n) : -1;

	er = ( len > 0 ) ? knl_ob_meta_write(user, j, len) : E_LIMIT;
	Kfree(j);

	return er;
}
