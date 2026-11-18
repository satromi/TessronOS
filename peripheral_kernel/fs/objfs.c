/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	objfs.c
 *	The world of real objects seen as a tree of files, at "/" (design
 *	5.8.1)
 *
 *	A POSIX program names files by paths; the objects are named by
 *	UUIDs and reached through the links of others. Here the paths start
 *	at one object, "/" (SYSDEF_ROOT), and go from an object to the
 *	objects its links point at, by their names: the file's name an
 *	object taken in from a file keeps ("tessronos.file.name"), otherwise
 *	the object's own name. A second object of the same name is "name~2"
 *	and so on; a '/' in a name is shown as U+FF0F. A name longer than
 *	NAME_MAX (FS_NAME_MAX - 1 bytes) is shortened at a whole letter and
 *	numbered "~1", "~2" ... the same way, so that every name shown is
 *	one a program can hold and give back: opening that name reaches the
 *	object, whose own name is never changed for it. Nothing is shown that
 *	is not an object: "/" links to bin (the commands), dev (the device
 *	objects, kept in step with the devices by devdir_task), home (the
 *	cabinet the desktop opens with) and tmp (emptied at each start, by the
 *	object layer: the list of 機器の設定).
 *
 *	An object is a directory when one of its applications is 仮身一覧,
 *	the キャビネット or the 小物箱, as the objects of the tree are. A
 *	device object is a device of characters: those of devfs.c (null,
 *	tty, ptmx, ptsN ...) open as such, the others not yet.
 *
 *	The bytes of a file are one record of its object: record 1 of an
 *	object of the file's form (include/ts/xf.h), read and written, and
 *	the executable record of a program, read. A document or a figure is
 *	listed but not opened yet (doc_open says EX_ACCES), so that a document
 *	of the desktop is never written over by a program that takes it for
 *	text. Making a file makes an object of the file's form on the volume
 *	of the directory and links it there; making a directory makes a plain
 *	object of links that opens in 仮身一覧. Removing a name takes the link out of the box: the
 *	object becomes garbage once nothing links to it (TSFS counts them).
 *	Renaming in the same box renames the object; into another box moves
 *	the link. A second name (link) is a second link to the object.
 *
 *	Every call is made with the caller's credentials -- the object layer
 *	judges them -- and no key is kept between calls: an open file may be
 *	shared by several processes, and a key belongs to the process that
 *	opened it. What a box holds is kept as long as neither the box nor
 *	any object it lists has changed (knl_ob_chg_serial, compared each
 *	time it is used) and no volume came or went (knl_obfile_serial), or
 *	until a call
 *	here changes it, so that listing and then looking at each name does
 *	not read every object's metadata again; the box's read right is
 *	asked each time it is used.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/xf.h>
#include <ts/dt.h>
#include <ts/time.h>
#include <ts/json.h>
#include <ts/sysdef.h>
#include <ts/psx.h>
#include <ts/psxid.h>
#include <ts/tsfs.h>
#include "../obj/obj.h"

#define OBJ_SHOWN	( FS_NAME_MAX - 1 )	/* bytes of a name shown at most: NAME_MAX */
#define OBJ_NUM_ROOM	12		/* "~" and the digits of a number, kept free in a name shortened */
#define OBJ_BOXES	16		/* boxes whose names are kept */
#define OBJ_KEEP_MS	2000

/* What an object is, as a file */
#define K_DIR		1		/* a box */
#define K_FILE		2		/* the file's form: record 1, read and written */
#define K_PROG		3		/* a program: its executable record */
#define K_DOC		4		/* anything else: not read as a file yet (doc_open) */
#define K_DEV		5		/* a device object: a device of characters */

/* A node a path reaches: always an object */
#define N_OBJ		4

typedef struct {
	UB	*name;			/* the name shown, at most OBJ_SHOWN bytes */
	UB	*oname;			/* the object's own name, whole, also found by; NULL without */
	TS_UUID	u;
	UB	kind;
	BOOL	cut;			/* name shortened from a longer one: it is numbered */
	UW	ser;			/* knl_ob_chg_serial of the object before it was read */
	INT	rec;
	TS_TIME	mtime;
} OENT;

/*
 * What a box holds, as one reading made it: shared by the cache and by
 * the calls looking at it, and freed when the last lets it go
 * (box_put), so that nothing is copied for each name looked up.
 */
typedef struct {
	INT	refs;			/* under ob_mtxid */
	INT	n;
	OENT	*e;
} OLIST;

typedef struct {
	BOOL	used;
	TS_UUID	box;
	UW	ser;			/* knl_ob_chg_serial of the box before it was read */
	UW	vser;			/* knl_obfile_serial then */
	UW	use;
	OLIST	*l;
} OBOX;

typedef struct {
	UINT	node;			/* N_* */
	TS_UUID	u;
	BOOL	hasparent;
	TS_UUID	parent;
	UB	kind;
	INT	rec;
	TS_TIME	mtime;
	UB	name[FS_NAME_MAX];
} ONODE;

/* What an open file of objfs holds (T_FILE.obj) */
typedef struct {
	ONODE	n;
	BOOL	wrote;
} OFILE;

LOCAL OBOX	ob_cache[OBJ_BOXES];
LOCAL UW	ob_use = 0;
LOCAL ID	ob_mtxid = 0;
LOCAL TS_TIME	ob_boot = 0;

/* ---------------------------------------------------------------- helpers */

LOCAL INT s_len( CONST UB *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL void s_cpy( UB *d, CONST UB *s, INT max )
{
	INT	i;

	for ( i = 0; i < max - 1 && s[i] != 0; i++ ) d[i] = s[i];
	d[i] = 0;
}

LOCAL BOOL s_same( CONST UB *a, CONST UB *b )
{
	while ( *a != 0 && *a == *b ) { a++; b++; }
	return ( *a == *b );
}

LOCAL UD now_ms( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000ULL;
}

LOCAL ER fx( ER er )
{
	switch ( er ) {
	  case E_NOEXS:	return EX_NOENT;
	  case E_OACV:	return EX_ACCES;
	  case E_NOMEM:	return EX_NOMEM;
	  case E_LIMIT:	return EX_NOSPC;
	  case E_PAR:	return EX_INVAL;
	  case E_OBJ:	return EX_PERM;
	  case E_NOSPT:	return EX_NOTSUP;
	  case E_RONLY:	return EX_ROFS;
	  case E_BUSY:	return EX_BUSY;
	  case E_DISWAI:
	  case E_RLWAI:	return EX_INTR;
	  default:	return ( er < 0 ) ? EX_IO : er;
	}
}

/* "2026-09-26T00:00:00Z" as seconds since 1985-01-01 UTC; 0 when it is not one */
LOCAL TS_TIME iso_time( CONST UB *s )
{
	INT	v[6], i, k = 0;
	D	y, m, d, era, yoe, doy, doe, days;

	for ( i = 0; i < 6; i++ ) {
		INT	n = 0, dig = 0;

		while ( s[k] >= '0' && s[k] <= '9' ) {
			n = n * 10 + ( s[k++] - '0' );
			dig++;
		}
		if ( dig == 0 ) return 0;
		v[i] = n;
		if ( i < 5 ) {
			if ( s[k] == 0 ) return 0;
			k++;
		}
	}
	y = v[0];
	m = v[1];
	d = v[2];
	if ( m <= 2 ) y--;
	era = ( y >= 0 ? y : y - 399 ) / 400;
	yoe = y - era * 400;
	doy = ( 153 * ( m + ( m > 2 ? -3 : 9 ) ) + 2 ) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	days = era * 146097 + doe - 719468;		/* since 1970-01-01 */
	return (TS_TIME)( days * 86400 + v[3] * 3600 + v[4] * 60 + v[5] ) - 473385600LL;
}

LOCAL UD ino_of( CONST TS_UUID *u )
{
	UD	h = 1469598103934665603ULL;
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		h = ( h ^ u->b[i] ) * 1099511628211ULL;
	}
	return h & 0x7FFFFFFFFFFFFFFFULL;
}

