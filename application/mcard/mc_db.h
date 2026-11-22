/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	mc_db.h
 *	マイクロカード: the cards, their record, searching, sorting, marks,
 *	writing out, reading in and merging (design 11.18)
 *
 *	A database is a list of fields (at most MC_FIELD_MAX, each named,
 *	of a kind -- 文字, 数値 or 仮身 -- with its largest size and its width
 *	in the list) and a list of cards, each holding a value for every
 *	field: a text, a number's digits, or the <link> element of one link.
 *	A card can be marked, and found by the last search (the search
 *	state); what is not saved -- the marks, what is found -- lives here
 *	with the rest.
 *
 *	The object keeps the database as the records of マイクロカード実身
 *	(record type 31): the format (subtype 1), the cards (2), their order
 *	(3) and the links (4), byte for byte as 超漢字 and B-right/V keep
 *	them (mc_bin.c). Record 0 is an xmlTAD text that says what the
 *	object is and holds, in order, one <link> for each object the links
 *	point at -- what a link record is elsewhere -- and in a <microcard>
 *	element what TessronOS keeps beside: how the cards are shown, the
 *	faces of the fields and how they are put on paper.
 *
 *	Nothing here makes a system call. Memory is taken with mc_alloc and
 *	given back with mc_free, and the name of the object a link points at
 *	is asked of mc_link_name; the program and the kernel's test each give
 *	their own. What is written goes into the caller's CTBUF.
 */

#ifndef __MC_DB_H__
#define __MC_DB_H__

#include <tk/typedef.h>
#include <tk/errno.h>
#include "../calendar/cal_tad.h"
#include "../calc/calc_tad.h"

#define MC_FIELD_MAX	64		/* fields of a card */
#define MC_NAME_LETTERS	8		/* letters of a field's name */
#define MC_NAME_MAX	40		/* bytes of a field's name, with its terminator */
#define MC_TEXT_MAX	500		/* letters of a text */
#define MC_NUM_MAX	20		/* digits of a number, its point and sign among them */
#define MC_LINK_LETTERS	30		/* the width a link is given, in letters */
#define MC_LINK_MAX	2000		/* links in one object */
#define MC_CARD_MAX	30000		/* cards of one object */
#define MC_WIDTH_MAX	50		/* letters a field is shown in the list */
#define MC_VALUE_MAX	( MC_TEXT_MAX * 4 + 1 )	/* bytes of a text, with its terminator */

/* The kinds of field */
#define MC_T_TEXT	0
#define MC_T_NUM	1
#define MC_T_LINK	2

/* The faces a text is shown in */
#define MC_FACE_STD	0		/* the system's */
#define MC_FACE_MINCHO	1
#define MC_FACE_GOTHIC	2

typedef struct {
	UB	name[MC_NAME_MAX];
	INT	type;			/* MC_T_* */
	INT	max;			/* letters (a number: digits, point and sign) */
	INT	width;			/* letters wide in the list; 0 not shown there */
	INT	face;			/* MC_FACE_* */
} MCFIELD;

typedef struct {
	UB	**v;			/* a value for each field, NULL when empty */
	INT	nv;			/* how many v holds room for */
	BOOL	mark;
	BOOL	found;			/* in the search state, one of the cards found */
} MCCARD;

/* How the cards are shown: 表示設定 */
typedef struct {
	INT	csize, lsize;		/* the letters of the card and of the list, in points */
	BOOL	cprop, lprop;		/* 比例ピッチ */
	BOOL	cgray, lgray;		/* 階調表示 */
	BOOL	names;			/* the fields' names shown on the card */
} MCVIEW;

/* How the cards are put on paper with a layout: 印刷設定 */
#define MC_PRINT_MAX	16
#define MC_LAYOUT_NAME	64
typedef struct {
	UB	layout[MC_LAYOUT_NAME];	/* the layout's name; "" 一覧表(標準形式) */
	INT	across, down;		/* cards on a page each way */
	INT	gapx, gapy;		/* from one card's left (top) to the next's, in 0.1 mm */
	BOOL	downfirst;		/* the cards go down a column first */
	INT	paper;			/* MC_PAPER_* */
	BOOL	turn;			/* the paper on its side */
	INT	margin[4];		/* top, bottom, left, right, mm */
	UB	head[64], foot[64];	/* ヘッダ and フッタ, with the % letters of the pages */
} MCPRINT;

