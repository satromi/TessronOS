/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	nw_main.c
 *	仮身ネットワーク: what an object links to, and what those link to, a
 *	system's tool running as a process (design 11.2.4)
 *
 *	The desktop starts it on the object a link points at (its start-up
 *	argument). The network is followed step by step outwards through
 *	the links of each object's first record (ob_lst_lnk, with the
 *	person's rights), each object met once, at the first step it is met
 *	at; a link to one already met is still a line, so that a loop
 *	shows as a loop. It is shown as one of two figures the program
 *	makes:
 *
 *	  仮身ネットワーク表示 (Ctrl+N)	a column to each step, the objects
 *				of a step one under another in the order they
 *				were met, and a line bent at right angles
 *				from each to each it links to
 *	  仮身グラフ表示 (Ctrl+G)	each object a circle, larger the more
 *				links in the network reach it, with its link
 *				beside it, and a line for each link; where the
 *				circles go is found by letting them push each
 *				other apart while the links pull their ends
 *				together
 *
 *	この仮身を起点にする starts the network again from the link the menu
 *	was opened on; run again on another link while it is open, the
 *	desktop brings the window to the front and the network starts from
 *	that one. What it does is said on the console for the tests
 *	("network: ...").
 */

#include "st.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/mn.h>

#define NW_W		200		/* each one's box */
#define NW_GAP_X	100		/* between one column and the next */
#define NW_GAP_Y	20		/* between one row and the next */
#define NW_PAD		40
#define NW_CHSZ		14

typedef struct {
	TS_UUID	id;
	INT	depth;
	INT	x, y;
} NWNODE;

/*
 * Every object the network reaches and every link between them: the
 * lists grow as they are met. hash finds an object's place by its UUID
 * (open addressing, nhash a power of two, -1 an empty slot).
 */
typedef struct {
	INT	n, ncap, nedge, ecap;
	NWNODE	*node;
	INT	*from, *to;
	INT	*hash, nhash;
} NWNET;

static void nw_told( CONST T_DTANS *an );

static T_TAHOST	nw_host = {
	.who = "network", .prog = SYSDEF_PROG_VNET, .box = SYSDEF_BOX_FIG,
	.remade = fg_model, .picks_none = fg_pick_none, .told = nw_told,
	.at_x = 80, .at_y = 60, .at_w = 900, .at_h = 600,
};

static TS_UUID	nw_start;
static BOOL	nw_graph = FALSE;	/* shown as a graph, not in columns */
static T_TAD	*g_first = NULL;
static UB	g_title[WM_TITLE_MAX];
static BOOL	g_up = FALSE;

/* ---------------------------------------------------------------- the network */

static UW nw_hash_of( CONST TS_UUID *id )
{
	CONST UB	*b = (CONST UB *)id;
	UW		h = 2166136261U;
	INT		i;

	for ( i = 0; i < (INT)sizeof(TS_UUID); i++ ) h = ( h ^ b[i] ) * 16777619U;
	return h;
}

static INT nw_find( CONST NWNET *g, CONST TS_UUID *id )
{
	UW	m = (UW)g->nhash - 1, k;

	if ( g->nhash == 0 ) return -1;
	for ( k = nw_hash_of(id) & m; g->hash[k] >= 0; k = ( k + 1 ) & m ) {
		if ( memcmp(&g->node[g->hash[k]].id, id, sizeof(TS_UUID)) == 0 ) return g->hash[k];
	}
	return -1;
}

/* The place of a new object, its hash kept less than half full: -1 for no memory */
static INT nw_add( NWNET *g, CONST TS_UUID *id, INT depth )
{
	INT	i;
	UW	m, k;

	if ( g->n == g->ncap ) {
		INT	cap = ( g->ncap > 0 ) ? g->ncap * 2 : 64;
		NWNODE	*t = realloc(g->node, sizeof(NWNODE) * (size_t)cap);

		if ( t == NULL ) return -1;
		g->node = t;
		g->ncap = cap;
	}
	if ( ( g->n + 1 ) * 2 > g->nhash ) {
		INT	nh = ( g->nhash > 0 ) ? g->nhash * 2 : 128;
		INT	*h = malloc(sizeof(INT) * (size_t)nh);

		if ( h == NULL ) return -1;
		for ( i = 0; i < nh; i++ ) h[i] = -1;
		for ( i = 0; i < g->n; i++ ) {
			for ( k = nw_hash_of(&g->node[i].id) & (UW)( nh - 1 ); h[k] >= 0; k = ( k + 1 ) & (UW)( nh - 1 ) ) ;
			h[k] = i;
		}
		free(g->hash);
		g->hash = h;
		g->nhash = nh;
	}
	i = g->n++;
	g->node[i].id = *id;
	g->node[i].depth = depth;
	m = (UW)g->nhash - 1;
	for ( k = nw_hash_of(id) & m; g->hash[k] >= 0; k = ( k + 1 ) & m ) ;
	g->hash[k] = i;
	return i;
}

