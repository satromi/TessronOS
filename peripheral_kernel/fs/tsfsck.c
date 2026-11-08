/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsck.c
 *	The check and repair of a native volume (ts/tsfsck.h,
 *	docs/tessronos/07-filesystem.md 7.5)
 *
 *	One look at the volume is a pass: the block layer's own blocks, the
 *	three trees walked from their roots, every object the index or the
 *	orphan tree names, its management parts, its records' places, then
 *	what can only be judged once everything is known -- the counts, the
 *	garbage list, the orphan tree, the maps against the blocks in use,
 *	and which objects a live object reaches. Every block a thing uses is
 *	marked in a map of the whole volume as it is met, so a block met
 *	twice is found where it is met the second time.
 *
 *	A repair is passes and steps in turn: the first pass tells what it
 *	finds; then each step puts right one kind of thing, and a pass that
 *	tells nothing looks again before the next, so that each step works
 *	from the volume as the steps before left it; a last pass tells what
 *	is left. Each step is made of the store's own transactions, so a cut
 *	in the power between them leaves a volume the next check takes up
 *	where this one stopped. The repair is not begun at all when the
 *	first pass found a thing it does not put right: what is missing from
 *	the picture -- an object that could not be read, a block two things
 *	use -- could make a block in use look free, or a count too low. A
 *	tree that cannot be walked is not such a thing: the objects are then
 *	found by reading the blocks the maps say are taken, and the tree is
 *	made again from them; nor is a map that does not match its head,
 *	which is made again from the blocks found in use. The two at once
 *	are, since each is found from the other.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/tsfsjrnl.h>
#include <ts/tsfsobj.h>
#include <ts/tsfsck.h>
#include <ts/json.h>
#include <ts/uuid.h>
#include <ts/vobjsz.h>
#include "tstdlib.h"
#include "tsfsfmt.h"

#define CK_DEPTH	32		/* levels a tree may have */
#define CK_TREES	3
#define CK_TRASH_MAX	TSFSO_MAX_LINK	/* objects one TRASH gathers */
#define CK_TRASH_NAME	"TRASH"
#define HD_OWNER	32		/* the owner in the common head */
#define KNOWN_FLAGS	( TSFSO_F_EDITABLE | TSFSO_F_DELETABLE | TSFSO_F_READABLE \
			| TSFSO_F_GARBAGE | TSFSO_F_ORPHAN | TSFSO_F_RELINK | TSFSO_F_TRASH )
#define BITS_PER_MAP	( TSFSBLK_BLOCK_SIZE * 8 )

/* ---------------------------------------------------------------- what a pass keeps */

typedef struct {
	TS_UUID	u;
	UD	v;
} CKITEM;

typedef struct {
	CKITEM	*it;
	INT	n, max;
	BOOL	ok;			/* walked whole */
	UD	leaf_next;		/* the next leaf the last leaf named */
	UD	last_leaf;
} CKTREE;

typedef struct {
	TS_UUID	u;
	UD	blk;
	UW	flags;
	INT	refcnt;
	INT	pins;
	INT	want;			/* what the link tables and the holds make the count */
	INT	lk0, nlk;		/* its counted links in CK.lk */
	UD	inl;			/* units of its inline area its parts use */
	UD	gen;			/* the transaction that last wrote its block, when found by reading */
	BOOL	orphan;			/* the orphan tree holds it, the index does not */
	BOOL	read;			/* its object block passed */
	BOOL	ok;			/* every part of it was read */
	BOOL	on_gc, on_orphan;
	UD	gc_v;			/* the block the garbage list names */
	BOOL	badlink;
	BOOL	reached;
	BOOL	hidden;
	BOOL	damaged;		/* its block reads but does not pass its check: what reads of it is saved */
} CKOBJ;

typedef struct {
	TS_UUID	target;
	INT	from;
} CKLNK;

typedef struct {
	ID		vol;
	UINT		mode;
	FP_TSFSCK	fn;
	void		*ctx;
	BOOL		tell;		/* what is found goes to fn */
	BOOL		verify;		/* the pass after a repair */
	INT		phase;
	INT		*count;		/* where what is found is counted */
	T_TSFSCK	*res;
	T_TSFSBLK_SB	sb;
	T_TSFSBLKINSP	insp;
	T_TSFSAGREF	*ag;
	UD		ag_first;
	UB		*claim;		/* a bit for each block something uses */
	BOOL		broken;		/* something could not be read whole */
	BOOL		legacy;		/* no object has a link table or a hold */
	BOOL		tree_bad[CK_TREES];	/* a tree that could not be walked: made again from the objects */
	BOOL		scanned;	/* the objects were found by reading the blocks in use */
	BOOL		map_bad;	/* a map block does not match its head: made again from the blocks in use */
	CKTREE		tree[CK_TREES];
	CKOBJ		*obj;
	INT		nobj, maxobj;
	CKLNK		*lk;
	INT		nlk, maxlk;
	INT		unfixable;
	UD		records, links, checked;
	INT		ndamaged;	/* objects whose block does not pass its check */
	BOOL		probing;	/* sifting old copies of objects out (sift) */
	BOOL		restale;	/* one more was found to be an old copy */
	INT		*owner;		/* while sifting: which object took each block, -1 the volume's own */
	UD		*stale;		/* blocks of object blocks found to be old copies */
	INT		nstale, maxstale;
	BOOL		xt2;		/* the volume says it has trees of two levels */
} CK;

/* ---------------------------------------------------------------- bytes */

LOCAL UH rd16( CONST UB *p )
{
	return (UH)((UW)p[0] | ((UW)p[1] << 8));
}

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL UD rd64( CONST UB *p )
{
	return (UD)rd32(p) | ((UD)rd32(p + 4) << 32);
}

LOCAL INT cmp_n( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return ( a[i] < b[i] ) ? -1 : 1;
	}
	return 0;
}

LOCAL BOOL is_nil( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( u->b[i] != 0 ) return FALSE;
	}
	return TRUE;
}

#define BLKS_FOR(len, per)	( ( (UD)(len) + (per) - 1 ) / (per) )

/* A table that grows by half again each time it is full */
LOCAL BOOL grow( void **p, INT *max, INT need, SZ esz )
{
	void	*q;
	INT	m;

	if ( need <= *max ) {
		return TRUE;
	}
	m = ( *max < 64 ) ? 64 : *max + *max / 2;
	if ( m < need ) m = need;
	q = Kmalloc(esz * (SZ)m);
	if ( q == NULL ) {
		return FALSE;
	}
	if ( *p != NULL ) {
		knl_memcpy(q, *p, (INT)( esz * (SZ)*max ));
		Kfree(*p);
	}
	*p = q;
	*max = m;
	return TRUE;
}

/* ---------------------------------------------------------------- telling */

/* Whether the repair leaves a thing of this kind as it is */
LOCAL BOOL beyond_repair( CK *c, INT kind, D b )
{
	switch ( kind ) {
	case TSFSCK_K_HEAD:
		/* an object block that reads but does not pass its check: what reads of it is saved */
		return (BOOL)( b != TSFSCK_W_DAMAGED );
	case TSFSCK_K_INDEX: case TSFSCK_K_PART:
	case TSFSCK_K_TABLE: case TSFSCK_K_DATA: case TSFSCK_K_META: case TSFSCK_K_DOUBLE:
	case TSFSCK_K_UNREAD:
		return TRUE;
	case TSFSCK_K_NODE:
	case TSFSCK_K_GROUP:
		/* a tree is made again from the objects, found by reading every block under a bad map
		   when it has to be; a map from the blocks in use */
		return FALSE;
	case TSFSCK_K_SUPER:
		return (BOOL)( b == 2 );		/* a root that is not on the volume */
	case TSFSCK_K_ORPHAN:
		/* one in the index marked as deleted while open; one deleted
		   while open that is unmarked is finished only on a volume
		   nothing else has open */
		return (BOOL)( b == 2 || ( b == 3 && ( c->mode & TSFSCK_ALONE ) == 0 ) );
	default:
		return FALSE;
	}
}

LOCAL void send( CK *c, INT kind, CONST TS_UUID *u, CONST TS_UUID *other, UD blk, D a, D b,
		 BOOL fixed )
{
	T_TSFSCKEV	e;

	if ( c->fn == NULL ) {
		return;
	}
	knl_memset(&e, 0, sizeof(e));
	e.phase = c->phase;
	e.kind = kind;
	e.fixed = fixed;
	if ( u != NULL ) {
		e.has_uuid = TRUE;
		e.uuid = *u;
	}
	if ( other != NULL ) {
		e.has_other = TRUE;
		e.other = *other;
	}
	e.blk = blk;
	e.a = a;
	e.b = b;
	(*c->fn)(c->ctx, &e);
}

/* A thing found: counted by `weight` (blocks, or 1), and told when the pass tells */
LOCAL void found( CK *c, INT kind, CONST TS_UUID *u, CONST TS_UUID *other, UD blk, D a, D b,
		  INT weight )
{
	if ( c->count != NULL ) {
		c->count[kind] += weight;
	}
	if ( beyond_repair(c, kind, b) ) {
		c->unfixable += weight;
	}
	if ( c->tell ) {
		send(c, kind, u, other, blk, a, b, FALSE);
	}
}

LOCAL void phase( CK *c, INT ph )
{
	c->phase = ph;
	if ( c->tell ) {
		send(c, TSFSCK_K_PHASE, NULL, NULL, 0, 0, 0, FALSE);
	}
}

/* A thing put right: always told */
LOCAL void mended( CK *c, INT kind, CONST TS_UUID *u, UD blk, D a, D b, INT weight )
{
	INT	ph = c->phase;

	c->res->fixed[kind] += weight;
	c->res->written = TRUE;
	c->phase = TSFSCK_PH_REPAIR;
	send(c, kind, u, NULL, blk, a, b, TRUE);
	c->phase = ph;
}

/* ---------------------------------------------------------------- blocks in use */

/* The group a block is in, or -1 */
LOCAL INT group_of( CK *c, UD blk )
{
	UD	n;

	if ( blk < c->ag_first || c->sb.ag_blocks == 0 ) {
		return -1;
	}
	n = ( blk - c->ag_first ) / c->sb.ag_blocks;
	if ( n >= c->sb.ag_count || blk >= c->ag[n].first + c->ag[n].len ) {
		return -1;
	}
	return (INT)n;
}

LOCAL BOOL used_bit( CONST UB *m, UD blk )
{
	return (BOOL)( ( m[blk >> 3] & ( 1 << ( blk & 7 ) ) ) != 0 );
}

LOCAL CKOBJ *obj_find( CK *c, CONST TS_UUID *u );
LOCAL void stale_pick( CK *c, UD blk, CONST TS_UUID *u );

/*
 * A block something points at: it has to be a block objects may use,
 * and nothing else may have pointed at it before. `kind` is what is
 * told when it is outside them.
 */
LOCAL BOOL claim( CK *c, UD blk, INT kind, CONST TS_UUID *u )
{
	INT	n = ( blk < c->sb.total_blocks ) ? group_of(c, blk) : -1;

	if ( n < 0 || blk < c->ag[n].first + c->ag[n].meta ) {
		found(c, kind, u, NULL, blk, 0, TSFSCK_W_OUTSIDE, 1);
		c->broken = TRUE;
		return FALSE;
	}
	if ( used_bit(c->claim, blk) ) {
		if ( c->probing && u != NULL && c->owner != NULL ) {
			stale_pick(c, blk, u);	/* an old copy of an object given up since */
			return FALSE;
		}
		found(c, TSFSCK_K_DOUBLE, u, NULL, blk, 0, 0, 1);
		c->broken = TRUE;
		return FALSE;
	}
	c->claim[blk >> 3] |= (UB)( 1 << ( blk & 7 ) );
	if ( c->owner != NULL ) {
		CKOBJ	*o = ( u != NULL ) ? obj_find(c, u) : NULL;

		c->owner[blk] = ( o != NULL ) ? (INT)( o - c->obj ) + 1 : -1;
	}
	return TRUE;
}

