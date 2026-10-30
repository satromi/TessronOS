/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sysinst.c
 *	システム登録: the window of the accessory that puts the system the
 *	PC was started from on an internal disk (si_core.c,
 *	docs/tessronos/03-platform.md 3.10.10, 11-desktop.md 11.32)
 *
 *	A window of the desktop's like ディスク修復's (DTOWN). 〈写し先の
 *	ディスク〉 lists the NVMe and SATA disks but the one the system runs
 *	from (USB disks are never offered), each with its size, its
 *	partitions and its largest free stretch. 〈登録の方法〉 is Windows
 *	と共存する (the default), 区画を1つ消して使う or ディスク全体を消して
 *	使う; for the second, 〈消す区画〉 lists the partitions of the disk
 *	that can go (si_replaceable: never the ESP, the Microsoft reserved
 *	one nor Windows recovery). [開始] looks at the way chosen (si_plan)
 *	and says why when it cannot be taken; else it asks first -- with
 *	every partition that is to go and its size -- then for the
 *	administrator's password, and runs
 *	si_run in a task of its own, the window following the steps and the
 *	bytes. At the end it says how the machine is started from the disk,
 *	and [再起動] starts it again.
 *
 *	Each step is said on the console, "sysinst: ...", for the tests.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/uuid.h>
#include <ts/sysdef.h>
#include "../desktop/desktop.h"

#ifdef X64

#include "si_core.h"

#define KEY_ENTER	0x28
#define KEY_ESCAPE	0x29

#define SW_W		620		/* the window's panel */
#define SW_H		640
#define SW_ROWS		9		/* lines of what is under way and what came of it */
#define SW_ROW0		396
#define SW_ROW_H	22
#define SW_STACK	(64 * 1024)
#define SW_PRI		22		/* the task: below the desktop's */
#define ASK_ROWS	16

#define PN_DISK		1
#define PN_PART		2
#define PN_COEXIST	20
#define PN_ERASE	21
#define PN_REPLACE	22
#define ANS_START	( WM_ANS_ACT + 1 )
#define ANS_END		( WM_ANS_ACT + 2 )
#define ANS_RESTART	( WM_ANS_ACT + 3 )

#define SW_SETUP	0
#define SW_RUN		1
#define SW_DONE		2

LOCAL DTWIN		*sw_d = NULL;
LOCAL INT		sw_wid = -1, sw_pid = -1;
LOCAL CONST DTOWN	sw_own;

LOCAL T_SIDISK		*sw_disk = NULL;	/* SI_DISK_MAX of them */
LOCAL INT		sw_ndisk = 0;
LOCAL INT		sw_sel = 0;
LOCAL INT		sw_mode = SI_COEXIST;
LOCAL INT		sw_pmap[SI_PART_MAX];	/* the rows of 〈消す区画〉: indices in the disk's parts */
LOCAL INT		sw_npmap = 0;
LOCAL INT		sw_psel = 0;		/* the row chosen */
LOCAL INT		sw_state = SW_SETUP;
LOCAL T_SIPLAN		sw_plan;
LOCAL T_SIRES		sw_res;
LOCAL UB		sw_line[SW_ROWS][WM_LABEL_MAX];

/* shared with the task */
LOCAL ID		sw_mtx = 0;
LOCAL INT		sw_step = 0;
LOCAL UD		sw_done_b = 0, sw_total_b = 0;
LOCAL BOOL		sw_over = FALSE;
LOCAL ER		sw_er = E_OK;
LOCAL INT		sw_shown_step = -1, sw_shown_pc = -1;

/* ---------------------------------------------------------------- words */

LOCAL INT put( UB *b, INT n, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && n < max - 1 ) {
		b[n++] = (UB)*s++;
	}
	while ( n > 0 && s != NULL && *s != 0 && ( (UB)*s & 0xC0 ) == 0x80 ) {
		s--;
		n--;
	}
	b[n] = 0;
	return n;
}

LOCAL INT put_num( UB *b, INT n, INT max, D v )
{
	char	t[24];
	INT	k = 0;
	UD	u = ( v < 0 ) ? (UD)-v : (UD)v;

	do {
		t[k++] = (char)( '0' + u % 10 );
		u /= 10;
	} while ( u > 0 && k < 22 );
	if ( v < 0 ) t[k++] = '-';
	while ( k > 0 && n < max - 1 ) {
		b[n++] = (UB)t[--k];
	}
	b[n] = 0;
	return n;
}

/* A size in bytes, as MB below a gigabyte and as GB with one place above */
LOCAL INT put_size( UB *b, INT n, INT max, UD bytes )
{
	if ( bytes < ( 1ULL << 30 ) ) {
		n = put_num(b, n, max, (D)( bytes >> 20 ));
		return put(b, n, max, "MB");
	}
	n = put_num(b, n, max, (D)( bytes >> 30 ));
	n = put(b, n, max, ".");
	n = put_num(b, n, max, (D)( ( bytes % ( 1ULL << 30 ) ) * 10 >> 30 ));
	return put(b, n, max, "GB");
}

LOCAL void line_set( INT i, CONST char *s )
{
	if ( i >= 0 && i < SW_ROWS ) {
		(void)put(sw_line[i], 0, WM_LABEL_MAX, s);
	}
}

