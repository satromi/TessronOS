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

LOCAL UI	ui;
LOCAL BOOL	closing;

/* The dictionary's text and a copy changed before it is kept: both as long as the words need */
LOCAL UB	*dic;
LOCAL INT	dlen, dcap;
LOCAL UB	*work;
LOCAL INT	wcap;

/* The page of the list shown: the words on it, as they were when it was made */
typedef struct {
	char	*yomi;
	char	*word;
	INT	pos;
} ROW;

LOCAL ROW	row[ROWS];
LOCAL INT	nrow;
LOCAL INT	first;			/* the first word of the page, among those found */
LOCAL INT	nfound;
LOCAL char	key_none[1];
LOCAL char	*key = key_none;	/* what is looked for; "" all */

/* The word picked in the list, which 変更 and 削除 act on */
LOCAL BOOL	picked;
LOCAL ROW	pick;

LOCAL void warn( CONST char *a, CONST char *b );

/* A string made by printf's rules, in memory of its own (free it); NULL without memory */
LOCAL char *fmt_new( CONST char *fmt, ... )
{
	va_list	ap;
	char	*s;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if ( n < 0 || ( s = (char *)malloc((size_t)n + 1) ) == NULL ) return NULL;
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return s;
}

/* n bytes of s as a string of its own; NULL without memory */
LOCAL char *dup_n( CONST UB *s, INT n )
{
	char	*t = (char *)malloc((size_t)n + 1);

	if ( t != NULL ) {
		memcpy(t, s, (size_t)n);
		t[n] = 0;
	}
	return t;
}

LOCAL void row_set( ROW *r, CONST UB *y, INT ny, CONST UB *w, INT nw, INT pos )
{
	free(r->yomi);
	free(r->word);
	r->yomi = dup_n(y, ny);
	r->word = dup_n(w, nw);
	r->pos = pos;
}

LOCAL void key_set( CONST char *s )
{
	char	*t = dup_n((CONST UB *)s, (INT)strlen(s));

	if ( key != key_none ) free(key);
	key = ( t != NULL ) ? t : key_none;
}

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

/* The size of record 1 of an object: the words in it */
LOCAL SZ rec1_size( ID k )
{
	T_OBREC	r[8];
	INT	cnt = 0, i;
	SZ	size = 0;

	if ( ob_lst_rec(k, r, 8, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt && i < 8; i++ ) {
			if ( r[i].recno == 1 ) size = (SZ)r[i].size;
		}
	}
	return size;
}

LOCAL ER dic_read( void )
{
	TS_UUID	u;
	ID	k;
	SZ	asz = 0, size;
	UB	*b;
	ER	er = E_OK;

	dlen = 0;
	if ( dic_uuid(&u) < E_OK ) return E_SYS;
	k = ob_opn_obj(&u, OB_OP_R);
	if ( k < E_OK ) return (ER)k;
	size = rec1_size(k);
	b = (UB *)malloc((size_t)size + 1);
	if ( b == NULL ) {
		ob_cls_obj(k);
		return E_NOMEM;
	}
	if ( size > 0 ) {
		er = ob_rea_rec(k, 1, 0, b, size, &asz);
	}
	ob_cls_obj(k);
	free(dic);
	dic = b;
	dcap = (INT)size + 1;
	if ( er >= E_OK ) dlen = (INT)( ( asz > size ) ? size : asz );
	return ( er == E_NOEXS ) ? E_OK : er;		/* no record yet: no words */
}

/* The words in work kept: written to the object, and taken as the dictionary */
LOCAL ER dic_keep( INT len )
{
	TS_UUID	u;
	ID	k;
	UB	*nd = NULL;
	ER	er;

	if ( dic_uuid(&u) < E_OK ) return E_SYS;
	if ( len + 1 > dcap && ( nd = (UB *)malloc((size_t)len + 1 + 4096) ) == NULL ) return E_NOMEM;
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( k < E_OK ) {
		free(nd);
		return (ER)k;
	}
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ob_rpl_rec(k, 1, work, (SZ)len);
	ob_cls_obj(k);
	if ( er >= E_OK ) {
		if ( nd != NULL ) {
			free(dic);
			dic = nd;
			dcap = len + 1 + 4096;
			nd = NULL;
		}
		memcpy(dic, work, (size_t)len);
		dlen = len;
	}
	free(nd);
	return er;
}

/* The dictionary copied to work, with room for extra bytes more; FALSE, said, without memory */
LOCAL BOOL to_work( INT *p_len, INT extra )
{
	INT	need = dlen + extra + 1;

	if ( need < dlen || need > wcap ) {
		UB	*nw = ( need > dlen ) ? (UB *)malloc((size_t)need + 1024) : NULL;

		if ( nw == NULL ) {
			warn("メモリが足りません。", NULL);
			return FALSE;
		}
		free(work);
		work = nw;
		wcap = need + 1024;
	}
	memcpy(work, dic, (size_t)dlen);
	*p_len = dlen;
	return TRUE;
}

