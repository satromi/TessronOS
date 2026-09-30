/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtprog.c
 *	The programs of the system as real objects: the program box, the
 *	原紙箱 and the 小物箱 (design 18.12)
 *
 *	A program is a real object whose metadata says what it is:
 *	  "tessronos": { "program": { "id": "basic-text-editor", "kind": "app",
 *		"name": "基本文章編集", "base": [ UUID ], "builtin": true } }
 *	The id is the one an applist names, so what a TADjs Desktop wrote
 *	opens with the program it names. "kind" is app (it has templates,
 *	原紙, and opens objects), accessory (小物: no data of its own) or
 *	utility (a tool of the system). A program the desktop carries in
 *	itself is "builtin" and has no executable; one that is a process has
 *	its executable in a record of type 9 (tessronos.exec).
 *
 *	The program box is the list of programs: an object of the store
 *	whose record links to each program object. The system's programs
 *	come as objects in the store (etc/def), the clock with its
 *	executable as record 1; nothing here makes them. What the 実行 menu
 *	names is looked up in the box, the 原紙箱 shows the templates of its
 *	applications, and the 小物箱 its accessories.
 *
 *	The parts box (部品箱) is made the same way: definitions of the
 *	standard parts -- a button, a switch, a box of letters, a panel, a
 *	menu -- each an object whose metadata says what it is,
 *	  "tessronos": { "part": { "sub": "part", "kind": 6, "rect": [0,0,80,24] } }
 *	and a program has a part of its own by copying one (ob_cpy_obj)
 *	and placing the copy (ob_map_rec, design 18.13).
 *
 *	A TADjs plugin -- a directory with its plugin.json and the files of
 *	its template -- is taken in as a program object and a template
 *	object (dt_plugin_import, design 18.12). Its code is HTML for the
 *	browser and does not run here, so the program has no executable:
 *	what it brings is its name, its id for the applist, and its
 *	template, which is taken in by the common module (xf_import_tadjs,
 *	design 18.20). Every plugin directory under /boot/PLUGINS is taken
 *	in when the desktop starts.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/part.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/tad.h>
#include <ts/tadview.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/proc.h>
#include <ts/fs.h>
#include <ts/json.h>
#include <ts/xf.h>
#include <ts/sysdef.h>
#include "desktop.h"

#define PROG_MAX	32

LOCAL DTPROG	prog[PROG_MAX];
LOCAL INT	nprog = 0;

/* ---------------------------------------------------------------- text */

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( *s != 0 && at + 1 < max ) {
		b[at++] = (UB)*s++;
	}
	b[at] = 0;
	return at;
}

/* Text inside a JSON string */
LOCAL INT put_json( UB *b, INT at, INT max, CONST UB *s )
{
	for ( ; *s != 0 && at + 2 < max; s++ ) {
		if ( *s == '"' || *s == '\\' ) b[at++] = '\\';
		if ( *s >= 0x20 ) b[at++] = *s;
	}
	b[at] = 0;
	return at;
}

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* ---------------------------------------------------------------- what a program says */

/* A program object's metadata read into an entry; FALSE when it is not a program */
LOCAL BOOL read_prog( CONST TS_UUID *uuid, DTPROG *p )
{
	T_JSON	root, tf, g, base, it;
	UB	*meta, s[TAD_NAME_MAX];
	SZ	size = 0;
	BOOL	ok;

	meta = om_obj_meta(uuid, &size);
	if ( meta == NULL ) {
		return FALSE;
	}
	ok = (BOOL)( js_parse(meta, (INT)size, &root) >= E_OK
		  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "program", &g) >= E_OK
		  && js_get_str(&g, "id", p->id, DT_PROG_ID) > 0 );
	if ( ok ) {
		p->uuid = *uuid;
		if ( js_get_str(&g, "name", p->name, TAD_NAME_MAX) <= 0 ) {
			(void)js_get_str(&root, "name", p->name, TAD_NAME_MAX);
		}
		p->kind = DT_PK_APP;
		if ( js_get_str(&g, "kind", s, sizeof(s)) > 0 ) {
			p->kind = same(s, "accessory") ? DT_PK_ACCESSORY
				: same(s, "utility") ? DT_PK_UTILITY : DT_PK_APP;
		}
		if ( js_get_str(&g, "version", p->version, DT_VER_MAX) <= 0 ) {
			p->version[0] = 0;
		}
		p->builtin = js_get_bool(&g, "builtin", FALSE);
		p->menu = js_get_bool(&g, "menu", FALSE);

		/* its templates */
		p->nbase = 0;
		if ( js_get(&g, "base", &base) >= E_OK ) {
			for ( it.s = NULL; p->nbase < DT_PROG_BASE && js_next(&base, &it); ) {
				UB	us[TS_UUID_STRLEN + 1];

				if ( js_str(&it, us, sizeof(us)) == TS_UUID_STRLEN
				  && ts_str_to_uuid((CONST char *)us, &p->base[p->nbase]) >= E_OK ) {
					p->nbase++;
				}
			}
		}
	}
	Kfree(meta);
	return ok;
}

