/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	om_store.c
 *	The real objects a program shows and edits (design 16.3.3, 18)
 *
 *	An object is reached by its identity through the basic operations
 *	of the object layer: its metadata, its records, the resources
 *	beside them (om_obj.c). Where they are kept -- the files a TADjs
 *	Desktop writes, on a volume the file manager has attached -- is the
 *	object layer's business, and so is who may read or change them.
 *
 *	What is read is kept. A link is drawn every time the document it
 *	stands in is drawn, and going to the disk for the name and the
 *	content each time means a file opened and a directory walked for
 *	every link on every screen. On a page of twenty links that came to
 *	twenty seconds -- more than everything else the drawing does put
 *	together.
 *
 *	The store owns what it has read until it is emptied. A record that
 *	has been parsed keeps its bytes with it, because the tree points
 *	into them.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/om.h>
#include <ts/tadlnk.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/img.h>
#include <ts/dt.h>
#include <ts/sysdef.h>

/*
 * The table of objects held (design 11.x, om_store): its entries, each
 * an object's record with its document, or its metadata (recno -1) with
 * what a link's band shows. The table is sized once, from the memory
 * the machine has: ST_BUDGET_PERMILLE of it for the entries and their
 * icons, counted at their real size (an icon is as large as it was
 * decoded, larger on a screen of more pixels to the inch). The number
 * of entries is what the budget holds at ST_ENTRY_ICON bytes of icon
 * each, between ST_MIN and ST_LIMIT. When either runs out, an entry
 * nothing is using makes room, the one asked for the longest time ago
 * (slot_of).
 */
#define ST_BUDGET_PERMILLE	5	/* 0.5% of the memory */
#define ST_ENTRY_ICON		1024	/* an icon of 16 x 16, the smallest drawn */
#define ST_MIN			256
#define ST_LIMIT		8192
#define ST_ORPHANS		32	/* icons in use when their entry was let go */
#define ST_KEEP			16	/* the entries asked for last, never made room of */

typedef struct {
	BOOL	used;
	TS_UUID	id;
	INT	recno;			/* -1: the metadata rather than a record */
	T_TAD	*doc;
	UB	*xml;
	SZ	snap;			/* how long its text came to when last written out */
	UB	name[TAD_NAME_MAX];
	BOOL	named;			/* the name has been looked for */
	BOOL	has_name;		/* and was found */
	UW	paper;			/* what it is written on */
	BOOL	has_paper;
	/* what a link's band says of it: its relationship, its program, its date */
	BOOL	described;
	UB	rel[TAD_REL_MAX * 2];
	UB	app[OM_APP_NAME];
	TS_TIME	updated;
	BOOL	has_updated;
	/* its icon */
	BOOL	icon_tried;
	UW	*icon;
	INT	icon_w, icon_h;
	/* the volume it is on, and the volumes' number when that was looked for (0 not yet) */
	ID	vol;
	UW	volser;
	UW	stamp;			/* when it was last asked for (st_clock) */
	INT	icon_pins;		/* drawings holding its icon (store_icon to icon_done) */
	SZ	icon_bytes;
	INT	hnext;			/* the next in its hash chain, or in the free list */
	INT	lprev, lnext;		/* the order of use, the latest first */
} STENT;

LOCAL STENT	*st_tab = NULL;
LOCAL INT	st_max = 0;		/* entries */
LOCAL INT	*st_hash = NULL;	/* the first entry of each chain, -1 none */
LOCAL UW	st_hmask = 0;
LOCAL INT	st_free = -1;		/* unused entries, chained by hnext */
LOCAL INT	st_lru = -1, st_lru_tail = -1;
LOCAL SZ	st_bytes = 0;		/* entries in use and their icons */
LOCAL INT	st_used = 0;		/* entries in use */
LOCAL INT	st_cap = 0;		/* entries at most, for the tests (om_store_cap); 0 the table */
LOCAL SZ	st_budget = 0;
LOCAL UW	st_clock = 0;
LOCAL UW	st_evicted = 0;		/* entries made room of, counted (om_store_stat) */
LOCAL CONST UW	*st_orphan[ST_ORPHANS];	/* icons let go while drawn, freed when done */
LOCAL INT	st_orphan_pins[ST_ORPHANS];

/*
 * Pictures, by the file name the record gives them. Decoding one is the
 * whole of the cost of showing it, and the same picture is shown on
 * every screen the record is on.
 */
/*
 * The pictures are held to a budget of their own, counted at their
 * decoded size (width x height x 4): PIC_BUDGET_PERMILLE of the memory,
 * between PIC_BUDGET_MIN and PIC_BUDGET_MAX, in at most PIC_MAX of them.
 * A picture a drawing holds (picture to picture_done) is never let go;
 * of the others, the one asked for the longest time ago goes first.
 */
#define PIC_MAX			512
#define PIC_NAME		80
#define PIC_BUDGET_PERMILLE	20		/* 2% of the memory */
#define PIC_BUDGET_MIN		( (SZ)16 * 1024 * 1024 )
#define PIC_BUDGET_MAX		( (SZ)256 * 1024 * 1024 )

typedef struct {
	BOOL	used;
	UB	name[PIC_NAME];
	UW	*px;
	INT	w, h;
	BOOL	bad;			/* looked for and not usable */
	BOOL	stale;			/* written anew while drawn: found by no name, freed when done */
	INT	pins;			/* drawings holding px */
	UW	stamp;			/* when it was last asked for */
	SZ	bytes;
} STPIC;

LOCAL STPIC	st_pic[PIC_MAX];
LOCAL SZ	st_pic_bytes = 0;
LOCAL SZ	st_pic_budget = 0;
LOCAL UW	st_pic_clock = 0;
LOCAL UW	st_pic_evicted = 0;

