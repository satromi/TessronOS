/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfix.c
 *	ディスク修復: a native volume checked, and repaired
 *	(ts/tsfsck.h, docs/tessronos/11-desktop.md 11.25)
 *
 *	A window of the desktop's like the others (DTOWN): the frame moves,
 *	sizes and closes it, and the second button opens its menu, where
 *	開始, 障害情報 and what the panel chooses are too, between 閉じる and
 *	the tail of ウインドウ, 小物 and 実行. 〈対象ドライブ〉 lists every native
 *	volume the machine has, by its device name, label and size, the one
 *	the system runs on and those in use said so. 〈処理方式〉 is 検査 (look
 *	only, the default) or 修復, and 簡易, 通常 (the default) or 完全. 開始
 *	asks first when it is to repair, and for the system's own volume asks
 *	to start the machine again: that volume is repaired at the next
 *	start, before anything uses it, and the window then opens by itself
 *	with what the repair found and did.
 *
 *	The check runs in a task of its own, the window following it: the
 *	item being looked at, how many things have been found, and at the
 *	end what was found, what was put right and what is left. 障害情報
 *	opens a second window with every thing told, page by page; 保存 there
 *	writes it as the text object ディスク修復記録 linked from the system
 *	box (/SYS), and closing the first window closes it too.
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
#include <ts/tsfsck.h>
#include <ts/fquit.h>
#include "desktop.h"

#define KEY_ENTER	0x28
#define KEY_ESCAPE	0x29

#define FX_W		600		/* the window's panel */
#define FX_H		476
#define FX_ROWS		7		/* lines of what is under way and what came of it */
#define FX_ROW0		232
#define FX_ROW_H	24
#define FX_EV_MAX	4000		/* things told, kept for 障害情報 */
#define FX_STACK	(32 * 1024)
#define FX_PRI		22		/* the check's task: below the desktop's */

#define FI_W		660		/* 障害情報 */
#define FI_ROWS		18
#define FI_ROW_H	22
#define FI_H		( 40 + FI_ROWS * FI_ROW_H + 48 )

/* the parts */
#define PN_DEV0		1		/* the list of the volumes */
#define PN_CHECK	20
#define PN_REPAIR	21
#define PN_QUICK	22
#define PN_NORMAL	23
#define PN_FULL		24
#define ANS_START	( WM_ANS_ACT + 1 )
#define ANS_INFO	( WM_ANS_ACT + 2 )
#define ANS_END		( WM_ANS_ACT + 3 )
#define ANS_PREV	( WM_ANS_ACT + 4 )
#define ANS_NEXT	( WM_ANS_ACT + 5 )
#define ANS_SAVE	( WM_ANS_ACT + 6 )

#define FX_SETUP	0
#define FX_RUN		1
#define FX_DONE		2

#define LOG_NAME	"ディスク修復記録"

/* A window of the two, one of the desktop's (DTOWN), and the panel filling it */
typedef struct {
	DTWIN		*d;
	INT		wid;
	INT		pid;
} FXWIN;

LOCAL FXWIN		fx = { NULL, -1, -1 };
LOCAL FXWIN		fi = { NULL, -1, -1 };	/* 障害情報 */
LOCAL CONST DTOWN	fx_own, fi_own;
LOCAL INT		fi_page = 0;

LOCAL T_TSFSCKDEV	*fx_dev = NULL;	/* every volume, in a list that rolls */
LOCAL INT		fx_ndev = 0, fx_room = 0;
LOCAL INT		fx_sel = 0;
LOCAL BOOL		fx_repair = FALSE;
LOCAL INT		fx_depth = 1;		/* 簡易 0, 通常 1, 完全 2 */
LOCAL INT		fx_state = FX_SETUP;
LOCAL BOOL		fx_boot = FALSE;	/* what is shown is the repair at start */
LOCAL BOOL		fx_boot_seen = FALSE;	/* the repair at start shown already */
LOCAL UB		fx_run_dev[TSFSCK_DEVNM_MAX];
LOCAL UINT		fx_run_mode = 0;
LOCAL BOOL		fx_run_named = FALSE;	/* its objects can be named: the volume is attached */

/* shared with the check's task */
LOCAL ID		fx_mtx = 0;
LOCAL T_TSFSCKEV	*fx_ev = NULL;
LOCAL INT		fx_nev = 0;		/* kept */
LOCAL INT		fx_told = 0;		/* told, kept or not */
LOCAL BOOL		fx_done = FALSE;
LOCAL ER		fx_er = E_OK;
LOCAL T_TSFSCK		fx_res;
LOCAL INT		fx_phase = 0;
LOCAL INT		fx_seen = 0;		/* events the window has followed */
LOCAL INT		fx_found = 0;		/* things found so far, for the window */

LOCAL UB		fx_line[FX_ROWS][WM_LABEL_MAX];

/*
 * What happened, told on the console with how many times it has, so
 * that one reading the console can wait for the next of a kind
 */
