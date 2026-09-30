/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsfsbtree.h
 *	Object index of a native volume (design 11.6, phase 6)
 *
 *	A B+tree whose key is the UUID of an object and whose value is the
 *	block its header sits in. One node is one block. A version 7 UUID
 *	carries the time it was made in its first bytes, so plain byte order
 *	is creation order and new objects land at the right hand edge of the
 *	tree, where they split few nodes.
 *
 *	The root block is kept in the superblock, so opening a volume does
 *	not walk the tree.
 */

#ifndef __TS_TSFSBTREE_H__
#define __TS_TSFSBTREE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>
#include <ts/tsfsblk.h>

#define TSFSBT_HDR_SIZE	96		/* the common head (64) and the node's own (32) */
#define TSFSBT_ENT_SIZE	24		/* a UUID and a block number */
#define TSFSBT_MAX_KEYS	((TSFSBLK_BLOCK_SIZE - TSFSBT_HDR_SIZE) / TSFSBT_ENT_SIZE)

#define TSFSBT_LEAF	0
#define TSFSBT_NODE	1

/*
 * A volume carries three trees of this shape. They differ only in which
 * field of the superblock holds the root: the object index is every
 * object of the volume, the garbage list is those whose reference count
 * has fallen to zero, the orphan tree those that are to be finished at
 * the next mount -- deleted while open, or with a link table to be made
 * again (design 11.7).
 */
#define TSFSBT_OBJ	0
#define TSFSBT_GC	1
#define TSFSBT_ORPHAN	2

/*
 * Put a key in, take one out, look one up. An insert of a key that is
 * already there answers E_OBJ and changes nothing; use ts_btree_update
 * to point an existing key at another block.
 */
IMPORT ER ts_btree_insert( ID vol, CONST TS_UUID *key, UD value );
IMPORT ER ts_btree_update( ID vol, CONST TS_UUID *key, UD value );
IMPORT ER ts_btree_lookup( ID vol, CONST TS_UUID *key, UD *p_value );
IMPORT ER ts_btree_delete( ID vol, CONST TS_UUID *key );

/*
 * Walk the keys in order. ts_btree_first answers the smallest; passing
 * a key to ts_btree_next answers the one after it, whether or not that
 * key is still there. Both answer E_NOEXS at the end.
 */
IMPORT ER ts_btree_first( ID vol, TS_UUID *p_key, UD *p_value );
IMPORT ER ts_btree_next( ID vol, CONST TS_UUID *after,
			 TS_UUID *p_key, UD *p_value );
IMPORT ER ts_btree_count( ID vol, UD *p_count );

/* Throw the whole index away, giving every node back to the volume */
IMPORT ER ts_btree_drop( ID vol );

/*
 * The same calls on a named tree. The ones above are these with
 * TSFSBT_OBJ.
 */
IMPORT ER ts_btree_insert_t( ID vol, UINT tree, CONST TS_UUID *key, UD value );
IMPORT ER ts_btree_update_t( ID vol, UINT tree, CONST TS_UUID *key, UD value );
IMPORT ER ts_btree_lookup_t( ID vol, UINT tree, CONST TS_UUID *key, UD *p_value );
IMPORT ER ts_btree_delete_t( ID vol, UINT tree, CONST TS_UUID *key );
IMPORT ER ts_btree_first_t( ID vol, UINT tree, TS_UUID *p_key, UD *p_value );
IMPORT ER ts_btree_next_t( ID vol, UINT tree, CONST TS_UUID *after,
			   TS_UUID *p_key, UD *p_value );
IMPORT ER ts_btree_count_t( ID vol, UINT tree, UD *p_count );
IMPORT ER ts_btree_drop_t( ID vol, UINT tree );

/* Every node of a tree, each block handed to `fn` once (the patrol) */
IMPORT ER ts_btree_nodes_t( ID vol, UINT tree, void (*fn)( void *ctx, UD blk ), void *ctx );

/*
 * While this is on, a node written by any of the calls above goes into
 * the open journal transaction instead of straight to its block, so that
 * a change of a tree and the change of the object header it belongs with
 * become real at the same moment.
 */
IMPORT ER ts_btree_jrnl( ID vol, BOOL on );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TSFSBTREE_H__ */