LOCAL BOOL same_id( CONST TS_UUID *a, CONST TS_UUID *b )
{
	CONST UB	*p = (CONST UB *)a;
	CONST UB	*q = (CONST UB *)b;
	UINT		i;

	for ( i = 0; i < sizeof(TS_UUID); i++ ) {
		if ( p[i] != q[i] ) {
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * The table's lock. The desktop's task reads and draws through the store;
 * other tasks reach it too (a process renaming an object through the
 * file system, ファイル変換, the forced quit emptying it), on other
 * processors. Taking an entry, making room and letting entries go are
 * done under it. The lock may be taken again by the task holding it.
 */
LOCAL ID	st_mtx = 0;
LOCAL ID	st_owner = 0;
LOCAL INT	st_depth = 0;
LOCAL ID	st_home = 0;		/* the task that draws through the store (om_store_files) */

LOCAL void st_lock( void )
{
	ID	me = tk_get_tid();

	if ( st_mtx == 0 ) {
		T_CMTX	c;
		ID	id;

		c.exinf = NULL;
		c.mtxatr = TA_INHERIT;
		c.ceilpri = 0;
		id = tk_cre_mtx(&c);
		/* the first to set it wins; tasks on other processors race here */
		if ( id > 0 && atomic_cmpxchg((UW *)&st_mtx, (UW)id, 0) == 0 ) {
			id = 0;
		}
		if ( id > 0 ) tk_del_mtx(id);
	}
	/* only this task ever sets st_owner to itself */
	if ( st_owner == me ) {
		st_depth++;
		return;
	}
	(void)tk_loc_mtx(st_mtx, TMO_FEVR);
	st_owner = me;
	st_depth = 1;
}

LOCAL void st_unlock( void )
{
	if ( --st_depth == 0 ) {
		st_owner = 0;
		(void)tk_unl_mtx(st_mtx);
	}
}

IMPORT UD knl_pf_total_count( INT zone );

LOCAL INT	store_name( CONST T_VOBJ *v, UB *buf, INT max, void *arg );
LOCAL INT	store_label( CONST T_VOBJ *v, UB *buf, INT max, void *arg );
LOCAL ER	store_icon( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h, void *arg );
LOCAL ER	store_paper( CONST T_VOBJ *v, UW *p_colour, void *arg );
LOCAL UINT	store_state( CONST T_VOBJ *v, void *arg );
LOCAL T_TAD	*store_open( CONST T_VOBJ *v, void *arg );
LOCAL void	meta_forget( CONST TS_UUID *id );

/* The table made, on first use, as large as the memory says (with the lock held) */
LOCAL BOOL st_make( void )
{
	UD	mem = knl_pf_total_count(-1) * 4096;	/* pages of 4KB */
	SZ	budget = (SZ)( mem / 1000 * ST_BUDGET_PERMILLE );
	INT	n = (INT)( budget / (SZ)( sizeof(STENT) + ST_ENTRY_ICON ) ), i;
	UW	nb = 1;

	if ( st_tab != NULL ) {
		return TRUE;
	}
	if ( n < ST_MIN ) n = ST_MIN;
	if ( n > ST_LIMIT ) n = ST_LIMIT;
	while ( nb < (UW)n ) nb <<= 1;
	for ( ; n >= ST_MIN / 2; n /= 2 ) {
		st_tab = (STENT *)Kcalloc((SZ)n, sizeof(STENT));
		st_hash = (INT *)Kmalloc((SZ)nb * sizeof(INT));
		if ( st_tab != NULL && st_hash != NULL ) break;
		if ( st_tab != NULL ) Kfree(st_tab);
		if ( st_hash != NULL ) Kfree(st_hash);
		st_tab = NULL;
		st_hash = NULL;
	}
	if ( st_tab == NULL ) {
		return FALSE;
	}
	st_max = n;
	st_hmask = nb - 1;
	st_budget = ( budget > (SZ)n * (SZ)sizeof(STENT) ) ? budget : (SZ)n * (SZ)sizeof(STENT);
	for ( i = 0; i < (INT)nb; i++ ) st_hash[i] = -1;
	for ( i = 0; i < n; i++ ) st_tab[i].hnext = ( i + 1 < n ) ? i + 1 : -1;
	st_free = 0;
	st_lru = st_lru_tail = -1;
	st_bytes = 0;
	st_used = 0;
	st_pic_budget = (SZ)( mem / 1000 * PIC_BUDGET_PERMILLE );
	if ( st_pic_budget < PIC_BUDGET_MIN ) st_pic_budget = PIC_BUDGET_MIN;
	if ( st_pic_budget > PIC_BUDGET_MAX ) st_pic_budget = PIC_BUDGET_MAX;
	return TRUE;
}

LOCAL UW st_hash_of( CONST TS_UUID *id, INT recno )
{
	CONST UB	*b = (CONST UB *)id;
	UW		h = 2166136261U;
	UINT		i;

	for ( i = 0; i < sizeof(TS_UUID); i++ ) {
		h = ( h ^ b[i] ) * 16777619U;
	}
	return ( h ^ (UW)recno ) & st_hmask;
}

LOCAL void lru_unlink( INT i )
{
	STENT	*e = &st_tab[i];

	if ( e->lprev >= 0 ) st_tab[e->lprev].lnext = e->lnext; else st_lru = e->lnext;
	if ( e->lnext >= 0 ) st_tab[e->lnext].lprev = e->lprev; else st_lru_tail = e->lprev;
	e->lprev = e->lnext = -1;
}

LOCAL void lru_front( INT i )
{
	STENT	*e = &st_tab[i];

	e->lprev = -1;
	e->lnext = st_lru;
	if ( st_lru >= 0 ) st_tab[st_lru].lprev = i; else st_lru_tail = i;
	st_lru = i;
}

/* An icon a drawing still holds, kept until it says it is done (icon_done) */
LOCAL void icon_orphan( CONST UW *px, INT pins )
{
	INT	k;

	for ( k = 0; k < ST_ORPHANS; k++ ) {
		if ( st_orphan[k] == NULL ) {
			st_orphan[k] = px;
			st_orphan_pins[k] = pins;
			return;
		}
	}
	/* no room to remember it: left allocated rather than freed under a drawing */
}

/* Entry i let go: out of its chain and the order of use, onto the free list */
LOCAL void st_release( INT i )
{
	STENT	*e = &st_tab[i];
	UW	h = st_hash_of(&e->id, e->recno);
	INT	*pp = &st_hash[h];

	while ( *pp >= 0 && *pp != i ) pp = &st_tab[*pp].hnext;
	if ( *pp == i ) *pp = e->hnext;
	lru_unlink(i);
	if ( e->doc != NULL ) tad_free(e->doc);
	if ( e->xml != NULL ) Kfree(e->xml);
	if ( e->icon != NULL ) {
		if ( e->icon_pins > 0 ) {
			icon_orphan(e->icon, e->icon_pins);
		} else {
			Kfree(e->icon);
		}
	}
	st_bytes -= (SZ)sizeof(STENT) + e->icon_bytes;
	st_used--;
	e->used = FALSE;
	e->doc = NULL;
	e->xml = NULL;
	e->icon = NULL;
	e->icon_bytes = 0;
	e->icon_pins = 0;
	e->hnext = st_free;
	st_free = i;
}

/*
 * The entry asked for the longest time ago that nothing is using: no
 * document (a window may be showing it), no drawing holding its icon,
 * and not among the last ST_KEEP asked for. -1 if there is none.
 */
LOCAL INT st_victim( void )
{
	INT	i;

	for ( i = st_lru_tail; i >= 0; i = st_tab[i].lprev ) {
		STENT	*e = &st_tab[i];

		if ( (INT)( st_clock - e->stamp ) < ST_KEEP ) {
			return -1;		/* the rest are newer still */
		}
		if ( e->doc == NULL && e->xml == NULL && e->icon_pins == 0 ) {
			return i;
		}
	}
	return -1;
}

/* Room made for bytes more by the task that may make it; FALSE if not */
LOCAL BOOL st_room( SZ more )
{
	BOOL	home = (BOOL)( st_home == 0 || tk_get_tid() == st_home );

	while ( st_free < 0 || st_bytes + more > st_budget
	     || ( st_cap > 0 && more == (SZ)sizeof(STENT) && st_used >= st_cap ) ) {
		INT	v = home ? st_victim() : -1;

		if ( v < 0 ) {
			return (BOOL)( st_free >= 0 );	/* over the budget, but not by an entry */
		}
		st_release(v);
		st_evicted++;
	}
	return TRUE;
}

/*
 * The entry of an object's record (-1 its metadata), found or taken.
 * Taking one when the table is full makes room (st_room): only the task
 * that draws through the store does, so another task never has an entry
 * it is using let go under it, and only of an entry nothing is using.
 * Everything that reads or writes an entry holds the table's lock for
 * the whole of it (the store_ hooks, the om_store_ calls), and an icon
 * handed out stays until the drawing says it is done with it.
 */
LOCAL INT slot_of( CONST TS_UUID *id, INT recno )
{
	INT	i;
	UW	h;
	STENT	*e;

	st_lock();
	if ( !st_make() ) {
		st_unlock();
		return -1;
	}
	h = st_hash_of(id, recno);
	for ( i = st_hash[h]; i >= 0; i = st_tab[i].hnext ) {
		if ( st_tab[i].recno == recno && same_id(&st_tab[i].id, id) ) {
			st_tab[i].stamp = ++st_clock;
			lru_unlink(i);
			lru_front(i);
			st_unlock();
			return i;
		}
	}
	if ( !st_room((SZ)sizeof(STENT)) ) {
		st_unlock();
		return -1;
	}
	i = st_free;
	e = &st_tab[i];
	st_free = e->hnext;
	knl_memset(e, 0, sizeof(*e));
	e->used     = TRUE;
	e->id       = *id;
	e->recno    = recno;
	e->stamp    = ++st_clock;
	e->hnext    = st_hash[h];
	st_hash[h]  = i;
	lru_front(i);
	st_bytes += (SZ)sizeof(STENT);
	st_used++;
	st_unlock();

	return i;
}

/* ---------------------------------------------------------------- reading */

/*
 * What an object is called. The metadata is JSON and the name is under
 * "name"; it is read out by hand rather than with a parser, because one
 * key out of a file this layer does not otherwise care about is not
 * worth a parser.
 */
/*
 * Whether the text at p is that key, quote and all. The metadata is
 * read by hand rather than parsed, so the match is written out: a key
 * is its letters followed by the quote that ends it, which is what
 * keeps "background" from matching "backgroundColor".
 */
LOCAL BOOL key_is( CONST UB *p, INT left, CONST char *key )
{
	INT	i;

	for ( i = 0; key[i] != 0; i++ ) {
		if ( i >= left || p[i] != (UB)key[i] ) {
			return FALSE;
		}
	}

	return (BOOL)( i < left && p[i] == '"' );
}

/*
 * A colour written as "#rrggbb" in the metadata. Answers below E_OK when
 * the text is not a colour, which is not a fault: an object that does
 * not say is written on the system's own paper.
 */
LOCAL ER hex_colour( CONST UB *p, INT n, UW *p_col )
{
	UW	v = 0;
	INT	i;

	if ( n < 7 || p[0] != '#' ) {
		return E_PAR;
	}
	for ( i = 1; i < 7; i++ ) {
		UB	c = p[i];
		UW	d;

		if ( c >= '0' && c <= '9' )      d = (UW)( c - '0' );
		else if ( c >= 'a' && c <= 'f' ) d = (UW)( c - 'a' + 10 );
		else if ( c >= 'A' && c <= 'F' ) d = (UW)( c - 'A' + 10 );
		else                             return E_PAR;
		v = ( v << 4 ) | d;
	}
	*p_col = v;

	return E_OK;
}

/*
 * What the object is written on: "backgroundColor" in its metadata.
 * The key is looked for by hand for the same reason the name is -- two
 * keys out of a file this layer does not otherwise care about are not
 * worth a parser.
 */
LOCAL ER store_paper_u( CONST T_VOBJ *v, UW *p_colour, void *arg )
{
	UB	*meta;
	SZ	size = 0;
	INT	i, slot;
	ER	er = E_NOEXS;

	slot = slot_of(&v->target, -1);
	if ( slot >= 0 && st_tab[slot].has_paper ) {
		if ( p_colour != NULL ) {
			*p_colour = st_tab[slot].paper;
		}
		return E_OK;
	}
	meta = om_obj_meta(&v->target, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	for ( i = 0; i + 24 < (INT)size; i++ ) {
		if ( meta[i] != '"' || meta[i + 1] != 'b' ) {
			continue;
		}
		if ( !key_is(meta + i + 1, (INT)size - i - 1, "backgroundColor") ) {
			continue;
		}
		{
			INT	j = i + 17;
			UW	col = 0;

			while ( j < (INT)size && meta[j] != '"' ) {
				j++;		/* past the colon and the space */
			}
			j++;
			if ( j + 7 <= (INT)size
			  && hex_colour(meta + j, 7, &col) >= E_OK ) {
				if ( p_colour != NULL ) {
					*p_colour = col;
				}
				if ( slot >= 0 ) {
					st_tab[slot].paper = col;
					st_tab[slot].has_paper = TRUE;
				}
				er = E_OK;
			}
		}
		break;
	}
	Kfree(meta);

	return er;
}

LOCAL ER store_paper( CONST T_VOBJ *v, UW *p_colour, void *arg )
{
	ER r;

	st_lock();
	r = store_paper_u(v, p_colour, arg);
	st_unlock();
	return r;
}

LOCAL INT store_name_u( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	UB	*meta;
	SZ	size = 0;
	INT	i, n = 0, slot;

	if ( max < TAD_NAME_MAX ) {
		/* what is kept is the whole name, whatever room this caller has */
		UB	full[TAD_NAME_MAX];

		n = store_name(v, full, TAD_NAME_MAX, arg);
		if ( n < 0 ) {
			return n;
		}
		for ( i = 0; i < n && i < max - 1; i++ ) {
			buf[i] = full[i];
		}
		buf[i] = 0;
		return i;
	}
	slot = slot_of(&v->target, -1);
	if ( slot >= 0 && st_tab[slot].named ) {
		if ( !st_tab[slot].has_name ) {
			return -1;
		}
		for ( n = 0; n < max - 1 && st_tab[slot].name[n] != 0; n++ ) {
			buf[n] = st_tab[slot].name[n];
		}
		buf[n] = 0;
		return n;
	}
	meta = om_obj_meta(&v->target, &size);
	if ( meta == NULL ) {
		if ( slot >= 0 ) {
			st_tab[slot].named = TRUE;
		}
		return -1;
	}
	for ( i = 0; i + 8 < (INT)size; i++ ) {
		if ( meta[i] == '"' && meta[i + 1] == 'n' && meta[i + 2] == 'a'
		  && meta[i + 3] == 'm' && meta[i + 4] == 'e'
		  && meta[i + 5] == '"' ) {
			INT	j = i + 6;

			while ( j < (INT)size && meta[j] != '"' ) {
				j++;		/* past the colon and the space */
			}
			j++;
			while ( j < (INT)size && meta[j] != '"' && n < max - 1 ) {
				buf[n++] = meta[j++];
			}
			break;
		}
	}
	buf[n] = 0;
	Kfree(meta);
	if ( slot >= 0 ) {
		INT	k;

		for ( k = 0; k < n && k < TAD_NAME_MAX - 1; k++ ) {
			st_tab[slot].name[k] = buf[k];
		}
		st_tab[slot].name[k] = 0;
		st_tab[slot].named = TRUE;
		st_tab[slot].has_name = (BOOL)( n > 0 );
	}

	return ( n > 0 ) ? n : -1;
}

LOCAL INT store_name( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	INT r;

	st_lock();
	r = store_name_u(v, buf, max, arg);
	st_unlock();
	return r;
}

LOCAL T_TAD *store_open_u( CONST T_VOBJ *v, void *arg )
{
	UB	*xml;
	SZ	size = 0;
	T_TAD	*doc = NULL;
	INT	recno = ( v->recno >= 0 ) ? v->recno : 0;
	INT	slot;

	slot = slot_of(&v->target, recno);
	if ( slot >= 0 && st_tab[slot].doc != NULL ) {
		return st_tab[slot].doc;	/* read already */
	}
	xml = om_obj_record(&v->target, recno, &size);
	if ( xml == NULL ) {
		return NULL;
	}
	if ( tad_parse(xml, size, NULL, &doc) < E_OK || doc == NULL ) {
		Kfree(xml);
		return NULL;
	}
	if ( slot < 0 ) {
		/* nowhere to keep it, and the tree points into the bytes */
		tad_free(doc);
		Kfree(xml);
		return NULL;
	}
	st_tab[slot].doc = doc;
	st_tab[slot].xml = xml;
	st_tab[slot].snap = size;	/* about how long it comes to written out */

	return doc;
}

LOCAL T_TAD *store_open( CONST T_VOBJ *v, void *arg )
{
	T_TAD *r;

	st_lock();
	r = store_open_u(v, arg);
	st_unlock();
	return r;
}

/*
 * Done with for now. What was read stays: the same link will be drawn
 * again on the next screen.
 */
LOCAL void store_shut( T_TAD *doc, void *arg )
{
	(void)doc;
	(void)arg;
}

/*
 * A picture the record names, read from beside the record and kept. A
 * name that could not be read or decoded is remembered as such, so that
 * a missing picture is looked for once and not on every screen.
 */
LOCAL TS_UUID	st_self;
LOCAL BOOL	st_has_self = FALSE;

EXPORT void om_store_self( CONST TS_UUID *id )
{
	st_has_self = (BOOL)( id != NULL );
	if ( id != NULL ) {
		st_self = *id;
	}
}

/* Picture i let go: its pixels freed, or kept until the last drawing holding them is done */
LOCAL void pic_release( INT i, BOOL drop )
{
	STPIC	*pc = &st_pic[i];

	if ( pc->pins > 0 ) {
		pc->stale = TRUE;		/* no name finds it any more */
		pc->name[0] = 0;
		return;
	}
	if ( pc->px != NULL ) {
		Kfree(pc->px);
	}
	st_pic_bytes -= pc->bytes;
	if ( drop ) {
		st_pic_evicted++;
	}
	knl_memset(pc, 0, sizeof(*pc));
}

/* Room for bytes more: pictures nothing holds let go, the longest unasked first */
LOCAL void pic_room( SZ more, INT keep )
{
	for ( ;; ) {
		INT	i, v = -1, n = 0;

		for ( i = 0; i < PIC_MAX; i++ ) {
			if ( st_pic[i].used ) n++;
		}
		if ( n < PIC_MAX && st_pic_bytes + more <= st_pic_budget ) {
			return;
		}
		for ( i = 0; i < PIC_MAX; i++ ) {
			STPIC	*pc = &st_pic[i];

			if ( !pc->used || pc->pins > 0 || pc->stale || i == keep ) continue;
			if ( v < 0 || (INT)( pc->stamp - st_pic[v].stamp ) < 0 ) v = i;
		}
		if ( v < 0 ) {
			return;			/* every one held: over the budget until they are done */
		}
		pic_release(v, TRUE);
	}
}

LOCAL ER store_picture_u( CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h )
{
	UB	*data;
	SZ	size = 0;
	INT	i, k, free = -1;
	STPIC	*pc;
	TS_UUID	id;
	CONST UB *name;
	UB	whole[PIC_NAME];

	if ( href == NULL || href[0] == 0 ) {
		return E_PAR;
	}
	(void)st_make();
	/* "{realId}_0_1.png": a resource of the object being drawn */
	if ( st_has_self && href[0] == '{' && href[1] == 'r' && href[2] == 'e' && href[3] == 'a'
	  && href[4] == 'l' && href[5] == 'I' && href[6] == 'd' && href[7] == '}' ) {
		if ( ts_uuid_to_str(&st_self, (char *)whole, sizeof(whole)) >= E_OK ) {
			for ( k = TS_UUID_STRLEN, i = 8; k < PIC_NAME - 1 && href[i] != 0; ) {
				whole[k++] = href[i++];
			}
			whole[k] = 0;
			href = whole;
		}
	}
	for ( i = 0; i < PIC_MAX; i++ ) {
		if ( !st_pic[i].used || st_pic[i].stale ) {
			continue;
		}
		for ( k = 0; k < PIC_NAME - 1 && href[k] != 0; k++ ) {
			if ( st_pic[i].name[k] != href[k] ) {
				break;
			}
		}
		if ( href[k] == 0 && st_pic[i].name[k] == 0 ) {
			st_pic[i].stamp = ++st_pic_clock;
			if ( st_pic[i].bad ) {
				return E_NOEXS;
			}
			st_pic[i].pins++;
			*p_px = st_pic[i].px;
			*p_w = st_pic[i].w;
			*p_h = st_pic[i].h;
			return E_OK;
		}
	}
	pic_room(0, -1);
	for ( i = 0; i < PIC_MAX && free < 0; i++ ) {
		if ( !st_pic[i].used ) free = i;
	}
	if ( free < 0 ) {
		return E_LIMIT;			/* every one held by a drawing */
	}
	pc = &st_pic[free];
	knl_memset(pc, 0, sizeof(*pc));
	pc->used = TRUE;
	pc->bad = TRUE;
	pc->stamp = ++st_pic_clock;
	for ( k = 0; k < PIC_NAME - 1 && href[k] != 0; k++ ) {
		pc->name[k] = href[k];
	}
	pc->name[k] = 0;

	/* "<uuid>_0_1.png": a resource of that object */
	if ( !om_obj_href(href, &id, &name) ) {
		return E_NOEXS;
	}
	data = om_obj_res(&id, name, &size);
	if ( data == NULL ) {
		return E_NOEXS;
	}
	if ( img_png_decode(data, size, &pc->px, &pc->w, &pc->h) >= E_OK ) {
		pc->bad = FALSE;
		pc->bytes = (SZ)pc->w * (SZ)pc->h * 4;
		st_pic_bytes += pc->bytes;
	}
	Kfree(data);
	if ( pc->bad ) {
		return E_OBJ;
	}
	pc->pins = 1;
	pic_room(0, free);		/* the others make room for it, if any can */
	*p_px = pc->px;
	*p_w = pc->w;
	*p_h = pc->h;

	return E_OK;
}

/*
 * A picture the record names, read from beside the record and kept. A
 * name that could not be read or decoded is remembered as such, so that
 * a missing picture is looked for once and not on every screen. The
 * pixels are the caller's to read until it says it is done with them
 * (store_picture_done, om_store_picture_done).
 */
LOCAL ER store_picture( CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h,
			void *arg )
{
	ER	er;

	st_lock();
	er = store_picture_u(href, p_px, p_w, p_h);
	st_unlock();
	return er;
}

/* The drawing done with the pixels of a picture store_picture gave it */
LOCAL void store_picture_done( CONST UW *px, void *arg )
{
	INT	i;

	if ( px == NULL ) {
		return;
	}
	st_lock();
	for ( i = 0; i < PIC_MAX; i++ ) {
		STPIC	*pc = &st_pic[i];

		if ( pc->used && pc->px == px && pc->pins > 0 ) {
			if ( --pc->pins == 0 && pc->stale ) {
				pic_release(i, FALSE);
			}
			break;
		}
	}
	st_unlock();
}

LOCAL INT store_label( CONST T_VOBJ *v, UB *buf, INT max, void *arg );
LOCAL ER store_icon( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h, void *arg );
LOCAL UINT store_state( CONST T_VOBJ *v, void *arg );

LOCAL void store_icon_done( CONST UW *px, void *arg );

LOCAL CONST T_TVSRC	om_files = { store_name, store_open, store_shut,
				     store_paper, store_picture, store_label,
				     store_icon, store_state, store_icon_done,
				     store_picture_done };

/*
 * The volume an object is on, remembered with the rest of it and looked
 * for again when the volumes attached have changed (0: on none).
 */
LOCAL ID store_vol( CONST TS_UUID *id )
{
	UW	ser = knl_obfile_serial() + 1;
	INT	slot = slot_of(id, -1);
	ID	vol;

	if ( slot >= 0 && st_tab[slot].volser == ser ) {
		return st_tab[slot].vol;
	}
	vol = knl_obfile_vol_of(id);
	if ( vol < 0 ) {
		vol = 0;
	}
	if ( slot >= 0 ) {
		st_tab[slot].vol = vol;
		st_tab[slot].volser = ser;
	}
	return vol;
}

/*
 * A link whose object cannot be found is a 虚身; one in a window whose
 * object is on another volume than its own is marked so.
 */
LOCAL UINT state_on_u( CONST T_VOBJ *v, CONST TS_UUID *host )
{
	UB	nm[8];
	UINT	st = 0;
	ID	vt;

	if ( store_name(v, nm, sizeof(nm), NULL) < 0 ) {
		return TV_ST_GONE;
	}
	if ( host != NULL && ( vt = store_vol(&v->target) ) > 0 ) {
		ID	vh = store_vol(host);

		if ( vh > 0 && vh != vt ) {
			st |= TV_ST_OTHER;
		}
	}
	return st;
}

/* Against the object whose window the task asking is drawing (tv_draw_host) */
LOCAL UINT store_state_u( CONST T_VOBJ *v, void *arg )
{
	TS_UUID	host;

	(void)arg;
	return state_on_u(v, tv_host_of(&host) ? &host : NULL);
}

LOCAL UINT store_state( CONST T_VOBJ *v, void *arg )
{
	UINT r;

	st_lock();
	r = store_state_u(v, arg);
	st_unlock();
	return r;
}

/* The same against an object named, for a process that draws the window itself (ob_ref_vob) */
EXPORT UINT om_store_state_on( CONST T_VOBJ *v, CONST TS_UUID *host )
{
	UINT r;

	if ( v == NULL ) {
		return TV_ST_GONE;
	}
	st_lock();
	r = state_on_u(v, host);
	st_unlock();
	return r;
}

/*
 * What is remembered of objects that cannot be found now let go of --
 * the disk they were on went -- and of those remembered as not found,
 * which may be found now that a disk came. For when the volumes
 * attached change; the windows of what went are closed first.
 */
EXPORT void om_store_recheck( void )
{
	T_OBREF	*r = (T_OBREF *)Kmalloc(sizeof(T_OBREF));
	INT	i;

	st_lock();
	for ( i = 0; i < st_max; i++ ) {
		BOOL	absent;

		if ( !st_tab[i].used ) {
			continue;
		}
		absent = (BOOL)( ( st_tab[i].named && !st_tab[i].has_name )
				 || ( r != NULL && ob_ref_obj(&st_tab[i].id, r) == E_NOEXS ) );
		st_tab[i].volser = 0;
		if ( absent ) {
			om_store_forget(&st_tab[i].id);
		}
	}
	st_unlock();
	if ( r != NULL ) Kfree(r);
}

/*
 * Whether om_store_recheck lets go of what is held of the object: it
 * can no longer be found, its disk taken out or the object thrown away.
 * Whoever keeps a record of the store's past that lets go of it first.
 */
EXPORT BOOL om_store_lost( CONST TS_UUID *id )
{
	T_OBREF	*r;
	BOOL	lost;

	if ( id == NULL || ( r = (T_OBREF *)Kmalloc(sizeof(T_OBREF)) ) == NULL ) {
		return FALSE;
	}
	lost = (BOOL)( ob_ref_obj(id, r) == E_NOEXS );
	Kfree(r);
	return lost;
}

/*
 * A record taken out of the store: the caller's from now on, freed with
 * tad_free, and the store keeps no document of it. For a window that
 * stays open showing an object whose disk went, which the store lets go
 * of at the next om_store_recheck. NULL when the store holds none.
 */
EXPORT T_TAD *om_store_take( CONST TS_UUID *id, INT recno )
{
	T_TAD	*doc = NULL;
	INT	slot;

	if ( id == NULL ) {
		return NULL;
	}
	st_lock();
	slot = slot_of(id, recno);
	if ( slot >= 0 && st_tab[slot].doc != NULL ) {
		doc = st_tab[slot].doc;
		st_tab[slot].doc = NULL;
		if ( st_tab[slot].xml != NULL ) {
			Kfree(st_tab[slot].xml);
			st_tab[slot].xml = NULL;
		}
	}
	st_unlock();
	return doc;
}

/* A picture beside the records: its pixels, the store's, not to be freed */
EXPORT ER om_store_picture( CONST UB *href, CONST UW **p_px, INT *p_w, INT *p_h )
{
	return store_picture(href, p_px, p_w, p_h, NULL);
}

/* Done with the pixels om_store_picture gave: they may be let go from now on */
EXPORT void om_store_picture_done( CONST UW *px )
{
	store_picture_done(px, NULL);
}

/* How many pictures, their bytes, the budget and how many were let go, for the tests */
EXPORT void om_store_picture_stat( INT *p_n, SZ *p_bytes, SZ *p_budget, UW *p_evicted )
{
	INT	i, n = 0;

	st_lock();
	(void)st_make();
	for ( i = 0; i < PIC_MAX; i++ ) {
		if ( st_pic[i].used ) n++;
	}
	if ( p_n != NULL ) *p_n = n;
	if ( p_bytes != NULL ) *p_bytes = st_pic_bytes;
	if ( p_budget != NULL ) *p_budget = st_pic_budget;
	if ( p_evicted != NULL ) *p_evicted = st_pic_evicted;
	st_unlock();
}

/* The picture budget held to bytes (0: as made), for the tests */
EXPORT void om_store_picture_cap( SZ bytes )
{
	LOCAL SZ	made = 0;

	st_lock();
	(void)st_make();
	if ( made == 0 ) made = st_pic_budget;
	st_pic_budget = ( bytes > 0 ) ? bytes : made;
	st_unlock();
}

/*
 * A picture written beside the records under a name, as a PNG, and what
 * is kept of it in memory made that picture, so that it is drawn as it
 * now is.
 */
EXPORT ER om_store_put_picture( CONST UB *href, CONST UW *px, INT w, INT h )
{
	UB	*png = NULL;
	SZ	len = 0;
	INT	i, k;
	TS_UUID	id;
	CONST UB *name;
	ER	er;

	if ( href == NULL || href[0] == 0 || px == NULL ) {
		return E_PAR;
	}
	if ( !om_obj_href(href, &id, &name) ) {
		return E_PAR;			/* not a name an object gives */
	}
	er = img_png_encode(px, w, h, &png, &len);
	if ( er < E_OK ) {
		return er;
	}
	er = om_obj_res_put(&id, name, png, len);
	Kfree(png);
	/*
	 * What was kept of it goes, and is read again as it now is. A
	 * drawing still holding the old pixels keeps them until it is done.
	 */
	st_lock();
	for ( i = 0; i < PIC_MAX; i++ ) {
		if ( !st_pic[i].used || st_pic[i].stale ) {
			continue;
		}
		for ( k = 0; k < PIC_NAME - 1 && href[k] != 0; k++ ) {
			if ( st_pic[i].name[k] != href[k] ) {
				break;
			}
		}
		if ( href[k] == 0 && st_pic[i].name[k] == 0 ) {
			pic_release(i, FALSE);
		}
	}
	st_unlock();

	return er;
}

/* ---------------------------------------------------------------- the window */

/*
 * Where a key's value starts, looking between 'from' and 'to' only, or
 * -1. A key is its name in quotes followed by a colon; the name in
 * quotes as a value is not a key.
 */
LOCAL INT json_key( CONST UB *b, INT from, INT to, CONST char *key )
{
	INT	i, j;

	for ( i = from; i + 2 < to; i++ ) {
		if ( b[i] != '"' || !key_is(b + i + 1, to - i - 1, key) ) {
			continue;
		}
		for ( j = i + 1; j < to && b[j] != '"'; j++ ) {
			;
		}
		for ( j++; j < to && ( b[j] == ' ' || b[j] == '\t' ); j++ ) {
			;
		}
		if ( j >= to || b[j] != ':' ) {
			continue;
		}
		for ( j++; j < to && ( b[j] == ' ' || b[j] == '\t'
				    || b[j] == '\r' || b[j] == '\n' ); j++ ) {
			;
		}
		return ( j < to ) ? j : -1;
	}

	return -1;
}

/* Where the object or list starting at 'at' ends, just past its bracket */
LOCAL INT json_close( CONST UB *b, INT at, INT to )
{
	INT	depth = 0, i;
	BOOL	in_str = FALSE;

	for ( i = at; i < to; i++ ) {
		if ( in_str ) {
			if ( b[i] == '"' && b[i - 1] != 0x5C ) {
				in_str = FALSE;
			}
			continue;
		}
		if ( b[i] == '"' ) {
			in_str = TRUE;
		} else if ( b[i] == '{' || b[i] == '[' ) {
			depth++;
		} else if ( b[i] == '}' || b[i] == ']' ) {
			if ( --depth == 0 ) {
				return i + 1;
			}
		}
	}

	return to;
}

/* A number, whole part only; FALSE when there is none there */
LOCAL BOOL json_int( CONST UB *b, INT at, INT to, INT *p_v )
{
	INT	v = 0;
	BOOL	neg = FALSE, any = FALSE;

	if ( at < to && b[at] == '-' ) {
		neg = TRUE;
		at++;
	}
	while ( at < to && b[at] >= '0' && b[at] <= '9' ) {
		v = v * 10 + ( b[at] - '0' );
		at++;
		any = TRUE;
	}
	if ( any ) {
		*p_v = neg ? -v : v;
	}

	return any;
}

/*
 * Where the value of one key of the window object stands, or -1: of the
 * object in it named sub ("pos", "panelpos"), or of its own when sub is NULL
 */
LOCAL INT win_key( CONST UB *b, INT size, CONST char *key, CONST char *sub,
		   INT *p_end )
{
	INT	w, we, at, ae;

	w = json_key(b, 0, size, "window");
	if ( w < 0 || b[w] != '{' ) {
		return -1;
	}
	we = json_close(b, w, size);
	at = w + 1;
	ae = we;
	if ( sub != NULL ) {
		INT	p = json_key(b, w + 1, we, sub);

		if ( p < 0 || b[p] != '{' ) {
			return -1;
		}
		at = p + 1;
		ae = json_close(b, p, we);
	} else {
		/* the window's own, not the ones inside what it holds */
		INT	p = json_key(b, w + 1, we, "pos");

		if ( p >= 0 && b[p] == '{' ) {
			INT	pe = json_close(b, p, we);
			INT	k = json_key(b, w + 1, p, key);

			if ( k >= 0 ) {
				*p_end = p;
				return k;
			}
			at = pe;
		}
	}
	*p_end = ae;

	return json_key(b, at, ae, key);
}

EXPORT ER om_store_window( CONST TS_UUID *id, T_DPRECT *r )
{
	UB	*meta;
	SZ	size = 0;
	INT	x = 0, y = 0, w = 0, h = 0, at, end;
	BOOL	ok;

	if ( id == NULL || r == NULL ) {
		return E_PAR;
	}
	meta = om_obj_meta(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	at = win_key(meta, (INT)size, "x", "pos", &end);
	ok = ( at >= 0 && json_int(meta, at, end, &x) );
	at = win_key(meta, (INT)size, "y", "pos", &end);
	ok = ok && at >= 0 && json_int(meta, at, end, &y);
	at = win_key(meta, (INT)size, "width", NULL, &end);
	ok = ok && at >= 0 && json_int(meta, at, end, &w);
	at = win_key(meta, (INT)size, "height", NULL, &end);
	ok = ok && at >= 0 && json_int(meta, at, end, &h);
	Kfree(meta);
	if ( !ok || w <= 0 || h <= 0 ) {
		return E_NOEXS;
	}
	r->left = x;
	r->top = y;
	r->right = x + w;
	r->bottom = y + h;

	return E_OK;
}

/* One number of the window object put in place of what was there */
LOCAL BOOL win_put( UB **p_meta, SZ *p_size, CONST char *key, CONST char *sub,
		    INT v )
{
	UB	*b = *p_meta, *nb;
	INT	at, end, e, n = 0, k;
	char	txt[16];
	SZ	nsize;
	UINT	u = ( v < 0 ) ? (UINT)-v : (UINT)v;

	at = win_key(b, (INT)*p_size, key, sub, &end);
	if ( at < 0 ) {
		return FALSE;
	}
	e = at;
	if ( e < end && b[e] == '-' ) {
		e++;
	}
	while ( e < end && ( ( b[e] >= '0' && b[e] <= '9' ) || b[e] == '.' ) ) {
		e++;
	}
	do {
		txt[n++] = (char)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 && n < 12 );
	if ( v < 0 ) {
		txt[n++] = '-';
	}
	nsize = *p_size - (SZ)( e - at ) + (SZ)n;
	nb = (UB *)Kmalloc(nsize + 1);
	if ( nb == NULL ) {
		return FALSE;
	}
	knl_memcpy(nb, b, (SZ)at);
	for ( k = 0; k < n; k++ ) {
		nb[at + k] = (UB)txt[n - 1 - k];
	}
	knl_memcpy(nb + at + n, b + e, *p_size - (SZ)e);
	nb[nsize] = 0;
	Kfree(b);
	*p_meta = nb;
	*p_size = nsize;

	return TRUE;
}

/*
 * Where a window showing the object stands, kept in its metadata so
 * that it opens there next time. Only the numbers change; every other
 * byte of the file is written back as it was read.
 */
EXPORT ER om_store_set_window( CONST TS_UUID *id, CONST T_DPRECT *r )
{
	UB	*meta;
	SZ	size = 0;
	ER	er;

	if ( id == NULL || r == NULL ) {
		return E_PAR;
	}
	meta = om_obj_meta(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	win_put(&meta, &size, "x", "pos", r->left);
	win_put(&meta, &size, "y", "pos", r->top);
	win_put(&meta, &size, "width", NULL, r->right - r->left);
	win_put(&meta, &size, "height", NULL, r->bottom - r->top);
	er = om_obj_meta_put(id, meta, size);
	Kfree(meta);

	return er;
}

/*
 * Where the tool panel of a window showing the object was left, kept in
 * its metadata as TADjs keeps it ("window": {"panelpos": {"x", "y"}}, the
 * panel window's corner on the screen): E_NOEXS when it has none.
 */
EXPORT ER om_store_panelpos( CONST TS_UUID *id, INT *p_x, INT *p_y )
{
	UB	*meta;
	SZ	size = 0;
	INT	at, end;
	BOOL	ok;

	if ( id == NULL || p_x == NULL || p_y == NULL ) {
		return E_PAR;
	}
	meta = om_obj_meta(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	at = win_key(meta, (INT)size, "x", "panelpos", &end);
	ok = ( at >= 0 && json_int(meta, at, end, p_x) );
	at = win_key(meta, (INT)size, "y", "panelpos", &end);
	ok = ok && at >= 0 && json_int(meta, at, end, p_y);
	Kfree(meta);

	return ok ? E_OK : E_NOEXS;
}

/* The tool panel's place kept: only the two numbers change, where the metadata has them */
EXPORT ER om_store_set_panelpos( CONST TS_UUID *id, INT x, INT y )
{
	UB	*meta;
	SZ	size = 0;
	ER	er;

	if ( id == NULL ) {
		return E_PAR;
	}
	meta = om_obj_meta(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	if ( !win_put(&meta, &size, "x", "panelpos", x) || !win_put(&meta, &size, "y", "panelpos", y) ) {
		Kfree(meta);
		return E_NOEXS;
	}
	er = om_obj_meta_put(id, meta, size);
	Kfree(meta);

	return er;
}

/*
 * Whether the object opens with a program: its applist names that
 * program and marks it as the one to open with.
 */
EXPORT BOOL om_store_opens_with( CONST TS_UUID *id, CONST char *app )
{
	UB	*meta;
	SZ	size = 0;
	INT	a, ae, p, pe, d;
	BOOL	yes = FALSE;

	if ( id == NULL || app == NULL ) {
		return FALSE;
	}
	meta = om_obj_meta(id, &size);
	if ( meta == NULL ) {
		return FALSE;
	}
	a = json_key(meta, 0, (INT)size, "applist");
	if ( a >= 0 && meta[a] == '{' ) {
		ae = json_close(meta, a, (INT)size);
		p = json_key(meta, a + 1, ae, app);
		if ( p >= 0 && meta[p] == '{' ) {
			pe = json_close(meta, p, ae);
			d = json_key(meta, p + 1, pe, "defaultOpen");
			yes = (BOOL)( d >= 0 && d + 4 <= pe && meta[d] == 't'
				   && meta[d + 1] == 'r' && meta[d + 2] == 'u'
				   && meta[d + 3] == 'e' );
		}
	}
	Kfree(meta);

	return yes;
}

/* ---------------------------------------------------------------- metadata */

/* How long the value at 'at' runs: a string with its quotes, a list or an
   object to its bracket, anything else to the comma or brace after it */
LOCAL INT json_extent( CONST UB *b, INT at, INT to )
{
	INT	j;

	if ( b[at] == '"' ) {
		for ( j = at + 1; j < to && b[j] != '"'; j++ ) {
			if ( b[j] == 0x5C ) {
				j++;
			}
		}
		return ( j < to ) ? j + 1 - at : to - at;
	}
	if ( b[at] == '[' || b[at] == '{' ) {
		return json_close(b, at, to) - at;
	}
	for ( j = at; j < to && b[j] != ',' && b[j] != '}' && b[j] != ']'
		     && b[j] != '\r' && b[j] != '\n'; j++ ) {
		;
	}
	while ( j > at && ( b[j - 1] == ' ' || b[j - 1] == '\t' ) ) {
		j--;
	}

	return j - at;
}

/* The bytes from 'at' for 'len' replaced by 'val' */
LOCAL BOOL meta_splice( UB **p_meta, SZ *p_size, INT at, INT len,
			CONST UB *val, SZ n )
{
	UB	*b = *p_meta, *nb;
	SZ	nsize = *p_size - (SZ)len + n;

	nb = (UB *)Kmalloc(nsize + 1);
	if ( nb == NULL ) {
		return FALSE;
	}
	knl_memcpy(nb, b, (SZ)at);
	knl_memcpy(nb + at, val, n);
	knl_memcpy(nb + at + (INT)n, b + at + len, *p_size - (SZ)( at + len ));
	nb[nsize] = 0;
	Kfree(b);
	*p_meta = nb;
	*p_size = nsize;

	return TRUE;
}

/* A string, quoted, with what must be escaped escaped */
LOCAL SZ meta_quote( UB *out, SZ max, CONST UB *s )
{
	SZ	n = 0;
	INT	k;

	out[n++] = '"';
	for ( k = 0; s[k] != 0 && n + 3 < max; k++ ) {
		if ( s[k] == '"' || s[k] == 0x5C ) {
			out[n++] = 0x5C;
		}
		out[n++] = s[k];
	}
	out[n++] = '"';

	return n;
}

#define meta_write	om_obj_meta_put
#define meta_read	om_obj_meta

/* What the store remembers of an object's metadata, forgotten */
LOCAL void meta_forget_u( CONST TS_UUID *id )
{
	INT	slot = slot_of(id, -1);

	if ( slot >= 0 ) {
		st_tab[slot].named = FALSE;
		st_tab[slot].has_name = FALSE;
		st_tab[slot].has_paper = FALSE;
		st_tab[slot].described = FALSE;
		/* the icon lives beside the metadata and is looked for again with it */
		if ( st_tab[slot].icon != NULL ) {
			Kfree(st_tab[slot].icon);
			st_tab[slot].icon = NULL;
		}
		st_tab[slot].icon_tried = FALSE;
	}
}

LOCAL void meta_forget( CONST TS_UUID *id )
{
	st_lock();
	meta_forget_u(id);
	st_unlock();
}

/*
 * A new name: in the metadata, and in the record's own name for itself,
 * the filename of its <tad> element. A record that is open has the
 * name set in what is open, so that saving it does not put the old
 * name back.
 */
LOCAL ER rename_locked( CONST TS_UUID *id, CONST UB *name );

/* From any task (the file system's rename too): all of it under the table's lock */
EXPORT ER om_store_rename( CONST TS_UUID *id, CONST UB *name )
{
	ER	er;

	st_lock();
	er = rename_locked(id, name);
	st_unlock();
	return er;
}

LOCAL ER rename_locked( CONST TS_UUID *id, CONST UB *name )
{
	UB	*meta, val[TAD_NAME_MAX * 2 + 4];
	SZ	size = 0, n;
	INT	at, slot;
	ER	er;

	if ( id == NULL || name == NULL || name[0] == 0 ) {
		return E_PAR;
	}
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	at = json_key(meta, 0, (INT)size, "name");
	if ( at < 0 ) {
		Kfree(meta);
		return E_OBJ;
	}
	n = meta_quote(val, sizeof(val), name);
	if ( !meta_splice(&meta, &size, at, json_extent(meta, at, (INT)size),
			  val, n) ) {
		Kfree(meta);
		return E_NOMEM;
	}
	er = meta_write(id, meta, size);
	Kfree(meta);
	meta_forget(id);
	if ( er < E_OK ) {
		return er;
	}

	/* the record's own name for itself */
	slot = slot_of(id, 0);
	if ( slot >= 0 && st_tab[slot].doc == NULL ) {
		T_VOBJ	v;

		knl_memset(&v, 0, sizeof(v));
		v.target = *id;
		v.recno = 0;
		(void)store_open(&v, NULL);
	}
	if ( slot >= 0 && st_tab[slot].doc != NULL ) {
		T_TADNODE	*root = tad_root(st_tab[slot].doc);

		if ( root != NULL
		  && tad_set_attr(st_tab[slot].doc, root, "filename", name)
		     >= E_OK ) {
			er = om_store_save(id, 0);
		}
	}

	return er;
}

/* What the object is written on, set, in the metadata's window */
EXPORT ER om_store_set_paper( CONST TS_UUID *id, UW colour )
{
	UB	*meta, val[12];
	SZ	size = 0;
	INT	at, end, i;
	CONST char *hex = "0123456789abcdef";
	ER	er;

	if ( id == NULL ) {
		return E_PAR;
	}
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	val[0] = '"';
	val[1] = '#';
	for ( i = 0; i < 6; i++ ) {
		val[2 + i] = (UB)hex[( colour >> ( 20 - i * 4 ) ) & 0xF];
	}
	val[8] = '"';
	at = win_key(meta, (INT)size, "backgroundColor", NULL, &end);
	if ( at < 0 ) {
		Kfree(meta);
		return E_OBJ;
	}
	if ( !meta_splice(&meta, &size, at, json_extent(meta, at, end), val, 9) ) {
		Kfree(meta);
		return E_NOMEM;
	}
	er = meta_write(id, meta, size);
	Kfree(meta);
	meta_forget(id);

	return er;
}

/*
 * The relationship of the object: the tags of its metadata's
 * "relationship" list, as "[tag] [tag]", one after another.
 */
EXPORT INT om_store_relationship( CONST TS_UUID *id, UB *buf, INT max )
{
	UB	*meta;
	SZ	size = 0;
	INT	at, end, i, n = 0;

	if ( id == NULL || buf == NULL || max < 1 ) {
		return -1;
	}
	buf[0] = 0;
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return -1;
	}
	at = json_key(meta, 0, (INT)size, "relationship");
	if ( at >= 0 && meta[at] == '[' ) {
		end = json_close(meta, at, (INT)size);
		for ( i = at + 1; i < end; i++ ) {
			if ( meta[i] != '"' ) {
				continue;
			}
			if ( n > 0 && n < max - 1 ) {
				buf[n++] = ' ';
			}
			if ( n < max - 1 ) {
				buf[n++] = '[';
			}
			for ( i++; i < end && meta[i] != '"' && n < max - 2; i++ ) {
				buf[n++] = meta[i];
			}
			if ( n < max - 1 ) {
				buf[n++] = ']';
			}
		}
	}
	buf[n] = 0;
	Kfree(meta);

	return n;
}

/* The tags given as "[tag] [tag]" made the metadata's relationship list */
EXPORT ER om_store_set_relationship( CONST TS_UUID *id, CONST UB *tags )
{
	UB	*meta, *val;
	SZ	size = 0, n = 0, cap = 512;
	INT	at, i, k;
	ER	er;
	BOOL	first = TRUE;

	if ( id == NULL || tags == NULL ) {
		return E_PAR;
	}
	val = (UB *)Kmalloc(cap);
	if ( val == NULL ) {
		return E_NOMEM;
	}
	val[n++] = '[';
	for ( i = 0; tags[i] != 0; i++ ) {
		if ( tags[i] != '[' ) {
			continue;
		}
		for ( k = i + 1; tags[k] != 0 && tags[k] != ']'; k++ ) {
			;
		}
		if ( n + (SZ)( k - i ) + 6 >= cap ) {
			break;
		}
		if ( !first ) {
			val[n++] = ',';
			val[n++] = ' ';
		}
		first = FALSE;
		val[n++] = '"';
		for ( i++; i < k; i++ ) {
			if ( tags[i] == '"' || tags[i] == 0x5C ) {
				val[n++] = 0x5C;
			}
			val[n++] = tags[i];
		}
		val[n++] = '"';
		if ( tags[i] == 0 ) {
			break;
		}
	}
	val[n++] = ']';

	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		Kfree(val);
		return E_NOEXS;
	}
	at = json_key(meta, 0, (INT)size, "relationship");
	if ( at < 0 ) {
		Kfree(meta);
		Kfree(val);
		return E_OBJ;
	}
	if ( !meta_splice(&meta, &size, at, json_extent(meta, at, (INT)size),
			  val, n) ) {
		Kfree(meta);
		Kfree(val);
		return E_NOMEM;
	}
	Kfree(val);
	er = meta_write(id, meta, size);
	Kfree(meta);
	meta_forget(id);

	return er;
}

/*
 * The programs the object's applist names, by the names they are shown
 * under, in the order the list gives them.
 */
LOCAL INT names_of( CONST UB *meta, SZ size, UB (*names)[OM_APP_NAME], INT max );

EXPORT INT om_store_apps( CONST TS_UUID *id, UB (*names)[OM_APP_NAME],
			  INT max )
{
	UB	*meta;
	SZ	size = 0;
	INT	n;

	if ( id == NULL || names == NULL ) {
		return 0;
	}
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return 0;
	}
	n = names_of(meta, size, names, max);
	Kfree(meta);
	return n;
}

/* The names an applist gives its programs, in its order; names NULL counts them all */
LOCAL INT names_of( CONST UB *meta, SZ size, UB (*names)[OM_APP_NAME], INT max )
{
	INT	a, ae, i, n = 0, depth = 0;

	a = json_key(meta, 0, (INT)size, "applist");
	if ( a >= 0 && meta[a] == '{' ) {
		ae = json_close(meta, a, (INT)size);
		for ( i = a + 1; i < ae && ( names == NULL || n < max ); i++ ) {
			if ( meta[i] == '{' ) {
				INT	e = json_close(meta, i, ae);
				INT	v = json_key(meta, i + 1, e, "name");

				if ( depth == 0 && v >= 0 && meta[v] == '"' ) {
					INT	k = 0;

					for ( v++; names != NULL && v < e && meta[v] != '"'
						   && k < OM_APP_NAME - 1; v++ ) {
						names[n][k++] = meta[v];
					}
					if ( names != NULL ) names[n][k] = 0;
					n++;
				}
				i = e - 1;
				continue;
			}
		}
	}
	return n;
}

/*
 * The identity of the program at a place in the applist, the same order
 * om_store_apps gives the names in ("basic-text-editor"); and the one
 * the object opens with: the first marked defaultOpen, or the first.
 * Answers the length, or -1.
 */
LOCAL INT app_key( CONST UB *meta, SZ size, INT index, BOOL dflt, UB *buf, INT max )
{
	INT	a, ae, i, j = 0, fs = -1, fl = 0, k;
	BOOL	found = FALSE;

	a = json_key(meta, 0, (INT)size, "applist");
	if ( a < 0 || meta[a] != '{' ) {
		return -1;
	}
	ae = json_close(meta, a, (INT)size);
	for ( i = a + 1; i < ae; ) {
		INT	ks, kl, v, e, d;
		BOOL	hit;

		while ( i < ae && meta[i] != '"' ) i++;		/* the next program's key */
		if ( i >= ae ) break;
		ks = i + 1;
		for ( i = ks; i < ae && meta[i] != '"'; i++ ) ;
		kl = i - ks;
		for ( v = i + 1; v < ae && meta[v] != '{'; v++ ) ;	/* what it says of it */
		if ( v >= ae ) break;
		e = json_close(meta, v, ae);
		if ( fs < 0 ) {
			fs = ks;
			fl = kl;
		}
		d = dflt ? json_key(meta, v + 1, e, "defaultOpen") : -1;
		hit = dflt ? (BOOL)( d >= 0 && meta[d] == 't' ) : (BOOL)( j == index );
		if ( hit ) {
			fs = ks;
			fl = kl;
			found = TRUE;
			break;
		}
		j++;
		i = e;
	}
	/* a place past the last names none (the first is only the default's stand-in) */
	if ( fs < 0 || ( !dflt && !found ) ) {
		return -1;
	}
	for ( k = 0; k < fl && k < max - 1; k++ ) {
		buf[k] = meta[fs + k];
	}
	buf[k] = 0;

	return k;
}

EXPORT INT om_store_app_id( CONST TS_UUID *id, INT index, UB *buf, INT max )
{
	UB	*meta;
	SZ	size = 0;
	INT	n;

	if ( id == NULL || buf == NULL || max < 2 || index < 0 ) {
		return -1;
	}
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return -1;
	}
	n = app_key(meta, size, index, FALSE, buf, max);
	Kfree(meta);

	return n;
}

EXPORT INT om_store_default_app( CONST TS_UUID *id, UB *buf, INT max )
{
	UB	*meta;
	SZ	size = 0;
	INT	n;

	if ( id == NULL || buf == NULL || max < 2 ) {
		return -1;
	}
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return -1;
	}
	n = app_key(meta, size, 0, TRUE, buf, max);
	Kfree(meta);

	return n;
}

/*
 * The applist made anew: the programs given, in their order, each by its
 * identity with the name it is shown under, and 'dflt' (0 up) the one
 * the object opens with; -1 none. What else the old list said of each
 * program is not kept. An object whose metadata has no applist is given
 * one.
 */
EXPORT ER om_store_set_apps( CONST TS_UUID *id, CONST UB *CONST *ids,
			     CONST UB *CONST *names, INT n, INT dflt )
{
	UB	*meta, *val;
	SZ	size = 0, k = 0, cap;
	INT	at, i;
	ER	er;
	BOOL	ok;

	if ( id == NULL || n < 0 || ( n > 0 && ( ids == NULL || names == NULL ) ) ) {
		return E_PAR;
	}
	cap = 32 + (SZ)n * ( 2 * OM_APP_ID + 2 * OM_APP_NAME + 48 );
	val = (UB *)Kmalloc(cap);
	if ( val == NULL ) {
		return E_NOMEM;
	}
	val[k++] = '{';
	for ( i = 0; i < n; i++ ) {
		CONST char	*d = ( i == dflt ) ? "true" : "false";
		CONST char	*mid = ":{\"name\":";
		CONST char	*tail = ",\"defaultOpen\":";

		if ( i > 0 ) {
			val[k++] = ',';
		}
		k += meta_quote(val + k, cap - k, ids[i]);
		while ( *mid != 0 ) val[k++] = (UB)*mid++;
		k += meta_quote(val + k, cap - k, names[i]);
		while ( *tail != 0 ) val[k++] = (UB)*tail++;
		while ( *d != 0 ) val[k++] = (UB)*d++;
		val[k++] = '}';
	}
	val[k++] = '}';

	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		Kfree(val);
		return E_NOEXS;
	}
	at = json_key(meta, 0, (INT)size, "applist");
	if ( at >= 0 ) {
		ok = meta_splice(&meta, &size, at, json_extent(meta, at, (INT)size), val, k);
	} else {
		/* a new key, first in the object */
		UB	*v2;
		SZ	k2 = 0;
		CONST char *key = "\"applist\":";

		for ( at = 0; at < (INT)size && meta[at] != '{'; at++ ) {
			;
		}
		v2 = ( at < (INT)size ) ? (UB *)Kmalloc(k + 16) : NULL;
		ok = FALSE;
		if ( v2 != NULL ) {
			while ( *key != 0 ) v2[k2++] = (UB)*key++;
			knl_memcpy(v2 + k2, val, k);
			k2 += k;
			/* a comma unless the object was empty */
			for ( i = at + 1; i < (INT)size && ( meta[i] == ' ' || meta[i] == '\t'
					|| meta[i] == '\r' || meta[i] == '\n' ); i++ ) {
				;
			}
			if ( i < (INT)size && meta[i] != '}' ) {
				v2[k2++] = ',';
			}
			ok = meta_splice(&meta, &size, at + 1, 0, v2, k2);
			Kfree(v2);
		}
	}
	Kfree(val);
	if ( !ok ) {
		Kfree(meta);
		return E_NOMEM;
	}
	er = meta_write(id, meta, size);
	Kfree(meta);
	meta_forget(id);

	return er;
}

/*
 * The program the object opens with made 'app', one its applist already
 * names: that entry's "defaultOpen" becomes true and every other's
 * false, and whatever else the entries say is kept as it was written.
 */
EXPORT ER om_store_set_default_app( CONST TS_UUID *id, CONST UB *app )
{
	UB	*meta, *out;
	SZ	size = 0, n = 0, cap;
	INT	a, ae, i, cur, alen = 0;
	BOOL	found = FALSE;
	ER	er;

	if ( id == NULL || app == NULL || app[0] == 0 ) {
		return E_PAR;
	}
	while ( app[alen] != 0 ) alen++;
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	a = json_key(meta, 0, (INT)size, "applist");
	if ( a < 0 || meta[a] != '{' ) {
		Kfree(meta);
		return E_OBJ;
	}
	ae = json_close(meta, a, (INT)size);
	cap = (SZ)( ae - a ) * 2 + 64;
	out = (UB *)Kmalloc(cap);
	if ( out == NULL ) {
		Kfree(meta);
		return E_NOMEM;
	}
	cur = a;
	for ( i = a + 1; i < ae; ) {
		INT		ks, kl, v, e, d, k;
		BOOL		want;
		CONST char	*val;

		while ( i < ae && meta[i] != '"' ) i++;		/* the next program's key */
		if ( i >= ae ) break;
		ks = i + 1;
		for ( i = ks; i < ae && meta[i] != '"'; i++ ) ;
		kl = i - ks;
		for ( v = i + 1; v < ae && meta[v] != '{'; v++ ) ;	/* what it says of it */
		if ( v >= ae ) break;
		e = json_close(meta, v, ae);
		want = (BOOL)( kl == alen );
		for ( k = 0; want && k < kl; k++ ) {
			if ( meta[ks + k] != app[k] ) want = FALSE;
		}
		found = (BOOL)( found || want );
		val = want ? "true" : "false";
		d = json_key(meta, v + 1, e, "defaultOpen");
		if ( d >= 0 ) {
			knl_memcpy(out + n, meta + cur, (SZ)( d - cur ));
			n += (SZ)( d - cur );
			while ( *val != 0 ) out[n++] = (UB)*val++;
			cur = d + json_extent(meta, d, e);
		} else if ( want ) {
			CONST char	*key = "\"defaultOpen\":true";
			BOOL		empty = TRUE;

			knl_memcpy(out + n, meta + cur, (SZ)( v + 1 - cur ));
			n += (SZ)( v + 1 - cur );
			while ( *key != 0 ) out[n++] = (UB)*key++;
			for ( k = v + 1; k < e - 1; k++ ) {
				if ( meta[k] != ' ' && meta[k] != '\t' && meta[k] != '\r' && meta[k] != '\n' ) {
					empty = FALSE;
				}
			}
			if ( !empty ) out[n++] = ',';
			cur = v + 1;
		}
		i = e;
	}
	knl_memcpy(out + n, meta + cur, (SZ)( ae - cur ));
	n += (SZ)( ae - cur );
	if ( !found ) {
		Kfree(out);
		Kfree(meta);
		return E_NOEXS;
	}
	if ( !meta_splice(&meta, &size, a, ae - a, out, n) ) {
		Kfree(out);
		Kfree(meta);
		return E_NOMEM;
	}
	Kfree(out);
	er = meta_write(id, meta, size);
	Kfree(meta);
	meta_forget(id);

	return er;
}


/*
 * The template a program of the program box names first in its "base",
 * when its "tessronos.program.id" is the one asked for.
 */
LOCAL BOOL prog_base( CONST TS_UUID *prog, CONST UB *app, TS_UUID *p_base )
{
	UB	*meta, us[TS_UUID_STRLEN + 1];
	SZ	size = 0;
	INT	t, te, p, pe, d, b, k;
	BOOL	ok = FALSE;

	meta = meta_read(prog, &size);
	if ( meta == NULL ) {
		return FALSE;
	}
	t = json_key(meta, 0, (INT)size, "tessronos");
	if ( t >= 0 && meta[t] == '{' ) {
		te = json_close(meta, t, (INT)size);
		p = json_key(meta, t + 1, te, "program");
		if ( p >= 0 && meta[p] == '{' ) {
			pe = json_close(meta, p, te);
			d = json_key(meta, p + 1, pe, "id");
			for ( k = 0; d >= 0 && meta[d] == '"' && app[k] != 0
				     && d + 1 + k < pe && meta[d + 1 + k] == app[k]; k++ ) ;
			b = json_key(meta, p + 1, pe, "base");
			if ( d >= 0 && app[k] == 0 && d + 1 + k < pe && meta[d + 1 + k] == '"'
			  && b >= 0 && meta[b] == '[' ) {
				for ( b++; b < pe && meta[b] != '"' && meta[b] != ']'; b++ ) ;
				if ( b + 1 + TS_UUID_STRLEN < pe && meta[b] == '"' ) {
					for ( k = 0; k < TS_UUID_STRLEN; k++ ) us[k] = meta[b + 1 + k];
					us[k] = 0;
					ok = (BOOL)( ts_str_to_uuid((CONST char *)us, p_base) >= E_OK );
				}
			}
		}
	}
	Kfree(meta);
	return ok;
}

/*
 * The icon of the template of a program ("basic-text-editor"), which an
 * object that opens in that program is given when it is made; NULL when
 * the program or its template or its icon is not there. The caller's to
 * Kfree.
 */
EXPORT UB *om_store_app_icon( CONST UB *app, SZ *p_size )
{
	TS_UUID	box, base, *lk;
	UB	*ico = NULL;
	INT	n = 0, i;

	*p_size = 0;
	if ( app == NULL || app[0] == 0 || ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) < E_OK ) {
		return NULL;
	}
	/* every program of the box: asked how many, then taken */
	if ( ob_lst_lnk(&box, NULL, NULL, 0, &n) < E_OK || n <= 0 ) {
		return NULL;
	}
	lk = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( lk != NULL ) {
		INT	all = 0;

		if ( ob_lst_lnk(&box, NULL, lk, n, &all) < E_OK ) all = 0;
		if ( all < n ) n = all;
	} else {
		n = 0;
	}
	for ( i = 0; i < n && ico == NULL; i++ ) {
		if ( prog_base(&lk[i], app, &base) ) {
			ico = om_obj_icon(&base, p_size);
		}
	}
	if ( lk != NULL ) {
		Kfree(lk);
	}
	return ico;
}

/*
 * The same three for a link: the object's own applist when it has one,
 * else the one the link carries (with the quotes a record may have left
 * as &quot;).
 */
LOCAL UB *link_applist( CONST T_VOBJ *v, SZ *p_size )
{
	UB	*j;
	INT	i, n = 0;
	CONST char *head = "{\"applist\":";

	if ( v->applist[0] == 0 ) {
		return NULL;
	}
	j = (UB *)Kmalloc(TAD_APPL_MAX + 16);
	if ( j == NULL ) {
		return NULL;
	}
	for ( i = 0; head[i] != 0; i++ ) j[n++] = (UB)head[i];
	for ( i = 0; v->applist[i] != 0 && n < TAD_APPL_MAX + 12; ) {
		if ( v->applist[i] == '&' && v->applist[i + 1] == 'q' && v->applist[i + 2] == 'u'
		  && v->applist[i + 3] == 'o' && v->applist[i + 4] == 't' && v->applist[i + 5] == ';' ) {
			j[n++] = '"';
			i += 6;
			continue;
		}
		j[n++] = v->applist[i++];
	}
	j[n++] = '}';
	j[n] = 0;
	*p_size = n;
	return j;
}

EXPORT INT om_link_apps( CONST T_VOBJ *v, UB (*names)[OM_APP_NAME], INT max )
{
	INT	n = om_store_apps(&v->target, names, max);
	UB	*j;
	SZ	size = 0;

	if ( n > 0 || ( j = link_applist(v, &size) ) == NULL ) {
		return n;
	}
	n = names_of(j, size, names, max);
	Kfree(j);
	return n;
}

EXPORT INT om_link_napps( CONST T_VOBJ *v )
{
	UB	*j;
	SZ	size = 0;
	INT	n = 0;

	j = meta_read(&v->target, &size);
	if ( j != NULL ) {
		n = names_of(j, size, NULL, 0);
		Kfree(j);
	}
	if ( n > 0 || ( j = link_applist(v, &size) ) == NULL ) {
		return n;
	}
	n = names_of(j, size, NULL, 0);
	Kfree(j);
	return n;
}

EXPORT INT om_link_app_id( CONST T_VOBJ *v, INT index, UB *buf, INT max )
{
	INT	n = om_store_app_id(&v->target, index, buf, max);
	UB	*j;
	SZ	size = 0;

	if ( n > 0 || ( j = link_applist(v, &size) ) == NULL ) {
		return n;
	}
	n = app_key(j, size, index, FALSE, buf, max);
	Kfree(j);
	return n;
}

/*
 * Whether the program an applist opens with is fixed to it (起動アプリの
 * 固定: "defaultOpenLock" on the entry marked defaultOpen). A link fixed
 * so opens with its own program whatever the object's applist says.
 */
LOCAL BOOL app_locked( CONST UB *j, SZ size )
{
	INT	a, ae, i;

	a = json_key(j, 0, (INT)size, "applist");
	if ( a < 0 || j[a] != '{' ) {
		return FALSE;
	}
	ae = json_close(j, a, (INT)size);
	for ( i = a + 1; i < ae; i++ ) {
		INT	e, d, k;

		if ( j[i] != '{' ) {
			continue;
		}
		e = json_close(j, i, ae);
		d = json_key(j, i + 1, e, "defaultOpen");
		k = json_key(j, i + 1, e, "defaultOpenLock");
		if ( d >= 0 && j[d] == 't' ) {
			return (BOOL)( k >= 0 && j[k] == 't' );
		}
		i = e - 1;
	}
	return FALSE;
}

EXPORT INT om_link_default_app( CONST T_VOBJ *v, UB *buf, INT max )
{
	SZ	size = 0;
	UB	*j = link_applist(v, &size);
	INT	n;

	if ( j != NULL && app_locked(j, size) ) {
		n = app_key(j, size, 0, TRUE, buf, max);
		Kfree(j);
		return n;
	}
	n = om_store_default_app(&v->target, buf, max);
	if ( n <= 0 && j != NULL ) {
		n = app_key(j, size, 0, TRUE, buf, max);
	}
	if ( j != NULL ) {
		Kfree(j);
	}
	return n;
}

/*
 * 起動アプリの固定 of a link (design 11.3): whether it keeps to the
 * program it opens with, "defaultOpenLock" on the entry of the link's
 * own applist that is marked defaultOpen. A link that does not keep to
 * one opens with the program its object's applist marks, which 実行
 * makes the one run last (om_link_ran).
 */
EXPORT BOOL om_link_locked( CONST T_VOBJ *v )
{
	SZ	size = 0;
	UB	*j = ( v != NULL ) ? link_applist(v, &size) : NULL;
	BOOL	locked;

	if ( j == NULL ) {
		return FALSE;
	}
	locked = app_locked(j, size);
	Kfree(j);
	return locked;
}

/*
 * A program of the applist run on the object from a link (実行): unless
 * the link keeps to its program, the object opens with that one from
 * now, written into the object's metadata.
 */
EXPORT ER om_link_ran( CONST T_VOBJ *v, CONST UB *app )
{
	if ( v == NULL || app == NULL || app[0] == 0 ) {
		return E_PAR;
	}
	if ( om_link_locked(v) ) {
		return E_OK;
	}
	return om_store_set_default_app(&v->target, app);
}

/* Past the spaces, tabs and line ends between the parts of a JSON text */
LOCAL INT lk_ws( CONST UB *b, INT i, INT to )
{
	while ( i < to && ( b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n' ) ) {
		i++;
	}
	return i;
}

/* Past the spaces and the comma before the next member */
LOCAL INT lk_next( CONST UB *b, INT i, INT to )
{
	while ( i < to && ( b[i] == ' ' || b[i] == '\t' || b[i] == '\r' || b[i] == '\n' || b[i] == ',' ) ) {
		i++;
	}
	return i;
}

LOCAL BOOL lk_put( UB *out, INT max, INT *n, CONST UB *s, INT len )
{
	if ( *n + len >= max ) {
		return FALSE;
	}
	knl_memcpy(out + *n, s, (SZ)len);
	*n += len;
	return TRUE;
}

LOCAL BOOL lk_str( UB *out, INT max, INT *n, CONST char *s )
{
	INT	len = 0;

	while ( s[len] != 0 ) len++;
	return lk_put(out, max, n, (CONST UB *)s, len);
}

/*
 * One member of an object at 'at' (its key's quote): where its key ends
 * (*p_ke, past the quote), where its value starts (*p_vs) and ends
 * (answered); -1 when there is no member there.
 */
LOCAL INT lk_member( CONST UB *b, INT at, INT to, INT *p_ke, INT *p_vs )
{
	INT	i;

	if ( at >= to || b[at] != '"' ) {
		return -1;
	}
	*p_ke = at + json_extent(b, at, to);
	i = lk_ws(b, *p_ke, to);
	if ( i >= to || b[i] != ':' ) {
		return -1;
	}
	*p_vs = lk_ws(b, i + 1, to);
	if ( *p_vs >= to ) {
		return -1;
	}
	return *p_vs + json_extent(b, *p_vs, to);
}

/*
 * The applist from s[0] (its brace) to s[to - 1] (its closing brace)
 * written again into out without the spaces between its parts: the
 * entry the object opens with -- the first marked defaultOpen, else the
 * first -- marked so, with "defaultOpenLock" as 'keep' says, and no
 * other entry with one. 'only' leaves out the other entries. The length,
 * -1 when the list names no program, -2 when out cannot hold it.
 */
LOCAL INT applist_kept( CONST UB *s, INT to, BOOL keep, BOOL only, UB *out, INT max )
{
	INT	i, e, ke, vs, d, nth = 0, dflt = -1, n = 0;
	INT	m, me, mke, mvs;
	BOOL	ok = TRUE, first = TRUE, mfirst;

	if ( to < 2 || s[0] != '{' ) {
		return -1;
	}
	for ( i = lk_next(s, 1, to - 1); ( e = lk_member(s, i, to - 1, &ke, &vs) ) >= 0;
	      i = lk_next(s, e, to - 1) ) {
		if ( s[vs] == '{' ) {
			d = json_key(s, vs + 1, e, "defaultOpen");
			if ( dflt < 0 && d >= 0 && s[d] == 't' ) {
				dflt = nth;
			}
		}
		nth++;
	}
	if ( nth == 0 ) {
		return -1;
	}
	if ( dflt < 0 ) {
		dflt = 0;
	}
	ok = lk_str(out, max, &n, "{");
	nth = 0;
	for ( i = lk_next(s, 1, to - 1); ok && ( e = lk_member(s, i, to - 1, &ke, &vs) ) >= 0;
	      i = lk_next(s, e, to - 1) ) {
		if ( only && nth != dflt ) {
			nth++;
			continue;
		}
		if ( !first ) ok = lk_str(out, max, &n, ",");
		first = FALSE;
		ok = ok && lk_put(out, max, &n, s + i, ke - i) && lk_str(out, max, &n, ":");
		if ( s[vs] != '{' ) {
			ok = ok && lk_put(out, max, &n, s + vs, e - vs);
			nth++;
			continue;
		}
		/* the entry's members, the marks of the default dropped and put back where they belong */
		ok = ok && lk_str(out, max, &n, "{");
		mfirst = TRUE;
		for ( m = lk_next(s, vs + 1, e - 1); ok && ( me = lk_member(s, m, e - 1, &mke, &mvs) ) >= 0;
		      m = lk_next(s, me, e - 1) ) {
			if ( key_is(s + m + 1, mke - m - 1, "defaultOpenLock")
			  || ( nth == dflt && key_is(s + m + 1, mke - m - 1, "defaultOpen") ) ) {
				continue;
			}
			if ( !mfirst ) ok = lk_str(out, max, &n, ",");
			mfirst = FALSE;
			ok = ok && lk_put(out, max, &n, s + m, mke - m) && lk_str(out, max, &n, ":")
			     && lk_put(out, max, &n, s + mvs, me - mvs);
		}
		if ( ok && nth == dflt ) {
			ok = ( mfirst || lk_str(out, max, &n, ",") )
			     && lk_str(out, max, &n, "\"defaultOpen\":true,\"defaultOpenLock\":")
			     && lk_str(out, max, &n, keep ? "true" : "false");
		}
		ok = ok && lk_str(out, max, &n, "}");
		nth++;
	}
	ok = ok && lk_str(out, max, &n, "}");
	if ( !ok ) {
		return -2;
	}
	out[n] = 0;
	return n;
}

/*
 * The applist of a JSON text (from its brace to just past its closing one)
 * made into out. A list of more programs than out holds is kept as the
 * entry the link opens with: which programs there are is the object's to say.
 */
LOCAL INT applist_kept_of( CONST UB *j, SZ size, BOOL keep, UB *out, INT max )
{
	INT	a = json_key(j, 0, (INT)size, "applist"), to, n;

	if ( a < 0 || j[a] != '{' ) {
		return -1;
	}
	to = json_close(j, a, (INT)size) - a;
	n = applist_kept(j + a, to, keep, FALSE, out, max);
	if ( n == -2 ) {
		n = applist_kept(j + a, to, keep, TRUE, out, max);
	}
	return n;
}

/*
 * 起動アプリの固定 set or let go: the link given the applist its object
 * has now (an object with none, the link's own), with "defaultOpenLock"
 * as 'keep' says on the entry the object opens with. A link fixed so
 * keeps to the program the object opens with at that moment.
 */
EXPORT ER om_link_lock( T_VOBJ *v, BOOL keep )
{
	UB	*j, *out;
	SZ	size = 0;
	INT	n = -1;

	if ( v == NULL ) {
		return E_PAR;
	}
	out = (UB *)Kmalloc(TAD_APPL_MAX);
	if ( out == NULL ) {
		return E_NOMEM;
	}
	j = meta_read(&v->target, &size);
	if ( j != NULL ) {
		n = applist_kept_of(j, size, keep, out, TAD_APPL_MAX);
		Kfree(j);
	}
	if ( n == -1 && ( j = link_applist(v, &size) ) != NULL ) {
		n = applist_kept_of(j, size, keep, out, TAD_APPL_MAX);
		Kfree(j);
	}
	if ( n >= 0 ) {
		knl_memcpy(v->applist, out, (SZ)n + 1);
	}
	Kfree(out);

	return ( n >= 0 ) ? E_OK : ( n == -2 ) ? E_LIMIT : E_NOEXS;
}

/* One string value of the metadata, or "" */
LOCAL void meta_str( CONST UB *meta, SZ size, CONST char *key, UB *out, INT max )
{
	INT	at = json_key(meta, 0, (INT)size, key), n = 0;

	if ( at >= 0 && meta[at] == '"' ) {
		for ( at++; at < (INT)size && meta[at] != '"' && n < max - 1; at++ ) {
			out[n++] = meta[at];
		}
	}
	out[n] = 0;
}

EXPORT ER om_store_info( CONST TS_UUID *id, T_OMINFO *info )
{
	UB	*meta;
	SZ	size = 0;
	INT	at, n, i;
	T_OBREC	rec[OM_REC_MAX];

	if ( id == NULL || info == NULL ) {
		return E_PAR;
	}
	knl_memset(info, 0, sizeof(*info));
	meta = meta_read(id, &size);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	meta_str(meta, size, "name", info->name, TAD_NAME_MAX);
	meta_str(meta, size, "maker", info->maker, OM_INFO_TEXT);
	meta_str(meta, size, "makeDate", info->made, OM_INFO_TEXT);
	meta_str(meta, size, "updateDate", info->updated, OM_INFO_TEXT);
	meta_str(meta, size, "accessDate", info->accessed, OM_INFO_TEXT);
	at = json_key(meta, 0, (INT)size, "refCount");
	if ( at >= 0 ) {
		json_int(meta, at, (INT)size, &info->refs);
	}
	at = json_key(meta, 0, (INT)size, "recordCount");
	if ( at >= 0 ) {
		json_int(meta, at, (INT)size, &info->records);
	}
	Kfree(meta);
	n = om_obj_records(id, rec, OM_REC_MAX);
	for ( i = 0; i < n; i++ ) {
		if ( rec[i].recno == 0 ) {
			info->bytes = (INT)rec[i].size;
		}
	}

	return E_OK;
}

/* ---------------------------------------------------------------- what a link's band says */

/*
 * The name of the program an applist opens with: the entry marked
 * defaultOpen. Nothing when none is marked. The list is the object's
 * metadata's, or the one the link carries.
 */
LOCAL INT default_app_name( CONST UB *j, INT size, UB *buf, INT max )
{
	INT	a, ae, i, n = 0;

	buf[0] = 0;
	a = json_key(j, 0, size, "applist");
	if ( a < 0 ) {
		a = 0;				/* the link's: the list itself */
		while ( a < size && j[a] != '{' ) a++;
		if ( a >= size ) return 0;
	}
	if ( j[a] != '{' ) {
		return 0;
	}
	ae = json_close(j, a, size);
	for ( i = a + 1; i < ae; i++ ) {
		INT	e, d, v;

		if ( j[i] != '{' ) {
			continue;
		}
		e = json_close(j, i, ae);
		d = json_key(j, i + 1, e, "defaultOpen");
		if ( d >= 0 && j[d] == 't' ) {
			v = json_key(j, i + 1, e, "name");
			if ( v >= 0 && j[v] == '"' ) {
				for ( v++; v < e && j[v] != '"' && n < max - 1; v++ ) {
					buf[n++] = j[v];
				}
			}
			break;
		}
		i = e - 1;
	}
	buf[n] = 0;
	return n;
}

/* A time written "YYYY-MM-DDTHH:MM:SS[.sss](Z|±HH:MM)", in system time */
LOCAL BOOL iso_time( CONST UB *s, INT n, TS_TIME *p_t )
{
	TS_TM	tm;
	INT	f[6], i, k = 0, v = 0, digits = 0, off = 0;

	for ( i = 0; i < n && k < 6; i++ ) {
		if ( s[i] >= '0' && s[i] <= '9' ) {
			v = v * 10 + ( s[i] - '0' );
			digits++;
			continue;
		}
		if ( digits == 0 ) return FALSE;
		f[k++] = v;
		v = 0;
		digits = 0;
		if ( k == 6 ) break;
	}
	if ( k < 6 && digits > 0 ) f[k++] = v;
	if ( k < 6 ) return FALSE;
	/* past the fraction, the zone */
	while ( i < n && ( s[i] == '.' || ( s[i] >= '0' && s[i] <= '9' ) ) ) i++;
	if ( i + 5 < n + 1 && ( s[i] == '+' || s[i] == '-' ) && i + 5 < n ) {
		INT	hh = ( s[i + 1] - '0' ) * 10 + ( s[i + 2] - '0' );
		INT	mm = ( s[i + 4] - '0' ) * 10 + ( s[i + 5] - '0' );

		off = ( hh * 60 + mm ) * 60;
		if ( s[i] == '-' ) off = -off;
	}
	knl_memset(&tm, 0, sizeof(tm));
	tm.tm_year = f[0] - 1900;
	tm.tm_mon = f[1] - 1;
	tm.tm_mday = f[2];
	tm.tm_hour = f[3];
	tm.tm_min = f[4];
	tm.tm_sec = f[5];
	if ( dt_mktime(&tm, p_t) < E_OK ) return FALSE;
	*p_t -= off;
	return TRUE;
}

/* The relationship, the program and the date of an object, looked for once */
LOCAL STENT *described( CONST TS_UUID *id )
{
	INT	slot = slot_of(id, -1), at, n;
	UB	*meta;
	SZ	size = 0;
	STENT	*e;

	if ( slot < 0 ) {
		return NULL;
	}
	e = &st_tab[slot];
	if ( e->described ) {
		return e;
	}
	e->rel[0] = 0;
	e->app[0] = 0;
	e->has_updated = FALSE;
	(void)om_store_relationship(id, e->rel, sizeof(e->rel));
	meta = meta_read(id, &size);
	if ( meta != NULL ) {
		(void)default_app_name(meta, (INT)size, e->app, OM_APP_NAME);
		at = json_key(meta, 0, (INT)size, "updateDate");
		if ( at >= 0 && meta[at] == '"' ) {
			for ( n = at + 1; n < (INT)size && meta[n] != '"'; n++ ) ;
			e->has_updated = iso_time(meta + at + 1, n - at - 1, &e->updated);
		}
		Kfree(meta);
	}
	e->described = TRUE;
	return e;
}

LOCAL INT put_s( UB *buf, INT n, INT max, CONST UB *s )
{
	while ( *s != 0 && n < max - 1 ) {
		buf[n++] = *s++;
	}
	buf[n] = 0;
	return n;
}

/* The program the link's own applist opens with */
LOCAL INT link_app_name( CONST T_VOBJ *v, UB *buf, INT max )
{
	UB	*j;
	SZ	size = 0;
	INT	n;

	buf[0] = 0;
	j = link_applist(v, &size);
	if ( j == NULL ) {
		return 0;
	}
	n = default_app_name(j, (INT)size, buf, max);
	Kfree(j);
	return n;
}

LOCAL INT store_label_u( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	STENT	*e = described(&v->target);
	UB	nm[TAD_NAME_MAX];
	INT	n = 0;

	buf[0] = 0;
	if ( ( v->disp & TAD_D_NAME ) != 0 ) {
		/* the object's own name before what the link remembers of it */
		if ( store_name(v, nm, TAD_NAME_MAX, arg) > 0 ) {
			n = put_s(buf, n, max, nm);
		} else {
			n = put_s(buf, n, max, v->name);
		}
	}
	if ( ( v->disp & TAD_D_ROLE ) != 0 ) {
		UB	rel[TAD_REL_MAX * 3];
		INT	k = 0;

		rel[0] = 0;
		if ( e != NULL ) k = put_s(rel, k, sizeof(rel), e->rel);
		if ( v->relationship[0] != 0 ) {
			if ( k > 0 ) k = put_s(rel, k, sizeof(rel), (CONST UB *)" ");
			k = put_s(rel, k, sizeof(rel), v->relationship);
		}
		if ( k > 0 ) {
			n = put_s(buf, n, max, (CONST UB *)" : ");
			n = put_s(buf, n, max, rel);
		}
	}
	if ( ( v->disp & TAD_D_TYPE ) != 0 ) {
		UB	app[OM_APP_NAME];

		SZ	size = 0;
		UB	*j = link_applist(v, &size);
		BOOL	locked = (BOOL)( j != NULL && app_locked(j, size) );

		if ( j != NULL ) {
			Kfree(j);
		}
		app[0] = 0;
		if ( e != NULL && e->app[0] != 0 && !locked ) {
			(void)put_s(app, 0, OM_APP_NAME, e->app);
		} else {
			(void)link_app_name(v, app, OM_APP_NAME);
		}
		if ( app[0] != 0 ) {
			n = put_s(buf, n, max, (CONST UB *)" (");
			n = put_s(buf, n, max, app);
			n = put_s(buf, n, max, (CONST UB *)")");
		}
	}
	if ( ( v->disp & TAD_D_UPDATE ) != 0 && e != NULL && e->has_updated ) {
		TS_TM	tm;
		char	d[24];

		if ( dt_localtime(&e->updated, &tm) >= E_OK
		  && dt_strftime(d, sizeof(d), " %Y/%m/%d %H:%M:%S", &tm) > 0 ) {
			n = put_s(buf, n, max, (CONST UB *)d);
		}
	}
	return n;
}

LOCAL INT store_label( CONST T_VOBJ *v, UB *buf, INT max, void *arg )
{
	INT r;

	st_lock();
	r = store_label_u(v, buf, max, arg);
	st_unlock();
	return r;
}

EXPORT INT om_store_label( CONST T_VOBJ *v, UB *buf, INT max )
{
	if ( v == NULL || buf == NULL || max < 1 ) {
		return -1;
	}
	return store_label(v, buf, max, NULL);
}

EXPORT ER om_store_icon( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h )
{
	if ( v == NULL || p_px == NULL || p_w == NULL || p_h == NULL ) {
		return E_PAR;
	}
	return store_icon(v, p_px, p_w, p_h, NULL);
}

/* Done with an icon om_store_icon gave: it may be let go from now on */
EXPORT void om_store_icon_done( CONST UW *px )
{
	store_icon_done(px, NULL);
}

EXPORT ER om_store_set_icon( CONST TS_UUID *id, CONST UB *buf, SZ size )
{
	ER	er = om_obj_icon_put(id, buf, size);

	meta_forget(id);
	return er;
}

/* The object's icon, beside its metadata, read and decoded once */
LOCAL ER store_icon_u( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h, void *arg )
{
	INT	slot = slot_of(&v->target, -1);
	STENT	*e;

	if ( slot < 0 ) {
		return E_LIMIT;
	}
	e = &st_tab[slot];
	if ( !e->icon_tried ) {
		UB	*file;
		SZ	size = 0;

		e->icon_tried = TRUE;
		file = om_obj_icon(&v->target, &size);
		if ( file != NULL ) {
			UW	*px = NULL;
			INT	w = 0, h = 0;

			if ( img_decode(file, size, &px, &w, &h) >= E_OK && px != NULL ) {
				SZ	bytes = (SZ)w * (SZ)h * 4;

				/* room for it made first: the entry itself is the newest and stays */
				(void)st_room(bytes);
				e = &st_tab[slot];
				e->icon = px;
				e->icon_w = w;
				e->icon_h = h;
				e->icon_bytes = bytes;
				st_bytes += bytes;
			}
			Kfree(file);
		}
	}
	if ( e->icon == NULL ) {
		return E_NOEXS;
	}
	e->icon_pins++;
	*p_px = e->icon;
	*p_w = e->icon_w;
	*p_h = e->icon_h;
	return E_OK;
}

LOCAL ER store_icon( CONST T_VOBJ *v, CONST UW **p_px, INT *p_w, INT *p_h, void *arg )
{
	ER r;

	st_lock();
	r = store_icon_u(v, p_px, p_w, p_h, arg);
	st_unlock();
	return r;
}

/* The drawing done with an icon store_icon handed it */
LOCAL void store_icon_done( CONST UW *px, void *arg )
{
	INT	i;

	if ( px == NULL ) {
		return;
	}
	st_lock();
	for ( i = 0; i < st_max; i++ ) {
		if ( st_tab[i].used && st_tab[i].icon == px ) {
			if ( st_tab[i].icon_pins > 0 ) st_tab[i].icon_pins--;
			st_unlock();
			return;
		}
	}
	for ( i = 0; i < ST_ORPHANS; i++ ) {
		if ( st_orphan[i] == px ) {
			if ( --st_orphan_pins[i] <= 0 ) {
				Kfree((void *)st_orphan[i]);
				st_orphan[i] = NULL;
			}
			break;
		}
	}
	st_unlock();
}

/* ---------------------------------------------------------------- the calls */

EXPORT ER om_store_files( CONST char *dir )
{
	ER	er = E_OK;

	/* the volume, for the file manager to find the objects on */
	if ( dir != NULL ) {
		er = ob_att_vol(dir, 0);
	}
	tv_source(&om_files, NULL);
	st_home = tk_get_tid();

	return er;
}

/* The table held to n entries, as on a machine with little memory (0: as made), for the tests */
EXPORT void om_store_cap( INT n )
{
	st_lock();
	st_cap = ( n > 0 ) ? n : 0;
	st_unlock();
}

/* How full the table is and how often room was made in it, for the tests */
EXPORT void om_store_stat( INT *p_used, INT *p_docs, INT *p_max, UW *p_evicted,
			   SZ *p_bytes, SZ *p_budget )
{
	INT	i, u = 0, d = 0;

	st_lock();
	(void)st_make();
	for ( i = 0; i < st_max; i++ ) {
		if ( st_tab[i].used ) u++;
		if ( st_tab[i].used && st_tab[i].doc != NULL ) d++;
	}
	if ( p_evicted != NULL ) *p_evicted = st_evicted;
	if ( p_bytes != NULL ) *p_bytes = st_bytes;
	if ( p_budget != NULL ) *p_budget = st_budget;
	if ( p_max != NULL ) *p_max = st_max;
	st_unlock();
	if ( p_used != NULL ) *p_used = u;
	if ( p_docs != NULL ) *p_docs = d;
}

EXPORT void om_store_empty( void )
{
	INT	i;

	st_lock();
	for ( i = 0; i < st_max; i++ ) {
		if ( st_tab[i].used ) {
			st_release(i);
		}
	}
	st_unlock();
}

/*
 * One record read by its identity, for a program that wants to open an
 * object rather than to draw a link to it. What comes back belongs to
 * the store.
 */
EXPORT T_TAD *om_store_get( CONST TS_UUID *id, INT recno )
{
	T_VOBJ	v;

	if ( id == NULL ) {
		return NULL;
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = *id;
	v.recno  = recno;

	return store_open(&v, NULL);
}

/*
 * A record written back to its file.
 *
 * What is written is the record as it now stands -- everything in it
 * that this system did not understand is kept as it was read, because
 * the writer keeps what it was given -- so a record saved here opens in
 * the program that wrote it with nothing lost.
 *
 * The file is written whole, from a buffer, rather than a piece at a
 * time: a record half written because the disk filled up is a record
 * that no longer opens anywhere, and a whole write either happens or
 * leaves the old file as it was up to the point it could not go on.
 */
/*
 * What the store holds of an object in memory, let go: its records and
 * what was remembered of its metadata. For an object that is thrown
 * away, so that nothing of it is drawn from memory afterwards.
 */
/* The volume an object is on, as the file manager knows it */
LOCAL ER vol_of_file( CONST TS_UUID *obj, TS_UUID *p_vol, void *arg )
{
	return knl_obfile_vuuid_of(obj, p_vol);
}

/*
 * The links of a record of the object 'host' made to say where their
 * objects are (design 8.3), as om_link_fix does to the text: no volume
 * for an object of host's own volume, that volume's UUID for one of
 * another; one whose object is on no volume attached, or on another
 * machine, as it is.
 */
LOCAL void links_fix_doc( T_TAD *doc, CONST TS_UUID *host )
{
	TS_UUID	hv, mine;
	INT	failed = 0;

	if ( doc == NULL || knl_obfile_vuuid_of(host, &hv) < E_OK ) {
		return;
	}
	if ( knl_obfile_machine(&mine) < E_OK ) knl_memset(&mine, 0, sizeof(mine));
	(void)tad_lnk_vol_doc(doc, &hv, &mine, vol_of_file, NULL, &failed);
	if ( failed > 0 ) {
		tm_printf((UB *)"om: %d links were not given their volume\n", failed);
	}
}

EXPORT void om_store_forget( CONST TS_UUID *id )
{
	INT	i;

	if ( id == NULL ) {
		return;
	}
	st_lock();
	for ( i = 0; i < st_max; i++ ) {
		if ( st_tab[i].used && same_id(&st_tab[i].id, id) ) {
			st_release(i);
		}
	}
	st_unlock();
}

/*
 * The record written back. What it linked to before and what it links
 * to now are compared, and the objects it links to more or less often
 * have their counts moved to match.
 */
LOCAL ER om_store_save_u( CONST TS_UUID *id, INT recno )
{
	T_VOBJ	v;
	INT	slot;
	UB	*buf, *was;
	SZ	size = 0, cap, was_len = 0;
	ER	er;

	if ( id == NULL ) {
		return E_PAR;
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = *id;
	slot = slot_of(id, recno);
	if ( slot < 0 || st_tab[slot].doc == NULL ) {
		return E_NOEXS;			/* nothing of it was read */
	}
	/* each link says on which volume its object is, in the record read as well as in what is written */
	links_fix_doc(st_tab[slot].doc, id);

	/* how large it comes to, then that much room */
	er = tad_write_mem(st_tab[slot].doc, NULL, 0, &size);
	if ( er < E_OK && size == 0 ) {
		return er;
	}
	cap = size + 1;
	buf = (UB *)Kmalloc(cap);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = tad_write_mem(st_tab[slot].doc, buf, cap, &size);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	/* each link says on which volume its object is, as seen from this one */
	(void)om_link_fix(&buf, &size, id);
	was = om_obj_record(id, recno, &was_len);
	er = om_obj_record_put(id, recno, OB_RT_TAD, 0, buf, size);
	meta_forget(id);			/* its date of change has moved */
	if ( er >= E_OK && !om_store_counts(id) ) {
		om_store_relink(id, was, was_len, buf, size);
	}
	if ( was != NULL ) {
		Kfree(was);
	}
	Kfree(buf);

	return er;
}

EXPORT ER om_store_save( CONST TS_UUID *id, INT recno )
{
	ER r;

	st_lock();
	r = om_store_save_u(id, recno);
	st_unlock();
	return r;
}

/*
 * The record as it now stands, as text. The caller owns what comes
 * back. This is what a change is taken back to: the record is the one
 * true copy of the object, so taking back a change is putting back
 * the record as it was, and everything drawn from it is drawn again.
 */
EXPORT ER om_store_snapshot( CONST TS_UUID *id, INT recno, UB **p_xml,
			     SZ *p_size )
{
	INT	slot;
	SZ	size = 0;
	UB	*buf;
	ER	er;

	if ( id == NULL || p_xml == NULL || p_size == NULL ) {
		return E_PAR;
	}
	slot = slot_of(id, recno);
	if ( slot < 0 || st_tab[slot].doc == NULL ) {
		return E_NOEXS;
	}
	/*
	 * Written once into room for about as much as last time: a record
	 * being edited changes little between two snapshots. Only when it
	 * has grown past that room is it written again, into room enough.
	 */
	size = st_tab[slot].snap + st_tab[slot].snap / 16 + 4096;
	buf = (UB *)Kmalloc(size + 1);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = tad_write_mem(st_tab[slot].doc, buf, size + 1, &size);
	if ( er == E_NOMEM && size > 0 ) {
		Kfree(buf);
		buf = (UB *)Kmalloc(size + 1);
		if ( buf == NULL ) {
			return E_NOMEM;
		}
		er = tad_write_mem(st_tab[slot].doc, buf, size + 1, &size);
	}
	if ( er < E_OK || size == 0 ) {
		Kfree(buf);
		return ( er < E_OK ) ? er : E_OBJ;
	}
	st_tab[slot].snap = size;
	buf[size] = 0;
	*p_xml = buf;
	*p_size = size;

	return E_OK;
}

/*
 * The record replaced by the text given. What the store kept is
 * given back, and the new record is what everything reads from now
 * on. Answers the new record.
 */
EXPORT T_TAD *om_store_restore( CONST TS_UUID *id, INT recno,
				CONST UB *xml, SZ size )
{
	INT	slot;
	T_TAD	*doc = NULL;
	UB	*copy;

	if ( id == NULL || xml == NULL || size == 0 ) {
		return NULL;
	}
	slot = slot_of(id, recno);
	if ( slot < 0 ) {
		return NULL;
	}
	copy = (UB *)Kmalloc(size + 1);
	if ( copy == NULL ) {
		return NULL;
	}
	knl_memcpy(copy, xml, size);
	copy[size] = 0;
	if ( tad_parse(copy, size, NULL, &doc) < E_OK || doc == NULL ) {
		Kfree(copy);
		return NULL;
	}
	if ( st_tab[slot].doc != NULL ) {
		tad_free(st_tab[slot].doc);
	}
	if ( st_tab[slot].xml != NULL ) {
		Kfree(st_tab[slot].xml);
	}
	st_tab[slot].doc = doc;
	st_tab[slot].xml = copy;

	return doc;
}

/* What an object is written on, by identity */
EXPORT ER om_store_paper( CONST TS_UUID *id, UW *p_colour )
{
	T_VOBJ	v;

	if ( id == NULL ) {
		return E_PAR;
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = *id;
	v.recno  = 0;

	return store_paper(&v, p_colour, NULL);
}

/* And what it is called */
EXPORT INT om_store_name( CONST TS_UUID *id, UB *buf, INT max )
{
	T_VOBJ	v;

	if ( id == NULL || buf == NULL || max <= 1 ) {
		return -1;
	}
	knl_memset(&v, 0, sizeof(v));
	v.target = *id;
	v.recno  = 0;

	return store_name(&v, buf, max, NULL);
}
