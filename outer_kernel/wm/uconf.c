/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	uconf.c
 *	The person's settings, as ユーザ環境設定 left them (design 16.5.23)
 *
 *	The user information object (SYSDEF_CONF_USER) keeps in its record 1
 *	a line for each thing the accessory sets (lib/libconf's table): the
 *	times, sizes and switches of the pointer, the keyboard, the screen
 *	and the sound, which are entries of the look table, and the name of
 *	the person who uses the machine. They are taken once the store is
 *	there; the look table's entries are what the window layer, the
 *	keyboard and the pointer read as they go, so nothing else is told.
 *	The name is taken at the start only: a name changed is the machine's
 *	from its next start, as the accessory says.
 *
 *	The machine's settings (SYSDEF_CONF_DEV) are taken here too: the
 *	colour scheme, the wallpaper, the keyboard's kind and the mode it
 *	starts in, and the output the sound plays on (SOUND_OUT). The
 *	network's are the network layer's (so_conf.c).
 *
 *	All three objects are watched once taken: a program that writes one
 *	of them -- a settings accessory -- has it taken again, as a notice
 *	of the change (OB_E_CHANGE) reaches a task of this file. Nobody asks
 *	the system to take them; writing them is enough.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/keymap.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/om.h>
#include <ts/hid.h>
#include <ts/so.h>
#include <ts/lockup.h>
#include <ts/xf.h>
#include <ts/cpuclk.h>
#include <ts/snd.h>
#include "obj/obj.h"

LOCAL UB	user_name[CF_NAME_MAX * 4 + 1];	/* UTF-8, up to twelve letters */
LOCAL BOOL	user_named = FALSE;

/*
 * A settings object opened. Writing one is exclusive, and the objects
 * are read and written by several at once -- the watcher reading what
 * was just written, the mixer's keeper, a program setting the keyboard
 * -- each for a moment: one that finds it busy waits for that moment
 * (up to a second) rather than failing.
 */
#define CONF_BUSY_TRIES	100
#define CONF_BUSY_MS	10

LOCAL ID conf_open( CONST TS_UUID *u, UINT ops )
{
	ID	key;
	INT	i;

	/* a writer keeps other writers out, not the readers (each record is replaced whole) */
	if ( ( ops & ( OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR ) ) != 0 ) {
		ops |= OB_O_WEXCL;
	}
	for ( i = 0; ; i++ ) {
		key = ob_opn_obj(u, ops);
		if ( key != E_BUSY || i >= CONF_BUSY_TRIES ) {
			return key;
		}
		tk_dly_tsk(CONF_BUSY_MS);
	}
}

/*
 * A settings object's record read whole, however long, into a buffer of
 * its own (*p_text, Kfree it): its length or an error. The object is the
 * one システム links with role, fixed by default (knl_sys_find).
 */
LOCAL INT conf_text_of( CONST char *role, CONST char *fixed, UB **p_text )
{
	TS_UUID	u;
	ID	key;
	INT	len;

	*p_text = NULL;
	if ( !knl_sys_find(role, fixed, &u) ) {
		return E_SYS;
	}
	key = conf_open(&u, OB_OP_READ);
	if ( key < E_OK ) {
		return (INT)key;
	}
	len = knl_ob_rea_all(key, 1, 0, p_text, NULL);
	ob_cls_obj(key);
	return len;
}

/*
 * A setting that names an object is a link of the settings object's
 * record 0, told by its relationship (cf_link_get, ts/conf.h): the
 * object the link with role points at.
 */
EXPORT BOOL knl_conf_link_get( CONST char *uuid, CONST char *role, TS_UUID *p_u )
{
	TS_UUID	u;
	ID	key;
	UB	*text = NULL;
	INT	len;
	BOOL	hit;

	if ( ts_str_to_uuid(uuid, &u) < E_OK ) return FALSE;
	key = conf_open(&u, OB_OP_READ);
	if ( key < E_OK ) return FALSE;
	len = knl_ob_rea_all(key, 0, 0, &text, NULL);
	ob_cls_obj(key);
	hit = (BOOL)( len > 0 && cf_link_get(text, len, role, p_u) );
	if ( text != NULL ) Kfree(text);
	return hit;
}

/*
 * The link with role set to point at target, in place of the one there
 * or as a paragraph of its own after label; counted in, and the object
 * it pointed at before counted out, where the store does not count
 */
EXPORT ER knl_conf_link_put( CONST char *uuid, CONST char *role, CONST TS_UUID *target, CONST char *label )
{
	TS_UUID	u, vobj, old;
	T_OBREF	r;
	ID	key;
	UB	*text = NULL;
	INT	len, cap = 0, n;
	SZ	asz = 0;
	ER	er;
	static CONST TS_UUID zero;

	if ( ts_str_to_uuid(uuid, &u) < E_OK || ts_gen_uuid(&vobj) < E_OK ) return E_SYS;
	key = conf_open(&u, OB_OP_READ | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	len = knl_ob_rea_all(key, 0, 640, &text, &cap);
	if ( len < 0 ) {
		ob_cls_obj(key);
		return (ER)len;
	}
	n = cf_link_put(text, len, cap, role, target, &vobj, label, &old);
	/* replaced in one change: a reader, let in meanwhile, sees the old record or the new one */
	er = ( n == -2 ) ? E_OBJ : ( n < 0 ) ? E_LIMIT : ob_rpl_rec(key, 0, text, (SZ)n);
	(void)asz;
	ob_cls_obj(key);
	Kfree(text);
	if ( er >= E_OK && ob_ref_obj(&u, &r) >= E_OK && ( r.flags & OB_F_AUTOREF ) == 0 ) {
		if ( ts_uuid_cmp(&old, target) != 0 ) {
			(void)ob_lnk_obj(target);
			if ( ts_uuid_cmp(&old, &zero) != 0 ) (void)ob_unl_obj(&old);
		}
	}
	return er;
}

/*
 * The system's objects, found from the root システム (SYSDEF_SYSTEM_BOX,
 * the one identity the code is sure of) by the relationships of its
 * links: what the root links to with the relationship role. A root
 * whose link has gone, or one made before the links had relationships,
 * still answers: the identity each object was made with is the
 * default, and knl_sys_mark gives the links their relationships, or
 * puts a link that has gone back, at the start.
 */
LOCAL CONST struct {
	CONST char	*role;
	CONST char	*fixed;
} sys_roles[] = {
	{ SYSROLE_DEFS,		SYSDEF_BOX },
	{ SYSROLE_PROGS,	SYSDEF_PROG_BOX },
	{ SYSROLE_ACCS,		SYSDEF_ACC_BOX },
	{ SYSROLE_FONTS,	SYSDEF_FONT_BOX },
	{ SYSROLE_WALLS,	SYSDEF_WALL_BOX },
	{ SYSROLE_LEARN,	SYSDEF_LEARN_BOX },
	{ SYSROLE_DICTS,	SYSDEF_DICT_BOX },
	{ SYSROLE_XFCONV,	SYSDEF_XFCONV_BOX },
	{ SYSROLE_CALENDAR,	SYSDEF_CALENDAR },
	{ SYSROLE_CONF_NET,	SYSDEF_CONF_NET },
	{ SYSROLE_CONF_DEV,	SYSDEF_CONF_DEV },
	{ SYSROLE_CONF_USER,	SYSDEF_CONF_USER },
	{ SYSROLE_SOUNDS,	SYSDEF_SND_BOX },
	{ SYSROLE_CERTS,	SYSDEF_CA_CERTS },
	{ SYSROLE_ROOT,		SYSDEF_ROOT },
};

EXPORT BOOL knl_sys_find( CONST char *role, CONST char *fixed, TS_UUID *p_u )
{
	T_OBREF	r;

	if ( role != NULL && knl_conf_link_get(SYSDEF_SYSTEM_BOX, role, p_u) && ob_ref_obj(p_u, &r) >= E_OK ) {
		return TRUE;
	}
	return (BOOL)( fixed != NULL && ts_str_to_uuid(fixed, p_u) >= E_OK );
}

/* The same for a program (TSN_OB_FND_SYS), and for the kernel by the name it has there */
EXPORT ER ob_fnd_sys( CONST char *role, CONST char *dflt, TS_UUID *p_uuid )
{
	TS_UUID	u;

	if ( role == NULL || p_uuid == NULL ) return E_PAR;
	if ( !knl_sys_find(role, dflt, &u) ) return E_NOEXS;
	*p_uuid = u;
	return E_OK;
}

EXPORT CONST char *knl_sys_id( CONST char *role, CONST char *fixed, char *buf )
{
	TS_UUID	u;

	if ( !knl_sys_find(role, fixed, &u) || ts_uuid_to_str(&u, buf, TS_UUID_STRLEN + 1) < E_OK ) {
		return fixed;
	}
	return buf;
}

EXPORT void knl_sys_mark( void )
{
	TS_UUID	root, u, any;
	T_OBREF	r;
	ID	key;
	UB	*text = NULL;
	INT	len, cap = 0, n;
	SZ	asz = 0;
	UINT	i;
	BOOL	wrote = FALSE;

	if ( ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &root) < E_OK ) return;
	key = conf_open(&root, OB_OP_READ | OB_OP_WRITE);
	if ( key < E_OK ) return;			/* a root no one may write: read as it is */
	len = knl_ob_rea_all(key, 0, 64 * 40, &text, &cap);
	for ( i = 0; len >= 0 && i < sizeof(sys_roles) / sizeof(sys_roles[0]); i++ ) {
		if ( cf_link_get(text, len, sys_roles[i].role, &any) ) continue;
		if ( ts_str_to_uuid(sys_roles[i].fixed, &u) < E_OK || ob_ref_obj(&u, &r) < E_OK ) continue;
		n = cf_link_tag(text, len, cap, &u, sys_roles[i].role);
		if ( n == -2 ) {
			TS_UUID	vobj;

			/* the link gone: put back, and counted where the store does not count */
			if ( ts_gen_uuid(&vobj) < E_OK ) continue;
			n = cf_link_put(text, len, cap, sys_roles[i].role, &u, &vobj, "", NULL);
			if ( n >= 0 && ob_ref_obj(&root, &r) >= E_OK && ( r.flags & OB_F_AUTOREF ) == 0 ) {
				(void)ob_lnk_obj(&u);
			}
			if ( n >= 0 ) tm_printf((UB *)"uconf: the link to %s put back in システム\n", sys_roles[i].role);
		}
		if ( n >= 0 && n != len ) {
			len = n;
			wrote = TRUE;
		}
	}
	if ( wrote ) {
		(void)ob_rpl_rec(key, 0, text, (SZ)len);	/* whole: a reader sees the old record or the new one */
		(void)asz;
	}
	ob_cls_obj(key);
	if ( text != NULL ) Kfree(text);
}

/*
 * A value of the record copied out whole, however long, as a string in
 * a buffer of its own (Kfree it): a list such as CPU_CHORE or KBMAP is
 * never cut to a buffer's size. NULL when there is no such line, or no
 * memory.
 */
LOCAL char *conf_dup( CONST UB *text, INT len, CONST char *key )
{
	CONST UB	*v;
	INT		n;
	char		*s;

	if ( !cf_find(text, len, key, 0, &v, &n) ) {
		return NULL;
	}
	s = (char *)Kmalloc((SZ)n + 1);
	if ( s != NULL ) {
		knl_memcpy(s, v, n);
		s[n] = '\0';
	}
	return s;
}

/*
 * The keyboard's and the pointer's attributes as the look table has
 * them, given to the device layer, which takes every report through
 * them: called whenever one of their entries is written. They are
 * written to the input object 入力 (OB_IN_ATTR), as whoever set the
 * entry -- the kernel, or the administrator's program that asked --
 * and directly only before the object layer is up.
 */
EXPORT void knl_input_sync( void )
{
	T_HIDATTR	a;
	ID		key;

	knl_memset(&a, 0, sizeof(a));
	a.pd_on = (UINT)wm_num(LK_PD_ON, 0);
	a.pd_off = (UINT)wm_num(LK_PD_OFF, 0);
	a.speed = (UINT)wm_num(LK_PD_SPEED, 12);
	a.accel = (UINT)wm_num(LK_PD_ACCEL, 7);
	a.main = (UINT)wm_num(LK_PD_MAIN, 0);
	a.absolute = (UINT)wm_num(LK_PD_ABS, 1);
	a.middbl = (UINT)wm_num(LK_PD_MIDDBL, 1);
	a.key_on = (UINT)wm_num(LK_KEY_ON, 0);
	a.key_off = (UINT)wm_num(LK_KEY_OFF, 0);
	a.key_sim = (UINT)wm_num(LK_KEY_SIM, 0);
	a.krp = (UINT)wm_num(LK_KRP, 1);
	a.krp_start = (UINT)wm_num(LK_KRP_START, 800);
	a.krp_int = (UINT)wm_num(LK_KRP_INT, 100);
	a.sclk = (UINT)wm_num(LK_SCLK, 0);
	a.tshift = (UINT)wm_num(LK_TSHIFT, 0);
	a.tch_off = ( wm_num(LK_TCH_ON, 1) == 0 );
	a.tch_dbl = (UINT)wm_num(LK_TCH_DBL, 50);
	a.tch_hold = (UINT)wm_num(LK_TCH_HOLD, 1000);
	a.tch_noscr = ( wm_num(LK_TCH_SCR, 1) == 0 );
	a.tch_scr = (UINT)wm_num(LK_TCH_STEP, 100);
	key = ob_opn_obj(&ob_uuid_input, OB_OP_WRITE);
	if ( key > 0 ) {
		(void)ob_wri_rec(key, OB_IN_ATTR, 0, &a, sizeof(a), NULL);
		(void)ob_cls_obj(key);
	} else if ( key == E_NOEXS ) {
		(void)ts_hid_setattr(&a);	/* before there are objects */
	}
}

EXPORT ER wm_conf_apply( void )
{
	UB	*text;
	char	v[CF_VAL_MAX];
	INT	len, i, n, got = 0;

	len = conf_text_of(SYSROLE_CONF_USER, SYSDEF_CONF_USER, &text);
	if ( len < 0 ) {
		return (ER)len;
	}
	for ( i = 0; i < cf_user_nitem; i++ ) {
		CONST CF_ITEM	*it = &cf_user_items[i];

		if ( !cf_get(text, len, it->key, 0, v, sizeof(v)) ) {
			continue;
		}
		n = cf_num(v, it->dflt);
		if ( n < it->lo ) n = it->lo;
		if ( n > it->hi ) n = it->hi;
		(void)knl_look_setting(it->look, (UW)n);
		got++;
	}
	if ( !user_named && cf_get(text, len, CF_USER_NAME, 0, v, sizeof(v)) ) {
		for ( i = 0; v[i] != 0 && i < (INT)sizeof(user_name) - 1; i++ ) {
			user_name[i] = (UB)v[i];
		}
		user_name[i] = 0;
		user_named = TRUE;
	}
	Kfree(text);
	knl_input_sync();
	tm_printf((UB *)"user: %d settings taken, the user is \"%s\"\n", got, user_name);
	return E_OK;
}

/*
 * The processors' clock: at most CPU_MHZ MHz, 0 the most the firmware
 * allows; with no line, or one that is not a number, CPU_MHZ_DEFAULT
 * (cpuclk.c).
 */
LOCAL void cpu_conf( CONST UB *text, INT len )
{
	char	v[CF_VAL_MAX];
	INT	n = CPU_MHZ_DEFAULT;

	if ( cf_get(text, len, "CPU_MHZ", 0, v, sizeof(v)) ) {
		n = cf_num(v, CPU_MHZ_DEFAULT);
		if ( n < 0 ) n = CPU_MHZ_DEFAULT;
	}
	(void)knl_cpu_limit((UW)n);

	/*
	 * The processors' roles (design 4.5): CPU_CHORE those for the
	 * system's chores, which take the devices' interrupts (PRC 1
	 * always), CPU_RT those kept for real time, which take none. No
	 * line: PRC 1 alone, and none for real time.
	 */
	{
		char	*c = conf_dup(text, len, "CPU_CHORE"), *r = conf_dup(text, len, "CPU_RT");

		(void)knl_cpu_roles_text(( c != NULL ) ? c : "", ( r != NULL ) ? r : "");
		if ( c != NULL ) Kfree(c);
		if ( r != NULL ) Kfree(r);
	}
}

/*
 * The same at the start, as soon as the store is there: the clock is
 * not to wait for the desktop, which takes all the settings later.
 */
EXPORT ER wm_cpu_conf_apply( void )
{
	UB	*text;
	INT	len;

	len = conf_text_of(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &text);
	if ( len >= 0 ) {
		cpu_conf(text, len);
		Kfree(text);
	}
	return ( len < 0 ) ? (ER)len : E_OK;
}

/*
 * The machine's settings: SCHEME n (the colour scheme), the wallpaper --
 * the link of record 0 whose relationship is CF_ROLE_WALL, and how it is
 * laid in the line WALLPAPER uuid [mode] (a system before kept the
 * picture in that line alone: it is followed, and the link made from it,
 * once) --, KBTYPE 0x41 or 0x40 (a
 * Japanese or an English keyboard), KBMODE (the input mode the desktop
 * starts in: 0 かな, 1 英数), the windows' look (WIN_*, cf_look_items), the
 * watchdog (WATCHDOG seconds, 0 off) and the processors' clock (CPU_MHZ).
 * A line not there leaves what is, but for the watchdog and the clock,
 * which go back to their defaults. The wallpaper is read again only
 * when its line has changed: writing another line of the object is not
 * to load a picture the size of the screen again.
 */
LOCAL char	wall_line[CF_VAL_MAX];		/* the WALLPAPER line last laid */

/*
 * The mixer's settings (include/ts/snd.h): the whole's volume as
 * "SOUND_VOL left right mute", each program's as "SOUND_APP uuid volume
 * mute", a line each. Taken as they are read, without being kept again;
 * the mixer has them kept, a second after they last changed, by
 * snd_keep. There are as many SOUND_APP lines as programs have had
 * a volume kept.
 */

/*
 * The mixer's lines as they were last kept or taken, summed. The
 * watcher takes them again only when they are not those: a write of
 * another line (the keyboard's, the output chosen), or the mixer's own
 * keeping, leaves them as the mixer had them a moment before, and a
 * volume or a mute changed since -- before the watcher came round, or
 * before the second the keeper waits -- would be set back by them and
 * then never kept, the mixer being set from the settings without
 * keeping them again.
 */
LOCAL UW	snd_lines_last = 0;

LOCAL UW snd_lines_sum( CONST UB *text, INT len )
{
	char	v[CF_VAL_MAX];
	UW	h = 2166136261U;
	INT	k, i;

	for ( k = -1; ; k++ ) {
		if ( !cf_get(text, len, ( k < 0 ) ? "SOUND_VOL" : "SOUND_APP", ( k < 0 ) ? 0 : k, v, sizeof(v)) ) {
			if ( k >= 0 ) {
				break;
			}
			v[0] = 0;
		}
		for ( i = 0; v[i] != 0; i++ ) {
			h = ( h ^ (UB)v[i] ) * 16777619U;
		}
		h = ( h ^ '\n' ) * 16777619U;
	}
	return h;
}

LOCAL void snd_keep( void );

/* The mixer's lines taken; when 'changed' only, only when they are not those last kept or taken */
LOCAL void snd_conf( CONST UB *text, INT len, BOOL changed )
{
	SDMixSet	s;
	char		v[CF_VAL_MAX], w[48];
	INT		k;
	TS_UUID		u;
	UW		sum = snd_lines_sum(text, len);

	knl_snd_mix_keeper(snd_keep);
	if ( changed && sum == snd_lines_last ) {
		return;
	}
	snd_lines_last = sum;
	if ( cf_get(text, len, "SOUND_VOL", 0, v, sizeof(v)) && cf_word(v, 1, w, sizeof(w)) ) {
		knl_memset(&s, 0, sizeof(s));
		s.what = SND_MIX_ALL;
		(void)cf_word(v, 0, w, sizeof(w));
		s.lv = (UW)cf_num(w, SND_VOL_MAX);
		(void)cf_word(v, 1, w, sizeof(w));
		s.rv = (UW)cf_num(w, SND_VOL_MAX);
		s.mute = ( cf_word(v, 2, w, sizeof(w)) && cf_num(w, 0) != 0 );
		(void)knl_snd_mix_set(&s, FALSE);
	}
	for ( k = 0; cf_get(text, len, "SOUND_APP", k, v, sizeof(v)); k++ ) {
		if ( !cf_word(v, 0, w, sizeof(w)) || ts_str_to_uuid(w, &u) < E_OK ) {
			continue;			/* a line left empty */
		}
		knl_memset(&s, 0, sizeof(s));
		s.what = SND_MIX_APP;
		knl_memcpy(s.prog, &u, 16);
		s.lv = s.rv = ( cf_word(v, 1, w, sizeof(w)) ) ? (UW)cf_num(w, SND_VOL_MAX) : SND_VOL_MAX;
		s.mute = ( cf_word(v, 2, w, sizeof(w)) && cf_num(w, 0) != 0 );
		(void)knl_snd_mix_set(&s, FALSE);
	}
}

/* The mixer's settings into the machine's settings: one write for all the lines */
LOCAL void snd_keep( void )
{
	SDMix		*m = (SDMix *)Kmalloc(sizeof(SDMix));
	SDMixSet	*a = NULL;
	UB		*text = NULL;
	char		v[CF_VAL_MAX], us[40];
	TS_UUID		u;
	SZ		asz = 0;
	INT		len, n = 0, k, cap = 0, room = 0;
	ID		key = E_NOMEM;
	ER		er = E_NOMEM;

	if ( m != NULL && knl_sys_find(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &u) ) {
		key = conf_open(&u, OB_OP_R | OB_OP_WRITE);
	}
	if ( key > 0 ) {
		(void)knl_snd_mix_get(m, 0);
		/* every program kept: as many as the mixer says, asked again while that grows */
		for ( ;; ) {
			n = knl_snd_mix_kept(a, room);
			if ( n <= room ) {
				break;
			}
			if ( a != NULL ) Kfree(a);
			room = n + 8;
			a = (SDMixSet *)Kmalloc(sizeof(SDMixSet) * (SZ)room);
			if ( a == NULL ) {
				room = 0;
				n = 0;
				break;
			}
		}
		/* room for the volume's line and a line for each program, whatever the record holds */
		len = knl_ob_rea_all(key, 1, ( n + 1 ) * ( CF_VAL_MAX + 16 ), &text, &cap);
		if ( len < 0 ) {
			len = 0;
			text = (UB *)Kmalloc(( n + 1 ) * ( CF_VAL_MAX + 16 ) + 1);
			cap = ( text != NULL ) ? ( n + 1 ) * ( CF_VAL_MAX + 16 ) : 0;
		}
		tm_sprintf((UB *)v, (CONST UB *)"%d %d %d", (INT)m->lv, (INT)m->rv, (INT)( m->mute != 0 ));
		len = ( text != NULL ) ? cf_put(text, len, cap, "SOUND_VOL", 0, v) : -1;
		for ( k = 0; len >= 0; k++ ) {
			if ( k < n ) {
				knl_memcpy(&u, a[k].prog, 16);
				(void)ts_uuid_to_str(&u, us, sizeof(us));
				tm_sprintf((UB *)v, (CONST UB *)"%s %d %d", us, (INT)a[k].lv, (INT)( a[k].mute != 0 ));
				len = cf_put(text, len, cap, "SOUND_APP", k, v);
			} else if ( cf_get(text, len, "SOUND_APP", k, v, sizeof(v)) ) {
				len = cf_put(text, len, cap, "SOUND_APP", k, "");	/* one no longer kept */
			} else {
				break;
			}
		}
		if ( len >= 0 ) {
			snd_lines_last = snd_lines_sum(text, len);	/* what the watcher is not to take again */
		}
		/* replaced in one change: a cut in the power leaves the old record or the new one */
		er = ( len < 0 ) ? E_LIMIT : ob_rpl_rec(key, 1, text, (SZ)len);
		(void)asz;
		ob_cls_obj(key);
	}
	if ( er < E_OK ) {
		tm_printf((UB *)"wm: the mixer's settings not kept (%d)\n", (INT)er);
	}
	if ( m != NULL ) Kfree(m);
	if ( a != NULL ) Kfree(a);
	if ( text != NULL ) Kfree(text);
}

LOCAL ER dev_conf_apply( BOOL watched );

EXPORT ER wm_dev_conf_apply( void )
{
	return dev_conf_apply(FALSE);
}

/* The machine's settings applied; 'watched': for a write the watcher heard of (snd_conf) */
LOCAL ER dev_conf_apply( BOOL watched )
{
	UB	*text;
	char	v[CF_VAL_MAX], w[64], *map;
	INT	len, i, n;
	BOOL	looked = FALSE;

	len = conf_text_of(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &text);
	if ( len < 0 ) {
		return (ER)len;
	}
	if ( cf_get(text, len, "SCHEME", 0, v, sizeof(v)) && v[0] != 0
	  && cf_num(v, -1) >= 0 && (UINT)cf_num(v, -1) != wm_scheme() ) {
		(void)wm_set_scheme((UINT)cf_num(v, 0));
	}
	{
		TS_UUID	u, lu;
		char	m[8], now[CF_VAL_MAX];
		INT	mode = WM_WALL_FIT;
		BOOL	line = (BOOL)( cf_get(text, len, "WALLPAPER", 0, v, sizeof(v)) && cf_word(v, 0, w, sizeof(w)) );
		BOOL	has = FALSE;
		char	idb[TS_UUID_STRLEN + 1];
		CONST char *dev = knl_sys_id(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, idb);

		if ( line && cf_word(v, 1, m, sizeof(m)) ) mode = cf_num(m, WM_WALL_FIT);
		if ( knl_conf_link_get(dev, CF_ROLE_WALL, &lu) ) {
			u = lu;
			has = TRUE;
		} else if ( line && ts_str_to_uuid(w, &u) >= E_OK ) {
			has = TRUE;
			(void)knl_conf_link_put(dev, CF_ROLE_WALL, &u, "壁紙：");
		}
		if ( has ) {
			(void)ts_uuid_to_str(&u, now, sizeof(now));
			n = (INT)knl_strlen(now);
			now[n++] = ' ';
			n += cf_itoa(mode, now + n);
			now[n] = 0;
		}
		if ( has && knl_strcmp(now, wall_line) != 0
		  && wm_load_wall_obj(&u, xf_data_rec(&u), (UINT)mode) >= E_OK ) {
			knl_obsys_wall(&u, mode);
			for ( i = 0; now[i] != 0 && i < (INT)sizeof(wall_line) - 1; i++ ) {
				wall_line[i] = now[i];
			}
			wall_line[i] = 0;
		}
	}
	for ( i = 0; i < cf_look_nitem; i++ ) {
		CONST CF_ITEM	*it = &cf_look_items[i];

		if ( !cf_get(text, len, it->key, 0, v, sizeof(v)) ) {
			continue;
		}
		n = cf_num(v, it->dflt);
		if ( n < it->lo ) n = it->lo;
		if ( n > it->hi ) n = it->hi;
		if ( knl_wm_look(it->look, n) > 0 ) {
			looked = TRUE;
		}
	}
	if ( looked ) {
		(void)wm_composite();		/* the windows in their look */
	}
	/* the watchdog: seconds from a stopped machine found to its reset, 0 none (lockup.c) */
	knl_wdog_set(cf_get(text, len, "WATCHDOG", 0, v, sizeof(v)) ? cf_num(v, LK_RESET_S_DEFAULT)
								 : LK_RESET_S_DEFAULT);
	cpu_conf(text, len);
	/* the output the sound plays on (include/ts/snd.h); no line leaves it as it is */
	if ( cf_get(text, len, "SOUND_OUT", 0, v, sizeof(v)) && v[0] != 0 ) {
		(void)knl_snd_select((CONST UB *)v);
	}
	/* whether the stream is kept going in silence; no line: it is let stop */
	if ( cf_get(text, len, "SOUND_KEEP", 0, v, sizeof(v)) && v[0] != 0 ) {
		(void)knl_snd_keep(cf_num(v, 0) != 0);
	}
	snd_conf(text, len, watched);
	if ( cf_get(text, len, "KBTYPE", 0, v, sizeof(v)) ) {
		(void)knl_look_setting(LK_KBD_US, ( cf_num(v, 0x41) == 0x40 ) ? 1 : 0);
	}
	if ( cf_get(text, len, "KBMODE", 0, v, sizeof(v)) ) {
		(void)knl_look_setting(LK_KBD_MODE, (UW)( cf_num(v, 1) & 3 ));
	}
	/* the layouts and the keys given other numbers (キー配列) */
	if ( cf_get(text, len, "KBLAYOUT", 0, v, sizeof(v)) ) {
		(void)knl_look_setting(LK_KBD_LAYOUT, (UW)cf_num(v, KM_LATIN_KEYS));
	}
	if ( cf_get(text, len, "KBKANA", 0, v, sizeof(v)) ) {
		(void)knl_look_setting(LK_KBD_KANA, (UW)cf_num(v, KM_KANA_JIS));
	}
	/* the keys given other numbers: a list, taken whole however long */
	map = conf_dup(text, len, "KBMAP");
	km_map_parse(( map != NULL ) ? (CONST char *)map : "");
	if ( map != NULL ) Kfree(map);
	Kfree(text);
	return E_OK;
}

/* One line of the machine's settings written: the key's value, "" for the key alone */
EXPORT ER wm_dev_conf_put( CONST char *key, CONST char *val )
{
	TS_UUID	u;
	UB	*text;
	ID	k;
	SZ	asz = 0;
	INT	len, cap = 0, room;
	ER	er;

	if ( !knl_sys_find(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &u) ) {
		return E_SYS;
	}
	k = conf_open(&u, OB_OP_R | OB_OP_WRITE);
	if ( k < E_OK ) {
		return (ER)k;
	}
	/* the record whole, with room for the line however long it is */
	room = (INT)knl_strlen(key) + (INT)( ( val != NULL ) ? knl_strlen(val) : 0 ) + 4;
	len = knl_ob_rea_all(k, 1, room, &text, &cap);
	if ( len < 0 ) {
		ob_cls_obj(k);
		return (ER)len;
	}
	len = cf_put(text, len, cap, key, 0, val);
	/* replaced in one change: a cut in the power leaves the old record or the new one */
	er = ( len < 0 ) ? E_LIMIT : ob_rpl_rec(k, 1, text, (SZ)len);
	(void)asz;
	ob_cls_obj(k);
	Kfree(text);
	return er;
}

/* A setting of the machine's read: the value's words into v; FALSE when there is no such line */
EXPORT BOOL wm_dev_conf_get( CONST char *key, char *v, INT max )
{
	UB	*text;
	INT	len;
	BOOL	got = FALSE;

	len = conf_text_of(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &text);
	if ( len < 0 ) {
		return FALSE;
	}
	if ( len > 0 ) got = cf_get(text, len, key, 0, v, max);
	Kfree(text);
	return got;
}

/* ---------------------------------------------------------------- emptied at each start */

/*
 * The list of what is emptied at each start is the links of 機器の設定's
 * record 0 with the word CF_ROLE_CLEAR: a list of the objects, so that
 * the start never walks a whole volume for them. 機器の設定 is the
 * system's (only it and the administrator may write it), so no one else
 * can have an object of theirs emptied. A settings object made before
 * there was a list (R1.110 and before: no CF_CLEAR_LIST line) is given
 * tmp in it, once; a list the administrator has changed since is left
 * as it is.
 */
LOCAL void clear_list_make( CONST TS_UUID *dev, CONST char *devid )
{
	TS_UUID	tmp, any;
	T_OBREF	r;
	UB	*text = NULL;
	INT	len;
	CONST UB *v;
	INT	vn;
	BOOL	made, listed;
	ID	key;

	key = conf_open(dev, OB_OP_READ);
	if ( key < E_OK ) return;
	len = knl_ob_rea_all(key, 1, 0, &text, NULL);
	made = (BOOL)( len >= 0 && cf_find(text, len, CF_CLEAR_LIST, 0, &v, &vn) );
	if ( text != NULL ) Kfree(text);
	text = NULL;
	if ( made ) {
		ob_cls_obj(key);
		return;
	}
	len = knl_ob_rea_all(key, 0, 0, &text, NULL);
	listed = (BOOL)( len > 0 && cf_link_get(text, len, CF_ROLE_CLEAR, &any) );
	if ( text != NULL ) Kfree(text);
	ob_cls_obj(key);
	if ( !listed && ts_str_to_uuid(SYSDEF_TMP, &tmp) >= E_OK && ob_ref_obj(&tmp, &r) >= E_OK ) {
		if ( knl_conf_link_put(devid, CF_ROLE_CLEAR, &tmp, "起動のたびに空にする実身：") < E_OK ) return;
		tm_printf((UB *)"uconf: tmp put in the list of what is emptied at each start\n");
	}
	(void)wm_dev_conf_put(CF_CLEAR_LIST, "1");
}

EXPORT void knl_sys_clear_start( CONST char *path )
{
	TS_UUID	dev, u;
	T_OBREF	r;
	UB	*text = NULL;
	INT	len, i;
	ID	key;
	char	idb[TS_UUID_STRLEN + 1];
	CONST char *devid;

	if ( path == NULL || !knl_sys_find(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, &dev)
	  || ob_ref_obj(&dev, &r) < E_OK ) {
		return;				/* no settings yet: on a volume still to come */
	}
	devid = knl_sys_id(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, idb);
	if ( knl_obfile_on(&dev, path) ) {
		clear_list_make(&dev, devid);
	}
	key = conf_open(&dev, OB_OP_READ);
	if ( key < E_OK ) return;
	len = knl_ob_rea_all(key, 0, 0, &text, NULL);
	ob_cls_obj(key);
	for ( i = 0; len > 0 && cf_link_nth(text, len, CF_ROLE_CLEAR, i, &u); i++ ) {
		if ( !knl_obfile_on(&u, path) || ob_ref_obj(&u, &r) < E_OK || r.type != OB_T_STORAGE ) {
			continue;		/* on another volume, emptied as that one is attached */
		}
		if ( om_obj_links_set(&u, NULL, 0) >= E_OK ) {
			tm_printf((UB *)"uconf: %s emptied at the start\n", r.name);
		}
	}
	if ( text != NULL ) Kfree(text);
}

/* ---------------------------------------------------------------- watched */

#define WATCH_PRI	30		/* low, and within 1..CNF_MAX_TSKPRI: beyond it the task is not made */

LOCAL ID	watch_tsk = 0;

LOCAL void conf_watch( INT stacd, void *exinf )
{
	CONST char *CONST which[3] = { SYSDEF_CONF_NET, SYSDEF_CONF_USER, SYSDEF_CONF_DEV };
	CONST char *CONST role[3] = { SYSROLE_CONF_NET, SYSROLE_CONF_USER, SYSROLE_CONF_DEV };
	TS_UUID	u[3], ch;
	T_OBCRE	c;
	T_OBNTF	req;
	T_OBNTM	m;
	ID	port = 0, key[3];
	SZ	asz = 0;
	INT	i;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK || ( port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) ) <= 0 ) {
		watch_tsk = 0;
		tk_exd_tsk();
	}
	/*
	 * The network's settings, taken behind the desktop's start: the
	 * interface may have to ask the network for an address, which is
	 * not to hold up the first window. A machine run for its tests
	 * keeps the interface its tests set.
	 */
