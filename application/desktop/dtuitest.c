/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtuitest.c
 *	Driving the desktop from outside, for the tests of what it shows
 *	(make DESKTOP=1 UITEST=1; tools/ui_run.py). The same lines come over
 *	the network in make TESTAGENT=1 (dttagent.c), one line done at a time
 *	whichever way it came.
 *
 *	A task reads lines from the second serial port (serb) and does
 *	what each says, answering "ok ..." or "err ..." on the console,
 *	each line marked "@ut ", when it is done, so that the one driving it goes on at once rather
 *	than waiting a fixed time. What it puts in goes in as the keyboard
 *	and the pointer would give it, written to the input object 入力
 *	(OB_IN_EVENTS) with a key kept open, at the place asked for, in one
 *	step. The lines:
 *
 *	  ready			answered once the desktop is up
 *	  move X Y		the pointer to X,Y
 *	  down [B] / up [B]	a button (0 the first, 1 the second) pressed, let go
 *	  click X Y [B]		there, pressed and let go at once
 *	  dclick X Y		two clicks, as close as a double press
 *	  hold X Y [B]		there, pressed and held (a menu opened and kept)
 *	  drag X1 Y1 X2 Y2 [B]	pressed at one place, carried in steps, let go at the other
 *	  key CODE [MODS]	a key by its usage code (hex), with the modifiers (hex)
 *	  text STRING		letters, digits and . - / : typed one after another
 *	  wheel N		the wheel turned N notches (up is positive)
 *	  open UUID		the object opened, as a double press on a link to it opens it
 *	  run PATH [N ...]	the program file started as a process, the numbers (up
 *				to eight) its start-up argument: "ok PID"
 *	  win TITLE		the window whose band says TITLE: "ok L T R B" of its outside
 *	  wins			every window shown, nearest first: "ok N" then one line each
 *	  front TAG T1|T2|...	the windows shown whose bands say T1, T2, ... are the
 *				nearest ones, in that order: "ok TAG: in front"
 *	  hidden TAG T		no window shown says T: "ok TAG: hidden"; both are
 *				looked at again for up to two seconds while the screen settles,
 *				and the TAG lets a test expect each answer apart
 *	  accpos ID		where the program of the id stands among the
 *				accessories, of how many, and + when 小物 lists it
 *	  progrm UUID		the program taken out as 削除 does, without the panel
 *	  proginst UUID		the object installed or registered as a drop on
 *				the list does, without the panel
 *	  proginstq UUID	the same to replace what is there, for a process
 *				with no rights, answered at once: the password's
 *				panel is then up, and progres tells what came of it
 *	  progres		"ok -1" while that goes on, then its error code
 *	  adminpanels		how many of the administrator's panels have been shown
 *	  mkpkg			a small package (.tpk) of one accessory, as an object
 *				of a fixed UUID (UT_PKG), made if not there
 *	  refs UUID		the object's count of references, -1 when it has gone
 *	  emurom		the ROMs of the tests of BeFC and BeSFC, objects of the
 *				fixed UUIDs of a test (UT_ROM_NES, UT_ROM_SFC), made if not there
 *	  emuromx UUID NAME KB COPIER TYPE MAP  a ROM of BeSFC's of the shape of a real
 *				cartridge (type and map byte in hex), MAP 0: bytes that are no ROM
 *	  wait MS		nothing for so long, and then the answer
 *	  idle			the input taken and the screen made up (a short wait)
 *	  lag MS		the pointer moved to and fro for MS ms: "ok WORST MOVES LATE",
 *				the longest the screen took to show a move (ms), the moves,
 *				and how many took longer than LAG_LATE_MS
 *	  look NUM VALUE	an entry of the look table set (LK_ROUND and the like)
 *				and the screen built again
 *	  stat			what was built and sent since the last stat: "ok BUILDS PARTS
 *				KPX_BUILT MOVES MOVES_BUILT RECTS_SENT KPX_SENT", the pointer's
 *				moves and how many of them came with something built
 *	  pnstat		since the last pnstat: "ok DRAWS TORN", the panels drawn
 *				whole and the builds that read the windows while one was drawn
 *	  wifitest		on a machine with no Wi-Fi card, the Wi-Fi manager's card
 *				for the tests (a build with the Wi-Fi): wlan0 with a scan's
 *				networks, its saved networks changed as on a card
 *	  xml TEXT		a line of xmlTAD kept, after the lines kept before it
 *	  focus TAG TITLE	the window of that title has the keys
 *	  xmlopen TITLE		the xmlTAD kept, a document, opened in a window titled
 *				TITLE that can be edited (no object behind it), and forgotten
 *	  xmlbig		the xmlTAD kept written as record 0 of the large object made
 *				last (mkbig), and forgotten: a text of the test's own made an
 *				object, for the desktop's editor and the process alike
 *	  caret			the caret of the document in front: "ok c PARA,POS a PARA,POS",
 *				the caret and the other end of the selection
 *	  mkbig doc|fig|calc N [M [T [V]]]
 *				a large object for the tests of size, a copy of the
 *				template of its kind with record 0 made here: a text of N
 *				paragraphs, a figure of N rectangles and lines of M points
 *				and T pieces of text (ABC, 120 by 40, from 100 across and
 *				120 below the lines, 150 apart; with V 1 written down the
 *				line, 縦書き, in boxes 40 by 120), or a sheet of N rows and M
 *				columns: "ok UUID BYTES"
 *	  sum [show] [anon] [link] [keys] [res NAME] UUID|big|bigN
 *				record 0 of the object: "ok BYTES HASH" (FNV-1a, 64 bits);
 *				with show, said on the console too, 200 bytes a line ("rec: ");
 *				with anon, every UUID written in it counted as noughts (what
 *				is the same but for identities made new); with link, the
 *				record of the object its last link points at instead, its
 *				name after the hash; with res, the resource NAME beside the
 *				records instead ("_0_1.png", a picture the canvas painted);
 *				with keys, the record of the desktop's window with the keys
 *				as it is now, and with keys link the stored record 0 of the
 *				object its last link points at. bigN is the large object
 *				made N before the last (1 to 7), wherever big is taken
 *	  objects [new]		how many objects the volumes keep: "ok N"; with new, how
 *				many more there are than at the last objects without it
 *	  paper [UUID|big|bigN]	the paper the object's metadata keeps: "ok RRGGBB", or
 *				"ok none" when it keeps none
 *	  ftdef			the palettes of the 道具パネル laid out as panels of a data
 *				box, said on the console ("ftdef: "): what the data box of
 *				基本図形編集 (SYSDEF_BOX_FIG) holds for the process's palettes
 *	  shapes		the figure in front: "ok SHAPES PICKED"
 *	  pfig UUID|big		the object opened with 基本図形編集 as a process, whatever
 *				its program object says
 *	  figproc 1|kernel|0	every figure opened with 基本図形編集 as a process from now
 *				(1), with the desktop's own figure editor even when the
 *				program object says the process (kernel), or as the
 *				program object says again (0)
 *	  pdoc UUID|big		the object opened with 基本文章編集 as a process, whatever
 *				its program object says
 *	  docproc 1|kernel|0	the same as figproc, for documents and 基本文章編集
 *	  pcab UUID|big		the object opened with 仮身一覧 as a process, whatever
 *				its program object says
 *	  cabproc 1|kernel|0	the same as figproc, for 仮身一覧
 *	  linkbig X Y		a link to the large object made last put in the window at
 *				X, Y of the screen
 *	  mark			the time from which the next two count
 *	  waitwin TITLE [S]	a window that says TITLE (spaces and all) shown, looked for up to S seconds
 *				(60): "ok MS" since the mark
 *	  proginstf UUID	proginst replacing what is there whatever its version
 *	  sysup UUID [force]	the package of the system object put on, step by step by
 *				the desktop's loop as the panel of システム環境設定 has it:
 *				"ok" and starting again finishes it; "err sysup N"
 *	  sync [S]		the desktop's own task come round its loop and found no
 *				input left (all that was put in before taken and done),
 *				waited for up to S seconds (60): "ok MS" since the mark
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/hid.h>
#include <ts/ser.h>
#include <ts/wm.h>
#include <ts/disp.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/dtreq.h>
#include <ts/proc.h>
#include <ts/om.h>
#include <ts/fs.h>
#include <ts/lockup.h>
#include <ts/figtool.h>
#include "desktop.h"

#ifdef USE_UITEST
#include "../befc/emu_rom.h"

IMPORT void knl_wifi_test_card( void ) __attribute__((weak));	/* device/net/wifi/wifi.c, with the Wi-Fi */

#define UT_LINE		256
#define UT_STEPS	12		/* moves a drag is made of */
#define UT_GAP_MS	15		/* between the pieces of one action */
#define UT_DCLICK_MS	60		/* between the two presses of a double click */

LOCAL ID	ut_dd = 0;

/* The large object made last: what "open big" and "sum" with no object take */
LOCAL TS_UUID	ut_big;
LOCAL BOOL	ut_big_have = FALSE;
#define UT_BIG_BACK	7
LOCAL TS_UUID	ut_big_was[UT_BIG_BACK];	/* the ones made before it, the newest first */
LOCAL INT	ut_big_nwas = 0;
LOCAL INT	ut_objects = 0;			/* how many objects there were at the last "objects" */
LOCAL volatile INT ut_sync = 0;		/* 1 asked, 3 seen by the loop, 2 no input left after that */
LOCAL INT	ut_x = 0, ut_y = 0;

/*
 * An answer goes out on the console, each line marked "@ut ", which the
 * runner reads from the console's log: the console reaches the host
 * however busy the machine is.
 */
LOCAL char	ut_out[UT_LINE + 8];
LOCAL INT	ut_outn = 0;
LOCAL void	(*ut_sink)( CONST char *answer ) = NULL;	/* who else is told (dttagent.c) */

/* Gathered into whole lines, each written at once so that what other tasks print cannot come between */
LOCAL void say( CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		if ( ut_outn == 0 ) {
			ut_out[0] = '@';  ut_out[1] = 'u';  ut_out[2] = 't';  ut_out[3] = ' ';
			ut_outn = 4;
		}
		if ( ut_outn < UT_LINE + 6 ) ut_out[ut_outn++] = *s;
		if ( *s == '\n' ) {
			ut_out[ut_outn] = 0;
			tm_putstring((UB *)ut_out);
			if ( ut_sink != NULL ) ut_sink(ut_out + 4);
			ut_outn = 0;
		}
	}
}

/*
 * A ROM of the tests, as an object: record 0 a word on what it is, record
 * 1 the ROM, as a game's object has it
 */
#define UT_PKG		"01a0d8c4-5500-7f03-8b12-3c4d5e6f7b72"
#define UT_PKG_PROG	"01a0d8c4-5500-7f04-8b12-3c4d5e6f7b73"
#define UT_NO_RIGHTS	9999		/* a process that does not exist has no rights */
#define UT_ROM_NES	"01a0d8c4-5500-7f01-8b12-3c4d5e6f7b70"
#define UT_ROM_SFC	"01a0d8c4-5501-7f01-8b12-3c4d5e6f7b71"

/* An object of the given UUID holding the ROM in a record of its own, made if not there */
LOCAL ER ut_rom_store( CONST char *id, CONST char *json, CONST UB *rom, SZ n )
{
	T_OBCRE	c;
	TS_UUID	u, got;
	INT	recno = -1;
	SZ	asz = 0;
	ID	key;
	ER	er;
	static CONST char	note[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>test</p></document></tad>";

	(void)ts_str_to_uuid(id, &u);
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key > 0 ) {
		(void)ob_cls_obj(key);
		return E_OK;
	}
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.uuid = u;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)knl_strlen(json);
	knl_ob_make_for_person(TRUE);
	er = ob_cre_obj(&c, &got);
	knl_ob_make_for_person(FALSE);
	if ( er >= E_OK ) {
		key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
		er = ( key > 0 ) ? E_OK : (ER)key;
		if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
		if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, note, (SZ)( sizeof(note) - 1 ), &asz);
		if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno);
		if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, rom, n, &asz);
		if ( key > 0 ) (void)ob_cls_obj(key);
	}
	return er;
}

LOCAL ER ut_rom_make( CONST char *id, CONST char *json, BOOL sfc )
{
	UB	*rom = (UB *)Kmalloc(EMU_ROM_MAX);
	INT	n, i;
	ER	er;

	if ( rom == NULL ) return E_NOMEM;
	for ( i = 0; i < EMU_ROM_MAX; i++ ) rom[i] = 0xea;
	n = sfc ? emu_rom_sfc(rom) : emu_rom_nes(rom);
	er = ut_rom_store(id, json, rom, n);
	Kfree(rom);
	return er;
}

LOCAL void ut_emurom( void );
LOCAL void ut_emuromx( CONST char *s );
LOCAL CONST char *skip( CONST char *s );
LOCAL BOOL num( CONST char **p, INT *v, BOOL hex );

LOCAL ID	ut_in_key = 0;		/* 入力, opened once to put events in */

/* One event put in, as a keyboard or the pointer would have reported it */
LOCAL void inject( CONST T_HIDEV *e )
{
	if ( ut_in_key <= 0 ) {
		ut_in_key = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE | OB_OP_EXEC);
		if ( ut_in_key <= 0 ) {
			ut_in_key = 0;
			return;
		}
	}
	(void)ob_wri_rec(ut_in_key, OB_IN_EVENTS, 0, e, sizeof(*e), NULL);
}

