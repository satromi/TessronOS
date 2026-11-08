/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wd_main.c
 *	単語登録: the accessory the person registers words with (design 11.16)
 *
 *	One window of panels (SYSDEF_BOX_WORDDIC): the reading, the word and
 *	its part of speech above, with 登録, 変更, 削除 and クリア beside
 *	them; below, the words registered, in the order of their readings, a
 *	page at a time, and a box to find words by the start of their
 *	reading or by letters in them. A word picked in the list is put in
 *	the boxes above, to be changed or deleted.
 *
 *	The words are the user dictionary object's (the person's own when
 *	they are logged in, which the converter makes; SYSDEF_DICT_USER for
 *	利用者; dic_uuid), as
 *	lines of text (ts/udic.h). Each change is written at once: the
 *	converter watches the object and takes the words again, so a word
 *	registered is converted to from the next key on.
 *
 *	Words are read in from what is dropped on the window -- an object
 *	taken in from a file (its record 1), or a text -- or from the tray
 *	(読込み), in the form the common converters write them out in, and
 *	are written out (書出し) to the tray as a text of the same lines, a
 *	paragraph to a word, to be put into a text and written out to a file
 *	from there.
 *
 *	Started from a text with letters chosen, it finds them in the tray's
 *	place of its own (UD_TRAY_SUB) and puts them in the word's box.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ts/ui.h>
#include <ts/tray.h>
#include <ts/sysdef.h>
#include <ts/udic.h>
#include <ts/obatr.h>

#define TICK_MS		20

/* The parts, by the numbers the data box gives them */
#define P_YOMI		1
#define P_WORD		2
#define P_POS		3
#define B_PUT		4
#define B_CHANGE	5
#define B_DELETE	6
#define B_CLEAR		7
#define P_FIND		8
#define B_FIND		9
#define P_LIST		10
#define B_PREV		11
#define B_NEXT		12
#define P_STATUS	13
#define B_ALL		14

#define PN_MAIN		1
#define PN_WARN		10
#define PN_DELETE	11
#define B_YES		103

#define ROWS		20		/* words a page of the list shows */
#define ROW_MAX		72		/* bytes of a row */

LOCAL UI	ui;
LOCAL BOOL	closing;

LOCAL UB	dic[UD_TEXT_MAX];	/* the dictionary's text */
LOCAL INT	dlen;
LOCAL UB	work[UD_TEXT_MAX];	/* a copy changed before it is kept */

/* The page of the list shown: the words on it, as they were when it was made */
typedef struct {
	UB	yomi[UD_YOMI_MAX];
	UB	word[UD_WORD_MAX];
	INT	pos;
} ROW;

LOCAL ROW	row[ROWS];
LOCAL INT	nrow;
LOCAL INT	first;			/* the first word of the page, among those found */
LOCAL INT	nfound;
LOCAL char	key[UD_YOMI_MAX * 2];	/* what is looked for; "" all */

/* The word picked in the list, which 変更 and 削除 act on */
LOCAL BOOL	picked;
LOCAL ROW	pick;

/*
 * The dictionary of whoever runs this: a person logged in has their own,
 * linked from 辞書箱 and told by its owner and its metadata
 * (tessronos.userdic). The converter makes it when they log in; until
 * there is one, it is the one 利用者 has.
 */
