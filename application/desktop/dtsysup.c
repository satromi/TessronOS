/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtsysup.c
 *	The system updated from its package (design 11.8)
 *
 *	The system's package (.tpk, made by tools/mktpk.py --system) is an
 *	application's package of another kind: package.json says "kind":
 *	"system", the version (TS_VERSION, R1.300), the machine and the
 *	processor it is for, and the objects it holds, each as the files of
 *	the TADjs form, and the files of the boot partition (/boot: the Pi's
 *	FAT partition, the PC's EFI system partition) as entries
 *	"boot/PATH". Taken in from outside (ファイル変換), it is an object
 *	whose record 1 is the file; it is read from there a piece at a time,
 *	never whole, as it holds every program of the system.
 *
 *	What the package says of each object decides what is done to it:
 *	  the system's own   its metadata, icon, records and resources made
 *	                     the package's; a record the same already is
 *	                     not written again
 *	  "keep"             the person's (the settings, the user
 *	                     dictionary, the browser's store, the home):
 *	                     made when the system does not have it, never
 *	                     written over
 *	  "links"            a box the person adds to: made when missing,
 *	                     else kept as it is, with a link added to each
 *	                     object of the package's box it does not link to
 *	Nothing is deleted: an object, a record or a resource the system has
 *	and the package does not is left as it is. Objects the package does
 *	not hold (the person's, the applications installed) are not touched.
 *	A new object that no link reaches is held once, as the image's are.
 *
 *	The update is a list of steps done in order: every object made or
 *	given its metadata, then every object's records, then the holds,
 *	then each boot file written beside the one there as PATH.new (the
 *	PATH.old of the update before deleted first, to make room), and
 *	last the boot files put in place. How depends on who reads the file:
 *	  the firmware, or     the one there copied to PATH.old and PATH.new
 *	  the kernel           renamed over PATH, which the FAT store does
 *	                       as one change (fatfs.c): PATH is there all
 *	                       the time, the old or the new
 *	  the PC's UEFI        PATH renamed to PATH.old, then PATH.new to
 *	  loader (the kernel,  PATH: no copy, so a 200 MB ESP shared with
 *	  cmdline.txt)         Windows has room for a large kernel. Between
 *	                       the two renames there is no PATH, and the
 *	                       loader reads PATH.new, else PATH.old
 *	                       (loader.c read_near)
 *	The ones the firmware reads go first, so that the loader that falls
 *	back to PATH.new is in place before a file it reads is renamed. A
 *	cut in the power leaves every record whole (the native store
 *	replaces a record in one change), and on the boot partition each
 *	file readable, the old or the new. Before the first step the
 *	machine's settings are given SYSUP, the package's UUID, and it is
 *	taken off after the last; a start that finds it does the steps again
 *	from the first (dt_sysup_resume). Every step can be done again: a
 *	record already the package's is passed over, a box gets only the
 *	links it lacks, a boot file already the new one is left, and one
 *	left between its two renames is finished.
 *
 *	A boot file is put where the running system keeps it. The package
 *	names it as a medium made by tools/mksd.py has it (the loader in
 *	EFI/BOOT/, the kernel at the root); a PC the system was put on with
 *	システム登録 (EFI/TESSRON/INSTALL.TXT there) keeps the loader, the
 *	kernel and cmdline.txt in \EFI\TESSRON\ of an ESP that may be
 *	Windows's. So:
 *	  the loader           every one of EFI/TESSRON/ and the package's
 *	  (EFI/...)            place that holds TessronOS's loader (the
 *	                       text LOADER_MARK); one that is another's
 *	                       (Windows's \EFI\Boot\bootx64.efi) is never
 *	                       written. None there: the package's place, or
 *	                       EFI/TESSRON/ when installed
 *	  the kernel,          the first of EFI/TESSRON/ and the root that
 *	  cmdline.txt          has it, as the loader looks; none: the root,
 *	                       or EFI/TESSRON/ when installed
 *	  any other            the package's place; when installed only if
 *	                       the system has one there, as nothing else of
 *	                       a shared ESP is the system's
 *
 *	The boot files the package names under "bootkeep" (the start-up
 *	settings the person writes, cmdline.txt of the PC) are put only
 *	where there is none.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/om.h>
#include <ts/fs.h>
#include <ts/json.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/time.h>
#include <ts/wm.h>
#include <ts/sysdef.h>
#include <ts/dtreq.h>
#include <ts/lockup.h>
#include "desktop.h"

#define PKG_HEAD	16
#define PKG_ENTRY	64
#define PKG_NAME	48
#define PKG_FORMAT	1

#define SU_PKG_REC	1		/* the record of the package object that is the file */
#define SU_CHUNK	( 4 * 1024 * 1024 )	/* bytes read and compared at a time */
#define SU_WHOLE	( 16 * 1024 * 1024 )	/* a record up to this written in one change */
#define SU_STEP_NS	300000000ULL	/* how long one DT_SYSUP_STEP works */
#define SU_PATH_MAX	96
#define SU_ROOT_MAX	40
#define SU_KEY		"SYSUP"		/* the machine's settings: the package being put on */
#define SU_TS_DIR	"EFI/TESSRON/"	/* where システム登録 puts the PC's boot files */
#define SU_INSTALL	SU_TS_DIR "INSTALL.TXT"	/* its note: this ESP is a PC's own disk's */
#define LOADER_MARK	"TessronOS loader"	/* a text TessronOS's UEFI loader holds */

#if defined(_RPI5_)
#define SU_MACHINE	"rpi5"
#elif defined(_RPI4_)
#define SU_MACHINE	"rpi4"
#elif defined(_X64_)
#define SU_MACHINE	"x64"
#else
#define SU_MACHINE	"qemu_virt"
#endif
#if defined(_X64_)
#define SU_ARCH		"x86_64"
#else
#define SU_ARCH		"aarch64"
#endif

/* What an entry of the package is */
#define E_META		(-1)		/* {uuid}.json */
#define E_ICON		(-2)		/* {uuid}.ico */
#define E_RES		(-3)		/* {uuid}_N_M.ext */
#define E_BOOT		(-4)		/* boot/PATH */
#define E_OTHER		(-5)		/* package.json, or what is not known */

/* How an object is updated */
#define U_SYSTEM	0
#define U_KEEP		1
#define U_LINKS		2

typedef struct {
	UB	name[PKG_NAME];
	UW	off, size, rt, sub;
	TS_UUID	u;			/* the object, for E_META .. E_RES and records */
	INT	what;			/* E_* or the record's number */
	BOOL	keep;			/* E_BOOT: one of "bootkeep" */
	BOOL	put;			/* E_BOOT: one of "boot" or "bootkeep" */
} SUENT;

typedef struct {
	TS_UUID	u;
	INT	how;			/* U_* */
} SUOBJ;

/* A place a boot file is put */
typedef struct {
	INT	ent;			/* the package's entry */
	BOOL	near;			/* read by the UEFI loader: put in place by two renames */
	char	path[SU_PATH_MAX];	/* the file, below the boot partition's root */
} SUBOOT;

/* The update under way, between the steps */
LOCAL struct {
	BOOL	on;
	TS_UUID	pkg;
	ID	key;			/* the package, opened to read */
	ID	pid;			/* the process that began it */
	SUENT	*e;
	INT	n;
	SUOBJ	*obj;
	INT	nobj;
	INT	nboot;
	SUBOOT	*boot;			/* where each boot file goes, those the firmware reads first */
	INT	total, done;
	UB	ver[DT_VER_MAX];
} su;

/* The boot partition's root, and for the tests a cut in the power after so many renames */
LOCAL char	su_root[SU_ROOT_MAX] = "/boot";
LOCAL INT	su_cut = -1;

/*
 * The files the PC's UEFI loader reads, looking in \EFI\TESSRON\ before
 * the root and falling back to NAME.new and NAME.old (loader.c
 * read_near). No other machine's package has these names.
 */
LOCAL CONST char * CONST su_near[] = { "tessronos.elf", "cmdline.txt", NULL };

LOCAL UW le32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL BOOL same_bytes( CONST UB *a, CONST UB *b, SZ n )
{
	SZ	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL name_is( CONST UB *name, CONST char *s )
{
	INT	i;

	for ( i = 0; s[i] != 0; i++ ) {
		if ( name[i] != (UB)s[i] ) return FALSE;
	}
	return (BOOL)( name[i] == 0 );
}

/* ---------------------------------------------------------------- reading the package */

/* size bytes of the package from off, all of them */
LOCAL ER pkg_read( ID key, D off, void *buf, SZ size )
{
	SZ	got = 0, asz;
	ER	er;

	knl_wdog_alive();		/* the desktop's loop is away, working: not stuck */
	while ( got < size ) {
		asz = 0;
		er = ob_rea_rec(key, SU_PKG_REC, off + (D)got, (UB *)buf + got, size - got, &asz);
		if ( er < E_OK ) return er;
		if ( asz <= 0 ) return E_OBJ;		/* the file is shorter than its table says */
		got += asz;
	}
	return E_OK;
}

/* An entry's bytes whole, the caller's to Kfree; NULL when they cannot be read */
LOCAL UB *ent_bytes( CONST SUENT *e )
{
	UB	*b = (UB *)Kmalloc((SZ)e->size + 1);

	if ( b == NULL ) return NULL;
	if ( pkg_read(su.key, (D)e->off, b, (SZ)e->size) < E_OK ) {
		Kfree(b);
		return NULL;
	}
	b[e->size] = 0;
	return b;
}

/* What an entry's name says: an object's file, a boot file, or neither */
LOCAL void ent_kind( SUENT *e )
{
	char	us[TS_UUID_STRLEN + 1];
	CONST UB *t = e->name + TS_UUID_STRLEN;
	INT	i, n = 0;

	e->what = E_OTHER;
	knl_memset(&e->u, 0, sizeof(e->u));
	if ( e->name[0] == 'b' && e->name[1] == 'o' && e->name[2] == 'o' && e->name[3] == 't'
	  && e->name[4] == '/' && e->name[5] != 0 ) {
		e->what = E_BOOT;
		return;
	}
	for ( i = 0; i < TS_UUID_STRLEN; i++ ) {
		if ( e->name[i] == 0 ) return;
		us[i] = (char)e->name[i];
	}
	us[i] = 0;
	if ( ts_str_to_uuid(us, &e->u) < E_OK ) return;
	if ( name_is(t, ".json") ) {
		e->what = E_META;
		return;
	}
	if ( name_is(t, ".ico") ) {
		e->what = E_ICON;
		return;
	}
	if ( t[0] != '_' || t[1] < '0' || t[1] > '9' ) return;
	for ( t++; *t >= '0' && *t <= '9'; t++ ) {
		n = n * 10 + ( *t - '0' );
		if ( n > 0xFFFF ) return;
	}
	if ( name_is(t, ".xtad") || name_is(t, ".bin") ) {
		e->what = n;
	} else if ( t[0] == '_' ) {
		e->what = E_RES;		/* the resource's name is what follows the UUID */
	}
}

/* ---------------------------------------------------------------- the version and the machine */

/* The system's version now (OB_SYS_INFO's VERSION) */
LOCAL void sys_version( UB *out, INT max )
{
	TS_UUID	u;
	UB	*t;
	SZ	size = 0;

	out[0] = 0;
	if ( ts_str_to_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return;
	t = om_obj_record(&u, OB_SYS_INFO, &size);
	if ( t == NULL ) return;
	(void)cf_get(t, (INT)size, "VERSION", 0, (char *)out, max);
	Kfree(t);
}

/* ---------------------------------------------------------------- the session */

LOCAL void su_end( void )
{
	if ( su.key > 0 ) ob_cls_obj(su.key);
	if ( su.e != NULL ) Kfree(su.e);
	if ( su.obj != NULL ) Kfree(su.obj);
	if ( su.boot != NULL ) Kfree(su.boot);
	knl_memset(&su, 0, sizeof(su));
}

LOCAL BOOL listed( CONST T_JSON *root, CONST char *list, CONST UB *what )
{
	T_JSON	l, it;
	UB	s[PKG_NAME + 1];

	if ( js_get(root, list, &l) < E_OK ) return FALSE;
	for ( it.s = NULL; js_next(&l, &it); ) {
		if ( js_str(&it, s, sizeof(s)) > 0 && name_is(what, (CONST char *)s) ) return TRUE;
	}
	return FALSE;
}

/*
 * The package opened and its table and package.json read: E_OK when it
 * is a newer system for this machine, E_LIMIT the same or older,
 * E_NOSPT another machine's, E_OBJ not a system package. text: the
 * package's version, a tab and the system's (E_NOSPT: its machine and
 * processor). The session is left open only for E_OK and E_LIMIT.
 */
LOCAL ER su_open( CONST TS_UUID *pkg, UB *text, INT max )
{
	UB	head[PKG_HEAD], *tab = NULL, *man = NULL, s[48], now[DT_VER_MAX];
	T_JSON	root, list, it;
	INT	i, k, room;
	ER	er;

	su_end();
	if ( text != NULL && max > 0 ) text[0] = 0;
	su.key = ob_opn_obj(pkg, OB_OP_READ | OB_OP_ATRRD);
	if ( su.key <= 0 ) {
		su.key = 0;
		return E_OBJ;
	}
	su.pkg = *pkg;
	if ( pkg_read(su.key, 0, head, PKG_HEAD) < E_OK
	  || head[0] != 'T' || head[1] != 'S' || head[2] != 'P' || head[3] != 'K'
	  || ( head[4] | ( head[5] << 8 ) ) != PKG_FORMAT ) {
		er = E_OBJ;
		goto bad;
	}
	su.n = head[6] | ( head[7] << 8 );
	tab = ( su.n > 0 ) ? (UB *)Kmalloc((SZ)su.n * PKG_ENTRY) : NULL;
	su.e = ( su.n > 0 ) ? (SUENT *)Kmalloc(sizeof(SUENT) * (SZ)su.n) : NULL;
	if ( tab == NULL || su.e == NULL ) {
		er = ( su.n > 0 ) ? E_NOMEM : E_OBJ;
		goto bad;
	}
	er = pkg_read(su.key, PKG_HEAD, tab, (SZ)su.n * PKG_ENTRY);
	if ( er < E_OK ) {
		er = E_OBJ;
		goto bad;
	}
	for ( i = 0; i < su.n; i++ ) {
		CONST UB	*d = tab + i * PKG_ENTRY;

		knl_memcpy(su.e[i].name, d, PKG_NAME);
		su.e[i].off = le32(d + PKG_NAME);
		su.e[i].size = le32(d + PKG_NAME + 4);
		su.e[i].rt = le32(d + PKG_NAME + 8);
		su.e[i].sub = le32(d + PKG_NAME + 12);
		su.e[i].keep = FALSE;
		su.e[i].put = FALSE;
		if ( su.e[i].name[PKG_NAME - 1] != 0 ) {
			er = E_OBJ;
			goto bad;
		}
		ent_kind(&su.e[i]);
	}
	Kfree(tab);
	tab = NULL;

	/* package.json: a system's, its version, machine and processor */
	for ( i = 0; i < su.n && !name_is(su.e[i].name, "package.json"); i++ ) ;
	man = ( i < su.n ) ? ent_bytes(&su.e[i]) : NULL;
	if ( man == NULL || js_parse(man, (INT)su.e[i].size, &root) < E_OK
	  || js_get_str(&root, "kind", s, sizeof(s)) <= 0 || !name_is(s, "system")
	  || js_get_str(&root, "version", su.ver, sizeof(su.ver)) <= 0 || dt_ver_num(su.ver) < 0 ) {
		er = E_OBJ;
		goto bad;
	}
	er = E_OK;
	if ( js_get_str(&root, "machine", s, sizeof(s)) <= 0 || !name_is(s, SU_MACHINE) ) {
		er = E_NOSPT;
	} else if ( js_get_str(&root, "arch", s, sizeof(s)) <= 0 || !name_is(s, SU_ARCH) ) {
		er = E_NOSPT;
	}
	if ( er == E_NOSPT ) {
		if ( text != NULL ) {
			UB	a[24];

			a[0] = 0;
			(void)js_get_str(&root, "machine", s, sizeof(s));
			(void)js_get_str(&root, "arch", a, sizeof(a));
			tm_sprintf(text, (UB *)"%s\t%s", s, a);
		}
		goto bad;
	}

	/* the objects, and how each is updated */
	room = ( js_get(&root, "objects", &list) >= E_OK ) ? js_count(&list) : 0;
	su.obj = (SUOBJ *)Kmalloc(sizeof(SUOBJ) * (SZ)( room + 1 ));
	if ( su.obj == NULL ) {
		er = E_NOMEM;
		goto bad;
	}
	for ( it.s = NULL; room > 0 && su.nobj < room && js_next(&list, &it); ) {
		UB	us[TS_UUID_STRLEN + 1];
		TS_UUID	u;

		if ( js_str(&it, us, sizeof(us)) != TS_UUID_STRLEN
		  || ts_str_to_uuid((CONST char *)us, &u) < E_OK ) continue;
		for ( k = 0; k < su.nobj && ts_uuid_cmp(&su.obj[k].u, &u) != 0; k++ ) ;
		if ( k < su.nobj ) continue;
		su.obj[su.nobj].u = u;
		su.obj[su.nobj].how = listed(&root, "keep", us) ? U_KEEP
				    : listed(&root, "links", us) ? U_LINKS : U_SYSTEM;
		su.nobj++;
	}

	/* the boot files the package names; where each goes is found by su_place */
	for ( i = 0; i < su.n; i++ ) {
		if ( su.e[i].what != E_BOOT ) continue;
		su.e[i].keep = listed(&root, "bootkeep", su.e[i].name + 5);
		su.e[i].put = su.e[i].keep || listed(&root, "boot", su.e[i].name + 5);
	}
	su.done = 0;

	sys_version(now, sizeof(now));
	if ( text != NULL ) tm_sprintf(text, (UB *)"%s\t%s", su.ver, now);
	er = ( dt_ver_num(su.ver) > dt_ver_num(now) ) ? E_OK : E_LIMIT;
	Kfree(man);
	return er;

bad:
	if ( tab != NULL ) Kfree(tab);
	if ( man != NULL ) Kfree(man);
	su_end();
	return er;
}

/* ---------------------------------------------------------------- an object's parts */

/* The entries of one object: the first of what */
LOCAL SUENT *ent_of( CONST TS_UUID *u, INT what )
{
	INT	i;

	for ( i = 0; i < su.n; i++ ) {
		if ( su.e[i].what == what && ts_uuid_cmp(&su.e[i].u, u) == 0 ) return &su.e[i];
	}
	return NULL;
}

/* The metadata written, the protection it says taken as well when 'prot' */
LOCAL ER meta_put( CONST TS_UUID *u, CONST UB *json, SZ size, BOOL prot )
{
	ID	key = prot ? ob_opn_obj(u, OB_OP_ATRRD | OB_OP_ATRWR | OB_OP_PROT) : 0;
	ER	er;

	if ( key <= 0 ) {
		return om_obj_meta_put(u, json, size);
	}
	er = ob_set_atr(key, json, size);
	ob_cls_obj(key);
	return er;
}

/* Step 1 of an object: made when missing; the system's own given the package's metadata and icon */
LOCAL ER obj_meta( CONST SUOBJ *o )
{
	SUENT	*me = ent_of(&o->u, E_META), *ie = ent_of(&o->u, E_ICON);
	UB	*meta, *icon = NULL, *now;
	T_OBREF	r;
	T_OBCRE	c;
	T_OBPRT	prt;
	TS_UUID	got;
	SZ	isz = 0;
	ER	er = E_OK;

	if ( me == NULL ) {
		return E_OBJ;
	}
	if ( ob_ref_obj(&o->u, &r) >= E_OK && o->how != U_SYSTEM ) {
		return E_OK;			/* the person's, or a box they add to: as it is */
	}
	meta = ent_bytes(me);
	icon = ( ie != NULL ) ? ent_bytes(ie) : NULL;
	if ( meta == NULL || ( ie != NULL && icon == NULL ) ) {
		er = E_NOMEM;
		goto out;
	}
	if ( ob_ref_obj(&o->u, &r) >= E_OK ) {
		er = om_obj_meta_put(&o->u, meta, (SZ)me->size);
		if ( er >= E_OK && icon != NULL ) {
			now = om_obj_icon(&o->u, &isz);
			if ( now == NULL || isz != (SZ)ie->size || !same_bytes(now, icon, isz) ) {
				er = om_obj_icon_put(&o->u, icon, (SZ)ie->size);
			}
			if ( now != NULL ) Kfree(now);
		}
	} else {
		dt_sys_prt(&prt);
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = meta;
		c.jsonsz = (SZ)me->size;
		c.prt = &prt;
		c.uuid = o->u;
		c.icon = icon;
		c.iconsz = ( icon != NULL ) ? (SZ)ie->size : 0;
		er = ob_cre_obj(&c, &got);
		if ( er >= E_OK ) {
			(void)meta_put(&o->u, meta, (SZ)me->size, TRUE);	/* and the protection it says */
		}
	}
out:
	if ( meta != NULL ) Kfree(meta);
	if ( icon != NULL ) Kfree(icon);
	return er;
}

/* Whether the record recno of an object is already the entry's bytes */
LOCAL BOOL rec_same( CONST TS_UUID *u, INT recno, UD size, CONST SUENT *e )
{
	UB	*a, *b;
	ID	k;
	UD	at;
	SZ	n, asz;
	BOOL	same = TRUE;

	if ( size != (UD)e->size ) return FALSE;
	if ( size == 0 ) return TRUE;
	a = (UB *)Kmalloc(SU_CHUNK);
	b = (UB *)Kmalloc(SU_CHUNK);
	k = ob_opn_obj(u, OB_OP_READ);
	if ( a == NULL || b == NULL || k <= 0 ) {
		same = FALSE;
	}
	for ( at = 0; same && at < size; at += (UD)n ) {
		n = ( size - at > SU_CHUNK ) ? SU_CHUNK : (SZ)( size - at );
		asz = 0;
		if ( ob_rea_rec(k, recno, (D)at, a, n, &asz) < E_OK || asz != n
		  || pkg_read(su.key, (D)e->off + (D)at, b, n) < E_OK || !same_bytes(a, b, n) ) {
			same = FALSE;
		}
	}
	if ( k > 0 ) ob_cls_obj(k);
	if ( a != NULL ) Kfree(a);
	if ( b != NULL ) Kfree(b);
	return same;
}

/*
 * Record recno made the entry's bytes. Up to SU_WHOLE they are written
 * in one change; a larger one is written a piece at a time, the record
 * cut to nothing first, so that a cut in the power leaves it shorter
 * than it is to be and the next start writes it again.
 */
LOCAL ER rec_put( CONST TS_UUID *u, INT recno, INT nrec, CONST SUENT *e )
{
	UB	*b;
	ID	k;
	UD	at;
	SZ	n, asz;
	INT	got;
	ER	er;

	if ( e->size <= SU_WHOLE ) {
		b = ent_bytes(e);
		if ( b == NULL ) return E_NOMEM;
		er = om_obj_record_put(u, recno, e->rt, e->sub, b, (SZ)e->size);
		Kfree(b);
		return er;
	}
	b = (UB *)Kmalloc(SU_CHUNK);
	if ( b == NULL ) return E_NOMEM;
	k = ob_opn_obj(u, OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRRD | OB_O_WEXCL);
	er = ( k > 0 ) ? E_OK : (ER)k;
	if ( er >= E_OK && recno == nrec ) {
		er = ob_apd_rec(k, e->rt, e->sub, &got);
	}
	if ( er >= E_OK ) er = ob_trn_rec(k, recno, 0);
	for ( at = 0; er >= E_OK && at < (UD)e->size; at += (UD)n ) {
		n = ( (UD)e->size - at > SU_CHUNK ) ? SU_CHUNK : (SZ)( (UD)e->size - at );
		er = pkg_read(su.key, (D)e->off + (D)at, b, n);
		asz = 0;
		if ( er >= E_OK ) er = ob_wri_rec(k, recno, (D)at, b, n, &asz);
	}
	if ( k > 0 ) ob_cls_obj(k);		/* closing waits for what was written to be on the medium */
	Kfree(b);
	return er;
}

/* A box given a link to each object the package's box links to and it does not */
LOCAL ER box_merge( CONST TS_UUID *box, CONST SUENT *e )
{
	UB	*rec, *cur;
	TS_UUID	*ids, *has = NULL;
	T_OBREF	r;
	SZ	csz = 0;
	INT	n, nh = 0, i, k, added = 0;
	ER	er = E_OK;

	rec = ent_bytes(e);
	if ( rec == NULL ) return E_NOMEM;
	n = om_store_links(rec, (SZ)e->size, NULL, 0);
	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( n + 1 ));
	cur = om_obj_record(box, 0, &csz);
	if ( cur != NULL ) {
		nh = om_store_links(cur, csz, NULL, 0);
		has = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( nh + 1 ));
		nh = ( has != NULL ) ? om_store_links(cur, csz, has, nh) : 0;
		Kfree(cur);
	}
	if ( ids == NULL || ( cur != NULL && has == NULL ) ) {
		er = E_NOMEM;
		n = 0;
	} else {
		n = om_store_links(rec, (SZ)e->size, ids, n);
	}
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		if ( ts_uuid_cmp(&ids[i], box) == 0 || ob_ref_obj(&ids[i], &r) < E_OK ) continue;
		for ( k = 0; k < nh && ts_uuid_cmp(&has[k], &ids[i]) != 0; k++ ) ;
		if ( k < nh ) continue;			/* linked already */
		er = om_obj_link_add(box, &ids[i]);
		if ( er >= E_OK ) added++;
	}
	if ( added > 0 ) {
		tm_printf((UB *)"TessronOS desktop: system update: %d links added to a box\n", added);
	}
	if ( ids != NULL ) Kfree(ids);
	if ( has != NULL ) Kfree(has);
	Kfree(rec);
	return er;
}

/*
 * Step 2 of an object: its records and resources. The system's own are
 * made the package's; one kept, or a box, gets only the records it does
 * not have yet (all of them when it was just made), and a box the links
 * it lacks.
 */
LOCAL ER obj_records( CONST SUOBJ *o )
{
	T_OBREC	*rec = NULL;
	SUENT	*e;
	INT	nrec, i, last = -1;
	ER	er = E_OK;

	for ( i = 0; i < su.n; i++ ) {
		if ( su.e[i].what > last && ts_uuid_cmp(&su.e[i].u, &o->u) == 0 ) last = su.e[i].what;
	}
	nrec = om_obj_records_all(&o->u, &rec);
	if ( nrec < 0 ) {
		return (ER)nrec;
	}
	for ( i = 0; i <= last && er >= E_OK; i++ ) {
		e = ent_of(&o->u, i);
		if ( e == NULL ) {
			er = E_OBJ;		/* the records are numbered from 0 without a gap */
			break;
		}
		if ( i < nrec && ( o->how != U_SYSTEM || rec_same(&o->u, i, rec[i].size, e) ) ) {
			continue;
		}
		er = rec_put(&o->u, i, ( i < nrec ) ? nrec : i, e);
	}
	if ( rec != NULL ) Kfree(rec);

	/* the resources: the system's own made the package's; another's only one it has not */
	for ( i = 0; i < su.n && er >= E_OK; i++ ) {
		UB	*want, *now;
		SZ	nsz = 0;

		e = &su.e[i];
		if ( e->what != E_RES || ts_uuid_cmp(&e->u, &o->u) != 0 ) continue;
		now = om_obj_res(&o->u, e->name + TS_UUID_STRLEN, &nsz);
		if ( now != NULL && o->how != U_SYSTEM ) {
			Kfree(now);
			continue;
		}
		want = ent_bytes(e);
		if ( want == NULL ) {
			er = E_NOMEM;
		} else if ( now == NULL || nsz != (SZ)e->size || !same_bytes(now, want, nsz) ) {
			er = om_obj_res_put(&o->u, e->name + TS_UUID_STRLEN, want, (SZ)e->size);
		}
		if ( want != NULL ) Kfree(want);
		if ( now != NULL ) Kfree(now);
	}

	if ( er >= E_OK && o->how == U_LINKS && ( e = ent_of(&o->u, 0) ) != NULL && e->rt == OB_RT_TAD ) {
		er = box_merge(&o->u, e);
	}
	return er;
}

/* ---------------------------------------------------------------- the boot files */

/* A path of the boot partition: the root, rel below it, and tail */
LOCAL void bpath( char *out, CONST char *rel, CONST char *tail )
{
	INT	n;

	knl_strcpy(out, su_root);
	n = (INT)knl_strlen(out);
	out[n++] = '/';
	for ( ; *rel != 0 && n < SU_PATH_MAX - 6; rel++ ) out[n++] = *rel;
	out[n] = 0;
	knl_strcat(out, tail);
}

/* Whether path is a file, its size in *p_size */
LOCAL BOOL f_size( CONST char *path, UD *p_size )
{
	T_FSTAT	st;

	if ( fs_stat(path, &st) < E_OK || ( st.mode & FS_IFMT ) != FS_IFREG ) return FALSE;
	if ( p_size != NULL ) *p_size = st.size;
	return TRUE;
}

/* rel there in any of its forms: PATH, or PATH.new, PATH.old an update left */
LOCAL BOOL f_any( CONST char *rel )
{
	char	p[SU_PATH_MAX];

	bpath(p, rel, "");
	if ( f_size(p, NULL) ) return TRUE;
	bpath(p, rel, ".new");
	if ( f_size(p, NULL) ) return TRUE;
	bpath(p, rel, ".old");
	return f_size(p, NULL);
}

/* Whether the file at path holds the text s, read a piece at a time */
LOCAL BOOL f_has( CONST char *path, CONST char *s )
{
	SZ	m = knl_strlen(s), have = 0, k;
	UB	*b;
	INT	fd, n;
	BOOL	got = FALSE;

	b = (UB *)Kmalloc(SU_CHUNK + m);
	fd = ( b != NULL ) ? fs_open(path, O_RDONLY) : -1;
	while ( fd >= 0 && !got && ( n = fs_read(fd, b + have, SU_CHUNK) ) > 0 ) {
		have += (SZ)n;
		for ( k = 0; k + m <= have && !got; k++ ) {
			got = same_bytes(b + k, (CONST UB *)s, m);
		}
		if ( have >= m ) {		/* the last m - 1 bytes kept: the text may go on into the next */
			for ( k = 0; k < m - 1; k++ ) b[k] = b[have - ( m - 1 ) + k];
			have = m - 1;
		}
	}
	if ( fd >= 0 ) fs_close(fd);
	if ( b != NULL ) Kfree(b);
	return got;
}

/* Whether the loader at rel is TessronOS's: PATH, or while an update has it, PATH.new */
LOCAL BOOL f_ours( CONST char *rel )
{
	char	p[SU_PATH_MAX];

	bpath(p, rel, "");
	if ( f_size(p, NULL) ) return f_has(p, LOADER_MARK);
	bpath(p, rel, ".new");
	if ( f_size(p, NULL) ) return f_has(p, LOADER_MARK);
	bpath(p, rel, ".old");
	return f_has(p, LOADER_MARK);
}

LOCAL void place( INT ent, CONST char *rel, BOOL near )
{
	SUBOOT	*b = &su.boot[su.nboot++];

	b->ent = ent;
	b->near = near;
	bpath(b->path, rel, "");
}

/*
 * Where each boot file of the package goes (the head of this file),
 * those the firmware or the kernel reads first, and then the steps
 * there are. The places are found the same way while an update is
 * under way, as a file between its two renames is still there as
 * PATH.new or PATH.old.
 */
LOCAL ER su_place( void )
{
	char	ts[SU_PATH_MAX], p[SU_PATH_MAX];
	BOOL	installed, near, any;
	INT	i, k, pass;

	if ( su.boot != NULL ) Kfree(su.boot);
	su.nboot = 0;
	su.boot = (SUBOOT *)Kmalloc(sizeof(SUBOOT) * (SZ)( 2 * su.n + 1 ));
	if ( su.boot == NULL ) {
		return E_NOMEM;
	}
	bpath(p, SU_INSTALL, "");
	installed = f_size(p, NULL);
	for ( pass = 0; pass < 2; pass++ ) {
		for ( i = 0; i < su.n; i++ ) {
			CONST char	*rel = (CONST char *)su.e[i].name + 5, *base = rel;

			if ( su.e[i].what != E_BOOT || !su.e[i].put ) continue;
			for ( k = 0; rel[k] != 0; k++ ) {
				if ( rel[k] == '/' ) base = rel + k + 1;
			}
			for ( k = 0; su_near[k] != NULL && !name_is((CONST UB *)rel, su_near[k]); k++ ) ;
			near = (BOOL)( su_near[k] != NULL );
			if ( near != (BOOL)( pass == 1 ) ) continue;
			knl_strcpy(ts, SU_TS_DIR);
			knl_strcat(ts, base);		/* a name of the table: shorter than PKG_NAME */

			if ( rel[0] == 'E' && rel[1] == 'F' && rel[2] == 'I' && rel[3] == '/' ) {
				/* the loader: each place that has TessronOS's, never another's */
				any = FALSE;
				for ( k = 0; k < 2; k++ ) {
					CONST char	*at = ( k == 0 ) ? ts : rel;

					if ( k == 1 && name_is((CONST UB *)rel, ts) ) break;
					if ( !f_any(at) ) continue;
					any = TRUE;
					if ( f_ours(at) ) {
						place(i, at, FALSE);
					} else {
						tm_printf((UB *)"TessronOS desktop: system update: %s/%s is not TessronOS's: left as it is\n",
							  su_root, at);
					}
				}
				if ( !any ) place(i, installed ? ts : rel, FALSE);
			} else if ( near ) {
				/* where the loader reads it from */
				place(i, f_any(ts) ? ts : f_any(rel) ? rel : installed ? ts : rel, TRUE);
			} else if ( !installed || f_any(rel) ) {
				place(i, rel, FALSE);
			}
		}
	}
	su.total = 2 * su.nobj + 1 + su.nboot + 1;
	return E_OK;
}

/* n bytes as the clusters of the boot partition they take */
LOCAL UD clus( UD n, UD bsize )
{
	return ( bsize > 0 ) ? ( n + bsize - 1 ) / bsize * bsize : n;
}

/*
 * The bytes of /boot the boot files take more than now: for each, the
 * new one, and the copy of the one there when it is put in place by a
 * copy, less what is deleted first (the PATH.old of the update before,
 * a PATH.new left). Taken at no less than nothing for each file, as
 * what one frees comes only before its own is written.
 */
LOCAL UD su_need( UD bsize )
{
	char	p[SU_PATH_MAX], pn[SU_PATH_MAX], po[SU_PATH_MAX];
	UD	need = 0, add, freed, sz;
	INT	i;

	for ( i = 0; i < su.nboot; i++ ) {
		CONST SUBOOT	*b = &su.boot[i];
		CONST SUENT	*e = &su.e[b->ent];
		BOOL		has;

		knl_strcpy(p, b->path);
		knl_strcpy(pn, b->path);
		knl_strcat(pn, ".new");
		knl_strcpy(po, b->path);
		knl_strcat(po, ".old");
		has = f_size(p, &sz);
		if ( e->keep && has ) continue;
		add = clus((UD)e->size, bsize) + bsize;	/* and room for its directory entries */
		if ( has && !b->near ) add += clus(sz, bsize);
		freed = 0;
		if ( f_size(pn, &sz) ) freed += clus(sz, bsize);
		if ( has && f_size(po, &sz) ) freed += clus(sz, bsize);
		if ( add > freed ) need += add - freed;
	}
	return need;
}

/* Whether the file at path is already the entry's bytes */
LOCAL BOOL file_same( CONST char *path, CONST SUENT *e )
{
	T_FSTAT	st;
	UB	*a, *b;
	UD	at;
	SZ	n;
	INT	fd;
	BOOL	same = TRUE;

	if ( fs_stat(path, &st) < E_OK || st.size != (UD)e->size ) return FALSE;
	a = (UB *)Kmalloc(SU_CHUNK);
	b = (UB *)Kmalloc(SU_CHUNK);
	fd = fs_open(path, O_RDONLY);
	if ( a == NULL || b == NULL || fd < 0 ) same = FALSE;
	for ( at = 0; same && at < (UD)e->size; at += (UD)n ) {
		n = ( (UD)e->size - at > SU_CHUNK ) ? SU_CHUNK : (SZ)( (UD)e->size - at );
		if ( fs_read(fd, a, n) != (INT)n || pkg_read(su.key, (D)e->off + (D)at, b, n) < E_OK
		  || !same_bytes(a, b, n) ) {
			same = FALSE;
		}
	}
	if ( fd >= 0 ) fs_close(fd);
	if ( a != NULL ) Kfree(a);
	if ( b != NULL ) Kfree(b);
	return same;
}

/* The directories a path of the boot partition is in, made when missing */
LOCAL void dirs_for( CONST char *path )
{
	char	d[SU_PATH_MAX];
	INT	i;

	for ( i = (INT)knl_strlen(su_root) + 1; path[i] != 0 && i < SU_PATH_MAX - 1; i++ ) {
		if ( path[i] == '/' ) {
			knl_memcpy(d, path, (SZ)i);
			d[i] = 0;
			(void)fs_mkdir(d);
		}
	}
}

/* The bytes of a file written to path whole, made sure to be on the medium */
LOCAL ER file_write( CONST char *path, ID from_key, D from_off, INT from_fd, UD size )
{
	UB	*b = (UB *)Kmalloc(SU_CHUNK);
	UD	at;
	SZ	n;
	INT	fd;
	ER	er = E_OK;

	if ( b == NULL ) return E_NOMEM;
	dirs_for(path);
	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		Kfree(b);
		return (ER)fd;
	}
	for ( at = 0; er >= E_OK && at < size; at += (UD)n ) {
		n = ( size - at > SU_CHUNK ) ? SU_CHUNK : (SZ)( size - at );
		if ( from_fd >= 0 ) {
			er = ( fs_read(from_fd, b, n) == (INT)n ) ? E_OK : E_IO;
		} else {
			er = pkg_read(from_key, from_off + (D)at, b, n);
		}
		if ( er >= E_OK && fs_write(fd, b, n) != (INT)n ) er = E_IO;
	}
	if ( er >= E_OK ) er = fs_fsync(fd);
	fs_close(fd);
	Kfree(b);
	return er;
}

