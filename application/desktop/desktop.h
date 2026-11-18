/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	desktop.h
 *	What the parts of the desktop share (design 16.5.12)
 *
 *	The desktop is the system's work, not a program with windows of
 *	objects: every object, the first window's cabinet too, is shown by
 *	a program that is a process (仮身一覧 for the first window). It reads
 *	what the person does and hands it to the window it is for, carries
 *	links between the programs' windows, starts the programs, and does
 *	for them what only it can (dtreq.h). The windows it keeps itself
 *	are those of its own parts (DTOWN): 通知, and what the system shows
 *	when its disk is out.
 */

#ifndef __DESKTOP_H__
#define __DESKTOP_H__

#include <ts/wm.h>
#include <ts/part.h>
#include <ts/tad.h>
#include <ts/dtreq.h>
#include <ts/tadview.h>
#include <ts/mn.h>
#include <ts/ob.h>
#include <ts/print.h>

#define DT_MAX_WIN	32		/* windows this program may have open */
#define DT_PAPER_W	595		/* A4, when a document names no paper */
#define DT_PAPER_H	842

/* One window of a part of the desktop's (DTOWN) */
typedef struct dtwin {
	BOOL		used;
	INT		wid;
	ID		okey;		/* the window as an object, watched for OB_E_CLOSE */
	TS_UUID		id;		/* the object it stands for; 0 none */
	UINT		tool;		/* DT_TOOL_OWN */
	UB		*name;		/* the title it was opened with, whole (Kmalloc); NULL for want of memory */

	/* 全画面表示, and where the window stood before it */
	BOOL		full;
	T_DPRECT	restore;

	/* what the part keeps for the window, and how it works it */
	CONST struct dtown	*own;
	void		*self;
} DTWIN;

#define DT_TOOL_NONE	0
#define DT_TOOL_OWN	5		/* a window with an own part (DTOWN) */

/*
 * What a window of the desktop's whose work area is its own part's does.
 * The desktop keeps it as it keeps any of its windows: the frame moves
 * it, sizes it and closes it, a press brings it to the front, the
 * second button opens its menu -- the part's definition 'menu', which
 * holds 閉じる and the tail of ウインドウ, 小物 and 実行 as every
 * window's does -- and a letter with Ctrl chooses the item that has it.
 * What is left is the part's:
 *
 *   draw	the work area drawn (opened, sized, uncovered)
 *   input	an event in the work area: the first button pressed in it
 *		and let go, a move, a key while it has the keys, the
 *		wheel; TRUE when it was taken. A key with Ctrl it does not
 *		take is looked for in the menu.
 *   link_at	the link at a place of the work area, for 実行; may be NULL
 *   menu_set	the menu's items set to what the window is; may be NULL
 *   menu_do	an item of the part's chosen; the window may be gone after
 *   closing	whether it may be closed now; NULL when always
 *   close	what the part keeps freed, the window still there
 *   poll	each turn of the desktop's loop; may be NULL
 *   lost	the volumes attached changed: whether what the part shows
 *		holds a record of the store's of an object that can no
 *		longer be found (om_store_lost), in which case the window is
 *		closed before the store lets go of it; NULL when it holds
 *		none past drawing
 */
typedef struct dtown {
	CONST char	*menu;
	void		(*draw)( DTWIN *d );
	BOOL		(*input)( DTWIN *d, CONST T_WMEV *ev );
	BOOL		(*link_at)( DTWIN *d, INT x, INT y, T_VOBJ *v );
	void		(*menu_set)( DTWIN *d, ID mid );
	void		(*menu_do)( DTWIN *d, CONST T_MNSEL *sel );
	BOOL		(*closing)( DTWIN *d );
	void		(*close)( DTWIN *d );
	void		(*poll)( DTWIN *d );
	BOOL		(*lost)( DTWIN *d );
} DTOWN;

/*
 * A window of such a part opened at o with the attributes and title
 * given, the icon of the program of id 'prog' in its band (NULL: none):
 * NULL when there was no room. dt_own_next goes through those of a part
 * open, after 'd' (NULL: from the first).
 */
IMPORT DTWIN *dt_own_open( CONST DTOWN *own, void *self, CONST T_DPRECT *o, UINT attr,
			   CONST char *title, CONST char *prog );
IMPORT DTWIN *dt_own_next( CONST DTOWN *own, DTWIN *d );
IMPORT void   dt_own_ground( DTWIN *d );	/* the work area laid with a panel's ground */

/* desktop.c */
IMPORT DTWIN *dt_win_of( INT wid );

/*
 * A window carried by the hand: moved at once, or with LK_LIVE at 0 an
 * outline in its place until the button is let go (dt_carry_end, TRUE
 * when the window went there).
 */
IMPORT BOOL   dt_carry_window( INT wid, CONST T_DPRECT *o );
IMPORT BOOL   dt_carry_end( INT wid );
/* Whether dt_quit waits for the session to end: a panel being worked is put away, nobody being left to answer it */
IMPORT BOOL   dt_stopping( void );
/* Links carried shown as themselves when LK_LIVE says so: the places they are taken from */
IMPORT void   dt_carry_looks( CONST T_DPRECT *r, INT n, INT dx, INT dy );

/*
 * dttray.c: what a window copies goes to the tray (design 18.16) as an
 * xmlTAD fragment, a <figure> of shapes and links or a <document> of
 * text; each kind of window takes from the set in hand what it can.
 */
