/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_face.c
 *	Faces (書体) as objects, found from a process (include/ts/face.h).
 *
 *	The 書体箱 is a box of links, and the faces are what they point at:
 *	both calls are the object layer's search of a box's links
 *	(ob_lst_lnk, ob_fnd_lnk), which knows a face by its object name or
 *	by the file name in its metadata. No C library is needed, so both
 *	the C programs (lib/libts) and the C++ ones (lib/libcxxrt) link it.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/sysdef.h>
#include <ts/face.h>

LOCAL BOOL font_box( TS_UUID *box )
{
	return (BOOL)( ob_fnd_sys(SYSROLE_FONTS, SYSDEF_FONT_BOX, box) >= E_OK );
}

EXPORT INT ts_face_list( TS_UUID *buf, INT max )
{
	TS_UUID	box;
	INT	cnt = 0;
	ER	er;

	if ( !font_box(&box) ) {
		return E_SYS;
	}
	if ( buf == NULL || max < 0 ) {
		max = 0;
	}
	er = ob_lst_lnk(&box, NULL, ( max > 0 ) ? buf : NULL, max, &cnt);
	return ( er < E_OK ) ? (INT)er : cnt;
}

EXPORT ER ts_face_find( CONST char *name, TS_UUID *p_uuid )
{
	TS_UUID	box;

	if ( p_uuid == NULL ) {
		return E_PAR;
	}
	if ( !font_box(&box) ) {
		return E_SYS;
	}
	return ob_fnd_lnk(&box, (CONST UB *)name, p_uuid);
}
