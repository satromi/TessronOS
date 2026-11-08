/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	emu_main.c
 *	BeFC and BeSFC: the window of a game console's emulator (design
 *	11.32). Built twice, the second time with EMU_SFC; the difference is
 *	the core, which knows nothing of this system, behind the macros below.
 *
 *	The object of a game is dropped on the window: its link is a real
 *	object one of whose records is the ROM as a file holds it. A game that
 *	cannot be run says why, on the message line and in the window. The picture
 *	is laid over the work area, kept in proportion. The keys are the pad's.
 *	The sound goes to the first sound device, kept playing while a game
 *	runs; with none it goes on silent.
 *
 *	What a game keeps stays with the game, in records of its own object:
 *	the battery's memory in one of the subtype SAVE_SUB, and each state saved
 *	from the menu in one of the subtype SAVE_SUB + its number. An object the
 *	person may not write is played all the same, and the console says it is
 *	not kept.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/sysdef.h>
#include <ts/hid.h>
#include <ts/wm.h>
#include <ts/mn.h>
#include <ts/snd.h>
#include <ts/obatr.h>
#include <ts/obdrop.h>
#include "emu_rom.h"

#ifdef EMU_SFC
#include <ts/usb.h>
#include "snes.h"
#define FPS_X100	6010		/* the console's 60.1 frames a second: its sound is made per frame */
#define CATCHUP_MAX	12
#define APP_NAME	"besfc"
#define APP_TITLE	"BeSFC"
#define APP_PROG	SYSDEF_PROG_BESFC
#define APP_MENU	SYSDEF_MENU_BESFC
#define EMU_W		SFC_W
#define EMU_H		SFC_H
#define SAVE_SUB	0x5346
#define SND_STEREO	1
#define emu_load	sfc_load
#define emu_reset	sfc_reset
#define emu_frame	sfc_run_frame
#define emu_input	sfc_set_input
#define emu_fb		sfc_framebuffer
#define emu_audio_on	sfc_audio_enable
#define emu_audio	sfc_audio_read
#define emu_state_size	sfc_state_size
#define emu_state_save	sfc_save_state
#define emu_state_load	sfc_load_state
#define emu_sram_size	sfc_sram_size
#define emu_sram_save	sfc_sram_save
#define emu_sram_load	sfc_sram_load
#define emu_error	sfc_last_error
#else
#include "nes.h"
#define FPS_X100	6000
#define CATCHUP_MAX	4
#define APP_NAME	"befc"
#define APP_TITLE	"BeFC"
#define APP_PROG	SYSDEF_PROG_BEFC
#define APP_MENU	SYSDEF_MENU_BEFC
#define EMU_W		256
#define EMU_H		240
#define SAVE_SUB	0x4643
#define SND_STEREO	0
#define emu_load	nes_load
#define emu_reset	nes_reset
#define emu_frame	nes_run_frame
#define emu_input	nes_set_input
#define emu_fb		nes_framebuffer
#define emu_audio_on	nes_audio_enable
#define emu_audio	nes_audio_read
#define emu_state_size	nes_state_size
#define emu_state_save	nes_save_state
#define emu_state_load	nes_load_state
#define emu_sram_size	nes_sram_size
#define emu_sram_save	nes_sram_save
#define emu_sram_load	nes_sram_load
#define emu_error	nes_last_error
#endif

#define FRAME_MS	16		/* the frames are paced by the 60 a second counted from the start */
#define BATCH_MS	48		/* frames caught up at a go, so that a heavy game leaves the window alive */
#define SPEED_ONE	256		/* the speed and the volume, as fractions of this */
#define AUDIO_MAX	4096		/* stereo frames at most, of a frame's sound */
#define DEV_BUF		( 16 * 1024 )	/* the sound device's buffer, in bytes */
#define SND_LOOK_FRAMES	30		/* how often the device is asked whether it still plays */
#define ROM_MAX		( 16 * 1024 * 1024 )
#define SAVE_MAGIC	0x56534d45U
#define SLOT_MAGIC	0x54534d45U
#define SLOT_MAX	5		/* the numbers of 状態を保存 and 状態を読込 */
#define EMU_STATE_OTHER	(-3)		/* the core's answer: the state belongs to another ROM */
#define C_GROUND	0x00000000U

#define WHY_NOTROM	"この機種のROMではありません"
#define WHY_NOMEM	"メモリが足りないので読み込めません"
#define WHY_READ	"ROMのレコードを読めません"
#define WHY_OPEN	"この実身を開けません"
#define WHY_BIG		"ROMが大きすぎます"
#define WHY_NOGAME	"ゲームが読み込まれていません。ROMの実身の仮身をドロップしてください"
#define WHY_STNONE	"まだ保存されていません"
#define WHY_STREAD	"レコードを読めません"
#define WHY_STBAD	"データが壊れています"
#define WHY_STVER	"別のバージョンで保存された状態です"
#define WHY_STOTHER	"別のゲームの状態です"
#define WHY_STSAVE	"状態を取り出せません"
#define WHY_STWRITE	"この実身に書き込めません"
#ifdef EMU_SFC
/* This core answers only -1, and says why in words */
#define WHY_LOAD(er)	( ( strstr(emu_error(), "memory") != NULL ) ? WHY_NOMEM : WHY_NOTROM )
#else
#define WHY_CORE	"対応していないマッパーのゲームなので動きません"
#define WHY_LOAD(er)	( ( (er) == EMU_ERR_NOMEM ) ? WHY_NOMEM : ( (er) == EMU_ERR_NOCHIP ) ? WHY_CORE : WHY_NOTROM )
#endif

/* The record of the battery's memory: this, then the memory. An older one has a state after it */
typedef struct {
	UW	magic;
	INT	sram, state;
} SAVEHD;

/* The record of one state: this, then the state */
typedef struct {
	UW	magic;
	INT	size;
} SLOTHD;

static TS_UUID	g_ch, g_win, g_game;
static ID	g_port, g_kw, g_snd;
static INT	g_gid = -1, g_w = EMU_W * 2, g_h = EMU_H * 2;
static BOOL	g_closing = FALSE, g_loaded = FALSE, g_paused = FALSE;
static BOOL	g_told = FALSE;		/* the message line has something of this program's */
static UINT	g_pad = 0;
static UINT	g_tap = 0;		/* pressed since the last frame: a frame sees it even if let go before */
static CONST char *g_why = NULL;	/* why the last game dropped did not start */
static INT	g_bigw = 0, g_bigh = 0;	/* the size of the picture laid out; 0 the window is to be cleared */
static INT	g_speed = SPEED_ONE, g_volume = SPEED_ONE;
static BOOL	g_repace = FALSE;	/* the speed changed: the count of frames starts again */
#ifdef EMU_SFC
static BOOL	g_fps_on = FALSE, g_noflicker = FALSE;
static INT	g_fps_val = 0, g_fps_cnt = 0;
static UD	g_fps_at = 0;
static UINT	g_cmdkeys = 0;		/* the keys of commands held down: each acts once a press */
#endif

static void say( CONST char *fmt, INT a, INT b )
{
	char	s[200];

	INT	n = snprintf(s, sizeof(s), APP_NAME ": ");

	snprintf(s + n, sizeof(s) - (size_t)n, fmt, a, b);
	tm_putstring((CONST UB *)s);
}

/* ---------------------------------------------------------------- the sound */