/* ---------------------------------------------------------------- the list */

/* The programs the box links to, read into the list */
LOCAL void read_box( void )
{
	TS_UUID	u, *lk;
	INT	n = 0, i;

	nprog = 0;
	if ( ts_str_to_uuid(SYSDEF_PROG_BOX, &u) < E_OK ) {
		return;
	}
	lk = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * PROG_MAX);
	if ( lk != NULL ) {
		if ( ob_lst_lnk(&u, NULL, lk, PROG_MAX, &n) < E_OK ) n = 0;
		if ( n > PROG_MAX ) n = PROG_MAX;
		for ( i = 0; i < n && nprog < PROG_MAX; i++ ) {
			if ( read_prog(&lk[i], &prog[nprog]) ) {
				nprog++;
			}
		}
		Kfree(lk);
	}
}

/* ---------------------------------------------------------------- TADjs plugins */

#define PLUGIN_DIR	"/boot/PLUGINS"
#define PLUGIN_JSON_MAX	( 64 * 1024 )

/* A path made of a directory and a name */
LOCAL void path_of( char *out, INT max, CONST char *dir, CONST UB *name )
{
	INT	n = put((UB *)out, 0, max, dir);

	n = put((UB *)out, n, max, "/");
	(void)put((UB *)out, n, max, (CONST char *)name);
}

/* plugin.json, read whole; NULL when it is not there */
LOCAL UB *plugin_json( CONST char *dir, SZ *p_size )
{
	char	path[FS_PATH_MAX];
	T_FSTAT	st;
	UB	*b;
	INT	fd, n;
	SZ	at = 0;

	path_of(path, sizeof(path), dir, (CONST UB *)"plugin.json");
	if ( fs_stat(path, &st) < EX_OK || st.size > PLUGIN_JSON_MAX ) {
		return NULL;
	}
	b = (UB *)Kmalloc((SZ)st.size + 1);
	fd = ( b != NULL ) ? fs_open(path, O_RDONLY) : -1;
	if ( fd < 0 ) {
		if ( b != NULL ) Kfree(b);
		return NULL;
	}
	while ( at < (SZ)st.size && ( n = fs_read(fd, b + at, (SZ)st.size - at) ) > 0 ) {
		at += n;
	}
	fs_close(fd);
	b[at] = 0;
	*p_size = at;
	return b;
}

/*
 * The program's metadata: what plugin.json says, as a program, and in
 * "tessronos.file" where it came from.
 */
