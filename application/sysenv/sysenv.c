/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysenv.c
 *	システム環境設定: the accessory (design 16.5.23)
 *
 *	What the machine is and how it is set:
 *	  〈機器〉〈機器情報〉 the devices; [表示] opens the page of the one
 *			      chosen -- the screen's size, the network card,
 *			      the USB devices, the disks, the video, the
 *			      keyboard, the sound
 *	  〈機器〉〈電源管理〉 how the machine is powered, and 再起動 and
 *			      電源を切る
 *	  〈画面〉〈背景〉	   the wallpaper, one of the wallpaper box's pictures
 *	  〈画面〉〈配色〉	   the colour scheme
 *	  〈書体〉		   the faces of the font box
 *	  〈バージョン〉	   the system and the programs of the program box,
 *			   each with its version (R1.000) at the right end;
 *			   [詳細情報] shows the one chosen; a package (.tpk)
 *			   dropped on the list is installed, or updates the
 *			   program it is a newer version of; 削除 in the
 *			   menu's 操作 deletes the one chosen
 *
 *	Everything is read and changed through objects: what the machine is
 *	from the system object's records (SYSDEF_SYSTEM, OB_SYS_*), which
 *	also take the scheme, the wallpaper and the power when written; what
 *	is kept from one start to the next in the machine's settings
 *	(SYSDEF_CONF_DEV), which the system takes again as it is written;
 *	the disks as the device objects they are, and the pictures, faces
 *	and programs as the objects their boxes link to.
 */

#include <string.h>
#include <stdio.h>
#include <ts/ui.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/look.h>
#include <ts/json.h>
#include <ts/dtreq.h>

#define TICK_MS		20
#define TEXT_MAX	4096
#define LIST_MAX	64

/* The sheets: the tags, and the devices' pages opened from 〈機器情報〉 */
#define S_DEVS		1
#define S_POWER		2
#define S_WALL		3
#define S_SCHEME	4
#define S_FONT		5
#define S_VERSION	6
#define S_D_SCREEN	8
#define S_D_NET		9
#define S_D_USB		10
#define S_D_DISK	11
#define S_D_VIDEO	12
#define S_D_KBD		13
#define S_D_SOUND	14

/* The parts, by the numbers the data box (SYSDEF_BOX_SYSENV) gives them */
#define P_DEVS		1
#define P_SHOW		2
#define P_POWER_NOW	3
#define P_BATTERY	4
#define P_PM		5
#define P_RESTART	6
#define P_OFF		7
#define P_WALLS		8
#define P_WALL_MODE	9
#define P_SCHEMES	11
#define P_FONTS		13
#define P_FONT_INFO	14
#define P_VER_NAME	15
#define P_VER_KERNEL	16
#define P_VER_BUILD	17
#define P_VER_BOARD	18
#define P_VER_CPU	19
#define P_VER_MEM	20
#define P_PROGS		21
#define P_BACK		22
#define P_SCR_LIST	23
#define P_SCR_CANCEL	25
#define P_SCR_SET	26
#define P_NET_1		27
#define P_USB_LIST	33
#define P_USB_AGAIN	34
#define P_DISK_LIST	35
#define P_VID_1		36
#define P_KBD_TYPE	39
#define P_KBD_MODE	40
#define P_KBD_CANCEL	41
#define P_KBD_SET	42
#define P_SND_1		43
#define P_PROG_INFO	46

#define PN_MAIN		1
#define PN_TELL		10
#define PN_POWER	11
#define PN_SCREEN	12
#define PN_PROG		13
#define B_POWER		3
#define B_RESTART	105

LOCAL CONST char *CONST dev_names[] = {
	"画面サイズ／色数", "ネットワークアダプタ", "ＵＳＢ接続機器", "ディスク",
	"ビデオアダプタ", "キーボード", "サウンド",
};
LOCAL CONST INT dev_pages[] = { S_D_SCREEN, S_D_NET, S_D_USB, S_D_DISK, S_D_VIDEO, S_D_KBD, S_D_SOUND };
#define NDEV	7

LOCAL UI	ui;
LOCAL BOOL	closing;
LOCAL UB	text[TEXT_MAX];

/* What lists show: names kept here */
LOCAL char	wall_name[LIST_MAX][64];
LOCAL TS_UUID	wall_id[LIST_MAX];
LOCAL INT	nwall, wall_cur;
LOCAL char	scheme_name[8][32];
LOCAL INT	nscheme;
LOCAL char	font_name[LIST_MAX][64];
LOCAL TS_UUID	font_id[LIST_MAX];
LOCAL INT	nfont;
LOCAL char	prog_name[LIST_MAX][64];
LOCAL TS_UUID	prog_id[LIST_MAX];
LOCAL INT	nprog;