#define MC_PAPER_A4	0
#define MC_PAPER_A3	1
#define MC_PAPER_A5	2
#define MC_PAPER_B4	3
#define MC_PAPER_B5	4
#define MC_PAPER_LETTER	5
#define MC_PAPERS	6

typedef struct {
	MCFIELD	f[MC_FIELD_MAX];
	INT	nf;
	MCCARD	**c;
	INT	n, cap;
	MCVIEW	view;
	UB	*layout;		/* 印刷書式: the <link> of the layout cabinet, NULL none */
	INT	layoutlen;
	MCPRINT	pr[MC_PRINT_MAX];
	INT	npr;
	BOOL	searching;		/* the search state */
} MCDB;

/* ---------------------------------------------------------------- given from outside */

IMPORT void *mc_alloc( INT size );
IMPORT void mc_free( void *p );

/*
 * The name of the object a link points at, as UTF-8 in out: what fits
 * of it, cut at a whole letter. Answers the length of the whole name
 * (bytes, 0 when it has none), which is more than out holds when it was
 * cut; mc_link_name_all has it whole.
 */
IMPORT INT  mc_link_name( CONST UB *el, INT len, UB *out, INT max );
/* The same name whole, in memory of mc_alloc (mc_free it), its length in *p_n; NULL for none */
IMPORT UB   *mc_link_name_all( CONST UB *el, INT len, INT *p_n );

/* ---------------------------------------------------------------- letters */

/* The letter at s (n bytes left) and how many bytes it takes */
IMPORT INT  mc_u8_get( CONST UB *s, INT n, UINT *p_c );
IMPORT INT  mc_u8_put( UINT c, UB *out );		/* bytes written, at most 4 */
IMPORT INT  mc_letters( CONST UB *s, INT n );		/* how many letters */
IMPORT INT  mc_cut( CONST UB *s, INT n, INT letters );	/* the bytes of the first so many letters */
IMPORT INT  mc_len( CONST UB *s );
/* The bytes of a text that go in so many TRON characters, their plane switches counted */
IMPORT INT  mc_tron_fit( CONST UB *s, INT n, INT max );
IMPORT BOOL mc_wide( UINT c );				/* a letter of full width */

/* ---------------------------------------------------------------- the database */

IMPORT void mc_init( MCDB *db );
IMPORT void mc_clear( MCDB *db );		/* everything given back; no fields, no cards */
IMPORT void mc_view_default( MCVIEW *v );
IMPORT void mc_print_default( MCPRINT *p, CONST UB *layout );
IMPORT MCPRINT *mc_print_of( MCDB *db, CONST UB *layout );	/* made when there is none */

/* ---------------------------------------------------------------- the records (mc_bin.c) */

#define MC_RT		31		/* the record type of マイクロカード実身's records */
#define MC_SUB_FORMAT	1		/* フォーマットデータレコード */
#define MC_SUB_CARDS	2		/* カードデータレコード */
#define MC_SUB_INDEX	3		/* インデックスデータレコード */
#define MC_SUB_VOBJ	4		/* 仮身データレコード */
#define MC_SUBS		5

/* The records of a database object as they were read; NULL those it does not have */
typedef struct {
	CONST UB	*x;		/* record 0 */
	INT		xn;
	CONST UB	*r[MC_SUBS];	/* by subtype */
	INT		n[MC_SUBS];
} MCRECS;

/*
 * The records read: E_OK; E_PAR when there is no format record or it is
 * not one this reads (its marks and version), E_OBJ when the cards do
 * not fit the format. A database read always has a card.
 */
IMPORT ER   mc_read( MCDB *db, CONST MCRECS *rs );

/* Record 0 written: what the object is, the objects its links point at, TessronOS's settings */
IMPORT void mc_write_x( CTBUF *b, CONST MCDB *db, CONST UB *name, BOOL found_only );

/* The record of a subtype written: its length, 0 when the object leaves it out */
IMPORT INT  mc_write_bin( CTBUF *b, CONST MCDB *db, INT sub, BOOL found_only );

/* A new vobjid for a link made from a record, as text (given from outside) */
IMPORT void mc_new_vobjid( char *out37 );

