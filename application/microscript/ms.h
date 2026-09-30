/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ms.h
 *	マイクロスクリプト: the pieces of the program and what they share
 *	(design 17.15)
 *
 *	A figure is the stage, its groups named with an "@name" text are the
 *	segments (the actors), and the text objects linked into it whose
 *	names begin with SCRIPT are the script. The script is read into a
 *	tree and run by threads that take turns: a thread gives the others
 *	their turn when it sleeps, waits, or goes round a loop.
 */

#ifndef __MS_H__
#define __MS_H__

#include <tk/typedef.h>
#include <ts/uapp.h>
#include <stddef.h>

/* ---------------------------------------------------------------- memory and text */

void	*ms_alloc( size_t n );		/* zeroed; the program ends when there is none */
void	*ms_realloc( void *p, size_t n );
void	ms_free( void *p );
char	*ms_strdup( const char *s );

/* UTF-8 */
INT	ms_utf8_dec( const UB *s, INT n, UW *p_cp );	/* bytes taken, 0 at the end */
INT	ms_utf8_enc( UW cp, UB *out );			/* bytes put */
UW	ms_norm_cp( UW cp );		/* full width ASCII and a few signs to plain */

/* A growing byte buffer */
typedef struct {
	char	*s;
	INT	n, cap;
} MSBUF;

void	mb_init( MSBUF *b );
void	mb_free( MSBUF *b );
void	mb_putn( MSBUF *b, const char *s, INT n );
void	mb_puts( MSBUF *b, const char *s );
void	mb_putc( MSBUF *b, char c );
void	mb_putcp( MSBUF *b, UW cp );
void	mb_printf( MSBUF *b, const char *fmt, ... );
void	mb_xml( MSBUF *b, const char *s, INT n );	/* escaped for XML text */

/* Names, kept once each so that they are compared by address */
typedef struct msstr {
	struct msstr	*next;
	UW		hash;
	INT		len;
	char		s[];
} MSSTR;

MSSTR	*ms_intern( const char *s, INT n );
MSSTR	*ms_intern_z( const char *s );
MSSTR	*ms_upper( MSSTR *s );		/* the same name in capitals (ASCII) */

/* ---------------------------------------------------------------- XML */

typedef struct msxa {
	const char	*name;
	INT		nlen;
	char		*val;		/* entities replaced, NUL ended */
} MSXA;

typedef struct msx {
	struct msx	*parent, *first, *last, *next;
	const char	*tag;		/* NULL for text */
	INT		tlen;
	MSXA		*attr;
	INT		nattr;
	char		*text;		/* text node: entities replaced */
	INT		start, end;	/* the element in the source, tags and all */
} MSX;

typedef struct {
	const char	*src;
	INT		len;
	MSX		*root;
} MSXDOC;

ER	msx_parse( MSXDOC *d, const char *src, INT len );
BOOL	msx_is( const MSX *e, const char *tag );
const char *msx_attr( const MSX *e, const char *name );
double	msx_num( const MSX *e, const char *name, double dflt );
MSX	*msx_find( MSX *e, const char *tag );		/* first descendant */
void	msx_text( const MSX *e, MSBUF *b, BOOL br_newline );	/* text inside */

/* ---------------------------------------------------------------- the figure */

typedef struct {
	double	l, t, r, b;
	BOOL	ok;
} MSBOX;