#define DT_TRAY_NONE	0
#define DT_TRAY_FIG	1
#define DT_TRAY_DOC	2
IMPORT T_TAD *dt_frag_new( CONST char *body );
IMPORT ER     dt_tray_put( CONST T_TAD *frag, CONST char *name );
IMPORT T_TAD *dt_tray_frag( void );
IMPORT UINT   dt_tray_kind( INT *p_links );
IMPORT INT    dt_frag_shapes( CONST T_TAD *frag );
IMPORT void   dt_tray_taken( void );
IMPORT INT    dt_tray_pictures( CONST TS_UUID *id, CONST T_TAD *rec, T_TAD *frag );
IMPORT INT    dt_roots( TS_UUID *ids, INT max );
IMPORT void   dt_work_rect( CONST DTWIN *d, T_DPRECT *r );
IMPORT void   dt_draw( DTWIN *d );
IMPORT void   dt_draw_all( void );	/* the parts' windows, and the programs' told to draw again */
IMPORT CONST char *dt_base_name( CONST TS_UUID *target );
IMPORT void   dt_open_vobj( DTWIN *from, CONST T_VOBJ *v );
/* Above 0 while what is opened is opened by no act of a person: it does not take the keys */
IMPORT INT    dt_opens_quiet;
IMPORT ER     dt_open_as( DTWIN *from, CONST T_VOBJ *v, CONST UB *app );
IMPORT void   dt_exec( DTWIN *from, CONST T_VOBJ *v, INT index );

/*
 * Links let go of over a window that is not the desktop's (design
 * 16.5.21): handed to the program there when it takes drops, and a
 * word on the message line when it does not. TRUE when it was such a
 * window, and nothing else is to be done with the links.
 */
IMPORT BOOL   dt_drop_links( DTWIN *d, INT sx, INT sy, CONST T_VOBJ *v, INT n );
/* the same, put at ax, ay of the screen: the corner of the outline carried, let go at sx, sy */
IMPORT BOOL   dt_drop_links_at( DTWIN *d, INT sx, INT sy, INT ax, INT ay, CONST T_VOBJ *v, INT n );

/* The desktop run in a task of its own, and stopped again (the tests) */
IMPORT ER     dt_start( void );
IMPORT void   dt_quit( void );
#if USE_KTEST
/* The tests stop the desktop's loop: 1 waits there, 2 runs on the spot, 0 lets it go on */
IMPORT volatile INT dt_test_stall;
#endif
#if USE_KTEST || defined(USE_UITEST)
IMPORT UW dt_test_loops( void );
#endif


/* The programs as real objects (dtprog.c, design 18.12) */
#define DT_PROG_ID	40
#define DT_PROG_BASE	4
#define DT_VER_MAX	16		/* R, a number, a point and three digits */
#define DT_TYPES_MAX	64
#define DT_PROG_ALIAS	2		/* ids a program was known by before */
#define DT_PK_APP	1		/* an application: templates, opens objects */
#define DT_PK_ACCESSORY	2		/* 小物: no data of its own */
#define DT_PK_UTILITY	3		/* a tool of the system */
#define DT_PK_COMMAND	4		/* no window of its own: run in a コンソール */

typedef struct {
	UB	id[DT_PROG_ID];		/* what an applist names it by */
	UB	*name;			/* whole (Kmalloc), never NULL once read: dt_prog_clear lets it go */
	UINT	kind;			/* DT_PK_* */
	TS_UUID	uuid;			/* the program object */
	INT	nbase;
	TS_UUID	base[DT_PROG_BASE];	/* its templates (原紙) */
	BOOL	builtin;		/* carried in the desktop: not replaced or deleted */
	BOOL	menu;			/* an accessory shown in the 小物 menu */
	UB	version[DT_VER_MAX];	/* R1.000; empty when its metadata says none */
	UB	types[DT_TYPES_MAX];	/* the kinds of object 実行 offers it for (fileTypes): "xtad,tad" */
	INT	nalias;
	UB	alias[DT_PROG_ALIAS][DT_PROG_ID];	/* the ids it had before ("aliases"): an applist naming one opens it */
	BOOL	open_link;		/* "openArg": "link": started with the link it was opened from (T_PRCOPEN) */
	BOOL	once;			/* "once": one window whatever it is run on: run again, that window told */
} DTPROG;

IMPORT ER     dt_prog_start( void );
/* What an entry read by dt_prog_read holds let go: its name */
IMPORT void   dt_prog_clear( DTPROG *p );
/* The program an applist names by id: the one with that id, else the one that had it before */
IMPORT CONST DTPROG *dt_prog_find( CONST UB *id );
IMPORT CONST DTPROG *dt_prog_of( CONST TS_UUID *uuid );
IMPORT ER     dt_command_run( CONST TS_UUID *target );	/* a command run in a コンソール; E_NOEXS: none */
IMPORT CONST DTPROG *dt_prog_at( INT i );
IMPORT INT    dt_prog_list( UINT kind, CONST DTPROG **out, INT max );
IMPORT INT    dt_prog_count( UINT kind );		/* how many dt_prog_list has to give */
/*
 * The programs an object is offered with: one list, which 実行 of the
 * link's menu and 付箋指定 of 管理情報 both show. Those its applist names
 * (the link's own applist when the object has none), in its order; when
 * neither names any, the programs that say they open an object of its
 * kind (fileTypes: "xtad" for xmlTAD, else the extension of the file it
 * was taken in from). Each program once, under the id and name the system has it by.
 * dt_app_list with out NULL answers how many entries are room enough;
 * else how many it gave, and *p_dflt (when not NULL) the place of the
 * one the object opens with, -1 none.
 */