/* A management block read as the volume holds it now, and its head looked at */
LOCAL BOOL mread( CK *c, UD blk, UB *buf, CONST char *magic )
{
	if ( blk >= c->sb.total_blocks ) {
		return FALSE;
	}
	c->checked++;
	if ( ts_jrnl_read(c->vol, blk, buf) < E_OK ) {
		return FALSE;
	}
	return (BOOL)( ts_blk_good(c->vol, buf, magic, blk) >= E_OK );
}

/* ---------------------------------------------------------------- trees */

LOCAL BOOL tree_add( CKTREE *t, CONST UB *key, UD v )
{
	if ( !grow((void **)&t->it, &t->max, t->n + 1, sizeof(CKITEM)) ) {
		return FALSE;
	}
	knl_memcpy(t->it[t->n].u.b, key, 16);
	t->it[t->n].v = v;
	t->n++;
	return TRUE;
}

/*
 * A node and what hangs from it: its keys inside [lo, hi), in order,
 * every child at the level below it. Answers its height above the
 * leaves, or -1 when it could not be used.
 */
LOCAL INT visit( CK *c, INT tree, UD blk, CONST UB *lo, CONST UB *hi, INT depth )
{
	CKTREE	*t = &c->tree[tree];
	UB	*buf;
	INT	i, n, h = -1, k, level;
	UD	child;
	BOOL	ok = TRUE;

	if ( depth > CK_DEPTH ) {
		found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_ORDER, 1);
		return -1;
	}
	if ( !claim(c, blk, TSFSCK_K_NODE, NULL) ) {
		return -1;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return -1;
	}
	if ( !mread(c, blk, buf, TSFSBLK_MAGIC_NODE) ) {
		found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_DAMAGED, 1);
		Kfree(buf);
		return -1;
	}
	n = (INT)rd16(buf + OFF_NKEYS);
	level = buf[OFF_LEVEL];
	if ( rd64(buf + HD_OWNER) != (UD)tree || rd64(buf + OFF_TREE) != (UD)tree ) {
		found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_WRONG, 1);
		ok = FALSE;
	}
	if ( n > (INT)TSFSBT_MAX_KEYS || ( buf[OFF_TYPE] != TSFSBT_LEAF && buf[OFF_TYPE] != TSFSBT_NODE ) ) {
		found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_FORM, 1);
		Kfree(buf);
		return -1;
	}
	for ( i = 0; i < n && ok; i++ ) {
		CONST UB	*key = ENT_KEY(buf, i);

		if ( ( i > 0 && cmp_n(ENT_KEY(buf, i - 1), key, 16) >= 0 )
		  || ( lo != NULL && cmp_n(key, lo, 16) < 0 ) || ( hi != NULL && cmp_n(key, hi, 16) >= 0 ) ) {
			found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_ORDER, 1);
			ok = FALSE;
		}
	}
	if ( !ok ) {
		Kfree(buf);
		return -1;
	}
	if ( buf[OFF_TYPE] == TSFSBT_LEAF ) {
		/* the leaves in order, each naming the one after it */
		if ( t->last_leaf != 0 && t->leaf_next != blk ) {
			found(c, TSFSCK_K_NODE, NULL, NULL, t->last_leaf, tree, TSFSCK_W_ORDER, 1);
			ok = FALSE;
		}
		t->last_leaf = blk;
		t->leaf_next = rd64(buf + OFF_NEXT);
		for ( i = 0; i < n; i++ ) {
			if ( !tree_add(t, ENT_KEY(buf, i), rd64(ENT_VAL(buf, i))) ) {
				ok = FALSE;
			}
		}
		Kfree(buf);
		return ok ? 0 : -1;
	}
	/* the children, each between the separators either side of it */
	for ( k = -1; k < n; k++ ) {
		INT	ch;

		child = ( k < 0 ) ? rd64(buf + OFF_CHILD0) : rd64(ENT_VAL(buf, k));
		if ( child == 0 ) {
			found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_FORM, 1);
			ok = FALSE;
			continue;
		}
		ch = visit(c, tree, child, ( k < 0 ) ? lo : ENT_KEY(buf, k), ( k + 1 < n ) ? ENT_KEY(buf, k + 1) : hi,
			   depth + 1);
		if ( ch < 0 ) {
			ok = FALSE;
		} else if ( h < 0 || ch + 1 > h ) {
			h = ch + 1;
		}
	}
	/* a level below the height is what an older kernel gave a new root */
	if ( ok && h > 0 && ( level > h || level < 1 ) ) {
		found(c, TSFSCK_K_NODE, NULL, NULL, blk, tree, TSFSCK_W_WRONG, 1);
		ok = FALSE;
	}
	Kfree(buf);
	return ok ? h : -1;
}

LOCAL void walk_tree( CK *c, INT tree )
{
	CKTREE	*t = &c->tree[tree];
	UD	root = 0, nodes = 0;

	t->n = 0;
	t->ok = TRUE;
	t->last_leaf = 0;
	t->leaf_next = 0;
	switch ( tree ) {
	case TSFSBT_OBJ:	(void)ts_get_root_blk(c->vol, &root, &nodes);		break;
	case TSFSBT_GC:		(void)ts_get_gc_root_blk(c->vol, &root, &nodes);	break;
	default:		(void)ts_get_orphan_root_blk(c->vol, &root, &nodes);	break;
	}
	if ( root == 0 ) {
		return;
	}
	if ( visit(c, tree, root, NULL, NULL, 0) < 0 ) {
		t->ok = FALSE;
	}
	if ( t->ok && t->leaf_next != 0 ) {
		found(c, TSFSCK_K_NODE, NULL, NULL, t->last_leaf, tree, TSFSCK_W_ORDER, 1);
		t->ok = FALSE;
	}
	if ( !t->ok ) {
		c->tree_bad[tree] = TRUE;
		t->n = 0;		/* what was walked of it is not the tree: it is made again */
	}
}

/* An item of a tree by its key, or NULL */
LOCAL CKITEM *tree_find( CKTREE *t, CONST TS_UUID *u )
{
	INT	lo = 0, hi = t->n - 1, m, d;

	while ( lo <= hi ) {
		m = ( lo + hi ) / 2;
		d = cmp_n(t->it[m].u.b, u->b, 16);
		if ( d == 0 ) return &t->it[m];
		if ( d < 0 ) lo = m + 1; else hi = m - 1;
	}
	return NULL;
}

/* ---------------------------------------------------------------- objects */

LOCAL CKOBJ *obj_find( CK *c, CONST TS_UUID *u )
{
	INT	lo = 0, hi = c->nobj - 1, m, d;

	while ( lo <= hi ) {
		m = ( lo + hi ) / 2;
		d = cmp_n(c->obj[m].u.b, u->b, 16);
		if ( d == 0 ) return &c->obj[m];
		if ( d < 0 ) lo = m + 1; else hi = m - 1;
	}
	return NULL;
}

LOCAL BOOL obj_add( CK *c, CONST CKITEM *it, BOOL orphan )
{
	CKOBJ	*o;

	if ( !grow((void **)&c->obj, &c->maxobj, c->nobj + 1, sizeof(CKOBJ)) ) {
		return FALSE;
	}
	o = &c->obj[c->nobj++];
	knl_memset(o, 0, sizeof(*o));
	o->u = it->u;
	o->blk = it->v;
	o->orphan = orphan;
	return TRUE;
}

/* The objects: the index's and the orphan tree's that the index does not have, in the order of their keys */
LOCAL void list_objects( CK *c )
{
	CKTREE	*ix = &c->tree[TSFSBT_OBJ], *or = &c->tree[TSFSBT_ORPHAN];
	INT	i = 0, k = 0, d;

	c->nobj = 0;
	while ( i < ix->n || k < or->n ) {
		d = ( i >= ix->n ) ? 1 : ( k >= or->n ) ? -1 : cmp_n(ix->it[i].u.b, or->it[k].u.b, 16);
		if ( d <= 0 ) {
			if ( !obj_add(c, &ix->it[i], FALSE) ) break;
			i++;
			if ( d == 0 ) k++;
		} else {
			if ( !obj_add(c, &or->it[k], TRUE) ) break;
			k++;
		}
	}
}

/* ファイルヘッダー: each object block, where the index says it is */
LOCAL void check_heads( CK *c, UB *buf )
{
	INT	i;

	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];
		CKITEM	*g, *r;

		g = tree_find(&c->tree[TSFSBT_GC], &o->u);
		r = tree_find(&c->tree[TSFSBT_ORPHAN], &o->u);
		o->on_gc = (BOOL)( g != NULL );
		o->gc_v = ( g != NULL ) ? g->v : 0;
		o->on_orphan = (BOOL)( r != NULL );
		if ( o->blk == 0 || o->blk >= c->sb.total_blocks ) {
			found(c, TSFSCK_K_INDEX, &o->u, NULL, o->blk, o->orphan ? 1 : 0, TSFSCK_W_OUTSIDE, 1);
			c->broken = TRUE;
			continue;
		}
		if ( !claim(c, o->blk, TSFSCK_K_INDEX, &o->u) ) {
			continue;
		}
		c->checked++;
		if ( ts_jrnl_read(c->vol, o->blk, buf) < E_OK ) {
			found(c, TSFSCK_K_UNREAD, &o->u, NULL, o->blk, 0, 0, 1);
			c->broken = TRUE;
			continue;
		}
		if ( ts_blk_good(c->vol, buf, TSFSBLK_MAGIC_OBJ, o->blk) < E_OK ) {
			/* it reads, but does not pass its check: what can be read of it is saved by the repair */
			found(c, TSFSCK_K_HEAD, &o->u, NULL, o->blk, 0, TSFSCK_W_DAMAGED, 1);
			o->damaged = TRUE;
			c->ndamaged++;
			continue;
		}
		if ( cmp_n(buf + OB_UUID, o->u.b, 16) != 0 ) {
			found(c, TSFSCK_K_HEAD, &o->u, NULL, o->blk, 0, TSFSCK_W_WRONG, 1);
			c->broken = TRUE;
			continue;
		}
		o->flags = rd32(buf + OB_FLAGS);
		o->refcnt = (INT)rd32(buf + OB_REFCNT);
		o->pins = (INT)rd32(buf + OB_PINS);
		if ( rd32(buf + OB_NPLACE) > TSFSO_MAX_REC || rd32(buf + OB_NRES) > TSFSO_MAX_RES
		  || rd32(buf + OB_NLINK) > TSFSO_MAX_LINK || ( rd64(buf + OB_INLMAP) >> INL_UNITS ) != 0
		  || ( o->flags & ~(UW)KNOWN_FLAGS ) != 0 ) {
			found(c, TSFSCK_K_HEAD, &o->u, NULL, o->blk, 0, TSFSCK_W_FORM, 1);
			c->broken = TRUE;
			continue;
		}
		o->read = TRUE;
		if ( o->pins > 0 ) {
			c->legacy = FALSE;
		}
		if ( c->scanned ) {
			o->orphan = (BOOL)( ( o->flags & TSFSO_F_ORPHAN ) != 0 );
		}
		if ( c->tree_bad[TSFSBT_GC] ) {
			o->on_gc = (BOOL)( ( o->flags & TSFSO_F_GARBAGE ) != 0 && ( o->flags & TSFSO_F_ORPHAN ) == 0 );
			o->gc_v = o->blk;
		}
		if ( c->tree_bad[TSFSBT_ORPHAN] ) {
			o->on_orphan = (BOOL)( ( o->flags & ( TSFSO_F_ORPHAN | TSFSO_F_RELINK ) ) != 0 );
		}
	}
}

/* Units of the inline area a place takes, checked against those taken before */
LOCAL BOOL inline_take( CK *c, CKOBJ *o, CONST UB *pl, INT kind )
{
	UW	off = rd16(pl + PL_A), units = rd16(pl + PL_A + 2), u, k;
	UD	want;

	if ( off < OB_INLINE || ( off - OB_INLINE ) % TSFSO_INLINE_UNIT != 0 ) {
		found(c, kind, &o->u, NULL, o->blk, 0, TSFSCK_W_OUTSIDE, 1);
		return FALSE;
	}
	u = ( off - OB_INLINE ) / TSFSO_INLINE_UNIT;
	if ( units == 0 || u + units > INL_UNITS || rd64(pl + PL_BYTES) > (UD)units * TSFSO_INLINE_UNIT ) {
		found(c, kind, &o->u, NULL, o->blk, 0, TSFSCK_W_SIZE, 1);
		return FALSE;
	}
	want = 0;
	for ( k = u; k < u + units; k++ ) {
		want |= 1ULL << k;
	}
	if ( ( o->inl & want ) != 0 ) {
		found(c, kind, &o->u, NULL, o->blk, 0, TSFSCK_W_SHARED, 1);
		return FALSE;
	}
	o->inl |= want;
	return TRUE;
}

