/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	om_copy.c
 *	A real object copied, with everything it holds (design 16.3.3, 18)
 *
 *	A copy of a real object is a new real object: a new identity, the
 *	same records, the same resources beside them, and metadata of its
 *	own. What it links to is copied as well, so that changing the copy
 *	changes nothing of the original -- a copy whose inner objects are
 *	still the original's is two objects sharing their insides.
 *
 *	Every object of the copy is made first through the object layer,
 *	which gives it its identity and makes the one copying its owner;
 *	then its records are written, xmlTAD as text with every copied
 *	identity changed to the copy's (an identity is 36 letters wherever
 *	it stands, so every other byte stays as it was written), other
 *	records as they are; then its resources; and last its metadata --
 *	the new identity, the name, today's dates, and how many links in
 *	the copy point at it.
 *
 *	A linked object that cannot be copied -- it is not there, or the
 *	copy has grown past what one copy may reach -- has its link taken
 *	out of the copy. A link left pointing at nothing would open nothing.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/wm.h>

#define CP_MAX		32		/* objects one copy may reach */
#define CP_ID_LEN	36		/* letters of an identity */
#define CP_RES_LIST	4096		/* bytes of the names of one object's resources */

typedef struct {
	INT	n;
	TS_UUID	from[CP_MAX];
	TS_UUID	to[CP_MAX];
	char	from_s[CP_MAX][CP_ID_LEN + 1];
	char	to_s[CP_MAX][CP_ID_LEN + 1];
	INT	refs[CP_MAX];		/* links in the copy that point here */
	BOOL	done[CP_MAX];		/* it is written */
	BOOL	bad[CP_MAX];		/* it could not be copied */
	BOOL	deep;			/* what is linked to is copied too */

	/* shared, when not deep: what the copy links to, and how often */
	TS_UUID	shared[CP_MAX];
	INT	nshared[CP_MAX];
	INT	nsh;
} CPMAP;

/* ---------------------------------------------------------------- bytes */

LOCAL SZ cp_strlen( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) {
		n++;
	}

	return n;
}

