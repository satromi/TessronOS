/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	con_box.c
 *	コンソール: the place it is in (design 16.5.19)
 *
 *	The place is a real object whose record 0 links to others -- any
 *	such object, a box or a plain one -- and the way it was reached: the
 *	objects from "/" (SYSDEF_ROOT) down to it, kept as it goes, so that
 *	".." goes back up the way it came. It starts in bin (SYSDEF_BIN), a
 *	plain object "/" links to. A path names the links one after another
 *	by their objects' names, '/' between them; one that starts with '/'
 *	starts at "/"; ".." is the one before, "." this one; a
 *	UUID names that object itself. '\' takes the
 *	next letter as it is, a '/' or a space in a name among them. Names
 *	are compared with the full-width forms of ASCII taken as ASCII, as
 *	they are typed in either.
 */

#include "console.h"
#include <ts/json.h>

#define DEPTH_MAX	32
#define LINKS_MAX	256
#define SHORT_ID	8		/* hex digits a UUID is shown by when there is no name */

/* What an object is, for the list and for finding a program */
#define K_OTHER		0
#define K_BOX		1
#define K_PROG		2

typedef struct {
	TS_UUID	u;
	UINT	type;
	UB	name[OB_NAME_MAX];
	UINT	kind;			/* K_* */
	CONST char *label;		/* what it is, in words */
	BOOL	exec;			/* a program with an executable */
	INT	execrec;		/* the record that holds it */
	INT	nrec;
	UD	size;
	BOOL	noatr;			/* the caller may not read its metadata */
	BOOL	nodata;			/* the caller may not read its records */
} ENT;

LOCAL TS_UUID	way[DEPTH_MAX];		/* way[0] "/", way[depth - 1] here */
LOCAL INT	depth;
LOCAL TS_UUID	sysbox, binobj;
LOCAL UB	here[OB_NAME_MAX];

LOCAL TS_UUID	links[LINKS_MAX];	/* kept off the stack: a program's is small */

/* ---------------------------------------------------------------- small helpers */

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a->b[i] != b->b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL INT uuid_text( CONST TS_UUID *u, UB *out, INT digits )
{
	static CONST char hex[] = "0123456789abcdef";
	INT	i, n = 0;

	for ( i = 0; i < 16 && n < digits + 4; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = (UB)hex[u->b[i] >> 4];
		out[n++] = (UB)hex[u->b[i] & 15];
		if ( digits == SHORT_ID && n >= SHORT_ID ) break;
	}
	out[n] = 0;
	return n;
}

/* A word that is a UUID, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
LOCAL BOOL is_uuid( CONST UB *s, TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < TS_UUID_STRLEN; i++ ) {
		UB	c = s[i];
		BOOL	dash = (BOOL)( i == 8 || i == 13 || i == 18 || i == 23 );

		if ( dash ? ( c != '-' )
			  : !( ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' ) ) ) {
			return FALSE;
		}
	}
	return (BOOL)( s[i] == 0 && lt_uuid((CONST char *)s, u) >= E_OK );
}

LOCAL INT ustrlen( CONST UB *s )
{
	return (INT)strlen((CONST char *)s);
}

LOCAL void ucopy( UB *d, CONST UB *s, INT max )
{
	INT	i;

	for ( i = 0; s[i] != 0 && i < max - 1; i++ ) d[i] = s[i];
	d[i] = 0;
}

/* The name of an object as it is shown: its own, or the head of its UUID when it has none */
LOCAL void shown_name( CONST ENT *e, UB *out, INT max )
{
	if ( e->name[0] != 0 ) {
		ucopy(out, e->name, max);
	} else {
		out[0] = '(';
		(void)uuid_text(&e->u, out + 1, SHORT_ID);
		ucopy(out + 1 + SHORT_ID, (CONST UB *)"…)", max - 1 - SHORT_ID);
	}
}

/* ---------------------------------------------------------------- what an object is */

/*
 * What the metadata says, a member at a time (ob_get_atv): the console
 * keeps no memory to read the metadata whole into, however long it is.
 * Whether the member is there; a string of it into out (without its
 * quotes, "" when it is longer than out has room for); a number of it.
 */
LOCAL BOOL atv_has( ID key, CONST char *path )
{
	SZ	asz = 0;

	return (BOOL)( ob_get_atv(key, (CONST UB *)path, NULL, 0, &asz) >= E_OK );
}