typedef struct {
	UB	id[DT_PROG_ID];		/* the program's id */
	UB	raw[DT_PROG_ID];	/* as the applist writes it (another id it had); "" not in the applist */
	UB	*name;			/* whole (Kmalloc), never NULL: dt_app_free lets it go */
	BOOL	listed;			/* named by the applist; FALSE: offered for the object's kind */
} DTAPP;
IMPORT INT    dt_app_list( CONST T_VOBJ *v, DTAPP *out, INT max, INT *p_dflt );
/* The n entries dt_app_list gave, their names and the table (Kmalloc) let go; a NULL: nothing */
IMPORT void   dt_app_free( DTAPP *a, INT n );
/* DT_RQ_APPS: an entry of that list for a program's link, as ts/dtreq.h says */
IMPORT ER     dt_apps_req( CONST T_DTREQ *rq, T_DTANS *an );
/* DT_RQ_LINK_STATE: whether a program's link's object is open, and can be fixed to a program */
IMPORT ER     dt_link_state_req( CONST T_DTREQ *rq, T_DTANS *an );
IMPORT INT    dt_prog_roots( TS_UUID *ids, INT max );
IMPORT CONST char *dt_prog_base_name( CONST TS_UUID *target );
IMPORT BOOL   dt_base_box( TS_UUID *p_box );
IMPORT void   dt_base_box_put( CONST TS_UUID *prog, BOOL add );
/* An object made from a template made the person's; a new 原紙箱 given the system's templates */
IMPORT void   dt_template_made( CONST TS_UUID *tmpl, CONST TS_UUID *made );
/*
 * A program run. One that is a process is started by a task of its own
 * (dtprog.c): E_OK means it was handed over, and the desktop goes on
 * while the program is read in.
 */
IMPORT ER     dt_prog_run( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v );
/*
 * The object v points at opened in the window of a program that is a
 * process. Until that window is up, dt_prog_opening is
 * TRUE for the object; dt_prog_opening_poll, from the desktop's loop,
 * forgets the starts that are over.
 */
IMPORT ER     dt_prog_open( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v );
/*
 * A program started as a process with a start-up argument of its own,
 * size bytes of arg (as long as a start-up argument may be,
 * TS_SPN_ARG_MAX): ディスク操作 is told so what it is to do (T_DTDISKARG)
 */
IMPORT ER     dt_prog_run_arg( DTWIN *from, CONST DTPROG *p, CONST void *arg, SZ size );
IMPORT BOOL   dt_prog_opening( CONST TS_UUID *obj );
IMPORT void   dt_prog_opening_keys( CONST TS_UUID *obj );	/* asked again: its window takes the keys when up */
IMPORT void   dt_prog_opening_poll( void );
/*
 * The answer to a request of a process (T_DTANS, 'size' bytes of it),
 * handed to the next process dt_prog_run starts: the task that starts
 * it answers at 'reply' once the process is there, with the process,
 * and with the error of the start when 'with_er'. NULL takes back what
 * no start took. dt_prog_reply_taken says whether a start took it.
 */
IMPORT void   dt_prog_reply( CONST TS_UUID *reply, UINT seq, SZ size, BOOL with_er );
IMPORT BOOL   dt_prog_reply_taken( void );
/*
 * What the desktop opens by itself as it starts: the processes started
 * while dt_prog_watch(TRUE) holds are watched, and dt_prog_settled is
 * TRUE once each has shown its first picture (wm_prc_drawn) or ended,
 * saying how many drew, how many are gone and how many were watched
 */
IMPORT void   dt_prog_watch( BOOL on );
/* The starts asked now are of a group opened together, at this place in it (wm_set_rank); 0 ends it */
IMPORT void   dt_prog_rank( UINT group, UINT rank );
IMPORT BOOL   dt_prog_settled( INT *p_drawn, INT *p_gone, INT *p_all );
/*
 * An answer written at a channel (desktop.c), in the form of the request
 * it answers: 'size' is sizeof(T_DTANS) for a request that had the tail's
 * size, and the size of an older form otherwise. dt_answer writes one
 * with no words (any task); dt_answer_words one with the words of a
 * DTANS (below; the desktop's task), in its tail for the form of now,
 * in its text for an older form when they fit (E_LIMIT in the answer
 * when they do not).
 */
IMPORT void   dt_answer( CONST TS_UUID *reply, CONST T_DTANS *an, SZ size );
struct dtans;
IMPORT void   dt_answer_words( CONST TS_UUID *reply, CONST struct dtans *a, SZ size );

/*
 * The words of a request (its tail, DT_TAIL_* of ts/dtreq.h). A request
 * is taken in the form of now whatever form it was written in: one of a
 * size before the tail has its fixed name, text, questions and word on
 * the button put into the tail as it is taken, and those fields are
 * emptied in every request, so the words are read only with dt_rq_word,
 * "" when there is none (*p_len 0). dt_rq_applist, dt_rq_view and
 * dt_rq_rel are the applist the request carries (DT_F_APPLIST), the
 * viewMode and the relationship of its link: an older form's name and
 * text are put into those parts as the request is taken. dt_rq_dup copies a request with its tail (Kfree
 * it); dt_rq_reword puts a word in place of the one of its kind, no
 * longer than the old (E_LIMIT otherwise). dt_rq_tailed says whether
 * the request being done had the tail's size: only then is the one who
 * asked answered and told with a tail.
 */
IMPORT CONST UB *dt_rq_word( CONST T_DTREQ *rq, UINT kind, SZ *p_len );
IMPORT CONST UB *dt_rq_applist( CONST T_DTREQ *rq, SZ *p_len );
IMPORT CONST UB *dt_rq_view( CONST T_DTREQ *rq, SZ *p_len );
IMPORT CONST UB *dt_rq_rel( CONST T_DTREQ *rq, SZ *p_len );
IMPORT T_DTREQ  *dt_rq_dup( CONST T_DTREQ *rq );
IMPORT ER        dt_rq_reword( T_DTREQ *rq, UINT kind, CONST UB *s, SZ len );
IMPORT BOOL      dt_rq_tailed( void );
IMPORT SZ        dt_rq_anssz( void );		/* the size its answer is written in (dt_answer) */

/*
 * An answer being made, with its words as long as they are: the text put
 * together piece by piece (DT_TAIL_TEXT) and the 固有データ (DT_TAIL_DATA).
 * The handlers of the requests are given &a->an of a DTANS begun with
 * dt_ans_begin and put the words with dt_ans_cat, dt_ans_num and
 * dt_ans_data; dt_answer sends them with it; dt_ans_end lets them go. A
 * word that cannot be kept (no memory) makes the answer E_NOMEM.
 * dt_ans_text gives the text put so far ("" none), dt_ans_untext empties
 * it. They do nothing (E_PAR) to a T_DTANS that is no begun DTANS's.
 */
