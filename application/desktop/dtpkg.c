/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtpkg.c
 *	Applications installed from a package, updated and deleted
 *	(design 18.12)
 *
 *	A package (.tpk, made by tools/mktpk.py) holds the program object
 *	and the templates it brings, each as the files of the TADjs form
 *	({uuid}.json, {uuid}_N.xtad, {uuid}_N.bin, {uuid}.ico), and
 *	"package.json", which names the program, its id and its version.
 *	Taken in from outside (ファイル変換), a package is an object whose
 *	record 1 is the file; that object is what is installed.
 *
 *	Installing puts each object of the package on the system under its
 *	own UUID -- made when the system does not have it, and its metadata
 *	and records written anew when it does -- and links the program from
 *	the program box. A program the system already has by the same id is
 *	updated only by a newer version, unless the caller says to go ahead
 *	all the same (the person was asked); one that came under another
 *	UUID is taken out of the box. A version is written R1.000: R, a
 *	number of any length, a point and three digits, and compares as the
 *	number times a thousand and the digits.
 *
 *	Deleting takes a program out of the box and deletes its object and
 *	its templates. The programs the desktop carries in itself (builtin)
 *	are not deleted.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/json.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "desktop.h"

#define PKG_HEAD	16
#define PKG_ENTRY	64
#define PKG_NAME	48
#define PKG_MAX_OBJ	(1 + DT_PROG_BASE)
#define PKG_FORMAT	1

typedef struct {
	CONST UB	*name;		/* NUL filled to PKG_NAME */
	CONST UB	*data;
	UW		size;
	UW		rt;
} PKGENT;

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL UH le16( CONST UB *p )
{
	return (UH)( p[0] | ( p[1] << 8 ) );
}

EXPORT D dt_ver_num( CONST UB *v )
{
	D	n = 0;
	INT	i = 1, k;

	if ( v == NULL || v[0] != 'R' || v[1] < '0' || v[1] > '9' ) {
		return -1;
	}
	for ( ; v[i] >= '0' && v[i] <= '9'; i++ ) {
		if ( n > 1000000000000LL ) return -1;
		n = n * 10 + ( v[i] - '0' );
	}
	if ( v[i] != '.' ) {
		return -1;
	}
	n *= 1000;
	for ( k = 0; k < 3; k++ ) {
		UB	c = v[i + 1 + k];

		if ( c < '0' || c > '9' ) return -1;
		n += ( c - '0' ) * ( k == 0 ? 100 : k == 1 ? 10 : 1 );
	}
	return ( v[i + 4] == 0 ) ? n : -1;
}

/* ---------------------------------------------------------------- reading a package */

/* The entries of a package, checked to lie within it: how many, or an error */
LOCAL INT pkg_entries( CONST UB *raw, SZ size, PKGENT *e, INT max )
{
	INT	n, i;

	if ( size < PKG_HEAD || raw[0] != 'T' || raw[1] != 'S' || raw[2] != 'P' || raw[3] != 'K'
	  || le16(raw + 4) != PKG_FORMAT ) {
		return E_OBJ;
	}
	n = le16(raw + 6);
	if ( n <= 0 || n > max || (SZ)PKG_HEAD + (SZ)n * PKG_ENTRY > size ) {
		return E_OBJ;
	}
	for ( i = 0; i < n; i++ ) {
		CONST UB	*d = raw + PKG_HEAD + i * PKG_ENTRY;
		UW		off = le32(d + PKG_NAME), sz = le32(d + PKG_NAME + 4);

		if ( d[PKG_NAME - 1] != 0 || (SZ)off > size || (SZ)sz > size - (SZ)off ) {
			return E_OBJ;
		}
		e[i].name = d;
		e[i].data = raw + off;
		e[i].size = sz;
		e[i].rt = le32(d + PKG_NAME + 8);
	}
	return n;
}

LOCAL BOOL name_is( CONST UB *name, CONST char *s )
{
	INT	i;

	for ( i = 0; s[i] != 0; i++ ) {
		if ( name[i] != (UB)s[i] ) return FALSE;
	}
	return (BOOL)( name[i] == 0 );
}

/*
 * What an entry's name says: the object (its UUID) and which of its
 * files it is -- the metadata (-1), the icon (-2), or record N.
 */
