/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wifi_nvram.c (Raspberry Pi 5)
 *	The settings of the Wi-Fi's board read out of their document (design 9.6.8).
 *
 *	The settings (NVRAM) of the CYW43455 are kept as a document on the
 *	system volume (SYSDEF_FW_BWFM43455_NV, tools/rpi5/wifi/mkobj.py): a
 *	few paragraphs of notes, then each line of the text linux-firmware
 *	publishes as one paragraph, so that it is read and changed with the
 *	text editor. bwfm(4) asks for the text itself, the lines
 *	"name=value"; it drops the blanks and the comments and packs the
 *	lines for the chip itself. What is given to it here:
 *
 *	- each paragraph (<p>) is read for its text, the text of the
 *	  elements inside it included; a line break (<br/>) and the end of
 *	  the paragraph both end a line;
 *	- a paragraph that starts with WIFI_NV_NOTE is a note, not settings;
 *	- full-width letters, digits and signs (U+FF01 to U+FF5E) and the
 *	  full-width space are read as their ASCII ones, as an edit with the
 *	  text editor may leave them;
 *	- a line is trimmed; an empty one and one that starts with '#' are
 *	  skipped; the blanks around its first '=' are dropped;
 *	- a line with anything else than printable ASCII left, or without
 *	  '=', is said on the console and dropped.
 *
 *	A paragraph and a line may be as long as they are: the paragraph's
 *	text is read into room made twice as large as it fills, and a line
 *	into room of its own length.
 *
 *	tools/rpi5/wifi/nvram.py does the same on the host; the two are tested
 *	with the same documents (test_nvram.py, tests/ktest/ktest_wifinv.c).
 */

#include <sys/machine.h>

#if defined(USE_WIFI) && !defined(X64)

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/tad.h>
#include "tstdlib.h"

/* The mark a note's paragraph starts with: 【注記】 */
#define WIFI_NV_NOTE	"\xe3\x80\x90\xe6\xb3\xa8\xe8\xa8\x98\xe3\x80\x91"
#define WIFI_NV_PARA	4096		/* the first room for a paragraph's text */

typedef struct {
	UB	*out;
	SZ	max;
	SZ	len;
	INT	drop;			/* lines dropped as not settings */
	BOOL	full;
	BOOL	nomem;
	UB	*para;			/* room for pcap bytes */
	SZ	pcap;
	SZ	plen;
} NV;

/* One byte more of the paragraph, the room made twice as large when it is full */
LOCAL BOOL nv_put( NV *nv, UB c )
{
	UB	*p;
	SZ	cap;

	if ( nv->plen + 1 >= nv->pcap ) {
		cap = ( nv->pcap > 0 ) ? nv->pcap * 2 : WIFI_NV_PARA;
		p = (UB *)Krealloc(nv->para, (size_t)cap);
		if ( p == NULL ) {
			tm_printf((UB *)"wifi: nvram: no memory for a paragraph of %d bytes\n", (INT)cap);
			nv->nomem = TRUE;
			return FALSE;
		}
		nv->para = p;
		nv->pcap = cap;
	}
	nv->para[nv->plen++] = c;
	return TRUE;
}

/* The text of a node and of what is under it, line breaks as '\n' */
LOCAL void nv_text( NV *nv, CONST T_TADNODE *nd )
{
	CONST T_TADNODE	*c;
	CONST UB	*t;

	for ( c = nd->first; c != NULL && !nv->nomem; c = c->next ) {
		if ( c->kind == TAD_ND_TEXT && c->text != NULL ) {
			for ( t = c->text; *t != '\0'; t++ ) {
				if ( !nv_put(nv, ( *t == '\r' ) ? '\n' : *t) ) return;
			}
		} else if ( c->kind == TAD_ND_ELEM && c->name != NULL ) {
			if ( knl_strcmp((CONST char *)c->name, "br") == 0 ) {
				if ( !nv_put(nv, '\n') ) return;
			} else {
				nv_text(nv, c);
			}
		}
	}
}

/*
 * One line [s, e) as the setting it is, put out; anything that is not one
 * is counted and said, an empty line and a comment are not
 */