/*
 * Step of a boot file: PATH.new written, unless PATH is already the new
 * one (or one kept is there). The PATH.old of the update before is
 * deleted first while PATH is there. A file the loader reads, left
 * between its two renames (no PATH): when PATH.new is the package's the
 * commit finishes it; otherwise PATH.old, the one the loader then
 * reads, is put back first, so that PATH.new is never written while it
 * is what the loader would read.
 */
LOCAL ER boot_stage( CONST SUBOOT *bt )
{
	CONST SUENT	*e = &su.e[bt->ent];
	char		p[SU_PATH_MAX], pn[SU_PATH_MAX], po[SU_PATH_MAX];
	ER		er;

	knl_strcpy(p, bt->path);
	knl_strcpy(pn, bt->path);
	knl_strcat(pn, ".new");
	knl_strcpy(po, bt->path);
	knl_strcat(po, ".old");
	if ( bt->near && !f_size(p, NULL) ) {
		if ( file_same(pn, e) ) {
			return E_OK;
		}
		if ( f_size(po, NULL) ) {
			er = fs_rename(po, p);
			if ( er < E_OK ) return er;
		}
	}
	if ( ( e->keep && f_size(p, NULL) ) || file_same(p, e) ) {
		(void)fs_unlink(pn);
		return E_OK;
	}
	if ( f_size(p, NULL) ) (void)fs_unlink(po);
	if ( file_same(pn, e) ) {
		return E_OK;
	}
	return file_write(pn, su.key, (D)e->off, -1, (UD)e->size);
}

