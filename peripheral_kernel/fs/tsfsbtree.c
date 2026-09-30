/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsbtree.c
 *	Object index of a native volume (design 11.6).
 *
 *	A B+tree from the UUID of an object to the block its header sits in.
 *	One node is one block of 4096 bytes, which holds 166 keys, so a
 *	volume of a million objects is three levels deep. A node carries
 *	the common head of a management block (design 11.6.4), with the
 *	tree it belongs to as the owner.
 *
 *	A version 7 UUID begins with the time it was made, so byte order is
 *	creation order: new objects go in at the right hand edge and split
 *	few nodes.
 *
 *	Keys live in the leaves. An internal node holds separators, each
 *	with the child that takes the keys from it upwards, and one more
 *	child on the left for everything below the first separator. The
 *	leaves are chained so that a walk in order does not go back up.
 *
 *	Nodes are not merged when they empty out: an empty leaf is given
 *	back and its separator dropped, but an internal node that thins out
 *	stays as it is. Searching stays correct either way, and a volume
 *	that has lost a great many objects is tidied by rebuilding the
 *	index rather than by rebalancing it in place.
 *
 *	The same code carries two trees of a volume, told apart by a tree
 *	number: the object index and the garbage list. They differ only in
 *	which field of the superblock holds the root.
 *
 *	While a volume is marked as journalling, a node written here goes
 *	into the open transaction instead of straight to its block, so that
 *	a change of the index and the change of the object header it belongs
 *	with become real at the same moment.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/crc32c.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsbtree.h>
#include <ts/tsfsjrnl.h>
#include "tstdlib.h"

/* ---------------------------------------------------------------- layout */
/*
 *	 0	the common head: kind "TFBT", its own block number, the
 *		volume, the tree as owner, the transaction, the CRC
 *	64	type (leaf or node)
 *	65	level (0 at the leaves)
 *	66	number of keys
 *	72	next leaf, 0 at the last one
 *	80	leftmost child (internal nodes)
 *	88	the tree (TSFSBT_OBJ, TSFSBT_GC, TSFSBT_ORPHAN)
 *	96	the keys and their values
 */
#define OFF_TYPE	64
#define OFF_LEVEL	65
#define OFF_NKEYS	66
#define OFF_NEXT	72
#define OFF_CHILD0	80
#define OFF_TREE	88

#define ENT(buf, i)	((buf) + TSFSBT_HDR_SIZE + (i) * TSFSBT_ENT_SIZE)
#define ENT_KEY(buf, i)	(ENT(buf, i))
#define ENT_VAL(buf, i)	(ENT(buf, i) + 16)

/* ---------------------------------------------------------------- bytes */

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL UH rd16( CONST UB *p )
{
	return (UH)((UH)p[0] | ((UH)p[1] << 8));
}

LOCAL UD rd64( CONST UB *p )
{
	return (UD)rd32(p) | ((UD)rd32(p + 4) << 32);
}

LOCAL void wr32( UB *p, UW v )
{
	p[0] = (UB)v; p[1] = (UB)(v >> 8);
	p[2] = (UB)(v >> 16); p[3] = (UB)(v >> 24);
}

LOCAL void wr16( UB *p, UH v )
{
	p[0] = (UB)v; p[1] = (UB)(v >> 8);
}

LOCAL void wr64( UB *p, UD v )
{
	wr32(p, (UW)v);
	wr32(p + 4, (UW)(v >> 32));
}

/* ---------------------------------------------------------------- roots */

/*
 * A volume has three trees of this shape. Which field of the superblock
 * holds the root is the only difference between them.
 */
LOCAL ER root_get( ID vol, UINT tree, UD *p_root, UD *p_nodes )
{
	if ( tree == TSFSBT_GC ) {
		return ts_get_gc_root_blk(vol, p_root, p_nodes);
	}
	if ( tree == TSFSBT_ORPHAN ) {
		return ts_get_orphan_root_blk(vol, p_root, p_nodes);
	}

	return ts_get_root_blk(vol, p_root, p_nodes);
}