LOCAL void lines_clear( void )
{
	INT	i;

	for ( i = 0; i < SW_ROWS; i++ ) sw_line[i][0] = 0;
}

LOCAL CONST char *step_name( INT st )
{
	switch ( st ) {
	case SI_ST_CHECK:	return "ディスクを確かめています";
	case SI_ST_TABLE:	return "区画を作っています";
	case SI_ST_COPY:	return "システムボリュームを写しています";
	case SI_ST_VERIFY:	return "写したものを読み返しています";
	case SI_ST_GROW:	return "ボリュームを区画の大きさに広げています";
	case SI_ST_ESP:		return "EFIシステム区画にローダとカーネルを置いています";
	case SI_ST_ENTRY:	return "起動の項目の登録を依頼しています";
	default:		return "終わりました";
	}
}

LOCAL CONST char *why_text( INT why )
{
	switch ( why ) {
	case SI_R_NODISK:	return "このディスクは読めないか、512バイトのセクタのディスクではありません。";
	case SI_R_NOGPT:	return "このディスクにはGPTの区画表がありません。〈ディスク全体を消して使う〉を選んでください。";
	case SI_R_KEEP:		return "消す区画を〈消す区画〉の一覧から選んでください。";
	case SI_R_NOESP:	return "このディスクにはEFIシステム区画がありません。";
	case SI_R_NOROOM:	return ( sw_mode == SI_REPLACE ) ? "選んだ区画は、システムボリュームより小さすぎます。"
					: "システムボリュームより大きな空き領域がありません。Windowsのディスクの管理でボリュームを縮めてください。";
	case SI_R_ESPFULL:	return "EFIシステム区画にローダとカーネルを置く空きがありません。";
	case SI_R_SMALL:	return "このディスクはEFIシステム区画とシステムボリュームを置くには小さすぎます。";
	case SI_R_INUSE:	return "このディスクの区画が使用中です。ディスク操作で切り離してください。";
	case SI_R_NOSYS:	return "起動したディスクにシステムボリュームか、ローダとカーネルが見つかりません。";
	case SI_R_BOOTDISK:	return "システムが動いているディスクには登録できません。";
	default:		return "";
	}
}

/* A disk as a row of the list */
LOCAL void disk_row( CONST T_SIDISK *dk, UB *b, INT max )
{
	INT	n;

	n = put(b, 0, max, (CONST char *)dk->name);
	n = put(b, n, max, "  ");
	n = put_size(b, n, max, dk->nsect * 512);
	n = put(b, n, max, "  ");
	if ( dk->gpt ) {
		n = put(b, n, max, "区画");
		n = put_num(b, n, max, dk->nparts);
	} else {
		n = put(b, n, max, dk->blank ? "空" : "GPTなし");
	}
	if ( dk->free_last > dk->free_first ) {
		n = put(b, n, max, "  空き");
		n = put_size(b, n, max, ( dk->free_last - dk->free_first + 1 ) * 512);
	}
	if ( dk->tsfs >= 0 ) n = put(b, n, max, "  TessronOSあり");
	if ( dk->bitlocker ) (void)put(b, n, max, "  BitLocker");
}

/* A partition as a row of 〈消す区画〉 */
LOCAL void part_row( CONST T_SIDISK *dk, CONST T_SIPART *p, UB *b, INT max )
{
	INT	n;

	n = put(b, 0, max, (CONST char *)dk->name);
	n = put_num(b, n, max, p->unit);
	n = put(b, n, max, "  ");
	n = put(b, n, max, si_kind_name(p->kind));
	n = put(b, n, max, "  ");
	n = put_size(b, n, max, ( p->last - p->first + 1 ) * 512);
	if ( p->name[0] != 0 ) {
		n = put(b, n, max, "  「");
		n = put(b, n, max, (CONST char *)p->name);
		n = put(b, n, max, "」");
	}
	if ( p->bitlocker ) (void)put(b, n, max, "  BitLocker");
}

/* The partitions of the disk chosen that can go; the largest Windows volume chosen first */
LOCAL void part_map( void )
{
	CONST T_SIDISK	*dk;
	INT		i, best = -1;
	UD		sz, bsz = 0;

	sw_npmap = 0;
	sw_psel = 0;
	if ( sw_disk == NULL || sw_sel >= sw_ndisk ) {
		return;
	}
	dk = &sw_disk[sw_sel];
	for ( i = 0; i < dk->nparts && sw_npmap < SI_PART_MAX; i++ ) {
		if ( !si_replaceable(&dk->parts[i]) ) {
			continue;
		}
		sz = dk->parts[i].last - dk->parts[i].first + 1;
		if ( dk->parts[i].kind == SI_K_BASIC && sz > bsz ) {
			bsz = sz;
			best = sw_npmap;
		}
		sw_pmap[sw_npmap++] = i;
	}
	if ( best >= 0 ) sw_psel = best;
}

