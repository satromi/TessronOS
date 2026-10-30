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
 *	Its window is a panel of the data box (SYSDEF_BOX_BACKUP) with two
 *	sheets under their tags, 保存 and 復帰; every part is the panel's,
 *	and the program only says what they hold. 保存 lists, in a list part,
 *	what a cabinet links to, starting at the first cabinet (or the object
 *	it was started on): 〈開く〉 (or →) goes into the one chosen, 〈戻る〉
 *	(or ←) back out; 〈選ぶ〉 (or Space) marks one to be saved (●),
 *	several may be, in any cabinet gone into, and 〈除く〉 (or X) marks one
 *	to be left out (×) with what only it links to. The pop-up 保存先
 *	chooses where the volumes go -- objects of their own, or the disks
 *	the system has, each by its device object's name, opened while it is
 *	used. 〈保存〉 (or Enter) saves what is marked, or else the one chosen,
 *	and all they link to; each is saved with where it was linked from,
 *	for a restore to put it back there. Volumes kept as objects take no
 *	more than the store they go on has free. 復帰 lists the volumes the
 *	place its pop-up chooses has; 〈復帰〉 (or Enter) reads the chosen
 *	one's head and shows it, and again makes the objects and links the
 *	first of them into the first cabinet, and the desktop opens it. A
 *	set of several volumes asks for each next one, and takes only the one
 *	it wants. While the work goes on, 〈中止〉 stops it; what a restore had
 *	made is then taken away again. A set saved over several media asks,
 *	in a panel, for each next medium.
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
#include <ts/obdrop.h>
#include <ts/obatr.h>
#include <ts/dtreq.h>
#include <ts/json.h>
#include <ts/hid.h>
#include <ts/ui.h>

/* The panels of the data box, and their parts */
#define PN_MAIN		1
#define PN_MEDIUM	2		/* the next medium asked for */
#define PT_TAGS		100
#define PT_WHAT		21		/* 保存: what is listed */
#define PT_MARKS	22
#define PT_ITEMS	23
#define PT_OPEN		24
#define PT_BACK		25
#define PT_PICK		26
#define PT_EXCL		27
#define PT_DEST		28
#define PT_SAVE		29
#define PT_SAVE_STOP	30
#define PT_SRC		41		/* 復帰: where the volumes are */
#define PT_VOLS		42
#define PT_HEAD		43		/* 43..45: the volume's head */
#define PT_REST		46
#define PT_REST_STOP	47
#define PT_STATUS	90
#define PT_PROGRESS	91
#define PT_GO_ON	11		/* PN_MEDIUM: 〈続ける〉 */

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
LOCAL UI	ui;			/* the window: the panel and its channel */
LOCAL BOOL	shown;		/* the window is up */
LOCAL INT	tab = T_SAVE, state = S_IDLE;
LOCAL BOOL	closing, dirty = TRUE, listed = TRUE, failed;
LOCAL char	status[256];
LOCAL UD	prog_n, prog_total;

LOCAL BKMEDIA	*media = NULL;		/* every place there is (bk_media_list) */
LOCAL INT	nmedia, dest, srcm;	/* where saving goes, where restoring reads */

LOCAL ITEM	*items = NULL;		/* the list, as many lines as it has */
LOCAL INT	nitem, sel, item_cap = 0;

/* Room for n lines of the list: FALSE for no memory */
LOCAL BOOL item_room( INT n )
{
	ITEM	*t;
	INT	cap;

	if ( n <= item_cap ) return TRUE;
	cap = ( item_cap > 0 ) ? item_cap : 64;
	while ( cap < n ) cap *= 2;
	t = (ITEM *)realloc(items, sizeof(ITEM) * (size_t)cap);
	if ( t == NULL ) return FALSE;
	items = t;
	item_cap = cap;
	return TRUE;
}
LOCAL TS_UUID	*place = NULL;		/* the cabinets gone into, as deep as one goes */
LOCAL INT	nplace, place_cap = 0;
LOCAL BOOL	dropped;		/* the list is what was dropped on the window */