/* A rename of the commit; the tests stop after so many, as a cut in the power would */
LOCAL ER commit_rename( CONST char *from, CONST char *to, INT *p_n )
{
	if ( su_cut >= 0 && *p_n >= su_cut ) {
		return E_IO;
	}
	(*p_n)++;
	return fs_rename(from, to);
}

/*
 * The last step: each PATH.new put in place. One the firmware or the
 * kernel reads: the one there copied to PATH.old, and PATH.new renamed
 * over PATH. One the UEFI loader reads: PATH renamed to PATH.old, then
 * PATH.new to PATH. They go in the order of su.boot, the first kind
 * first.
 */
LOCAL ER boot_commit( void )
{
	char	p[SU_PATH_MAX], pn[SU_PATH_MAX], po[SU_PATH_MAX];
	T_FSTAT	sn;
	UD	sz;
	INT	i, fd, moved = 0, renames = 0;
	ER	er = E_OK;

	(void)fs_sync();			/* everything before, on the medium first */
	for ( i = 0; i < su.nboot && er >= E_OK; i++ ) {
		CONST SUBOOT	*bt = &su.boot[i];
		CONST SUENT	*e = &su.e[bt->ent];

		knl_strcpy(p, bt->path);
		knl_strcpy(pn, bt->path);
		knl_strcat(pn, ".new");
		knl_strcpy(po, bt->path);
		knl_strcat(po, ".old");
		if ( fs_stat(pn, &sn) < E_OK ) continue;
		if ( sn.size != (UD)e->size ) {
			er = E_IO;		/* written short: the next start writes it again */
			break;
		}
		if ( f_size(p, &sz) ) {
			(void)fs_unlink(po);
			if ( bt->near ) {
				er = commit_rename(p, po, &renames);
			} else {
				fd = fs_open(p, O_RDONLY);
				er = ( fd >= 0 ) ? file_write(po, 0, 0, fd, sz) : (ER)fd;
				if ( fd >= 0 ) fs_close(fd);
			}
		}
		if ( er >= E_OK ) er = commit_rename(pn, p, &renames);
		if ( er >= E_OK ) moved++;
	}
	(void)fs_sync();
	tm_printf((UB *)"TessronOS desktop: system update: %d boot files put in place (%d)\n", moved, (INT)er);
	return er;
}

