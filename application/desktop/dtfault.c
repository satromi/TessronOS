/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtfault.c
 *	プログラムの異常終了: a process ended by a processor exception,
 *	told in a dialog (design 5.5, 11.1)
 *
 *	The exception handler builds nothing on the screen. It records what
 *	happened as the process ends (knl_prc_abort, knl_prc_call_leave),
 *	and the desktop's own task takes the records here each time round
 *	its loop (dt_fault_poll) and shows them: the program and its pid,
 *	what the exception was in words, the address it met (FAR), where
 *	the program was (ELR) and its LR, and the exit code.
 *
 *	There is one such window at most. It shows the latest exception;
 *	one that comes while it is up takes the place of what it showed, and
 *	the window counts how many others there were, so that faults one
 *	after another neither pile windows up nor go unsaid. The kernel
 *	keeps only a few records and says how many it had to let go, and
 *	those are counted too.
 *
 *	It is a window of its own, not a panel that holds the desktop still:
 *	it is brought to the front but does not take the keys from the
 *	window that has them. 了解, Enter or Escape while it has the keys,
 *	or a double press on its pictogram closes it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/hid.h>
#include <ts/proc.h>
#include "desktop.h"

#define FT_W		580		/* the panel, inside its frame */
#define FT_H		316
#define FT_NAME_W	140		/* the column of what each row is */
#define FT_ROW0		40		/* the first row */
#define FT_ROW_H	26
#define FT_ROWS		10		/* the lines kept for whoever asks (dt_fault_lines) */

#define KEY_ENTER	0x28
#define KEY_ESCAPE	0x29

#define EC_UNKNOWN	0x00
#define EC_WFX		0x01
#define EC_FP		0x07
#define EC_ILLSTATE	0x0e
#define EC_SYSREG	0x18
#define EC_IABT_LOW	0x20
#define EC_IABT_CUR	0x21
#define EC_PC_ALIGN	0x22
#define EC_DABT_LOW	0x24
#define EC_DABT_CUR	0x25
#define EC_SP_ALIGN	0x26
#define EC_FP_EXC	0x2c
#define ISS_WNR		( 1U << 6 )	/* a data abort: the access was a write */

typedef struct {
	INT		wid;		/* the window */
	INT		pid;		/* the panel filling it */
	BOOL		moving;		/* carried by its band */
	INT		grab_dx, grab_dy;
	T_PRCFLT	f;		/* the exception shown: the latest */
	UW		others;		/* the ones before it while the window was up, or let go */
	UB		line[FT_ROWS][WM_LABEL_MAX];
	INT		nline;
} FAULTWIN;

LOCAL FAULTWIN	*ft = NULL;

/* ---------------------------------------------------------------- words */

LOCAL INT put( UB *b, INT n, INT max, CONST char *s )
{
	while ( s != NULL && *s != 0 && n < max - 1 ) {
		b[n++] = (UB)*s++;
	}
	/* never half a character */
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
	if ( v < 0 ) {
		t[k++] = '-';
	}
	while ( k > 0 && n < max - 1 ) {
		b[n++] = (UB)t[--k];
	}
	b[n] = 0;
	return n;
}

/* 0x and 'digits' hex digits */
LOCAL INT put_hex( UB *b, INT n, INT max, UD v, INT digits )
{
	static CONST char	hex[] = "0123456789abcdef";
	INT			i;

	n = put(b, n, max, "0x");
	for ( i = digits - 1; i >= 0 && n < max - 1; i-- ) {
		b[n++] = (UB)hex[( v >> ( i * 4 ) ) & 0xF];
	}
	b[n] = 0;
	return n;
}

/* Hex digits without 0x */
LOCAL INT put_hexd( UB *b, INT n, INT max, UD v, INT digits )
{
	static CONST char	hex[] = "0123456789abcdef";
	INT			i;

	for ( i = digits - 1; i >= 0 && n < max - 1; i-- ) {
		b[n++] = (UB)hex[( v >> ( i * 4 ) ) & 0xF];
	}
	b[n] = 0;
	return n;
}