LOCAL ER root_set( ID vol, UINT tree, UD root, UD nodes )
{
	if ( tree == TSFSBT_GC ) {
		return ts_set_gc_root_blk(vol, root, nodes);
	}
	if ( tree == TSFSBT_ORPHAN ) {
		return ts_set_orphan_root_blk(vol, root, nodes);
	}

	return ts_set_root_blk(vol, root, nodes);
}

/* ---------------------------------------------------------------- nodes */

/*
 * Volumes whose node writes go into the open journal transaction. A node
 * is read before it is written within one operation, so reading past the
 * log while a transaction is open still gives the right block.
 */
LOCAL BOOL	btree_jrnl[TSFSBLK_MAX_VOL];

EXPORT ER ts_btree_jrnl( ID vol, BOOL on )
{
	if ( vol < 1 || vol > TSFSBLK_MAX_VOL ) {
		return E_ID;
	}
	btree_jrnl[vol - 1] = on;

	return E_OK;
}

LOCAL BOOL jrnl_is_on( ID vol )
{
	return ( vol >= 1 && vol <= TSFSBLK_MAX_VOL ) ? btree_jrnl[vol - 1] : FALSE;
}

LOCAL ER node_read( ID vol, UD blk, UB *buf )
{
	ER	er = ( jrnl_is_on(vol) ) ? ts_jrnl_read(vol, blk, buf)
				     : ts_read_blk(vol, blk, buf);

	if ( er < E_OK ) {
		return er;
	}
	if ( ts_blk_check(vol, buf, TSFSBLK_MAGIC_NODE, blk) < E_OK ) {
		return E_IO;			/* damaged, or not a node of this volume */
	}
	if ( rd16(buf + OFF_NKEYS) > TSFSBT_MAX_KEYS ) {
		ts_blk_error(vol, blk, TSFSBLK_ERR_TREE);
		return E_OBJ;
	}

	return E_OK;
}

LOCAL ER node_write( ID vol, UD blk, UB *buf )
{
	ts_blk_seal(vol, buf, TSFSBLK_MAGIC_NODE, blk, rd32(buf + OFF_TREE));

	if ( jrnl_is_on(vol) ) {
		return ts_jrnl_write(vol, blk, buf);
	}

	return ts_write_blk(vol, blk, buf);
}

LOCAL void node_init( UB *buf, UINT type, UINT level, UINT tree )
{
	knl_memset(buf, 0, TSFSBLK_BLOCK_SIZE);
	wr32(buf + OFF_TREE, tree);
	buf[OFF_TYPE]  = (UB)type;
	buf[OFF_LEVEL] = (UB)level;
	wr16(buf + OFF_NKEYS, 0);
}

LOCAL INT node_nkeys( CONST UB *buf )
{
	return (INT)rd16(buf + OFF_NKEYS);
}

LOCAL BOOL node_is_leaf( CONST UB *buf )
{
	return ( buf[OFF_TYPE] == TSFSBT_LEAF );
}

LOCAL void key_get( CONST UB *buf, INT i, TS_UUID *p_key )
{
	knl_memcpy(p_key->b, ENT_KEY(buf, i), sizeof(p_key->b));
}

LOCAL void key_put( UB *buf, INT i, CONST TS_UUID *key )
{
	knl_memcpy(ENT_KEY(buf, i), key->b, sizeof(key->b));
}

LOCAL INT key_cmp_at( CONST UB *buf, INT i, CONST TS_UUID *key )
{
	TS_UUID	k;

	key_get(buf, i, &k);

	return ts_uuid_cmp(&k, key);
}

/*
 * The first entry whose key is not smaller than the one looked for. In a
 * leaf that is where the key is or would go; in an internal node it is
 * one past the child that takes it.
 */
LOCAL INT node_search( CONST UB *buf, CONST TS_UUID *key, BOOL *p_exact )
{
	INT	lo = 0, hi = node_nkeys(buf), mid, c;

	*p_exact = FALSE;
	while ( lo < hi ) {
		mid = (lo + hi) / 2;
		c = key_cmp_at(buf, mid, key);
		if ( c == 0 ) {
			*p_exact = TRUE;
			return mid;
		}
		if ( c < 0 ) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}

	return lo;
}