static void snd_w( INT rec, INT v )
{
	SZ	asz = 0;
	W	w = v;

	(void)ob_wri_rec(g_snd, rec, 0, &w, sizeof(w), &asz);
}

static void snd_open( void )
{
	static H	zero[DEV_BUF / 2];
	TS_UUID		ids[8];
	SDPcmMode	m;
	INT		n = 0, i;
	SZ		asz = 0;

	if ( g_snd > 0 ) return;
	if ( ob_lst_obj(OB_T_DEVICE, OB_S_SOUND, NULL, ids, 8, &n) < E_OK ) n = 0;
	for ( i = 0; i < n && g_snd <= 0; i++ ) g_snd = ob_opn_obj(&ids[i], OB_OP_R | OB_OP_WRITE);
	if ( g_snd <= 0 ) {
		g_snd = 0;
		say("no sound device: silent\n", 0, 0);
		return;
	}
	snd_w(OB_SND_CTL, PCM_STOP);
	memset(&m, 0, sizeof(m));
	m.sampling = PCMST48K;
	m.datafmt = PCMFmt16;
	m.stereo = 1;
	(void)ob_wri_rec(g_snd, OB_SND_MODE, 0, &m, sizeof(m), &asz);
	snd_w(OB_SND_BUFSZ, DEV_BUF);
	snd_w(OB_SND_ENDCNT, 0);
	(void)ob_wri_rec(g_snd, OB_SND_PCM, 0, zero, DEV_BUF / 2, &asz);
	snd_w(OB_SND_CTL, PCM_PLAY);
	emu_audio_on(1);
}

static void snd_close( void )
{
	if ( g_snd > 0 ) {
		snd_w(OB_SND_CTL, PCM_STOP);
		(void)ob_cls_obj(g_snd);
		g_snd = 0;
	}
}

static void snd_volume( int16_t *s, INT n )
{
	INT	i;

	if ( g_volume >= SPEED_ONE ) return;
	for ( i = 0; i < n; i++ ) s[i] = (int16_t)( (INT)s[i] * g_volume / SPEED_ONE );
}

/* A frame's sound to the device, both sides; what finds no room is let go rather than holding the picture back */
static void snd_frame( void )
{
	static int16_t	in[AUDIO_MAX * 2];
#if !SND_STEREO
	static int16_t	both[AUDIO_MAX * 2];
#endif
	static INT	look = 0;
	INT		n;
	SZ		asz = 0;

	if ( g_snd <= 0 ) return;
	/*
	 * The device stops a channel left short for half a second, and writes
	 * do not start it again: after a stall the sound would stay off.
	 */
	if ( ++look >= SND_LOOK_FRAMES ) {
		W	st = PCM_PLAY;

		look = 0;
		if ( ob_rea_rec(g_snd, OB_SND_CTL, 0, &st, sizeof(st), &asz) >= E_OK && st == PCM_STOP ) {
			say("sound stopped short: started again\n", 0, 0);
			snd_w(OB_SND_CTL, PCM_PLAY);
		}
	}
#if SND_STEREO
	n = emu_audio(in, AUDIO_MAX);
	if ( n > 0 ) snd_volume(in, n * 2);
	if ( n > 0 ) (void)ob_wri_rec(g_snd, OB_SND_PCM, 0, in, (SZ)n * 4, &asz);
#else
	{
		INT	i;

		n = emu_audio(in, AUDIO_MAX);
		if ( n > 0 ) snd_volume(in, n);
		for ( i = 0; i < n; i++ ) both[2 * i] = both[2 * i + 1] = in[i];
		if ( n > 0 ) (void)ob_wri_rec(g_snd, OB_SND_PCM, 0, both, (SZ)n * 4, &asz);
	}
#endif
}

/* ---------------------------------------------------------------- the picture */

#ifdef EMU_SFC
/* The frames a second and what the console's registers say, over the picture */
extern void ppu_dbg_bgscroll( unsigned short *h, unsigned short *v );
extern unsigned char *ppu_oam_ptr( void );

static void overlay( INT x, INT y )
{
	char		s[80], title[24];
	unsigned	bm = 0, tm = 0, br = 0, fnv = 0;
	int		mt = 0;
	long		len = 0;
	unsigned short	hs[4], vs[4];
	CONST UB	*oam = ppu_oam_ptr();

	snprintf(s, sizeof(s), "FPS %d", g_fps_val);
	(void)dp_text(g_gid, x + 4, y + 16, (CONST UB *)s, 0x00FFFF00U, 14);
	sfc_dbg_state(&bm, &tm, &br, &mt);
	sfc_dbg_rom(title, &len, &fnv);
	snprintf(s, sizeof(s), "M%02X TM%02X Br%02X %s", bm, tm, br, mt == 0 ? "Lo" : mt == 1 ? "Hi" : "Ex");
	(void)dp_text(g_gid, x + 4, y + 34, (CONST UB *)s, 0x0000FF00U, 14);
	(void)dp_text(g_gid, x + 4, y + 52, (CONST UB *)title, 0x0000FF00U, 14);
	ppu_dbg_bgscroll(hs, vs);
	snprintf(s, sizeof(s), "1:%03X,%03X 2:%03X,%03X O:%02X,%02X", hs[0] & 0x3FF, vs[0] & 0x3FF, hs[1] & 0x3FF, vs[1] & 0x3FF, oam[0], oam[1]);
	(void)dp_text(g_gid, x + 4, y + 70, (CONST UB *)s, 0x0000FF00U, 14);
}
#endif

static void paint( void )
{
	T_DPRECT	box, all;
	INT		w = g_w, h = g_w * EMU_H / EMU_W;

	if ( g_gid < 0 ) return;
	if ( h > g_h ) {
		h = g_h;
		w = g_h * EMU_W / EMU_H;
	}
	box.left = ( g_w - w ) / 2;
	box.top = ( g_h - h ) / 2;
	box.right = box.left + w;
	box.bottom = box.top + h;
	if ( !g_loaded ) {
		all.left = all.top = 0;
		all.right = g_w;
		all.bottom = g_h;
		(void)dp_fill_rect(g_gid, &all, C_GROUND);
		(void)dp_text(g_gid, 16, 16, (CONST UB *)"ゲームの実身の仮身をここへドロップしてください", 0x00FFFFFFU, 14);
		if ( g_why != NULL ) (void)dp_text(g_gid, 16, 40, (CONST UB *)g_why, 0x00FFD040U, 14);
	} else {
		/* laid out at the window's size here, each pixel from the nearest of the picture */
		static UW	*big = NULL;
		CONST uint32_t	*fb = emu_fb();
		INT		x, y;

		if ( w != g_bigw || h != g_bigh ) {
			free(big);
			big = (UW *)malloc((size_t)w * (size_t)h * sizeof(UW));
			g_bigw = ( big != NULL ) ? w : 0;
			g_bigh = ( big != NULL ) ? h : 0;
			all.left = all.top = 0;
			all.right = g_w;
			all.bottom = g_h;
			(void)dp_fill_rect(g_gid, &all, C_GROUND);
			if ( big == NULL ) (void)dp_text(g_gid, 16, 16, (CONST UB *)WHY_NOMEM, 0x00FFD040U, 14);
		}
		for ( y = 0; big != NULL && y < h; y++ ) {
			CONST uint32_t	*row = fb + (size_t)( y * EMU_H / h ) * EMU_W;
			UW		*o = big + (size_t)y * w;

			for ( x = 0; x < w; x++ ) o[x] = row[x * EMU_W / w];
		}
		if ( big != NULL ) (void)dp_put_argb(g_gid, box.left, box.top, big, w, w, h, 0xFFFFFFFFU);
#ifdef EMU_SFC
		if ( g_fps_on ) overlay(box.left, box.top);
#endif
	}
	(void)wm_obj_flush(g_kw, NULL);
}

