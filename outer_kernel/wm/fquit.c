/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fquit.c
 *	強制終了: Shift+Pause, the window that lists the processes, and the
 *	desktop started again (design 5.5.1, 5.5.3 - 5.5.5)
 *
 *	The keys are taken in the input layer (knl_fq_input, from hid.c), as
 *	the keyboard reports them and before the ring the desktop reads, so
 *	that they work whatever the desktop is doing: waiting for a program,
 *	copying a large object, or stopped for good. Two tasks of their own
 *	do the rest.
 *
 *	fq_task, above the input's task, answers the keys. It decides what
 *	a press is for, looking at the windows with locks it waits for only
 *	a little (a task that holds one and never lets go must not stop the
 *	last resort), and stops the desktop's tasks when the desktop is to
 *	be started again. It never waits for anything else.
 *
 *	fq_work, above the desktop and below the compositor, does what takes
 *	time or may wait: ends the process (knl_prc_force_end, knl_prc_kill),
 *	writes the message line, opens the window 強制終了 and answers what
 *	is done in it, and puts in order what the desktop's stopped tasks
 *	left before the desktop is started again. The window is a window of
 *	panels from a data box (SYSDEF_BOX_FQUIT): the window manager draws
 *	and works its parts. A desktop stopped while holding a lock the
 *	window layer or the letters need holds this task up too; fq_task
 *	still answers, and starting the desktop again lets the lock go.
 *
 *	What is said goes to the console at once, from fq_task, and to the
 *	message line from fq_work.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/hid.h>
#include <ts/look.h>
#include <ts/proc.h>
#include <ts/lockup.h>
#include <ts/om.h>
#include <ts/time.h>
#include <ts/ob.h>
#include <ts/mn.h>
#include <ts/part.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include <ts/fquit.h>
#include "wmobj.h"

/* ---------------------------------------------------------------- numbers */

#define FQ_KEY_PAUSE	0x48
#define FQ_KEY_ENTER	0x28
#define FQ_KEY_ESC	0x29
#define FQ_KEY_KPENTER	0x58
#define FQ_KEY_HOME	0x4A
#define FQ_KEY_PGUP	0x4B
#define FQ_KEY_END	0x4D
#define FQ_KEY_PGDN	0x4E
#define FQ_KEY_DOWN	0x51
#define FQ_KEY_UP	0x52
#define FQ_MOD_SHIFT	( HID_MOD_LSHIFT | HID_MOD_RSHIFT )
#define FQ_MOD_CTRL	( HID_MOD_LCTRL | HID_MOD_RCTRL )

#define FQ_PRI		3	/* above the input (8): the keys are answered whatever else runs */
#define FQ_WORK_PRI	11	/* below the compositor (10) it waits for, above the desktop (20) */
#define FQ_STACK	( 16 * 1024 )
#define FQ_WORK_STACK	( 48 * 1024 )	/* the letters are made on it */

#define FQ_HOLD_MS	2000	/* a press this soon after the last, with no let go between: the key held */
#define FQ_CONFIRM_MS	15000	/* the press that confirms starting the desktop again, at most this later */
#define FQ_LOCK_MS	300	/* a lock of the windows waited for, no longer */
#define FQ_KILL_AFTER_MS 1000	/* a process asked to end this long ago is ended by force when asked again */
#define FQ_SETTLE_MS	1000	/* a process asked to end is waited for this long before it is said to be ending */
#define FQ_REFRESH_MS	1000	/* the list of processes looked at again */
#define FQ_BUSY_MS	1000	/* fq_work held up this long: the console says what the window would */

#define FQ_IN_MAX	64	/* events kept for the window */
#define FQ_MSG_MAX	256
#define FQ_NAME_MAX	64
#define FQ_DESK_TSK	32	/* the desktop's tasks stopped at a time */
#define FQ_UNSAVED_MAX	160

/* The window: its panels in the data box SYSDEF_BOX_FQUIT, and their parts */
#define PN_MAIN		1	/* the window's own */
#define PN_ASK		10	/* asking before the desktop is started again */
#define P_LIST		10	/* the processes */
#define P_STATE		21	/* what is shown of the one chosen */
#define P_NWIN		22
#define P_CPU		23
#define P_MEM		24
#define P_MSG		25	/* what came of something */
#define P_RESTART	31	/* 〈デスクトップを立ち上げ直す〉 */
#define P_END		32	/* 〈強制終了〉 */
#define P_CANCEL	33	/* 〈取消〉, the one Enter presses */
#define Q_LINE1		1
#define Q_LINE2		2
#define Q_LINE3		3
#define Q_CANCEL	102	/* 〈取消〉, the one Enter presses */
#define Q_RESTART	103	/* 〈立ち上げ直す〉 */
#define FW_PARTS	48
#define FW_FRONT_MS	6000	/* the window kept in front after the desktop was started again */

/* The desktop's code, data and zeroed data, from the linker scripts */
IMPORT UB	__dt_text_start[], __dt_text_end[];
IMPORT UB	__dt_data_start[], __dt_data_end[];
IMPORT UB	__dt_bss_start[], __dt_bss_end[];

/* ---------------------------------------------------------------- state */

typedef struct {
	ID	pid;
	UB	name[FQ_NAME_MAX];
	BOOL	sys;
	BOOL	ending;
	BOOL	killed;
	INT	incall;
	INT	nwin;
	UD	cpu_ms;
	UD	memsz;
} FQROW;

LOCAL BOOL	fq_ready = FALSE;
LOCAL ID	fq_tsk = 0, fq_wtsk = 0;
LOCAL ID	fq_sem = 0;		/* fq_task: a key came */
LOCAL ID	fq_wsem = 0;		/* fq_work: a job or an event came */
LOCAL T_SPLOCK	fq_lock;

/* The keys, as knl_fq_input sees them (under the input layer's lock) */
LOCAL UW	fq_key_mine[256 / 32];	/* keys whose press was taken here: their let go too */
LOCAL UW	fq_btn_mine;		/* the same for the buttons */
LOCAL UD	fq_pause_at = 0;	/* ms the last press of Pause with Shift came */
LOCAL volatile BOOL fq_grab = FALSE;	/* the window is up and the desktop reads no input: every event is the window's */

/* From the keys to fq_task */
LOCAL INT	fq_n_quit = 0, fq_n_list = 0;

/* Events for the window */
LOCAL T_HIDEV	fq_in[FQ_IN_MAX];
LOCAL INT	fq_in_h = 0, fq_in_t = 0;

/* Jobs for fq_work */
LOCAL struct {
	BOOL	say;
	char	text[FQ_MSG_MAX];
	ID	end;			/* a process to end */
	BOOL	end_sel;		/* the row chosen in the window */
	BOOL	open;			/* the window opened, or closed when up */
	BOOL	restart;		/* the desktop's tasks stopped: the rest done, and it started */
	ID	tids[FQ_DESK_TSK];
	INT	ntid;
	ID	main;
} fq_job;

LOCAL volatile UD fq_busy_at = 0;	/* ms fq_work took up what it is doing, 0 idle */

/* The desktop */
LOCAL CONST T_FQDESK *fq_desk = NULL;
LOCAL UB	*fq_dt_image = NULL;	/* its variables as the machine started */
LOCAL SZ	fq_dt_isz = 0;
LOCAL UD	fq_armed_at = 0;	/* ms Shift+Pause found it stuck: the next press starts it again */
LOCAL INT	fq_restarted = 0;
LOCAL ID	fq_old_main = 0;	/* the stopped task of the desktop before, kept dormant */

/* A window of a panel from the data box, and its parts by number */
typedef struct {
	INT	num;
	ID	key;
	UINT	kind;
} FWPART;

typedef struct {
	TS_UUID	win, pnl;
	ID	kw, pk;
	INT	wid;			/* the window's number with the window manager */
	INT	w, h;			/* the panel's size */
	FWPART	p[FW_PARTS];
	INT	np;
} FWIN;