LOCAL void nv_line( NV *nv, CONST UB *s, CONST UB *e )
{
	UB	*ln;
	INT	n = 0, i, k, eq = -1;
	BOOL	bad = FALSE;
	CONST UB *s0 = s;

	/* what is put in ln is never longer than the line it comes from */
	ln = (UB *)Kmalloc((size_t)( e - s ) + 1);
	if ( ln == NULL ) {
		tm_printf((UB *)"wifi: nvram: no memory for a line of %d bytes\n", (INT)( e - s ));
		nv->nomem = TRUE;
		return;
	}

	/* the full-width ones as ASCII; anything else above ASCII stays as a mark */
	while ( s < e ) {
		UB	c = *s;

		if ( c == 0xef && e - s >= 3 && ( s[1] == 0xbc || s[1] == 0xbd ) && s[2] >= 0x80 && s[2] <= 0xbf ) {
			/* U+FF00 to U+FF7F: EF BC 80..BF, EF BD 80..BF */
			UINT	u = 0xff00 + ( ( s[1] - 0xbc ) << 6 ) + ( s[2] - 0x80 );

			if ( u >= 0xff01 && u <= 0xff5e ) {
				c = (UB)( u - 0xfee0 );
				s += 3;
			} else {
				bad = TRUE;
				s++;
				continue;
			}
		} else if ( c == 0xe3 && e - s >= 3 && s[1] == 0x80 && s[2] == 0x80 ) {
			c = ' ';		/* U+3000 */
			s += 3;
		} else {
			s++;
		}
		if ( c == '\t' ) c = ' ';
		if ( c < 0x20 || c > 0x7e ) {
			bad = TRUE;
			continue;
		}
		ln[n++] = c;
	}
	/* trimmed */
	for ( i = 0; i < n && ln[i] == ' '; i++ ) ;
	for ( ; n > i && ln[n - 1] == ' '; n-- ) ;
	if ( n == i && !bad ) {
		Kfree(ln);
		return;				/* an empty line */
	}
	if ( n > i && ln[i] == '#' ) {
		Kfree(ln);
		return;				/* a comment */
	}
	for ( k = i; k < n; k++ ) {
		if ( ln[k] == '=' ) {
			eq = k;
			break;
		}
	}
	if ( bad || eq < 0 ) {
		nv->drop++;
		/* said as it was written */
		for ( n = 0; s0 + n < e; n++ ) ln[n] = s0[n];
		ln[n] = '\0';
		tm_printf((UB *)"wifi: nvram: \"%s\" is not a setting (%s): left out\n", ln,
			  bad ? "not ASCII" : "no '='");
		Kfree(ln);
		return;
	}
	/* name = value, without the blanks around '=' */
	{
		INT	ke = eq, vs = eq + 1;

		while ( ke > i && ln[ke - 1] == ' ' ) ke--;
		while ( vs < n && ln[vs] == ' ' ) vs++;
		if ( nv->len + ( ke - i ) + 1 + ( n - vs ) + 1 >= nv->max ) {
			nv->full = TRUE;
			Kfree(ln);
			return;
		}
		knl_memcpy(nv->out + nv->len, &ln[i], (SZ)( ke - i ));
		nv->len += ke - i;
		nv->out[nv->len++] = '=';
		knl_memcpy(nv->out + nv->len, &ln[vs], (SZ)( n - vs ));
		nv->len += n - vs;
		nv->out[nv->len++] = '\n';
	}
	Kfree(ln);
}

/* Whether the paragraph's text starts with the note's mark (blanks before it allowed) */
LOCAL BOOL nv_note( CONST NV *nv )
{
	SZ	i = 0, k, m = sizeof(WIFI_NV_NOTE) - 1;

	while ( i < nv->plen && ( nv->para[i] == ' ' || nv->para[i] == '\n' || nv->para[i] == '\t' ) ) i++;
	if ( nv->plen - i < m ) return FALSE;
	for ( k = 0; k < m; k++ ) {
		if ( nv->para[i + k] != (UB)WIFI_NV_NOTE[k] ) return FALSE;
	}
	return TRUE;
}

/*
 * The settings of a document (xmlTAD text of 'len' bytes) into 'out' (of
 * 'max' bytes), the lines "name=value\n" and a NUL after them. Answers
 * the bytes of the lines; *p_drop (when not NULL) the lines left out as
 * not settings. E_PAR: not a document; E_LIMIT: the settings do not fit
 * in 'out'; E_NOMEM.
 */
EXPORT INT knl_wifi_nvram( CONST UB *xml, SZ len, UB *out, SZ max, INT *p_drop )
{
	T_TAD		*doc = NULL;
	CONST T_TADNODE	*body, *p;
	NV		*nv;
	ER		er;
	SZ		a, b;
	INT		res;

	if ( xml == NULL || out == NULL || max < 1 ) {
		return E_PAR;
	}
	er = tad_parse(xml, len, NULL, &doc);
	if ( er < E_OK ) {
		return er;
	}
	body = tad_body(doc);
	if ( body == NULL ) {
		tad_free(doc);
		return E_PAR;
	}
	nv = (NV *)Kmalloc(sizeof(NV));
	if ( nv == NULL ) {
		tad_free(doc);
		return E_NOMEM;
	}
	knl_memset(nv, 0, sizeof(NV));
	nv->out = out;
	nv->max = max;
	for ( p = body->first; p != NULL && !nv->full && !nv->nomem; p = p->next ) {
		if ( p->kind != TAD_ND_ELEM || p->name == NULL || knl_strcmp((CONST char *)p->name, "p") != 0 ) {
			continue;
		}
		nv->plen = 0;
		nv_text(nv, p);
		if ( nv->nomem || nv_note(nv) ) {
			continue;
		}
		for ( a = 0; a <= nv->plen; a = b + 1 ) {
			for ( b = a; b < nv->plen && nv->para[b] != '\n'; b++ ) ;
			nv_line(nv, &nv->para[a], &nv->para[b]);
			if ( nv->full || nv->nomem ) break;
		}
	}
	tad_free(doc);
	if ( nv->para != NULL ) {
		Kfree(nv->para);
	}
	if ( nv->full || nv->nomem ) {
		res = nv->nomem ? E_NOMEM : E_LIMIT;
		Kfree(nv);
		return res;
	}
	out[nv->len] = '\0';
	if ( p_drop != NULL ) *p_drop = nv->drop;
	res = (INT)nv->len;
	Kfree(nv);
	return res;
}

#endif /* USE_WIFI && !X64 */
