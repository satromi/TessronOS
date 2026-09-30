/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	bk_main.c
 *	バックアップ: the accessory (design 17.18)
 *
 *	Its window has two sheets, 保存 and 復帰, chosen with their tabs or
 *	Tab. 保存 lists what a cabinet links to, starting at the first
 *	cabinet (or the object it was started on): ↑↓ choose, → goes into
 *	one, ← back out; Space marks one to be saved (●), several may be,
 *	in any cabinet gone into, and X marks one to be left out (×) with
 *	what only it links to. D or a press on 保存先 goes through where the
 *	volumes may go -- objects of their own, or the disks the system has,
 *	each mounted through its device object while it is used. Enter or
 *	[保存] saves what is marked, or else the one chosen, and all they
 *	link to; each is saved with where it was linked from, for a restore
 *	to put it back there. Volumes kept as objects take no more than the
 *	store they go on has free. 復帰 lists the
 *	volumes a place has; Enter reads the chosen one's head and shows it,
 *	Enter again or [復帰] makes the objects and links the first of them
 *	into the first cabinet, and the desktop opens it. A set of several
 *	volumes asks for each next one, and takes only the one it wants.
 *	While the work goes on, Esc or [中止] stops it; what a restore had
 *	made is then taken away again.
 *
 *	Links dropped on the window from the desktop (OB_E_DROP) become the
 *	list of the 保存 sheet and are marked to be saved; dropped with
 *	Shift held, they are marked to be left out instead.
 *
 *	With a T_BKARG as its argument (bk_arg.h) it does the work without
 *	the window, says on the console how it went and ends.
 */

#include "bk.h"
#include "bk_arg.h"
#include "bk_vol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/json.h>
#include <ts/hid.h>

#define WORK_W		540
#define WORK_H		400
#define TITLE_H		34
#define FRAME_W		16
#define TEXT_PX		16
#define ROW_H		22
#define ROWS		8
#define LIST_Y		84

#define K_ENTER		0x28
#define K_ESC		0x29
#define K_TAB		0x2B
#define K_SPACE		0x2C
#define K_RIGHT		0x4F
#define K_LEFT		0x50
#define K_DOWN		0x51
#define K_UP		0x52
#define K_KPENTER	0x58
#define M_CTRL		0x11

#define C_TEXT		0x00000000
#define C_DIM		0x00707070
#define C_GROUND	0x00FFFFFF
#define C_BUTTON	0x00E8E8E8
#define C_SEL		0x00C8D8F0
#define C_BAR		0x004070C0
#define C_ERR		0x00C00000

#define ITEM_MAX	128
#define DEPTH_MAX	16

enum { T_SAVE, T_REST };
enum { S_IDLE, S_BUSY, S_PEEKED, S_NEXT };

typedef struct {
	char	name[BK_NAME_MAX];
	TS_UUID	uuid;			/* an object */
	char	file[FS_NAME_MAX];	/* a file */
	TS_UUID	parent;			/* an object: the one whose record links to it */
	BOOL	hasbox;
	INT	box[4];			/* where that link is */
} ITEM;

LOCAL BKJOB	job;
LOCAL TS_UUID	win, ch, argobj;
LOCAL BOOL	argvol;		/* started on a volume */
LOCAL ID	kw, port;
LOCAL INT	gid = -1;
LOCAL INT	tab = T_SAVE, state = S_IDLE;
LOCAL BOOL	closing, dirty = TRUE, failed;
LOCAL char	status[256];
LOCAL UD	prog_n, prog_total;

LOCAL BKMEDIA	media[BK_MED_MAX];
LOCAL INT	nmedia, dest, srcm;	/* where saving goes, where restoring reads */

LOCAL ITEM	items[ITEM_MAX];
LOCAL INT	nitem, sel, top;
LOCAL TS_UUID	place[DEPTH_MAX];	/* the cabinets gone into */
LOCAL INT	nplace;
LOCAL BOOL	dropped;		/* the list is what was dropped on the window */

LOCAL BKROOT	pick[BK_ROOT_MAX];	/* marked to be saved */
LOCAL INT	npick;
LOCAL TS_UUID	excl[BK_EXCL_MAX];	/* marked to be left out */
LOCAL INT	nexcl;

LOCAL BKVOL	vol;			/* the volume being read */
LOCAL char	peekinfo[3][160];
LOCAL char	expect[BK_NAME_MAX];	/* the name the next volume must have */
LOCAL T_DPRECT	b_tab[2], b_go, b_stop, b_dest;

/* ---------------------------------------------------------------- saying things */

LOCAL void say( const char *s )
{
	char	line[300];

	strncpy(status, s, sizeof(status) - 1);
	status[sizeof(status) - 1] = 0;
	(void)wm_msg_put(s);
	snprintf(line, sizeof(line), "backup: %s\n", s);
	tm_putstring((const UB *)line);
	dirty = TRUE;
}

LOCAL void grouped( UD v, char *out )
{
	char	d[32];
	INT	n = snprintf(d, sizeof(d), "%llu", (unsigned long long)v), i, k = 0;

	for ( i = 0; i < n; i++ ) {
		if ( i > 0 && ( n - i ) % 3 == 0 ) out[k++] = ',';
		out[k++] = d[i];
	}
	out[k] = 0;
}

/* ---------------------------------------------------------------- lists */

/* The name of an object, from its metadata or else its reference */
LOCAL void name_of( const TS_UUID *u, char *out, INT max )
{
	static UB	meta[OB_ATR_MAX + 1];
	T_JSON	root;
	SZ	asz = 0;
	T_OBREF	r;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	out[0] = 0;
	if ( k > 0 ) {
		if ( ob_get_atr(k, meta, OB_ATR_MAX, &asz) >= E_OK && js_parse(meta, (INT)asz, &root) >= E_OK ) {
			(void)js_get_str(&root, "name", (UB *)out, max);
		}
		ob_cls_obj(k);
	}
	if ( out[0] == 0 && ob_ref_obj(u, &r) >= E_OK ) {
		strncpy(out, (const char *)r.name, (size_t)max - 1);
		out[max - 1] = 0;
	}
}