#define SN_ASK		0
#define SN_ANSWER	1
#define SN_INFO		2
#define SN_INFOEND	3
#define SN_SAVED	4
#define SN_DONE		5
#define SN_MENU		6
#define SN_KINDS	7
LOCAL INT		fx_told_kind[SN_KINDS];

LOCAL INT told( INT kind )
{
	return ++fx_told_kind[kind];
}

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

LOCAL void line_set( INT i, CONST UB *s )
{
	if ( i >= 0 && i < FX_ROWS ) {
		(void)put(fx_line[i], 0, WM_LABEL_MAX, (CONST char *)s);
	}
}

LOCAL CONST char *phase_name( INT ph )
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

LOCAL CONST char *kind_name( INT k )
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

LOCAL CONST char *why_name( D w )
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

LOCAL CONST char *part_name( D off )
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

/* An object as it is written in a line: its name when the store knows it, and the head of its UUID */
LOCAL INT put_obj( UB *b, INT n, INT max, CONST TS_UUID *u )
{
	char	s[TS_UUID_STRLEN + 1];
	UB	name[64];

	(void)ts_uuid_to_str(u, s, sizeof(s));
	s[18] = 0;
	n = put(b, n, max, "実身");
	if ( fx_run_named && om_store_name(u, name, sizeof(name)) > 0 ) {
		n = put(b, n, max, "「");
		n = put(b, n, max, (CONST char *)name);
		n = put(b, n, max, "」");
	}
	n = put(b, n, max, "(");
	n = put(b, n, max, s);
	return put(b, n, max, "…)");
}

LOCAL INT put_blk( UB *b, INT n, INT max, UD blk )
{
	n = put(b, n, max, "ブロック");
	return put_num(b, n, max, (D)blk);
}

/* One thing told, in words; 0 for an event that is not a line */
LOCAL INT ev_text( CONST T_TSFSCKEV *e, UB *b, INT max )
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

LOCAL INT sum( CONST INT *k )
{
	INT	i, s = 0;

	for ( i = 1; i < TSFSCK_NKIND; i++ ) s += k[i];
	return s;
}

/* The lines the window shows when the check is over */
LOCAL void result_lines( void )
{
	UB	b[WM_LABEL_MAX];
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
		line_set(1, (CONST UB *)"障害は見つかりませんでした。");
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
			line_set(i++, (CONST UB *)"使用中の実身があるため、ディスクを取り外して修復できませんでした。");
			line_set(i++, (CONST UB *)"実身を閉じてから、もう一度修復してください。");
		} else if ( fx_res.refused ) {
			line_set(i++, (CONST UB *)"修復できない障害があるため、ディスクを書き換えませんでした。");
		} else {
			if ( fx_res.detached ) {
				line_set(i++, (CONST UB *)"使用中のディスクを取り外して修復し、取り付け直しました。");
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
		line_set(i++, (CONST UB *)"〈修復〉を選んで開始すると、修復を試みます。");
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
		line_set(FX_ROWS - 2, (CONST UB *)"くわしくは〈障害情報〉で見られます。");
	}
}