/* The child of an internal node that a key belongs to */
LOCAL UD node_child( CONST UB *buf, CONST TS_UUID *key )
{
	BOOL	exact;
	INT	i = node_search(buf, key, &exact);

	if ( exact ) {
		i++;			/* the separator itself belongs right */
	}
	if ( i == 0 ) {
		return rd64(buf + OFF_CHILD0);
	}

	return rd64(ENT_VAL(buf, i - 1));
}

/*
 * Make room at `at` and put one entry there. The entries above it move
 * up, which overlaps, so they are copied from the top down rather than
 * with a plain block move.
 */
LOCAL void ent_insert( UB *buf, INT at, CONST TS_UUID *key, UD value )
{
	INT	n = node_nkeys(buf);
	INT	i;

	for ( i = n; i > at; i-- ) {
		knl_memcpy(ENT(buf, i), ENT(buf, i - 1), TSFSBT_ENT_SIZE);
	}
	key_put(buf, at, key);
	wr64(ENT_VAL(buf, at), value);
	wr16(buf + OFF_NKEYS, (UH)(n + 1));
}

LOCAL void ent_remove( UB *buf, INT at )
{
	INT	n = node_nkeys(buf);
	INT	i;

	for ( i = at; i < n - 1; i++ ) {
		knl_memcpy(ENT(buf, i), ENT(buf, i + 1), TSFSBT_ENT_SIZE);
	}
	wr16(buf + OFF_NKEYS, (UH)(n - 1));
}

/* ---------------------------------------------------------------- the tree */

/*
 * What an insert carries back up: whether the child split, and if so the
 * key that separates the two halves and the block of the right one.
 */
typedef struct {
	BOOL	split;
	TS_UUID	key;
	UD	right;
	UD	nodes_added;		/* blocks the index grew by */
} SPLIT;

LOCAL ER insert_rec( ID vol, UD blk, CONST TS_UUID *key, UD value, SPLIT *sp );

/*
 * Split a full leaf in two and put the new key in whichever half it
 * belongs to. The first key of the right half becomes the separator; it
 * stays in the leaf as well, because the leaves hold every key.
 */
LOCAL ER leaf_split( ID vol, UD blk, UB *buf, INT at,
		     CONST TS_UUID *key, UD value, SPLIT *sp )
{
	UB	*rbuf;
	UD	rblk = 0, count = 0;
	INT	n = node_nkeys(buf), half = n / 2;
	ER	er;

	er = ts_alloc_ext(vol, 1, &rblk, &count);
	if ( er < E_OK ) {
		return er;
	}
	rbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( rbuf == NULL ) {
		ts_free_ext(vol, rblk, 1);
		return E_NOMEM;
	}
	node_init(rbuf, TSFSBT_LEAF, 0, rd32(buf + OFF_TREE));

	knl_memcpy(ENT(rbuf, 0), ENT(buf, half), (SZ)(n - half) * TSFSBT_ENT_SIZE);
	wr16(rbuf + OFF_NKEYS, (UH)(n - half));
	wr16(buf  + OFF_NKEYS, (UH)half);

	/* the new leaf takes the old one's place in the chain */
	wr64(rbuf + OFF_NEXT, rd64(buf + OFF_NEXT));
	wr64(buf  + OFF_NEXT, rblk);

	if ( at <= half ) {
		ent_insert(buf, at, key, value);
	} else {
		ent_insert(rbuf, at - half, key, value);
	}

	er = node_write(vol, rblk, rbuf);
	if ( er >= E_OK ) {
		er = node_write(vol, blk, buf);
	}
	if ( er >= E_OK ) {
		sp->split = TRUE;
		key_get(rbuf, 0, &sp->key);
		sp->right = rblk;
		sp->nodes_added++;
	} else {
		ts_free_ext(vol, rblk, 1);
	}
	Kfree(rbuf);

	return er;
}