/* The objects a cabinet links to */
LOCAL void list_objects( const TS_UUID *cab )
{
	UB	*x;
	UINT	len = 0;
	const char *p;
	ID	k;

	nitem = sel = top = 0;
	k = ob_opn_obj(cab, OB_OP_R);
	if ( k <= 0 ) return;
	x = bk_read_rec(k, 0, &len);
	ob_cls_obj(k);
	if ( x == NULL ) return;
	for ( p = (const char *)x; nitem < ITEM_MAX && ( p = strstr(p, "<link") ) != NULL; p++ ) {
		const char *id = strstr(p, " id=\"");
		const char *end = strchr(p, '>');
		ITEM	*it = &items[nitem];
		T_OBREF	r;
		INT	i;
		BOOL	dup = FALSE;

		if ( id == NULL || end == NULL || id > end || !bk_uuid_parse(id + 5, &it->uuid) ) continue;
		if ( ob_ref_obj(&it->uuid, &r) < E_OK || r.type != OB_T_STORAGE ) continue;
		for ( i = 0; i < nitem; i++ ) {
			if ( memcmp(&items[i].uuid, &it->uuid, sizeof(TS_UUID)) == 0 ) dup = TRUE;
		}
		if ( dup ) continue;
		name_of(&it->uuid, it->name, sizeof(it->name));
		it->file[0] = 0;
		it->parent = *cab;
		it->hasbox = bk_tag_box(p, end, it->box);
		nitem++;
	}
	free(x);
}

/* ---------------------------------------------------------------- marks */

LOCAL BOOL same( const TS_UUID *a, const TS_UUID *b )
{
	return (BOOL)( memcmp(a, b, sizeof(TS_UUID)) == 0 );
}

LOCAL INT pick_at( const TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < npick; i++ ) if ( same(&pick[i].uuid, u) ) return i;
	return -1;
}

LOCAL INT excl_at( const TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < nexcl; i++ ) if ( same(&excl[i], u) ) return i;
	return -1;
}

/* An item marked to be saved, with where it is linked from */
LOCAL BOOL pick_add( const ITEM *it )
{
	INT	x = excl_at(&it->uuid);

	if ( pick_at(&it->uuid) >= 0 ) return TRUE;
	if ( npick >= BK_ROOT_MAX ) return FALSE;
	if ( x >= 0 ) excl[x] = excl[--nexcl];
	memset(&pick[npick], 0, sizeof(BKROOT));
	pick[npick].uuid = it->uuid;
	pick[npick].parent = it->parent;
	pick[npick].hasbox = it->hasbox;
	memcpy(pick[npick].box, it->box, sizeof(it->box));
	npick++;
	return TRUE;
}

LOCAL BOOL excl_add( const TS_UUID *u )
{
	INT	x = pick_at(u);

	if ( excl_at(u) >= 0 ) return TRUE;
	if ( nexcl >= BK_EXCL_MAX ) return FALSE;
	if ( x >= 0 ) pick[x] = pick[--npick];
	excl[nexcl++] = *u;
	return TRUE;
}

/* How many are marked, on the console */
LOCAL void marks_said( void )
{
	char	s[80];

	snprintf(s, sizeof(s), "backup: picked %d left out %d\n", npick, nexcl);
	tm_putstring((const UB *)s);
}

/* The one chosen marked to be saved, or no longer */
LOCAL void pick_toggle( void )
{
	INT	i;

	if ( sel >= nitem ) return;
	i = pick_at(&items[sel].uuid);
	if ( i >= 0 ) pick[i] = pick[--npick];
	else if ( !pick_add(&items[sel]) ) return;
	if ( sel + 1 < nitem ) sel++;
	dirty = TRUE;
	marks_said();
}

/* The one chosen marked to be left out, or no longer */
LOCAL void excl_toggle( void )
{
	INT	i;

	if ( sel >= nitem ) return;
	i = excl_at(&items[sel].uuid);
	if ( i >= 0 ) excl[i] = excl[--nexcl];
	else if ( !excl_add(&items[sel].uuid) ) return;
	if ( sel + 1 < nitem ) sel++;
	dirty = TRUE;
	marks_said();
}

/* The volumes a place has: files on a medium, or objects linked from the first cabinet */
LOCAL void list_volumes( void )
{
	BKMEDIA	*m = &media[srcm];

	nitem = sel = top = 0;
	if ( m->kind == BK_MED_OBJECT ) {
		static ITEM keep[ITEM_MAX];
		TS_UUID	cab;
		BKVOL	v;
		INT	n = 0, i;

		if ( argvol ) {
			/* the volume it was started on comes first */
			memset(&keep[0], 0, sizeof(keep[0]));
			keep[0].uuid = argobj;
			name_of(&argobj, keep[0].name, sizeof(keep[0].name));
			n = 1;
		}
		(void)bk_first_cabinet(&cab);
		list_objects(&cab);
		/* only those that hold a volume */
		for ( i = 0; i < nitem && n < ITEM_MAX; i++ ) {
			if ( argvol && memcmp(&items[i].uuid, &argobj, sizeof(TS_UUID)) == 0 ) continue;
			memset(&v, 0, sizeof(v));
			v.obj = items[i].uuid;
			if ( bk_vol_open(&v) < E_OK ) continue;
			(void)bk_vol_done(&v);
			keep[n++] = items[i];
		}
		memcpy(items, keep, sizeof(ITEM) * (size_t)n);
		nitem = n;
		sel = top = 0;
		return;
	}
	if ( bk_media_open(m, FALSE) < E_OK ) {
		say("媒体を読めません");
		return;
	}
	{
		static char names[ITEM_MAX][FS_NAME_MAX];
		INT	n = bk_media_files(m, names, ITEM_MAX), i, k = 0;
		BKVOL	v;

		/* only files that begin as a volume does */
		for ( i = 0; i < n; i++ ) {
			memset(&v, 0, sizeof(v));
			snprintf(v.path, sizeof(v.path), "%s/%s", m->dir, names[i]);
			if ( bk_vol_open(&v) < E_OK ) continue;
			(void)bk_vol_done(&v);
			memset(&items[k], 0, sizeof(items[k]));
			strncpy(items[k].file, names[i], sizeof(items[k].file) - 1);
			(void)xfu_name_in((const UB *)names[i], (UB *)items[k].name, sizeof(items[k].name));
			k++;
		}
		nitem = k;
	}
}

/* The objects dropped no longer listed */
LOCAL void drop_let_go( void )
{
	dropped = FALSE;
}

LOCAL void list_again( void )
{
	if ( tab == T_SAVE && dropped ) {
		dirty = TRUE;
		return;				/* what was dropped stays listed */
	}
	if ( dropped ) drop_let_go();		/* the other sheet lists volumes instead */
	if ( tab == T_SAVE ) list_objects(&place[nplace - 1]);
	else list_volumes();
	dirty = TRUE;
}