/* A link from a to b: FALSE for no memory */
static BOOL nw_edge( NWNET *g, INT a, INT b )
{
	if ( g->nedge == g->ecap ) {
		INT	cap = ( g->ecap > 0 ) ? g->ecap * 2 : 128;
		INT	*f = malloc(sizeof(INT) * (size_t)cap * 2);

		if ( f == NULL ) return FALSE;
		if ( g->from != NULL ) {
			memcpy(f, g->from, sizeof(INT) * (size_t)g->nedge);
			memcpy(f + cap, g->to, sizeof(INT) * (size_t)g->nedge);
			free(g->from);
		}
		g->from = f;
		g->to = f + cap;
		g->ecap = cap;
	}
	g->from[g->nedge] = a;
	g->to[g->nedge] = b;
	g->nedge++;
	return TRUE;
}

static void nw_free( NWNET *g )
{
	free(g->node);
	free(g->from);
	free(g->hash);
	memset(g, 0, sizeof(*g));
}

/*
 * The network from one object: step by step outwards, each object met
 * once, at the first step it is met at. A link to an object already met
 * is still a line, back or across, so that a loop shows as a loop.
 */
static void nw_build( NWNET *g, CONST TS_UUID *start )
{
	INT	head = 0, n, j, k, e, ebase;

	memset(g, 0, sizeof(*g));
	if ( nw_add(g, start, 0) < 0 ) return;
	while ( head < g->n ) {
		TS_UUID	*lk = NULL, at = g->node[head].id;

		n = ui_lst_lnk(&at, &lk);	/* the links of its first record, as many as there are */
		ebase = g->nedge;		/* the links of this object start here */
		for ( j = 0; j < n; j++ ) {
			BOOL	dup = FALSE;

			k = nw_find(g, &lk[j]);
			if ( k < 0 ) {
				if ( ob_acc_obj(&lk[j], 0) == E_NOEXS ) continue;	/* not in the store */
				k = nw_add(g, &lk[j], g->node[head].depth + 1);
				if ( k < 0 ) continue;	/* no memory: shown as far as there was */
			}
			for ( e = ebase; e < g->nedge && !dup; e++ ) {
				if ( g->to[e] == k ) dup = TRUE;
			}
			if ( !dup ) (void)nw_edge(g, head, k);
		}
		free(lk);
		head++;
	}
}

static void link_of( char *xml, INT *p_at, INT max, CONST NWNODE *nd, INT l, INT t, INT w, INT band, BOOL first )
{
	char	us[TS_UUID_STRLEN + 1];
	UB	*nm = tl_name_dup(&nd->id);
	INT	at = *p_at;

	ts_uuid_text(&nd->id, us);
	at += snprintf(xml + at, (size_t)( max - at ), "<link id=\"%s_0.xtad\" name=\"", us);
	at = tl_put_x(xml, at, max, ( nm != NULL ) ? nm : (CONST UB *)"");
	free(nm);
	/* the one it starts from stands out */
	at += snprintf(xml + at, (size_t)( max - at ),
		       "\" vobjleft=\"%d\" vobjtop=\"%d\" vobjright=\"%d\" vobjbottom=\"%d\" height=\"%d\" chsz=\"%d\""
		       " frcol=\"#000000\" chcol=\"#000000\" tbcol=\"%s\" bgcol=\"#ffffff\""
		       " pictdisp=\"true\" namedisp=\"true\" framedisp=\"true\" autoopen=\"false\"/>",
		       l, t, l + w, t + band, band, NW_CHSZ, first ? "#e0e0ff" : "#ffffff");
	*p_at = at;
}

/* The window's title: what fits of it (a title is shown, not kept) */
static void title_of( CONST NWNET *g, CONST char *what )
{
	UB	*nm = tl_name_dup(&g->node[0].id);
	INT	at;

	at = st_cat(g_title, 0, sizeof(g_title), what);
	(void)st_cat(g_title, at, sizeof(g_title), ( nm != NULL ) ? (CONST char *)nm : "");
	free(nm);
}