/* What the window says before anything is started */
LOCAL void setup_lines( void )
{
	lines_clear();
	line_set(0, "写し先のディスクと登録の方法を選んで、[開始]を押してください。");
	line_set(1, "いま動いているシステムボリューム(実身、設定、入れたアプリ)と、");
	line_set(2, "起動したEFIシステム区画のローダ、カーネル、起動の設定を写します。");
	line_set(3, "起動したディスクとUSBのディスクは、写し先に出しません。");
	line_set(5, "BitLockerやデバイスの暗号化を使っているときは、回復キーを控えてください。");
	line_set(6, "セキュアブートを切るとWindowsは回復キーを求めます(EFIシステム区画に");
	line_set(7, "ファイルを足すことは差し支えありません)。");
	if ( sw_ndisk == 0 ) line_set(4, "写し先にできる内蔵のディスクがありません。");
}

LOCAL void running_lines( void )
{
	UB	b[WM_LABEL_MAX];
	INT	n, st, pc;
	UD	done, total;

	tk_loc_mtx(sw_mtx, TMO_FEVR);
	st = sw_step;
	done = sw_done_b;
	total = sw_total_b;
	tk_unl_mtx(sw_mtx);
	lines_clear();
	n = put(b, 0, sizeof(b), ( sw_plan.mode == SI_ERASE ) ? "ディスク全体を使って登録しています: "
				 : ( sw_plan.mode == SI_REPLACE ) ? "区画を消して登録しています: "
				 : "Windowsと共存して登録しています: ");
	(void)put(b, n, sizeof(b), (CONST char *)sw_plan.disk);
	line_set(0, (CONST char *)b);
	n = put(b, 0, sizeof(b), "手順: ");
	(void)put(b, n, sizeof(b), step_name(st));
	line_set(1, (CONST char *)b);
	if ( total > 0 && ( st == SI_ST_COPY || st == SI_ST_ESP ) ) {
		pc = (INT)( done * 100 / total );
		n = put_size(b, 0, sizeof(b), done);
		n = put(b, n, sizeof(b), " / ");
		n = put_size(b, n, sizeof(b), total);
		n = put(b, n, sizeof(b), "  (");
		n = put_num(b, n, sizeof(b), pc);
		(void)put(b, n, sizeof(b), "%)");
		line_set(2, (CONST char *)b);
	} else if ( total > 0 && st == SI_ST_VERIFY ) {
		n = put_num(b, 0, sizeof(b), (D)( done * 100 / total ));
		(void)put(b, n, sizeof(b), "%");
		line_set(2, (CONST char *)b);
	}
	line_set(4, "写している間は、ほかの操作をしないでください。");
	line_set(5, "システムボリュームへの書込みは、写し終わるまで待たされます。");
}

LOCAL void result_lines( void )
{
	UB	b[WM_LABEL_MAX];
	INT	n;

	lines_clear();
	if ( sw_er < E_OK ) {
		n = put(b, 0, sizeof(b), "登録できませんでした(");
		n = put(b, n, sizeof(b), step_name(sw_res.failed_step));
		n = put(b, n, sizeof(b), "の途中、");
		n = put_num(b, n, sizeof(b), sw_er);
		(void)put(b, n, sizeof(b), ")。");
		line_set(0, (CONST char *)b);
		if ( sw_er == E_BUSY ) line_set(1, "ディスクかシステムボリュームが使用中でした。");
		if ( sw_plan.mode == SI_ERASE && sw_res.failed_step >= SI_ST_TABLE ) {
			line_set(2, "ディスクの元の区画は消えています。もう一度登録してください。");
		}
		if ( sw_plan.mode == SI_REPLACE && sw_res.failed_step >= SI_ST_TABLE ) {
			line_set(2, "選んだ区画の中身は消えています。もう一度登録してください。");
		}
		return;
	}
	n = put(b, 0, sizeof(b), "登録が終わりました: ");
	n = put(b, n, sizeof(b), ( sw_res.part[0] != 0 ) ? (CONST char *)sw_res.part : (CONST char *)sw_plan.disk);
	n = put(b, n, sizeof(b), "に");
	n = put_num(b, n, sizeof(b), (D)( sw_res.copied * 4096 >> 20 ));
	(void)put(b, n, sizeof(b), "MBを写しました。");
	line_set(0, (CONST char *)b);
	if ( sw_res.grow_later ) {
		line_set(1, "ボリュームは、内蔵のディスクから最初に起動したときに区画の大きさに広がります。");
	} else if ( sw_res.vol_to > sw_res.vol_from ) {
		n = put(b, 0, sizeof(b), "ボリュームを");
		n = put_size(b, n, sizeof(b), sw_res.vol_from);
		n = put(b, n, sizeof(b), "から");
		n = put_size(b, n, sizeof(b), sw_res.vol_to);
		(void)put(b, n, sizeof(b), "に広げました。");
		line_set(1, (CONST char *)b);
	}
	if ( sw_res.entry_asked ) {
		line_set(2, "[再起動]を押し、もう一度このmicroSD(USBメモリ)から起動してください。");
		line_set(3, "ローダが起動の項目「TessronOS」を登録し、内蔵ディスクから起動し直します。");
	} else {
		line_set(2, "起動の項目を登録できませんでした。ファームウェアの起動のメニューで選んでください。");
	}
	line_set(4, "Surface Go 2: 音量を上げるボタンと電源ボタンでUEFIに入り、起動先を選びます。");
	line_set(5, "ThinkPad X1 Yoga: 起動のときにF12を押して、起動のメニューから選びます。");
	line_set(6, si_entry_first(&sw_plan)
		    ? "登録の後は、microSDを抜くと内蔵のディスクから起動します。"
		    : "登録の後は、起動のメニューの「TessronOS」で起動します(既定はWindowsのまま)。");
}

