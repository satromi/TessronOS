/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtres.c
 *	The system's faces and the pictures of the ground as real objects
 *	(design 18.18)
 *
 *	The system's faces and pictures are objects on the disk from the
 *	start, linked from the 書体箱 and the 壁紙箱 -- boxes of fixed
 *	identity -- each with the file it was made from named in its
 *	metadata ("tessronos.file") and the file's bytes in record 1
 *	(tools/mkobj.py). Nothing is taken in when the desktop starts. The
 *	boxes are searched by the object layer (ob_lst_lnk, ob_fnd_lnk),
 *	which knows a link by its object name or its file name.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/fn.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/xf.h>
#include <ts/sysdef.h>
#include "desktop.h"

#define RES_MAX		32		/* faces, or pictures, in one box */

LOCAL CONST char font_box_id[] = SYSDEF_FONT_BOX;
LOCAL CONST char wall_box_id[] = SYSDEF_WALL_BOX;

/* The face the letters of the system are drawn in, when it is there */
LOCAL CONST char *system_face[] = {
	"NotoSansJP-Regular.otf", "NOTOSANS.OTF", "KOGANEI.TTF", NULL
};

/* The picture the ground is laid with, when it is there */
LOCAL CONST char *system_wall[] = {
	"TESSRON.PIC", NULL
};

/* ---------------------------------------------------------------- names */

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

/* Whether two names are the same, without case: FAT keeps none */
LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( lower(a[i]) != lower(b[i]) ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* ---------------------------------------------------------------- boxes */

/* What a box links to */
LOCAL INT box_links( CONST char *id, TS_UUID *ids, INT max )
{
	TS_UUID	u;
	INT	n = 0;

	if ( ts_str_to_uuid(id, &u) < E_OK || ob_lst_lnk(&u, NULL, ids, max, &n) < E_OK ) {
		return 0;
	}
	return ( n > max ) ? max : n;
}

/* The first of the names that the box has a link of: the one most wanted there is */
LOCAL BOOL box_pick( CONST char *id, CONST char **names, TS_UUID *p_uuid )
{
	TS_UUID	u;
	INT	k;

	if ( ts_str_to_uuid(id, &u) < E_OK ) {
		return FALSE;
	}
	for ( k = 0; names[k] != NULL; k++ ) {
		if ( ob_fnd_lnk(&u, (CONST UB *)names[k], p_uuid) >= E_OK ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( ts_uuid_cmp(a, b) == 0 );
}

/* ---------------------------------------------------------------- what the desktop asks */

/*
 * A face open before fid of the same family, 0 none. The faces stay open
 * for the life of the system (the windows, the menus and the message
 * line draw with them), and the table of faces is small: taking them in
 * again, as a desktop started a second time does, opens each once more
 * and fills it.
 */
LOCAL ID face_open_before( ID fid )
{
	UB	fam[64], other[64];
	INT	i, n = fn_nface();

	if ( fn_family(fid, fam, sizeof(fam)) <= 0 ) {
		return 0;
	}
	for ( i = 0; i < n; i++ ) {
		ID	f = fn_face_at(i);

		if ( f > 0 && f != fid && fn_family(f, other, sizeof(other)) > 0
		  && same_name(fam, other) ) {
			return f;
		}
	}
	return 0;
}

/*
 * Every face in the 書体箱 opened, the system's own face chosen among
 * them. Answers how many are open, taken in now or before; the system
 * face is set when one is.
 */
EXPORT INT dt_res_faces( void )
{
	TS_UUID	*ids, sys;
	ID	fid, pick = 0;
	INT	n, i, opened = 0;
	BOOL	have_sys, is_sys = FALSE;

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * RES_MAX);
	if ( ids == NULL ) {
		return 0;
	}
	have_sys = box_pick(font_box_id, system_face, &sys);
	n = box_links(font_box_id, ids, RES_MAX);
	for ( i = 0; i < n; i++ ) {
		if ( fn_open_obj(&ids[i], xf_data_rec(&ids[i]), 0, &fid) < E_OK || fid <= 0 ) {
			continue;
		}
		{
			ID	before = face_open_before(fid);

			if ( before > 0 ) {
				(void)fn_close(fid);	/* the one open already is used */
				fid = before;
			}
		}
		opened++;
		if ( have_sys && !is_sys && same_uuid(&ids[i], &sys) ) {
			is_sys = TRUE;
			pick = fid;
		}
		if ( pick == 0 ) {
			pick = fid;			/* any face rather than none */
		}
	}
	Kfree(ids);
	if ( pick > 0 ) {
		fn_set_size(pick, 14);
		fn_set_system(pick);
	}
	return opened;
}

/*
 * The picture the ground is laid with: the system's own among those in
 * the 壁紙箱, or the first there when it is not among them.
 */
EXPORT ER dt_res_wall( UINT mode )
{
	TS_UUID	box, pick;

	if ( !box_pick(wall_box_id, system_wall, &pick) ) {
		if ( ts_str_to_uuid(wall_box_id, &box) < E_OK || ob_fnd_lnk(&box, NULL, &pick) < E_OK ) {
			return E_NOEXS;
		}
	}
	return wm_load_wall_obj(&pick, xf_data_rec(&pick), mode);
}