/* The bytes the objects' names take in a record, each letter escaped to six at most; -1 too many */
static D names_room( CONST NWNET *g )
{
	D	total = 0;
	INT	i;

	for ( i = 0; i < g->n; i++ ) {
		UB	*nm = tl_name_dup(&g->node[i].id);

		if ( nm != NULL ) total += 6 * (D)strlen((CONST char *)nm);
		free(nm);
		if ( total > 0x70000000 ) return -1;
	}
	return total;
}

/*
 * The network laid out as a figure: a column to each step, the objects
 * of a step one under another in the order they were met, each a
 * closed link as wide as the column; and a line from the right edge
 * of each object to the left edge of each it links to.
 */
static T_TAD *nw_record( NWNET *g )
{
	INT	*row, i, e, at = 0, max, band = tad_closed_h(NW_CHSZ, TAD_D_DEFAULT);
	char	*xml;
	T_TAD	*doc = NULL;

	/* a column to each step: there are no more steps than objects */
	row = calloc((size_t)g->n, sizeof(INT));
	if ( row == NULL ) return NULL;
	for ( i = 0; i < g->n; i++ ) {
		INT	dp = g->node[i].depth;

		g->node[i].x = NW_PAD + dp * ( NW_W + NW_GAP_X );
		g->node[i].y = NW_PAD + row[dp] * ( band + NW_GAP_Y );
		row[dp]++;
	}
	free(row);
	{
		D	nr = names_room(g);

		if ( nr < 0 || (D)g->n * 480 + nr + (D)g->nedge * 200 > 0x70000000 ) return NULL;
		max = 512 + g->n * 480 + (INT)nr + g->nedge * 200;
	}
	xml = malloc((size_t)max);
	if ( xml == NULL ) return NULL;
	title_of(g, "仮身ネットワーク: ");
	at = snprintf(xml, (size_t)max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"仮身ネットワーク\"><figure>");
	/*
	 * The lines first, so that the objects stand over their ends: each a
	 * polyline bent at right angles, with no arrowhead. To a later
	 * column: across from the middle of the right edge of the one, down
	 * or up halfway between the two columns, and across into the middle
	 * of the left edge of the other -- the links of one object share
	 * their upright, and show as a tree. Back or across (to a column not
	 * further right): down or up from the foot of the one, across halfway
	 * between the two rows, and down or up into the other.
	 */
	for ( e = 0; e < g->nedge; e++ ) {
		CONST NWNODE	*a = &g->node[g->from[e]];
		CONST NWNODE	*b = &g->node[g->to[e]];
		INT		x1, y1, x2, y2, mx, my;

		if ( b->x > a->x ) {
			x1 = a->x + NW_W;  y1 = a->y + band / 2;
			x2 = b->x;  y2 = b->y + band / 2;
			mx = ( x1 + x2 ) / 2;
			at += snprintf(xml + at, (size_t)( max - at ),
				       "<polyline lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"0\" start_arrow=\"0\""
				       " end_arrow=\"0\" points=\"%d,%d %d,%d %d,%d %d,%d\"/>",
				       x1, y1, mx, y1, mx, y2, x2, y2);
		} else {
			x1 = a->x + NW_W / 2;  y1 = a->y + band;
			x2 = b->x + NW_W / 2;  y2 = ( b->y > a->y ) ? b->y : b->y + band;
			my = ( y1 + y2 ) / 2;
			at += snprintf(xml + at, (size_t)( max - at ),
				       "<polyline lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"0\" start_arrow=\"0\""
				       " end_arrow=\"0\" points=\"%d,%d %d,%d %d,%d %d,%d\"/>",
				       x1, y1, x1, my, x2, my, x2, y2);
		}
	}
	for ( i = 0; i < g->n; i++ ) {
		link_of(xml, &at, max, &g->node[i], g->node[i].x, g->node[i].y, NW_W, band, (BOOL)( i == 0 ));
	}
	at += snprintf(xml + at, (size_t)( max - at ), "</figure></tad>");
	if ( tad_parse((CONST UB *)xml, (SZ)at, NULL, &doc) < E_OK ) doc = NULL;
	free(xml);
	return doc;
}

/* ---------------------------------------------------------------- the graph */

