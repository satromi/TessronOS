/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_wifinv.c
 *	The Raspberry Pi 5 Wi-Fi's board settings read out of their document
 *	(peripheral_kernel/network/netbsd/wifi_nvram.c, design 9.6.8).
 *
 *	Built with WIFI=1 (build_make/wifi_arm.mk). The documents of the cases
 *	are those of tools/rpi5/wifi/test_nvram.py, which reads them on the
 *	host the same way; the document on the system volume, made from
 *	linux-firmware's text, is read as the Wi-Fi reads it.
 */

#include "ktest.h"
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "tstdlib.h"

IMPORT INT knl_wifi_nvram( CONST UB *xml, SZ len, UB *out, SZ max, INT *p_drop );

#define HEAD	"<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"t\">\n<document>\n"
#define TAIL	"</document>\n</tad>\n"

typedef struct {
	CONST char	*body;
	CONST char	*want;
	INT		drop;
} NV_CASE;

/* 【注記】, ａａ５ｇ　＝　１, 名前 in UTF-8 */
LOCAL CONST NV_CASE	cases[] = {
	{ "<p>\xe3\x80\x90\xe6\xb3\xa8\xe8\xa8\x98\xe3\x80\x91notes</p>\n<p># SPDX-License-Identifier: GPL-2.0+</p>\n<p></p>\n"
	  "<p>aa2g=1</p>\n<p>boardflags=0x00480201</p>\n",
	  "aa2g=1\nboardflags=0x00480201\n", 0 },
	{ "<p>\xef\xbd\x81\xef\xbd\x81\xef\xbc\x95\xef\xbd\x87\xe3\x80\x80\xef\xbc\x9d\xe3\x80\x80\xef\xbc\x91</p>\n"
	  "<p>  devid = 0x43ab  </p>\n",
	  "aa5g=1\ndevid=0x43ab\n", 0 },
	{ "<p>ldo1=4<br/>macaddr=b8:27:eb:74:f2:6c</p>\n<p><font size=\"12\">nocrc=1</font></p>\n",
	  "ldo1=4\nmacaddr=b8:27:eb:74:f2:6c\nnocrc=1\n", 0 },
	{ "<p>\xe5\x90\x8d\xe5\x89\x8d=1</p>\n<p>sromrev</p>\n<p>sromrev=11</p>\n",
	  "sromrev=11\n", 2 },
	{ "<p>x=a&amp;b</p>\n<p>y=&lt;1&gt;</p>\n",
	  "x=a&b\ny=<1>\n", 0 },
	{ "<p>\xe3\x80\x90\xe6\xb3\xa8\xe8\xa8\x98\xe3\x80\x91" "first<br/>key=ignored</p>\n<p>k=v</p>\n",
	  "k=v\n", 0 },
};

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	while ( *a != '\0' && *a == (UB)*b ) {
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

/* the cases: notes, comments, blank lines, full-width, line breaks, what is not a setting */
LOCAL void test_cases( void )
{
	UB	doc[1024], out[512];
	INT	i, n, len, drop;

	for ( i = 0; i < (INT)( sizeof(cases) / sizeof(cases[0]) ); i++ ) {
		n = (INT)knl_strlen(HEAD);
		knl_memcpy(doc, HEAD, (SZ)n);
		len = (INT)knl_strlen(cases[i].body);
		knl_memcpy(doc + n, cases[i].body, (SZ)len);
		n += len;
		len = (INT)knl_strlen(TAIL);
		knl_memcpy(doc + n, TAIL, (SZ)len);
		n += len;
		drop = -1;
		len = knl_wifi_nvram(doc, (SZ)n, out, sizeof(out), &drop);
		KT_ASSERT_EQ(len, (INT)knl_strlen(cases[i].want));
		KT_ASSERT(len >= 0 && same(out, cases[i].want));
		KT_ASSERT_EQ(drop, cases[i].drop);
		if ( len < 0 || !same(out, cases[i].want) ) {
			tm_printf((UB *)"  case %d: %d \"%s\"\n", i, len, ( len >= 0 ) ? (char *)out : "");
		}
	}
}

/* what does not fit, and what is not a document */
LOCAL void test_limits( void )
{
	static CONST char	doc[] = HEAD "<p>abcdefgh=12345678</p>\n" TAIL;
	UB			out[64];

	KT_ASSERT_ER(knl_wifi_nvram((CONST UB *)doc, sizeof(doc) - 1, out, 8, NULL), E_LIMIT);
	KT_ASSERT_EQ(knl_wifi_nvram((CONST UB *)doc, sizeof(doc) - 1, out, sizeof(out), NULL), 18);
	KT_ASSERT(knl_wifi_nvram((CONST UB *)"<tad><document><p>", 18, out, sizeof(out), NULL) < 0);
}

/* the document on the system volume, as the Wi-Fi reads it */
LOCAL void test_volume( void )
{
	TS_UUID	u;
	T_OBREC	r[4];
	ID	key;
	INT	i, cnt = 0, len, drop = -1, lines = 0;
	SZ	sz = 0, asz = 0;
	UB	*doc, *out;

	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_FW_BWFM43455_NV, &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key <= 0 ) {
		KT_SKIP("no board settings on the system volume (WIFI=1 puts them there)");
		return;
	}
	KT_ASSERT_ER(ob_lst_rec(key, r, 4, &cnt), E_OK);
	for ( i = 0; i < cnt; i++ ) {
		if ( r[i].recno == 0 ) sz = (SZ)r[i].size;
	}
	KT_ASSERT(sz > 0 && sz < 64 * 1024);
	doc = (UB *)Kmalloc(sz + 1);
	out = (UB *)Kmalloc(sz + 1);
	KT_ASSERT(doc != NULL && out != NULL);
	if ( doc != NULL && out != NULL && sz > 0 ) {
		KT_ASSERT_ER(ob_rea_rec(key, 0, 0, doc, sz, &asz), E_OK);
		len = knl_wifi_nvram(doc, asz, out, sz + 1, &drop);
		KT_ASSERT(len > 0);
		KT_ASSERT_EQ(drop, 0);
		for ( i = 0; i < len; i++ ) {
			if ( out[i] == '\n' ) lines++;
			KT_ASSERT(out[i] >= 0x20 || out[i] == '\n');
			if ( out[i] < 0x20 && out[i] != '\n' ) break;
		}
		/* linux-firmware's text of the board: 79 settings, the first and one in the middle */
		KT_ASSERT_EQ(lines, 79);
		KT_ASSERT(len > 6 && out[0] == 'a' && out[1] == 'a' && out[2] == '2' && out[3] == 'g' && out[4] == '=');
		tm_printf((UB *)"  the document: %d bytes, %d settings, %d bytes of text\n", (INT)asz, lines, len);
	}
	if ( doc != NULL ) Kfree(doc);
	if ( out != NULL ) Kfree(out);
	ob_cls_obj(key);
}

EXPORT void ktest_wifinv( void )
{
	KT_RUN(test_cases);
	KT_RUN(test_limits);
	KT_RUN(test_volume);
}