LOCAL ER dic_uuid( TS_UUID *u )
{
	T_OBCRD	me;
	TS_UUID	box, *lk = NULL;
	INT	n, i;

	if ( ob_get_crd(&me) >= E_OK && ui_uuid(SYSDEF_DICT_BOX, &box) >= E_OK ) {
		n = ui_lst_lnk(&box, &lk);
		for ( i = 0; i < n; i++ ) {
			UB	pbuf[OB_PRT_SIZE(16, 4)];
			T_OBPRT	*prt = (T_OBPRT *)pbuf;
			UB	*meta = NULL;
			SZ	asz = 0;
			ID	key;
			BOOL	yes = FALSE;

			if ( ob_get_prt(&lk[i], prt, sizeof(pbuf), &asz) < E_OK
			  || memcmp(&prt->owner, &me.user, sizeof(TS_UUID)) != 0 ) continue;
			key = ob_opn_obj(&lk[i], OB_OP_ATRRD);
			if ( key <= 0 ) continue;
			if ( ob_atr_read(key, 0, &meta) >= 0 ) {
				yes = (BOOL)( strstr((CONST char *)meta, "\"userdic\"") != NULL );
			}
			free(meta);
			ob_cls_obj(key);
			if ( yes ) {
				*u = lk[i];
				free(lk);
				return E_OK;
			}
		}
		free(lk);
	}
	return ui_uuid(SYSDEF_DICT_USER, u);
}

/* ---------------------------------------------------------------- the dictionary object */

LOCAL ER dic_read( void )
{
	TS_UUID	u;
	ID	k;
	SZ	asz = 0;
	ER	er;

	dlen = 0;
	if ( dic_uuid(&u) < E_OK ) return E_SYS;
	k = ob_opn_obj(&u, OB_OP_R);
	if ( k < E_OK ) return (ER)k;
	er = ob_rea_rec(k, 1, 0, dic, sizeof(dic), &asz);
	ob_cls_obj(k);
	if ( er >= E_OK ) dlen = (INT)( ( asz > (SZ)sizeof(dic) ) ? (SZ)sizeof(dic) : asz );
	return ( er == E_NOEXS ) ? E_OK : er;		/* no record yet: no words */
}

/* The words in work kept: written to the object, and taken as the dictionary */
LOCAL ER dic_keep( INT len )
{
	TS_UUID	u;
	ID	k;
	SZ	asz = 0;
	ER	er;

	if ( dic_uuid(&u) < E_OK ) return E_SYS;
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( k < E_OK ) return (ER)k;
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ob_rpl_rec(k, 1, work, (SZ)len);
	(void)asz;
	ob_cls_obj(k);
	if ( er >= E_OK ) {
		memcpy(dic, work, (size_t)len);
		dlen = len;
	}
	return er;
}

LOCAL void to_work( INT *p_len )
{
	memcpy(work, dic, (size_t)dlen);
	*p_len = dlen;
}

/* ---------------------------------------------------------------- words to the person */

/* A line to the console, which the tests read */
LOCAL void say( CONST char *fmt, ... )
{
	char	s[UD_LINE_MAX * 2];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(s, sizeof(s), fmt, ap);
	va_end(ap);
	tm_putstring((CONST UB *)s);
}

LOCAL void status( CONST char *s )
{
	ui_set_text(&ui, P_STATUS, s);
}

LOCAL void warn( CONST char *a, CONST char *b )
{
	CONST char	*l[3] = { a, ( b != NULL ) ? b : " ", " " };

	(void)ui_ask(&ui, PN_WARN, l, 3);
}

/* The bytes of s that fit in max with a 0 after them, cut at a letter's edge */
LOCAL INT fit( CONST UB *s, INT n, INT max )
{
	if ( n < max ) return n;
	n = max - 1;
	while ( n > 0 && ( s[n] & 0xC0 ) == 0x80 ) n--;
	return n;
}

LOCAL void copy_to( UB *to, INT max, CONST UB *s, INT n )
{
	n = fit(s, n, max);
	memcpy(to, s, (size_t)n);
	to[n] = 0;
}

/* ---------------------------------------------------------------- the parts of speech */

LOCAL INT	npos;

LOCAL void pos_names( void )
{
	CONST char	*nm[UD_POS_MAX];
	INT		p;

	for ( npos = 0; npos < UD_POS_MAX && ( p = ud_pos_offer(npos) ) != 0; npos++ ) {
		nm[npos] = ud_pos_name(p);
	}
	ui_names(&ui, P_POS, nm, npos);
	ui_set_value(&ui, P_POS, 1);
}

