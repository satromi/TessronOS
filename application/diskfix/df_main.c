/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	df_main.c
 *	ディスク修復: a native volume checked, and repaired, an accessory
 *	running as a process (ts/tsfsck.h, docs/tessronos/11-desktop.md
 *	11.25)
 *
 *	The window is a panel of the data box (SYSDEF_BOX_DISKFIX). The
 *	frame moves, sizes and closes it, and the second button opens its
 *	menu, where 開始, 障害情報 and what the panel chooses are too, between
 *	閉じる and the tail of ウインドウ, 小物 and 実行. 〈対象ドライブ〉 lists
 *	every native volume the machine has (ob_chk_list), by its device
 *	name, label and size, the one the system runs on and those in use said
 *	so. 〈処理方式〉 is 検査 (look only, the default) or 修復, and 簡易, 通常
 *	(the default) or 完全. 開始 asks first when it is to repair, and for
 *	the system's own volume asks to start the machine again: that volume
 *	is repaired at the next start, before anything uses it, and the
 *	desktop then starts this program by itself, which shows what the
 *	repair found and did (ob_chk_boot_result).
 *
 *	A check, and a repair asked for the next start, are an administrator's:
 *	the desktop asks the person for the administrator's password first
 *	(ts_admin_ask), and the kernel refuses the call without it. The check
 *	runs in a task of this program's own (ob_chk_vol, the kernel putting
 *	each thing told into this program's memory and the count after it),
 *	the window following it: the item being looked at, how many things
 *	have been found, and at the end what was found, what was put right and
 *	what is left. The kernel keeps a volume being checked from being
 *	attached, taken away or made larger meanwhile, and the window is not
 *	closed until the check is over. 障害情報 opens a second window with
 *	every thing told, page by page; 保存 there hands it to the desktop,
 *	which writes it as the text object ディスク修復記録 linked from the
 *	system box (/SYS); closing the first window closes it too.
 *
 *	Its window says it shows the program object, so that running the
 *	program again brings it forward. What it does is said on the console
 *	for the tests ("dfix: ...").
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <tk/typedef.h>
#include <tk/syscall.h>
#include <ts/ui.h>
#include <ts/wm.h>
#include <ts/hid.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/dtreq.h>
#include <ts/tsfsck.h>
#include <ts/uiprn.h>

#define FX_W		600		/* the window's panel */
#define FX_H		476
#define FI_W		660		/* 障害情報's */
#define FI_H		484
#define FX_ROWS		7		/* lines of what is under way and what came of it */
#define FI_ROWS		18
#define FX_EV_MAX	4000		/* things told, kept for 障害情報 */
#define FX_STACK	(32 * 1024)
#define FX_DEV_MAX	64		/* volumes listed */

/* the panels and their parts */
#define PN_MAIN		1
#define PN_INFO		2
#define PN_ASK		10
#define DF_DEV		1
#define DF_CHECK		20
#define DF_REPAIR	21
#define DF_QUICK		22
#define DF_NORMAL	23
#define DF_FULL		24
#define B_START		30
#define B_INFO		31
#define B_END		32
#define DF_LINE0		41
#define DF_IHEAD		50
#define DF_IROW0		51
#define B_PREV		70
#define B_NEXT		71
#define B_SAVE		72
#define B_IEND		73
#define B_YES		101

#define FX_SETUP	0
#define FX_RUN		1
#define FX_DONE		2

static UI		fx, fi;			/* the window, and 障害情報's */
static BOOL		fi_up = FALSE;
static INT		fi_page = 0;

static T_TSFSCKDEV	fx_dev[FX_DEV_MAX];
static char		*fx_rows[FX_DEV_MAX];
static char		fx_rowtext[FX_DEV_MAX][96];
static INT		fx_ndev = 0, fx_sel = 0;
static BOOL		fx_repair = FALSE;
static INT		fx_depth = 1;		/* 簡易 0, 通常 1, 完全 2 */
static INT		fx_state = FX_SETUP;
static BOOL		fx_boot = FALSE;	/* what is shown is the repair at start */
static char		fx_run_dev[TSFSCK_DEVNM_MAX];
static UINT		fx_run_mode = 0;
static BOOL		fx_run_named = FALSE;	/* its objects can be named: the volume is attached */

/* shared with the check's task: the events, and how many the kernel has told */
static T_TSFSCKEV	*fx_ev = NULL;
static INT		fx_told = 0;
static INT		fx_done = 0;
static ER		fx_er = E_OK;
static T_TSFSCK		fx_res, fx_res_run;
static INT		fx_nev = 0;		/* kept, of them */
static INT		fx_seen = 0;		/* followed by the window */
static INT		fx_found = 0;
static INT		fx_phase = 0;

static char		fx_line[FX_ROWS][160];

/* What happened, told on the console with how many times it has */
#define SN_ASK		0
#define SN_ANSWER	1
#define SN_INFO		2
#define SN_INFOEND	3
#define SN_SAVED	4
#define SN_DONE		5
#define SN_MENU		6
#define SN_KINDS	7
static INT		fx_told_kind[SN_KINDS];

static INT told( INT kind )
{
	return ++fx_told_kind[kind];
}

static void note( CONST char *s )
{
	char	b[240];

	snprintf(b, sizeof(b), "dfix: %s\n", s);
	tm_putstring((CONST UB *)b);
}

/* ---------------------------------------------------------------- words */

/* s after b[n], cut at a letter's edge when there is no more room */
static INT put( char *b, INT n, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && n < max - 1 ) {
		b[n++] = *s++;
	}
	while ( n > 0 && s != NULL && *s != 0 && ( (UB)*s & 0xC0 ) == 0x80 ) {
		s--;
		n--;
	}
	b[n] = 0;
	return n;
}

static INT put_num( char *b, INT n, INT max, D v )
{
	char	t[24];

	snprintf(t, sizeof(t), "%lld", (long long)v);
	return put(b, n, max, t);
}

static void line_set( INT i, CONST char *s )
{
	if ( i >= 0 && i < FX_ROWS ) {
		(void)put(fx_line[i], 0, sizeof(fx_line[i]), s);
	}
}