/*
 * The same network laid out as a graph: each object a circle, larger
 * the more links in the network reach it, with its link beside it, and
 * a line for each link. Where the circles go is found by letting them
 * push each other apart while the links pull the ends of each together,
 * for a fixed number of steps with the moves made smaller each step.
 * Places are kept in sixteenths of a pixel.
 */
#define GR_STEPS	160
#define GR_SIDE		720		/* the square they are laid in */
#define GR_K		( 110 * 16 )	/* the length a link settles to */

static D isqrt( D v )
{
	D	r = 0, b = (D)1 << 40;

	if ( v <= 0 ) return 0;
	while ( b > v ) b >>= 2;
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

/* The push of k on i, added to i's force (and taken from k's when both) */
static void gr_push( D *px, D *py, D *fx, D *fy, INT i, INT k, BOOL both )
{
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
	if ( both ) {
		fx[k] -= dx * f / d;  fy[k] -= dy * f / d;
	}
}

/*
 * Many objects: each is pushed only by those within twice the length a
 * link settles to, found through a grid of squares that size, so that a
 * step costs about as much as there are objects rather than its square.
 * The squares are kept in a hash of 2^hbits places; a node's square is
 * kept with it, so that two squares in one place are told apart.
 */
#define GR_ALL		256		/* objects pushed each by all the others up to this many */
#define GR_CELL		( 2 * GR_K )

static void gr_push_near( NWNET *g, D *px, D *py, D *fx, D *fy, INT *head, INT *next, INT *cx, INT *cy, INT hbits )
{
	UW	m = ( 1U << hbits ) - 1;
	INT	i, k, a, b;

#define GR_SLOT(x, y)	( ( (UW)(x) * 73856093U ^ (UW)(y) * 19349663U ) & m )
	for ( i = 0; i <= (INT)m; i++ ) head[i] = -1;
	for ( i = 0; i < g->n; i++ ) {
		UW	h;

		cx[i] = (INT)( px[i] / GR_CELL ) - ( ( px[i] < 0 ) ? 1 : 0 );
		cy[i] = (INT)( py[i] / GR_CELL ) - ( ( py[i] < 0 ) ? 1 : 0 );
		h = GR_SLOT(cx[i], cy[i]);
		next[i] = head[h];
		head[h] = i;
	}
	for ( i = 0; i < g->n; i++ ) {
		for ( a = cx[i] - 1; a <= cx[i] + 1; a++ ) {
			for ( b = cy[i] - 1; b <= cy[i] + 1; b++ ) {
				for ( k = head[GR_SLOT(a, b)]; k >= 0; k = next[k] ) {
					if ( k != i && cx[k] == a && cy[k] == b ) gr_push(px, py, fx, fy, i, k, FALSE);
				}
			}
		}
	}
#undef GR_SLOT
}

static void gr_layout( NWNET *g, D *px, D *py )
{
	static CONST INT	cs[16][2] = {
		{ 1000, 0 }, { 924, 383 }, { 707, 707 }, { 383, 924 },
		{ 0, 1000 }, { -383, 924 }, { -707, 707 }, { -924, 383 },
		{ -1000, 0 }, { -924, -383 }, { -707, -707 }, { -383, -924 },
		{ 0, -1000 }, { 383, -924 }, { 707, -707 }, { 924, -383 }
	};
	D	*fx, *fy;
	INT	i, k, e, step, *head = NULL, *next = NULL, *cx = NULL, *cy = NULL, hbits = 0;

	fx = malloc(sizeof(D) * (size_t)g->n * 2);
	if ( fx == NULL ) return;
	fy = fx + g->n;
	if ( g->n > GR_ALL ) {
		while ( ( 1 << hbits ) < g->n * 2 ) hbits++;
		head = malloc(sizeof(INT) * ( ( (size_t)1 << hbits ) + (size_t)g->n * 3 ));
		if ( head == NULL ) {
			free(fx);
			return;
		}
		next = head + ( 1 << hbits );
		cx = next + g->n;
		cy = cx + g->n;
	}
	/*
	 * Round a circle to begin with, sixteen to a ring and each ring
	 * further out, the one started from in the middle. The directions
	 * are a table: cosine and sine by thousandths.
	 */
	for ( i = 0; i < g->n; i++ ) {
		D	r = ( i == 0 ) ? 0 : (D)( 120 + ( ( i - 1 ) / 16 ) * 60 ) * 16;

		px[i] = ( GR_SIDE / 2 ) * 16 + r * cs[( i - 1 ) & 15][0] / 1000;
		py[i] = ( GR_SIDE / 2 ) * 16 + r * cs[( i - 1 ) & 15][1] / 1000;
	}
	for ( step = 0; step < GR_STEPS; step++ ) {
		D	limit = ( (D)( GR_STEPS - step ) * 40 * 16 ) / GR_STEPS + 16;

		for ( i = 0; i < g->n; i++ ) fx[i] = fy[i] = 0;
		/* everything pushes everything else away; of many, those near */
		if ( head != NULL ) {
			gr_push_near(g, px, py, fx, fy, head, next, cx, cy, hbits);
		} else {
			for ( i = 0; i < g->n; i++ ) {
				for ( k = i + 1; k < g->n; k++ ) gr_push(px, py, fx, fy, i, k, TRUE);
			}
		}
		/* and each link pulls its ends together */
		for ( e = 0; e < g->nedge; e++ ) {
			INT	a = g->from[e], b = g->to[e];
			D	dx = px[a] - px[b], dy = py[a] - py[b];
			D	d = isqrt(dx * dx + dy * dy), f;

			if ( a == b || d == 0 ) continue;
			f = d * d / GR_K;			/* d^2 / k */
			fx[a] -= dx * f / d;  fy[a] -= dy * f / d;
			fx[b] += dx * f / d;  fy[b] += dy * f / d;
		}
		/* each moves along its force, no further than the step allows */
		for ( i = 0; i < g->n; i++ ) {
			D	f = isqrt(fx[i] * fx[i] + fy[i] * fy[i]);

			if ( f == 0 ) continue;
			if ( f > limit ) {
				fx[i] = fx[i] * limit / f;
				fy[i] = fy[i] * limit / f;
			}
			px[i] += fx[i];
			py[i] += fy[i];
		}
	}
	free(fx);
	free(head);
}

static T_TAD *gr_record( NWNET *g )
{
	INT	i, e, at = 0, max, band = tad_closed_h(NW_CHSZ, TAD_D_DEFAULT), *in;
	D	*px, *py, minx, miny;
	char	*xml;
	T_TAD	*doc = NULL;

	px = malloc(sizeof(D) * (size_t)g->n * 2);
	in = malloc(sizeof(INT) * (size_t)g->n);
	if ( px == NULL || in == NULL ) {
		free(px);
		free(in);
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
	for ( e = 0; e < g->nedge; e++ ) in[g->to[e]]++;
	for ( i = 0; i < g->n; i++ ) {
		g->node[i].x = (INT)( ( px[i] - minx ) / 16 ) + NW_PAD + 30;
		g->node[i].y = (INT)( ( py[i] - miny ) / 16 ) + NW_PAD + 30;
	}
	free(px);

	{
		D	nr = names_room(g);

		if ( nr < 0 || (D)g->n * 700 + nr + (D)g->nedge * 160 > 0x70000000 ) {
			free(in);
			return NULL;
		}
		max = 512 + g->n * 700 + (INT)nr + g->nedge * 160;
	}
	xml = malloc((size_t)max);
	if ( xml == NULL ) {
		free(in);
		return NULL;
	}
	title_of(g, "仮身グラフ: ");
	at = snprintf(xml, (size_t)max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"仮身グラフ\"><figure>");
	for ( e = 0; e < g->nedge; e++ ) {
		CONST NWNODE	*a = &g->node[g->from[e]];
		CONST NWNODE	*b = &g->node[g->to[e]];

		if ( a == b ) continue;
		at += snprintf(xml + at, (size_t)( max - at ),
			       "<line lineType=\"0\" lineWidth=\"1\" l_pat=\"47\" f_pat=\"0\" start_arrow=\"0\""
			       " end_arrow=\"0\" points=\"%d,%d %d,%d\"/>", a->x, a->y, b->x, b->y);
	}
	for ( i = 0; i < g->n; i++ ) {
		INT	r = 6 + 3 * in[i];

		if ( r > 30 ) r = 30;
		/* the circle: the one started from in its own colour */
		at += snprintf(xml + at, (size_t)( max - at ),
			       "<ellipse lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"%s\" cx=\"%d\" cy=\"%d\" rx=\"%d\" ry=\"%d\"/>",
			       ( i == 0 ) ? "36" : "40", g->node[i].x, g->node[i].y, r, r);
		/* and beside it, the object itself */
		link_of(xml, &at, max, &g->node[i], g->node[i].x + r + 4, g->node[i].y - band / 2, 150, band, FALSE);
	}
	at += snprintf(xml + at, (size_t)( max - at ), "</figure></tad>");
	if ( tad_parse((CONST UB *)xml, (SZ)at, NULL, &doc) < E_OK ) doc = NULL;
	free(xml);
	free(in);
	return doc;
}

/* The network from one object, as columns or as a graph */
static T_TAD *network( CONST TS_UUID *start )
{
	NWNET	g;
	T_TAD	*rec;
	char	s[64];

	nw_start = *start;
	nw_build(&g, start);
	rec = ( g.n == 0 ) ? NULL : nw_graph ? gr_record(&g) : nw_record(&g);
	snprintf(s, sizeof(s), "%d objects %d links as %s", g.n, g.nedge, nw_graph ? "a graph" : "columns");
	note(s);
	nw_free(&g);
	return rec;
}

/* Shown again in the window, from start */
static void show( CONST TS_UUID *start )
{
	T_TAD	*rec = network(start);

	if ( rec == NULL ) return;
	fg_list_set(rec);
	ta_title((CONST char *)g_title);
}

/* ---------------------------------------------------------------- what the cabinet asks of it */

/* The window stands for the program object; the network starts from the object it was started on */
static ER nw_start_on( T_OBREF *p_ref )
{
	T_PRCOPEN	a;
	ER		er;

	memset(&a, 0, sizeof(a));
	if ( ts_get_arg(&a, sizeof(a)) < (SZ)sizeof(TS_UUID) ) {
		note("no object to start from");
		return E_PAR;
	}
	er = st_start(&nw_host, SYSDEF_PROG_VNET, p_ref);
	if ( er < E_OK ) return er;
	g_first = network(&a.target);
	return ( g_first != NULL ) ? E_OK : E_NOEXS;
}

static void nw_begin( void )
{
	fg_as = FG_AS_TOOL;
}

static ER nw_load( T_TAD **p_rec )
{
	if ( g_first == NULL ) return E_NOMEM;
	*p_rec = g_first;
	g_first = NULL;
	return E_OK;
}

static void nw_listen( void )
{
	ta_listen("virtual-object-network");
}

static void nw_shown( void )
{
	if ( !g_up ) {
		g_up = TRUE;
		ta_title((CONST char *)g_title);
		note("up");
	}
}

/* Run again on another link while open: the network from that one */
static void nw_told( CONST T_DTANS *an )
{
	if ( an->how != DT_HOW_TOOL_AGAIN ) return;
	note("run again");
	show(&an->into);
}

/* 仮身ネットワーク表示, 仮身グラフ表示 ticked as the window shows it; この仮身を起点にする on a link alone */
static void nw_menu( ID mid, CONST T_VOBJ *v )
{
	(void)mn_chg_atr(mid, NULL, "net", nw_graph ? 0 : MN_TICK);
	(void)mn_chg_atr(mid, NULL, "graph", nw_graph ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "focus", ( v != NULL ) ? 0 : MN_GREY);
}

static BOOL nw_command( CONST char *code, CONST T_VOBJ *v )
{
	if ( strcmp(code, "refresh") == 0 ) show(&nw_start);
	else if ( strcmp(code, "net") == 0 || strcmp(code, "graph") == 0 ) {
		nw_graph = (BOOL)( strcmp(code, "graph") == 0 );
		show(&nw_start);
	} else if ( strcmp(code, "focus") == 0 ) {
		if ( v != NULL ) show(&v->target);
	} else if ( strcmp(code, "cut") == 0 ) (void)fg_copy();	/* Ctrl+X: a copy, nothing leaves the figure */
	else return FALSE;
	return TRUE;
}

static CONST char *nw_key( INT ch )
{
	if ( ch == 'n' || ch == 'N' ) return "net";
	if ( ch == 'g' || ch == 'G' ) return "graph";
	return NULL;
}

static CONST T_FGBOX	nw_box = {
	.name = "仮身ネットワーク", .start = nw_start_on, .begin = nw_begin, .load = nw_load, .listen = nw_listen,
	.shown = nw_shown, .menu = nw_menu, .command = nw_command, .menu_def = SYSDEF_MENU_NETWORK, .key = nw_key,
};

CONST T_FGBOX	*fg_box = &nw_box;