LOCAL void ut_emurom( void )
{
	static CONST char	jn[] = "{\"name\":\"試験ROM(FC)\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	static CONST char	js[] = "{\"name\":\"試験ROM(SFC)\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	ER	a = ut_rom_make(UT_ROM_NES, jn, FALSE), b = ut_rom_make(UT_ROM_SFC, js, TRUE);
	char	t[48];

	(void)tm_sprintf((UB *)t, (UB *)"ok %d %d\n", (INT)a, (INT)b);
	say(t);
}

/*
 * A ROM of BeSFC's in the shape of a real cartridge, or (a map of 0) bytes that are
 * no ROM: emuromx UUID NAME KB COPIER TYPE MAP
 */
LOCAL void ut_emuromx( CONST char *s )
{
	static char	id[40], name[32], js[256];
	INT		kb = 0, cp = 0, type = 0, map = 0, i;
	UB		*rom;
	long		n;
	ER		er = E_PAR;

	s = skip(s);
	for ( i = 0; i < 36 && *s != 0 && *s != ' '; i++ ) id[i] = *s++;
	id[i] = 0;
	s = skip(s);
	for ( i = 0; i < 31 && *s != 0 && *s != ' '; i++ ) name[i] = *s++;
	name[i] = 0;
	if ( num(&s, &kb, FALSE) && num(&s, &cp, FALSE) && num(&s, &type, TRUE) && num(&s, &map, TRUE) && kb > 0 ) {
		rom = (UB *)Kmalloc((SZ)kb * 1024 + 512);
		er = E_NOMEM;
		if ( rom != NULL ) {
			if ( map != 0 ) {
				n = emu_rom_sfc_shape(rom, (long)kb * 1024, cp, type, map);
			} else {
				n = (long)kb * 1024;
				for ( i = 0; i < n; i++ ) rom[i] = (UB)( 'a' + i % 26 );
			}
			(void)tm_sprintf((UB *)js, (UB *)"{\"name\":\"%s\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}", name);
			er = ut_rom_store(id, js, rom, (SZ)n);
			Kfree(rom);
		}
	}
	(void)tm_sprintf((UB *)js, (UB *)"ok %d\n", (INT)er);
	say(js);
}

/* A program taken out or put in by the desktop's own loop (dt_uitest_poll), as the requests are */
LOCAL volatile INT	ut_prog_state = 0;	/* 1 asked, 2 done */
LOCAL INT		ut_prog_what;
LOCAL TS_UUID		ut_prog_u;
LOCAL ER		ut_prog_er;
LOCAL BOOL		ut_prog_force = FALSE;
LOCAL ID		ut_prog_pid = 0;

LOCAL void prog_ask( CONST char *us, INT what )
{
	char	t[48];
	INT	tries;

	if ( ts_str_to_uuid(us, &ut_prog_u) < E_OK ) {
		say("err no uuid\n");
		return;
	}
	if ( what == 2 ) {
		(void)tm_sprintf((UB *)t, (UB *)"ok %d\n", om_store_refs(&ut_prog_u));
		say(t);
		return;
	}
	ut_prog_what = what;
	ut_prog_force = (BOOL)( what == 3 || what == 4 );
	ut_prog_pid = ( what == 3 ) ? UT_NO_RIGHTS : 0;
	ut_prog_state = 1;
	if ( what == 3 ) {
		say("ok\n");
		return;
	}
	for ( tries = 0; tries < 12000 && ut_prog_state != 2; tries++ ) {
		tk_dly_tsk(50);
	}
	(void)tm_sprintf((UB *)t, (UB *)"ok %d\n", (INT)ut_prog_er);
	say(t);
	ut_prog_state = 0;
}

/*
 * The system's package put on by the desktop's loop, a step each time
 * round, so that the screen stays alive through what takes minutes
 */
LOCAL volatile INT	ut_su_state = 0;	/* 1 asked, 3 going, 2 done */
LOCAL TS_UUID		ut_su_u;
LOCAL BOOL		ut_su_force = FALSE;
LOCAL ER		ut_su_er;
LOCAL INT		ut_su_total = 0;

LOCAL void sysup_poll( void )
{
	INT	done = 0, total = 0;

	if ( ut_su_state == 1 ) {
		ut_su_state = 3;
		ut_su_er = dt_sysup_begin(&ut_su_u, ut_su_force, 0, &ut_su_total);
		if ( ut_su_er < E_OK || ut_su_total <= 0 ) {
			ut_su_state = 2;
		}
	} else if ( ut_su_state == 3 ) {
		ut_su_er = dt_sysup_step(0, &done, &total);
		if ( ut_su_er < E_OK || done >= total ) {
			ut_su_state = 2;
		}
	}
}

LOCAL void sysup_ask( CONST char *s )
{
	char	t[32];
	INT	k;

	if ( ts_str_to_uuid(s, &ut_su_u) < E_OK ) {
		say("err no uuid\n");
		return;
	}
	for ( k = 0; s[k] != 0 && s[k] != ' '; k++ ) ;
	ut_su_force = (BOOL)( s[k] == ' ' );
	ut_su_state = 1;
	while ( ut_su_state != 2 ) {
		tk_dly_tsk(100);
	}
	ut_su_state = 0;
	if ( ut_su_er < E_OK ) {
		(void)tm_sprintf((UB *)t, (UB *)"err sysup %d\n", (INT)ut_su_er);
		say(t);
	} else {
		say("ok\n");
	}
}

LOCAL void progres_say( void )
{
	char	t[32];

	(void)tm_sprintf((UB *)t, (UB *)"ok %d\n", ( ut_prog_state == 2 ) ? (INT)ut_prog_er : -1);
	say(t);
}

LOCAL void adminpanels_say( void )
{
	char	t[32];

	(void)tm_sprintf((UB *)t, (UB *)"ok %d\n", dt_admin_panels());
	say(t);
}

/* One entry of a package: its name, where its bytes are, how many, and the record's type */
LOCAL void pk_entry( UB *d, CONST char *name, UW off, UW size, UW rt )
{
	INT	i;

	knl_memset(d, 0, 64);
	for ( i = 0; name[i] != 0; i++ ) d[i] = (UB)name[i];
	for ( i = 0; i < 4; i++ ) {
		d[48 + i] = (UB)( off >> ( 8 * i ) );
		d[52 + i] = (UB)( size >> ( 8 * i ) );
		d[56 + i] = (UB)( rt >> ( 8 * i ) );
	}
}

LOCAL void mkpkg_say( void )
{
	static CONST char	pk[] = "{\"format\":1,\"program\":\"" UT_PKG_PROG "\",\"id\":\"ut-acc\",\"name\":\"UT小物\",\"version\":\"R1.000\",\"objects\":[]}";
	static CONST char	pm[] = "{\"name\":\"UT小物\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":0,\"editable\":false,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"program\":{\"id\":\"ut-acc\",\"kind\":\"accessory\",\"name\":\"UT小物\",\"version\":\"R1.000\",\"menu\":true,\"builtin\":false,\"base\":[]}}}";
	static CONST char	note[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>test</p></document></tad>";
	static CONST char	om[] = "{\"name\":\"UT小物のパッケージ\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	UW	pl = (UW)knl_strlen(pk), ml = (UW)knl_strlen(pm), off = 16 + 2 * 64, all = off + pl + ml;
	UB	*b = (UB *)Kcalloc(1, (SZ)all);
	T_OBCRE	c;
	TS_UUID	u, got;
	T_OBREF	r;
	ER	er;
	char	t[32];

	(void)ts_str_to_uuid(UT_PKG, &u);
	if ( b == NULL ) {
		say("err no memory\n");
		return;
	}
	if ( ob_ref_obj(&u, &r) >= E_OK ) {
		Kfree(b);
		say("ok 0\n");
		return;
	}
	b[0] = 'T';  b[1] = 'S';  b[2] = 'P';  b[3] = 'K';
	b[4] = 1;
	b[6] = 2;
	pk_entry(b + 16, "package.json", off, pl, 15);
	pk_entry(b + 16 + 64, UT_PKG_PROG ".json", off + pl, ml, 15);
	knl_memcpy(b + off, pk, pl);
	knl_memcpy(b + off + pl, pm, ml);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.uuid = u;
	c.json = (CONST UB *)om;
	c.jsonsz = (SZ)knl_strlen(om);
	er = ob_cre_obj(&c, &got);
	if ( er >= E_OK ) {
		(void)ob_lnk_obj(&u);
		er = om_obj_record_put(&u, 0, 1, 0, (CONST UB *)note, (SZ)knl_strlen(note));
		if ( er >= E_OK ) er = om_obj_record_put(&u, 1, 15, 0, b, (SZ)all);
	}
	Kfree(b);
	(void)tm_sprintf((UB *)t, (UB *)"ok %d\n", (INT)er);
	say(t);
}

LOCAL void accpos_say( CONST char *id )
{
	char	t[48];
	INT	n = 0, at;
	BOOL	show = FALSE;

	at = dt_acc_pos((CONST UB *)id, &n, &show);
	(void)tm_sprintf((UB *)t, (UB *)"ok %d %d %c\n", at, n, show ? '+' : '-');
	say(t);
}

LOCAL void ev( UINT type, UINT code, UINT mods )
{
	T_HIDEV	e;

	knl_memset(&e, 0, sizeof(e));
	e.type = type;
	e.code = code;
	e.mods = mods;
	e.x = ut_x;
	e.y = ut_y;
	inject(&e);
}

LOCAL void move( INT x, INT y )
{
	ut_x = x;
	ut_y = y;
	ev(HID_EV_MOVE, 0, 0);
}

/* ---------------------------------------------------------------- the words of a line */

LOCAL CONST char *skip( CONST char *s )
{
	while ( *s == ' ' || *s == '\t' ) s++;
	return s;
}

/* A number, decimal or with 0x hex; FALSE when there is none */
LOCAL BOOL num( CONST char **p, INT *v, BOOL hex )
{
	CONST char	*s = skip(*p);
	INT		n = 0, sign = 1, d, any = 0;

	if ( *s == '-' ) {
		sign = -1;
		s++;
	}
	if ( s[0] == '0' && ( s[1] == 'x' || s[1] == 'X' ) ) {
		hex = TRUE;
		s += 2;
	}
	for ( ;; s++, any++ ) {
		if ( *s >= '0' && *s <= '9' ) d = *s - '0';
		else if ( hex && *s >= 'a' && *s <= 'f' ) d = *s - 'a' + 10;
		else if ( hex && *s >= 'A' && *s <= 'F' ) d = *s - 'A' + 10;
		else break;
		n = n * ( hex ? 16 : 10 ) + d;
	}
	*p = s;
	*v = n * sign;
	return (BOOL)( any > 0 );
}

LOCAL BOOL word( CONST char **p, CONST char *w )
{
	CONST char	*s = skip(*p);
	INT		i;

	for ( i = 0; w[i] != 0; i++ ) {
		if ( s[i] != w[i] ) return FALSE;
	}
	if ( s[i] != 0 && s[i] != ' ' && s[i] != '\t' ) return FALSE;
	*p = s + i;
	return TRUE;
}

/* The usage code a letter, digit or sign is typed with, and whether with Shift */
LOCAL UINT usage_of( char c, BOOL *p_shift )
{
	*p_shift = FALSE;
	if ( c >= 'a' && c <= 'z' ) return 0x04 + (UINT)( c - 'a' );
	if ( c >= 'A' && c <= 'Z' ) { *p_shift = TRUE; return 0x04 + (UINT)( c - 'A' ); }
	if ( c >= '1' && c <= '9' ) return 0x1E + (UINT)( c - '1' );
	if ( c == '0' ) return 0x27;
	if ( c == ' ' ) return 0x2C;
	if ( c == '.' ) return 0x37;
	if ( c == '-' ) return 0x2D;
	if ( c == '/' ) return 0x38;
	if ( c == ':' ) return 0x34;		/* the Japanese keyboard's colon, unshifted */
	if ( c == ',' ) return 0x36;
	if ( c == ';' ) return 0x33;
	if ( c == '[' ) return 0x30;
	if ( c == ']' ) return 0x31;
	if ( c == '@' ) return 0x2F;
	if ( c == '^' ) return 0x2E;
	{
		/* the signs over the digits and the other keys, shifted, as the Japanese keyboard has them */
		static CONST char	sh[] = "!\"#$%&'()";
		INT			i;

		for ( i = 0; sh[i] != 0; i++ ) {
			if ( c == sh[i] ) {
				*p_shift = TRUE;
				return 0x1E + (UINT)i;
			}
		}
	}
	*p_shift = TRUE;
	if ( c == '=' ) return 0x2D;
	if ( c == '+' ) return 0x33;
	if ( c == '*' ) return 0x34;
	if ( c == '<' ) return 0x36;
	if ( c == '>' ) return 0x37;
	if ( c == '?' ) return 0x38;
	if ( c == '{' ) return 0x30;
	if ( c == '}' ) return 0x31;
	*p_shift = FALSE;
	return 0;
}

/* ---------------------------------------------------------------- the windows */

LOCAL BOOL same_title( CONST char *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

LOCAL void put_num( char *b, INT *n, INT v )
{
	char	t[12];
	INT	k = 0;

	if ( v < 0 ) {
		b[(*n)++] = '-';
		v = -v;
	}
	do {
		t[k++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( k > 0 ) b[(*n)++] = t[--k];
}

LOCAL void say_rect( CONST char *head, INT wid, CONST char *tail )
{
	T_WMWIN	w;
	char	b[UT_LINE];
	INT	n = 0, i;

	if ( wm_ref(wid, &w) < E_OK ) {
		say("err no window\n");
		return;
	}
	for ( i = 0; head[i] != 0; i++ ) b[n++] = head[i];
	put_num(b, &n, w.outer.left);  b[n++] = ' ';
	put_num(b, &n, w.outer.top);   b[n++] = ' ';
	put_num(b, &n, w.outer.right); b[n++] = ' ';
	put_num(b, &n, w.outer.bottom);
	for ( i = 0; tail != NULL && tail[i] != 0 && n < UT_LINE - 2; i++ ) b[n++] = tail[i];
	b[n++] = '\n';
	b[n] = 0;
	say(b);
}

/*
 * The windows shown with a band, front first, however many there are:
 * the list asked for again into twice the room while it filled all it
 * was given. Freed by the caller (Kfree); NULL with *p_n 0 when there
 * is no memory.
 */
LOCAL INT *ut_windows( INT *p_n )
{
	INT	cap = 32, n;
	INT	*wids;

	for ( ;; ) {
		wids = (INT *)Kmalloc((SZ)cap * sizeof(INT));
		if ( wids == NULL ) {
			*p_n = 0;
			return NULL;
		}
		n = wm_list(wids, cap);
		if ( n < cap ) {
			*p_n = n;
			return wids;
		}
		Kfree(wids);
		cap *= 2;
	}
}

/* The first window shown with a band that says title, 0 when there is none */
LOCAL INT ut_named( CONST char *title )
{
	INT	n, i, got = 0;
	INT	*wids = ut_windows(&n);
	char	t[WM_TITLE_MAX];

	for ( i = 0; i < n && got == 0; i++ ) {
		if ( wm_title(wids[i], t, sizeof(t)) >= 0 && same_title(t, title) ) got = wids[i];
	}
	if ( wids != NULL ) Kfree(wids);
	return got;
}

/* The window of that name, looked for again for up to two seconds while it opens */
LOCAL void find_win( CONST char *title )
{
	INT	tries, wid;

	for ( tries = 0; tries < 20; tries++ ) {
		if ( ( wid = ut_named(title) ) > 0 ) {
			say_rect("ok ", wid, NULL);
			return;
		}
		tk_dly_tsk(100);
	}
	say("err no such window\n");
}

/* A word of a line, up to the next space, into w; FALSE when there is none */
LOCAL BOOL token( CONST char **p, char *w, INT max )
{
	CONST char	*s = skip(*p);
	INT		i = 0;

	while ( s[i] != 0 && s[i] != ' ' && s[i] != '\t' && i < max - 1 ) {
		w[i] = s[i];
		i++;
	}
	w[i] = 0;
	*p = s + i;
	return (BOOL)( i > 0 );
}

/*
 * Where the link to an object of that name stands in the window of that
 * name (a figure of links, as a 原紙箱 or a cabinet is), on the screen:
 * looked for over the work area, the box of the first one found. FALSE
 * when there is none.
 */
LOCAL BOOL link_named( CONST char *title, CONST char *name, T_DPRECT *p_box )
{
	DTWIN		*d;
	T_WMWIN		w;
	T_DPRECT	page, box;
	T_VOBJ		v;
	UB		nm[TAD_NAME_MAX];
	INT		wid = 0, tries, x, y, which, cw, ch;

	for ( tries = 0; tries < 20 && ( wid = ut_named(title) ) <= 0; tries++ ) {
		tk_dly_tsk(100);
	}
	d = ( wid > 0 ) ? dt_win_of(wid) : NULL;
	if ( d == NULL || d->fig == NULL || d->rec == NULL || wm_ref(wid, &w) < E_OK ) {
		return FALSE;
	}
	dt_work_rect(d, &page);
	cw = w.work.right - w.work.left;
	ch = w.work.bottom - w.work.top;
	for ( y = 0; y < ch; y += 4 ) {
		for ( x = 0; x < cw; x += 8 ) {
			if ( tv_fig_at(d->fig, &page, d->scroll_x, d->scroll_y, d->rec, x, y, &v, &box, &which) < E_OK ) {
				continue;
			}
			if ( om_store_name(&v.target, nm, sizeof(nm)) > 0 && same_title((CONST char *)nm, name) ) {
				p_box->left = w.work.left + box.left;
				p_box->top = w.work.top + box.top;
				p_box->right = w.work.left + box.right;
				p_box->bottom = w.work.top + box.bottom;
				return TRUE;
			}
		}
	}
	return FALSE;
}

LOCAL void all_wins( void )
{
	INT	n, i, k = 0;
	INT	*wids = ut_windows(&n);
	char	b[16], t[WM_TITLE_MAX + 2];

	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, n);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
	for ( i = 0; i < n; i++ ) {
		t[0] = ' ';
		if ( wm_title(wids[i], t + 1, WM_TITLE_MAX) < 0 ) t[1] = 0;
		say_rect("", wids[i], t);
	}
	if ( wids != NULL ) Kfree(wids);
}

/*
 * The tag of a front or hidden line, and where what follows it begins:
 * FALSE when there is no tag
 */
LOCAL BOOL tag_of( CONST char **p, char *tag, INT max )
{
	CONST char	*s = skip(*p);
	INT		n = 0;

	while ( *s != 0 && *s != ' ' && *s != '\t' && n < max - 1 ) tag[n++] = *s++;
	tag[n] = 0;
	*p = skip(s);
	return (BOOL)( n > 0 );
}

LOCAL void say_tagged( CONST char *tag, CONST char *what )
{
	char	b[UT_LINE];
	INT	n = 0, i;

	b[n++] = 'o';  b[n++] = 'k';  b[n++] = ' ';
	for ( i = 0; tag[i] != 0 && n < UT_LINE - 40; i++ ) b[n++] = tag[i];
	b[n++] = ':';  b[n++] = ' ';
	for ( i = 0; what[i] != 0 && n < UT_LINE - 2; i++ ) b[n++] = what[i];
	b[n++] = '\n';
	b[n] = 0;
	say(b);
}

/* Whether the n names of 'names' (split by '|') are the first n windows of the list, in that order */
LOCAL BOOL front_is( CONST char *names )
{
	INT	n, i = 0, k;
	INT	*wids = ut_windows(&n);
	char	want[WM_TITLE_MAX], t[WM_TITLE_MAX];
	BOOL	yes = TRUE;

	while ( *names != 0 && yes ) {
		for ( k = 0; names[k] != 0 && names[k] != '|' && k < WM_TITLE_MAX - 1; k++ ) {
			want[k] = names[k];
		}
		want[k] = 0;
		names += k;
		if ( *names == '|' ) names++;
		if ( i >= n || wm_title(wids[i], t, sizeof(t)) < 0 || !same_title(t, want) ) {
			yes = FALSE;
		}
		i++;
	}
	if ( wids != NULL ) Kfree(wids);
	return (BOOL)( yes && i > 0 );
}

LOCAL BOOL shown_named( CONST char *title )
{
	return (BOOL)( ut_named(title) > 0 );
}

LOCAL void front_check( CONST char *s )
{
	char	tag[32];
	INT	tries;

	if ( !tag_of(&s, tag, sizeof(tag)) || *s == 0 ) {
		say("err front TAG T1|T2\n");
		return;
	}
	for ( tries = 0; tries < 20; tries++ ) {
		if ( front_is(s) ) {
			say_tagged(tag, "in front");
			return;
		}
		tk_dly_tsk(100);
	}
	say("err not in front\n");
}

/* Asked until the window of that title has the keys: a window of a program comes when it is ready */
LOCAL void focus_check( CONST char *s )
{
	char	tag[32], t[WM_TITLE_MAX];
	INT	tries, wid;

	if ( !tag_of(&s, tag, sizeof(tag)) || *s == 0 ) {
		say("err focus TAG TITLE\n");
		return;
	}
	for ( tries = 0; tries < 20; tries++ ) {
		wid = wm_focused();
		if ( wid > 0 && wm_title(wid, t, sizeof(t)) >= 0 && same_title(t, s) ) {
			say_tagged(tag, "has the keys");
			return;
		}
		tk_dly_tsk(100);
	}
	say("err keys elsewhere\n");
}

LOCAL void hidden_check( CONST char *s )
{
	char	tag[32];
	INT	tries;

	if ( !tag_of(&s, tag, sizeof(tag)) || *s == 0 ) {
		say("err hidden TAG T\n");
		return;
	}
	for ( tries = 0; tries < 20; tries++ ) {
		if ( !shown_named(s) ) {
			say_tagged(tag, "hidden");
			return;
		}
		tk_dly_tsk(100);
	}
	say("err still shown\n");
}

/* An object opened as a double press on a link to it opens it, asked of the desktop as a program asks */
/*
 * "kslide UUID": the object shown with the desktop's own slides
 * (dtslide.c) whatever its program object says, so that a test measures
 * them and the slides of the process (application/slide) in one run
 */
LOCAL void kslide( CONST char *us )
{
	T_VOBJ	*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));

	if ( v == NULL ) {
		say("err no memory\n");
		return;
	}
	knl_memset(v, 0, sizeof(*v));
	v->recno = -1;
	if ( ts_str_to_uuid(us, &v->target) < E_OK ) {
		Kfree(v);
		say("err no uuid\n");
		return;
	}
	dt_slide_open(NULL, v);
	Kfree(v);
	say("ok\n");
}

/*
 * "pfig UUID": the object opened with 基本図形編集 as a process
 * (application/fig, its program object's record 1) whatever the program
 * object says, so that a test works the process while the program object
 * still opens figures with the desktop's own figure editor, and both in
 * one run; "pdoc UUID" likewise with 基本文章編集 (application/doc)
 */
/*
 * "big" (the large object made last) or "bigN" (made N before it) at the
 * start of us, standing alone: that object in *u and TRUE
 */
LOCAL BOOL big_of( CONST char *us, TS_UUID *u )
{
	INT	n = 0;

	if ( !ut_big_have || us[0] != 'b' || us[1] != 'i' || us[2] != 'g' ) {
		return FALSE;
	}
	if ( us[3] >= '1' && us[3] <= '9' ) {
		n = us[3] - '0';
		us++;
	}
	if ( us[3] != 0 && us[3] != ' ' ) {
		return FALSE;
	}
	if ( n == 0 ) {
		*u = ut_big;
		return TRUE;
	}
	if ( n > ut_big_nwas ) {
		return FALSE;
	}
	*u = ut_big_was[n - 1];
	return TRUE;
}

LOCAL void pfig( CONST char *prog, CONST char *us )
{
	CONST DTPROG	*p = dt_prog_find((CONST UB *)prog);
	DTPROG		*q = (DTPROG *)Kmalloc(sizeof(DTPROG));
	T_VOBJ		*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	ER		er = E_NOEXS;

	if ( q == NULL || v == NULL ) {
		if ( q != NULL ) Kfree(q);
		if ( v != NULL ) Kfree(v);
		say("err no memory\n");
		return;
	}
	knl_memset(v, 0, sizeof(*v));
	v->recno = -1;
	if ( big_of(us, &v->target) ) {
		er = E_OK;
	} else {
		er = ts_str_to_uuid(us, &v->target);
	}
	if ( p == NULL ) er = E_NOEXS;
	if ( er >= E_OK ) {
		*q = *p;
		q->builtin = FALSE;
		er = dt_prog_run(NULL, q, v);
	}
	Kfree(q);
	Kfree(v);
	say(( er >= E_OK ) ? "ok\n" : "err not started\n");
}

/*
 * "figproc 1": from then on every figure that would open in the
 * desktop's own figure editor opens with 基本図形編集 as a process
 * instead, from wherever it is opened (a link of a window, 実行, open),
 * so that the tests of the figure editor run as they are on the process;
 * "figproc 0" puts it back. The program object is not changed.
 */
LOCAL BOOL	ut_fig_proc = FALSE;

/*
 * "figproc 1": figures opened by the process even while the program
 * object says the desktop's figure editor; "figproc kernel": by the
 * desktop's even while it says the process, so that the two can still
 * be compared once the program object has been switched; "figproc 0":
 * as the program object says. "docproc 1|kernel|0" does the same for
 * documents and 基本文章編集.
 */
LOCAL BOOL	ut_fig_kernel = FALSE;
LOCAL BOOL	ut_doc_proc = FALSE;
LOCAL BOOL	ut_doc_kernel = FALSE;
LOCAL BOOL	ut_cab_proc = FALSE;		/* cabproc, for 仮身一覧 */
LOCAL BOOL	ut_cab_kernel = FALSE;

LOCAL void figproc( BOOL *p_proc, BOOL *p_kernel, CONST char *s )
{
	*p_proc = (BOOL)( s[0] == '1' );
	*p_kernel = (BOOL)( s[0] == 'k' );
	say("ok\n");
}

/* The program is one of those whose two editors figproc (fig), docproc (doc) or cabproc (cab) switches */
LOCAL BOOL ut_switched( CONST DTPROG *p, BOOL fig, BOOL doc, BOOL cab )
{
	if ( p == NULL ) {
		return FALSE;
	}
	return (BOOL)( ( fig && knl_strcmp((CONST char *)p->id, "basic-figure-editor") == 0 )
		       || ( doc && knl_strcmp((CONST char *)p->id, "basic-text-editor") == 0 )
		       || ( cab && knl_strcmp((CONST char *)p->id, "virtual-object-list") == 0 ) );
}

EXPORT BOOL dt_ut_as_kernel( CONST DTPROG *p )
{
	return (BOOL)( p != NULL && !p->builtin && ut_switched(p, ut_fig_kernel, ut_doc_kernel, ut_cab_kernel) );
}

EXPORT BOOL dt_ut_as_process( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v )
{
	DTPROG	*q;
	ER	er;

	if ( p == NULL || !p->builtin || !ut_switched(p, ut_fig_proc, ut_doc_proc, ut_cab_proc) ) {
		return FALSE;
	}
	q = (DTPROG *)Kmalloc(sizeof(DTPROG));
	if ( q == NULL ) {
		return FALSE;
	}
	*q = *p;
	q->builtin = FALSE;
	er = dt_prog_run(from, q, v);
	Kfree(q);
	return (BOOL)( er >= E_OK );
}

/*
 * "linkbig X Y": a link to the large object made last put in the window
 * at X, Y of the screen, its corner there, as a link carried there is
 * put: what a test then opens from a link of a window of the desktop's
 */
LOCAL void linkbig( CONST char *s )
{
	T_VOBJ	*v;
	INT	x = 0, y = 0;
	BOOL	ok;

	if ( !ut_big_have || !num(&s, &x, FALSE) || !num(&s, &y, FALSE) ) {
		say("err linkbig X Y, after mkbig\n");
		return;
	}
	v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	if ( v == NULL ) {
		say("err no memory\n");
		return;
	}
	knl_memset(v, 0, sizeof(*v));
	v->target = ut_big;
	v->recno = -1;
	v->frcol = v->chcol = v->tbcol = v->bgcol = TAD_COL_NONE;
	v->disp = TAD_D_DEFAULT;
	v->zoom = 100;
	(void)om_store_name(&ut_big, v->name, TAD_NAME_MAX);
	v->right = tad_lnk_least_w(v) * 4;
	v->bottom = tad_lnk_closed_h(v);
	v->height = v->bottom;
	ok = ed_place_link(dt_win_of(wm_at(x, y)), x, y, v);
	Kfree(v);
	say(ok ? "ok\n" : "err no window there takes links\n");
}

LOCAL void open_obj( CONST char *us )
{
	T_DTREQ	*rq = (T_DTREQ *)Kmalloc(sizeof(T_DTREQ));
	TS_UUID	d;
	SZ	asz = 0;
	ID	k;
	ER	er;

	if ( rq == NULL ) {
		say("err no memory\n");
		return;
	}
	knl_memset(rq, 0, sizeof(*rq));
	rq->req = DT_RQ_OPEN;
	rq->recno = -1;
	if ( big_of(us, &rq->target) ) {
		er = E_OK;
	} else {
		er = ts_str_to_uuid(us, &rq->target);
	}
	if ( er >= E_OK ) er = ob_fnd_nam((CONST UB *)DT_REQ_NAME, &d);
	k = ( er >= E_OK ) ? ob_opn_obj(&d, OB_OP_WRITE) : (ID)er;
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	Kfree(rq);
	say(( er >= E_OK ) ? "ok\n" : "err not opened\n");
}

/*
 * The object's 機能付箋 written anew: "apps UUID|big id id ...", the program of
 * each id under its id's name, the first the one it opens with. An object
 * with as many programs as a test wants, without 付箋指定's panel.
 */
LOCAL void set_apps( CONST char *s )
{
	CONST UB	*ids[UT_LINE / 2];
	char		line[UT_LINE];
	TS_UUID		u;
	INT		n = 0, i = 0, k = 0;
	ER		er;

	s = skip(s);
	while ( s[i] != 0 && i < UT_LINE - 1 ) {
		line[i] = s[i];
		i++;
	}
	line[i] = 0;
	while ( line[k] != 0 && line[k] != ' ' ) k++;
	if ( line[k] == 0 ) {
		say("err no programs\n");
		return;
	}
	line[k++] = 0;
	if ( big_of(line, &u) ) {
		er = E_OK;
	} else {
		er = ts_str_to_uuid(line, &u);
	}
	while ( er >= E_OK && line[k] != 0 ) {
		ids[n++] = (CONST UB *)&line[k];
		while ( line[k] != 0 && line[k] != ' ' ) k++;
		if ( line[k] != 0 ) line[k++] = 0;
	}
	if ( er >= E_OK ) er = om_store_set_apps(&u, ids, ids, n, 0);
	say(( er >= E_OK ) ? "ok\n" : "err not set\n");
}

/* A program file started as a process, with up to eight numbers as its argument */
LOCAL void run_prog( CONST char *s )
{
	T_CPRC	cprc;
	UW	arg[8];
	char	path[UT_LINE], b[24];
	INT	n = 0, k = 0, v;
	ID	pid;

	s = skip(s);
	while ( *s != 0 && *s != ' ' && *s != '\t' && n < UT_LINE - 1 ) {
		path[n++] = *s++;
	}
	path[n] = 0;
	knl_memset(arg, 0, sizeof(arg));
	for ( n = 0; n < 8 && num(&s, &v, FALSE); n++ ) {
		arg[n] = (UW)v;
	}
	knl_memset(&cprc, 0, sizeof(cprc));
	cprc.arg = arg;
	cprc.argsz = sizeof(arg);
	pid = ts_cre_prc(path, &cprc);
	if ( pid <= 0 ) {
		say("err not started\n");
		return;
	}
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, pid);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* ---------------------------------------------------------------- how the pointer keeps up */

#define LAG_STEP_MS	50		/* between two moves of the probe */
#define LAG_POLL_MS	5		/* how often it looks whether the screen shows the move */
#define LAG_LATE_MS	200		/* a move shown later than this is counted late */
#define LAG_GIVEUP_MS	120000		/* a move not shown by then is not waited for */

LOCAL UW ut_now( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

/*
 * For 'ms' milliseconds, the pointer moved to and fro every LAG_STEP_MS,
 * and each time, how long the screen took to show it at the place it
 * was sent to (wm_pointer_last): "ok WORST MOVES LATE", the longest
 * wait in ms, the moves made, and how many were shown later than
 * LAG_LATE_MS. A move is not made before the last one is shown, so a
 * screen that stood still for a second shows as one move of a second.
 */
LOCAL void lag( INT ms )
{
	T_DPRECT	to;
	char		b[48];
	INT		x0 = ut_x, y0 = ut_y, x, lx, ly, side, n = 0, late = 0, k = 0;
	UW		start = ut_now(), sent, waited, worst = 0;

	while ( (INT)( ut_now() - start ) < ms ) {
		x = ( ( n & 1 ) != 0 ) ? x0 + 24 : x0;
		move(x, y0);
		sent = ut_now();
		wm_pointer_box(x, y0, &to);
		for ( ;; ) {
			waited = ut_now() - sent;
			if ( ( wm_pointer_last(&lx, &ly, &side) && lx == to.left && ly == to.top )
			  || waited >= LAG_GIVEUP_MS ) {
				break;
			}
			tk_dly_tsk(LAG_POLL_MS);
		}
		if ( waited > worst ) worst = waited;
		if ( waited > LAG_LATE_MS ) late++;
		n++;
		if ( waited < LAG_STEP_MS ) tk_dly_tsk((RELTIM)( LAG_STEP_MS - waited ));
	}
	move(x0, y0);
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, (INT)worst);  b[k++] = ' ';
	put_num(b, &k, n);  b[k++] = ' ';
	put_num(b, &k, late);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/*
 * The counts of the window manager and the screen since the last time
 * they were asked for
 */
LOCAL T_WMSTAT		ut_wm_was;
LOCAL T_DISPSTAT	ut_disp_was;

LOCAL void ut_stat( void )
{
	T_WMSTAT	w;
	T_DISPSTAT	d;
	char		b[160];
	INT		k = 0;

	knl_memset(&w, 0, sizeof(w));
	knl_memset(&d, 0, sizeof(d));
	(void)wm_stat(&w);
	(void)ts_disp_stat(&d);
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, (INT)( w.composites - ut_wm_was.composites ));  b[k++] = ' ';
	put_num(b, &k, (INT)( w.parts - ut_wm_was.parts ));  b[k++] = ' ';
	put_num(b, &k, (INT)( ( w.pixels - ut_wm_was.pixels ) / 1000 ));  b[k++] = ' ';
	put_num(b, &k, (INT)( w.ptr_moves - ut_wm_was.ptr_moves ));  b[k++] = ' ';
	put_num(b, &k, (INT)( w.ptr_built - ut_wm_was.ptr_built ));  b[k++] = ' ';
	put_num(b, &k, (INT)( d.rects - ut_disp_was.rects ));  b[k++] = ' ';
	put_num(b, &k, (INT)( ( d.pixels - ut_disp_was.pixels ) / 1000 ));
	b[k++] = '\n';
	b[k] = 0;
	ut_wm_was = w;
	ut_disp_was = d;
	say(b);
}

/*
 * The calls of processes, the takes of the process table's lock and
 * those of them that waited, since the last time they were asked for
 */
IMPORT volatile UD	knl_svc_calls;
IMPORT void knl_prc_lock_stat( UD *p_takes, UD *p_waits );
LOCAL UD	ut_svc_was[3];

LOCAL void svc_stat( void )
{
	UD	v[3];
	char	b[96];
	INT	k = 0, i;

	v[0] = knl_svc_calls;
	knl_prc_lock_stat(&v[1], &v[2]);
	b[k++] = 'o';  b[k++] = 'k';
	for ( i = 0; i < 3; i++ ) {
		b[k++] = ' ';
		put_num(b, &k, (INT)( v[i] - ut_svc_was[i] ));
		ut_svc_was[i] = v[i];
	}
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/*
 * What the tasks are doing now: how many run, are ready, and wait for
 * each kind of thing, and the three objects most waited on with how
 * many wait on each ("m12x30": mutex 12, thirty tasks)
 */
#define UT_WAIT_TOP	3

LOCAL void waits_say( void )
{
	T_RTSK	r;
	INT	run = 0, rdy = 0, dly = 0, slp = 0, sem = 0, flg = 0, mtx = 0, mbf = 0, oth = 0, sus = 0;
	INT	topid[UT_WAIT_TOP], topn[UT_WAIT_TOP], topk[UT_WAIT_TOP];
	INT	tid, i, j, k = 0, n;
	ID	ids[64];
	INT	idn[64], idk[64], nid = 0;
	char	b[200];

	for ( tid = 1; tid <= CNF_MAX_TSKID; tid++ ) {
		INT	kind = 0;

		if ( tk_ref_tsk(tid, &r) < E_OK ) continue;
		if ( ( r.tskstat & TTS_RUN ) != 0 ) run++;
		else if ( ( r.tskstat & TTS_RDY ) != 0 ) rdy++;
		else if ( ( r.tskstat & TTS_WAI ) != 0 ) {
			if ( r.tskwait & TTW_DLY ) dly++;
			else if ( r.tskwait & TTW_SLP ) slp++;
			else if ( r.tskwait & TTW_SEM ) { sem++; kind = 's'; }
			else if ( r.tskwait & TTW_FLG ) { flg++; kind = 'f'; }
			else if ( r.tskwait & TTW_MTX ) { mtx++; kind = 'm'; }
			else if ( r.tskwait & ( TTW_SMBF | TTW_RMBF ) ) { mbf++; kind = 'b'; }
			else oth++;
		} else if ( ( r.tskstat & TTS_SUS ) != 0 ) sus++;
		if ( kind != 0 && r.wid > 0 ) {
			for ( i = 0; i < nid && !( ids[i] == r.wid && idk[i] == kind ); i++ ) ;
			if ( i == nid && nid < 64 ) { ids[nid] = r.wid; idk[nid] = kind; idn[nid] = 0; nid++; }
			if ( i < nid ) idn[i]++;
		}
	}
	for ( j = 0; j < UT_WAIT_TOP; j++ ) {
		topid[j] = 0; topn[j] = 0; topk[j] = 0;
		for ( i = 0; i < nid; i++ ) {
			if ( idn[i] > topn[j] ) { topn[j] = idn[i]; topid[j] = ids[i]; topk[j] = idk[i]; n = i; }
		}
		if ( topn[j] > 0 ) idn[n] = 0;
	}
	b[k++] = 'o';  b[k++] = 'k';
	{
		CONST char	*lab[10] = { " run ", " rdy ", " dly ", " slp ", " sem ", " flg ", " mtx ", " mbf ", " oth ", " sus " };
		INT		v[10];

		v[0] = run; v[1] = rdy; v[2] = dly; v[3] = slp; v[4] = sem; v[5] = flg; v[6] = mtx; v[7] = mbf; v[8] = oth; v[9] = sus;
		for ( i = 0; i < 10; i++ ) {
			for ( j = 0; lab[i][j] != 0; j++ ) b[k++] = lab[i][j];
			put_num(b, &k, v[i]);
		}
	}
	for ( j = 0; j < UT_WAIT_TOP && topn[j] > 0; j++ ) {
		b[k++] = ' ';
		b[k++] = (char)topk[j];
		put_num(b, &k, topid[j]);
		b[k++] = 'x';
		put_num(b, &k, topn[j]);
	}
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* xmlTAD gathered a line at a time for xmlopen */
LOCAL UB	*ut_xml = NULL;
LOCAL SZ	ut_xml_n = 0, ut_xml_max = 0;

LOCAL void xml_keep( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != 0 ) n++;
	if ( ut_xml_n + n + 2 > ut_xml_max ) {
		SZ	m = ( ut_xml_max > 0 ) ? ut_xml_max * 2 : 4096;
		UB	*b;

		while ( m < ut_xml_n + n + 2 ) m *= 2;
		b = (UB *)Kmalloc(m);
		if ( b == NULL ) {
			say("err no memory\n");
			return;
		}
		if ( ut_xml != NULL ) {
			knl_memcpy(b, ut_xml, ut_xml_n);
			Kfree(ut_xml);
		}
		ut_xml = b;
		ut_xml_max = m;
	}
	knl_memcpy(ut_xml + ut_xml_n, s, n);
	ut_xml_n += n;
	ut_xml[ut_xml_n++] = '\n';
	say("ok\n");
}

/*
 * The window is opened by the desktop's own task, in its loop
 * (dt_uitest_poll): the drawing is the desktop's, and its task has the
 * stack for it. This task waits for it.
 */
LOCAL T_TAD	*ut_open_rec = NULL;
LOCAL char	ut_open_title[UT_LINE];
LOCAL volatile INT ut_open_state = 0;	/* 1 asked, 2 opened, 3 not */

/*
 * The desktop's loop asked for input and none was there. A sync is
 * answered by the first such answer after the loop has seen the ask
 * (dt_uitest_poll, before it reads the input): the keys put in before
 * the ask are read and done by then. Coming round the loop is not
 * enough, nor is an answer the loop had before it saw the ask (it may
 * lay paragraphs for a while after one, with keys come meanwhile).
 */
/*
 * "redraw": the window typed into drawn again whole, by the desktop's
 * task in its loop -- one of the desktop's drawn and built again, one of
 * a process's told to draw itself again (OB_E_REDRAW, as when its size
 * changes) -- for a test to compare with what a part drawn again left.
 */
IMPORT void knl_wmobj_moved( INT wid, BOOL resized );

LOCAL volatile INT ut_redraw_state = 0;	/* 1 asked, 2 done */

LOCAL void redraw_focused( void )
{
	INT		wid = wm_focused();
	DTWIN		*d = dt_win_of(wid);
	T_WMWIN		w;
	T_DPRECT	all;

	if ( d == NULL ) {
		knl_wmobj_moved(wid, TRUE);
		return;
	}
	dt_draw(d);
	if ( wm_ref(d->wid, &w) >= E_OK ) {
		all.left = 0;
		all.top = 0;
		all.right = w.work.right - w.work.left;
		all.bottom = w.work.bottom - w.work.top;
		(void)wm_damage(d->wid, &all);
	}
	wm_update();
}

LOCAL void redraw_ask( void )
{
	UW	start = ut_now();

	ut_redraw_state = 1;
	while ( ut_redraw_state == 1 && (INT)( ut_now() - start ) < 10000 ) {
		tk_dly_tsk(10);
	}
	if ( ut_redraw_state != 2 ) {
		ut_redraw_state = 0;
		say("err desktop busy\n");
		return;
	}
	ut_redraw_state = 0;
	say("ok\n");
}

EXPORT void dt_uitest_idle( void )
{
	if ( ut_sync == 3 ) {
		ut_sync = 2;
	}
}

EXPORT void dt_uitest_poll( void )
{
	T_DPRECT	o;
	DTWIN		*d;

	if ( ut_sync == 1 ) {
		ut_sync = 3;
	}
	sysup_poll();
	if ( ut_prog_state == 1 ) {
		UINT	how = 0;

		/* a panel keeps the loop's events going in here: not started twice */
		ut_prog_state = 3;
		ut_prog_er = ( ut_prog_what == 0 ) ? dt_prog_remove(&ut_prog_u)
			   : dt_pkg_install(&ut_prog_u, ut_prog_force, ut_prog_pid, NULL, NULL, &how);
		ut_prog_state = 2;
	}
	if ( ut_redraw_state == 1 ) {
		redraw_focused();
		ut_redraw_state = 2;
	}
	if ( ut_open_state != 1 ) {
		return;
	}
	o.left = 100;  o.top = 60;  o.right = 600;  o.bottom = 460;
	d = dt_open_made(ut_open_rec, ut_open_title, DT_TOOL_NONE, &o);
	ut_open_rec = NULL;
	if ( d != NULL ) {
		d->sealed = FALSE;
		dt_draw(d);
	}
	ut_open_state = ( d != NULL ) ? 2 : 3;
}

LOCAL void xml_open( CONST char *title )
{
	T_TAD	*rec = NULL;
	INT	i, tries;

	if ( ut_xml == NULL || tad_parse(ut_xml, ut_xml_n, NULL, &rec) < E_OK ) {
		ut_xml_n = 0;
		say("err not xmlTAD\n");
		return;
	}
	ut_xml_n = 0;
	for ( i = 0; title[i] != 0 && i < UT_LINE - 1; i++ ) {
		ut_open_title[i] = title[i];
	}
	ut_open_title[i] = 0;
	ut_open_rec = rec;
	ut_open_state = 1;
	for ( tries = 0; tries < 100 && ut_open_state == 1; tries++ ) {
		tk_dly_tsk(50);
	}
	say(( ut_open_state == 2 ) ? "ok\n" : "err not opened\n");
	ut_open_state = 0;
}

/* "xmlbig": record 0 of the large object made last replaced by the xmlTAD kept, and that forgotten */
LOCAL void xml_big( void )
{
	T_TAD	*rec = NULL;
	ID	k;
	ER	er;

	if ( !ut_big_have || ut_xml == NULL || tad_parse(ut_xml, ut_xml_n, NULL, &rec) < E_OK ) {
		ut_xml_n = 0;
		say("err not xmlTAD, or no mkbig\n");
		return;
	}
	tad_free(rec);
	{
		/* ffffffff-ffff-ffff-ffff-ffffffffffff stands for the object itself: a link to itself */
		CONST char	*self = "ffffffff-ffff-ffff-ffff-ffffffffffff";
		char		us[TS_UUID_STRLEN + 1];
		SZ		i, j;

		ts_uuid_text(&ut_big, us);
		for ( i = 0; i + 36 <= ut_xml_n; i++ ) {
			for ( j = 0; j < 36 && ut_xml[i + j] == (UB)self[j]; j++ ) ;
			if ( j < 36 ) continue;
			for ( j = 0; j < 36; j++ ) ut_xml[i + j] = (UB)us[j];
		}
	}
	k = ob_opn_obj(&ut_big, OB_OP_R | OB_OP_W);
	er = ( k > 0 ) ? ob_rpl_rec(k, 0, ut_xml, ut_xml_n) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	if ( er >= E_OK ) om_store_forget(&ut_big);
	ut_xml_n = 0;
	say(( er >= E_OK ) ? "ok\n" : "err not written\n");
}

/* The caret of the document in front, or of the text being edited in the figure in front */
/* How many shapes the figure in front has, and how many of them are picked */
LOCAL void shapes_say( void )
{
	DTWIN	*d = dt_win_of(wm_focused());
	char	b[48];
	INT	k = 0;

	tk_dly_tsk(200);			/* what was put in before taken first */
	d = dt_win_of(wm_focused());
	if ( d == NULL || d->fig == NULL ) {
		say("err no figure\n");
		return;
	}
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, d->fig->nsh);
	b[k++] = ' ';
	put_num(b, &k, d->npick);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

LOCAL void caret_say( void )
{
	DTWIN	*d;
	char	b[64];
	INT	k = 0, same = 0, tries, was[4] = { -1, -1, -1, -1 };

	/* the keys put in before are taken first: asked until it stays the same */
	for ( tries = 0; tries < 100 && same < 3; tries++ ) {
		tk_dly_tsk(100);
		d = dt_win_of(wm_focused());
		if ( d != NULL && d->tb != NULL ) d = d->tb;
		if ( d == NULL || d->doc == NULL ) break;
		if ( d->dd.cpara == was[0] && d->dd.cpos == was[1] && d->dd.apara == was[2] && d->dd.apos == was[3] ) {
			same++;
		} else {
			same = 0;
			was[0] = d->dd.cpara;  was[1] = d->dd.cpos;  was[2] = d->dd.apara;  was[3] = d->dd.apos;
		}
	}
	d = dt_win_of(wm_focused());
	if ( d != NULL && d->tb != NULL ) {
		d = d->tb;
	}
	if ( d == NULL || d->doc == NULL ) {
		say("err no document\n");
		return;
	}
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';  b[k++] = 'c';  b[k++] = ' ';
	put_num(b, &k, d->dd.cpara);
	b[k++] = ',';
	put_num(b, &k, d->dd.cpos);
	b[k++] = ' ';  b[k++] = 'a';  b[k++] = ' ';
	put_num(b, &k, d->dd.apara);
	b[k++] = ',';
	put_num(b, &k, d->dd.apos);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* ---------------------------------------------------------------- objects of size */

/* The templates the large objects are copies of: 基本文章編集, 画用紙, 基本表計算 */
LOCAL CONST char *CONST ut_tpl[3] = {
	"019a1132-762b-7b02-ba2a-a918a9b37c39",
	"019a0f91-066f-706f-93da-ffe8150d6207",
	"019b54a4-6ccf-72f7-e320-c98e4a0bab4e"
};

/* What is written, a buffer at a time */
typedef struct {
	ID	key;
	UB	*buf;
	SZ	n, max, at;
	ER	er;
} UTOUT;

LOCAL void out_flush( UTOUT *o )
{
	SZ	asz = 0;
	ER	er;

	if ( o->n == 0 || o->er < E_OK ) {
		o->n = 0;
		return;
	}
	er = ob_wri_rec(o->key, 0, (D)o->at, o->buf, o->n, &asz);
	if ( er < E_OK || asz != o->n ) {
		o->er = ( er < E_OK ) ? er : E_IO;
	}
	o->at += o->n;
	o->n = 0;
}

LOCAL void out_str( UTOUT *o, CONST char *s )
{
	for ( ; *s != 0; s++ ) {
		if ( o->n >= o->max ) {
			out_flush(o);
		}
		o->buf[o->n++] = (UB)*s;
	}
}

LOCAL void out_num( UTOUT *o, INT v )
{
	char	b[16];
	INT	k = 0;

	put_num(b, &k, v);
	b[k] = 0;
	out_str(o, b);
}

/* "A", "Z", "AA": a column's letters */
LOCAL void out_col( UTOUT *o, INT col )
{
	char	b[8];
	INT	k = 0, i;

	col++;
	while ( col > 0 && k < 7 ) {
		b[k++] = (char)( 'A' + ( col - 1 ) % 26 );
		col = ( col - 1 ) / 26;
	}
	for ( i = 0; i < k / 2; i++ ) {
		char	t = b[i];

		b[i] = b[k - 1 - i];
		b[k - 1 - i] = t;
	}
	b[k] = 0;
	out_str(o, b);
}

LOCAL void big_doc( UTOUT *o, INT n )
{
	INT	i;

	out_str(o, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<document>\n<text lang=\"0\" bpat=\"0\"/>\n");
	for ( i = 0; i < n && o->er >= E_OK; i++ ) {
		out_str(o, "<p>段落");
		out_num(o, i);
		out_str(o, " あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん");
		out_str(o, "ABCDEFGHIJKLMNOPQRSTUVWXYZ</p>\n");
	}
	out_str(o, "</document>\n</tad>\n");
}

LOCAL void big_fig( UTOUT *o, INT n, INT m, INT t, BOOL vert )
{
	INT	i, k;

	out_str(o, "<tad version=\"1.0\" encoding=\"UTF-8\">\n<figure>\n");
	for ( i = 0; i < n && o->er >= E_OK; i++ ) {
		INT	x = ( i % 100 ) * 12, y = ( i / 100 ) * 12;

		out_str(o, "<rect round=\"0\" lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"0\" angle=\"0\" left=\"");
		out_num(o, x);
		out_str(o, "\" top=\"");
		out_num(o, y);
		out_str(o, "\" right=\"");
		out_num(o, x + 8);
		out_str(o, "\" bottom=\"");
		out_num(o, y + 8);
		out_str(o, "\" zIndex=\"");
		out_num(o, i);
		out_str(o, "\"/>\n");
	}
	/* two lines of many points: a zigzag across under the rectangles */
	for ( k = 0; k < 2 && m > 1; k++ ) {
		out_str(o, "<polyline lineType=\"0\" lineWidth=\"1\" l_pat=\"1\" f_pat=\"0\" points=\"");
		for ( i = 0; i < m && o->er >= E_OK; i++ ) {
			if ( i > 0 ) out_str(o, " ");
			out_num(o, i * 1200 / m);
			out_str(o, ",");
			out_num(o, ( n / 100 ) * 12 + 20 + k * 40 + ( ( i & 1 ) ? 30 : 0 ));
		}
		out_str(o, "\" zIndex=\"");
		out_num(o, n + k);
		out_str(o, "\"/>\n");
	}
	/* pieces of text, each in its box */
	for ( k = 0; k < t && o->er >= E_OK; k++ ) {
		INT	l = 100 + k * 150, tp = ( n / 100 ) * 12 + 120;
		INT	bw = vert ? 40 : 120, bh = vert ? 120 : 40;

		out_str(o, "<document><docView viewleft=\"");
		out_num(o, l);
		out_str(o, "\" viewtop=\"");
		out_num(o, tp);
		out_str(o, "\" viewright=\"");
		out_num(o, l + bw);
		out_str(o, "\" viewbottom=\"");
		out_num(o, tp + bh);
		out_str(o, "\"/><docDraw drawleft=\"");
		out_num(o, l);
		out_str(o, "\" drawtop=\"");
		out_num(o, tp);
		out_str(o, "\" drawright=\"");
		out_num(o, l + bw);
		out_str(o, "\" drawbottom=\"");
		out_num(o, tp + bh);
		out_str(o, vert ? "\"/><docScale hunit=\"-72\" vunit=\"-72\"/><text lang=\"0\" bpat=\"0\" direction=\"2\" zIndex=\""
				: "\"/><docScale hunit=\"-72\" vunit=\"-72\"/><text lang=\"0\" bpat=\"0\" zIndex=\"");
		out_num(o, n + 2 + k);
		out_str(o, "\"/><font size=\"14\" face=\"sans-serif\" color=\"#000000\"/><p>ABC</p></document>\n");
	}
	out_str(o, "</figure>\n</tad>\n");
}

LOCAL void big_calc( UTOUT *o, INT rows, INT cols )
{
	INT	r, c;

	out_str(o, "<tad version=\"1.0\" filename=\"基本表計算\" encoding=\"UTF-8\">\n<document>\n");
	for ( r = 0; r < rows && o->er >= E_OK; r++ ) {
		out_str(o, "<p>");
		for ( c = 0; c < cols; c++ ) {
			if ( c > 0 ) out_str(o, "\t");
			out_str(o, "<calcPos cell=\"");
			out_col(o, c);
			out_num(o, r + 1);
			out_str(o, "\"/>");
			if ( c == cols - 1 && cols > 2 ) {
				/* the last column a formula of the first two */
				out_str(o, "=A");
				out_num(o, r + 1);
				out_str(o, "+B");
				out_num(o, r + 1);
			} else {
				out_num(o, r * cols + c);
			}
		}
		out_str(o, "</p>\n");
	}
	out_str(o, "</document>\n</tad>\n");
}

/*
 * The palettes of the 道具パネル as the panels of a data box would
 * define them, laid out by the figure editor's code with the system's
 * letters (figtool.c): what the process's figure editor's data box
 * (SYSDEF_BOX_FIG) holds, written out once to be put there. Each line
 * is said as "ftdef: ..." on the console.
 */
LOCAL void ftdef_rect( UB *b, INT *k, CONST char *name, INT l, INT t, INT r, INT bt )
{
	CONST char	*p;
	INT		v[4], i;

	v[0] = l;  v[1] = t;  v[2] = r;  v[3] = bt;
	b[( *k )++] = ' ';
	for ( p = name; *p != 0; p++ ) b[( *k )++] = (UB)*p;
	b[( *k )++] = '=';
	b[( *k )++] = '"';
	for ( i = 0; i < 4; i++ ) {
		*k = ft_put_num(b, *k, v[i]);
		if ( i < 3 ) b[( *k )++] = ',';
	}
	b[( *k )++] = '"';
	b[*k] = 0;
}

LOCAL void ftdef_str( UB *b, INT *k, CONST char *s )
{
	while ( *s != 0 ) b[( *k )++] = (UB)*s++;
	b[*k] = 0;
}

LOCAL void ftdef_num( UB *b, INT *k, CONST char *name, INT v )
{
	ftdef_str(b, k, " ");
	ftdef_str(b, k, name);
	ftdef_str(b, k, "=\"");
	*k = ft_put_num(b, *k, v);
	ftdef_str(b, k, "\"");
}

LOCAL void ftdef_say( UB *b, INT *k )
{
	tm_printf((UB *)"ftdef: %s\n", b);
	*k = 0;
	b[0] = 0;
}

LOCAL void ftdef( void )
{
	static CONST INT	which[] = { FT_PAL_FILL, FT_PAL_STROKE, FT_PAL_LSTYLE, FT_PAL_CORNER,
					    FT_PAL_PAINT, FT_PAL_ZOOM, FT_PAL_GRID, FT_PAL_FONT };
	T_FTPOP		p;
	T_DPRECT	c;
	UB		b[512];
	INT		w, i, j, k = 0, y;

	b[0] = 0;
	for ( w = 0; w < (INT)( sizeof(which) / sizeof(which[0]) ); w++ ) {
		if ( !ft_palette(which[w], &p) ) continue;
		ftdef_str(b, &k, "<panel kind=\"bare\"");
		ftdef_num(b, &k, "num", 10 + which[w]);
		ftdef_rect(b, &k, "rect", 0, 0, FT_POP_W, ft_pop_height(&p));
		ftdef_str(b, &k, " title=\"\">");
		ftdef_say(b, &k);
		for ( i = 0; i < p.n; i++ ) {
			T_FTITEM	*it = &p.it[i];

			y = it->r.top;
			if ( it->label != NULL && it->label[0] != 0 && it->type != FT_PI_CHECK
			  && it->type != FT_PI_BUTTON ) {
				/*
				 * where the desktop's label part (8 from the left, its row less
				 * 2 above and 4 below) puts its words: four in from its left,
				 * the system's letters in its middle
				 */
				ftdef_str(b, &k, "<text");
				ftdef_num(b, &k, "x", 8 + 4);
				ftdef_num(b, &k, "y", y + FT_ROW_H - 7);
				ftdef_str(b, &k, ">");
				ftdef_str(b, &k, it->label);
				ftdef_str(b, &k, "</text>");
				ftdef_say(b, &k);
			}
			switch ( it->type ) {
			case FT_PI_CHOICE:
				for ( j = 0; j < it->n; j++ ) {
					(void)ft_choice_at(it, j, &c);
					ftdef_str(b, &k, "<choice");
					ftdef_num(b, &k, "num", FT_PN_ITEM(i, j));
					ftdef_rect(b, &k, "rect", c.left, c.top, c.right, c.bottom);
					ftdef_num(b, &k, "group", i + 1);
					ftdef_str(b, &k, ">");
					ftdef_str(b, &k, it->names[j]);
					ftdef_str(b, &k, "</choice>");
					ftdef_say(b, &k);
				}
				break;
			case FT_PI_CHECK:
				ftdef_str(b, &k, "<switch");
				ftdef_num(b, &k, "num", FT_PN_ITEM(i, 0));
				ftdef_rect(b, &k, "rect", 8, y + 1, FT_POP_W - 8, y + FT_ROW_H - 3);
				ftdef_str(b, &k, ">");
				ftdef_str(b, &k, it->label);
				ftdef_str(b, &k, "</switch>");
				ftdef_say(b, &k);
				break;
			case FT_PI_TEXT:
				ftdef_str(b, &k, "<box");
				ftdef_num(b, &k, "num", FT_PN_ITEM(i, 0));
				ftdef_rect(b, &k, "rect", FT_LABEL_W, y + 1, FT_LABEL_W + 110, y + FT_ROW_H - 3);
				ftdef_num(b, &k, "max", it->max - 1);
				ftdef_str(b, &k, " ascii=\"true\"></box>");
				ftdef_say(b, &k);
				break;
			case FT_PI_COLOUR:
				ftdef_str(b, &k, "<swatch");
				ftdef_num(b, &k, "num", FT_PN_ITEM(i, 0));
				ftdef_rect(b, &k, "rect", FT_LABEL_W, y + 1, FT_LABEL_W + 110, y + FT_ROW_H - 3);
				ftdef_str(b, &k, "/>");
				ftdef_say(b, &k);
				break;
			case FT_PI_SWATCH:
				for ( j = 0; j < it->n; j++ ) {
					ftdef_str(b, &k, "<swatch");
					ftdef_num(b, &k, "num", FT_PN_ITEM(i, j));
					ftdef_rect(b, &k, "rect", FT_LABEL_W + j * 20, y + 3,
						   FT_LABEL_W + j * 20 + 18, y + 21);
					ftdef_num(b, &k, "group", i + 1);
					ftdef_str(b, &k, "/>");
					ftdef_say(b, &k);
				}
				break;
			case FT_PI_BUTTON:
				ftdef_str(b, &k, "<button");
				ftdef_num(b, &k, "num", FT_PN_ITEM(i, 0));
				ftdef_rect(b, &k, "rect", 8, y + 1, 8 + ft_str_w(it->label, FT_PART_FH) + 24,
					   y + FT_ROW_H - 3);
				ftdef_str(b, &k, ">");
				ftdef_str(b, &k, it->label);
				ftdef_str(b, &k, "</button>");
				ftdef_say(b, &k);
				break;
			default:
				break;
			}
		}
		ftdef_str(b, &k, "</panel>");
		ftdef_say(b, &k);
	}
	say("ok\n");
}

LOCAL void make_big( CONST char *s )
{
	INT	kind, n = 0, m = 0, t = 0, v = 0, k = 0;
	TS_UUID	tpl, u;
	UTOUT	o;
	char	b[80];
	ER	er;

	s = skip(s);
	kind = ( s[0] == 'd' ) ? 0 : ( s[0] == 'f' ) ? 1 : ( s[0] == 'c' ) ? 2 : -1;
	while ( *s != 0 && *s != ' ' ) s++;
	if ( kind < 0 || !num(&s, &n, FALSE) ) {
		say("err mkbig doc|fig|calc N [M]\n");
		return;
	}
	(void)num(&s, &m, FALSE);
	(void)num(&s, &t, FALSE);
	(void)num(&s, &v, FALSE);
	er = ts_str_to_uuid(ut_tpl[kind], &tpl);
	/* the person's, as a copy made from a template on the desktop is, so that a program may write it */
	knl_ob_make_for_person(TRUE);
	if ( er >= E_OK ) er = ob_cpy_obj(&tpl, NULL, &u);
	knl_ob_make_for_person(FALSE);
	if ( er < E_OK ) {
		say("err no copy\n");
		return;
	}
	knl_memset(&o, 0, sizeof(o));
	o.max = 64 * 1024;
	o.buf = (UB *)Kmalloc(o.max);
	o.key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	if ( o.buf == NULL || o.key <= 0 ) {
		if ( o.buf != NULL ) Kfree(o.buf);
		if ( o.key > 0 ) ob_cls_obj(o.key);
		say("err not opened\n");
		return;
	}
	if ( kind == 0 )      big_doc(&o, n);
	else if ( kind == 1 ) big_fig(&o, n, m, t, (BOOL)( v != 0 ));
	else                  big_calc(&o, n, m);
	out_flush(&o);
	if ( o.er >= E_OK ) o.er = ob_trn_rec(o.key, 0, (UD)o.at);
	ob_cls_obj(o.key);
	Kfree(o.buf);
	if ( o.er < E_OK ) {
		say("err not written\n");
		return;
	}
	if ( ut_big_have ) {
		for ( k = UT_BIG_BACK - 1; k > 0; k-- ) {
			ut_big_was[k] = ut_big_was[k - 1];
		}
		ut_big_was[0] = ut_big;
		if ( ut_big_nwas < UT_BIG_BACK ) ut_big_nwas++;
	}
	k = 0;
	ut_big = u;
	ut_big_have = TRUE;
	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	(void)ts_uuid_to_str(&u, b + k, sizeof(b) - k);
	while ( b[k] != 0 ) k++;
	b[k++] = ' ';
	put_num(b, &k, (INT)o.at);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* Record 0 of an object, as long as it is and a hash of it */
/* Record 0 of an object read whole: its bytes (Kfree them) and how many, or NULL */
LOCAL UB *rec_whole_n( CONST TS_UUID *u, INT recno, SZ *p_len )
{
	ID	k;
	UB	*buf = NULL, *more;
	SZ	at = 0, cap = 0, asz = 0;
	ER	er;

	*p_len = 0;
	if ( ( k = ob_opn_obj(u, OB_OP_R) ) <= 0 ) {
		return NULL;
	}
	for ( ;; ) {
		if ( at + 64 * 1024 > cap ) {
			more = (UB *)Kmalloc(cap + 256 * 1024 + 1);
			if ( more == NULL ) {
				break;
			}
			if ( buf != NULL ) {
				knl_memcpy(more, buf, at);
				Kfree(buf);
			}
			buf = more;
			cap += 256 * 1024;
		}
		er = ob_rea_rec(k, recno, (D)at, buf + at, 64 * 1024, &asz);
		if ( er < E_OK || asz <= 0 ) {
			break;
		}
		at += asz;
	}
	ob_cls_obj(k);
	if ( buf != NULL ) {
		buf[at] = 0;
	}
	*p_len = at;
	return buf;
}

LOCAL UB *rec_whole( CONST TS_UUID *u, SZ *p_len )
{
	return rec_whole_n(u, 0, p_len);
}

/* The time a PDF file says it was made (/CreationDate (D:...)) made noughts: the same file but for when */
LOCAL void pdf_date_blank( UB *s, SZ n )
{
	CONST char	*key = "/CreationDate (D:";
	SZ		i, j, kl = 0;

	while ( key[kl] != 0 ) kl++;
	for ( i = 0; i + kl < n; i++ ) {
		for ( j = 0; j < kl && s[i + j] == (UB)key[j]; j++ ) ;
		if ( j < kl ) continue;
		for ( j = i + kl; j < n && s[j] >= '0' && s[j] <= '9'; j++ ) s[j] = '0';
		return;
	}
}

LOCAL BOOL hex_at( CONST UB *s, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		UB	c = s[i];

		if ( !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ) ) ) {
			return FALSE;
		}
	}
	return TRUE;
}

/* Every UUID written in the text (8-4-4-4-12) made all noughts: what is the same but for new identities */
LOCAL void uuids_blank( UB *s, SZ n )
{
	SZ	i, j;

	for ( i = 0; i + 36 <= n; i++ ) {
		if ( s[i + 8] == '-' && s[i + 13] == '-' && s[i + 18] == '-' && s[i + 23] == '-'
		  && hex_at(s + i, 8) && hex_at(s + i + 9, 4) && hex_at(s + i + 14, 4)
		  && hex_at(s + i + 19, 4) && hex_at(s + i + 24, 12) ) {
			for ( j = 0; j < 36; j++ ) {
				if ( s[i + j] != '-' ) s[i + j] = '0';
			}
			i += 35;
		}
	}
}

/* The object the last link of the record of u points at */
LOCAL BOOL last_link( CONST TS_UUID *u, TS_UUID *p_to )
{
	UB	*x;
	SZ	n = 0;
	T_TAD	*doc = NULL;
	T_VOBJ	v;
	INT	c;
	BOOL	got = FALSE;

	x = rec_whole(u, &n);
	if ( x == NULL ) {
		return FALSE;
	}
	if ( tad_parse(x, n, NULL, &doc) >= E_OK && doc != NULL ) {
		c = tad_lnk_count(doc);
		if ( c > 0 && tad_lnk_get(doc, c - 1, &v) >= E_OK ) {
			*p_to = v.target;
			got = TRUE;
		}
		tad_free(doc);
	}
	Kfree(x);
	return got;
}

LOCAL void sum_bytes( UD *h, CONST UB *buf, SZ n, BOOL show )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		*h ^= buf[i];
		*h *= 0x100000001B3ULL;
	}
	for ( i = 0; show && i < n; i += 200 ) {
		UB	part[201];
		INT	j;

		for ( j = 0; j < 200 && i + j < n; j++ ) {
			part[j] = ( buf[i + j] == 0x0A ) ? (UB)' ' : buf[i + j];
		}
		part[j] = 0;
		tm_printf((UB *)"rec: %s\n", part);
	}
}