/* ---------------------------------------------------------------- the task */

LOCAL void progress( void *ctx, INT step, UD done, UD total )
{
	(void)ctx;
	tk_loc_mtx(sw_mtx, TMO_FEVR);
	sw_step = step;
	sw_done_b = done;
	sw_total_b = total;
	tk_unl_mtx(sw_mtx);
}

LOCAL void worker( INT stacd, void *exinf )
{
	T_SIRES	r;
	ER	er;

	(void)stacd;
	(void)exinf;
	er = si_run(&sw_plan, progress, NULL, &r);
	tk_loc_mtx(sw_mtx, TMO_FEVR);
	sw_res = r;
	sw_er = er;
	sw_over = TRUE;
	tk_unl_mtx(sw_mtx);
	tk_exd_tsk();
}

/* ---------------------------------------------------------------- the window */

LOCAL T_WMPART *choice( T_WMPANEL *def, INT num, INT group, INT x, INT y, INT w, CONST char *label,
			BOOL on, BOOL off )
{
	T_WMPART	*pt = dt_pn_add(def, WM_PT_CHOICE | ( off ? P_DISABLE : 0 ), num, x, y, w, 22, label);

	if ( pt != NULL ) {
		pt->group = group;
		pt->value = on ? 1 : 0;
	}
	return pt;
}

LOCAL T_WMPART *button( T_WMPANEL *def, INT num, UINT ans, INT x, INT y, INT w, CONST char *label, BOOL off )
{
	T_WMPART	*pt = dt_pn_add(def, WM_PT_BUTTON | ( off ? P_DISABLE : 0 ), num, x, y, w, 26, label);

	if ( pt != NULL ) {
		pt->answer = ans;
	}
	return pt;
}

LOCAL void keep_choices( void )
{
	INT	i;

	if ( sw_pid < 0 || sw_state != SW_SETUP ) {
		return;
	}
	i = dt_pn_value(sw_pid, PN_DISK);
	if ( i >= 1 && i <= sw_ndisk && i - 1 != sw_sel ) {
		sw_sel = i - 1;
		part_map();
	} else if ( sw_mode == SI_REPLACE ) {
		i = dt_pn_value(sw_pid, PN_PART);
		if ( i >= 1 && i <= sw_npmap ) sw_psel = i - 1;
	}
	sw_mode = ( dt_pn_value(sw_pid, PN_ERASE) != 0 ) ? SI_ERASE
		: ( dt_pn_value(sw_pid, PN_REPLACE) != 0 ) ? SI_REPLACE : SI_COEXIST;
}

LOCAL T_WMPANEL *build( void )
{
	T_WMPANEL	*def = dt_pn_new(SW_W, SW_H);
	BOOL		busy = (BOOL)( sw_state != SW_SETUP );
	UB		b[WM_LABEL_MAX];
	T_WMPART	*lst;
	INT		i, k, at = -1, cnt = 0;

	if ( def == NULL ) {
		return NULL;
	}
	def->kind = WM_PNL_BARE;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 6, 300, 22, "写し先のディスク");
	lst = dt_pn_add(def, WM_PT_LIST | ( busy ? P_DISABLE : 0 ), PN_DISK, 14, 32, SW_W - 28, 110, NULL);
	for ( i = 0; i < sw_ndisk; i++ ) {
		disk_row(&sw_disk[i], b, sizeof(b));
		k = wm_panel_name(def, b);
		if ( k < 0 ) break;
		if ( i == 0 ) at = k;
		cnt++;
	}
	if ( lst != NULL ) {
		lst->pool = ( at >= 0 ) ? at : 0;
		lst->count = cnt;
		lst->top = 1;
		lst->value = ( sw_sel < cnt ) ? sw_sel + 1 : 0;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 152, 300, 22, "登録の方法");
	(void)choice(def, PN_COEXIST, PN_COEXIST, 14, 178, 460, "Windowsと共存する(空き領域に区画を作る)",
		     (BOOL)( sw_mode == SI_COEXIST ), busy);
	(void)choice(def, PN_REPLACE, PN_COEXIST, 14, 204, 460, "区画を1つ消して使う(EFIシステムと回復の区画は残す)",
		     (BOOL)( sw_mode == SI_REPLACE ), busy);
	(void)choice(def, PN_ERASE, PN_COEXIST, 14, 230, 460, "ディスク全体を消して使う", (BOOL)( sw_mode == SI_ERASE ),
		     busy);
	(void)button(def, ANS_START, ANS_START, SW_W - 114, 230, 100, "開始", (BOOL)( busy || sw_ndisk == 0 ));
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 262, 300, 22, "消す区画");
	lst = dt_pn_add(def, WM_PT_LIST | ( ( busy || sw_mode != SI_REPLACE ) ? P_DISABLE : 0 ), PN_PART, 14, 288,
			SW_W - 28, 90, NULL);
	at = -1;
	cnt = 0;
	for ( i = 0; i < sw_npmap && sw_sel < sw_ndisk; i++ ) {
		part_row(&sw_disk[sw_sel], &sw_disk[sw_sel].parts[sw_pmap[i]], b, sizeof(b));
		k = wm_panel_name(def, b);
		if ( k < 0 ) break;
		if ( i == 0 ) at = k;
		cnt++;
	}
	if ( lst != NULL ) {
		lst->pool = ( at >= 0 ) ? at : 0;
		lst->count = cnt;
		lst->top = 1;
		lst->value = ( sw_psel < cnt ) ? sw_psel + 1 : 0;
	}
	dt_pn_add(def, WM_PT_LINE, 0, 14, SW_ROW0 - 10, SW_W - 28, 1, NULL);
	for ( i = 0; i < SW_ROWS; i++ ) {
		T_WMPART	*pt = dt_pn_add(def, WM_PT_LABEL, 0, 14, SW_ROW0 + i * SW_ROW_H, SW_W - 28, 20, NULL);

		if ( pt != NULL ) dt_pn_text(pt->label, WM_LABEL_MAX, (CONST char *)sw_line[i]);
	}
	(void)button(def, ANS_RESTART, ANS_RESTART, 14, SW_H - 36, 120, "再起動",
		     (BOOL)( sw_state != SW_DONE || sw_er < E_OK ));
	(void)button(def, ANS_END, ANS_END, SW_W - 114, SW_H - 36, 100, "終了", (BOOL)( sw_state == SW_RUN ));
	return def;
}