/*
 * A management part's bytes, the caller's to Kfree (one byte at least),
 * or NULL when they cannot be had. A first look (`first`) claims its
 * blocks and its units and tells what is wrong; a later one only reads.
 */
LOCAL UB *part_read( CK *c, CKOBJ *o, CONST UB *hdr, INT off, BOOL first, SZ *p_len )
{
	CONST UB	*pl = hdr + off;
	UD		len = rd64(pl + PL_BYTES), start, cnt, i;
	UB		*out, *blk;
	SZ		done = 0, n;

	*p_len = 0;
	switch ( pl[PL_FORM] ) {
	case F_NONE:
		if ( len != 0 ) {
			if ( first ) found(c, TSFSCK_K_PART, &o->u, NULL, o->blk, off, TSFSCK_W_SIZE, 1);
			return NULL;
		}
		return (UB *)Kmalloc(1);
	case F_INLINE:
		if ( first && !inline_take(c, o, pl, TSFSCK_K_PART) ) {
			return NULL;
		}
		out = (UB *)Kmalloc((SZ)len + 1);
		if ( out != NULL ) {
			knl_memcpy(out, hdr + rd16(pl + PL_A), (INT)len);
			*p_len = (SZ)len;
		}
		return out;
	case F_RUN:
		break;
	default:
		if ( first ) found(c, TSFSCK_K_PART, &o->u, NULL, o->blk, off, TSFSCK_W_FORM, 1);
		return NULL;
	}
	start = rd64(pl + PL_A);
	cnt = rd64(pl + PL_B);
	if ( cnt != BLKS_FOR(len, MB_PAY) || len > 16 * 1024 * 1024 ) {
		if ( first ) found(c, TSFSCK_K_PART, &o->u, NULL, start, off, TSFSCK_W_SIZE, 1);
		return NULL;
	}
	if ( first ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( !claim(c, start + i, TSFSCK_K_PART, &o->u) ) return NULL;
		}
	}
	out = (UB *)Kmalloc((SZ)len + 1);
	blk = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( out == NULL || blk == NULL ) {
		if ( out != NULL ) Kfree(out);
		if ( blk != NULL ) Kfree(blk);
		return NULL;
	}
	for ( i = 0; i < cnt; i++ ) {
		if ( !mread(c, start + i, blk, TSFSBLK_MAGIC_TBL) ) {
			if ( first ) found(c, TSFSCK_K_PART, &o->u, NULL, start + i, off, TSFSCK_W_DAMAGED, 1);
			Kfree(out);
			Kfree(blk);
			return NULL;
		}
		n = MB_PAY;
		if ( n > (SZ)len - done ) n = (SZ)len - done;
		knl_memcpy(out + done, blk + TSFSBLK_HDR_SIZE, (INT)n);
		done += n;
	}
	Kfree(blk);
	*p_len = (SZ)len;
	return out;
}

/* The three tables of an object read again, quietly; NULL when one cannot be had */
typedef struct {
	UB	*place, *res, *link, *meta;
	INT	nplace, nres, nlink;
	SZ	metalen;
} CKTABS;

LOCAL void tabs_free( CKTABS *t )
{
	if ( t->place != NULL ) Kfree(t->place);
	if ( t->res != NULL ) Kfree(t->res);
	if ( t->link != NULL ) Kfree(t->link);
	if ( t->meta != NULL ) Kfree(t->meta);
	knl_memset(t, 0, sizeof(*t));
}

LOCAL BOOL tabs_read( CK *c, CKOBJ *o, CONST UB *hdr, BOOL first, CKTABS *t )
{
	SZ	len;
	BOOL	ok = TRUE;

	knl_memset(t, 0, sizeof(*t));
	t->nplace = (INT)rd32(hdr + OB_NPLACE);
	t->nres = (INT)rd32(hdr + OB_NRES);
	t->nlink = (INT)rd32(hdr + OB_NLINK);
	t->meta = part_read(c, o, hdr, OB_META, first, &t->metalen);
	if ( t->meta == NULL ) ok = FALSE;
	{
		UB	*icon = part_read(c, o, hdr, OB_ICON, first, &len);

		if ( icon == NULL ) ok = FALSE; else Kfree(icon);
	}
	t->place = part_read(c, o, hdr, OB_PLACE, first, &len);
	if ( t->place != NULL && len != (SZ)t->nplace * PE_SIZE ) {
		if ( first ) found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_PLACE, TSFSCK_W_COUNT, 1);
		Kfree(t->place);
		t->place = NULL;
	}
	t->res = part_read(c, o, hdr, OB_RES, first, &len);
	if ( t->res != NULL && len != (SZ)t->nres * RS_SIZE ) {
		if ( first ) found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_RES, TSFSCK_W_COUNT, 1);
		Kfree(t->res);
		t->res = NULL;
	}
	t->link = part_read(c, o, hdr, OB_LINK, first, &len);
	if ( t->link != NULL && len != (SZ)t->nlink * LK_SIZE ) {
		if ( first ) found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_LINK, TSFSCK_W_COUNT, 1);
		Kfree(t->link);
		t->link = NULL;
	}
	return (BOOL)( ok && t->place != NULL && t->res != NULL && t->link != NULL );
}

/* 通常インデックスと接続インデックス: the parts of each object and its three tables */
LOCAL void check_tables( CK *c, UB *hdr )
{
	CKTABS	t;
	INT	i, k, m;

	c->nlk = 0;
	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];
		BOOL	ok;

		o->lk0 = c->nlk;
		o->nlk = 0;
		if ( !o->read || ts_jrnl_read(c->vol, o->blk, hdr) < E_OK ) {
			continue;
		}
		ok = tabs_read(c, o, hdr, TRUE, &t);
		if ( !ok ) {
			c->broken = TRUE;
		}
		if ( t.place != NULL ) {
			UW	last = 0;

			for ( k = 0; k < t.nplace; k++ ) {
				CONST UB	*e = t.place + k * PE_SIZE;
				UW		rid = rd32(e + PE_RID);

				if ( rid == 0 || rid <= last || rid >= rd32(hdr + OB_NEXTRID) ) {
					found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_PLACE, TSFSCK_W_ORDER, 1);
					ok = FALSE;
				}
				if ( rd32(e + PE_KIND) != TSFS_REC_XTAD && rd32(e + PE_KIND) != TSFS_REC_BIN ) {
					found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_PLACE, TSFSCK_W_FORM, 1);
					ok = FALSE;
				}
				if ( rd64(e + PE_BYTES) != rd64(e + PE_PLACE + PL_BYTES) ) {
					found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_PLACE, TSFSCK_W_SIZE, 1);
					ok = FALSE;
				}
				last = rid;
			}
			c->records += (UD)t.nplace;
		}
		if ( t.res != NULL ) {
			for ( k = 0; k < t.nres; k++ ) {
				CONST UB	*e = t.res + k * RS_SIZE;

				for ( m = 0; m < k; m++ ) {
					CONST UB	*f = t.res + m * RS_SIZE;

					if ( rd32(e + RS_OWNER) == rd32(f + RS_OWNER) && rd32(e + RS_RESNO) == rd32(f + RS_RESNO) ) {
						found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_RES, TSFSCK_W_ORDER, 1);
						ok = FALSE;
					}
				}
				if ( rd64(e + RS_BYTES) != rd64(e + RS_PLACE + PL_BYTES) ) {
					found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_RES, TSFSCK_W_SIZE, 1);
					ok = FALSE;
				}
			}
		}
		if ( t.link != NULL ) {
			for ( k = 0; k < t.nlink; k++ ) {
				CONST UB	*e = t.link + k * LK_SIZE;

				if ( k > 0 && cmp_n(e, e - LK_SIZE, LK_KEY) < 0 ) {
					found(c, TSFSCK_K_TABLE, &o->u, NULL, o->blk, OB_LINK, TSFSCK_W_ORDER, 1);
					ok = FALSE;
				}
				if ( ( rd32(e + LK_FLAGS) & LK_F_EXTERNAL ) != 0 ) {
					continue;		/* off the volume: not counted */
				}
				if ( !grow((void **)&c->lk, &c->maxlk, c->nlk + 1, sizeof(CKLNK)) ) {
					ok = FALSE;
					break;
				}
				knl_memcpy(c->lk[c->nlk].target.b, e + LK_TARGET, 16);
				c->lk[c->nlk].from = i;
				c->nlk++;
				o->nlk++;
			}
			c->links += (UD)t.nlink;
			if ( t.nlink > 0 ) {
				c->legacy = FALSE;
			}
		}
		o->ok = ok;
		if ( !ok ) {
			c->broken = TRUE;
		}
		tabs_free(&t);
	}
}

/*
 * The runs of one leaf (in blk) claimed and looked at: each starts where
 * the one before ended (*p_have), none empty. FALSE when one is wrong.
 */
LOCAL BOOL leaf_runs( CK *c, CKOBJ *o, CONST UB *blk, UD n, UD *p_have )
{
	UD	k, i, l, p, cnt;

	for ( k = 0; k < n; k++ ) {
		l = rd64(blk + XL_ENT + k * XL_ESZ);
		p = rd64(blk + XL_ENT + k * XL_ESZ + 8);
		cnt = rd64(blk + XL_ENT + k * XL_ESZ + 16);
		if ( l != *p_have || cnt == 0 || cnt > c->sb.total_blocks ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, p, 0, TSFSCK_W_ORDER, 1);
			c->broken = TRUE;
			return FALSE;
		}
		for ( i = 0; i < cnt; i++ ) {
			if ( !claim(c, p + i, TSFSCK_K_DATA, &o->u) ) return FALSE;
		}
		*p_have = l + cnt;
	}
	return TRUE;
}

/*
 * A tree of two levels (design 7.3): the index and each leaf claimed,
 * each leaf where the index says it starts, holding as many runs as the
 * index says, and all of them full but the last.
 */
LOCAL BOOL tree2_runs( CK *c, CKOBJ *o, CONST UB *pl, UB *blk, UD *p_have )
{
	UD	idx = rd64(pl + PL_A), nl, k, leaf, cnt, first;
	UB	*ib;
	BOOL	ok = TRUE;

	if ( !claim(c, idx, TSFSCK_K_DATA, &o->u) ) {
		return FALSE;
	}
	ib = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( ib == NULL ) {
		c->broken = TRUE;
		return FALSE;
	}
	if ( !mread(c, idx, ib, TSFSBLK_MAGIC_TBL) ) {
		found(c, TSFSCK_K_DATA, &o->u, NULL, idx, 0, TSFSCK_W_DAMAGED, 1);
		c->broken = TRUE;
		Kfree(ib);
		return FALSE;
	}
	nl = rd32(ib + XL_N);
	if ( nl < 2 || nl > TSFSO_MAX_EXTENTS ) {
		found(c, TSFSCK_K_DATA, &o->u, NULL, idx, 0, TSFSCK_W_COUNT, 1);
		c->broken = TRUE;
		Kfree(ib);
		return FALSE;
	}
	for ( k = 0; k < nl && ok; k++ ) {
		first = rd64(ib + XL_ENT + k * XL_ESZ);
		leaf = rd64(ib + XL_ENT + k * XL_ESZ + 8);
		cnt = rd64(ib + XL_ENT + k * XL_ESZ + 16);
		ok = claim(c, leaf, TSFSCK_K_DATA, &o->u);
		if ( ok && !mread(c, leaf, blk, TSFSBLK_MAGIC_TBL) ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, leaf, 0, TSFSCK_W_DAMAGED, 1);
			c->broken = TRUE;
			ok = FALSE;
		}
		if ( ok && ( rd32(blk + XL_N) != cnt || cnt == 0 || first != *p_have
			     || ( k + 1 < nl && cnt != TSFSO_MAX_EXTENTS ) || cnt > TSFSO_MAX_EXTENTS ) ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, leaf, 0, TSFSCK_W_COUNT, 1);
			c->broken = TRUE;
			ok = FALSE;
		}
		if ( ok ) ok = leaf_runs(c, o, blk, cnt, p_have);
	}
	Kfree(ib);
	return ok;
}