LOCAL BKROOT	*pick = NULL;		/* marked to be saved, as many as are marked */
LOCAL INT	npick, pick_cap = 0;
LOCAL TS_UUID	*excl = NULL;		/* marked to be left out */
LOCAL INT	nexcl, excl_cap = 0;

/* Room for n entries of esz bytes in *p: FALSE for no memory */
LOCAL BOOL room_for( void **p, INT *p_cap, INT n, size_t esz )
{
	void	*t;
	INT	cap = ( *p_cap > 0 ) ? *p_cap : 16;

	if ( n <= *p_cap ) return TRUE;
	while ( cap < n ) cap *= 2;
	t = realloc(*p, esz * (size_t)cap);
	if ( t == NULL ) return FALSE;
	*p = t;
	*p_cap = cap;
	return TRUE;
}

/* The cabinet gone into at the end of the way: FALSE for no memory */
LOCAL BOOL place_push( const TS_UUID *u )
{
	if ( !room_for((void **)&place, &place_cap, nplace + 1, sizeof(TS_UUID)) ) return FALSE;
	place[nplace++] = *u;
	return TRUE;
}

LOCAL BKVOL	vol;			/* the volume being read */
LOCAL char	peekinfo[3][160];
LOCAL char	expect[BK_NAME_MAX];	/* the name the next volume must have */

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

/* A volume's file on a medium: below the root of its disk, or at the boot disk's place */
LOCAL void bk_vol_at( BKVOL *v, const BKMEDIA *m, const char *file )
{
	if ( m->mounted ) {
		v->ondisk = TRUE;
		v->vd = m->vd;
		snprintf(v->path, sizeof(v->path), "%s", file);
	} else {
		snprintf(v->path, sizeof(v->path), "%s/%s", m->dir, file);
	}
}