#if !USE_KTEST
	(void)so_conf(SO_CONF_APPLY);
#endif
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	for ( i = 0; i < 3; i++ ) {
		key[i] = 0;
		if ( !knl_sys_find(role[i], which[i], &u[i]) ) continue;
		key[i] = ob_opn_obj(&u[i], OB_OP_ATRRD);
		if ( key[i] > 0 && ob_ntf_evt(key[i], 1, &req, port) <= 0 ) {
			(void)ob_cls_obj(key[i]);
			key[i] = 0;
		}
	}
	for ( ;; ) {
		BOOL	got[3] = { FALSE, FALSE, FALSE };

		if ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) < E_OK || asz == 0 ) {
			tk_dly_tsk(200);		/* nothing yet */
			continue;
		}
		/* a write and the cut after it are one change: what else comes soon is the same */
		for ( ;; ) {
			for ( i = 0; i < 3; i++ ) {
				if ( ts_uuid_cmp(&m.uuid, &u[i]) == 0 ) got[i] = TRUE;
			}
			tk_dly_tsk(100);
			if ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) < E_OK || asz == 0 ) break;
		}
#if !USE_KTEST
		if ( got[0] ) {
			tm_printf((UB *)"settings: the network's were written\n");
			(void)so_conf(SO_CONF_APPLY);
		}
#endif
		if ( got[1] ) {
			tm_printf((UB *)"settings: the person's were written\n");
			(void)wm_conf_apply();
		}
		if ( got[2] ) {
			tm_printf((UB *)"settings: the machine's were written\n");
			(void)dev_conf_apply(TRUE);
		}
	}
}

/* The three settings objects watched from now on */
EXPORT ER wm_conf_watch( void )
{
	T_CTSK	ct;

	if ( watch_tsk > 0 ) {
		return E_OK;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)conf_watch;
	ct.itskpri = WATCH_PRI;
	ct.stksz = 16 * 1024;
	watch_tsk = tk_cre_tsk(&ct);
	if ( watch_tsk <= 0 ) {
		tm_printf((UB *)"settings: the watcher could not be made (%d)\n", (INT)watch_tsk);
		return (ER)watch_tsk;
	}
	return tk_sta_tsk(watch_tsk, 0);
}

EXPORT INT wm_user_name( UB *buf, INT max )
{
	INT	i;

	if ( buf == NULL || max <= 0 ) {
		return 0;
	}
	for ( i = 0; user_name[i] != 0 && i < max - 1; i++ ) {
		buf[i] = user_name[i];
	}
	buf[i] = 0;
	return i;
}