/* The window, worked by fq_work alone */
LOCAL FWIN	fw_main, fw_ask;
LOCAL TS_UUID	fw_ch;
LOCAL ID	fw_port = 0;		/* the channel the notices come on */
LOCAL volatile BOOL fw_up = FALSE;
LOCAL BOOL	fw_asking = FALSE;
LOCAL INT	fw_was_focus = 0;
LOCAL UB	*fw_names = NULL;	/* what the list was last given */
LOCAL SZ	fw_names_len = 0;
LOCAL FQROW	*fq_row = NULL;		/* a row for each process, as many as there are */
LOCAL INT	fq_nrow = 0, fq_cap = 0;
LOCAL UD	fq_refresh_at = 0;
LOCAL UD	fw_front_until = 0;	/* ms: the window kept in front of what a desktop started again opens */

LOCAL void fw_status( CONST char *msg );

/* What was said, for the tests */
LOCAL UW	fq_said = 0;
LOCAL char	fq_said_text[FQ_MSG_MAX];

/* ---------------------------------------------------------------- helpers */

LOCAL UD fq_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000ULL;
}

/* s copied into d of max bytes, never half a character */
LOCAL void fq_copy( char *d, CONST char *s, INT max )
{
	INT	n = 0;

	while ( s[n] != 0 && n < max - 1 ) {
		d[n] = s[n];
		n++;
	}
	while ( n > 0 && s[n] != 0 && ( (UB)s[n] & 0xC0 ) == 0x80 ) {
		n--;
	}
	d[n] = 0;
}

LOCAL void fq_wake_work( void )
{
	if ( fq_wsem > 0 ) {
		(void)tk_sig_sem(fq_wsem, 1);
	}
}

/*
 * Something said: to the console now, and to the message line by
 * fq_work. Called from either task.
 */
LOCAL void fq_say( CONST char *text )
{
	UINT	imask;

	tm_printf((UB *)"TessronOS: 強制終了: %s\n", text);
	ISpinLock(&fq_lock, &imask);
	fq_copy(fq_job.text, text, FQ_MSG_MAX);
	fq_job.say = TRUE;
	fq_copy(fq_said_text, text, FQ_MSG_MAX);
	fq_said++;
	ISpinUnlock(&fq_lock, &imask);
	fq_wake_work();
}

/* ---------------------------------------------------------------- the keys */

LOCAL BOOL key_is( UW *map, UINT code )
{
	return (BOOL)( ( map[code / 32] & ( 1U << ( code % 32 ) ) ) != 0 );
}

LOCAL void key_set( UW *map, UINT code, BOOL on )
{
	if ( on ) {
		map[code / 32] |= 1U << ( code % 32 );
	} else {
		map[code / 32] &= ~( 1U << ( code % 32 ) );
	}
}

LOCAL void fq_in_put( CONST T_HIDEV *ev )
{
	UINT	imask;
	INT	next;

	ISpinLock(&fq_lock, &imask);
	next = ( fq_in_t + 1 ) % FQ_IN_MAX;
	if ( next == fq_in_h ) {
		fq_in_h = ( fq_in_h + 1 ) % FQ_IN_MAX;	/* the oldest goes */
	}
	fq_in[fq_in_t] = *ev;
	fq_in_t = next;
	ISpinUnlock(&fq_lock, &imask);
	fq_wake_work();
}

LOCAL BOOL fq_in_take( T_HIDEV *ev )
{
	UINT	imask;
	BOOL	got = FALSE;

	ISpinLock(&fq_lock, &imask);
	if ( fq_in_h != fq_in_t ) {
		*ev = fq_in[fq_in_h];
		fq_in_h = ( fq_in_h + 1 ) % FQ_IN_MAX;
		got = TRUE;
	}
	ISpinUnlock(&fq_lock, &imask);
	return got;
}

/*
 * Every event before the ring (hid.c, under the ring's lock): TRUE when
 * it is taken here.
 *
 * Shift+Pause, with Ctrl or without, is never let through. The keyboard
 * reports a key's press once; what repeats while it is held is made
 * later, by the reader of the ring, which never sees this one. A press
 * that comes with no let go since the last is still taken as the key
 * held when it is within FQ_HOLD_MS of the last, and as a new press
 * after that: a let go lost on the way (a keyboard pulled out, events
 * put in by hand) does not leave the keys dead.
 *
 * While the window is up and the desktop does not read the input (it
 * is stuck), every event is the window's (fw_feed), but a key or a
 * button pressed before goes up where it went down, and one pressed
 * meanwhile goes up here: the desktop never sees a let go without its
 * press, nor a press without its let go.
 */
EXPORT BOOL knl_fq_input( CONST T_HIDEV *ev, BOOL inj )
{
	UINT	code = ev->code;
	BOOL	took = FALSE;

	if ( !fq_ready ) {
		return FALSE;
	}
	switch ( ev->type ) {
	case HID_EV_KEY_DOWN:
		if ( code == FQ_KEY_PAUSE && ( ev->mods & FQ_MOD_SHIFT ) != 0 ) {
			UD	now = fq_now();
			BOOL	held = (BOOL)( key_is(fq_key_mine, code) && now - fq_pause_at < FQ_HOLD_MS );
			UINT	imask;

			fq_pause_at = now;
			key_set(fq_key_mine, code, TRUE);
			if ( !held ) {
				ISpinLock(&fq_lock, &imask);
				if ( ( ev->mods & FQ_MOD_CTRL ) != 0 ) {
					fq_n_list++;
				} else {
					fq_n_quit++;
				}
				ISpinUnlock(&fq_lock, &imask);
				(void)tk_sig_sem(fq_sem, 1);
			}
			return TRUE;
		}
		if ( code < 256 ) {
			key_set(fq_key_mine, code, fq_grab);
		}
		took = fq_grab;
		break;

	case HID_EV_KEY_UP:
		if ( code < 256 && key_is(fq_key_mine, code) ) {
			key_set(fq_key_mine, code, FALSE);
			if ( code == FQ_KEY_PAUSE ) {
				return TRUE;
			}
			took = TRUE;
		}
		break;

	case HID_EV_BTN_DOWN:
		if ( code < 32 ) {
			if ( fq_grab ) fq_btn_mine |= 1U << code;
			else fq_btn_mine &= ~( 1U << code );
		}
		took = fq_grab;
		break;

	case HID_EV_BTN_UP:
		if ( code < 32 && ( fq_btn_mine & ( 1U << code ) ) != 0 ) {
			fq_btn_mine &= ~( 1U << code );
			took = TRUE;
		}
		break;

	default:
		took = fq_grab;
		break;
	}
	if ( took ) {
		fq_in_put(ev);
	}
	(void)inj;
	return took;
}

/*
 * Whether the window takes every event, the desktop reading none: the
 * input layer lets go of what it would make of them itself (two fingers'
 * wheel, hid.c), which would reach the windows behind when the desktop
 * reads again.
 */
EXPORT BOOL knl_fq_grabbing( void )
{
	return ( fq_ready && fq_grab );
}

/* ---------------------------------------------------------------- the desktop */

EXPORT void knl_fq_desktop( CONST T_FQDESK *d )
{
	fq_desk = d;
}

EXPORT INT knl_fq_restarts( void )
{
	return fq_restarted;
}

/* Whether a task's start is in the desktop's code */
LOCAL BOOL in_desktop( FP entry )
{
	UB	*a = (UB *)(UBINT)entry;

	return (BOOL)( a >= __dt_text_start && a < __dt_text_end );
}

/*
 * The desktop's tasks: its own, and the kernel's tasks that start in its
 * code -- the one that starts programs, the check of a disk, the players
 * of 実時間制御TAD -- that are not dormant. The main one first.
 */
LOCAL INT fq_desk_tasks( ID *tids, INT max, ID *p_main )
{
	ID	tid, main = ( fq_desk != NULL ) ? fq_desk->task() : 0;
	INT	n = 0;

	*p_main = main;
	if ( main > 0 ) {
		tids[n++] = main;
	}
	for ( tid = 1; tid <= CNF_MAX_TSKID && n < max; tid++ ) {
		TCB	*t;
		T_RTSK	r;

		if ( tid == main || tid == fq_tsk || tid == fq_wtsk
		  || tk_ref_tsk(tid, &r) < E_OK || r.tskstat == TTS_DMT ) {
			continue;
		}
		t = knl_tsk_tcb(tid);
		if ( t != NULL && t->owner == 0 && in_desktop(t->task) ) {
			tids[n++] = tid;
		}
	}
	return n;
}

