/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sndmix.c
 *	音量ミキサー: the accessory (design 11.6, 6.5.6)
 *
 *	The sound device's mixer, as its object shows it (OB_SND_MIX) and
 *	takes it (OB_SND_MIXSET):
 *
 *	  全体		    the output the sound plays on, the volume of the
 *			    left and of the right, 消音, and 左右をそろえる,
 *			    with which a move of either takes the other along
 *	  アプリケーション  every program that has played on the device and
 *			    is still there, a row each: its icon, its name
 *			    with 再生中 while it plays, its volume and 消音
 *
 *	The window can be made taller and shorter; as many rows are shown as
 *	its work area has room for (up to NROW_MAX, the rows the panel has),
 *	and the rest are wound with the window's own scroll bar down its
 *	right side, or the wheel, as in any window. The bar counts programs:
 *	everything there is is the list, what is shown the rows from the
 *	top one. The top row stays on the program it shows when programs
 *	come and go before it.
 *
 *	A change is written at once and so heard at once; the device keeps
 *	it for the next start by itself, a second after the last change.
 *	The page is read again when the device tells that the mixer changed
 *	(OB_E_CHANGE of OB_SND_MIX: a program come or gone, one that starts
 *	or stops playing, a volume set here or elsewhere -- システム環境設定's
 *	音量 is the whole's) and when the system object tells that the output
 *	changed (OB_SYS_SOUND); nothing is looked at between. A part the hand
 *	has just moved is left as the hand put it for a second, so that a
 *	reading does not pull it back mid-drag, and read again once the
 *	second is over.
 *
 *	Everything is the window manager's parts from the data box
 *	(SYSDEF_BOX_SNDMIX): volumes, switches, boxes, the icon part that
 *	shows a program object's icon. Nothing here draws.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ts/ui.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/snd.h>

#define HOLD_MS		1000		/* a part just moved is left alone this long */
#define TEXT_MAX	4096

#define PN_MAIN		1
#define P_OUT		1
#define P_LEFT		2
#define P_LEFT_BOX	3
#define P_RIGHT		4
#define P_RIGHT_BOX	5
#define P_MUTE		6
#define P_LINK		7
#define NROW_MAX	10		/* the rows the panel has */
#define ROW_TOP		222		/* where the first row is in the panel */
#define ROW_H		40
#define START_ROWS	5		/* rows the window opens with */
#define P_ROW(k)	( 100 + (k) * 10 )
#define R_ICON		0
#define R_NAME		1
#define R_VOL		2
#define R_VOL_BOX	3
#define R_MUTE		4

LOCAL UI	ui;
LOCAL BOOL	closing;
LOCAL ID	dev = 0;		/* the sound device's object */
LOCAL SDMix	mix_none;		/* no mixer: nothing at all */
LOCAL SDMix	*mixp = &mix_none;	/* the mixer as last read: the whole and every program */
#define mix	(*mixp)
LOCAL INT	top;			/* the program in the first row, from 0 */
LOCAL INT	vis = START_ROWS;	/* rows the work area has room for */
LOCAL UB	top_prog[16];		/* the program in the first row, to keep it there */
LOCAL INT	top_pid = -1;
LOCAL INT	said_vis = -1, said_top = -1, said_n = -1, said_pid = -2;	/* the list as last said */
LOCAL UD	hold_all;		/* the whole left as the hand put it until then, 0 not */
LOCAL UD	hold_row[NROW_MAX];	/* and each row */

/*
 * What the parts were last set to from the mixer. A reading sets a part
 * only when what the mixer says of it is other than that, or the row now
 * shows another program: a reading told of something else -- a program
 * come or gone, the click's own sound -- leaves a switch the hand has
 * just turned as it is, before the program has taken the turn.
 */
typedef struct {
	BOOL	set;
	UB	prog[16];
	INT	pid;
	UINT	vol, mute;
} SHOWN;

LOCAL SHOWN	shown_all;
LOCAL SHOWN	shown_row[NROW_MAX];
LOCAL INT	shown_rows = -1;	/* rows shown, -1 not yet set */
LOCAL char	said[512];		/* what was last said of the programs */

/* ---------------------------------------------------------------- the device */

LOCAL ID dev_open( void )
{
	TS_UUID	u;
	INT	cnt = 0;

	if ( dev > 0 ) return dev;
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, &u, 1, &cnt) < E_OK || cnt < 1 ) return E_NOEXS;
	dev = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	return dev;
}