LOCAL void rec_sum( CONST char *us )
{
	TS_UUID	u, to;
	ID	k;
	UB	*buf;
	SZ	at = 0, asz = 0;
	UD	h = 0xCBF29CE484222325ULL;
	char	b[64 + TAD_NAME_MAX];
	UB	nm[TAD_NAME_MAX];
	INT	n = 0, i;
	ER	er = E_OK;
	BOOL	show = FALSE, anon = FALSE, link = FALSE, res = FALSE, pdf = FALSE, keys = FALSE;
	UB	rn[TAD_NAME_MAX];
	T_VOBJ	lv;
	INT	c;

	for ( ;; ) {
		if ( word(&us, "pdf") ) {
			/*
			 * the file 印刷 wrote last: record 1 of what the last link of
			 * the desktop's first window points at, its time made noughts
			 */
			pdf = TRUE;
			link = TRUE;
		} else if ( word(&us, "res") ) {
			res = TRUE;		/* a resource beside the records, by its name ("_0_1.png") */
			us = skip(us);
			for ( i = 0; us[i] != 0 && us[i] != ' ' && i < (INT)sizeof(rn) - 1; i++ ) rn[i] = (UB)us[i];
			rn[i] = 0;
			us += i;
		} else if ( word(&us, "show") ) {
			show = TRUE;		/* the record said on the console as well */
		} else if ( word(&us, "anon") ) {
			anon = TRUE;		/* the UUIDs written in it made noughts */
		} else if ( word(&us, "link") ) {
			link = TRUE;		/* the object its last link points at, and that one's name */
		} else if ( word(&us, "keys") ) {
			keys = TRUE;		/* the record of the desktop's window with the keys, as it is now */
		} else {
			break;
		}
	}
	us = skip(us);
	if ( keys ) {
		/* written as xmlTAD from the window's record: a window with no object behind it too */
		DTWIN	*f = dt_win_of(wm_focused());

		buf = NULL;
		if ( link ) {
			/* the object the window's last link points at, as the store keeps it */
			c = ( f != NULL && f->rec != NULL ) ? tad_lnk_count(f->rec) : 0;
			if ( c <= 0 || tad_lnk_get(f->rec, c - 1, &lv) < E_OK ) {
				say("err no link\n");
				return;
			}
			u = lv.target;
			goto read;
		}
		if ( f != NULL && f->rec != NULL && tad_write_mem(f->rec, NULL, 0, &at) >= E_OK && at > 0 ) {
			buf = (UB *)Kmalloc(at + 1);
			if ( buf != NULL && tad_write_mem(f->rec, buf, at + 1, &at) < E_OK ) {
				Kfree(buf);
				buf = NULL;
			}
		}
		if ( buf == NULL ) {
			say("err no window\n");
			return;
		}
		sum_bytes(&h, buf, at, show);
		Kfree(buf);
		goto said;
	}
	if ( pdf ) {
		if ( dt_first_win() == NULL ) {
			say("err no window\n");
			return;
		}
		u = dt_first_win()->id;
	} else if ( *us == 0 && ut_big_have ) {
		u = ut_big;
	} else if ( big_of(us, &u) ) {
		/* one of the large objects made */
	} else if ( ts_str_to_uuid(us, &u) < E_OK ) {
		say("err no object\n");
		return;
	}
	if ( link ) {
		if ( !last_link(&u, &to) ) {
			say("err no link\n");
			return;
		}
		u = to;
	}
read:
	if ( res ) {
		if ( ( k = ob_opn_obj(&u, OB_OP_R) ) <= 0 ) {
			say("err no object\n");
			return;
		}
		buf = (UB *)Kmalloc(64 * 1024);
		while ( buf != NULL ) {
			er = ob_rea_res(k, rn, (D)at, buf, 64 * 1024, &asz);
			if ( er < E_OK || asz <= 0 ) break;
			sum_bytes(&h, buf, asz, FALSE);
			at += asz;
		}
		ob_cls_obj(k);
		if ( buf != NULL ) Kfree(buf);
	} else if ( anon || link ) {
		buf = pdf ? rec_whole_n(&u, 1, &at) : rec_whole(&u, &at);
		if ( buf == NULL ) {
			say("err no object\n");
			return;
		}
		if ( anon ) {
			uuids_blank(buf, at);
		}
		if ( pdf ) {
			pdf_date_blank(buf, at);
		}
		sum_bytes(&h, buf, at, show && !pdf);
		Kfree(buf);
	} else {
		if ( ( k = ob_opn_obj(&u, OB_OP_R) ) <= 0 ) {
			say("err no object\n");
			return;
		}
		buf = (UB *)Kmalloc(64 * 1024);
		while ( buf != NULL ) {
			er = ob_rea_rec(k, 0, (D)at, buf, 64 * 1024, &asz);
			if ( er < E_OK || asz <= 0 ) break;
			sum_bytes(&h, buf, asz, show);
			at += asz;
		}
		ob_cls_obj(k);
		if ( buf != NULL ) Kfree(buf);
	}
said:
	b[n++] = 'o';  b[n++] = 'k';  b[n++] = ' ';
	put_num(b, &n, (INT)at);
	b[n++] = ' ';
	for ( i = 15; i >= 0; i-- ) {
		b[n++] = "0123456789abcdef"[( h >> ( i * 4 ) ) & 0xF];
	}
	if ( link && om_store_name(&u, nm, sizeof(nm)) > 0 ) {
		b[n++] = ' ';
		for ( i = 0; nm[i] != 0 && n < (INT)sizeof(b) - 2; i++ ) b[n++] = (char)nm[i];
	}
	b[n++] = '\n';
	b[n] = 0;
	say(b);
}