/* A segment as the figure gives it */
typedef struct msfseg {
	MSSTR	*name;
	MSX	*group;		/* its element */
	MSX	*label;		/* the "@name" text, not drawn */
	MSBOX	box;		/* what is drawn, in the figure's coordinates */
	double	gl, gt;		/* the group's own corner, the label and all */
	char	*frag;		/* a figure of its own to draw it with */
	INT	fraglen;
	char	*frag_notext;	/* the same without its texts (TEXT wrote over them) */
	INT	fraglen_notext;
	char	*text;		/* its first text other than the label, UTF-8 */
	UW	textcol;	/* that text's colour, or 0xFFFFFFFF */
	double	textsize;	/* and its size, 0 when it does not say */
	MSBOX	tbox;		/* and where it stands */
	BOOL	hastextbox;
	BOOL	haslink, alltext;
	BOOL	isvobj;		/* a virtual object of the figure: a segment by its object's name */
	TS_UUID	target;		/* and what it points at */
	UW	*img;		/* newimgseg: its pixels, the part to show at img */
	UW	*imgbuf;	/* the whole picture */
	INT	imgw, imgh, imgpitch;
} MSFSEG;

typedef struct {
	MSX	*node;
	TS_UUID	target;
	char	*name;		/* the object's name */
} MSFLINK;

typedef struct {
	MSXDOC	doc;
	char	*head;		/* <pattern>/<mask> definitions every piece carries */
	INT	headlen;
	char	*back;		/* the shapes that are not segments */
	INT	backlen;
	MSFSEG	*seg;
	INT	nseg;
	MSFLINK	*link;
	INT	nlink;
	MSBOX	view;		/* <figView>, when there is one */
	UW	paper;		/* the ground */
} MSFIG;

ER	ms_fig_read( MSFIG *f, const char *xml, INT len );
void	ms_fig_add_vobj( MSFIG *f, INT link, MSSTR *name );
void	ms_fig_order( MSFIG *f );

/* Virtual objects opened, closed and waited for */
INT	ms_pid_of( const TS_UUID *proc );
ER	ms_vopen_obj( const TS_UUID *target, INT *p_pid );
ER	ms_vopen_tool( const char *name, INT *p_pid );
void	ms_vclose( const TS_UUID *targets, INT nt, const char (*names)[64], INT nn );
BOOL	ms_vopen_any( const TS_UUID *targets, INT nt, const char (*names)[64], INT nn );
void	ms_vobj_end( void );

/* What every script on the machine shares: $GV and the global names */
INT	ms_gv_get( INT i );
void	ms_gv_set( INT i, INT v );
BOOL	ms_gnm_get( const char *name, INT *p_val );
BOOL	ms_gnm_set( const char *name, INT val );
BOOL	ms_gnm_del( const char *name );
void	ms_shared_end( void );
BOOL	ms_label_name( const MSX *e, char *out, INT max );
BOOL	ms_uuid( const char *s, TS_UUID *u );	/* "xxxxxxxx-xxxx-..." */
void	ms_uuid_str( const TS_UUID *u, char *out );	/* and back: 37 bytes */

/* ---------------------------------------------------------------- the script: tokens */

enum {
	T_IDENT = 1, T_INT, T_FLOAT, T_STRING, T_SYSVAR, T_OP, T_NL, T_EOF
};

typedef struct {
	UB	type;
	UB	spaced;		/* space came before it */
	UH	op;		/* T_OP: one of the OP_ below; T_NL: ';' or '\n' */
	INT	line;
	double	num;
	MSSTR	*name;		/* T_IDENT, T_SYSVAR */
	UW	*chars;		/* T_STRING */
	INT	nchars;
} MSTOK;

/* Operators, one or two characters, kept as their characters */
#define OPC(a, b)	( (UH)( ( (UH)(UB)(a) << 8 ) | (UB)(b) ) )

typedef struct {
	MSTOK	*t;
	INT	n, cap;
} MSTOKS;

ER	ms_lex( const char *src, INT len, MSTOKS *out, char *err, INT errmax );

/* ---------------------------------------------------------------- the script: the tree */

