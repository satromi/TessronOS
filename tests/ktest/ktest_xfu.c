/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_xfu.c
 *	The libraries of ファイル変換: text encodings, names of files, the
 *	FTP client, and trees of files taken in and given out (design 18.20)
 *
 *	The encodings go round trips and read known bytes. The FTP client
 *	talks to a server that is a script: each command it must send, each
 *	reply it gets, each data connection it must open. The converter
 *	takes in a tree kept in memory -- a Shift_JIS text, pictures, a
 *	directory, bytes, an xmlTAD file and two TADjs objects -- on the
 *	store on FAT and on a native volume, and gives it out again into
 *	another tree, where what was not changed comes back byte for byte.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/tsfs.h>
#include <ts/txc.h>
#include <ts/ftp.h>
#include <ts/xfu.h>
#include <ts/img.h>
#include <ts/json.h>
#include <ts/fs.h>

LOCAL INT s_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != 0 ) n++;
	return n;
}

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL same_str( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == (UB)b[i]; i++ ) ;
	return (BOOL)( a[i] == (UB)b[i] );
}

LOCAL BOOL has( CONST UB *s, INT n, CONST char *w )
{
	INT	i, k;

	for ( i = 0; i < n; i++ ) {
		for ( k = 0; w[k] != 0 && i + k < n && s[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == 0 ) return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------- encodings */

/* 日本語 in each encoding */
LOCAL CONST char u_nihongo[] = "日本語";
LOCAL CONST char s_nihongo[] = "\x93\xfa\x96\x7b\x8c\xea";
LOCAL CONST char e_nihongo[] = "\xc6\xfc\xcb\xdc\xb8\xec";
LOCAL CONST char j_nihongo[] = "\x1b\x24\x42\x46\x7c\x4b\x5c\x38\x6c\x1b\x28\x42";

/* in, turned into enc and back: the bytes expected, and the same text again */
LOCAL void round( INT enc, CONST char *utf, CONST char *want, INT wn, INT wbad )
{
	UB	out[128], back[128];
	SZ	n, m;
	INT	bad = -1;

	n = txc_from_utf8(enc, (CONST UB *)utf, s_len(utf), out, sizeof(out), &bad);
	KT_ASSERT_EQ(bad, wbad);
	if ( want != NULL ) {
		KT_ASSERT_EQ(n, wn);
		KT_ASSERT(same_bytes(out, (CONST UB *)want, wn));
	}
	if ( wbad == 0 ) {
		m = txc_to_utf8(enc, out, n, back, sizeof(back), &bad);
		KT_ASSERT_EQ(bad, 0);
		KT_ASSERT_EQ(m, s_len(utf));
		KT_ASSERT(same_str(back, utf));
	}
}

LOCAL void test_txc_codec( void )
{
	UB	out[64];
	INT	bad = 0;
	SZ	n;

	round(TXC_SJIS, u_nihongo, s_nihongo, 6, 0);
	round(TXC_EUCJP, u_nihongo, e_nihongo, 6, 0);
	round(TXC_JIS, u_nihongo, j_nihongo, 12, 0);
	round(TXC_UTF8, u_nihongo, u_nihongo, 9, 0);

	/* the NEC and IBM extensions, half width katakana, a mark of row 13 */
	round(TXC_SJIS, "①", "\x87\x40", 2, 0);
	round(TXC_SJIS, "髙", "\xfb\xfc", 2, 0);		/* the IBM row, not the NEC copy */
	round(TXC_EUCJP, "髙", NULL, 0, 0);			/* the NEC copy: rows 1 to 94 only */
	round(TXC_SJIS, "ｱ", "\xb1", 1, 0);
	round(TXC_EUCJP, "ｱ", "\x8e\xb1", 2, 0);
	round(TXC_JIS, "aｱb", "a\x1b(I1\x1b(Bb", 9, 0);
	round(TXC_SJIS, "A\\~", "A\\~", 3, 0);
	round(TXC_UTF16LE, "aあ", "a\0\x42\x30", 4, 0);

	/* the JIS forms of the Windows code page's characters are taken too */
	n = txc_from_utf8(TXC_SJIS, (CONST UB *)"〜", 3, out, sizeof(out), &bad);
	KT_ASSERT(n == 2 && bad == 0 && same_bytes(out, (CONST UB *)"\x81\x60", 2));
	n = txc_to_utf8(TXC_SJIS, (CONST UB *)"\x81\x60", 2, out, sizeof(out), NULL);
	KT_ASSERT(n == 3 && same_str(out, "～"));

	/* what has no place becomes 〓, counted */
	round(TXC_SJIS, "a😀b", "a\x81\xac" "b", 4, 1);
	round(TXC_EUCJP, "Ω☃", NULL, 0, 1);
	n = txc_to_utf8(TXC_SJIS, (CONST UB *)"\x81\x20z", 3, out, sizeof(out), &bad);
	KT_ASSERT_EQ(bad, 1);
	KT_ASSERT(has(out, (INT)n, "〓"));

	/* too little room: whole characters, a nought, and the length the whole takes */
	n = txc_to_utf8(TXC_SJIS, (CONST UB *)s_nihongo, 6, out, 5, NULL);
	KT_ASSERT_EQ(n, 9);
	KT_ASSERT(same_str(out, "日"));

	/* JIS X 0212, from lib/libbpk's table when it is linked: 丂 is row 16, cell 1 */
	if ( txc_has_x0212() ) {
		round(TXC_EUCJP, "丂", "\x8f\xb0\xa1", 3, 0);
		round(TXC_JIS, "a丂", "a\x1b$(D0!\x1b(B", 10, 0);
		round(TXC_JIS, "丂日", "\x1b$(D0!\x1b$BF|\x1b(B", 14, 0);
		round(TXC_SJIS, "丂", "\x81\xac", 2, 1);	/* Shift_JIS has no place for it */
	} else {
		n = txc_to_utf8(TXC_EUCJP, (CONST UB *)"\x8f\xb0\xa1", 3, out, sizeof(out), &bad);
		KT_ASSERT(bad == 1 && same_str(out, "〓"));
		tm_printf((UB *)"  JIS X 0212: no table linked\n");
	}

	KT_ASSERT_EQ(txc_by_name((CONST UB *)"Shift_JIS"), TXC_SJIS);
	KT_ASSERT_EQ(txc_by_name((CONST UB *)"cp932"), TXC_SJIS);
	KT_ASSERT_EQ(txc_by_name((CONST UB *)"EUC-JP"), TXC_EUCJP);
	KT_ASSERT(same_str((CONST UB *)txc_name(TXC_JIS), "iso-2022-jp"));
	KT_ASSERT_EQ(txc_jis_char(4, 2), 0x3042);	/* あ */
	KT_ASSERT_EQ(txc_sjis_code(0x3042), 0x82A0);
}

LOCAL void test_txc_detect( void )
{
	CONST char	*u = "こんにちは、世界。テストです。漢字かな交じり文";
	UB		b[128];
	SZ		n;

	n = txc_from_utf8(TXC_SJIS, (CONST UB *)u, s_len(u), b, sizeof(b), NULL);
	KT_ASSERT_EQ(txc_detect(b, n), TXC_SJIS);
	n = txc_from_utf8(TXC_EUCJP, (CONST UB *)u, s_len(u), b, sizeof(b), NULL);
	KT_ASSERT_EQ(txc_detect(b, n), TXC_EUCJP);
	n = txc_from_utf8(TXC_JIS, (CONST UB *)u, s_len(u), b, sizeof(b), NULL);
	KT_ASSERT_EQ(txc_detect(b, n), TXC_JIS);
	KT_ASSERT_EQ(txc_detect((CONST UB *)u, s_len(u)), TXC_UTF8);
	KT_ASSERT_EQ(txc_detect((CONST UB *)"plain ascii", 11), TXC_UTF8);
	KT_ASSERT_EQ(txc_detect((CONST UB *)"\xff\xfe" "a\0b\0", 6), TXC_UTF16LE);
	KT_ASSERT_EQ(txc_detect((CONST UB *)"a\0b\0c\0d\0", 8), TXC_UTF16LE);
	KT_ASSERT_EQ(txc_detect((CONST UB *)"\xef\xbb\xbf" "abc", 6), TXC_UTF8);

	/* a text cut in the middle of a character still reads as what it is */
	n = txc_from_utf8(TXC_SJIS, (CONST UB *)u, s_len(u), b, sizeof(b), NULL);
	KT_ASSERT_EQ(txc_detect(b, n - 1), TXC_SJIS);
	KT_ASSERT_EQ(txc_detect((CONST UB *)u, s_len(u) - 1), TXC_UTF8);

	/* a byte order mark is not text */
	n = txc_to_utf8(TXC_AUTO, (CONST UB *)"\xef\xbb\xbf" "abc", 6, b, sizeof(b), NULL);
	KT_ASSERT(n == 3 && same_str(b, "abc"));
}

/* ---------------------------------------------------------------- names */

LOCAL void test_names( void )
{
	UB		out[XFU_NAME_MAX], *big;
	T_XFUNAMES	s;
	INT		n, i;

	n = xfu_name_out((CONST UB *)"a:b*c?\"<>|/\\.txt", out, sizeof(out));
	KT_ASSERT(same_str(out, "a：b＊c？＂＜＞｜／＼.txt"));
	KT_ASSERT_EQ(n, s_len("a：b＊c？＂＜＞｜／＼.txt"));
	(void)xfu_name_in(out, out, sizeof(out));
	KT_ASSERT(same_str(out, "a:b*c?\"<>|/\\.txt"));

	(void)xfu_name_out((CONST UB *)"name. . ", out, sizeof(out));
	KT_ASSERT(same_str(out, "name"));
	(void)xfu_name_out((CONST UB *)"con.txt", out, sizeof(out));
	KT_ASSERT(same_str(out, "con_.txt"));
	(void)xfu_name_out((CONST UB *)"COM3", out, sizeof(out));
	KT_ASSERT(same_str(out, "COM3_"));
	(void)xfu_name_out((CONST UB *)"console", out, sizeof(out));
	KT_ASSERT(same_str(out, "console"));
	(void)xfu_name_out((CONST UB *)"", out, sizeof(out));
	KT_ASSERT(same_str(out, "_"));
	(void)xfu_name_out((CONST UB *)"a\x01" "b", out, sizeof(out));
	KT_ASSERT(same_str(out, "a␁b"));

	/* long: within 255 UTF-16 units, the extension kept */
	big = (UB *)Kmalloc(2048);
	if ( big == NULL ) KT_SKIP("no memory");
	for ( i = 0; i < 300; i++ ) {
		big[i * 3] = 0xE3;  big[i * 3 + 1] = 0x81;  big[i * 3 + 2] = 0x82;	/* あ */
	}
	knl_strcpy((char *)big + 900, ".txt");
	n = xfu_name_out(big, out, sizeof(out));
	KT_ASSERT_EQ(n, 251 * 3 + 4);
	KT_ASSERT(same_str(out + n - 4, ".txt"));
	/* and within 63 bytes where the file layer allows no more */
	n = xfu_name_out(big, out, 64);
	KT_ASSERT(n <= 63 && n == 19 * 3 + 4);
	Kfree(big);

	/* one name a directory: A to Z the same, a number put in */
	xfu_names_init(&s);
	KT_ASSERT_ER(xfu_names_add(&s, (CONST UB *)"a.txt"), E_OK);
	knl_strcpy((char *)out, "A.TXT");
	KT_ASSERT_ER(xfu_names_unique(&s, out, sizeof(out), 0), E_OK);
	KT_ASSERT(same_str(out, "A (2).TXT"));
	knl_strcpy((char *)out, "a.txt");
	KT_ASSERT_ER(xfu_names_unique(&s, out, sizeof(out), 0), E_OK);
	KT_ASSERT(same_str(out, "a (3).txt"));
	knl_strcpy((char *)out, "b");
	KT_ASSERT_ER(xfu_names_unique(&s, out, sizeof(out), 0), E_OK);
	KT_ASSERT(same_str(out, "b"));
	KT_ASSERT(xfu_names_has(&s, (CONST UB *)"B"));
	/* a number that makes the name too long cuts the stem, not the extension */
	knl_strcpy((char *)out, "abcdefgh.txt");
	KT_ASSERT_ER(xfu_names_unique(&s, out, sizeof(out), 12), E_OK);
	knl_strcpy((char *)out, "abcdefgh.txt");
	KT_ASSERT_ER(xfu_names_unique(&s, out, sizeof(out), 12), E_OK);
	KT_ASSERT(same_str(out, "abcd (2).txt"));
	xfu_names_free(&s);

	/* times */
	KT_ASSERT_EQ(xfu_time_iso(xfu_time_parse((CONST UB *)"2026-09-27T10:15:30Z"), out), 20);
	KT_ASSERT(same_str(out, "2026-09-27T10:15:30Z"));
	KT_ASSERT_EQ(xfu_time_parse((CONST UB *)"1985-01-01T00:01:00.5+00:00"), 60);
	KT_ASSERT_EQ(xfu_time_parse((CONST UB *)"1985-01-01T09:00:00+09:00"), 0);
}

/* Two GIFs: 3 by 3 with colour 2 transparent, and 9 by 10 interlaced */
LOCAL CONST UB gif33[] = {
	0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x03, 0x00, 0x03, 0x00, 0x81, 0x00, 0x00, 0xFF, 0x00, 0x00,
	0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x21, 0xF9, 0x04, 0x01, 0x00, 0x00, 0x02,
	0x00, 0x21, 0xFE, 0x03, 0x68, 0x69, 0x21, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03,
	0x00, 0x00, 0x02, 0x04, 0x44, 0x34, 0x86, 0x50, 0x00, 0x3B,
};

LOCAL CONST UB gif910[] = {
	0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x09, 0x00, 0x0A, 0x00, 0x81, 0x00, 0x00, 0xFF, 0x00, 0x00,
	0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x21, 0xFE, 0x03, 0x68, 0x69, 0x21, 0x00,
	0x2C, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x40, 0x02, 0x11, 0x44, 0x34, 0x86, 0xA0,
	0x79, 0xD9, 0xE2, 0x7A, 0x6A, 0x5A, 0x77, 0xA9, 0xC6, 0x5C, 0x4F, 0xDE, 0x14, 0x00, 0x3B,
};


LOCAL void test_gif( void )
{
	UW	*px = NULL;
	INT	w = 0, h = 0, x, y, i;
	CONST UW pal[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };

	KT_ASSERT_ER(img_size(gif33, sizeof(gif33), &w, &h), E_OK);
	KT_ASSERT(w == 3 && h == 3);
	KT_ASSERT_ER(img_decode(gif33, sizeof(gif33), &px, &w, &h), E_OK);
	if ( px != NULL ) {
		for ( i = 0; i < 9; i++ ) {
			INT	c = i % 4;

			KT_ASSERT_EQ(px[i], ( c == 2 ) ? IMG_CLEAR : pal[c]);
		}
		Kfree(px);
		px = NULL;
	}
	KT_ASSERT_ER(img_decode(gif910, sizeof(gif910), &px, &w, &h), E_OK);
	KT_ASSERT(w == 9 && h == 10);
	if ( px != NULL ) {
		for ( y = 0; y < 10; y++ ) {
			for ( x = 0; x < 9; x++ ) KT_ASSERT_EQ(px[y * 9 + x], pal[( x + y * 3 ) % 4]);
		}
		Kfree(px);
	}
	/* cut short: refused, or what there is */
	KT_ASSERT(img_decode(gif33, 20, &px, &w, &h) < E_OK);
}

LOCAL void test_png( void )
{
	UINT	px[6] = { 0xFF0000, 0x00FF00, 0x0000FF, 0x123456, 0xFFFFFF, 0x000000 };
	UW	*back = NULL;
	UB	*png = NULL;
	SZ	len = 0;
	INT	w = 0, h = 0, i;

	KT_ASSERT_ER(xfu_png_encode(px, 3, 2, &png, &len), E_OK);
	if ( png == NULL ) return;
	KT_ASSERT_ER(xfu_png_size(png, len, &w, &h), E_OK);
	KT_ASSERT(w == 3 && h == 2);
	KT_ASSERT_ER(img_png_decode(png, len, &back, &w, &h), E_OK);
	if ( back != NULL ) {
		for ( i = 0; i < 6; i++ ) KT_ASSERT_EQ(back[i] & 0xFFFFFF, px[i]);
		Kfree(back);
	}
	KT_ASSERT_EQ(xfu_crc32(0, (CONST UB *)"123456789", 9), 0xCBF43926);
	Kfree(png);
}

/* ---------------------------------------------------------------- a server in a script */

/*
 * Each step: 'S' a line the client must send, 'R' a reply it reads,
 * 'O' a data connection it opens to that port, 'D' what the data
 * connection gives, 'W' what the client must have written on it when
 * it closes it. The active way: 'L' the client listens and is given
 * "a.b.c.d:port" as where, 'A' the server's connection to it is taken
 * ("A:-" when none comes).
 */
typedef struct {
	CONST char * CONST *step;
	INT		n, at;
	SZ		off;
	INT		piece;		/* bytes a read gives at most */
	UB		line[512];
	INT		ln;
	UB		wbuf[256];
	INT		wn;
	INT		fails;
	INT		failat;
	INT		deof;		/* reads of the data connection past its end */
	BOOL		lsn;		/* the listening end (3) is open */
} FAKE;

LOCAL void fk_fail( FAKE *k )
{
	if ( k->fails++ == 0 ) k->failat = k->at;
}

LOCAL INT fk_open( void *ctx, CONST char *host, UINT port, INT peer )
{
	FAKE		*k = (FAKE *)ctx;
	CONST char	*s;
	UINT		p = 0;
	INT		i;

	if ( peer < 0 ) {
		return ( host != NULL ) ? 1 : E_PAR;
	}
	s = ( k->at < k->n ) ? k->step[k->at] : "";
	for ( i = 2; s[i] >= '0' && s[i] <= '9'; i++ ) p = p * 10 + ( s[i] - '0' );
	if ( s[0] != 'O' || p != port || peer != 1 || host != NULL ) {
		fk_fail(k);
		return E_IO;
	}
	k->at++;
	k->off = 0;
	k->wn = 0;
	k->deof = 0;
	return 2;
}

LOCAL INT fk_send( void *ctx, INT c, CONST void *buf, SZ n )
{
	FAKE		*k = (FAKE *)ctx;
	CONST UB	*b = (CONST UB *)buf;
	SZ		i;

	for ( i = 0; i < n; i++ ) {
		if ( c == 2 ) {
			if ( k->wn < (INT)sizeof(k->wbuf) ) k->wbuf[k->wn++] = b[i];
			continue;
		}
		if ( k->ln < (INT)sizeof(k->line) - 1 ) k->line[k->ln++] = b[i];
		if ( k->ln >= 2 && k->line[k->ln - 2] == '\r' && k->line[k->ln - 1] == '\n' ) {
			CONST char	*s = ( k->at < k->n ) ? k->step[k->at] : "";

			k->line[k->ln - 2] = 0;
			if ( s[0] != 'S' || !same_str(k->line, s + 2) ) {
				tm_printf((UB *)"  fake: sent \"%s\" at step %d\n", k->line, k->at);
				fk_fail(k);
			}
			k->at++;
			k->off = 0;
			k->ln = 0;
		}
	}
	return (INT)n;
}

LOCAL INT fk_recv( void *ctx, INT c, void *buf, SZ n )
{
	FAKE		*k = (FAKE *)ctx;
	CONST char	*s = ( k->at < k->n ) ? k->step[k->at] : "";
	SZ		len, give;

	if ( ( c == 1 && s[0] != 'R' ) || ( c == 2 && s[0] != 'D' ) ) {
		/* the other side closed: the end once, a fault after it, as a socket may */
		if ( c == 2 ) return ( k->deof++ == 0 ) ? 0 : E_IO;
		tm_printf((UB *)"  fake: a reply read at step %d\n", k->at);
		fk_fail(k);
		return E_TMOUT;
	}
	len = (SZ)s_len(s + 2) - k->off;
	give = ( len < n ) ? len : n;
	if ( give > (SZ)k->piece ) give = k->piece;
	knl_memcpy(buf, s + 2 + k->off, give);
	k->off += give;
	if ( k->off >= (SZ)s_len(s + 2) ) {
		k->at++;
		k->off = 0;
	}
	return (INT)give;
}

/* Where the client listens: "a.b.c.d:port" of the step */
LOCAL INT fk_listen( void *ctx, INT peer, UB addr[4], UINT *p_port )
{
	FAKE		*k = (FAKE *)ctx;
	CONST char	*s = ( k->at < k->n ) ? k->step[k->at] : "";
	UINT		v = 0;
	INT		i, n = 0;

	if ( s[0] != 'L' || peer != 1 || k->lsn ) {
		fk_fail(k);
		return E_IO;
	}
	for ( i = 2; s[i] != 0; i++ ) {
		if ( s[i] >= '0' && s[i] <= '9' ) {
			v = v * 10 + (UINT)( s[i] - '0' );
		} else {
			if ( n < 4 ) addr[n++] = (UB)v;
			v = 0;
		}
	}
	*p_port = v;
	k->at++;
	k->lsn = TRUE;
	return 3;
}

LOCAL INT fk_accept( void *ctx, INT l, INT tmo )
{
	FAKE		*k = (FAKE *)ctx;
	CONST char	*s = ( k->at < k->n ) ? k->step[k->at] : "";

	if ( s[0] != 'A' || l != 3 || !k->lsn || tmo <= 0 ) {
		fk_fail(k);
		return E_IO;
	}
	k->at++;
	if ( s[2] == '-' ) {
		return E_TMOUT;			/* the server never came */
	}
	k->off = 0;
	k->wn = 0;
	k->deof = 0;
	return 2;
}

LOCAL void fk_close( void *ctx, INT c )
{
	FAKE		*k = (FAKE *)ctx;
	CONST char	*s = ( k->at < k->n ) ? k->step[k->at] : "";

	if ( c == 3 ) {
		k->lsn = FALSE;
		return;
	}
	if ( c != 2 ) {
		return;
	}
	if ( s[0] == 'W' ) {
		if ( k->wn != s_len(s + 2) || !same_bytes(k->wbuf, (CONST UB *)s + 2, k->wn) ) {
			fk_fail(k);
		}
		k->at++;
	}
	k->wn = 0;
}

LOCAL void fk_init( FAKE *k, T_FTPNET *net, CONST char * CONST *step, INT n, INT piece )
{
	knl_memset(k, 0, sizeof(*k));
	k->step = step;
	k->n = n;
	k->piece = piece;
	net->ctx = k;
	net->open = fk_open;
	net->send = fk_send;
	net->recv = fk_recv;
	net->close = fk_close;
	net->listen = fk_listen;
	net->accept = fk_accept;
}

#define NSTEP(a)	( (INT)( sizeof(a) / sizeof(a[0]) ) )

/* A server of today: UTF-8, MLSD, EPSV, MFMT */
LOCAL CONST char * CONST script_new[] = {
	"R:220-Welcome\r\n220-to the test\r\n220 ready\r\n",
	"S:USER anonymous",
	"R:331 password please\r\n",
	"S:PASS anonymous@",
	"R:230 in\r\n",
	"S:SYST",
	"R:215 UNIX Type: L8\r\n",
	"S:FEAT",
	"R:211-Features:\r\n MDTM\r\n MLST type*;size*;modify*;\r\n SIZE\r\n UTF8\r\n EPSV\r\n MFMT\r\n211 End\r\n",
	"S:OPTS UTF8 ON",
	"R:200 ok\r\n",
	"S:TYPE I",
	"R:200 binary\r\n",
	"S:PWD",
	"R:257 \"/home/ta\"\"ro\" is here\r\n",
	"S:CWD pub",
	"R:250 ok\r\n",
	"S:EPSV",
	"R:229 Entering Extended Passive Mode (|||40001|)\r\n",
	"O:40001",
	"S:MLSD",
	"R:150 here\r\n",
	"D:type=cdir;modify=20260101000000; .\r\ntype=file;size=12;modify=20260927101500; 日本語.txt\r\n"
	  "type=dir;modify=20250102030405; sub dir\r\n",
	"R:226 done\r\n",
	"S:EPSV",
	"R:229 ok (|||40002|)\r\n",
	"O:40002",
	"S:RETR 日本語.txt",
	"R:150 open\r\n",
	"D:hello world\n",
	"R:226 done\r\n",
	"S:EPSV",
	"R:229 (|||40003|)\r\n",
	"O:40003",
	"S:STOR up.bin",
	"R:150 ok\r\n",
	"W:0123456789",
	"R:226 ok\r\n",
	"S:SIZE nothere",
	"R:550 No such file\r\n",
	"S:SIZE up.bin",
	"R:213 10\r\n",
	"S:MDTM 日本語.txt",
	"R:213 20260927101500\r\n",
	"S:MFMT 20260927101500 up.bin",
	"R:213 Modify=20260927101500; up.bin\r\n",
	"S:RNFR up.bin",
	"R:350 ready\r\n",
	"S:RNTO down.bin",
	"R:250 ok\r\n",
	"S:MKD newdir",
	"R:257 \"newdir\" created\r\n",
	"S:DELE down.bin",
	"R:250 deleted\r\n",
	"S:QUIT",
	"R:221 bye\r\n",
};

typedef struct {
	INT	n;
	UB	name[4][64];
	BOOL	dir[4];
	D	size[4];
	D	mtime[4];
} SEEN;

LOCAL INT seen_ent( void *arg, CONST T_FTPENT *e )
{
	SEEN	*s = (SEEN *)arg;

	if ( s->n < 4 ) {
		(void)knl_strncpy((char *)s->name[s->n], (CONST char *)e->name, 63);
		s->name[s->n][63] = 0;
		s->dir[s->n] = e->dir;
		s->size[s->n] = e->size;
		s->mtime[s->n] = e->mtime;
	}
	s->n++;
	return 0;
}

LOCAL INT sink_write( void *ctx, CONST void *buf, SZ n )
{
	(void)ctx;
	(void)buf;
	return (INT)n;
}

LOCAL void test_ftp_new( void )
{
	T_FTP		*f;
	T_FTPNET	net;
	FAKE		k;
	SEEN		s;
	UB		b[64];
	D		v = 0;
	INT		n, got;

	f = (T_FTP *)Kmalloc(sizeof(T_FTP));
	if ( f == NULL ) KT_SKIP("no memory");
	(void)sink_write;
	fk_init(&k, &net, script_new, NSTEP(script_new), 5);
	KT_ASSERT_ER(ftp_open(f, &net, "ftp.example", 0, NULL, NULL, TXC_SJIS), E_OK);
	KT_ASSERT_EQ(f->feat, FTP_F_UTF8 | FTP_F_MLSD | FTP_F_SIZE | FTP_F_MDTM | FTP_F_MFMT | FTP_F_EPSV);
	KT_ASSERT_EQ(f->enc, TXC_UTF8);
	KT_ASSERT(same_str(f->syst, "UNIX Type: L8"));
	KT_ASSERT_ER(ftp_pwd(f, b, sizeof(b)), E_OK);
	KT_ASSERT(same_str(b, "/home/ta\"ro"));
	KT_ASSERT_ER(ftp_cwd(f, (CONST UB *)"pub"), E_OK);

	knl_memset(&s, 0, sizeof(s));
	KT_ASSERT_ER(ftp_list(f, NULL, seen_ent, &s), E_OK);
	KT_ASSERT_EQ(s.n, 2);
	KT_ASSERT(same_str(s.name[0], "日本語.txt") && !s.dir[0] && s.size[0] == 12);
	KT_ASSERT_EQ(s.mtime[0], xfu_time_parse((CONST UB *)"2026-09-27T10:15:00Z"));
	KT_ASSERT(same_str(s.name[1], "sub dir") && s.dir[1]);

	KT_ASSERT_ER(ftp_get(f, (CONST UB *)"日本語.txt"), E_OK);
	n = 0;
	while ( ( got = ftp_read(f, b + n, sizeof(b) - n) ) > 0 ) n += got;
	KT_ASSERT(n == 12 && same_bytes(b, (CONST UB *)"hello world\n", 12));
	KT_ASSERT_ER(ftp_end(f), E_OK);

	KT_ASSERT_ER(ftp_put(f, (CONST UB *)"up.bin"), E_OK);
	KT_ASSERT_EQ(ftp_write(f, "01234", 5), 5);
	KT_ASSERT_EQ(ftp_write(f, "56789", 5), 5);
	KT_ASSERT_ER(ftp_end(f), E_OK);

	KT_ASSERT_ER(ftp_size(f, (CONST UB *)"nothere", &v), E_NOEXS);
	KT_ASSERT_EQ(f->code, 550);
	KT_ASSERT_ER(ftp_size(f, (CONST UB *)"up.bin", &v), E_OK);
	KT_ASSERT_EQ(v, 10);
	KT_ASSERT_ER(ftp_mdtm(f, (CONST UB *)"日本語.txt", &v), E_OK);
	KT_ASSERT_EQ(v, xfu_time_parse((CONST UB *)"2026-09-27T10:15:00Z"));
	KT_ASSERT_ER(ftp_mfmt(f, (CONST UB *)"up.bin", v), E_OK);
	KT_ASSERT_ER(ftp_rename(f, (CONST UB *)"up.bin", (CONST UB *)"down.bin"), E_OK);
	KT_ASSERT_ER(ftp_mkd(f, (CONST UB *)"newdir"), E_OK);
	KT_ASSERT_ER(ftp_dele(f, (CONST UB *)"down.bin"), E_OK);
	/* a line end in a name would be a second command */
	KT_ASSERT_ER(ftp_dele(f, (CONST UB *)"x\r\nDELE y"), E_PAR);
	KT_ASSERT_ER(ftp_close(f), E_OK);
	KT_ASSERT_EQ(k.fails, 0);
	KT_ASSERT_EQ(k.at, k.n);
	if ( k.fails > 0 ) tm_printf((UB *)"  fake: first fault at step %d\n", k.failat);
	Kfree(f);
}

/* A server of old: no FEAT, no EPSV, names in Shift_JIS, listings as ls and dir print them */
LOCAL CONST char * CONST script_old[] = {
	"R:220 old ftpd\r\n",
	"S:USER taro",
	"R:331 pw\r\n",
	"S:PASS secret",
	"R:230 ok\r\n",
	"S:SYST",
	"R:215 Windows_NT\r\n",
	"S:FEAT",
	"R:500 unknown\r\n",
	"S:TYPE I",
	"R:200 ok\r\n",
	"S:EPSV",
	"R:500 no EPSV here\r\n",
	"S:PASV",
	"R:227 Entering Passive Mode (192,168,9,9,156,65)\r\n",
	"O:40001",
	"S:LIST",
	"R:150 list\r\n",
	"D:total 3\r\n-rw-r--r--   1 taro  users   1234 Sep 27  2025 \x8e\x91\x97\xbf.txt\r\n"
	  "drwxr-xr-x   2 taro  users   4096 Jan  3 10:20 dir one\r\n"
	  "lrwxrwxrwx 1 taro users 4 Mar  1  2024 ln -> target\r\n",
	"R:226 ok\r\n",
	"S:PASV",
	"R:227 =192,168,9,9,156,66\r\n",
	"O:40002",
	"S:LIST dos",
	"R:125 go\r\n",
	"D:09-27-26  10:15AM       <DIR>          \x83\x74\x83\x48\x83\x8b\x83\x5f\r\n"
	  "01-02-2025  01:05PM              5678 report.doc\r\n",
	"R:226 ok\r\n",
	"S:PASV",
	"R:227 (192,168,9,9,156,67)\r\n",
	"O:40003",
	"S:NLST",
	"R:150\r\n",
	"D:a.txt\r\nsub/b.txt\r\n",
	"R:226\r\n",
	"S:CWD \x8e\x91\x97\xbf",
	"R:250 ok\r\n",
	"S:QUIT",
	"R:221\r\n",
};

LOCAL CONST char * CONST script_deny[] = {
	"R:220 hi\r\n",
	"S:USER anonymous",
	"R:331 pw\r\n",
	"S:PASS anonymous@",
	"R:530 Login incorrect.\r\n",
};

LOCAL void test_ftp_old( void )
{
	T_FTP		*f;
	T_FTPNET	net;
	FAKE		k;
	SEEN		s;

	f = (T_FTP *)Kmalloc(sizeof(T_FTP));
	if ( f == NULL ) KT_SKIP("no memory");
	fk_init(&k, &net, script_old, NSTEP(script_old), 3);
	f->now = xfu_time_parse((CONST UB *)"2026-09-27T12:00:00Z");
	KT_ASSERT_ER(ftp_open(f, &net, "old.example", 21, (CONST UB *)"taro", (CONST UB *)"secret",
			      TXC_SJIS), E_OK);
	KT_ASSERT_EQ(f->feat, 0);
	KT_ASSERT_EQ(f->enc, TXC_SJIS);

	knl_memset(&s, 0, sizeof(s));
	KT_ASSERT_ER(ftp_list(f, NULL, seen_ent, &s), E_OK);
	KT_ASSERT(f->pasv);
	KT_ASSERT_EQ(s.n, 3);
	KT_ASSERT(same_str(s.name[0], "資料.txt") && !s.dir[0] && s.size[0] == 1234);
	KT_ASSERT_EQ(s.mtime[0], xfu_time_parse((CONST UB *)"2025-09-27T00:00:00Z"));
	KT_ASSERT(same_str(s.name[1], "dir one") && s.dir[1]);
	KT_ASSERT_EQ(s.mtime[1], xfu_time_parse((CONST UB *)"2026-01-03T10:20:00Z"));
	KT_ASSERT(same_str(s.name[2], "ln") && !s.dir[2]);

	knl_memset(&s, 0, sizeof(s));
	KT_ASSERT_ER(ftp_list(f, (CONST UB *)"dos", seen_ent, &s), E_OK);
	KT_ASSERT_EQ(s.n, 2);
	KT_ASSERT(same_str(s.name[0], "フォルダ") && s.dir[0]);
	KT_ASSERT_EQ(s.mtime[0], xfu_time_parse((CONST UB *)"2026-09-27T10:15:00Z"));
	KT_ASSERT(same_str(s.name[1], "report.doc") && !s.dir[1] && s.size[1] == 5678);
	KT_ASSERT_EQ(s.mtime[1], xfu_time_parse((CONST UB *)"2025-01-02T13:05:00Z"));

	knl_memset(&s, 0, sizeof(s));
	KT_ASSERT_ER(ftp_nlst(f, NULL, seen_ent, &s), E_OK);
	KT_ASSERT(s.n == 2 && same_str(s.name[0], "a.txt") && same_str(s.name[1], "b.txt"));

	KT_ASSERT_ER(ftp_cwd(f, (CONST UB *)"資料"), E_OK);
	KT_ASSERT_ER(ftp_close(f), E_OK);
	KT_ASSERT_EQ(k.fails, 0);
	KT_ASSERT_EQ(k.at, k.n);
	if ( k.fails > 0 ) tm_printf((UB *)"  fake: first fault at step %d\n", k.failat);

	/* refused: the password was wrong */
	fk_init(&k, &net, script_deny, NSTEP(script_deny), 64);
	KT_ASSERT_ER(ftp_open(f, &net, "deny.example", 0, NULL, NULL, TXC_UTF8), E_OACV);
	KT_ASSERT_EQ(f->code, 530);
	KT_ASSERT_EQ(f->ctl, -1);
	KT_ASSERT_EQ(k.fails, 0);
	Kfree(f);
}

/*
 * The active way: EPRT taken, then refused and PORT from then on, the
 * server's connection taken after the first reply, and a connection
 * that never came
 */
LOCAL CONST char * CONST script_active[] = {
	"R:220 ready\r\n",
	"S:USER anonymous",
	"R:331 pw\r\n",
	"S:PASS anonymous@",
	"R:230 in\r\n",
	"S:SYST",
	"R:215 UNIX Type: L8\r\n",
	"S:FEAT",
	"R:211-Features:\r\n SIZE\r\n211 End\r\n",
	"S:TYPE I",
	"R:200 binary\r\n",
	"L:10.0.2.15:40010",
	"S:EPRT |1|10.0.2.15|40010|",
	"R:200 EPRT ok\r\n",
	"S:RETR a.txt",
	"R:150 connecting\r\n",
	"A:",
	"D:abc\n",
	"R:226 done\r\n",
	"L:10.0.2.15:40011",
	"S:EPRT |1|10.0.2.15|40011|",
	"R:500 EPRT not understood\r\n",
	"S:PORT 10,0,2,15,156,75",
	"R:200 PORT ok\r\n",
	"S:STOR b.bin",
	"R:150 ok\r\n",
	"A:",
	"W:xyz",
	"R:226 ok\r\n",
	"L:192.168.1.20:1025",
	"S:PORT 192,168,1,20,4,1",
	"R:200 ok\r\n",
	"S:LIST",
	"R:125 go\r\n",
	"A:",
	"D:-rw-r--r--   1 ftp  ftp   3 Sep 27 10:00 a.txt\r\n",
	"R:226 ok\r\n",
	"L:192.168.1.20:1026",
	"S:PORT 192,168,1,20,4,2",
	"R:200 ok\r\n",
	"S:RETR late",
	"R:150 connecting\r\n",
	"A:-",
	"R:425 Can't open data connection\r\n",
	"L:192.168.1.20:1027",
	"S:PORT 192,168,1,20,4,3",
	"R:200 ok\r\n",
	"S:RETR nothere",
	"R:550 No such file\r\n",
	"S:QUIT",
	"R:221 bye\r\n",
};

LOCAL void test_ftp_active( void )
{
	T_FTP		*f;
	T_FTPNET	net;
	FAKE		k;
	SEEN		s;
	UB		b[16];
	INT		n, got;

	f = (T_FTP *)Kmalloc(sizeof(T_FTP));
	if ( f == NULL ) KT_SKIP("no memory");
	fk_init(&k, &net, script_active, NSTEP(script_active), 4);
	KT_ASSERT_ER(ftp_open(f, &net, "active.example", 0, NULL, NULL, TXC_UTF8), E_OK);
	f->active = TRUE;

	/* EPRT, the data taken on the connection the server made */
	KT_ASSERT_ER(ftp_get(f, (CONST UB *)"a.txt"), E_OK);
	n = 0;
	while ( ( got = ftp_read(f, b + n, sizeof(b) - n) ) > 0 ) n += got;
	KT_ASSERT(n == 4 && same_bytes(b, (CONST UB *)"abc\n", 4));
	KT_ASSERT_ER(ftp_end(f), E_OK);
	KT_ASSERT(!f->port);

	/* EPRT refused: PORT, and PORT from then on */
	KT_ASSERT_ER(ftp_put(f, (CONST UB *)"b.bin"), E_OK);
	KT_ASSERT_EQ(ftp_write(f, "xyz", 3), 3);
	KT_ASSERT_ER(ftp_end(f), E_OK);
	KT_ASSERT(f->port);
	knl_memset(&s, 0, sizeof(s));
	KT_ASSERT_ER(ftp_list(f, NULL, seen_ent, &s), E_OK);
	KT_ASSERT(s.n == 1 && same_str(s.name[0], "a.txt") && s.size[0] == 3);

	/* the server did not come: the wait's end, and its word on it read */
	KT_ASSERT_ER(ftp_get(f, (CONST UB *)"late"), E_TMOUT);
	KT_ASSERT_EQ(f->code, 425);
	KT_ASSERT_EQ(f->data, -1);
	KT_ASSERT_EQ(f->lsn, -1);
	/* refused at once: nothing waited for, the listening end let go */
	KT_ASSERT_ER(ftp_get(f, (CONST UB *)"nothere"), E_NOEXS);
	KT_ASSERT_EQ(f->lsn, -1);
	KT_ASSERT(!k.lsn);
	KT_ASSERT_ER(ftp_close(f), E_OK);
	KT_ASSERT_EQ(k.fails, 0);
	KT_ASSERT_EQ(k.at, k.n);
	if ( k.fails > 0 ) tm_printf((UB *)"  fake: first fault at step %d\n", k.failat);

	/* connections that cannot listen: the active way is not there */
	fk_init(&k, &net, script_deny, 3, 64);
	net.listen = NULL;
	f->ctl = 1;
	f->net = &net;
	f->data = -1;
	f->lsn = -1;
	f->active = TRUE;
	KT_ASSERT_ER(ftp_get(f, (CONST UB *)"x"), E_NOSPT);
	f->ctl = -1;
	Kfree(f);
}

/* ---------------------------------------------------------------- a tree in memory */

#define MT_MAX	48
#define MT_OPEN	4

typedef struct {
	UB	path[200];
	UINT	kind;
	UB	*b;
	SZ	n, cap;
	D	mtime;
} MTF;

typedef struct {
	MTF	f[MT_MAX];
	INT	nf;
	INT	oh[MT_OPEN];		/* the file each handle has, -1 none */
	SZ	pos[MT_OPEN];
} MTREE;

LOCAL INT mt_find( MTREE *m, CONST UB *path )
{
	INT	i;

	for ( i = 0; i < m->nf; i++ ) {
		if ( same_str(m->f[i].path, (CONST char *)path) ) return i;
	}
	return -1;
}

LOCAL INT mt_add( MTREE *m, CONST UB *path, UINT kind )
{
	INT	i = mt_find(m, path);

	if ( i >= 0 ) return i;
	if ( m->nf >= MT_MAX ) return E_LIMIT;
	i = m->nf++;
	knl_memset(&m->f[i], 0, sizeof(MTF));
	(void)knl_strncpy((char *)m->f[i].path, (CONST char *)path, sizeof(m->f[i].path) - 1);
	m->f[i].kind = kind;
	m->f[i].mtime = xfu_time_parse((CONST UB *)"2026-09-01T08:00:00Z");
	return i;
}

LOCAL ER mt_grow( MTF *f, SZ need )
{
	UB	*nb;
	SZ	cap = ( f->cap < 256 ) ? 256 : f->cap;

	if ( need <= f->cap ) return E_OK;
	while ( cap < need ) cap *= 2;
	nb = (UB *)Kmalloc(cap);
	if ( nb == NULL ) return E_NOMEM;
	if ( f->b != NULL ) {
		knl_memcpy(nb, f->b, f->n);
		Kfree(f->b);
	}
	f->b = nb;
	f->cap = cap;
	return E_OK;
}

LOCAL void mt_put( MTREE *m, CONST char *path, CONST void *b, SZ n )
{
	INT	i = mt_add(m, (CONST UB *)path, XFU_K_FILE);

	if ( i >= 0 && mt_grow(&m->f[i], n) >= E_OK ) {
		knl_memcpy(m->f[i].b, b, n);
		m->f[i].n = n;
	}
}

LOCAL void mt_free( MTREE *m )
{
	INT	i;

	for ( i = 0; i < m->nf; i++ ) {
		if ( m->f[i].b != NULL ) Kfree(m->f[i].b);
	}
	m->nf = 0;
}

LOCAL INT mt_open( void *ctx, CONST UB *path, UINT mode )
{
	MTREE	*m = (MTREE *)ctx;
	INT	h, i;

	for ( h = 0; h < MT_OPEN && m->oh[h] >= 0; h++ ) ;
	if ( h == MT_OPEN ) return E_LIMIT;
	i = ( mode == XFU_O_WRITE ) ? mt_add(m, path, XFU_K_FILE) : mt_find(m, path);
	if ( i < 0 ) return E_NOEXS;
	if ( m->f[i].kind != XFU_K_FILE ) return E_OBJ;
	if ( mode == XFU_O_WRITE ) m->f[i].n = 0;
	m->oh[h] = i;
	m->pos[h] = 0;
	return h;
}

LOCAL INT mt_read( void *ctx, INT h, void *buf, SZ n )
{
	MTREE	*m = (MTREE *)ctx;
	MTF	*f = &m->f[m->oh[h]];
	SZ	k = f->n - m->pos[h];

	if ( k > n ) k = n;
	if ( k > 1000 ) k = 1000;		/* in pieces, as a medium gives them */
	knl_memcpy(buf, f->b + m->pos[h], k);
	m->pos[h] += k;
	return (INT)k;
}

LOCAL INT mt_write( void *ctx, INT h, CONST void *buf, SZ n )
{
	MTREE	*m = (MTREE *)ctx;
	MTF	*f = &m->f[m->oh[h]];

	if ( mt_grow(f, f->n + n) < E_OK ) return E_NOMEM;
	knl_memcpy(f->b + f->n, buf, n);
	f->n += n;
	return (INT)n;
}

LOCAL ER mt_close( void *ctx, INT h )
{
	MTREE	*m = (MTREE *)ctx;

	m->oh[h] = -1;
	return E_OK;
}

/* The entry's name when it is right inside dir, or NULL */
LOCAL CONST UB *mt_inside( CONST UB *path, CONST UB *dir )
{
	INT	n = s_len((CONST char *)dir), i;

	if ( n > 0 ) {
		for ( i = 0; i < n; i++ ) {
			if ( path[i] != dir[i] ) return NULL;
		}
		if ( path[n] != '/' ) return NULL;
		path += n + 1;
	}
	for ( i = 0; path[i] != 0; i++ ) {
		if ( path[i] == '/' ) return NULL;
	}
	return ( path[0] != 0 ) ? path : NULL;
}

LOCAL ER mt_list( void *ctx, CONST UB *path, XFU_ENTCB cb, void *arg )
{
	MTREE		*m = (MTREE *)ctx;
	T_XFUENT	*e;
	CONST UB	*nm;
	INT		i;

	e = (T_XFUENT *)Kmalloc(sizeof(T_XFUENT));
	if ( e == NULL ) return E_NOMEM;
	for ( i = 0; i < m->nf; i++ ) {
		nm = mt_inside(m->f[i].path, path);
		if ( nm == NULL ) continue;
		knl_strcpy((char *)e->name, (CONST char *)nm);
		e->kind = m->f[i].kind;
		e->size = (D)m->f[i].n;
		e->mtime = m->f[i].mtime;
		if ( cb(arg, e) < 0 ) break;
	}
	Kfree(e);
	return E_OK;
}

LOCAL ER mt_stat( void *ctx, CONST UB *path, T_XFUENT *e )
{
	MTREE	*m = (MTREE *)ctx;
	INT	i;

	if ( path[0] == 0 ) {
		e->name[0] = 0;
		e->kind = XFU_K_DIR;
		e->size = -1;
		e->mtime = 0;
		return E_OK;
	}
	i = mt_find(m, path);
	if ( i < 0 ) return E_NOEXS;
	knl_strcpy((char *)e->name, (CONST char *)m->f[i].path);
	e->kind = m->f[i].kind;
	e->size = (D)m->f[i].n;
	e->mtime = m->f[i].mtime;
	return E_OK;
}

LOCAL ER mt_mkdir( void *ctx, CONST UB *path )
{
	return ( mt_add((MTREE *)ctx, path, XFU_K_DIR) >= 0 ) ? E_OK : E_LIMIT;
}

LOCAL ER mt_utime( void *ctx, CONST UB *path, D mtime )
{
	MTREE	*m = (MTREE *)ctx;
	INT	i = mt_find(m, path);

	if ( i < 0 ) return E_NOEXS;
	m->f[i].mtime = mtime;
	return E_OK;
}

LOCAL MTREE *mt_new( T_XFUTREE *t )
{
	MTREE	*m = (MTREE *)Kmalloc(sizeof(MTREE));
	INT	i;

	if ( m == NULL ) return NULL;
	knl_memset(m, 0, sizeof(*m));
	for ( i = 0; i < MT_OPEN; i++ ) m->oh[i] = -1;
	t->ctx = m;
	t->open = mt_open;
	t->read = mt_read;
	t->write = mt_write;
	t->close = mt_close;
	t->list = mt_list;
	t->stat = mt_stat;
	t->mkdir = mt_mkdir;
	t->utime = mt_utime;
	t->namemax = 0;
	return m;
}

LOCAL MTF *mt_file( MTREE *m, CONST char *path )
{
	INT	i = mt_find(m, (CONST UB *)path);

	return ( i >= 0 ) ? &m->f[i] : NULL;
}

/* ---------------------------------------------------------------- taking in and giving out */

/* The two TADjs objects: new identities each round, so that no run meets another's */
LOCAL char	set1[40], set2[40];
LOCAL char	set1_rec[512];

/* A path of a TADjs file: dir, the UUID, what follows it */
LOCAL CONST char *tj( CONST char *dir, CONST char *us, CONST char *tail )
{
	LOCAL char	p[128];

	knl_strcpy(p, dir);
	knl_strcat(p, us);
	knl_strcat(p, tail);
	return p;
}

LOCAL CONST char memo_sjis[] = "\x93\xfa\x96\x7b\x8c\xea\x83\x65\x83\x4c\x83\x58\x83\x67\x0d\x0a"
			       "\x93\xf1\x8d\x73\x96\xda\x20\x3c\x26\x3e\x0d\x0a";
LOCAL CONST char inner_utf8[] = "中の文章\nsecond line";
LOCAL CONST char doc_xtad[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>そのまま</p>"
			      "</document></tad>\n";
LOCAL CONST char set1_json[] = "{\"name\":\"集合1\",\"refCount\":3,\"recordCount\":1,"
			       "\"applist\":{\"basic-figure-editor\":{\"defaultOpen\":true}}}";
LOCAL CONST char set1_head[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure>\n<link id=\"";
LOCAL CONST char set1_tail[] = "_0.xtad\" vobjid=\"01a0d8c4-5299-7aaa-8aaa-000000000001\" vobjleft=\"8\""
			       " vobjtop=\"8\" vobjright=\"200\" vobjbottom=\"33\"/>\n</figure></tad>";
LOCAL CONST char set2_json[] = "{\"name\":\"集合2\",\"refCount\":1,\"recordCount\":1,"
			       "\"tessronos\":{\"note\":1}}";
LOCAL CONST char set2_rec[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>二つ目</p>"
			      "<p>a &amp; b</p></document></tad>";

/* A BMP of 2 by 2, 24 bits: red, green / blue, white (rows from the bottom) */
LOCAL CONST UB bmp22[] = {
	'B', 'M', 70, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
	40, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 16, 0, 0, 0,
	0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0,		/* blue, white */
	0, 0, 0xFF, 0, 0xFF, 0, 0, 0			/* red, green */
};

/* The tree taken in: src/ and what it holds */
LOCAL void fill_src( MTREE *m )
{
	UINT	px[16];
	UB	*png = NULL, bin[256];
	SZ	len = 0;
	INT	i;

	for ( i = 0; i < 16; i++ ) px[i] = (UINT)( i * 0x101010 );
	for ( i = 0; i < 256; i++ ) bin[i] = (UB)( 255 - i );
	(void)mt_add(m, (CONST UB *)"src", XFU_K_DIR);
	mt_put(m, "src/memo.txt", memo_sjis, s_len(memo_sjis));
	if ( xfu_png_encode(px, 4, 4, &png, &len) >= E_OK ) {
		mt_put(m, "src/pic.png", png, len);
		Kfree(png);
	}
	mt_put(m, "src/small.bmp", bmp22, sizeof(bmp22));
	mt_put(m, "src/data.bin", bin, sizeof(bin));
	mt_put(m, "src/anim.gif", gif33, sizeof(gif33));
	mt_put(m, "src/doc.xtad", doc_xtad, s_len(doc_xtad));
	(void)mt_add(m, (CONST UB *)"src/sub", XFU_K_DIR);
	mt_put(m, "src/sub/inner.txt", inner_utf8, s_len(inner_utf8));
	mt_put(m, tj("src/", set1, ".json"), set1_json, s_len(set1_json));
	mt_put(m, tj("src/", set1, "_0.xtad"), set1_rec, s_len(set1_rec));
	mt_put(m, tj("src/", set2, ".json"), set2_json, s_len(set2_json));
	mt_put(m, tj("src/", set2, "_0.xtad"), set2_rec, s_len(set2_rec));
}

/* A box to take into, an empty figure, on a volume (NULL: the first) */
LOCAL ER make_box( CONST char *vol, TS_UUID *box )
{
	T_OBCRE		c;
	CONST char	*mj = "{\"name\":\"変換の箱\",\"refCount\":0,\"recordCount\":1}";
	CONST char	*r = "<tad version=\"1.0\" encoding=\"UTF-8\"><figure>\n</figure></tad>";
	ER		er;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)mj;
	c.jsonsz = s_len(mj);
	c.vol = vol;
	er = ob_cre_obj(&c, box);
	if ( er >= E_OK ) er = om_obj_record_put(box, 0, OB_RT_TAD, 0, (CONST UB *)r, s_len(r));
	return er;
}

/* The links of an object's record 0, in order */
LOCAL INT links_of( CONST TS_UUID *u, TS_UUID *ids, INT max )
{
	UB	*rec;
	SZ	size = 0;
	INT	n;

	rec = om_obj_record(u, 0, &size);
	if ( rec == NULL ) return -1;
	n = om_store_links(rec, size, ids, max);
	Kfree(rec);
	return n;
}

/*
 * An object thrown away. A store refuses one something still links
 * to; what linked to it is gone first, and on the store on FAT, which
 * does not count by itself, its count is taken down by hand.
 */
LOCAL ER gone( CONST TS_UUID *u )
{
	ER	er = ob_del_obj(u);
	INT	i;

	for ( i = 0; er == E_OBJ && i < 8; i++ ) {
		(void)ob_unl_obj(u);
		er = ob_del_obj(u);
	}
	return er;
}

/* Whether an object has an icon, and whether it is the one of a program's template */
LOCAL BOOL icon_as( CONST TS_UUID *u, CONST char *app )
{
	UB	*a, *b;
	SZ	na = 0, nb = 0;
	BOOL	same;

	a = om_obj_icon(u, &na);
	b = ( app != NULL ) ? om_store_app_icon((CONST UB *)app, &nb) : NULL;
	same = (BOOL)( ( a == NULL && b == NULL )
		    || ( a != NULL && b != NULL && na == nb && same_bytes(a, b, (INT)na) ) );
	if ( a != NULL ) Kfree(a);
	if ( b != NULL ) Kfree(b);
	return same;
}

LOCAL INT refs( CONST TS_UUID *u )
{
	T_OBREF	r;

	return ( ob_ref_obj(u, &r) >= E_OK ) ? r.refcnt : -1;
}

/* The object linked from a figure whose name is this */
LOCAL BOOL by_name( TS_UUID *ids, INT n, CONST char *name, TS_UUID *p )
{
	T_OBREF	r;
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) >= E_OK && same_str(r.name, name) ) {
			*p = ids[i];
			return TRUE;
		}
	}
	return FALSE;
}

/* A member of "tessronos.file", as text */
LOCAL BOOL file_is( CONST TS_UUID *u, CONST char *member, CONST char *want )
{
	T_JSON	root, tf, f;
	UB	*meta, v[XFU_NAME_MAX];
	SZ	size = 0;
	BOOL	ok = FALSE;

	meta = om_obj_meta(u, &size);
	if ( meta == NULL ) return FALSE;
	if ( js_parse(meta, (INT)size, &root) >= E_OK && js_get(&root, "tessronos", &tf) >= E_OK
	  && js_get(&tf, "file", &f) >= E_OK && js_get_str(&f, member, v, sizeof(v)) >= 0 ) {
		ok = same_str(v, want);
	}
	Kfree(meta);
	return ok;
}

/* A file of a tree the same as bytes given */
LOCAL BOOL file_same( MTREE *m, CONST char *path, CONST void *b, SZ n )
{
	MTF	*f = mt_file(m, path);

	if ( f == NULL ) {
		tm_printf((UB *)"  no file %s\n", path);
		return FALSE;
	}
	if ( f->n != n || !same_bytes(f->b, (CONST UB *)b, (INT)n) ) {
		tm_printf((UB *)"  %s differs (%d bytes, %d wanted)\n", path, (INT)f->n, (INT)n);
		return FALSE;
	}
	return TRUE;
}

/* The whole round on one store */
LOCAL void round_on( CONST char *vol )
{
	T_XFUTREE	tin, tout;
	MTREE		*min, *mout;
	T_XFUOPT	opt;
	T_XFUSTAT	st;
	TS_UUID		box, top, *ids, memo, pic, bmp, gif, bin, doc, sub, inner, s1, s2, t;
	UB		name[64], *rec, *res;
	SZ		size = 0, asz = 0;
	INT		n, w = 0, h = 0;
	BOOL		counts;
	ID		key;
	MTF		*mf;

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 16);
	res = (UB *)Kmalloc(4096);
	min = mt_new(&tin);
	mout = mt_new(&tout);
	if ( ids == NULL || res == NULL || min == NULL || mout == NULL ) {
		KT_ASSERT(FALSE);
		return;
	}
	(void)ts_gen_uuid(&s1);
	(void)ts_gen_uuid(&s2);
	(void)ts_uuid_to_str(&s1, set1, sizeof(set1));
	(void)ts_uuid_to_str(&s2, set2, sizeof(set2));
	knl_strcpy(set1_rec, set1_head);
	knl_strcat(set1_rec, set2);
	knl_strcat(set1_rec, set1_tail);
	fill_src(min);
	KT_ASSERT_ER(make_box(vol, &box), E_OK);
	counts = om_store_counts(&box);

	/* taken in */
	knl_memset(&opt, 0, sizeof(opt));
	opt.source = "ktest";
	opt.stat = &st;
	KT_ASSERT_ER(xfu_import(&tin, (CONST UB *)"src", &opt, &box, &top), E_OK);
	KT_ASSERT_EQ(st.skipped, 0);
	KT_ASSERT_EQ(st.dirs, 2);
	KT_ASSERT_EQ(st.objects, 11);			/* 7 files, 2 directories, 2 TADjs objects */
	KT_ASSERT(links_of(&box, ids, 16) == 1 && ts_uuid_cmp(&ids[0], &top) == 0);
	KT_ASSERT_EQ(refs(&top), 1);
	KT_ASSERT(file_is(&top, "mediatype", "inode/directory"));
	KT_ASSERT(file_is(&top, "name", "src"));

	/* the directory: a figure linking to 8, the TADjs object linked from the other left out */
	n = links_of(&top, ids, 16);
	KT_ASSERT_EQ(n, 8);
	KT_ASSERT(by_name(ids, n, "anim", &gif));
	KT_ASSERT(by_name(ids, n, "memo", &memo));
	KT_ASSERT(by_name(ids, n, "pic", &pic));
	KT_ASSERT(by_name(ids, n, "small", &bmp));
	KT_ASSERT(by_name(ids, n, "data", &bin));
	KT_ASSERT(by_name(ids, n, "doc", &doc));
	KT_ASSERT(by_name(ids, n, "sub", &sub));
	KT_ASSERT(by_name(ids, n, "集合1", &t) && ts_uuid_cmp(&t, &s1) == 0);
	KT_ASSERT(!by_name(ids, n, "集合2", &t));
	KT_ASSERT(links_of(&sub, ids, 16) == 1 && by_name(ids, 1, "inner", &inner));
	KT_ASSERT(links_of(&s1, ids, 16) == 1 && ts_uuid_cmp(&ids[0], &s2) == 0);

	/* each linked to once, counted by whichever store it is */
	KT_ASSERT_EQ(refs(&memo), 1);
	KT_ASSERT_EQ(refs(&pic), 1);
	KT_ASSERT_EQ(refs(&sub), 1);
	KT_ASSERT_EQ(refs(&inner), 1);
	KT_ASSERT_EQ(refs(&s1), 1);
	KT_ASSERT_EQ(refs(&s2), 1);
	KT_ASSERT_EQ(om_store_counts(&memo), counts);

	/* the icon of the template of the program each opens with; none for bytes */
	KT_ASSERT(icon_as(&memo, "basic-text-editor"));
	KT_ASSERT(icon_as(&pic, "basic-figure-editor"));
	KT_ASSERT(icon_as(&top, "virtual-object-list"));
	KT_ASSERT(icon_as(&bin, NULL));

	/* the text: shown in UTF-8, kept as it came */
	rec = om_obj_record(&memo, 0, &size);
	KT_ASSERT(rec != NULL && has(rec, (INT)size, "<p>日本語テキスト</p>")
		  && has(rec, (INT)size, "<p>二行目 &lt;&amp;&gt;</p>"));
	if ( rec != NULL ) Kfree(rec);
	rec = om_obj_record(&memo, 1, &size);
	KT_ASSERT(rec != NULL && size == (SZ)s_len(memo_sjis) && same_bytes(rec, (CONST UB *)memo_sjis, (INT)size));
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT(file_is(&memo, "encoding", "shift_jis"));
	KT_ASSERT(file_is(&memo, "eol", "crlf"));
	KT_ASSERT(file_is(&memo, "source", "ktest"));
	KT_ASSERT(file_is(&memo, "mtime", "2026-09-01T08:00:00Z"));

	/* the pictures: a figure showing a PNG kept beside the records */
	rec = om_obj_record(&pic, 0, &size);
	KT_ASSERT(rec != NULL && has(rec, (INT)size, "<image") && has(rec, (INT)size, "_0_0.png\""));
	if ( rec != NULL ) Kfree(rec);
	key = ob_opn_obj(&bmp, OB_OP_R);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0_0.png", 0, res, 4096, &asz), E_OK);
		KT_ASSERT(xfu_png_size(res, asz, &w, &h) >= E_OK && w == 2 && h == 2);
		ob_cls_obj(key);
	}
	key = ob_opn_obj(&gif, OB_OP_R);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_rea_res(key, (CONST UB *)"_0_0.png", 0, res, 4096, &asz), E_OK);
		KT_ASSERT(xfu_png_size(res, asz, &w, &h) >= E_OK && w == 3 && h == 3);
		ob_cls_obj(key);
	}

	/* given out: what was not changed comes back as it was */
	KT_ASSERT_ER(xfu_export(&top, &tout, (CONST UB *)"", &opt, name, sizeof(name)), E_OK);
	KT_ASSERT(same_str(name, "src"));
	KT_ASSERT_EQ(st.skipped, 0);
	KT_ASSERT(file_same(mout, "src/memo.txt", memo_sjis, s_len(memo_sjis)));
	mf = mt_file(min, "src/pic.png");
	KT_ASSERT(mf != NULL && file_same(mout, "src/pic.png", mf->b, mf->n));
	KT_ASSERT(file_same(mout, "src/small.bmp", bmp22, sizeof(bmp22)));
	KT_ASSERT(file_same(mout, "src/anim.gif", gif33, sizeof(gif33)));
	mf = mt_file(min, "src/data.bin");
	KT_ASSERT(mf != NULL && file_same(mout, "src/data.bin", mf->b, mf->n));
	KT_ASSERT(file_same(mout, "src/doc.xtad", doc_xtad, s_len(doc_xtad)));
	KT_ASSERT(file_same(mout, "src/sub/inner.txt", inner_utf8, s_len(inner_utf8)));
	KT_ASSERT(file_same(mout, "src/集合1/集合2.txt", "二つ目\na & b\n", s_len("二つ目\na & b\n")));
	mf = mt_file(mout, "src/memo.txt");
	KT_ASSERT(mf != NULL && mf->mtime == xfu_time_parse((CONST UB *)"2026-09-01T08:00:00Z"));

	/* given out again: a name that is there already is not written over */
	KT_ASSERT_ER(xfu_export(&memo, &tout, (CONST UB *)"src", NULL, name, sizeof(name)), E_OK);
	KT_ASSERT(same_str(name, "memo (2).txt"));

	/* the text changed here: its text, in the encoding and line ends it came in */
	key = ob_opn_obj(&memo, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		CONST char	*nd = "<tad version=\"1.0\" encoding=\"UTF-8\"><document>\n<p>変更後</p>\n"
				      "<p>二行目</p>\n</document></tad>";

		KT_ASSERT_ER(ob_wri_rec(key, 0, 0, nd, s_len(nd), &asz), E_OK);
		KT_ASSERT_ER(ob_trn_rec(key, 0, s_len(nd)), E_OK);
		ob_cls_obj(key);
	}
	(void)mt_mkdir(mout, (CONST UB *)"again");
	KT_ASSERT_ER(xfu_export(&memo, &tout, (CONST UB *)"again", NULL, name, sizeof(name)), E_OK);
	KT_ASSERT(file_same(mout, "again/memo.txt", "\x95\xcf\x8d\x58\x8c\xe3\x0d\x0a\x93\xf1\x8d\x73\x96\xda\x0d\x0a", 16));
	opt.outenc = TXC_UTF8;
	KT_ASSERT_ER(xfu_export(&memo, &tout, (CONST UB *)"again", &opt, name, sizeof(name)), E_OK);
	KT_ASSERT(file_same(mout, "again/memo (2).txt", "変更後\r\n二行目\r\n", s_len("変更後\r\n二行目\r\n")));
	opt.outenc = TXC_AUTO;

	/* a figure alone, and TADjs file sets of it and what it links to */
	opt.flags = XFU_F_ONE;
	KT_ASSERT_ER(xfu_export(&s1, &tout, (CONST UB *)"again", &opt, name, sizeof(name)), E_OK);
	KT_ASSERT(same_str(name, "集合1.xtad"));
	(void)mt_mkdir(mout, (CONST UB *)"tadjs");
	opt.flags = XFU_F_TADJS;
	KT_ASSERT_ER(xfu_export(&s1, &tout, (CONST UB *)"tadjs", &opt, name, sizeof(name)), E_OK);
	KT_ASSERT(same_str(name, set1));
	KT_ASSERT(file_same(mout, tj("tadjs/", set1, "_0.xtad"), set1_rec, s_len(set1_rec)));
	KT_ASSERT(file_same(mout, tj("tadjs/", set2, "_0.xtad"), set2_rec, s_len(set2_rec)));
	mf = mt_file(mout, tj("tadjs/", set2, ".json"));
	KT_ASSERT(mf != NULL && has(mf->b, (INT)mf->n, "\"note\":1") && has(mf->b, (INT)mf->n, "集合2"));
	KT_ASSERT(mt_file(mout, tj("tadjs/", set1, ".json")) != NULL);

	/* thrown away, from the box down */
	KT_ASSERT_ER(gone(&box), E_OK);
	KT_ASSERT_ER(gone(&top), E_OK);
	KT_ASSERT_ER(gone(&sub), E_OK);
	KT_ASSERT_ER(gone(&inner), E_OK);
	KT_ASSERT_ER(gone(&memo), E_OK);
	KT_ASSERT_ER(gone(&pic), E_OK);
	KT_ASSERT_ER(gone(&bmp), E_OK);
	KT_ASSERT_ER(gone(&gif), E_OK);
	KT_ASSERT_ER(gone(&bin), E_OK);
	KT_ASSERT_ER(gone(&doc), E_OK);
	KT_ASSERT_ER(gone(&s1), E_OK);
	KT_ASSERT_ER(gone(&s2), E_OK);
	mt_free(min);
	mt_free(mout);
	Kfree(min);
	Kfree(mout);
	Kfree(ids);
	Kfree(res);
}