LOCAL BOOL mix_read( void )
{
	SZ	asz = 0;
	ER	er = E_NOEXS;
	LOCAL ER said_er = 1;

	if ( dev_open() > 0 ) {
		UB	*b = NULL;
		INT	cap = 0, len = ui_key_all(dev, OB_SND_MIX, 0, &b, &cap);

		er = ( len < 0 ) ? (ER)len : E_OK;
		asz = ( len < 0 ) ? 0 : len;
		if ( er >= E_OK && ( asz < (SZ)sizeof(SDMix)
				  || asz < (SZ)SND_MIX_SIZE(((SDMix *)b)->napp) ) ) er = E_OBJ;
		if ( er >= E_OK ) {
			if ( mixp != &mix_none ) free(mixp);
			mixp = (SDMix *)b;
		} else {
			free(b);
		}
	}
	if ( er < E_OK && er != said_er ) {
		char	s[80];

		snprintf(s, sizeof(s), "sndmix: the mixer not read (%d, %d bytes)\n", (INT)er, (INT)asz);
		tm_putstring((CONST UB *)s);
	}
	said_er = er;
	return (BOOL)( er >= E_OK );
}

LOCAL ER mix_put( CONST SDMixSet *s )
{
	SZ	asz = 0;

	if ( dev_open() <= 0 ) return E_NOEXS;
	return ob_wri_rec(dev, OB_SND_MIXSET, 0, s, sizeof(*s), &asz);
}

/* The output the sound plays on now, or would, for a person */
LOCAL void out_name( char *out, INT max )
{
	SDOut		*o;
	UB		*b = NULL;
	INT		n, k, at = -1, cap = 0, len;
	CONST char	*nm;

	snprintf(out, max, "（なし）");
	if ( dev_open() <= 0 || ( len = ui_key_all(dev, OB_SND_OUTS, 0, &b, &cap) ) < 0 ) return;
	o = (SDOut *)b;
	n = len / (INT)sizeof(SDOut);
	for ( k = 0; k < n && at < 0; k++ ) {
		if ( ( o[k].flags & SND_OF_PLAYING ) != 0 ) at = k;
	}
	for ( k = 1; k < n && at < 0; k++ ) {
		if ( ( o[k].flags & SND_OF_NOSOUND ) == 0 ) at = k;	/* what 自動 would take */
	}
	if ( at < 0 ) {
		free(b);
		return;
	}
	nm = (CONST char *)o[at].name;
	switch ( o[at].kind ) {
	case SND_BE_USB:
		snprintf(out, max, "ＵＳＢ：%s", ( nm[0] != 0 ) ? nm : "オーディオ機器");
		break;
	case SND_BE_I2S:
		snprintf(out, max, "Ｉ２ＳのＤＡＣ（ＧＰＩＯ１８〜２１）");
		break;
	case SND_BE_HDMI:
		snprintf(out, max, "%s：%s", ( strcmp((CONST char *)o[at].id, "hdmi1") == 0 ) ? "ＨＤＭＩ１" : "ＨＤＭＩ０",
			 ( nm[0] != 0 ) ? nm : "画面");
		break;
	case SND_BE_HDA:
		snprintf(out, max, "内蔵のサウンド：%s", ( nm[0] != 0 ) ? nm : "ＨＤオーディオ");
		break;
	default:
		break;
	}
	if ( ( o[at].flags & SND_OF_PLAYING ) == 0 ) {
		strncat(out, "（いまは鳴っていません）", max - strlen(out) - 1);
	}
	free(b);
}

/* ---------------------------------------------------------------- the page */