#define F_META	(-1)
#define F_ICON	(-2)
LOCAL BOOL entry_of( CONST UB *name, TS_UUID *u, INT *p_what )
{
	char	us[TS_UUID_STRLEN + 1];
	CONST UB *t = name + TS_UUID_STRLEN;
	INT	i, n = 0;

	for ( i = 0; i < TS_UUID_STRLEN; i++ ) {
		if ( name[i] == 0 ) return FALSE;
		us[i] = (char)name[i];
	}
	us[i] = 0;
	if ( ts_str_to_uuid(us, u) < E_OK ) {
		return FALSE;
	}
	if ( name_is(t, ".json") ) {
		*p_what = F_META;
		return TRUE;
	}
	if ( name_is(t, ".ico") ) {
		*p_what = F_ICON;
		return TRUE;
	}
	if ( t[0] != '_' || t[1] < '0' || t[1] > '9' ) {
		return FALSE;
	}
	for ( t++; *t >= '0' && *t <= '9'; t++ ) {
		n = n * 10 + ( *t - '0' );
		if ( n > 255 ) return FALSE;
	}
	if ( !name_is(t, ".xtad") && !name_is(t, ".bin") ) {
		return FALSE;
	}
	*p_what = n;
	return TRUE;
}

/* ---------------------------------------------------------------- one object put on */

LOCAL ER put_object( CONST TS_UUID *u, CONST PKGENT *e, INT n, CONST T_OBPRT *prt )
{
	CONST PKGENT	*meta = NULL, *icon = NULL;
	TS_UUID		fu, got;
	T_OBREF		r;
	T_OBCRE		c;
	INT		i, what;
	ER		er = E_OK;

	for ( i = 0; i < n; i++ ) {
		if ( entry_of(e[i].name, &fu, &what) && ts_uuid_cmp(&fu, u) == 0 ) {
			if ( what == F_META ) meta = &e[i];
			if ( what == F_ICON ) icon = &e[i];
		}
	}
	if ( meta == NULL ) {
		return E_OBJ;
	}
	if ( ob_ref_obj(u, &r) >= E_OK ) {
		/* one the system has: its metadata and records made the package's */
		er = om_obj_meta_put(u, meta->data, (SZ)meta->size);
		if ( er >= E_OK && icon != NULL ) {
			er = om_obj_icon_put(u, icon->data, (SZ)icon->size);
		}
	} else {
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = meta->data;
		c.jsonsz = (SZ)meta->size;
		c.prt = prt;
		c.uuid = *u;
		c.icon = ( icon != NULL ) ? icon->data : NULL;
		c.iconsz = ( icon != NULL ) ? (SZ)icon->size : 0;
		er = ob_cre_obj(&c, &got);
	}
	for ( i = 0; er >= E_OK && i < n; i++ ) {
		if ( entry_of(e[i].name, &fu, &what) && what >= 0 && ts_uuid_cmp(&fu, u) == 0 ) {
			er = om_obj_record_put(u, what, e[i].rt, 0, e[i].data, (SZ)e[i].size);
		}
	}
	return er;
}

/* ---------------------------------------------------------------- installing */