LOCAL BOOL atv_str( ID key, CONST char *path, UB *out, INT max )
{
	UB	b[64];
	SZ	asz = 0;
	INT	i;

	out[0] = 0;
	if ( ob_get_atv(key, (CONST UB *)path, b, sizeof(b), &asz) < E_OK
	  || asz < 2 || asz > (SZ)sizeof(b) || b[0] != '"' || b[asz - 1] != '"' || asz - 2 >= max ) {
		return FALSE;
	}
	for ( i = 0; i < (INT)asz - 2; i++ ) out[i] = b[i + 1];
	out[i] = 0;
	return TRUE;
}

LOCAL INT atv_num( ID key, CONST char *path, INT dflt )
{
	UB	b[24];
	SZ	asz = 0;
	INT	i, v = 0;

	if ( ob_get_atv(key, (CONST UB *)path, b, sizeof(b), &asz) < E_OK || asz <= 0 || asz >= (SZ)sizeof(b) ) {
		return dflt;
	}
	for ( i = 0; i < (INT)asz && b[i] >= '0' && b[i] <= '9'; i++ ) v = v * 10 + ( b[i] - '0' );
	return ( i == (INT)asz ) ? v : dflt;
}

LOCAL void look_at( CONST TS_UUID *u, ENT *e )
{
	T_OBREF	r;
	UB	kind[24];
	ID	key;

	memset(e, 0, sizeof(*e));
	e->u = *u;
	e->label = "実身";
	e->execrec = 1;
	if ( ob_ref_obj(u, &r) < E_OK ) {
		e->label = "見えない実身";
		return;
	}
	e->type = r.type;
	ucopy(e->name, r.name, OB_NAME_MAX);
	e->nrec = r.nrec;
	e->size = r.size;
	if ( r.type != OB_T_STORAGE ) {
		e->label = ( r.type == OB_T_DEVICE ) ? "デバイス" : ( r.type == OB_T_PROCESS ) ? "プロセス"
			 : ( r.type == OB_T_CHANNEL ) ? "通信" : ( r.type == OB_T_WINDOW ) ? "ウインドウ" : "実身";
		return;
	}
	/* asked, not tried: an object the caller may not read is listed as such, not opened to be refused */
	e->noatr = (BOOL)( ob_acc_obj(u, OB_OP_ATRRD) < E_OK );
	e->nodata = (BOOL)( ob_acc_obj(u, OB_OP_READ) < E_OK );
	if ( e->noatr ) {
		return;
	}
	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key <= 0 ) {
		return;
	}
	if ( atv_has(key, "applist.virtual-object-list") ) {
		e->kind = K_BOX;
		e->label = "実身";
	}
	if ( atv_has(key, "tessronos.program") ) {
		e->kind = K_PROG;
		e->label = "プログラム";
		if ( atv_str(key, "tessronos.program.kind", kind, sizeof(kind)) && kind[0] != 0 ) {
			e->label = lt_same(kind, "command") ? "コマンド"
				 : lt_same(kind, "accessory") ? "小物"
				 : lt_same(kind, "app") ? "アプリ"
				 : lt_same(kind, "utility") ? "ツール" : "プログラム";
		}
	}
	if ( atv_has(key, "tessronos.exec") ) {
		e->exec = TRUE;
		e->execrec = atv_num(key, "tessronos.exec.record", 1);
		if ( e->kind != K_PROG ) {
			e->kind = K_PROG;
			e->label = "プログラム";
		}
	}
	ob_cls_obj(key);
}