LOCAL void pos_show( INT pos )
{
	INT	k;

	for ( k = 0; k < npos; k++ ) {
		if ( ud_pos_offer(k) == pos ) {
			ui_set_value(&ui, P_POS, k + 1);
			return;
		}
	}
	ui_set_value(&ui, P_POS, 0);
}

/* The part chosen; the word picked keeps its own when none is */
LOCAL INT pos_chosen( void )
{
	INT	k = ui_value(&ui, P_POS);

	if ( k >= 1 && k <= npos ) return ud_pos_offer(k - 1);
	return picked ? pick.pos : UD_POS_NOUN;
}

/* ---------------------------------------------------------------- the list */

LOCAL void list_show( void )
{
	static char	text[ROWS][ROW_MAX + 8];
	CONST char	*nm[ROWS];
	char		s[96];
	UDENT		e;
	INT		at = 0, n = 0, total = 0;
	INT		nk = (INT)strlen(key);

	nrow = 0;
	while ( ud_next(dic, dlen, &at, &e) ) {
		total++;
		if ( !ud_match(&e, (CONST UB *)key, nk) ) continue;
		if ( n >= first && nrow < ROWS ) {
			ROW		*r = &row[nrow];
			CONST char	*pn = ud_pos_name(e.pos);
			INT		k;

			copy_to(r->yomi, sizeof(r->yomi), e.yomi, e.nyomi);
			copy_to(r->word, sizeof(r->word), e.word, e.nword);
			r->pos = e.pos;
			/* よみ　単語　〔品詞〕, cut to what a row holds */
			snprintf(s, sizeof(s), "%s　%s", (char *)r->yomi, (char *)r->word);
			k = fit((CONST UB *)s, (INT)strlen(s), ROW_MAX - 24);
			memcpy(text[nrow], s, (size_t)k);
			snprintf(text[nrow] + k, ROW_MAX + 8 - k, "　〔%s〕", pn != NULL ? pn : "");
			nm[nrow] = text[nrow];
			nrow++;
		}
		n++;
	}
	nfound = n;
	ui_names(&ui, P_LIST, nm, nrow);
	ui_set_value(&ui, P_LIST, 0);
	ui_off(&ui, B_PREV, first == 0);
	ui_off(&ui, B_NEXT, first + nrow >= nfound);
	if ( nk > 0 ) {
		snprintf(s, sizeof(s), "%d 語中 %d 語、%d〜%d", total, nfound, nrow > 0 ? first + 1 : 0, first + nrow);
	} else {
		snprintf(s, sizeof(s), "全 %d 語、%d〜%d", total, nrow > 0 ? first + 1 : 0, first + nrow);
	}
	status(s);
}

/* The page that holds the word yomi/word, among those found */
LOCAL void list_at( CONST UB *yomi, CONST UB *word )
{
	UDENT	e;
	INT	at = 0, n = 0, ny = (INT)strlen((CONST char *)yomi), nw = (INT)strlen((CONST char *)word);
	INT	nk = (INT)strlen(key);

	while ( ud_next(dic, dlen, &at, &e) ) {
		if ( !ud_match(&e, (CONST UB *)key, nk) ) continue;
		if ( e.nyomi == ny && e.nword == nw && memcmp(e.yomi, yomi, (size_t)ny) == 0
		  && memcmp(e.word, word, (size_t)nw) == 0 ) {
			first = ( n / ROWS ) * ROWS;
			list_show();
			ui_set_value(&ui, P_LIST, n - first + 1);
			return;
		}
		n++;
	}
	list_show();
}

/* A word picked in the list: put in the boxes above */
LOCAL void list_picked( void )
{
	INT	k = ui_value(&ui, P_LIST) - 1;

	if ( k < 0 || k >= nrow ) {
		return;
	}
	pick = row[k];
	picked = TRUE;
	ui_set_text(&ui, P_YOMI, (CONST char *)pick.yomi);
	ui_set_text(&ui, P_WORD, (CONST char *)pick.word);
	pos_show(pick.pos);
	say("worddic: picked %s %s\n", pick.yomi, pick.word);
}