/*
 * The code at the PC as the program's pages held it when it ended, the
 * instruction the exception was reported at in brackets; what could
 * not be read is dashes. Set beside the program's file, it tells
 * whether the processor ran what the file has there.
 */
#ifdef CPU_CORE_X86_64
#define CODE_FROM	0		/* bytes from the PC */
#define CODE_TO		12
#define CODE_UNIT	1
#else
#define CODE_FROM	( -4 )		/* the instruction before it and the one after */
#define CODE_TO		8
#define CODE_UNIT	4
#endif

LOCAL void put_code( CONST T_PRCFLT *f, UB *b, INT max )
{
	INT	o, at, k, n = 0;
	UD	v;

	for ( o = CODE_FROM; o < CODE_TO; o += CODE_UNIT ) {
		at = TS_FLT_CODE_BACK + o;
		if ( n > 0 ) n = put(b, n, max, " ");
		if ( o == 0 ) n = put(b, n, max, "[");
		if ( ( f->codeok & ( 1U << ( at / 4 ) ) ) == 0 ) {
			n = put(b, n, max, ( CODE_UNIT == 4 ) ? "--------" : "--");
		} else {
			for ( v = 0, k = CODE_UNIT - 1; k >= 0; k-- ) {
				v = ( v << 8 ) | f->code[at + k];
			}
			n = put_hexd(b, n, max, v, CODE_UNIT * 2);
		}
		if ( o == 0 && CODE_UNIT == 4 ) n = put(b, n, max, "]");
	}
	if ( CODE_UNIT == 1 ) (void)put(b, n, max, "]");
}

#ifndef CPU_CORE_X86_64
/* What went wrong with the address of an abort, by its fault status code */
LOCAL CONST char *fsc_word( UW fsc, BOOL exec )
{
	if ( fsc <= 0x03 ) {
		return "アドレスの範囲外";
	}
	if ( fsc <= 0x07 ) {
		return "マップされていないアドレス";
	}
	if ( fsc <= 0x0b ) {
		return "アクセスフラグのないページ";
	}
	if ( fsc <= 0x0f ) {
		return exec ? "実行を許されていないアドレス" : "アクセスを許されていないアドレス";
	}
	return NULL;
}
#endif

#ifdef CPU_CORE_X86_64
/*
 * x86-64: the exception in words, from its vector (bits 39:32 of esr)
 * and, for a page fault, its error code: P the page was there (and was
 * refused), W a write, I an instruction's fetch
 */
#define PF_P		0x01
#define PF_W		0x02
#define PF_I		0x10

