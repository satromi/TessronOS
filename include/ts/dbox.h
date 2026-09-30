/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dbox.h
 *	The data box: where definitions live (design 16.2.3, 18.15)
 *
 *	A panel, a menu, a part or a pattern is a definition, and a
 *	definition is data that some program wrote and another program
 *	draws. The box is where they are kept, found by what kind they are,
 *	by number, and by whose they are.
 *
 *	Two rules.
 *
 *	Nothing is handed out by pointer. A definition held by pointer
 *	could be changed by one program under everyone drawing it, and a
 *	program that died would leave it behind for whoever came next. A
 *	definition is copied in when it is put and copied out when it is
 *	got, and the box owns the copy.
 *
 *	A definition is named by kind, number and owner, not by number
 *	alone. Two programs may both have a panel 3 and they are not the
 *	same panel. An owner of 0 is the system's own, which every program
 *	may read.
 *
 *	When a program ends, everything it owned goes with it. That is one
 *	call, and it is the whole of the cleanup.
 */

#ifndef __TS_DBOX_H__
#define __TS_DBOX_H__

#ifdef __cplusplus
extern "C" {
#endif

/* What a definition is */
#define DB_PANEL	1		/* a panel: its parts and where they sit */
#define DB_MENU		2		/* a menu: its items */
#define DB_PART		3		/* one part on its own */
#define DB_PAT		4		/* a pattern */
#define DB_TEXT		5		/* words: labels, names, messages */
#define DB_PICT		6		/* a picture */

/*
 * What a picture definition looks like: the same picture in several
 * sizes, one after another, each headed by this. 'next' is how many
 * bytes from the head of one size to the head of the next, and is zero
 * on the last -- a length rather than a pointer, so that a definition
 * copied out of the box is whole wherever it lands.
 *
 * A depth of 1 is one bit a pixel, rows padded out to bytes, drawn in
 * whatever ink the caller asks for; a depth of 32 is a colour a pixel,
 * 0x00rrggbb, and a pixel of WM_PICT_CLEAR is not drawn.
 */
typedef struct {
	UH	width, height;
	UH	depth;
	UH	next;
} T_DBPICT;

#define DB_MAX_ENT	64		/* definitions the box holds at once */
/*
 * The largest definition. A panel of the most parts it may hold, each
 * with its label, its words and its fields, is a little under ninety
 * thousand bytes, so this is room for the largest thing the box is
 * asked to hold.
 */
#define DB_MAX_SIZE	131072

/*
 * Put a definition in, replacing whatever stood under that name. The
 * bytes are copied, so the caller may change or free its own at once.
 */
IMPORT ER  db_put( UINT kind, INT num, ID owner, CONST void *data, SZ size );

/*
 * Take a copy of one. Answers how many bytes it came to, E_NOEXS when
 * there is no such definition, or E_LIMIT when the room given is too
 * small -- in which case nothing is written and db_size says how much
 * is wanted.
 *
 * An owner of 0 is looked at only when the owner asked for has nothing
 * under that name, so a program may put its own panel 3 over the
 * system's and take it away again by deleting it.
 */
IMPORT INT db_get( UINT kind, INT num, ID owner, void *buf, SZ max );
IMPORT INT db_size( UINT kind, INT num, ID owner );

IMPORT ER  db_del( UINT kind, INT num, ID owner );

/* Everything one program owned, taken away at once when it ends */
IMPORT INT db_del_owner( ID owner );

IMPORT INT db_count( void );		/* how many are in the box */
IMPORT INT knl_db_init( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_DBOX_H__ */