LOCAL void clear( void )
{
	picked = FALSE;
	ui_set_text(&ui, P_YOMI, "");
	ui_set_text(&ui, P_WORD, "");
	ui_set_value(&ui, P_POS, 1);
	ui_set_value(&ui, P_LIST, 0);
}

/* ---------------------------------------------------------------- 登録, 変更, 削除 */

/* The reading and the word in the boxes; FALSE, said why, when they cannot be a word */
LOCAL BOOL boxes( UB *yomi, UB *word )
{
	char	y[UD_YOMI_MAX * 2], w[UD_WORD_MAX + 8];
	INT	ny, nw;

	ny = ui_text(&ui, P_YOMI, y, sizeof(y));
	nw = ui_text(&ui, P_WORD, w, sizeof(w));
	if ( ny < 0 ) ny = 0;
	if ( nw < 0 ) nw = 0;
	y[( ny < (INT)sizeof(y) ) ? ny : (INT)sizeof(y) - 1] = 0;
	w[( nw < (INT)sizeof(w) ) ? nw : (INT)sizeof(w) - 1] = 0;
	ny = (INT)strlen(y);
	nw = (INT)strlen(w);
	if ( ny == 0 ) {
		warn("読みを入れてください。", NULL);
		return FALSE;
	}
	if ( nw == 0 ) {
		warn("単語を入れてください。", NULL);
		return FALSE;
	}
	if ( ud_yomi((CONST UB *)y, ny, yomi, UD_YOMI_MAX) < 0 ) {
		warn("読みが長すぎるか、読みに使えない文字（空白など）があります。", NULL);
		return FALSE;
	}
	if ( nw >= UD_WORD_MAX ) {
		warn("単語が長すぎます。", NULL);
		return FALSE;
	}
	memcpy(word, w, (size_t)nw + 1);
	return TRUE;
}

/* An answer of ud_put, told */
LOCAL BOOL put_ok( ER er )
{
	if ( er == E_LIMIT ) {
		warn("ユーザ辞書がいっぱいです。", "使わない単語を削除してください。");
	} else if ( er < E_OK ) {
		warn("この単語は登録できません。", "単語にタブや改行は使えません。");
	}
	return (BOOL)( er >= E_OK );
}

LOCAL void keep_failed( ER er )
{
	char	s[64];

	snprintf(s, sizeof(s), "（エラー %d）", (INT)er);
	warn("ユーザ辞書に書き込めませんでした。", s);
}

LOCAL void word_put( void )
{
	UB	y[UD_YOMI_MAX], w[UD_WORD_MAX];
	INT	pos = pos_chosen(), len;
	ER	er;

	if ( !boxes(y, w) ) return;
	to_work(&len);
	if ( !put_ok(ud_put(work, &len, sizeof(work), y, (INT)strlen((char *)y), w,
			    (INT)strlen((char *)w), pos)) ) {
		return;
	}
	er = dic_keep(len);
	if ( er < E_OK ) {
		keep_failed(er);
		return;
	}
	say("worddic: put %s %s %s\n", y, w, ud_pos_name(pos));
	list_at(y, w);
	pick.pos = pos;
	memcpy(pick.yomi, y, sizeof(pick.yomi));
	memcpy(pick.word, w, sizeof(pick.word));
	picked = TRUE;
	status("登録しました");
}

LOCAL void word_change( void )
{
	UB	y[UD_YOMI_MAX], w[UD_WORD_MAX];
	INT	pos = pos_chosen(), len;
	ER	er;

	if ( !picked ) {
		warn("変更する単語を一覧で選んでください。", NULL);
		return;
	}
	if ( !boxes(y, w) ) return;
	to_work(&len);
	(void)ud_del(work, &len, pick.yomi, (INT)strlen((char *)pick.yomi), pick.word,
		     (INT)strlen((char *)pick.word));
	if ( !put_ok(ud_put(work, &len, sizeof(work), y, (INT)strlen((char *)y), w,
			    (INT)strlen((char *)w), pos)) ) {
		return;
	}
	er = dic_keep(len);
	if ( er < E_OK ) {
		keep_failed(er);
		return;
	}
	say("worddic: changed %s %s to %s %s %s\n", pick.yomi, pick.word, y, w, ud_pos_name(pos));
	memcpy(pick.yomi, y, sizeof(pick.yomi));
	memcpy(pick.word, w, sizeof(pick.word));
	pick.pos = pos;
	list_at(y, w);
	status("変更しました");
}

