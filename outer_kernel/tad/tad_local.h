/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad_local.h
 *	What the parts of lib/libtad share and nothing else uses.
 */

#ifndef _TAD_LOCAL_H_
#define _TAD_LOCAL_H_

#include <tk/tkernel.h>
#include <ts/tad.h>

/*
 * One document owns its memory. Everything a parse makes goes into an
 * arena of chunks, so that tad_free gives it all back in one walk and a
 * node never has to be freed on its own. What a removed <link> held is
 * given back when the document is freed, not before.
 */
typedef struct t_tadchunk {
	struct t_tadchunk *next;
	SZ	size;			/* bytes after this header */
	SZ	used;
} T_TADCHUNK;

/* One pending reference count change (design 16.3.4) */
typedef struct t_taddelta {
	struct t_taddelta *next;
	TS_UUID	target;
	INT	delta;			/* +1 for a link added, -1 for one removed */
} T_TADDELTA;

struct t_tad {
	T_TADCHUNK *chunk;		/* the arena, newest first */
	UB	*src;			/* the text as it was parsed */
	SZ	srclen;
	T_TADNODE *head;		/* the first node of the top level */
	T_TADNODE *tail;
	T_TADNODE *root;		/* the <tad> element */
	T_TADLIM lim;
	INT	nnode;
	T_TADDELTA *delta;
	T_TADDELTA *delta_last;
	INT	ndelta;
};

/* Memory out of the arena of a document; it comes back zeroed */
IMPORT void *tad_alloc( T_TAD *doc, SZ size );
IMPORT UB *tad_dup( T_TAD *doc, CONST UB *s, SZ len );

/* Nodes */
IMPORT T_TADNODE *tad_new_node( T_TAD *doc, UINT kind );
IMPORT void tad_add_child( T_TADNODE *parent, T_TADNODE *nd );

/* A change the file system owes (design 16.3.4) */
IMPORT ER tad_put_delta( T_TAD *doc, CONST TS_UUID *target, INT delta );

/* Text helpers shared by the parts of the library */
IMPORT SZ tad_slen( CONST char *s );
IMPORT BOOL tad_same( CONST UB *a, CONST char *b );

#endif /* _TAD_LOCAL_H_ */
