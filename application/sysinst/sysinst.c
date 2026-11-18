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
 *	システム登録: what the program (application/sysinst/si_main.c, a
 *	process) asks of the desktop (DT_RQ_SYSINST, ts/dtreq.h;
 *	docs/tessronos/03-platform.md 3.10.10, 11-desktop.md 11.32)
 *
 *	The disks, the plan and the copy are the kernel's (si_core.c): the
 *	disks are read through their devices, and the system volume is
 *	copied while its journal is held. The desktop lists the disks and the
 *	partitions that can go as the rows the program shows, looks at the
 *	way chosen and keeps the plan, says in lines what will be done and
 *	what came of it, and does the plan in a task of its own -- for an
 *	administrator only -- whose steps and bytes the program asks for
 *	while it runs. The task runs to its end whatever the program does.
 *	After it went well the machine may be started again.
 *
 *	Each step is said on the console, "sysinst: ...", for the tests.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/dtreq.h>
#include "../desktop/desktop.h"

#ifdef X64

#include "si_core.h"

#define SW_STACK	(64 * 1024)
#define SW_PRI		22		/* the task: below the desktop's */
#define ASK_ROWS	DT_SI_ROWS

#define SW_SETUP	0
#define SW_RUN		1
#define SW_DONE		2

LOCAL T_SIDISK		*sw_disk = NULL;	/* SI_DISK_MAX of them */
LOCAL INT		sw_ndisk = 0;
LOCAL INT		sw_pmap[SI_PART_MAX];	/* the partitions of a disk that can go: indices in its parts */
LOCAL INT		sw_npmap = 0, sw_pbest = 0, sw_pdisk = -1;
LOCAL INT		sw_state = SW_SETUP;
LOCAL T_SIPLAN		sw_plan;
LOCAL BOOL		sw_planned = FALSE;	/* sw_plan was looked at and can be taken */
LOCAL INT		sw_pldisk = -1;		/* the disk of the plan, in sw_disk */
LOCAL T_SIRES		sw_res;
LOCAL UB		sw_ask[ASK_ROWS][WM_LABEL_MAX];
LOCAL INT		sw_nask = 0;

/* shared with the task */
LOCAL ID		sw_mtx = 0;
LOCAL INT		sw_step = 0;
LOCAL UD		sw_done_b = 0, sw_total_b = 0;
LOCAL BOOL		sw_over = FALSE;
LOCAL ER		sw_er = E_OK;

/* ---------------------------------------------------------------- words */

LOCAL BOOL same( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && a[i] == b[i]; i++ ) ;
	return (BOOL)( a[i] == b[i] );
}

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
	case SI_ST_UPDATE:	return "システムの実身とローダ、カーネルを入れ替えています";
	default:		return "終わりました";
	}
}