static void work_size( void )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( ob_rea_rec(g_kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK && asz >= (SZ)sizeof(wp) ) {
		g_w = wp.wright - wp.wleft;
		g_h = wp.wbottom - wp.wtop;
	}
}

/* ---------------------------------------------------------------- what a game keeps */

/* Whether a record is one of the game's saves, never its ROM */
static BOOL is_save_rec( CONST T_OBREC *r )
{
	return (BOOL)( r->rt == OB_RT_SYSDATA && r->sub >= SAVE_SUB && r->sub <= SAVE_SUB + SLOT_MAX );
}

/* The number of the record of slot n (0: the battery's), -1 none. Record numbers are not fixed: found by subtype */
static INT save_rec( ID key, INT n, UD *p_size )
{
	INT	cnt = 0, i, at = -1;
	T_OBREC	*r = ob_rec_read(key, &cnt);

	for ( i = 0; r != NULL && i < cnt; i++ ) {
		if ( r[i].rt == OB_RT_SYSDATA && r[i].sub == (UINT)( SAVE_SUB + n ) ) {
			at = r[i].recno;
			if ( p_size != NULL ) *p_size = r[i].size;
		}
	}
	free(r);
	return at;
}

/* All n bytes of a record from at: a read may answer with fewer */
static BOOL rec_read( ID key, INT rec, D at, UB *b, SZ n )
{
	SZ	got = 0, asz = 0;

	while ( got < n && ob_rea_rec(key, rec, at + got, b + got, n - got, &asz) >= E_OK && asz > 0 ) {
		got += asz;
	}
	return (BOOL)( got == n );
}

/* The battery's record when it still carries a state of the older form: its number, else -1 */
static INT old_state( ID key, SAVEHD *h )
{
	UD	size = 0;
	INT	rec = save_rec(key, 0, &size);

	if ( rec < 0 || size < sizeof(*h) || !rec_read(key, rec, 0, (UB *)h, sizeof(*h))
	  || h->magic != SAVE_MAGIC || h->sram < 0 || h->state <= 0
	  || size < sizeof(*h) + (UD)h->sram + (UD)h->state ) {
		return -1;
	}
	return rec;
}

/* The slots holding something: a bit for each number. A state of the older form counts as 1 until it has moved */
static UINT slots_used( void )
{
	UINT	mask = 0;
	INT	n;
	SAVEHD	h;
	ID	key;

	if ( !g_loaded || ( key = ob_opn_obj(&g_game, OB_OP_R) ) <= 0 ) return 0;
	for ( n = 1; n <= SLOT_MAX; n++ ) {
		UD	size = 0;

		if ( save_rec(key, n, &size) >= 0 && size >= sizeof(SLOTHD) ) mask |= 1U << n;
	}
	if ( ( mask & 2U ) == 0 && old_state(key, &h) >= 0 ) mask |= 2U;
	(void)ob_cls_obj(key);
	return mask;
}

/* b has the room for the head and then len bytes of state: the record of slot n replaced by it */
static ER slot_write( ID key, INT n, UB *b, INT len )
{
	SLOTHD	h;
	INT	rec = save_rec(key, n, NULL);
	ER	er = E_OK;

	h.magic = SLOT_MAGIC;
	h.size = len;
	memcpy(b, &h, sizeof(h));
	if ( rec < 0 ) er = ob_apd_rec(key, OB_RT_SYSDATA, (UINT)( SAVE_SUB + n ), &rec);
	if ( er >= E_OK ) er = ob_rpl_rec(key, rec, b, (SZ)sizeof(h) + len);
	return er;
}

/* A slot with nothing in it for a state of the older form: 1 first, not the one about to be written; 0 none */
static INT slot_free( ID key, INT avoid )
{
	INT	n;

	for ( n = 1; n <= SLOT_MAX; n++ ) {
		UD	size = 0;

		if ( n != avoid && ( save_rec(key, n, &size) < 0 || size < sizeof(SLOTHD) ) ) return n;
	}
	return 0;
}

/*
 * The battery's memory kept. A state the record still carries from the older
 * form goes to a slot of its own first, made sure of before it leaves this
 * record: when there is no slot for it, or it cannot be written, it stays
 * here and nothing is lost. avoid is the slot about to be written. *p_moved
 * says where it went, 0 when it did not.
 */
