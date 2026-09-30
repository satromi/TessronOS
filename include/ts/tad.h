/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad.h
 *	xmlTAD: the XML form of TAD (design 16.3, 16.4.1)
 *
 *	The parser lives in a user side library, not in the kernel: the
 *	file system does not interpret the content of a real object, so
 *	whoever edits a document is the one who reads it (design 11.7,
 *	16.3.3).
 *
 *	A document is parsed into a tree of nodes. Every node that came
 *	out of the text keeps the bytes it was parsed from, so writing a
 *	document that nothing has changed gives back the same bytes.
 *
 *	The <link> elements of the tree are the virtual objects: each one
 *	refers to another real object by UUID and carries how it is to be
 *	drawn (design 16.3.2). Adding and removing them is recorded as a
 *	list of pending reference count changes, which lib/libom applies
 *	with ts_lnk_obj/ts_unl_obj in the transaction that writes the
 *	document (design 16.3.4).
 */

#ifndef __TS_TAD_H__
#define __TS_TAD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

/*
 * Limits of one parse. A real object may have been written by someone
 * else, so nothing about a document is believed until it has been
 * counted. A zero field takes the default beside it.
 */
#define TAD_DEF_DEPTH	32		/* nesting of elements */
#define TAD_DEF_NODES	4096		/* nodes of one document */
#define TAD_DEF_ATTRS	64		/* attributes of one element */
#define TAD_DEF_ATTRLEN	4096		/* bytes of one attribute value */
#define TAD_DEF_DOCSIZE	(256 * 1024)	/* bytes of the document text */

typedef struct {
	INT	depth;
	INT	nodes;
	INT	attrs;
	SZ	attrlen;
	SZ	docsize;
} T_TADLIM;

/* What a node is */
#define TAD_ND_ELEM	0		/* <name ...> ... </name> or <name .../> */
#define TAD_ND_TEXT	1		/* the text between two tags */
#define TAD_ND_COMMENT	2		/* <!-- ... -->, kept as it was */
#define TAD_ND_DECL	3		/* <?xml ...?>, kept as it was */

typedef struct {
	UB	*name;			/* attribute name, NUL terminated */
	UB	*value;			/* its value, entity references resolved */
} T_TADATTR;

/*
 * One node. `raw` is the text the node was parsed from -- the start tag
 * of an element, the whole of anything else -- and is NULL for a node
 * that was built rather than parsed. The end tag of an element is not
 * kept, because the accepted form of one is fixed (see below).
 */
typedef struct t_tadnode {
	UINT	kind;			/* TAD_ND_* */
	UB	*name;			/* element name, NULL otherwise */
	UB	*text;			/* text with references resolved, else NULL */
	T_TADATTR *attr;
	INT	nattr;
	BOOL	empty;			/* the element was written <name/> */
	CONST UB *raw;
	SZ	rawlen;
	struct t_tadnode *parent;
	struct t_tadnode *first;	/* first child */
	struct t_tadnode *last;
	struct t_tadnode *next;		/* next sibling */
} T_TADNODE;

typedef struct t_tad T_TAD;		/* one parsed document */

/*
 * A virtual object: what a <link> element says (design 16.3.2, xmlTAD
 * specification 5.2). Lengths are the bytes kept here, not a limit of
 * the format; an attribute longer than its field is refused rather than
 * cut short, so that nothing is lost without being said.
 */
#define TAD_NAME_MAX	192		/* the displayed name */
#define TAD_REL_MAX	64		/* the relationship of this link */
#define TAD_APPL_MAX	512		/* the applist of this link, held opaque */
#define TAD_VIEW_MAX	16		/* viewMode */
#define TAD_WRAP_NONE	0		/* wordWrap not said */
#define TAD_WRAP_ON	1
#define TAD_WRAP_OFF	2