/* A task made dormant wherever it stands; one running on another processor is taken off first */
LOCAL void fq_tsk_stop( ID tid )
{
	T_RTSK	r;
	INT	n;

	for ( n = 0; n < 200; n++ ) {
		if ( tk_ref_tsk(tid, &r) < E_OK || r.tskstat == TTS_DMT ) {
			return;
		}
		if ( tk_ter_tsk(tid) >= E_OK ) {
			return;
		}
		(void)tk_sus_tsk(tid);
		tk_dly_tsk(1);
	}
	tm_printf((UB *)"TessronOS: 強制終了: task %d of the desktop could not be stopped\n", tid);
}

/* The desktop's variables put back as they were when the machine started */
LOCAL ER fq_desk_reset( void )
{
	SZ	bsz = (SZ)( __dt_bss_end - __dt_bss_start );

	if ( fq_dt_isz > 0 && fq_dt_image == NULL ) {
		return E_NOMEM;
	}
	if ( fq_dt_isz > 0 ) {
		knl_memcpy(__dt_data_start, fq_dt_image, fq_dt_isz);
	}
	if ( bsz > 0 ) {
		knl_memset(__dt_bss_start, 0, bsz);
	}
	return E_OK;
}

/*
 * The desktop stopped (from either task): its tasks made dormant where
 * they stand -- the core lets go of the mutexes they hold -- and the rest
 * left to fq_work (fq_restart_rest). FALSE when there is no desktop.
 */
LOCAL BOOL fq_restart_stop( void )
{
	ID	tids[FQ_DESK_TSK], main;
	INT	n, i;
	UINT	imask;

	if ( fq_desk == NULL ) {
		fq_say("立ち上げ直すデスクトップがありません");
		return FALSE;
	}
	n = fq_desk_tasks(tids, FQ_DESK_TSK, &main);
	tm_printf((UB *)"TessronOS: 強制終了: the desktop is started again: %d task(s) of it stopped (main %d)\n",
		  n, main);
	knl_wdog_watch(0);
	for ( i = 0; i < n; i++ ) {
		fq_tsk_stop(tids[i]);
	}
	ISpinLock(&fq_lock, &imask);
	for ( i = 0; i < n; i++ ) {
		fq_job.tids[i] = tids[i];
	}
	fq_job.ntid = n;
	fq_job.main = main;
	fq_job.restart = TRUE;
	ISpinUnlock(&fq_lock, &imask);
	fq_wake_work();
	return TRUE;
}

/*
 * What the stopped tasks left, put in order (fq_work): their windows
 * with the panels and menus in them, their menus and their keys to
 * objects; the documents the desktop kept in memory let go (their
 * changes not saved are lost, as the person was told); the desktop's
 * variables put back; and the desktop started. The processes, their
 * windows among them, are not touched.
 */
LOCAL void fq_restart_rest( CONST ID *tids, INT n, ID main )
{
	char	msg[FQ_MSG_MAX];
	INT	nwin, nkey, i;
	ER	er;

	knl_wmobj_act_for(0);
	nwin = knl_wm_tsk_end(tids, n);
	(void)knl_mn_tsk_end(tids, n);
	nkey = knl_ob_tsk_end(tids, n);
	/*
	 * The desktop's own task is kept dormant until the next time: a
	 * request it had made of another task may still be answered into
	 * its stack. The others are deleted.
	 */
	for ( i = 0; i < n; i++ ) {
		if ( tids[i] != main ) {
			(void)tk_del_tsk(tids[i]);
		}
	}
	if ( fq_old_main > 0 && fq_old_main != main ) {
		(void)tk_del_tsk(fq_old_main);
	}
	fq_old_main = main;
	wm_obj_painter(NULL, NULL);
	om_store_empty();
	er = fq_desk_reset();
	tm_printf((UB *)"TessronOS: 強制終了: %d window(s) and %d key(s) of the desktop closed\n", nwin, nkey);
	if ( er >= E_OK && fq_desk != NULL ) {
		fq_restarted++;
		er = fq_desk->start();
	}
	if ( er >= E_OK ) {
		(void)tm_sprintf((UB *)msg, (CONST UB *)"デスクトップを立ち上げ直しました");
	} else {
		(void)tm_sprintf((UB *)msg, (CONST UB *)"デスクトップを立ち上げ直せませんでした(%d)", (INT)er);
	}
	fq_say(msg);
	fw_status(msg);
}

/* The documents with changes not saved, said for the question */
LOCAL INT fq_unsaved( char *buf, INT max )
{
	UB	names[FQ_UNSAVED_MAX];
	INT	n;

	names[0] = 0;
	n = ( fq_desk != NULL && fq_desk->unsaved != NULL ) ? fq_desk->unsaved(names, sizeof(names)) : 0;
	if ( n <= 0 ) {
		fq_copy(buf, "保存していない変更はありません", max);
	} else {
		char	t[FQ_UNSAVED_MAX + 64];

		(void)tm_sprintf((UB *)t, (CONST UB *)"保存していない%d個の文書(%s)の変更は失われます", n, names);
		fq_copy(buf, t, max);
	}
	return n;
}

/* ---------------------------------------------------------------- ending a process */

/*
 * The process for Shift+Pause: of the popups in front of the window a
 * person works in, the first that the desktop opened for a process
 * (knl_wmobj_act_for), else that window's maker, else the process that
 * window was opened for. When that window is the system's own and none
 * of these, the window nearest the viewer is looked at the same way: a
 * program's window shown in front, which the keys have not gone to
 * (a press on it does not give them; its program asks for them when it
 * wants them), is the one a person means. E_TMOUT when the windows
 * could not be looked at.
 */
LOCAL ER fq_pick( ID *p_pid )
{
	INT	pops[8], np, wid, front, i;
	ID	mk, fr;
	ER	er;

	*p_pid = 0;
	er = knl_wm_front_pick(FQ_LOCK_MS, &wid, pops, 8, &np);
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < np; i++ ) {
		er = knl_wmobj_who(pops[i], FQ_LOCK_MS, &mk, &fr);
		if ( er == E_TMOUT ) {
			return er;
		}
		if ( er >= E_OK && fr > 0 ) {
			*p_pid = fr;
			return E_OK;
		}
	}
	if ( wid > 0 ) {
		er = knl_wmobj_who(wid, FQ_LOCK_MS, &mk, &fr);
		if ( er == E_TMOUT ) {
			return er;
		}
		if ( er >= E_OK ) {
			*p_pid = ( mk > 0 ) ? mk : fr;
		}
	}
	if ( *p_pid <= 0 && knl_wm_front_any(FQ_LOCK_MS, &front) >= E_OK
	  && front > 0 && front != wid ) {
		er = knl_wmobj_who(front, FQ_LOCK_MS, &mk, &fr);
		if ( er == E_TMOUT ) {
			return er;
		}
		if ( er >= E_OK ) {
			*p_pid = ( mk > 0 ) ? mk : fr;
		}
	}
	return E_OK;
}