static CONST char *phase_name( INT ph )
{
	switch ( ph ) {
	case TSFSCK_PH_SUPER:	return "ボリュームの管理情報";
	case TSFSCK_PH_INDEX:	return "リンクインデックス";
	case TSFSCK_PH_HEAD:	return "ファイルヘッダー";
	case TSFSCK_PH_TABLE:	return "通常インデックスと接続インデックス";
	case TSFSCK_PH_DATA:	return "フラグメント・区切り・レコードサイズ";
	case TSFSCK_PH_INFO:	return "ファイルヘッダー情報";
	case TSFSCK_PH_SPACE:	return "領域の割当て";
	case TSFSCK_PH_FREE:	return "空き領域";
	case TSFSCK_PH_HIDDEN:	return "隠れ屑実身";
	case TSFSCK_PH_REPAIR:	return "修復";
	case TSFSCK_PH_VERIFY:	return "修復後の検査";
	default:		return "";
	}
}

static CONST char *kind_name( INT k )
{
	switch ( k ) {
	case TSFSCK_K_SUPER:	return "スーパーブロック";
	case TSFSCK_K_GROUP:	return "割当て群";
	case TSFSCK_K_FREECNT:	return "空きブロック数";
	case TSFSCK_K_NODE:	return "索引の節";
	case TSFSCK_K_INDEX:	return "索引の項目";
	case TSFSCK_K_HEAD:	return "ファイルヘッダー";
	case TSFSCK_K_PART:	return "管理情報";
	case TSFSCK_K_TABLE:	return "インデックス";
	case TSFSCK_K_DATA:	return "レコードの置き場";
	case TSFSCK_K_META:	return "管理用セグメント";
	case TSFSCK_K_REFCNT:	return "参照数";
	case TSFSCK_K_GCLIST:	return "屑実身の一覧";
	case TSFSCK_K_ORPHAN:	return "孤児の木";
	case TSFSCK_K_LINK:	return "壊れたリンク";
	case TSFSCK_K_DOUBLE:	return "二重割当て";
	case TSFSCK_K_LOST:	return "割当て漏れ";
	case TSFSCK_K_LEAK:	return "使われていない領域";
	case TSFSCK_K_UNREAD:	return "読めないブロック";
	case TSFSCK_K_HIDDEN:	return "隠れ屑実身";
	case TSFSCK_K_RELINK:	return "仮身表の作り直し";
	case TSFSCK_K_XREF:	return "ほかのボリュームからの参照";
	default:		return "";
	}
}

static CONST char *why_name( D w )
{
	switch ( w ) {
	case TSFSCK_W_DAMAGED:	return "検査値が合わない";
	case TSFSCK_W_OUTSIDE:	return "範囲の外を指す";
	case TSFSCK_W_WRONG:	return "ほかのものが書かれている";
	case TSFSCK_W_SIZE:	return "大きさが合わない";
	case TSFSCK_W_FORM:	return "形が正しくない";
	case TSFSCK_W_COUNT:	return "項目の数が合わない";
	case TSFSCK_W_ORDER:	return "順序が正しくない";
	case TSFSCK_W_SHARED:	return "置き場が重なっている";
	default:		return "";
	}
}

static CONST char *part_name( D off )
{
	switch ( off ) {
	case 128:	return "管理用セグメント";
	case 160:	return "置き場表";
	case 192:	return "リソース表";
	case 224:	return "仮身表";
	case 288:	return "アイコン";
	default:	return "管理情報";
	}
}

/* An object as it is written in a line: its name when it can be read, and the head of its UUID */
static INT put_obj( char *b, INT n, INT max, CONST TS_UUID *u )
{
	char	s[TS_UUID_STRLEN + 1];
	T_OBREF	ref;

	ts_uuid_text(u, s);
	s[18] = 0;
	n = put(b, n, max, "実身");
	if ( fx_run_named && ob_ref_obj(u, &ref) >= E_OK && ref.name[0] != 0 ) {
		ref.name[OB_NAME_MAX - 1] = 0;
		n = put(b, n, max, "「");
		n = put(b, n, max, (CONST char *)ref.name);
		n = put(b, n, max, "」");
	}
	n = put(b, n, max, "(");
	n = put(b, n, max, s);
	return put(b, n, max, "…)");
}

static INT put_blk( char *b, INT n, INT max, UD blk )
{
	n = put(b, n, max, "ブロック");
	return put_num(b, n, max, (D)blk);
}

