/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	udic.h
 *	ユーザ辞書: the words a person registers for かな漢字変換 (design 11.16)
 *
 *	The dictionary is text, UTF-8, a line to a word:
 *
 *	  よみ<TAB>単語<TAB>品詞<LF>
 *
 *	the form the common converters write their user dictionaries out in
 *	and read them in from, and the form it is kept in: record 1 of the
 *	user dictionary object (SYSDEF_DICT_USER). The lines are kept in the
 *	order of their readings, and of their words under one reading; one
 *	word is in it once under a reading. A part of speech is one of the
 *	names below; their numbers are the converter's (mozc's PosType).
 *
 *	Reading other files in, lines starting with ! or # are passed over
 *	(the heading lines another converter writes), UTF-16 with its byte
 *	order mark is read as such, the names of the parts of speech other
 *	converters use are taken as the nearest of these, and a reading in
 *	katakana is taken in hiragana.
 *
 *	Nothing here makes a system call or takes memory: what is read is
 *	the caller's bytes, what is written goes into the caller's buffer.
 *	The converter (application/kconv), the accessory 単語登録
 *	(application/worddic) and the kernel's test all use it.
 */

#ifndef __TS_UDIC_H__
#define __TS_UDIC_H__

#include <tk/typedef.h>
#include <tk/errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A reading, a word and the dictionary have no length limit here: the
 * functions below work on the lengths they are given and copy nothing
 * but into the buffer the caller sizes (the dictionary's own buffer
 * with its max, the buffer ud_yomi writes a reading into: the reading's
 * length plus 1 is enough).
 */

/*
 * The letters chosen in a text to be registered, handed to 単語登録 as
 * it is started: the tray's place of its own (TR_PUT) holds one record
 * of TR_REC with this subtype, the letters in UTF-8.
 */
#define UD_TRAY_SUB	0x5744

/* Parts of speech: mozc's numbers (1 .. UD_POS_MAX) */
#define UD_POS_NOUN		1	/* 名詞 */
#define UD_POS_ABBREV		2	/* 短縮よみ */
#define UD_POS_PROPER		4	/* 固有名詞 */
#define UD_POS_PERSON		5	/* 人名 */
#define UD_POS_FAMILY		6	/* 姓 */
#define UD_POS_FIRST		7	/* 名 */
#define UD_POS_ORG		8	/* 組織 */
#define UD_POS_PLACE		9	/* 地名 */
#define UD_POS_SAHEN		10	/* 名詞サ変 */
#define UD_POS_KEIDOU		11	/* 名詞形動 */
#define UD_POS_EMOTICON		15	/* 顔文字 */
#define UD_POS_MAX		44

/* One word of the dictionary, as it lies in the text */
typedef struct {
	CONST UB	*yomi;
	INT		nyomi;
	CONST UB	*word;
	INT		nword;
	INT		pos;		/* UD_POS_* */
	INT		at;		/* where its line starts in the text */
	INT		end;		/* and where the next starts */
} UDENT;

/* A part of speech's name, or NULL; and the part a name stands for (any converter's names), 0 for none */
IMPORT CONST char *ud_pos_name( INT pos );
IMPORT INT  ud_pos_of( CONST UB *name, INT n );

/* The parts the accessory offers, in the order it offers them */
IMPORT INT  ud_pos_offer( INT k );		/* the k-th (from 0), 0 past the last */

/* The words in order: the next from *p_at on, FALSE when there are no more */
IMPORT BOOL ud_next( CONST UB *text, INT len, INT *p_at, UDENT *e );
IMPORT INT  ud_count( CONST UB *text, INT len );

/*
 * The words read as yomi exactly, in their order, up to max of them:
 * how many there are. yomi is taken in hiragana.
 */
IMPORT INT  ud_lookup( CONST UB *text, INT len, CONST UB *yomi, INT nyomi, UDENT *e, INT max );

/*
 * Whether a word is found by key: its reading starts with it, or its
 * word holds it. Full-width letters and half-width ones are the same
 * here, and katakana and hiragana.
 */
IMPORT BOOL ud_match( CONST UDENT *e, CONST UB *key, INT nkey );

/*
 * A reading as it is kept: katakana as hiragana, the spaces round it
 * taken off. Its length, or E_PAR when it is empty, does not fit in max
 * (its length is below max), or has a letter a reading cannot have (a
 * space, a tab, a control).
 */
IMPORT INT  ud_yomi( CONST UB *in, INT n, UB *out, INT max );

/*
 * A word put in its place: under the same reading the same word has its
 * part of speech changed. E_OK, E_PAR for a reading or word that cannot
 * be (empty, a tab or a line's end in it, a part unknown),
 * E_LIMIT when the text would not fit in max.
 */
IMPORT ER   ud_put( UB *text, INT *p_len, INT max, CONST UB *yomi, INT nyomi,
		    CONST UB *word, INT nword, INT pos );

/* A word taken out: E_OK, E_NOEXS when it is not there */
IMPORT ER   ud_del( UB *text, INT *p_len, CONST UB *yomi, INT nyomi, CONST UB *word, INT nword );

/*
 * A file of words read in (another converter's, or one written out
 * here) and every word in it put: how many were put. *p_bad says how
 * many lines were not words (left out). E_LIMIT when the dictionary
 * filled up before the end; what was put stays. A line of any length is
 * read; a UTF-16 file needs a free place at the end of text as long as
 * its longest line in UTF-8, and is E_LIMIT without it.
 */
IMPORT INT  ud_import( UB *text, INT *p_len, INT max, CONST UB *in, INT nin, INT *p_bad );

/* The words as the engine is given them: "よみ<TAB>単語<TAB>番号<LF>" lines; the length, or E_LIMIT */
IMPORT INT  ud_engine( CONST UB *text, INT len, UB *out, INT max );

/* Katakana in s turned into hiragana where they stand (UTF-8, the same length) */
IMPORT void ud_hira( UB *s, INT n );

#ifdef __cplusplus
}
#endif

#endif /* __TS_UDIC_H__ */