/* "objects [new]": how many objects the volumes keep, or how many more than at the last count */
LOCAL void objects_say( CONST char *s )
{
	INT	n = om_store_objects(NULL, 0), k = 0;
	char	b[24];

	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	if ( word(&s, "new") ) {
		put_num(b, &k, n - ut_objects);
	} else {
		ut_objects = n;
		put_num(b, &k, n);
	}
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* "paper [UUID|big|bigN]": the paper the object's metadata keeps */
LOCAL void paper_say( CONST char *us )
{
	TS_UUID	u;
	UW	col = TV_PAPER_LOOK;
	char	b[24];

	us = skip(us);
	if ( *us == 0 && ut_big_have ) {
		u = ut_big;
	} else if ( !big_of(us, &u) && ts_str_to_uuid(us, &u) < E_OK ) {
		say("err no object\n");
		return;
	}
	if ( om_store_paper(&u, &col) < E_OK || col == TV_PAPER_LOOK ) {
		say("ok none\n");
		return;
	}
	tm_sprintf((UB *)b, (UB *)"ok %06X\n", (UINT)( col & 0x00FFFFFFU ));
	say(b);
}

LOCAL UW	ut_mark = 0;

LOCAL void say_ms( UW ms )
{
	char	b[24];
	INT	k = 0;

	b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
	put_num(b, &k, (INT)ms);
	b[k++] = '\n';
	b[k] = 0;
	say(b);
}

/* Up to S seconds for the window that says TITLE */
LOCAL void wait_win( CONST char *s )
{
	INT	i, secs = 60;
	char	title[WM_TITLE_MAX];
	UW	start = ut_now();

	INT	n, k;

	/* the rest of the line is the title, spaces and all; a number last is the seconds */
	s = skip(s);
	for ( n = 0; s[n] != 0 && s[n] != '\r' && s[n] != '\n'; n++ ) {
	}
	while ( n > 0 && s[n - 1] == ' ' ) n--;
	for ( k = n; k > 0 && s[k - 1] >= '0' && s[k - 1] <= '9'; k-- ) {
	}
	if ( k < n && k > 0 && s[k - 1] == ' ' ) {
		CONST char	*d = s + k;

		(void)num(&d, &secs, FALSE);
		for ( n = k; n > 0 && s[n - 1] == ' '; n-- ) {
		}
	}
	for ( i = 0; i < n && i < WM_TITLE_MAX - 1; i++ ) {
		title[i] = s[i];
	}
	title[i] = 0;
	while ( (INT)( ut_now() - start ) < secs * 1000 ) {
		if ( ut_named(title) > 0 ) {
			say_ms(ut_now() - ut_mark);
			return;
		}
		tk_dly_tsk(20);
	}
	say("err no such window\n");
}

/* Up to S seconds for the desktop's task to come round its loop: what it was doing done */
LOCAL void sync_desk( CONST char *s )
{
	INT	secs = 60;
	UW	start = ut_now();

	(void)num(&s, &secs, FALSE);
	ut_sync = 1;
	while ( ( ut_sync == 1 || ut_sync == 3 ) && (INT)( ut_now() - start ) < secs * 1000 ) {
		tk_dly_tsk(10);
	}
	if ( ut_sync != 2 ) {
		ut_sync = 0;
		say("err desktop busy\n");
		return;
	}
	ut_sync = 0;
	say_ms(ut_now() - ut_mark);
}

/* ---------------------------------------------------------------- a line done */

IMPORT ER dt_tagent_power( INT how );

/* The test program's three ways of being there as the machine goes off (tests/uprog/svcprog.c) */
LOCAL void sdn_start( void )
{
	static CONST UW	mode[3] = { 42, 43, 44 };	/* SP_SDNWIN, SP_SDNBG, SP_SDNSTUCK */
	ID		pid[3];
	INT		i;

	for ( i = 0; i < 3; i++ ) {
		T_CPRC	cprc;
		UW	arg[8];

		knl_memset(arg, 0, sizeof(arg));
		arg[0] = mode[i];
		cprc.pri = 5;
		cprc.prcatr = 0;
		cprc.stksz = 0;
		cprc.arg = arg;
		cprc.argsz = sizeof(arg);
		pid[i] = ts_cre_prc("/boot/SVCPROG.ELF", &cprc);
		if ( pid[i] <= 0 ) {
			say("err no process\n");
			return;
		}
	}
	tk_dly_tsk(1500);
	{
		char	b[64];

		tm_sprintf((UB *)b, (UB *)"ok %d %d %d\n", (INT)pid[0], (INT)pid[1], (INT)pid[2]);
		say(b);
	}
}

/* How long a file is: what an earlier start wrote, read on this one */
LOCAL void file_has( CONST char *path )
{
	T_FSTAT	st;

	if ( fs_stat(path, &st) < EX_OK ) {
		say("err none\n");
	} else {
		char	b[32];

		tm_sprintf((UB *)b, (UB *)"ok %d\n", (INT)st.size);
		say(b);
	}
}

LOCAL void line( CONST char *s )
{
	INT	x, y, x2, y2, b = 0, i, m = 0;

	if ( word(&s, "ready") ) {
		/*
		 * The desktop is up once processes can ask it for things: its
		 * request channel (DT_REQ_NAME) is made after its settings and
		 * programs are taken, which on a slow start is seconds after
		 * this task begins. Until then an open would be refused.
		 */
		TS_UUID	rq;

		for ( i = 0; i < 1000 && ob_fnd_nam((CONST UB *)DT_REQ_NAME, &rq) < E_OK; i++ ) {
			tk_dly_tsk(50);
		}
		/*
		 * and the steps of its start over (the first screen, the windows
		 * it puts up): its loop has come round. A window put up by a step
		 * after a test opened its own would stand in front of it.
		 */
		for ( ; i < 1000 && dt_test_loops() == 0; i++ ) {
			tk_dly_tsk(50);
		}
		say(( i < 1000 ) ? "ok\n" : "err the desktop has no request channel\n");
	} else if ( word(&s, "move") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		move(x, y);
		say("ok\n");
	} else if ( word(&s, "down") ) {
		(void)num(&s, &b, FALSE);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "up") ) {
		(void)num(&s, &b, FALSE);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "click") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		(void)num(&s, &b, FALSE);
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "dclick") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		move(x, y);
		for ( i = 0; i < 2; i++ ) {
			tk_dly_tsk(i == 0 ? UT_GAP_MS : UT_DCLICK_MS);
			ev(HID_EV_BTN_DOWN, 0, 0);
			tk_dly_tsk(UT_GAP_MS);
			ev(HID_EV_BTN_UP, 0, 0);
		}
		say("ok\n");
	} else if ( word(&s, "hold") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		(void)num(&s, &b, FALSE);
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "dragn") && num(&s, &x, FALSE) && num(&s, &y, FALSE)
		 && num(&s, &x2, FALSE) && num(&s, &y2, FALSE) ) {
		/*
		 * dragn X Y X2 Y2 STEPS GAP [BUTTON]: a drag made of STEPS
		 * moves GAP ms apart, as a mouse reporting at 1000 / GAP Hz
		 * moves the pointer
		 */
		INT	steps = UT_STEPS, gap = UT_GAP_MS;

		(void)num(&s, &steps, FALSE);
		(void)num(&s, &gap, FALSE);
		(void)num(&s, &b, FALSE);
		if ( steps < 1 ) steps = 1;
		if ( gap < 1 ) gap = 1;
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		for ( i = 1; i <= steps; i++ ) {
			tk_dly_tsk(gap);
			move(x + ( x2 - x ) * i / steps, y + ( y2 - y ) * i / steps);
		}
		tk_dly_tsk(UT_GAP_MS * 4);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "drag") && num(&s, &x, FALSE) && num(&s, &y, FALSE)
		 && num(&s, &x2, FALSE) && num(&s, &y2, FALSE) ) {
		(void)num(&s, &b, FALSE);
		move(x, y);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_BTN_DOWN, (UINT)b, 0);
		for ( i = 1; i <= UT_STEPS; i++ ) {
			tk_dly_tsk(UT_GAP_MS);
			move(x + ( x2 - x ) * i / UT_STEPS, y + ( y2 - y ) * i / UT_STEPS);
		}
		tk_dly_tsk(UT_GAP_MS * 4);
		ev(HID_EV_BTN_UP, (UINT)b, 0);
		say("ok\n");
	} else if ( word(&s, "linkdrag") ) {
		/*
		 * linkdrag WINDOW NAME X Y [BUTTON]: the link to the object
		 * NAME in the window WINDOW dragged from its middle to X, Y,
		 * wherever it stands among the others
		 */
		char		title[WM_TITLE_MAX], name[TAD_NAME_MAX];
		T_DPRECT	box;

		if ( !token(&s, title, sizeof(title)) || !token(&s, name, sizeof(name))
		  || !num(&s, &x2, FALSE) || !num(&s, &y2, FALSE) ) {
			say("err linkdrag WINDOW NAME X Y\n");
		} else if ( !link_named(title, name, &box) ) {
			say("err no such link\n");
		} else {
			b = 0;
			(void)num(&s, &b, FALSE);
			x = ( box.left + box.right ) / 2;
			y = ( box.top + box.bottom ) / 2;
			move(x, y);
			tk_dly_tsk(UT_GAP_MS);
			ev(HID_EV_BTN_DOWN, (UINT)b, 0);
			for ( i = 1; i <= UT_STEPS; i++ ) {
				tk_dly_tsk(UT_GAP_MS);
				move(x + ( x2 - x ) * i / UT_STEPS, y + ( y2 - y ) * i / UT_STEPS);
			}
			tk_dly_tsk(UT_GAP_MS * 4);
			ev(HID_EV_BTN_UP, (UINT)b, 0);
			say("ok\n");
		}
	} else if ( word(&s, "key") && num(&s, &x, TRUE) ) {
		(void)num(&s, &m, TRUE);
		ev(HID_EV_KEY_DOWN, (UINT)x, (UINT)m);
		tk_dly_tsk(UT_GAP_MS);
		ev(HID_EV_KEY_UP, (UINT)x, (UINT)m);
		say("ok\n");
	} else if ( word(&s, "keydown") && num(&s, &x, TRUE) ) {
		/* held until its keyup: keys pressed together, a shift held over others */
		(void)num(&s, &m, TRUE);
		ev(HID_EV_KEY_DOWN, (UINT)x, (UINT)m);
		say("ok\n");
	} else if ( word(&s, "keyup") && num(&s, &x, TRUE) ) {
		(void)num(&s, &m, TRUE);
		ev(HID_EV_KEY_UP, (UINT)x, (UINT)m);
		say("ok\n");
	} else if ( word(&s, "text") ) {
		s = skip(s);
		for ( ; *s != 0; s++ ) {
			BOOL	sh;
			UINT	u = usage_of(*s, &sh);

			if ( u == 0 ) continue;
			ev(HID_EV_KEY_DOWN, u, sh ? HID_MOD_LSHIFT : 0);
			tk_dly_tsk(UT_GAP_MS);
			ev(HID_EV_KEY_UP, u, sh ? HID_MOD_LSHIFT : 0);
			tk_dly_tsk(UT_GAP_MS);
		}
		say("ok\n");
	} else if ( word(&s, "wheel") && num(&s, &x, FALSE) ) {
		T_HIDEV	e;

		knl_memset(&e, 0, sizeof(e));
		e.type = HID_EV_WHEEL;
		e.code = HID_WHEEL_V;
		e.x = ut_x;
		e.y = ut_y;
		e.dz = x;
		inject(&e);
		say("ok\n");
	} else if ( word(&s, "accpos") ) {
		accpos_say(skip(s));
	} else if ( word(&s, "mkpkg") ) {
		mkpkg_say();
	} else if ( word(&s, "proginstq") ) {
		prog_ask(skip(s), 3);
	} else if ( word(&s, "proginstf") ) {
		prog_ask(skip(s), 4);
	} else if ( word(&s, "sysup") ) {
		sysup_ask(skip(s));
	} else if ( word(&s, "progres") ) {
		progres_say();
	} else if ( word(&s, "adminpanels") ) {
		adminpanels_say();
	} else if ( word(&s, "progrm") ) {
		prog_ask(skip(s), 0);
	} else if ( word(&s, "proginst") ) {
		prog_ask(skip(s), 1);
	} else if ( word(&s, "refs") ) {
		prog_ask(skip(s), 2);
	} else if ( word(&s, "emurom") ) {
		ut_emurom();
	} else if ( word(&s, "emuromx") ) {
		ut_emuromx(s);
	} else if ( word(&s, "kslide") ) {
		kslide(skip(s));
	} else if ( word(&s, "linkbig") ) {
		linkbig(s);
	} else if ( word(&s, "figproc") ) {
		figproc(&ut_fig_proc, &ut_fig_kernel, skip(s));
	} else if ( word(&s, "docproc") ) {
		figproc(&ut_doc_proc, &ut_doc_kernel, skip(s));
	} else if ( word(&s, "pfig") ) {
		pfig("basic-figure-editor", skip(s));
	} else if ( word(&s, "pdoc") ) {
		pfig("basic-text-editor", skip(s));
	} else if ( word(&s, "cabproc") ) {
		figproc(&ut_cab_proc, &ut_cab_kernel, skip(s));
	} else if ( word(&s, "pcab") ) {
		pfig("virtual-object-list", skip(s));
	} else if ( word(&s, "open") ) {
		open_obj(skip(s));
	} else if ( word(&s, "apps") ) {
		set_apps(s);
	} else if ( word(&s, "run") ) {
		run_prog(s);
	} else if ( word(&s, "sdnstart") ) {
		sdn_start();
	} else if ( word(&s, "filehas") ) {
		file_has(skip(s));
	} else if ( word(&s, "poweroff") ) {
		say("ok\n");
		(void)dt_tagent_power(0);
	} else if ( word(&s, "offlimit") && num(&s, &x, FALSE) ) {
		knl_off_limit((UW)x, 0);	/* the going off not finished in time, with nothing going off */
		say("ok\n");
	} else if ( word(&s, "reboot") ) {
		say("ok\n");
		(void)dt_tagent_power(1);
	} else if ( word(&s, "win") ) {
		find_win(skip(s));
	} else if ( word(&s, "wins") ) {
		all_wins();
	} else if ( word(&s, "front") ) {
		front_check(s);
	} else if ( word(&s, "focus") ) {
		focus_check(s);
	} else if ( word(&s, "hidden") ) {
		hidden_check(s);
	} else if ( word(&s, "wait") && num(&s, &x, FALSE) ) {
		tk_dly_tsk(( x > 0 ) ? x : 0);
		say("ok\n");
	} else if ( word(&s, "idle") ) {
		tk_dly_tsk(150);
		say("ok\n");
	} else if ( word(&s, "lag") && num(&s, &x, FALSE) ) {
		lag(x);
	} else if ( word(&s, "svcstat") ) {
		svc_stat();
	} else if ( word(&s, "stkuse") ) {
		/* the deepest use of every stack, to the console (design 4.7.7) */
#if CNF_STK_USAGE
		IMPORT void knl_stk_usage_say( void );

		knl_stk_usage_say();
#endif
		say("ok\n");
	} else if ( word(&s, "waits") ) {
		waits_say();
	} else if ( word(&s, "pnstat") ) {
		/* the panels drawn whole, and the builds that read the windows while one was drawn */
		IMPORT volatile UW	knl_pn_draws, knl_wm_torn;
		static UW		pn_was, torn_was;
		char			b[48];
		INT			k = 0;
		UW			d = knl_pn_draws, t = knl_wm_torn;

		b[k++] = 'o';  b[k++] = 'k';  b[k++] = ' ';
		put_num(b, &k, (INT)( d - pn_was ));  b[k++] = ' ';
		put_num(b, &k, (INT)( t - torn_was ));
		b[k++] = '\n';
		b[k] = 0;
		pn_was = d;
		torn_was = t;
		say(b);
	} else if ( word(&s, "stat") ) {
		ut_stat();
	} else if ( word(&s, "xmlopen") ) {
		xml_open(skip(s));
	} else if ( word(&s, "xmlbig") ) {
		xml_big();
	} else if ( word(&s, "xml") ) {
		xml_keep(skip(s));
	} else if ( word(&s, "shapes") ) {
		shapes_say();
	} else if ( word(&s, "caret") ) {
		caret_say();
	} else if ( word(&s, "redraw") ) {
		redraw_ask();
	} else if ( word(&s, "mkbig") ) {
		make_big(s);
	} else if ( word(&s, "ftdef") ) {
		ftdef();
	} else if ( word(&s, "sum") ) {
		rec_sum(s);
	} else if ( word(&s, "objects") ) {
		objects_say(s);
	} else if ( word(&s, "paper") ) {
		paper_say(s);
	} else if ( word(&s, "mark") ) {
		ut_mark = ut_now();
		say("ok\n");
	} else if ( word(&s, "waitwin") ) {
		wait_win(s);
	} else if ( word(&s, "sync") ) {
		sync_desk(s);
	} else if ( word(&s, "wifitest") ) {
		if ( knl_wifi_test_card != NULL ) {
			knl_wifi_test_card();
			say("ok\n");
		} else {
			say("err no wifi\n");
		}
	} else if ( word(&s, "look") && num(&s, &x, FALSE) && num(&s, &y, FALSE) ) {
		if ( wm_set_look((UINT)x, (UW)y) >= E_OK ) {
			(void)wm_composite();
			say("ok\n");
		} else {
			say("err look\n");
		}
	} else {
		say("err what\n");
	}
}