/* A data part's place: its units, or its blocks claimed and the runs looked at */
LOCAL void data_place( CK *c, CKOBJ *o, CONST UB *pl, UB *blk )
{
	UD	len = rd64(pl + PL_BYTES), have = 0, need, i, k, n, l, p, cnt;

	switch ( pl[PL_FORM] ) {
	case F_NONE:
		if ( len != 0 ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, o->blk, 0, TSFSCK_W_SIZE, 1);
			c->broken = TRUE;
		}
		return;
	case F_INLINE:
		if ( !inline_take(c, o, pl, TSFSCK_K_DATA) ) c->broken = TRUE;
		return;
	case F_RUN:
		n = ( rd64(pl + PL_B) > 0 ) ? 1 : 0;
		break;
	case F_TREE:
		p = rd64(pl + PL_A);
		if ( !claim(c, p, TSFSCK_K_DATA, &o->u) ) {
			return;
		}
		if ( !mread(c, p, blk, TSFSBLK_MAGIC_TBL) ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, p, 0, TSFSCK_W_DAMAGED, 1);
			c->broken = TRUE;
			return;
		}
		n = rd32(blk + XL_N);
		if ( n > TSFSO_MAX_EXTENTS ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, p, 0, TSFSCK_W_COUNT, 1);
			c->broken = TRUE;
			return;
		}
		break;
	case F_TREE2:
		if ( !c->xt2 ) {
			/* the form, on a volume that does not say it has it */
			found(c, TSFSCK_K_DATA, &o->u, NULL, o->blk, 0, TSFSCK_W_FORM, 1);
			c->broken = TRUE;
			return;
		}
		if ( !tree2_runs(c, o, pl, blk, &have) ) {
			return;
		}
		n = 0;				/* the runs are looked at */
		break;
	default:
		found(c, TSFSCK_K_DATA, &o->u, NULL, o->blk, 0, TSFSCK_W_FORM, 1);
		c->broken = TRUE;
		return;
	}
	for ( k = 0; k < n; k++ ) {
		if ( pl[PL_FORM] == F_RUN ) {
			l = 0;
			p = rd64(pl + PL_A);
			cnt = rd64(pl + PL_B);
		} else {
			l = rd64(blk + XL_ENT + k * XL_ESZ);
			p = rd64(blk + XL_ENT + k * XL_ESZ + 8);
			cnt = rd64(blk + XL_ENT + k * XL_ESZ + 16);
		}
		if ( l != have || cnt == 0 || cnt > c->sb.total_blocks ) {
			found(c, TSFSCK_K_DATA, &o->u, NULL, p, 0, TSFSCK_W_ORDER, 1);
			c->broken = TRUE;
			return;
		}
		for ( i = 0; i < cnt; i++ ) {
			if ( !claim(c, p + i, TSFSCK_K_DATA, &o->u) ) return;
		}
		have = l + cnt;
	}
	need = BLKS_FOR(len, TSFSBLK_BLOCK_SIZE);
	if ( have < need ) {
		found(c, TSFSCK_K_DATA, &o->u, NULL, o->blk, (D)len, TSFSCK_W_SIZE, 1);
		c->broken = TRUE;
	}
}

/* The blocks of a data part read from the device, past every cache (完全) */
LOCAL void data_read( CK *c, CKOBJ *o, CONST UB *pl, UB *blk, UB *buf )
{
	UD	i, k, n, p, cnt;

	if ( pl[PL_FORM] == F_TREE2 ) {
		/* each leaf the index names, read in turn */
		UB	*ib = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		UD	nl, j;

		if ( ib == NULL ) return;
		if ( ts_jrnl_read(c->vol, rd64(pl + PL_A), ib) >= E_OK ) {
			nl = rd32(ib + XL_N);
			for ( j = 0; j < nl && j < TSFSO_MAX_EXTENTS; j++ ) {
				if ( ts_jrnl_read(c->vol, rd64(ib + XL_ENT + j * XL_ESZ + 8), blk) < E_OK ) continue;
				n = rd32(blk + XL_N);
				for ( k = 0; k < n && k < TSFSO_MAX_EXTENTS; k++ ) {
					p = rd64(blk + XL_ENT + k * XL_ESZ + 8);
					cnt = rd64(blk + XL_ENT + k * XL_ESZ + 16);
					for ( i = 0; i < cnt && p + i < c->sb.total_blocks; i++ ) {
						c->checked++;
						if ( knl_tsfsblk_raw_read(c->vol, p + i, buf) < E_OK ) {
							found(c, TSFSCK_K_UNREAD, &o->u, NULL, p + i, 0, 0, 1);
						}
					}
				}
			}
		}
		Kfree(ib);
		return;
	}
	if ( pl[PL_FORM] == F_RUN ) {
		n = 1;
	} else if ( pl[PL_FORM] == F_TREE && ts_jrnl_read(c->vol, rd64(pl + PL_A), blk) >= E_OK ) {
		n = rd32(blk + XL_N);
		if ( n > TSFSO_MAX_EXTENTS ) return;
	} else {
		return;
	}
	for ( k = 0; k < n; k++ ) {
		p = ( pl[PL_FORM] == F_RUN ) ? rd64(pl + PL_A) : rd64(blk + XL_ENT + k * XL_ESZ + 8);
		cnt = ( pl[PL_FORM] == F_RUN ) ? rd64(pl + PL_B) : rd64(blk + XL_ENT + k * XL_ESZ + 16);
		for ( i = 0; i < cnt && p + i < c->sb.total_blocks; i++ ) {
			c->checked++;
			if ( knl_tsfsblk_raw_read(c->vol, p + i, buf) < E_OK ) {
				found(c, TSFSCK_K_UNREAD, &o->u, NULL, p + i, 0, 0, 1);
			}
		}
	}
}

/* フラグメント・区切り・レコードサイズ: where each record's and resource's bytes are */
LOCAL void check_data( CK *c, UB *hdr )
{
	CKTABS	t;
	UB	*blk, *buf;
	INT	i, k;

	blk = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( blk == NULL || buf == NULL ) {
		if ( blk != NULL ) Kfree(blk);
		if ( buf != NULL ) Kfree(buf);
		c->broken = TRUE;
		return;
	}
	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];

		if ( !o->ok || ts_jrnl_read(c->vol, o->blk, hdr) < E_OK ) {
			continue;
		}
		if ( !tabs_read(c, o, hdr, FALSE, &t) ) {
			tabs_free(&t);
			continue;
		}
		for ( k = 0; k < t.nplace; k++ ) {
			data_place(c, o, t.place + k * PE_SIZE + PE_PLACE, blk);
			if ( ( c->mode & TSFSCK_FULL ) != 0 ) {
				data_read(c, o, t.place + k * PE_SIZE + PE_PLACE, blk, buf);
			}
		}
		for ( k = 0; k < t.nres; k++ ) {
			data_place(c, o, t.res + k * RS_SIZE + RS_PLACE, blk);
			if ( ( c->mode & TSFSCK_FULL ) != 0 ) {
				data_read(c, o, t.res + k * RS_SIZE + RS_PLACE, blk, buf);
			}
		}
		tabs_free(&t);
		/* every unit a part sits in has to be taken in the map */
		if ( ( o->inl & ~rd64(hdr + OB_INLMAP) ) != 0 ) {
			found(c, TSFSCK_K_HEAD, &o->u, NULL, o->blk, 0, TSFSCK_W_SHARED, 1);
			c->broken = TRUE;
		}
	}
	Kfree(blk);
	Kfree(buf);
}

/* The "records" of a metadata text against the rids of its placement table */
LOCAL BOOL records_agree( CONST UB *j, INT len, CONST UB *place, INT nplace )
{
	T_JSON	root, arr, it;
	UB	seen[TSFSO_MAX_REC];
	INT	n = 0, k;
	UW	rid;

	if ( js_span(j, len, 0, &root) < E_OK || js_type(&root) != JS_OBJECT
	  || js_get(&root, "records", &arr) < E_OK ) {
		return TRUE;			/* none: the order of the rids */
	}
	if ( js_type(&arr) != JS_ARRAY ) {
		return FALSE;
	}
	knl_memset(seen, 0, sizeof(seen));
	it.s = NULL;
	while ( js_next(&arr, &it) ) {
		if ( js_type(&it) != JS_OBJECT || n >= nplace ) {
			return FALSE;
		}
		rid = (UW)js_get_num(&it, "rid", 0);
		for ( k = 0; k < nplace && rd32(place + k * PE_SIZE + PE_RID) != rid; k++ ) ;
		if ( k == nplace || seen[k] ) {
			return FALSE;
		}
		seen[k] = 1;
		n++;
	}
	return (BOOL)( n == nplace );
}

/* ファイルヘッダー情報: the metadata, the counts, the garbage list and the orphan tree */
LOCAL void check_info( CK *c, UB *hdr )
{
	CKTABS		t;
	T_JSON		root;
	CONST TS_UUID	*roots[4];
	INT		i, k;

	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];

		o->want = o->pins;
		if ( !o->ok || ts_jrnl_read(c->vol, o->blk, hdr) < E_OK ) {
			continue;
		}
		if ( !tabs_read(c, o, hdr, FALSE, &t) ) {
			tabs_free(&t);
			continue;
		}
		if ( t.metalen > 0 && ( js_span(t.meta, (INT)t.metalen, 0, &root) < E_OK
					|| js_type(&root) != JS_OBJECT ) ) {
			found(c, TSFSCK_K_META, &o->u, NULL, o->blk, 0, TSFSCK_W_FORM, 1);
		} else if ( t.metalen > 0 && !records_agree(t.meta, (INT)t.metalen, t.place, t.nplace) ) {
			found(c, TSFSCK_K_META, &o->u, NULL, o->blk, 0, TSFSCK_W_ORDER, 1);
		}
		tabs_free(&t);
	}

	/* the links: each counted one adds to what it points at */
	for ( k = 0; k < c->nlk; k++ ) {
		CKOBJ	*to = obj_find(c, &c->lk[k].target), *from = &c->obj[c->lk[k].from];

		if ( to == NULL ) {
			found(c, TSFSCK_K_LINK, &from->u, &c->lk[k].target, from->blk, 0, 0, 1);
			from->badlink = TRUE;
		} else {
			to->want++;
		}
	}

	/* the objects the superblock names */
	roots[0] = &c->sb.root_uuid;
	roots[1] = &c->sb.sysbox_uuid;
	roots[2] = &c->sb.domain_uuid;
	roots[3] = &c->sb.xref_uuid;	/* and what other volumes link to through it */
	for ( k = 0; k < 4; k++ ) {
		CKOBJ	*o = obj_find(c, roots[k]);

		if ( !is_nil(roots[k]) && ( o == NULL || o->orphan ) && c->tree[TSFSBT_OBJ].ok ) {
			found(c, TSFSCK_K_SUPER, roots[k], NULL, 0, k, 2, 1);
		}
	}

	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];

		if ( !o->read ) {
			continue;
		}
		/* the marks of the orphan tree */
		if ( o->orphan ) {
			if ( ( o->flags & TSFSO_F_ORPHAN ) == 0 ) {
				found(c, TSFSCK_K_ORPHAN, &o->u, NULL, o->blk, 0, 3, 1);
			}
			continue;
		}
		if ( ( o->flags & TSFSO_F_ORPHAN ) != 0 ) {
			found(c, TSFSCK_K_ORPHAN, &o->u, NULL, o->blk, 0, 2, 1);
			continue;
		}
		if ( o->on_orphan ) {
			CKITEM	*r = tree_find(&c->tree[TSFSBT_ORPHAN], &o->u);

			if ( r != NULL && r->v != o->blk ) {
				found(c, TSFSCK_K_ORPHAN, &o->u, NULL, r->v, 0, 0, 1);
			} else if ( ( o->flags & TSFSO_F_RELINK ) == 0 ) {
				found(c, TSFSCK_K_ORPHAN, &o->u, NULL, o->blk, 0, 1, 1);
			}
		} else if ( ( o->flags & TSFSO_F_RELINK ) != 0 ) {
			found(c, TSFSCK_K_RELINK, &o->u, NULL, o->blk, 0, 0, 1);
		}
		if ( !o->ok ) {
			continue;
		}
		/* the count, which a volume that keeps no link tables does not hold,
		   and which cannot be judged while a link table could not be read */
		if ( !c->legacy && !c->broken && ( o->refcnt != o->want || o->pins > o->refcnt ) ) {
			found(c, TSFSCK_K_REFCNT, &o->u, NULL, o->blk, o->refcnt, o->want, 1);
		}
		/* the garbage list: the objects of count nought that have been referred to */
		if ( o->on_gc ) {
			if ( o->gc_v != o->blk ) {
				found(c, TSFSCK_K_GCLIST, &o->u, NULL, o->gc_v, o->refcnt, 1, 1);
			} else if ( ( o->flags & TSFSO_F_GARBAGE ) == 0 ) {
				found(c, TSFSCK_K_GCLIST, &o->u, NULL, o->blk, o->refcnt, 2, 1);
			} else if ( o->refcnt != 0 ) {
				found(c, TSFSCK_K_GCLIST, &o->u, NULL, o->blk, o->refcnt, 3, 1);
			}
		} else if ( ( o->flags & TSFSO_F_GARBAGE ) != 0 ) {
			found(c, TSFSCK_K_GCLIST, &o->u, NULL, o->blk, o->refcnt, 4, 1);
		}
	}
	/* entries of the garbage list for objects that are not there */
	for ( k = 0; k < c->tree[TSFSBT_GC].n; k++ ) {
		CKOBJ	*o = obj_find(c, &c->tree[TSFSBT_GC].it[k].u);

		if ( o == NULL || o->orphan ) {
			found(c, TSFSCK_K_GCLIST, &c->tree[TSFSBT_GC].it[k].u, NULL, c->tree[TSFSBT_GC].it[k].v, 0, 0, 1);
		}
	}
}

