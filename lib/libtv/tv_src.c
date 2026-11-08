/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tv_src.c
 *	Where the drawing of a program running as a process finds what a
 *	link shows and what it points at (T_TVSRC, design 11.11)
 *
 *	What a link shows -- its object's name, the words of its band, its
 *	icon, the paper of the object, whether the object can be found --
 *	is what the kernel's store holds for the desktop, asked for with
 *	ob_ref_vob: as many links as one screen has in one call, before the
 *	screen is drawn (tv_src_prefetch), and kept by the link until the
 *	object is said to have changed (tv_src_forget). What a link points
 *	at, opened, and the pictures a record names are read here with the
 *	object calls and kept likewise. What is kept is let go of when there
 *	is more of it than TVS_*_MAX, the longest unused first.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/dp.h>
#include <ts/uapp.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/tvsrc.h>
#include <string.h>
#include <stdlib.h>

#define TVS_LOOK_MAX	256		/* links whose look is kept */
#define TVS_DOC_MAX	32		/* records kept read */
#define TVS_PIC_MAX	32		/* pictures kept */
#define TVS_ICON_MAX	( 64 * 64 )	/* the pixels of an icon asked for at first */
#define TVS_CHUNK	( 64 * 1024 )

typedef struct {
	BOOL	used;
	TS_UUID	vobjid, target;
	UINT	clock;			/* when it was last used */
	ER	er;
	UINT	state;
	UW	paper;
	BOOL	has_paper;		/* the paper was asked for */
	INT	name_len, label_len;
	UB	name[TAD_NAME_MAX];
	UB	label[OB_VW_LABEL_MAX];
	UW	*icon;
	INT	icon_w, icon_h;
} LOOK;

typedef struct {
	BOOL	used;
	TS_UUID	id;
	INT	recno;
	UINT	clock;
	INT	held;			/* opened and not yet shut */
	UB	*xml;
	T_TAD	*doc;
} DOC;

typedef struct {
	BOOL	used;
	UB	name[TAD_NAME_MAX];
	UINT	clock;
	BOOL	bad;
	UW	*px;
	INT	w, h;
} PIC;

LOCAL LOOK	*tvs_look;
LOCAL DOC	tvs_doc[TVS_DOC_MAX];
LOCAL PIC	tvs_pic[TVS_PIC_MAX];
LOCAL UINT	tvs_clock;
LOCAL TS_UUID	tvs_host, tvs_self;
LOCAL BOOL	tvs_has_self;
LOCAL TS_UUID	tvs_live;		/* the object being edited, its record tvs_live_no */
LOCAL INT	tvs_live_no;
LOCAL T_TAD *CONST *tvs_live_rec;	/* where the program keeps it; NULL none */
LOCAL T_TVSRCSTAT tvs_stat;