LOCAL void describe( CONST T_PRCFLT *f, UB *b, INT max )
{
	UW	vec = (UW)( f->esr >> 32 ) & 0xff;
	UW	err = (UW)f->esr;
	INT	n = 0;

	switch ( vec ) {
	case 0:		n = put(b, n, max, "除算エラー");			break;
	case 3:		n = put(b, n, max, "ブレークポイント");			break;
	case 4:		n = put(b, n, max, "オーバーフロー");			break;
	case 5:		n = put(b, n, max, "範囲外(BOUND)");			break;
	case 6:		n = put(b, n, max, "未定義命令");			break;
	case 7:		n = put(b, n, max, "浮動小数点・SIMD命令の使用");	break;
	case 12:	n = put(b, n, max, "スタックセグメント例外");		break;
	case 13:	n = put(b, n, max, "一般保護例外");			break;
	case 14:
		n = put(b, n, max, "ページフォールト(");
		n = put(b, n, max, ( err & PF_P ) ? ( ( err & PF_I ) ? "実行を許されていないアドレス"
							   : "アクセスを許されていないアドレス" )
						   : "マップされていないアドレス");
		n = put(b, n, max, ( err & PF_I ) ? "の実行)" : ( err & PF_W ) ? "への書込み)" : "からの読出し)");
		break;
	case 16:	n = put(b, n, max, "浮動小数点例外");			break;
	case 17:	n = put(b, n, max, "アライメント例外");			break;
	case 19:	n = put(b, n, max, "SIMD浮動小数点例外");		break;
	default:
		(void)put(b, n, max, ( vec == 1 ) ? "デバッグ例外" : "その他の例外");
		break;
	}
}
#else
/* The exception in words, from the exception class of ESR and its syndrome */
LOCAL void describe( CONST T_PRCFLT *f, UB *b, INT max )
{
	UW		ec = (UW)( f->esr >> 26 ) & 0x3f;
	UW		iss = (UW)f->esr & 0x1ffffff;
	UW		fsc = iss & 0x3f;
	CONST char	*w;
	INT		n = 0;

	switch ( ec ) {
	case EC_UNKNOWN:	n = put(b, n, max, "未定義命令");			break;
	case EC_WFX:		n = put(b, n, max, "WFI・WFE命令のトラップ");		break;
	case EC_FP:		n = put(b, n, max, "浮動小数点・SIMD命令の使用");	break;
	case EC_ILLSTATE:	n = put(b, n, max, "不正な実行状態");			break;
	case EC_SYSREG:		n = put(b, n, max, "システムレジスタへのアクセス");	break;
	case EC_PC_ALIGN:	n = put(b, n, max, "アライメント例外(PC)");		break;
	case EC_SP_ALIGN:	n = put(b, n, max, "アライメント例外(SP)");		break;
	case EC_FP_EXC:		n = put(b, n, max, "浮動小数点例外");			break;
	case EC_IABT_LOW:
	case EC_IABT_CUR:
		n = put(b, n, max, "命令アボート");
		w = fsc_word(fsc, TRUE);
		if ( w != NULL ) {
			n = put(b, n, max, "(");
			n = put(b, n, max, w);
			n = put(b, n, max, "の実行)");
		}
		break;
	case EC_DABT_LOW:
	case EC_DABT_CUR:
		if ( fsc == 0x21 ) {
			n = put(b, n, max, "アライメント例外(");
			n = put(b, n, max, ( iss & ISS_WNR ) ? "書込み)" : "読出し)");
			break;
		}
		n = put(b, n, max, "データアボート");
		w = fsc_word(fsc, FALSE);
		if ( w != NULL ) {
			n = put(b, n, max, "(");
			n = put(b, n, max, w);
			n = put(b, n, max, ( iss & ISS_WNR ) ? "への書込み)" : "からの読出し)");
		}
		break;
	default:
		(void)put(b, n, max, ( ec >= 0x30 && ec <= 0x3c ) ? "デバッグ例外" : "その他の例外");
		break;
	}
}
#endif

/* ---------------------------------------------------------------- the panel */

/* One row: what it is, and its value; the value kept for dt_fault_lines */
LOCAL void row( T_WMPANEL *def, INT i, CONST char *name, CONST UB *val )
{
	T_WMPART	*pt;
	INT		n;

	(void)dt_pn_add(def, WM_PT_LABEL, 0, 14, FT_ROW0 + i * FT_ROW_H, FT_NAME_W - 10, 22, name);
	pt = dt_pn_add(def, WM_PT_LABEL, 0, FT_NAME_W + 14, FT_ROW0 + i * FT_ROW_H,
		       FT_W - FT_NAME_W - 28, 22, NULL);
	if ( pt != NULL ) {
		dt_pn_text(pt->label, WM_LABEL_MAX, (CONST char *)val);
	}
	if ( ft->nline < FT_ROWS ) {
		n = put(ft->line[ft->nline], 0, WM_LABEL_MAX, name);
		n = put(ft->line[ft->nline], n, WM_LABEL_MAX, " ");
		(void)put(ft->line[ft->nline], n, WM_LABEL_MAX, (CONST char *)val);
		ft->nline++;
	}
}