/* A number's digits as the DOUBLE the record keeps, and back; an empty number is a NaN */
IMPORT void mc_num_bits( CONST UB *s, UB *out8 );
IMPORT INT  mc_bits_num( CONST UB *in8, UB *out, INT max );

/* A card put in before position at (0 .. n), empty; NULL without memory */
IMPORT MCCARD *mc_card_new( MCDB *db, INT at );
IMPORT MCCARD *mc_card_dup( MCDB *db, INT from, INT at );
IMPORT void mc_card_del( MCDB *db, INT i );
IMPORT void mc_card_blank( MCDB *db, INT i );
IMPORT BOOL mc_card_empty( CONST MCDB *db, INT i );

/*
 * A value set: a text cut to the field's size, without tabs and new
 * lines; a number as mc_num_norm makes it (E_PAR when it is none); a
 * link's element as it is. len 0 empties it.
 */
IMPORT ER   mc_set( MCDB *db, MCCARD *c, INT f, CONST UB *v, INT len );
IMPORT CONST UB *mc_get( CONST MCCARD *c, INT f );

/* What a value shows, as text: a link its object's name; its length */
IMPORT INT  mc_shown( CONST MCDB *db, CONST MCCARD *c, INT f, UB *out, INT max );

/* A number's digits as they are kept ("-12.5"); its length, -1 when it is not a number */
IMPORT INT  mc_num_norm( CONST UB *s, INT n, UB *out, INT max );
/* The number at the start of a text, as far as it goes; its length, 0 none */
IMPORT INT  mc_num_lead( CONST UB *s, INT n, UB *out, INT max );
IMPORT INT  mc_num_cmp( CONST UB *a, CONST UB *b );

/* The links a database holds: in its cards, and its 印刷書式 */
IMPORT INT  mc_link_count( CONST MCDB *db );

/* ---------------------------------------------------------------- the format */

/*
 * A field list checked: its names (not empty, at most 8 letters, of full
 * width, each once), its kinds and sizes. E_OK, or the reason and the
 * field in *p_at: E_PAR a name, E_OBJ a name twice, E_LIMIT a size.
 */
IMPORT ER   mc_fields_check( CONST MCFIELD *f, INT n, INT *p_at );

/*
 * The cards made to a new field list: from[k] is the old field the new
 * field k takes its values from, -1 a new one. mc_format_loss says
 * which new fields lose something on the way (lose[k]) and how many.
 */
IMPORT INT  mc_format_loss( CONST MCDB *db, CONST MCFIELD *f, CONST INT *from, INT n, BOOL *lose );
IMPORT ER   mc_format_set( MCDB *db, CONST MCFIELD *f, CONST INT *from, INT n );

/* ---------------------------------------------------------------- the cards in order */

/* The cards worked on: all of them, or in the search state those found */
IMPORT INT  mc_count( CONST MCDB *db );
IMPORT INT  mc_index( CONST MCDB *db, INT pos );	/* the card at that place among them, -1 */
IMPORT INT  mc_pos( CONST MCDB *db, INT i );		/* the place of card i among them, -1 */
IMPORT BOOL mc_in( CONST MCDB *db, INT i );

/* ---------------------------------------------------------------- searching */

/* A condition of one field, read from what is written for it */
typedef struct mcrx MCRX;
typedef struct {
	INT	op;			/* MC_OP_* */
	UB	*a;			/* what is compared with, NULL none */
	INT	an;
	MCRX	*rx;			/* a regular expression, or a simple one */
	BOOL	wild;			/* * and ? in a = or ≠ */
} MCCOND;

#define MC_OP_NONE	0		/* nothing written: every card */
#define MC_OP_EQ	1
#define MC_OP_NE	2
#define MC_OP_LT	3
#define MC_OP_GT	4
#define MC_OP_LE	5
#define MC_OP_GE	6
#define MC_OP_RX	7		/* / ≒ ≒/: holds a match */

IMPORT ER   mc_cond_make( MCCOND *k, INT type, CONST UB *s, INT n );
IMPORT void mc_cond_free( MCCOND *k );
IMPORT BOOL mc_cond_test( CONST MCDB *db, CONST MCCARD *c, INT f, CONST MCCOND *k );