typedef struct dtans {
	T_DTANS		an;		/* what the handler fills in */
	UB		*text;		/* the text so far, ending in 0; NULL none */
	SZ		textlen, textcap;
	UB		*data;		/* the 固有データ; NULL none */
	SZ		datalen;
	BOOL		hasdata;
	ER		er;		/* E_NOMEM when a word could not be kept */
	struct dtans	*next;		/* the answers being made */
} DTANS;
IMPORT void      dt_ans_begin( DTANS *a, UINT seq );
IMPORT void      dt_ans_end( DTANS *a );
IMPORT ER        dt_ans_cat( T_DTANS *an, CONST UB *s, SZ len );
IMPORT ER        dt_ans_num( T_DTANS *an, D v );
IMPORT ER        dt_ans_data( T_DTANS *an, CONST UB *s, SZ len );
IMPORT CONST UB *dt_ans_text( CONST T_DTANS *an, SZ *p_len );
IMPORT void      dt_ans_untext( T_DTANS *an );
/* The answer of a as it is written in the form of now, its tail after it (Kfree it); NULL no memory */
IMPORT T_DTANS  *dt_ans_message( CONST DTANS *a );
/*
 * A program told at its channel (OB_O_NOWAIT, then OB_E_CHANGE) with an
 * and its words, in the form of now when it asked in it (tail), else in
 * the older one (dt_answer_words); len < 0 up to the 0. *p_gone: the
 * channel could not be opened (its process ended).
 */
IMPORT ER        dt_tell_words( CONST TS_UUID *chan, CONST T_DTANS *an, BOOL tail,
				CONST UB *text, SZ tl, CONST UB *data, SZ dl, BOOL *p_gone );

/* The program box read again: what the registry knows is what the box links to */
IMPORT void   dt_prog_reread( void );

/*
 * dtpkg.c: applications installed from a package (.tpk), updated and
 * deleted (design 18.12). A version R1.000 as a number to compare
 * (1000), -1 when it is not one.
 */
IMPORT D      dt_ver_num( CONST UB *v );
#define DT_PKG_NEW	DT_HOW_NEW
#define DT_PKG_UPDATED	DT_HOW_UPDATED
IMPORT ER     dt_pkg_admin( ID pid, CONST TS_UUID *win );
IMPORT INT    dt_admin_panels( void );
IMPORT ER     dt_pkg_install( CONST TS_UUID *pkg, BOOL force, ID pid, CONST TS_UUID *win,
			      TS_UUID *p_prog, UINT *p_how );
IMPORT ER     dt_prog_remove( CONST TS_UUID *prog );
IMPORT ER     dt_prog_drop( CONST TS_UUID *prog );
IMPORT BOOL   dt_prog_read( CONST TS_UUID *uuid, DTPROG *p );

/* dtacc.c: the order of the accessories and which of them 小物 lists (design 11.2.1) */
IMPORT INT    dt_acc_menu_list( CONST DTPROG ***p_acc );
IMPORT void   dt_acc_sync( void );
IMPORT void   dt_acc_place( CONST TS_UUID *target, CONST TS_UUID *before );
IMPORT ER     dt_acc_req( CONST T_DTREQ *rq, T_DTANS *an );	/* DT_RQ_ACC_LIST, DT_RQ_ACC_SET */
IMPORT void   dt_acc_boxes_tell( void );
IMPORT INT    dt_acc_pos( CONST UB *id, INT *p_n, BOOL *p_show );

/*
 * dtsysup.c: the system updated from its package (design 11.8).
 * dt_sysup_check says what a package is (DT_SYSUP_CHECK of ts/dtreq.h,
 * text the versions); dt_sysup_begin begins the update for process pid
 * (force: even when it is not newer), the steps there are in *p_total;
 * dt_sysup_step does the next of them for that process; dt_sysup_run
 * does them all; dt_sysup_req answers DT_RQ_SYSUP; dt_sysup_resume, at
 * the start, does again an update that was begun and not ended.
 * dt_sysup_need: the bytes of the boot partition the package's boot
 * files take more than now, and the bytes free there. dt_sysup_test,
 * for the tests: the boot partition's root taken to be root (NULL:
 * /boot), and the commit stopped after cut renames (-1: not stopped).
 */
IMPORT ER     dt_sysup_check( CONST TS_UUID *pkg, UB *text, INT max );
IMPORT ER     dt_sysup_begin( CONST TS_UUID *pkg, BOOL force, ID pid, INT *p_total );
IMPORT ER     dt_sysup_step( ID pid, INT *p_done, INT *p_total );
IMPORT ER     dt_sysup_run( CONST TS_UUID *pkg, BOOL force );
IMPORT ER     dt_sysup_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid );
IMPORT void   dt_sysup_resume( DTWIN *d );
IMPORT ER     dt_sysup_need( CONST TS_UUID *pkg, UD *p_need, UD *p_free );
IMPORT void   dt_sysup_test( CONST char *root, INT cut );

/*
 * The system's objects and boot files put on the system volume of
 * another disk as an update puts a package's (dt_sysup_onto, design
 * 11.8.1; システム登録's 上書き): the source is the running system volume
 * and the list of the system's objects it keeps, the target the TSFS
 * volume of the partition tsdev and the FAT volume of its ESP espdev.
 * bf names the boot files as a package does, each taken from a file of
 * the tree. With run FALSE it is only looked at; run, the steps are done
 * at once in the caller's task, each told to cb. res->why says whether
 * it can be done (DT_SU_R_*: E_OK either way when looked at, E_OBJ when
 * run and it cannot), with the versions and the room.
 */