LOCAL void word_delete( void )
{
	UB		y[UD_YOMI_MAX], w[UD_WORD_MAX];
	char		s[UD_YOMI_MAX + UD_WORD_MAX + 32];
	CONST char	*l[2];
	INT		len;
	ER		er;

	if ( picked ) {
		memcpy(y, pick.yomi, sizeof(y));
		memcpy(w, pick.word, sizeof(w));
	} else if ( !boxes(y, w) ) {
		return;
	}
	snprintf(s, sizeof(s), "「%s」（%s）", (char *)w, (char *)y);
	l[0] = s;
	l[1] = "をユーザ辞書から削除します。";
	if ( ui_ask(&ui, PN_DELETE, l, 2) != B_YES ) {
		return;
	}
	to_work(&len);
	if ( ud_del(work, &len, y, (INT)strlen((char *)y), w, (INT)strlen((char *)w)) < E_OK ) {
		warn("その単語は登録されていません。", NULL);
		return;
	}
	er = dic_keep(len);
	if ( er < E_OK ) {
		keep_failed(er);
		return;
	}
	say("worddic: deleted %s %s\n", y, w);
	clear();
	if ( first >= nfound - 1 && first > 0 ) first -= ROWS;
	list_show();
	status("削除しました");
}

LOCAL void find( BOOL all )
{
	INT	n;

	if ( all ) {
		key[0] = 0;
		ui_set_text(&ui, P_FIND, "");
	} else {
		n = ui_text(&ui, P_FIND, key, sizeof(key));
		if ( n < 0 ) n = 0;
		key[( n < (INT)sizeof(key) ) ? n : (INT)sizeof(key) - 1] = 0;
	}
	first = 0;
	list_show();
	say("worddic: found %d for \"%s\"\n", nfound, key);
}

/* ---------------------------------------------------------------- reading in, writing out */

/*
 * The letters of an xmlTAD text, as lines: a paragraph or a break ends
 * a line, a tab is a tab, the other tags are passed over and the
 * entities given back as their letters. Its length.
 */
LOCAL INT tad_lines( CONST UB *s, INT n, UB *out, INT max )
{
	INT	i = 0, o = 0;

	while ( i < n && o < max - 4 ) {
		if ( s[i] == '<' ) {
			INT	e = i + 1;
			BOOL	close = (BOOL)( e < n && s[e] == '/' );
			CONST UB *nm = s + e + ( close ? 1 : 0 );

			while ( e < n && s[e] != '>' ) e++;
			if ( close && strncmp((CONST char *)nm, "p>", 2) == 0 ) out[o++] = '\n';
			if ( !close && strncmp((CONST char *)nm, "br", 2) == 0 ) out[o++] = '\n';
			if ( !close && strncmp((CONST char *)nm, "tab", 3) == 0 ) out[o++] = '\t';
			i = e + 1;
			continue;
		}
		if ( s[i] == '&' ) {
			if ( strncmp((CONST char *)s + i, "&lt;", 4) == 0 ) { out[o++] = '<'; i += 4; continue; }
			if ( strncmp((CONST char *)s + i, "&gt;", 4) == 0 ) { out[o++] = '>'; i += 4; continue; }
			if ( strncmp((CONST char *)s + i, "&amp;", 5) == 0 ) { out[o++] = '&'; i += 5; continue; }
			if ( strncmp((CONST char *)s + i, "&quot;", 6) == 0 ) { out[o++] = '"'; i += 6; continue; }
			if ( strncmp((CONST char *)s + i, "&apos;", 6) == 0 ) { out[o++] = '\''; i += 6; continue; }
		}
		if ( s[i] != '\r' && s[i] != '\n' ) out[o++] = s[i];
		i++;
	}
	return o;
}