/* ---------------------------------------------------------------- the steps */

/* One step, the k-th of su.total */
LOCAL ER su_do( INT k )
{
	ER	er;
	INT	i;

	if ( k < su.nobj ) {
		return obj_meta(&su.obj[k]);
	}
	k -= su.nobj;
	if ( k < su.nobj ) {
		return obj_records(&su.obj[k]);
	}
	k -= su.nobj;
	if ( k == 0 ) {
		/* what nothing links to, held, so that it is not taken for a 屑実身 */
		for ( i = 0; i < su.nobj; i++ ) {
			if ( om_store_refs(&su.obj[i].u) == 0 ) (void)ob_lnk_obj(&su.obj[i].u);
		}
		return E_OK;
	}
	k -= 1;
	if ( k < su.nboot ) {
		return boot_stage(&su.boot[k]);
	}
	er = boot_commit();
	if ( er >= E_OK ) {
		(void)wm_dev_conf_put(SU_KEY, "");
		dt_prog_reread();
		tm_printf((UB *)"TessronOS desktop: system update to %s done\n", su.ver);
	}
	return er;
}

EXPORT ER dt_sysup_check( CONST TS_UUID *pkg, UB *text, INT max )
{
	ER	er = su_open(pkg, text, max);

	su_end();
	return er;
}