/* The lines while the check runs: the item being looked at, and how many found so far */
LOCAL void running_lines( void )
{
	UB	b[WM_LABEL_MAX];
	INT	n, i;

	for ( i = 0; i < FX_ROWS; i++ ) fx_line[i][0] = 0;
	n = put(b, 0, sizeof(b), ( ( fx_run_mode & TSFSCK_REPAIR ) != 0 ) ? "修復しています: " : "検査しています: ");
	(void)put(b, n, sizeof(b), (CONST char *)fx_run_dev);
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

LOCAL void take_ev( void *ctx, CONST T_TSFSCKEV *ev )
{
	(void)ctx;
	tk_loc_mtx(fx_mtx, TMO_FEVR);
	if ( fx_ev != NULL && fx_nev < FX_EV_MAX ) {
		fx_ev[fx_nev++] = *ev;
	}
	fx_told++;
	tk_unl_mtx(fx_mtx);
}

LOCAL void worker( INT stacd, void *exinf )
{
	T_TSFSCK	r;
	ER		er;

	(void)stacd;
	(void)exinf;
	knl_memset(&r, 0, sizeof(r));
	er = ob_chk_run((CONST char *)fx_run_dev, fx_run_mode, &r, take_ev, NULL);
	tk_loc_mtx(fx_mtx, TMO_FEVR);
	fx_res = r;
	fx_er = er;
	fx_done = TRUE;
	tk_unl_mtx(fx_mtx);
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

/* What was chosen in the panel, kept before it is made again */
LOCAL void keep_choices( void )
{
	INT	i;

	if ( fx.pid < 0 || fx_state == FX_RUN ) {
		return;
	}
	i = dt_pn_value(fx.pid, PN_DEV0);
	if ( i >= 1 && i <= fx_ndev ) fx_sel = i - 1;
	fx_repair = (BOOL)( dt_pn_value(fx.pid, PN_REPAIR) != 0 );
	fx_depth = ( dt_pn_value(fx.pid, PN_QUICK) != 0 ) ? 0 : ( dt_pn_value(fx.pid, PN_FULL) != 0 ) ? 2 : 1;
}

LOCAL T_WMPANEL *build( void )
{
	T_WMPANEL	*def = dt_pn_new(FX_W, FX_H);
	BOOL		run = (BOOL)( fx_state == FX_RUN );
	UB		b[WM_LABEL_MAX];
	T_WMPART	*lst;
	INT		i, n, k, at = -1, cnt = 0;

	if ( def == NULL ) {
		return NULL;
	}
	def->kind = WM_PNL_BARE;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 6, 300, 22, "対象ドライブ");
	/* every volume there is, in a list that rolls; chosen by its row */
	lst = dt_pn_add(def, WM_PT_LIST | ( run ? P_DISABLE : 0 ), PN_DEV0, 14, 32, 350, 150, NULL);
	for ( i = 0; i < fx_ndev; i++ ) {
		CONST T_TSFSCKDEV	*d = &fx_dev[i];

		n = put(b, 0, sizeof(b), (CONST char *)d->devnm);
		n = put(b, n, sizeof(b), "  ");
		n = put(b, n, sizeof(b), ( d->label[0] != 0 ) ? (CONST char *)d->label : "(名前なし)");
		n = put(b, n, sizeof(b), "  ");
		n = put_num(b, n, sizeof(b), (D)( d->blocks * TSFSBLK_BLOCK_SIZE / ( 1024 * 1024 ) ));
		n = put(b, n, sizeof(b), "MB");
		if ( d->system ) n = put(b, n, sizeof(b), "  システム");
		else if ( d->attached ) n = put(b, n, sizeof(b), "  使用中");
		k = wm_panel_name(def, b);
		if ( k < 0 ) {
			break;			/* no room for more names */
		}
		if ( i == 0 ) at = k;
		cnt++;
	}
	if ( lst != NULL ) {
		lst->pool = ( at >= 0 ) ? at : 0;
		lst->count = cnt;
		lst->top = ( fx_sel >= 5 ) ? fx_sel - 4 + 1 : 1;
		lst->value = ( fx_sel < cnt ) ? fx_sel + 1 : 0;
	}
	if ( fx_ndev == 0 ) {
		dt_pn_add(def, WM_PT_LABEL, 0, 24, 32, 280, 22, "ディスクがありません");
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 380, 6, 200, 22, "処理方式");
	(void)choice(def, PN_CHECK, PN_CHECK, 380, 32, 120, "検査", (BOOL)!fx_repair, run);
	(void)choice(def, PN_REPAIR, PN_CHECK, 380, 58, 120, "修復", fx_repair, run);
	(void)choice(def, PN_QUICK, PN_QUICK, 380, 98, 120, "簡易", (BOOL)( fx_depth == 0 ), run);
	(void)choice(def, PN_NORMAL, PN_QUICK, 380, 124, 120, "通常", (BOOL)( fx_depth == 1 ), run);
	(void)choice(def, PN_FULL, PN_QUICK, 380, 150, 120, "完全", (BOOL)( fx_depth == 2 ), run);
	(void)button(def, ANS_START, ANS_START, FX_W - 114, 188, 100, "開始", (BOOL)( run || fx_ndev == 0 ));
	dt_pn_add(def, WM_PT_LINE, 0, 14, FX_ROW0 - 10, FX_W - 28, 1, NULL);
	for ( i = 0; i < FX_ROWS; i++ ) {
		T_WMPART	*pt = dt_pn_add(def, WM_PT_LABEL, 0, 14, FX_ROW0 + i * FX_ROW_H, FX_W - 28, 22, NULL);

		if ( pt != NULL ) dt_pn_text(pt->label, WM_LABEL_MAX, (CONST char *)fx_line[i]);
	}
	(void)button(def, ANS_INFO, ANS_INFO, 14, FX_H - 36, 120, "障害情報",
		     (BOOL)( fx_state != FX_DONE || fx_nev == 0 ));
	(void)button(def, ANS_END, ANS_END, FX_W - 114, FX_H - 36, 100, "終了", run);
	return def;
}

/* The window's panel made again from what it shows now, what was chosen in it kept first or not */
LOCAL void rebuild( BOOL keep )
{
	T_WMPANEL	*def;
	INT		pw, ph;

	if ( fx.wid < 0 ) {
		return;
	}
	if ( keep ) {
		keep_choices();
	}
	def = build();
	if ( def == NULL ) {
		return;
	}
	pw = def->r.right - def->r.left;
	ph = def->r.bottom - def->r.top;
	if ( fx.pid >= 0 ) {
		(void)wm_panel_close(fx.pid);
	}
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = pw;
	def->r.bottom = ph;
	fx.pid = wm_panel_open(fx.wid, def);
	Kfree(def);
	if ( fx.pid >= 0 ) {
		dt_draw(fx.d);
	}
	wm_update();
}

LOCAL void show( void )
{
	rebuild(TRUE);
}

/*
 * A window of the desktop's whose work area is a panel's size, framed,
 * the panel's corner a little left and above the middle of the screen
 */
LOCAL DTWIN *open_framed( CONST DTOWN *own, INT pw, INT ph, INT dx, INT dy, CONST char *title )
{
	T_DPRECT	o;
	T_WMWIN		w;
	DTWIN		*d;
	INT		sw = 0, sh = 0, wl;

	wm_screen_main(&sw, &sh);
	wl = ( sw - pw ) / 2 - 2 + dx;
	o.left = wl - 10;
	o.top = ( sh - ph ) / 2 - 20 + dy;
	if ( o.top < 0 ) o.top = 0;
	o.right = o.left + pw + 26;
	o.bottom = o.top + ph + 50;
	d = dt_own_open(own, NULL, &o, WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE, title, "disk-repair");
	if ( d == NULL ) {
		return NULL;
	}
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		/* the frame measured: the work area where it was asked for, and the panel's size */
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

/* A question over the window: TRUE for the button that agrees */
LOCAL BOOL ask( CONST char *l1, CONST char *l2, CONST char *l3, CONST char *yes )
{
	T_WMPANEL	*def;
	T_WMPART	*pt;
	T_WMEV		ev;
	INT		pid, pw = 520, ph = 140;
	UINT		ans = WM_ANS_NONE;

	def = dt_pn_new(pw, ph);
	if ( def == NULL ) {
		return FALSE;
	}
	def->kind = WM_PNL_WARN;
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 10, pw - 28, 22, l1);
	if ( l2 != NULL ) dt_pn_add(def, WM_PT_LABEL, 0, 14, 34, pw - 28, 22, l2);
	if ( l3 != NULL ) dt_pn_add(def, WM_PT_LABEL, 0, 14, 58, pw - 28, 22, l3);
	pt = dt_pn_add(def, WM_PT_BUTTON, DT_PN_CANCEL, pw - 212, ph - 36, 94, 26, "取消");
	if ( pt != NULL ) pt->answer = WM_ANS_CANCEL;
	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, pw - 108, ph - 36, 94, 26, yes);
	if ( pt != NULL ) pt->answer = WM_ANS_OK;
	pid = wm_panel_open_centre(fx.wid, def);
	Kfree(def);
	if ( pid < 0 ) {
		return FALSE;
	}
	wm_focus(fx.wid);
	wm_panel_draw(pid);
	wm_update();
	tm_printf((UB *)"dfix: ask %d %s\n", told(SN_ASK), yes);
	while ( ans == WM_ANS_NONE ) {
		if ( wm_read_event(&ev, 2000) < E_OK ) {
			continue;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ENTER ) {
			ans = WM_ANS_OK;
			break;
		}
		if ( ev.type == HID_EV_KEY_DOWN && ev.code == KEY_ESCAPE ) {
			ans = WM_ANS_CANCEL;
			break;
		}
		if ( ev.type == HID_EV_KEY_DOWN || ev.type == HID_EV_KEY_UP ) {
			ev.wid = fx.wid;
		}
		if ( wm_panel_event(pid, &ev, &ans) < E_OK ) {
			ans = WM_ANS_CANCEL;
		}
		wm_update();
	}
	(void)wm_panel_close(pid);
	if ( fx.pid >= 0 ) wm_panel_draw(fx.pid);
	wm_update();
	tm_printf((UB *)"dfix: answered %d %d\n", told(SN_ANSWER), ( ans == WM_ANS_OK ) ? 1 : 0);
	return (BOOL)( ans == WM_ANS_OK );
}