/* How many storage objects there are */
LOCAL INT n_objects( void )
{
	TS_UUID	*l = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 2048);
	INT	cnt = -1;

	if ( l != NULL && ob_lst_obj(OB_T_STORAGE, OB_S_FILE, NULL, l, 2048, &cnt) < E_OK ) cnt = -1;
	if ( l != NULL ) Kfree(l);
	return cnt;
}

LOCAL INT	notes;

/* The third note stops the call */
LOCAL ER stop_at_3( void *arg, CONST UB *name, ER er )
{
	(void)arg;
	(void)name;
	(void)er;
	return ( ++notes >= 3 ) ? E_ABORT : E_OK;
}

/*
 * Stopped part way: E_ABORT, and every object made so far thrown away
 * again -- the TADjs objects made with their own UUIDs among them --
 * with nothing put in the box
 */
LOCAL void test_xfu_stop( void )
{
	T_XFUTREE	tin;
	MTREE		*min;
	T_XFUOPT	opt;
	TS_UUID		box, top, s1, s2, ids[4];
	T_OBREF		r;
	INT		before;

	min = mt_new(&tin);
	if ( min == NULL || make_box(NULL, &box) < E_OK ) KT_SKIP("no memory or no store");
	(void)ts_gen_uuid(&s1);
	(void)ts_gen_uuid(&s2);
	(void)ts_uuid_to_str(&s1, set1, sizeof(set1));
	(void)ts_uuid_to_str(&s2, set2, sizeof(set2));
	knl_strcpy(set1_rec, set1_head);
	knl_strcat(set1_rec, set2);
	knl_strcat(set1_rec, set1_tail);
	fill_src(min);
	before = n_objects();
	knl_memset(&opt, 0, sizeof(opt));
	opt.note = stop_at_3;
	notes = 0;
	KT_ASSERT_ER(xfu_import(&tin, (CONST UB *)"src", &opt, &box, &top), E_ABORT);
	KT_ASSERT(notes >= 3);
	KT_ASSERT_EQ(n_objects(), before);
	KT_ASSERT_EQ(links_of(&box, ids, 4), 0);
	KT_ASSERT(ob_ref_obj(&s1, &r) < E_OK && ob_ref_obj(&s2, &r) < E_OK);
	KT_ASSERT_ER(gone(&box), E_OK);
	mt_free(min);
	Kfree(min);
}

