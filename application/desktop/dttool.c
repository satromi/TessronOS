/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttool.c
 *	The system's own tools (design 16.5.17)
 *
 *	Three tools that look at the store as a whole rather than at one
 *	object, each shown in a window whose page the tool makes:
 *
 *	  屑実身操作		the objects no link reaches any more; each
 *				can be carried out, which links to it again,
 *				or thrown away for good
 *	  実身/仮身検索		the objects whose name, relationship tags or
 *				text hold some words, or whose dates fall
 *				between two days
 *	  仮身ネットワーク	what an object links to, and what those link
 *				to, a column to each step, with an arrow
 *				from each object to each it links to
 *
 *	A tool's page is a record made here, sealed like the 原紙箱's: it is
 *	drawn, pressed, opened from and carried from by the same code as any
 *	page, and nothing in it is changed or saved. Carrying an object out
 *	of a tool puts a link to it where it is let go; the count of links
 *	to it goes up when that record is saved.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dbox.h>
#include <ts/hid.h>
#include <ts/om.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/uuid.h>
#include <ts/fn.h>
#include <ts/mn.h>
#include <ts/sysdef.h>
#include "desktop.h"

#define KEY_C		0x06
#define KEY_G		0x0A
#define KEY_N		0x11
#define KEY_X		0x1B
#define KEY_DEL		0x4C
#define MOD_CTRL	( HID_MOD_LCTRL | HID_MOD_RCTRL )

/* What the items of the tools' menus are, as the tools' commands */
#define TC_FULL		501
#define TC_REFRESH	502
#define TC_COPY		503
#define TC_CUT		504
#define TC_DELETE	505
#define TC_RESCAN	506
#define TC_FOCUS	508
#define TC_SEARCH	509
#define TC_NET		510
#define TC_GRAPH	511

#define TL_MAX		256		/* objects a tool's page shows */

/* ---------------------------------------------------------------- words */

LOCAL INT cat( UB *buf, INT at, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && at < max - 1 ) {
		buf[at++] = (UB)*s++;
	}
	buf[at] = 0;

	return at;
}

LOCAL INT cat_n( UB *buf, INT at, INT max, INT v )
{
	char	num[12];
	INT	k = 0;

	if ( v < 0 ) {
		v = 0;
	}
	do {
		num[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && k < 11 );
	while ( k > 0 && at < max - 1 ) {
		buf[at++] = (UB)num[--k];
	}
	buf[at] = 0;

	return at;
}

/* The objects the links taken in a tool's page point at */
LOCAL INT picked_targets( DTWIN *d, TS_UUID *ids, INT max )
{
	T_VOBJ	v;
	INT	i, n = 0;

	for ( i = 0; d->fig != NULL && i < d->fig->nsh && n < max; i++ ) {
		if ( ed_picked(d, i) && ed_link_at(d, i, &v) ) {
			ids[n++] = v.target;
		}
	}

	return n;
}

/* Where a tool's window stands when it is first opened */
LOCAL void tool_rect( T_DPRECT *o, INT x, INT y, INT w, INT h )
{
	o->left = x;
	o->top = y;
	o->right = x + w;
	o->bottom = y + h;
}

/* ---------------------------------------------------------------- 屑実身操作 */

/*
 * The objects no link reaches: a count of nought in the metadata, and
 * not one the system itself holds.
 */
LOCAL T_TAD *trash_record( INT *p_n )
{
	TS_UUID	*all, roots[8];
	INT	n, nr, i, k, keep = 0;
	T_TAD	*rec;

	n = om_store_objects(NULL, 0);
	all = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( n + 1 ));
	if ( all == NULL ) {
		return NULL;
	}
	n = om_store_objects(all, n);
	nr = dt_roots(roots, 8);
	for ( i = 0; i < n && keep < TL_MAX; i++ ) {
		BOOL	root = FALSE;

		for ( k = 0; k < nr; k++ ) {
			if ( ts_uuid_cmp(&all[i], &roots[k]) == 0 ) {
				root = TRUE;
			}
		}
		if ( !root && om_store_refs(&all[i]) == 0 ) {
			all[keep++] = all[i];
		}
	}
	rec = dt_list_record("屑実身操作", all, NULL, keep);
	Kfree(all);
	if ( p_n != NULL ) {
		*p_n = keep;
	}

	return rec;
}

EXPORT void dt_trash_open( void )
{
	DTWIN		*d = dt_tool_window(DT_TOOL_TRASH);
	T_DPRECT	o;

	if ( d != NULL ) {
		dt_tool_refresh(DT_TOOL_TRASH);
		wm_raise(d->wid);
		wm_focus(d->wid);
		wm_composite();
		return;
	}
	tool_rect(&o, 140, 100, 600, 500);
	(void)dt_open_made(trash_record(NULL), "屑実身操作", DT_TOOL_TRASH, &o);
	wm_composite();
}

/*
 * The objects taken, thrown away for good, once it has been agreed to.
 * A window showing one is closed first; what they linked to loses those
 * links, and may itself become a 屑実身.
 */