/* ---------------------------------------------------------------- the maps */

/* Whether one block of group `n` is taken in the map copy `m` of its map block `k` */
LOCAL BOOL map_bit( CONST UB *m, UD off )
{
	UD	bit = off % BITS_PER_MAP;

	return (BOOL)( ( m[bit / 8] & ( 1 << ( bit % 8 ) ) ) != 0 );
}

/* The objects in the order of their UUIDs */
LOCAL void obj_sort( CK *c )
{
	CKOBJ	x;
	INT	gap, i, k;

	for ( gap = c->nobj / 2; gap > 0; gap /= 2 ) {
		for ( i = gap; i < c->nobj; i++ ) {
			x = c->obj[i];
			for ( k = i; k >= gap && cmp_n(c->obj[k - gap].u.b, x.u.b, 16) > 0; k -= gap ) {
				c->obj[k] = c->obj[k - gap];
			}
			c->obj[k] = x;
		}
	}
}

/* ---------------------------------------------------------------- old copies of objects */

/*
 * Under a map that does not hold, an object block read from the blocks
 * may be an old copy: of an object deleted since, whose blocks were given
 * back and some of them taken by another. Two objects that take the same
 * block are told apart by the transaction that last wrote each one's
 * block: the older is the old copy, and is left out of the volume from
 * then on (its blocks, used by nothing, are given back by the repair).
 * One that takes a block the volume keeps for itself is an old copy too.
 */
LOCAL BOOL is_stale( CK *c, UD blk )
{
	INT	i;

	for ( i = 0; i < c->nstale; i++ ) {
		if ( c->stale[i] == blk ) return TRUE;
	}
	return FALSE;
}

LOCAL void stale_pick( CK *c, UD blk, CONST TS_UUID *u )
{
	CKOBJ	*a = obj_find(c, u), *b = NULL, *old;
	INT	w = c->owner[blk];

	if ( a == NULL ) {
		c->broken = TRUE;
		return;
	}
	if ( w > 0 ) {
		b = &c->obj[w - 1];
	}
	if ( b == a ) {
		c->broken = TRUE;		/* one object that takes a block twice: a fault of its own */
		return;
	}
	old = ( b == NULL || a->gen < b->gen ) ? a : ( b->gen < a->gen ) ? b : NULL;
	if ( old == NULL || is_stale(c, old->blk) ) {
		if ( old == NULL ) c->broken = TRUE;	/* written in the same transaction: cannot be told */
		return;
	}
	if ( !grow((void **)&c->stale, &c->maxstale, c->nstale + 1, sizeof(UD)) ) {
		c->broken = TRUE;
		return;
	}
	c->stale[c->nstale++] = old->blk;
	c->restale = TRUE;
}

/*
 * The objects found by reading every block the maps say is taken and
 * nothing claimed yet, for an object block with a good head: how the
 * objects are known when the index or the orphan tree cannot be walked.
 * A freed block keeps what it held, so only the blocks the maps say are
 * taken are read. With the index whole, its objects are taken from it
 * and only those deleted while open are taken from the blocks. An
 * object found in two blocks is the one the later transaction wrote;
 * two of the same transaction cannot be told apart and leave the
 * picture incomplete.
 */
LOCAL void scan_objects( CK *c, UB *buf, UB *map )
{
	CKTREE	*ix = &c->tree[TSFSBT_OBJ];
	CKITEM	it;
	UD	n, off, lo, hi, blk;
	UW	k;
	INT	i, w;
	BOOL	whole = (BOOL)!c->tree_bad[TSFSBT_OBJ];

	c->scanned = TRUE;
	c->nobj = 0;
	for ( i = 0; whole && i < ix->n; i++ ) {
		if ( !obj_add(c, &ix->it[i], FALSE) ) { c->broken = TRUE; return; }
	}
	for ( n = 0; n < c->sb.ag_count; n++ ) {
		T_TSFSAGREF	*a = &c->ag[n];

		for ( k = 0; k < a->map_blocks; k++ ) {
			ER	me = ts_blk_map(c->vol, n, k, map);
			BOOL	all = FALSE;

			if ( me == E_OBJ && c->map_bad ) {
				all = TRUE;		/* a map that does not hold: every block under it read */
			} else if ( me < E_OK ) {
				c->broken = TRUE;
				return;
			}
			lo = (UD)k * BITS_PER_MAP;
			hi = lo + BITS_PER_MAP;
			if ( lo < a->meta ) lo = a->meta;
			if ( hi > a->len ) hi = a->len;
			for ( off = lo; off < hi; off++ ) {
				blk = a->first + off;
				if ( ( !all && !map_bit(map, off) ) || used_bit(c->claim, blk)
				  || is_stale(c, blk) ) continue;
				c->checked++;
				if ( ts_jrnl_read(c->vol, blk, buf) < E_OK
				  || ts_blk_good(c->vol, buf, TSFSBLK_MAGIC_OBJ, blk) < E_OK ) {
					continue;
				}
				knl_memcpy(it.u.b, buf + OB_UUID, 16);
				it.v = blk;
				if ( whole && ( tree_find(ix, &it.u) != NULL || ( rd32(buf + OB_FLAGS) & TSFSO_F_ORPHAN ) == 0 ) ) {
					continue;
				}
				if ( !obj_add(c, &it, !whole) ) {
					c->broken = TRUE;
					return;
				}
				c->obj[c->nobj - 1].gen = rd64(buf + 40);
			}
		}
	}
	obj_sort(c);
	for ( i = 0, w = 0; i < c->nobj; i++ ) {
		if ( w > 0 && cmp_n(c->obj[w - 1].u.b, c->obj[i].u.b, 16) == 0 ) {
			if ( c->obj[i].gen == c->obj[w - 1].gen ) {
				found(c, TSFSCK_K_INDEX, &c->obj[i].u, NULL, c->obj[i].blk, 2, TSFSCK_W_WRONG, 1);
				c->broken = TRUE;
			} else if ( c->obj[i].gen > c->obj[w - 1].gen ) {
				c->obj[w - 1] = c->obj[i];
			}
			continue;
		}
		c->obj[w++] = c->obj[i];
	}
	c->nobj = w;
}

/*
 * The old copies of objects sifted out, under a map that does not hold,
 * before the objects are looked at for what is told: the objects found
 * from the blocks, their blocks and their parts taken, again and again
 * until no two take the same block, quietly. What is kept of the pass is
 * as it was before, but for the list of old copies (c->stale).
 */
#define SIFT_ROUNDS	8

LOCAL void sift( CK *c, UB *buf, UB *blk )
{
	INT	scratch[TSFSCK_NKIND], *count = c->count;
	UD	bytes = ( c->sb.total_blocks + 7 ) / 8, records = c->records, links = c->links;
	UD	checked = c->checked;
	UB	*keep;
	UINT	mode = c->mode;
	BOOL	tell = c->tell, legacy = c->legacy, settled = FALSE;
	INT	unfixable = c->unfixable, round;

	keep = (UB *)Kmalloc((SZ)bytes);
	c->owner = (INT *)Kmalloc(sizeof(INT) * (SZ)c->sb.total_blocks);
	if ( keep == NULL || c->owner == NULL ) {
		if ( keep != NULL ) Kfree(keep);
		if ( c->owner != NULL ) Kfree(c->owner);
		c->owner = NULL;
		c->broken = TRUE;
		return;
	}
	knl_memcpy(keep, c->claim, (INT)bytes);
	c->tell = FALSE;
	c->count = scratch;
	c->mode &= ~(UINT)TSFSCK_FULL;
	c->probing = TRUE;
	for ( round = 0; round < SIFT_ROUNDS && !settled; round++ ) {
		knl_memset(scratch, 0, sizeof(scratch));
		knl_memcpy(c->claim, keep, (INT)bytes);
		knl_memset(c->owner, 0, sizeof(INT) * (SZ)c->sb.total_blocks);
		c->restale = FALSE;
		c->broken = FALSE;
		c->nlk = 0;
		scan_objects(c, buf, blk);
		if ( c->broken ) {
			break;
		}
		check_heads(c, buf);
		check_tables(c, buf);
		check_data(c, buf);
		settled = (BOOL)!c->restale;
	}
	c->probing = FALSE;
	Kfree(c->owner);
	c->owner = NULL;
	knl_memcpy(c->claim, keep, (INT)bytes);
	Kfree(keep);
	c->tell = tell;
	c->count = count;
	c->mode = mode;
	c->legacy = legacy;
	c->unfixable = unfixable;
	c->records = records;
	c->links = links;
	c->checked = checked;
	c->ndamaged = 0;
	c->nlk = 0;
	c->nobj = 0;
	/* what else is wrong is found again by the pass; one that never settles is not put right */
	c->broken = (BOOL)!settled;
	if ( c->nstale > 0 ) {
		tm_printf((UB *)"tsfsck: %d old copies of objects left out\n", c->nstale);
	}
}

/* The trees that could not be walked, as the objects make them */
LOCAL void derive_trees( CK *c )
{
	INT	t, i;

	for ( t = 0; t < CK_TREES; t++ ) {
		CKTREE	*tr = &c->tree[t];

		if ( !c->tree_bad[t] ) continue;
		tr->n = 0;
		for ( i = 0; i < c->nobj; i++ ) {
			CKOBJ	*o = &c->obj[i];
			BOOL	in;

			if ( !o->read ) continue;
			in = ( t == TSFSBT_OBJ ) ? (BOOL)( ( o->flags & TSFSO_F_ORPHAN ) == 0 )
			   : ( t == TSFSBT_GC ) ? (BOOL)( ( o->flags & TSFSO_F_GARBAGE ) != 0
							&& ( o->flags & TSFSO_F_ORPHAN ) == 0 )
			   : (BOOL)( ( o->flags & ( TSFSO_F_ORPHAN | TSFSO_F_RELINK ) ) != 0 );
			if ( in && !tree_add(tr, o->u.b, o->blk) ) {
				c->broken = TRUE;
			}
		}
	}
}

typedef void (*FP_RUN)( CK *c, INT kind, UD start, UD n, void *arg );

/*
 * Each run of blocks of the data area whose map and use disagree: in use
 * but free in the map (K_LOST), or taken in the map and used by nothing
 * (K_LEAK), handed to `fn` a run at a time. A group whose map cannot be
 * had is passed over.
 */
