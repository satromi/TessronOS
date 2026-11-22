/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obxref.c
 *	Links between volumes: the record a volume keeps of the links
 *	objects of other volumes make to its objects (design 8.3)
 *
 *	A link to an object of another volume is not in the count of its
 *	object on that volume: the link table of the object linking marks it
 *	as pointing off its volume. So that such an object is still counted,
 *	is never garbage and is reached when its volume is checked, its
 *	volume keeps a record of the links made to it from elsewhere: which
 *	volume, which object, how many. The record is an object of the
 *	volume, named by the superblock (T_TSFSBLK_SB.xref_uuid) and held
 *	once like the top object, whose one xmlTAD record has a <link> for
 *	each link from elsewhere:
 *
 *	  <link id="target" xref-volume="volume" xref-object="object"/>
 *
 *	The link table of that object is made from these, so each counts
 *	once in the count of the object it names, as a link of the volume.
 *
 *	The record follows the link tables: what they gain and lose of
 *	links to other volumes is queued by the native store
 *	(ts_obj_xq_take) and taken in here when an object is closed or
 *	deleted. What happens while the other volume is not attached, or
 *	whatever was lost, is put right when a volume is attached: the
 *	record of each volume there is made to agree, for the links of each
 *	other volume there, with what their link tables say.
 *
 *	A record says at most the links one object of its volume may have
 *	(1024 on a volume of the format's version 4, any number on one of
 *	version 5, knl_tsfs_max_link); a link beyond that is not recorded
 *	(E_LIMIT, said on the console), and is found again by the next
 *	matching.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/tsfs.h>
#include <ts/tsfsobj.h>
#include <ts/tsfsck.h>
#include <ts/uuid.h>
#include "obj.h"

#define XR_VOLS		64		/* native volumes matched at once */
#define XR_TAKE		64		/* queued changes taken at once */
#define XR_LIST		64		/* objects listed at once */
#define XR_LINE		160		/* bytes of one <link> of the record */

/* One entry of the record: so many links from that object of that volume */
typedef struct {
	TS_UUID	v;
	TS_UUID	h;
	TS_UUID	t;
	INT	n;
} XRENT;

/* The entries, as many as there are, the table twice as large each time it fills */
typedef struct {
	XRENT	*e;
	INT	n, cap;
	UINT	max;			/* links the record may say, 0 for no limit */
} XRTAB;

LOCAL XRTAB *xr_new( void )
{
	return (XRTAB *)Kcalloc(1, sizeof(XRTAB));
}

LOCAL void xr_free( XRTAB *x )
{
	if ( x != NULL ) {
		if ( x->e != NULL ) Kfree(x->e);
		Kfree(x);
	}
}

/* One entry put at the end; FALSE when there is no memory for it */
LOCAL BOOL xr_push( XRTAB *x, CONST XRENT *e )
{
	if ( x->n >= x->cap ) {
		INT	cap = ( x->cap > 0 ) ? x->cap * 2 : 64;
		XRENT	*ne = (XRENT *)Kmalloc(sizeof(XRENT) * (SZ)cap);

		if ( ne == NULL ) {
			return FALSE;
		}
		if ( x->n > 0 ) knl_memcpy(ne, x->e, sizeof(XRENT) * (SZ)x->n);
		if ( x->e != NULL ) Kfree(x->e);
		x->e = ne;
		x->cap = cap;
	}
	x->e[x->n++] = *e;
	return TRUE;
}

LOCAL ID	xr_mtx = 0;		/* the records, one change at a time */
LOCAL ID	xr_dmtx = 0;		/* the queue, taken in by one task at a time */

LOCAL CONST char xr_json[] =
	"{\"name\":\"ほかのボリュームからの参照\",\"refCount\":0,\"recordCount\":1,"
	"\"editable\":false,\"deletable\":false,\"readable\":true}";

EXPORT void knl_obxref_init( void )
{
	T_CMTX	cmtx;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	if ( xr_mtx <= 0 ) xr_mtx = tk_cre_mtx(&cmtx);
	if ( xr_dmtx <= 0 ) xr_dmtx = tk_cre_mtx(&cmtx);
}