LOCAL BOOL uuid_of( CONST char *s, TS_UUID *u )
{
	return ( ts_str_to_uuid(s, u) >= E_OK );
}

/* Whether the caller may do ops to the object */
LOCAL BOOL may( CONST TS_UUID *u, UINT ops )
{
	return ( ( knl_ob_permit_on(u, knl_ob_crd_of(ts_get_pid())) & ops ) == ops );
}

/* ---------------------------------------------------------------- what an object is */

/* The name of the member whose value v is, in an object that starts at base */
LOCAL INT key_before( CONST UB *base, CONST T_JSON *v, CONST UB **p_key )
{
	CONST UB *p = v->s - 1, *q;

	*p_key = NULL;
	while ( p > base && ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) ) p--;
	if ( p <= base || *p != ':' ) return 0;
	p--;
	while ( p > base && ( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ) ) p--;
	if ( p <= base || *p != '"' ) return 0;
	for ( q = p - 1; q > base && *q != '"'; q-- ) ;
	if ( *q != '"' ) return 0;
	*p_key = q + 1;
	return (INT)( p - ( q + 1 ) );
}

/* Whether the n bytes at k are the whole of the 0 ended s */
LOCAL BOOL key_is_n( CONST UB *k, INT n, CONST char *s )
{
	INT	i;

	for ( i = 0; i < n && s[i] != 0 && k[i] == (UB)s[i]; i++ ) ;
	return (BOOL)( i == n && s[i] == 0 );
}

/* The names an entry holds let go */
LOCAL void ent_clear( OENT *e )
{
	if ( e->name != NULL ) Kfree(e->name);
	if ( e->oname != NULL ) Kfree(e->oname);
	e->name = NULL;
	e->oname = NULL;
}

LOCAL void ents_free( OENT *e, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) ent_clear(&e[i]);
	Kfree(e);
}

/*
 * The name shown for s, in memory of its own (e->name): a '/' as U+FF0F,
 * none, "." or ".." as "(名前なし)", and one longer than OBJ_SHOWN bytes
 * shortened at a whole letter with room left for its number (e->cut).
 */
LOCAL ER name_show( OENT *e, CONST UB *s )
{
	CONST UB *none = (CONST UB *)"(名前なし)";
	UB	*t;
	INT	i, n = 0, k = 0;

	for ( i = 0; s != NULL && s[i] != 0; i++ ) n += ( s[i] == '/' ) ? 3 : 1;
	n += s_len(none) + 1;
	t = (UB *)Kmalloc((SZ)n);
	if ( t == NULL ) {
		return EX_NOMEM;
	}
	for ( i = 0; s != NULL && s[i] != 0; i++ ) {
		if ( s[i] == '/' ) {
			t[k++] = 0xEF; t[k++] = 0xBC; t[k++] = 0x8F;
		} else {
			t[k++] = s[i];
		}
	}
	t[k] = 0;
	if ( k == 0 || ( k == 1 && t[0] == '.' ) || ( k == 2 && t[0] == '.' && t[1] == '.' ) ) {
		s_cpy(t, none, n);
		k = s_len(t);
	}
	e->cut = FALSE;
	if ( k > OBJ_SHOWN ) {
		k = (INT)knl_utf8_fit(t, k, OBJ_SHOWN - OBJ_NUM_ROOM);
		t[k] = 0;
		e->cut = TRUE;
	}
	e->name = t;
	return EX_OK;
}

/*
 * base and "~v", in memory of its own: base cut at a whole letter as far
 * as the whole must be at most OBJ_SHOWN bytes. NULL when it cannot be had.
 */