/* ---------------------------------------------------------------- drawing */

LOCAL void text( INT x, INT y, const char *s, UW colour )
{
	(void)dp_text(gid, x, y, (const UB *)s, colour, TEXT_PX);
}

LOCAL void button( T_DPRECT *b, INT x, INT y, INT w, const char *label, BOOL on )
{
	INT	tw;

	b->left = x;
	b->top = y;
	b->right = x + w;
	b->bottom = y + 30;
	(void)dp_fill_rect(gid, b, on ? C_BUTTON : C_GROUND);
	(void)dp_frame_rect(gid, b, on ? C_TEXT : C_DIM, 1);
	tw = dp_text_width((const UB *)label, TEXT_PX);
	text(x + ( w - tw ) / 2, y + 21, label, on ? C_TEXT : C_DIM);
}

LOCAL void draw( void )
{
	T_DPRECT	all, r;
	INT		i, y;
	char		line[300], name[BK_NAME_MAX];
	static const char *const tabs[2] = { "保　存", "復　帰" };
	BOOL		busy = ( state == S_BUSY );

	if ( gid < 0 ) return;
	all.left = 0;
	all.top = 0;
	all.right = WORK_W;
	all.bottom = WORK_H;
	(void)dp_fill_rect(gid, &all, C_GROUND);

	for ( i = 0; i < 2; i++ ) {
		b_tab[i].left = 16 + i * 108;
		b_tab[i].top = 8;
		b_tab[i].right = b_tab[i].left + 100;
		b_tab[i].bottom = 38;
		(void)dp_fill_rect(gid, &b_tab[i], ( tab == i ) ? C_SEL : C_BUTTON);
		(void)dp_frame_rect(gid, &b_tab[i], C_TEXT, 1);
		text(b_tab[i].left + 26, 29, tabs[i], C_TEXT);
	}

	if ( tab == T_SAVE && dropped ) {
		snprintf(line, sizeof(line), "対象：置かれた実身（← 戻る）");
	} else if ( tab == T_SAVE ) {
		name_of(&place[nplace - 1], name, sizeof(name));
		snprintf(line, sizeof(line), "対象：%s の中（→ 開く ← 戻る）", name);
	} else if ( state == S_PEEKED || state == S_NEXT ) {
		snprintf(line, sizeof(line), "巻：%s", expect[0] ? expect : "");
	} else {
		snprintf(line, sizeof(line), "書庫の場所：%s（Space で替えます）", media[srcm].label);
	}
	text(16, 66, line, C_TEXT);
	if ( tab == T_SAVE ) {
		snprintf(line, sizeof(line), "● %d　× %d", npick, nexcl);
		text(WORK_W - 16 - dp_text_width((const UB *)line, TEXT_PX), 66, line, C_DIM);
	}

	/* the list */
	r.left = 16;
	r.top = LIST_Y - 4;
	r.right = WORK_W - 16;
	r.bottom = LIST_Y + ROWS * ROW_H;
	(void)dp_frame_rect(gid, &r, C_DIM, 1);
	if ( tab == T_REST && state == S_PEEKED ) {
		for ( i = 0; i < 3; i++ ) text(24, LIST_Y + 16 + i * ROW_H, peekinfo[i], C_TEXT);
	} else {
		if ( sel < top ) top = sel;
		if ( sel >= top + ROWS ) top = sel - ROWS + 1;
		for ( i = 0; i < ROWS && top + i < nitem; i++ ) {
			T_DPRECT row;

			row.left = 17;
			row.top = LIST_Y + i * ROW_H - 3;
			row.right = WORK_W - 17;
			row.bottom = row.top + ROW_H;
			if ( top + i == sel ) (void)dp_fill_rect(gid, &row, C_SEL);
			if ( tab == T_SAVE && pick_at(&items[top + i].uuid) >= 0 ) {
				text(24, LIST_Y + i * ROW_H + 13, "●", C_BAR);
			} else if ( tab == T_SAVE && excl_at(&items[top + i].uuid) >= 0 ) {
				text(24, LIST_Y + i * ROW_H + 13, "×", C_ERR);
			}
			text(44, LIST_Y + i * ROW_H + 13, items[top + i].name, C_TEXT);
		}
		if ( nitem == 0 ) text(24, LIST_Y + 13, ( tab == T_SAVE ) ? "（仮身がありません）" : "（巻がありません）", C_DIM);
	}

	y = LIST_Y + ROWS * ROW_H + 12;
	b_dest.left = 16;
	b_dest.top = y;
	b_dest.right = WORK_W - 16;
	b_dest.bottom = y + 24;
	if ( tab == T_SAVE ) {
		snprintf(line, sizeof(line), "保存先：%s（D で替えます。Space で選ぶ、X で除く）", media[dest].label);
		text(16, y + 17, line, C_TEXT);
	} else if ( state == S_NEXT ) {
		text(16, y + 17, "次の巻を選んで Enter を押してください（Space で場所を替えます）", C_TEXT);
	}
	y += 30;
	button(&b_go, 16, y, 120, ( tab == T_SAVE ) ? "保存" : ( state == S_PEEKED || state == S_NEXT ) ? "復帰" : "読む",
	       (BOOL)!busy);
	button(&b_stop, 152, y, 120, "中止", busy);
	y += 42;
	if ( busy && prog_total > 0 ) {
		r.left = 16;
		r.top = y;
		r.right = WORK_W - 16;
		r.bottom = y + 10;
		(void)dp_frame_rect(gid, &r, C_DIM, 1);
		r.right = 16 + (INT)( (UD)( WORK_W - 32 ) * ( prog_n > prog_total ? prog_total : prog_n ) / prog_total );
		(void)dp_fill_rect(gid, &r, C_BAR);
	}
	text(16, y + 36, status, failed ? C_ERR : C_TEXT);
	(void)wm_obj_flush(kw, NULL);
	dirty = FALSE;
}

/* ---------------------------------------------------------------- while working */

LOCAL BOOL in( const T_DPRECT *b, INT x, INT y )
{
	return (BOOL)( x >= b->left && x < b->right && y >= b->top && y < b->bottom );
}

LOCAL void drop_in( void );