/*
 * Split a full internal node. The middle separator moves up rather than
 * being copied, and its child becomes the left hand child of the new
 * node.
 */
LOCAL ER node_split( ID vol, UD blk, UB *buf, SPLIT *sp )
{
	UB	*rbuf;
	UD	rblk = 0, count = 0;
	INT	n = node_nkeys(buf), half = n / 2;
	ER	er;

	er = ts_alloc_ext(vol, 1, &rblk, &count);
	if ( er < E_OK ) {
		return er;
	}
	rbuf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( rbuf == NULL ) {
		ts_free_ext(vol, rblk, 1);
		return E_NOMEM;
	}
	node_init(rbuf, TSFSBT_NODE, buf[OFF_LEVEL], rd32(buf + OFF_TREE));

	key_get(buf, half, &sp->key);
	wr64(rbuf + OFF_CHILD0, rd64(ENT_VAL(buf, half)));

	knl_memcpy(ENT(rbuf, 0), ENT(buf, half + 1),
		   (SZ)(n - half - 1) * TSFSBT_ENT_SIZE);
	wr16(rbuf + OFF_NKEYS, (UH)(n - half - 1));
	wr16(buf  + OFF_NKEYS, (UH)half);

	er = node_write(vol, rblk, rbuf);
	if ( er >= E_OK ) {
		er = node_write(vol, blk, buf);
	}
	if ( er >= E_OK ) {
		sp->split = TRUE;
		sp->right = rblk;
		sp->nodes_added++;
	} else {
		ts_free_ext(vol, rblk, 1);
	}
	Kfree(rbuf);

	return er;
}

LOCAL ER insert_rec( ID vol, UD blk, CONST TS_UUID *key, UD value, SPLIT *sp )
{
	UB	*buf;
	SPLIT	child;
	BOOL	exact;
	INT	at;
	UD	next;
	ER	er;

	/* One entry of slack: a full internal node takes the key
	   coming up from below before it is split, and that is one
	   more than the block itself holds. */
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE + TSFSBT_ENT_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = node_read(vol, blk, buf);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}

	if ( node_is_leaf(buf) ) {
		at = node_search(buf, key, &exact);
		if ( exact ) {
			er = E_OBJ;		/* it is already in the index */
		} else if ( node_nkeys(buf) < (INT)TSFSBT_MAX_KEYS ) {
			ent_insert(buf, at, key, value);
			er = node_write(vol, blk, buf);
		} else {
			er = leaf_split(vol, blk, buf, at, key, value, sp);
		}
		Kfree(buf);
		return er;
	}

	/* an internal node: go down, then take in what comes back */
	next = node_child(buf, key);
	child.split = FALSE;
	child.nodes_added = 0;
	er = insert_rec(vol, next, key, value, &child);
	sp->nodes_added += child.nodes_added;
	if ( er < E_OK || !child.split ) {
		Kfree(buf);
		return er;
	}

	at = node_search(buf, &child.key, &exact);
	if ( node_nkeys(buf) < (INT)TSFSBT_MAX_KEYS ) {
		ent_insert(buf, at, &child.key, child.right);
		er = node_write(vol, blk, buf);
	} else {
		/* it has to go in before this node can be split */
		TS_UUID	promoted = child.key;
		UD	promoted_blk = child.right;

		ent_insert(buf, at, &promoted, promoted_blk);
		er = node_split(vol, blk, buf, sp);
	}
	Kfree(buf);

	return er;
}