LOCAL void rebuild( BOOL keep )
{
	T_WMPANEL	*def;
	INT		pw, ph;

	if ( sw_wid < 0 ) {
		return;
	}
	if ( keep ) keep_choices();
	def = build();
	if ( def == NULL ) {
		return;
	}
	pw = def->r.right - def->r.left;
	ph = def->r.bottom - def->r.top;
	if ( sw_pid >= 0 ) (void)wm_panel_close(sw_pid);
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = pw;
	def->r.bottom = ph;
	sw_pid = wm_panel_open(sw_wid, def);
	Kfree(def);
	if ( sw_pid >= 0 ) dt_draw(sw_d);
	wm_update();
}

/* The window: the work area a panel's size, framed, a little left of and above the middle */
LOCAL DTWIN *open_framed( INT pw, INT ph )
{
	T_DPRECT	o;
	T_WMWIN		w;
	DTWIN		*d;
	INT		sw = 0, sh = 0, wl;

	wm_screen(&sw, &sh);
	wl = ( sw - pw ) / 2 - 2;
	o.left = wl - 10;
	o.top = ( sh - ph ) / 2 - 20;
	if ( o.top < 0 ) o.top = 0;
	o.right = o.left + pw + 26;
	o.bottom = o.top + ph + 50;
	d = dt_own_open(&sw_own, NULL, &o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE, "システム登録",
			"system-install");
	if ( d == NULL ) {
		return NULL;
	}
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		o.left = wl - ( w.work.left - w.outer.left );
		o.top = w.outer.top;
		if ( o.left < 0 ) o.left = 0;
		o.right = o.left + ( w.outer.right - w.outer.left ) + pw - ( w.work.right - w.work.left );
		o.bottom = o.top + ( w.outer.bottom - w.outer.top ) + ph - ( w.work.bottom - w.work.top );
		(void)wm_move(d->wid, &o);
	}
	return d;
}

/* ---------------------------------------------------------------- asking */

/* A question over the window, a line to a row: TRUE for the button that agrees */
LOCAL BOOL ask( UB (*l)[WM_LABEL_MAX], INT n, CONST char *yes, BOOL warn )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_WMEV		ev;
	INT		pid, pw = 600, ph = 60 + n * 22, i;
	UINT		ans = WM_ANS_NONE;

	def = dt_pn_new(pw, ph);
	if ( def == NULL ) {
		return FALSE;
	}
	def->kind = warn ? WM_PNL_WARN : WM_PNL_PLAIN;
	for ( i = 0; i < n; i++ ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 14, 10 + i * 22, pw - 28, 20, (CONST char *)l[i]);
	}
	pt = dt_pn_add(def, WM_PT_BUTTON, DT_PN_CANCEL, pw - 232, ph - 36, 94, 26, "取消");
	if ( pt != NULL ) pt->answer = WM_ANS_CANCEL;
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, pw - 128, ph - 36, 114, 26, yes);
	if ( pt != NULL ) pt->answer = WM_ANS_OK;
	pid = wm_panel_open_centre(sw_wid, def);
	Kfree(def);
	if ( pid < 0 ) {
		return FALSE;
	}
	wm_focus(sw_wid);
	wm_panel_draw(pid);
	wm_update();
	tm_printf((UB *)"sysinst: ask %s\n", yes);
	while ( ans == WM_ANS_NONE ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ESCAPE ) {
			ans = WM_ANS_CANCEL;
			break;
		}
		if ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) {
			ev.wid = sw_wid;
		}
		if ( wm_panel_event(pid, &ev, &ans) < E_OK ) {
			ans = WM_ANS_CANCEL;
		}
		wm_update();
	}
	(void)wm_panel_close(pid);
	if ( sw_pid >= 0 ) wm_panel_draw(sw_pid);
	wm_update();
	tm_printf((UB *)"sysinst: answered %d\n", ( ans == WM_ANS_OK ) ? 1 : 0);
	return (BOOL)( ans == WM_ANS_OK );
}