LOCAL BOOL same( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

/* ---------------------------------------------------------------- the looks */

LOCAL void look_free( LOOK *k )
{
	if ( k->icon != NULL ) free(k->icon);
	memset(k, 0, sizeof(*k));
}

LOCAL LOOK *look_find( CONST T_VOBJ *v )
{
	INT	i;

	if ( tvs_look == NULL ) return NULL;
	for ( i = 0; i < TVS_LOOK_MAX; i++ ) {
		if ( tvs_look[i].used && same(&tvs_look[i].target, &v->target)
		  && same(&tvs_look[i].vobjid, &v->vobjid) ) {
			tvs_look[i].clock = ++tvs_clock;
			return &tvs_look[i];
		}
	}
	return NULL;
}

/* A slot to keep a look in: a free one, else the longest unused */
LOCAL LOOK *look_slot( void )
{
	INT	i, old = 0;

	if ( tvs_look == NULL ) {
		tvs_look = (LOOK *)calloc(TVS_LOOK_MAX, sizeof(LOOK));
		if ( tvs_look == NULL ) return NULL;
	}
	for ( i = 0; i < TVS_LOOK_MAX; i++ ) {
		if ( !tvs_look[i].used ) return &tvs_look[i];
		if ( tvs_look[i].clock < tvs_look[old].clock ) old = i;
	}
	look_free(&tvs_look[old]);
	return &tvs_look[old];
}

/* The answer of the kernel for one link kept */
LOCAL void look_keep( CONST T_OBRVOB *q )
{
	LOOK	*k = look_slot();

	if ( k == NULL ) return;
	k->used = TRUE;
	k->vobjid = q->vobj.vobjid;
	k->target = q->vobj.target;
	k->clock = ++tvs_clock;
	k->er = q->er;
	k->state = q->state & ~OB_VW_SHORT;
	k->paper = q->paper;
	k->has_paper = (BOOL)( ( q->want & OB_VW_PAPER ) != 0 );
	k->name_len = q->name_len;
	k->label_len = q->label_len;
	memcpy(k->name, q->name, sizeof(k->name));
	memcpy(k->label, q->label, sizeof(k->label));
	if ( q->icon != NULL && q->icon_w > 0 && q->icon_h > 0
	  && (INT)( q->icon_w * q->icon_h ) <= q->icon_max ) {
		SZ	n = (SZ)q->icon_w * q->icon_h;

		k->icon = (UW *)malloc(sizeof(UW) * (size_t)n);
		if ( k->icon != NULL ) {
			memcpy(k->icon, q->icon, sizeof(UW) * (size_t)n);
			k->icon_w = q->icon_w;
			k->icon_h = q->icon_h;
		}
	}
}

/*
 * The looks of n links asked for in one call of the kernel; an icon too
 * large for the room first given is asked for again with room for it
 */
LOCAL void look_ask( CONST T_VOBJ *const *v, INT n )
{
	T_OBRVOB	*q;
	UW		*room;
	INT		i, got;

	if ( n <= 0 ) return;
	q = (T_OBRVOB *)calloc((size_t)n, sizeof(T_OBRVOB));
	room = (UW *)malloc(sizeof(UW) * TVS_ICON_MAX * (size_t)n);
	if ( q == NULL || room == NULL ) {
		free(q);
		free(room);
		return;
	}
	for ( i = 0; i < n; i++ ) {
		q[i].vobj = *v[i];
		q[i].host = tvs_host;
		/*
		 * the object's paper only shows inside a link that says no colour
		 * of its own, and is the dearest to find: asked for then alone
		 */
		q[i].want = OB_VW_NAME | OB_VW_LABEL | OB_VW_ICON | OB_VW_STATE
			  | ( ( v[i]->bgcol == TAD_COL_NONE ) ? OB_VW_PAPER : 0 );
		q[i].icon = room + (SZ)TVS_ICON_MAX * i;
		q[i].icon_max = TVS_ICON_MAX;
	}
	got = ob_ref_vob(q, n);
	tvs_stat.asks++;
	for ( i = 0; i < got; i++ ) {
		if ( ( q[i].state & OB_VW_SHORT ) != 0 && q[i].icon_w > 0 && q[i].icon_h > 0 ) {
			T_OBRVOB	one = q[i];
			UW		*big = (UW *)malloc(sizeof(UW) * (size_t)q[i].icon_w * q[i].icon_h);

			if ( big != NULL ) {
				one.icon = big;
				one.icon_max = q[i].icon_w * q[i].icon_h;
				if ( ob_ref_vob(&one, 1) == 1 ) look_keep(&one);
				tvs_stat.asks++;
				free(big);
				continue;
			}
		}
		look_keep(&q[i]);
	}
	free(room);
	free(q);
}

LOCAL LOOK *look_of( CONST T_VOBJ *v )
{
	LOOK	*k = look_find(v);

	if ( k == NULL ) {
		look_ask(&v, 1);
		k = look_find(v);
	}
	return k;
}

EXPORT void tv_src_prefetch( CONST T_TAD *rec )
{
	T_VOBJ		*v;
	CONST T_VOBJ	*want[OB_RVOB_MAX];
	INT		i, k = 0, total;

	if ( rec == NULL ) return;
	total = tad_lnk_count(rec);
	if ( total <= 0 ) return;
	v = (T_VOBJ *)malloc(sizeof(T_VOBJ) * OB_RVOB_MAX);
	if ( v == NULL ) return;
	for ( i = 0; i < total; i++ ) {
		if ( tad_lnk_get(rec, i, &v[k]) < E_OK || look_find(&v[k]) != NULL ) continue;
		want[k] = &v[k];
		if ( ++k == OB_RVOB_MAX ) {
			look_ask(want, k);
			k = 0;
		}
	}
	if ( k > 0 ) look_ask(want, k);
	free(v);
}

/* ---------------------------------------------------------------- what T_TVSRC asks */

LOCAL INT src_name( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	LOOK	*k = look_of(v);
	INT	n;

	(void)arg;
	if ( k == NULL || k->name_len < 0 || max <= 0 ) return -1;
	n = ( k->name_len < max - 1 ) ? k->name_len : max - 1;
	memcpy(buf, k->name, (size_t)n);
	buf[n] = 0;
	return n;
}

LOCAL INT src_label( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	LOOK	*k = look_of(v);
	INT	n;

	(void)arg;
	if ( k == NULL || k->label_len < 0 || max <= 0 ) return -1;
	n = ( k->label_len < max - 1 ) ? k->label_len : max - 1;
	memcpy(buf, k->label, (size_t)n);
	buf[n] = 0;
	return n;
}

LOCAL ER src_icon( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h, void *arg )
{
	LOOK	*k = look_of(v);

	(void)arg;
	if ( k == NULL || k->icon == NULL ) return E_NOEXS;
	*p_px = k->icon;
	*p_w = k->icon_w;
	*p_h = k->icon_h;
	return E_OK;
}

/* The paper of a link kept from when it said a colour of its own, asked for now */
LOCAL void paper_ask( LOOK *k, CONST T_VOBJ *v )
{
	T_OBRVOB	q;

	if ( k == NULL || k->er < E_OK || k->has_paper ) return;
	memset(&q, 0, sizeof(q));
	q.vobj = *v;
	q.host = tvs_host;
	q.want = OB_VW_PAPER;
	k->paper = ( ob_ref_vob(&q, 1) == 1 && q.er >= E_OK ) ? q.paper : TV_PAPER_LOOK;
	k->has_paper = TRUE;
	tvs_stat.asks++;
}

LOCAL ER src_paper( CONST T_VOBJ *v, UW *p_colour, void *arg )
{
	LOOK	*k = look_of(v);

	(void)arg;
	paper_ask(k, v);
	if ( k == NULL || k->er < E_OK ) return E_NOEXS;
	*p_colour = k->paper;
	return E_OK;
}

LOCAL UINT src_state( CONST T_VOBJ *v, void *arg )
{
	LOOK	*k = look_of(v);

	(void)arg;
	return ( k != NULL ) ? k->state : TV_ST_GONE;
}

/* ---------------------------------------------------------------- records */

/* A record read whole, with a terminator after it; NULL when it cannot be */
EXPORT UB *tv_src_read( CONST TS_UUID *id, INT recno, SZ *p_len )
{
	UB	*buf = NULL, *nb;
	SZ	cap = 0, got = 0, n;
	ID	key = ob_opn_obj(id, OB_OP_READ);

	if ( key <= 0 ) return NULL;
	for (;;) {
		if ( cap - got < TVS_CHUNK ) {
			cap = ( cap == 0 ) ? TVS_CHUNK * 2 : cap * 2;
			nb = (UB *)realloc(buf, (size_t)cap + 1);
			if ( nb == NULL ) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = nb;
		}
		n = 0;
		if ( ob_rea_rec(key, recno, (D)got, buf + got, cap - got, &n) < E_OK || n <= 0 ) break;
		got += n;
		tvs_stat.reads++;
	}
	ob_cls_obj(key);
	if ( buf == NULL || got == 0 ) {
		free(buf);
		return NULL;
	}
	buf[got] = 0;
	if ( p_len != NULL ) *p_len = got;
	return buf;
}

LOCAL void doc_free( DOC *d )
{
	if ( d->doc != NULL ) tad_free(d->doc);
	free(d->xml);
	memset(d, 0, sizeof(*d));
}

EXPORT T_TAD *tv_src_get( CONST TS_UUID *id, INT recno )
{
	DOC	*d = NULL;
	UB	*xml;
	SZ	len = 0;
	T_TAD	*doc = NULL;
	INT	i, old = -1;

	if ( recno < 0 ) recno = 0;
	for ( i = 0; i < TVS_DOC_MAX; i++ ) {
		if ( tvs_doc[i].used && tvs_doc[i].recno == recno && same(&tvs_doc[i].id, id) ) {
			tvs_doc[i].clock = ++tvs_clock;
			return tvs_doc[i].doc;
		}
	}
	xml = tv_src_read(id, recno, &len);
	if ( xml == NULL ) return NULL;
	if ( tad_parse(xml, len, NULL, &doc) < E_OK || doc == NULL ) {
		free(xml);
		return NULL;
	}
	for ( i = 0; i < TVS_DOC_MAX && d == NULL; i++ ) {
		if ( !tvs_doc[i].used ) d = &tvs_doc[i];
		else if ( tvs_doc[i].held == 0 && ( old < 0 || tvs_doc[i].clock < tvs_doc[old].clock ) ) old = i;
	}
	if ( d == NULL && old >= 0 ) {
		d = &tvs_doc[old];
		doc_free(d);
	}
	if ( d == NULL ) {
		/* every one is being drawn: nowhere to keep it, and the tree points into the bytes */
		tad_free(doc);
		free(xml);
		return NULL;
	}
	d->used = TRUE;
	d->id = *id;
	d->recno = recno;
	d->clock = ++tvs_clock;
	d->xml = xml;
	d->doc = doc;
	return doc;
}

LOCAL T_TAD *src_open( CONST T_VOBJ *v, void *arg )
{
	T_TAD	*doc;
	INT	i;

	(void)arg;
	/* the record being edited, as it is now: not one of those kept, so not held */
	if ( tvs_live_rec != NULL && *tvs_live_rec != NULL && same(&v->target, &tvs_live)
	  && ( ( v->recno >= 0 ) ? v->recno : 0 ) == tvs_live_no ) {
		return *tvs_live_rec;
	}
	doc = tv_src_get(&v->target, ( v->recno >= 0 ) ? v->recno : 0);
	for ( i = 0; doc != NULL && i < TVS_DOC_MAX; i++ ) {
		if ( tvs_doc[i].used && tvs_doc[i].doc == doc ) tvs_doc[i].held++;
	}
	return doc;
}

LOCAL void src_shut( T_TAD *doc, void *arg )
{
	INT	i;

	(void)arg;
	for ( i = 0; i < TVS_DOC_MAX; i++ ) {
		if ( tvs_doc[i].used && tvs_doc[i].doc == doc && tvs_doc[i].held > 0 ) tvs_doc[i].held--;
	}
}

/* ---------------------------------------------------------------- pictures */

/* "<uuid>_..." or "<uuid>.": the object of a picture the record names, and the resource's name */
LOCAL BOOL href_of( CONST UB *href, TS_UUID *id, CONST UB **p_name )
{
	char	txt[TS_UUID_STRLEN + 1];
	INT	k;

	for ( k = 0; k < TS_UUID_STRLEN; k++ ) {
		if ( href[k] == 0 ) return FALSE;
		txt[k] = (char)href[k];
	}
	txt[k] = 0;
	if ( ts_str_to_uuid(txt, id) < E_OK || ( href[k] != '_' && href[k] != '.' ) ) return FALSE;
	*p_name = href + k;
	return TRUE;
}

LOCAL UB *res_read( CONST TS_UUID *id, CONST UB *name, SZ *p_size )
{
	UB	*buf = NULL, *nb;
	SZ	cap = 0, got = 0, n;
	ID	key = ob_opn_obj(id, OB_OP_READ);

	if ( key <= 0 ) return NULL;
	for (;;) {
		if ( got == cap ) {
			cap = ( cap == 0 ) ? 16384 : cap * 2;
			nb = (UB *)realloc(buf, (size_t)cap + 1);
			if ( nb == NULL ) break;
			buf = nb;
		}
		n = 0;
		if ( ob_rea_res(key, name, (D)got, buf + got, cap - got, &n) < E_OK || n <= 0 ) break;
		got += n;
		tvs_stat.reads++;
	}
	ob_cls_obj(key);
	if ( buf == NULL || got == 0 ) {
		free(buf);
		return NULL;
	}
	*p_size = got;
	return buf;
}

LOCAL ER src_picture( CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h, void *arg )
{
	UB	whole[TAD_NAME_MAX];
	PIC	*pc = NULL;
	TS_UUID	id;
	CONST UB *name;
	UB	*data;
	SZ	size = 0;
	INT	i, k, old = 0, w = 0, h = 0;

	(void)arg;
	if ( href == NULL || href[0] == 0 ) return E_PAR;
	/* "{realId}_0_1.png": a resource of the object being drawn */
	if ( tvs_has_self && strncmp((CONST char *)href, "{realId}", 8) == 0 ) {
		ts_uuid_text(&tvs_self, (char *)whole);
		for ( k = TS_UUID_STRLEN, i = 8; k < (INT)sizeof(whole) - 1 && href[i] != 0; ) whole[k++] = href[i++];
		whole[k] = 0;
		href = whole;
	}
	for ( i = 0; i < TVS_PIC_MAX; i++ ) {
		if ( tvs_pic[i].used && strcmp((CONST char *)tvs_pic[i].name, (CONST char *)href) == 0 ) {
			tvs_pic[i].clock = ++tvs_clock;
			if ( tvs_pic[i].bad ) return E_NOEXS;
			*p_px = tvs_pic[i].px;
			*p_w = tvs_pic[i].w;
			*p_h = tvs_pic[i].h;
			return E_OK;
		}
	}
	for ( i = 0; i < TVS_PIC_MAX && pc == NULL; i++ ) {
		if ( !tvs_pic[i].used ) pc = &tvs_pic[i];
		else if ( tvs_pic[i].clock < tvs_pic[old].clock ) old = i;
	}
	if ( pc == NULL ) {
		pc = &tvs_pic[old];
		free(pc->px);
	}
	memset(pc, 0, sizeof(*pc));
	pc->used = TRUE;
	pc->bad = TRUE;
	pc->clock = ++tvs_clock;
	strncpy((char *)pc->name, (CONST char *)href, sizeof(pc->name) - 1);
	if ( !href_of(href, &id, &name) || ( data = res_read(&id, name, &size) ) == NULL ) return E_NOEXS;
	if ( dp_img_decode(data, size, NULL, 0, &w, &h) >= E_OK && w > 0 && h > 0 ) {
		pc->px = (UW *)malloc(sizeof(UW) * (size_t)w * h);
		if ( pc->px != NULL && dp_img_decode(data, size, pc->px, (SZ)w * h, &w, &h) >= E_OK ) {
			pc->w = w;
			pc->h = h;
			pc->bad = FALSE;
		}
	}
	free(data);
	if ( pc->bad ) return E_OBJ;
	*p_px = pc->px;
	*p_w = pc->w;
	*p_h = pc->h;
	return E_OK;
}

/* ---------------------------------------------------------------- the calls */

LOCAL CONST T_TVSRC	tvs_src = {
	src_name, src_open, src_shut, src_paper, src_picture, src_label,
	src_icon, src_state, NULL, NULL
};

EXPORT void tv_src_init( void )
{
	tv_source(&tvs_src, NULL);
}

EXPORT void tv_src_self( CONST TS_UUID *id )
{
	tvs_has_self = (BOOL)( id != NULL );
	if ( id != NULL ) tvs_self = *id;
}

EXPORT void tv_src_live( CONST TS_UUID *id, INT recno, T_TAD *CONST *p_rec )
{
	tvs_live_rec = ( id != NULL ) ? p_rec : NULL;
	if ( id != NULL ) {
		tvs_live = *id;
		tvs_live_no = ( recno >= 0 ) ? recno : 0;
	}
}

EXPORT void tv_src_host( CONST TS_UUID *id )
{
	TS_UUID	h;
	INT	i;

	memset(&h, 0, sizeof(h));
	if ( id != NULL ) h = *id;
	if ( same(&h, &tvs_host) ) return;
	tvs_host = h;
	/* whether a link is on another volume is said against the host: asked again */
	for ( i = 0; tvs_look != NULL && i < TVS_LOOK_MAX; i++ ) {
		if ( tvs_look[i].used ) look_free(&tvs_look[i]);
	}
}

EXPORT void tv_src_forget( CONST TS_UUID *id )
{
	char	txt[TS_UUID_STRLEN + 1];
	INT	i;

	for ( i = 0; tvs_look != NULL && i < TVS_LOOK_MAX; i++ ) {
		if ( tvs_look[i].used && ( id == NULL || same(&tvs_look[i].target, id) ) ) look_free(&tvs_look[i]);
	}
	for ( i = 0; i < TVS_DOC_MAX; i++ ) {
		if ( tvs_doc[i].used && tvs_doc[i].held == 0 && ( id == NULL || same(&tvs_doc[i].id, id) ) ) {
			doc_free(&tvs_doc[i]);
		}
	}
	/* the pictures beside its records: named "<uuid>_..." or "<uuid>." */
	if ( id != NULL ) ts_uuid_text(id, txt);
	for ( i = 0; i < TVS_PIC_MAX; i++ ) {
		if ( tvs_pic[i].used && ( id == NULL
		  || ( strncmp((CONST char *)tvs_pic[i].name, txt, TS_UUID_STRLEN) == 0
		    && ( tvs_pic[i].name[TS_UUID_STRLEN] == '_' || tvs_pic[i].name[TS_UUID_STRLEN] == '.' ) ) ) ) {
			free(tvs_pic[i].px);
			memset(&tvs_pic[i], 0, sizeof(tvs_pic[i]));
		}
	}
}

EXPORT INT tv_src_look( CONST T_VOBJ *v, UB *name, INT max, UW *p_paper )
{
	LOOK	*k = look_of(v);
	INT	n;

	if ( k == NULL || k->er < E_OK ) return -1;
	if ( p_paper != NULL ) {
		paper_ask(k, v);
		*p_paper = k->paper;
	}
	if ( name == NULL || max <= 0 ) return k->name_len;
	n = ( k->name_len >= 0 && k->name_len < max - 1 ) ? k->name_len : ( k->name_len < 0 ? 0 : max - 1 );
	memcpy(name, k->name, (size_t)n);
	name[n] = 0;
	return n;
}

EXPORT void tv_src_stat( T_TVSRCSTAT *st )
{
	if ( st != NULL ) *st = tvs_stat;
}