LOCAL void trash_delete( DTWIN *d )
{
	TS_UUID	ids[TL_MAX];
	UB	l1[WM_LABEL_MAX * 2];
	INT	n, i, at = 0;

	n = picked_targets(d, ids, TL_MAX);
	if ( n == 0 ) {
		return;
	}
	if ( n == 1 ) {
		UB	nm[TAD_NAME_MAX];

		if ( om_store_name(&ids[0], nm, TAD_NAME_MAX) < 0 ) {
			nm[0] = 0;
		}
		at = cat(l1, at, sizeof(l1), "実身「");
		at = cat(l1, at, sizeof(l1), (CONST char *)nm);
		at = cat(l1, at, sizeof(l1), "」を完全に削除しますか？");
	} else {
		at = cat_n(l1, at, sizeof(l1), n);
		at = cat(l1, at, sizeof(l1), "件の実身を完全に削除しますか？");
	}
	if ( !dt_confirm(d, (CONST char *)l1, "この操作は取り消せません。",
			 "いいえ", "はい") ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		DTWIN	*w = dt_showing(&ids[i]);

		while ( w != NULL ) {
			dt_close(w);
			w = dt_showing(&ids[i]);
		}
		(void)om_store_delete(&ids[i]);
	}
	dt_tool_refresh(DT_TOOL_TRASH);
}

/*
 * Every count made again from the records, once it has been agreed to,
 * and what came of it told.
 */
LOCAL void trash_rescan( DTWIN *d )
{
	TS_UUID	roots[8];
	INT	nr, total = 0, changed = 0, at = 0;
	UB	l1[WM_LABEL_MAX];

	if ( !dt_confirm(d, "全実身の参照数を、実際に置かれている仮身の数から数え直します。",
			 "開いている文書で保存していない仮身は数えません。",
			 "取消", "数え直す") ) {
		return;
	}
	nr = dt_roots(roots, 8);
	if ( om_store_recount(roots, nr, &total, &changed) < E_OK ) {
		dt_tell(d, "参照数を数え直せませんでした。", NULL);
		return;
	}
	dt_tool_refresh(DT_TOOL_TRASH);
	at = cat_n(l1, at, sizeof(l1), total);
	if ( changed > 0 ) {
		at = cat(l1, at, sizeof(l1), "件の実身のうち、");
		at = cat_n(l1, at, sizeof(l1), changed);
		at = cat(l1, at, sizeof(l1), "件の参照数を修正しました。");
	} else {
		at = cat(l1, at, sizeof(l1), "件の実身の参照数はすべて正しい値でした。");
	}
	dt_tell(d, (CONST char *)l1, NULL);
}

/* ---------------------------------------------------------------- 実身/仮身検索 */

/*
 * The last question asked, kept so that asking again starts from it,
 * and the objects that answered it.
 */
LOCAL T_DTQUERY	sr_q;
LOCAL BOOL	sr_asked = FALSE;

/* An ASCII letter made small, so that the words match either way */
LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c + 'a' - 'A' ) : c;
}

/* Whether 'what' is somewhere in the first 'n' bytes of 'in' */
LOCAL BOOL has( CONST UB *in, SZ n, CONST UB *what )
{
	SZ	i, k;

	if ( what[0] == 0 ) {
		return TRUE;
	}
	for ( i = 0; i < n; i++ ) {
		for ( k = 0; what[k] != 0 && i + k < n
			     && lower(in[i + k]) == lower(what[k]); k++ ) {
			;
		}
		if ( what[k] == 0 ) {
			return TRUE;
		}
	}

	return FALSE;
}

/* Whether any value of an attribute in the record holds the words */
LOCAL BOOL attr_has( CONST UB *buf, SZ len, CONST char *attr, CONST UB *what )
{
	SZ	i, k, n = 0;

	while ( attr[n] != 0 ) {
		n++;
	}
	for ( i = 0; i + n < len; i++ ) {
		for ( k = 0; k < n && buf[i + k] == (UB)attr[k]; k++ ) {
			;
		}
		if ( k < n ) {
			continue;
		}
		for ( k = i + n; k < len && buf[k] != '"'; k++ ) {
			;
		}
		if ( k > i + n && has(buf + i + n, k - ( i + n ), what) ) {
			return TRUE;
		}
		i = k;
	}

	return FALSE;
}

/*
 * The words of a record, without its markup: everything outside the
 * tags, with the five named characters put back. Done in place.
 */
LOCAL SZ text_of( UB *buf, SZ len )
{
	SZ	i, o = 0;
	BOOL	in_tag = FALSE;

	for ( i = 0; i < len; i++ ) {
		UB	c = buf[i];

		if ( in_tag ) {
			if ( c == '>' ) {
				in_tag = FALSE;
			}
			continue;
		}
		if ( c == '<' ) {
			in_tag = TRUE;
			continue;
		}
		if ( c == '&' ) {
			CONST char	*nm[] = { "&amp;", "&lt;", "&gt;", "&quot;", "&apos;" };
			CONST UB	ch[] = { '&', '<', '>', '"', '\'' };
			INT		k;
			SZ		j;

			for ( k = 0; k < 5; k++ ) {
				for ( j = 0; nm[k][j] != 0 && i + j < len
					     && buf[i + j] == (UB)nm[k][j]; j++ ) {
					;
				}
				if ( nm[k][j] == 0 ) {
					c = ch[k];
					i += j - 1;
					break;
				}
			}
		}
		buf[o++] = c;
	}
	buf[o] = 0;

	return o;
}