/* ---------------------------------------------------------------- starting */

LOCAL void list_devices( void )
{
	/* as many as there are: asked again with room for them while that is more than the room */
	while ( ( fx_ndev = ob_chk_list(fx_dev, fx_room) ) > fx_room ) {
		T_TSFSCKDEV	*n = (T_TSFSCKDEV *)Kmalloc(sizeof(T_TSFSCKDEV) * (SZ)( fx_ndev + 8 ));

		if ( n == NULL ) {
			fx_ndev = fx_room;
			break;
		}
		if ( fx_dev != NULL ) Kfree(fx_dev);
		fx_dev = n;
		fx_room = fx_ndev + 8;
	}
	if ( fx_ndev < 0 ) fx_ndev = 0;
	if ( fx_sel >= fx_ndev ) fx_sel = 0;
}

/* The system started again, to repair its own volume before it uses it */
LOCAL void restart_for( CONST T_TSFSCKDEV *d, UINT mode )
{
	ID	key;
	ER	er;

	er = ob_chk_request((CONST char *)d->devnm, mode);
	if ( er < E_OK ) {
		(void)wm_msg_put("ディスク修復: 修復の依頼を書けませんでした");
		return;
	}
	tm_printf((UB *)"dfix: restart for %s\n", d->devnm);
	key = ob_opn_obj(&ob_uuid_system, OB_OP_WRITE);
	if ( key > 0 ) {
		er = ob_wri_rec(key, OB_SYS_POWER, 0, "RESTART", 7, NULL);
		(void)ob_cls_obj(key);
	}
	if ( key <= 0 || er < E_OK ) {
		(void)wm_msg_put("ディスク修復: 次に起動したときに修復します");
	}
}