EXPORT ER dt_pkg_install( CONST TS_UUID *pkg, BOOL force, TS_UUID *p_prog, UINT *p_how )
{
	PKGENT		*e = NULL;
	UB		*raw, id[DT_PROG_ID], ver[DT_VER_MAX], us[TS_UUID_STRLEN + 1];
	SZ		size = 0;
	INT		n, i, k, nobj = 0;
	TS_UUID		prog, obj[PKG_MAX_OBJ], box;
	T_JSON		root, list, it;
	T_OBPRT		prt;
	CONST DTPROG	*have;
	TS_UUID		old;
	BOOL		had = FALSE, moved = FALSE;
	ER		er;

	if ( pkg == NULL ) {
		return E_PAR;
	}
	raw = om_obj_record(pkg, 1, &size);
	if ( raw == NULL ) {
		return E_OBJ;
	}
	e = (PKGENT *)Kmalloc(sizeof(PKGENT) * 64);
	n = ( e != NULL ) ? pkg_entries(raw, size, e, 64) : E_NOMEM;
	er = ( n > 0 ) ? E_OBJ : (ER)n;

	/* package.json: the program, its id and version, and the objects */
	for ( i = 0; n > 0 && i < n; i++ ) {
		if ( !name_is(e[i].name, "package.json") ) continue;
		if ( js_parse(e[i].data, (INT)e[i].size, &root) < E_OK
		  || js_get_str(&root, "id", id, sizeof(id)) <= 0
		  || js_get_str(&root, "version", ver, sizeof(ver)) <= 0 || dt_ver_num(ver) < 0
		  || js_get_str(&root, "program", us, sizeof(us)) != TS_UUID_STRLEN
		  || ts_str_to_uuid((CONST char *)us, &prog) < E_OK ) {
			break;
		}
		obj[nobj++] = prog;
		if ( js_get(&root, "objects", &list) >= E_OK ) {
			for ( it.s = NULL; nobj < PKG_MAX_OBJ && js_next(&list, &it); ) {
				TS_UUID	u;

				if ( js_str(&it, us, sizeof(us)) != TS_UUID_STRLEN
				  || ts_str_to_uuid((CONST char *)us, &u) < E_OK ) continue;
				for ( k = 0; k < nobj && ts_uuid_cmp(&obj[k], &u) != 0; k++ ) ;
				if ( k == nobj ) obj[nobj++] = u;
			}
		}
		er = E_OK;
		break;
	}
	if ( er < E_OK ) {
		goto done;
	}

	/* the same program: only a newer version, unless asked to all the same */
	have = dt_prog_find(id);
	if ( have != NULL ) {
		had = TRUE;
		old = have->uuid;
		if ( have->builtin ) {
			er = E_PAR;		/* the desktop's own is not replaced */
			goto done;
		}
		if ( !force && dt_ver_num(ver) <= dt_ver_num(have->version) ) {
			er = E_LIMIT;
			goto done;
		}
		moved = (BOOL)( ts_uuid_cmp(&old, &prog) != 0 );
	}

	dt_sys_prt(&prt);
	prt.attr = 0;				/* what is installed can be deleted */
	for ( k = 0; er >= E_OK && k < nobj; k++ ) {
		er = put_object(&obj[k], e, n, &prt);
	}
	if ( er >= E_OK && ts_str_to_uuid(SYSDEF_PROG_BOX, &box) >= E_OK ) {
		if ( moved ) {
			(void)om_obj_link_del(&box, &old);
		}
		if ( !had || moved ) {
			er = om_obj_link_add(&box, &prog);
		}
	}
	dt_prog_reread();
	if ( er >= E_OK ) {
		if ( p_prog != NULL ) *p_prog = prog;
		if ( p_how != NULL ) *p_how = had ? DT_PKG_UPDATED : DT_PKG_NEW;
		tm_printf((UB *)"TessronOS desktop: %s %s %s\n", id, ver, had ? "updated" : "installed");
	}
done:
	if ( e != NULL ) Kfree(e);
	Kfree(raw);
	return er;
}

/* ---------------------------------------------------------------- deleting */

/* An object deleted, what kept it from deletion (削除不可) taken off first */
LOCAL ER del_object( CONST TS_UUID *u )
{
	T_OBPRT	prt;

	if ( ob_get_prt(u, &prt) >= E_OK && ( prt.attr & ( OB_A_PERM | OB_A_RONLY ) ) ) {
		prt.attr &= ~( OB_A_PERM | OB_A_RONLY );
		(void)ob_set_prt(u, &prt);
	}
	return ob_del_obj(u);
}

EXPORT ER dt_prog_remove( CONST TS_UUID *prog )
{
	CONST DTPROG	*p = dt_prog_of(prog);
	TS_UUID		box, base[DT_PROG_BASE];
	INT		nbase, i;
	ER		er;

	if ( p == NULL ) {
		return E_NOEXS;
	}
	if ( p->builtin ) {
		return E_PAR;
	}
	nbase = p->nbase;
	for ( i = 0; i < nbase; i++ ) {
		base[i] = p->base[i];
	}
	er = ( ts_str_to_uuid(SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_del(&box, prog) : E_SYS;
	if ( er >= E_OK ) {
		er = del_object(prog);
		for ( i = 0; i < nbase; i++ ) {
			(void)del_object(&base[i]);
		}
	}
	dt_prog_reread();
	return er;
}