EXPORT ER ts_btree_insert_t( ID vol, UINT tree, CONST TS_UUID *key, UD value )
{
	SPLIT	sp;
	UB	*buf;
	UD	root = 0, nodes = 0, blk = 0, count = 0;
	ER	er;

	if ( key == NULL || value == 0 ) {
		return E_PAR;
	}
	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK ) {
		return er;
	}

	if ( root == 0 ) {
		/* the first key makes the first leaf */
		er = ts_alloc_ext(vol, 1, &blk, &count);
		if ( er < E_OK ) {
			return er;
		}
		buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( buf == NULL ) {
			ts_free_ext(vol, blk, 1);
			return E_NOMEM;
		}
		node_init(buf, TSFSBT_LEAF, 0, tree);
		ent_insert(buf, 0, key, value);
		er = node_write(vol, blk, buf);
		Kfree(buf);
		if ( er < E_OK ) {
			ts_free_ext(vol, blk, 1);
			return er;
		}
		return root_set(vol, tree, blk, nodes + 1);
	}

	sp.split = FALSE;
	sp.nodes_added = 0;
	er = insert_rec(vol, root, key, value, &sp);
	if ( er < E_OK ) {
		if ( sp.nodes_added > 0 ) {
			root_set(vol, tree, root, nodes + sp.nodes_added);
		}
		return er;
	}

	if ( sp.split ) {
		/* the tree grew a level */
		er = ts_alloc_ext(vol, 1, &blk, &count);
		if ( er < E_OK ) {
			return er;
		}
		buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
		if ( buf == NULL ) {
			ts_free_ext(vol, blk, 1);
			return E_NOMEM;
		}
		/* one level above the root it stands over */
		er = node_read(vol, root, buf);
		if ( er < E_OK ) {
			Kfree(buf);
			ts_free_ext(vol, blk, 1);
			return er;
		}
		node_init(buf, TSFSBT_NODE, (UINT)buf[OFF_LEVEL] + 1, tree);
		wr64(buf + OFF_CHILD0, root);
		ent_insert(buf, 0, &sp.key, sp.right);
		er = node_write(vol, blk, buf);
		Kfree(buf);
		if ( er < E_OK ) {
			ts_free_ext(vol, blk, 1);
			return er;
		}
		root = blk;
		sp.nodes_added++;
	}

	return root_set(vol, tree, root, nodes + sp.nodes_added);
}

/*
 * Find the leaf a key belongs in, following the separators down.
 */
LOCAL ER leaf_find( ID vol, UINT tree, CONST TS_UUID *key, UB *buf, UD *p_blk )
{
	UD	root = 0, nodes = 0, blk;
	INT	depth;
	ER	er;

	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK ) {
		return er;
	}
	if ( root == 0 ) {
		return E_NOEXS;			/* the index is empty */
	}
	blk = root;
	for ( depth = 0; depth < 32; depth++ ) {
		er = node_read(vol, blk, buf);
		if ( er < E_OK ) {
			return er;
		}
		if ( node_is_leaf(buf) ) {
			*p_blk = blk;
			return E_OK;
		}
		blk = node_child(buf, key);
		if ( blk == 0 ) {
			return E_OBJ;		/* a separator points nowhere */
		}
	}

	return E_OBJ;				/* deeper than any real tree */
}