/* One line at a time, whichever way it came */
LOCAL ID	ut_one = 0;

LOCAL void one_line( CONST char *s, void (*sink)( CONST char *answer ) )
{
	if ( ut_one > 0 ) (void)tk_wai_sem(ut_one, 1, TMO_FEVR);
	say("> ");			/* what came, in the log beside what was answered */
	say(s);
	say("\n");
	ut_sink = sink;
	line(s);
	ut_sink = NULL;
	if ( ut_one > 0 ) (void)tk_sig_sem(ut_one, 1);
}

#ifdef USE_TESTAGENT
EXPORT void dt_uitest_line( CONST char *s, void (*sink)( CONST char *answer ) )
{
	one_line(s, sink);
}
#endif

#ifdef USE_UT_SERIAL
LOCAL void ut_task( INT stacd, void *exinf )
{
	char	buf[UT_LINE];
	UB	c;
	INT	n = 0;
	SZ	asz = 0;
	TMO	tmo = 200;

	(void)stacd;
	(void)exinf;
	(void)tk_swri_dev(ut_dd, TDN_SER_RCVTMO, &tmo, sizeof(tmo), &asz);
	tm_printf((UB *)"uitest: ready\n");
	for ( ;; ) {
		asz = 0;
		if ( tk_srea_dev(ut_dd, 0, &c, 1, &asz) < E_OK || asz == 0 ) {
			continue;
		}
		if ( c == '\r' ) continue;
		if ( c != '\n' ) {
			if ( n < UT_LINE - 1 ) buf[n++] = (char)c;
			continue;
		}
		buf[n] = 0;
		n = 0;
		one_line(buf, NULL);
	}
}
#endif /* USE_UT_SERIAL */