/* The words read in from bytes, put among those there are; how many, or below 0 */
LOCAL INT take_in( CONST UB *in, INT n, INT *p_bad )
{
	INT	len, put;
	ER	er;

	to_work(&len);
	put = ud_import(work, &len, sizeof(work), in, n, p_bad);
	if ( put == E_LIMIT ) {
		warn("ユーザ辞書がいっぱいになりました。", "入りきった単語までを登録します。");
		put = ud_count(work, len) - ud_count(dic, dlen);
	}
	if ( put <= 0 ) {
		return put;
	}
	er = dic_keep(len);
	if ( er < E_OK ) {
		keep_failed(er);
		return er;
	}
	return put;
}

/* An object's words: its record 1 as a file, or the text of its record 0 */
LOCAL INT take_obj( CONST TS_UUID *u, INT *p_bad )
{
	T_OBREC	r[8];
	ID	k;
	SZ	size = 0, asz = 0;
	INT	cnt = 0, i, recno = 0, put = 0;
	UB	*b;

	k = ob_opn_obj(u, OB_OP_R);
	if ( k < E_OK ) return (INT)k;
	if ( ob_lst_rec(k, r, 8, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( r[i].recno == 1 && r[i].size > 0 ) {
				recno = 1;
				size = (SZ)r[i].size;
			}
			if ( r[i].recno == 0 && recno == 0 ) size = (SZ)r[i].size;
		}
	}
	if ( size <= 0 || size > (SZ)UD_TEXT_MAX * 2 ) {
		ob_cls_obj(k);
		return 0;
	}
	b = malloc((size_t)size + 1);
	if ( b == NULL ) {
		ob_cls_obj(k);
		return E_NOMEM;
	}
	if ( ob_rea_rec(k, recno, 0, b, size, &asz) >= E_OK ) {
		if ( recno == 1 ) {
			put = take_in(b, (INT)asz, p_bad);
		} else {
			UB	*t = malloc((size_t)asz + 8);

			if ( t != NULL ) {
				put = take_in(t, tad_lines(b, (INT)asz, t, (INT)asz + 8), p_bad);
				free(t);
			}
		}
	}
	free(b);
	ob_cls_obj(k);
	return put;
}

/* What was read in, told */
LOCAL void taken( INT put, INT bad )
{
	char	s[96];

	first = 0;
	key[0] = 0;
	list_show();
	snprintf(s, sizeof(s), "%d 語を読み込みました", put > 0 ? put : 0);
	if ( bad > 0 ) {
		snprintf(s + strlen(s), sizeof(s) - strlen(s), "（読めない行 %d）", bad);
	}
	status(s);
	say("worddic: read in %d bad %d\n", put, bad);
}

LOCAL void drop( void )
{
	T_OBDROP	*d;
	INT		i, put = 0, bad = 0, b, r;

	if ( ( d = ui_drop(&ui) ) == NULL ) return;
	ui_drop_answer(&ui, d, OB_DR_ACCEPT, "");
	for ( i = 0; i < d->n; i++ ) {
		b = 0;
		r = take_obj(&d->v[i].target, &b);
		if ( r > 0 ) put += r;
		bad += b;
	}
	taken(put, bad);
}

/* The UUID of a link in a fragment: id="xxxxxxxx-..._0.xtad" */
LOCAL BOOL link_at( CONST UB *s, INT n, INT *p_at, TS_UUID *u )
{
	char	t[40];
	INT	i;

	for ( i = *p_at; i + 4 + 36 < n; i++ ) {
		if ( strncmp((CONST char *)s + i, "<link", 5) != 0 ) continue;
		for ( ; i + 4 + 36 < n && s[i] != '>'; i++ ) {
			if ( strncmp((CONST char *)s + i, " id=\"", 5) == 0 ) {
				INT	at = i + 5 + ts_lnk_split(s + i + 5, n - i - 5, NULL, NULL);

				if ( at + 36 > n ) break;
				memcpy(t, s + at, 36);
				t[36] = 0;
				*p_at = i + 5;
				return (BOOL)( ui_uuid(t, u) >= E_OK );
			}
		}
	}
	*p_at = n;
	return FALSE;
}

