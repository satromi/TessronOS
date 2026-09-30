/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obprot.c
 *	Protection: what a subject may do to an object (design 18.9)
 *
 *	The protection is kept in the metadata under "tessronos"."access";
 *	an object without it -- one a TADjs Desktop wrote -- has no owner
 *	and lets everyone do what its readable/editable/deletable flags
 *	allow. The decision is made in operation bits, the rwx of the mode
 *	standing for groups of them.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include "obj.h"

LOCAL CONST struct {
	CONST char	*name;
	UINT		ops;
} op_names[] = {
	{ "read",	OB_OP_READ },
	{ "atrread",	OB_OP_ATRRD },
	{ "write",	OB_OP_WRITE },
	{ "record",	OB_OP_RECORD },
	{ "atrwrite",	OB_OP_ATRWR },
	{ "link",	OB_OP_LINK },
	{ "delete",	OB_OP_DELETE },
	{ "exec",	OB_OP_EXEC },
	{ "prot",	OB_OP_PROT },
	{ "r",		OB_OP_R },
	{ "w",		OB_OP_W },
	{ "x",		OB_OP_X },
};

EXPORT BOOL knl_ob_uuid_zero( CONST TS_UUID *u )
{
	return (BOOL)( u->d.hi == 0 && u->d.lo == 0 );
}

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* The operations one rwx triple stands for */
LOCAL UINT expand( UINT rwx )
{
	return ( ( rwx & 4 ) ? OB_OP_R : 0 ) | ( ( rwx & 2 ) ? OB_OP_W : 0 )
	     | ( ( rwx & 1 ) ? OB_OP_X : 0 );
}

/* An object written without TessronOS's protection: no owner, the flags it has */
EXPORT void knl_ob_prt_legacy( T_OBPRT *prt, BOOL readable, BOOL editable,
			       BOOL deletable )
{
	knl_memset(prt, 0, sizeof(*prt));
	prt->mode = 0777;
	if ( !readable ) prt->mode &= ~0444U;
	if ( !editable ) prt->mode &= ~0222U;
	if ( !deletable ) prt->attr |= OB_A_PERM;
}

/* What an object gets when its maker says nothing: the maker's, rw-r--r-- */
EXPORT void knl_ob_prt_default( T_OBPRT *prt, CONST T_OBCRD *crd )
{
	knl_memset(prt, 0, sizeof(*prt));
	prt->mode = 0644;
	if ( crd != NULL ) {
		prt->owner = crd->user;
		if ( crd->ngrp > 0 ) {
			prt->group = crd->grp[0];
		}
	}
}

/* ---------------------------------------------------------------- deciding */

