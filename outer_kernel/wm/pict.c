/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	pict.c
 *	Pictograms: the small pictures that stand for things (design 16.2.3)
 *
 *	A pictogram is a definition in the data box like any other, and one
 *	definition holds the same picture in several sizes, laid one after
 *	another. Whoever draws one gives the box it has room for, and the
 *	size nearest to that box without going over it is the one drawn.
 *	That is why the sizes are kept together: a title band, a cabinet
 *	and a list all show the same thing, at whatever size each of them
 *	can give it, and none of them is a scaled copy of another.
 *
 *	A picture that is not in the box is not an error. Everything that
 *	shows one shows the standing mark instead -- a leaf of paper with
 *	its corner turned -- because a name that shifts left when a picture
 *	is missing is a window that looks different for a reason that has
 *	nothing to do with the window.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/dbox.h>
#include <ts/fs.h>
#include <ts/ob.h>

/*
 * The largest size in the definition that fits the box given. The
 * pieces are walked by the length each one declares; a piece that
 * points past the end of the definition ends the walk, so a definition
 * that has been cut short is drawn as far as it is whole.
 */
LOCAL CONST T_DBPICT *pick_size( CONST UB *def, SZ size, INT w, INT h )
{
	CONST T_DBPICT	*best = NULL;
	SZ		at = 0;

	while ( at + (SZ)sizeof(T_DBPICT) <= size ) {
		CONST T_DBPICT	*p = (CONST T_DBPICT *)(def + at);
		SZ		need = wm_pict_bytes(p);

		if ( need == 0 || at + need > size ) {
			break;
		}
		if ( p->width <= (UH)w && p->height <= (UH)h
		  && ( best == NULL || p->height > best->height ) ) {
			best = p;
		}
		if ( p->next == 0 ) {
			break;
		}
		at += p->next;
	}

	return best;
}

EXPORT SZ wm_pict_bytes( CONST T_DBPICT *p )
{
	SZ	rows;

	if ( p == NULL || p->width == 0 || p->height == 0 ) {
		return 0;
	}
	if ( p->depth == 1 ) {
		rows = (SZ)( ( p->width + 7 ) / 8 ) * p->height;
	} else if ( p->depth == 32 ) {
		rows = (SZ)p->width * p->height * 4;
	} else {
		return 0;
	}

	return sizeof(T_DBPICT) + rows;
}

/*
 * The standing mark: a leaf of paper with the corner turned, drawn to
 * fill the box given in three quarters of its height. It is drawn
 * rather than left out so that whatever follows it stands in the same
 * place whether or not a picture was found.
 */
EXPORT void wm_pict_mark( INT gid, CONST T_DPRECT *box, UW ink, UW paper )
{
	T_DPRECT	q;
	INT		fold;

	if ( box == NULL
	  || box->right - box->left < 6 || box->bottom - box->top < 6 ) {
		return;
	}
	q = *box;
	q.right = q.left + ( q.bottom - q.top ) * 3 / 4;
	fold = ( q.right - q.left ) / 3;
	dp_fill_rect(gid, &q, paper);
	dp_frame_rect(gid, &q, ink, 1);
	dp_line(gid, q.right - 1 - fold, q.top, q.right - 1, q.top + fold, ink);
	dp_line(gid, q.right - 1 - fold, q.top,
		q.right - 1 - fold, q.top + fold, ink);
	dp_line(gid, q.right - 1 - fold, q.top + fold, q.right - 1, q.top + fold,
		ink);
}

/*
 * One pictogram in the box given, centred in it. A picture of one bit a
 * pixel is drawn in the ink given, and the bits that are not set are
 * left alone; a picture of a colour a pixel carries its own colours,
 * and a pixel of WM_PICT_CLEAR is left alone. Answers E_NOEXS when
 * nothing of that number is in the box, so the caller may fall back to
 * the standing mark.
 */
EXPORT ER wm_pict_draw( INT gid, INT num, ID owner, CONST T_DPRECT *box,
			UW ink )
{
	UB		*def;
	CONST T_DBPICT	*p;
	INT		got, w, h, x, y;

	if ( box == NULL ) {
		return E_PAR;
	}
	w = box->right - box->left;
	h = box->bottom - box->top;
	if ( w <= 0 || h <= 0 ) {
		return E_PAR;
	}
	/* an entry of the box can be larger than a stack wants to hold */
	def = (UB *)Kmalloc(DB_MAX_SIZE);
	if ( def == NULL ) {
		return E_NOMEM;
	}
	got = db_get(DB_PICT, num, owner, def, DB_MAX_SIZE);
	if ( got < 0 ) {
		Kfree(def);
		return E_NOEXS;
	}
	p = pick_size(def, (SZ)got, w, h);
	if ( p == NULL ) {
		Kfree(def);
		return E_NOEXS;
	}
	x = box->left + ( w - (INT)p->width ) / 2;
	y = box->top + ( h - (INT)p->height ) / 2;

	if ( p->depth == 1 ) {
		dp_put_mono(gid, x, y, (CONST UB *)( p + 1 ),
			    ( p->width + 7 ) / 8, p->width, p->height, ink);
	} else {
		dp_put_argb(gid, x, y, (CONST UW *)(CONST void *)( p + 1 ),
			    (INT)p->width, (INT)p->width, (INT)p->height,
			    WM_PICT_CLEAR);
	}
	Kfree(def);

	return E_OK;
}