LOCAL CONST char *why_text( INT why, INT mode )
{
	switch ( why ) {
	case SI_R_NODISK:	return "このディスクは読めないか、512バイトのセクタのディスクではありません。";
	case SI_R_NOGPT:	return "このディスクにはGPTの区画表がありません。〈ディスク全体を消して使う〉を選んでください。";
	case SI_R_KEEP:		return "消す区画を〈消す区画〉の一覧から選んでください。";
	case SI_R_NOESP:	return "このディスクにはEFIシステム区画がありません。";
	case SI_R_NOROOM:	return ( mode == SI_REPLACE ) ? "選んだ区画は、システムボリュームより小さすぎます。"
					: "システムボリュームより大きな空き領域がありません。Windowsのディスクの管理でボリュームを縮めてください。";
	case SI_R_ESPFULL:	return "EFIシステム区画にローダとカーネルを置く空きがありません。";
	case SI_R_SMALL:	return "このディスクはEFIシステム区画とシステムボリュームを置くには小さすぎます。";
	case SI_R_INUSE:	return "このディスクの区画が使用中です。ディスク操作で切り離してください。";
	case SI_R_NOSYS:	return "起動したディスクにシステムボリュームか、ローダとカーネルが見つかりません。";
	case SI_R_BOOTDISK:	return "システムが動いているディスクには登録できません。";
	case SI_R_NOTSFS:	return ( mode == SI_ERASE ) ? "ディスク全体を消して使うときは、上書きはできません。"
					: "登録先にTessronOSの区画がないので、上書きはできません。";
	case SI_R_NOLIST:	return "いま動いているシステムはシステムの実身の一覧を持っていないので、上書きはできません。";
	case SI_R_NOVOL:	return "登録先のTessronOSのボリュームを読めないので、上書きはできません。";
	case SI_R_NOSYSVOL:	return "登録先のボリュームはTessronOSのシステムボリュームではないので、上書きはできません。";
	case SI_R_NEWER:	return "登録先のシステムのほうが新しいので、上書きはできません。";
	case SI_R_FORM:		return "登録先のシステムは別の機種のものなので、上書きはできません。";
	case SI_R_VOLFULL:	return "登録先のボリュームに、システムを入れ替える空きがありません。";
	case SI_R_NOBOOT:	return "登録先のEFIシステム区画にTessronOSの起動のファイルがないので、上書きはできません。";
	case SI_R_BUSY:		return "システムの更新が進んでいるので、上書きはできません。";
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

/* The partitions of a disk that can go; the largest Windows volume offered first */
LOCAL void part_map( INT disk )
{
	CONST T_SIDISK	*dk;
	INT		i;
	UD		sz, bsz = 0;

	sw_npmap = 0;
	sw_pbest = 0;
	sw_pdisk = disk;
	if ( sw_disk == NULL || disk < 0 || disk >= sw_ndisk ) {
		return;
	}
	dk = &sw_disk[disk];
	for ( i = 0; i < dk->nparts && sw_npmap < SI_PART_MAX; i++ ) {
		if ( !si_replaceable(&dk->parts[i]) ) {
			continue;
		}
		sz = dk->parts[i].last - dk->parts[i].first + 1;
		if ( dk->parts[i].kind == SI_K_BASIC && sz > bsz ) {
			bsz = sz;
			sw_pbest = sw_npmap;
		}
		sw_pmap[sw_npmap++] = i;
	}
}

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
	sw_pdisk = -1;
	sw_planned = FALSE;
	tm_printf((UB *)"sysinst: %d disks\n", sw_ndisk);
}

/* What is asked before the work starts: what will be written, and what will go */
LOCAL void confirm_lines( CONST T_SIDISK *dk )
{
	UB	(*l)[WM_LABEL_MAX] = sw_ask;
	INT	n = 0, k, i;

	if ( sw_plan.keep ) {
		k = put(l[n], 0, WM_LABEL_MAX, (CONST char *)sw_plan.disk);
		k = put_num(l[n], k, WM_LABEL_MAX, sw_plan.ts_unit);
		k = put(l[n], k, WM_LABEL_MAX, "にあるTessronOSの区画(");
		k = put_size(l[n], k, WM_LABEL_MAX, ( sw_plan.ts_last - sw_plan.ts_first + 1 ) * 512);
		(void)put(l[n], k, WM_LABEL_MAX, ")に、システムを上書きします。");
		n++;
		(void)put(l[n++], 0, WM_LABEL_MAX, "実身は残ります。登録先の実身・仮身、設定、入れたアプリはそのままです。");
		(void)put(l[n++], 0, WM_LABEL_MAX, "システムの実身(プログラム、メニュー、書体など)を入れ替えます。");
		k = put(l[n], 0, WM_LABEL_MAX, "登録先のシステム: ");
		k = put(l[n], k, WM_LABEL_MAX, ( sw_plan.tver[0] != 0 ) ? (CONST char *)sw_plan.tver : "(一覧なし、前のバージョン)");
		k = put(l[n], k, WM_LABEL_MAX, " → ");
		(void)put(l[n], k, WM_LABEL_MAX, (CONST char *)sw_plan.ver);
		n++;
		k = put(l[n], 0, WM_LABEL_MAX, "システムの実身: ");
		k = put_num(l[n], k, WM_LABEL_MAX, sw_plan.nobj);
		k = put(l[n], k, WM_LABEL_MAX, "個、要る空き: ");
		k = put_size(l[n], k, WM_LABEL_MAX, sw_plan.vol_need);
		k = put(l[n], k, WM_LABEL_MAX, "(空き ");
		k = put_size(l[n], k, WM_LABEL_MAX, sw_plan.vol_free);
		(void)put(l[n], k, WM_LABEL_MAX, ")");
		n++;
		(void)put(l[n++], 0, WM_LABEL_MAX, "区画表は変えません。");
		(void)put(l[n++], 0, WM_LABEL_MAX, "EFIシステム区画では、ローダとカーネルだけを入れ替えます。");
	} else if ( sw_plan.mode == SI_REPLACE ) {
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
		if ( sw_plan.had_ts ) {
			(void)put(l[n++], 0, WM_LABEL_MAX, "初期化します。登録先の実身はすべて消えます。");
		}
		(void)put(l[n++], 0, WM_LABEL_MAX, "消したものは元に戻せません。");
	} else if ( sw_plan.mode == SI_COEXIST ) {
		k = put(l[n], 0, WM_LABEL_MAX, (CONST char *)sw_plan.disk);
		if ( sw_plan.ts_unit >= 0 ) {
			/* the partition by its name, the disk's and its number, as when the system is written over */
			k = put_num(l[n], k, WM_LABEL_MAX, sw_plan.ts_unit);
			k = put(l[n], k, WM_LABEL_MAX, "にあるTessronOSの区画(");
		} else {
			k = put(l[n], k, WM_LABEL_MAX, "の空き領域に、TessronOSの区画(");
		}
		k = put_size(l[n], k, WM_LABEL_MAX, ( sw_plan.ts_last - sw_plan.ts_first + 1 ) * 512);
		(void)put(l[n], k, WM_LABEL_MAX, sw_plan.ts_unit >= 0 ? ")を書き直します。" : ")を作ります。");
		n++;
		(void)put(l[n++], 0, WM_LABEL_MAX, "EFIシステム区画には\\EFI\\TESSRON\\だけを足します。Windowsのファイルと、");
		(void)put(l[n++], 0, WM_LABEL_MAX, "ほかの区画の中身は変えません。");
		if ( sw_plan.had_ts ) {
			(void)put(l[n++], 0, WM_LABEL_MAX, "初期化します。登録先の実身はすべて消えます。消したものは元に戻せません。");
		}
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
	sw_nask = n;
}

/* The lines while the work runs: line i into b */
LOCAL INT running_line( INT i, UB *b, INT max )
{
	INT	n, st, pc;
	UD	done, total;

	tk_loc_mtx(sw_mtx, TMO_FEVR);
	st = sw_step;
	done = sw_done_b;
	total = sw_total_b;
	tk_unl_mtx(sw_mtx);
	b[0] = 0;
	switch ( i ) {
	case 0:
		n = put(b, 0, max, sw_plan.keep ? "実身を残してシステムを上書きしています: "
				 : ( sw_plan.mode == SI_ERASE ) ? "ディスク全体を使って登録しています: "
				 : ( sw_plan.mode == SI_REPLACE ) ? "区画を消して登録しています: "
				 : "Windowsと共存して登録しています: ");
		(void)put(b, n, max, (CONST char *)sw_plan.disk);
		break;
	case 1:
		n = put(b, 0, max, "手順: ");
		(void)put(b, n, max, step_name(st));
		break;
	case 2:
		if ( total > 0 && ( st == SI_ST_COPY || st == SI_ST_ESP ) ) {
			pc = (INT)( done * 100 / total );
			n = put_size(b, 0, max, done);
			n = put(b, n, max, " / ");
			n = put_size(b, n, max, total);
			n = put(b, n, max, "  (");
			n = put_num(b, n, max, pc);
			(void)put(b, n, max, "%)");
		} else if ( total > 0 && ( st == SI_ST_VERIFY || st == SI_ST_UPDATE ) ) {
			n = put_num(b, 0, max, (D)( done * 100 / total ));
			(void)put(b, n, max, "%");
		}
		break;
	case 4:
		(void)put(b, 0, max, sw_plan.keep ? "入れ替えている間は、ほかの操作をしないでください。"
				 : "写している間は、ほかの操作をしないでください。");
		break;
	case 5:
		if ( !sw_plan.keep ) (void)put(b, 0, max, "システムボリュームへの書込みは、写し終わるまで待たされます。");
		break;
	default:
		break;
	}
	return 6;
}

/* What came of it: line i into b */
LOCAL INT result_line( INT i, UB *b, INT max )
{
	INT	n;

	b[0] = 0;
	if ( sw_er < E_OK ) {
		if ( i == 0 ) {
			n = put(b, 0, max, "登録できませんでした(");
			n = put(b, n, max, step_name(sw_res.failed_step));
			n = put(b, n, max, "の途中、");
			n = put_num(b, n, max, sw_er);
			(void)put(b, n, max, ")。");
		} else if ( i == 1 && sw_er == E_BUSY ) {
			(void)put(b, 0, max, "ディスクかシステムボリュームが使用中でした。");
		} else if ( i == 2 && sw_plan.keep ) {
			(void)put(b, 0, max, "登録先の実身は残っています。もう一度上書きすると、残りを入れ替えます。");
		} else if ( i == 2 && sw_plan.mode == SI_ERASE && sw_res.failed_step >= SI_ST_TABLE ) {
			(void)put(b, 0, max, "ディスクの元の区画は消えています。もう一度登録してください。");
		} else if ( i == 2 && sw_plan.mode == SI_REPLACE && sw_res.failed_step >= SI_ST_TABLE ) {
			(void)put(b, 0, max, "選んだ区画の中身は消えています。もう一度登録してください。");
		}
		return 3;
	}
	if ( sw_plan.keep && i == 0 ) {
		n = put(b, 0, max, "登録が終わりました: ");
		n = put(b, n, max, (CONST char *)sw_res.part);
		n = put(b, n, max, "のシステムを");
		n = put(b, n, max, (CONST char *)sw_plan.ver);
		n = put(b, n, max, "にしました(システムの実身");
		n = put_num(b, n, max, sw_res.nobj);
		(void)put(b, n, max, "個)。実身は残っています。");
		return 7;
	}
	switch ( i ) {
	case 0:
		n = put(b, 0, max, "登録が終わりました: ");
		n = put(b, n, max, ( sw_res.part[0] != 0 ) ? (CONST char *)sw_res.part : (CONST char *)sw_plan.disk);
		n = put(b, n, max, "に");
		n = put_num(b, n, max, (D)( sw_res.copied * 4096 >> 20 ));
		(void)put(b, n, max, "MBを写しました。");
		break;
	case 1:
		if ( sw_res.grow_later ) {
			(void)put(b, 0, max, "ボリュームは、内蔵のディスクから最初に起動したときに区画の大きさに広がります。");
		} else if ( sw_res.vol_to > sw_res.vol_from ) {
			n = put(b, 0, max, "ボリュームを");
			n = put_size(b, n, max, sw_res.vol_from);
			n = put(b, n, max, "から");
			n = put_size(b, n, max, sw_res.vol_to);
			(void)put(b, n, max, "に広げました。");
		}
		break;
	case 2:
		(void)put(b, 0, max, sw_res.entry_asked ? "[再起動]を押し、もう一度このmicroSD(USBメモリ)から起動してください。"
				 : "起動の項目を登録できませんでした。ファームウェアの起動のメニューで選んでください。");
		break;
	case 3:
		if ( sw_res.entry_asked ) {
			(void)put(b, 0, max, "ローダが起動の項目「TessronOS」を登録し、内蔵ディスクから起動し直します。");
		}
		break;
	case 4:
		(void)put(b, 0, max, "Surface Go 2: 音量を上げるボタンと電源ボタンでUEFIに入り、起動先を選びます。");
		break;
	case 5:
		(void)put(b, 0, max, "ThinkPad X1 Yoga: 起動のときにF12を押して、起動のメニューから選びます。");
		break;
	case 6:
		(void)put(b, 0, max, si_entry_first(&sw_plan)
			    ? "登録の後は、microSDを抜くと内蔵のディスクから起動します。"
			    : "登録の後は、起動のメニューの「TessronOS」で起動します(既定はWindowsのまま)。");
		break;
	default:
		break;
	}
	return 7;
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
	tm_printf((UB *)"sysinst: over %d\n", (INT)er);
	tk_exd_tsk();
}

LOCAL ER run( ID pid, CONST TS_UUID *win )
{
	T_CTSK	ct;
	ID	tid;

	if ( sw_state == SW_RUN ) {
		return E_BUSY;
	}
	if ( !sw_planned ) {
		return E_OBJ;
	}
	/* an administrator's: the password asked for, as DT_RQ_ADMIN asks */
	if ( !knl_ob_pid_admin(pid) ) {
		ER	er = dt_admin_ask(pid, sw_plan.keep ? (CONST UB *)"内蔵のディスクのTessronOSにシステムを上書きする"
					      : (CONST UB *)"内蔵のディスクにTessronOSを登録する", win);

		if ( er < E_OK ) return er;
	}
	if ( sw_mtx <= 0 ) {
		T_CMTX	cm;

		cm.exinf = NULL;
		cm.mtxatr = TA_TFIFO;
		sw_mtx = tk_cre_mtx(&cm);
		if ( sw_mtx <= 0 ) return E_LIMIT;
	}
	sw_step = SI_ST_CHECK;
	sw_done_b = sw_total_b = 0;
	sw_over = FALSE;
	sw_er = E_OK;
	knl_memset(&sw_res, 0, sizeof(sw_res));
	sw_res.failed_step = -1;

	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)worker;
	ct.itskpri = SW_PRI;
	ct.stksz = SW_STACK;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		return E_SYS;
	}
	sw_state = SW_RUN;
	sw_planned = FALSE;
	tm_printf((UB *)"sysinst: started %s\n", sw_plan.disk);
	return E_OK;
}