LOCAL UB *name_num( CONST UB *base, INT v )
{
	UB	num[12], *d;
	INT	nn = 0, bl = s_len(base);

	do {
		num[nn++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && nn < (INT)sizeof(num) );
	bl = (INT)knl_utf8_fit(base, bl, OBJ_SHOWN - 1 - nn);
	d = (UB *)Kmalloc((SZ)( bl + nn + 2 ));
	if ( d == NULL ) {
		return NULL;
	}
	knl_memcpy(d, base, bl);
	d[bl++] = '~';
	while ( nn > 0 ) d[bl++] = num[--nn];
	d[bl] = 0;
	return d;
}

/* The other way: U+FF0F typed back to '/' as it is in the object's name */
LOCAL void name_raw( CONST UB *s, UB *d, INT max )
{
	INT	i, k = 0;

	for ( i = 0; s[i] != 0 && k < max - 1; i++ ) {
		if ( s[i] == 0xEF && s[i + 1] == 0xBC && s[i + 2] == 0x8F ) {
			d[k++] = '/';
			i += 2;
		} else {
			d[k++] = s[i];
		}
	}
	d[k] = 0;
}

/*
 * The name, the kind and the time of an object, from its metadata. An
 * object whose metadata the caller may not read is a document named by
 * its object name. EX_NOMEM when the names cannot be held (what *e holds
 * then is let go by ent_clear).
 */
LOCAL ER ent_of( CONST TS_UUID *u, OENT *e )
{
	T_JSON	root, ts, f, ex, apps, it, fn;
	T_OBREF	r;
	UB	*meta, *fname = NULL, upd[40];	/* an ISO 8601 date: its form sets the length */
	SZ	size = 0;
	BOOL	has_file = FALSE, has_exec = FALSE, box_app = FALSE;
	ER	er = EX_OK, oer = E_OK;

	knl_memset(e, 0, sizeof(*e));
	e->u = *u;
	e->kind = K_DOC;
	e->rec = 0;
	if ( ob_ref_obj(u, &r) >= E_OK ) {
		/* the whole name: T_OBREF.name holds only its first bytes */
		e->oname = knl_ob_name(u, NULL, &oer);
		if ( e->oname == NULL && oer == E_NOMEM ) {
			return EX_NOMEM;
		}
		e->rec = ( r.nrec >= 2 ) ? 1 : 0;
		if ( r.type == OB_T_DEVICE ) {
			e->kind = K_DEV;
			e->rec = 1;
			return name_show(e, e->oname);
		}
		if ( r.type != OB_T_STORAGE ) {
			return name_show(e, e->oname);	/* a process, a window, a channel: listed, not opened */
		}
	}
	meta = om_obj_meta(u, &size);
	if ( meta != NULL && js_parse(meta, (INT)size, &root) >= E_OK ) {
		if ( js_get_str(&root, "updateDate", upd, sizeof(upd)) > 0 ) {
			e->mtime = iso_time(upd);
		}
		if ( js_get(&root, "tessronos", &ts) >= E_OK ) {
			if ( js_get(&ts, "file", &f) >= E_OK ) {
				has_file = TRUE;
				/* the file's name, whole: unescaped it is never longer than its text */
				if ( js_get(&f, "name", &fn) >= E_OK && js_type(&fn) == JS_STRING ) {
					fname = (UB *)Kmalloc((SZ)fn.len + 1);
					if ( fname == NULL ) {
						er = EX_NOMEM;
					} else if ( js_str(&fn, fname, fn.len + 1) <= 0 ) {
						Kfree(fname);
						fname = NULL;
					}
				}
			}
			if ( js_get(&ts, "exec", &ex) >= E_OK ) {
				has_exec = TRUE;
				e->rec = (INT)js_get_num(&ex, "record", 1);
			}
		}
		/*
		 * A directory: an object that opens in 仮身一覧, the キャビネット
		 * or the 小物箱 -- one of its applications, not only the one it
		 * opens in by default -- a figure of links, as the objects of the
		 * tree are.
		 */
		box_app = FALSE;
		if ( js_get(&root, "applist", &apps) >= E_OK && js_type(&apps) == JS_OBJECT ) {
			CONST UB	*key;
			INT		kn;

			/* each program's identity compared whole, as long as it is */
			it.s = NULL;
			while ( js_next(&apps, &it) ) {
				kn = key_before(apps.s, &it, &key);
				if ( kn <= 0 ) continue;
				if ( key_is_n(key, kn, "virtual-object-list")
				  || key_is_n(key, kn, "base-file-manager")
				  || key_is_n(key, kn, "accessory-box") ) {
					box_app = TRUE;
				}
			}
		}
		if ( box_app && !has_exec && !has_file ) {
			e->kind = K_DIR;
		} else if ( has_exec ) {
			e->kind = K_PROG;
		} else if ( has_file ) {
			e->kind = K_FILE;
			e->rec = 1;
		}
	}
	if ( meta != NULL ) Kfree(meta);
	if ( er >= EX_OK ) {
		er = name_show(e, ( fname != NULL ) ? fname : e->oname);
	}
	if ( fname != NULL ) Kfree(fname);
	return er;
}

/* ---------------------------------------------------------------- boxes */

/* One holder of a list less, under ob_mtxid; the last frees it */
LOCAL void list_drop( OLIST *l )
{
	if ( l != NULL && --l->refs <= 0 ) {
		ents_free(l->e, l->n);
		Kfree(l);
	}
}

/* A list box_get gave let go */
LOCAL void box_put( OLIST *l )
{
	tk_loc_mtx(ob_mtxid, TMO_FEVR);
	list_drop(l);
	tk_unl_mtx(ob_mtxid);
}

LOCAL void box_forget( CONST TS_UUID *box )
{
	INT	i;

	tk_loc_mtx(ob_mtxid, TMO_FEVR);
	for ( i = 0; i < OBJ_BOXES; i++ ) {
		if ( ob_cache[i].used && ( box == NULL || ts_uuid_cmp(&ob_cache[i].box, box) == 0 ) ) {
			list_drop(ob_cache[i].l);
			ob_cache[i].l = NULL;
			ob_cache[i].used = FALSE;
		}
	}
	tk_unl_mtx(ob_mtxid);
}

/*
 * The name the k-th entry is shown by, made one no entry before it has:
 * a name shortened is numbered from "~1", a name met again from "~2".
 */
LOCAL ER name_unique( OENT *e, INT k )
{
	UB	*base = e[k].name, *nm;
	INT	j, v = e[k].cut ? 1 : 0;
	ER	er = EX_OK;

	for ( ;; ) {
		if ( v > 0 ) {
			nm = name_num(base, v);
			if ( nm == NULL ) {
				er = EX_NOMEM;
				break;
			}
			if ( e[k].name != base ) Kfree(e[k].name);
			e[k].name = nm;
		}
		for ( j = 0; j < k && !s_same(e[j].name, e[k].name); j++ ) ;
		if ( j == k ) break;
		v = ( v == 0 ) ? 2 : v + 1;
	}
	if ( e[k].name != base ) Kfree(base);
	return er;
}

/* Made from the box: every link's object, named; NULL with *p_er on failure */
LOCAL OENT *box_read( CONST TS_UUID *box, INT *p_n, ER *p_er )
{
	TS_UUID	*ids;
	OENT	*e;
	INT	cnt = 0, i, j, k;
	UW	s;
	ER	er;

	*p_n = 0;
	er = ob_lst_lnk(box, NULL, NULL, 0, &cnt);
	if ( er < E_OK ) {
		*p_er = fx(er);
		return NULL;
	}
	/* every link, however many: read again with more room if the box grew meanwhile */
	for ( ;; ) {
		ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( cnt + 1 ));
		if ( ids == NULL ) {
			*p_er = EX_NOMEM;
			return NULL;
		}
		i = 0;
		if ( cnt > 0 ) {
			er = ob_lst_lnk(box, NULL, ids, cnt, &i);
			if ( er < E_OK ) {
				Kfree(ids);
				*p_er = fx(er);
				return NULL;
			}
		}
		if ( i <= cnt ) {
			cnt = i;
			break;
		}
		Kfree(ids);
		cnt = i;
	}
	e = (OENT *)Kmalloc(sizeof(OENT) * (SZ)( cnt + 1 ));
	if ( e == NULL ) {
		Kfree(ids);
		*p_er = EX_NOMEM;
		return NULL;
	}
	k = 0;
	er = EX_OK;
	for ( i = 0; i < cnt; i++ ) {
		/* the same object linked twice is one name */
		for ( j = 0; j < k && ts_uuid_cmp(&e[j].u, &ids[i]) != 0; j++ ) ;
		if ( j < k ) continue;
		s = knl_ob_chg_serial(&ids[i]);		/* before: a change while it is read shows */
		er = ent_of(&ids[i], &e[k]);
		e[k].ser = s;
		if ( er >= EX_OK ) {
			er = name_unique(e, k);
		}
		if ( er < EX_OK ) {
			ent_clear(&e[k]);
			break;
		}
		k++;
	}
	Kfree(ids);
	if ( er < EX_OK ) {
		ents_free(e, k);
		*p_er = er;
		return NULL;
	}
	*p_n = k;
	*p_er = EX_OK;
	return e;
}

/* Whether none of the objects a list was read from has changed since (under ob_mtxid) */
LOCAL BOOL list_same( CONST OLIST *l )
{
	INT	i;

	for ( i = 0; i < l->n; i++ ) {
		if ( knl_ob_chg_serial(&l->e[i].u) != l->e[i].ser ) return FALSE;
	}
	return TRUE;
}

/*
 * What a box holds, kept or read now, for the caller to look at until
 * it lets it go (box_put): NULL with *p_er when it cannot be had or the
 * caller may not read the box. What is kept is used only while the box,
 * every object it lists and the volumes attached are as they were when
 * it was read: a link added or taken out by anyone (the desktop, the
 * object layer's own calls), or an object of it renamed, is seen at the
 * next look. Only the serials are compared, no object is read for it.
 */