/* What is asked before the work starts: what will be written, and what will go */
LOCAL BOOL confirm( CONST T_SIDISK *dk )
{
	UB	l[ASK_ROWS][WM_LABEL_MAX];
	INT	n = 0, k, i;

	if ( sw_plan.mode == SI_REPLACE ) {
		CONST T_SIPART	*p = &sw_plan.lost[0];

		(void)put(l[n++], 0, WM_LABEL_MAX, "次の区画の中身を消し、TessronOSの区画にします:");
		k = put(l[n], 0, WM_LABEL_MAX, "  ");
		part_row(dk, p, l[n] + k, WM_LABEL_MAX - k);
		n++;
		if ( sw_plan.ts_last > p->last ) {
			k = put(l[n], 0, WM_LABEL_MAX, "後ろの空き領域も合わせて");
			k = put_size(l[n], k, WM_LABEL_MAX, ( sw_plan.ts_last - sw_plan.ts_first + 1 ) * 512);
			(void)put(l[n], k, WM_LABEL_MAX, "の区画にします。");
			n++;
		}
		(void)put(l[n++], 0, WM_LABEL_MAX, "EFIシステム、Microsoft予約、Windows回復の区画と、ほかの区画は残します。");
		(void)put(l[n++], 0, WM_LABEL_MAX, "EFIシステム区画には\\EFI\\TESSRON\\だけを足します。");
		if ( p->kind == SI_K_BASIC ) {
			(void)put(l[n++], 0, WM_LABEL_MAX, "Windowsの区画を消すと、Windowsは起動しなくなります。");
		}
		(void)put(l[n++], 0, WM_LABEL_MAX, "消したものは元に戻せません。");
	} else if ( sw_plan.mode == SI_COEXIST ) {
		k = put(l[n], 0, WM_LABEL_MAX, (CONST char *)sw_plan.disk);
		if ( sw_plan.ts_unit >= 0 ) {
			k = put(l[n], k, WM_LABEL_MAX, "にあるTessronOSの区画(");
		} else {
			k = put(l[n], k, WM_LABEL_MAX, "の空き領域に、TessronOSの区画(");
		}
		k = put_size(l[n], k, WM_LABEL_MAX, ( sw_plan.ts_last - sw_plan.ts_first + 1 ) * 512);
		(void)put(l[n], k, WM_LABEL_MAX, sw_plan.ts_unit >= 0 ? ")を書き直します。" : ")を作ります。");
		n++;
		(void)put(l[n++], 0, WM_LABEL_MAX, "EFIシステム区画には\\EFI\\TESSRON\\だけを足します。Windowsのファイルと、");
		(void)put(l[n++], 0, WM_LABEL_MAX, "ほかの区画の中身は変えません。");
		if ( dk->bitlocker ) {
			(void)put(l[n++], 0, WM_LABEL_MAX, "BitLockerの区画があります。回復キーを控えてから進めてください。");
		}
	} else {
		k = put(l[n], 0, WM_LABEL_MAX, (CONST char *)sw_plan.disk);
		k = put(l[n], k, WM_LABEL_MAX, "(");
		k = put_size(l[n], k, WM_LABEL_MAX, sw_plan.nsect * 512);
		(void)put(l[n], k, WM_LABEL_MAX, ")の中身をすべて消し、TessronOSだけのディスクにします。");
		n++;
		if ( sw_plan.nlost == 0 ) {
			(void)put(l[n++], 0, WM_LABEL_MAX, "このディスクに区画はありません。");
		} else {
			(void)put(l[n++], 0, WM_LABEL_MAX, "次の区画が消えます:");
		}
		for ( i = 0; i < sw_plan.nlost && n < ASK_ROWS - 2; i++ ) {
			CONST T_SIPART	*p = &sw_plan.lost[i];

			k = put(l[n], 0, WM_LABEL_MAX, "  ");
			k = put_num(l[n], k, WM_LABEL_MAX, p->unit + 1);
			k = put(l[n], k, WM_LABEL_MAX, ". ");
			k = put(l[n], k, WM_LABEL_MAX, si_kind_name(p->kind));
			k = put(l[n], k, WM_LABEL_MAX, "  ");
			k = put_size(l[n], k, WM_LABEL_MAX, ( p->last - p->first + 1 ) * 512);
			if ( p->name[0] != 0 ) {
				k = put(l[n], k, WM_LABEL_MAX, "  「");
				k = put(l[n], k, WM_LABEL_MAX, (CONST char *)p->name);
				k = put(l[n], k, WM_LABEL_MAX, "」");
			}
			if ( p->bitlocker ) (void)put(l[n], k, WM_LABEL_MAX, "  BitLocker");
			n++;
		}
		if ( i < sw_plan.nlost ) {
			k = put(l[n], 0, WM_LABEL_MAX, "  ほかに");
			k = put_num(l[n], k, WM_LABEL_MAX, sw_plan.nlost - i);
			(void)put(l[n], k, WM_LABEL_MAX, "区画");
			n++;
		}
		(void)put(l[n++], 0, WM_LABEL_MAX, "消したものは元に戻せません。");
	}
	return ask(l, n, ( sw_plan.mode == SI_COEXIST ) ? "登録" : "消して登録", (BOOL)( sw_plan.mode != SI_COEXIST ));
}