/* Whether the process has finished (its end told, or collected), looked at for FQ_SETTLE_MS */
LOCAL BOOL fq_settled( ID pid )
{
	T_PRCINFO	pi;
	INT		t;

	for ( t = 0; t <= FQ_SETTLE_MS / 50; t++ ) {
		if ( knl_prc_info(pid, &pi) < E_OK || pi.done ) {
			return TRUE;
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

/*
 * A process ended from the keyboard or the window (fq_work). Asked
 * again once it has been ending for a while, it is ended by force
 * (knl_prc_kill). What came of it is said.
 */
LOCAL void fq_end( ID pid )
{
	UB		name[FQ_NAME_MAX];
	char		msg[FQ_MSG_MAX];
	CONST char	*fmt;
	T_PRCINFO	pi;
	ER		er;

	if ( pid <= 0 || knl_prc_info(pid, &pi) < E_OK || pi.done ) {
		fq_say("強制終了できるプロセスがありません");
		return;
	}
	(void)knl_prc_name(pid, name, sizeof(name));
	if ( pi.sys ) {
		er = E_OACV;
		fmt = "%s(pid %d)はシステムのプロセスなので強制終了できません";
	} else if ( pi.killed ) {
		er = E_OBJ;
		fmt = "%s(pid %d)は強制的に終わらせました。後始末を待っています";
	} else if ( pi.ending && pi.ending_ms >= FQ_KILL_AFTER_MS ) {
		er = knl_prc_kill(pid);
		fmt = ( er >= E_OK ) ? "%s(pid %d)の待ちを切り、強制的に終わらせました"
		    :                  "%s(pid %d)を強制的に終わらせることができませんでした";
	} else if ( pi.ending ) {
		er = E_OBJ;
		fmt = "%s(pid %d)は終了の途中です。止まったままなら、もう一度強制終了すると強制的に終わらせます";
	} else {
		er = knl_prc_force_end(pid);
		/*
		 * Ended where it stood, it is done by now. One whose task is
		 * inside a call comes out at the call's next piece or slice:
		 * waited for a little, and said to be ending when it does not.
		 */
		if ( er >= E_OK && !fq_settled(pid) ) {
			er = E_OBJ;
		}
		fmt = ( er >= E_OK )   ? "%s(pid %d)を強制終了しました"
		    : ( er == E_OACV ) ? "%s(pid %d)はシステムのプロセスなので強制終了できません"
		    : ( er == E_OBJ )  ? "%s(pid %d)は終了の途中です。止まったままなら、もう一度強制終了すると強制的に終わらせます"
		    :                    "%s(pid %d)を強制終了できませんでした";
	}
	(void)tm_sprintf((UB *)msg, (CONST UB *)fmt, name, pid);
	fq_say(msg);
	fw_status(msg);
	(void)er;
}

LOCAL void fq_post_end( ID pid )
{
	UINT	imask;

	ISpinLock(&fq_lock, &imask);
	fq_job.end = pid;
	ISpinUnlock(&fq_lock, &imask);
	fq_wake_work();
}

/* ---------------------------------------------------------------- fq_task */

/*
 * The processes there are now, in a table of their own (Kfree it): a
 * table that came back full is asked for again in twice the room
 */
LOCAL INT fq_pids( ID **p_pids )
{
	ID	*pids;
	INT	room = 64, n;

	for ( ;; ) {
		pids = (ID *)Kmalloc(sizeof(ID) * (SZ)room);
		if ( pids == NULL ) {
			*p_pids = NULL;
			return 0;
		}
		n = knl_prc_list(pids, room);
		if ( n < room ) break;
		Kfree(pids);
		room *= 2;
	}
	*p_pids = pids;
	return n;
}

/* The list said on the console, read without locks: for when the window cannot be drawn */
LOCAL void fq_console_list( void )
{
	ID		*pids;
	UB		file[TS_FLT_FILE + 1];
	T_PRCINFO	pi;
	INT		n, i;

	n = fq_pids(&pids);
	tm_printf((UB *)"TessronOS: 強制終了: the window cannot be drawn now; the processes are:\n");
	for ( i = 0; i < n; i++ ) {
		if ( knl_prc_info(pids[i], &pi) < E_OK || pi.done ) {
			continue;
		}
		(void)knl_prc_file_peek(pids[i], file, sizeof(file));
		tm_printf((UB *)"  pid %d %s%s%s cpu %d ms\n", pids[i], file,
			  pi.sys ? " (system)" : "", pi.ending ? " (ending)" : "", (INT)pi.cpu_ms);
	}
	if ( pids != NULL ) Kfree(pids);
}

/*
 * Shift+Pause. With the window up, the row chosen there is ended. Else
 * the process in front is (fq_pick). With none there and the desktop
 * stuck (its loop away for LK_SILENT_S, knl_wdog_stuck), the press is
 * the first of two that start the desktop again: what is lost is said,
 * and a second within FQ_CONFIRM_MS does it.
 */
LOCAL void fq_quit( void )
{
	char	msg[FQ_MSG_MAX], lost[FQ_MSG_MAX];
	UW	ms = 0;
	ID	pid = 0;
	BOOL	stuck;
	UD	now = fq_now();
	ER	er;

	if ( fw_up ) {
		UINT	imask;

		ISpinLock(&fq_lock, &imask);
		fq_job.end_sel = TRUE;
		ISpinUnlock(&fq_lock, &imask);
		fq_wake_work();
		return;
	}
	er = fq_pick(&pid);
	if ( pid > 0 ) {
		fq_armed_at = 0;
		fq_post_end(pid);
		return;
	}
	stuck = knl_wdog_stuck(&ms, NULL);
	if ( !stuck || fq_desk == NULL ) {
		fq_armed_at = 0;
		fq_say(( er == E_TMOUT ) ? "ウインドウの状態がわからないため、強制終了できません"
					 : "強制終了できるプロセスがありません");
		return;
	}
	if ( fq_armed_at != 0 && now - fq_armed_at <= FQ_CONFIRM_MS ) {
		fq_armed_at = 0;
		(void)fq_restart_stop();
		return;
	}
	fq_armed_at = now;
	(void)fq_unsaved(lost, sizeof(lost));
	(void)tm_sprintf((UB *)msg, (CONST UB *)"デスクトップが%d秒応答していません。もう一度Shift+Pauseを押すと立ち上げ直します。%s",
			 (INT)( ms / 1000 ), lost);
	fq_say(msg);
}

/* Ctrl+Shift+Pause: the window opened, or closed when it is up */
LOCAL void fq_list( void )
{
	UINT	imask;
	UD	busy = fq_busy_at;

	ISpinLock(&fq_lock, &imask);
	fq_job.open = TRUE;
	ISpinUnlock(&fq_lock, &imask);
	fq_wake_work();
	if ( busy != 0 && fq_now() - busy >= FQ_BUSY_MS ) {
		fq_console_list();		/* fq_work is held up: the window will not come yet */
	}
}

LOCAL void fq_task( INT stacd, void *exinf )
{
	INT	quit, list;
	UINT	imask;

	for ( ;; ) {
		(void)tk_wai_sem(fq_sem, 1, TMO_FEVR);
		ISpinLock(&fq_lock, &imask);
		quit = fq_n_quit;
		list = fq_n_list;
		fq_n_quit = fq_n_list = 0;
		ISpinUnlock(&fq_lock, &imask);
		while ( quit-- > 0 ) {
			fq_quit();
		}
		if ( list > 0 ) {
			fq_list();
		}
	}
}

/* ---------------------------------------------------------------- the window's panels */

/*
 * The window 強制終了 is a window of panels made as objects: the panel is
 * made from its definition in the data box SYSDEF_BOX_FQUIT and placed
 * on the window, and the window manager draws and works every part --
 * the list, the boxes that show what is chosen, the push buttons with
 * the one that answers Enter (〈取消〉) framed heavier. What happens
 * comes on a channel as notices; here the parts are only read and
 * written, by the numbers the definition gives them.
 */

/* The panel of a data box made, standing on its own for now */
LOCAL ER fw_panel_make( FWIN *f, INT num )
{
	T_OBCRE	c;
	char	json[160];
	ID	key;

	(void)tm_sprintf((UB *)json, (CONST UB *)"{\"tessronos\":{\"part\":{\"sub\":\"panel\",\"box\":\"%s\",\"num\":%d}}}",
			 SYSDEF_BOX_FQUIT, num);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_PANEL;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)knl_strlen(json);
	if ( ob_cre_obj(&c, &f->pnl) < E_OK ) {
		return E_OBJ;
	}
	key = ob_opn_obj(&f->pnl, OB_OP_R | OB_OP_W | OB_OP_PROT);
	if ( key < E_OK ) {
		(void)ob_del_obj(&f->pnl);
		return (ER)key;
	}
	f->pk = key;
	return E_OK;
}

/* Where w first stands in s, NULL nowhere */
LOCAL CONST char *fw_find( CONST char *s, CONST char *w )
{
	INT	i, j;

	for ( i = 0; s[i] != 0; i++ ) {
		for ( j = 0; w[j] != 0 && s[i + j] == w[j]; j++ ) ;
		if ( w[j] == 0 ) {
			return s + i;
		}
	}
	return NULL;
}

/* A number after "key": in the attributes of a part */
LOCAL INT fw_json_num( CONST char *j, CONST char *key )
{
	char		k[24];
	CONST char	*p;
	INT		v = 0;

	(void)tm_sprintf((UB *)k, (CONST UB *)"\"%s\":", key);
	p = fw_find(j, k);
	if ( p == NULL ) {
		return 0;
	}
	p += knl_strlen(k);
	while ( *p == ' ' ) p++;
	while ( *p >= '0' && *p <= '9' ) v = v * 10 + ( *p++ - '0' );
	return v;
}

/* A placed panel's parts opened by the links it keeps, and known by number */
LOCAL void fw_parts_open( FWIN *f )
{
	TS_UUID	pu;
	SZ	asz = 0;
	char	*jw = NULL;
	INT	i;

	f->np = 0;
	for ( i = 0; i < FW_PARTS; i++ ) {
		FWPART	*p = &f->p[i];

		if ( ob_rea_rec(f->pk, OB_WR_KIND + i, 0, &pu, sizeof(pu), &asz) < E_OK
		  || asz < (SZ)sizeof(pu) ) {
			break;
		}
		p->num = 0;
		p->kind = 0;
		p->key = ob_opn_obj(&pu, OB_OP_R | OB_OP_W | OB_OP_PROT);
		if ( p->key > 0 && knl_ob_atr_all(p->key, 0, (UB **)&jw, NULL) >= 0 ) {
			p->num = fw_json_num(jw, "part");	/* read whole: the keys may come anywhere */
			p->kind = (UINT)fw_json_num(jw, "kind");
			Kfree(jw);
			jw = NULL;
		}
		if ( p->key > 0 && p->num == 0 ) {
			(void)ob_cls_obj(p->key);	/* words: nothing to work */
			p->key = 0;
		}
		f->np = i + 1;
	}
}

LOCAL FWPART *fw_part( FWIN *f, INT num )
{
	INT	i;

	for ( i = 0; i < f->np; i++ ) {
		if ( f->p[i].num == num && f->p[i].key > 0 ) {
			return &f->p[i];
		}
	}
	return NULL;
}

/* The number of the part a notice names, by its link record */
LOCAL INT fw_num_of( FWIN *f, CONST T_OBNTM *m )
{
	INT	i = m->recno - OB_WR_KIND;

	return ( i >= 0 && i < f->np ) ? f->p[i].num : 0;
}

LOCAL INT fw_value( FWIN *f, INT num )
{
	FWPART	*p = fw_part(f, num);
	INT	v[2] = { 0, 0 };
	SZ	asz = 0;

	if ( p != NULL ) {
		(void)ob_rea_rec(p->key, OB_WR_STATE, 0, v, sizeof(v), &asz);
	}
	return v[0];
}

LOCAL void fw_set_value( FWIN *f, INT num, INT v )
{
	FWPART	*p = fw_part(f, num);
	SZ	asz = 0;

	if ( p != NULL ) {
		(void)ob_wri_rec(p->key, OB_WR_STATE, 0, &v, sizeof(v), &asz);
	}
}

/* A box's letters, or a label's words */
LOCAL void fw_set_text( FWIN *f, INT num, CONST char *s )
{
	FWPART	*p = fw_part(f, num);
	SZ	asz = 0;

	if ( p != NULL ) {
		(void)ob_wri_rec(p->key, OB_WR_STATE, 0, s, (SZ)knl_strlen(s), &asz);
	}
}

/* Whether a part can be worked */
LOCAL void fw_off( FWIN *f, INT num, BOOL off )
{
	FWPART		*p = fw_part(f, num);
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( p == NULL || ob_rea_rec(p->key, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		return;
	}
	wp.flags = off ? ( wp.flags | OB_WP_OFF ) : ( wp.flags & ~(UINT)OB_WP_OFF );
	(void)ob_wri_rec(p->key, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

LOCAL void fw_parts_close( FWIN *f )
{
	INT	i;

	for ( i = 0; i < f->np; i++ ) {
		if ( f->p[i].key > 0 ) {
			(void)ob_cls_obj(f->p[i].key);
		}
		f->p[i].key = 0;
	}
	f->np = 0;
}

/* A window of a panel and the panel, taken away */
LOCAL void fw_take_away( FWIN *f )
{
	fw_parts_close(f);
	if ( f->pk > 0 ) {
		(void)ob_cls_obj(f->pk);
		(void)ob_del_obj(&f->pnl);
	}
	if ( f->kw > 0 ) {
		(void)ob_cls_obj(f->kw);
		(void)ob_del_obj(&f->win);
	}
	f->pk = f->kw = 0;
	f->wid = 0;
}

/*
 * A window of a panel at (x, y) of the screen: attr as a window object's
 * (3: a frame and a band with a name; 256: a pop-up), the work area the
 * panel's size, the panel filling it, its parts opened, and its notices
 * told on the channel with the number id (its parts' with id + 1).
 */
LOCAL ER fw_make( FWIN *f, INT num, INT x, INT y, BOOL centre, UINT attr, UINT id )
{
	T_OBWPOS	wp;
	T_OBCRE		c;
	T_OBMAP		m;
	T_OBNTF		req;
	SZ		asz = 0;
	char		json[224], *meta = NULL, title[WM_TITLE_MAX];
	INT		sw = 0, sh = 0, k;
	CONST char	*nm;

	knl_memset(f, 0, sizeof(*f));
	if ( fw_panel_make(f, num) < E_OK ) {
		return E_OBJ;
	}
	if ( ob_rea_rec(f->pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		fw_take_away(f);
		return E_OBJ;
	}
	f->w = wp.right - wp.left;
	f->h = wp.bottom - wp.top;
	title[0] = 0;
	if ( knl_ob_atr_all(f->pk, 0, (UB **)&meta, NULL) >= 0 ) {
		nm = fw_find(meta, "\"name\":\"");
		if ( nm != NULL ) {
			nm += 8;
			for ( k = 0; nm[k] != 0 && nm[k] != '"' && k < WM_TITLE_MAX - 1; k++ ) title[k] = nm[k];
			title[k] = 0;
		}
		Kfree(meta);
	}
	if ( centre ) {
		wm_screen(&sw, &sh);
		x = ( sw > f->w + 16 ) ? ( sw - f->w - 16 ) / 2 : 0;
		y = ( sh > f->h + 34 ) ? ( sh - f->h - 34 ) / 3 : 0;
	}
	if ( ( attr & WM_ATTR_POPUP ) != 0 ) {
		(void)tm_sprintf((UB *)json, (CONST UB *)"{\"rect\":[%d,%d,%d,%d],\"attr\":%d}",
				 x, y, x + f->w, y + f->h, (INT)attr);
	} else {
		(void)tm_sprintf((UB *)json, (CONST UB *)"{\"rect\":[%d,%d,%d,%d],\"attr\":%d,\"icon\":\"%s\"}",
				 x, y, x + f->w + 16, y + f->h + 34, (INT)attr, SYSDEF_PROG_FQUIT);
	}
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)knl_strlen(json);
	if ( ob_cre_obj(&c, &f->win) < E_OK
	  || ( f->kw = ob_opn_obj(&f->win, OB_OP_R | OB_OP_W | OB_OP_PROT) ) <= 0 ) {
		f->kw = 0;
		fw_take_away(f);
		return E_OBJ;
	}
	/* the work area exactly the panel's size, whatever the frame takes */
	if ( ( attr & WM_ATTR_POPUP ) == 0 && ob_rea_rec(f->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + f->w + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + f->h + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(f->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	knl_memset(&m, 0, sizeof(m));
	if ( ob_map_rec(f->pk, 0, f->kw, &m) < E_OK ) {
		fw_take_away(f);
		return E_OBJ;
	}
	fw_parts_open(f);
	f->wid = wm_obj_wid(&f->win);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE | OB_E_PRESS | OB_E_KEY;
	req.id = id;
	if ( ob_ntf_evt(f->kw, OB_REC_ANY, &req, fw_port) <= 0 ) {
		fw_take_away(f);
		return E_OBJ;
	}
	req.events = OB_E_CHANGE;
	req.id = id + 1;
	(void)ob_ntf_evt(f->pk, OB_REC_ANY, &req, fw_port);
	return E_OK;
}

/* ---------------------------------------------------------------- what the list shows */

LOCAL void fq_cpu_text( char *b, UD ms )
{
	UD	s = ms / 1000;

	if ( s < 60 ) {
		(void)tm_sprintf((UB *)b, (CONST UB *)"%d.%d秒", (INT)s, (INT)( ( ms % 1000 ) / 100 ));
	} else {
		(void)tm_sprintf((UB *)b, (CONST UB *)"%d分%02d秒", (INT)( s / 60 ), (INT)( s % 60 ));
	}
}

LOCAL void fq_mem_text( char *b, UD bytes )
{
	if ( bytes >= 10ULL * 1024 * 1024 ) {
		(void)tm_sprintf((UB *)b, (CONST UB *)"%dMB", (INT)( bytes >> 20 ));
	} else {
		(void)tm_sprintf((UB *)b, (CONST UB *)"%dKB", (INT)( bytes >> 10 ));
	}
}

LOCAL CONST char *fq_state_text( CONST FQROW *r )
{
	return r->killed ? "強制的に終わらせました。後始末を待っています"
	     : ( r->ending && r->incall > 0 ) ? "終了の途中(システムコールの中で止まっています)"
	     : r->ending ? "終了の途中"
	     : r->sys ? "システムのプロセス(強制終了できません)"
	     : "実行中";
}

/* The rows made again from the processes there are */
LOCAL void fq_refresh( void )
{
	ID		*pids;
	T_PRCINFO	pi;
	INT		n, i, k = 0;

	/* a row for every process: the rows' room made as large as there are processes */
	n = fq_pids(&pids);
	if ( pids == NULL ) {
		return;				/* the rows as they were */
	}
	if ( n > fq_cap ) {
		FQROW	*t = (FQROW *)Kmalloc(sizeof(FQROW) * (SZ)n);

		if ( t == NULL ) {
			Kfree(pids);
			return;
		}
		if ( fq_row != NULL ) Kfree(fq_row);
		fq_row = t;
		fq_cap = n;
	}
	for ( i = 0; i < n && k < fq_cap; i++ ) {
		FQROW	*r = &fq_row[k];
		INT	nw;

		if ( knl_prc_info(pids[i], &pi) < E_OK || pi.done ) {
			continue;		/* its end told: only collecting it is left */
		}
		knl_memset(r, 0, sizeof(*r));
		r->pid = pi.pid;
		r->sys = pi.sys;
		r->ending = pi.ending;
		r->killed = pi.killed;
		r->incall = pi.incall;
		r->cpu_ms = pi.cpu_ms;
		r->memsz = pi.memsz;
		nw = knl_wmobj_count(pi.pid, FQ_LOCK_MS);
		r->nwin = ( nw > 0 ) ? nw : 0;
		(void)knl_prc_name(pi.pid, r->name, sizeof(r->name));
		k++;
	}
	fq_nrow = k;
	fq_refresh_at = fq_now();
	Kfree(pids);
}

/* The row chosen in the list (0 up), -1 none */
LOCAL INT fw_sel( void )
{
	INT	v = ( fw_up ) ? fw_value(&fw_main, P_LIST) : 0;

	return ( v >= 1 && v <= fq_nrow ) ? v - 1 : -1;
}

/* What is shown of the row chosen, and whether 〈強制終了〉 can be pressed */
LOCAL void fw_detail( void )
{
	INT	s = fw_sel();
	char	b[48];

	if ( s < 0 ) {
		fw_set_text(&fw_main, P_STATE, "");
		fw_set_text(&fw_main, P_NWIN, "");
		fw_set_text(&fw_main, P_CPU, "");
		fw_set_text(&fw_main, P_MEM, "");
		fw_off(&fw_main, P_END, TRUE);
		return;
	}
	fw_set_text(&fw_main, P_STATE, fq_state_text(&fq_row[s]));
	(void)tm_sprintf((UB *)b, (CONST UB *)"%d", fq_row[s].nwin);
	fw_set_text(&fw_main, P_NWIN, b);
	fq_cpu_text(b, fq_row[s].cpu_ms);
	fw_set_text(&fw_main, P_CPU, b);
	fq_mem_text(b, fq_row[s].memsz);
	fw_set_text(&fw_main, P_MEM, b);
	fw_off(&fw_main, P_END, (BOOL)( fq_row[s].sys || fq_row[s].killed ));
}

/*
 * The list made again from the rows: a name for each, the program's and
 * its number. Written only when it changed, so that the list does not
 * jump while it is looked at; the row chosen stays with its process.
 */
LOCAL void fw_list( ID keep )
{
	UB	*buf;
	SZ	len = 0, asz = 0, room;
	INT	i, v = 0;
	FWPART	*p = fw_part(&fw_main, P_LIST);

	/* room for every row's name, as long as the longest can be */
	room = (SZ)( fq_nrow + 1 ) * ( FQ_NAME_MAX + 48 );
	buf = (UB *)Kmalloc(room);
	if ( buf == NULL || p == NULL ) {
		if ( buf != NULL ) Kfree(buf);
		return;
	}
	for ( i = 0; i < fq_nrow; i++ ) {
		char	one[FQ_NAME_MAX + 48];
		SZ	k;

		(void)tm_sprintf((UB *)one, (CONST UB *)"%s (pid %d)%s", fq_row[i].name, fq_row[i].pid,
				 fq_row[i].killed ? "　強制的に終了" : fq_row[i].ending ? "　終了の途中" : "");
		k = (SZ)knl_strlen(one);
		if ( len + k + 1 > room ) {
			break;			/* not reached: the room was made for every row */
		}
		knl_memcpy(buf + len, one, k + 1);
		len += k + 1;
		if ( fq_row[i].pid == keep ) {
			v = i + 1;
		}
	}
	for ( i = 0; fw_names != NULL && i < (INT)len && len == fw_names_len && buf[i] == fw_names[i]; i++ ) ;
	if ( fw_names == NULL || len != fw_names_len || i < (INT)len ) {
		(void)ob_wri_rec(p->key, OB_WR_KIND, 0, buf, len, &asz);
		if ( fw_names != NULL ) Kfree(fw_names);
		fw_names = buf;			/* kept as what the list was given */
		fw_names_len = len;
	} else {
		Kfree(buf);
	}
	if ( v == 0 && fq_nrow > 0 ) {
		v = fw_value(&fw_main, P_LIST);
		if ( v < 1 ) v = 1;
		if ( v > fq_nrow ) v = fq_nrow;
	}
	if ( v != fw_value(&fw_main, P_LIST) ) {
		fw_set_value(&fw_main, P_LIST, v);
	}
	fw_detail();
}

/* The list looked at again */
LOCAL void fw_relist( void )
{
	INT	s = fw_sel();
	ID	keep = ( s >= 0 ) ? fq_row[s].pid : 0;

	fq_refresh();
	fw_list(keep);
}

/* What came of something, in the box at the foot */
LOCAL void fw_status( CONST char *msg )
{
	if ( fw_up ) {
		fw_set_text(&fw_main, P_MSG, msg);
	}
}

/* ---------------------------------------------------------------- the window */

LOCAL void fw_open( void )
{
	T_OBCRE	c;

	if ( fw_port <= 0 ) {
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_CHANNEL;
		if ( ob_cre_obj(&c, &fw_ch) >= E_OK ) {
			fw_port = ob_opn_obj(&fw_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
		}
		if ( fw_port <= 0 ) {
			fw_port = 0;
			fq_say("強制終了のウインドウを開けません");
			return;
		}
	}
	fw_was_focus = wm_focused();
	if ( fw_make(&fw_main, PN_MAIN, 0, 0, TRUE, WM_ATTR_FRAME | WM_ATTR_TITLE, 1) < E_OK ) {
		fq_say("強制終了のウインドウを開けません");
		return;
	}
	fw_names_len = (SZ)-1;
	fq_grab = knl_wdog_stuck(NULL, NULL);
	fw_up = TRUE;
	fq_refresh();
	fw_list(0);
	fw_set_text(&fw_main, P_MSG, "");
	if ( fq_desk == NULL ) {
		fw_off(&fw_main, P_RESTART, TRUE);
	}
	if ( fw_main.wid > 0 ) {
		(void)wm_raise(fw_main.wid);
		(void)wm_focus(fw_main.wid);
	}
	wm_update();
	fq_grab = knl_wdog_stuck(NULL, NULL);
	tm_printf((UB *)"TessronOS: 強制終了: the window is up (%d processes)\n", fq_nrow);
}

LOCAL void fw_ask_close( void )
{
	if ( fw_asking ) {
		fw_take_away(&fw_ask);
		fw_asking = FALSE;
		if ( fw_main.wid > 0 ) {
			(void)wm_focus(fw_main.wid);
		}
		wm_update();
	}
}

LOCAL void fw_close( void )
{
	if ( !fw_up ) {
		return;
	}
	fq_grab = FALSE;
	fw_ask_close();
	fw_up = FALSE;
	fw_take_away(&fw_main);
	if ( fw_was_focus > 0 ) {
		(void)wm_focus(fw_was_focus);	/* E_ID when it went meanwhile */
	}
	wm_update();
}

/*
 * 〈デスクトップを立ち上げ直す〉: a panel that warns asks first, saying
 * what will be lost; its 〈立ち上げ直す〉 does it, and 〈取消〉, the one
 * Enter presses, does not.
 */
LOCAL void fw_ask_open( void )
{
	T_WMWIN	w;
	char	lost[FQ_MSG_MAX];
	INT	x = 100, y = 100;

	if ( fq_desk == NULL ) {
		fw_status("立ち上げ直すデスクトップがありません");
		return;
	}
	if ( fw_asking ) {
		return;
	}
	if ( fw_main.wid > 0 && wm_ref(fw_main.wid, &w) >= E_OK ) {
		x = w.work.left + ( fw_main.w - 480 ) / 2;
		y = w.work.top + ( fw_main.h - 150 ) / 2;
	}
	if ( fw_make(&fw_ask, PN_ASK, x, y, FALSE, WM_ATTR_POPUP, 3) < E_OK ) {
		fw_status("尋ねるパネルを開けません");
		return;
	}
	fw_asking = TRUE;
	(void)fq_unsaved(lost, sizeof(lost));
	fw_set_text(&fw_ask, Q_LINE1, "デスクトップを立ち上げ直します。");
	fw_set_text(&fw_ask, Q_LINE2, lost);
	fw_set_text(&fw_ask, Q_LINE3, "プロセスとして動くプログラムは、そのまま残ります。");
	if ( fw_ask.wid > 0 ) {
		(void)wm_raise(fw_ask.wid);
		(void)wm_focus(fw_ask.wid);
	}
	wm_update();
	tm_printf((UB *)"TessronOS: 強制終了: asked whether to start the desktop again: %s\n", lost);
}

/* 〈立ち上げ直す〉 of the panel that asked */
LOCAL void fw_restart( void )
{
	ID	tids[FQ_DESK_TSK], main;
	INT	n, i;
	UINT	imask;

	fw_ask_close();
	fw_status("デスクトップを立ち上げ直しています");
	if ( !fq_restart_stop() ) {
		return;
	}
	ISpinLock(&fq_lock, &imask);
	n = fq_job.ntid;
	main = fq_job.main;
	for ( i = 0; i < n; i++ ) tids[i] = fq_job.tids[i];
	fq_job.restart = FALSE;
	ISpinUnlock(&fq_lock, &imask);
	fq_restart_rest(tids, n, main);
	fw_front_until = fq_now() + FW_FRONT_MS;
	fq_grab = ( fw_up && knl_wdog_stuck(NULL, NULL) );
}

LOCAL void fw_end_sel( void )
{
	INT	s = fw_sel();

	if ( s < 0 ) {
		return;
	}
	fq_end(fq_row[s].pid);
	fw_relist();
}

/* The window's menu: the common items and its own (SYSDEF_MENU_FQUIT) */
LOCAL void fw_menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	INT	pid, cmd;
	BOOL	chosen;

	if ( ts_str_to_uuid(SYSDEF_MENU_FQUIT, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) {
		return;
	}
	if ( fw_sel() < 0 || fq_row[fw_sel()].sys ) {
		(void)mn_chg_atr(mid, NULL, "end", MN_GREY);
	}
	if ( fq_desk == NULL ) {
		(void)mn_chg_atr(mid, NULL, "restart", MN_GREY);
	}
	/* opened on the window and waited for: the window manager's objects work it as the input comes */
	chosen = FALSE;
	mn_winlist_fill(mid);			/* ウインドウ */
	mn_tail_fill(mid);			/* 小物 and 実行 */
	pid = ( fw_main.wid > 0 ) ? mn_opn_men(mid, fw_main.wid, m->x, m->y) : E_OBJ;
	if ( pid >= 0 ) {
		wm_panel_draw(pid);
		wm_update();
		cmd = knl_wmobj_menu_run(pid, m->when);
		wm_panel_close(pid);
		wm_update();
		chosen = (BOOL)( cmd > 0 && mn_get_sel(mid, cmd, &sel) >= E_OK
				 && !mn_winlist_do(&sel) && !mn_tail_do(&sel) );
	}
	(void)mn_del_men(mid);
	if ( !chosen ) {
		return;
	}
	if ( mn_is(&sel, "close") ) {
		fw_close();
	} else if ( mn_is(&sel, "end") ) {
		fw_end_sel();
	} else if ( mn_is(&sel, "restart") ) {
		fw_ask_open();
	}
}

/* A key told to the window: one no part took */
LOCAL void fw_key( CONST T_OBNTM *m )
{
	INT	v = fw_value(&fw_main, P_LIST);

	if ( ( m->mods & OB_KEY_UP ) != 0 ) {
		return;
	}
	if ( ( m->mods & FQ_MOD_CTRL ) != 0 ) {
		UB	c = wm_key_char(m->code, m->mods);

		if ( c == 'e' || c == 'E' ) {
			fw_close();		/* 閉じる of the common menu */
		}
		return;
	}
	switch ( m->code ) {
	case FQ_KEY_UP:		v--;			break;
	case FQ_KEY_DOWN:	v++;			break;
	case FQ_KEY_PGUP:	v -= 8;			break;
	case FQ_KEY_PGDN:	v += 8;			break;
	case FQ_KEY_HOME:	v = 1;			break;
	case FQ_KEY_END:	v = fq_nrow;		break;
	case FQ_KEY_ESC:
		fw_close();
		return;
	default:
		return;
	}
	if ( v > fq_nrow ) v = fq_nrow;
	if ( v < 1 ) v = ( fq_nrow > 0 ) ? 1 : 0;
	fw_set_value(&fw_main, P_LIST, v);
	fw_detail();
}

/* The notices of the window and of the panel asking, as they came */
LOCAL void fw_notices( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	num;

	while ( fw_up && fw_port > 0 && ob_rea_rec(fw_port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		switch ( m.id ) {
		case 1:				/* the window */
			if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
				fw_close();
			} else if ( m.event == OB_E_PRESS && m.code == 1 && m.recno < 0 && !fq_grab ) {
				fw_menu(&m);
			} else if ( m.event == OB_E_KEY ) {
				fw_key(&m);
			}
			break;
		case 2:				/* its panel's parts */
			if ( m.event != OB_E_CHANGE ) {
				break;
			}
			num = fw_num_of(&fw_main, &m);
			if ( num == P_LIST ) {
				fw_detail();
			} else if ( num == P_END ) {
				fw_end_sel();
			} else if ( num == P_CANCEL ) {
				fw_close();
			} else if ( num == P_RESTART ) {
				fw_ask_open();
			}
			break;
		case 3:				/* the panel asking */
			if ( m.event == OB_E_CLOSE || ( m.event == OB_E_KEY && m.code == FQ_KEY_ESC ) ) {
				fw_ask_close();
			}
			break;
		case 4:				/* its parts */
			if ( m.event != OB_E_CHANGE || !fw_asking ) {
				break;
			}
			num = fw_num_of(&fw_ask, &m);
			if ( num == Q_RESTART ) {
				fw_restart();
			} else if ( num == Q_CANCEL ) {
				fw_ask_close();
			}
			break;
		default:
			break;
		}
	}
}

/*
 * With the desktop stopped, nobody reads the input and hands it to the
 * window manager's objects (wm_read_event, knl_wmobj_event), which work
 * the panels' parts and tell the notices. While the window is up, the
 * input layer gives every event here instead (knl_fq_input), and it is
 * handed on the same way: addressed to the window it is over, or for a
 * key to the window asking or else the window, its place measured from
 * that window's work area. Only this window and the panel asking are
 * worked so; a press anywhere else does nothing. The window's frame is
 * not worked (it is not moved, nor its menu opened) until the desktop
 * reads the input again.
 */
LOCAL void fw_feed( CONST T_HIDEV *h )
{
	T_WMEV	ev;
	T_WMWIN	w;
	INT	wid;

	knl_memset(&ev, 0, sizeof(ev));
	ev.type = h->type;
	ev.code = h->code;
	ev.mods = h->mods;
	ev.when = h->when;
	ev.x = h->x;
	ev.y = h->y;
	ev.dz = h->dz;
	if ( h->type == HID_EV_KEY_DOWN || h->type == HID_EV_KEY_UP ) {
		wid = fw_asking ? fw_ask.wid : fw_main.wid;
	} else {
		wid = wm_at(h->x, h->y);
		if ( wid != fw_main.wid && !( fw_asking && wid == fw_ask.wid ) ) {
			return;
		}
	}
	if ( wid <= 0 || wm_ref(wid, &w) < E_OK ) {
		return;
	}
	ev.wid = wid;
	if ( h->type != HID_EV_KEY_DOWN && h->type != HID_EV_KEY_UP ) {
		ev.x -= w.work.left;
		ev.y -= w.work.top;
	}
	knl_wmobj_event(&ev);
}

/* ---------------------------------------------------------------- what is said */

/*
 * Said on the message line, which the desktop lays along the foot of the
 * screen and the compositor lays over it whether the desktop runs or
 * not. Without one (the desktop never started) the console alone says
 * it: a window of its own for a line of words would be one more thing
 * that is not a part of the system's.
 */
LOCAL void fq_show( CONST char *text )
{
	if ( wm_msg_height() > 0 ) {
		(void)wm_msg_put(text);
	}
}

/* ---------------------------------------------------------------- fq_work */

LOCAL void fq_work( INT stacd, void *exinf )
{
	char	text[FQ_MSG_MAX];
	T_HIDEV	ev;
	ID	end, tids[FQ_DESK_TSK], main;
	BOOL	say, sel, open, restart;
	INT	n, i;
	UINT	imask;

	for ( ;; ) {
		TMO	tmo = fw_up ? 50 : TMO_FEVR;

		(void)tk_wai_sem(fq_wsem, 1, tmo);
		fq_busy_at = fq_now() | 1;

		ISpinLock(&fq_lock, &imask);
		say = fq_job.say;
		if ( say ) {
			fq_copy(text, fq_job.text, FQ_MSG_MAX);
		}
		end = fq_job.end;
		sel = fq_job.end_sel;
		open = fq_job.open;
		restart = fq_job.restart;
		n = fq_job.ntid;
		main = fq_job.main;
		for ( i = 0; i < n && i < FQ_DESK_TSK; i++ ) tids[i] = fq_job.tids[i];
		fq_job.say = FALSE;
		fq_job.end = 0;
		fq_job.end_sel = FALSE;
		fq_job.open = FALSE;
		fq_job.restart = FALSE;
		ISpinUnlock(&fq_lock, &imask);

		if ( restart ) {
			fq_restart_rest(tids, n, main);
			fw_front_until = fq_now() + FW_FRONT_MS;
		}
		if ( end > 0 ) {
			fq_end(end);
			if ( fw_up ) fw_relist();
		}
		if ( open ) {
			if ( fw_up ) {
				fw_close();
			} else {
				fw_open();
			}
		}
		if ( sel && fw_up ) {
			fw_end_sel();
		}
		/* the input taken while the desktop does not read it, handed on */
		while ( fq_in_take(&ev) ) {
			if ( fw_up ) {
				fw_feed(&ev);
			}
		}
		if ( fw_up ) {
			fw_notices();
		}
		/* what was said, after whatever else the same turn said */
		ISpinLock(&fq_lock, &imask);
		if ( fq_job.say ) {
			fq_copy(text, fq_job.text, FQ_MSG_MAX);
			fq_job.say = FALSE;
			say = TRUE;
		}
		ISpinUnlock(&fq_lock, &imask);
		if ( say ) {
			fq_show(text);
		}
		if ( fw_up ) {
			/* the input taken here only while the desktop does not read it */
			fq_grab = knl_wdog_stuck(NULL, NULL);
			if ( fq_now() - fq_refresh_at >= FQ_REFRESH_MS ) {
				fw_relist();
				/*
				 * A desktop started again opens its windows in front
				 * as it comes up: the window that started it is kept
				 * in front of them for a while.
				 */
				if ( fw_front_until != 0 && fq_now() < fw_front_until && fw_main.wid > 0 ) {
					(void)wm_raise(fw_main.wid);
					wm_update();
				}
			}
		}
		fq_busy_at = 0;
	}
}

/* ---------------------------------------------------------------- start */

EXPORT void knl_fq_open( void )
{
	UINT	imask;

	if ( !fq_ready ) {
		return;
	}
	ISpinLock(&fq_lock, &imask);
	fq_n_list++;
	ISpinUnlock(&fq_lock, &imask);
	(void)tk_sig_sem(fq_sem, 1);
}

EXPORT ER knl_fq_init( void )
{
	T_CSEM	cs;
	T_CTSK	ct;

	if ( fq_ready ) {
		return E_OK;
	}
	/* the desktop's variables as they are before it ever ran, to put back when it is started again */
	fq_dt_isz = (SZ)( __dt_data_end - __dt_data_start );
	if ( fq_dt_isz > 0 ) {
		fq_dt_image = (UB *)Kmalloc(fq_dt_isz);
		if ( fq_dt_image != NULL ) {
			knl_memcpy(fq_dt_image, __dt_data_start, fq_dt_isz);
		}
	}
	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 0;
	cs.maxsem = 0x7FFFFFFF;
	fq_sem = tk_cre_sem(&cs);
	fq_wsem = tk_cre_sem(&cs);
	if ( fq_sem <= 0 || fq_wsem <= 0 ) {
		return E_LIMIT;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)fq_task;
	ct.itskpri = FQ_PRI;
	ct.stksz = FQ_STACK;
	fq_tsk = tk_cre_tsk(&ct);
	ct.task = (FP)fq_work;
	ct.itskpri = FQ_WORK_PRI;
	ct.stksz = FQ_WORK_STACK;
	fq_wtsk = tk_cre_tsk(&ct);
	if ( fq_tsk <= 0 || fq_wtsk <= 0 ) {
		return E_LIMIT;
	}
	(void)tk_sta_tsk(fq_tsk, 0);
	(void)tk_sta_tsk(fq_wtsk, 0);
	fq_ready = TRUE;
	tm_printf((UB *)"TessronOS: 強制終了 is ready (desktop data %d bytes, zeroed %d bytes)\n",
		  (INT)fq_dt_isz, (INT)( __dt_bss_end - __dt_bss_start ));

	return E_OK;
}

#if USE_KTEST
EXPORT void knl_fq_stat( T_FQSTAT *st )
{
	UINT	imask;
	INT	i, k;

	knl_memset(st, 0, sizeof(*st));
	st->wid = fw_up ? fw_main.wid : 0;
	st->askwid = fw_asking ? fw_ask.wid : 0;
	st->grab = fq_grab;
	st->nrow = fq_nrow;
	for ( i = 0; i < fq_nrow && i < FQ_ROWS_SEEN; i++ ) {
		st->pid[i] = fq_row[i].pid;
	}
	k = fw_sel();
	st->sel = ( k >= 0 ) ? fq_row[k].pid : 0;
	st->confirm = fw_asking;
	st->armed = (BOOL)( fq_armed_at != 0 );
	st->restarts = fq_restarted;
	ISpinLock(&fq_lock, &imask);
	st->said = fq_said;
	fq_copy(st->text, fq_said_text, sizeof(st->text));
	ISpinUnlock(&fq_lock, &imask);
}
#endif