#define DT_SU_R_OK	0
#define DT_SU_R_NOLIST	1	/* the running system volume keeps no list of the system's objects */
#define DT_SU_R_NOVOL	2	/* the target's volume cannot be opened */
#define DT_SU_R_NOSYS	3	/* it is not a system volume: no system box */
#define DT_SU_R_NEWER	4	/* the system there is newer than this one */
#define DT_SU_R_FORM	5	/* its list is another machine's or processor's, or broken */
#define DT_SU_R_VOLFULL	6	/* the volume has not the room */
#define DT_SU_R_ESPFULL	7	/* its ESP has not the room for the boot files */
#define DT_SU_R_NOESP	8	/* its ESP cannot be opened */
#define DT_SU_R_BUSY	9	/* a system update is under way */

typedef struct {
	CONST char	*name;		/* below the boot partition's root, as a package names it */
	CONST char	*src;		/* the file it is taken from, a path of the tree */
	BOOL		keep;		/* put only where there is none ("bootkeep") */
} T_SUBOOTF;

typedef struct {
	INT	why;			/* DT_SU_R_* */
	UB	ver[DT_VER_MAX];	/* the running system's, as its list says */
	UB	tver[DT_VER_MAX];	/* the target's, "" when its volume keeps no list */
	UD	vol_need, vol_free;	/* bytes of the target volume */
	UD	esp_need, esp_free;	/* and of its ESP */
	INT	nobj;			/* objects of the list */
	INT	total;			/* the steps */
} T_SUONTO;

typedef void (*DT_SUPROG)( void *ctx, INT done, INT total );

IMPORT ER     dt_sysup_onto( CONST char *tsdev, CONST char *espdev, CONST T_SUBOOTF *bf, INT nbf,
			     BOOL run, DT_SUPROG cb, void *ctx, T_SUONTO *res );

/* A TADjs plugin directory taken in as a program and its template (design 18.12) */
IMPORT ER     dt_plugin_install( CONST TS_UUID *dir, BOOL force, ID pid, CONST TS_UUID *win,
				 TS_UUID *p_prog, UINT *p_how );

/* dtres.c: the faces and the pictures of the ground as objects (design 18.18) */
IMPORT INT    dt_res_faces( void );
IMPORT ER     dt_res_wall( UINT mode );
IMPORT ER     dt_font_req( CONST T_DTREQ *rq, T_DTANS *an );	/* DT_RQ_FONT_* (dtreq.h) */
/*
 * Whether the object v points at is open: in a window of the desktop's,
 * in a program's window that says it shows it (wm_obj_showing), or in
 * one of a program still being started on it (dt_prog_opening). The
 * window in *p_wid, 0 for one not up yet. One object is open in one
 * window: opening it again brings that window forward (dt_obj_front),
 * and 実行 of its links' menus is grey.
 */
IMPORT BOOL   dt_obj_open( CONST T_VOBJ *v, INT *p_wid );
IMPORT void   dt_obj_front( CONST T_VOBJ *v, INT wid );
IMPORT void   dt_close( DTWIN *d );
IMPORT void   dt_fullscreen( DTWIN *d );
IMPORT ER     dt_obj_fullscreen( INT wid, BOOL change, BOOL *p_full );	/* a program's window: 全画面表示 or back */
IMPORT ER     dt_screen_size( UINT *p_w, UINT *p_h );	/* through the screen object 画面 */
IMPORT ER     dt_screen_req( CONST T_DTREQ *rq );	/* DT_RQ_SCREEN: set now, asked, kept or put back */
IMPORT INT    dt_run_menu( DTWIN *d, INT pid, UD when );

/* dtsys.c: the protection of what the system owns */
IMPORT void   dt_sys_prt( T_OBPRT *prt );		/* owned by the system, read by all */

/*
 * dtvmn.c: the items every window's menu has (design 18.15). dt_get_vmn
 * sets them for the virtual object the menu was opened on; dt_exe_vmn
 * does one of them and answers TRUE, FALSE when the item is not one.
 */
IMPORT void   dt_get_vmn( DTWIN *d, ID mid, BOOL on_vobj, CONST T_VOBJ *v );
IMPORT void   dt_tail_fill( ID mid );		/* 小物 and 実行 of SYSDEF_MENU_TAIL */
IMPORT BOOL   dt_tail_do( CONST T_MNSEL *sel );
IMPORT void   dt_tail_poll( void );
IMPORT void   dt_tail_start( void );
IMPORT void   dt_tail_stop( void );
IMPORT BOOL   dt_exe_vmn( DTWIN *d, CONST T_MNSEL *sel, BOOL on_vobj,
			  CONST T_VOBJ *v );
IMPORT BOOL   dt_app_locked( CONST T_VOBJ *v );
IMPORT BOOL   dt_app_lockable( CONST T_VOBJ *v );
IMPORT ER     dt_req_applist( CONST T_DTREQ *rq, T_VOBJ *v );	/* the applist a program carried of its link */

/*
 * The 仮身操作 and 実身操作 a process asks for a link it shows itself
 * (DT_RQ_CLOSE .. DT_RQ_FULL of ts/dtreq.h), its panels over the
 * process's window (dt_host_of, desktop.c); what the link is to become
 * is put in the answer.
 */
IMPORT ER     dt_proc_link_op( CONST T_DTREQ *rq, T_DTANS *an );
/* The link a window was opened from: its view written back, a copy put with it (DT_RQ_LINK_VIEW, DT_RQ_SAVE_NEW) */
IMPORT ER     dt_link_view( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target,
			    INT sx, INT sy, CONST char *vm, INT wrap, INT zoom );
IMPORT ER     dt_save_new_of( DTWIN *ask, CONST TS_UUID *id, CONST TS_UUID *parent,
			      CONST TS_UUID *parent_vobj, CONST TS_UUID *target, TS_UUID *p_new );
IMPORT DTWIN *dt_host_of( CONST TS_UUID *win );
/*
 * A link of a program's window that a window was opened from (DT_RQ_OPEN
 * with DT_F_LINK): dt_plink_opening says, while DT_RQ_OPEN opens v, the
 * object whose record holds it; dt_plink_has whether link vobjid of
 * parent is such a one, pointing at target when given; dt_plink_add
 * tells the program a link to copy goes with it (新たな実身に保存).
 */