LOCAL void start( void )
{
	CONST T_TSFSCKDEV	*d;
	T_CTSK			ct;
	UINT			mode;
	ID			tid;
	INT			i;

	keep_choices();
	if ( fx_state == FX_RUN || fx_ndev == 0 ) {
		return;
	}
	d = &fx_dev[fx_sel];
	mode = ( fx_repair ? TSFSCK_REPAIR : 0 ) | ( fx_depth == 0 ? TSFSCK_QUICK : fx_depth == 2 ? TSFSCK_FULL : 0 );
	if ( fx_repair ) {
		if ( !ask("修復を行うと、ディスクの状態によってはデータの一部が", "失われることがあります。",
			  "実際に修復を行う場合は[実行]を押してください。", "実行") ) {
			return;
		}
		if ( d->system ) {
			if ( ask("このディスクでシステムが動いています。修復はシステムを", "再起動し、ディスクを使い始める前に行います。",
				 "[実行]を押すと再起動します。", "実行") ) {
				restart_for(d, mode);
			}
			return;
		}
	}
	if ( fx_ev == NULL ) {
		fx_ev = (T_TSFSCKEV *)Kmalloc(sizeof(T_TSFSCKEV) * FX_EV_MAX);
	}
	if ( fx_mtx <= 0 ) {
		T_CMTX	cm;

		cm.exinf = NULL;
		cm.mtxatr = TA_TFIFO;
		fx_mtx = tk_cre_mtx(&cm);
		if ( fx_mtx <= 0 ) return;
	}
	for ( i = 0; d->devnm[i] != 0 && i < TSFSCK_DEVNM_MAX - 1; i++ ) fx_run_dev[i] = d->devnm[i];
	fx_run_dev[i] = 0;
	fx_run_mode = mode;
	fx_run_named = d->attached;
	fx_nev = fx_told = fx_seen = fx_found = 0;
	fx_done = FALSE;
	fx_er = E_OK;
	fx_phase = 0;
	fx_boot = FALSE;
	knl_memset(&fx_res, 0, sizeof(fx_res));

	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)worker;
	ct.itskpri = FX_PRI;
	ct.stksz = FX_STACK;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		(void)wm_msg_put("ディスク修復: 検査を始められませんでした");
		return;
	}
	fx_state = FX_RUN;
	tm_printf((UB *)"dfix: started %s %d\n", fx_run_dev, (INT)mode);
	running_lines();
	show();
}

/* ---------------------------------------------------------------- 障害情報 */

/* The lines of what was told, the phases as headings */
LOCAL INT info_count( void )
{
	return fx_nev;
}

LOCAL T_WMPANEL *info_build( void )
{
	T_WMPANEL	*def = dt_pn_new(FI_W, FI_H);
	UB		b[WM_LABEL_MAX];
	INT		i, k, n = info_count(), pages = ( n + FI_ROWS - 1 ) / FI_ROWS;

	if ( def == NULL ) {
		return NULL;
	}
	def->kind = WM_PNL_BARE;
	if ( pages < 1 ) pages = 1;
	if ( fi_page >= pages ) fi_page = pages - 1;
	k = put(b, 0, sizeof(b), (CONST char *)fx_run_dev);
	k = put(b, k, sizeof(b), "  ");
	k = put_num(b, k, sizeof(b), fi_page + 1);
	k = put(b, k, sizeof(b), "/");
	k = put_num(b, k, sizeof(b), pages);
	k = put(b, k, sizeof(b), "ページ");
	if ( fx_told > fx_nev ) {
		k = put(b, k, sizeof(b), "(ほかに");
		k = put_num(b, k, sizeof(b), fx_told - fx_nev);
		(void)put(b, k, sizeof(b), "件)");
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 6, FI_W - 28, 22, (CONST char *)b);
	for ( i = 0; i < FI_ROWS; i++ ) {
		INT	e = fi_page * FI_ROWS + i;

		if ( e >= n ) break;
		(void)ev_text(&fx_ev[e], b, sizeof(b));
		dt_pn_add(def, WM_PT_LABEL, 0, 14, 36 + i * FI_ROW_H, FI_W - 28, 20, (CONST char *)b);
	}
	(void)button(def, ANS_PREV, ANS_PREV, 14, FI_H - 36, 90, "前へ", (BOOL)( fi_page == 0 ));
	(void)button(def, ANS_NEXT, ANS_NEXT, 112, FI_H - 36, 90, "次へ", (BOOL)( fi_page + 1 >= pages ));
	(void)button(def, ANS_SAVE, ANS_SAVE, FI_W - 212, FI_H - 36, 94, "保存", FALSE);
	(void)button(def, ANS_END, ANS_END, FI_W - 108, FI_H - 36, 94, "終了", FALSE);
	return def;
}