/* What happened to the window while the work goes on: only 中止 and closing count */
LOCAL void poll( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	while ( port > 0 && ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
			closing = TRUE;
			job.stop = TRUE;
		} else if ( m.event == OB_E_KEY && m.code == K_ESC ) {
			job.stop = TRUE;
		} else if ( m.event == OB_E_RELEASE && in(&b_stop, m.x, m.y) ) {
			job.stop = TRUE;
		} else if ( m.event == OB_E_REDRAW ) {
			dirty = TRUE;
		} else if ( m.event == OB_E_DROP ) {
			drop_in();			/* refused while the work goes on */
		}
	}
}

LOCAL UD	told_at;

LOCAL void tell( BKJOB *j, const char *line, UD done, UD total )
{
	UD	now = 0;

	prog_n = done;
	prog_total = total;
	strncpy(status, line, sizeof(status) - 1);
	status[sizeof(status) - 1] = 0;
	poll();
	(void)ts_get_mono(&now);
	if ( gid >= 0 && ( now - told_at > 100000000ULL || done >= total ) ) {
		(void)wm_msg_put(line);
		draw();
		told_at = now;
	}
}

/* ---------------------------------------------------------------- saving */

#define VOL_MIN		0x10000		/* the least room a volume is started with */

typedef struct {
	BKMEDIA	*m;
	BKVOL	v;
	char	names[16][BK_NAME_MAX];	/* the volumes written */
	INT	n;
	TS_UUID	made[64];		/* the volumes made as objects, to take back */
	INT	nmade;
} DEST;

/* Enter pressed (TRUE), or Esc or the window closed (FALSE) */
LOCAL BOOL wait_enter( void )
{
	T_OBNTM	m;
	SZ	asz = 0;

	for ( ;; ) {
		while ( port > 0 && ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
			if ( m.event == OB_E_CLOSE || m.event == OB_E_DELETE ) {
				closing = TRUE;
				return FALSE;
			}
			if ( m.event == OB_E_KEY && ( m.code == K_ENTER || m.code == K_KPENTER ) ) return TRUE;
			if ( m.event == OB_E_KEY && m.code == K_ESC ) return FALSE;
			if ( m.event == OB_E_RELEASE && in(&b_go, m.x, m.y) ) return TRUE;
			if ( m.event == OB_E_RELEASE && in(&b_stop, m.x, m.y) ) return FALSE;
			if ( m.event == OB_E_REDRAW ) draw();
		}
		(void)tk_dly_tsk(50);
	}
}

LOCAL ER dest_open( void *ctx, INT volno, const char *name, BK_OUT *p_out, void **p_arg, UD *p_cap )
{
	DEST	*d = ctx;
	ER	er;

	if ( volno > 1 && d->m->kind == BK_MED_DIR && d->m->dev[0] != 0 && gid >= 0 ) {
		/* a medium of its own for each volume */
		char	s[BK_NAME_MAX + 80];

		bk_media_close(d->m);
		snprintf(s, sizeof(s), "次の媒体を入れて Enter を押してください（「%s」を書きます）", name);
		say(s);
		draw();
		if ( !wait_enter() ) return E_ABORT;
		er = bk_media_open(d->m, TRUE);
		if ( er < E_OK ) return er;
	}
	memset(&d->v, 0, sizeof(d->v));
	if ( d->m->kind == BK_MED_DIR ) strncpy(d->v.dir, d->m->dir, sizeof(d->v.dir) - 1);
	else d->v.into = d->m->into;
	*p_cap = bk_media_free(d->m);
	{
		char	s[80];

		snprintf(s, sizeof(s), "backup: volume %d capacity %llu\n", volno, (unsigned long long)*p_cap);
		tm_putstring((const UB *)s);
	}
	if ( d->m->kind == BK_MED_OBJECT && *p_cap < VOL_MIN ) {
		/* the store the objects go on is full: no volume is begun there */
		snprintf(job.err, sizeof(job.err), "保存先の空きが足りません（%d 巻目）。媒体を選んでください", volno);
		return E_LIMIT;
	}
	er = bk_vol_create(&d->v, name, job.base);
	if ( er < E_OK ) return er;
	if ( d->m->kind == BK_MED_OBJECT && d->nmade < 64 ) d->made[d->nmade++] = d->v.obj;
	if ( d->n < 16 ) strncpy(d->names[d->n++], name, BK_NAME_MAX - 1);
	*p_out = bk_vol_writer(&d->v);
	*p_arg = &d->v;
	return E_OK;
}

/* The volumes a save that failed had made as objects, taken away again */
LOCAL void dest_undo( DEST *d )
{
	TS_UUID	cab;
	static const TS_UUID zero;
	INT	i;

	if ( same(&d->m->into, &zero) ) (void)bk_first_cabinet(&cab);
	else cab = d->m->into;
	for ( i = 0; i < d->nmade; i++ ) {
		(void)bk_link_out(&cab, &d->made[i]);
		(void)ob_del_obj(&d->made[i]);
	}
	d->nmade = 0;
}

LOCAL ER dest_close( void *ctx, INT volno, BOOL more )
{
	DEST	*d = ctx;

	return bk_vol_done(&d->v);
}

LOCAL UD	fixed_cap;

LOCAL ER dest_open_fixed( void *ctx, INT volno, const char *name, BK_OUT *p_out, void **p_arg, UD *p_cap )
{
	ER	er = dest_open(ctx, volno, name, p_out, p_arg, p_cap);

	if ( er >= E_OK && fixed_cap > 0 ) *p_cap = fixed_cap;
	return er;
}

/*
 * The roots and everything linked from them saved, but what is left out;
 * the console told how it went
 */