IMPORT BOOL   dt_plink_opening( CONST T_VOBJ *v, TS_UUID *p_parent );
IMPORT BOOL   dt_plink_has( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *target );
IMPORT ER     dt_plink_add( CONST TS_UUID *parent, CONST TS_UUID *vobjid, CONST TS_UUID *copy );

/*
 * dtinfo.c: 管理情報 of the real object a link points at, a process of
 * its own for each object (application/realinfo): started on the link,
 * or its window brought to the front; what it asks of the desktop
 * (DT_RQ_INFO_APPS, DT_RQ_INFO_KEEP, DT_RQ_APPS with DT_F_INFO) is done
 * for the link it was started on. dt_info_started is the starter's word
 * of the process made for place slot (dtprog.c, dt_prog_start_info).
 */
IMPORT void   dt_real_info( DTWIN *d, CONST T_VOBJ *v );
IMPORT void   dt_info_started( INT slot, ER er, CONST TS_UUID *proc );
IMPORT ER     dt_info_req( CONST T_DTREQ *rq, T_DTANS *an, ID from );
IMPORT ER     dt_prog_start_info( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v, INT slot );

#ifdef USE_TESTAGENT
/* dttagent.c: the same lines taken over the network (make TESTAGENT=1) */
IMPORT void   dt_tagent_start( void );
IMPORT ER     dt_tagent_power( INT how );
IMPORT void   dt_uitest_line( CONST char *s, void (*sink)( CONST char *answer ) );
#endif

/* dtuitest.c: the desktop driven over the second serial port (make UITEST=1) */
IMPORT void   dt_uitest_start( void );
IMPORT void   dt_uitest_poll( void );
IMPORT void   dt_uitest_idle( void );

/*
 * dtfault.c: a process ended by a processor exception, told in a
 * dialog (プログラムの異常終了). The desktop's loop takes what the kernel
 * recorded (dt_fault_poll) and gives each event to dt_fault_event
 * first; dt_fault_wid and dt_fault_lines say what is shown.
 */
IMPORT void   dt_fault_poll( void );
IMPORT void   dt_fault_forget( void );
IMPORT BOOL   dt_fault_event( CONST T_WMEV *ev );
IMPORT void   dt_fault_close_all( void );
IMPORT INT    dt_fault_wid( void );
IMPORT INT    dt_fault_lines( UB *buf, INT max );
IMPORT DTWIN *dt_first_win( void );	/* the first window, for a panel over it; NULL while there is none */
IMPORT DTWIN *dt_win_at( INT i );		/* the table's place i, used or not */

/*
 * dtdisk.c: ディスク操作 (design 11.26). The disks attached as volumes
 * of objects, followed each turn; what the program ディスク操作 asks
 * (DT_RQ_DISK), started from the menu with the item chosen
 * (dt_disk_menu); and the top object of a volume, where a link to what
 * is saved on it goes. dt_fix_boot starts ディスク修復 at the start when
 * the system volume was repaired before it was attached.
 */
IMPORT void   dt_disk_poll( void );
IMPORT ER     dt_disk_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid );
IMPORT void   dt_disk_menu( DTWIN *d, CONST char *code, CONST T_VOBJ *v );
IMPORT void   dt_fix_boot( void );
IMPORT BOOL   dt_disk_detachable( CONST T_VOBJ *v );
IMPORT BOOL   dt_disk_top( CONST char *path, TS_UUID *p_top );
IMPORT BOOL   dt_disk_absent( DTWIN *d, CONST T_VOBJ *v );
IMPORT BOOL   dt_disk_on_delete( DTWIN *d, CONST T_VOBJ *v );
IMPORT BOOL   dt_disk_growable( CONST T_VOBJ *v );
IMPORT BOOL   dt_reachable( CONST T_VOBJ *v );

/* The window a link was opened into, closed (〈閉じる〉) */
IMPORT void   dt_close_vobj( CONST T_VOBJ *v );

/* dtform.c: panels that ask and tell, and the helpers panels are built with */
#define DT_PN_IN	8		/* contents stand in this far from the outline */
#define DT_PN_CANCEL	90		/* the part numbers of the two buttons */
#define DT_PN_OK	91
IMPORT void   dt_pn_text( UB *to, INT max, CONST char *s );
IMPORT T_WMPANEL *dt_pn_new( INT w, INT h );
IMPORT T_WMPART *dt_pn_add( T_WMPANEL *p, UW type, INT num, INT l, INT t,
			    INT w, INT h, CONST char *label );
IMPORT void   dt_pn_buttons( T_WMPANEL *p, CONST char *ok );
IMPORT UINT   dt_pn_run( DTWIN *d, T_WMPANEL *def, INT *p_pid );
IMPORT void   dt_pn_done( DTWIN *d, INT pid );
IMPORT INT    dt_pn_value( INT pid, INT num );
IMPORT void   dt_colour_text( UW col, UB *out );
IMPORT BOOL   dt_colour_of( CONST UB *s, UW *p_col );

/*
 * A colour asked for: the pop-up of dtcolour.c, over 'owner', whose keys
 * become the pop-up's, at the pointer or under part 'num' of panel 'pid'.
 * TRUE with the colour in *io when it was kept.
 */
IMPORT BOOL   dt_pick_colour( INT owner, UW *io );
IMPORT BOOL   dt_pick_colour_part( INT owner, INT pid, INT num, UW *io );
IMPORT BOOL   dt_pick_colour_at( INT owner, UW *io, INT x, INT y );
/* 背景色変更's: with the switch 仮身属性と同じ, read and given back in *p_same */
IMPORT BOOL   dt_pick_paper( INT owner, UW *io, BOOL *p_same );

/*
 * A part of a panel showing a colour; pressed in dt_pn_run, the colour
 * is chosen with dt_pick_colour and the part shows what was chosen.
 * wm_panel_colour reads it when the panel has ended.
 */