/*
 * The ground's picture, read from a file.
 *
 * A picture large enough to cover a screen does not belong in the data
 * box -- the box is for definitions, and a definition is small -- so
 * the ground's own picture is read straight off the disk into pages of
 * its own. Only the first size in the file is used: a ground is laid
 * once, at whatever size it was made.
 *
 * The pages are kept for as long as the picture is on the ground,
 * because what is handed over is the pixels themselves and not a copy.
 * Laying a new picture frees the one before it; laying none frees what
 * there was and leaves the ground plain.
 */
LOCAL UW	*wall_pix = NULL;
LOCAL UD	wall_pages = 0;

EXPORT ER wm_load_wall( CONST char *path, UINT mode )
{
	T_DBPICT	head;
	T_FSTAT		st;
	INT		fd, got;
	SZ		want;
	UD		pages;
	UW		*pix;

	if ( path == NULL ) {
		wm_set_wall(NULL, 0, 0, mode);
		if ( wall_pix != NULL ) {
			knl_vunmap((UB *)wall_pix, wall_pages);
			wall_pix = NULL;
			wall_pages = 0;
		}
		return E_OK;
	}
	if ( fs_stat(path, &st) < EX_OK || st.size <= sizeof(head) ) {
		return E_NOEXS;
	}
	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		return E_NOEXS;
	}
	if ( fs_read(fd, &head, sizeof(head)) != (INT)sizeof(head)
	  || head.depth != 32 || head.width == 0 || head.height == 0 ) {
		fs_close(fd);
		return E_OBJ;
	}
	want = (SZ)head.width * head.height * 4;
	if ( want + (SZ)sizeof(head) > (SZ)st.size ) {
		fs_close(fd);
		return E_OBJ;			/* cut short: not a whole picture */
	}
	pages = (UD)( ( want + 4095 ) / 4096 );
	pix = (UW *)knl_vmap(pages, 0);
	if ( pix == NULL ) {
		fs_close(fd);
		return E_NOMEM;
	}
	/*
	 * Read it in pieces. A picture of a few megabytes is more than the
	 * file system will hand over in one call on every medium, and a
	 * short read is not a fault.
	 */
	{
		SZ	at = 0;

		while ( at < want ) {
			SZ	chunk = want - at;

			if ( chunk > 65536 ) {
				chunk = 65536;
			}
			got = fs_read(fd, (UB *)pix + at, (INT)chunk);
			if ( got <= 0 ) {
				break;
			}
			at += (SZ)got;
		}
		fs_close(fd);
		if ( at < want ) {
			knl_vunmap((UB *)pix, pages);
			return E_IO;
		}
	}
	wm_set_wall(pix, (INT)head.width, (INT)head.height, mode);
	if ( wall_pix != NULL ) {
		knl_vunmap((UB *)wall_pix, wall_pages);
	}
	wall_pix = pix;
	wall_pages = pages;

	return E_OK;
}

EXPORT ER wm_load_wall_obj( CONST TS_UUID *uuid, INT recno, UINT mode )
{
	T_DBPICT	head;
	SZ		want, at = 0, got = 0;
	UD		pages;
	UW		*pix;
	ID		key;
	ER		er = E_OK;

	if ( uuid == NULL ) {
		return E_PAR;
	}
	key = ob_opn_obj(uuid, OB_OP_READ);
	if ( key <= 0 ) {
		return E_NOEXS;
	}
	if ( ob_rea_rec(key, recno, 0, &head, sizeof(head), &got) < E_OK
	  || got != (SZ)sizeof(head) || head.depth != 32 || head.width == 0 || head.height == 0 ) {
		(void)ob_cls_obj(key);
		return E_OBJ;
	}
	want = (SZ)head.width * head.height * 4;
	pages = (UD)( ( want + 4095 ) / 4096 );
	pix = (UW *)knl_vmap(pages, 0);
	if ( pix == NULL ) {
		(void)ob_cls_obj(key);
		return E_NOMEM;
	}
	while ( at < want ) {
		SZ	chunk = ( want - at > 65536 ) ? 65536 : want - at;

		got = 0;
		er = ob_rea_rec(key, recno, (D)( sizeof(head) + at ), (UB *)pix + at, chunk, &got);
		if ( er < E_OK || got <= 0 ) {
			break;
		}
		at += got;
	}
	(void)ob_cls_obj(key);
	if ( at < want ) {
		knl_vunmap((UB *)pix, pages);
		return E_IO;			/* cut short: not a whole picture */
	}
	wm_set_wall(pix, (INT)head.width, (INT)head.height, mode);
	if ( wall_pix != NULL ) {
		knl_vunmap((UB *)wall_pix, wall_pages);
	}
	wall_pix = pix;
	wall_pages = pages;

	return E_OK;
}