LOCAL OLIST *box_get( CONST TS_UUID *box, ER *p_er )
{
	OLIST	*l;
	OENT	*e;
	INT	i, n = 0, slot = -1;
	UW	bs, vs;

	if ( !may(box, OB_OP_READ) ) {
		*p_er = EX_ACCES;
		return NULL;
	}
	/* before reading: a change while the box is read makes the next look read it again */
	bs = knl_ob_chg_serial(box);
	vs = knl_obfile_serial();
	tk_loc_mtx(ob_mtxid, TMO_FEVR);
	for ( i = 0; i < OBJ_BOXES; i++ ) {
		OBOX	*b = &ob_cache[i];

		if ( b->used && ts_uuid_cmp(&b->box, box) == 0 && b->ser == bs && b->vser == vs
		  && list_same(b->l) ) {
			l = b->l;
			l->refs++;
			b->use = ++ob_use;
			tk_unl_mtx(ob_mtxid);
			*p_er = EX_OK;
			return l;
		}
	}
	tk_unl_mtx(ob_mtxid);

	e = box_read(box, &n, p_er);
	if ( e == NULL ) {
		return NULL;
	}
	l = (OLIST *)Kmalloc(sizeof(OLIST));
	if ( l == NULL ) {
		ents_free(e, n);
		*p_er = EX_NOMEM;
		return NULL;
	}
	l->refs = 2;			/* the cache's and the caller's */
	l->n = n;
	l->e = e;

	tk_loc_mtx(ob_mtxid, TMO_FEVR);
	for ( i = 0; i < OBJ_BOXES; i++ ) {
		if ( ob_cache[i].used && ts_uuid_cmp(&ob_cache[i].box, box) == 0 ) {
			slot = i;
			break;
		}
		if ( !ob_cache[i].used ) {
			if ( slot < 0 || ob_cache[slot].used ) slot = i;
		} else if ( slot < 0 || ( ob_cache[slot].used && ob_cache[i].use < ob_cache[slot].use ) ) {
			slot = i;
		}
	}
	if ( ob_cache[slot].used ) {
		list_drop(ob_cache[slot].l);
	}
	ob_cache[slot].used = TRUE;
	ob_cache[slot].box = *box;
	ob_cache[slot].ser = bs;
	ob_cache[slot].vser = vs;
	ob_cache[slot].use = ++ob_use;
	ob_cache[slot].l = l;
	tk_unl_mtx(ob_mtxid);

	*p_er = EX_OK;
	return l;
}

/* ---------------------------------------------------------------- paths */

/* The node of an object, as its box listed it */
LOCAL void node_obj( ONODE *nd, CONST OENT *e, CONST TS_UUID *parent )
{
	nd->node = N_OBJ;
	nd->u = e->u;
	nd->kind = e->kind;
	nd->rec = e->rec;
	nd->mtime = e->mtime;
	s_cpy(nd->name, ( e->name != NULL ) ? e->name : (CONST UB *)"", FS_NAME_MAX);
	nd->hasparent = ( parent != NULL );
	if ( parent != NULL ) nd->parent = *parent;
}

/* One name looked for in a box: the name shown, then the object's own name */
LOCAL ER find_in_box( CONST TS_UUID *box, CONST UB *name, INT len, ONODE *nd )
{
	OLIST	*l;
	UB	*want, *raw;
	INT	i;
	ER	er;

	if ( len >= FS_NAME_MAX ) {
		return EX_NAMETOOLONG;
	}
	want = (UB *)Kmalloc((SZ)len * 2 + 2);
	if ( want == NULL ) {
		return EX_NOMEM;
	}
	raw = want + len + 1;
	knl_memcpy(want, name, len);
	want[len] = 0;
	l = box_get(box, &er);
	if ( l == NULL ) {
		Kfree(want);
		return er;
	}
	for ( i = 0; i < l->n; i++ ) {
		if ( s_same(l->e[i].name, want) ) break;
	}
	if ( i == l->n ) {
		name_raw(want, raw, len + 1);
		for ( i = 0; i < l->n; i++ ) {
			if ( l->e[i].oname != NULL && l->e[i].oname[0] != 0 && s_same(l->e[i].oname, raw) ) break;
		}
	}
	if ( i < l->n ) {
		node_obj(nd, &l->e[i], box);
		er = EX_OK;
	} else {
		er = EX_NOENT;
	}
	box_put(l);
	Kfree(want);
	return er;
}

/*
 * The node of "/": the object システム links as the root of the paths
 * (SYSDEF_ROOT by default), looked for again no sooner than what a box
 * holds is read again (OBJ_KEEP_MS)
 */
LOCAL ER node_root( ONODE *nd )
{
	static TS_UUID	root;
	static UD	found = 0;
	static BOOL	known = FALSE;
	TS_UUID	u;
	T_OBREF	r;
	OENT	e;
	UD	now = now_ms();
	BOOL	fresh;
	ER	er;

	tk_loc_mtx(ob_mtxid, TMO_FEVR);
	u = root;
	fresh = (BOOL)( known && now - found < OBJ_KEEP_MS );
	tk_unl_mtx(ob_mtxid);
	if ( !fresh ) {
		if ( ob_fnd_sys(SYSROLE_ROOT, SYSDEF_ROOT, &u) < E_OK ) {
			return EX_NOENT;
		}
		tk_loc_mtx(ob_mtxid, TMO_FEVR);
		root = u;
		found = now;
		known = TRUE;
		tk_unl_mtx(ob_mtxid);
	}
	if ( ob_ref_obj(&u, &r) < E_OK ) {
		return EX_NOENT;
	}
	er = ent_of(&u, &e);
	if ( er >= EX_OK ) {
		e.kind = K_DIR;
		node_obj(nd, &e, NULL);
	}
	ent_clear(&e);
	return er;
}

/*
 * The node a path names. With p_last, the path's last name may be
 * missing: EX_NOENT then with *nd the directory it would be made in and
 * *p_last, *p_len that name. Without, EX_NOENT for any name not there.
 */
LOCAL ER walk( CONST char *path, ONODE *nd, CONST UB **p_last, INT *p_len )
{
	CONST UB *s = (CONST UB *)path;
	ONODE	cur, nx;
	INT	len;
	ER	er;

	if ( p_last != NULL ) *p_last = NULL;
	er = node_root(&cur);
	if ( er < EX_OK ) {
		return er;
	}
	for ( ;; ) {
		while ( *s == '/' ) s++;
		if ( *s == 0 ) {
			*nd = cur;
			return EX_OK;
		}
		for ( len = 0; s[len] != 0 && s[len] != '/'; len++ ) ;
		if ( cur.kind != K_DIR ) {
			return EX_NOTDIR;
		}
		er = find_in_box(&cur.u, s, len, &nx);
		if ( er == EX_NOENT && p_last != NULL ) {
			CONST UB *t = s + len;

			while ( *t == '/' ) t++;
			if ( *t == 0 ) {
				*nd = cur;
				*p_last = s;
				*p_len = len;
			}
			return EX_NOENT;
		}
		if ( er < EX_OK ) {
			return er;
		}
		cur = nx;
		s += len;
	}
}

/* ---------------------------------------------------------------- the fimp */

LOCAL ER of_mount( T_MOUNT *m )
{
	T_CMTX	c;

	(void)m;
	if ( ob_mtxid <= 0 ) {
		c.exinf = NULL;
		c.mtxatr = TA_TFIFO;
		ob_mtxid = tk_cre_mtx(&c);
		if ( ob_mtxid <= 0 ) return EX_NOMEM;
	}
	(void)dt_gettime(&ob_boot);
	return EX_OK;
}

LOCAL ER of_unmount( T_MOUNT *m )
{
	(void)m;
	return EX_BUSY;
}

