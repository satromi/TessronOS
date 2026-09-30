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

LOCAL INT hexval( INT c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

/* "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" at s; FALSE when it is not one */
LOCAL BOOL parse_uuid( CONST UB *s, TS_UUID *u )
{
	INT	i, k = 0;

	for ( i = 0; i < 36; i++ ) {
		INT	hi, lo;

		if ( i == 8 || i == 13 || i == 18 || i == 23 ) {
			if ( s[i] != '-' ) return FALSE;
			continue;
		}
		hi = hexval(s[i]);
		lo = hexval(s[i + 1]);
		if ( hi < 0 || lo < 0 ) return FALSE;
		u->b[k++] = (UB)( ( hi << 4 ) | lo );
		i++;
	}
	return (BOOL)( k == 16 );
}

LOCAL BOOL font_box( TS_UUID *box )
{
	static CONST char	box_id[] = SYSDEF_FONT_BOX;

	return parse_uuid((CONST UB *)box_id, box);
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