LOCAL void test_xfu_fat( void )
{
	TS_UUID	box;

	/* the first volume: the store on the FAT of the test disk */
	if ( make_box(NULL, &box) < E_OK ) KT_SKIP("no store");
	KT_ASSERT_ER(gone(&box), E_OK);
	round_on(NULL);
}

#define NATDEV	kt_scratch()

LOCAL void test_xfu_native( void )
{
	if ( NATDEV == NULL ) KT_SKIP(KT_NO_SCRATCH);
	if ( ob_att_vol(NATDEV, TSFS_STORE_BLK) < E_OK ) {
		if ( ts_format_blk(NATDEV, "OBJECTS") < E_OK
		  || ob_att_vol(NATDEV, TSFS_STORE_BLK) < E_OK ) {
			KT_SKIP("no native volume");
		}
	}
	round_on(NATDEV);
	(void)ob_det_vol(NATDEV);
}

/* ---------------------------------------------------------------- the file layer */

/* A file of the file layer read whole into buf; its length, or -1 */
LOCAL INT fs_all( CONST char *path, UB *buf, INT max )
{
	INT	fd = fs_open(path, O_RDONLY), n = 0, k;

	if ( fd < 0 ) return -1;
	while ( n < max && ( k = fs_read(fd, buf + n, max - n) ) > 0 ) n += k;
	fs_close(fd);
	return n;
}