LOCAL BOOL cp_same( CONST UB *a, CONST char *b, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != (UB)b[i] ) {
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * Bytes cut out of a buffer and others put in their place. The buffer
 * is made again, because what goes in need not be as long as what came
 * out.
 */
LOCAL ER cp_splice( UB **p_buf, SZ *p_len, SZ at, SZ cut, CONST UB *ins,
		    SZ ins_len )
{
	UB	*old = *p_buf, *nb;
	SZ	len = *p_len, nlen, i, k = 0;

	nlen = len - cut + ins_len;
	nb = (UB *)Kmalloc(nlen + 1);
	if ( nb == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < at; i++ ) {
		nb[k++] = old[i];
	}
	for ( i = 0; i < ins_len; i++ ) {
		nb[k++] = ins[i];
	}
	for ( i = at + cut; i < len; i++ ) {
		nb[k++] = old[i];
	}
	nb[k] = 0;
	Kfree(old);
	*p_buf = nb;
	*p_len = nlen;

	return E_OK;
}

/* Every place one identity's letters stand, given another's */
LOCAL void cp_swap_id( UB *buf, SZ len, CONST char *from, CONST char *to )
{
	SZ	i;
	INT	k;

	for ( i = 0; i + CP_ID_LEN <= len; i++ ) {
		if ( cp_same(buf + i, from, CP_ID_LEN) ) {
			for ( k = 0; k < CP_ID_LEN; k++ ) {
				buf[i + (SZ)k] = (UB)to[k];
			}
			i += CP_ID_LEN - 1;
		}
	}
}

/* ---------------------------------------------------------------- the map */

LOCAL INT cp_find( CONST CPMAP *m, CONST TS_UUID *id )
{
	INT	i;

	for ( i = 0; i < m->n; i++ ) {
		if ( ts_uuid_cmp(&m->from[i], id) == 0 ) {
			return i;
		}
	}

	return -1;
}

LOCAL INT cp_find_new( CONST CPMAP *m, CONST TS_UUID *id )
{
	INT	i;

	for ( i = 0; i < m->n; i++ ) {
		if ( ts_uuid_cmp(&m->to[i], id) == 0 ) {
			return i;
		}
	}

	return -1;
}

/* ---------------------------------------------------------------- links */

/*
 * The next <link> at or after 'from': where its tag starts and ends,
 * and the identity its id names. The attribute is " id=", with the
 * space: "vobjid=" also ends in id=, and is the link's own identity,
 * not what it points at.
 */
LOCAL BOOL cp_next_link( CONST UB *buf, SZ len, SZ from, SZ *p_start,
			 SZ *p_end, TS_UUID *p_id )
{
	SZ	i, j;

	for ( i = from; i + 5 < len; i++ ) {
		if ( !cp_same(buf + i, "<link", 5)
		  || ( buf[i + 5] != ' ' && buf[i + 5] != '\t'
		    && buf[i + 5] != '\n' && buf[i + 5] != '\r' ) ) {
			continue;
		}
		for ( j = i + 5; j < len && buf[j] != '>'; j++ ) {
			;
		}
		if ( j >= len ) {
			return FALSE;
		}
		*p_start = i;
		*p_end = j + 1;
		{
			SZ	a;
			char	txt[CP_ID_LEN + 1];
			INT	k;

			for ( a = i + 5; a + 5 + CP_ID_LEN < j; a++ ) {
				if ( ( buf[a] == ' ' || buf[a] == '\t'
				    || buf[a] == '\n' || buf[a] == '\r' )
				  && cp_same(buf + a + 1, "id=\"", 4) ) {
					for ( k = 0; k < CP_ID_LEN; k++ ) {
						txt[k] = (char)buf[a + 5 + (SZ)k];
					}
					txt[CP_ID_LEN] = 0;
					if ( ts_str_to_uuid(txt, p_id) >= E_OK ) {
						return TRUE;
					}
					break;
				}
			}
		}
		/* a link with no identity to follow: look further on */
		i = j;
	}

	return FALSE;
}

/*
 * A link taken out of a record. A link written as one tag goes whole;
 * one with its name between an opening and a closing tag keeps the
 * name, which was words of the record, and loses only the tags.
 */
LOCAL void cp_drop_link( UB **p_buf, SZ *p_len, SZ start, SZ end )
{
	UB	*buf = *p_buf;
	SZ	i;

	if ( end >= 2 && buf[end - 2] == '/' ) {
		cp_splice(p_buf, p_len, start, end - start, NULL, 0);
		return;
	}
	for ( i = end; i + 7 <= *p_len; i++ ) {
		if ( cp_same(buf + i, "</link>", 7) ) {
			cp_splice(p_buf, p_len, i, 7, NULL, 0);
			break;
		}
	}
	cp_splice(p_buf, p_len, start, end - start, NULL, 0);
}

/* ---------------------------------------------------------------- the name */

/* The record's own name, in the <tad> element, made the copy's */
LOCAL void cp_set_filename( UB **p_buf, SZ *p_len, CONST UB *name )
{
	UB	*buf = *p_buf;
	UB	esc[TAD_NAME_MAX * 6];
	SZ	i, j, n = 0;
	INT	k;

	for ( i = 0; i + 4 < *p_len; i++ ) {
		if ( cp_same(buf + i, "<tad", 4) ) {
			break;
		}
	}
	for ( ; i + 11 < *p_len && buf[i] != '>'; i++ ) {
		if ( cp_same(buf + i, " filename=\"", 11) ) {
			break;
		}
	}
	if ( i + 11 >= *p_len || buf[i] == '>' ) {
		return;				/* it has none to change */
	}
	i += 11;
	for ( j = i; j < *p_len && buf[j] != '"'; j++ ) {
		;
	}
	for ( k = 0; name[k] != 0 && n + 6 < (SZ)sizeof(esc); k++ ) {
		CONST char	*e = NULL;

		switch ( name[k] ) {
		case '&':	e = "&amp;";	break;
		case '<':	e = "&lt;";	break;
		case '>':	e = "&gt;";	break;
		case '"':	e = "&quot;";	break;
		default:	break;
		}
		if ( e != NULL ) {
			while ( *e != 0 ) {
				esc[n++] = (UB)*e++;
			}
		} else {
			esc[n++] = name[k];
		}
	}
	cp_splice(p_buf, p_len, i, j - i, esc, n);
}

/* ---------------------------------------------------------------- metadata */

/* Where the value of a key at the top of the metadata starts, or -1 */
LOCAL INT cp_json_value( CONST UB *buf, SZ len, CONST char *key )
{
	SZ	kl = cp_strlen(key), i, j;
	INT	depth = 0;

	for ( i = 0; i + kl + 2 < len; i++ ) {
		if ( buf[i] == '{' ) {
			depth++;
			continue;
		}
		if ( buf[i] == '}' ) {
			depth--;
			continue;
		}
		if ( depth != 1 || buf[i] != '"'
		  || !cp_same(buf + i + 1, key, kl) || buf[i + 1 + kl] != '"' ) {
			continue;
		}
		for ( j = i + kl + 2; j < len && buf[j] != ':'; j++ ) {
			;
		}
		for ( j++; j < len && ( buf[j] == ' ' || buf[j] == '\t' ); j++ ) {
			;
		}
		return ( j < len ) ? (INT)j : -1;
	}

	return -1;
}

/* How long the value starting there is: a string with its quotes, or up
   to the comma or brace that ends it */
LOCAL SZ cp_json_extent( CONST UB *buf, SZ len, SZ at )
{
	SZ	j = at;

	/* a list or an object: to the bracket that closes it */
	if ( buf[at] == '[' || buf[at] == '{' ) {
		INT	depth = 0;
		BOOL	in_str = FALSE;

		for ( j = at; j < len; j++ ) {
			if ( in_str ) {
				if ( buf[j] == '\\' ) {
					j++;
				} else if ( buf[j] == '"' ) {
					in_str = FALSE;
				}
				continue;
			}
			if ( buf[j] == '"' ) {
				in_str = TRUE;
			} else if ( buf[j] == '[' || buf[j] == '{' ) {
				depth++;
			} else if ( buf[j] == ']' || buf[j] == '}' ) {
				if ( --depth == 0 ) {
					return j + 1 - at;
				}
			}
		}
		return len - at;
	}

	if ( buf[at] == '"' ) {
		for ( j = at + 1; j < len && buf[j] != '"'; j++ ) {
			if ( buf[j] == '\\' ) {
				j++;
			}
		}
		return ( j < len ) ? j + 1 - at : len - at;
	}
	while ( j < len && buf[j] != ',' && buf[j] != '}' && buf[j] != '\n'
	     && buf[j] != '\r' ) {
		j++;
	}
	while ( j > at && ( buf[j - 1] == ' ' || buf[j - 1] == '\t' ) ) {
		j--;
	}

	return j - at;
}

/*
 * A key at the top of the metadata given a new value, written as it is
 * to stand in the file (quotes and all for a string). A key that is not
 * there is put in at the start.
 */
LOCAL void cp_json_set( UB **p_buf, SZ *p_len, CONST char *key,
			CONST UB *val, SZ vlen )
{
	INT	at = cp_json_value(*p_buf, *p_len, key);
	UB	line[96 + TAD_NAME_MAX * 2];
	SZ	n = 0, i, kl;

	if ( at >= 0 ) {
		cp_splice(p_buf, p_len, (SZ)at,
			  cp_json_extent(*p_buf, *p_len, (SZ)at), val, vlen);
		return;
	}
	for ( i = 0; i < *p_len && (*p_buf)[i] != '{'; i++ ) {
		;
	}
	if ( i >= *p_len ) {
		return;
	}
	kl = cp_strlen(key);
	if ( kl + vlen + 12 > (SZ)sizeof(line) ) {
		return;
	}
	line[n++] = '\n';  line[n++] = ' ';  line[n++] = ' ';
	line[n++] = '"';
	knl_memcpy(line + n, key, kl);  n += kl;
	line[n++] = '"';  line[n++] = ':';  line[n++] = ' ';
	knl_memcpy(line + n, val, vlen);  n += vlen;
	line[n++] = ',';
	cp_splice(p_buf, p_len, i + 1, 0, line, n);
}

/* A key and its value taken out */
LOCAL void cp_json_drop( UB **p_buf, SZ *p_len, CONST char *key )
{
	INT	at = cp_json_value(*p_buf, *p_len, key);
	SZ	start, end;
	UB	*b = *p_buf;

	if ( at < 0 ) {
		return;
	}
	/* from the quote before the key to the comma after the value */
	start = (SZ)at;
	while ( start > 0 && b[start] != '"' ) {
		start--;
	}
	if ( start > 0 ) {
		start--;
		while ( start > 0 && b[start] != '"' ) {
			start--;
		}
	}
	end = (SZ)at + cp_json_extent(b, *p_len, (SZ)at);
	if ( end < *p_len && b[end] == ',' ) {
		end++;
	} else {
		/* the last key: the comma before it goes instead */
		SZ	c = start;

		while ( c > 0 && ( b[c - 1] == ' ' || b[c - 1] == '\t'
				|| b[c - 1] == '\n' || b[c - 1] == '\r' ) ) {
			c--;
		}
		if ( c > 0 && b[c - 1] == ',' ) {
			start = c - 1;
		}
	}
	cp_splice(p_buf, p_len, start, end - start, NULL, 0);
}

/* A member of the object a top-level member holds, dropped ("tessronos" -> "file") */
LOCAL void cp_json_drop_in( UB **p_buf, SZ *p_len, CONST char *outer, CONST char *key )
{
	INT	at = cp_json_value(*p_buf, *p_len, outer);
	SZ	ext, ilen;
	UB	*inner;

	if ( at < 0 || (*p_buf)[at] != '{' ) {
		return;
	}
	ext = cp_json_extent(*p_buf, *p_len, (SZ)at);
	inner = (UB *)Kmalloc(ext + 1);
	if ( inner == NULL ) {
		return;
	}
	knl_memcpy(inner, *p_buf + at, (INT)ext);
	ilen = ext;
	cp_json_drop(&inner, &ilen, key);
	cp_splice(p_buf, p_len, (SZ)at, ext, inner, ilen);
	Kfree(inner);
}

/* A string value, quoted and with what must be escaped escaped */
LOCAL SZ cp_json_quote( UB *out, SZ max, CONST UB *s )
{
	SZ	n = 0;
	INT	k;

	out[n++] = '"';
	for ( k = 0; s[k] != 0 && n + 3 < max; k++ ) {
		if ( s[k] == '"' || s[k] == '\\' ) {
			out[n++] = '\\';
		}
		out[n++] = s[k];
	}
	out[n++] = '"';

	return n;
}

/* Now, as the metadata writes a time: "2026-09-24T10:03:12.345Z" */
LOCAL SZ cp_now( UB *out )
{
	SYSTIM	tim;
	UD	ms, secs, days;
	D	z, era, doe, yoe, y, doy, mp, d, m;
	INT	hh, mi, ss, ml, i;
	char	txt[32];
	INT	n = 0;

	if ( tk_get_tim(&tim) < E_OK ) {
		tim.hi = 0;
		tim.lo = 0;
	}
	/* system time counts from 1985; the date arithmetic below from 1970 */
	ms = (((UD)(UW)tim.hi << 32) | tim.lo) + 473385600000ULL;
	secs = ms / 1000;
	ml = (INT)( ms % 1000 );
	days = secs / 86400;
	ss = (INT)( secs % 60 );
	mi = (INT)( ( secs / 60 ) % 60 );
	hh = (INT)( ( secs / 3600 ) % 24 );

	/* days since 1970 as a date in the proleptic Gregorian calendar */
	z = (D)days + 719468;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = ( doe - doe / 1460 + doe / 36524 - doe / 146096 ) / 365;
	y = yoe + era * 400;
	doy = doe - ( 365 * yoe + yoe / 4 - yoe / 100 );
	mp = ( 5 * doy + 2 ) / 153;
	d = doy - ( 153 * mp + 2 ) / 5 + 1;
	m = ( mp < 10 ) ? mp + 3 : mp - 9;
	if ( m <= 2 ) {
		y++;
	}

	txt[n++] = '"';
	txt[n++] = (char)( '0' + (INT)( y / 1000 ) % 10 );
	txt[n++] = (char)( '0' + (INT)( y / 100 ) % 10 );
	txt[n++] = (char)( '0' + (INT)( y / 10 ) % 10 );
	txt[n++] = (char)( '0' + (INT)y % 10 );
	txt[n++] = '-';
	txt[n++] = (char)( '0' + (INT)m / 10 );
	txt[n++] = (char)( '0' + (INT)m % 10 );
	txt[n++] = '-';
	txt[n++] = (char)( '0' + (INT)d / 10 );
	txt[n++] = (char)( '0' + (INT)d % 10 );
	txt[n++] = 'T';
	txt[n++] = (char)( '0' + hh / 10 );
	txt[n++] = (char)( '0' + hh % 10 );
	txt[n++] = ':';
	txt[n++] = (char)( '0' + mi / 10 );
	txt[n++] = (char)( '0' + mi % 10 );
	txt[n++] = ':';
	txt[n++] = (char)( '0' + ss / 10 );
	txt[n++] = (char)( '0' + ss % 10 );
	txt[n++] = '.';
	txt[n++] = (char)( '0' + ml / 100 );
	txt[n++] = (char)( '0' + ( ml / 10 ) % 10 );
	txt[n++] = (char)( '0' + ml % 10 );
	txt[n++] = 'Z';
	txt[n++] = '"';
	for ( i = 0; i < n; i++ ) {
		out[i] = (UB)txt[i];
	}

	return (SZ)n;
}

/* ---------------------------------------------------------------- the copy */

LOCAL INT cp_tree( CPMAP *m, CONST TS_UUID *src, BOOL root, CONST UB *name );

/*
 * One xmlTAD record of the object at map entry 'me' copied: its links
 * followed and copied first, then every copied identity in it changed to
 * the copy's, then written as the copy's record of the same number.
 */
LOCAL ER cp_record( CPMAP *m, INT me, INT recno, BOOL root, CONST UB *name )
{
	UB	*buf;
	SZ	len = 0, at = 0, s, e;
	TS_UUID	id;
	INT	i;
	ER	er;

	buf = om_obj_record(&m->from[me], recno, &len);
	if ( buf == NULL ) {
		return E_NOEXS;
	}

	/*
	 * What it links to: copied, when the copy is deep, and a link
	 * that cannot be is taken out; otherwise shared, and counted, so
	 * that each shared object can be told it is linked to once more.
	 */
	while ( cp_next_link(buf, len, at, &s, &e, &id) ) {
		if ( !m->deep ) {
			if ( ts_uuid_cmp(&id, &m->from[me]) != 0 ) {
				INT	k;

				for ( k = 0; k < m->nsh; k++ ) {
					if ( ts_uuid_cmp(&m->shared[k], &id) == 0 ) {
						break;
					}
				}
				if ( k == m->nsh && m->nsh < CP_MAX ) {
					m->shared[k] = id;
					m->nshared[k] = 0;
					m->nsh++;
				}
				if ( k < m->nsh ) {
					m->nshared[k]++;
				}
			}
			at = e;
			continue;
		}
		if ( cp_tree(m, &id, FALSE, NULL) < 0 ) {
			cp_drop_link(&buf, &len, s, e);
			at = s;
			continue;
		}
		at = e;
	}

	/* every identity the copy has made, changed to its copy's */
	for ( i = 0; i < m->n; i++ ) {
		if ( !m->bad[i] ) {
			cp_swap_id(buf, len, m->from_s[i], m->to_s[i]);
		}
	}
	if ( root && name != NULL ) {
		cp_set_filename(&buf, &len, name);
	}

	/* how many links in the copy point at each object of the copy */
	at = 0;
	while ( cp_next_link(buf, len, at, &s, &e, &id) ) {
		INT	k = cp_find_new(m, &id);

		if ( k >= 0 ) {
			m->refs[k]++;
		}
		at = e;
	}

	er = om_obj_record_put(&m->to[me], recno, OB_RT_TAD, 0, buf, len);
	Kfree(buf);

	return er;
}

/* A record that is not xmlTAD: the same bytes, the same type */
LOCAL ER cp_bytes( CPMAP *m, INT me, CONST T_OBREC *rec )
{
	UB	*buf;
	SZ	len = 0;
	ER	er;

	buf = om_obj_record(&m->from[me], rec->recno, &len);
	if ( buf == NULL ) {
		return E_NOEXS;
	}
	er = om_obj_record_put(&m->to[me], rec->recno, rec->rt, rec->sub, buf, len);
	Kfree(buf);

	return er;
}

/* The resources beside the records, the same bytes under the copy */
LOCAL void cp_resources( CPMAP *m, INT me )
{
	UB	*names;
	INT	cnt, k, at = 0;

	names = (UB *)Kmalloc(CP_RES_LIST);
	if ( names == NULL ) {
		return;
	}
	cnt = om_obj_res_list(&m->from[me], names, CP_RES_LIST);
	for ( k = 0; k < cnt; k++ ) {
		CONST UB	*nm = names + at;
		UB		*data;
		SZ		size = 0;
		INT		l = 0;

		while ( nm[l] != 0 ) l++;
		if ( nm[0] == '.' ) {
			at += l + 1;
			continue;		/* the icon: made with the object (cp_make) */
		}
		data = om_obj_res(&m->from[me], nm, &size);
		if ( data != NULL ) {
			(void)om_obj_res_put(&m->to[me], nm, data, size);
			Kfree(data);
		}
		at += l + 1;
	}
	Kfree(names);
}

/*
 * The object the copy of 'src' will be: made now, with the original's
 * metadata, so that it has its identity before any record that links to
 * it is written. It counts as linked to by nothing until the copy is
 * done, so that a copy that fails can take it away again.
 */
LOCAL ER cp_make( CPMAP *m, INT me )
{
	T_OBCRE	c;
	UB	*meta, zero = '0';
	SZ	len = 0;
	ER	er;

	meta = om_obj_meta(&m->from[me], &len);
	if ( meta == NULL ) {
		return E_NOEXS;
	}
	cp_json_set(&meta, &len, "refCount", &zero, 1);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = meta;
	c.jsonsz = len;
	c.near = m->from[me];		/* on the original's volume */
	/* and its icon, as its metadata: made with it */
	c.icon = om_obj_icon(&m->from[me], &c.iconsz);
	er = ob_cre_obj(&c, &m->to[me]);
	if ( er == E_PAR && c.icon != NULL ) {
		/* an icon that is not one (a store that did not look) is left behind */
		Kfree((void *)c.icon);
		c.icon = NULL;
		c.iconsz = 0;
		er = ob_cre_obj(&c, &m->to[me]);
	}
	Kfree(meta);
	if ( c.icon != NULL ) {
		Kfree((void *)c.icon);
	}
	if ( er < E_OK ) {
		return er;
	}
	return ( ts_uuid_to_str(&m->to[me], m->to_s[me], CP_ID_LEN + 1) >= E_OK )
	       ? E_OK : E_OBJ;
}

/*
 * One object and everything it links to, copied. Answers its place in
 * the map, or an error. An object met again -- two links to it, or a
 * link back to something the copy is already inside -- is the copy
 * already made, which is what keeps a loop of links from copying for
 * ever.
 */
LOCAL INT cp_tree( CPMAP *m, CONST TS_UUID *src, BOOL root, CONST UB *name )
{
	T_OBREC	rec[OM_REC_MAX];
	T_OBREF	r;
	INT	me, i, n, got = 0;

	me = cp_find(m, src);
	if ( me >= 0 ) {
		return m->bad[me] ? E_NOEXS : me;
	}
	if ( m->n >= CP_MAX ) {
		return E_LIMIT;
	}
	/* only what is kept on a volume is copied */
	if ( ob_ref_obj(src, &r) < E_OK || r.type != OB_T_STORAGE || r.sub != OB_S_FILE ) {
		return E_NOEXS;
	}
	me = m->n;
	m->from[me] = *src;
	if ( ts_uuid_to_str(src, m->from_s[me], CP_ID_LEN + 1) < E_OK ) {
		return E_PAR;
	}
	m->refs[me] = 0;
	m->done[me] = FALSE;
	m->bad[me] = FALSE;
	if ( cp_make(m, me) < E_OK ) {
		return E_NOEXS;
	}
	m->n++;

	n = om_obj_records(src, rec, OM_REC_MAX);
	for ( i = 0; i < n; i++ ) {
		ER	er = ( rec[i].rt == OB_RT_TAD )
			   ? cp_record(m, me, rec[i].recno, root, name)
			   : cp_bytes(m, me, &rec[i]);

		if ( er < E_OK ) {
			break;			/* the records are numbered from 0 with no gap */
		}
		got++;
	}
	if ( got == 0 ) {
		m->bad[me] = TRUE;	/* met again, it is still not there */
		(void)ob_del_obj(&m->to[me]);
		return E_NOEXS;
	}
	cp_resources(m, me);
	m->done[me] = TRUE;

	return me;
}

/*
 * The metadata of one object of the copy: the copy's identity, name and
 * dates, and the count of links to it. What the original's metadata
 * says about who may reach it over a network or a port is not carried:
 * that was given to the original, not to whatever is made from it.
 */
LOCAL ER cp_meta( CPMAP *m, INT me, BOOL root, CONST UB *name )
{
	UB	*buf, val[TAD_NAME_MAX * 2 + 8];
	SZ	len = 0, n;
	ER	er;
	INT	refs = m->refs[me];
	char	num[12];
	INT	k = 0, i;

	buf = om_obj_meta(&m->to[me], &len);
	if ( buf == NULL ) {
		return E_NOEXS;
	}
	n = 0;
	val[n++] = '"';
	for ( i = 0; i < CP_ID_LEN; i++ ) {
		val[n++] = (UB)m->to_s[me][i];
	}
	val[n++] = '"';
	cp_json_set(&buf, &len, "realId", val, n);
	if ( root && name != NULL ) {
		n = cp_json_quote(val, sizeof(val), name);
		cp_json_set(&buf, &len, "name", val, n);
	}
	/*
	 * An object inside the copy is linked to from the copy. The root is
	 * linked to by nothing yet: the link the caller puts to it counts
	 * when the record it goes in is saved, as every link does.
	 */
	if ( refs <= 0 && !root ) {
		refs = 1;
	}
	if ( refs < 0 ) {
		refs = 0;
	}
	do {
		num[k++] = (char)( '0' + refs % 10 );
		refs /= 10;
	} while ( refs > 0 && k < 11 );
	for ( i = 0; i < k; i++ ) {
		val[i] = (UB)num[k - 1 - i];
	}
	cp_json_set(&buf, &len, "refCount", val, (SZ)k);
	n = cp_now(val);
	cp_json_set(&buf, &len, "makeDate", val, n);
	cp_json_set(&buf, &len, "updateDate", val, n);
	cp_json_set(&buf, &len, "accessDate", val, n);
	cp_json_drop(&buf, &len, "networkGrants");
	cp_json_drop(&buf, &len, "serialPorts");
	/* the file an import made the original from is the original's, not the copy's */
	cp_json_drop_in(&buf, &len, "tessronos", "file");

	er = om_obj_meta_put(&m->to[me], buf, len);
	Kfree(buf);

	return er;
}

/* An object that is linked to 'more' times more than it was (never below nought) */
LOCAL void cp_bump( CONST TS_UUID *id, INT more )
{
	for ( ; more > 0; more-- ) {
		if ( ob_lnk_obj(id) < E_OK ) break;
	}
	for ( ; more < 0; more++ ) {
		if ( ob_unl_obj(id) < E_OK ) break;
	}
}

EXPORT ER om_store_copy( CONST TS_UUID *src, CONST UB *name, BOOL deep,
			 TS_UUID *p_new )
{
	CPMAP	*m;
	INT	root, i;
	ER	er = E_OK;

	if ( src == NULL || p_new == NULL ) {
		return E_PAR;
	}
	m = (CPMAP *)Kmalloc(sizeof(CPMAP));
	if ( m == NULL ) {
		return E_NOMEM;
	}
	m->n = 0;
	m->deep = deep;
	m->nsh = 0;
	root = cp_tree(m, src, TRUE, name);
	if ( root < 0 ) {
		/* what was made on the way is taken away again */
		for ( i = 0; i < m->n; i++ ) {
			if ( !m->bad[i] ) {
				(void)ob_del_obj(&m->to[i]);
			}
		}
		Kfree(m);
		return (ER)root;
	}
	/* the metadata last, when every link to every object is counted */
	for ( i = 0; i < m->n; i++ ) {
		if ( m->done[i] ) {
			ER	e = cp_meta(m, i, (BOOL)( i == root ), name);

			if ( e < E_OK && i == root ) {
				er = e;
			}
		}
	}
	for ( i = 0; i < m->nsh && !om_store_counts(&m->to[root]); i++ ) {
		cp_bump(&m->shared[i], m->nshared[i]);
	}
	*p_new = m->to[root];
	Kfree(m);

	return er;
}

/* ---------------------------------------------------------------- references */

/*
 * How many links point at each object is kept in its metadata as
 * refCount, moved through the object layer (ob_lnk_obj, ob_unl_obj). It
 * is kept right by counting where links come and go: a record written
 * back is compared with what was there, and each object gains or loses
 * what it gained or lost in that record; an object thrown away takes its
 * links with it. An object no link points at any more is a 屑実身:
 * nothing reaches it, and only the list of such objects can bring it
 * back or throw it away for good.
 *
 * The count can still go wrong -- a record changed outside this system,
 * or a machine stopped between one write and the next -- so it can also
 * be counted again from nothing: every record read, every link in it
 * counted.
 */

/*
 * The maker of an object set to the user the caller acts for (a new
 * object made from a template is the user's, not the template's maker's).
 * Nothing is changed when the caller acts for no named user.
 */
EXPORT ER om_store_set_maker( CONST TS_UUID *id )
{
	T_OBCRD	*crd;
	UB	name[TAD_NAME_MAX], val[TAD_NAME_MAX * 2 + 8], *buf;
	SZ	len = 0, n;
	ER	er;

	crd = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	if ( crd == NULL ) {
		return E_NOMEM;
	}
	er = ob_get_crd(crd);
	if ( er >= E_OK && om_store_name(&crd->user, name, TAD_NAME_MAX) <= 0 ) {
		/* a user with no object of its own: the name ユーザ環境設定 gave */
		if ( wm_user_name(name, TAD_NAME_MAX) <= 0 ) {
			er = E_NOEXS;
		}
	}
	Kfree(crd);
	if ( er < E_OK ) {
		return er;
	}
	buf = om_obj_meta(id, &len);
	if ( buf == NULL ) {
		return E_NOEXS;
	}
	n = cp_json_quote(val, sizeof(val), name);
	cp_json_set(&buf, &len, "maker", val, n);
	er = om_obj_meta_put(id, buf, len);
	Kfree(buf);
	return er;
}

EXPORT BOOL om_store_counts( CONST TS_UUID *id )
{
	T_OBREF	r;

	return (BOOL)( id != NULL && ob_ref_obj(id, &r) >= E_OK
		    && ( r.flags & OB_F_AUTOREF ) != 0 );
}

/* An object's count moved by 'more'. It never goes below nought */
EXPORT void om_store_bump( CONST TS_UUID *id, INT more )
{
	if ( id != NULL && more != 0 ) {
		cp_bump(id, more);
	}
}

/* An object's count as its metadata gives it; -1 when it has none */
EXPORT INT om_store_refs( CONST TS_UUID *id )
{
	UB	*buf;
	SZ	len = 0, j;
	INT	at, refs = 0;

	if ( id == NULL ) {
		return -1;
	}
	buf = om_obj_meta(id, &len);
	if ( buf == NULL ) {
		return -1;
	}
	at = cp_json_value(buf, len, "refCount");
	if ( at < 0 ) {
		Kfree(buf);
		return -1;
	}
	for ( j = (SZ)at; j < len && buf[j] >= '0' && buf[j] <= '9'; j++ ) {
		refs = refs * 10 + ( buf[j] - '0' );
	}
	Kfree(buf);

	return refs;
}

/*
 * The objects the links of a record point at, one for every link --
 * an object linked to twice is there twice. At most 'max' are given;
 * what comes back is how many there were.
 */
EXPORT INT om_store_links( CONST UB *buf, SZ len, TS_UUID *ids, INT max )
{
	SZ	at = 0, s, e;
	TS_UUID	id;
	INT	n = 0;

	if ( buf == NULL ) {
		return 0;
	}
	while ( cp_next_link(buf, len, at, &s, &e, &id) ) {
		if ( ids != NULL && n < max ) {
			ids[n] = id;
		}
		n++;
		at = e;
	}

	return n;
}

#define RF_LINKS	512		/* links of one record that are counted */

/*
 * A record's text replaced: every object gains the links to it that
 * the new text has and the old did not, and loses those it had and has
 * no more. Either text may be missing -- a record written for the first
 * time, or one thrown away.
 */
EXPORT void om_store_relink( CONST UB *was, SZ was_len, CONST UB *now,
			     SZ now_len )
{
	TS_UUID	*a, *b;
	INT	na, nb, i, k;

	a = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * RF_LINKS * 2);
	if ( a == NULL ) {
		return;
	}
	b = a + RF_LINKS;
	na = om_store_links(was, was_len, a, RF_LINKS);
	nb = om_store_links(now, now_len, b, RF_LINKS);
	if ( na > RF_LINKS ) na = RF_LINKS;
	if ( nb > RF_LINKS ) nb = RF_LINKS;

	/* what the two have in common cancels, one link against one */
	for ( i = 0; i < na; i++ ) {
		for ( k = 0; k < nb; k++ ) {
			if ( ts_uuid_cmp(&a[i], &b[k]) == 0 ) {
				b[k] = b[--nb];
				a[i] = a[--na];
				i--;
				break;
			}
		}
	}
	for ( i = 0; i < na; i++ ) {
		cp_bump(&a[i], -1);
	}
	for ( k = 0; k < nb; k++ ) {
		cp_bump(&b[k], 1);
	}
	Kfree(a);
}