LOCAL void info_show( void )
{
	T_WMPANEL	*def;
	INT		pw, ph;

	if ( fi.wid < 0 ) {
		return;
	}
	def = info_build();
	if ( def == NULL ) {
		return;
	}
	pw = def->r.right - def->r.left;
	ph = def->r.bottom - def->r.top;
	if ( fi.pid >= 0 ) (void)wm_panel_close(fi.pid);
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = pw;
	def->r.bottom = ph;
	fi.pid = wm_panel_open(fi.wid, def);
	Kfree(def);
	if ( fi.pid >= 0 ) dt_draw(fi.d);
	wm_update();
	tm_printf((UB *)"dfix: info %d page %d\n", told(SN_INFO), fi_page + 1);
}

LOCAL void info_open( void )
{
	if ( fi.wid >= 0 ) {
		wm_raise(fi.wid);
		wm_focus(fi.wid);
		info_show();
		return;
	}
	fi_page = 0;
	fi.d = open_framed(&fi_own, FI_W + 2 * DT_PN_IN, FI_H + 2 * DT_PN_IN, 40, 30, "障害情報");
	if ( fi.d == NULL ) {
		return;
	}
	fi.wid = fi.d->wid;
	fi.pid = -1;
	info_show();
}

/* 障害情報 closed: its window goes, and with it the panel (fi_close) */
LOCAL void info_close( void )
{
	if ( fi.d != NULL ) {
		dt_close(fi.d);
		wm_update();
	}
}

LOCAL void fi_close( DTWIN *d )
{
	(void)d;
	if ( fi.pid >= 0 ) (void)wm_panel_close(fi.pid);
	fi.d = NULL;
	fi.wid = fi.pid = -1;
	tm_printf((UB *)"dfix: info closed %d\n", told(SN_INFOEND));
}

/* Words inside an element of xmlTAD, the three that cannot stand there written as entities */
LOCAL INT put_text( UB *b, INT n, INT max, CONST UB *s )
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

/* The object of that name the system box links to, or a new one linked from it */
LOCAL ER log_object( CONST TS_UUID *box, TS_UUID *p_u )
{
	TS_UUID	*ids;
	UB	*rec, name[64];
	SZ	len = 0;
	INT	n, i;
	ER	er;

	rec = om_obj_record(box, 0, &len);
	if ( rec != NULL ) {
		/* every link of the box: asked how many, then taken */
		n = om_store_links(rec, len, NULL, 0);
		ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( ( n > 0 ) ? n : 1 ));
		n = ( ids != NULL ) ? om_store_links(rec, len, ids, n) : 0;
		Kfree(rec);
		for ( i = 0; i < n; i++ ) {
			if ( om_store_name(&ids[i], name, sizeof(name)) > 0
			  && knl_strcmp((char *)name, LOG_NAME) == 0 ) {
				*p_u = ids[i];
				Kfree(ids);
				return E_OK;
			}
		}
		if ( ids != NULL ) Kfree(ids);
	}
	er = om_cre_obj(NULL, (CONST UB *)LOG_NAME, OM_BODY_DOC, p_u);
	if ( er >= E_OK ) {
		er = om_obj_link_add(box, p_u);
	}
	return er;
}

/* 保存: what was told, as the text object ディスク修復記録 of the system box */
LOCAL void info_save( void )
{
	TS_UUID	box, u;
	UB	*x, b[WM_LABEL_MAX];
	INT	n = info_count(), at, max = 512 + ( n + FX_ROWS ) * ( WM_LABEL_MAX + 16 ), i;
	ER	er;

	if ( ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &box) < E_OK ) {
		return;
	}
	x = (UB *)Kmalloc((SZ)max);
	if ( x == NULL ) {
		return;
	}
	at = put(x, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"" LOG_NAME "\">\n<document>\n<p>");
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
	er = log_object(&box, &u);
	if ( er >= E_OK ) {
		er = om_obj_record_put(&u, 0, OB_RT_TAD, 0, x, (SZ)at);
		om_store_forget(&u);
	}
	Kfree(x);
	(void)wm_msg_put(( er >= E_OK ) ? "ディスク修復: 障害情報を「" LOG_NAME "」に保存しました"
					: "ディスク修復: 障害情報を保存できませんでした");
	tm_printf((UB *)"dfix: saved %d %d\n", told(SN_SAVED), (INT)er);
}

/* ---------------------------------------------------------------- the calls */

/* ディスク修復's window closed: 障害情報 goes with it */
LOCAL void fx_close( DTWIN *d )
{
	(void)d;
	if ( fx.pid >= 0 ) (void)wm_panel_close(fx.pid);
	fx.d = NULL;
	fx.wid = fx.pid = -1;
	info_close();
	if ( fx_state != FX_RUN ) {
		fx_state = FX_SETUP;
	}
	tm_printf((UB *)"dfix: closed\n");
}

/* Not while the check runs: it runs to its end */
LOCAL BOOL fx_closing( DTWIN *d )
{
	(void)d;
	if ( fx_state == FX_RUN ) {
		(void)wm_msg_put("ディスク修復: 検査が終わるまで閉じられません");
		return FALSE;
	}
	return TRUE;
}