/* "YYYY-MM-DD" as a number that sorts as the days do; 0 when not a day */
LOCAL INT day_of( CONST UB *s )
{
	INT	v = 0, n = 0, i;

	for ( i = 0; s[i] != 0 && n < 8; i++ ) {
		if ( s[i] >= '0' && s[i] <= '9' ) {
			v = v * 10 + ( s[i] - '0' );
			n++;
		} else if ( s[i] != '-' && s[i] != '/' && s[i] != ' ' ) {
			break;
		}
	}

	return ( n == 8 ) ? v : 0;
}

/* Whether one object answers the question */
LOCAL BOOL sr_match( CONST TS_UUID *id, CONST T_DTQUERY *q )
{
	UB	nm[TAD_NAME_MAX];
	BOOL	hit = FALSE;

	if ( q->date_kind != DQ_DATE_NONE ) {
		T_OMINFO	info;
		CONST UB	*when;
		INT		day, from = day_of(q->from), to = day_of(q->to);

		if ( om_store_info(id, &info) < E_OK ) {
			return FALSE;
		}
		when = ( q->date_kind == DQ_DATE_MADE ) ? info.made
		     : ( q->date_kind == DQ_DATE_UPDATED ) ? info.updated
		     : info.accessed;
		day = day_of(when);
		if ( day == 0 || ( from != 0 && day < from )
		  || ( to != 0 && day > to ) ) {
			return FALSE;
		}
		if ( q->text[0] == 0 ) {
			return TRUE;		/* the dates alone */
		}
	}
	if ( q->target == DQ_NAME || q->target == DQ_ALL ) {
		if ( om_store_name(id, nm, TAD_NAME_MAX) >= 0
		  && has(nm, (SZ)TAD_NAME_MAX, q->text) ) {
			return TRUE;
		}
	}
	if ( q->target == DQ_RELATION || q->target == DQ_ALL ) {
		UB	tags[WM_LABEL_MAX * 2];
		SZ	n;

		if ( om_store_relationship(id, tags, sizeof(tags)) > 0 ) {
			for ( n = 0; tags[n] != 0; n++ ) {
				;
			}
			if ( has(tags, n, q->text) ) {
				return TRUE;
			}
		}
	}
	if ( q->target == DQ_TEXT || q->target == DQ_ALL
	  || q->target == DQ_RELATION ) {
		SZ	len = 0;
		UB	*buf = om_store_read(id, 0, &len);

		if ( buf != NULL ) {
			/* the tags the links in the record carry */
			if ( q->target != DQ_TEXT ) {
				hit = attr_has(buf, len, "relationship=\"", q->text);
			}
			if ( !hit && q->target != DQ_RELATION ) {
				len = text_of(buf, len);
				hit = has(buf, len, q->text);
			}
			Kfree(buf);
		}
	}

	return hit;
}

/* The question put to every object in the store; the page of those that answer */
LOCAL T_TAD *sr_record( CONST T_DTQUERY *q, INT *p_total, INT *p_hits,
			UB *title, INT max )
{
	TS_UUID	*all;
	INT	n, i, hits = 0, at = 0;
	T_TAD	*rec;

	n = om_store_objects(NULL, 0);
	all = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( n + 1 ));
	if ( all == NULL ) {
		return NULL;
	}
	n = om_store_objects(all, n);
	for ( i = 0; i < n && hits < TL_MAX; i++ ) {
		if ( sr_match(&all[i], q) ) {
			all[hits++] = all[i];
		}
	}
	at = cat(title, at, max, "検索結果");
	if ( q->text[0] != 0 ) {
		at = cat(title, at, max, "「");
		at = cat(title, at, max, (CONST char *)q->text);
		at = cat(title, at, max, "」");
	}
	at = cat(title, at, max, " ");
	at = cat_n(title, at, max, hits);
	at = cat(title, at, max, "件");
	rec = dt_list_record((CONST char *)title, all, NULL, hits);
	Kfree(all);
	if ( p_total != NULL ) *p_total = n;
	if ( p_hits != NULL ) *p_hits = hits;

	return rec;
}

/*
 * The question asked in a panel in the window it was asked from, then
 * put to the store, and what answered shown in a window of its own --
 * the one that showed the last answers, if it is still open.
 */
EXPORT void dt_search_open( DTWIN *from )
{
	DTWIN		*w = dt_tool_window(DT_TOOL_SEARCH);
	DTWIN		*ask = ( from != NULL ) ? from : w;
	T_TAD		*rec;
	T_DPRECT	o;
	UB		title[WM_TITLE_MAX];
	INT		total = 0, hits = 0;

	if ( ask == NULL ) {
		return;
	}
	if ( !sr_asked ) {
		knl_memset(&sr_q, 0, sizeof(sr_q));
		sr_q.target = DQ_ALL;
		sr_q.date_kind = DQ_DATE_NONE;
	}
	if ( !dt_search_form(ask, &sr_q) ) {
		return;
	}
	sr_asked = TRUE;
	rec = sr_record(&sr_q, &total, &hits, title, sizeof(title));
	if ( rec == NULL ) {
		return;
	}
	if ( hits == 0 ) {
		tad_free(rec);
		dt_tell(ask, "該当する実身はありませんでした。", NULL);
		return;
	}
	if ( w != NULL ) {
		dt_remade(w, rec);
		wm_set_title(w->wid, (CONST char *)title);
		wm_raise(w->wid);
		wm_focus(w->wid);
	} else {
		tool_rect(&o, 200, 120, 600, 500);
		(void)dt_open_made(rec, (CONST char *)title, DT_TOOL_SEARCH, &o);
	}
	wm_composite();
}