enum {
	/* expressions */
	N_NUM = 1, N_STR, N_UNARY, N_BIN, N_SYSVAR, N_CALL, N_NAME,
	/* statements */
	S_LOCAL, S_SET, S_IF, S_WHILE, S_REPEAT, S_SWITCH, S_BREAK, S_CONTINUE,
	S_EXIT, S_CALL, S_EXECUTE, S_FINISH, S_TERMINATE, S_SUSPEND, S_SLEEP,
	S_WAIT, S_SCENE, S_APPEAR, S_DISAPPEAR, S_MOVE, S_BEEP, S_WSIZE,
	S_WMOVE, S_WSAVE, S_FULLWIND, S_UPDATE, S_MESG, S_LOG, S_TEXT, S_INPUT,
	S_KINPUT, S_VOPEN, S_VCLOSE, S_VWAIT, S_SETSEG, S_COPYSEG, S_EVENT,
	S_FOPEN, S_FCLOSE, S_FREAD, S_FWRITE, S_PROCESS, S_PWAIT, S_MSEND,
	S_MRECV, S_HWNOOP, S_RSINIT, S_RSPUT, S_RSPUTN, S_RSWAIT, S_RSGETN,
	S_RSGETC, S_RSCNTL,
	/* TADjs's: the console and the network (ms_net.c) */
	S_CONSOLE, S_SPRINTF, S_HTTPREQ, S_HTTPHDR, S_JSONGET, S_JSONLEN,
	S_TCPOPEN, S_TCPSEND, S_TCPWAIT, S_TCPRECV, S_TCPCLOSE
};

/* Flags */
#define NF_FLOAT	0x0001		/* N_NUM written with a point or an exponent */
#define NF_SLICE	0x0002		/* N_NAME: [a:b] */
#define NF_SSLICE	0x0004		/* N_NAME: .STATE[a:b] */
#define NF_DEFERRED	0x0008		/* a display statement ended with ';' */
#define NF_DUP		0x0010		/* MOVE ...:DUP */
#define NF_BASE_AT	0x0020		/* MOVE ... @ */
#define NF_SPACED	0x0040

typedef struct msn MSN;

/* A declaration: VARIABLE, LOCAL, SEGMENT, a parameter */
typedef struct {
	MSSTR	*name;
	char	type;		/* B C I F S G */
	MSN	*size;		/* NULL: 1; for a parameter written [] it is 0 */
	BOOL	open;		/* a parameter written [] */
	MSSTR	*share;		/* =name */
	MSN	*share_off;
} MSDECL;

struct msn {
	UH	k;		/* N_ or S_ */
	UH	flags;
	UH	op;		/* operator; the effect of SCENE and the like; EVENT's kind */
	INT	line;
	double	num;
	MSSTR	*name;		/* the name; for N_NAME with a state, the variable's */
	MSSTR	*name2;		/* N_NAME: the state; the variable EXECUTE puts the number in */
	UW	*chars;		/* N_STR */
	INT	nchars;
	/*
	 * Children by use:
	 * N_UNARY a; N_BIN a b; N_SYSVAR a = index; N_NAME a = index or
	 * slice start, b = slice length, c = state index or start, d = state
	 * slice length. Statements: a, b, c, d as each needs them.
	 */
	MSN	*a, *b, *c, *d;
	MSN	**v;		/* lists: arguments, values, statements, segments */
	INT	nv;
	MSN	**w;		/* a second list: bodies of an IF, cases */
	INT	nw;
	MSDECL	*decl;		/* S_LOCAL */
	INT	ndecl;
};

/* Procedures */
enum { P_PROLOGUE = 1, P_EPILOGUE, P_ACTION, P_MACTION, P_FUNC };
enum { EV_NONE = 0, EV_PRESS, EV_CLICK, EV_DCLICK, EV_QPRESS, EV_MENU, EV_KEY };

typedef struct msproc {
	INT	type;
	MSSTR	*name;
	MSDECL	*param;
	INT	nparam;
	INT	event;
	MSSTR	**target;
	INT	ntarget;
	UW	*menu;		/* MENU "label" */
	INT	nmenu;
	MSN	**body;
	INT	nbody;
	INT	running;	/* threads running it */
} MSPROC;