LOCAL INT plugin_meta( UB *m, INT max, CONST char *dir, CONST UB *id, CONST UB *name,
		       CONST char *kind, CONST UB *ver, CONST TS_UUID *base )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n = 0;

	n = put(m, n, max, "{\"name\":\"");
	n = put_json(m, n, max, name);
	n = put(m, n, max, "\",\"relationship\":[],\"linktype\":false,"
			   "\"refCount\":1,\"recordCount\":0,\"editable\":false,"
			   "\"deletable\":false,\"readable\":true,\"maker\":\"TADjs plugin\","
			   "\"applist\":{},\"tessronos\":{\"program\":{\"id\":\"");
	n = put_json(m, n, max, id);
	n = put(m, n, max, "\",\"kind\":\"");
	n = put(m, n, max, kind);
	n = put(m, n, max, "\",\"name\":\"");
	n = put_json(m, n, max, name);
	n = put(m, n, max, "\",\"builtin\":false,\"base\":[");
	if ( base != NULL && ts_uuid_to_str(base, us, sizeof(us)) >= E_OK ) {
		n = put(m, n, max, "\"");
		n = put(m, n, max, us);
		n = put(m, n, max, "\"");
	}
	n = put(m, n, max, "]},\"plugin\":{\"version\":\"");
	n = put_json(m, n, max, ver);
	n = put(m, n, max, "\"},\"file\":{\"name\":\"");
	n = put_json(m, n, max, (CONST UB *)dir);
	n = put(m, n, max, "/plugin.json\",\"mediatype\":\"application/json\",\"source\":\"boot\"}}}");
	return ( n < max - 1 ) ? n : -1;
}

EXPORT ER dt_plugin_import( CONST char *dir, TS_UUID *p_prog )
{
	T_JSON		root, bf;
	UB		id[DT_PROG_ID], name[TAD_NAME_MAX], type[32], ver[32], jf[128];
	UB		*j, *m;
	SZ		size = 0;
	INT		n;
	TS_UUID		base, prog, box;
	CONST DTPROG	*have;
	CONST char	*kind;
	BOOL		has_base = FALSE;
	T_OBCRE		c;
	T_OBPRT		prt;
	ER		er = E_OK;

	if ( dir == NULL ) {
		return E_PAR;
	}
	j = plugin_json(dir, &size);
	if ( j == NULL ) {
		return E_NOEXS;
	}
	if ( js_parse(j, (INT)size, &root) < E_OK
	  || js_get_str(&root, "id", id, sizeof(id)) <= 0
	  || js_get_str(&root, "name", name, sizeof(name)) <= 0 ) {
		Kfree(j);
		return E_PAR;
	}
	if ( js_get_str(&root, "type", type, sizeof(type)) <= 0 ) type[0] = 0;
	if ( js_get_str(&root, "version", ver, sizeof(ver)) <= 0 ) ver[0] = 0;
	dt_sys_prt(&prt);
	kind = same(type, "base") ? "app" : same(type, "accessory") ? "accessory" : "utility";

	/* its template: a TADjs object named by its UUID */
	if ( js_get(&root, "basefile", &bf) >= E_OK && js_type(&bf) == JS_OBJECT ) {
		char	us[TS_UUID_STRLEN + 1];
		INT	k;

		er = ( js_get_str(&bf, "json", jf, sizeof(jf)) >= TS_UUID_STRLEN ) ? E_OK : E_PAR;
		for ( k = 0; er >= E_OK && k < TS_UUID_STRLEN; k++ ) us[k] = (char)jf[k];
		us[TS_UUID_STRLEN] = 0;
		if ( er >= E_OK && ts_str_to_uuid(us, &base) < E_OK ) er = E_PAR;
		if ( er >= E_OK ) er = xf_import_tadjs(dir, &base, "boot", &prt, NULL);
		has_base = (BOOL)( er >= E_OK );
	}
	Kfree(j);
	if ( er < E_OK ) {
		return er;
	}

	/* a program the system has by that id already is left as it is */
	have = dt_prog_find(id);
	if ( have != NULL ) {
		if ( p_prog != NULL ) *p_prog = have->uuid;
		return E_OK;
	}

	m = (UB *)Kmalloc(1024 + FS_PATH_MAX);
	if ( m == NULL ) {
		return E_NOMEM;
	}
	n = plugin_meta(m, 1024 + FS_PATH_MAX, dir, id, name, kind, ver, has_base ? &base : NULL);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = m;
	c.jsonsz = ( n > 0 ) ? n : 0;
	c.prt = &prt;
	if ( has_base ) {
		c.near = base;				/* beside its template */
	}
	er = ( n > 0 ) ? ob_cre_obj(&c, &prog) : E_LIMIT;
	Kfree(m);
	if ( er >= E_OK ) {
		er = ( ts_str_to_uuid(SYSDEF_PROG_BOX, &box) >= E_OK ) ? om_obj_link_add(&box, &prog)
								      : E_SYS;
	}
	if ( er >= E_OK ) {
		read_box();				/* the registry sees it now */
		if ( p_prog != NULL ) *p_prog = prog;
	}
	return er;
}