/* The size of a record of an object, 0 when it has none */
LOCAL UD rec_size( CONST TS_UUID *u, INT rec )
{
	T_OBREC	*r;
	INT	cnt = 0, i;
	UD	size = 0;
	ID	key;

	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key < E_OK ) return 0;
	r = (T_OBREC *)Kmalloc(sizeof(T_OBREC) * 256);
	if ( r != NULL && ob_lst_rec(key, r, 256, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 256; i++ ) {
			if ( r[i].recno == rec ) size = r[i].size;
		}
	}
	if ( r != NULL ) Kfree(r);
	ob_cls_obj(key);
	return size;
}

/*
 * A document, a figure, anything not of the file's form nor a program:
 * its contents are not given as a file yet (EX_ACCES). Turning one into
 * text or a file of its kind, and back, is for a later stage, here.
 */
LOCAL ER doc_open( CONST ONODE *nd, UINT oflags )
{
	(void)nd;
	(void)oflags;
	return EX_ACCES;
}

/*
 * A device object other than the devices of characters of devfs.c (the
 * sound, a disk, ...): not opened as a file yet; how each of them reads
 * and writes as a file comes with it, here.
 */
LOCAL ER dev_open( CONST ONODE *nd, UINT oflags )
{
	(void)nd;
	(void)oflags;
	return EX_ACCES;
}

/*
 * An open file of a device of characters is devfs.c's: its calls go
 * there, and T_FILE.dir_off (a FAT file's, not used here otherwise) says so.
 */
#define OF_TTY		1
#define IS_TTY(f)	( (f)->dir_off == OF_TTY )

LOCAL ER of_open( T_MOUNT *m, CONST char *path, UINT oflags, T_FILE *f )
{
	OFILE	*of;
	ONODE	nd;
	CONST UB *last;
	INT	len;
	ER	er;
	BOOL	wr = ( ( oflags & O_ACCMODE ) != O_RDONLY );

	(void)m;
	er = walk(path, &nd, ( oflags & O_CREAT ) ? &last : NULL, &len);
	if ( er == EX_NOENT && ( oflags & O_CREAT ) != 0 && last != NULL ) {
		UB	*name;
		TS_UUID	u;

		if ( nd.node != N_OBJ || nd.kind != K_DIR ) {
			return EX_ACCES;
		}
		if ( len >= FS_NAME_MAX ) return EX_NAMETOOLONG;
		if ( !may(&nd.u, OB_OP_WRITE) ) {
			return EX_ACCES;
		}
		name = (UB *)Kmalloc((SZ)len + 1);
		if ( name == NULL ) {
			return EX_NOMEM;
		}
		knl_memcpy(name, last, len);
		name[len] = 0;
		name_raw(name, name, len + 1);
		er = xf_create(name, &nd.u, &u);
		Kfree(name);
		box_forget(&nd.u);
		if ( er < E_OK ) {
			return fx(er);
		}
		er = walk(path, &nd, NULL, NULL);
		oflags &= ~(UINT)O_TRUNC;
	} else if ( er >= EX_OK && ( oflags & ( O_CREAT | O_EXCL ) ) == ( O_CREAT | O_EXCL ) ) {
		return EX_EXIST;
	}
	if ( er < EX_OK ) {
		return er;
	}
	if ( nd.kind == K_DEV ) {
		if ( ( oflags & O_DIRECTORY ) != 0 ) return EX_NOTDIR;
		if ( !may(&nd.u, wr ? OB_OP_WRITE : OB_OP_READ) ) return EX_ACCES;
		er = knl_ttydev_open(&nd.u, oflags, f);
		if ( er == EX_NOENT ) {
			return dev_open(&nd, oflags);
		}
		if ( er >= EX_OK ) {
			f->dir_off = OF_TTY;
		}
		return er;
	}
	if ( nd.kind == K_DIR ) {
		if ( wr ) return EX_ISDIR;
		f->mode = FS_IFDIR;
	} else {
		if ( ( oflags & O_DIRECTORY ) != 0 ) return EX_NOTDIR;
		if ( nd.kind == K_DOC ) {
			return doc_open(&nd, oflags);
		}
		if ( wr && nd.kind != K_FILE ) {
			return EX_ACCES;		/* a program: read only here */
		}
		if ( wr && !may(&nd.u, OB_OP_WRITE) ) {
			return EX_ACCES;
		}
		if ( !wr && !may(&nd.u, OB_OP_READ) ) {
			return EX_ACCES;
		}
		f->mode = FS_IFREG;
	}
	of = (OFILE *)Kcalloc(1, sizeof(OFILE));
	if ( of == NULL ) {
		return EX_NOMEM;
	}
	of->n = nd;
	if ( f->mode == FS_IFREG ) {
		if ( wr && ( oflags & O_TRUNC ) != 0 ) {
			ID	key = ob_opn_obj(&nd.u, OB_OP_WRITE);

			if ( key >= E_OK ) {
				(void)ob_trn_rec(key, nd.rec, 0);
				ob_cls_obj(key);
				of->wrote = TRUE;
			}
		}
		f->size = rec_size(&nd.u, nd.rec);
	}
	f->obj = of;
	f->ino = ( nd.node == N_OBJ ) ? ino_of(&nd.u) : (UD)nd.node;
	f->offset = 0;
	return EX_OK;
}

LOCAL ER of_close( T_FILE *f )
{
	OFILE	*of = (OFILE *)f->obj;

	if ( IS_TTY(f) ) return knl_fimp_ttydev.close(f);
	if ( of == NULL ) return EX_OK;
	if ( of->wrote ) {
		(void)xf_refresh(&of->n.u);
		if ( of->n.hasparent ) box_forget(&of->n.parent);
	}
	Kfree(of);
	f->obj = NULL;
	return EX_OK;
}

LOCAL INT of_read( T_FILE *f, void *buf, SZ len )
{
	OFILE	*of = (OFILE *)f->obj;
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( IS_TTY(f) ) return knl_fimp_ttydev.read(f, buf, len);
	key = ob_opn_obj(&of->n.u, OB_OP_READ);
	if ( key < E_OK ) {
		return fx(key);
	}
	er = ob_rea_rec(key, of->n.rec, (D)f->offset, buf, len, &asz);
	ob_cls_obj(key);
	if ( er == E_NOEXS || er == E_PAR ) {
		return 0;			/* at or past the end */
	}
	if ( er < E_OK ) {
		return fx(er);
	}
	f->offset += (UD)asz;
	return (INT)asz;
}

LOCAL INT of_write( T_FILE *f, CONST void *buf, SZ len )
{
	OFILE	*of = (OFILE *)f->obj;
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( IS_TTY(f) ) return knl_fimp_ttydev.write(f, buf, len);
	if ( of->n.kind != K_FILE ) {
		return EX_ACCES;
	}
	key = ob_opn_obj(&of->n.u, OB_OP_WRITE);
	if ( key < E_OK ) {
		return fx(key);
	}
	if ( f->offset > f->size ) {
		/* past the end: the record lengthened to there, which reads as
		   zeros, in one change */
		er = ob_trn_rec(key, of->n.rec, f->offset);
		if ( er < E_OK ) {
			ob_cls_obj(key);
			return fx(er);
		}
	}
	er = ob_wri_rec(key, of->n.rec, (D)f->offset, buf, len, &asz);
	ob_cls_obj(key);
	if ( er < E_OK ) {
		return fx(er);
	}
	of->wrote = TRUE;
	f->offset += (UD)asz;
	if ( f->offset > f->size ) f->size = f->offset;
	return (INT)asz;
}