/* The package opened and the places of its boot files found; *p_need, *p_free the room of /boot */
LOCAL ER su_ready( CONST TS_UUID *pkg, BOOL force, UD *p_need, UD *p_free )
{
	T_FSSTAT vs;
	ER	er;

	*p_need = 0;
	*p_free = ~(UD)0;
	er = su_open(pkg, NULL, 0);
	if ( er == E_LIMIT && force ) er = E_OK;
	if ( er >= E_OK ) er = su_place();
	if ( er < E_OK ) {
		su_end();
		return er;
	}
	if ( su.nboot > 0 && fs_statvfs(su_root, &vs) >= E_OK ) {
		*p_need = su_need(vs.bsize);
		*p_free = vs.bfree * vs.bsize;
	}
	return E_OK;
}

EXPORT ER dt_sysup_need( CONST TS_UUID *pkg, UD *p_need, UD *p_free )
{
	ER	er;

	if ( su.on ) {
		return E_BUSY;
	}
	er = su_ready(pkg, TRUE, p_need, p_free);
	su_end();
	return er;
}

EXPORT void dt_sysup_test( CONST char *root, INT cut )
{
	knl_strncpy(su_root, ( root != NULL && root[0] != 0 ) ? root : "/boot", SU_ROOT_MAX - 1);
	su_root[SU_ROOT_MAX - 1] = 0;
	su_cut = cut;
}