typedef struct {
	MSSTR	*name;
	MSTOK	*body;
	INT	nbody;
	MSN	*expr;		/* made the first time it is used as a value */
	BOOL	tried;
} MSMACRO;

typedef struct {
	INT	version;
	INT	debug;
	MSDECL	*global;
	INT	nglobal;
	MSDECL	*segdecl;
	INT	nsegdecl;
	MSPROC	**proc;
	INT	nproc;
	MSMACRO	*macro;
	INT	nmacro;
} MSPROG;

ER	ms_parse( MSTOKS *toks, MSPROG *prog, char *err, INT errmax );
MSN	*ms_parse_expr_toks( const MSTOK *t, INT n );
MSMACRO	*ms_macro( MSPROG *p, MSSTR *name );

/* ---------------------------------------------------------------- values */

enum { V_NUM = 0, V_STR, V_ARR };

typedef struct msarr MSARR;

typedef struct {
	UB	t;
	union {
		double	n;
		MSSTR	*s;
		MSARR	*a;
	} u;
} MSV;

struct msarr {
	INT	ref;
	INT	n;
	MSV	e[];
};

#define MS_INVALID	(-2147483648.0)

MSV	mv_num( double n );
MSV	mv_str( MSSTR *s );
MSV	mv_arr( INT n );		/* zeros, one reference */
MSV	mv_chars( const UW *c, INT n );	/* the characters and a 0 */
MSV	mv_cstr( const char *utf8 );	/* the same from UTF-8 */
MSV	mv_ref( MSV v );		/* one more reference */
void	mv_drop( MSV v );
BOOL	mv_is_invalid( MSV v );
double	mv_n( MSV v );			/* as a number (an array: its first) */
INT	mv_i( MSV v );			/* as a 32 bit integer, wrapped */
void	mv_text( MSV v, MSBUF *b );	/* as text: an array up to its 0 */

/* ---------------------------------------------------------------- the stage */

typedef struct msseg {
	MSSTR	*name;		/* the name the script uses */
	MSSTR	*key;		/* unique: the name, or the name and a number */
	MSFSEG	*fs;		/* where its drawing comes from, or NULL */
	double	x, y, x0, y0, hx, hy, w, h;
	double	lox, loy;	/* the label's offset from what is drawn */
	BOOL	visible, positioned, textover, tbset;
	INT	z;
	INT	pid;
	struct msseg *shared;	/* SETSEG: drawn as that one */
	UW	*tx;		/* the text, its characters and a 0 */
	INT	ntx;
	double	tfcol, tbcol, tstyl, tsize, tcgap, tlgap;
	UW	tfont[13];
	struct msseg *next;
	struct msseg *hnext;	/* in its bucket */
	struct msseg *dupof;	/* a copy MOVE ... DUP left: the segment it is of */
} MSSEG;

/* ---------------------------------------------------------------- run time */

typedef struct msvar {
	MSSTR	*name;
	char	type;
	INT	size;
	BOOL	scalar, segref, segname;
	MSV	*data;		/* the elements, unless raw */
	INT	avail;		/* elements there are behind data */
	BOOL	owned;		/* data is this variable's own */
	UB	*raw;		/* shared bytes: elements of `type` over them */
	INT	rawoff;
	INT	rawlen;
	BOOL	rawowned;
	struct msvar *next;
} MSVAR;

typedef struct msscope {
	struct msscope	*parent;
	MSVAR		*vars;
	MSV		*args;		/* $ARG */
	INT		nargs;
	BOOL		hasargs;
} MSSCOPE;

struct msthread;

/* What the pieces of the program ask of each other */
void	ms_message( const char *utf8 );		/* MESG */
void	ms_console( BOOL on );			/* CONSOLE: the messages over the whole window */
BOOL	ms_net_granted( const char *host );	/* in the figure's "networkGrants" */
void	ms_net_grant( const char *host );
INT	ms_net_ask( const char *host );		/* the person asked: 1 allowed, 0 not, -1 stopped */
void	ms_net_end( void );
void	ms_message_clear( void );
void	ms_dirty( void );			/* the stage wants drawing */
void	ms_render_now( void );
void	ms_finish( void );			/* FINISH: after the epilogue, close */
UD	ms_now_ms( void );
void	ms_yield( UD wake_ms );			/* the thread lets the others run */