/* Every plugin directory under PLUGIN_DIR taken in */
LOCAL void plugins_import( void )
{
	T_DIRENT	*de;
	char		path[FS_PATH_MAX];
	INT		fd, cnt, i;

	fd = fs_open(PLUGIN_DIR, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) {
		return;
	}
	de = (T_DIRENT *)Kmalloc(sizeof(T_DIRENT) * 8);
	while ( de != NULL && ( cnt = fs_getdents(fd, de, 8) ) > 0 ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( ( de[i].mode & FS_IFDIR ) == 0 || de[i].name[0] == '.' ) {
				continue;
			}
			path_of(path, sizeof(path), PLUGIN_DIR, de[i].name);
			if ( dt_plugin_import(path, NULL) < E_OK ) {
				tm_printf((UB *)"dtprog: the plugin %s was not taken in\n", de[i].name);
			}
		}
	}
	fs_close(fd);
	if ( de != NULL ) Kfree(de);
}

EXPORT void dt_prog_reread( void )
{
	read_box();
}

EXPORT ER dt_prog_start( void )
{
	read_box();
	plugins_import();
	tm_printf((UB *)"TessronOS desktop: %d programs\n", nprog);

	return ( nprog > 0 ) ? E_OK : E_NOEXS;
}

EXPORT CONST DTPROG *dt_prog_find( CONST UB *id )
{
	INT	i;

	for ( i = 0; id != NULL && i < nprog; i++ ) {
		if ( same(prog[i].id, (CONST char *)id) ) {
			return &prog[i];
		}
	}
	return NULL;
}

EXPORT CONST DTPROG *dt_prog_of( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; uuid != NULL && i < nprog; i++ ) {
		if ( ts_uuid_cmp(&prog[i].uuid, uuid) == 0 ) {
			return &prog[i];
		}
	}
	return NULL;
}

EXPORT INT dt_prog_list( UINT kind, CONST DTPROG **out, INT max )
{
	INT	i, n = 0;

	for ( i = 0; i < nprog && n < max; i++ ) {
		if ( kind == 0 || prog[i].kind == kind ) {
			out[n++] = &prog[i];
		}
	}
	return n;
}

/*
 * A box's page: the 原紙箱 shows the templates of the applications,
 * each under its program's name; the 小物箱 the accessories themselves.
 */
EXPORT T_TAD *dt_prog_page( UINT kind )
{
	TS_UUID		ids[PROG_MAX * DT_PROG_BASE];
	CONST char	*names[PROG_MAX * DT_PROG_BASE];
	INT		i, k, n = 0;

	for ( i = 0; i < nprog; i++ ) {
		if ( prog[i].kind != kind ) continue;
		if ( kind == DT_PK_APP ) {
			for ( k = 0; k < prog[i].nbase; k++ ) {
				ids[n] = prog[i].base[k];
				names[n++] = (CONST char *)prog[i].name;
			}
		} else {
			ids[n] = prog[i].uuid;
			names[n++] = (CONST char *)prog[i].name;
		}
	}
	return dt_list_record(( kind == DT_PK_APP ) ? "原紙箱" : "小物箱", ids, names, n);
}

/*
 * What the system holds that no record links to: the システム箱, which
 * links to every box of the system, and the templates of the programs
 */
EXPORT INT dt_prog_roots( TS_UUID *ids, INT max )
{
	INT	i, k, n = 0;

	if ( n < max && ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &ids[n]) >= E_OK ) n++;
	for ( i = 0; i < nprog; i++ ) {
		for ( k = 0; k < prog[i].nbase && n < max; k++ ) {
			ids[n++] = prog[i].base[k];
		}
	}
	return n;
}