LOCAL void fx_open_window( void )
{
	fx.d = open_framed(&fx_own, FX_W + 2 * DT_PN_IN, FX_H + 2 * DT_PN_IN, 0, 0, "ディスク修復");
	if ( fx.d == NULL ) {
		return;
	}
	fx.wid = fx.d->wid;
	fx.pid = -1;
}

EXPORT void dt_fix_open( void )
{
	INT	i;

	if ( fx.wid >= 0 ) {
		wm_raise(fx.wid);
		wm_focus(fx.wid);
		wm_update();
		return;
	}
	if ( fx_state == FX_RUN ) {
		/* the window closed by someone else while the check ran: it shows the check again */
		fx_open_window();
		running_lines();
		show();
		tm_printf((UB *)"dfix: opened %d\n", fx_ndev);
		return;
	}
	list_devices();
	fx_state = FX_SETUP;
	fx_repair = FALSE;
	fx_depth = 1;
	for ( i = 0; i < FX_ROWS; i++ ) fx_line[i][0] = 0;
	line_set(0, (CONST UB *)"対象ドライブと処理方式を選んで、[開始]を押してください。");
	fx_open_window();
	show();
	tm_printf((UB *)"dfix: opened %d\n", fx_ndev);
}

/*
 * At start: when the system volume was repaired before it was attached,
 * the window opens by itself with what the repair found and did.
 */
EXPORT void dt_fix_boot( void )
{
	UB	dev[TSFSCK_DEVNM_MAX];
	INT	n, i;

	/*
	 * once for a start of the machine: a desktop started again does not
	 * show it again, nor one started again by 強制終了, whose variables
	 * were put back as they were when the machine started
	 */
	if ( fx_boot_seen || knl_fq_restarts() > 0 ) {
		return;
	}
	fx_boot_seen = TRUE;
	if ( fx_ev == NULL ) {
		fx_ev = (T_TSFSCKEV *)Kmalloc(sizeof(T_TSFSCKEV) * FX_EV_MAX);
		if ( fx_ev == NULL ) return;
	}
	n = ob_chk_boot_result(dev, sizeof(dev), &fx_res, fx_ev, FX_EV_MAX);
	if ( n <= 0 ) {
		return;
	}
	list_devices();
	for ( i = 0; i < fx_ndev; i++ ) {
		if ( knl_strcmp((char *)fx_dev[i].devnm, (char *)dev) == 0 ) fx_sel = i;
	}
	for ( i = 0; dev[i] != 0 && i < TSFSCK_DEVNM_MAX - 1; i++ ) fx_run_dev[i] = dev[i];
	fx_run_dev[i] = 0;
	fx_nev = fx_told = ( n < FX_EV_MAX ) ? n : FX_EV_MAX;
	fx_er = E_OK;
	fx_boot = TRUE;
	fx_run_named = TRUE;		/* attached since */
	fx_repair = TRUE;
	fx_state = FX_DONE;
	result_lines();
	fx_open_window();
	show();
	tm_printf((UB *)"dfix: boot result %d %d\n", fx_res.problems, fx_res.remaining);
}

/* The check followed from the desktop's loop, whether the window is open or not */
EXPORT void dt_fix_poll( void )
{
	INT	n, i;
	BOOL	done, moved = FALSE;

	if ( fx_state != FX_RUN || fx_mtx <= 0 ) {
		return;
	}
	tk_loc_mtx(fx_mtx, TMO_FEVR);
	n = fx_nev;
	done = fx_done;
	tk_unl_mtx(fx_mtx);
	for ( i = fx_seen; i < n; i++ ) {
		CONST T_TSFSCKEV	*e = &fx_ev[i];

		if ( e->kind == TSFSCK_K_PHASE ) {
			fx_phase = e->phase;
			tm_printf((UB *)"dfix: phase %d\n", e->phase);
			moved = TRUE;
		} else if ( !e->fixed && e->phase != TSFSCK_PH_VERIFY && e->phase != TSFSCK_PH_REPAIR ) {
			fx_found++;
			moved = TRUE;
		}
	}
	fx_seen = n;
	if ( done ) {
		fx_state = FX_DONE;
		result_lines();
		show();
		tm_printf((UB *)"dfix: done %d: found %d fixed %d left %d refused %d unfixable %d er %d\n", told(SN_DONE), fx_res.problems,
			  sum(fx_res.fixed), fx_res.remaining, fx_res.refused ? 1 : 0, fx_res.unfixable, (INT)fx_er);
		list_devices();
		return;
	}
	if ( moved ) {
		running_lines();
		show();
	}
}

/* ---------------------------------------------------------------- the two windows */

/* A window of the two drawn: the panel, on a panel's ground where the window is larger */
LOCAL void fx_draw( DTWIN *d )
{
	FXWIN	*w = ( d == fi.d ) ? &fi : &fx;

	dt_own_ground(d);
	if ( w->pid >= 0 ) {
		wm_panel_draw(w->pid);
	}
	wm_update();
}