/* ---------------------------------------------------------------- 仮身ネットワーク */

#define NW_MAX		128		/* objects one network shows */
#define NW_W		200		/* each one's box */
#define NW_GAP_X	100		/* between one column and the next */
#define NW_GAP_Y	20		/* between one row and the next */
#define NW_PAD		40
#define NW_CHSZ		14
#define NW_EDGE_MAX	512

typedef struct {
	TS_UUID	id;
	INT	depth;
	INT	x, y;
} NWNODE;

typedef struct {
	INT	n, nedge;
	NWNODE	node[NW_MAX];
	UH	from[NW_EDGE_MAX], to[NW_EDGE_MAX];
} NWNET;

LOCAL TS_UUID	nw_start;
LOCAL BOOL	nw_graph = FALSE;	/* shown as a graph, not in columns */

LOCAL INT nw_find( CONST NWNET *g, CONST TS_UUID *id )
{
	INT	i;

	for ( i = 0; i < g->n; i++ ) {
		if ( ts_uuid_cmp(&g->node[i].id, id) == 0 ) {
			return i;
		}
	}

	return -1;
}

/*
 * The network from one object: step by step outwards, each object met
 * once, at the first step it is met at. A link to an object already met
 * is still an arrow, back or across, so that a loop shows as a loop.
 */
LOCAL void nw_build( NWNET *g, CONST TS_UUID *start )
{
	TS_UUID	*lk;
	INT	head = 0, n, j, k;

	g->n = 1;
	g->nedge = 0;
	g->node[0].id = *start;
	g->node[0].depth = 0;
	lk = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	if ( lk == NULL ) {
		return;
	}
	while ( head < g->n ) {
		SZ	len = 0;
		UB	*buf = om_store_read(&g->node[head].id, 0, &len);

		n = 0;
		if ( buf != NULL ) {
			n = om_store_links(buf, len, lk, 256);
			if ( n > 256 ) n = 256;
			Kfree(buf);
		}
		for ( j = 0; j < n; j++ ) {
			k = nw_find(g, &lk[j]);
			if ( k < 0 ) {
				if ( g->n >= NW_MAX || om_store_refs(&lk[j]) < 0 ) {
					continue;	/* too many, or not in the store */
				}
				k = g->n++;
				g->node[k].id = lk[j];
				g->node[k].depth = g->node[head].depth + 1;
			}
			{
				INT	e;
				BOOL	dup = FALSE;

				for ( e = 0; e < g->nedge; e++ ) {
					if ( g->from[e] == head && g->to[e] == k ) {
						dup = TRUE;
					}
				}
				if ( !dup && g->nedge < NW_EDGE_MAX ) {
					g->from[g->nedge] = (UH)head;
					g->to[g->nedge] = (UH)k;
					g->nedge++;
				}
			}
		}
		head++;
	}
	Kfree(lk);
}

LOCAL SZ nw_put( UB *buf, SZ at, SZ max, CONST char *s )
{
	while ( *s != 0 && at + 1 < max ) {
		buf[at++] = (UB)*s++;
	}
	buf[at] = 0;

	return at;
}

LOCAL SZ nw_num( UB *buf, SZ at, SZ max, INT v )
{
	UB	t[16];
	INT	n = cat_n(t, 0, sizeof(t), v);

	(void)n;

	return nw_put(buf, at, max, (CONST char *)t);
}

/* A name made safe to stand in an attribute */
LOCAL SZ nw_esc( UB *buf, SZ at, SZ max, CONST UB *s )
{
	for ( ; *s != 0 && at + 7 < max; s++ ) {
		switch ( *s ) {
		case '&':	at = nw_put(buf, at, max, "&amp;");	break;
		case '<':	at = nw_put(buf, at, max, "&lt;");	break;
		case '>':	at = nw_put(buf, at, max, "&gt;");	break;
		case '"':	at = nw_put(buf, at, max, "&quot;");	break;
		default:	buf[at++] = *s;				break;
		}
	}
	buf[at] = 0;

	return at;
}

/*
 * The network laid out as a figure: a column to each step, the objects
 * of a step one under another in the order they were met, each a
 * closed virtual object as wide as the column; and an arrow from the
 * right edge of each object to the left edge of each it links to.
 */