/* The name a template is shown by: its program's; NULL for what is not one */
EXPORT CONST char *dt_prog_base_name( CONST TS_UUID *target )
{
	INT	i, k;

	for ( i = 0; i < nprog; i++ ) {
		for ( k = 0; k < prog[i].nbase; k++ ) {
			if ( ts_uuid_cmp(&prog[i].base[k], target) == 0 ) {
				return (CONST char *)prog[i].name;
			}
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- running one */

/*
 * A process is started by a task of its own, not by the desktop's.
 * Making it reads the whole program into the new space, and a large
 * program (the browser is over 100MB) takes seconds to read from an SD
 * card; the desktop's task is the one that follows the pointer and
 * moves the windows, and would stand still for all that time. The
 * desktop hands the start over and goes on; the starter makes the
 * process object, answers the process that asked for it when one did,
 * and says on the message line when the program could not be started.
 * One starter takes the starts in the order they were asked for.
 */
#define START_QLEN	8		/* starts waiting for the starter */
#define START_PRI	21		/* just below the desktop, which has input to follow */
#define START_STKSZ	(32 * 1024)

typedef struct {
	TS_UUID	prog;			/* the program object */
	TS_UUID	arg;			/* the object to open, when has_arg */
	BOOL	has_arg;
	TS_UUID	reply;			/* where to answer; 0 for nobody */
	UINT	seq;
	SZ	anssz;			/* how much of a T_DTANS the asker reads */
	BOOL	with_er;		/* the answer says whether the start failed */
	UB	name[TAD_NAME_MAX];	/* the program, for the message line */
} DTSTART;

LOCAL ID	start_mbf = 0;
LOCAL ID	start_tsk = 0;

/* The answer the next start takes over (dt_prog_reply) */
LOCAL struct {
	BOOL	on;
	BOOL	taken;
	TS_UUID	reply;
	UINT	seq;
	SZ	size;
	BOOL	with_er;
} start_reply;

EXPORT void dt_prog_reply( CONST TS_UUID *reply, UINT seq, SZ size, BOOL with_er )
{
	knl_memset(&start_reply, 0, sizeof(start_reply));
	if ( reply != NULL ) {
		start_reply.on = TRUE;
		start_reply.reply = *reply;
		start_reply.seq = seq;
		start_reply.size = size;
		start_reply.with_er = with_er;
	}
}

EXPORT BOOL dt_prog_reply_taken( void )
{
	return start_reply.taken;
}

/* The process object made from the program, given the object to open (its UUID as the start-up argument) */
LOCAL ER start_now( CONST DTSTART *s, TS_UUID *pu )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = s->prog;
	if ( s->has_arg ) {
		c.arg = &s->arg;
		c.argsz = sizeof(s->arg);
	}
	return ob_cre_obj(&c, pu);
}

/*
 * A start done: the asker answered with the process, and a start that
 * failed told on the message line and the console, unless the answer
 * already says so to the one who asked.
 */
LOCAL void start_done( CONST DTSTART *s, ER er, CONST TS_UUID *pu )
{
	T_DTANS	an;
	TS_UUID	none;
	BOOL	asked;

	knl_memset(&none, 0, sizeof(none));
	asked = (BOOL)( ts_uuid_cmp(&s->reply, &none) != 0 );
	if ( asked ) {
		knl_memset(&an, 0, sizeof(an));
		an.seq = s->seq;
		an.er = s->with_er ? er : E_OK;
		if ( er >= E_OK && pu != NULL ) {
			an.proc = *pu;
		}
		dt_answer(&s->reply, &an, s->anssz);
	}
	if ( er < E_OK && !( asked && s->with_er ) ) {
		UB	msg[TAD_NAME_MAX + 48];
		INT	n = put(msg, 0, sizeof(msg), (CONST char *)s->name);

		(void)put(msg, n, sizeof(msg), "を起動できませんでした");
		(void)wm_msg_put((CONST char *)msg);
		tm_printf((UB *)"TessronOS desktop: %s not started (%d)\n", s->name, (INT)er);
	}
}

LOCAL void start_task( INT stacd, void *exinf )
{
	DTSTART	s;
	TS_UUID	pu;
	ER	er;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		if ( tk_rcv_mbf(start_mbf, &s, TMO_FEVR) != (INT)sizeof(s) ) {
			continue;
		}
		knl_memset(&pu, 0, sizeof(pu));
		er = start_now(&s, &pu);
		start_done(&s, er, &pu);
	}
}

/* The starter, made the first time it is wanted; FALSE when it cannot be */
LOCAL BOOL starter( void )
{
	T_CMBF	cmbf;
	T_CTSK	ctsk;
	ID	id;

	if ( start_tsk > 0 ) {
		return TRUE;
	}
	if ( start_mbf <= 0 ) {
		knl_memset(&cmbf, 0, sizeof(cmbf));
		cmbf.mbfatr = TA_TFIFO;
		cmbf.bufsz = (SZ)( START_QLEN * ( sizeof(DTSTART) + sizeof(W) ) );
		cmbf.maxmsz = (INT)sizeof(DTSTART);
		id = tk_cre_mbf(&cmbf);
		if ( id <= 0 ) {
			return FALSE;
		}
		start_mbf = id;
	}
	knl_memset(&ctsk, 0, sizeof(ctsk));
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)start_task;
	ctsk.itskpri = START_PRI;
	ctsk.stksz = START_STKSZ;
	id = tk_cre_tsk(&ctsk);
	if ( id <= 0 ) {
		return FALSE;
	}
	if ( tk_sta_tsk(id, 0) < E_OK ) {
		tk_del_tsk(id);
		return FALSE;
	}
	start_tsk = id;
	return TRUE;
}