/*
 * A tree of the file layer: a name of 90 kana (270 bytes, more than a
 * short limit would hold) with a time of its own, taken in, and given
 * out again beside it with " (2)" and the same time.
 */
LOCAL void test_xfu_fs( void )
{
	T_XFUFS		*fs;
	T_XFUTREE	t;
	T_XFUENT	*e;
	T_FSTAT		st;
	TS_UUID		box, u;
	UB		*name, back[64], iso[24];
	char		*path, *path2;
	D		when = xfu_time_parse((CONST UB *)"2024-05-06T07:08:10Z");
	INT		fd, i, n;

	fs = (T_XFUFS *)Kmalloc(sizeof(T_XFUFS));
	e = (T_XFUENT *)Kmalloc(sizeof(T_XFUENT));
	name = (UB *)Kmalloc(XFU_NAME_MAX);
	path = (char *)Kmalloc(FS_PATH_MAX);
	path2 = (char *)Kmalloc(FS_PATH_MAX);
	if ( fs == NULL || e == NULL || name == NULL || path == NULL || path2 == NULL ) KT_SKIP("no memory");

	/* the long name: 90 times あ */
	n = 0;
	for ( i = 0; i < 90; i++ ) {
		name[n++] = 0xE3;  name[n++] = 0x81;  name[n++] = 0x82;
	}
	name[n] = 0;
	knl_strcpy(path, "/boot/XFUT/");
	knl_strcat(path, (CONST char *)name);
	knl_strcat(path, ".txt");
	knl_strcpy(path2, "/boot/XFUT/");
	knl_strcat(path2, (CONST char *)name);
	knl_strcat(path2, " (2).txt");

	(void)fs_mkdir("/boot/XFUT");
	(void)fs_unlink(path2);
	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) KT_SKIP("no file system");
	KT_ASSERT_EQ(fs_write(fd, memo_sjis, s_len(memo_sjis)), s_len(memo_sjis));
	fs_close(fd);
	KT_ASSERT_ER(fs_utime(path, (UD)when), EX_OK);
	KT_ASSERT(fs_stat(path, &st) >= EX_OK && (D)st.mtime == when);
	KT_ASSERT_ER(fs_utime("/boot", (UD)when), EX_INVAL);		/* the root has no entry */

	if ( make_box(NULL, &box) < E_OK ) KT_SKIP("no store");
	KT_ASSERT_ER(xfu_fs_tree(fs, "/boot", &t), E_OK);
	KT_ASSERT_EQ(t.namemax, FS_NAME_MAX - 1);
	KT_ASSERT(t.utime != NULL);
	KT_ASSERT_ER(t.stat(t.ctx, (CONST UB *)"XFUT", e), E_OK);
	KT_ASSERT_EQ(e->kind, XFU_K_DIR);
	KT_ASSERT_ER(xfu_import(&t, (CONST UB *)path + 6, NULL, &box, &u), E_OK);
	KT_ASSERT(file_is(&u, "encoding", "shift_jis"));
	(void)xfu_time_iso(when, iso);
	KT_ASSERT(file_is(&u, "mtime", (CONST char *)iso));
	KT_ASSERT(file_is(&u, "name", path + 11));
	KT_ASSERT_ER(xfu_export(&u, &t, (CONST UB *)"XFUT", NULL, name, XFU_NAME_MAX), E_OK);
	KT_ASSERT(same_str(name, path2 + 11));
	KT_ASSERT_EQ(fs_all(path2, back, sizeof(back)), s_len(memo_sjis));
	KT_ASSERT(same_bytes(back, (CONST UB *)memo_sjis, s_len(memo_sjis)));
	KT_ASSERT(fs_stat(path2, &st) >= EX_OK && (D)st.mtime == when);
	KT_ASSERT_ER(gone(&box), E_OK);
	KT_ASSERT_ER(gone(&u), E_OK);
	(void)fs_unlink(path2);
	(void)fs_unlink(path);
	(void)fs_rmdir("/boot/XFUT");
	Kfree(fs);
	Kfree(e);
	Kfree(name);
	Kfree(path);
	Kfree(path2);
}