static ER keep_put( INT avoid, INT *p_moved )
{
	INT	sr = emu_sram_size(), rec, st = 0;
	SAVEHD	h, old;
	UB	*b, *ob = NULL;
	ID	key;
	ER	er = E_OK;

	*p_moved = 0;
	if ( !g_loaded ) return E_OK;
	if ( sr <= 0 ) {
		/* nothing of its own to write: an object the person may not write is not asked to */
		INT	has = -1;

		if ( ( key = ob_opn_obj(&g_game, OB_OP_R) ) > 0 ) {
			has = old_state(key, &old);
			(void)ob_cls_obj(key);
		}
		if ( has < 0 ) return E_OK;
	}
	key = ob_opn_obj(&g_game, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	if ( key <= 0 ) {
		say("the game's object cannot be written (%d): nothing kept\n", (INT)key, 0);
		return (ER)key;
	}
	rec = save_rec(key, 0, NULL);
	if ( old_state(key, &old) >= 0 ) {
		/* the head of a slot goes before the bytes, so that they are written as they are */
		ob = (UB *)malloc(sizeof(SLOTHD) + (size_t)old.state);
		if ( ob == NULL || !rec_read(key, rec, (D)sizeof(old) + old.sram, ob + sizeof(SLOTHD), old.state) ) {
			/* the record is left as it was, the state in it */
			say("the older state cannot be read: nothing kept\n", 0, 0);
			free(ob);
			(void)ob_cls_obj(key);
			return E_IO;
		}
		st = old.state;
		*p_moved = slot_free(key, avoid);
		if ( *p_moved > 0 && ( slot_write(key, *p_moved, ob, st) < E_OK || ob_syn_obj(key) < E_OK ) ) {
			say("the older state stays where it is: slot %d not written\n", *p_moved, 0);
			*p_moved = 0;
		}
		if ( *p_moved > 0 ) {
			say("the older state moved to slot %d: %d bytes\n", *p_moved, st);
			st = 0;
		}
	}
	b = (UB *)malloc(sizeof(h) + (size_t)( sr > 0 ? sr : 0 ) + (size_t)st);
	if ( b == NULL ) {
		free(ob);
		(void)ob_cls_obj(key);
		return E_NOMEM;
	}
	h.magic = SAVE_MAGIC;
	h.sram = ( sr > 0 ) ? emu_sram_save(b + sizeof(h), sr) : 0;
	if ( h.sram < 0 ) h.sram = 0;
	h.state = st;
	if ( st > 0 ) memcpy(b + sizeof(h) + h.sram, ob + sizeof(SLOTHD), (size_t)st);
	memcpy(b, &h, sizeof(h));
	if ( rec < 0 ) er = ob_apd_rec(key, OB_RT_SYSDATA, SAVE_SUB, &rec);
	if ( er >= E_OK ) er = ob_rpl_rec(key, rec, b, (SZ)sizeof(h) + h.sram + h.state);
	say("kept: battery %d, older state %d bytes", h.sram, h.state);
	say(" (%d)\n", (INT)er, 0);
	free(b);
	free(ob);
	(void)ob_cls_obj(key);
	return er;
}

/* The battery's memory read back */
static void keep_get( void )
{
	SAVEHD	h;
	UB	*b;
	INT	rec;
	ID	key = ob_opn_obj(&g_game, OB_OP_R);

	if ( key <= 0 ) return;
	rec = save_rec(key, 0, NULL);
	if ( rec >= 0 && rec_read(key, rec, 0, (UB *)&h, sizeof(h)) && h.magic == SAVE_MAGIC && h.sram > 0
	  && ( b = (UB *)malloc((size_t)h.sram) ) != NULL ) {
		if ( rec_read(key, rec, (D)sizeof(h), b, h.sram) ) (void)emu_sram_load(b, h.sram);
		free(b);
	}
	(void)ob_cls_obj(key);
}

/* The message line says what came of it, with the number and, when there is one, the reason */
static void tell( CONST char *fmt, INT n, CONST char *why )
{
	char	s[160];

	snprintf(s, sizeof(s), fmt, n, why);
	(void)wm_msg_put(s);
	g_told = TRUE;
}

static void tell_no_game( void )
{
	(void)wm_msg_put(WHY_NOGAME);
	g_told = TRUE;
}

/* The state of the game kept in slot n */
static ER slot_save( INT n )
{
	INT	st = emu_state_size(), len, moved = 0;
	UB	*b;
	ID	key;
	ER	er;

	if ( !g_loaded ) {
		tell_no_game();
		return E_OBJ;
	}
	b = ( st > 0 ) ? (UB *)malloc(sizeof(SLOTHD) + (size_t)st) : NULL;
	if ( b == NULL ) {
		tell("状態を%d番に保存できません(%s)", n, WHY_NOMEM);
		return E_NOMEM;
	}
	len = emu_state_save(b + sizeof(SLOTHD), st);
	if ( len <= 0 ) {
		free(b);
		say("state %d: the core gave none (%d)\n", n, len);
		tell("状態を%d番に保存できません(%s)", n, WHY_STSAVE);
		return E_OBJ;
	}
	(void)keep_put(n, &moved);
	key = ob_opn_obj(&g_game, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	er = ( key > 0 ) ? slot_write(key, n, b, len) : (ER)key;
	if ( key > 0 ) (void)ob_cls_obj(key);
	free(b);
	say("state %d kept: %d bytes", n, len);
	say(" (%d)\n", (INT)er, 0);
	if ( er < E_OK ) {
		tell("状態を%d番に保存できません(%s)", n, WHY_STWRITE);
	} else if ( moved > 0 ) {
		char	s[160];

		snprintf(s, sizeof(s), "状態を%d番に保存しました。以前の形式の状態は%d番へ移しました", n, moved);
		(void)wm_msg_put(s);
		g_told = TRUE;
	} else {
		tell("状態を%d番に保存しました", n, NULL);
	}
	return er;
}

/*
 * The state of len bytes given to the core. What the core reads goes over the
 * running game as it reads, and it can stop part of the way: the game as it
 * was is kept aside and put back then. *why says why it was refused.
 */
static BOOL state_apply( CONST UB *s, INT len, CONST char **why )
{
	INT	size = emu_state_size(), er, had;
	UB	*keep;

	if ( len != size ) {
		say("state read: %d bytes, the core's are %d\n", len, size);
		*why = WHY_STVER;
		return FALSE;
	}
	keep = (UB *)malloc((size_t)size);
	if ( keep == NULL ) {
		*why = WHY_NOMEM;
		return FALSE;
	}
	had = emu_state_save(keep, size);
	er = ( had > 0 ) ? emu_state_load(s, len) : -1;
	if ( er != 0 ) {
		if ( had > 0 ) (void)emu_state_load(keep, had);
		*why = ( er == EMU_STATE_OTHER ) ? WHY_STOTHER : WHY_STVER;
	}
	free(keep);
	say("state read (%d)\n", er, 0);
	return (BOOL)( er == 0 );
}

/* The state of slot n read back; one not of its form or size is not given to the core */
static ER slot_load( INT n )
{
	UD	size = 0;
	SLOTHD	h;
	SAVEHD	old;
	INT	rec, len = 0;
	D	at = 0;
	UB	*b;
	ID	key;
	CONST char	*why = NULL;
	BOOL	ok = FALSE;

	if ( !g_loaded ) {
		tell_no_game();
		return E_OBJ;
	}
	key = ob_opn_obj(&g_game, OB_OP_R);
	if ( key <= 0 ) {
		say("the object cannot be opened (%d)\n", (INT)key, 0);
		tell("状態%d番を読み込めません(%s)", n, WHY_OPEN);
		return (ER)key;
	}
	rec = save_rec(key, n, &size);
	if ( rec >= 0 && size >= sizeof(h) ) {
		if ( !rec_read(key, rec, 0, (UB *)&h, sizeof(h)) ) {
			why = WHY_STREAD;
		} else if ( h.magic != SLOT_MAGIC || h.size <= 0 || size != sizeof(h) + (UD)h.size ) {
			why = WHY_STBAD;
		} else {
			len = h.size;
			at = (D)sizeof(h);
		}
	} else if ( n == 1 && ( rec = old_state(key, &old) ) >= 0 ) {
		len = old.state;
		at = (D)sizeof(old) + old.sram;
	} else {
		why = WHY_STNONE;
	}
	if ( why == NULL ) {
		b = (UB *)malloc((size_t)len);
		if ( b == NULL ) {
			why = WHY_NOMEM;
		} else {
			if ( !rec_read(key, rec, at, b, len) ) why = WHY_STREAD;
			else ok = state_apply(b, len, &why);
			free(b);
		}
	}
	(void)ob_cls_obj(key);
	if ( ok ) {
		tell("状態%d番を読み込みました", n, NULL);
		return E_OK;
	}
	say("state %d not read\n", n, 0);
	tell("状態%d番を読み込めません(%s)", n, why);
	return E_OBJ;
}

/* The number at the end of a code of the menu ("emu.save.3"), 0 when it is not one of prefix */
static INT slot_code( CONST char *code, CONST char *prefix )
{
	size_t	p = strlen(prefix);

	if ( strncmp(code, prefix, p) != 0 || code[p] < '1' || code[p] > '0' + SLOT_MAX || code[p + 1] != 0 ) return 0;
	return code[p] - '0';
}

/* The numbers of 状態を読込 with nothing saved under them shown grey: how many items that came to */
static INT slot_marks( ID mid )
{
	UINT	mask = slots_used();
	INT	n, items = 0, k;

	for ( n = 1; n <= SLOT_MAX; n++ ) {
		char	code[16];

		snprintf(code, sizeof(code), "emu.load.%d", n);
		k = mn_chg_atr(mid, NULL, code, ( mask & ( 1U << n ) ) ? 0 : MN_GREY);
		if ( k > 0 ) items += k;
	}
	return items;
}

/* ---------------------------------------------------------------- the game */

/* Whether the bytes look like a ROM of this console */
static BOOL is_rom( CONST UB *b, SZ n )
{
#ifdef EMU_SFC
	/* a cartridge starts in the ROM, and its header has a true checksum pair or a printable title */
	static CONST long	hdr[3] = { 0x7FC0, 0xFFC0, 0x40FFC0 };
	SZ			base = ( n % 1024 == 512 ) ? 512 : 0;
	INT			i, k;

	if ( n < 0x8000 || n % 1024 > 512 ) return FALSE;
	for ( i = 0; i < 3; i++ ) {
		CONST UB	*h = b + base + hdr[i];
		BOOL		ok;

		if ( base + hdr[i] + 64 > n || (UINT)( h[0x3C] | ( h[0x3D] << 8 ) ) < 0x8000 ) continue;
		ok = (BOOL)( ( ( h[0x1C] | ( h[0x1D] << 8 ) ) ^ ( h[0x1E] | ( h[0x1F] << 8 ) ) ) == 0xFFFF );
		if ( !ok ) {
			ok = TRUE;
			for ( k = 0; k < 21; k++ ) if ( h[k] < 0x20 || h[k] > 0x7E ) ok = FALSE;
		}
		if ( ok ) return TRUE;
	}
	return FALSE;
#else
	return (BOOL)( n >= 16 && b[0] == 'N' && b[1] == 'E' && b[2] == 'S' && b[3] == 0x1a );
#endif
}

/*
 * The game of object u loaded: the first of its records that is a ROM. When
 * none is, *why says the reason, the likeliest of those met; *core is set when the
 * core was asked, which ends the game it was running.
 */
static BOOL game_load( CONST TS_UUID *u, CONST char **why, BOOL *core )
{
	ID	key = ob_opn_obj(u, OB_OP_R);
	INT	cnt = 0, i, er;
	T_OBREC	*r;
	BOOL	ok = FALSE;

	*why = WHY_NOTROM;
	*core = FALSE;
	if ( key <= 0 ) {
		say("the object cannot be opened (%d)\n", (INT)key, 0);
		*why = WHY_OPEN;
		return FALSE;
	}
	r = ob_rec_read(key, &cnt);
	for ( i = 0; r != NULL && i < cnt && !ok; i++ ) {
		UB	*b;
		SZ	asz = 0;
		D	at = 0;

		if ( r[i].recno == 0 || r[i].size < 16 || is_save_rec(&r[i]) ) continue;
		if ( r[i].size > ROM_MAX ) {
			say("record %d: %d bytes, more than a ROM can be\n", r[i].recno, (INT)r[i].size);
			*why = WHY_BIG;
			continue;
		}
		b = (UB *)malloc((size_t)r[i].size);
		if ( b == NULL ) {
			say("record %d: no memory for %d bytes\n", r[i].recno, (INT)r[i].size);
			*why = WHY_NOMEM;
			continue;
		}
		while ( at < (D)r[i].size && ob_rea_rec(key, r[i].recno, at, b + at, (SZ)( r[i].size - at ), &asz) >= E_OK && asz > 0 ) {
			at += asz;
		}
		if ( at != (D)r[i].size ) {
			say("record %d: read %d bytes only\n", r[i].recno, (INT)at);
			*why = WHY_READ;
		} else if ( !is_rom(b, (SZ)at) ) {
			say("record %d, %d bytes: not a ROM\n", r[i].recno, (INT)at);
		} else {
			*core = TRUE;
			er = emu_load(b, (long)at);	/* the core keeps a copy of its own */
			if ( er == 0 ) {
				ok = TRUE;
				say("record %d, %d bytes: running\n", r[i].recno, (INT)at);
			} else {
				say("record %d: not loaded (%d): ", r[i].recno, er);
				tm_putstring((CONST UB *)emu_error());
				tm_putstring((CONST UB *)"\n");
				*why = WHY_LOAD(er);
			}
		}
		free(b);
	}
	free(r);
	(void)ob_cls_obj(key);
	return ok;
}

static void drop_in( void )
{
	T_OBDROP	*d = ob_drop_read(g_kw);
	T_OBDRANS	a;
	SZ		asz = 0;
	BOOL		ok = FALSE, core = FALSE;
	CONST char	*why = WHY_NOTROM;

	if ( d == NULL ) return;
	if ( d->n > 0 ) {
		if ( g_loaded ) {
			INT	moved;

			(void)keep_put(0, &moved);
		}
		ok = game_load(&d->v[0].target, &why, &core);
	}
	memset(&a, 0, sizeof(a));
	a.seq = d->seq;
	a.answer = ok ? OB_DR_ACCEPT : OB_DR_REFUSE;
	strncpy((char *)a.msg, ok ? "読み込みました" : why, sizeof(a.msg) - 1);
	(void)ob_wri_rec(g_kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
	if ( ok ) {
		g_game = d->v[0].target;
		g_loaded = TRUE;
		g_paused = FALSE;
		g_why = NULL;
		g_bigw = g_bigh = 0;
		keep_get();
		snd_open();
	} else {
		/* a game still running goes on, and the message line has said why */
		if ( core ) g_loaded = FALSE;
		if ( !g_loaded ) {
			g_why = why;
			g_bigw = g_bigh = 0;
			paint();
		}
	}
	free(d);
}

/* ---------------------------------------------------------------- input */

#ifdef EMU_SFC
#include "emu_pad.h"
#endif

/* The pad's bit of a key */
static UINT pad_bit( UINT code )
{
#ifdef EMU_SFC
	switch ( code ) {
	case 0x52:	return SFC_BTN_UP;
	case 0x51:	return SFC_BTN_DOWN;
	case 0x50:	return SFC_BTN_LEFT;
	case 0x4F:	return SFC_BTN_RIGHT;
	case 0x1D:	return SFC_BTN_B;	/* Z */
	case 0x1B:	return SFC_BTN_A;	/* X */
	case 0x04:	return SFC_BTN_Y;	/* A */
	case 0x16:	return SFC_BTN_X;	/* S */
	case 0x14:	return SFC_BTN_L;	/* Q */
	case 0x1A:	return SFC_BTN_R;	/* W */
	case 0x28:	return SFC_BTN_START;
	case 0x2C:	return SFC_BTN_SELECT;
	default:	return 0;
	}
#else
	switch ( code ) {
	case 0x52:	return NES_BTN_UP;
	case 0x51:	return NES_BTN_DOWN;
	case 0x50:	return NES_BTN_LEFT;
	case 0x4F:	return NES_BTN_RIGHT;
	case 0x1D:	return NES_BTN_B;	/* Z */
	case 0x1B:	return NES_BTN_A;	/* X */
	case 0x06:	return NES_BTN_A | NES_BTN_B;	/* C */
	case 0x04:	return NES_BTN_SELECT;	/* A */
	case 0x16:	return NES_BTN_START;	/* S */
	case 0x2C:	return NES_BTN_A;	/* space */
	case 0x28:	return NES_BTN_START;
	default:	return 0;
	}
#endif
}

#ifdef EMU_SFC
/* R resets; F shows the frames a second; 1 to 5 turn BG1 to BG4 and the sprites on and off */
static void key_command( UINT code, BOOL up )
{
	INT	at = ( code == 0x15 ) ? 0 : ( code == 0x09 ) ? 1 : ( code >= 0x1E && code <= 0x22 ) ? (INT)code - 0x1E + 2 : -1;

	if ( at < 0 ) return;
	if ( up ) {
		g_cmdkeys &= ~( 1U << at );
		return;
	}
	if ( ( g_cmdkeys & ( 1U << at ) ) != 0 ) return;
	g_cmdkeys |= 1U << at;
	if ( at == 1 ) g_fps_on = !g_fps_on;
	else if ( !g_loaded ) return;
	else if ( at == 0 ) emu_reset();
	else sfc_set_layer_mask(sfc_get_layer_mask() ^ ( 1 << ( at - 2 ) ));
}

/* The settings, which hold whether or not a game is in */
static BOOL setting( CONST char *code )
{
	static CONST struct { CONST char *code; INT val; } sp[] = {
		{ "emu.sp4", 1024 }, { "emu.sp2", 512 }, { "emu.sp15", 384 }, { "emu.sp1", 256 }
	}, vo[] = {
		{ "emu.volmax", 256 }, { "emu.volmid", 128 }, { "emu.vollow", 64 }, { "emu.volmute", 0 }
	};
	INT	i;

	for ( i = 0; i < 4; i++ ) {
		if ( strcmp(code, sp[i].code) == 0 ) {
			g_speed = sp[i].val;
			g_repace = TRUE;
			return TRUE;
		}
		if ( strcmp(code, vo[i].code) == 0 ) {
			g_volume = vo[i].val;
			return TRUE;
		}
	}
	if ( strcmp(code, "emu.noflick") == 0 ) {
		g_noflicker = !g_noflicker;
		sfc_set_sprite_limit(g_noflicker ? 0 : 1);
		return TRUE;
	}
	if ( strcmp(code, "emu.fps") == 0 ) {
		g_fps_on = !g_fps_on;
		return TRUE;
	}
	return FALSE;
}

/* The marks of the settings in a menu */
static void setting_marks( ID mid )
{
	(void)mn_chg_atr(mid, NULL, "emu.sp4", g_speed == 1024 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.sp2", g_speed == 512 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.sp15", g_speed == 384 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.sp1", g_speed == 256 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.volmax", g_volume == 256 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.volmid", g_volume == 128 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.vollow", g_volume == 64 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.volmute", g_volume == 0 ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.noflick", g_noflicker ? MN_TICK : 0);
	(void)mn_chg_atr(mid, NULL, "emu.fps", g_fps_on ? MN_TICK : 0);
}
#endif

static void command( CONST char *code )
{
	INT	n;

#ifdef EMU_SFC
	if ( setting(code) ) return;
#endif
	if ( strcmp(code, "close") == 0 ) {
		g_closing = TRUE;
	} else if ( ( n = slot_code(code, "emu.save.") ) > 0 ) {
		(void)slot_save(n);
	} else if ( ( n = slot_code(code, "emu.load.") ) > 0 ) {
		(void)slot_load(n);
	} else if ( !g_loaded ) {
		return;
	} else if ( strcmp(code, "emu.reset") == 0 ) {
		emu_reset();
	} else if ( strcmp(code, "emu.pause") == 0 ) {
		g_paused = !g_paused;
	}
}

static void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( ui_uuid(APP_MENU, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	(void)mn_chg_atr(mid, NULL, "emu.pause", g_paused ? MN_TICK : 0);
	(void)slot_marks(mid);
#ifdef EMU_SFC
	setting_marks(mid);
#endif
	chosen = (BOOL)( mn_pop_men(mid, g_kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( chosen ) command((CONST char *)sel.code);
}

static void event( CONST T_OBNTM *m )
{
	UINT	bit;

	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		g_closing = TRUE;
		break;
	case OB_E_PRESS:
		if ( m->code == 1 ) menu(m);
		break;
	case OB_E_KEY:
#ifdef EMU_SFC
		key_command(m->code, (BOOL)( ( m->mods & OB_KEY_UP ) != 0 ));
#endif
		bit = pad_bit(m->code);
		if ( ( m->mods & OB_KEY_UP ) != 0 ) g_pad &= ~bit;
		else {
			g_pad |= bit;
			g_tap |= bit;
		}
		break;
	case OB_E_REDRAW:
		work_size();
		paint();
		break;
	case OB_E_DROP:
		drop_in();
		paint();
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- without a window */

/*
 * The saves, on objects made here. Each state differs, having been saved
 * some frames apart, and what the core gives back after a load is compared
 * with what was saved under that number.
 */
#define SELF_CHECK( c )	do { if ( !( c ) ) { say("selftest: line %d failed\n", __LINE__, 0); bad++; } } while ( 0 )

/* An object with only a TAD record, for the saves to go in */
static BOOL self_object( TS_UUID *u )
{
	static CONST char	js[] = "{\"name\":\"selftest\",\"relationship\":[],\"linktype\":false,\"refCount\":0,\"recordCount\":1,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1}]}}";
	static CONST char	note[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>selftest</p></document></tad>";
	T_OBCRE	c;
	SZ	asz = 0;
	INT	rec = -1;
	ID	key;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = (CONST UB *)js;
	c.jsonsz = (SZ)strlen(js);
	if ( ob_cre_obj(&c, u) < E_OK ) return FALSE;
	key = ob_opn_obj(u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	if ( key <= 0 ) return FALSE;
	(void)ob_apd_rec(key, OB_RT_TAD, 0, &rec);
	(void)ob_wri_rec(key, rec, 0, note, (SZ)( sizeof(note) - 1 ), &asz);
	(void)ob_cls_obj(key);
	return TRUE;
}

/* A record of the older form: the head, the battery's bytes (none), and a state */
static BOOL self_old( CONST TS_UUID *u, CONST UB *st, INT len )
{
	UB	*b = (UB *)malloc(sizeof(SAVEHD) + (size_t)len);
	SAVEHD	h;
	INT	rec = -1;
	ID	key = ob_opn_obj(u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	ER	er = E_NOMEM;

	if ( key > 0 && b != NULL ) {
		h.magic = SAVE_MAGIC;
		h.sram = 0;
		h.state = len;
		memcpy(b, &h, sizeof(h));
		memcpy(b + sizeof(h), st, (size_t)len);
		er = ob_apd_rec(key, OB_RT_SYSDATA, SAVE_SUB, &rec);
		if ( er >= E_OK ) er = ob_rpl_rec(key, rec, b, (SZ)sizeof(h) + len);
	}
	if ( key > 0 ) (void)ob_cls_obj(key);
	free(b);
	return (BOOL)( er >= E_OK );
}

/* Records of the object with the subtype of slot n */
static INT self_count( CONST TS_UUID *u, INT n, UD *p_size )
{
	INT	cnt = 0, i, k = 0;
	ID	key = ob_opn_obj(u, OB_OP_R);
	T_OBREC	*r = ( key > 0 ) ? ob_rec_read(key, &cnt) : NULL;

	for ( i = 0; r != NULL && i < cnt; i++ ) {
		if ( r[i].rt == OB_RT_SYSDATA && r[i].sub == (UINT)( SAVE_SUB + n ) ) {
			k++;
			if ( p_size != NULL ) *p_size = r[i].size;
		}
	}
	free(r);
	if ( key > 0 ) (void)ob_cls_obj(key);
	return k;
}

/* A slot's record overwritten as a damaged one would be */
static BOOL self_put( CONST TS_UUID *u, INT n, CONST void *b, SZ size )
{
	ID	key = ob_opn_obj(u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	ER	er = E_OBJ;

	if ( key > 0 ) {
		INT	rec = save_rec(key, n, NULL);

		if ( rec >= 0 ) er = ob_rpl_rec(key, rec, b, size);
		(void)ob_cls_obj(key);
	}
	return (BOOL)( er >= E_OK );
}

/* Whether what the core holds now is what was kept */
static BOOL self_same( CONST UB *want, INT len )
{
	UB	*now = (UB *)malloc((size_t)len);
	BOOL	same = (BOOL)( now != NULL && emu_state_save(now, len) == len && memcmp(now, want, (size_t)len) == 0 );

	free(now);
	return same;
}

static INT self_slots( CONST UB *rom, INT romsz )
{
	UB	*st[SLOT_MAX + 2], *big;
	TS_UUID	u, v, def;
	INT	bad = 0, n, len = emu_state_size(), rec = -1, moved = 0;
	UD	size = 0;
	ID	mid = 0, key;

	/* a state for every number, and two more */
	for ( n = 0; n < SLOT_MAX + 2; n++ ) {
		INT	f;

		st[n] = (UB *)malloc((size_t)len);
		if ( st[n] == NULL ) return 1;
		for ( f = 0; f < 3; f++ ) emu_frame();
		SELF_CHECK(emu_state_save(st[n], len) == len);
	}
	SELF_CHECK(memcmp(st[1], st[2], (size_t)len) != 0);
	if ( !self_object(&u) || !self_object(&v) ) {
		say("selftest: no object to save in\n", 0, 0);
		return 1;
	}
	g_game = u;
	g_loaded = TRUE;

	/* nothing saved: every number grey, and reading one changes nothing */
	SELF_CHECK(slots_used() == 0);
	if ( ui_uuid(APP_MENU, &def) >= E_OK && mn_cre_men(&def, &mid) >= E_OK ) {
		/* the definition has each number once, under its own code */
		SELF_CHECK(slot_marks(mid) == SLOT_MAX);
		for ( n = 1; n <= SLOT_MAX; n++ ) {
			char	code[16];

			snprintf(code, sizeof(code), "emu.save.%d", n);
			SELF_CHECK(mn_chg_atr(mid, NULL, code, 0) == 1);
		}
	} else {
		say("selftest: no menu\n", 0, 0);
		bad++;
	}
	SELF_CHECK(emu_state_load(st[0], len) == 0);
	SELF_CHECK(slot_load(3) < E_OK);
	SELF_CHECK(self_same(st[0], len));

	/* each number its own state, in a record of its own */
	for ( n = 1; n <= SLOT_MAX; n++ ) {
		SELF_CHECK(emu_state_load(st[n], len) == 0);
		SELF_CHECK(slot_save(n) >= E_OK);
		SELF_CHECK(self_count(&u, n, &size) == 1);
		SELF_CHECK(size == sizeof(SLOTHD) + (UD)len);
		SELF_CHECK(slots_used() == ( ( 1U << ( n + 1 ) ) - 2U ));
	}
	/* read back out of order, none mistaken for another */
	for ( n = SLOT_MAX; n >= 1; n-- ) {
		SELF_CHECK(slot_load(n) >= E_OK);
		SELF_CHECK(self_same(st[n], len));
	}
	/* the same number again: replaced, one record as before, the others as they were */
	SELF_CHECK(emu_state_load(st[SLOT_MAX + 1], len) == 0);
	SELF_CHECK(slot_save(3) >= E_OK);
	SELF_CHECK(self_count(&u, 3, NULL) == 1);
	SELF_CHECK(slot_load(3) >= E_OK);
	SELF_CHECK(self_same(st[SLOT_MAX + 1], len));
	SELF_CHECK(slot_load(2) >= E_OK);
	SELF_CHECK(self_same(st[2], len));

	/* a state that does not hold together is not read, and the game goes on as it was */
	big = (UB *)malloc(sizeof(SLOTHD) + (size_t)len);
	if ( big != NULL ) {
		SLOTHD	h;

		h.magic = SLOT_MAGIC ^ 1;
		h.size = len;
		memcpy(big, &h, sizeof(h));
		memcpy(big + sizeof(h), st[4], (size_t)len);
		SELF_CHECK(self_put(&u, 4, big, (SZ)sizeof(h) + len));
		SELF_CHECK(slot_load(4) < E_OK);
		SELF_CHECK(self_same(st[2], len));
		/* the head's size not the record's */
		h.magic = SLOT_MAGIC;
		h.size = len - 1;
		memcpy(big, &h, sizeof(h));
		SELF_CHECK(self_put(&u, 4, big, (SZ)sizeof(h) + len));
		SELF_CHECK(slot_load(4) < E_OK);
		SELF_CHECK(self_same(st[2], len));
		/* in order, but of a size the core does not read */
		SELF_CHECK(self_put(&u, 4, big, (SZ)sizeof(h) + len - 1));
		SELF_CHECK(slot_load(4) < E_OK);
		SELF_CHECK(self_same(st[2], len));
		/* in order and of the size, and the core itself refuses it */
		h.size = len;
		memcpy(big, &h, sizeof(h));
		big[sizeof(h)] ^= 0xff;
		SELF_CHECK(self_put(&u, 4, big, (SZ)sizeof(h) + len));
		SELF_CHECK(slot_load(4) < E_OK);
		SELF_CHECK(self_same(st[2], len));
		free(big);
	}

	/* the older form: reads as number 1, and goes to 1 when something else is saved */
	g_game = v;
	SELF_CHECK(self_old(&v, st[1], len));
	SELF_CHECK(slots_used() == 2U);
	SELF_CHECK(emu_state_load(st[0], len) == 0);
	SELF_CHECK(slot_load(1) >= E_OK);
	SELF_CHECK(self_same(st[1], len));
	SELF_CHECK(emu_state_load(st[3], len) == 0);
	SELF_CHECK(slot_save(3) >= E_OK);
	SELF_CHECK(self_count(&v, 1, &size) == 1 && size == sizeof(SLOTHD) + (UD)len);
	SELF_CHECK(self_count(&v, 0, &size) == 1);
	SELF_CHECK(size == sizeof(SAVEHD) + (UD)emu_sram_size());
	SELF_CHECK(slots_used() == ( 2U | 8U ));
	SELF_CHECK(slot_load(1) >= E_OK);
	SELF_CHECK(self_same(st[1], len));
	SELF_CHECK(slot_load(3) >= E_OK);
	SELF_CHECK(self_same(st[3], len));

	/* saving over number 1 itself: the new state takes 1, the older one is not lost */
	(void)ob_del_obj(&v);
	SELF_CHECK(self_object(&v));
	g_game = v;
	SELF_CHECK(self_old(&v, st[1], len));
	SELF_CHECK(emu_state_load(st[5], len) == 0);
	SELF_CHECK(slot_save(1) >= E_OK);
	SELF_CHECK(slots_used() == ( 2U | 4U ));
	SELF_CHECK(slot_load(1) >= E_OK);
	SELF_CHECK(self_same(st[5], len));
	SELF_CHECK(slot_load(2) >= E_OK);
	SELF_CHECK(self_same(st[1], len));

	/* the battery's save alone moves it too, once */
	(void)ob_del_obj(&v);
	SELF_CHECK(self_object(&v));
	g_game = v;
	SELF_CHECK(self_old(&v, st[2], len));
	(void)keep_put(0, &moved);
	SELF_CHECK(moved == 1);
	(void)keep_put(0, &moved);
	SELF_CHECK(moved == 0);
	SELF_CHECK(slot_load(1) >= E_OK);
	SELF_CHECK(self_same(st[2], len));

	/* the ROM still found though saves come before it. Last: the core then holds new
	   copies of the ROM, whose addresses are among the bytes of a state */
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD);
	if ( key > 0 ) {
		SZ		asz = 0;
		CONST char	*why = NULL;
		BOOL		core = FALSE;

		(void)ob_apd_rec(key, OB_RT_SYSDATA, 0, &rec);
		(void)ob_wri_rec(key, rec, 0, rom, romsz, &asz);
		(void)ob_cls_obj(key);
		SELF_CHECK(game_load(&u, &why, &core));
	}

	if ( mid > 0 ) (void)mn_del_men(mid);
	(void)ob_del_obj(&u);
	(void)ob_del_obj(&v);
	for ( n = 0; n < SLOT_MAX + 2; n++ ) free(st[n]);
	say("selftest slots: %d failed\n", bad, 0);
	return bad;
}

/*
 * The core worked by itself on a small ROM made here: frames run, the
 * picture has the colour the program put in the palette, and a saved
 * state read back gives the same frame again.
 */
static INT selftest( void )
{
	static UB	rom[EMU_ROM_MAX];
	static UB	st[2 * 1024 * 1024];
	uint32_t	*keep;
	CONST uint32_t	*fb;
	INT		i, n, bad = 0, romsz;

	memset(rom, 0xea, sizeof(rom));
#ifdef EMU_SFC
	n = emu_rom_sfc(rom);
#else
	n = emu_rom_nes(rom);
#endif
	romsz = n;
	if ( emu_load(rom, n) != 0 ) {
		say("selftest: not loaded\n", 0, 0);
		tm_putstring((CONST UB *)emu_error());
		return 1;
	}
	for ( i = 0; i < 30; i++ ) emu_frame();
	fb = emu_fb();
	say("selftest: pixel %06x, %d\n", (INT)fb[EMU_W * ( EMU_H / 2 ) + EMU_W / 2], emu_state_size());
	if ( ( fb[EMU_W * ( EMU_H / 2 ) + EMU_W / 2] & 0xffffff ) == 0 ) bad++;
	n = emu_state_save(st, sizeof(st));
	if ( n <= 0 ) bad++;
	keep = (uint32_t *)malloc(EMU_W * EMU_H * 4);
	if ( keep == NULL ) return 2;
	emu_frame();
	memcpy(keep, emu_fb(), EMU_W * EMU_H * 4);
	if ( n > 0 && emu_state_load(st, n) != 0 ) bad++;
	emu_frame();
	if ( memcmp(keep, emu_fb(), EMU_W * EMU_H * 4) != 0 ) bad++;
	free(keep);
	bad += self_slots(rom, romsz);
	say("selftest over: %d failed\n", bad, 0);
	return bad;
}

/* ---------------------------------------------------------------- the start */

static ER open_all( void )
{
	T_OBCRE	c;
	T_OBNTF	req;
	char	json[160];
	INT	sw = 0, sh = 0, x, y;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &g_ch) < E_OK ) return E_NOMEM;
	g_port = ob_opn_obj(&g_ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( g_port <= 0 ) return E_OBJ;
	wm_screen(&sw, &sh);
	x = ( sw > g_w + 16 ) ? ( sw - g_w - 16 ) / 2 : 0;
	y = ( sh > g_h + 60 ) ? ( sh - g_h - 60 ) / 2 : 0;
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%u,\"icon\":\"%s\"}",
		 x, y, x + g_w + 16, y + g_h + 34, (UINT)( WM_ATTR_FRAME | WM_ATTR_TITLE | WM_ATTR_RESIZE ), APP_PROG);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)APP_TITLE;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &g_win) < E_OK ) return E_OBJ;
	g_kw = ob_opn_obj(&g_win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	g_gid = ( g_kw > 0 ) ? wm_obj_gid(g_kw) : -1;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE | OB_E_PRESS | OB_E_KEY | OB_E_REDRAW | OB_E_DROP;
	req.flags = OB_N_KEYUP;		/* a button is held only while its key is down */
	req.id = 7;
	if ( g_kw <= 0 || g_gid < 0 || ob_ntf_evt(g_kw, OB_REC_ANY, &req, g_port) <= 0 ) return E_OBJ;
	work_size();
	return E_OK;
}

int main( void )
{
	T_OBNTM	m;
	SZ	asz = 0;
	char	arg[16];
	UD	start, shown = 0;

	memset(arg, 0, sizeof(arg));
	if ( ts_get_arg(arg, sizeof(arg) - 1) >= 8 && memcmp(arg, "selftest", 8) == 0 ) return selftest();
	if ( open_all() < E_OK ) {
		say("no window\n", 0, 0);
		return 2;
	}
	paint();
	say("ready\n", 0, 0);
	start = ui_now();
	while ( !g_closing ) {
		TMO	tmo = ( g_loaded && !g_paused ) ? TMO_POL : 100;

		while ( !g_closing && ob_rea_tmo(g_port, 0, 0, &m, sizeof(m), &asz, tmo) >= E_OK ) {
			event(&m);
			tmo = TMO_POL;
		}
		if ( g_closing || !g_loaded || g_paused ) {
			start = ui_now();
			shown = 0;
			continue;
		}
		{
			/* 60 frames a second; a slow machine skips what it cannot draw, not the game's time */
			UD	now = ui_now(), due;

			if ( g_repace ) {
				g_repace = FALSE;
				start = now;
				shown = 0;
			}
			due = ( now - start ) * FPS_X100 * (UD)g_speed / ( 100 * 1000 * SPEED_ONE );

			if ( shown >= due ) {
				tk_dly_tsk(FRAME_MS / 4);
				continue;
			}
			if ( due - shown > CATCHUP_MAX ) {
				start = now;
				shown = 0;
				due = 1;
			}
			while ( shown < due ) {
				UINT	pads = g_pad | g_tap;

#ifdef EMU_SFC
				pads |= pad_read(0);
				emu_input(1, pad_read(1));
				g_fps_cnt++;
#endif
				emu_input(0, (uint16_t)pads);
				g_tap = 0;
				emu_frame();
				snd_frame();
				shown++;
				if ( ui_now() - now >= BATCH_MS ) {	/* the rest is let go: slow, not stuck */
					start = ui_now();
					shown = 0;
					break;
				}
			}
#ifdef EMU_SFC
			if ( now - g_fps_at >= 1000 ) {
				g_fps_val = g_fps_cnt;
				g_fps_cnt = 0;
				g_fps_at = now;
			}
#endif
		}
		paint();
	}
	if ( g_loaded ) {
		INT	moved;

		(void)keep_put(0, &moved);
	}
	if ( g_told ) (void)wm_msg_put(NULL);
	snd_close();
#ifdef EMU_SFC
	pad_close();
#endif
	if ( g_kw > 0 ) {
		(void)ob_cls_obj(g_kw);
		(void)ob_del_obj(&g_win);
	}
	(void)ob_cls_obj(g_port);
	(void)ob_del_obj(&g_ch);
	say("closed\n", 0, 0);
	return 0;
}