/* ---------------------------------------------------------------- words to the person */

/* A line to the console, which the tests read */
LOCAL void say( CONST char *fmt, ... )
{
	va_list	ap;
	char	*s;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if ( n < 0 || ( s = (char *)malloc((size_t)n + 1) ) == NULL ) return;
	va_start(ap, fmt);
	vsnprintf(s, (size_t)n + 1, fmt, ap);
	va_end(ap);
	tm_putstring((CONST UB *)s);
	free(s);
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
	char		*text[ROWS];
	CONST char	*nm[ROWS];
	char		s[96];
	UDENT		e;
	INT		at = 0, n = 0, total = 0, i;
	INT		nk = (INT)strlen(key);

	for ( i = 0; i < nrow; i++ ) {
		free(row[i].yomi);
		free(row[i].word);
		row[i].yomi = row[i].word = NULL;
	}
	nrow = 0;
	while ( ud_next(dic, dlen, &at, &e) ) {
		total++;
		if ( !ud_match(&e, (CONST UB *)key, nk) ) continue;
		if ( n >= first && nrow < ROWS ) {
			ROW		*r = &row[nrow];
			CONST char	*pn = ud_pos_name(e.pos);

			row_set(r, e.yomi, e.nyomi, e.word, e.nword, e.pos);
			/* よみ　単語　〔品詞〕: whole, the list clips what its width does not show */
			text[nrow] = ( r->yomi != NULL && r->word != NULL )
				? fmt_new("%s　%s　〔%s〕", r->yomi, r->word, pn != NULL ? pn : "") : NULL;
			nm[nrow] = ( text[nrow] != NULL ) ? text[nrow] : "";
			nrow++;
		}
		n++;
	}
	nfound = n;
	ui_names(&ui, P_LIST, nm, nrow);
	for ( i = 0; i < nrow; i++ ) free(text[i]);
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

	if ( k < 0 || k >= nrow || row[k].yomi == NULL || row[k].word == NULL ) {
		return;
	}
	row_set(&pick, (CONST UB *)row[k].yomi, (INT)strlen(row[k].yomi),
		(CONST UB *)row[k].word, (INT)strlen(row[k].word), row[k].pos);
	if ( pick.yomi == NULL || pick.word == NULL ) {
		picked = FALSE;
		return;
	}
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

/*
 * The reading and the word in the boxes, each in memory of its own (free
 * both); FALSE, said why, when they cannot be a word
 */
LOCAL BOOL boxes( UB **p_y, UB **p_w )
{
	char	*y = ui_text_all(&ui, P_YOMI), *w = ui_text_all(&ui, P_WORD);
	UB	*yo = NULL;
	INT	ny, nw;

	if ( y == NULL || w == NULL ) {
		free(y);
		free(w);
		warn("メモリが足りません。", NULL);
		return FALSE;
	}
	ny = (INT)strlen(y);
	nw = (INT)strlen(w);
	if ( ny == 0 ) {
		warn("読みを入れてください。", NULL);
	} else if ( nw == 0 ) {
		warn("単語を入れてください。", NULL);
	} else if ( ( yo = (UB *)malloc((size_t)ny + 1) ) == NULL ) {
		warn("メモリが足りません。", NULL);
	} else if ( ud_yomi((CONST UB *)y, ny, yo, ny + 1) < 0 ) {
		free(yo);
		yo = NULL;
		warn("読みに使えない文字（空白など）があります。", NULL);
	}
	free(y);
	if ( yo == NULL ) {
		free(w);
		return FALSE;
	}
	*p_y = yo;
	*p_w = (UB *)w;
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

/* The room a word takes in the text: its two parts, the name of a part of speech and the marks between */
#define LINE_EXTRA	64

LOCAL void word_put( void )
{
	UB	*y, *w;
	INT	pos = pos_chosen(), len, ny, nw;
	ER	er;

	if ( !boxes(&y, &w) ) return;
	ny = (INT)strlen((char *)y);
	nw = (INT)strlen((char *)w);
	if ( !to_work(&len, ny + nw + LINE_EXTRA) ) goto done;
	if ( !put_ok(ud_put(work, &len, wcap, y, ny, w, nw, pos)) ) goto done;
	er = dic_keep(len);
	if ( er < E_OK ) {
		keep_failed(er);
		goto done;
	}
	say("worddic: put %s %s %s\n", y, w, ud_pos_name(pos));
	list_at(y, w);
	row_set(&pick, y, ny, w, nw, pos);
	picked = ( pick.yomi != NULL && pick.word != NULL );
	status("登録しました");
done:
	free(y);
	free(w);
}

LOCAL void word_change( void )
{
	UB	*y, *w;
	INT	pos = pos_chosen(), len, ny, nw;
	ER	er;

	if ( !picked ) {
		warn("変更する単語を一覧で選んでください。", NULL);
		return;
	}
	if ( !boxes(&y, &w) ) return;
	ny = (INT)strlen((char *)y);
	nw = (INT)strlen((char *)w);
	if ( !to_work(&len, ny + nw + LINE_EXTRA) ) goto done;
	(void)ud_del(work, &len, (CONST UB *)pick.yomi, (INT)strlen(pick.yomi), (CONST UB *)pick.word,
		     (INT)strlen(pick.word));
	if ( !put_ok(ud_put(work, &len, wcap, y, ny, w, nw, pos)) ) goto done;
	er = dic_keep(len);
	if ( er < E_OK ) {
		keep_failed(er);
		goto done;
	}
	say("worddic: changed %s %s to %s %s %s\n", pick.yomi, pick.word, y, w, ud_pos_name(pos));
	row_set(&pick, y, ny, w, nw, pos);
	picked = ( pick.yomi != NULL && pick.word != NULL );
	list_at(y, w);
	status("変更しました");
done:
	free(y);
	free(w);
}

/* The word y/w taken out of the dictionary after asking */
LOCAL void delete_word( CONST UB *y, CONST UB *w )
{
	char		*s = fmt_new("「%s」（%s）", (CONST char *)w, (CONST char *)y);
	CONST char	*l[2];
	INT		len;
	ER		er;

	if ( s == NULL ) {
		warn("メモリが足りません。", NULL);
		return;
	}
	l[0] = s;
	l[1] = "をユーザ辞書から削除します。";
	er = ui_ask(&ui, PN_DELETE, l, 2);
	free(s);
	if ( er != B_YES ) {
		return;
	}
	if ( !to_work(&len, 0) ) return;
	if ( ud_del(work, &len, y, (INT)strlen((CONST char *)y), w, (INT)strlen((CONST char *)w)) < E_OK ) {
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

LOCAL void word_delete( void )
{
	UB	*y, *w;

	if ( picked ) {
		y = (UB *)dup_n((CONST UB *)pick.yomi, (INT)strlen(pick.yomi));
		w = (UB *)dup_n((CONST UB *)pick.word, (INT)strlen(pick.word));
		if ( y == NULL || w == NULL ) {
			free(y);
			free(w);
			warn("メモリが足りません。", NULL);
			return;
		}
	} else if ( !boxes(&y, &w) ) {
		return;
	}
	delete_word(y, w);
	free(y);
	free(w);
}

LOCAL void find( BOOL all )
{
	if ( all ) {
		key_set("");
		ui_set_text(&ui, P_FIND, "");
	} else {
		char	*t = ui_text_all(&ui, P_FIND);

		key_set(( t != NULL ) ? t : "");
		free(t);
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
	INT	len, put, extra = n + 4096;
	ER	er;

	/* a line takes a part of speech's name more than it had: room is made again, larger, until all fit */
	for ( ;; ) {
		if ( !to_work(&len, extra) ) return E_NOMEM;
		put = ud_import(work, &len, wcap, in, n, p_bad);
		if ( put != E_LIMIT || (long long)extra > 32LL * n + 65536 ) break;
		extra *= 2;
	}
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
	if ( size <= 0 || size > (SZ)0x3FFFFFFF ) {		/* lengths are INT */
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
	key_set("");
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
		if ( inf.size <= 0 || inf.size > (SZ)0x3FFFFFFF ) continue;	/* lengths are INT */
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
	UB	*b;
	SZ	asz = 0;

	if ( tr_ref_rec(TR_PUT, 0, &inf) < E_OK || inf.kind != TR_REC || inf.sub != UD_TRAY_SUB ) {
		return;
	}
	if ( inf.size > 0 && inf.size < (SZ)0x3FFFFFFF && ( b = (UB *)malloc((size_t)inf.size + 1) ) != NULL ) {
		if ( tr_rea_rec(TR_PUT, 0, b, inf.size, &asz) >= E_OK ) {
			b[( asz < inf.size ) ? asz : inf.size] = 0;
			ui_set_text(&ui, P_WORD, (CONST char *)b);
			say("worddic: handed %s\n", b);
		}
		free(b);
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
	if ( ui_open(&ui, SYSDEF_BOX_WORDDIC, PN_MAIN, wm_new_x(200), wm_new_y(80)) < E_OK ) {
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