EXPORT ER dt_sysup_begin( CONST TS_UUID *pkg, BOOL force, ID pid, INT *p_total )
{
	UB	us[TS_UUID_STRLEN + 1];
	UD	need, room;
	ER	er;

	if ( su.on ) {
		return E_BUSY;
	}
	er = su_ready(pkg, force, &need, &room);
	if ( er < E_OK ) {
		return er;
	}
	if ( room < need ) {
		tm_printf((UB *)"TessronOS desktop: system update: %s has %d KB free, %d KB needed\n",
			  su_root, (INT)( room / 1024 ), (INT)( need / 1024 ));
		su_end();
		return E_NOMEM;
	}
	(void)ts_uuid_to_str(pkg, (char *)us, sizeof(us));
	er = wm_dev_conf_put(SU_KEY, (CONST char *)us);
	if ( er < E_OK ) {
		su_end();
		return er;
	}
	su.on = TRUE;
	su.pid = pid;
	tm_printf((UB *)"TessronOS desktop: system update to %s begun, %d objects, %d boot files\n",
		  su.ver, su.nobj, su.nboot);
	if ( p_total != NULL ) *p_total = su.total;
	return E_OK;
}

EXPORT ER dt_sysup_step( ID pid, INT *p_done, INT *p_total )
{
	UD	t0 = 0, t = 0;
	ER	er = E_OK;

	if ( !su.on ) {
		return E_NOEXS;
	}
	if ( pid != su.pid ) {
		return E_OACV;
	}
	(void)ts_get_mono(&t0);
	do {
		er = su_do(su.done);
		if ( er < E_OK ) {
			tm_printf((UB *)"TessronOS desktop: system update: step %d of %d failed (%d)\n",
				  su.done, su.total, (INT)er);
			break;
		}
		su.done++;
		(void)ts_get_mono(&t);
	} while ( su.done < su.total && t - t0 < SU_STEP_NS );
	if ( p_done != NULL ) *p_done = su.done;
	if ( p_total != NULL ) *p_total = su.total;
	if ( er < E_OK || su.done >= su.total ) {
		su_end();			/* failed: SYSUP stays, and the next start does it again */
	}
	return er;
}