/* One thing told, in words */
static INT ev_text( CONST T_TSFSCKEV *e, char *b, INT max )
{
	INT	n = 0;

	b[0] = 0;
	if ( e->kind == TSFSCK_K_PHASE ) {
		n = put(b, n, max, "■ ");
		return put(b, n, max, phase_name(e->phase));
	}
	if ( e->phase == TSFSCK_PH_VERIFY && !e->fixed ) {
		n = put(b, n, max, "  修復後も残った障害: ");
		n = put(b, n, max, kind_name(e->kind));
		n = put(b, n, max, " ");
		n = put_num(b, n, max, e->a);
		return put(b, n, max, "件");
	}
	n = put(b, n, max, e->fixed ? "  修復: " : "  ");
	if ( e->has_uuid ) {
		n = put_obj(b, n, max, &e->uuid);
		n = put(b, n, max, " ");
	}
	if ( e->fixed ) {
		switch ( e->kind ) {
		case TSFSCK_K_REFCNT:
			n = put(b, n, max, "参照数を");
			n = put_num(b, n, max, e->b);
			return put(b, n, max, "に直しました");
		case TSFSCK_K_GCLIST:	return put(b, n, max, "屑実身の一覧を直しました");
		case TSFSCK_K_ORPHAN:	return put(b, n, max, "孤児の木を直しました");
		case TSFSCK_K_LINK:
		case TSFSCK_K_RELINK:	return put(b, n, max, "仮身表を作り直しました");
		case TSFSCK_K_HIDDEN:	return put(b, n, max, "TRASHに集めました");
		case TSFSCK_K_HEAD:
			if ( e->b == 0 ) return put(b, n, max, "壊れた実身ブロックを外しました(読めるものはありませんでした)");
			n = put(b, n, max, "壊れた実身ブロックを外し、読めたレコード");
			n = put_num(b, n, max, e->a);
			return put(b, n, max, "個を新しい実身に移してTRASHに集めました");
		case TSFSCK_K_LOST:
			n = put_blk(b, n, max, e->blk);
			n = put(b, n, max, "から");
			n = put_num(b, n, max, e->a);
			return put(b, n, max, "個を使用中にしました");
		case TSFSCK_K_LEAK:
			n = put_blk(b, n, max, e->blk);
			n = put(b, n, max, "から");
			n = put_num(b, n, max, e->a);
			return put(b, n, max, "個を解放しました");
		case TSFSCK_K_SUPER:
			return put(b, n, max, ( e->b == 1 ) ? "ボリュームの異常の記録を消しました"
							   : "スーパーブロックの写しを書き直しました");
		case TSFSCK_K_GROUP:	return put(b, n, max, "割当て群のヘッダを書き直しました");
		case TSFSCK_K_FREECNT:	return put(b, n, max, "空きブロック数を数え直しました");
		case TSFSCK_K_XREF:
			n = put(b, n, max, "ほかのボリュームからの参照の記録を、接続中のボリュームの仮身に合わせました(");
			n = put_num(b, n, max, (D)e->blk);
			return put(b, n, max, "件)");
		default:		return put(b, n, max, kind_name(e->kind));
		}
	}
	switch ( e->kind ) {
	case TSFSCK_K_SUPER:
		if ( e->b == 1 ) {
			n = put(b, n, max, "ボリュームに異常が記録されています(");
			n = put_blk(b, n, max, e->blk);
			return put(b, n, max, ")");
		}
		if ( e->b == 2 ) return put(b, n, max, "スーパーブロックが指す実身がボリュームにありません");
		n = put(b, n, max, "スーパーブロックの写しが壊れています(");
		n = put_blk(b, n, max, e->blk);
		return put(b, n, max, ")");
	case TSFSCK_K_GROUP:
		n = put(b, n, max, ( e->b == 1 ) ? "空きビットマップが壊れています(" : "割当て群のヘッダが壊れています(");
		n = put_blk(b, n, max, e->blk);
		return put(b, n, max, ")");
	case TSFSCK_K_FREECNT:
		n = put(b, n, max, "空きブロック数がビットマップと合わない割当て群が");
		n = put_num(b, n, max, e->a);
		return put(b, n, max, "個あります");
	case TSFSCK_K_NODE:
		n = put(b, n, max, ( e->a == 0 ) ? "実身索引" : ( e->a == 1 ) ? "屑実身の木" : "孤児の木");
		n = put(b, n, max, "の節が壊れています(");
		n = put_blk(b, n, max, e->blk);
		n = put(b, n, max, "、");
		n = put(b, n, max, why_name(e->b));
		return put(b, n, max, ")");
	case TSFSCK_K_INDEX:
		n = put(b, n, max, "索引が実身ブロックでない所を指しています(");
		n = put_blk(b, n, max, e->blk);
		return put(b, n, max, ")");
	case TSFSCK_K_HEAD:
	case TSFSCK_K_PART:
	case TSFSCK_K_TABLE:
	case TSFSCK_K_DATA:
		n = put(b, n, max, ( e->kind == TSFSCK_K_HEAD ) ? "ファイルヘッダー"
			: ( e->kind == TSFSCK_K_DATA ) ? "レコードの置き場" : part_name(e->a));
		n = put(b, n, max, "が正しくありません(");
		n = put_blk(b, n, max, e->blk);
		n = put(b, n, max, "、");
		n = put(b, n, max, why_name(e->b));
		return put(b, n, max, ")");
	case TSFSCK_K_META:
		return put(b, n, max, ( e->b == TSFSCK_W_FORM ) ? "管理用セグメントがJSONのオブジェクトではありません"
								 : "管理用セグメントのレコードの並びが置き場表と合いません");
	case TSFSCK_K_REFCNT:
		n = put(b, n, max, "参照数が");
		n = put_num(b, n, max, e->a);
		n = put(b, n, max, "ですが、仮身と固定から数えると");
		n = put_num(b, n, max, e->b);
		return put(b, n, max, "です");
	case TSFSCK_K_GCLIST:
		return put(b, n, max, "屑実身の一覧と実身の印が合いません");
	case TSFSCK_K_ORPHAN:
		return put(b, n, max, "孤児の木と実身の印が合いません");
	case TSFSCK_K_LINK:
		n = put(b, n, max, "の仮身が、ボリュームに無い");
		if ( e->has_other ) n = put_obj(b, n, max, &e->other);
		return put(b, n, max, "を指しています");
	case TSFSCK_K_DOUBLE:
		n = put_blk(b, n, max, e->blk);
		return put(b, n, max, "が二重に割り当てられています");
	case TSFSCK_K_LOST:
		n = put_blk(b, n, max, e->blk);
		n = put(b, n, max, "から");
		n = put_num(b, n, max, e->a);
		return put(b, n, max, "個が、使われているのに空きになっています(割当て漏れ)");
	case TSFSCK_K_LEAK:
		n = put_blk(b, n, max, e->blk);
		n = put(b, n, max, "から");
		n = put_num(b, n, max, e->a);
		return put(b, n, max, "個が、使われていないのに使用中になっています");
	case TSFSCK_K_UNREAD:
		n = put_blk(b, n, max, e->blk);
		return put(b, n, max, "を読めません");
	case TSFSCK_K_HIDDEN:
		n = put(b, n, max, "は隠れ屑実身です(参照数");
		n = put_num(b, n, max, e->a);
		return put(b, n, max, ")");
	case TSFSCK_K_RELINK:
		return put(b, n, max, "の仮身表の作り直しが残っています");
	case TSFSCK_K_XREF:
		switch ( e->b ) {
		case TSFSCK_X_TARGET:
			return put(b, n, max, "は無いのに、ほかのボリュームからの参照が記録されています");
		case TSFSCK_X_SOURCE:
			n = put(b, n, max, "を指す");
			if ( e->has_other ) n = put_obj(b, n, max, &e->other);
			return put(b, n, max, "からの参照が記録されていますが、その実身がありません");
		case TSFSCK_X_MISSING:
			n = put(b, n, max, "を指す");
			if ( e->has_other ) n = put_obj(b, n, max, &e->other);
			n = put(b, n, max, "の仮身");
			n = put_num(b, n, max, (D)e->blk);
			return put(b, n, max, "個が、参照として記録されていません");
		case TSFSCK_X_OBJECT:
			return put(b, n, max, "(ほかのボリュームからの参照の記録)がありません");
		default:
			n = put(b, n, max, "を指す");
			if ( e->has_other ) n = put_obj(b, n, max, &e->other);
			n = put(b, n, max, "からの参照が");
			n = put_num(b, n, max, e->a);
			n = put(b, n, max, "と記録されていますが、仮身は");
			n = put_num(b, n, max, (D)e->blk);
			return put(b, n, max, "個です");
		}
	default:
		return put(b, n, max, kind_name(e->kind));
	}
}