/* 読込み: from the set in hand in the tray -- the objects of its links, or its text */
LOCAL void tray_in( void )
{
	T_TRINF	inf;
	UB	*b;
	SZ	asz = 0;
	INT	i, put = 0, bad = 0, r, x;

	for ( i = 0; i < TR_REC_MAX; i++ ) {
		if ( tr_ref_rec(TR_HAND, i, &inf) < E_OK ) break;
		if ( inf.size <= 0 || inf.size > (SZ)UD_TEXT_MAX * 4 ) continue;
		if ( inf.kind != TR_TAD && inf.kind != TR_REC ) continue;
		b = malloc((size_t)inf.size + 8);
		if ( b == NULL ) break;
		if ( tr_rea_rec(TR_HAND, i, b, inf.size, &asz) >= E_OK ) {
			if ( inf.kind == TR_REC ) {
				x = 0;
				r = take_in(b, (INT)asz, &x);
				if ( r > 0 ) put += r;
				bad += x;
			} else {
				TS_UUID	u;
				INT	at = 0, links = 0;

				while ( link_at(b, (INT)asz, &at, &u) ) {
					x = 0;
					r = take_obj(&u, &x);
					if ( r > 0 ) put += r;
					bad += x;
					links++;
				}
				if ( links == 0 ) {
					UB	*t = malloc((size_t)asz + 8);

					if ( t != NULL ) {
						x = 0;
						r = take_in(t, tad_lines(b, (INT)asz, t, (INT)asz + 8), &x);
						if ( r > 0 ) put += r;
						bad += x;
						free(t);
					}
				}
			}
		}
		free(b);
	}
	if ( i == 0 ) {
		warn("トレーが空です。", NULL);
		return;
	}
	taken(put, bad);
}

/* A letter of the text into the fragment, as xmlTAD writes it */
LOCAL INT xml_put( UB *o, INT at, INT max, CONST UB *s, INT n )
{
	INT	i;

	for ( i = 0; i < n && at < max - 8; i++ ) {
		CONST char	*e = ( s[i] == '<' ) ? "&lt;" : ( s[i] == '>' ) ? "&gt;" : ( s[i] == '&' ) ? "&amp;" : NULL;

		if ( e != NULL ) {
			while ( *e != 0 ) o[at++] = (UB)*e++;
		} else {
			o[at++] = s[i];
		}
	}
	return at;
}

/* 書出し: the words to the tray, a text of their lines */
LOCAL void tray_out( void )
{
	CONST char	*head = "<tad version=\"1.0\" encoding=\"UTF-8\"><document>";
	CONST char	*tail = "</document></tad>";
	T_TRREC		r;
	UDENT		e;
	UB		*o;
	INT		max = dlen * 2 + 4096, at = 0, n = 0;

	if ( ud_count(dic, dlen) == 0 ) {
		warn("書き出す単語がありません。", NULL);
		return;
	}
	o = malloc((size_t)max);
	if ( o == NULL ) return;
	memcpy(o, head, strlen(head));
	at = (INT)strlen(head);
	{
		INT	k = 0;

		while ( ud_next(dic, dlen, &k, &e) ) {
			CONST char	*pn = ud_pos_name(e.pos);

			memcpy(o + at, "<p>", 3);
			at += 3;
			at = xml_put(o, at, max, e.yomi, e.nyomi);
			memcpy(o + at, "<tab/>", 6);
			at += 6;
			at = xml_put(o, at, max, e.word, e.nword);
			memcpy(o + at, "<tab/>", 6);
			at += 6;
			at = xml_put(o, at, max, (CONST UB *)pn, (INT)strlen(pn));
			memcpy(o + at, "</p>", 4);
			at += 4;
			n++;
		}
	}
	memcpy(o + at, tail, strlen(tail));
	at += (INT)strlen(tail);
	memset(&r, 0, sizeof(r));
	r.kind = TR_TAD;
	r.data = o;
	r.size = at;
	if ( tr_psh_dat(&r, 1, (CONST UB *)"ユーザ辞書") < E_OK ) {
		warn("トレーに入れられませんでした。", NULL);
	} else {
		char	s[64];

		snprintf(s, sizeof(s), "%d 語をトレーに書き出しました", n);
		status(s);
		say("worddic: written out %d\n", n);
	}
	free(o);
}