/* ---------------------------------------------------------------- starting */

LOCAL void list_disks( void )
{
	if ( sw_disk == NULL ) {
		sw_disk = (T_SIDISK *)Kmalloc(sizeof(T_SIDISK) * SI_DISK_MAX);
		if ( sw_disk == NULL ) {
			sw_ndisk = 0;
			return;
		}
	}
	sw_ndisk = si_disks(sw_disk, SI_DISK_MAX);
	if ( sw_ndisk < 0 ) sw_ndisk = 0;
	if ( sw_sel >= sw_ndisk ) sw_sel = 0;
	part_map();
	tm_printf((UB *)"sysinst: %d disks\n", sw_ndisk);
}

LOCAL void restart( void )
{
	ID	key;
	ER	er = E_OK;

	tm_printf((UB *)"sysinst: restart\n");
	key = ob_opn_obj(&ob_uuid_system, OB_OP_WRITE);
	if ( key > 0 ) {
		er = ob_wri_rec(key, OB_SYS_POWER, 0, "RESTART", 7, NULL);
		(void)ob_cls_obj(key);
	}
	if ( key <= 0 || er < E_OK ) {
		(void)wm_msg_put("システム登録: 再起動できませんでした");
	}
}

LOCAL void start( void )
{
	T_CTSK	ct;
	ID	tid;
	INT	i, unit;

	keep_choices();
	if ( sw_state == SW_RUN || sw_ndisk == 0 ) {
		return;
	}
	unit = ( sw_mode == SI_REPLACE && sw_psel < sw_npmap ) ? sw_disk[sw_sel].parts[sw_pmap[sw_psel]].unit : -1;
	if ( si_plan((CONST char *)sw_disk[sw_sel].name, sw_mode, unit, &sw_plan) < E_OK ) {
		(void)wm_msg_put("システム登録: メモリが足りません");
		return;
	}
	tm_printf((UB *)"sysinst: plan %s %s %d: %d\n", sw_plan.disk, si_mode_name(sw_mode), unit, sw_plan.why);
	if ( sw_plan.why != SI_R_OK ) {
		lines_clear();
		line_set(0, why_text(sw_plan.why));
		rebuild(FALSE);
		return;
	}
	if ( !confirm(&sw_disk[sw_sel]) ) {
		return;
	}
	if ( dt_admin_self("システム登録", (CONST UB *)"内蔵のディスクにTessronOSを登録する") < E_OK ) {
		return;
	}
	if ( sw_mtx <= 0 ) {
		T_CMTX	cm;

		cm.exinf = NULL;
		cm.mtxatr = TA_TFIFO;
		sw_mtx = tk_cre_mtx(&cm);
		if ( sw_mtx <= 0 ) return;
	}
	sw_step = SI_ST_CHECK;
	sw_done_b = sw_total_b = 0;
	sw_over = FALSE;
	sw_er = E_OK;
	sw_shown_step = sw_shown_pc = -1;
	for ( i = 0; i < SW_ROWS; i++ ) sw_line[i][0] = 0;

	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)worker;
	ct.itskpri = SW_PRI;
	ct.stksz = SW_STACK;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		(void)wm_msg_put("システム登録: 始められませんでした");
		return;
	}
	sw_state = SW_RUN;
	tm_printf((UB *)"sysinst: started %s\n", sw_plan.disk);
	running_lines();
	rebuild(FALSE);
}

/* The task followed from the desktop's loop: the window made again when the step or the tenth changes */
LOCAL void sw_poll( DTWIN *d )
{
	BOOL	over;
	INT	st, pc;
	UD	done, total;

	(void)d;
	if ( sw_state != SW_RUN || sw_mtx <= 0 ) {
		return;
	}
	tk_loc_mtx(sw_mtx, TMO_FEVR);
	over = sw_over;
	st = sw_step;
	done = sw_done_b;
	total = sw_total_b;
	tk_unl_mtx(sw_mtx);
	if ( over ) {
		sw_state = SW_DONE;
		result_lines();
		rebuild(FALSE);
		tm_printf((UB *)"sysinst: over %d\n", (INT)sw_er);
		return;
	}
	pc = ( total > 0 ) ? (INT)( done * 10 / total ) : 0;
	if ( st != sw_shown_step || pc != sw_shown_pc ) {
		sw_shown_step = st;
		sw_shown_pc = pc;
		running_lines();
		rebuild(FALSE);
	}
}

/* ---------------------------------------------------------------- the calls */