EXPORT UINT knl_ob_permit( CONST T_OBCRD *crd, CONST T_OBPRT *prt, INT recno )
{
	UINT	ops, rwx;
	INT	i, g;
	BOOL	owner = FALSE, member = FALSE;

	if ( crd == NULL || prt == NULL ) {
		return 0;
	}
	if ( knl_ob_is_admin(crd) ) {
		ops = OB_OP_ALL;
	} else {
		if ( !knl_ob_uuid_zero(&prt->owner) && same_uuid(&prt->owner, &crd->user) ) {
			owner = TRUE;
		}
		if ( !owner && !knl_ob_uuid_zero(&prt->group) ) {
			for ( g = 0; g < crd->ngrp; g++ ) {
				if ( same_uuid(&prt->group, &crd->grp[g]) ) {
					member = TRUE;
					break;
				}
			}
		}
		rwx = owner ? ( prt->mode >> 6 ) : member ? ( prt->mode >> 3 ) : prt->mode;
		ops = expand(rwx & 7);

		/* the list adds; it never takes away */
		for ( i = 0; i < prt->nacl && i < OB_ACL_MAX; i++ ) {
			CONST T_OBACE	*a = &prt->acl[i];

			if ( a->kind == OB_ACE_USER && same_uuid(&a->who, &crd->user) ) {
				ops |= a->ops;
			} else if ( a->kind == OB_ACE_GROUP ) {
				for ( g = 0; g < crd->ngrp; g++ ) {
					if ( same_uuid(&a->who, &crd->grp[g]) ) {
						ops |= a->ops;
						break;
					}
				}
			}
		}
		if ( owner ) {
			ops |= OB_OP_PROT;
		}
	}

	/* the attributes bind everyone, administrators too */
	if ( ( prt->attr & OB_A_RONLY ) != 0 ) {
		ops &= ~( OB_OP_WRITE | OB_OP_RECORD );
	}
	if ( ( prt->attr & OB_A_PERM ) != 0 ) {
		ops &= ~OB_OP_DELETE;
	}

	/* a record's own limit, on what touches that record */
	if ( recno >= 0 ) {
		CONST UINT	onrec = OB_OP_READ | OB_OP_WRITE | OB_OP_RECORD;

		for ( i = 0; i < prt->nrmask && i < OB_RMASK_MAX; i++ ) {
			if ( prt->rmask[i].recno == recno ) {
				UINT	lim = prt->rmask[i].ops;

				/* a limit that names x: only one who may control the object */
				if ( ( lim & OB_OP_EXEC ) != 0 && ( ops & OB_OP_EXEC ) == 0 ) lim = 0;
				ops = ( ops & ~onrec ) | ( ops & lim & onrec );
				break;
			}
		}
	}
	return ops;
}

/* ---------------------------------------------------------------- reading */

LOCAL UINT parse_ops( CONST UB *j, INT len, INT arr )
{
	UINT	ops = 0;
	INT	pos = arr, e;
	UB	s[16];

	while ( ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
		UINT	k;

		if ( knl_oj_str(j, len, e, s, sizeof(s)) < 0 ) {
			continue;
		}
		for ( k = 0; k < sizeof(op_names) / sizeof(op_names[0]); k++ ) {
			CONST char	*n = op_names[k].name;
			INT		c = 0;

			while ( n[c] != 0 && n[c] == (char)s[c] ) c++;
			if ( n[c] == 0 && s[c] == 0 ) {
				ops |= op_names[k].ops;
				break;
			}
		}
	}
	return ops;
}

LOCAL BOOL parse_uuid( CONST UB *j, INT len, INT v, TS_UUID *u )
{
	UB	s[TS_UUID_STRLEN + 1];

	return (BOOL)( v >= 0 && knl_oj_str(j, len, v, s, sizeof(s)) == TS_UUID_STRLEN
		    && ts_str_to_uuid((CONST char *)s, u) >= E_OK );
}

/* "rwxr-x---" as nine bits */
LOCAL UINT parse_mode( CONST UB *s )
{
	UINT	m = 0;
	INT	i;

	for ( i = 0; i < 9 && s[i] != 0; i++ ) {
		m <<= 1;
		if ( s[i] != '-' ) {
			m |= 1;
		}
	}
	return ( i == 9 ) ? m : 0;
}