#define DT_PN_COLOUR_ANS	( WM_ANS_ACT + 0x1000 )
IMPORT T_WMPART *dt_pn_colour( T_WMPANEL *p, INT num, INT l, INT t, INT w, INT h, UW col );
IMPORT BOOL   dt_ask_name( DTWIN *d, CONST char *prompt, CONST UB *start,
			   UB *out, INT max );
IMPORT BOOL   dt_confirm( DTWIN *d, CONST char *l1, CONST char *l2,
			  CONST char *no, CONST char *yes );
IMPORT void   dt_tell( DTWIN *d, CONST char *l1, CONST char *l2 );
IMPORT void   dt_no_memory( DTWIN *d );
IMPORT BOOL   dt_ask_text( DTWIN *d, CONST char *p1, CONST char *p2,
			   CONST UB *start, UB *out, INT max, CONST char *ok );
/*
 * A text put where a part of a panel or a menu keeps fixed room
 * (WM_LABEL_MAX and the like): at most max - 1 bytes of s, cut at a
 * whole letter and 0 ended; s NULL puts "". Answers the bytes put.
 */
IMPORT INT    dt_fit( UB *dst, INT max, CONST UB *s );
/*
 * The texts given, up to a NULL, one after another in memory of its own
 * (Kfree it): a message made with a name as long as it is. NULL without
 * memory.
 */
IMPORT UB    *dt_cat( CONST char *s, ... );
/*
 * The label of the volume attached with path, whole, in memory of its
 * own (Kfree it): NULL when it has none, or for want of memory (said on
 * the console).
 */
IMPORT UB    *dt_vol_label( CONST char *path );

/* One of a list of names chosen: its number, or -1 */
IMPORT INT    dt_list_form( DTWIN *d, CONST char *title,
			    CONST char *CONST *names, INT n, INT start );

/*
 * dtprint.c: 印刷 (design 11.22). The pages of a document or a figure
 * as the layer that writes the file asks for them; the jobs 基本印刷 (a
 * process) works, and the file put on the desktop; the pages a process
 * hands over.
 */

typedef struct {
	UINT		kind;		/* TV_KIND_DOC, TV_KIND_FIG */
	T_TVDOC		*doc;		/* what is drawn: the window's, or a copy without links */
	T_TVFIG		*fig;
	CONST T_TAD	*src;
	UW		paper;
	INT		cols, rows;	/* a figure's sheets across and down */
	INT		pw, ph;		/* a figure's sheet, in its own pixels */
	T_TVDOC		*own_doc;	/* the copies made, freed after */
	T_TVFIG		*own_fig;
	T_TVRUN		*own_run;
	T_TVPARA	*own_para;
	T_TVSHAPE	*own_sh;
} DTPRSRC;

IMPORT void   dt_print_src_doc( DTPRSRC *s, T_PRNJOB *job, T_TVDOC *d, CONST T_TAD *src,
				UW paper, CONST UB *name, BOOL links );
IMPORT void   dt_print_src_fig( DTPRSRC *s, T_PRNJOB *job, T_TVFIG *f, CONST T_TAD *src,
				UW paper, CONST UB *name, BOOL links );
IMPORT void   dt_print_src_free( DTPRSRC *s );
IMPORT ER     dt_print_out( DTWIN *host, CONST T_PRNJOB *job, CONST T_PRNSET *set );

/*
 * A job of the desktop's own worked by 基本印刷: job and what it draws
 * stay the caller's until done is called, which may be before this
 * returns. DT_PRINT_SHOW: 画面表示 is offered; DT_PRINT_SET: only the
 * setting is asked for, and done is given it with E_OK.
 */
#define DT_PRINT_SHOW	0x0001
#define DT_PRINT_SET	0x0002
IMPORT ER     dt_print_start( T_PRNJOB *job, CONST TS_UUID *win, UINT how,
			      void (*done)( void *arg, ER er, CONST T_PRNSET *set ), void *arg );
IMPORT ER     dt_print_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid );	/* DT_RQ_PRJOB */
IMPORT void   dt_print_poll( void );
IMPORT void   dt_raise_pid( ID pid );		/* a process's windows in front */
/* An answer a request handler writes itself, later: none is written when it returns */
#define DT_ANS_LATER	1
IMPORT void   dt_place_on_desktop( CONST TS_UUID *u, CONST UB *name );
IMPORT ER     dt_place_in( CONST TS_UUID *box, CONST TS_UUID *u, CONST UB *name );
/* The same with an applist of the link's own (固有データ with it, design 11.3) */
IMPORT ER     dt_place_on_desktop_appl( CONST TS_UUID *u, CONST UB *name, CONST UB *applist );
IMPORT ER     dt_place_in_appl( CONST TS_UUID *box, CONST TS_UUID *u, CONST UB *name, CONST UB *applist );
IMPORT void   dt_print_vobj( DTWIN *from, CONST T_VOBJ *v );	/* 基本印刷 */
IMPORT ER     dt_print_spool( CONST T_DTREQ *rq, SZ anssz );	/* DT_ANS_LATER */
IMPORT void  *dt_spool_open( CONST TS_UUID *obj, T_PRNJOB *job, CONST UB *name );
IMPORT void   dt_spool_close( void *sp );


/*
 * dttool.c: what the desktop does for the system's tools, programs
 * running as processes (application/systool): started, run again while
 * open, told of changes, and what they ask (DT_RQ_TELL ..
 * DT_RQ_WIN_LIST, DT_RQ_ROOTS); every program run again while open
 */