LOCAL ER save( const BKROOT *roots, INT nroot, const TS_UUID *ex, INT nex, BKMEDIA *m,
	       const char *base, UD cap )
{
	DEST	d;
	BKDEST	bd;
	char	s[300], a[40], b[40];
	ER	er;

	memset(&job, 0, sizeof(job));
	memset(&d, 0, sizeof(d));
	d.m = m;
	job.tell = tell;
	job.excl = ex;
	job.nexcl = nex;
	if ( base != NULL && base[0] != 0 ) strncpy(job.base, base, sizeof(job.base) - 1);
	else name_of(&roots[0].uuid, job.base, sizeof(job.base));
	er = bk_media_open(m, TRUE);
	if ( er < E_OK ) {
		snprintf(job.err, sizeof(job.err), "媒体を使えません (%d)", (INT)er);
		return er;
	}
	er = bk_save_walk(&job, roots, nroot);
	if ( er >= E_OK ) {
		grouped(job.total, a);
		snprintf(s, sizeof(s), "全体：%s バイト（実身数：%d）", a, job.nobj);
		tell(&job, s, 0, (UD)job.nobj);
		fixed_cap = cap;
		bd.open = dest_open_fixed;
		bd.close = dest_close;
		bd.ctx = &d;
		er = bk_save_write(&job, &bd);
	}
	if ( er >= E_OK ) {
		grouped(job.total, a);
		grouped(job.src, b);
		snprintf(s, sizeof(s), "backup: saved %d objects in %d volumes, total %llu, first %s, roots %d\n",
			 job.nobj, job.nvol, (unsigned long long)job.total, d.names[0], job.nroot);
		tm_putstring((const UB *)s);
		snprintf(s, sizeof(s), "%d 個の実身を %d 巻に保存しました（%s）", job.nobj, job.nvol, d.names[0]);
		strncpy(job.err, s, sizeof(job.err) - 1);
	} else if ( d.nmade > 0 ) {
		dest_undo(&d);			/* nothing half saved stays on the store */
	}
	bk_save_free(&job);
	bk_media_close(m);
	return er;
}

/* ---------------------------------------------------------------- restoring */

/* The head of the volume chosen, read to show it */
LOCAL ER peek( void )
{
	static char	names[64][BK_NAME_MAX];
	BK_VOLHEAD	vh;
	INT		n = 0, i, k;
	char		a[40], memo[200];
	ER		er;

	er = bk_vol_open(&vol);
	if ( er < E_OK ) return er;
	er = bk_rest_peek(bk_vol_reader(&vol), &vol, &vh, names, 64, &n);
	bk_vol_rewind(&vol);
	if ( er < E_OK ) return er;
	bpk_tron_utf8(vh.memo, BK_MEMO_TC, memo, sizeof(memo));
	grouped(vh.total, a);
	snprintf(peekinfo[0], sizeof(peekinfo[0]), "メモ：%s", memo);
	snprintf(peekinfo[1], sizeof(peekinfo[1]), "実身数：%u　大きさ：%s バイト　巻：%d%s", vh.nobj, a, vh.vol + 1,
		 vh.more ? "（続きあり）" : "");
	k = snprintf(peekinfo[2], sizeof(peekinfo[2]), "実身：");
	for ( i = 0; i < n && k < (INT)sizeof(peekinfo[2]) - 40; i++ ) {
		k += snprintf(peekinfo[2] + k, sizeof(peekinfo[2]) - (size_t)k, "%s%s", i ? "、" : "", names[i]);
	}
	return E_OK;
}