/* ---------------------------------------------------------------- what came of it */

static INT sum( CONST INT *k )
{
	INT	i, s = 0;

	for ( i = 1; i < TSFSCK_NKIND; i++ ) s += k[i];
	return s;
}

/* The lines the window shows when the check is over */
static void result_lines( void )
{
	char	b[160];
	INT	n, i;

	for ( i = 0; i < FX_ROWS; i++ ) fx_line[i][0] = 0;
	if ( fx_er < E_OK ) {
		n = put(b, 0, sizeof(b), ( fx_er == E_BUSY ) ? "このディスクはいま検査できません(使用中)"
							      : "ディスクを検査できませんでした(");
		if ( fx_er != E_BUSY ) {
			n = put_num(b, n, sizeof(b), fx_er);
			(void)put(b, n, sizeof(b), ")");
		}
		line_set(0, b);
		return;
	}
	n = put(b, 0, sizeof(b), fx_boot ? "再起動の前に依頼された" : "");
	n = put(b, n, sizeof(b), ( ( fx_res.mode & TSFSCK_REPAIR ) != 0 ) ? "修復が終わりました。" : "検査が終わりました。");
	line_set(0, b);
	if ( fx_res.problems == 0 ) {
		line_set(1, "障害は見つかりませんでした。");
	} else {
		n = put_num(b, 0, sizeof(b), fx_res.problems);
		n = put(b, n, sizeof(b), "件の障害が見つかりました");
		if ( fx_res.unfixable > 0 ) {
			n = put(b, n, sizeof(b), "(修復できないもの");
			n = put_num(b, n, sizeof(b), fx_res.unfixable);
			n = put(b, n, sizeof(b), "件)");
		}
		(void)put(b, n, sizeof(b), "。");
		line_set(1, b);
	}
	i = 2;
	if ( ( fx_res.mode & TSFSCK_REPAIR ) != 0 && fx_res.problems > 0 ) {
		if ( fx_res.refused && fx_res.inuse ) {
			line_set(i++, "使用中の実身があるため、ディスクを取り外して修復できませんでした。");
			line_set(i++, "実身を閉じてから、もう一度修復してください。");
		} else if ( fx_res.refused ) {
			line_set(i++, "修復できない障害があるため、ディスクを書き換えませんでした。");
		} else {
			if ( fx_res.detached ) {
				line_set(i++, "使用中のディスクを取り外して修復し、取り付け直しました。");
			}
			n = put_num(b, 0, sizeof(b), sum(fx_res.fixed));
			n = put(b, n, sizeof(b), "件を修復しました。修復後に残った障害は");
			n = put_num(b, n, sizeof(b), fx_res.remaining);
			(void)put(b, n, sizeof(b), "件です。");
			line_set(i++, b);
		}
		if ( fx_res.fixed[TSFSCK_K_HIDDEN] > 0 ) {
			n = put_num(b, 0, sizeof(b), fx_res.fixed[TSFSCK_K_HIDDEN]);
			(void)put(b, n, sizeof(b), "個の隠れ屑実身を回収しました。TRASHは屑実身操作で見られます。");
			line_set(i++, b);
		}
	} else if ( fx_res.problems > 0 ) {
		line_set(i++, "〈修復〉を選んで開始すると、修復を試みます。");
	}
	n = put(b, 0, sizeof(b), "実身");
	n = put_num(b, n, sizeof(b), (D)fx_res.objects);
	n = put(b, n, sizeof(b), "個、レコード");
	n = put_num(b, n, sizeof(b), (D)fx_res.records);
	n = put(b, n, sizeof(b), "個、仮身");
	n = put_num(b, n, sizeof(b), (D)fx_res.links);
	n = put(b, n, sizeof(b), "個、空き");
	n = put_num(b, n, sizeof(b), (D)fx_res.free);
	n = put(b, n, sizeof(b), "/");
	n = put_num(b, n, sizeof(b), (D)fx_res.blocks);
	(void)put(b, n, sizeof(b), "ブロック");
	line_set(FX_ROWS - 1, b);
	if ( fx_res.problems > 0 ) {
		line_set(FX_ROWS - 2, "くわしくは〈障害情報〉で見られます。");
	}
}

/* The lines while the check runs: the item being looked at, and how many found so far */
static void running_lines( void )
{
	char	b[160];
	INT	n, i;

	for ( i = 0; i < FX_ROWS; i++ ) fx_line[i][0] = 0;
	n = put(b, 0, sizeof(b), ( ( fx_run_mode & TSFSCK_REPAIR ) != 0 ) ? "修復しています: " : "検査しています: ");
	(void)put(b, n, sizeof(b), fx_run_dev);
	line_set(0, b);
	n = put(b, 0, sizeof(b), "項目: ");
	(void)put(b, n, sizeof(b), phase_name(fx_phase));
	line_set(1, b);
	n = put(b, 0, sizeof(b), "見つかった障害: ");
	n = put_num(b, n, sizeof(b), fx_found);
	(void)put(b, n, sizeof(b), "件");
	line_set(2, b);
}

/* ---------------------------------------------------------------- the check's task */

static void worker( INT stacd, void *exinf )
{
	ER	er;

	(void)stacd;
	(void)exinf;
	memset(&fx_res_run, 0, sizeof(fx_res_run));
	er = ob_chk_vol(fx_run_dev, fx_run_mode, &fx_res_run, fx_ev, FX_EV_MAX, &fx_told);
	fx_er = er;
	__atomic_store_n(&fx_done, 1, __ATOMIC_RELEASE);
	tk_exd_tsk();
}

/* ---------------------------------------------------------------- the window */