/* Whether w is among the n bytes of s */
LOCAL BOOL has_text( CONST UB *s, SZ n, CONST char *w )
{
	INT	i, k, wl = s_len(w);

	for ( i = 0; i + wl <= (INT)n; i++ ) {
		for ( k = 0; k < wl && s[i + k] == (UB)w[k]; k++ ) ;
		if ( k == wl ) return TRUE;
	}
	return FALSE;
}

/*
 * A file taken in as the caller says it is: a text made full width
 * (letters, the blank, the half width katakana with its voiced mark),
 * bytes alone (record 0 only names it; given out again, the same
 * bytes), and an archive for 書庫解凍.
 */
LOCAL void test_xfu_as( void )
{
	static CONST char	body[] = "Ab 1\xEF\xBD\xB6\xEF\xBE\x9E\n";	/* Ab 1ｶﾞ */
	T_XFUFS		*fs;
	T_XFUTREE	t;
	T_XFUOPT	opt;
	TS_UUID		box, u;
	UB		*rec, name[64], back[32];
	SZ		size = 0;
	INT		fd;

	fs = (T_XFUFS *)Kmalloc(sizeof(T_XFUFS));
	if ( fs == NULL ) KT_SKIP("no memory");
	(void)fs_mkdir("/boot/XFUA");
	(void)fs_unlink("/boot/XFUA/HK (2).TXT");
	fd = fs_open("/boot/XFUA/HK.TXT", O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		Kfree(fs);
		KT_SKIP("no file system");
	}
	KT_ASSERT_EQ(fs_write(fd, body, s_len(body)), s_len(body));
	fs_close(fd);
	if ( make_box(NULL, &box) < E_OK ) {
		Kfree(fs);
		KT_SKIP("no store");
	}
	KT_ASSERT_ER(xfu_fs_tree(fs, "/boot", &t), E_OK);

	/* full width */
	knl_memset(&opt, 0, sizeof(opt));
	opt.as = XFU_AS_ZEN;
	opt.enc = TXC_UTF8;
	KT_ASSERT_ER(xfu_import(&t, (CONST UB *)"XFUA/HK.TXT", &opt, NULL, &u), E_OK);
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL && has_text(rec, size, "<p>\xEF\xBC\xA1\xEF\xBD\x82\xE3\x80\x80\xEF\xBC\x91\xE3\x82\xAC</p>"));
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT_ER(gone(&u), E_OK);

	/* bytes alone, and the same bytes given out */
	opt.as = XFU_AS_RAW;
	KT_ASSERT_ER(xfu_import(&t, (CONST UB *)"XFUA/HK.TXT", &opt, NULL, &u), E_OK);
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL && !has_text(rec, size, "<p>Ab"));
	if ( rec != NULL ) Kfree(rec);
	rec = om_obj_record(&u, 1, &size);
	KT_ASSERT(rec != NULL && size == (SZ)s_len(body) && same_bytes(rec, (CONST UB *)body, s_len(body)));
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT_ER(xfu_export(&u, &t, (CONST UB *)"XFUA", NULL, name, sizeof(name)), E_OK);
	KT_ASSERT_EQ(fs_all("/boot/XFUA/HK (2).TXT", back, sizeof(back)), s_len(body));
	KT_ASSERT(same_bytes(back, (CONST UB *)body, s_len(body)));
	(void)fs_unlink("/boot/XFUA/HK (2).TXT");
	KT_ASSERT_ER(gone(&u), E_OK);

	/* an archive */
	opt.as = XFU_AS_ARCHIVE;
	KT_ASSERT_ER(xfu_import(&t, (CONST UB *)"XFUA/HK.TXT", &opt, NULL, &u), E_OK);
	KT_ASSERT(file_is(&u, "mediatype", "application/x-btron-archive"));
	KT_ASSERT_ER(gone(&u), E_OK);

	/* given out under a name of the caller's */
	knl_memset(&opt, 0, sizeof(opt));
	KT_ASSERT_ER(xfu_import(&t, (CONST UB *)"XFUA/HK.TXT", NULL, NULL, &u), E_OK);
	opt.name = (CONST UB *)"OUT.TXT";
	KT_ASSERT_ER(xfu_export(&u, &t, (CONST UB *)"XFUA", &opt, name, sizeof(name)), E_OK);
	KT_ASSERT(same_bytes(name, (CONST UB *)"OUT.TXT", 8));
	KT_ASSERT_EQ(fs_all("/boot/XFUA/OUT.TXT", back, sizeof(back)), s_len(body));
	(void)fs_unlink("/boot/XFUA/OUT.TXT");
	KT_ASSERT_ER(gone(&u), E_OK);

	KT_ASSERT_ER(gone(&box), E_OK);
	(void)fs_unlink("/boot/XFUA/HK.TXT");
	(void)fs_rmdir("/boot/XFUA");
	Kfree(fs);
}