/* How a search goes */
#define MC_S_NEW	0		/* 新規検索: among all the cards */
#define MC_S_NARROW	1		/* 絞込検索: among those found */

/*
 * The cards that meet the conditions written for the fields (cond[f],
 * NULL or "" none) found: how many, or the error of the field *p_at.
 * The search state stays even when none was found.
 */
IMPORT INT  mc_search( MCDB *db, CONST UB *CONST *cond, INT how, INT *p_at );
IMPORT void mc_search_off( MCDB *db );
IMPORT INT  mc_mark_search( MCDB *db, BOOL add );	/* マーク付カード新規検索, 追加検索 */
IMPORT INT  mc_dups( MCDB *db, INT f );			/* 重複検索 */

/* ---------------------------------------------------------------- marks */

#define MC_M_FLIP_ALL	0
#define MC_M_ALL	1
#define MC_M_NONE	2
#define MC_M_BEFORE_ON	3
#define MC_M_BEFORE_OFF	4
#define MC_M_AFTER_ON	5
#define MC_M_AFTER_OFF	6
IMPORT INT  mc_marks( MCDB *db, INT how, INT cur );	/* among the cards worked on; how many marked */
IMPORT INT  mc_mark_next( CONST MCDB *db, INT pos, INT d );	/* the next marked place, -1 */

/* ---------------------------------------------------------------- order */

/* Two texts in the order 並べ替え puts them */
IMPORT INT  mc_text_cmp( CONST UB *a, INT an, CONST UB *b, INT bn );
IMPORT INT  mc_val_cmp( CONST MCDB *db, INT f, CONST MCCARD *a, CONST MCCARD *b );

/* The cards worked on put in the order of field f; the others keep their places */
IMPORT ER   mc_sort( MCDB *db, INT f, BOOL desc );

/* ---------------------------------------------------------------- regular expressions (mc_rx.c) */

#define MC_RX_SIMPLE	0x0001		/* 簡易正規表現: * ? | \ */
#define MC_RX_FOLD	0x0002		/* 異形字ゆらぎ同一視 */

IMPORT ER   mc_rx_make( CONST UB *pat, INT n, UINT flags, MCRX **p_rx );
IMPORT void mc_rx_free( MCRX *rx );
IMPORT BOOL mc_rx_find( CONST MCRX *rx, CONST UB *s, INT n );
IMPORT BOOL mc_wild( CONST UB *pat, INT pn, CONST UB *s, INT n );	/* * and ?, the whole */
IMPORT UINT mc_fold( UINT c );			/* the letter a variant stands for */
IMPORT UINT mc_sort_key( UINT c, UINT prev );	/* where a letter goes in order */
IMPORT INT  mc_charset( UINT c );		/* MC_CS_* */

#define MC_CS_JIS12	1		/* JIS第1第2水準 (and the letters of the system's script) */
#define MC_CS_JIS3	2
#define MC_CS_JIS4	3
#define MC_CS_SUPP	4		/* JIS X 0212 */
#define MC_CS_CHINA	5
#define MC_CS_KOREA	6
#define MC_CS_TAIWAN	7
#define MC_CS_OTHER	8		/* letters of other countries */

/* The variants and the letters they stand for (mc_var.c), in the order of the variants */
IMPORT CONST UINT mc_var_tab[][2];
IMPORT CONST INT  mc_var_n;

/* ---------------------------------------------------------------- writing out, reading in (mc_io.c) */

IMPORT void mc_out_at( CTBUF *b, CONST MCDB *db, BOOL found_only );	/* @形式, as text */
/* Lines of values, a card a line: the places from..to of the cards worked on (from -1 all) */
#define MC_OUT_HEAD	0x0001		/* the fields' names first */
#define MC_OUT_CSV	0x0002		/* commas between, not tabs */
#define MC_OUT_QUOTE	0x0004		/* texts in quotes */
IMPORT void mc_out_rows( CTBUF *b, CONST MCDB *db, INT from, INT to, BOOL found_only, UINT how );
IMPORT void mc_out_sheet( CXSHEET *s, CONST MCDB *db, BOOL found_only );	/* 表計算 */

/* Plain text as a text record: each line a paragraph, each tab <tab/> */
IMPORT void mc_text_doc( CTBUF *b, CONST UB *t, INT n, CONST UB *name );