LOCAL void map_runs( CK *c, UB *buf, FP_RUN fn, void *arg )
{
	UD	n, off, lo, hi, run0 = 0, len = 0;
	UW	k;
	INT	kind = 0, now;

	for ( n = 0; n < c->sb.ag_count; n++ ) {
		T_TSFSAGREF	*a = &c->ag[n];

		for ( k = 0; k < a->map_blocks; k++ ) {
			if ( ts_blk_map(c->vol, n, k, buf) < E_OK ) {
				continue;
			}
			lo = (UD)k * BITS_PER_MAP;
			hi = lo + BITS_PER_MAP;
			if ( lo < a->meta ) lo = a->meta;
			if ( hi > a->len ) hi = a->len;
			for ( off = lo; off < hi; off++ ) {
				BOOL	have = map_bit(buf, off), want = used_bit(c->claim, a->first + off);

				now = ( want && !have ) ? TSFSCK_K_LOST : ( have && !want ) ? TSFSCK_K_LEAK : 0;
				if ( now == kind && now != 0 && run0 + len == a->first + off ) {
					len++;
					continue;
				}
				if ( kind != 0 ) (*fn)(c, kind, run0, len, arg);
				kind = now;
				run0 = a->first + off;
				len = 1;
			}
			if ( kind != 0 ) (*fn)(c, kind, run0, len, arg);
			kind = 0;
		}
	}
}

LOCAL void tell_run( CK *c, INT kind, UD start, UD n, void *arg )
{
	(void)arg;
	/* blocks nothing uses are not judged while something could not be read */
	if ( kind == TSFSCK_K_LEAK && c->broken ) {
		return;
	}
	found(c, kind, NULL, NULL, start, (D)n, 0, (INT)n);
}

/* The free blocks read from the device, past every cache (完全) */
LOCAL void check_free( CK *c, UB *buf, UB *blk )
{
	UD	n, off, lo, hi;
	UW	k;

	for ( n = 0; n < c->sb.ag_count; n++ ) {
		T_TSFSAGREF	*a = &c->ag[n];

		for ( k = 0; k < a->map_blocks; k++ ) {
			if ( ts_blk_map(c->vol, n, k, buf) < E_OK ) {
				continue;
			}
			lo = (UD)k * BITS_PER_MAP;
			hi = lo + BITS_PER_MAP;
			if ( lo < a->meta ) lo = a->meta;
			if ( hi > a->len ) hi = a->len;
			for ( off = lo; off < hi; off++ ) {
				if ( map_bit(buf, off) ) continue;
				c->checked++;
				if ( knl_tsfsblk_raw_read(c->vol, a->first + off, blk) < E_OK ) {
					found(c, TSFSCK_K_UNREAD, NULL, NULL, a->first + off, 0, 1, 1);
				}
			}
		}
	}
}

/* ---------------------------------------------------------------- objects no live object reaches */

/*
 * What a live object reaches: from the objects the superblock names,
 * those held, those of count nought (garbage, whose links still count
 * until it is collected) and those deleted while open, along the counted
 * links. An object left over with a count above nought is reached only
 * from objects that are themselves left over: a 隠れ屑実身.
 */
LOCAL void check_hidden( CK *c )
{
	INT		*q, nq = 0, i, k;
	CONST TS_UUID	*roots[4];
	CKOBJ		*o;

	for ( i = 0; i < c->nobj; i++ ) {
		c->obj[i].reached = FALSE;
		c->obj[i].hidden = FALSE;
	}
	if ( c->broken || c->legacy || c->nobj == 0 ) {
		return;
	}
	q = (INT *)Kmalloc(sizeof(INT) * (SZ)c->nobj);
	if ( q == NULL ) {
		return;
	}
	roots[0] = &c->sb.root_uuid;
	roots[1] = &c->sb.sysbox_uuid;
	roots[2] = &c->sb.domain_uuid;
	roots[3] = &c->sb.xref_uuid;	/* and what other volumes link to through it */
	for ( k = 0; k < 4; k++ ) {
		o = obj_find(c, roots[k]);
		if ( o != NULL && !o->reached ) {
			o->reached = TRUE;
			q[nq++] = (INT)( o - c->obj );
		}
	}
	for ( i = 0; i < c->nobj; i++ ) {
		o = &c->obj[i];
		if ( !o->reached && ( o->pins > 0 || o->want == 0 || o->orphan || !o->ok ) ) {
			o->reached = TRUE;
			q[nq++] = i;
		}
	}
	for ( i = 0; i < nq; i++ ) {
		CKOBJ	*from = &c->obj[q[i]];

		for ( k = from->lk0; k < from->lk0 + from->nlk; k++ ) {
			CKOBJ	*to = obj_find(c, &c->lk[k].target);

			if ( to != NULL && !to->reached ) {
				to->reached = TRUE;
				q[nq++] = (INT)( to - c->obj );
			}
		}
	}
	Kfree(q);
	for ( i = 0; i < c->nobj; i++ ) {
		o = &c->obj[i];
		if ( !o->reached ) {
			o->hidden = TRUE;
			found(c, TSFSCK_K_HIDDEN, &o->u, NULL, o->blk, o->want, 0, 1);
		}
	}
}

/* ---------------------------------------------------------------- a pass */

LOCAL void pass_free( CK *c )
{
	INT	i;

	if ( c->claim != NULL ) Kfree(c->claim);
	if ( c->ag != NULL ) Kfree(c->ag);
	c->claim = NULL;
	c->ag = NULL;
	for ( i = 0; i < CK_TREES; i++ ) {
		c->tree[i].n = 0;
	}
	c->nobj = 0;
	c->nlk = 0;
}

LOCAL void all_free( CK *c )
{
	INT	i;

	pass_free(c);
	for ( i = 0; i < CK_TREES; i++ ) {
		if ( c->tree[i].it != NULL ) Kfree(c->tree[i].it);
		c->tree[i].it = NULL;
		c->tree[i].max = 0;
	}
	if ( c->obj != NULL ) Kfree(c->obj);
	if ( c->lk != NULL ) Kfree(c->lk);
	if ( c->stale != NULL ) Kfree(c->stale);
	c->obj = NULL;
	c->lk = NULL;
	c->stale = NULL;
	c->maxobj = c->maxlk = c->maxstale = 0;
	c->nstale = 0;
}

/* The block layer's own blocks, and the blocks it keeps for itself marked as used */
LOCAL ER check_super( CK *c )
{
	UD	n, b;

	if ( ts_ref_vol_blk(c->vol, &c->sb) < E_OK ) {
		return E_ID;
	}
	c->xt2 = (BOOL)( ( c->sb.incompat & TSFSBLK_INCOMPAT_XT2 ) != 0 );
	c->ag_first = c->sb.journal_start + c->sb.journal_blocks;
	c->claim = (UB *)Kmalloc((SZ)( c->sb.total_blocks / 8 + 1 ));
	c->ag = (T_TSFSAGREF *)Kmalloc(sizeof(T_TSFSAGREF) * (SZ)c->sb.ag_count);
	if ( c->claim == NULL || c->ag == NULL ) {
		return E_NOMEM;
	}
	knl_memset(c->claim, 0, (INT)( c->sb.total_blocks / 8 + 1 ));
	for ( n = 0; n < c->sb.ag_count; n++ ) {
		if ( ts_blk_group(c->vol, n, &c->ag[n]) < E_OK ) {
			return E_SYS;
		}
		for ( b = c->ag[n].first; b < c->ag[n].first + c->ag[n].meta; b++ ) {
			c->claim[b >> 3] |= (UB)( 1 << ( b & 7 ) );
		}
	}
	if ( ts_blk_inspect(c->vol, &c->insp) < E_OK ) {
		return E_SYS;
	}
	if ( c->insp.sb_bad > 0 ) {
		found(c, TSFSCK_K_SUPER, NULL, NULL, c->insp.sb_first, (D)c->insp.sb_bad, 0, 1);
	}
	if ( c->insp.head_bad > 0 ) {
		found(c, TSFSCK_K_GROUP, NULL, NULL, c->insp.head_first, (D)c->insp.head_bad, 0, 1);
	}
	if ( c->insp.map_bad > 0 ) {
		c->map_bad = TRUE;
		found(c, TSFSCK_K_GROUP, NULL, NULL, c->insp.map_first, (D)c->insp.map_bad, 1, 1);
	}
	if ( c->insp.count_bad > 0 ) {
		found(c, TSFSCK_K_FREECNT, NULL, NULL, 0, (D)c->insp.count_bad, (D)c->insp.count_first, 1);
	}
	/* an error the volume recorded: cleared once a repair leaves nothing */
	if ( c->insp.error && !c->verify ) {
		found(c, TSFSCK_K_SUPER, NULL, NULL, c->sb.err_blk, c->sb.err_kind, 1, 1);
	}
	return E_OK;
}

/* One whole look at the volume */
LOCAL ER pass( CK *c )
{
	UB	*buf, *blk;
	INT	i;
	ER	er;

	pass_free(c);
	c->broken = FALSE;
	c->legacy = TRUE;
	c->scanned = FALSE;
	c->map_bad = FALSE;
	for ( i = 0; i < CK_TREES; i++ ) {
		c->tree_bad[i] = FALSE;
	}
	c->unfixable = 0;
	c->ndamaged = 0;
	c->records = c->links = c->checked = 0;
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	blk = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL || blk == NULL ) {
		if ( buf != NULL ) Kfree(buf);
		if ( blk != NULL ) Kfree(blk);
		return E_NOMEM;
	}

	phase(c, TSFSCK_PH_SUPER);
	er = check_super(c);
	if ( er < E_OK ) {
		goto exit;
	}

	phase(c, TSFSCK_PH_INDEX);
	for ( i = 0; i < CK_TREES; i++ ) {
		walk_tree(c, i);
	}
	if ( c->tree_bad[TSFSBT_OBJ] || c->tree_bad[TSFSBT_ORPHAN] ) {
		/*
		 * The objects found from the blocks. Under a map that does not
		 * hold every block is read, and old copies of objects given up
		 * since are told from the live ones first.
		 */
		if ( c->map_bad ) {
			sift(c, buf, blk);
			if ( c->broken ) {
				goto exit;
			}
		}
		scan_objects(c, buf, blk);
		if ( c->broken ) {
			goto exit;
		}
	} else {
		list_objects(c);
	}

	phase(c, TSFSCK_PH_HEAD);
	check_heads(c, buf);
	derive_trees(c);
	phase(c, TSFSCK_PH_TABLE);
	check_tables(c, buf);
	if ( ( c->mode & TSFSCK_QUICK ) == 0 || c->map_bad ) {
		/* the records' blocks as well: a map made again has to know them */
		phase(c, TSFSCK_PH_DATA);
		check_data(c, buf);
	}
	phase(c, TSFSCK_PH_INFO);
	check_info(c, buf);
	if ( ( c->mode & TSFSCK_QUICK ) == 0 ) {
		phase(c, TSFSCK_PH_SPACE);
		map_runs(c, buf, tell_run, NULL);
	}
	if ( ( c->mode & TSFSCK_FULL ) != 0 ) {
		phase(c, TSFSCK_PH_FREE);
		check_free(c, buf, blk);
	}
	phase(c, TSFSCK_PH_HIDDEN);
	check_hidden(c);

    exit:
	Kfree(buf);
	Kfree(blk);
	return er;
}

/* ---------------------------------------------------------------- the repair */

/*
 * The objects whose block reads but does not pass its check: what can be
 * read of each -- its name, the records that read whole -- is made into
 * a new object (ts_obj_salvage), and the old one is taken out of the
 * trees, so that its UUID names nothing and its blocks, used by nothing,
 * are given back with the rest. The new objects are gathered into a
 * TRASH of their own, as the 隠れ屑実身 are: nothing links to them, and
 * whoever looks there decides what to keep. A link to the old object
 * points at nothing from then on, and its link table says so.
 */
LOCAL ER make_trash( CK *c, CONST TS_UUID *list, INT nlist, TS_UUID *p_u );