LOCAL void vol_show( INT num, INT box, INT v )
{
	char	t[16];

	ui_set_range(&ui, num, v, SND_VOL_MAX, 0);
	snprintf(t, sizeof(t), "%d", v);
	ui_set_text(&ui, box, t);
}

LOCAL BOOL prog_none( CONST UB *p )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( p[i] != 0 ) return FALSE;
	}
	return TRUE;
}

/* A program object's UUID as text, for the icon part; "" with none */
LOCAL void uuid_text( CONST UB *p, char *out )
{
	INT	i, at = 0;

	out[0] = 0;
	if ( prog_none(p) ) return;
	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[at++] = '-';
		at += sprintf(out + at, "%02x", p[i]);
	}
}

LOCAL void row_show( INT k, BOOL on )
{
	INT	f;

	for ( f = R_ICON; f <= R_MUTE; f++ ) {
		ui_show(&ui, P_ROW(k) + f, on);
	}
}

/* The rows the work area has room for */
LOCAL void rows_fit( void )
{
	INT	w, h;

	ui_work(&ui, &w, &h);
	vis = ( h - ROW_TOP ) / ROW_H;
	if ( vis < 1 ) vis = 1;
	if ( vis > NROW_MAX ) vis = NROW_MAX;
}

/* The first row kept inside the list, the bar told, and which program is there remembered */
LOCAL void top_keep( INT n )
{
	if ( top > n - vis ) top = n - vis;
	if ( top < 0 ) top = 0;
	ui_bar(&ui, 0, n, top, ( top + vis < n ) ? top + vis : n);
	if ( top < n ) {
		memcpy(top_prog, mix.app[top].prog, 16);
		top_pid = mix.app[top].pid;
	} else {
		top_pid = -1;
	}
}

/* The program that was in the first row, found again in a list read anew */
LOCAL void top_find( INT n )
{
	INT	i;

	if ( top_pid < 0 ) return;
	for ( i = 0; i < n; i++ ) {
		if ( memcmp(mix.app[i].prog, top_prog, 16) == 0 && mix.app[i].pid == top_pid ) {
			top = i;
			return;
		}
	}
}