/* What each program's metadata says (tessronos.program) */
typedef struct {
	char	name[64];
	char	id[40];
	char	kind[16];
	char	version[16];
	BOOL	builtin;
} PROGINFO;
LOCAL PROGINFO	prog_info[LIST_MAX];
LOCAL char	usb_line[LIST_MAX][80];
LOCAL char	disk_line[LIST_MAX][80];
LOCAL char	mode_name[16][40];
LOCAL INT	mode_w[16], mode_h[16], nmode, mode_cur;

/* A list's names, as pointers to what is kept */
LOCAL void names_of( INT num, char (*v)[64], INT n )
{
	CONST char	*p[LIST_MAX];
	INT		i;

	for ( i = 0; i < n && i < LIST_MAX; i++ ) p[i] = v[i];
	ui_names(&ui, num, p, n);
}

/* ---------------------------------------------------------------- objects */

/* A record of an object, into text; its length or an error */
LOCAL INT rec_of( CONST TS_UUID *u, INT recno, UB *buf, INT max )
{
	ID	key;
	SZ	asz = 0;
	ER	er;

	key = ob_opn_obj(u, OB_OP_R);
	if ( key < E_OK ) return (INT)key;
	er = ob_rea_rec(key, recno, 0, buf, max - 1, &asz);
	ob_cls_obj(key);
	if ( er < E_OK ) return (INT)er;
	buf[asz] = 0;
	return (INT)asz;
}

LOCAL INT sys_rec( INT recno )
{
	TS_UUID	u;

	if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return E_SYS;
	return rec_of(&u, recno, text, TEXT_MAX);
}

/* The screen's mode, from the screen's own object 画面 (SIZE, BPP, DRIVER, MODES) */
LOCAL INT disp_rec( void )
{
	CONST TS_UUID	u = OB_UUID_DISPLAY_INIT;

	return rec_of(&u, OB_DSP_MODE, text, TEXT_MAX);
}

LOCAL ER sys_write( INT recno, CONST char *s )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, recno, 0, s, (SZ)strlen(s), &asz);
	ob_cls_obj(key);
	return er;
}