/* The letters chosen in a text, handed over in the tray's place of its own */
LOCAL void handed( void )
{
	T_TRINF	inf;
	UB	b[UD_WORD_MAX * 2];
	SZ	asz = 0;
	INT	n;

	if ( tr_ref_rec(TR_PUT, 0, &inf) < E_OK || inf.kind != TR_REC || inf.sub != UD_TRAY_SUB ) {
		return;
	}
	if ( inf.size > 0 && inf.size < (SZ)sizeof(b)
	  && tr_rea_rec(TR_PUT, 0, b, inf.size, &asz) >= E_OK ) {
		n = fit(b, (INT)asz, UD_WORD_MAX);
		b[n] = 0;
		ui_set_text(&ui, P_WORD, (CONST char *)b);
		say("worddic: handed %s\n", b);
	}
	(void)tr_set_dat(NULL, 0);
}

/* ---------------------------------------------------------------- the menu */

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

LOCAL void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( ui_uuid(SYSDEF_MENU_WORDDIC, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		closing = TRUE;
	} else if ( is_code(&sel, "import") ) {
		tray_in();
	} else if ( is_code(&sel, "export") ) {
		tray_out();
	} else if ( is_code(&sel, "clear") ) {
		clear();
	}
}

/* ---------------------------------------------------------------- what happens */

LOCAL void pressed( INT id )
{
	switch ( id ) {
	case B_PUT:	word_put();		break;
	case B_CHANGE:	word_change();		break;
	case B_DELETE:	word_delete();		break;
	case B_CLEAR:	clear();		break;
	case B_FIND:	find(FALSE);		break;
	case B_ALL:	find(TRUE);		break;
	case B_PREV:
		if ( first > 0 ) {
			first = ( first > ROWS ) ? first - ROWS : 0;
			list_show();
		}
		break;
	case B_NEXT:
		if ( first + nrow < nfound ) {
			first += ROWS;
			list_show();
		}
		break;
	default:
		break;
	}
}

int main( void )
{
	T_OBNTM	m;
	INT	id, ev;

	ui_icon(SYSDEF_PROG_WORDDIC);
	if ( ui_open(&ui, SYSDEF_BOX_WORDDIC, PN_MAIN, 200, 80) < E_OK ) {
		tm_putstring((CONST UB *)"worddic: no window\n");
		return 1;
	}
	if ( dic_read() < E_OK ) {
		warn("ユーザ辞書が見つかりません。", NULL);
	}
	pos_names();
	list_show();
	handed();
	say("worddic: ready %d\n", ud_count(dic, dlen));

	while ( !closing ) {
		for ( TMO tmo = TMO_FEVR; !closing && ui_wait(&ui, &m, tmo) >= E_OK; tmo = TMO_POL ) {
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				closing = TRUE;
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_DROP:
				drop();
				break;
			case UI_EV_KEY:
				if ( ( m.mods & ( 0x01 | 0x10 ) ) != 0 ) {
					UB	c = wm_key_char(m.code, m.mods);

					if ( c == 'e' || c == 'E' ) closing = TRUE;
				}
				break;
			case UI_EV_CHANGE:
				if ( id == P_LIST ) list_picked();
				break;
			case UI_EV_BUTTON:
				pressed(id);
				break;
			default:
				break;
			}
		}
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"worddic: closed\n");
	return 0;
}