/* The page from the mixer as it is now */
LOCAL void fill( void )
{
	char	t[160], u[40], s[512];
	INT	k, n, at = 0, i, shown;

	if ( !mix_read() ) {
		if ( mixp != &mix_none ) free(mixp);
		mixp = &mix_none;
		ui_set_text(&ui, P_OUT, "（サウンドデバイスがありません）");
	} else {
		out_name(t, sizeof(t));
		ui_set_text(&ui, P_OUT, t);
	}
	if ( ui_now() >= hold_all && ( !shown_all.set || shown_all.vol != ( mix.lv << 16 | mix.rv )
				       || shown_all.mute != mix.mute ) ) {
		vol_show(P_LEFT, P_LEFT_BOX, (INT)mix.lv);
		vol_show(P_RIGHT, P_RIGHT_BOX, (INT)mix.rv);
		ui_set_value(&ui, P_MUTE, mix.mute != 0);
		shown_all.set = TRUE;
		shown_all.vol = mix.lv << 16 | mix.rv;
		shown_all.mute = mix.mute;
	}
	n = (INT)mix.napp;
	top_find(n);
	top_keep(n);
	for ( k = 0; k < vis; k++ ) {
		SDMixApp	*a;

		i = top + k;
		if ( i >= n ) {
			break;
		}
		a = &mix.app[i];
		uuid_text(a->prog, u);
		if ( u[0] == 0 && a->pid == 0 ) {
			TS_UUID	snd;

			/* the system's own: the icon of システムの音 */
			if ( ob_fnd_sys(SYSROLE_SOUNDS, SYSDEF_SND_BOX, &snd) >= E_OK ) uuid_text(snd.b, u);
		}
		ui_set_text(&ui, P_ROW(k) + R_ICON, u);
		/* two of the same name (started from one file) are told apart by their process */
		{
			INT	j, same = 0;

			for ( j = 0; j < n; j++ ) {
				if ( j != i && strcmp((CONST char *)mix.app[j].name, (CONST char *)a->name) == 0 ) same++;
			}
			if ( same > 0 && a->pid > 0 ) {
				snprintf(t, sizeof(t), "%s（%d）%s", (CONST char *)a->name, (INT)a->pid,
					 ( a->playing > 0 ) ? "　再生中" : "");
			} else {
				snprintf(t, sizeof(t), "%s%s", (CONST char *)a->name, ( a->playing > 0 ) ? "　再生中" : "");
			}
		}
		ui_set_text(&ui, P_ROW(k) + R_NAME, t);
		if ( ui_now() >= hold_row[k] ) {
			SHOWN	*w = &shown_row[k];

			if ( !w->set || memcmp(w->prog, a->prog, 16) != 0 || w->pid != a->pid
			  || w->vol != a->vol || w->mute != a->mute ) {
				vol_show(P_ROW(k) + R_VOL, P_ROW(k) + R_VOL_BOX, (INT)a->vol);
				ui_set_value(&ui, P_ROW(k) + R_MUTE, a->mute != 0);
				w->set = TRUE;
				memcpy(w->prog, a->prog, 16);
				w->pid = a->pid;
				w->vol = a->vol;
				w->mute = a->mute;
			}
		}
	}
	shown = ( n - top < vis ) ? n - top : vis;
	if ( shown < 0 ) shown = 0;
	for ( k = shown; k < NROW_MAX; k++ ) shown_row[k].set = FALSE;	/* set again when shown again */
	if ( shown != shown_rows ) {
		for ( k = 0; k < NROW_MAX; k++ ) row_show(k, k < shown);
		shown_rows = shown;
	}
	for ( i = 0; i < n; i++ ) {
		SDMixApp	*a = &mix.app[i];

		at += snprintf(s + at, sizeof(s) - at, "%s %d %d %d; ", (CONST char *)a->name,
			       (INT)a->vol, (INT)a->mute, (INT)a->playing);
		if ( at >= (INT)sizeof(s) - 1 ) at = (INT)sizeof(s) - 1;
	}
	s[at] = 0;
	if ( vis != said_vis || top != said_top || n != said_n || top_pid != said_pid ) {
		char	l[80];

		/* where the list is wound and which process is first, told when it changes, for the tests */
		said_vis = vis;
		said_top = top;
		said_n = n;
		said_pid = top_pid;
		snprintf(l, sizeof(l), "sndmix: rows %d from %d of %d, pid %d first\n", vis, top, n, top_pid);
		tm_putstring((CONST UB *)l);
	}
	if ( strcmp(s, said) != 0 ) {
		char	l[600];

		/* told when it changes, for the tests of the screen */
		snprintf(said, sizeof(said), "%s", s);
		snprintf(l, sizeof(l), "sndmix: %d programs: %s\n", n, s);
		tm_putstring((CONST UB *)l);
	}
}

/* The list wound so that program first is in the first row */
LOCAL void wind( INT first )
{
	INT	k;

	top = first;
	top_keep((INT)mix.napp);	/* the program now first is the one kept first */
	for ( k = 0; k < NROW_MAX; k++ ) hold_row[k] = 0;
	fill();
}