EXPORT void dt_uitest_start( void )
{
#ifdef USE_UT_SERIAL
	T_CTSK	ct;
	ID	tid;
#endif
	T_CSEM	cs;

	knl_memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO | TA_FIRST;
	cs.isemcnt = 1;
	cs.maxsem = 1;
	ut_one = tk_cre_sem(&cs);
#ifdef USE_TESTAGENT
	dt_tagent_start();
#endif
#ifdef USE_UT_SERIAL
	ut_dd = tk_opn_dev((UB *)"serb", TD_UPDATE);
	if ( ut_dd <= 0 ) {
		tm_printf((UB *)"uitest: no second serial port\n");
		return;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ut_task;
	ct.itskpri = 6;		/* above the desktop, which may be busy for a while; it mostly waits on the port */
	ct.stksz = 32768;	/* it draws windows (linkbig): the letters of a CFF face are made on this stack */
	tid = tk_cre_tsk(&ct);
	if ( tid > 0 ) {
		(void)tk_sta_tsk(tid, 0);
	}
#endif /* USE_UT_SERIAL */
}

#else

EXPORT void dt_uitest_start( void )
{
}

EXPORT void dt_uitest_poll( void )
{
}

EXPORT void dt_uitest_idle( void )
{
}

#endif /* USE_UITEST */