LOCAL ER of_truncate( T_FILE *f, UD len )
{
	OFILE	*of = (OFILE *)f->obj;
	ID	key;
	ER	er;

	if ( of->n.kind != K_FILE ) {
		return EX_ACCES;
	}
	if ( len > f->size ) {
		UD	keep = f->offset;
		INT	r;

		f->offset = len - 1;
		r = of_write(f, (CONST UB *)"", 1);
		f->offset = keep;
		return ( r < 0 ) ? r : EX_OK;
	}
	key = ob_opn_obj(&of->n.u, OB_OP_WRITE);
	if ( key < E_OK ) {
		return fx(key);
	}
	er = ob_trn_rec(key, of->n.rec, len);
	ob_cls_obj(key);
	if ( er < E_OK ) {
		return fx(er);
	}
	f->size = len;
	of->wrote = TRUE;
	return EX_OK;
}

/* The stat of a node */
LOCAL ER node_st( CONST ONODE *nd, T_FSXSTAT *st )
{
	T_OBPRT	*prt;
	T_OBREF	r;

	knl_memset(st, 0, sizeof(*st));
	st->st.ino = ino_of(&nd->u);
	st->st.mtime = (UD)( ( nd->mtime > 0 ) ? nd->mtime : ob_boot );
	st->st.mode = ( nd->kind == K_DIR ) ? FS_IFDIR : ( nd->kind == K_DEV ) ? FS_IFCHR : FS_IFREG;
	prt = knl_ob_prt_get(&nd->u);
	if ( prt != NULL ) {
		UINT	mode = prt->mode & 0777;

		if ( nd->kind == K_DOC ) mode &= ~0222U;	/* read only here */
		if ( nd->kind == K_FILE || nd->kind == K_DOC ) mode &= ~0111U;
		if ( ( prt->attr & OB_A_RONLY ) != 0 ) mode &= ~0222U;
		st->st.mode |= mode;
		st->uid = psx_id_of(&prt->owner);
		st->gid = psx_id_of(&prt->group);
	} else {
		st->st.mode |= ( nd->kind == K_DIR ) ? 0555 : ( nd->kind == K_PROG ) ? 0555 : 0444;
	}
	if ( prt != NULL ) Kfree(prt);
	if ( ob_ref_obj(&nd->u, &r) >= E_OK ) {
		st->nlink = ( r.refcnt > 0 ) ? (UINT)r.refcnt : 1;
	}
	if ( nd->kind == K_DIR ) {
		st->st.size = 512;
	} else {
		st->st.size = rec_size(&nd->u, nd->rec);
	}
	return EX_OK;
}

LOCAL ER of_xstat( T_MOUNT *m, CONST char *path, T_FSXSTAT *st )
{
	ONODE	nd;
	ER	er;

	(void)m;
	er = walk(path, &nd, NULL, NULL);
	return ( er < EX_OK ) ? er : node_st(&nd, st);
}

LOCAL ER of_fxstat( T_FILE *f, T_FSXSTAT *st )
{
	OFILE	*of = (OFILE *)f->obj;
	ER	er;

	if ( IS_TTY(f) ) return knl_fimp_ttydev.fxstat(f, st);
	er = node_st(&of->n, st);

	if ( er >= EX_OK && f->mode == FS_IFREG ) {
		st->st.size = f->size;
	}
	return er;
}

LOCAL void dent( T_DIRENT *d, CONST UB *name, UINT mode, UD ino )
{
	s_cpy(d->name, name, FS_NAME_MAX);
	d->mode = mode;
	d->size = 0;
	d->ino = ino;
}

LOCAL INT of_getdents( T_FILE *f, T_DIRENT *buf, INT nent )
{
	OFILE	*of = (OFILE *)f->obj;
	OLIST	*l;
	OENT	*e;
	INT	k = 0, i;
	ER	er = EX_OK;

	l = box_get(&of->n.u, &er);
	if ( l == NULL ) {
		return er;
	}
	e = l->e;
	for ( i = (INT)f->offset; i < l->n && k < nent; i++ ) {
		dent(&buf[k++], e[i].name, ( e[i].kind == K_DIR ) ? FS_IFDIR : ( e[i].kind == K_DEV ) ? FS_IFCHR
				: FS_IFREG, ino_of(&e[i].u));
		f->offset++;
	}
	box_put(l);
	return k;
}

/*
 * The box a new name goes into, and the name as the object will have it,
 * in memory of its own (*p_name, Kfree it)
 */
LOCAL ER new_name( CONST char *path, ONODE *dir, UB **p_name )
{
	CONST UB *last;
	UB	*name;
	INT	len;
	ER	er;

	*p_name = NULL;
	er = walk(path, dir, &last, &len);
	if ( er >= EX_OK ) {
		return EX_EXIST;
	}
	if ( er != EX_NOENT || last == NULL ) {
		return er;
	}
	if ( dir->node != N_OBJ || dir->kind != K_DIR ) {
		return EX_ACCES;
	}
	if ( len >= FS_NAME_MAX ) {
		return EX_NAMETOOLONG;
	}
	name = (UB *)Kmalloc((SZ)len + 1);
	if ( name == NULL ) {
		return EX_NOMEM;
	}
	knl_memcpy(name, last, len);
	name[len] = 0;
	name_raw(name, name, len + 1);
	*p_name = name;
	return EX_OK;
}

/* s as the text of a JSON string, every character the rules ask escaped; the bytes put */
LOCAL INT json_esc( UB *d, CONST UB *s )
{
	CONST char *hex = "0123456789abcdef";
	INT	i, k = 0;

	for ( i = 0; s[i] != 0; i++ ) {
		UB	c = s[i];

		if ( c == '"' || c == '\\' ) {
			d[k++] = '\\';
			d[k++] = c;
		} else if ( c < 0x20 ) {
			d[k++] = '\\'; d[k++] = 'u'; d[k++] = '0'; d[k++] = '0';
			d[k++] = (UB)hex[c >> 4];
			d[k++] = (UB)hex[c & 15];
		} else {
			d[k++] = c;
		}
	}
	return k;
}