/* The window made larger or smaller: as many rows as there is room for */
LOCAL void resized( void )
{
	INT	w, h, was = vis;
	T_OBWPOS wp;
	SZ	asz = 0;

	ui_work(&ui, &w, &h);
	/* never so short that no row is left */
	if ( h < ROW_TOP + ROW_H && ob_rea_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.bottom += ROW_TOP + ROW_H - h;
		(void)ob_wri_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	rows_fit();
	if ( vis != was ) {
		wind(top);
	}
}

/* ---------------------------------------------------------------- what is done */

LOCAL void all_set( INT moved )
{
	SDMixSet	s;
	INT		l = ui_value(&ui, P_LEFT), r = ui_value(&ui, P_RIGHT);
	char		t[96];
	ER		er;

	if ( l < 0 ) l = 0;
	if ( l > SND_VOL_MAX ) l = SND_VOL_MAX;
	if ( r < 0 ) r = 0;
	if ( r > SND_VOL_MAX ) r = SND_VOL_MAX;
	if ( ui_value(&ui, P_LINK) != 0 ) {
		/* the two together: the other follows the one moved */
		if ( moved == P_LEFT ) {
			r = l;
			vol_show(P_RIGHT, P_RIGHT_BOX, r);
		} else if ( moved == P_RIGHT ) {
			l = r;
			vol_show(P_LEFT, P_LEFT_BOX, l);
		}
	}
	snprintf(t, sizeof(t), "%d", l);
	ui_set_text(&ui, P_LEFT_BOX, t);
	snprintf(t, sizeof(t), "%d", r);
	ui_set_text(&ui, P_RIGHT_BOX, t);
	memset(&s, 0, sizeof(s));
	s.what = SND_MIX_ALL;
	s.lv = (UW)l;
	s.rv = (UW)r;
	s.mute = ( ui_value(&ui, P_MUTE) != 0 );
	er = mix_put(&s);
	hold_all = ui_now() + HOLD_MS;
	snprintf(t, sizeof(t), "sndmix: whole %d %d mute %d (%d)\n", l, r, (INT)s.mute, (INT)er);
	tm_putstring((CONST UB *)t);
}

LOCAL void row_set( INT k )
{
	INT		i = top + k, v;
	SDMixApp	*a;
	SDMixSet	s;
	char		t[160];
	ER		er;

	if ( k < 0 || k >= vis || i >= (INT)mix.napp ) return;
	a = &mix.app[i];
	v = ui_value(&ui, P_ROW(k) + R_VOL);
	if ( v < 0 ) v = 0;
	if ( v > SND_VOL_MAX ) v = SND_VOL_MAX;
	snprintf(t, sizeof(t), "%d", v);
	ui_set_text(&ui, P_ROW(k) + R_VOL_BOX, t);
	memset(&s, 0, sizeof(s));
	s.what = SND_MIX_APP;
	memcpy(s.prog, a->prog, 16);
	s.pid = a->pid;
	s.lv = s.rv = (UW)v;
	s.mute = ( ui_value(&ui, P_ROW(k) + R_MUTE) != 0 );
	er = mix_put(&s);
	a->vol = s.lv;
	a->mute = s.mute;
	hold_row[k] = ui_now() + HOLD_MS;
	snprintf(t, sizeof(t), "sndmix: %s %d mute %d (%d)\n", (CONST char *)a->name, v, (INT)s.mute, (INT)er);
	tm_putstring((CONST UB *)t);
}

LOCAL void changed( INT id )
{
	INT	k;

	if ( id == P_LEFT || id == P_RIGHT || id == P_MUTE ) {
		all_set(id);
		return;
	}
	for ( k = 0; k < NROW_MAX; k++ ) {
		if ( id == P_ROW(k) + R_VOL || id == P_ROW(k) + R_MUTE ) {
			row_set(k);
			return;
		}
	}
}

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

	if ( ui_uuid(SYSDEF_MENU_SNDMIX, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		closing = TRUE;
	} else if ( is_code(&sel, "again") ) {
		fill();
	}
}

/* When the first part left as the hand put it is to be read again, 0 none */
LOCAL UD hold_next( void )
{
	UD	at = 0;
	INT	k;

	if ( hold_all != 0 ) at = hold_all;
	for ( k = 0; k < NROW_MAX; k++ ) {
		if ( hold_row[k] != 0 && ( at == 0 || hold_row[k] < at ) ) at = hold_row[k];
	}
	return at;
}

/* The parts whose second is over: no more left alone */
LOCAL void hold_over( void )
{
	UD	now = ui_now();
	INT	k;

	if ( hold_all != 0 && now >= hold_all ) hold_all = 0;
	for ( k = 0; k < NROW_MAX; k++ ) {
		if ( hold_row[k] != 0 && now >= hold_row[k] ) hold_row[k] = 0;
	}
}

#define NTF_MIX		12		/* the id of the notices of the mixer */
#define NTF_OUT		13		/* and of what plays sound (the system object) */

/*
 * The mixer and the output watched, asked before the page is first read
 * so that nothing that changes after that read goes untold
 */
LOCAL ID mix_watch( void )
{
	T_OBNTF	req;
	TS_UUID	u;
	ID	k = 0;

	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = NTF_MIX;
	if ( dev_open() > 0 ) (void)ob_ntf_evt(dev, OB_SND_MIX, &req, ui.port);
	req.id = NTF_OUT;
	if ( ui_uuid(SYSDEF_SYSTEM, &u) >= E_OK && ( k = ob_opn_obj(&u, OB_OP_ATRRD) ) > 0
	  && ob_ntf_evt(k, OB_SYS_SOUND, &req, ui.port) <= 0 ) {
		ob_cls_obj(k);
		k = 0;
	}
	return ( k > 0 ) ? k : 0;
}

/*
 * What plays sound, the record the output's notice is of, read whole: a
 * change after this read is told again (the page names the output from
 * the device's own list, OB_SND_OUTS)
 */
LOCAL void out_read( void )
{
	TS_UUID	u;
	UB	*b = NULL;

	if ( ui_uuid(SYSDEF_SYSTEM, &u) >= E_OK ) (void)ui_rec_all(&u, OB_SYS_SOUND, 0, &b);
	free(b);
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	id, ev;
	ID	ksys;
	BOOL	told;

	ui_icon(SYSDEF_PROG_SNDMIX);
	ui_attr(WM_ATTR_RESIZE | WM_ATTR_RBAR);
	if ( ui_open(&ui, SYSDEF_BOX_SNDMIX, PN_MAIN, 200, 100) < E_OK ) {
		tm_putstring((CONST UB *)"sndmix: no window\n");
		return 1;
	}
	/* opened with START_ROWS rows; the panel has more, for a taller window */
	{
		T_OBWPOS	wp;

		if ( ob_rea_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
			wp.bottom -= ( wp.wbottom - wp.wtop ) - ( ROW_TOP + START_ROWS * ROW_H + 8 );
			(void)ob_wri_rec(ui.kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		}
	}
	rows_fit();
	ksys = mix_watch();
	out_read();
	fill();
	tm_putstring((CONST UB *)"sndmix: ready\n");

	/* until something is told, or a part's second of being left alone is over */
	while ( !closing ) {
		UD	at = hold_next();

		told = FALSE;
		for ( TMO tmo = ( at != 0 ) ? ui_left(at) : TMO_FEVR;
		      !closing && ui_wait(&ui, &m, tmo) >= E_OK; tmo = TMO_POL ) {
			if ( m.id == NTF_MIX || m.id == NTF_OUT ) {
				if ( m.id == NTF_OUT ) out_read();
				told = TRUE;		/* the mixer or the output changed: read once the notices are taken */
				continue;
			}
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				closing = TRUE;
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_KEY:
				if ( ( m.mods & ( 0x01 | 0x10 ) ) != 0 ) {
					UB	c = wm_key_char(m.code, m.mods);

					if ( c == 'e' || c == 'E' ) closing = TRUE;
				}
				break;
			case UI_EV_CHANGE:
				changed(id);
				break;
			case UI_EV_WHEEL:
				wind(top - id);		/* a notch away from the person: up the list */
				break;
			case UI_EV_SCROLL:
				wind(id);
				break;
			case UI_EV_SIZE:
				resized();
				break;
			default:
				break;
			}
		}
		if ( closing ) break;
		if ( at != 0 && ui_now() >= at ) {
			hold_over();
			told = TRUE;		/* what a part left alone shows, read again */
		}
		if ( told ) fill();
	}
	if ( ksys > 0 ) ob_cls_obj(ksys);
	if ( dev > 0 ) ob_cls_obj(dev);
	ui_close(&ui);
	tm_putstring((CONST UB *)"sndmix: closed\n");
	return 0;
}