EXPORT ER knl_ob_prt_parse( CONST UB *j, INT len, T_OBPRT *prt )
{
	INT	acc, v, pos, e;
	UB	s[16];

	acc = knl_oj_path(j, len, "tessronos", "access");
	if ( acc < 0 || j[acc] != '{' ) {
		INT	root = knl_oj_root(j, len);
		BOOL	rd = TRUE, ed = TRUE, dl = TRUE;

		(void)knl_oj_bool(j, len, knl_oj_member(j, len, root, "readable"), &rd);
		(void)knl_oj_bool(j, len, knl_oj_member(j, len, root, "editable"), &ed);
		(void)knl_oj_bool(j, len, knl_oj_member(j, len, root, "deletable"), &dl);
		knl_ob_prt_legacy(prt, rd, ed, dl);
		return E_OK;
	}

	knl_memset(prt, 0, sizeof(*prt));
	(void)parse_uuid(j, len, knl_oj_member(j, len, acc, "owner"), &prt->owner);
	(void)parse_uuid(j, len, knl_oj_member(j, len, acc, "group"), &prt->group);
	v = knl_oj_member(j, len, acc, "mode");
	if ( v >= 0 && knl_oj_str(j, len, v, s, sizeof(s)) >= 0 ) {
		prt->mode = parse_mode(s);
	}

	v = knl_oj_member(j, len, acc, "attr");
	pos = v;
	while ( v >= 0 && ( e = knl_oj_next(j, len, v, &pos) ) >= 0 ) {
		if ( knl_oj_str(j, len, e, s, sizeof(s)) < 0 ) continue;
		if ( s[0] == 'r' && s[1] == 'o' ) prt->attr |= OB_A_RONLY;	/* "ronly" */
		if ( s[0] == 'p' && s[1] == 'e' ) prt->attr |= OB_A_PERM;	/* "perm" */
	}

	v = knl_oj_member(j, len, acc, "acl");
	pos = v;
	while ( v >= 0 && prt->nacl < OB_ACL_MAX
	     && ( e = knl_oj_next(j, len, v, &pos) ) >= 0 ) {
		T_OBACE	*a = &prt->acl[prt->nacl];

		if ( j[e] != '{' ) continue;
		if ( parse_uuid(j, len, knl_oj_member(j, len, e, "user"), &a->who) ) {
			a->kind = OB_ACE_USER;
		} else if ( parse_uuid(j, len, knl_oj_member(j, len, e, "group"), &a->who) ) {
			a->kind = OB_ACE_GROUP;
		} else {
			continue;
		}
		a->ops = parse_ops(j, len, knl_oj_member(j, len, e, "ops"));
		prt->nacl++;
	}

	v = knl_oj_member(j, len, acc, "records");
	pos = v;
	while ( v >= 0 && prt->nrmask < OB_RMASK_MAX
	     && ( e = knl_oj_next(j, len, v, &pos) ) >= 0 ) {
		T_OBRMASK	*m = &prt->rmask[prt->nrmask];
		D		n;

		if ( j[e] != '{' || !knl_oj_num(j, len, knl_oj_member(j, len, e, "n"), &n) ) {
			continue;
		}
		m->recno = (INT)n;
		m->ops = parse_ops(j, len, knl_oj_member(j, len, e, "ops"));
		prt->nrmask++;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- writing */

/* The operations as a JSON array of their names */
EXPORT INT knl_ob_ops_text( UB *out, INT pos, INT max, UINT ops )
{
	UINT	k;
	BOOL	first = TRUE;

	pos = knl_oj_put(out, pos, max, "[");
	for ( k = 0; k < 9; k++ ) {		/* the single operations only */
		if ( ( ops & op_names[k].ops ) == 0 ) continue;
		if ( !first ) pos = knl_oj_put(out, pos, max, ",");
		pos = knl_oj_put_str(out, pos, max, (CONST UB *)op_names[k].name);
		first = FALSE;
	}
	return knl_oj_put(out, pos, max, "]");
}

/* The protection written into the metadata; the new length, or -1 */
EXPORT INT knl_ob_prt_store( UB *json, INT len, INT max, CONST T_OBPRT *prt )
{
	UB	*t;
	INT	n = 0, i, root;
	char	mode[10];
	BOOL	first;

	t = (UB *)Kmalloc(OB_META_MAX);
	if ( t == NULL ) {
		return -1;
	}
	for ( i = 0; i < 9; i++ ) {
		mode[i] = ( ( prt->mode >> ( 8 - i ) ) & 1 ) ? "rwxrwxrwx"[i] : '-';
	}
	mode[9] = 0;

	n = knl_oj_put(t, n, OB_META_MAX, "{");
	if ( !knl_ob_uuid_zero(&prt->owner) ) {
		n = knl_oj_put(t, n, OB_META_MAX, "\"owner\":");
		n = knl_oj_put_uuid(t, n, OB_META_MAX, &prt->owner);
		n = knl_oj_put(t, n, OB_META_MAX, ",");
	}
	if ( !knl_ob_uuid_zero(&prt->group) ) {
		n = knl_oj_put(t, n, OB_META_MAX, "\"group\":");
		n = knl_oj_put_uuid(t, n, OB_META_MAX, &prt->group);
		n = knl_oj_put(t, n, OB_META_MAX, ",");
	}
	n = knl_oj_put(t, n, OB_META_MAX, "\"mode\":");
	n = knl_oj_put_str(t, n, OB_META_MAX, (CONST UB *)mode);
	n = knl_oj_put(t, n, OB_META_MAX, ",\"attr\":[");
	first = TRUE;
	if ( ( prt->attr & OB_A_RONLY ) != 0 ) {
		n = knl_oj_put(t, n, OB_META_MAX, "\"ronly\"");
		first = FALSE;
	}
	if ( ( prt->attr & OB_A_PERM ) != 0 ) {
		n = knl_oj_put(t, n, OB_META_MAX, first ? "\"perm\"" : ",\"perm\"");
	}
	n = knl_oj_put(t, n, OB_META_MAX, "],\"acl\":[");
	for ( i = 0; i < prt->nacl && i < OB_ACL_MAX; i++ ) {
		if ( i > 0 ) n = knl_oj_put(t, n, OB_META_MAX, ",");
		n = knl_oj_put(t, n, OB_META_MAX, ( prt->acl[i].kind == OB_ACE_USER )
			       ? "{\"user\":" : "{\"group\":");
		n = knl_oj_put_uuid(t, n, OB_META_MAX, &prt->acl[i].who);
		n = knl_oj_put(t, n, OB_META_MAX, ",\"ops\":");
		n = knl_ob_ops_text(t, n, OB_META_MAX, prt->acl[i].ops);
		n = knl_oj_put(t, n, OB_META_MAX, "}");
	}
	n = knl_oj_put(t, n, OB_META_MAX, "],\"records\":[");
	for ( i = 0; i < prt->nrmask && i < OB_RMASK_MAX; i++ ) {
		if ( i > 0 ) n = knl_oj_put(t, n, OB_META_MAX, ",");
		n = knl_oj_put(t, n, OB_META_MAX, "{\"n\":");
		n = knl_oj_put_num(t, n, OB_META_MAX, prt->rmask[i].recno);
		n = knl_oj_put(t, n, OB_META_MAX, ",\"ops\":");
		n = knl_ob_ops_text(t, n, OB_META_MAX, prt->rmask[i].ops);
		n = knl_oj_put(t, n, OB_META_MAX, "}");
	}
	n = knl_oj_put(t, n, OB_META_MAX, "]}");
	if ( n < 0 ) {
		Kfree(t);
		return -1;
	}

	len = knl_oj_set_path(json, len, max, "tessronos", "access", t, n);

	/* the flags a TADjs Desktop reads, kept in step with the attributes */
	root = knl_oj_root(json, len);
	if ( len >= 0 && root >= 0 ) {
		len = knl_oj_set(json, len, max, root, "editable",
				 (CONST UB *)( ( prt->attr & OB_A_RONLY ) ? "false" : "true" ),
				 ( prt->attr & OB_A_RONLY ) ? 5 : 4);
	}
	root = ( len >= 0 ) ? knl_oj_root(json, len) : -1;
	if ( len >= 0 && root >= 0 ) {
		len = knl_oj_set(json, len, max, root, "deletable",
				 (CONST UB *)( ( prt->attr & OB_A_PERM ) ? "false" : "true" ),
				 ( prt->attr & OB_A_PERM ) ? 5 : 4);
	}
	Kfree(t);

	return len;
}