LOCAL void fix_damaged( CK *c )
{
	TS_UUID	*saved, trash;
	INT	i, n = 0, recs;
	BOOL	sv;
	ER	er;

	saved = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( c->ndamaged + 1 ));
	if ( saved == NULL ) {
		return;
	}
	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];
		TS_UUID	nu;

		if ( !o->damaged ) {
			continue;
		}
		recs = 0;
		sv = (BOOL)( ts_obj_salvage(c->vol, o->blk, &nu, &recs) >= E_OK );
		if ( sv && n < c->ndamaged ) {
			saved[n++] = nu;
		}
		if ( ts_obj_begin(c->vol) < E_OK ) {
			continue;
		}
		/* out of every tree; the index entry last, as the store takes an object out */
		(void)ts_btree_delete_t(c->vol, TSFSBT_GC, &o->u);
		(void)ts_btree_delete_t(c->vol, TSFSBT_ORPHAN, &o->u);
		er = ts_btree_delete(c->vol, &o->u);
		if ( er == E_NOEXS ) er = E_OK;
		if ( ts_obj_end(c->vol, (BOOL)( er >= E_OK )) >= E_OK && er >= E_OK ) {
			mended(c, TSFSCK_K_HEAD, &o->u, o->blk, recs, sv ? 1 : 0, 1);
		}
	}
	if ( n > 0 && make_trash(c, saved, n, &trash) >= E_OK ) {
		c->res->has_trash = TRUE;
		c->res->trash = trash;
	}
	Kfree(saved);
}

/*
 * A map block that does not match its head, made again from the blocks
 * found in use: those the group keeps for itself, the bits past its end,
 * and every block a thing claimed. Only from a whole picture, and first
 * of all, before anything takes a block from that group.
 */
LOCAL void fix_bitmaps( CK *c )
{
	UB	*m, *b;
	UD	n, off, bit;
	UW	k;
	ER	er;

	if ( !c->map_bad || c->broken ) {
		return;
	}
	m = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	b = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	for ( n = 0; m != NULL && b != NULL && n < c->sb.ag_count; n++ ) {
		T_TSFSAGREF	*a = &c->ag[n];

		for ( k = 0; k < a->map_blocks; k++ ) {
			if ( ts_blk_map(c->vol, n, k, b) != E_OBJ ) {
				continue;		/* this one is good */
			}
			knl_memset(m, 0, TSFSBLK_BLOCK_SIZE);
			for ( bit = 0; bit < BITS_PER_MAP; bit++ ) {
				off = (UD)k * BITS_PER_MAP + bit;
				if ( off < a->meta || off >= a->len || used_bit(c->claim, a->first + off) ) {
					m[bit / 8] |= (UB)( 1 << ( bit % 8 ) );
				}
			}
			if ( ts_obj_begin(c->vol) < E_OK ) continue;
			er = ts_blk_map_put(c->vol, n, k, m);
			if ( ts_obj_end(c->vol, (BOOL)( er >= E_OK )) >= E_OK && er >= E_OK ) {
				mended(c, TSFSCK_K_GROUP, NULL, a->map_first + k, 1, 1, 1);
			}
		}
	}
	if ( m != NULL ) Kfree(m);
	if ( b != NULL ) Kfree(b);
}

/*
 * The trees that could not be walked, made again from the objects in one
 * transaction: the old root forgotten and every key put in, so that a
 * cut in the power leaves the old tree or the new one and never an empty
 * one. The old tree's nodes are then used by nothing and are given back
 * with the rest of such blocks at the end of the repair.
 */
LOCAL void fix_trees( CK *c )
{
	INT	t, i;
	BOOL	any = FALSE;
	ER	er = E_OK;

	for ( t = 0; t < CK_TREES; t++ ) {
		if ( c->tree_bad[t] ) any = TRUE;
	}
	if ( !any || c->broken || ts_obj_begin(c->vol) < E_OK ) {
		return;
	}
	for ( t = 0; t < CK_TREES && er >= E_OK; t++ ) {
		CKTREE	*tr = &c->tree[t];

		if ( !c->tree_bad[t] ) continue;
		er = ( t == TSFSBT_OBJ ) ? ts_set_root_blk(c->vol, 0, 0)
		   : ( t == TSFSBT_GC ) ? ts_set_gc_root_blk(c->vol, 0, 0) : ts_set_orphan_root_blk(c->vol, 0, 0);
		for ( i = 0; i < tr->n && er >= E_OK; i++ ) {
			er = ts_btree_insert_t(c->vol, (UINT)t, &tr->it[i].u, tr->it[i].v);
		}
	}
	if ( ts_obj_end(c->vol, (BOOL)( er >= E_OK )) >= E_OK && er >= E_OK ) {
		for ( t = 0; t < CK_TREES; t++ ) {
			if ( c->tree_bad[t] ) mended(c, TSFSCK_K_NODE, NULL, 0, t, c->tree[t].n, 1);
		}
	}
}

/* A link table made again from the records: links to what is not there become external */
LOCAL void fix_links( CK *c )
{
	INT	i, k;

	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];

		if ( !o->ok || o->orphan ) {
			continue;
		}
		if ( o->badlink || ( ( o->flags & TSFSO_F_RELINK ) != 0 && !o->on_orphan ) ) {
			if ( ts_obj_mark_relink(c->vol, &o->u) >= E_OK && ts_obj_relink(c->vol, &o->u) >= E_OK ) {
				mended(c, o->badlink ? TSFSCK_K_LINK : TSFSCK_K_RELINK, &o->u, o->blk, 0, 0, 1);
			}
		}
	}
	/* entries of the orphan tree with nothing to finish, or at the wrong block */
	for ( k = 0; k < c->tree[TSFSBT_ORPHAN].n; k++ ) {
		CKITEM	*r = &c->tree[TSFSBT_ORPHAN].it[k];
		CKOBJ	*o = obj_find(c, &r->u);
		ER	er = E_OK;

		if ( o == NULL || o->orphan || !o->read || ( o->flags & TSFSO_F_ORPHAN ) != 0 ) {
			continue;
		}
		if ( r->v != o->blk ) {
			if ( ts_obj_begin(c->vol) < E_OK ) continue;
			er = ts_btree_update_t(c->vol, TSFSBT_ORPHAN, &r->u, o->blk);
		} else if ( ( o->flags & TSFSO_F_RELINK ) == 0 ) {
			if ( ts_obj_begin(c->vol) < E_OK ) continue;
			er = ts_btree_delete_t(c->vol, TSFSBT_ORPHAN, &r->u);
		} else {
			continue;
		}
		if ( ts_obj_end(c->vol, (BOOL)( er >= E_OK )) >= E_OK && er >= E_OK ) {
			mended(c, TSFSCK_K_ORPHAN, &r->u, r->v, 0, ( r->v != o->blk ) ? 0 : 1, 1);
		}
	}
	/* entries of the garbage list for objects that are not there */
	for ( k = 0; k < c->tree[TSFSBT_GC].n; k++ ) {
		CKITEM	*g = &c->tree[TSFSBT_GC].it[k];
		CKOBJ	*o = obj_find(c, &g->u);
		ER	er;

		if ( o != NULL && !o->orphan ) {
			continue;
		}
		if ( ts_obj_begin(c->vol) < E_OK ) continue;
		er = ts_btree_delete_t(c->vol, TSFSBT_GC, &g->u);
		if ( ts_obj_end(c->vol, (BOOL)( er >= E_OK )) >= E_OK && er >= E_OK ) {
			mended(c, TSFSCK_K_GCLIST, &g->u, g->v, 0, 0, 1);
		}
	}
	/* on a volume nothing else has open, the deleted ones go as at mount */
	if ( ( c->mode & TSFSCK_ALONE ) != 0 ) {
		for ( i = 0; i < c->nobj; i++ ) {
			if ( c->obj[i].orphan && c->obj[i].read && ( c->obj[i].flags & TSFSO_F_ORPHAN ) == 0 ) {
				break;
			}
		}
		if ( i < c->nobj ) {
			INT	n = 0;

			if ( ts_obj_tidy(c->vol, &n) >= E_OK ) {
				mended(c, TSFSCK_K_ORPHAN, NULL, 0, n, 3, 1);
			}
		}
	}
}

/* Each count made what the link tables and holds make it, and the garbage list with it */
LOCAL void fix_counts( CK *c )
{
	INT	i;

	if ( c->legacy ) {
		return;
	}
	for ( i = 0; i < c->nobj; i++ ) {
		CKOBJ	*o = &c->obj[i];
		BOOL	gc_wrong;

		if ( !o->ok || o->orphan || ( o->flags & TSFSO_F_ORPHAN ) != 0 ) {
			continue;
		}
		gc_wrong = (BOOL)( ( o->on_gc && ( o->gc_v != o->blk || ( o->flags & TSFSO_F_GARBAGE ) == 0 || o->refcnt != 0 ) )
				|| ( !o->on_gc && ( o->flags & TSFSO_F_GARBAGE ) != 0 ) );
		if ( o->refcnt == o->want && o->pins <= o->refcnt && !gc_wrong ) {
			continue;
		}
		if ( ts_obj_set_ref(c->vol, &o->u, o->want) >= E_OK ) {
			if ( o->refcnt != o->want || o->pins > o->refcnt ) {
				mended(c, TSFSCK_K_REFCNT, &o->u, o->blk, o->refcnt, o->want, 1);
			} else {
				mended(c, TSFSCK_K_GCLIST, &o->u, o->blk, o->refcnt, 0, 1);
			}
		}
	}
}

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( *s != '\0' && at < max - 1 ) {
		b[at++] = (UB)*s++;
	}
	b[at] = '\0';
	return at;
}