EXPORT ER dt_sysup_run( CONST TS_UUID *pkg, BOOL force )
{
	INT	done = 0, total = 0;
	ER	er = dt_sysup_begin(pkg, force, 0, &total);

	while ( er >= E_OK && done < total ) {
		er = dt_sysup_step(0, &done, &total);
	}
	return er;
}

EXPORT ER dt_sysup_req( CONST T_DTREQ *rq, T_DTANS *an, ID pid )
{
	INT	done = 0, total = 0;
	ER	er;

	switch ( rq->recno ) {
	case DT_SYSUP_CHECK:
		return dt_sysup_check(&rq->target, an->text, sizeof(an->text));
	case DT_SYSUP_BEGIN:
		if ( su.on ) {
			return E_BUSY;
		}
		if ( !knl_ob_pid_admin(pid) ) {
			er = dt_admin_ask(pid, (CONST UB *)"システムの更新", &rq->win);
			if ( er < E_OK ) return er;
		}
		er = dt_sysup_begin(&rq->target, (BOOL)( rq->flags & 1 ), pid, &total);
		an->how = (UINT)total;
		return er;
	case DT_SYSUP_STEP:
		er = dt_sysup_step(pid, &done, &total);
		an->how = (UINT)done;
		an->flags = (UINT)total;
		return er;
	default:
		return E_PAR;
	}
}