/*
 * Every object kept on the volumes, as the object layer lists them. At
 * most 'max' are given; what comes back is how many there are.
 */
EXPORT INT om_store_objects( TS_UUID *ids, INT max )
{
	TS_UUID	page[32];
	TS_UUID	last;
	INT	n = 0, cnt, i;
	BOOL	first = TRUE;

	for (;;) {
		if ( ob_lst_obj(OB_T_STORAGE, OB_S_FILE, first ? NULL : &last,
				page, 32, &cnt) < E_OK || cnt <= 0 ) {
			break;
		}
		for ( i = 0; i < cnt; i++ ) {
			if ( ids != NULL && n < max ) {
				ids[n] = page[i];
			}
			n++;
		}
		last = page[cnt - 1];
		first = FALSE;
		if ( cnt < 32 ) {
			break;
		}
	}
	return n;
}

/*
 * An object thrown away for good, with everything beside its records.
 * What its records linked to is linked to that much less. What the
 * store holds of it in memory is let go first.
 */
EXPORT ER om_store_delete( CONST TS_UUID *id )
{
	T_OBREC	rec[OM_REC_MAX];
	INT	n, i;

	if ( id == NULL ) {
		return E_PAR;
	}
	om_store_forget(id);
	n = om_obj_records(id, rec, OM_REC_MAX);
	if ( n < 0 ) {
		return E_NOEXS;
	}
	for ( i = 0; i < n && !om_store_counts(id); i++ ) {
		SZ	len = 0;
		UB	*buf;

		if ( rec[i].rt != OB_RT_TAD ) continue;
		buf = om_obj_record(id, rec[i].recno, &len);
		if ( buf != NULL ) {
			om_store_relink(buf, len, NULL, 0);
			Kfree(buf);
		}
	}
	/* nothing may still count it: it is being thrown away */
	while ( ob_unl_obj(id) >= E_OK ) ;

	return ob_del_obj(id);
}