/* What was chosen in the panel, kept before it is shown again */
static void keep_choices( void )
{
	INT	i;

	if ( fx_state == FX_RUN ) {
		return;
	}
	i = ui_value(&fx, DF_DEV);
	if ( i >= 1 && i <= fx_ndev ) fx_sel = i - 1;
	fx_repair = (BOOL)( ui_value(&fx, DF_REPAIR) != 0 );
	fx_depth = ( ui_value(&fx, DF_QUICK) != 0 ) ? 0 : ( ui_value(&fx, DF_FULL) != 0 ) ? 2 : 1;
}

/* The window's parts set from what it shows now */
static void show( void )
{
	BOOL	run = (BOOL)( fx_state == FX_RUN );
	INT	i;

	ui_set_value(&fx, DF_CHECK, fx_repair ? 0 : 1);
	ui_set_value(&fx, DF_REPAIR, fx_repair ? 1 : 0);
	ui_set_value(&fx, DF_QUICK, ( fx_depth == 0 ) ? 1 : 0);
	ui_set_value(&fx, DF_NORMAL, ( fx_depth == 1 ) ? 1 : 0);
	ui_set_value(&fx, DF_FULL, ( fx_depth == 2 ) ? 1 : 0);
	ui_off(&fx, DF_DEV, run);
	for ( i = DF_CHECK; i <= DF_FULL; i++ ) ui_off(&fx, i, run);
	ui_off(&fx, B_START, (BOOL)( run || fx_ndev == 0 ));
	ui_off(&fx, B_INFO, (BOOL)( fx_state != FX_DONE || fx_nev == 0 ));
	ui_off(&fx, B_END, run);
	for ( i = 0; i < FX_ROWS; i++ ) {
		ui_set_text(&fx, DF_LINE0 + i, ( fx_line[i][0] != 0 ) ? fx_line[i] : " ");
	}
}

/* Every volume there is, in the list, the one chosen kept when it is still there */
static void list_devices( void )
{
	INT	i, n;

	n = ob_chk_list(fx_dev, FX_DEV_MAX);
	fx_ndev = ( n < 0 ) ? 0 : ( n > FX_DEV_MAX ) ? FX_DEV_MAX : n;
	for ( i = 0; i < fx_ndev; i++ ) {
		CONST T_TSFSCKDEV	*d = &fx_dev[i];
		INT			k;

		k = put(fx_rowtext[i], 0, sizeof(fx_rowtext[i]), (CONST char *)d->devnm);
		k = put(fx_rowtext[i], k, sizeof(fx_rowtext[i]), "  ");
		k = put(fx_rowtext[i], k, sizeof(fx_rowtext[i]), ( d->label[0] != 0 ) ? (CONST char *)d->label : "(名前なし)");
		k = put(fx_rowtext[i], k, sizeof(fx_rowtext[i]), "  ");
		k = put_num(fx_rowtext[i], k, sizeof(fx_rowtext[i]), (D)( d->blocks * TSFSBLK_BLOCK_SIZE / ( 1024 * 1024 ) ));
		k = put(fx_rowtext[i], k, sizeof(fx_rowtext[i]), "MB");
		if ( d->system ) (void)put(fx_rowtext[i], k, sizeof(fx_rowtext[i]), "  システム");
		else if ( d->attached ) (void)put(fx_rowtext[i], k, sizeof(fx_rowtext[i]), "  使用中");
		fx_rows[i] = fx_rowtext[i];
	}
	if ( fx_sel >= fx_ndev ) fx_sel = 0;
	ui_names(&fx, DF_DEV, (CONST char *const *)fx_rows, fx_ndev);
	ui_set_value(&fx, DF_DEV, ( fx_ndev > 0 ) ? fx_sel + 1 : 0);
}

/* ---------------------------------------------------------------- asking */

/* A question over the window: TRUE for 実行 */
static BOOL ask( CONST char *l1, CONST char *l2, CONST char *l3 )
{
	CONST char	*lines[3];
	char		b[64];
	INT		a;

	lines[0] = l1;
	lines[1] = l2;
	lines[2] = l3;
	snprintf(b, sizeof(b), "ask %d 実行", told(SN_ASK));
	note(b);
	a = ui_ask(&fx, PN_ASK, lines, 3);
	snprintf(b, sizeof(b), "answered %d %d", told(SN_ANSWER), ( a == B_YES ) ? 1 : 0);
	note(b);
	return (BOOL)( a == B_YES );
}

/* The administrator's rights for what is to be done; FALSE, and the person told, when not given */
static BOOL admin( CONST char *what, CONST char *noright )
{
	ER	er = ts_admin_ask(what, &fx.win);
	char	b[64];

	snprintf(b, sizeof(b), "administrator %d", (INT)er);
	note(b);
	if ( er >= E_OK ) {
		return TRUE;
	}
	if ( er != E_ABORT ) {
		ts_no_right(noright);
	}
	return FALSE;
}

/* ---------------------------------------------------------------- starting */

/* The system started again, to repair its own volume before it uses it */
static void restart_for( CONST T_TSFSCKDEV *d, UINT mode )
{
	TS_UUID	u;
	SZ	asz = 0;
	ID	key;
	ER	er;

	if ( !admin("システムのディスクの修復", "システムのディスクの修復を") ) {
		return;
	}
	er = ob_chk_request((CONST char *)d->devnm, mode);
	if ( er < E_OK ) {
		(void)wm_msg_put("ディスク修復: 修復の依頼を書けませんでした");
		return;
	}
	note("restart");
	key = ( ui_uuid(SYSDEF_SYSTEM, &u) >= E_OK ) ? ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE) : E_SYS;
	er = E_SYS;
	if ( key > 0 ) {
		er = ob_wri_rec(key, OB_SYS_POWER, 0, "RESTART", 7, &asz);
		(void)ob_cls_obj(key);
	}
	(void)ob_end_adm();
	if ( key <= 0 || er < E_OK ) {
		(void)wm_msg_put("ディスク修復: 次に起動したときに修復します");
	}
}