/* The panel for what ft shows, made anew */
LOCAL T_WMPANEL *build( void )
{
	CONST T_PRCFLT	*f = &ft->f;
	BOOL		kpan = (BOOL)( f->kind == TS_FLT_KPAN );
	BOOL		kcall = (BOOL)( f->kind == TS_FLT_KCALL || kpan );
	T_WMPANEL	*def = dt_pn_new(FT_W, FT_H);
	T_WMPART	*pt;
	UB		b[WM_LABEL_MAX], nm[OB_NAME_MAX + 1];
	CONST char	*head;
	INT		n;

	if ( def == NULL ) {
		return NULL;
	}
	def->kind = WM_PNL_BARE;
	ft->nline = 0;

	head = kpan ? "カーネルがプログラムのメモリに確かめずに触れたため、プログラムを終了しました"
	     : kcall ? "プログラムがシステムコールに不正なアドレスを渡したため、終了しました"
		     : "プログラムがプロセッサの例外を起こしたため、終了しました";
	(void)dt_pn_add(def, WM_PT_LABEL, 0, 14, 8, FT_W - 28, 22, head);
	(void)put(ft->line[ft->nline++], 0, WM_LABEL_MAX, head);

	(void)knl_prc_flt_name(f, nm, sizeof(nm));
	n = put(b, 0, sizeof(b), (CONST char *)nm);
	n = put(b, n, sizeof(b), "(pid ");
	n = put_num(b, n, sizeof(b), f->pid);
	(void)put(b, n, sizeof(b), ")");
	row(def, 0, "プログラム", b);

	describe(f, b, sizeof(b));
	row(def, 1, "例外", b);

#ifdef CPU_CORE_X86_64
	(void)put_hex(b, 0, sizeof(b), f->far, 16);
	row(def, 2, "アドレス(CR2)", b);
	(void)put_hex(b, 0, sizeof(b), f->elr, 16);
	row(def, 3, kcall ? "PC(カーネル)" : "PC(RIP)", b);
	(void)put_hex(b, 0, sizeof(b), f->lr, 16);
	row(def, 4, kcall ? "RBP(カーネル)" : "RBP", b);
#else
	(void)put_hex(b, 0, sizeof(b), f->far, 16);
	row(def, 2, "アドレス(FAR)", b);
	(void)put_hex(b, 0, sizeof(b), f->elr, 16);
	row(def, 3, kcall ? "PC(カーネル)" : "PC(ELR)", b);
	(void)put_hex(b, 0, sizeof(b), f->lr, 16);
	row(def, 4, kcall ? "LR(カーネル)" : "LR", b);
#endif

	n = put_num(b, 0, sizeof(b), f->exitcd);
	(void)put(b, n, sizeof(b), ( f->exitcd == TS_ABORT_ILL ) ? "(TS_ABORT_ILL)"
				 : ( f->exitcd == TS_ABORT_FAULT ) ? "(TS_ABORT_FAULT)" : "");
	row(def, 5, "終了コード", b);

	n = put(b, 0, sizeof(b), "タスク");
	n = put_num(b, n, sizeof(b), f->tskid);
#ifdef CPU_CORE_X86_64
	n = put(b, n, sizeof(b), "、ベクタ ");
	n = put_num(b, n, sizeof(b), (D)( ( f->esr >> 32 ) & 0xff ));
	n = put(b, n, sizeof(b), "、エラーコード ");
	(void)put_hex(b, n, sizeof(b), f->esr & 0xffffffffU, 8);
#else
	n = put(b, n, sizeof(b), "、ESR ");
	n = put_hex(b, n, sizeof(b), f->esr, 8);
	n = put(b, n, sizeof(b), "(EC ");
	n = put_hex(b, n, sizeof(b), ( f->esr >> 26 ) & 0x3f, 2);
	(void)put(b, n, sizeof(b), ")");
#endif
	row(def, 6, "詳細", b);

	if ( f->kind == TS_FLT_EL0 ) {
		put_code(f, b, sizeof(b));
		row(def, 7, "PCの命令", b);
	}

	if ( ft->others > 0 ) {
		n = put(b, 0, sizeof(b), "このほかに");
		n = put_num(b, n, sizeof(b), (D)ft->others);
		(void)put(b, n, sizeof(b), "件、例外で終了したプログラムがあります");
		(void)dt_pn_add(def, WM_PT_LABEL, 0, 14, FT_ROW0 + 8 * FT_ROW_H + 4, FT_W - 28, 22,
				(CONST char *)b);
		if ( ft->nline < FT_ROWS ) {
			(void)put(ft->line[ft->nline++], 0, WM_LABEL_MAX, (CONST char *)b);
		}
	}

	pt = dt_pn_add(def, WM_PT_BUTTON | P_EMPHAS, DT_PN_OK, FT_W - 108, FT_H - 36, 94, 26, "了解");
	if ( pt != NULL ) {
		pt->answer = WM_ANS_OK;
	}
	return def;
}