void	st_init( MSFIG *f, UW paper );
MSSEG	*st_seg( MSSTR *key );			/* by key, or NULL */
MSSEG	*st_seg_occ( MSSTR *name, INT occ );
INT	st_seg_count( MSSTR *name );
MSSEG	*st_add( MSSTR *name, MSFSEG *fs );
void	st_remove( MSSEG *s );
MSSEG	*st_first( void );
void	st_front( MSSEG *s );
void	st_sort_by_place( void );
MSSEG	*st_hit( INT x, INT y, BOOL (*has)( MSSEG * ) );
void	st_render( INT gid, INT w, INT h, const T_DPRECT *clip );
INT	st_effect_rects( const char *effect, double ratio, const T_DPRECT *box, INT steps,
			 T_DPRECT *out, INT max );
void	st_view( double x, double y );
double	st_vx( void );
double	st_vy( void );
BOOL	st_shown( const MSSEG *s );
void	st_input_text( const UW *c, INT n );
void	st_text_set( MSSEG *s, const UW *c, INT n );
void	st_input_begin( MSSEG *s, BOOL kanji, INT x, INT y );
void	st_input_end( void );
BOOL	st_input_key( UW ch );
MSSEG	*st_input_seg( void );
BOOL	st_input_kanji( void );

/* かな漢字変換 for KINPUT */
BOOL	ms_ime_key( UINT key, UINT mods );
BOOL	ms_ime_busy( void );
void	ms_ime_commit( void );
BOOL	ms_ime_press( INT x, INT y );
void	ms_ime_draw( INT gid, INT x, INT y, INT px, INT lh );
void	ms_ime_end( void );

INT	rt_load( MSPROG *p );
void	rt_start( void );
void	rt_stop( void );
BOOL	rt_finished( void );
void	rt_event( INT kind, MSSEG *seg, INT x, INT y );	/* PRESS CLICK DCLICK QPRESS */
void	rt_key( UW ch, UW meta );
void	rt_menu( INT index );
INT	rt_menu_count( void );
const UW *rt_menu_label( INT index, INT *p_n );
BOOL	rt_has_handler( MSSEG *s, INT kind );
BOOL	rt_has_any_handler( MSSEG *s );
BOOL	rt_has_dclick( MSSEG *s );
void	rt_run_threads( void );			/* each ready thread once */
BOOL	rt_ready( void );			/* a thread wants to run now */
UD	rt_next_wake( void );
void	rt_error( const char *utf8 );
void	rt_fmt( MSV fmt, MSV *args, INT nargs, MSBUF *out );
void	rt_preset_sv( INT i, INT v );

/* What the window knows */
typedef struct {
	INT	pdx, pdy, pdb, pds;
	UW	lastkey, metakey, kstat;
	INT	wdx, wdy, wdw, wdh, scrw, scrh;
	BOOL	update;		/* UPDATE 0 stops the drawing */
	TS_UUID	fig;		/* the figure it runs */
	ID	kfig;		/* its key */
	INT	wid;
	UD	start_ms;
} MSWIN;

extern MSWIN	ms_win;
extern MSFIG	ms_fig;
extern MSPROG	ms_prog;

/* Links of the figure by name, for FOPEN and VOPEN */
#define MS_NAME		256		/* bytes of an object's name */
BOOL	ms_link_by_name( const char *name, TS_UUID *u );
INT	ms_links_of( const TS_UUID *u, TS_UUID *out, char (*names)[MS_NAME], INT max );
BOOL	ms_path_resolve( const char *path, TS_UUID *out );

#endif /* __MS_H__ */