LOCAL ER of_mkdir( T_MOUNT *m, CONST char *path )
{
	CONST char *a = "{\"name\":\"";
	CONST char *b = "\",\"relationship\":[],\"linktype\":false,\"refCount\":0,\"recordCount\":1,"
			"\"editable\":true,\"deletable\":true,\"readable\":true,"
			"\"applist\":{\"virtual-object-list\":{\"name\":\"仮身一覧\",\"defaultOpen\":true}}}";
	CONST char *x = "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"";
	CONST char *y = "\">\n<figure>\n</figure>\n</tad>\n";
	ONODE	dir;
	UB	*name, *j, *t;
	T_OBCRE	c;
	TS_UUID	u;
	INT	jn, tn, nl, i;
	ER	er;

	(void)m;
	er = new_name(path, &dir, &name);
	if ( er < EX_OK ) {
		return er;
	}
	if ( !may(&dir.u, OB_OP_WRITE) ) {
		Kfree(name);
		return EX_ACCES;
	}
	/* an escaped byte takes at most six */
	nl = s_len(name);
	j = (UB *)Kmalloc((SZ)( s_len((CONST UB *)a) + 6 * nl + s_len((CONST UB *)b) + 1 ));
	t = (UB *)Kmalloc((SZ)( s_len((CONST UB *)x) + 6 * nl + s_len((CONST UB *)y) + 1 ));
	if ( j == NULL || t == NULL ) {
		if ( j != NULL ) Kfree(j);
		if ( t != NULL ) Kfree(t);
		Kfree(name);
		return EX_NOMEM;
	}
	jn = 0;
	for ( i = 0; a[i] != 0; i++ ) j[jn++] = (UB)a[i];
	jn += json_esc(j + jn, name);
	for ( i = 0; b[i] != 0; i++ ) j[jn++] = (UB)b[i];
	j[jn] = 0;

	tn = 0;
	for ( i = 0; x[i] != 0; i++ ) t[tn++] = (UB)x[i];
	for ( i = 0; name[i] != 0; i++ ) {
		switch ( name[i] ) {
		  case '&':	t[tn++] = '&'; t[tn++] = 'a'; t[tn++] = 'm'; t[tn++] = 'p'; t[tn++] = ';'; break;
		  case '<':	t[tn++] = '&'; t[tn++] = 'l'; t[tn++] = 't'; t[tn++] = ';'; break;
		  case '"':	t[tn++] = '&'; t[tn++] = 'q'; t[tn++] = 'u'; t[tn++] = 'o'; t[tn++] = 't'; t[tn++] = ';'; break;
		  default:	t[tn++] = name[i]; break;
		}
	}
	for ( i = 0; y[i] != 0; i++ ) t[tn++] = (UB)y[i];
	t[tn] = 0;
	Kfree(name);

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = j;
	c.jsonsz = jn;
	c.near = dir.u;
	er = ob_cre_obj(&c, &u);
	if ( er >= E_OK ) {
		er = om_obj_record_put(&u, 0, OB_RT_TAD, 0, t, tn);
		if ( er >= E_OK ) er = om_obj_link_add(&dir.u, &u);
		if ( er < E_OK ) (void)ob_del_obj(&u);
	}
	Kfree(j);
	Kfree(t);
	box_forget(&dir.u);
	return ( er < E_OK ) ? fx(er) : EX_OK;
}

/* A name taken out of its box */
LOCAL ER unname( CONST char *path, BOOL isdir )
{
	ONODE	nd;
	OLIST	*l;
	INT	n = 0;
	ER	er;

	er = walk(path, &nd, NULL, NULL);
	if ( er < EX_OK ) {
		return er;
	}
	if ( nd.node != N_OBJ || !nd.hasparent ) {
		return EX_BUSY;			/* a place of the root */
	}
	if ( isdir ) {
		if ( nd.kind != K_DIR ) return EX_NOTDIR;
		l = box_get(&nd.u, &er);
		if ( l == NULL ) return er;
		n = l->n;
		box_put(l);
		if ( n > 0 ) return EX_NOTEMPTY;
	} else if ( nd.kind == K_DIR ) {
		return EX_ISDIR;
	}
	er = om_obj_link_del(&nd.parent, &nd.u);
	box_forget(&nd.parent);
	return ( er < E_OK ) ? fx(er) : EX_OK;
}

LOCAL ER of_rmdir( T_MOUNT *m, CONST char *path )	{ (void)m; return unname(path, TRUE); }
LOCAL ER of_unlink( T_MOUNT *m, CONST char *path )	{ (void)m; return unname(path, FALSE); }

LOCAL ER of_rename( T_MOUNT *m, CONST char *from, CONST char *to )
{
	ONODE	a, b, dir;
	UB	*name, *raw;
	CONST UB *last;
	INT	len;
	ER	er;

	(void)m;
	er = walk(from, &a, NULL, NULL);
	if ( er < EX_OK ) return er;
	if ( a.node != N_OBJ || !a.hasparent ) return EX_BUSY;
	er = walk(to, &b, &last, &len);
	if ( er >= EX_OK ) {
		if ( ts_uuid_cmp(&a.u, &b.u) == 0 ) return EX_OK;
		if ( b.node != N_OBJ || !b.hasparent ) return EX_BUSY;
		if ( b.kind == K_DIR && a.kind != K_DIR ) return EX_ISDIR;
		if ( b.kind != K_DIR && a.kind == K_DIR ) return EX_NOTDIR;
		er = unname(to, b.kind == K_DIR);
		if ( er < EX_OK ) return er;
		er = walk(to, &dir, &last, &len);
		if ( er != EX_NOENT || last == NULL ) return ( er < EX_OK ) ? er : EX_EXIST;
	} else if ( er == EX_NOENT && last != NULL ) {
		dir = b;
	} else {
		return er;
	}
	if ( dir.node != N_OBJ || dir.kind != K_DIR ) return EX_ACCES;
	if ( len >= FS_NAME_MAX ) return EX_NAMETOOLONG;
	name = (UB *)Kmalloc((SZ)len * 2 + 2);
	if ( name == NULL ) return EX_NOMEM;
	raw = name + len + 1;
	knl_memcpy(name, last, len);
	name[len] = 0;

	er = E_OK;
	if ( ts_uuid_cmp(&dir.u, &a.parent) != 0 ) {
		er = om_obj_link_add(&dir.u, &a.u);
		if ( er >= E_OK ) er = om_obj_link_del(&a.parent, &a.u);
		box_forget(&dir.u);
		box_forget(&a.parent);
	}
	if ( er >= E_OK && !s_same(name, a.name) ) {
		/* the object takes the name whole: a long one is never cut */
		name_raw(name, raw, len + 1);
		er = ( a.kind == K_FILE ) ? xf_rename(&a.u, raw) : om_store_rename(&a.u, raw);
		box_forget(NULL);
	}
	Kfree(name);
	return ( er < E_OK ) ? fx(er) : EX_OK;
}

LOCAL ER of_link( T_MOUNT *m, CONST char *from, CONST char *to )
{
	ONODE	a, dir;
	UB	*name;
	BOOL	same;
	ER	er;

	(void)m;
	er = walk(from, &a, NULL, NULL);
	if ( er < EX_OK ) return er;
	if ( a.node != N_OBJ ) return EX_PERM;
	er = new_name(to, &dir, &name);
	if ( er < EX_OK ) return er;
	same = s_same(name, a.name);
	Kfree(name);
	if ( !same ) {
		return EX_NOTSUP;		/* a link shows its object's own name */
	}
	er = om_obj_link_add(&dir.u, &a.u);
	box_forget(&dir.u);
	return ( er < E_OK ) ? fx(er) : EX_OK;
}

LOCAL ER of_chmod( T_MOUNT *m, CONST char *path, UINT mode )
{
	ONODE	nd;
	T_OBPRT	*prt;
	ER	er;

	(void)m;
	er = walk(path, &nd, NULL, NULL);
	if ( er < EX_OK ) return er;
	if ( nd.node != N_OBJ ) return EX_PERM;
	prt = knl_ob_prt_get(&nd.u);
	if ( prt == NULL ) return EX_NOMEM;
	er = E_OK;
	if ( er >= E_OK ) {
		prt->mode = ( prt->mode & ~0777U ) | ( mode & 0777 );
		er = ob_set_prt(&nd.u, prt);
	}
	Kfree(prt);
	return ( er < E_OK ) ? fx(er) : EX_OK;
}