static void start( void )
{
	CONST T_TSFSCKDEV	*d;
	T_CTSK			ct;
	T_RTSK			rt;
	UINT			mode;
	ID			tid;
	char			b[64];

	keep_choices();
	if ( fx_state == FX_RUN || fx_ndev == 0 ) {
		return;
	}
	d = &fx_dev[fx_sel];
	mode = ( fx_repair ? TSFSCK_REPAIR : 0 ) | ( fx_depth == 0 ? TSFSCK_QUICK : fx_depth == 2 ? TSFSCK_FULL : 0 );
	if ( fx_repair ) {
		if ( !ask("修復を行うと、ディスクの状態によってはデータの一部が", "失われることがあります。",
			  "実際に修復を行う場合は[実行]を押してください。") ) {
			return;
		}
		if ( d->system ) {
			if ( ask("このディスクでシステムが動いています。修復はシステムを", "再起動し、ディスクを使い始める前に行います。",
				 "[実行]を押すと再起動します。") ) {
				restart_for(d, mode);
			}
			return;
		}
	}
	if ( !admin(fx_repair ? "ディスクの修復" : "ディスクの検査", fx_repair ? "ディスクを修復" : "ディスクを検査") ) {
		return;
	}
	if ( fx_ev == NULL ) {
		fx_ev = (T_TSFSCKEV *)malloc(sizeof(T_TSFSCKEV) * FX_EV_MAX);
		if ( fx_ev == NULL ) return;
	}
	(void)put(fx_run_dev, 0, sizeof(fx_run_dev), (CONST char *)d->devnm);
	fx_run_mode = mode;
	fx_run_named = d->attached;
	fx_told = fx_seen = fx_found = fx_nev = 0;
	fx_done = 0;
	fx_er = E_OK;
	fx_phase = 0;
	fx_boot = FALSE;
	memset(&fx_res, 0, sizeof(fx_res));

	memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG3;
	ct.task = (FP)worker;
	ct.itskpri = ( tk_ref_tsk(TSK_SELF, &rt) >= E_OK ) ? rt.tskbpri + 1 : 20;
	ct.stksz = FX_STACK;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		(void)wm_msg_put("ディスク修復: 検査を始められませんでした");
		(void)ob_end_adm();
		return;
	}
	fx_state = FX_RUN;
	snprintf(b, sizeof(b), "started %s %d", fx_run_dev, (INT)mode);
	note(b);
	running_lines();
	show();
}

/* The check followed from the loop: what the kernel has told since */
static void follow( void )
{
	INT	n, i, done;
	BOOL	moved = FALSE;
	char	b[160];

	if ( fx_state != FX_RUN ) {
		return;
	}
	done = __atomic_load_n(&fx_done, __ATOMIC_ACQUIRE);
	n = __atomic_load_n(&fx_told, __ATOMIC_ACQUIRE);
	if ( n > FX_EV_MAX ) n = FX_EV_MAX;
	for ( i = fx_seen; i < n; i++ ) {
		CONST T_TSFSCKEV	*e = &fx_ev[i];

		if ( e->kind == TSFSCK_K_PHASE ) {
			fx_phase = e->phase;
			snprintf(b, sizeof(b), "phase %d", e->phase);
			note(b);
			moved = TRUE;
		} else if ( !e->fixed && e->phase != TSFSCK_PH_VERIFY && e->phase != TSFSCK_PH_REPAIR ) {
			fx_found++;
			moved = TRUE;
		}
	}
	fx_seen = n;
	fx_nev = n;
	if ( done ) {
		fx_res = fx_res_run;
		fx_state = FX_DONE;
		(void)ob_end_adm();
		result_lines();
		list_devices();
		show();
		snprintf(b, sizeof(b), "done %d: found %d fixed %d left %d refused %d unfixable %d er %d", told(SN_DONE),
			 fx_res.problems, sum(fx_res.fixed), fx_res.remaining, fx_res.refused ? 1 : 0, fx_res.unfixable,
			 (INT)fx_er);
		note(b);
		return;
	}
	if ( moved ) {
		running_lines();
		show();
	}
}

/* ---------------------------------------------------------------- 障害情報 */

static INT told_all( void )
{
	return ( fx_told > fx_nev ) ? fx_told : fx_nev;
}

static void info_show( void )
{
	char	b[200];
	INT	i, k, n = fx_nev, pages = ( n + FI_ROWS - 1 ) / FI_ROWS;

	if ( !fi_up ) {
		return;
	}
	if ( pages < 1 ) pages = 1;
	if ( fi_page >= pages ) fi_page = pages - 1;
	k = put(b, 0, sizeof(b), fx_run_dev);
	k = put(b, k, sizeof(b), "  ");
	k = put_num(b, k, sizeof(b), fi_page + 1);
	k = put(b, k, sizeof(b), "/");
	k = put_num(b, k, sizeof(b), pages);
	k = put(b, k, sizeof(b), "ページ");
	if ( told_all() > fx_nev ) {
		k = put(b, k, sizeof(b), "(ほかに");
		k = put_num(b, k, sizeof(b), told_all() - fx_nev);
		(void)put(b, k, sizeof(b), "件)");
	}
	ui_set_text(&fi, DF_IHEAD, b);
	for ( i = 0; i < FI_ROWS; i++ ) {
		INT	e = fi_page * FI_ROWS + i;

		if ( e < n ) {
			(void)ev_text(&fx_ev[e], b, sizeof(b));
		} else {
			(void)put(b, 0, sizeof(b), " ");
		}
		ui_set_text(&fi, DF_IROW0 + i, b);
	}
	ui_off(&fi, B_PREV, (BOOL)( fi_page == 0 ));
	ui_off(&fi, B_NEXT, (BOOL)( fi_page + 1 >= pages ));
	snprintf(b, sizeof(b), "info %d page %d", told(SN_INFO), fi_page + 1);
	note(b);
}

static void info_open( void )
{
	INT	sw = 0, sh = 0;

	keep_choices();
	if ( fi_up ) {
		ui_reveal(&fi);
		info_show();
		return;
	}
	fi_page = 0;
	wm_new_centre(FI_W, FI_H, &sw, &sh);	/* in the middle of the screen new windows open on: sw, sh where its corner goes */
	ui_icon(SYSDEF_PROG_DISKFIX);
	ui_attr(UI_ATTR_HIDDEN | WM_ATTR_RESIZE);
	if ( ui_open_on(&fi, &fx, SYSDEF_BOX_DISKFIX, PN_INFO, sw + 23, sh + 13) < E_OK ) {
		return;
	}
	fi_up = TRUE;
	info_show();
	ui_reveal(&fi);
}