/* The name of an object, from its metadata or else its reference */
LOCAL void name_of( const TS_UUID *u, char *out, INT max )
{
	UB	*meta = NULL;
	T_JSON	root;
	INT	asz;
	T_OBREF	r;
	ID	k = ob_opn_obj(u, OB_OP_ATRRD);

	out[0] = 0;
	if ( k > 0 ) {
		asz = ob_atr_read(k, 0, &meta);
		if ( asz >= 0 && js_parse(meta, asz, &root) >= E_OK ) {
			(void)js_get_str(&root, "name", (UB *)out, max);
		}
		free(meta);
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

	nitem = sel = 0;
	listed = TRUE;
	k = ob_opn_obj(cab, OB_OP_R);
	if ( k <= 0 ) return;
	x = bk_read_rec(k, 0, &len);
	ob_cls_obj(k);
	if ( x == NULL ) return;
	for ( p = (const char *)x; item_room(nitem + 1) && ( p = strstr(p, "<link") ) != NULL; p++ ) {
		const char *id = strstr(p, " id=\"");
		const char *end = strchr(p, '>');
		ITEM	*it = &items[nitem];
		T_OBREF	r;
		INT	i;
		BOOL	dup = FALSE;

		if ( id == NULL || end == NULL || id > end ) continue;
		id += ts_lnk_split((const UB *)id + 5, (INT)( end - id - 5 ), NULL, NULL);	/* past a volume it names */
		if ( !bk_uuid_parse(id + 5, &it->uuid) ) continue;
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
	if ( !room_for((void **)&pick, &pick_cap, npick + 1, sizeof(BKROOT)) ) return FALSE;
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
	if ( !room_for((void **)&excl, &excl_cap, nexcl + 1, sizeof(TS_UUID)) ) return FALSE;
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
	dirty = listed = TRUE;
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
	dirty = listed = TRUE;
	marks_said();
}

/* The volumes a place has: files on a medium, or objects linked from the first cabinet */
LOCAL void list_volumes( void )
{
	BKMEDIA	*m = &media[srcm];

	nitem = sel = 0;
	listed = TRUE;
	if ( m->kind == BK_MED_OBJECT ) {
		ITEM	*keep;
		TS_UUID	cab;
		BKVOL	v;
		INT	n = 0, i;

		(void)bk_first_cabinet(&cab);
		list_objects(&cab);
		keep = (ITEM *)calloc((size_t)( nitem + 1 ), sizeof(ITEM));
		if ( keep == NULL || !item_room(nitem + 1) ) {
			free(keep);
			nitem = 0;
			return;
		}
		if ( argvol ) {
			/* the volume it was started on comes first */
			keep[0].uuid = argobj;
			name_of(&argobj, keep[0].name, sizeof(keep[0].name));
			n = 1;
		}
		/* only those that hold a volume */
		for ( i = 0; i < nitem; i++ ) {
			if ( argvol && memcmp(&items[i].uuid, &argobj, sizeof(TS_UUID)) == 0 ) continue;
			memset(&v, 0, sizeof(v));
			v.obj = items[i].uuid;
			if ( bk_vol_open(&v) < E_OK ) continue;
			(void)bk_vol_done(&v);
			keep[n++] = items[i];
		}
		memcpy(items, keep, sizeof(ITEM) * (size_t)n);
		free(keep);
		nitem = n;
		sel = 0;
		return;
	}
	if ( bk_media_open(m, FALSE) < E_OK ) {
		say("媒体を読めません");
		return;
	}
	{
		char	(*names)[FS_NAME_MAX];
		INT	n = bk_media_files(m, NULL, 0), i, k = 0;
		BKVOL	v;

		/* every file of the medium: asked how many, then taken */
		names = calloc((size_t)( n + 1 ), sizeof(*names));
		if ( names == NULL || !item_room(n + 1) ) {
			free(names);
			nitem = 0;
			return;
		}
		i = bk_media_files(m, names, n);
		if ( i < n ) n = i;		/* fewer now; more came meanwhile are left for the next look */

		/* only files that begin as a volume does */
		for ( i = 0; i < n; i++ ) {
			memset(&v, 0, sizeof(v));
			bk_vol_at(&v, m, names[i]);
			if ( bk_vol_open(&v) < E_OK ) continue;
			(void)bk_vol_done(&v);
			memset(&items[k], 0, sizeof(items[k]));
			strncpy(items[k].file, names[i], sizeof(items[k].file) - 1);
			(void)xfu_name_in((const UB *)names[i], (UB *)items[k].name, sizeof(items[k].name));
			k++;
		}
		free(names);
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
		dirty = listed = TRUE;
		return;				/* what was dropped stays listed */
	}
	if ( dropped ) drop_let_go();		/* the other sheet lists volumes instead */
	if ( tab == T_SAVE ) list_objects(&place[nplace - 1]);
	else list_volumes();
	dirty = listed = TRUE;
}

/* ---------------------------------------------------------------- what the panel shows */

/* The places, as the pop-ups list them */
LOCAL void places_named( INT num, INT which )
{
	const char	**names = (const char **)calloc((size_t)( nmedia + 1 ), sizeof(char *));
	INT		i;

	if ( names == NULL ) return;
	for ( i = 0; i < nmedia; i++ ) names[i] = media[i].label;
	ui_names(&ui, num, names, nmedia);
	ui_set_value(&ui, num, which + 1);
	free(names);
}

/* The list of the sheet shown: each line its mark, a tab and its name */
LOCAL void list_shown( void )
{
	char	**names = (char **)calloc((size_t)( nitem + 1 ), sizeof(char *));
	INT	i;

	if ( names == NULL ) return;
	for ( i = 0; i < nitem; i++ ) {
		const char	*mark = ( tab != T_SAVE ) ? "" : ( pick_at(&items[i].uuid) >= 0 ) ? "●"
				      : ( excl_at(&items[i].uuid) >= 0 ) ? "×" : "";
		size_t		n = strlen(items[i].name) + 8;

		names[i] = (char *)malloc(n);
		if ( names[i] != NULL ) snprintf(names[i], n, "%s\t%s", mark, items[i].name);
	}
	if ( nitem == 0 ) {
		static const char *const none[2] = { "\t（仮身がありません）", "\t（巻がありません）" };

		ui_names(&ui, ( tab == T_SAVE ) ? PT_ITEMS : PT_VOLS, &none[( tab == T_SAVE ) ? 0 : 1], 1);
	} else {
		ui_names(&ui, ( tab == T_SAVE ) ? PT_ITEMS : PT_VOLS, (const char *const *)names, nitem);
	}
	ui_set_value(&ui, ( tab == T_SAVE ) ? PT_ITEMS : PT_VOLS, ( nitem > 0 ) ? sel + 1 : 0);
	for ( i = 0; i < nitem; i++ ) free(names[i]);
	free(names);
}

LOCAL void draw( void )
{
	char		line[300], name[BK_NAME_MAX];
	BOOL		busy = ( state == S_BUSY );
	INT		i;

	if ( !shown ) return;
	if ( ui_sheet(&ui) != ( ( tab == T_SAVE ) ? 1 : 2 ) ) ui_show_sheet(&ui, ( tab == T_SAVE ) ? 1 : 2);
	if ( tab == T_SAVE ) {
		if ( dropped ) {
			snprintf(line, sizeof(line), "対象：置かれた実身");
		} else {
			name_of(&place[nplace - 1], name, sizeof(name));
			snprintf(line, sizeof(line), "対象：%s の中", name);
		}
		ui_set_text(&ui, PT_WHAT, line);
		snprintf(line, sizeof(line), "● %d　× %d", npick, nexcl);
		ui_set_text(&ui, PT_MARKS, line);
		ui_off(&ui, PT_OPEN, (BOOL)( busy || dropped || nitem == 0 ));
		ui_off(&ui, PT_BACK, (BOOL)( busy || ( !dropped && nplace <= 1 ) ));
		ui_off(&ui, PT_PICK, (BOOL)( busy || nitem == 0 ));
		ui_off(&ui, PT_EXCL, (BOOL)( busy || nitem == 0 ));
		ui_off(&ui, PT_DEST, busy);
		ui_off(&ui, PT_SAVE, (BOOL)( busy || ( nitem == 0 && npick == 0 ) ));
		ui_off(&ui, PT_SAVE_STOP, (BOOL)!busy);
	} else {
		for ( i = 0; i < 3; i++ ) {
			ui_set_text(&ui, PT_HEAD + i, ( state == S_PEEKED ) ? peekinfo[i] : " ");
		}
		if ( state == S_NEXT ) {
			snprintf(line, sizeof(line), "次の巻「%s」を選んで〈復帰〉を押してください", expect);
			ui_set_text(&ui, PT_HEAD, line);
		}
		ui_off(&ui, PT_SRC, (BOOL)( busy || state == S_PEEKED ));
		ui_off(&ui, PT_VOLS, busy);
		ui_off(&ui, PT_REST, (BOOL)( busy || ( nitem == 0 && state != S_PEEKED ) ));
		ui_off(&ui, PT_REST_STOP, (BOOL)!busy);
	}
	if ( listed ) {
		list_shown();
		listed = FALSE;
	}
	ui_set_text(&ui, PT_STATUS, status[0] != 0 ? status : " ");
	if ( busy && prog_total > 0 ) {
		UD	n = ( prog_n > prog_total ) ? prog_total : prog_n;

		snprintf(line, sizeof(line), "進み具合：%llu%%", (unsigned long long)( n * 100 / prog_total ));
		ui_set_text(&ui, PT_PROGRESS, line);
	} else {
		ui_set_text(&ui, PT_PROGRESS, " ");
	}
	dirty = FALSE;
}

LOCAL void drop_in( void );

/* What happened to the window while the work goes on: only 中止 and closing count */
LOCAL void poll( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	num = 0, ev;

	while ( shown && ob_rea_rec(ui.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
		ev = ui_event(&ui, &m, &num);
		if ( ev == UI_EV_CLOSE ) {
			closing = TRUE;
			job.stop = TRUE;
		} else if ( ev == UI_EV_KEY && m.code == K_ESC ) {
			job.stop = TRUE;
		} else if ( ev == UI_EV_BUTTON && ( num == PT_SAVE_STOP || num == PT_REST_STOP ) ) {
			job.stop = TRUE;
		} else if ( ev == UI_EV_DROP ) {
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
	if ( shown && ( now - told_at > 100000000ULL || done >= total ) ) {
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

/* The next medium asked for in a panel: 〈続ける〉 (TRUE), or 〈中止〉, Esc or the panel closed (FALSE) */
LOCAL BOOL ask_medium( const char *name )
{
	char		l[2][BK_NAME_MAX + 80];
	const char	*lines[2];

	snprintf(l[0], sizeof(l[0]), "次の媒体を入れて〈続ける〉を押してください。");
	snprintf(l[1], sizeof(l[1]), "書く巻：%s", name);
	lines[0] = l[0];
	lines[1] = l[1];
	tm_putstring((const UB *)"backup: asked for the next medium\n");
	return (BOOL)( ui_ask(&ui, PN_MEDIUM, lines, 2) == PT_GO_ON );
}

LOCAL ER dest_open( void *ctx, INT volno, const char *name, BK_OUT *p_out, void **p_arg, UD *p_cap )
{
	DEST	*d = ctx;
	ER	er;

	if ( volno > 1 && d->m->kind == BK_MED_DIR && d->m->dev[0] != 0 && shown ) {
		/* a medium of its own for each volume */
		bk_media_close(d->m);
		if ( !ask_medium(name) ) return E_ABORT;
		er = bk_media_open(d->m, TRUE);
		if ( er < E_OK ) return er;
	}
	memset(&d->v, 0, sizeof(d->v));
	if ( d->m->kind == BK_MED_DIR && d->m->mounted ) {
		d->v.ondisk = TRUE;			/* written at the disk's root */
		d->v.vd = d->m->vd;
	} else if ( d->m->kind == BK_MED_DIR ) {
		strncpy(d->v.dir, d->m->dir, sizeof(d->v.dir) - 1);
	}
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
		if ( k >= (INT)sizeof(peekinfo[2]) ) k = (INT)sizeof(peekinfo[2]) - 1;
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
	/* the answer waited for on its channel, at most 5000 ms */
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, &an, sizeof(an), &asz, 5000);
		if ( er >= E_OK && asz == (SZ)sizeof(an) && an.seq == rq.seq ) {
			er = an.er;
			break;
		}
	}
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
		BKROOT		*rs = calloc((size_t)a->nroot + 1, sizeof(BKROOT));
		BKMEDIA		m;
		INT		n = 0, i;

		if ( rs == NULL ) {
			tm_putstring((const UB *)"backup: failed no memory\n");
			return 1;
		}
		memset(&m, 0, sizeof(m));
		m.kind = ( a->path[0] != 0 ) ? BK_MED_DIR : BK_MED_OBJECT;
		strncpy(m.dir, (const char *)a->path, sizeof(m.dir) - 1);
		m.into = a->into;
		for ( i = 0; i <= a->nroot; i++ ) {
			static const TS_UUID zero;

			memset(&rs[n], 0, sizeof(rs[n]));
			rs[n].uuid = ( i == 0 ) ? a->obj : BK_ARG_ROOT(a)[i - 1];
			rs[n].parent = ( i == 0 ) ? a->objparent : BK_ARG_PARENT(a)[i - 1];
			if ( !same(&rs[n].parent, &zero) ) rs[n].hasbox = bk_link_box(&rs[n].parent, &rs[n].uuid, rs[n].box);
			n++;
		}
		er = save(rs, n, BK_ARG_EXCL(a), a->nexcl, &m, (const char *)a->base, a->capacity);
		free(rs);
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
		if ( items[sel].file[0] != 0 ) bk_vol_at(&vol, &media[srcm], items[sel].file);
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
			say("〈復帰〉で復帰します");
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
	if ( shown ) ui_set_value(&ui, restore ? PT_SRC : PT_DEST, *which + 1);
	snprintf(s, sizeof(s), "backup: %s %s\n", restore ? "from" : "to", media[*which].label);
	tm_putstring((const UB *)s);
}

/* The place the sheet shown works with: where to save, or where the volumes are */
LOCAL void next_place( void )
{
	if ( tab == T_SAVE ) cycle_media(&dest, FALSE);
	else cycle_media(&srcm, TRUE);
}

/* The sheet shown changed: to the other's work, unless the work forbids it */
LOCAL void sheet_to( INT t )
{
	if ( t == tab ) return;
	if ( state == S_BUSY || state == S_NEXT ) {
		dirty = TRUE;			/* the tags put back */
		return;
	}
	tab = t;
	state = S_IDLE;
	list_again();
}

/* Into the one chosen, and back out */
LOCAL void go_in( void )
{
	if ( tab == T_SAVE && !dropped && nitem > 0 && state != S_BUSY && place_push(&items[sel].uuid) ) {
		list_again();
	}
}

LOCAL void go_out( void )
{
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
}

/* A key no part took: the list's commands, as the buttons have them */
LOCAL void key( const T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);

	if ( ( m->mods & M_CTRL ) && ( c == 'e' || c == 'E' ) ) {
		closing = TRUE;
		return;
	}
	switch ( m->code ) {
	case K_RIGHT:
		go_in();
		break;
	case K_LEFT:
		go_out();
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

/* A part worked: a button pushed, or what a list or a pop-up chose */
LOCAL void part( INT ev, INT num )
{
	INT	v;

	if ( ev == UI_EV_BUTTON ) {
		switch ( num ) {
		case PT_OPEN:	go_in();						break;
		case PT_BACK:	go_out();						break;
		case PT_PICK:	if ( state != S_BUSY ) pick_toggle();			break;
		case PT_EXCL:	if ( state != S_BUSY ) excl_toggle();			break;
		case PT_SAVE:
		case PT_REST:	go();							break;
		default:								break;
		}
		return;
	}
	v = ui_value(&ui, num) - 1;
	if ( ( num == PT_ITEMS || num == PT_VOLS ) && v >= 0 && v < nitem ) {
		sel = v;
		if ( tab == T_REST && state == S_PEEKED ) {
			state = S_IDLE;
			dirty = TRUE;
		}
	} else if ( num == PT_DEST && v >= 0 && v < nmedia && v != dest ) {
		bk_media_close(&media[dest]);
		dest = v;
		tm_putstring((const UB *)"backup: to ");
		tm_putstring((const UB *)media[dest].label);
		tm_putstring((const UB *)"\\n");
	} else if ( num == PT_SRC && v >= 0 && v < nmedia && v != srcm ) {
		bk_media_close(&media[srcm]);
		srcm = v;
		if ( state == S_PEEKED ) state = S_IDLE;
		list_volumes();
		dirty = listed = TRUE;
		tm_putstring((const UB *)"backup: from ");
		tm_putstring((const UB *)media[srcm].label);
		tm_putstring((const UB *)"\\n");
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
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &msel) >= E_OK );
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
		sheet_to(( msel.code[0] == 's' ) ? T_SAVE : T_REST);
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
	(void)ob_wri_rec(ui.kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
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
	T_OBDROP	*d;
	T_OBREF		r;
	INT		i, n = 0;
	char		s[160];

	d = ui_drop(&ui);
	if ( d == NULL ) return;
	if ( d->n <= 0 ) {
		free(d);
		return;
	}
	if ( state == S_BUSY || state == S_NEXT ) {
		drop_answer(d, OB_DR_REFUSE, "バックアップは作業中です");
		free(d);
		return;
	}
	drop_let_go();
	for ( i = 0; i < d->n && item_room(n + 1); i++ ) {
		if ( ob_ref_obj(&d->v[i].target, &r) < E_OK || r.type != OB_T_STORAGE ) continue;
		memset(&items[n], 0, sizeof(items[n]));
		items[n].uuid = d->v[i].target;
		name_of(&items[n].uuid, items[n].name, sizeof(items[n].name));
		if ( items[n].name[0] == 0 ) strncpy(items[n].name, (const char *)d->v[i].name, sizeof(items[n].name) - 1);
		items[n].parent = d->fromobj;		/* the record the links were in */
		items[n].box[0] = d->v[i].left;
		items[n].box[1] = d->v[i].top;
		items[n].box[2] = d->v[i].right;
		items[n].box[3] = d->v[i].bottom;
		items[n].hasbox = (BOOL)( d->v[i].right > d->v[i].left && d->v[i].bottom > d->v[i].top );
		if ( ( d->mods & ( HID_MOD_LSHIFT | HID_MOD_RSHIFT ) ) != 0 ) (void)excl_add(&items[n].uuid);
		else (void)pick_add(&items[n]);
		n++;
	}
	if ( n == 0 ) {
		drop_answer(d, OB_DR_REFUSE, "保存できる実身がありません");
		list_again();
		free(d);
		return;
	}
	drop_answer(d, OB_DR_ACCEPT, "置かれた実身を保存の対象にしました");
	dropped = TRUE;
	tab = T_SAVE;
	state = S_IDLE;
	nitem = n;
	sel = 0;
	dirty = listed = TRUE;
	snprintf(s, sizeof(s), "backup: dropped %d picked %d left out %d\n", n, npick, nexcl);
	tm_putstring((const UB *)s);
	say("置かれた実身を〈保存〉で保存します（〈選ぶ〉で選び直し、〈除く〉で除く）");
	free(d);
}

LOCAL void event( const T_OBNTM *m )
{
	INT	num = 0, ev = ui_event(&ui, m, &num);

	switch ( ev ) {
	case UI_EV_CLOSE:
		closing = TRUE;
		break;
	case UI_EV_MENU:
		menu(m);
		break;
	case UI_EV_KEY:
		key(m);
		break;
	case UI_EV_DROP:
		drop_in();
		break;
	case UI_EV_SHEET:
		sheet_to(( num == 2 ) ? T_REST : T_SAVE);
		break;
	case UI_EV_BUTTON:
	case UI_EV_CHANGE:
		part(ev, num);
		break;
	default:
		break;
	}
}

LOCAL ER open_window( void )
{
	ui_icon(SYSDEF_PROG_BACKUP);
	if ( ui_open(&ui, SYSDEF_BOX_BACKUP, PN_MAIN, 160, 120) < E_OK ) return E_OBJ;
	shown = TRUE;
	places_named(PT_DEST, dest);
	places_named(PT_SRC, srcm);
	return E_OK;
}

LOCAL void close_window( void )
{
	if ( shown ) ui_close(&ui);
	shown = FALSE;
}

int main( void )
{
	static T_BKARG	a;
	T_OBNTM	m;
	SZ	asz = 0, n;
	INT	i;

	n = ts_get_arg(&a, sizeof(a));
	if ( n >= (SZ)sizeof(a) && a.magic == BK_ARG_MAGIC ) {
		/* the work without the window: the argument whole, its lists as long as they are */
		T_BKARG	*w;

		if ( a.nroot < 0 || a.nexcl < 0 || n != (SZ)BK_ARG_SIZE(a.nroot, a.nexcl) ) {
			tm_putstring((const UB *)"backup: failed the argument is not whole\n");
			return 1;
		}
		w = (T_BKARG *)malloc((size_t)n);
		if ( w == NULL || ts_get_arg(w, n) != n ) {
			tm_putstring((const UB *)"backup: failed no memory\n");
			return 1;
		}
		i = batch(w);
		free(w);
		return i;
	}

	nmedia = bk_media_list(&media);
	{
		TS_UUID	first;

		memset(&first, 0, sizeof(first));
		(void)bk_first_cabinet(&first);
		nplace = 0;
		if ( !place_push(&first) || nmedia < 2 ) {
			tm_putstring((const UB *)"backup: no memory\n");
			return 3;
		}
	}
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
			(void)place_push(&argobj);
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
	say(( tab == T_SAVE ) ? "保存する実身を選んで〈保存〉を押してください" : "復帰する巻を選んで〈復帰〉を押してください");
	draw();
	{
		char	s[80];

		/* said once the list is in its part (draw): a press on a line from now on finds it there */
		snprintf(s, sizeof(s), "backup: ready, %d listed\n", nitem);
		tm_putstring((const UB *)s);
	}

	/* the window's channel waited on: the first notice of a round for as long as it takes, the rest as they are */
	while ( !closing ) {
		for ( TMO tmo = TMO_FEVR; !closing && ob_rea_tmo(ui.port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK;
		      tmo = TMO_POL ) {
			event(&m);
		}
		if ( dirty && !closing ) draw();
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