LOCAL ER of_utime( T_MOUNT *m, CONST char *path, UD mtime )
{
	ONODE	nd;

	(void)m;
	(void)mtime;
	return walk(path, &nd, NULL, NULL);
}

LOCAL ER of_statvfs( T_MOUNT *m, T_FSSTAT *st )
{
	T_OBVOL	v;
	ER	er;

	(void)m;
	er = ob_ref_vol(NULL, &v);
	if ( er < E_OK ) {
		return fx(er);
	}
	st->blocks = v.blocks;
	st->bfree = v.bfree;
	st->bsize = v.bsize;
	return EX_OK;
}

LOCAL ER of_ioctl( T_FILE *f, UINT cmd, void *arg )
{
	if ( IS_TTY(f) ) return knl_fimp_ttydev.ioctl(f, cmd, arg);
	return EX_NOTTY;
}

LOCAL UINT of_poll( T_FILE *f, UINT events )
{
	if ( IS_TTY(f) ) return knl_fimp_ttydev.poll(f, events);
	return events & ( FS_POLL_IN | FS_POLL_OUT );
}

/* fs_sync, and with it sync and syncfs of the POSIX layer: every native volume's log on the medium */
LOCAL ER of_sync( T_MOUNT *m )
{
	(void)m;
	return ( ts_syn_all() >= E_OK ) ? EX_OK : EX_IO;
}

/*
 * fsync and fdatasync of one file: each write of an object here opens
 * the object, writes and closes it, and the closing waits until what it
 * wrote is on the medium (design 7.3), so there is nothing left to wait for.
 */
LOCAL ER of_fsync( T_FILE *f )
{
	(void)f;
	return EX_OK;
}

EXPORT CONST T_FIMP knl_fimp_obj = {
	.name     = "objfs",
	.mount    = of_mount,
	.unmount  = of_unmount,
	.open     = of_open,
	.close    = of_close,
	.read     = of_read,
	.write    = of_write,
	.truncate = of_truncate,
	.getdents = of_getdents,
	.mkdir    = of_mkdir,
	.rmdir    = of_rmdir,
	.unlink   = of_unlink,
	.rename   = of_rename,
	.statvfs  = of_statvfs,
	.utime    = of_utime,
	.xstat    = of_xstat,
	.fxstat   = of_fxstat,
	.link     = of_link,
	.chmod    = of_chmod,
	.ioctl    = of_ioctl,
	.poll     = of_poll,
	.sync     = of_sync,
	.fsync    = of_fsync,
};

/* ---------------------------------------------------------------- for the kernel */

EXPORT ER knl_objfs_lookup( CONST char *path, TS_UUID *p_uuid )
{
	ONODE	nd;
	ER	er;

	if ( path == NULL || path[0] != '/' || ob_mtxid <= 0 ) {
		return EX_NOENT;
	}
	er = walk(path, &nd, NULL, NULL);
	if ( er < EX_OK ) {
		return er;
	}
	if ( nd.kind == K_DIR ) {
		return EX_ISDIR;
	}
	*p_uuid = nd.u;
	return ( nd.kind == K_PROG ) ? EX_OK : EX_NOEXEC;
}

EXPORT UINT knl_fs_uid_of( CONST TS_UUID *who )
{
	return psx_id_of(who);
}

/*
 * The object of the tree the kernel keeps: dev, whose links are made to
 * be the device objects there are, again whenever the devices change
 * (the device manager's serial number). Done by a task of the kernel's,
 * as the system, which owns it. tmp is emptied by the object layer as
 * its volume is attached, being in the list of 機器の設定 (include/ts/conf.h).
 */
#define DEVDIR_TICK	1000
#define DEVDIR_PAGE	256		/* devices asked for at a time */

LOCAL void devdir_sync( CONST TS_UUID *dev )
{
	TS_UUID	*want = NULL, *nw_;
	INT	nw = 0, got, room = 0;

	/* every device there is, a page at a time from the last one given */
	for ( ;; ) {
		nw_ = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( room + DEVDIR_PAGE ));
		if ( nw_ == NULL ) {
			if ( want != NULL ) Kfree(want);
			return;			/* the links as they were */
		}
		if ( want != NULL ) {
			knl_memcpy(nw_, want, (INT)( sizeof(TS_UUID) * (SZ)nw ));
			Kfree(want);
		}
		want = nw_;
		room += DEVDIR_PAGE;
		got = 0;
		if ( ob_lst_obj(OB_T_DEVICE, 0, ( nw > 0 ) ? &want[nw - 1] : NULL, want + nw, DEVDIR_PAGE, &got) < E_OK ) {
			break;
		}
		nw += got;
		if ( got < DEVDIR_PAGE ) break;
	}
	(void)om_obj_links_set(dev, want, nw);
	box_forget(dev);
	Kfree(want);
}

LOCAL ID	devdir_tid = 0;
LOCAL ID	devdir_flg = 0;
LOCAL volatile BOOL devdir_force = FALSE;	/* made again now, whatever the serial says */

LOCAL void devdir_task( INT stacd, void *exinf )
{
	TS_UUID	dev;
	UW	seen = ~(UW)0, now;

	(void)stacd;
	(void)exinf;
	if ( !uuid_of(SYSDEF_DEV, &dev) ) {
		tk_ext_tsk();
	}
	for ( ;; ) {
		now = knl_obdev_serial();
		if ( now != seen || devdir_force ) {
			devdir_force = FALSE;
			T_OBREF	r;

			if ( ob_ref_obj(&dev, &r) >= E_OK ) {
				devdir_sync(&dev);
				seen = now;
			}
		}
		if ( devdir_flg > 0 ) {
			tk_set_flg(devdir_flg, 1);	/* those waiting in knl_objfs_dev_sync */
		}
		(void)tk_slp_tsk(DEVDIR_TICK);
	}
}

/*
 * dev made to agree with the devices at once, rather than at the next
 * tick: what adds a device a program opens straight away (the slave of
 * a pseudo terminal) waits here for it to be listed, up to tmo ms.
 */
EXPORT void knl_objfs_dev_sync( TMO tmo )
{
	UINT	ptn;

	if ( devdir_tid <= 0 || devdir_flg <= 0 ) {
		return;
	}
	(void)tk_clr_flg(devdir_flg, 0);
	devdir_force = TRUE;
	(void)tk_wup_tsk(devdir_tid);
	if ( tmo != TMO_POL ) {
		(void)tk_wai_flg(devdir_flg, 1, TWF_ORW, &ptn, tmo);
	}
}

/* "/" and the devices of the POSIX programs, once the file layer and the objects are up */
EXPORT void knl_fs_posix_mounts( void )
{
	T_CTSK	ctsk;
	ID	tid;

	(void)fs_regist(&knl_fimp_obj);
	(void)fs_attach("objfs", NULL, "/", 0);
	knl_ttydev_start();

	{
		T_CFLG	cflg;

		cflg.exinf = NULL;
		cflg.flgatr = TA_TFIFO | TA_WMUL;
		cflg.iflgptn = 0;
		devdir_flg = tk_cre_flg(&cflg);
	}
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)devdir_task;
	ctsk.itskpri = 20;
	ctsk.stksz   = 8 * 1024;
	tid = tk_cre_tsk(&ctsk);
	if ( tid > 0 && tk_sta_tsk(tid, 0) >= E_OK ) devdir_tid = tid;
}