static void info_close( void )
{
	char	b[32];

	if ( !fi_up ) {
		return;
	}
	ui_close(&fi);
	fi_up = FALSE;
	snprintf(b, sizeof(b), "info closed %d", told(SN_INFOEND));
	note(b);
}

/* Words inside an element of xmlTAD, the three that cannot stand there written as entities */
static INT put_text( char *b, INT n, INT max, CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		switch ( *s ) {
		case '&':	n = put(b, n, max, "&amp;");	break;
		case '<':	n = put(b, n, max, "&lt;");	break;
		case '>':	n = put(b, n, max, "&gt;");	break;
		default:
			if ( n < max - 1 ) b[n++] = *s;
			b[n] = 0;
			break;
		}
	}
	return n;
}

/* The desktop asked, and its answer waited for */
static ER ask_desktop( T_DTREQ *rq )
{
	T_DTANS	an;
	T_OBCRE	c;
	TS_UUID	d, reply;
	SZ	asz = 0;
	ID	k, ka;
	ER	er;

	if ( ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &reply) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&reply, OB_OP_READ);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&reply);
		return E_OBJ;
	}
	rq->seq = 1;
	rq->reply = reply;
	rq->win = fx.win;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : E_NOEXS;
	if ( k > 0 ) (void)ob_cls_obj(k);
	while ( er >= E_OK ) {
		memset(&an, 0, sizeof(an));
		if ( ob_rea_tmo(ka, 0, 0, &an, sizeof(an), &asz, 60 * 1000) < E_OK ) {
			er = E_TMOUT;
			break;
		}
		if ( asz == (SZ)sizeof(an) && an.seq == rq->seq ) {
			er = an.er;
			break;
		}
	}
	(void)ob_cls_obj(ka);
	(void)ob_del_obj(&reply);
	return er;
}

/* 保存: what was told, handed to the desktop to be written as ディスク修復記録 of the system box */
static void info_save( void )
{
	T_UIPRN	prn;
	T_DTREQ	rq;
	char	*x, b[200];
	INT	n = fx_nev, at, max = 512 + ( n + FX_ROWS ) * ( 200 + 16 ), i;
	ER	er;

	x = (char *)malloc((size_t)max);
	if ( x == NULL ) {
		return;
	}
	at = put(x, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"ディスク修復記録\">\n<document>\n<p>");
	at = put(x, at, max, "ディスク修復: ");
	at = put_text(x, at, max, fx_run_dev);
	at = put(x, at, max, "</p>\n");
	for ( i = 0; i < FX_ROWS; i++ ) {
		if ( fx_line[i][0] == 0 ) continue;
		at = put(x, at, max, "<p>");
		at = put_text(x, at, max, fx_line[i]);
		at = put(x, at, max, "</p>\n");
	}
	for ( i = 0; i < n; i++ ) {
		(void)ev_text(&fx_ev[i], b, sizeof(b));
		at = put(x, at, max, "<p>");
		at = put_text(x, at, max, b);
		at = put(x, at, max, "</p>\n");
	}
	at = put(x, at, max, "</document>\n</tad>\n");
	er = ui_prn_open(&prn, NULL);
	if ( er >= E_OK ) {
		er = ui_prn_page(&prn, (CONST UB *)x, at);
		if ( er >= E_OK ) er = ui_prn_ready(&prn);
		if ( er >= E_OK ) {
			memset(&rq, 0, sizeof(rq));
			rq.req = DT_RQ_DISK;
			rq.recno = DT_DISK_LOG;
			rq.target = prn.obj;
			er = ask_desktop(&rq);
		}
		ui_prn_close(&prn);
	}
	free(x);
	(void)wm_msg_put(( er >= E_OK ) ? "ディスク修復: 障害情報を「ディスク修復記録」に保存しました"
					: "ディスク修復: 障害情報を保存できませんでした");
	snprintf(b, sizeof(b), "saved %d %d", told(SN_SAVED), (INT)er);
	note(b);
}

/* ---------------------------------------------------------------- the menus */

static BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

static BOOL fx_closing( void );
static BOOL g_quit = FALSE;