LOCAL INT put_num( UB *b, INT at, INT max, D v )
{
	char	t[24];
	INT	n = 0;

	if ( v < 0 ) {
		at = put(b, at, max, "-");
		v = -v;
	}
	do {
		t[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( n > 0 && at < max - 1 ) {
		b[at++] = (UB)t[--n];
	}
	b[at] = '\0';
	return at;
}

/*
 * TRASH: a cabinet of a link to each of the objects given, made on the
 * volume with a count of nought, so that it is a 屑実身 like any other:
 * moved into a window it becomes an ordinary object again, and deleted
 * it takes its links with it.
 */
LOCAL ER make_trash( CK *c, CONST TS_UUID *list, INT nlist, TS_UUID *p_u )
{
	static CONST char	json[] = "{\"name\":\"" CK_TRASH_NAME "\",\"refCount\":0,\"recordCount\":1,"
					 "\"editable\":true,\"deletable\":true,\"readable\":true}";
	TS_UUID	u, vid;
	char	us[TS_UUID_STRLEN + 1];
	UB	*x;
	INT	i, n = 0, at, pos = 0, max, k;
	ER	er;

	if ( list == NULL ) {
		for ( i = 0; i < c->nobj; i++ ) {
			if ( c->obj[i].hidden ) n++;
		}
	} else {
		n = nlist;
	}
	if ( n > CK_TRASH_MAX ) n = CK_TRASH_MAX;
	max = 256 + n * 400;
	x = (UB *)Kmalloc((SZ)max);
	if ( x == NULL ) {
		return E_NOMEM;
	}
	at = put(x, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"" CK_TRASH_NAME "\">\n<figure>\n");
	for ( i = 0, k = 0, n = 0; n < CK_TRASH_MAX && ( ( list == NULL && i < c->nobj ) || ( list != NULL && k < nlist ) ); ) {
		CONST TS_UUID	*one;

		if ( list == NULL ) {
			if ( !c->obj[i].hidden ) {
				i++;
				continue;
			}
			one = &c->obj[i++].u;
		} else {
			one = &list[k++];
		}
		if ( ts_gen_uuid(&vid) < E_OK ) {
			Kfree(x);
			return E_SYS;
		}
		at = put(x, at, max, "<link id=\"");
		(void)ts_uuid_to_str(one, us, sizeof(us));
		at = put(x, at, max, us);
		at = put(x, at, max, "_0.xtad\" vobjid=\"");
		(void)ts_uuid_to_str(&vid, us, sizeof(us));
		at = put(x, at, max, us);
		at = put(x, at, max, "\" vobjleft=\"8\" vobjtop=\"");
		at = put_num(x, at, max, 8 + n * ( VOBJ_CLOSED_H(14, 1, 1) + 8 ));
		at = put(x, at, max, "\" vobjright=\"248\" vobjbottom=\"");
		at = put_num(x, at, max, 8 + n * ( VOBJ_CLOSED_H(14, 1, 1) + 8 ) + VOBJ_CLOSED_H(14, 1, 1));
		at = put(x, at, max, "\" height=\"");
		at = put_num(x, at, max, VOBJ_CLOSED_H(14, 1, 1));	/* closed, at 14 */
		at = put(x, at, max, "\" chsz=\"14\" frcol=\"#000000\" chcol=\"#000000\""
				     " tbcol=\"#ffffff\" bgcol=\"#ffffff\" pictdisp=\"true\" namedisp=\"true\""
				     " framedisp=\"true\"/>\n");
		n++;
	}
	at = put(x, at, max, "</figure>\n</tad>\n");

	er = ts_obj_create(c->vol, (CONST UB *)json, (INT)( sizeof(json) - 1 ), &u);
	if ( er >= E_OK ) er = ts_obj_ins_rec(c->vol, &u, -1, 1, 0, &pos);
	if ( er >= E_OK ) er = ts_obj_rpl_rec(c->vol, &u, pos, x, (SZ)at);
	if ( er >= E_OK ) er = ts_obj_set_flags(c->vol, &u, TSFSO_F_TRASH, TSFSO_F_TRASH);
	if ( er >= E_OK ) er = ts_obj_relink(c->vol, &u);	/* the links counted */
	Kfree(x);
	if ( er >= E_OK ) {
		*p_u = u;
	}
	return er;
}

LOCAL void fix_hidden( CK *c )
{
	TS_UUID	u;
	INT	i, n = 0;

	for ( i = 0; i < c->nobj; i++ ) {
		if ( c->obj[i].hidden ) n++;
	}
	if ( n == 0 || make_trash(c, NULL, 0, &u) < E_OK ) {
		return;
	}
	c->res->has_trash = TRUE;
	c->res->trash = u;
	for ( i = 0, n = 0; i < c->nobj && n < CK_TRASH_MAX; i++ ) {
		if ( c->obj[i].hidden ) {
			mended(c, TSFSCK_K_HIDDEN, &c->obj[i].u, c->obj[i].blk, 0, 0, 1);
			n++;
		}
	}
}

/* A run of the map put right, in a transaction of its own; `arg` says which kind is done now */
LOCAL void fix_run( CK *c, INT kind, UD start, UD n, void *arg )
{
	UD	got = 0, at = start, left = n;
	ER	er = E_OK;

	if ( kind != *(INT *)arg ) {
		return;
	}
	if ( ts_obj_begin(c->vol) < E_OK ) {
		return;
	}
	if ( kind == TSFSCK_K_LEAK ) {
		er = ts_free_ext(c->vol, start, n);
	} else {
		while ( left > 0 && er >= E_OK ) {
			er = ts_alloc_ext_at(c->vol, at, left, &got);
			if ( er >= E_OK ) {
				at += got;
				left -= got;
			}
		}
	}
	if ( ts_obj_end(c->vol, (BOOL)( er >= E_OK )) >= E_OK && er >= E_OK ) {
		mended(c, kind, NULL, start, (D)n, 0, (INT)n);
	}
}

/*
 * The runs of one kind of the map put right. Blocks in use that the map
 * has free are taken first of all, before any step can allocate them to
 * something else; blocks nothing uses are given back last, once every
 * step has done.
 */
LOCAL void fix_map( CK *c, INT kind )
{
	UB	*buf;

	if ( ( c->mode & TSFSCK_QUICK ) != 0 || c->broken ) {
		return;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf != NULL ) {
		map_runs(c, buf, fix_run, &kind);
		Kfree(buf);
	}
}

/* Blocks nothing uses given back, then what the block layer keeps of itself */
LOCAL void fix_space( CK *c )
{
	UD	m = 0;

	fix_map(c, TSFSCK_K_LEAK);
	if ( c->insp.sb_bad == 0 && c->insp.head_bad == 0 && c->insp.count_bad == 0 ) {
		/* the counts may have moved with the runs: looked at again below */
		(void)ts_blk_inspect(c->vol, &c->insp);
		if ( c->insp.count_bad == 0 ) {
			return;
		}
	}
	if ( ts_blk_mend(c->vol, &m) >= E_OK && m > 0 ) {
		if ( c->insp.sb_bad > 0 ) mended(c, TSFSCK_K_SUPER, NULL, c->insp.sb_first, (D)c->insp.sb_bad, 0, 1);
		if ( c->insp.head_bad > 0 ) mended(c, TSFSCK_K_GROUP, NULL, c->insp.head_first, (D)c->insp.head_bad, 0, 1);
		if ( c->insp.count_bad > 0 ) mended(c, TSFSCK_K_FREECNT, NULL, 0, (D)c->insp.count_bad, 0, 1);
	}
}

/* A look that tells nothing, from which the next step works */
LOCAL ER quiet_pass( CK *c, INT *scratch )
{
	knl_memset(scratch, 0, sizeof(INT) * TSFSCK_NKIND);
	c->tell = FALSE;
	c->count = scratch;
	return pass(c);
}

LOCAL INT sum( CONST INT *k )
{
	INT	i, s = 0;

	for ( i = 1; i < TSFSCK_NKIND; i++ ) {
		s += k[i];
	}
	return s;
}

/*
 * Of the objects a TRASH reached, once it has gone, those that no live
 * object reaches now -- the 隠れ屑実身 it held, and whatever only they
 * reach -- are let go as well. Each one's count is set to nought, so that
 * it goes on the garbage list, and then each is collected; collecting
 * one gives up its links, which takes the others' counts no lower than
 * nought, so a group linking only to one another goes whole. Nothing is
 * done unless the volume can be seen whole.
 */
EXPORT INT ts_obj_drop_cycles( ID bvol, CONST TS_UUID *from, INT nfrom )
{
	CK		*c;
	T_TSFSCK	res;
	INT		scratch[TSFSCK_NKIND];
	INT		*q = NULL, nq = 0, i, k, gone = 0;

	if ( from == NULL || nfrom <= 0 ) {
		return 0;
	}
	c = (CK *)Kmalloc(sizeof(CK));
	if ( c == NULL ) {
		return 0;
	}
	knl_memset(c, 0, sizeof(*c));
	knl_memset(&res, 0, sizeof(res));
	knl_memset(scratch, 0, sizeof(scratch));
	c->vol = bvol;
	c->mode = TSFSCK_QUICK;
	c->res = &res;
	c->count = scratch;
	if ( pass(c) < E_OK || c->broken || c->nobj == 0 ) {
		goto exit;
	}
	q = (INT *)Kmalloc(sizeof(INT) * (SZ)c->nobj);
	if ( q == NULL ) {
		goto exit;
	}
	/* what is left over after check_hidden has 'reached' FALSE: it marks what is chosen now */
	for ( i = 0; i < nfrom; i++ ) {
		CKOBJ	*o = obj_find(c, &from[i]);

		if ( o != NULL && o->hidden && !o->reached ) {
			o->reached = TRUE;
			q[nq++] = (INT)( o - c->obj );
		}
	}
	for ( i = 0; i < nq; i++ ) {
		CKOBJ	*o = &c->obj[q[i]];

		for ( k = o->lk0; k < o->lk0 + o->nlk; k++ ) {
			CKOBJ	*to = obj_find(c, &c->lk[k].target);

			if ( to != NULL && to->hidden && !to->reached ) {
				to->reached = TRUE;
				q[nq++] = (INT)( to - c->obj );
			}
		}
	}
	for ( i = 0; i < nq; i++ ) {
		(void)ts_obj_set_ref(bvol, &c->obj[q[i]].u, 0);
	}
	for ( i = 0; i < nq; i++ ) {
		if ( ts_obj_gc_one(bvol, &c->obj[q[i]].u) >= E_OK ) gone++;
	}

    exit:
	if ( q != NULL ) Kfree(q);
	all_free(c);
	Kfree(c);
	return gone;
}

EXPORT ER ts_obj_check( ID bvol, UINT mode, T_TSFSCK *res, FP_TSFSCK fn, void *ctx )
{
	CK		*c;
	INT		scratch[TSFSCK_NKIND];
	ER		er;

	if ( res == NULL ) {
		return E_PAR;
	}
	knl_memset(res, 0, sizeof(*res));
	res->mode = mode;
	c = (CK *)Kmalloc(sizeof(CK));
	if ( c == NULL ) {
		return E_NOMEM;
	}
	knl_memset(c, 0, sizeof(*c));
	c->vol = bvol;
	c->mode = mode;
	c->fn = fn;
	c->ctx = ctx;
	c->res = res;

	/* a volume opened with an error recorded seen as its log leaves it */
	if ( ( mode & TSFSCK_ALONE ) != 0 ) {
		(void)ts_blk_replay(bvol);
	}

	/* the first look, told */
	c->tell = TRUE;
	c->count = res->found;
	er = pass(c);
	res->problems = sum(res->found);
	res->unfixable = c->unfixable;
	res->objects = (UD)c->nobj;
	res->records = c->records;
	res->links = c->links;
	res->blocks = c->sb.total_blocks;
	res->checked = c->checked;
	knl_memcpy(res->left, res->found, sizeof(res->left));
	res->remaining = res->problems;
	if ( er < E_OK || ( mode & TSFSCK_REPAIR ) == 0 || res->problems == 0 ) {
		goto done;
	}
	if ( c->broken || !knl_jrnl_ready(bvol) ) {
		/* the picture is not whole, or the log was never read: nothing is written */
		res->refused = TRUE;
		goto done;
	}

	(void)ts_blk_writable(bvol, TRUE);
	c->phase = TSFSCK_PH_REPAIR;
	if ( fn != NULL ) send(c, TSFSCK_K_PHASE, NULL, NULL, 0, 0, 0, FALSE);
	if ( c->ndamaged > 0 ) {
		fix_damaged(c);
		if ( quiet_pass(c, scratch) < E_OK || c->broken ) {
			goto last;
		}
	}
	fix_bitmaps(c);
	fix_map(c, TSFSCK_K_LOST);
	fix_trees(c);
	if ( quiet_pass(c, scratch) >= E_OK && !c->broken ) {
		fix_links(c);
	}
	if ( quiet_pass(c, scratch) >= E_OK && !c->broken ) {
		fix_counts(c);
	}
	if ( quiet_pass(c, scratch) >= E_OK && !c->broken ) {
		fix_hidden(c);
	}
	if ( quiet_pass(c, scratch) >= E_OK && !c->broken ) {
		fix_space(c);
	}

    last:
	/* the last look, told as what is left */
	knl_memset(res->left, 0, sizeof(res->left));
	c->tell = TRUE;
	c->verify = TRUE;
	c->count = res->left;
	c->phase = TSFSCK_PH_VERIFY;
	if ( fn != NULL ) send(c, TSFSCK_K_PHASE, NULL, NULL, 0, 0, 0, FALSE);
	c->tell = FALSE;
	er = pass(c);
	c->tell = TRUE;
	{
		INT	k;

		/* what is left told under the phase of the last look */
		c->phase = TSFSCK_PH_VERIFY;
		for ( k = 1; k < TSFSCK_NKIND; k++ ) {
			if ( res->left[k] > 0 ) send(c, k, NULL, NULL, 0, res->left[k], 0, FALSE);
		}
	}
	res->remaining = sum(res->left);
	res->objects = (UD)c->nobj;
	res->records = c->records;
	res->links = c->links;
	if ( er >= E_OK && res->remaining == 0 && !c->broken ) {
		if ( res->found[TSFSCK_K_SUPER] > 0 || c->insp.error ) {
			if ( ts_blk_checked(bvol) >= E_OK && c->insp.error ) {
				mended(c, TSFSCK_K_SUPER, NULL, c->sb.err_blk, c->sb.err_kind, 1, 1);
			}
		} else {
			(void)ts_blk_checked(bvol);
		}
	} else {
		(void)ts_blk_writable(bvol, FALSE);
	}

    done:
	{
		T_TSFSBLK_SB	sb;

		if ( ts_ref_vol_blk(bvol, &sb) >= E_OK ) {
			res->free = sb.free_blocks;
		}
	}
	all_free(c);
	Kfree(c);
	return er;
}