LOCAL T_TAD *nw_record( NWNET *g, UB *title, INT max )
{
	UB	*xml;
	SZ	cap, at = 0;
	INT	row[NW_MAX], i, e, band = NW_CHSZ + 11;
	T_TAD	*doc = NULL;
	UB	nm[TAD_NAME_MAX];

	for ( i = 0; i < NW_MAX; i++ ) {
		row[i] = 0;
	}
	for ( i = 0; i < g->n; i++ ) {
		INT	dp = g->node[i].depth;

		if ( dp >= NW_MAX ) dp = NW_MAX - 1;
		g->node[i].x = NW_PAD + dp * ( NW_W + NW_GAP_X );
		g->node[i].y = NW_PAD + row[dp] * ( band + NW_GAP_Y );
		row[dp]++;
	}
	cap = 512 + (SZ)g->n * ( 480 + TAD_NAME_MAX * 5 ) + (SZ)g->nedge * 200;
	xml = (UB *)Kmalloc(cap);
	if ( xml == NULL ) {
		return NULL;
	}
	if ( om_store_name(&g->node[0].id, nm, TAD_NAME_MAX) < 0 ) {
		nm[0] = 0;
	}
	i = cat(title, 0, max, "仮身ネットワーク: ");
	(void)cat(title, i, max, (CONST char *)nm);

	at = nw_put(xml, at, cap, "<tad version=\"1.0\" encoding=\"UTF-8\""
				  " filename=\"仮身ネットワーク\"><figure>");
	/* the arrows first, so that the objects stand over their ends */
	for ( e = 0; e < g->nedge; e++ ) {
		CONST NWNODE	*a = &g->node[g->from[e]];
		CONST NWNODE	*b = &g->node[g->to[e]];
		INT		x1 = a->x + NW_W, y1 = a->y + band / 2;
		INT		x2 = b->x, y2 = b->y + band / 2;

		if ( b->x <= a->x ) {
			/* back or across: from the foot of the one to the head of the other */
			x1 = a->x + NW_W / 2;  y1 = a->y + band;
			x2 = b->x + NW_W / 2;  y2 = ( b->y > a->y ) ? b->y : b->y + band;
		}
		at = nw_put(xml, at, cap, "<line lineType=\"0\" lineWidth=\"1\""
			    " l_pat=\"1\" f_pat=\"0\" start_arrow=\"0\""
			    " end_arrow=\"1\" arrow_type=\"filled\" points=\"");
		at = nw_num(xml, at, cap, x1);
		at = nw_put(xml, at, cap, ",");
		at = nw_num(xml, at, cap, y1);
		at = nw_put(xml, at, cap, " ");
		at = nw_num(xml, at, cap, x2);
		at = nw_put(xml, at, cap, ",");
		at = nw_num(xml, at, cap, y2);
		at = nw_put(xml, at, cap, "\"/>");
	}
	for ( i = 0; i < g->n; i++ ) {
		char	txt[40];

		if ( ts_uuid_to_str(&g->node[i].id, txt, sizeof(txt)) < E_OK ) {
			continue;
		}
		if ( om_store_name(&g->node[i].id, nm, TAD_NAME_MAX) < 0 ) {
			nm[0] = 0;
		}
		at = nw_put(xml, at, cap, "<link id=\"");
		at = nw_put(xml, at, cap, txt);
		at = nw_put(xml, at, cap, "_0.xtad\" name=\"");
		at = nw_esc(xml, at, cap, nm);
		at = nw_put(xml, at, cap, "\" vobjleft=\"");
		at = nw_num(xml, at, cap, g->node[i].x);
		at = nw_put(xml, at, cap, "\" vobjtop=\"");
		at = nw_num(xml, at, cap, g->node[i].y);
		at = nw_put(xml, at, cap, "\" vobjright=\"");
		at = nw_num(xml, at, cap, g->node[i].x + NW_W);
		at = nw_put(xml, at, cap, "\" vobjbottom=\"");
		at = nw_num(xml, at, cap, g->node[i].y + band);
		at = nw_put(xml, at, cap, "\" height=\"");
		at = nw_num(xml, at, cap, band);
		at = nw_put(xml, at, cap, "\" chsz=\"");
		at = nw_num(xml, at, cap, NW_CHSZ);
		/* the one it starts from stands out */
		at = nw_put(xml, at, cap, ( i == 0 )
			    ? "\" frcol=\"#000000\" chcol=\"#000000\""
			      " tbcol=\"#e0e0ff\" bgcol=\"#ffffff\""
			    : "\" frcol=\"#000000\" chcol=\"#000000\""
			      " tbcol=\"#ffffff\" bgcol=\"#ffffff\"");
		at = nw_put(xml, at, cap, " pictdisp=\"true\" namedisp=\"true\""
			    " framedisp=\"true\" autoopen=\"false\"/>");
	}
	at = nw_put(xml, at, cap, "</figure></tad>");
	if ( tad_parse(xml, at, NULL, &doc) < E_OK ) {
		doc = NULL;
	}
	Kfree(xml);

	return doc;
}


/* ---------------------------------------------------------------- the graph */

/*
 * The same network laid out as a graph: each object a circle, larger
 * the more links in the network reach it, with its virtual object
 * beside it, and a line for each link. Where the circles go is found
 * by letting them push each other apart while the links pull the ends
 * of each together, for a fixed number of steps with the moves made
 * smaller each step. Places are kept in sixteenths of a pixel.
 */
#define GR_STEPS	160
#define GR_SIDE		720		/* the square they are laid in */
#define GR_K		( 110 * 16 )	/* the length a link settles to */