LOCAL BOOL is_zero( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( u->b[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the record as text */

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( *s != 0 && at < max - 1 ) b[at++] = (UB)*s++;
	b[at] = 0;
	return at;
}

LOCAL INT put_uuid( UB *b, INT at, INT max, CONST TS_UUID *u )
{
	char	s[TS_UUID_STRLEN + 1];

	(void)ts_uuid_to_str(u, s, sizeof(s));
	return put(b, at, max, s);
}

/* The value of attribute 'name' in the tag [a, b) as a UUID */
LOCAL BOOL attr_uuid( CONST UB *t, SZ a, SZ b, CONST char *name, TS_UUID *u )
{
	char	s[TS_UUID_STRLEN + 1];
	SZ	i;
	INT	k, m = 0;

	while ( name[m] != 0 ) m++;
	for ( i = a; i + m + 2 + TS_UUID_STRLEN < b; i++ ) {
		if ( t[i] != ' ' ) continue;
		for ( k = 0; k < m && t[i + 1 + k] == (UB)name[k]; k++ ) ;
		if ( k < m || t[i + 1 + m] != '=' || t[i + 2 + m] != '"' ) continue;
		for ( k = 0; k < TS_UUID_STRLEN; k++ ) s[k] = (char)t[i + 3 + m + k];
		s[TS_UUID_STRLEN] = 0;
		return (BOOL)( ts_str_to_uuid(s, u) >= E_OK );
	}
	return FALSE;
}

LOCAL XRENT *ent_of( XRTAB *x, CONST TS_UUID *v, CONST TS_UUID *h, CONST TS_UUID *t )
{
	INT	i;

	for ( i = 0; i < x->n; i++ ) {
		if ( ts_uuid_cmp(&x->e[i].t, t) == 0 && ts_uuid_cmp(&x->e[i].h, h) == 0
		  && ts_uuid_cmp(&x->e[i].v, v) == 0 ) {
			return &x->e[i];
		}
	}
	return NULL;
}

/* So many more links (or fewer) in the table; FALSE when it is full */
LOCAL BOOL ent_add( XRTAB *x, CONST TS_UUID *v, CONST TS_UUID *h, CONST TS_UUID *t, INT d )
{
	XRENT	*e = ent_of(x, v, h, t), z;
	UD	sum = 0;
	INT	i;

	for ( i = 0; i < x->n && x->max != 0; i++ ) sum += (UD)x->e[i].n;
	if ( d > 0 && x->max != 0 && sum + (UD)d > x->max ) {
		return FALSE;
	}
	if ( e == NULL ) {
		if ( d <= 0 ) {
			return TRUE;
		}
		z.v = *v;
		z.h = *h;
		z.t = *t;
		z.n = 0;
		if ( !xr_push(x, &z) ) {
			return FALSE;
		}
		e = &x->e[x->n - 1];
	}
	e->n += d;
	if ( e->n <= 0 ) {
		*e = x->e[--x->n];
	}
	return TRUE;
}

LOCAL void parse( XRTAB *x, CONST UB *t, SZ len )
{
	TS_UUID	v, h, g;
	SZ	i, j;

	x->n = 0;
	for ( i = 0; i + 6 < len; i++ ) {
		if ( t[i] != '<' || t[i + 1] != 'l' || t[i + 2] != 'i' || t[i + 3] != 'n'
		  || t[i + 4] != 'k' || t[i + 5] != ' ' ) {
			continue;
		}
		for ( j = i + 5; j < len && t[j] != '>'; j++ ) ;
		if ( attr_uuid(t, i + 5, j, "id", &g) && attr_uuid(t, i + 5, j, "xref-volume", &v)
		  && attr_uuid(t, i + 5, j, "xref-object", &h) ) {
			(void)ent_add(x, &v, &h, &g, 1);
		}
		i = j;
	}
}

/* ---------------------------------------------------------------- the record on the volume */

/* The record's object, made when it has none and 'make' says so; zero when there is none */
LOCAL ER rec_obj( ID vol, BOOL make, TS_UUID *p )
{
	T_COBJ	cobj;
	ID	od;
	INT	n;
	ER	er;

	er = knl_tsfs_xref_obj(vol, p);
	if ( er < E_OK ) {
		return er;			/* not a native volume */
	}
	if ( !is_zero(p) && knl_tsfs_exists(vol, p) >= E_OK ) {
		return E_OK;
	}
	knl_memset(p, 0, sizeof(*p));
	if ( !make ) {
		return E_OK;
	}
	cobj.json = (CONST UB *)xr_json;
	cobj.jsonsz = sizeof(xr_json) - 1;
	er = ts_cre_obj(vol, &cobj, p);
	if ( er < E_OK ) {
		return er;
	}
	od = ts_opn_obj(vol, p, TFO_READ | TFO_WRITE);
	if ( od > 0 ) {
		er = ts_ins_rec(od, -1, 1, 0, &n);
		(void)ts_cls_obj(od);
	} else {
		er = ( od < 0 ) ? (ER)od : E_SYS;
	}
	if ( er >= E_OK ) er = knl_tsfs_set_xref(vol, p);
	if ( er >= E_OK ) er = ts_lnk_obj(vol, p);	/* held, as the superblock names it */
	if ( er < E_OK ) {
		(void)ts_del_obj(vol, p);
		knl_memset(p, 0, sizeof(*p));
	}
	return er;
}

LOCAL ER rec_load( ID vol, CONST TS_UUID *obj, XRTAB *x )
{
	T_RREC	rr;
	UB	*buf;
	SZ	got = 0;
	ID	od;
	INT	cnt = 0;
	ER	er;

	x->n = 0;
	x->max = knl_tsfs_max_link(vol);
	if ( is_zero(obj) ) {
		return E_OK;
	}
	od = ts_opn_obj(vol, obj, TFO_READ);
	if ( od <= 0 ) {
		return ( od < 0 ) ? (ER)od : E_SYS;
	}
	er = ts_lst_rec(od, &rr, 1, &cnt);
	if ( er >= E_OK && cnt > 0 && rr.size > 0 ) {
		buf = (UB *)Kmalloc((SZ)rr.size + 1);
		if ( buf == NULL ) {
			er = E_NOMEM;
		} else {
			er = ts_rea_rec(od, 0, 0, buf, (SZ)rr.size, &got);
			if ( er >= E_OK ) parse(x, buf, got);
			Kfree(buf);
		}
	}
	(void)ts_cls_obj(od);
	return er;
}

/* The record written whole; the link table of its object, and so the counts, follow on closing it */
LOCAL ER rec_store( ID vol, CONST TS_UUID *obj, CONST XRTAB *x )
{
	UB	*buf;
	SZ	cap = 64;
	INT	i, k, at;
	ID	od;
	ER	er;

	for ( i = 0; i < x->n; i++ ) cap += (SZ)x->e[i].n * XR_LINE;
	buf = (UB *)Kmalloc(cap);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	at = put(buf, 0, (INT)cap, "<document>\n");
	for ( i = 0; i < x->n; i++ ) {
		for ( k = 0; k < x->e[i].n; k++ ) {
			at = put(buf, at, (INT)cap, "<link id=\"");
			at = put_uuid(buf, at, (INT)cap, &x->e[i].t);
			at = put(buf, at, (INT)cap, "\" xref-volume=\"");
			at = put_uuid(buf, at, (INT)cap, &x->e[i].v);
			at = put(buf, at, (INT)cap, "\" xref-object=\"");
			at = put_uuid(buf, at, (INT)cap, &x->e[i].h);
			at = put(buf, at, (INT)cap, "\"/>\n");
		}
	}
	at = put(buf, at, (INT)cap, "</document>\n");
	od = ts_opn_obj(vol, obj, TFO_READ | TFO_WRITE);
	if ( od <= 0 ) {
		Kfree(buf);
		return ( od < 0 ) ? (ER)od : E_SYS;
	}
	er = ts_rpl_rec(od, 0, buf, (SZ)at);
	(void)ts_cls_obj(od);
	Kfree(buf);
	return er;
}

LOCAL BOOL same_tab( CONST XRTAB *a, CONST XRTAB *b )
{
	INT	i, k;

	if ( a->n != b->n ) {
		return FALSE;
	}
	for ( i = 0; i < a->n; i++ ) {
		for ( k = 0; k < b->n; k++ ) {
			if ( ts_uuid_cmp(&a->e[i].t, &b->e[k].t) == 0 && ts_uuid_cmp(&a->e[i].h, &b->e[k].h) == 0
			  && ts_uuid_cmp(&a->e[i].v, &b->e[k].v) == 0 ) {
				break;
			}
		}
		if ( k == b->n || a->e[i].n != b->e[k].n ) {
			return FALSE;
		}
	}
	return TRUE;
}

/* ---------------------------------------------------------------- one link at a time */

LOCAL ER vol_uuid( ID vol, TS_UUID *p )
{
	T_TSFSVINF	*inf = (T_TSFSVINF *)Kmalloc(sizeof(T_TSFSVINF));
	ER		er;

	if ( inf == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_vol_info(vol, inf);
	if ( er >= E_OK ) *p = inf->vuuid;
	Kfree(inf);
	return er;
}

EXPORT ER knl_obxref_move( CONST TS_UUID *hvol, CONST TS_UUID *host, CONST TS_UUID *target, INT d )
{
	XRTAB	*x;
	TS_UUID	obj, tv, real;
	ID	vol;
	ER	er;

	if ( hvol == NULL || host == NULL || target == NULL || d == 0 || xr_mtx <= 0 ) {
		return E_PAR;
	}
	/* the record of the volume holds the target under the UUID it has on that volume */
	if ( knl_obfile_real(target, &vol, &real) < E_OK || vol <= 0 ) {
		return E_OK;			/* not attached: matched when it is */
	}
	if ( vol_uuid(vol, &tv) < E_OK || ts_uuid_cmp(&tv, hvol) == 0 ) {
		return E_OK;			/* on the volume of the link after all */
	}
	x = xr_new();
	if ( x == NULL ) {
		return E_NOMEM;
	}
	tk_loc_mtx(xr_mtx, TMO_FEVR);
	er = rec_obj(vol, (BOOL)( d > 0 ), &obj);
	if ( er >= E_OK && ts_uuid_cmp(host, &obj) == 0 ) {
		er = E_PAR;			/* the record's own links */
	}
	if ( er >= E_OK && !is_zero(&obj) ) {
		er = rec_load(vol, &obj, x);
		if ( er >= E_OK ) {
			if ( !ent_add(x, hvol, host, &real, d) ) {
				er = E_LIMIT;
			} else {
				er = rec_store(vol, &obj, x);
			}
		}
	}
	tk_unl_mtx(xr_mtx);
	xr_free(x);
	if ( er < E_OK && er != E_PAR ) {
		tm_printf((UB *)"ob: link from another volume not recorded (%d)\n", (INT)er);
	}
	return er;
}

EXPORT void knl_obxref_drain( void )
{
	T_TSXQ	*q;
	ID	ids[XR_VOLS];
	BOOL	lost = FALSE, l;
	INT	n, i;

	if ( xr_dmtx <= 0 || !ts_obj_xq_any() || tk_loc_mtx(xr_dmtx, TMO_POL) < E_OK ) {
		return;				/* nothing waits, or another task is taking them in */
	}
	q = (T_TSXQ *)Kmalloc(sizeof(T_TSXQ) * XR_TAKE);
	while ( q != NULL && ( n = ts_obj_xq_take(q, XR_TAKE, &l) ) > 0 ) {
		lost = (BOOL)( lost || l );
		for ( i = 0; i < n; i++ ) {
			(void)knl_obxref_move(&q[i].hvol, &q[i].host, &q[i].target, q[i].d);
		}
	}
	if ( q != NULL ) {
		(void)ts_obj_xq_take(q, 0, &l);
		lost = (BOOL)( lost || l );
		Kfree(q);
	}
	tk_unl_mtx(xr_dmtx);
	if ( lost ) {
		/* some were not kept: every volume matched again */
		n = knl_obfile_natives(ids, NULL, XR_VOLS);
		for ( i = 0; i < n; i++ ) {
			(void)knl_obxref_sync(ids[i]);
		}
	}
}

/* ---------------------------------------------------------------- matching whole volumes */

/*
 * What the link tables of the volume 'hv' (its UUID hu) say they link
 * on the volume 'tv': into x, as entries from hu.
 */
LOCAL ER links_of( ID hv, CONST TS_UUID *hu, ID tv, XRTAB *x )
{
	TS_UUID	*ids, from, own, rt;
	T_RLNK	*lk;
	ID	rv;
	INT	got, nl, i, k, cap;
	BOOL	first = TRUE;
	ER	er = E_OK;

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * XR_LIST);
	cap = 64;
	lk = (T_RLNK *)Kmalloc(sizeof(T_RLNK) * (SZ)cap);
	if ( ids == NULL || lk == NULL ) {
		if ( ids != NULL ) Kfree(ids);
		if ( lk != NULL ) Kfree(lk);
		return E_NOMEM;
	}
	if ( knl_tsfs_xref_obj(hv, &own) < E_OK ) {
		knl_memset(&own, 0, sizeof(own));
	}
	for (;;) {
		got = 0;
		er = ts_lst_obj(hv, first ? NULL : &from, ids, XR_LIST, &got);
		if ( er < E_OK || got <= 0 ) {
			break;
		}
		first = FALSE;
		from = ids[got - 1];
		for ( i = 0; i < got; i++ ) {
			if ( ts_uuid_cmp(&ids[i], &own) == 0 ) {
				continue;		/* the record of hv itself */
			}
			/* how many links it has first, then room for them */
			nl = 0;
			if ( knl_tsfs_lst_lnk(hv, &ids[i], NULL, 0, &nl) < E_OK ) {
				continue;
			}
			if ( nl > cap ) {
				T_RLNK	*nk = (T_RLNK *)Kmalloc(sizeof(T_RLNK) * (SZ)nl);

				if ( nk == NULL ) {
					er = E_NOMEM;
					break;
				}
				Kfree(lk);
				lk = nk;
				cap = nl;
			}
			if ( nl == 0 || knl_tsfs_lst_lnk(hv, &ids[i], lk, cap, &nl) < E_OK ) {
				continue;
			}
			for ( k = 0; k < nl; k++ ) {
				/*
				 * an alias is the object of another machine's volume under
				 * the UUID it has there; any other UUID is looked for on tv
				 * as it is, tv being opened only for its check as well
				 */
				if ( knl_obfile_real(&lk[k].target, &rv, &rt) < E_OK || rv != tv ) {
					rt = lk[k].target;
				}
				if ( ( lk[k].flags & TSFS_LNK_EXTERNAL ) != 0
				  && knl_tsfs_exists(tv, &rt) >= E_OK
				  && !ent_add(x, hu, &ids[i], &rt, 1) ) {
					er = E_LIMIT;
				}
			}
		}
		if ( got < XR_LIST || er == E_NOMEM ) {
			break;
		}
	}
	Kfree(ids);
	Kfree(lk);
	return ( er == E_LIMIT || er == E_NOMEM || er >= E_OK ) ? er : E_OK;
}

/*
 * The record of 'tv' made to say, of the links from 'hv', what the link
 * tables of 'hv' say. Under xr_mtx.
 */
LOCAL ER sync_pair( ID hv, CONST TS_UUID *hu, ID tv )
{
	XRTAB	*now, *want;
	TS_UUID	obj;
	INT	i;
	ER	er;

	now = xr_new();
	want = xr_new();
	if ( now == NULL || want == NULL ) {
		xr_free(now);
		xr_free(want);
		return E_NOMEM;
	}
	er = rec_obj(tv, FALSE, &obj);
	if ( er >= E_OK ) er = rec_load(tv, &obj, now);
	if ( er >= E_OK ) {
		/* what the others link stays; what hv links is what its tables say */
		want->n = 0;
		want->max = now->max;
		for ( i = 0; i < now->n && er >= E_OK; i++ ) {
			if ( ts_uuid_cmp(&now->e[i].v, hu) != 0 && !xr_push(want, &now->e[i]) ) er = E_NOMEM;
		}
		if ( er >= E_OK ) er = links_of(hv, hu, tv, want);
		if ( er == E_LIMIT ) {
			tm_printf((UB *)"ob: more links from another volume than one record says\n");
			er = E_OK;
		}
	}
	if ( er >= E_OK && !same_tab(now, want) ) {
		if ( is_zero(&obj) ) er = rec_obj(tv, TRUE, &obj);
		if ( er >= E_OK ) er = rec_store(tv, &obj, want);
		tm_printf((UB *)"ob: links from another volume put right (%d)\n", (INT)er);
	}
	xr_free(now);
	xr_free(want);
	return er;
}

EXPORT ER knl_obxref_sync( ID vol )
{
	ID	ids[XR_VOLS];
	TS_UUID	vu[XR_VOLS], mine;
	INT	n, i;
	ER	er = E_OK, e;

	if ( xr_mtx <= 0 || vol_uuid(vol, &mine) < E_OK ) {
		return E_PAR;
	}
	knl_obxref_drain();			/* what was queued first */
	n = knl_obfile_natives(ids, vu, XR_VOLS);
	tk_loc_mtx(xr_mtx, TMO_FEVR);
	for ( i = 0; i < n; i++ ) {
		if ( ids[i] == vol ) {
			continue;
		}
		e = sync_pair(ids[i], &vu[i], vol);	/* what the others link here */
		if ( e < E_OK ) er = e;
		e = sync_pair(vol, &mine, ids[i]);	/* what this one links there */
		if ( e < E_OK ) er = e;
	}
	tk_unl_mtx(xr_mtx);
	knl_obxref_drain();			/* the records' own tables made again */
	return er;
}

EXPORT INT knl_obxref_list( ID vol, T_OBXREF *buf, INT max )
{
	XRTAB	*x;
	TS_UUID	obj;
	INT	i, n = 0;

	if ( xr_mtx <= 0 || ( x = xr_new() ) == NULL ) {
		return 0;
	}
	tk_loc_mtx(xr_mtx, TMO_FEVR);
	if ( rec_obj(vol, FALSE, &obj) >= E_OK && rec_load(vol, &obj, x) >= E_OK ) {
		for ( i = 0; i < x->n && n < max; i++ ) {
			buf[n].from_vol = x->e[i].v;
			buf[n].from_obj = x->e[i].h;
			buf[n].target = x->e[i].t;
			buf[n].count = x->e[i].n;
			n++;
		}
	}
	tk_unl_mtx(xr_mtx);
	xr_free(x);
	return n;
}

/* ---------------------------------------------------------------- the check */

LOCAL void tell( T_TSFSCK *res, FP_TSFSCK fn, void *ctx, BOOL fixed, CONST XRENT *e, INT why, INT found )
{
	T_TSFSCKEV	ev;

	knl_memset(&ev, 0, sizeof(ev));
	ev.phase = fixed ? TSFSCK_PH_REPAIR : TSFSCK_PH_INFO;
	ev.kind = TSFSCK_K_XREF;
	ev.fixed = fixed;
	ev.has_uuid = TRUE;
	ev.uuid = e->t;
	ev.has_other = TRUE;
	ev.other = e->h;
	ev.a = e->n;
	ev.b = why;
	ev.blk = (UD)found;
	if ( res != NULL && !fixed ) {
		res->found[TSFSCK_K_XREF]++;
		res->problems++;
	}
	if ( fn != NULL ) fn(ctx, &ev);
}

EXPORT ER knl_obxref_check( ID vol, BOOL fix, T_TSFSCK *res, FP_TSFSCK fn, void *ctx )
{
	XRTAB	*now, *want, *theirs;
	TS_UUID	obj, vu[XR_VOLS], mine, saved;
	ID	ids[XR_VOLS];
	INT	nv, i, k, s, before = ( res != NULL ) ? res->found[TSFSCK_K_XREF] : 0;
	BOOL	gone = FALSE;
	ER	er;

	if ( xr_mtx <= 0 || vol_uuid(vol, &mine) < E_OK ) {
		return E_PAR;
	}
	now = xr_new();
	want = xr_new();
	theirs = xr_new();
	if ( now == NULL || want == NULL || theirs == NULL ) {
		xr_free(now);
		xr_free(want);
		xr_free(theirs);
		return E_NOMEM;
	}
	nv = knl_obfile_natives(ids, vu, XR_VOLS);
	tk_loc_mtx(xr_mtx, TMO_FEVR);
	er = knl_tsfs_xref_obj(vol, &saved);
	if ( er >= E_OK ) er = rec_obj(vol, FALSE, &obj);
	if ( er >= E_OK && !is_zero(&saved) && is_zero(&obj) ) {
		XRENT	z;

		/* the superblock names a record that is not there */
		knl_memset(&z, 0, sizeof(z));
		z.t = saved;
		tell(res, fn, ctx, FALSE, &z, TSFSCK_X_OBJECT, 0);
		gone = TRUE;
	}
	if ( er >= E_OK ) er = rec_load(vol, &obj, now);
	want->n = 0;
	want->max = theirs->max = now->max;
	for ( i = 0; er >= E_OK && i < now->n; i++ ) {
		XRENT	*e = &now->e[i];

		if ( knl_tsfs_exists(vol, &e->t) < E_OK ) {
			tell(res, fn, ctx, FALSE, e, TSFSCK_X_TARGET, 0);
			continue;
		}
		for ( k = 0; k < nv && ts_uuid_cmp(&vu[k], &e->v) != 0; k++ ) ;
		if ( k == nv || ids[k] == vol ) {
			if ( k == nv ) {
				/* a volume not attached, or not native: whether its object is there is asked when it can be */
				if ( knl_obfile_vuuid_on(&e->v) && knl_obfile_vol_of(&e->h) <= 0 ) {
					tell(res, fn, ctx, FALSE, e, TSFSCK_X_SOURCE, 0);
					continue;
				}
			}
			if ( !xr_push(want, e) ) er = E_NOMEM;
			continue;
		}
		if ( knl_tsfs_exists(ids[k], &e->h) < E_OK ) {
			tell(res, fn, ctx, FALSE, e, TSFSCK_X_SOURCE, 0);
			continue;
		}
		/* counted again below, from that volume's link tables */
	}
	/* what the native volumes attached link here, from their link tables */
	for ( k = 0; er >= E_OK && k < nv; k++ ) {
		if ( ids[k] == vol ) {
			continue;
		}
		theirs->n = 0;
		if ( links_of(ids[k], &vu[k], vol, theirs) < E_OK ) {
			continue;
		}
		for ( i = 0; i < theirs->n; i++ ) {
			XRENT	*e = &theirs->e[i], *was = ent_of(now, &e->v, &e->h, &e->t);

			s = ( was != NULL ) ? was->n : 0;
			if ( s != e->n ) {
				XRENT	t = *e;

				t.n = s;
				tell(res, fn, ctx, FALSE, &t, ( s == 0 ) ? TSFSCK_X_MISSING : TSFSCK_X_COUNT, e->n);
			}
			(void)ent_add(want, &e->v, &e->h, &e->t, e->n);
		}
		/* entries of that volume whose object no longer links here */
		for ( i = 0; i < now->n; i++ ) {
			XRENT	*e = &now->e[i];

			if ( ts_uuid_cmp(&e->v, &vu[k]) == 0 && knl_tsfs_exists(vol, &e->t) >= E_OK
			  && knl_tsfs_exists(ids[k], &e->h) >= E_OK && ent_of(theirs, &e->v, &e->h, &e->t) == NULL ) {
				tell(res, fn, ctx, FALSE, e, TSFSCK_X_COUNT, 0);
			}
		}
	}
	if ( er >= E_OK && fix && ( gone || !same_tab(now, want) ) ) {
		if ( gone ) {
			(void)knl_tsfs_set_xref(vol, NULL);
			knl_memset(&obj, 0, sizeof(obj));
		}
		if ( is_zero(&obj) && want->n > 0 ) er = rec_obj(vol, TRUE, &obj);
		if ( er >= E_OK && !is_zero(&obj) ) er = rec_store(vol, &obj, want);
		if ( er >= E_OK ) {
			XRENT	z;

			knl_memset(&z, 0, sizeof(z));
			z.t = obj;
			z.n = 0;
			for ( i = 0; i < want->n; i++ ) z.n += want->e[i].n;
			tell(res, fn, ctx, TRUE, &z, TSFSCK_X_COUNT, z.n);
			if ( res != NULL ) {
				res->fixed[TSFSCK_K_XREF] += res->found[TSFSCK_K_XREF] - before;
				res->written = TRUE;
			}
		}
	}
	tk_unl_mtx(xr_mtx);
	xr_free(now);
	xr_free(want);
	xr_free(theirs);
	knl_obxref_drain();
	return er;
}