/*
 * An event in the work area of one of the two: its panel's. The first
 * button only; a key with Ctrl is left to the menu's letters.
 */
LOCAL UINT panel_input( FXWIN *w, CONST T_WMEV *ev, BOOL *p_took )
{
	UINT	ans = WM_ANS_NONE;

	*p_took = TRUE;
	if ( w->pid < 0 ) {
		*p_took = FALSE;
		return WM_ANS_NONE;
	}
	if ( ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP )
	  && ( ev->mods & ( HID_MOD_LCTRL | HID_MOD_RCTRL ) ) != 0 ) {
		*p_took = FALSE;
		return WM_ANS_NONE;
	}
	if ( ( ev->type == HID_EV_BTN_DOWN || ev->type == HID_EV_BTN_UP ) && ev->code != 0 ) {
		return WM_ANS_NONE;
	}
	if ( wm_panel_event(w->pid, ev, &ans) < E_OK ) {
		ans = WM_ANS_NONE;
	}
	if ( ev->type != HID_EV_MOVE ) {
		wm_panel_draw(w->pid);
	}
	wm_update();
	return ans;
}

LOCAL BOOL fx_input( DTWIN *d, CONST T_WMEV *ev )
{
	BOOL	took;

	(void)d;
	switch ( panel_input(&fx, ev, &took) ) {
	case ANS_START:	start();			break;
	case ANS_INFO:	keep_choices(); info_open();	break;
	case ANS_END:	dt_close(fx.d);			break;
	default:	break;
	}
	return took;
}

LOCAL BOOL fi_input( DTWIN *d, CONST T_WMEV *ev )
{
	BOOL	took;

	(void)d;
	switch ( panel_input(&fi, ev, &took) ) {
	case ANS_PREV:	if ( fi_page > 0 ) fi_page--;	info_show();	break;
	case ANS_NEXT:	fi_page++;			info_show();	break;
	case ANS_SAVE:	info_save();					break;
	case ANS_END:	info_close();					break;
	default:	break;
	}
	return took;
}

/*
 * The menu of ディスク修復: 閉じる, 開始, 障害情報, 検査 and 修復, 簡易,
 * 通常 and 完全 as the panel's choices, ドライブの一覧を更新, and the tail.
 * What cannot be done while the check runs is grey.
 */
LOCAL void fx_menu_set( DTWIN *d, ID mid )
{
	BOOL	run = (BOOL)( fx_state == FX_RUN );

	(void)d;
	keep_choices();
	tm_printf((UB *)"dfix: menu %d\n", told(SN_MENU));
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

LOCAL void fx_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	(void)d;
	if ( mn_is(sel, "dfix.start") ) {
		start();
		return;
	}
	if ( mn_is(sel, "dfix.info") ) {
		keep_choices();
		info_open();
		return;
	}
	if ( fx_state == FX_RUN ) {
		return;
	}
	keep_choices();
	if ( mn_is(sel, "dfix.check") ) fx_repair = FALSE;
	else if ( mn_is(sel, "dfix.repair") ) fx_repair = TRUE;
	else if ( mn_is(sel, "dfix.quick") ) fx_depth = 0;
	else if ( mn_is(sel, "dfix.normal") ) fx_depth = 1;
	else if ( mn_is(sel, "dfix.full") ) fx_depth = 2;
	else if ( mn_is(sel, "dfix.list") ) list_devices();
	else return;
	rebuild(FALSE);			/* the panel made again from what was chosen here */
}

/* The menu of 障害情報: 閉じる, 前へ, 次へ, 保存 and the tail */
LOCAL void fi_menu_set( DTWIN *d, ID mid )
{
	INT	pages = ( info_count() + FI_ROWS - 1 ) / FI_ROWS;

	(void)d;
	tm_printf((UB *)"dfix: menu %d\n", told(SN_MENU));
	(void)mn_chg_atr(mid, NULL, "dfix.prev", ( fi_page == 0 ) ? MN_GREY : 0);
	(void)mn_chg_atr(mid, NULL, "dfix.next", ( fi_page + 1 >= pages ) ? MN_GREY : 0);
}

LOCAL void fi_menu_do( DTWIN *d, CONST T_MNSEL *sel )
{
	(void)d;
	if ( mn_is(sel, "dfix.prev") ) {
		if ( fi_page > 0 ) fi_page--;
		info_show();
	} else if ( mn_is(sel, "dfix.next") ) {
		fi_page++;
		info_show();
	} else if ( mn_is(sel, "dfix.save") ) {
		info_save();
	}
}

LOCAL CONST DTOWN	fx_own = {
	SYSDEF_MENU_DISKFIX,
	fx_draw,
	fx_input,
	NULL,
	fx_menu_set,
	fx_menu_do,
	fx_closing,
	fx_close,
	NULL
};

LOCAL CONST DTOWN	fi_own = {
	SYSDEF_MENU_DFIX_INFO,
	fx_draw,
	fi_input,
	NULL,
	fi_menu_set,
	fi_menu_do,
	NULL,
	fi_close,
	NULL
};
