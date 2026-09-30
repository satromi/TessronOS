/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_vol.h
 *	バックアップ: one volume's bytes, in a file or in an object
 *	(design 17.18)
 */

#ifndef __BK_VOL_H__
#define __BK_VOL_H__

#include <ts/xfu.h>

typedef struct {
	char	dir[FS_PATH_MAX];	/* writing: the directory; "" an object */
	char	path[FS_PATH_MAX * 2];	/* the file */
	INT	fd;
	TS_UUID	obj;			/* the object */
	TS_UUID	into;			/* writing an object: the cabinet it goes in; 0 the first */
	ID	key;
	INT	rec;			/* reading an object: its record that holds it */
	UD	size, at;
	UB	*wb;			/* writing: bytes not yet put out */
	INT	wn;
	UD	woff;			/* where wb goes */
} BKVOL;

IMPORT ER     bk_vol_create( BKVOL *v, const char *name, const char *memo );
IMPORT BK_OUT bk_vol_writer( BKVOL *v );
IMPORT ER     bk_vol_done( BKVOL *v );
IMPORT ER     bk_vol_open( BKVOL *v );
IMPORT BK_SRC bk_vol_reader( BKVOL *v );
IMPORT void   bk_vol_rewind( BKVOL *v );
IMPORT void   bk_vol_label( BKVOL *v, char *out, INT max );

IMPORT ER     bk_link_into( const TS_UUID *cab, const TS_UUID *u, const char *name );
/* The same, the link where box says (a figure's), at the foot when box is NULL */
IMPORT ER     bk_link_at( const TS_UUID *cab, const TS_UUID *u, const char *name, const INT *box );
IMPORT ER     bk_first_cabinet( TS_UUID *u );

/* The box of a <link> tag from p to e (vobjleft ... vobjbottom), FALSE when it has none */
IMPORT BOOL   bk_tag_box( const char *p, const char *e, INT box[4] );
/* The links to u taken out of a cabinet */
IMPORT ER     bk_link_out( const TS_UUID *cab, const TS_UUID *u );

#endif /* __BK_VOL_H__ */