/* ---------------------------------------------------------------- over FTP */

LOCAL CONST char * CONST script_tree[] = {
	"R:220 ready\r\n",
	"S:USER anonymous",
	"R:331 pw\r\n",
	"S:PASS anonymous@",
	"R:230 ok\r\n",
	"S:SYST",
	"R:215 UNIX\r\n",
	"S:FEAT",
	"R:211-x\r\n UTF8\r\n MLST type*;size*;\r\n211 end\r\n",
	"S:OPTS UTF8 ON",
	"R:200 ok\r\n",
	"S:TYPE I",
	"R:200 ok\r\n",
	/* what the path is: the listing of its directory */
	"S:EPSV",
	"R:229 (|||40010|)\r\n",
	"O:40010",
	"S:MLSD /pub",
	"R:150 ok\r\n",
	"D:type=file;size=20;modify=20260102030405; 報告.txt\r\n",
	"R:226 ok\r\n",
	/* and the file */
	"S:EPSV",
	"R:229 (|||40011|)\r\n",
	"O:40011",
	"S:RETR /pub/報告.txt",
	"R:150 ok\r\n",
	"D:\x95\xf1\x8d\x90\x0a\x93\xe0\x97\x65\x0a",
	"R:226 ok\r\n",
	"S:QUIT",
	"R:221 bye\r\n",
};