/* The letters of an xmlTAD text: a paragraph or a break ends a line, a tab is a tab */
IMPORT INT  mc_doc_text( CONST UB *x, INT n, UB *out, INT max );

/*
 * Cards read in from a text before position at: @形式, or tab- or
 * comma-separated lines whose first is the fields' names. The values go
 * to the fields of the same names, made to their kinds and sizes; a
 * database with no fields takes the text's. How many cards, or below 0.
 */
#define MC_IN_AUTO	0
#define MC_IN_AT	1
#define MC_IN_TAB	2
#define MC_IN_CSV	3
IMPORT INT  mc_in_text( MCDB *db, CONST UB *t, INT n, INT at, INT form );
IMPORT INT  mc_in_db( MCDB *db, CONST MCDB *src, INT at );
IMPORT INT  mc_in_sheet( MCDB *db, CONST CXSHEET *s, INT at );

/* Lines without names: each a card, its tab-separated values in the fields from the first */
IMPORT INT  mc_in_rows( MCDB *db, CONST UB *t, INT n, INT at );

/* ---------------------------------------------------------------- merging (mc_merge.c) */

/* What the % letters of a layout say besides a card's values */
typedef struct {
	UB	*name;			/* %N: the object's name, whole (mc_alloc), NULL for none */
	INT	y, m, d;		/* the day it is put on paper */
	INT	pages;			/* %T */
} MCMINFO;

/*
 * Where the pages go: start once with the kind of record they make and
 * the page's size in dots, page for each page (its elements, in the
 * page's own place), end once. The writer of an object (mc_obj.c) puts
 * them in one record; another writer -- to PDF -- takes the same pages.
 */
#define MC_PG_DOC	0		/* the pages are texts */
#define MC_PG_FIG	1		/* the pages are figures */
typedef struct {
	void	*arg;
	void	(*start)( void *arg, INT kind, INT w, INT h );
	void	(*page)( void *arg, CONST UB *body, INT n );
	void	(*end)( void *arg );
} MCSINK;

/*
 * The writer to 印刷 (mc_prn.c): each page made a record of xmlTAD of
 * its own on the 印刷設定's paper and handed to put, in mem (max bytes)
 * the caller gives. er is the first thing that went wrong.
 */
typedef struct {
	ER	(*put)( void *arg, CONST UB *xml, INT n );
	void	*arg;
	UB	*mem;
	INT	max;
	INT	kind, w, h, m[4];
	INT	pages;
	ER	er;
} MCPSINK;

IMPORT void mc_prn_sink( MCSINK *sink, MCPSINK *ps, CONST MCPRINT *p, UB *mem, INT max,
			 ER (*put)( void *arg, CONST UB *xml, INT n ), void *arg );

/* The paper of a 印刷設定 in dots, and its margins */
IMPORT void mc_paper( CONST MCPRINT *p, INT *p_w, INT *p_h, INT *margin );

/* The cards to be put on paper: the one at cur, or all those worked on; how many */
IMPORT INT  mc_targets( CONST MCDB *db, BOOL one, INT cur, INT *idx, INT max );

/* A layout's text with the % letters made the card's (and the pages') */
IMPORT void mc_subst( CTBUF *b, CONST UB *s, INT n, CONST MCDB *db, CONST MCCARD *c, INT all_i, INT tgt_i,
		      INT ntgt, CONST MCMINFO *mi, INT page, BOOL esc );

/* How many pages the cards take with a layout of the kind ("" the list) */
IMPORT INT  mc_pages( CONST MCDB *db, CONST MCPRINT *p, BOOL fig, BOOL list, INT ntgt );

/*
 * The cards idx[0..n) merged into the layout x (its record 0; NULL the
 * list as it is shown), page by page into the sink: E_OK, E_LIMIT past
 * 500 pages.
 */
IMPORT ER   mc_merge( CONST MCDB *db, CONST MCPRINT *p, CONST UB *x, INT xn, CONST INT *idx, INT n,
		      CONST MCMINFO *mi, CONST MCSINK *sink, UB *work, INT wmax );

/* A figure's elements moved by dx, dy (their boxes and points) */
IMPORT void mc_shift( CTBUF *b, CONST UB *s, INT n, INT dx, INT dy );

#endif /* __MC_DB_H__ */