/* A process started from the program: handed to the starter, or started here when there is none */
LOCAL ER start_process( CONST DTPROG *p, CONST T_VOBJ *v )
{
	DTSTART	s;
	TS_UUID	pu;
	INT	i;
	ER	er;

	knl_memset(&s, 0, sizeof(s));
	s.prog = p->uuid;
	if ( v != NULL ) {
		s.arg = v->target;
		s.has_arg = TRUE;
	}
	for ( i = 0; i < TAD_NAME_MAX - 1 && p->name[i] != 0; i++ ) {
		s.name[i] = p->name[i];
	}
	if ( start_reply.on && !start_reply.taken ) {
		s.reply = start_reply.reply;
		s.seq = start_reply.seq;
		s.anssz = start_reply.size;
		s.with_er = start_reply.with_er;
		start_reply.taken = TRUE;
	}
	if ( starter() ) {
		er = tk_snd_mbf(start_mbf, &s, (INT)sizeof(s), TMO_POL);
		if ( er >= E_OK ) {
			return E_OK;
		}
		if ( er != E_TMOUT ) {
			start_done(&s, er, NULL);
			return er;
		}
	}
	/* no starter, or it has more than it can hold: started here */
	knl_memset(&pu, 0, sizeof(pu));
	er = start_now(&s, &pu);
	start_done(&s, er, &pu);
	return er;
}

/*
 * A program run: one of the desktop's own by what it is, any other as a
 * process object made from it, given the object to open (its UUID as
 * the start-up argument) when there is one.
 */
EXPORT ER dt_prog_run( DTWIN *from, CONST DTPROG *p, CONST T_VOBJ *v )
{
	if ( p == NULL ) {
		return E_PAR;
	}
	if ( !p->builtin ) {
		return start_process(p, v);
	}
	if ( same(p->id, "base-file-manager") || same(p->id, "accessory-box") ) {
		T_DPRECT	o;
		UINT		kind = same(p->id, "base-file-manager") ? DT_PK_APP : DT_PK_ACCESSORY;

		o.left = 120;  o.top = 90;  o.right = 520;  o.bottom = 420;
		return ( dt_open_made(dt_prog_page(kind), (CONST char *)p->name,
				      DT_TOOL_NONE, &o) != NULL ) ? E_OK : E_NOMEM;
	}
	if ( same(p->id, "trash-real-objects") ) {
		dt_trash_open();
		return E_OK;
	}
	if ( same(p->id, "real-object-search") ) {
		dt_search_open(from);
		return E_OK;
	}
	if ( same(p->id, "virtual-object-network") ) {
		if ( v == NULL ) return E_PAR;
		dt_network_open(from, v);
		return E_OK;
	}
	/* an editor: the object opened in a window of that kind */
	if ( v == NULL ) {
		return E_PAR;
	}
	return dt_open_as(from, v, p->id);
}
