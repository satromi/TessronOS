/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tad_lnkvol.c
 *	The ids of the links of an xmlTAD text made to say where their
 *	objects are (design 8.3)
 *
 *	A link's id names its object by UUID alone when the object is on
 *	the volume of the record that holds the link, and with that
 *	volume's UUID before it when the object is on another. Whatever
 *	writes a record puts this right first: the desktop when it saves
 *	(om_link_fix), and a program that writes its records itself, which
 *	learns the volumes with ob_ref_vid. Nothing here takes memory or
 *	calls the system: the caller gives the room and says how a volume
 *	is learnt, so the same source serves the kernel and the processes.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/uuid.h>
#include <ts/tadlnk.h>

LOCAL BOOL lv_same( CONST UB *a, CONST char *b, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL lv_space( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\n' || c == '\r' );
}

LOCAL BOOL lv_zero( CONST TS_UUID *u )
{
	CONST UB	*p = (CONST UB *)u;
	UINT		i;

	for ( i = 0; i < sizeof(TS_UUID); i++ ) {
		if ( p[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* The UUID written at s (TS_UUID_STRLEN letters) */
LOCAL BOOL lv_uuid_at( CONST UB *s, TS_UUID *p )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	q;

	for ( q = 0; q < TS_UUID_STRLEN; q++ ) us[q] = (char)s[q];
	us[TS_UUID_STRLEN] = 0;
	return (BOOL)( ts_str_to_uuid(us, p) >= E_OK );
}

/*
 * The next <link tag at or after from: where its id's value starts (v0)
 * and where the tag ends (just past its '>'). FALSE when there is none
 * more; a tag with no id is passed over.
 */
LOCAL BOOL lv_next( CONST UB *buf, SZ len, SZ from, SZ *p_v0, SZ *p_end )
{
	SZ	i, j, a;

	for ( i = from; i + 5 < len; i++ ) {
		if ( !lv_same(buf + i, "<link", 5) || !lv_space(buf[i + 5]) ) continue;
		for ( j = i + 5; j < len && buf[j] != '>'; j++ ) ;
		if ( j >= len ) return FALSE;
		for ( a = i + 5; a + 5 < j && !( lv_space(buf[a]) && lv_same(buf + a + 1, "id=\"", 4) ); a++ ) ;
		if ( a + 5 < j ) {
			*p_v0 = a + 5;
			*p_end = j + 1;
			return TRUE;
		}
		i = j;
	}
	return FALSE;
}

EXPORT SZ tad_lnk_vol_room( CONST UB *in, SZ len )
{
	SZ	at = 0, v0, e, n = 0;

	while ( in != NULL && lv_next(in, len, at, &v0, &e) ) {
		n++;
		at = e;
	}
	return len + n * TS_LNK_PART + 1;
}

EXPORT SZ tad_lnk_vol_fix( CONST UB *in, SZ len, UB *out, CONST TS_UUID *hv, CONST TS_UUID *mine,
			   TAD_VOLOF vol_of, void *arg, INT *p_changed )
{
	SZ	at = 0, v0, e, o = 0, c;
	INT	k, vat, mat, changed = 0;
	TS_UUID	id, tv, said;

	while ( lv_next(in, len, at, &v0, &e) ) {
		BOOL	same;

		/* what is before the id's value goes as it is */
		for ( c = at; c < v0; c++ ) out[o++] = in[c];
		at = v0;
		k = ts_lnk_split(in + v0, (INT)( e - v0 ), &vat, &mat);
		if ( v0 + (SZ)k + TS_UUID_STRLEN > e || !lv_uuid_at(in + v0 + k, &id) ) {
			continue;			/* no object to look at: as it is */
		}
		if ( mat >= 0 && ( mine == NULL || !lv_uuid_at(in + v0 + mat, &said)
				   || ts_uuid_cmp(&said, mine) != 0 ) ) {
			continue;			/* another machine's: as it is */
		}
		if ( vol_of(&id, &tv, arg) < E_OK ) {
			continue;			/* on no volume attached: as it is */
		}
		same = (BOOL)( ts_uuid_cmp(&tv, hv) == 0 || lv_zero(&tv) );
		if ( same ) {
			if ( k == 0 ) continue;		/* bare already */
		} else if ( k == TS_LNK_PART && lv_uuid_at(in + v0, &said) && ts_uuid_cmp(&said, &tv) == 0 ) {
			continue;			/* it names the right volume already */
		}
		if ( !same ) {
			char	us[TS_UUID_STRLEN + 1];
			INT	q;

			(void)ts_uuid_to_str(&tv, us, sizeof(us));
			for ( q = 0; q < TS_UUID_STRLEN; q++ ) out[o++] = (UB)us[q];
			out[o++] = '/';
		}
		at = v0 + (SZ)k;		/* the volume and the machine it said, left out */
		changed++;
	}
	for ( c = at; c < len; c++ ) out[o++] = in[c];
	out[o] = 0;
	if ( p_changed != NULL ) *p_changed = changed;
	return o;
}