EXPORT ER ts_btree_lookup_t( ID vol, UINT tree, CONST TS_UUID *key, UD *p_value )
{
	UB	*buf;
	UD	blk = 0;
	BOOL	exact;
	INT	at;
	ER	er;

	if ( key == NULL || p_value == NULL ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = leaf_find(vol, tree, key, buf, &blk);
	if ( er >= E_OK ) {
		at = node_search(buf, key, &exact);
		if ( exact ) {
			*p_value = rd64(ENT_VAL(buf, at));
		} else {
			er = E_NOEXS;
		}
	}
	Kfree(buf);

	return er;
}

EXPORT ER ts_btree_update_t( ID vol, UINT tree, CONST TS_UUID *key, UD value )
{
	UB	*buf;
	UD	blk = 0;
	BOOL	exact;
	INT	at;
	ER	er;

	if ( key == NULL || value == 0 ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = leaf_find(vol, tree, key, buf, &blk);
	if ( er >= E_OK ) {
		at = node_search(buf, key, &exact);
		if ( exact ) {
			wr64(ENT_VAL(buf, at), value);
			er = node_write(vol, blk, buf);
		} else {
			er = E_NOEXS;
		}
	}
	Kfree(buf);

	return er;
}

/*
 * Take the separator that points at `child` out of an internal node, so
 * that an empty leaf can be given back. The leftmost child is not named
 * by a separator, so the first one takes its place.
 */
LOCAL ER parent_drop( ID vol, UD parent, UD child )
{
	UB	*buf;
	INT	i, n;
	ER	er;

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = node_read(vol, parent, buf);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	n = node_nkeys(buf);

	if ( rd64(buf + OFF_CHILD0) == child ) {
		if ( n == 0 ) {
			Kfree(buf);
			return E_OBJ;		/* nothing would be left */
		}
		wr64(buf + OFF_CHILD0, rd64(ENT_VAL(buf, 0)));
		ent_remove(buf, 0);
	} else {
		for ( i = 0; i < n; i++ ) {
			if ( rd64(ENT_VAL(buf, i)) == child ) {
				break;
			}
		}
		if ( i == n ) {
			Kfree(buf);
			return E_NOEXS;
		}
		ent_remove(buf, i);
	}
	er = node_write(vol, parent, buf);
	Kfree(buf);

	return er;
}

/*
 * Follow the chain of leaves to the one before `blk`, so that its next
 * pointer can be made to skip a leaf that is going away.
 */
LOCAL ER leaf_unchain( ID vol, UD first_leaf, UD blk, UD next )
{
	UB	*buf;
	UD	cur = first_leaf;
	ER	er = E_NOEXS;

	if ( first_leaf == blk ) {
		return E_OK;			/* it is the first one */
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	while ( cur != 0 ) {
		er = node_read(vol, cur, buf);
		if ( er < E_OK ) {
			break;
		}
		if ( rd64(buf + OFF_NEXT) == blk ) {
			wr64(buf + OFF_NEXT, next);
			er = node_write(vol, cur, buf);
			break;
		}
		cur = rd64(buf + OFF_NEXT);
		er = E_NOEXS;
	}
	Kfree(buf);

	return er;
}

/* The leftmost leaf, which is where a walk in order starts */
LOCAL ER leaf_leftmost( ID vol, UINT tree, UB *buf, UD *p_blk )
{
	UD	root = 0, nodes = 0, blk;
	INT	depth;
	ER	er;

	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK ) {
		return er;
	}
	if ( root == 0 ) {
		return E_NOEXS;
	}
	blk = root;
	for ( depth = 0; depth < 32; depth++ ) {
		er = node_read(vol, blk, buf);
		if ( er < E_OK ) {
			return er;
		}
		if ( node_is_leaf(buf) ) {
			*p_blk = blk;
			return E_OK;
		}
		blk = rd64(buf + OFF_CHILD0);
		if ( blk == 0 ) {
			return E_OBJ;
		}
	}

	return E_OBJ;
}

/* The node one level above the leaf a key belongs to, 0 at the root */
LOCAL ER leaf_parent( ID vol, UINT tree, CONST TS_UUID *key, UD *p_parent )
{
	UB	*buf;
	UD	root = 0, nodes = 0, blk, parent = 0;
	INT	depth;
	ER	er;

	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK ) {
		return er;
	}
	if ( root == 0 ) {
		return E_NOEXS;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	blk = root;
	er = E_OBJ;
	for ( depth = 0; depth < 32; depth++ ) {
		if ( node_read(vol, blk, buf) < E_OK ) {
			er = E_IO;
			break;
		}
		if ( node_is_leaf(buf) ) {
			*p_parent = parent;
			er = E_OK;
			break;
		}
		parent = blk;
		blk = node_child(buf, key);
		if ( blk == 0 ) {
			break;
		}
	}
	Kfree(buf);

	return er;
}

EXPORT ER ts_btree_delete_t( ID vol, UINT tree, CONST TS_UUID *key )
{
	UB	*buf;
	UD	blk = 0, root = 0, nodes = 0, parent = 0, next, first = 0;
	BOOL	exact;
	INT	at, left;
	ER	er;

	if ( key == NULL ) {
		return E_PAR;
	}
	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK ) {
		return er;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = leaf_find(vol, tree, key, buf, &blk);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	at = node_search(buf, key, &exact);
	if ( !exact ) {
		Kfree(buf);
		return E_NOEXS;
	}
	ent_remove(buf, at);
	next = rd64(buf + OFF_NEXT);
	left = node_nkeys(buf);
	er = node_write(vol, blk, buf);
	Kfree(buf);
	if ( er < E_OK ) {
		return er;
	}

	if ( left > 0 ) {
		return E_OK;			/* the leaf still holds keys */
	}

	/* an empty leaf goes back to the volume */
	if ( blk == root ) {
		ts_free_ext(vol, blk, 1);
		return root_set(vol, tree, 0, 0);
	}

	if ( leaf_parent(vol, tree, key, &parent) < E_OK || parent == 0 ) {
		return E_OK;			/* leave it in place */
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf != NULL ) {
		if ( leaf_leftmost(vol, tree, buf, &first) >= E_OK ) {
			leaf_unchain(vol, first, blk, next);
		}
		Kfree(buf);
	}
	if ( parent_drop(vol, parent, blk) >= E_OK ) {
		ts_free_ext(vol, blk, 1);
		root_set(vol, tree, root, ( nodes > 0 ) ? nodes - 1 : 0);
	}

	return E_OK;
}

/* ---------------------------------------------------------------- walking */

EXPORT ER ts_btree_first_t( ID vol, UINT tree, TS_UUID *p_key, UD *p_value )
{
	UB	*buf;
	UD	blk = 0;
	ER	er;

	if ( p_key == NULL || p_value == NULL ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = leaf_leftmost(vol, tree, buf, &blk);
	while ( er >= E_OK && node_nkeys(buf) == 0 ) {
		blk = rd64(buf + OFF_NEXT);	/* an emptied leaf on the way */
		if ( blk == 0 ) {
			er = E_NOEXS;
			break;
		}
		er = node_read(vol, blk, buf);
	}
	if ( er >= E_OK ) {
		key_get(buf, 0, p_key);
		*p_value = rd64(ENT_VAL(buf, 0));
	}
	Kfree(buf);

	return er;
}

EXPORT ER ts_btree_next_t( ID vol, UINT tree, CONST TS_UUID *after,
			   TS_UUID *p_key, UD *p_value )
{
	UB	*buf;
	UD	blk = 0;
	BOOL	exact;
	INT	at;
	ER	er;

	if ( after == NULL || p_key == NULL || p_value == NULL ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = leaf_find(vol, tree, after, buf, &blk);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	at = node_search(buf, after, &exact);
	if ( exact ) {
		at++;				/* the one after it */
	}
	while ( at >= node_nkeys(buf) ) {
		blk = rd64(buf + OFF_NEXT);
		if ( blk == 0 ) {
			Kfree(buf);
			return E_NOEXS;		/* that was the last key */
		}
		er = node_read(vol, blk, buf);
		if ( er < E_OK ) {
			Kfree(buf);
			return er;
		}
		at = 0;
	}
	key_get(buf, at, p_key);
	*p_value = rd64(ENT_VAL(buf, at));
	Kfree(buf);

	return E_OK;
}

EXPORT ER ts_btree_count_t( ID vol, UINT tree, UD *p_count )
{
	UB	*buf;
	UD	blk = 0, n = 0;
	INT	guard;
	ER	er;

	if ( p_count == NULL ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = leaf_leftmost(vol, tree, buf, &blk);
	if ( er == E_NOEXS ) {
		Kfree(buf);
		*p_count = 0;
		return E_OK;			/* an empty index has no keys */
	}
	for ( guard = 0; er >= E_OK && guard < 1000000; guard++ ) {
		n += (UD)node_nkeys(buf);
		blk = rd64(buf + OFF_NEXT);
		if ( blk == 0 ) {
			break;
		}
		er = node_read(vol, blk, buf);
	}
	Kfree(buf);
	if ( er >= E_OK ) {
		*p_count = n;
	}

	return er;
}

LOCAL ER nodes_rec( ID vol, UD blk, void (*fn)( void *ctx, UD blk ), void *ctx, INT depth )
{
	UB	*buf;
	INT	i, n;
	ER	er;

	if ( depth > 16 ) {
		ts_blk_error(vol, blk, TSFSBLK_ERR_TREE);
		return E_OBJ;			/* a loop, not a tree */
	}
	fn(ctx, blk);
	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = node_read(vol, blk, buf);
	if ( er >= E_OK && !node_is_leaf(buf) ) {
		n = node_nkeys(buf);
		er = nodes_rec(vol, rd64(buf + OFF_CHILD0), fn, ctx, depth + 1);
		for ( i = 0; i < n && er >= E_OK; i++ ) {
			er = nodes_rec(vol, rd64(ENT_VAL(buf, i)), fn, ctx, depth + 1);
		}
	}
	Kfree(buf);
	return er;
}

EXPORT ER ts_btree_nodes_t( ID vol, UINT tree, void (*fn)( void *ctx, UD blk ), void *ctx )
{
	UD	root = 0, nodes = 0;
	ER	er;

	if ( fn == NULL ) {
		return E_PAR;
	}
	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK || root == 0 ) {
		return er;
	}
	return nodes_rec(vol, root, fn, ctx, 0);
}

/* ---------------------------------------------------------------- dropping */

LOCAL ER drop_rec( ID vol, UD blk, UD *p_freed )
{
	UB	*buf;
	INT	i, n;
	ER	er;

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = node_read(vol, blk, buf);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	if ( !node_is_leaf(buf) ) {
		n = node_nkeys(buf);
		drop_rec(vol, rd64(buf + OFF_CHILD0), p_freed);
		for ( i = 0; i < n; i++ ) {
			drop_rec(vol, rd64(ENT_VAL(buf, i)), p_freed);
		}
	}
	Kfree(buf);

	er = ts_free_ext(vol, blk, 1);
	if ( er >= E_OK ) {
		(*p_freed)++;
	}

	return er;
}

EXPORT ER ts_btree_drop_t( ID vol, UINT tree )
{
	UD	root = 0, nodes = 0, freed = 0;
	ER	er;

	er = root_get(vol, tree, &root, &nodes);
	if ( er < E_OK ) {
		return er;
	}
	if ( root == 0 ) {
		return E_OK;
	}
	er = drop_rec(vol, root, &freed);
	root_set(vol, tree, 0, 0);

	return er;
}

/* ---------------------------------------------------------------- index */

/*
 * The object index is the tree almost every caller means, so it has
 * calls of its own that name no tree.
 */
EXPORT ER ts_btree_insert( ID vol, CONST TS_UUID *key, UD value )
{
	return ts_btree_insert_t(vol, TSFSBT_OBJ, key, value);
}

EXPORT ER ts_btree_update( ID vol, CONST TS_UUID *key, UD value )
{
	return ts_btree_update_t(vol, TSFSBT_OBJ, key, value);
}

EXPORT ER ts_btree_lookup( ID vol, CONST TS_UUID *key, UD *p_value )
{
	return ts_btree_lookup_t(vol, TSFSBT_OBJ, key, p_value);
}

EXPORT ER ts_btree_delete( ID vol, CONST TS_UUID *key )
{
	return ts_btree_delete_t(vol, TSFSBT_OBJ, key);
}

EXPORT ER ts_btree_first( ID vol, TS_UUID *p_key, UD *p_value )
{
	return ts_btree_first_t(vol, TSFSBT_OBJ, p_key, p_value);
}

EXPORT ER ts_btree_next( ID vol, CONST TS_UUID *after,
			 TS_UUID *p_key, UD *p_value )
{
	return ts_btree_next_t(vol, TSFSBT_OBJ, after, p_key, p_value);
}

EXPORT ER ts_btree_count( ID vol, UD *p_count )
{
	return ts_btree_count_t(vol, TSFSBT_OBJ, p_count);
}

EXPORT ER ts_btree_drop( ID vol )
{
	return ts_btree_drop_t(vol, TSFSBT_OBJ);
}