/*
 * Every count made again from nothing: each record read, each link in
 * it counted against what it points at, and each of 'roots' counted
 * once more -- the objects the system itself holds, which no record
 * links to. The count of an object that was wrong is put right. Answers
 * how many objects there are and how many were wrong.
 */
EXPORT ER om_store_recount( CONST TS_UUID *roots, INT nroot, INT *p_total,
			    INT *p_changed )
{
	TS_UUID	*obj, *lk;
	INT	*cnt;
	INT	i, k, nobj, changed = 0;

	if ( p_total != NULL ) *p_total = 0;
	if ( p_changed != NULL ) *p_changed = 0;
	nobj = om_store_objects(NULL, 0);
	obj = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * ( (SZ)nobj + 1 ));
	cnt = (INT *)Kmalloc(sizeof(INT) * ( (SZ)nobj + 1 ));
	lk = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * RF_LINKS);
	if ( obj == NULL || cnt == NULL || lk == NULL ) {
		if ( obj != NULL ) Kfree(obj);
		if ( cnt != NULL ) Kfree(cnt);
		if ( lk != NULL ) Kfree(lk);
		return E_NOMEM;
	}
	nobj = om_store_objects(obj, nobj);
	for ( k = 0; k < nobj; k++ ) {
		cnt[k] = 0;
	}

	/* the links of every record; a store that counts them itself
	   keeps its own counts, and fsck.tsfs puts those right */
	for ( i = 0; i < nobj; i++ ) {
		T_OBREC	rec[OM_REC_MAX];
		INT	nr, r;

		if ( om_store_counts(&obj[i]) ) {
			continue;
		}
		nr = om_obj_records(&obj[i], rec, OM_REC_MAX);

		for ( r = 0; r < nr; r++ ) {
			UB	*buf;
			SZ	len = 0;
			INT	n, j;

			if ( rec[r].rt != OB_RT_TAD ) continue;
			buf = om_obj_record(&obj[i], rec[r].recno, &len);
			if ( buf == NULL ) continue;
			n = om_store_links(buf, len, lk, RF_LINKS);
			if ( n > RF_LINKS ) n = RF_LINKS;
			Kfree(buf);
			for ( j = 0; j < n; j++ ) {
				for ( k = 0; k < nobj; k++ ) {
					if ( ts_uuid_cmp(&lk[j], &obj[k]) == 0 ) {
						cnt[k]++;
						break;
					}
				}
			}
		}
	}
	for ( i = 0; roots != NULL && i < nroot; i++ ) {
		for ( k = 0; k < nobj; k++ ) {
			if ( ts_uuid_cmp(&roots[i], &obj[k]) == 0 ) {
				cnt[k]++;
				break;
			}
		}
	}

	/* and each count that was wrong, put right */
	for ( k = 0; k < nobj; k++ ) {
		INT	was;

		if ( om_store_counts(&obj[k]) ) {
			continue;
		}
		was = om_store_refs(&obj[k]);
		if ( was != cnt[k] ) {
			cp_bump(&obj[k], cnt[k] - ( ( was < 0 ) ? 0 : was ));
			changed++;
		}
	}
	if ( p_total != NULL ) *p_total = nobj;
	if ( p_changed != NULL ) *p_changed = changed;

	Kfree(obj);
	Kfree(cnt);
	Kfree(lk);

	return E_OK;
}

/*
 * A record read whole, as it was last written, with a nought after it.
 * The caller owns what comes back. For what looks through records
 * without opening them: the search, the network of links.
 */
EXPORT UB *om_store_read( CONST TS_UUID *id, INT recno, SZ *p_len )
{
	if ( id == NULL ) {
		return NULL;
	}
	return om_obj_record(id, ( recno < 0 ) ? 0 : recno, p_len);
}