IMPORT void   dt_trash_open( void );
IMPORT void   dt_winlist_open( void );
IMPORT void   dt_winlist_poll( void );
IMPORT void   dt_search_open( DTWIN *from );
IMPORT void   dt_network_open( DTWIN *from, CONST T_VOBJ *v );
IMPORT BOOL   dt_prog_again( CONST DTPROG *p, CONST T_VOBJ *v, ER *p_er );
/*
 * 固有データ (design 11.3): the data the applist gives program p started
 * from v (the link's applist first, then the object's; the entry of
 * p's id, else of an id it had before), as long as it is, ending in 0,
 * in memory of its own (Kfree it), *p_len its length; NULL none.
 * dt_prog_data_next(TRUE, ...) gives the next start, or the window told
 * it is run again, data of its own (a link to the program object itself,
 * which the program is not started on), a copy of it (E_NOMEM none
 * kept); FALSE takes it back. dt_prog_data_take hands what it gave to
 * the one who takes it (*p_data NULL: none, Kfree it otherwise): FALSE
 * when nothing was given.
 */
IMPORT UB    *dt_prog_data( CONST DTPROG *p, CONST TS_UUID *target, CONST UB *link_applist, SZ *p_len );
IMPORT ER     dt_prog_data_next( BOOL on, CONST UB *data, SZ len );
IMPORT BOOL   dt_prog_data_take( UB **p_data, SZ *p_len );
/*
 * The channels a program named (DT_RQ_TELL) told how, into the object it
 * is about, with text, and data (DT_TAIL_DATA; NULL none): FALSE none
 * heard. One that asked in the form of now is told with a tail.
 */
IMPORT BOOL   dt_prog_tell( CONST char *prog, UINT how, CONST TS_UUID *into );
IMPORT BOOL   dt_prog_tell_text( CONST char *prog, UINT how, CONST TS_UUID *into, CONST UB *text );
/* The same with a link's name and applist as the tail's DT_TAIL_NAME and DT_TAIL_APPLIST (DT_HOW_LINK_PUT) */
IMPORT BOOL   dt_prog_tell_link( CONST char *prog, UINT how, CONST TS_UUID *into,
			       CONST UB *name, CONST UB *applist );
IMPORT BOOL   dt_prog_tell_data( CONST char *prog, UINT how, CONST TS_UUID *into,
				 CONST UB *text, CONST UB *data, SZ dlen );
IMPORT void   dt_trash_tell( void );
IMPORT ER     dt_tool_req( CONST T_DTREQ *rq, T_DTANS *an, ID asker );

/*
 * dtscr.c: 画面印刷. The screen, the window in front or a rectangle
 * dragged out on it taken as pixels (DT_SCR_*), with the pointer or
 * without it, and made a figure object, put in the tray or printed.
 */
#define DT_SCR_SCREEN	0		/* 全画面 */
#define DT_SCR_FRONT	1		/* 最前面ウィンドウのみ */
#define DT_SCR_RANGE	2		/* 範囲指定 */
IMPORT ER     dt_scr_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid );	/* DT_RQ_SCRPRINT */
IMPORT void   dt_scr_poll( void );
IMPORT BOOL   dt_scr_event( CONST T_WMEV *ev );


/* dtnotify.c: 通知, notices of processes in small windows of the desktop's (DT_RQ_NOTIFY) */
IMPORT ER     dt_note_req( CONST T_DTREQ *rq, T_DTANS *an );
IMPORT INT    dt_note_wid( INT i );

/*
 * application/sysinst/sysinst.c: what システム登録 (a process, x64 only)
 * asks of the desktop, the system put on an internal disk (DT_RQ_SYSINST)
 */
IMPORT ER     dt_sysinst_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid );

IMPORT ER     dt_scr_grab( UINT area, BOOL pointer, CONST T_DPRECT *range, UW **p_px,
			   T_DPRECT *p_at );
IMPORT ER     dt_scr_save( CONST UW *px, INT w, INT h, CONST UB *name, CONST char *vol,
			   TS_UUID *p_uuid );
IMPORT ER     dt_scr_tray( CONST UW *px, INT w, INT h );
typedef struct {
	CONST UW	*px;		/* the picture, w by h */
	INT		w, h;
} DTSCRPIC;
IMPORT void   dt_scr_job( T_PRNJOB *job, CONST DTSCRPIC *pic, CONST UB *name );

/*
 * What a rectangle being dragged out shows beside the pointer at (x, y):
 * the DT_SCR_LOUPE_SRC square of pixels round it, without the pointer,
 * each DT_SCR_LOUPE_ZOOM times as large in a frame, the pixel under the
 * pointer outlined, into out (DT_SCR_LOUPE_SIDE squared), and where it
 * goes on the screen, below and right of the pointer or on the side
 * that has room.
 */
#define DT_SCR_LOUPE_SRC	21
#define DT_SCR_LOUPE_ZOOM	6
#define DT_SCR_LOUPE_SIDE	( DT_SCR_LOUPE_SRC * DT_SCR_LOUPE_ZOOM + 4 )
#define DT_SCR_LOUPE_GAP	24
IMPORT ER     dt_scr_loupe( INT x, INT y, UW *out, T_DPRECT *p_at );

/* dttype.c: letters a program types into the window the keys went to before (DT_RQ_TYPE) */
IMPORT void   dt_type_poll( void );
IMPORT ER     dt_type_req( CONST T_DTREQ *rq, T_DTANS *an );

/* The administrator's rights asked of the person for a process (dtadmin.c, DT_RQ_ADMIN) */
IMPORT ER     dt_admin_ask( ID pid, CONST UB *what, CONST TS_UUID *win );
IMPORT ER     dt_admin_self( CONST char *name, CONST UB *what );
IMPORT ER     dt_admin_req( CONST T_DTREQ *rq, ID pid );
IMPORT void   dt_win_icon( INT wid, CONST char *prog );

/* A closed link to a new object sized to the object's name; an opened one kept as it is (dtvmn.c) */
IMPORT void   dt_link_fit( T_VOBJ *c );

/* How far apart the presses of a double press may be, each way (ユーザ環境設定 位置許容度) */
#define DT_DBL_NEAR	( wm_num(LK_DBL_W, 10) / 2 )

IMPORT BOOL dt_confirm_timed( DTWIN *d, CONST char *CONST *lines, INT nline,
			      CONST char *no, CONST char *yes, INT ms );

#endif /* __DESKTOP_H__ */