LOCAL ER open_on_desktop( const TS_UUID *u )
{
	TS_UUID	d, c2;
	T_DTREQ	rq;
	T_DTANS	an;
	T_OBCRE	c;
	SZ	asz = 0;
	ID	k, ka;
	INT	i;
	ER	er;

	if ( ob_fnd_nam((const UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &c2) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&c2, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&c2);
		return E_OBJ;
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_OPEN;
	rq.seq = 1;
	rq.target = *u;
	rq.recno = -1;
	rq.reply = c2;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	for ( i = 0; er >= E_OK && i < 250; i++ ) {
		if ( ob_rea_rec(ka, 0, 0, &an, sizeof(an), &asz) >= E_OK && asz == (SZ)sizeof(an) && an.seq == rq.seq ) {
			er = an.er;
			break;
		}
		(void)tk_dly_tsk(20);
	}
	if ( i >= 250 ) er = E_TMOUT;
	ob_cls_obj(ka);
	(void)ob_del_obj(&c2);
	return er;
}

/*
 * One volume taken in; the set finished when it was the last, each root
 * linked back into what the archive says it was linked from when that
 * is still there, else into the cabinet cab
 */
LOCAL ER restore_volume( const TS_UUID *cab )
{
	char	s[300], label[BK_NAME_MAX], id[40];
	INT	r, nback;
	ER	er;

	/* the name of a later volume must be the one asked for */
	bk_vol_label(&vol, label, sizeof(label));
	if ( job.nvol > 0 && ( bk_vol_number(label) >= 0 || vol.path[0] != 0 ) && strcmp(label, expect) != 0 ) {
		snprintf(job.err, sizeof(job.err), "「%s」を選んでください", expect);
		return E_OBJ;
	}
	if ( job.nvol == 0 && bk_vol_number(label) > 1 ) {
		snprintf(job.err, sizeof(job.err), "これは %d 巻目です。１巻目から選んでください", bk_vol_number(label));
		return E_OBJ;
	}
	er = bk_vol_open(&vol);
	if ( er >= E_OK ) er = bk_rest_volume(&job, bk_vol_reader(&vol), &vol);
	(void)bk_vol_done(&vol);
	if ( er < E_OK ) return er;
	if ( job.more ) {
		char	base[BK_NAME_MAX];

		strncpy(base, ( job.nvol == 1 ) ? label : expect, sizeof(base) - 1);
		bk_vol_name(base, job.nvol + 1, expect, sizeof(expect));
		return E_OK;
	}
	er = bk_rest_end(&job);
	nback = 0;
	for ( r = 0; er >= E_OK && r < job.nrroot; r++ ) {
		/* where the archive says it was linked from, when that is still there; else the cabinet */
		BKROOT		*rt = &job.rroot[r];
		static const TS_UUID zero;
		char		root[BK_NAME_MAX];
		T_OBREF		pr;

		name_of(&rt->uuid, root, sizeof(root));
		if ( !same(&rt->parent, &zero) && ob_ref_obj(&rt->parent, &pr) >= E_OK && pr.type == OB_T_STORAGE
		  && bk_link_at(&rt->parent, &rt->uuid, root, rt->hasbox ? rt->box : NULL) >= E_OK ) {
			nback++;
			continue;
		}
		er = bk_link_into(cab, &rt->uuid, root);
		if ( er < E_OK ) snprintf(job.err, sizeof(job.err), "ルートの仮身を置けません (%d)", (INT)er);
	}
	if ( er >= E_OK ) {
		bk_uuid_str(&job.root, id);
		snprintf(s, sizeof(s), "backup: restored %d objects from %d volumes, %d links, %d lost, %d converted, root %s,"
			 " roots %d back %d\n", job.nmade, job.nvol, job.nlinked, job.nlost, job.nconv, id, job.nrroot, nback);
		tm_putstring((const UB *)s);
	}
	return er;
}

/* ---------------------------------------------------------------- the work without the window */

LOCAL INT batch( const T_BKARG *a )
{
	char	s[300];
	ER	er;

	if ( a->op == BK_OP_SAVE ) {
		static BKROOT	rs[BK_ARG_ROOTS + 1];
		BKMEDIA		m;
		INT		n = 0, i;

		memset(&m, 0, sizeof(m));
		m.kind = ( a->path[0] != 0 ) ? BK_MED_DIR : BK_MED_OBJECT;
		strncpy(m.dir, (const char *)a->path, sizeof(m.dir) - 1);
		m.into = a->into;
		for ( i = 0; i <= a->nroot && i <= BK_ARG_ROOTS; i++ ) {
			static const TS_UUID zero;

			memset(&rs[n], 0, sizeof(rs[n]));
			rs[n].uuid = ( i == 0 ) ? a->obj : a->root[i - 1];
			rs[n].parent = a->parent[i];
			if ( !same(&rs[n].parent, &zero) ) rs[n].hasbox = bk_link_box(&rs[n].parent, &rs[n].uuid, rs[n].box);
			n++;
		}
		er = save(rs, n, a->excl, ( a->nexcl < BK_ARG_ROOTS ) ? a->nexcl : BK_ARG_ROOTS, &m,
			  (const char *)a->base, a->capacity);
	} else {
		TS_UUID	cab = a->obj;
		static const TS_UUID zero;
		char	dir[FS_PATH_MAX], *sl;

		if ( memcmp(&cab, &zero, sizeof(cab)) == 0 ) (void)bk_first_cabinet(&cab);
		memset(&job, 0, sizeof(job));
		job.tell = tell;
		job.near = cab;
		er = bk_rest_begin(&job);
		memset(&vol, 0, sizeof(vol));
		strncpy(vol.path, (const char *)a->path, sizeof(vol.path) - 1);
		strncpy(dir, vol.path, sizeof(dir) - 1);
		dir[sizeof(dir) - 1] = 0;
		sl = strrchr(dir, '/');
		if ( sl != NULL ) *sl = 0;
		while ( er >= E_OK ) {
			er = restore_volume(&cab);
			if ( er < E_OK || !job.more ) break;
			/* the next volume: beside the first, by its name */
			{
				char	file[FS_NAME_MAX * 2];

				bk_vol_file(expect, file, sizeof(file) - 8);
				snprintf(vol.path, sizeof(vol.path), "%s/%s", dir, file);
			}
		}
		if ( er < E_OK ) bk_rest_undo(&job);
		bk_rest_free(&job);
	}
	if ( er < E_OK ) {
		snprintf(s, sizeof(s), "backup: failed %d %s\n", (INT)er, job.err);
		tm_putstring((const UB *)s);
		return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------- the window */

LOCAL void go( void )
{
	char	s[300];
	ER	er;

	if ( tab == T_SAVE ) {
		static BKROOT	one;
		const BKROOT	*rs = pick;
		INT		n = npick;

		if ( n == 0 ) {
			/* nothing marked: the one chosen */
			if ( nitem == 0 ) return;
			memset(&one, 0, sizeof(one));
			one.uuid = items[sel].uuid;
			one.parent = items[sel].parent;
			one.hasbox = items[sel].hasbox;
			memcpy(one.box, items[sel].box, sizeof(one.box));
			rs = &one;
			n = 1;
		}
		state = S_BUSY;
		failed = FALSE;
		dirty = TRUE;
		er = save(rs, n, excl, nexcl, &media[dest], NULL, 0);
		state = S_IDLE;
		if ( er < E_OK ) {
			failed = TRUE;
			snprintf(s, sizeof(s), "保存できませんでした：%s", job.err);
		} else {
			strncpy(s, job.err, sizeof(s) - 1);
			s[sizeof(s) - 1] = 0;
			npick = nexcl = 0;		/* done with */
		}
		say(s);
		return;
	}

	/* 復帰: read the head first, then restore */
	if ( nitem == 0 && state != S_PEEKED ) return;
	if ( state == S_IDLE || state == S_NEXT ) {
		memset(&vol, 0, sizeof(vol));
		if ( items[sel].file[0] != 0 ) snprintf(vol.path, sizeof(vol.path), "%s/%s", media[srcm].dir, items[sel].file);
		else vol.obj = items[sel].uuid;
		if ( state == S_IDLE ) {
			er = peek();
			if ( er < E_OK ) {
				failed = TRUE;
				snprintf(s, sizeof(s), "バックアップの巻ではありません (%d)", (INT)er);
				say(s);
				return;
			}
			bk_vol_label(&vol, expect, sizeof(expect));
			state = S_PEEKED;
			failed = FALSE;
			say("Enter で復帰します");
			return;
		}
	}
	/* S_PEEKED (the first volume) or S_NEXT (a later one chosen) */
	{
		TS_UUID	cab;

		(void)bk_first_cabinet(&cab);
		if ( state == S_PEEKED ) {		/* the first volume: a new job */
			memset(&job, 0, sizeof(job));
			job.tell = tell;
			job.near = cab;
			if ( bk_rest_begin(&job) < E_OK ) {
				say("復帰を始められません");
				return;
			}
		}
		state = S_BUSY;
		failed = FALSE;
		dirty = TRUE;
		er = restore_volume(&cab);
		if ( er >= E_OK && job.more ) {
			state = S_NEXT;
			snprintf(s, sizeof(s), "次の巻「%s」を選んでください", expect);
			say(s);
			list_again();
			return;
		}
		if ( er < E_OK && job.nvol > 0 && ( er == E_OBJ && strstr(job.err, "選んで") != NULL ) ) {
			/* the wrong volume: ask again */
			state = S_NEXT;
			failed = TRUE;
			say(job.err);
			return;
		}
		if ( er < E_OK ) {
			bk_rest_undo(&job);
			failed = TRUE;
			snprintf(s, sizeof(s), "復帰できませんでした：%s", job.err);
		} else {
			snprintf(s, sizeof(s), "%d 個の実身を復帰しました", job.nmade);
		}
		bk_rest_free(&job);
		job.nvol = 0;
		state = S_IDLE;
		say(s);
		if ( er >= E_OK ) (void)open_on_desktop(&job.root);
		list_again();
	}
}

LOCAL void cycle_media( INT *which, BOOL restore )
{
	BKMEDIA	*m = &media[*which];
	char	s[80];

	bk_media_close(m);
	*which = ( *which + 1 ) % nmedia;
	if ( restore ) {
		if ( state == S_PEEKED ) state = S_IDLE;
		list_volumes();
	}
	dirty = TRUE;
	snprintf(s, sizeof(s), "backup: %s %s\n", restore ? "from" : "to", media[*which].label);
	tm_putstring((const UB *)s);
}

/* The place the sheet shown works with: where to save, or where the volumes are */
LOCAL void next_place( void )
{
	if ( tab == T_SAVE ) cycle_media(&dest, FALSE);
	else cycle_media(&srcm, TRUE);
}

LOCAL void key( const T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);

	if ( ( m->mods & M_CTRL ) && ( c == 'e' || c == 'E' ) ) {
		closing = TRUE;
		return;
	}
	switch ( m->code ) {
	case K_TAB:
		if ( state == S_BUSY || state == S_NEXT ) break;
		tab = ( tab == T_SAVE ) ? T_REST : T_SAVE;
		state = S_IDLE;
		list_again();
		break;
	case K_UP:
		if ( sel > 0 ) sel--;
		dirty = TRUE;
		break;
	case K_DOWN:
		if ( sel + 1 < nitem ) sel++;
		dirty = TRUE;
		break;
	case K_RIGHT:
		if ( tab == T_SAVE && !dropped && nitem > 0 && nplace < DEPTH_MAX ) {
			place[nplace++] = items[sel].uuid;
			list_again();
		}
		break;
	case K_LEFT:
		if ( tab == T_SAVE && dropped ) {
			drop_let_go();
			list_again();
		} else if ( tab == T_SAVE && nplace > 1 ) {
			nplace--;
			list_again();
		}
		if ( tab == T_REST && state == S_PEEKED ) {
			state = S_IDLE;
			dirty = TRUE;
		}
		break;
	case K_SPACE:
		if ( tab == T_SAVE ) pick_toggle();
		else cycle_media(&srcm, TRUE);
		break;
	case K_ENTER:
	case K_KPENTER:
		go();
		break;
	default:
		if ( ( c == 'x' || c == 'X' ) && tab == T_SAVE && state != S_BUSY ) {
			excl_toggle();
		} else if ( ( c == 'd' || c == 'D' ) && state != S_BUSY ) {
			next_place();
		}
		break;
	}
}

/*
 * The window's menu: the sheets, the marks on the one chosen and the
 * place (what Space, X and D do), and closing
 */
LOCAL void menu( const T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	msel;
	ID	mid;
	BOOL	chosen, marking;

	if ( !bk_uuid_parse(SYSDEF_MENU_BACKUP, &def) || mn_cre_men(&def, &mid) < E_OK ) return;
	if ( state == S_NEXT ) {
		(void)mn_chg_atr(mid, NULL, "save", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "restore", MN_GREY);
	} else {
		(void)mn_chg_atr(mid, NULL, "stop", MN_GREY);
	}
	marking = (BOOL)( tab == T_SAVE && state != S_BUSY && sel < nitem );
	if ( !marking ) {
		(void)mn_chg_atr(mid, NULL, "pick", MN_GREY);
		(void)mn_chg_atr(mid, NULL, "exclude", MN_GREY);
	} else if ( pick_at(&items[sel].uuid) >= 0 ) {
		(void)mn_chg_atr(mid, NULL, "pick", MN_TICK);
	} else if ( excl_at(&items[sel].uuid) >= 0 ) {
		(void)mn_chg_atr(mid, NULL, "exclude", MN_TICK);
	}
	(void)mn_chg_atr(mid, NULL, ( tab == T_SAVE ) ? "source" : "dest", MN_HIDE);
	if ( state == S_BUSY ) {
		(void)mn_chg_atr(mid, NULL, ( tab == T_SAVE ) ? "dest" : "source", MN_GREY);
	}
	chosen = (BOOL)( mn_pop_men(mid, kw, m->x, m->y, m->when, &msel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( strcmp((const char *)msel.code, "close") == 0 ) {
		closing = TRUE;
	} else if ( strcmp((const char *)msel.code, "pick") == 0 ) {
		if ( tab == T_SAVE && state != S_BUSY ) pick_toggle();
	} else if ( strcmp((const char *)msel.code, "exclude") == 0 ) {
		if ( tab == T_SAVE && state != S_BUSY ) excl_toggle();
	} else if ( strcmp((const char *)msel.code, "dest") == 0 || strcmp((const char *)msel.code, "source") == 0 ) {
		if ( state != S_BUSY ) next_place();
	} else if ( strcmp((const char *)msel.code, "save") == 0 || strcmp((const char *)msel.code, "restore") == 0 ) {
		tab = ( msel.code[0] == 's' ) ? T_SAVE : T_REST;
		state = S_IDLE;
		list_again();
	} else if ( strcmp((const char *)msel.code, "stop") == 0 ) {
		/* a set of volumes given up half way: what was made goes */
		bk_rest_undo(&job);
		bk_rest_free(&job);
		job.nvol = 0;
		state = S_IDLE;
		failed = TRUE;
		say("復帰を中止しました");
		list_again();
	}
}

/* A drop answered: taken or not, and what the one who dropped it is told */
LOCAL void drop_answer( const T_OBDROP *d, UINT answer, const char *msg )
{
	T_OBDRANS	a;
	SZ		asz = 0;
	char		s[160];

	memset(&a, 0, sizeof(a));
	a.seq = d->seq;
	a.answer = answer;
	strncpy((char *)a.msg, msg, sizeof(a.msg) - 1);
	(void)ob_wri_rec(kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
	snprintf(s, sizeof(s), "backup: drop %s\n", ( answer == OB_DR_ACCEPT ) ? "taken" : "refused");
	tm_putstring((const UB *)s);
}

/*
 * Links dropped on the window: the objects they point at become the
 * list of the 保存 sheet, to be saved from; ← goes back to the
 * cabinets.
 */
LOCAL void drop_in( void )
{
	static T_OBDROP	d;
	T_OBREF		r;
	SZ		asz = 0;
	INT		i, n = 0;
	char		s[160];

	memset(&d, 0, sizeof(d));
	if ( ob_rea_rec(kw, OB_WR_DROP, 0, &d, sizeof(d), &asz) < E_OK || d.n <= 0 ) return;
	if ( state == S_BUSY || state == S_NEXT ) {
		drop_answer(&d, OB_DR_REFUSE, "バックアップは作業中です");
		return;
	}
	drop_let_go();
	for ( i = 0; i < d.n && i < OB_DROP_MAX && n < ITEM_MAX; i++ ) {
		if ( ob_ref_obj(&d.v[i].target, &r) < E_OK || r.type != OB_T_STORAGE ) continue;
		memset(&items[n], 0, sizeof(items[n]));
		items[n].uuid = d.v[i].target;
		name_of(&items[n].uuid, items[n].name, sizeof(items[n].name));
		if ( items[n].name[0] == 0 ) strncpy(items[n].name, (const char *)d.v[i].name, sizeof(items[n].name) - 1);
		items[n].parent = d.fromobj;		/* the record the links were in */
		items[n].box[0] = d.v[i].left;
		items[n].box[1] = d.v[i].top;
		items[n].box[2] = d.v[i].right;
		items[n].box[3] = d.v[i].bottom;
		items[n].hasbox = (BOOL)( d.v[i].right > d.v[i].left && d.v[i].bottom > d.v[i].top );
		if ( ( d.mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 ) (void)excl_add(&items[n].uuid);
		else (void)pick_add(&items[n]);
		n++;
	}
	if ( n == 0 ) {
		drop_answer(&d, OB_DR_REFUSE, "保存できる実身がありません");
		list_again();
		return;
	}
	drop_answer(&d, OB_DR_ACCEPT, "置かれた実身を保存の対象にしました");
	dropped = TRUE;
	tab = T_SAVE;
	state = S_IDLE;
	nitem = n;
	sel = top = 0;
	snprintf(s, sizeof(s), "backup: dropped %d picked %d left out %d\n", n, npick, nexcl);
	tm_putstring((const UB *)s);
	say("置かれた実身を Enter で保存します（Space で選び直し、X で除く）");
}

LOCAL void event( const T_OBNTM *m )
{
	INT	i;

	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) menu(m);
		break;
	case OB_E_RELEASE:
		if ( m->code == 1 ) break;
		for ( i = 0; i < 2; i++ ) {
			if ( in(&b_tab[i], m->x, m->y) && tab != i && state != S_BUSY && state != S_NEXT ) {
				tab = i;
				state = S_IDLE;
				list_again();
				return;
			}
		}
		if ( in(&b_go, m->x, m->y) ) {
			go();
		} else if ( in(&b_dest, m->x, m->y) ) {
			next_place();
		} else if ( m->y >= LIST_Y - 4 && m->y < LIST_Y + ROWS * ROW_H ) {
			i = top + ( m->y - LIST_Y + 3 ) / ROW_H;
			if ( i >= 0 && i < nitem ) {
				sel = i;
				dirty = TRUE;
			}
		}
		break;
	case OB_E_KEY:
		key(m);
		break;
	case OB_E_REDRAW:
		dirty = TRUE;
		break;
	case OB_E_DROP:
		drop_in();
		break;
	default:
		break;
	}
}

LOCAL ER open_window( void )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[192];

	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":7}",
		 160, 120, 160 + WORK_W + FRAME_W, 120 + WORK_H + TITLE_H);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (const UB *)"バックアップ";
	c.json = (const UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &win) < E_OK ) return E_OBJ;
	kw = ob_opn_obj(&win, OB_OP_ALL);
	gid = ( kw > 0 ) ? wm_obj_gid(kw) : E_ID;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	port = ( ob_cre_obj(&c, &ch) >= E_OK ) ? ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_PRESS | OB_E_RELEASE | OB_E_KEY | OB_E_REDRAW | OB_E_DELETE | OB_E_DROP;
	if ( kw <= 0 || gid < 0 || port <= 0 || ob_ntf_evt(kw, OB_REC_ANY, &req, port) <= 0 ) return E_OBJ;
	if ( ob_rea_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + WORK_W + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + WORK_H + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	return E_OK;
}

LOCAL void close_window( void )
{
	if ( port > 0 ) {
		ob_cls_obj(port);
		(void)ob_del_obj(&ch);
	}
	if ( kw > 0 ) ob_cls_obj(kw);
	(void)ob_del_obj(&win);
	port = kw = 0;
	gid = -1;
}

int main( void )
{
	static T_BKARG	a;
	T_OBNTM	m;
	SZ	asz = 0, n;
	INT	i;

	n = ts_get_arg(&a, sizeof(a));
	if ( n == (SZ)sizeof(a) && a.magic == BK_ARG_MAGIC ) return batch(&a);

	nmedia = bk_media_list(media, BK_MED_MAX);
	(void)bk_first_cabinet(&place[0]);
	nplace = 1;
	if ( n >= (SZ)sizeof(TS_UUID) ) {
		/* started on an object: a volume is restored from, anything else saved */
		BKVOL	v;

		memcpy(&argobj, &a, sizeof(argobj));
		memset(&v, 0, sizeof(v));
		v.obj = argobj;
		if ( bk_vol_open(&v) >= E_OK ) {
			(void)bk_vol_done(&v);
			argvol = TRUE;
			tab = T_REST;
			srcm = 0;
		} else {
			place[nplace++] = argobj;
		}
	}
	for ( i = 0; i < nmedia; i++ ) {
		if ( media[i].kind == BK_MED_DIR && media[i].dev[0] == 0 ) dest = i;	/* /boot until one is chosen */
	}
	if ( tab == T_SAVE ) srcm = dest;	/* volumes are looked for where they are written */
	if ( open_window() < E_OK ) {
		tm_putstring((const UB *)"backup: no window\n");
		close_window();
		return 3;
	}
	list_again();
	{
		char	s[80];

		snprintf(s, sizeof(s), "backup: ready, %d listed\n", nitem);
		tm_putstring((const UB *)s);
	}
	say(( tab == T_SAVE ) ? "保存する実身を選んで Enter を押してください" : "復帰する巻を選んで Enter を押してください");
	draw();

	while ( !closing ) {
		while ( !closing && ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) event(&m);
		if ( dirty && !closing ) draw();
		if ( !closing ) (void)tk_dly_tsk(20);
	}
	if ( state == S_NEXT ) {
		bk_rest_undo(&job);
		bk_rest_free(&job);
	}
	for ( i = 0; i < nmedia; i++ ) bk_media_close(&media[i]);
	drop_let_go();
	close_window();
	(void)wm_msg_put(NULL);
	return 0;
}