/*
 * At the start: an update the machine's settings say was begun and not
 * ended (the power went, or the system stopped) is done again from its
 * first step, and the person is told to start the system again.
 */
EXPORT void dt_sysup_resume( DTWIN *d )
{
	char	v[TS_UUID_STRLEN + 8];
	TS_UUID	pkg;
	T_OBREF	r;
	INT	done = 0, total = 0;
	ER	er;

	if ( !wm_dev_conf_get(SU_KEY, v, sizeof(v)) || v[0] == 0 ) {
		return;
	}
	if ( ts_str_to_uuid(v, &pkg) < E_OK || ob_ref_obj(&pkg, &r) < E_OK ) {
		tm_printf((UB *)"TessronOS desktop: system update: the package %s is gone\n", v);
		(void)wm_dev_conf_put(SU_KEY, "");
		if ( d != NULL ) {
			dt_tell(d, "システムの更新が途中で止まっていますが、パッケージが見つかりません。",
				"システムのパッケージを〈バージョン〉にもう一度ドロップしてください。");
		}
		return;
	}
	tm_printf((UB *)"TessronOS desktop: system update: resumed\n");
	er = dt_sysup_begin(&pkg, TRUE, 0, &total);
	if ( er == E_OBJ || er == E_NOSPT ) {
		/* not a package to be put on here (broken, or another machine's): no start does better */
		tm_printf((UB *)"TessronOS desktop: system update: the package %s cannot be put on (%d)\n", v, (INT)er);
		(void)wm_dev_conf_put(SU_KEY, "");
		if ( d != NULL ) {
			dt_tell(d, "システムの更新が途中で止まっていますが、パッケージを読めません。",
				"システムのパッケージを〈バージョン〉にもう一度ドロップしてください。");
		}
		return;
	}
	while ( er >= E_OK && done < total ) {
		er = dt_sysup_step(0, &done, &total);
	}
	if ( d == NULL ) {
		return;
	}
	if ( er >= E_OK ) {
		dt_tell(d, "途中で止まっていたシステムの更新を終えました。",
			"システムを再起動すると新しいバージョンになります。");
	} else {
		dt_tell(d, "途中で止まっていたシステムの更新を続けられませんでした。",
			"次に起動したときにもう一度続けます。");
	}
}