LOCAL void sw_close( DTWIN *d )
{
	(void)d;
	if ( sw_pid >= 0 ) (void)wm_panel_close(sw_pid);
	sw_d = NULL;
	sw_wid = sw_pid = -1;
	if ( sw_state != SW_RUN ) sw_state = SW_SETUP;
	tm_printf((UB *)"sysinst: closed\n");
}

/* Not while the work runs: it runs to its end */
LOCAL BOOL sw_closing( DTWIN *d )
{
	(void)d;
	if ( sw_state == SW_RUN ) {
		(void)wm_msg_put("システム登録: 登録が終わるまで閉じられません");
		return FALSE;
	}
	return TRUE;
}

EXPORT void dt_sysinst_open( void )
{
	if ( sw_wid >= 0 ) {
		wm_raise(sw_wid);
		wm_focus(sw_wid);
		wm_update();
		return;
	}
	if ( sw_state != SW_RUN ) {
		list_disks();
		sw_state = SW_SETUP;
		sw_mode = SI_COEXIST;
		setup_lines();
	}
	sw_d = open_framed(SW_W + 2 * DT_PN_IN, SW_H + 2 * DT_PN_IN);
	if ( sw_d == NULL ) {
		return;
	}
	sw_wid = sw_d->wid;
	sw_pid = -1;
	if ( sw_state == SW_RUN ) running_lines();
	rebuild(FALSE);
	tm_printf((UB *)"sysinst: opened %d\n", sw_ndisk);
}

LOCAL void sw_draw( DTWIN *d )
{
	dt_own_ground(d);
	if ( sw_pid >= 0 ) wm_panel_draw(sw_pid);
	wm_update();
}

LOCAL BOOL sw_input( DTWIN *d, CONST T_WMEV *ev )
{
	UINT	ans = WM_ANS_NONE;

	(void)d;
	if ( sw_pid < 0 ) {
		return FALSE;
	}
	if ( ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP )
	  && ( ev->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) {
		return FALSE;			/* the menu's letters */
	}
	if ( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP ) && ev->code != 0 ) {
		return TRUE;
	}
	if ( wm_panel_event(sw_pid, ev, &ans) < E_OK ) {
		ans = WM_ANS_NONE;
	}
	if ( ev->type != HID_EV_MOVE ) wm_panel_draw(sw_pid);
	wm_update();
	if ( ( ev->type == HID_EV_BTN_UP || ev->type == HID_EV_KEY_UP ) && sw_state == SW_SETUP && ans == WM_ANS_NONE ) {
		INT	os = sw_sel, om = sw_mode;

		/* another disk or way: 〈消す区画〉 follows */
		keep_choices();
		if ( sw_sel != os || sw_mode != om ) rebuild(FALSE);
	}
	switch ( ans ) {
	case ANS_START:		start();		break;
	case ANS_RESTART:	restart();		break;
	case ANS_END:		dt_close(sw_d);		break;
	default:		break;
	}
	return TRUE;
}

/* The menu: 閉じる, 開始, ディスクの一覧を更新, the three ways as the panel's choices, 再起動, and the tail */
LOCAL void sw_menu_set( DTWIN *d, ID mid )
{
	BOOL	run = (BOOL)( sw_state == SW_RUN );

	(void)d;
	keep_choices();
	(void)mn_chg_atr(mid, NULL, "sysinst.start", ( sw_state != SW_SETUP || sw_ndisk == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.coexist", ( sw_state != SW_SETUP ) ? MN_GREY
			 : ( sw_mode == SI_COEXIST ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.replace", ( sw_state != SW_SETUP ) ? MN_GREY
			 : ( sw_mode == SI_REPLACE ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.erase", ( sw_state != SW_SETUP ) ? MN_GREY
			 : ( sw_mode == SI_ERASE ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.list", run ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "sysinst.restart", ( sw_state != SW_DONE || sw_er < E_OK ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "close", run ? MN_GREY : 0);
}

LOCAL void sw_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	(void)d;
	if ( mn_is(sel, "sysinst.start") ) {
		start();
		return;
	}
	if ( mn_is(sel, "sysinst.restart") ) {
		if ( sw_state == SW_DONE && sw_er >= E_OK ) restart();
		return;
	}
	if ( sw_state == SW_RUN ) {
		return;
	}
	keep_choices();
	if ( mn_is(sel, "sysinst.coexist") ) sw_mode = SI_COEXIST;
	else if ( mn_is(sel, "sysinst.replace") ) sw_mode = SI_REPLACE;
	else if ( mn_is(sel, "sysinst.erase") ) sw_mode = SI_ERASE;
	else if ( mn_is(sel, "sysinst.list") ) {
		list_disks();
		sw_state = SW_SETUP;
		setup_lines();
	} else return;
	rebuild(FALSE);
}

LOCAL CONST DTOWN	sw_own = {
	SYSDEF_MENU_SYSINST,
	sw_draw,
	sw_input,
	NULL,
	sw_menu_set,
	sw_menu_do,
	sw_closing,
	sw_close,
	sw_poll,
	NULL
};

#else /* X64 */

/* The PC's only: elsewhere the system is put on a disk as an image (03-platform.md) */
EXPORT void dt_sysinst_open( void )
{
	(void)wm_msg_put("システム登録はx64のPCだけで使えます");
}

#endif /* X64 */