/* What the frame of a virtual object shows */
#define TAD_D_NAME	0x0001
#define TAD_D_ROLE	0x0002		/* the relationship */
#define TAD_D_TYPE	0x0004
#define TAD_D_UPDATE	0x0008
#define TAD_D_FRAME	0x0010
#define TAD_D_PICT	0x0020		/* the icon */
#define TAD_D_DEFAULT	(TAD_D_NAME | TAD_D_FRAME | TAD_D_PICT)

/* A colour that was not given */
#define TAD_COL_NONE	0xFFFFFFFFU

typedef struct {
	TS_UUID	vobjid;			/* identity of the link itself */
	TS_UUID	target;			/* the real object it refers to */
	INT	recno;			/* the record of it the id names */
	UB	name[TAD_NAME_MAX];
	UB	relationship[TAD_REL_MAX];
	UB	applist[TAD_APPL_MAX];
	INT	left, top, right, bottom, height;	/* a figure holds these */
	INT	width, heightpx;			/* a document holds these */
	UW	frcol, chcol, tbcol, bgcol;		/* 0x00rrggbb, or TAD_COL_NONE */
	INT	chsz;
	INT	dlen;
	UINT	disp;			/* TAD_D_* */
	BOOL	autoopen;
	BOOL	fixed;			/* protected: not carried or thrown away */
	BOOL	background;		/* protected, and kept behind the rest */
	BOOL	hidden;
	INT	scrollx, scrolly;
	INT	zoom;			/* per cent: 100 is life size */

	/*
	 * How a window opened from it shows the object: viewMode
	 * ("formatted", "detailed" or "xml" for a document, "canvas" or
	 * "xml" for a figure; empty when it does not say) and wordWrap
	 * (TAD_WRAP_*).
	 */
	UB	viewmode[TAD_VIEW_MAX];
	INT	wordwrap;
	INT	vobjheight;		/* the height a binary TAD gave it, kept as it is; 0 none */
	BOOL	has_zindex;		/* zIndex: its place among the shapes of a figure */
	INT	zindex;
} T_VOBJ;

/*
 * Reading and writing a document.
 *
 * tad_parse copies the text it is given, so the caller may free its own
 * buffer at once. Malformed input gives E_PAR and a document past one of
 * the limits gives E_LIMIT; neither leaves anything allocated.
 *
 * tad_write_mem with a NULL buffer only measures; a buffer too small to
 * hold the document gives E_NOMEM with the size wanted in *p_asize.
 * tad_write puts the document in a record of an object opened with the
 * object layer's key, and cuts the record to its length; it does not
 * touch any reference count, which is what om_wri_doc is for.
 */
IMPORT ER tad_parse( CONST UB *xml, SZ size, CONST T_TADLIM *lim, T_TAD **p_doc );
IMPORT void tad_free( T_TAD *doc );
IMPORT ER tad_write_mem( CONST T_TAD *doc, UB *buf, SZ size, SZ *p_asize );
IMPORT ER tad_write( ID key, INT recno, CONST T_TAD *doc );

/* Walking the tree */
IMPORT T_TADNODE *tad_root( CONST T_TAD *doc );		/* the <tad> element */
IMPORT T_TADNODE *tad_body( CONST T_TAD *doc );		/* <document> or <figure> */
IMPORT T_TADNODE *tad_walk( CONST T_TAD *doc, CONST T_TADNODE *cur );
IMPORT CONST UB *tad_attr( CONST T_TADNODE *nd, CONST char *name );
/*
 * An attribute of an element set, added when it is not there. The
 * element is written from its attributes afterwards, not from the text
 * it was read from.
 */
IMPORT ER tad_set_attr( T_TAD *doc, T_TADNODE *nd, CONST char *name,
			CONST UB *value );

/*
 * The virtual objects of the document. tad_lnk_add makes a vobjid when
 * the one it is given is all zero, and puts the element at the end of
 * the body element.
 */