LOCAL void ft_close( void )
{
	FAULTWIN	*w = ft;

	if ( w == NULL ) {
		return;
	}
	ft = NULL;
	if ( w->pid >= 0 ) {
		(void)wm_panel_close(w->pid);
	}
	(void)wm_close(w->wid);
	Kfree(w);
	wm_update();
}

/*
 * The window opened, or what it shows made anew: in the middle of the
 * screen, in front, the keys left where they were.
 */
LOCAL void show( CONST T_PRCFLT *f, UW others )
{
	T_WMPANEL	*def;
	T_WMWIN		w;
	T_DPRECT	o;
	INT		pw, ph, sw = 0, sh = 0;
	BOOL		fresh = FALSE;

	if ( ft == NULL ) {
		ft = (FAULTWIN *)Kmalloc(sizeof(FAULTWIN));
		if ( ft == NULL ) {
			return;
		}
		knl_memset(ft, 0, sizeof(*ft));
		ft->pid = -1;
		ft->wid = -1;
		fresh = TRUE;
	} else {
		others += ft->others + 1;	/* the one it showed is one of them now */
	}
	ft->f = *f;
	ft->others = others;
	def = build();
	if ( def == NULL ) {
		if ( fresh ) {
			Kfree(ft);
			ft = NULL;
		}
		return;
	}
	pw = def->r.right - def->r.left;
	ph = def->r.bottom - def->r.top;
	if ( fresh ) {
		wm_screen(&sw, &sh);
		o.left = ( sw - pw ) / 2 - 8;
		o.top = ( sh - ph ) / 2 - 20;
		if ( o.left < 0 ) o.left = 0;
		if ( o.top < 0 ) o.top = 0;
		o.right = o.left + pw + 16;
		o.bottom = o.top + ph + 40;
		ft->wid = wm_open(&o, WM_ATTR_FRAME | WM_ATTR_TITLE, "プログラムの異常終了");
		if ( ft->wid < 0 ) {
			Kfree(def);
			Kfree(ft);
			ft = NULL;
			return;
		}
		if ( wm_ref(ft->wid, &w) >= E_OK ) {
			/* the frame measured: the work area made the panel's size */
			o = w.outer;
			o.right += pw - ( w.work.right - w.work.left );
			o.bottom += ph - ( w.work.bottom - w.work.top );
			(void)wm_move(ft->wid, &o);
		}
	} else if ( ft->pid >= 0 ) {
		(void)wm_panel_close(ft->pid);
		ft->pid = -1;
	}
	def->r.left = 0;
	def->r.top = 0;
	def->r.right = pw;
	def->r.bottom = ph;
	ft->pid = wm_panel_open(ft->wid, def);
	Kfree(def);
	if ( ft->pid < 0 ) {
		ft_close();
		return;
	}
	wm_raise(ft->wid);
	wm_panel_draw(ft->pid);
	wm_update();
}

/* ---------------------------------------------------------------- the desktop's side */

/*
 * The exceptions recorded since the last look, from the desktop's loop:
 * the latest shown, the rest counted.
 */
EXPORT void dt_fault_poll( void )
{
	T_PRCFLT	f, t;
	UW		lost = 0, others = 0;
	BOOL		got = FALSE;

	while ( knl_prc_fault_take(&t, &lost) ) {
		others += lost;
		if ( got ) {
			others++;		/* an older one, not shown */
		}
		f = t;
		got = TRUE;
	}
	others += lost;
	if ( got ) {
#ifdef CPU_CORE_X86_64
		tm_printf((UB *)"TessronOS desktop: pid %d ended by an exception (vector %d): told\n",
			  f.pid, (INT)( ( f.esr >> 32 ) & 0xff ));
#else
		tm_printf((UB *)"TessronOS desktop: pid %d ended by an exception (EC %02x): told\n",
			  f.pid, (INT)( ( f.esr >> 26 ) & 0x3f ));
#endif
		show(&f, others);
	}
}