/* A line of the machine's settings written; the system takes it as it sees it */
LOCAL ER dev_put( CONST char *key, CONST char *val )
{
	TS_UUID	u;
	ID	k;
	SZ	asz = 0;
	INT	len;
	ER	er;

	if ( ui_uuid(SYSDEF_CONF_DEV, &u) < E_OK ) return E_SYS;
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( k < E_OK ) return (ER)k;
	er = ob_rea_rec(k, 1, 0, text, TEXT_MAX, &asz);
	len = ( er >= E_OK ) ? (INT)asz : 0;
	len = cf_put(text, len, TEXT_MAX, key, 0, val);
	er = ( len < 0 ) ? E_LIMIT : ob_wri_rec(k, 1, 0, text, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(k, 1, (UD)len);
	ob_cls_obj(k);
	return er;
}

LOCAL BOOL dev_get( CONST char *key, char *v, INT max )
{
	TS_UUID	u;
	INT	len;

	if ( ui_uuid(SYSDEF_CONF_DEV, &u) < E_OK ) return FALSE;
	len = rec_of(&u, 1, text, TEXT_MAX);
	return (BOOL)( len > 0 && cf_get(text, len, key, 0, v, max) );
}

/*
 * The objects a box links to (ob_lst_lnk), those that are there, and
 * each one's name.
 */
#define BOX_LINKS_MAX	256

LOCAL INT box_links( CONST char *box, TS_UUID *ids, char (*names)[64], INT max )
{
	TS_UUID	b;
	T_OBREF	r;
	INT	all = 0, i, n = 0;
	static TS_UUID lk[BOX_LINKS_MAX];

	if ( ui_uuid(box, &b) < E_OK ) return 0;
	if ( ob_lst_lnk(&b, NULL, lk, BOX_LINKS_MAX, &all) < E_OK ) return 0;
	for ( i = 0; i < all && i < BOX_LINKS_MAX && n < max; i++ ) {
		if ( ob_ref_obj(&lk[i], &r) < E_OK ) continue;
		ids[n] = lk[i];
		strncpy(names[n], (CONST char *)r.name, 63);
		names[n][63] = 0;
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------- filled from what is */

/* A program object's metadata read: its name, id, kind, version */
LOCAL void prog_read( CONST TS_UUID *u, PROGINFO *pi )
{
	static UB	meta[4096];
	T_JSON		root, tf, g;
	SZ		asz = 0;
	ID		key;

	memset(pi, 0, sizeof(*pi));
	key = ob_opn_obj(u, OB_OP_R);
	if ( key <= 0 ) return;
	if ( ob_get_atr(key, meta, sizeof(meta) - 1, &asz) >= E_OK
	  && js_parse(meta, (INT)asz, &root) >= E_OK
	  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "program", &g) >= E_OK ) {
		if ( js_get_str(&g, "name", (UB *)pi->name, sizeof(pi->name)) <= 0 ) {
			(void)js_get_str(&root, "name", (UB *)pi->name, sizeof(pi->name));
		}
		(void)js_get_str(&g, "id", (UB *)pi->id, sizeof(pi->id));
		(void)js_get_str(&g, "kind", (UB *)pi->kind, sizeof(pi->kind));
		(void)js_get_str(&g, "version", (UB *)pi->version, sizeof(pi->version));
		pi->builtin = js_get_bool(&g, "builtin", FALSE);
	}
	ob_cls_obj(key);
}

LOCAL void val_of( INT len, CONST char *key, char *v, INT max )
{
	if ( len <= 0 || !cf_get(text, len, key, 0, v, max) ) snprintf(v, max, "（不明）");
}

LOCAL void fill_version( void )
{
	char	v[CF_VAL_MAX], s[CF_VAL_MAX * 2];
	INT	len = sys_rec(OB_SYS_INFO), i;
	static TS_UUID	ids[LIST_MAX];
	INT	n;

	val_of(len, "NAME", v, sizeof(v));
	if ( len > 0 && cf_get(text, len, "VERSION", 0, s, sizeof(s)) ) {
		strncat(v, " ", sizeof(v) - strlen(v) - 1);
		strncat(v, s, sizeof(v) - strlen(v) - 1);
	}
	ui_set_text(&ui, P_VER_NAME, v);
	val_of(len, "KERNEL", v, sizeof(v));
	ui_set_text(&ui, P_VER_KERNEL, v);
	val_of(len, "BUILD", v, sizeof(v));
	ui_set_text(&ui, P_VER_BUILD, v);
	val_of(len, "BOARD", v, sizeof(v));
	ui_set_text(&ui, P_VER_BOARD, v);
	val_of(len, "CPU", v, sizeof(v));
	snprintf(s, sizeof(s), "%s ×", v);
	val_of(len, "CPUS", v, sizeof(v));
	strncat(s, v, sizeof(s) - strlen(s) - 1);
	ui_set_text(&ui, P_VER_CPU, s);
	val_of(len, "MEMORY_MB", v, sizeof(v));
	snprintf(s, sizeof(s), "%sMB（空き ", v);
	val_of(len, "FREE_MB", v, sizeof(v));
	strncat(s, v, sizeof(s) - strlen(s) - 1);
	strncat(s, "MB）", sizeof(s) - strlen(s) - 1);
	ui_set_text(&ui, P_VER_MEM, s);

	n = box_links(SYSDEF_PROG_BOX, ids, prog_name, LIST_MAX);
	for ( i = 0; i < n; i++ ) {
		prog_id[i] = ids[i];
		prog_read(&ids[i], &prog_info[i]);
		if ( prog_info[i].name[0] == 0 ) {
			snprintf(prog_info[i].name, sizeof(prog_info[i].name), "%s", prog_name[i]);
		}
		/* the name, and the version against the right end */
		snprintf(prog_name[i], sizeof(prog_name[i]), "%s\t%s", prog_info[i].name,
			 ( prog_info[i].version[0] != 0 ) ? prog_info[i].version : "－");
	}
	nprog = n;
	names_of(P_PROGS, prog_name, n);
	ui_set_value(&ui, P_PROGS, 0);
}

LOCAL void fill_power( void )
{
	char	v[CF_VAL_MAX];
	INT	len = sys_rec(OB_SYS_POWER);

	ui_set_text(&ui, P_POWER_NOW, "ＡＣ電源");
	ui_set_text(&ui, P_BATTERY, "なし");
	ui_set_text(&ui, P_PM, "対応しない");
	if ( len <= 0 || !cf_get(text, len, "CAN", 0, v, sizeof(v)) || strstr(v, "RESTART") == NULL ) {
		ui_off(&ui, P_RESTART, TRUE);
		ui_off(&ui, P_OFF, TRUE);
	}
}

LOCAL void fill_walls( void )
{
	char	v[CF_VAL_MAX], w[48], m[8];
	INT	i, len;

	nwall = box_links(SYSDEF_WALL_BOX, wall_id, wall_name, LIST_MAX);
	names_of(P_WALLS, wall_name, nwall);
	wall_cur = 0;
	len = sys_rec(OB_SYS_WALL);
	if ( len > 0 && cf_get(text, len, "WALLPAPER", 0, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) ) {
		TS_UUID	u;

		if ( ui_uuid(w, &u) >= E_OK ) {
			for ( i = 0; i < nwall; i++ ) {
				if ( memcmp(&u, &wall_id[i], sizeof(u)) == 0 ) wall_cur = i + 1;
			}
		}
	}
	ui_set_now(&ui, P_WALLS, wall_cur, wall_cur);
	ui_set_value(&ui, P_WALL_MODE, ( len > 0 && cf_get(text, len, "MODE", 0, m, sizeof(m)) ) ? cf_num(m, 2) + 1 : 3);
}

LOCAL void fill_schemes( void )
{
	char	v[CF_VAL_MAX];
	CONST char *nm[8];
	INT	len = sys_rec(OB_SYS_SCHEME), cur;

	nscheme = 0;
	while ( len > 0 && nscheme < 8 && cf_get(text, len, "NAME", nscheme, scheme_name[nscheme], 32) ) {
		nm[nscheme] = scheme_name[nscheme];
		nscheme++;
	}
	ui_names(&ui, P_SCHEMES, nm, nscheme);
	cur = ( len > 0 && cf_get(text, len, "SCHEME", 0, v, sizeof(v)) ) ? cf_num(v, 0) + 1 : 1;
	ui_set_now(&ui, P_SCHEMES, cur, cur);
}

LOCAL void fill_fonts( void )
{
	nfont = box_links(SYSDEF_FONT_BOX, font_id, font_name, LIST_MAX);
	names_of(P_FONTS, font_name, nfont);
	ui_set_value(&ui, P_FONTS, 0);
}

LOCAL void font_info( INT i )
{
	T_OBREF	r;
	char	s[120];

	if ( i < 0 || i >= nfont || ob_ref_obj(&font_id[i], &r) < E_OK ) return;
	snprintf(s, sizeof(s), "%s　%d バイト", font_name[i], (INT)r.size);
	ui_set_text(&ui, P_FONT_INFO, s);
}

/* The screen's page: the sizes it may be started at, the one it has underlined */
LOCAL void fill_screen( void )
{
	char	v[CF_VAL_MAX], w[16], cur[32] = "";
	CONST char *nm[16];
	INT	len = disp_rec(), i;

	if ( len > 0 ) (void)cf_get(text, len, "SIZE", 0, cur, sizeof(cur));
	nmode = 0;
	mode_cur = 0;
	if ( len > 0 && cf_get(text, len, "MODES", 0, v, sizeof(v)) ) {
		for ( i = 0; nmode < 16 && cf_word(v, i, w, sizeof(w)); i++ ) {
			INT	a = 0, b = 0;
			CONST char *x = strchr(w, 'x');

			if ( x == NULL ) continue;
			a = cf_num(w, 0);
			{
				char	ww[8];
				INT	k = (INT)( x - w );

				if ( k > 7 ) continue;
				memcpy(ww, w, k);
				ww[k] = 0;
				a = cf_num(ww, 0);
			}
			b = cf_num(x + 1, 0);
			mode_w[nmode] = a;
			mode_h[nmode] = b;
			snprintf(mode_name[nmode], sizeof(mode_name[nmode]), "%d×%d：１６７７万色", a, b);
			nm[nmode] = mode_name[nmode];
			if ( strcmp(w, cur) == 0 ) mode_cur = nmode + 1;
			nmode++;
		}
	}
	ui_names(&ui, P_SCR_LIST, nm, nmode);
	ui_set_now(&ui, P_SCR_LIST, mode_cur, mode_cur);
}

LOCAL void fill_net( void )
{
	char	v[CF_VAL_MAX], s[CF_VAL_MAX * 2];
	INT	len = sys_rec(OB_SYS_NET);

	val_of(len, "ADAPTER", v, sizeof(v));
	ui_set_text(&ui, P_NET_1, v);
	val_of(len, "MAC", v, sizeof(v));
	ui_set_text(&ui, ( P_NET_1 + 1 ), v);
	val_of(len, "LINK", v, sizeof(v));
	ui_set_text(&ui, ( P_NET_1 + 2 ), strcmp(v, "up") == 0 ? "接続" : strcmp(v, "down") == 0 ? "未接続" : v);
	val_of(len, "ADDRESS", v, sizeof(v));
	ui_set_text(&ui, ( P_NET_1 + 3 ), v);
	val_of(len, "DHCP", v, sizeof(v));
	ui_set_text(&ui, ( P_NET_1 + 4 ), strcmp(v, "on") == 0 ? "使用する" : "使用しない");
	val_of(len, "SENT", v, sizeof(v));
	snprintf(s, sizeof(s), "送信 %s／受信 ", v);
	val_of(len, "RECEIVED", v, sizeof(v));
	strncat(s, v, sizeof(s) - strlen(s) - 1);
	strncat(s, " フレーム", sizeof(s) - strlen(s) - 1);
	ui_set_text(&ui, ( P_NET_1 + 5 ), s);
}

LOCAL void fill_usb( void )
{
	CONST char *nm[LIST_MAX];
	INT	len = sys_rec(OB_SYS_USB), n = 0;

	while ( len > 0 && n < LIST_MAX && cf_get(text, len, "DEVICE", n, usb_line[n], sizeof(usb_line[n])) ) {
		nm[n] = usb_line[n];
		n++;
	}
	if ( n == 0 ) {
		strcpy(usb_line[0], "（接続された機器はありません）");
		nm[0] = usb_line[0];
		n = 1;
	}
	ui_names(&ui, P_USB_LIST, nm, n);
	ui_set_value(&ui, P_USB_LIST, 0);
}

/* The disks: the device objects that are disks, their size and where they are mounted */
LOCAL void fill_disks( void )
{
	CONST char *nm[LIST_MAX];
	static TS_UUID	ids[64];
	INT	cnt = 0, i, n = 0;

	if ( ob_lst_obj(OB_T_DEVICE, OB_S_DISK, NULL, ids, 64, &cnt) < E_OK ) cnt = 0;
	for ( i = 0; i < cnt && n < LIST_MAX; i++ ) {
		T_OBREF	r;
		T_OBREC	rec[2];
		INT	nr = 0;
		ID	key;
		UD	size = 0;
		SZ	asz = 0;
		static UB meta[2048];
		char	where[64] = "";

		if ( ob_ref_obj(&ids[i], &r) < E_OK ) continue;
		key = ob_opn_obj(&ids[i], OB_OP_ATRRD);
		if ( key > 0 ) {
			if ( ob_lst_rec(key, rec, 2, &nr) >= E_OK && nr > 1 ) size = rec[1].size;
			if ( ob_get_atr(key, meta, sizeof(meta) - 1, &asz) >= E_OK ) {
				CONST char *m;

				meta[asz] = 0;
				m = strstr((CONST char *)meta, "\"path\":\"");
				if ( m != NULL ) {
					INT	k;

					m += 8;
					for ( k = 0; m[k] != 0 && m[k] != '"' && k < 63; k++ ) where[k] = m[k];
					where[k] = 0;
				}
			}
			ob_cls_obj(key);
		}
		snprintf(disk_line[n], sizeof(disk_line[n]), "%s　%d MB　%s", (CONST char *)r.name,
			 (INT)( size >> 20 ), where);
		nm[n] = disk_line[n];
		n++;
	}
	ui_names(&ui, P_DISK_LIST, nm, n);
	ui_set_value(&ui, P_DISK_LIST, 0);
}

LOCAL void fill_video( void )
{
	char	v[CF_VAL_MAX];
	INT	len = disp_rec();

	val_of(len, "SIZE", v, sizeof(v));
	ui_set_text(&ui, P_VID_1, v);
	val_of(len, "BPP", v, sizeof(v));
	strncat(v, " ビット／画素（１６７７万色）", sizeof(v) - strlen(v) - 1);
	ui_set_text(&ui, ( P_VID_1 + 1 ), v);
	val_of(len, "DRIVER", v, sizeof(v));
	ui_set_text(&ui, ( P_VID_1 + 2 ), v);
}

LOCAL void fill_kbd( void )
{
	char	v[CF_VAL_MAX];

	ui_set_now(&ui, P_KBD_TYPE, ( dev_get("KBTYPE", v, sizeof(v)) && cf_num(v, 0x41) == 0x40 ) ? 2 : 1,
		   ( wm_look(LK_KBD_US) != 0 ) ? 2 : 1);
	ui_set_value(&ui, P_KBD_MODE, ( dev_get("KBMODE", v, sizeof(v)) && ( cf_num(v, 1) & 3 ) == 0 ) ? 1 : 2);
}

LOCAL void fill_sound( void )
{
	char	v[CF_VAL_MAX];
	INT	len = sys_rec(OB_SYS_SOUND);

	val_of(len, "OUTPUT", v, sizeof(v));
	ui_set_text(&ui, P_SND_1, v);
	val_of(len, "RATE", v, sizeof(v));
	strncat(v, " Hz", sizeof(v) - strlen(v) - 1);
	ui_set_text(&ui, ( P_SND_1 + 1 ), v);
	val_of(len, "CHANNELS", v, sizeof(v));
	ui_set_text(&ui, ( P_SND_1 + 2 ), v);
}

/* ---------------------------------------------------------------- what is done */

LOCAL void tell( CONST char *line )
{
	CONST char *l[1];

	l[0] = line;
	(void)ui_ask(&ui, PN_TELL, l, 1);
}

LOCAL void power( BOOL restart )
{
	CONST char *l[3];

	l[0] = restart ? "システムを再起動します。" : "システムを終了して電源を切ります。";
	l[1] = "開いているウィンドウは保存して閉じます。";
	l[2] = restart ? "再起動" : "電源を切る";
	if ( ui_ask(&ui, PN_POWER, l, 3) != B_POWER ) return;
	if ( sys_write(OB_SYS_POWER, restart ? "RESTART" : "OFF") < E_OK ) {
		tell("電源の操作ができません。");
		return;
	}
	closing = TRUE;
}

LOCAL void open_page( INT sheet )
{
	switch ( sheet ) {
	case S_D_SCREEN:	fill_screen();	break;
	case S_D_NET:		fill_net();	break;
	case S_D_USB:		fill_usb();	break;
	case S_D_DISK:		fill_disks();	break;
	case S_D_VIDEO:		fill_video();	break;
	case S_D_KBD:		fill_kbd();	break;
	case S_D_SOUND:		fill_sound();	break;
	default:		break;
	}
	ui_show_sheet(&ui, sheet);
}

LOCAL void screen_set( void )
{
	INT	k = ui_value(&ui, P_SCR_LIST) - 1, c = mode_cur - 1;
	char	v[64];

	if ( k < 0 || k >= nmode ) return;
	if ( k == c ) {
		ui_show_sheet(&ui, S_DEVS);
		return;
	}
	/* the size asked for, and the one there is now to go back to */
	snprintf(v, sizeof(v), "%dx%d %dx%d", mode_w[k], mode_h[k],
		 ( c >= 0 ) ? mode_w[c] : mode_w[k], ( c >= 0 ) ? mode_h[c] : mode_h[k]);
	if ( dev_put("VIDEOMODE", v) < E_OK ) {
		tell("設定した内容が保存できません。");
		return;
	}
	if ( ui_ask(&ui, PN_SCREEN, NULL, 0) == B_RESTART ) {
		power(TRUE);
		return;
	}
	ui_show_sheet(&ui, S_DEVS);
}

LOCAL void kbd_set( void )
{
	char	v[8];

	snprintf(v, sizeof(v), "0x%02x", ( ui_value(&ui, P_KBD_TYPE) == 2 ) ? 0x40 : 0x41);
	if ( dev_put("KBTYPE", v) < E_OK ) {
		tell("設定した内容が保存できません。");
		return;
	}
	snprintf(v, sizeof(v), "%d", ( ui_value(&ui, P_KBD_MODE) == 1 ) ? 0 : 1);
	(void)dev_put("KBMODE", v);
	ui_show_sheet(&ui, S_DEVS);
}

/* A picture of the wallpaper box laid now, and kept for the next start */
LOCAL void wall_set( void )
{
	INT	k = ui_value(&ui, P_WALLS) - 1, i;
	char	us[48], v[64];
	CONST char *hex = "0123456789abcdef";

	if ( k < 0 || k >= nwall ) return;
	for ( i = 0; i < 16; i++ ) {
		INT	d = i * 2 + ( i >= 4 ) + ( i >= 6 ) + ( i >= 8 ) + ( i >= 10 );

		us[d] = hex[wall_id[k].b[i] >> 4];
		us[d + 1] = hex[wall_id[k].b[i] & 15];
	}
	us[8] = us[13] = us[18] = us[23] = '-';
	us[36] = 0;
	snprintf(v, sizeof(v), "%s %d", us, ui_value(&ui, P_WALL_MODE) - 1);
	if ( sys_write(OB_SYS_WALL, v) < E_OK ) {
		tell("背景画面データは不正またはサポート範囲外です。");
		return;
	}
	(void)dev_put("WALLPAPER", v);
	wall_cur = k + 1;
	ui_set_now(&ui, P_WALLS, wall_cur, wall_cur);
}

LOCAL void scheme_set( void )
{
	INT	k = ui_value(&ui, P_SCHEMES) - 1;
	char	v[8];

	if ( k < 0 || k >= nscheme ) return;
	snprintf(v, sizeof(v), "%d", k);
	if ( sys_write(OB_SYS_SCHEME, v) < E_OK ) return;
	(void)dev_put("SCHEME", v);
	ui_set_now(&ui, P_SCHEMES, k + 1, k + 1);
}

/* ---------------------------------------------------------------- the programs */

/*
 * A request to the desktop (DT_RQ_INSTALL, DT_RQ_REMOVE), waited for:
 * what it answered, and the answer itself.
 */
LOCAL ER desk_ask( UINT req, CONST TS_UUID *target, INT recno, T_DTANS *an )
{
	T_DTREQ	rq;
	T_OBCRE	c;
	TS_UUID	d, reply;
	SZ	asz = 0;
	ID	k, ka;
	INT	i;
	ER	er;

	memset(an, 0, sizeof(*an));
	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	memset(&rq, 0, sizeof(rq));
	rq.req = req;
	rq.seq = 1;
	rq.target = *target;
	rq.recno = recno;
	rq.reply = reply;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, &rq, sizeof(rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	for ( i = 0; er >= E_OK && i < 1500; i++ ) {
		if ( ob_rea_rec(ka, 0, 0, an, sizeof(*an), &asz) >= E_OK && asz == (SZ)sizeof(*an)
		  && an->seq == rq.seq ) {
			er = an->er;
			break;
		}
		(void)tk_dly_tsk(20);
	}
	if ( i >= 1500 ) er = E_TMOUT;
	ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* A question with two buttons (panel 11): TRUE for the one that goes ahead */
LOCAL BOOL confirm( CONST char *l1, CONST char *l2, CONST char *yes )
{
	CONST char *l[3];

	l[0] = l1;
	l[1] = l2;
	l[2] = yes;
	return (BOOL)( ui_ask(&ui, PN_POWER, l, 3) == B_POWER );
}

LOCAL void prog_detail( void )
{
	INT		k = ui_value(&ui, P_PROGS) - 1;
	PROGINFO	*pi;
	char		l[6][96], us[40];
	CONST char	*lp[6];
	INT		i;

	if ( k < 0 || k >= nprog ) {
		tell("アプリケーションを選んでください。");
		return;
	}
	pi = &prog_info[k];
	for ( i = 0; i < 16; i++ ) {
		snprintf(us + i * 2 + ( i >= 4 ) + ( i >= 6 ) + ( i >= 8 ) + ( i >= 10 ), 3, "%02x",
			 prog_id[k].b[i]);
	}
	us[8] = us[13] = us[18] = us[23] = '-';
	us[36] = 0;
	snprintf(l[0], sizeof(l[0]), "名前：%s", pi->name);
	snprintf(l[1], sizeof(l[1]), "バージョン：%s", ( pi->version[0] != 0 ) ? pi->version : "（なし）");
	snprintf(l[2], sizeof(l[2]), "識別子：%s", pi->id);
	snprintf(l[3], sizeof(l[3]), "種類：%s",
		 ( strcmp(pi->kind, "accessory") == 0 ) ? "小物"
		 : ( strcmp(pi->kind, "utility") == 0 ) ? "道具" : "アプリケーション");
	snprintf(l[4], sizeof(l[4]), "実身：%s", us);
	snprintf(l[5], sizeof(l[5]), "%s", pi->builtin ? "システムに組み込み（削除できません）" : "追加したもの（削除できます）");
	for ( i = 0; i < 6; i++ ) lp[i] = l[i];
	(void)ui_ask(&ui, PN_PROG, lp, 6);
}

LOCAL void prog_delete( void )
{
	INT		k = ui_value(&ui, P_PROGS) - 1;
	T_DTANS		an;
	char		l1[128], s[128];
	ER		er;

	if ( ui_sheet(&ui) != S_VERSION || k < 0 || k >= nprog ) {
		tell("削除するアプリケーションを選んでください。");
		return;
	}
	if ( prog_info[k].builtin ) {
		tell("システムに組み込みのアプリケーションは削除できません。");
		return;
	}
	snprintf(l1, sizeof(l1), "〈%s %s〉を削除します。", prog_info[k].name, prog_info[k].version);
	if ( !confirm(l1, "削除すると元に戻せません。", "削除") ) return;
	er = desk_ask(DT_RQ_REMOVE, &prog_id[k], 0, &an);
	snprintf(s, sizeof(s), "sysenv: remove %s %d\n", prog_info[k].id, (INT)er);
	tm_putstring((CONST UB *)s);
	if ( er < E_OK ) tell("削除できませんでした。");
	fill_version();
}

/* A package dropped on the list: installed, or the program it is a newer version of updated */
LOCAL void prog_drop( void )
{
	static T_OBDROP	d;
	T_DTANS		an;
	char		s[160];
	INT		i, done = 0;
	ER		er;

	if ( ui_drop(&ui, &d) < E_OK ) return;
	if ( ui_sheet(&ui) != S_VERSION ) {
		ui_drop_answer(&ui, &d, OB_DR_REFUSE, "〈バージョン〉の一覧に落としてください");
		return;
	}
	ui_drop_answer(&ui, &d, OB_DR_ACCEPT, "");
	for ( i = 0; i < d.n && i < OB_DROP_MAX; i++ ) {
		er = desk_ask(DT_RQ_INSTALL, &d.v[i].target, 0, &an);
		if ( er == E_LIMIT ) {
			if ( !confirm("同じか、より新しいバージョンがすでにあります。",
				      "置き換えますか？", "置き換え") ) continue;
			er = desk_ask(DT_RQ_INSTALL, &d.v[i].target, 1, &an);
		}
		snprintf(s, sizeof(s), "sysenv: install %d how %d\n", (INT)er, (INT)an.how);
		tm_putstring((CONST UB *)s);
		if ( er == E_OBJ ) {
			tell("アプリケーションのパッケージではありません。");
		} else if ( er == E_PAR ) {
			tell("システムに組み込みのアプリケーションは置き換えられません。");
		} else if ( er < E_OK ) {
			tell("インストールできませんでした。");
		} else {
			done++;
		}
	}
	fill_version();
	if ( done > 0 ) {
		tell(( an.how == DT_HOW_UPDATED ) ? "アプリケーションを更新しました。"
						  : "アプリケーションをインストールしました。");
	}
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

	if ( ui_uuid(SYSDEF_MENU_SYSENV, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	{
		TS_UUID	od;
		INT	k = ui_value(&ui, P_PROGS) - 1;

		/* 削除: a program chosen on 〈バージョン〉 that is not the system's own */
		if ( ui_uuid(SYSDEF_MENU_SYSENV_OP, &od) >= E_OK ) {
			(void)mn_chg_atr(mid, &od, "prog.delete",
					 ( ui_sheet(&ui) == S_VERSION && k >= 0 && k < nprog
					   && !prog_info[k].builtin ) ? 0 : MN_GREY);
		}
	}
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		closing = TRUE;
	} else if ( is_code(&sel, "prog.delete") ) {
		prog_delete();
	} else if ( is_code(&sel, "again") ) {
		fill_power();
		fill_walls();
		fill_schemes();
		fill_fonts();
		fill_version();
	}
}

LOCAL void changed( INT id )
{
	switch ( id ) {
	case P_WALLS:
	case P_WALL_MODE:
		wall_set();
		break;
	case P_SCHEMES:
		scheme_set();
		break;
	case P_FONTS:
		font_info(ui_value(&ui, P_FONTS) - 1);
		break;
	default:
		break;
	}
}

LOCAL void pressed( INT id )
{
	switch ( id ) {
	case P_SHOW:
	{
		INT	k = ui_value(&ui, P_DEVS) - 1;

		if ( k >= 0 && k < NDEV ) open_page(dev_pages[k]);
		break;
	}
	case P_BACK:
	case P_SCR_CANCEL:
	case P_KBD_CANCEL:
		ui_show_sheet(&ui, S_DEVS);
		break;
	case P_SCR_SET:
		screen_set();
		break;
	case P_KBD_SET:
		kbd_set();
		break;
	case P_USB_AGAIN:
		fill_usb();
		break;
	case P_PROG_INFO:
		prog_detail();
		break;
	case P_RESTART:
		power(TRUE);
		break;
	case P_OFF:
		power(FALSE);
		break;
	default:
		break;
	}
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	INT	id, ev;

	ui_icon(SYSDEF_PROG_SYSENV);
	if ( ui_open(&ui, SYSDEF_BOX_SYSENV, PN_MAIN, 160, 80) < E_OK ) {
		tm_putstring((CONST UB *)"sysenv: no window\n");
		return 1;
	}
	ui_names(&ui, P_DEVS, dev_names, NDEV);
	ui_set_value(&ui, P_DEVS, 1);
	fill_power();
	fill_walls();
	fill_schemes();
	fill_fonts();
	fill_version();
	tm_putstring((CONST UB *)"sysenv: ready\n");

	while ( !closing ) {
		while ( !closing && ob_rea_rec(ui.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				closing = TRUE;
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_DROP:
				prog_drop();
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
			case UI_EV_BUTTON:
				pressed(id);
				break;
			default:
				break;
			}
		}
		if ( !closing ) {
			(void)tk_dly_tsk(TICK_MS);
		}
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"sysenv: closed\n");
	return 0;
}