LOCAL void test_xfu_ftp( void )
{
	T_FTP		*f;
	T_FTPNET	net;
	T_XFUFTP	*ft;
	T_XFUTREE	t;
	T_XFUOPT	opt;
	FAKE		k;
	TS_UUID		box, u;
	UB		*rec;
	SZ		size = 0;

	f = (T_FTP *)Kmalloc(sizeof(T_FTP));
	ft = (T_XFUFTP *)Kmalloc(sizeof(T_XFUFTP));
	if ( f == NULL || ft == NULL ) KT_SKIP("no memory");
	if ( make_box(NULL, &box) < E_OK ) KT_SKIP("no store");
	fk_init(&k, &net, script_tree, NSTEP(script_tree), 7);
	KT_ASSERT_ER(ftp_open(f, &net, "files.example", 0, NULL, NULL, TXC_SJIS), E_OK);
	KT_ASSERT_ER(xfu_ftp_tree(ft, f, (CONST UB *)"/pub", &t), E_OK);
	KT_ASSERT(t.utime == NULL);
	knl_memset(&opt, 0, sizeof(opt));
	opt.source = "ftp";
	KT_ASSERT_ER(xfu_import(&t, (CONST UB *)"報告.txt", &opt, &box, &u), E_OK);
	rec = om_obj_record(&u, 0, &size);
	KT_ASSERT(rec != NULL && has(rec, (INT)size, "<p>報告</p>") && has(rec, (INT)size, "<p>内容</p>"));
	if ( rec != NULL ) Kfree(rec);
	KT_ASSERT(file_is(&u, "source", "ftp"));
	KT_ASSERT(file_is(&u, "mtime", "2026-01-02T03:04:05Z"));
	KT_ASSERT(file_is(&u, "encoding", "shift_jis"));
	KT_ASSERT_ER(ftp_close(f), E_OK);
	KT_ASSERT_EQ(k.fails, 0);
	KT_ASSERT_EQ(k.at, k.n);
	if ( k.fails > 0 ) tm_printf((UB *)"  fake: first fault at step %d\n", k.failat);
	KT_ASSERT_ER(gone(&box), E_OK);
	KT_ASSERT_ER(gone(&u), E_OK);
	Kfree(f);
	Kfree(ft);
}

EXPORT void ktest_xfu( void )
{
	KT_RUN(test_txc_codec);
	KT_RUN(test_txc_detect);
	KT_RUN(test_names);
	KT_RUN(test_png);
	KT_RUN(test_gif);
	KT_RUN(test_ftp_new);
	KT_RUN(test_ftp_old);
	KT_RUN(test_ftp_active);
	KT_RUN(test_xfu_fat);
	KT_RUN(test_xfu_native);
	KT_RUN(test_xfu_stop);
	KT_RUN(test_xfu_fs);
	KT_RUN(test_xfu_as);
	KT_RUN(test_xfu_ftp);
}