/* Whether the executable a program names is there and is an ELF */
LOCAL BOOL has_elf( CONST ENT *e )
{
	UB	b[4];
	SZ	asz = 0;
	ID	key;
	BOOL	ok;

	if ( !e->exec ) return FALSE;
	key = ob_opn_obj(&e->u, OB_OP_READ);
	if ( key <= 0 ) return FALSE;
	ok = (BOOL)( ob_rea_rec(key, e->execrec, 0, b, 4, &asz) >= E_OK && asz == 4
		  && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F' );
	ob_cls_obj(key);
	return ok;
}

/* Whether an object may be gone into: a box, or anything that links to others */
LOCAL BOOL enterable( CONST ENT *e )
{
	INT	cnt = 0;

	if ( e->type != OB_T_STORAGE || e->nodata ) return FALSE;	/* its links are in a record */
	if ( e->kind == K_BOX ) return TRUE;
	if ( e->kind == K_PROG ) return FALSE;
	return (BOOL)( ob_lst_lnk(&e->u, NULL, NULL, 0, &cnt) >= E_OK && cnt > 0 );
}

/* The links of a box: how many were read into links[] */
LOCAL INT links_of( CONST TS_UUID *box )
{
	INT	cnt = 0;

	if ( ob_lst_lnk(box, NULL, links, LINKS_MAX, &cnt) < E_OK ) return 0;
	return ( cnt > LINKS_MAX ) ? LINKS_MAX : cnt;
}

/* The link of a box whose object has the name: TRUE and what it is */
LOCAL BOOL find_in( CONST TS_UUID *box, CONST UB *name, ENT *e, BOOL want_prog )
{
	INT	n = links_of(box), i;
	T_OBREF	r;
	BOOL	found = FALSE;

	for ( i = 0; i < n; i++ ) {
		if ( ob_ref_obj(&links[i], &r) < E_OK || !con_same_name(r.name, name) ) continue;
		look_at(&links[i], e);
		found = TRUE;
		if ( !want_prog || e->kind == K_PROG ) return TRUE;
	}
	return found;			/* none a program: e is the last that had the name */
}

/* ---------------------------------------------------------------- paths */

/* The next name of a path from *pp, the escapes taken off: its length, -1 at the end */
LOCAL INT next_part( CONST UB **pp, UB *out, INT max )
{
	CONST UB *p = *pp;
	INT	n = 0;

	if ( *p == 0 ) return -1;
	while ( *p != 0 && *p != '/' ) {
		if ( *p == '\\' && p[1] != 0 ) p++;
		if ( n < max - 1 ) out[n++] = *p;
		p++;
	}
	out[n] = 0;
	if ( *p == '/' ) p++;
	*pp = p;
	return n;
}

/*
 * A path followed from the place, into w[] and *pd as way[] and depth
 * are kept; e is what it ends at. FALSE when it cannot be followed, with
 * the reason said.
 */
LOCAL BOOL follow( CONST UB *path, TS_UUID *w, INT *pd, ENT *e )
{
	UB		part[OB_NAME_MAX * 2];
	CONST UB	*p = path;
	INT		d = depth, i, n;
	TS_UUID		u;

	for ( i = 0; i < depth; i++ ) w[i] = way[i];
	if ( *p == '/' ) {
		d = 1;
		while ( *p == '/' ) p++;
	}
	look_at(&w[d - 1], e);
	while ( ( n = next_part(&p, part, sizeof(part)) ) >= 0 ) {
		if ( n == 0 || lt_same(part, ".") ) continue;
		if ( lt_same(part, "..") && part[2] == 0 ) {
			if ( d > 1 ) d--;
			look_at(&w[d - 1], e);
			continue;
		}
		if ( !enterable(e) ) {
			say_u(e->name);
			say(": 仮身を持つ実身ではありません\n");
			return FALSE;
		}
		if ( is_uuid(part, &u) ) {
			look_at(&u, e);
			if ( e->type == 0 ) {
				say_u(part);
				say(": その実身はありません\n");
				return FALSE;
			}
		} else if ( !find_in(&w[d - 1], part, e, FALSE) ) {
			say_u(part);
			say(": 見つかりません\n");
			return FALSE;
		}
		if ( d >= DEPTH_MAX ) {
			say("深すぎます\n");
			return FALSE;
		}
		w[d++] = e->u;
	}
	*pd = d;
	return TRUE;
}

EXPORT INT box_quote( CONST UB *name, UB *out, INT max )
{
	INT	n = 0;

	for ( ; *name != 0 && n < max - 2; name++ ) {
		if ( *name == ' ' || *name == '/' || *name == '"' || *name == '\\' ) out[n++] = '\\';
		out[n++] = *name;
	}
	out[n] = 0;
	return n;
}

/* ---------------------------------------------------------------- the place */

LOCAL void set_here( void )
{
	ENT	e;

	look_at(&way[depth - 1], &e);
	shown_name(&e, here, sizeof(here));
}

EXPORT void box_init( void )
{
	(void)ob_fnd_sys(SYSROLE_ROOT, SYSDEF_ROOT, &sysbox);
	(void)lt_uuid(SYSDEF_BIN, &binobj);
	way[0] = sysbox;
	way[1] = binobj;
	depth = 2;
	set_here();
}

EXPORT CONST UB *box_here_name( void )
{
	return here;
}

/* The object the console is in */
EXPORT void box_here_uuid( TS_UUID *p_u )
{
	*p_u = way[( depth > 0 ) ? depth - 1 : 0];
}

EXPORT void box_where( UB *buf, INT max )
{
	UB	nm[OB_NAME_MAX + 16];
	INT	i, n = 0, k;
	ENT	e;

	buf[0] = 0;
	if ( depth <= 1 ) {
		ucopy(buf, (CONST UB *)"/", max);
		return;
	}
	for ( i = 1; i < depth; i++ ) {
		look_at(&way[i], &e);
		shown_name(&e, nm, OB_NAME_MAX);
		if ( n < max - 1 ) buf[n++] = '/';
		k = box_quote(nm, buf + n, max - n);
		n += k;
	}
	buf[n] = 0;
}

EXPORT void cmd_pwd( void )
{
	UB	p[DEPTH_MAX * ( OB_NAME_MAX + 2 )];

	box_where(p, sizeof(p));
	say_u(p);
	say("\n");
}

EXPORT void cmd_cd( INT argc, UB **argv )
{
	TS_UUID	w[DEPTH_MAX];
	INT	d, i;
	ENT	e;

	if ( argc < 2 ) {
		way[0] = sysbox;
		way[1] = binobj;
		depth = 2;
		set_here();
		return;
	}
	if ( !follow(argv[1], w, &d, &e) ) return;
	if ( !enterable(&e) ) {
		UB	nm[OB_NAME_MAX + 16];

		shown_name(&e, nm, sizeof(nm));
		say_u(nm);
		say(": 仮身を持つ実身ではありません(");
		say(e.label);
		say(")\n");
		return;
	}
	for ( i = 0; i < d; i++ ) way[i] = w[i];
	depth = d;
	set_here();
}

EXPORT void cmd_ls( INT argc, UB **argv )
{
	TS_UUID	w[DEPTH_MAX], box;
	UB	nm[OB_NAME_MAX + 16], id[TS_UUID_STRLEN + 1];
	BOOL	lng = FALSE;
	INT	i, n, d;
	ENT	e;
	CONST UB *path = NULL;

	for ( i = 1; i < argc; i++ ) {
		if ( lt_same(argv[i], "-l") ) lng = TRUE;
		else path = argv[i];
	}
	box = way[depth - 1];
	if ( path != NULL ) {
		if ( !follow(path, w, &d, &e) ) return;
		if ( !enterable(&e) ) {
			shown_name(&e, nm, sizeof(nm));
			say("  [");
			say(e.label);
			say("] ");
			say_u(nm);
			say("\n");
			return;
		}
		box = w[d - 1];
	}
	n = links_of(&box);
	if ( n == 0 ) {
		say("(仮身はありません)\n");
		return;
	}
	/* links[] is read again inside look_at's callers: keep a copy */
	{
		static TS_UUID	mine[LINKS_MAX];

		for ( i = 0; i < n; i++ ) mine[i] = links[i];
		for ( i = 0; i < n; i++ ) {
			look_at(&mine[i], &e);
			shown_name(&e, nm, sizeof(nm));
			say("  [");
			say(e.label);
			say("] ");
			say_u(nm);
			if ( enterable(&e) ) say("/");	/* it has links: cd goes into it */
			if ( e.noatr || e.nodata ) say("  (読めません)");
			if ( lng ) {
				(void)uuid_text(&e.u, id, TS_UUID_STRLEN);
				say("  ");
				say_u(id);
				say("  レコード ");
				say_num(e.nrec);
				say("  ");
				say_num((D)e.size);
				say(" バイト");
			}
			say("\n");
		}
	}
}

/* ---------------------------------------------------------------- programs */

EXPORT ER box_find_prog( CONST UB *name, TS_UUID *prog, UB *pname, INT pmax )
{
	TS_UUID	w[DEPTH_MAX];
	INT	d, i;
	ENT	e;
	BOOL	slash = FALSE, seen = FALSE;
	CONST TS_UUID *where[2];

	for ( i = 0; name[i] != 0; i++ ) {
		if ( name[i] == '\\' && name[i + 1] != 0 ) i++;
		else if ( name[i] == '/' ) slash = TRUE;
	}
	if ( slash ) {
		if ( !follow(name, w, &d, &e) ) return E_OBJ;
		seen = TRUE;
	} else {
		UB	plain[OB_NAME_MAX * 2];
		CONST UB *p = name;

		(void)next_part(&p, plain, sizeof(plain));
		where[0] = &way[depth - 1];
		where[1] = &binobj;
		for ( i = 0; i < 2; i++ ) {
			if ( i == 1 && same_uuid(where[0], where[1]) ) break;
			if ( find_in(where[i], plain, &e, TRUE) ) {
				seen = TRUE;
				if ( e.kind == K_PROG ) break;
			}
		}
		if ( !seen ) return E_NOEXS;
	}
	shown_name(&e, pname, pmax);
	if ( e.kind != K_PROG ) {
		say_u(pname);
		say(": プログラムではありません(");
		say(e.label);
		say(")\n");
		return E_OBJ;
	}
	if ( !has_elf(&e) ) {
		say_u(pname);
		say(": 実行形式を持たないプログラムです\n");
		return E_OBJ;
	}
	*prog = e.u;
	return E_OK;
}

EXPORT ER box_prog_named( CONST TS_UUID *u, UB *pname, INT pmax )
{
	ENT	e;

	look_at(u, &e);
	shown_name(&e, pname, pmax);
	return ( e.kind == K_PROG && has_elf(&e) ) ? E_OK : E_OBJ;
}

/* ---------------------------------------------------------------- completion */

LOCAL INT common_len( CONST UB *a, CONST UB *b )
{
	INT	n = 0, last = 0;

	while ( a[n] != 0 && a[n] == b[n] ) {
		n++;
		if ( ( a[n] & 0xC0 ) != 0x80 ) last = n;	/* only whole letters */
	}
	return last;
}

LOCAL BOOL starts( CONST UB *name, CONST UB *pre )
{
	UB	a[OB_NAME_MAX], b[OB_NAME_MAX * 2];
	INT	i;

	ucopy(a, name, sizeof(a));
	ucopy(b, pre, sizeof(b));
	con_fold(a);
	con_fold(b);
	for ( i = 0; b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return TRUE;
}

EXPORT INT box_complete( CONST UB *pre, BOOL cmd, UB *common, INT cmax, BOOL list )
{
	static UB	names[64][OB_NAME_MAX];
	static BOOL	boxes[64];
	UB		dir[CON_LINE_MAX], last[OB_NAME_MAX * 2], q[OB_NAME_MAX * 2];
	TS_UUID		w[DEPTH_MAX], from[2];
	INT		cut = -1, i, k, n, nb = 0, cnt = 0, d, c;
	CONST UB	*p;
	ENT		e;

	/* the part before the last unescaped '/' is a path to the box to look in */
	for ( i = 0; pre[i] != 0; i++ ) {
		if ( pre[i] == '\\' && pre[i + 1] != 0 ) i++;
		else if ( pre[i] == '/' ) cut = i;
	}
	from[0] = way[depth - 1];
	nb = 1;
	if ( cut >= 0 ) {
		for ( i = 0; i <= cut && i < (INT)sizeof(dir) - 1; i++ ) dir[i] = pre[i];
		dir[i] = 0;
		if ( !follow(dir, w, &d, &e) ) return 0;
		from[0] = w[d - 1];
		cmd = FALSE;
	} else {
		dir[0] = 0;
		if ( cmd && !same_uuid(&from[0], &binobj) ) from[nb++] = binobj;
	}
	p = pre + cut + 1;
	last[0] = 0;
	(void)next_part(&p, last, sizeof(last));

	for ( k = 0; k < nb; k++ ) {
		static TS_UUID	mine[LINKS_MAX];

		n = links_of(&from[k]);
		for ( i = 0; i < n; i++ ) mine[i] = links[i];
		for ( i = 0; i < n && cnt < 64; i++ ) {
			INT	j;
			BOOL	dup = FALSE;

			look_at(&mine[i], &e);
			if ( e.name[0] == 0 || !starts(e.name, last) ) continue;
			if ( cmd && k == 1 && e.kind != K_PROG ) continue;
			for ( j = 0; j < cnt; j++ ) {
				if ( con_same_name(names[j], e.name) ) dup = TRUE;
			}
			if ( dup ) continue;
			ucopy(names[cnt], e.name, OB_NAME_MAX);
			boxes[cnt] = enterable(&e);
			cnt++;
		}
	}
	if ( cnt == 0 ) return 0;

	c = ustrlen(names[0]);
	for ( i = 1; i < cnt; i++ ) {
		k = common_len(names[0], names[i]);
		if ( k < c ) c = k;
	}
	ucopy(common, dir, cmax);
	n = ustrlen(common);
	ucopy(q, names[0], ( c + 1 < (INT)sizeof(q) ) ? c + 1 : (INT)sizeof(q));
	n += box_quote(q, common + n, cmax - n);
	if ( cnt == 1 && n < cmax - 2 ) {
		common[n++] = boxes[0] ? '/' : ' ';
		common[n] = 0;
	}
	if ( list && cnt > 1 ) {
		for ( i = 0; i < cnt; i++ ) {
			say("  ");
			say_u(names[i]);
			if ( boxes[i] ) say("/");
		}
		say("\n");
	}
	return cnt;
}