/* ディスク修復's: what cannot be done while the check runs is grey */
static void fx_menu_set( ID mid )
{
	BOOL	run = (BOOL)( fx_state == FX_RUN );
	char	b[32];

	keep_choices();
	snprintf(b, sizeof(b), "menu %d", told(SN_MENU));
	note(b);
	(void)mn_chg_atr(mid, NULL, "dfix.start", ( run || fx_ndev == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.info", ( fx_state != FX_DONE || fx_nev == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.check", run ? MN_GREY : !fx_repair ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.repair", run ? MN_GREY : fx_repair ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.quick", run ? MN_GREY : ( fx_depth == 0 ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.normal", run ? MN_GREY : ( fx_depth == 1 ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.full", run ? MN_GREY : ( fx_depth == 2 ) ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.list", run ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "close", run ? MN_GREY : 0);
}

static void fx_menu_do( CONST T_MNSEL *sel )
{
	char	b[48];

	snprintf(b, sizeof(b), "chosen %s", (CONST char *)sel->code);
	note(b);
	if ( is_code(sel, "close") ) {
		if ( fx_closing() ) g_quit = TRUE;
		return;
	}
	if ( is_code(sel, "dfix.start") ) {
		start();
		return;
	}
	if ( is_code(sel, "dfix.info") ) {
		info_open();
		return;
	}
	if ( fx_state == FX_RUN ) {
		return;
	}
	keep_choices();
	if ( is_code(sel, "dfix.check") ) fx_repair = FALSE;
	else if ( is_code(sel, "dfix.repair") ) fx_repair = TRUE;
	else if ( is_code(sel, "dfix.quick") ) fx_depth = 0;
	else if ( is_code(sel, "dfix.normal") ) fx_depth = 1;
	else if ( is_code(sel, "dfix.full") ) fx_depth = 2;
	else if ( is_code(sel, "dfix.list") ) list_devices();
	else return;
	show();
}

/* 障害情報's: 閉じる, 前へ, 次へ, 保存 and the tail */
static void fi_menu_set( ID mid )
{
	INT	pages = ( fx_nev + FI_ROWS - 1 ) / FI_ROWS;
	char	b[32];

	snprintf(b, sizeof(b), "menu %d", told(SN_MENU));
	note(b);
	(void)mn_chg_atr(mid, NULL, "dfix.prev", ( fi_page == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.next", ( fi_page + 1 >= pages ) ? MN_GREY : 0);
}

static void fi_menu_do( CONST T_MNSEL *sel )
{
	char	b[48];

	snprintf(b, sizeof(b), "chosen %s", (CONST char *)sel->code);
	note(b);
	if ( is_code(sel, "close") ) {
		info_close();
	} else if ( is_code(sel, "dfix.prev") ) {
		if ( fi_page > 0 ) fi_page--;
		info_show();
	} else if ( is_code(sel, "dfix.next") ) {
		fi_page++;
		info_show();
	} else if ( is_code(sel, "dfix.save") ) {
		info_save();
	}
}

/* Whether a key with Ctrl is E, the letter of 閉じる in the menus */
static BOOL key_close( CONST T_OBNTM *m )
{
	return (BOOL)( ( m->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 && m->code == 0x08 );
}

/* A window's menu, at the press (m) */
static void menu( BOOL info, CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( ui_uuid(info ? SYSDEF_MENU_DFIX_INFO : SYSDEF_MENU_DISKFIX, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) {
		return;
	}
	if ( info ) fi_menu_set(mid);
	else fx_menu_set(mid);
	chosen = (BOOL)( mn_pop_men(mid, info ? fi.kw : fx.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( info ) fi_menu_do(&sel);
	else fx_menu_do(&sel);
}

/* ---------------------------------------------------------------- the windows */

/* Not while the check runs: it runs to its end */
static BOOL fx_closing( void )
{
	if ( fx_state == FX_RUN ) {
		(void)wm_msg_put("ディスク修復: 検査が終わるまで閉じられません");
		return FALSE;
	}
	return TRUE;
}

static void fx_event( CONST T_OBNTM *m )
{
	INT	num;

	switch ( ui_event(&fx, m, &num) ) {
	case UI_EV_CLOSE:
		if ( fx_closing() ) g_quit = TRUE;
		break;
	case UI_EV_MENU:
		menu(FALSE, m);
		break;
	case UI_EV_KEY:
		if ( key_close(m) && fx_closing() ) g_quit = TRUE;
		break;
	case UI_EV_BUTTON:
		if ( num == B_START ) start();
		else if ( num == B_INFO ) info_open();
		else if ( num == B_END && fx_closing() ) g_quit = TRUE;
		break;
	case UI_EV_CHANGE:
		keep_choices();
		break;
	default:
		break;
	}
}

static void fi_event( CONST T_OBNTM *m )
{
	INT	num;

	switch ( ui_event(&fi, m, &num) ) {
	case UI_EV_CLOSE:
		info_close();
		break;
	case UI_EV_MENU:
		menu(TRUE, m);
		break;
	case UI_EV_KEY:
		if ( key_close(m) ) info_close();
		break;
	case UI_EV_BUTTON:
		if ( num == B_PREV ) {
			if ( fi_page > 0 ) fi_page--;
			info_show();
		} else if ( num == B_NEXT ) {
			fi_page++;
			info_show();
		} else if ( num == B_SAVE ) {
			info_save();
		} else if ( num == B_IEND ) {
			info_close();
		}
		break;
	default:
		break;
	}
}

/*
 * At start, when the system volume was repaired before it was attached:
 * what that repair found and did, given once (the desktop starts this
 * program for it). FALSE when there is none.
 */
static BOOL boot_result( void )
{
	char	dev[TSFSCK_DEVNM_MAX], b[64];
	INT	n, i;

	fx_ev = (T_TSFSCKEV *)malloc(sizeof(T_TSFSCKEV) * FX_EV_MAX);
	if ( fx_ev == NULL ) return FALSE;
	n = ob_chk_boot_result((UB *)dev, sizeof(dev), &fx_res, fx_ev, FX_EV_MAX);
	if ( n <= 0 ) {
		return FALSE;
	}
	for ( i = 0; i < fx_ndev; i++ ) {
		if ( strcmp((char *)fx_dev[i].devnm, dev) == 0 ) fx_sel = i;
	}
	ui_set_value(&fx, DF_DEV, fx_sel + 1);
	(void)put(fx_run_dev, 0, sizeof(fx_run_dev), dev);
	fx_nev = fx_told = ( n < FX_EV_MAX ) ? n : FX_EV_MAX;
	fx_er = E_OK;
	fx_boot = TRUE;
	fx_run_named = TRUE;		/* attached since */
	fx_repair = TRUE;
	fx_state = FX_DONE;
	result_lines();
	snprintf(b, sizeof(b), "boot result %d %d", fx_res.problems, fx_res.remaining);
	note(b);
	return TRUE;
}

int main( void )
{
	T_OBNTM	m;
	INT	sw = 0, sh = 0, i;
	char	b[32];

	/* the window a little left of and above the middle of the screen, 障害情報 down and right of it */
	wm_new_centre(FX_W, FX_H, &sw, &sh);	/* in the middle of the screen new windows open on: sw, sh where its corner goes */
	ui_icon(SYSDEF_PROG_DISKFIX);
	ui_shows(SYSDEF_PROG_DISKFIX);
	ui_attr(UI_ATTR_HIDDEN | WM_ATTR_RESIZE);
	if ( ui_open(&fx, SYSDEF_BOX_DISKFIX, PN_MAIN, sw - 9, sh - 17) < E_OK ) {
		note("no window");
		return 1;
	}
	list_devices();
	for ( i = 0; i < FX_ROWS; i++ ) fx_line[i][0] = 0;
	if ( !boot_result() ) {
		line_set(0, "対象ドライブと処理方式を選んで、[開始]を押してください。");
	}
	show();
	ui_reveal(&fx);
	snprintf(b, sizeof(b), "opened %d", fx_ndev);
	note(b);
	while ( !g_quit ) {
		/* while the check runs, what it has told is looked at a few times a second */
		ER	er = ui_wait(&fx, &m, ( fx_state == FX_RUN ) ? 200 : TMO_FEVR);

		if ( er >= E_OK ) {
			if ( fi_up && ui_mine(&fi, &m) ) fi_event(&m);
			else fx_event(&m);
		}
		follow();
	}
	info_close();
	ui_close(&fx);
	note("closed");
	return 0;
}