/* What was recorded before the desktop came up: the console has said it */
EXPORT void dt_fault_forget( void )
{
	T_PRCFLT	f;

	while ( knl_prc_fault_take(&f, NULL) ) ;
}

/*
 * An event, if it is for the dialog: TRUE when it was, and nothing else
 * is to be done with it. A move of the pointer is also the desktop's to
 * follow, so it answers FALSE for that.
 */
EXPORT BOOL dt_fault_event( CONST T_WMEV *ev )
{
	T_WMEV	e = *ev;
	T_WMWIN	w;
	UINT	ans = WM_ANS_NONE, btn, bar = 0;
	INT	px, py, got = 0;

	if ( ft == NULL ) {
		return FALSE;
	}
	if ( ft->moving ) {
		if ( ev->type == HID_EV_MOVE && ts_hid_pointer(&px, &py, &btn) >= E_OK
		  && wm_ref(ft->wid, &w) >= E_OK ) {
			T_DPRECT	o;

			o.left = px - ft->grab_dx;
			o.top = py - ft->grab_dy;
			o.right = o.left + ( w.outer.right - w.outer.left );
			o.bottom = o.top + ( w.outer.bottom - w.outer.top );
			if ( dt_carry_window(ft->wid, &o) ) wm_update();
			return FALSE;
		}
		if ( ev->type == HID_EV_BTN_UP ) {
			ft->moving = FALSE;
			if ( dt_carry_end(ft->wid) ) wm_update();
			return TRUE;
		}
	}
	if ( ev->type == HID_EV_KEY_DOWN || ev->type == HID_EV_KEY_UP ) {
		if ( wm_focused() != ft->wid ) {
			return FALSE;
		}
		if ( ev->type == HID_EV_KEY_DOWN && ( ev->code == KEY_ENTER || ev->code == KEY_ESCAPE ) ) {
			ft_close();
			return TRUE;
		}
		e.wid = ft->wid;
	} else if ( ev->wid != ft->wid ) {
		return FALSE;
	}
	if ( ev->type == HID_EV_BTN_DOWN ) {
		wm_raise(ft->wid);
		wm_focus(ft->wid);
		if ( ev->y < 0 && wm_ref(ft->wid, &w) >= E_OK ) {
			px = w.work.left + ev->x;
			py = w.work.top + ev->y;
			if ( wm_part_at(px, py, &got, &bar) == WM_PART_PICT && got == ft->wid ) {
				if ( wm_pict_close(ft->wid, px, py, ev->when) ) {
					ft_close();
				} else {
					wm_update();
				}
				return TRUE;
			}
			/* its band: carried */
			ft->moving = TRUE;
			ft->grab_dx = px - w.outer.left;
			ft->grab_dy = py - w.outer.top;
			wm_update();
			return TRUE;
		}
		if ( ev->code != 0 ) {
			wm_update();
			return TRUE;
		}
	}
	if ( wm_panel_event(ft->pid, &e, &ans) < E_OK ) {
		ans = WM_ANS_CANCEL;
	}
	if ( ans == WM_ANS_OK || ans == WM_ANS_CANCEL ) {
		ft_close();
		return TRUE;
	}
	wm_panel_draw(ft->pid);
	wm_update();
	return (BOOL)( ev->type != HID_EV_MOVE );
}

/* The dialog closed (the desktop stopping) */
EXPORT void dt_fault_close_all( void )
{
	ft_close();
}

/* The dialog's window, 0 while none is shown */
EXPORT INT dt_fault_wid( void )
{
	return ( ft != NULL ) ? ft->wid : 0;
}

/* What it says, a line feed after each line; the bytes written */
EXPORT INT dt_fault_lines( UB *buf, INT max )
{
	INT	n = 0, i;

	if ( buf == NULL || max <= 0 ) {
		return 0;
	}
	buf[0] = 0;
	for ( i = 0; ft != NULL && i < ft->nline; i++ ) {
		n = put(buf, n, max, (CONST char *)ft->line[i]);
		n = put(buf, n, max, "\n");
	}
	return n;
}