LOCAL D isqrt( D v )
{
	D	r = 0, b = (D)1 << 40;

	if ( v <= 0 ) {
		return 0;
	}
	while ( b > v ) {
		b >>= 2;
	}
	while ( b != 0 ) {
		if ( v >= r + b ) {
			v -= r + b;
			r = ( r >> 1 ) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}

	return r;
}

LOCAL void gr_layout( NWNET *g, D *px, D *py )
{
	D	*fx, *fy;
	INT	i, k, e, step;

	fx = (D *)Kmalloc(sizeof(D) * (SZ)g->n * 2);
	if ( fx == NULL ) {
		return;
	}
	fy = fx + g->n;
	/*
	 * Round a circle to begin with, sixteen to a ring and each ring
	 * further out, the one started from in the middle. The directions
	 * are a table: cosine and sine by thousandths.
	 */
	for ( i = 0; i < g->n; i++ ) {
		LOCAL CONST INT	cs[16][2] = {
			{ 1000, 0 }, { 924, 383 }, { 707, 707 }, { 383, 924 },
			{ 0, 1000 }, { -383, 924 }, { -707, 707 }, { -924, 383 },
			{ -1000, 0 }, { -924, -383 }, { -707, -707 }, { -383, -924 },
			{ 0, -1000 }, { 383, -924 }, { 707, -707 }, { 924, -383 }
		};
		D	r = ( i == 0 ) ? 0 : (D)( 120 + ( ( i - 1 ) / 16 ) * 60 ) * 16;

		px[i] = ( GR_SIDE / 2 ) * 16 + r * cs[( i - 1 ) & 15][0] / 1000;
		py[i] = ( GR_SIDE / 2 ) * 16 + r * cs[( i - 1 ) & 15][1] / 1000;
	}
	for ( step = 0; step < GR_STEPS; step++ ) {
		D	limit = ( (D)( GR_STEPS - step ) * 40 * 16 ) / GR_STEPS + 16;

		for ( i = 0; i < g->n; i++ ) {
			fx[i] = fy[i] = 0;
		}
		/* everything pushes everything else away */
		for ( i = 0; i < g->n; i++ ) {
			for ( k = i + 1; k < g->n; k++ ) {
				D	dx = px[i] - px[k], dy = py[i] - py[k];
				D	d2 = dx * dx + dy * dy, d, f;

				if ( d2 < 256 ) {
					dx = ( i % 2 ) ? 16 : -16;
					dy = ( k % 2 ) ? 16 : -16;
					d2 = 512;
				}
				d = isqrt(d2);
				f = (D)GR_K * GR_K / d;		/* k^2 / d */
				fx[i] += dx * f / d;  fy[i] += dy * f / d;
				fx[k] -= dx * f / d;  fy[k] -= dy * f / d;
			}
		}
		/* and each link pulls its ends together */
		for ( e = 0; e < g->nedge; e++ ) {
			INT	a = g->from[e], b = g->to[e];
			D	dx = px[a] - px[b], dy = py[a] - py[b];
			D	d = isqrt(dx * dx + dy * dy), f;

			if ( a == b || d == 0 ) {
				continue;
			}
			f = d * d / GR_K;			/* d^2 / k */
			fx[a] -= dx * f / d;  fy[a] -= dy * f / d;
			fx[b] += dx * f / d;  fy[b] += dy * f / d;
		}
		/* each moves along its force, no further than the step allows */
		for ( i = 0; i < g->n; i++ ) {
			D	f = isqrt(fx[i] * fx[i] + fy[i] * fy[i]);

			if ( f == 0 ) {
				continue;
			}
			if ( f > limit ) {
				fx[i] = fx[i] * limit / f;
				fy[i] = fy[i] * limit / f;
			}
			px[i] += fx[i];
			py[i] += fy[i];
		}
	}
	Kfree(fx);
}

LOCAL T_TAD *gr_record( NWNET *g, UB *title, INT max )
{
	UB	*xml;
	SZ	cap, at = 0;
	INT	i, e, band = NW_CHSZ + 11, in[NW_MAX];
	D	*px, *py, minx, miny;
	T_TAD	*doc = NULL;
	UB	nm[TAD_NAME_MAX];

	px = (D *)Kmalloc(sizeof(D) * (SZ)g->n * 2);
	if ( px == NULL ) {
		return NULL;
	}
	py = px + g->n;
	gr_layout(g, px, py);
	/* the whole moved so that nothing is left of or above the margin */
	minx = px[0];
	miny = py[0];
	for ( i = 0; i < g->n; i++ ) {
		if ( px[i] < minx ) minx = px[i];
		if ( py[i] < miny ) miny = py[i];
		in[i] = 0;
	}
	for ( e = 0; e < g->nedge; e++ ) {
		in[g->to[e]]++;
	}
	for ( i = 0; i < g->n; i++ ) {
		g->node[i].x = (INT)( ( px[i] - minx ) / 16 ) + NW_PAD + 30;
		g->node[i].y = (INT)( ( py[i] - miny ) / 16 ) + NW_PAD + 30;
	}
	Kfree(px);

	cap = 512 + (SZ)g->n * ( 700 + TAD_NAME_MAX * 5 ) + (SZ)g->nedge * 160;
	xml = (UB *)Kmalloc(cap);
	if ( xml == NULL ) {
		return NULL;
	}
	if ( om_store_name(&g->node[0].id, nm, TAD_NAME_MAX) < 0 ) {
		nm[0] = 0;
	}
	i = cat(title, 0, max, "仮身グラフ: ");
	(void)cat(title, i, max, (CONST char *)nm);

	at = nw_put(xml, at, cap, "<tad version=\"1.0\" encoding=\"UTF-8\""
				  " filename=\"仮身グラフ\"><figure>");
	for ( e = 0; e < g->nedge; e++ ) {
		CONST NWNODE	*a = &g->node[g->from[e]];
		CONST NWNODE	*b = &g->node[g->to[e]];

		if ( a == b ) {
			continue;
		}
		at = nw_put(xml, at, cap, "<line lineType=\"0\" lineWidth=\"1\""
			    " l_pat=\"47\" f_pat=\"0\" start_arrow=\"0\""
			    " end_arrow=\"0\" points=\"");
		at = nw_num(xml, at, cap, a->x);
		at = nw_put(xml, at, cap, ",");
		at = nw_num(xml, at, cap, a->y);
		at = nw_put(xml, at, cap, " ");
		at = nw_num(xml, at, cap, b->x);
		at = nw_put(xml, at, cap, ",");
		at = nw_num(xml, at, cap, b->y);
		at = nw_put(xml, at, cap, "\"/>");
	}
	for ( i = 0; i < g->n; i++ ) {
		char	txt[40];
		INT	r = 6 + 3 * in[i];

		if ( r > 30 ) {
			r = 30;
		}
		/* the circle: the one started from in its own colour */
		at = nw_put(xml, at, cap, "<ellipse lineType=\"0\" lineWidth=\"1\""
			    " l_pat=\"1\" f_pat=\"");
		at = nw_put(xml, at, cap, ( i == 0 ) ? "36" : "40");
		at = nw_put(xml, at, cap, "\" cx=\"");
		at = nw_num(xml, at, cap, g->node[i].x);
		at = nw_put(xml, at, cap, "\" cy=\"");
		at = nw_num(xml, at, cap, g->node[i].y);
		at = nw_put(xml, at, cap, "\" rx=\"");
		at = nw_num(xml, at, cap, r);
		at = nw_put(xml, at, cap, "\" ry=\"");
		at = nw_num(xml, at, cap, r);
		at = nw_put(xml, at, cap, "\"/>");

		if ( ts_uuid_to_str(&g->node[i].id, txt, sizeof(txt)) < E_OK ) {
			continue;
		}
		if ( om_store_name(&g->node[i].id, nm, TAD_NAME_MAX) < 0 ) {
			nm[0] = 0;
		}
		/* and beside it, the object itself */
		at = nw_put(xml, at, cap, "<link id=\"");
		at = nw_put(xml, at, cap, txt);
		at = nw_put(xml, at, cap, "_0.xtad\" name=\"");
		at = nw_esc(xml, at, cap, nm);
		at = nw_put(xml, at, cap, "\" vobjleft=\"");
		at = nw_num(xml, at, cap, g->node[i].x + r + 4);
		at = nw_put(xml, at, cap, "\" vobjtop=\"");
		at = nw_num(xml, at, cap, g->node[i].y - band / 2);
		at = nw_put(xml, at, cap, "\" vobjright=\"");
		at = nw_num(xml, at, cap, g->node[i].x + r + 4 + 150);
		at = nw_put(xml, at, cap, "\" vobjbottom=\"");
		at = nw_num(xml, at, cap, g->node[i].y - band / 2 + band);
		at = nw_put(xml, at, cap, "\" height=\"");
		at = nw_num(xml, at, cap, band);
		at = nw_put(xml, at, cap, "\" chsz=\"");
		at = nw_num(xml, at, cap, NW_CHSZ);
		at = nw_put(xml, at, cap, "\" frcol=\"#000000\" chcol=\"#000000\""
			    " tbcol=\"#ffffff\" bgcol=\"#ffffff\""
			    " pictdisp=\"true\" namedisp=\"true\""
			    " framedisp=\"true\" autoopen=\"false\"/>");
	}
	at = nw_put(xml, at, cap, "</figure></tad>");
	if ( tad_parse(xml, at, NULL, &doc) < E_OK ) {
		doc = NULL;
	}
	Kfree(xml);

	return doc;
}

/* The network from one object, shown in the network's window */
LOCAL void nw_show( CONST TS_UUID *start )
{
	DTWIN		*w = dt_tool_window(DT_TOOL_NETWORK);
	NWNET		*g;
	T_TAD		*rec;
	T_DPRECT	o;
	UB		title[WM_TITLE_MAX];

	g = (NWNET *)Kmalloc(sizeof(NWNET));
	if ( g == NULL ) {
		return;
	}
	nw_start = *start;
	nw_build(g, start);
	rec = nw_graph ? gr_record(g, title, sizeof(title))
		       : nw_record(g, title, sizeof(title));
	Kfree(g);
	if ( rec == NULL ) {
		return;
	}
	if ( w != NULL ) {
		dt_remade(w, rec);
		wm_set_title(w->wid, (CONST char *)title);
		wm_raise(w->wid);
		wm_focus(w->wid);
	} else {
		tool_rect(&o, 80, 60, 900, 600);
		(void)dt_open_made(rec, (CONST char *)title, DT_TOOL_NETWORK, &o);
	}
	wm_composite();
}

EXPORT void dt_network_open( DTWIN *from, CONST T_VOBJ *v )
{
	(void)from;
	if ( v != NULL ) {
		nw_show(&v->target);
	}
}

/* ---------------------------------------------------------------- shared */

/* A tool's page made again from the store, if the tool is open */
EXPORT void dt_tool_refresh( UINT tool )
{
	DTWIN	*d = dt_tool_window(tool);

	if ( d == NULL ) {
		return;
	}
	switch ( tool ) {
	case DT_TOOL_TRASH:
		dt_remade(d, trash_record(NULL));
		break;
	case DT_TOOL_SEARCH:
		if ( sr_asked ) {
			UB	title[WM_TITLE_MAX];
			T_TAD	*rec = sr_record(&sr_q, NULL, NULL, title,
						 sizeof(title));

			if ( rec != NULL ) {
				dt_remade(d, rec);
				wm_set_title(d->wid, (CONST char *)title);
			}
		}
		break;
	case DT_TOOL_NETWORK:
		nw_show(&nw_start);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the menus */

typedef struct {
	CONST char	*code;
	INT		cmd;
} TOOLCMD;

LOCAL CONST TOOLCMD tool_cmds[] = {
	{ "fullscreen",	TC_FULL },
	{ "refresh",	TC_REFRESH },
	{ "copy",	TC_COPY },
	{ "cut",	TC_CUT },
	{ "delete",	TC_DELETE },
	{ "rescan",	TC_RESCAN },
	{ "research",	TC_SEARCH },
	{ "net",	TC_NET },
	{ "graph",	TC_GRAPH },
	{ "focus",	TC_FOCUS },
};

#define NTOOLCMD	( (INT)( sizeof(tool_cmds) / sizeof(tool_cmds[0]) ) )

/* A tool's menu (TOOLMENU.DEF), set to what its window is */
EXPORT ER dt_tool_menu_make( DTWIN *d, BOOL on_vobj, ID *p_mid )
{
	CONST char	*id;
	TS_UUID		def;
	UINT		none = ( d->npick == 0 ) ? MN_GREY : 0;
	ID		mid;
	ER		er;

	switch ( d->tool ) {
	case DT_TOOL_TRASH:	id = SYSDEF_MENU_TRASH;		break;
	case DT_TOOL_SEARCH:	id = SYSDEF_MENU_SEARCH;	break;
	case DT_TOOL_NETWORK:	id = SYSDEF_MENU_NETWORK;	break;
	default:		return E_PAR;
	}
	er = ts_str_to_uuid(id, &def);
	if ( er >= E_OK ) {
		er = mn_cre_men(&def, &mid);
	}
	if ( er < E_OK ) {
		return er;
	}
	(void)mn_chg_atr(mid, NULL, "fullscreen", d->full ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "copy", none);
	(void)mn_chg_atr(mid, NULL, "cut", none);
	(void)mn_chg_atr(mid, NULL, "delete", none);
	(void)mn_chg_atr(mid, NULL, "net", nw_graph ? 0 : MN_TICK);
	(void)mn_chg_atr(mid, NULL, "graph", nw_graph ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "focus", on_vobj ? 0 : MN_GREY);
	*p_mid = mid;

	return E_OK;
}

EXPORT void dt_tool_menu_do( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			     CONST T_VOBJ *v )
{
	INT	i, cmd = 0;

	for ( i = 0; i < NTOOLCMD && cmd == 0; i++ ) {
		if ( mn_is(sel, tool_cmds[i].code) ) {
			cmd = tool_cmds[i].cmd;
		}
	}
	switch ( cmd ) {
	case TC_FULL:		dt_fullscreen(d);			break;
	case TC_REFRESH:	dt_tool_refresh(d->tool);		break;
	case TC_COPY:
	case TC_CUT:		ed_copy(d);				break;
	case TC_DELETE:		trash_delete(d);			break;
	case TC_RESCAN:		trash_rescan(d);			break;
	case TC_SEARCH:		dt_search_open(d);			break;
	case TC_NET:
	case TC_GRAPH:
		nw_graph = (BOOL)( cmd == TC_GRAPH );
		dt_tool_refresh(DT_TOOL_NETWORK);
		break;
	case TC_FOCUS:
		if ( on_vobj && v != NULL ) {
			nw_show(&v->target);
		}
		break;
	default:
		break;
	}
	wm_composite();
}

/*
 * A key in a tool's window, when it is the tool's: Delete throws away
 * what is taken in the 屑実身 list, Ctrl+X takes it to the clipboard as
 * Ctrl+C does -- nothing leaves the list until it is linked to again.
 */
EXPORT BOOL dt_tool_key( DTWIN *d, UINT code, UINT mods )
{
	BOOL	ctrl = (BOOL)( ( mods & MOD_CTRL ) != 0 );

	if ( d->tool == DT_TOOL_TRASH && !ctrl && code == KEY_DEL ) {
		trash_delete(d);
		wm_composite();
		return TRUE;
	}
	if ( ctrl && code == KEY_X ) {
		ed_copy(d);
		return TRUE;
	}
	if ( d->tool == DT_TOOL_NETWORK && ctrl
	  && ( code == KEY_N || code == KEY_G ) ) {
		nw_graph = (BOOL)( code == KEY_G );
		dt_tool_refresh(DT_TOOL_NETWORK);
		return TRUE;
	}

	return FALSE;
}

/* An object carried out of a tool's page: a link to it where it lands */
EXPORT void dt_tool_carry( DTWIN *d, DTWIN *t, CONST T_VOBJ *v )
{
	ed_drop_link(d, t, v);
}