LOCAL ER restart( void )
{
	ID	key;
	ER	er = E_OK;

	if ( sw_state != SW_DONE || sw_er < E_OK ) {
		return E_OBJ;
	}
	tm_printf((UB *)"sysinst: restart\n");
	key = ob_opn_obj(&ob_uuid_system, OB_OP_WRITE);
	if ( key > 0 ) {
		er = ob_wri_rec(key, OB_SYS_POWER, 0, "RESTART", 7, NULL);
		(void)ob_cls_obj(key);
	}
	return ( key <= 0 ) ? (ER)key : er;
}

/* The task followed: over, the state is that of what came of it */
LOCAL void follow( void )
{
	if ( sw_state == SW_RUN && sw_mtx > 0 ) {
		tk_loc_mtx(sw_mtx, TMO_FEVR);
		if ( sw_over ) sw_state = SW_DONE;
		tk_unl_mtx(sw_mtx);
	}
}

/*
 * A line of the answer's text (DT_TAIL_TEXT), made in t: the lines this
 * says of a disk and a plan are its own, of a few words each
 */
#define SI_LINE_MAX	DT_TEXT_MAX

EXPORT ER dt_sysinst_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	UB		t[SI_LINE_MAX];
	CONST UB	*dname = dt_rq_word(rq, DT_TAIL_NAME, NULL);	/* the disk the request names */
	INT		i, way;

	follow();
	switch ( rq->recno ) {
	case DT_SI_DISK:
		if ( rq->grab_x == 0 && sw_state != SW_RUN ) list_disks();
		an->how = (UINT)sw_ndisk;
		if ( rq->grab_x < 0 || rq->grab_x >= sw_ndisk ) return E_NOEXS;
		i = put(t, 0, SI_LINE_MAX, (CONST char *)sw_disk[rq->grab_x].name);
		i = put(t, i, SI_LINE_MAX, "\t");
		disk_row(&sw_disk[rq->grab_x], t + i, SI_LINE_MAX - i);
		return dt_ans_cat(an, t, -1);
	case DT_SI_PART:
		if ( rq->grab_x < 0 || rq->grab_x >= sw_ndisk ) return E_NOEXS;
		if ( rq->grab_x != sw_pdisk || rq->grab_y == 0 ) part_map(rq->grab_x);
		an->how = (UINT)sw_npmap;
		if ( rq->grab_y < 0 || rq->grab_y >= sw_npmap ) return E_NOEXS;
		{
			CONST T_SIDISK	*dk = &sw_disk[rq->grab_x];
			CONST T_SIPART	*p = &dk->parts[sw_pmap[rq->grab_y]];

			i = put_num(t, 0, SI_LINE_MAX, p->unit);
			i = put(t, i, SI_LINE_MAX, "\t");
			part_row(dk, p, t + i, SI_LINE_MAX - i);
			if ( rq->grab_y == sw_pbest ) an->flags |= DT_F_SI_BEST;
		}
		return dt_ans_cat(an, t, -1);
	case DT_SI_PLAN:
		if ( sw_state == SW_RUN ) return E_BUSY;
		sw_planned = FALSE;
		for ( i = 0; i < sw_ndisk && !same(sw_disk[i].name, dname); i++ ) ;
		if ( i >= sw_ndisk ) return E_NOEXS;
		way = rq->grab_x & ~DT_SI_KEEP;
		if ( si_plan((CONST char *)dname, way | ( ( rq->grab_x & DT_SI_KEEP ) ? SI_KEEP : 0 ),
			     ( way == SI_REPLACE ) ? rq->grab_y : -1, &sw_plan) < E_OK ) {
			return E_NOMEM;
		}
		tm_printf((UB *)"sysinst: plan %s %s%s %d: %d\n", sw_plan.disk, si_mode_name(way),
			  sw_plan.keep ? " keep" : "", ( way == SI_REPLACE ) ? rq->grab_y : -1, sw_plan.why);
		an->how = (UINT)sw_plan.why;
		if ( sw_plan.why != SI_R_OK ) {
			return dt_ans_cat(an, (CONST UB *)why_text(sw_plan.why, way), -1);
		}
		sw_pldisk = i;
		confirm_lines(&sw_disk[i]);
		an->look.w = sw_nask;
		/* what the disk holds goes: asked on the warning panel */
		an->look.h = ( !sw_plan.keep && ( way != SI_COEXIST || sw_plan.had_ts ) ) ? 1 : 0;
		sw_planned = TRUE;
		return E_OK;
	case DT_SI_KEEPQ:
		/* whether the TessronOS there can be kept: looked at only, the plan kept is not touched */
		if ( sw_state == SW_RUN ) return E_BUSY;
		for ( i = 0; i < sw_ndisk && !same(sw_disk[i].name, dname); i++ ) ;
		if ( i >= sw_ndisk ) return E_NOEXS;
		way = rq->grab_x & ~DT_SI_KEEP;
		{
			T_SIPLAN	*kp = (T_SIPLAN *)Kmalloc(sizeof(T_SIPLAN));
			ER		er = E_OK;

			if ( kp == NULL ) return E_NOMEM;
			if ( si_plan((CONST char *)dname, way | SI_KEEP, ( way == SI_REPLACE ) ? rq->grab_y : -1, kp) < E_OK ) {
				Kfree(kp);
				return E_NOMEM;
			}
			tm_printf((UB *)"sysinst: keep %s %s %d: %d\n", kp->disk, si_mode_name(way),
				  ( way == SI_REPLACE ) ? rq->grab_y : -1, kp->why);
			an->how = ( kp->why == SI_R_NOTSFS ) ? (UINT)DT_SI_K_NONE : (UINT)kp->why;
			if ( kp->why != SI_R_OK ) er = dt_ans_cat(an, (CONST UB *)why_text(kp->why, way), -1);
			Kfree(kp);
			return er;
		}
	case DT_SI_LINE:
		t[0] = 0;
		if ( rq->grab_x == DT_SI_L_ASK ) {
			an->how = (UINT)sw_nask;
			if ( rq->grab_y < 0 || rq->grab_y >= sw_nask ) return E_NOEXS;
			return dt_ans_cat(an, (CONST UB *)sw_ask[rq->grab_y], -1);
		} else if ( rq->grab_x == DT_SI_L_RESULT ) {
			if ( sw_state != SW_DONE ) return E_OBJ;
			an->how = (UINT)result_line(rq->grab_y, t, SI_LINE_MAX);
		} else if ( rq->grab_x == DT_SI_L_RUN ) {
			if ( sw_state == SW_SETUP ) return E_OBJ;
			an->how = (UINT)running_line(rq->grab_y, t, SI_LINE_MAX);
		} else {
			return E_PAR;
		}
		return ( t[0] != 0 ) ? dt_ans_cat(an, t, -1) : E_OK;
	case DT_SI_RUN:
		return run(pid, &rq->win);
	case DT_SI_STATE:
		an->how = (UINT)( ( sw_mtx > 0 ) ? sw_step : 0 );
		if ( sw_state == SW_RUN ) an->flags |= DT_F_SI_RUN;
		if ( sw_state == SW_DONE ) {
			an->flags |= DT_F_SI_OVER;
			an->look.w = (INT)sw_er;
			an->look.h = sw_res.failed_step;
		}
		return E_OK;
	case DT_SI_RESTART:
		return restart();
	default:
		return E_PAR;
	}
}

#else /* X64 */

/* The PC's only: elsewhere the system is put on a disk as an image (03-platform.md) */
EXPORT ER dt_sysinst_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	(void)rq;
	(void)an;
	(void)pid;
	return E_NOSPT;
}

#endif /* X64 */