IMPORT INT tad_lnk_count( CONST T_TAD *doc );
IMPORT ER tad_lnk_get( CONST T_TAD *doc, INT i, T_VOBJ *pk_vobj );
IMPORT ER tad_lnk_find( CONST T_TAD *doc, CONST TS_UUID *vobjid, T_VOBJ *pk_vobj );
IMPORT ER tad_lnk_add( T_TAD *doc, CONST T_VOBJ *pk_vobj, TS_UUID *p_vobjid );
/*
 * Change an existing <link>: it keeps its place and its identity and
 * what it points at, and everything else it says is rewritten. The rest
 * of the document is untouched, so a document whose links did not move
 * still comes back byte for byte.
 */
IMPORT ER tad_lnk_set( T_TAD *doc, CONST T_VOBJ *pk_vobj );

/*
 * The links put in a new order, front to back as the document holds
 * them: 'order' names every link of the document once, by its identity,
 * first to last. Each link moves into the place in the document that
 * the link at its new number had, so what stands between the links --
 * shapes, words -- stays where it was. Later in the document is further
 * forward on the page.
 */
IMPORT ER tad_lnk_reorder( T_TAD *doc, CONST TS_UUID *order, INT n );
IMPORT ER tad_lnk_del( T_TAD *doc, CONST TS_UUID *vobjid );

/*
 * Changing the text of a document (design 17.12). Each works on the
 * nodes and leaves them to be written from what they now hold.
 *
 *   tad_text_splice	'cut' bytes of a text node from 'at' replaced by 'n'
 *   tad_text_new	a text node put into 'parent' before 'before' (NULL:
 *			last)
 *   tad_elem_new	an element put in the same way, with no attributes
 *   tad_node_remove	a node taken out, with everything under it
 *   tad_node_move	a node moved, with everything under it
 *   tad_text_split	a text node cut in two at a byte; answers the second
 *   tad_split_before	'c' and all after it, to the end of 'upto', moved
 *			into copies of the elements they sit in; answers
 *			the copy of 'upto', put right after it
 *   tad_join		the children of 'b' moved to the end of 'a', and 'b'
 *			taken out
 *   tad_wrap		siblings 'first' to 'last' put inside a new element
 */
IMPORT ER tad_text_splice( T_TAD *doc, T_TADNODE *nd, INT at, INT cut,
			   CONST UB *ins, INT n );
IMPORT T_TADNODE *tad_text_new( T_TAD *doc, T_TADNODE *parent,
				T_TADNODE *before, CONST UB *s, INT n );
IMPORT T_TADNODE *tad_elem_new( T_TAD *doc, CONST char *name,
				T_TADNODE *parent, T_TADNODE *before,
				BOOL empty );
IMPORT void tad_node_remove( T_TADNODE *nd );
/* A node copied, with all it holds, into a place of the same or another document */
IMPORT T_TADNODE *tad_node_copy( T_TAD *doc, T_TADNODE *parent, T_TADNODE *before,
				 CONST T_TADNODE *from );
/* a node taken from where it is and put into 'parent' before 'before' */
IMPORT void tad_node_move( T_TADNODE *nd, T_TADNODE *parent,
			   T_TADNODE *before );
IMPORT T_TADNODE *tad_text_split( T_TAD *doc, T_TADNODE *nd, INT at );
IMPORT T_TADNODE *tad_split_before( T_TAD *doc, T_TADNODE *c,
				    T_TADNODE *upto );
IMPORT void tad_join( T_TADNODE *a, T_TADNODE *b );
IMPORT T_TADNODE *tad_wrap( T_TAD *doc, T_TADNODE *first, T_TADNODE *last,
			    CONST char *name );

/*
 * What the edits owe the file system: one entry for each <link> added
 * (+1) or removed (-1) since the document was parsed. They are applied
 * and cleared by lib/libom, in the transaction that writes the document
 * (design 16.3.4).
 */
IMPORT INT tad_lnk_ndelta( CONST T_TAD *doc );
IMPORT ER tad_lnk_delta( CONST T_TAD *doc, INT i, TS_UUID *p_target, INT *p_delta );
IMPORT void tad_lnk_clr_delta( T_TAD *doc );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TAD_H__ */
